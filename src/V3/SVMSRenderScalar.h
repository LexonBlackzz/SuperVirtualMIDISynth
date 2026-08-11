#ifndef SVMS_RENDER_SCALAR_H
#define SVMS_RENDER_SCALAR_H

#include "SVMSTypes.h"
#include "SVMSVoiceManager.h"
#include "SVMSChannelCache.h"
#include "SVMSEnvelope.h"
#include "SVMSPageAllocator.h"
#include "SVMSRenderKernels.h"
#include <algorithm>
#include "SVMSSoundFont.h"

namespace svms {

// ── Linear interpolation between two sample frames ──────────────────────
inline float InterpolateSample(const float* data, uint32_t baseIndex,
                                uint32_t nextIndex, float frac) {
    const float s0 = data[baseIndex];
    const float s1 = data[nextIndex];
    return s0 + (s1 - s0) * frac;
}

// Render the compact continuation captured when this primary voice slot was
// stolen.  The tail follows the old sample cursor and loop for a fixed,
// short 64-frame
// linear ramp.  It has no MIDI identity and cannot consume another voice
// slot, so note-off/controller handling remains attached only to the new
// occupant of the slot.
inline void RenderStealTailSample(VoiceSoA& v, uint32_t idx,
                                  const float* sampleData,
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
    const float sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
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

struct SpanRetirement {
    uint32_t handle;
    uint32_t frameOffset;
    uint32_t capturePosition;
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

    void RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                     const float* sampleData, uint32_t sampleDataFrames,
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
                     const float* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events = nullptr,
                     uint32_t eventCount = 0,
                     bool correctnessMode = false,
                     uint64_t blockStartFrame = 0);
#endif

    void SetEventDispatcher(EventDispatcher dispatcher, void* userData);
    bool SetRenderBackend(RenderBackend backend);
    RenderBackend GetRenderBackend() const { return kernelSet_->backend; }
    const char* GetRenderBackendName() const { return kernelSet_->name; }

private:
    void RenderBlockFrameMajor(VoiceManager& voices, const ChannelCache& channels,
                     const float* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events, uint32_t eventCount,
                     bool correctnessMode, uint64_t blockStartFrame);
    EventDispatcher dispatcher_;
    void* dispatcherUserData_;
    const RenderKernelSet* kernelSet_;
    alignas(64) uint32_t classChanges_[kMaxPolyphony];
    alignas(64) uint32_t tailFrameCounts_[kMaxPolyphony];
    alignas(64) SpanRetirement retirements_[kMaxPolyphony];
};

inline RenderScalar::RenderScalar()
    : dispatcher_(nullptr), dispatcherUserData_(nullptr),
      kernelSet_(&SelectBestRenderKernelSet()) {}

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
                                        const float* sampleData, uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                        const RenderEvent* events, uint32_t eventCount,
                                        bool correctnessMode,
                                        uint64_t blockStartFrame) {
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
        while (eventIdx < eventCount) {
            if (events[eventIdx].frameOffset > frameCursor) break;
            if (dispatcher_)
                dispatcher_(events[eventIdx], f, dispatcherUserData_);
            ++eventIdx;
        }

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

        for (uint32_t i = 0; i < voices.activeCount_; ) {
            uint32_t idx = voices.activeList_[i];

            // Steal tails are always mixed.  Decimating the short ramp could
            // itself reintroduce the discontinuity this path removes.
            RenderStealTailSample(v, idx, sampleData, sampleDataFrames, outL, outR);

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
                const float scaled = sample * gain * stealFadeIn;
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
                                        const float* sampleData,
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

// Render a stolen voice continuation across an event-free span.  All state is
// held in locals and committed once, avoiding the former SoA round-trip on
// every frame.  The caller limits frameCount when the replacement voice
// retires inside the span because the tail is owned by that primary slot.
inline void RenderStealTailSpan(VoiceSoA& v, uint32_t idx,
                                const float* sampleData,
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
        const float sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
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
                                       const float* sampleData,
                                       uint32_t sampleDataFrames,
                                       float* outputLeft, float* outputRight,
                                       uint32_t frameStart, uint32_t frameCount,
                                       uint32_t mixedFrameCount) {
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
            }
            v.phases[idx] = phase;
            return UINT32_MAX;
        }

        // Decimated steady voices still advance phase without fetching sample
        // memory or touching the output buffers.
        if (mixedFrameCount == 0u && fadeRemaining == 0u) {
            phase += phaseStep * static_cast<float>(frameCount);
            if (phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                const float loopLength = relLoopEF - relLoopSF;
                if (loopLength > 0.0f && overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
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
                const float sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
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

            const float scaled = sample * gain;
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
            const float scaled = sample * gain;
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
            const float scaled = sample * gain * fade;
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

inline void RenderScalar::RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                                      const float* sampleData, uint32_t sampleDataFrames,
                                      float* outputLeft, float* outputRight,
                                      uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                      const RenderEvent* events, uint32_t eventCount,
                                      bool correctnessMode,
                                      uint64_t blockStartFrame) {
    (void)cfg;
    (void)channels;
    VoiceSoA& v = voices.v;
    const RenderKernelSet& kernelSet = *kernelSet_;
    // Gain state is already current for this boundary; see the frame-major
    // oracle above and Driver::HandleControlChange.

    const uint32_t initialStep = correctnessMode
        ? 1u : ComputeDecimationStep(voices.activeCount_);
    if (initialStep > 1u && voices.activeCount_ > 1u) {
        std::sort(voices.activeList_, voices.activeList_ + voices.activeCount_,
            [&v](uint32_t a, uint32_t b) { return v.velocity[a] > v.velocity[b]; });
        voices.RebuildActivePositions();
    }

    uint32_t eventIndex = 0u;
    uint32_t cursor = 0u;
    while (cursor < numFrames) {
        voices.SetCurrentFrame(blockStartFrame + cursor);

        // State changes at this boundary are visible to the first rendered
        // frame of the span.  Equal-frame order is already ingress order.
        while (eventIndex < eventCount && events[eventIndex].frameOffset <= cursor) {
            if (dispatcher_) dispatcher_(events[eventIndex], cursor, dispatcherUserData_);
            ++eventIndex;
        }

        uint32_t spanEnd = numFrames;
        if (eventIndex < eventCount && events[eventIndex].frameOffset < spanEnd)
            spanEnd = events[eventIndex].frameOffset;
        if (spanEnd <= cursor) continue;

        const uint32_t spanFrames = spanEnd - cursor;
        uint32_t retireCount = 0u;
        uint32_t classChangeCount = 0u;
        const uint32_t tailCount = voices.GetStealTailCount();
        const uint32_t* tailHandles = voices.GetStealTailList();
        const uint32_t voiceCapacity = voices.GetMaxVoices();
        const bool denseTails = tailCount * 2u >= voiceCapacity;
        if (denseTails) {
            // Continuous full-pool stealing leaves nearly every slot with a
            // tail. Sequential slot traversal is cheaper and much friendlier
            // to the SoA caches than chasing the constantly shuffled sparse
            // list. Normal playback retains the sparse O(tailCount) path.
            for (uint32_t idx = 0; idx < voiceCapacity; ++idx) {
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
            const uint32_t classCount = voices.GetRenderClassCount(renderClass);
            const uint32_t* handles = voices.GetRenderClassList(renderClass);

            RenderClassKernel classKernel = kernelSet.kernels[classIndex];
            if (classKernel != nullptr && sampleData != nullptr) {
                const RenderSpanContext context{
                    &v, sampleData, sampleDataFrames, outputLeft, outputRight,
                    cursor, spanFrames, voices.GetMaxVoices()};
                classKernel(context, handles, classCount);
                continue;
            }

            for (uint32_t position = 0; position < classCount; ++position) {
                const uint32_t idx = handles[position];
                if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) continue;

                uint32_t retiredAt = UINT32_MAX;
                const bool cleanPrimary =
                    v.stealFadeInFramesRemaining[idx] == 0u;
                if (cleanPrimary && renderClass == VoiceRenderClass::SustainedLoop) {
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
                    if (v.stealTailFramesRemaining[idx] != 0u)
                        tailFrameCounts_[idx] = retiredAt + 1u;
                    retirements_[retireCount++] = {
                        idx, retiredAt, voices.activePosition_[idx]};
                } else if (renderClass == VoiceRenderClass::TransientLoop ||
                           renderClass == VoiceRenderClass::Generic) {
                    classChanges_[classChangeCount++] = idx;
                }
            }
        }

        // Tails have their own sparse lifecycle list and render independently
        // from primary class ordering.  Iterate backwards so O(1) swap-removal
        // of a completed tail cannot skip an unprocessed entry.
        if (denseTails) {
            for (uint32_t idx = 0; idx < voiceCapacity; ++idx) {
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

    voices.SetCurrentFrame(blockStartFrame + numFrames);
}

} // namespace svms

#endif
