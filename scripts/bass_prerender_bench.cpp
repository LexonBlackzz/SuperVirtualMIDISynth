// Throughput harness for the BASSMIDI prerender shim. Drives a
// chopped-notes Black MIDI-style load (note churn) and a sustained load
// through BASS_MIDI_StreamEvents (RAW+TIME byte positions) + GetData, and
// reports audio-seconds rendered per wall-second ("x realtime") for big and
// small pull sizes. Mirrors Kiva's prerender pump so shim-side overhead
// (per-pull planning, chunking) shows up in the numbers.
//
// Build: scripts\build_bass_prerender_bench.bat
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <vector>

typedef DWORD HSTREAM;
typedef DWORD HSOUNDFONT;

static const DWORD kDataFloat = 0x40000000u;

static int RunLivePump(const char* sf2Path);

int main(int argc, char** argv) {
    const char* sf2Path = argc > 1
        ? argv[1]
        : "E:\\backup\\Misc\\Black MIDI\\omv2 with zmp PFAViz\\Morphine Piano.sf2";
    if (argc > 2 && strcmp(argv[2], "--livepump") == 0)
        return RunLivePump(sf2Path);

    char bassPath[MAX_PATH];
    GetModuleFileNameA(nullptr, bassPath, MAX_PATH);
    char* slash = strrchr(bassPath, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(bassPath, "..\\build\\V3\\bin\\bass.dll");
    HMODULE bass = LoadLibraryA(bassPath);
    if (!bass) { printf("LoadLibrary bass.dll FAILED %lu\n", GetLastError()); return 1; }

    auto fn = [&](const char* name) -> void* {
        void* p = (void*)GetProcAddress(bass, name);
        if (!p) printf("MISSING EXPORT: %s\n", name);
        return p;
    };
    using InitFn = BOOL(WINAPI*)(int, DWORD, DWORD, HWND, const GUID*);
    using FontInitFn = HSOUNDFONT(WINAPI*)(const void*, DWORD);
    using CreateFn = HSTREAM(WINAPI*)(DWORD, DWORD, DWORD);
    using EventsFn = DWORD(WINAPI*)(HSTREAM, DWORD, const void*, DWORD);
    using GetDataFn = DWORD(WINAPI*)(HSTREAM, void*, DWORD);
    using ErrFn = int(WINAPI*)(void);
    using FreeFn = BOOL(WINAPI*)(HSTREAM);
    using SetAttrFn = BOOL(WINAPI*)(HSTREAM, DWORD, float);
    using ActiveFn = DWORD(WINAPI*)(HSTREAM);

    auto init = (InitFn)fn("BASS_Init");
    auto fontInit = (FontInitFn)fn("BASS_MIDI_FontInit");
    auto streamCreate = (CreateFn)fn("BASS_MIDI_StreamCreate");
    auto streamEvents = (EventsFn)fn("BASS_MIDI_StreamEvents");
    auto getData = (GetDataFn)fn("BASS_ChannelGetData");
    auto errCode = (ErrFn)fn("BASS_ErrorGetCode");
    auto streamFree = (FreeFn)fn("BASS_StreamFree");
    auto setAttr = (SetAttrFn)fn("BASS_ChannelSetAttribute");
    auto isActive = (ActiveFn)fn("BASS_ChannelIsActive");
    if (!init || !fontInit || !streamCreate || !streamEvents || !getData ||
        !errCode || !streamFree || !setAttr || !isActive) return 1;

    if (!init(0, 48000, 0, nullptr, nullptr)) { printf("BASS_Init failed\n"); return 1; }
    HSOUNDFONT font = fontInit(sf2Path, 0);
    if (!font) { printf("FontInit failed err=%d\n", errCode()); return 1; }

    const DWORD flags = 0x100u | 0x200000u;  // FLOAT | DECODE
    const uint32_t kRate = 48000u;

    // Build a chopped-notes event list: note rate / s, key churn, fixed
    // note length. Events as RAW+TIME blocks: (u32 pos, u32 len, 3 bytes).
    struct Load { const char* name; double noteRate; uint32_t noteLenFrames; uint32_t keyCount; double seconds; };
    const Load loads[] = {
        { "chopped r16k len512 88keys", 16000.0, 512u, 88u, 10.0 },
        { "chopped r8k len512 88keys",   8000.0, 512u, 88u, 10.0 },
        { "sustained r100 len1s 127keys", 100.0, (uint32_t)(kRate * 1), 127u, 10.0 },
    };

    for (const Load& load : loads) {
        // Build all events.
        std::vector<uint8_t> raw;
        const uint32_t totalFrames = (uint32_t)(load.seconds * kRate);
        const double perNote = kRate / load.noteRate;  // frames between note-ons
        uint64_t noteOns = 0;
        uint32_t key = 0;
        for (double t = 0.0; t < (double)totalFrames; t += perNote) {
            const uint32_t onFrame = (uint32_t)t;
            const uint32_t offFrame = onFrame + load.noteLenFrames;
            key = (uint32_t)(noteOns % load.keyCount);
            const uint8_t note = (uint8_t)(36u + (key % 60u));
            const uint8_t ch = (uint8_t)(noteOns & 15u);
            // note-on
            {
                const uint32_t pos = onFrame * 8u;
                uint8_t blk[11];
                std::memcpy(blk, &pos, 4u);
                const uint32_t len = 3u;
                std::memcpy(blk + 4, &len, 4u);
                blk[8] = (uint8_t)(0x90u | ch); blk[9] = note; blk[10] = 100u;
                raw.insert(raw.end(), blk, blk + 11);
            }
            // note-off
            if (offFrame < totalFrames) {
                const uint32_t pos = offFrame * 8u;
                uint8_t blk[11];
                std::memcpy(blk, &pos, 4u);
                const uint32_t len = 3u;
                std::memcpy(blk + 4, &len, 4u);
                blk[8] = (uint8_t)(0x80u | ch); blk[9] = note; blk[10] = 0u;
                raw.insert(raw.end(), blk, blk + 11);
            }
            ++noteOns;
        }
        // note-offs past the last on may exceed totalFrames — the stream
        // end = maxEventFrame + 2s tail, so extend the drain target.
        const uint32_t streamEndFrames = totalFrames + 2u * kRate;

        for (int pullVariant = 0; pullVariant < 2; ++pullVariant) {
            const uint32_t pullFrames =
                pullVariant == 0 ? 65536u : 2048u;   // big pull vs Kiva-ish
            HSTREAM s = streamCreate(16, flags, kRate);
            if (!s) { printf("StreamCreate failed err=%d\n", errCode()); return 1; }
            setAttr(s, 0x12003u, 8192.0f);  // generous pool
            DWORD sent = streamEvents(s, 0x10000u | 0x8000000u, raw.data(),
                                      (DWORD)raw.size());
            if (sent == 0u || sent == (DWORD)-1) {
                printf("StreamEvents failed err=%d\n", errCode());
                return 1;
            }

            std::vector<float> buf(pullFrames * 2u);
            uint64_t rendered = 0u;
            const auto t0 = std::chrono::steady_clock::now();
            DWORD pulls = 0u;
            for (;;) {
                DWORD got = getData(s, buf.data(),
                                    pullFrames * 8u | kDataFloat);
                ++pulls;
                if (got == (DWORD)-1) {
                    if (errCode() != 45) {
                        printf("FAIL: pull err=%d\n", errCode());
                        return 1;
                    }
                    break;
                }
                rendered += got / 8u;
                if (pulls > 1000000u) break;
            }
            const auto t1 = std::chrono::steady_clock::now();
            const double wall = std::chrono::duration<double>(t1 - t0).count();
            const double audio = (double)streamEndFrames / kRate;
            // Pulls before end return 0? Guard: ended at cursor >= streamEnd.
            printf("%-28s pull=%6u  %.2fs audio in %6.3fs wall = %7.2fx realtime "
                   "(events=%zu sent=%u pulls=%u)\n",
                   load.name, pullFrames, audio, wall,
                   audio / (wall > 0.0 ? wall : 1e-9),
                   raw.size() / 11u, sent, pulls);
            streamFree(s);
        }
    }
    printf("OK\n");
    return 0;
}

// ── Kiva-style live pump ────────────────────────────────────────────────
// Events submitted ONE PER StreamEvents call (positionless RAW|NORSTATUS,
// bare 3-byte messages, Kiva's SendEventRaw shape), pulls of ~10 ms between
// event groups. This is the prerender-playback generator pattern.
static int RunLivePump(const char* sf2Path) {
    char bassPath[MAX_PATH];
    GetModuleFileNameA(nullptr, bassPath, MAX_PATH);
    char* slash = strrchr(bassPath, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(bassPath, "..\\build\\V3\\bin\\bass.dll");
    HMODULE bass = LoadLibraryA(bassPath);
    if (!bass) { printf("LoadLibrary FAILED\n"); return 1; }
    using InitFn = BOOL(WINAPI*)(int, DWORD, DWORD, HWND, const GUID*);
    using FontInitFn = HSOUNDFONT(WINAPI*)(const void*, DWORD);
    using CreateFn = HSTREAM(WINAPI*)(DWORD, DWORD, DWORD);
    using EventsFn = DWORD(WINAPI*)(HSTREAM, DWORD, const void*, DWORD);
    using GetDataFn = DWORD(WINAPI*)(HSTREAM, void*, DWORD);
    using SetAttrFn = BOOL(WINAPI*)(HSTREAM, DWORD, float);
    auto init = (InitFn)GetProcAddress(bass, "BASS_Init");
    auto fontInit = (FontInitFn)GetProcAddress(bass, "BASS_MIDI_FontInit");
    auto streamCreate = (CreateFn)GetProcAddress(bass, "BASS_MIDI_StreamCreate");
    auto sendRaw = (EventsFn)GetProcAddress(bass, "BASS_MIDI_StreamEvents");
    auto getData = (GetDataFn)GetProcAddress(bass, "BASS_ChannelGetData");
    auto setAttr = (SetAttrFn)GetProcAddress(bass, "BASS_ChannelSetAttribute");
    if (!init || !fontInit || !streamCreate || !sendRaw || !getData || !setAttr) {
        printf("exports missing\n");
        return 1;
    }
    init(0, 48000, 0, nullptr, nullptr);
    HSOUNDFONT font = fontInit(sf2Path, 0);
    if (!font) { printf("FontInit failed\n"); return 1; }

    struct Load { const char* name; double noteRate; uint32_t lenFrames; uint32_t keys; double seconds; };
    const Load loads[] = {
        { "livepump r16k len512 88keys",  16000.0,  512u,  88u, 8.0 },
        { "livepump r100k len96 127keys", 100000.0,  96u, 127u, 8.0 },
    };
    const DWORD flags = 0x100u | 0x200000u;
    const uint32_t kRate = 48000u;
    const uint32_t window = 480u;  // ~10 ms pull

    for (const Load& load : loads) {
        HSTREAM s = streamCreate(16, flags, kRate);
        if (!s) { printf("StreamCreate failed\n"); return 1; }
        setAttr(s, 0x12003u, 4096.0f);
        std::vector<float> buf(window * 2u);
        const uint32_t totalFrames = (uint32_t)(load.seconds * kRate);
        const double perNote = kRate / load.noteRate;
        uint64_t noteOns = 0u;
        uint32_t pulled = 0u;
        uint32_t sends = 0u;
        const auto t0 = std::chrono::steady_clock::now();
        while (pulled < totalFrames) {
            // Send every event whose time falls in this window, one
            // StreamEvents call each (positionless).
            const double horizon = (double)(pulled + window);
            while (noteOns * perNote < horizon && noteOns * perNote < totalFrames) {
                const uint8_t note = (uint8_t)(36u + (noteOns % load.keys));
                const uint8_t ch = (uint8_t)(noteOns & 15u);
                uint32_t msg = 0x90u | ch | ((uint32_t)note << 8u) | (100u << 16u);
                sendRaw(s, 0x10000u | 0x2000000u, &msg, 3u);
                ++sends;
                const uint32_t off = (uint32_t)(noteOns * perNote) + load.lenFrames;
                if (off < totalFrames) {
                    uint32_t moff = 0x80u | ch | ((uint32_t)note << 8u);
                    sendRaw(s, 0x10000u | 0x2000000u, &moff, 3u);
                    ++sends;
                }
                ++noteOns;
            }
            DWORD got = getData(s, buf.data(), window * 8u | 0x40000000u);
            if (got == (DWORD)-1) break;
            pulled += window;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double>(t1 - t0).count();
        printf("%-30s %6.2fs audio in %6.3fs wall = %7.2fx realtime "
               "(sends=%u)\n", load.name, (double)totalFrames / kRate, wall,
               ((double)totalFrames / kRate) / (wall > 0.0 ? wall : 1e-9),
               sends);
    }
    return 0;
}
