#pragma once

#include "SVMSConfig.h"
#include "SVMSPostFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace svms {

// The v0.6.5 limiter, kept intact as the A/B reference implementation.
// It intentionally preserves the original envelope + lookahead-delay + tanh
// behavior so "Classic" really means the limiter users were hearing before
// the predictive implementation was introduced.
struct ClassicLimiterState {
    static constexpr uint32_t kMaxDelayFrames = 8192u;

    alignas(64) float delayBuffer[kMaxDelayFrames * 2u]{};
    uint32_t delayWritePos = 0u;
    uint32_t delayFrames = 128u;
    uint32_t delayFramesTarget = 128u;
    float threshold = 0.95f;
    float thresholdTarget = 0.95f;
    float envelope = 0.0f;
    float attackCoeff = 0.25f;
    float releaseCoeff = 0.001f;
    bool enabled = true;

    float inputPeakL = 0.0f;
    float inputPeakR = 0.0f;
    float outputPeakL = 0.0f;
    float outputPeakR = 0.0f;
    float gainReductionDb = 0.0f;

    void Reset() noexcept {
        std::memset(delayBuffer, 0, sizeof(delayBuffer));
        delayWritePos = 0u;
        envelope = 0.0f;
        delayFrames = ClampDelay(delayFramesTarget);
        threshold = ClampThreshold(thresholdTarget);
        ResetMeters();
    }

    void Configure(uint32_t sampleRate, const EngineConfig& cfg) noexcept {
        enabled = cfg.limiterEnabled;
        thresholdTarget = ClampThreshold(cfg.limiterThreshold);
        threshold = thresholdTarget;
        delayFramesTarget = ClampDelay(static_cast<uint32_t>(
            (std::max)(0.0f, cfg.limiterLookaheadMs) *
            static_cast<float>((std::max)(1u, sampleRate)) * 0.001f + 0.5f));
        delayFrames = delayFramesTarget;
        attackCoeff = TimeToCoeff(cfg.limiterAttackMs, sampleRate, 0.01f);
        releaseCoeff = TimeToCoeff(cfg.limiterReleaseMs, sampleRate, 1.0f);
        Reset();
    }

    void Process(float* interleaved, uint32_t numFrames, uint32_t channels,
                 PostHighPass3Hz& highPass) noexcept {
        ResetMeters();
        if (!interleaved || numFrames == 0u || channels == 0u) return;

        for (uint32_t f = 0u; f < numFrames; ++f) {
            const uint32_t offset = f * channels;
            const float inL = interleaved[offset];
            const float inR = channels > 1u ? interleaved[offset + 1u] : inL;
            float outL = 0.0f;
            float outR = 0.0f;
            ProcessStereoFrame(inL, inR, outL, outR, highPass);
            interleaved[offset] = outL;
            if (channels > 1u) interleaved[offset + 1u] = outR;
        }
        highPass.FinishBlock();
    }

    void ProcessPlanar(float* left, float* right, uint32_t numFrames,
                       PostHighPass3Hz& highPass) noexcept {
        ResetMeters();
        if (!left || !right || numFrames == 0u) return;
        for (uint32_t f = 0u; f < numFrames; ++f) {
            const float inL = left[f];
            const float inR = right[f];
            float outL = 0.0f;
            float outR = 0.0f;
            ProcessStereoFrame(inL, inR, outL, outR, highPass);
            left[f] = outL;
            right[f] = outR;
        }
        highPass.FinishBlock();
    }

private:
    static float ClampThreshold(float value) noexcept {
        return (std::max)(0.1f, (std::min)(1.0f, value));
    }

    static uint32_t ClampDelay(uint32_t frames) noexcept {
        // Classic historically always had at least one frame of delay even
        // when the UI requested 0 ms.
        return (std::max)(1u, (std::min)(kMaxDelayFrames, frames));
    }

    static float TimeToCoeff(float milliseconds, uint32_t sampleRate,
                             float minimumMs) noexcept {
        milliseconds = (std::max)(minimumMs, milliseconds);
        const float samples = (std::max)(1.0f,
            milliseconds * static_cast<float>((std::max)(1u, sampleRate)) * 0.001f);
        return 1.0f - std::exp(-1.0f / samples);
    }

    static uint32_t GlideU32(uint32_t current, uint32_t target) noexcept {
        if (current < target) return current + 1u;
        if (current > target) return current - 1u;
        return current;
    }

    static float GlideF32(float current, float target, float step) noexcept {
        if (current < target) return (std::min)(target, current + step);
        if (current > target) return (std::max)(target, current - step);
        return current;
    }

    void ResetMeters() noexcept {
        inputPeakL = inputPeakR = outputPeakL = outputPeakR = 0.0f;
        gainReductionDb = 0.0f;
    }

    void ProcessStereoFrame(float inL, float inR, float& outL, float& outR,
                            PostHighPass3Hz& highPass) noexcept {
        if (delayFrames != delayFramesTarget)
            delayFrames = ClampDelay(GlideU32(delayFrames, delayFramesTarget));
        if (threshold != thresholdTarget)
            threshold = ClampThreshold(GlideF32(threshold, thresholdTarget, 0.0005f));

        const float absL = std::fabs(inL);
        const float absR = std::fabs(inR);
        const float peak = (std::max)(absL, absR);
        inputPeakL = (std::max)(inputPeakL, absL);
        inputPeakR = (std::max)(inputPeakR, absR);

        const uint32_t writeOffset = delayWritePos * 2u;

        if (!enabled) {
            // Original live path bypassed lookahead entirely while still
            // keeping the delay ring warm for a later re-enable.
            delayBuffer[writeOffset] = inL;
            delayBuffer[writeOffset + 1u] = inR;
            delayWritePos = (delayWritePos + 1u) % delayFrames;
            outL = inL;
            outR = inR;
            highPass.ProcessStereoSample(outL, outR);
            outputPeakL = (std::max)(outputPeakL, std::fabs(outL));
            outputPeakR = (std::max)(outputPeakR, std::fabs(outR));
            return;
        }

        if (peak > envelope)
            envelope += attackCoeff * (peak - envelope);
        else
            envelope += releaseCoeff * (peak - envelope);

        float gain = 1.0f;
        if (envelope > threshold) {
            gain = threshold / envelope;
            gainReductionDb = (std::max)(gainReductionDb,
                -20.0f * std::log10((std::max)(gain, 1.0e-12f)));
        }

        outL = delayBuffer[writeOffset] * gain;
        outR = delayBuffer[writeOffset + 1u] * gain;
        delayBuffer[writeOffset] = inL;
        delayBuffer[writeOffset + 1u] = inR;

        const float limitThreshold = threshold;
        const auto softLimit = [limitThreshold](float x) noexcept {
            const float ax = std::fabs(x);
            if (ax <= limitThreshold) return x;
            const float headroom = 1.0f - limitThreshold;
            const float compressed = limitThreshold + headroom *
                std::tanh((ax - limitThreshold) /
                    (headroom > 0.0001f ? headroom : 0.0001f));
            return x < 0.0f ? -compressed : compressed;
        };
        outL = softLimit(outL);
        outR = softLimit(outR);

        // Preserve the old meter ordering: Classic measured the post-limiter
        // sample before the 3 Hz DC blocker.
        outputPeakL = (std::max)(outputPeakL, std::fabs(outL));
        outputPeakR = (std::max)(outputPeakR, std::fabs(outR));
        highPass.ProcessStereoSample(outL, outR);
        delayWritePos = (delayWritePos + 1u) % delayFrames;
    }
};

// Low-latency predictive limiter used by both the live driver and standalone
// renderer. It uses the configured lookahead as an actual future constraint
// window rather than merely delaying the audio behind a smoothed detector.
struct AdaptiveLimiterState {
    static constexpr uint32_t kMaxDelayFrames = 8192u;

    // The storage must be larger than kMaxDelayFrames. A ring with exactly N
    // slots cannot represent an N-sample delay because write-N aliases write.
    static constexpr uint32_t kDelayCapacity = 16384u;
    static constexpr uint32_t kDelayMask = kDelayCapacity - 1u;
    static_assert((kDelayCapacity & kDelayMask) == 0u,
                  "limiter delay capacity must be a power of two");
    static_assert(kDelayCapacity > kMaxDelayFrames,
                  "limiter delay storage must exceed maximum lookahead");

    static constexpr uint32_t kDetectorCapacity = 16384u;
    static constexpr uint32_t kDetectorMask = kDetectorCapacity - 1u;

    alignas(64) float delayBuffer[kDelayCapacity * 2u]{};
    alignas(64) float detectorGain[kDetectorCapacity]{};
    alignas(64) uint64_t detectorIndex[kDetectorCapacity]{};

    uint32_t delayWritePos = 0u;
    uint32_t delayFrames = 128u;
    uint32_t delayFramesTarget = 128u;

    float threshold = 0.95f;
    float thresholdTarget = 0.95f;
    float attackCoeff = 0.25f;
    float releaseCoeff = 0.001f;
    bool enabled = true;

    float envelope = 0.0f;

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
        cachedDeadlineFrames_ = UINT32_MAX;
        deadlineAttackCoeff_ = 1.0f;
        ResetMeters();
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

        averagePeakCoeff_ = TimeToCoeff(20.0f, configuredSampleRate_, 1.0f);
        densityCoeff_ = TimeToCoeff(25.0f, configuredSampleRate_, 1.0f);
        peakMemoryDecay_ = std::exp(-1.0f /
            ((std::max)(1.0f,
                15.0f * static_cast<float>(configuredSampleRate_) * 0.001f)));

        Reset();
    }

    void Process(float* interleaved, uint32_t numFrames, uint32_t channels,
                 PostHighPass3Hz& highPass) noexcept {
        ResetMeters();
        if (!interleaved || numFrames == 0u || channels == 0u) return;

        for (uint32_t f = 0u; f < numFrames; ++f) {
            const uint32_t offset = f * channels;
            const float inL = interleaved[offset];
            const float inR = channels > 1u ? interleaved[offset + 1u] : inL;
            float outL = 0.0f;
            float outR = 0.0f;
            ProcessStereoFrame(inL, inR, outL, outR, highPass);
            interleaved[offset] = outL;
            if (channels > 1u) interleaved[offset + 1u] = outR;
        }

        FinishBlock(highPass);
    }

    void ProcessPlanar(float* left, float* right, uint32_t numFrames,
                       PostHighPass3Hz& highPass) noexcept {
        ResetMeters();
        if (!left || !right || numFrames == 0u) return;

        for (uint32_t f = 0u; f < numFrames; ++f) {
            const float inL = left[f];
            const float inR = right[f];
            float outL = 0.0f;
            float outR = 0.0f;
            ProcessStereoFrame(inL, inR, outL, outR, highPass);
            left[f] = outL;
            right[f] = outR;
        }

        FinishBlock(highPass);
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
        return (std::min)(kMaxDelayFrames, frames);
    }

    static float TimeToCoeff(float milliseconds, uint32_t sampleRate,
                             float minimumMs) noexcept {
        milliseconds = (std::max)(minimumMs, milliseconds);
        const float samples = (std::max)(1.0f,
            milliseconds * static_cast<float>(sampleRate) * 0.001f);
        return 1.0f - std::exp(-1.0f / samples);
    }

    void ResetMeters() noexcept {
        inputPeakL = inputPeakR = outputPeakL = outputPeakR = 0.0f;
        gainReductionDb = 0.0f;
        minimumAppliedGain_ = 1.0f;
    }

    void FinishBlock(PostHighPass3Hz& highPass) noexcept {
        highPass.FinishBlock();
        if (minimumAppliedGain_ < 0.999999f) {
            gainReductionDb = -20.0f * std::log10(
                (std::max)(minimumAppliedGain_, 1.0e-12f));
        }
    }

    void AdvanceDelay() noexcept {
        delayWritePos = (delayWritePos + 1u) & kDelayMask;
    }

    void ProcessStereoFrame(float inL, float inR,
                            float& outL, float& outR,
                            PostHighPass3Hz& highPass) noexcept {
        const float absL = std::fabs(inL);
        const float absR = std::fabs(inR);
        const float peak = (std::max)(absL, absR);
        envelope = peak;
        inputPeakL = (std::max)(inputPeakL, absL);
        inputPeakR = (std::max)(inputPeakR, absR);

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
            currentGain_ = 1.0f;
            outL = inL;
            outR = inR;
            highPass.ProcessStereoSample(outL, outR);
            outputPeakL = (std::max)(outputPeakL, std::fabs(outL));
            outputPeakR = (std::max)(outputPeakR, std::fabs(outR));
            AdvanceDelay();
            ++sampleCounter_;
            return;
        }

        RecomputeDeadlineAttackIfNeeded();
        const float effectiveAttack =
            (std::max)(ClampCoeff(attackCoeff), deadlineAttackCoeff_);

        if (predictiveTarget < currentGain_) {
            currentGain_ += effectiveAttack *
                (predictiveTarget - currentGain_);
        } else {
            const float adaptiveRelease = ComputeAdaptiveReleaseCoeff();
            currentGain_ += adaptiveRelease *
                (predictiveTarget - currentGain_);
        }
        currentGain_ = (std::max)(0.0f, (std::min)(1.0f, currentGain_));

        const uint32_t readPos =
            (delayWritePos - delayFrames) & kDelayMask;
        const uint32_t readOffset = readPos * 2u;
        outL = delayBuffer[readOffset] * currentGain_;
        outR = delayBuffer[readOffset + 1u] * currentGain_;

        highPass.ProcessStereoSample(outL, outR);

        float safetyGain = 1.0f;
        const float postPeak = (std::max)(std::fabs(outL), std::fabs(outR));
        if (postPeak > threshold && postPeak > 0.0f) {
            safetyGain = threshold / postPeak;
            outL *= safetyGain;
            outR *= safetyGain;
        }

        outputPeakL = (std::max)(outputPeakL, std::fabs(outL));
        outputPeakR = (std::max)(outputPeakR, std::fabs(outR));
        minimumAppliedGain_ = (std::min)(minimumAppliedGain_,
                                          currentGain_ * safetyGain);

        AdvanceDelay();
        ++sampleCounter_;
    }

    float PushDetectorTarget(float target) noexcept {
        target = (std::max)(0.0f, (std::min)(1.0f, target));

        while (detectorHead_ != detectorTail_) {
            const uint32_t back = (detectorTail_ - 1u) & kDetectorMask;
            if (detectorGain[back] < target) break;
            detectorTail_ = back;
        }
        detectorGain[detectorTail_] = target;
        detectorIndex[detectorTail_] = sampleCounter_;
        detectorTail_ = (detectorTail_ + 1u) & kDetectorMask;

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

        if (delayFrames == 0u) {
            deadlineAttackCoeff_ = 1.0f;
            return;
        }

        constexpr float kResidual = 0.001f;
        deadlineAttackCoeff_ = 1.0f - std::exp(
            std::log(kResidual) / static_cast<float>(delayFrames));
        deadlineAttackCoeff_ = ClampCoeff(deadlineAttackCoeff_);
    }

    void UpdateProgramAnalysis(float peak, float rawRequiredGain) noexcept {
        averagePeak_ += averagePeakCoeff_ * (peak - averagePeak_);
        peakMemory_ = (std::max)(peak, peakMemory_ * peakMemoryDecay_);
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

        float timeScale = 0.65f + 1.85f * density;
        timeScale *= 1.0f - 0.35f * transient;
        timeScale = (std::max)(0.35f, (std::min)(3.0f, timeScale));
        return (std::min)(1.0f, base / timeScale);
    }

    uint32_t detectorHead_ = 0u;
    uint32_t detectorTail_ = 0u;
    uint64_t sampleCounter_ = 0u;

    float currentGain_ = 1.0f;
    float averagePeak_ = 0.0f;
    float peakMemory_ = 0.0f;
    float limitingDensity_ = 0.0f;
    float minimumAppliedGain_ = 1.0f;

    uint32_t configuredSampleRate_ = 44100u;
    float averagePeakCoeff_ = 0.001f;
    float densityCoeff_ = 0.001f;
    float peakMemoryDecay_ = 0.999f;

    uint32_t cachedDeadlineFrames_ = UINT32_MAX;
    float deadlineAttackCoeff_ = 1.0f;
};

// Keeps both algorithms hot and crossfades between them on a live selector
// change. No allocation occurs on the audio thread. The destination limiter
// has already seen the same input history when the switch begins, so A/B is
// immediate without flushing either lookahead buffer.
struct LimiterRouterState {
    static constexpr uint32_t kMaxDelayFrames = AdaptiveLimiterState::kMaxDelayFrames;
    static constexpr uint32_t kScratchFrames = 8192u;

    bool enabled = true;
    uint32_t algorithmTarget = static_cast<uint32_t>(LimiterAlgorithm::Classic);
    float thresholdTarget = 0.95f;
    uint32_t delayFramesTarget = 128u;
    float attackCoeff = 0.25f;
    float releaseCoeff = 0.001f;

    // Public compatibility fields used by current diagnostics / RuntimeLink.
    float inputPeakL = 0.0f;
    float inputPeakR = 0.0f;
    float outputPeakL = 0.0f;
    float outputPeakR = 0.0f;
    float gainReductionDb = 0.0f;

    void Configure(uint32_t sampleRate, const EngineConfig& cfg) noexcept {
        configuredSampleRate_ = (std::max)(1u, sampleRate);
        enabled = cfg.limiterEnabled;
        algorithmTarget = static_cast<uint32_t>(cfg.limiterAlgorithm);
        if (algorithmTarget > 1u) algorithmTarget = 0u;
        thresholdTarget = (std::max)(0.1f, (std::min)(1.0f, cfg.limiterThreshold));
        delayFramesTarget = (std::min)(kMaxDelayFrames,
            static_cast<uint32_t>((std::max)(0.0f, cfg.limiterLookaheadMs) *
                static_cast<float>(configuredSampleRate_) * 0.001f + 0.5f));
        attackCoeff = TimeToCoeff(cfg.limiterAttackMs, configuredSampleRate_, 0.01f);
        releaseCoeff = TimeToCoeff(cfg.limiterReleaseMs, configuredSampleRate_, 1.0f);

        classicHighPass_.Initialize(configuredSampleRate_);
        adaptiveHighPass_.Initialize(configuredSampleRate_);
        classic.Configure(configuredSampleRate_, cfg);
        adaptive.Configure(configuredSampleRate_, cfg);

        blend_ = algorithmTarget == static_cast<uint32_t>(LimiterAlgorithm::Adaptive)
            ? 1.0f : 0.0f;
        blendStep_ = 1.0f / (std::max)(1.0f,
            static_cast<float>(configuredSampleRate_) * 0.005f); // ~5 ms
        ResetMeters();
    }

    void Reset() noexcept {
        classic.Reset();
        adaptive.Reset();
        classicHighPass_.Reset();
        adaptiveHighPass_.Reset();
        blend_ = algorithmTarget == static_cast<uint32_t>(LimiterAlgorithm::Adaptive)
            ? 1.0f : 0.0f;
        ResetMeters();
    }

    void Process(float* interleaved, uint32_t numFrames, uint32_t channels,
                 PostHighPass3Hz& legacyHighPass) noexcept {
        (void)legacyHighPass;
        ResetMeters();
        if (!interleaved || numFrames == 0u || channels == 0u) return;

        SyncChildren();

        uint32_t base = 0u;
        while (base < numFrames) {
            const uint32_t count = (std::min)(kScratchFrames, numFrames - base);
            for (uint32_t f = 0u; f < count; ++f) {
                const uint32_t sourceOffset = (base + f) * channels;
                scratch_[f * 2u] = interleaved[sourceOffset];
                scratch_[f * 2u + 1u] = channels > 1u
                    ? interleaved[sourceOffset + 1u]
                    : interleaved[sourceOffset];
            }

            // Classic gets the host buffer; Adaptive gets the copy. Both see
            // identical undelayed input and therefore stay warm continuously.
            classic.Process(interleaved + base * channels, count, channels,
                            classicHighPass_);
            adaptive.Process(scratch_, count, 2u, adaptiveHighPass_);

            AccumulateReduction();
            for (uint32_t f = 0u; f < count; ++f) {
                StepBlend();
                const uint32_t outputOffset = (base + f) * channels;
                const float classicL = interleaved[outputOffset];
                const float classicR = channels > 1u
                    ? interleaved[outputOffset + 1u] : classicL;
                const float adaptiveL = scratch_[f * 2u];
                const float adaptiveR = scratch_[f * 2u + 1u];
                const float inverse = 1.0f - blend_;
                const float outL = classicL * inverse + adaptiveL * blend_;
                const float outR = classicR * inverse + adaptiveR * blend_;
                interleaved[outputOffset] = outL;
                if (channels > 1u) interleaved[outputOffset + 1u] = outR;
                outputPeakL = (std::max)(outputPeakL, std::fabs(outL));
                outputPeakR = (std::max)(outputPeakR, std::fabs(outR));
            }
            inputPeakL = (std::max)(inputPeakL,
                (std::max)(classic.inputPeakL, adaptive.inputPeakL));
            inputPeakR = (std::max)(inputPeakR,
                (std::max)(classic.inputPeakR, adaptive.inputPeakR));
            base += count;
        }
    }

    void ProcessPlanar(float* left, float* right, uint32_t numFrames,
                       PostHighPass3Hz& legacyHighPass) noexcept {
        (void)legacyHighPass;
        ResetMeters();
        if (!left || !right || numFrames == 0u) return;

        SyncChildren();

        uint32_t base = 0u;
        while (base < numFrames) {
            const uint32_t count = (std::min)(kScratchFrames, numFrames - base);
            float* scratchL = scratch_;
            float* scratchR = scratch_ + kScratchFrames;
            std::memcpy(scratchL, left + base,
                        static_cast<size_t>(count) * sizeof(float));
            std::memcpy(scratchR, right + base,
                        static_cast<size_t>(count) * sizeof(float));

            classic.ProcessPlanar(left + base, right + base, count,
                                  classicHighPass_);
            adaptive.ProcessPlanar(scratchL, scratchR, count,
                                   adaptiveHighPass_);

            AccumulateReduction();
            for (uint32_t f = 0u; f < count; ++f) {
                StepBlend();
                const float inverse = 1.0f - blend_;
                const float outL = left[base + f] * inverse + scratchL[f] * blend_;
                const float outR = right[base + f] * inverse + scratchR[f] * blend_;
                left[base + f] = outL;
                right[base + f] = outR;
                outputPeakL = (std::max)(outputPeakL, std::fabs(outL));
                outputPeakR = (std::max)(outputPeakR, std::fabs(outR));
            }
            inputPeakL = (std::max)(inputPeakL,
                (std::max)(classic.inputPeakL, adaptive.inputPeakL));
            inputPeakR = (std::max)(inputPeakR,
                (std::max)(classic.inputPeakR, adaptive.inputPeakR));
            base += count;
        }
    }

    ClassicLimiterState classic;
    AdaptiveLimiterState adaptive;

private:
    static float TimeToCoeff(float milliseconds, uint32_t sampleRate,
                             float minimumMs) noexcept {
        milliseconds = (std::max)(minimumMs, milliseconds);
        const float samples = (std::max)(1.0f,
            milliseconds * static_cast<float>(sampleRate) * 0.001f);
        return 1.0f - std::exp(-1.0f / samples);
    }

    void ResetMeters() noexcept {
        inputPeakL = inputPeakR = outputPeakL = outputPeakR = 0.0f;
        gainReductionDb = 0.0f;
    }

    void SyncChildren() noexcept {
        if (algorithmTarget > 1u) algorithmTarget = 0u;

        classic.enabled = enabled;
        adaptive.enabled = enabled;
        classic.thresholdTarget = thresholdTarget;
        adaptive.thresholdTarget = thresholdTarget;
        classic.delayFramesTarget = (std::max)(1u,
            (std::min)(ClassicLimiterState::kMaxDelayFrames, delayFramesTarget));
        adaptive.delayFramesTarget = (std::min)(AdaptiveLimiterState::kMaxDelayFrames,
                                                 delayFramesTarget);
        classic.attackCoeff = attackCoeff;
        adaptive.attackCoeff = attackCoeff;
        classic.releaseCoeff = releaseCoeff;
        adaptive.releaseCoeff = releaseCoeff;
    }

    void StepBlend() noexcept {
        const float target = algorithmTarget ==
            static_cast<uint32_t>(LimiterAlgorithm::Adaptive) ? 1.0f : 0.0f;
        if (blend_ < target) blend_ = (std::min)(target, blend_ + blendStep_);
        else if (blend_ > target) blend_ = (std::max)(target, blend_ - blendStep_);
    }

    void AccumulateReduction() noexcept {
        // While crossfading, report the stronger child so the GR meter never
        // misleadingly drops to zero during an A/B transition.
        if (blend_ > 0.001f && blend_ < 0.999f)
            gainReductionDb = (std::max)(gainReductionDb,
                (std::max)(classic.gainReductionDb, adaptive.gainReductionDb));
        else if (blend_ >= 0.999f)
            gainReductionDb = (std::max)(gainReductionDb, adaptive.gainReductionDb);
        else
            gainReductionDb = (std::max)(gainReductionDb, classic.gainReductionDb);
    }

    uint32_t configuredSampleRate_ = 44100u;
    float blend_ = 0.0f;
    float blendStep_ = 1.0f / 220.5f;
    alignas(64) float scratch_[kScratchFrames * 2u]{};
    PostHighPass3Hz classicHighPass_{};
    PostHighPass3Hz adaptiveHighPass_{};
};

} // namespace svms
