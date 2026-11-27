#pragma once

// Partitioned convolution for the release bus: one real input (a bus's train of note impulses) against a stereo
// kernel, with no latency. A block's input is complete before the block renders, so the first partitions are one
// block long and a block's own input reaches its own output.
//
// The kernel is cut into levels. Level 0 has partitions of one block and covers the first 8 blocks; level k has
// partitions of 4^k blocks and starts 2 * 4^k blocks into the kernel, so each level's input chunk is complete one
// whole partition before its output is due. That partition's worth of blocks is spent on its work (forward FFT,
// spectrum products, inverse FFT), cut into steps and spread evenly, so no block carries a level's FFT alone.
//
// The left and right kernels are packed as one complex kernel (left + i * right). The input is real, so one complex
// product and one inverse FFT give both sides: the real part is the left output, the imaginary part the right.
//
// A kernel (ConvKernel) is immutable and shared by every history that uses it; a history (ConvHistory) holds one
// input's state. Nothing allocates after ConvHistory::Prepare.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#if (defined(_M_X64) || defined(__x86_64__)) && !defined(SVMS_XP_COMPAT)
#define SVMS_BUS_AVX 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define SVMS_BUS_AVX_FN
#else
#include <cpuid.h>
#define SVMS_BUS_AVX_FN __attribute__((target("avx")))
#endif
#endif

namespace svms {
namespace bus {

using Cf = std::complex<float>;

// AVX paths for the complex products and butterflies. They do the same IEEE operations as the scalar loops (no
// FMA), so both give the same bits. Tests set busScalarOnly to compare.
inline bool busScalarOnly = false;
inline bool BusAvx() noexcept {
#if defined(SVMS_BUS_AVX)
    if (busScalarOnly) return false;
    static const bool has = [] {
        unsigned int ecx = 0;
#if defined(_MSC_VER)
        int r[4];
        __cpuid(r, 1);
        ecx = static_cast<unsigned int>(r[2]);
#else
        unsigned int a = 0, b = 0, d = 0;
        if (!__get_cpuid(1, &a, &b, &ecx, &d)) return false;
#endif
        if ((ecx & (1u << 27)) == 0 || (ecx & (1u << 28)) == 0) return false;   // OSXSAVE, AVX
#if defined(_MSC_VER)
        return (_xgetbv(0) & 6u) == 6u;
#else
        unsigned int lo = 0, hi = 0;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        return (lo & 6u) == 6u;
#endif
    }();
    return has;
#else
    return false;
#endif
}

// acc (=, or += when `add`) x * h, element-wise over `count` complex values (interleaved floats).
inline void MulScalar(float* acc, const float* x, const float* h, int count, bool add) noexcept {
    if (add) {
        for (int i = 0; i < count; ++i) {
            acc[2 * i] += x[2 * i] * h[2 * i] - x[2 * i + 1] * h[2 * i + 1];
            acc[2 * i + 1] += x[2 * i] * h[2 * i + 1] + x[2 * i + 1] * h[2 * i];
        }
    } else {
        for (int i = 0; i < count; ++i) {
            acc[2 * i] = x[2 * i] * h[2 * i] - x[2 * i + 1] * h[2 * i + 1];
            acc[2 * i + 1] = x[2 * i] * h[2 * i + 1] + x[2 * i + 1] * h[2 * i];
        }
    }
}

#if defined(SVMS_BUS_AVX)
// x * h for four complex values: (xr hr - xi hi, xi hr + xr hi).
SVMS_BUS_AVX_FN inline __m256 MulAvx4(__m256 x, __m256 h) noexcept {
    return _mm256_addsub_ps(_mm256_mul_ps(x, _mm256_moveldup_ps(h)),
                            _mm256_mul_ps(_mm256_permute_ps(x, 0xB1), _mm256_movehdup_ps(h)));
}
SVMS_BUS_AVX_FN inline void MulAvx(float* acc, const float* x, const float* h, int count, bool add) noexcept {
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        __m256 p = MulAvx4(_mm256_loadu_ps(x + 2 * i), _mm256_loadu_ps(h + 2 * i));
        if (add) p = _mm256_add_ps(_mm256_loadu_ps(acc + 2 * i), p);
        _mm256_storeu_ps(acc + 2 * i, p);
    }
    MulScalar(acc + 2 * i, x + 2 * i, h + 2 * i, count - i, add);
}
#endif

inline void Mul(float* acc, const float* x, const float* h, int count, bool add) noexcept {
#if defined(SVMS_BUS_AVX)
    if (BusAvx()) return MulAvx(acc, x, h, count, add);
#endif
    MulScalar(acc, x, h, count, add);
}

// Radix-2 complex FFT, unnormalized, whose passes can run one at a time. Spectra are kept in bit-reversed bin
// order: the forward transform (decimation in frequency) takes natural input and leaves bin rev(p) at position p,
// the inverse (decimation in time) takes that order back to natural output. Everything between them is
// element-wise, so no permutation is ever done. Pass 0 is empty (it keeps the pass count of a permuting FFT);
// passes 1..Bits are the butterflies, forward from span n down to 2, inverse from 2 up to n.
//
// In that order the bins of a real signal's partners (bin -k for bin k) are mirrored within each octave of
// positions: Partner(p) = 3 * 2^m - 1 - p for p in [2^m, 2^(m+1)), and positions 0 and 1 are their own.
class SplitFft {
public:
    explicit SplitFft(int n) : n_(n) {
        while ((1 << bits_) < n_) ++bits_;
        tw_.resize(static_cast<size_t>((std::max)(1, n_ - 1)));   // span 2h: h twiddles at offset h - 1
        for (int s = 1; s <= bits_; ++s) {
            const int half = 1 << (s - 1);
            for (int k = 0; k < half; ++k)
                tw_[static_cast<size_t>(half - 1 + k)] =
                    Cf(static_cast<float>(std::cos(-6.283185307179586 * k / (2 * half))),
                       static_cast<float>(std::sin(-6.283185307179586 * k / (2 * half))));
        }
    }
    int Size() const noexcept { return n_; }
    int Bits() const noexcept { return bits_; }

    void Pass(Cf* x, int s, bool inverse) const noexcept {
        if (s == 0) return;
        const int half = inverse ? 1 << (s - 1) : 1 << (bits_ - s), len = half << 1;
        float* f = reinterpret_cast<float*>(x);
        const float* wf = reinterpret_cast<const float*>(&tw_[static_cast<size_t>(half - 1)]);
#if defined(SVMS_BUS_AVX)
        if (n_ >= 8 && BusAvx()) return half >= 4 ? PassAvx(f, wf, half, inverse) : PassAvxSmall(f, wf, half, inverse);
#endif
        const float sign = inverse ? -1.0f : 1.0f;
        for (int i = 0; i < n_; i += len) {
            float* a = f + 2 * i;
            float* b = f + 2 * (i + half);
            for (int k = 0; k < half; ++k) {
                const float wr = wf[2 * k], wi = sign * wf[2 * k + 1];
                const float ar = a[2 * k], ai = a[2 * k + 1];
                if (inverse) {   // a + b w, a - b w
                    const float br = b[2 * k] * wr - b[2 * k + 1] * wi;
                    const float bi = b[2 * k] * wi + b[2 * k + 1] * wr;
                    a[2 * k] = ar + br; a[2 * k + 1] = ai + bi;
                    b[2 * k] = ar - br; b[2 * k + 1] = ai - bi;
                } else {         // a + b, (a - b) w
                    const float dr = ar - b[2 * k], di = ai - b[2 * k + 1];
                    a[2 * k] = ar + b[2 * k]; a[2 * k + 1] = ai + b[2 * k + 1];
                    b[2 * k] = dr * wr - di * wi;
                    b[2 * k + 1] = dr * wi + di * wr;
                }
            }
        }
    }
    void Run(Cf* x, bool inverse) const noexcept {
        for (int s = 1; s <= bits_; ++s) Pass(x, s, inverse);
    }

    static int Partner(int p) noexcept {
        if (p < 2) return p;
        int lo = 2;
        while (lo * 2 <= p) lo <<= 1;
        return 3 * lo - 1 - p;
    }

private:
#if defined(SVMS_BUS_AVX)
    // One butterfly of either kind on four lanes; the scalar loop's arithmetic.
    SVMS_BUS_AVX_FN static void Butterfly(__m256& a, __m256& b, __m256 w, bool inverse) noexcept {
        if (inverse) {
            const __m256 bw = MulAvx4(b, w);
            b = _mm256_sub_ps(a, bw);
            a = _mm256_add_ps(a, bw);
        } else {
            const __m256 d = _mm256_sub_ps(a, b);
            a = _mm256_add_ps(a, b);
            b = MulAvx4(d, w);
        }
    }
    SVMS_BUS_AVX_FN static __m256 Twiddle(const float* wf, bool inverse) noexcept {
        const __m256 w = _mm256_loadu_ps(wf);
        return inverse ? _mm256_xor_ps(w, _mm256_setr_ps(0.0f, -0.0f, 0.0f, -0.0f, 0.0f, -0.0f, 0.0f, -0.0f)) : w;
    }
    // Spans of 8 and more, four butterflies at a time.
    SVMS_BUS_AVX_FN void PassAvx(float* f, const float* wf, int half, bool inverse) const noexcept {
        const int len = half << 1;
        for (int i = 0; i < n_; i += len) {
            float* a = f + 2 * i;
            float* b = f + 2 * (i + half);
            for (int k = 0; k < half; k += 4) {
                __m256 av = _mm256_loadu_ps(a + 2 * k), bv = _mm256_loadu_ps(b + 2 * k);
                Butterfly(av, bv, Twiddle(wf + 2 * k, inverse), inverse);
                _mm256_storeu_ps(a + 2 * k, av);
                _mm256_storeu_ps(b + 2 * k, bv);
            }
        }
    }
    // Spans of 2 and 4: eight values at a time, regrouped so each lane pairs an a with its b.
    SVMS_BUS_AVX_FN void PassAvxSmall(float* f, const float* wf, int half, bool inverse) const noexcept {
        __m256 w;
        if (half == 1) {
            w = _mm256_castpd_ps(_mm256_broadcast_sd(reinterpret_cast<const double*>(wf)));
        } else {
            const __m128 w2 = _mm_loadu_ps(wf);
            w = _mm256_insertf128_ps(_mm256_castps128_ps256(w2), w2, 1);
        }
        if (inverse) w = _mm256_xor_ps(w, _mm256_setr_ps(0.0f, -0.0f, 0.0f, -0.0f, 0.0f, -0.0f, 0.0f, -0.0f));
        for (int i = 0; i < n_; i += 8) {
            const __m256 v0 = _mm256_loadu_ps(f + 2 * i), v1 = _mm256_loadu_ps(f + 2 * i + 8);
            __m256 a, b;
            if (half == 1) {   // pairs (0 1) (2 3) | (4 5) (6 7): a = [0 4 2 6], b = [1 5 3 7]
                a = _mm256_castpd_ps(_mm256_shuffle_pd(_mm256_castps_pd(v0), _mm256_castps_pd(v1), 0x0));
                b = _mm256_castpd_ps(_mm256_shuffle_pd(_mm256_castps_pd(v0), _mm256_castps_pd(v1), 0xF));
            } else {           // quads (0 1 | 2 3) (4 5 | 6 7): a = [0 1 4 5], b = [2 3 6 7]
                a = _mm256_permute2f128_ps(v0, v1, 0x20);
                b = _mm256_permute2f128_ps(v0, v1, 0x31);
            }
            Butterfly(a, b, w, inverse);
            __m256 o0, o1;
            if (half == 1) {
                o0 = _mm256_castpd_ps(_mm256_shuffle_pd(_mm256_castps_pd(a), _mm256_castps_pd(b), 0x0));
                o1 = _mm256_castpd_ps(_mm256_shuffle_pd(_mm256_castps_pd(a), _mm256_castps_pd(b), 0xF));
            } else {
                o0 = _mm256_permute2f128_ps(a, b, 0x20);
                o1 = _mm256_permute2f128_ps(a, b, 0x31);
            }
            _mm256_storeu_ps(f + 2 * i, o0);
            _mm256_storeu_ps(f + 2 * i + 8, o1);
        }
    }
#endif

    int n_ = 0, bits_ = 0;
    std::vector<Cf> tw_;
};

struct ConvLevel {
    int m = 1;                // partition length in blocks
    int offsetBlocks = 0;     // where in the kernel the level starts
    int parts = 0;
    std::shared_ptr<const SplitFft> fft;   // 2 * m * block points
    std::vector<Cf> h;        // parts x 2N spectra of (left + i * right), bins bit-reversed (SplitFft)
};

struct ConvKernel {
    int block = 0;
    int length = 0;
    std::vector<ConvLevel> levels;
};

// Arithmetic of one pass and one spectrum product of a level, in flops (a butterfly is 10, a complex
// multiply-add 8). Used for the even split of a level's work and for the cost figures.
inline double PassFlops(int fftSize) noexcept { return 5.0 * fftSize; }
inline double ProductFlops(int fftSize) noexcept { return 8.0 * fftSize; }

// Cuts a kernel of `length` frames into levels of m0, 4 m0, 16 m0, ... blocks, at most `maxLevels`, and transforms
// it. One level of m0 > 1 blocks is a uniform partition, for a ConvBank that gets its input m0 blocks ahead.
inline ConvKernel MakeKernel(const float* left, const float* right, int length, int block, int maxLevels = 3,
                             int m0 = 1) {
    ConvKernel k;
    k.block = block;
    k.length = length;
    const int blocks = (std::max)(1, (length + block - 1) / block);
    int start = 0;
    for (int lv = 0, m = m0; lv < maxLevels && start < blocks; ++lv, m *= 4) {
        const int end = lv + 1 == maxLevels ? blocks : (std::min)(blocks, 8 * m);   // level lv+1 starts at 2 * 4m
        ConvLevel L;
        L.m = m;
        L.offsetBlocks = start;
        L.parts = (end - start + m - 1) / m;
        const int n = m * block, size = 2 * n;
        L.fft = std::make_shared<const SplitFft>(size);
        L.h.assign(static_cast<size_t>(L.parts) * static_cast<size_t>(size), Cf(0.0f, 0.0f));
        for (int p = 0; p < L.parts; ++p) {
            Cf* h = &L.h[static_cast<size_t>(p) * static_cast<size_t>(size)];
            const int seg = (start + p * m) * block;
            for (int i = 0; i < n; ++i) {
                const int src = seg + i;
                if (src < length) h[i] = Cf(left[src], right[src]);
            }
            L.fft->Run(h, false);
        }
        start += L.parts * m;
        k.levels.push_back(std::move(L));
    }
    return k;
}

// Flops per output frame of one history on this kernel.
inline double FlopsPerFrame(const ConvKernel& k) noexcept {
    double f = 0.0;
    for (const ConvLevel& L : k.levels) {
        const int size = L.fft->Size();
        f += (2.0 * L.fft->Bits() * PassFlops(size) + L.parts * ProductFlops(size)) / (L.m * k.block);
    }
    return f;
}

class ConvHistory {
public:
    void Prepare(const ConvKernel& k) {
        block_ = k.block;
        int maxN = block_;
        state_.clear();
        state_.resize(k.levels.size());
        for (size_t i = 0; i < k.levels.size(); ++i) {
            const ConvLevel& L = k.levels[i];
            LevelState& s = state_[i];
            const int n = L.m * block_, size = 2 * n;
            maxN = (std::max)(maxN, n);
            s.fdl.assign(static_cast<size_t>(L.parts) * static_cast<size_t>(size), Cf(0.0f, 0.0f));
            s.acc.assign(static_cast<size_t>(size), Cf(0.0f, 0.0f));
            s.out.assign(static_cast<size_t>(4 * n), 0.0f);   // [parity][side][n]
            // Work units in order: input copy (with the empty pass 0), forward passes, one product per partition,
            // the inverse's empty pass 0 and its passes, output write. Each gets the step its cost midpoint falls in.
            s.unitFlops.clear();
            const int bits = L.fft->Bits();
            for (int p = 0; p <= bits; ++p) s.unitFlops.push_back(p == 0 ? 0.0 : PassFlops(size));
            for (int p = 0; p < L.parts; ++p) s.unitFlops.push_back(ProductFlops(size));
            for (int p = 0; p <= bits; ++p) s.unitFlops.push_back(p == 0 ? 0.0 : PassFlops(size));
            s.unitFlops.push_back(0.0);
            double total = 0.0;
            for (double f : s.unitFlops) total += f;
            s.firstUnit.assign(static_cast<size_t>(L.m) + 1, static_cast<int>(s.unitFlops.size()));
            s.firstUnit[0] = 0;
            double before = 0.0;
            int step = 0;
            for (size_t u = 0; u < s.unitFlops.size(); ++u) {
                const double mid = before + 0.5 * s.unitFlops[u];
                const int want = (std::min)(L.m - 1, static_cast<int>(mid / (std::max)(total, 1.0) * L.m));
                while (step < want) s.firstUnit[static_cast<size_t>(++step)] = static_cast<int>(u);
                before += s.unitFlops[u];
            }
            while (step < L.m) s.firstUnit[static_cast<size_t>(++step)] = static_cast<int>(s.unitFlops.size());
        }
        ring_ = 1;
        while (ring_ < 4 * maxN) ring_ <<= 1;
        in_.assign(static_cast<size_t>(ring_), 0.0f);
        blockIndex_ = 0;
        lastFlops_ = 0.0;
    }

    // Adds this block's output (`block` frames per side) to outL/outR. `in` is the block's input.
    void Process(const ConvKernel& k, const float* in, float* outL, float* outR) noexcept {
        const uint64_t mask = static_cast<uint64_t>(ring_ - 1);
        const uint64_t frame0 = blockIndex_ * static_cast<uint64_t>(block_);
        for (int i = 0; i < block_; ++i) in_[static_cast<size_t>((frame0 + static_cast<uint64_t>(i)) & mask)] = in[i];
        lastFlops_ = 0.0;
        for (size_t lv = 0; lv < k.levels.size(); ++lv) {
            const ConvLevel& L = k.levels[lv];
            LevelState& s = state_[lv];
            const int n = L.m * block_;
            const uint64_t chunk = blockIndex_ / static_cast<uint64_t>(L.m);
            const int step = static_cast<int>(blockIndex_ % static_cast<uint64_t>(L.m));
            if (L.m == 1) {
                // Level 0: this block's own output, all in this block.
                for (int u = 0; u < static_cast<int>(s.unitFlops.size()); ++u) Unit(L, s, u, chunk, mask);
            } else {
                for (int u = s.firstUnit[static_cast<size_t>(step)]; u < s.firstUnit[static_cast<size_t>(step) + 1]; ++u)
                    Unit(L, s, u, chunk + 1, mask);
            }
            // Output chunk `chunk` (written by the last unit of the previous window, or just now for level 0).
            const float* oL = &s.out[static_cast<size_t>(chunk & 1) * 2 * static_cast<size_t>(n)];
            const float* oR = oL + n;
            const int off = step * block_;
            for (int i = 0; i < block_; ++i) { outL[i] += oL[off + i]; outR[i] += oR[off + i]; }
        }
        ++blockIndex_;
    }

    void Reset() noexcept {
        std::fill(in_.begin(), in_.end(), 0.0f);
        for (LevelState& s : state_) {
            std::fill(s.fdl.begin(), s.fdl.end(), Cf(0.0f, 0.0f));
            std::fill(s.acc.begin(), s.acc.end(), Cf(0.0f, 0.0f));
            std::fill(s.out.begin(), s.out.end(), 0.0f);
        }
        blockIndex_ = 0;
    }

    double LastBlockFlops() const noexcept { return lastFlops_; }

private:
    struct LevelState {
        std::vector<Cf> fdl;          // parts x 2N spectra of past input chunk pairs
        std::vector<Cf> acc;
        std::vector<float> out;
        std::vector<double> unitFlops;
        std::vector<int> firstUnit;   // units of step s: [firstUnit[s], firstUnit[s + 1])
    };

    // Unit u of the work that produces output chunk `target` of a level (chunks of N frames). Output chunk n is
    // the sum over partitions p of X_{n - lag - p} H_p, where X_j is the spectrum of input chunks (j - 1, j).
    // Level 0 has lag 0 (its chunk is this block); level k >= 1 starts 2N into the kernel, lag 2.
    void Unit(const ConvLevel& L, LevelState& s, int u, uint64_t target, uint64_t mask) noexcept {
        const int n = L.m * block_, size = 2 * n, bits = L.fft->Bits();
        const int64_t lag = L.m == 1 ? 0 : 2;
        const int64_t j = static_cast<int64_t>(target) - lag;
        const int64_t slot = ((j % L.parts) + L.parts) % L.parts;
        Cf* x = &s.fdl[static_cast<size_t>(slot) * static_cast<size_t>(size)];
        lastFlops_ += s.unitFlops[static_cast<size_t>(u)];
        if (u == 0) {
            // X_j = FFT of input frames [(j - 1) N, (j + 1) N).
            const uint64_t first = static_cast<uint64_t>((j - 1) * n);
            for (int i = 0; i < size; ++i) x[i] = Cf(in_[static_cast<size_t>((first + static_cast<uint64_t>(i)) & mask)], 0.0f);
            L.fft->Pass(x, 0, false);
            return;
        }
        if (u <= bits) { L.fft->Pass(x, u, false); return; }
        u -= bits + 1;
        if (u < L.parts) {
            const int64_t sj = (((j - u) % L.parts) + L.parts) % L.parts;
            const float* xf = reinterpret_cast<const float*>(&s.fdl[static_cast<size_t>(sj) * static_cast<size_t>(size)]);
            const float* hf = reinterpret_cast<const float*>(&L.h[static_cast<size_t>(u) * static_cast<size_t>(size)]);
            float* af = reinterpret_cast<float*>(s.acc.data());
            if (u == 0) {
                for (int i = 0; i < size; ++i) {
                    af[2 * i] = xf[2 * i] * hf[2 * i] - xf[2 * i + 1] * hf[2 * i + 1];
                    af[2 * i + 1] = xf[2 * i] * hf[2 * i + 1] + xf[2 * i + 1] * hf[2 * i];
                }
            } else {
                for (int i = 0; i < size; ++i) {
                    af[2 * i] += xf[2 * i] * hf[2 * i] - xf[2 * i + 1] * hf[2 * i + 1];
                    af[2 * i + 1] += xf[2 * i] * hf[2 * i + 1] + xf[2 * i + 1] * hf[2 * i];
                }
            }
            return;
        }
        u -= L.parts;
        if (u <= bits) { L.fft->Pass(s.acc.data(), u, true); return; }
        // Overlap-save: the second half is the output chunk.
        const float scale = 1.0f / static_cast<float>(size);
        float* oL = &s.out[static_cast<size_t>(target & 1) * 2 * static_cast<size_t>(n)];
        float* oR = oL + n;
        for (int i = 0; i < n; ++i) {
            oL[i] = s.acc[static_cast<size_t>(n + i)].real() * scale;
            oR[i] = s.acc[static_cast<size_t>(n + i)].imag() * scale;
        }
    }

    int block_ = 0, ring_ = 1;
    uint64_t blockIndex_ = 0;
    double lastFlops_ = 0.0;
    std::vector<float> in_;
    std::vector<LevelState> state_;
};

// Many inputs, each against its own kernel, summed into one output (one MIDI channel of the release bus). The sum
// is linear, so all inputs share each level's output spectra and one inverse FFT per output chunk.
//
// Scatter form: when an input's spectrum X_j is complete, its products X_j H_p go at once into the output
// spectra they belong to (output chunk j + lag + p), a ring of accumulators per level. Each spectrum is read once,
// while it is in cache, and no input keeps a history of spectra. The products run over tiles of bins, so a tile
// of every pending output stays in cache across the inputs. An input silent in a chunk has no spectrum then: its
// forward FFT and products are skipped. Kernels must share the block size and the level layout of MakeKernel
// (m0, 4 m0, 16 m0, ... blocks).
//
// Offline form: a level's work runs whole at the block its output chunk starts, so a block carries the whole FFT
// of every level that starts there. (ConvHistory spreads it; the live bus will need that.) Running just in time
// leaves room for late impulses: one at a past frame can join until its chunk is transformed (Insertable).
// A kernel level with no partitions (the bus kernels' level 0) costs nothing.
//
// Impulses may come up to `ahead` blocks past the next block. Level 0 of m0 > 1 blocks transforms chunk c at its
// first block, so all of chunk c must be in by then: its input must come m0 - 1 blocks ahead or more (a bus
// rendered that far behind its events). Its blocks are fewer and bigger than 1, 4, 16: far less arithmetic.
class ConvBank {
public:
    void Prepare(int block, int maxLevels = 3, int m0 = 1, int ahead = 0) {
        block_ = block;
        ahead_ = ahead;
        levels_.clear();
        for (int lv = 0, m = m0; lv < maxLevels; ++lv, m *= 4) {
            Level L;
            L.m = m;
            L.lag = lv ? 2 : 0;   // level lv >= 1 starts 2 of its chunks into the kernel (MakeKernel)
            L.size = 2 * m * block;
            L.fft = std::make_shared<const SplitFft>(L.size);
            Scratch(L);
            L.out.assign(static_cast<size_t>(4 * m * block), 0.0f);
            levels_.push_back(std::move(L));
        }
        // Input blocks from the oldest a level still reads (lag chunks back) to the furthest ahead, whole blocks.
        const int top = levels_.back().m, oldest = levels_.size() > 1 ? 2 * top : 0;
        ring_ = 1;
        while (ring_ < (oldest + ahead + 2) * block) ring_ <<= 1;
        inputs_.clear();
        free_.clear();
        used_ = 0;
    }

    // Threads for a chunk's work: `fn` runs job(context, i) for i in [0, jobs) and returns when all are done
    // (null: serially). `lanes` is how many run at once. The sums keep their order, so the output is the same.
    using ParallelFn = void (*)(void* user, uint32_t jobs, void (*job)(void* context, uint32_t index), void* context);
    void SetParallel(ParallelFn fn, void* user, uint32_t lanes) {
        par_ = fn;
        parUser_ = user;
        lanes_ = fn ? (std::max)(1u, lanes) : 1u;
        for (Level& L : levels_) Scratch(L);
    }

    // A new input against `k` (which must outlive it); silent before now. Returns its id.
    int Add(const ConvKernel& k) {
        int id;
        if (!free_.empty()) { id = free_.back(); free_.pop_back(); }
        else { id = static_cast<int>(inputs_.size()); inputs_.emplace_back(); }
        Input& in = inputs_[static_cast<size_t>(id)];
        in.k = &k;
        in.ring.assign(static_cast<size_t>(ring_), 0.0f);
        in.loud.assign(static_cast<size_t>(ring_ / block_), 0u);
        in.x.resize(k.levels.size());
        in.prev.resize(k.levels.size());
        in.xLive.assign(k.levels.size(), 0u);
        in.prevLive.assign(k.levels.size(), 0u);
        for (size_t lv = 0; lv < k.levels.size(); ++lv) {
            const size_t size = static_cast<size_t>(2 * k.levels[lv].m * block_);
            in.x[lv].assign(size, Cf(0.0f, 0.0f));
            in.prev[lv].assign(size, Cf(0.0f, 0.0f));
        }
        in.used = true;
        ++used_;
        return id;
    }

    // An input's products already made stay in the outputs: its tail plays on.
    void Remove(int id) {
        Input& in = inputs_[static_cast<size_t>(id)];
        if (!in.used) return;
        in = Input{};
        free_.push_back(id);
        --used_;
    }

    // Every input and every pending output (all sound off).
    void RemoveAll() {
        for (size_t i = 0; i < inputs_.size(); ++i) if (inputs_[i].used) Remove(static_cast<int>(i));
        for (Level& L : levels_) {
            L.outLive[0] = L.outLive[1] = false;
            std::fill(L.accLive.begin(), L.accLive.end(), 0u);
        }
    }

    // Block b's input for `id` (b from the next block to `ahead` past it), block frames to add to.
    float* InputBlock(int id, uint64_t b) {
        Input& in = inputs_[static_cast<size_t>(id)];
        in.loud[static_cast<size_t>(b % static_cast<uint64_t>(ring_ / block_))] = 1u;
        return &in.ring[static_cast<size_t>((b * static_cast<uint64_t>(block_)) & static_cast<uint64_t>(ring_ - 1))];
    }
    int Inputs() const noexcept { return used_; }

    // Whether an impulse at `frame` can still go in when block `next` is the next to process: every level that
    // uses it must not have transformed its chunk yet. A level transforms chunk j at block (j + lag) * m; level 0
    // (if the kernels have one) of 1 block needs the frame in the open block.
    bool Insertable(uint64_t frame, uint64_t next, bool level0) const noexcept {
        const uint64_t blockOf = frame / static_cast<uint64_t>(block_);
        for (size_t lv = level0 ? 0 : 1; lv < levels_.size(); ++lv) {
            const uint64_t m = static_cast<uint64_t>(levels_[lv].m), lag = static_cast<uint64_t>(levels_[lv].lag);
            if ((blockOf / m + lag) * m < next) return false;
        }
        return true;
    }

    // Adds an impulse of weight `w` at `frame` to input `id`: Insertable must hold, and the frame be at most
    // `ahead` blocks past `next`, the next block.
    void AddImpulse(int id, uint64_t frame, float w, uint64_t next) {
        (void)next;
        Input& in = inputs_[static_cast<size_t>(id)];
        in.ring[static_cast<size_t>(frame & static_cast<uint64_t>(ring_ - 1))] += w;
        in.loud[static_cast<size_t>((frame / static_cast<uint64_t>(block_)) % static_cast<uint64_t>(ring_ / block_))] = 1u;
    }

    // Moves impulses (frame, weight) of input `fromId` in `from` to input `toId` in `to` (null: drops them),
    // whatever their age, with the outputs from block `next` - 1's frame `fromOffset` on. What a level has not
    // transformed yet moves in the input rings; what it has moves as products: into the pending output spectra,
    // the output chunk being played (its block `next` - 1 in fromPre / toPre, the processed block's sum) and the
    // input's last chunk spectrum. The banks share block size and levels and are both at block `next`, and the
    // inputs one kernel.
    static void Move(ConvBank& from, int fromId, ConvBank* to, int toId, const std::pair<uint64_t, float>* imp,
                     size_t count, uint64_t next, uint32_t fromOffset, float* const fromPre[2], float* const toPre[2]) {
        const int B = from.block_;
        auto untransformed = [&](uint64_t frame) {
            const uint64_t bo = frame / static_cast<uint64_t>(B);
            for (size_t lv = 0; lv < from.levels_.size(); ++lv) {
                const uint64_t m = static_cast<uint64_t>(from.levels_[lv].m), lag = static_cast<uint64_t>(from.levels_[lv].lag);
                if ((bo / m + lag) * m >= next) return true;
            }
            return false;
        };
        for (size_t i = 0; i < count; ++i)
            if (untransformed(imp[i].first)) {
                from.AddImpulse(fromId, imp[i].first, -imp[i].second, next);
                if (to) to->AddImpulse(toId, imp[i].first, imp[i].second, next);
            }
        if (next == 0) return;
        const ConvKernel& k = *from.inputs_[static_cast<size_t>(fromId)].k;
        for (size_t lv = 0; lv < from.levels_.size() && lv < k.levels.size(); ++lv) {
            const ConvLevel& K = k.levels[lv];
            const int64_t lag = from.levels_[lv].lag;   // chunk c is made from X_{c - lag}
            if (K.parts == 0) continue;
            Level& LF = from.levels_[lv];
            Level* LT = to ? &to->levels_[lv] : nullptr;
            const int m = LF.m, n = m * B, size = LF.size;
            const int64_t cur = static_cast<int64_t>((next - 1) / static_cast<uint64_t>(m));   // output chunk playing
            const int64_t jT = cur - lag;                                                       // last chunk transformed
            int64_t jMin = INT64_MAX;
            for (size_t i = 0; i < count; ++i) {
                const int64_t j = static_cast<int64_t>(imp[i].first / static_cast<uint64_t>(n));
                if (j <= jT) jMin = (std::min)(jMin, j);
            }
            if (jMin == INT64_MAX) continue;
            Grow(LF, cur + 1, static_cast<size_t>(K.parts));
            if (LT) Grow(*LT, cur + 1, static_cast<size_t>(K.parts));
            std::vector<Cf> z(static_cast<size_t>(size)), zn(static_cast<size_t>(size)), ycur(static_cast<size_t>(size), Cf(0.0f, 0.0f));
            bool curLive = false;
            // The window [chunk j - 1, chunk j] of the moved impulses.
            auto window = [&](int64_t j, bool firstHalfOnly) {
                std::fill(z.begin(), z.end(), Cf(0.0f, 0.0f));
                const int64_t lo = firstHalfOnly ? j * n : (j - 1) * n, hi = firstHalfOnly ? (j + 1) * n : (j + 1) * n;
                bool any = false;
                for (size_t i = 0; i < count; ++i) {
                    const int64_t f = static_cast<int64_t>(imp[i].first);
                    if (f < lo || f >= hi) continue;
                    z[static_cast<size_t>(f - lo)] += Cf(imp[i].second, 0.0f);
                    any = true;
                }
                return any;
            };
            auto slot = [](Level& L, int64_t t, int size) -> Cf* {
                const size_t s = static_cast<size_t>(t % static_cast<int64_t>(L.acc.size()));
                if (!L.accLive[s]) { L.acc[s].assign(static_cast<size_t>(size), Cf(0.0f, 0.0f)); L.accLive[s] = 1u; }
                return L.acc[s].data();
            };
            for (int64_t j = jMin; j <= jT; ++j) {
                if (!window(j, false)) continue;
                LF.fft->Run(z.data(), false);
                for (int i = 0; i < size; ++i) zn[static_cast<size_t>(i)] = -z[static_cast<size_t>(i)];
                for (int64_t p = 0; p < K.parts; ++p) {
                    const int64_t t = j + lag + p;
                    if (t < cur) continue;
                    const float* h = reinterpret_cast<const float*>(&K.h[static_cast<size_t>(p) * static_cast<size_t>(size)]);
                    if (t == cur) {
                        Mul(reinterpret_cast<float*>(ycur.data()), reinterpret_cast<const float*>(z.data()), h, size, true);
                        curLive = true;
                        continue;
                    }
                    Mul(reinterpret_cast<float*>(slot(LF, t, size)), reinterpret_cast<const float*>(zn.data()), h, size, true);
                    if (LT) Mul(reinterpret_cast<float*>(slot(*LT, t, size)), reinterpret_cast<const float*>(z.data()), h, size, true);
                }
            }
            if (curLive) {
                // The chunk being played: its rest (from the next block) in the level's output, the processed block
                // in the group's sum.
                LF.fft->Run(ycur.data(), true);
                const float scale = 1.0f / static_cast<float>(size);
                const int off = static_cast<int>((next - 1) % static_cast<uint64_t>(m)) * B;
                auto apply = [&](Level& L, float sign, float* const pre[2]) {
                    if (m != 1) {   // a level of 1 block outputs each block as it makes it: nothing ahead
                        float* oL = &L.out[static_cast<size_t>(cur & 1) * 2 * static_cast<size_t>(n)];
                        float* oR = oL + n;
                        if (!L.outLive[cur & 1]) { std::fill(oL, oR + n, 0.0f); L.outLive[cur & 1] = true; }
                        for (int i = off + B; i < n; ++i) {
                            oL[i] += sign * ycur[static_cast<size_t>(n + i)].real() * scale;
                            oR[i] += sign * ycur[static_cast<size_t>(n + i)].imag() * scale;
                        }
                    }
                    for (int i = static_cast<int>(fromOffset); i < B; ++i) {
                        pre[0][i] += sign * ycur[static_cast<size_t>(n + off + i)].real() * scale;
                        pre[1][i] += sign * ycur[static_cast<size_t>(n + off + i)].imag() * scale;
                    }
                };
                apply(LF, -1.0f, fromPre);
                if (LT) apply(*LT, 1.0f, toPre);
            }
            // G_jT, which the next chunk's window takes from the input.
            if (window(jT, true)) {
                LF.fft->Run(z.data(), false);
                auto fold = [&](Input& in, float sign) {
                    Cf* prev = in.prev[lv].data();
                    if (!in.prevLive[lv]) { std::fill(prev, prev + size, Cf(0.0f, 0.0f)); in.prevLive[lv] = 1u; }
                    for (int i = 0; i < size; ++i) prev[i] += sign * z[static_cast<size_t>(i)];
                };
                fold(from.inputs_[static_cast<size_t>(fromId)], -1.0f);
                if (to) fold(to->inputs_[static_cast<size_t>(toId)], 1.0f);
            }
        }
    }

    // Adds block `b`'s output (block frames per side) to outL/outR. Blocks are absolute (frame b * block on) and
    // consecutive. Returns false if it added nothing.
    bool Process(float* outL, float* outR, uint64_t b) {
        const uint64_t mask = static_cast<uint64_t>(ring_ - 1);
        const uint64_t blocksInRing = static_cast<uint64_t>(ring_ / block_);
        bool any = false;
        for (size_t lv = 0; lv < levels_.size(); ++lv) {
            Level& L = levels_[lv];
            const uint64_t m = static_cast<uint64_t>(L.m);
            const int n = L.m * block_;
            if (L.m == 1) {
                // Output chunk b: X_b H_0 now, the rest made in earlier blocks.
                any = Chunk(lv, static_cast<int64_t>(b), 0, mask, blocksInRing, outL, outR, true) || any;
                continue;
            }
            const int64_t c = static_cast<int64_t>(b / m);
            if (b % m == 0u) {
                // Chunk c starts and needs its output now, from X_{c - lag} (with lag 2, complete since chunk c - 1
                // began; with lag 0, from input given ahead). Just in time rather than a chunk ahead, so an impulse
                // can join chunk j until block (j + lag) * m.
                float* oL = &L.out[static_cast<size_t>(c & 1) * 2 * static_cast<size_t>(n)];
                L.outLive[c & 1] = Chunk(lv, c - L.lag, L.lag, mask, blocksInRing, oL, oL + n, false);
            }
            if (L.outLive[c & 1]) {
                const float* oL = &L.out[static_cast<size_t>(c & 1) * 2 * static_cast<size_t>(n)];
                const float* oR = oL + n;
                const int off = static_cast<int>(b % m) * block_;
                for (int i = 0; i < block_; ++i) { outL[i] += oL[off + i]; outR[i] += oR[off + i]; }
                any = true;
            }
        }
        // The furthest block an impulse can come in next starts empty (its ring slot held one long gone).
        const uint64_t clear = b + 1u + static_cast<uint64_t>(ahead_);
        for (Input& in : inputs_) {
            if (!in.used) continue;
            std::fill_n(in.ring.begin() + static_cast<std::ptrdiff_t>((clear * static_cast<uint64_t>(block_)) & mask), block_, 0.0f);
            in.loud[static_cast<size_t>(clear % blocksInRing)] = 0u;
        }
        return any;
    }

private:
    static constexpr int kTile = 1024;   // bins per product tile

    struct Input {
        const ConvKernel* k = nullptr;
        std::vector<float> ring;              // input, block b at (b * block) % ring
        std::vector<uint8_t> loud;            // per block in the ring: any non-zero input
        std::vector<std::vector<Cf>> x, prev; // per level: X_j (when xLive), G of the last chunk (when prevLive)
        std::vector<uint8_t> xLive, prevLive;
        bool used = false;
    };
    struct Level {
        int m = 1, lag = 0, size = 0;
        std::shared_ptr<const SplitFft> fft;
        std::vector<std::vector<Cf>> z, ga, gb;   // per lane
        std::vector<std::vector<Cf>> acc;   // output spectra, chunk t in slot t % acc.size()
        std::vector<uint8_t> accLive;       // the slot holds products (else its contents are stale)
        std::vector<float> out;             // [parity][side][N], levels >= 1
        bool outLive[2] = {false, false};
    };

    // X_j of input `in`, from G_j = g (nullptr: chunk j silent): X_j = G_{j-1} + (-1)^k G_j, as chunk j sits in
    // the second half of X_j's window. Bins are bit-reversed (SplitFft), so the even bins fill the first half of
    // the positions and the odd ones the second. Keeps G_j for X_{j+1}.
    static void Fold(Input& in, size_t lv, int size, const Cf* g) {
        Cf* x = in.x[lv].data();
        Cf* prev = in.prev[lv].data();
        const bool had = in.prevLive[lv] != 0;
        const int h = size / 2;
        in.xLive[lv] = had || g;
        in.prevLive[lv] = g != nullptr;
        if (!g) {
            if (had) std::memcpy(x, prev, sizeof(Cf) * static_cast<size_t>(size));
            return;
        }
        if (had) {
            for (int k = 0; k < h; ++k) x[k] = prev[k] + g[k];
            for (int k = h; k < size; ++k) x[k] = prev[k] - g[k];
        } else {
            for (int k = 0; k < h; ++k) x[k] = g[k];
            for (int k = h; k < size; ++k) x[k] = -g[k];
        }
        std::memcpy(prev, g, sizeof(Cf) * static_cast<size_t>(size));
    }

    void Scratch(Level& L) const {
        const std::vector<Cf> zero(static_cast<size_t>(L.size), Cf(0.0f, 0.0f));
        L.z.assign(lanes_, zero);
        L.ga.assign(lanes_, zero);
        L.gb.assign(lanes_, zero);
    }
    // f(i) for i in [0, jobs), on the threads when there are two jobs or more.
    template <class F> void Par(uint32_t jobs, F&& f) {
        if (!par_ || jobs < 2u) { for (uint32_t i = 0; i < jobs; ++i) f(i); return; }
        using Fn = std::remove_reference_t<F>;
        par_(parUser_, jobs, [](void* c, uint32_t i) { (*static_cast<Fn*>(c))(i); }, &f);
    }

    // At least `need` accumulator slots; live ones keep their chunk (window [t, t + old size)).
    static void Grow(Level& L, int64_t t, size_t need) {
        const size_t old = L.acc.size();
        if (old >= need) return;
        std::vector<std::vector<Cf>> acc(need);
        std::vector<uint8_t> live(need, 0u);
        for (size_t s = 0; s < old; ++s) {
            if (!L.accLive[s]) continue;
            const int64_t ahead = ((static_cast<int64_t>(s) - t) % static_cast<int64_t>(old) + static_cast<int64_t>(old)) %
                                  static_cast<int64_t>(old);
            const size_t to = static_cast<size_t>((t + ahead) % static_cast<int64_t>(need));
            acc[to].swap(L.acc[s]);
            live[to] = 1u;
        }
        L.acc.swap(acc);
        L.accLive.swap(live);
    }

    // Level lv at input chunk j: every input's X_j (input chunks j - 1 and j), its products into output chunks
    // j + lag + p, then output chunk t = j + lag, inverted, into oL/oR (added if `add`). False if that chunk is
    // silent (oL/oR untouched).
    bool Chunk(size_t lv, int64_t j, int64_t lag, uint64_t mask, uint64_t blocksInRing, float* oL, float* oR, bool add) {
        Level& L = levels_[lv];
        const int n = L.m * block_, size = L.size;
        const int64_t t = j + lag;
        // G_j of every input loud in chunk j. The inputs are real, so two share one complex FFT (a + i b) and
        // split by symmetry: G_a[k] = (Z[k] + conj Z[-k]) / 2, G_b[k] = (Z[k] - conj Z[-k]) / 2i.
        loudIds_.clear();
        for (size_t id = 0; id < inputs_.size(); ++id) {
            Input& in = inputs_[id];
            if (!in.used || lv >= in.k->levels.size() || in.k->levels[lv].parts == 0) continue;
            bool loud = false;
            for (int64_t bb = j * L.m; bb < (j + 1) * L.m && !loud; ++bb)
                if (bb >= 0) loud = in.loud[static_cast<size_t>(static_cast<uint64_t>(bb) % blocksInRing)] != 0;
            if (loud) loudIds_.push_back(static_cast<uint32_t>(id));
            else Fold(in, lv, size, nullptr);
        }
        // Each lane takes every lanes-th pair; the pairs touch distinct inputs.
        const uint32_t pairs = static_cast<uint32_t>((loudIds_.size() + 1) / 2);
        const uint32_t lanes = (std::min)(lanes_, pairs);
        Par(lanes, [&](uint32_t lane) {
            Cf* z = L.z[lane].data();
            Cf* ga = L.ga[lane].data();
            Cf* gb = L.gb[lane].data();
            for (size_t q = 2 * size_t(lane); q < loudIds_.size(); q += 2 * size_t(lanes)) {
                Input& a = inputs_[loudIds_[q]];
                Input* b = q + 1 < loudIds_.size() ? &inputs_[loudIds_[q + 1]] : nullptr;
                const uint64_t first = static_cast<uint64_t>(j * n);   // j >= 0: chunk j had input
                for (int i = 0; i < n; ++i) {
                    const size_t r = static_cast<size_t>((first + static_cast<uint64_t>(i)) & mask);
                    z[i] = Cf(a.ring[r], b ? b->ring[r] : 0.0f);
                }
                std::fill(z + n, z + size, Cf(0.0f, 0.0f));
                L.fft->Run(z, false);
                if (!b) { Fold(a, lv, size, z); continue; }
                // Bin -k of position p sits at SplitFft::Partner(p): mirrored within each octave of positions.
                const auto split = [&](int p, int q2) {
                    const Cf zk = z[p], zc = std::conj(z[q2]);
                    ga[p] = 0.5f * (zk + zc);
                    const Cf d = zk - zc;
                    gb[p] = Cf(0.5f * d.imag(), -0.5f * d.real());
                };
                split(0, 0);
                split(1, 1);
                for (int lo = 2; lo < size; lo <<= 1)
                    for (int p = lo; p < 2 * lo; ++p) split(p, 3 * lo - 1 - p);
                Fold(a, lv, size, ga);
                Fold(*b, lv, size, gb);
            }
        });
        // The products, scattered. Slots a product reaches for the first time start from zero.
        liveIds_.clear();
        size_t need = 1;
        for (size_t id = 0; id < inputs_.size(); ++id) {
            const Input& in = inputs_[id];
            if (!in.used || lv >= in.k->levels.size() || in.k->levels[lv].parts == 0 || !in.xLive[lv]) continue;
            liveIds_.push_back(static_cast<uint32_t>(id));
            need = (std::max)(need, static_cast<size_t>(in.k->levels[lv].parts));
        }
        Grow(L, t, need);
        const int64_t slots = static_cast<int64_t>(L.acc.size());
        for (const uint32_t id : liveIds_) {
            const int64_t parts = inputs_[id].k->levels[lv].parts;
            for (int64_t p = 0; p < parts; ++p) {
                const size_t s = static_cast<size_t>((t + p) % slots);
                if (L.accLive[s]) continue;
                L.acc[s].assign(static_cast<size_t>(size), Cf(0.0f, 0.0f));
                L.accLive[s] = 1u;
            }
        }
        // Tiles of bins are independent: one job each.
        Par(liveIds_.empty() ? 0u : static_cast<uint32_t>((size + kTile - 1) / kTile), [&](uint32_t tile) {
            const int k0 = static_cast<int>(tile) * kTile;
            const int count = (std::min)(kTile, size - k0);
            for (const uint32_t id : liveIds_) {
                const Input& in = inputs_[id];
                const ConvLevel& K = in.k->levels[lv];
                const float* xf = reinterpret_cast<const float*>(in.x[lv].data() + k0);
                for (int64_t p = 0; p < K.parts; ++p) {
                    Cf* acc = L.acc[static_cast<size_t>((t + p) % slots)].data() + k0;
                    const Cf* h = &K.h[static_cast<size_t>(p) * static_cast<size_t>(size) + static_cast<size_t>(k0)];
                    Mul(reinterpret_cast<float*>(acc), xf, reinterpret_cast<const float*>(h), count, true);
                }
            }
        });
        // Output chunk t is complete.
        const size_t s = static_cast<size_t>(t % slots);
        if (!L.accLive[s]) return false;
        L.accLive[s] = 0u;
        Cf* y = L.acc[s].data();
        L.fft->Run(y, true);
        const float scale = 1.0f / static_cast<float>(size);
        if (add) {
            for (int i = 0; i < n; ++i) { oL[i] += y[n + i].real() * scale; oR[i] += y[n + i].imag() * scale; }
        } else {
            for (int i = 0; i < n; ++i) { oL[i] = y[n + i].real() * scale; oR[i] = y[n + i].imag() * scale; }
        }
        return true;
    }

    int block_ = 0, ring_ = 1, used_ = 0, ahead_ = 0;
    ParallelFn par_ = nullptr;
    void* parUser_ = nullptr;
    uint32_t lanes_ = 1;
    std::vector<Level> levels_;
    std::vector<Input> inputs_;
    std::vector<int> free_;
    std::vector<uint32_t> loudIds_, liveIds_;
};


}  // namespace bus
}  // namespace svms
