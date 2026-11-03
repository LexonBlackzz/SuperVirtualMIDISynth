#pragma once

// Driver-side owner of the 3D stage: builds the room response on a background
// thread, then renders it from the audio callback. Off by default; when it is
// off (or still building) the driver mixes exactly as before. Update() hands
// it new settings while it runs: the thread rebuilds the tail when the room
// changed and only the paths when something merely moved.

#include "render/StageBuilder.h"
#include "render/StageTracerVulkan.h"
#include "synth/StageConfig.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace svms {

class StageEngine {
public:
    ~StageEngine() { Stop(); }

    // Allocates the renderer and starts building the response. Control thread.
    bool Start(const StageConfig& cfg, uint32_t sampleRate, uint32_t maxFrames) {
        Stop();
        if (sampleRate == 0u || maxFrames == 0u) return false;
        cfg_ = cfg;
        pending_ = cfg;
        keepReverb_.store(cfg.keepReverb, std::memory_order_relaxed);
        enabled_.store(cfg.enabled, std::memory_order_relaxed);
        mask_.store(MaskOf(cfg), std::memory_order_relaxed);
        sr_ = static_cast<float>(sampleRate);
        stage::BuildOptions opt = Options(cfg, nullptr);
        const int earlyParts = static_cast<int>(std::ceil(opt.earlySeconds * sr_ / opt.partition)) + 2;
        const int tailParts = static_cast<int>(std::ceil(12.5f * sr_ / opt.partition)) + 2;
        renderer_ = std::make_unique<stage::StageRenderer>();
        renderer_->Prepare(sr_, opt.partition, static_cast<int>(maxFrames), earlyParts, tailParts);
        for (auto& m : mono_) m.assign(maxFrames, 0.0f);
        maxFrames_ = maxFrames;
        ready_.store(false, std::memory_order_release);
        cancel_.store(false, std::memory_order_relaxed);
        stop_.store(false, std::memory_order_relaxed);
        dirty_.store(cfg.enabled, std::memory_order_relaxed);
        worker_ = std::thread([this] { Run(); });
        return true;
    }

    // Any thread. New settings replace the pending ones; a build in flight is
    // abandoned and started again from them.
    void Update(const StageConfig& cfg) {
        {
            std::lock_guard<std::mutex> g(lock_);
            pending_ = cfg;
        }
        mask_.store(MaskOf(cfg), std::memory_order_relaxed);
        enabled_.store(cfg.enabled, std::memory_order_relaxed);
        if (cfg.enabled) {
            cancel_.store(true, std::memory_order_relaxed);
            dirty_.store(true, std::memory_order_release);
        }
    }

    void Stop() {
        stop_.store(true, std::memory_order_relaxed);
        cancel_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) worker_.join();
        ready_.store(false, std::memory_order_release);
        renderer_.reset();
    }

    bool Ready() const noexcept {
        return ready_.load(std::memory_order_acquire) && enabled_.load(std::memory_order_relaxed);
    }
    bool KeepReverb() const noexcept { return keepReverb_.load(std::memory_order_relaxed); }

    // Audio thread. `busL/busR` are the (limited) per-channel buses; the stage
    // output is added to outL/outR. Returns false when the stage is not ready,
    // in which case the caller mixes the buses itself.
    bool Process(float* const* busL, float* const* busR, uint32_t frames, float* outL,
                 float* outR) noexcept {
        if (!Ready() || frames > maxFrames_ || !renderer_) return false;
        const float* in[stage::kMaxSources] = {};
        const uint32_t bits = mask_.load(std::memory_order_relaxed);
        for (int c = 0; c < stage::kMaxSources; ++c) {
            if (!((bits >> c) & 1u)) continue;
            float* m = mono_[static_cast<size_t>(c)].data();
            const float* l = busL[c];
            const float* r = busR[c];
            for (uint32_t i = 0; i < frames; ++i) m[i] = 0.5f * (l[i] + r[i]);
            in[c] = m;
        }
        renderer_->Process(in, stage::kMaxSources, static_cast<int>(frames), outL, outR);
        return true;
    }

private:
    static uint32_t MaskOf(const StageConfig& c) {
        uint32_t m = 0;
        for (int i = 0; i < stage::kMaxSources; ++i)
            if (c.channelOn[i]) m |= 1u << i;
        return m;
    }

    // What changes the tail. Positions, the listener and the mute mask only move paths.
    static bool SameRoom(const StageConfig& a, const StageConfig& b) {
        if (a.roomWidth != b.roomWidth || a.roomDepth != b.roomDepth || a.roomHeight != b.roomHeight ||
            a.rays != b.rays || a.airAbsorption != b.airAbsorption || a.earModel != b.earModel ||
            a.tailTrim != b.tailTrim || a.furnitureCount != b.furnitureCount)
            return false;
        for (int i = 0; i < 6; ++i)
            if (a.surface[i] != b.surface[i]) return false;
        for (uint32_t i = 0; i < a.furnitureCount; ++i)
            if (a.furniture[i].kind != b.furniture[i].kind || a.furniture[i].x != b.furniture[i].x ||
                a.furniture[i].y != b.furniture[i].y || a.furniture[i].rotated != b.furniture[i].rotated)
                return false;
        return true;
    }

    stage::BuildOptions Options(const StageConfig& cfg_, const std::atomic<bool>* cancel) const {
        stage::BuildOptions o;
        o.sampleRate = sr_;
        o.partition = 512;
        o.rays = static_cast<int>(cfg_.rays);
        o.air = cfg_.airAbsorption;
        o.earModel = cfg_.earModel;
        o.tailTrim = cfg_.tailTrim;
        o.cancel = cancel;
        return o;
    }

    static stage::StageScene Scene(const StageConfig& cfg_) {
        stage::StageScene s;
        s.room.width = cfg_.roomWidth;
        s.room.depth = cfg_.roomDepth;
        s.room.height = cfg_.roomHeight;
        for (int w = 0; w < 6; ++w) s.room.surface[w] = static_cast<stage::Material>(cfg_.surface[w]);
        for (uint32_t i = 0; i < cfg_.furnitureCount; ++i) {
            stage::Furniture f;
            f.kind = static_cast<stage::FurnitureKind>(cfg_.furniture[i].kind);
            f.x = cfg_.furniture[i].x;
            f.y = cfg_.furniture[i].y;
            f.rotated = cfg_.furniture[i].rotated;
            s.room.furniture.push_back(f);
        }
        s.listener = {cfg_.listener[0], cfg_.listener[1], cfg_.listener[2]};
        s.yawDegrees = cfg_.listenerYaw;
        for (int c = 0; c < stage::kMaxSources; ++c) {
            s.source[c] = {cfg_.channel[c][0], cfg_.channel[c][1], cfg_.channel[c][2]};
            s.sourceOn[c] = cfg_.channelOn[c];
        }
        return s;
    }

    void Run() {
        // Ray queries on the GPU when a device has them (about 75x faster); the
        // CPU tracer otherwise.
        std::string why;
        std::unique_ptr<stage::TraceBackend> gpu = stage::CreateVulkanTraceBackend(why);
        stage::CpuTracer cpu;
        stage::TraceBackend& tracer = gpu ? *gpu : static_cast<stage::TraceBackend&>(cpu);
        std::shared_ptr<const stage::IrSpectra> tail;
        stage::RoomMeta meta;
        StageConfig last;
        bool haveLast = false;
        uint64_t serial = 1;
        while (!stop_.load(std::memory_order_relaxed)) {
            if (!dirty_.exchange(false, std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }
            cancel_.store(false, std::memory_order_relaxed);
            StageConfig cur;
            {
                std::lock_guard<std::mutex> g(lock_);
                cur = pending_;
            }
            stage::EarModelSpatializer ear;
            ear.enabled = cur.earModel;
            const stage::BuildOptions opt = Options(cur, &cancel_);
            const stage::StageScene scene = Scene(cur);
            if (!haveLast || !SameRoom(cur, last) || (!tail && MaskOf(cur) != 0u)) {
                stage::RoomMeta m2;
                auto t = stage::BuildTailSpectra(renderer_->Plan(), scene, tracer, opt, &m2);
                if (cancel_.load(std::memory_order_relaxed)) {
                    if (stop_.load(std::memory_order_relaxed)) return;
                    dirty_.store(true, std::memory_order_release);   // superseded: start over from the newest
                    continue;
                }
                tail = std::move(t);
                meta = m2;
                haveLast = false;
            }
            renderer_->Submit(stage::BuildPaths(scene, ear, opt, renderer_->Plan(), tail, meta, serial++, !haveLast));
            last = cur;
            haveLast = true;
            ready_.store(true, std::memory_order_release);
        }
    }

    StageConfig cfg_;
    StageConfig pending_;
    std::mutex lock_;
    std::atomic<bool> dirty_{false}, stop_{false}, enabled_{false}, keepReverb_{false};
    std::atomic<uint32_t> mask_{0};
    float sr_ = 48000.0f;
    uint32_t maxFrames_ = 0;
    std::unique_ptr<stage::StageRenderer> renderer_;
    std::vector<float> mono_[stage::kMaxSources];
    std::atomic<bool> ready_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

} // namespace svms
