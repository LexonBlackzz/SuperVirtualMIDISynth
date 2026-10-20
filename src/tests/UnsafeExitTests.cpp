// Unsafe-exit robustness tests for the V3 winmm.dll driver.
//
// A parent process re-spawns itself as children, each exercising a
// different hostile shutdown path while the synth is actively rendering:
//   clean      — reset+close, then normal exit (the polite path)
//   noclose    — normal exit WITHOUT closing the MIDI device (device and
//                audio thread still live at ExitProcess/DLL detach)
//   concurrent — another thread calls ExitProcess mid-event-flood
//   freelib    — FreeLibraryAndExitThread while the audio thread renders
//
// The parent classifies child exit codes: 0 = pass; STATUS_ACCESS_VIOLATION
// (0xC0000005), fastfail (0xC0000409) or stack overflow (0xC00000FD) fail,
// with a pointer at %TEMP%\SVMSV3Crash.log for the symbolized stack.
#include <windows.h>
#include <mmeapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void FloodMidi(int milliseconds) {
    const DWORD start = GetTickCount();
    DWORD tick = 0;
    unsigned rng = 0x1234567u;
    while (static_cast<int>(GetTickCount() - start) < milliseconds) {
        for (int i = 0; i < 64; ++i) {
            rng = rng * 1664525u + 1013904223u;
            const int ch = (rng >> 16) & 15;
            const int key = 36 + ((rng >> 8) % 60);
            if ((rng & 1) == 0)
                midiOutShortMsg(nullptr, 0x90 | ch | (key << 8) | (80u << 16));
            else
                midiOutShortMsg(nullptr, 0x80 | ch | (key << 8));
            // Occasional CC sweeps (mix-gain path) and CC120 resets.
            if ((rng & 7) == 0)
                midiOutShortMsg(nullptr,
                    0xB0 | ch | (((rng >> 20) % 120) << 8) | (((rng >> 10) & 127) << 16));
            if ((rng & 31) == 0)
                midiOutShortMsg(nullptr, (0xB0 | ch) | (120 << 8));
        }
        Sleep(2);
        if (++tick % 64 == 0) midiOutReset(nullptr);
    }
}

int ChildMain(const char* mode) {
    HMIDIOUT handle = nullptr;
    const MMRESULT openResult = midiOutOpen(&handle, 0, 0, 0, CALLBACK_NULL);
    if (openResult != MMSYSERR_NOERROR) {
        std::printf("child: midiOutOpen failed (MMRESULT %u)\n",
                    static_cast<unsigned>(openResult));
        return 77;  // no device / no config — treat as skip
    }
    if (strcmp(mode, "clean") == 0) {
        FloodMidi(1500);
        midiOutReset(handle);
        midiOutClose(handle);
        return 0;
    }
    if (strcmp(mode, "noclose") == 0) {
        FloodMidi(1500);
        return 0;   // device open, audio thread running at CRT exit
    }
    if (strcmp(mode, "concurrent") == 0) {
        HANDLE killer = CreateThread(nullptr, 0,
            [](LPVOID) -> DWORD { Sleep(700); ExitProcess(0); return 0; },
            nullptr, 0, nullptr);
        (void)killer;
        FloodMidi(1500);    // ExitProcess lands mid-flood
        return 0;
    }
    if (strcmp(mode, "freelib") == 0) {
        FloodMidi(1500);
        // Host-style "unload the DLL mid-render": with the self-pin, the
        // FreeLibrary must return without unloading and ExitProcess reclaims.
        FreeLibrary(GetModuleHandleW(L"winmm.dll"));
        ExitProcess(0);
        return 0;
    }
    if (strcmp(mode, "freelibthread") == 0) {
        // FreeLibraryAndExitThread kills the calling thread; a healthy host
        // exits via another thread. The DLL must not deadlock or corrupt.
        HANDLE killer = CreateThread(nullptr, 0,
            [](LPVOID) -> DWORD { Sleep(3000); ExitProcess(0); return 0; },
            nullptr, 0, nullptr);
        (void)killer;
        FloodMidi(1500);
        FreeLibraryAndExitThread(GetModuleHandleW(L"winmm.dll"), 0);
        return 0;
    }
    midiOutClose(handle);
    return 2;   // unknown mode
}

const DWORD kFatalCodes[] = {
    0xC0000005u, 0xC0000409u, 0xC00000FDu, 0xC000001Du, 0xC0000094u,
};

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "--child") == 0)
        return ChildMain(argv[2]);

    // Isolate from the user's real V3 config, like the two-process smoke.
    wchar_t tempDir[MAX_PATH] = {};
    wchar_t configPath[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempDir) ||
        !GetTempFileNameW(tempDir, L"svm", 0, configPath)) {
        std::puts("FAIL: cannot reserve isolated config path");
        return 1;
    }
    DeleteFileW(configPath);
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", configPath);
    SetEnvironmentVariableW(L"SVMS_TEST_LOCAL_CONFIG_PATH", configPath);
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS", L"0");
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS_WINDOW", L"0");
    SetEnvironmentVariableW(L"SVMS_DEBUG_OUTPUT", L"0");

    struct Mode { const char* name; };
    const Mode modes[] = {{"clean"}, {"noclose"}, {"concurrent"}, {"freelib"},
                          {"freelibthread"}};
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t cmd[512];

    int failures = 0;
    for (const Mode& m : modes) {
        wchar_t modeW[32];
        MultiByteToWideChar(CP_UTF8, 0, m.name, -1, modeW, 32);
        wsprintfW(cmd, L"\"%s\" --child %s", exePath, modeW);
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &si, &pi)) {
            std::printf("FAIL: %s: cannot spawn child (error %lu)\n",
                        m.name, GetLastError());
            ++failures;
            continue;
        }
        const DWORD wait = WaitForSingleObject(pi.hProcess, 45000);
        DWORD exitCode = 0;
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 3);
            std::printf("FAIL: %s: timed out (45 s), terminated\n", m.name);
            ++failures;
        } else {
            GetExitCodeProcess(pi.hProcess, &exitCode);
            bool fatal = false;
            for (const DWORD c : kFatalCodes)
                if (exitCode == c) { fatal = true; break; }
            if (fatal) {
                std::printf("FAIL: %s: child died with fatal code 0x%08lX "
                            "(see %%TEMP%%\\SVMSV3Crash.log for the stack)\n",
                            m.name, exitCode);
                ++failures;
            } else if (exitCode == 77) {
                std::printf("SKIP: %s: no MIDI device/config available\n", m.name);
            } else if (exitCode == 2) {
                std::printf("FAIL: %s: unknown child mode\n", m.name);
                ++failures;
            } else {
                std::printf("PASS: %s (exit %lu)\n", m.name, exitCode);
            }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DeleteFileW(configPath);
    if (failures)
        std::printf("FAILED: %d hostile-exit modes crashed\n", failures);
    else
        std::puts("PASSED: all hostile-exit modes survived");
    return failures ? 1 : 0;
}
