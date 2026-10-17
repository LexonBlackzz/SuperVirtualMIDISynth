#include "PageAudio.h"
#include "PlatformUtil.h"
#include "ConfigDocument.h"
#ifdef _WIN32
#include "WasapiDevices.h"
#endif
#include "EasterEggs.h"
#include "Theme.h"
#include "Widgets.h"
#include "SVMSBuildInfo.h"
#include "DriverLink.h"
#include "imgui.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include <cerrno>
#include <cstdlib>

namespace svms::cfg {
namespace {

std::string WideToUtf8Str(const std::wstring& ws) {
    return svms::cfg::util::WideToUtf8(ws);
}

std::wstring Utf8ToWideStr(const std::string& s) {
    return svms::cfg::util::Utf8ToWide(s);
}

bool EqualAsciiCI(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool BeginAudioSettingsTable(const char* id) {
    if (!ImGui::BeginTable(id, 3,
                           ImGuiTableFlags_SizingStretchProp |
                           ImGuiTableFlags_BordersInnerH,
                           ImVec2(0.0f, 0.0f))) {
        return false;
    }
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 250.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 135.0f);
    return true;
}

void AudioLabelCell(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextColumn();
    SettingLabel(label, tooltip);
}

void RestartCell() {
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    PushMono();
    const float pillW = ImGui::CalcTextSize("RESTART").x + 12.0f;
    PopMono();
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - pillW) * 0.5f));
    RestartPill();
}

} // namespace

namespace {

#ifdef _WIN32
// Installed ASIO drivers register under HKLM\SOFTWARE\ASIO (and the WOW6432
// view); each subkey's name is the driver's user-visible name. This matches
// how the SDK's host `getDriverNames()` discovers drivers, without linking
// the SDK into the configurator.
std::vector<std::string> EnumerateAsioDrivers() {
    std::vector<std::string> names;
    static const wchar_t* kRoots[] = {
        L"SOFTWARE\\ASIO", L"SOFTWARE\\WOW6432Node\\ASIO",
    };
    for (const wchar_t* root : kRoots) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &key)
                != ERROR_SUCCESS)
            continue;
        DWORD index = 0;
        wchar_t name[256];
        DWORD nameLen = 256;
        while (RegEnumKeyExW(key, index++, name, &nameLen, nullptr,
                             nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            // A valid ASIO registration carries the CLSID of its driver DLL.
            DWORD type = 0;
            wchar_t clsid[128] = {};
            DWORD cb = sizeof(clsid);
            if (RegGetValueW(key, name, L"CLSID", RRF_RT_REG_SZ,
                             &type, clsid, &cb) == ERROR_SUCCESS && clsid[0]) {
                std::string n = WideToUtf8Str(name);
                if (!n.empty() &&
                    std::find(names.begin(), names.end(), n) == names.end())
                    names.push_back(std::move(n));
            }
            nameLen = 256;
        }
        RegCloseKey(key);
    }
    return names;
}
#endif

// ASIO_MARKER_DEVICE_COMBO

#ifndef _WIN32
// Linux output view: the driver renders to one ALSA PCM. Backend/endpoint
// pickers (WASAPI, ASIO) do not exist here.
void DrawLinuxOutputView(ConfigDocument& doc, ConfigValues& w) {
    if (BeginRackPanel("OUTPUT (ALSA)")) {
        PanelCaption("ALSA PCM DEVICE",
                     "PCM the driver opens: 'default' follows PipeWire/PulseAudio, or name one (hw:0,0, plughw:1,0, ...). Also settable per run with SVMS_AUDIO_DEVICE.",
                     true);
        char device[128]{};
        WideToUtf8Str(w.linuxDevice).copy(device, sizeof(device) - 1u);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##alsadevice", device, sizeof(device))) {
            w.linuxDevice = Utf8ToWideStr(device);
            doc.MarkDirty();
        }
        ImGui::Spacing();
        ImGui::Spacing();

        static const uint32_t sampleRateValues[] = {44100, 48000, 88200, 96000, 176400, 192000};
        static const char* sampleRateKeys[] = {"44.1k", "48k", "88.2k", "96k", "176k", "192k"};
        int srIdx = -1;
        for (int i = 0; i < 6; ++i)
            if (sampleRateValues[i] == w.sampleRate) srIdx = i;
        if (PanelKeys("SAMPLE RATE", &srIdx, sampleRateKeys, 6,
                      "Sample rate for audio output. Higher rates increase the amount of work per second.",
                      true) && srIdx >= 0) {
            w.sampleRate = sampleRateValues[srIdx];
            doc.MarkDirty();
        }
        if (srIdx < 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("custom: %u Hz", w.sampleRate);
        }
        ImGui::Spacing();
        ImGui::Spacing();

        static const uint32_t bufferValues[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192};
        static const char* bufferKeys[] = {"64", "128", "256", "512", "1k", "2k", "4k", "8k"};
        int bufIdx = -1;
        for (int i = 0; i < 8; ++i)
            if (bufferValues[i] == w.bufferFrames) bufIdx = i;
        if (PanelKeys("BUFFER FRAMES", &bufIdx, bufferKeys, 8,
                      "Frames rendered per audio block. Smaller buffers reduce latency but leave less time for each render.",
                      true) && bufIdx >= 0) {
            w.bufferFrames = bufferValues[bufIdx];
            doc.MarkDirty();
        }
        if (bufIdx < 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("custom: %u", w.bufferFrames);
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Latency about %.1f ms",
                            1000.0f * static_cast<float>(w.bufferFrames) /
                                static_cast<float>((std::max)(1u, w.sampleRate)));
    }
    EndRackPanel();
}
#endif

} // namespace

void DrawAudioPage(ConfigDocument& doc, const EasterEggState& easterEggs,
                   AudioView view) {
    auto& w = doc.Working();
    const LiveLinkContext& live = GetLiveLinkContext();
#ifdef _WIN32
    static WasapiDeviceList deviceList;
    static bool devicesEnumerated = false;
    if (!devicesEnumerated) {
        deviceList.Enumerate();
        devicesEnumerated = true;
    }
#endif

#ifndef _WIN32
    if (view != AudioView::SoundFont) DrawLinuxOutputView(doc, w);
#else
    if (view != AudioView::SoundFont) {

    const bool asioActive =
        EqualAsciiCI(WideToUtf8Str(w.audioBackend), "asio");

    // ASIO driver list: re-enumerate when the backend flips to ASIO or the
    // user hits Refresh devices.
    static std::vector<std::string> asioDrivers;
    static bool asioEnumerated = false;
    if (asioActive && !asioEnumerated) {
        asioDrivers = EnumerateAsioDrivers();
        asioEnumerated = true;
    }
    if (!asioActive) asioEnumerated = false;

    int currentDevice = -1;
    const std::string configuredUtf8 = WideToUtf8Str(w.audioDevice);
    std::vector<std::string> names;
    if (asioActive) {
        names.push_back("Default (first installed ASIO driver)");
        names.insert(names.end(), asioDrivers.begin(), asioDrivers.end());
        if (w.audioDevice.empty() || w.audioDevice == L"default") {
            currentDevice = 0;
        } else {
            for (size_t i = 0; i < asioDrivers.size(); ++i) {
                if (asioDrivers[i] == configuredUtf8) {
                    currentDevice = static_cast<int>(i + 1);
                    break;
                }
            }
        }
    } else {
        names = deviceList.FriendlyNames();
        if (w.audioDevice.empty() || w.audioDevice == L"default") {
            currentDevice = 0;
        } else {
            for (size_t i = 0; i < deviceList.Devices().size(); ++i) {
                const auto& dev = deviceList.Devices()[i];
                const std::string devName = WideToUtf8Str(dev.friendlyName);
                const std::string devId = WideToUtf8Str(dev.id);
                if (EqualAsciiCI(devName, configuredUtf8) || devId == configuredUtf8) {
                    currentDevice = static_cast<int>(i + 1);
                    break;
                }
            }
        }
    }

    if (ImGui::BeginTable("##out_top", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("output", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("synth", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        // ---------------------------------------------------- OUTPUT
        ImGui::TableNextColumn();
        if (BeginRackPanel("OUTPUT")) {
            PanelCaption("DEVICE",
                         asioActive
                             ? "ASIO driver to open. 'Default' uses the first installed ASIO driver. The driver's own control panel sets its buffer size and channels."
                             : "WASAPI output endpoint. 'Default Windows Output Device' follows the current Windows default endpoint.",
                         false);
            std::string devicePreview;
            if (easterEggs.megaFuckerDac && !asioActive) {
                devicePreview = "MegaFucker DAC Pro 9000";
            } else if (currentDevice >= 0 && currentDevice < static_cast<int>(names.size())) {
                devicePreview = names[static_cast<size_t>(currentDevice)];
            } else if (asioActive) {
                devicePreview = "Missing ASIO driver: " + configuredUtf8;
            } else {
                devicePreview = "Missing: " + configuredUtf8;
            }
            ImGui::SetNextItemWidth((std::max)(180.0f, ImGui::GetContentRegionAvail().x - 110.0f));
            PushMono();
            ImGui::PushStyleColor(ImGuiCol_Text, GetAccent());
            const bool comboOpen = ImGui::BeginCombo("##device", devicePreview.c_str());
            ImGui::PopStyleColor();
            PopMono();
            if (comboOpen) {
                for (int i = 0; i < static_cast<int>(names.size()); ++i) {
                    const bool selected = i == currentDevice;
                    if (ImGui::Selectable(names[static_cast<size_t>(i)].c_str(), selected)) {
                        currentDevice = i;
                        if (i == 0) {
                            w.audioDevice = L"default";
                        } else if (asioActive) {
                            w.audioDevice = Utf8ToWideStr(asioDrivers[static_cast<size_t>(i - 1)]);
                        } else {
                            w.audioDevice = deviceList.Devices()[static_cast<size_t>(i - 1)].friendlyName;
                        }
                        doc.MarkDirty();
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                if (asioActive && asioDrivers.empty()) {
                    ImGui::TextDisabled("No installed ASIO drivers found (HKLM\\SOFTWARE\\ASIO)");
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (KeyButton("Refresh", ImVec2(92.0f, 26.0f))) {
                deviceList.Enumerate();
                asioDrivers = EnumerateAsioDrivers();
                asioEnumerated = true;
            }
            ImGui::Spacing();
            ImGui::Spacing();

            static const char* backendKeys[] = {"WASAPI", "ASIO"};
            int backendIdx = asioActive ? 1 : 0;
            if (PanelKeys("BACKEND", &backendIdx, backendKeys, 2,
                          "WASAPI Shared Output is the production backend on modern builds. ASIO bypasses the Windows audio mixer for lower latency; it needs an installed ASIO driver and falls back to WASAPI if none can be opened.",
                          true)) {
                w.audioBackend = (backendIdx == 1) ? L"asio" : L"wasapi-shared";
                doc.MarkDirty();
            }
            ImGui::Spacing();
            ImGui::Spacing();

            static const uint32_t sampleRateValues[] = {44100, 48000, 88200, 96000, 176400, 192000};
            static const char* sampleRateKeys[] = {"44.1k", "48k", "88.2k", "96k", "176k", "192k"};
            int srIdx = -1;
            for (int i = 0; i < 6; ++i)
                if (sampleRateValues[i] == w.sampleRate) srIdx = i;
            if (PanelKeys("SAMPLE RATE", &srIdx, sampleRateKeys, 6,
                          "Sample rate for audio output. Higher rates increase the amount of work per second.",
                          true) && srIdx >= 0) {
                w.sampleRate = sampleRateValues[srIdx];
                doc.MarkDirty();
            }
            if (srIdx < 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("custom: %u Hz", w.sampleRate);
            }
            ImGui::Spacing();
            ImGui::Spacing();

            static const uint32_t bufferValues[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192};
            static const char* bufferKeys[] = {"64", "128", "256", "512", "1k", "2k", "4k", "8k"};
            int bufIdx = -1;
            for (int i = 0; i < 8; ++i)
                if (bufferValues[i] == w.bufferFrames) bufIdx = i;
            if (PanelKeys("BUFFER FRAMES", &bufIdx, bufferKeys, 8,
                          "Audio endpoint buffer size. Smaller buffers reduce latency but leave less time for each render callback.",
                          true) && bufIdx >= 0) {
                w.bufferFrames = bufferValues[bufIdx];
                doc.MarkDirty();
            }
            if (bufIdx < 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("custom: %u", w.bufferFrames);
            }
        }
        EndRackPanel();

        // ------------------------------------------- LATENCY / SYNTH
        ImGui::TableNextColumn();
        if (BeginRackPanel("LATENCY / SYNTH")) {
            const ThemeSettings& th = GetThemeSettings();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float latencyMs = (static_cast<float>(w.bufferFrames) /
                                     static_cast<float>((std::max)(1u, w.sampleRate))) * 1000.0f;
            {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                const float height = 74.0f;
                DrawLcdFrame(dl, p0, ImVec2(p0.x + width, p0.y + height));
                PushMono(0.80f);
                dl->AddText(ImVec2(p0.x + 10.0f, p0.y + 6.0f),
                            ImGui::GetColorU32(ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.5f)),
                            "OUTPUT LATENCY");
                char sub[64];
                std::snprintf(sub, sizeof(sub), "%u frames @ %u Hz", w.bufferFrames, w.sampleRate);
                const ImVec2 ss = ImGui::CalcTextSize(sub);
                dl->AddText(ImVec2(p0.x + width - ss.x - 10.0f, p0.y + height - ss.y - 6.0f),
                            ImGui::GetColorU32(ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.5f)),
                            sub);
                PopMono();
                PushMono(2.1f);
                char big[32];
                std::snprintf(big, sizeof(big), "%.1f ms", latencyMs);
                dl->AddText(ImVec2(p0.x + 10.0f, p0.y + 22.0f), ImGui::GetColorU32(th.accent), big);
                PopMono();
                ImGui::Dummy(ImVec2(width, height));
            }
            ImGui::Spacing();
            ImGui::Spacing();

            static const char* synthKeys[] = {"SVMS", "API DLL", "KDMAPI", "WINMM", "AUTO"};
            int synthBackend = static_cast<int>(w.apiBackend);
            if (synthBackend < 0 || synthBackend > 4) synthBackend = 0;
            if (PanelKeys("SYNTH BACKEND", &synthBackend, synthKeys, 5,
                          "Where MIDI events are sent: the built-in SVMS engine (default), an external synth DLL speaking the SVMS-API backend interface, a KDMAPI-compatible synth DLL (OmniMIDI and friends), a WinMM MIDI-out device such as the Microsoft GS Wavetable Synth, or auto-detect. External backends own their audio output.",
                          true)) {
                w.apiBackend = static_cast<uint32_t>(synthBackend);
                doc.MarkDirty();
            }
            if (w.apiBackend == 1u || w.apiBackend == 2u || w.apiBackend == 4u) {
                char dllBuf[512]{};
                WideToUtf8Str(w.apiBackendDll).copy(dllBuf, sizeof(dllBuf) - 1u);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::InputText("##backenddll", dllBuf, sizeof(dllBuf))) {
                    w.apiBackendDll = Utf8ToWideStr(dllBuf);
                    doc.MarkDirty();
                }
                ImGui::TextDisabled(w.apiBackend == 2u ? "KDMAPI DLL path" : "synth DLL path");
            } else if (w.apiBackend == 3u) {
                int device = static_cast<int>(w.apiWinMmDevice);
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::InputInt("##winmmdevice", &device)) {
                    device = (std::max)(0, (std::min)(255, device));
                    w.apiWinMmDevice = static_cast<uint32_t>(device);
                    doc.MarkDirty();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("MIDI-out device index");
            }
            if (w.apiBackend != 0u) {
                if (PanelLever("ALLOW NESTED", &w.apiAllowNested,
                               "Synth-ception. By default an SVMS instance started inside another synth host (such as OmniMIDIv2 loading the SVMS plugin) ignores this backend setting, because the inner instance shares this config and would route events back out in a loop. Enable only if the inner instance uses a separate config.",
                               true)) {
                    doc.MarkDirty();
                }
            }
            ImGui::Spacing();
            ImGui::Spacing();
        }
        EndRackPanel();
        ImGui::EndTable();
    }

    if (easterEggs.megaFuckerDac) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.20f, 1.0f));
        ImGui::TextUnformatted("Display override active — your actual audio device has not changed.");
        ImGui::PopStyleColor();
    }

    }  // output view
#endif

}

} // namespace svms::cfg