#include "PageAudio.h"
#include "ConfigDocument.h"
#include "WasapiDevices.h"
#include "EasterEggs.h"
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
    ofn.lpstrFilter = L"SoundFont files (*.sf2)\0*.sf2\0All files (*.*)\0*.*\0";
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
                           ImGuiTableFlags_BordersInnerH |
                           ImGuiTableFlags_RowBg,
                           ImVec2(0.0f, 0.0f))) {
        return false;
    }
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 135.0f);
    return true;
}

void AudioLabelCell(const char* label, const char* tooltip = nullptr) {
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

void FixedCell() {
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();

    constexpr const char* label = "FIXED";
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - labelWidth) * 0.5f));

    ImGui::TextDisabled("%s", label);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("The audio backend is currently fixed to WASAPI Shared Mode.");
        ImGui::EndTooltip();
    }
}

} // namespace

void DrawAudioPage(ConfigDocument& doc, const EasterEggState& easterEggs) {
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

    SectionHeader("AUDIO OUTPUT");

    auto names = deviceList.FriendlyNames();
    int currentDevice = -1;
    const std::string configuredUtf8 = WideToUtf8Str(w.audioDevice);

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

    if (BeginAudioSettingsTable("##audio_output_settings")) {
        ImGui::TableNextRow();
        AudioLabelCell("Output device",
                       "WASAPI output endpoint. 'Default Windows Output Device' follows the current Windows default endpoint.");
        ImGui::TableNextColumn();

        std::string devicePreview;
        if (easterEggs.megaFuckerDac) {
            devicePreview = "MegaFucker DAC Pro 9000";
        } else if (currentDevice >= 0 && currentDevice < static_cast<int>(names.size())) {
            devicePreview = names[static_cast<size_t>(currentDevice)];
        } else {
            devicePreview = "Missing: " + configuredUtf8;
        }

        const float deviceWidth = (std::min)(360.0f, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth((std::max)(180.0f, deviceWidth));
        if (ImGui::BeginCombo("##device", devicePreview.c_str())) {
            for (int i = 0; i < static_cast<int>(names.size()); ++i) {
                const bool selected = i == currentDevice;
                if (ImGui::Selectable(names[static_cast<size_t>(i)].c_str(), selected)) {
                    currentDevice = i;
                    if (i == 0) {
                        w.audioDevice = L"default";
                    } else {
                        w.audioDevice = deviceList.Devices()[static_cast<size_t>(i - 1)].friendlyName;
                    }
                    doc.MarkDirty();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Refresh devices")) {
            deviceList.Enumerate();
        }

        ImGui::TableNextRow();
        AudioLabelCell("Sample rate",
                       "Sample rate for audio output. Higher rates increase the amount of work per second.");
        ImGui::TableNextColumn();

        static const char* sampleRateItems[] = {
            "44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz",
            "176400 Hz", "192000 Hz"
        };
        static const uint32_t sampleRateValues[] = {
            44100, 48000, 88200, 96000, 176400, 192000
        };

        int srIdx = -1;
        for (int i = 0; i < 6; ++i) {
            if (sampleRateValues[i] == w.sampleRate) {
                srIdx = i;
                break;
            }
        }
        char srPreview[48];
        if (srIdx >= 0) {
            std::snprintf(srPreview, sizeof(srPreview), "%u Hz", w.sampleRate);
        } else {
            std::snprintf(srPreview, sizeof(srPreview), "%u Hz (custom)", w.sampleRate);
        }
        ImGui::SetNextItemWidth((std::min)(220.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##samplerate", srPreview)) {
            for (int i = 0; i < 6; ++i) {
                const bool selected = srIdx == i;
                if (ImGui::Selectable(sampleRateItems[i], selected)) {
                    w.sampleRate = sampleRateValues[i];
                    doc.MarkDirty();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        RestartCell();

        ImGui::TableNextRow();
        AudioLabelCell("Buffer frames",
                       "Audio endpoint buffer size. Smaller buffers reduce latency but leave less time for each render callback.");
        ImGui::TableNextColumn();

        static const char* bufferItems[] = {
            "64", "128", "256", "512", "1024", "2048", "4096", "8192"
        };
        static const uint32_t bufferValues[] = {
            64, 128, 256, 512, 1024, 2048, 4096, 8192
        };

        int bufIdx = -1;
        for (int i = 0; i < 8; ++i) {
            if (bufferValues[i] == w.bufferFrames) {
                bufIdx = i;
                break;
            }
        }
        char bufferPreview[48];
        if (bufIdx >= 0) {
            std::snprintf(bufferPreview, sizeof(bufferPreview), "%u", w.bufferFrames);
        } else {
            std::snprintf(bufferPreview, sizeof(bufferPreview), "%u (custom)", w.bufferFrames);
        }

        const float comboWidth = (std::min)(220.0f, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("##buffer", bufferPreview)) {
            for (int i = 0; i < 8; ++i) {
                const bool selected = bufIdx == i;
                if (ImGui::Selectable(bufferItems[i], selected)) {
                    w.bufferFrames = bufferValues[i];
                    doc.MarkDirty();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const float latencyMs = (static_cast<float>(w.bufferFrames) /
                                 static_cast<float>(w.sampleRate)) * 1000.0f;
        char latencyBuf[64];
        std::snprintf(latencyBuf, sizeof(latencyBuf), "%.2f ms @ %u Hz",
                      latencyMs, w.sampleRate);
        if (ImGui::GetContentRegionAvail().x > 150.0f) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", latencyBuf);
        } else {
            ImGui::TextDisabled("%s", latencyBuf);
        }
        RestartCell();

        ImGui::TableNextRow();
        AudioLabelCell("Audio backend",
                       "V3 uses Windows Audio Session API in shared mode. This is the production backend on modern builds.");
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("WASAPI Shared Mode");
        FixedCell();

        ImGui::TableNextRow();
        AudioLabelCell("Phase rotation",
                       "Rotates each voice by an independent random constant phase "
                       "(Hilbert/quadrature form) at note-on, so the coherent "
                       "black-MIDI hum no longer sums across voices. Per-frequency "
                       "magnitude, loudness and sample-exact timing are untouched; "
                       "Coherent (off) is the bit-exact baseline renderer. "
                       "2× more expensive than Coherent.");
        ImGui::TableNextColumn();

        static const char* phaseItems[] = {
            "Coherent (off)", "Analytic (Hilbert)", "Sweep", "Diffuse", "Random"
        };

        int idx = static_cast<int>(w.phaseRotationMode);
        if (idx < 0 || idx > 4) idx = 0;

        const float phaseComboWidth = (std::min)(220.0f, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(phaseComboWidth);
        if (ImGui::BeginCombo("##phaserotation", phaseItems[idx])) {
            for (int i = 0; i < 5; ++i) {
                const bool selected = idx == i;
                if (ImGui::Selectable(phaseItems[i], selected)) {
                    w.phaseRotationMode = static_cast<uint32_t>(i);
                    doc.MarkDirty();
                    if (live.connected && live.client) {
                        char phaseResult[svms::kRuntimeLinkResultTextCapacity]{};
                        live.client->SendCommand(
                            svms::RLCommandType::SetPhaseRotation, 0u,
                            static_cast<uint32_t>(i),
                            svms::RuntimeLiveStateV2{}, 100u, phaseResult);
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::TableNextColumn();
        if (live.connected) {
            ImGui::AlignTextToFramePadding();
            LiveBadge("Applied live via RuntimeLink");
        }

        ImGui::EndTable();
    }

    if (easterEggs.megaFuckerDac) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.20f, 1.0f));
        ImGui::TextUnformatted("Display override active — your actual audio device has not changed.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    SectionHeader("SOUND FONT");

    {
        ImGui::TextUnformatted("Configured SoundFont:");
        ImGui::SameLine();
        if (w.soundFontPath.empty()) {
            ImGui::TextDisabled("(none — engine uses the local fallback)");
        } else {
            std::string utf8 = WideToUtf8Str(w.soundFontPath);
            if (utf8.size() > 72) utf8 = "…" + utf8.substr(utf8.size() - 72);
            ImGui::TextDisabled("%s", utf8.c_str());
        }
        ImGui::Spacing();

        static std::wstring lastSoundFontDir;
        if (lastSoundFontDir.empty() && !w.soundFontPath.empty()) {
            lastSoundFontDir =
                std::filesystem::path(w.soundFontPath).parent_path().wstring();
        }

        if (ImGui::Button("Browse…", ImVec2(90, 0))) {
            std::wstring selected;
            if (BrowseSoundFont(selected, lastSoundFontDir,
                                L"Select primary SoundFont")) {
                SetPrimarySoundFont(w, selected);
                doc.MarkDirty();
            }
        }
        ImGui::SameLine();
        if (!w.soundFontPath.empty()) {
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                SetPrimarySoundFont(w, {});
                doc.MarkDirty();
            }
        }
        if (liveSoundFont) LiveBadge("Loads off-thread and activates at an audio-block boundary.");
        else RestartRequiredBadge();

        ImGui::Spacing();
        const bool busy = soundFontStatus.state == 1u ||
                          soundFontStatus.state == 2u;
        const bool canLoad = liveSoundFont && !w.soundFontPath.empty() && !busy;
        if (!canLoad) ImGui::BeginDisabled();
        if (ImGui::Button("Load Now", ImVec2(110.0f, 0.0f)) && canLoad)
            StartSoundFontLoad(*live.client, w.soundFontPath, soundFontStatus);
        if (!canLoad) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!liveSoundFont) {
            ImGui::TextDisabled("Connect to a compatible running V3 driver to switch live.");
        } else {
            const ImVec4 color = soundFontStatus.error
                ? ImVec4(0.95f, 0.35f, 0.30f, 1.0f)
                : (busy ? ImVec4(0.95f, 0.75f, 0.20f, 1.0f)
                        : ImVec4(0.35f, 0.90f, 0.55f, 1.0f));
            ImGui::TextColored(color, "%s", soundFontStatus.message.c_str());
        }
        if (live.telemetry && live.telemetry->soundFontName[0] != '\0') {
            ImGui::TextDisabled("Active: %s", live.telemetry->soundFontName);
        }
        ImGui::TextDisabled(
            "Active voices are silenced at activation; MIDI state is retained. Save Configuration to keep the selection after restart.");

        static char searchBuf[128] = {};
        static std::vector<std::wstring> folderFonts;
        static std::wstring scannedDir;
        {
            std::wstring scanDir = lastSoundFontDir.empty() ? L"." : lastSoundFontDir;
            if (scannedDir != scanDir) {
                scannedDir = scanDir;
                folderFonts.clear();
                std::error_code ec;
                for (std::filesystem::directory_iterator it(scanDir, ec), end;
                     it != end; ++it) {
                    if (ec) break;
                    const std::wstring ext = it->path().extension().wstring();
                    if (_wcsicmp(ext.c_str(), L".sf2") == 0 ||
                        _wcsicmp(ext.c_str(), L".dls") == 0) {
                        folderFonts.push_back(it->path().filename().wstring());
                    }
                }
                std::sort(folderFonts.begin(), folderFonts.end());
            }
        }

        const float sfWidth = (std::min)(420.0f, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(sfWidth);
        ImGui::InputTextWithHint("##sfsearch",
                                 "Filter SoundFonts in folder…",
                                 searchBuf, sizeof(searchBuf));

        ImGui::BeginChild("sfList", ImVec2(sfWidth, 150.0f), true);
        std::string filter = searchBuf;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) -> char {
                           return static_cast<char>(std::tolower(c));
                       });
        for (const auto& file : folderFonts) {
            std::string name = WideToUtf8Str(file);
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) -> char {
                               return static_cast<char>(std::tolower(c));
                           });
            if (!filter.empty() && lower.find(filter) == std::string::npos) {
                continue;
            }
            const bool selected = !w.soundFontPath.empty() &&
                _wcsicmp(file.c_str(),
                         std::filesystem::path(w.soundFontPath).filename().c_str()) == 0;
            if (ImGui::Selectable(name.c_str(), selected)) {
                SetPrimarySoundFont(
                    w, (std::filesystem::path(scannedDir) / file).wstring());
                doc.MarkDirty();
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::SeparatorText("PRIORITY STACK");
        ImGui::TextDisabled(
            "The first matching bank wins unless an explicit route below matches first.");
        if (ImGui::Button("Add SoundFont...", ImVec2(130.0f, 0.0f))) {
            std::wstring selected;
            if (w.soundFontPaths.size() < 16u &&
                BrowseSoundFont(selected, lastSoundFontDir,
                                L"Add SoundFont to stack")) {
                bool duplicate = false;
                for (const std::wstring& existing : w.soundFontPaths)
                    duplicate |= _wcsicmp(existing.c_str(), selected.c_str()) == 0;
                if (!duplicate) {
                    w.soundFontPaths.push_back(selected);
                    if (w.soundFontPath.empty())
                        w.soundFontPath = w.soundFontPaths.front();
                    doc.MarkDirty();
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Up to 16 banks; stack changes activate after restart.");

        for (uint32_t i = 0u; i < w.soundFontPaths.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string label = std::to_string(i) + ": " +
                WideToUtf8Str(std::filesystem::path(w.soundFontPaths[i]).filename().wstring());
            ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine();
            if (i == 0u) ImGui::TextDisabled("(primary)");
            if (ImGui::GetContentRegionAvail().x > 180.0f) {
                ImGui::SameLine(ImGui::GetCursorPosX() +
                                ImGui::GetContentRegionAvail().x - 170.0f);
            }
            if (i == 0u) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Up")) {
                SwapSoundFonts(w, i, i - 1u);
                doc.MarkDirty();
                ImGui::PopID();
                break;
            }
            if (i == 0u) ImGui::EndDisabled();
            ImGui::SameLine();
            if (i + 1u >= w.soundFontPaths.size()) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Down")) {
                SwapSoundFonts(w, i, i + 1u);
                doc.MarkDirty();
                ImGui::PopID();
                break;
            }
            if (i + 1u >= w.soundFontPaths.size()) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                RemoveSoundFont(w, i);
                doc.MarkDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::SeparatorText("BANK / PRESET ROUTES");
        if (ImGui::Button("Add Route", ImVec2(100.0f, 0.0f)) &&
            !w.soundFontPaths.empty() && w.soundFontRoutes.size() < 256u) {
            SoundFontRouteValue route{};
            route.soundFontIndex = static_cast<uint32_t>(
                w.soundFontPaths.size() > 1u ? 1u : 0u);
            w.soundFontRoutes.push_back(route);
            doc.MarkDirty();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Preset -1 means keep the incoming program.");

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
            ImGui::SetNextItemWidth((std::min)(260.0f,
                                               ImGui::GetContentRegionAvail().x));
            if (ImGui::BeginCombo("SoundFont", preview)) {
                for (uint32_t sf = 0u; sf < w.soundFontPaths.size(); ++sf) {
                    const std::string name = WideToUtf8Str(std::filesystem::path(
                        w.soundFontPaths[sf]).filename().wstring());
                    if (ImGui::Selectable(name.c_str(),
                                          route.soundFontIndex == sf)) {
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
                route.targetBank = static_cast<uint32_t>((std::clamp)(
                    targetBank, 0, 127));
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
                route.sourceBank = static_cast<uint32_t>((std::clamp)(
                    sourceBank, 0, 65535));
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
            if (ImGui::SmallButton("Remove route")) {
                w.soundFontRoutes.erase(w.soundFontRoutes.begin() + i);
                doc.MarkDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
}

} // namespace svms::cfg
