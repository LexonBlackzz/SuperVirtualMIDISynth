#include "PagePerformance.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "Theme.h"
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

void CenteredStatusCell(const char* label, const ImVec4& color,
                        const char* tooltip) {
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - labelWidth) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void RestartCell() {
    CenteredStatusCell("RESTART", GetWarning(),
                       "Requires driver restart to take effect.");
}

bool VoiceCapFitsCurrentPool(const LiveLinkContext& lc, uint32_t requested) {
    if (!lc.connected || !lc.telemetry || lc.telemetry->maxVoices == 0u)
        return true;
    return requested <= lc.telemetry->maxVoices;
}

void VoiceCapStatusCell(const LiveLinkContext& lc, uint32_t requested) {
    if (!VoiceCapFitsCurrentPool(lc, requested)) {
        CenteredStatusCell(
            "RESTART", GetWarning(),
            "The requested cap is larger than the physical voice pool allocated when this driver instance started. Save the value and restart the driver to grow that pool. Lowering the cap, or raising it within the existing pool, is live.");
        return;
    }

    CenteredStatusCell(
        "LIVE", GetSuccess(),
        "Changes apply immediately while the requested cap fits inside the voice pool allocated at driver startup. Lowering the cap force-releases the least-important excess voices over about 50 ms.");
}

void QueueVoiceCapIfLive(const LiveLinkContext& lc, uint32_t requested) {
    // Do not queue a command we already know the current driver cannot honor.
    // The edited value is still kept in the working config so Save + restart
    // can grow the physical pool deliberately.
    if (VoiceCapFitsCurrentPool(lc, requested))
        PushLiveMaxVoices(requested);
}

float BlockBudgetMs(const svms::RuntimeLinkTelemetryV2& t) {
    if (t.sampleRate == 0u) return 0.0f;
    return static_cast<float>(t.bufferFrames) * 1000.0f /
           static_cast<float>(t.sampleRate);
}

float PercentToMs(float percent, float budgetMs) {
    return budgetMs * percent * 0.01f;
}

void BudgetMetric(const char* label, const char* format, ...) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
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
                  "Logical primary-voice ceiling. Lowering it live releases excess voices using the normal steal-priority policy and a short anti-click release. The current physical pool size is fixed until restart.");
        ImGui::TableNextColumn();
        int maxVoices = static_cast<int>(w.maxVoices);
        ImGui::SetNextItemWidth((std::min)(260.0f, ImGui::GetContentRegionAvail().x));
        const bool editedMaxVoices = ImGui::InputInt("##maxvoices", &maxVoices, 0, 0);
        if (editedMaxVoices) {
            maxVoices = (std::max)(1, (std::min)(524288, maxVoices));
            w.maxVoices = static_cast<uint32_t>(maxVoices);
            doc.MarkDirty();
        }
        // Avoid shedding thousands of voices because the user temporarily
        // deleted a digit while typing. Commit the text field when editing is
        // finished; presets below are applied immediately on click.
        if (editedMaxVoices && !ImGui::IsItemActive()) {
            QueueVoiceCapIfLive(lc, w.maxVoices);
        } else if (ImGui::IsItemDeactivatedAfterEdit()) {
            QueueVoiceCapIfLive(lc, w.maxVoices);
        }
        VoiceCapStatusCell(lc, w.maxVoices);

        ImGui::TableNextRow();
        LabelCell("Voice presets");
        ImGui::TableNextColumn();
        static const int presetValues[] = {
            1024, 2048, 4096, 8192, 16384, 32768,
            65536, 131072, 262144, 524288
        };
        for (int i = 0; i < 10; ++i) {
            if (i > 0) ImGui::SameLine();
            char label[32];
            std::snprintf(label, sizeof(label), "%d###voices_%d", presetValues[i], i);
            const bool selected = static_cast<int>(w.maxVoices) == presetValues[i];
            if (selected) {
                ImVec4 selectedButton = GetAccent();
                selectedButton.w = 0.35f;
                ImGui::PushStyleColor(ImGuiCol_Button, selectedButton);
            }
            if (ImGui::SmallButton(label)) {
                w.maxVoices = static_cast<uint32_t>(presetValues[i]);
                doc.MarkDirty();
                QueueVoiceCapIfLive(lc, w.maxVoices);
            }
            if (selected) ImGui::PopStyleColor();
        }
        VoiceCapStatusCell(lc, w.maxVoices);

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
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::TextWrapped("Extreme voice capacity. Actual realtime performance is workload and CPU dependent.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Capacity is not a realtime performance guarantee.");
    }
    ImGui::TextDisabled("Multicore scaling is workload-dependent.");

    if (lc.connected && t) {
        ImGui::Spacing();
        SectionHeader("LIVE RENDER BUDGET");

        const float budgetMs = BlockBudgetMs(*t);
        const float load = (std::max)(0.0f, t->cpuLoadPercent);
        const float headroom = (std::max)(0.0f, 100.0f - load);
        const float renderMs = PercentToMs(load, budgetMs);
        const uint32_t appliedVoiceCap = t->live.maxVoices != 0u
            ? t->live.maxVoices : t->maxVoices;

        if (ImGui::BeginTable("##performance_live_primary", 4,
                              ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            BudgetMetric("ACTIVE VOICES", "%u / %u", t->activeVoices, appliedVoiceCap);
            BudgetMetric("RENDER / BUDGET", "%.2f / %.2f ms", renderMs, budgetMs);
            BudgetMetric("RENDER LOAD", "%.1f%%", load);
            BudgetMetric("HEADROOM", "%.1f%%", headroom);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::BeginTable("##performance_live_percentiles", 4,
                              ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            BudgetMetric("P95", "%.2f ms  (%.0f%%)",
                         PercentToMs(t->callbackP95Percent, budgetMs),
                         t->callbackP95Percent);
            BudgetMetric("P99", "%.2f ms  (%.0f%%)",
                         PercentToMs(t->callbackP99Percent, budgetMs),
                         t->callbackP99Percent);
            BudgetMetric("P99.9", "%.2f ms  (%.0f%%)",
                         PercentToMs(t->callbackP999Percent, budgetMs),
                         t->callbackP999Percent);
            BudgetMetric("OVER BUDGET", "%llu  |  streak %u",
                         static_cast<unsigned long long>(t->overBudgetCallbacks),
                         t->maxConsecutiveOverBudget);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Physical pool: %u voices   |   Buffer: %u frames @ %u Hz   |   Decimation: %ux",
                            t->maxVoices, t->bufferFrames, t->sampleRate,
                            (std::max)(1u, t->decimationStep));

        if (w.maxVoices > t->maxVoices && t->maxVoices != 0u) {
            ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
            ImGui::TextWrapped(
                "Requested cap: %u. This driver instance only allocated %u physical voices, so growing to the requested value requires a restart.",
                w.maxVoices, t->maxVoices);
            ImGui::PopStyleColor();
        }
    }
}

} // namespace svms::cfg
