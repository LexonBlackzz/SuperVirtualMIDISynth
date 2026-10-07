// KDMAPI / OmniMIDI extension exports.

#include "SVMSDriverInternal.h"


extern "C" {

// ── KDMAPI / OmniMIDI extensions ──────────────────────────────────────

BOOL WINAPI IsKDMAPIAvailable(void) {
    XPBootstrapTrace("[SVMS XP] IsKDMAPIAvailable reached\r\n");
    return TRUE;
}

LPVOID WINAPI InitializeKDMAPIStream(void) {
    const bool wasInitialized = g_kdmapiInitialized.exchange(
        true, std::memory_order_acq_rel);
    if (!EnsureDriverInitialized()) {
        if (!wasInitialized)
            g_kdmapiInitialized.store(false, std::memory_order_release);
        return nullptr;
    }
    return reinterpret_cast<LPVOID>(1);
}

void WINAPI TerminateKDMAPIStream(void) {
    g_kdmapiInitialized.store(false, std::memory_order_release);
    MaybeShutdownDriver();
}

void WINAPI ResetKDMAPIStream(void) {
    if (g_driver) g_driver->ResetAllVoices();
}

UINT WINAPI ReturnKDMAPIVer(LPDWORD pdwMajor, LPDWORD pdwMinor, LPDWORD pdwBuild, LPDWORD pdwRevision) {
    if (pdwMajor) *pdwMajor = 4;
    if (pdwMinor) *pdwMinor = 1;
    if (pdwBuild) *pdwBuild = 0;
    if (pdwRevision) *pdwRevision = 0;
    return TRUE;
}

void WINAPI SendDirectData(DWORD dwMsg) {
    if (!g_kdmapiInitialized.load(std::memory_order_acquire)) return;
    if (g_driver) g_driver->SubmitShortMsg(dwMsg);
}

void WINAPI SendDirectDataNoBuf(DWORD dwMsg) {
    SendDirectData(dwMsg);
}

// Canonical KDMAPI SendCustomEvent carries a BASSMIDI event triple — the
// OmniMIDI header forwards it straight to BASS_MIDI_StreamEvent(eventtype,
// chan, param). The previous facade signature misread the triple as
// (message, pointer, size) and submitted the event *type* itself as a raw
// MIDI message, producing channel garbage on any caller using the documented
// signature. We do not run BASSMIDI, so event types are mapped explicitly
// below; unmapped types are refused cleanly — BASSMIDI semantics also ignore
// invalid events — and never reach the engine as accidental channel messages.
// TODO(v3): BASSMIDI event mapping table (MIDI_EVENT_NOTE/_PROGRAM/_PITCH/
// _BANK/_SYSTEM/_DRUMS/...) once bassmidi.h is vendored for constant
// verification. Until then every event type is reported unsupported.
BOOL WINAPI SendCustomEvent(DWORD eventtype, DWORD chan, DWORD param) {
    (void)eventtype;
    (void)param;
    if (!g_kdmapiInitialized.load(std::memory_order_acquire)) return FALSE;
    if (!g_driver) return FALSE;
    if (chan >= 16u) return FALSE;
    return FALSE;
}

UINT WINAPI SendDirectLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (!g_kdmapiInitialized.load(std::memory_order_acquire) || !g_driver ||
        !HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr) ||
        (!lpMidiHdr->lpData && lpMidiHdr->dwBufferLength != 0u))
        return MMSYSERR_INVALPARAM;
    if (!g_driver->SubmitSystemExclusiveCancellable(
        reinterpret_cast<const uint8_t*>(lpMidiHdr->lpData),
        lpMidiHdr->dwBufferLength, nullptr, 0u)) return MMSYSERR_INVALPARAM;
    lpMidiHdr->dwFlags |= MHDR_DONE;
    lpMidiHdr->dwFlags &= ~MHDR_INQUEUE;
    return MMSYSERR_NOERROR;
}

UINT WINAPI SendDirectLongDataNoBuf(LPSTR data, DWORD size) {
    if (!g_kdmapiInitialized.load(std::memory_order_acquire) || !g_driver ||
        (!data && size != 0u))
        return MMSYSERR_INVALPARAM;
    return g_driver->SubmitSystemExclusiveCancellable(
        reinterpret_cast<const uint8_t*>(data), size, nullptr, 0u)
        ? MMSYSERR_NOERROR : MMSYSERR_INVALPARAM;
}

MMRESULT WINAPI PrepareLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr))
        return MMSYSERR_INVALPARAM;
    lpMidiHdr->dwFlags |= MHDR_PREPARED;
    lpMidiHdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI UnprepareLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr))
        return MMSYSERR_INVALPARAM;
    if ((lpMidiHdr->dwFlags & MHDR_INQUEUE) != 0u)
        return MIDIERR_STILLPLAYING;
    lpMidiHdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

// ── KDMAPI DriverSettings (OmniMIDI DeveloperContent/OmniMIDI.h) ──────
//
// Modes: OM_SET 0x0, OM_GET 0x1, OM_MANAGE 0x2, OM_LEAVE 0x3. Setting ids
// live in the 0x10000+ block. Getters answer from live engine state; live
// sets are not supported yet and report failure instead of pretending —
// OmniMIDI-compatible callers treat FALSE as "setting not available".
namespace {
constexpr DWORD kOmSetMode = 0x0;
constexpr DWORD kOmGetMode = 0x1;
constexpr DWORD kOmManageMode = 0x2;
constexpr DWORD kOmLeaveMode = 0x3;
constexpr DWORD kOmAudioBitDepth = 0x10017;
constexpr DWORD kOmAudioFreq = 0x10018;
constexpr DWORD kOmCurrentEngine = 0x10019;
constexpr DWORD kOmBufferLength = 0x10020;
constexpr DWORD kOmMaxVoices = 0x10026;
// OmniMIDI audio-engine ids: AUDTOWAV 0, DXAUDIO 1, ASIO 2, WASAPI 3.
constexpr DWORD kOmEngineWasapi = 3u;
} // namespace

BOOL WINAPI DriverSettings(DWORD setting, DWORD mode, LPVOID value,
                           UINT cbValue) {
    if (mode == kOmManageMode || mode == kOmLeaveMode) return TRUE;
    if (mode != kOmGetMode || !value) return FALSE;
    if (!g_driver || !g_driver->initialized) return FALSE;
    switch (setting) {
    case kOmAudioFreq:
        if (cbValue < sizeof(DWORD)) return FALSE;
        *static_cast<DWORD*>(value) = g_driver->sampleRate;
        return TRUE;
    case kOmBufferLength:
        if (cbValue < sizeof(DWORD)) return FALSE;
        *static_cast<DWORD*>(value) = g_driver->bufferFrames;
        return TRUE;
    case kOmCurrentEngine:
        if (cbValue < sizeof(DWORD)) return FALSE;
        *static_cast<DWORD*>(value) = kOmEngineWasapi;
        return TRUE;
    case kOmAudioBitDepth:
        // Float pipeline end to end; the int16 store is internal only.
        if (cbValue < sizeof(DWORD)) return FALSE;
        *static_cast<DWORD*>(value) = 32u;
        return TRUE;
    case kOmMaxVoices: {
        if (cbValue < sizeof(DWORD)) return FALSE;
        svms::SnappyVoiceStatistics statistics;
        g_driver->CopyVoiceStatistics(statistics);
        *static_cast<DWORD*>(value) =
            statistics.activeVoices + statistics.freeVoices;
        return TRUE;
    }
    default:
        return FALSE;
    }
}

svms::LegacyDriverDebugInfo* WINAPI GetDriverDebugInfo(void) {
    if (!g_driver) return nullptr;
    return const_cast<svms::LegacyDriverDebugInfo*>(
        g_driver->GetLegacyDebugInfo());
}

FLOAT WINAPI GetRenderingTime(void) {
    return g_driver ? g_driver->GetRenderingTimeMilliseconds() : 0.0f;
}

DWORD WINAPI GetVoiceCount(void) {
    if (!g_driver) return 0u;
    svms::SnappyVoiceStatistics statistics;
    g_driver->CopyVoiceStatistics(statistics);
    return statistics.activeVoices;
}

svms::SnappyVoiceStatistics* WINAPI GetVoiceStatistics(
        svms::SnappyVoiceStatistics* statistics) {
    if (!statistics) return nullptr;
    if (g_driver) {
        g_driver->CopyVoiceStatistics(*statistics);
    } else {
        *statistics = svms::SnappyVoiceStatistics{};
    }
    return statistics;
}

DWORD WINAPI SVMSGetDriverDebugInfoV1(LPVOID pMem, DWORD cbMem) {
    if (!pMem || cbMem < sizeof(svms::DriverDebugInfo) || !g_driver) return 0;
    svms::DriverDebugInfo info;
    g_driver->CopyDebugInfo(info);
    std::memcpy(pMem, &info, sizeof(info));
    return static_cast<DWORD>(sizeof(info));
}

// Canonical KDMAPI passes a single wide-string path (SF2/SFZ/sflist). Our
// engine keeps one active soundfont, so a load replaces the current one —
// acceptable for the facade; sflist layering is not modelled (the engine's
// own soundfont picker covers multi-font setups).
VOID WINAPI LoadCustomSoundFontsList(LPWSTR directory) {
    if (!g_kdmapiInitialized.load(std::memory_order_acquire)) return;
    if (!directory || !*directory) return;
    if (g_driver) (void)g_driver->LoadSoundFont(directory);
}

// KDMAPI contract: timeGetTime semantics — milliseconds since an arbitrary
// epoch — widened to 64 bits so it never rolls over. The split keeps the
// multiply from overflowing on long uptimes with high QPC frequencies.
ULONGLONG WINAPI timeGetTime64(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return static_cast<ULONGLONG>(cnt.QuadPart / freq.QuadPart) * 1000ULL +
           static_cast<ULONGLONG>((cnt.QuadPart % freq.QuadPart) * 1000ULL /
                                  freq.QuadPart);
}

} // extern "C"
