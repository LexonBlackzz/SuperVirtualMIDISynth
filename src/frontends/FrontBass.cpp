// BASS/BASSMIDI prerender shim exports (C++ linkage, exported by name).
//
// File map:
//   state            BassStream / BassFont tables, guarded by g_bassMutex
//   helpers          logging, offline-session config, event translation
//   MIDI files       SMF reader and the streaming feed for huge files
//   KMC modes        exact / hold / note-table behaviours (BassModeSelect)
//   BASS_* exports   init, config, fonts, streams, events
//   BassChannelGetDataCore / BASS_ChannelGetData   the render pull
//   unsupported      BASSMIDI calls that return a clean error
//   channel, FX, DSP and attribute calls that hosts probe

#include "driver/DriverInternal.h"
#include "events/MidiStream.h"
#include "synth/BassKmcApi.h"
#include "frontends/NativeOffline.h"
#include <atomic>
#include <future>
#include <queue>
#include <chrono>
#include <thread>

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
// (BassMidiForwarder.cpp) so a player loading both module names sees
// one engine instance.

typedef DWORD HSTREAM;
typedef DWORD HSOUNDFONT;

namespace {

// One entry of a stream's soundfont list (BASS_MIDI_FONTEX): which font, and
// the source and destination preset/bank it maps.
struct BassFontMapEntry {
    uint32_t font = 0u;      // BASS_MIDI_FontInit handle
    int32_t spreset = -1;
    int32_t sbank = -1;
    int32_t dpreset = -1;
    int32_t dbank = -1;
    int32_t dbanklsb = -1;
};

// A DSP callback installed with BASS_ChannelSetDSP (bassenc uses this).
struct BassDspEntry {
    DWORD handle;
    void* proc;
    void* user;
    int priority;
};

// An FX handle created with BASS_ChannelSetFX.
struct BassFxEntry {
    DWORD stream;
    DWORD type;
};

// Very large SMFs (a 2.2-billion-note Black MIDI) cannot be held as an event
// vector. Their events are produced on demand by the ParallelMidiDecoder
// into a bounded ring and consumed in render order, like the offline renderer.
struct BassFileFeed {
    svms::MappedMidiFile file;
    svms::ParsedEventRing ring;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::thread thread;
    std::string error;
    uint32_t sampleRate = 44100u;
    svms::PackedMidiEvent head{};
    bool hasHead = false;
    explicit BassFileFeed(uint32_t megabytes) : ring(megabytes) {}
    ~BassFileFeed() {
        cancel.store(true);
        if (thread.joinable()) thread.join();
    }
};

// A file event waiting for its (possibly delayed) frame; seq keeps the original order.
struct BassFileEv {
    uint32_t frame;
    uint64_t seq;
    uint32_t message;
    bool operator>(const BassFileEv& o) const {
        return frame != o.frame ? frame > o.frame : seq > o.seq;
    }
};

// One BASSMIDI stream.  Backed by an offline SVMS session: events are queued
// with absolute frames, and BASS_ChannelGetData renders forward on demand.
// Handles are 1-based indices into g_bassStreams.
struct BassStream {
    SVMS_Session session = 0;
    uint32_t sampleRate = 44100u;
    uint32_t channels = 2u;
    bool floating = true;      // BASS_SAMPLE_FLOAT output; else 16-bit
    uint64_t renderedFrames = 0u;  // frames rendered into the cache
    uint64_t servedFrames = 0u;    // frames handed to the caller
    float cpuLoad = 0.0f;          // BASS_ATTRIB_CPU: smoothed render time / audio time, %
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
    // render time and consumed as the pull cursor passes them.  Events whose
    // frame lands beyond the current render call's window stay here with
    // ABSOLUTE frames and are re-based at the next pull.
    std::vector<SVMS_OfflineEvent> pending;
    std::vector<BassDspEntry> dsps;   // BASS_ChannelSetDSP chain (bassenc installs one)
    float fxVolume = 1.0f;            // BASS_FX_VOLUME target (BASS_ChannelSetFX type 9)
    uint32_t trackCount = 1u;         // SMF track count for file streams
    std::vector<uint64_t> trackNotes;  // note-on count per SMF track (KMC lists these)
    // File streams: the whole SMF as frame-sorted events consumed from
    // fileHead (no per-pull rescans); `pending` still takes live events.
    std::vector<SVMS_OfflineEvent> fileEvents;
    std::unique_ptr<BassFileFeed> feed;  // set instead of fileEvents for huge files
    size_t fileHead = 0u;
    bool fileStream = false;
    // KMC/BASSMIDI imitation: the in-process note-table renderer (bassmidi mode),
    // created on the first pull and read forward in step with the stream.
    bool kmcMode = false;
    std::wstring midiPath;
    void* kmc = nullptr;
    // KMC 18.2.8's bassmidi.dll holds every note at least ~4 ms (measured). In hold
    // mode a file note-off closer than that to its key's last note-on moves out to
    // note-on + holdFrames, the same rule as events.min_note_ms in the realtime
    // compiler. 0 = off (exact timing).
    uint32_t holdFrames = 0u;
    uint64_t fileSeq = 0u;
    int64_t lastOn[16 * 128];
    std::priority_queue<BassFileEv, std::vector<BassFileEv>, std::greater<BassFileEv>> heldOffs;
    BassStream() { std::fill(std::begin(lastOn), std::end(lastOn), INT64_MIN / 4); }
    BassStream(const BassStream&) = delete;
    ~BassStream() {
        if (kmc) SvmsBassKmcApi()->Close(kmc);
    }
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
std::vector<BassFxEntry> g_bassFx;
// Default soundfont configuration (BASS_MIDI_StreamSetFonts with handle 0):
// applied by subsequently created streams. Earlier entries have priority
// (BASSMIDI rule, Bass.Net.xml StreamSetFonts docs).
std::vector<BassFontMapEntry> g_bassDefaultFonts;
uint32_t g_bassInitRate = 44100u;
int g_bassLastError = 0;   // BASS_ERROR codes: 0 = BASS_OK

// Looks up a live font by handle; null for 0, unknown, freed or empty fonts.
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

// Looks up a stream by handle (null when unknown).  Call with g_bassMutex held.
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
#define SVMS_ILLPARAM() (BassLogNow("BASS_ERROR_ILLPARAM set in %s:%d", __func__, __LINE__), 20)

// Builds an offline-session event for a packed short message at `frame`.
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

// ── MIDI file loading for BASS_MIDI_StreamCreateFile ─────────────────────
// BASSMIDI decode streams parse the MIDI file themselves; the shim does the
// same with a streaming SMF reader and converts every channel event to its
// exact output frame (tempo map included) so the engine sees what a BASSMIDI
// file stream would feed it. Meta events other than tempo and all SysEx are
// skipped. Memory guard: BASSMIDI keeps the whole file's events too, but a
// shim holding 16-byte records must not eat the machine — refused past
// kBassMaxFileEvents with BASS_ERROR_MEM.

constexpr uint64_t kBassMaxFileEvents = 120000000ull;
// Files at least this big (and read from a path) take the streaming feed.
constexpr uint64_t kBassStreamFileBytes = 256ull << 20;

// SVMS_BASS_STREAM_MB overrides the threshold (testing the feed on small files).
uint64_t BassStreamFileBytes() {
    char text[16] = {};
    if (GetEnvironmentVariableA("SVMS_BASS_STREAM_MB", text, sizeof(text)) != 0u)
        return static_cast<uint64_t>(std::strtoul(text, nullptr, 10)) << 20;
    return kBassStreamFileBytes;
}

// Decoder callbacks: the feed sink pushes into the stream's ring (false =
// cancelled); the count sink only counts, for measuring a file without
// keeping its events.
bool BassFeedSink(const svms::PackedMidiEvent& e, void* user) {
    auto* f = static_cast<BassFileFeed*>(user);
    return f->ring.Push(e, f->cancel);
}
bool BassCountSink(const svms::PackedMidiEvent&, void*) { return true; }

// KMC opens a file once to parse it and again to convert it, and a re-run opens it a third time.
// The scan (length + note-ons per track) only depends on the file and the rate, so it is kept in
// %TEMP% keyed by path, size, write time and rate; a repeat open skips the 17 GB read entirely.
struct BassMeasureKey {
    std::wstring path; uint64_t size = 0, mtime = 0; uint32_t rate = 0;
};
bool BassMeasureKeyOf(const std::wstring& path, uint32_t rate, BassMeasureKey& k) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    k.path = path; k.rate = rate;
    k.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    k.mtime = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}
std::wstring BassMeasureCachePath(const BassMeasureKey& k) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over path, size, mtime, rate
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) { h ^= static_cast<const uint8_t*>(p)[i]; h *= 1099511628211ull; }
    };
    mix(k.path.data(), k.path.size() * sizeof(wchar_t)); mix(&k.size, 8); mix(&k.mtime, 8); mix(&k.rate, 4);
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[40];
    swprintf(name, 40, L"svms_midi_%016llx.bin", static_cast<unsigned long long>(h));
    return std::wstring(tmp) + name;
}
bool BassMeasureLoad(const BassMeasureKey& k, uint64_t& frames, std::vector<uint64_t>& notes) {
    FILE* f = _wfopen(BassMeasureCachePath(k).c_str(), L"rb");
    if (!f) return false;
    uint64_t head[5] = {};
    bool ok = std::fread(head, 8, 5, f) == 5 && head[0] == 0x31434d5653ull && head[1] == k.size &&
              head[2] == k.mtime && head[3] == k.rate && head[4] < (1ull << 24);
    if (ok) {
        notes.resize(static_cast<size_t>(head[4]));
        ok = std::fread(&frames, 8, 1, f) == 1 &&
             (notes.empty() || std::fread(notes.data(), 8, notes.size(), f) == notes.size());
    }
    std::fclose(f);
    return ok;
}
void BassMeasureStore(const BassMeasureKey& k, uint64_t frames, const std::vector<uint64_t>& notes) {
    FILE* f = _wfopen(BassMeasureCachePath(k).c_str(), L"wb");
    if (!f) return;
    const uint64_t head[5] = {0x31434d5653ull, k.size, k.mtime, k.rate, notes.size()};
    std::fwrite(head, 8, 5, f); std::fwrite(&frames, 8, 1, f);
    if (!notes.empty()) std::fwrite(notes.data(), 8, notes.size(), f);
    std::fclose(f);
}

// Starts the decoder thread for a streamed file (no-op if already running).
void BassFeedStart(BassFileFeed& f) {
    if (f.thread.joinable()) return;
    f.thread = std::thread([&f] {
        svms::ParallelMidiDecoder decoder;
        svms::MidiStreamInfo info;
        decoder.Decode(f.file, f.sampleRate, BassFeedSink, &f, &f.cancel, &info, f.error);
        f.done.store(true, std::memory_order_release);
    });
}

// Next file event (frame-sorted) without consuming it; false = none left.
// A streamed file waits for the producer rather than reporting a false end.
bool BassFilePeek(BassStream& s, uint32_t& frame, uint32_t& message) {
    if (BassFileFeed* f = s.feed.get()) {
        if (!f->hasHead) {
            BassFeedStart(*f);
            for (;;) {
                if (f->ring.Pop(f->head)) { f->hasHead = true; break; }
                if (f->done.load(std::memory_order_acquire)) {
                    if (!f->ring.Pop(f->head)) return false;
                    f->hasHead = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        frame = static_cast<uint32_t>((std::min<uint64_t>)(f->head.outputFrame, UINT32_MAX));
        message = f->head.message;
        return true;
    }
    if (s.fileHead >= s.fileEvents.size()) return false;
    frame = s.fileEvents[s.fileHead].frame_offset;
    message = s.fileEvents[s.fileHead].packed_message;
    return true;
}
// Consumes the event BassFilePeek returned.
void BassFilePop(BassStream& s) {
    if (s.feed) s.feed->hasHead = false; else ++s.fileHead;
}

// Directory of this DLL (the shim, svms_v3_render.exe and the mode flag live there).
std::wstring BassModuleDir() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&BassModuleDir), &module);
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(module, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring dir = buf;
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

// Mode: 0 exact engine (default), 1 exact engine + ~4 ms minimum note (the 2018 KMC
// buzz), 2 the note-table imitation. Chosen by SVMS_BASS_MODE=exact|bassmidi|notetable,
// or by an empty file next to this DLL: svms_bassmidi_mode (1) / svms_notetable_mode (2).
int BassModeSelect() {
    char text[16] = {};
    if (GetEnvironmentVariableA("SVMS_BASS_MODE", text, sizeof(text)) != 0u)
        return std::strcmp(text, "bassmidi") == 0 ? 1 : std::strcmp(text, "notetable") == 0 ? 2 : 0;
    const std::wstring dir = BassModuleDir();
    if (GetFileAttributesW((dir + L"\\svms_notetable_mode").c_str()) != INVALID_FILE_ATTRIBUTES) return 2;
    if (GetFileAttributesW((dir + L"\\svms_bassmidi_mode").c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
    return 0;
}

// Hold mode: true when the note-off was queued for a later frame instead.
bool BassHoldDelays(BassStream& s, uint32_t frame, uint32_t message, uint64_t seq) {
    const uint32_t type = message & 0xf0u, key = (message >> 8) & 0xffu;
    if (key >= 128u) return false;
    int64_t& lastOn = s.lastOn[(message & 0x0fu) * 128u + key];
    const bool noteOn = type == 0x90u && ((message >> 16) & 0x7fu) != 0u;
    if (noteOn) { lastOn = frame; return false; }
    if ((type == 0x80u || type == 0x90u) && static_cast<int64_t>(frame) < lastOn + s.holdFrames) {
        s.heldOffs.push({static_cast<uint32_t>(lastOn + s.holdFrames), seq, message});
        return true;
    }
    return false;
}

// Only the bass.dll build links the renderer; elsewhere the stub answers null
// and a KMC-mode stream cannot be created.
extern "C" const SvmsKmcApi* SvmsBassKmcApiStub() { return nullptr; }
#if defined(_MSC_VER)
#pragma comment(linker, "/alternatename:SvmsBassKmcApi=SvmsBassKmcApiStub")
#endif

// Fills scratchLeft/Right with the next `n` frames of the imitation (forward only).
void BassKmcRead(BassStream& s, uint32_t n) {
    if (!s.kmc) {
        const SvmsKmcApi* api = SvmsBassKmcApi();
        if (api) s.kmc = api->Open(s.midiPath.c_str(), s.fontPath.c_str(), s.sampleRate, s.voiceLimit);
        BassLogNow("KMC mode: renderer %s ('%ls', %u voices)", s.kmc ? "started" : "unavailable",
                   s.midiPath.c_str(), s.voiceLimit);
    }
    if (!s.kmc) {
        std::fill(s.scratchLeft.begin(), s.scratchLeft.begin() + n, 0.0f);
        std::fill(s.scratchRight.begin(), s.scratchRight.begin() + n, 0.0f);
        return;
    }
    SvmsBassKmcApi()->Read(s.kmc, s.scratchLeft.data(), s.scratchRight.data(), n);
}

// Byte source for the SMF parser: a file handle or a memory block.
struct BassMidiReader {
    HANDLE file = INVALID_HANDLE_VALUE;
    const uint8_t* mem = nullptr;   // memory source (whole block)
    size_t memLen = 0;
    std::vector<uint8_t> window;    // file read window
    size_t pos = 0, len = 0;
    uint64_t trackLeft = 0;         // bytes left in the current track chunk

    bool Refill() {
        if (file == INVALID_HANDLE_VALUE) return false;
        window.resize(1u << 18);
        DWORD got = 0;
        if (!ReadFile(file, window.data(), static_cast<DWORD>(window.size()), &got, nullptr) || got == 0u)
            return false;
        pos = 0; len = got;
        return true;
    }
    int Raw() {
        if (mem) return pos < memLen ? mem[pos++] : -1;
        if (pos == len && !Refill()) return -1;
        return window[pos++];
    }
    bool Read(void* out, size_t n) {
        auto* o = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < n; ++i) { const int c = Raw(); if (c < 0) return false; o[i] = static_cast<uint8_t>(c); }
        return true;
    }
    // Byte inside the current track chunk; -1 at the chunk end.
    int Get() { if (trackLeft == 0u) return -1; const int c = Raw(); if (c >= 0) --trackLeft; return c; }
    void SkipTrackRest() {
        uint64_t n = trackLeft; trackLeft = 0;
        if (mem) { pos = static_cast<size_t>((std::min<uint64_t>)(memLen, pos + n)); return; }
        const uint64_t inWindow = len - pos;
        if (n <= inWindow) { pos += static_cast<size_t>(n); return; }
        n -= inWindow; pos = len = 0;
        LARGE_INTEGER d; d.QuadPart = static_cast<LONGLONG>(n);
        SetFilePointerEx(file, d, nullptr, FILE_CURRENT);
    }
};

struct BassMidiEv { uint64_t tick; uint32_t msg; uint32_t track; };

// Returns 0 on success, else a BASS error code (6 format, 1 memory, 2 file).
int BassParseMidi(BassMidiReader& r, uint32_t sampleRate,
                  std::vector<SVMS_OfflineEvent>& out, uint64_t& lastFrame,
                  uint32_t& firstTempoUs, uint32_t& trackCount,
                  std::vector<uint64_t>& trackNotes) {
    uint8_t head[14];
    if (!r.Read(head, 14) || std::memcmp(head, "MThd", 4) != 0) return 2;
    const uint32_t ntrk = (head[10] << 8) | head[11];
    const uint32_t div = (head[12] << 8) | head[13];
    if (div == 0u || (div & 0x8000u) != 0u) return 6;  // SMPTE timing unsupported
    trackCount = ntrk;
    trackNotes.assign(ntrk, 0u);
    std::vector<BassMidiEv> evs;
    std::vector<std::pair<uint64_t, uint32_t>> tempos;  // (tick, us per quarter note)
    for (uint32_t t = 0u; t < ntrk; ++t) {
        uint8_t ch[8];
        if (!r.Read(ch, 8)) break;
        if (std::memcmp(ch, "MTrk", 4) != 0) return 2;
        r.trackLeft = (static_cast<uint64_t>(ch[4]) << 24) | (ch[5] << 16) | (ch[6] << 8) | ch[7];
        uint64_t tick = 0u; int status = 0;
        auto vlq = [&](uint32_t& v) {
            v = 0u;
            for (int i = 0; i < 5; ++i) {
                const int c = r.Get(); if (c < 0) return false;
                v = (v << 7) | static_cast<uint32_t>(c & 0x7f);
                if ((c & 0x80) == 0) return true;
            }
            return false;
        };
        for (;;) {
            uint32_t d;
            if (!vlq(d)) break;
            tick += d;
            const int s = r.Get(); if (s < 0) break;
            if (s == 0xFF) {
                const int ty = r.Get(); uint32_t l;
                if (ty < 0 || !vlq(l)) break;
                uint8_t data[3] = {};
                for (uint32_t i = 0; i < l; ++i) { const int c = r.Get(); if (c < 0) break; if (i < 3u) data[i] = static_cast<uint8_t>(c); }
                if (ty == 0x51 && l == 3u)
                    tempos.push_back({tick, (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) | data[2]});
                if (ty == 0x2F) break;
                continue;
            }
            if (s == 0xF0 || s == 0xF7) {
                uint32_t l; if (!vlq(l)) break;
                for (uint32_t i = 0; i < l; ++i) if (r.Get() < 0) break;
                continue;
            }
            int a;
            if (s & 0x80) { status = s; a = r.Get(); } else a = s;  // running status
            if (a < 0 || status == 0) break;
            const int k = status & 0xF0;
            int b = 0;
            if (k != 0xC0 && k != 0xD0) { b = r.Get(); if (b < 0) break; }
            if (evs.size() >= kBassMaxFileEvents) return 1;
            if (k == 0x90 && b != 0) ++trackNotes[t];
            evs.push_back({tick, static_cast<uint32_t>(status) | (static_cast<uint32_t>(a) << 8) | (static_cast<uint32_t>(b) << 16), t});
        }
        r.SkipTrackRest();
    }
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const auto& x, const auto& y) { return x.first < y.first; });
    firstTempoUs = tempos.empty() ? 500000u : tempos.front().second;
    // Merge the tracks into one timeline; equal ticks keep track order.
    std::stable_sort(evs.begin(), evs.end(),
                     [](const BassMidiEv& x, const BassMidiEv& y) { return x.tick < y.tick; });
    out.reserve(out.size() + evs.size());
    size_t ti = 0; double secAtTempo = 0.0; uint64_t tickAtTempo = 0u; uint32_t usPerQn = 500000u;
    lastFrame = 0u;
    for (const BassMidiEv& e : evs) {
        while (ti < tempos.size() && tempos[ti].first <= e.tick) {
            secAtTempo += static_cast<double>(tempos[ti].first - tickAtTempo) * usPerQn / div / 1e6;
            tickAtTempo = tempos[ti].first; usPerQn = tempos[ti].second; ++ti;
        }
        const double sec = secAtTempo + static_cast<double>(e.tick - tickAtTempo) * usPerQn / div / 1e6;
        const uint64_t frame = static_cast<uint64_t>(sec * sampleRate);
        const uint32_t f32 = static_cast<uint32_t>((std::min<uint64_t>)(frame, UINT32_MAX));
        SVMS_OfflineEvent ev{};
        ev.frame_offset = f32; ev.packed_message = e.msg;
        out.push_back(ev);
        lastFrame = (std::max<uint64_t>)(lastFrame, f32);
    }
    return 0;
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

// ── BASS exports: init, config, errors ───────────────────────────────────
// The BASS_* and BASS_MIDI_* functions below mirror the real library's
// signatures.  Anything the shim does not implement fails with the matching
// BASS error code, or succeeds as a no-op where hosts only probe.

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
    // 2.4.14.0 - .NET BASS wrappers and add-ons (BASS_VST, bassenc) version-check
    // before their first call; real bass.dll builds on KMC machines report
    // 2.4.13.41 .. 2.4.17.4, so claim a value inside that range.
    return 0x02040e00u;
}

int WINAPI BASS_ErrorGetCode(void) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    return g_bassLastError;
}

// ── BASSMIDI exports: fonts, streams, events ─────────────────────────────

HSOUNDFONT WINAPI BASS_MIDI_FontInit(const void* file, DWORD flags) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (!file) {
        g_bassLastError = SVMS_ILLPARAM();  // BASS_ERROR_ILLPARAM
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
    g_bassLastError = 0;
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
    BassLogNow("StreamSetFonts: handle=%u fonts=%p count=%#X",
            handle, fonts, count);
    if (!fonts && (count & 0xffffffu) != 0u) {
        g_bassLastError = SVMS_ILLPARAM();  // BASS_ERROR_ILLPARAM
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
        BassLogNow("StreamSetFonts(handle=%u): 64-bit-font variant unsupported",
                handle);
        g_bassLastError = SVMS_ILLPARAM();
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
            g_bassLastError = SVMS_ILLPARAM();  // BASS_ERROR_ILLPARAM: bad font handle
            BassLogNow("StreamSetFonts(handle=%u): entry %u has bad font %u",
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
        BassLogNow("StreamSetFonts(handle=0): default config, %u entries",
                entries);
    } else {
        stream->fontConfig = std::move(list);
        // The session took its font at creation; if the stream's list picks
        // a different (higher-priority) font, rebuild while untouched. The
        // priority path of the per-stream list is authoritative.
        const wchar_t* path = BassResolveFontPath(stream->fontConfig);
        if (path && stream->renderedFrames == 0u &&
            path != stream->fontPath) {
            BassLogNow("StreamSetFonts(handle=%u): font switch '%ls' -> '%ls'",
                    handle, stream->fontPath.c_str(), path);
            BassApplyStreamFont(*stream, path, stream->voiceLimit);
        } else {
            BassLogNow("StreamSetFonts(handle=%u): %u entries (font unchanged)",
                    handle, entries);
        }
    }
    g_bassLastError = 0;
    return TRUE;
}

// Creates a stream and its offline session.  Call with g_bassMutex held.
// Returns the 1-based handle, or 0 with g_bassLastError set.
static HSTREAM BassCreateStreamLocked(DWORD channels, DWORD flags,
                                      DWORD freq, bool allowNoFont = false) {
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
    if (!fontPath && allowNoFont) {
        // File streams are created before the host loads its fonts (KMC adds
        // MIDIs, reads their length and note counts, and only then calls
        // StreamSetFonts): the session is built when the font list arrives.
        BassLogNow("StreamCreate: no soundfont yet (file stream), handle=%u",
                   static_cast<uint32_t>(g_bassStreams.size() + 1u));
        g_bassStreams.push_back(std::move(stream));
        g_bassLastError = 0;
        return static_cast<HSTREAM>(g_bassStreams.size());
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

HSTREAM WINAPI BASS_MIDI_StreamCreate(DWORD channels, DWORD flags,
                                      DWORD freq) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    return BassCreateStreamLocked(channels, flags, freq);
}

// BASSMIDI file stream: the shim parses the SMF itself (see BassParseMidi)
// and queues every event at its exact frame, TIME-anchored like a batch
// pump so the render-ahead cache applies. mem=TRUE reads from a memory block
// of `length` bytes; otherwise `file` is a path (UTF-16 when BASS_UNICODE or
// UTF-16-shaped, else UTF-8/ANSI), `offset` bytes into the file.
HSTREAM WINAPI BASS_MIDI_StreamCreateFile(BOOL mem, const void* file,
                                          unsigned long long offset,
                                          unsigned long long length,
                                          DWORD flags, DWORD freq) {
    BassMidiReader reader;
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (!file) {
        std::lock_guard<std::mutex> lock(g_bassMutex);
        g_bassLastError = SVMS_ILLPARAM();
        return 0u;
    }
    if (mem) {
        reader.mem = static_cast<const uint8_t*>(file) + static_cast<size_t>(offset);
        reader.memLen = static_cast<size_t>(length);
    } else {
        const char* utf8 = static_cast<const char*>(file);
        const bool wide = (flags & 0x80000000u) != 0u || (utf8[0] != 0 && utf8[1] == 0);
        if (wide) {
            path = static_cast<const wchar_t*>(file);
        } else {
            const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
            path.assign(n > 0 ? n - 1 : 0, L'\0');
            if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, path.data(), n);
        }
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> lock(g_bassMutex);
            g_bassLastError = 2;  // BASS_ERROR_FILEOPEN
            return 0u;
        }
        if (offset != 0u) {
            LARGE_INTEGER d; d.QuadPart = static_cast<LONGLONG>(offset);
            SetFilePointerEx(handle, d, nullptr, FILE_BEGIN);
        }
        reader.file = handle;
    }
    const uint32_t rate = freq != 0u ? freq : g_bassInitRate;
    std::vector<SVMS_OfflineEvent> events;
    uint64_t lastFrame = 0u;
    uint32_t tempoUs = 500000u, tracks = 0u;
    std::vector<uint64_t> trackNotes;
    std::unique_ptr<BassFileFeed> feed;
    int rc = 0;
    LARGE_INTEGER fileSize{};
    const int bassMode = BassModeSelect();
    {
        // Hold mode also takes BASSMIDI's voice admission; SVMS_BASS_STEAL_REJECT=0/1 overrides.
        char flag[8] = {};
        const bool forced = GetEnvironmentVariableA("SVMS_BASS_STEAL_REJECT", flag, sizeof(flag)) != 0u;
        svms::g_nativeOfflineStealReject.store(forced ? flag[0] == '1' : bassMode == 1);
        char ageless[8] = {};
        const bool agelessForced = GetEnvironmentVariableA("SVMS_BASS_AGELESS", ageless, sizeof(ageless)) != 0u;
        svms::g_nativeOfflineStealAgeless.store(agelessForced ? ageless[0] == '1' : bassMode == 1);
    }
    const bool kmcMode = handle != INVALID_HANDLE_VALUE && offset == 0u && bassMode == 2;
    if (handle != INVALID_HANDLE_VALUE && offset == 0u &&
        (kmcMode || (GetFileSizeEx(handle, &fileSize) &&
                     static_cast<uint64_t>(fileSize.QuadPart) >= BassStreamFileBytes()))) {
        // One counting pass (parallel, nothing stored) gives the length and
        // note total; the events are decoded again, lazily, while rendering.
        // Per-track counts are not kept: the total is reported on track 0.
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        feed = std::make_unique<BassFileFeed>(128u);
        feed->sampleRate = rate;
        svms::MidiStreamInfo info;
        std::atomic<bool> noCancel{false};
        std::string error;
        if (!feed->ring.IsValid()) rc = 1;
        else if (!feed->file.Open(path.c_str(), error)) rc = 2;
        else {
            BassMeasureKey key;
            const bool keyed = BassMeasureKeyOf(path, rate, key);
            const auto t0 = std::chrono::steady_clock::now();
            if (keyed && BassMeasureLoad(key, lastFrame, trackNotes)) {
                tracks = static_cast<uint32_t>(trackNotes.size());
                BassLogNow("StreamCreateFile: scan cache hit");
            } else if (!svms::ParallelMidiDecoder::MeasureDuration(feed->file, rate, lastFrame, error,
                                                                   &noCancel, 0u, &trackNotes)) {
                // The minimal skipper (deltas, tempo, note-on count per track) replaces a full
                // decode pass: same length, per-track note counts, a fraction of the time.
                BassLogNow("StreamCreateFile: stream measure failed: %s", error.c_str());
                rc = 2;
            } else {
                tracks = static_cast<uint32_t>(trackNotes.size());
                if (keyed) BassMeasureStore(key, lastFrame, trackNotes);
                BassLogNow("StreamCreateFile: scanned in %.1f s",
                           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
        }
    } else {
        rc = BassParseMidi(reader, rate, events, lastFrame, tempoUs, tracks, trackNotes);
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (rc != 0) {
        BassLogNow("StreamCreateFile: parse failed rc=%d", rc);
        g_bassLastError = rc;
        return 0u;
    }
    // MIDI streams are always 16 channels, float or 16-bit per the flags.
    const HSTREAM stream = BassCreateStreamLocked(16u, flags, freq, true);
    if (stream == 0u) return 0u;  // error code set by the creator
    BassStream* s = BassStreamResolve(stream);
    s->fileEvents = std::move(events);
    s->feed = std::move(feed);
    s->kmcMode = kmcMode;
    if (bassMode == 1) s->holdFrames = rate * 4u / 1000u;
    if (kmcMode) s->midiPath = path;
    s->fileStream = true;
    s->maxEventFrame = lastFrame;
    s->sawTimeEvents = true;
    s->trackCount = tracks;
    s->trackNotes = std::move(trackNotes);
    s->eventState[(0u << 24u) | 62u] = tempoUs;  // MIDI_EVENT_TEMPO (us per quarter note)
    BassLogNow("StreamCreateFile: %s%zu events, last frame %llu, tempo %u us, tracks %u -> handle=%u",
               s->feed ? "streamed, " : "", s->fileEvents.size(), static_cast<unsigned long long>(lastFrame), tempoUs, tracks, stream);
    g_bassLastError = 0;
    return stream;
}

DWORD WINAPI BASS_MIDI_StreamEvents(HSTREAM handle, DWORD mode,
                                    const void* events, DWORD length) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream || !events || length == 0u) {
        g_bassLastError = SVMS_ILLPARAM();
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
    // No session needed: file streams exist before the host loads its fonts, and
    // KMC sets the tempo at that point; the event simply queues for frame 0.
    if (!stream || chan >= 16u) {
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
    g_bassLastError = 0;
    return TRUE;
}

BOOL WINAPI BASS_MIDI_StreamSetFilter(HSTREAM handle, BOOL time,
                                      float speed) {
    (void)handle; (void)time; (void)speed;
    // Decode streams render as fast as pulled; a playback filter does not
    // apply.
    g_bassLastError = 0;
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

HSTREAM WINAPI BASS_MIDI_StreamCreateURL(const char* url, DWORD offset,
                                         DWORD flags, DWORD freq) {
    (void)url; (void)offset; (void)flags; (void)freq;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_StreamCreateURL -> ILLPARAM/unsupported");
    g_bassLastError = 2;  // BASS_ERROR_FILEOPEN
    return 0u;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateFileUser(DWORD system, DWORD flags,
                                              const void* procs,
                                              void* user, DWORD freq) {
    (void)system; (void)flags; (void)procs; (void)user; (void)freq;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_StreamCreateFileUser -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();  // BASS_ERROR_ILLPARAM
    return 0u;
}

HSTREAM WINAPI BASS_MIDI_StreamCreateEvents(const void* events, DWORD ppqn,
                                            DWORD flags, DWORD freq) {
    (void)events; (void)ppqn; (void)flags; (void)freq;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_StreamCreateEvents -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();  // BASS_ERROR_ILLPARAM
    return 0u;
}

DWORD WINAPI BASS_MIDI_ConvertEvents(const void* src, DWORD count,
                                     void* dest, DWORD mode) {
    (void)src; (void)count; (void)dest; (void)mode;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = SVMS_ILLPARAM();
    return 0u;
}

DWORD WINAPI BASS_MIDI_StreamGetEvents(HSTREAM handle, DWORD chan,
                                       DWORD typefilter, void* events) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) { g_bassLastError = 5; return static_cast<DWORD>(-1); }
    // Counting form only (events == NULL): MIDI_EVENT_NOTES (0x20000) = the
    // note-on count of a track (-1 = all tracks), which is what KMC lists for
    // each MIDI. Copying the events out is not supported.
    if (events || !stream->fileStream) { g_bassLastError = SVMS_ILLPARAM(); return static_cast<DWORD>(-1); }
    g_bassLastError = 0;
    if ((typefilter & 0xffffu) != 0u && typefilter != 0x20000u) return 0u;
    if (static_cast<int>(chan) == -1) {
        uint64_t total = 0u;
        for (uint64_t n : stream->trackNotes) total += n;
        return static_cast<DWORD>((std::min<uint64_t>)(total, 0xffffffffu));
    }
    return chan < stream->trackNotes.size()
               ? static_cast<DWORD>((std::min<uint64_t>)(stream->trackNotes[chan], 0xffffffffu)) : 0u;
}

DWORD WINAPI BASS_MIDI_StreamGetEventsEx(HSTREAM handle, DWORD chan,
                                         DWORD typefilter, void* events,
                                         DWORD start, DWORD count) {
    (void)handle; (void)chan; (void)typefilter; (void)events; (void)start;
    (void)count;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = SVMS_ILLPARAM();
    return static_cast<DWORD>(-1);
}

const void* WINAPI BASS_MIDI_StreamGetMark(HSTREAM handle, DWORD chan,
                                           DWORD type, DWORD index) {
    (void)handle; (void)chan; (void)type; (void)index;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = SVMS_ILLPARAM();
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
    BassLogNow("stub called: BASS_MIDI_StreamGetPreset -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

DWORD WINAPI BASS_MIDI_StreamGetFonts(HSTREAM handle, void* fonts,
                                      DWORD count) {
    (void)handle; (void)fonts; (void)count;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_StreamGetFonts -> ILLPARAM/unsupported");
    g_bassLastError = 0;
    return 0u;  // no per-stream font list is retained
}

// A MIDI channel handle is 0x7e000000 | stream << 8 | channel. It only answers
// attribute queries (the per-channel voice counts of a host's voice window).
DWORD WINAPI BASS_MIDI_StreamGetChannel(HSTREAM handle, DWORD chan) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (!BassStreamResolve(handle) || chan >= 16u) {
        g_bassLastError = SVMS_ILLPARAM();  // BASS_ERROR_ILLPARAM
        return 0u;
    }
    g_bassLastError = 0;
    return 0x7e000000u | ((handle & 0xffffu) << 8) | chan;
}

HSOUNDFONT WINAPI BASS_MIDI_FontInitUser(const void* procs, void* user,
                                         DWORD flags) {
    (void)procs; (void)user; (void)flags;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_FontInitUser -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();  // BASS_ERROR_ILLPARAM
    return 0u;
}

BOOL WINAPI BASS_MIDI_FontLoadEx(HSOUNDFONT handle, int preset, int bank,
                                 DWORD length) {
    (void)handle; (void)preset; (void)bank; (void)length;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_FontLoadEx -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

const char* WINAPI BASS_MIDI_FontPack(HSOUNDFONT handle, const char* outfile,
                                      const char* encoder, DWORD flags) {
    (void)handle; (void)outfile; (void)encoder; (void)flags;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_FontPack -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();
    return nullptr;
}

HSOUNDFONT WINAPI BASS_MIDI_FontUnpack(HSOUNDFONT handle, const char* name,
                                       DWORD offset, DWORD length) {
    (void)handle; (void)name; (void)offset; (void)length;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_FontUnpack -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();
    return 0u;
}

BOOL WINAPI BASS_MIDI_FontGetInfo(HSOUNDFONT handle, void* info) {
    (void)handle; (void)info;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_FontGetInfo -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

BOOL WINAPI BASS_MIDI_FontGetPreset(HSOUNDFONT handle, int preset,
                                    int bank) {
    (void)handle; (void)preset; (void)bank;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_FontGetPreset -> ILLPARAM/unsupported");
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

DWORD WINAPI BASS_MIDI_FontGetPresets(HSOUNDFONT handle) {
    (void)handle;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassLogNow("stub called: BASS_MIDI_FontGetPresets -> ILLPARAM/unsupported");
    g_bassLastError = 0;
    return 0u;
}

BOOL WINAPI BASS_MIDI_InInit(DWORD device, const void* proc, void* user) {
    (void)device; (void)proc; (void)user;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

BOOL WINAPI BASS_MIDI_InFree(DWORD device) {
    (void)device;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

BOOL WINAPI BASS_MIDI_InStart(DWORD device) {
    (void)device;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

BOOL WINAPI BASS_MIDI_InStop(DWORD device) {
    (void)device;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

BOOL WINAPI BASS_MIDI_InGetDeviceInfo(DWORD device, void* info) {
    (void)device; (void)info;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    g_bassLastError = SVMS_ILLPARAM();
    return FALSE;
}

// ── The render pull ──────────────────────────────────────────────────────
// Serves BASS_ChannelGetData: delivers the requested bytes from a render-ahead
// cache, refilling it from the offline session in bounded chunks.  Handles the
// float/16-bit formats and the BASS_DATA_* query flags.  DSP callbacks are run
// by the BASS_ChannelGetData wrapper below, outside the shim lock.
static DWORD BassChannelGetDataCore(DWORD handle, void* buffer, DWORD length) {
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
    const bool pumpAlive = !stream->fileStream && stream->lastSubmitTickMs != 0u &&
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
        LARGE_INTEGER chunkStart{};
        QueryPerformanceCounter(&chunkStart);
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
        const bool hadLive = !window.empty();
        stream->pending.swap(stillPending);
        if (!stream->kmcMode) {
            // File events (already frame-sorted) go first: ties keep file order.
            std::vector<BassFileEv> batch;
            uint32_t fileFrame = 0u, fileMessage = 0u;
            bool more = BassFilePeek(*stream, fileFrame, fileMessage);
            while (more && fileFrame < windowEnd) {
                const uint64_t seq = stream->fileSeq++;
                if (!stream->holdFrames || !BassHoldDelays(*stream, fileFrame, fileMessage, seq))
                    batch.push_back({fileFrame, seq, fileMessage});
                BassFilePop(*stream);
                more = BassFilePeek(*stream, fileFrame, fileMessage);
            }
            bool delayed = false;
            while (!stream->heldOffs.empty() && stream->heldOffs.top().frame < windowEnd) {
                batch.push_back(stream->heldOffs.top());
                stream->heldOffs.pop();
                delayed = true;
            }
            if (delayed)
                std::sort(batch.begin(), batch.end(),
                          [](const BassFileEv& x, const BassFileEv& y) { return y > x; });
            if (!batch.empty()) {
                std::vector<SVMS_OfflineEvent> fromFile;
                fromFile.reserve(batch.size() + window.size());
                for (const BassFileEv& e : batch)
                    fromFile.push_back(BassPackEvent(
                        static_cast<uint32_t>(e.frame - static_cast<uint32_t>(stream->renderedFrames)),
                        e.message));
                fromFile.insert(fromFile.end(), window.begin(), window.end());
                window.swap(fromFile);
            }
        }
        if (hadLive)
            std::stable_sort(window.begin(), window.end(),
                             [](const SVMS_OfflineEvent& a,
                                const SVMS_OfflineEvent& b) {
                                 return a.frame_offset < b.frame_offset;
                             });

        if (stream->kmcMode) BassKmcRead(*stream, chunk);
        if (!stream->kmcMode &&
            NativeRenderOffline(stream->session, window.data(),
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
        LARGE_INTEGER chunkEnd{}, freq{};
        QueryPerformanceCounter(&chunkEnd);
        QueryPerformanceFrequency(&freq);
        const double load = 100.0 *
            (double(chunkEnd.QuadPart - chunkStart.QuadPart) / double(freq.QuadPart)) /
            (double(chunk) / double(stream->sampleRate));
        stream->cpuLoad += 0.25f * (static_cast<float>(load) - stream->cpuLoad);
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

// BASS_ChannelGetData as the host sees it: the core pull plus the stream's
// FX volume and DSP chain. bassenc (BASS_Encode_Start) works by installing a
// DSP that receives every pulled block, so KMC's WAV export runs through
// here. DSP procs are called outside the shim lock (they may call back into
// BASS). Higher priority runs first, like BASS.
DWORD WINAPI BASS_ChannelGetData(DWORD handle, void* buffer, DWORD length) {
    const DWORD got = BassChannelGetDataCore(handle, buffer, length);
    if (got == 0u || got >= 0x80000000u || !buffer) return got;
    std::vector<BassDspEntry> dsps;
    float volume = 1.0f;
    bool floating = true;
    {
        std::lock_guard<std::mutex> lock(g_bassMutex);
        BassStream* stream = BassStreamResolve(handle);
        if (!stream) return got;
        dsps = stream->dsps;
        volume = stream->fxVolume;
        floating = stream->floating;
    }
    if (volume != 1.0f) {
        if (floating) {
            float* p = static_cast<float*>(buffer);
            for (DWORD i = 0u; i < got / 4u; ++i) p[i] *= volume;
        } else {
            int16_t* p = static_cast<int16_t*>(buffer);
            for (DWORD i = 0u; i < got / 2u; ++i) {
                const float v = p[i] * volume;
                p[i] = static_cast<int16_t>(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
            }
        }
    }
    std::stable_sort(dsps.begin(), dsps.end(),
                     [](const BassDspEntry& a, const BassDspEntry& b) { return a.priority > b.priority; });
    for (const BassDspEntry& d : dsps) {
        using Proc = void(WINAPI*)(DWORD, DWORD, void*, DWORD, void*);
        reinterpret_cast<Proc>(d.proc)(d.handle, handle, buffer, got, d.user);
    }
    return got;
}

DWORD WINAPI BASS_ChannelSetDSP(DWORD handle, void* proc, void* user, int priority) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream || !proc) { g_bassLastError = stream ? 20 : 5; return 0u; }
    static DWORD nextDsp = 0x7d000000u;
    BassDspEntry entry{++nextDsp, proc, user, priority};
    stream->dsps.push_back(entry);
    g_bassLastError = 0;
    return entry.handle;
}

BOOL WINAPI BASS_ChannelRemoveDSP(DWORD handle, DWORD dsp) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) { g_bassLastError = 5; return FALSE; }
    const auto it = std::remove_if(stream->dsps.begin(), stream->dsps.end(),
                                   [&](const BassDspEntry& e) { return e.handle == dsp; });
    const bool found = it != stream->dsps.end();
    stream->dsps.erase(it, stream->dsps.end());
    g_bassLastError = found ? 0 : 5;
    return found ? TRUE : FALSE;
}

// Syncs are accepted and never fire: decode pumps (KMC's tempo/end hooks)
// only use them for display and statistics.
DWORD WINAPI BASS_ChannelSetSync(DWORD handle, DWORD type, unsigned long long param,
                                 void* proc, void* user) {
    (void)type; (void)param; (void)proc; (void)user;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (!BassStreamResolve(handle)) { g_bassLastError = 5; return 0u; }
    static DWORD nextSync = 0x7e000000u;
    g_bassLastError = 0;
    return ++nextSync;
}

BOOL WINAPI BASS_ChannelRemoveSync(DWORD handle, DWORD sync) {
    (void)handle; (void)sync;
    return TRUE;
}

// FX: only the volume effect (BASS_FX_VOLUME = 9) has an effect, applied to
// the pulled samples in BASS_ChannelGetData. Other types are accepted.
DWORD WINAPI BASS_ChannelSetFX(DWORD handle, DWORD type, int priority) {
    (void)priority;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    if (!BassStreamResolve(handle)) { g_bassLastError = 5; return 0u; }
    g_bassFx.push_back({handle, type});
    g_bassLastError = 0;
    return 0x7f000000u + static_cast<DWORD>(g_bassFx.size());
}

BOOL WINAPI BASS_ChannelRemoveFX(DWORD handle, DWORD fx) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    const DWORD index = fx - 0x7f000000u;
    if (stream && index >= 1u && index <= g_bassFx.size() && g_bassFx[index - 1u].type == 9u)
        stream->fxVolume = 1.0f;
    return TRUE;
}

// Tags: Bass.Net implements BASS_MIDI_StreamGetTrackCount by counting
// BASS_TAG_MIDI_TRACK (0x11000 + track) entries until one is NULL, so every
// real track of a file stream answers with an (empty) text entry.
const void* WINAPI BASS_ChannelGetTags(DWORD handle, DWORD tags) {
    static const char kEmpty[4] = {0, 0, 0, 0};
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) { g_bassLastError = 5; return nullptr; }
    g_bassLastError = 0;
    if (stream->fileStream && tags >= 0x11000u && tags - 0x11000u < stream->trackCount) return kEmpty;
    g_bassLastError = 23;  // BASS_ERROR_NOTAVAIL
    return nullptr;
}


// ── Bass.Net surface stubs ────────────────────────────────────────────────
// Bass.Net's managed wrappers touch more of bass.dll than a decode pump needs
// (BASS_MIDI_StreamGetTrackCount alone calls BASS_GetCPU), and a missing
// export is an EntryPointNotFoundException. These are no-op stubs for the
// device, 3D, sample, music, plugin and record families: they report failure
// (or success for the harmless "start/stop/update" ones) and never touch the
// engine. x64 only: the generic zero-argument signature relies on the caller
// owning the argument cleanup, which is not true for 32-bit stdcall.
#if defined(_WIN64)
extern "C" {
__declspec(dllexport) float WINAPI BASS_GetCPU(void) { return 0.0f; }
__declspec(dllexport) float WINAPI BASS_GetVolume(void) { return 1.0f; }
__declspec(dllexport) DWORD WINAPI BASS_Apply3D(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelGet3DAttributes(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelGet3DPosition(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelGetAttributeEx(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelGetDevice(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelGetLevel(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelGetLevelEx(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelIsSliding(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelLock(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelPause(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelPlay(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelRemoveLink(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelSet3DAttributes(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelSet3DPosition(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelSetAttributeEx(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelSetDevice(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelSetLink(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelSlideAttribute(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelStop(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_ChannelUpdate(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_Get3DFactors(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_Get3DPosition(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_GetDSoundObject(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_GetDevice(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_GetDeviceInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_GetEAXParameters(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_GetInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_IsStarted(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_MusicFree(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_MusicLoad(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_Pause(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_PluginFree(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_PluginGetInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_PluginLoad(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordFree(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordGetDevice(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordGetDeviceInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordGetInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordGetInput(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordGetInputName(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordInit(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordSetDevice(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordSetInput(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_RecordStart(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleCreate(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleFree(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleGetChannel(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleGetChannels(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleGetData(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleGetInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleLoad(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleSetData(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleSetInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SampleStop(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_Set3DFactors(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_Set3DPosition(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SetConfigPtr(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_SetDevice(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_SetEAXParameters(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_SetVolume(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_Start(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_Stop(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_StreamCreate(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_StreamCreateFile(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_StreamCreateFileUser(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_StreamCreateURL(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_StreamGetFilePosition(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_StreamPutData(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_StreamPutFileData(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_Update(void) { return 1u; }
}  // extern "C"
#endif

const void* WINAPI BASS_GetConfigPtr(DWORD option) {
    (void)option;
    return nullptr;
}

int WINAPI BASS_MIDI_StreamGetTrackCount(HSTREAM handle) {
    std::lock_guard<std::mutex> lock(g_bassMutex);
    BassStream* stream = BassStreamResolve(handle);
    if (!stream) { g_bassLastError = 5; return -1; }
    g_bassLastError = 0;
    return static_cast<int>(stream->trackCount);
}

// ── Channel position, attributes and info ────────────────────────────────

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
    return !stream->fileStream && stream->lastSubmitTickMs != 0u &&
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
            g_bassLastError = SVMS_ILLPARAM();  // BASS_ERROR_ILLPARAM
            return FALSE;
        }
        const uint32_t voices = static_cast<uint32_t>(value);
        g_bassLastError = 0;
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
        g_bassLastError = 0;
        return TRUE;
    }
    g_bassLastError = 0;
    return TRUE;  // permissive: unrecognized attribs succeed as no-ops
}

BOOL WINAPI BASS_ChannelGetAttribute(DWORD handle, DWORD attrib,
                                     float* value) {
    constexpr DWORD kBassAttribMidiVoices = 0x12003u;
    constexpr DWORD kBassAttribMidiVoicesActive = 0x12004u;
    constexpr DWORD kBassAttribMidiSrc = 0x12006u;
    std::lock_guard<std::mutex> lock(g_bassMutex);
    const bool isChannel = (handle & 0xff000000u) == 0x7e000000u;
    BassStream* stream = BassStreamResolve(isChannel ? (handle >> 8) & 0xffffu : handle);
    if (!stream || !value) {
        g_bassLastError = stream ? 20 : 5;
        return FALSE;
    }
    if (attrib == kBassAttribMidiVoices) {
        *value = static_cast<float>(stream->voiceLimit);
        return TRUE;
    }
    if (isChannel && attrib == kBassAttribMidiVoicesActive) {
        uint32_t perChannel[16] = {};
        if (stream->kmc) SvmsBassKmcApi()->Channels(stream->kmc, perChannel);
        else NativeGetOfflineChannelVoices(stream->session, perChannel);
        *value = static_cast<float>(perChannel[handle & 15u]);
        g_bassLastError = 0;
        return TRUE;
    }
    if (attrib == kBassAttribMidiVoicesActive) {
        if (stream->kmc) {
            *value = static_cast<float>(SvmsBassKmcApi()->Active(stream->kmc));
            return TRUE;
        }
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
    if (attrib == 7u) {  // BASS_ATTRIB_CPU
        *value = stream->cpuLoad;
        return TRUE;
    }
    // Hosts poll attribs (PPQN, STATE, SPEED, ...) from a UI
    // thread while rendering; failing here left a stale ILLPARAM that the host
    // then read as the error of its next, unrelated call. Report 0 and succeed.
    static DWORD lastUnknown = 0u;
    if (attrib != lastUnknown) {
        lastUnknown = attrib;
        BassLogNow("GetAttribute: unmodelled attrib %#X -> 0", attrib);
    }
    *value = 0.0f;
    return TRUE;
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
    // BASS_FX_VOLUME_PARAM { float fTarget; float fCurrent; float fTime; DWORD lCurve; }:
    // applied instantly (fTime is ignored; KMC sets the target with time 0).
    std::lock_guard<std::mutex> lock(g_bassMutex);
    const DWORD index = handle - 0x7f000000u;
    if (params && index >= 1u && index <= g_bassFx.size() && g_bassFx[index - 1u].type == 9u) {
        BassStream* stream = BassStreamResolve(g_bassFx[index - 1u].stream);
        if (stream) std::memcpy(&stream->fxVolume, params, sizeof(float));
    }
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
    // BASS_ENCODE_AUTOFREE: an encoder dies (and a WAV gets its final sizes) with its channel.
    // Called before the lock because stopping an encoder removes its DSP through this module.
    if (HMODULE enc = GetModuleHandleW(L"bassenc.dll"))
        if (auto stop = reinterpret_cast<BOOL(WINAPI*)(DWORD)>(GetProcAddress(enc, "BASS_Encode_Stop")))
            stop(handle);
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
