// End-to-end check of send_short_batch_direct against send_short_batch on the
// real driver (audio thread included). Same dense, deterministic stream is
// paced in like a player would, once through each entry point; the engine's own
// census must agree and the submit-side cost is reported.
//
//   direct_e2e.exe <path to SVMSAPI.dll> [seconds=2] [notes_per_ms=300]
//
// Run it with SVMS_TEST_CONFIG_PATH pointing at a config whose master_volume is
// 0 to keep it silent.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "include/svmsapi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <random>
#include <vector>

static uint64_t Qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return (uint64_t)v.QuadPart; }
static double CpuSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto t = [](FILETIME f) { return ((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return t(k) + t(u);
}

struct Run { double submitSec; double cpuSec; SVMS_TelemetryV2 d; uint64_t events; SVMS_Result firstResult; };

static SVMS_Interface g_api;
static SVMS_Session g_session;
static uint64_t g_freq;

static SVMS_TelemetryV2 Telemetry() {
    SVMS_TelemetryV2 t{};
    t.struct_size = sizeof(t);
    t.struct_version = 1;
    g_api.get_telemetry_v2(g_session, &t);
    return t;
}

static Run Play(bool direct, double seconds, uint32_t notesPerMs, uint32_t seed) {
    std::mt19937 rng(seed);
    const uint32_t ms = (uint32_t)(seconds * 1000.0);
    std::vector<std::vector<SVMS_ShortEvent>> bucket(ms + 40);
    for (uint32_t t = 0; t < ms; ++t) {
        for (uint32_t i = 0; i < notesPerMs; ++i) {
            const uint32_t ch = rng() % 16, key = 20 + rng() % 100, vel = 60 + rng() % 68;
            const uint32_t on = (0x90u | ch) | key << 8 | vel << 16, off = (0x80u | ch) | key << 8;
            bucket[t].push_back({0, on, 0});
            bucket[t + 20 + rng() % 15].push_back({0, off, 0});
        }
        // A few controllers and bends per millisecond, to exercise collapse.
        for (int i = 0; i < 3; ++i) {
            static const uint32_t ccs[] = {7, 10, 11, 74, 91, 1};
            const uint32_t ch = rng() % 16;
            bucket[t].push_back({0, (0xB0u | ch) | ccs[rng() % 6] << 8 | (rng() % 128) << 16, 0});
        }
        bucket[t].push_back({0, (0xE0u | rng() % 16) | (rng() % 128) << 8 | (rng() % 128) << 16, 0});
    }
    const SVMS_TelemetryV2 before = Telemetry();
    const uint64_t start = Qpc() + g_freq / 5;
    for (uint32_t t = 0; t < ms + 40; ++t)
        for (auto& e : bucket[t]) e.timestamp_qpc = start + (uint64_t)t * g_freq / 1000;

    Run run{};
    std::vector<SVMS_ShortEvent> chunk;
    const double cpu0 = CpuSeconds();
    uint32_t t = 0;
    while (t < ms + 40) {
        // Feed everything due within the next 20 ms, as a player does.
        const uint64_t horizon = Qpc() + g_freq / 50;
        chunk.clear();
        while (t < ms + 40 && start + (uint64_t)t * g_freq / 1000 < horizon) {
            chunk.insert(chunk.end(), bucket[t].begin(), bucket[t].end());
            ++t;
        }
        if (!chunk.empty()) {
            const uint64_t a = Qpc();
            SVMS_Result r = direct ? g_api.send_short_batch_direct(g_session, chunk.data(), (uint32_t)chunk.size())
                                   : g_api.send_short_batch(g_session, chunk.data(), (uint32_t)chunk.size());
            run.submitSec += (double)(Qpc() - a) / g_freq;
            if (run.events == 0) run.firstResult = r;
            run.events += chunk.size();
        }
        Sleep(1);
    }
    Sleep(600);
    run.cpuSec = CpuSeconds() - cpu0;
    const SVMS_TelemetryV2 after = Telemetry();
    run.d = after;
    // Report deltas for the counters that matter.
    run.d.submitted_events -= before.submitted_events;
    run.d.accepted_events -= before.accepted_events;
    run.d.dispatched_events -= before.dispatched_events;
    run.d.note_ons -= before.note_ons;
    run.d.late_events -= before.late_events;
    run.d.late_clamped_events -= before.late_clamped_events;
    run.d.shed_note_ons -= before.shed_note_ons;
    run.d.dropped_events -= before.dropped_events;
    run.d.fence_suppressed_note_ons -= before.fence_suppressed_note_ons;
    run.d.cc_collapsed_events -= before.cc_collapsed_events;
    run.d.note_merged_events_low -= before.note_merged_events_low;
    return run;
}

static void Print(const char* name, const Run& r) {
    std::printf("%-7s sent %llu (first call result %d)  submit %.1f ns/event  process CPU %.2f s\n"
                "        submitted %llu accepted %llu dispatched %llu note_ons %llu late %llu clamped %llu shed %llu dropped %llu fenced %llu ccCollapsed %llu merged %u\n",
                name, (unsigned long long)r.events, (int)r.firstResult, r.submitSec / (double)r.events * 1e9, r.cpuSec,
                (unsigned long long)r.d.submitted_events, (unsigned long long)r.d.accepted_events,
                (unsigned long long)r.d.dispatched_events, (unsigned long long)r.d.note_ons,
                (unsigned long long)r.d.late_events, (unsigned long long)r.d.late_clamped_events,
                (unsigned long long)r.d.shed_note_ons, (unsigned long long)r.d.dropped_events,
                (unsigned long long)r.d.fence_suppressed_note_ons,
                (unsigned long long)r.d.cc_collapsed_events, r.d.note_merged_events_low);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: direct_e2e <SVMSAPI.dll> [seconds] [notes_per_ms]\n"); return 2; }
    const double seconds = argc > 2 ? atof(argv[2]) : 2.0;
    const uint32_t perMs = argc > 3 ? (uint32_t)atoi(argv[3]) : 300;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_freq = (uint64_t)f.QuadPart;

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { std::printf("cannot load %s\n", argv[1]); return 2; }
    auto getIf = (SVMS_Result(SVMS_CALL*)(uint32_t, uint32_t, SVMS_Interface*))GetProcAddress(dll, "SVMS_GetInterface");
    auto init = (BOOL(WINAPI*)())GetProcAddress(dll, "InitializeKDMAPIStream");
    auto term = (BOOL(WINAPI*)())GetProcAddress(dll, "TerminateKDMAPIStream");
    if (!getIf || !init || !term) { std::printf("missing exports\n"); return 2; }
    if (getIf(SVMS_ABI_VERSION_1, sizeof(g_api), &g_api) != SVMS_RESULT_OK ||
        !(g_api.capabilities & SVMS_CAP_DIRECT_PAGES) || !g_api.send_short_batch_direct) {
        std::printf("this build has no direct pages\n"); return 2;
    }
    if (g_api.create_session(nullptr, &g_session) != SVMS_RESULT_OK) { std::printf("no session\n"); return 2; }
    if (!init()) { std::printf("InitializeKDMAPIStream failed\n"); return 2; }
    for (int i = 0; i < 100 && !Telemetry().audio_running; ++i) Sleep(50);
    std::printf("audio running: %u, %u Hz, %u frames\n", Telemetry().audio_running, Telemetry().sample_rate, Telemetry().buffer_frames);

    Play(false, 0.5, 50, 1); // warm up both entry points' pages
    Play(true, 0.5, 50, 1);
    int status = 0;
    Run ring[2], dir[2];
    for (int i = 0; i < 2; ++i) {
        ring[i] = Play(false, seconds, perMs, 7);
        dir[i] = Play(true, seconds, perMs, 7);
    }
    for (int i = 0; i < 2; ++i) { Print("ring", ring[i]); Print("direct", dir[i]); }
    for (int i = 0; i < 2; ++i) {
        auto accounted = [](const Run& r) {
            return r.d.dispatched_events + r.d.cc_collapsed_events + r.d.note_merged_events_low;
        };
        std::printf("accounted ring %llu direct %llu of %llu sent\n",
                    (unsigned long long)accounted(ring[i]), (unsigned long long)accounted(dir[i]),
                    (unsigned long long)dir[i].events);
        // Merge and collapse windows follow page boundaries, so only the total is exact.
        const bool same = accounted(ring[i]) == accounted(dir[i]) &&
                          dir[i].d.dropped_events == 0 && dir[i].d.shed_note_ons == 0 &&
                          dir[i].firstResult == SVMS_RESULT_OK;
        std::printf("round %d: %s\n", i, same ? "dispatch counts match" : "MISMATCH");
        if (!same) status = 1;
    }
    term();
    return status;
}
