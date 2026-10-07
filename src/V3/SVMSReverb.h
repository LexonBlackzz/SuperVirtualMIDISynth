#pragma once

// Driver reverb: 8-line FDN with input diffusion, predelay, early
// reflections and glided live-parameter changes. Moved verbatim out of
// SVMSDriver.cpp so other hosts can share it.

#include "SVMSConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace svms {

struct ReverbState {
    static constexpr uint32_t kFdnLines = 8u;
    static constexpr uint32_t kInputDiffusers = 2u;
    static constexpr uint32_t kEarlyTapCount = 6u;

    // Power-of-two ring buffers.
    //
    // 65536 frames gives:
    //   1.486 s @ 44.1 kHz
    //   341 ms @ 192 kHz
    //   171 ms @ 384 kHz
    //
    // Plenty for the FDN delays used here.
    static constexpr uint32_t kFdnBufferFrames = 65536u;
    static constexpr uint32_t kFdnBufferMask = kFdnBufferFrames - 1u;

    static constexpr uint32_t kEarlyBufferFrames = 65536u;
    static constexpr uint32_t kEarlyBufferMask = kEarlyBufferFrames - 1u;

    // Input diffusers only need short delays.
    static constexpr uint32_t kAllpassBufferFrames = 8192u;

    // 200 ms at 384 kHz = 76800 samples.
    // Use power-of-two buffer for cheap wrapping.
    static constexpr uint32_t kPreDelayBufferFrames = 131072u;
    static constexpr uint32_t kPreDelayBufferMask =
        kPreDelayBufferFrames - 1u;

    static constexpr float kPi =
        3.14159265358979323846f;

    static constexpr float kInvSqrt2 =
        0.7071067811865475244f;

    static constexpr float kInvSqrt8 =
        0.3535533905932737622f;

    // Prevent the reverb from becoming a rumour again.
    static constexpr float kFdnInputGain = 0.42f;
    static constexpr float kEarlyOutputGain = 0.60f;

    static_assert(
        (kFdnBufferFrames & (kFdnBufferFrames - 1u)) == 0u);

    static_assert(
        (kEarlyBufferFrames & (kEarlyBufferFrames - 1u)) == 0u);

    static_assert(
        (kPreDelayBufferFrames & (kPreDelayBufferFrames - 1u)) == 0u);

    // =====================================================================
    // FDN
    // =====================================================================

    alignas(64)
    float fdnDelay[kFdnLines][kFdnBufferFrames]{};

    uint32_t fdnWritePosition = 0u;

    float fdnBaseDelayFrames[kFdnLines]{};
    float fdnModDepthFrames[kFdnLines]{};
    float fdnFeedback[kFdnLines]{};

    // One-pole damping state for each feedback path.
    float fdnDampingState[kFdnLines]{};

    // =====================================================================
    // FDN modulation
    //
    // Recursive sine oscillator:
    //
    // sin(a+b) = sin(a)cos(b) + cos(a)sin(b)
    // cos(a+b) = cos(a)cos(b) - sin(a)sin(b)
    //
    // No sin() calls in the sample loop.
    // =====================================================================

    float lfoSin[kFdnLines]{};
    float lfoCos[kFdnLines]{};

    float lfoSinIncrement[kFdnLines]{};
    float lfoCosIncrement[kFdnLines]{};

    // =====================================================================
    // Input diffusion
    // =====================================================================

    alignas(64)
    float allpassDelay[2][kInputDiffusers][kAllpassBufferFrames]{};

    uint32_t allpassPosition[2][kInputDiffusers]{};
    uint32_t allpassLength[2][kInputDiffusers]{
        {1u, 1u},
        {1u, 1u}
    };

    float allpassFeedback = 0.60f;

    // =====================================================================
    // Predelay
    // =====================================================================

    alignas(64)
    float preDelay[2][kPreDelayBufferFrames]{};

    uint32_t preDelayPosition = 0u;
    uint32_t preDelayLength = 0u;

    // =====================================================================
    // Early reflections
    // =====================================================================

    alignas(64)
    float earlyDelay[2][kEarlyBufferFrames]{};

    uint32_t earlyPosition = 0u;

    uint32_t earlyTapFrames[kEarlyTapCount][2]{};

    // =====================================================================
    // Wet output filtering
    // =====================================================================

    float outputHighpassState[2]{};
    float outputLowpassState[2]{};

    float outputHighpassAlpha = 0.0f;
    float outputLowpassAlpha = 1.0f;

    // Feedback damping coefficient.
    float dampingAlpha = 1.0f;

    // =====================================================================
    // User parameters
    // =====================================================================

    bool enabled = false;

    float mix = 0.25f;

    float roomSize = 0.60f;
    float decay = 0.50f;
    float damping = 0.35f;
    float width = 1.0f;

    float diffusion = 0.70f;
    float preDelayMs = 12.0f;

    float earlyLevel = 0.35f;
    float lateLevel = 0.85f;

    float modDepth = 0.30f;
    float modRate = 0.35f;

    float lowCutHz = 70.0f;
    float highCutHz = 16000.0f;

    float decaySeconds = 4.6875f;

    uint32_t configuredSampleRate = 44100u;

    // =====================================================================
    // Live-change morph targets
    //
    // Delay lengths never jump: UpdateDerived() writes the *target* values
    // here, and Process() glides the current values toward them by a small
    // per-sample step.  A length step on a ring buffer is otherwise a
    // splice (read-position jump = click); the glide turns it into a brief
    // pitch/behaviour wobble instead.  Coefficients (feedback, damping,
    // filter alphas, gains) apply instantly — a coefficient step is a
    // smooth amplitude/tone transition on material already in the lines.
    // =====================================================================

    uint32_t preDelayLengthTarget = 0u;
    uint32_t allpassLengthTarget[2][kInputDiffusers]{
        {1u, 1u},
        {1u, 1u}
    };
    uint32_t earlyTapFramesTarget[kEarlyTapCount][2]{};
    float fdnBaseDelayFramesTarget[kFdnLines]{};

    // True while any delay-length current differs from its target.
    // UpdateDerived() arms it (it runs only when live reverb parameters
    // actually changed); Process() clears it once every glide lands.
    bool gliding_ = false;

    static uint32_t GlideU32(uint32_t current, uint32_t target,
                             uint32_t step) noexcept {
        if (current < target) {
            const uint32_t next = current + step;
            return next > target ? target : next;
        }
        if (current > target) {
            const uint32_t next = current - step;
            return next < target ? target : next;
        }
        return current;
    }

    static float GlideF32(float current, float target, float step) noexcept {
        if (current < target) return (std::min)(target, current + step);
        if (current > target) return (std::max)(target, current - step);
        return current;
    }

    // =====================================================================
    // Helpers
    // =====================================================================

    static float Clamp(
        float value,
        float minimum,
        float maximum) noexcept
    {
        return (std::max)(
            minimum,
            (std::min)(maximum, value));
    }

    static float Clamp01(float value) noexcept {
        return Clamp(value, 0.0f, 1.0f);
    }

    static uint32_t MsToFrames(
        float milliseconds,
        uint32_t sampleRate,
        uint32_t maxFrames) noexcept
    {
        const float frames =
            milliseconds *
            0.001f *
            static_cast<float>(sampleRate);

        return (std::min)(
            maxFrames,
            (std::max)(
                1u,
                static_cast<uint32_t>(frames + 0.5f)));
    }

    static float OnePoleAlpha(
        float cutoffHz,
        uint32_t sampleRate) noexcept
    {
        const float sr =
            static_cast<float>(sampleRate);

        cutoffHz =
            Clamp(
                cutoffHz,
                1.0f,
                sr * 0.45f);

        return
            1.0f -
            std::exp(
                -2.0f *
                kPi *
                cutoffHz /
                sr);
    }

    static float ComputeFeedback(
        float delayFrames,
        uint32_t sampleRate,
        float rt60Seconds) noexcept
    {
        const float delaySeconds =
            delayFrames /
            static_cast<float>(sampleRate);

        // -60 dB after RT60.
        const float feedback =
            std::pow(
                10.0f,
                -3.0f *
                delaySeconds /
                rt60Seconds);

        return Clamp(
            feedback,
            0.0f,
            0.9997f);
    }

    // ---------------------------------------------------------------------
    // Fractional FDN read with linear interpolation.
    // ---------------------------------------------------------------------

    float ReadFdnDelay(
        uint32_t line,
        float delayFrames) const noexcept
    {
        delayFrames =
            Clamp(
                delayFrames,
                1.0f,
                static_cast<float>(
                    kFdnBufferFrames - 3u));

        const uint32_t whole =
            static_cast<uint32_t>(delayFrames);

        const float fraction =
            delayFrames -
            static_cast<float>(whole);

        const uint32_t index0 =
            (fdnWritePosition - whole) &
            kFdnBufferMask;

        const uint32_t index1 =
            (index0 - 1u) &
            kFdnBufferMask;

        const float a =
            fdnDelay[line][index0];

        const float b =
            fdnDelay[line][index1];

        return
            a +
            (b - a) *
            fraction;
    }

    // ---------------------------------------------------------------------
    // Proper Schroeder allpass diffuser.
    // ---------------------------------------------------------------------

    float ProcessAllpass(
        float input,
        uint32_t channel,
        uint32_t stage) noexcept
    {
        const uint32_t p =
            allpassPosition[channel][stage];

        const float delayed =
            allpassDelay[channel][stage][p];

        const float output =
            delayed -
            allpassFeedback *
            input;

        allpassDelay[channel][stage][p] =
            input +
            allpassFeedback *
            output;

        allpassPosition[channel][stage] =
            (p + 1u ==
             allpassLength[channel][stage])
                ? 0u
                : p + 1u;

        // Safety wrap: if the length ever lands below the advancing
        // position (shrinking lengths are glided, but a stale position
        // from a reparameterization would otherwise walk past the wrap
        // condition and read out of the ring until the buffer edge).
        if (allpassPosition[channel][stage] >= kAllpassBufferFrames)
            allpassPosition[channel][stage] = 0u;

        return output;
    }

    // =====================================================================
    // Reset
    // =====================================================================

    void Reset() noexcept {
        std::memset(
            fdnDelay,
            0,
            sizeof(fdnDelay));

        std::memset(
            fdnDampingState,
            0,
            sizeof(fdnDampingState));

        fdnWritePosition = 0u;

        // Seed the FDN LFOs with their spread initial phases.  Live
        // reparameterizations preserve the phase (UpdateDerived only
        // rewrites the increments); Reset is the init/reload point where
        // a fresh phase is wanted.
        static constexpr float kInitialPhase[kFdnLines] =
        {
            0.03f,
            0.17f,
            0.31f,
            0.46f,
            0.59f,
            0.71f,
            0.83f,
            0.94f
        };
        for (uint32_t i = 0u; i < kFdnLines; ++i) {
            const float angle = 2.0f * kPi * kInitialPhase[i];
            lfoSin[i] = std::sin(angle);
            lfoCos[i] = std::cos(angle);
        }

        std::memset(
            allpassDelay,
            0,
            sizeof(allpassDelay));

        std::memset(
            allpassPosition,
            0,
            sizeof(allpassPosition));

        std::memset(
            preDelay,
            0,
            sizeof(preDelay));

        preDelayPosition = 0u;

        std::memset(
            earlyDelay,
            0,
            sizeof(earlyDelay));

        earlyPosition = 0u;

        std::memset(
            outputHighpassState,
            0,
            sizeof(outputHighpassState));

        std::memset(
            outputLowpassState,
            0,
            sizeof(outputLowpassState));
    }

    // =====================================================================
    // Configure
    // =====================================================================

    void Configure(
        uint32_t sampleRate,
        const EngineConfig& cfg) noexcept
    {
        configuredSampleRate =
            (std::max)(1u, sampleRate);

        enabled =
            cfg.enableReverb;

        mix =
            Clamp01(cfg.reverbMix);

        roomSize =
            Clamp01(cfg.reverbRoomSize);

        decay =
            Clamp01(cfg.reverbDecay);

        damping =
            Clamp01(cfg.reverbDamping);

        width =
            Clamp01(cfg.reverbWidth);

        diffusion =
            Clamp01(cfg.reverbDiffusion);

        preDelayMs =
            Clamp(
                cfg.reverbPreDelayMs,
                0.0f,
                200.0f);

        earlyLevel =
            Clamp(
                cfg.reverbEarlyLevel,
                0.0f,
                1.5f);

        lateLevel =
            Clamp(
                cfg.reverbLateLevel,
                0.0f,
                1.5f);

        modDepth =
            Clamp01(cfg.reverbModDepth);

        modRate =
            Clamp01(cfg.reverbModRate);

        lowCutHz =
            Clamp(
                cfg.reverbLowCutHz,
                0.0f,
                2000.0f);

        highCutHz =
            Clamp(
                cfg.reverbHighCutHz,
                1000.0f,
                static_cast<float>(
                    configuredSampleRate) *
                    0.45f);

        // Derived coefficients (FDN delays, feedback, taps, filters,
        // LFOs) from the raw fields.  UpdateDerived never clears the
        // delay lines, so live parameter changes morph the tail in
        // place instead of cutting it dead.  Reset() here zeroes
        // everything for the init/reload path only.
        UpdateDerived();
        Reset();
    }

    // =====================================================================
    // UpdateDerived
    // =====================================================================
    //
    // Recomputes all derived coefficients (FDN delay lengths, feedback,
    // damping, diffuser/allpass lengths, predelay/early taps, wet EQ and
    // LFO increments) from the current raw fields.  Deliberately does NOT
    // clear any delay line or state: the audio thread calls this once per
    // block after folding live reverb parameters from the mailbox, so
    // knob moves morph the existing tail instead of cutting it.
    void UpdateDerived() noexcept
    {

        // =============================================================
        // RT60
        //
        // 0.0 -> 0.25 sec
        // 0.5 -> 4.69 sec
        // 0.7 -> 8.95 sec
        // 0.8 -> 11.61 sec
        // 1.0 -> 18 sec
        //
        // This is intentionally capable of being stupidly huge.
        // =============================================================

        decaySeconds =
            0.25f +
            decay *
            decay *
            17.75f;

        // =============================================================
        // FDN delay lengths
        // =============================================================

        static constexpr float
            kBaseDelayMs[kFdnLines] =
        {
            29.7f,
            37.1f,
            41.1f,
            43.7f,
            53.3f,
            59.9f,
            67.7f,
            73.9f
        };

        // Non-linear mapping:
        //
        // low room_size still gives useful small/medium rooms,
        // upper end expands aggressively into hall/cathedral territory.
        const float roomScale =
            0.60f +
            std::pow(
                roomSize,
                1.35f) *
            1.50f;

        // Maximum modulation is only a few milliseconds.
        //
        // Enough to break stationary resonances without becoming chorus.
        const float modulationMs =
            modDepth *
            modDepth *
            2.8f;

        static constexpr float
            kModDepthMultiplier[kFdnLines] =
        {
            0.73f,
            0.89f,
            1.07f,
            0.81f,
            1.17f,
            0.94f,
            1.11f,
            0.77f
        };

        for (uint32_t i = 0u;
             i < kFdnLines;
             ++i)
        {
            float baseFrames =
                kBaseDelayMs[i] *
                roomScale *
                0.001f *
                static_cast<float>(
                    configuredSampleRate);

            const float modFrames =
                modulationMs *
                kModDepthMultiplier[i] *
                0.001f *
                static_cast<float>(
                    configuredSampleRate);

            // Keep enough safety margin for interpolation +
            // modulation.
            const float maximumBase =
                static_cast<float>(
                    kFdnBufferFrames - 4u) -
                modFrames;

            baseFrames =
                Clamp(
                    baseFrames,
                    2.0f,
                    maximumBase);

            fdnBaseDelayFramesTarget[i] =
                baseFrames;

            // Snap the current length on the first update (and on
            // Configure/Reset, where the lines are empty anyway); later
            // live changes are glided by Process().
            if (fdnBaseDelayFrames[i] == 0.0f)
                fdnBaseDelayFrames[i] = baseFrames;

            fdnModDepthFrames[i] =
                modFrames;

            fdnFeedback[i] =
                ComputeFeedback(
                    baseFrames,
                    configuredSampleRate,
                    decaySeconds);
        }

        // =============================================================
        // Feedback damping
        //
        // damping = 0 -> bright tail
        // damping = 1 -> heavily damped tail
        //
        // Logarithmic cutoff mapping.
        // =============================================================

        constexpr float brightCutoff =
            18000.0f;

        constexpr float darkCutoff =
            1400.0f;

        const float dampingCutoff =
            brightCutoff *
            std::pow(
                darkCutoff /
                brightCutoff,
                damping);

        dampingAlpha =
            OnePoleAlpha(
                dampingCutoff,
                configuredSampleRate);

        // =============================================================
        // Input diffusion
        // =============================================================

        static constexpr float
            kDiffuserMs[kInputDiffusers] =
        {
            3.1f,
            7.7f
        };

        static constexpr float
            kRightOffsetMs[kInputDiffusers] =
        {
            0.43f,
            0.71f
        };

        const float diffuserScale =
            0.85f +
            roomSize *
            0.55f;

        for (uint32_t i = 0u;
             i < kInputDiffusers;
             ++i)
        {
            const uint32_t targetL =
                MsToFrames(
                    kDiffuserMs[i] *
                    diffuserScale,
                    configuredSampleRate,
                    kAllpassBufferFrames);

            const uint32_t targetR =
                MsToFrames(
                    (kDiffuserMs[i] +
                     kRightOffsetMs[i]) *
                    diffuserScale,
                    configuredSampleRate,
                    kAllpassBufferFrames);

            allpassLengthTarget[0][i] =
                targetL;
            allpassLengthTarget[1][i] =
                targetR;

            // First-time snap; live changes are glided in Process().
            if (allpassLength[0][i] == 0u)
                allpassLength[0][i] = targetL;
            if (allpassLength[1][i] == 0u)
                allpassLength[1][i] = targetR;

            // Ring-cursor normalization: a position past the (possibly
            // shrunken) length would never hit the wrap condition and
            // would read stale or out-of-wrap content until it wrapped
            // at the buffer edge; ProcessAllpass now also wraps at the
            // buffer edge, so clamp here for the warm-up case only.
            for (uint32_t ch = 0u; ch < 2u; ++ch) {
                const uint32_t len = allpassLength[ch][i];
                allpassPosition[ch][i] =
                    (std::min)(allpassPosition[ch][i], len != 0u ? len - 1u : 0u);
            }
        }

        allpassFeedback =
            0.22f +
            diffusion *
            0.53f;

        // =============================================================
        // Predelay
        // =============================================================

        if (preDelayMs <= 0.0f) {
            preDelayLengthTarget = 0u;
        }
        else {
            preDelayLengthTarget =
                MsToFrames(
                    preDelayMs,
                    configuredSampleRate,
                    kPreDelayBufferFrames - 1u);
        }

        // First-time snap (glided on live changes by Process()).
        preDelayLength =
            preDelayLengthTarget == 0u
                ? 0u
                : (preDelayLength != 0u
                       ? preDelayLength
                       : preDelayLengthTarget);

        // =============================================================
        // Early reflections
        // =============================================================

        static constexpr float
            kEarlyMs[kEarlyTapCount] =
        {
             7.1f,
            11.9f,
            18.3f,
            27.1f,
            39.7f,
            56.9f
        };

        static constexpr float
            kEarlyStereoOffsetMs[kEarlyTapCount] =
        {
             0.71f,
            -0.93f,
             1.27f,
            -1.53f,
             2.11f,
            -2.37f
        };

        const float earlyScale =
            0.70f +
            roomSize *
            0.90f;

        for (uint32_t i = 0u;
             i < kEarlyTapCount;
             ++i)
        {
            const uint32_t target0 =
                MsToFrames(
                    kEarlyMs[i] *
                    earlyScale,
                    configuredSampleRate,
                    kEarlyBufferFrames - 1u);

            const uint32_t target1 =
                MsToFrames(
                    (kEarlyMs[i] +
                     kEarlyStereoOffsetMs[i]) *
                    earlyScale,
                    configuredSampleRate,
                    kEarlyBufferFrames - 1u);

            earlyTapFramesTarget[i][0] =
                target0;
            earlyTapFramesTarget[i][1] =
                target1;

            // First-time snap; live changes are glided in Process().
            if (earlyTapFrames[i][0] == 0u)
                earlyTapFrames[i][0] = target0;
            if (earlyTapFrames[i][1] == 0u)
                earlyTapFrames[i][1] = target1;
        }

        // =============================================================
        // Wet EQ
        // =============================================================

        if (lowCutHz <= 0.0f) {
            outputHighpassAlpha =
                0.0f;
        }
        else {
            outputHighpassAlpha =
                OnePoleAlpha(
                    lowCutHz,
                    configuredSampleRate);
        }

        outputLowpassAlpha =
            OnePoleAlpha(
                highCutHz,
                configuredSampleRate);

        // =============================================================
        // LFO setup
        // =============================================================

        static constexpr float
            kRateMultiplier[kFdnLines] =
        {
            0.79f,
            0.93f,
            1.07f,
            1.19f,
            0.71f,
            1.31f,
            0.87f,
            1.13f
        };

        // 0.07 .. ~1.0 Hz.
        const float baseRateHz =
            0.07f +
            modRate *
            modRate *
            0.93f;

        for (uint32_t i = 0u;
             i < kFdnLines;
             ++i)
        {
            // LFO PHASE IS PRESERVED across live reparameterizations:
            // only the per-sample increment is recomputed, so a modRate
            // change sweeps the existing oscillator rather than resetting
            // it (the old code re-seeded sin/cos to the initial phase on
            // every UpdateDerived call, making each knob move restart the
            // modulation pattern).
            const float rateHz =
                baseRateHz *
                kRateMultiplier[i];

            const float increment =
                2.0f *
                kPi *
                rateHz /
                static_cast<float>(
                    configuredSampleRate);

            lfoSinIncrement[i] =
                std::sin(increment);

            lfoCosIncrement[i] =
                std::cos(increment);
        }

        // Arm the per-sample glides.  UpdateDerived only runs when live
        // reverb parameters changed (or on init/reload), so this flag is
        // not a per-block cost.
        gliding_ = true;
    }

    // =====================================================================
    // Process
    // =====================================================================

    void Process(
        float* audio,
        uint32_t frames,
        uint32_t channels) noexcept
    {
        if (!audio ||
            frames == 0u ||
            channels == 0u ||
            !enabled ||
            mix <= 0.0f)
        {
            return;
        }

        const float dryGain =
            1.0f - mix;

        // =============================================================
        // Delay-length glides (click-free live parameter changes)
        //
        // UpdateDerived() writes targets; here the current lengths creep
        // toward them by one frame per sample (FDN base delays by 1/4
        // frame — the read is interpolated, so a slower creep sounds
        // smoother).  A stepped ring length would be a splice/click.
        // =============================================================
        if (gliding_) {
            gliding_ = false;
            preDelayLength = GlideU32(preDelayLength,
                                      preDelayLengthTarget, 1u);
            if (preDelayLength != preDelayLengthTarget) gliding_ = true;
            for (uint32_t ch = 0u; ch < 2u; ++ch) {
                for (uint32_t i = 0u; i < kInputDiffusers; ++i) {
                    allpassLength[ch][i] = GlideU32(
                        allpassLength[ch][i],
                        allpassLengthTarget[ch][i], 1u);
                    if (allpassLength[ch][i] != allpassLengthTarget[ch][i])
                        gliding_ = true;
                }
            }
            for (uint32_t i = 0u; i < kEarlyTapCount; ++i) {
                earlyTapFrames[i][0] = GlideU32(
                    earlyTapFrames[i][0], earlyTapFramesTarget[i][0], 1u);
                if (earlyTapFrames[i][0] != earlyTapFramesTarget[i][0])
                    gliding_ = true;
                earlyTapFrames[i][1] = GlideU32(
                    earlyTapFrames[i][1], earlyTapFramesTarget[i][1], 1u);
                if (earlyTapFrames[i][1] != earlyTapFramesTarget[i][1])
                    gliding_ = true;
            }
            for (uint32_t i = 0u; i < kFdnLines; ++i) {
                fdnBaseDelayFrames[i] = GlideF32(
                    fdnBaseDelayFrames[i],
                    fdnBaseDelayFramesTarget[i], 0.25f);
                if (fdnBaseDelayFrames[i] != fdnBaseDelayFramesTarget[i])
                    gliding_ = true;
            }
        }

        // Fixed early reflection gains.
        static constexpr float
            kEarlyGain[kEarlyTapCount] =
        {
             0.52f,
            -0.39f,
             0.31f,
            -0.25f,
             0.20f,
            -0.16f
        };

        float* frame =
            audio;

        for (uint32_t f = 0u;
             f < frames;
             ++f,
             frame += channels)
        {
            const float dryL =
                frame[0];

            const float dryR =
                channels > 1u
                    ? frame[1]
                    : dryL;

            // =========================================================
            // Predelay
            // =========================================================

            float inputL =
                dryL;

            float inputR =
                dryR;

            if (preDelayLength > 0u) {
                const uint32_t readPosition =
                    (preDelayPosition -
                     preDelayLength) &
                    kPreDelayBufferMask;

                inputL =
                    preDelay[0][readPosition];

                inputR =
                    preDelay[1][readPosition];

                preDelay[0][preDelayPosition] =
                    dryL;

                preDelay[1][preDelayPosition] =
                    dryR;

                preDelayPosition =
                    (preDelayPosition + 1u) &
                    kPreDelayBufferMask;
            }

            // =========================================================
            // Early reflections
            // =========================================================

            earlyDelay[0][earlyPosition] =
                inputL;

            earlyDelay[1][earlyPosition] =
                inputR;

            float earlyL = 0.0f;
            float earlyR = 0.0f;

            for (uint32_t i = 0u;
                 i < kEarlyTapCount;
                 ++i)
            {
                const uint32_t readL =
                    (earlyPosition -
                     earlyTapFrames[i][0]) &
                    kEarlyBufferMask;

                const uint32_t readR =
                    (earlyPosition -
                     earlyTapFrames[i][1]) &
                    kEarlyBufferMask;

                // Alternate direct/cross-channel taps.
                //
                // Cheap way of making the early field much wider and
                // less "six obvious echoes".
                if ((i & 1u) == 0u) {
                    earlyL +=
                        earlyDelay[0][readL] *
                        kEarlyGain[i];

                    earlyR +=
                        earlyDelay[1][readR] *
                        kEarlyGain[i];
                }
                else {
                    earlyL +=
                        earlyDelay[1][readL] *
                        kEarlyGain[i];

                    earlyR +=
                        earlyDelay[0][readR] *
                        kEarlyGain[i];
                }
            }

            earlyPosition =
                (earlyPosition + 1u) &
                kEarlyBufferMask;

            earlyL *=
                kEarlyOutputGain;

            earlyR *=
                kEarlyOutputGain;

            // =========================================================
            // Input diffusion
            // =========================================================

            float diffusedL =
                inputL;

            float diffusedR =
                inputR;

            for (uint32_t i = 0u;
                 i < kInputDiffusers;
                 ++i)
            {
                diffusedL =
                    ProcessAllpass(
                        diffusedL,
                        0u,
                        i);

                diffusedR =
                    ProcessAllpass(
                        diffusedR,
                        1u,
                        i);
            }

            // =========================================================
            // Read all 8 modulated FDN lines
            // =========================================================

            float delayed[kFdnLines];
            float damped[kFdnLines];

            for (uint32_t i = 0u;
                 i < kFdnLines;
                 ++i)
            {
                // Recursive oscillator update.
                const float sinValue =
                    lfoSin[i];

                const float cosValue =
                    lfoCos[i];

                const float newSin =
                    sinValue *
                        lfoCosIncrement[i] +
                    cosValue *
                        lfoSinIncrement[i];

                const float newCos =
                    cosValue *
                        lfoCosIncrement[i] -
                    sinValue *
                        lfoSinIncrement[i];

                lfoSin[i] =
                    newSin;

                lfoCos[i] =
                    newCos;

                const float modulatedDelay =
                    fdnBaseDelayFrames[i] +
                    newSin *
                    fdnModDepthFrames[i];

                delayed[i] =
                    ReadFdnDelay(
                        i,
                        modulatedDelay);

                // Frequency-dependent decay.
                fdnDampingState[i] +=
                    dampingAlpha *
                    (delayed[i] -
                     fdnDampingState[i]);

                damped[i] =
                    fdnDampingState[i];
            }

            // =========================================================
            // Late stereo decode
            //
            // Different +/- patterns produce stereo decorrelation.
            // 1/sqrt(8) keeps the level sensible.
            // =========================================================

            float lateL =
                (
                    delayed[0] +
                    delayed[1] +
                    delayed[2] -
                    delayed[3] +
                    delayed[4] -
                    delayed[5] -
                    delayed[6] +
                    delayed[7]
                ) *
                kInvSqrt8;

            float lateR =
                (
                    delayed[0] -
                    delayed[1] +
                    delayed[2] +
                    delayed[3] -
                    delayed[4] +
                    delayed[5] -
                    delayed[6] -
                    delayed[7]
                ) *
                kInvSqrt8;

            // =========================================================
            // Householder feedback matrix
            //
            // H = I - (2/N) * 11^T
            //
            // N=8 -> 2/N = 0.25.
            //
            // This is the heart of the FDN.
            //
            // Instead of an 8x8 matrix multiply:
            //
            //     sum all lines once
            //     subtract 0.25 * sum from each line
            //
            // Very cheap and energy-preserving.
            // =========================================================

            const float sum =
                damped[0] +
                damped[1] +
                damped[2] +
                damped[3] +
                damped[4] +
                damped[5] +
                damped[6] +
                damped[7];

            const float matrixCommon =
                sum *
                0.25f;

            // =========================================================
            // Stereo input injection
            // =========================================================

            const float sumInput =
                (diffusedL + diffusedR) *
                kInvSqrt2;

            const float differenceInput =
                (diffusedL - diffusedR) *
                kInvSqrt2;

            const float injection[kFdnLines] =
            {
                 diffusedL,
                 diffusedR,
                 sumInput,
                 differenceInput,
                -diffusedL,
                -diffusedR,
                -sumInput,
                -differenceInput
            };

            // =========================================================
            // Write feedback network
            // =========================================================

            for (uint32_t i = 0u;
                 i < kFdnLines;
                 ++i)
            {
                const float mixed =
                    damped[i] -
                    matrixCommon;

                fdnDelay[i][fdnWritePosition] =
                    injection[i] *
                        kFdnInputGain +
                    mixed *
                        fdnFeedback[i];
            }

            fdnWritePosition =
                (fdnWritePosition + 1u) &
                kFdnBufferMask;

            // =========================================================
            // Early + late
            // =========================================================

            float wetL =
                earlyL *
                    earlyLevel +
                lateL *
                    lateLevel;

            float wetR =
                earlyR *
                    earlyLevel +
                lateR *
                    lateLevel;

            // =========================================================
            // Wet low-cut
            // =========================================================

            if (outputHighpassAlpha > 0.0f) {
                outputHighpassState[0] +=
                    outputHighpassAlpha *
                    (wetL -
                     outputHighpassState[0]);

                outputHighpassState[1] +=
                    outputHighpassAlpha *
                    (wetR -
                     outputHighpassState[1]);

                wetL -=
                    outputHighpassState[0];

                wetR -=
                    outputHighpassState[1];
            }

            // =========================================================
            // Wet high-cut
            // =========================================================

            outputLowpassState[0] +=
                outputLowpassAlpha *
                (wetL -
                 outputLowpassState[0]);

            outputLowpassState[1] +=
                outputLowpassAlpha *
                (wetR -
                 outputLowpassState[1]);

            wetL =
                outputLowpassState[0];

            wetR =
                outputLowpassState[1];

            // =========================================================
            // Stereo width
            // =========================================================

            const float mid =
                0.5f *
                (wetL + wetR);

            const float side =
                0.5f *
                (wetL - wetR) *
                width;

            wetL =
                mid + side;

            wetR =
                mid - side;

            // =========================================================
            // Dry / wet
            // =========================================================

            frame[0] =
                dryL *
                    dryGain +
                wetL *
                    mix;

            if (channels > 1u) {
                frame[1] =
                    dryR *
                        dryGain +
                    wetR *
                        mix;
            }
        }

        // =================================================================
        // LFO drift cleanup.
        //
        // Only 8 sqrt() calls per PROCESS BLOCK, not per sample.
        // =================================================================

        for (uint32_t i = 0u;
             i < kFdnLines;
             ++i)
        {
            const float magnitudeSquared =
                lfoSin[i] *
                    lfoSin[i] +
                lfoCos[i] *
                    lfoCos[i];

            if (magnitudeSquared > 0.0f) {
                const float inverseMagnitude =
                    1.0f /
                    std::sqrt(
                        magnitudeSquared);

                lfoSin[i] *=
                    inverseMagnitude;

                lfoCos[i] *=
                    inverseMagnitude;
            }
        }

        // =================================================================
        // Denormal cleanup.
        //
        // Still preferably enable FTZ/DAZ for the audio thread globally.
        // =================================================================

        for (uint32_t i = 0u;
             i < kFdnLines;
             ++i)
        {
            if (std::fabs(
                    fdnDampingState[i]) <
                1.0e-20f)
            {
                fdnDampingState[i] =
                    0.0f;
            }
        }

        for (uint32_t c = 0u;
             c < 2u;
             ++c)
        {
            if (std::fabs(
                    outputHighpassState[c]) <
                1.0e-20f)
            {
                outputHighpassState[c] =
                    0.0f;
            }

            if (std::fabs(
                    outputLowpassState[c]) <
                1.0e-20f)
            {
                outputLowpassState[c] =
                    0.0f;
            }
        }
    }
};

} // namespace svms
