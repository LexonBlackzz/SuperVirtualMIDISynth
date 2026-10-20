#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>

namespace {

typedef UINT (WINAPI* MidiOutGetNumDevsProc)();
typedef MMRESULT (WINAPI* MidiOutOpenProc)(LPHMIDIOUT, UINT, DWORD_PTR,
                                           DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI* MidiOutCloseProc)(HMIDIOUT);

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::puts("FAIL: expected the path to the XP winmm.dll");
        return 1;
    }
    const bool expectDiagnosticWindow =
        argc == 3 && std::strcmp(argv[2], "--diag-window") == 0;
    const bool forceWaveOut =
        argc == 3 && std::strcmp(argv[2], "--waveout") == 0;
    if (argc == 3 && !expectDiagnosticWindow && !forceWaveOut) {
        std::puts("FAIL: unknown smoke-test mode");
        return 1;
    }

    // Keep this integration test isolated from the user's real V3 config and
    // suppress its diagnostic window while still exercising DLL initialization.
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
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS",
                            expectDiagnosticWindow ? L"1" : L"0");
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS_WINDOW",
                            expectDiagnosticWindow ? L"1" : L"0");
    SetEnvironmentVariableW(L"SVMS_DEBUG_OUTPUT", L"0");
    SetEnvironmentVariableW(L"SVMS_XP_FORCE_WAVEOUT",
                            forceWaveOut ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SVMS_XP_FORCE_WINMM_COPY",
                            forceWaveOut ? L"1" : nullptr);

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
    MidiOutCloseProc close = reinterpret_cast<MidiOutCloseProc>(
        GetProcAddress(module, "midiOutClose"));
    if (!getNumDevs || !open || !close) {
        std::puts("FAIL: required MIDI exports are missing");
        FreeLibrary(module);
        return 1;
    }
    if (getNumDevs() != 1u) {
        std::puts("FAIL: XP DLL did not advertise one MIDI output");
        FreeLibrary(module);
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
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    Sleep(100);
    if (expectDiagnosticWindow) {
        HWND diagnosticWindow = nullptr;
        for (unsigned int attempt = 0; attempt < 40u && !diagnosticWindow;
             ++attempt) {
            diagnosticWindow = FindWindowW(nullptr, L"SVMS V3 Monitor");
            if (!diagnosticWindow) Sleep(25);
        }
        if (!diagnosticWindow) {
            std::puts("FAIL: XP diagnostic window did not appear");
            close(handle);
            FreeLibrary(module);
            DeleteFileW(configPath);
            return 1;
        }
    }
    if (close(handle) != MMSYSERR_NOERROR) {
        std::puts("FAIL: midiOutClose failed");
        FreeLibrary(module);
        DeleteFileW(configPath);
        return 1;
    }

    FreeLibrary(module);
    DeleteFileW(configPath);
    if (expectDiagnosticWindow)
        std::puts("PASS: XP DLL opened MIDI_MAPPER and displayed diagnostics");
    else if (forceWaveOut)
        std::puts("PASS: XP DLL opened MIDI_MAPPER through system waveOut");
    else
        std::puts("PASS: XP DLL loaded and accepted MIDI_MAPPER");
    return 0;
}
