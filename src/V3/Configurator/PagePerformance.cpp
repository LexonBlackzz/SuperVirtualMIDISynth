#include "PagePerformance.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace svms::cfg {

void DrawPerformancePage(ConfigDocument& doc) {
    auto& w = doc.Working();
    auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;

    SectionHeader("MAXIMUM VOICES");

    int maxVoices = static_cast<int>(w.maxVoices);
    bool changed = LabeledInt("Max voices", &maxVoices, 1, 524288,
        "Maximum number of simultaneously allocated/rendered primary voices. "
        "This is not the same as source MIDI polyphony.");
    if (changed) {
        w.maxVoices = static_cast<uint32_t>(maxVoices);
        doc.MarkDirty();
    }
    RestartRequiredBadge();

    static const char* presets[] = {
        "1000", "2048", "4096", "8192", "16384", "32768",
        "65536", "131072", "262144", "524288"
    };
    static const int presetValues[] = {
        1000, 2048, 4096, 8192, 16384, 32768,
        65536, 131072, 262144, 524288
    };

    ImGui::Text("Presets:");
    ImGui::SameLine();
    for (int i = 0; i < 10; ++i) {
        if (i > 0) ImGui::SameLine();
        char btnLabel[32];
        snprintf(btnLabel, sizeof(btnLabel), "%s###p%d", presets[i], i);
        bool isCurrent = (static_cast<int>(w.maxVoices) == presetValues[i]);
        if (isCurrent) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.447f, 0.533f, 0.855f, 0.3f));
        }
        if (ImGui::SmallButton(btnLabel)) {
            w.maxVoices = static_cast<uint32_t>(presetValues[i]);
            doc.MarkDirty();
        }
        if (isCurrent) {
            ImGui::PopStyleColor();
        }
    }

    if (w.maxVoices > 4096) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.20f, 1.0f));
        ImGui::TextWrapped(
            "Extreme voice capacity. Actual realtime performance is workload "
            "and CPU dependent.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Capacity is not a realtime performance guarantee.");

    ImGui::Spacing();
    SectionHeader("RENDER THREADS");

    int threads = static_cast<int>(w.renderThreads);
    static const char* threadItems[] = {
        "Automatic", "1 (audio thread only)", "2", "3", "4", "6", "8",
        "10", "12", "16", "24", "32", "48", "64"
    };
    static const int threadValues[] = {
        0, 1, 2, 3, 4, 6, 8, 10, 12, 16, 24, 32, 48, 64
    };

    int threadIdx = 0;
    for (int i = 0; i < 14; ++i) {
        if (threadValues[i] == threads) {
            threadIdx = i;
            break;
        }
    }

    ImGui::PushItemWidth(300.0f);
    if (ImGui::Combo("##threads", &threadIdx, threadItems, 14)) {
        w.renderThreads = static_cast<uint32_t>(threadValues[threadIdx]);
        doc.MarkDirty();
    }
    ImGui::PopItemWidth();

    HelpMarker(
        "Total voice-render lanes. 1 keeps voice rendering on the audio "
        "thread. 0 selects V3's automatic thread count.");
    RestartRequiredBadge();

    ImGui::TextDisabled("Multicore scaling is workload-dependent.");

    if (t) {
        ImGui::Spacing();
        ImGui::Text("Live active voices: %u / %u", t->activeVoices, t->maxVoices);
        ImGui::Text("Decimation step: %u", t->decimationStep);
    }

    ImGui::Spacing();
    SectionHeader("QUALITY");

    bool correctness = w.correctnessMode;
    if (ToggleSwitch("Correctness Mode", &correctness,
                     "Renders the complete configured pool at full quality. "
                     "Disable only if you understand the quality tradeoff.")) {
        w.correctnessMode = correctness;
        doc.MarkDirty();
        PushLiveBool(svms::RLCommandType::SetCorrectnessMode, correctness);
    }
    if (lc.connected) LiveBadge("Applied live via RuntimeLink");
}

} // namespace svms::cfg
