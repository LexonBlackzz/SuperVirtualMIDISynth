#include "PageOverview.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace svms::cfg {

void DrawOverviewPage(ConfigDocument& doc) {
    auto& w = doc.Working();
    auto& lc = GetLiveLinkContext();

    SectionHeader("SOUND SOURCE");

    if (w.soundFontPath.empty()) {
        ImGui::TextDisabled("SoundFont: Automatic");
        auto resolved = std::filesystem::path(w.soundFontPath);
        if (!resolved.empty()) {
            ImGui::Text("Resolved: %s", resolved.string().c_str());
        } else {
            ImGui::TextDisabled("Resolved: (no SoundFont found)");
        }
    } else {
        ImGui::Text("SoundFont: %s", std::filesystem::path(w.soundFontPath)
                                         .filename()
                                         .string()
                                         .c_str());
        ImGui::TextDisabled("Full path: %s",
                            std::filesystem::path(w.soundFontPath)
                                .string()
                                .c_str());
    }
    ImGui::Spacing();

    SectionHeader("AUDIO");

    const char* deviceName = "Default Windows Output Device";
    std::string deviceStr;
    if (!w.audioDevice.empty() && w.audioDevice != L"default") {
        int len = WideCharToMultiByte(CP_UTF8, 0, w.audioDevice.data(),
                                      static_cast<int>(w.audioDevice.size()),
                                      nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            deviceStr.resize(static_cast<size_t>(len));
            WideCharToMultiByte(CP_UTF8, 0, w.audioDevice.data(),
                                static_cast<int>(w.audioDevice.size()),
                                deviceStr.data(), len, nullptr, nullptr);
            deviceName = deviceStr.c_str();
        }
    }
    ImGui::Text("Device: %s", deviceName);
    RestartRequiredBadge();
    ImGui::Text("Sample rate: %u Hz", w.sampleRate);
    RestartRequiredBadge();
    float latencyMs = (static_cast<float>(w.bufferFrames) /
                       static_cast<float>(w.sampleRate)) *
                      1000.0f;
    ImGui::Text("Buffer: %u frames / %.2f ms", w.bufferFrames, latencyMs);
    RestartRequiredBadge();
    ImGui::Spacing();

    SectionHeader("PERFORMANCE");

    ImGui::Text("Max voices: %u", w.maxVoices);
    RestartRequiredBadge();
    if (w.renderThreads == 0)
        ImGui::Text("Render threads: Auto");
    else
        ImGui::Text("Render threads: %u", w.renderThreads);
    RestartRequiredBadge();
    ImGui::Spacing();

    SectionHeader("EFFECTS");

    ImGui::Text("Reverb: %s", w.enableReverb ? "Enabled" : "Disabled");
    LiveBadge("Reverb enable/disable is applied live via RuntimeLink");
    ImGui::Text("Limiter: %s", w.limiterEnabled ? "Enabled" : "Disabled");
    LiveBadge("Limiter enable/disable is applied live via RuntimeLink");
    ImGui::Spacing();

    SectionHeader("CONFIGURATION");

    auto path = doc.GetActivePath();
    if (!path.empty()) {
        int len = WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                      static_cast<int>(path.size()),
                                      nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string pathStr(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                static_cast<int>(path.size()),
                                pathStr.data(), len, nullptr, nullptr);
            ImGui::TextWrapped("%s", pathStr.c_str());
        }
    } else {
        ImGui::TextDisabled("No config loaded");
    }

    // ── Live telemetry (when RuntimeLink is connected) ─────────────────
    if (lc.connected && lc.telemetry) {
        ImGui::Spacing();
        SectionHeader("LIVE TELEMETRY");

        auto* t = lc.telemetry;

        const char* audioStatus = t->audioRunning ? "Running" : "Stopped";
        ImGui::Text("Audio: %s", audioStatus);
        if (!t->audioRunning && t->audioHResult != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(HRESULT 0x%08X)",
                                static_cast<unsigned>(t->audioHResult));
        }

        ImGui::Text("SoundFont: %s",
                    t->soundFontLoaded ? "Loaded" : "Not loaded");

        ImGui::Spacing();

        ImGui::Text("Active voices: %u / %u", t->activeVoices, t->maxVoices);
        ImGui::Text("Releasing: %u", t->releasingVoices);
        ImGui::Text("Free slots: %u", t->freeTop);
        ImGui::Text("Steals: %u", t->voiceSteals);
        ImGui::Text("Retired: %u (immediate: %u)",
                    t->retiredCount, t->retiredImmediateCount);
        ImGui::Text("Decimation step: %u", t->decimationStep);

        ImGui::Spacing();

        ImGui::Text("CPU load: %.1f%%", t->cpuLoadPercent);
        ImGui::Text("Peak: %.3f", t->renderPeak);
        ImGui::Text("Master volume: %.2f", t->masterVolume);
        ImGui::Text("Sample rate: %u Hz", t->sampleRate);
        ImGui::Text("Buffer: %u frames", t->bufferFrames);

        ImGui::Spacing();
        ImGui::Text("Events submitted: %llu",
                    static_cast<unsigned long long>(t->eventsSubmitted));
        ImGui::Text("Events accepted: %llu",
                    static_cast<unsigned long long>(t->eventsAccepted));
        ImGui::Text("Events dropped: %llu",
                    static_cast<unsigned long long>(t->eventsDropped));
        ImGui::Text("Events dispatched: %llu",
                    static_cast<unsigned long long>(t->eventsDispatched));
    }
}

} // namespace svms::cfg
