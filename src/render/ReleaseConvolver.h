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
#include <vector>

namespace svms {
namespace bus {

using Cf = std::complex<float>;

// Radix-2 complex FFT, unnormalized, whose passes can run one at a time: pass 0 is the bit-reversal permutation,
// pass s (1..Bits) the butterflies of span 2^s.
class SplitFft {
public:
    explicit SplitFft(int n) : n_(n) {
        while ((1 << bits_) < n_) ++bits_;
        rev_.resize(static_cast<size_t>(n_));
        for (int i = 0; i < n_; ++i) {
            int r = 0;
            for (int b = 0; b < bits_; ++b) if (i & (1 << b)) r |= 1 << (bits_ - 1 - b);
            rev_[static_cast<size_t>(i)] = r;
        }
        tw_.resize(static_cast<size_t>((std::max)(1, n_ - 1)));   // pass s: 2^(s-1) twiddles at offset 2^(s-1) - 1
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
        if (s == 0) {
            for (int i = 0; i < n_; ++i) {
                const int j = rev_[static_cast<size_t>(i)];
                if (j > i) std::swap(x[i], x[j]);
            }
            return;
        }
        const int half = 1 << (s - 1), len = half << 1;
        const Cf* w = &tw_[static_cast<size_t>(half - 1)];
        float* f = reinterpret_cast<float*>(x);
        const float* wf = reinterpret_cast<const float*>(w);
        const float sign = inverse ? -1.0f : 1.0f;
        for (int i = 0; i < n_; i += len) {
            float* a = f + 2 * i;
            float* b = f + 2 * (i + half);
            for (int k = 0; k < half; ++k) {
                const float wr = wf[2 * k], wi = sign * wf[2 * k + 1];
                const float br = b[2 * k] * wr - b[2 * k + 1] * wi;
                const float bi = b[2 * k] * wi + b[2 * k + 1] * wr;
                const float ar = a[2 * k], ai = a[2 * k + 1];
                a[2 * k] = ar + br; a[2 * k + 1] = ai + bi;
                b[2 * k] = ar - br; b[2 * k + 1] = ai - bi;
            }
        }
    }
    void Run(Cf* x, bool inverse) const noexcept {
        for (int s = 0; s <= bits_; ++s) Pass(x, s, inverse);
    }

private:
    int n_ = 0, bits_ = 0;
    std::vector<int> rev_;
    std::vector<Cf> tw_;
};

struct ConvLevel {
    int m = 1;                // partition length in blocks
    int offsetBlocks = 0;     // where in the kernel the level starts
    int parts = 0;
    std::shared_ptr<const SplitFft> fft;   // 2 * m * block points
    std::vector<Cf> h;        // parts x 2N spectra of (left + i * right)
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

// Cuts a kernel of `length` frames into levels of 1, 4, 16, ... blocks, at most `maxLevels`, and transforms it.
inline ConvKernel MakeKernel(const float* left, const float* right, int length, int block, int maxLevels = 3) {
    ConvKernel k;
    k.block = block;
    k.length = length;
    const int blocks = (std::max)(1, (length + block - 1) / block);
    int start = 0;
    for (int lv = 0, m = 1; lv < maxLevels && start < blocks; ++lv, m *= 4) {
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
            // Work units in order: input copy + bit reversal, forward passes, one product per partition,
            // inverse bit reversal + passes, output write. Each gets the step its cost midpoint falls in.
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

}  // namespace bus
}  // namespace svms
