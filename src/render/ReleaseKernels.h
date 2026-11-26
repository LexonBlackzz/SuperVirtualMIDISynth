#pragma once

// Release-bus kernels, built from the SoundFont. A kernel is one region played on one key, held, at velocity gain 1
// and unity channel mix: the sample read at the key's pitch, through the region's static filter, times its
// envelope, attenuation and pan. Velocity and the channel mix (volume, expression, pan) are plain gains in the voice
// path, so the bus applies them outside the kernel.
//
// RenderHeld repeats the per-frame arithmetic of the scalar voice loop (renderscalar/Core.inl) for one voice that is
// never released: the same cursor, interpolation, envelope stages and filter. ReleaseKernelTests holds the two
// together against notes rendered by the engine.
//
// For a note held d frames, the voice's release is A(tau) * r^(tau - d), with A the held output and r the region's
// per-frame release decay. That is the held output times a decay only if A's envelope no longer moves after d, so a
// region whose envelope ramps (delay, attack, or a decay to a sustain below full) is refused, as is one whose loop
// ends at release (sampleModes 3). Such notes stay voices.
//
// The store builds kernels lazily on its own thread, under a memory budget; in that mode Get never blocks or
// allocates. Offline renders build inline instead, so the output does not depend on thread timing.

#include "render/Envelope.h"
#include "render/ReleaseConvolver.h"
#include "render/RenderScalar.h"
#include "render/VoiceFilter.h"
#include "synth/PhaseFx.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace svms {
namespace bus {

// What the voice path reads for one region on one key, as StandaloneSynth::ReleaseKernelSource fills it.
struct KernelSource {
    const int16_t* samples = nullptr;   // the synth's sample store
    uint32_t sampleFrames = 0;
    uint32_t sampleStart = 0, sampleEnd = 0, loopStart = 0, loopEnd = 0;   // absolute frames in the store
    uint8_t loopMode = 0;               // SF2 sampleModes
    float phaseStep = 0.0f;             // source frames per output frame at this key, no bend
    float gain = 0.0f;                  // region attenuation (the velocity gain is 1)
    float panL = 0.0f, panR = 0.0f;     // region pan
    uint32_t delay = 0, hold = 0, attack = 0, decay = 0;   // frames
    float decaySlope = 1.0f, sustain = 1.0f;               // per frame; sustain as a fraction of gain
    float releaseDecay = 0.0f;          // r
    uint32_t releaseFrames = 0;         // release length the voice counts
    PreparedVoiceFilter filter{};
};

inline uint64_t KernelKey(uint32_t font, uint32_t region, uint8_t note) noexcept {
    return (static_cast<uint64_t>(font) << 40) | (static_cast<uint64_t>(region) << 8) | note;
}

// Why the release cannot be the held output times r^tau, or nullptr when it can.
inline const char* KernelRefusal(const KernelSource& s) noexcept {
    if (!s.samples || s.sampleEnd <= s.sampleStart + 1u || s.sampleEnd > s.sampleFrames) return "no sample";
    if (s.loopMode == 3u) return "loop ends at release";
    if (s.delay || s.attack || (s.decay && s.sustain < 1.0f)) return "envelope moves after note-on";
    if (!(s.releaseDecay > 0.0f && s.releaseDecay < 1.0f) || s.releaseFrames < 2u) return "no release decay";
    return nullptr;
}

// The held voice's output for tau < frames, added to nothing: left and right are overwritten.
inline void RenderHeld(const KernelSource& s, uint32_t frames, float* left, float* right) {
    std::fill(left, left + frames, 0.0f);
    std::fill(right, right + frames, 0.0f);
    if (!s.samples || s.sampleEnd <= s.sampleStart) return;
    const uint32_t relEnd = s.sampleEnd - s.sampleStart;
    const uint32_t relLoopS = s.loopStart > s.sampleStart ? s.loopStart - s.sampleStart : 0u;
    const uint32_t relLoopE = s.loopEnd > s.sampleStart ? s.loopEnd - s.sampleStart : 0u;
    const bool loop = (s.loopMode == 1u || s.loopMode == 3u) && s.loopEnd > s.loopStart + 1u;
    const uint64_t loopS64 = phasefx::Pack(relLoopS, 0u), loopE64 = phasefx::Pack(relLoopE, 0u);
    const uint64_t step = phasefx::FromFloat(s.phaseStep);
    const float target = s.gain, sustainLevel = s.sustain * s.gain;
    const float attackStep = s.attack ? s.gain / static_cast<float>(s.attack) : 0.0f;
    uint32_t delay = s.delay, hold = s.hold, attack = s.attack, decay = s.decay;
    // Launch state (VoiceManager launch): stage 4 delay, 0 hold, 1 attack, 2 decay, 3 sustain.
    uint8_t stage = delay ? 4u : hold ? 0u : attack ? 1u : decay ? 2u : 3u;
    float gain = (stage >= 2u && stage <= 3u) ? s.gain : 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
    uint64_t pos = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        uint32_t base = phasefx::Index(pos);
        if (base + 1u >= relEnd) {
            if (!loop) return;
            pos = loopS64;
            base = relLoopS;
        }
        uint32_t next = base + 1u;
        if (loop && next >= relLoopE) next = relLoopS;
        if (next >= relEnd) next = relEnd - 1u;
        if (s.sampleStart + next >= s.sampleFrames) return;
        float sample = InterpolateSample(s.samples, s.sampleStart + base, s.sampleStart + next,
                                         phasefx::Weight(phasefx::Frac(pos)));
        if (stage != 3u) {
            if (stage == 4u) { if (delay) { --delay; gain = 0.0f; } else stage = 0u; }
            if (stage == 0u) { if (hold) { --hold; gain = target; } else stage = 1u; }
            if (stage == 1u) {
                if (attack) { gain += attackStep; --attack; if (gain > target) gain = target; }
                else gain = target;
                if (!attack) stage = decay ? 2u : 3u;
            }
            if (stage == 2u) {
                if (decay) { gain *= s.decaySlope; --decay; if (gain < sustainLevel) gain = sustainLevel; }
                else gain = sustainLevel;
                if (!decay) stage = 3u;
            }
        }
        pos += step;
        if (loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
        if (s.filter.enabled) {   // ProcessVoiceFilterSample
            const float out = sample * s.filter.a0 + z1;
            z1 = sample * (2.0f * s.filter.a0) + z2 - s.filter.b1 * out;
            z2 = sample * s.filter.a0 - s.filter.b2 * out;
            sample = out;
        }
        const float scaled = sample * gain;
        left[f] = scaled * s.panL;
        right[f] = scaled * s.panR;
    }
}

struct ReleaseKernel {
    uint32_t heldFrames = 0;           // A covers tau < heldFrames (the bus window)
    std::vector<float> heldL, heldR;   // A, for the held-part correction
    ConvKernel tail;                   // B(tau) = A(tau) * r^tau for tau < heldFrames + releaseFrames
    float releaseDecay = 0.0f;
    size_t bytes = 0;
};

// The regions a note plays at once (a stereo pair, layers) can share one kernel when they release at the same
// rate: the kernel is the sum of their held outputs. SetRefusal says why a set cannot, or returns nullptr.
inline const char* SetRefusal(const std::vector<KernelSource>& set) noexcept {
    if (set.empty()) return "no regions";
    for (const KernelSource& s : set) {
        if (const char* why = KernelRefusal(s)) return why;
        if (s.releaseDecay != set[0].releaseDecay || s.releaseFrames != set[0].releaseFrames)
            return "regions release at different rates";
    }
    return nullptr;
}

inline std::unique_ptr<ReleaseKernel> BuildKernel(const std::vector<KernelSource>& set, uint32_t heldFrames,
                                                  int block, int maxLevels = 3) {
    const KernelSource& s = set[0];
    const uint32_t length = heldFrames + s.releaseFrames;
    std::vector<float> l(length, 0.0f), r(length, 0.0f), one(length), oneR(length);
    for (const KernelSource& region : set) {
        RenderHeld(region, length, one.data(), oneR.data());
        for (uint32_t t = 0; t < length; ++t) { l[t] += one[t]; r[t] += oneR[t]; }
    }
    std::unique_ptr<ReleaseKernel> k(new ReleaseKernel());
    k->heldFrames = heldFrames;
    k->heldL.assign(l.begin(), l.begin() + heldFrames);
    k->heldR.assign(r.begin(), r.begin() + heldFrames);
    double p = 1.0;
    for (uint32_t t = 0; t < length; ++t) {
        l[t] = static_cast<float>(l[t] * p);
        r[t] = static_cast<float>(r[t] * p);
        p *= s.releaseDecay;
    }
    k->tail = MakeKernel(l.data(), r.data(), static_cast<int>(length), block, maxLevels);
    k->releaseDecay = s.releaseDecay;
    k->bytes = 2u * heldFrames * sizeof(float);
    for (const ConvLevel& L : k->tail.levels) k->bytes += L.h.size() * sizeof(Cf);
    return k;
}

inline std::unique_ptr<ReleaseKernel> BuildKernel(const KernelSource& s, uint32_t heldFrames, int block,
                                                  int maxLevels = 3) {
    return BuildKernel(std::vector<KernelSource>{s}, heldFrames, block, maxLevels);
}

class ReleaseKernelStore {
public:
    // Fills the sources (one per region the kernel sums) for a key; false if the key names nothing. Called on the
    // store's thread, or the caller's in inline mode.
    using SourceFn = std::function<bool(uint64_t key, std::vector<KernelSource>& sources)>;

    ReleaseKernelStore() = default;
    ReleaseKernelStore(const ReleaseKernelStore&) = delete;
    ReleaseKernelStore& operator=(const ReleaseKernelStore&) = delete;
    ~ReleaseKernelStore() { Stop(); }

    // background: build on the store's own thread (Get returns nullptr until the kernel is ready). Without it Get
    // builds a new kernel on the spot, so offline renders do not depend on thread timing.
    void Start(SourceFn source, uint32_t heldFrames, int block, size_t budgetBytes, int maxLevels = 3,
               bool background = true) {
        Stop();
        background_ = background;
        source_ = std::move(source);
        heldFrames_ = heldFrames;
        block_ = block;
        maxLevels_ = maxLevels;
        budget_ = budgetBytes;
        slots_.reset(new Slot[kSlots]);
        running_.store(true, std::memory_order_relaxed);
        if (background_) worker_ = std::thread([this] { Work(); });
    }

    // Joins the builder and frees every kernel. Call before the sample store changes, with no Get in flight.
    void Stop() {
        running_.store(false, std::memory_order_relaxed);
        if (worker_.joinable()) worker_.join();
        if (slots_) for (uint32_t i = 0; i < kSlots; ++i) delete slots_[i].kernel;
        slots_.reset();
        ffts_.clear();
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        used_ = 0;
        queued_.store(0, std::memory_order_relaxed);
        done_.store(0, std::memory_order_relaxed);
        bytes_.store(0, std::memory_order_relaxed);
        refused_.store(0, std::memory_order_relaxed);
    }

    // The kernel if it is built, else nullptr; a key seen for the first time is queued for building.
    // One caller thread at a time. In background mode it never blocks or allocates.
    const ReleaseKernel* Get(uint64_t key) noexcept {
        if (!slots_) return nullptr;
        const uint64_t tag = key + 1u;
        uint32_t i = Hash(key);
        for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = (i + 1u) & (kSlots - 1u)) {
            Slot& s = slots_[i];
            const uint64_t k = s.key.load(std::memory_order_acquire);
            if (k == tag)
                return s.state.load(std::memory_order_acquire) == kReady ? s.kernel : nullptr;
            if (k != 0u) continue;
            const uint32_t h = head_.load(std::memory_order_relaxed);
            if (used_ >= kSlots / 4u * 3u || h - tail_.load(std::memory_order_acquire) >= kRing) return nullptr;
            ++used_;
            s.state.store(kQueued, std::memory_order_relaxed);
            s.key.store(tag, std::memory_order_release);
            if (!background_) {
                Build(s);
                return s.state.load(std::memory_order_relaxed) == kReady ? s.kernel : nullptr;
            }
            ring_[h & (kRing - 1u)] = i;
            queued_.fetch_add(1u, std::memory_order_relaxed);
            head_.store(h + 1u, std::memory_order_release);
            return nullptr;
        }
        return nullptr;
    }

    // Blocks until every queued key is built or refused (tests, offline renders).
    void WaitIdle() const {
        while (running_.load(std::memory_order_relaxed) &&
               done_.load(std::memory_order_acquire) != queued_.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    size_t Bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }
    uint32_t Refused() const noexcept { return refused_.load(std::memory_order_relaxed); }
    uint64_t BuildNs() const noexcept { return buildNs_.load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kSlots = 1u << 16, kMaxProbe = 64u, kRing = 4096u;
    enum : uint32_t { kQueued = 1, kReady = 2, kRefused = 3 };
    struct Slot {
        std::atomic<uint64_t> key{0};      // key + 1; 0 = empty
        std::atomic<uint32_t> state{0};
        ReleaseKernel* kernel = nullptr;   // written by the builder before state turns kReady
    };

    static uint32_t Hash(uint64_t key) noexcept {
        key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 33;
        return static_cast<uint32_t>(key) & (kSlots - 1u);
    }

    void Work() {
        while (running_.load(std::memory_order_relaxed)) {
            const uint32_t t = tail_.load(std::memory_order_relaxed);
            if (t == head_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            Slot& s = slots_[ring_[t & (kRing - 1u)]];
            tail_.store(t + 1u, std::memory_order_release);
            Build(s);
            done_.fetch_add(1u, std::memory_order_release);
        }
    }

    void Build(Slot& s) {
        const auto t0 = std::chrono::steady_clock::now();
        struct Timer {
            std::atomic<uint64_t>& ns; std::chrono::steady_clock::time_point t0;
            ~Timer() { ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed); }
        } timer{buildNs_, t0};
        std::vector<KernelSource> src;
        const uint64_t key = s.key.load(std::memory_order_relaxed) - 1u;
        if (!source_(key, src) || SetRefusal(src)) return Refuse(s);
        std::unique_ptr<ReleaseKernel> k = BuildKernel(src, heldFrames_, block_, maxLevels_);
        if (bytes_.load(std::memory_order_relaxed) + k->bytes > budget_) return Refuse(s);
        for (ConvLevel& L : k->tail.levels) L.fft = SharedFft(L.fft);   // one twiddle table per size
        bytes_.fetch_add(k->bytes, std::memory_order_relaxed);
        s.kernel = k.release();
        s.state.store(kReady, std::memory_order_release);
    }

    void Refuse(Slot& s) {
        refused_.fetch_add(1u, std::memory_order_relaxed);
        s.state.store(kRefused, std::memory_order_release);
    }

    std::shared_ptr<const SplitFft> SharedFft(const std::shared_ptr<const SplitFft>& fft) {
        for (const auto& f : ffts_) if (f->Size() == fft->Size()) return f;
        ffts_.push_back(fft);
        return fft;
    }

    SourceFn source_;
    uint32_t heldFrames_ = 0;
    int block_ = 1024, maxLevels_ = 3;
    size_t budget_ = 0;
    std::unique_ptr<Slot[]> slots_;
    uint32_t ring_[kRing]{};
    std::atomic<uint32_t> head_{0}, tail_{0};   // ring of slot indices: Get pushes, the builder pops
    uint32_t used_ = 0;                          // slots taken (caller thread)
    std::atomic<uint32_t> queued_{0}, done_{0}, refused_{0};
    std::atomic<size_t> bytes_{0};
    std::atomic<uint64_t> buildNs_{0};
    std::vector<std::shared_ptr<const SplitFft>> ffts_;   // builder thread
    std::atomic<bool> running_{false};
    bool background_ = true;
    std::thread worker_;
};

}  // namespace bus
}  // namespace svms
