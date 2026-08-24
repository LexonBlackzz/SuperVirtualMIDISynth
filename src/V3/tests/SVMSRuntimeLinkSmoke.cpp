// SVMSRuntimeLinkSmoke.cpp — live smoke test for the RuntimeLink V2
// driver (control thread + shared-memory IPC) inside the real winmm.dll.
//
// Flow: load build/bin/winmm.dll → open MIDI_MAPPER → discover the
// driver's host slot (same PID) → connect → wait for settled telemetry
// → send a note-on/off through the real MIDI export → Ping → live
// ApplyLiveConfig (master 2.0 — beyond the old 1.0 clamp — and limiter
// threshold) → verify the echoed telemetry.live → ResetVoices → verify
// telemetry still flows (heartbeat + tickMs advance) → close.
//
// Skips (exit 77) when the machine has no MIDI/audio device.
//
#include <windows.h>
#include <mmsystem.h>

#include "../SVMSRuntimeLink.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

typedef UINT (WINAPI* MidiOutGetNumDevsProc)();
typedef MMRESULT (WINAPI* MidiOutOpenProc)(LPHMIDIOUT, UINT, DWORD_PTR,
                                           DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI* MidiOutShortMsgProc)(HMIDIOUT, DWORD);
typedef MMRESULT (WINAPI* MidiOutPrepareHeaderProc)(HMIDIOUT, LPMIDIHDR, UINT);
typedef MMRESULT (WINAPI* MidiOutLongMsgProc)(HMIDIOUT, LPMIDIHDR, UINT);
typedef MMRESULT (WINAPI* MidiOutUnprepareHeaderProc)(HMIDIOUT, LPMIDIHDR, UINT);
typedef MMRESULT (WINAPI* MidiOutResetProc)(HMIDIOUT);
typedef MMRESULT (WINAPI* MidiOutCloseProc)(HMIDIOUT);

bool WaitForTelemetry(svms::RuntimeLinkClientV2& client,
                      svms::RuntimeLinkTelemetryV2& out,
                      unsigned int timeoutMs) {
    // A zero slot is "settled" too (sequence starts even), so require a
    // non-zero publish timestamp to avoid accepting the pre-publish
    // zeroed mapping.
    const DWORD start = GetTickCount();
    do {
        if (client.ReadTelemetry(out) && out.timestampQpc != 0u) return true;
        Sleep(25);
    } while (static_cast<int>(GetTickCount() - start) < (int)timeoutMs);
    return false;
}

bool WaitForTelemetry(svms::RuntimeLinkClientV3& client,
                      svms::RuntimeLinkTelemetryV2& out,
                      unsigned int timeoutMs) {
    const DWORD start = GetTickCount();
    do {
        if (client.ReadTelemetry(out) && out.timestampQpc != 0u) return true;
        Sleep(25);
    } while (static_cast<int>(GetTickCount() - start) < (int)timeoutMs);
    return false;
}

bool WaitForTelemetry(svms::RuntimeLinkClient& client,
                      svms::RuntimeLinkTelemetryV2& out,
                      unsigned int timeoutMs) {
    const DWORD start = GetTickCount();
    do {
        if (client.ReadTelemetry(out) && out.timestampQpc != 0u) return true;
        Sleep(25);
    } while (static_cast<int>(GetTickCount() - start) < (int)timeoutMs);
    return false;
}

std::string WideToUtf8(const wchar_t* value) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           value, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                            result.data(), count, nullptr, nullptr) != count)
        return {};
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::puts("FAIL: expected the path to the V3 winmm.dll");
        return 1;
    }

    // Isolate from the user's real V3 config; disable diagnostics.
    wchar_t tempDirectory[MAX_PATH] = {};
    wchar_t configPath[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempDirectory) ||
        !GetTempFileNameW(tempDirectory, L"svm", 0, configPath)) {
        std::puts("FAIL: could not reserve an isolated config path");
        return 1;
    }
    DeleteFileW(configPath);
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", configPath);
    SetEnvironmentVariableW(L"SVMS_TEST_LOCAL_CONFIG_PATH", configPath);
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS", L"0");
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS_WINDOW", L"0");
    SetEnvironmentVariableW(L"SVMS_DEBUG_OUTPUT", L"0");

    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        std::printf("FAIL: LoadLibrary returned error %lu\n",
                    static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    MidiOutGetNumDevsProc getNumDevs =
        reinterpret_cast<MidiOutGetNumDevsProc>(
            GetProcAddress(module, "midiOutGetNumDevs"));
    MidiOutOpenProc open = reinterpret_cast<MidiOutOpenProc>(
        GetProcAddress(module, "midiOutOpen"));
    MidiOutShortMsgProc shortMsg = reinterpret_cast<MidiOutShortMsgProc>(
        GetProcAddress(module, "midiOutShortMsg"));
    MidiOutPrepareHeaderProc prepareHeader =
        reinterpret_cast<MidiOutPrepareHeaderProc>(
            GetProcAddress(module, "midiOutPrepareHeader"));
    MidiOutLongMsgProc longMsg = reinterpret_cast<MidiOutLongMsgProc>(
        GetProcAddress(module, "midiOutLongMsg"));
    MidiOutUnprepareHeaderProc unprepareHeader =
        reinterpret_cast<MidiOutUnprepareHeaderProc>(
            GetProcAddress(module, "midiOutUnprepareHeader"));
    MidiOutResetProc reset = reinterpret_cast<MidiOutResetProc>(
        GetProcAddress(module, "midiOutReset"));
    MidiOutCloseProc close = reinterpret_cast<MidiOutCloseProc>(
        GetProcAddress(module, "midiOutClose"));
    if (!getNumDevs || !open || !shortMsg || !prepareHeader || !longMsg ||
        !unprepareHeader || !reset || !close) {
        std::puts("FAIL: required MIDI exports are missing");
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    if (getNumDevs() != 1u) {
        std::puts("FAIL: V3 DLL did not advertise one MIDI output");
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    HMIDIOUT handle = nullptr;
    const MMRESULT result = open(&handle, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        std::printf("FAIL: MIDI_MAPPER open returned %u\n",
                    static_cast<unsigned int>(result));
        FreeLibrary(module);
        DeleteFileW(configPath);
        // No MIDI/audio device available: environment-dependent skip.
        return result == MMSYSERR_NOMEM ? 77 : 1;
    }
    if (!handle) {
        std::puts("FAIL: MIDI_MAPPER open returned a null handle");
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    // Exercise the real WinMM long-message path with a universal GM reset.
    // The driver completes its copy synchronously, just as it does for the
    // KDMAPI raw-buffer entry point, while the reset itself stays timestamped.
    char gmReset[] = {static_cast<char>(0xF0), static_cast<char>(0x7E),
                      static_cast<char>(0x7F), static_cast<char>(0x09),
                      static_cast<char>(0x01), static_cast<char>(0xF7)};
    MIDIHDR sysexHeader = {};
    sysexHeader.lpData = gmReset;
    sysexHeader.dwBufferLength = sizeof(gmReset);
    const MMRESULT prepareResult =
        prepareHeader(handle, &sysexHeader, sizeof(sysexHeader));
    const DWORD preparedFlags = sysexHeader.dwFlags;
    const MMRESULT longResult =
        longMsg(handle, &sysexHeader, sizeof(sysexHeader));
    const DWORD completedFlags = sysexHeader.dwFlags;
    const MMRESULT unprepareResult =
        unprepareHeader(handle, &sysexHeader, sizeof(sysexHeader));
    const DWORD unpreparedFlags = sysexHeader.dwFlags;
    if (prepareResult != MMSYSERR_NOERROR ||
        (preparedFlags & MHDR_PREPARED) == 0u ||
        longResult != MMSYSERR_NOERROR ||
        (completedFlags & MHDR_DONE) == 0u ||
        (completedFlags & MHDR_INQUEUE) != 0u ||
        unprepareResult != MMSYSERR_NOERROR ||
        (unpreparedFlags & MHDR_PREPARED) != 0u) {
        std::printf("FAIL: WinMM SysEx lifecycle handle=%p hdr=%zu prepare=%u/%08lX "
                    "long=%u/%08lX unprepare=%u/%08lX\n",
                    static_cast<void*>(handle), sizeof(sysexHeader),
                    static_cast<unsigned int>(prepareResult), preparedFlags,
                    static_cast<unsigned int>(longResult), completedFlags,
                    static_cast<unsigned int>(unprepareResult),
                    unpreparedFlags);
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    const DWORD ownPid = GetCurrentProcessId();

    // ── 1. Host discovery: the driver must register itself ──────────
    svms::RuntimeLinkClientV2::HostInfo hosts[4];
    uint32_t hostCount = 0;
    {
        const DWORD start = GetTickCount();
        do {
            hostCount = svms::RuntimeLinkClientV2::EnumerateHosts(hosts, 4);
            for (uint32_t i = 0; i < hostCount; ++i) {
                if (hosts[i].pid == ownPid && hosts[i].fresh) break;
            }
            if (hostCount > 0) break;
            Sleep(50);
        } while (static_cast<int>(GetTickCount() - start) < 10000);
    }
    if (hostCount == 0u) {
        std::puts("FAIL: driver never registered a RuntimeLink host slot");
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    uint32_t ownHostIndex = hostCount;
    for (uint32_t i = 0; i < hostCount; ++i) {
        if (hosts[i].pid == ownPid) { ownHostIndex = i; break; }
    }
    if (ownHostIndex == hostCount) {
        std::puts("FAIL: driver host slot has a different PID");
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    // ── 2. Connect and wait for settled telemetry ───────────────────
    svms::RuntimeLinkClientV2 client;
    if (!client.Open(ownPid)) {
        std::puts("FAIL: could not open the driver mapping (ValidateHeader)");
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    svms::RuntimeLinkTelemetryV2 t{};
    if (!WaitForTelemetry(client, t, 15000)) {
        std::puts("FAIL: no telemetry within 15 s (audio thread never published)");
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    if (t.timestampQpc == 0u) {
        std::puts("FAIL: telemetry timestamp is zero");
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    if (t.sampleRate == 0u) {
        std::puts("FAIL: telemetry sampleRate is zero");
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    if (t.live.masterVolume != 1.0f) {
        std::puts("FAIL: default telemetry masterVolume is not 1.0");
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    std::printf("INFO: telemetry OK (sr=%u frames=%u master=%.2f)\n",
                t.sampleRate, t.bufferFrames, t.live.masterVolume);

    // RuntimeLink V3 is published beside, not instead of, V2. Its neutral
    // discovery registry carries version/capability information while the
    // section payload remains exactly compatible in this generation.
    svms::RuntimeLinkClientV3::PeerInfo v3Hosts[4]{};
    const uint32_t v3HostCount =
        svms::RuntimeLinkClientV3::EnumerateHosts(v3Hosts, 4u);
    bool foundV3Host = false;
    for (uint32_t i = 0u; i < v3HostCount; ++i) {
        if (v3Hosts[i].pid == ownPid && v3Hosts[i].fresh) {
            foundV3Host = true;
            break;
        }
    }
    if (!foundV3Host) {
        std::puts("FAIL: V3 discovery registry did not publish this driver");
        reset(handle); close(handle); FreeLibrary(module); DeleteFileW(configPath);
        return 1;
    }

    svms::RuntimeLinkClientV3 v3Client;
    if (!v3Client.Open(ownPid)) {
        std::puts("FAIL: could not negotiate RuntimeLink V3");
        reset(handle); close(handle); FreeLibrary(module); DeleteFileW(configPath);
        return 1;
    }
    const svms::RuntimeLinkClientV3::PeerInfo peer = v3Client.GetPeerInfo();
    if (peer.protocolMin > 3u || peer.protocolMax < 3u ||
        (peer.capabilityFlags & svms::build::CapabilityRuntimeLinkV3) == 0u ||
        (peer.capabilityFlags & svms::build::CapabilityNativeApiV1) == 0u ||
        (peer.capabilityFlags & svms::build::CapabilityLiveRecording) == 0u ||
        (peer.capabilityFlags & svms::build::CapabilitySoundFontReload) == 0u ||
        peer.nativeAbiMin > 1u || peer.nativeAbiMax < 1u ||
        peer.productMajor != svms::build::kProductMajor ||
        peer.buildNumber != svms::build::kBuildNumber) {
        std::puts("FAIL: RuntimeLink V3 discovery metadata is inconsistent");
        reset(handle); close(handle); FreeLibrary(module); DeleteFileW(configPath);
        return 1;
    }
    svms::RuntimeLinkTelemetryV2 v3Telemetry{};
    if (!WaitForTelemetry(v3Client, v3Telemetry, 3000u) ||
        v3Telemetry.sampleRate != t.sampleRate ||
        v3Client.SendPing(1000u) != svms::RLResult::Ok) {
        std::puts("FAIL: RuntimeLink V3 telemetry/command path is not live");
        reset(handle); close(handle); FreeLibrary(module); DeleteFileW(configPath);
        return 1;
    }

    // The configurator-facing negotiator must prefer V3 without callers
    // knowing either mapped layout. V2 fallback is retained by the wrapper.
    svms::RuntimeLinkClient negotiatedClient;
    if (!negotiatedClient.Open(ownPid) ||
        negotiatedClient.GetProtocol() !=
            svms::RuntimeLinkClient::Protocol::V3 ||
        !negotiatedClient.HasCapability(
            svms::build::CapabilityLiveConfiguration)) {
        std::puts("FAIL: negotiated client did not select RuntimeLink V3");
        reset(handle); close(handle); FreeLibrary(module); DeleteFileW(configPath);
        return 1;
    }
    svms::RuntimeLinkTelemetryV2 negotiatedTelemetry{};
    if (!WaitForTelemetry(negotiatedClient, negotiatedTelemetry, 3000u)) {
        std::puts("FAIL: negotiated client could not read telemetry");
        reset(handle); close(handle); FreeLibrary(module); DeleteFileW(configPath);
        return 1;
    }
    negotiatedClient.Close();

    // ── 3. Note through the real export; expect dispatch counters ───
    shortMsg(handle, 0x00643C90u);  // note 60, vel 100, channel 0
    shortMsg(handle, 0x00003C80u);  // note 60 off
    {
        const DWORD start = GetTickCount();
        bool dispatched = false;
        do {
            if (client.ReadTelemetry(t) && t.eventsDispatched > 0u) {
                dispatched = true;
                break;
            }
            Sleep(25);
        } while (static_cast<int>(GetTickCount() - start) < 8000);
        if (!dispatched) {
            std::puts("FAIL: note-on was never dispatched (event pipeline dead)");
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
    }
    std::printf("INFO: note dispatched (submitted=%llu dispatched=%llu)\n",
                static_cast<unsigned long long>(t.eventsSubmitted),
                static_cast<unsigned long long>(t.eventsDispatched));

    // ── 4. Ping ACK ──────────────────────────────────────────────────
    char resultText[svms::kRuntimeLinkResultTextCapacity] = {};
    const svms::RLResult pingResult = client.SendPing(2000, resultText);
    if (pingResult != svms::RLResult::Ok) {
        std::printf("FAIL: Ping returned %d (%s)\n", (int)pingResult, resultText);
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    // ── 5. Live ApplyLiveConfig: master 2.0 (past the old 1.0 clamp)
    //       + limiter threshold; verify the telemetry echo ──────────
    {
        svms::RuntimeLiveStateV2 live{};
        live.masterVolume = 2.0f;
        live.limiterEnabled = 1u;
        live.limiterThreshold = 0.90f;
        const svms::RLResult r = client.SendCommand(
            svms::RLCommandType::ApplyLiveConfig,
            svms::RLGroupMaster | svms::RLGroupLimiter,
            0u, live, 2000, resultText);
        if (r != svms::RLResult::Ok) {
            std::printf("FAIL: ApplyLiveConfig returned %d (%s)\n",
                        (int)r, resultText);
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
        const DWORD start = GetTickCount();
        bool echoed = false;
        do {
            if (client.ReadTelemetry(t) &&
                t.live.masterVolume > 1.999f && t.live.masterVolume < 2.001f &&
                t.live.limiterThreshold > 0.899f &&
                t.live.limiterThreshold < 0.901f) {
                echoed = true;
                break;
            }
            Sleep(25);
        } while (static_cast<int>(GetTickCount() - start) < 8000);
        if (!echoed) {
            std::printf("FAIL: telemetry never echoed live master=%.3f "
                        "limit=%.3f\n", t.live.masterVolume,
                        t.live.limiterThreshold);
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
    }
    std::printf("INFO: live params applied and echoed (master=%.2f "
                "limiter=%.2f)\n", t.live.masterVolume, t.live.limiterThreshold);

    // Reload the currently active bank through the asynchronous bundle path.
    // The command must ACK before parsing completes, audio must remain live,
    // and QuerySoundFontLoad must observe block-boundary activation.
    if (t.soundFontLoaded != 0u && t.soundFontName[0] != '\0') {
        const svms::RLResult reloadResult = client.SendCommand(
            svms::RLCommandType::ReloadSoundFont, 0u, 0u,
            svms::RuntimeLiveStateV2{}, 2000u, resultText,
            t.soundFontName);
        if (reloadResult != svms::RLResult::Ok) {
            std::printf("FAIL: ReloadSoundFont returned %d (%s)\n",
                        static_cast<int>(reloadResult), resultText);
            reset(handle); close(handle); FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
        const DWORD reloadStart = GetTickCount();
        bool activated = false;
        do {
            uint32_t state = 0u;
            unsigned long long requested = 0u;
            unsigned long long active = 0u;
            resultText[0] = '\0';
            if (client.SendCommand(
                    svms::RLCommandType::QuerySoundFontLoad, 0u, 0u,
                    svms::RuntimeLiveStateV2{}, 1000u, resultText) ==
                    svms::RLResult::Ok &&
                std::sscanf(resultText, "%u\t%llu\t%llu", &state,
                            &requested, &active) == 3 &&
                state == 3u && requested != 0u && active == requested) {
                activated = true;
                break;
            }
            Sleep(25u);
        } while (static_cast<int>(GetTickCount() - reloadStart) < 15000);
        if (!activated || !client.ReadTelemetry(t) || t.audioRunning == 0u ||
            t.soundFontLoaded == 0u) {
            std::printf("FAIL: live SoundFont activation did not settle (%s)\n",
                        resultText);
            reset(handle); close(handle); FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
        shortMsg(handle, 0x00643C90u);
        Sleep(75u);
        shortMsg(handle, 0x00003C80u);
        std::puts("INFO: live SoundFont bundle activated without restarting audio");
    }

    // Exercise UTF-8 request text and the real post-DSP recorder through the
    // driver. Stop must drain its ring and leave a finalized RIFF file.
    wchar_t recordingPath[MAX_PATH]{};
    swprintf_s(recordingPath, L"%sSVMS_live_\x0442\x0435\x0441\x0442_%lu.wav",
               tempDirectory, static_cast<unsigned long>(ownPid));
    const std::string recordingUtf8 = WideToUtf8(recordingPath);
    {
        const svms::RLResult startResult = client.SendCommand(
            svms::RLCommandType::StartLiveRecording, 0u, 0u,
            svms::RuntimeLiveStateV2{}, 2000u, resultText,
            recordingUtf8.c_str());
        if (startResult != svms::RLResult::Ok) {
            std::printf("FAIL: StartLiveRecording returned %d (%s)\n",
                        static_cast<int>(startResult), resultText);
            reset(handle); close(handle); FreeLibrary(module);
            DeleteFileW(configPath); DeleteFileW(recordingPath);
            return 1;
        }
        shortMsg(handle, 0x00643C90u);
        Sleep(150u);
        shortMsg(handle, 0x00003C80u);
        Sleep(75u);
        const svms::RLResult stopResult = client.SendCommand(
            svms::RLCommandType::StopLiveRecording, 0u, 0u,
            svms::RuntimeLiveStateV2{}, 3000u, resultText);
        if (stopResult != svms::RLResult::Ok) {
            std::printf("FAIL: StopLiveRecording returned %d (%s)\n",
                        static_cast<int>(stopResult), resultText);
            reset(handle); close(handle); FreeLibrary(module);
            DeleteFileW(configPath); DeleteFileW(recordingPath);
            return 1;
        }
        uint32_t recordState = 99u;
        uint32_t recordRate = 0u;
        unsigned long long recordFrames = 0u;
        unsigned long long recordDrops = 0u;
        uint32_t recordError = 99u;
        const svms::RLResult queryResult = client.SendCommand(
            svms::RLCommandType::QueryLiveRecording, 0u, 0u,
            svms::RuntimeLiveStateV2{}, 2000u, resultText);
        if (queryResult != svms::RLResult::Ok ||
            std::sscanf(resultText, "%u\t%u\t%llu\t%llu\t%u",
                        &recordState, &recordRate, &recordFrames,
                        &recordDrops, &recordError) != 5 ||
            recordState != 0u || recordRate != t.sampleRate ||
            recordFrames == 0u || recordDrops != 0u || recordError != 0u) {
            std::printf("FAIL: recording status malformed (%s)\n", resultText);
            reset(handle); close(handle); FreeLibrary(module);
            DeleteFileW(configPath); DeleteFileW(recordingPath);
            return 1;
        }
        FILE* recording = _wfopen(recordingPath, L"rb");
        char riff[4]{};
        const bool validFile = recording &&
            std::fread(riff, 1u, sizeof(riff), recording) == sizeof(riff) &&
            std::memcmp(riff, "RIFF", sizeof(riff)) == 0;
        if (recording) std::fclose(recording);
        if (!validFile) {
            std::puts("FAIL: live recording WAV was not finalized");
            reset(handle); close(handle); FreeLibrary(module);
            DeleteFileW(configPath); DeleteFileW(recordingPath);
            return 1;
        }
    }
    DeleteFileW(recordingPath);

    // ── 6. ResetVoices (routes through the SPSC ingress) ─────────────
    {
        const svms::RLResult r = client.SendCommand(
            svms::RLCommandType::ResetVoices, 0u, 0u,
            svms::RuntimeLiveStateV2{}, 2000, resultText);
        if (r != svms::RLResult::Ok) {
            std::printf("FAIL: ResetVoices returned %d (%s)\n",
                        (int)r, resultText);
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
    }

    // ── 7. Post-ACK sanity: commands still work, telemetry flows ────
    {
        const svms::RLResult r = client.SendPing(2000, resultText);
        if (r != svms::RLResult::Ok) {
            std::printf("FAIL: Ping after ResetVoices returned %d (%s)\n",
                        (int)r, resultText);
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
        if (!client.IsHostAlive(3000)) {
            std::puts("FAIL: driver heartbeat is stale after commands");
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
        const uint64_t qpcBefore = t.timestampQpc;
        bool advanced = false;
        for (int i = 0; i < 40; ++i) {
            Sleep(50);
            if (client.ReadTelemetry(t) && t.timestampQpc != qpcBefore) {
                advanced = true;
                break;
            }
        }
        if (!advanced) {
            std::puts("FAIL: telemetry stopped advancing after commands");
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
    }

    client.Close();
    reset(handle);
    if (close(handle) != MMSYSERR_NOERROR) {
        std::puts("FAIL: midiOutClose failed");
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    FreeLibrary(module);
    DeleteFileW(configPath);

    std::puts("PASS: RuntimeLink V2 driver smoke (hosts, telemetry, ping, "
              "live apply, reset, heartbeat)");
    return 0;
}
