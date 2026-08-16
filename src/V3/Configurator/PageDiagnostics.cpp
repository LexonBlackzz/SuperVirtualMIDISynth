#include "PageDiagnostics.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"

namespace svms::cfg {

void DrawDiagnosticsPage(ConfigDocument& doc) {
    auto& w = doc.Working();
    auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;

    SectionHeader("DIAGNOSTICS");

    bool diagEnabled = w.diagnosticsEnabled;
    if (ToggleSwitch("Diagnostics Enabled", &diagEnabled,
                     "Enable V3's runtime diagnostic subsystem.")) {
        w.diagnosticsEnabled = diagEnabled;
        doc.MarkDirty();
    }
    RestartRequiredBadge();

    bool diagWindow = w.diagnosticsWindow;
    if (ToggleSwitch("Diagnostics Window", &diagWindow,
                     "Shows V3's built-in runtime diagnostic window when enabled.")) {
        w.diagnosticsWindow = diagWindow;
        doc.MarkDirty();
    }
    RestartRequiredBadge();

    bool debugOutput = w.diagnosticsDebugOutput;
    if (ToggleSwitch("Debug Output", &debugOutput,
                     "Emits additional diagnostic messages to debugger output. "
                     "Use DebugView or similar to view.")) {
        w.diagnosticsDebugOutput = debugOutput;
        doc.MarkDirty();
    }
    RestartRequiredBadge();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (lc.connected && t) {
        SectionHeader("LIVE EVENT PIPELINE");

        ImGui::Text("Events submitted: %llu",
                    static_cast<unsigned long long>(t->eventsSubmitted));
        ImGui::Text("Events accepted: %llu",
                    static_cast<unsigned long long>(t->eventsAccepted));
        ImGui::Text("Events dropped: %llu",
                    static_cast<unsigned long long>(t->eventsDropped));
        ImGui::Text("Events dispatched: %llu",
                    static_cast<unsigned long long>(t->eventsDispatched));

        float dropRate = t->eventsSubmitted > 0
            ? 100.0f * static_cast<float>(t->eventsDropped) /
              static_cast<float>(t->eventsSubmitted)
            : 0.0f;
        ImGui::TextDisabled("Drop rate: %.2f%%", dropRate);

        ImGui::Spacing();
        SectionHeader("LIVE CALLBACK PERFORMANCE");

        // Callback percentiles are PERCENT OF BUDGET (P95 = 48 means the
        // P95 callback consumed 48% of its budget).  Budget ms is one
        // device buffer: bufferFrames / sampleRate * 1000.
        const float budgetMs = t->bufferFrames > 0u
            ? 1000.0f * static_cast<float>(t->bufferFrames)
              / static_cast<float>(t->sampleRate)
            : 0.0f;
        ImGui::Text("CPU load: %.1f%%", t->cpuLoadPercent);
        ImGui::Text("Callback P95: %.1f%% of budget (%.2f ms)",
                    t->callbackP95Percent, budgetMs * t->callbackP95Percent / 100.0f);
        ImGui::Text("Callback P99: %.1f%% of budget (%.2f ms)",
                    t->callbackP99Percent, budgetMs * t->callbackP99Percent / 100.0f);
        ImGui::Text("Callback P99.9: %.1f%% of budget (%.2f ms)",
                    t->callbackP999Percent, budgetMs * t->callbackP999Percent / 100.0f);
        ImGui::Text("Over-budget callbacks: %llu",
                    static_cast<unsigned long long>(t->overBudgetCallbacks));
        ImGui::Text("Max consecutive over-budget: %u",
                    t->maxConsecutiveOverBudget);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
        ImGui::TextWrapped(
            "Diagnostics settings require V3 to be restarted to take effect.");
        ImGui::PopStyleColor();
    }
}

} // namespace svms::cfg
