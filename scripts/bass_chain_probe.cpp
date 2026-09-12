// Exercises the exact Kiva prerender sequence against our bass.dll:
// Init -> FontInit(path) -> FontLoad -> StreamCreate(16, FLOAT|DECODE) ->
// StreamEvents (TIME|STRUCT then RAW) -> ChannelGetData -> StreamFree.
// Also: font slots + StreamSetFonts FONT/FONTEX (count-flag formats),
// BASS_ATTRIB_MIDI_VOICES rebuild, the 0x40000000 BASS_DATA_FLOAT pull
// flag, SOUNDOFF/RESET/KEYPRES struct translation, available-bytes query.
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>

typedef DWORD HSTREAM;
typedef DWORD HSOUNDFONT;

static const DWORD kDataFloat = 0x40000000u;  // BASS_DATA_FLOAT (reflected)

int main(int argc, char** argv) {
    _putenv("SVMS_BASS_QUIET_MS=250");
    const char* sf2Path = argc > 1
        ? argv[1]
        : "E:\\backup\\Misc\\Black MIDI\\omv2 with zmp PFAViz\\Morphine Piano.sf2";

    // Absolute path: a real Un4seen bass.dll sits in the repo root and
    // shadows ours through the DLL search path.
    char bassPath[MAX_PATH];
    GetModuleFileNameA(nullptr, bassPath, MAX_PATH);
    char* slash = strrchr(bassPath, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(bassPath, "..\\build\\V3\\bin\\bass.dll");
    HMODULE bass = LoadLibraryA(bassPath);
    if (!bass) { printf("LoadLibrary bass.dll FAILED %lu\n", GetLastError()); return 1; }
    char loaded[MAX_PATH] = {};
    GetModuleFileNameA(bass, loaded, MAX_PATH);
    printf("bass.dll loaded: %s\n", loaded);

    auto fn = [&](const char* name) -> void* {
        void* p = (void*)GetProcAddress(bass, name);
        if (!p) printf("MISSING EXPORT: %s\n", name);
        return p;
    };
    using InitFn = BOOL(WINAPI*)(int, DWORD, DWORD, HWND, const GUID*);
    using FontInitFn = HSOUNDFONT(WINAPI*)(const void*, DWORD);
    using FontLoadFn = BOOL(WINAPI*)(HSOUNDFONT, int, int);
    using CreateFn = HSTREAM(WINAPI*)(DWORD, DWORD, DWORD);
    using EventsFn = DWORD(WINAPI*)(HSTREAM, DWORD, const void*, DWORD);
    using GetDataFn = DWORD(WINAPI*)(HSTREAM, void*, DWORD);
    using ErrFn = int(WINAPI*)(void);
    using FreeFn = BOOL(WINAPI*)(HSTREAM);

    auto init = (InitFn)fn("BASS_Init");
    auto fontInit = (FontInitFn)fn("BASS_MIDI_FontInit");
    auto fontLoad = (FontLoadFn)fn("BASS_MIDI_FontLoad");
    auto streamCreate = (CreateFn)fn("BASS_MIDI_StreamCreate");
    auto streamEvents = (EventsFn)fn("BASS_MIDI_StreamEvents");
    auto getData = (GetDataFn)fn("BASS_ChannelGetData");
    auto errCode = (ErrFn)fn("BASS_ErrorGetCode");
    auto streamFree = (FreeFn)fn("BASS_StreamFree");
    if (!init || !fontInit || !fontLoad || !streamCreate || !streamEvents ||
        !getData || !errCode || !streamFree) return 1;

    if (!init(0, 48000, 0, nullptr, nullptr)) { printf("BASS_Init failed err=%d\n", errCode()); return 1; }
    HSOUNDFONT font = fontInit(sf2Path, 0);
    printf("FontInit -> %u err=%d\n", font, errCode());
    if (!font) return 1;
    if (!fontLoad(font, 0, 0)) { printf("FontLoad failed err=%d\n", errCode()); return 1; }

    const DWORD flags = 0x100u | 0x200000u;  // FLOAT | DECODE
    HSTREAM stream = streamCreate(16, flags, 48000);
    printf("StreamCreate(16, FLOAT|DECODE, 48000) -> %u err=%d\n", stream, errCode());
    if (!stream) return 1;

#pragma pack(push, 1)
    struct BassMidiEvent { DWORD type, param, chan, tick, time; };
#pragma pack(pop)
    // TIME|STRUCT mode (0x1000000): one note-on at 100 ms, note-off at 1000 ms.
    BassMidiEvent evs[2] = {
        {1u, 0x4040u, 0u, 0u, 100u},   // MIDI_EVENT_NOTE, note 64 vel 64
        {1u, 0x4000u, 0u, 0u, 1000u},  // note 64 vel 0
    };
    DWORD accepted = streamEvents(stream, 0x1000000u, evs, sizeof(evs));
    printf("StreamEvents TIME|STRUCT -> %u accepted, err=%d\n", accepted, errCode());

    float buf[48000 * 2] = {};
    DWORD got = getData(stream, buf, sizeof(buf) | kDataFloat);
    double peak = 0.0;
    for (int i = 0; i < 48000 * 2; ++i) {
        double a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    printf("GetData 1s -> %u bytes err=%d peak=%.5f\n", got, errCode(), peak);

    // RAW mode (0x10000) with channel folded into the high byte.
    uint8_t raw[64];
    DWORD* w = (DWORD*)raw;
    w[0] = 2000u; w[1] = 3u;  // tick 2000ms, 3 bytes
    raw[8] = 0x90u; raw[9] = 0x3Cu; raw[10] = 0x40u;
    std::memcpy(raw + 11, &(w[3] = 2600u), 4u);
    std::memcpy(raw + 15, &(w[4] = 3u), 4u);
    raw[8 + 11] = 0x80u; raw[9 + 11] = 0x3Cu; raw[10 + 11] = 0x00u;
    accepted = streamEvents(stream, 0x10000u | (0u << 24), raw, 11u + 8u + 3u);
    printf("StreamEvents RAW -> %u accepted, err=%d\n", accepted, errCode());

    got = getData(stream, buf, sizeof(buf) | kDataFloat);
    peak = 0.0;
    for (int i = 0; i < 48000 * 2; ++i) {
        double a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    printf("GetData 1s (after raw) -> %u bytes err=%d peak=%.5f\n", got, errCode(), peak);

    // Stream end: events stop at 2600 ms; the stream ends at 4.6 s.
    // Drain: every pull full until the crossing (partial), then -1/45.
    int pulls = 0;
    for (;;) {
        got = getData(stream, buf, sizeof(buf) | kDataFloat);
        ++pulls;
        printf("Drain pull %d -> %u bytes err=%d\n", pulls, got, errCode());
        if (got == (DWORD)-1) {
            if (errCode() != 45) { printf("FAIL: expected ENDED 45\n"); return 1; }
            break;
        }
        if (pulls > 4000) { printf("FAIL: stream never ended\n"); return 1; }
        // 0-returns are legal inside the quiescence window (250 ms here).
        if (got == 0) { Sleep(1); continue; }
    }

    // ── SYNC (flagless) events: realtime player contract ──────────────
    // New stream. Send a note flagless NOW, pull 0.5 s -> that window must
    // sound (the note was pending at pull time). Then a second stream
    // where the pull happens BEFORE the send -> that window must be
    // silent.
    auto makeStream = [&]() -> HSTREAM {
        HSTREAM s2 = streamCreate(16, flags, 48000);
        if (!s2) { printf("FAIL: stream2 create err=%d\n", errCode()); exit(1); }
        return s2;
    };
    using SendRawFn = DWORD(WINAPI*)(HSTREAM, DWORD, const void*, DWORD);
    auto sendRaw = (SendRawFn)GetProcAddress(bass, "BASS_MIDI_StreamEvents");

    // ── Real-call-shape tests ──────────────────────────────────────────
    // (a) Kiva prerender: RAW without TIME, plain 3-byte MIDI message
    //     (SendEventRaw passes just the message bytes) -> SYNC anchor at
    //     the pull cursor.
    HSTREAM sa = makeStream();
    uint32_t syncMsg = 0x90u | (60u << 8) | (100u << 16);
    DWORD sent = sendRaw(sa, 0x10000u, &syncMsg, 3u);
    printf("SYNC(RAW no-TIME) send -> %u accepted (want 1)\n", sent);
    memset(buf, 0, sizeof(buf));
    got = getData(sa, buf, (48000 / 2) * 8 | kDataFloat);
    double peakA = 0.0;
    for (int i = 0; i < 48000; ++i) {
        double a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peakA) peakA = a;
    }
    printf("Pull-after-send 0.5s peak=%.5f (want >0.01)\n", peakA);
    streamFree(sa);

    // (b) Same send AFTER a pull: earlier window must stay silent.
    HSTREAM sb = makeStream();
    memset(buf, 0, sizeof(buf));
    getData(sb, buf, (48000 / 2) * 8 | kDataFloat);
    double peakB0 = 0.0;
    for (int i = 0; i < 48000; ++i) {
        double a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peakB0) peakB0 = a;
    }
    sendRaw(sb, 0x10000u, &syncMsg, 3u);
    memset(buf, 0, sizeof(buf));
    getData(sb, buf, (48000 / 2) * 8 | kDataFloat);
    double peakB1 = 0.0;
    for (int i = 0; i < 48000; ++i) {
        double a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peakB1) peakB1 = a;
    }
    printf("Send-after-pull: pre=%.5f (want 0) post=%.5f (want >0.01)\n",
           peakB0, peakB1);
    streamFree(sb);

    // (c) TIME|STRUCT (0x1000000): note positioned by the ms field.
    HSTREAM sc2 = makeStream();
    BassMidiEvent cev = {1u, 0x4064u, 0u, 0u, 100u};  // note 60 vel 100 @100ms
    sent = sendRaw(sc2, 0x1000000u, &cev, sizeof(cev));
    printf("TIME|STRUCT send -> %u accepted (want 1)\n", sent);
    memset(buf, 0, sizeof(buf));
    getData(sc2, buf, (48000 / 2) * 8 | kDataFloat);
    double peakC = 0.0;
    for (int i = 0; i < 48000; ++i) {
        double a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peakC) peakC = a;
    }
    printf("TIME|STRUCT first 0.5s peak=%.5f (want >0.01)\n", peakC);
    streamFree(sc2);

    // ── Font slots + StreamSetFonts formats + attribs ──────────────────
    using SetFontsFn = BOOL(WINAPI*)(HSTREAM, const void*, DWORD);
    using SetAttrFn = BOOL(WINAPI*)(HSTREAM, DWORD, float);
    using GetAttrFn = BOOL(WINAPI*)(HSTREAM, DWORD, float*);
    auto setFonts = (SetFontsFn)fn("BASS_MIDI_StreamSetFonts");
    auto setAttr = (SetAttrFn)fn("BASS_ChannelSetAttribute");
    auto getAttr = (GetAttrFn)fn("BASS_ChannelGetAttribute");
    if (!setFonts || !setAttr || !getAttr) return 1;

    // Available-bytes query: (nullptr, 0) must return the pending tail.
    HSTREAM sd = makeStream();
    DWORD avail = getData(sd, nullptr, 0);
    printf("Available query -> %u bytes (want >0, err=%d)\n", avail, errCode());

    // Second font slot: handles must be distinct and resolvable.
    HSOUNDFONT font2 = fontInit(sf2Path, 0);
    printf("FontInit #2 -> %u (want != %u)\n", font2, font);
    if (!font2 || font2 == font) return 1;

#pragma pack(push, 1)
    struct BassFontEx { DWORD font; int spreset, sbank, dpreset, dbank, dbanklsb; };
    struct BassFont12 { DWORD font; int preset, bank; };
#pragma pack(pop)
    // FONTEX default config, count|0x1000000: priority = font (the FIRST
    // slot), font2 behind it.
    BassFontEx fex[2] = {
        {font, -1, -1, -1, 0, 0},
        {font2, -1, -1, -1, 0, 0},
    };
    if (!setFonts(0, fex, 2u | 0x1000000u)) {
        printf("FAIL: StreamSetFonts(0, FONTEX) err=%d\n", errCode());
        return 1;
    }
    printf("StreamSetFonts(0, FONTEX x2) -> ok\n");
    // Bad font handle must fail.
    BassFontEx fexBad[1] = {{99u, -1, -1, -1, 0, 0}};
    if (setFonts(0, fexBad, 1u | 0x1000000u)) {
        printf("FAIL: bad font handle accepted\n");
        return 1;
    }
    printf("StreamSetFonts bad handle -> rejected (err=%d, want 20)\n",
           errCode());
    // Plain BASS_MIDI_FONT (12-byte) form.
    BassFont12 f12[1] = {{font, -1, -1}};
    if (!setFonts(0, f12, 1u)) {
        printf("FAIL: StreamSetFonts(0, FONT) err=%d\n", errCode());
        return 1;
    }
    printf("StreamSetFonts(0, FONT x1) -> ok\n");

    // MIDI_VOICES: set after create (Kiva's order) must rebuild the session
    // and round-trip through GetAttribute.
    HSTREAM se = makeStream();
    if (!setAttr(se, 0x12003u, 1234.0f)) {
        printf("FAIL: SetAttribute(MIDI_VOICES) err=%d\n", errCode());
        return 1;
    }
    float voices = 0.0f;
    if (!getAttr(se, 0x12003u, &voices) || (int)voices != 1234) {
        printf("FAIL: GetAttribute(MIDI_VOICES) -> %.1f err=%d\n", voices,
               errCode());
        return 1;
    }
    printf("MIDI_VOICES 1234 rebuild + round-trip ok\n");

    // Lifecycle struct events translate (SOUNDOFF=16, RESET=17, KEYPRES=71).
    HSTREAM sf2s = makeStream();
    BassMidiEvent life[3] = {
        {1u, 0x3C40u, 0u, 0u, 0u},     // note 60 on
        {16u, 0u, 0u, 0u, 200u},       // SOUNDOFF
        {71u, (60u << 16) | 40u, 0u, 0u, 300u},  // KEYPRES key 60 pressure 40
    };
    sent = streamEvents(sf2s, 0u, life, sizeof(life));
    printf("STRUCT lifecycle/KEYPRES -> %u accepted (want 3)\n", sent);
    if (sent != 3u) return 1;
    streamFree(sf2s);
    streamFree(sd);

    // ── Singular BASS_MIDI_StreamEvent (PFA imports it) ─────────────────
    using StreamEventFn = DWORD(WINAPI*)(HSTREAM, DWORD, DWORD, DWORD);
    auto streamEvent = (StreamEventFn)fn("BASS_MIDI_StreamEvent");
    using GetEventFn = DWORD(WINAPI*)(HSTREAM, DWORD, DWORD);
    auto getEvent = (GetEventFn)fn("BASS_MIDI_StreamGetEvent");
    if (!streamEvent || !getEvent) return 1;
    HSTREAM sg = makeStream();
    DWORD r = streamEvent(sg, 0u, 1u /*NOTE*/, 0x4040u);  // note 64 vel 64
    printf("StreamEvent NOTE -> %X (want 4040)\n", r);
    DWORD g = getEvent(sg, 0u, 1u);
    printf("StreamGetEvent NOTE -> %X (want 4040)\n", g);
    memset(buf, 0, sizeof(buf));
    getData(sg, buf, (48000 / 2) * 8 | kDataFloat);
    double peakG = 0.0;
    for (int i = 0; i < 48000; ++i) {
        double a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peakG) peakG = a;
    }
    printf("StreamEvent pull 0.5s peak=%.5f (want >0.01)\n", peakG);
    streamFree(sg);

    printf("OK\n");
    return 0;
}
