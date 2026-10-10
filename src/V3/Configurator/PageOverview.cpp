#include "PageOverview.h"
#include "ConfigDocument.h"
#include "ConfiguratorApp.h"
#include "PageReverb.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLink.h"
#include "../SVMSRuntimeLinkProtocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace svms::cfg {
namespace {

ImVec4 WithAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

float Norm(float linear) {
    if (!(linear > 0.000001f)) return 0.0f;
    const float db = 20.0f * std::log10(linear);
    return ImClamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
}

bool LiveKnob(ConfigDocument& doc, float& value, float minValue, float maxValue,
              float defaultValue, const char* label, const char* format,
              svms::RLCommandType command, float displayScale = 1.0f,
              float (*displayFn)(float) = nullptr,
              float (*inverseFn)(float) = nullptr) {
    constexpr float size = 46.0f;
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - size) * 0.5f));
    KnobState knob = {value, minValue, maxValue, defaultValue,
                      label, nullptr, size, displayScale, displayFn, inverseFn};
    if (!RotaryKnob(knob, format)) return false;
    value = knob.value;
    doc.MarkDirty();
    PushLiveFloat(command, knob.value);
    return true;
}

// Right-aligned key that jumps to a stage.
void OpenKey(Page page) {
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    const float w = 92.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (std::max)(0.0f, ImGui::GetContentRegionAvail().x - w));
    if (KeyButton("OPEN  >", ImVec2(w, 26.0f))) {
        if (ConfiguratorApp* app = GetLiveLinkContext().app) app->NavigateTo(page);
    }
}

std::string Utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
                                        static_cast<int>(ws.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

void SynthModule(ConfigDocument& doc) {
    auto& w = doc.Working();
    if (BeginRackPanel("SYNTH")) {
        const ThemeSettings& th = GetThemeSettings();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float height = 46.0f;
        DrawLcdFrame(dl, p, ImVec2(p.x + width, p.y + height));
        PushMono();
        std::string name = "(none - local fallback)";
        std::string dir;
        if (!w.soundFontPath.empty()) {
            const std::filesystem::path sf(w.soundFontPath);
            name = Utf8(sf.filename().wstring());
            dir = Utf8(sf.parent_path().wstring());
        }
        dl->PushClipRect(p, ImVec2(p.x + width - 8.0f, p.y + height), true);
        dl->AddText(ImVec2(p.x + 10.0f, p.y + 6.0f), ImGui::GetColorU32(th.accent), name.c_str());
        dl->AddText(ImVec2(p.x + 10.0f, p.y + 6.0f + ImGui::GetTextLineHeight() + 2.0f),
                    ImGui::GetColorU32(WithAlpha(th.accent, 0.5f)), dir.c_str());
        dl->PopClipRect();
        PopMono();
        ImGui::Dummy(ImVec2(width, height));

        ImGui::Spacing();
        PushMono();
        ImGui::TextDisabled("MAX VOICES");
        PopMono();
        static const uint32_t values[] = {1024, 2048, 4096, 8192, 16384, 65536};
        static const char* labels[] = {"1k", "2k", "4k", "8k", "16k", "64k"};
        int idx = -1;
        for (int i = 0; i < 6; ++i)
            if (values[i] == w.maxVoices) idx = i;
        if (KeyGroup("##home_voices", &idx, labels, 6) && idx >= 0) {
            w.maxVoices = values[idx];
            doc.MarkDirty();
            PushLiveMaxVoices(w.maxVoices);
        }
        if (idx < 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%u", w.maxVoices);
        }
        OpenKey(Page::Synth);
    }
    EndRackPanel();
}

void ReverbModule(ConfigDocument& doc) {
    auto& w = doc.Working();
    if (BeginRackPanel("REVERB")) {
        bool enabled = w.enableReverb;
        if (ToggleSwitch("POWER", &enabled)) {
            w.enableReverb = enabled;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetReverbEnabled, enabled);
        }
        ImGui::Spacing();
        DrawDecayTraceMini(ImVec2(ImGui::GetContentRegionAvail().x, 70.0f), w);
        ImGui::Spacing();
        if (ImGui::BeginTable("##home_rv", 3, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            LiveKnob(doc, w.reverbMix, 0.0f, 1.0f, 0.25f, "MIX", "%.0f%%",
                     svms::RLCommandType::SetReverbMix, 100.0f);
            ImGui::TableNextColumn();
            LiveKnob(doc, w.reverbRoomSize, 0.0f, 1.0f, 0.60f, "SIZE", "%.0f%%",
                     svms::RLCommandType::SetReverbRoomSize, 100.0f);
            ImGui::TableNextColumn();
            LiveKnob(doc, w.reverbDecay, 0.0f, 1.0f, 0.50f, "DECAY", "%.0f%%",
                     svms::RLCommandType::SetReverbDecay, 100.0f);
            ImGui::EndTable();
        }
        OpenKey(Page::Reverb);
    }
    EndRackPanel();
}

float DbToLinearHome(float db) {
    return std::pow(10.0f, db / 20.0f);
}

float LinearToDbHome(float linear) {
    if (linear <= 0.000001f) return -60.0f;
    return (std::max)(-60.0f, (std::min)(0.0f, 20.0f * std::log10(linear)));
}

void LimiterModule(ConfigDocument& doc) {
    auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();
    const bool tel = lc.connected && lc.telemetry != nullptr;
    if (BeginRackPanel("LIMITER")) {
        bool enabled = w.limiterEnabled;
        if (ToggleSwitch("ENABLED", &enabled)) {
            w.limiterEnabled = enabled;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetLimiterEnabled, enabled);
        }
        ImGui::Spacing();
        if (ImGui::BeginTable("##home_lm", 3, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 size(14.0f, 66.0f);
                const float startX = ImGui::GetCursorPosX();
                const float avail = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(startX + (std::max)(0.0f, (avail - 34.0f) * 0.5f));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float l = tel ? Norm(lc.telemetry->limiterOutputPeakL) : 0.0f;
                const float r = tel ? Norm(lc.telemetry->limiterOutputPeakR) : 0.0f;
                DrawLedLadder(dl, p, size, l, 0.0f, false, nullptr);
                DrawLedLadder(dl, ImVec2(p.x + 20.0f, p.y), size, r, 0.0f, false, nullptr);
                ImGui::Dummy(ImVec2(34.0f, 66.0f));
            }
            ImGui::TableNextColumn();
            LiveKnob(doc, w.limiterThreshold, 0.1f, 1.0f, 0.95f, "THRESH", "%.1f dB",
                     svms::RLCommandType::SetLimiterThreshold, 1.0f, LinearToDbHome,
                     DbToLinearHome);
            ImGui::TableNextColumn();
            LiveKnob(doc, w.limiterReleaseMs, 1.0f, 5000.0f, 100.0f, "RELEASE", "%.0f ms",
                     svms::RLCommandType::SetLimiterRelease);
            ImGui::EndTable();
        }
        OpenKey(Page::Limiter);
    }
    EndRackPanel();
}

void MidiModule(ConfigDocument& doc) {
    auto& w = doc.Working();
    if (BeginRackPanel("MIDI / EVENTS")) {
        const ThemeSettings& th = GetThemeSettings();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        PushMono();
        const float lh = ImGui::GetTextLineHeight();
        const float height = lh * 3.0f + 28.0f;
        DrawLcdFrame(dl, p, ImVec2(p.x + width, p.y + height));
        char buf[96];
        auto row = [&](int i, const char* label, const char* value) {
            const float y = p.y + 8.0f + i * (lh + 4.0f);
            dl->AddText(ImVec2(p.x + 10.0f, y), ImGui::GetColorU32(WithAlpha(th.accent, 0.5f)), label);
            const ImVec2 vs = ImGui::CalcTextSize(value);
            dl->AddText(ImVec2(p.x + width - vs.x - 10.0f, y), ImGui::GetColorU32(th.accent), value);
        };
        row(0, "OVERFLOW", w.overflowMode == 0 ? "PRIORITY" : "LOSSLESS");
        std::snprintf(buf, sizeof(buf), "%.1fM EVENTS", w.eventRingCapacity / 1.0e6);
        row(1, "QUEUE", buf);
        std::snprintf(buf, sizeof(buf), "%.1fM EVENTS", w.maxEventsPerBlock / 1.0e6);
        row(2, "PER CALLBACK", buf);
        PopMono();
        ImGui::Dummy(ImVec2(width, height));
        if (w.eventRingCapacity > 4000000u) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("Large queue: up to ~%.1f GB of address space if it ever fills. "
                        "Most Black MIDI plays fine at 2M.",
                        w.eventRingCapacity * 74.0 / 1.0e9);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        OpenKey(Page::Midi);
    }
    EndRackPanel();
}

} // namespace

// Home: the front panel. Live modules at a glance; OPEN jumps to a stage.
void DrawOverviewPage(ConfigDocument& doc) {
    if (ImGui::BeginTable("##home", 2,
                          ImGuiTableFlags_SizingStretchSame |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        SynthModule(doc);
        ImGui::TableNextColumn();
        ReverbModule(doc);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        LimiterModule(doc);
        ImGui::TableNextColumn();
        MidiModule(doc);
        ImGui::EndTable();
    }
}

} // namespace svms::cfg
