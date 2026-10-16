#ifndef SVMS_RENDER_SCALAR_H
#define SVMS_RENDER_SCALAR_H

#include "SVMSTypes.h"
#include "SVMSRenderEvent.h"
#if defined(_DEBUG) && defined(_MSC_VER)
#include <crtdbg.h>
#else
#endif
#include "SVMSVoiceManager.h"
#include "SVMSChannelCache.h"
#include "SVMSEnvelope.h"
#include "SVMSPageAllocator.h"
#include "SVMSRenderKernels.h"
#include "SVMSVoiceFilter.h"
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
                                  const int16_t* hilbertData,
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
    if (v.rot)
        sample = RotateVoiceSample(v.stealTailRot[idx], sample, hilbertData,
                                   baseIndex, nextIndex, frac);
    sample = ProcessStealTailFilterSample(v, idx, sample);
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

// One voice's vibrato window of `frameCount` frames: advance-then-use LFO,
// then refresh the phase increment.  `depth` is the channel modulation depth
// and `bendRatio` the exact channel bend ratio for this voice (callers fold
// the per-voice bend scale in).  The scalar/SSE2 whole-voice windows call
// this; it is the single definition of the scalar vibrato math.
inline void AdvanceVibratoSpanRow(VoiceSoA& v, uint32_t idx,
                                  uint32_t frameCount, float depth,
                                  float bendRatio) {
    if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) return;
    const float step = v.vibLfoSteps[idx];
    if (step == 0.0f) return;
    const float depthCents = v.vibLfoToPitchCents[idx];
    uint32_t frames = frameCount;
    const uint32_t delay = v.vibLfoDelays[idx];
    if (delay > 0u) {
        if (delay >= frames) {
            // The LFO holds its zero start value through the delay.
            v.vibLfoDelays[idx] = delay - frames;
            return;
        }
        frames -= delay;
        v.vibLfoDelays[idx] = 0u;
    }
    float phase = v.vibLfoPhases[idx] +
        step * static_cast<float>(frames);
    phase -= floorf(phase);
    v.vibLfoPhases[idx] = phase;

    if (!(depth > 0.0f) || !(depthCents > 0.0f)) {
        if (v.vibLfoModulated[idx]) {
            // Modulation just ended: restore the exact unmodulated
            // increment once instead of rewriting it every span.
            v.vibLfoModulated[idx] = 0u;
            v.phaseIncs[idx] = v.basePhaseIncs[idx] * bendRatio;
        }
        return;
    }
    v.vibLfoModulated[idx] = 1u;
    // SF2 LFOs are positive-going triangles starting at zero.
    const float t = phase * 4.0f;
    const float tri = t < 1.0f ? t : (t < 3.0f ? 2.0f - t : t - 4.0f);
    const float cents = depthCents * depth * tri;
    const float lfoRatio = powf(2.0f, cents / 1200.0f);
    v.phaseIncs[idx] = v.basePhaseIncs[idx] * bendRatio * lfoRatio;
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

// Counters bumped from every render worker. A single shared word per
// counter turned each bump into a cross-core cache-line transfer: under a
// pitch-bend riser (every bend splits each voice of its channel into a new
// whole-voice segment) that cost ~2000 cycles per segment and was most of
// the render time (32k voices, 150k bends/s: 1470% -> 122% of budget once
// removed). Each thread now bumps its own cache-line shard; readers sum.
template <uint32_t N>
struct ShardedCounters {
    static constexpr uint32_t kShards = 64u;
    struct alignas(64) Shard {
        std::atomic<uint64_t> value[N];
    };
    Shard shards[kShards] = {};

    // Thread stacks sit megabytes apart, so a stack address picks a stable
    // per-thread shard with no TLS (unsafe in XP-loaded DLLs) or OS call.
    // Two threads sharing a shard stay correct, only slower.
    static uint32_t ShardIndex() noexcept {
        volatile int probe = 0;
        const uintptr_t address = reinterpret_cast<uintptr_t>(&probe);
        return static_cast<uint32_t>((address >> 20) ^ (address >> 26)) &
               (kShards - 1u);
    }
    Shard& Mine() noexcept { return shards[ShardIndex()]; }
    static void Add(Shard& shard, uint32_t counter, uint64_t amount) noexcept {
        // Plain load+store, not a locked RMW: a shard is one thread's in
        // practice, and telemetry may drop an update when two threads hash to
        // one shard. The lock prefix cost ~20 cycles per bump, twice per
        // fallback span.
        shard.value[counter].store(
            shard.value[counter].load(std::memory_order_relaxed) + amount,
            std::memory_order_relaxed);
    }
    uint64_t Sum(uint32_t counter) const noexcept {
        uint64_t total = 0u;
        for (const Shard& shard : shards)
            total += shard.value[counter].load(std::memory_order_relaxed);
        return total;
    }
};

// Fallback-span telemetry: RenderPrimaryVoiceSpan is the scalar per-voice
// renderer used wherever a vector class kernel refuses (short segments,
// stage/loop guards). Worker threads increment these; the census reports
// deltas so the scalar share can be quantified before optimizing it.
// Counters: 0 = calls, 1 = frames.
inline ShardedCounters<2> g_primarySpans;

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
// ════════════════════════════════════════════════════════════════════════
// Whole-voice whole-block renderer.
//
// One renderer for every voice: each active voice renders a single
// contiguous timeline spanning the whole block, applying only its own
// events at their exact frames.  There are no shared inter-event spans, no
// class buckets, and no snapshots — the per-voice time-chunked
// kernels engage for every voice, which is what event-dense Black MIDI
// material needs (shared spans fragment to 1–3 frames there, even though
// events are sparse per voice).
//
// Block structure:
//   1. Pre-pass (coordinator, single-threaded): walks the event list in
//      ingress order and dispatches every event through the normal driver
//      dispatcher with the render clock set to the event's exact frame.
//      All lifecycle bookkeeping (steal transactions, chain links, churn
//      stats, play indices) therefore happens exactly as today and in the
//      exact same order.  Three renderer hooks observe the pass:
//        - VoiceConfigured: the configured voice begins sounding at this
//          frame (startFrame).
//        - PreTailCapture: the displaced victim's whole render row is
//          snapshotted as a "ghost" that renders [its start, the steal
//          frame) — exactly the samples the victim would have produced —
//          and continues as a normal 64-frame steal tail.  The in-band
//          tail capture is suppressed (its state would be block-start,
//          not steal-frame); the ghost reproduces it from the exact
//          steal-frame state instead.
//        - DeferredRelease: StartRelease keeps its chain/held bookkeeping
//          but defers the audible state flip to the owning worker at the
//          event frame.
//   2. Render (worker pool): disjoint slices of [active voices, ghosts]
//      render whole-block timelines into per-job mix buffers; retirement
//      is recorded per job and applied by the coordinator in exact
//      (frame, capture-position) order — identical to the frame-major oracle.
//   3. Post-pass: deferred-release finalization, render-class refresh,
//      pre-existing steal tails, and ghost tails that outlive the block
//      are adopted into real tail slots.
//
// This is the only production render path.  The frame-major oracle
// (RenderBlockReference) is compiled into test and bench builds as the
// correctness reference.
// ════════════════════════════════════════════════════════════════════════
class RenderScalar;

struct WholeVoiceJobContext {
    RenderScalar* renderer;
    VoiceManager* voices;
    const int16_t* sampleData;
    uint32_t sampleDataFrames;
    uint32_t numFrames;
    uint32_t voiceItemCount;
    uint32_t ghostItemCount;
    uint32_t jobCount;
};

// Steal-tail record captured from a ghost's exact steal-frame state.  The
// VoiceSoA steal-tail arrays are fixed kStealTailReserve slots and must
// never be indexed by ghost row, so ghosts carry their own records.
struct WholeVoiceGhostTail {
    float phase = 0.0f;
    float phaseInc = 0.0f;
    float gain = 0.0f;
    float mixGainL = 0.0f;
    float mixGainR = 0.0f;
    float filterA0 = 0.0f;
    float filterB1 = 0.0f;
    float filterB2 = 0.0f;
    float filterZ1 = 0.0f;
    float filterZ2 = 0.0f;
    uint8_t filterEnabled = 0u;
    uint32_t sampleStart = 0u;
    uint32_t relEnd = 0u;
    uint32_t relLoopS = 0u;
    uint32_t relLoopE = 0u;
    float relLoopSF = 0.0f;
    float relLoopEF = 0.0f;
    uint32_t framesRemaining = 0u;
    uint32_t framesTotal = 0u;
    uint8_t sampleBacked = 0u;
    uint8_t loopEnabled = 0u;
    VoiceRotationState rot{};  // valid only while the pool rotates (v.rot)
};

// Channel-wide row mutation recorded by the whole-voice pre-pass (pitch-bend
// ratio or CC7/10/11 mix-gain fold).  The pre-pass appends one op per
// mutator event in frame order; the plan then regroups them per channel so
// each worker item walks its own row's window and applies the ops between
// spans — exactly where the frame-major renderers fold/bend inline.  A
// per-channel log (not per-voice) is what makes ghosts correct for free: a
// killed or stolen voice's ghost walks the same channel ops inside its own
// [start, death) window, no transfer bookkeeping needed.
struct WholeVoiceChannelOp {
    uint32_t frame;
    // Dispatch ordinal (position in the block's event array).  Same-frame
    // ops apply only to voices launched at a STRICTLY EARLIER ordinal: the
    // frame-major renderers fold/bend exactly the voices that existed when
    // the event dispatched, and a voice born later in the same frame (which
    // reads the fresh channel state at configure time) must not be touched.
    uint32_t ingress;
    uint8_t channel;
    uint8_t kind;   // 0 = bend (a = bendSemitones, b = commonRatio)
                    // 1 = mix-gain (a = mixScaleLeft, b = mixScaleRight)
                    // 2 = vibrato depth (a = channel modDepth)
    float a;
    float b;
};


// One re-excite hit on one live voice, recorded by the whole-voice pre-pass
// at the event's exact frame. `playIndex` pins it to a generation: a handle
// can host a ghost (stolen mid-block) and its successor in one block.
struct WholeVoiceExcite {
    uint32_t frame;
    uint32_t playIndex;
    uint32_t next;   // next hit on the same handle (frame order), UINT32_MAX = end
    // The envelope fields ExciteVoiceRow writes, as they were BEFORE this
    // hit's pre-pass mutation. The pre-pass applies the excite to the row so
    // later launches in the block score steal victims exactly as the
    // frame-major renderers do (an excited voice is in its attack stage, which
    // scores by peak level); the worker puts the first hit's snapshot back
    // before rendering and replays every hit at its exact frame.
    uint8_t savedStage;
    uint32_t savedAttackRemaining;
    uint32_t savedDecayRemaining;
    float savedAttackStep;
};

class RenderScalar {
public:
    RenderScalar();
    ~RenderScalar();
    RenderScalar(const RenderScalar&) = delete;
    RenderScalar& operator=(const RenderScalar&) = delete;

    bool ReserveVoiceCapacity(uint32_t voiceCapacity);
    // Reserve everything the whole-voice renderer needs for this pool, thread
    // count and rotation state, so RenderBlock never allocates (and never
    // falls back) at block time.  Call after ReserveVoiceCapacity,
    // ConfigureRenderThreads and SetPhaseRotationMode; failure is an init
    // error the caller reports.
    bool PrepareWholeVoice(VoiceManager& voices);
    // Bench/test only: shrink the per-stretch op-log budget.
    void SetRowOpBudgetForTest(uint32_t budget) { wvRowOpBudget_ = budget; }
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
            (workerPool_ ? workerPool_->GetAllocatedBytes() : 0u);
        return bytes;
    }

    // hilbertData: optional analytic companion store (Hilbert pair).
    // Overload resolution on the 4th argument (pointer vs frame count)
    // keeps every pre-pair call site source-compatible.
    // channelBusLeft/Right: optional per-MIDI-channel bus planes
    // (SVMSChannelLimiter.h).  Null (default) mixes straight into
    // outputLeft/outputRight — bit-identical legacy behavior.  When set,
    // voices mix into their channel's plane and the caller sums/limits.
    void RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                     const int16_t* sampleData,
                     const int16_t* hilbertData,
                     uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events = nullptr,
                     uint32_t eventCount = 0,
                     bool correctnessMode = false,
                     uint64_t blockStartFrame = 0,
                     float* const* channelBusLeft = nullptr,
                     float* const* channelBusRight = nullptr);
    void RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                     const int16_t* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events = nullptr,
                     uint32_t eventCount = 0,
                     bool correctnessMode = false,
                     uint64_t blockStartFrame = 0,
                     float* const* channelBusLeft = nullptr,
                     float* const* channelBusRight = nullptr);

private:
    // Renders one barrier-free stretch of a block (everything RenderBlock did
    // before barrier splitting). Events are relative to this stretch.
    void RenderBlockStretch(VoiceManager& voices, const ChannelCache& channels,
                            const int16_t* sampleData,
                            const int16_t* hilbertData,
                            uint32_t sampleDataFrames,
                            float* outputLeft, float* outputRight,
                            uint32_t numFrames, const RenderEvent* events,
                            uint32_t eventCount, bool correctnessMode,
                            uint64_t blockStartFrame,
                            float* const* channelBusLeft,
                            float* const* channelBusRight);
    // Events the whole-voice plan cannot fold into a stretch (Reset, the
    // no-op AllNotesOff/AllSoundOff commands, unknown types).  The block is
    // split around them and they dispatch at their exact frame.
    // Channel ops one event records in the whole-voice pre-pass (bend, mix
    // fold, vibrato depth).  The plan sums it to size the op log; the block
    // splitter uses it to keep every stretch inside kWvRowOpBudget, so the
    // log is reserved once at init and never grows on the audio thread.
    static uint32_t WholeVoiceRowOpEstimate(const RenderEvent& event) {
        switch (event.type) {
            case RenderEventType::ControlChange:
                switch (event.data1) {
                    case 1u: case 6u: case 38u: case 96u: case 97u:
                    case 7u: case 10u: case 11u:
                        return 1u;
                    case 121u:
                        return 3u;  // bend-center, mix fold, mod-depth reset
                    default:
                        return 0u;
                }
            case RenderEventType::PitchBend:
            case RenderEventType::ChannelPressure:
                return 1u;
            case RenderEventType::MasterVolume:
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
                return kChannelCount;
            default:
                return 0u;
        }
    }
    static constexpr uint32_t kWvRowOpBudget = 1u << 16;
    uint32_t wvRowOpBudget_ = kWvRowOpBudget;
    static bool IsBarrierEvent(RenderEventType type) {
        switch (type) {
            case RenderEventType::NoteOn:
            case RenderEventType::NoteOff:
            case RenderEventType::ControlChange:
            case RenderEventType::ProgramChange:
            case RenderEventType::PitchBend:
            case RenderEventType::StaleNoteOffBatch:
            case RenderEventType::MasterVolume:
            case RenderEventType::RhythmPart:
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
            case RenderEventType::ChannelPressure:
            case RenderEventType::NoteReExcite:
                return false;
            default:
                return true;
        }
    }
    std::vector<RenderEvent> splitEvents_;
    float* splitBusLeft_[kChannelCount] = {};
    float* splitBusRight_[kChannelCount] = {};
    // ── Whole-voice whole-block renderer ────────────────────────────────
    bool PlanWholeVoiceBlock(VoiceManager& voices, const RenderEvent* events,
                             uint32_t eventCount, uint64_t blockStartFrame,
                             const ChannelParamsSnapshot* channelParams);
    void RenderWholeVoiceBlock(VoiceManager& voices,
                               const int16_t* sampleData,
                               uint32_t sampleDataFrames,
                               float* outputLeft, float* outputRight,
                               uint32_t numFrames, uint64_t blockStartFrame,
                               float* const* channelBusLeft,
                               float* const* channelBusRight);
    bool ReserveWholeVoiceStorage(uint32_t handleCapacity);
    bool EnsureWholeVoiceJobScratch(uint32_t jobCount);
    // Ghost rows and per-job tail stages mirror the pool's rotation state:
    // allocated while v.rot exists, released when rotation is switched off
    // (a stale rot array on a ghost would rotate a coherent voice).
    bool SyncWholeVoiceRotationScratch(bool rotationActive);
    // A row decimation skips this block (never one launched this block).
    bool WholeVoiceRowMuted(uint32_t handle) const {
        return wvDecimating_ && wvMuted_[handle] != 0u &&
               wvStartFrame_[handle] == UINT32_MAX;
    }
    void PartitionMutedRows(uint32_t activeCount, uint32_t itemCount,
                            uint32_t jobCount);
    bool RenderWholeVoiceSegment(VoiceSoA& state, VoiceManager* voices,
                                 uint32_t row, const int16_t* sampleData,
                                 uint32_t sampleDataFrames, float* outL,
                                 float* outR, uint32_t segStart,
                                 uint32_t segFrames, bool isReal,
                                 uint32_t jobIndex, float lfoDepth,
                                 float lfoBendRatio, bool lfoActive);
    bool RenderWholeVoiceSegmentLfo(VoiceSoA& state, VoiceManager* voices,
                                    uint32_t row, const int16_t* sampleData,
                                    uint32_t sampleDataFrames, float* outL,
                                    float* outR, uint32_t segStart,
                                    uint32_t segFrames, bool isReal,
                                    uint32_t jobIndex, float lfoDepth,
                                    float lfoBendRatio, bool lfoActive);
    bool RenderWholeVoiceBatch(VoiceSoA& state, VoiceManager& voices,
                               const uint32_t* handles,
                               uint32_t handleCount,
                               VoiceRenderClass renderClass,
                               const int16_t* sampleData,
                               uint32_t sampleDataFrames,
                               const IndexedJobMix& mix,
                               uint32_t segStart, uint32_t segFrames,
                               uint32_t jobIndex);
    void CaptureGhostTail(uint32_t ghost);
    void RenderGhostTailSpan(uint32_t ghost, VoiceSoA& scratch,
                             const int16_t* sampleData,
                             uint32_t sampleDataFrames, float* outL,
                             float* outR, uint32_t frameStart,
                             uint32_t frameCount);
    static void WholeVoiceVoiceConfiguredHook(VoiceHandle handle,
                                              void* userData);
    // A partial release of a merged voice forks a twin that must render the
    // source's trajectory from the source's own start (see
    // VoiceManager::ForkReleasedUnits): same start frame, same op ingress.
    static void WholeVoiceVoiceForkedHook(VoiceHandle twin, VoiceHandle source,
                                          void* userData);
    uint32_t WholeVoiceGhostLimit() const;
    static void WholeVoiceReExciteHook(VoiceHandle handle, void* userData);
    bool EnsureWholeVoiceExciteStorage(uint32_t handleCapacity);
    // First entry at or after `index` that belongs to this generation.
    uint32_t WholeVoiceSkipForeignExcites(uint32_t index,
                                          uint32_t playIndex) const {
        while (index != UINT32_MAX && wvExcites_[index].playIndex != playIndex)
            index = wvExcites_[index].next;
        return index;
    }
    static void WholeVoicePreTailCaptureHook(VoiceHandle handle,
                                             void* userData);
    static void WholeVoiceDeferredReleaseHook(VoiceHandle handle,
                                              void* userData);
    static void WholeVoiceSilenceVoiceHook(VoiceHandle handle,
                                           void* userData);
    static void WholeVoiceSilenceTailHook(uint32_t slot, void* userData);
    static void WholeVoiceMixOpHook(uint8_t channel, float mixScaleLeft,
                                    float mixScaleRight, void* userData);
    static void WholeVoiceBendOpHook(uint8_t channel, float bendSemitones,
                                     float commonRatio, void* userData);
    static void WholeVoiceVibratoOpHook(uint8_t channel, float modDepth,
                                        void* userData);
    void DropWholeVoiceReleaseOp(VoiceHandle handle);
    bool EnsureWholeVoiceRowOpStorage(uint32_t opCount);
    void ApplyWholeVoiceRowOp(VoiceSoA& state, uint32_t row,
                              const WholeVoiceChannelOp& op);
    static void WholeVoiceJobThunk(uint32_t jobIndex,
                                   const IndexedJobMix& mix,
                                   uint32_t frameCount, void* userData);

    VoiceSoA wvGhostState_;                  // displaced-voice snapshots
    WholeVoiceGhostTail* wvGhostTails_ = nullptr;
    VoiceSoA* wvTailScratch_ = nullptr;      // per-job single-row tail stage
    // Hilbert companion store for the block being rendered (null = none).
    const int16_t* wvHilbertData_ = nullptr;
    // Scalar/SSE2 vibrato: this block renders every voice in fixed LFO windows (global event/64-frame boundaries) instead of the AVX2
    // kernels' internal windows.  wvLfoBounds_ holds each window's end frame.
    bool wvScalarVibrato_ = false;
    // Scalar/SSE2 block that starts with no modulated channel: the sparse
    // loop never enters vibrato mode for it (the flag is sampled at block
    // start), so a CC1 arriving mid-block modulates nothing until the next.
    bool wvScalarNoVibrato_ = false;
    std::vector<uint32_t> wvLfoBounds_;
    uint32_t wvGhostCapacity_ = 0u;
    uint32_t wvGhostCount_ = 0u;
    uint32_t* wvGhostStartFrame_ = nullptr;
    uint32_t* wvGhostDeathFrame_ = nullptr;
    uint32_t* wvGhostReleaseFrame_ = nullptr;
    // Per ghost: 1 = All-Sound-Off kill (renders to its death frame with no
    // fade tail), 0 = steal victim (64-frame tail fade from death state).
    uint8_t* wvGhostNoTail_ = nullptr;
    // Per-channel exact-frame row ops (bend / mix-gain / vibrato depth).
    // wvRowOps_ is the dispatch-order log; wvChanOps_ is the per-channel
    // regrouped copy the worker items walk, bracketed by wvChanOpStart_
    // (CSR, frame-sorted within each channel).
    WholeVoiceChannelOp* wvRowOps_ = nullptr;
    WholeVoiceChannelOp* wvChanOps_ = nullptr;
    // Per-channel channel state at block START, captured by the pre-pass
    // before any event dispatches: the worker items' depth/bend cursors
    // start here and track the op log.  (The live snapshot is block-FINAL
    // state and would mislead rows whose ops land mid-block.)
    float wvChanStartDepth_[kChannelCount] = {};
    float wvChanStartBendRatio_[kChannelCount] = {};
    float wvChanStartBendCents_[kChannelCount] = {};
    // Per-channel vibrato relevance (block-start depth > 0 or a vibrato
    // depth op lands mid-block): rows on irrelevant channels skip every
    // LFO check in the item loop, keeping non-vibrato blocks at their
    // pre-vibrato cost.
    bool wvChanVibratoActive_[kChannelCount] = {};
    uint32_t wvRowOpCapacity_ = 0u;
    uint32_t wvRowOpCount_ = 0u;
    uint64_t wvRowOpChannelMask_ = 0u;   // channels touched by this block's ops
    uint32_t wvEventOrdinal_ = 0u;       // dispatch position of the current event
    // CSR bracket for wvChanOps_; one extra slot so a defensive clamp of a
    // corrupt channel to kChannelCount yields an empty (safe) op range.
    uint32_t wvChanOpStart_[kChannelCount + 2];
    // Per steal-tail slot: frame an in-block All Sound Off killed the tail
    // (UINT32_MAX = alive or dead before this block).  kStealTailReserve is
    // a small compile-time constant, so a fixed member array is fine.
    uint32_t wvTailKillFrame_[kStealTailReserve];
    // Re-excite hits (see WholeVoiceExcite): the pre-pass appends in dispatch
    // (= frame) order; wvExciteHead_/Tail_ are per-handle list ends, reset
    // only for the handles a block touched.
    std::vector<WholeVoiceExcite> wvExcites_;
    std::vector<uint32_t> wvExciteTouched_;
    uint32_t* wvExciteHead_ = nullptr;
    uint32_t* wvExciteTail_ = nullptr;
    uint32_t wvExciteCapacity_ = 0u;
    uint32_t* wvStartFrame_ = nullptr;       // per handle; UINT32_MAX = from 0
    uint32_t* wvReleaseFrame_ = nullptr;     // per handle; UINT32_MAX = none
    // Dispatch ordinal of the launch that created each handle's current
    // generation (UINT32_MAX = carried over from a previous block, so every
    // op in the block applies).  Qualifies same-frame ops — see
    // WholeVoiceChannelOp::ingress.
    uint32_t* wvStartIngress_ = nullptr;
    uint32_t* wvGhostStartIngress_ = nullptr;
    // Handle a ghost was captured from: its re-excite hits live on that
    // handle's list, filtered by the ghost's playIndex.
    uint32_t* wvGhostHandle_ = nullptr;
    uint32_t wvHandleCapacity_ = 0u;         // handle-range size of the above
    uint32_t* wvOpHandles_ = nullptr;
    uint32_t* wvOpFrames_ = nullptr;
    // Per handle: position of its entry in wvOpHandles_ (valid only while
    // wvOpHandles_[pos] == handle && pos < wvOpCount_; never cleared).
    uint32_t* wvOpIndex_ = nullptr;
    uint32_t wvOpCapacity_ = 0u;
    uint32_t wvOpCount_ = 0u;
    uint32_t wvEventFrame_ = 0u;
    VoiceManager* wvPlanVoices_ = nullptr;
    uint32_t* wvSliceHandles_ = nullptr;
    SpanRetirement* wvJobRetirements_ = nullptr;
    // Per-job retirement counts, one cache line apart: kernels bump them on
    // every retirement from all workers at once.
    static constexpr uint32_t kWvRetireCountStride = 16u;
    uint32_t* wvJobRetireCounts_ = nullptr;
    static constexpr uint32_t kWvJobsPerThread = 4u;
    static constexpr uint32_t kWvMinItemsPerJob = 64u;
    uint32_t wvJobCapacity_ = 0u;
    uint32_t wvJobScratchCapacity_ = 0u;     // scratch stride wvJobRetirements_ was sized for
    uint64_t wvGhostOverflowCount_ = 0u;
    // Optional per-block ghost budget; 0 = unbounded (default).
    uint32_t ghostBudget_ = 0u;
    uint32_t wholeVoiceBlocks_ = 0u;

public:

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    // Test-only oracle: the pre-optimization frame-major renderer.  It is
    // intentionally excluded from production builds so the DLL carries only
    // the whole-voice renderer, while differential tests can still prove
    // state and waveform equivalence.
    void RenderBlockReference(VoiceManager& voices, const ChannelCache& channels,
                     const int16_t* sampleData,
                     const int16_t* hilbertData,
                     uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events = nullptr,
                     uint32_t eventCount = 0,
                     bool correctnessMode = false,
                     uint64_t blockStartFrame = 0,
                     float* const* channelBusLeft = nullptr,
                     float* const* channelBusRight = nullptr);
    void RenderBlockReference(VoiceManager& voices, const ChannelCache& channels,
                     const int16_t* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events = nullptr,
                     uint32_t eventCount = 0,
                     bool correctnessMode = false,
                     uint64_t blockStartFrame = 0,
                     float* const* channelBusLeft = nullptr,
                     float* const* channelBusRight = nullptr);
#endif

    // Note-on re-excite without a NoteReExcite event (the offline synth
    // restarts voices from inside its NoteOn dispatch) mutates live voices at
    // dispatch. The whole-voice pre-pass records those hits and replays them
    // per voice.
    void SetReExciteMode(bool enabled) { reExciteMode_ = enabled; }

    // Optional ghost budget (0 = unbounded): caps ghosts captured per
    // whole-voice block. Beyond the cap, displaced/killed voices die without
    // their pre-steal render pass (same path as capacity overflow).
    void SetGhostBudget(uint32_t budget) { ghostBudget_ = budget; }
    void SetEventDispatcher(EventDispatcher dispatcher, void* userData);
    void SetEventBatchDispatcher(EventBatchDispatcher dispatcher, void* userData);
    uint32_t GetWholeVoiceBlocksForTest() const { return wholeVoiceBlocks_; }
    // Path telemetry for the driver census: which renderer(s) the last block
    // used. Bit0 whole-voice, bit8 = vibrato active, bit9 = the block was
    // split around a barrier/budget point. Bits 1/2 (dense/sparse) are retired.
    uint32_t GetLastRenderPaths() const { return lastRenderPaths_; }
    // Stage-0 instrumentation for the single-renderer project: how often each
    // path ran and why whole-voice planning was skipped or refused.
    enum RefusalReason : uint32_t {
        kRefuseRotation = 0, kRefuseEventType, kRefuseNoDispatcher,
        kRefuseReservation, kRefuseScalarVibrato, kRefuseForced,
        kRefuseReasonCount };
    uint64_t GetTotalRefusals() const {
        uint64_t total = 0u;
        for (uint32_t reason = 0u; reason < kRefuseReasonCount; ++reason)
            total += refusalCounts_[reason];
        return total;
    }
    uint64_t GetRefusalCount(uint32_t reason) const {
        return reason < kRefuseReasonCount ? refusalCounts_[reason] : 0u;
    }
    uint64_t GetPathBlocks(uint32_t path) const {  // 0 wv, 1 dense, 2 sparse
        return path < 3u ? pathBlocks_[path] : 0u;
    }
    // Voice decimation (opt-in).  Above the tier voice counts the quietest
    // voices (by velocity, ranked at block start) stop mixing for the block;
    // they still advance phase/envelope/retirement, and voices launched in the
    // block always mix.  Off by default: every voice mixes.
    void SetDecimationEnabled(bool enabled) {
        decimationEnabled_.store(enabled, std::memory_order_relaxed);
    }
    // Opt-in: rank by audibility (envelope level x output gain, the steal
    // score's level term) instead of velocity.
    void SetDecimationByLevel(bool enabled) {
        decimationByLevel_.store(enabled, std::memory_order_relaxed);
    }
    // Opt-in (needs decimation on): a releasing voice that has been muted for
    // this many consecutive blocks is retired instead of advancing forever.
    // 0 = off.
    void SetMutedTailCullBlocks(uint32_t blocks) {
        mutedCullBlocks_.store(blocks, std::memory_order_relaxed);
    }
    bool GetDecimationEnabled() const {
        return decimationEnabled_.load(std::memory_order_relaxed);
    }
    uint32_t DecimationStep(uint32_t activeCount) const {
        if (!decimationEnabled_.load(std::memory_order_relaxed)) return 1u;
        if (activeCount <= decimationTiers_[0]) return 1u;
        if (activeCount <= decimationTiers_[1]) return 2u;
        if (activeCount <= decimationTiers_[2]) return 4u;
        if (activeCount <= decimationTiers_[3]) return 8u;
        return 16u;
    }
    void SetDecimationTiersForTest(uint32_t t1, uint32_t t2, uint32_t t3,
                                   uint32_t t4) {
        decimationTiers_[0] = t1; decimationTiers_[1] = t2;
        decimationTiers_[2] = t3; decimationTiers_[3] = t4;
    }
    // Diagnostics-only whole-voice phase accumulators (rdtsc, caller-thread
    // wall cycles; read racily from telemetry — fine for profiling).
    uint64_t wvPlanCycles_ = 0u;
    uint64_t wvParallelJobs_ = 0u;
    uint64_t wvSerialJobs_ = 0u;
    uint64_t wvJobCycles_ = 0u;
    uint64_t wvPostCycles_ = 0u;
    // Segment-path diagnostics: calls into RenderWholeVoiceSegment, cycles
    // inside it, how many took the batch-kernel fast path vs the scalar
    // RenderPrimaryVoiceSpan fallback, and frames rendered per path.
    // Whole-voice segment stats. Bumped per segment by the job that owns
    // them: one cache line per job, no atomics (a shared counter cost
    // ~2000 cycles per segment under bend risers, see ShardedCounters).
    // Read on the audio thread after the jobs finish.
    enum WvSegCounter : uint32_t {
        kWvSegCalls, kWvSegCycles, kWvSegKernelOk, kWvSegFallback,
        kWvSegKernelFrames, kWvSegFallbackFrames, kWvSegCounterCount
    };
    struct alignas(64) WvJobSegStats {
        uint64_t value[kWvSegCounterCount];
    };
    WvJobSegStats* wvJobSeg_ = nullptr;  // wvJobCapacity_ entries
    uint64_t wvSegRetired_[kWvSegCounterCount] = {};  // from freed arrays
    uint64_t WvSegSum(WvSegCounter counter) const {
        uint64_t total = wvSegRetired_[counter];
        for (uint32_t job = 0u; wvJobSeg_ && job < wvJobCapacity_; ++job)
            total += wvJobSeg_[job].value[counter];
        return total;
    }
    uint64_t GetWvPlanCycles() const { return wvPlanCycles_; }
    // Whole-voice job stages that ran on the worker pool vs serially on the
    // audio thread (pool refused or single job).
    uint64_t GetWvParallelJobs() const { return wvParallelJobs_; }
    uint64_t GetWvSerialJobs() const { return wvSerialJobs_; }
    uint64_t GetWvJobCycles() const { return wvJobCycles_; }
    uint64_t GetWvPostCycles() const { return wvPostCycles_; }
    uint64_t GetWvSegCalls() const { return WvSegSum(kWvSegCalls); }
    uint64_t GetWvSegCycles() const { return WvSegSum(kWvSegCycles); }
    uint64_t GetWvSegKernelOk() const { return WvSegSum(kWvSegKernelOk); }
    uint64_t GetWvSegFallback() const { return WvSegSum(kWvSegFallback); }
    uint64_t GetWvSegKernelFrames() const {
        return WvSegSum(kWvSegKernelFrames);
    }
    uint64_t GetWvSegFallbackFrames() const {
        return WvSegSum(kWvSegFallbackFrames);
    }
    static void GetPrimarySpanTotals(uint64_t& calls, uint64_t& frames) {
        calls = g_primarySpans.Sum(0u);
        frames = g_primarySpans.Sum(1u);
    }
    void GetLastPlanRefusal(uint8_t& type, uint8_t& data1) const {
        type = lastRefusalType_;
        data1 = lastRefusalData1_;
    }
    bool SetRenderBackend(RenderBackend backend);
    RenderBackend GetRenderBackend() const { return kernelSet_->backend; }
    const char* GetRenderBackendName() const { return kernelSet_->name; }
    // Re-applies the configured thread-affinity mode to the worker pool.
    void ApplyWorkerAffinity() {
        if (workerPool_) workerPool_->ApplyAffinity();
    }

private:
private:
    void RenderBlockFrameMajor(VoiceManager& voices, const ChannelCache& channels,
                     const int16_t* sampleData, const int16_t* hilbertData,
                     uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events, uint32_t eventCount,
                     bool correctnessMode, uint64_t blockStartFrame,
                     float* const* channelBusLeft = nullptr,
                     float* const* channelBusRight = nullptr);
    EventDispatcher dispatcher_;
    EventBatchDispatcher batchDispatcher_;
    void* dispatcherUserData_;
    const RenderKernelSet* kernelSet_;
    uint32_t* classChanges_;
    alignas(64) uint32_t tailFrameCounts_[kStealTailReserve];
    SpanRetirement* retirements_;
    uint32_t scratchCapacity_;
    RenderWorkerPool* workerPool_;
    bool reExciteMode_ = false;
    uint32_t lastRenderPaths_ = 0u;
    uint64_t refusalCounts_[kRefuseReasonCount] = {};
    uint64_t pathBlocks_[3] = {};
    std::atomic<bool> decimationEnabled_{false};
    std::atomic<bool> decimationByLevel_{false};
    std::atomic<uint32_t> mutedCullBlocks_{0u};
    uint32_t wvCullBlocks_ = 0u;         // mutedCullBlocks_ latched per block
    std::vector<uint32_t> wvMutedSince_; // per handle: first block of the muted run
    std::vector<uint32_t> wvMutedLast_;  // per handle: last block it was muted
    uint32_t decimationTiers_[4] = {kDecimationTier1, kDecimationTier2,
                                    kDecimationTier3, kDecimationTier4};
    bool wvDecimating_ = false;          // this block has muted rows
    bool wvMutedDirty_ = false;          // wvMuted_ holds stale flags
    std::vector<uint8_t> wvMuted_;       // per handle: 1 = skip mixing
    std::vector<uint32_t> wvPartMixed_;   // decimation partition scratch
    std::vector<uint32_t> wvPartMuted_;
    std::vector<uint8_t> wvDecimKey_;     // by-level rank key per active slot
    std::vector<uint32_t> wvDecimRank_;  // scratch for the velocity ranking
    // The event that made PlanWholeVoiceBlock refuse the last block: type
    // value + data1 (controller number for CC events). Diagnostic only; the
    // plan cannot refuse on event type any more (barriers are split out).
    uint8_t lastRefusalType_ = 0u;
    uint8_t lastRefusalData1_ = 0u;
};

inline RenderScalar::RenderScalar()
    : dispatcher_(nullptr), batchDispatcher_(nullptr), dispatcherUserData_(nullptr),
      kernelSet_(&SelectBestRenderKernelSet()), classChanges_(nullptr),
      retirements_(nullptr), scratchCapacity_(0u),
      workerPool_(new (std::nothrow) RenderWorkerPool())
      {
    const bool reserved = ReserveVoiceCapacity(kMaxVoicesDefault);
    assert(reserved);
    (void)reserved;
}

inline RenderScalar::~RenderScalar() {
    delete workerPool_;
    _aligned_free(classChanges_);
    _aligned_free(retirements_);
    _aligned_free(wvExciteHead_);
    _aligned_free(wvExciteTail_);
    _aligned_free(wvStartFrame_);
    _aligned_free(wvReleaseFrame_);
    _aligned_free(wvStartIngress_);
    _aligned_free(wvSliceHandles_);
    _aligned_free(wvOpHandles_);
    _aligned_free(wvOpFrames_);
    _aligned_free(wvOpIndex_);
    _aligned_free(wvGhostStartFrame_);
    _aligned_free(wvGhostHandle_);
    _aligned_free(wvGhostStartIngress_);
    _aligned_free(wvGhostDeathFrame_);
    _aligned_free(wvGhostReleaseFrame_);
    _aligned_free(wvGhostNoTail_);
    _aligned_free(wvRowOps_);
    _aligned_free(wvChanOps_);
    _aligned_free(wvJobRetirements_);
    _aligned_free(wvJobRetireCounts_);
    _aligned_free(wvJobSeg_);
    delete[] wvGhostTails_;
    delete[] wvTailScratch_;
}

inline bool RenderScalar::ConfigureRenderThreads(
    uint32_t totalRenderThreads, uint32_t maximumBlockFrames) {
    if (!workerPool_) return totalRenderThreads <= 1u;
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
    if (!classChanges || !retirements) {
        _aligned_free(classChanges);
        _aligned_free(retirements);
        return false;
    }
    _aligned_free(classChanges_);
    _aligned_free(retirements_);
    classChanges_ = classChanges;
    retirements_ = retirements;
    scratchCapacity_ = voiceCapacity;
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
                                        const int16_t* sampleData,
                                        const int16_t* hilbertData,
                                        uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                        const RenderEvent* events, uint32_t eventCount,
                                        bool correctnessMode,
                                        uint64_t blockStartFrame,
                                        float* const* channelBusLeft,
                                        float* const* channelBusRight) {
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

    uint32_t step = DecimationStep(voices.activeCount_);

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
            // Exact-frame CC dispatch may have marked channels stale; fold
            // before this frame's voices render (reference path).
            voices.FoldStaleChannelMixGains();
        }
        if (voices.activeCount_ > scratchCapacity_ &&
            !ReserveVoiceCapacity(voices.activeCount_)) return;

        // ── 2. Determine decimation step ──────────────────────────────
        // At step=1 every voice mixes: mixLimit and the newborn check are
        // dead work, so they are skipped entirely (fullMix fast path).
        // mixLimit is hoisted per-sample; staleness by one retirement is
        // harmless (at most one extra voice mixes for one sample).
        uint32_t stepNow = DecimationStep(voices.activeCount_);
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
            float* tailOutL = outL;
            float* tailOutR = outR;
            if (channelBusLeft != nullptr) {
                const uint32_t channel = v.stealTailChannel[tailSlot];
                const uint32_t safeChannel =
                    channel < kChannelCount ? channel : 0u;
                tailOutL = channelBusLeft[safeChannel] + f;
                tailOutR = channelBusRight[safeChannel] + f;
            }
            RenderStealTailSample(v, tailSlot, sampleData, hilbertData,
                                  sampleDataFrames, tailOutL, tailOutR);
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
            // Sample-fetch coordinates, captured where the fetch happens so
            // the Hilbert companion can be read with identical index math at
            // the mix site below (the goto skips the envelope, so locals
            // from the fetch block are not in scope there).
            uint32_t fetchBaseIndex = 0u;
            uint32_t fetchNextIndex = 0u;
            float fetchFrac = 0.0f;
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
                        sample = InterpolateSample(
                            sampleData, baseIdx, nextIdx, frac);
                        fetchBaseIndex = baseIdx;
                        fetchNextIndex = nextIdx;
                        fetchFrac = frac;
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
                if (v.rot)
                    rotated = RotateVoiceSample(v.rot[idx], rotated,
                                                hilbertData, fetchBaseIndex,
                                                fetchNextIndex, fetchFrac);
                rotated = ProcessVoiceFilterSample(v, idx, rotated);
                const float scaled = rotated * gain * stealFadeIn;
                if (channelBusLeft != nullptr) {
                    // Bus mode: mix into this voice's channel plane.
                    const uint32_t channel = v.channel[idx];
                    const uint32_t safeChannel =
                        channel < kChannelCount ? channel : 0u;
                    channelBusLeft[safeChannel][f] +=
                        scaled * v.mixGainL[idx];
                    channelBusRight[safeChannel][f] +=
                        scaled * v.mixGainR[idx];
                } else {
                    *outL += scaled * v.mixGainL[idx];
                    *outR += scaled * v.mixGainR[idx];
                }
            }

            const bool thresholdReleaseFinished = isReleased &&
                v.releaseSamplesRemaining[idx] == UINT32_MAX &&
                gain < VoiceRetireThreshold();
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
                                        uint64_t blockStartFrame,
                                        float* const* channelBusLeft,
                                        float* const* channelBusRight) {
    RenderBlockReference(voices, channels, sampleData, nullptr,
                         sampleDataFrames, outputLeft, outputRight, numFrames,
                         cfg, events, eventCount, correctnessMode,
                         blockStartFrame, channelBusLeft, channelBusRight);
}

inline void RenderScalar::RenderBlockReference(VoiceManager& voices,
                                        const ChannelCache& channels,
                                        const int16_t* sampleData,
                                        const int16_t* hilbertData,
                                        uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames,
                                        const RuntimeConfigSnapshot& cfg,
                                        const RenderEvent* events,
                                        uint32_t eventCount,
                                        bool correctnessMode,
                                        uint64_t blockStartFrame,
                                        float* const* channelBusLeft,
                                        float* const* channelBusRight) {
    RenderBlockFrameMajor(voices, channels, sampleData, hilbertData,
                          sampleDataFrames,
                          outputLeft, outputRight, numFrames, cfg, events,
                          eventCount, correctnessMode, blockStartFrame,
                          channelBusLeft, channelBusRight);
}
#endif

// Render an independent stolen-voice continuation across an event-free span.
// All state is held in locals and committed once.
inline void RenderStealTailSpan(VoiceSoA& v, uint32_t idx,
                                const int16_t* sampleData,
                                const int16_t* hilbertData,
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
        if (v.rot)
            sample = RotateVoiceSample(v.stealTailRot[idx], sample,
                                       hilbertData, baseIndex, nextIndex,
                                       frac);
        sample = ProcessStealTailFilterSample(v, idx, sample);
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
// Closed-form release advance for a MUTED (decimated) releasing voice: nothing
// is mixed, so the per-frame gain recurrence is replaced by one powf and the
// retire frame is found arithmetically. Returns the 0-based frame the voice
// retires at inside `frames` (UINT32_MAX = survives) and writes the number of
// frames consumed. Differs from the sequential loop by float rounding of the
// gain (~1e-6 relative) and, for threshold-governed tails, by at most one
// frame of retire timing; counted tails retire on the exact frame. Callers
// only use it when decay is in (0, 1) and exactSilentAdvance is off.
inline uint32_t SilentReleaseAdvance(float& gain, uint32_t& remaining,
                                     float decay, uint32_t frames,
                                     uint32_t& consumed) {
    if (remaining == 0u) {
        consumed = 1u;
        return 0u;
    }
    if (remaining != UINT32_MAX) {
        if (remaining <= frames) {
            consumed = remaining;
            gain *= powf(decay, static_cast<float>(consumed));
            remaining = 0u;
            return consumed - 1u;
        }
        consumed = frames;
        gain *= powf(decay, static_cast<float>(frames));
        remaining -= frames;
        return UINT32_MAX;
    }
    const float threshold = VoiceRetireThreshold();
    // Survivors (the overwhelming case) cost one logf + one expf: the tail
    // outlives the block exactly when gain * decay^frames is still at or
    // above the floor; only a retiring tail needs the quotient.
    const float logDecay = logf(decay);
    const float survived = gain * expf(static_cast<float>(frames) * logDecay);
    if (survived >= threshold) {
        consumed = frames;
        gain = survived;
        return UINT32_MAX;
    }
    uint32_t needed = 1u;
    if (gain * decay >= threshold) {
        const float quotient = logf(threshold / gain) / logDecay;
        needed = quotient >= 4.0e9f ? UINT32_MAX
                                    : static_cast<uint32_t>(quotient) + 1u;
    }
    if (needed > frames) needed = frames;
    consumed = needed;
    gain *= expf(static_cast<float>(needed) * logDecay);
    return needed - 1u;
}

inline uint32_t RenderPrimaryVoiceSpan(VoiceSoA& v, uint32_t idx,
                                       const int16_t* sampleData,
                                       const int16_t* hilbertData,
                                       uint32_t sampleDataFrames,
                                       float* outputLeft, float* outputRight,
                                       uint32_t frameStart, uint32_t frameCount,
                                       uint32_t mixedFrameCount,
                                       bool exactSilentAdvance = false) {
    {
        auto& shard = g_primarySpans.Mine();
        g_primarySpans.Add(shard, 0u, 1u);
        g_primarySpans.Add(shard, 1u, frameCount);
    }

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
    if (!released && stage == 3u && loop && v.rot == nullptr) {
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
                    if (phase >= relLoopEF) {  // a long span may end with the wrap pending: carry the overflow
                        float wrapOverflow = phase - relLoopEF;
                        const float wrapLength = relLoopEF - relLoopSF;
                        if (wrapLength > 0.0f && wrapOverflow >= wrapLength)
                            wrapOverflow -= floorf(wrapOverflow / wrapLength) * wrapLength;
                        phase = relLoopSF + wrapOverflow;
                        baseOffset = static_cast<uint32_t>(phase);
                    }
                    if (baseOffset + 1u >= relEnd) {
                        phase = relLoopSF;
                        baseOffset = relLoopS;
                    }
                    uint32_t nextRel = baseOffset + 1u;
                    if (nextRel >= relLoopE) nextRel = relLoopS;
                    const float frac = phase - static_cast<float>(baseOffset);
                    float sample = InterpolateSample(
                        sampleData, sampleStart + baseOffset,
                        sampleStart + nextRel, frac);
                    sample = ProcessVoiceFilterSample(v, idx, sample);
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
                        float sample = InterpolateSample(
                            sampleData, sampleStart + baseOffset,
                            sampleStart + baseOffset + 1u, frac);
                        sample = ProcessVoiceFilterSample(v, idx, sample);
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
                // A muted/silent advance moves authoritative state without
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
                sample = ProcessVoiceFilterSample(v, idx, sample);
                if (v.rot)
                    sample = RotateVoiceSample(v.rot[idx], sample, hilbertData,
                                               baseIndex, nextIndex, frac);
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

    // A silent advance moves the authoritative steal/envelope state without
    // producing samples. Releasing loops dominate chopped-note workloads, so
    // keep them out of the generic sample/envelope machine: only the phase,
    // release gain/countdown and exact retirement frame can change here.
    if (released && loop && fadeRemaining == 0u &&
        mixedFrameCount == 0u) {
        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
        const float releaseDecay = v.releaseDecay[idx];
        uint32_t retiredAt = UINT32_MAX;

        if (!exactSilentAdvance && releaseDecay > 0.0f &&
            releaseDecay < 1.0f) {
            if (static_cast<uint32_t>(phase) + 1u >= relEnd)
                phase = relLoopSF;
            uint32_t consumed = 0u;
            retiredAt = SilentReleaseAdvance(gain, releaseRemaining,
                                             releaseDecay, frameCount,
                                             consumed);
            phase += phaseStep * static_cast<float>(consumed);
            if (phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                const float loopLength = relLoopEF - relLoopSF;
                if (loopLength > 0.0f && overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }
            v.phases[idx] = phase;
            v.currentGain[idx] = gain;
            v.releaseSamplesRemaining[idx] = releaseRemaining;
            return retiredAt;
        }

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
                 gain < VoiceRetireThreshold())) {
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

        if (!exactSilentAdvance && releaseDecay > 0.0f &&
            releaseDecay < 1.0f && phaseStep > 0.0f) {
            // The sample runs out (base + 1 >= relEnd) at the first frame
            // whose phase reaches relEnd - 1; the release retires on its own
            // schedule. The earlier of the two wins.
            const float lastPhase = static_cast<float>(relEnd - 1u);
            uint32_t endFrame = UINT32_MAX;
            if (phase >= lastPhase) {
                endFrame = 0u;
            } else {
                const float frames = ceilf((lastPhase - phase) / phaseStep);
                if (frames < 4.0e9f) endFrame = static_cast<uint32_t>(frames);
            }
            uint32_t consumed = 0u;
            const uint32_t releaseAt = SilentReleaseAdvance(
                gain, releaseRemaining, releaseDecay, frameCount, consumed);
            if (endFrame < frameCount &&
                (releaseAt == UINT32_MAX || endFrame <= releaseAt)) {
                retiredAt = endFrame;
                consumed = endFrame;
            } else {
                retiredAt = releaseAt;
            }
            phase += phaseStep * static_cast<float>(consumed);
            v.phases[idx] = phase;
            v.currentGain[idx] = gain;
            v.releaseSamplesRemaining[idx] = releaseRemaining;
            return retiredAt;
        }

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
                 gain < VoiceRetireThreshold())) {
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
        mixedFrameCount == frameCount && (stage == 1u || stage == 2u) &&
        v.rot == nullptr) {
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
            float sample = InterpolateSample(
                sampleData, sampleStart + baseOffset, sampleStart + nextRel, frac);
            sample = ProcessVoiceFilterSample(v, idx, sample);

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
            if (v.rot)
                sampleR = RotateVoiceSample(v.rot[idx], sampleR, hilbertData,
                                            sampleStart + baseOffset,
                                            sampleStart + nextRel, frac);
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
        mixedFrameCount == frameCount && v.rot == nullptr) {
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
            float sample = InterpolateSample(
                sampleData, sampleStart + baseOffset, sampleStart + nextRel, frac);
            sample = ProcessVoiceFilterSample(v, idx, sample);

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
            if (v.rot)
                sampleR = RotateVoiceSample(v.rot[idx], sampleR, hilbertData,
                                            sampleStart + baseOffset,
                                            sampleStart + nextRel, frac);
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
                (releaseRemaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
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
        // Fetch coordinates for the Hilbert companion (the mix block below
        // runs after the phase advance, so the fetch locals are out of scope).
        uint32_t fetchBaseIndex = 0u;
        uint32_t fetchNextIndex = 0u;
        float fetchFrac = 0.0f;
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
            sample = InterpolateSample(
                sampleData, baseIndex, nextIndex, frac);
            fetchBaseIndex = baseIndex;
            fetchNextIndex = nextIndex;
            fetchFrac = frac;
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
            if (v.rot)
                sampleR = RotateVoiceSample(v.rot[idx], sampleR, hilbertData,
                                            fetchBaseIndex, fetchNextIndex,
                                            fetchFrac);
            sampleR = ProcessVoiceFilterSample(v, idx, sampleR);
            const float scaled = sampleR * gain * fade;
            outputLeft[frameStart + n] += scaled * mixL;
            outputRight[frameStart + n] += scaled * mixR;
        }

        const bool thresholdFinished = released && releaseRemaining == UINT32_MAX &&
                                       gain < VoiceRetireThreshold();
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

inline void RenderScalar::RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                                      const int16_t* sampleData,
                                      uint32_t sampleDataFrames,
                                      float* outputLeft, float* outputRight,
                                      uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                      const RenderEvent* events, uint32_t eventCount,
                                      bool correctnessMode,
                                      uint64_t blockStartFrame,
                                      float* const* channelBusLeft,
                                      float* const* channelBusRight) {
    RenderBlock(voices, channels, sampleData, nullptr, sampleDataFrames,
                outputLeft, outputRight, numFrames, cfg, events, eventCount,
                correctnessMode, blockStartFrame, channelBusLeft,
                channelBusRight);
}

inline void RenderScalar::RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                                      const int16_t* sampleData, const int16_t* hilbertData,
                                      uint32_t sampleDataFrames,
                                      float* outputLeft, float* outputRight,
                                      uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                      const RenderEvent* events, uint32_t eventCount,
                                      bool correctnessMode,
                                      uint64_t blockStartFrame,
                                      float* const* channelBusLeft,
                                      float* const* channelBusRight) {
    (void)cfg;
    voices.SetStealKeyBackend(kernelSet_->backend);
    // Gains changed during the previous block's render; the volatile steal
    // heap must rebuild at this block's first steal.  One rebuild per block
    // (not per event frame — see PopStealCandidate).
    voices.MarkStealScoresDirty();
    // Batched CC mix folds (MarkChannelMixStale) land before whole-voice
    // planning and before any voice renders this block. Mid-block CCs fold at
    // their exact dispatch boundaries instead.
    voices.FoldStaleChannelMixGains();
    // Live pool-limit changes are callback-boundary commands.  Applying one
    // before dense eligibility/snapshotting prevents a mid-plan lifecycle
    // mutation from invalidating worker-visible voice state.
    voices.ApplyRuntimeVoiceLimit(blockStartFrame);
    // Barrier events split the block: render the stretch before each one,
    // dispatch the barrier at its exact frame (same call sparse makes), and
    // continue with the rest.  Frames, order and blockCursor values are those
    // of the unsplit block.
    // Split points: a barrier event (dispatched at its frame between two
    // stretches) or the op-log budget running out (no dispatch; the split
    // lands on the start of the offending frame's event group so same-frame
    // order is preserved).  kind: 0 none, 1 barrier, 2 budget.
    const auto findSplit = [&](uint32_t from, uint32_t& kind) -> uint32_t {
        uint32_t opSum = 0u;
        for (uint32_t i = from; i < eventCount; ++i) {
            if (IsBarrierEvent(events[i].type)) { kind = 1u; return i; }
            const uint32_t ops = WholeVoiceRowOpEstimate(events[i]);
            if (opSum + ops > wvRowOpBudget_) {
                uint32_t group = i;
                while (group > from &&
                       events[group - 1u].frameOffset == events[i].frameOffset)
                    --group;
                if (group > from) { kind = 2u; return group; }
                // One frame holds more than a whole budget of ops: keep the
                // group together and let the log grow (pathological).
            }
            opSum += ops;
        }
        kind = 0u;
        return eventCount;
    };
    uint32_t splitKind = 0u;
    uint32_t split = findSplit(0u, splitKind);
    if (splitKind == 0u) {
        RenderBlockStretch(voices, channels, sampleData, hilbertData,
                           sampleDataFrames, outputLeft, outputRight,
                           numFrames, events, eventCount, correctnessMode,
                           blockStartFrame, channelBusLeft, channelBusRight);
        return;
    }
    uint32_t paths = 0u;
    uint32_t cursor = 0u;       // next unconsumed event
    uint32_t frameBase = 0u;    // first frame not yet rendered
    const auto renderStretch = [&](uint32_t frameEnd, uint32_t eventEnd) {
        if (frameEnd <= frameBase) return;
        splitEvents_.clear();
        for (uint32_t i = cursor; i < eventEnd; ++i) {
            splitEvents_.push_back(events[i]);
            splitEvents_.back().frameOffset -= frameBase;
        }
        float* const* busL = nullptr;
        float* const* busR = nullptr;
        if (channelBusLeft != nullptr && channelBusRight != nullptr) {
            for (uint32_t c = 0u; c < kChannelCount; ++c) {
                splitBusLeft_[c] = channelBusLeft[c] + frameBase;
                splitBusRight_[c] = channelBusRight[c] + frameBase;
            }
            busL = splitBusLeft_;
            busR = splitBusRight_;
        }
        RenderBlockStretch(voices, channels, sampleData, hilbertData,
                           sampleDataFrames, outputLeft + frameBase,
                           outputRight + frameBase, frameEnd - frameBase,
                           splitEvents_.data(),
                           static_cast<uint32_t>(splitEvents_.size()),
                           correctnessMode, blockStartFrame + frameBase,
                           busL, busR);
        paths |= lastRenderPaths_;
        frameBase = frameEnd;
    };
    while (splitKind != 0u) {
        if (splitKind == 1u) {
            const uint32_t barrier = split;
            const uint32_t barrierFrame =
                (std::min)(events[barrier].frameOffset, numFrames);
            // Events strictly before the barrier frame render with the
            // stretch.
            uint32_t stretchEnd = cursor;
            while (stretchEnd < barrier &&
                   events[stretchEnd].frameOffset < barrierFrame)
                ++stretchEnd;
            renderStretch(barrierFrame, stretchEnd);
            cursor = stretchEnd;
            // Same-frame events up to and including the barrier dispatch now.
            voices.SetCurrentFrame(blockStartFrame + barrierFrame);
            if (batchDispatcher_ != nullptr) {
                batchDispatcher_(events + cursor, barrier + 1u - cursor,
                                 barrierFrame, dispatcherUserData_);
            } else if (dispatcher_ != nullptr) {
                for (uint32_t i = cursor; i <= barrier; ++i)
                    dispatcher_(events[i], barrierFrame, dispatcherUserData_);
            }
            voices.MarkStealScoresDirty();
            voices.FoldStaleChannelMixGains();
            cursor = barrier + 1u;
            frameBase = (std::max)(frameBase, barrierFrame);
        } else {
            const uint32_t splitFrame =
                (std::min)(events[split].frameOffset, numFrames);
            renderStretch(splitFrame, split);
            cursor = split;
            frameBase = (std::max)(frameBase, splitFrame);
        }
        split = findSplit(cursor, splitKind);
    }
    renderStretch(numFrames, eventCount);
    lastRenderPaths_ = paths | 0x200u;  // bit9: block was barrier-split
}

inline void RenderScalar::RenderBlockStretch(
    VoiceManager& voices, const ChannelCache& channels,
    const int16_t* sampleData, const int16_t* hilbertData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t numFrames, const RenderEvent* events, uint32_t eventCount,
    bool correctnessMode, uint64_t blockStartFrame,
    float* const* channelBusLeft, float* const* channelBusRight) {
    if (voices.activeCount_ > scratchCapacity_ &&
        !ReserveVoiceCapacity(voices.activeCount_)) return;
    if (!classChanges_ || !retirements_) return;
    // The whole-voice renderer takes vibrato on every backend: AVX2 kernels
    // rebuild the 64-frame LFO windows internally, scalar/SSE2 render in
    // fixed windows split at every event frame (wvLfoBounds_).
    const ChannelParamsSnapshot* channelParams = channels.GetParams();
    bool vibratoActive = false;
    for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
        if (channelParams[channel].modDepth > 0.0f) {
            vibratoActive = true;
            break;
        }
    }
    lastRenderPaths_ = vibratoActive ? 0x100u : 0u;
    // ── Whole-voice whole-block renderer ────────────────────────────────
    // Single per-voice renderer: owns every note-only block, blocks with no
    // events at all, and note blocks carrying the lifecycle controllers
    // (sustain/sostenuto/all-notes-off/all-sound-off) plus the channel-wide
    // row mutators (pitch bend and CC7/10/11 mix folds), which land as
    // exact-frame channel ops applied inside the owning workers.  Vibrato
    // (CC1/channel pressure) rides this path on every backend: AVX2 kernels
    // rebuild the 64-frame LFO windows themselves; scalar/SSE2 render in
    // fixed windows, so their output is unchanged.
    {
        // Scalar/SSE2 vibrato renders on the whole-voice path in the
        // loop's exact LFO windows: every event frame and every 64 frames
        // inside a gap is a window boundary (kVibratoUpdateFrames).
        wvScalarNoVibrato_ = !vibratoActive &&
            kernelSet_->backend != RenderBackend::AVX2;
        wvScalarVibrato_ = vibratoActive &&
            kernelSet_->backend != RenderBackend::AVX2;
        if (wvScalarVibrato_) {
            wvLfoBounds_.clear();
            uint32_t windowCursor = 0u;
            uint32_t windowEvent = 0u;
            while (windowCursor < numFrames) {
                while (windowEvent < eventCount &&
                       events[windowEvent].frameOffset <= windowCursor)
                    ++windowEvent;
                uint32_t windowEnd = numFrames;
                if (windowEvent < eventCount &&
                    events[windowEvent].frameOffset < windowEnd)
                    windowEnd = events[windowEvent].frameOffset;
                if (windowEnd - windowCursor > kVibratoUpdateFrames)
                    windowEnd = windowCursor + kVibratoUpdateFrames;
                if (windowEnd <= windowCursor) break;
                wvLfoBounds_.push_back(windowEnd);
                windowCursor = windowEnd;
            }
        }
#if defined(_MSC_VER)
        const uint64_t wvPlanBegin = __rdtsc();
#endif
        const bool planned =
            PlanWholeVoiceBlock(voices, events, eventCount, blockStartFrame,
                                channelParams);
#if defined(_MSC_VER)
        wvPlanCycles_ += __rdtsc() - wvPlanBegin;
#endif
        if (planned) {
            lastRenderPaths_ |= 0x1u;
            ++pathBlocks_[0];
            wvHilbertData_ = hilbertData;
            RenderWholeVoiceBlock(voices, sampleData, sampleDataFrames,
                                  outputLeft, outputRight, numFrames,
                                  blockStartFrame, channelBusLeft,
                                  channelBusRight);
            return;
        }
    }
    // The plan refused only because a reservation failed (out of memory after
    // a late pool growth; PrepareWholeVoice reserves everything at init).  The
    // stretch is skipped.
    (void)vibratoActive;
    (void)correctnessMode;
}

// ── Whole-voice whole-block renderer ────────────────────────────────────

// Copy the render state of a displaced/killed voice into a ghost row.  Every
// dense field rides along (the macro keeps that set complete), plus the
// launch bookkeeping row (one cache line: channel, loopMode, the row-op
// inputs and the vibrato LFO state are what ghost rendering reads) and
// stealOutputGain.  birthFrame is voice-manager bookkeeping no ghost path
// touches.  A new SoA cold field that the ghost render path reads must be
// added here.
#define SVMS_COPY_VOICE_ROW_FIELD(type, name)     destination.name[destinationRow] = source.name[sourceRow];
inline void CopyGhostRow(const VoiceSoA& source, VoiceSoA& destination,
                         uint32_t sourceRow, uint32_t destinationRow) {
    SVMS_VOICE_SOA_DENSE_FIELDS(SVMS_COPY_VOICE_ROW_FIELD)
    destination.rows[destinationRow] = source.rows[sourceRow];
    destination.stealOutputGain[destinationRow] =
        source.stealOutputGain[sourceRow];
    if (source.rot != nullptr && destination.rot != nullptr)
        destination.rot[destinationRow] = source.rot[sourceRow];
}
#undef SVMS_COPY_VOICE_ROW_FIELD

// Remove a pending deferred-release op entry for a handle whose generation
// is dying (displacement overflow or an All Sound Off kill).  Block-end
// finalization must not observe the dead generation: the handle may already
// host a newer launch carrying its own release op, and a stale entry would
// double-count releasingCount_ once the new voice's own release lands.
// O(1) via wvOpIndex_: a handle holds at most one entry (the hook guards on
// wvReleaseFrame_), so this finds and swaps exactly the entry the former
// linear scan did — finalization order is unchanged.  The scan was O(pending
// releases) per steal, i.e. O(steals x pool) per block under chopped churn.
inline void RenderScalar::DropWholeVoiceReleaseOp(VoiceHandle handle) {
    const uint32_t i = wvOpIndex_[handle];
    if (i >= wvOpCount_ || wvOpHandles_[i] != handle) return;
    const uint32_t last = --wvOpCount_;
    if (i != last) {
        wvOpHandles_[i] = wvOpHandles_[last];
        wvOpFrames_[i] = wvOpFrames_[last];
        wvOpIndex_[wvOpHandles_[i]] = i;
    }
}

// Classify from raw render state; independent of the render-class lists so
// workers never touch shared scheduling metadata.
inline VoiceRenderClass ClassifyWholeVoiceRow(const VoiceSoA& v,
                                              uint32_t row) {
    if (v.state[row] == static_cast<uint8_t>(VoiceState::Free) ||
        v.sampleBacked[row] == 0u || v.relEnd[row] < 2u) {
        return VoiceRenderClass::Generic;
    }
    if (v.stealFadeInFramesRemaining[row] != 0u)
        return VoiceRenderClass::Generic;
    const bool loop = v.loopEnabled[row] != 0u;
    if (loop && (v.relLoopS[row] >= v.relLoopE[row] ||
                 v.relLoopE[row] > v.relEnd[row])) {
        return VoiceRenderClass::Generic;
    }
    if (v.state[row] == static_cast<uint8_t>(VoiceState::Releasing))
        return loop ? VoiceRenderClass::ReleaseLoop
                    : VoiceRenderClass::ReleaseOneShot;
    if (v.envelopeStage[row] == 3u)
        return loop ? VoiceRenderClass::SustainedLoop
                    : VoiceRenderClass::SustainedOneShot;
    if (v.envelopeStage[row] == 1u || v.envelopeStage[row] == 2u) {
        return loop ? VoiceRenderClass::TransientLoop
                    : VoiceRenderClass::TransientOneShot;
    }
    return VoiceRenderClass::Generic;
}

inline void RenderScalar::WholeVoiceVoiceConfiguredHook(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || handle >= renderer->wvHandleCapacity_) return;
    renderer->wvStartFrame_[handle] = renderer->wvEventFrame_;
    renderer->wvStartIngress_[handle] = renderer->wvEventOrdinal_;
}

inline void RenderScalar::WholeVoiceVoiceForkedHook(
    VoiceHandle twin, VoiceHandle source, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || twin >= renderer->wvHandleCapacity_ ||
        source >= renderer->wvHandleCapacity_) {
        return;
    }
    // The twin's row is an exact copy of the source's pre-render row, so
    // rendering it from the source's start reproduces the source's trajectory
    // up to the release frame, where it diverges onto the release envelope.
    // Inheriting the ingress ordinal keeps same-frame op qualification
    // identical for both halves.
    renderer->wvStartFrame_[twin] = renderer->wvStartFrame_[source];
    renderer->wvStartIngress_[twin] = renderer->wvStartIngress_[source];
    // The twin replays the source's trajectory up to its release, so it
    // must also hear the re-excite hits the source took so far.
    if (renderer->wvExciteHead_ != nullptr &&
        source < renderer->wvExciteCapacity_ &&
        twin < renderer->wvExciteCapacity_) {
        const VoiceSoA& rows = renderer->wvPlanVoices_->v;
        const uint32_t sourcePlay = rows.playIndex[source];
        const uint32_t twinPlay = rows.playIndex[twin];
        for (uint32_t i = renderer->WholeVoiceSkipForeignExcites(
                 renderer->wvExciteHead_[source], sourcePlay);
             i != UINT32_MAX;
             i = renderer->WholeVoiceSkipForeignExcites(
                 renderer->wvExcites_[i].next, sourcePlay)) {
            WholeVoiceExcite copy = renderer->wvExcites_[i];
            copy.playIndex = twinPlay;
            copy.next = UINT32_MAX;
            const uint32_t slot =
                static_cast<uint32_t>(renderer->wvExcites_.size());
            renderer->wvExcites_.push_back(copy);
            if (renderer->wvExciteHead_[twin] == UINT32_MAX) {
                renderer->wvExciteHead_[twin] = slot;
                renderer->wvExciteTouched_.push_back(twin);
            } else {
                renderer->wvExcites_[renderer->wvExciteTail_[twin]].next = slot;
            }
            renderer->wvExciteTail_[twin] = slot;
        }
    }
}

inline void RenderScalar::WholeVoiceReExciteHook(VoiceHandle handle,
                                                 void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || renderer->wvExciteHead_ == nullptr ||
        handle >= renderer->wvExciteCapacity_ ||
        renderer->wvPlanVoices_ == nullptr) {
        return;
    }
    VoiceManager& voices = *renderer->wvPlanVoices_;
    VoiceSoA& rows = voices.v;
    const uint32_t slot = static_cast<uint32_t>(renderer->wvExcites_.size());
    renderer->wvExcites_.push_back(
        {renderer->wvEventFrame_, rows.playIndex[handle], UINT32_MAX,
         rows.envelopeStage[handle], rows.attackSamplesRemaining[handle],
         rows.decaySamplesRemaining[handle], rows.attackGainStep[handle]});
    if (renderer->wvExciteHead_[handle] == UINT32_MAX) {
        renderer->wvExciteHead_[handle] = slot;
        renderer->wvExciteTouched_.push_back(handle);
    } else {
        renderer->wvExcites_[renderer->wvExciteTail_[handle]].next = slot;
    }
    renderer->wvExciteTail_[handle] = slot;
    // Steal scoring for the rest of the pre-pass sees the excited stage.
    const uint8_t stageBefore = rows.envelopeStage[handle];
    ExciteVoiceRow(rows, handle, voices.ReExciteAttackFrames());
    if (rows.envelopeStage[handle] != stageBefore)
        voices.RefreshRenderClass(handle);
}

inline bool RenderScalar::EnsureWholeVoiceExciteStorage(
    uint32_t handleCapacity) {
    if (wvExciteHead_ != nullptr && wvExciteTail_ != nullptr &&
        wvExciteCapacity_ >= handleCapacity) {
        return true;
    }
    auto* head = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(handleCapacity) * sizeof(uint32_t),
        kMixBufferAlign));
    auto* tail = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(handleCapacity) * sizeof(uint32_t),
        kMixBufferAlign));
    if (head == nullptr || tail == nullptr) {
        _aligned_free(head);
        _aligned_free(tail);
        return false;
    }
    std::fill(head, head + handleCapacity, UINT32_MAX);
    std::fill(tail, tail + handleCapacity, UINT32_MAX);
    _aligned_free(wvExciteHead_);
    _aligned_free(wvExciteTail_);
    wvExciteHead_ = head;
    wvExciteTail_ = tail;
    wvExciteCapacity_ = handleCapacity;
    wvExciteTouched_.clear();   // every list end was just reset
    return true;
}

inline uint32_t RenderScalar::WholeVoiceGhostLimit() const {
    return ghostBudget_ != 0u && ghostBudget_ < wvGhostCapacity_
        ? ghostBudget_ : wvGhostCapacity_;
}

inline void RenderScalar::WholeVoicePreTailCaptureHook(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || handle >= renderer->wvHandleCapacity_) return;
    if (renderer->wvGhostCount_ >= renderer->WholeVoiceGhostLimit()) {
        // Pathological block (more steals than the pool).  The displaced
        // voice's pre-steal samples are lost for this block; the launch
        // itself stays exact.  Counted for telemetry.
        ++renderer->wvGhostOverflowCount_;
        // The displaced voice is destroyed without a ghost, so its pending
        // deferred release must not survive the displacement.  The handle is
        // immediately reused by the replacement launch, and a stale
        // wvReleaseFrame_ below the new start frame leaves the item loop in
        // WholeVoiceJobThunk unable to advance cursor (a release frame at or
        // below the cursor renders nothing and flips nothing) — a permanent
        // all-workers hang at saturated launch storms.  Drop the stale
        // release and swap its op entry out so block-end finalization cannot
        // observe the dead generation either.
        if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) {
            renderer->wvReleaseFrame_[handle] = UINT32_MAX;
            renderer->DropWholeVoiceReleaseOp(handle);
        }
        return;
    }
    const uint32_t ghost = renderer->wvGhostCount_++;
    CopyGhostRow(renderer->wvPlanVoices_->v, renderer->wvGhostState_,
                 handle, ghost);
    renderer->wvGhostStartFrame_[ghost] =
        renderer->wvStartFrame_[handle] == UINT32_MAX
            ? 0u : renderer->wvStartFrame_[handle];
    renderer->wvGhostDeathFrame_[ghost] = renderer->wvEventFrame_;
    renderer->wvGhostNoTail_[ghost] = 0u;
    // An in-place steal successor inherits this slot's rotation state, which
    // the oracle holds at the steal frame; the pre-pass only has the
    // block-start copy (the ghost renders the stretch in a worker).  Advance
    // the live slot by the samples the victim renders before it dies.
    {
        VoiceSoA& pool = renderer->wvPlanVoices_->v;
        if (pool.rot != nullptr && pool.sampleBacked[handle] != 0u &&
            renderer->wvEventFrame_ > renderer->wvGhostStartFrame_[ghost]) {
            AdvanceVoiceRotation(pool.rot[handle],
                renderer->wvEventFrame_ - renderer->wvGhostStartFrame_[ghost]);
        }
    }
    // Zero the tail record up front: if the ghost retires before its death
    // frame (envelope end mid-span) no CaptureGhostTail runs, and a record
    // left over from a previous use of this ghost slot must never be
    // adopted at block end.
    renderer->wvGhostTails_[ghost].framesRemaining = 0u;
    // The ghost inherits the victim's launch ordinal so same-frame op
    // qualification survives the displacement.
    renderer->wvGhostStartIngress_[ghost] = renderer->wvStartIngress_[handle];
    renderer->wvGhostHandle_[ghost] = handle;
    if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) {
        // A deferred release on the displaced voice transfers to the ghost.
        // The victim's op entry is dropped with the transfer: the victim is
        // dead, so block-end finalization has nothing left to observe, and
        // a kept entry would double-count if the reused handle releases
        // again later in the same block.
        renderer->wvGhostReleaseFrame_[ghost] =
            renderer->wvReleaseFrame_[handle];
        renderer->wvReleaseFrame_[handle] = UINT32_MAX;
        renderer->DropWholeVoiceReleaseOp(handle);
    } else {
        renderer->wvGhostReleaseFrame_[ghost] = UINT32_MAX;
    }
}

// CC120 (All Sound Off) hook, fired by VoiceManager::SilenceChannelImmediate
// for every channel voice just before the bookkeeping retires it.  The
// pre-kill row state becomes a ghost that renders [start, killFrame) —
// exactly the samples the frame-major renderers still hear — and then stops
// dead: no envelope release, no steal-tail fade.  The kill itself stays in
// VoiceManager, so the freed slot is reusable by later in-block launches
// exactly as live dispatch leaves it.
inline void RenderScalar::WholeVoiceSilenceVoiceHook(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || handle >= renderer->wvHandleCapacity_) return;
    if (renderer->wvGhostCount_ >= renderer->WholeVoiceGhostLimit()) {
        // Pathological block (more kills+steals than pool rows).  The killed
        // voice's pre-kill samples are lost for this block; the kill itself
        // stays exact.  Counted for telemetry.
        ++renderer->wvGhostOverflowCount_;
        if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) {
            renderer->wvReleaseFrame_[handle] = UINT32_MAX;
            renderer->DropWholeVoiceReleaseOp(handle);
        }
        return;
    }
    const uint32_t ghost = renderer->wvGhostCount_++;
    CopyGhostRow(renderer->wvPlanVoices_->v, renderer->wvGhostState_,
                 handle, ghost);
    renderer->wvGhostStartFrame_[ghost] =
        renderer->wvStartFrame_[handle] == UINT32_MAX
            ? 0u : renderer->wvStartFrame_[handle];
    renderer->wvGhostDeathFrame_[ghost] = renderer->wvEventFrame_;
    // All Sound Off is an immediate stop: no fade tail after the kill frame.
    renderer->wvGhostNoTail_[ghost] = 1u;
    renderer->wvGhostTails_[ghost].framesRemaining = 0u;
    // The kill ghost inherits the victim's launch ordinal so same-frame op
    // qualification survives the kill.
    renderer->wvGhostStartIngress_[ghost] = renderer->wvStartIngress_[handle];
    renderer->wvGhostHandle_[ghost] = handle;
    if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) {
        if (renderer->wvReleaseFrame_[handle] < renderer->wvEventFrame_) {
            // The release flip lands before the kill frame; the ghost keeps
            // rendering through it (sustained -> releasing) and dies on
            // schedule at the kill frame.
            renderer->wvGhostReleaseFrame_[ghost] =
                renderer->wvReleaseFrame_[handle];
        } else {
            // Killed before (or on) its release frame: the flip never
            // happens audibly.
            renderer->wvGhostReleaseFrame_[ghost] = UINT32_MAX;
        }
        // The handle dies here either way: drop the pending flip so a
        // reused handle cannot inherit it and block-end finalization cannot
        // double-count (same reasoning as the steal-transfer drop above).
        renderer->wvReleaseFrame_[handle] = UINT32_MAX;
        renderer->DropWholeVoiceReleaseOp(handle);
    } else {
        renderer->wvGhostReleaseFrame_[ghost] = UINT32_MAX;
    }
}

// CC120 hook for channel-owned steal tails, fired before the tail is
// unlinked.  Only the kill frame is recorded: UnlinkStealTail preserves the
// tail's fields, so RenderWholeVoiceBlock renders the tail's audio up to
// that frame and then stops exactly where live dispatch would have.
inline void RenderScalar::WholeVoiceSilenceTailHook(uint32_t slot,
                                                    void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || slot >= kStealTailReserve) return;
    if (renderer->wvTailKillFrame_[slot] == UINT32_MAX)
        renderer->wvTailKillFrame_[slot] = renderer->wvEventFrame_;
}

// CC7/10/11/121 mix-fold op: one log entry per marking event (per channel),
// not per voice — the owning worker recomputes each row's gains from its own
// per-voice gainLeft/gainRight at the op frame, with the exact
// FoldVoiceMixGains expressions (renderGain vintage = currentGain at the op
// frame, which the oracle's batch-boundary fold also observes).
inline void RenderScalar::WholeVoiceMixOpHook(uint8_t channel,
                                              float mixScaleLeft,
                                              float mixScaleRight,
                                              void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr) return;
    assert(renderer->wvRowOpCount_ < renderer->wvRowOpCapacity_ &&
           "row-op estimate undercounted a mix CC");
    if (renderer->wvRowOpCount_ >= renderer->wvRowOpCapacity_) return;
    WholeVoiceChannelOp& op = renderer->wvRowOps_[renderer->wvRowOpCount_++];
    op.frame = renderer->wvEventFrame_;
    op.ingress = renderer->wvEventOrdinal_;
    op.channel = channel;
    op.kind = 1u;
    op.a = mixScaleLeft;
    op.b = mixScaleRight;
}

// Pitch-bend op: the payload is the channel's sysex-aware semitone total
// plus the common (unscaled) ratio; the worker reapplies each row's own
// pitchBendScales exactly as VoiceManager::ApplyChannelBendRatio does
// inline outside the pre-pass.
inline void RenderScalar::WholeVoiceBendOpHook(uint8_t channel,
                                               float bendSemitones,
                                               float commonRatio,
                                               void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr) return;
    assert(renderer->wvRowOpCount_ < renderer->wvRowOpCapacity_ &&
           "row-op estimate undercounted a bend event");
    if (renderer->wvRowOpCount_ >= renderer->wvRowOpCapacity_) return;
    WholeVoiceChannelOp& op = renderer->wvRowOps_[renderer->wvRowOpCount_++];
    op.frame = renderer->wvEventFrame_;
    op.ingress = renderer->wvEventOrdinal_;
    op.channel = channel;
    op.kind = 0u;
    op.a = bendSemitones;
    op.b = commonRatio;
}

// Vibrato depth op (CC1 mod wheel / CC121 reset / channel pressure): the
// payload is the channel's post-rebuild modulation depth.  The worker
// updates its row-local depth cursor and performs the modulated ->
// unmodulated phaseIncs restore when the depth reaches zero; turning the
// LFO on needs no row write (the next segment's LFO window rebuilds the
// increment from base x bendRatio x lfoRatio).
inline void RenderScalar::WholeVoiceVibratoOpHook(uint8_t channel,
                                                  float modDepth,
                                                  void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr) return;
    assert(renderer->wvRowOpCount_ < renderer->wvRowOpCapacity_ &&
           "row-op estimate undercounted a vibrato depth change");
    if (renderer->wvRowOpCount_ >= renderer->wvRowOpCapacity_) return;
    WholeVoiceChannelOp& op = renderer->wvRowOps_[renderer->wvRowOpCount_++];
    op.frame = renderer->wvEventFrame_;
    op.ingress = renderer->wvEventOrdinal_;
    op.channel = channel;
    op.kind = 2u;
    op.a = modDepth;
    op.b = 0.0f;
}

inline bool RenderScalar::EnsureWholeVoiceRowOpStorage(uint32_t opCount) {
    if (opCount == 0u) return true;
    if (wvRowOps_ != nullptr && wvChanOps_ != nullptr &&
        wvRowOpCapacity_ >= opCount) {
        return true;
    }
    auto* freshOps = static_cast<WholeVoiceChannelOp*>(_aligned_malloc(
        static_cast<size_t>(opCount) * sizeof(WholeVoiceChannelOp),
        kMixBufferAlign));
    auto* freshChan = static_cast<WholeVoiceChannelOp*>(_aligned_malloc(
        static_cast<size_t>(opCount) * sizeof(WholeVoiceChannelOp),
        kMixBufferAlign));
    if (freshOps == nullptr || freshChan == nullptr) {
        _aligned_free(freshOps);
        _aligned_free(freshChan);
        return false;
    }
    _aligned_free(wvRowOps_);
    _aligned_free(wvChanOps_);
    wvRowOps_ = freshOps;
    wvChanOps_ = freshChan;
    wvRowOpCapacity_ = opCount;
    return true;
}

// Apply one channel op to a single render row (real voice or ghost).
// Row-local only, so any worker can execute it.  The gain expressions
// mirror VoiceManager::FoldVoiceMixGains and the bend expression mirrors
// VoiceManager::ApplyChannelBendRatio bit-for-bit.
inline void RenderScalar::ApplyWholeVoiceRowOp(VoiceSoA& state, uint32_t row,
                                               const WholeVoiceChannelOp& op) {
    if (op.kind == 0u) {
        const float scale = state.pitchBendScales[row];
        const float ratio = scale == 1.0f
            ? op.b : powf(2.0f, op.a * scale / 12.0f);
        state.phaseIncs[row] = state.basePhaseIncs[row] * ratio;
        return;
    }
    state.mixGainL[row] = state.gainLeft[row] * op.a;
    state.mixGainR[row] = state.gainRight[row] * op.b;
    state.stealOutputGain[row] = std::sqrt(
        state.mixGainL[row] * state.mixGainL[row] +
        state.mixGainR[row] * state.mixGainR[row]);
    state.renderGainL[row] = state.currentGain[row] * state.mixGainL[row];
    state.renderGainR[row] = state.currentGain[row] * state.mixGainR[row];
}

inline void RenderScalar::WholeVoiceDeferredReleaseHook(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || handle >= renderer->wvHandleCapacity_) return;
    if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) return;
    // One deferred flip per voice per block: releases never re-target an
    // already-released generation because StartRelease unlinked its key
    // chain before this hook ran.
    renderer->wvReleaseFrame_[handle] = renderer->wvEventFrame_;
    renderer->wvOpIndex_[handle] = renderer->wvOpCount_;
    renderer->wvOpHandles_[renderer->wvOpCount_] = handle;
    renderer->wvOpFrames_[renderer->wvOpCount_] = renderer->wvEventFrame_;
    ++renderer->wvOpCount_;
}

inline bool RenderScalar::PlanWholeVoiceBlock(
    VoiceManager& voices, const RenderEvent* events, uint32_t eventCount,
    uint64_t blockStartFrame, const ChannelParamsSnapshot* channelParams) {
    // Rotation state lives in v.rot (per row) and the ghost/tail scratch
    // mirrors below; ReserveWholeVoiceStorage-era reservations are checked
    // with the other scratch further down.
    // Eligibility is decided before any state changes, so a refusal leaves the
    // voice pool and renderer untouched.
    bool hasEvents = false;
    bool hasReExcite = false;
    uint32_t rowOpEstimate = 0u;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        rowOpEstimate += WholeVoiceRowOpEstimate(events[i]);
        switch (events[i].type) {
            case RenderEventType::NoteReExcite:
                hasReExcite = true;
                [[fallthrough]];
            case RenderEventType::NoteOn:
            case RenderEventType::NoteOff:
            case RenderEventType::StaleNoteOffBatch:
                hasEvents = true;
                break;
            case RenderEventType::ControlChange: {
                const uint8_t controller = events[i].data1;
                switch (controller) {
                    case 1u:
                        // Mod wheel -> vibrato depth op (the driver reports
                        // the post-rebuild depth through the vibrato hook).
                        // The LFO itself advances inside the AVX2 kernels'
                        // 64-frame windows; see RenderSpanContext::lfoActive.
                        break;
                    default:
                    case 0u:
                    case 32u:
                        // Bank select: launch-time state only.  Unmapped
                        // controllers share this: they have no engine
                        // effect, so treating them as no-ops keeps CC-heavy
                        // material on this path without audio change.
                        break;
                    case 64u:
                    case 66u:
                    case 120u:
                    case 123u:
                        // Lifecycle: deferred-release / kill hooks.
                        break;
                    case 6u:
                    case 38u:
                    case 96u:
                    case 97u:
                        // RPN data wheel -> pitch-bend op.
                        break;
                    case 7u:
                    case 10u:
                    case 11u:
                        // Volume/pan/expression -> mix-gain fold op.
                        break;
                    case 121u:
                        // Reset All Controllers -> bend-center, mix fold AND
                        // mod-depth reset (channelModWheel_ clears).
                        break;
                }
                hasEvents = true;
                break;
            }
            case RenderEventType::PitchBend:
                hasEvents = true;
                break;
            case RenderEventType::ChannelPressure:
                // Channel aftertouch scales the vibrato depth via the SF2
                // default modulator -> vibrato depth op.
                hasEvents = true;
                break;
            case RenderEventType::ProgramChange:
            case RenderEventType::RhythmPart:
                // Launch-time state only: preset/kit selection is captured at
                // configure time and sounding voices are untouched (the same
                // category as bank select above).
                hasEvents = true;
                break;
            case RenderEventType::MasterVolume:
                // SysEx master volume: the driver marks every channel's mix
                // gains stale, which the pre-pass hooks turn into one
                // mix-gain op per channel.
                hasEvents = true;
                break;
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
                // SysEx master tune/transpose: RefreshAllPitchIncrements
                // rewrites every channel's phaseIncs, which the pre-pass
                // hooks turn into one bend op per channel.
                hasEvents = true;
                break;
            default:
                lastRefusalType_ = static_cast<uint8_t>(events[i].type);
                lastRefusalData1_ = events[i].data1;
                ++refusalCounts_[kRefuseEventType];
                assert(false && "barrier events are split out of the plan");
                return false;
        }
    }
    // No dispatcher (tests/bench only): events are ignored, voices render.
    // Launches dispatched below can grow the active set into free pool slots
    // (steals replace, free-slot launches add).  Worst case: one new voice per
    // event.  Reserve up front so every active-count-sized structure
    // (wvSliceHandles_, op/ghost arrays, retirement buffers) covers the
    // post-dispatch population; on failure the plan refuses cleanly (nothing
    // has been dispatched yet).
    //
    // The post-dispatch population is bounded by the PHYSICAL pool (steals
    // replace, free-slot launches cap out), never by activeCount + eventCount.
    // Reserving the pool size once — instead of active + events per dense
    // block — keeps every O(capacity) structure (ghost VoiceSoA, per-job
    // retirement scratch, dense mutation state) at pool size and lets the
    // early-out fire permanently after the first plan.
    if (voices.GetMaxVoices() > scratchCapacity_ &&
        !ReserveVoiceCapacity(voices.GetMaxVoices())) {
        ++refusalCounts_[kRefuseReservation];
        return false;
    }
    // Per-handle timeline arrays are indexed by raw voice handle, which in a
    // grown or partially-filled pool can be far above the active count.
    const uint32_t handleCapacity = voices.v.GetCapacity();
    if (!ReserveWholeVoiceStorage(handleCapacity)) {
        ++refusalCounts_[kRefuseReservation];
        return false;
    }
    if (!EnsureWholeVoiceJobScratch(
            workerPool_ ? workerPool_->GetThreadCount() * kWvJobsPerThread
                        : 1u) ||
        !SyncWholeVoiceRotationScratch(voices.v.rot != nullptr)) {
        ++refusalCounts_[kRefuseReservation];
        return false;
    }
    if (!EnsureWholeVoiceRowOpStorage(rowOpEstimate) ||
        (hasReExcite && !EnsureWholeVoiceExciteStorage(handleCapacity))) {
        ++refusalCounts_[kRefuseReservation];
        return false;
    }
    // Forget the previous block's hits (only the touched handles' list ends
    // are dirty).
    if (wvExciteHead_ != nullptr) {
        for (uint32_t touched : wvExciteTouched_) {
            if (touched < wvExciteCapacity_)
                wvExciteHead_[touched] = wvExciteTail_[touched] = UINT32_MAX;
        }
    }
    wvExciteTouched_.clear();
    wvExcites_.clear();

    std::fill(wvStartFrame_, wvStartFrame_ + handleCapacity, UINT32_MAX);
    std::fill(wvReleaseFrame_, wvReleaseFrame_ + handleCapacity, UINT32_MAX);
    std::fill(wvStartIngress_, wvStartIngress_ + handleCapacity, UINT32_MAX);
    std::fill(wvTailKillFrame_, wvTailKillFrame_ + kStealTailReserve,
              UINT32_MAX);
    wvOpCount_ = 0u;
    wvGhostCount_ = 0u;
    wvRowOpCount_ = 0u;
    wvPlanVoices_ = &voices;
    // Worker vibrato/bend cursors start from the block-START channel state
    // (captured before any dispatch mutates the live snapshot) and follow
    // the op log from there.
    if (channelParams != nullptr) {
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
            wvChanStartDepth_[channel] = channelParams[channel].modDepth;
            wvChanStartBendRatio_[channel] = channelParams[channel].bendRatio;
            wvChanStartBendCents_[channel] =
                channelParams[channel].pitchBendCents;
            wvChanVibratoActive_[channel] =
                channelParams[channel].modDepth > 0.0f;
        }
    } else {
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
            wvChanStartDepth_[channel] = 0.0f;
            wvChanStartBendRatio_[channel] = 0.0f;
            wvChanStartBendCents_[channel] = 0.0f;
            wvChanVibratoActive_[channel] = false;
        }
    }

    voices.SetDeferredReleaseHook(&WholeVoiceDeferredReleaseHook, this);
    voices.SetVoiceConfiguredHook(&WholeVoiceVoiceConfiguredHook, this);
    voices.SetVoiceForkedHook(&WholeVoiceVoiceForkedHook, this);
    voices.SetPreTailCaptureHook(&WholeVoicePreTailCaptureHook, this);
    voices.SetSilenceChannelHooks(&WholeVoiceSilenceVoiceHook,
                                  &WholeVoiceSilenceTailHook, this);
    voices.SetRowOpHooks(&WholeVoiceMixOpHook, &WholeVoiceBendOpHook,
                         &WholeVoiceVibratoOpHook, this);
    voices.SetStealTailCaptureSuppressed(true);
    if (hasReExcite)
        voices.SetReExciteHook(&WholeVoiceReExciteHook, this);

    // Ingress-order dispatch with the render clock at each event's exact
    // frame: birth ages, steal scores and every driver-side decision see
    // the same values they would see mid-block today.
    //
    // A maximal same-frame run of note events goes to the batch dispatcher
    // in one call, so the driver's exact-frame batching (launch-plan reuse,
    // adjacent note-off aggregation) engages here.
    // Every launch in the run is stamped with the run's first ordinal.  That
    // is exact for same-frame op qualification: ordinals are only ever
    // compared against row-op ordinals, row-op events never join a run, so
    // for any op ordinal o outside [begin, end) "o < i" == "o < begin".
    const auto isNoteEvent = [](RenderEventType type) {
        return type == RenderEventType::NoteOn ||
               type == RenderEventType::NoteOff ||
               type == RenderEventType::StaleNoteOffBatch;
    };
    for (uint32_t i = 0u; i < eventCount;) {
        const RenderEvent& event = events[i];
        wvEventFrame_ = event.frameOffset;
        wvEventOrdinal_ = i;
        voices.SetCurrentFrame(blockStartFrame + event.frameOffset);
        if (batchDispatcher_ != nullptr) {
            uint32_t end = i + 1u;
            if (isNoteEvent(event.type)) {
                while (end < eventCount &&
                       events[end].frameOffset == event.frameOffset &&
                       isNoteEvent(events[end].type)) {
                    ++end;
                }
            }
            batchDispatcher_(&event, end - i, event.frameOffset,
                             dispatcherUserData_);
            i = end;
        } else {
            if (dispatcher_ != nullptr)
                dispatcher_(event, event.frameOffset, dispatcherUserData_);
            ++i;
        }
    }

    voices.SetDeferredReleaseHook(nullptr, nullptr);
    voices.SetVoiceConfiguredHook(nullptr, nullptr);
    voices.SetVoiceForkedHook(nullptr, nullptr);
    voices.SetPreTailCaptureHook(nullptr, nullptr);
    voices.SetSilenceChannelHooks(nullptr, nullptr, nullptr);
    voices.SetRowOpHooks(nullptr, nullptr, nullptr, nullptr);
    voices.SetReExciteHook(nullptr, nullptr);
    voices.SetStealTailCaptureSuppressed(false);
    wvPlanVoices_ = nullptr;

    // Regroup the op log per channel (stable → frame-sorted within each
    // channel) so worker items can binary-walk their own row's window.
    wvRowOpChannelMask_ = 0u;
    {
        uint32_t chanCounts[kChannelCount] = {};
        for (uint32_t i = 0u; i < wvRowOpCount_; ++i) {
            ++chanCounts[wvRowOps_[i].channel];
            // Only mix ops fold at block end: their per-channel scales live
            // in the VoiceManager cache (refreshed by every mark).  Bend ops
            // must NOT widen the fold mask — a channel whose cache was never
            // marked still holds its construction-time zero, and folding it
            // would zero every active row's mixGain with writeRenderGain
            // disabled.
            if (wvRowOps_[i].kind == 1u)
                wvRowOpChannelMask_ |= uint64_t(1) << wvRowOps_[i].channel;
            else if (wvRowOps_[i].kind == 2u)
                wvChanVibratoActive_[wvRowOps_[i].channel] = true;
        }
        uint32_t running = 0u;
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
            wvChanOpStart_[channel] = running;
            running += chanCounts[channel];
        }
        wvChanOpStart_[kChannelCount] = running;
        wvChanOpStart_[kChannelCount + 1] = running;
        uint32_t writeCursor[kChannelCount];
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel)
            writeCursor[channel] = wvChanOpStart_[channel];
        for (uint32_t i = 0u; i < wvRowOpCount_; ++i) {
            const WholeVoiceChannelOp& op = wvRowOps_[i];
            wvChanOps_[writeCursor[op.channel]++] = op;
        }
    }
    return true;
}

// Decimation leaves the mixed rows scattered through the active list, so the
// batch kernels (which need a run of consecutive rows) never fire and every
// kept voice pays full per-voice setup.  Re-lay the slice so that inside each
// job's range the mixed rows come first, in their original relative order,
// then the muted ones.  Muted rows add nothing to the mix, so the order the
// mixed rows accumulate in is unchanged (each job still mixes a contiguous
// piece of the mixed sequence), and the job ranges keep their sizes.
inline void RenderScalar::PartitionMutedRows(uint32_t activeCount,
                                             uint32_t itemCount,
                                             uint32_t jobCount) {
    uint32_t mixedTotal = 0u, mutedTotal = 0u;
    for (uint32_t i = 0u; i < activeCount; ++i) {
        const uint32_t handle = wvSliceHandles_[i];
        if (WholeVoiceRowMuted(handle))
            wvPartMuted_[mutedTotal++] = handle;
        else
            wvPartMixed_[mixedTotal++] = handle;
    }
    uint32_t mixedUsed = 0u, mutedUsed = 0u, mixedGiven = 0u;
    uint64_t voicesSoFar = 0u;
    for (uint32_t job = 0u; job < jobCount; ++job) {
        const uint32_t begin = static_cast<uint32_t>(
            static_cast<uint64_t>(itemCount) * job / jobCount);
        const uint32_t end = static_cast<uint32_t>(
            static_cast<uint64_t>(itemCount) * (job + 1u) / jobCount);
        const uint32_t vb = (std::min)(begin, activeCount);
        const uint32_t ve = (std::min)(end, activeCount);
        const uint32_t n = ve > vb ? ve - vb : 0u;
        voicesSoFar += n;
        const uint32_t target = static_cast<uint32_t>(
            static_cast<uint64_t>(mixedTotal) * voicesSoFar / activeCount);
        const uint32_t mixedHere = target - mixedGiven;
        mixedGiven = target;
        uint32_t pos = vb;
        for (uint32_t k = 0u; k < mixedHere; ++k)
            wvSliceHandles_[pos++] = wvPartMixed_[mixedUsed++];
        for (uint32_t k = mixedHere; k < n; ++k)
            wvSliceHandles_[pos++] = wvPartMuted_[mutedUsed++];
    }
}

inline bool RenderScalar::RenderWholeVoiceSegment(
    VoiceSoA& state, VoiceManager* voices, uint32_t row,
    const int16_t* sampleData, uint32_t sampleDataFrames, float* outL,
    float* outR, uint32_t segStart, uint32_t segFrames, bool isReal,
    uint32_t jobIndex, float lfoDepth, float lfoBendRatio, bool lfoActive) {
    // Per-segment rdtsc profiling is opt-in: this function runs once per
    // voice per op-split segment (tens of thousands of calls per block on
    // op-dense material), and six rdtsc per call measured ~+25% on every
    // whole-voice block. The cheap call/frame counters below stay on.
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
    const uint64_t segBegin = __rdtsc();
#endif
    uint64_t* segStats = wvJobSeg_[jobIndex].value;
    ++segStats[kWvSegCalls];
    SpanRetirement* retBuf = wvJobRetirements_ +
        static_cast<size_t>(jobIndex) * scratchCapacity_;
    const uint32_t retCountBefore = wvJobRetireCounts_[jobIndex * kWvRetireCountStride];
    // Fresh class at the segment start: compared against the class after
    // rendering to detect a lifecycle transition INSIDE this segment.
    const VoiceRenderClass classBefore = ClassifyWholeVoiceRow(state, row);
    RenderClassKernel kernel = kernelSet_->kernels[
        static_cast<uint32_t>(classBefore)];
    // A muted row (decimation) is never mixed, but a row launched this block
    // always is.
    const bool muted = isReal && wvDecimating_ && wvMuted_[row] != 0u &&
                       wvStartFrame_[row] == UINT32_MAX;
    if (!muted && segFrames >= 8u && kernel != nullptr &&
        sampleData != nullptr) {
        ++segStats[kWvSegKernelOk];
        segStats[kWvSegKernelFrames] += segFrames;
        RenderSpanContext context{
            &state, sampleData, wvHilbertData_, sampleDataFrames, outL, outR,
            segStart, segFrames, state.GetCapacity(),
            nullptr, nullptr,
            isReal ? voices->activePosition_ : nullptr,
            isReal ? retBuf : nullptr,
            isReal ? &wvJobRetireCounts_[jobIndex * kWvRetireCountStride] : nullptr,
            lfoDepth, lfoBendRatio, lfoActive ? 1u : 0u};
        if (kernel(context, &row, 1u)) {
            if (isReal) {
                for (uint32_t i = retCountBefore;
                     i < wvJobRetireCounts_[jobIndex * kWvRetireCountStride]; ++i) {
                    retBuf[i].frameOffset += segStart;
                }
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
                segStats[kWvSegCycles] += __rdtsc() - segBegin;
#endif
                return wvJobRetireCounts_[jobIndex * kWvRetireCountStride] > retCountBefore;
            }
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
            segStats[kWvSegCycles] += __rdtsc() - segBegin;
#endif
            return false;
        }
    }
    ++segStats[kWvSegFallback];
    segStats[kWvSegFallbackFrames] += segFrames;
    if (lfoActive) {
        // Scalar fallback segments still honor the LFO: one whole-segment
        // window (advance-then-use + ratio rebuild) keeps the row's LFO
        // phase and modulated flag consistent for the next segment.
        AdvanceVibratoLfoWindow(state, row, segFrames, lfoDepth,
                                lfoBendRatio);
    }
    const uint32_t retiredAt = RenderPrimaryVoiceSpan(
        state, row, sampleData, wvHilbertData_, sampleDataFrames, outL, outR,
        segStart, segFrames, muted ? 0u : segFrames);
    if (retiredAt != UINT32_MAX) {
        if (isReal) {
            retBuf[wvJobRetireCounts_[jobIndex * kWvRetireCountStride]++] = {
                row, segStart + retiredAt, voices->activePosition_[row]};
        }
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
        segStats[kWvSegCycles] += __rdtsc() - segBegin;
#endif
        return true;
    }
    // Post-segment class transition bake, replicating the frame-major
    // oracle's per-frame envelope-stage check: when the lifecycle moved
    // inside this segment (transient→sustained crossing above all), the
    // oracle relinks the render class at the exact crossing frame and
    // LinkRenderClass bakes renderGainL/R = currentGain x mixGain with the
    // frozen sustain level — the value the sustained class kernels read.
    // The whole-voice path advances the same state inside its segments but
    // never relinks mid-block (the block-end RefreshRenderClass pass sees
    // field == desired — the launch already configured a sustained class —
    // and early-outs), so the bake must happen here at the crossing vintage.
    // Row-local writes only, so any worker can execute them.
    const VoiceRenderClass classAfter = ClassifyWholeVoiceRow(state, row);
    if (classAfter != classBefore &&
        (classAfter == VoiceRenderClass::SustainedLoop ||
         classAfter == VoiceRenderClass::SustainedOneShot)) {
        state.renderGainL[row] =
            state.currentGain[row] * state.mixGainL[row];
        state.renderGainR[row] =
            state.currentGain[row] * state.mixGainR[row];
    }
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
    segStats[kWvSegCycles] += __rdtsc() - segBegin;
#endif
    return false;
}

// Render a run of real voices that already proved they have one identical
// segment for the rest of the block: same exact start frame and render class,
// with no release, row-op, or vibrato boundary.  The class kernels retain the
// input handle order, so scalar accumulation order and per-row state evolution
// are unchanged; this only amortizes classification, context construction,
// kernel dispatch, and retirement-offset fixup across the run.
// Scalar/SSE2 vibrato: split the segment at the block's global LFO window
// boundaries and refresh each voice's pitch increment per window, exactly as
// the shared AdvanceVibratoSpanRow does (advance-then-use, powf ratio).  AVX2 blocks pass
// straight through to the kernels' internal windows.
inline bool RenderScalar::RenderWholeVoiceSegmentLfo(
    VoiceSoA& state, VoiceManager* voices, uint32_t row,
    const int16_t* sampleData, uint32_t sampleDataFrames, float* outL,
    float* outR, uint32_t segStart, uint32_t segFrames, bool isReal,
    uint32_t jobIndex, float lfoDepth, float lfoBendRatio, bool lfoActive) {
    // A row with no LFO (step 0) is untouched by every window, so it renders
    // as one segment; windows only matter for rows that carry a vibrato LFO.
    if (!wvScalarVibrato_ || state.vibLfoSteps[row] == 0.0f) {
        return RenderWholeVoiceSegment(state, voices, row, sampleData,
            sampleDataFrames, outL, outR, segStart, segFrames, isReal,
            jobIndex, lfoDepth, lfoBendRatio, lfoActive);
    }
    const uint32_t end = segStart + segFrames;
    uint32_t pos = segStart;
    // Bounds are strictly increasing and pos only advances to a bound (or to
    // the segment end), so one search finds the first window and a plain
    // increment walks the rest.
    auto it = std::upper_bound(wvLfoBounds_.begin(), wvLfoBounds_.end(), pos);
    while (pos < end) {
        const uint32_t next =
            it == wvLfoBounds_.end() ? end : (std::min)(*it, end);
        if (it != wvLfoBounds_.end() && *it == next) ++it;
        AdvanceVibratoSpanRow(state, row, next - pos, lfoDepth,
                              lfoBendRatio);
        if (RenderWholeVoiceSegment(state, voices, row, sampleData,
                sampleDataFrames, outL, outR, pos, next - pos, isReal,
                jobIndex, lfoDepth, lfoBendRatio, false)) {
            return true;
        }
        pos = next;
    }
    return false;
}

inline bool RenderScalar::RenderWholeVoiceBatch(
    VoiceSoA& state, VoiceManager& voices, const uint32_t* handles,
    uint32_t handleCount, VoiceRenderClass renderClass,
    const int16_t* sampleData, uint32_t sampleDataFrames,
    const IndexedJobMix& mix, uint32_t segStart, uint32_t segFrames,
    uint32_t jobIndex) {
    RenderClassKernel kernel =
        kernelSet_->kernels[static_cast<uint32_t>(renderClass)];
    if (handleCount < 2u || segFrames < 8u || kernel == nullptr ||
        sampleData == nullptr) {
        return false;
    }
    SpanRetirement* retBuf = wvJobRetirements_ +
        static_cast<size_t>(jobIndex) * scratchCapacity_;
    const uint32_t retCountBefore = wvJobRetireCounts_[jobIndex * kWvRetireCountStride];
    RenderSpanContext context{
        &state, sampleData, wvHilbertData_, sampleDataFrames,
        mix.outputLeft, mix.outputRight, segStart, segFrames,
        state.GetCapacity(), nullptr, nullptr, voices.activePosition_,
        retBuf, &wvJobRetireCounts_[jobIndex * kWvRetireCountStride],
        0.0f, 1.0f, 0u, mix.busLeft, mix.busRight};
    if (!kernel(context, handles, handleCount)) return false;

    {
        uint64_t* stats = wvJobSeg_[jobIndex].value;
        stats[kWvSegCalls] += handleCount;
        stats[kWvSegKernelOk] += handleCount;
        stats[kWvSegKernelFrames] +=
            static_cast<uint64_t>(segFrames) * handleCount;
    }
    for (uint32_t i = retCountBefore;
         i < wvJobRetireCounts_[jobIndex * kWvRetireCountStride]; ++i) {
        retBuf[i].frameOffset += segStart;
    }
    for (uint32_t i = 0u; i < handleCount; ++i) {
        const uint32_t handle = handles[i];
        const VoiceRenderClass classAfter =
            ClassifyWholeVoiceRow(state, handle);
        if (classAfter != renderClass &&
            (classAfter == VoiceRenderClass::SustainedLoop ||
             classAfter == VoiceRenderClass::SustainedOneShot)) {
            state.renderGainL[handle] =
                state.currentGain[handle] * state.mixGainL[handle];
            state.renderGainR[handle] =
                state.currentGain[handle] * state.mixGainR[handle];
        }
    }
    return true;
}

// Reproduce CaptureStealTail's tail record from the ghost's exact
// steal-frame state.  Stored per ghost: the VoiceSoA steal-tail arrays are
// fixed kStealTailReserve slots, so ghost rows must never index them.
inline void RenderScalar::CaptureGhostTail(uint32_t ghost) {
    VoiceSoA& v = wvGhostState_;
    WholeVoiceGhostTail& tail = wvGhostTails_[ghost];
    tail.framesRemaining = 0u;
    if (v.sampleBacked[ghost] == 0u || v.relEnd[ghost] <= 1u) return;
    const float gain = v.currentGain[ghost];
    const float mixL = v.mixGainL[ghost];
    const float mixR = v.mixGainR[ghost];
    const float outgoingLevel =
        std::fabs(gain) * (std::fabs(mixL) + std::fabs(mixR));
    if (outgoingLevel <= VoiceRetireThreshold()) return;
    tail.phase = v.phases[ghost];
    tail.phaseInc = v.phaseIncs[ghost];
    tail.gain = gain;
    tail.mixGainL = mixL;
    tail.mixGainR = mixR;
    tail.filterA0 = v.filterA0[ghost];
    tail.filterB1 = v.filterB1[ghost];
    tail.filterB2 = v.filterB2[ghost];
    tail.filterZ1 = v.filterZ1[ghost];
    tail.filterZ2 = v.filterZ2[ghost];
    tail.filterEnabled = static_cast<uint8_t>(v.filterEnabled[ghost] != 0u);
    tail.sampleStart = v.sampleStart[ghost];
    tail.relEnd = v.relEnd[ghost];
    tail.relLoopS = v.relLoopS[ghost];
    tail.relLoopE = v.relLoopE[ghost];
    tail.relLoopSF = v.relLoopSF[ghost];
    tail.relLoopEF = v.relLoopEF[ghost];
    tail.sampleBacked = v.sampleBacked[ghost];
    tail.loopEnabled = v.loopEnabled[ghost];
    tail.framesRemaining = kStealFadeFrames;
    tail.framesTotal = kStealFadeFrames;
    if (v.rot != nullptr) tail.rot = v.rot[ghost];
}

// Stage one ghost tail record into the job's single-row scratch pool and
// render its fade through the shared RenderStealTailSpan math.
inline void RenderScalar::RenderGhostTailSpan(uint32_t ghost, VoiceSoA& s,
                                              const int16_t* sampleData,
                                              uint32_t sampleDataFrames,
                                              float* outL, float* outR,
                                              uint32_t frameStart,
                                              uint32_t frameCount) {
    const WholeVoiceGhostTail& tail = wvGhostTails_[ghost];
    if (tail.framesRemaining == 0u) return;
    s.stealTailPhase[0] = tail.phase;
    s.stealTailPhaseInc[0] = tail.phaseInc;
    s.stealTailGain[0] = tail.gain;
    s.stealTailMixGainL[0] = tail.mixGainL;
    s.stealTailMixGainR[0] = tail.mixGainR;
    s.stealTailSampleStart[0] = tail.sampleStart;
    s.stealTailRelEnd[0] = tail.relEnd;
    s.stealTailRelLoopS[0] = tail.relLoopS;
    s.stealTailRelLoopE[0] = tail.relLoopE;
    s.stealTailRelLoopSF[0] = tail.relLoopSF;
    s.stealTailRelLoopEF[0] = tail.relLoopEF;
    s.stealTailSampleBacked[0] = tail.sampleBacked;
    s.stealTailLoopEnabled[0] = tail.loopEnabled;
    s.stealTailFramesRemaining[0] = tail.framesRemaining;
    s.stealTailFramesTotal[0] = tail.framesTotal;
    if (s.rot != nullptr) s.stealTailRot[0] = tail.rot;
    RenderStealTailSpan(s, 0, sampleData, wvHilbertData_, sampleDataFrames,
                        outL, outR, frameStart, frameCount);
    wvGhostTails_[ghost].framesRemaining = s.stealTailFramesRemaining[0];
    // Rotation phase/angle advanced with the fade; adoption continues it.
    if (s.rot != nullptr) wvGhostTails_[ghost].rot = s.stealTailRot[0];
}

inline void RenderScalar::WholeVoiceJobThunk(uint32_t jobIndex,
                                             const IndexedJobMix& mix,
                                             uint32_t frameCount,
                                             void* userData) {
    WholeVoiceJobContext* ctx = static_cast<WholeVoiceJobContext*>(userData);
    RenderScalar* renderer = ctx->renderer;
    VoiceSoA& v = ctx->voices->v;
    const uint32_t itemCount = ctx->voiceItemCount + ctx->ghostItemCount;
    const uint32_t begin = itemCount * jobIndex / ctx->jobCount;
    const uint32_t end = itemCount * (jobIndex + 1u) / ctx->jobCount;
    renderer->wvJobRetireCounts_[jobIndex * kWvRetireCountStride] = 0u;
    for (uint32_t item = begin; item < end; ++item) {
        if (item < ctx->voiceItemCount) {
            const uint32_t handle = renderer->wvSliceHandles_[item];
            if (v.state[handle] == static_cast<uint8_t>(VoiceState::Free))
                continue;
            // Muted (decimated) carried-over row with one plain whole-block
            // segment: nothing mixes, no release/op/excite/vibrato boundary
            // can split it, so skip the per-item timeline setup (op window,
            // bend-ratio powf, excite walk, batch probing) and go straight
            // to the closed-form advance.  Same call the general path ends
            // in for this row, so state and retire frames are identical.
            if (renderer->wvDecimating_ && renderer->wvMuted_[handle] != 0u &&
                renderer->wvStartFrame_[handle] == UINT32_MAX &&
                renderer->wvReleaseFrame_[handle] == UINT32_MAX &&
                !renderer->wvScalarVibrato_) {
                const uint8_t fastChannelRaw = v.channel[handle];
                const uint32_t fastChannel = fastChannelRaw < kChannelCount
                    ? fastChannelRaw : kChannelCount;
                if (renderer->wvChanOpStart_[fastChannel] ==
                        renderer->wvChanOpStart_[fastChannel + 1u] &&
                    !renderer->wvChanVibratoActive_[fastChannel] &&
                    (renderer->wvExciteHead_ == nullptr ||
                     handle >= renderer->wvExciteCapacity_ ||
                     renderer->wvExciteHead_[handle] == UINT32_MAX)) {
                    if (renderer->wvCullBlocks_ != 0u &&
                        v.state[handle] ==
                            static_cast<uint8_t>(VoiceState::Releasing)) {
                        // Muted-tail cull: a run of consecutive muted blocks
                        // (block numbers start at 1, so 0 = never muted).
                        const uint32_t blk = renderer->wholeVoiceBlocks_;
                        const uint32_t lastMuted = renderer->wvMutedLast_[handle];
                        if (lastMuted == 0u || lastMuted + 1u != blk)
                            renderer->wvMutedSince_[handle] = blk;
                        renderer->wvMutedLast_[handle] = blk;
                        if (blk - renderer->wvMutedSince_[handle] >=
                            renderer->wvCullBlocks_) {
                            // Retire at frame 0 through the normal list; the
                            // row was never going to mix again this block.
                            renderer->wvJobRetirements_[
                                static_cast<size_t>(jobIndex) *
                                    renderer->scratchCapacity_ +
                                renderer->wvJobRetireCounts_[
                                    jobIndex * kWvRetireCountStride]++] = {
                                handle, 0u,
                                ctx->voices->activePosition_[handle]};
                            continue;
                        }
                    }
                    renderer->RenderWholeVoiceSegment(
                        v, ctx->voices, handle, ctx->sampleData,
                        ctx->sampleDataFrames, mix.outputLeft,
                        mix.outputRight, 0u, frameCount, true, jobIndex,
                        0.0f, 1.0f, false);
                    continue;
                }
            }
            // Common whole-block case: consecutive carried/new voices with
            // the same single segment can share one class-kernel dispatch.
            // Runs never cross a job boundary, and handles remain in active-
            // list order to preserve scalar mixing exactly.
            uint32_t batchStart = renderer->wvStartFrame_[handle];
            if (batchStart == UINT32_MAX) batchStart = 0u;
            const uint8_t batchChannelRaw = v.channel[handle];
            const uint32_t batchChannel = batchChannelRaw < kChannelCount
                ? batchChannelRaw : kChannelCount;
            const VoiceRenderClass batchClass =
                ClassifyWholeVoiceRow(v, handle);
            const uint32_t batchFilterEnabled = v.filterEnabled[handle];
            const bool batchHeadEligible = batchStart < frameCount &&
                frameCount - batchStart >= 8u &&
                ctx->sampleData != nullptr &&
                renderer->kernelSet_->kernels[
                    static_cast<uint32_t>(batchClass)] != nullptr &&
                renderer->wvReleaseFrame_[handle] == UINT32_MAX &&
                (renderer->wvExciteHead_ == nullptr ||
                 handle >= renderer->wvExciteCapacity_ ||
                 renderer->wvExciteHead_[handle] == UINT32_MAX) &&
                renderer->wvChanOpStart_[batchChannel] ==
                    renderer->wvChanOpStart_[batchChannel + 1u] &&
                !renderer->wvScalarVibrato_ &&
                !renderer->WholeVoiceRowMuted(handle) &&
                !renderer->wvChanVibratoActive_[batchChannel];
            if (batchHeadEligible) {
                uint32_t batchEnd = item + 1u;
                while (batchEnd < end &&
                       batchEnd < ctx->voiceItemCount &&
                       batchEnd - item < 256u) {
                    const uint32_t next =
                        renderer->wvSliceHandles_[batchEnd];
                    if (v.state[next] ==
                        static_cast<uint8_t>(VoiceState::Free) ||
                        renderer->WholeVoiceRowMuted(next) ||
                        renderer->wvReleaseFrame_[next] != UINT32_MAX ||
                        (renderer->wvExciteHead_ != nullptr &&
                         next < renderer->wvExciteCapacity_ &&
                         renderer->wvExciteHead_[next] != UINT32_MAX))
                        break;
                    uint32_t nextStart = renderer->wvStartFrame_[next];
                    if (nextStart == UINT32_MAX) nextStart = 0u;
                    const uint8_t nextChannelRaw = v.channel[next];
                    const uint32_t nextChannel =
                        nextChannelRaw < kChannelCount
                            ? nextChannelRaw : kChannelCount;
                    if (nextStart != batchStart ||
                        ClassifyWholeVoiceRow(v, next) != batchClass ||
                        v.filterEnabled[next] != batchFilterEnabled ||
                        renderer->wvChanOpStart_[nextChannel] !=
                            renderer->wvChanOpStart_[nextChannel + 1u] ||
                        renderer->wvChanVibratoActive_[nextChannel])
                        break;
                    ++batchEnd;
                }
                const uint32_t batchCount = batchEnd - item;
                if (renderer->RenderWholeVoiceBatch(
                        v, *ctx->voices,
                        renderer->wvSliceHandles_ + item, batchCount,
                        batchClass, ctx->sampleData,
                        ctx->sampleDataFrames, mix, batchStart,
                        frameCount - batchStart, jobIndex)) {
                    item = batchEnd - 1u;
                    continue;
                }
            }
            // Bus mode: this item's channel plane is its mix destination.
            float* itemLeft = mix.outputLeft;
            float* itemRight = mix.outputRight;
            if (mix.busLeft != nullptr) {
                const uint32_t channel = v.channel[handle];
                const uint32_t safeChannel =
                    channel < kChannelCount ? channel : 0u;
                itemLeft = mix.busLeft[safeChannel];
                itemRight = mix.busRight[safeChannel];
            }
            uint32_t start = renderer->wvStartFrame_[handle];
            if (start == UINT32_MAX) start = 0u;
            uint32_t releaseFrame = renderer->wvReleaseFrame_[handle];
            // Channel row-op window: bend/mix/vibrato ops at frames inside
            // the voice's timeline split it exactly where the frame-major
            // renderers fold/bend inline at their dispatch boundaries.
            const uint8_t opChannelRaw = v.channel[handle];
            const uint32_t opChannel =
                opChannelRaw < kChannelCount ? opChannelRaw : kChannelCount;
            uint32_t opIdx = renderer->wvChanOpStart_[opChannel];
            const uint32_t opEnd = renderer->wvChanOpStart_[opChannel + 1];
            // Per-row vibrato/bend cursors: depth and bend ratio start from
            // the block-start channel snapshot and follow the op log.  Ops
            // skipped below (before the voice's window, or dispatched
            // before its same-frame launch) are already baked into the
            // row's configure-time state — tracking them keeps the cursors
            // truthful for later LFO window rebuilds.
            float vDepth = renderer->wvChanStartDepth_[opChannel];
            float vRatio;
            {
                const float rowScale = v.pitchBendScales[handle];
                const float startRatio =
                    renderer->wvChanStartBendRatio_[opChannel];
                vRatio = rowScale == 1.0f && startRatio > 0.0f
                    ? startRatio
                    : powf(2.0f, renderer->wvChanStartBendCents_[opChannel] *
                                      rowScale / 1200.0f);
            }
            while (opIdx < opEnd &&
                   renderer->wvChanOps_[opIdx].frame < start) {
                const WholeVoiceChannelOp& op =
                    renderer->wvChanOps_[opIdx];
                if (op.kind == 2u) {
                    vDepth = op.a;
                } else if (op.kind == 0u) {
                    const float scale = v.pitchBendScales[handle];
                    vRatio = scale == 1.0f ? op.b
                        : powf(2.0f, op.a * scale / 12.0f);
                }
                ++opIdx;
            }
            // Same-frame qualification: an op dispatched in the same frame
            // BEFORE this voice's launch never observed it (the voice reads
            // the fresh channel state at configure time instead).  Carried
            // over voices (UINT32_MAX) hear every op in the block.
            const uint32_t startIngress = renderer->wvStartIngress_[handle];
            if (startIngress != UINT32_MAX) {
                while (opIdx < opEnd &&
                       renderer->wvChanOps_[opIdx].frame == start &&
                       renderer->wvChanOps_[opIdx].ingress < startIngress) {
                    const WholeVoiceChannelOp& op =
                        renderer->wvChanOps_[opIdx];
                    if (op.kind == 2u) {
                        vDepth = op.a;
                    } else if (op.kind == 0u) {
                        const float scale = v.pitchBendScales[handle];
                        vRatio = scale == 1.0f ? op.b
                            : powf(2.0f, op.a * scale / 12.0f);
                    }
                    ++opIdx;
                }
            }
            // Re-excite hits on this generation, in frame order: each one
            // splits the timeline and restarts the row's envelope at its
            // exact frame, before the release flip a later-ordinal note-off
            // may land on the same frame (an earlier one would have unlinked
            // the voice from the key chain, so no hit exists for it).
            uint32_t exIdx = UINT32_MAX;
            const uint32_t rowPlayIndex = v.playIndex[handle];
            if (renderer->wvExciteHead_ != nullptr &&
                handle < renderer->wvExciteCapacity_) {
                exIdx = renderer->WholeVoiceSkipForeignExcites(
                    renderer->wvExciteHead_[handle], rowPlayIndex);
            }
            const uint32_t exciteFrames =
                ctx->voices->ReExciteAttackFrames();
            if (exIdx != UINT32_MAX) {
                // Undo the pre-pass overlay: render from the block-start
                // envelope and replay each hit at its exact frame.
                const WholeVoiceExcite& first = renderer->wvExcites_[exIdx];
                v.envelopeStage[handle] = first.savedStage;
                v.attackSamplesRemaining[handle] = first.savedAttackRemaining;
                v.decaySamplesRemaining[handle] = first.savedDecayRemaining;
                v.attackGainStep[handle] = first.savedAttackStep;
            }
            // A hit that lands while the previous hit's attack ramp is still
            // running changes nothing (the stage is already attack), so it
            // neither splits the timeline nor needs replaying: a flood of
            // repeats costs one segment per ramp, not one per hit.
            uint32_t exSkipUntil = 0u;
            uint32_t cursor = start;
            while (cursor < frameCount) {
                while (exIdx != UINT32_MAX &&
                       renderer->wvExcites_[exIdx].frame < exSkipUntil) {
                    exIdx = renderer->WholeVoiceSkipForeignExcites(
                        renderer->wvExcites_[exIdx].next, rowPlayIndex);
                }
                uint32_t stop = frameCount;
                if (releaseFrame != UINT32_MAX && releaseFrame < stop)
                    stop = releaseFrame;
                if (opIdx < opEnd &&
                    renderer->wvChanOps_[opIdx].frame < stop)
                    stop = renderer->wvChanOps_[opIdx].frame;
                if (exIdx != UINT32_MAX &&
                    renderer->wvExcites_[exIdx].frame < stop)
                    stop = renderer->wvExcites_[exIdx].frame;
                if (stop > cursor) {
                    // Vibrato gating per segment (only on channels where a
                    // depth exists at block start or arrives mid-block): an
                    // LFO-bearing row renders with kernel-internal 64-frame
                    // windows while its channel depth is positive (the
                    // kernels advance the free-running LFO phase); while
                    // delayed or unmodulated the phase free-runs linearly
                    // here instead.  A delay that exhausts mid-segment
                    // splits the segment so the modulation wakes at its
                    // exact frame.
                    bool lfoActive = false;
                    uint32_t segFrames = stop - cursor;
                    if (!renderer->wvScalarVibrato_ && !renderer->wvScalarNoVibrato_ &&
                        renderer->wvChanVibratoActive_[opChannel] &&
                        v.vibLfoSteps[handle] != 0.0f &&
                        v.vibLfoToPitchCents[handle] > 0.0f) {
                        const uint32_t delay = v.vibLfoDelays[handle];
                        if (delay != 0u) {
                            if (delay >= segFrames) {
                                v.vibLfoDelays[handle] = delay - segFrames;
                            } else {
                                v.vibLfoDelays[handle] = 0u;
                                stop = cursor + delay;
                                segFrames = delay;
                            }
                        } else if (vDepth > 0.0f) {
                            lfoActive = true;
                        } else {
                            float lp = v.vibLfoPhases[handle] +
                                v.vibLfoSteps[handle] *
                                    static_cast<float>(segFrames);
                            lp -= std::floor(lp);
                            v.vibLfoPhases[handle] = lp;
                        }
                    }
                    if (renderer->RenderWholeVoiceSegmentLfo(
                            v, ctx->voices, handle, ctx->sampleData,
                            ctx->sampleDataFrames, itemLeft, itemRight,
                            cursor, stop - cursor, true, jobIndex,
                            vDepth, vRatio, lfoActive)) {
                        break;  // retired; coordinator applies it
                    }
                    cursor = stop;
                }
                while (exIdx != UINT32_MAX &&
                       renderer->wvExcites_[exIdx].frame <= cursor) {
                    if (renderer->wvExcites_[exIdx].frame == cursor &&
                        cursor >= exSkipUntil) {
                        ExciteVoiceRow(v, handle, exciteFrames);
                        if (v.envelopeStage[handle] == 1u &&
                            v.attackSamplesRemaining[handle] != 0u) {
                            exSkipUntil =
                                cursor + v.attackSamplesRemaining[handle];
                        }
                    }
                    exIdx = renderer->WholeVoiceSkipForeignExcites(
                        renderer->wvExcites_[exIdx].next, rowPlayIndex);
                }
                if (releaseFrame != UINT32_MAX && cursor == releaseFrame &&
                    cursor < frameCount) {
                    // Exact-frame deferred release flip.
                    v.state[handle] =
                        static_cast<uint8_t>(VoiceState::Releasing);
                    if (v.loopMode[handle] == 3u)
                        v.loopEnabled[handle] = 0u;
                    releaseFrame = UINT32_MAX;
                }
                // Apply every op landing on this frame (ingress order).
                while (opIdx < opEnd &&
                       renderer->wvChanOps_[opIdx].frame == cursor) {
                    const WholeVoiceChannelOp& op =
                        renderer->wvChanOps_[opIdx];
                    if (op.kind == 2u) {
                        vDepth = op.a;
                        if (!(vDepth > 0.0f) &&
                            v.vibLfoModulated[handle] != 0u) {
                            // Modulation ended: restore the exact
                            // unmodulated increment once.
                            v.vibLfoModulated[handle] = 0u;
                            v.phaseIncs[handle] =
                                v.basePhaseIncs[handle] * vRatio;
                        }
                    } else {
                        if (op.kind == 0u) {
                            const float scale = v.pitchBendScales[handle];
                            vRatio = scale == 1.0f ? op.b
                                : powf(2.0f, op.a * scale / 12.0f);
                        }
                        renderer->ApplyWholeVoiceRowOp(v, handle, op);
                    }
                    ++opIdx;
                }
            }
        } else {
            const uint32_t ghost = item - ctx->voiceItemCount;
            if (ghost >= renderer->wvGhostCount_) continue;
            const uint32_t start = renderer->wvGhostStartFrame_[ghost];
            const uint32_t death = renderer->wvGhostDeathFrame_[ghost];
            if (start >= frameCount) continue;
            uint32_t releaseFrame = renderer->wvGhostReleaseFrame_[ghost];
            VoiceSoA& g = renderer->wvGhostState_;
            // Bus mode: the ghost's snapshot row carries its channel.
            float* itemLeft = mix.outputLeft;
            float* itemRight = mix.outputRight;
            if (mix.busLeft != nullptr) {
                const uint32_t channel = g.channel[ghost];
                const uint32_t safeChannel =
                    channel < kChannelCount ? channel : 0u;
                itemLeft = mix.busLeft[safeChannel];
                itemRight = mix.busRight[safeChannel];
            }
            // Ghosts walk the same channel ops within their own window, so
            // a displaced or killed voice hears the bend/fold events that
            // fired while it was still sounding.  Ops at (or past) the
            // death frame never apply — the voice is gone by then.
            const uint8_t opChannelRaw = g.channel[ghost];
            const uint32_t opChannel =
                opChannelRaw < kChannelCount ? opChannelRaw : kChannelCount;
            uint32_t opIdx = renderer->wvChanOpStart_[opChannel];
            const uint32_t opEnd = renderer->wvChanOpStart_[opChannel + 1];
            // Same per-row vibrato/bend cursors as real rows; skipped ops
            // are baked into the ghost's captured state and only update the
            // cursors.
            float vDepth = renderer->wvChanStartDepth_[opChannel];
            float vRatio;
            {
                const float rowScale = g.pitchBendScales[ghost];
                const float startRatio =
                    renderer->wvChanStartBendRatio_[opChannel];
                vRatio = rowScale == 1.0f && startRatio > 0.0f
                    ? startRatio
                    : powf(2.0f, renderer->wvChanStartBendCents_[opChannel] *
                                      rowScale / 1200.0f);
            }
            while (opIdx < opEnd &&
                   renderer->wvChanOps_[opIdx].frame < start) {
                const WholeVoiceChannelOp& op =
                    renderer->wvChanOps_[opIdx];
                if (op.kind == 2u) {
                    vDepth = op.a;
                } else if (op.kind == 0u) {
                    const float scale = g.pitchBendScales[ghost];
                    vRatio = scale == 1.0f ? op.b
                        : powf(2.0f, op.a * scale / 12.0f);
                }
                ++opIdx;
            }
            const uint32_t startIngress =
                renderer->wvGhostStartIngress_[ghost];
            if (startIngress != UINT32_MAX) {
                while (opIdx < opEnd &&
                       renderer->wvChanOps_[opIdx].frame == start &&
                       renderer->wvChanOps_[opIdx].ingress < startIngress) {
                    const WholeVoiceChannelOp& op =
                        renderer->wvChanOps_[opIdx];
                    if (op.kind == 2u) {
                        vDepth = op.a;
                    } else if (op.kind == 0u) {
                        const float scale = g.pitchBendScales[ghost];
                        vRatio = scale == 1.0f ? op.b
                            : powf(2.0f, op.a * scale / 12.0f);
                    }
                    ++opIdx;
                }
            }
            // Re-excite hits the ghost took while it was still sounding.
            uint32_t exIdx = UINT32_MAX;
            const uint32_t ghostPlayIndex = g.playIndex[ghost];
            const uint32_t ghostHandle = renderer->wvGhostHandle_[ghost];
            if (renderer->wvExciteHead_ != nullptr &&
                ghostHandle < renderer->wvExciteCapacity_) {
                exIdx = renderer->WholeVoiceSkipForeignExcites(
                    renderer->wvExciteHead_[ghostHandle], ghostPlayIndex);
            }
            const uint32_t exciteFrames =
                ctx->voices->ReExciteAttackFrames();
            if (exIdx != UINT32_MAX) {
                const WholeVoiceExcite& first = renderer->wvExcites_[exIdx];
                g.envelopeStage[ghost] = first.savedStage;
                g.attackSamplesRemaining[ghost] = first.savedAttackRemaining;
                g.decaySamplesRemaining[ghost] = first.savedDecayRemaining;
                g.attackGainStep[ghost] = first.savedAttackStep;
            }
            uint32_t exSkipUntil = 0u;   // see the real-voice loop
            uint32_t cursor = start;
            bool retired = false;
            while (cursor < death && cursor < frameCount) {
                while (exIdx != UINT32_MAX &&
                       renderer->wvExcites_[exIdx].frame < exSkipUntil) {
                    exIdx = renderer->WholeVoiceSkipForeignExcites(
                        renderer->wvExcites_[exIdx].next, ghostPlayIndex);
                }
                uint32_t stop = death < frameCount ? death : frameCount;
                if (releaseFrame != UINT32_MAX && releaseFrame < stop)
                    stop = releaseFrame;
                if (opIdx < opEnd &&
                    renderer->wvChanOps_[opIdx].frame < stop)
                    stop = renderer->wvChanOps_[opIdx].frame;
                if (exIdx != UINT32_MAX &&
                    renderer->wvExcites_[exIdx].frame < stop)
                    stop = renderer->wvExcites_[exIdx].frame;
                if (stop > cursor) {
                    bool lfoActive = false;
                    uint32_t segFrames = stop - cursor;
                    if (!renderer->wvScalarVibrato_ && !renderer->wvScalarNoVibrato_ &&
                        renderer->wvChanVibratoActive_[opChannel] &&
                        g.vibLfoSteps[ghost] != 0.0f &&
                        g.vibLfoToPitchCents[ghost] > 0.0f) {
                        const uint32_t delay = g.vibLfoDelays[ghost];
                        if (delay != 0u) {
                            if (delay >= segFrames) {
                                g.vibLfoDelays[ghost] = delay - segFrames;
                            } else {
                                g.vibLfoDelays[ghost] = 0u;
                                stop = cursor + delay;
                                segFrames = delay;
                            }
                        } else if (vDepth > 0.0f) {
                            lfoActive = true;
                        } else {
                            float lp = g.vibLfoPhases[ghost] +
                                g.vibLfoSteps[ghost] *
                                    static_cast<float>(segFrames);
                            lp -= std::floor(lp);
                            g.vibLfoPhases[ghost] = lp;
                        }
                    }
                    if (renderer->RenderWholeVoiceSegmentLfo(
                            g, nullptr, ghost, ctx->sampleData,
                            ctx->sampleDataFrames, itemLeft, itemRight,
                            cursor, stop - cursor, false, jobIndex,
                            vDepth, vRatio, lfoActive)) {
                        retired = true;
                        break;
                    }
                    cursor = stop;
                }
                while (exIdx != UINT32_MAX &&
                       renderer->wvExcites_[exIdx].frame <= cursor) {
                    if (renderer->wvExcites_[exIdx].frame == cursor &&
                        cursor < death && cursor >= exSkipUntil) {
                        ExciteVoiceRow(g, ghost, exciteFrames);
                        if (g.envelopeStage[ghost] == 1u &&
                            g.attackSamplesRemaining[ghost] != 0u) {
                            exSkipUntil =
                                cursor + g.attackSamplesRemaining[ghost];
                        }
                    }
                    exIdx = renderer->WholeVoiceSkipForeignExcites(
                        renderer->wvExcites_[exIdx].next, ghostPlayIndex);
                }
                if (releaseFrame != UINT32_MAX && cursor == releaseFrame) {
                    g.state[ghost] =
                        static_cast<uint8_t>(VoiceState::Releasing);
                    if (g.loopMode[ghost] == 3u)
                        g.loopEnabled[ghost] = 0u;
                    releaseFrame = UINT32_MAX;
                }
                while (opIdx < opEnd &&
                       renderer->wvChanOps_[opIdx].frame == cursor &&
                       cursor < death) {
                    const WholeVoiceChannelOp& op =
                        renderer->wvChanOps_[opIdx];
                    if (op.kind == 2u) {
                        vDepth = op.a;
                        if (!(vDepth > 0.0f) &&
                            g.vibLfoModulated[ghost] != 0u) {
                            g.vibLfoModulated[ghost] = 0u;
                            g.phaseIncs[ghost] =
                                g.basePhaseIncs[ghost] * vRatio;
                        }
                    } else {
                        if (op.kind == 0u) {
                            const float scale = g.pitchBendScales[ghost];
                            vRatio = scale == 1.0f ? op.b
                                : powf(2.0f, op.a * scale / 12.0f);
                        }
                        renderer->ApplyWholeVoiceRowOp(g, ghost, op);
                    }
                    ++opIdx;
                }
            }
            if (retired || cursor < death) continue;  // ghost ended early
            if (renderer->wvGhostNoTail_[ghost]) {
                // All Sound Off kill reached: hard stop, no fade tail.
                // (The tail record was zeroed at capture; nothing to adopt.)
                continue;
            }
            // Steal frame reached: reproduce the 64-frame tail fade from
            // the ghost's exact steal-frame state, then keep rendering it.
            renderer->CaptureGhostTail(ghost);
            renderer->RenderGhostTailSpan(ghost,
                                          renderer->wvTailScratch_[jobIndex],
                                          ctx->sampleData,
                                          ctx->sampleDataFrames, itemLeft,
                                          itemRight, death,
                                          frameCount - death);
        }
    }
}

inline void RenderScalar::RenderWholeVoiceBlock(
    VoiceManager& voices, const int16_t* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t numFrames, uint64_t blockStartFrame,
    float* const* channelBusLeft, float* const* channelBusRight) {
    const uint32_t activeCount = voices.GetActiveCount();
    ++wholeVoiceBlocks_;
    std::memcpy(wvSliceHandles_, voices.activeList_,
                static_cast<size_t>(activeCount) * sizeof(uint32_t));
    // Decimation: rank the active voices by velocity once and mute the
    // quietest beyond activeCount / step for this block.  Muted rows keep
    // their state advancing (scalar span with nothing mixed), so they retire
    // and resume exactly as they would unmuted.
    wvDecimating_ = false;
    wvCullBlocks_ = mutedCullBlocks_.load(std::memory_order_relaxed);
    if (wvMutedSince_.size() < wvMuted_.size()) wvCullBlocks_ = 0u;
    if (wvMutedDirty_) {
        std::fill(wvMuted_.begin(), wvMuted_.end(), uint8_t(0));
        wvMutedDirty_ = false;
    }
    if (activeCount > 1u) {
        const uint32_t step = DecimationStep(activeCount);
        const uint32_t keep = activeCount / step;
        if (step > 1u && keep < activeCount &&
            wvMuted_.size() >= voices.v.GetCapacity()) {
            const VoiceSoA& rows = voices.v;
            // Keep the `keep` loudest voices, ties broken toward the lower
            // handle. A velocity histogram finds the threshold velocity in one
            // pass; only the voices sitting exactly on it need an order. The
            // muted set is identical to a full nth_element ranking by
            // (velocity desc, handle asc), at about a quarter of the cost.
            // Rank key, 0..255, larger = louder.  Velocity by default; with
            // by-level ranking, the float bits of |envelope level| x output
            // gain (monotone for positive floats) at 1/8-octave resolution
            // over 2^-32..1.  Pre-decay voices rank by their target gain, as
            // the steal score does.
            const bool byLevel =
                decimationByLevel_.load(std::memory_order_relaxed);
            auto rankKey = [&](uint32_t handle) -> uint32_t {
                if (!byLevel) return rows.velocity[handle];
                const uint8_t stage = rows.envelopeStage[handle];
                const bool preDecay =
                    rows.state[handle] ==
                        static_cast<uint8_t>(VoiceState::Active) &&
                    (stage == 4u || stage == 0u || stage == 1u);
                const float level =
                    std::fabs(preDecay ? rows.targetGain[handle]
                                       : rows.currentGain[handle]) *
                    rows.stealOutputGain[handle];
                uint32_t bits;
                std::memcpy(&bits, &level, sizeof(bits));
                const uint32_t key = bits >> 20;
                return key <= 760u ? 0u : (key >= 1015u ? 255u : key - 760u);
            };
            uint32_t histogram[256] = {};
            if (byLevel) {
                // The key reads five scattered SoA arrays per voice: compute
                // it once (prefetching ahead) and replay it sequentially.
                wvDecimKey_.resize(activeCount);
                for (uint32_t i = 0u; i < activeCount; ++i) {
#if defined(SVMS_VM_HAS_PREFETCH)
                    if (i + 16u < activeCount) {
                        const uint32_t ahead = wvSliceHandles_[i + 16u];
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.currentGain[ahead]), _MM_HINT_T0);
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.stealOutputGain[ahead]), _MM_HINT_T0);
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.targetGain[ahead]), _MM_HINT_T0);
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.envelopeStage[ahead]), _MM_HINT_T0);
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.state[ahead]), _MM_HINT_T0);
                    }
#endif
                    wvDecimKey_[i] =
                        static_cast<uint8_t>(rankKey(wvSliceHandles_[i]));
                    ++histogram[wvDecimKey_[i]];
                }
            } else {
                for (uint32_t i = 0u; i < activeCount; ++i)
                    ++histogram[rows.velocity[wvSliceHandles_[i]]];
            }
            uint32_t above = 0u;
            uint32_t threshold = 255u;
            for (;; --threshold) {
                if (above + histogram[threshold] >= keep || threshold == 0u)
                    break;
                above += histogram[threshold];
            }
            const uint32_t tiesKept = keep - above;
            wvDecimRank_.clear();
            // Even spread of the kept ties (Bresenham over the tie sequence):
            // the render jobs split the active list into contiguous slices,
            // so keeping the first N would pile all the work onto one job.
            const uint32_t levelTieTotal = histogram[threshold];
            uint64_t levelTieSeen = 0u;
            for (uint32_t i = 0u; i < activeCount; ++i) {
                const uint32_t handle = wvSliceHandles_[i];
                const uint32_t velocity =
                    byLevel ? wvDecimKey_[i] : rows.velocity[handle];
                if (velocity < threshold)
                    wvMuted_[handle] = 1u;
                else if (velocity == threshold) {
                    // Level bins are coarse and real pools tie heavily, so
                    // by-level ranking picks its kept ties by an even,
                    // deterministic spread over the list (O(n)) instead of
                    // ordering the tie bin by handle.
                    if (!byLevel) {
                        wvDecimRank_.push_back(handle);
                    } else {
                        const uint64_t before =
                            levelTieSeen * tiesKept / levelTieTotal;
                        ++levelTieSeen;
                        if (levelTieSeen * tiesKept / levelTieTotal == before)
                            wvMuted_[handle] = 1u;
                    }
                }
            }
            if (tiesKept < wvDecimRank_.size()) {
                std::nth_element(wvDecimRank_.begin(),
                                 wvDecimRank_.begin() + tiesKept,
                                 wvDecimRank_.end());
                for (size_t k = tiesKept; k < wvDecimRank_.size(); ++k)
                    wvMuted_[wvDecimRank_[k]] = 1u;
            }
            wvDecimating_ = true;
            wvMutedDirty_ = true;
        }
    }
    const uint32_t ghostCount = wvGhostCount_;
    const uint32_t itemCount = activeCount + ghostCount;
    WholeVoiceJobContext ctx{this, &voices, sampleData, sampleDataFrames,
                             numFrames, activeCount, ghostCount, 1u};
    // Retire-stage input must be zeroed for THIS block: empty blocks
    // (itemCount == 0) dispatch no thunk, so every count slot they will read
    // has to start at zero (thunks re-zero their own slot when they run).
    if (wvJobRetireCounts_ != nullptr && wvJobCapacity_ != 0u)
        std::fill(wvJobRetireCounts_,
                  wvJobRetireCounts_ +
                      static_cast<size_t>(wvJobCapacity_) * kWvRetireCountStride,
                  0u);
    if (itemCount != 0u) {
        // Oversubscribe the pool (jobs are claimed from an atomic queue):
        // one static slice per thread left the slowest core (E-core, noisy
        // neighbour) as the critical path while fast cores idled.  Merge and
        // retirement order are per-job-index, so assignment does not matter.
        uint32_t jobCount = 1u;
        if (workerPool_) {
            const uint32_t threads = workerPool_->GetThreadCount();
            jobCount = (std::min)(threads, itemCount);
            const uint32_t fine = (std::min)(
                (std::min)(threads * kWvJobsPerThread,
                           workerPool_->GetJobCapacity()),
                itemCount / kWvMinItemsPerJob);
            if (fine > jobCount) jobCount = fine;
        }
        if (jobCount < 2u) jobCount = 1u;
        ctx.jobCount = jobCount;
        if (wvDecimating_ && activeCount > 1u &&
            wvPartMixed_.size() >= activeCount)
            PartitionMutedRows(activeCount, itemCount, jobCount);
#if defined(_MSC_VER)
        const uint64_t wvJobBegin = __rdtsc();
#endif
        bool dispatched = false;
        if (jobCount >= 2u && workerPool_ != nullptr) {
            dispatched = workerPool_->ExecuteIndexed(
                jobCount, numFrames, outputLeft, outputRight,
                &WholeVoiceJobThunk, &ctx, channelBusLeft, channelBusRight);
        }
        ++(dispatched ? wvParallelJobs_ : wvSerialJobs_);
        if (!dispatched) {
            // Serial: one job over every item, mixing directly.  In bus
            // mode the serial job writes the block buses (sequential, so
            // no per-job planes are needed).
            ctx.jobCount = 1u;
            IndexedJobMix serialMix{outputLeft, outputRight,
                                    channelBusLeft, channelBusRight};
            WholeVoiceJobThunk(0u, serialMix, numFrames, &ctx);
        }
#if defined(_MSC_VER)
        wvJobCycles_ += __rdtsc() - wvJobBegin;
#endif
    }

    // Retirement in exact (frame, capture-position) order — identical to
    // the frame-major oracle's inline retirement.
    uint32_t retireTotal = 0u;
    for (uint32_t job = 0u; job < ctx.jobCount; ++job) {
        const uint32_t count = wvJobRetireCounts_[job * kWvRetireCountStride];
        std::memcpy(retirements_ + retireTotal,
                    wvJobRetirements_ + static_cast<size_t>(job) *
                        scratchCapacity_,
                    static_cast<size_t>(count) * sizeof(SpanRetirement));
        retireTotal += count;
    }
    std::sort(retirements_, retirements_ + retireTotal,
        [](const SpanRetirement& a, const SpanRetirement& b) {
            if (a.frameOffset != b.frameOffset)
                return a.frameOffset < b.frameOffset;
            return a.capturePosition < b.capturePosition;
        });
    for (uint32_t i = 0u; i < retireTotal; ++i) {
        voices.SetCurrentFrame(blockStartFrame + retirements_[i].frameOffset);
        const VoiceHandle retiring =
            static_cast<VoiceHandle>(retirements_[i].handle);
        // A voice released and retired inside this block flipped to
        // Releasing in the worker, but its releasing-counter increment is
        // still pending in the deferred-release list.  Finalize it first so
        // RetireVoice's decrement balances (else the counter underflows).
        const uint32_t opSlot = wvOpIndex_[retiring];
        if (opSlot < wvOpCount_ && wvOpHandles_[opSlot] == retiring)
            voices.FinalizeDeferredRelease(retiring);
        voices.RetireVoice(retiring);
    }

#if defined(_MSC_VER)
    const uint64_t wvPostBegin = __rdtsc();
#endif
    // Deferred release flips: counters, Releasing-ring and class lists.
    for (uint32_t i = 0u; i < wvOpCount_; ++i)
        voices.FinalizeDeferredRelease(
            static_cast<VoiceHandle>(wvOpHandles_[i]));

    // Render-class lists track lifecycle (transient→sustained crossings
    // included).
    for (uint32_t i = 0u; i < activeCount; ++i)
        voices.RefreshRenderClass(static_cast<VoiceHandle>(wvSliceHandles_[i]));

    // Row-op channels had no masks set during the pre-pass (a launch-entry
    // steal fold would have corrupted pre-CC ghost captures), so refresh
    // their steal candidates here with writeRenderGain=false: the workers
    // already wrote mixGain/renderGain at the op frames with the exact
    // currentGain vintage the oracle's batch-boundary folds produce, and
    // the scales have not moved since — the value writes are idempotent,
    // only the cached candidate keys move.
    voices.FoldStealGainsForChannels(wvRowOpChannelMask_);

    // Pre-existing steal tails (born before this block) render exactly as
    // the oracle's tail section does.  A tail killed by an in-block
    // All Sound Off was unlinked during the pre-pass, but UnlinkStealTail
    // preserves its fields: render the audio up to the recorded kill frame,
    // then stop exactly where live dispatch would have stopped it.
#if defined(_MSC_VER)
    wvPostCycles_ += __rdtsc() - wvPostBegin;
#endif
    const uint32_t voiceCapacity = voices.GetMaxVoices();
    const uint32_t tailCapacity =
        (std::min)(voiceCapacity, static_cast<uint32_t>(kStealTailReserve));
    for (uint32_t slot = 0u; slot < tailCapacity; ++slot) {
        const uint32_t killFrame = wvTailKillFrame_[slot];
        float* tailOutLeft = outputLeft;
        float* tailOutRight = outputRight;
        if (channelBusLeft != nullptr) {
            const uint32_t channel = voices.v.stealTailChannel[slot];
            const uint32_t safeChannel =
                channel < kChannelCount ? channel : 0u;
            tailOutLeft = channelBusLeft[safeChannel];
            tailOutRight = channelBusRight[safeChannel];
        }
        if (voices.v.stealTailFramesRemaining[slot] == 0u) {
            if (killFrame == UINT32_MAX) continue;
            RenderStealTailSpan(voices.v, slot, sampleData, wvHilbertData_,
                                sampleDataFrames,
                                tailOutLeft, tailOutRight, 0u, killFrame);
            continue;
        }
        // A live tail never carries a kill frame; the min() only defends
        // against a plan/render mismatch.
        const uint32_t tailFrames = killFrame == UINT32_MAX
            ? numFrames : (std::min)(killFrame, numFrames);
        RenderStealTailSpan(voices.v, slot, sampleData, wvHilbertData_,
                            sampleDataFrames,
                            tailOutLeft, tailOutRight, 0u, tailFrames);
        voices.RefreshStealTail(static_cast<VoiceHandle>(slot));
    }

    // Ghost tails that outlive the block are adopted into real tail slots
    // so their fade continues into the next block exactly as an in-band
    // capture would.
    for (uint32_t ghost = 0u; ghost < ghostCount; ++ghost) {
        const WholeVoiceGhostTail& tail = wvGhostTails_[ghost];
        if (tail.framesRemaining == 0u) continue;
        if (wvGhostDeathFrame_[ghost] >= numFrames) continue;
        for (uint32_t slot = 0u; slot < tailCapacity; ++slot) {
            if (voices.v.stealTailFramesRemaining[slot] != 0u) continue;
            voices.v.stealTailPhase[slot] = tail.phase;
            voices.v.stealTailPhaseInc[slot] = tail.phaseInc;
            voices.v.stealTailGain[slot] = tail.gain;
            voices.v.stealTailMixGainL[slot] = tail.mixGainL;
            voices.v.stealTailMixGainR[slot] = tail.mixGainR;
            voices.v.stealTailFilterA0[slot] = tail.filterA0;
            voices.v.stealTailFilterB1[slot] = tail.filterB1;
            voices.v.stealTailFilterB2[slot] = tail.filterB2;
            voices.v.stealTailFilterZ1[slot] = tail.filterZ1;
            voices.v.stealTailFilterZ2[slot] = tail.filterZ2;
            voices.v.stealTailFilterEnabled[slot] = tail.filterEnabled;
            voices.v.stealTailSampleStart[slot] = tail.sampleStart;
            voices.v.stealTailRelEnd[slot] = tail.relEnd;
            voices.v.stealTailRelLoopS[slot] = tail.relLoopS;
            voices.v.stealTailRelLoopE[slot] = tail.relLoopE;
            voices.v.stealTailRelLoopSF[slot] = tail.relLoopSF;
            voices.v.stealTailRelLoopEF[slot] = tail.relLoopEF;
            voices.v.stealTailSampleBacked[slot] = tail.sampleBacked;
            voices.v.stealTailLoopEnabled[slot] = tail.loopEnabled;
            voices.v.stealTailFramesRemaining[slot] = tail.framesRemaining;
            voices.v.stealTailFramesTotal[slot] = tail.framesTotal;
            if (voices.v.rot != nullptr) voices.v.stealTailRot[slot] = tail.rot;
            voices.AdoptStealTailSlot(slot);
            break;
        }
    }

    voices.SetCurrentFrame(blockStartFrame + numFrames);
}

inline bool RenderScalar::PrepareWholeVoice(VoiceManager& voices) {
    const uint32_t handleCapacity = voices.v.GetCapacity();
    return ReserveVoiceCapacity(voices.GetMaxVoices()) &&
           ReserveWholeVoiceStorage(handleCapacity) &&
           EnsureWholeVoiceJobScratch(
               workerPool_ ? workerPool_->GetThreadCount() * kWvJobsPerThread
                           : 1u) &&
           EnsureWholeVoiceRowOpStorage(wvRowOpBudget_) &&
           EnsureWholeVoiceExciteStorage(handleCapacity) &&
           SyncWholeVoiceRotationScratch(voices.v.rot != nullptr);
}

inline bool RenderScalar::ReserveWholeVoiceStorage(uint32_t handleCapacity) {
    const uint32_t cap = scratchCapacity_;
    if (cap == 0u || handleCapacity == 0u) return false;
    if (wvStartFrame_ != nullptr && wvGhostCapacity_ >= cap &&
        wvOpCapacity_ >= cap && wvHandleCapacity_ >= handleCapacity) {
        return true;
    }
    // Per-handle timeline arrays (wvStartFrame_/wvReleaseFrame_) are indexed
    // by RAW voice handle (0..maxVoices-1), which can be far above the active
    // count in a grown or partially-filled pool.  Everything else is indexed
    // by active-count rows (0..scratchCapacity_-1).  Allocate all-or-nothing
    // so a partial failure cannot leave mixed-size arrays behind.
    auto allocU32 = [](uint32_t*& pointer, uint32_t count) {
        return static_cast<uint32_t*>(_aligned_malloc(
            static_cast<size_t>(count) * sizeof(uint32_t), kMixBufferAlign));
    };
    uint32_t* freshStart = allocU32(wvStartFrame_, handleCapacity);
    uint32_t* freshRelease = allocU32(wvReleaseFrame_, handleCapacity);
    uint32_t* freshStartIngress = allocU32(wvStartIngress_, handleCapacity);
    uint32_t* freshOpIndex = allocU32(wvOpIndex_, handleCapacity);
    uint32_t* freshSlice = allocU32(wvSliceHandles_, cap);
    uint32_t* freshOpHandles = allocU32(wvOpHandles_, cap);
    uint32_t* freshOpFrames = allocU32(wvOpFrames_, cap);
    uint32_t* freshGhostStart = allocU32(wvGhostStartFrame_, cap);
    uint32_t* freshGhostDeath = allocU32(wvGhostDeathFrame_, cap);
    uint32_t* freshGhostRelease = allocU32(wvGhostReleaseFrame_, cap);
    uint32_t* freshGhostIngress = allocU32(wvGhostStartIngress_, cap);
    uint32_t* freshGhostHandle = allocU32(wvGhostHandle_, cap);
    uint8_t* freshGhostNoTail = static_cast<uint8_t*>(_aligned_malloc(
        static_cast<size_t>(cap) * sizeof(uint8_t), kMixBufferAlign));
    if (freshStart == nullptr || freshRelease == nullptr ||
        freshStartIngress == nullptr || freshOpIndex == nullptr ||
        freshSlice == nullptr || freshOpHandles == nullptr ||
        freshOpFrames == nullptr || freshGhostStart == nullptr ||
        freshGhostDeath == nullptr || freshGhostRelease == nullptr ||
        freshGhostIngress == nullptr || freshGhostHandle == nullptr ||
        freshGhostNoTail == nullptr) {
        _aligned_free(freshStart);
        _aligned_free(freshRelease);
        _aligned_free(freshStartIngress);
        _aligned_free(freshOpIndex);
        _aligned_free(freshSlice);
        _aligned_free(freshOpHandles);
        _aligned_free(freshOpFrames);
        _aligned_free(freshGhostStart);
        _aligned_free(freshGhostDeath);
        _aligned_free(freshGhostRelease);
        _aligned_free(freshGhostIngress);
        _aligned_free(freshGhostHandle);
        _aligned_free(freshGhostNoTail);
        return false;
    }
    _aligned_free(wvStartFrame_);
    _aligned_free(wvReleaseFrame_);
    _aligned_free(wvStartIngress_);
    _aligned_free(wvSliceHandles_);
    _aligned_free(wvOpHandles_);
    _aligned_free(wvOpFrames_);
    _aligned_free(wvGhostStartFrame_);
    _aligned_free(wvGhostDeathFrame_);
    _aligned_free(wvGhostReleaseFrame_);
    _aligned_free(wvGhostStartIngress_);
    _aligned_free(wvGhostHandle_);
    _aligned_free(wvGhostNoTail_);
    wvStartFrame_ = freshStart;
    wvReleaseFrame_ = freshRelease;
    wvStartIngress_ = freshStartIngress;
    _aligned_free(wvOpIndex_);
    wvOpIndex_ = freshOpIndex;
    wvSliceHandles_ = freshSlice;
    wvOpHandles_ = freshOpHandles;
    wvOpFrames_ = freshOpFrames;
    wvGhostStartFrame_ = freshGhostStart;
    wvGhostDeathFrame_ = freshGhostDeath;
    wvGhostReleaseFrame_ = freshGhostRelease;
    wvGhostStartIngress_ = freshGhostIngress;
    wvGhostHandle_ = freshGhostHandle;
    wvGhostNoTail_ = freshGhostNoTail;
    wvHandleCapacity_ = handleCapacity;
    wvMuted_.assign(handleCapacity, uint8_t(0));
    wvMutedSince_.assign(handleCapacity, 0u);
    wvPartMixed_.assign(handleCapacity, 0u);
    wvPartMuted_.assign(handleCapacity, 0u);
    wvMutedLast_.assign(handleCapacity, 0u);
    if (wvGhostCapacity_ < cap) {
        VoiceSoA grown;
        if (!grown.Reserve(cap)) return false;
        wvGhostState_ = grown;   // copy-assign steals the storage
        wvGhostCapacity_ = cap;
        delete[] wvGhostTails_;
        wvGhostTails_ = new (std::nothrow) WholeVoiceGhostTail[cap];
        if (wvGhostTails_ == nullptr) return false;
    }
    wvOpCapacity_ = cap;
    return true;
}

inline bool RenderScalar::SyncWholeVoiceRotationScratch(bool rotationActive) {
    if (!rotationActive) {
        if (wvGhostState_.rot != nullptr) wvGhostState_.ReleaseRotation();
        for (uint32_t job = 0u; job < wvJobCapacity_; ++job) {
            if (wvTailScratch_[job].rot != nullptr)
                wvTailScratch_[job].ReleaseRotation();
        }
        return true;
    }
    if (wvGhostState_.rot == nullptr &&
        !wvGhostState_.ReserveRotation(wvGhostCapacity_)) {
        return false;
    }
    for (uint32_t job = 0u; job < wvJobCapacity_; ++job) {
        if (wvTailScratch_[job].rot == nullptr &&
            !wvTailScratch_[job].ReserveRotation(1u)) {
            return false;
        }
    }
    return true;
}

inline bool RenderScalar::EnsureWholeVoiceJobScratch(uint32_t jobCount) {
    if (jobCount == 0u) jobCount = 1u;
    if (wvJobCapacity_ >= jobCount && wvJobScratchCapacity_ == scratchCapacity_ &&
        wvJobRetirements_ != nullptr && wvTailScratch_ != nullptr) {
        return true;
    }
    SpanRetirement* retirements = static_cast<SpanRetirement*>(_aligned_malloc(
        static_cast<size_t>(jobCount) * scratchCapacity_ *
            sizeof(SpanRetirement),
        kMixBufferAlign));
    uint32_t* counts = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(jobCount) * kWvRetireCountStride * sizeof(uint32_t),
        kMixBufferAlign));
    VoiceSoA* tailScratch = new (std::nothrow) VoiceSoA[jobCount];
    WvJobSegStats* segStats = static_cast<WvJobSegStats*>(_aligned_malloc(
        static_cast<size_t>(jobCount) * sizeof(WvJobSegStats), 64u));
    if (retirements == nullptr || counts == nullptr ||
        tailScratch == nullptr || segStats == nullptr) {
        _aligned_free(retirements);
        _aligned_free(counts);
        _aligned_free(segStats);
        delete[] tailScratch;
        return false;
    }
    std::memset(segStats, 0, static_cast<size_t>(jobCount) *
                                 sizeof(WvJobSegStats));
    for (uint32_t job = 0u; job < jobCount; ++job) {
        if (!tailScratch[job].Reserve(1u)) {
            _aligned_free(retirements);
            _aligned_free(counts);
            _aligned_free(segStats);
            delete[] tailScratch;
            return false;
        }
    }
    // Counts MUST start zeroed: the thunks zero their own slot only when a
    // block actually dispatches items, and the retire stage reads the counts
    // of every empty block (active=0) too.
    std::memset(counts, 0, static_cast<size_t>(jobCount) *
                               kWvRetireCountStride * sizeof(uint32_t));
    for (uint32_t counter = 0u; counter < kWvSegCounterCount; ++counter)
        wvSegRetired_[counter] = WvSegSum(static_cast<WvSegCounter>(counter));
    _aligned_free(wvJobRetirements_);
    _aligned_free(wvJobRetireCounts_);
    _aligned_free(wvJobSeg_);
    delete[] wvTailScratch_;
    wvJobSeg_ = segStats;
    wvJobRetirements_ = retirements;
    wvJobRetireCounts_ = counts;
    wvTailScratch_ = tailScratch;
    wvJobCapacity_ = jobCount;
    wvJobScratchCapacity_ = scratchCapacity_;
    return true;
}

} // namespace svms

#endif
