#include "PageMidi.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include "../SVMSRuntimeLink.h"

#include <mmeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

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
    ImGui::AlignTextToFramePadding();

    constexpr const char* label = "RESTART";
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - labelWidth) * 0.5f));

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.20f, 1.0f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Requires driver restart to take effect.");
        ImGui::EndTooltip();
    }
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

struct MidiInputDevice {
    UINT id = 0u;
    std::wstring name;
    std::string displayName;
};

std::string WideToUtf8Midi(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), bytes,
                        nullptr, nullptr);
    return result;
}

std::vector<MidiInputDevice> EnumerateMidiInputs() {
    std::vector<MidiInputDevice> result;
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0u || length + 11u >= MAX_PATH) return result;
    std::wcscat(path, L"\\winmm.dll");
    HMODULE winmm = LoadLibraryW(path);
    if (!winmm) return result;
    using GetNumProc = UINT (WINAPI*)(void);
    using GetCapsProc = MMRESULT (WINAPI*)(UINT_PTR, LPMIDIINCAPSW, UINT);
    GetNumProc getNum = reinterpret_cast<GetNumProc>(
        GetProcAddress(winmm, "midiInGetNumDevs"));
    GetCapsProc getCaps = reinterpret_cast<GetCapsProc>(
        GetProcAddress(winmm, "midiInGetDevCapsW"));
    if (getNum && getCaps) {
        const UINT count = getNum();
        result.reserve(count);
        for (UINT id = 0u; id < count; ++id) {
            MIDIINCAPSW caps{};
            if (getCaps(id, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
                continue;
            MidiInputDevice device;
            device.id = id;
            device.name = caps.szPname;
            device.displayName = WideToUtf8Midi(device.name);
            result.push_back(std::move(device));
        }
    }
    FreeLibrary(winmm);
    return result;
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
        LabelCell("Queue capacity",
                  "Total raw MIDI ingress capacity. Larger values absorb denser bursts but reserve more memory. This does not change callback work.");
        ImGui::TableNextColumn();
        uint32_t eventBuffer = w.eventRingCapacity;
        if (InputU32("##evbuffer", eventBuffer, 4096u, UINT32_MAX)) {
            w.eventRingCapacity = eventBuffer;
            doc.MarkDirty();
        }
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Events per callback",
                  "Maximum due MIDI events dispatched in one audio callback. Excess work remains ordered and becomes explicitly late; changing this does not resize the ingress queue.");
        ImGui::TableNextColumn();
        uint32_t callbackEvents = w.maxEventsPerBlock;
        if (InputU32("##eventspercallback", callbackEvents, 1u, UINT32_MAX)) {
            w.maxEventsPerBlock = callbackEvents;
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
        LabelCell("Note-on coalescing",
                  "OFF by default: every note-on spawns a voice at its exact "
                  "timestamp, preserving retrigger timing precision. When "
                  "enabled, repeated hits of the same key within a fixed "
                  "20 ms window spawn one voice per N hits (velocity stacking "
                  "compensates loudness). Use only for extreme black-MIDI "
                  "workloads that would otherwise starve the audio thread.");
        ImGui::TableNextColumn();
        bool collapseOn = w.noteOnCollapseThreshold > 1u;
        if (ImGui::Checkbox("##noteoncollapse", &collapseOn)) {
            if (!collapseOn) {
                w.noteOnCollapseThreshold = 1u;
            } else if (w.noteOnCollapseThreshold <= 1u) {
                w.noteOnCollapseThreshold = 32u;
            }
            doc.MarkDirty();
            if (lc.connected && lc.client) {
                char collapseResult[svms::kRuntimeLinkResultTextCapacity]{};
                lc.client->SendCommand(
                    svms::RLCommandType::SetNoteOnCollapse, 0u,
                    w.noteOnCollapseThreshold,
                    svms::RuntimeLiveStateV2{}, 100u, collapseResult);
            }
        }
        ImGui::SameLine();
        if (collapseOn) {
            static const uint32_t kThresholds[] = {
                2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u,
                1024u, 2048u, 4096u, 8192u, 16384u, 32768u, 65536u
            };
            static const char* kThresholdLabels[] = {
                "1 voice per 2 hits", "1 voice per 4 hits",
                "1 voice per 8 hits", "1 voice per 16 hits",
                "1 voice per 32 hits", "1 voice per 64 hits",
                "1 voice per 128 hits", "1 voice per 256 hits",
                "1 voice per 512 hits", "1 voice per 1024 hits",
                "1 voice per 2048 hits", "1 voice per 4096 hits",
                "1 voice per 8192 hits", "1 voice per 16384 hits",
                "1 voice per 32768 hits", "1 voice per 65536 hits"
            };
            int tIdx = 4; // default 32
            for (int i = 0; i < 16; ++i) {
                if (w.noteOnCollapseThreshold == kThresholds[i]) {
                    tIdx = i;
                    break;
                }
            }
            ImGui::SetNextItemWidth((std::min)(220.0f,
                ImGui::GetContentRegionAvail().x));
            if (ImGui::BeginCombo("##noteoncollapsesethreshold",
                                  kThresholdLabels[tIdx])) {
                for (int i = 0; i < 16; ++i) {
                    const bool selected = i == tIdx;
                    if (ImGui::Selectable(kThresholdLabels[i], selected)) {
                        w.noteOnCollapseThreshold = kThresholds[i];
                        doc.MarkDirty();
                        if (lc.connected && lc.client) {
                            char collapseResult[
                                svms::kRuntimeLinkResultTextCapacity]{};
                            lc.client->SendCommand(
                                svms::RLCommandType::SetNoteOnCollapse, 0u,
                                kThresholds[i],
                                svms::RuntimeLiveStateV2{}, 100u,
                                collapseResult);
                        }
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextUnformatted("Disabled (exact retrigger timing)");
        }
        ImGui::TableNextColumn();
        if (lc.connected) LiveBadge("Applied live via RuntimeLink");

        ImGui::EndTable();
    }

    if (lc.connected && lc.telemetry) {
        const auto& t = *lc.telemetry;
        const uint64_t pagedPressureCount =
            static_cast<uint64_t>(t.compiledPagedCount) +
            t.scheduledBacklogCount;
        const float pagePressure = w.eventRingCapacity != 0u
            ? 100.0f * static_cast<float>(pagedPressureCount) /
                  static_cast<float>(w.eventRingCapacity)
            : 0.0f;
        ImGui::Spacing();
        SectionHeader("LIVE EVENT PIPELINE");
        ImGui::Text("Raw ingress: %u", t.rawIngressCount);
        ImGui::Text("Compiled pages: %u events", t.compiledPagedCount);
        ImGui::Text("Scheduled backlog: %u events", t.scheduledBacklogCount);
        ImGui::Text("Page-pool pressure: %.1f%%", pagePressure);
        ImGui::Text("Scheduler / dispatch: %.2f%% / %.2f%%",
                    t.schedulerPercent, t.eventDispatchPercent);
    }

    ImGui::Spacing();
    SectionHeader("MIDI INPUT");
    static std::vector<MidiInputDevice> inputDevices;
    static bool inputsEnumerated = false;
    if (!inputsEnumerated) {
        inputDevices = EnumerateMidiInputs();
        inputsEnumerated = true;
    }

    if (BeginSettingsTable("##midi_input_settings")) {
        ImGui::TableNextRow();
        LabelCell("Route physical input",
                  "The driver opens the selected system MIDI input and sends it directly "
                  "through SVMS with arrival-time QPC timestamps. Host midiIn APIs remain "
                  "available independently.");
        ImGui::TableNextColumn();
        bool enabled = w.midiInputEnabled;
        if (ImGui::Checkbox("##midiinputenabled", &enabled)) {
            w.midiInputEnabled = enabled;
            doc.MarkDirty();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(enabled ? "Enabled" : "Disabled");
        RestartCell();

        ImGui::TableNextRow();
        LabelCell("Input device",
                  "An empty selection follows the first available system MIDI input. "
                  "A named selection is matched case-insensitively at driver startup.");
        ImGui::TableNextColumn();
        std::string preview = w.midiInputDevice.empty()
            ? "First available input"
            : WideToUtf8Midi(w.midiInputDevice);
        ImGui::SetNextItemWidth((std::min)(420.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##midiinputdevice", preview.c_str())) {
            const bool firstSelected = w.midiInputDevice.empty();
            if (ImGui::Selectable("First available input", firstSelected)) {
                w.midiInputDevice.clear();
                doc.MarkDirty();
            }
            for (const MidiInputDevice& device : inputDevices) {
                const bool selected = !w.midiInputDevice.empty() &&
                    _wcsicmp(w.midiInputDevice.c_str(), device.name.c_str()) == 0;
                if (ImGui::Selectable(device.displayName.c_str(), selected)) {
                    w.midiInputDevice = device.name;
                    doc.MarkDirty();
                }
            }
            ImGui::EndCombo();
        }
        RestartCell();
        ImGui::EndTable();
    }
    if (ImGui::Button("Refresh MIDI inputs")) {
        inputDevices = EnumerateMidiInputs();
        inputsEnumerated = true;
    }
    ImGui::SameLine();
    if (inputDevices.empty())
        ImGui::TextDisabled("No system MIDI input devices found");
    else
        ImGui::TextDisabled("%u input%s found; short MIDI and SysEx are supported",
                            static_cast<unsigned>(inputDevices.size()),
                            inputDevices.size() == 1u ? "" : "s");
}

} // namespace svms::cfg
