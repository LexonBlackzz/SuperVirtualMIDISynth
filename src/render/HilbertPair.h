#ifndef SVMS_HILBERT_PAIR_H
#define SVMS_HILBERT_PAIR_H

// ════════════════════════════════════════════════════════════════════════
// Analytic companion store x̂ for per-voice Hilbert phase rotation.
//
// Phase rotation renders y = x·cosθ − x̂·sinθ (PhaseRotation.h). That is
// a pure phase shift with unchanged magnitude ONLY when x̂ is the Hilbert
// transform of the signal the voice actually plays. A sample is not played
// as one isolated slice:
//
//   * One-shot: silence, the slice, silence. x̂ is the LINEAR Hilbert
//     transform of the slice; the FFT is zero-padded with a guard band so
//     its circular wrap does not fold the slice's end into its start.
//   * Looped: attack, then the loop repeated, then (loop mode 3 after
//     release) the tail. Over the loop, x̂ is the PERIODIC Hilbert transform
//     of exactly one loop period, so x̂ wraps seamlessly with x. A transform
//     of the whole slice is not periodic over the loop: x̂ then steps at
//     every wrap and the rotated voice clicks at the loop rate (buzz).
//     Attack and tail are transformed with the loop repeated next to them
//     and crossfaded into the periodic loop companion at the loop edges.
//
// Loop points are per region; a sample's companion uses the loop that most
// of its looping regions play (all of them, in practically every SoundFont).
//
// Load time only, double precision, fixed operation order: the store is
// bit-identical however the slices are distributed across threads.
// ════════════════════════════════════════════════════════════════════════

#include "synth/SoundFont.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace svms {

// One sample slice of the store. loopEnd == 0: played as a one-shot.
struct HilbertSliceSpec {
    uint32_t start;
    uint32_t end;
    uint32_t loopStart;
    uint32_t loopEnd;
};

// One-shot slices longer than this transform in independent segments so the
// FFT workspace stays bounded (2^21 frames ≈ 47 s at 44.1 kHz).
inline constexpr uint32_t kHilbertMaxSegmentFrames = 1u << 21u;
// Zero guard appended before the FFT of a non-periodic signal, so the circular
// wrap cannot fold the end of the signal onto its onset.
inline constexpr uint32_t kHilbertGuardFrames = 1u << 13u;
// Loop repetitions placed next to an attack or tail when transforming it.
inline constexpr uint32_t kHilbertLoopContextFrames = 1u << 13u;
// Crossfade from the attack/tail companion into the periodic loop one.
inline constexpr uint32_t kHilbertLoopCrossfadeFrames = 1024u;

namespace hilbert_detail {

inline uint32_t NextPow2(uint64_t value) noexcept {
    uint64_t n = 1u;
    while (n < value) n <<= 1u;
    return static_cast<uint32_t>(n);
}

// Twiddles for a complex FFT of `n` points: stage `length` (2, 4, .., n)
// reads w[length/2 + k] = e^(-2πik/length), k < length/2, contiguously. Each
// entry comes straight from cos/sin (no recurrence), so the table is exact to
// double rounding and the butterflies stay sequential in memory.
struct TwiddleTable {
    std::vector<double> re, im;
    explicit TwiddleTable(uint32_t n) : re(n ? n : 1u), im(n ? n : 1u) {
        for (uint32_t length = 2u; length <= n; length <<= 1u) {
            const uint32_t half = length >> 1u;
            for (uint32_t k = 0u; k < half; ++k) {
                const double angle = -6.28318530717958647692 *
                                     static_cast<double>(k) /
                                     static_cast<double>(length);
                re[half + k] = std::cos(angle);
                im[half + k] = std::sin(angle);
            }
        }
    }
};

// Tables are shared by every worker building slices of the same size and
// dropped when the build ends (a 2^21-point table is 32 MB).
inline std::mutex& TwiddleMutex() {
    static std::mutex m;
    return m;
}
inline std::map<uint32_t, std::shared_ptr<const TwiddleTable>>& TwiddleCache() {
    static std::map<uint32_t, std::shared_ptr<const TwiddleTable>> cache;
    return cache;
}
inline std::shared_ptr<const TwiddleTable> Twiddles(uint32_t n) {
    {
        std::lock_guard<std::mutex> lock(TwiddleMutex());
        const auto it = TwiddleCache().find(n);
        if (it != TwiddleCache().end()) return it->second;
    }
    auto table = std::make_shared<const TwiddleTable>(n);  // built unlocked
    std::lock_guard<std::mutex> lock(TwiddleMutex());
    return TwiddleCache().emplace(n, table).first->second;
}
inline void ClearTwiddleCache() {
    std::lock_guard<std::mutex> lock(TwiddleMutex());
    TwiddleCache().clear();
}

// In-place iterative radix-2 complex FFT, unnormalized, n a power of two.
// Deterministic: the operation sequence depends only on n.
inline void Fft(double* re, double* im, uint32_t n, bool inverse,
                const TwiddleTable& tw) noexcept {
    for (uint32_t i = 1u, j = 0u; i < n; ++i) {
        uint32_t bit = n >> 1u;
        for (; j & bit; bit >>= 1u) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    const double sign = inverse ? -1.0 : 1.0;  // table holds the forward sign
    // Stages up to kBlock points run block by block so the working set stays
    // in cache (a 2^21-point transform otherwise streams 32 MB per stage). The
    // butterflies and their order within each dependency chain are unchanged,
    // so the result is bit-identical to running stage by stage.
    constexpr uint32_t kBlock = 1u << 13u;
    const uint32_t block = n < kBlock ? n : kBlock;
    const auto stage = [&](uint32_t from, uint32_t to, uint32_t length) {
        const uint32_t half = length >> 1u;
        const double* const wr = tw.re.data() + half;
        const double* const wi = tw.im.data() + half;
        for (uint32_t i = from; i < to; i += length) {
            double* const ar = re + i;
            double* const ai = im + i;
            double* const br = ar + half;
            double* const bi = ai + half;
            for (uint32_t k = 0u; k < half; ++k) {
                const double wRe = wr[k];
                const double wIm = sign * wi[k];
                const double tr = br[k] * wRe - bi[k] * wIm;
                const double ti = br[k] * wIm + bi[k] * wRe;
                br[k] = ar[k] - tr;
                bi[k] = ai[k] - ti;
                ar[k] += tr;
                ai[k] += ti;
            }
        }
    };
    for (uint32_t base = 0u; base < n; base += block)
        for (uint32_t length = 2u; length <= block; length <<= 1u)
            stage(base, base + block, length);
    for (uint32_t length = block << 1u; length <= n && length != 0u; length <<= 1u)
        stage(0u, n, length);
}

// Reusable FFT workspace. One per worker: a long slice's transforms need
// tens of MB, and reallocating (and zeroing) that for every slice of a
// 1000-sample bank costs more than it should. Buffers only grow.
struct Scratch {
    std::vector<double> xr, xi, hr, hi;
};
inline void Grow(std::vector<double>& v, size_t n) {
    if (v.size() < n) v.resize(n);
}

// Real signals of N points (N a power of two >= 4) go through one complex FFT
// of M = N/2 points: z[i] = x[2i] + i·x[2i+1]. The spectrum of x then comes
// out of bins k and M-k together, so everything below works on the pair
// (k, j = M-k) in place and the workspace is just M complex values.
struct Cx { double re, im; };

// Z[0..M) = FFT of the packed signal  ->  X[k], X[j] of the real signal.
inline void SplitPair(const double* zr, const double* zi, uint32_t k,
                      uint32_t j, const TwiddleTable& tw, uint32_t m,
                      Cx& xk, Cx& xj) noexcept {
    const double ar = zr[k], ai = zi[k], br = zr[j], bi = zi[j];
    // E = (a + conj b)/2, O = (a - conj b)/(2i);  X[k] = E + e^(-2πik/N)·O.
    const double er = 0.5 * (ar + br), ei = 0.5 * (ai - bi);
    const double orr = 0.5 * (ai + bi), oi = -0.5 * (ar - br);
    const double wkr = tw.re[m + k], wki = tw.im[m + k];
    xk = {er + wkr * orr - wki * oi, ei + wkr * oi + wki * orr};
    // The same for bin j with a and b swapped.
    const double er2 = er, ei2 = -ei;                // E[j] = conj(E[k])
    const double or2 = orr, oi2 = oi;                // O[j] = (bi+ai, ar-br)/2·…
    // O[j] = (b - conj a)/(2i) = ((bi + ai)/2, -(br - ar)/2)
    const double ojr = or2, oji = -oi2;
    const double wjr = tw.re[m + j], wji = tw.im[m + j];
    xj = {er2 + wjr * ojr - wji * oji, ei2 + wjr * oji + wji * ojr};
}

// Y[k], Y[j] (half-spectrum of a real signal) -> the packed values the
// inverse M-point FFT needs at k and j.
inline void JoinPair(Cx yk, Cx yj, uint32_t k, uint32_t j,
                     const TwiddleTable& tw, uint32_t m, Cx& zk,
                     Cx& zj) noexcept {
    {
        const double pr = yj.re, pi = -yj.im;          // conj(Y[M-k])
        const double ar = yk.re + pr, ai = yk.im + pi;
        const double dr = yk.re - pr, di = yk.im - pi;
        const double cr = tw.re[m + k], ci = tw.im[m + k];
        const double br = dr * cr + di * ci;           // D·e^(+2πik/N)
        const double bi = di * cr - dr * ci;
        zk = {ar - bi, ai + br};
    }
    {
        const double pr = yk.re, pi = -yk.im;          // conj(Y[M-j]) = conj(Y[k])
        const double ar = yj.re + pr, ai = yj.im + pi;
        const double dr = yj.re - pr, di = yj.im - pi;
        const double cr = tw.re[m + j], ci = tw.im[m + j];
        const double br = dr * cr + di * ci;
        const double bi = di * cr - dr * ci;
        zj = {ar - bi, ai + br};
    }
}

inline void Pack(const double* x, uint32_t nx, uint32_t m, double* zr,
                 double* zi) noexcept {
    const uint32_t full = (std::min)(nx / 2u, m);
    for (uint32_t i = 0u; i < full; ++i) {
        zr[i] = x[2u * i];
        zi[i] = x[2u * i + 1u];
    }
    uint32_t i = full;
    if (i < m && 2u * i < nx) {
        zr[i] = x[2u * i];
        zi[i] = 0.0;
        ++i;
    }
    for (; i < m; ++i) zr[i] = zi[i] = 0.0;
}

inline void Unpack(const double* zr, const double* zi, uint32_t m, double scale,
                   uint32_t n, double* out) noexcept {
    for (uint32_t i = 0u; 2u * i < n; ++i) {
        out[2u * i] = zr[i] * scale;
        if (2u * i + 1u < n) out[2u * i + 1u] = zi[i] * scale;
    }
    (void)m;
}

// Linear Hilbert transform of x[0..n): out[i] = H{x}[i] for x surrounded by
// silence. Returns false when the workspace cannot be allocated.
inline bool LinearHilbert(const double* x, uint32_t n, double* out,
                          Scratch& sc) noexcept {
    if (n < 2u) {
        for (uint32_t i = 0; i < n; ++i) out[i] = 0.0;
        return true;
    }
    const uint32_t guard = (std::min)(n, kHilbertGuardFrames);
    const uint32_t size = (std::max)(4u, NextPow2(static_cast<uint64_t>(n) + guard));
    const uint32_t m = size / 2u;
    std::shared_ptr<const TwiddleTable> table;
    try {
        Grow(sc.xr, m);
        Grow(sc.xi, m);
        table = Twiddles(size);
    } catch (...) {
        return false;
    }
    double* const zr = sc.xr.data();
    double* const zi = sc.xi.data();
    Pack(x, n, m, zr, zi);
    Fft(zr, zi, m, false, *table);
    // x̂ spectrum: -i·X on bins 1..M-1; DC and Nyquist carry no quadrature.
    zr[0] = zi[0] = 0.0;
    for (uint32_t k = 1u; k <= m / 2u; ++k) {
        const uint32_t j = m - k;
        Cx xk, xj;
        SplitPair(zr, zi, k, j, *table, m, xk, xj);
        const Cx yk{xk.im, -xk.re}, yj{xj.im, -xj.re};   // -i·X
        Cx zk, zj;
        JoinPair(yk, yj, k, j, *table, m, zk, zj);
        zr[k] = zk.re; zi[k] = zk.im;
        zr[j] = zj.re; zi[j] = zj.im;
    }
    Fft(zr, zi, m, true, *table);
    Unpack(zr, zi, m, 1.0 / static_cast<double>(size), n, out);
    return true;
}

// Periodic Hilbert transform of one period x[0..n), any n. x̂ = x ⊛ h with
// the periodic Hilbert kernel h = IDFT(−i·sgn(k)) (DC and an even-n Nyquist
// bin carry no quadrature):
//     h[m] = (2/n)·Σ_{k=1..K} sin(2πkm/n),  K = ⌊(n−1)/2⌋
//          = (2/n)·sin(Kφ/2)·sin((K+1)φ/2) / sin(φ/2),  φ = 2πm/n.
// The circular convolution runs as one zero-padded linear convolution
// (3 real FFTs) folded back onto the period. Returns false on allocation
// failure.
inline bool PeriodicHilbert(const double* x, uint32_t n, double* out,
                            Scratch& sc) noexcept {
    if (n < 3u) {
        for (uint32_t i = 0; i < n; ++i) out[i] = 0.0;
        return true;
    }
    const uint32_t size = (std::max)(4u, NextPow2(2ull * n - 1u));
    const uint32_t m = size / 2u;
    std::shared_ptr<const TwiddleTable> table;
    std::vector<double> kernel, folded;
    try {
        Grow(sc.xr, m);
        Grow(sc.xi, m);
        Grow(sc.hr, m);
        Grow(sc.hi, m);
        kernel.assign(n, 0.0);
        folded.resize(size);
        table = Twiddles(size);
    } catch (...) {
        return false;
    }
    const double pi = 3.14159265358979323846;
    const double kk = static_cast<double>((n - 1u) / 2u);
    for (uint32_t i = 1; i < n; ++i) {
        const double halfAngle = pi * static_cast<double>(i) / static_cast<double>(n);
        kernel[i] = (2.0 / n) * std::sin(kk * halfAngle) *
                    std::sin((kk + 1.0) * halfAngle) / std::sin(halfAngle);
    }
    double* const xr = sc.xr.data();
    double* const xi = sc.xi.data();
    double* const hr = sc.hr.data();
    double* const hi = sc.hi.data();
    Pack(x, n, m, xr, xi);
    Pack(kernel.data(), n, m, hr, hi);
    Fft(xr, xi, m, false, *table);
    Fft(hr, hi, m, false, *table);
    {   // bins 0 and M (real): X[0] = Re+Im of Z[0], X[M] = Re-Im.
        const double x0 = xr[0] + xi[0], xm = xr[0] - xi[0];
        const double h0 = hr[0] + hi[0], hm = hr[0] - hi[0];
        const double y0 = x0 * h0, ym = xm * hm;
        // Join with Y[M-0] = Y[M]: A = Y0 + Ym, B = (Y0 - Ym)·1.
        xr[0] = (y0 + ym) - (y0 - ym) * 0.0;
        xi[0] = 0.0 + (y0 - ym);
    }
    for (uint32_t k = 1u; k <= m / 2u; ++k) {
        const uint32_t j = m - k;
        Cx xk, xj, hk, hj;
        SplitPair(xr, xi, k, j, *table, m, xk, xj);
        SplitPair(hr, hi, k, j, *table, m, hk, hj);
        const Cx yk{xk.re * hk.re - xk.im * hk.im, xk.re * hk.im + xk.im * hk.re};
        const Cx yj{xj.re * hj.re - xj.im * hj.im, xj.re * hj.im + xj.im * hj.re};
        Cx zk, zj;
        JoinPair(yk, yj, k, j, *table, m, zk, zj);
        xr[k] = zk.re; xi[k] = zk.im;
        xr[j] = zj.re; xi[j] = zj.im;
    }
    Fft(xr, xi, m, true, *table);
    Unpack(xr, xi, m, 1.0 / static_cast<double>(size), size, folded.data());
    for (uint32_t i = 0; i < n; ++i)
        out[i] = i + n < size ? folded[i] + folded[i + n] : folded[i];
    return true;
}

inline int16_t Quantize(double value) noexcept {
    const float scaled = static_cast<float>(value * 32768.0);
    int32_t q = static_cast<int32_t>(std::lrintf(scaled));
    if (q > 32767) q = 32767;
    if (q < -32768) q = -32768;
    return static_cast<int16_t>(q);
}

inline double Decode(int16_t value) noexcept {
    return static_cast<double>(value) * (1.0 / 32768.0);
}

// Raised-cosine weight in (0, 1]: 0 → `from`, 1 → `to`.
inline double Crossfade(double from, double to, double weight) noexcept {
    return from + (to - from) * weight;
}

inline bool BuildOneShot(const int16_t* store, int16_t* companion,
                         uint32_t start, uint32_t end, Scratch& sc) noexcept {
    std::vector<double> x, h;
    for (uint32_t done = start; done < end;) {
        const uint32_t take = (std::min)(kHilbertMaxSegmentFrames, end - done);
        try {
            x.resize(take);
            h.resize(take);
        } catch (...) {
            return false;
        }
        for (uint32_t i = 0; i < take; ++i) x[i] = Decode(store[done + i]);
        if (!LinearHilbert(x.data(), take, h.data(), sc)) return false;
        for (uint32_t i = 0; i < take; ++i) companion[done + i] = Quantize(h[i]);
        done += take;
    }
    return true;
}

inline bool BuildLooped(const int16_t* store, int16_t* companion,
                        const HilbertSliceSpec& s, Scratch& sc) {
    const uint32_t loopLength = s.loopEnd - s.loopStart;
    std::vector<double> loopX(loopLength), loopH(loopLength);
    for (uint32_t i = 0; i < loopLength; ++i)
        loopX[i] = Decode(store[s.loopStart + i]);
    if (!PeriodicHilbert(loopX.data(), loopLength, loopH.data(), sc)) return false;
    for (uint32_t i = 0; i < loopLength; ++i)
        companion[s.loopStart + i] = Quantize(loopH[i]);
    // Periodic loop companion at loop-relative offset r (any sign).
    auto periodic = [&](int64_t r) {
        int64_t m = r % static_cast<int64_t>(loopLength);
        if (m < 0) m += loopLength;
        return loopH[static_cast<size_t>(m)];
    };

    // Attack: the attack followed by the repeating loop.
    const uint32_t attack = s.loopStart - s.start;
    if (attack != 0u) {
        const uint32_t context = kHilbertLoopContextFrames;
        std::vector<double> e(static_cast<size_t>(attack) + context), h(e.size());
        for (uint32_t i = 0; i < attack; ++i) e[i] = Decode(store[s.start + i]);
        for (uint32_t i = 0; i < context; ++i) e[attack + i] = loopX[i % loopLength];
        if (!LinearHilbert(e.data(), static_cast<uint32_t>(e.size()), h.data(), sc))
            return false;
        const uint32_t fade = (std::min)(attack, kHilbertLoopCrossfadeFrames);
        for (uint32_t i = 0; i < attack; ++i) {
            double value = h[i];
            const uint32_t intoFade = i + fade;   // ≥ attack inside the fade
            if (intoFade >= attack) {
                const double t = static_cast<double>(intoFade - attack + 1u) /
                                 static_cast<double>(fade + 1u);
                const double w = 0.5 - 0.5 * std::cos(3.14159265358979323846 * t);
                value = Crossfade(value, periodic(static_cast<int64_t>(i) - attack), w);
            }
            companion[s.start + i] = Quantize(value);
        }
    }

    // Tail (loop mode 3 after release): the repeating loop, then the tail.
    const uint32_t tail = s.end - s.loopEnd;
    if (tail != 0u) {
        uint32_t context = kHilbertLoopContextFrames;
        context += (loopLength - context % loopLength) % loopLength;  // whole loops
        std::vector<double> e(static_cast<size_t>(context) + tail), h(e.size());
        for (uint32_t i = 0; i < context; ++i) e[i] = loopX[i % loopLength];
        for (uint32_t i = 0; i < tail; ++i) e[context + i] = Decode(store[s.loopEnd + i]);
        if (!LinearHilbert(e.data(), static_cast<uint32_t>(e.size()), h.data(), sc))
            return false;
        const uint32_t fade = (std::min)(tail, kHilbertLoopCrossfadeFrames);
        for (uint32_t i = 0; i < tail; ++i) {
            double value = h[context + i];
            if (i < fade) {
                const double t = static_cast<double>(fade - i) /
                                 static_cast<double>(fade + 1u);
                const double w = 0.5 - 0.5 * std::cos(3.14159265358979323846 * t);
                value = Crossfade(value, periodic(static_cast<int64_t>(loopLength) + i), w);
            }
            companion[s.loopEnd + i] = Quantize(value);
        }
    }
    return true;
}

}  // namespace hilbert_detail

// Writes companion[spec.start, spec.end). On allocation failure the slice is
// left silent (rotation of those voices then degenerates to x·cosθ).
// `scratch` (optional) is a worker's reusable workspace; pass the same one
// for every slice that worker builds.
inline void BuildHilbertSlice(const int16_t* store, int16_t* companion,
                              const HilbertSliceSpec& spec,
                              hilbert_detail::Scratch* scratch = nullptr) noexcept {
    bool ok = false;
    try {
        hilbert_detail::Scratch local;
        hilbert_detail::Scratch& sc = scratch ? *scratch : local;
        ok = spec.loopEnd != 0u
            ? hilbert_detail::BuildLooped(store, companion, spec, sc)
            : hilbert_detail::BuildOneShot(store, companion, spec.start, spec.end, sc);
    } catch (...) {
        ok = false;
    }
    if (!ok) {
        for (uint32_t i = spec.start; i < spec.end; ++i) companion[i] = 0;
    }
}

// One spec per distinct sample slice, with the loop its looping regions play.
inline std::vector<HilbertSliceSpec> PlanHilbertSlices(const SF2Data& sf2) {
    struct Use { uint32_t sample, loopStart, loopEnd; };
    std::vector<Use> uses;
    for (uint32_t r = 0; r < sf2.regionCount; ++r) {
        const SFSampleRegion& region = sf2.regions[r];
        if (region.loopMode == 0u || region.sampleIndex >= sf2.sampleCount) continue;
        uses.push_back({region.sampleIndex,
                        static_cast<uint32_t>(region.loopStartOffset),
                        static_cast<uint32_t>(region.loopEndOffset)});
    }
    std::sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) {
        if (a.sample != b.sample) return a.sample < b.sample;
        if (a.loopStart != b.loopStart) return a.loopStart < b.loopStart;
        return a.loopEnd < b.loopEnd;
    });

    std::vector<HilbertSliceSpec> specs;
    size_t u = 0;
    for (uint32_t i = 0; i < sf2.sampleCount; ++i) {
        const SF2Sample& sample = sf2.samples[i];
        HilbertSliceSpec spec{sample.start, sample.end, 0u, 0u};
        uint32_t bestCount = 0u;
        while (u < uses.size() && uses[u].sample < i) ++u;
        while (u < uses.size() && uses[u].sample == i) {
            size_t run = u;
            while (run < uses.size() && uses[run].sample == i &&
                   uses[run].loopStart == uses[u].loopStart &&
                   uses[run].loopEnd == uses[u].loopEnd) ++run;
            const uint32_t count = static_cast<uint32_t>(run - u);
            if (count > bestCount && uses[u].loopStart >= sample.start &&
                uses[u].loopEnd <= sample.end &&
                uses[u].loopEnd > uses[u].loopStart + 1u) {
                bestCount = count;
                spec.loopStart = uses[u].loopStart;
                spec.loopEnd = uses[u].loopEnd;
            }
            u = run;
        }
        if (spec.end > spec.start && spec.end <= sf2.sampleDataFrames)
            specs.push_back(spec);
    }
    // ponytail: slices that overlap without being identical are built
    // independently (last writer wins); dedupe only exact duplicates.
    std::stable_sort(specs.begin(), specs.end(), [](const HilbertSliceSpec& a,
                                              const HilbertSliceSpec& b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    specs.erase(std::unique(specs.begin(), specs.end(),
                            [](const HilbertSliceSpec& a, const HilbertSliceSpec& b) {
                                return a.start == b.start && a.end == b.end;
                            }),
                specs.end());
    return specs;
}

// Builds the whole companion store (store-sized, zero-initialized). Slices
// are independent; threads > 1 fans them out over std::thread, largest first
// (the XP driver keeps its own CreateThread pool and calls BuildHilbertSlice).
inline void BuildHilbertCompanion(const SF2Data& sf2, const int16_t* store,
                                  int16_t* companion, uint32_t threads = 1u) {
    struct DropTables { ~DropTables() { hilbert_detail::ClearTwiddleCache(); } } dropTables;
    std::vector<HilbertSliceSpec> specs = PlanHilbertSlices(sf2);
    std::stable_sort(specs.begin(), specs.end(), [](const HilbertSliceSpec& a,
                                                     const HilbertSliceSpec& b) {
        return a.end - a.start > b.end - b.start;
    });
    std::atomic<size_t> next{0u};
    auto work = [&]() {
        hilbert_detail::Scratch scratch;
        for (size_t i; (i = next.fetch_add(1u)) < specs.size();)
            BuildHilbertSlice(store, companion, specs[i], &scratch);
    };
    std::vector<std::thread> pool;
    for (uint32_t t = 1u; t < threads && t < specs.size(); ++t) {
        try {
            pool.emplace_back(work);
        } catch (...) {
            break;
        }
    }
    work();
    for (std::thread& thread : pool) thread.join();
}

}  // namespace svms

#endif  // SVMS_HILBERT_PAIR_H
