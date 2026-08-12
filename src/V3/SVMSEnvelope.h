#ifndef SVMS_ENVELOPE_H
#define SVMS_ENVELOPE_H

#include "SVMSTypes.h"
#include <cmath>

namespace svms {

constexpr float kVoiceRetireThreshold = 0.00015f;
constexpr float kDefaultReleaseDecay = 0.9985f;
// Do not let a missing or ultra-short SF2 release collapse a chopped note
// into a sub-millisecond click.  V3's intended floor is 10 ms: short enough
// for dense MIDI, but long enough for the source transient to reach the mix.
constexpr float kMinReleaseSeconds = 0.010f;

inline float TimecentsToSeconds(int16_t tc) {
    // SF2's legal volume-envelope timecents range is [-12000, 8000].
    // Match TSF's near-zero pin so values at the lower limit become the
    // fast-release fallback rather than a sub-millisecond segment.
    if (tc < -11950) return 0.0f;
    const int16_t clamped = tc > 8000 ? 8000 : tc;
    return powf(2.0f, static_cast<float>(clamped) / 1200.0f);
}

inline uint32_t MakeReleaseSamples(float releaseSeconds, uint32_t sampleRate) {
    float clamped = releaseSeconds < kMinReleaseSeconds
        ? kMinReleaseSeconds : releaseSeconds;
    const float samples = clamped * static_cast<float>(sampleRate > 0 ? sampleRate : 44100u);
    if (samples <= 1.0f) return 1u;
    return static_cast<uint32_t>(samples);
}

inline float MakeReleaseDecay(float releaseSeconds, uint32_t sampleRate) {
    const uint32_t releaseSamples = MakeReleaseSamples(releaseSeconds, sampleRate);
    if (releaseSamples <= 1u) return 0.0f;
    return expf(-9.226f / static_cast<float>(releaseSamples));
}

inline float SustainAttenuationToGain(float centibels) {
    if (centibels <= 0.0f) return 1.0f;
    if (centibels >= 1000.0f) centibels = 1000.0f;
    return powf(10.0f, -centibels / 200.0f);
}

inline float InitialAttenuationToGain(float centibels) {
    if (centibels <= 0.0f) return 1.0f;
    return powf(10.0f, -centibels / 200.0f);
}

} // namespace svms

#endif
