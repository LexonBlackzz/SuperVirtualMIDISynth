#ifndef SYSTEMWINMM_H
#define SYSTEMWINMM_H

#include <windows.h>
#include <string>

// Define function pointers for the functions we are proxying
typedef MMRESULT (WINAPI *Func_midiInGetNumDevs)(void);
typedef MMRESULT (WINAPI *Func_midiInGetDevCapsA)(UINT_PTR, LPMIDIINCAPSA, UINT);
typedef MMRESULT (WINAPI *Func_midiInGetDevCapsW)(UINT_PTR, LPMIDIINCAPSW, UINT);
typedef MMRESULT (WINAPI *Func_midiInOpen)(LPHMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *Func_midiInClose)(HMIDIIN);
typedef MMRESULT (WINAPI *Func_midiInPrepareHeader)(HMIDIIN, LPMIDIHDR, UINT);
typedef MMRESULT (WINAPI *Func_midiInUnprepareHeader)(HMIDIIN, LPMIDIHDR, UINT);
typedef MMRESULT (WINAPI *Func_midiInAddBuffer)(HMIDIIN, LPMIDIHDR, UINT);
typedef MMRESULT (WINAPI *Func_midiInStart)(HMIDIIN);
typedef MMRESULT (WINAPI *Func_midiInStop)(HMIDIIN);
typedef MMRESULT (WINAPI *Func_midiInReset)(HMIDIIN);
typedef UINT (WINAPI *Func_waveInGetNumDevs)(void);
typedef MMRESULT (WINAPI *Func_waveInGetDevCapsA)(UINT_PTR, LPWAVEINCAPSA, UINT);
typedef MMRESULT (WINAPI *Func_waveInGetDevCapsW)(UINT_PTR, LPWAVEINCAPSW, UINT);
typedef MMRESULT (WINAPI *Func_waveInOpen)(LPHWAVEIN, UINT, LPCWAVEFORMATEX,
                                           DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *Func_waveInClose)(HWAVEIN);
typedef MMRESULT (WINAPI *Func_waveInPrepareHeader)(HWAVEIN, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *Func_waveInUnprepareHeader)(HWAVEIN, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *Func_waveInAddBuffer)(HWAVEIN, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *Func_waveInStart)(HWAVEIN);
typedef MMRESULT (WINAPI *Func_waveInStop)(HWAVEIN);
typedef MMRESULT (WINAPI *Func_waveInReset)(HWAVEIN);
typedef MMRESULT (WINAPI *Func_waveInMessage)(HWAVEIN, UINT, DWORD_PTR,
                                              DWORD_PTR);
typedef MMRESULT (WINAPI *Func_waveInGetPosition)(HWAVEIN, LPMMTIME, UINT);
typedef UINT (WINAPI *Func_mixerGetNumDevs)(void);
typedef MMRESULT (WINAPI *Func_mixerGetDevCapsA)(UINT_PTR, LPMIXERCAPSA, UINT);
typedef MMRESULT (WINAPI *Func_mixerGetDevCapsW)(UINT_PTR, LPMIXERCAPSW, UINT);
typedef MMRESULT (WINAPI *Func_mixerOpen)(LPHMIXER, UINT, DWORD_PTR, DWORD_PTR,
                                          DWORD);
typedef MMRESULT (WINAPI *Func_mixerClose)(HMIXER);
typedef DWORD (WINAPI *Func_mixerMessage)(HMIXER, UINT, DWORD_PTR, DWORD_PTR);
typedef MMRESULT (WINAPI *Func_mixerGetLineInfoA)(HMIXEROBJ, LPMIXERLINEA,
                                                  DWORD);
typedef MMRESULT (WINAPI *Func_mixerGetLineInfoW)(HMIXEROBJ, LPMIXERLINEW,
                                                  DWORD);
typedef MMRESULT (WINAPI *Func_mixerGetID)(HMIXEROBJ, UINT *, DWORD);
typedef MMRESULT (WINAPI *Func_mixerGetLineControlsA)(HMIXEROBJ,
                                                      LPMIXERLINECONTROLSA,
                                                      DWORD);
typedef MMRESULT (WINAPI *Func_mixerGetLineControlsW)(HMIXEROBJ,
                                                      LPMIXERLINECONTROLSW,
                                                      DWORD);
typedef MMRESULT (WINAPI *Func_mixerGetControlDetailsA)(
    HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
typedef MMRESULT (WINAPI *Func_mixerGetControlDetailsW)(
    HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
typedef MMRESULT (WINAPI *Func_mixerSetControlDetails)(
    HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);

typedef MMRESULT (WINAPI *Func_midiOutSetVolume)(HMIDIOUT, DWORD);
typedef MMRESULT (WINAPI *Func_midiOutGetVolume)(HMIDIOUT, LPDWORD);

typedef MMRESULT (WINAPI *Func_waveOutGetNumDevs)(void);
typedef MMRESULT (WINAPI *Func_waveOutGetDevCapsA)(UINT_PTR, LPWAVEOUTCAPSA, UINT);
typedef MMRESULT (WINAPI *Func_waveOutGetDevCapsW)(UINT_PTR, LPWAVEOUTCAPSW, UINT);
typedef MMRESULT (WINAPI *Func_waveOutOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *Func_waveOutClose)(HWAVEOUT);
typedef MMRESULT (WINAPI *Func_waveOutPrepareHeader)(HWAVEOUT, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *Func_waveOutUnprepareHeader)(HWAVEOUT, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *Func_waveOutWrite)(HWAVEOUT, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *Func_waveOutReset)(HWAVEOUT);
typedef MMRESULT (WINAPI *Func_waveOutRestart)(HWAVEOUT);
typedef MMRESULT (WINAPI *Func_waveOutPause)(HWAVEOUT);
typedef MMRESULT (WINAPI *Func_waveOutBreakLoop)(HWAVEOUT);
typedef MMRESULT (WINAPI *Func_waveOutGetPosition)(HWAVEOUT, LPMMTIME, UINT);
typedef MMRESULT (WINAPI *Func_waveOutGetVolume)(HWAVEOUT, LPDWORD);
typedef MMRESULT (WINAPI *Func_waveOutSetVolume)(HWAVEOUT, DWORD);
typedef MMRESULT (WINAPI *Func_waveOutGetPitch)(HWAVEOUT, LPDWORD);
typedef MMRESULT (WINAPI *Func_waveOutSetPitch)(HWAVEOUT, DWORD);
typedef MMRESULT (WINAPI *Func_waveOutGetPlaybackRate)(HWAVEOUT, LPDWORD);
typedef MMRESULT (WINAPI *Func_waveOutSetPlaybackRate)(HWAVEOUT, DWORD);
typedef MMRESULT (WINAPI *Func_waveOutGetID)(HWAVEOUT, LPUINT);
typedef MMRESULT (WINAPI *Func_waveOutGetErrorTextA)(MMRESULT, LPSTR, UINT);
typedef MMRESULT (WINAPI *Func_waveOutGetErrorTextW)(MMRESULT, LPWSTR, UINT);
typedef MMRESULT (WINAPI *Func_waveOutMessage)(HWAVEOUT, UINT, DWORD_PTR,
                                               DWORD_PTR);

typedef DWORD (WINAPI *Func_timeGetTime)(void);
typedef MMRESULT (WINAPI *Func_timeBeginPeriod)(UINT);
typedef MMRESULT (WINAPI *Func_timeEndPeriod)(UINT);
typedef MMRESULT (WINAPI *Func_timeGetDevCaps)(LPTIMECAPS, UINT);
typedef MMRESULT (WINAPI *Func_timeSetEvent)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
typedef MMRESULT (WINAPI *Func_timeKillEvent)(UINT);

class SystemWinmm {
public:
    static SystemWinmm& Instance();

    void Initialize();
    
    // MIDI In Pointers
    Func_midiInGetNumDevs Real_midiInGetNumDevs = nullptr;
    Func_midiInGetDevCapsA Real_midiInGetDevCapsA = nullptr;
    Func_midiInGetDevCapsW Real_midiInGetDevCapsW = nullptr;
    Func_midiInOpen Real_midiInOpen = nullptr;
    Func_midiInClose Real_midiInClose = nullptr;
    Func_midiInPrepareHeader Real_midiInPrepareHeader = nullptr;
    Func_midiInUnprepareHeader Real_midiInUnprepareHeader = nullptr;
    Func_midiInAddBuffer Real_midiInAddBuffer = nullptr;
    Func_midiInStart Real_midiInStart = nullptr;
    Func_midiInStop Real_midiInStop = nullptr;
    Func_midiInReset Real_midiInReset = nullptr;
    Func_waveInGetNumDevs Real_waveInGetNumDevs = nullptr;
    Func_waveInGetDevCapsA Real_waveInGetDevCapsA = nullptr;
    Func_waveInGetDevCapsW Real_waveInGetDevCapsW = nullptr;
    Func_waveInOpen Real_waveInOpen = nullptr;
    Func_waveInClose Real_waveInClose = nullptr;
    Func_waveInPrepareHeader Real_waveInPrepareHeader = nullptr;
    Func_waveInUnprepareHeader Real_waveInUnprepareHeader = nullptr;
    Func_waveInAddBuffer Real_waveInAddBuffer = nullptr;
    Func_waveInStart Real_waveInStart = nullptr;
    Func_waveInStop Real_waveInStop = nullptr;
    Func_waveInReset Real_waveInReset = nullptr;
    Func_waveInMessage Real_waveInMessage = nullptr;
    Func_waveInGetPosition Real_waveInGetPosition = nullptr;
    Func_mixerGetNumDevs Real_mixerGetNumDevs = nullptr;
    Func_mixerGetDevCapsA Real_mixerGetDevCapsA = nullptr;
    Func_mixerGetDevCapsW Real_mixerGetDevCapsW = nullptr;
    Func_mixerOpen Real_mixerOpen = nullptr;
    Func_mixerClose Real_mixerClose = nullptr;
    Func_mixerMessage Real_mixerMessage = nullptr;
    Func_mixerGetLineInfoA Real_mixerGetLineInfoA = nullptr;
    Func_mixerGetLineInfoW Real_mixerGetLineInfoW = nullptr;
    Func_mixerGetID Real_mixerGetID = nullptr;
    Func_mixerGetLineControlsA Real_mixerGetLineControlsA = nullptr;
    Func_mixerGetLineControlsW Real_mixerGetLineControlsW = nullptr;
    Func_mixerGetControlDetailsA Real_mixerGetControlDetailsA = nullptr;
    Func_mixerGetControlDetailsW Real_mixerGetControlDetailsW = nullptr;
    Func_mixerSetControlDetails Real_mixerSetControlDetails = nullptr;

    // MIDI Out Pointers
    Func_midiOutSetVolume Real_midiOutSetVolume = nullptr;
    Func_midiOutGetVolume Real_midiOutGetVolume = nullptr;

    // Wave Out Pointers
    Func_waveOutGetNumDevs Real_waveOutGetNumDevs = nullptr;
    Func_waveOutGetDevCapsA Real_waveOutGetDevCapsA = nullptr;
    Func_waveOutGetDevCapsW Real_waveOutGetDevCapsW = nullptr;
    Func_waveOutOpen Real_waveOutOpen = nullptr;
    Func_waveOutClose Real_waveOutClose = nullptr;
    Func_waveOutPrepareHeader Real_waveOutPrepareHeader = nullptr;
    Func_waveOutUnprepareHeader Real_waveOutUnprepareHeader = nullptr;
    Func_waveOutWrite Real_waveOutWrite = nullptr;
    Func_waveOutReset Real_waveOutReset = nullptr;
    Func_waveOutRestart Real_waveOutRestart = nullptr;
    Func_waveOutPause Real_waveOutPause = nullptr;
    Func_waveOutBreakLoop Real_waveOutBreakLoop = nullptr;
    Func_waveOutGetPosition Real_waveOutGetPosition = nullptr;
    Func_waveOutGetVolume Real_waveOutGetVolume = nullptr;
    Func_waveOutSetVolume Real_waveOutSetVolume = nullptr;
    Func_waveOutGetPitch Real_waveOutGetPitch = nullptr;
    Func_waveOutSetPitch Real_waveOutSetPitch = nullptr;
    Func_waveOutGetPlaybackRate Real_waveOutGetPlaybackRate = nullptr;
    Func_waveOutSetPlaybackRate Real_waveOutSetPlaybackRate = nullptr;
    Func_waveOutGetID Real_waveOutGetID = nullptr;
    Func_waveOutGetErrorTextA Real_waveOutGetErrorTextA = nullptr;
    Func_waveOutGetErrorTextW Real_waveOutGetErrorTextW = nullptr;
    Func_waveOutMessage Real_waveOutMessage = nullptr;

    // Time
    Func_timeGetTime Real_timeGetTime = nullptr;
    Func_timeBeginPeriod Real_timeBeginPeriod = nullptr;
    Func_timeEndPeriod Real_timeEndPeriod = nullptr;
    Func_timeGetDevCaps Real_timeGetDevCaps = nullptr;
    Func_timeSetEvent Real_timeSetEvent = nullptr;
    Func_timeKillEvent Real_timeKillEvent = nullptr;

private:
    SystemWinmm() {}
    HMODULE hSystemWinmm = NULL;
};

#endif
