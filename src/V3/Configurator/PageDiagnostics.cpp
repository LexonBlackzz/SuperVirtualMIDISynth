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

        ImGui::Text("CPU load: %.1f%%", t->cpuLoadPercent);
        ImGui::Text("Callback P95: %.2f ms",
                    t->callbackP95 * 1000.0f /
                    static_cast<float>(t->sampleRate));
        ImGui::Text("Callback P99: %.2f ms",
                    t->callbackP99 * 1000.0f /
                    static_cast<float>(t->sampleRate));
        ImGui::Text("Callback P99.9: %.2f ms",
                    t->callbackP999 * 1000.0f /
                    static_cast<float>(t->sampleRate));
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
