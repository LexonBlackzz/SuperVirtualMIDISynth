#include "SVMSTypes.h"

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 3;
    }
    using Available = int (*)();
    using Statistics = svms::SnappyVoiceStatistics (*)();
    const auto available = reinterpret_cast<Available>(
        dlsym(library, "IsKDMAPIAvailable"));
    const auto statistics = reinterpret_cast<Statistics>(
        dlsym(library, "GetVoiceStatistics"));
    const bool exportsPresent =
        available && statistics && dlsym(library, "InitializeKDMAPIStream") &&
        dlsym(library, "TerminateKDMAPIStream") &&
        dlsym(library, "ResetKDMAPIStream") &&
        dlsym(library, "SendDirectData") &&
        dlsym(library, "SendDirectLongData") &&
        dlsym(library, "GetRenderingTime") && dlsym(library, "GetVoiceCount");
    if (!exportsPresent || available() == 0) return 4;
    const svms::SnappyVoiceStatistics initial = statistics();
    if (initial.activeVoices || initial.freeVoices || initial.voiceSteals)
        return 5;
    dlclose(library);
    return 0;
}
