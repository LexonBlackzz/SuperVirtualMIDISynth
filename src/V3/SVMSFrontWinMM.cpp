// WinMM shim exports: midiOut (routed to the engine), midiIn/waveIn/
// waveOut/mixer/time (forwarded to the system winmm).

#include "SVMSDriverInternal.h"


static const HMIDIOUT kSVMSMidiOutHandle = reinterpret_cast<HMIDIOUT>(0x1234);
static DWORD_PTR g_midiOutCallback = 0u;
static DWORD_PTR g_midiOutInstance = 0u;
static DWORD g_midiOutCallbackFlags = CALLBACK_NULL;

static void NotifyMidiOutClient(UINT message, DWORD_PTR param1 = 0u,
                                DWORD_PTR param2 = 0u) {
    switch (g_midiOutCallbackFlags & CALLBACK_TYPEMASK) {
        case CALLBACK_FUNCTION:
            if (g_midiOutCallback) {
                using CallbackProc = void (CALLBACK*)(
                    HMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
                reinterpret_cast<CallbackProc>(g_midiOutCallback)(
                    kSVMSMidiOutHandle, message, g_midiOutInstance,
                    param1, param2);
            }
            break;
        case CALLBACK_WINDOW:
            if (g_midiOutCallback) {
                PostMessageW(reinterpret_cast<HWND>(g_midiOutCallback),
                             message,
                             reinterpret_cast<WPARAM>(kSVMSMidiOutHandle),
                             static_cast<LPARAM>(param1));
            }
            break;
        case CALLBACK_THREAD:
            if (g_midiOutCallback) {
                PostThreadMessageW(static_cast<DWORD>(g_midiOutCallback),
                                   message,
                                   reinterpret_cast<WPARAM>(kSVMSMidiOutHandle),
                                   static_cast<LPARAM>(param1));
            }
            break;
        case CALLBACK_EVENT:
            if (g_midiOutCallback)
                SetEvent(reinterpret_cast<HANDLE>(g_midiOutCallback));
            break;
        default:
            break;
    }
}

static bool IsSupportedMidiOutputDevice(UINT_PTR deviceId) {
    return deviceId == 0u || deviceId == static_cast<UINT_PTR>(MIDI_MAPPER);
}

extern "C" {

BOOL WINAPI PlaySoundA(LPCSTR pszSound, HMODULE hmod, DWORD fdwSound) {
    using Proc = BOOL (WINAPI*)(LPCSTR, HMODULE, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("PlaySoundA"));
    return proc ? proc(pszSound, hmod, fdwSound) : FALSE;
}

BOOL WINAPI PlaySoundW(LPCWSTR pszSound, HMODULE hmod, DWORD fdwSound) {
    using Proc = BOOL (WINAPI*)(LPCWSTR, HMODULE, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("PlaySoundW"));
    return proc ? proc(pszSound, hmod, fdwSound) : FALSE;
}

UINT WINAPI midiOutGetNumDevs(void) {
    XPBootstrapTrace("[SVMS XP] midiOutGetNumDevs reached\r\n");
    LOG("midiOutGetNumDevs -> 1");
    return 1;
}

MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR uDeviceID, LPMIDIOUTCAPSA lpCaps, UINT cbCaps) {
    if (!IsSupportedMidiOutputDevice(uDeviceID) || !lpCaps || cbCaps < sizeof(MIDIOUTCAPSA))
        return MMSYSERR_BADDEVICEID;
    std::memset(lpCaps, 0, cbCaps);
    lpCaps->wMid = 1;
    lpCaps->wPid = 1;
    lpCaps->vDriverVersion = 0x0300;
    std::memcpy(lpCaps->szPname, "SuperVirtualMIDISynth V3", 25);
    lpCaps->wTechnology = MOD_SWSYNTH;
    lpCaps->wVoices = 64;
    lpCaps->wNotes = 64;
    lpCaps->wChannelMask = 0xFFFF;
    lpCaps->dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR uDeviceID, LPMIDIOUTCAPSW lpCaps, UINT cbCaps) {
    if (!IsSupportedMidiOutputDevice(uDeviceID) || !lpCaps || cbCaps < sizeof(MIDIOUTCAPSW))
        return MMSYSERR_BADDEVICEID;
    std::memset(lpCaps, 0, cbCaps);
    lpCaps->wMid = 1;
    lpCaps->wPid = 1;
    lpCaps->vDriverVersion = 0x0300;
    const wchar_t name[] = L"SuperVirtualMIDISynth V3";
    std::memcpy(lpCaps->szPname, name, sizeof(name));
    lpCaps->wTechnology = MOD_SWSYNTH;
    lpCaps->wVoices = 64;
    lpCaps->wNotes = 64;
    lpCaps->wChannelMask = 0xFFFF;
    lpCaps->dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutOpen(LPHMIDIOUT phmo, UINT uDeviceID,
    DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
    XPBootstrapTrace("[SVMS XP] midiOutOpen reached\r\n");
    LOG("midiOutOpen: uDeviceID=%u", uDeviceID);
    if (!IsSupportedMidiOutputDevice(uDeviceID)) {
        XPBootstrapTrace("[SVMS XP] midiOutOpen rejected unsupported device ID\r\n");
        return MMSYSERR_BADDEVICEID;
    }
    if (!phmo) return MMSYSERR_INVALPARAM;

    g_winmmOwners.fetch_add(1u, std::memory_order_acq_rel);
    if (!EnsureDriverInitialized()) {
        g_winmmOwners.fetch_sub(1u, std::memory_order_acq_rel);
        LOG("midiOutOpen: engine start FAILED");
        XPBootstrapTrace("[SVMS XP] engine initialization FAILED\r\n");
        return MMSYSERR_NOMEM;
    }

    LOG("midiOutOpen: SUCCESS, returning handle");
    *phmo = kSVMSMidiOutHandle;
    g_midiOutCallback = dwCallback;
    g_midiOutInstance = dwInstance;
    g_midiOutCallbackFlags = fdwOpen;
    NotifyMidiOutClient(MOM_OPEN);
    XPBootstrapTrace("[SVMS XP] midiOutOpen SUCCESS handle=0x00001234\r\n");
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutClose(HMIDIOUT hmo) {
    if (hmo != kSVMSMidiOutHandle) return MMSYSERR_INVALHANDLE;
    XPBootstrapTrace("[SVMS XP] midiOutClose reached\r\n");
    uint32_t owners = g_winmmOwners.load(std::memory_order_acquire);
    while (owners != 0u && !g_winmmOwners.compare_exchange_weak(
        owners, owners - 1u, std::memory_order_acq_rel,
        std::memory_order_acquire)) {}
    MaybeShutdownDriver();
    NotifyMidiOutClient(MOM_CLOSE);
    g_midiOutCallback = 0u;
    g_midiOutInstance = 0u;
    g_midiOutCallbackFlags = CALLBACK_NULL;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutShortMsg(HMIDIOUT hmo, DWORD dwMsg) {
    static LONG traceCount = 0;
    const LONG traceIndex = InterlockedIncrement(&traceCount);
    if (traceIndex <= 32) {
        char message[256] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] midiOutShortMsg #%ld handle=%p raw=0x%08lX status=0x%02lX data1=%lu data2=%lu driver=%s\r\n",
                      static_cast<long>(traceIndex), static_cast<void*>(hmo),
                      static_cast<unsigned long>(dwMsg),
                      static_cast<unsigned long>(dwMsg & 0xFFu),
                      static_cast<unsigned long>((dwMsg >> 8) & 0x7Fu),
                      static_cast<unsigned long>((dwMsg >> 16) & 0x7Fu),
                      g_driver ? "ready" : "null");
        OutputDebugStringA(message);
    }
    if (g_driver) {
        static int msgCount = 0;
        if (msgCount < 15) {
            LOG("midiOutShortMsg #%d: 0x%08X", msgCount, dwMsg);
        }
        msgCount++;
        g_driver->SubmitShortMsg(dwMsg);
    }
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutLongMsg(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    XPBootstrapTrace("[SVMS XP] midiOutLongMsg reached\r\n");
    if (hmo != kSVMSMidiOutHandle) return MMSYSERR_INVALHANDLE;
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr) ||
        (!lpMidiHdr->lpData && lpMidiHdr->dwBufferLength != 0u))
        return MMSYSERR_INVALPARAM;
    if ((lpMidiHdr->dwFlags & MHDR_PREPARED) == 0u)
        return MIDIERR_UNPREPARED;
    lpMidiHdr->dwFlags &= ~MHDR_DONE;
    lpMidiHdr->dwFlags |= MHDR_INQUEUE;
    if (g_driver && lpMidiHdr->dwBufferLength != 0u) {
        g_driver->SubmitSystemExclusive(
            reinterpret_cast<const uint8_t*>(lpMidiHdr->lpData),
            lpMidiHdr->dwBufferLength);
    }
    lpMidiHdr->dwFlags &= ~MHDR_INQUEUE;
    lpMidiHdr->dwFlags |= MHDR_DONE;
    NotifyMidiOutClient(MOM_DONE,
                        reinterpret_cast<DWORD_PTR>(lpMidiHdr), 0u);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutReset(HMIDIOUT hmo) {
    (void)hmo;
    if (g_driver) g_driver->ResetAllVoices();
    LOG("midiOutReset: all voices released");
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (hmo != kSVMSMidiOutHandle) return MMSYSERR_INVALHANDLE;
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr))
        return MMSYSERR_INVALPARAM;
    lpMidiHdr->dwFlags |= MHDR_PREPARED;
    lpMidiHdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (hmo != kSVMSMidiOutHandle) return MMSYSERR_INVALHANDLE;
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr))
        return MMSYSERR_INVALPARAM;
    if ((lpMidiHdr->dwFlags & MHDR_INQUEUE) != 0u)
        return MIDIERR_STILLPLAYING;
    lpMidiHdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetVolume(HMIDIOUT hmo, LPDWORD pdwVolume) {
    (void)hmo;
    if (!pdwVolume) return MMSYSERR_INVALPARAM;
    *pdwVolume = 0xFFFFFFFF;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutSetVolume(HMIDIOUT hmo, DWORD dwVolume) {
    (void)hmo; (void)dwVolume;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT mmrError, LPSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const char* msg = "Unknown error";
    switch (mmrError) {
        case MMSYSERR_NOERROR: msg = "No error"; break;
        case MMSYSERR_BADDEVICEID: msg = "Bad device ID"; break;
        case MMSYSERR_INVALHANDLE: msg = "Invalid handle"; break;
        case MMSYSERR_INVALPARAM: msg = "Invalid parameter"; break;
        case MMSYSERR_NOMEM: msg = "Out of memory"; break;
    }
    size_t len = std::strlen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len);
    lpText[len] = '\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetErrorTextW(MMRESULT mmrError, LPWSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const wchar_t* msg = L"Unknown error";
    switch (mmrError) {
        case MMSYSERR_NOERROR: msg = L"No error"; break;
        case MMSYSERR_BADDEVICEID: msg = L"Bad device ID"; break;
        case MMSYSERR_INVALHANDLE: msg = L"Invalid handle"; break;
        case MMSYSERR_INVALPARAM: msg = L"Invalid parameter"; break;
        case MMSYSERR_NOMEM: msg = L"Out of memory"; break;
    }
    size_t len = wcslen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len * sizeof(wchar_t));
    lpText[len] = L'\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutMessage(HMIDIOUT hmo, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hmo; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_NOERROR;
}

// ── MIDI Input ────────────────────────────────────────────────────────

UINT WINAPI midiInGetNumDevs(void) {
    using Proc = UINT (WINAPI*)(void);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInGetNumDevs"));
    return proc ? proc() : 0u;
}

MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR uDeviceID, LPMIDIINCAPSA lpCaps, UINT cbCaps) {
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPMIDIINCAPSA, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInGetDevCapsA"));
    return proc ? proc(uDeviceID, lpCaps, cbCaps) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR uDeviceID, LPMIDIINCAPSW lpCaps, UINT cbCaps) {
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPMIDIINCAPSW, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInGetDevCapsW"));
    return proc ? proc(uDeviceID, lpCaps, cbCaps) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInOpen(LPHMIDIIN phmi, UINT uDeviceID, DWORD_PTR dwCallback,
                           DWORD_PTR dwInstance, DWORD fdwOpen) {
    using Proc = MMRESULT (WINAPI*)(LPHMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInOpen"));
    return proc ? proc(phmi, uDeviceID, dwCallback, dwInstance, fdwOpen)
                : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInClose(HMIDIIN hmi) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInClose"));
    return proc ? proc(hmi) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInPrepareHeader(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInPrepareHeader"));
    return proc ? proc(hmi, lpMidiHdr, cbMidiHdr) : MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInUnprepareHeader(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInUnprepareHeader"));
    return proc ? proc(hmi, lpMidiHdr, cbMidiHdr) : MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInAddBuffer(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInAddBuffer"));
    return proc ? proc(hmi, lpMidiHdr, cbMidiHdr) : MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInStart(HMIDIIN hmi) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInStart"));
    return proc ? proc(hmi) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInStop(HMIDIIN hmi) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInStop"));
    return proc ? proc(hmi) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInReset(HMIDIIN hmi) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInReset"));
    return proc ? proc(hmi) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInMessage(HMIDIIN hmi, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInMessage"));
    return proc ? proc(hmi, uMsg, dw1, dw2) : MMSYSERR_ERROR;
}

// ── Wave Input ─────────────────────────────────────────────────────────

UINT WINAPI waveInGetNumDevs(void) { return 0; }

MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR uDeviceID, LPWAVEINCAPSA lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR uDeviceID, LPWAVEINCAPSW lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInOpen(LPHWAVEIN phwi, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                           DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)phwi; (void)uDeviceID; (void)pwfx; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInClose(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInPrepareHeader(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInAddBuffer(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInStart(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInStop(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInReset(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInMessage(HWAVEIN hwi, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hwi; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInGetPosition(HWAVEIN hwi, LPMMTIME pmmt, UINT cbmmt) {
    (void)hwi; (void)pmmt; (void)cbmmt;
    return MMSYSERR_ERROR;
}

// ── Mixer ──────────────────────────────────────────────────────────────

UINT WINAPI mixerGetNumDevs(void) {
#if defined(SVMS_XP_COMPAT)
    using Proc = UINT (WINAPI*)(void);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetNumDevs"));
    if (proc) return proc();
#endif
    return 0;
}

MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR uMxId, LPMIXERCAPSA lpCaps, UINT cbCaps) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPMIXERCAPSA, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetDevCapsA"));
    if (proc) return proc(uMxId, lpCaps, cbCaps);
#endif
    (void)uMxId; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR uMxId, LPMIXERCAPSW lpCaps, UINT cbCaps) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPMIXERCAPSW, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetDevCapsW"));
    if (proc) return proc(uMxId, lpCaps, cbCaps);
#endif
    (void)uMxId; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerOpen(LPHMIXER phmx, UINT uMxId, DWORD_PTR dwCallback,
                          DWORD_PTR dwInstance, DWORD fdwOpen) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(LPHMIXER, UINT, DWORD_PTR, DWORD_PTR, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerOpen"));
    if (proc) return proc(phmx, uMxId, dwCallback, dwInstance, fdwOpen);
#endif
    (void)phmx; (void)uMxId; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerClose(HMIXER hmx) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXER);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerClose"));
    if (proc) return proc(hmx);
#endif
    (void)hmx;
    return MMSYSERR_BADDEVICEID;
}

DWORD WINAPI mixerMessage(HMIXER hmx, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
#if defined(SVMS_XP_COMPAT)
    using Proc = DWORD (WINAPI*)(HMIXER, UINT, DWORD_PTR, DWORD_PTR);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerMessage"));
    if (proc) return proc(hmx, uMsg, dw1, dw2);
#endif
    (void)hmx; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineInfoA(HMIXEROBJ hmxobj, LPMIXERLINEA pmxl, DWORD fdwInfo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERLINEA, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetLineInfoA"));
    if (proc) return proc(hmxobj, pmxl, fdwInfo);
#endif
    (void)hmxobj; (void)pmxl; (void)fdwInfo;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineInfoW(HMIXEROBJ hmxobj, LPMIXERLINEW pmxl, DWORD fdwInfo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERLINEW, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetLineInfoW"));
    if (proc) return proc(hmxobj, pmxl, fdwInfo);
#endif
    (void)hmxobj; (void)pmxl; (void)fdwInfo;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetID(HMIXEROBJ hmxobj, LPUINT puMxId, DWORD fdwId) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPUINT, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetID"));
    if (proc) return proc(hmxobj, puMxId, fdwId);
#endif
    (void)hmxobj; (void)puMxId; (void)fdwId;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineControlsA(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSA pmxlc, DWORD fdwControls) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERLINECONTROLSA, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetLineControlsA"));
    if (proc) return proc(hmxobj, pmxlc, fdwControls);
#endif
    (void)hmxobj; (void)pmxlc; (void)fdwControls;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineControlsW(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSW pmxlc, DWORD fdwControls) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERLINECONTROLSW, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetLineControlsW"));
    if (proc) return proc(hmxobj, pmxlc, fdwControls);
#endif
    (void)hmxobj; (void)pmxlc; (void)fdwControls;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetControlDetailsA(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetControlDetailsA"));
    if (proc) return proc(hmxobj, pmxcd, fdwDetails);
#endif
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetControlDetailsW(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetControlDetailsW"));
    if (proc) return proc(hmxobj, pmxcd, fdwDetails);
#endif
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerSetControlDetails(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerSetControlDetails"));
    if (proc) return proc(hmxobj, pmxcd, fdwDetails);
#endif
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

// ── Wave Output ────────────────────────────────────────────────────────

UINT WINAPI waveOutGetNumDevs(void) {
#if defined(SVMS_XP_COMPAT)
    using Proc = UINT (WINAPI*)(void);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetNumDevs"));
    if (proc) {
        const UINT count = proc();
        char message[256] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] forwarded waveOutGetNumDevs proc=%p result=%u\r\n",
                      reinterpret_cast<void*>(proc),
                      static_cast<unsigned>(count));
        OutputDebugStringA(message);
        return count;
    }
#endif
    return 0;
}

MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR uDeviceID, LPWAVEOUTCAPSA lpCaps, UINT cbCaps) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPWAVEOUTCAPSA, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetDevCapsA"));
    if (proc) return proc(uDeviceID, lpCaps, cbCaps);
#endif
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR uDeviceID, LPWAVEOUTCAPSW lpCaps, UINT cbCaps) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPWAVEOUTCAPSW, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetDevCapsW"));
    if (proc) return proc(uDeviceID, lpCaps, cbCaps);
#endif
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutOpen(LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                            DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX,
                                     DWORD_PTR, DWORD_PTR, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutOpen"));
    if (proc) return proc(phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen);
#endif
    (void)phwo; (void)uDeviceID; (void)pwfx; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutClose(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutClose"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutPrepareHeader"));
    if (proc) return proc(hwo, lpWaveHdr, cbWaveHdr);
#endif
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutUnprepareHeader"));
    if (proc) return proc(hwo, lpWaveHdr, cbWaveHdr);
#endif
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutWrite(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutWrite"));
    if (proc) return proc(hwo, lpWaveHdr, cbWaveHdr);
#endif
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutReset(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutReset"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutRestart(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutRestart"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutPause(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutPause"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutBreakLoop"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutGetPosition(HWAVEOUT hwo, LPMMTIME pmmt, UINT cbmmt) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPMMTIME, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetPosition"));
    if (proc) return proc(hwo, pmmt, cbmmt);
#endif
    (void)hwo; (void)pmmt; (void)cbmmt;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutGetVolume(HWAVEOUT hwo, LPDWORD pdwVolume) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPDWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetVolume"));
    if (proc) return proc(hwo, pdwVolume);
#endif
    (void)hwo;
    if (pdwVolume) *pdwVolume = 0xFFFFFFFF;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetVolume(HWAVEOUT hwo, DWORD dwVolume) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutSetVolume"));
    if (proc) return proc(hwo, dwVolume);
#endif
    (void)hwo; (void)dwVolume;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetPitch(HWAVEOUT hwo, LPDWORD pdwPitch) {
    (void)hwo;
    if (pdwPitch) *pdwPitch = 0x00010000;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetPitch(HWAVEOUT hwo, DWORD dwPitch) {
    (void)hwo; (void)dwPitch;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetPlaybackRate(HWAVEOUT hwo, LPDWORD pdwRate) {
    (void)hwo;
    if (pdwRate) *pdwRate = 0x00010000;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetPlaybackRate(HWAVEOUT hwo, DWORD dwRate) {
    (void)hwo; (void)dwRate;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetID(HWAVEOUT hwo, LPUINT puDeviceID) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPUINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetID"));
    if (proc) return proc(hwo, puDeviceID);
#endif
    (void)hwo;
    if (puDeviceID) *puDeviceID = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT mmrError, LPSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const char* msg = "Unknown error";
    size_t len = std::strlen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len);
    lpText[len] = '\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT mmrError, LPWSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const wchar_t* msg = L"Unknown error";
    size_t len = wcslen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len * sizeof(wchar_t));
    lpText[len] = L'\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutMessage(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutMessage"));
    if (proc) return proc(hwo, uMsg, dw1, dw2);
#endif
    (void)hwo; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

// ── Multimedia Timer ───────────────────────────────────────────────────

static LARGE_INTEGER g_timeFreq;
static BOOL g_timeInitialized = FALSE;

DWORD WINAPI timeGetTime(void) {
    if (!g_timeInitialized) {
        QueryPerformanceFrequency(&g_timeFreq);
        g_timeInitialized = TRUE;
    }
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return (DWORD)(cnt.QuadPart * 1000ULL / g_timeFreq.QuadPart);
}

MMRESULT WINAPI timeBeginPeriod(UINT uPeriod) {
    (void)uPeriod;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeEndPeriod(UINT uPeriod) {
    (void)uPeriod;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc) {
    if (!ptc || cbtc < sizeof(TIMECAPS))
        return TIMERR_NOCANDO;
    ptc->wPeriodMin = 1;
    ptc->wPeriodMax = 65535;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeSetEvent(UINT uDelay, UINT uResolution,
                             void (CALLBACK *fptc)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR),
                             DWORD_PTR dwUser, UINT fuEvent) {
    (void)uDelay; (void)uResolution; (void)fptc; (void)dwUser; (void)fuEvent;
    return 0;
}

MMRESULT WINAPI timeKillEvent(UINT uTimerID) {
    (void)uTimerID;
    return TIMERR_NOERROR;
}

} // extern "C"
