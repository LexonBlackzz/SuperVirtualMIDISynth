#include "configurator/PageMidi.h"
#include "configurator/ConfigDocument.h"
#include "configurator/ConfiguratorApp.h"
#include "configurator/DriverLink.h"
#include "configurator/ModernWidgets.h"
#include "configurator/PlatformUtil.h"
#include "configurator/Widgets.h"
#include "imgui.h"
#include "driver/SVMSRuntimeLinkProtocol.h"

#ifdef _WIN32
#include <mmeapi.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace svms::cfg {
namespace {

struct MidiInputDevice {
    std::wstring name;
    std::string displayName;
};

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
    using GetNumProc = UINT(WINAPI*)(void);
    using GetCapsProc = MMRESULT(WINAPI*)(UINT_PTR, LPMIDIINCAPSW, UINT);
    GetNumProc getNum = reinterpret_cast<GetNumProc>(GetProcAddress(winmm, "midiInGetNumDevs"));
    GetCapsProc getCaps = reinterpret_cast<GetCapsProc>(GetProcAddress(winmm, "midiInGetDevCapsW"));
    if (getNum && getCaps) {
        const UINT count = getNum();
        for (UINT id = 0u; id < count; ++id) {
            MIDIINCAPSW caps{};
            if (getCaps(id, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
            MidiInputDevice device;
            device.name = caps.szPname;
            device.displayName = util::WideToUtf8(device.name);
            result.push_back(std::move(device));
        }
    }
    FreeLibrary(winmm);
    return result;
#endif
}

void NoteText(const char* text, const ImVec4* color = nullptr) {
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, color ? ImGui::ColorConvertFloat4ToU32(*color) : ui::Tok().muted);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::PopFont();
}

// Number field as a row control.
bool NumberRow(const char* anchor, const char* title, const char* desc, uint32_t* value,
               uint32_t lo, uint32_t hi, const char* id) {
    const float s = ui::Scale();
    ui::BeginRow(anchor, title, desc, ImVec2(150.0f * s, 38.0f * s));
    int v = static_cast<int>((std::min)(*value, static_cast<uint32_t>(2000000000u)));
    bool changed = false;
    if (ui::NumberField(id, &v, static_cast<int>(lo), static_cast<int>((std::min)(hi, 2000000000u)), 150.0f) &&
        static_cast<uint32_t>(v) != *value) {
        *value = static_cast<uint32_t>(v);
        changed = true;
    }
    ui::EndRow();
    return changed;
}

void KeyboardCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    static std::vector<MidiInputDevice> inputs;
    static bool enumerated = false;
    if (!enumerated) {
        inputs = EnumerateMidiInputs();
        enumerated = true;
    }
    ui::BeginCard("##midiin", nullptr, nullptr, 0.0f, "midiIn");
    ui::BeginRow("midiIn", "Play from a MIDI keyboard",
                 "Routes a physical MIDI input straight into SVMS, with exact arrival timing.",
                 ImVec2(46.0f * s, 28.0f * s));
    if (ui::Toggle("##midiin", &w.midiInputEnabled)) doc.MarkDirty();
    ui::EndRow();
    if (w.midiInputEnabled) {
        ui::BeginNest();
#ifdef _WIN32
        std::vector<std::string> names{"First available input"};
        int current = 0;
        for (size_t i = 0; i < inputs.size(); ++i) {
            names.push_back(inputs[i].displayName);
            if (!w.midiInputDevice.empty() && _wcsicmp(w.midiInputDevice.c_str(), inputs[i].name.c_str()) == 0)
                current = static_cast<int>(i + 1);
        }
        if (!w.midiInputDevice.empty() && current == 0) {
            names.push_back("Missing: " + util::WideToUtf8(w.midiInputDevice));
            current = static_cast<int>(names.size()) - 1;
        }
        std::vector<const char*> items;
        for (const std::string& n : names) items.push_back(n.c_str());
        char found[48];
        if (inputs.empty()) std::snprintf(found, sizeof(found), "No system MIDI input devices found.");
        else std::snprintf(found, sizeof(found), "%u input%s found.", static_cast<unsigned>(inputs.size()), inputs.size() == 1u ? "" : "s");
        ui::BeginRow("midiDev", "Input device", found,
                     ImVec2(240.0f * s + 8.0f * s + ui::ButtonSize("Refresh").x, 38.0f * s));
        int sel = current;
        if (ui::Select("##mididev", &sel, items.data(), static_cast<int>(items.size()), 240.0f * s) && sel != current &&
            sel <= static_cast<int>(inputs.size())) {
            w.midiInputDevice = sel == 0 ? std::wstring() : inputs[static_cast<size_t>(sel - 1)].name;
            doc.MarkDirty();
        }
        ImGui::SameLine(0.0f, 8.0f * s);
        if (ui::Button("Refresh")) inputs = EnumerateMidiInputs();
        ui::EndRow();
#else
        NoteText("On Linux, connect keyboards to SVMS with aconnect (ALSA sequencer).");
#endif
        ui::EndNest();
    }
    ui::EndCard();
}

void OverflowCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    ui::BeginCard("##overflow", "When events arrive too fast", nullptr, 0.0f, "overflow");
    static const char* modes[2] = {"Priority", "Lossless"};
    int mode = w.overflowMode == 0 ? 0 : 1;
    ui::BeginRow("overflow", "Overload behavior",
                 mode ? "Lossless never drops anything. The player is slowed down instead."
                      : "Priority may drop quiet notes when the queue is nearly full. Loud notes, controllers and note-offs are always kept.",
                 ui::SegmentedSize(modes, 2));
    if (ui::Segmented("##overflowmode", &mode, modes, 2)) {
        w.overflowMode = mode;
        doc.MarkDirty();
    }
    ui::EndRow();
    if (mode == 0) {
        ui::BeginNest();
        if (NumberRow("protVel", "Never drop notes at or above velocity", "Loud notes are always kept.",
                      &w.highPriorityVelocity, 1, 127, "##protvel"))
            doc.MarkDirty();
        if (NumberRow("shedPct", "Start dropping when the queue is this full (%)", "Below this, nothing is dropped.",
                      &w.shedStartPercent, 1, 99, "##shedpct"))
            doc.MarkDirty();
        ui::EndNest();
    }
    (void)s;
    ui::EndCard();
}

void VelocityPointerCard() {
    const float s = ui::Scale();
    ui::BeginCard("##velptr", nullptr, nullptr, 0.0f, "velIgnore");
    ui::BeginRow("velIgnore", "Ignore notes softer than",
                 "Note-ons below this velocity are not played. Set on the Sound page with the other velocity knobs.",
                 ui::ButtonSize("Go to Velocity"));
    if (ui::Button("Go to Velocity")) {
        if (ConfiguratorApp* app = GetLiveLinkContext().app) app->Navigate(NavPage::Sound);
        ui::FlashAnchor("velIgnore");
    }
    ui::EndRow();
    (void)s;
    ui::EndCard();
}

void QueueCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    if (!ui::Disclosure("midi", "Queue sizes", "Most people never need to change these")) return;
    ui::BeginReveal("midi");
    ui::BeginCard("##queues");
    if (NumberRow("queueCap", "Queue capacity (events)",
                  "How big a burst can wait in line. Bigger uses more memory; it does not add CPU.",
                  &w.eventRingCapacity, 4096, 2000000000u, "##queuecap"))
        doc.MarkDirty();
    if (NumberRow("perCb", "Max events per audio block", "The rest stay in order and play late.",
                  &w.maxEventsPerBlock, 1, 2000000000u, "##percb"))
        doc.MarkDirty();
    ui::BeginRow("fastProd", "Fast player lane",
                 "The player app gets a private, cheaper queue. Order against events from other apps can differ by a few dozen events.",
                 ImVec2(46.0f * s, 28.0f * s));
    if (ui::Toggle("##fastprod", &w.fastProducer)) {
        doc.MarkDirty();
        SendLiveCommand(svms::RLCommandType::SetFastProducer, w.fastProducer ? 1u : 0u);
    }
    ui::EndRow();
    if (w.eventRingCapacity > 4000000u) {
        char warn[160];
        std::snprintf(warn, sizeof(warn),
                      "Large queue: up to ~%.1f GB of address space if it ever fills. Most Black MIDI plays fine at 2M.",
                      w.eventRingCapacity * 74.0 / 1.0e9);
        ImGui::Dummy(ImVec2(0.0f, 10.0f * s - ImGui::GetStyle().ItemSpacing.y));
        const ImVec4 amber = ImGui::ColorConvertU32ToFloat4(ui::Tok().acc);
        NoteText(warn, &amber);
    }
    ui::EndCard();
    ui::EndReveal();
}

} // namespace

void DrawMidiPage(ConfigDocument& doc) {
    KeyboardCard(doc);
    OverflowCard(doc);
    VelocityPointerCard();
    QueueCard(doc);
}

} // namespace svms::cfg
