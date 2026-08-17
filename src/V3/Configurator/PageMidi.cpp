#include "PageMidi.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace svms::cfg {
namespace {

bool BeginSettingsTable(const char* id) {
    if (!ImGui::BeginTable(id, 3,
                           ImGuiTableFlags_SizingStretchProp |
                           ImGuiTableFlags_BordersInnerH |
                           ImGuiTableFlags_RowBg)) {
        return false;
    }
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 185.0f);
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

bool InputU32(const char* id, uint32_t& value, uint32_t minValue, uint32_t maxValue) {
    uint32_t temp = value;
    ImGui::SetNextItemWidth((std::min)(300.0f, ImGui::GetContentRegionAvail().x));
    if (!ImGui::InputScalar(id, ImGuiDataType_U32, &temp, nullptr, nullptr, "%u")) {
        return false;
    }
    temp = (std::max)(minValue, (std::min)(maxValue, temp));
    value = temp;
    return true;
}

} // namespace

void DrawMidiPage(ConfigDocument& doc) {
    auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();

    SectionHeader("SYNTH SETTINGS");

    if (BeginSettingsTable("##synth_settings")) {
        ImGui::TableNextRow();
        LabelCell("Master volume",
                  "Master output volume multiplier. 1.0 is unity gain.");
        ImGui::TableNextColumn();
        float master = w.masterVolume;
        ImGui::SetNextItemWidth((std::min)(360.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderFloat("##mastervolume", &master, 0.0f, 4.0f, "%.2f")) {
            w.masterVolume = master;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetMasterVolume, master);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f dB",
                            20.0f * std::log10((std::max)(master, 0.001f)));
        ImGui::TableNextColumn();
        if (lc.connected) LiveBadge("Applied live via RuntimeLink");
        AppliedStateBadge(lc.connected, lc.telemetry, w,
                          "Master-volume applied state vs working copy");

        ImGui::TableNextRow();
        LabelCell("Velocity curve",
                  "Exponent applied to MIDI velocity. Values above 1 emphasize loud notes; "
                  "values below 1 lift quieter notes.");
        ImGui::TableNextColumn();
        float curve = w.velocityCurve;
        ImGui::SetNextItemWidth((std::min)(360.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderFloat("##velocitycurve", &curve, 0.1f, 10.0f, "%.2f")) {
            w.velocityCurve = curve;
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Velocity floor",
                  "Raises the minimum mapped loudness of notes that survive the ignore threshold. "
                  "This is not the event-shedding threshold.");
        ImGui::TableNextColumn();
        float floor = w.velocityFloor;
        ImGui::SetNextItemWidth((std::min)(360.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderFloat("##velocityfloor", &floor, 0.0f, 0.99f, "%.2f")) {
            w.velocityFloor = floor;
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Ignore velocity below",
                  "MIDI note-ons with velocity strictly below this value are ignored. "
                  "The threshold itself is still accepted.");
        ImGui::TableNextColumn();
        int ignore = static_cast<int>(w.velocityIgnoreBelow);
        ImGui::SetNextItemWidth((std::min)(220.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputInt("##velocityignore", &ignore, 0, 0)) {
            ignore = (std::max)(0, (std::min)(127, ignore));
            w.velocityIgnoreBelow = static_cast<uint32_t>(ignore);
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::EndTable();
    }

    ImGui::Spacing();
    SectionHeader("EVENT QUEUE");

    if (BeginSettingsTable("##event_settings")) {
        ImGui::TableNextRow();
        LabelCell("Overflow mode",
                  "Priority allows quiet note-ons to be shed under severe pressure. "
                  "Lossless favors backpressure instead.");
        ImGui::TableNextColumn();
        static const char* modes[] = { "Priority", "Lossless" };
        int mode = w.overflowMode;
        ImGui::SetNextItemWidth((std::min)(300.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::Combo("##overflowmode", &mode, modes, 2)) {
            w.overflowMode = mode;
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Ring capacity",
                  "Total MIDI event ring capacity. Larger values absorb longer bursts but use more memory.");
        ImGui::TableNextColumn();
        if (InputU32("##ringcapacity", w.eventRingCapacity, 4096u, UINT32_MAX)) {
            if (w.maxEventsPerBlock > w.eventRingCapacity)
                w.maxEventsPerBlock = w.eventRingCapacity;
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("High-priority velocity",
                  "MIDI note-ons at or above this velocity are protected from priority shedding.");
        ImGui::TableNextColumn();
        uint32_t high = w.highPriorityVelocity;
        if (InputU32("##highpriority", high, 1u, 127u)) {
            w.highPriorityVelocity = high;
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Shed start percent",
                  "Queue fill percentage where priority shedding begins.");
        ImGui::TableNextColumn();
        uint32_t shed = w.shedStartPercent;
        if (InputU32("##shedstart", shed, 1u, 99u)) {
            w.shedStartPercent = shed;
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Max events per block",
                  "Maximum MIDI events processed during one audio callback. Must not exceed ring capacity.");
        ImGui::TableNextColumn();
        if (InputU32("##maxevents", w.maxEventsPerBlock, 1u,
                     (std::max)(1u, w.eventRingCapacity))) {
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::EndTable();
    }

    ImGui::Spacing();
    SectionHeader("MIDI INPUT");
    ImGui::TextDisabled(
        "MIDI In routing is not implemented yet. The winmm midiIn* compatibility entry points "
        "currently return MMSYSERR_BADDEVICEID.");
}

} // namespace svms::cfg
