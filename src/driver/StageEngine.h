#pragma once

// Driver-side owner of the 3D stage: builds the room response on a background
// thread, then renders it from the audio callback. Off by default; when it is
// off (or still building) the driver mixes exactly as before.

#include "render/StageBuilder.h"
#include "render/StageTracerVulkan.h"
#include "synth/StageConfig.h"

#include <atomic>
#include <memory>
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
        sr_ = static_cast<float>(sampleRate);
        stage::BuildOptions opt = Options();
        const int earlyParts = static_cast<int>(std::ceil(opt.earlySeconds * sr_ / opt.partition)) + 2;
        const int tailParts = static_cast<int>(std::ceil(12.5f * sr_ / opt.partition)) + 2;
        renderer_ = std::make_unique<stage::StageRenderer>();
        renderer_->Prepare(sr_, opt.partition, static_cast<int>(maxFrames), earlyParts, tailParts);
        for (auto& m : mono_) m.assign(maxFrames, 0.0f);
        maxFrames_ = maxFrames;
        ready_.store(false, std::memory_order_release);
        cancel_.store(false, std::memory_order_relaxed);
        worker_ = std::thread([this] { Build(); });
        return true;
    }

    void Stop() {
        cancel_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) worker_.join();
        ready_.store(false, std::memory_order_release);
        renderer_.reset();
    }

    bool Ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    bool KeepReverb() const noexcept { return cfg_.keepReverb; }

    // Audio thread. `busL/busR` are the (limited) per-channel buses; the stage
    // output is added to outL/outR. Returns false when the stage is not ready,
    // in which case the caller mixes the buses itself.
    bool Process(float* const* busL, float* const* busR, uint32_t frames, float* outL,
                 float* outR) noexcept {
        if (!Ready() || frames > maxFrames_ || !renderer_) return false;
        const float* in[stage::kMaxSources] = {};
        for (int c = 0; c < stage::kMaxSources; ++c) {
            if (!cfg_.channelOn[c]) continue;
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
    stage::BuildOptions Options() const {
        stage::BuildOptions o;
        o.sampleRate = sr_;
        o.partition = 512;
        o.rays = static_cast<int>(cfg_.rays);
        o.air = cfg_.airAbsorption;
        o.earModel = cfg_.earModel;
        o.tailTrim = cfg_.tailTrim;
        o.cancel = &cancel_;
        return o;
    }

    stage::StageScene Scene() const {
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

    void Build() {
        // Ray queries on the GPU when a device has them (about 75x faster); the
        // CPU tracer otherwise.
        std::string why;
        std::unique_ptr<stage::TraceBackend> gpu = stage::CreateVulkanTraceBackend(why);
        stage::CpuTracer cpu;
        stage::TraceBackend& tracer = gpu ? *gpu : static_cast<stage::TraceBackend&>(cpu);
        stage::EarModelSpatializer ear;
        ear.enabled = cfg_.earModel;
        const stage::BuildOptions opt = Options();
        const stage::StageScene scene = Scene();
        stage::RoomMeta meta;
        auto tail = stage::BuildTailSpectra(renderer_->Plan(), scene, tracer, opt, &meta);
        if (cancel_.load(std::memory_order_relaxed)) return;
        renderer_->Submit(stage::BuildPaths(scene, ear, opt, renderer_->Plan(), std::move(tail), meta, 1, true));
        ready_.store(true, std::memory_order_release);
    }

    StageConfig cfg_;
    float sr_ = 48000.0f;
    uint32_t maxFrames_ = 0;
    std::unique_ptr<stage::StageRenderer> renderer_;
    std::vector<float> mono_[stage::kMaxSources];
    std::atomic<bool> ready_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

} // namespace svms
