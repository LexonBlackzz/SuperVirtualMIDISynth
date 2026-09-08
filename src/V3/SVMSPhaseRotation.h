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
// Rotation is applied PER VOICE, with a pseudo-random constant angle θ
// drawn at note-on.  The rotation is the analytic-signal (Hilbert) form:
//
//     y = x·cos(θ) + H{x}·sin(θ)
//
// where H{x} is produced by a 2nd-order-per-branch allpass quadrature
// splitter (a cheap continuous-time IIR Hilbert approximation).  Properties:
//
//   * Magnitude spectrum of the voice is untouched (each branch is a unity-
//     gain allpass; in quadrature, I² + Q² = |x|² at every frequency).
//   * Onset timing is SAMPLE-EXACT: the filter state starts with the voice,
//     the first output sample lands on the note-on frame.  There is no
//     delay/latency of any kind — this is NOT the old note-onset jitter hack
//     (a delay is linear phase = a time shift; this is a constant phase
//     rotation with zero time shift).
//   * Two voices playing the SAME sample with independent angles θ₁, θ₂
//     correlate as cos(θ₁−θ₂): averaged over random angles the coherent
//     (hum) term of N simultaneous voices sums as √N instead of N.
//
// COHERENT (mode 0) IS BIT-EXACT: the per-voice state pointer is null and
// no float operation anywhere in the render path changes.
//
// MODES
//   0 Coherent — bypass (bit-identical render path).
//   1 Analytic — per-voice random constant θ via quadrature splitter.
//   2 Sweep    — like Analytic, θ additionally rotates slowly (0.25 Hz).
//   3 Diffuse  — per-voice random 4-section unity-gain allpass cascade
//                (frequency-dependent dispersion; no quadrature stage).
//   4 Random   — Analytic with per-voice jittered splitter coefficients
//                plus the slow sweep: deepest decorrelation.
// ════════════════════════════════════════════════════════════════════════

namespace svms {

// Classic wideband 2nd-order-per-branch quadrature allpass pair.  Branch A
// uses a0/a1, branch B uses a2/a3.  I = (A+B)/2 and Q = (A−B)/2 are in
// approximately 90° phase from ~60 Hz to ~16 kHz at 44.1 kHz.
inline constexpr float kPhaseRotationQuadCoeffs[4] = {
    0.479400865589f, 0.876218493539f,
    -0.479400865589f, -0.876218493539f
};

// Sweep rate for modes 2/4 (Hz).  Slow enough to stay under perceptual
// modulation thresholds, fast enough to decorrelate long sustained walls.
inline constexpr float kPhaseRotationSweepHz = 0.25f;

inline constexpr float kPhaseRotationTwoPi = 6.28318530717958647692f;

// ── Hilbert-pair form (form 2) ───────────────────────────────────────────
// When the active SoundFont was loaded with an analytic pair, modes 1/2/4
// rotate with the EXACT 90° companion x̂ from the precomputed store instead
// of the allpass quadrature splitter:
//
//     y = x·cos(θ) − x̂·sin(θ)
//
// The pair is built per SoundFont sample slice at load time; lerp is LTI,
// so fetching x̂ with the identical index math that produced x preserves
// the analytic relationship.  A live mode switch without a pair-bearing
// bundle falls back to the allpass forms until the next SoundFont load.
// With the pair, VoiceRotationState only uses c/s/dc/ds; the allpass z/a
// fields stay untouched zeros.  (The reference offline synth compensates
// the rotation with a power-normalization gain g = sqrt(Σx²/(c²Σx² +
// s²Σx̂² − 2csΣx̂x)); for an exact pair Σx̂² = Σx² and Σx̂x = 0, so g ≡ 1
// and the engine omits it — form 2 carries no gain slot.)
//
// Pair construction (matches the reference offline synth): FFT-based
// analytic-signal Hilbert transform.  The i16 slice is decoded with the
// canonical ×(1/32768) scale, zero-padded to the next power of two,
// transformed, the positive-frequency bins 1..N/2−1 doubled, bin 0 and
// bin N/2 zeroed, and x̂ = Im(IFFT(Y)) quantized back to i16 with ×32768.
// This gives an exact 90° shift for every in-band frequency; the
// zero-padded circular extension only affects the slice's edge frames
// (SoundFont slices are usually silence-padded there, as in the
// reference).  Slices longer than kHilbertPairMaxSegmentFrames transform
// in independent segments of that length — the join artifact is the same
// class as the loop-wrap approximation and keeps the workspace bounded.
inline constexpr uint32_t kHilbertPairMaxSegmentFrames = 1u << 22u;

// One complex bin pair exchange of the iterative radix-2 FFT is inlined in
// HilbertTransformSegment below (double precision, fixed recurrence order,
// so the store is bit-identical regardless of thread count).

// Transform [0, frames) of one contiguous sample slice: src/dst point at
// the slice start.  Deterministic; independent of thread decomposition.
inline void HilbertTransformSegment(const int16_t* src, int16_t* dst,
                                    uint32_t frames) noexcept {
    if (frames < 2u) {
        for (uint32_t n = 0; n < frames; ++n) dst[n] = 0;
        return;
    }
    uint32_t padded = 1u;
    while (padded < frames) padded <<= 1u;

    // Workspace: one complex array, reused for forward transform and
    // inverse via conjugation.  malloc (not aligned) — load-time only.
    double* re = static_cast<double*>(
        malloc(static_cast<size_t>(padded) * sizeof(double)));
    double* im = static_cast<double*>(
        malloc(static_cast<size_t>(padded) * sizeof(double)));
    if (!re || !im) {
        free(re);
        free(im);
        // Out of workspace: leave the pair silent rather than half-built.
        for (uint32_t n = 0; n < frames; ++n) dst[n] = 0;
        return;
    }

    auto runFft = [&](double* ar, double* ai, uint32_t n, bool inverse) {
        // Bit-reversal permutation.
        for (uint32_t i = 1u, j = 0u; i < n; ++i) {
            uint32_t bit = n >> 1u;
            for (; j & bit; bit >>= 1u) j ^= bit;
            j ^= bit;
            if (i < j) {
                double t = ar[i]; ar[i] = ar[j]; ar[j] = t;
                t = ai[i]; ai[i] = ai[j]; ai[j] = t;
            }
        }
        // Butterflies.  Fixed recurrence for the twiddles (per-stage
        // repeated multiplication) keeps the accumulation order identical
        // everywhere the store is built.
        const double direction = inverse ? 1.0 : -1.0;
        for (uint32_t length = 2u; length <= n; length <<= 1u) {
            const double angle =
                direction * 6.28318530717958647692 / static_cast<double>(length);
            const double stepRe = std::cos(angle);
            const double stepIm = std::sin(angle);
            for (uint32_t i = 0u; i < n; i += length) {
                double wRe = 1.0;
                double wIm = 0.0;
                for (uint32_t k = 0u; k < length / 2u; ++k) {
                    const uint32_t a = i + k;
                    const uint32_t b = i + k + length / 2u;
                    const double tr = ar[b] * wRe - ai[b] * wIm;
                    const double ti = ar[b] * wIm + ai[b] * wRe;
                    ar[b] = ar[a] - tr;
                    ai[b] = ai[a] - ti;
                    ar[a] += tr;
                    ai[a] += ti;
                    const double nextRe = wRe * stepRe - wIm * stepIm;
                    const double nextIm = wRe * stepIm + wIm * stepRe;
                    wIm = nextIm;
                    wRe = nextRe;
                }
            }
        }
    };

    for (uint32_t n = 0u; n < frames; ++n) {
        re[n] = static_cast<double>(src[n]) * (1.0 / 32768.0);
        im[n] = 0.0;
    }
    for (uint32_t n = frames; n < padded; ++n) {
        re[n] = 0.0;
        im[n] = 0.0;
    }
    runFft(re, im, padded, false);
    // Analytic spectrum: double the positive bins, kill DC/Nyquist and all
    // negative bins, then x̂ = Im(IFFT(Y)).
    for (uint32_t k = 1u; k < padded / 2u; ++k) {
        re[k] *= 2.0;
        im[k] *= 2.0;
        re[padded - k] = 0.0;
        im[padded - k] = 0.0;
    }
    re[0] = 0.0;
    im[0] = 0.0;
    if ((padded & 1u) == 0u) {
        re[padded / 2u] = 0.0;
        im[padded / 2u] = 0.0;
    }
    runFft(re, im, padded, true);
    const double scale = 1.0 / static_cast<double>(padded);
    for (uint32_t n = 0u; n < frames; ++n) {
        const double hilbert = im[n] * scale;
        const float scaled = static_cast<float>(hilbert * 32768.0);
        int32_t quantized = static_cast<int32_t>(std::lrintf(scaled));
        if (quantized > 32767) quantized = 32767;
        if (quantized < -32768) quantized = -32768;
        dst[n] = static_cast<int16_t>(quantized);
    }
    free(re);
    free(im);
}

// Whole slice; pathological lengths process in bounded segments.
inline void HilbertTransformSlice(const int16_t* src, int16_t* dst,
                                  uint32_t frames) noexcept {
    uint32_t done = 0u;
    while (done < frames) {
        const uint32_t take = (std::min)(kHilbertPairMaxSegmentFrames,
                                         frames - done);
        HilbertTransformSegment(src + done, dst + done, take);
        done += take;
    }
}

// ── Per-sample rotation ──────────────────────────────────────────────────
// Self-contained: every per-voice constant lives in the state, so render
// kernels only need the state pointer (VoiceSoA::rot).
inline float RotateVoiceSample(VoiceRotationState& st, float x) noexcept {
    if (st.form != 0u) {
        // Diffuse: per-voice random 4-section allpass cascade.
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

    // Quadrature form (Analytic / Sweep / Random).  Advance θ first so a
    // static angle (dc=1, ds=0) shares the identical code shape.
    const float c = st.c;
    const float s = st.s;
    st.c = c * st.dc - s * st.ds;
    st.s = s * st.dc + c * st.ds;

    // Branch A
    float t = st.a0 * x + st.z0;
    st.z0 = x - st.a0 * t;
    float A = st.a1 * t + st.z1;
    st.z1 = t - st.a1 * A;
    // Branch B
    t = st.a2 * x + st.z2;
    st.z2 = x - st.a2 * t;
    float B = st.a3 * t + st.z3;
    st.z3 = t - st.a3 * B;

    const float I = 0.5f * (A + B);
    const float Q = 0.5f * (A - B);
    return I * st.c + Q * st.s;
}

// ── Hilbert-pair rotation (form 2) ───────────────────────────────────────
// xhat must have been fetched from the companion store with the IDENTICAL
// index math that produced x (lerp is LTI — interpolating x̂ preserves the
// analytic pair).  Allpass forms and a missing store delegate to the
// splitter unchanged, so a caller can always pass its region/indices.
inline float RotateVoiceSample(VoiceRotationState& st, float x,
                               const int16_t* hilbertRegion,
                               uint32_t baseOffset, uint32_t nextOffset,
                               float fraction) noexcept {
    if (st.form != 2u || hilbertRegion == nullptr)
        return RotateVoiceSample(st, x);

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
                              uint64_t seed, float sampleRate,
                              bool hilbertPairAvailable = false) noexcept {
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

    if (mode == 2u || mode == 4u) {
        const float dTheta =
            kPhaseRotationTwoPi * kPhaseRotationSweepHz / sampleRate;
        st.dc = std::cos(dTheta);
        st.ds = std::sin(dTheta);
    }

    if (mode != 3u && hilbertPairAvailable) {
        // Exact analytic pair from the SoundFont's companion store.  Same
        // angles/sweep as the allpass forms; only the quadrature source
        // differs (x̂ from the store instead of the splitter's I and Q).
        st.form = 2u;
        return;
    }

    float* const coeffs[4] = {&st.a0, &st.a1, &st.a2, &st.a3};
    switch (mode) {
        case 3u: {
            // Diffuse: random unity-gain cascade.  |a| < 0.93 keeps every
            // section stable with a wideband phase spread.
            st.form = 1u;
            for (uint32_t i = 0; i < 4u; ++i)
                *coeffs[i] = (PhaseRotationUnit(rng) * 2.0f - 1.0f) * 0.93f;
            break;
        }
        case 4u: {
            // Random: jitter the splitter coefficients per voice so the
            // phase profile differs in BOTH angle and frequency shape.
            for (uint32_t i = 0; i < 4u; ++i) {
                float a = kPhaseRotationQuadCoeffs[i] +
                          (PhaseRotationUnit(rng) * 2.0f - 1.0f) * 0.06f;
                if (a > 0.97f) a = 0.97f;
                if (a < -0.97f) a = -0.97f;
                *coeffs[i] = a;
            }
            break;
        }
        default:
            // Analytic (1) / Sweep (2): canonical splitter coefficients.
            st.a0 = kPhaseRotationQuadCoeffs[0];
            st.a1 = kPhaseRotationQuadCoeffs[1];
            st.a2 = kPhaseRotationQuadCoeffs[2];
            st.a3 = kPhaseRotationQuadCoeffs[3];
            break;
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
