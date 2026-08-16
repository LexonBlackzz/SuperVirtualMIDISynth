#include "PageAudio.h"
#include "ConfigDocument.h"
#include "WasapiDevices.h"
#include "EasterEggs.h"
#include "Widgets.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace svms::cfg {

static std::string WideToUtf8Str(const std::wstring& ws) {
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

void DrawAudioPage(ConfigDocument& doc, const EasterEggState& easterEggs) {
    auto& w = doc.Working();

    static WasapiDeviceList deviceList;
    static bool devicesEnumerated = false;
    if (!devicesEnumerated) {
        deviceList.Enumerate();
        devicesEnumerated = true;
    }

    SectionHeader("OUTPUT DEVICE");

    auto names = deviceList.FriendlyNames();
    int currentDevice = 0;
    std::string configuredUtf8 = WideToUtf8Str(w.audioDevice);

    if (w.audioDevice.empty() || w.audioDevice == L"default") {
        currentDevice = deviceList.DefaultIndex();
    } else {
        for (size_t i = 0; i < deviceList.Devices().size(); ++i) {
            const auto& dev = deviceList.Devices()[i];
            std::string devName = WideToUtf8Str(dev.friendlyName);
            std::string cfgLower = configuredUtf8;
            std::string devLower = devName;
            std::transform(cfgLower.begin(), cfgLower.end(), cfgLower.begin(),
                           [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
            std::transform(devLower.begin(), devLower.end(), devLower.begin(),
                           [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
            if (devLower.find(cfgLower) != std::string::npos ||
                cfgLower == WideToUtf8Str(dev.id)) {
                currentDevice = static_cast<int>(i + 1);
                break;
            }
        }
    }

    const char** namePtrs = new const char*[names.size()];
    for (size_t i = 0; i < names.size(); ++i) {
        namePtrs[i] = names[i].c_str();
    }

    const char* displayDevice = namePtrs[currentDevice];
    if (easterEggs.megaFuckerDac && currentDevice != 0) {
        static const char* megaName = "MegaFucker DAC Pro 9000";
        displayDevice = megaName;
    }

    int newDevice = currentDevice;
    ImGui::PushItemWidth(300.0f);
    if (ImGui::Combo("##device", &newDevice, namePtrs, static_cast<int>(names.size()))) {
        if (newDevice == 0) {
            w.audioDevice = L"default";
        } else {
            const auto& dev = deviceList.Devices()[static_cast<size_t>(newDevice - 1)];
            w.audioDevice = dev.friendlyName;
        }
        doc.MarkDirty();
    }
    ImGui::PopItemWidth();

    if (currentDevice == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                "Uses the current Windows default audio output device.");
            ImGui::EndTooltip();
        }
    }

    if (easterEggs.megaFuckerDac) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.20f, 1.0f));
        ImGui::Text("Note: Display override active. "
                    "Your actual device has NOT changed.");
        ImGui::PopStyleColor();
    }

    bool needsRefresh = ImGui::Button("Refresh Devices");
    if (needsRefresh) {
        deviceList.Enumerate();
    }

    delete[] namePtrs;

    ImGui::Spacing();
    SectionHeader("SAMPLE RATE");

    static const char* sampleRateItems[] = {
        "44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz",
        "176400 Hz", "192000 Hz"
    };
    static const uint32_t sampleRateValues[] = {
        44100, 48000, 88200, 96000, 176400, 192000
    };

    int srIdx = 0;
    for (int i = 0; i < 6; ++i) {
        if (sampleRateValues[i] == w.sampleRate) {
            srIdx = i;
            break;
        }
    }

    ImGui::PushItemWidth(200.0f);
    if (ImGui::Combo("##samplerate", &srIdx, sampleRateItems, 6)) {
        w.sampleRate = sampleRateValues[srIdx];
        doc.MarkDirty();
    }
    ImGui::PopItemWidth();

    HelpMarker("Sample rate for audio output. Higher rates improve quality "
               "but increase CPU load.");

    ImGui::Spacing();
    SectionHeader("BUFFER FRAMES");

    static const char* bufferItems[] = {
        "64", "128", "256", "512", "1024", "2048", "4096", "8192"
    };
    static const uint32_t bufferValues[] = {
        64, 128, 256, 512, 1024, 2048, 4096, 8192
    };

    int bufIdx = 5;
    for (int i = 0; i < 8; ++i) {
        if (bufferValues[i] == w.bufferFrames) {
            bufIdx = i;
            break;
        }
    }

    ImGui::PushItemWidth(200.0f);
    if (ImGui::Combo("##buffer", &bufIdx, bufferItems, 8)) {
        w.bufferFrames = bufferValues[bufIdx];
        doc.MarkDirty();
    }
    ImGui::PopItemWidth();

    float latencyMs = (static_cast<float>(w.bufferFrames) /
                       static_cast<float>(w.sampleRate)) *
                      1000.0f;
    ImGui::SameLine();
    char latencyBuf[64];
    snprintf(latencyBuf, sizeof(latencyBuf), "%u frames @ %u Hz = %.2f ms",
             w.bufferFrames, w.sampleRate, latencyMs);
    ImGui::TextDisabled("%s", latencyBuf);

    HelpMarker("Audio endpoint buffer size. Smaller buffers reduce latency "
               "but leave less time for each render callback.");

    ImGui::Spacing();
    SectionHeader("AUDIO BACKEND");

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
    ImGui::Text("WASAPI Shared Mode (default)");
    ImGui::PopStyleColor();
    HelpMarker("V3 uses Windows Audio Session API in shared mode. "
               "This is the only production backend on modern builds.");
}

} // namespace svms::cfg
