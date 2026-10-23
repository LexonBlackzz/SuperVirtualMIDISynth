#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "render/RenderEvent.h"
#include "synth/StandaloneSynth.h"

namespace svms {

// Key-sharded engine cores for the native sessions (SVMS_*SessionConfig::shards),
// the session twin of the driver's synth.shards. Core 0 is the session's own
// StandaloneSynth; cores 1..N-1 are full copies (own SoundFont load, voice pool,
// renderer). Note events go to the core that owns (channel,key); everything else
// is broadcast. Cores render raw mixes in parallel, the sum gets reverb and the
// limiter once on core 0. The serial plan loop (note launch/steal) is what bounds
// Black MIDI speed, and this splits it.
// ponytail: every core loads its own SoundFont copy; share one bundle if RAM bites.
class ShardCores {
public:
    ~ShardCores() { StopWorkers(); }

    uint32_t Count() const { return count_; }
    bool Active() const { return count_ > 1u; }

    static uint32_t Owner(uint32_t channel, uint32_t note, uint32_t n) {
        return (((channel * 128u + note) * 2654435761u) >> 16u) % n;
    }
    static bool NoteLike(RenderEventType type) {
        return type == RenderEventType::NoteOn || type == RenderEventType::NoteOff ||
               type == RenderEventType::NoteReExcite ||
               type == RenderEventType::StaleNoteOffBatch;
    }
    // Packed 0x80/0x90 channel messages route to one core, everything else to all.
    bool Wants(uint32_t index, uint32_t message) const {
        const uint8_t status = static_cast<uint8_t>(message);
        const uint8_t type = status & 0xf0u;
        if (count_ < 2u || (type != 0x80u && type != 0x90u) ||
            IsInternalEngineMessage(message))
            return true;
        return Owner(status & 0x0fu, (message >> 8u) & 0x7fu, count_) == index;
    }
    bool Wants(uint32_t index, const RenderEvent& e) const {
        return count_ < 2u || !NoteLike(e.type) ||
               Owner(e.channel, e.data1, count_) == index;
    }

    // Initializes `primary` and the extra cores. n <= 1 initializes only `primary`
    // with the config unchanged. Pool and render threads are split across cores.
    bool Init(StandaloneSynth& primary, StandaloneSynthConfig config, uint32_t n,
              uint32_t planeFrames, std::string& error) {
#if defined(SVMS_XP_COMPAT)
        n = 1u;  // no worker threads on the XP build
#endif
        count_ = (std::max)(1u, (std::min)(n, 64u));
        if (count_ < 2u) return primary.Initialize(config, error);
        config.maxVoices = (std::max)(1u, (config.maxVoices + count_ - 1u) / count_);
        config.renderThreads = (std::max)(1u, (config.renderThreads
            ? config.renderThreads
            : (std::max)(1u, std::thread::hardware_concurrency())) / count_);
        extra_.resize(count_ - 1u);
        std::vector<std::string> errors(count_);
        std::vector<char> ok(count_, 0);
        std::vector<std::thread> loaders;
        for (uint32_t i = 0u; i < count_; ++i)
            loaders.emplace_back([&, i] {
                StandaloneSynth* s = &primary;
                if (i != 0u) {
                    extra_[i - 1u] = std::make_unique<StandaloneSynth>();
                    s = extra_[i - 1u].get();
                }
                ok[i] = s->Initialize(config, errors[i]);
            });
        for (auto& t : loaders) t.join();
        for (uint32_t i = 0u; i < count_; ++i)
            if (!ok[i]) { error = errors[i]; return false; }
        primary.SetRawMix(true);
        planeL_.resize(count_);
        planeR_.resize(count_);
        for (uint32_t i = 0u; i < count_; ++i) {
            if (i != 0u) extra_[i - 1u]->SetRawMix(true);
            planeL_[i].assign(planeFrames, 0.0f);
            planeR_[i].assign(planeFrames, 0.0f);
        }
        for (uint32_t i = 1u; i < count_; ++i)
            workers_.emplace_back([this, i] { WorkerLoop(i); });
        return true;
    }

    StandaloneSynth& At(StandaloneSynth& primary, uint32_t i) {
        return i == 0u ? primary : *extra_[i - 1u];
    }
    const StandaloneSynth& At(const StandaloneSynth& primary, uint32_t i) const {
        return i == 0u ? primary : *extra_[i - 1u];
    }
    template <class F> void ForEachCore(StandaloneSynth& primary, F&& f) {
        f(primary);
        for (auto& s : extra_) f(*s);
    }
    template <class F> uint64_t Sum(const StandaloneSynth& primary, F&& f) const {
        uint64_t total = f(primary);
        for (auto& s : extra_) total += f(*s);
        return total;
    }

    // Runs fn(i) for every core; core 0 on the caller, the rest on workers.
    void Run(const std::function<void(uint32_t)>& fn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &fn;
            pending_ = static_cast<uint32_t>(workers_.size());
            ++generation_;
        }
        wake_.notify_all();
        fn(0u);
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return pending_ == 0u; });
        job_ = nullptr;
    }

    float* PlaneL(uint32_t i) { return planeL_[i].data(); }
    float* PlaneR(uint32_t i) { return planeR_[i].data(); }
    uint32_t PlaneFrames() const {
        return planeL_.empty() ? 0u : static_cast<uint32_t>(planeL_[0].size());
    }
    // Adds cores 1..N-1 planes onto the first `frames` of left/right.
    void SumPlanes(float* left, float* right, uint32_t frames) {
        for (uint32_t i = 1u; i < count_; ++i)
            for (uint32_t f = 0u; f < frames; ++f) {
                left[f] += planeL_[i][f];
                right[f] += planeR_[i][f];
            }
    }

    // One block of render events: every core renders its own subset (raw) in
    // parallel; the summed mix lands in left/right, post-processed by `primary`.
    void RenderEvents(StandaloneSynth& primary, const RenderEvent* events,
                      uint32_t count, float* left, float* right, uint32_t frames,
                      uint64_t frame) {
        if (mine_.size() != count_) mine_.resize(count_);
        Run([&](uint32_t index) {
            auto& mine = mine_[index];
            mine.clear();
            for (uint32_t i = 0u; i < count; ++i)
                if (Wants(index, events[i])) mine.push_back(events[i]);
            float* l = index == 0u ? left : planeL_[index].data();
            float* r = index == 0u ? right : planeR_[index].data();
            At(primary, index).RenderWithEvents(
                mine.data(), static_cast<uint32_t>(mine.size()), l, r, frames, frame);
        });
        SumPlanes(left, right, frames);
        primary.PostProcess(left, right, frames);
    }

private:
    void WorkerLoop(uint32_t index) {
        uint64_t seen = 0u;
        for (;;) {
            const std::function<void(uint32_t)>* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
                job = job_;
            }
            (*job)(index);
            std::lock_guard<std::mutex> lock(mutex_);
            if (--pending_ == 0u) done_.notify_one();
        }
    }
    void StopWorkers() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        wake_.notify_all();
        for (auto& t : workers_) t.join();
        workers_.clear();
    }

    uint32_t count_ = 1u;
    std::vector<std::unique_ptr<StandaloneSynth>> extra_;
    std::vector<std::vector<float>> planeL_, planeR_;
    std::vector<std::vector<RenderEvent>> mine_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    const std::function<void(uint32_t)>* job_ = nullptr;
    uint64_t generation_ = 0u;
    uint32_t pending_ = 0u;
    bool stop_ = false;
};

}  // namespace svms
