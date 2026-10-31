#pragma once
// 32.32 fixed-point sample cursor.
//
// A voice position is stored as two uint32 lanes (whole frame index, 32-bit
// fraction) and handled in the kernels as one uint64 (idx << 32 | frac).
// Integer adds are exact and associative, so N frames advance in a single
// step (pos + N * step) instead of N dependent float adds, and the cursor
// never drifts at large sample offsets the way a float cursor does.
//
// The per-span step is derived once from the float phaseIncs (bend and
// vibrato keep writing floats), so only the cursor itself is fixed point.

#include <cmath>
#include <cstdint>

namespace svms {
namespace phasefx {

constexpr double kOne = 4294967296.0;  // 2^32

inline uint64_t Pack(uint32_t idx, uint32_t frac) {
    return (static_cast<uint64_t>(idx) << 32) | frac;
}
inline uint32_t Index(uint64_t pos) { return static_cast<uint32_t>(pos >> 32); }
inline uint32_t Frac(uint64_t pos) { return static_cast<uint32_t>(pos); }

// Float -> cursor.  Negative values clamp to 0 (the float cursor did the same).
inline uint64_t FromFloat(float value) {
    return value <= 0.0f ? 0u
                         : static_cast<uint64_t>(std::llround(
                               static_cast<double>(value) * kOne));
}

// Cursor -> float (telemetry, tests, GPU upload).
inline float ToFloat(uint64_t pos) {
    return static_cast<float>(static_cast<double>(pos) * (1.0 / kOne));
}

// Interpolation weight: the top 24 fraction bits, exactly representable as a
// float, so scalar and AVX2 compute the identical weight.
inline float Weight(uint32_t frac) {
    return static_cast<float>(frac >> 8) * (1.0f / 16777216.0f);
}

// Wrap a cursor that ran past loopEnd back into [loopStart, loopEnd), carrying
// the overshoot (any number of whole loops).  Requires loopStart < loopEnd.
inline uint64_t Wrap(uint64_t pos, uint64_t loopStart, uint64_t loopEnd) {
    if (pos < loopEnd) return pos;
    uint64_t over = pos - loopEnd;
    const uint64_t length = loopEnd - loopStart;
    if (length == 0u) return loopStart;
    if (over >= length) over %= length;
    return loopStart + over;
}

// Cursor of voice h (any struct with phaseIdx/phaseFrac arrays).
template <class V>
inline uint64_t Load(const V& v, uint32_t h) {
    return Pack(v.phaseIdx[h], v.phaseFrac[h]);
}
template <class V>
inline void Store(V& v, uint32_t h, uint64_t pos) {
    v.phaseIdx[h] = Index(pos);
    v.phaseFrac[h] = Frac(pos);
}
template <class V>
inline float LoadFloat(const V& v, uint32_t h) { return ToFloat(Load(v, h)); }
template <class V>
inline void StoreFloat(V& v, uint32_t h, float value) {
    Store(v, h, FromFloat(value));
}

}  // namespace phasefx
}  // namespace svms
