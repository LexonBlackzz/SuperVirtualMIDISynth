#include "PageDiagnostics.h"
#include "ConfigDocument.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "dxgi.lib")

namespace svms::cfg {
namespace {

std::string WideToUtf8Diag(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
                                        static_cast<int>(ws.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        result.data(), len, nullptr, nullptr);
    return result;
}

std::string CpuName() {
#ifdef _MSC_VER
    int regs[4] = {};
    __cpuid(regs, 0x80000000);
    const unsigned maxExt = static_cast<unsigned>(regs[0]);
    if (maxExt >= 0x80000004u) {
        char brand[49] = {};
        int* dst = reinterpret_cast<int*>(brand);
        __cpuid(dst + 0, 0x80000002);
        __cpuid(dst + 4, 0x80000003);
        __cpuid(dst + 8, 0x80000004);
        std::string s(brand);
        const auto first = s.find_first_not_of(' ');
        const auto last = s.find_last_not_of(' ');
        if (first != std::string::npos)
            return s.substr(first, last - first + 1);
    }
#endif
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    return std::string("Windows CPU (") +
           std::to_string(si.dwNumberOfProcessors) + " logical processors)";
}

std::string GpuName() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory)))) {
        return "Unavailable";
    }

    std::string result = "Unavailable";
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            result = WideToUtf8Diag(desc.Description);
            adapter->Release();
            break;
        }
        adapter->Release();
    }
    factory->Release();
    return result;
}

void MemoryText(char* out, size_t outSize) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        std::snprintf(out, outSize, "Unavailable");
        return;
    }
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    const double total = static_cast<double>(ms.ullTotalPhys) / kGiB;
    const double used = static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / kGiB;
    std::snprintf(out, outSize, "%.1f / %.1f GB used", used, total);
}

void KeyValue(const char* key, const char* value, bool wrap = false) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", key);
    ImGui::TableNextColumn();
    if (wrap) ImGui::TextWrapped("%s", value);
    else ImGui::TextUnformatted(value);
}

bool BeginDetailsTable(const char* id) {
    if (!ImGui::BeginTable(id, 2,
                           ImGuiTableFlags_SizingStretchProp |
                           ImGuiTableFlags_BordersInnerH |
                           ImGuiTableFlags_RowBg,
                           ImVec2(0.0f, 0.0f))) {
        return false;
    }
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    return true;
}

std::string AudioDeviceText(const ConfigValues& w) {
    if (w.audioDevice.empty() || w.audioDevice == L"default")
        return "Default Windows Output Device";
    return WideToUtf8Diag(w.audioDevice);
}

std::string SoundFontText(const ConfigValues& w) {
    if (w.soundFontPath.empty()) return "Automatic / local fallback";
    return WideToUtf8Diag(w.soundFontPath);
}

float BudgetMs(const svms::RuntimeLinkTelemetryV2& t) {
    return t.sampleRate > 0u
        ? 1000.0f * static_cast<float>(t.bufferFrames) /
          static_cast<float>(t.sampleRate)
        : 0.0f;
}

float BudgetPercentMs(const svms::RuntimeLinkTelemetryV2& t, float percent) {
    return BudgetMs(t) * percent * 0.01f;
}

std::string BuildClipboardReport(const ConfigValues& w,
                                 const svms::RuntimeLinkTelemetryV2* t,
                                 bool connected,
                                 const std::string& cpu,
                                 const std::string& gpu,
                                 const char* memory) {
    std::ostringstream out;
    out << "SuperVirtualMIDISynth V3 diagnostics\n";
    out << "CPU: " << cpu << "\n";
    out << "RAM: " << memory << "\n";
    out << "GPU: " << gpu << "\n";
    out << "Logical processors: " << std::thread::hardware_concurrency() << "\n";
    out << "RuntimeLink: " << (connected ? "Connected" : "Offline") << "\n";
    out << "Configured render threads: ";
    if (w.renderThreads == 0) out << "Auto\n";
    else out << w.renderThreads << "\n";
    out << "Audio device: " << AudioDeviceText(w) << "\n";
    out << "SoundFont: " << SoundFontText(w) << "\n";
    out << "Configured sample rate: " << w.sampleRate << " Hz\n";
    out << "Configured buffer: " << w.bufferFrames << " frames\n";

    if (connected && t) {
        const float budgetMs = BudgetMs(*t);
        const float renderMs = BudgetPercentMs(*t, t->cpuLoadPercent);
        const uint32_t logicalCap = t->live.maxVoices != 0u
            ? t->live.maxVoices : t->maxVoices;
        out << "Active voices: " << t->activeVoices << " / " << logicalCap << " logical\n";
        out << "Physical voice pool: " << t->maxVoices << "\n";
        out << "Releasing voices: " << t->releasingVoices << "\n";
        out << "Voice steals: " << t->voiceSteals << "\n";
        out << "Callback budget: " << budgetMs << " ms\n";
        out << "Render load: " << t->cpuLoadPercent << "% (~" << renderMs << " ms)\n";
        out << "Render headroom: " << (std::max)(0.0f, 100.0f - t->cpuLoadPercent) << "%\n";
        out << "Callback P95: " << t->callbackP95Percent << "% (~"
            << BudgetPercentMs(*t, t->callbackP95Percent) << " ms)\n";
        out << "Callback P99: " << t->callbackP99Percent << "% (~"
            << BudgetPercentMs(*t, t->callbackP99Percent) << " ms)\n";
        out << "Callback P99.9: " << t->callbackP999Percent << "% (~"
            << BudgetPercentMs(*t, t->callbackP999Percent) << " ms)\n";
        out << "Over-budget callbacks: " << t->overBudgetCallbacks << "\n";
        out << "Max consecutive over-budget: " << t->maxConsecutiveOverBudget << "\n";
        out << "Events submitted/accepted/dropped/dispatched: "
            << t->eventsSubmitted << " / " << t->eventsAccepted << " / "
            << t->eventsDropped << " / " << t->eventsDispatched << "\n";
        out << "Audio: " << (t->audioRunning ? "Running" : "Stopped") << "\n";
        out << "SoundFont loaded: " << (t->soundFontLoaded ? "Yes" : "No") << "\n";
    }
    return out.str();
}

} // namespace

void DrawDiagnosticsPage(ConfigDocument& doc) {
    const auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;

    static const std::string cpu = CpuName();
    static const std::string gpu = GpuName();
    static bool showDetails = false;

    char memory[96] = {};
    MemoryText(memory, sizeof(memory));

    SectionHeader("DIAGNOSTICS");

    if (ImGui::BeginTable("##diag_essentials", 4,
                          ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("Voice label", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Voice value", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Render label", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Render value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextDisabled("VOICES");
        ImGui::TableNextColumn();
        if (lc.connected && t) {
            const uint32_t logicalCap = t->live.maxVoices != 0u
                ? t->live.maxVoices : t->maxVoices;
            ImGui::Text("%u / %u", t->activeVoices, logicalCap);
        } else {
            ImGui::TextDisabled("-- / %u", w.maxVoices);
        }

        ImGui::TableNextColumn();
        ImGui::TextDisabled("RENDER");
        ImGui::TableNextColumn();
        if (lc.connected && t && t->sampleRate > 0u) {
            const float renderMs = BudgetPercentMs(*t, t->cpuLoadPercent);
            ImGui::Text("%.2f ms", renderMs);
            ImGui::SameLine();
            ImGui::TextDisabled("(%.1f%% of callback budget)", t->cpuLoadPercent);
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::EndTable();
    }

    if (!lc.connected || !t) {
        ImGui::Spacing();
        ImGui::TextDisabled("RuntimeLink is offline. Live voice and render values are unavailable.");
    }

    ImGui::Spacing();
    SectionHeader("SYSTEM");
    if (BeginDetailsTable("##diag_system")) {
        KeyValue("CPU", cpu.c_str(), true);
        KeyValue("Memory", memory);
        KeyValue("GPU", gpu.c_str(), true);
        const std::string logical = std::to_string(std::thread::hardware_concurrency()) +
                                    " logical processors";
        KeyValue("Processor count", logical.c_str());
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button(showDetails ? "Hide details" : "More details")) {
        showDetails = !showDetails;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Engine, render-budget, audio and event-pipeline details");

    if (!showDetails) return;

    if (lc.connected && t) {
        ImGui::Spacing();
        SectionHeader("RENDER BUDGET");
        if (BeginDetailsTable("##diag_budget")) {
            char value[160] = {};
            const float budget = BudgetMs(*t);
            std::snprintf(value, sizeof(value), "%.2f ms (%u frames @ %u Hz)",
                          budget, t->bufferFrames, t->sampleRate);
            KeyValue("Block budget", value);

            std::snprintf(value, sizeof(value), "%.2f ms / %.1f%%",
                          BudgetPercentMs(*t, t->cpuLoadPercent),
                          t->cpuLoadPercent);
            KeyValue("Current render", value);

            std::snprintf(value, sizeof(value), "%.1f%%",
                          (std::max)(0.0f, 100.0f - t->cpuLoadPercent));
            KeyValue("Current headroom", value);

            std::snprintf(value, sizeof(value), "%.2f ms / %.1f%%",
                          BudgetPercentMs(*t, t->callbackP95Percent),
                          t->callbackP95Percent);
            KeyValue("P95", value);

            std::snprintf(value, sizeof(value), "%.2f ms / %.1f%%",
                          BudgetPercentMs(*t, t->callbackP99Percent),
                          t->callbackP99Percent);
            KeyValue("P99", value);

            std::snprintf(value, sizeof(value), "%.2f ms / %.1f%%",
                          BudgetPercentMs(*t, t->callbackP999Percent),
                          t->callbackP999Percent);
            KeyValue("P99.9", value);

            std::snprintf(value, sizeof(value), "%llu total / %u max streak",
                          static_cast<unsigned long long>(t->overBudgetCallbacks),
                          t->maxConsecutiveOverBudget);
            KeyValue("Over budget", value);
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    SectionHeader("ENGINE / AUDIO");
    if (BeginDetailsTable("##diag_engine")) {
        KeyValue("RuntimeLink", lc.connected ? "Connected" : "Offline");

        char threads[64] = {};
        if (w.renderThreads == 0)
            std::snprintf(threads, sizeof(threads), "Auto (runtime count not exposed)");
        else
            std::snprintf(threads, sizeof(threads), "%u configured", w.renderThreads);
        KeyValue("Render threads", threads);

        const std::string device = AudioDeviceText(w);
        KeyValue("Audio device", device.c_str(), true);

        char format[96] = {};
        const float configuredMs = w.sampleRate > 0u
            ? 1000.0f * static_cast<float>(w.bufferFrames) /
              static_cast<float>(w.sampleRate) : 0.0f;
        std::snprintf(format, sizeof(format), "%u Hz / %u frames / %.2f ms",
                      w.sampleRate, w.bufferFrames, configuredMs);
        KeyValue("Configured format", format);

        const std::string soundFont = SoundFontText(w);
        KeyValue("SoundFont", soundFont.c_str(), true);

        if (lc.connected && t) {
            KeyValue("Audio state", t->audioRunning ? "Running" : "Stopped");
            KeyValue("SoundFont state", t->soundFontLoaded ? "Loaded" : "Not loaded");

            char voices[160] = {};
            const uint32_t logicalCap = t->live.maxVoices != 0u
                ? t->live.maxVoices : t->maxVoices;
            std::snprintf(voices, sizeof(voices),
                          "%u active / %u releasing / %u logical cap / %u physical pool",
                          t->activeVoices, t->releasingVoices,
                          logicalCap, t->maxVoices);
            KeyValue("Voice state", voices);

            char steals[64] = {};
            std::snprintf(steals, sizeof(steals), "%u", t->voiceSteals);
            KeyValue("Voice steals", steals);
        }
        ImGui::EndTable();
    }

    if (lc.connected && t) {
        ImGui::Spacing();
        SectionHeader("EVENT PIPELINE");
        if (BeginDetailsTable("##diag_events")) {
            char submitted[64] = {}, accepted[64] = {}, dropped[64] = {}, dispatched[64] = {};
            std::snprintf(submitted, sizeof(submitted), "%llu",
                          static_cast<unsigned long long>(t->eventsSubmitted));
            std::snprintf(accepted, sizeof(accepted), "%llu",
                          static_cast<unsigned long long>(t->eventsAccepted));
            std::snprintf(dropped, sizeof(dropped), "%llu",
                          static_cast<unsigned long long>(t->eventsDropped));
            std::snprintf(dispatched, sizeof(dispatched), "%llu",
                          static_cast<unsigned long long>(t->eventsDispatched));
            KeyValue("Submitted", submitted);
            KeyValue("Accepted", accepted);
            KeyValue("Dropped", dropped);
            KeyValue("Dispatched", dispatched);
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Copy diagnostics")) {
        const std::string report = BuildClipboardReport(w, t, lc.connected,
                                                        cpu, gpu, memory);
        ImGui::SetClipboardText(report.c_str());
    }
}

} // namespace svms::cfg
