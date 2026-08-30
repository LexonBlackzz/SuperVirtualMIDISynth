#ifndef SVMS_RENDER_SCALAR_H
#define SVMS_RENDER_SCALAR_H

#include "SVMSTypes.h"
#include "SVMSVoiceManager.h"
#include "SVMSChannelCache.h"
#include "SVMSEnvelope.h"
#include "SVMSPageAllocator.h"
#include "SVMSRenderKernels.h"
#include "SVMSRenderWorkers.h"
#include <algorithm>
#include <cstdlib>
#include <malloc.h>
#include "SVMSSoundFont.h"

namespace svms {

// ── Linear interpolation between two sample frames ──────────────────────
inline float InterpolateSample(const int16_t* data, uint32_t baseIndex,
                                uint32_t nextIndex, float frac) {
    const float s0 = static_cast<float>(data[baseIndex]) * (1.0f / 32768.0f);
    const float s1 = static_cast<float>(data[nextIndex]) * (1.0f / 32768.0f);
    return s0 + (s1 - s0) * frac;
}

// Render a compact continuation from the independent BASS-like tail reserve.
// It follows the old sample cursor and loop for a fixed 64-frame linear ramp,
// has no MIDI identity, and does not consume a primary voice slot.
inline void RenderStealTailSample(VoiceSoA& v, uint32_t idx,
                                  const int16_t* sampleData,
                                  uint32_t sampleDataFrames,
                                  float* outL, float* outR) {
    uint32_t remaining = v.stealTailFramesRemaining[idx];
    if (remaining == 0 || v.stealTailSampleBacked[idx] == 0 || !sampleData)
        return;

    const uint32_t relEnd = v.stealTailRelEnd[idx];
    if (relEnd < 2u) {
        v.stealTailFramesRemaining[idx] = 0;
        return;
    }

    float phase = v.stealTailPhase[idx];
    if (phase < 0.0f) phase = 0.0f;
    uint32_t baseOffset = static_cast<uint32_t>(phase);
    const bool loop = v.stealTailLoopEnabled[idx] != 0;

    if (baseOffset + 1u >= relEnd) {
        if (!loop) {
            v.stealTailFramesRemaining[idx] = 0;
            return;
        }
        phase = v.stealTailRelLoopSF[idx];
        baseOffset = v.stealTailRelLoopS[idx];
    }

    uint32_t nextRel = baseOffset + 1u;
    if (loop && nextRel >= v.stealTailRelLoopE[idx])
        nextRel = v.stealTailRelLoopS[idx];
    if (nextRel >= relEnd) nextRel = relEnd - 1u;

    const uint32_t sampleStart = v.stealTailSampleStart[idx];
    const uint32_t baseIndex = sampleStart + baseOffset;
    const uint32_t nextIndex = sampleStart + nextRel;
    if (baseIndex >= sampleDataFrames || nextIndex >= sampleDataFrames) {
        v.stealTailFramesRemaining[idx] = 0;
        return;
    }

    const float frac = phase - static_cast<float>(baseOffset);
    float sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
    if (v.rot) sample = RotateVoiceSample(v.stealTailRot[idx], sample);
    const uint32_t total = v.stealTailFramesTotal[idx];
    const float fade = total > 1u
        ? static_cast<float>(remaining - 1u) / static_cast<float>(total - 1u)
        : 0.0f;
    const float scaled = sample * v.stealTailGain[idx] * fade;
    *outL += scaled * v.stealTailMixGainL[idx];
    *outR += scaled * v.stealTailMixGainR[idx];

    phase += v.stealTailPhaseInc[idx];
    const float loopEnd = v.stealTailRelLoopEF[idx];
    if (loop && phase >= loopEnd) {
        float overflow = phase - loopEnd;
        const float loopStart = v.stealTailRelLoopSF[idx];
        const float loopLength = loopEnd - loopStart;
        if (loopLength > 0.0f && overflow >= loopLength)
            overflow -= floorf(overflow / loopLength) * loopLength;
        phase = loopStart + overflow;
    }

    v.stealTailPhase[idx] = phase;
    v.stealTailFramesRemaining[idx] = remaining - 1u;
}

inline void AdvanceStealTailSpanExact(VoiceSoA& v, uint32_t idx,
                                      uint32_t frameCount) {
    uint32_t remaining = v.stealTailFramesRemaining[idx];
    if (remaining == 0u || v.stealTailSampleBacked[idx] == 0u) return;
    float phase = (std::max)(0.0f, v.stealTailPhase[idx]);
    const float phaseStep = v.stealTailPhaseInc[idx];
    const uint32_t relEnd = v.stealTailRelEnd[idx];
    const bool loop = v.stealTailLoopEnabled[idx] != 0u;
    for (uint32_t frame = 0u; frame < frameCount && remaining != 0u; ++frame) {
        uint32_t base = static_cast<uint32_t>(phase);
        if (relEnd < 2u || base + 1u >= relEnd) {
            if (!loop) {
                remaining = 0u;
                break;
            }
            phase = v.stealTailRelLoopSF[idx];
        }
        phase += phaseStep;
        const float loopEnd = v.stealTailRelLoopEF[idx];
        if (loop && phase >= loopEnd) {
            float overflow = phase - loopEnd;
            const float loopStart = v.stealTailRelLoopSF[idx];
            const float loopLength = loopEnd - loopStart;
            if (loopLength > 0.0f && overflow >= loopLength)
                overflow -= floorf(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        --remaining;
    }
    v.stealTailPhase[idx] = phase;
    v.stealTailFramesRemaining[idx] = remaining;
}

// ── Vibrato LFO (SF2 default modulators) ────────────────────────────────
// While any channel has an active modulation depth (mod wheel CC1 plus
// channel pressure), render spans are subdivided to this length so each
// voice's triangle LFO advances and its phase increment is refreshed at a
// fixed deterministic cadence. When no channel is modulated the pass is
// skipped entirely and output remains byte-identical to the unmodulated
// engine.
constexpr uint32_t kVibratoUpdateFrames = 64u;

inline void AdvanceVibratoSpan(VoiceManager& voices,
                               const ChannelParamsSnapshot* chParams,
                               uint32_t frameCount) {
    if (frameCount == 0u || !chParams) return;
    VoiceSoA& v = voices.v;
    for (uint32_t position = 0; position < voices.activeCount_; ++position) {
        const uint32_t idx = voices.activeList_[position];
        if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) continue;
        const float step = v.vibLfoSteps[idx];
        if (step == 0.0f) continue;
        const float depthCents = v.vibLfoToPitchCents[idx];
        uint32_t frames = frameCount;
        const uint32_t delay = v.vibLfoDelays[idx];
        if (delay > 0u) {
            if (delay >= frames) {
                // The LFO holds its zero start value through the delay.
                v.vibLfoDelays[idx] = delay - frames;
                continue;
            }
            frames -= delay;
            v.vibLfoDelays[idx] = 0u;
        }
        float phase = v.vibLfoPhases[idx] +
            step * static_cast<float>(frames);
        phase -= floorf(phase);
        v.vibLfoPhases[idx] = phase;

        const ChannelParamsSnapshot& cp = chParams[v.channel[idx]];
        const float scale = v.pitchBendScales[idx];
        // scale == 1 uses the driver-maintained exact common ratio so an
        // active vibrato can never drop SysEx master tune/transpose. The
        // rare scaled-bend branch recomputes from channel cents without
        // master offsets (documented limitation).
        const float bendRatio = scale == 1.0f && cp.bendRatio > 0.0f
            ? cp.bendRatio
            : powf(2.0f, cp.pitchBendCents * scale / 1200.0f);
        const float depth = cp.modDepth;
        if (!(depth > 0.0f) || !(depthCents > 0.0f)) {
            if (v.vibLfoModulated[idx]) {
                // Modulation just ended: restore the exact unmodulated
                // increment once instead of rewriting it every span.
                v.vibLfoModulated[idx] = 0u;
                v.phaseIncs[idx] = v.basePhaseIncs[idx] * bendRatio;
            }
            continue;
        }
        v.vibLfoModulated[idx] = 1u;
        // SF2 LFOs are positive-going triangles starting at zero.
        const float t = phase * 4.0f;
        const float tri = t < 1.0f ? t : (t < 3.0f ? 2.0f - t : t - 4.0f);
        const float cents = depthCents * depth * tri;
        const float lfoRatio = powf(2.0f, cents / 1200.0f);
        v.phaseIncs[idx] = v.basePhaseIncs[idx] * bendRatio * lfoRatio;
    }
}

// ── Loop eligibility check ──────────────────────────────────────────────
inline bool ShouldLoopSVMS(uint8_t loopMode, uint32_t loopStart, uint32_t loopEnd,
                            uint8_t state) {
    // SF2 sampleModes: 0=no loop, 1=continuous, 2=reserved,
    // 3=loop while the key is held and continue past the loop on release.
    if (loopMode == 0 || loopMode == 2) return false;
    if (state == static_cast<uint8_t>(VoiceState::Releasing) && loopMode == 3)
        return false;
    return loopEnd > loopStart + 1u;
}

// ════════════════════════════════════════════════════════════════════════
// RenderEvent — sub-sample-precise MIDI event descriptor.
// ════════════════════════════════════════════════════════════════════════
enum class RenderEventType : uint8_t {
    NoteOn       = 0,
    NoteOff      = 1,
    ControlChange= 2,
    ProgramChange= 3,
    PitchBend    = 4,
    AllNotesOff  = 5,
    AllSoundOff  = 6,
    Reset         = 7,
    // Internal overload-recovery command. data2 carries a count from 1..255.
    // It is never created for events that still have a writable exact frame.
    StaleNoteOffBatch = 8,
    MasterVolume = 9,
    RhythmPart = 10,
    MasterFineTune = 11,
    MasterTranspose = 12,
    // Channel aftertouch (0xD0). Feeds the SF2 default channel-pressure
    // modulator that scales per-voice vibrato LFO depth.
    ChannelPressure = 13,
};

struct RenderEvent {
    RenderEventType type;
    uint8_t  channel;
    uint8_t  data1;
    uint8_t  data2;
    uint32_t frameOffset;
    // Original producer order.  Termination fences use this to reject note
    // ons that were queued before a later CC120/CC123/reset but reached the
    // audio thread afterward through another priority lane.
    uint32_t ingressSequence;
};

using EventDispatcher = void(*)(const RenderEvent& event, uint32_t blockCursor,
                                 void* userData);
using EventBatchDispatcher = void(*)(const RenderEvent* events, uint32_t eventCount,
                                     uint32_t blockCursor, void* userData);

constexpr uint32_t kDenseRenderChunkFrames = 128u;
constexpr uint32_t kDenseRenderHandlesPerTile = 256u;
constexpr uint32_t kDenseRenderMaximumVoices = 8192u;
constexpr uint32_t kDenseRenderMutationCapacity = 262144u;
constexpr uint64_t kDenseRenderMinimumRejectedVoiceSamples = 1u << 22u;

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
enum class DensePlanRejectReason : uint32_t {
    CorrectnessDisabled,
    MissingEvents,
    EventDensity,
    MissingWorkers,
    MissingStorage,
    VoiceCapacity,
    ShadowCapacity,
    MutationCapacity,
    Count
};

struct RenderCoverageStats {
    static constexpr uint32_t kSpanBuckets = 10u;
    uint64_t callbacks = 0u;
    uint64_t denseRendered = 0u;
    uint64_t denseExecutionFallbacks = 0u;
    uint64_t denseRejected[
        static_cast<uint32_t>(DensePlanRejectReason::Count)]{};
    uint64_t spans = 0u;
    uint64_t spanCounts[kSpanBuckets]{};
    uint64_t spanVoiceSamples[kSpanBuckets]{};
    uint64_t sparseVoiceSamples = 0u;
    uint64_t sustainedParallelVoiceSamples = 0u;
    uint64_t sustainedRejectedVoiceSamples[5]{};
};
#endif

struct DenseVoiceMutation {
    uint32_t frameOffset;
    uint32_t handle;
    uint32_t next;
};

struct alignas(64) DenseVoiceSnapshot {
    float phase;
    float phaseInc;
    float currentGain;
    float targetGain;
    float sustainLevel;
    float attackGainStep;
    float decaySlope;
    float releaseDecay;
    float mixGainL;
    float mixGainR;
    float renderGainL;
    float renderGainR;
    float relLoopSF;
    float relLoopEF;
    uint32_t sampleStart;
    uint32_t relEnd;
    uint32_t relLoopS;
    uint32_t relLoopE;
    uint32_t delayRemaining;
    uint32_t holdRemaining;
    uint32_t attackRemaining;
    uint32_t decayRemaining;
    uint32_t releaseRemaining;
    uint32_t fadeRemaining;
    uint32_t fadeTotal;
    uint8_t state;
    uint8_t envelopeStage;
    uint8_t sampleBacked;
    uint8_t loopEnabled;
    uint8_t renderClass;
    uint8_t padding[3];
};

inline void CaptureDenseVoiceSnapshot(DenseVoiceSnapshot& out,
                                      const VoiceSoA& v, uint32_t h) {
    out.phase = v.phases[h];
    out.phaseInc = v.phaseIncs[h];
    out.currentGain = v.currentGain[h];
    out.targetGain = v.targetGain[h];
    out.sustainLevel = v.sustainLevel[h];
    out.attackGainStep = v.attackGainStep[h];
    out.decaySlope = v.decaySlope[h];
    out.releaseDecay = v.releaseDecay[h];
    out.mixGainL = v.mixGainL[h];
    out.mixGainR = v.mixGainR[h];
    out.renderGainL = v.renderGainL[h];
    out.renderGainR = v.renderGainR[h];
    out.relLoopSF = v.relLoopSF[h];
    out.relLoopEF = v.relLoopEF[h];
    out.sampleStart = v.sampleStart[h];
    out.relEnd = v.relEnd[h];
    out.relLoopS = v.relLoopS[h];
    out.relLoopE = v.relLoopE[h];
    out.delayRemaining = v.delaySamplesRemaining[h];
    out.holdRemaining = v.holdSamplesRemaining[h];
    out.attackRemaining = v.attackSamplesRemaining[h];
    out.decayRemaining = v.decaySamplesRemaining[h];
    out.releaseRemaining = v.releaseSamplesRemaining[h];
    out.fadeRemaining = v.stealFadeInFramesRemaining[h];
    out.fadeTotal = v.stealFadeInFramesTotal[h];
    out.state = v.state[h];
    out.envelopeStage = v.envelopeStage[h];
    out.sampleBacked = v.sampleBacked[h];
    out.loopEnabled = v.loopEnabled[h];
    out.renderClass = v.renderClass[h];
}

inline void ApplyDenseVoiceSnapshot(VoiceSoA& v, uint32_t h,
                                    const DenseVoiceSnapshot& in) {
    v.phases[h] = in.phase;
    v.phaseIncs[h] = in.phaseInc;
    v.currentGain[h] = in.currentGain;
    v.targetGain[h] = in.targetGain;
    v.sustainLevel[h] = in.sustainLevel;
    v.attackGainStep[h] = in.attackGainStep;
    v.decaySlope[h] = in.decaySlope;
    v.releaseDecay[h] = in.releaseDecay;
    v.mixGainL[h] = in.mixGainL;
    v.mixGainR[h] = in.mixGainR;
    v.renderGainL[h] = in.renderGainL;
    v.renderGainR[h] = in.renderGainR;
    v.relLoopSF[h] = in.relLoopSF;
    v.relLoopEF[h] = in.relLoopEF;
    v.sampleStart[h] = in.sampleStart;
    v.relEnd[h] = in.relEnd;
    v.relLoopS[h] = in.relLoopS;
    v.relLoopE[h] = in.relLoopE;
    v.delaySamplesRemaining[h] = in.delayRemaining;
    v.holdSamplesRemaining[h] = in.holdRemaining;
    v.attackSamplesRemaining[h] = in.attackRemaining;
    v.decaySamplesRemaining[h] = in.decayRemaining;
    v.releaseSamplesRemaining[h] = in.releaseRemaining;
    v.stealFadeInFramesRemaining[h] = in.fadeRemaining;
    v.stealFadeInFramesTotal[h] = in.fadeTotal;
    v.state[h] = in.state;
    v.envelopeStage[h] = in.envelopeStage;
    v.sampleBacked[h] = in.sampleBacked;
    v.loopEnabled[h] = in.loopEnabled;
    v.renderClass[h] = in.renderClass;
}

struct DenseTailMutation {
    uint32_t frameOffset = 0u;
    VoiceSoA state;
};

struct DenseChunkPlan {
    DenseVoiceMutation* mutations = nullptr;
    DenseVoiceSnapshot* mutationStates = nullptr;
    DenseTailMutation* tailMutations = nullptr;
    uint32_t* tileHeads = nullptr;
    uint32_t* tileTails = nullptr;
    uint32_t mutationCount = 0u;
    uint32_t tailMutationCount = 0u;
    uint32_t tileCount = 0u;
};

struct DenseJobContext {
    class RenderScalar* renderer = nullptr;
    DenseChunkPlan* plan = nullptr;
};

// ════════════════════════════════════════════════════════════════════════
// RenderScalar — per-sample dispatch + adaptive-decimation scalar render.
//
// Architecture:
//   1. Producers push QPC-timestamped events into priority MPSC lanes;
//      the audio thread schedules them by absolute integer output frame.
//   2. RenderBlock processes one sample at a time.  At each sample:
//      a. Dispatch all events assigned to the current output frame.
//      b. Compute a decimation step from the active voice count.
//      c. Render one sample of each active voice, advancing the
//         envelope and mixing output for every Nth voice (step).
//   3. Equal-frame events retain global ingress sequence ordering.
//
// Performance notes (scalar hot loop):
//   - The per-voice body is fused directly into RenderBlock's voice loop.
//     A previous out-of-line version cost ~10 cycles/voice-sample in call
//     + 11-argument marshaling overhead and blocked SoA base registers
//     from staying cached across iterations.
//   - At decimation step 1 (< 2K voices) every voice mixes, so the
//     mixLimit division and newborn check are skipped entirely and
//     newborn age is derived from the absolute output-frame clock.
//   - Envelope dispatch is 3-way: sustained voices (stage 3, the
//     steady-state majority) skip the whole stage chain and the
//     currentGain store; releasing voices do one multiply; only
//     transient stages run the sequential chain.
//   - Loop bounds (relEnd/relLoopS/relLoopE), the loop-enabled flag and
//     the pan×volume mix gains are precomputed per voice (see
//     VoiceManager) so the per-sample path never recomputes them or
//     touches ChannelParamsSnapshot.
//
// This is the same path for live WASAPI output and offline rendering.
// ════════════════════════════════════════════════════════════════════════
class RenderScalar {
public:
    RenderScalar();
    ~RenderScalar();
    RenderScalar(const RenderScalar&) = delete;
    RenderScalar& operator=(const RenderScalar&) = delete;

    bool ReserveVoiceCapacity(uint32_t voiceCapacity);
    bool ConfigureRenderThreads(uint32_t totalRenderThreads,
                                uint32_t maximumBlockFrames);
    uint32_t GetRenderThreadCount() const;
    float GetMulticoreEffectiveness() const {
        return workerPool_ ? workerPool_->GetHelperJobPercent() : 0.0f;
    }
    uint32_t GetScratchCapacity() const { return scratchCapacity_; }
    static size_t EstimateAllocatedBytes(uint32_t voiceCapacity,
                                         uint32_t totalRenderThreads,
                                         uint32_t maximumBlockFrames) noexcept {
        if (voiceCapacity == 0u || voiceCapacity > kMaxPolyphony) return 0u;
        // The constructor keeps the default 1,000-voice scratch reservation
        // even when the configured logical/physical pool starts smaller.
        voiceCapacity = (std::max)(voiceCapacity, kMaxVoicesDefault);
        size_t bytes = sizeof(RenderScalar) +
            static_cast<size_t>(voiceCapacity) *
                (sizeof(uint32_t) * 5u + sizeof(SpanRetirement));
        if (totalRenderThreads > 1u &&
            voiceCapacity <= kDenseRenderMaximumVoices) {
            const uint32_t tileCapacity =
                (voiceCapacity + kDenseRenderHandlesPerTile - 1u) /
                    kDenseRenderHandlesPerTile;
            bytes += VoiceSoA::EstimateStorageBytes(voiceCapacity, true);
            bytes += static_cast<size_t>(kDenseRenderMutationCapacity) * 2u *
                (sizeof(DenseVoiceMutation) + sizeof(DenseVoiceSnapshot));
            bytes += static_cast<size_t>(kDenseRenderChunkFrames) * 2u *
                sizeof(DenseTailMutation);
            bytes += static_cast<size_t>(tileCapacity) * 4u *
                sizeof(uint32_t);
        }
        const size_t workerBytes = RenderWorkerPool::EstimateAllocatedBytes(
            totalRenderThreads, maximumBlockFrames, voiceCapacity);
        if (workerBytes > (std::numeric_limits<size_t>::max)() - bytes)
            return (std::numeric_limits<size_t>::max)();
        return bytes + workerBytes;
    }
    size_t GetAllocatedBytes() const {
        size_t bytes = sizeof(*this) +
            static_cast<size_t>(scratchCapacity_) *
                (sizeof(uint32_t) * 5u + sizeof(SpanRetirement)) +
            static_cast<size_t>(denseMutationCapacity_) *
                2u * (sizeof(DenseVoiceMutation) +
                      sizeof(DenseVoiceSnapshot)) +
            denseRenderState_.GetAllocatedBytes() - sizeof(VoiceSoA) +
            (workerPool_ ? workerPool_->GetAllocatedBytes() : 0u);
        if (denseMutationCapacity_ != 0u) {
            const uint32_t tileCapacity =
                (scratchCapacity_ + kDenseRenderHandlesPerTile - 1u) /
                    kDenseRenderHandlesPerTile;
            bytes += static_cast<size_t>(kDenseRenderChunkFrames) * 2u *
                sizeof(DenseTailMutation);
            bytes += static_cast<size_t>(tileCapacity) * 4u *
                sizeof(uint32_t);
        }
        return bytes;
    }

    void RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                     const int16_t* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events = nullptr,
                     uint32_t eventCount = 0,
                     bool correctnessMode = false,
                     uint64_t blockStartFrame = 0);

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    // Test-only oracle: the pre-optimization frame-major renderer.  It is
    // intentionally excluded from production builds so the DLL carries only
    // the span renderer, while differential tests can still prove state and
    // waveform equivalence.
    void RenderBlockReference(VoiceManager& voices, const ChannelCache& channels,
                     const int16_t* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events = nullptr,
                     uint32_t eventCount = 0,
                     bool correctnessMode = false,
                     uint64_t blockStartFrame = 0);
    void SetCoverageProfilingEnabledForTest(bool enabled) {
        coverageProfilingEnabled_ = enabled;
    }
    void ResetCoverageStatsForTest() { coverageStats_ = {}; }
    const RenderCoverageStats& GetCoverageStatsForTest() const {
        return coverageStats_;
    }
#endif

    void SetEventDispatcher(EventDispatcher dispatcher, void* userData);
    void SetEventBatchDispatcher(EventBatchDispatcher dispatcher, void* userData);
    bool SetRenderBackend(RenderBackend backend);
    RenderBackend GetRenderBackend() const { return kernelSet_->backend; }
    const char* GetRenderBackendName() const { return kernelSet_->name; }

private:
private:
    void RenderBlockFrameMajor(VoiceManager& voices, const ChannelCache& channels,
                     const int16_t* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events, uint32_t eventCount,
                     bool correctnessMode, uint64_t blockStartFrame);
    bool EnsureDenseStorage();
    // Returns a bit per kDenseRenderChunkFrames chunk: 1 = execute that chunk
    // through the dense parallel pipeline, 0 = leave it to the span renderer.
    // Hard eligibility gates (correctness mode, workers, storage) clear every
    // bit; workload gates (mutation capacity, duplicated planner state) clear
    // only the offending chunks so mixed callbacks stay eligible.
    uint64_t ComputeDenseChunkMask(const VoiceManager& voices,
                         const RenderEvent* events,
                         uint32_t eventCount, uint32_t numFrames,
                         bool correctnessMode) const;
    bool RenderBlockDensePlanned(VoiceManager& voices,
                     const int16_t* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t rangeStart, uint32_t rangeEnd,
                     const RenderEvent* events,
                     uint32_t eventCount, uint32_t eventIndexBegin,
                     uint64_t blockStartFrame, uint32_t* renderedTo);
    void RenderBlockSparseRange(VoiceManager& voices,
                     const ChannelCache& channels,
                     const int16_t* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t rangeStart, uint32_t rangeEnd,
                     const RenderEvent* events,
                     uint32_t eventCount, uint32_t eventIndexBegin,
                     bool vibratoActive, bool correctnessMode,
                     uint64_t blockStartFrame);
    void AdvanceAuthoritativeSpan(VoiceManager& voices,
                     const int16_t* sampleData, uint32_t sampleDataFrames,
                     uint32_t frameCount, uint64_t absoluteFrame);
    bool AdvanceDenseHandleTo(VoiceManager& voices, uint32_t handle,
                     uint32_t frameOffset);
    bool AdvanceDenseReleaseStateTo(VoiceManager& voices, uint32_t handle,
                     uint32_t frameOffset);
    bool AdvanceDensePhaseTo(VoiceManager& voices, uint32_t handle,
                     uint32_t frameOffset);
    void AdvanceDenseTailsTo(VoiceManager& voices, uint32_t frameOffset);
    static void DensePreTailCapture(VoiceHandle handle, void* userData);
    static void DenseVoiceConfigured(VoiceHandle handle, void* userData);
    static void DenseIndexedJob(uint32_t jobIndex, float* outputLeft,
                     float* outputRight, uint32_t frameCount, void* userData);
    void RenderDenseVoiceTile(const DenseChunkPlan& plan, uint32_t tileIndex,
                     float* outputLeft, float* outputRight,
                     uint32_t frameCount);
    void RenderDenseTails(const DenseChunkPlan& plan, float* outputLeft,
                     float* outputRight, uint32_t frameCount);
    EventDispatcher dispatcher_;
    EventBatchDispatcher batchDispatcher_;
    void* dispatcherUserData_;
    const RenderKernelSet* kernelSet_;
    uint32_t* classChanges_;
    alignas(64) uint32_t tailFrameCounts_[kStealTailReserve];
    SpanRetirement* retirements_;
    uint32_t scratchCapacity_;
    RenderWorkerPool* workerPool_;
    VoiceSoA denseRenderState_;
    DenseChunkPlan densePlans_[2];
    DenseJobContext denseJobContexts_[2];
    uint32_t* denseMarkEpoch_;
    uint32_t* denseMarkedHandles_;
    uint32_t* denseLastAdvancedFrames_;
    uint32_t* denseLastPhaseAdvancedFrames_;
    uint32_t denseMutationCapacity_;
    uint32_t denseMarkedCount_;
    // Adaptive dense-planner gate: total handles snapshot-marked during the
    // previous callback.  When events invalidate most of the pool every
    // frame (CC sweeps over full pools: tens of pool-sweeps per callback),
    // exact-frame planning has no leverage and its snapshot costs dominate;
    // the gate then keeps the callback on the span renderer.
    uint32_t denseCallbackMarked_ = 0;
    uint32_t denseLastCallbackMarked_ = 0;
    uint32_t denseEpoch_;
    uint32_t denseTileCount_;
    const int16_t* denseSampleData_;
    uint32_t denseSampleDataFrames_;
    const RenderKernelSet* denseKernelSet_;
    VoiceManager* densePlannerVoices_;
    uint64_t densePlannerChunkFrame_;
    uint32_t densePlannerCursor_;
    uint32_t denseTailAdvancedFrame_;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    bool coverageProfilingEnabled_;
    mutable RenderCoverageStats coverageStats_;
#endif
};

inline RenderScalar::RenderScalar()
    : dispatcher_(nullptr), batchDispatcher_(nullptr), dispatcherUserData_(nullptr),
      kernelSet_(&SelectBestRenderKernelSet()), classChanges_(nullptr),
      retirements_(nullptr), scratchCapacity_(0u),
      workerPool_(new (std::nothrow) RenderWorkerPool()),
      denseMarkEpoch_(nullptr), denseMarkedHandles_(nullptr),
      denseLastAdvancedFrames_(nullptr), denseLastPhaseAdvancedFrames_(nullptr),
      denseMutationCapacity_(0u), denseMarkedCount_(0u), denseEpoch_(1u),
      denseTileCount_(0u), denseSampleData_(nullptr),
      denseSampleDataFrames_(0u), denseKernelSet_(nullptr),
      densePlannerVoices_(nullptr), densePlannerChunkFrame_(0u),
      densePlannerCursor_(0u), denseTailAdvancedFrame_(0u)
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
      , coverageProfilingEnabled_(false), coverageStats_{}
#endif
      {
    const bool reserved = ReserveVoiceCapacity(kMaxVoicesDefault);
    assert(reserved);
    (void)reserved;
}

inline RenderScalar::~RenderScalar() {
    delete workerPool_;
    _aligned_free(classChanges_);
    _aligned_free(retirements_);
    for (DenseChunkPlan& plan : densePlans_) {
        _aligned_free(plan.mutations);
        _aligned_free(plan.mutationStates);
        delete[] plan.tailMutations;
        _aligned_free(plan.tileHeads);
        _aligned_free(plan.tileTails);
    }
    _aligned_free(denseMarkEpoch_);
    _aligned_free(denseMarkedHandles_);
    _aligned_free(denseLastAdvancedFrames_);
    _aligned_free(denseLastPhaseAdvancedFrames_);
}

inline bool RenderScalar::ConfigureRenderThreads(
    uint32_t totalRenderThreads, uint32_t maximumBlockFrames) {
    if (!workerPool_) return totalRenderThreads <= 1u;
    if (totalRenderThreads > 1u && !EnsureDenseStorage()) return false;
    // Size deterministic tile storage for the capacity already reserved by
    // the engine. A later live grow safely falls back to serial rendering
    // until the worker pool is rebuilt at restart.
    return workerPool_->Initialize(totalRenderThreads, maximumBlockFrames,
                                   scratchCapacity_);
}

inline uint32_t RenderScalar::GetRenderThreadCount() const {
    return workerPool_ ? workerPool_->GetThreadCount() : 1u;
}

inline bool RenderScalar::ReserveVoiceCapacity(uint32_t voiceCapacity) {
    if (voiceCapacity <= scratchCapacity_) return true;
    if (voiceCapacity == 0u || voiceCapacity > kMaxPolyphony) return false;
    uint32_t* classChanges = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(voiceCapacity) * sizeof(uint32_t),
        kMixBufferAlign));
    SpanRetirement* retirements = static_cast<SpanRetirement*>(_aligned_malloc(
        static_cast<size_t>(voiceCapacity) * sizeof(SpanRetirement),
        kMixBufferAlign));
    uint32_t* markEpoch = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(voiceCapacity) * sizeof(uint32_t), kMixBufferAlign));
    uint32_t* markedHandles = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(voiceCapacity) * sizeof(uint32_t), kMixBufferAlign));
    uint32_t* lastAdvancedFrames = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(voiceCapacity) * sizeof(uint32_t), kMixBufferAlign));
    uint32_t* lastPhaseAdvancedFrames = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(voiceCapacity) * sizeof(uint32_t), kMixBufferAlign));
    if (!classChanges || !retirements || !markEpoch || !markedHandles ||
        !lastAdvancedFrames || !lastPhaseAdvancedFrames) {
        _aligned_free(classChanges);
        _aligned_free(retirements);
        _aligned_free(markEpoch);
        _aligned_free(markedHandles);
        _aligned_free(lastAdvancedFrames);
        _aligned_free(lastPhaseAdvancedFrames);
        return false;
    }
    std::memset(markEpoch, 0,
                static_cast<size_t>(voiceCapacity) * sizeof(uint32_t));
    _aligned_free(classChanges_);
    _aligned_free(retirements_);
    _aligned_free(denseMarkEpoch_);
    _aligned_free(denseMarkedHandles_);
    _aligned_free(denseLastAdvancedFrames_);
    _aligned_free(denseLastPhaseAdvancedFrames_);
    classChanges_ = classChanges;
    retirements_ = retirements;
    denseMarkEpoch_ = markEpoch;
    denseMarkedHandles_ = markedHandles;
    denseLastAdvancedFrames_ = lastAdvancedFrames;
    denseLastPhaseAdvancedFrames_ = lastPhaseAdvancedFrames;
    scratchCapacity_ = voiceCapacity;
    return true;
}

inline bool RenderScalar::EnsureDenseStorage() {
    if (scratchCapacity_ == 0u ||
        scratchCapacity_ > kDenseRenderMaximumVoices) {
        return true;
    }
    if (!denseRenderState_.ReserveDenseRender(scratchCapacity_)) return false;
    if (denseMutationCapacity_ == 0u) {
        const uint32_t mutationCapacity = kDenseRenderMutationCapacity;
        const uint32_t tileCapacity =
            (scratchCapacity_ + kDenseRenderHandlesPerTile - 1u) /
            kDenseRenderHandlesPerTile;
        for (DenseChunkPlan& plan : densePlans_) {
            plan.mutations = static_cast<DenseVoiceMutation*>(_aligned_malloc(
                static_cast<size_t>(mutationCapacity) *
                    sizeof(DenseVoiceMutation), kMixBufferAlign));
            plan.mutationStates = static_cast<DenseVoiceSnapshot*>(
                _aligned_malloc(
                    static_cast<size_t>(mutationCapacity) *
                        sizeof(DenseVoiceSnapshot), kMixBufferAlign));
            plan.tailMutations =
                new (std::nothrow) DenseTailMutation[kDenseRenderChunkFrames];
            plan.tileHeads = static_cast<uint32_t*>(_aligned_malloc(
                static_cast<size_t>(tileCapacity) * sizeof(uint32_t),
                kMixBufferAlign));
            plan.tileTails = static_cast<uint32_t*>(_aligned_malloc(
                static_cast<size_t>(tileCapacity) * sizeof(uint32_t),
                kMixBufferAlign));
            if (!plan.mutations || !plan.mutationStates ||
                !plan.tailMutations || !plan.tileHeads || !plan.tileTails) {
                return false;
            }
        }
        denseJobContexts_[0] = {this, &densePlans_[0]};
        denseJobContexts_[1] = {this, &densePlans_[1]};
        denseMutationCapacity_ = mutationCapacity;
    }
    return true;
}

inline bool RenderScalar::SetRenderBackend(RenderBackend backend) {
    const RenderKernelSet* selected = SelectRenderKernelSet(backend);
    if (selected == nullptr) return false;
    kernelSet_ = selected;
    return true;
}

inline void RenderScalar::SetEventDispatcher(EventDispatcher dispatcher, void* userData) {
    dispatcher_ = dispatcher;
    dispatcherUserData_ = userData;
}

inline void RenderScalar::SetEventBatchDispatcher(EventBatchDispatcher dispatcher,
                                                   void* userData) {
    batchDispatcher_ = dispatcher;
    dispatcherUserData_ = userData;
}



// ════════════════════════════════════════════════════════════════════════
// RenderBlock — per-sample dispatch + decimation-stride render.
//
// Walk the frame range one sample at a time.  At each sample:
//   1. Dispatch all events whose integer floor equals the current frame.
//   2. Compute decimation step from the active voice count.
//   3. Process every active voice for this sample (fused per-voice body).
//
// The per-voice DSP body is fused into the loop below (it used to be a
// separate ProcessVoiceOneSample call — see class comment for why).
// `mixAudio` gates only the sample fetch + output accumulation; envelope,
// phase and retirement always advance so decimated voices don't freeze.
// ════════════════════════════════════════════════════════════════════════
inline void RenderScalar::RenderBlockFrameMajor(VoiceManager& voices, const ChannelCache& channels,
                                        const int16_t* sampleData, uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                        const RenderEvent* events, uint32_t eventCount,
                                        bool correctnessMode,
                                        uint64_t blockStartFrame) {
    voices.ApplyRuntimeVoiceLimit(blockStartFrame);
    // Scratch is indexed by the number of voices that can retire/reclassify in
    // one span, not by the physical handle ceiling. A live VoiceSoA grow may
    // therefore safely leave scratch at its old size until active polyphony
    // actually crosses it.
    if (voices.activeCount_ > scratchCapacity_ &&
        !ReserveVoiceCapacity(voices.activeCount_)) return;
    if (!classChanges_ || !retirements_) return;
    (void)cfg;
    (void)channels;
    VoiceSoA& v = voices.v;

    // Premultiplied gains are refreshed at note-on and by the exact-frame
    // controller dispatcher.  A callback-wide refresh is both redundant and
    // disproportionately expensive at small buffers on legacy processors.

    uint32_t eventIdx = 0;

    uint32_t step = correctnessMode ? 1u : ComputeDecimationStep(voices.activeCount_);

    // ── Sort activeList_ by velocity descending ───────────────────────
    // Only needed when decimation is active (step > 1).  At step=1 all
    // voices are mixed regardless of position, so the sort is dead weight.
    if (step > 1 && voices.activeCount_ > 1) {
        std::sort(voices.activeList_, voices.activeList_ + voices.activeCount_,
            [&v](uint32_t a, uint32_t b) {
                return v.velocity[a] > v.velocity[b];
            });
        voices.RebuildActivePositions();
    }

    // Running float copy of the frame index for the event-floor compare
    // (avoids a float→int conversion per event check per sample).
    uint32_t frameCursor = 0;

    for (uint32_t f = 0; f < numFrames; ++f, ++frameCursor) {
        voices.SetCurrentFrame(blockStartFrame + f);
        // ── 1. Dispatch events at this frame ──────────────────────────
        // floor(sampleOffset) <= f  <=>  sampleOffset < f + 1
        const uint32_t batchBegin = eventIdx;
        while (eventIdx < eventCount) {
            if (events[eventIdx].frameOffset > frameCursor) break;
            ++eventIdx;
        }
        if (eventIdx != batchBegin) {
            if (batchDispatcher_) {
                batchDispatcher_(events + batchBegin, eventIdx - batchBegin,
                                 f, dispatcherUserData_);
            } else if (dispatcher_) {
                for (uint32_t i = batchBegin; i < eventIdx; ++i)
                    dispatcher_(events[i], f, dispatcherUserData_);
            }
        }
        if (voices.activeCount_ > scratchCapacity_ &&
            !ReserveVoiceCapacity(voices.activeCount_)) return;

        // ── 2. Determine decimation step ──────────────────────────────
        // At step=1 every voice mixes: mixLimit and the newborn check are
        // dead work, so they are skipped entirely (fullMix fast path).
        // mixLimit is hoisted per-sample; staleness by one retirement is
        // harmless (at most one extra voice mixes for one sample).
        uint32_t stepNow = correctnessMode ? 1u
                                           : ComputeDecimationStep(voices.activeCount_);
        const bool fullMix = (stepNow == 1);
        const uint32_t mixLimit = fullMix ? 0u : (voices.activeCount_ / stepNow);

        // ── 3. Process active voices ──────────────────────────────────
        float* outL = outputLeft + f;
        float* outR = outputRight + f;

        // Tail slots are independent from active voice handles. Iterate the
        // dense reserve backwards so completed-tail swap removal is safe.
        const uint32_t tailCount = voices.GetStealTailCount();
        const uint32_t* tailHandles = voices.GetStealTailList();
        for (uint32_t position = tailCount; position > 0u; --position) {
            const uint32_t tailSlot = tailHandles[position - 1u];
            RenderStealTailSample(v, tailSlot, sampleData, sampleDataFrames,
                                  outL, outR);
            voices.RefreshStealTail(static_cast<VoiceHandle>(tailSlot));
        }

        for (uint32_t i = 0; i < voices.activeCount_; ) {
            uint32_t idx = voices.activeList_[i];

            if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) {
                // Retired earlier but not yet cleaned from activeList_ by
                // the swap-remove in RetireVoice.  Skip; the list compacts
                // naturally as retirements shrink activeCount_.
                ++i;
                continue;
            }

            const bool mixAudio = fullMix
                || (i < mixLimit)
                || (voices.GetVoiceAge(static_cast<VoiceHandle>(idx)) <
                    kNewbornProtectSamples);

            // ── Fused per-voice body ──────────────────────────────────
            float phase          = v.phases[idx];
            const float phaseStep = v.phaseIncs[idx];
            float gain           = v.currentGain[idx];
            const uint8_t voiceState = v.state[idx];
            const uint32_t sStart = v.sampleStart[idx];

            const bool isSampleBacked = (v.sampleBacked[idx] != 0 && sampleData != nullptr);
            const bool isReleased = (voiceState == static_cast<uint8_t>(VoiceState::Releasing));
            const uint8_t envelopeStageBefore = v.envelopeStage[idx];

            // Precomputed region constants (see VoiceManager::SetVoiceSample).
            const uint32_t relEnd    = v.relEnd[idx];
            const uint32_t relLoopS  = v.relLoopS[idx];
            const uint32_t relLoopE  = v.relLoopE[idx];
            const float    relLoopSF = v.relLoopSF[idx];
            const float    relLoopEF = v.relLoopEF[idx];
            const bool loop = isSampleBacked && (v.loopEnabled[idx] != 0);

            float sample = 0.0f;
            bool retireVoice = false;
            bool releaseFinished = false;
            bool sustain = false;

            if (isSampleBacked) {
                if (phase < 0.0f) phase = 0.0f;

                uint32_t baseOffset = static_cast<uint32_t>(phase);
                if (baseOffset + 1u >= relEnd) {
                    if (!loop) {
                        retireVoice = true;
                        goto done;
                    }
                    phase = relLoopSF;
                    baseOffset = relLoopS;
                }

                {
                    const uint32_t baseIdx = sStart + baseOffset;
                    uint32_t nextRel = baseOffset + 1u;
                    if (loop && nextRel >= relLoopE) nextRel = relLoopS;
                    if (nextRel >= relEnd) nextRel = relEnd - 1u;
                    const uint32_t nextIdx = sStart + nextRel;

                    const float frac = phase - static_cast<float>(baseOffset);

                    // Skip expensive sample memory access when decimated.
                    // The voice still advances phase and envelope so it
                    // retires correctly; only the audio output is skipped.
                    if (mixAudio) {
                        sample = InterpolateSample(sampleData, baseIdx, nextIdx, frac);
                    }
                }

                // ── Envelope — 3-way early dispatch ───────────────────
                // Sustain (stage 3, steady-state majority): no envelope
                // work at all and no currentGain store.  Release: one
                // multiply.  Transient stages run the sequential chain.
                {
                    const uint8_t stage = v.envelopeStage[idx];
                    sustain = !isReleased && (stage == 3);

                    if (isReleased) {
                        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
                        if (releaseRemaining == 0u) {
                            releaseFinished = true;
                        } else {
                            gain *= v.releaseDecay[idx];
                            if (releaseRemaining != UINT32_MAX) {
                                --releaseRemaining;
                                v.releaseSamplesRemaining[idx] = releaseRemaining;
                                releaseFinished = (releaseRemaining == 0u);
                            }
                        }
                    } else if (!sustain) {
                        if (v.envelopeStage[idx] == 4) {
                            if (v.delaySamplesRemaining[idx] > 0) {
                                --v.delaySamplesRemaining[idx];
                                gain = 0.0f;
                            } else {
                                v.envelopeStage[idx] = 0;
                            }
                        }
                        if (v.envelopeStage[idx] == 0) {
                            if (v.holdSamplesRemaining[idx] > 0) {
                                --v.holdSamplesRemaining[idx];
                                gain = v.targetGain[idx];
                            } else {
                                v.envelopeStage[idx] = 1;
                            }
                        }
                        if (v.envelopeStage[idx] == 1) {
                            if (v.attackSamplesRemaining[idx] > 0) {
                                gain += v.attackGainStep[idx];
                                --v.attackSamplesRemaining[idx];
                                if (gain > v.targetGain[idx]) gain = v.targetGain[idx];
                            } else {
                                gain = v.targetGain[idx];
                            }
                            if (v.attackSamplesRemaining[idx] == 0)
                                v.envelopeStage[idx] = (v.decaySamplesRemaining[idx] > 0) ? 2 : 3;
                        }
                        if (v.envelopeStage[idx] == 2) {
                            if (v.decaySamplesRemaining[idx] > 0) {
                                gain *= v.decaySlope[idx];
                                --v.decaySamplesRemaining[idx];
                                if (gain < v.sustainLevel[idx]) gain = v.sustainLevel[idx];
                            } else {
                                gain = v.sustainLevel[idx];
                            }
                            if (v.decaySamplesRemaining[idx] == 0)
                                v.envelopeStage[idx] = 3;
                        }
                    }
                }

                phase += phaseStep;

                if (loop && phase >= relLoopEF) {
                    float overflow = phase - relLoopEF;
                    const float loopLength = relLoopEF - relLoopSF;
                    if (loopLength > 0.0f && overflow >= loopLength)
                        overflow -= floorf(overflow / loopLength) * loopLength;
                    phase = relLoopSF + overflow;
                }
            }

        done:
            v.phases[idx] = phase;
            if (!sustain) v.currentGain[idx] = gain;

            float stealFadeIn = 1.0f;
            uint32_t fadeInRemaining = v.stealFadeInFramesRemaining[idx];
            if (fadeInRemaining > 0u) {
                const uint32_t fadeInTotal = v.stealFadeInFramesTotal[idx];
                stealFadeIn = fadeInTotal > 0u
                    ? static_cast<float>(fadeInTotal - fadeInRemaining + 1u) /
                      static_cast<float>(fadeInTotal)
                    : 1.0f;
                v.stealFadeInFramesRemaining[idx] = fadeInRemaining - 1u;
            }

            if (mixAudio) {
                float rotated = sample;
                if (v.rot) rotated = RotateVoiceSample(v.rot[idx], rotated);
                const float scaled = rotated * gain * stealFadeIn;
                *outL += scaled * v.mixGainL[idx];
                *outR += scaled * v.mixGainR[idx];
            }

            const bool thresholdReleaseFinished = isReleased &&
                v.releaseSamplesRemaining[idx] == UINT32_MAX &&
                gain < kVoiceRetireThreshold;
            if (retireVoice || releaseFinished || thresholdReleaseFinished) {
                voices.RetireVoice(static_cast<VoiceHandle>(idx));
                // RetireVoice swap-removes: the voice at the end of
                // activeList_ moves into position i.  Don't advance i —
                // re-process the swapped-in voice at this frame.
            } else {
                if (v.envelopeStage[idx] != envelopeStageBefore)
                    voices.RefreshRenderClass(static_cast<VoiceHandle>(idx));
                ++i;
            }
        }
    }

    // ── Batched age update ────────────────────────────────────────────
    // Whole block ran at step=1: per-sample age increments were skipped
    // in the loop, so add the block length once here.  Voices that
    // retired mid-block keep their (stale) block-start age — this makes
    // retireImmediateCount_ over-count voices that died young but past
    // their first 2 samples within the same block.  Diagnostic only.
    voices.SetCurrentFrame(blockStartFrame + numFrames);
}

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
inline void RenderScalar::RenderBlockReference(VoiceManager& voices,
                                        const ChannelCache& channels,
                                        const int16_t* sampleData,
                                        uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames,
                                        const RuntimeConfigSnapshot& cfg,
                                        const RenderEvent* events,
                                        uint32_t eventCount,
                                        bool correctnessMode,
                                        uint64_t blockStartFrame) {
    RenderBlockFrameMajor(voices, channels, sampleData, sampleDataFrames,
                          outputLeft, outputRight, numFrames, cfg, events,
                          eventCount, correctnessMode, blockStartFrame);
}
#endif

// Render an independent stolen-voice continuation across an event-free span.
// All state is held in locals and committed once.
inline void RenderStealTailSpan(VoiceSoA& v, uint32_t idx,
                                const int16_t* sampleData,
                                uint32_t sampleDataFrames,
                                float* outputLeft, float* outputRight,
                                uint32_t frameStart, uint32_t frameCount) {
    uint32_t remaining = v.stealTailFramesRemaining[idx];
    if (remaining == 0u || v.stealTailSampleBacked[idx] == 0u ||
        sampleData == nullptr || frameCount == 0u) {
        return;
    }

    const uint32_t relEnd = v.stealTailRelEnd[idx];
    if (relEnd < 2u) {
        v.stealTailFramesRemaining[idx] = 0u;
        return;
    }

    float phase = (std::max)(0.0f, v.stealTailPhase[idx]);
    const float phaseStep = v.stealTailPhaseInc[idx];
    const float gain = v.stealTailGain[idx];
    const float mixL = v.stealTailMixGainL[idx];
    const float mixR = v.stealTailMixGainR[idx];
    const uint32_t sampleStart = v.stealTailSampleStart[idx];
    const uint32_t relLoopS = v.stealTailRelLoopS[idx];
    const uint32_t relLoopE = v.stealTailRelLoopE[idx];
    const float relLoopSF = v.stealTailRelLoopSF[idx];
    const float relLoopEF = v.stealTailRelLoopEF[idx];
    const bool loop = v.stealTailLoopEnabled[idx] != 0u;
    const uint32_t total = v.stealTailFramesTotal[idx];
    const uint32_t count = (std::min)(frameCount, remaining);

    for (uint32_t n = 0; n < count; ++n) {
        uint32_t baseOffset = static_cast<uint32_t>(phase);
        if (baseOffset + 1u >= relEnd) {
            if (!loop) {
                remaining = 0u;
                break;
            }
            phase = relLoopSF;
            baseOffset = relLoopS;
        }

        uint32_t nextRel = baseOffset + 1u;
        if (loop && nextRel >= relLoopE) nextRel = relLoopS;
        if (nextRel >= relEnd) nextRel = relEnd - 1u;
        const uint32_t baseIndex = sampleStart + baseOffset;
        const uint32_t nextIndex = sampleStart + nextRel;
        if (baseIndex >= sampleDataFrames || nextIndex >= sampleDataFrames) {
            remaining = 0u;
            break;
        }

        const float frac = phase - static_cast<float>(baseOffset);
        float sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
        if (v.rot) sample = RotateVoiceSample(v.stealTailRot[idx], sample);
        const float fade = total > 1u
            ? static_cast<float>(remaining - 1u) / static_cast<float>(total - 1u)
            : 0.0f;
        const float scaled = sample * gain * fade;
        outputLeft[frameStart + n] += scaled * mixL;
        outputRight[frameStart + n] += scaled * mixR;

        phase += phaseStep;
        if (loop && phase >= relLoopEF) {
            float overflow = phase - relLoopEF;
            const float loopLength = relLoopEF - relLoopSF;
            if (loopLength > 0.0f && overflow >= loopLength)
                overflow -= floorf(overflow / loopLength) * loopLength;
            phase = relLoopSF + overflow;
        }
        --remaining;
    }

    v.stealTailPhase[idx] = phase;
    v.stealTailFramesRemaining[idx] = remaining;
}

// Render one primary voice across a span.  The return value is the zero-based
// frame inside the span on which the voice retires, or UINT32_MAX when it
// remains active.  Event dispatch cannot mutate voice state inside a span.
inline uint32_t RenderPrimaryVoiceSpan(VoiceSoA& v, uint32_t idx,
                                       const int16_t* sampleData,
                                       uint32_t sampleDataFrames,
                                       float* outputLeft, float* outputRight,
                                       uint32_t frameStart, uint32_t frameCount,
                                       uint32_t mixedFrameCount,
                                       bool exactSilentAdvance = false) {
    if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free) || frameCount == 0u)
        return UINT32_MAX;

    const bool sampleBacked = v.sampleBacked[idx] != 0u && sampleData != nullptr;
    uint32_t fadeRemaining = v.stealFadeInFramesRemaining[idx];
    const uint32_t fadeTotal = v.stealFadeInFramesTotal[idx];

    // Preserve the legacy behavior for synthetic voices without sample data:
    // they are silent and stationary, but an outstanding replacement fade
    // still advances.
    if (!sampleBacked) {
        const uint32_t consumed = (std::min)(fadeRemaining, frameCount);
        v.stealFadeInFramesRemaining[idx] -= consumed;
        return UINT32_MAX;
    }

    const uint32_t sampleStart = v.sampleStart[idx];
    const uint32_t relEnd = v.relEnd[idx];
    const uint32_t relLoopS = v.relLoopS[idx];
    const uint32_t relLoopE = v.relLoopE[idx];
    const float relLoopSF = v.relLoopSF[idx];
    const float relLoopEF = v.relLoopEF[idx];
    const bool loop = v.loopEnabled[idx] != 0u;
    const float phaseStep = v.phaseIncs[idx];
    const float mixL = v.mixGainL[idx];
    const float mixR = v.mixGainR[idx];
    float phase = (std::max)(0.0f, v.phases[idx]);
    float gain = v.currentGain[idx];
    const bool released = v.state[idx] == static_cast<uint8_t>(VoiceState::Releasing);
    uint8_t stage = v.envelopeStage[idx];

    if (relEnd < 2u || sampleStart >= sampleDataFrames ||
        relEnd > sampleDataFrames - sampleStart ||
        (loop && (relLoopS >= relLoopE || relLoopE > relEnd))) {
        return 0u;
    }

    // The overwhelmingly common path: a held, sustained, looping SF2 voice.
    // Only phase/fade state is written back after the span.
    if (!released && stage == 3u && loop) {
        // Full-quality steady state: no decimation, no replacement fade, and
        // bounds already validated above.  This is the 4K acceptance kernel.
        if (mixedFrameCount == frameCount && fadeRemaining == 0u) {
            const float gainL = gain * mixL;
            const float gainR = gain * mixR;
            float* outL = outputLeft + frameStart;
            float* outR = outputRight + frameStart;
            // Wrap-free fast chunks: phaseStep and the loop bounds are fixed
            // for the whole span, so a bounded run of frames can interpolate
            // without any wrap checks, as long as the run is provably short
            // of the next wrap point. Repeated float addition is kept (not
            // phase += step*k) so the phase trajectory is bit-identical to
            // the branchy loop; only the checks — guaranteed not to fire
            // inside the chunk — are skipped. Chunks are capped at 64 frames
            // with an 8-frame safety margin so accumulated addition drift
            // (per-frame ulp(phase)) can never cross the wrap estimate.
            const float wrapGuard = static_cast<float>(relLoopE - 1u);
            uint32_t n = 0u;
            while (n < frameCount) {
                const float distToWrap = (std::min)(wrapGuard, relLoopEF) - phase;
                uint32_t run = 0u;
                if (phaseStep > 0.0f && distToWrap > 64.0f &&
                    phase < 1048576.0f) {
                    run = static_cast<uint32_t>(distToWrap / phaseStep);
                    if (run > 8u) run -= 8u; else run = 0u;
                    if (run > 64u) run = 64u;
                    if (run > frameCount - n) run = frameCount - n;
                }
                if (run == 0u) {
                    uint32_t baseOffset = static_cast<uint32_t>(phase);
                    if (baseOffset + 1u >= relEnd) {
                        phase = relLoopSF;
                        baseOffset = relLoopS;
                    }
                    uint32_t nextRel = baseOffset + 1u;
                    if (nextRel >= relLoopE) nextRel = relLoopS;
                    const float frac = phase - static_cast<float>(baseOffset);
                    const float sample = InterpolateSample(
                        sampleData, sampleStart + baseOffset,
                        sampleStart + nextRel, frac);
                    outL[n] += sample * gainL;
                    outR[n] += sample * gainR;
                    phase += phaseStep;
                    if (phase >= relLoopEF) {
                        float overflow = phase - relLoopEF;
                        const float loopLength = relLoopEF - relLoopSF;
                        if (overflow >= loopLength)
                            overflow -= floorf(overflow / loopLength) * loopLength;
                        phase = relLoopSF + overflow;
                    }
                    ++n;
                } else {
                    const uint32_t endIndex = n + run;
                    for (; n < endIndex; ++n) {
                        const float phaseF = phase;
                        const uint32_t baseOffset = static_cast<uint32_t>(phaseF);
                        const float frac = phaseF - static_cast<float>(baseOffset);
                        const float sample = InterpolateSample(
                            sampleData, sampleStart + baseOffset,
                            sampleStart + baseOffset + 1u, frac);
                        outL[n] += sample * gainL;
                        outR[n] += sample * gainR;
                        phase += phaseStep;
                    }
                }
            }
            v.phases[idx] = phase;
            return UINT32_MAX;
        }

        // Decimated steady voices still advance phase without fetching sample
        // memory or touching the output buffers.
        if (mixedFrameCount == 0u && fadeRemaining == 0u) {
            if (exactSilentAdvance) {
                // The dense planner advances authoritative state without
                // fetching samples. Repeated addition preserves the same
                // phase rounding as the full-quality scalar/SIMD kernels.
                for (uint32_t n = 0u; n < frameCount; ++n) {
                    phase += phaseStep;
                    if (phase >= relLoopEF) {
                        float overflow = phase - relLoopEF;
                        const float loopLength = relLoopEF - relLoopSF;
                        if (loopLength > 0.0f && overflow >= loopLength)
                            overflow -= floorf(overflow / loopLength) * loopLength;
                        phase = relLoopSF + overflow;
                    }
                }
            } else {
                phase += phaseStep * static_cast<float>(frameCount);
                if (phase >= relLoopEF) {
                    float overflow = phase - relLoopEF;
                    const float loopLength = relLoopEF - relLoopSF;
                    if (loopLength > 0.0f && overflow >= loopLength)
                        overflow -= floorf(overflow / loopLength) * loopLength;
                    phase = relLoopSF + overflow;
                }
            }
            v.phases[idx] = phase;
            return UINT32_MAX;
        }

        for (uint32_t n = 0; n < frameCount; ++n) {
            uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset + 1u >= relEnd) {
                phase = relLoopSF;
                baseOffset = relLoopS;
            }

            float fade = 1.0f;
            if (fadeRemaining > 0u) {
                fade = fadeTotal > 0u
                    ? static_cast<float>(fadeTotal - fadeRemaining + 1u) /
                      static_cast<float>(fadeTotal)
                    : 1.0f;
                --fadeRemaining;
            }

            if (n < mixedFrameCount) {
                uint32_t nextRel = baseOffset + 1u;
                if (nextRel >= relLoopE) nextRel = relLoopS;
                if (nextRel >= relEnd) nextRel = relEnd - 1u;
                const uint32_t baseIndex = sampleStart + baseOffset;
                const uint32_t nextIndex = sampleStart + nextRel;
                const float frac = phase - static_cast<float>(baseOffset);
                float sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
                if (v.rot) sample = RotateVoiceSample(v.rot[idx], sample);
                const float scaled = sample * gain * fade;
                outputLeft[frameStart + n] += scaled * mixL;
                outputRight[frameStart + n] += scaled * mixR;
            }

            phase += phaseStep;
            if (phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                const float loopLength = relLoopEF - relLoopSF;
                if (loopLength > 0.0f && overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }
        }
        v.phases[idx] = phase;
        v.stealFadeInFramesRemaining[idx] = fadeRemaining;
        return UINT32_MAX;
    }

    // Dense planning advances the authoritative steal/envelope state without
    // producing samples. Releasing loops dominate chopped-note workloads, so
    // keep them out of the generic sample/envelope machine: only the phase,
    // release gain/countdown and exact retirement frame can change here.
    if (released && loop && fadeRemaining == 0u &&
        mixedFrameCount == 0u) {
        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
        const float releaseDecay = v.releaseDecay[idx];
        uint32_t retiredAt = UINT32_MAX;

        for (uint32_t n = 0u; n < frameCount; ++n) {
            const uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset + 1u >= relEnd)
                phase = relLoopSF;

            bool releaseFinished = false;
            if (releaseRemaining == 0u) {
                releaseFinished = true;
            } else {
                gain *= releaseDecay;
                if (releaseRemaining != UINT32_MAX) {
                    --releaseRemaining;
                    releaseFinished = releaseRemaining == 0u;
                }
            }

            phase += phaseStep;
            if (phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                const float loopLength = relLoopEF - relLoopSF;
                if (loopLength > 0.0f && overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }

            if (releaseFinished ||
                (releaseRemaining == UINT32_MAX &&
                 gain < kVoiceRetireThreshold)) {
                retiredAt = n;
                break;
            }
        }

        v.phases[idx] = phase;
        v.currentGain[idx] = gain;
        v.releaseSamplesRemaining[idx] = releaseRemaining;
        return retiredAt;
    }

    if (released && !loop && fadeRemaining == 0u &&
        mixedFrameCount == 0u) {
        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
        const float releaseDecay = v.releaseDecay[idx];
        uint32_t retiredAt = UINT32_MAX;

        for (uint32_t n = 0u; n < frameCount; ++n) {
            const uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset + 1u >= relEnd) {
                retiredAt = n;
                break;
            }

            bool releaseFinished = false;
            if (releaseRemaining == 0u) {
                releaseFinished = true;
            } else {
                gain *= releaseDecay;
                if (releaseRemaining != UINT32_MAX) {
                    --releaseRemaining;
                    releaseFinished = releaseRemaining == 0u;
                }
            }
            phase += phaseStep;

            if (releaseFinished ||
                (releaseRemaining == UINT32_MAX &&
                 gain < kVoiceRetireThreshold)) {
                retiredAt = n;
                break;
            }
        }

        v.phases[idx] = phase;
        v.currentGain[idx] = gain;
        v.releaseSamplesRemaining[idx] = releaseRemaining;
        return retiredAt;
    }

    // Full-quality looping attack/decay voices.  Envelope state remains local
    // while the sample cursor follows the same validated loop as steady state.
    if (!released && loop && fadeRemaining == 0u &&
        mixedFrameCount == frameCount && (stage == 1u || stage == 2u)) {
        uint32_t attackRemaining = v.attackSamplesRemaining[idx];
        uint32_t decayRemaining = v.decaySamplesRemaining[idx];
        const float targetGain = v.targetGain[idx];
        const float sustainLevel = v.sustainLevel[idx];
        const float attackStep = v.attackGainStep[idx];
        const float decaySlope = v.decaySlope[idx];
        float* outL = outputLeft + frameStart;
        float* outR = outputRight + frameStart;

        for (uint32_t n = 0; n < frameCount; ++n) {
            uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset + 1u >= relEnd) {
                phase = relLoopSF;
                baseOffset = relLoopS;
            }
            uint32_t nextRel = baseOffset + 1u;
            if (nextRel >= relLoopE) nextRel = relLoopS;
            const float frac = phase - static_cast<float>(baseOffset);
            const float sample = InterpolateSample(
                sampleData, sampleStart + baseOffset, sampleStart + nextRel, frac);

            if (stage == 1u) {
                if (attackRemaining > 0u) {
                    gain += attackStep;
                    --attackRemaining;
                    if (gain > targetGain) gain = targetGain;
                } else {
                    gain = targetGain;
                }
                if (attackRemaining == 0u)
                    stage = decayRemaining > 0u ? 2u : 3u;
            }
            if (stage == 2u) {
                if (decayRemaining > 0u) {
                    gain *= decaySlope;
                    --decayRemaining;
                    if (gain < sustainLevel) gain = sustainLevel;
                } else {
                    gain = sustainLevel;
                }
                if (decayRemaining == 0u) stage = 3u;
            }

            float sampleR = sample;
            if (v.rot) sampleR = RotateVoiceSample(v.rot[idx], sampleR);
            const float scaled = sampleR * gain;
            outL[n] += scaled * mixL;
            outR[n] += scaled * mixR;
            phase += phaseStep;
            if (phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                const float loopLength = relLoopEF - relLoopSF;
                if (overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }
        }

        v.phases[idx] = phase;
        v.currentGain[idx] = gain;
        v.envelopeStage[idx] = stage;
        v.attackSamplesRemaining[idx] = attackRemaining;
        v.decaySamplesRemaining[idx] = decayRemaining;
        return UINT32_MAX;
    }

    // Full-quality continuous-loop release. Mode-3 key-held loops are already
    // disabled by StartRelease and therefore remain on the generic path.
    if (released && loop && fadeRemaining == 0u &&
        mixedFrameCount == frameCount) {
        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
        const float releaseDecay = v.releaseDecay[idx];
        float* outL = outputLeft + frameStart;
        float* outR = outputRight + frameStart;
        uint32_t retiredAt = UINT32_MAX;

        for (uint32_t n = 0; n < frameCount; ++n) {
            uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset + 1u >= relEnd) {
                phase = relLoopSF;
                baseOffset = relLoopS;
            }
            uint32_t nextRel = baseOffset + 1u;
            if (nextRel >= relLoopE) nextRel = relLoopS;
            const float frac = phase - static_cast<float>(baseOffset);
            const float sample = InterpolateSample(
                sampleData, sampleStart + baseOffset, sampleStart + nextRel, frac);

            bool releaseFinished = false;
            if (releaseRemaining == 0u) {
                releaseFinished = true;
            } else {
                gain *= releaseDecay;
                if (releaseRemaining != UINT32_MAX) {
                    --releaseRemaining;
                    releaseFinished = releaseRemaining == 0u;
                }
            }
            float sampleR = sample;
            if (v.rot) sampleR = RotateVoiceSample(v.rot[idx], sampleR);
            const float scaled = sampleR * gain;
            outL[n] += scaled * mixL;
            outR[n] += scaled * mixR;

            phase += phaseStep;
            if (phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                const float loopLength = relLoopEF - relLoopSF;
                if (overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }
            if (releaseFinished ||
                (releaseRemaining == UINT32_MAX && gain < kVoiceRetireThreshold)) {
                retiredAt = n;
                break;
            }
        }

        v.phases[idx] = phase;
        v.currentGain[idx] = gain;
        v.releaseSamplesRemaining[idx] = releaseRemaining;
        return retiredAt;
    }

    uint32_t delayRemaining = v.delaySamplesRemaining[idx];
    uint32_t holdRemaining = v.holdSamplesRemaining[idx];
    uint32_t attackRemaining = v.attackSamplesRemaining[idx];
    uint32_t decayRemaining = v.decaySamplesRemaining[idx];
    uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
    const float targetGain = v.targetGain[idx];
    const float sustainLevel = v.sustainLevel[idx];
    const float attackStep = v.attackGainStep[idx];
    const float decaySlope = v.decaySlope[idx];
    const float releaseDecay = v.releaseDecay[idx];
    uint32_t retiredAt = UINT32_MAX;

    for (uint32_t n = 0; n < frameCount; ++n) {
        float sample = 0.0f;
        bool sampleEnded = false;
        uint32_t baseOffset = static_cast<uint32_t>(phase);
        if (baseOffset + 1u >= relEnd) {
            if (!loop) {
                sampleEnded = true;
            } else {
                phase = relLoopSF;
                baseOffset = relLoopS;
            }
        }

        if (!sampleEnded && n < mixedFrameCount) {
            uint32_t nextRel = baseOffset + 1u;
            if (loop && nextRel >= relLoopE) nextRel = relLoopS;
            if (nextRel >= relEnd) nextRel = relEnd - 1u;
            const uint32_t baseIndex = sampleStart + baseOffset;
            const uint32_t nextIndex = sampleStart + nextRel;
            const float frac = phase - static_cast<float>(baseOffset);
            sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
        }

        bool releaseFinished = false;
        const bool sustained = !released && stage == 3u;
        if (!sampleEnded) {
            if (released) {
                if (releaseRemaining == 0u) {
                    releaseFinished = true;
                } else {
                    gain *= releaseDecay;
                    if (releaseRemaining != UINT32_MAX) {
                        --releaseRemaining;
                        releaseFinished = releaseRemaining == 0u;
                    }
                }
            } else if (!sustained) {
                if (stage == 4u) {
                    if (delayRemaining > 0u) {
                        --delayRemaining;
                        gain = 0.0f;
                    } else {
                        stage = 0u;
                    }
                }
                if (stage == 0u) {
                    if (holdRemaining > 0u) {
                        --holdRemaining;
                        gain = targetGain;
                    } else {
                        stage = 1u;
                    }
                }
                if (stage == 1u) {
                    if (attackRemaining > 0u) {
                        gain += attackStep;
                        --attackRemaining;
                        if (gain > targetGain) gain = targetGain;
                    } else {
                        gain = targetGain;
                    }
                    if (attackRemaining == 0u)
                        stage = decayRemaining > 0u ? 2u : 3u;
                }
                if (stage == 2u) {
                    if (decayRemaining > 0u) {
                        gain *= decaySlope;
                        --decayRemaining;
                        if (gain < sustainLevel) gain = sustainLevel;
                    } else {
                        gain = sustainLevel;
                    }
                    if (decayRemaining == 0u) stage = 3u;
                }
            }

            phase += phaseStep;
            if (loop && phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                const float loopLength = relLoopEF - relLoopSF;
                if (loopLength > 0.0f && overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }
        }

        float fade = 1.0f;
        if (fadeRemaining > 0u) {
            fade = fadeTotal > 0u
                ? static_cast<float>(fadeTotal - fadeRemaining + 1u) /
                  static_cast<float>(fadeTotal)
                : 1.0f;
            --fadeRemaining;
        }

        if (!sampleEnded && n < mixedFrameCount) {
            float sampleR = sample;
            if (v.rot) sampleR = RotateVoiceSample(v.rot[idx], sampleR);
            const float scaled = sampleR * gain * fade;
            outputLeft[frameStart + n] += scaled * mixL;
            outputRight[frameStart + n] += scaled * mixR;
        }

        const bool thresholdFinished = released && releaseRemaining == UINT32_MAX &&
                                       gain < kVoiceRetireThreshold;
        if (sampleEnded || releaseFinished || thresholdFinished) {
            retiredAt = n;
            break;
        }
    }

    v.phases[idx] = phase;
    v.currentGain[idx] = gain;
    v.envelopeStage[idx] = stage;
    v.delaySamplesRemaining[idx] = delayRemaining;
    v.holdSamplesRemaining[idx] = holdRemaining;
    v.attackSamplesRemaining[idx] = attackRemaining;
    v.decaySamplesRemaining[idx] = decayRemaining;
    v.releaseSamplesRemaining[idx] = releaseRemaining;
    v.stealFadeInFramesRemaining[idx] = fadeRemaining;
    return retiredAt;
}

inline uint64_t RenderScalar::ComputeDenseChunkMask(
    const VoiceManager& voices, const RenderEvent* events,
    uint32_t eventCount, uint32_t numFrames, bool correctnessMode) const {
    // Per-voice phase rotation keeps its filter state in VoiceSoA, which the
    // dense snapshot pipeline does not carry; rotation runs on the span
    // renderer only.
    if (voices.v.rot != nullptr) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (coverageProfilingEnabled_)
            ++coverageStats_.denseRejected[
                static_cast<uint32_t>(DensePlanRejectReason::MissingWorkers)];
#endif
        return 0ull;
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    auto reject = [&](DensePlanRejectReason reason) {
        if (coverageProfilingEnabled_)
            ++coverageStats_.denseRejected[static_cast<uint32_t>(reason)];
        return false;
    };
    constexpr uint64_t kEmptyMask = 0ull;
#else
    constexpr uint64_t kEmptyMask = 0ull;
#endif
    if (!events || numFrames == 0u) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        reject(DensePlanRejectReason::MissingEvents);
#endif
        return kEmptyMask;
    }
    if (numFrames > 8192u) {
        // The per-chunk estimate slots and the chunk mask both assume the
        // documented maximum callback length; longer blocks stay sparse.
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        reject(DensePlanRejectReason::MissingEvents);
#endif
        return kEmptyMask;
    }
    if (!workerPool_ || workerPool_->GetThreadCount() <= 1u) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        reject(DensePlanRejectReason::MissingWorkers);
#endif
        return kEmptyMask;
    }

    // Planning must advance every time-varying voice on the coordinator so
    // exact-frame stealing sees the same envelope state as the serial oracle.
    // Workers then advance those voices again while producing their samples.
    // In release-heavy chopped material this duplicated serial work costs
    // more than the tile render saves (and adding workers makes it worse).
    // Keep that regime on the short-span SIMD path, which is exact and already
    // vectorizes across voices.  This is a workload gate only; event frames
    // and ordering are unchanged.
    const uint32_t activeVoices = voices.GetActiveCount();
    const uint32_t sustainedVoices = voices.GetRenderClassCount(
        VoiceRenderClass::SustainedLoop);
    const uint32_t timeVaryingVoices = activeVoices > sustainedVoices
        ? activeVoices - sustainedVoices : 0u;
    if (eventCount < numFrames) {
        // Sparse rendering already parallelizes profitable sustained spans.
        // Dense planning earns its setup cost only when event fragmentation
        // leaves substantial synthesis serial after those existing gates.
        const uint32_t otherVoices = voices.GetActiveCount() > sustainedVoices
            ? voices.GetActiveCount() - sustainedVoices : 0u;
        uint64_t rejectedVoiceSamples =
            static_cast<uint64_t>(otherVoices) * numFrames;
        uint32_t cursor = 0u;
        uint32_t previousBoundary = UINT32_MAX;
        for (uint32_t index = 0u; index < eventCount; ++index) {
            const uint32_t boundary =
                (std::min)(events[index].frameOffset, numFrames);
            if (boundary == previousBoundary) continue;
            previousBoundary = boundary;
            if (boundary > cursor) {
                const uint32_t spanFrames = boundary - cursor;
                if (workerPool_->ClassifyParallelization(
                        sustainedVoices, spanFrames) !=
                    RenderParallelRejectReason::None) {
                    rejectedVoiceSamples +=
                        static_cast<uint64_t>(sustainedVoices) * spanFrames;
                }
                cursor = boundary;
            }
        }
        if (cursor < numFrames) {
            const uint32_t spanFrames = numFrames - cursor;
            if (workerPool_->ClassifyParallelization(
                    sustainedVoices, spanFrames) !=
                RenderParallelRejectReason::None) {
                rejectedVoiceSamples +=
                    static_cast<uint64_t>(sustainedVoices) * spanFrames;
            }
        }
        if (rejectedVoiceSamples <
            kDenseRenderMinimumRejectedVoiceSamples) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
            reject(DensePlanRejectReason::EventDensity);
#endif
            return kEmptyMask;
        }
    }
    if (!densePlans_[0].mutations || !densePlans_[1].mutations) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        reject(DensePlanRejectReason::MissingStorage);
#endif
        return kEmptyMask;
    }
    if (voices.GetMaxVoices() > kDenseRenderMaximumVoices) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        reject(DensePlanRejectReason::VoiceCapacity);
#endif
        return kEmptyMask;
    }
    if (denseRenderState_.GetCapacity() < voices.GetMaxVoices()) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        reject(DensePlanRejectReason::ShadowCapacity);
#endif
        return kEmptyMask;
    }

    // Bound the handles that can actually mutate in each chunk. Every event
    // batch marks the voices inside its affected pools; global operations
    // refresh the full active pool; a note launch mutates only its own
    // physical group plus the play groups its layers retire; channel and key
    // operations cannot escape their affected channel populations. Repeated
    // same-frame changes collapse to one final snapshot per handle.
    uint64_t estimatedMutationsPerChunk[
        (8192u + kDenseRenderChunkFrames - 1u) /
        kDenseRenderChunkFrames]{};
    // Distinct event frames per chunk drive the per-chunk form of the
    // duplicated-planner-state gate: dense planning re-advances every
    // time-varying voice once per event batch on the coordinator.
    uint32_t distinctEventFramesPerChunk[
        (8192u + kDenseRenderChunkFrames - 1u) /
        kDenseRenderChunkFrames]{};
    // One launch of L layers retires at most L play groups (one steal per
    // layer) and configures at most L handles, all deduplicated per batch.
    // Group sizes never exceed the largest group ever launched, which
    // VoiceManager maintains in O(1). This keeps the execution-time capacity
    // check a true assertion without charging every note-on the full pool.
    const uint64_t launchMutationBound = [&voices]() {
        const uint64_t groupSize = voices.GetMaxLaunchGroupSize();
        return groupSize * (groupSize + 1u);
    }();
    uint32_t frame = UINT32_MAX;
    uint16_t affectedChannels = 0u;
    bool mayTouchFullPool = false;
    uint32_t noteOnCount = 0u;
    auto flushFrameEstimate = [&]() {
        if (frame == UINT32_MAX || frame >= numFrames) return;
        uint64_t& slot = estimatedMutationsPerChunk[
            frame / kDenseRenderChunkFrames];
        // Mutations are recorded only for marked handles: voices inside the
        // batch's affected pools (channel/key populations, or the full pool
        // for global ops) plus handles configured by note launches. The
        // per-batch advance of every time-varying decay/release voice serves
        // exact-frame stealing but records no snapshot, so it must not be
        // charged here; that duplicated planner work is bounded separately by
        // the per-chunk event-density gate below.
        if (mayTouchFullPool) slot += voices.GetMaxVoices();
        slot += static_cast<uint64_t>(noteOnCount) * launchMutationBound;
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
            if ((affectedChannels & (1u << channel)) != 0u)
                slot += voices.GetChannelActiveCount(
                    static_cast<uint8_t>(channel));
        }
    };
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const RenderEvent& event = events[index];
        if (event.frameOffset != frame) {
            flushFrameEstimate();
            frame = event.frameOffset;
            affectedChannels = 0u;
            mayTouchFullPool = false;
            noteOnCount = 0u;
            if (frame < numFrames)
                ++distinctEventFramesPerChunk[
                    frame / kDenseRenderChunkFrames];
        }
        switch (event.type) {
            case RenderEventType::Reset:
            case RenderEventType::MasterVolume:
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
                mayTouchFullPool = true;
                break;
            case RenderEventType::NoteOn:
                ++noteOnCount;
                break;
            case RenderEventType::NoteOff:
            case RenderEventType::StaleNoteOffBatch:
            case RenderEventType::ControlChange:
            case RenderEventType::PitchBend:
            case RenderEventType::AllNotesOff:
            case RenderEventType::AllSoundOff:
                if (event.channel < kChannelCount)
                    affectedChannels |=
                        static_cast<uint16_t>(1u << event.channel);
                break;
            default:
                break;
        }
    }
    flushFrameEstimate();
    const uint32_t chunks =
        (numFrames + kDenseRenderChunkFrames - 1u) /
        kDenseRenderChunkFrames;
    uint64_t chunkMask = chunks >= 64u
        ? ~0ull : ((1ull << chunks) - 1ull);
    for (uint32_t chunk = 0u; chunk < chunks; ++chunk) {
        if (estimatedMutationsPerChunk[chunk] > denseMutationCapacity_) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
            // Per-chunk accounting: a single hot chunk no longer disqualifies
            // its neighbours, it only runs on the span renderer itself.
            reject(DensePlanRejectReason::MutationCapacity);
#endif
            chunkMask &= ~(1ull << chunk);
            continue;
        }
        // Per-chunk duplicated-state gate (see the whole-callback rationale
        // above): planning that would re-advance every time-varying voice for
        // more than half of the chunk's synthesis work costs more than the
        // tile render saves. Keep such chunks on the vectorized span path.
        const uint32_t chunkBegin = chunk * kDenseRenderChunkFrames;
        const uint32_t chunkFrames = (std::min)(
            numFrames - chunkBegin, kDenseRenderChunkFrames);
        const uint64_t duplicatedStateWork =
            static_cast<uint64_t>(timeVaryingVoices) *
            distinctEventFramesPerChunk[chunk];
        const uint64_t chunkVoiceSamples =
            static_cast<uint64_t>(activeVoices) * chunkFrames;
        if (chunkVoiceSamples != 0u &&
            duplicatedStateWork >= chunkVoiceSamples / 2u) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
            reject(DensePlanRejectReason::EventDensity);
#endif
            chunkMask &= ~(1ull << chunk);
        }
    }
    return chunkMask;
}

inline void RenderScalar::AdvanceAuthoritativeSpan(
    VoiceManager& voices, const int16_t* sampleData, uint32_t sampleDataFrames,
    uint32_t frameCount, uint64_t absoluteFrame) {
    if (frameCount == 0u) return;
    VoiceSoA& v = voices.v;
    voices.SetCurrentFrame(absoluteFrame);
    uint32_t retireCount = 0u;
    uint32_t classChangeCount = 0u;
    const uint32_t capturedCount = voices.activeCount_;
    for (uint32_t position = 0u; position < capturedCount; ++position) {
        const uint32_t handle = voices.activeList_[position];
        const uint8_t oldClass = v.renderClass[handle];
        const uint32_t retiredAt = RenderPrimaryVoiceSpan(
            v, handle, sampleData, sampleDataFrames, nullptr, nullptr, 0u,
            frameCount, 0u, true);
        if (retiredAt != UINT32_MAX) {
            retirements_[retireCount++] = {
                handle, retiredAt, voices.activePosition_[handle]};
        } else if (v.renderClass[handle] == oldClass &&
                   (oldClass == static_cast<uint8_t>(
                                    VoiceRenderClass::TransientLoop) ||
                    oldClass == static_cast<uint8_t>(
                                    VoiceRenderClass::Generic))) {
            classChanges_[classChangeCount++] = handle;
        }
    }

    const uint32_t tailCount = voices.GetStealTailCount();
    const uint32_t* tailHandles = voices.GetStealTailList();
    for (uint32_t position = tailCount; position > 0u; --position) {
        const uint32_t handle = tailHandles[position - 1u];
        AdvanceStealTailSpanExact(v, handle, frameCount);
        voices.RefreshStealTail(static_cast<VoiceHandle>(handle));
    }
    for (uint32_t index = 0u; index < classChangeCount; ++index)
        voices.RefreshRenderClass(
            static_cast<VoiceHandle>(classChanges_[index]));

    std::sort(retirements_, retirements_ + retireCount,
        [](const SpanRetirement& a, const SpanRetirement& b) {
            if (a.frameOffset != b.frameOffset)
                return a.frameOffset < b.frameOffset;
            return a.capturePosition < b.capturePosition;
        });
    for (uint32_t index = 0u; index < retireCount; ++index) {
        voices.SetCurrentFrame(absoluteFrame + retirements_[index].frameOffset);
        voices.RetireVoice(
            static_cast<VoiceHandle>(retirements_[index].handle));
    }
    voices.SetCurrentFrame(absoluteFrame + frameCount);
}

inline bool RenderScalar::AdvanceDenseHandleTo(
    VoiceManager& voices, uint32_t handle, uint32_t frameOffset) {
    if (handle >= voices.GetMaxVoices() ||
        voices.v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) {
        return false;
    }
    const uint32_t previous = denseLastAdvancedFrames_[handle];
    if (frameOffset <= previous) return true;
    VoiceSoA& v = voices.v;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Active) &&
        v.envelopeStage[handle] == 3u && v.loopEnabled[handle] != 0u &&
        v.stealFadeInFramesRemaining[handle] == 0u) {
        float phase = (std::max)(0.0f, v.phases[handle]);
        const float step = v.phaseIncs[handle];
        const float loopStart = v.relLoopSF[handle];
        const float loopEnd = v.relLoopEF[handle];
        const float loopLength = loopEnd - loopStart;
        const uint32_t relEnd = v.relEnd[handle];
        // Mirror the per-sample loop's baseOffset+1 >= relEnd check
        // (RenderPrimaryVoiceSpan lines 1229-1232): when the integer sample
        // position reaches the end of the sample, the per-sample path wraps
        // to loop start BEFORE adding phaseStep on that frame.  The bulk
        // advance must account for this initial wrap, otherwise a voice at
        // relEnd-1 gets an extra 0.5..1.0 loop-samples of phase advance
        // compared to per-sample rendering, producing a sub-sample
        // discontinuity at every mutation boundary in the dense planner.
        if (relEnd > 0u && static_cast<uint32_t>(phase) + 1u >= relEnd)
            phase = loopStart;
        const uint32_t advancedFrames = frameOffset - previous;
        phase += step * static_cast<float>(advancedFrames);
        // AVX2's 1-4-frame kernel wraps before sampling and intentionally
        // leaves the final increment pending.  Match that phase convention
        // while planning the next chunk, otherwise stealing on the crossing
        // frame captures a tail from a different sample position.  Scalar
        // and SSE2 commit the final wrap eagerly.
        if (kernelSet_->backend == RenderBackend::AVX2 &&
            advancedFrames != 0u && loopLength > 0.0f) {
            const float phaseBeforeFinalIncrement = phase - step;
            if (phaseBeforeFinalIncrement >= loopEnd) {
                const float completedLoops = 1.0f + floorf(
                    (phaseBeforeFinalIncrement - loopEnd) / loopLength);
                phase -= completedLoops * loopLength;
            }
        } else if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (loopLength > 0.0f && overflow >= loopLength)
                overflow -= floorf(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        v.phases[handle] = phase;
        denseLastAdvancedFrames_[handle] = frameOffset;
        denseLastPhaseAdvancedFrames_[handle] = frameOffset;
        return true;
    }
    const uint32_t retiredAt = RenderPrimaryVoiceSpan(
        v, handle, denseSampleData_, denseSampleDataFrames_, nullptr,
        nullptr, 0u, frameOffset - previous, 0u, true);
    denseLastAdvancedFrames_[handle] = frameOffset;
    denseLastPhaseAdvancedFrames_[handle] = frameOffset;
    if (retiredAt == UINT32_MAX) return true;
    voices.SetCurrentFrame(densePlannerChunkFrame_ + previous + retiredAt);
    voices.RetireVoice(static_cast<VoiceHandle>(handle));
    voices.SetCurrentFrame(densePlannerChunkFrame_ + frameOffset);
    return false;
}

inline bool RenderScalar::AdvanceDenseReleaseStateTo(
    VoiceManager& voices, uint32_t handle, uint32_t frameOffset) {
    VoiceSoA& v = voices.v;
    // This helper consumes a snapshot of the ReleaseLoop render class.  Its
    // class invariants guarantee a live, looping release without a steal
    // fade, so repeating those checks for every voice and event frame only
    // bloats the hottest dense-launch loop.
    assert(handle < voices.GetMaxVoices());
    assert(v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing));
    assert(v.loopEnabled[handle] != 0u);
    assert(v.stealFadeInFramesRemaining[handle] == 0u);
    const uint32_t previous = denseLastAdvancedFrames_[handle];
    if (frameOffset <= previous) return true;

    float gain = v.currentGain[handle];
    uint32_t releaseRemaining = v.releaseSamplesRemaining[handle];
    const float releaseDecay = v.releaseDecay[handle];
    const uint32_t frameCount = frameOffset - previous;
    uint32_t retiredAt = UINT32_MAX;
    if (frameCount == 1u) {
        bool releaseFinished = releaseRemaining == 0u;
        if (!releaseFinished) {
            gain *= releaseDecay;
            if (releaseRemaining != UINT32_MAX) {
                --releaseRemaining;
                releaseFinished = releaseRemaining == 0u;
            }
        }
        if (releaseFinished ||
            (releaseRemaining == UINT32_MAX &&
             gain < kVoiceRetireThreshold)) {
            retiredAt = 0u;
        }
    } else for (uint32_t n = 0u; n < frameCount; ++n) {
        bool releaseFinished = false;
        if (releaseRemaining == 0u) {
            releaseFinished = true;
        } else {
            gain *= releaseDecay;
            if (releaseRemaining != UINT32_MAX) {
                --releaseRemaining;
                releaseFinished = releaseRemaining == 0u;
            }
        }
        if (releaseFinished ||
            (releaseRemaining == UINT32_MAX &&
             gain < kVoiceRetireThreshold)) {
            retiredAt = n;
            break;
        }
    }
    v.currentGain[handle] = gain;
    v.releaseSamplesRemaining[handle] = releaseRemaining;
    denseLastAdvancedFrames_[handle] = frameOffset;
    if (retiredAt == UINT32_MAX) return true;
    voices.SetCurrentFrame(densePlannerChunkFrame_ + previous + retiredAt);
    voices.RetireVoice(static_cast<VoiceHandle>(handle));
    voices.SetCurrentFrame(densePlannerChunkFrame_ + frameOffset);
    return false;
}

inline bool RenderScalar::AdvanceDensePhaseTo(
    VoiceManager& voices, uint32_t handle, uint32_t frameOffset) {
    if (handle >= voices.GetMaxVoices() ||
        voices.v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) {
        return false;
    }
    const uint32_t previous = denseLastPhaseAdvancedFrames_[handle];
    if (frameOffset <= previous) return true;
    VoiceSoA& v = voices.v;
    if (v.state[handle] != static_cast<uint8_t>(VoiceState::Releasing) ||
        v.loopEnabled[handle] == 0u) {
        denseLastPhaseAdvancedFrames_[handle] = frameOffset;
        return true;
    }

    float phase = (std::max)(0.0f, v.phases[handle]);
    const float phaseStep = v.phaseIncs[handle];
    const float loopStart = v.relLoopSF[handle];
    const float loopEnd = v.relLoopEF[handle];
    const uint32_t relEnd = v.relEnd[handle];
    for (uint32_t n = previous; n < frameOffset; ++n) {
        if (static_cast<uint32_t>(phase) + 1u >= relEnd)
            phase = loopStart;
        phase += phaseStep;
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            const float loopLength = loopEnd - loopStart;
            if (loopLength > 0.0f && overflow >= loopLength)
                overflow -= floorf(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
    }
    v.phases[handle] = phase;
    denseLastPhaseAdvancedFrames_[handle] = frameOffset;
    return true;
}

inline void RenderScalar::AdvanceDenseTailsTo(
    VoiceManager& voices, uint32_t frameOffset) {
    if (frameOffset <= denseTailAdvancedFrame_) return;
    const uint32_t frames = frameOffset - denseTailAdvancedFrame_;
    const uint32_t count = voices.GetStealTailCount();
    const uint32_t* handles = voices.GetStealTailList();
    for (uint32_t position = count; position > 0u; --position) {
        const uint32_t handle = handles[position - 1u];
        AdvanceStealTailSpanExact(voices.v, handle, frames);
        voices.RefreshStealTail(static_cast<VoiceHandle>(handle));
    }
    denseTailAdvancedFrame_ = frameOffset;
}

inline void RenderScalar::DensePreTailCapture(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer && renderer->densePlannerVoices_) {
        renderer->AdvanceDenseHandleTo(*renderer->densePlannerVoices_, handle,
                                       renderer->densePlannerCursor_);
        renderer->AdvanceDensePhaseTo(*renderer->densePlannerVoices_, handle,
                                      renderer->densePlannerCursor_);
    }
}

inline void RenderScalar::DenseVoiceConfigured(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (!renderer || handle >= renderer->scratchCapacity_) return;
    if (renderer->denseMarkEpoch_[handle] == renderer->denseEpoch_) return;
    renderer->denseMarkEpoch_[handle] = renderer->denseEpoch_;
    renderer->denseMarkedHandles_[renderer->denseMarkedCount_++] = handle;
    renderer->denseLastAdvancedFrames_[handle] =
        renderer->densePlannerCursor_;
    renderer->denseLastPhaseAdvancedFrames_[handle] =
        renderer->densePlannerCursor_;
}

inline void RenderScalar::RenderDenseVoiceTile(
    const DenseChunkPlan& plan, uint32_t tileIndex, float* outputLeft,
    float* outputRight, uint32_t frameCount) {
    VoiceSoA& v = denseRenderState_;
    const uint32_t firstHandle = tileIndex * kDenseRenderHandlesPerTile;
    const uint32_t lastHandle = (std::min)(
        firstHandle + kDenseRenderHandlesPerTile, v.GetCapacity());
    uint32_t mutation = plan.tileHeads[tileIndex];
    alignas(64) uint32_t
        classHandles[kVoiceRenderClassCount][kDenseRenderHandlesPerTile];
    uint32_t classCounts[kVoiceRenderClassCount]{};
    uint8_t touched[kDenseRenderHandlesPerTile] = {};
    uint32_t touchedCount = 0u;

    // Pass 1: mark the voices this chunk actually mutates.  Everything else
    // is state-stable for the whole chunk and renders in ONE long span,
    // which keeps the per-voice time-chunked kernels (8-frame vector
    // chunks) engaged.  Per-frame span splits had degenerated the dense
    // path into single-frame voice-batched dispatch — several times the
    // cycles per voice-sample — even though each event touches a handful
    // of voices.
    for (uint32_t m = mutation; m != UINT32_MAX; m = plan.mutations[m].next) {
        const uint32_t slot = plan.mutations[m].handle - firstHandle;
        if (slot < kDenseRenderHandlesPerTile && touched[slot] == 0u) {
            touched[slot] = 1u;
            ++touchedCount;
        }
    }

    // Pass 2: stable voices, one full-chunk span per class.  Class kernels
    // carry envelope evolution internally (transient's stage 3 is a no-op
    // with the same effective gain as the sustained path), so no mid-chunk
    // re-classification is needed.  Retiring classes free their slots in
    // dense mode (RecordRetirement with null arrays).
    if (false && touchedCount < lastHandle - firstHandle) {
        std::memset(classCounts, 0, sizeof(classCounts));
        for (uint32_t handle = firstHandle; handle < lastHandle; ++handle) {
            if (touched[handle - firstHandle] != 0u ||
                v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) {
                continue;
            }
            uint32_t actual = v.renderClass[handle] < kVoiceRenderClassCount
                ? v.renderClass[handle]
                : static_cast<uint32_t>(VoiceRenderClass::Generic);
            const bool clean = v.stealFadeInFramesRemaining[handle] == 0u;
            const bool active = v.state[handle] ==
                static_cast<uint8_t>(VoiceState::Active);
            if (clean && active && v.loopEnabled[handle] != 0u &&
                v.envelopeStage[handle] == 3u) {
                actual = static_cast<uint32_t>(
                    VoiceRenderClass::SustainedLoop);
            } else if (clean && active && v.loopEnabled[handle] != 0u &&
                       (v.envelopeStage[handle] == 1u ||
                        v.envelopeStage[handle] == 2u)) {
                actual = static_cast<uint32_t>(
                    VoiceRenderClass::TransientLoop);
            }
            classHandles[actual][classCounts[actual]++] = handle;
        }
        // Region-grouped ordering within each class: consecutive voices
        // then read the same sample-data cache lines (at 40k+ polyphony
        // over a fixed key range, dozens of voices share one SF2 region).
        // Once per chunk instead of once per span.
        for (uint32_t classIndex = 0u;
             classIndex < kVoiceRenderClassCount; ++classIndex) {
            const uint32_t count = classCounts[classIndex];
            if (count > 1u) {
                uint32_t* list = classHandles[classIndex];
                std::sort(list, list + count, [&v](uint32_t a, uint32_t b) {
                    return v.sampleStart[a] < v.sampleStart[b];
                });
            }
        }
        for (uint32_t classIndex = 0u;
             classIndex < kVoiceRenderClassCount; ++classIndex) {
            const uint32_t count = classCounts[classIndex];
            if (count == 0u) continue;
            const RenderSpanContext context{
                &v, denseSampleData_, denseSampleDataFrames_, outputLeft,
                outputRight, 0u, frameCount, v.GetCapacity(),
                nullptr, nullptr, nullptr, nullptr, nullptr, 0u};
            RenderClassKernel kernel = denseKernelSet_->kernels[classIndex];
            if (kernel && kernel(context, classHandles[classIndex], count))
                continue;
            for (uint32_t index = 0u; index < count; ++index) {
                const uint32_t handle = classHandles[classIndex][index];
                const uint32_t retiredAt = RenderPrimaryVoiceSpan(
                    v, handle, denseSampleData_, denseSampleDataFrames_,
                    outputLeft, outputRight, 0u, frameCount, frameCount);
                if (retiredAt != UINT32_MAX)
                    v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
            }
        }
    }

    // Shared classification for the dense shadow state: renderClass as
    // recorded by the VoiceManager, refined by live shadow envelope state
    // (stage 3 -> sustained, stages 1/2 -> transient).
    auto classifyDense = [&v](uint32_t handle) -> uint32_t {
        uint32_t actual = v.renderClass[handle] < kVoiceRenderClassCount
            ? v.renderClass[handle]
            : static_cast<uint32_t>(VoiceRenderClass::Generic);
        const bool clean = v.stealFadeInFramesRemaining[handle] == 0u;
        const bool active = v.state[handle] ==
            static_cast<uint8_t>(VoiceState::Active);
        if (clean && active && v.loopEnabled[handle] != 0u &&
            v.envelopeStage[handle] == 3u) {
            actual = static_cast<uint32_t>(VoiceRenderClass::SustainedLoop);
        } else if (clean && active && v.loopEnabled[handle] != 0u &&
                   (v.envelopeStage[handle] == 1u ||
                    v.envelopeStage[handle] == 2u)) {
            actual = static_cast<uint32_t>(VoiceRenderClass::TransientLoop);
        }
        return actual;
    };

    // Pass 3: mutated voices, exact-frame segments, rendered through the
    // class kernels over live per-segment buckets.  In chopped material
    // most of the pool is touched every chunk, so these buckets — not
    // pass 2 — carry the load; routing them through the class kernels keeps
    // the voice-batched AVX2 short-span paths engaged instead of falling
    // back to scalar per-voice rendering.  Bucket membership changes only
    // at mutation frames (snapshot application) and at kernel-recorded
    // class transitions / retirements, both applied at segment ends.
    uint32_t cursor = 0u;
    uint32_t tileClassChanges[kDenseRenderHandlesPerTile];
    uint32_t tileClassChangeCount = 0u;
    uint8_t slotClass[kDenseRenderHandlesPerTile];
    uint32_t touchedSlots[kDenseRenderHandlesPerTile];
    uint32_t touchedSlotCount = 0u;
    std::memset(classCounts, 0, sizeof(classCounts));
    for (uint32_t slot = 0u; slot < kDenseRenderHandlesPerTile; ++slot) {
        if (touched[slot] == 0u) continue;
        touchedSlots[touchedSlotCount++] = slot;
        const uint32_t handle = firstHandle + slot;
        if (v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) {
            slotClass[slot] = UINT8_MAX;
            continue;
        }
        const uint32_t actual = classifyDense(handle);
        classHandles[actual][classCounts[actual]++] = handle;
        slotClass[slot] = static_cast<uint8_t>(actual);
    }
    while (cursor < frameCount) {
        while (mutation != UINT32_MAX &&
               plan.mutations[mutation].frameOffset == cursor) {
            const DenseVoiceMutation& change = plan.mutations[mutation];
            ApplyDenseVoiceSnapshot(v, change.handle,
                                    plan.mutationStates[mutation]);
            mutation = change.next;
            const uint32_t slot = change.handle - firstHandle;
            if (slot >= kDenseRenderHandlesPerTile) continue;
            // Move the slot between buckets: out of its old class, and back
            // in under its post-snapshot classification (unless freed).
            const uint8_t oldClass = slotClass[slot];
            const uint32_t handle = firstHandle + slot;
            if (oldClass != UINT8_MAX) {
                uint32_t* list = classHandles[oldClass];
                uint32_t& count = classCounts[oldClass];
                for (uint32_t index = 0u; index < count; ++index) {
                    if (list[index] != handle) continue;
                    list[index] = list[--count];
                    break;
                }
            }
            if (v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) {
                slotClass[slot] = UINT8_MAX;
            } else {
                const uint32_t actual = classifyDense(handle);
                classHandles[actual][classCounts[actual]++] = handle;
                slotClass[slot] = static_cast<uint8_t>(actual);
            }
        }
        uint32_t spanEnd = frameCount;
        if (mutation != UINT32_MAX)
            spanEnd = (std::min)(spanEnd,
                plan.mutations[mutation].frameOffset);
        if (spanEnd <= cursor) {
            ++cursor;
            continue;
        }
        const uint32_t spanFrames = spanEnd - cursor;
        for (uint32_t classIndex = 0u;
             classIndex < kVoiceRenderClassCount; ++classIndex) {
            const uint32_t count = classCounts[classIndex];
            if (count == 0u) continue;
            uint32_t* list = classHandles[classIndex];
            const RenderSpanContext context{
                &v, denseSampleData_, denseSampleDataFrames_, outputLeft,
                outputRight, cursor, spanFrames, v.GetCapacity(),
                tileClassChanges, &tileClassChangeCount,
                nullptr, nullptr, nullptr, 0u};
            RenderClassKernel kernel = denseKernelSet_->kernels[classIndex];
            if (kernel && kernel(context, list, count)) {
                // Class transitions recorded by the kernel move voices
                // between buckets (transient -> sustained on attack/decay
                // completion).
                for (uint32_t index = 0u; index < tileClassChangeCount;
                     ++index) {
                    const uint32_t handle = tileClassChanges[index];
                    const uint32_t slot = handle - firstHandle;
                    if (slot >= kDenseRenderHandlesPerTile) continue;
                    const uint8_t oldClass = slotClass[slot];
                    if (oldClass == UINT8_MAX) continue;
                    uint32_t* oldList = classHandles[oldClass];
                    uint32_t& oldCount = classCounts[oldClass];
                    for (uint32_t pos = 0u; pos < oldCount; ++pos) {
                        if (oldList[pos] != handle) continue;
                        oldList[pos] = oldList[--oldCount];
                        break;
                    }
                    const uint32_t actual = classifyDense(handle);
                    classHandles[actual][classCounts[actual]++] = handle;
                    slotClass[slot] = static_cast<uint8_t>(actual);
                }
                tileClassChangeCount = 0u;
                continue;
            }
            for (uint32_t index = 0u; index < count; ++index) {
                const uint32_t handle = list[index];
                const uint32_t slot = handle - firstHandle;
                const uint32_t retiredAt = RenderPrimaryVoiceSpan(
                    v, handle, denseSampleData_, denseSampleDataFrames_,
                    outputLeft, outputRight, cursor, spanFrames, spanFrames);
                if (retiredAt != UINT32_MAX) {
                    v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
                    slotClass[slot] = UINT8_MAX;
                }
            }
        }
        // Retiring kernels free slots without records (dense mode drops the
        // retirement record); sweep the touched set for freed voices and
        // drop them from their buckets.
        for (uint32_t index = 0u; index < touchedSlotCount; ++index) {
            const uint32_t slot = touchedSlots[index];
            if (slotClass[slot] == UINT8_MAX) continue;
            const uint32_t handle = firstHandle + slot;
            if (v.state[handle] != static_cast<uint8_t>(VoiceState::Free))
                continue;
            uint32_t* freedList = classHandles[slotClass[slot]];
            uint32_t& freedCount = classCounts[slotClass[slot]];
            for (uint32_t pos = 0u; pos < freedCount; ++pos) {
                if (freedList[pos] != handle) continue;
                freedList[pos] = freedList[--freedCount];
                break;
            }
            slotClass[slot] = UINT8_MAX;
        }
        tileClassChangeCount = 0u;
        cursor = spanEnd;
    }
}

inline void RenderScalar::RenderDenseTails(
    const DenseChunkPlan& plan, float* outputLeft, float* outputRight,
    uint32_t frameCount) {
    VoiceSoA& v = denseRenderState_;
    uint32_t mutation = 0u;
    uint32_t cursor = 0u;
    while (cursor < frameCount) {
        while (mutation < plan.tailMutationCount &&
               plan.tailMutations[mutation].frameOffset == cursor) {
            v.CopyFixedTailsFrom(plan.tailMutations[mutation].state);
            ++mutation;
        }
        uint32_t spanEnd = frameCount;
        if (mutation < plan.tailMutationCount)
            spanEnd = plan.tailMutations[mutation].frameOffset;
        for (uint32_t handle = 0u; handle < kStealTailReserve; ++handle) {
            if (v.stealTailFramesRemaining[handle] == 0u) continue;
            RenderStealTailSpan(v, handle, denseSampleData_,
                                denseSampleDataFrames_, outputLeft,
                                outputRight, cursor, spanEnd - cursor);
        }
        cursor = spanEnd;
    }
}

inline void RenderScalar::DenseIndexedJob(
    uint32_t jobIndex, float* outputLeft, float* outputRight,
    uint32_t frameCount, void* userData) {
    DenseJobContext* job = static_cast<DenseJobContext*>(userData);
    RenderScalar* renderer = job->renderer;
    const DenseChunkPlan& plan = *job->plan;
    if (jobIndex < plan.tileCount) {
        renderer->RenderDenseVoiceTile(plan, jobIndex, outputLeft,
                                       outputRight, frameCount);
    } else {
        renderer->RenderDenseTails(plan, outputLeft, outputRight, frameCount);
    }
}

inline bool RenderScalar::RenderBlockDensePlanned(
    VoiceManager& voices, const int16_t* sampleData, uint32_t sampleDataFrames,
    float* outputLeft, float* outputRight, uint32_t rangeStart,
    uint32_t rangeEnd, const RenderEvent* events, uint32_t eventCount,
    uint32_t eventIndexBegin, uint64_t blockStartFrame,
    uint32_t* renderedTo) {
    *renderedTo = rangeStart;
    denseRenderState_.CopyDenseRenderStateFrom(voices.v);
    denseSampleData_ = sampleData;
    denseSampleDataFrames_ = sampleDataFrames;
    denseKernelSet_ = kernelSet_;
    denseTileCount_ = (voices.GetMaxVoices() +
        kDenseRenderHandlesPerTile - 1u) / kDenseRenderHandlesPerTile;
    // Advance-tracking arrays hold block-relative offsets. A segment may
    // start mid-callback, so seed every active handle at the segment start
    // instead of zero; inactive slots never influence planning.
    std::memset(denseLastAdvancedFrames_, 0,
                static_cast<size_t>(voices.GetMaxVoices()) *
                    sizeof(uint32_t));
    std::memset(denseLastPhaseAdvancedFrames_, 0,
                static_cast<size_t>(voices.GetMaxVoices()) *
                    sizeof(uint32_t));
    for (uint32_t position = 0u; position < voices.activeCount_; ++position) {
        const uint32_t handle = voices.activeList_[position];
        denseLastAdvancedFrames_[handle] = rangeStart;
        denseLastPhaseAdvancedFrames_[handle] = rangeStart;
    }
    densePlannerVoices_ = &voices;
    densePlannerChunkFrame_ = blockStartFrame;
    densePlannerCursor_ = rangeStart;
    denseTailAdvancedFrame_ = rangeStart;
    voices.SetPreTailCaptureHook(DensePreTailCapture, this);
    voices.SetVoiceConfiguredHook(DenseVoiceConfigured, this);

    uint32_t eventIndex = eventIndexBegin;
    bool renderInFlight = false;
    uint32_t chunkIndex = 0u;
    for (uint32_t chunkStart = rangeStart; chunkStart < rangeEnd;
         chunkStart += kDenseRenderChunkFrames, ++chunkIndex) {
        DenseChunkPlan& plan = densePlans_[chunkIndex & 1u];
        const uint32_t chunkEnd = (std::min)(
            rangeEnd, chunkStart + kDenseRenderChunkFrames);
        const uint32_t chunkFrames = chunkEnd - chunkStart;
        plan.mutationCount = 0u;
        plan.tailMutationCount = 0u;
        plan.tileCount = denseTileCount_;
        std::fill(plan.tileHeads, plan.tileHeads + denseTileCount_, UINT32_MAX);
        std::fill(plan.tileTails, plan.tileTails + denseTileCount_, UINT32_MAX);

        uint32_t cursor = chunkStart;
        while (cursor < chunkEnd) {
            const uint32_t batchBegin = eventIndex;
            while (eventIndex < eventCount &&
                   events[eventIndex].frameOffset <= cursor) {
                ++eventIndex;
            }
            if (eventIndex != batchBegin) {
                densePlannerCursor_ = cursor;
                AdvanceDenseTailsTo(voices, densePlannerCursor_);
                // Stable sustained loops have a time-invariant steal level.
                // Advance only volatile/finite voices before a decision; a
                // selected stable victim is advanced lazily by the tail hook.
                uint32_t advanceCount = 0u;
                uint32_t releaseLoopBegin = 0u;
                uint32_t releaseLoopEnd = 0u;
                // SustainedLoop's class invariant already guarantees active,
                // clean stage-3 looping state; scanning that overwhelmingly
                // common class here would recreate a frame-major O(V) loop.
                for (uint32_t classIndex =
                         static_cast<uint32_t>(
                             VoiceRenderClass::SustainedOneShot);
                     classIndex < kVoiceRenderClassCount; ++classIndex) {
                    if (classIndex == static_cast<uint32_t>(
                            VoiceRenderClass::ReleaseLoop)) {
                        releaseLoopBegin = advanceCount;
                    }
                    voices.ForEachRenderClassBlock(
                        static_cast<VoiceRenderClass>(classIndex),
                        [&](const uint32_t* handles, uint32_t count) {
                            std::memcpy(classChanges_ + advanceCount, handles,
                                        static_cast<size_t>(count) *
                                            sizeof(uint32_t));
                            advanceCount += count;
                        });
                    if (classIndex == static_cast<uint32_t>(
                            VoiceRenderClass::ReleaseLoop)) {
                        releaseLoopEnd = advanceCount;
                    }
                }
                for (uint32_t index = 0u; index < advanceCount; ++index) {
                    if (index >= releaseLoopBegin &&
                        index < releaseLoopEnd) {
                        AdvanceDenseReleaseStateTo(
                            voices, classChanges_[index], densePlannerCursor_);
                    } else {
                        AdvanceDenseHandleTo(
                            voices, classChanges_[index], densePlannerCursor_);
                    }
                }
                if (++denseEpoch_ == 0u) {
                    std::memset(denseMarkEpoch_, 0,
                        static_cast<size_t>(scratchCapacity_) *
                            sizeof(uint32_t));
                    denseEpoch_ = 1u;
                }
                denseMarkedCount_ = 0u;
                bool affectAll = false;
                uint16_t affectedChannels = 0u;
                bool affectedKeys[kChannelCount][kNoteCount]{};
                bool hasAffectedKey = false;
                for (uint32_t index = batchBegin; index < eventIndex; ++index) {
                    const RenderEvent& event = events[index];
                    switch (event.type) {
                        case RenderEventType::Reset:
                        case RenderEventType::MasterVolume:
                        case RenderEventType::MasterFineTune:
                        case RenderEventType::MasterTranspose:
                            affectAll = true;
                            break;
                        case RenderEventType::NoteOff:
                        case RenderEventType::StaleNoteOffBatch:
                            if (event.channel < kChannelCount &&
                                event.data1 < kNoteCount) {
                                affectedKeys[event.channel][event.data1] = true;
                                hasAffectedKey = true;
                            }
                            break;
                        case RenderEventType::ControlChange:
                        case RenderEventType::PitchBend:
                        case RenderEventType::AllNotesOff:
                        case RenderEventType::AllSoundOff:
                            if (event.channel < kChannelCount)
                                affectedChannels |=
                                    static_cast<uint16_t>(1u << event.channel);
                            break;
                        default:
                            break;
                    }
                }
                auto mark = [&](uint32_t handle) {
                    if (denseMarkEpoch_[handle] == denseEpoch_) return;
                    denseMarkEpoch_[handle] = denseEpoch_;
                    denseMarkedHandles_[denseMarkedCount_++] = handle;
                };
                if (affectAll) {
                    // Rare global operations keep the authoritative full-pool
                    // scan.  Everything else resolves through the per-key and
                    // per-channel voice indices — an O(voices-on-the-key)
                    // walk instead of an O(activeCount) sweep per event
                    // batch, which at full pools dominated event-dense
                    // callbacks (712 batches x 8192 voices).
                    for (uint32_t position = 0u;
                         position < voices.activeCount_;) {
                        const uint32_t handle = voices.activeList_[position];
                        if (AdvanceDenseHandleTo(
                                voices, handle, densePlannerCursor_)) {
                            AdvanceDensePhaseTo(
                                voices, handle, densePlannerCursor_);
                            mark(handle);
                            ++position;
                        }
                    }
                } else {
                    for (uint8_t channel = 0u;
                         channel < kChannelCount; ++channel) {
                        const bool channelHit =
                            (affectedChannels & (1u << channel)) != 0u;
                        if (!channelHit && !hasAffectedKey) continue;
                        if (channelHit) {
                            // CC/pitch/sustain-class events change every
                            // voice on the channel, releasing ones included
                            // (ForEachChannelActive covers both).
                            voices.ForEachChannelActive(channel,
                                [&](VoiceHandle handle) {
                                    if (AdvanceDenseHandleTo(
                                            voices, handle,
                                            densePlannerCursor_)) {
                                        AdvanceDensePhaseTo(
                                            voices, handle,
                                            densePlannerCursor_);
                                        mark(handle);
                                    }
                                });
                        } else {
                            for (uint32_t note = 0u; note < kNoteCount;
                                 ++note) {
                                if (!affectedKeys[channel][note]) continue;
                                voices.ForEachChannelKeyVoice(channel,
                                    static_cast<uint8_t>(note),
                                    [&](VoiceHandle handle) {
                                        if (AdvanceDenseHandleTo(
                                                voices, handle,
                                                densePlannerCursor_)) {
                                            AdvanceDensePhaseTo(
                                                voices, handle,
                                                densePlannerCursor_);
                                            mark(handle);
                                        }
                                    });
                            }
                        }
                    }
                }

                voices.SetCurrentFrame(blockStartFrame + cursor);
                if (batchDispatcher_) {
                    batchDispatcher_(events + batchBegin,
                                     eventIndex - batchBegin, cursor,
                                     dispatcherUserData_);
                } else if (dispatcher_) {
                    for (uint32_t index = batchBegin; index < eventIndex;
                         ++index) {
                        dispatcher_(events[index], cursor, dispatcherUserData_);
                    }
                }
                if (plan.mutationCount + denseMarkedCount_ >
                    denseMutationCapacity_) {
                    voices.SetPreTailCaptureHook(nullptr, nullptr);
                    voices.SetVoiceConfiguredHook(nullptr, nullptr);
                    densePlannerVoices_ = nullptr;
                    if (renderInFlight) workerPool_->FinishIndexed();
                    // Feed the adaptive gate: a capacity overrun means this
                    // callback marked far too many handles for exact-frame
                    // planning to pay off; without this the next callback
                    // would repeat the same doomed planning attempt.
                    denseLastCallbackMarked_ = denseCallbackMarked_;
                    // Chunks before this one are fully rendered; the caller
                    // recovers by sparse-rendering [chunkStart, rangeEnd).
                    *renderedTo = chunkStart;
                    return false;
                }
                denseCallbackMarked_ += denseMarkedCount_;
                for (uint32_t marked = 0u; marked < denseMarkedCount_;
                     ++marked) {
                    const uint32_t handle = denseMarkedHandles_[marked];
                    const uint32_t mutationIndex = plan.mutationCount++;
                    DenseVoiceMutation& mutation =
                        plan.mutations[mutationIndex];
                    mutation.frameOffset = cursor - chunkStart;
                    mutation.handle = handle;
                    mutation.next = UINT32_MAX;
                    CaptureDenseVoiceSnapshot(
                        plan.mutationStates[mutationIndex], voices.v, handle);
                    const uint32_t tile =
                        handle / kDenseRenderHandlesPerTile;
                    if (plan.tileHeads[tile] == UINT32_MAX)
                        plan.tileHeads[tile] = mutationIndex;
                    else
                        plan.mutations[plan.tileTails[tile]].next =
                            mutationIndex;
                    plan.tileTails[tile] = mutationIndex;
                }
                DenseTailMutation& tail =
                    plan.tailMutations[plan.tailMutationCount++];
                tail.frameOffset = cursor - chunkStart;
                tail.state.CopyFixedTailsFrom(voices.v);
            }

            uint32_t next = chunkEnd;
            if (eventIndex < eventCount)
                next = (std::min)(next, events[eventIndex].frameOffset);
            if (next <= cursor) continue;
            cursor = next;
        }

        // Chunk N was planned while helpers rendered chunk N-1. Once the
        // plan is immutable, join any remaining old jobs and publish this
        // plan without touching its buffers again.
        if (renderInFlight) {
            workerPool_->FinishIndexed();
            renderInFlight = false;
        }
        DenseJobContext& jobContext = denseJobContexts_[chunkIndex & 1u];
        if (workerPool_->BeginIndexed(
                denseTileCount_ + 1u, chunkFrames,
                outputLeft + chunkStart, outputRight + chunkStart,
                DenseIndexedJob, &jobContext)) {
            renderInFlight = true;
        } else {
            for (uint32_t job = 0u; job <= denseTileCount_; ++job)
                DenseIndexedJob(job, outputLeft + chunkStart,
                                outputRight + chunkStart, chunkFrames,
                                &jobContext);
        }
    }
    if (renderInFlight) workerPool_->FinishIndexed();
    voices.SetPreTailCaptureHook(nullptr, nullptr);
    voices.SetVoiceConfiguredHook(nullptr, nullptr);
    densePlannerVoices_ = nullptr;

    // Worker shadow state is authoritative for render progress. MIDI/linkage
    // state stayed exclusively in VoiceManager while chunks were planned.
    for (uint32_t position = 0u; position < voices.activeCount_;) {
        const uint32_t handle = voices.activeList_[position];
        if (denseRenderState_.state[handle] ==
            static_cast<uint8_t>(VoiceState::Free)) {
            voices.SetCurrentFrame(blockStartFrame + rangeEnd);
            voices.RetireVoice(static_cast<VoiceHandle>(handle));
            continue;
        }
        // Long scalar/SSE2 tile spans use the branch-free pre-wrap loop and
        // may leave their final increment pending.  Their exact-frame serial
        // path commits that wrap after every 1-4-frame span, so normalize the
        // shadow at the callback boundary before it becomes authoritative.
        if (kernelSet_->backend != RenderBackend::AVX2 &&
            denseRenderState_.state[handle] ==
                static_cast<uint8_t>(VoiceState::Active) &&
            denseRenderState_.loopEnabled[handle] != 0u) {
            float& phase = denseRenderState_.phases[handle];
            const float loopStart = denseRenderState_.relLoopSF[handle];
            const float loopEnd = denseRenderState_.relLoopEF[handle];
            const float loopLength = loopEnd - loopStart;
            if (phase >= loopEnd && loopLength > 0.0f) {
                float overflow = phase - loopEnd;
                if (overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = loopStart + overflow;
            }
        }
        VoiceSoA::CopyRenderProgress(voices.v, handle, denseRenderState_);
        voices.RefreshRenderClass(static_cast<VoiceHandle>(handle));
        ++position;
    }
    voices.v.CopyFixedTailsFrom(denseRenderState_);
    for (uint32_t tail = 0u; tail < kStealTailReserve; ++tail)
        voices.RefreshStealTail(static_cast<VoiceHandle>(tail));
    voices.SetCurrentFrame(blockStartFrame + rangeEnd);
    denseLastCallbackMarked_ = denseCallbackMarked_;
    *renderedTo = rangeEnd;
    return true;
}

inline void RenderScalar::RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                                      const int16_t* sampleData, uint32_t sampleDataFrames,
                                      float* outputLeft, float* outputRight,
                                      uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                      const RenderEvent* events, uint32_t eventCount,
                                      bool correctnessMode,
                                      uint64_t blockStartFrame) {
    (void)cfg;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    if (coverageProfilingEnabled_) ++coverageStats_.callbacks;
#endif
    voices.SetStealKeyBackend(kernelSet_->backend);
    // Live pool-limit changes are callback-boundary commands.  Applying one
    // before dense eligibility/snapshotting prevents a mid-plan lifecycle
    // mutation from invalidating worker-visible voice state.
    voices.ApplyRuntimeVoiceLimit(blockStartFrame);
    if (voices.activeCount_ > scratchCapacity_ &&
        !ReserveVoiceCapacity(voices.activeCount_)) return;
    if (!classChanges_ || !retirements_) return;
    // Vibrato modulation mutates phaseIncs between spans on the audio
    // thread, which the dense planner's immutable chunk plans cannot model.
    // Bypass dense planning while any channel carries a modulation depth;
    // the serial path below handles every voice exactly as before.
    const ChannelParamsSnapshot* channelParams = channels.GetParams();
    bool vibratoActive = false;
    for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
        if (channelParams[channel].modDepth > 0.0f) {
            vibratoActive = true;
            break;
        }
    }
    // Chunk-granular mixed mode: dense-parallel chunks where planning is
    // profitable, exact span rendering for the rest. Segments run strictly
    // sequentially, so the dense pipeline's shadow state and the span
    // renderer's authoritative state never interleave: each dense segment
    // copies state in, renders, and commits before the next segment starts.
    const bool denseGatesOpen = !vibratoActive && correctnessMode &&
        events != nullptr && numFrames != 0u && workerPool_ != nullptr &&
        workerPool_->GetThreadCount() > 1u &&
        denseLastCallbackMarked_ <= voices.GetActiveCount() * 24u;
    denseCallbackMarked_ = 0;
    const uint64_t denseChunkMask = denseGatesOpen
        ? ComputeDenseChunkMask(voices, events, eventCount, numFrames,
                                correctnessMode)
        : 0ull;
    const uint32_t chunkCount =
        (numFrames + kDenseRenderChunkFrames - 1u) / kDenseRenderChunkFrames;
    uint32_t segStart = 0u;
    uint32_t eventCursor = 0u;
    while (segStart < numFrames) {
        const bool denseRun =
            ((denseChunkMask >> (segStart / kDenseRenderChunkFrames)) & 1ull)
                != 0ull;
        uint32_t chunk = segStart / kDenseRenderChunkFrames;
        while (chunk < chunkCount &&
               (((denseChunkMask >> chunk) & 1ull) != 0ull) == denseRun)
            ++chunk;
        const uint32_t segEnd =
            (std::min)(numFrames, chunk * kDenseRenderChunkFrames);
        while (eventCursor < eventCount &&
               events[eventCursor].frameOffset < segStart)
            ++eventCursor;
        bool needSparse = !denseRun;
        if (denseRun) {
            uint32_t renderedTo = segEnd;
            if (RenderBlockDensePlanned(
                    voices, sampleData, sampleDataFrames, outputLeft,
                    outputRight, segStart, segEnd, events, eventCount,
                    eventCursor, blockStartFrame, &renderedTo)) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
                if (coverageProfilingEnabled_) ++coverageStats_.denseRendered;
#endif
            } else {
                // The chunk-mask estimator bounds mutations from above, so an
                // execution overrun is assertion-grade. Recover exactly by
                // span-rendering only the unrendered remainder.
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
                if (coverageProfilingEnabled_)
                    ++coverageStats_.denseExecutionFallbacks;
#endif
                assert(renderedTo >= segStart && renderedTo <= segEnd);
                needSparse = true;
                segStart = renderedTo;
                while (eventCursor < eventCount &&
                       events[eventCursor].frameOffset < segStart)
                    ++eventCursor;
            }
        }
        if (needSparse) {
            RenderBlockSparseRange(voices, channels, sampleData,
                sampleDataFrames, outputLeft, outputRight, segStart, segEnd,
                events, eventCount, eventCursor, vibratoActive,
                correctnessMode, blockStartFrame);
        }
        segStart = segEnd;
    }
}

inline void RenderScalar::RenderBlockSparseRange(
    VoiceManager& voices, const ChannelCache& channels,
    const int16_t* sampleData, uint32_t sampleDataFrames,
    float* outputLeft, float* outputRight, uint32_t rangeStart,
    uint32_t rangeEnd, const RenderEvent* events, uint32_t eventCount,
    uint32_t eventIndexBegin, bool vibratoActive, bool correctnessMode,
    uint64_t blockStartFrame) {
    VoiceSoA& v = voices.v;
    const RenderKernelSet& kernelSet = *kernelSet_;
    const ChannelParamsSnapshot* channelParams = channels.GetParams();
    // Gain state is already current for this boundary; see the frame-major
    // oracle above and Driver::HandleControlChange.

    const uint32_t initialStep = correctnessMode
        ? 1u : ComputeDecimationStep(voices.activeCount_);
    if (initialStep > 1u && voices.activeCount_ > 1u) {
        std::sort(voices.activeList_, voices.activeList_ + voices.activeCount_,
            [&v](uint32_t a, uint32_t b) { return v.velocity[a] > v.velocity[b]; });
        voices.RebuildActivePositions();
    }

    uint32_t eventIndex = eventIndexBegin;
    uint32_t cursor = rangeStart;
    while (cursor < rangeEnd) {
        voices.SetCurrentFrame(blockStartFrame + cursor);

        // State changes at this boundary are visible to the first rendered
        // frame of the span.  Equal-frame order is already ingress order.
        const uint32_t batchBegin = eventIndex;
        while (eventIndex < eventCount && events[eventIndex].frameOffset <= cursor)
            ++eventIndex;
        if (eventIndex != batchBegin) {
            if (batchDispatcher_) {
                batchDispatcher_(events + batchBegin, eventIndex - batchBegin,
                                 cursor, dispatcherUserData_);
            } else if (dispatcher_) {
                for (uint32_t i = batchBegin; i < eventIndex; ++i)
                    dispatcher_(events[i], cursor, dispatcherUserData_);
            }
        }
        if (voices.activeCount_ > scratchCapacity_ &&
            !ReserveVoiceCapacity(voices.activeCount_)) return;

        uint32_t spanEnd = rangeEnd;
        if (eventIndex < eventCount && events[eventIndex].frameOffset < spanEnd)
            spanEnd = events[eventIndex].frameOffset;
        if (vibratoActive && spanEnd - cursor > kVibratoUpdateFrames)
            spanEnd = cursor + kVibratoUpdateFrames;
        if (spanEnd <= cursor) continue;

        if (vibratoActive)
            AdvanceVibratoSpan(voices, channelParams, spanEnd - cursor);

        const uint32_t spanFrames = spanEnd - cursor;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (coverageProfilingEnabled_) {
            uint32_t bucket = 0u;
            uint32_t range = spanFrames;
            while (range > 1u &&
                   bucket + 1u < RenderCoverageStats::kSpanBuckets) {
                range = (range + 1u) >> 1u;
                ++bucket;
            }
            const uint64_t voiceSamples =
                static_cast<uint64_t>(voices.activeCount_) * spanFrames;
            ++coverageStats_.spans;
            ++coverageStats_.spanCounts[bucket];
            coverageStats_.spanVoiceSamples[bucket] += voiceSamples;
            coverageStats_.sparseVoiceSamples += voiceSamples;
        }
#endif
        uint32_t retireCount = 0u;
        uint32_t classChangeCount = 0u;
        const uint32_t tailCount = voices.GetStealTailCount();
        const uint32_t* tailHandles = voices.GetStealTailList();
        const uint32_t voiceCapacity = voices.GetMaxVoices();
        const uint32_t tailCapacity =
            (std::min)(voiceCapacity, kStealTailReserve);
        const bool denseTails = tailCount * 2u >= voiceCapacity;
        if (denseTails) {
            // Continuous full-pool stealing leaves nearly every slot with a
            // tail. Sequential slot traversal is cheaper and much friendlier
            // to the SoA caches than chasing the constantly shuffled sparse
            // list. Normal playback retains the sparse O(tailCount) path.
            for (uint32_t idx = 0; idx < tailCapacity; ++idx) {
                if (v.stealTailFramesRemaining[idx] != 0u)
                    tailFrameCounts_[idx] = spanFrames;
            }
        } else {
            for (uint32_t position = 0; position < tailCount; ++position)
                tailFrameCounts_[tailHandles[position]] = spanFrames;
        }

        // Class counts are captured before rendering.  Natural retirements
        // and envelope transitions are deferred, so every list stays stable
        // while all backends consume the span without copying activeList_.
        uint32_t remainingClasses = voices.GetNonemptyRenderClassMask();
        for (uint32_t classIndex = 0; classIndex < kVoiceRenderClassCount;
             ++classIndex) {
            if ((remainingClasses & (1u << classIndex)) == 0u) continue;
            const VoiceRenderClass renderClass =
                static_cast<VoiceRenderClass>(classIndex);
            RenderClassKernel classKernel = kernelSet.kernels[classIndex];
            const RenderSpanContext context{
                &v, sampleData, sampleDataFrames, outputLeft, outputRight,
                cursor, spanFrames, voices.GetMaxVoices(), classChanges_,
                &classChangeCount, voices.activePosition_, retirements_,
                &retireCount};
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
            if (coverageProfilingEnabled_ &&
                renderClass == VoiceRenderClass::SustainedLoop) {
                const uint32_t classVoices =
                    voices.GetRenderClassCount(renderClass);
                const uint64_t voiceSamples =
                    static_cast<uint64_t>(classVoices) * spanFrames;
                RenderParallelRejectReason reason =
                    RenderParallelRejectReason::Unavailable;
                if (workerPool_ && classKernel != nullptr && sampleData != nullptr)
                    reason = workerPool_->ClassifyParallelization(
                        classVoices, spanFrames);
                if (reason == RenderParallelRejectReason::None) {
                    coverageStats_.sustainedParallelVoiceSamples += voiceSamples;
                } else {
                    coverageStats_.sustainedRejectedVoiceSamples[
                        static_cast<uint32_t>(reason)] += voiceSamples;
                }
            }
#endif
            // TransientLoop voices never retire mid-span but can complete
            // attack+decay (reported through per-job class-change scratch);
            // ReleaseLoop voices retire mid-span (per-job retirement scratch).
            // Both lifecycle records are merged deterministically by
            // RenderWorkerPool::Execute, so these classes parallelize too.
            if ((renderClass == VoiceRenderClass::SustainedLoop ||
                 renderClass == VoiceRenderClass::TransientLoop ||
                 renderClass == VoiceRenderClass::ReleaseLoop) &&
                classKernel != nullptr && sampleData != nullptr &&
                workerPool_ && workerPool_->ShouldParallelize(
                    voices.GetRenderClassCount(renderClass), spanFrames)) {
                workerPool_->BeginSpan(context);
                bool queued = true;
                voices.ForEachRenderClassBlock(renderClass,
                    [&](const uint32_t* handles, uint32_t classCount) {
                        if (queued) {
                            queued = workerPool_->AddClassRange(
                                classKernel, handles, classCount);
                        }
                    });
                if (queued && workerPool_->Execute()) continue;
            }
            voices.ForEachRenderClassBlock(renderClass,
                [&](const uint32_t* handles, uint32_t classCount) {
                if (classKernel != nullptr && sampleData != nullptr &&
                    classKernel(context, handles, classCount)) return;

                for (uint32_t position = 0; position < classCount; ++position) {
                    const uint32_t idx = handles[position];
                    if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free))
                        continue;

                    uint32_t retiredAt = UINT32_MAX;
                    const bool cleanPrimary =
                        v.stealFadeInFramesRemaining[idx] == 0u;
                    if (cleanPrimary &&
                        renderClass == VoiceRenderClass::SustainedLoop) {
                        retiredAt = ScalarRenderSustainedLoop(
                            v, idx, sampleData, sampleDataFrames, outputLeft,
                            outputRight, cursor, spanFrames);
                    } else if (cleanPrimary &&
                               renderClass == VoiceRenderClass::SustainedOneShot) {
                        retiredAt = ScalarRenderSustainedOneShot(
                            v, idx, sampleData, sampleDataFrames, outputLeft,
                            outputRight, cursor, spanFrames);
                    } else {
                        retiredAt = RenderPrimaryVoiceSpan(
                            v, idx, sampleData, sampleDataFrames, outputLeft,
                            outputRight, cursor, spanFrames, spanFrames);
                    }

                    if (retiredAt != UINT32_MAX) {
                        retirements_[retireCount++] = {
                            idx, retiredAt, voices.activePosition_[idx]};
                    } else if (
                        renderClass == VoiceRenderClass::TransientLoop ||
                        renderClass == VoiceRenderClass::Generic) {
                        classChanges_[classChangeCount++] = idx;
                    }
                }
            });
        }

        // Tails have their own sparse lifecycle list and render independently
        // from primary class ordering.  Iterate backwards so O(1) swap-removal
        // of a completed tail cannot skip an unprocessed entry.
        if (denseTails) {
            for (uint32_t idx = 0; idx < tailCapacity; ++idx) {
                if (v.stealTailFramesRemaining[idx] == 0u) continue;
                RenderStealTailSpan(v, idx, sampleData, sampleDataFrames,
                                    outputLeft, outputRight, cursor,
                                    tailFrameCounts_[idx]);
                voices.RefreshStealTail(static_cast<VoiceHandle>(idx));
            }
        } else {
            for (uint32_t position = tailCount; position > 0u; --position) {
                const uint32_t idx = tailHandles[position - 1u];
                RenderStealTailSpan(v, idx, sampleData, sampleDataFrames,
                                    outputLeft, outputRight, cursor,
                                    tailFrameCounts_[idx]);
                voices.RefreshStealTail(static_cast<VoiceHandle>(idx));
            }
        }

        for (uint32_t i = 0; i < classChangeCount; ++i)
            voices.RefreshRenderClass(static_cast<VoiceHandle>(classChanges_[i]));

        // Retirement is deferred until every captured handle has rendered,
        // so class-list and active-list swap removal cannot skip a voice.
        std::sort(retirements_, retirements_ + retireCount,
            [](const SpanRetirement& a, const SpanRetirement& b) {
                if (a.frameOffset != b.frameOffset)
                    return a.frameOffset < b.frameOffset;
                return a.capturePosition < b.capturePosition;
            });
        for (uint32_t i = 0; i < retireCount; ++i) {
            voices.SetCurrentFrame(blockStartFrame + cursor + retirements_[i].frameOffset);
            voices.RetireVoice(static_cast<VoiceHandle>(retirements_[i].handle));
        }
        cursor = spanEnd;
    }

    voices.SetCurrentFrame(blockStartFrame + rangeEnd);
}

} // namespace svms

#endif
