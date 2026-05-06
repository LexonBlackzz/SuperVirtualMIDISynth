#include "AudioOutput.h"
#include "Compat.h"
#include "Config.h"
#include "LiveRuntime.h"
#include "Synth.h"
#include "SystemWinmm.h"
#include <stdio.h>
#include <windows.h>

#define KDMAPI_ONLYSTRUCTS
#include "OmniMIDI.h"

static const char *kAttributionLine =
    "SVMS: Copyright LexonBlackzz\n";
static const char *kStartupAttributionLine =
    "SVMS: SuperVirtualMIDISynth by LexonBlackzz\n";

static volatile LONG g_runtimeInitState = 0;

// Windows 7 compatibility: global function pointer for GetSystemTimePreciseAsFileTime
#if SVMS_NEED_GETSYSTEMTIMEPRECISE
PFN_GetSystemTimePreciseAsFileTime g_pfnGetSystemTimePreciseAsFileTime = NULL;
#endif

#ifdef SVMS_LEGACY_XP
static HMODULE GetSystemWinmmModule() {
  static HMODULE module = NULL;
  if (module)
    return module;

  char path[MAX_PATH];
  if (!GetSystemDirectoryA(path, MAX_PATH))
    return NULL;

  strcat(path, "\\winmm.dll");
  module = LoadLibraryA(path);
  return module;
}

static FARPROC GetSystemWinmmProc(const char *name) {
  HMODULE module = GetSystemWinmmModule();
  return module ? GetProcAddress(module, name) : NULL;
}

#define DEFINE_WINMM_PROXY_MMRESULT(name, params, args)                           \
  __declspec(dllexport) MMRESULT WINAPI name params {                             \
    typedef MMRESULT(WINAPI *Fn) params;                                          \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return MMSYSERR_NODRIVER;                                                     \
  }

#define DEFINE_WINMM_PROXY_LRESULT(name, params, args)                            \
  __declspec(dllexport) LRESULT WINAPI name params {                              \
    typedef LRESULT(WINAPI *Fn) params;                                           \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return 0;                                                                     \
  }

#define DEFINE_WINMM_PROXY_UINT(name, params, args)                               \
  __declspec(dllexport) UINT WINAPI name params {                                 \
    typedef UINT(WINAPI *Fn) params;                                              \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return 0;                                                                     \
  }

#define DEFINE_WINMM_PROXY_DWORD(name, params, args)                              \
  __declspec(dllexport) DWORD WINAPI name params {                                \
    typedef DWORD(WINAPI *Fn) params;                                             \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return 0;                                                                     \
  }

#define DEFINE_WINMM_PROXY_HMODULE(name, params, args)                            \
  __declspec(dllexport) HMODULE WINAPI name params {                              \
    typedef HMODULE(WINAPI *Fn) params;                                           \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return NULL;                                                                  \
  }

#define DEFINE_WINMM_PROXY_HANDLE(name, params, args)                             \
  __declspec(dllexport) decltype(::name) WINAPI name params {                     \
    typedef decltype(::name)(WINAPI *Fn) params;                                  \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return NULL;                                                                  \
  }

#define DEFINE_WINMM_PROXY_LONG(name, params, args)                               \
  __declspec(dllexport) LONG WINAPI name params {                                 \
    typedef LONG(WINAPI *Fn) params;                                              \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return 0;                                                                     \
  }

#define DEFINE_WINMM_PROXY_FOURCC(name, params, args)                             \
  __declspec(dllexport) FOURCC WINAPI name params {                               \
    typedef FOURCC(WINAPI *Fn) params;                                            \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return 0;                                                                     \
  }

#define DEFINE_WINMM_PROXY_RET(ret, name, params, args, fail_value)               \
  __declspec(dllexport) ret WINAPI name params {                                  \
    typedef ret(WINAPI *Fn) params;                                               \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return fail_value;                                                            \
  }

#define DEFINE_WINMM_PROXY_BOOL(name, params, args)                               \
  __declspec(dllexport) BOOL WINAPI name params {                                 \
    typedef BOOL(WINAPI *Fn) params;                                              \
    static Fn fn = reinterpret_cast<Fn>(GetSystemWinmmProc(#name));               \
    if (fn)                                                                       \
      return fn args;                                                             \
    return FALSE;                                                                 \
  }

#undef PlaySound
#endif

static void InitializeRuntimeIfNeeded() {
  LONG state = InterlockedCompareExchange(&g_runtimeInitState, 2, 2);
  if (state == 2) {
    return;
  }

  if (InterlockedCompareExchange(&g_runtimeInitState, 1, 0) == 0) {
    OutputDebugStringA("SVMS: Lazy-initializing runtime...\n");
    SystemWinmm::Instance().Initialize();
    Config::Instance().Load("config.ini");
    LiveRuntime::Instance().Initialize();
    InterlockedExchange(&g_runtimeInitState, 2);
    return;
  }

  while (InterlockedCompareExchange(&g_runtimeInitState, 2, 2) != 2) {
    Sleep(0);
  }
}

extern "C" {

// ========================== KDMAPI IMPLEMENTATION ==========================

DebugInfo g_DebugInfo;

__declspec(dllexport) BOOL WINAPI IsKDMAPIAvailable() { return TRUE; }

__declspec(dllexport) BOOL WINAPI InitializeKDMAPIStream() {
  InitializeRuntimeIfNeeded();
  Synth::Instance().Initialize();
  if (Synth::Instance().GetRefCount() <= 0) {
    std::string status = Synth::Instance().GetLastInitStatus();
    LiveRuntime::Instance().PublishError(status.c_str());
    return FALSE;
  }
  if (!AudioOutput::Instance().Start()) {
    Synth::Instance().Shutdown();
    LiveRuntime::Instance().PublishError("Audio backend failed to start");
    return FALSE;
  }
  LiveRuntime::Instance().PublishStatus(
      Synth::Instance().GetLastInitStatus().c_str());
  return TRUE;
}

__declspec(dllexport) BOOL WINAPI TerminateKDMAPIStream() {
  InitializeRuntimeIfNeeded();
  AudioOutput::Instance().Stop();
  Synth::Instance().Shutdown();
  LiveRuntime::Instance().PublishStatus("KDMAPI stream closed");
  return TRUE;
}

__declspec(dllexport) VOID WINAPI ResetKDMAPIStream() {
  InitializeRuntimeIfNeeded();
  Synth::Instance().Reset();
}

__declspec(dllexport) BOOL WINAPI ReturnKDMAPIVer(LPDWORD Major, LPDWORD Minor,
                                                  LPDWORD Build,
                                                  LPDWORD Revision) {
  InitializeRuntimeIfNeeded();
  if (Major)
    *Major = CUR_MAJOR;
  if (Minor)
    *Minor = CUR_MINOR;
  if (Build)
    *Build = CUR_BUILD;
  if (Revision)
    *Revision = CUR_REV;
  return TRUE;
}

__declspec(dllexport) VOID WINAPI SendDirectData(DWORD dwMsg) {
  InitializeRuntimeIfNeeded();
  int status = dwMsg & 0xFF;
  int data1 = (dwMsg >> 8) & 0xFF;
  int data2 = (dwMsg >> 16) & 0xFF;
  int command = status & 0xF0;
  int channel = status & 0x0F;

  static int msgCount = 0;
  if (++msgCount <= 20) {
    char buf[128];
    sprintf(buf, "SVMS: MIDI cmd=%02X ch=%d d1=%d d2=%d\n", command, channel,
            data1, data2);
    OutputDebugStringA(buf);
  }

  if (command == 0x90) {
    if (data2 > 0)
      Synth::Instance().NoteOn(channel, data1, data2);
    else
      Synth::Instance().NoteOff(channel, data1);
  } else if (command == 0x80)
    Synth::Instance().NoteOff(channel, data1);
  else if (command == 0xC0)
    Synth::Instance().ProgramChange(channel, data1);
  else if (command == 0xB0)
    Synth::Instance().ControlChange(channel, data1, data2);
  else if (command == 0xE0)
    Synth::Instance().PitchBend(channel, data1 | (data2 << 7));
}

__declspec(dllexport) VOID WINAPI SendDirectDataNoBuf(DWORD dwMsg) {
  SendDirectData(dwMsg);
}

__declspec(dllexport) BOOL WINAPI SendCustomEvent(DWORD eventtype, DWORD chan,
                                                  DWORD param) {
  InitializeRuntimeIfNeeded();
  if (eventtype == 0x90)
    Synth::Instance().NoteOn(chan, param & 0x7F, (param >> 8) & 0x7F);
  else if (eventtype == 0x80)
    Synth::Instance().NoteOff(chan, param & 0x7F);
  return TRUE;
}

__declspec(dllexport) UINT WINAPI SendDirectLongData(MIDIHDR *IIMidiHdr,
                                                     UINT IIMidiHdrSize) {
  InitializeRuntimeIfNeeded();
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) UINT WINAPI
SendDirectLongDataNoBuf(LPSTR MidiHdrData, DWORD MidiHdrDataLen) {
  InitializeRuntimeIfNeeded();
  if (MidiHdrDataLen >= 6 && (unsigned char)MidiHdrData[0] == 0xF0 &&
      (unsigned char)MidiHdrData[1] == 0x7E) {
    if ((unsigned char)MidiHdrData[3] == 0x09 &&
        (unsigned char)MidiHdrData[4] == 0x01) {
      Synth::Instance().Reset();
    }
  }
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) UINT WINAPI PrepareLongData(MIDIHDR *IIMidiHdr,
                                                  UINT IIMidiHdrSize) {
  InitializeRuntimeIfNeeded();
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) UINT WINAPI UnprepareLongData(MIDIHDR *IIMidiHdr,
                                                    UINT IIMidiHdrSize) {
  InitializeRuntimeIfNeeded();
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) BOOL WINAPI DriverSettings(DWORD Setting, DWORD Mode,
                                                 LPVOID Value, UINT cbValue) {
  InitializeRuntimeIfNeeded();
  return TRUE;
}

__declspec(dllexport) DebugInfo *WINAPI GetDriverDebugInfo() {
  InitializeRuntimeIfNeeded();
  return &g_DebugInfo;
}

__declspec(dllexport) VOID WINAPI LoadCustomSoundFontsList(LPWSTR Directory) {}

__declspec(dllexport) DWORD64 WINAPI timeGetTime64() {
  InitializeRuntimeIfNeeded();
  return compat::GetTickCount64Compat();
}

// ========================== GENERIC WINMM PASSTHROUGH ==========================

#ifdef SVMS_LEGACY_XP
DEFINE_WINMM_PROXY_LRESULT(CloseDriver, (HDRVR hDriver, LPARAM lParam1,
                                         LPARAM lParam2),
                           (hDriver, lParam1, lParam2))
DEFINE_WINMM_PROXY_LRESULT(DefDriverProc, (DWORD_PTR dwDriverIdentifier,
                                           HDRVR hdrvr, UINT uMsg,
                                           LPARAM lParam1, LPARAM lParam2),
                           (dwDriverIdentifier, hdrvr, uMsg, lParam1, lParam2))
DEFINE_WINMM_PROXY_BOOL(DriverCallback,
                        (DWORD_PTR dwCallBack, DWORD dwFlags, HDRVR hDevice,
                         DWORD wMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1,
                         DWORD_PTR dwParam2),
                        (dwCallBack, dwFlags, hDevice, wMsg, dwUser, dwParam1,
                         dwParam2))
DEFINE_WINMM_PROXY_HMODULE(DrvGetModuleHandle, (HDRVR hDriver), (hDriver))
DEFINE_WINMM_PROXY_HMODULE(GetDriverModuleHandle, (HDRVR hDriver), (hDriver))
DEFINE_WINMM_PROXY_RET(HDRVR, OpenDriver,
                       (LPCWSTR szDriverName, LPCWSTR szSectionName,
                        LPARAM lParam2),
                       (szDriverName, szSectionName, lParam2), NULL)
DEFINE_WINMM_PROXY_LRESULT(SendDriverMessage,
                           (HDRVR hDriver, UINT message, LPARAM lParam1,
                            LPARAM lParam2),
                           (hDriver, message, lParam1, lParam2))
DEFINE_WINMM_PROXY_BOOL(PlaySoundA,
                        (LPCSTR pszSound, HMODULE hmod, DWORD fdwSound),
                        (pszSound, hmod, fdwSound))
DEFINE_WINMM_PROXY_BOOL(PlaySoundW,
                        (LPCWSTR pszSound, HMODULE hmod, DWORD fdwSound),
                        (pszSound, hmod, fdwSound))
DEFINE_WINMM_PROXY_BOOL(PlaySound,
                        (LPCSTR pszSound, HMODULE hmod, DWORD fdwSound),
                        (pszSound, hmod, fdwSound))
DEFINE_WINMM_PROXY_BOOL(sndPlaySoundA, (LPCSTR pszSound, UINT fuSound),
                        (pszSound, fuSound))
DEFINE_WINMM_PROXY_BOOL(sndPlaySoundW, (LPCWSTR pszSound, UINT fuSound),
                        (pszSound, fuSound))
DEFINE_WINMM_PROXY_UINT(auxGetNumDevs, (void), ())
DEFINE_WINMM_PROXY_MMRESULT(auxGetDevCapsA,
                            (UINT_PTR uDeviceID, LPAUXCAPSA pac, UINT cbac),
                            (uDeviceID, pac, cbac))
DEFINE_WINMM_PROXY_MMRESULT(auxGetDevCapsW,
                            (UINT_PTR uDeviceID, LPAUXCAPSW pac, UINT cbac),
                            (uDeviceID, pac, cbac))
DEFINE_WINMM_PROXY_MMRESULT(auxGetVolume, (UINT uDeviceID, LPDWORD pdwVolume),
                            (uDeviceID, pdwVolume))
DEFINE_WINMM_PROXY_MMRESULT(auxSetVolume, (UINT uDeviceID, DWORD dwVolume),
                            (uDeviceID, dwVolume))
DEFINE_WINMM_PROXY_MMRESULT(auxOutMessage,
                            (UINT uDeviceID, UINT uMsg, DWORD_PTR dwParam1,
                             DWORD_PTR dwParam2),
                            (uDeviceID, uMsg, dwParam1, dwParam2))
DEFINE_WINMM_PROXY_UINT(joyGetNumDevs, (void), ())
DEFINE_WINMM_PROXY_MMRESULT(joyGetDevCapsA,
                            (UINT_PTR uJoyID, LPJOYCAPSA pjc, UINT cbjc),
                            (uJoyID, pjc, cbjc))
DEFINE_WINMM_PROXY_MMRESULT(joyGetDevCapsW,
                            (UINT_PTR uJoyID, LPJOYCAPSW pjc, UINT cbjc),
                            (uJoyID, pjc, cbjc))
DEFINE_WINMM_PROXY_MMRESULT(joyGetPos, (UINT uJoyID, LPJOYINFO pji),
                            (uJoyID, pji))
DEFINE_WINMM_PROXY_MMRESULT(joyGetPosEx, (UINT uJoyID, LPJOYINFOEX pji),
                            (uJoyID, pji))
DEFINE_WINMM_PROXY_MMRESULT(joyGetThreshold, (UINT uJoyID, LPUINT puThreshold),
                            (uJoyID, puThreshold))
DEFINE_WINMM_PROXY_MMRESULT(joyReleaseCapture, (UINT uJoyID), (uJoyID))
DEFINE_WINMM_PROXY_MMRESULT(joySetCapture,
                            (HWND hwnd, UINT uJoyID, UINT uPeriod,
                             BOOL fChanged),
                            (hwnd, uJoyID, uPeriod, fChanged))
DEFINE_WINMM_PROXY_MMRESULT(joySetThreshold, (UINT uJoyID, UINT uThreshold),
                            (uJoyID, uThreshold))
DEFINE_WINMM_PROXY_MMRESULT(joyConfigChanged, (DWORD dwFlags), (dwFlags))
DEFINE_WINMM_PROXY_BOOL(mciExecute, (LPCSTR pszCommand), (pszCommand))
DEFINE_WINMM_PROXY_BOOL(mciGetErrorStringA,
                        (MCIERROR mcierr, LPSTR pszText, UINT cchText),
                        (mcierr, pszText, cchText))
DEFINE_WINMM_PROXY_BOOL(mciGetErrorStringW,
                        (MCIERROR mcierr, LPWSTR pszText, UINT cchText),
                        (mcierr, pszText, cchText))
DEFINE_WINMM_PROXY_RET(MCIERROR, mciSendStringA,
                       (LPCSTR lpstrCommand, LPSTR lpstrReturnString,
                        UINT uReturnLength, HWND hwndCallback),
                       (lpstrCommand, lpstrReturnString, uReturnLength,
                        hwndCallback),
                       0)
DEFINE_WINMM_PROXY_RET(MCIERROR, mciSendStringW,
                       (LPCWSTR lpstrCommand, LPWSTR lpstrReturnString,
                        UINT uReturnLength, HWND hwndCallback),
                       (lpstrCommand, lpstrReturnString, uReturnLength,
                        hwndCallback),
                       0)
DEFINE_WINMM_PROXY_DWORD(mmsystemGetVersion, (void), ())
DEFINE_WINMM_PROXY_RET(HMMIO, mmioOpenA,
                       (LPSTR pszFileName, LPMMIOINFO pmmioinfo, DWORD fdwOpen),
                       (pszFileName, pmmioinfo, fdwOpen), NULL)
DEFINE_WINMM_PROXY_RET(HMMIO, mmioOpenW,
                       (LPWSTR pszFileName, LPMMIOINFO pmmioinfo, DWORD fdwOpen),
                       (pszFileName, pmmioinfo, fdwOpen), NULL)
DEFINE_WINMM_PROXY_MMRESULT(mmioClose, (HMMIO hmmio, UINT wFlags),
                            (hmmio, wFlags))
DEFINE_WINMM_PROXY_LONG(mmioRead, (HMMIO hmmio, HPSTR pch, LONG cch),
                        (hmmio, pch, cch))
DEFINE_WINMM_PROXY_LONG(mmioWrite, (HMMIO hmmio, const char *pch, LONG cch),
                        (hmmio, pch, cch))
DEFINE_WINMM_PROXY_LRESULT(mmioSeek, (HMMIO hmmio, LONG lOffset, int iOrigin),
                           (hmmio, lOffset, iOrigin))
DEFINE_WINMM_PROXY_MMRESULT(mmioGetInfo, (HMMIO hmmio, LPMMIOINFO pmmioinfo,
                                          UINT wFlags),
                            (hmmio, pmmioinfo, wFlags))
DEFINE_WINMM_PROXY_MMRESULT(mmioSetInfo, (HMMIO hmmio, LPCMMIOINFO pmmioinfo,
                                          UINT wFlags),
                            (hmmio, pmmioinfo, wFlags))
DEFINE_WINMM_PROXY_FOURCC(mmioStringToFOURCCA, (LPCSTR sz, UINT wFlags),
                          (sz, wFlags))
DEFINE_WINMM_PROXY_FOURCC(mmioStringToFOURCCW, (LPCWSTR sz, UINT wFlags),
                          (sz, wFlags))
#endif

// ========================== MIDI OUT (INTERCEPTED) ==========================

__declspec(dllexport) MMRESULT WINAPI midiOutGetNumDevs(void) { return 1; }

__declspec(dllexport) MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR uDeviceID,
                                                         LPMIDIOUTCAPSA pmoc,
                                                         UINT cbmoc) {
  if (uDeviceID != 0 && uDeviceID != (UINT_PTR)-1)
    return MMSYSERR_BADDEVICEID;
  if (!pmoc)
    return MMSYSERR_INVALPARAM;
  memset(pmoc, 0, cbmoc);
  pmoc->wMid = 1;
  pmoc->wPid = 1;
  pmoc->vDriverVersion = 0x0100;
  strncpy(pmoc->szPname, "SuperVirtualMIDISynth", 32);
  pmoc->wTechnology = MOD_SQSYNTH;
  pmoc->wVoices = 0;
  pmoc->wNotes = 0;
  pmoc->wChannelMask = 0xFFFF;
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR uDeviceID,
                                                         LPMIDIOUTCAPSW pmoc,
                                                         UINT cbmoc) {
  if (uDeviceID != 0 && uDeviceID != (UINT_PTR)-1)
    return MMSYSERR_BADDEVICEID;
  if (!pmoc)
    return MMSYSERR_INVALPARAM;
  memset(pmoc, 0, cbmoc);
  pmoc->wMid = 1;
  pmoc->wPid = 1;
  pmoc->vDriverVersion = 0x0100;
  wcsncpy(pmoc->szPname, L"SuperVirtualMIDISynth", 32);
  pmoc->wTechnology = MOD_SQSYNTH;
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI midiOutOpen(LPHMIDIOUT phmo,
                                                  UINT uDeviceID,
                                                  DWORD_PTR dwCallback,
                                                  DWORD_PTR dwInstance,
                                                  DWORD fdwOpen) {
  InitializeRuntimeIfNeeded();
  OutputDebugStringA("SVMS: midiOutOpen called - initializing resources\n");
  Synth::Instance().Initialize();
  if (Synth::Instance().GetRefCount() <= 0) {
    std::string status = Synth::Instance().GetLastInitStatus();
    LiveRuntime::Instance().PublishError(status.c_str());
    return MMSYSERR_ERROR;
  }
  if (!AudioOutput::Instance().Start()) {
    Synth::Instance().Shutdown();
    LiveRuntime::Instance().PublishError("Audio backend failed to start");
    return MMSYSERR_ERROR;
  }
  if (phmo)
    *phmo = (HMIDIOUT)0x1234;
  LiveRuntime::Instance().PublishStatus(Synth::Instance().GetLastInitStatus().c_str());
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI midiOutClose(HMIDIOUT hmo) {
  InitializeRuntimeIfNeeded();
  OutputDebugStringA("SVMS: midiOutClose called - releasing resources\n");
  AudioOutput::Instance().Stop();
  Synth::Instance().Shutdown();
  LiveRuntime::Instance().PublishStatus("MIDI output closed");
  return MMSYSERR_NOERROR;
}
__declspec(dllexport) MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT hmo,
                                                           LPMIDIHDR pmh,
                                                           UINT cbmh) {
  return MMSYSERR_NOERROR;
}
__declspec(dllexport) MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT hmo,
                                                             LPMIDIHDR pmh,
                                                             UINT cbmh) {
  return MMSYSERR_NOERROR;
}
__declspec(dllexport) MMRESULT WINAPI midiOutShortMsg(HMIDIOUT hmo,
                                                      DWORD dwMsg) {
  SendDirectData(dwMsg);
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI midiOutLongMsg(HMIDIOUT hmo,
                                                     LPMIDIHDR pmh, UINT cbmh) {
  if (pmh && pmh->lpData)
    return SendDirectLongDataNoBuf(pmh->lpData, pmh->dwBufferLength);
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI midiOutReset(HMIDIOUT hmo) {
  InitializeRuntimeIfNeeded();
  Synth::Instance().Reset();
  return MMSYSERR_NOERROR;
}
__declspec(dllexport) MMRESULT WINAPI midiOutSetVolume(HMIDIOUT hmo,
                                                       DWORD dwVolume) {
  return MMSYSERR_NOERROR;
}
__declspec(dllexport) MMRESULT WINAPI midiOutGetVolume(HMIDIOUT hmo,
                                                       LPDWORD pdwVolume) {
  if (pdwVolume)
    *pdwVolume = 0xFFFFFFFF;
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT mmrError,
                                                           LPSTR pszText,
                                                           UINT cchText) {
  strncpy(pszText, "No Error", cchText);
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI midiOutGetErrorTextW(MMRESULT mmrError,
                                                           LPWSTR pszText,
                                                           UINT cchText) {
  wcsncpy(pszText, L"No Error", cchText);
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI midiOutMessage(HMIDIOUT hmo, UINT uMsg,
                                                     DWORD_PTR dw1,
                                                     DWORD_PTR dw2) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiOutMessage)
    return SystemWinmm::Instance().Real_midiOutMessage(hmo, uMsg, dw1, dw2);
  return MMSYSERR_NODRIVER;
}

// ========================== MIDI IN (PASSTHROUGH) ==========================

__declspec(dllexport) MMRESULT WINAPI midiInGetNumDevs(void) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInGetNumDevs)
    return SystemWinmm::Instance().Real_midiInGetNumDevs();
  return 0;
}

__declspec(dllexport) MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR uDeviceID,
                                                        LPMIDIINCAPSA pmic,
                                                        UINT cbmic) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInGetDevCapsA)
    return SystemWinmm::Instance().Real_midiInGetDevCapsA(uDeviceID, pmic,
                                                          cbmic);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR uDeviceID,
                                                        LPMIDIINCAPSW pmic,
                                                        UINT cbmic) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInGetDevCapsW)
    return SystemWinmm::Instance().Real_midiInGetDevCapsW(uDeviceID, pmic,
                                                          cbmic);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInOpen(LPHMIDIIN phmi, UINT uDeviceID,
                                                 DWORD_PTR dwCallback,
                                                 DWORD_PTR dwInstance,
                                                 DWORD fdwOpen) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInOpen)
    return SystemWinmm::Instance().Real_midiInOpen(phmi, uDeviceID, dwCallback,
                                                   dwInstance, fdwOpen);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInClose(HMIDIIN hmi) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInClose)
    return SystemWinmm::Instance().Real_midiInClose(hmi);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInPrepareHeader(HMIDIIN hmi,
                                                          LPMIDIHDR pmh,
                                                          UINT cbmh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInPrepareHeader)
    return SystemWinmm::Instance().Real_midiInPrepareHeader(hmi, pmh, cbmh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInUnprepareHeader(HMIDIIN hmi,
                                                            LPMIDIHDR pmh,
                                                            UINT cbmh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInUnprepareHeader)
    return SystemWinmm::Instance().Real_midiInUnprepareHeader(hmi, pmh, cbmh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInAddBuffer(HMIDIIN hmi,
                                                      LPMIDIHDR pmh,
                                                      UINT cbmh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInAddBuffer)
    return SystemWinmm::Instance().Real_midiInAddBuffer(hmi, pmh, cbmh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInStart(HMIDIIN hmi) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInStart)
    return SystemWinmm::Instance().Real_midiInStart(hmi);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInStop(HMIDIIN hmi) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInStop)
    return SystemWinmm::Instance().Real_midiInStop(hmi);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInReset(HMIDIIN hmi) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInReset)
    return SystemWinmm::Instance().Real_midiInReset(hmi);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI midiInMessage(HMIDIIN hmi, UINT uMsg,
                                                    DWORD_PTR dwParam1,
                                                    DWORD_PTR dwParam2) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_midiInMessage)
    return SystemWinmm::Instance().Real_midiInMessage(hmi, uMsg, dwParam1,
                                                      dwParam2);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) UINT WINAPI waveInGetNumDevs(void) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInGetNumDevs)
    return SystemWinmm::Instance().Real_waveInGetNumDevs();
  return 0;
}

__declspec(dllexport) MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR uDeviceID,
                                                        LPWAVEINCAPSA pwic,
                                                        UINT cbwic) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInGetDevCapsA)
    return SystemWinmm::Instance().Real_waveInGetDevCapsA(uDeviceID, pwic,
                                                          cbwic);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR uDeviceID,
                                                        LPWAVEINCAPSW pwic,
                                                        UINT cbwic) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInGetDevCapsW)
    return SystemWinmm::Instance().Real_waveInGetDevCapsW(uDeviceID, pwic,
                                                          cbwic);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInOpen(LPHWAVEIN phwi,
                                                 UINT uDeviceID,
                                                 LPCWAVEFORMATEX pwfx,
                                                 DWORD_PTR dwCallback,
                                                 DWORD_PTR dwInstance,
                                                 DWORD fdwOpen) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInOpen)
    return SystemWinmm::Instance().Real_waveInOpen(phwi, uDeviceID, pwfx,
                                                   dwCallback, dwInstance,
                                                   fdwOpen);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInClose(HWAVEIN hwi) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInClose)
    return SystemWinmm::Instance().Real_waveInClose(hwi);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInPrepareHeader(HWAVEIN hwi,
                                                          LPWAVEHDR pwh,
                                                          UINT cbwh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInPrepareHeader)
    return SystemWinmm::Instance().Real_waveInPrepareHeader(hwi, pwh, cbwh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN hwi,
                                                            LPWAVEHDR pwh,
                                                            UINT cbwh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInUnprepareHeader)
    return SystemWinmm::Instance().Real_waveInUnprepareHeader(hwi, pwh, cbwh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInAddBuffer(HWAVEIN hwi,
                                                      LPWAVEHDR pwh,
                                                      UINT cbwh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInAddBuffer)
    return SystemWinmm::Instance().Real_waveInAddBuffer(hwi, pwh, cbwh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInStart(HWAVEIN hwi) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInStart)
    return SystemWinmm::Instance().Real_waveInStart(hwi);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInStop(HWAVEIN hwi) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInStop)
    return SystemWinmm::Instance().Real_waveInStop(hwi);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInReset(HWAVEIN hwi) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInReset)
    return SystemWinmm::Instance().Real_waveInReset(hwi);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInMessage(HWAVEIN hwi, UINT uMsg,
                                                    DWORD_PTR dwParam1,
                                                    DWORD_PTR dwParam2) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInMessage)
    return SystemWinmm::Instance().Real_waveInMessage(hwi, uMsg, dwParam1,
                                                      dwParam2);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveInGetPosition(HWAVEIN hwi,
                                                        LPMMTIME pmmt,
                                                        UINT cbmmt) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveInGetPosition)
    return SystemWinmm::Instance().Real_waveInGetPosition(hwi, pmmt, cbmmt);
  return MMSYSERR_NODRIVER;
}

// ========================== MIXER (PASSTHROUGH) ==========================

__declspec(dllexport) UINT WINAPI mixerGetNumDevs(void) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetNumDevs)
    return SystemWinmm::Instance().Real_mixerGetNumDevs();
  return 0;
}

__declspec(dllexport) MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR uMxId,
                                                       LPMIXERCAPSA pmxcaps,
                                                       UINT cbmxcaps) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetDevCapsA)
    return SystemWinmm::Instance().Real_mixerGetDevCapsA(uMxId, pmxcaps,
                                                         cbmxcaps);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR uMxId,
                                                       LPMIXERCAPSW pmxcaps,
                                                       UINT cbmxcaps) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetDevCapsW)
    return SystemWinmm::Instance().Real_mixerGetDevCapsW(uMxId, pmxcaps,
                                                         cbmxcaps);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI mixerOpen(LPHMIXER phmx, UINT uMxId,
                                                DWORD_PTR dwCallback,
                                                DWORD_PTR dwInstance,
                                                DWORD fdwOpen) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerOpen)
    return SystemWinmm::Instance().Real_mixerOpen(phmx, uMxId, dwCallback,
                                                  dwInstance, fdwOpen);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI mixerClose(HMIXER hmx) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerClose)
    return SystemWinmm::Instance().Real_mixerClose(hmx);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) DWORD WINAPI mixerMessage(HMIXER hmx, UINT uMsg,
                                                DWORD_PTR dwParam1,
                                                DWORD_PTR dwParam2) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerMessage)
    return SystemWinmm::Instance().Real_mixerMessage(hmx, uMsg, dwParam1,
                                                     dwParam2);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI mixerGetLineInfoA(HMIXEROBJ hmxobj,
                                                        LPMIXERLINEA pmxl,
                                                        DWORD fdwInfo) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetLineInfoA)
    return SystemWinmm::Instance().Real_mixerGetLineInfoA(hmxobj, pmxl,
                                                          fdwInfo);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI mixerGetLineInfoW(HMIXEROBJ hmxobj,
                                                        LPMIXERLINEW pmxl,
                                                        DWORD fdwInfo) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetLineInfoW)
    return SystemWinmm::Instance().Real_mixerGetLineInfoW(hmxobj, pmxl,
                                                          fdwInfo);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI mixerGetID(HMIXEROBJ hmxobj,
                                                 UINT *puMxId, DWORD fdwId) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetID)
    return SystemWinmm::Instance().Real_mixerGetID(hmxobj, puMxId, fdwId);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI
mixerGetLineControlsA(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSA pmxlc,
                      DWORD fdwControls) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetLineControlsA)
    return SystemWinmm::Instance().Real_mixerGetLineControlsA(hmxobj, pmxlc,
                                                              fdwControls);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI
mixerGetLineControlsW(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSW pmxlc,
                      DWORD fdwControls) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetLineControlsW)
    return SystemWinmm::Instance().Real_mixerGetLineControlsW(hmxobj, pmxlc,
                                                              fdwControls);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI
mixerGetControlDetailsA(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd,
                        DWORD fdwDetails) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetControlDetailsA)
    return SystemWinmm::Instance().Real_mixerGetControlDetailsA(hmxobj, pmxcd,
                                                                fdwDetails);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI
mixerGetControlDetailsW(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd,
                        DWORD fdwDetails) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerGetControlDetailsW)
    return SystemWinmm::Instance().Real_mixerGetControlDetailsW(hmxobj, pmxcd,
                                                                fdwDetails);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI
mixerSetControlDetails(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd,
                       DWORD fdwDetails) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_mixerSetControlDetails)
    return SystemWinmm::Instance().Real_mixerSetControlDetails(hmxobj, pmxcd,
                                                               fdwDetails);
  return MMSYSERR_NODRIVER;
}

// ========================== WAVE OUT (PASSTHROUGH) ==========================

__declspec(dllexport) MMRESULT WINAPI waveOutGetNumDevs(void) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetNumDevs)
    return SystemWinmm::Instance().Real_waveOutGetNumDevs();
  return 0;
}

__declspec(dllexport) MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR uDeviceID,
                                                         LPWAVEOUTCAPSA pwoc,
                                                         UINT cbwoc) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetDevCapsA)
    return SystemWinmm::Instance().Real_waveOutGetDevCapsA(uDeviceID, pwoc,
                                                           cbwoc);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR uDeviceID,
                                                         LPWAVEOUTCAPSW pwoc,
                                                         UINT cbwoc) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetDevCapsW)
    return SystemWinmm::Instance().Real_waveOutGetDevCapsW(uDeviceID, pwoc,
                                                           cbwoc);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI
waveOutOpen(LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
            DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutOpen)
    return SystemWinmm::Instance().Real_waveOutOpen(
        phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutClose(HWAVEOUT hwo) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutClose)
    return SystemWinmm::Instance().Real_waveOutClose(hwo);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT hwo,
                                                           LPWAVEHDR pwh,
                                                           UINT cbwh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutPrepareHeader)
    return SystemWinmm::Instance().Real_waveOutPrepareHeader(hwo, pwh, cbwh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT hwo,
                                                             LPWAVEHDR pwh,
                                                             UINT cbwh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutUnprepareHeader)
    return SystemWinmm::Instance().Real_waveOutUnprepareHeader(hwo, pwh, cbwh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutWrite(HWAVEOUT hwo, LPWAVEHDR pwh,
                                                   UINT cbwh) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutWrite)
    return SystemWinmm::Instance().Real_waveOutWrite(hwo, pwh, cbwh);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutReset(HWAVEOUT hwo) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutReset)
    return SystemWinmm::Instance().Real_waveOutReset(hwo);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutRestart(HWAVEOUT hwo) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutRestart)
    return SystemWinmm::Instance().Real_waveOutRestart(hwo);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutPause(HWAVEOUT hwo) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutPause)
    return SystemWinmm::Instance().Real_waveOutPause(hwo);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT hwo) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutBreakLoop)
    return SystemWinmm::Instance().Real_waveOutBreakLoop(hwo);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutGetPosition(HWAVEOUT hwo,
                                                         LPMMTIME pmmt,
                                                         UINT cbmmt) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetPosition)
    return SystemWinmm::Instance().Real_waveOutGetPosition(hwo, pmmt, cbmmt);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutGetVolume(HWAVEOUT hwo,
                                                       LPDWORD pdwVolume) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetVolume)
    return SystemWinmm::Instance().Real_waveOutGetVolume(hwo, pdwVolume);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutSetVolume(HWAVEOUT hwo,
                                                       DWORD dwVolume) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutSetVolume)
    return SystemWinmm::Instance().Real_waveOutSetVolume(hwo, dwVolume);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutGetPitch(HWAVEOUT hwo,
                                                      LPDWORD pdwPitch) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetPitch)
    return SystemWinmm::Instance().Real_waveOutGetPitch(hwo, pdwPitch);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutSetPitch(HWAVEOUT hwo,
                                                      DWORD dwPitch) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutSetPitch)
    return SystemWinmm::Instance().Real_waveOutSetPitch(hwo, dwPitch);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI
waveOutGetPlaybackRate(HWAVEOUT hwo, LPDWORD pdwRate) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetPlaybackRate)
    return SystemWinmm::Instance().Real_waveOutGetPlaybackRate(hwo, pdwRate);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutSetPlaybackRate(HWAVEOUT hwo,
                                                             DWORD dwRate) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutSetPlaybackRate)
    return SystemWinmm::Instance().Real_waveOutSetPlaybackRate(hwo, dwRate);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutGetID(HWAVEOUT hwo,
                                                   LPUINT puDeviceID) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetID)
    return SystemWinmm::Instance().Real_waveOutGetID(hwo, puDeviceID);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT mmrError,
                                                           LPSTR pszText,
                                                           UINT cchText) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetErrorTextA)
    return SystemWinmm::Instance().Real_waveOutGetErrorTextA(mmrError, pszText,
                                                             cchText);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT mmrError,
                                                           LPWSTR pszText,
                                                           UINT cchText) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutGetErrorTextW)
    return SystemWinmm::Instance().Real_waveOutGetErrorTextW(mmrError, pszText,
                                                             cchText);
  return MMSYSERR_NODRIVER;
}

__declspec(dllexport) MMRESULT WINAPI waveOutMessage(HWAVEOUT hwo, UINT uMsg,
                                                     DWORD_PTR dwParam1,
                                                     DWORD_PTR dwParam2) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_waveOutMessage)
    return SystemWinmm::Instance().Real_waveOutMessage(hwo, uMsg, dwParam1,
                                                       dwParam2);
  return MMSYSERR_NODRIVER;
}

// ========================== TIME (PASSTHROUGH) ==========================

__declspec(dllexport) DWORD WINAPI timeGetTime(void) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_timeGetTime)
    return SystemWinmm::Instance().Real_timeGetTime();
  return GetTickCount(); // Fallback
}

__declspec(dllexport) MMRESULT WINAPI timeBeginPeriod(UINT uPeriod) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_timeBeginPeriod)
    return SystemWinmm::Instance().Real_timeBeginPeriod(uPeriod);
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI timeEndPeriod(UINT uPeriod) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_timeEndPeriod)
    return SystemWinmm::Instance().Real_timeEndPeriod(uPeriod);
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS ptc,
                                                     UINT cbtc) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_timeGetDevCaps)
    return SystemWinmm::Instance().Real_timeGetDevCaps(ptc, cbtc);
  if (ptc) {
    ptc->wPeriodMin = 1;
    ptc->wPeriodMax = 1000;
  }
  return MMSYSERR_NOERROR;
}

__declspec(dllexport) MMRESULT WINAPI timeSetEvent(UINT uDelay,
                                                   UINT uResolution,
                                                   LPTIMECALLBACK lpTimeProc,
                                                   DWORD_PTR dwUser,
                                                   UINT fuEvent) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_timeSetEvent)
    return SystemWinmm::Instance().Real_timeSetEvent(
        uDelay, uResolution, lpTimeProc, dwUser, fuEvent);
  return 0;
}

__declspec(dllexport) MMRESULT WINAPI timeKillEvent(UINT uTimerID) {
  InitializeRuntimeIfNeeded();
  if (SystemWinmm::Instance().Real_timeKillEvent)
    return SystemWinmm::Instance().Real_timeKillEvent(uTimerID);
  return MMSYSERR_NOERROR;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
  case DLL_PROCESS_ATTACH:
    // Initialize Windows 7 compatibility layer for time functions
    CompatInitializeTimeFunctions();
    
    OutputDebugStringA("SVMS: DLL_PROCESS_ATTACH\n");
    OutputDebugStringA(kStartupAttributionLine);
    OutputDebugStringA(kAttributionLine);
    OutputDebugStringA("SVMS: Deferring runtime initialization\n");
    OutputDebugStringA("SVMS: DLL_PROCESS_ATTACH complete\n");
    break;
  case DLL_PROCESS_DETACH:
    OutputDebugStringA("SVMS: DLL_PROCESS_DETACH\n");
    // If lpReserved is non-NULL, the process is terminating.
    // Joining threads here is unsafe (Loader Lock deadlock).
    // If NULL, we are being unloaded (FreeLibrary), so we MUST join to avoid
    // crashing.
    bool isTerminating = (lpReserved != NULL);
    OutputDebugStringA(isTerminating
                           ? "SVMS: Process terminating, detaching threads...\n"
                           : "SVMS: Dynamic unload, joining threads...\n");

    // Stop the audio thread before tearing down synth state so hot unloads
    // cannot render against a destroyed engine.
    AudioOutput::Instance().Stop(!isTerminating);
    // RefCounted shutdown will handle safety internally or return early if
    // already shutdown.
    Synth::Instance().Shutdown(!isTerminating);
    LiveRuntime::Instance().Shutdown(!isTerminating);
    break;
  }
  return TRUE;
}
