#pragma once

#include <cmath>
#include <cstdint>

namespace svms {

// First-order DC blocker with a 3 Hz pole. This runs on the final limiter
// output so it removes subsonic energy without altering gain detection.
// State is continuous across callback boundaries and all coefficient work is
// performed during initialization, not on the audio thread.
class PostHighPass3Hz {
public:
    void Initialize(uint32_t sampleRate) noexcept {
        constexpr float kTwoPi = 6.28318530717958647692f;
        pole_ = sampleRate != 0u
            ? std::exp(-kTwoPi * 3.0f / static_cast<float>(sampleRate))
            : 0.0f;
        Reset();
    }

    void Reset() noexcept {
        previousInputL_ = 0.0f;
        previousInputR_ = 0.0f;
        previousOutputL_ = 0.0f;
        previousOutputR_ = 0.0f;
    }

    void ProcessPlanar(float* left, float* right, uint32_t frames) noexcept {
        if (!left || !right || frames == 0u) return;
        float x1L = previousInputL_;
        float x1R = previousInputR_;
        float y1L = previousOutputL_;
        float y1R = previousOutputR_;
        const float pole = pole_;
        for (uint32_t i = 0; i < frames; ++i) {
            const float xL = left[i];
            const float xR = right[i];
            const float yL = xL - x1L + pole * y1L;
            const float yR = xR - x1R + pole * y1R;
            left[i] = yL;
            right[i] = yR;
            x1L = xL;
            x1R = xR;
            y1L = yL;
            y1R = yR;
        }
        Commit(x1L, x1R, y1L, y1R);
    }

    void ProcessInterleavedStereo(float* samples, uint32_t frames) noexcept {
        if (!samples || frames == 0u) return;
        float x1L = previousInputL_;
        float x1R = previousInputR_;
        float y1L = previousOutputL_;
        float y1R = previousOutputR_;
        const float pole = pole_;
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t offset = i * 2u;
            const float xL = samples[offset];
            const float xR = samples[offset + 1u];
            const float yL = xL - x1L + pole * y1L;
            const float yR = xR - x1R + pole * y1R;
            samples[offset] = yL;
            samples[offset + 1u] = yR;
            x1L = xL;
            x1R = xR;
            y1L = yL;
            y1R = yR;
        }
        Commit(x1L, x1R, y1L, y1R);
    }

    void ProcessStereoSample(float& left, float& right) noexcept {
        const float inputL = left;
        const float inputR = right;
        left = inputL - previousInputL_ + pole_ * previousOutputL_;
        right = inputR - previousInputR_ + pole_ * previousOutputR_;
        previousInputL_ = inputL;
        previousInputR_ = inputR;
        previousOutputL_ = left;
        previousOutputR_ = right;
    }

    void FinishBlock() noexcept {
        if (std::fabs(previousOutputL_) < 1.0e-20f) previousOutputL_ = 0.0f;
        if (std::fabs(previousOutputR_) < 1.0e-20f) previousOutputR_ = 0.0f;
    }

    float GetPole() const noexcept { return pole_; }

private:
    void Commit(float x1L, float x1R, float y1L, float y1R) noexcept {
        // Avoid carrying denormals forever when a stream becomes silent.
        if (std::fabs(y1L) < 1.0e-20f) y1L = 0.0f;
        if (std::fabs(y1R) < 1.0e-20f) y1R = 0.0f;
        previousInputL_ = x1L;
        previousInputR_ = x1R;
        previousOutputL_ = y1L;
        previousOutputR_ = y1R;
    }

    float pole_ = 0.0f;
    float previousInputL_ = 0.0f;
    float previousInputR_ = 0.0f;
    float previousOutputL_ = 0.0f;
    float previousOutputR_ = 0.0f;
};

} // namespace svms
