#ifndef SVMS_RENDER_SCALAR_H
#define SVMS_RENDER_SCALAR_H

#include "synth/Types.h"
#include "render/RenderEvent.h"
#if defined(_DEBUG) && defined(_MSC_VER)
#include <crtdbg.h>
#else
#endif
#include "synth/VoiceManager.h"
#include "synth/ChannelCache.h"
#include "render/Envelope.h"
#include "events/PageAllocator.h"
#include "render/RenderKernels.h"
#include "render/VoiceFilter.h"
#include "render/RenderWorkers.h"
// GPU render stage (default off; compiled out of the XP build).
#if !defined(SVMS_XP_COMPAT) && !defined(SVMS_NO_GPU_STAGE)
#define SVMS_GPU_STAGE 1
#include "render/GpuStage.h"
#endif
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>
#include <cstdlib>
#include <malloc.h>
#include "synth/SoundFont.h"

namespace svms {

// ── Linear interpolation between two sample frames ──────────────────────
// EXPERIMENT: SVMS_INTERP=cubic switches the scalar path to 4-point Catmull-Rom.
inline const bool g_interpCubic = std::getenv("SVMS_INTERP") != nullptr;
inline float InterpolateSample(const int16_t* data, uint32_t baseIndex,
                                uint32_t nextIndex, float frac) {
    const float s0 = static_cast<float>(data[baseIndex]) * (1.0f / 32768.0f);
    const float s1 = static_cast<float>(data[nextIndex]) * (1.0f / 32768.0f);
    if (!g_interpCubic) return s0 + (s1 - s0) * frac;
    const float sm = static_cast<float>(data[baseIndex ? baseIndex - 1u : 0u]) * (1.0f / 32768.0f);
    const float s2 = static_cast<float>(data[nextIndex + 1u]) * (1.0f / 32768.0f);
    const float c1 = 0.5f * (s1 - sm), c2 = 0.5f * (s2 - s0);
    const float d = s1 - s0;
    return s0 + frac * (c1 + frac * ((3.0f * d - 2.0f * c1 - c2) + frac * (c1 + c2 - 2.0f * d)));
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

    uint64_t pos = phasefx::Pack(v.stealTailPhaseIdx[idx], v.stealTailPhaseFrac[idx]);
    uint32_t baseOffset = phasefx::Index(pos);
    const bool loop = v.stealTailLoopEnabled[idx] != 0;
    const uint64_t loopS64 = phasefx::Pack(v.stealTailRelLoopS[idx], 0u);
    const uint64_t loopE64 = phasefx::Pack(v.stealTailRelLoopE[idx], 0u);

    if (baseOffset + 1u >= relEnd) {
        if (!loop) {
            v.stealTailFramesRemaining[idx] = 0;
            return;
        }
        pos = loopS64;
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

    const float frac = phasefx::Weight(phasefx::Frac(pos));
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

    pos += phasefx::FromFloat(v.stealTailPhaseInc[idx]);
    if (loop) pos = phasefx::Wrap(pos, loopS64, loopE64);

    v.stealTailPhaseIdx[idx] = phasefx::Index(pos);
    v.stealTailPhaseFrac[idx] = phasefx::Frac(pos);
    v.stealTailFramesRemaining[idx] = remaining - 1u;
}

inline void AdvanceStealTailSpanExact(VoiceSoA& v, uint32_t idx,
                                      uint32_t frameCount) {
    uint32_t remaining = v.stealTailFramesRemaining[idx];
    if (remaining == 0u || v.stealTailSampleBacked[idx] == 0u) return;
    uint64_t pos = phasefx::Pack(v.stealTailPhaseIdx[idx], v.stealTailPhaseFrac[idx]);
    const uint64_t step = phasefx::FromFloat(v.stealTailPhaseInc[idx]);
    const uint32_t relEnd = v.stealTailRelEnd[idx];
    const bool loop = v.stealTailLoopEnabled[idx] != 0u;
    const uint64_t loopS64 = phasefx::Pack(v.stealTailRelLoopS[idx], 0u);
    const uint64_t loopE64 = phasefx::Pack(v.stealTailRelLoopE[idx], 0u);
    for (uint32_t frame = 0u; frame < frameCount && remaining != 0u; ++frame) {
        uint32_t base = phasefx::Index(pos);
        if (relEnd < 2u || base + 1u >= relEnd) {
            if (!loop) {
                remaining = 0u;
                break;
            }
            pos = loopS64;
        }
        pos += step;
        if (loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
        --remaining;
    }
    v.stealTailPhaseIdx[idx] = phasefx::Index(pos);
    v.stealTailPhaseFrac[idx] = phasefx::Frac(pos);
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
    uint32_t phaseIdx = 0u;
    uint32_t phaseFrac = 0u;
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
// The volume/pan ops of one channel over a block as a curve (see
// BuildWholeVoiceMixCurves): the AVX2 kernels read it instead of the voice being
// cut at every op.  Built once per block per channel; every voice of the channel
// shares it.
struct WholeVoiceMixCurve {
    const float* left;          // channel mix scale in force at each frame
    const float* right;         // (readable 8 frames past the block)
    uint32_t from;              // frame of the channel's first mix op
    uint32_t lastFrame;         // frame of its last mix op
    uint32_t rowStart;          // frame the row being rendered starts
    const uint32_t* opFrames;   // frames of the channel's mix ops, ascending
    uint32_t opCount;
};

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
    // (ChannelLimiter.h).  Null (default) mixes straight into
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
    std::atomic<uint32_t> bulkAdmissionPercent_{0u};
    bool bendFusion_ = true;
    std::atomic<uint64_t> bendFusedSpans_{0u};
    uint64_t bulkAdmissionDropped_ = 0u;
    std::vector<RenderEvent> admittedEvents_;
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
                                 float lfoBendRatio, bool lfoActive,
                                 const StepBreak* breaks = nullptr,
                                 uint32_t breakCount = 0u,
                                 const WholeVoiceMixCurve* curve = nullptr);
    bool RenderWholeVoiceSegmentLfo(VoiceSoA& state, VoiceManager* voices,
                                    uint32_t row, const int16_t* sampleData,
                                    uint32_t sampleDataFrames, float* outL,
                                    float* outR, uint32_t segStart,
                                    uint32_t segFrames, bool isReal,
                                    uint32_t jobIndex, float lfoDepth,
                                    float lfoBendRatio, bool lfoActive,
                                    const StepBreak* breaks = nullptr,
                                    uint32_t breakCount = 0u,
                                    const WholeVoiceMixCurve* curve = nullptr);
    void BuildWholeVoiceMixCurves(uint32_t numFrames);
    static void SyncMixFromCurve(VoiceSoA& state, uint32_t row,
                                 const WholeVoiceMixCurve& curve,
                                 uint32_t frame);
    void RenderWholeVoiceRow(VoiceManager& voices, uint32_t handle,
                             const int16_t* sampleData,
                             uint32_t sampleDataFrames, float* itemLeft,
                             float* itemRight, uint32_t frameCount,
                             uint32_t jobIndex);
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
    // Fan-out the voice manager uses for big linear passes during the plan.
    static void PlanParallelFor(void* user, uint32_t jobCount,
                                void (*job)(void* context, uint32_t index),
                                void* context);
    static void PlanParallelJobThunk(uint32_t index, const IndexedJobMix& mix,
                                     uint32_t frameCount, void* userData);
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
                              const WholeVoiceChannelOp& op, float bendRatio,
                              bool finalMix);
    static void WholeVoiceJobThunk(uint32_t jobIndex,
                                   const IndexedJobMix& mix,
                                   uint32_t frameCount, void* userData);

#if defined(SVMS_GPU_STAGE)
    struct GpuPassContext;
    GpuStage* gpuStage_ = nullptr;
    uint32_t gpuVoiceEpoch_ = 0u;   // VoiceManager::StaticEpoch last seen
    uint32_t gpuSustainedRows_ = 0u;   // this block: rows on the sustained kernel
    uint32_t gpuEnvRows_ = 0u;         // this block: rows on the envelope kernel
    // GPU rows with no lifecycle change need no render-class refresh: the
    // post loop skips the GPU tail of wvSliceHandles_ and refreshes only the
    // rows that crossed a stage (gpuRefreshHandles_).
    uint32_t gpuSkipRefresh_ = 0u;
    // A block's GPU rows go out in one or two parts (slices of the sustained
    // and envelope lists) so the second part is packed while the first runs.
    struct GpuPart {
        uint32_t sBase, sCount, eBase, eCount;
    };
    GpuPart gpuPart_[gpu::kMaxParts] = {};
    uint32_t gpuPartCount_ = 0u;
    uint32_t gpuSubmittedParts_ = 0u;
    std::vector<uint32_t> gpuRefreshHandles_;
    // A start or release frame inside the block: the row's block is split at it.
    bool GpuRowHasTimeline(uint32_t handle) const {
        return wvStartFrame_[handle] != UINT32_MAX ||
               wvReleaseFrame_[handle] != UINT32_MAX;
    }
    bool GpuRowBlockOk(const VoiceSoA& v, uint32_t handle, bool stagedPending,
                       bool timelineOk) const;
    uint8_t GpuRowKind(const VoiceSoA& v, uint32_t handle, bool stagedPending,
                       uint32_t storeFrames, bool timelineOk) const;
    static void GpuPassThunk(uint32_t job, const IndexedJobMix& mix,
                             uint32_t frameCount, void* userData);
    void RunGpuPass(GpuPassContext& ctx, float* outputLeft, float* outputRight);
    void SortGpuRows(GpuPassContext& ctx, uint32_t* rows, uint32_t count,
                     float* outputLeft, float* outputRight);
    uint32_t TryGpuSubmit(VoiceManager& voices, const int16_t* sampleData,
                          uint32_t sampleDataFrames, float* outputLeft,
                          float* outputRight, uint32_t numFrames,
                          uint32_t activeCount, float* const* channelBusLeft,
                          bool& submitted);
    void FinishGpuRows(VoiceManager& voices, const int16_t* sampleData,
                       uint32_t sampleDataFrames, float* outputLeft,
                       float* outputRight, uint32_t numFrames,
                       uint32_t gpuCount, uint32_t cpuCount, bool submitted);
#endif
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
    // Index in wvChanOps_ of each channel's last mix op (UINT32_MAX = none).
    uint32_t wvChanLastMix_[kChannelCount + 1];
    // Volume/pan curves (BuildWholeVoiceMixCurves): per channel the mix scale in
    // force at every frame, the first and last mix op frames (UINT32_MAX = the
    // channel has none this block), the ascending op frames, and for every op
    // the index of the next op that is not a mix op.
    std::vector<float> wvMixCurve_;
    uint32_t wvMixCurveStride_ = 0u;
    uint32_t wvMixCurveFrom_[kChannelCount + 1] = {};
    uint32_t wvMixCurveLast_[kChannelCount + 1] = {};
    uint32_t wvMixOpStart_[kChannelCount + 2] = {};
    std::vector<uint32_t> wvMixOpFrames_;
    std::vector<uint32_t> wvNextNonMix_;
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
    // The per-handle plan state is one 16-byte record, so the plan hooks (and a
    // steal victim's ghost snapshot) touch one cache line per handle instead of
    // one per array.  The column views keep the old array spelling.
    struct WvPlanRec {
        uint32_t start;     // UINT32_MAX = from 0
        uint32_t release;   // UINT32_MAX = none
        uint32_t ingress;   // see wvStartIngress_ below
        uint32_t pending;   // see wvGhostPendingOf_ below
    };
    template <uint32_t WvPlanRec::*Member>
    struct WvPlanColumn {
        WvPlanRec* base = nullptr;
        uint32_t& operator[](size_t i) const { return base[i].*Member; }
    };
    WvPlanRec* wvPlan_ = nullptr;
    WvPlanColumn<&WvPlanRec::start> wvStartFrame_;
    WvPlanColumn<&WvPlanRec::release> wvReleaseFrame_;
    // Dispatch ordinal of the launch that created each handle's current
    // generation (UINT32_MAX = carried over from a previous block, so every
    // op in the block applies).  Qualifies same-frame ops — see
    // WholeVoiceChannelOp::ingress.
    WvPlanColumn<&WvPlanRec::ingress> wvStartIngress_;
    uint32_t* wvGhostStartIngress_ = nullptr;
    // Handle a ghost was captured from: its re-excite hits live on that
    // handle's list, filtered by the ghost's playIndex.
    uint32_t* wvGhostHandle_ = nullptr;
    // Deferred ghost copy (launch staging).  A steal victim's ghost row is
    // filled in two steps: the plan snapshots what the successor's launch
    // overwrites at once (wvGhostImm_, the VoiceRow line, rot), and the rest of
    // the dense row is copied later by whoever touches the victim row first
    // (the ghost's render item, or the successor's MaterializeLaunch) under the
    // per-ghost claim word: 0 = owed, 1 = being copied, 2 = complete.  Ghosts
    // copied eagerly start at 2.  wvGhostPendingOf_[handle] = ghost + 1 of the
    // newest deferred ghost sourced from that handle (0 = none).
    struct WholeVoiceGhostImmediate {
        uint8_t state, envelopeStage, renderClass, loopEnabled;
        float currentGain, targetGain;
        uint32_t stealFadeInFramesRemaining, stealFadeInFramesTotal;
    };
    std::vector<WholeVoiceGhostImmediate> wvGhostImm_;
    std::unique_ptr<std::atomic<uint32_t>[]> wvGhostCopyState_;
    WvPlanColumn<&WvPlanRec::pending> wvGhostPendingOf_;
    VoiceManager* wvDeferredPool_ = nullptr;   // source pool of deferred ghosts
    void EnsureGhostCopied(uint32_t ghost);
    static void WholeVoiceDeferredGhostFlush(VoiceHandle handle,
                                             void* userData);
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
    // The rows the CPU job stage renders: wvSliceHandles_ (every active row)
    // unless the GPU stage took some, then its list of the remaining ones.
    const uint32_t* wvItemHandles_ = nullptr;
    SpanRetirement* wvJobRetirements_ = nullptr;
    // Per-job retirement counts, one cache line apart: kernels bump them on
    // every retirement from all workers at once.
    static constexpr uint32_t kWvRetireCountStride = 16u;
    uint32_t* wvJobRetireCounts_ = nullptr;
    static constexpr uint32_t kWvJobsPerThread = 4u;
    static constexpr uint32_t kWvMinItemsPerJob = 64u;
    // Most pitch bends one row fuses into a single span before the next span
    // starts at the bend that did not fit.
    static constexpr uint32_t kWvMaxFusedBreaks = 256u;
    uint32_t wvJobCapacity_ = 0u;
    uint32_t wvJobScratchCapacity_ = 0u;     // scratch stride wvJobRetirements_ was sized for
    uint64_t wvGhostOverflowCount_ = 0u;
    uint64_t wvStagedLeaks_ = 0u;   // test builds: blocks that left a staged row
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
    // Bulk block admission (voices.bulk_admission_percent, opt-in, 0 = off):
    // a block carrying more note-on units than percent% of the voice limit
    // keeps only the highest-ranked ones; the rest are removed from the event
    // list before the plan sees them, exactly as if they had been launched and
    // stolen before they rendered (their note-offs then pair with the oldest
    // older generation on the key, as for any stolen note).  Ranking is by
    // velocity; the notes at the cut velocity are thinned evenly along the
    // block.  Event frames of the notes that stay are untouched.
    // Pitch bends inside a voice's span ride the AVX2 kernels as step breaks
    // (the cursor changes step at the bend's exact frame inside a chunk) and
    // volume/pan ops as a per-channel gain curve, instead of splitting the span
    // at every op.  On by default; off restores the split, which the tests use
    // as the reference.
    void SetBendFusion(bool on) { bendFusion_ = on; }
    // Spans that ran with fused bends since the renderer started.
    uint64_t GetFusedBendSpans() const {
        return bendFusedSpans_.load(std::memory_order_relaxed);
    }
    void SetBulkAdmissionPercent(uint32_t percent) {
        bulkAdmissionPercent_.store(percent, std::memory_order_relaxed);
    }
    uint64_t GetBulkAdmissionDropped() const { return bulkAdmissionDropped_; }
    // Pure filter behind it (public for the tests).  Returns the note-on units
    // dropped; `out` is filled only when that is non-zero.
    static uint32_t BulkAdmissionFilter(const RenderEvent* events,
                                        uint32_t count, uint32_t budgetUnits,
                                        std::vector<RenderEvent>& out);
#if defined(SVMS_GPU_STAGE)
    // Attach the realtime GPU stage (null = CPU only, today's code exactly).
    // The stage is owned by the caller and must outlive the renderer's use.
    void SetGpuStage(GpuStage* stage) { gpuStage_ = stage; }
    GpuStage* GetGpuStage() const { return gpuStage_; }
#endif
    void SetEventDispatcher(EventDispatcher dispatcher, void* userData);
    void SetEventBatchDispatcher(EventBatchDispatcher dispatcher, void* userData);
    uint32_t GetWholeVoiceBlocksForTest() const { return wholeVoiceBlocks_; }
    uint64_t GetGhostOverflowCount() const { return wvGhostOverflowCount_; }
    uint64_t GetStagedLeakCount() const { return wvStagedLeaks_; }
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

#include "render/renderscalar/Core.inl"
#include "render/renderscalar/Spans.inl"
#include "render/renderscalar/Stretch.inl"
#include "render/renderscalar/WholeVoicePlan.inl"
#include "render/renderscalar/WholeVoiceRender.inl"
#include "render/renderscalar/WholeVoiceStorage.inl"
#include "render/renderscalar/GpuStageRender.inl"

} // namespace svms

#endif
