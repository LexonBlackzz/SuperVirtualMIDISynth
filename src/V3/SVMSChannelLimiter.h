#pragma once

#include "SVMSConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace svms {

// ── Per-MIDI-channel limiter (optional, default OFF) ────────────────────
//
// Sixteen stereo buses — one per MIDI channel — are limited independently
// and then summed into the master planar mix. The stage is purely POST: it
// runs after RenderBlock has filled the buses and before reverb/limiter see
// the interleaved output, so event timing, dispatch order, and every
// exact-frame render semantic are untouched. When the feature is disabled
// the render path writes the master mix directly and this state is never
// invoked — bit-identical output.
//
// Topology: classic zero-latency feed-forward limiting (deliberately NOT a
// lookahead/predictive design — sixteen lookahead delay lines would add
// output latency and memory to guard against summed transients that the
// master limiter downstream already handles; the per-channel stage exists
// to keep one runaway channel from eating everyone else's headroom).
//
// Per channel and frame:
//   peak = max(|L|, |R|)
//   envelope: instant attack, one-pole release toward the current peak
//   target gain = knee(threshold / envelope)
//   applied gain ramps down over 0.5 ms and follows release recovery
//   bus  *= gain, and the gained bus accumulates into the master mix
//
// The envelope recurrence mirrors ClassicLimiterState's detector exactly
// (instant attack, release-coefficient recovery toward the running peak).
// The detector still catches peaks immediately, but the applied gain ramps
// down over 0.5 ms instead of jumping in one sample. That removes the click
// caused by a discontinuous channel gain change. The brief attack overshoot
// is intentional and is caught by the downstream master limiter, which
// remains the final ceiling; no per-channel lookahead latency is added.

struct ChannelLimiterState {
    // Knee width over which reduction eases in, as a linear overshoot
    // ratio (4 dB). Below knee: gain 1 with zero slope; above: the exact
    // threshold/envelope ratio.
    static constexpr float kKneeRatio = 1.5848931924611136f;
    static constexpr float kGainAttackMs = 0.5f;

    float threshold = 0.5011872336272722f;        // -6 dBFS default
    float thresholdTarget = 0.5011872336272722f;
    float envelopeReleaseCoeff = 0.0003f;         // from Release (~150 ms)
    float gainAttackCoeff = 0.04f;                // 0.5 ms at ~44.1 kHz
    bool enabled = false;

    struct ChannelState {
        float envelope = 0.0f;
        float appliedGain = 1.0f;
        // Per-block meters (ResetMeters at block start).
        float inputPeak = 0.0f;
        float gainReductionDb = 0.0f;
        bool limiting = false;
    };
    ChannelState channel[kChannelCount]{};

    void Reset() noexcept {
        for (uint32_t i = 0u; i < kChannelCount; ++i) {
            channel[i].envelope = 0.0f;
            channel[i].appliedGain = 1.0f;
        }
        threshold = ClampThreshold(thresholdTarget);
        ResetMeters();
    }

    void Configure(uint32_t sampleRate, const EngineConfig& cfg) noexcept {
        enabled = cfg.channelLimiterEnabled;
        thresholdTarget = ClampThreshold(cfg.channelLimiterThreshold);
        threshold = thresholdTarget;
        SetLiveTargets(enabled, thresholdTarget, cfg.channelLimiterReleaseMs,
                       sampleRate);
        Reset();
    }

    // Live targets adopted at the block boundary (mirror of the master
    // limiter's glide-target pattern). The release coefficient is derived
    // driver-side from milliseconds so the sample rate never leaves the DLL.
    void SetLiveTargets(bool enabledTarget, float thresholdLinear,
                        float releaseMs, uint32_t sampleRate) noexcept {
        enabled = enabledTarget;
        thresholdTarget = ClampThreshold(thresholdLinear);
        const float samples = (std::max)(1.0f,
            (std::max)(1.0f, releaseMs) *
            static_cast<float>((std::max)(1u, sampleRate)) * 0.001f);
        envelopeReleaseCoeff = 1.0f - std::exp(-1.0f / samples);
        const float attackSamples = (std::max)(1.0f,
            kGainAttackMs *
            static_cast<float>((std::max)(1u, sampleRate)) * 0.001f);
        gainAttackCoeff = 1.0f - std::exp(-1.0f / attackSamples);
    }

    // Bus planes are capacity-strided, not packed by the current callback's
    // frame count. Clear through the pointer tables so variable-size WASAPI
    // callbacks cannot replay samples left in another channel's plane.
    static void ClearBuses(float* const* busLeft, float* const* busRight,
                           uint32_t numFrames) noexcept {
        if (!busLeft || !busRight || numFrames == 0u) return;
        const size_t bytes = static_cast<size_t>(numFrames) * sizeof(float);
        for (uint32_t c = 0u; c < kChannelCount; ++c) {
            std::memset(busLeft[c], 0, bytes);
            std::memset(busRight[c], 0, bytes);
        }
    }

    // Applies per-channel limiting to the bus planes and sums them into the
    // master planar mix (outputLeft/Right must be zeroed by the caller).
    // Runs only when enabled; the driver gates the call on the same flag
    // that routes RenderBlock into bus mode, so a disabled stage never
    // touches audio.
    void ProcessAndSum(float* const* busLeft, float* const* busRight,
                       float* outputLeft, float* outputRight,
                       uint32_t numFrames) noexcept {
        ResetMeters();
        if (!busLeft || !busRight || !outputLeft || !outputRight ||
            numFrames == 0u) {
            return;
        }
        for (uint32_t f = 0u; f < numFrames; ++f) {
            // The threshold is one shared control, so advance its live glide
            // once per audio frame. Advancing it inside the channel loop made
            // recovery 16x too fast and gave every channel a different
            // threshold during the same callback.
            if (threshold != thresholdTarget) {
                threshold = GlideF32(threshold, thresholdTarget, 0.0005f);
                threshold = ClampThreshold(threshold);
            }
            for (uint32_t c = 0u; c < kChannelCount; ++c) {
                ChannelState& state = channel[c];
                const float* busL = busLeft[c];
                const float* busR = busRight[c];
                const float inL = busL[f];
                const float inR = busR[f];
                const float absL = std::fabs(inL);
                const float absR = std::fabs(inR);
                const float peak = (std::max)(absL, absR);
                state.inputPeak = (std::max)(state.inputPeak, peak);

                if (peak > state.envelope) {
                    state.envelope = peak;           // instant attack
                } else {
                    state.envelope += envelopeReleaseCoeff *
                        (peak - state.envelope);
                    if (state.envelope < 1.0e-6f)
                        state.envelope = 0.0f;        // denormal guard
                }

                float targetGain = 1.0f;
                if (state.envelope > threshold) {
                    const float rho = state.envelope / threshold;
                    if (rho >= kKneeRatio) {
                        targetGain = 1.0f / rho;
                    } else {
                        const float t = (rho - 1.0f) / (kKneeRatio - 1.0f);
                        const float eased = t * t;
                        targetGain =
                            1.0f - eased * (1.0f - 1.0f / rho);
                    }
                }
                if (targetGain < state.appliedGain) {
                    state.appliedGain += gainAttackCoeff *
                        (targetGain - state.appliedGain);
                } else {
                    // Envelope release already makes upward gain movement
                    // gradual; follow it directly instead of filtering it a
                    // second time.
                    state.appliedGain = targetGain;
                }
                const float gain = state.appliedGain;
                if (gain < 1.0f) {
                    state.gainReductionDb = (std::max)(
                        state.gainReductionDb,
                        -20.0f * std::log10((std::max)(gain, 1.0e-12f)));
                }

                outputLeft[f] += inL * gain;
                outputRight[f] += inR * gain;
            }
        }
        for (uint32_t c = 0u; c < kChannelCount; ++c) {
            channel[c].limiting = channel[c].gainReductionDb > 0.01f;
        }
    }

    void ResetMeters() noexcept {
        for (uint32_t i = 0u; i < kChannelCount; ++i) {
            channel[i].inputPeak = 0.0f;
            channel[i].gainReductionDb = 0.0f;
            channel[i].limiting = false;
        }
    }

private:
    static float ClampThreshold(float value) noexcept {
        if (!std::isfinite(value) || value <= 0.0f)
            return 0.5011872336272722f;
        return (std::max)(0.0316227766f, (std::min)(1.0f, value));
    }

    static float GlideF32(float current, float target, float step) noexcept {
        if (current < target) return (std::min)(target, current + step);
        if (current > target) return (std::max)(target, current - step);
        return current;
    }
};

} // namespace svms
