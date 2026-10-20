// Part of Player.cpp; included inside its anonymous namespace. Not standalone.

struct EngineSpec {
    bool svms = true;
    std::wstring apiPath;      // svms: explicit SVMSAPI.dll path ("" = search)
    std::wstring legacyPath;   // kdapi/winmm: synth DLL ("" = defaults)
    LegacySink::Kind legacyKind = LegacySink::kKindKdapi;
    std::wstring soundfont;    // svms only
};

// Defined below (Entry section); declared here for BringUpEngine.
std::string WideToUtf8(const std::wstring& wide);

bool BringUpEngine(PlayerCore& core, const EngineSpec& spec,
                   uint32_t lookaheadMs, std::string& error) {
    // The backend flag is only committed after the module is fully up —
    // a failed bring-up must leave the previous backend in place so the
    // UI state can never claim an engine that is not loaded.
    if (spec.svms) {
        if (!core.api.Load(spec.apiPath, error)) return false;
        if (core.api.fn.create_session(nullptr, &core.session) !=
                SVMS_RESULT_OK ||
            core.api.fn.set_ingress_mode(core.session,
                                         SVMS_INGRESS_LOSSLESS) !=
                SVMS_RESULT_OK) {
            error = "could not create an SVMS session";
            core.api.Unload();
            return false;
        }
        core.soundfontUtf8.clear();
        if (!spec.soundfont.empty()) {
            core.soundfontUtf8 = WideToUtf8(spec.soundfont);
            if (core.api.fn.load_soundfont_utf8(
                    core.session, core.soundfontUtf8.c_str()) !=
                SVMS_RESULT_OK) {
                std::fwprintf(stderr,
                              L"warning: soundfont load failed (%ls)\n",
                              spec.soundfont.c_str());
            }
        }
        // The decoder's frame domain is the engine's sample rate.
        uint64_t deviceNext = 0;
        if (core.api.fn.get_output_clock(core.session, &deviceNext,
                                         &core.sampleRate) !=
            SVMS_RESULT_OK) {
            error = "engine clock unavailable";
            core.api.fn.destroy_session(core.session);
            core.session = 0;
            core.api.Unload();
            return false;
        }
        core.backend = kBackendSvms;
    } else {
        if (!core.legacy.Load(spec.legacyKind, spec.legacyPath, error))
            return false;
        if (!core.legacy.Start(error)) {
            core.legacy.Unload();
            return false;
        }
        core.sampleRate = 48000;  // decoder timebase (wall-clock paced)
        core.backend = spec.legacyKind == LegacySink::kKindKdapi
                           ? kBackendKdapi
                           : kBackendWinmm;
        if (!spec.soundfont.empty()) {
            std::fwprintf(stderr,
                          L"note: --sf2 ignored with the legacy backend — "
                          L"the synth loads its own soundfonts\n");
        }
    }
    core.lookaheadFrames =
        static_cast<uint64_t>(lookaheadMs) * core.sampleRate / 1000u;
    core.delayFrames = core.sampleRate / 25u;  // 40 ms start offset
    return true;
}

std::thread g_submitThread;

void StartSubmitThread(PlayerCore* core) {
    core->quit.store(false, std::memory_order_release);
    g_submitThread = std::thread(core->backend == kBackendSvms
                                     ? SubmitThread
                                     : LegacySubmitThread,
                                 core);
}

// Stops everything, in the order the shutdown path proved out: quit flag →
// cancel (wakes a submit parked in a lossless wait; permanently fences the
// session, which is fine because it is about to be destroyed) → join →
// decoder join → drain → engine teardown.
void TearDownEngine(PlayerCore& core) {
    core.quit.store(true, std::memory_order_release);
    core.state.store(kStateIdle, std::memory_order_release);
    if (core.backend == kBackendSvms && core.session)
        core.api.fn.cancel_session_submissions(core.session);
    if (g_submitThread.joinable()) g_submitThread.join();
    StopDecoder();
    svms::PackedMidiEvent drainEvt{};
    while (core.ring && core.ring->Pop(drainEvt)) {
    }
    if (core.backend == kBackendSvms) {
        if (core.session) {
            core.api.fn.destroy_session(core.session);
            core.session = 0;
        }
        core.api.Unload();
    } else {
        core.legacy.Stop();
        core.legacy.Unload();
    }
}

// Runtime synth switch: swap ONLY the engine. The song stays loaded and
// playback resumes at the current position (channel replay state is kept so
// instruments come back correctly) — the MIDI file is never re-opened.
bool SwitchEngine(PlayerCore& core, const EngineSpec& spec,
                  uint32_t lookaheadMs, std::string& error) {
    uint64_t resumeFrame =
        core.totalFrames
            ? static_cast<uint64_t>(core.playhead.load(
                  std::memory_order_relaxed))
            : 0u;
    if (resumeFrame > core.totalFrames) resumeFrame = core.totalFrames;
    core.state.store(kStateSeeking, std::memory_order_release);
    TearDownEngine(core);
    core.lastSubmittedFrame.store(resumeFrame, std::memory_order_relaxed);
    core.cursorFrame.store(resumeFrame, std::memory_order_relaxed);
    core.playhead.store(static_cast<int64_t>(resumeFrame),
                        std::memory_order_relaxed);
    core.submittedEvents.store(0, std::memory_order_relaxed);
    core.submittedNoteOns.store(0, std::memory_order_relaxed);
    core.endOfSong.store(false, std::memory_order_release);
    core.decoderFailed.store(false, std::memory_order_relaxed);
    core.decoderError.clear();
    // Channel replay state is deliberately KEPT: resuming mid-song needs it
    // to restore programs/controllers at the new position.
    if (!BringUpEngine(core, spec, lookaheadMs, error)) {
        core.quit.store(false, std::memory_order_release);
        core.state.store(kStateIdle, std::memory_order_release);
        return false;
    }
    core.state.store(kStateIdle, std::memory_order_release);
    StartSubmitThread(&core);
    if (core.totalFrames > 0) {
        StartDecoder(&core, resumeFrame);
        StartPlayback(&core);
    }
    return true;
}

// Runtime song load: mirrors SeekTo's stop→drain→reset→restart chain, but
// swaps the file first. Call only from the main/UI thread. Refuses when no
// engine is up (engineless start) — calling fn.reset on a zeroed function
// table would jump to null.
bool LoadSong(PlayerCore& core, const std::wstring& path) {
    if (core.backend == kBackendSvms ? core.session == 0
                                     : !core.legacy.started)
        return false;
    core.state.store(kStateSeeking, std::memory_order_release);
    StopDecoder();
    svms::PackedMidiEvent drainEvt{};
    while (core.ring->Pop(drainEvt)) {
    }
    if (core.backend == kBackendSvms) core.api.fn.reset(core.session);
    else core.legacy.FullReset();
    core.filePath = path;
    core.cursorFrame.store(0, std::memory_order_relaxed);
    core.lastSubmittedFrame.store(0, std::memory_order_relaxed);
    core.playhead.store(0, std::memory_order_relaxed);
    core.submittedEvents.store(0, std::memory_order_relaxed);
    core.endOfSong.store(false, std::memory_order_release);
    core.decoderFailed.store(false, std::memory_order_relaxed);
    core.decoderError.clear();
    for (auto& channel : core.channels) channel = {};
    core.ClearNotes();
    StartDecoder(&core, 0);
    StartPlayback(&core);
    return true;
}

// ── Shared per-tick playback state (console and GUI loops) ─────────────
// One struct + three tick functions drive both front-ends, so the playback
// state machine (telemetry sampling, auto-quit, end-of-song, feed rate)
// cannot drift between them.

struct UiShared {
    float voiceHistory[128] = {};
    float renderHistory[128] = {};
    float npsHistory[128] = {};
    float polyHistory[128] = {};   // MIDI polyphony (player-side)
    uint32_t historyCount = 0;
    uint32_t historyIndex = 0;
    float currentNps = 0.0f;
    float currentMidiPoly = 0.0f;
    uint64_t lastNoteOnSnapshot = 0;   // 0.5 s feed-rate window
    LARGE_INTEGER lastTickQpc{};       // per-tick NPS (vsync cadence)
    uint64_t lastTickNoteOns = 0;
    uint64_t lastFeedSnapshot = 0;
    uint64_t feedRatePerSecond = 0;
    LARGE_INTEGER lastFeedQpc{};
    LARGE_INTEGER playingStart{};
    LARGE_INTEGER uiFrequency{};
    bool playingTimerStarted = false;
    float volume = 1.0f;
    bool running = true;
    bool songEnded = false;
    SVMS_TelemetryV1 telemetry{};
    uint64_t stressRng = GetTickCount64() | 1u;
    LARGE_INTEGER lastStressPatch{}, lastStressFont{};

    UiShared() { QueryPerformanceFrequency(&uiFrequency); }
};

const char* PlayerStateName(const PlayerCore& core, bool songEnded) {
    const char* stateName = "stopped";
    switch (core.state.load(std::memory_order_relaxed)) {
    case kStatePlaying: stateName = "PLAYING"; break;
    case kStatePausing: stateName = "pausing"; break;
    case kStatePaused:  stateName = "PAUSED"; break;
    case kStateSeeking: stateName = "seeking"; break;
    default: break;
    }
    if (songEnded &&
        core.state.load(std::memory_order_relaxed) == kStateIdle)
        stateName = "ENDED";
    return stateName;
}

void SampleTelemetryTick(PlayerCore& core, bool svmsBackend, UiShared& ui) {
    if (core.stressConfig && svmsBackend) {
        LARGE_INTEGER stressNow{};
        QueryPerformanceCounter(&stressNow);
        StressConfigTick(core, &ui.stressRng, stressNow, ui.uiFrequency,
                         &ui.lastStressPatch, &ui.lastStressFont);
    }
    const uint64_t tickPlayhead =
        core.playhead.load(std::memory_order_relaxed);
    ui.currentMidiPoly = static_cast<float>(core.MidiPolyphony(tickPlayhead));

    // Per-tick NPS: instant rate over the elapsed frame, EMA-smoothed so a
    // 60 Hz UI shows a live value instead of the old 0.5 s stair-step.
    LARGE_INTEGER tickNow{};
    QueryPerformanceCounter(&tickNow);
    const uint64_t tickNoteOns =
        core.submittedNoteOns.load(std::memory_order_relaxed);
    if (ui.lastTickQpc.QuadPart != 0) {
        const double dt = static_cast<double>(tickNow.QuadPart -
                                              ui.lastTickQpc.QuadPart) /
                          static_cast<double>(ui.uiFrequency.QuadPart);
        if (dt > 0.0) {
            const double instant =
                static_cast<double>(tickNoteOns - ui.lastTickNoteOns) / dt;
            ui.currentNps += static_cast<float>(
                (instant - static_cast<double>(ui.currentNps)) * 0.18);
        }
    }
    ui.lastTickQpc = tickNow;
    ui.lastTickNoteOns = tickNoteOns;

    if (svmsBackend) {
        ui.telemetry.struct_size = sizeof(ui.telemetry);
        ui.telemetry.struct_version = SVMS_STRUCT_VERSION_1;
        if (core.api.fn.get_telemetry(core.session, &ui.telemetry) ==
            SVMS_RESULT_OK) {
            ui.voiceHistory[ui.historyIndex] =
                static_cast<float>(ui.telemetry.active_voices);
            ui.renderHistory[ui.historyIndex] = ui.telemetry.render_time_ms;
            ui.npsHistory[ui.historyIndex] = ui.currentNps;
            ui.polyHistory[ui.historyIndex] = ui.currentMidiPoly;
            ui.historyIndex = (ui.historyIndex + 1u) % 128u;
            if (ui.historyCount < 128u) ++ui.historyCount;
        }
    } else {
        // Legacy synths expose (at most) a voice count; the render-time
        // graph is SVMS telemetry and stays flat here. NPS is counted
        // player-side, so it works on every backend.
        ui.telemetry.active_voices = core.legacy.VoiceCount();
        ui.telemetry.audio_running = 1u;
        ui.telemetry.sample_rate = core.sampleRate;
        ui.voiceHistory[ui.historyIndex] =
            static_cast<float>(ui.telemetry.active_voices);
        ui.renderHistory[ui.historyIndex] = 0.0f;
        ui.npsHistory[ui.historyIndex] = ui.currentNps;
        ui.polyHistory[ui.historyIndex] = ui.currentMidiPoly;
        ui.historyIndex = (ui.historyIndex + 1u) % 128u;
        if (ui.historyCount < 128u) ++ui.historyCount;
    }
}

void PlaybackEventsTick(PlayerCore& core, bool svmsBackend,
                        double autoSeconds, bool loop, bool quitAtEnd,
                        UiShared& ui) {
    if (!ui.playingTimerStarted && autoSeconds > 0.0) {
        ui.playingTimerStarted = true;
        QueryPerformanceCounter(&ui.playingStart);
    }
    if (ui.playingTimerStarted && autoSeconds > 0.0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (static_cast<double>(now.QuadPart - ui.playingStart.QuadPart) /
                static_cast<double>(ui.uiFrequency.QuadPart) >=
            autoSeconds)
            ui.running = false;
    }
    if (core.endOfSong.exchange(false, std::memory_order_acq_rel)) {
        if (loop) {
            SeekTo(&core, 0);
        } else {
            if (svmsBackend && core.session) core.api.fn.reset(core.session);
            else if (core.legacy.started) core.legacy.FullReset();
            core.state.store(kStateIdle, std::memory_order_release);
            ui.songEnded = true;
            if (quitAtEnd) ui.running = false;
        }
    }
    if (core.decoderFailed.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "\ndecoder error: %s\n",
                     core.decoderError.c_str());
        ui.running = false;
    }
    if (core.state.load(std::memory_order_relaxed) == kStatePlaying)
        ui.songEnded = false;
}

void FeedRateTick(PlayerCore& core, UiShared& ui) {
    const uint64_t submittedNow =
        core.submittedEvents.load(std::memory_order_relaxed);
    LARGE_INTEGER feedNow{};
    QueryPerformanceCounter(&feedNow);
    if (ui.lastFeedQpc.QuadPart != 0) {
        const double elapsed =
            static_cast<double>(feedNow.QuadPart -
                                ui.lastFeedQpc.QuadPart) /
            static_cast<double>(ui.uiFrequency.QuadPart);
        if (elapsed >= 0.5) {
            ui.feedRatePerSecond = static_cast<uint64_t>(
                static_cast<double>(submittedNow - ui.lastFeedSnapshot) /
                elapsed);
            ui.lastFeedSnapshot = submittedNow;
            ui.lastFeedQpc = feedNow;
        }
    } else {
        ui.lastFeedSnapshot = submittedNow;
        ui.lastFeedQpc = feedNow;
    }
}

// ── Entry ──────────────────────────────────────────────────────────────
