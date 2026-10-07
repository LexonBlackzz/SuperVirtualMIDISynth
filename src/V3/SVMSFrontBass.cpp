// BASS/BASSMIDI prerender shim exports (C++ linkage, exported by name).

#include "SVMSDriverInternal.h"


// ── BASS/BASSMIDI prerender shim ─────────────────────────────────────
// The subset of the BASS/BASSMIDI surface that MIDI players drive for
// faster-than-realtime prerendering (surface extracted from the BPFA and
// PGFA decompiles; constants from BassMIDI/bassmidi.h 2.4). Streams are
// SVMS offline sessions: events submitted via BASS_MIDI_StreamEvents land
// on their exact frames, BASS_ChannelGetData renders forward as fast as
// the caller pulls. No audio device is opened.
//
// Module layout: the engine hosts in the module that is loaded as
// bass.dll; the bassmidi.dll build is a pure forwarder
// (SVMSBassMidiForwarder.cpp) so a player loading both module names sees
// one engine instance.

typedef DWORD HSTREAM;
typedef DWORD HSOUNDFONT;

namespace {

struct BassFontMapEntry {
    uint32_t font = 0u;      // BASS_MIDI_FontInit handle
    int32_t spreset = -1;
    int32_t sbank = -1;
    int32_t dpreset = -1;
    int32_t dbank = -1;
    int32_t dbanklsb = -1;
};

struct BassStream {
    SVMS_Session session = 0;
    uint32_t sampleRate = 44100u;
    uint32_t channels = 2u;
    bool floating = true;      // BASS_SAMPLE_FLOAT output; else 16-bit
    uint64_t renderedFrames = 0u;  // frames rendered into the cache
    uint64_t servedFrames = 0u;    // frames handed to the caller
    uint64_t maxEventFrame = 0u;
    uint32_t maxBlockFrames = 2048u;  // NativeRenderOffline per-call cap
    uint32_t voiceLimit = 4096u;      // BASS_ATTRIB_MIDI_VOICES -> session pool
    std::wstring fontPath;            // font materialized into the session
    std::vector<BassFontMapEntry> fontConfig;  // per-stream FONTEX list
    // A positionless (SYNC) event means the caller is a realtime pump:
    // events must land at the consumption cursor, so the render-ahead
    // cache is capped to the pull size for such streams.
    bool syncAnchored = false;
    // A TIME-anchored event marks the caller as a batch pumper: full
    // render-ahead caching is safe from then on (see BASS_ChannelGetData).
    bool sawTimeEvents = false;
    // Render-ahead cache: interleaved frames in [cacheStart, cacheEnd),
    // refilled in max_block_frames-bounded engine chunks and served to
    // pull-sized requests (see BASS_ChannelGetData).
    std::vector<float> cache;
    uint32_t cacheStart = 0u;
    uint32_t cacheEnd = 0u;
    std::vector<float> scratchLeft;   // planar render targets, maxBlockFrames
    std::vector<float> scratchRight;
    // Last-set parameter per (channel<<24 | type), for StreamGetEvent
    // queries (set by StreamEvent and struct-mode StreamEvents).
    std::unordered_map<uint32_t, uint32_t> eventState;
    // Wall-clock tick of the last accepted event submission (GetTickCount64
    // ms). A live pump keeps the stream alive even when the pull cursor
    // reaches the submitted-events horizon; see BASS_ChannelGetData.
    uint64_t lastSubmitTickMs = 0u;
    // Events accumulated from BASS_MIDI_StreamEvents, sorted by frame at
    // render time and consumed as the pull cursor passes them.
    std::vector<SVMS_OfflineEvent> pending;
    // Per-stream pending events whose frame_offset landed beyond the
    // current render_offline call's window stay here with ABSOLUTE frames;
    // they are re-based at the next pull.
};

// Soundfont slots from BASS_MIDI_FontInit. Handles are 1-based indices into
// this vector and are never reused (matching BASS's handle semantics).
struct BassFont {
    std::wstring path;
    DWORD flags = 0u;      // BASS_MIDI_FONT_XGDRUMS etc.
    bool freed = false;
};

std::mutex g_bassMutex;
std::vector<std::unique_ptr<BassStream>> g_bassStreams;
std::vector<BassFont> g_bassFonts;
// Default soundfont configuration (BASS_MIDI_StreamSetFonts with handle 0):
// applied by subsequently created streams. Earlier entries have priority
// (BASSMIDI rule, Bass.Net.xml StreamSetFonts docs).
std::vector<BassFontMapEntry> g_bassDefaultFonts;
uint32_t g_bassInitRate = 44100u;
int g_bassLastError = 0;   // BASS_ERROR codes: 0 = BASS_OK

const BassFont* BassFontResolve(uint32_t handle) {
    if (handle == 0u || handle > g_bassFonts.size()) return nullptr;
    const BassFont& font = g_bassFonts[handle - 1u];
    if (font.freed || font.path.empty()) return nullptr;
    return &font;
}

// The soundfont list's first valid entry provides the session's font: the
// native offline-session API takes a single path, so stacked-font fallback
// (later entries) is not represented — the priority rule (earlier wins) is.
const wchar_t* BassResolveFontPath(
    const std::vector<BassFontMapEntry>& config) {
    for (const BassFontMapEntry& entry : config) {
        const BassFont* font = BassFontResolve(entry.font);
        if (font) return font->path.c_str();
    }
    return nullptr;
}

// Offline session configuration for BASSMIDI decode streams. Threads/backend
// are engine defaults (0 = auto hardware-concurrency up to 16, AUTO backend),
// and the limiter is OFF: real BASSMIDI hands raw float samples to the
// player, and prerender hosts (Kiva) apply their own limiter on top —
// double limiting produced pumping/"trash" output.
void BassBuildOfflineConfig(SVMS_OfflineSessionConfig* config,
                            const BassStream& stream, uint32_t maxVoices) {
    config->struct_size = sizeof(*config);
    config->struct_version = SVMS_STRUCT_VERSION_1;
    config->session_kind = SVMS_SESSION_OFFLINE_RENDER;
    config->sample_rate = stream.sampleRate;
    config->max_voices = maxVoices;
    config->render_threads = 0u;
    // Diagnostic override: pin the offline render thread count to compare
    // parallel scaling (e.g. SVMS_BASS_THREADS=1 vs auto).
    if (const char* threads = std::getenv("SVMS_BASS_THREADS")) {
        const int parsed = std::atoi(threads);
        if (parsed > 0 && parsed <= 64) config->render_threads = (uint32_t)parsed;
    }
    config->max_block_frames = 65536u;
    config->render_backend = SVMS_RENDER_BACKEND_AUTO;
    config->limiter_enabled = 0u;
    config->limiter_algorithm = SVMS_LIMITER_CLASSIC;
    config->master_volume = 1.0f;
    config->limiter_threshold = 0.95f;
    config->limiter_lookahead_ms = 3.0f;
    config->limiter_attack_ms = 0.5f;
    config->limiter_release_ms = 100.0f;
}

// (Re)create the session behind a stream with the given font and voice
// limit. Only valid while the pull cursor is still at frame 0; pending
// events carry absolute frames and survive the swap. Returns false (stream
// unchanged) if the new session cannot be created.
bool BassApplyStreamFont(BassStream& stream, const std::wstring& fontPath,
                         uint32_t maxVoices) {
    SVMS_OfflineSessionConfig config{};
    BassBuildOfflineConfig(&config, stream, maxVoices);
    const int utf8Length = WideCharToMultiByte(
        CP_UTF8, 0, fontPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string fontUtf8(utf8Length > 0 ? utf8Length - 1 : 0, '\0');
    if (utf8Length > 1)
        WideCharToMultiByte(CP_UTF8, 0, fontPath.c_str(), -1,
                            fontUtf8.data(), utf8Length, nullptr, nullptr);
    SVMS_Session session = 0u;
    if (NativeCreateOfflineSession(&config, fontUtf8.c_str(), &session) !=
            SVMS_RESULT_OK ||
        session == 0u) {
        return false;
    }
    if (stream.session) NativeDestroySession(stream.session);
    stream.session = session;
    stream.fontPath = fontPath;
    stream.voiceLimit = maxVoices;
    stream.maxBlockFrames = config.max_block_frames;
    stream.renderedFrames = 0u;
    stream.servedFrames = 0u;
    stream.cache.clear();
    stream.cacheStart = stream.cacheEnd = 0u;
    stream.scratchLeft.assign(config.max_block_frames, 0.0f);
    stream.scratchRight.assign(config.max_block_frames, 0.0f);
    return true;
}

BassStream* BassStreamResolve(DWORD handle) {
    if (handle == 0u || handle > g_bassStreams.size()) return nullptr;
    return g_bassStreams[handle - 1u].get();
}

// File-only diagnostic channel for the BASS shim. Deliberately independent
// of OutputDebugString: DebugView dies on certain debug streams emitted in
// OmniMIDI's presence, and this log must survive that. Appends to
// %TEMP%\svms_bass.log; safe to leave enabled.
// File-only diagnostic channel for the BASS shim. Deliberately independent
// of OutputDebugString: DebugView dies on certain debug streams emitted in
// OmniMIDI's presence, and this log must survive that. Appends to
// %TEMP%\svms_bass.log — RATE LIMITED: BassLog drops lines inside a 1 s
// window, BassLogNow bypasses the throttle for rare lifecycle lines, and
// the file self-truncates past 4 MB so a long session can never balloon it.
void BassLogWrite(const char* fmt, va_list args) {
    static char logPath[MAX_PATH];
    static bool pathReady = false;
    if (!pathReady) {
        const UINT n = GetTempPathA(MAX_PATH, logPath);
        if (n == 0 || n + 16 >= MAX_PATH) return;
        lstrcatA(logPath, "svms_bass.log");
        pathReady = true;
    }
    DWORD openFlags = FILE_APPEND_DATA;
    // Cap the file: past 4 MB, start a fresh log.
    HANDLE probe = CreateFileA(logPath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(probe, nullptr);
        CloseHandle(probe);
        if (size != INVALID_FILE_SIZE && size > 4u * 1024u * 1024u)
            openFlags = GENERIC_WRITE;  // + CREATE_ALWAYS below: truncate
    }
    HANDLE f = CreateFileA(logPath, openFlags,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr,
                           openFlags == GENERIC_WRITE ? CREATE_ALWAYS
                                                      : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[512];
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
    const size_t len = strnlen(line, sizeof(line));
    DWORD written = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[40];
    _snprintf_s(stamp, sizeof(stamp), _TRUNCATE,
                "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);
    WriteFile(f, stamp, lstrlenA(stamp), &written, nullptr);
    WriteFile(f, line, static_cast<DWORD>(len), &written, nullptr);
    WriteFile(f, "\r\n", 2, &written, nullptr);
    CloseHandle(f);
}

void BassLogNow(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    BassLogWrite(fmt, args);
    va_end(args);
}

void BassLog(const char* fmt, ...) {
    // At most one line per second: diagnostics, not a firehose.
    static LARGE_INTEGER lastEmit{};
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (lastEmit.QuadPart != 0 &&
        (now.QuadPart - lastEmit.QuadPart) * 1000 < frequency.QuadPart)
        return;
    lastEmit = now;
    va_list args;
    va_start(args, fmt);
    BassLogWrite(fmt, args);
    va_end(args);
}

SVMS_OfflineEvent BassPackEvent(uint32_t frame, uint32_t message) {
    SVMS_OfflineEvent ev{};
    ev.frame_offset = frame;
    ev.packed_message = message;
    return ev;
}

// BASSMIDI event type → packed short message(s). Returns false for types the
// shim does not synthesize (they are counted as consumed, not errors —
// BASSMIDI itself ignores unknown types).
//
// Type numbers verified by reflecting Bass.Net.dll's BASSMIDIEvent enum
// (2026-09-11): NOTE=1, PROGRAM=2, CHANPRES=3, PITCH=4, PITCHRANGE=5,
// DRUMS=6, FINETUNE=7, COARSETUNE=8, MASTERVOL=9, BANK=10, MODULATION=11,
// VOLUME=12, PAN=13, EXPRESSION=14, SUSTAIN=15, SOUNDOFF=16, RESET=17,
// NOTESOFF=18, PORTAMENTO=19, PORTATIME=20, ..., REVERB=23, CHORUS=24,
// CUTOFF=25, RESONANCE=26, ..., DRUM_*=50..58, SYSTEM=61, TEMPO=62, ...,
// BANK_LSB=70, KEYPRES=71, ..., SOSTENUTO=76, ... (the previous table mapped
// 16/17 to CC91/CC93 — SOUNDOFF/RESET were being played as reverb/chorus).
// Only engine-implemented messages are emitted; RPN types (5/7/8), reverb /
// chorus sends (23/24), and per-note controllers have no SVMS channel
// mapping (CC6/38/71/74/91/93 are unmapped) and are consumed as no-ops.
bool BassTranslateMidiEvent(DWORD type, DWORD param, DWORD chan,
                            uint32_t& out) {
    if (chan >= 16u) return false;
    switch (type) {
        case 1u:  // MIDI_EVENT_NOTE: param lo = note, hi = velocity
            out = ((param & 0xff00u) != 0u ? 0x90u : 0x80u) | chan |
                  ((param & 0xffu) << 8u) | (((param >> 8u) & 0xffu) << 16u);
            return true;
        case 2u:  // MIDI_EVENT_PROGRAM
            out = 0xC0u | chan | ((param & 0xffu) << 8u);
            return true;
        case 3u:  // MIDI_EVENT_CHANPRES
            out = 0xD0u | chan | ((param & 0xffu) << 8u);
            return true;
        case 4u:  // MIDI_EVENT_PITCH: 14-bit in param
            out = 0xE0u | chan | ((param & 0x7fu) << 8u) |
                  (((param >> 7u) & 0x7fu) << 16u);
            return true;
        case 10u:  // MIDI_EVENT_BANK → CC0 (bank select coarse)
            out = 0xB0u | chan | (0u << 8u) | ((param & 0xffu) << 16u);
            return true;
        case 11u:  // MIDI_EVENT_MODULATION → CC1
        case 12u:  // MIDI_EVENT_VOLUME → CC7
        case 13u:  // MIDI_EVENT_PAN → CC10
        case 14u:  // MIDI_EVENT_EXPRESSION → CC11
        case 15u:  // MIDI_EVENT_SUSTAIN → CC64
        {
            static const uint32_t kCcMap[] = {1u, 7u, 10u, 11u, 64u};
            out = 0xB0u | chan |
                  (kCcMap[type - 11u] << 8u) | ((param & 0xffu) << 16u);
            return true;
        }
        case 16u:  // MIDI_EVENT_SOUNDOFF → CC120 (all sound off)
            out = 0xB0u | chan | (120u << 8u) | ((param & 0xffu) << 16u);
            return true;
        case 17u:  // MIDI_EVENT_RESET → CC121 (reset all controllers)
            out = 0xB0u | chan | (121u << 8u) | ((param & 0xffu) << 16u);
            return true;
        case 18u:  // MIDI_EVENT_NOTESOFF → CC123 (all notes off)
            out = 0xB0u | chan | (123u << 8u) | ((param & 0xffu) << 16u);
            return true;
        case 70u:  // MIDI_EVENT_BANK_LSB → CC32 (bank select fine)
            out = 0xB0u | chan | (32u << 8u) | ((param & 0xffu) << 16u);
            return true;
        case 71u:  // MIDI_EVENT_KEYPRES → 0xA0 poly aftertouch:
                   // param = (key << 16) | pressure (Bass.Net StreamGetEvent:
                   // the key rides in the HIWORD for per-note events)
            out = 0xA0u | chan | (((param >> 16u) & 0xffu) << 8u) |
                  ((param & 0xffu) << 16u);
            return true;
        case 76u:  // MIDI_EVENT_SOSTENUTO → CC66
            out = 0xB0u | chan | (66u << 8u) | ((param & 0xffu) << 16u);
            return true;
        default:
            return false;  // RPN/reverb/chorus/per-note CCs: engine no-ops
    }
}

} // namespace

// Wall-clock ms for the live-pump quiescence check. GetTickCount64 is
// Vista+; XP falls back to the 32-bit tick (wraps at 49.7 days — harmless
// for a recency comparison).
uint64_t BassNowTickMs() {
#if defined(SVMS_XP_COMPAT)
    return static_cast<uint64_t>(GetTickCount());
#else
    return GetTickCount64();
#endif
}

BOOL WINAPI BASS_Init(int device, DWORD freq, DWORD flags, HWND win,
                      const GUID* clsid) {
    (void)device; (void)flags; (void)win; (void)clsid;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (freq != 0u) g_bassInitRate = freq;
    g_bassLastError = 0;
    return TRUE;
}

BOOL WINAPI BASS_Free(void) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    for (auto& stream : g_bassStreams) {
        if (stream && stream->session)
            NativeDestroySession(stream->session);
    }
    g_bassStreams.clear();
    g_bassLastError = 0;
    return TRUE;
}

BOOL WINAPI BASS_SetConfig(DWORD option, DWORD value) {
    (void)option; (void)value;
    return TRUE;
}

DWORD WINAPI BASS_GetConfig(DWORD option) {
    (void)option;
    return 0u;
}

DWORD WINAPI BASS_GetVersion(void) {
    // 2.4.4 - .NET BASS wrappers version-check before their first call.
    return 0x02040400u;
}

int WINAPI BASS_ErrorGetCode(void) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    return g_bassLastError;
}

HSOUNDFONT WINAPI BASS_MIDI_FontInit(const void* file, DWORD flags) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (!file) {
        g_bassLastError = 20;  // BASS_ERROR_ILLPARAM
        return 0u;
    }
    constexpr DWORD kBassUnicode = 0x40000000u;
    const char* utf8 = reinterpret_cast<const char*>(file);
    // Callers differ on pairing UTF-16 strings with the BASS_UNICODE flag
    // (BASS.NET marshals .NET strings as UTF-16); an ASCII-shaped UTF-16
    // buffer has a NUL in every second byte, so accept both conventions.
    const bool wide =
        (flags & kBassUnicode) != 0u || (utf8[0] != 0u && utf8[1] == 0u);
    BassFont font;
    font.flags = flags;
    if (wide) {
        font.path = reinterpret_cast<const wchar_t*>(file);
    } else {
        const int wideLength = MultiByteToWideChar(
            CP_UTF8, 0, utf8, -1, nullptr, 0);
        std::wstring converted(wideLength > 0 ? wideLength - 1 : 0, L'\0');
        if (wideLength > 1)
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, converted.data(),
                                wideLength);
        font.path = converted;
    }
    const bool valid = !font.path.empty();
    const uint32_t handle = valid ? static_cast<uint32_t>(g_bassFonts.size() + 1u) : 0u;
    if (valid) {
        BassLogNow("FontInit: %s path='%ls' flags=%#X -> handle=%u",
                wide ? "utf16" : "utf8", font.path.c_str(), flags, handle);
        g_bassFonts.push_back(std::move(font));
    } else {
        BassLogNow("FontInit: %s path=<empty> flags=%#X -> failed",
                wide ? "utf16" : "utf8", flags);
    }
    g_bassLastError = valid ? 0 : 2;  // BASS_ERROR_FILEOPEN
    return valid ? handle : 0u;
}

BOOL WINAPI BASS_MIDI_FontLoad(HSOUNDFONT handle, int preset, int bank) {
    (void)preset; (void)bank;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (!BassFontResolve(handle)) {
        g_bassLastError = 5;  // BASS_ERROR_HANDLE
        return FALSE;
    }
    // The full font materializes when the session is created; per-preset
    // load accounting is not represented.
    return TRUE;
}

BOOL WINAPI BASS_MIDI_FontFree(HSOUNDFONT handle) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (handle == 0u || handle > g_bassFonts.size()) {
        g_bassLastError = 5;  // BASS_ERROR_HANDLE
        return FALSE;
    }
    g_bassFonts[handle - 1u].freed = true;
    return TRUE;
}

BOOL WINAPI BASS_MIDI_StreamSetFonts(HSTREAM handle, const void* fonts,
                                     DWORD count) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (!fonts && (count & 0xffffffu) != 0u) {
        g_bassLastError = 20;  // BASS_ERROR_ILLPARAM
        return FALSE;
    }
    // The count parameter doubles as the entry-format carrier (verified by
    // disassembling BASS_MIDI_StreamSetFonts' copy helper and Bass.Net's
    // wrapper IL, which ORs 0x1000000 into count for FONTEX arrays):
    //   count & 0x1000000 -> BASS_MIDI_FONTEX  (6 x 32-bit, 24 bytes)
    //   count & 0x2000000 -> FONTEX with a 64-bit font field (unsupported)
    //   otherwise         -> BASS_MIDI_FONT    (font, preset, bank; 12 bytes)
    const DWORD format = count & 0x3000000u;
    const DWORD entries = count & 0xffffffu;
    if (format == 0x2000000u) {
        BassLog("StreamSetFonts(handle=%u): 64-bit-font variant unsupported",
                handle);
        g_bassLastError = 20;
        return FALSE;
    }
    std::vector<BassFontMapEntry> list(entries);
    for (DWORD i = 0u; i < entries; ++i) {
        BassFontMapEntry& entry = list[i];
        if (format == 0x1000000u) {
            std::memcpy(&entry.font, static_cast<const uint8_t*>(fonts) +
                                         i * 24u, 4u);
            std::memcpy(&entry.spreset, static_cast<const uint8_t*>(fonts) +
                                            i * 24u + 4u, 4u);
            std::memcpy(&entry.sbank, static_cast<const uint8_t*>(fonts) +
                                          i * 24u + 8u, 4u);
            std::memcpy(&entry.dpreset, static_cast<const uint8_t*>(fonts) +
                                            i * 24u + 12u, 4u);
            std::memcpy(&entry.dbank, static_cast<const uint8_t*>(fonts) +
                                          i * 24u + 16u, 4u);
            std::memcpy(&entry.dbanklsb, static_cast<const uint8_t*>(fonts) +
                                             i * 24u + 20u, 4u);
        } else {
            std::memcpy(&entry.font, static_cast<const uint8_t*>(fonts) +
                                         i * 12u, 4u);
            std::memcpy(&entry.spreset, static_cast<const uint8_t*>(fonts) +
                                            i * 12u + 4u, 4u);
            std::memcpy(&entry.sbank, static_cast<const uint8_t*>(fonts) +
                                          i * 12u + 8u, 4u);
        }
        if (!BassFontResolve(entry.font)) {
            g_bassLastError = 20;  // BASS_ERROR_ILLPARAM: bad font handle
            BassLog("StreamSetFonts(handle=%u): entry %u has bad font %u",
                    handle, i, entry.font);
            return FALSE;
        }
    }
    BassStream* stream = BassStreamResolve(handle);
    if (!stream && handle != 0u) {
        g_bassLastError = 5;  // BASS_ERROR_HANDLE
        return FALSE;
    }
    if (handle == 0u) {
        g_bassDefaultFonts = std::move(list);
        BassLog("StreamSetFonts(handle=0): default config, %u entries",
                entries);
    } else {
        stream->fontConfig = std::move(list);
        // The session took its font at creation; if the stream's list picks
        // a different (higher-priority) font, rebuild while untouched. The
        // priority path of the per-stream list is authoritative.
        const wchar_t* path = BassResolveFontPath(stream->fontConfig);
        if (path && stream->renderedFrames == 0u &&
            path != stream->fontPath) {
            BassLog("StreamSetFonts(handle=%u): font switch '%ls' -> '%ls'",
                    handle, stream->fontPath.c_str(), path);
            BassApplyStreamFont(*stream, path, stream->voiceLimit);
        } else {
            BassLog("StreamSetFonts(handle=%u): %u entries (font unchanged)",
                    handle, entries);
        }
    }
    return TRUE;
}

HSTREAM WINAPI BASS_MIDI_StreamCreate(DWORD channels, DWORD flags,
                                      DWORD freq) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    // First parameter = the stream's MIDI channel count (Kiva passes 16 and
    // then also sets BASS_ATTRIB_MIDI_CHANS); output is always the engine's
    // stereo pair regardless.
    if (channels == 0u || channels > 16u) {
        g_bassLastError = 6;  // BASS_ERROR_FORMAT
        return 0u;
    }
    constexpr DWORD kBassSampleFloat = 0x100u;
    constexpr DWORD kBassStreamDecode = 0x200000u;
    auto stream = std::make_unique<BassStream>();
    stream->sampleRate = freq != 0u ? freq : g_bassInitRate;
    stream->channels = 2u;
    stream->floating = (flags & kBassSampleFloat) != 0u;
    if ((flags & kBassStreamDecode) == 0u) {
        // BASS streams without the decode flag play through BASS's own
        // device output, which this shim does not implement. Prerender
        // flows always use decode.
        g_bassLastError = 6;  // BASS_ERROR_NONET-ish: unsupported request
        return 0u;
    }
    // Font selection: the stream-level config (rarely set pre-create) wins,
    // then the default config, then the most recent FontInit slot — the
    // single-slot behavior of the previous shim, kept as the fallback for
    // hosts that never call StreamSetFonts(0, ...).
    const std::vector<BassFontMapEntry>* config =
        stream->fontConfig.empty() ? &g_bassDefaultFonts
                                   : &stream->fontConfig;
    const wchar_t* fontPath = BassResolveFontPath(*config);
    if (!fontPath) {
        for (auto it = g_bassFonts.rbegin(); it != g_bassFonts.rend(); ++it) {
            if (!it->freed && !it->path.empty()) {
                fontPath = it->path.c_str();
                break;
            }
        }
    }
    if (!fontPath) {
        g_bassLastError = 21;  // BASS_ERROR_NOTAVAIL: no soundfont
        return 0u;
    }

    if (!BassApplyStreamFont(*stream, fontPath, stream->voiceLimit)) {
        LOG("  BASS StreamCreate: session failed font='%ls'", fontPath);
        g_bassLastError = 2;  // BASS_ERROR_FILEOPEN / font load failed
        return 0u;
    }
    BassLogNow("StreamCreate: ch=%u flags=%#X freq=%u voices=%u font='%ls' "
            "-> handle=%u",
            channels, flags, stream->sampleRate, stream->voiceLimit,
            stream->fontPath.c_str(),
            static_cast<uint32_t>(g_bassStreams.size()));
    g_bassStreams.push_back(std::move(stream));
    g_bassLastError = 0;
    return static_cast<HSTREAM>(g_bassStreams.size());  // 1-based handle
}

DWORD WINAPI BASS_MIDI_StreamEvents(HSTREAM handle, DWORD mode,
                                    const void* events, DWORD length) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream || !events || length == 0u) {
        g_bassLastError = 20;
        return 0u;
    }
    // bassmidi.h event modes — values verified by reflecting
    // Bass.Net.dll's BASSMIDIEventMode (STRUCT=0, RAW=0x10000,
    // SYNC=0x1000000, NORSTATUS=0x2000000, CANCEL=0x4000000,
    // TIME=0x8000000). TIME = "delta-time info is present": the event
    // carries its stream position (the RAW block header's tick field /
    // the struct's pos field, both in stream BYTES — BASS_MIDI_EVENT.pos
    // is documented as bytes). Without TIME, positions are ignored and
    // the event applies at the CURRENT decode cursor — the realtime
    // contract players like Kiva's generator rely on (pull up to the
    // event time, then send the event raw).
    constexpr DWORD kBassMidiEventsRaw = 0x10000u;
    constexpr DWORD kBassMidiEventsSync = 0x1000000u;   // sync-callback flag
    constexpr DWORD kBassMidiEventsNorStatus = 0x2000000u;
    constexpr DWORD kBassMidiEventsCancel = 0x4000000u;
    constexpr DWORD kBassMidiEventsTime = 0x8000000u;
    (void)kBassMidiEventsSync;
    (void)kBassMidiEventsNorStatus;

    if ((mode & kBassMidiEventsCancel) != 0u) stream->pending.clear();

    const uint32_t bytesPerFrame =
        stream->channels * (stream->floating ? 4u : 2u);
    // Positioning anchor for position-less (realtime) events: the pull
    // cursor. Events sent between pulls land at the first frame of the
    // NEXT pull — for Kiva's pump this is exactly the sample the event
    // belongs to, because it drains up to the event time before sending.
    const uint64_t syncFrame = stream->renderedFrames;
    const uint8_t* cursor = static_cast<const uint8_t*>(events);
    // Channel override: mode's low 16 bits carry a 1-based channel number
    // (0 = none); the override replaces each event's channel nibble/field.
    const DWORD override = mode & 0xffffu;
    const uint8_t overrideChan =
        override != 0u && override <= 16u
            ? static_cast<uint8_t>(override - 1u) : 0u;
    DWORD accepted = 0u;
    if ((mode & kBassMidiEventsRaw) != 0u) {
        if ((mode & kBassMidiEventsTime) != 0u) {
            // Raw blocks: runs of (DWORD pos, DWORD length, MIDI bytes),
            // pos = stream byte position of the event (prerender batch
            // pumps such as BPFA submit absolute byte positions).
            for (DWORD i = 0u; i + 8u <= length;) {
                DWORD pos = 0u, block = 0u;
                std::memcpy(&pos, cursor + i, 4u);
                std::memcpy(&block, cursor + i + 4u, 4u);
                i += 8u;
                if (block == 0u || i + block > length) break;
                const uint8_t status = cursor[i] & 0xf0u;
                if (cursor[i] >= 0x80u && block >= 2u + (status == 0xC0u || status == 0xD0u ? 0u : 1u)) {
                    const uint32_t message =
                        static_cast<uint32_t>(
                            override != 0u ? status | overrideChan
                                           : cursor[i]) |
                        (static_cast<uint32_t>(cursor[i + 1]) << 8u) |
                        (block > 2u ? static_cast<uint32_t>(cursor[i + 2]) << 16u : 0u);
                    const uint64_t frame = pos / bytesPerFrame;
                    stream->pending.push_back(
                        BassPackEvent(static_cast<uint32_t>(
                            (std::min)(frame, static_cast<uint64_t>(UINT32_MAX))),
                            message));
                    stream->maxEventFrame = (std::max)(stream->maxEventFrame, frame);
                }
                i += block;
                ++accepted;
            }
        } else {
            // Plain raw MIDI byte stream (Bass.Net IntPtr overload: "the
            // pointer to the event data, e.g. as received in a
            // MIDIINPROC"): consecutive messages, no per-event headers.
            // Realtime players (Kiva's SendEventRaw) submit exactly one
            // 3-byte message per call between pulls.
            for (DWORD i = 0u; i < length;) {
                const uint8_t status = cursor[i];
                if (status < 0x80u) break;  // running status unsupported here
                const uint32_t message =
                    static_cast<uint32_t>(
                        override != 0u ? (status & 0xf0u) | overrideChan
                                       : status) |
                    (i + 1u < length ? static_cast<uint32_t>(cursor[i + 1]) << 8u : 0u) |
                    (i + 2u < length ? static_cast<uint32_t>(cursor[i + 2]) << 16u : 0u);
                const DWORD consumed =
                    2u + ((status & 0xf0u) == 0xC0u || (status & 0xf0u) == 0xD0u ? 0u : 1u);
                if (i + consumed > length) break;
                // Positionless anchor: realtime pump (see syncAnchored).
                stream->syncAnchored = true;
                stream->pending.push_back(
                    BassPackEvent(static_cast<uint32_t>(
                        (std::min)(syncFrame, static_cast<uint64_t>(UINT32_MAX))),
                        message));
                stream->maxEventFrame = (std::max)(stream->maxEventFrame, syncFrame);
                i += consumed;
                ++accepted;
            }
        }
    } else {
        // BASS_MIDI_EVENT structures: {event, param, chan, tick, time}.
        constexpr DWORD kEventStructSize = 20u;
        for (DWORD i = 0u; i + kEventStructSize <= length;
             i += kEventStructSize) {
            DWORD type = 0u, param = 0u, chan = 0u, tick = 0u, timeMs = 0u;
            std::memcpy(&type, cursor + i, 4u);
            std::memcpy(&param, cursor + i + 4u, 4u);
            std::memcpy(&chan, cursor + i + 8u, 4u);
            std::memcpy(&tick, cursor + i + 12u, 4u);
            std::memcpy(&timeMs, cursor + i + 16u, 4u);
            if (override != 0u && override <= 16u) chan = overrideChan;
            uint32_t message = 0u;
            if (!BassTranslateMidiEvent(type, param, chan, message)) continue;
            // With TIME the struct's pos field carries the stream byte
            // position (BASS_MIDI_EVENT.pos is documented as bytes);
            // without TIME both position fields are ignored and the event
            // applies at the current cursor.
            const uint64_t frame =
                (mode & kBassMidiEventsTime) != 0u
                    ? timeMs / bytesPerFrame
                    : syncFrame;
            if ((mode & kBassMidiEventsTime) == 0u)
                stream->syncAnchored = true;  // realtime pump
            stream->pending.push_back(
                BassPackEvent(static_cast<uint32_t>(
                    (std::min)(frame, static_cast<uint64_t>(UINT32_MAX))),
                    message));
            stream->maxEventFrame = (std::max)(stream->maxEventFrame, frame);
            ++accepted;
        }
    }
    g_bassLastError = 0;
    if (accepted != 0u) stream->lastSubmitTickMs = BassNowTickMs();
    if ((mode & kBassMidiEventsTime) != 0u) stream->sawTimeEvents = true;
    // NO per-call logging here: realtime pumps submit one event per call at
    // six-figure event rates, and each BassLog line is a file
    // open/write/close (~30-60us) — pure I/O strangulation of the render.
    // Failures only.
    if (accepted == 0u && length != 0u)
        BassLog("StreamEvents(handle=%u mode=%#X len=%u) accepted 0",
                handle, mode, length);
    return accepted;
}

DWORD WINAPI BASS_MIDI_StreamEvent(HSTREAM handle, DWORD chan, DWORD type,
                                   DWORD param) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream || !stream->session || chan >= 16u) {
        g_bassLastError = stream ? 20 : 5;  // BASS_ERROR_HANDLE
        return static_cast<DWORD>(-1);
    }
    // Record the parameter even for types this shim does not synthesize
    // (RPN/reverb/chorus/per-note CCs are consumed-as-skipped, matching
    // StreamEvents struct mode) so StreamGetEvent round-trips them.
    stream->eventState[(chan << 24u) | (type & 0xffffffu)] = param;
    uint32_t message = 0u;
    if (BassTranslateMidiEvent(type, param, chan, message)) {
        const uint64_t syncFrame = stream->renderedFrames;
        stream->syncAnchored = true;  // realtime single-event API
        stream->pending.push_back(BassPackEvent(
            static_cast<uint32_t>(
                (std::min)(syncFrame, static_cast<uint64_t>(UINT32_MAX))),
            message));
        stream->maxEventFrame =
            (std::max)(stream->maxEventFrame, syncFrame);
    }
    stream->lastSubmitTickMs = BassNowTickMs();
    g_bassLastError = 0;
    return param;
}

DWORD WINAPI BASS_MIDI_StreamGetEvent(HSTREAM handle, DWORD chan, DWORD type) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream || chan >= 16u) {
        g_bassLastError = stream ? 20 : 5;  // BASS_ERROR_HANDLE
        return static_cast<DWORD>(-1);
    }
    // HIWORD of type can carry a per-note key (NOTE/KEYPRES/drum events);
    // the base type is what the cache is keyed by.
    const auto it = stream->eventState.find(
        (chan << 24u) | (type & 0xffffffu));
    g_bassLastError = 0;
    return it != stream->eventState.end() ? it->second : 0u;
}

DWORD WINAPI BASS_MIDI_GetVersion(void) {
    return 0x02040400u;
}

BOOL WINAPI BASS_MIDI_StreamLoadSamples(HSTREAM handle) {
    (void)handle;
    // Samples load on demand in this engine; nothing to preload.
    return TRUE;
}

BOOL WINAPI BASS_MIDI_StreamSetFilter(HSTREAM handle, BOOL time,
                                      float speed) {
    (void)handle; (void)time; (void)speed;
    // Decode streams render as fast as pulled; a playback filter does not
    // apply.
    return TRUE;
}

DWORD WINAPI BASS_MIDI_FontFlags(HSOUNDFONT handle, DWORD flags, DWORD mask) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (handle == 0u || handle > g_bassFonts.size()) {
        g_bassLastError = 5;  // BASS_ERROR_HANDLE
        return static_cast<DWORD>(-1);
    }
    BassFont& font = g_bassFonts[handle - 1u];
    const DWORD prev = font.flags;
    font.flags = (font.flags & ~mask) | (flags & mask);
    return prev;
}

BOOL WINAPI BASS_MIDI_FontSetVolume(HSOUNDFONT handle, float volume) {
    (void)handle; (void)volume;
    // Per-font gain is not represented (master volume covers the session).
    return TRUE;
}

float WINAPI BASS_MIDI_FontGetVolume(HSOUNDFONT handle) {
    (void)handle;
    return 1.0f;
}

BOOL WINAPI BASS_MIDI_FontCompact(HSOUNDFONT handle) {
    (void)handle;
    return TRUE;
}

BOOL WINAPI BASS_MIDI_FontUnload(HSOUNDFONT handle, int preset, int bank) {
    (void)handle; (void)preset; (void)bank;
    return TRUE;
}

// ── Unsupported BASSMIDI surface ─────────────────────────────────────────
// These exist so statically-importing hosts load; this engine does not
// implement them. File/URL/user-stream MIDI creation would need a MIDI
// parser — prerender hosts of the BPFA/PGFA family use StreamCreate +
// StreamEvents instead (verified against the decompiles).

HSTREAM WINAPI BASS_MIDI_StreamCreateFile(BOOL mem, const void* file,
                                          unsigned long long offset,
                                          unsigned long long length,
                                          DWORD flags, DWORD freq) {
    (void)mem; (void)file; (void)offset; (void)length; (void)flags;
    (void)freq;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 2;  // BASS_ERROR_FILEOPEN
    return 0u;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateURL(const char* url, DWORD offset,
                                         DWORD flags, DWORD freq) {
    (void)url; (void)offset; (void)flags; (void)freq;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 2;  // BASS_ERROR_FILEOPEN
    return 0u;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateFileUser(DWORD system, DWORD flags,
                                              const void* procs,
                                              void* user, DWORD freq) {
    (void)system; (void)flags; (void)procs; (void)user; (void)freq;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;  // BASS_ERROR_ILLPARAM
    return 0u;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateEvents(const void* events, DWORD ppqn,
                                            DWORD flags, DWORD freq) {
    (void)events; (void)ppqn; (void)flags; (void)freq;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;  // BASS_ERROR_ILLPARAM
    return 0u;
}

DWORD WINAPI BASS_MIDI_ConvertEvents(const void* src, DWORD count,
                                     void* dest, DWORD mode) {
    (void)src; (void)count; (void)dest; (void)mode;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return 0u;
}

DWORD WINAPI BASS_MIDI_StreamGetEvents(HSTREAM handle, DWORD chan,
                                       DWORD typefilter, void* events) {
    (void)handle; (void)chan; (void)typefilter; (void)events;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return static_cast<DWORD>(-1);
}

DWORD WINAPI BASS_MIDI_StreamGetEventsEx(HSTREAM handle, DWORD chan,
                                         DWORD typefilter, void* events,
                                         DWORD start, DWORD count) {
    (void)handle; (void)chan; (void)typefilter; (void)events; (void)start;
    (void)count;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return static_cast<DWORD>(-1);
}

const void* WINAPI BASS_MIDI_StreamGetMark(HSTREAM handle, DWORD chan,
                                           DWORD type, DWORD index) {
    (void)handle; (void)chan; (void)type; (void)index;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return nullptr;
}

DWORD WINAPI BASS_MIDI_StreamGetMarks(HSTREAM handle, DWORD chan, DWORD type,
                                      void* marks) {
    (void)handle; (void)chan; (void)type; (void)marks;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 0;
    return 0u;
}

BOOL WINAPI BASS_MIDI_StreamGetPreset(HSTREAM handle, DWORD chan,
                                      void* preset) {
    (void)handle; (void)chan; (void)preset;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

DWORD WINAPI BASS_MIDI_StreamGetFonts(HSTREAM handle, void* fonts,
                                      DWORD count) {
    (void)handle; (void)fonts; (void)count;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 0;
    return 0u;  // no per-stream font list is retained
}

DWORD WINAPI BASS_MIDI_StreamGetChannel(HSTREAM handle, DWORD chan) {
    (void)handle; (void)chan;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;  // BASS_ERROR_ILLPARAM
    return 0u;
}

HSOUNDFONT WINAPI BASS_MIDI_FontInitUser(const void* procs, void* user,
                                         DWORD flags) {
    (void)procs; (void)user; (void)flags;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;  // BASS_ERROR_ILLPARAM
    return 0u;
}

BOOL WINAPI BASS_MIDI_FontLoadEx(HSOUNDFONT handle, int preset, int bank,
                                 DWORD length) {
    (void)handle; (void)preset; (void)bank; (void)length;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

const char* WINAPI BASS_MIDI_FontPack(HSOUNDFONT handle, const char* outfile,
                                      const char* encoder, DWORD flags) {
    (void)handle; (void)outfile; (void)encoder; (void)flags;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return nullptr;
}

HSOUNDFONT WINAPI BASS_MIDI_FontUnpack(HSOUNDFONT handle, const char* name,
                                       DWORD offset, DWORD length) {
    (void)handle; (void)name; (void)offset; (void)length;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return 0u;
}

BOOL WINAPI BASS_MIDI_FontGetInfo(HSOUNDFONT handle, void* info) {
    (void)handle; (void)info;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

BOOL WINAPI BASS_MIDI_FontGetPreset(HSOUNDFONT handle, int preset,
                                    int bank) {
    (void)handle; (void)preset; (void)bank;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

DWORD WINAPI BASS_MIDI_FontGetPresets(HSOUNDFONT handle) {
    (void)handle;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 0;
    return 0u;
}

BOOL WINAPI BASS_MIDI_InInit(DWORD device, const void* proc, void* user) {
    (void)device; (void)proc; (void)user;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

BOOL WINAPI BASS_MIDI_InFree(DWORD device) {
    (void)device;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

BOOL WINAPI BASS_MIDI_InStart(DWORD device) {
    (void)device;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

BOOL WINAPI BASS_MIDI_InStop(DWORD device) {
    (void)device;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

BOOL WINAPI BASS_MIDI_InGetDeviceInfo(DWORD device, void* info) {
    (void)device; (void)info;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = 20;
    return FALSE;
}

DWORD WINAPI BASS_ChannelGetData(DWORD handle, void* buffer, DWORD length) {
    // Flag values reflected from Bass.Net (BASSData):
    // BASS_DATA_FLOAT = 0x40000000, BASS_DATA_AVAILABLE = 0.
    // (The previous shim used 0x400, so the float bit survived the strip and
    // Kiva's 1 MB pulls were read as ~1 GB requests — frames beyond the
    // caller's buffer were rendered and written: the "trash audio" overrun.)
    constexpr DWORD kBassDataFloat = 0x40000000u;
    constexpr DWORD kBassDataNoPos = 0x800000u;
    // BASS_DATA_AVAILABLE == 0: an available-bytes query is the
    // (nullptr, length=0) call shape.
    const bool available = buffer == nullptr && length == 0u;
    const bool wantFloat = (length & kBassDataFloat) != 0u;
    length &= ~(kBassDataFloat | kBassDataNoPos);

    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream || !stream->session || !buffer || length == 0u) {
        if (available && stream && stream->session) {
            // Available-bytes query: everything up to the two-second tail.
            const uint32_t bytesPerFrame =
                stream->channels * (stream->floating ? 4u : 2u);
            const uint64_t streamEnd = stream->maxEventFrame +
                static_cast<uint64_t>(stream->sampleRate) * 2u;
            const uint64_t frames =
                streamEnd > stream->servedFrames
                    ? streamEnd - stream->servedFrames
                    : 0u;
            g_bassLastError = 0;
            return static_cast<DWORD>(
                (std::min<uint64_t>)(frames, 0x7fffffffu / bytesPerFrame) *
                bytesPerFrame);
        }
        g_bassLastError = stream ? 20 : 5;  // 5 = BASS_ERROR_HANDLE
        return static_cast<DWORD>(-1);
    }
    const uint32_t bytesPerFrame =
        stream->channels * (stream->floating ? 4u : 2u);
    // BASS converts on request; honoring the stream's own format when the
    // caller's flag differs is a follow-up — today both default to float.
    (void)wantFloat;
    const uint32_t frames = length / bytesPerFrame;
    if (frames == 0u) return 0u;

    // A BASSMIDI decode stream is FINITE: it ends past the last submitted
    // event plus the same two-second tail ChannelIsActive reports. Pulls
    // crossing the end return the remaining frames; pulls beyond it fail
    // with BASS_ERROR_ENDED. Without this, a broken caller length (Kiva's
    // generator wraps its ring count negative -> an Int32 length near
    // UINT32_MAX) renders silence forever, straight past the caller's
    // buffer, while real BASSMIDI stops at the song end.
    const uint64_t streamEnd = stream->maxEventFrame +
        static_cast<uint64_t>(stream->sampleRate) * 2u;
    // The submitted-events horizon (last event + 2 s tail) is only a TRUE
    // end when the pump has gone quiet: live pumps (PFA/Kiva playback)
    // submit events just-in-time, and reporting ENDED whenever playback
    // catches the horizon made the player believe the song was over and
    // restart it — "old audio plays again". A pump that submitted anything
    // within the quiescence window keeps the stream alive (partial/zero
    // pulls instead). File-style pumps submit everything upfront and go
    // quiescent long before the end, so their termination is unchanged.
    // SVMS_BASS_QUIET_MS overrides the window (tests).
    static uint32_t quietMs = 0u;
    if (quietMs == 0u) {
        // GetEnvironmentVariable (not getenv): hosts and test harnesses set
        // this via _putenv in a different CRT instance.
        char env[32] = {};
        const DWORD n = GetEnvironmentVariableA("SVMS_BASS_QUIET_MS", env,
                                                sizeof(env));
        unsigned parsed = 5000u;
        if (n > 0 && n < sizeof(env)) {
            const int v = std::atoi(env);
            if (v > 0) parsed = static_cast<unsigned>(v);
        }
        quietMs = parsed;
    }
    const uint64_t nowTick = BassNowTickMs();
    const bool pumpAlive = stream->lastSubmitTickMs != 0u &&
        nowTick - stream->lastSubmitTickMs < quietMs;
    if (stream->servedFrames >= streamEnd && !pumpAlive) {
        g_bassLastError = 45;  // BASS_ERROR_ENDED
        BassLog("GetData(handle=%u) -> ENDED at %llu", handle,
                static_cast<unsigned long long>(stream->servedFrames));
        return static_cast<DWORD>(-1);
    }
    const uint32_t renderable = static_cast<uint32_t>(
        (std::min<uint64_t>)(frames, streamEnd - stream->servedFrames));
    if (renderable == 0u) {
        // Caught up to the horizon while the pump is still alive: hand
        // back an empty pull rather than ending the stream.
        g_bassLastError = 0;
        return 0u;
    }

    // Render-ahead cache: refill in max_block_frames-bounded chunks and
    // serve the caller from the cache, so small-pull callers (CSCore's
    // ISampleSource reads of a few thousand frames) pay the engine's
    // per-block planning once per 1.4 s of audio instead of once per pull.
    // Policy: batch pumpers (TIME-anchored events seen, and no positionless
    // event ever) refill a full engine chunk per call; everyone else —
    // especially realtime pumps whose positionless events must land at the
    // consumption cursor — refills exactly the outstanding request.
    // (Render-ahead on a stream that later sends positionless events would
    // anchor them in already-rendered audio: the send-after-pull probe
    // regression.)
    const bool renderAhead =
        stream->sawTimeEvents && !stream->syncAnchored;
    while (stream->cacheEnd - stream->cacheStart < renderable &&
           stream->renderedFrames < streamEnd) {
        const uint32_t want =
            renderable - (stream->cacheEnd - stream->cacheStart);
        const uint32_t chunk = (std::min<uint64_t>)(
            renderAhead ? stream->maxBlockFrames : want,
            streamEnd - stream->renderedFrames);
        if (chunk == 0u) break;
        const uint64_t windowEnd = stream->renderedFrames + chunk;
        std::vector<SVMS_OfflineEvent> window;
        std::vector<SVMS_OfflineEvent> stillPending;
        stillPending.reserve(stream->pending.size());
        for (const SVMS_OfflineEvent& ev : stream->pending) {
            if (ev.frame_offset < windowEnd) {
                window.push_back(BassPackEvent(
                    static_cast<uint32_t>(
                        stream->renderedFrames == 0u
                            ? ev.frame_offset
                            : ev.frame_offset -
                                  static_cast<uint32_t>(
                                      stream->renderedFrames)),
                    ev.packed_message));
            } else {
                stillPending.push_back(ev);
            }
        }
        // Non-decreasing frame order for NativeRenderOffline. Within one
        // frame the SUBMISSION order is kept (stable sort): same-frame
        // sequences such as RPN selects before their data entry are
        // position-sensitive, so never reorder by packed message.
        std::stable_sort(window.begin(), window.end(),
                         [](const SVMS_OfflineEvent& a,
                            const SVMS_OfflineEvent& b) {
                             return a.frame_offset < b.frame_offset;
                         });
        stream->pending.swap(stillPending);

        if (NativeRenderOffline(stream->session, window.data(),
                                static_cast<uint32_t>(window.size()),
                                stream->scratchLeft.data(),
                                stream->scratchRight.data(),
                                chunk) != SVMS_RESULT_OK) {
            g_bassLastError = -1;
            return static_cast<DWORD>(-1);
        }
        stream->renderedFrames = windowEnd;

        // Compact consumed frames, then append this chunk interleaved.
        if (stream->cacheStart > 0u) {
            stream->cache.erase(
                stream->cache.begin(),
                stream->cache.begin() +
                    static_cast<ptrdiff_t>(stream->cacheStart) *
                        stream->channels);
            stream->cacheEnd -= stream->cacheStart;
            stream->cacheStart = 0u;
        }
        stream->cache.reserve(
            static_cast<size_t>(stream->maxBlockFrames) * 2u);
        for (uint32_t f = 0u; f < chunk; ++f) {
            stream->cache.push_back(stream->scratchLeft[f]);
            if (stream->channels > 1u)
                stream->cache.push_back(stream->scratchRight[f]);
        }
        stream->cacheEnd += chunk;
    }

    float* const outFloat = static_cast<float*>(buffer);
    int16_t* const outShort = static_cast<int16_t*>(buffer);
    for (uint32_t f = 0u; f < renderable; ++f) {
        const float l =
            stream->cache[(stream->cacheStart + f) * stream->channels];
        const float r = stream->channels > 1u
            ? stream->cache[(stream->cacheStart + f) * stream->channels + 1u]
            : 0.0f;
        if (stream->floating) {
            outFloat[f * stream->channels] = l;
            if (stream->channels > 1u)
                outFloat[f * stream->channels + 1u] = r;
        } else {
            auto clamp = [](float v) -> int16_t {
                const float s =
                    v >= -1.0f ? (v <= 1.0f ? v : 1.0f) : -1.0f;
                return static_cast<int16_t>(s * 32767.0f);
            };
            outShort[f * stream->channels] = clamp(l);
            if (stream->channels > 1u)
                outShort[f * stream->channels + 1u] = clamp(r);
        }
    }
    stream->cacheStart += renderable;
    stream->servedFrames += renderable;
    static uint32_t pullCount = 0u;
    if ((pullCount++ % 1024u) == 0u) {
        SVMS_OfflineTelemetry tel{};
        tel.struct_size = sizeof(tel);
        tel.struct_version = SVMS_STRUCT_VERSION_1;
        if (NativeGetOfflineTelemetry(stream->session, &tel) ==
            SVMS_RESULT_OK && tel.struct_size >= offsetof(SVMS_OfflineTelemetry, render_cycles) + 8u) {
            BassLog("profile: paths=%#x render=%.1fMcyc noteon=%u(%.1fMcyc) "
                    "res=%.1f alloc=%.1f cfg=%.1f noff=%.1f ctl=%.1f "
                    "coal=%llu act=%u steals=%u",
                    tel.render_paths,
                    static_cast<double>(tel.render_cycles) / 1e6,
                    tel.dispatch_note_ons,
                    static_cast<double>(tel.dispatch_note_on_cycles) / 1e6,
                    static_cast<double>(tel.dispatch_resolve_cycles) / 1e6,
                    static_cast<double>(tel.dispatch_alloc_cycles) / 1e6,
                    static_cast<double>(tel.dispatch_configure_cycles) / 1e6,
                    static_cast<double>(tel.dispatch_note_off_cycles) / 1e6,
                    static_cast<double>(tel.dispatch_control_cycles) / 1e6,
                    static_cast<unsigned long long>(tel.dispatch_coalesced),
                    tel.active_voices, tel.voice_steals);
        }
        BassLog("GetData(handle=%u len=%u) -> %u frames (pull %u, "
                "cursor=%llu/%llu)", handle, length, renderable, pullCount,
                static_cast<unsigned long long>(stream->servedFrames),
                static_cast<unsigned long long>(streamEnd));
    }
    g_bassLastError = 0;
    return renderable * bytesPerFrame;
}

unsigned long long WINAPI BASS_ChannelGetLength(DWORD handle, DWORD mode) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) return 0u;
    const uint64_t frames = stream->maxEventFrame + stream->sampleRate * 2u;
    if (mode == 1u) return frames;  // BASS_POS_BYTE=0 handled below
    const uint32_t bytesPerFrame = stream->channels * (stream->floating ? 4u : 2u);
    return frames * bytesPerFrame;
}

unsigned long long WINAPI BASS_ChannelGetPosition(DWORD handle, DWORD mode) {
    (void)mode;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) return 0u;
    const uint32_t bytesPerFrame = stream->channels * (stream->floating ? 4u : 2u);
    return stream->servedFrames * bytesPerFrame;
}

BOOL WINAPI BASS_ChannelSetPosition(DWORD handle, unsigned long long pos, DWORD mode) {
    (void)handle; (void)pos; (void)mode;
    // Offline sessions are forward-only; a prerender pump does not seek.
    return TRUE;
}

DWORD WINAPI BASS_ChannelIsActive(DWORD handle) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) return 0u;
    // 1 = BASS_ACTIVE_PLAYING while the served cursor is inside the
    // submitted-events horizon OR the pump submitted something recently
    // (live pump — see BASS_ChannelGetData's quiescence rule); 0 =
    // BASS_ACTIVE_STOPPED only when both are past.
    const uint64_t streamEnd = stream->maxEventFrame +
        static_cast<uint64_t>(stream->sampleRate) * 2u;
    if (stream->servedFrames < streamEnd) return 1u;
    const uint64_t nowTick = BassNowTickMs();
    return stream->lastSubmitTickMs != 0u &&
                   nowTick - stream->lastSubmitTickMs < 5000u
               ? 1u
               : 0u;
}

unsigned long long WINAPI BASS_ChannelSeconds2Bytes(DWORD handle, double seconds) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) return 0u;
    const uint32_t bytesPerFrame = stream->channels * (stream->floating ? 4u : 2u);
    return static_cast<unsigned long long>(seconds * stream->sampleRate) * bytesPerFrame;
}

double WINAPI BASS_ChannelBytes2Seconds(DWORD handle, unsigned long long pos) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) return 0.0;
    const uint32_t bytesPerFrame = stream->channels * (stream->floating ? 4u : 2u);
    return bytesPerFrame != 0u
        ? static_cast<double>(pos / bytesPerFrame) / stream->sampleRate
        : 0.0;
}

BOOL WINAPI BASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value) {
    // Attribute codes reflected from Bass.Net (BASSAttribute):
    // MIDI_CHANS = 0x12002, MIDI_VOICES = 0x12003, MIDI_VOICES_ACTIVE =
    // 0x12004, MIDI_STATE = 0x12005, SRC = 0x12006.
    constexpr DWORD kBassAttribMidiChans = 0x12002u;
    constexpr DWORD kBassAttribMidiVoices = 0x12003u;
    constexpr DWORD kBassAttribMidiSrc = 0x12006u;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) {
        g_bassLastError = 5;  // BASS_ERROR_HANDLE
        return FALSE;
    }
    if (attrib == kBassAttribMidiVoices) {
        if (!std::isfinite(value) || value < 1.0f ||
            value > static_cast<float>(svms::kMaxPolyphony)) {
            g_bassLastError = 20;  // BASS_ERROR_ILLPARAM
            return FALSE;
        }
        const uint32_t voices = static_cast<uint32_t>(value);
        if (voices == stream->voiceLimit) return TRUE;
        if (stream->renderedFrames == 0u && !stream->fontPath.empty()) {
            // Prerender flows set attribs before the first pull; the
            // session is rebuilt at frame 0 (pending events keep their
            // absolute frames and survive the swap).
            if (!BassApplyStreamFont(*stream, stream->fontPath, voices)) {
                g_bassLastError = 2;
                BassLogNow("ChannelSetAttribute(MIDI_VOICES=%u): rebuild FAILED",
                        voices);
                return FALSE;
            }
            BassLogNow("ChannelSetAttribute(MIDI_VOICES=%u): session rebuilt",
                    voices);
        } else {
            // Mid-render resize is not supported forward-only; record the
            // request so GetAttribute reports it, but keep rendering.
            stream->voiceLimit = voices;
            BassLogNow("ChannelSetAttribute(MIDI_VOICES=%u) mid-render: "
                    "recorded only", voices);
        }
        return TRUE;
    }
    if (attrib == kBassAttribMidiChans || attrib == kBassAttribMidiSrc) {
        // The engine renders a fixed 16-channel input with its own
        // interpolation; both attribs are accepted as no-ops.
        return TRUE;
    }
    return TRUE;  // permissive: unrecognized attribs succeed as no-ops
}

BOOL WINAPI BASS_ChannelGetAttribute(DWORD handle, DWORD attrib,
                                     float* value) {
    constexpr DWORD kBassAttribMidiVoices = 0x12003u;
    constexpr DWORD kBassAttribMidiVoicesActive = 0x12004u;
    constexpr DWORD kBassAttribMidiSrc = 0x12006u;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream || !value) {
        g_bassLastError = stream ? 20 : 5;
        return FALSE;
    }
    if (attrib == kBassAttribMidiVoices) {
        *value = static_cast<float>(stream->voiceLimit);
        return TRUE;
    }
    if (attrib == kBassAttribMidiVoicesActive) {
        SVMS_OfflineTelemetry telemetry{};
        telemetry.struct_size = sizeof(telemetry);
        telemetry.struct_version = SVMS_STRUCT_VERSION_1;
        if (NativeGetOfflineTelemetry(stream->session, &telemetry) ==
            SVMS_RESULT_OK)
            *value = static_cast<float>(telemetry.active_voices);
        else
            *value = 0.0f;
        return TRUE;
    }
    if (attrib == kBassAttribMidiSrc) {
        *value = 0.0f;  // engine's linear interpolation is not BASS's SRC
        return TRUE;
    }
    g_bassLastError = 20;
    return FALSE;
}

BOOL WINAPI BASS_ChannelGetInfo(DWORD handle, void* info) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream || !info) return FALSE;
    // BASS_CHANNELINFO { freq, chans, flags, ctype, origres, plugin... } —
    // fill the leading fields every prerender flow reads.
    struct InfoHead {
        DWORD freq;
        DWORD chans;
        DWORD flags;
        DWORD ctype;
    };
    auto* out = static_cast<InfoHead*>(info);
    out->freq = stream->sampleRate;
    out->chans = stream->channels;
    out->flags = (stream->floating ? 0x100u : 0u) | 0x200000u;  // FLOAT|DECODE
    out->ctype = 0x10006u;  // BASS_CTYPE_STREAM_MIDI
    return TRUE;
}

// ── BASS_FX surface ──────────────────────────────────────────────────────
// OmniMIDI's KDMAPI loader binds its BuiltInEngine's BASS_FX imports to
// whatever module is loaded as bassmidi/bass.dll (its own BASSMIDI build
// merges BASS_FX). With our shim loaded under those names, the entry
// points must exist. This engine has no effect chain, so every call is a
// successful no-op.

DWORD WINAPI BASS_FXReset(DWORD handle) {
    (void)handle;
    return TRUE;
}

DWORD WINAPI BASS_FXFree(DWORD handle) {
    (void)handle;
    return TRUE;
}

DWORD WINAPI BASS_FXSetParameters(DWORD handle, const void* params) {
    (void)handle; (void)params;
    return TRUE;
}

DWORD WINAPI BASS_FXGetParameters(DWORD handle, void* params) {
    (void)handle; (void)params;
    return TRUE;
}

DWORD WINAPI BASS_FXSetPriority(DWORD handle, int priority) {
    (void)handle; (void)priority;
    return TRUE;
}

DWORD WINAPI BASS_FXVersion(void) {
    return 0x02040400u;
}

DWORD WINAPI BASS_ChannelFlags(HSTREAM handle, DWORD flags, DWORD mask) {
    (void)handle; (void)flags; (void)mask;
    // Decode streams carry no mutable flags in this shim (no effect chain);
    // report no previous flags.
    return 0u;
}

BOOL WINAPI BASS_StreamFree(DWORD handle) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (handle == 0u || handle > g_bassStreams.size()) return FALSE;
    if (g_bassStreams[handle - 1u]) {
        BassStream* stream = g_bassStreams[handle - 1u].get();
        if (stream->session) {
            SVMS_OfflineTelemetry tel{};
            tel.struct_size = sizeof(tel);
            tel.struct_version = SVMS_STRUCT_VERSION_1;
            if (NativeGetOfflineTelemetry(stream->session, &tel) ==
                SVMS_RESULT_OK) {
                BassLogNow("final(handle=%u): rendered=%llu events=%llu "
                        "render=%.2fGcyc noteons=%u coal=%llu "
                        "noteon=%.2fGcyc alloc=%.2fGcyc steals=%u act=%u",
                        handle,
                        static_cast<unsigned long long>(tel.rendered_frames),
                        static_cast<unsigned long long>(tel.submitted_events),
                        static_cast<double>(tel.render_cycles) / 1e9,
                        tel.dispatch_note_ons,
                        static_cast<unsigned long long>(tel.dispatch_coalesced),
                        static_cast<double>(tel.dispatch_note_on_cycles) / 1e9,
                        static_cast<double>(tel.dispatch_alloc_cycles) / 1e9,
                        tel.voice_steals, tel.active_voices);
            if (tel.struct_size >= offsetof(SVMS_OfflineTelemetry, wv_seg_fallback_frames) + 8u)
                BassLogNow("wv(handle=%u): plan=%.2fGcyc jobs=%.2fGcyc "
                           "post=%.2fGcyc seg=%.2fGcyc calls=%llu "
                           "kernok=%llu fb=%llu kernframes=%.1fM "
                           "fbframes=%.1fM",
                           handle,
                           static_cast<double>(tel.wv_plan_cycles) / 1e9,
                           static_cast<double>(tel.wv_job_cycles) / 1e9,
                           static_cast<double>(tel.wv_post_cycles) / 1e9,
                           static_cast<double>(tel.wv_seg_cycles) / 1e9,
                           static_cast<unsigned long long>(tel.wv_seg_calls),
                           static_cast<unsigned long long>(tel.wv_seg_kernel_ok),
                           static_cast<unsigned long long>(tel.wv_seg_fallback),
                           static_cast<double>(tel.wv_seg_kernel_frames) / 1e6,
                           static_cast<double>(tel.wv_seg_fallback_frames) / 1e6);
            }
            NativeDestroySession(stream->session);
        }
        g_bassStreams[handle - 1u].reset();
    }
    g_bassLastError = 0;
    return TRUE;
}
