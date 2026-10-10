#include "PageReverb.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "Theme.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace svms::cfg {
namespace {

float HzToKHz(float hz) {
    return hz / 1000.0f;
}

float KHzToHz(float khz) {
    return khz * 1000.0f;
}

ImVec4 WithAlpha(ImVec4 color, float alpha) {
    color.w = ImClamp(alpha, 0.0f, 1.0f);
    return color;
}

// Illustrative decay trace on an inset display. It is a model of the reverb
// tail (exponential late decay, a damped high band, early reflections), not a
// measurement: it moves with the parameters so the shape of a setting is
// visible before it is heard.
void DrawDecayTrace(const ImVec2& size, const ConfigValues& v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& th = GetThemeSettings();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    DrawLcdFrame(dl, p, q, 8, 4);

    const bool on = v.enableReverb;
    const float alpha = on ? 1.0f : 0.35f;
    const float t60 = 0.25f + v.reverbDecay * 3.0f + v.reverbRoomSize * 0.8f;
    const float tMax = (std::max)(1.0f, std::ceil(t60 * 1.15f * 2.0f) * 0.5f);
    const float preS = v.reverbPreDelayMs * 0.001f;
    const float lateAmp = ImClamp(v.reverbLateLevel / 1.5f * 0.9f + 0.1f, 0.1f, 1.0f);

    const float left = p.x + 12.0f;
    const float right = q.x - 12.0f;
    const float top = p.y + 22.0f;
    const float bottom = q.y - 22.0f;
    const float plotW = right - left;
    const float plotH = bottom - top;
    auto xAt = [&](float tSec) { return left + plotW * ImClamp(tSec / tMax, 0.0f, 1.0f); };
    auto yAt = [&](float a) { return bottom - plotH * ImClamp(a, 0.0f, 1.0f); };

    const ImVec4 accent = WithAlpha(th.accent, alpha);
    // Late tail: filled envelope plus a brighter outline.
    const float step = 3.0f;
    float prevY = yAt(0.0f);
    float prevX = left;
    for (float x = left; x <= right; x += step) {
        const float t = (x - left) / plotW * tMax;
        float a = 0.0f;
        if (t >= preS) a = lateAmp * std::exp(-6.91f * (t - preS) / t60);
        const float y = yAt(a);
        dl->AddLine(ImVec2(x, y), ImVec2(x, bottom),
                    ImGui::GetColorU32(WithAlpha(th.accent, 0.13f * alpha)), step);
        if (x > left) {
            dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, y),
                        ImGui::GetColorU32(accent), 1.8f);
        }
        prevX = x;
        prevY = y;
    }
    // High band: decays faster as damping rises.
    {
        const float t60Hf = t60 * (1.0f - 0.75f * v.reverbDamping);
        float px = left, py = bottom;
        for (float x = left; x <= right; x += step) {
            const float t = (x - left) / plotW * tMax;
            const float a = t >= preS
                ? lateAmp * 0.8f * std::exp(-6.91f * (t - preS) / t60Hf) : 0.0f;
            const float y = yAt(a);
            if (x > left) {
                dl->AddLine(ImVec2(px, py), ImVec2(x, y),
                            ImGui::GetColorU32(WithAlpha(th.text, 0.35f * alpha)), 1.0f);
            }
            px = x;
            py = y;
        }
    }
    // Early reflections.
    static const float kEarlyMs[] = {4.0f, 9.0f, 15.0f, 23.0f, 31.0f, 42.0f};
    for (int i = 0; i < 6; ++i) {
        const float t = preS + kEarlyMs[i] * 0.001f * (0.5f + v.reverbRoomSize);
        const float a = ImClamp(v.reverbEarlyLevel / 1.5f, 0.0f, 1.0f) * (1.0f - 0.13f * i);
        dl->AddLine(ImVec2(xAt(t), bottom), ImVec2(xAt(t), yAt(a)),
                    ImGui::GetColorU32(WithAlpha(th.text, 0.55f * alpha)), 1.5f);
    }
    // Pre-delay marker.
    dl->AddLine(ImVec2(xAt(preS), top - 6.0f), ImVec2(xAt(preS), bottom),
                ImGui::GetColorU32(WithAlpha(GetWarning(), 0.55f * alpha)), 1.0f);

    PushMono();
    const ImU32 label = ImGui::GetColorU32(WithAlpha(th.accent, 0.55f));
    char buf[64];
    dl->AddText(ImVec2(p.x + 10.0f, p.y + 4.0f), label, "DECAY");
    std::snprintf(buf, sizeof(buf), "RT60 %.1fs", t60);
    const ImVec2 rt = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(q.x - rt.x - 10.0f, p.y + 4.0f),
                ImGui::GetColorU32(accent), buf);
    dl->AddText(ImVec2(p.x + 10.0f, q.y - ImGui::GetTextLineHeight() - 3.0f), label, "0");
    std::snprintf(buf, sizeof(buf), "%.1fs", tMax);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(q.x - ts.x - 10.0f, q.y - ImGui::GetTextLineHeight() - 3.0f), label, buf);
    std::snprintf(buf, sizeof(buf), "TONE %.0f Hz - %.1f kHz",
                  v.reverbLowCutHz, v.reverbHighCutHz * 0.001f);
    const ImVec2 tone = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p.x + (size.x - tone.x) * 0.5f,
                       q.y - ImGui::GetTextLineHeight() - 3.0f), label, buf);
    PopMono();

    ImGui::Dummy(size);
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

    PushMono();
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
    PopMono();
}

bool DrawLiveKnob(ConfigDocument& doc, float& value,
                  float minValue, float maxValue, float defaultValue,
                  const char* label, float size, const char* format,
                  svms::RLCommandType command,
                  float displayScale = 1.0f,
                  float (*displayFn)(float) = nullptr,
                  float (*inverseFn)(float) = nullptr) {
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - size) * 0.5f));

    KnobState knob = {
        value, minValue, maxValue, defaultValue,
        label, nullptr, size, displayScale, displayFn, inverseFn
    };
    if (!RotaryKnob(knob, format)) return false;

    value = knob.value;
    doc.MarkDirty();
    PushLiveFloat(command, knob.value);
    return true;
}

void DrawSpacePanel(ConfigDocument& doc, float knob) {
    auto& v = doc.Working();
    if (!BeginRackPanel("SPACE / STEREO")) { EndRackPanel(); return; }
    if (ImGui::BeginTable("##rv_space", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbMix, 0.0f, 1.0f, 0.25f, "MIX", knob, "%.0f%%",
                     svms::RLCommandType::SetReverbMix, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbRoomSize, 0.0f, 1.0f, 0.60f, "SIZE", knob, "%.0f%%",
                     svms::RLCommandType::SetReverbRoomSize, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbDecay, 0.0f, 1.0f, 0.50f, "DECAY", knob, "%.0f%%",
                     svms::RLCommandType::SetReverbDecay, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbPreDelayMs, 0.0f, 200.0f, 12.0f, "PRE-DELAY", knob,
                     "%.0f ms", svms::RLCommandType::SetReverbPreDelayMs);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbWidth, 0.0f, 1.0f, 1.0f, "WIDTH", knob, "%.0f%%",
                     svms::RLCommandType::SetReverbWidth, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbEarlyLevel, 0.0f, 1.5f, 0.35f, "EARLY", knob, "%.2f",
                     svms::RLCommandType::SetReverbEarlyLevel);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbLateLevel, 0.0f, 1.5f, 0.85f, "LATE", knob, "%.2f",
                     svms::RLCommandType::SetReverbLateLevel);
        ImGui::EndTable();
    }
    EndRackPanel();
}

void DrawTonePanel(ConfigDocument& doc, float knob) {
    auto& v = doc.Working();
    if (!BeginRackPanel("TEXTURE / MODULATION / TONE")) { EndRackPanel(); return; }
    if (ImGui::BeginTable("##rv_tone", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbDiffusion, 0.0f, 1.0f, 0.70f, "DIFFUSE", knob, "%.0f%%",
                     svms::RLCommandType::SetReverbDiffusion, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbDamping, 0.0f, 1.0f, 0.35f, "DAMPING", knob, "%.0f%%",
                     svms::RLCommandType::SetReverbDamping, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbModDepth, 0.0f, 1.0f, 0.30f, "MOD DEPTH", knob, "%.0f%%",
                     svms::RLCommandType::SetReverbModDepth, 100.0f);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbModRate, 0.0f, 1.0f, 0.35f, "MOD RATE", knob, "%.2f",
                     svms::RLCommandType::SetReverbModRate);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbLowCutHz, 0.0f, 2000.0f, 70.0f, "LOW CUT", knob,
                     "%.0f Hz", svms::RLCommandType::SetReverbLowCutHz);
        ImGui::TableNextColumn();
        DrawLiveKnob(doc, v.reverbHighCutHz, 1000.0f, 20000.0f, 16000.0f, "HIGH CUT",
                     knob, "%.1f kHz", svms::RLCommandType::SetReverbHighCutHz,
                     1.0f, HzToKHz, KHzToHz);
        ImGui::EndTable();
    }
    EndRackPanel();
}

} // namespace

void DrawDecayTraceMini(const ImVec2& size, const ConfigValues& values) {
    DrawDecayTrace(size, values);
}

void DrawReverbPage(ConfigDocument& doc) {
    PushEffectPageStyle();

    auto& values = doc.Working();
    const auto& live = GetLiveLinkContext();

    // Faceplate: decay display on the left, power and state on the right.
    if (BeginRackPanel("REVERB")) {
        const float rightW = 150.0f;
        const float spacing = 14.0f;
        const float lcdW = (std::max)(200.0f, ImGui::GetContentRegionAvail().x - rightW - spacing);
        const float lcdH = ImClamp(ImGui::GetIO().DisplaySize.y - 520.0f, 150.0f, 300.0f);
        DrawDecayTrace(ImVec2(lcdW, lcdH), values);
        ImGui::SameLine(0.0f, spacing);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(rightW, 6.0f));
        bool enabled = values.enableReverb;
        if (ToggleSwitch("POWER", &enabled, "Enable the FDN reverb effect.")) {
            values.enableReverb = enabled;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetReverbEnabled, enabled);
        }
        ImGui::Spacing();
        DrawHeaderState(live.connected, live.telemetry, values,
                        "All reverb parameters support live preview",
                        "Reverb group applied state vs working copy");
        ImGui::EndGroup();
    }
    EndRackPanel();

    const float width = ImGui::GetContentRegionAvail().x;
    const float knob = width > 900.0f ? 56.0f : 48.0f;
    if (ImGui::BeginTable("##reverb_panels", 2,
                          ImGuiTableFlags_SizingStretchSame |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawSpacePanel(doc, knob);
        ImGui::TableNextColumn();
        DrawTonePanel(doc, knob);
        ImGui::EndTable();
    }

    PopEffectPageStyle();
}

} // namespace svms::cfg
