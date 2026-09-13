#include "PageChannelLimiter.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "Theme.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace svms::cfg {
namespace {

constexpr float kMeterFloorDb = -60.0f;
constexpr float kGrDisplayMaxDb = 24.0f;
constexpr int kChannelGrid = 16;  // MIDI channels 1..16 (internal 0..15)

float LinearToDb(float linear) {
    if (!std::isfinite(linear) || linear <= 0.000001f) return kMeterFloorDb;
    return (std::max)(kMeterFloorDb, 20.0f * std::log10(linear));
}

float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// One vertical gain-reduction bar in the 4x4 channel grid, styled after the
// limiter page's GR bar: fill grows downward from the top as the channel's
// bus is pushed below its threshold.
void DrawChannelGrBar(const char* id, float reductionDb, float peakLinear,
                      bool engineActive, const ImVec2& size) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 bg = ImGui::GetColorU32(ImVec4(0.055f, 0.062f, 0.074f, 1.0f));
    const ImU32 border = ImGui::GetColorU32(
        engineActive ? ImVec4(0.16f, 0.18f, 0.22f, 1.0f)
                     : ImVec4(0.10f, 0.11f, 0.13f, 1.0f));
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 3.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border, 3.0f);

    // Pre-limit peak, drawn faintly behind the reduction fill so the user
    // can tell "quiet channel" from "channel riding its threshold".
    const float peakDb = LinearToDb(peakLinear);
    const float peakNorm =
        ImClamp((peakDb - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
    if (engineActive && peakNorm > 0.0f) {
        dl->AddRectFilled(
            ImVec2(pos.x + 2.0f,
                   pos.y + size.y * (1.0f - peakNorm)),
            ImVec2(pos.x + size.x - 2.0f, pos.y + size.y - 2.0f),
            ImGui::GetColorU32(ImVec4(0.25f, 0.76f, 0.43f, 0.30f)), 2.0f);
    }

    const float gr = ImClamp(reductionDb, 0.0f, kGrDisplayMaxDb);
    const float norm = gr / kGrDisplayMaxDb;
    if (engineActive && norm > 0.0f) {
        dl->AddRectFilled(
            ImVec2(pos.x + 2.0f, pos.y + 2.0f),
            ImVec2(pos.x + size.x - 2.0f,
                   pos.y + 2.0f + (size.y - 4.0f) * norm),
            ImGui::GetColorU32(ImVec4(0.94f, 0.68f, 0.16f, 0.95f)), 2.0f);
    }

    ImGui::Dummy(size);
    ImGui::PopID();
}

void DrawChannelGrid(const svms::RuntimeLinkTelemetryV2* telemetry,
                     bool telemetryAvailable) {
    const bool engineActive = telemetryAvailable &&
                              telemetry->channelLimiterEnabled != 0u;
    const float barW = 34.0f;
    const float barH = 120.0f;
    const float labelH = ImGui::GetFontSize() + 6.0f;

    if (ImGui::BeginTable("##channel_grid", 4,
                          ImGuiTableFlags_SizingStretchSame)) {
        for (int row = 0; row < 4; ++row) {
            ImGui::TableNextRow();
            for (int col = 0; col < 4; ++col) {
                const int index = row * 4 + col;
                if (index >= kChannelGrid) continue;
                ImGui::TableNextColumn();
                char id[16];
                std::snprintf(id, sizeof(id), "##cl%d", index);
                const float gr = telemetryAvailable
                    ? telemetry->channelLimiterGainReductionDb[index]
                    : 0.0f;
                const float peak = telemetryAvailable
                    ? telemetry->channelLimiterInputPeak[index]
                    : 0.0f;
                DrawChannelGrBar(id, gr, peak, engineActive,
                                 ImVec2(barW, barH));

                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 pos = ImGui::GetItemRectMin();
                char label[8];
                std::snprintf(label, sizeof(label), "%d", index + 1);
                const ImVec2 ts = ImGui::CalcTextSize(label);
                dl->AddText(ImVec2(pos.x + (barW - ts.x) * 0.5f,
                                   pos.y + barH + 3.0f),
                            ImGui::GetColorU32(
                                gr > 0.05f ? ImVec4(0.94f, 0.68f, 0.16f, 1.0f)
                                           : ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                            label);
                // Reserve row height including the label.
                ImGui::Dummy(ImVec2(barW, labelH));
            }
        }
        ImGui::EndTable();
    }
}

} // namespace

void DrawChannelLimiterPage(ConfigDocument& doc) {
    PushEffectPageStyle();

    auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;
    const bool telemetryAvailable = lc.connected && t != nullptr;

    auto pushAll = [&]() {
        PushLiveChannelLimiter(w.channelLimiterEnabled,
                               w.channelLimiterThreshold,
                               w.channelLimiterReleaseMs);
    };

    constexpr float headerSideWidth = 190.0f;
    constexpr float headerRowHeight = 40.0f;
    if (ImGui::BeginTable("##cl_header", 3,
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("enable", ImGuiTableColumnFlags_WidthFixed,
                                headerSideWidth);
        ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed,
                                headerSideWidth);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, headerRowHeight);

        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             (headerRowHeight - 22.0f) * 0.5f);
        bool enabled = w.channelLimiterEnabled;
        if (ToggleSwitch("##cl_enabled_switch", &enabled,
                         "Limit each MIDI channel bus independently before "
                         "the master chain.")) {
            w.channelLimiterEnabled = enabled;
            doc.MarkDirty();
            pushAll();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("ENABLED");

        ImGui::TableNextColumn();
        const char* title = "PER-CHANNEL LIMITER";
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        const float titleHeight = ImGui::GetTextLineHeight();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             (headerRowHeight - titleHeight) * 0.5f);
        const float start = ImGui::GetCursorPosX();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(start + (width - titleSize.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.55f, 1.0f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             (headerRowHeight - titleHeight) * 0.5f);
        const bool engineEcho = telemetryAvailable &&
                                t->channelLimiterEnabled != 0u;
        const bool synced = !telemetryAvailable ||
                            engineEcho == w.channelLimiterEnabled;
        const char* stateText = !telemetryAvailable
            ? "OFFLINE" : (synced ? "APPLIED" : "PENDING");
        const ImVec4 stateColor = !telemetryAvailable
            ? GetMutedText()
            : (synced ? GetSuccess() : GetWarning());
        float totalWidth = ImGui::CalcTextSize(stateText).x;
        if (telemetryAvailable)
            totalWidth += ImGui::CalcTextSize("LIVE").x +
                          ImGui::GetStyle().ItemSpacing.x;
        const float startX = ImGui::GetCursorPosX();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(startX + (std::max)(0.0f, avail - totalWidth));
        if (telemetryAvailable) {
            ImGui::PushStyleColor(ImGuiCol_Text, GetSuccess());
            ImGui::TextUnformatted("LIVE");
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
        ImGui::TextUnformatted(stateText);
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Spacing();

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float topHeight = 240.0f;
    if (ImGui::BeginTable("##cl_top", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch,
                                0.85f);
        ImGui::TableSetupColumn("meters", ImGuiTableColumnFlags_WidthStretch,
                                1.15f);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, topHeight);

        ImGui::TableNextColumn();
        ImGui::BeginChild("##cl_control_panel",
                          ImVec2(0.0f, topHeight - 6.0f), false,
                          ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float knobSize = avail > 360.0f ? 92.0f : 78.0f;

            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.68f, 0.71f, 0.76f, 1.0f));
            ImGui::TextUnformatted("CONTROLS");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            // Classic zero-latency limiter per channel: threshold engages
            // the reduction, release shapes how the bus recovers.
            if (ImGui::BeginTable("##cl_knobs", 2,
                                  ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                const float start = ImGui::GetCursorPosX();
                const float colAvail = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(start + (colAvail - knobSize) * 0.5f);
                float threshold = w.channelLimiterThreshold;
                KnobState ks = { threshold, 0.0316227766f, 1.0f, 0.5011872336272722f,
                                 "THRESHOLD", nullptr, knobSize, 1.0f,
                                 LinearToDb };
                if (RotaryKnob(ks, "%.1f dB")) {
                    w.channelLimiterThreshold = ks.value;
                    doc.MarkDirty();
                    pushAll();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(
                        "Per-channel ceiling (linear domain). Each MIDI "
                        "channel bus is limited independently to it before "
                        "the sum reaches reverb and the master limiter.");
                    ImGui::EndTooltip();
                }

                ImGui::TableNextColumn();
                const float start2 = ImGui::GetCursorPosX();
                const float colAvail2 = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(start2 + (colAvail2 - knobSize) * 0.5f);
                float release = w.channelLimiterReleaseMs;
                KnobState ks2 = { release, 20.0f, 1000.0f, 150.0f,
                                  "RELEASE", "ms", knobSize };
                if (RotaryKnob(ks2, "%.0f ms")) {
                    w.channelLimiterReleaseMs = ks2.value;
                    doc.MarkDirty();
                    pushAll();
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
            ImGui::TextWrapped(
                "Classic zero-latency topology: 16 stereo buses (one per "
                "MIDI channel) are limited independently, then summed into "
                "the master chain. Transients bite instantly and the master "
                "limiter stays the final ceiling. Purely post-render - "
                "disabled means bit-identical output.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("##cl_meter_panel",
                          ImVec2(0.0f, topHeight - 6.0f), false,
                          ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextDisabled("CHANNEL ACTIVITY");
        ImGui::Spacing();
        DrawChannelGrid(t, telemetryAvailable);
        ImGui::EndChild();
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (telemetryAvailable) {
        if (t->channelLimiterEnabled != 0u) {
            float maxGr = 0.0f;
            int maxChannel = -1;
            for (int i = 0; i < kChannelGrid; ++i) {
                const float gr = t->channelLimiterGainReductionDb[i];
                if (gr > maxGr) {
                    maxGr = gr;
                    maxChannel = i;
                }
            }
            if (maxChannel >= 0 && maxGr > 0.05f) {
                ImGui::TextDisabled(
                    "Bus %d is riding the threshold at %.1f dB reduction; "
                    "peak %.1f dB.",
                    maxChannel + 1, maxGr,
                    LinearToDb(t->channelLimiterInputPeak[maxChannel]));
            } else {
                ImGui::TextDisabled(
                    "No channel is above the threshold right now.");
            }
        } else {
            ImGui::TextDisabled(
                "Per-channel limiting is off in the engine - enable it to "
                "see per-channel gain reduction here.");
        }
    } else {
        ImGui::TextDisabled(
            "Runtime telemetry unavailable - controls remain editable and "
            "live preview resumes when a driver connects.");
    }

    PopEffectPageStyle();
}

} // namespace svms::cfg
