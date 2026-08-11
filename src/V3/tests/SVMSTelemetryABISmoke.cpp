#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace {

struct VoiceStatistics {
    uint32_t activeVoices;
    uint32_t freeVoices;
    uint32_t voiceSteals;
};

using GetVoiceStatisticsProc = VoiceStatistics* (WINAPI*)(VoiceStatistics*);
using GetVoiceCountProc = DWORD (WINAPI*)();
using GetRenderingTimeProc = FLOAT (WINAPI*)();
using GetDriverDebugInfoProc = void* (WINAPI*)();

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::puts("FAIL: expected a V3 winmm.dll path");
        return 1;
    }
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        std::printf("FAIL: LoadLibrary returned %lu\n",
                    static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    const auto getStatistics = reinterpret_cast<GetVoiceStatisticsProc>(
        GetProcAddress(module, "GetVoiceStatistics"));
    const auto getVoiceCount = reinterpret_cast<GetVoiceCountProc>(
        GetProcAddress(module, "GetVoiceCount"));
    const auto getRenderingTime = reinterpret_cast<GetRenderingTimeProc>(
        GetProcAddress(module, "GetRenderingTime"));
    const auto getDriverDebugInfo = reinterpret_cast<GetDriverDebugInfoProc>(
        GetProcAddress(module, "GetDriverDebugInfo"));
    if (!getStatistics || !getVoiceCount || !getRenderingTime ||
        !getDriverDebugInfo) {
        std::puts("FAIL: a Ziggy/SSV2 compatibility export is missing");
        FreeLibrary(module);
        return 1;
    }

    VoiceStatistics statistics{1u, 2u, 3u};
    if (getStatistics(&statistics) != &statistics ||
        statistics.activeVoices != 0u || statistics.freeVoices != 0u ||
        statistics.voiceSteals != 0u || getStatistics(nullptr) != nullptr ||
        getVoiceCount() != 0u || getRenderingTime() != 0.0f ||
        getDriverDebugInfo() != nullptr) {
        std::puts("FAIL: uninitialized compatibility telemetry is invalid");
        FreeLibrary(module);
        return 1;
    }
    FreeLibrary(module);
    std::puts("PASS: Ziggy/SSV2 telemetry ABI exports are callable");
    return 0;
}
