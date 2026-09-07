#ifndef SVMSAPI_H
#define SVMSAPI_H

// ═══════════════════════════════════════════════════════════════════════
//  SVMSAPI.h — single-header integration for SuperVirtualMIDISynth API
// ═══════════════════════════════════════════════════════════════════════
//
//  PLAYERS: include this header in exactly one source file (it works
//  from C and C++) and call
//  IsKDMAPIAvailable() / InitializeKDMAPIStream() / SendDirectDataNoBuf()
//  exactly like the OmniMIDI KDMAPI header. The DLL is located and bound
//  automatically; there is no import library. Code written against
//  OmniMIDI.h ports by swapping the include — the entry-point names are
//  identical.
//
//    SVMSAPI_Load()   — optional: bind explicitly, check the result
//    SVMSAPI_Unload() — optional: release the module
//
//  By default the host loads SVMSAPI.dll. To drive a different KDMAPI
//  synth (OmniMIDI, or any compatible implementation) define the module
//  name before including:
//
//    #define SVMSAPI_MODULE_NAME L"OmniMIDI\\OmniMIDI.dll"
//    #include "SVMSAPI.h"
//
//  SYNTH AUTHORS: the SVMS-API BACKEND CONTRACT at the bottom of this
//  header defines the C ABI a synthesizer DLL implements so SVMSAPI.dll
//  (or any SVMS-API host) can load it as a MIDI event sink. The host owns
//  event pacing, ordering and shedding; the backend owns synthesis and
//  audio output.
//
//  Binding model: LoadLibrary + GetProcAddress on first use, from the
//  calling thread. Every wrapper is safe to call when no DLL was found —
//  it returns the documented failure value instead of crashing.
// ═══════════════════════════════════════════════════════════════════════

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef SVMSAPI_MODULE_NAME
#define SVMSAPI_MODULE_NAME L"SVMSAPI.dll"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SVMSAPI_NO_CLIENT_BINDERS
/* ── Binding plumbing ──────────────────────────────────────────────── */

typedef BOOL   (WINAPI *SVMSAPI_ReturnKDMAPIVerFn)(LPDWORD, LPDWORD, LPDWORD, LPDWORD);
typedef BOOL   (WINAPI *SVMSAPI_IsAvailableFn)(void);
typedef BOOL   (WINAPI *SVMSAPI_InitStreamFn)(void);
typedef BOOL   (WINAPI *SVMSAPI_TermStreamFn)(void);
typedef VOID   (WINAPI *SVMSAPI_ResetStreamFn)(void);
typedef BOOL   (WINAPI *SVMSAPI_SendCustomEventFn)(DWORD, DWORD, DWORD);
typedef VOID   (WINAPI *SVMSAPI_SendDirectDataFn)(DWORD);
typedef UINT   (WINAPI *SVMSAPI_SendDirectLongDataFn)(MIDIHDR*, UINT);
typedef UINT   (WINAPI *SVMSAPI_SendDirectLongDataNoBufFn)(LPSTR, DWORD);
typedef UINT   (WINAPI *SVMSAPI_PrepareLongDataFn)(MIDIHDR*, UINT);
typedef UINT   (WINAPI *SVMSAPI_UnprepareLongDataFn)(MIDIHDR*, UINT);
typedef BOOL   (WINAPI *SVMSAPI_DriverSettingsFn)(DWORD, DWORD, LPVOID, UINT);
typedef VOID*  (WINAPI *SVMSAPI_GetDebugInfoFn)(void);
typedef VOID   (WINAPI *SVMSAPI_LoadFontsListFn)(LPWSTR);
typedef DWORD64(WINAPI *SVMSAPI_TimeGetTime64Fn)(void);

static HMODULE                        SVMSAPI_module = NULL;
static SVMSAPI_ReturnKDMAPIVerFn      SVMSAPI_ReturnKDMAPIProc = NULL;
static SVMSAPI_IsAvailableFn          SVMSAPI_IsAvailableProc = NULL;
static SVMSAPI_InitStreamFn           SVMSAPI_InitStreamProc = NULL;
static SVMSAPI_TermStreamFn           SVMSAPI_TermStreamProc = NULL;
static SVMSAPI_ResetStreamFn          SVMSAPI_ResetStreamProc = NULL;
static SVMSAPI_SendCustomEventFn      SVMSAPI_SendCustomEventProc = NULL;
static SVMSAPI_SendDirectDataFn       SVMSAPI_SendDirectDataProc = NULL;
static SVMSAPI_SendDirectDataFn       SVMSAPI_SendDirectDataNoBufProc = NULL;
static SVMSAPI_SendDirectLongDataFn   SVMSAPI_SendDirectLongDataProc = NULL;
static SVMSAPI_SendDirectLongDataNoBufFn SVMSAPI_SendDirectLongDataNoBufProc = NULL;
static SVMSAPI_PrepareLongDataFn      SVMSAPI_PrepareLongDataProc = NULL;
static SVMSAPI_UnprepareLongDataFn    SVMSAPI_UnprepareLongDataProc = NULL;
static SVMSAPI_DriverSettingsFn       SVMSAPI_DriverSettingsProc = NULL;
static SVMSAPI_GetDebugInfoFn         SVMSAPI_GetDebugInfoProc = NULL;
static SVMSAPI_LoadFontsListFn        SVMSAPI_LoadFontsListProc = NULL;
static SVMSAPI_TimeGetTime64Fn        SVMSAPI_TimeGetTime64Proc = NULL;

/* Loads and binds the synth DLL. Returns TRUE when every core export was
 * found. Safe to call repeatedly; the module is only loaded once. */
static BOOL SVMSAPI_Load(void) {
    if (SVMSAPI_module) return TRUE;
    const HMODULE module = LoadLibraryW(SVMSAPI_MODULE_NAME);
    if (!module) return FALSE;
    const FARPROC ok = GetProcAddress(module, "IsKDMAPIAvailable");
    if (!ok) { FreeLibrary(module); return FALSE; }
    SVMSAPI_ReturnKDMAPIProc = (SVMSAPI_ReturnKDMAPIVerFn)(void*)GetProcAddress(module, "ReturnKDMAPIVer");
    SVMSAPI_IsAvailableProc = (SVMSAPI_IsAvailableFn)(void*)GetProcAddress(module, "IsKDMAPIAvailable");
    SVMSAPI_InitStreamProc = (SVMSAPI_InitStreamFn)(void*)GetProcAddress(module, "InitializeKDMAPIStream");
    SVMSAPI_TermStreamProc = (SVMSAPI_TermStreamFn)(void*)GetProcAddress(module, "TerminateKDMAPIStream");
    SVMSAPI_ResetStreamProc = (SVMSAPI_ResetStreamFn)(void*)GetProcAddress(module, "ResetKDMAPIStream");
    SVMSAPI_SendCustomEventProc = (SVMSAPI_SendCustomEventFn)(void*)GetProcAddress(module, "SendCustomEvent");
    SVMSAPI_SendDirectDataProc = (SVMSAPI_SendDirectDataFn)(void*)GetProcAddress(module, "SendDirectData");
    SVMSAPI_SendDirectDataNoBufProc = (SVMSAPI_SendDirectDataNoBufFn)(void*)GetProcAddress(module, "SendDirectDataNoBuf");
    SVMSAPI_SendDirectLongDataProc = (SVMSAPI_SendDirectLongDataFn)(void*)GetProcAddress(module, "SendDirectLongData");
    SVMSAPI_SendDirectLongDataNoBufProc = (SVMSAPI_SendDirectLongDataNoBufFn)(void*)GetProcAddress(module, "SendDirectLongDataNoBuf");
    SVMSAPI_PrepareLongDataProc = (SVMSAPI_PrepareLongDataFn)(void*)GetProcAddress(module, "PrepareLongData");
    SVMSAPI_UnprepareLongDataProc = (SVMSAPI_UnprepareLongDataFn)(void*)GetProcAddress(module, "UnprepareLongData");
    SVMSAPI_DriverSettingsProc = (SVMSAPI_DriverSettingsFn)(void*)GetProcAddress(module, "DriverSettings");
    SVMSAPI_GetDebugInfoProc = (SVMSAPI_GetDebugInfoFn)(void*)GetProcAddress(module, "GetDriverDebugInfo");
    SVMSAPI_LoadFontsListProc = (SVMSAPI_LoadFontsListFn)(void*)GetProcAddress(module, "LoadCustomSoundFontsList");
    SVMSAPI_TimeGetTime64Proc = (SVMSAPI_TimeGetTime64Fn)(void*)GetProcAddress(module, "timeGetTime64");
    SVMSAPI_module = module;
    return TRUE;
}

static void SVMSAPI_Unload(void) {
    if (!SVMSAPI_module) return;
    FreeLibrary(SVMSAPI_module);
    SVMSAPI_module = NULL;
    SVMSAPI_ReturnKDMAPIProc = NULL;
    SVMSAPI_IsAvailableProc = NULL;
    SVMSAPI_InitStreamProc = NULL;
    SVMSAPI_TermStreamProc = NULL;
    SVMSAPI_ResetStreamProc = NULL;
    SVMSAPI_SendCustomEventProc = NULL;
    SVMSAPI_SendDirectDataProc = NULL;
    SVMSAPI_SendDirectDataNoBufProc = NULL;
    SVMSAPI_SendDirectLongDataProc = NULL;
    SVMSAPI_SendDirectLongDataNoBufProc = NULL;
    SVMSAPI_PrepareLongDataProc = NULL;
    SVMSAPI_UnprepareLongDataProc = NULL;
    SVMSAPI_DriverSettingsProc = NULL;
    SVMSAPI_GetDebugInfoProc = NULL;
    SVMSAPI_LoadFontsListProc = NULL;
    SVMSAPI_TimeGetTime64Proc = NULL;
    SVMSAPI_GetVoiceCountProc = NULL;
    SVMSAPI_GetVoiceStatisticsProc = NULL;
    SVMSAPI_GetRenderingTimeProc = NULL;
}

/* ── KDMAPI-compatible entry points (names match OmniMIDI.h) ───────── */

static BOOL IsKDMAPIAvailable(void) {
    return SVMSAPI_Load() && SVMSAPI_IsAvailableProc
        ? SVMSAPI_IsAvailableProc() : FALSE;
}

static BOOL InitializeKDMAPIStream(void) {
    return SVMSAPI_Load() && SVMSAPI_InitStreamProc
        ? SVMSAPI_InitStreamProc() : FALSE;
}

static BOOL TerminateKDMAPIStream(void) {
    return SVMSAPI_TermStreamProc ? SVMSAPI_TermStreamProc() : FALSE;
}

static VOID ResetKDMAPIStream(void) {
    if (SVMSAPI_ResetStreamProc) SVMSAPI_ResetStreamProc();
}

static BOOL ReturnKDMAPIVer(LPDWORD major, LPDWORD minor,
                            LPDWORD build, LPDWORD revision) {
    return SVMSAPI_ReturnKDMAPIProc
        ? SVMSAPI_ReturnKDMAPIProc(major, minor, build, revision) : FALSE;
}

static VOID SendDirectData(DWORD dwMsg) {
    if (SVMSAPI_Load() && SVMSAPI_SendDirectDataProc)
        SVMSAPI_SendDirectDataProc(dwMsg);
}

static VOID SendDirectDataNoBuf(DWORD dwMsg) {
    if (SVMSAPI_Load() && SVMSAPI_SendDirectDataNoBufProc)
        SVMSAPI_SendDirectDataNoBufProc(dwMsg);
}

static BOOL SendCustomEvent(DWORD eventtype, DWORD chan, DWORD param) {
    return SVMSAPI_SendCustomEventProc
        ? SVMSAPI_SendCustomEventProc(eventtype, chan, param) : FALSE;
}

static UINT SendDirectLongData(MIDIHDR* header, UINT headerSize) {
    return SVMSAPI_SendDirectLongDataProc
        ? SVMSAPI_SendDirectLongDataProc(header, headerSize)
        : MMSYSERR_INVALPARAM;
}

static UINT SendDirectLongDataNoBuf(LPSTR data, DWORD size) {
    return SVMSAPI_SendDirectLongDataNoBufProc
        ? SVMSAPI_SendDirectLongDataNoBufProc(data, size)
        : MMSYSERR_INVALPARAM;
}

static UINT PrepareLongData(MIDIHDR* header, UINT headerSize) {
    return SVMSAPI_PrepareLongDataProc
        ? SVMSAPI_PrepareLongDataProc(header, headerSize)
        : MMSYSERR_INVALPARAM;
}

static UINT UnprepareLongData(MIDIHDR* header, UINT headerSize) {
    return SVMSAPI_UnprepareLongDataProc
        ? SVMSAPI_UnprepareLongDataProc(header, headerSize)
        : MMSYSERR_INVALPARAM;
}

static BOOL DriverSettings(DWORD setting, DWORD mode, LPVOID value,
                           UINT cbValue) {
    return SVMSAPI_DriverSettingsProc
        ? SVMSAPI_DriverSettingsProc(setting, mode, value, cbValue) : FALSE;
}

/* Opaque handle: the debug-info layout is implementation specific
 * (OmniMIDI, SVMS and other KDMAPI synths differ). */
static VOID* GetDriverDebugInfo(void) {
    return SVMSAPI_GetDebugInfoProc ? SVMSAPI_GetDebugInfoProc() : nullptr;
}

static VOID LoadCustomSoundFontsList(LPWSTR directory) {
    if (SVMSAPI_LoadFontsListProc) SVMSAPI_LoadFontsListProc(directory);
}

static DWORD64 timeGetTime64(void) {
    return SVMSAPI_TimeGetTime64Proc ? SVMSAPI_TimeGetTime64Proc() : 0u;
}

#endif /* SVMSAPI_NO_CLIENT_BINDERS */

/* ═══════════════════════════════════════════════════════════════════════
 *  SVMS-API BACKEND CONTRACT v1
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  A synthesizer that implements this interface ships as a DLL exporting
 *  one symbol, SVMSBackend_GetInterface. Any SVMS-API host (SVMSAPI.dll
 *  today) can then load it as a MIDI event sink: the host owns event
 *  pacing (ordering, throttling to playback time, shedding, CC collapse),
 *  the backend owns synthesis and audio output. This mirrors how KDMAPI
 *  synths plug into players — except the host, not the player, does the
 *  pipeline work, so a minimal backend only needs to consume MIDI and
 *  make sound.
 *
 *  Contract rules:
 *   * The host calls SVMSBackend_GetInterface once, with out->struct_size
 *     preset to sizeof(SVMSBackendInterface). The backend fills the table
 *     and returns 0, or returns non-zero to refuse (wrong struct_size).
 *   * initialize() is called once before any send and returns 0 on
 *     success. reset() is a hard all-notes-off; shutdown() after the
 *     final send.
 *   * All functions are called from one host thread at a time. Backends
 *     may dispatch internally to their own audio thread.
 *   * send_short consumes a packed short MIDI message 0x00sskkvv (status
 *     byte, data1, data2). It must not block: the host may deliver
 *     hundreds of thousands of messages per second. Returning non-zero
 *     means the message was consumed; the host does not retry.
 *   * send_short_batch (optional, advertise SVMSBACKEND_CAP_BATCH) is a
 *     best-effort ordered batch of packed short messages. The host uses
 *     it only when advertised; otherwise it loops send_short.
 *   * System exclusive (long) data and master-level controls are not part
 *     of v1 and are not delivered.
 */

#define SVMSBACKEND_API_VERSION 1u

/* Capability flags for SVMSBackendInterface::capabilities. */
#define SVMSBACKEND_CAP_BATCH 0x00000001u

typedef struct SVMSBackendOpenParams {
    unsigned int struct_size;   /* set to sizeof(SVMSBackendOpenParams)    */
    unsigned int sample_rate;   /* host playback rate, informational       */
    unsigned int buffer_frames; /* host dispatch granularity, frames       */
    unsigned int voices_hint;   /* suggested polyphony, 0 = backend choice */
} SVMSBackendOpenParams;

typedef struct SVMSBackendInterface {
    unsigned int struct_size;   /* host presets sizeof(*this)              */
    unsigned int api_version;   /* backend sets SVMSBACKEND_API_VERSION    */
    unsigned int capabilities;  /* SVMSBACKEND_CAP_* bitmask               */
    void* user;                 /* backend-private, passed back verbatim   */

    int (*initialize)(void* user, const SVMSBackendOpenParams* params);
    void (*shutdown)(void* user);
    void (*reset)(void* user);
    unsigned int (*send_short)(void* user, unsigned int msg);
    unsigned int (*send_short_batch)(void* user, const unsigned int* msgs,
                                     unsigned int count);
} SVMSBackendInterface;

typedef unsigned int (*SVMSBackendGetInterfaceFn)(SVMSBackendInterface* out);

#define SVMSBACKEND_GETINTERFACE_NAME "SVMSBackend_GetInterface"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SVMSAPI_H */
