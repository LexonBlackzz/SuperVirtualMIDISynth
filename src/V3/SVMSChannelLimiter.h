#pragma once

#include "SVMSConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
//   gain = knee(threshold / envelope), applied to the same sample
//   bus  *= gain, and the gained bus accumulates into the master mix
//
// The envelope recurrence mirrors ClassicLimiterState's detector exactly
// (instant attack, release-coefficient recovery toward the running peak).
// Because the gain is computed from the envelope of the very sample it
// scales and the 4 dB knee only softens how reduction engages (it never
// lifts gain above the exact ratio), a channel's post-limited peak never
// exceeds the threshold beyond float rounding. Transients bite instantly —
// the classic tradeoff — while the master limiter stays the final ceiling.

struct ChannelLimiterState {
    // Knee width over which reduction eases in, as a linear overshoot
    // ratio (4 dB). Below knee: gain 1 with zero slope; above: the exact
    // threshold/envelope ratio.
    static constexpr float kKneeRatio = 1.5848931924611136f;

    float threshold = 0.5011872336272722f;        // -6 dBFS default
    float thresholdTarget = 0.5011872336272722f;
    float envelopeReleaseCoeff = 0.0003f;         // from Release (~150 ms)
    bool enabled = false;

    struct ChannelState {
        float envelope = 0.0f;
        // Per-block meters (ResetMeters at block start).
        float inputPeak = 0.0f;
        float gainReductionDb = 0.0f;
        bool limiting = false;
    };
    ChannelState channel[kChannelCount]{};

    void Reset() noexcept {
        for (uint32_t i = 0u; i < kChannelCount; ++i) {
            channel[i].envelope = 0.0f;
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
        for (uint32_t c = 0u; c < kChannelCount; ++c) {
            ChannelState& state = channel[c];
            const float* busL = busLeft[c];
            const float* busR = busRight[c];
            float env = state.envelope;
            float blockPeak = 0.0f;
            float blockReduction = 0.0f;
            for (uint32_t f = 0u; f < numFrames; ++f) {
                if (threshold != thresholdTarget) {
                    threshold = GlideF32(threshold, thresholdTarget, 0.0005f);
                    threshold = ClampThreshold(threshold);
                }
                const float inL = busL[f];
                const float inR = busR[f];
                const float absL = std::fabs(inL);
                const float absR = std::fabs(inR);
                const float peak = (std::max)(absL, absR);
                blockPeak = (std::max)(blockPeak, peak);

                if (peak > env) {
                    env = peak;                      // instant attack
                } else {
                    env += envelopeReleaseCoeff * (peak - env);
                    if (env < 1.0e-6f) env = 0.0f;   // denormal guard
                }

                float gain = 1.0f;
                if (env > threshold) {
                    const float rho = env / threshold;
                    if (rho >= kKneeRatio) {
                        gain = 1.0f / rho;
                    } else {
                        const float t = (rho - 1.0f) / (kKneeRatio - 1.0f);
                        const float eased = t * t;
                        gain = 1.0f - eased * (1.0f - 1.0f / rho);
                    }
                    blockReduction = (std::max)(blockReduction,
                        -20.0f * std::log10((std::max)(gain, 1.0e-12f)));
                }

                outputLeft[f] += inL * gain;
                outputRight[f] += inR * gain;
            }
            state.envelope = env;
            state.inputPeak = blockPeak;
            state.gainReductionDb = blockReduction;
            state.limiting = blockReduction > 0.01f;
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
