#ifndef SVMS_PHASE_ROTATION_H
#define SVMS_PHASE_ROTATION_H

#include "SVMSTypes.h"

#include <cmath>
#include <algorithm>
#include <cstdlib>

// ════════════════════════════════════════════════════════════════════════
// Per-voice phase rotation — the "hum removal" engine.
//
// WHY THE OLD POST-MIX ALLPASS COULD NOT WORK
//
// The black-MIDI "hum" is a steady-state Fourier component of the mixed
// signal at the note-dispatch rate (a wall of notes triggered at rhythmic
// intervals sums coherently into a periodic buzz).  Any fixed LTI filter —
// including the previous post-mix allpass cascade — preserves the MAGNITUDE
// of every Fourier component of a periodic signal and only rotates their
// phases.  The hum's magnitude therefore survived untouched and the effect
// was inaudible.
//
// HOW THIS IMPLEMENTATION WORKS
//
// Rotation is applied PER VOICE, with a pseudo-random angle θ drawn at
// note-on, in analytic-signal (Hilbert) form:
//
//     y = x·cos(θ) − x̂·sin(θ)
//
// where x̂ is the exact Hilbert transform of the sample, precomputed per
// SoundFont into a companion store (SVMSHilbertPair.h; periodic over loops).
//   * Every frequency is shifted by exactly θ and keeps its magnitude.
//   * Onset timing is SAMPLE-EXACT: a constant phase rotation, no delay.
//   * Two voices playing the SAME sample with independent angles θ₁, θ₂
//     correlate as cos(θ₁−θ₂): averaged over random angles the coherent
//     (hum) term of N simultaneous voices sums as √N instead of N.
//
// An earlier version produced x̂ with a 2-branch allpass "quadrature
// splitter". Its branches were 7°…170° apart instead of 90°, so the
// "rotation" was really a θ-dependent filter (up to ~24 dB of per-voice
// coloration, moving with the sweep). It is gone; only the exact pair
// rotates.
//
// COHERENT (mode 0) IS BIT-EXACT: the per-voice state pointer is null and
// no float operation anywhere in the render path changes.
//
// MODES
//   0 Coherent — bypass (bit-identical render path).
//   1 Analytic — per-voice random constant θ.
//   2 Sweep    — like Analytic, θ additionally rotates slowly (0.25 Hz).
//   3 Diffuse  — per-voice random 4-section unity-gain allpass cascade
//                (frequency-dependent dispersion; deliberately not analytic).
//   4 Random   — like Sweep, but every voice sweeps at its own rate
//                (0.125…0.375 Hz) and direction: deepest decorrelation.
// ════════════════════════════════════════════════════════════════════════

namespace svms {

// Sweep rate for modes 2/4 (Hz).  Slow enough to stay under perceptual
// modulation thresholds, fast enough to decorrelate long sustained walls.
inline constexpr float kPhaseRotationSweepHz = 0.25f;

inline constexpr float kPhaseRotationTwoPi = 6.28318530717958647692f;

// ── Diffuse (form 1) ─────────────────────────────────────────────────────
// Self-contained: every per-voice constant lives in the state, so render
// kernels only need the state pointer (VoiceSoA::rot).  Any other form
// passes x through.
inline float RotateVoiceSample(VoiceRotationState& st, float x) noexcept {
    if (st.form != 1u) return x;
    float t = st.a0 * x + st.z0;
    st.z0 = x - st.a0 * t;
    float y = st.a1 * t + st.z1;
    st.z1 = t - st.a1 * y;
    t = st.a2 * y + st.z2;
    st.z2 = y - st.a2 * t;
    y = st.a3 * t + st.z3;
    st.z3 = t - st.a3 * y;
    return y;
}

// ── Hilbert-pair rotation (form 2: Analytic / Sweep / Random) ────────────
// x̂ is fetched from the companion store with the IDENTICAL index math that
// produced x (lerp is LTI, so interpolating x̂ preserves the analytic pair).
// Without a companion store the voice stays unrotated: there is no
// approximate fallback.
inline float RotateVoiceSample(VoiceRotationState& st, float x,
                               const int16_t* hilbertRegion,
                               uint32_t baseOffset, uint32_t nextOffset,
                               float fraction) noexcept {
    if (st.form != 2u) return RotateVoiceSample(st, x);
    if (hilbertRegion == nullptr) return x;

    const float first =
        static_cast<float>(hilbertRegion[baseOffset]) * (1.0f / 32768.0f);
    const float xhat =
        first + (static_cast<float>(hilbertRegion[nextOffset]) *
                 (1.0f / 32768.0f) - first) * fraction;

    // Advance θ first so a static angle (dc=1, ds=0) shares the identical
    // code shape; c*1 − s*0 is exact in IEEE-754.
    const float c = st.c;
    const float s = st.s;
    st.c = c * st.dc - s * st.ds;
    st.s = s * st.dc + c * st.ds;
    return x * c - xhat * s;
}

// ── Deterministic seeding ────────────────────────────────────────────────
// The same MIDI input always produces the same angles, so offline renders
// are reproducible bit-for-bit in every rotation mode.

inline uint64_t PhaseRotationSplitMix64(uint64_t& state) noexcept {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Uniform [0, 1) from the top 24 bits.
inline float PhaseRotationUnit(uint64_t& state) noexcept {
    return static_cast<float>(PhaseRotationSplitMix64(state) >> 40) *
           (1.0f / 16777216.0f);
}

inline void SeedVoiceRotation(VoiceRotationState& st, uint32_t mode,
                              uint64_t seed, float sampleRate) noexcept {
    st.c = 1.0f;  st.s = 0.0f;
    st.dc = 1.0f; st.ds = 0.0f;
    st.z0 = st.z1 = st.z2 = st.z3 = 0.0f;
    st.a0 = st.a1 = st.a2 = st.a3 = 0.0f;
    st.form = 0u;
    st.pad = 0u;
    if (mode == 0u || mode > 4u) return;

    uint64_t rng = seed ^ 0xD1B54A32D192ED03ull;
    // Burn one draw so structurally different seeds never share low bits.
    (void)PhaseRotationSplitMix64(rng);

    const float theta = kPhaseRotationTwoPi * PhaseRotationUnit(rng);
    st.c = std::cos(theta);
    st.s = std::sin(theta);

    if (mode == 3u) {
        // Diffuse: random unity-gain cascade.  |a| < 0.93 keeps every
        // section stable with a wideband phase spread.
        st.form = 1u;
        float* const coeffs[4] = {&st.a0, &st.a1, &st.a2, &st.a3};
        for (uint32_t i = 0; i < 4u; ++i)
            *coeffs[i] = (PhaseRotationUnit(rng) * 2.0f - 1.0f) * 0.93f;
        return;
    }

    st.form = 2u;
    if (mode == 2u || mode == 4u) {
        float hz = kPhaseRotationSweepHz;
        if (mode == 4u) {
            hz *= 0.5f + PhaseRotationUnit(rng);
            if (PhaseRotationUnit(rng) < 0.5f) hz = -hz;
        }
        const float dTheta = kPhaseRotationTwoPi * hz / sampleRate;
        st.dc = std::cos(dTheta);
        st.ds = std::sin(dTheta);
    }
}

// Deterministic per-note seed material.
inline uint64_t MakeVoiceRotationSeed(uint8_t channel, uint8_t note,
                                      uint32_t handle, uint64_t birthFrame,
                                      uint64_t counter) noexcept {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(channel);
    h = (h ^ (static_cast<uint64_t>(note) << 8)) * 0x100000001B3ull;
    h ^= static_cast<uint64_t>(handle) << 16;
    h ^= birthFrame * 0xD1B54A32D192ED03ull;
    h ^= counter << 1;
    return h;
}

}  // namespace svms

#endif  // SVMS_PHASE_ROTATION_H
