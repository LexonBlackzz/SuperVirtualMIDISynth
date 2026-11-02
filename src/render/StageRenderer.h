#pragma once

// Audio side of the 3D stage. The control thread builds an immutable
// `RoomResponse` (direct paths, the strongest early reflections as live taps,
// a static early response and the shared traced tail) and hands it over; the
// audio thread only reads it. Nothing here allocates or locks once `Prepare`
// has run, and the output does not depend on how a stream is split into blocks.
//
// Per source the signal is summed to mono and goes three ways:
//   * delay line -> direct path and live taps, each with its own delay (ramped
//     across the block, which is exactly the Doppler shift of a moving path)
//     and per-ear band gains from the spatializer;
//   * a partitioned convolver holding the static early response;
//   * the shared tail convolver, fed with all sources.
// The convolved paths carry one partition (B frames) of latency; their
// responses start B samples late to compensate.

#include "render/StageSolver.h"
#include "render/StageTracer.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace svms {
namespace stage {

constexpr int kMaxSources = 16;
constexpr int kMaxLiveTaps = 10;
constexpr uint32_t kResponseVersion = 1;

// ── Spatializer seam ──────────────────────────────────────────────────────
// What reaches each ear from a direction: an arrival time offset and a gain
// per band. A HRTF implementation adds FIRs through `fir` (null = unused).
struct EarFilters {
    float delay[2] = {0.0f, 0.0f};          // seconds, relative to the path's own delay
    float band[2][kBands] = {};
    const float* fir[2] = {nullptr, nullptr};
    int firLength = 0;
};

struct Spatializer {
    virtual ~Spatializer() = default;
    // `bandGain` is the path's own gain per band (distance, air, surfaces, blocking).
    virtual void Filters(const Relative& rel, const float bandGain[kBands],
                         EarFilters& out) const noexcept = 0;
};

// Level panning, interaural time difference (Woodworth), head shadow on the
// far ear's highs and a rolloff for sources behind. Kept for headphones
// without HRTF data and as the default.
struct EarModelSpatializer final : Spatializer {
    bool enabled = true;
    void Filters(const Relative& rel, const float bandGain[kBands],
                 EarFilters& out) const noexcept override {
        const float rr = (std::max)(0.1f, rel.distance);
        const float lat = (std::max)(-1.0f, (std::min)(1.0f, rel.right / rr));
        const float behind = BehindFactor(rel);
        const float theta = std::asin(lat);
        const float itd = enabled ? kHeadRadius / kSpeedOfSound * (theta + std::sin(theta)) : 0.0f;
        const float pan = (lat + 1.0f) * 0.7853981633974483f;
        const float behindGain = enabled ? (std::max)(0.12f, std::pow(0.2f, behind * 2.2f)) : 1.0f;
        for (int ear = 0; ear < 2; ++ear) {
            const float levelPan = ear == 0 ? std::cos(pan) : std::sin(pan);
            const float shadowSide = ear == 0 ? (std::max)(0.0f, lat) : (std::max)(0.0f, -lat);
            const float f = enabled ? std::pow(0.18f, shadowSide) * behindGain : 1.0f;
            out.delay[ear] = ear == 0 ? itd * 0.5f : -itd * 0.5f;
            out.band[ear][0] = bandGain[0] * levelPan;
            out.band[ear][1] = bandGain[1] * levelPan;
            out.band[ear][2] = bandGain[2] * levelPan * std::sqrt(f);
            out.band[ear][3] = bandGain[3] * levelPan * f;
        }
    }
};

// ── FFT and partitioned convolution ──────────────────────────────────────
struct FftPlan {
    int n = 0;
    std::vector<float> c, s;
    std::vector<int> rev;
    explicit FftPlan(int size = 0) { Init(size); }
    void Init(int size) {
        n = size;
        c.assign(static_cast<size_t>((std::max)(1, n / 2)), 0.0f);
        s = c;
        rev.assign(static_cast<size_t>(n), 0);
        for (int i = 0; i < n / 2; ++i) {
            c[static_cast<size_t>(i)] = static_cast<float>(std::cos(6.283185307179586 * i / n));
            s[static_cast<size_t>(i)] = static_cast<float>(-std::sin(6.283185307179586 * i / n));
        }
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[static_cast<size_t>(i)] = r;
        }
    }
    void Run(float* re, float* im, bool inverse) const noexcept {
        for (int i = 0; i < n; ++i) {
            const int j = rev[static_cast<size_t>(i)];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k) {
                    const float wr = c[static_cast<size_t>(k * step)];
                    const float wi = inverse ? -s[static_cast<size_t>(k * step)] : s[static_cast<size_t>(k * step)];
                    const int a = i + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
        }
        if (inverse) {
            const float k = 1.0f / static_cast<float>(n);
            for (int i = 0; i < n; ++i) { re[i] *= k; im[i] *= k; }
        }
    }
};

// Spectra of a stereo impulse response cut into partitions of B samples.
struct IrSpectra {
    int B = 0, P = 0, bins = 0;
    std::vector<float> re[2], im[2];   // [ear][partition * bins + k]
};

inline std::shared_ptr<const IrSpectra> MakeSpectra(const FftPlan& plan, const float* left,
                                                    const float* right, int length) {
    auto out = std::make_shared<IrSpectra>();
    const int B = plan.n / 2;
    out->B = B;
    out->bins = B + 1;
    out->P = (std::max)(1, (length + B - 1) / B);
    std::vector<float> re(static_cast<size_t>(2 * B)), im(static_cast<size_t>(2 * B));
    const float* ir[2] = {left, right};
    for (int e = 0; e < 2; ++e) {
        out->re[e].assign(static_cast<size_t>(out->P) * static_cast<size_t>(out->bins), 0.0f);
        out->im[e] = out->re[e];
        for (int p = 0; p < out->P; ++p) {
            for (int i = 0; i < 2 * B; ++i) {
                const int src = p * B + i;
                re[static_cast<size_t>(i)] = (i < B && src < length) ? ir[e][src] : 0.0f;
                im[static_cast<size_t>(i)] = 0.0f;
            }
            plan.Run(re.data(), im.data(), false);
            std::copy(re.begin(), re.begin() + out->bins, out->re[e].begin() + static_cast<long>(p) * out->bins);
            std::copy(im.begin(), im.begin() + out->bins, out->im[e].begin() + static_cast<long>(p) * out->bins);
        }
    }
    return out;
}

// Mono in, stereo out; one per stream. Input is gathered into B-frame chunks
// so any block size works; the output lags the input by exactly B frames.
class StreamConvolver {
public:
    void Prepare(const FftPlan& plan, int partitions, int maxFrames) {
        B_ = plan.n / 2;
        bins_ = B_ + 1;
        P_ = (std::max)(1, partitions);
        fdRe_.assign(static_cast<size_t>(P_) * static_cast<size_t>(bins_), 0.0f);
        fdIm_ = fdRe_;
        prev_.assign(static_cast<size_t>(B_), 0.0f);
        tRe_.assign(static_cast<size_t>(2 * B_), 0.0f);
        tIm_ = tRe_;
        accRe_ = tRe_;
        accIm_ = tRe_;
        ring_ = 1;
        while (ring_ < maxFrames + 2 * B_ + 8) ring_ <<= 1;
        in_.assign(static_cast<size_t>(ring_), 0.0f);
        out_[0].assign(static_cast<size_t>(ring_), 0.0f);
        out_[1] = out_[0];
        inW_ = 0; inCount_ = 0; slot_ = 0;
        outR_ = 0; outW_ = static_cast<uint64_t>(B_);   // B frames of latency
    }
    int Partitions() const noexcept { return P_; }

    // Adds the convolution of `in` with `ir` to outL/outR.
    void Process(const FftPlan& plan, const IrSpectra* ir, const float* in, int n,
                 float* outL, float* outR) noexcept {
        const size_t mask = static_cast<size_t>(ring_ - 1);
        for (int i = 0; i < n; ++i) {
            in_[static_cast<size_t>(inW_++) & mask] = in[i];
            if (++inCount_ == B_) { inCount_ = 0; Chunk(plan, ir); }
            outL[i] += out_[0][static_cast<size_t>(outR_) & mask];
            outR[i] += out_[1][static_cast<size_t>(outR_) & mask];
            ++outR_;
        }
    }

private:
    void Chunk(const FftPlan& plan, const IrSpectra* ir) noexcept {
        const size_t mask = static_cast<size_t>(ring_ - 1);
        const uint64_t start = inW_ - static_cast<uint64_t>(B_);
        for (int i = 0; i < B_; ++i) {
            const float x = in_[static_cast<size_t>(start + static_cast<uint64_t>(i)) & mask];
            tRe_[static_cast<size_t>(i)] = prev_[static_cast<size_t>(i)];
            tRe_[static_cast<size_t>(B_ + i)] = x;
            tIm_[static_cast<size_t>(i)] = tIm_[static_cast<size_t>(B_ + i)] = 0.0f;
            prev_[static_cast<size_t>(i)] = x;
        }
        plan.Run(tRe_.data(), tIm_.data(), false);
        std::copy(tRe_.begin(), tRe_.begin() + bins_, fdRe_.begin() + static_cast<long>(slot_) * bins_);
        std::copy(tIm_.begin(), tIm_.begin() + bins_, fdIm_.begin() + static_cast<long>(slot_) * bins_);
        for (int e = 0; e < 2; ++e) {
            float* ar = accRe_.data();
            float* ai = accIm_.data();
            std::fill(ar, ar + bins_, 0.0f);
            std::fill(ai, ai + bins_, 0.0f);
            if (ir && ir->B == B_) {
                const int parts = (std::min)(ir->P, P_);
                for (int p = 0; p < parts; ++p) {
                    int sl = slot_ - p;
                    if (sl < 0) sl += P_;
                    const float* xr = &fdRe_[static_cast<size_t>(sl) * static_cast<size_t>(bins_)];
                    const float* xi = &fdIm_[static_cast<size_t>(sl) * static_cast<size_t>(bins_)];
                    const float* hr = &ir->re[e][static_cast<size_t>(p) * static_cast<size_t>(bins_)];
                    const float* hi = &ir->im[e][static_cast<size_t>(p) * static_cast<size_t>(bins_)];
                    for (int k = 0; k < bins_; ++k) {
                        ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
                        ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
                    }
                }
            }
            for (int k = 1; k < B_; ++k) { ar[2 * B_ - k] = ar[k]; ai[2 * B_ - k] = -ai[k]; }
            plan.Run(ar, ai, true);
            for (int i = 0; i < B_; ++i)
                out_[e][static_cast<size_t>(outW_ + static_cast<uint64_t>(i)) & mask] = ar[B_ + i];
        }
        outW_ += static_cast<uint64_t>(B_);
        if (++slot_ == P_) slot_ = 0;
    }

    int B_ = 0, bins_ = 0, P_ = 1, ring_ = 1, inCount_ = 0, slot_ = 0;
    uint64_t inW_ = 0, outR_ = 0, outW_ = 0;
    std::vector<float> fdRe_, fdIm_, prev_, tRe_, tIm_, accRe_, accIm_, in_;
    std::vector<float> out_[2];
};

// ── Response snapshot ────────────────────────────────────────────────────
struct LiveTap {
    float delaySeconds = 0.0f;
    EarFilters ears;
};

struct SourceResponse {
    bool active = false;
    LiveTap direct;
    int tapCount = 0;
    LiveTap taps[kMaxLiveTaps];
    std::shared_ptr<const IrSpectra> early;   // static early response, may be null
};

struct RoomMeta {
    float t60[kBands] = {};
    float schroederHz = 0.0f;
    bool waveLimited[kBands] = {};
    int rays = 0;
    float tailSeconds = 0.0f;
};

// Everything the audio thread reads. Built on a control thread, never changed
// afterwards. New fields are added at the end only.
struct RoomResponse {
    uint32_t version = kResponseVersion;
    uint64_t serial = 0;
    bool cut = false;     // jump delays instead of gliding (a new room, not a moving source)
    SourceResponse source[kMaxSources];
    std::shared_ptr<const IrSpectra> tail;
    RoomMeta meta;
};

// ── Renderer ─────────────────────────────────────────────────────────────
class StageRenderer {
public:
    // Allocates everything. Control thread.
    void Prepare(float sampleRate, int partition, int maxFrames, int earlyPartitions,
                 int tailPartitions) {
        sr_ = sampleRate;
        partition_ = partition;
        plan_.Init(2 * partition);
        for (int i = 0; i < 3; ++i) alpha_[i] = 1.0f - std::exp(-6.283185307179586f * kSplit[i] / sr_);
        feedSize_ = 1;
        while (feedSize_ < static_cast<int>(sr_ * 1.2f) + maxFrames) feedSize_ <<= 1;
        for (SourceState& s : src_) {
            s.feed.assign(static_cast<size_t>(feedSize_), 0.0f);
            s.writePos = 0;
            s.early.Prepare(plan_, earlyPartitions, maxFrames);
            s.mono.assign(static_cast<size_t>(maxFrames), 0.0f);
            s.fresh = true;
            s.tapsSeen = 0;
            for (TapState& t : s.tap) t = TapState{};
        }
        tail_.Prepare(plan_, tailPartitions, maxFrames);
        tailIn_.assign(static_cast<size_t>(maxFrames), 0.0f);
        zeros_.assign(static_cast<size_t>(maxFrames), 0.0f);
        scratchL_ = zeros_;
        scratchR_ = zeros_;
        maxFrames_ = maxFrames;
    }
    int Partition() const noexcept { return partition_; }
    const FftPlan& Plan() const noexcept { return plan_; }

    // Control thread: offer a new response. The previous pending one, if the
    // audio thread has not taken it yet, is dropped here.
    void Submit(std::shared_ptr<const RoomResponse> r) {
        const RoomResponse* raw = r.get();
        owned_.push_back(std::move(r));
        current_.store(raw, std::memory_order_seq_cst);
        // Free what neither the current slot nor the audio thread's hazard
        // pointer refers to.
        const RoomResponse* inUse = inUse_.load(std::memory_order_seq_cst);
        for (size_t i = 0; i < owned_.size();) {
            const RoomResponse* o = owned_[i].get();
            if (o != raw && o != inUse) owned_.erase(owned_.begin() + static_cast<long>(i));
            else ++i;
        }
    }

    // Audio thread. `in` holds one mono feed per MIDI channel (null = silent);
    // the stage's output is added to outL/outR.
    void Process(const float* const* in, int channels, int frames, float* outL, float* outR) noexcept {
        if (frames > maxFrames_) return;
        // Hazard pointer: publish what is being read, then check it is still current.
        const RoomResponse* cur;
        do {
            cur = current_.load(std::memory_order_seq_cst);
            inUse_.store(cur, std::memory_order_seq_cst);
        } while (cur != current_.load(std::memory_order_seq_cst));
        if (!cur) return;
        const RoomResponse& rsp = *cur;
        std::fill(tailIn_.begin(), tailIn_.begin() + frames, 0.0f);
        const int count = (std::min)(channels, kMaxSources);
        for (int ch = 0; ch < count; ++ch) {
            SourceState& s = src_[ch];
            const SourceResponse& sr = rsp.source[ch];
            const bool live = sr.active && in[ch];
            if (!live) {
                // Keep the feed ringing out silently so a re-enabled source starts clean.
                s.fresh = true;
                std::fill(scratchL_.begin(), scratchL_.begin() + frames, 0.0f);
                std::fill(scratchR_.begin(), scratchR_.begin() + frames, 0.0f);
                s.early.Process(plan_, nullptr, zeros_.data(), frames, scratchL_.data(), scratchR_.data());
                continue;
            }
            const float* x = in[ch];
            const size_t mask = static_cast<size_t>(feedSize_ - 1);
            for (int i = 0; i < frames; ++i) {
                s.feed[(s.writePos + static_cast<uint64_t>(i)) & mask] = x[i];
                tailIn_[static_cast<size_t>(i)] += x[i];
            }
            RenderTap(s, 0, sr.direct, rsp.cut, frames, outL, outR);
            for (int t = 0; t < sr.tapCount; ++t) RenderTap(s, 1 + t, sr.taps[t], rsp.cut, frames, outL, outR);
            for (int t = sr.tapCount; t < kMaxLiveTaps; ++t) FadeOutTap(s, 1 + t, frames, outL, outR);
            s.writePos += static_cast<uint64_t>(frames);
            s.early.Process(plan_, sr.early.get(), x, frames, outL, outR);
            s.fresh = false;
        }
        tail_.Process(plan_, rsp.tail.get(), tailIn_.data(), frames, outL, outR);
    }

private:
    static constexpr float kSplit[3] = {250.0f, 1000.0f, 4000.0f};

    struct EarState {
        float delay = 0.0f;                 // seconds
        float gain[kBands] = {};
        float l0 = 0.0f, l1 = 0.0f, l2 = 0.0f;
    };
    struct TapState {
        EarState ear[2];
        bool primed = false;
    };
    struct SourceState {
        std::vector<float> feed, mono;
        uint64_t writePos = 0;
        StreamConvolver early;
        TapState tap[1 + kMaxLiveTaps];
        bool fresh = true;
        int tapsSeen = 0;
    };

    // Reads one tap from the source's delay line into both ears, ramping the
    // delay and band gains from their previous values over the block.
    void RenderTap(SourceState& s, int slot, const LiveTap& tap, bool cut, int frames,
                   float* outL, float* outR) noexcept {
        TapState& ts = s.tap[slot];
        const size_t mask = static_cast<size_t>(feedSize_ - 1);
        float* outs[2] = {outL, outR};
        for (int e = 0; e < 2; ++e) {
            EarState& es = ts.ear[e];
            const float targetDelay = (std::max)(1.0f / sr_, tap.delaySeconds + tap.ears.delay[e]);
            float d0 = es.delay, g0[kBands];
            for (int b = 0; b < kBands; ++b) g0[b] = es.gain[b];
            if (!ts.primed) {
                d0 = targetDelay;
                for (int b = 0; b < kBands; ++b) g0[b] = tap.ears.band[e][b];
            } else if (cut) {
                d0 = targetDelay;   // a new room: jump the delay, glide the gains
            }
            const float inv = 1.0f / static_cast<float>(frames);
            for (int i = 0; i < frames; ++i) {
                const float u = static_cast<float>(i + 1) * inv;
                const float delaySamples = (d0 + (targetDelay - d0) * u) * sr_;
                const double pos = static_cast<double>(s.writePos + static_cast<uint64_t>(i)) - delaySamples;
                const double fl = std::floor(pos);
                const float fr = static_cast<float>(pos - fl);
                const int64_t i0 = static_cast<int64_t>(fl);
                const float a = s.feed[static_cast<size_t>(i0) & mask];
                const float b2 = s.feed[static_cast<size_t>(i0 + 1) & mask];
                const float x = pos < 0.0 ? 0.0f : a + (b2 - a) * fr;
                es.l0 += alpha_[0] * (x - es.l0);
                es.l1 += alpha_[1] * (x - es.l1);
                es.l2 += alpha_[2] * (x - es.l2);
                float g[kBands];
                for (int b = 0; b < kBands; ++b) g[b] = g0[b] + (tap.ears.band[e][b] - g0[b]) * u;
                outs[e][i] += g[0] * es.l0 + g[1] * (es.l1 - es.l0) + g[2] * (es.l2 - es.l1) + g[3] * (x - es.l2);
            }
            es.delay = targetDelay;
            for (int b = 0; b < kBands; ++b) es.gain[b] = tap.ears.band[e][b];
        }
        ts.primed = true;
    }

    // A tap that disappeared from the response fades to nothing instead of cutting off.
    void FadeOutTap(SourceState& s, int slot, int frames, float* outL, float* outR) noexcept {
        TapState& ts = s.tap[slot];
        if (!ts.primed) return;
        LiveTap gone;
        gone.delaySeconds = ts.ear[0].delay;
        RenderTap(s, slot, gone, false, frames, outL, outR);
        bool silent = true;
        for (int e = 0; e < 2; ++e) for (float g : ts.ear[e].gain) silent = silent && g == 0.0f;
        if (silent) ts.primed = false;
    }

    float sr_ = 48000.0f;
    int partition_ = 512, feedSize_ = 1, maxFrames_ = 0;
    float alpha_[3] = {};
    FftPlan plan_;
    SourceState src_[kMaxSources];
    StreamConvolver tail_;
    std::vector<float> tailIn_, zeros_, scratchL_, scratchR_;
    std::atomic<const RoomResponse*> current_{nullptr};
    std::atomic<const RoomResponse*> inUse_{nullptr};
    std::vector<std::shared_ptr<const RoomResponse>> owned_;   // control thread only
};

} // namespace stage
} // namespace svms
