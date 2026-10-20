#include "configurator/PageDiagnostics.h"
#include "configurator/PlatformUtil.h"
#include "configurator/ConfigDocument.h"
#include "configurator/ConfiguratorApp.h"
#include "configurator/DriverLink.h"
#include "configurator/ModernWidgets.h"
#include "configurator/Widgets.h"
#include "imgui.h"
#include "driver/RuntimeLinkProtocol.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#else
#include <fstream>
#include <sys/sysinfo.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#pragma comment(lib, "dxgi.lib")
#endif

namespace svms::cfg {
namespace {

std::string WideToUtf8Diag(const std::wstring& ws) {
    return svms::cfg::util::WideToUtf8(ws);
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
#ifdef _WIN32
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    return std::string("Windows CPU (") +
           std::to_string(si.dwNumberOfProcessors) + " logical processors)";
#else
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.compare(0, 10, "model name") != 0) continue;
        const auto colon = line.find(':');
        if (colon != std::string::npos && colon + 2 <= line.size())
            return line.substr(colon + 2);
    }
    return std::string("Linux CPU (") +
           std::to_string(std::thread::hardware_concurrency()) + " logical processors)";
#endif
}

std::string GpuName() {
#ifndef _WIN32
    return "Unavailable";
#else
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
#endif
}

// The backend the engine picks on this CPU when the config says Auto.
const char* DetectedBackend() {
#ifdef _WIN32
    if (IsProcessorFeaturePresent(40 /* PF_AVX2_INSTRUCTIONS_AVAILABLE */)) return "AVX2";
    return "SSE2";
#elif defined(__GNUC__)
    return __builtin_cpu_supports("avx2") ? "AVX2" : "SSE2";
#else
    return "SSE2";
#endif
}

void MemoryText(char* out, size_t outSize) {
#ifndef _WIN32
    struct sysinfo si{};
    if (sysinfo(&si) != 0) {
        std::snprintf(out, outSize, "Unavailable");
        return;
    }
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    const double total = static_cast<double>(si.totalram) * si.mem_unit / kGiB;
    const double freeRam = static_cast<double>(si.freeram + si.bufferram) * si.mem_unit / kGiB;
    std::snprintf(out, outSize, "%.1f / %.1f GB used", total - freeRam, total);
    return;
#else
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
#endif
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
        out << "Scheduler / event dispatch: " << t->schedulerPercent
            << "% / " << t->eventDispatchPercent << "%\n";
        out << "Raw / compiled / scheduled events: "
            << t->rawIngressCount << " / " << t->compiledPagedCount
            << " / " << t->scheduledBacklogCount << "\n";
        out << "Audio: " << (t->audioRunning ? "Running" : "Stopped") << "\n";
        out << "SoundFont loaded: " << (t->soundFontLoaded ? "Yes" : "No") << "\n";
    }
    return out.str();
}

} // namespace

void DrawDiagnosticsPage(ConfigDocument& doc) {
    const auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();
    const auto* t = lc.connected ? lc.telemetry : nullptr;
    const float s = ui::Scale();

    static const std::string cpu = CpuName();
    static const std::string gpu = GpuName();
    char memory[96] = {};
    MemoryText(memory, sizeof(memory));

    // Render budget: one callback's worth of time.
    char budget[48];
    const float budgetMs = t ? BudgetMs(*t)
        : 1000.0f * static_cast<float>(w.bufferFrames) / static_cast<float>((std::max)(1u, w.sampleRate));
    std::snprintf(budget, sizeof(budget), "%.1f ms per block", budgetMs);
    ui::PushText(0, 13.0f);
    const float budgetW = ImGui::CalcTextSize(budget).x;
    ui::PopFont();
    ui::BeginCard("##budget", "Render budget", nullptr, budgetW, "diag");
    ui::PushText(0, 13.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (38.0f * s - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
    ImGui::TextUnformatted(budget);
    ImGui::PopStyleColor();
    ui::PopFont();
    ui::CardHeaderEnd();
    char load[24], head[24], p99[24], over[24];
    const char* dash = "\xE2\x80\x94";
    if (t) {
        std::snprintf(load, sizeof(load), "%.0f%%", t->cpuLoadPercent);
        std::snprintf(head, sizeof(head), "%.0f%%", (std::max)(0.0f, 100.0f - t->cpuLoadPercent));
        std::snprintf(p99, sizeof(p99), "%.0f%%", t->callbackP99Percent);
        std::snprintf(over, sizeof(over), "%llu", static_cast<unsigned long long>(t->overBudgetCallbacks));
    } else {
        std::snprintf(load, sizeof(load), "%s", dash);
        std::snprintf(head, sizeof(head), "%s", dash);
        std::snprintf(p99, sizeof(p99), "%s", dash);
        std::snprintf(over, sizeof(over), "%s", dash);
    }
    const char* labels[4] = {"Render load", "Headroom", "P99", "Over budget"};
    const char* values[4] = {load, head, p99, over};
    ui::StatGrid(labels, values, 4, false);
    if (!t) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
        ImGui::TextUnformatted("No app is using SVMS right now, so live numbers are unavailable.");
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    ui::EndCard();

    // This PC + the clipboard report.
    ui::BeginCard("##thispc", "This PC", nullptr, ui::ButtonSize("Copy report").x);
    if (ui::Button("Copy report")) {
        const std::string report = BuildClipboardReport(w, t, lc.connected, cpu, gpu, memory);
        ImGui::SetClipboardText(report.c_str());
        if (ConfiguratorApp* app = lc.app) app->ShowToast("Report copied");
    }
    ui::CardHeaderEnd();
    const std::string logical = std::to_string(std::thread::hardware_concurrency());
    const std::string backend = std::string(DetectedBackend());
    const char* keys[5] = {"CPU", "Logical processors", "Memory", "GPU", "Render backend"};
    const char* vals[5] = {cpu.c_str(), logical.c_str(), memory, gpu.c_str(), backend.c_str()};
    ui::KeyValueGrid(keys, vals, 5);
    ui::EndCard();
}

} // namespace svms::cfg
