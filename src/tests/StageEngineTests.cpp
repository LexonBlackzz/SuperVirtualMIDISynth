#include "driver/StageEngine.h"
#include "synth/StageWire.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using namespace svms;
using namespace svms::stage;

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

bool WaitReady(const StageEngine& e, int seconds) {
    for (int i = 0; i < seconds * 20 && !e.Ready(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return e.Ready();
}

// Not ready: Process refuses, so the driver keeps mixing the buses itself.
// Ready: an impulse on one bus comes out as sound after the direct delay.
void TestEngine() {
    StageConfig cfg;
    cfg.enabled = true;
    cfg.rays = 8000u;
    cfg.channelOn[3] = false;
    StageEngine engine;
    const uint32_t frames = 1024;
    CHECK(engine.Start(cfg, 48000u, frames));
    std::vector<float> bus[16][2];
    float* busL[16];
    float* busR[16];
    for (int c = 0; c < 16; ++c) {
        bus[c][0].assign(frames, 0.0f);
        bus[c][1].assign(frames, 0.0f);
        busL[c] = bus[c][0].data();
        busR[c] = bus[c][1].data();
    }
    std::vector<float> outL(frames, 0.0f), outR(frames, 0.0f);
    if (!engine.Ready()) CHECK(!engine.Process(busL, busR, frames, outL.data(), outR.data()));
    CHECK(WaitReady(engine, 60));
    bus[0][0][0] = 1.0f;
    bus[0][1][0] = 1.0f;
    bus[3][0][0] = 1.0f;   // a channel that is off must stay out of the stage
    double energy = 0.0;
    for (int block = 0; block < 6; ++block) {
        std::fill(outL.begin(), outL.end(), 0.0f);
        std::fill(outR.begin(), outR.end(), 0.0f);
        CHECK(engine.Process(busL, busR, frames, outL.data(), outR.data()));
        for (uint32_t i = 0; i < frames; ++i) {
            CHECK(std::isfinite(outL[i]) && std::isfinite(outR[i]));
            energy += static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i];
        }
        bus[0][0][0] = bus[0][1][0] = bus[3][0][0] = 0.0f;
    }
    CHECK(energy > 0.0);
    engine.Stop();
    CHECK(!engine.Ready());
    // Stopping while a build runs must not hang.
    CHECK(engine.Start(cfg, 48000u, frames));
    engine.Stop();
}

// Energy of eight blocks that start with an impulse on channel 0.
double Pulse(StageEngine& engine, uint32_t frames) {
    std::vector<float> bus[16][2];
    float* busL[16];
    float* busR[16];
    for (int c = 0; c < 16; ++c) {
        bus[c][0].assign(frames, 0.0f);
        bus[c][1].assign(frames, 0.0f);
        busL[c] = bus[c][0].data();
        busR[c] = bus[c][1].data();
    }
    std::vector<float> outL(frames), outR(frames);
    double energy = 0.0;
    bus[0][0][0] = bus[0][1][0] = 1.0f;
    for (int block = 0; block < 8; ++block) {
        std::fill(outL.begin(), outL.end(), 0.0f);
        std::fill(outR.begin(), outR.end(), 0.0f);
        if (!engine.Process(busL, busR, frames, outL.data(), outR.data())) return -1.0;
        for (uint32_t i = 0; i < frames; ++i)
            energy += static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i];
        bus[0][0][0] = bus[0][1][0] = 0.0f;
    }
    return energy;
}

// A stage started off turns on, and a changed room changes the sound, without a restart.
void TestLiveUpdate() {
    StageConfig cfg;
    cfg.rays = 8000u;
    cfg.enabled = false;
    StageEngine engine;
    const uint32_t frames = 1024;
    CHECK(engine.Start(cfg, 48000u, frames));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!engine.Ready());
    cfg.enabled = true;
    engine.Update(cfg);
    CHECK(WaitReady(engine, 60));
    const double before = Pulse(engine, frames);
    CHECK(before > 0.0);
    cfg.tailTrim = 3.0f;
    cfg.roomWidth = 40.0f;
    cfg.roomDepth = 60.0f;
    engine.Update(cfg);
    double after = before;
    for (int i = 0; i < 1200 && std::fabs(after - before) < 0.05 * before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        after = Pulse(engine, frames);
    }
    CHECK(std::fabs(after - before) >= 0.05 * before);
    // Switched off live: the driver mixes the buses itself again.
    cfg.enabled = false;
    engine.Update(cfg);
    CHECK(!engine.Ready());
    engine.Stop();
}

// Settings survive the trip through the command text parts.
void TestWire() {
    StageConfig a;
    a.enabled = true;
    a.rays = 120000u;
    a.tailTrim = 0.3f;
    a.roomWidth = 31.5f;
    a.surface[2] = 9;
    a.listener[1] = 17.25f;
    a.listenerYaw = -40.0f;
    a.channel[11][2] = 4.5f;
    a.channelOn[12] = false;
    a.furnitureCount = 11;
    for (uint32_t i = 0; i < a.furnitureCount; ++i) a.furniture[i] = {static_cast<uint8_t>(i % 5), 1.0f + i, 2.5f * i, (i & 1u) != 0u};
    StageConfig b;
    for (uint32_t part = 0; part < StageWirePartCount(a); ++part) {
        char text[348] = {};
        CHECK(EncodeStagePart(a, part, text, sizeof(text)));
        CHECK(DecodeStagePart(b, part, text));
    }
    CHECK(b.enabled && b.rays == 120000u && std::fabs(b.tailTrim - 0.3f) < 1e-4f);
    CHECK(std::fabs(b.roomWidth - 31.5f) < 1e-2f && b.surface[2] == 9);
    CHECK(std::fabs(b.listener[1] - 17.25f) < 1e-2f && std::fabs(b.listenerYaw + 40.0f) < 1e-2f);
    CHECK(std::fabs(b.channel[11][2] - 4.5f) < 1e-2f && !b.channelOn[12] && b.channelOn[0]);
    CHECK(b.furnitureCount == 11);
    CHECK(b.furniture[10].kind == 0 && std::fabs(b.furniture[10].y - 25.0f) < 1e-2f && !b.furniture[10].rotated);
    CHECK(b.furniture[9].kind == 4 && b.furniture[9].rotated);
}

// The traced tail must carry energy at any room size: a larger room spreads the same rays
// over more volume, and a tail with no energy is a silent reverb.
void TestRoomScales() {
    FftPlan plan;
    plan.Init(1024);
    CpuTracer cpu;
    for (float scale : {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
        StageScene scene;
        scene.room.width *= scale;
        scene.room.depth *= scale;
        scene.room.height *= scale;
        scene.listener = {scene.room.width * 0.5f, scene.room.depth * 0.35f, 1.2f};
        for (int c = 0; c < 4; ++c) {
            scene.source[c] = {scene.room.width * (0.3f + 0.13f * c), scene.room.depth * 0.7f, 1.5f};
            scene.sourceOn[c] = true;
        }
        BuildOptions opt;
        opt.rays = 20000;
        RoomMeta meta;
        const auto tail = BuildTailSpectra(plan, scene, cpu, opt, &meta);
        double energy = 0.0;
        if (tail)
            for (float v : tail->re[0]) energy += static_cast<double>(v) * v;
        std::printf("scale %.1f: tail %s energy %.3g t60 %.2f %.2f %.2f %.2f seconds %.2f\n", scale,
                    tail ? "yes" : "NO", energy, meta.t60[0], meta.t60[1], meta.t60[2], meta.t60[3], meta.tailSeconds);
        CHECK(tail != nullptr);
        CHECK(energy > 0.0 && std::isfinite(energy));
    }
}

} // namespace

int main() {
    TestRoomScales();
    TestEngine();
    TestWire();
    TestLiveUpdate();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("stage engine tests passed\n");
    return 0;
}
