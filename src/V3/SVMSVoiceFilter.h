#ifndef SVMS_VOICE_FILTER_H
#define SVMS_VOICE_FILTER_H

#include "SVMSTypes.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace svms {

struct PreparedVoiceFilter {
    float a0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    uint32_t enabled = 0u;
};

// SoundFont low-pass. initialFilterFc is absolute cents relative to 8.176 Hz;
// initialFilterQ is centibels. The coefficient model matches TinySoundFont's
// vendored resonant two-pole DF2T filter.
inline PreparedVoiceFilter PrepareVoiceLowPass(
    uint8_t filterType, int16_t cutoffCents, int16_t resonanceCentibels,
    uint32_t sampleRate) noexcept {
    PreparedVoiceFilter out{};
    if (filterType != static_cast<uint8_t>(FilterType::LowPass2Pole) ||
        sampleRate == 0u) {
        return out;
    }

    const float cents = static_cast<float>((std::max)(
        -12000, (std::min)(13500, static_cast<int>(cutoffCents))));
    const float cutoffHz = 8.176f * std::pow(2.0f, cents / 1200.0f);
    const float normalized = cutoffHz / static_cast<float>(sampleRate);
    if (!(normalized > 0.0f) || normalized >= 0.499f) return out;

    const float qDb = static_cast<float>((std::max)(
        0, (std::min)(960, static_cast<int>(resonanceCentibels)))) * 0.1f;
    const float qInv = 1.0f / std::pow(10.0f, qDb / 20.0f);
    constexpr float kPi = 3.14159265358979323846f;
    const float k = std::tan(kPi * normalized);
    const float kk = k * k;
    const float norm = 1.0f / (1.0f + k * qInv + kk);

    out.a0 = kk * norm;
    out.b1 = 2.0f * (kk - 1.0f) * norm;
    out.b2 = (1.0f - k * qInv + kk) * norm;
    out.enabled = 1u;
    return out;
}

inline float ProcessVoiceFilterSample(
    VoiceSoA& v, uint32_t handle, float input) noexcept {
    if (v.filterEnabled[handle] == 0u) return input;
    const float a0 = v.filterA0[handle];
    const float output = input * a0 + v.filterZ1[handle];
    v.filterZ1[handle] =
        input * (2.0f * a0) + v.filterZ2[handle] -
        v.filterB1[handle] * output;
    v.filterZ2[handle] =
        input * a0 - v.filterB2[handle] * output;
    return output;
}

inline float ProcessStealTailFilterSample(
    VoiceSoA& v, uint32_t slot, float input) noexcept {
    if (v.stealTailFilterEnabled[slot] == 0u) return input;
    const float a0 = v.stealTailFilterA0[slot];
    const float output = input * a0 + v.stealTailFilterZ1[slot];
    v.stealTailFilterZ1[slot] =
        input * (2.0f * a0) + v.stealTailFilterZ2[slot] -
        v.stealTailFilterB1[slot] * output;
    v.stealTailFilterZ2[slot] =
        input * a0 - v.stealTailFilterB2[slot] * output;
    return output;
}

} // namespace svms
#endif
