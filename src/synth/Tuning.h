#ifndef SVMS_TUNING_H
#define SVMS_TUNING_H
#include <cstdint>
#include <cmath>
namespace svms {
// Key zero keeps MIDI C-1. In 31EDO an octave is 31 keys (155 = C4).
// Drum keys name instruments and must never be remapped as pitches.
inline float MidiKeyPitch(uint8_t key, bool edo31) noexcept {
    return edo31 ? float(key) * 12.0f / 31.0f : float(key);
}
inline uint8_t MidiRegionKey(uint8_t key, bool edo31) noexcept {
    return edo31 ? static_cast<uint8_t>(MidiKeyPitch(key, true) + 0.5f) : key;
}
inline float RetunePhaseStep(float base, uint8_t key, float scale, bool edo31) {
    return edo31 ? base * powf(2.0f, (MidiKeyPitch(key, true) - key) * scale / 12.0f) : base;
}
}
#endif
