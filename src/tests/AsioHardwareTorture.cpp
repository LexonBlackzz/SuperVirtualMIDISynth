// ASIO torture run against a real driver, through AudioOutputASIO.
//
//   SVMS_ASIO_TEST_DRIVER   driver to torture (required)
//   SVMS_ASIO_TEST_DRIVER2  second driver for the switch scenario (optional)
//   SVMS_ASIO_TEST_FRAMES   buffer size to request (default 64)
//
// Output is silence. Every step that should produce audio must see callbacks
// within 3 s; a step blocked for more than 20 s ends the run as a hang. A busy
// or missing driver is a failure, not a skip: this run only exists to prove
// the driver works.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include "driver/AudioOutputASIO.h"

namespace {

std::atomic<unsigned long> g_calls{0};
std::atomic<unsigned long> g_lastFrames{0};
int g_fail = 0;
uint32_t g_frames = 64;

void Silence(float* out, uint32_t frames, void*) {
    g_calls.fetch_add(1);
    g_lastFrames = frames;
    std::memset(out, 0, static_cast<size_t>(frames) * 2u * sizeof(float));
}

bool Flowing(unsigned long ms = 3000) {
    const unsigned long before = g_calls.load();
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < ms) {
        if (g_calls.load() > before + 2) return true;
        Sleep(5);
    }
    return false;
}

std::wstring Wide(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// Runs f on a fresh thread (like an arbitrary player thread) with a watchdog.
template <typename F> void Step(const char* what, F f) {
    std::atomic<bool> done{false};
    bool ok = false;
    const ULONGLONG t0 = GetTickCount64();
    std::thread t([&] {
        ok = f();
        done = true;
    });
    while (!done && GetTickCount64() - t0 < 20000) Sleep(5);
    if (!done) {
        std::printf("HANG  %-46s (>20 s)\n", what);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    t.join();
    std::printf("%s  %-46s %5llu ms  frames=%lu\n", ok ? "ok  " : "FAIL", what,
                GetTickCount64() - t0, g_lastFrames.load());
    std::fflush(stdout);
    if (!ok) ++g_fail;
}

bool OpenStart(svms::AudioOutputASIO*& a, const char* driver) {
    a = new svms::AudioOutputASIO();
    a->SetRenderCallback(&Silence, nullptr);
    if (!a->Initialize(44100, g_frames, Wide(driver))) {
        std::printf("      init failed: %s\n", a->GetLastErrorText());
        return false;
    }
    if (!a->Start()) {
        std::printf("      start failed: %s\n", a->GetLastErrorText());
        return false;
    }
    return Flowing();
}

bool Close(svms::AudioOutputASIO*& a) {
    if (a) {
        a->Stop();
        a->Shutdown();
        delete a;
        a = nullptr;
    }
    return true;
}

} // namespace

int main() {
    const char* drv = std::getenv("SVMS_ASIO_TEST_DRIVER");
    if (!drv || !*drv) {
        std::puts("SVMS_ASIO_TEST_DRIVER is not set");
        return 1;
    }
    const char* drv2 = std::getenv("SVMS_ASIO_TEST_DRIVER2");
    if (!drv2 || !*drv2) drv2 = drv;
    if (const char* f = std::getenv("SVMS_ASIO_TEST_FRAMES")) g_frames = std::strtoul(f, nullptr, 10);
    svms::AudioOutputASIO* a = nullptr;

    std::printf("-- basic\n");
    Step("open+start", [&] { return OpenStart(a, drv); });
    Sleep(1000);
    Step("still flowing after 1 s", [&] { return Flowing(); });
    Step("close", [&] { return Close(a); });

    std::printf("-- reselect\n");
    for (int i = 0; i < 10; ++i) {
        char what[64];
        std::snprintf(what, sizeof(what), "reselect %d: close+open back to back", i);
        Step(what, [&] {
            Close(a);
            return OpenStart(a, drv);
        });
    }
    Step("close", [&] { return Close(a); });

    std::printf("-- threads\n");
    for (int i = 0; i < 5; ++i) {
        char what[64];
        std::snprintf(what, sizeof(what), "round %d: open on a new thread", i);
        Step(what, [&] { return OpenStart(a, drv); });
        std::snprintf(what, sizeof(what), "round %d: close on another thread", i);
        Step(what, [&] { return Close(a); });
    }

    std::printf("-- switch\n");
    const char* order[4] = {drv, drv2, drv, drv2};
    for (int i = 0; i < 4; ++i) {
        char what[96];
        std::snprintf(what, sizeof(what), "switch to %s", order[i]);
        Step(what, [&] {
            Close(a);
            return OpenStart(a, order[i]);
        });
    }
    Step("close", [&] { return Close(a); });

    std::printf("-- reopen after the linger window\n");
    Sleep(3500);
    Step("open+start after the driver unloaded", [&] { return OpenStart(a, drv); });

    std::printf("-- idle\n");
    const unsigned long frames0 = g_lastFrames.load();
    for (int s = 0; s < 15; ++s) {
        Sleep(1000);
        if (!Flowing(500)) {
            std::printf("FAIL  stalled at %d s\n", s);
            ++g_fail;
            break;
        }
    }
    const bool sameFrames = g_lastFrames.load() == frames0;
    std::printf("%s  idle 15 s, frames %lu -> %lu, callbacks %lu\n", sameFrames ? "ok  " : "FAIL",
                frames0, g_lastFrames.load(), g_calls.load());
    if (!sameFrames) ++g_fail;
    Step("close", [&] { return Close(a); });

    svms::AsioHost::Instance().ShutdownForProcessEnd();
    std::printf("== %s: %s (%d failed steps)\n", drv, g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
