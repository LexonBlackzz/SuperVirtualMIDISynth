#include "../SVMSLimiter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using svms::AdaptiveLimiterState;
using svms::ClassicLimiterState;
using svms::EngineConfig;
using svms::LimiterAlgorithm;
using svms::LimiterRouterState;
using svms::PostHighPass3Hz;

EngineConfig LimiterConfig(bool enabled, float threshold, float lookaheadMs,
                           float attackMs, float releaseMs,
                           LimiterAlgorithm algorithm = LimiterAlgorithm::Classic) {
    EngineConfig cfg{};
    cfg.limiterEnabled = enabled;
    cfg.limiterAlgorithm = algorithm;
    cfg.limiterThreshold = threshold;
    cfg.limiterLookaheadMs = lookaheadMs;
    cfg.limiterAttackMs = attackMs;
    cfg.limiterReleaseMs = releaseMs;
    return cfg;
}

bool AllFinite(const std::vector<float>& samples) {
    for (float sample : samples) {
        if (!std::isfinite(sample)) return false;
    }
    return true;
}

float MaximumAbs(const std::vector<float>& samples) {
    float maximum = 0.0f;
    for (float sample : samples)
        maximum = (std::max)(maximum, std::fabs(sample));
    return maximum;
}

bool TestBypassHasNoLookaheadLatency() {
    constexpr uint32_t kRate = 44100u;
    AdaptiveLimiterState limiter;
    PostHighPass3Hz highPass;
    highPass.Initialize(kRate);
    limiter.Configure(kRate, LimiterConfig(false, 0.5f, 5.0f, 0.5f, 100.0f));

    float samples[8] = {0.25f, -0.125f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    limiter.Process(samples, 4u, 2u, highPass);

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
    limiter.Configure(kRate,
        LimiterConfig(true, kThreshold, kLookaheadMs, 100.0f, 100.0f,
                      LimiterAlgorithm::Adaptive));

    constexpr uint32_t kFrames = 256u;
    constexpr uint32_t kImpulseFrame = 64u;
    std::vector<float> samples(kFrames * 2u, 0.0f);
    samples[kImpulseFrame * 2u] = 2.0f;
    samples[kImpulseFrame * 2u + 1u] = -2.0f;

    limiter.Process(samples.data(), kFrames, 2u, highPass);

    if (MaximumAbs(samples) > kThreshold + 1.0e-5f) return false;

    const uint32_t expected = kImpulseFrame + lookaheadFrames;
    for (uint32_t frame = 0u; frame < expected; ++frame) {
        if (std::fabs(samples[frame * 2u]) > 1.0e-5f ||
            std::fabs(samples[frame * 2u + 1u]) > 1.0e-5f) {
            return false;
        }
    }

    // Also require the delayed impulse to actually arrive where expected;
    // "nothing before expected" alone would allow an accidentally longer
    // delay line to pass.
    return std::fabs(samples[expected * 2u]) > 1.0e-3f &&
           std::fabs(samples[expected * 2u + 1u]) > 1.0e-3f;
}

bool TestDenseWallStaysBounded() {
    constexpr uint32_t kRate = 48000u;
    constexpr float kThreshold = 0.72f;
    AdaptiveLimiterState limiter;
    PostHighPass3Hz highPass;
    highPass.Initialize(kRate);
    limiter.Configure(kRate,
        LimiterConfig(true, kThreshold, 1.0f, 0.5f, 80.0f,
                      LimiterAlgorithm::Adaptive));

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

    return AllFinite(samples) &&
           MaximumAbs(samples) <= kThreshold + 1.0e-5f &&
           limiter.gainReductionDb > 0.0f;
}

std::vector<float> MakeRoutingSignal(uint32_t frames, uint32_t rate) {
    std::vector<float> samples(frames * 2u);
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        const float a = std::sin(6.28318530717958647692f * 733.0f *
                                 static_cast<float>(frame) /
                                 static_cast<float>(rate));
        const float transient = (frame % 79u) == 0u ? 1.70f : 0.92f;
        samples[frame * 2u] = a * transient;
        samples[frame * 2u + 1u] = a * transient * -0.81f;
    }
    return samples;
}

bool TestRouterClassicMatchesClassicCore() {
    constexpr uint32_t kRate = 48000u;
    constexpr uint32_t kFrames = 4096u;
    const EngineConfig cfg = LimiterConfig(
        true, 0.83f, 2.0f, 0.7f, 125.0f, LimiterAlgorithm::Classic);

    std::vector<float> direct = MakeRoutingSignal(kFrames, kRate);
    std::vector<float> routed = direct;

    ClassicLimiterState classic;
    PostHighPass3Hz classicHighPass;
    classicHighPass.Initialize(kRate);
    classic.Configure(kRate, cfg);
    classic.Process(direct.data(), kFrames, 2u, classicHighPass);

    LimiterRouterState router;
    PostHighPass3Hz unused;
    unused.Initialize(kRate);
    router.Configure(kRate, cfg);
    router.Process(routed.data(), kFrames, 2u, unused);

    for (size_t i = 0u; i < direct.size(); ++i) {
        if (std::fabs(direct[i] - routed[i]) > 1.0e-6f) return false;
    }
    return true;
}

bool TestRouterAdaptiveMatchesAdaptiveCore() {
    constexpr uint32_t kRate = 48000u;
    constexpr uint32_t kFrames = 4096u;
    const EngineConfig cfg = LimiterConfig(
        true, 0.83f, 2.0f, 0.7f, 125.0f, LimiterAlgorithm::Adaptive);

    std::vector<float> direct = MakeRoutingSignal(kFrames, kRate);
    std::vector<float> routed = direct;

    AdaptiveLimiterState adaptive;
    PostHighPass3Hz adaptiveHighPass;
    adaptiveHighPass.Initialize(kRate);
    adaptive.Configure(kRate, cfg);
    adaptive.Process(direct.data(), kFrames, 2u, adaptiveHighPass);

    LimiterRouterState router;
    PostHighPass3Hz unused;
    unused.Initialize(kRate);
    router.Configure(kRate, cfg);
    router.Process(routed.data(), kFrames, 2u, unused);

    for (size_t i = 0u; i < direct.size(); ++i) {
        if (std::fabs(direct[i] - routed[i]) > 1.0e-6f) return false;
    }
    return true;
}

bool TestRouterLiveSwitchStaysFinite() {
    constexpr uint32_t kRate = 48000u;
    constexpr uint32_t kFrames = 4096u;
    const EngineConfig cfg = LimiterConfig(
        true, 0.76f, 1.0f, 0.5f, 90.0f, LimiterAlgorithm::Classic);

    LimiterRouterState router;
    PostHighPass3Hz unused;
    unused.Initialize(kRate);
    router.Configure(kRate, cfg);

    std::vector<float> first = MakeRoutingSignal(kFrames, kRate);
    router.Process(first.data(), kFrames, 2u, unused);
    if (!AllFinite(first)) return false;

    router.algorithmTarget = static_cast<uint32_t>(LimiterAlgorithm::Adaptive);
    std::vector<float> second = MakeRoutingSignal(kFrames, kRate);
    router.Process(second.data(), kFrames, 2u, unused);
    if (!AllFinite(second)) return false;

    // Switch back as well; this catches one-way transition state bugs.
    router.algorithmTarget = static_cast<uint32_t>(LimiterAlgorithm::Classic);
    std::vector<float> third = MakeRoutingSignal(kFrames, kRate);
    router.Process(third.data(), kFrames, 2u, unused);
    if (!AllFinite(third)) return false;

    return MaximumAbs(second) < 4.0f && MaximumAbs(third) < 4.0f;
}

} // namespace

int main() {
    struct Test { const char* name; bool (*fn)(); };
    const Test tests[] = {
        {"bypass-no-lookahead-latency", TestBypassHasNoLookaheadLatency},
        {"slow-attack-impulse", TestImpulseIsCaughtEvenWithSlowUserAttack},
        {"dense-wall-bounded", TestDenseWallStaysBounded},
        {"router-classic-matches-core", TestRouterClassicMatchesClassicCore},
        {"router-adaptive-matches-core", TestRouterAdaptiveMatchesAdaptiveCore},
        {"router-live-switch-finite", TestRouterLiveSwitchStaysFinite},
    };

    bool ok = true;
    for (const Test& test : tests) {
        const bool passed = test.fn();
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", test.name);
        ok = ok && passed;
    }
    return ok ? 0 : 1;
}
