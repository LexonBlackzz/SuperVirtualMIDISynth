#ifndef SVMS_HILBERT_PAIR_H
#define SVMS_HILBERT_PAIR_H

// ════════════════════════════════════════════════════════════════════════
// Analytic companion store x̂ for per-voice Hilbert phase rotation.
//
// Phase rotation renders y = x·cosθ − x̂·sinθ (SVMSPhaseRotation.h). That is
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

#include "SVMSSoundFont.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

// In-place iterative radix-2 FFT, unnormalized. Twiddles advance by a fixed
// recurrence so every build performs the identical operation sequence.
inline void Fft(double* re, double* im, uint32_t n, bool inverse) noexcept {
    for (uint32_t i = 1u, j = 0u; i < n; ++i) {
        uint32_t bit = n >> 1u;
        for (; j & bit; bit >>= 1u) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
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
                const uint32_t b = a + length / 2u;
                const double tr = re[b] * wRe - im[b] * wIm;
                const double ti = re[b] * wIm + im[b] * wRe;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const double nextRe = wRe * stepRe - wIm * stepIm;
                wIm = wRe * stepIm + wIm * stepRe;
                wRe = nextRe;
            }
        }
    }
}

// Linear Hilbert transform of x[0..n): out[i] = H{x}[i] for x surrounded by
// silence. Returns false when the workspace cannot be allocated.
inline bool LinearHilbert(const double* x, uint32_t n, double* out) noexcept {
    if (n < 2u) {
        for (uint32_t i = 0; i < n; ++i) out[i] = 0.0;
        return true;
    }
    const uint32_t guard = (std::min)(n, kHilbertGuardFrames);
    const uint32_t size = NextPow2(static_cast<uint64_t>(n) + guard);
    std::vector<double> re, im;
    try {
        re.assign(size, 0.0);
        im.assign(size, 0.0);
    } catch (...) {
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) re[i] = x[i];
    Fft(re.data(), im.data(), size, false);
    // Analytic spectrum: positive bins doubled, DC/Nyquist/negative zeroed;
    // x̂ = Im(IFFT).
    for (uint32_t k = 1u; k < size / 2u; ++k) {
        re[k] *= 2.0;
        im[k] *= 2.0;
        re[size - k] = 0.0;
        im[size - k] = 0.0;
    }
    re[0] = im[0] = 0.0;
    re[size / 2u] = im[size / 2u] = 0.0;
    Fft(re.data(), im.data(), size, true);
    const double scale = 1.0 / static_cast<double>(size);
    for (uint32_t i = 0; i < n; ++i) out[i] = im[i] * scale;
    return true;
}

// Periodic Hilbert transform of one period x[0..n), any n. x̂ = x ⊛ h with
// the periodic Hilbert kernel h = IDFT(−i·sgn(k)) (DC and an even-n Nyquist
// bin carry no quadrature):
//     h[m] = (2/n)·Σ_{k=1..K} sin(2πkm/n),  K = ⌊(n−1)/2⌋
//          = (2/n)·sin(Kφ/2)·sin((K+1)φ/2) / sin(φ/2),  φ = 2πm/n.
// The circular convolution runs as one zero-padded linear convolution
// (3 FFTs) folded back onto the period. Returns false on allocation failure.
inline bool PeriodicHilbert(const double* x, uint32_t n, double* out) noexcept {
    if (n < 3u) {
        for (uint32_t i = 0; i < n; ++i) out[i] = 0.0;
        return true;
    }
    const uint32_t size = NextPow2(2ull * n - 1u);
    std::vector<double> xRe, xIm, hRe, hIm;
    try {
        xRe.assign(size, 0.0);
        xIm.assign(size, 0.0);
        hRe.assign(size, 0.0);
        hIm.assign(size, 0.0);
    } catch (...) {
        return false;
    }
    const double pi = 3.14159265358979323846;
    const double k = static_cast<double>((n - 1u) / 2u);
    for (uint32_t m = 1; m < n; ++m) {
        const double half = pi * static_cast<double>(m) / static_cast<double>(n);
        hRe[m] = (2.0 / n) * std::sin(k * half) * std::sin((k + 1.0) * half) /
                 std::sin(half);
    }
    for (uint32_t i = 0; i < n; ++i) xRe[i] = x[i];
    Fft(xRe.data(), xIm.data(), size, false);
    Fft(hRe.data(), hIm.data(), size, false);
    for (uint32_t i = 0; i < size; ++i) {
        const double r = xRe[i] * hRe[i] - xIm[i] * hIm[i];
        xIm[i] = xRe[i] * hIm[i] + xIm[i] * hRe[i];
        xRe[i] = r;
    }
    Fft(xRe.data(), xIm.data(), size, true);
    const double scale = 1.0 / static_cast<double>(size);
    for (uint32_t i = 0; i < n; ++i) {
        const double folded = i + n < size ? xRe[i] + xRe[i + n] : xRe[i];
        out[i] = folded * scale;
    }
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
                         uint32_t start, uint32_t end) noexcept {
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
        if (!LinearHilbert(x.data(), take, h.data())) return false;
        for (uint32_t i = 0; i < take; ++i) companion[done + i] = Quantize(h[i]);
        done += take;
    }
    return true;
}

inline bool BuildLooped(const int16_t* store, int16_t* companion,
                        const HilbertSliceSpec& s) {
    const uint32_t loopLength = s.loopEnd - s.loopStart;
    std::vector<double> loopX(loopLength), loopH(loopLength);
    for (uint32_t i = 0; i < loopLength; ++i)
        loopX[i] = Decode(store[s.loopStart + i]);
    if (!PeriodicHilbert(loopX.data(), loopLength, loopH.data())) return false;
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
        if (!LinearHilbert(e.data(), static_cast<uint32_t>(e.size()), h.data()))
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
        if (!LinearHilbert(e.data(), static_cast<uint32_t>(e.size()), h.data()))
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
inline void BuildHilbertSlice(const int16_t* store, int16_t* companion,
                              const HilbertSliceSpec& spec) noexcept {
    bool ok = false;
    try {
        ok = spec.loopEnd != 0u
            ? hilbert_detail::BuildLooped(store, companion, spec)
            : hilbert_detail::BuildOneShot(store, companion, spec.start, spec.end);
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
    std::vector<HilbertSliceSpec> specs = PlanHilbertSlices(sf2);
    std::stable_sort(specs.begin(), specs.end(), [](const HilbertSliceSpec& a,
                                                     const HilbertSliceSpec& b) {
        return a.end - a.start > b.end - b.start;
    });
    std::atomic<size_t> next{0u};
    auto work = [&]() {
        for (size_t i; (i = next.fetch_add(1u)) < specs.size();)
            BuildHilbertSlice(store, companion, specs[i]);
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
