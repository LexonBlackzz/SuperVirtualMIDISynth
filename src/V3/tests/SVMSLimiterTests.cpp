#include "../SVMSLimiter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using svms::AdaptiveLimiterState;
using svms::EngineConfig;
using svms::PostHighPass3Hz;

EngineConfig LimiterConfig(bool enabled, float threshold, float lookaheadMs,
                           float attackMs, float releaseMs) {
    EngineConfig cfg{};
    cfg.limiterEnabled = enabled;
    cfg.limiterThreshold = threshold;
    cfg.limiterLookaheadMs = lookaheadMs;
    cfg.limiterAttackMs = attackMs;
    cfg.limiterReleaseMs = releaseMs;
    return cfg;
}

bool TestBypassHasNoLookaheadLatency() {
    constexpr uint32_t kRate = 44100u;
    AdaptiveLimiterState limiter;
    PostHighPass3Hz highPass;
    highPass.Initialize(kRate);
    limiter.Configure(kRate, LimiterConfig(false, 0.5f, 5.0f, 0.5f, 100.0f));

    float samples[8] = {0.25f, -0.125f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    limiter.Process(samples, 4u, 2u, highPass);

    // The first high-pass sample equals the first input exactly. If bypass
    // accidentally entered the lookahead delay this would be zero.
    return std::fabs(samples[0] - 0.25f) < 1.0e-6f &&
           std::fabs(samples[1] + 0.125f) < 1.0e-6f;
}

bool TestImpulseIsCaughtEvenWithSlowUserAttack() {
    constexpr uint32_t kRate = 44100u;
    constexpr float kThreshold = 0.5f;
    constexpr float kLookaheadMs = 1.0f;
    const uint32_t lookaheadFrames = static_cast<uint32_t>(
        kLookaheadMs * kRate * 0.001f + 0.5f);

    AdaptiveLimiterState limiter;
    PostHighPass3Hz highPass;
    highPass.Initialize(kRate);
    // Deliberately absurdly slow user attack: the deadline-aware predictive
    // stage must still catch the one-sample peak.
    limiter.Configure(kRate,
        LimiterConfig(true, kThreshold, kLookaheadMs, 100.0f, 100.0f));

    constexpr uint32_t kFrames = 256u;
    constexpr uint32_t kImpulseFrame = 64u;
    std::vector<float> samples(kFrames * 2u, 0.0f);
    samples[kImpulseFrame * 2u] = 2.0f;
    samples[kImpulseFrame * 2u + 1u] = -2.0f;

    limiter.Process(samples.data(), kFrames, 2u, highPass);

    float maximum = 0.0f;
    for (float sample : samples) maximum = (std::max)(maximum, std::fabs(sample));
    if (maximum > kThreshold + 1.0e-5f) return false;

    // The delayed impulse must not appear before the configured lookahead.
    const uint32_t expected = kImpulseFrame + lookaheadFrames;
    for (uint32_t frame = 0u; frame < expected; ++frame) {
        if (std::fabs(samples[frame * 2u]) > 1.0e-5f ||
            std::fabs(samples[frame * 2u + 1u]) > 1.0e-5f) {
            return false;
        }
    }
    return true;
}

bool TestDenseWallStaysBounded() {
    constexpr uint32_t kRate = 48000u;
    constexpr float kThreshold = 0.72f;
    AdaptiveLimiterState limiter;
    PostHighPass3Hz highPass;
    highPass.Initialize(kRate);
    limiter.Configure(kRate,
        LimiterConfig(true, kThreshold, 1.0f, 0.5f, 80.0f));

    constexpr uint32_t kFrames = 4096u;
    std::vector<float> samples(kFrames * 2u);
    for (uint32_t frame = 0u; frame < kFrames; ++frame) {
        const float carrier = std::sin(
            6.28318530717958647692f * 997.0f *
            static_cast<float>(frame) / static_cast<float>(kRate));
        const float burst = (frame % 31u) == 0u ? 1.85f : 1.15f;
        samples[frame * 2u] = carrier * burst;
        samples[frame * 2u + 1u] = -carrier * burst * 0.93f;
    }

    limiter.Process(samples.data(), kFrames, 2u, highPass);

    float maximum = 0.0f;
    for (float sample : samples) maximum = (std::max)(maximum, std::fabs(sample));
    return maximum <= kThreshold + 1.0e-5f &&
           limiter.gainReductionDb > 0.0f;
}

} // namespace

int main() {
    struct Test { const char* name; bool (*fn)(); };
    const Test tests[] = {
        {"bypass-no-lookahead-latency", TestBypassHasNoLookaheadLatency},
        {"slow-attack-impulse", TestImpulseIsCaughtEvenWithSlowUserAttack},
        {"dense-wall-bounded", TestDenseWallStaysBounded},
    };

    bool ok = true;
    for (const Test& test : tests) {
        const bool passed = test.fn();
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", test.name);
        ok = ok && passed;
    }
    return ok ? 0 : 1;
}
