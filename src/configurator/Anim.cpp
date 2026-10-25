#include "configurator/Anim.h"

#include <cmath>
#include <unordered_map>

namespace svms::cfg {
namespace {

struct AnimState {
    float value = 0.0f;
    float velocity = 0.0f;
    float from = 0.0f;      // ease start
    float target = 0.0f;
    double start = 0.0;     // ease start time / pulse trigger time
    double last = -1.0;     // time of the previous update
};

Motion g_motion = Motion::Full;
// Note: entries are never evicted; a few hundred widget IDs at most.
std::unordered_map<ImGuiID, AnimState> g_springs;
std::unordered_map<ImGuiID, AnimState> g_eases;
std::unordered_map<ImGuiID, double> g_pulses;

constexpr float kSpringSeconds = 0.45f;
// A value that was not drawn for this long (page change, closed disclosure)
// snaps instead of animating from a stale position.
constexpr double kStaleSeconds = 0.2;

double Now() { return ImGui::GetTime(); }

float EaseValue(AnimState& s, float target, float seconds) {
    const double now = Now();
    if (s.last < 0.0 || now - s.last > kStaleSeconds) {
        s.value = s.from = s.target = target;
        s.last = now;
        return target;
    }
    s.last = now;
    if (target != s.target) {
        s.from = s.value;
        s.target = target;
        s.start = now;
    }
    const float t = seconds > 0.0f
        ? static_cast<float>((now - s.start) / seconds) : 1.0f;
    s.value = t >= 1.0f ? target : s.from + (target - s.from) * EaseOut(t);
    return s.value;
}

} // namespace

void SetMotion(Motion motion) { g_motion = motion; }
Motion GetMotion() { return g_motion; }

float EaseOut(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float u = 1.0f - t;
    return 1.0f - u * u * u * u;
}

float AnimSpring(ImGuiID id, float target) {
    if (g_motion == Motion::Off) return target;
    if (g_motion == Motion::Calm) return EaseValue(g_eases[id], target, kSpringSeconds);

    AnimState& s = g_springs[id];
    const double now = Now();
    if (s.last < 0.0 || now - s.last > kStaleSeconds) {
        s.value = target;
        s.velocity = 0.0f;
        s.last = now;
        return target;
    }
    float dt = static_cast<float>(now - s.last);
    s.last = now;
    // Damped spring: omega 14 rad/s, zeta 0.6 gives ~10% overshoot and
    // settles in ~0.45 s, like the prototype's spring curve.
    constexpr float kOmega = 14.0f;
    constexpr float kZeta = 0.6f;
    while (dt > 0.0f) {
        const float h = dt > (1.0f / 240.0f) ? (1.0f / 240.0f) : dt;
        const float accel = -kOmega * kOmega * (s.value - target) -
                            2.0f * kZeta * kOmega * s.velocity;
        s.velocity += accel * h;
        s.value += s.velocity * h;
        dt -= h;
    }
    const float scale = std::fabs(target) > 1.0f ? std::fabs(target) : 1.0f;
    if (std::fabs(s.value - target) < 1e-4f * scale && std::fabs(s.velocity) < 1e-3f * scale) {
        s.value = target;
        s.velocity = 0.0f;
    }
    return s.value;
}

float AnimEase(ImGuiID id, float target, float seconds) {
    if (g_motion == Motion::Off) return target;
    return EaseValue(g_eases[id], target, seconds);
}

void AnimTrigger(ImGuiID id) { g_pulses[id] = Now(); }

float AnimPulse(ImGuiID id, float seconds) {
    if (g_motion == Motion::Off) return 1.0f;
    const auto it = g_pulses.find(id);
    if (it == g_pulses.end() || seconds <= 0.0f) return 1.0f;
    const float t = static_cast<float>((Now() - it->second) / seconds);
    return t >= 1.0f ? 1.0f : (t < 0.0f ? 0.0f : t);
}

bool AnimPulseActive(ImGuiID id, float seconds) {
    return AnimPulse(id, seconds) < 1.0f;
}

} // namespace svms::cfg
