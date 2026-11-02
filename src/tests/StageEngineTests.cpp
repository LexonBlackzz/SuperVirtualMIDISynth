#include "driver/StageEngine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using namespace svms;

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

} // namespace

int main() {
    TestEngine();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("stage engine tests passed\n");
    return 0;
}
