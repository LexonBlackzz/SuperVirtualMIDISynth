#pragma once

#include "SVMSConfig.h"
#include "SVMSPostFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace svms {

// Low-latency lookahead limiter used by both the live driver and the
// standalone renderer.
//
// The old limiter delayed the audio, but its detector was a conventional
// attack/release envelope. A one-sample transient could therefore disappear
// before the detector ever reached its true peak, leaving the final tanh
// stage to do most of the real peak control.
//
// This limiter treats lookahead as an actual prediction window:
//   1. Detect the undelayed stereo sample peak immediately.
//   2. Convert that peak to the gain required to meet the threshold.
//   3. Hold the strongest requirement in a monotonic sliding window for the
//      configured lookahead duration.
//   4. Ramp the gain toward that requirement while the matching audio is
//      still travelling through the delay line.
//   5. Recover with a light program-dependent release: isolated transients
//      recover faster, sustained dense material recovers more slowly.
//
// The user-facing controls remain the existing Enabled / Threshold /
// Lookahead / Attack / Release controls. No extra latency beyond Lookahead is
// introduced. A final linked safety gain catches the tiny numerical or live-
// parameter overshoot that can remain after the predictive stage; unlike the
// old tanh stage it is not intended to be part of the normal sound.
struct AdaptiveLimiterState {
    static constexpr uint32_t kMaxDelayFrames = 8192u;
    static constexpr uint32_t kDelayMask = kMaxDelayFrames - 1u;
    static_assert((kMaxDelayFrames & kDelayMask) == 0u,
                  "limiter delay capacity must be a power of two");

    // The monotonic detector deque can contain delayFrames + 1 entries. Use a
    // larger power-of-two ring so even the maximum 8192-frame lookahead has a
    // spare slot and all queue wrapping stays branch-light.
    static constexpr uint32_t kDetectorCapacity = 16384u;
    static constexpr uint32_t kDetectorMask = kDetectorCapacity - 1u;

    alignas(64) float delayBuffer[kMaxDelayFrames * 2u]{};
    alignas(64) float detectorGain[kDetectorCapacity]{};
    alignas(64) uint64_t detectorIndex[kDetectorCapacity]{};

    uint32_t delayWritePos = 0u;
    uint32_t delayFrames = 128u;
    uint32_t delayFramesTarget = 128u;

    float threshold = 0.95f;
    float thresholdTarget = 0.95f;

    // Kept public because RuntimeLink already publishes these coefficients
    // directly. They represent the user's requested attack/release time.
    float attackCoeff = 0.25f;
    float releaseCoeff = 0.001f;
    bool enabled = true;

    // Legacy/public state name retained for diagnostic/source compatibility.
    // It now mirrors the instantaneous undelayed peak detector rather than a
    // smoothed peak envelope.
    float envelope = 0.0f;

    // Per-block meters. gainReductionDb is a positive reduction magnitude.
    float inputPeakL = 0.0f;
    float inputPeakR = 0.0f;
    float outputPeakL = 0.0f;
    float outputPeakR = 0.0f;
    float gainReductionDb = 0.0f;

    void Reset() noexcept {
        std::memset(delayBuffer, 0, sizeof(delayBuffer));
        delayWritePos = 0u;
        detectorHead_ = detectorTail_ = 0u;
        sampleCounter_ = 0u;
        currentGain_ = 1.0f;
        envelope = 0.0f;
        averagePeak_ = 0.0f;
        peakMemory_ = 0.0f;
        limitingDensity_ = 0.0f;
        delayFrames = ClampDelay(delayFramesTarget);
        threshold = ClampThreshold(thresholdTarget);
        cachedDeadlineFrames_ = 0u;
        deadlineAttackCoeff_ = 1.0f;
        inputPeakL = inputPeakR = outputPeakL = outputPeakR = 0.0f;
        gainReductionDb = 0.0f;
    }

    void Configure(uint32_t sampleRate, const EngineConfig& cfg) noexcept {
        configuredSampleRate_ = (std::max)(1u, sampleRate);
        enabled = cfg.limiterEnabled;
        thresholdTarget = ClampThreshold(cfg.limiterThreshold);
        threshold = thresholdTarget;
        delayFramesTarget = ClampDelay(static_cast<uint32_t>(
            (std::max)(0.0f, cfg.limiterLookaheadMs) *
            static_cast<float>(configuredSampleRate_) * 0.001f + 0.5f));
        delayFrames = delayFramesTarget;

        attackCoeff = TimeToCoeff(cfg.limiterAttackMs, configuredSampleRate_,
                                  0.01f);
        releaseCoeff = TimeToCoeff(cfg.limiterReleaseMs, configuredSampleRate_,
                                   1.0f);

        // Program analysis is intentionally much shorter than the user
        // release. It classifies "single transient" versus "continuous wall"
        // without becoming another audible compressor envelope.
        averagePeakCoeff_ = TimeToCoeff(20.0f, configuredSampleRate_, 1.0f);
        densityCoeff_ = TimeToCoeff(25.0f, configuredSampleRate_, 1.0f);
        peakMemoryDecay_ = std::exp(-1.0f /
            ((std::max)(1.0f,
                15.0f * static_cast<float>(configuredSampleRate_) * 0.001f)));

        Reset();
    }

    void Process(float* interleaved, uint32_t numFrames, uint32_t channels,
                 PostHighPass3Hz& highPass) noexcept {
        inputPeakL = inputPeakR = outputPeakL = outputPeakR = 0.0f;
        gainReductionDb = 0.0f;
        if (!interleaved || numFrames == 0u || channels == 0u) return;

        for (uint32_t f = 0u; f < numFrames; ++f) {
            const uint32_t offset = f * channels;
            float inL = interleaved[offset];
            float inR = channels > 1u ? interleaved[offset + 1u] : inL;

            const float absL = std::fabs(inL);
            const float absR = std::fabs(inR);
            const float peak = (std::max)(absL, absR);
            envelope = peak;
            inputPeakL = (std::max)(inputPeakL, absL);
            inputPeakR = (std::max)(inputPeakR, absR);

            // Keep delay storage warm even in bypass mode so enabling the
            // limiter never reads uninitialized/stale memory.
            const uint32_t writeOffset = delayWritePos * 2u;
            delayBuffer[writeOffset] = inL;
            delayBuffer[writeOffset + 1u] = inR;

            if (delayFrames != delayFramesTarget) {
                if (delayFrames < delayFramesTarget) ++delayFrames;
                else --delayFrames;
                delayFrames = ClampDelay(delayFrames);
            }
            if (threshold != thresholdTarget) {
                constexpr float kThresholdGlide = 0.0005f;
                if (threshold < thresholdTarget)
                    threshold = (std::min)(thresholdTarget,
                        threshold + kThresholdGlide);
                else
                    threshold = (std::max)(thresholdTarget,
                        threshold - kThresholdGlide);
                threshold = ClampThreshold(threshold);
            }

            const float rawRequiredGain = peak > threshold && peak > 0.0f
                ? threshold / peak
                : 1.0f;
            const float predictiveTarget = PushDetectorTarget(rawRequiredGain);

            UpdateProgramAnalysis(peak, rawRequiredGain);

            if (!enabled) {
                // True bypass: no lookahead latency and no gain processing.
                // Reset the audible gain state while still feeding the delay
                // and detector histories for a less stale re-enable.
                currentGain_ = 1.0f;
                highPass.ProcessStereoSample(inL, inR);
                interleaved[offset] = inL;
                if (channels > 1u) interleaved[offset + 1u] = inR;
                outputPeakL = (std::max)(outputPeakL, std::fabs(inL));
                outputPeakR = (std::max)(outputPeakR, std::fabs(inR));
                AdvanceDelay();
                ++sampleCounter_;
                continue;
            }

            RecomputeDeadlineAttackIfNeeded();
            const float effectiveAttack =
                (std::max)(ClampCoeff(attackCoeff), deadlineAttackCoeff_);

            if (predictiveTarget < currentGain_) {
                // The peak is still in the future relative to the delayed
                // audio. The deadline coefficient guarantees that even a
                // user attack setting slower than the lookahead cannot simply
                // fail to notice a one-sample transient.
                currentGain_ += effectiveAttack *
                    (predictiveTarget - currentGain_);
            } else {
                // Program-dependent recovery. A sparse/high-crest transient
                // gets out of the way quickly; sustained limiting lengthens
                // the release so dense Black MIDI does not chatter/pump.
                const float adaptiveRelease = ComputeAdaptiveReleaseCoeff();
                currentGain_ += adaptiveRelease *
                    (predictiveTarget - currentGain_);
            }
            currentGain_ = (std::max)(0.0f, (std::min)(1.0f, currentGain_));

            const uint32_t readPos =
                (delayWritePos - delayFrames) & kDelayMask;
            const uint32_t readOffset = readPos * 2u;
            float outL = delayBuffer[readOffset] * currentGain_;
            float outR = delayBuffer[readOffset + 1u] * currentGain_;

            // DC blocking remains after gain detection, exactly as in the
            // previous signal path.
            highPass.ProcessStereoSample(outL, outR);

            // Linked last-resort sample-peak guard. The predictive stage
            // should do virtually all normal work; this catches parameter
            // glides, startup/history holes and floating-point residue without
            // making tanh saturation part of the limiter's character.
            float safetyGain = 1.0f;
            const float postPeak = (std::max)(std::fabs(outL), std::fabs(outR));
            if (postPeak > threshold && postPeak > 0.0f) {
                safetyGain = threshold / postPeak;
                outL *= safetyGain;
                outR *= safetyGain;
            }

            interleaved[offset] = outL;
            if (channels > 1u) interleaved[offset + 1u] = outR;

            outputPeakL = (std::max)(outputPeakL, std::fabs(outL));
            outputPeakR = (std::max)(outputPeakR, std::fabs(outR));

            const float appliedGain = currentGain_ * safetyGain;
            if (appliedGain < 0.999999f) {
                const float reduction = -20.0f *
                    std::log10((std::max)(appliedGain, 1.0e-12f));
                gainReductionDb = (std::max)(gainReductionDb, reduction);
            }

            AdvanceDelay();
            ++sampleCounter_;
        }

        highPass.FinishBlock();
    }

private:
    static float ClampThreshold(float value) noexcept {
        return (std::max)(0.1f, (std::min)(1.0f, value));
    }

    static float ClampCoeff(float value) noexcept {
        if (!std::isfinite(value)) return 0.0f;
        return (std::max)(0.0f, (std::min)(1.0f, value));
    }

    static uint32_t ClampDelay(uint32_t frames) noexcept {
        return (std::max)(1u, (std::min)(kMaxDelayFrames, frames));
    }

    static float TimeToCoeff(float milliseconds, uint32_t sampleRate,
                             float minimumMs) noexcept {
        milliseconds = (std::max)(minimumMs, milliseconds);
        const float samples = (std::max)(1.0f,
            milliseconds * static_cast<float>(sampleRate) * 0.001f);
        return 1.0f - std::exp(-1.0f / samples);
    }

    void AdvanceDelay() noexcept {
        delayWritePos = (delayWritePos + 1u) & kDelayMask;
    }

    float PushDetectorTarget(float target) noexcept {
        target = (std::max)(0.0f, (std::min)(1.0f, target));

        // Maintain monotonically increasing gain requirements. The front is
        // always the strongest (smallest) gain target in the live window.
        while (detectorHead_ != detectorTail_) {
            const uint32_t back = (detectorTail_ - 1u) & kDetectorMask;
            if (detectorGain[back] < target) break;
            detectorTail_ = back;
        }
        detectorGain[detectorTail_] = target;
        detectorIndex[detectorTail_] = sampleCounter_;
        detectorTail_ = (detectorTail_ + 1u) & kDetectorMask;

        // An input sample detected at n is heard at n + delayFrames. Keep its
        // constraint alive through that exact output sample (age <= delay).
        while (detectorHead_ != detectorTail_) {
            const uint64_t index = detectorIndex[detectorHead_];
            if (sampleCounter_ - index <= delayFrames) break;
            detectorHead_ = (detectorHead_ + 1u) & kDetectorMask;
        }

        return detectorHead_ != detectorTail_
            ? detectorGain[detectorHead_]
            : 1.0f;
    }

    void RecomputeDeadlineAttackIfNeeded() noexcept {
        if (cachedDeadlineFrames_ == delayFrames) return;
        cachedDeadlineFrames_ = delayFrames;

        // Reach 99.9% of a newly required reduction by the time its sample
        // exits the lookahead delay. This is an attack *floor*; a faster user
        // Attack setting still wins.
        constexpr float kResidual = 0.001f;
        deadlineAttackCoeff_ = 1.0f - std::exp(
            std::log(kResidual) /
            static_cast<float>((std::max)(1u, delayFrames)));
        deadlineAttackCoeff_ = ClampCoeff(deadlineAttackCoeff_);
    }

    void UpdateProgramAnalysis(float peak, float rawRequiredGain) noexcept {
        averagePeak_ += averagePeakCoeff_ * (peak - averagePeak_);
        peakMemory_ = (std::max)(peak, peakMemory_ * peakMemoryDecay_);

        // Reduction demand rather than a boolean gives a dense +12 dB wall
        // more weight than a barely-over-threshold waveform.
        const float demand = 1.0f - rawRequiredGain;
        limitingDensity_ += densityCoeff_ * (demand - limitingDensity_);
    }

    float ComputeAdaptiveReleaseCoeff() const noexcept {
        const float base = ClampCoeff(releaseCoeff);
        if (base <= 0.0f) return 0.0f;

        const float density = (std::max)(0.0f,
            (std::min)(1.0f, limitingDensity_ * 3.0f));
        const float crest = peakMemory_ /
            (std::max)(averagePeak_, 1.0e-5f);
        const float transient = (std::max)(0.0f,
            (std::min)(1.0f, (crest - 1.5f) / 4.5f));

        // ~0.42x user release at the sparse/high-crest extreme;
        // ~2.5x for continuously limited dense material.
        float timeScale = 0.65f + 1.85f * density;
        timeScale *= 1.0f - 0.35f * transient;
        timeScale = (std::max)(0.35f, (std::min)(3.0f, timeScale));

        // For limiter release coefficients (normally << 1), scaling the
        // one-pole coefficient inversely is a close approximation to scaling
        // its time constant and avoids exp()/pow() in the per-sample path.
        return (std::min)(1.0f, base / timeScale);
    }

    uint32_t detectorHead_ = 0u;
    uint32_t detectorTail_ = 0u;
    uint64_t sampleCounter_ = 0u;

    float currentGain_ = 1.0f;
    float averagePeak_ = 0.0f;
    float peakMemory_ = 0.0f;
    float limitingDensity_ = 0.0f;

    uint32_t configuredSampleRate_ = 44100u;
    float averagePeakCoeff_ = 0.001f;
    float densityCoeff_ = 0.001f;
    float peakMemoryDecay_ = 0.999f;

    uint32_t cachedDeadlineFrames_ = 0u;
    float deadlineAttackCoeff_ = 1.0f;
};

} // namespace svms
