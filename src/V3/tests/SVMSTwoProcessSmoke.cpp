// SVMSTwoProcessSmoke.cpp — REAL two-process RuntimeLink V2 smoke test.
//
// Process 1 (this test): loads winmm.dll, opens MIDI_MAPPER, discovers
// the driver's host slot, then spawns Process 2 (svms_v3_runtime_link_
// probe) with --host-pid.  The probe connects from a DIFFERENT process,
// pings, applies a live reverb change and waits for the telemetry echo.
// After the probe exits, the parent re-connects and verifies the driver
// STILL echoes the applied values (driver-side state persisted across
// process boundaries).
//
// Skips (exit 77) when the machine has no MIDI/audio device.
//
#include <windows.h>
#include <mmsystem.h>

#include "../SVMSRuntimeLink.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

typedef UINT (WINAPI* MidiOutGetNumDevsProc)();
typedef MMRESULT (WINAPI* MidiOutOpenProc)(LPHMIDIOUT, UINT, DWORD_PTR,
                                           DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI* MidiOutResetProc)(HMIDIOUT);
typedef MMRESULT (WINAPI* MidiOutCloseProc)(HMIDIOUT);

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
    if (argc < 3) {
        std::puts("FAIL: expected <probe exe> <winmm.dll path>");
        return 1;
    }
    const char* probeExe = argv[1];
    const char* dllPath = argv[2];

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

    HMODULE module = LoadLibraryA(dllPath);
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
    MidiOutResetProc reset = reinterpret_cast<MidiOutResetProc>(
        GetProcAddress(module, "midiOutReset"));
    MidiOutCloseProc close = reinterpret_cast<MidiOutCloseProc>(
        GetProcAddress(module, "midiOutClose"));
    if (!getNumDevs || !open || !reset || !close) {
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
        return result == MMSYSERR_NOMEM ? 77 : 1;
    }
    if (!handle) {
        std::puts("FAIL: MIDI_MAPPER open returned a null handle");
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    const DWORD ownPid = GetCurrentProcessId();

    // ── 1. Discover the driver's host slot ──────────────────────────
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

    // ── 2. Spawn the probe in a separate process ────────────────────
    wchar_t probePathW[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, probeExe, -1, probePathW, MAX_PATH);
    wchar_t pidArg[32] = {};
    swprintf_s(pidArg, L"--host-pid %lu", static_cast<unsigned long>(ownPid));

    wchar_t commandLine[MAX_PATH + 64] = {};
    swprintf_s(commandLine, L"\"%s\" %s", probePathW, pidArg);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(probePathW, commandLine, nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        std::printf("FAIL: cannot spawn probe process (error %lu)\n",
                    static_cast<unsigned long>(GetLastError()));
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    CloseHandle(pi.hThread);

    const DWORD waitResult = WaitForSingleObject(pi.hProcess, 40000);
    DWORD probeExit = 0;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &probeExit);
    } else {
        TerminateProcess(pi.hProcess, 2);
        CloseHandle(pi.hProcess);
        std::puts("FAIL: probe timed out (40 s)");
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    CloseHandle(pi.hProcess);
    if (probeExit != 0) {
        std::printf("FAIL: probe exited with code %lu\n",
                    static_cast<unsigned long>(probeExit));
        reset(handle);
        close(handle);
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    // ── 3. Parent re-connects: the driver must still echo the probe's
    //       applied values (persisted across process boundary) ───────
    {
        svms::RuntimeLinkClientV2 client;
        if (!client.Open(ownPid)) {
            std::puts("FAIL: parent cannot reconnect to the driver");
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
        svms::RuntimeLinkTelemetryV2 t{};
        if (!WaitForTelemetry(client, t, 15000)) {
            std::puts("FAIL: no telemetry after probe exit");
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
        if (t.live.reverbMix < 0.419f || t.live.reverbMix > 0.421f ||
            t.live.reverbRoomSize < 0.699f ||
            t.live.reverbRoomSize > 0.701f) {
            std::printf("FAIL: driver did not persist probe's live values "
                        "(mix=%.3f room=%.3f)\n",
                        t.live.reverbMix, t.live.reverbRoomSize);
            reset(handle);
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
        std::printf("INFO: driver persists probe values (mix=%.3f "
                    "room=%.3f)\n", t.live.reverbMix, t.live.reverbRoomSize);
    }

    reset(handle);
    if (close(handle) != MMSYSERR_NOERROR) {
        std::puts("FAIL: midiOutClose failed");
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }
    FreeLibrary(module);
    DeleteFileW(configPath);

    std::puts("PASS: two-process RuntimeLink smoke "
              "(host + probe ACK + persistence)");
    return 0;
}