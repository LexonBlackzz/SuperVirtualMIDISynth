#ifndef SVMS_RENDER_KERNELS_H
#define SVMS_RENDER_KERNELS_H

#include "SVMSTypes.h"
#include "SVMSPhaseRotation.h"

namespace svms {

struct SpanRetirement {
    uint32_t handle;
    uint32_t frameOffset;
    uint32_t capturePosition;
};

// Backend-neutral span contract.  Explicit SSE2 and AVX2 implementations can
// replace a class function without changing scheduling or voice ownership.
struct RenderSpanContext {
    VoiceSoA* voices;
    const int16_t* sampleData;
    // Optional analytic companion store (Hilbert pair, form-2 rotation).
    // Null when the bundle carries no pair or rotation is off; kernels may
    // forward it to the 6-arg RotateVoiceSample overload unconditionally —
    // non-pair rotation states ignore it.
    const int16_t* hilbertData;
    uint32_t sampleDataFrames;
    float* outputLeft;
    float* outputRight;
    uint32_t frameStart;
    uint32_t frameCount;
    uint32_t voiceCapacity;
    uint32_t* classChangeHandles;
    uint32_t* classChangeCount;
    const uint32_t* activePositions;
    SpanRetirement* retirements;
    uint32_t* retirementCount;
    // Non-zero: handles are the contiguous identity range starting at this
    // slot, letting kernels use aligned vector loads instead of gathers
    // (dense-tile mode).  Zero: handles are arbitrary.
    uint32_t handleBase;
    // Whole-voice vibrato (CC1 / channel pressure).  When lfoActive is
    // non-zero the per-row kernels rebuild the voice's 64-frame LFO control
    // window internally (scalar ratio refresh between vectorized chunks),
    // so whole-voice blocks no longer refuse to the legacy 64-frame-capped
    // sparse path.  lfoDepth/lfoBendRatio are the channel modulation depth
    // and bend ratio at segment start — segment-constant because channel
    // ops split timelines before segments render.  Sparse-path callers
    // leave lfoActive zero: AdvanceVibratoSpan owns vibrato there and
    // refreshes phaseIncs per span instead.
    float lfoDepth;
    float lfoBendRatio;
    uint32_t lfoActive;
    // Optional per-MIDI-channel bus planes (SVMSChannelLimiter.h).  Null in
    // legacy mode: kernels mix into outputLeft/outputRight as before.  When
    // set, each of the kChannelCount entries is a block-length (or job-length)
    // planar buffer and the voice's destination is channelBusLeft[voice
    // channel] — a voice's MIDI channel is fixed for its lifetime, so the
    // plane is selected once per voice/span, never per frame.  frameStart
    // stays relative to the plane exactly as it is relative to outputLeft.
    // Aggregate initializers that omit these fields value-init them to null
    // (legacy behavior), so existing construction sites stay untouched.
    float* const* channelBusLeft;
    float* const* channelBusRight;
};

// Per-voice destination resolution for class kernels: when the span carries
// channel buses, the voice's plane replaces the shared mix.  Returns the
// context unchanged (by value; cheap POD) in legacy mode.
inline RenderSpanContext SelectVoiceDestination(const RenderSpanContext& c,
                                                uint32_t channel) {
    if (c.channelBusLeft == nullptr) return c;
    RenderSpanContext vc = c;
    const uint32_t safeChannel =
        channel < kChannelCount ? channel : 0u;
    vc.outputLeft = c.channelBusLeft[safeChannel];
    vc.outputRight = c.channelBusRight[safeChannel];
    return vc;
}

// exp2(cents / 1200) for vibrato ratios.  Degree-5 Taylor in
// x = cents * ln2 / 1200: |x| <= 0.35 at +-600 cents keeps the truncation
// below ~3e-7 relative — two orders under the 2e-5 AVX2 oracle tolerance
// and far under the 64-frame control-rate drift this path already carries.
// Replaces a ~30-50-cycle powf per voice per window at high polyphony.
inline float Exp2CentsApprox(float cents) {
    const float x = cents * 5.776226504666211e-4f;
    return 1.0f + x * (0.6931471805599453f +
        x * (0.2402265069591007f +
        x * (0.05550410866482158f +
        x * (0.009618129107628477f + x * 0.0013333558146428443f))));
}

// One whole-voice LFO control window for a single row: advance the voice's
// free-running triangle LFO across `frames` (look-ahead — the same
// advance-then-use semantics as AdvanceVibratoSpan), rebuild the pitch
// ratio and return the segment phase increment.  Marks the row modulated
// and stores phaseIncs so a mid-segment fallback, the next segment or the
// next block observes a consistent row.
inline float AdvanceVibratoLfoWindow(VoiceSoA& v, uint32_t row,
                                     uint32_t frames, float depth,
                                     float bendRatio) {
    float lfoPhase = v.vibLfoPhases[row] +
        v.vibLfoSteps[row] * static_cast<float>(frames);
    lfoPhase -= std::floor(lfoPhase);
    v.vibLfoPhases[row] = lfoPhase;
    const float t = lfoPhase * 4.0f;
    const float tri = t < 1.0f ? t : (t < 3.0f ? 2.0f - t : t - 4.0f);
    const float cents = v.vibLfoToPitchCents[row] * depth * tri;
    const float step = v.basePhaseIncs[row] * bendRatio *
        Exp2CentsApprox(cents);
    v.vibLfoModulated[row] = 1u;
    v.phaseIncs[row] = step;
    return step;
}

// A backend returns false without mutating state when it cannot safely consume
// the complete class, allowing the established scalar voice path to take over.
using RenderClassKernel = bool(*)(const RenderSpanContext& context,
                                  const uint32_t* handles,
                                  uint32_t handleCount);
struct RenderKernelSet {
    RenderClassKernel kernels[kVoiceRenderClassCount];
    RenderBackend backend;
    const char* name;
};

const RenderKernelSet& GetScalarRenderKernelSet();
const RenderKernelSet& GetSSE2RenderKernelSet();
#if !defined(SVMS_XP_COMPAT)
const RenderKernelSet& GetAVX2RenderKernelSet();
#endif
const RenderKernelSet& SelectBestRenderKernelSet();
const RenderKernelSet* SelectRenderKernelSet(RenderBackend backend);
bool IsRenderBackendSupported(RenderBackend backend);

#if !defined(SVMS_XP_COMPAT)
// Builds the exact packed keys used by the volatile steal heap. Returns false
// when the absolute frame is outside the vector kernel's exact 32-bit age
// range, allowing the caller to retain the scalar path.
bool BuildVolatileStealKeysAVX2(
    const uint32_t* handles, uint32_t handleCount,
    const uint64_t* birthFrames, const float* currentGains,
    const float* outputGains, const uint32_t* activePositions,
    uint64_t currentFrame, float gainScale, uint64_t* outputKeys,
    uint32_t* outputHandles, uint32_t* inverseHeapPositions);
#endif

uint32_t ScalarRenderSustainedLoop(
    VoiceSoA& voices, uint32_t handle, const int16_t* sampleData,
    const int16_t* hilbertData, uint32_t sampleDataFrames,
    float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount);

uint32_t RenderSustainedOneShotSpan(
    VoiceSoA& v, uint32_t idx, const int16_t* sampleData,
    const int16_t* hilbertData, uint32_t sampleDataFrames,
    float* outputLeft, float* outputRight, uint32_t frameStart,
    uint32_t frameCount);
uint32_t ScalarRenderSustainedOneShot(
    VoiceSoA& voices, uint32_t handle, const int16_t* sampleData,
    const int16_t* hilbertData, uint32_t sampleDataFrames,
    float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount);

void ScalarRenderSustainedLoopShortBatch(
    VoiceSoA& voices, const uint32_t* handles, uint32_t handleCount,
    const int16_t* sampleData, const int16_t* hilbertData,
    uint32_t sampleDataFrames, float* outputLeft,
    float* outputRight, uint32_t frameStart, uint32_t frameCount,
    float* const* channelBusLeft = nullptr,
    float* const* channelBusRight = nullptr);

bool ScalarRenderTransientLoopClass(const RenderSpanContext& context,
                                    const uint32_t* handles,
                                    uint32_t handleCount);

} // namespace svms

#endif
