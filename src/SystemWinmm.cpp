#include "SystemWinmm.h"
#include <stdio.h>

SystemWinmm& SystemWinmm::Instance() {
    static SystemWinmm instance;
    return instance;
}

void SystemWinmm::Initialize() {
    if (hSystemWinmm) return;
    
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    std::string dllPath = std::string(path) + "\\winmm.dll";
    hSystemWinmm = LoadLibraryA(dllPath.c_str());

    if (!hSystemWinmm) {
        fprintf(stderr, "Failed to load system winmm.dll from %s\n", dllPath.c_str());
        return;
    }

    // MIDI In
    Real_midiInGetNumDevs = (Func_midiInGetNumDevs)GetProcAddress(hSystemWinmm, "midiInGetNumDevs");
    Real_midiInGetDevCapsA = (Func_midiInGetDevCapsA)GetProcAddress(hSystemWinmm, "midiInGetDevCapsA");
    Real_midiInGetDevCapsW = (Func_midiInGetDevCapsW)GetProcAddress(hSystemWinmm, "midiInGetDevCapsW");
    Real_midiInOpen = (Func_midiInOpen)GetProcAddress(hSystemWinmm, "midiInOpen");
    Real_midiInClose = (Func_midiInClose)GetProcAddress(hSystemWinmm, "midiInClose");
    Real_midiInPrepareHeader = (Func_midiInPrepareHeader)GetProcAddress(hSystemWinmm, "midiInPrepareHeader");
    Real_midiInUnprepareHeader = (Func_midiInUnprepareHeader)GetProcAddress(hSystemWinmm, "midiInUnprepareHeader");
    Real_midiInAddBuffer = (Func_midiInAddBuffer)GetProcAddress(hSystemWinmm, "midiInAddBuffer");
    Real_midiInStart = (Func_midiInStart)GetProcAddress(hSystemWinmm, "midiInStart");
    Real_midiInStop = (Func_midiInStop)GetProcAddress(hSystemWinmm, "midiInStop");
    Real_midiInReset = (Func_midiInReset)GetProcAddress(hSystemWinmm, "midiInReset");
    Real_midiInMessage = (Func_midiInMessage)GetProcAddress(hSystemWinmm, "midiInMessage");
    Real_waveInGetNumDevs = (Func_waveInGetNumDevs)GetProcAddress(hSystemWinmm, "waveInGetNumDevs");
    Real_waveInGetDevCapsA = (Func_waveInGetDevCapsA)GetProcAddress(hSystemWinmm, "waveInGetDevCapsA");
    Real_waveInGetDevCapsW = (Func_waveInGetDevCapsW)GetProcAddress(hSystemWinmm, "waveInGetDevCapsW");
    Real_waveInOpen = (Func_waveInOpen)GetProcAddress(hSystemWinmm, "waveInOpen");
    Real_waveInClose = (Func_waveInClose)GetProcAddress(hSystemWinmm, "waveInClose");
    Real_waveInPrepareHeader = (Func_waveInPrepareHeader)GetProcAddress(hSystemWinmm, "waveInPrepareHeader");
    Real_waveInUnprepareHeader = (Func_waveInUnprepareHeader)GetProcAddress(hSystemWinmm, "waveInUnprepareHeader");
    Real_waveInAddBuffer = (Func_waveInAddBuffer)GetProcAddress(hSystemWinmm, "waveInAddBuffer");
    Real_waveInStart = (Func_waveInStart)GetProcAddress(hSystemWinmm, "waveInStart");
    Real_waveInStop = (Func_waveInStop)GetProcAddress(hSystemWinmm, "waveInStop");
    Real_waveInReset = (Func_waveInReset)GetProcAddress(hSystemWinmm, "waveInReset");
    Real_waveInMessage = (Func_waveInMessage)GetProcAddress(hSystemWinmm, "waveInMessage");
    Real_waveInGetPosition = (Func_waveInGetPosition)GetProcAddress(hSystemWinmm, "waveInGetPosition");
    Real_mixerGetNumDevs = (Func_mixerGetNumDevs)GetProcAddress(hSystemWinmm, "mixerGetNumDevs");
    Real_mixerGetDevCapsA = (Func_mixerGetDevCapsA)GetProcAddress(hSystemWinmm, "mixerGetDevCapsA");
    Real_mixerGetDevCapsW = (Func_mixerGetDevCapsW)GetProcAddress(hSystemWinmm, "mixerGetDevCapsW");
    Real_mixerOpen = (Func_mixerOpen)GetProcAddress(hSystemWinmm, "mixerOpen");
    Real_mixerClose = (Func_mixerClose)GetProcAddress(hSystemWinmm, "mixerClose");
    Real_mixerMessage = (Func_mixerMessage)GetProcAddress(hSystemWinmm, "mixerMessage");
    Real_mixerGetLineInfoA = (Func_mixerGetLineInfoA)GetProcAddress(hSystemWinmm, "mixerGetLineInfoA");
    Real_mixerGetLineInfoW = (Func_mixerGetLineInfoW)GetProcAddress(hSystemWinmm, "mixerGetLineInfoW");
    Real_mixerGetID = (Func_mixerGetID)GetProcAddress(hSystemWinmm, "mixerGetID");
    Real_mixerGetLineControlsA = (Func_mixerGetLineControlsA)GetProcAddress(hSystemWinmm, "mixerGetLineControlsA");
    Real_mixerGetLineControlsW = (Func_mixerGetLineControlsW)GetProcAddress(hSystemWinmm, "mixerGetLineControlsW");
    Real_mixerGetControlDetailsA = (Func_mixerGetControlDetailsA)GetProcAddress(hSystemWinmm, "mixerGetControlDetailsA");
    Real_mixerGetControlDetailsW = (Func_mixerGetControlDetailsW)GetProcAddress(hSystemWinmm, "mixerGetControlDetailsW");
    Real_mixerSetControlDetails = (Func_mixerSetControlDetails)GetProcAddress(hSystemWinmm, "mixerSetControlDetails");

    // MIDI Out
    Real_midiOutSetVolume = (Func_midiOutSetVolume)GetProcAddress(hSystemWinmm, "midiOutSetVolume");
    Real_midiOutGetVolume = (Func_midiOutGetVolume)GetProcAddress(hSystemWinmm, "midiOutGetVolume");
    Real_midiOutMessage = (Func_midiOutMessage)GetProcAddress(hSystemWinmm, "midiOutMessage");

    // Wave Out
    Real_waveOutGetNumDevs = (Func_waveOutGetNumDevs)GetProcAddress(hSystemWinmm, "waveOutGetNumDevs");
    Real_waveOutGetDevCapsA = (Func_waveOutGetDevCapsA)GetProcAddress(hSystemWinmm, "waveOutGetDevCapsA");
    Real_waveOutGetDevCapsW = (Func_waveOutGetDevCapsW)GetProcAddress(hSystemWinmm, "waveOutGetDevCapsW");
    Real_waveOutOpen = (Func_waveOutOpen)GetProcAddress(hSystemWinmm, "waveOutOpen");
    Real_waveOutClose = (Func_waveOutClose)GetProcAddress(hSystemWinmm, "waveOutClose");
    Real_waveOutPrepareHeader = (Func_waveOutPrepareHeader)GetProcAddress(hSystemWinmm, "waveOutPrepareHeader");
    Real_waveOutUnprepareHeader = (Func_waveOutUnprepareHeader)GetProcAddress(hSystemWinmm, "waveOutUnprepareHeader");
    Real_waveOutWrite = (Func_waveOutWrite)GetProcAddress(hSystemWinmm, "waveOutWrite");
    Real_waveOutReset = (Func_waveOutReset)GetProcAddress(hSystemWinmm, "waveOutReset");
    Real_waveOutRestart = (Func_waveOutRestart)GetProcAddress(hSystemWinmm, "waveOutRestart");
    Real_waveOutPause = (Func_waveOutPause)GetProcAddress(hSystemWinmm, "waveOutPause");
    Real_waveOutBreakLoop = (Func_waveOutBreakLoop)GetProcAddress(hSystemWinmm, "waveOutBreakLoop");
    Real_waveOutGetPosition = (Func_waveOutGetPosition)GetProcAddress(hSystemWinmm, "waveOutGetPosition");
    Real_waveOutGetVolume = (Func_waveOutGetVolume)GetProcAddress(hSystemWinmm, "waveOutGetVolume");
    Real_waveOutSetVolume = (Func_waveOutSetVolume)GetProcAddress(hSystemWinmm, "waveOutSetVolume");
    Real_waveOutGetPitch = (Func_waveOutGetPitch)GetProcAddress(hSystemWinmm, "waveOutGetPitch");
    Real_waveOutSetPitch = (Func_waveOutSetPitch)GetProcAddress(hSystemWinmm, "waveOutSetPitch");
    Real_waveOutGetPlaybackRate = (Func_waveOutGetPlaybackRate)GetProcAddress(hSystemWinmm, "waveOutGetPlaybackRate");
    Real_waveOutSetPlaybackRate = (Func_waveOutSetPlaybackRate)GetProcAddress(hSystemWinmm, "waveOutSetPlaybackRate");
    Real_waveOutGetID = (Func_waveOutGetID)GetProcAddress(hSystemWinmm, "waveOutGetID");
    Real_waveOutGetErrorTextA = (Func_waveOutGetErrorTextA)GetProcAddress(hSystemWinmm, "waveOutGetErrorTextA");
    Real_waveOutGetErrorTextW = (Func_waveOutGetErrorTextW)GetProcAddress(hSystemWinmm, "waveOutGetErrorTextW");
    Real_waveOutMessage = (Func_waveOutMessage)GetProcAddress(hSystemWinmm, "waveOutMessage");

    // Time
    Real_timeGetTime = (Func_timeGetTime)GetProcAddress(hSystemWinmm, "timeGetTime");
    Real_timeBeginPeriod = (Func_timeBeginPeriod)GetProcAddress(hSystemWinmm, "timeBeginPeriod");
    Real_timeEndPeriod = (Func_timeEndPeriod)GetProcAddress(hSystemWinmm, "timeEndPeriod");
    Real_timeGetDevCaps = (Func_timeGetDevCaps)GetProcAddress(hSystemWinmm, "timeGetDevCaps");
    Real_timeSetEvent = (Func_timeSetEvent)GetProcAddress(hSystemWinmm, "timeSetEvent");
    Real_timeKillEvent = (Func_timeKillEvent)GetProcAddress(hSystemWinmm, "timeKillEvent");
}
