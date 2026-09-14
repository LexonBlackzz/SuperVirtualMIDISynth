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
constexpr float kGrBaseMaxDb = 24.0f;
constexpr float kGrHardMaxDb = 192.0f;
constexpr int kChannelGrid = 16;  // MIDI channels 1..16 (internal 0..15)

float LinearToDb(float linear) {
    if (!std::isfinite(linear) || linear <= 0.000001f) return kMeterFloorDb;
    return (std::max)(kMeterFloorDb, 20.0f * std::log10(linear));
}

// Black MIDI channel buses routinely peak 20-40 dB above full scale, so a
// fixed 24 dB GR scale saturates half the grid.  Step the shared display
// maximum above the worst channel, mirroring the limiter page's behaviour.
float SelectGrDisplayMax(float observedDb) {
    if (!std::isfinite(observedDb)) return kGrBaseMaxDb;
    observedDb = ImClamp(observedDb, 0.0f, kGrHardMaxDb);
    if (observedDb <= 24.0f) return 24.0f;
    if (observedDb <= 48.0f) return 48.0f;
    if (observedDb <= 96.0f) return 96.0f;
    return kGrHardMaxDb;
}

float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// One channel row in the 2x8 activity grid: channel number plus a compact
// horizontal gain-reduction bar, styled after the limiter page's GR bar —
// fill grows from the left as the channel's bus is pushed below its
// threshold, with the pre-limit peak drawn faintly underneath so the user
// can tell "quiet channel" from "channel riding its threshold".
void DrawChannelGrRow(int index, float reductionDb, float peakLinear,
                      bool engineActive, float grDisplayMaxDb) {
    char label[8];
    std::snprintf(label, sizeof(label), "%d", index + 1);
    const ImU32 labelColor = ImGui::GetColorU32(
        engineActive && reductionDb > 0.05f
            ? ImVec4(0.94f, 0.68f, 0.16f, 1.0f)
            : ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
    const float labelStartX = ImGui::GetCursorPosX();
    const float labelWidth = ImGui::CalcTextSize("16").x;
    ImGui::PushStyleColor(ImGuiCol_Text, labelColor);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    // Reserve two-digit label width for every row. Without this, channel 9
    // was the only right-column meter starting one digit farther left.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(labelStartX + labelWidth + 6.0f);

    const float rowHeight = ImGui::GetTextLineHeight();
    const float barHeight = rowHeight - 4.0f;
    const float barYOffset = 2.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 barPos(pos.x, pos.y + barYOffset);
    const ImVec2 barEnd(pos.x + barW, pos.y + barYOffset + barHeight);

    const ImU32 bg = ImGui::GetColorU32(ImVec4(0.055f, 0.062f, 0.074f, 1.0f));
    const ImU32 border = ImGui::GetColorU32(
        engineActive ? ImVec4(0.16f, 0.18f, 0.22f, 1.0f)
                     : ImVec4(0.10f, 0.11f, 0.13f, 1.0f));
    dl->AddRectFilled(barPos, barEnd, bg, 2.0f);
    dl->AddRect(barPos, barEnd, border, 2.0f);

    if (engineActive) {
        const float peakDb = LinearToDb(peakLinear);
        const float peakNorm =
            ImClamp((peakDb - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
        if (peakNorm > 0.0f) {
            dl->AddRectFilled(
                ImVec2(barPos.x + 2.0f, barPos.y + 2.0f),
                ImVec2(barPos.x + 2.0f +
                           (barW - 4.0f) * peakNorm,
                       barEnd.y - 2.0f),
                ImGui::GetColorU32(ImVec4(0.25f, 0.76f, 0.43f, 0.30f)), 2.0f);
        }

        const float gr = ImClamp(reductionDb, 0.0f, grDisplayMaxDb);
        const float norm = gr / grDisplayMaxDb;
        if (norm > 0.0f) {
            dl->AddRectFilled(
                ImVec2(barPos.x + 2.0f, barPos.y + 2.0f),
                ImVec2(barPos.x + 2.0f + (barW - 4.0f) * norm,
                       barEnd.y - 2.0f),
                ImGui::GetColorU32(ImVec4(0.94f, 0.68f, 0.16f, 0.95f)), 2.0f);
        }
    }

    ImGui::Dummy(ImVec2(barW, rowHeight));
}

void DrawChannelGrid(const svms::RuntimeLinkTelemetryV2* telemetry,
                     bool telemetryAvailable) {
    const bool engineActive = telemetryAvailable &&
                              telemetry->channelLimiterEnabled != 0u;
    float worstGr = 0.0f;
    if (engineActive) {
        for (int i = 0; i < kChannelGrid; ++i) {
            worstGr = (std::max)(worstGr,
                                 telemetry->channelLimiterGainReductionDb[i]);
        }
    }
    const float grDisplayMaxDb = SelectGrDisplayMax(worstGr);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 3.0f));
    if (ImGui::BeginTable("##channel_grid", 2,
                          ImGuiTableFlags_SizingStretchSame)) {
        for (int row = 0; row < 8; ++row) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawChannelGrRow(row,
                             telemetryAvailable
                                 ? telemetry->channelLimiterGainReductionDb[row]
                                 : 0.0f,
                             telemetryAvailable
                                 ? telemetry->channelLimiterInputPeak[row]
                                 : 0.0f,
                             engineActive, grDisplayMaxDb);
            ImGui::TableNextColumn();
            DrawChannelGrRow(row + 8,
                             telemetryAvailable
                                 ? telemetry->channelLimiterGainReductionDb[row + 8]
                                 : 0.0f,
                             telemetryAvailable
                                 ? telemetry->channelLimiterInputPeak[row + 8]
                                 : 0.0f,
                             engineActive, grDisplayMaxDb);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    if (engineActive && grDisplayMaxDb > 24.0f) {
        ImGui::TextDisabled("Scale: 0 - %.0f dB gain reduction", grDisplayMaxDb);
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
        if (ToggleSwitch("ENABLED", &enabled,
                         "Limit each MIDI channel bus independently before "
                         "the master chain.")) {
            w.channelLimiterEnabled = enabled;
            doc.MarkDirty();
            pushAll();
        }

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

    const float topHeight = 210.0f;
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
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(
                        "How fast a channel bus recovers after being pushed "
                        "below its threshold.");
                    ImGui::EndTooltip();
                }
                ImGui::EndTable();
            }
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
