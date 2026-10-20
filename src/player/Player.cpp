// Player.cpp — console MIDI player over the native SVMS API.
//
// A dogfood client for the standalone SVMSAPI.dll: it integrates exactly the
// way an external player would (LoadLibrary + GetProcAddress of the single
// SVMS_GetInterface bootstrap) and exercises the API's differentiating paths:
//   - output-frame scheduling (send_timed_short_batch, OUTPUT_FRAME domain)
//   - lossless ingress        (bounded backpressure, never drops events)
//   - telemetry polling       (voices / render time ASCII graphs)
//   - live config patching    (+/- master volume via patch_config_json)
//
// Architecture (three threads, one SPSC ring, bounded memory):
//   decoder thread  MappedMidiFile + MidiStreamDecoder -> PackedMidiEvent
//                   stream pushed into the ring; events before the start
//                   frame are folded into per-channel replay state instead
//                   (program/bank/CC/pitch), so seeks replay correctly.
//   submit thread   ring -> send_timed_short_batch with OUTPUT_FRAME stamps
//                   anchored to get_output_clock; bounded lookahead.
//   main thread     console UI, keyboard, telemetry sampling.
//
// Memory: the file is mapped (zero copy), the ring is a fixed budget
// (default 64 MB) and the decoder blocks on Push when full — any file size
// plays with constant memory. Pause drains the lookahead before silencing
// (zero music loss); seek/stop fence the engine via reset() (instant).

#include <windows.h>
#include <mmsystem.h>

#include <dbghelp.h>

#include "events/MidiStream.h"
#include "include/svmsapi.h"
#include "player/PlayerScan.h"

#include <d3d11.h>
#include <commdlg.h>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

// Defined in imgui_impl_win32.cpp — must stay at global scope so the player's
// WndProc references the same symbol the backend defines.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

#include "player/parts/Platform.inl"
#include "player/parts/Core.inl"
#include "player/parts/Engine.inl"
#include "player/parts/Gui.inl"

} // namespace

int wmain(int argc, wchar_t** argv) {
    AddVectoredExceptionHandler(0, PlayerCrashFilter);

    std::wstring file;
    std::wstring soundfont;
    std::wstring apiPath;
    std::wstring backendName;
    std::wstring legacyPath;
    uint32_t ringMegabytes = 64u;
    uint32_t lookaheadMs = 150u;
    double autoSeconds = 0.0;
    bool loop = false;
    bool stressConfig = false;
    bool idleMode = false;
    bool quitAtEnd = false;
    bool guiRequested = false;
    bool scanRequested = false;
    std::wstring scanDir;
    std::wstring probePath;
    std::wstring switchPath;

    std::vector<std::wstring> positional;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring {
            return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring();
        };
        if (arg == L"--sf2" && i + 1 < argc) {
            soundfont = next();
        } else if (arg == L"--api" && i + 1 < argc) {
            apiPath = next();
        } else if (arg == L"--backend" && i + 1 < argc) {
            backendName = next();
        } else if (arg == L"--dll" && i + 1 < argc) {
            legacyPath = next();
        } else if (arg == L"--ring-mb" && i + 1 < argc) {
            ringMegabytes = static_cast<uint32_t>(wcstoul(next().c_str(),
                                                          nullptr, 10));
        } else if (arg == L"--lookahead-ms" && i + 1 < argc) {
            lookaheadMs = static_cast<uint32_t>(wcstoul(next().c_str(),
                                                        nullptr, 10));
        } else if (arg == L"--auto" && i + 1 < argc) {
            autoSeconds = wcstod(next().c_str(), nullptr);
        } else if (arg == L"--loop") {
            loop = true;
        } else if (arg == L"--stress-config") {
            stressConfig = true;
        } else if (arg == L"--idle") {
            idleMode = true;
        } else if (arg == L"--quit-at-end") {
            quitAtEnd = true;
        } else if (arg == L"--gui") {
            guiRequested = true;
        } else if (arg == L"--switch" && i + 1 < argc) {
            // Hidden dogfood path: run the GUI library Load click (full
            // SwitchEngine sequence, starting from the engineless state).
            switchPath = next();
        } else if (arg == L"--probe" && i + 1 < argc) {
            probePath = next();
        } else if (arg == L"--scan") {
            scanRequested = true;
            // Optional directory argument: consumed only if present and not
            // another flag.
            if (i + 1 < argc && argv[i + 1][0] != L'-') scanDir = next();
        } else if (arg == L"--help" || arg == L"-h") {
            PrintUsage();
            return 0;
        } else if (!arg.empty() && arg[0] == L'-') {
            std::fwprintf(stderr, L"unknown option: %ls\n", arg.c_str());
            PrintUsage();
            return 1;
        } else {
            positional.push_back(arg);
        }
    }
    if (!probePath.empty()) return RunProbeMode(probePath);
    if (scanRequested) return RunScanMode(scanDir.empty() ? ExeDirectory()
                                                          : scanDir);
    if (positional.empty()) {
        // Plain launch (or --gui with no file): open the window and pick a
        // song — and a synth — inside it.
        guiRequested = true;
        file.clear();
    } else {
        file = positional.front();
    }
    if (ringMegabytes < 1u) ringMegabytes = 1u;

    PlayerCore core;
    core.filePath = file;
    svms::ParsedEventRing ring(ringMegabytes);
    core.ring = &ring;

    if (!switchPath.empty()) {
        // Emulate the GUI library Load click: classify, build the spec from
        // the static classification, run the full SwitchEngine sequence
        // from the engineless state (no engine bring-up first).
        svmscan::Entry entry;
        entry.path = switchPath;
        const size_t slash = switchPath.find_last_of(L"\\/");
        entry.filename = (slash != std::wstring::npos)
                             ? switchPath.substr(slash + 1)
                             : switchPath;
        const svmscan::PeExports pe = svmscan::ReadExports(entry.path);
        entry.kind = svmscan::ClassifyKind(pe);
        EngineSpec spec;
        if (entry.kind == "svms") {
            spec.svms = true;
            spec.apiPath = entry.path;
        } else if (entry.kind == "kdapi") {
            spec.svms = false;
            spec.legacyKind = LegacySink::kKindKdapi;
            spec.legacyPath = entry.path;
        } else if (entry.kind == "winmm") {
            spec.svms = false;
            spec.legacyKind = LegacySink::kKindWinmm;
            spec.legacyPath = entry.path;
        } else {
            std::printf("switch: refusing \"%s\" (not a synth)\n",
                        entry.kind.c_str());
            return 1;
        }
        std::string switchError;
        const bool ok =
            SwitchEngine(core, spec, lookaheadMs, switchError);
        std::printf("switch(kind=%s): %s%s\n", entry.kind.c_str(),
                    ok ? "OK" : "FAILED",
                    switchError.empty() ? "" : (" — " + switchError).c_str());
        TearDownEngine(core);
        return ok ? 0 : 1;
    }

    if (backendName.empty() || backendName == L"svms") {
        core.backend = kBackendSvms;
    } else if (backendName == L"kdapi") {
        core.backend = kBackendKdapi;
    } else if (backendName == L"winmm") {
        core.backend = kBackendWinmm;
    } else {
        std::fwprintf(stderr, L"unknown backend: %ls\n", backendName.c_str());
        return 1;
    }
    const bool svmsBackend = core.backend == kBackendSvms;

    EngineSpec engine;
    engine.svms = svmsBackend;
    engine.apiPath = apiPath;
    engine.legacyPath = legacyPath;
    engine.legacyKind = core.backend == kBackendKdapi
                            ? LegacySink::kKindKdapi
                            : LegacySink::kKindWinmm;
    engine.soundfont = soundfont;

    core.stressConfig = stressConfig;
    std::string error;
    bool engineUp = true;
    if (!BringUpEngine(core, engine, lookaheadMs, error)) {
        if (!guiRequested) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        // Engineless GUI start is fine: the library panel brings a synth up.
        std::fprintf(stderr,
                     "note: no synth loaded (%s) — pick one in the "
                     "library panel\n",
                     error.c_str());
        engineUp = false;
    }

    // Scan first: validates the file and yields duration/peak statistics.
    // A fileless GUI launch skips this — the song is picked inside the window.
    // The mapping persists in g_songFile so synth switches never re-open it.
    svms::MidiStreamInfo info{};
    if (!file.empty()) {
        if (!g_songFile.Open(file.c_str(), error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            TearDownEngine(core);
            return 1;
        }
        svms::MidiStreamDecoder decoder;
        std::printf("scanning...\r");
        std::fflush(stdout);
        if (!decoder.Scan(g_songFile, core.sampleRate, info, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            TearDownEngine(core);
            return 1;
        }
        core.totalFrames = info.totalFrames;
    }

    // ── Launch threads and enter the console UI loop ────────────────────
    const bool fancy = EnableConsoleFancy();
    if (fancy) std::printf("\x1b[2J\x1b[?25l");

    char versionLabel[96];
    if (svmsBackend) {
        snprintf(versionLabel, sizeof(versionLabel), "%u.%u.%u (API %u)",
                 core.api.fn.product_major, core.api.fn.product_minor,
                 core.api.fn.product_patch, core.api.fn.abi_version);
    } else {
        char moduleAscii[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, core.legacy.moduleName, -1,
                            moduleAscii, sizeof(moduleAscii), nullptr,
                            nullptr);
        snprintf(versionLabel, sizeof(versionLabel), "%s backend — %.230s",
                 core.backend == kBackendKdapi ? "KDMAPI" : "WinMM",
                 moduleAscii);
    }
    std::printf("SVMS Player %s — %ls\n", versionLabel,
                file.empty() ? L"<no file — pick one in the window>"
                             : file.c_str());
    if (!file.empty()) {
    std::printf("events %llu  notes %llu  duration %.1f s  peak %llu evt/s  "
                "dup note-ons %llu\n\n",
                static_cast<unsigned long long>(info.eventCount),
                static_cast<unsigned long long>(info.noteOnCount),
                static_cast<double>(info.totalFrames) / core.sampleRate,
                static_cast<unsigned long long>(info.peakEventsPerSecond),
                static_cast<unsigned long long>(
                    info.exactDuplicateNoteOnCount));
    }

    if (engineUp && !file.empty()) StartDecoder(&core, 0);
    if (engineUp) StartSubmitThread(&core);
    if (engineUp && !idleMode && !file.empty()) StartPlayback(&core);

    UiShared ui;
    if (svmsBackend && core.session) ui.volume = ReadMasterVolume(core);
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);

    if (guiRequested) {
        RunGuiMode(core, engine, lookaheadMs, autoSeconds, loop, quitAtEnd,
                   ui);
    } else {
    while (ui.running && !core.quit.load(std::memory_order_relaxed)) {
        // ── Input (30 ms cadence drives the UI framerate too) ──────────
        DWORD wait = WaitForSingleObject(stdinHandle, 30u);
        while (wait == WAIT_OBJECT_0) {
            INPUT_RECORD record{};
            DWORD read = 0;
            if (!ReadConsoleInputW(stdinHandle, &record, 1u, &read) ||
                read == 0u)
                break;
            if (record.EventType != KEY_EVENT ||
                !record.Event.KeyEvent.bKeyDown)
                break;
            const WORD key = record.Event.KeyEvent.wVirtualKeyCode;
            const int64_t playhead =
                core.playhead.load(std::memory_order_relaxed);
            switch (key) {
            case VK_SPACE:
                if (core.state.load(std::memory_order_relaxed) ==
                    kStatePlaying) {
                    core.state.store(kStatePausing,
                                     std::memory_order_release);
                } else if (core.state.load(std::memory_order_relaxed) ==
                           kStatePaused) {
                    StartPlayback(&core);
                } else if (core.state.load(std::memory_order_relaxed) ==
                           kStateIdle) {
                    SeekTo(&core, 0);
                }
                break;
            case 'S':
                SeekTo(&core, 0);
                core.state.store(kStateIdle, std::memory_order_release);
                break;
            case VK_LEFT:
                SeekTo(&core,
                       static_cast<uint64_t>(
                           playhead - 5i64 * core.sampleRate));
                break;
            case VK_RIGHT:
                SeekTo(&core,
                       static_cast<uint64_t>(
                           playhead + 5i64 * core.sampleRate));
                break;
            case VK_UP:
                ui.volume += 0.1f;
                if (ui.volume > 1.0f) ui.volume = 1.0f;
                if (svmsBackend) PatchMasterVolume(core, ui.volume);
                else core.legacy.SendVolume(ui.volume);
                break;
            case VK_DOWN:
                ui.volume -= 0.1f;
                if (svmsBackend) PatchMasterVolume(core, ui.volume);
                else core.legacy.SendVolume(ui.volume);
                break;
            case 'L':
                loop = !loop;
                break;
            case 'Q':
            case VK_ESCAPE:
                ui.running = false;
                break;
            case VK_HOME:
                SeekTo(&core, 0);
                break;
            default:
                break;
            }
            wait = WaitForSingleObject(stdinHandle, 0u);
        }

        // ── Telemetry + auto-quit/end-of-song + feed rate (shared) ──────
        SampleTelemetryTick(core, svmsBackend, ui);
        PlaybackEventsTick(core, svmsBackend, autoSeconds, loop, quitAtEnd,
                           ui);
        FeedRateTick(core, ui);
        const int64_t playhead =
            core.playhead.load(std::memory_order_relaxed);

        char played[32], total[32];
        FormatTime(played, sizeof(played), playhead, core.sampleRate);
        FormatTime(total, sizeof(total),
                   static_cast<int64_t>(core.totalFrames),
                   core.sampleRate);

        const char* stateName = PlayerStateName(core, ui.songEnded);

        char voiceSpark[512], renderSpark[512], npsSpark[512], midiSpark[512];
        float voiceMax = 1.0f, renderMax = 1.0f, npsMax = 1.0f, polyMax = 1.0f;
        for (uint32_t i = 0; i < ui.historyCount; ++i) {
            if (ui.voiceHistory[i] > voiceMax) voiceMax = ui.voiceHistory[i];
            if (ui.renderHistory[i] > renderMax) renderMax = ui.renderHistory[i];
            if (ui.npsHistory[i] > npsMax) npsMax = ui.npsHistory[i];
            if (ui.polyHistory[i] > polyMax) polyMax = ui.polyHistory[i];
        }
        DrawSpark(voiceSpark, sizeof(voiceSpark), ui.voiceHistory,
                  ui.historyCount, 72u, voiceMax);
        DrawSpark(renderSpark, sizeof(renderSpark), ui.renderHistory,
                  ui.historyCount, 72u, renderMax);
        DrawSpark(npsSpark, sizeof(npsSpark), ui.npsHistory,
                  ui.historyCount, 72u, npsMax);
        DrawSpark(midiSpark, sizeof(midiSpark), ui.polyHistory,
                  ui.historyCount, 72u, polyMax);

        double progress =
            core.totalFrames
                ? static_cast<double>(playhead) /
                      static_cast<double>(core.totalFrames) * 100.0
                : 0.0;
        if (progress < 0.0) progress = 0.0;
        if (progress > 100.0) progress = 100.0;

        const uint64_t ringSize = core.ring->Size();
        const uint64_t ringCapacity = core.ring->Capacity();
        const uint32_t ringPercent = static_cast<uint32_t>(
            ringCapacity ? ringSize * 100u / ringCapacity : 0u);
        const float renderBudgetMs =
            (ui.telemetry.buffer_frames && ui.telemetry.sample_rate)
                ? static_cast<float>(ui.telemetry.buffer_frames) * 1000.0f /
                      static_cast<float>(ui.telemetry.sample_rate)
                : 0.0f;
        const float renderCpu =
            renderBudgetMs
                ? ui.telemetry.render_time_ms / renderBudgetMs * 100.0f
                : 0.0f;

        std::printf(
            "\x1b[H"
            "SVMS Player %s  [%s]  volume %.0f%%  %s\n"
            "--------------------------------------------------------------------------\n"
            "  %s / %s   (%.1f%%)   events %llu   feed %llu evt/s\n"
            "  midipoly %s  now %u  peak %.0f\n"
            "  voices   %s  now %u  peak %.0f\n"
            "  nps      %s  now %.0f\n"
            "  render   %s  now %.2f ms (%.0f%% of frame)  peak %.2f ms%s\n"
            "  ring %u%%  steals %llu  audio %s  sf2 %s  rate %u\n"
            "  [space] pause  [s] stop  [left/right] seek  [up/down] volume  [l]oop  [q]uit\n",
            versionLabel, stateName, ui.volume * 100.0f,
            loop ? "loop" : "",
            played, total, progress,
            static_cast<unsigned long long>(core.submittedEvents.load(
                std::memory_order_relaxed)),
            static_cast<unsigned long long>(ui.feedRatePerSecond),
            midiSpark, ui.currentMidiPoly, static_cast<double>(polyMax),
            voiceSpark,
            ui.telemetry.active_voices, static_cast<double>(voiceMax),
            npsSpark, static_cast<double>(ui.currentNps),
            renderSpark, ui.telemetry.render_time_ms, renderCpu,
            ui.telemetry.render_peak,
            svmsBackend ? "" : "   (SVMS telemetry only)",
            ringPercent,
            static_cast<unsigned long long>(ui.telemetry.voice_steals),
            ui.telemetry.audio_running ? "running" : "NOT RUNNING",
            ui.telemetry.soundfont_loaded ? "loaded" : "none",
            ui.telemetry.sample_rate);
    }
    }

    // ── Shutdown: every player thread must be out of the DLL before the
    // session is destroyed — a call in flight during destroy races the
    // session-slot teardown (observed as a 0xC0000005 at exit). A submit
    // parked in a lossless wait can only be woken by
    // cancel_session_submissions (the API's designed shutdown path);
    // set_ingress_mode alone never reaches an already-blocked producer.
    TearDownEngine(core);
    if (fancy) std::printf("\x1b[?25h\n");
    // core.ring points at the stack-local `ring` — it dies with the scope.
    core.ring = nullptr;
    return 0;
}

