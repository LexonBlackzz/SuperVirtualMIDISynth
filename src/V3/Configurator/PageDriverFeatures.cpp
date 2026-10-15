#include "PageDriverFeatures.h"
#include "DriverLink.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"

#include <cstdio>
#include <map>
#include <string>

namespace svms::cfg {

void DrawDriverFeaturesPage(svms::RuntimeLinkClient* client) {
    if (!client || !client->IsOpen()) {
        ImGui::TextDisabled("Connect to a running driver to see what it offers.");
        return;
    }
    client->Poll();

    SectionHeader("UNKNOWN BY THIS CONFIGURATOR VERSION");
    ImGui::TextDisabled(
        "The driver reports these controls, but this configurator has no dedicated page for them yet.");
    ImGui::Spacing();

    // Slider state while dragging: the driver echoes values back, which would
    // otherwise yank the handle to the previous value mid-drag.
    static std::map<std::string, float> editing;
    int shown = 0;
    for (const auto& param : client->Params()) {
        if (svms::RuntimeLinkClient::IsKnownParam(param.id)) continue;
        ++shown;
        ImGui::PushID(param.id.c_str());
        const float current = static_cast<float>(client->ParamValue(param.id));
        const std::string label = param.label.empty() ? param.id : param.label;
        if (param.type == "bool") {
            bool on = current != 0.0f;
            if (ImGui::Checkbox(label.c_str(), &on))
                client->SetParam(param.id, on ? 1.0 : 0.0);
        } else if (param.type == "enum" && !param.options.empty()) {
            int index = static_cast<int>(current);
            const char* preview = index >= 0 && index < static_cast<int>(param.options.size())
                ? param.options[static_cast<size_t>(index)].c_str() : "?";
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo(label.c_str(), preview)) {
                for (int i = 0; i < static_cast<int>(param.options.size()); ++i)
                    if (ImGui::Selectable(param.options[static_cast<size_t>(i)].c_str(), i == index))
                        client->SetParam(param.id, i);
                ImGui::EndCombo();
            }
        } else {
            float value = editing.count(param.id) ? editing[param.id] : current;
            ImGui::SetNextItemWidth(260.0f);
            const std::string format = param.unit.empty() ? std::string("%.3f")
                : "%.3f " + param.unit;
            bool changed = false;
            if (param.type == "int") {
                int iv = static_cast<int>(value);
                changed = ImGui::SliderInt(label.c_str(), &iv, static_cast<int>(param.min),
                                           static_cast<int>(param.max));
                value = static_cast<float>(iv);
            } else {
                changed = ImGui::SliderFloat(label.c_str(), &value, static_cast<float>(param.min),
                                             static_cast<float>(param.max), format.c_str());
            }
            if (ImGui::IsItemActive()) editing[param.id] = value;
            else editing.erase(param.id);
            if (changed) client->SetParam(param.id, value);
        }
        if (!param.group.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", param.group.c_str());
        }
        ImGui::PopID();
    }
    if (shown == 0)
        ImGui::TextDisabled("Nothing: every control this driver offers has a page of its own.");

    ImGui::Spacing();
    SectionHeader("UNKNOWN METRICS");
    int metrics = 0;
    for (const auto& metric : client->MetricSchema()) {
        if (svms::RuntimeLinkClient::IsKnownMetric(metric.id)) continue;
        ++metrics;
        ImGui::Text("%s", (metric.label.empty() ? metric.id : metric.label).c_str());
        ImGui::SameLine(280.0f);
        ImGui::TextDisabled("%.3f %s", client->MetricValue(metric.id), metric.unit.c_str());
    }
    if (metrics == 0) ImGui::TextDisabled("None.");
}

} // namespace svms::cfg
