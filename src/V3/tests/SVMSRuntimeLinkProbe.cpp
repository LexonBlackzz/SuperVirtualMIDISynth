// SVMSRuntimeLinkProbe.cpp — child process for the two-process smoke
// test.  Connects to a RuntimeLink V2 driver hosted in ANOTHER process
// (the parent loads winmm.dll), waits for settled telemetry, pings,
// applies a live reverb change, waits for the telemetry echo, then
// exits 0 on success (1 on failure).
//
// Usage: SVMSRuntimeLinkProbe --host-pid <pid>
//
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../SVMSRuntimeLink.h"

namespace {

bool WaitForTelemetry(svms::RuntimeLinkClientV2& client,
                      svms::RuntimeLinkTelemetryV2& out,
                      unsigned int timeoutMs) {
    const DWORD start = GetTickCount();
    do {
        if (client.ReadTelemetry(out) && out.timestampQpc != 0u) return true;
        Sleep(25);
    } while (static_cast<int>(GetTickCount() - start) < (int)timeoutMs);
    return false;
}

} // namespace

int main(int argc, char** argv) {
    uint32_t hostPid = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host-pid") == 0 && i + 1 < argc) {
            hostPid = static_cast<uint32_t>(std::atoi(argv[i + 1]));
        }
    }
    if (hostPid == 0u) {
        std::puts("PROBE-FAIL: --host-pid required");
        return 1;
    }

    svms::RuntimeLinkClientV2 client;
    if (!client.Open(hostPid)) {
        std::printf("PROBE-FAIL: cannot open driver mapping for pid %u\n",
                    hostPid);
        return 1;
    }

    svms::RuntimeLinkTelemetryV2 t{};
    if (!WaitForTelemetry(client, t, 15000)) {
        std::puts("PROBE-FAIL: no telemetry within 15 s");
        return 1;
    }
    std::printf("PROBE-INFO: telemetry from pid %u (sr=%u)\n",
                hostPid, t.sampleRate);

    char resultText[svms::kRuntimeLinkResultTextCapacity] = {};
    if (client.SendPing(2000, resultText) != svms::RLResult::Ok) {
        std::puts("PROBE-FAIL: ping rejected");
        return 1;
    }

    // Apply a live reverb change (two knobs) through the REAL command
    // path: cross-process command slot + ACK.
    {
        svms::RuntimeLiveStateV2 live{};
        live.masterVolume = 1.0f;
        live.reverbEnabled = 1u;
        live.reverbMix = 0.42f;
        live.reverbRoomSize = 0.70f;
        const svms::RLResult r = client.SendCommand(
            svms::RLCommandType::ApplyLiveConfig,
            svms::RLGroupMaster | svms::RLGroupReverb,
            0u, live, 4000, resultText);
        if (r != svms::RLResult::Ok) {
            std::printf("PROBE-FAIL: ApplyLiveConfig returned %d (%s)\n",
                        (int)r, resultText);
            return 1;
        }

        const DWORD start = GetTickCount();
        bool echoed = false;
        do {
            if (client.ReadTelemetry(t) &&
                t.live.reverbMix > 0.419f && t.live.reverbMix < 0.421f &&
                t.live.reverbRoomSize > 0.699f &&
                t.live.reverbRoomSize < 0.701f) {
                echoed = true;
                break;
            }
            Sleep(25);
        } while (static_cast<int>(GetTickCount() - start) < 8000);
        if (!echoed) {
            std::printf("PROBE-FAIL: telemetry echo missing "
                        "(mix=%.3f room=%.3f)\n",
                        t.live.reverbMix, t.live.reverbRoomSize);
            return 1;
        }
    }
    std::printf("PROBE-OK: ping + ApplyLiveConfig ACK + echo verified\n");
    return 0;
}