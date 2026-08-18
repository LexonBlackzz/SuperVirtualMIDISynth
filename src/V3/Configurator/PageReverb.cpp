#include "PageReverb.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "Theme.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace svms::cfg {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = kPi * 2.0f;

float Clamp01(float value) {
    return ImClamp(value, 0.0f, 1.0f);
}

float SmoothValue(float current, float target, float dt, float speed) {
    const float clampedDt = ImClamp(dt, 0.0f, 0.05f);
    const float alpha = 1.0f - std::exp(-speed * clampedDt);
    return current + (target - current) * alpha;
}

ImVec4 MixColor(const ImVec4& a, const ImVec4& b, float t) {
    t = ImClamp(t, 0.0f, 1.0f);
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

ImVec4 WithAlpha(ImVec4 color, float alpha) {
    color.w = ImClamp(alpha, 0.0f, 1.0f);
    return color;
}

float Hash01(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return static_cast<float>(value & 0x00ffffffu) / 16777215.0f;
}

float HzToKHz(float hz) {
    return hz / 1000.0f;
}

struct AtmosphereState {
    bool initialized = false;
    float roomSize = 0.0f;
    float decay = 0.0f;
    float damping = 0.0f;
    float width = 0.0f;
    float diffusion = 0.0f;
    float preDelayMs = 0.0f;
    float earlyLevel = 0.0f;
    float lateLevel = 0.0f;
    float modDepth = 0.0f;
    float modRate = 0.0f;
    float lowCutHz = 0.0f;
    float highCutHz = 0.0f;
    float mix = 0.0f;
    float phase = 0.0f;
};

void UpdateAtmosphere(AtmosphereState& state, const ConfigValues& values, float dt) {
    if (!state.initialized) {
        state.initialized = true;
        state.roomSize = values.reverbRoomSize;
        state.decay = values.reverbDecay;
        state.damping = values.reverbDamping;
        state.width = values.reverbWidth;
        state.diffusion = values.reverbDiffusion;
        state.preDelayMs = values.reverbPreDelayMs;
        state.earlyLevel = values.reverbEarlyLevel;
        state.lateLevel = values.reverbLateLevel;
        state.modDepth = values.reverbModDepth;
        state.modRate = values.reverbModRate;
        state.lowCutHz = values.reverbLowCutHz;
        state.highCutHz = values.reverbHighCutHz;
        state.mix = values.reverbMix;
    } else {
        // Purely visual smoothing. The DSP still receives the exact working
        // values through RuntimeLink; only the eye-candy field eases between
        // parameter states so dragging a knob feels fluid at VSync rate.
        constexpr float shapeSpeed = 8.5f;
        constexpr float toneSpeed = 6.0f;
        state.roomSize = SmoothValue(state.roomSize, values.reverbRoomSize, dt, shapeSpeed);
        state.decay = SmoothValue(state.decay, values.reverbDecay, dt, shapeSpeed);
        state.damping = SmoothValue(state.damping, values.reverbDamping, dt, toneSpeed);
        state.width = SmoothValue(state.width, values.reverbWidth, dt, shapeSpeed);
        state.diffusion = SmoothValue(state.diffusion, values.reverbDiffusion, dt, shapeSpeed);
        state.preDelayMs = SmoothValue(state.preDelayMs, values.reverbPreDelayMs, dt, shapeSpeed);
        state.earlyLevel = SmoothValue(state.earlyLevel, values.reverbEarlyLevel, dt, shapeSpeed);
        state.lateLevel = SmoothValue(state.lateLevel, values.reverbLateLevel, dt, shapeSpeed);
        state.modDepth = SmoothValue(state.modDepth, values.reverbModDepth, dt, shapeSpeed);
        state.modRate = SmoothValue(state.modRate, values.reverbModRate, dt, shapeSpeed);
        state.lowCutHz = SmoothValue(state.lowCutHz, values.reverbLowCutHz, dt, toneSpeed);
        state.highCutHz = SmoothValue(state.highCutHz, values.reverbHighCutHz, dt, toneSpeed);
        state.mix = SmoothValue(state.mix, values.reverbMix, dt, shapeSpeed);
    }

    state.phase += ImClamp(dt, 0.0f, 0.05f) * (0.35f + state.modRate * 2.6f);
    if (state.phase > 10000.0f) state.phase = std::fmod(state.phase, kTau);
}

void DrawEllipse(ImDrawList* drawList, const ImVec2& center,
                 float radiusX, float radiusY, float phase,
                 float irregularity, ImU32 color, float thickness) {
    constexpr int segments = 64;
    ImVec2 previous{};
    for (int i = 0; i <= segments; ++i) {
        const float angle = (static_cast<float>(i) / segments) * kTau;
        const float warp = 1.0f +
            std::sin(angle * 2.0f + phase) * irregularity +
            std::sin(angle * 5.0f - phase * 0.7f) * irregularity * 0.35f;
        const ImVec2 point(center.x + std::cos(angle) * radiusX * warp,
                           center.y + std::sin(angle) * radiusY * warp);
        if (i > 0) drawList->AddLine(previous, point, color, thickness);
        previous = point;
    }
}

void DrawAtmospherePanel(const ImVec2& pos, const ImVec2& size,
                         const AtmosphereState& state, bool enabled) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const ThemeSettings& theme = GetThemeSettings();
    const ImVec4 accent = theme.accent;
    const ImVec4 accentMid = MixColor(accent, theme.text, 0.13f);
    const ImVec4 accentBright = MixColor(accent, theme.text, 0.34f);

    // Keep the visual darker than the surrounding effect page so the coloured
    // field has somewhere to glow, but tint the panel with the active theme.
    const ImVec4 top = MixColor(theme.background, theme.panel, 0.56f);
    const ImVec4 bottom = MixColor(theme.background, ImVec4(0, 0, 0, 1), 0.28f);
    const ImU32 topColor = ImGui::GetColorU32(top);
    const ImU32 bottomColor = ImGui::GetColorU32(bottom);
    drawList->AddRectFilledMultiColor(pos, max,
                                      topColor, topColor,
                                      bottomColor, bottomColor);
    drawList->AddRect(pos, max,
                      ImGui::GetColorU32(MixColor(theme.panel, theme.text, 0.16f)),
                      6.0f, 0, 1.0f);

    if (size.x < 80.0f || size.y < 80.0f) return;

    drawList->PushClipRect(ImVec2(pos.x + 2.0f, pos.y + 2.0f),
                           ImVec2(max.x - 2.0f, max.y - 2.0f), true);

    const ImVec2 center(pos.x + size.x * 0.5f,
                        pos.y + size.y * 0.49f);

    const float room = Clamp01(state.roomSize);
    const float decay = Clamp01(state.decay);
    const float damping = Clamp01(state.damping);
    const float width = Clamp01(state.width);
    const float diffusion = Clamp01(state.diffusion);
    const float preDelay = Clamp01(state.preDelayMs / 200.0f);
    const float early = Clamp01(state.earlyLevel / 1.5f);
    const float late = Clamp01(state.lateLevel / 1.5f);
    const float modDepth = Clamp01(state.modDepth);
    const float lowCut = Clamp01(state.lowCutHz / 2000.0f);
    const float highCut = Clamp01((state.highCutHz - 1000.0f) / 19000.0f);
    const float mix = Clamp01(state.mix);

    const float enabledGain = enabled ? 1.0f : 0.22f;
    // The old field multiplied already-small alpha values by roughly 0.5 at
    // the default mix, which made the animation practically disappear. Keep
    // mix meaningful, but give the visual a healthy baseline presence.
    const float wetGain = (0.62f + mix * 0.38f) * enabledGain;
    const float brightness = (0.78f + highCut * 0.30f) *
                             (1.0f - damping * 0.24f);

    // Width directly stretches the field. Radius is compensated so even
    // maximum width remains inside the available panel.
    const float xScale = 0.58f + width * 0.78f;
    const float yScale = 0.88f + room * 0.08f;
    const float maxRadiusX = (size.x * 0.43f) / xScale;
    const float maxRadiusY = (size.y * 0.39f) / yScale;
    const float outerRadius = (std::max)(20.0f,
        (std::min)(maxRadiusX, maxRadiusY));
    const float fieldRadius = outerRadius * (0.58f + room * 0.42f);
    const float deadZone = fieldRadius * (0.055f + preDelay * 0.19f);

    // Center glow and the pre-delay boundary. Low Cut reduces the warm/inner
    // energy while Pre-delay opens an obvious quiet space before the room.
    for (int i = 5; i >= 1; --i) {
        const float t = static_cast<float>(i) / 5.0f;
        const float radius = deadZone * (1.15f + t * 2.8f);
        const float alpha = (0.025f + early * 0.050f) *
                            (1.0f - lowCut * 0.38f) * wetGain;
        drawList->AddCircleFilled(center, radius,
            ImGui::GetColorU32(WithAlpha(accentMid, alpha / t)), 40);
    }

    DrawEllipse(drawList, center,
                deadZone * xScale, deadZone * yScale,
                state.phase * 0.25f, 0.012f,
                ImGui::GetColorU32(WithAlpha(
                    accentBright, (0.18f + early * 0.18f) * wetGain)),
                1.2f);

    // Early reflections remain comparatively crisp and live near the center.
    const int earlyShells = 3 + static_cast<int>(early * 3.0f);
    for (int i = 0; i < earlyShells; ++i) {
        const float t = (static_cast<float>(i) + 1.0f) /
                        (static_cast<float>(earlyShells) + 1.0f);
        const float radius = deadZone +
            (fieldRadius * 0.43f - deadZone) * t;
        const float irregularity = (1.0f - diffusion) * 0.045f +
            modDepth * 0.014f * std::sin(state.phase * 1.7f + i * 1.9f);
        const float alpha = (0.11f + early * 0.25f) *
                            (1.0f - t * 0.32f) * wetGain;
        DrawEllipse(drawList, center,
                    radius * xScale, radius * yScale,
                    state.phase * 0.18f + i,
                    irregularity,
                    ImGui::GetColorU32(WithAlpha(accentBright, alpha)),
                    1.1f + early * 0.45f);
    }

    // Late FDN-style field. This is intentionally parameter-driven eye candy,
    // not a fake spectrum or signal analyzer.
    const int ringCount = 8 + static_cast<int>(diffusion * 9.0f + decay * 4.0f);
    for (int i = 0; i < ringCount; ++i) {
        const float t = ringCount > 1
            ? static_cast<float>(i) / static_cast<float>(ringCount - 1)
            : 0.0f;
        const float radius = fieldRadius * (0.28f + t * 0.72f);
        const float tail = std::pow(1.0f - t,
                                    0.45f + (1.0f - decay) * 1.8f);
        const float distantDamping = 1.0f - t * damping * 0.56f;
        const float alpha = (0.060f + late * 0.190f) *
                            (0.38f + decay * 0.62f) *
                            tail * distantDamping * brightness * wetGain;
        const float phase = state.phase * (0.32f + modDepth * 0.70f) + i * 0.61f;
        const float irregularity = (1.0f - diffusion) * 0.070f +
            modDepth * 0.018f * std::sin(state.phase * 1.3f + i * 0.7f);

        const ImVec4 ringColor = MixColor(accentMid, theme.text,
                                          0.04f + highCut * 0.12f);
        DrawEllipse(drawList, center,
                    radius * xScale, radius * yScale,
                    phase, irregularity,
                    ImGui::GetColorU32(WithAlpha(ringColor, alpha)),
                    1.0f);
    }

    // Deterministic diffuse cloud. It never randomly pops between frames;
    // the points drift and breathe from Mod Rate/Depth while the smoothed
    // shape responds continuously to the other controls.
    const int pointCount = 32 + static_cast<int>(diffusion * 82.0f);
    for (int i = 0; i < pointCount; ++i) {
        const uint32_t seed = static_cast<uint32_t>(i);
        const float h0 = Hash01(seed * 747796405u + 2891336453u);
        const float h1 = Hash01(seed * 277803737u + 1013904223u);
        const float h2 = Hash01(seed * 1597334677u + 3812015801u);

        const float baseT = 0.18f + h0 * 0.82f;
        const float radialShape = std::pow(baseT, 0.72f + diffusion * 0.55f);
        const float radius = fieldRadius * radialShape;
        const float baseAngle = h1 * kTau;
        const float drift = state.phase * (0.16f + h2 * 0.40f);
        const float angle = baseAngle + drift * (0.18f + modDepth * 0.42f);
        const float breathe = 1.0f +
            std::sin(state.phase * (0.7f + h2) + h1 * 8.0f) * modDepth * 0.065f;

        const ImVec2 point(center.x + std::cos(angle) * radius * breathe * xScale,
                           center.y + std::sin(angle) * radius * breathe * yScale);
        const float tailEnergy = 1.0f - radialShape *
            (0.40f + (1.0f - decay) * 0.45f);
        const float alpha = (0.085f + diffusion * 0.120f) *
                            (0.34f + late * 0.66f) *
                            Clamp01(tailEnergy) * brightness * wetGain;
        const float dotSize = 1.0f + diffusion * 0.82f + h2 * 0.52f;

        const float trailAngle = angle - 0.018f *
            (0.5f + decay + modDepth);
        const ImVec2 trail(center.x + std::cos(trailAngle) * radius * breathe * xScale,
                           center.y + std::sin(trailAngle) * radius * breathe * yScale);
        drawList->AddLine(trail, point,
            ImGui::GetColorU32(WithAlpha(accentMid, alpha * 0.62f)), 1.0f);
        drawList->AddCircleFilled(point, dotSize,
            ImGui::GetColorU32(WithAlpha(accentBright, alpha)), 8);
    }

    // Stereo focus points make Width readable immediately without claiming
    // to represent live audio energy.
    const float stereoOffset = fieldRadius * xScale * width * 0.28f;
    const float focusAlpha = (0.14f + width * 0.18f) * wetGain;
    drawList->AddCircleFilled(ImVec2(center.x - stereoOffset, center.y), 2.4f,
        ImGui::GetColorU32(WithAlpha(accentBright, focusAlpha)), 10);
    drawList->AddCircleFilled(ImVec2(center.x + stereoOffset, center.y), 2.4f,
        ImGui::GetColorU32(WithAlpha(accentBright, focusAlpha)), 10);

    drawList->AddText(ImVec2(pos.x + 12.0f, pos.y + 10.0f),
        ImGui::GetColorU32(WithAlpha(theme.mutedText, 0.94f)),
        "SPACE FIELD");

    const char* hint = enabled ? "parameter-driven atmosphere" : "reverb bypassed";
    const ImVec2 hintSize = ImGui::CalcTextSize(hint);
    drawList->AddText(ImVec2(max.x - hintSize.x - 12.0f,
                             max.y - hintSize.y - 10.0f),
        ImGui::GetColorU32(WithAlpha(theme.mutedText, 0.72f)), hint);

    drawList->PopClipRect();
}

bool DrawHeaderToggle(const char* id, const char* label, bool* value,
                      const char* tooltip) {
    ImGui::PushID(id);
    const float switchW = 44.0f;
    const float switchH = 22.0f;
    const float gap = 10.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 itemSize(switchW + gap + textSize.x, switchH);

    ImGui::InvisibleButton("##header_toggle", itemSize);
    bool changed = false;
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
    }
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& theme = GetThemeSettings();
    const float radius = switchH * 0.5f;
    const float t = *value ? 1.0f : 0.0f;
    const float cx = pos.x + radius + t * (switchW - radius * 2.0f);
    const float cy = pos.y + radius;
    dl->AddRectFilled(pos, ImVec2(pos.x + switchW, pos.y + switchH),
                      ImGui::GetColorU32(*value ? GetAccent() : theme.control),
                      radius);
    dl->AddCircleFilled(ImVec2(cx, cy), radius - 2.0f,
                        ImGui::GetColorU32(theme.text));
    dl->AddText(ImVec2(pos.x + switchW + gap,
                       pos.y + (switchH - textSize.y) * 0.5f),
                ImGui::GetColorU32(theme.text), label);

    if (tooltip && hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return changed;
}

void DrawHeaderState(bool connected,
                     const svms::RuntimeLinkTelemetryV2* telemetry,
                     const ConfigValues& values,
                     const char* liveTooltip,
                     const char* appliedTooltip) {
    const bool synced = connected && telemetry &&
                        LiveAppliedMatches(*telemetry, values);
    const char* stateText = !connected || !telemetry
        ? "OFFLINE" : (synced ? "APPLIED" : "PENDING");
    const ImVec4 stateColor = !connected || !telemetry
        ? GetMutedText() : (synced ? GetSuccess() : GetWarning());
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float totalWidth = ImGui::CalcTextSize(stateText).x;
    if (connected) totalWidth += ImGui::CalcTextSize("LIVE").x + spacing;

    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, available - totalWidth));

    if (connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetSuccess());
        ImGui::TextUnformatted("LIVE");
        ImGui::PopStyleColor();
        if (liveTooltip && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(liveTooltip);
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
    ImGui::TextUnformatted(stateText);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(appliedTooltip);
        if (connected && telemetry && !synced)
            ImGui::TextUnformatted("Working values differ from the engine's applied echo.");
        ImGui::EndTooltip();
    }
}

void CompactSectionHeader(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

bool DrawLiveKnob(ConfigDocument& doc, float& value,
                  float minValue, float maxValue, float defaultValue,
                  const char* label, float size, const char* format,
                  svms::RLCommandType command,
                  float displayScale = 1.0f,
                  float (*displayFn)(float) = nullptr) {
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - size) * 0.5f));

    KnobState knob = {
        value, minValue, maxValue, defaultValue,
        label, nullptr, size, displayScale, displayFn
    };
    if (!RotaryKnob(knob, format)) return false;

    value = knob.value;
    doc.MarkDirty();
    PushLiveFloat(command, knob.value);
    return true;
}

void DrawControlPanel(ConfigDocument& doc, float panelHeight) {
    auto& values = doc.Working();
    const float panelWidth = ImGui::GetContentRegionAvail().x;

    float knobSize = 52.0f;
    if (panelHeight < 400.0f || panelWidth < 440.0f) knobSize = 47.0f;
    if (panelHeight < 350.0f || panelWidth < 380.0f) knobSize = 42.0f;
    const float mixKnobSize = (std::min)(74.0f, knobSize * 1.32f);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.0f, 3.0f));

    CompactSectionHeader("SPACE");
    if (ImGui::BeginTable("##reverb_space", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, values.reverbMix, 0.0f, 1.0f, 0.25f,
                     "MIX", mixKnobSize, "%.0f%%",
                     svms::RLCommandType::SetReverbMix, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, values.reverbRoomSize, 0.0f, 1.0f, 0.60f,
                     "ROOM SIZE", knobSize, "%.0f%%",
                     svms::RLCommandType::SetReverbRoomSize, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, values.reverbDecay, 0.0f, 1.0f, 0.50f,
                     "DECAY", knobSize, "%.0f%%",
                     svms::RLCommandType::SetReverbDecay, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, values.reverbPreDelayMs, 0.0f, 200.0f, 12.0f,
                     "PRE-DELAY", knobSize, "%.0f ms",
                     svms::RLCommandType::SetReverbPreDelayMs);
        ImGui::EndTable();
    }

    // RotaryKnob draws its label below the invisible interaction item. Reserve
    // explicit breathing room before the next section so the label can never
    // collide with the following header at smaller panel sizes.
    ImGui::Dummy(ImVec2(0.0f, 16.0f));

    CompactSectionHeader("STEREO / BALANCE");
    if (ImGui::BeginTable("##reverb_stereo", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, values.reverbWidth, 0.0f, 1.0f, 1.0f,
                     "WIDTH", knobSize, "%.0f%%",
                     svms::RLCommandType::SetReverbWidth, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, values.reverbEarlyLevel, 0.0f, 1.5f, 0.35f,
                     "EARLY", knobSize, "%.2f",
                     svms::RLCommandType::SetReverbEarlyLevel);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, values.reverbLateLevel, 0.0f, 1.5f, 0.85f,
                     "LATE", knobSize, "%.2f",
                     svms::RLCommandType::SetReverbLateLevel);
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 16.0f));

    // Three compact mini-sections use the page width much more efficiently
    // than stacking Texture, Modulation, and Filter vertically.
    if (ImGui::BeginTable("##reverb_lower", 3,
                          ImGuiTableFlags_SizingStretchSame |
                          ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        CompactSectionHeader("TEXTURE");
        if (ImGui::BeginTable("##reverb_texture", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawLiveKnob(doc, values.reverbDiffusion, 0.0f, 1.0f, 0.70f,
                         "DIFFUSION", knobSize, "%.0f%%",
                         svms::RLCommandType::SetReverbDiffusion, 100.0f);
            ImGui::TableNextColumn();
            DrawLiveKnob(doc, values.reverbDamping, 0.0f, 1.0f, 0.35f,
                         "DAMPING", knobSize, "%.0f%%",
                         svms::RLCommandType::SetReverbDamping, 100.0f);
            ImGui::EndTable();
        }

        ImGui::TableNextColumn();
        CompactSectionHeader("MODULATION");
        if (ImGui::BeginTable("##reverb_modulation", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawLiveKnob(doc, values.reverbModDepth, 0.0f, 1.0f, 0.30f,
                         "DEPTH", knobSize, "%.0f%%",
                         svms::RLCommandType::SetReverbModDepth, 100.0f);
            ImGui::TableNextColumn();
            DrawLiveKnob(doc, values.reverbModRate, 0.0f, 1.0f, 0.35f,
                         "RATE", knobSize, "%.2f",
                         svms::RLCommandType::SetReverbModRate);
            ImGui::EndTable();
        }

        ImGui::TableNextColumn();
        CompactSectionHeader("FILTER");
        if (ImGui::BeginTable("##reverb_filter", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawLiveKnob(doc, values.reverbLowCutHz, 0.0f, 2000.0f, 70.0f,
                         "LOW CUT", knobSize, "%.0f Hz",
                         svms::RLCommandType::SetReverbLowCutHz);
            ImGui::TableNextColumn();
            DrawLiveKnob(doc, values.reverbHighCutHz, 1000.0f, 20000.0f, 16000.0f,
                         "HIGH CUT", knobSize, "%.1f kHz",
                         svms::RLCommandType::SetReverbHighCutHz, 1.0f, HzToKHz);
            ImGui::EndTable();
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();
}

} // namespace

void DrawReverbPage(ConfigDocument& doc) {
    PushEffectPageStyle();

    auto& values = doc.Working();
    const auto& live = GetLiveLinkContext();

    static AtmosphereState atmosphere;
    UpdateAtmosphere(atmosphere, values, ImGui::GetIO().DeltaTime);

    // Compact plugin header: enable on the left, effect identity centered in
    // the whole page, and runtime state right-aligned. Mix belongs with the
    // other parameters below, where it has comfortable drag space.
    constexpr float headerSideWidth = 190.0f;
    constexpr float headerRowHeight = 40.0f;
    if (ImGui::BeginTable("##reverb_header", 3,
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("power", ImGuiTableColumnFlags_WidthFixed, headerSideWidth);
        ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, headerSideWidth);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, headerRowHeight);

        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (headerRowHeight - 22.0f) * 0.5f);
        bool enabled = values.enableReverb;
        if (DrawHeaderToggle("reverb_power", "POWER", &enabled,
                             "Enable the FDN reverb effect.")) {
            values.enableReverb = enabled;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetReverbEnabled, enabled);
        }

        ImGui::TableNextColumn();
        const char* title = "REVERB";
        const float titleHeight = ImGui::GetTextLineHeight();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             (headerRowHeight - titleHeight) * 0.5f);
        const float startX = ImGui::GetCursorPosX();
        const float available = ImGui::GetContentRegionAvail().x;
        const float titleWidth = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX(startX + (available - titleWidth) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              MixColor(GetAccent(), GetThemeSettings().text, 0.28f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             (headerRowHeight - titleHeight) * 0.5f);
        DrawHeaderState(live.connected, live.telemetry, values,
                        "All reverb parameters support live preview",
                        "Reverb group applied state vs working copy");

        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Spacing();

    const ImVec2 bodyAvailable = ImGui::GetContentRegionAvail();
    const float bodyHeight = (std::max)(330.0f, bodyAvailable.y - 2.0f);

    if (ImGui::BeginTable("##reverb_body", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("atmosphere", ImGuiTableColumnFlags_WidthStretch, 0.86f);
        ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch, 1.34f);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, bodyHeight);

        ImGui::TableNextColumn();
        ImGui::BeginChild("##reverb_atmosphere", ImVec2(0.0f, bodyHeight), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 visualPos = ImGui::GetCursorScreenPos();
        const ImVec2 visualSize((std::max)(80.0f, ImGui::GetContentRegionAvail().x - 6.0f),
                                (std::max)(80.0f, ImGui::GetContentRegionAvail().y - 6.0f));
        DrawAtmospherePanel(visualPos, visualSize, atmosphere, values.enableReverb);
        ImGui::Dummy(visualSize);
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("##reverb_controls", ImVec2(0.0f, bodyHeight), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawControlPanel(doc, bodyHeight);
        ImGui::EndChild();

        ImGui::EndTable();
    }

    PopEffectPageStyle();
}

} // namespace svms::cfg
