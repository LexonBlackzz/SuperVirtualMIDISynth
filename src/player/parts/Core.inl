// Part of Player.cpp; included inside its anonymous namespace. Not standalone.

struct ChannelReplay {
    bool programSeen = false;
    uint8_t program = 0;
    uint8_t cc[128] = {};
    bool ccSeen[128] = {};
    bool pitchSeen = false;
    uint8_t pitchLsb = 0;
    uint8_t pitchMsb = 0;
    bool pressureSeen = false;
    uint8_t pressure = 0;
};

struct PlayerCore {
    Api api;
    Backend backend = kBackendSvms;
    LegacySink legacy;
    // Legacy (kdapi/winmm) wall-clock scheduling anchor: file frame
    // `legacyAnchorFrame` maps to QPC tick `legacyAnchorQpc`.
    std::atomic<int64_t> legacyAnchorQpc{0};
    std::atomic<int64_t> legacyAnchorFrame{0};
    SVMS_Session session = 0;
    svms::ParsedEventRing* ring = nullptr;  // owned by main

    std::wstring filePath;
    uint32_t sampleRate = 48000;
    uint64_t totalFrames = 0;
    uint64_t lookaheadFrames = 0;
    uint64_t delayFrames = 0;

    std::atomic<uint32_t> state{kStateIdle};
    std::atomic<bool> quit{false};
    std::atomic<bool> reanchor{false};
    std::atomic<bool> endOfSong{false};
    std::atomic<uint64_t> cursorFrame{0};   // resume point (file frames)
    std::atomic<int64_t> playhead{0};       // file-space playhead (submit)
    std::atomic<uint64_t> anchorFrame{0};
    std::atomic<uint64_t> submittedEvents{0};
    // Player-side note-on count (both submit paths) — drives the NPS graph,
    // which needs no engine telemetry and works on every backend.
    std::atomic<uint64_t> submittedNoteOns{0};
    std::atomic<bool> decoderFailed{false};
    std::string decoderError;
    std::string soundfontUtf8;   // set when --sf2 was given (stress reloads)
    bool stressConfig = false;   // --stress-config: hammer live settings

    // Replay state accumulates across seeks (documented approximation).
    ChannelReplay channels[16];

    // Highest file frame handed to the engine (submit thread; main during
    // seeks while the submit thread is parked in a non-playing state).
    std::atomic<uint64_t> lastSubmittedFrame{0};

    // Held-note spans for the MIDI-polyphony graph: per (channel, key), the
    // frame the note was submitted at and the frame its note-off arrives.
    // Note-offs are submitted LOOKAHEAD-ahead of the playhead, so the off
    // frame must be kept: polyphony at the playhead is
    //     on <= playhead < off
    // (clearing eagerly undercounted every note ending inside the lookahead
    // window — long-sustained channels only, which looked like "channel 1").
    struct NoteSpan {
        uint64_t on;
        uint64_t off;
    };
    SRWLOCK noteLock = SRWLOCK_INIT;
    NoteSpan noteSpans[16u * 256u] = {};  // zero = silent (on=0)

    void ClearNotes() {
        AcquireSRWLockExclusive(&noteLock);
        memset(noteSpans, 0, sizeof(noteSpans));
        ReleaseSRWLockExclusive(&noteLock);
    }

    void NoteOn(uint32_t channel, uint32_t key, uint64_t frame) {
        AcquireSRWLockExclusive(&noteLock);
        NoteSpan& span = noteSpans[channel * 256u + key];
        span.on = frame;
        span.off = UINT64_MAX;  // until its note-off is submitted
        ReleaseSRWLockExclusive(&noteLock);
    }

    void NoteOff(uint32_t channel, uint32_t key, uint64_t frame) {
        AcquireSRWLockExclusive(&noteLock);
        NoteSpan& span = noteSpans[channel * 256u + key];
        if (span.on != 0u) span.off = frame;
        ReleaseSRWLockExclusive(&noteLock);
    }

    // Notes the MIDI holds down at `playhead`.
    uint32_t MidiPolyphony(uint64_t playhead) {
        AcquireSRWLockShared(&noteLock);
        uint32_t count = 0;
        for (const NoteSpan& span : noteSpans)
            if (span.on != 0u && span.on <= playhead && playhead < span.off)
                ++count;
        ReleaseSRWLockShared(&noteLock);
        return count;
    }

    void ObserveChannelState(const svms::PackedMidiEvent& event) {
        const uint32_t msg = event.message;
        ChannelReplay& channel = channels[msg & 0x0fu];
        switch (msg & 0xf0u) {
        case 0xC0u:
            channel.programSeen = true;
            channel.program = static_cast<uint8_t>((msg >> 8) & 0x7fu);
            break;
        case 0xB0u: {
            const uint32_t controller = (msg >> 8) & 0x7fu;
            channel.cc[controller] =
                static_cast<uint8_t>((msg >> 16) & 0x7fu);
            channel.ccSeen[controller] = true;
            break;
        }
        case 0xE0u:
            channel.pitchSeen = true;
            channel.pitchLsb = static_cast<uint8_t>((msg >> 8) & 0x7fu);
            channel.pitchMsb = static_cast<uint8_t>((msg >> 16) & 0x7fu);
            break;
        case 0xD0u:
            channel.pressureSeen = true;
            channel.pressure = static_cast<uint8_t>((msg >> 8) & 0x7fu);
            break;
        default:
            break;
        }
    }

    // Push the collected channel state as messages stamped at startFrame so
    // a seek lands with the right instruments, volumes and controllers.
    void EmitReplay(uint64_t startFrame, const std::atomic<bool>& cancel) {
        svms::PackedMidiEvent event{};
        event.outputFrame = startFrame;
        for (uint32_t ch = 0; ch < 16; ++ch) {
            ChannelReplay& channel = channels[ch];
            const uint32_t base = ch;
            if (channel.ccSeen[0]) {
                event.message = 0xB0u | base | (0u << 8) |
                                (uint32_t(channel.cc[0]) << 16);
                if (!ring->Push(event, cancel)) return;
            }
            if (channel.ccSeen[32]) {
                event.message = 0xB0u | base | (32u << 8) |
                                (uint32_t(channel.cc[32]) << 16);
                if (!ring->Push(event, cancel)) return;
            }
            if (channel.programSeen) {
                event.message =
                    0xC0u | base | (uint32_t(channel.program) << 8);
                if (!ring->Push(event, cancel)) return;
            }
            for (uint32_t controller = 1; controller < 128; ++controller) {
                if (controller == 32u || !channel.ccSeen[controller]) continue;
                event.message = 0xB0u | base | (controller << 8) |
                                (uint32_t(channel.cc[controller]) << 16);
                if (!ring->Push(event, cancel)) return;
            }
            if (channel.pitchSeen) {
                event.message = 0xE0u | base |
                                (uint32_t(channel.pitchLsb) << 8) |
                                (uint32_t(channel.pitchMsb) << 16);
                if (!ring->Push(event, cancel)) return;
            }
            if (channel.pressureSeen) {
                event.message = 0xD0u | base |
                                (uint32_t(channel.pressure) << 8);
                if (!ring->Push(event, cancel)) return;
            }
        }
    }
};

// ── Decoder thread ─────────────────────────────────────────────────────

struct DecodeArgs {
    PlayerCore* core = nullptr;
    svms::MappedMidiFile* file = nullptr;  // shared persistent mapping
    uint64_t startFrame = 0;
    bool replayEmitted = false;
    std::atomic<bool> cancel{false};
};

bool DecodeSink(const svms::PackedMidiEvent& event, void* user) {
    auto* args = static_cast<DecodeArgs*>(user);
    if (args->cancel.load(std::memory_order_relaxed)) return false;
    if (event.outputFrame < args->startFrame) {
        // Fold pre-seek events into the replay state instead of the ring.
        args->core->ObserveChannelState(event);
        return true;
    }
    if (!args->replayEmitted) {
        args->replayEmitted = true;
        args->core->EmitReplay(args->startFrame, args->cancel);
    }
    return args->core->ring->Push(event, args->cancel);
}

void DecoderThread(DecodeArgs* args) {
    PlayerCore& core = *args->core;
    std::string error;
    if (!args->file) {
        core.decoderError = "no song mapping";
        core.decoderFailed.store(true, std::memory_order_release);
        delete args;
        return;
    }
    svms::MidiStreamDecoder decoder;
    if (!decoder.Decode(*args->file, core.sampleRate, DecodeSink, args,
                        &args->cancel, nullptr, error)) {
        if (!args->cancel.load(std::memory_order_relaxed)) {
            core.decoderError = error;
            core.decoderFailed.store(true, std::memory_order_release);
        }
    }
    delete args;
}

// Player-side note-on classification for the NPS graph: note-on with a
// nonzero velocity (0x9n with velocity 0 is the note-off form).
inline bool IsNoteOn(uint32_t message) {
    return (message & 0xF0u) == 0x90u && ((message >> 16) & 0xFFu) != 0u;
}

// Track the event in the held-note span map (MIDI-polyphony graph).
inline void TrackNote(PlayerCore* core, uint32_t message, uint64_t frame) {
    const uint32_t channel = message & 0x0Fu;
    const uint32_t key = (message >> 8) & 0xFFu;
    switch (message & 0xF0u) {
    case 0x90u:
        if ((message >> 16) & 0xFFu) core->NoteOn(channel, key, frame);
        else core->NoteOff(channel, key, frame);  // velocity-0 note-off
        break;
    case 0x80u:
        core->NoteOff(channel, key, frame);
        break;
    default:
        break;
    }
}

// ── Submit thread: ring -> engine, output-frame stamped ───────────────

// Legacy (KDMAPI / WinMM) submission: immediate sends paced by a QPC
// wall-clock scheduler. The file-frame domain maps linearly onto QPC:
// due(event) = anchorQpc + (frame - anchorFrame) * freq / sampleRate.
// Pausing silences the synth immediately (no engine-side lookahead to
// drain) and re-anchors on resume; held notes are lost — the SVMS path
// is the exact-timing one, these backends measure the synth, not us.
void LegacySubmitThread(PlayerCore* core) {
    LegacySink& sink = core->legacy;
    svms::PackedMidiEvent event;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double freq = static_cast<double>(frequency.QuadPart);
    const double framesToQpc = freq / static_cast<double>(core->sampleRate);
    LARGE_INTEGER now{};

    while (!core->quit.load(std::memory_order_relaxed)) {
        const uint32_t state = core->state.load(std::memory_order_relaxed);
        if (state == kStatePausing) {
            sink.Silence();
            const int64_t playhead =
                core->playhead.load(std::memory_order_relaxed);
            core->cursorFrame.store(
                playhead > 0 ? static_cast<uint64_t>(playhead) : 0u,
                std::memory_order_relaxed);
            QueryPerformanceCounter(&now);
            core->legacyAnchorQpc.store(now.QuadPart,
                                        std::memory_order_relaxed);
            core->legacyAnchorFrame.store(
                playhead - static_cast<int64_t>(core->delayFrames),
                std::memory_order_relaxed);
            core->state.store(kStatePaused, std::memory_order_release);
            continue;
        }
        if (state != kStatePlaying) {
            Sleep(2u);
            continue;
        }
        if (core->reanchor.exchange(false, std::memory_order_acq_rel)) {
            QueryPerformanceCounter(&now);
            core->legacyAnchorQpc.store(now.QuadPart,
                                        std::memory_order_relaxed);
            core->legacyAnchorFrame.store(
                static_cast<int64_t>(core->cursorFrame.load(
                    std::memory_order_relaxed)) -
                    static_cast<int64_t>(core->delayFrames),
                std::memory_order_relaxed);
        }

        const int64_t anchorQpc = core->legacyAnchorQpc.load(
            std::memory_order_relaxed);
        const int64_t anchorFrame = core->legacyAnchorFrame.load(
            std::memory_order_relaxed);
        QueryPerformanceCounter(&now);
        const int64_t playhead = anchorFrame + static_cast<int64_t>(
            static_cast<double>(now.QuadPart - anchorQpc) / framesToQpc);
        core->playhead.store(playhead, std::memory_order_relaxed);

        if (core->ring->Size() == 0u && core->totalFrames != 0u &&
            playhead >= static_cast<int64_t>(core->totalFrames)) {
            core->endOfSong.store(true, std::memory_order_release);
            Sleep(2u);
            continue;
        }
        if (!core->ring->Peek(event)) {
            Sleep(1u);
            continue;
        }

        const int64_t dueQpc = anchorQpc + static_cast<int64_t>(
            static_cast<double>(static_cast<int64_t>(event.outputFrame) -
                                anchorFrame) * framesToQpc);
        int64_t delta = dueQpc - now.QuadPart;
        if (delta > static_cast<int64_t>(frequency.QuadPart / 500u)) {
            Sleep(1u);   // more than 2 ms away
            continue;
        }
        while (delta > 0) {
            YieldProcessor();
            QueryPerformanceCounter(&now);
            delta = dueQpc - now.QuadPart;
        }
        core->ring->Pop(event);
        sink.Send(event.message);
        core->submittedEvents.fetch_add(1u, std::memory_order_relaxed);
        TrackNote(core, event.message,
                  static_cast<uint64_t>(event.outputFrame));
        if (IsNoteOn(event.message))
            core->submittedNoteOns.fetch_add(1u, std::memory_order_relaxed);
    }
}

// (note-on classification helper IsNoteOn lives above LegacySubmitThread)

void SubmitThread(PlayerCore* core) {
    if (core->backend != kBackendSvms) {
        LegacySubmitThread(core);
        return;
    }
    std::vector<SVMS_TimedShortEvent> batch;
    batch.reserve(kSubmitBatchMax);
    svms::PackedMidiEvent event;

    while (!core->quit.load(std::memory_order_relaxed)) {
        const uint32_t state = core->state.load(std::memory_order_relaxed);

        if (state == kStatePausing) {
            // Let the device play out everything already submitted so no
            // music is lost, then silence the tails (CC120 also fences the
            // channel against anything still in flight).
            uint64_t deviceNext = 0;
            uint32_t rate32 = 0;
            const int64_t anchor =
                static_cast<int64_t>(
                    core->anchorFrame.load(std::memory_order_relaxed));
            if (core->api.fn.get_output_clock(core->session, &deviceNext,
                                              &rate32) == SVMS_RESULT_OK &&
                static_cast<int64_t>(deviceNext) - anchor >=
                    static_cast<int64_t>(core->lastSubmittedFrame)) {
                for (uint32_t ch = 0; ch < 16; ++ch) {
                    core->api.fn.send_short(core->session,
                                            0xB0u | ch | (120u << 8));
                }
                core->cursorFrame.store(core->lastSubmittedFrame + 1u,
                                        std::memory_order_relaxed);
                core->state.store(kStatePaused, std::memory_order_release);
            }
            Sleep(1u);
            continue;
        }
        if (state != kStatePlaying) {
            Sleep(2u);
            continue;
        }

        uint64_t deviceNext = 0;
        uint32_t rate32 = 0;
        if (core->api.fn.get_output_clock(core->session, &deviceNext,
                                          &rate32) != SVMS_RESULT_OK) {
            Sleep(5u);
            continue;
        }

        if (core->reanchor.exchange(false, std::memory_order_acq_rel)) {
            const uint64_t cursor =
                core->cursorFrame.load(std::memory_order_relaxed);
            core->anchorFrame.store(deviceNext + core->delayFrames - cursor,
                                    std::memory_order_relaxed);
        }
        const int64_t anchor = static_cast<int64_t>(
            core->anchorFrame.load(std::memory_order_relaxed));
        const int64_t playhead =
            static_cast<int64_t>(deviceNext) - anchor;
        core->playhead.store(playhead, std::memory_order_relaxed);
        const int64_t horizon = playhead +
            static_cast<int64_t>(core->lookaheadFrames);

        uint32_t count = 0;
        while (count < kSubmitBatchMax && core->ring->Peek(event) &&
               static_cast<int64_t>(event.outputFrame) <= horizon) {
            core->ring->Pop(event);
            TrackNote(core, event.message, event.outputFrame);
            SVMS_TimedShortEvent out{};
            out.timestamp = static_cast<uint64_t>(
                anchor + static_cast<int64_t>(event.outputFrame));
            out.timestamp_domain = SVMS_TIMESTAMP_OUTPUT_FRAME;
            out.packed_message = event.message;
            batch.push_back(out);
            ++count;
        }

        if (count == 0u) {
            if (core->ring->Size() == 0u &&
                core->totalFrames != 0u &&
                playhead >= static_cast<int64_t>(core->totalFrames)) {
                core->endOfSong.store(true, std::memory_order_release);
            }
            Sleep(2u);
            continue;
        }

        const SVMS_Result result = core->api.fn.send_timed_short_batch(
            core->session, batch.data(), count);
        core->submittedEvents.fetch_add(count, std::memory_order_relaxed);
        uint64_t noteOns = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (IsNoteOn(batch[i].packed_message)) ++noteOns;
        if (noteOns)
            core->submittedNoteOns.fetch_add(noteOns,
                                             std::memory_order_relaxed);
        core->lastSubmittedFrame = static_cast<uint64_t>(
            static_cast<int64_t>(batch[count - 1u].timestamp) - anchor);
        batch.clear();
        if (result != SVMS_RESULT_OK && result != SVMS_RESULT_CANCELLED)
            Sleep(2u);
    }
}

// ── Console / UI helpers ───────────────────────────────────────────────

bool EnableConsoleFancy() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) return false;
    return SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void FormatTime(char* buffer, size_t size, int64_t frames, uint32_t rate) {
    if (frames < 0) frames = 0;
    const double seconds = static_cast<double>(frames) /
                           static_cast<double>(rate ? rate : 1u);
    const int whole = static_cast<int>(seconds);
    snprintf(buffer, size, "%02d:%05.2f", whole / 60,
             seconds - 60.0 * (whole / 60));
}

// Draws the last `width` samples of `history` (ring buffer) as block glyphs.
void DrawSpark(char* out, size_t size, const float* history,
               uint32_t historyCount, uint32_t width, float scaleMax) {
    size_t used = 0;
    const uint32_t samples = historyCount < width ? historyCount : width;
    for (uint32_t i = 0; i < width; ++i) {
        float value = 0.0f;
        if (i >= width - samples) {
            const uint32_t index =
                (historyCount - (width - i)) % historyCount;
            value = history[index];
        }
        uint32_t band = 0;
        if (scaleMax > 0.0f) {
            band = static_cast<uint32_t>(value / scaleMax * 7.0f + 0.5f);
            if (band > 7u) band = 7u;
        }
        const int written = snprintf(out + used, size - used, "%s",
                                     kSparkBands[band]);
        if (written < 0 || used + static_cast<size_t>(written) >= size) break;
        used += static_cast<size_t>(written);
    }
}

// ── Live master volume via the config-JSON patch path ──────────────────

float ReadMasterVolume(PlayerCore& core) {
    std::vector<char> buffer(4096);
    uint32_t size = static_cast<uint32_t>(buffer.size());
    for (;;) {
        const SVMS_Result result = core.api.fn.get_config_json(
            core.session, buffer.data(), &size);
        if (result == SVMS_RESULT_OK) break;
        if (result != SVMS_RESULT_BUFFER_TOO_SMALL) return 1.0f;
        buffer.resize(size);
    }
    std::string document(buffer.data(), size);
    const size_t key = document.find("\"master_volume\"");
    if (key == std::string::npos) return 1.0f;
    const size_t colon = document.find(':', key);
    if (colon == std::string::npos) return 1.0f;
    return static_cast<float>(atof(document.c_str() + colon + 1));
}

bool PatchMasterVolume(PlayerCore& core, float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 2.0f) value = 2.0f;
    // The key lives at synth.master_volume in the config schema — a
    // top-level patch is silently ignored by the parser.
    char patch[80];
    snprintf(patch, sizeof(patch),
             "{\"synth\":{\"master_volume\":%.3f}}", value);
    return core.api.fn.patch_config_json(
               core.session, patch,
               static_cast<uint32_t>(strlen(patch))) == SVMS_RESULT_OK;
}

// ── --stress-config: inciter for live-adoption races ───────────────────
// Hammers the same paths the configurator uses while audio renders:
// voice-pool resizes, worker-thread resizes, buffer resizes, gain sweeps
// and soundfont reloads. The VEH crash logger in the DLL captures stacks.

uint32_t StressRandom(uint64_t* state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return static_cast<uint32_t>(*state >> 32);
}

void StressConfigTick(PlayerCore& core, uint64_t* rng,
                      LARGE_INTEGER now, LARGE_INTEGER frequency,
                      LARGE_INTEGER* lastPatch,
                      LARGE_INTEGER* lastFontReload) {
    const double patchInterval =
        0.08 + 0.0001 * static_cast<double>(StressRandom(rng) % 500);
    if (lastPatch->QuadPart != 0) {
        const double elapsed =
            static_cast<double>(now.QuadPart - lastPatch->QuadPart) /
            static_cast<double>(frequency.QuadPart);
        if (elapsed < patchInterval) {
            // Soundfont reloads on their own ~1.2 s cadence.
            if (core.soundfontUtf8.empty()) return;
            const double sinceReload =
                static_cast<double>(now.QuadPart - lastFontReload->QuadPart) /
                static_cast<double>(frequency.QuadPart);
            if (sinceReload >= 1.2) {
                *lastFontReload = now;
                core.api.fn.load_soundfont_utf8(
                    core.session, core.soundfontUtf8.c_str());
            }
            return;
        }
    }
    *lastPatch = now;

    static const uint32_t kVoicePools[] = {64u, 256u, 1024u, 2048u, 4096u};
    static const uint32_t kRenderThreads[] = {0u, 1u, 2u, 4u, 6u, 8u};
    static const uint32_t kBufferFrames[] = {256u, 512u, 1024u, 2048u};
    switch (StressRandom(rng) % 6u) {
    case 0: {
        char patch[96];
        snprintf(patch, sizeof(patch),
                 "{\"synth\":{\"max_voices\":%u}}",
                 kVoicePools[StressRandom(rng) % 5u]);
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    case 1: {
        char patch[96];
        snprintf(patch, sizeof(patch),
                 "{\"synth\":{\"render_threads\":%u}}",
                 kRenderThreads[StressRandom(rng) % 6u]);
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    case 2: {
        char patch[96];
        snprintf(patch, sizeof(patch),
                 "{\"audio\":{\"buffer_frames\":%u}}",
                 kBufferFrames[StressRandom(rng) % 4u]);
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    case 3: {
        char patch[96];
        snprintf(patch, sizeof(patch),
                 "{\"synth\":{\"master_volume\":%.2f,"
                 "\"velocity_curve\":%.2f}}",
                 0.1f + 0.001f *
                     static_cast<float>(StressRandom(rng) % 1200u),
                 0.5f + 0.01f *
                     static_cast<float>(StressRandom(rng) % 300u));
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    case 4:
        if (!core.soundfontUtf8.empty()) {
            core.api.fn.load_soundfont_utf8(core.session,
                                            core.soundfontUtf8.c_str());
            *lastFontReload = now;
        }
        break;
    default: {
        char patch[128];
        snprintf(patch, sizeof(patch),
                 "{\"synth\":{\"max_voices\":%u,"
                 "\"render_threads\":%u}}",
                 kVoicePools[StressRandom(rng) % 5u],
                 kRenderThreads[StressRandom(rng) % 6u]);
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    }
}

// ── Transport control (main thread) ────────────────────────────────────

DecodeArgs* g_decoderArgs = nullptr;
std::thread g_decoderThread;

// The song's file mapping persists across synth switches — switching only
// swaps the engine and resumes decoding from the current position, it never
// re-opens (or re-scans) the MIDI file.
svms::MappedMidiFile g_songFile;

void StopDecoder() {
    if (g_decoderArgs) g_decoderArgs->cancel.store(true);
    if (g_decoderThread.joinable()) g_decoderThread.join();
    g_decoderArgs = nullptr;  // DecoderThread owns and deletes its args
}

void StartDecoder(PlayerCore* core, uint64_t startFrame) {
    StopDecoder();
    g_decoderArgs = new DecodeArgs{core, &g_songFile, startFrame};
    g_decoderThread = std::thread(DecoderThread, g_decoderArgs);
}

void StartPlayback(PlayerCore* core) {
    core->reanchor.store(true, std::memory_order_release);
    core->state.store(kStatePlaying, std::memory_order_release);
}

// Seeks are instant: reset() publishes a global termination fence in the
// engine (pending and scheduled events are rejected at dispatch), the ring
// is drained, and the decoder restarts from the target with channel replay.
void SeekTo(PlayerCore* core, uint64_t target) {
    const uint64_t limit =
        core->totalFrames ? core->totalFrames - 1u : 0u;
    if (target > limit) target = limit;
    core->state.store(kStateSeeking, std::memory_order_release);
    StopDecoder();
    core->ClearNotes();  // held notes re-register from the replay stream
    svms::PackedMidiEvent drain{};
    while (core->ring->Pop(drain)) {
    }
    if (core->backend == kBackendSvms) core->api.fn.reset(core->session);
    else core->legacy.FullReset();
    core->cursorFrame.store(target, std::memory_order_relaxed);
    core->lastSubmittedFrame.store(target, std::memory_order_relaxed);
    core->endOfSong.store(false, std::memory_order_release);
    StartDecoder(core, target);
    StartPlayback(core);
}

// ── Engine management (startup, runtime synth switch, song load) ────────
// The GUI can load a different synth DLL (or a different song) at runtime.
// BringUpEngine/TearDownEngine mirror the proven startup/shutdown sequence
// in wmain; SwitchEngine runs the full stop → load → restart chain on the
// main thread while the submit thread is parked in a non-playing state.
