// ASIO robustness stress test: streams audio through AudioOutputASIO and
// hammers the park → teardown → reload → restart path (the same one the
// watcher uses for driver-initiated resets / silent size drift) with a
// rotating set of buffer sizes. PASS = every reopen completes and the
// callback keeps flowing afterwards; FAIL = reopen error, callback stall,
// or hang (parent watchdog kills at 120 s and reports).
// Exit codes: 0 pass, 1 fail, 77 skip (no ASIO drivers installed).
#include <windows.h>
#include <objbase.h>   // DECLARE_INTERFACE for combase.h
#include <cstdio>
#include <cmath>
#include <atomic>
#include "combase.h"   // must precede iasiodrv.h (DECLARE_INTERFACE_ macros)
#include "asiolist.h"
#include "SVMSAudioOutputASIO.h"

static std::atomic<unsigned long> g_calls{0};

static void ToneCallback(float* out, uint32_t frames, void* user) {
    static double phase = 0.0;
    const double rate = *static_cast<double*>(user);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(0.2 * sin(phase));
        phase += 2.0 * 3.14159265358979 * 440.0 / rate;
        if (phase >= 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
        out[i * 2] = s;
        out[i * 2 + 1] = s;
    }
}

static bool WaitForCallbacks(unsigned long before, unsigned long timeoutMs) {
    const DWORD start = GetTickCount();
    while (static_cast<int>(GetTickCount() - start) < static_cast<int>(timeoutMs)) {
        if (g_calls.load(std::memory_order_relaxed) > before) return true;
        Sleep(10);
    }
    return false;
}

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;

    AsioDrivers drivers;
    ::asioDrivers = &drivers;
    char names[8][32] = {};
    char* ptrs[8];
    for (int i = 0; i < 8; ++i) ptrs[i] = names[i];
    const long found = drivers.getDriverNames(ptrs, 8);
    if (found <= 0) {
        std::puts("SKIP: no ASIO drivers installed");
        return 77;
    }
    long pick = -1;
    for (long i = 0; i < found && pick < 0; ++i)
        if (!filter || strstr(names[i], filter)) pick = i;
    if (pick < 0) {
        std::printf("SKIP: no driver matching '%s'\n", filter);
        return 77;
    }
    std::printf("stress driver: %s\n", names[pick]);

    double rate = 44100.0;
    svms::AudioOutputASIO asio;
    if (!asio.Initialize(44100, 512,
                         std::wstring(&names[pick][0], &names[pick][strlen(names[pick])]))) {
        // Hardware ASIO drivers are exclusive-access: a live host (DAW,
        // game, the running configurator) holding the driver makes init
        // fail. That is an environmental conflict, not a code failure —
        // skip so CI/runs with busy drivers don't read as regressions.
        std::printf("SKIP: driver '%s' busy or unavailable (init: %s)\n",
                    names[pick], asio.GetLastErrorText());
        return 77;
    }
    asio.SetRenderCallback(&ToneCallback, &rate);
    if (!asio.Start()) {
        std::printf("FAIL: Start\n");
        return 1;
    }
    if (!WaitForCallbacks(0, 5000)) {
        std::printf("FAIL: no callbacks after Start\n");
        return 1;
    }

    // Reopen gauntlet: each round reopens with a different size hint
    // (0 = driver's preferred, like the drift path) and requires the
    // callback stream to resume within 5 s afterwards.
    const uint32_t sizes[] = {0, 64, 1024, 0, 256, 2048, 0};
    int failures = 0;
    for (const uint32_t sz : sizes) {
        const unsigned long before = g_calls.load(std::memory_order_relaxed);
        const DWORD t0 = GetTickCount();
        if (!asio.TestReopenWithBufferSize(sz)) {
            std::printf("FAIL: reopen(size=%u) returned false\n", sz);
            ++failures;
            continue;
        }
        const DWORD reopenMs = GetTickCount() - t0;
        if (!WaitForCallbacks(before, 5000)) {
            std::printf("FAIL: callbacks stalled after reopen(size=%u)\n", sz);
            ++failures;
            continue;
        }
        std::printf("PASS: reopen(size=%u) in %lu ms, stream resumed "
                    "(buffer now %u)\n",
                    sz, reopenMs, asio.GetBufferFrames());
    }
    std::printf(failures ? "FAILED: %d reopen rounds\n" : "PASSED: all reopen rounds\n",
                failures);
    return failures ? 1 : 0;
}
