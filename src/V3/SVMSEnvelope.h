#ifndef SVMS_ENVELOPE_H
#define SVMS_ENVELOPE_H

#include "SVMSTypes.h"

namespace svms {

constexpr float kVoiceRetireThreshold = 0.00015f;
constexpr float kDefaultReleaseDecay = 0.9985f;

inline float TimecentsToSeconds(int16_t tc) {
    if (tc <= -12000) return 0.0f;
    return powf(2.0f, (float)tc / 1200.0f);
}

inline float MakeReleaseDecay(float releaseSeconds, uint32_t sampleRate) {
    float clamped = releaseSeconds <= 0.0005f ? 0.0005f : releaseSeconds;
    float releaseSamples = clamped * (float)(sampleRate > 0 ? sampleRate : 44100u);
    if (releaseSamples <= 1.0f) return 0.0f;
    return expf(-9.226f / releaseSamples);
}

} // namespace svms

#endif
