#include "PageAudio.h"
#include "ConfigDocument.h"
#include "WasapiDevices.h"
#include "EasterEggs.h"
#include "Theme.h"
#include "Widgets.h"
#include "SVMSBuildInfo.h"
#include "../SVMSRuntimeLink.h"
#include "imgui.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>
#include <cerrno>
#include <cstdlib>

namespace svms::cfg {
namespace {

std::string WideToUtf8Str(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
                                  static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWideStr(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0);
    if (len <= 0) return {};
    std::wstring ws(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        ws.data(), len);
    return ws;
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

void SetPrimarySoundFont(ConfigValues& values, const std::wstring& path) {
    values.soundFontPath = path;
    if (path.empty()) {
        if (!values.soundFontPaths.empty())
            values.soundFontPaths.erase(values.soundFontPaths.begin());
    } else if (values.soundFontPaths.empty()) {
        values.soundFontPaths.push_back(path);
    } else {
        values.soundFontPaths.front() = path;
    }
    for (auto it = values.soundFontRoutes.begin();
         it != values.soundFontRoutes.end();) {
        if (it->soundFontIndex >= values.soundFontPaths.size())
            it = values.soundFontRoutes.erase(it);
        else
            ++it;
    }
}

bool BrowseSoundFont(std::wstring& path, std::wstring& lastDirectory,
                     const wchar_t* title) {
    wchar_t fileBuf[1024] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"SoundFont files (*.sf2;*.sfz)\0*.sf2;*.sfz\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = static_cast<DWORD>(_countof(fileBuf));
    ofn.lpstrInitialDir = lastDirectory.empty() ? nullptr : lastDirectory.c_str();
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    path = fileBuf;
    lastDirectory = std::filesystem::path(path).parent_path().wstring();
    return true;
}

void SwapSoundFonts(ConfigValues& values, uint32_t a, uint32_t b) {
    if (a >= values.soundFontPaths.size() || b >= values.soundFontPaths.size())
        return;
    std::swap(values.soundFontPaths[a], values.soundFontPaths[b]);
    for (SoundFontRouteValue& route : values.soundFontRoutes) {
        if (route.soundFontIndex == a) route.soundFontIndex = b;
        else if (route.soundFontIndex == b) route.soundFontIndex = a;
    }
    values.soundFontPath = values.soundFontPaths.empty()
        ? std::wstring() : values.soundFontPaths.front();
}

void RemoveSoundFont(ConfigValues& values, uint32_t index) {
    if (index >= values.soundFontPaths.size()) return;
    values.soundFontPaths.erase(values.soundFontPaths.begin() + index);
    for (auto it = values.soundFontRoutes.begin();
         it != values.soundFontRoutes.end();) {
        if (it->soundFontIndex == index) {
            it = values.soundFontRoutes.erase(it);
        } else {
            if (it->soundFontIndex > index) --it->soundFontIndex;
            ++it;
        }
    }
    values.soundFontPath = values.soundFontPaths.empty()
        ? std::wstring() : values.soundFontPaths.front();
}

struct SoundFontLiveStatus {
    uint32_t state = 0u;
    uint64_t requested = 0u;
    uint64_t activated = 0u;
    float pollTimer = 0.25f;
    std::string message = "Ready";
    bool error = false;
};

bool ParseU64Field(char*& cursor, uint64_t& value) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor || *end != '\t') return false;
    value = static_cast<uint64_t>(parsed);
    cursor = end + 1;
    return true;
}

void PollSoundFontStatus(svms::RuntimeLinkClient& client,
                         SoundFontLiveStatus& status, bool force = false) {
    if (!force) {
        status.pollTimer += ImGui::GetIO().DeltaTime;
        if (status.pollTimer < 0.25f) return;
    }
    status.pollTimer = 0.0f;
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    if (client.SendCommand(svms::RLCommandType::QuerySoundFontLoad,
                           0u, 0u, svms::RuntimeLiveStateV2{}, 100u,
                           result) != svms::RLResult::Ok) return;
    char* cursor = result;
    uint64_t state = 0u;
    if (!ParseU64Field(cursor, state) ||
        !ParseU64Field(cursor, status.requested)) return;
    char* end = nullptr;
    errno = 0;
    status.activated = std::strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor) return;
    cursor = *end == '\t' ? end + 1 : end;
    status.state = static_cast<uint32_t>(state);
    status.error = status.state == 4u;
    if (status.error) {
        status.message = *cursor ? cursor : "SoundFont load failed";
    } else if (status.state == 1u) {
        status.message = "Loading and preparing SoundFont off-thread...";
    } else if (status.state == 2u) {
        status.message = "Prepared; waiting for the next audio block...";
    } else if (status.state == 3u) {
        status.message = "SoundFont activated";
    } else {
        status.message = "Ready";
    }
}

void StartSoundFontLoad(svms::RuntimeLinkClient& client,
                        const std::wstring& path,
                        SoundFontLiveStatus& status) {
    const std::string utf8 = WideToUtf8Str(path);
    if (utf8.empty() || utf8.size() >= svms::kRuntimeLinkCommandTextCapacity) {
        status.message = "The SoundFont path is too long for RuntimeLink.";
        status.error = true;
        return;
    }
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    const svms::RLResult code = client.SendCommand(
        svms::RLCommandType::ReloadSoundFont, 0u, 0u,
        svms::RuntimeLiveStateV2{}, 500u, result, utf8.c_str());
    status.error = code != svms::RLResult::Ok;
    status.message = result[0] ? result : svms::RLV2_ResultToString(code);
    if (!status.error) {
        status.state = 1u;
        status.pollTimer = 0.25f;
    }
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

// ASIO_MARKER_DEVICE_COMBO

} // namespace

void DrawAudioPage(ConfigDocument& doc, const EasterEggState& easterEggs,
                   AudioView view) {
    auto& w = doc.Working();
    const LiveLinkContext& live = GetLiveLinkContext();
    const bool liveSoundFont = live.connected && live.client &&
        live.client->HasCapability(svms::build::CapabilitySoundFontReload);
    static SoundFontLiveStatus soundFontStatus;
    if (liveSoundFont)
        PollSoundFontStatus(*live.client, soundFontStatus);

    static WasapiDeviceList deviceList;
    static bool devicesEnumerated = false;
    if (!devicesEnumerated) {
        deviceList.Enumerate();
        devicesEnumerated = true;
    }

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

            static const char* phaseKeys[] = {"OFF", "ANALYTIC", "SWEEP", "DIFFUSE", "RANDOM"};
            int phase = static_cast<int>(w.phaseRotationMode);
            if (phase < 0 || phase > 4) phase = 0;
            if (PanelKeys("PHASE ROTATION  (LIVE)", &phase, phaseKeys, 5,
                          "Rotates each voice by an independent random constant phase (Hilbert/quadrature form) at note-on, so the coherent black-MIDI hum no longer sums across voices. Per-frequency magnitude, loudness and sample-exact timing are untouched; OFF is the bit-exact baseline renderer. Roughly 2x more expensive than OFF.")) {
                w.phaseRotationMode = static_cast<uint32_t>(phase);
                doc.MarkDirty();
                if (live.connected && live.client) {
                    char phaseResult[svms::kRuntimeLinkResultTextCapacity]{};
                    live.client->SendCommand(
                        svms::RLCommandType::SetPhaseRotation, 0u,
                        static_cast<uint32_t>(phase),
                        svms::RuntimeLiveStateV2{}, 100u, phaseResult);
                }
            }
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

    if (view != AudioView::Output) {
    ImGui::Spacing();
    static std::wstring lastSoundFontDir;
    static std::wstring browserDir;
    static std::wstring scannedDir;
    static char searchBuf[128] = {};
    static std::vector<std::wstring> folderDirs;
    static std::vector<std::wstring> folderFonts;
    const ThemeSettings& th = GetThemeSettings();

    if (lastSoundFontDir.empty() && !w.soundFontPath.empty()) {
        lastSoundFontDir = std::filesystem::path(w.soundFontPath).parent_path().wstring();
    }
    if (browserDir.empty())
        browserDir = lastSoundFontDir.empty() ? L"." : lastSoundFontDir;

    {
        const std::wstring scanDir = browserDir.empty() ? L"." : browserDir;
        if (scannedDir != scanDir) {
            scannedDir = scanDir;
            folderDirs.clear();
            folderFonts.clear();
            std::error_code ec;
            for (std::filesystem::directory_iterator it(
                     scanDir,
                     std::filesystem::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (ec) break;
                std::error_code typeEc;
                if (it->is_directory(typeEc) && !typeEc) {
                    folderDirs.push_back(it->path().filename().wstring());
                    continue;
                }
                if (typeEc) continue;
                const std::wstring ext = it->path().extension().wstring();
                if (_wcsicmp(ext.c_str(), L".sf2") == 0 || _wcsicmp(ext.c_str(), L".sfz") == 0)
                    folderFonts.push_back(it->path().filename().wstring());
            }
            std::sort(folderDirs.begin(), folderDirs.end());
            std::sort(folderFonts.begin(), folderFonts.end());
        }
    }

    if (ImGui::BeginTable("##sf_top", 2,
                          ImGuiTableFlags_SizingStretchSame |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();

        // ------------------------------------------------ current font
        ImGui::TableNextColumn();
        if (BeginRackPanel("SOUNDFONT")) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = 78.0f;
            DrawLcdFrame(dl, p0, ImVec2(p0.x + width, p0.y + height));
            std::string name = "(none - engine uses the local fallback)";
            std::string dir;
            if (!w.soundFontPath.empty()) {
                const std::filesystem::path sf(w.soundFontPath);
                name = WideToUtf8Str(sf.filename().wstring());
                dir = WideToUtf8Str(sf.parent_path().wstring());
            }
            dl->PushClipRect(p0, ImVec2(p0.x + width - 8.0f, p0.y + height), true);
            PushMono(0.80f);
            dl->AddText(ImVec2(p0.x + 10.0f, p0.y + 6.0f),
                        ImGui::GetColorU32(ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.5f)),
                        "CONFIGURED");
            dl->AddText(ImVec2(p0.x + 10.0f, p0.y + height - ImGui::GetTextLineHeight() - 6.0f),
                        ImGui::GetColorU32(ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.5f)),
                        dir.c_str());
            PopMono();
            PushMono(1.25f);
            dl->AddText(ImVec2(p0.x + 10.0f, p0.y + 22.0f), ImGui::GetColorU32(th.accent), name.c_str());
            PopMono();
            dl->PopClipRect();
            ImGui::Dummy(ImVec2(width, height));
            ImGui::Spacing();

            if (KeyButton("Browse", ImVec2(88.0f, 28.0f))) {
                std::wstring selected;
                if (BrowseSoundFont(selected, lastSoundFontDir, L"Select primary SoundFont")) {
                    SetPrimarySoundFont(w, selected);
                    browserDir = lastSoundFontDir.empty() ? L"." : lastSoundFontDir;
                    scannedDir.clear();
                    searchBuf[0] = '\0';
                    doc.MarkDirty();
                }
            }
            ImGui::SameLine();
            if (KeyButton("Clear", ImVec2(72.0f, 28.0f), false, !w.soundFontPath.empty())) {
                SetPrimarySoundFont(w, {});
                doc.MarkDirty();
            }
            ImGui::SameLine();
            const bool busy = soundFontStatus.state == 1u || soundFontStatus.state == 2u;
            const bool canLoad = liveSoundFont && !w.soundFontPath.empty() && !busy;
            if (KeyButton("Load now", ImVec2(100.0f, 28.0f), canLoad, canLoad))
                StartSoundFontLoad(*live.client, w.soundFontPath, soundFontStatus);

            ImGui::Spacing();
            if (liveSoundFont) {
                const ImVec4 color = soundFontStatus.error
                    ? th.error : (busy ? th.warning : th.success);
                PushMono();
                ImGui::TextColored(color, "%s", soundFontStatus.message.c_str());
                PopMono();
            } else {
                ImGui::TextDisabled("Connect to a running V3 driver to switch live.");
                ImGui::SameLine();
                RestartPill();
            }
            if (live.telemetry && live.telemetry->soundFontName[0] != '\0')
                ImGui::TextDisabled("Active: %s", live.telemetry->soundFontName);
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted("Loading silences active voices; MIDI state is kept. Save Configuration to keep the choice after a restart.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        EndRackPanel();

        // ------------------------------------------------ folder browser
        ImGui::TableNextColumn();
        if (BeginRackPanel("FOLDER")) {
            std::string folderLabel = WideToUtf8Str(
                std::filesystem::path(scannedDir.empty() ? L"." : scannedDir)
                    .lexically_normal().wstring());
            if (folderLabel.size() > 56)
                folderLabel = "..." + folderLabel.substr(folderLabel.size() - 56);
            PushMono();
            ImGui::PushStyleColor(ImGuiCol_Text, th.accent);
            ImGui::TextUnformatted(folderLabel.c_str());
            ImGui::PopStyleColor();
            PopMono();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##sfsearch", "Filter SoundFonts in folder...",
                                     searchBuf, sizeof(searchBuf));
            ImGui::BeginChild("sfList", ImVec2(0.0f, 168.0f), ImGuiChildFlags_Borders);
            std::string filter = searchBuf;
            std::transform(filter.begin(), filter.end(), filter.begin(),
                           [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
            const std::filesystem::path currentDir =
                std::filesystem::path(scannedDir.empty() ? L"." : scannedDir).lexically_normal();
            const std::filesystem::path parentDir = currentDir.parent_path();
            bool navigated = false;
            if (!parentDir.empty() && parentDir != currentDir) {
                if (ImGui::Selectable("[..]")) {
                    browserDir = parentDir.wstring();
                    scannedDir.clear();
                    searchBuf[0] = '\0';
                    navigated = true;
                }
            }
            if (!navigated) {
                for (const auto& folder : folderDirs) {
                    const std::string dname = "[DIR] " + WideToUtf8Str(folder);
                    if (ImGui::Selectable(dname.c_str())) {
                        browserDir = (currentDir / folder).lexically_normal().wstring();
                        scannedDir.clear();
                        searchBuf[0] = '\0';
                        navigated = true;
                        break;
                    }
                }
            }
            if (!navigated) {
                for (const auto& file : folderFonts) {
                    std::string fname = WideToUtf8Str(file);
                    std::string lower = fname;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                                   [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
                    if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
                    const std::filesystem::path candidate = (currentDir / file).lexically_normal();
                    const bool selected = !w.soundFontPath.empty() &&
                        _wcsicmp(candidate.c_str(),
                                 std::filesystem::path(w.soundFontPath).lexically_normal().c_str()) == 0;
                    if (ImGui::Selectable(fname.c_str(), selected)) {
                        SetPrimarySoundFont(w, candidate.wstring());
                        lastSoundFontDir = currentDir.wstring();
                        doc.MarkDirty();
                    }
                }
            }
            ImGui::EndChild();
        }
        EndRackPanel();
        ImGui::EndTable();
    }

    // ------------------------------------------------------ bank stack
    if (BeginRackPanel("BANK STACK")) {
        if (KeyButton("Add SoundFont", ImVec2(128.0f, 28.0f))) {
            std::wstring selected;
            if (w.soundFontPaths.size() < 16u &&
                BrowseSoundFont(selected, lastSoundFontDir, L"Add SoundFont to stack")) {
                bool duplicate = false;
                for (const std::wstring& existing : w.soundFontPaths)
                    duplicate |= _wcsicmp(existing.c_str(), selected.c_str()) == 0;
                if (!duplicate) {
                    w.soundFontPaths.push_back(selected);
                    if (w.soundFontPath.empty()) w.soundFontPath = w.soundFontPaths.front();
                    doc.MarkDirty();
                }
                browserDir = lastSoundFontDir.empty() ? L"." : lastSoundFontDir;
                scannedDir.clear();
                searchBuf[0] = '\0';
            }
        }
        ImGui::SameLine();
        RestartPill();
        ImGui::SameLine();
        ImGui::TextDisabled("Up to 16 banks. The first matching bank wins unless a route below matches first.");
        ImGui::Spacing();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (uint32_t i = 0u; i < w.soundFontPaths.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            const float rowX = ImGui::GetCursorPosX();
            dl->AddRectFilled(rowStart, ImVec2(rowStart.x + rowW, rowStart.y + 32.0f),
                              ImGui::GetColorU32(GetLcdBg()), th.cornerRadius);
            DrawLed(dl, ImVec2(rowStart.x + 14.0f, rowStart.y + 16.0f), 3.5f, i == 0u);
            std::string label = std::to_string(i) + "  " +
                WideToUtf8Str(std::filesystem::path(w.soundFontPaths[i]).filename().wstring());
            if (i == 0u) label += "   (PRIMARY)";
            PushMono();
            dl->AddText(ImVec2(rowStart.x + 30.0f, rowStart.y + (32.0f - ImGui::GetTextLineHeight()) * 0.5f),
                        ImGui::GetColorU32(th.accent), label.c_str());
            PopMono();
            bool changed = false;
            ImGui::SetCursorPos(ImVec2(rowX + rowW - 196.0f, ImGui::GetCursorPosY() + 3.0f));
            if (KeyButton("Up", ImVec2(52.0f, 26.0f), false, i > 0u)) {
                SwapSoundFonts(w, i, i - 1u);
                doc.MarkDirty();
                changed = true;
            }
            ImGui::SameLine(0.0f, 6.0f);
            if (!changed && KeyButton("Down", ImVec2(60.0f, 26.0f), false, i + 1u < w.soundFontPaths.size())) {
                SwapSoundFonts(w, i, i + 1u);
                doc.MarkDirty();
                changed = true;
            }
            ImGui::SameLine(0.0f, 6.0f);
            if (!changed && KeyButton("Remove", ImVec2(70.0f, 26.0f))) {
                RemoveSoundFont(w, i);
                doc.MarkDirty();
                changed = true;
            }
            ImGui::SetCursorPos(ImVec2(rowX, ImGui::GetCursorPosY() + 6.0f));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
            if (changed) break;
        }
    }
    EndRackPanel();

    // --------------------------------------------------- routes
    if (BeginRackPanel("BANK / PRESET ROUTES")) {
        if (KeyButton("Add route", ImVec2(110.0f, 28.0f)) &&
            !w.soundFontPaths.empty() && w.soundFontRoutes.size() < 256u) {
            SoundFontRouteValue route{};
            route.soundFontIndex = static_cast<uint32_t>(w.soundFontPaths.size() > 1u ? 1u : 0u);
            w.soundFontRoutes.push_back(route);
            doc.MarkDirty();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Preset -1 keeps the incoming program.");

        for (uint32_t i = 0u; i < w.soundFontRoutes.size(); ++i) {
            SoundFontRouteValue& route = w.soundFontRoutes[i];
            ImGui::PushID(static_cast<int>(0x4000u + i));
            ImGui::Separator();
            const char* preview = "(missing)";
            std::string previewStorage;
            if (route.soundFontIndex < w.soundFontPaths.size()) {
                previewStorage = WideToUtf8Str(std::filesystem::path(
                    w.soundFontPaths[route.soundFontIndex]).filename().wstring());
                preview = previewStorage.c_str();
            }
            ImGui::SetNextItemWidth((std::min)(260.0f, ImGui::GetContentRegionAvail().x));
            if (ImGui::BeginCombo("SoundFont", preview)) {
                for (uint32_t sf = 0u; sf < w.soundFontPaths.size(); ++sf) {
                    const std::string name = WideToUtf8Str(std::filesystem::path(
                        w.soundFontPaths[sf]).filename().wstring());
                    if (ImGui::Selectable(name.c_str(), route.soundFontIndex == sf)) {
                        route.soundFontIndex = sf;
                        doc.MarkDirty();
                    }
                }
                ImGui::EndCombo();
            }
            int targetBank = static_cast<int>(route.targetBank);
            int targetPreset = route.targetPreset;
            int sourceBank = static_cast<int>(route.sourceBank);
            int sourcePreset = route.sourcePreset;
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::InputInt("MIDI bank", &targetBank)) {
                route.targetBank = static_cast<uint32_t>((std::clamp)(targetBank, 0, 127));
                doc.MarkDirty();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::InputInt("MIDI preset", &targetPreset)) {
                route.targetPreset = (std::clamp)(targetPreset, -1, 127);
                doc.MarkDirty();
            }
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::InputInt("Source bank", &sourceBank)) {
                route.sourceBank = static_cast<uint32_t>((std::clamp)(sourceBank, 0, 65535));
                doc.MarkDirty();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::InputInt("Source preset", &sourcePreset)) {
                route.sourcePreset = (std::clamp)(sourcePreset, -1, 127);
                doc.MarkDirty();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Drums", &route.percussion)) doc.MarkDirty();
            ImGui::SameLine();
            const bool removeRoute = KeyButton("Remove", ImVec2(76.0f, 26.0f));
            if (removeRoute) {
                w.soundFontRoutes.erase(w.soundFontRoutes.begin() + i);
                doc.MarkDirty();
            }
            ImGui::PopID();
            if (removeRoute) break;
        }
    }
    EndRackPanel();
    }  // soundfont view
}

} // namespace svms::cfg