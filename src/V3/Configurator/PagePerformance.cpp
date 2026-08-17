#include "PagePerformance.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <cstdio>

namespace svms::cfg {
namespace {

bool BeginSettingsTable(const char* id) {
    if (!ImGui::BeginTable(id, 3,
                           ImGuiTableFlags_SizingStretchProp |
                           ImGuiTableFlags_BordersInnerH |
                           ImGuiTableFlags_RowBg,
                           ImVec2(0.0f, 0.0f))) {
        return false;
    }
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 135.0f);
    return true;
}

void LabelCell(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip) {
        ImGui::SameLine();
        HelpMarker(tooltip);
    }
}

void RestartCell() {
    ImGui::TableNextColumn();
    RestartRequiredBadge();
}

} // namespace

void DrawPerformancePage(ConfigDocument& doc) {
    auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;

    SectionHeader("PERFORMANCE");

    if (BeginSettingsTable("##performance_settings")) {
        ImGui::TableNextRow();
        LabelCell("Maximum voices",
                  "Maximum number of simultaneously allocated/rendered primary voices. "
                  "This is not the same as source MIDI polyphony.");
        ImGui::TableNextColumn();
        int maxVoices = static_cast<int>(w.maxVoices);
        ImGui::SetNextItemWidth((std::min)(260.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputInt("##maxvoices", &maxVoices, 0, 0)) {
            maxVoices = (std::max)(1, (std::min)(524288, maxVoices));
            w.maxVoices = static_cast<uint32_t>(maxVoices);
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Voice presets");
        ImGui::TableNextColumn();
        static const int presetValues[] = {
            1000, 2048, 4096, 8192, 16384, 32768,
            65536, 131072, 262144, 524288
        };
        for (int i = 0; i < 10; ++i) {
            if (i > 0) ImGui::SameLine();
            char label[32];
            std::snprintf(label, sizeof(label), "%d###voices_%d", presetValues[i], i);
            const bool selected = static_cast<int>(w.maxVoices) == presetValues[i];
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.447f, 0.533f, 0.855f, 0.35f));
            }
            if (ImGui::SmallButton(label)) {
                w.maxVoices = static_cast<uint32_t>(presetValues[i]);
                doc.MarkDirty();
            }
            if (selected) ImGui::PopStyleColor();
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("restart");

        ImGui::TableNextRow();
        LabelCell("Render threads",
                  "Total voice-render lanes. 1 keeps voice rendering on the audio thread. "
                  "0 selects V3's automatic thread count.");
        ImGui::TableNextColumn();
        static const char* threadItems[] = {
            "Automatic", "1 (audio thread only)", "2", "3", "4", "6", "8",
            "10", "12", "16", "24", "32", "48", "64"
        };
        static const int threadValues[] = {
            0, 1, 2, 3, 4, 6, 8, 10, 12, 16, 24, 32, 48, 64
        };
        int threadIdx = 0;
        for (int i = 0; i < 14; ++i) {
            if (threadValues[i] == static_cast<int>(w.renderThreads)) {
                threadIdx = i;
                break;
            }
        }
        ImGui::SetNextItemWidth((std::min)(300.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::Combo("##renderthreads", &threadIdx, threadItems, 14)) {
            w.renderThreads = static_cast<uint32_t>(threadValues[threadIdx]);
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Correctness mode",
                  "Renders the complete configured pool at full quality. Disable only if "
                  "you understand the quality tradeoff.");
        ImGui::TableNextColumn();
        bool correctness = w.correctnessMode;
        if (ToggleSwitch("Full correctness", &correctness)) {
            w.correctnessMode = correctness;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetCorrectnessMode, correctness);
        }
        ImGui::TableNextColumn();
        if (lc.connected) LiveBadge("Applied live via RuntimeLink");
        AppliedStateBadge(lc.connected, lc.telemetry, w,
                          "Correctness-mode applied state vs working copy");

        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (w.maxVoices > 4096) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.20f, 1.0f));
        ImGui::TextWrapped("Extreme voice capacity. Actual realtime performance is workload and CPU dependent.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Capacity is not a realtime performance guarantee.");
    }
    ImGui::TextDisabled("Multicore scaling is workload-dependent.");

    if (lc.connected && t) {
        ImGui::Spacing();
        SectionHeader("LIVE");
        if (ImGui::BeginTable("##performance_live", 4,
                              ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("ACTIVE VOICES");
            ImGui::Text("%u / %u", t->activeVoices, t->maxVoices);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("CPU LOAD");
            ImGui::Text("%.1f%%", t->cpuLoadPercent);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("P99 BUDGET");
            ImGui::Text("%.0f%%", t->callbackP99Percent);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("DECIMATION");
            ImGui::Text("%u", t->decimationStep);
            ImGui::EndTable();
        }
    }
}

} // namespace svms::cfg
