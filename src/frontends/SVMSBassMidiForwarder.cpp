// SVMSBassMidiForwarder.cpp — the bassmidi.dll build.
//
// BASS players resolve BASS_* from bass.dll and BASS_MIDI_* from
// bassmidi.dll. The engine hosts in the bass.dll module (the full SVMS
// runtime, which exports the entire surface); every export in this module
// is a cached forward to the same-named function in bass.dll, so a player
// loading both module names sees exactly one engine instance. If bass.dll
// is not loaded yet, it is loaded from the application directory first
// (the drop-in convention) and the normal search path second.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

typedef DWORD HSTREAM;
typedef DWORD HSOUNDFONT;

namespace {

HMODULE HostModule() {
    HMODULE host = GetModuleHandleW(L"bass.dll");
    if (host) return host;
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) != 0u) {
        std::wstring local(self);
        const size_t slash = local.find_last_of(L"\\/");
        if (slash != std::wstring::npos) local.resize(slash + 1);
        local += L"bass.dll";
        host = LoadLibraryW(local.c_str());
    }
    if (!host) host = LoadLibraryW(L"bass.dll");
    return host;
}

FARPROC Forward(const char* name) {
    const HMODULE host = HostModule();
    return host ? GetProcAddress(host, name) : nullptr;
}

} // namespace

// Each export forwards to the host module's same-named export. The host
// declares these with WINAPI; signatures here must match SVMSDriver.cpp.

extern "C" {
BOOL WINAPI BASS_Init(int device, DWORD freq, DWORD flags,
                                 HWND win, const GUID* clsid) {
    using Fn = BOOL(WINAPI*)(int, DWORD, DWORD, HWND, const GUID*);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_Init"));
    return fn ? fn(device, freq, flags, win, clsid) : FALSE;
}

BOOL WINAPI BASS_Free(void) {
    const auto fn = reinterpret_cast<BOOL(WINAPI*)(void)>(Forward("BASS_Free"));
    return fn ? fn() : FALSE;
}

BOOL WINAPI BASS_SetConfig(DWORD option, DWORD value) {
    const auto fn =
        reinterpret_cast<BOOL(WINAPI*)(DWORD, DWORD)>(Forward("BASS_SetConfig"));
    return fn ? fn(option, value) : FALSE;
}

DWORD WINAPI BASS_GetVersion(void) {
    const auto fn = reinterpret_cast<DWORD(WINAPI*)(void)>(Forward("BASS_GetVersion"));
    return fn ? fn() : 0x02040400u;
}

DWORD WINAPI BASS_GetConfig(DWORD option) {
    const auto fn = reinterpret_cast<DWORD(WINAPI*)(DWORD)>(Forward("BASS_GetConfig"));
    return fn ? fn(option) : 0u;
}

int WINAPI BASS_ErrorGetCode(void) {
    const auto fn = reinterpret_cast<int(WINAPI*)(void)>(Forward("BASS_ErrorGetCode"));
    return fn ? fn() : 0;
}

BOOL WINAPI BASS_MIDI_FontLoad(HSOUNDFONT handle, int preset, int bank) {
    using Fn = BOOL(WINAPI*)(HSOUNDFONT, int, int);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_FontLoad"));
    return fn ? fn(handle, preset, bank) : FALSE;
}

HSOUNDFONT WINAPI BASS_MIDI_FontInit(const void* file, DWORD flags) {
    using Fn = HSOUNDFONT(WINAPI*)(const void*, DWORD);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_FontInit"));
    return fn ? fn(file, flags) : 0u;
}

BOOL WINAPI BASS_MIDI_FontFree(HSOUNDFONT handle) {
    const auto fn =
        reinterpret_cast<BOOL(WINAPI*)(HSOUNDFONT)>(Forward("BASS_MIDI_FontFree"));
    return fn ? fn(handle) : FALSE;
}

BOOL WINAPI BASS_MIDI_StreamSetFonts(HSTREAM handle,
                                                const void* fonts,
                                                DWORD count) {
    using Fn = BOOL(WINAPI*)(HSTREAM, const void*, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamSetFonts"));
    return fn ? fn(handle, fonts, count) : FALSE;
}

HSTREAM WINAPI BASS_MIDI_StreamCreate(DWORD channels, DWORD flags,
                                                 DWORD freq) {
    using Fn = HSTREAM(WINAPI*)(DWORD, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamCreate"));
    return fn ? fn(channels, flags, freq) : 0u;
}

DWORD WINAPI BASS_MIDI_StreamEvents(HSTREAM handle, DWORD mode,
                                               const void* events,
                                               DWORD length) {
    using Fn = DWORD(WINAPI*)(HSTREAM, DWORD, const void*, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamEvents"));
    return fn ? fn(handle, mode, events, length) : 0u;
}

DWORD WINAPI BASS_ChannelGetData(DWORD handle, void* buffer,
                                            DWORD length) {
    using Fn = DWORD(WINAPI*)(DWORD, void*, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelGetData"));
    return fn ? fn(handle, buffer, length) : static_cast<DWORD>(-1);
}

unsigned long long WINAPI BASS_ChannelGetLength(DWORD handle, DWORD mode) {
    using Fn = unsigned long long(WINAPI*)(DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelGetLength"));
    return fn ? fn(handle, mode) : 0u;
}

unsigned long long WINAPI BASS_ChannelGetPosition(DWORD handle, DWORD mode) {
    using Fn = unsigned long long(WINAPI*)(DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelGetPosition"));
    return fn ? fn(handle, mode) : 0u;
}

BOOL WINAPI BASS_ChannelSetPosition(DWORD handle, unsigned long long pos,
                                               DWORD mode) {
    using Fn = BOOL(WINAPI*)(DWORD, unsigned long long, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelSetPosition"));
    return fn ? fn(handle, pos, mode) : FALSE;
}

DWORD WINAPI BASS_ChannelIsActive(DWORD handle) {
    const auto fn =
        reinterpret_cast<DWORD(WINAPI*)(DWORD)>(Forward("BASS_ChannelIsActive"));
    return fn ? fn(handle) : 0u;
}

unsigned long long WINAPI BASS_ChannelSeconds2Bytes(DWORD handle, double seconds) {
    using Fn = unsigned long long(WINAPI*)(DWORD, double);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelSeconds2Bytes"));
    return fn ? fn(handle, seconds) : 0u;
}

double WINAPI BASS_ChannelBytes2Seconds(DWORD handle, unsigned long long pos) {
    using Fn = double(WINAPI*)(DWORD, unsigned long long);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelBytes2Seconds"));
    return fn ? fn(handle, pos) : 0.0;
}

BOOL WINAPI BASS_ChannelSetAttribute(DWORD handle, DWORD attrib,
                                                float value) {
    using Fn = BOOL(WINAPI*)(DWORD, DWORD, float);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelSetAttribute"));
    return fn ? fn(handle, attrib, value) : FALSE;
}

BOOL WINAPI BASS_ChannelGetAttribute(DWORD handle, DWORD attrib,
                                                float* value) {
    using Fn = BOOL(WINAPI*)(DWORD, DWORD, float*);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelGetAttribute"));
    return fn ? fn(handle, attrib, value) : FALSE;
}

BOOL WINAPI BASS_ChannelGetInfo(DWORD handle, void* info) {
    using Fn = BOOL(WINAPI*)(DWORD, void*);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelGetInfo"));
    return fn ? fn(handle, info) : FALSE;
}

DWORD WINAPI BASS_FXReset(DWORD handle) {
    const auto fn = reinterpret_cast<DWORD(WINAPI*)(DWORD)>(Forward("BASS_FXReset"));
    return fn ? fn(handle) : TRUE;
}

DWORD WINAPI BASS_FXFree(DWORD handle) {
    const auto fn = reinterpret_cast<DWORD(WINAPI*)(DWORD)>(Forward("BASS_FXFree"));
    return fn ? fn(handle) : TRUE;
}

DWORD WINAPI BASS_FXSetParameters(DWORD handle, const void* params) {
    using Fn = DWORD(WINAPI*)(DWORD, const void*);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_FXSetParameters"));
    return fn ? fn(handle, params) : TRUE;
}

DWORD WINAPI BASS_FXGetParameters(DWORD handle, void* params) {
    using Fn = DWORD(WINAPI*)(DWORD, void*);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_FXGetParameters"));
    return fn ? fn(handle, params) : TRUE;
}

DWORD WINAPI BASS_FXSetPriority(DWORD handle, int priority) {
    using Fn = DWORD(WINAPI*)(DWORD, int);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_FXSetPriority"));
    return fn ? fn(handle, priority) : TRUE;
}

DWORD WINAPI BASS_FXVersion(void) {
    const auto fn = reinterpret_cast<DWORD(WINAPI*)(void)>(Forward("BASS_FXVersion"));
    return fn ? fn() : 0x02040400u;
}

DWORD WINAPI BASS_ChannelFlags(HSTREAM handle, DWORD flags, DWORD mask) {
    using Fn = DWORD(WINAPI*)(HSTREAM, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_ChannelFlags"));
    return fn ? fn(handle, flags, mask) : 0u;
}

BOOL WINAPI BASS_StreamFree(DWORD handle) {
    const auto fn =
        reinterpret_cast<BOOL(WINAPI*)(DWORD)>(Forward("BASS_StreamFree"));
    return fn ? fn(handle) : FALSE;
}


// ── Extended BASSMIDI surface (StreamEvent singular + coverage) ─────────

DWORD WINAPI BASS_MIDI_StreamEvent(HSTREAM handle, DWORD chan, DWORD type,
                                   DWORD param) {
    using Fn = DWORD(WINAPI*)(HSTREAM, DWORD, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamEvent"));
    return fn ? fn(handle, chan, type, param) : static_cast<DWORD>(-1);
}

DWORD WINAPI BASS_MIDI_StreamGetEvent(HSTREAM handle, DWORD chan, DWORD type) {
    using Fn = DWORD(WINAPI*)(HSTREAM, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamGetEvent"));
    return fn ? fn(handle, chan, type) : static_cast<DWORD>(-1);
}

DWORD WINAPI BASS_MIDI_GetVersion(void) {
    const auto fn =
        reinterpret_cast<DWORD(WINAPI*)(void)>(Forward("BASS_MIDI_GetVersion"));
    return fn ? fn() : 0x02040400u;
}

BOOL WINAPI BASS_MIDI_StreamLoadSamples(HSTREAM handle) {
    using Fn = BOOL(WINAPI*)(HSTREAM);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamLoadSamples"));
    return fn ? fn(handle) : FALSE;
}

BOOL WINAPI BASS_MIDI_StreamSetFilter(HSTREAM handle, BOOL time, float speed) {
    using Fn = BOOL(WINAPI*)(HSTREAM, BOOL, float);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamSetFilter"));
    return fn ? fn(handle, time, speed) : FALSE;
}

DWORD WINAPI BASS_MIDI_FontFlags(HSOUNDFONT handle, DWORD flags, DWORD mask) {
    using Fn = DWORD(WINAPI*)(HSOUNDFONT, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_FontFlags"));
    return fn ? fn(handle, flags, mask) : static_cast<DWORD>(-1);
}

BOOL WINAPI BASS_MIDI_FontSetVolume(HSOUNDFONT handle, float volume) {
    using Fn = BOOL(WINAPI*)(HSOUNDFONT, float);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_FontSetVolume"));
    return fn ? fn(handle, volume) : FALSE;
}

float WINAPI BASS_MIDI_FontGetVolume(HSOUNDFONT handle) {
    using Fn = float(WINAPI*)(HSOUNDFONT);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_FontGetVolume"));
    return fn ? fn(handle) : 0.0f;
}

BOOL WINAPI BASS_MIDI_FontCompact(HSOUNDFONT handle) {
    using Fn = BOOL(WINAPI*)(HSOUNDFONT);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_FontCompact"));
    return fn ? fn(handle) : FALSE;
}

BOOL WINAPI BASS_MIDI_FontUnload(HSOUNDFONT handle, int preset, int bank) {
    using Fn = BOOL(WINAPI*)(HSOUNDFONT, int, int);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_FontUnload"));
    return fn ? fn(handle, preset, bank) : FALSE;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateFile(BOOL mem, const void* file,
                                          unsigned long long offset,
                                          unsigned long long length,
                                          DWORD flags, DWORD freq) {
    using Fn = HSTREAM(WINAPI*)(BOOL, const void*, unsigned long long,
                                unsigned long long, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamCreateFile"));
    return fn ? fn(mem, file, offset, length, flags, freq) : 0u;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateURL(const char* url, DWORD offset,
                                         DWORD flags, DWORD freq) {
    using Fn = HSTREAM(WINAPI*)(const char*, DWORD, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamCreateURL"));
    return fn ? fn(url, offset, flags, freq) : 0u;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateFileUser(DWORD system, DWORD flags,
                                              const void* procs, void* user,
                                              DWORD freq) {
    using Fn = HSTREAM(WINAPI*)(DWORD, DWORD, const void*, void*, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamCreateFileUser"));
    return fn ? fn(system, flags, procs, user, freq) : 0u;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateEvents(const void* events, DWORD ppqn,
                                            DWORD flags, DWORD freq) {
    using Fn = HSTREAM(WINAPI*)(const void*, DWORD, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamCreateEvents"));
    return fn ? fn(events, ppqn, flags, freq) : 0u;
}

DWORD WINAPI BASS_MIDI_ConvertEvents(const void* src, DWORD count, void* dest,
                                     DWORD mode) {
    using Fn = DWORD(WINAPI*)(const void*, DWORD, void*, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_ConvertEvents"));
    return fn ? fn(src, count, dest, mode) : 0u;
}

DWORD WINAPI BASS_MIDI_StreamGetEvents(HSTREAM handle, DWORD chan,
                                       DWORD typefilter, void* events) {
    using Fn = DWORD(WINAPI*)(HSTREAM, DWORD, DWORD, void*);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamGetEvents"));
    return fn ? fn(handle, chan, typefilter, events)
              : static_cast<DWORD>(-1);
}

DWORD WINAPI BASS_MIDI_StreamGetEventsEx(HSTREAM handle, DWORD chan,
                                         DWORD typefilter, void* events,
                                         DWORD start, DWORD count) {
    using Fn = DWORD(WINAPI*)(HSTREAM, DWORD, DWORD, void*, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamGetEventsEx"));
    return fn ? fn(handle, chan, typefilter, events, start, count)
              : static_cast<DWORD>(-1);
}

const void* WINAPI BASS_MIDI_StreamGetMark(HSTREAM handle, DWORD chan,
                                           DWORD type, DWORD index) {
    using Fn = const void*(WINAPI*)(HSTREAM, DWORD, DWORD, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamGetMark"));
    return fn ? fn(handle, chan, type, index) : nullptr;
}

DWORD WINAPI BASS_MIDI_StreamGetMarks(HSTREAM handle, DWORD chan, DWORD type,
                                      void* marks) {
    using Fn = DWORD(WINAPI*)(HSTREAM, DWORD, DWORD, void*);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamGetMarks"));
    return fn ? fn(handle, chan, type, marks) : 0u;
}

BOOL WINAPI BASS_MIDI_StreamGetPreset(HSTREAM handle, DWORD chan,
                                      void* preset) {
    using Fn = BOOL(WINAPI*)(HSTREAM, DWORD, void*);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamGetPreset"));
    return fn ? fn(handle, chan, preset) : FALSE;
}

DWORD WINAPI BASS_MIDI_StreamGetFonts(HSTREAM handle, void* fonts,
                                      DWORD count) {
    using Fn = DWORD(WINAPI*)(HSTREAM, void*, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamGetFonts"));
    return fn ? fn(handle, fonts, count) : 0u;
}

DWORD WINAPI BASS_MIDI_StreamGetChannel(HSTREAM handle, DWORD chan) {
    using Fn = DWORD(WINAPI*)(HSTREAM, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_StreamGetChannel"));
    return fn ? fn(handle, chan) : 0u;
}

HSOUNDFONT WINAPI BASS_MIDI_FontInitUser(const void* procs, void* user,
                                         DWORD flags) {
    using Fn = HSOUNDFONT(WINAPI*)(const void*, void*, DWORD);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_FontInitUser"));
    return fn ? fn(procs, user, flags) : 0u;
}

BOOL WINAPI BASS_MIDI_FontLoadEx(HSOUNDFONT handle, int preset, int bank,
                                 DWORD length) {
    using Fn = BOOL(WINAPI*)(HSOUNDFONT, int, int, DWORD);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_FontLoadEx"));
    return fn ? fn(handle, preset, bank, length) : FALSE;
}

const char* WINAPI BASS_MIDI_FontPack(HSOUNDFONT handle, const char* outfile,
                                      const char* encoder, DWORD flags) {
    using Fn = const char*(WINAPI*)(HSOUNDFONT, const char*, const char*,
                                    DWORD);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_FontPack"));
    return fn ? fn(handle, outfile, encoder, flags) : nullptr;
}

HSOUNDFONT WINAPI BASS_MIDI_FontUnpack(HSOUNDFONT handle, const char* name,
                                       DWORD offset, DWORD length) {
    using Fn = HSOUNDFONT(WINAPI*)(HSOUNDFONT, const char*, DWORD, DWORD);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_FontUnpack"));
    return fn ? fn(handle, name, offset, length) : 0u;
}

BOOL WINAPI BASS_MIDI_FontGetInfo(HSOUNDFONT handle, void* info) {
    using Fn = BOOL(WINAPI*)(HSOUNDFONT, void*);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_FontGetInfo"));
    return fn ? fn(handle, info) : FALSE;
}

BOOL WINAPI BASS_MIDI_FontGetPreset(HSOUNDFONT handle, int preset, int bank) {
    using Fn = BOOL(WINAPI*)(HSOUNDFONT, int, int);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_FontGetPreset"));
    return fn ? fn(handle, preset, bank) : FALSE;
}

DWORD WINAPI BASS_MIDI_FontGetPresets(HSOUNDFONT handle) {
    using Fn = DWORD(WINAPI*)(HSOUNDFONT);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_FontGetPresets"));
    return fn ? fn(handle) : 0u;
}

BOOL WINAPI BASS_MIDI_InInit(DWORD device, const void* proc, void* user) {
    using Fn = BOOL(WINAPI*)(DWORD, const void*, void*);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_InInit"));
    return fn ? fn(device, proc, user) : FALSE;
}

BOOL WINAPI BASS_MIDI_InFree(DWORD device) {
    using Fn = BOOL(WINAPI*)(DWORD);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_InFree"));
    return fn ? fn(device) : FALSE;
}

BOOL WINAPI BASS_MIDI_InStart(DWORD device) {
    using Fn = BOOL(WINAPI*)(DWORD);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_InStart"));
    return fn ? fn(device) : FALSE;
}

BOOL WINAPI BASS_MIDI_InStop(DWORD device) {
    using Fn = BOOL(WINAPI*)(DWORD);
    const auto fn = reinterpret_cast<Fn>(Forward("BASS_MIDI_InStop"));
    return fn ? fn(device) : FALSE;
}

BOOL WINAPI BASS_MIDI_InGetDeviceInfo(DWORD device, void* info) {
    using Fn = BOOL(WINAPI*)(DWORD, void*);
    const auto fn =
        reinterpret_cast<Fn>(Forward("BASS_MIDI_InGetDeviceInfo"));
    return fn ? fn(device, info) : FALSE;
}

} // extern "C"

// The forwarder is linked with /EXPORT entries for every name above (see
// CMakeLists); bass.dll carries the real implementations.
