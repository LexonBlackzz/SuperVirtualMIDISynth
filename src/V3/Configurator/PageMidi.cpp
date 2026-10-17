#include "PageMidi.h"
#include "PlatformUtil.h"
#include "ConfigDocument.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include "DriverLink.h"

#ifdef _WIN32
#include <mmeapi.h>
#endif

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
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 250.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 135.0f);
    return true;
}

void LabelCell(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextColumn();
    SettingLabel(label, tooltip);
}

void RestartCell() {
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    PushMono(0.78f);
    const float pillW = ImGui::CalcTextSize("RESTART").x + 12.0f;
    PopMono();
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - pillW) * 0.5f));
    RestartPill();
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
    uint32_t id = 0u;
    std::wstring name;
    std::string displayName;
};

std::string WideToUtf8Midi(const std::wstring& value) {
    return svms::cfg::util::WideToUtf8(value);
}

std::vector<MidiInputDevice> EnumerateMidiInputs() {
    std::vector<MidiInputDevice> result;
#ifndef _WIN32
    return result;  // Linux input routing is ALSA sequencer connections (aconnect), not a picker.
#else
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
#endif
}

} // namespace

void DrawMidiPage(ConfigDocument& doc) {
    auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();

    auto send = [&](svms::RLCommandType type, uint32_t param) {
        if (!lc.connected || !lc.client) return;
        char result[svms::kRuntimeLinkResultTextCapacity]{};
        lc.client->SendCommand(type, 0u, param, svms::RuntimeLiveStateV2{}, 100u, result);
        NoteLiveChange();
    };
    const ThemeSettings& th = GetThemeSettings();

    static std::vector<MidiInputDevice> inputDevices;
    static bool inputsEnumerated = false;
    if (!inputsEnumerated) {
        inputDevices = EnumerateMidiInputs();
        inputsEnumerated = true;
    }

    if (ImGui::BeginTable("##midi_top", 2,
                          ImGuiTableFlags_SizingStretchSame |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();

        // ======================================================= left
        ImGui::TableNextColumn();
        if (BeginRackPanel("VELOCITY")) {
            RestartPill();
            ImGui::SameLine();
            ImGui::TextDisabled("applies after restart");
            ImGui::Spacing();
            if (ImGui::BeginTable("##vel_knobs", 3, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                float curve = w.velocityCurve;
                if (PanelKnob("CURVE", &curve, 0.1f, 10.0f, 1.0f, "%.2f", 60.0f,
                              "Exponent applied to MIDI velocity. Values above 1 emphasize loud notes; values below 1 lift quieter notes.")) {
                    w.velocityCurve = curve;
                    doc.MarkDirty();
                }
                ImGui::TableNextColumn();
                float floor = w.velocityFloor;
                if (PanelKnob("FLOOR", &floor, 0.0f, 0.99f, 0.0f, "%.2f", 60.0f,
                              "Raises the minimum mapped loudness of notes that survive the ignore threshold. This is not the event-shedding threshold.")) {
                    w.velocityFloor = floor;
                    doc.MarkDirty();
                }
                ImGui::TableNextColumn();
                float ignore = static_cast<float>(w.velocityIgnoreBelow);
                if (PanelKnob("IGNORE BELOW", &ignore, 0.0f, 127.0f, 0.0f,
                              ignore < 0.5f ? "OFF" : "%.0f", 60.0f,
                              "MIDI note-ons with velocity strictly below this value are ignored. The threshold itself is still accepted.")) {
                    w.velocityIgnoreBelow = static_cast<uint32_t>(ignore + 0.5f);
                    doc.MarkDirty();
                }
                ImGui::EndTable();
            }
            ImGui::Dummy(ImVec2(0.0f, 12.0f));
            ImGui::TextDisabled("Master volume lives in the strip at the top.");
        }
        EndRackPanel();

        if (BeginRackPanel("TIMING / THROUGHPUT  (LIVE)")) {
            if (PanelLever("FAST PRODUCER  (LISTEN)", &w.fastProducer,
                           "Listening test, OFF by default. The first thread that submits MIDI (the player) feeds the engine through a private ring with plain stores instead of the shared queue lanes, cutting the per-message cost seen in the player's ns/msg readout. Event order from that thread is identical; only the tie-break against events from OTHER threads (resets, controller commands) can differ by a few dozen events. Other threads keep the old path.")) {
                doc.MarkDirty();
                send(svms::RLCommandType::SetFastProducer, w.fastProducer ? 1u : 0u);
            }
        }
        EndRackPanel();

        // ======================================================= right
        ImGui::TableNextColumn();
        if (BeginRackPanel("EVENT QUEUE")) {
            static const char* overflowKeys[] = {"PRIORITY", "LOSSLESS"};
            int mode = w.overflowMode == 0 ? 0 : 1;
            if (PanelKeys("OVERFLOW MODE", &mode, overflowKeys, 2,
                          "Priority allows quiet note-ons to be shed under severe pressure. Lossless favors backpressure instead.",
                          true)) {
                w.overflowMode = mode;
                doc.MarkDirty();
            }
            ImGui::Spacing();
            ImGui::Spacing();

            if (ImGui::BeginTable("##q_fields", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                int cap = static_cast<int>((std::min)(w.eventRingCapacity, 2000000000u));
                if (PanelLcdInt("QUEUE CAPACITY", &cap, 4096, 2000000000,
                                "Total raw MIDI ingress capacity. Larger values absorb denser bursts but reserve more address space. This does not change callback work.",
                                true)) {
                    w.eventRingCapacity = static_cast<uint32_t>(cap);
                    doc.MarkDirty();
                }
                ImGui::TableNextColumn();
                int perCb = static_cast<int>((std::min)(w.maxEventsPerBlock, 2000000000u));
                if (PanelLcdInt("EVENTS / CALLBACK", &perCb, 1, 2000000000,
                                "Maximum due MIDI events dispatched in one audio callback. Excess work remains ordered and becomes explicitly late; changing this does not resize the ingress queue.",
                                true)) {
                    w.maxEventsPerBlock = static_cast<uint32_t>(perCb);
                    doc.MarkDirty();
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Spacing();
                int high = static_cast<int>(w.highPriorityVelocity);
                if (PanelLcdInt("PROTECTED VELOCITY", &high, 1, 127,
                                "MIDI note-ons at or above this velocity are protected from priority shedding.",
                                true)) {
                    w.highPriorityVelocity = static_cast<uint32_t>(high);
                    doc.MarkDirty();
                }
                ImGui::TableNextColumn();
                ImGui::Spacing();
                int shed = static_cast<int>(w.shedStartPercent);
                if (PanelLcdInt("SHED STARTS AT %", &shed, 1, 99,
                                "Queue fill percentage where priority shedding begins.", true)) {
                    w.shedStartPercent = static_cast<uint32_t>(shed);
                    doc.MarkDirty();
                }
                ImGui::EndTable();
            }
            if (w.eventRingCapacity > 4000000u) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Text("Large queue: up to ~%.1f GB of address space if it ever fills. Most Black MIDI plays fine at 2M.",
                            w.eventRingCapacity * 74.0 / 1.0e9);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            ImGui::Spacing();
            static const char* tuningKeys[] = {"12EDO", "31EDO"};
            int tuning = w.tuningEdo == 31u ? 1 : 0;
            if (PanelKeys("TUNING", &tuning, tuningKeys, 2,
                          "Normal: 12 semitones per octave, keys 0-255. 31EDO: 31 steps per octave; key 155 is middle C. Percussion keeps its drum mapping.",
                          true)) {
                w.tuningEdo = tuning ? 31u : 12u;
                doc.MarkDirty();
            }
        }
        EndRackPanel();

        if (BeginRackPanel("MIDI INPUT")) {
            bool enabled = w.midiInputEnabled;
            if (PanelLever("ROUTE PHYSICAL INPUT", &enabled,
                           "The driver opens the selected system MIDI input and sends it directly through SVMS with arrival-time QPC timestamps. Host midiIn APIs remain available independently.",
                           true)) {
                w.midiInputEnabled = enabled;
                doc.MarkDirty();
            }
            ImGui::Spacing();
            PanelCaption("INPUT DEVICE",
                         "An empty selection follows the first available system MIDI input. A named selection is matched case-insensitively at driver startup.",
                         true);
            std::string preview = w.midiInputDevice.empty()
                ? "First available input" : WideToUtf8Midi(w.midiInputDevice);
            ImGui::SetNextItemWidth((std::max)(160.0f, ImGui::GetContentRegionAvail().x - 110.0f));
            PushMono();
            ImGui::PushStyleColor(ImGuiCol_Text, th.accent);
            const bool open = ImGui::BeginCombo("##midiinputdevice", preview.c_str());
            ImGui::PopStyleColor();
            PopMono();
            if (open) {
                if (ImGui::Selectable("First available input", w.midiInputDevice.empty())) {
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
            ImGui::SameLine();
            if (KeyButton("Refresh", ImVec2(92.0f, 26.0f))) {
                inputDevices = EnumerateMidiInputs();
                inputsEnumerated = true;
            }
            if (inputDevices.empty())
                ImGui::TextDisabled("No system MIDI input devices found");
            else
                ImGui::TextDisabled("%u input%s found; short MIDI and SysEx are supported",
                                    static_cast<unsigned>(inputDevices.size()),
                                    inputDevices.size() == 1u ? "" : "s");
        }
        EndRackPanel();
        ImGui::EndTable();
    }

    // ------------------------------------------------- live event pipeline
    if (lc.connected && lc.telemetry) {
        const auto& t = *lc.telemetry;
        const uint64_t pagedPressureCount =
            static_cast<uint64_t>(t.compiledPagedCount) + t.scheduledBacklogCount;
        const float pagePressure = w.eventRingCapacity != 0u
            ? 100.0f * static_cast<float>(pagedPressureCount) /
                  static_cast<float>(w.eventRingCapacity)
            : 0.0f;
        if (BeginRackPanel("LIVE EVENT PIPELINE")) {
            struct Cell { const char* label; char value[48]; };
            Cell cells[5];
            auto set = [&](int i, const char* label, const char* fmt, auto... args) {
                cells[i].label = label;
                std::snprintf(cells[i].value, sizeof(cells[i].value), fmt, args...);
            };
            set(0, "RAW INGRESS", "%u", t.rawIngressCount);
            set(1, "COMPILED PAGES", "%u", t.compiledPagedCount);
            set(2, "SCHEDULED BACKLOG", "%u", t.scheduledBacklogCount);
            set(3, "PAGE-POOL PRESSURE", "%.1f%%", pagePressure);
            set(4, "SCHED / DISPATCH", "%.2f%% / %.2f%%", t.schedulerPercent, t.eventDispatchPercent);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            PushMono(0.95f);
            const float lh = ImGui::GetTextLineHeight();
            const float height = lh * 2.0f + 26.0f;
            DrawLcdFrame(dl, p0, ImVec2(p0.x + width, p0.y + height));
            for (int i = 0; i < 5; ++i) {
                const float x = p0.x + 16.0f + i * (width - 32.0f) / 5.0f;
                dl->AddText(ImVec2(x, p0.y + 10.0f),
                            ImGui::GetColorU32(ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.5f)),
                            cells[i].label);
                dl->AddText(ImVec2(x, p0.y + 10.0f + lh + 2.0f),
                            ImGui::GetColorU32(th.accent), cells[i].value);
            }
            PopMono();
            ImGui::Dummy(ImVec2(width, height));
            const uint64_t merged =
                (static_cast<uint64_t>(t.noteMergedHigh) << 32u) | t.noteMergedLow;
            if (w.noteMerge || merged != 0u || t.effectiveVoices > t.activeVoices) {
                ImGui::TextDisabled(
                    "note merge + coalescing: %u effective voices (peak %u) in %u live voices | %llu note-ons folded",
                    t.effectiveVoices, t.effectiveVoicesPeak, t.activeVoices,
                    static_cast<unsigned long long>(merged));
            }
        }
        EndRackPanel();
    }
}

} // namespace svms::cfg
