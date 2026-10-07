// RuntimeLink V2 command handling, telemetry and census.

#include "SVMSDriverInternal.h"

namespace svms {

// ── RuntimeLink command handler (runs on control thread) ────────────────
// V2 command handling.  Live parameters arrive as ONE grouped
// ApplyLiveConfig command carrying the full RuntimeLiveStateV2 payload
// and a groupMask; the handler validates the payload, writes only the
// masked groups into the seqlock mailbox (odd sequence = in-progress,
// even = published; the audio thread reads it once per block — no locks,
// no torn reads). ReloadSoundFont only queues the dedicated loader thread;
// ResetVoices routes through the SPSC ingress exactly
// like midiOutReset so the audio thread performs the release work.

#if !defined(SVMS_XP_COMPAT)
static svms::RuntimeLiveStateV2 LiveStateFromMailbox(
    const NonAtomicLiveConfigMailbox& mb, uint32_t sampleRate) {
    svms::RuntimeLiveStateV2 l{};
    l.masterVolume = mb.masterVolume;
    l.correctnessMode = mb.correctnessMode ? 1u : 0u;
    l.reverbEnabled = mb.reverbEnabled ? 1u : 0u;
    l.reverbMix = mb.reverbMix;
    l.reverbRoomSize = mb.reverbRoomSize;
    l.reverbDecay = mb.reverbDecay;
    l.reverbDamping = mb.reverbDamping;
    l.reverbWidth = mb.reverbWidth;
    l.reverbDiffusion = mb.reverbDiffusion;
    l.reverbPreDelayMs = mb.reverbPreDelayMs;
    l.reverbEarlyLevel = mb.reverbEarlyLevel;
    l.reverbLateLevel = mb.reverbLateLevel;
    l.reverbModDepth = mb.reverbModDepth;
    l.reverbModRate = mb.reverbModRate;
    l.reverbLowCutHz = mb.reverbLowCutHz;
    l.reverbHighCutHz = mb.reverbHighCutHz;
    l.limiterEnabled = mb.limiterEnabled ? 1u : 0u;
    l.limiterAlgorithm = mb.limiterAlgorithm;
    l.limiterThreshold = mb.limiterThreshold;
    l.limiterLookaheadMs = static_cast<float>(mb.limiterDelayFrames)
                         / sampleRate * 1000.0f;
    const float attackCoeff = mb.limiterAttackCoeff;
    const float releaseCoeff = mb.limiterReleaseCoeff;
    l.limiterAttackMs = attackCoeff > 0.0f
        ? -1000.0f / (sampleRate * std::log(1.0f - attackCoeff))
        : 0.01f;
    l.limiterReleaseMs = releaseCoeff > 0.0f
        ? -1000.0f / (sampleRate * std::log(1.0f - releaseCoeff))
        : 100.0f;
    return l;
}

svms::RLResult Driver::HandleRuntimeLinkCommand(
    const svms::RuntimeLinkCommandV2& cmd, char* resultText) {
    using RT = svms::RLCommandType;
    constexpr uint32_t kText = svms::kRuntimeLinkResultTextCapacity;

    switch (static_cast<RT>(cmd.type)) {
    case RT::Ping:
        return svms::RLResult::Ok;

    case RT::ApplyLiveConfig: {
        if (cmd.groupMask == 0u) {
            strncpy_s(resultText, kText, "empty group mask", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        const svms::RuntimeLiveStateV2& l = cmd.live;
        // Reject non-finite payloads before touching the mailbox.
        if (!svms::RLV2_IsFinite(l.masterVolume) ||
            !svms::RLV2_IsFinite(l.reverbMix) ||
            !svms::RLV2_IsFinite(l.reverbRoomSize) ||
            !svms::RLV2_IsFinite(l.reverbDecay) ||
            !svms::RLV2_IsFinite(l.reverbDamping) ||
            !svms::RLV2_IsFinite(l.reverbWidth) ||
            !svms::RLV2_IsFinite(l.reverbDiffusion) ||
            !svms::RLV2_IsFinite(l.reverbPreDelayMs) ||
            !svms::RLV2_IsFinite(l.reverbEarlyLevel) ||
            !svms::RLV2_IsFinite(l.reverbLateLevel) ||
            !svms::RLV2_IsFinite(l.reverbModDepth) ||
            !svms::RLV2_IsFinite(l.reverbModRate) ||
            !svms::RLV2_IsFinite(l.reverbLowCutHz) ||
            !svms::RLV2_IsFinite(l.reverbHighCutHz) ||
            !svms::RLV2_IsFinite(l.limiterThreshold) ||
            !svms::RLV2_IsFinite(l.limiterLookaheadMs) ||
            !svms::RLV2_IsFinite(l.limiterAttackMs) ||
            !svms::RLV2_IsFinite(l.limiterReleaseMs)) {
            strncpy_s(resultText, kText, "non-finite parameter", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        if (l.correctnessMode > 1u || l.reverbEnabled > 1u ||
            l.limiterEnabled > 1u) {
            strncpy_s(resultText, kText, "boolean flags must be 0 or 1",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        if (l.limiterAlgorithm > 1u) {
            strncpy_s(resultText, kText,
                      "limiter algorithm must be 0 or 1", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }

        // Seqlock write: odd sequence marks the mailbox as in-progress
        // for the audio thread, even publishes the completed copy.
        LiveConfigMailbox* mb = &liveMailbox_;
        const uint32_t even = liveMailboxSeq_.load(std::memory_order_relaxed);
        liveMailboxSeq_.store(even | 1u, std::memory_order_relaxed);
        RLV2_MemBarrier();

        if (cmd.groupMask & svms::RLGroupMaster) {
            mb->masterVolume = (std::max)(0.0f, (std::min)(4.0f, l.masterVolume));
        }
        if (cmd.groupMask & svms::RLGroupCorrectness) {
            mb->correctnessMode = l.correctnessMode != 0u;
        }
        if (cmd.groupMask & svms::RLGroupReverb) {
            mb->reverbEnabled = l.reverbEnabled != 0u;
            mb->reverbMix = (std::max)(0.0f, (std::min)(1.0f, l.reverbMix));
            mb->reverbRoomSize = (std::max)(0.0f, (std::min)(1.0f, l.reverbRoomSize));
            mb->reverbDecay = (std::max)(0.0f, (std::min)(1.0f, l.reverbDecay));
            mb->reverbDamping = (std::max)(0.0f, (std::min)(1.0f, l.reverbDamping));
            mb->reverbWidth = (std::max)(0.0f, (std::min)(1.0f, l.reverbWidth));
            mb->reverbDiffusion = (std::max)(0.0f, (std::min)(1.0f, l.reverbDiffusion));
            mb->reverbPreDelayMs = (std::max)(0.0f, (std::min)(200.0f, l.reverbPreDelayMs));
            mb->reverbEarlyLevel = (std::max)(0.0f, (std::min)(1.5f, l.reverbEarlyLevel));
            mb->reverbLateLevel = (std::max)(0.0f, (std::min)(1.5f, l.reverbLateLevel));
            mb->reverbModDepth = (std::max)(0.0f, (std::min)(1.0f, l.reverbModDepth));
            mb->reverbModRate = (std::max)(0.0f, (std::min)(1.0f, l.reverbModRate));
            mb->reverbLowCutHz = (std::max)(0.0f, (std::min)(2000.0f, l.reverbLowCutHz));
            mb->reverbHighCutHz = (std::max)(1000.0f,
                (std::min)(static_cast<float>(sampleRate) * 0.45f, l.reverbHighCutHz));
        }
        if (cmd.groupMask & svms::RLGroupLimiter) {
            mb->limiterEnabled = l.limiterEnabled != 0u;
            mb->limiterAlgorithm = l.limiterAlgorithm;
            mb->limiterThreshold = (std::max)(0.1f, (std::min)(1.0f, l.limiterThreshold));
            uint32_t frames = static_cast<uint32_t>(
                (std::max)(0.0f, (std::min)(20.0f, l.limiterLookaheadMs))
                * sampleRate * 0.001f + 0.5f);
            mb->limiterDelayFrames =
                (std::min)(svms::LimiterState::kMaxDelayFrames, frames);
            float attackSamples = (std::max)(0.01f, (std::min)(100.0f, l.limiterAttackMs))
                                * sampleRate * 0.001f;
            attackSamples = (std::max)(1.0f, attackSamples);
            mb->limiterAttackCoeff = 1.0f - std::exp(-1.0f / attackSamples);
            float releaseSamples = (std::max)(1.0f, (std::min)(5000.0f, l.limiterReleaseMs))
                                 * sampleRate * 0.001f;
            releaseSamples = (std::max)(1.0f, releaseSamples);
            mb->limiterReleaseCoeff = 1.0f - std::exp(-1.0f / releaseSamples);
        }

        // Publish the completed copy (even sequence, release store).
        RLV2_MemBarrier();
        liveMailboxSeq_.store(even + 2u, std::memory_order_release);
        lastPublishedMailboxSeq_ = even + 2u;
        return svms::RLResult::Ok;
    }

    case RT::SetChannelLimiter: {
        // Payload: "enabled;threshold;releaseMs" in the command text area
        // (the documented extension area for new commands).  Values are
        // clamped, then written to the mailbox under RLGroupChannelLimiter
        // seqlock discipline; the audio thread adopts at the next block.
        const char* payload = cmd.resultText;
        if (!payload || !payload[0]) {
            strncpy_s(resultText, kText, "empty channel limiter payload",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        unsigned enabledValue = 0u;
        float thresholdValue = 0.0f;
        float releaseValue = 0.0f;
        if (sscanf_s(payload, "%u;%f;%f", &enabledValue, &thresholdValue,
                     &releaseValue) != 3) {
            strncpy_s(resultText, kText, "malformed channel limiter payload",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        if (thresholdValue <= 0.0f || !std::isfinite(thresholdValue) ||
            !std::isfinite(releaseValue)) {
            strncpy_s(resultText, kText, "non-finite parameter", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        thresholdValue = (std::max)(0.0316227766f,
                                    (std::min)(1.0f, thresholdValue));
        releaseValue = (std::max)(20.0f, (std::min)(1000.0f, releaseValue));

        LiveConfigMailbox* mb = &liveMailbox_;
        const uint32_t even = liveMailboxSeq_.load(std::memory_order_relaxed);
        liveMailboxSeq_.store(even | 1u, std::memory_order_relaxed);
        RLV2_MemBarrier();
        mb->channelLimiterEnabled = enabledValue != 0u ? 1u : 0u;
        mb->channelLimiterThreshold = thresholdValue;
        mb->channelLimiterReleaseMs = releaseValue;
        RLV2_MemBarrier();
        liveMailboxSeq_.store(even + 2u, std::memory_order_release);
        lastPublishedMailboxSeq_ = even + 2u;
        return svms::RLResult::Ok;
    }

    case RT::ReloadSoundFont: {
        // Queue a transactional immutable-bundle load. The loader thread does
        // all file, conversion, and preparation work; callback entry performs
        // only the completed bundle activation.
        std::wstring widePath;
        const size_t length = strnlen_s(
            cmd.resultText, svms::kRuntimeLinkCommandTextCapacity);
        if (length != 0u && length < svms::kRuntimeLinkCommandTextCapacity) {
            const int wideLength = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, cmd.resultText,
                static_cast<int>(length), nullptr, 0);
            if (wideLength <= 0) {
                strncpy_s(resultText, kText,
                          "SoundFont path is not valid UTF-8", _TRUNCATE);
                return svms::RLResult::InvalidArgument;
            }
            widePath.resize(static_cast<size_t>(wideLength));
            if (MultiByteToWideChar(
                    CP_UTF8, MB_ERR_INVALID_CHARS, cmd.resultText,
                    static_cast<int>(length), widePath.data(), wideLength) !=
                wideLength) {
                strncpy_s(resultText, kText, "could not decode SoundFont path",
                          _TRUNCATE);
                return svms::RLResult::InvalidArgument;
            }
        } else {
            std::string resolutionWarning;
            widePath = ResolveV3SoundFontPath(engineConfig_, &resolutionWarning);
            if (widePath.empty() && !resolutionWarning.empty())
                strncpy_s(resultText, kText, resolutionWarning.c_str(), _TRUNCATE);
        }
        if (widePath.empty()) {
            strncpy_s(resultText, kText, "no SoundFont configured", _TRUNCATE);
            return svms::RLResult::LoadFailed;
        }
        uint64_t requestId = 0u;
        if (!QueueSoundFontLoad(widePath, requestId)) {
            strncpy_s(resultText, kText, "SoundFont loader is unavailable",
                      _TRUNCATE);
            return svms::RLResult::LoadFailed;
        }
        snprintf(resultText, kText, "loading request %llu",
                 static_cast<unsigned long long>(requestId));
        return svms::RLResult::Ok;
    }

    case RT::QuerySoundFontLoad: {
        ReclaimRetiredSoundFonts();
        const uint32_t state = soundFontLoadState_.load(
            std::memory_order_acquire);
        const uint64_t requested = soundFontRequestId_.load(
            std::memory_order_acquire);
        const uint64_t activated = soundFontActivatedId_.load(
            std::memory_order_acquire);
        std::string error;
        EnterCriticalSection(&cs);
        error = soundFontLoadError_;
        LeaveCriticalSection(&cs);
        snprintf(resultText, kText, "%u\t%llu\t%llu\t%s", state,
                 static_cast<unsigned long long>(requested),
                 static_cast<unsigned long long>(activated), error.c_str());
        return svms::RLResult::Ok;
    }

    case RT::ResetVoices:
        // Routes through the SPSC ingress when audio is running, so the
        // release work happens on the audio thread exactly like
        // midiOutReset.
        ResetAllVoices();
        return svms::RLResult::Ok;

    case RT::SetPhaseRotation: {
        const uint32_t mode = cmd.param;
        if (mode > 4u) {
            strncpy_s(resultText, kText,
                      "phase rotation mode must be 0..4", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        LiveConfigMailbox* mb = &liveMailbox_;
        const uint32_t even = liveMailboxSeq_.load(std::memory_order_relaxed);
        liveMailboxSeq_.store(even | 1u, std::memory_order_relaxed);
        RLV2_MemBarrier();
        mb->phaseRotationMode.store(mode, std::memory_order_relaxed);
        RLV2_MemBarrier();
        liveMailboxSeq_.store(even + 2u, std::memory_order_release);
        lastPublishedMailboxSeq_ = even + 2u;
        strncpy_s(resultText, kText, "phase rotation mode set", _TRUNCATE);
        return svms::RLResult::Ok;
    }

    case RT::SetNoteOnCollapse: {
        // param = spawn interval; 0/1 disables coalescing (default state:
        // every note-on spawns at its exact QPC timestamp). The gate
        // rounds the value down to a power of two internally.
        const uint32_t threshold = cmd.param;
        if (threshold > 65536u) {
            strncpy_s(resultText, kText,
                      "note-on collapse threshold must be 0..65536",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        noteOnCollapse_.SetThreshold(threshold);
        if (threshold <= 1u) {
            strncpy_s(resultText, kText,
                      "note-on coalescing disabled (every note-on spawns)",
                      _TRUNCATE);
        } else {
            snprintf(resultText, kText,
                     "note-on coalescing enabled, 1 voice per %u hits",
                     noteOnCollapse_.Threshold());
        }
        return svms::RLResult::Ok;
    }

    case RT::SetVoiceRetireFloor: {
        // param = raw linear gain threshold bits (IEEE-754). Applies to
        // newly started releases; sounding tails finish at their start
        // floor, so live changes never truncate a fade mid-flight.
        float threshold = 0.0f;
        static_assert(sizeof(threshold) == sizeof(cmd.param),
                      "retire floor rides the param word");
        std::memcpy(&threshold, &cmd.param, sizeof(threshold));
        if (!std::isfinite(threshold) || threshold <= 0.0f ||
            threshold > 0.05f) {
            strncpy_s(resultText, kText,
                      "retire floor must be finite in (0, 0.05]",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        g_voiceRetireThreshold.store(threshold,
                                     std::memory_order_relaxed);
        engineConfig_.voiceRetireThreshold = threshold;
        snprintf(resultText, kText,
                 "voice retire floor set to %.6f (%.2f dB)",
                 threshold, 20.0 * std::log10(static_cast<double>(threshold)));
        return svms::RLResult::Ok;
    }
    case RT::SetStealPolicy: {
        // param = 0 (quality: incremental priority tree) or 1 (fast
        // cursor: O(1) round-robin victim, no index structures). Live
        // switches rebuild or drop the index as appropriate.
        if (cmd.param > 2u || !voiceManager) {
            strncpy_s(resultText, kText, "steal policy must be 0, 1 or 2",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        // Applied by the audio thread at the next block boundary — never
        // here, mid-block, where it would race a launch transaction.
        pendingStealPolicy_.store(cmd.param, std::memory_order_release);
        engineConfig_.stealPolicy = cmd.param;
        static const char* const policyNames[] = {
            "quality (priority tree)", "fast cursor", "scan" };
        snprintf(resultText, kText, "steal policy: %s (next block)",
                 policyNames[cmd.param]);
        return svms::RLResult::Ok;
    }
    case RT::SetPerKeyVoiceCap: {
        // param = max still-playing voices per (channel,note); 0 disables.
        // At the cap a note-on replaces the oldest key member (syndrv-style
        // per-key slots) instead of growing the pileup.
        if (cmd.param > 256u || !voiceManager) {
            strncpy_s(resultText, kText,
                      "per-key voice cap must be 0 (off) or 1..256",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        voiceManager->SetPerKeyVoiceCap(cmd.param);
        engineConfig_.perKeyVoiceCap = cmd.param;
        if (cmd.param != 0u) {
            snprintf(resultText, kText,
                     "per-key voice cap: %u voice%s per key",
                     cmd.param, cmd.param != 1u ? "s" : "");
        } else {
            strncpy_s(resultText, kText, "per-key voice cap: off",
                      _TRUNCATE);
        }
        return svms::RLResult::Ok;
    }
    case RT::SetThreadAffinityMode: {
        // param = 0 off, 1 all render threads on P-cores,
        // 2 RT on P-cores + workers on E-cores.
        // Re-applies to the live threads; threads created later pick the
        // mode up at birth. No-op on single-class CPUs and XP.
        if (cmd.param > 2u) {
            strncpy_s(resultText, kText,
                      "thread affinity mode must be 0, 1 or 2", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        svms::g_threadAffinityMode.store(cmd.param,
                                         std::memory_order_relaxed);
        engineConfig_.threadAffinityMode = cmd.param;
        if (useEventCompiler_ && eventCompilerThread_.joinable()) {
            svms::ApplyThreadAffinity(eventCompilerThread_.native_handle(),
                                      svms::AffinityRole::Compiler);
        }
        if (renderScalar) renderScalar->ApplyWorkerAffinity();
        if (audioOutput) audioOutput->ApplyThreadAffinity();
        static const char* const affinityText[] = {
            "thread affinity: off",
            "thread affinity: all render threads on P-cores",
            "thread affinity: RT on P-cores, workers on E-cores",
        };
        strncpy_s(resultText, kText, affinityText[cmd.param], _TRUNCATE);
        return svms::RLResult::Ok;
    }
    case RT::SetCcCollapse: {
        // param = opt-in compiler-side collapse of superseded
        // same-(channel,controller) state CCs. The compiler thread reads the
        // flag at page-fill time, so it applies to the next compiled page.
        if (cmd.param > 1u) {
            strncpy_s(resultText, kText, "cc collapse must be 0 or 1",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        ccCollapseEnabled_.store(cmd.param != 0u,
                                 std::memory_order_relaxed);
        engineConfig_.ccCollapse = cmd.param != 0u;
        strncpy_s(resultText, kText,
                  cmd.param ? "cc collapse: on (compiler-side)"
                            : "cc collapse: off",
                  _TRUNCATE);
        return svms::RLResult::Ok;
    }
    case RT::SetBlockTiming: {
        // param = opt-in block-granular dispatch. Events still admitted in
        // order; only their intra-block offset is relinquished (fires at
        // block start).
        if (cmd.param > 1u) {
            strncpy_s(resultText, kText, "block timing must be 0 or 1",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        blockTimingEnabled_.store(cmd.param != 0u,
                                  std::memory_order_relaxed);
        engineConfig_.blockTimingMode = cmd.param != 0u;
        strncpy_s(resultText, kText,
                  cmd.param ? "block timing: on (events fire at block start)"
                            : "block timing: off (exact-frame dispatch)",
                  _TRUNCATE);
        return svms::RLResult::Ok;
    }
    case RT::SetGhostBudget: {
        // param = max whole-voice ghosts captured per block; 0 = unbounded.
        if (cmd.param > 65536u || !renderScalar) {
            strncpy_s(resultText, kText,
                      "ghost budget must be 0 (unbounded) or 1..65536",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        renderScalar->SetGhostBudget(cmd.param);
        engineConfig_.ghostBudget = cmd.param;
        if (cmd.param != 0u) {
            snprintf(resultText, kText, "ghost budget: %u per block",
                     cmd.param);
        } else {
            strncpy_s(resultText, kText, "ghost budget: unbounded",
                      _TRUNCATE);
        }
        return svms::RLResult::Ok;
    }
    case RT::SetUnboundedRender: {
        // param = opt-in unbounded render: no wall-time recovery jump, no
        // per-block admission soft cap. The schedule renders in exact
        // order at whatever speed the engine manages.
        if (cmd.param > 1u) {
            strncpy_s(resultText, kText, "unbounded render must be 0 or 1",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        unboundedRenderEnabled_.store(cmd.param != 0u,
                                      std::memory_order_relaxed);
        engineConfig_.unboundedRender = cmd.param != 0u;
        strncpy_s(resultText, kText,
                  cmd.param ? "unbounded render: on (schedule over realtime)"
                            : "unbounded render: off",
                  _TRUNCATE);
        return svms::RLResult::Ok;
    }

    case RT::StartLiveRecording: {
        const size_t length = strnlen_s(
            cmd.resultText, svms::kRuntimeLinkCommandTextCapacity);
        if (length == 0u ||
            length >= svms::kRuntimeLinkCommandTextCapacity) {
            strncpy_s(resultText, kText, "missing or invalid WAV path",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        const int wideLength = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, cmd.resultText,
            static_cast<int>(length), nullptr, 0);
        if (wideLength <= 0) {
            strncpy_s(resultText, kText, "WAV path is not valid UTF-8",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        std::wstring path(static_cast<size_t>(wideLength), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                cmd.resultText, static_cast<int>(length),
                                path.data(), wideLength) != wideLength) {
            strncpy_s(resultText, kText, "could not decode WAV path",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        const auto status = liveRecorder_.GetStatus();
        if (status.state == svms::LiveWaveRecorder::State::Recording ||
            status.state == svms::LiveWaveRecorder::State::Starting ||
            status.state == svms::LiveWaveRecorder::State::Stopping) {
            strncpy_s(resultText, kText, "a live recording is already active",
                      _TRUNCATE);
            return svms::RLResult::Busy;
        }
        std::string error;
        if (!liveRecorder_.Start(path.c_str(), sampleRate, error)) {
            strncpy_s(resultText, kText, error.c_str(), _TRUNCATE);
            return svms::RLResult::LoadFailed;
        }
        strncpy_s(resultText, kText, "recording started", _TRUNCATE);
        return svms::RLResult::Ok;
    }

    case RT::StopLiveRecording:
        liveRecorder_.Stop();
        strncpy_s(resultText, kText, "recording stopped and WAV finalized",
                  _TRUNCATE);
        return svms::RLResult::Ok;

    case RT::QueryLiveRecording: {
        const auto status = liveRecorder_.GetStatus();
        std::snprintf(resultText, kText, "%u\t%u\t%llu\t%llu\t%u",
                      static_cast<unsigned>(status.state), status.sampleRate,
                      static_cast<unsigned long long>(status.framesWritten),
                      static_cast<unsigned long long>(status.droppedFrames),
                      status.errorCode);
        return svms::RLResult::Ok;
    }

    case RT::RequestRestart:
        strncpy_s(resultText, kText,
                  "restart is a manual operation: change restart-only "
                  "fields and restart the driver",
                  _TRUNCATE);
        return svms::RLResult::Unsupported;

    default:
        strncpy_s(resultText, kText, "unknown command type", _TRUNCATE);
        return svms::RLResult::InvalidArgument;
    }
}
#endif // !defined(SVMS_XP_COMPAT)

// ── RuntimeLink telemetry builder (runs on the control thread) ──────────
// Reads the process-local audio snapshot (written by the audio thread),
// the immutable engine parameters, and the applied-live echo, then hands
// the result to the driver for publication.  No audio-thread structures
// are touched here: every per-block counter travels through the snapshot.
#if !defined(SVMS_XP_COMPAT)
svms::RuntimeLinkTelemetryV2 Driver::BuildRuntimeLinkTelemetry() {
    // This control thread is the sole reclaimer. Draining before reading the
    // active pointer ensures a bundle retired concurrently after the load
    // remains alive until the next telemetry pass.
    ReclaimRetiredSoundFonts();
    // Wide→UTF-8 for the SoundFont name broadcast (local helper; the
    // SVMSConfig.cpp copy of WideToUtf8 is not exported via the header).
    auto wideToUtf8 = [](const std::wstring& value) -> std::string {
        if (value.empty()) return std::string();
        const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                              static_cast<int>(value.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (count <= 0) return std::string();
        std::string out(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(),
                            static_cast<int>(value.size()), out.data(), count,
                            nullptr, nullptr);
        return out;
    };

    svms::RuntimeLinkTelemetryV2 snap{};
    static svms::RuntimeLinkTelemetryV2 s_lastBuilt;  // last stable publish

    // Monotonic seqlock read: take the even sequence, copy the relaxed
    // payload, then confirm nothing moved (odd, or the sequence advanced
    // mid-copy = the frame may be torn).  Because the sequence only ever
    // grows by 2, an equality check is exact (no ABA).
    const svms::RuntimeAudioSnapshot& as = g_audioSnapshot;
    const uint32_t seq = as.sequence.load(std::memory_order_acquire);
    if ((seq & 1u) == 0u) {
        const uint32_t tick = as.tickMs.load(std::memory_order_relaxed);
        snap.activeVoices = as.activeVoices.load(std::memory_order_relaxed);
        snap.releasingVoices = as.releasingVoices.load(std::memory_order_relaxed);
        snap.freeTop = as.freeTop.load(std::memory_order_relaxed);
        snap.voiceSteals = as.voiceSteals.load(std::memory_order_relaxed);
        snap.retiredCount = as.retiredCount.load(std::memory_order_relaxed);
        snap.retiredImmediateCount =
            as.retiredImmediateCount.load(std::memory_order_relaxed);
        snap.decimationStep = as.decimationStep.load(std::memory_order_relaxed);
        snap.renderPeak = U32BitsToFloat(
            as.renderPeakBits.load(std::memory_order_relaxed));
        snap.audioRunning = as.audioRunning.load(std::memory_order_relaxed);
        snap.soundFontLoaded = as.soundFontLoaded.load(std::memory_order_relaxed);
        snap.audioHResult = as.audioHResult.load(std::memory_order_relaxed);
        snap.cpuLoadPercent = U32BitsToFloat(
            as.cpuLoadPercentBits.load(std::memory_order_relaxed));
        snap.callbackP95Percent = U32BitsToFloat(
            as.callbackP95PercentBits.load(std::memory_order_relaxed));
        snap.callbackP99Percent = U32BitsToFloat(
            as.callbackP99PercentBits.load(std::memory_order_relaxed));
        snap.callbackP999Percent = U32BitsToFloat(
            as.callbackP999PercentBits.load(std::memory_order_relaxed));
        snap.maxConsecutiveOverBudget =
            as.maxConsecutiveOverBudget.load(std::memory_order_relaxed);
        snap.overBudgetCallbacks =
            as.overBudgetCallbacks.load(std::memory_order_relaxed);
        snap.eventsSubmitted = as.eventsSubmitted.load(std::memory_order_relaxed);
        snap.eventsAccepted = as.eventsAccepted.load(std::memory_order_relaxed);
        snap.eventsDropped = as.eventsDropped.load(std::memory_order_relaxed);
        snap.eventsDispatched = as.eventsDispatched.load(std::memory_order_relaxed);
        snap.limiterInputPeakL = U32BitsToFloat(
            as.limiterInputPeakLBits.load(std::memory_order_relaxed));
        snap.limiterInputPeakR = U32BitsToFloat(
            as.limiterInputPeakRBits.load(std::memory_order_relaxed));
        snap.limiterOutputPeakL = U32BitsToFloat(
            as.limiterOutputPeakLBits.load(std::memory_order_relaxed));
        snap.limiterOutputPeakR = U32BitsToFloat(
            as.limiterOutputPeakRBits.load(std::memory_order_relaxed));
        snap.limiterGainReductionDb = U32BitsToFloat(
            as.limiterGainReductionDbBits.load(std::memory_order_relaxed));
        snap.channelLimiterEnabled =
            as.channelLimiterEnabled.load(std::memory_order_relaxed);
        for (uint32_t clChannel = 0u; clChannel < kRLV2ChannelCount;
             ++clChannel) {
            snap.channelLimiterGainReductionDb[clChannel] = U32BitsToFloat(
                as.channelLimiterGainReductionDbBits[clChannel].load(
                    std::memory_order_relaxed));
            snap.channelLimiterInputPeak[clChannel] = U32BitsToFloat(
                as.channelLimiterInputPeakBits[clChannel].load(
                    std::memory_order_relaxed));
        }
        snap.schedulerPercent = U32BitsToFloat(
            as.schedulerPercentBits.load(std::memory_order_relaxed));
        snap.eventDispatchPercent = U32BitsToFloat(
            as.eventDispatchPercentBits.load(std::memory_order_relaxed));
        snap.rawIngressCount =
            as.rawIngressCount.load(std::memory_order_relaxed);
        snap.compiledPagedCount =
            as.compiledPagedCount.load(std::memory_order_relaxed);
        snap.scheduledBacklogCount =
            as.scheduledBacklogCount.load(std::memory_order_relaxed);

        // Re-verify the settlement: a writer that started mid-copy means
        // this frame may be torn — reuse the last stable publish instead
        // (same skip-if-busy pattern as the client side).
        RLV2_MemBarrier();
        if (as.sequence.load(std::memory_order_acquire) != seq ||
            as.tickMs.load(std::memory_order_relaxed) != tick) {
            return s_lastBuilt;
        }
        s_lastBuilt = snap;
    } else {
        return s_lastBuilt;
    }

    snap.maxVoices = voiceManager ? voiceManager->GetMaxVoices() : 0u;
    snap.sampleRate = sampleRate;
    snap.bufferFrames = bufferFrames;

    // Live-state echo: what the audio thread last applied from the
    // mailbox (appliedMailbox_ + appliedSeq_ — release/acquire ordered),
    // forwarded to the client as the "applied" live config.
    const uint32_t appliedSeq = appliedSeq_.load(std::memory_order_acquire);
    if (appliedSeq != lastEchoedAppliedSeq_) {
        lastEchoedAppliedSeq_ = appliedSeq;
        appliedLiveEcho_ = LiveStateFromMailbox(appliedMailbox_, sampleRate);
    }
    snap.live = appliedLiveEcho_;

    const std::wstring activePath = CopyActiveSoundFontPath();
    if (!activePath.empty()) {
        std::string narrow = wideToUtf8(activePath);
        strncpy_s(snap.soundFontName, sizeof(snap.soundFontName),
                  narrow.c_str(), _TRUNCATE);
    }

    return snap;
}
#endif // !defined(SVMS_XP_COMPAT)

#if !defined(SVMS_XP_COMPAT)
svms::RLResult Driver::ExecuteRuntimeCommand(
    const svms::RuntimeLinkCommandV2& cmd, char* resultText) {
    return HandleRuntimeLinkCommand(cmd, resultText);
}
#endif

void Driver::CopyTelemetryCensus(SVMS_TelemetryV2* out) const {
    if (!out) return;
    DriverDebugInfo debug{};
    SnappyVoiceStatistics voices{};
    CopyDebugInfo(debug);
    CopyVoiceStatistics(voices);
    SVMS_TelemetryV2 r{};
    r.struct_size = sizeof(r);
    r.struct_version = SVMS_STRUCT_VERSION_1;
    r.callback_count = debug.callbackCount;
    r.submitted_events = debug.submitted;
    r.accepted_events = debug.accepted;
    r.dispatched_events = debug.dispatched;
    r.note_ons = debug.noteOns;
    r.matched_regions = debug.matchedRegions;
    r.configured_voices = debug.configuredVoices;
    r.voice_steals = voices.voiceSteals;
    r.active_voices = voices.activeVoices;
    r.free_voices = voices.freeVoices;
    r.sample_rate = sampleRate;
    r.buffer_frames = bufferFrames;
    r.soundfont_loaded = debug.soundFontLoaded;
    r.audio_running = debug.audioRunning;
    r.render_time_ms = GetRenderingTimeMilliseconds();
    r.render_peak = debug.renderPeak;
    const EventTelemetry& t = telemetry_;
    r.late_events = t.late;
    r.late_clamped_events = t.lateClamped;
    r.late_clamp_max_lateness_frames = t.lateClampMaxLateness;
    r.late_clamp_block_pileup_max = t.lateClampBlockPileupMax;
    r.stale_note_ons_skipped = t.staleNoteOnsSkipped;
    r.stale_note_offs_compacted = t.staleNoteOffsCompacted;
    r.fence_suppressed_note_ons = fenceSuppressedNoteOns_;
    r.cc_collapsed_events = ccCollapsedCount_;
    r.shed_note_ons = t.shedNoteOns;
    r.sequence_gaps = t.sequenceGaps;
    r.dropped_events = t.dropped;
    r.cancelled_submissions = t.cancelledSubmissions;
    r.scheduled_events = scheduledSizePublished_.load(std::memory_order_acquire);
    r.render_paths = renderScalar ? renderScalar->GetLastRenderPaths() : 0u;
    if (renderScalar)
        renderScalar->GetLastPlanRefusal(r.plan_refusal_type,
                                         r.plan_refusal_data1);
    r.backend_kind =
        static_cast<uint16_t>(externalBackendKind_.load(std::memory_order_relaxed));
    RenderScalar::GetPrimarySpanTotals(r.primary_span_calls,
                                       r.primary_span_frames);
    r.over_budget_callbacks = t.overBudgetCallbacks;
    r.max_consecutive_over_budget = t.maxConsecutiveOverBudget;
    r.callback_p95_percent = t.callbackP95Percent;
    r.callback_p99_percent = t.callbackP99Percent;
    r.callback_p999_percent = t.callbackP999Percent;
    *out = r;
}

} // namespace svms
