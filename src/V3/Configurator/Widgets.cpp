#include "Widgets.h"
#include "ConfiguratorApp.h"
#include "ConfigDocument.h"
#include "Theme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../SVMSRuntimeLink.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace svms::cfg {

static LiveLinkContext g_liveLink = {};

void SetLiveLinkContext(const LiveLinkContext& ctx) { g_liveLink = ctx; }
const LiveLinkContext& GetLiveLinkContext() { return g_liveLink; }

// Live changes are routed through the ConfiguratorApp so widgets never
// talk to the driver directly: every knob/edit marks its group dirty on
// the app's coalescing working live state, and the app sends ONE grouped
// ApplyLiveConfig command per flush interval.
void PushLiveFloat(svms::RLCommandType type, float value) {
    if (g_liveLink.app) g_liveLink.app->SetLiveFloat(type, value);
}

void PushLiveBool(svms::RLCommandType type, bool value) {
    if (g_liveLink.app) g_liveLink.app->SetLiveBool(type, value);
}

static float g_toastTimer = 0.0f;
static char g_toastText[512] = {};
static bool g_toastActive = false;

void SectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
    ImGui::PushFont(nullptr);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool ToggleSwitch(const char* label, bool* value, const char* tooltip) {
    ImGui::PushID(label);
    bool changed = false;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = 44.0f;
    float h = 22.0f;
    float radius = h * 0.5f;

    ImGui::InvisibleButton("toggle", ImVec2(w, h));
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bgCol = *value
        ? ImGui::GetColorU32(GetAccent())
        : ImGui::GetColorU32(GetThemeSettings().control);
    float t = *value ? 1.0f : 0.0f;
    float cx = pos.x + radius + t * (w - radius * 2.0f);
    float cy = pos.y + radius;

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bgCol, h * 0.5f);
    dl->AddCircleFilled(ImVec2(cx, cy), radius - 2.0f,
                        ImGui::GetColorU32(GetThemeSettings().text));

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
    ImGui::TextUnformatted(label);

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool LabeledFloat(const char* label, float* value, float min, float max,
                  const char* format, const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float inputWidth = ImMin(avail - labelWidth - 8.0f, 160.0f);

    ImGui::PushItemWidth(inputWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - inputWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + inputWidth);

    if (ImGui::InputFloat("##val", value, 0.0f, 0.0f, format)) {
        if (*value < min) *value = min;
        if (*value > max) *value = max;
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool LabeledInt(const char* label, int* value, int min, int max,
                const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float inputWidth = ImMin(avail - labelWidth - 8.0f, 160.0f);

    ImGui::PushItemWidth(inputWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - inputWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + inputWidth);

    if (ImGui::InputInt("##val", value, 0, 0)) {
        if (*value < min) *value = min;
        if (*value > max) *value = max;
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool LabeledUInt(const char* label, unsigned int* value, unsigned int min,
                 unsigned int max, const char* tooltip) {
    int signedVal = static_cast<int>(*value);
    bool changed = LabeledInt(label, &signedVal, static_cast<int>(min),
                              static_cast<int>(max), tooltip);
    if (changed) *value = static_cast<unsigned int>(signedVal);
    return changed;
}

bool LabeledCombo(const char* label, int* current, const char* const* items,
                  int itemCount, const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float comboWidth = ImMin(avail - labelWidth - 8.0f, 240.0f);

    ImGui::PushItemWidth(comboWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - comboWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + comboWidth);

    if (ImGui::Combo("##val", current, items, itemCount)) {
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool SliderFloat(const char* label, float* value, float min, float max,
                 const char* format, const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float sliderWidth = ImMin(avail - labelWidth - 8.0f, 300.0f);

    ImGui::PushItemWidth(sliderWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - sliderWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + sliderWidth);

    if (ImGui::SliderFloat("##val", value, min, max, format)) {
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool SliderInt(const char* label, int* value, int min, int max,
               const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float sliderWidth = ImMin(avail - labelWidth - 8.0f, 300.0f);

    ImGui::PushItemWidth(sliderWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - sliderWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + sliderWidth);

    if (ImGui::SliderInt("##val", value, min, max)) {
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

void StatusBar(const char* text, bool modified) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

    if (modified) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::Text("Configuration modified");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
}

void ToastNotification(const char* message, float durationSeconds) {
    snprintf(g_toastText, sizeof(g_toastText), "%s", message);
    g_toastTimer = durationSeconds;
    g_toastActive = true;
}

void PushToastStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        (std::max)(6.0f, GetThemeSettings().cornerRadius));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));
    ImVec4 bg = GetPanelBg();
    bg.w = 0.97f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, GetAccentDim());
}

void PopToastStyle() {
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

bool BeginToast(const char* id) {
    if (!g_toastActive) return false;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos(io.DisplaySize.x - 20.0f, io.DisplaySize.y - 20.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);

    PushToastStyle();
    bool visible = ImGui::Begin(id, nullptr,
                                ImGuiWindowFlags_NoDecoration |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoFocusOnAppearing);
    return visible;
}

void EndToast() {
    ImGui::End();
    PopToastStyle();
}

bool BeginToastOverlay() {
    if (!g_toastActive) return false;

    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    g_toastTimer -= dt;
    if (g_toastTimer <= 0.0f) {
        g_toastActive = false;
        return false;
    }

    ImVec2 pos(io.DisplaySize.x - 20.0f, io.DisplaySize.y - 20.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);

    PushToastStyle();
    bool visible = ImGui::Begin("##toast", nullptr,
                                ImGuiWindowFlags_NoDecoration |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoFocusOnAppearing);
    return visible;
}

void EndToastOverlay() {
    ImGui::End();
    PopToastStyle();
}

bool RotaryKnob(KnobState& state, const char* format) {
    ImGui::PushID(state.label);

    bool changed = false;
    float size = state.size > 0 ? state.size : 58.0f;
    float radius = size * 0.42f;
    float innerRadius = radius - 4.0f;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 center(cursor.x + size * 0.5f, cursor.y + radius + 8.0f);
    ImVec2 totalSize(size, size + 24.0f);

    ImGui::InvisibleButton("knob", totalSize);

    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    float t = (state.value - state.minValue) / (state.maxValue - state.minValue);
    t = ImClamp(t, 0.0f, 1.0f);

    float startAngle = 0.75f * 3.14159265f;
    float endAngle = 2.25f * 3.14159265f;
    float angle = startAngle + t * (endAngle - startAngle);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& theme = GetThemeSettings();

    ImU32 borderCol = ImGui::GetColorU32(
        hovered ? ImGui::GetStyleColorVec4(ImGuiCol_Border)
                : GetInputBorder());
    ImVec4 arc = GetAccent();
    arc.w = 0.88f;
    ImU32 arcCol = ImGui::GetColorU32(arc);
    ImU32 knobFace = ImGui::GetColorU32(
        active ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive)
               : theme.control);

    dl->AddCircleFilled(center, radius, knobFace, 32);
    dl->AddCircle(center, radius, borderCol, 32, 1.5f);

    int arcSegments = 24;
    float arcFrac = t;
    if (arcFrac > 0.01f) {
        for (int i = 0; i < arcSegments; ++i) {
            float a0 = startAngle + (static_cast<float>(i) / arcSegments) * arcFrac * (endAngle - startAngle);
            float a1 = startAngle + (static_cast<float>(i + 1) / arcSegments) * arcFrac * (endAngle - startAngle);
            if (a1 > endAngle) a1 = endAngle;
            ImVec2 p0(center.x + std::cos(a0) * (radius - 3.0f),
                      center.y + std::sin(a0) * (radius - 3.0f));
            ImVec2 p1(center.x + std::cos(a1) * (radius - 3.0f),
                      center.y + std::sin(a1) * (radius - 3.0f));
            dl->AddLine(p0, p1, arcCol, 3.0f);
        }
    }

    ImVec2 pointer(center.x + std::cos(angle) * innerRadius,
                   center.y + std::sin(angle) * innerRadius);
    ImVec4 pointerColor = theme.text;
    pointerColor.w = 0.90f;
    dl->AddLine(center, pointer, ImGui::GetColorU32(pointerColor), 2.0f);

    char valueBuf[64];
    float displayVal = state.displayFn ? state.displayFn(state.value)
                                       : state.value * state.displayScale;
    snprintf(valueBuf, sizeof(valueBuf), format, displayVal);
    ImVec2 textSize = ImGui::CalcTextSize(valueBuf);
    dl->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 4.0f),
                ImGui::GetColorU32(theme.text), valueBuf);

    char labelBuf[128];
    snprintf(labelBuf, sizeof(labelBuf), "%s", state.label);
    ImVec2 labelText = ImGui::CalcTextSize(labelBuf);
    dl->AddText(ImVec2(center.x - labelText.x * 0.5f, center.y + radius + 22.0f),
                ImGui::GetColorU32(theme.mutedText), labelBuf);

    if (active) {
        float delta = ImGui::GetIO().MouseDelta.y;
        float range = state.maxValue - state.minValue;
        float step = range * 0.002f;
        if (ImGui::GetIO().KeyShift) step *= 0.1f;
        float newVal = state.value - delta * step;
        newVal = ImClamp(newVal, state.minValue, state.maxValue);
        if (newVal != state.value) {
            state.value = newVal;
            changed = true;
        }
    } else if (hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float range = state.maxValue - state.minValue;
            float step = range * 0.02f;
            if (ImGui::GetIO().KeyShift) step *= 0.1f;
            float newVal = state.value + wheel * step;
            newVal = ImClamp(newVal, state.minValue, state.maxValue);
            if (newVal != state.value) {
                state.value = newVal;
                changed = true;
            }
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle) ||
        (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        state.value = state.defaultValue;
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

void DrawVerticalMeter(const char* /*id*/, float value, float peak,
                       const ImVec2& size, bool showScale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    ImU32 bgCol = ImGui::GetColorU32(GetPanelBg());
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 2.0f);

    float h = size.y * ImClamp(value, 0.0f, 1.0f);

    if (h > 0) {
        float frac = h / size.y;
        ImU32 barCol;
        if (frac < 0.6f)
            barCol = ImGui::GetColorU32(GetSuccess());
        else if (frac < 0.8f)
            barCol = ImGui::GetColorU32(GetWarning());
        else
            barCol = ImGui::GetColorU32(GetError());
        dl->AddRectFilled(ImVec2(pos.x + 1, pos.y + size.y),
                          ImVec2(pos.x + size.x - 1, pos.y + size.y - h),
                          barCol, 1.0f);
    }

    if (peak > 0.01f) {
        float peakY = pos.y + size.y - size.y * ImClamp(peak, 0.0f, 1.0f);
        ImVec4 peakColor = GetThemeSettings().text;
        peakColor.w = 0.82f;
        ImU32 peakCol = ImGui::GetColorU32(peakColor);
        dl->AddLine(ImVec2(pos.x, peakY), ImVec2(pos.x + size.x, peakY), peakCol, 1.0f);
    }

    if (showScale) {
        auto drawTick = [&](float db, const char* text) {
            float frac = (db + 48.0f) / 48.0f;
            float y = pos.y + size.y - size.y * frac;
            ImVec4 tick = GetMutedText();
            tick.w = 0.82f;
            dl->AddText(ImVec2(pos.x + size.x + 3.0f, y - 5.0f),
                        ImGui::GetColorU32(tick), text);
        };
        drawTick(0.0f, "0");
        drawTick(-3.0f, "-3");
        drawTick(-6.0f, "-6");
        drawTick(-12.0f, "-12");
        drawTick(-24.0f, "-24");
        drawTick(-48.0f, "-48");
    }

    ImGui::Dummy(ImVec2(size.x + (showScale ? 30.0f : 0.0f), size.y));
}

void DrawGainReductionMeter(const char* /*id*/, float gr,
                            const ImVec2& size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    ImU32 bgCol = ImGui::GetColorU32(GetPanelBg());
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 2.0f);

    float grAbs = fabsf(gr);
    float h = size.y * ImClamp(grAbs / 24.0f, 0.0f, 1.0f);

    if (h > 0) {
        ImVec2 barTL(pos.x + 1, pos.y + 1);
        ImVec2 barBR(pos.x + size.x - 1, pos.y + 1 + h);
        ImU32 barCol = ImGui::GetColorU32(GetWarning());
        dl->AddRectFilled(barTL, barBR, barCol, 1.0f);
    }

    auto drawTick = [&](float db, const char* text) {
        float frac = db / 24.0f;
        float y = pos.y + size.y * frac;
        ImVec4 tick = GetMutedText();
        tick.w = 0.82f;
        dl->AddText(ImVec2(pos.x + size.x + 3.0f, y - 5.0f),
                    ImGui::GetColorU32(tick), text);
    };
    drawTick(0.0f, "0");
    drawTick(-3.0f, "-3");
    drawTick(-6.0f, "-6");
    drawTick(-12.0f, "-12");
    drawTick(-24.0f, "-24");

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f dB", gr);
    dl->AddText(ImVec2(pos.x + 2.0f, pos.y + size.y + 2.0f),
                ImGui::GetColorU32(GetMutedText()), buf);

    ImGui::Dummy(ImVec2(size.x + 30.0f, size.y + 16.0f));
}

void DrawReverbVisualizer(ImDrawList* dl, ImVec2 center, float radius,
                           float roomSize, float decay, float diffusion,
                           float width, float modDepth, float time) {
    int ringCount = 6 + static_cast<int>(roomSize * 10.0f);
    if (ringCount > 18) ringCount = 18;

    for (int i = 0; i < ringCount; ++i) {
        float t = static_cast<float>(i) / ringCount;
        float r = radius * (0.15f + t * 0.85f) * roomSize;
        float alpha = (1.0f - t * 0.7f) * decay;
        float wobble = std::sin(time * 1.5f + i * 0.7f) * modDepth * 4.0f * t;

        ImVec4 ring = GetAccent();
        ring.w = alpha * 0.35f;
        ImU32 col = ImGui::GetColorU32(ring);

        int segments = 32;
        for (int j = 0; j < segments; ++j) {
            float a0 = (static_cast<float>(j) / segments) * 6.28318f;
            float a1 = (static_cast<float>(j + 1) / segments) * 6.28318f;

            float spread = 1.0f + std::sin(a0 * 2.0f) * (1.0f - diffusion) * 0.3f;
            float spread2 = 1.0f + std::sin(a1 * 2.0f) * (1.0f - diffusion) * 0.3f;

            float xw = width;
            ImVec2 p0(center.x + std::cos(a0) * r * spread * xw + wobble,
                      center.y + std::sin(a0) * r * spread + wobble * 0.5f);
            ImVec2 p1(center.x + std::cos(a1) * r * spread2 * xw + wobble * 0.8f,
                      center.y + std::sin(a1) * r * spread2 + wobble * 0.3f);
            dl->AddLine(p0, p1, col, 1.0f);
        }
    }

    int cloudPoints = static_cast<int>(diffusion * 60.0f) + 10;
    for (int i = 0; i < cloudPoints; ++i) {
        float angle = static_cast<float>(i) / cloudPoints * 6.28318f;
        float dist = radius * (0.1f + fmodf(static_cast<float>(i * 7 + 3),
                                             17.0f) / 17.0f * 0.6f * roomSize);
        float modOffset = std::sin(time * 0.8f + angle * 3.0f) * modDepth * dist * 0.15f;
        ImVec2 pt(center.x + std::cos(angle) * (dist + modOffset) * width,
                  center.y + std::sin(angle) * (dist + modOffset));
        ImVec4 dot = GetAccent();
        dot.w = 0.15f + decay * 0.15f;
        ImU32 dotCol = ImGui::GetColorU32(dot);
        dl->AddCircleFilled(pt, 1.5f, dotCol, 6);
    }
}

bool LiveAppliedMatches(const svms::RuntimeLinkTelemetryV2& telemetry,
                        const ConfigValues& working) {
    const svms::RuntimeLiveStateV2& e = telemetry.live;
    const auto closeEnough = [](float a, float b) {
        const float d = a - b;
        return d > -1e-4f && d < 1e-4f;
    };
    if (e.correctnessMode != (working.correctnessMode ? 1u : 0u)) return false;
    if (e.reverbEnabled != (working.enableReverb ? 1u : 0u)) return false;
    if (e.limiterEnabled != (working.limiterEnabled ? 1u : 0u)) return false;
    if (!closeEnough(e.masterVolume, working.masterVolume)) return false;
    if (!closeEnough(e.reverbMix, working.reverbMix)) return false;
    if (!closeEnough(e.reverbRoomSize, working.reverbRoomSize)) return false;
    if (!closeEnough(e.reverbDecay, working.reverbDecay)) return false;
    if (!closeEnough(e.reverbDamping, working.reverbDamping)) return false;
    if (!closeEnough(e.reverbWidth, working.reverbWidth)) return false;
    if (!closeEnough(e.reverbDiffusion, working.reverbDiffusion)) return false;
    if (!closeEnough(e.reverbPreDelayMs, working.reverbPreDelayMs)) return false;
    if (!closeEnough(e.reverbEarlyLevel, working.reverbEarlyLevel)) return false;
    if (!closeEnough(e.reverbLateLevel, working.reverbLateLevel)) return false;
    if (!closeEnough(e.reverbModDepth, working.reverbModDepth)) return false;
    if (!closeEnough(e.reverbModRate, working.reverbModRate)) return false;
    if (!closeEnough(e.reverbLowCutHz, working.reverbLowCutHz)) return false;
    if (!closeEnough(e.reverbHighCutHz, working.reverbHighCutHz)) return false;
    if (!closeEnough(e.limiterThreshold, working.limiterThreshold)) return false;
    if (!closeEnough(e.limiterLookaheadMs, working.limiterLookaheadMs)) return false;
    if (!closeEnough(e.limiterAttackMs, working.limiterAttackMs)) return false;
    if (!closeEnough(e.limiterReleaseMs, working.limiterReleaseMs)) return false;
    return true;
}

void AppliedStateBadge(bool connected,
                       const svms::RuntimeLinkTelemetryV2* telemetry,
                       const ConfigValues& working, const char* scopeTooltip) {
    ImGui::SameLine();
    bool synced = connected && telemetry && LiveAppliedMatches(*telemetry, working);
    if (connected && telemetry && !synced) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::TextDisabled("PENDING");
    } else if (connected && telemetry) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetSuccess());
        ImGui::TextDisabled("APPLIED");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
        ImGui::TextDisabled("OFFLINE");
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(scopeTooltip ? scopeTooltip
            : "Engine applied state vs working copy");
        if (connected && telemetry && !synced) {
            ImGui::TextUnformatted("Working values differ from the engine's");
            ImGui::TextUnformatted("applied echo — awaiting the next live flush.");
        }
        ImGui::EndTooltip();
    }
}

void LiveBadge(const char* tooltip) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, GetSuccess());
    ImGui::TextDisabled("LIVE");
    ImGui::PopStyleColor();
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
}

void RestartRequiredBadge() {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
    ImGui::TextDisabled("RESTART");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Requires driver restart to take effect.");
        ImGui::EndTooltip();
    }
}

} // namespace svms::cfg
