// SVMSV3MidiSong.cpp — "proper MIDI" playback test with audible toggle.
//
// Two engines, one MIDI source:
//
//   offline (default)  Renders an SMF through the production V3 chain
//                      (StandaloneSynth: RenderBlock + limiter) as fast as
//                      the CPU allows.  Silent by construction — no audio
//                      device is opened.  --wav PATH dumps a listenable
//                      16-bit PCM file of exactly what was rendered.
//
//   --realtime         Streams the same SMF into the LIVE winmm.dll with
//                      wall-clock pacing.  Silent by DEFAULT: the test
//                      applies masterVolume=0 through the RuntimeLink live
//                      command path and restores the previous value at exit.
//                      --audible skips the muting so the piece can be heard.
//
// Telemetry (both modes): voice census (active/releasing/tails/steals),
// render-load percentiles (cpvs offline, callback % realtime), dropped
// events, limiter gain reduction.  --verbose prints interval lines.
//
// MIDI source: --midi PATH for any real file (Black MIDIs welcome), or the
// deterministic built-in composition (multi-channel, sustain pedal, pitch
// bend, CC swells) when no file is given.
//
// Exit codes: 0 = pass, 77 = skipped (no soundfont / no MIDI+audio device),
// 1 = failure.
//
#include "SVMSConfig.h"
#include "SVMSMidiStream.h"
#include "SVMSRuntimeLink.h"
#include "SVMSStandaloneSynth.h"

#include <windows.h>
#include <mmsystem.h>
#include <intrin.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#pragma comment(lib, "dbghelp.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ── Options ─────────────────────────────────────────────────────────────────

struct Options {
    std::wstring midiPath;
    std::wstring soundfontPath;
    std::wstring dllPath;
    std::wstring wavPath;
    bool realtime = false;
    bool audible = false;
    bool verbose = false;
    bool quiet = false;
    bool coverage = false;
    uint32_t frames = 2048u;
    uint32_t renderThreads = 0u;      // 0 = automatic
    uint32_t maxVoices = 4096u;
    uint32_t repeat = 1u;             // whole-piece repetitions
    uint32_t realtimeSecondsCap = 0u; // 0 = unlimited (piece + tail)
    uint32_t tailSeconds = 4u;
    double startSeconds = 0.0;        // trim/shift: window starts at 0
    std::string backend = "auto";
};

const wchar_t* WArg(const char* value) {
    static thread_local wchar_t buffer[1024];
    MultiByteToWideChar(CP_UTF8, 0, value, -1, buffer, 1024);
    return buffer;
}

bool ParseArgs(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(arg, "--realtime") == 0) {
            options.realtime = true;
        } else if (std::strcmp(arg, "--audible") == 0) {
            options.audible = true;
        } else if (std::strcmp(arg, "--verbose") == 0) {
            options.verbose = true;
        } else if (std::strcmp(arg, "--coverage") == 0) {
            options.coverage = true;
        } else if (std::strcmp(arg, "--quiet") == 0) {
            options.quiet = true;
        } else if (std::strcmp(arg, "--midi") == 0 && hasValue) {
            options.midiPath = WArg(argv[++i]);
        } else if (std::strcmp(arg, "--soundfont") == 0 && hasValue) {
            options.soundfontPath = WArg(argv[++i]);
        } else if (std::strcmp(arg, "--dll") == 0 && hasValue) {
            options.dllPath = WArg(argv[++i]);
        } else if (std::strcmp(arg, "--wav") == 0 && hasValue) {
            options.wavPath = WArg(argv[++i]);
        } else if (std::strcmp(arg, "--frames") == 0 && hasValue) {
            options.frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(arg, "--render-threads") == 0 && hasValue) {
            options.renderThreads = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(arg, "--voices") == 0 && hasValue) {
            options.maxVoices = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(arg, "--repeat") == 0 && hasValue) {
            options.repeat = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(arg, "--seconds") == 0 && hasValue) {
            options.realtimeSecondsCap = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(arg, "--start-seconds") == 0 && hasValue) {
            options.startSeconds = std::atof(argv[++i]);
            if (options.startSeconds < 0.0) options.startSeconds = 0.0;
        } else if (std::strcmp(arg, "--backend") == 0 && hasValue) {
            options.backend = argv[++i];
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            std::printf(
                "usage: svms_v3_midi_song [--midi PATH] [--soundfont PATH]\n"
                "       [--realtime] [--audible] [--dll PATH] [--wav PATH]\n"
                "       [--frames 16..8192] [--render-threads N] [--voices N]\n"
                "       [--repeat N] [--seconds N] [--start-seconds S]\n"
                "       [--backend auto|scalar|sse2|avx2]\n"
                "       [--verbose] [--quiet]\n"
                "default: offline render of the built-in piece, silent, one JSON\n"
                "summary line.  --realtime drives the live winmm.dll (muted unless\n"
                "--audible).  exit 0 = pass, 77 = skipped.\n");
            return false;
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", arg);
            return false;
        }
    }
    if (options.frames == 0u || options.frames > 8192u) options.frames = 2048u;
    if (options.maxVoices == 0u) options.maxVoices = 4096u;
    if (options.repeat == 0u) options.repeat = 1u;
    return true;
}

// ── Percentile helper (nearest-rank on a copied vector) ─────────────────────

double Percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const double position = fraction * static_cast<double>(samples.size() - 1);
    const size_t index = static_cast<size_t>(position + 0.5);
    return samples[index < samples.size() ? index : samples.size() - 1u];
}

// ── Built-in composition (deterministic "proper MIDI" default) ──────────────
//
// Format-0 SMF, 120 BPM, 24 bars of Am–F–C–G: melody with sustain pedal
// (CC64) on channel 0, eighth-note arpeggio accompaniment on channel 1,
// bass on channel 2, string pad on channel 3.  Program changes, CC11/CC7
// swells and one pitch-bend phrase are included so blocks exercise the
// production dispatch paths (program resolution, pedal release, bend).

void VLQ(std::vector<uint8_t>& out, uint32_t value) {
    uint8_t buffer[5];
    int count = 0;
    buffer[count++] = static_cast<uint8_t>(value & 0x7fu);
    value >>= 7u;
    while (value != 0u) {
        buffer[count++] = static_cast<uint8_t>((value & 0x7fu) | 0x80u);
        value >>= 7u;
    }
    for (int i = count - 1; i >= 0; --i) out.push_back(buffer[i]);
}

void TrackEvent(std::vector<uint8_t>& track, uint32_t deltaTicks,
                uint8_t status, uint8_t data1, uint8_t data2) {
    VLQ(track, deltaTicks);
    track.push_back(status);
    track.push_back(data1);
    track.push_back(data2);
}

void BuildDefaultMidi(std::vector<uint8_t>& smf) {
    constexpr uint32_t kDivision = 480u;               // ticks per quarter
    constexpr uint32_t kBarTicks = 4u * kDivision;
    constexpr uint32_t kBars = 24u;

    std::vector<uint8_t> track;
    // Tempo 120 BPM (500000 µs per quarter note).
    TrackEvent(track, 0u, 0xffu, 0x51u, 0x03u);
    track.push_back(0x07u); track.push_back(0xa1u); track.push_back(0x20u);
    // Programs: ch0 grand piano, ch1 grand piano, ch2 acoustic bass (32),
    // ch3 strings (48).
    TrackEvent(track, 0u, 0xc0u, 0u, 0u);
    TrackEvent(track, 0u, 0xc1u, 0u, 0u);
    TrackEvent(track, 0u, 0xc2u, 32u, 0u);
    TrackEvent(track, 0u, 0xc3u, 48u, 0u);

    struct Bar { uint8_t bass; uint8_t chord[3]; };
    static const Bar bars[4] = {
        {45u, {57u, 60u, 64u}},   // Am: A2 bass, A3 C4 E4
        {41u, {53u, 57u, 60u}},   // F:  F2 bass, F3 A3 C4
        {48u, {60u, 64u, 67u}},   // C:  C3 bass, C4 E4 G4
        {43u, {55u, 59u, 62u}},   // G:  G2 bass, G3 B3 D4
    };
    struct Phrase { uint32_t tick; uint32_t length; uint8_t note; };
    static const Phrase phrases[4][4] = {
        {{0u, 4u, 76u}, {4u, 2u, 79u}, {6u, 2u, 76u}, {8u, 8u, 72u}},
        {{0u, 6u, 74u}, {6u, 2u, 72u}, {8u, 4u, 69u}, {12u, 4u, 72u}},
        {{0u, 4u, 79u}, {4u, 4u, 76u}, {8u, 4u, 74u}, {12u, 4u, 76u}},
        {{0u, 8u, 81u}, {8u, 4u, 79u}, {12u, 4u, 76u}, {16u, 0u, 0u}},
    };

    struct TickEvent { uint32_t tick; uint8_t status, d1, d2; };
    std::vector<TickEvent> timeline;

    for (uint32_t bar = 0u; bar < kBars; ++bar) {
        const Bar& chord = bars[bar % 4u];
        const uint32_t base = bar * kBarTicks;
        const Phrase* phrase = phrases[bar % 4u];

        // Melody (ch0): pedal down at the bar start, up mid-bar so each
        // half re-pedals like a pianist.
        timeline.push_back({base, 0xb0u, 64u, 127u});
        for (uint32_t n = 0u; n < 4u && phrase[n].length != 0u; ++n) {
            const uint32_t on = base + phrase[n].tick * (kDivision / 4u);
            const uint32_t off = on + phrase[n].length * (kDivision / 4u);
            timeline.push_back({on, 0x90u, phrase[n].note, 96u});
            timeline.push_back({off, 0x80u, phrase[n].note, 64u});
        }
        timeline.push_back({base + kBarTicks / 2u, 0xb0u, 64u, 0u});

        // Arpeggio (ch1): eighth notes cycling the chord, one octave up.
        for (uint32_t step = 0u; step < 8u; ++step) {
            const uint8_t note = static_cast<uint8_t>(chord.chord[step % 3u] + 12u);
            const uint32_t tick = base + step * (kDivision / 2u);
            timeline.push_back({tick, 0x91u, note, 64u});
            timeline.push_back({tick + kDivision / 2u, 0x81u, note, 40u});
        }

        // Bass (ch2): root then fifth, half notes.
        timeline.push_back({base, 0x92u, chord.bass, 88u});
        timeline.push_back({base + kDivision * 2u, 0x82u, chord.bass, 40u});
        timeline.push_back({base + kDivision * 2u, 0x92u,
                            static_cast<uint8_t>(chord.bass + 7u), 80u});
        timeline.push_back({base + kBarTicks, 0x82u,
                            static_cast<uint8_t>(chord.bass + 7u), 40u});

        // Pad (ch3): whole-bar chord, low velocity.
        for (uint32_t n = 0u; n < 3u; ++n) {
            timeline.push_back({base, 0x93u, chord.chord[n], 48u});
            timeline.push_back({base + kBarTicks, 0x83u, chord.chord[n], 32u});
        }

        // Expression swell every second bar; volume ride every fourth.
        if ((bar % 2u) == 0u)
            timeline.push_back({base, 0xb1u, 11u,
                                static_cast<uint8_t>(72u + (bar % 8u) * 3u)});
        if ((bar % 4u) == 0u)
            timeline.push_back({base, 0xb0u, 7u,
                                static_cast<uint8_t>(100u + (bar % 8u))});
        // Pitch-bend dip in bar 12 (bend down, return to center).
        if (bar == 12u) {
            timeline.push_back({base + kDivision, 0xe0u, 0x00u, 0x30u});
            timeline.push_back({base + kDivision * 2u, 0xe0u, 0x00u, 0x40u});
        }
    }

    // MIDI deltas require ascending ticks; sort stably so same-tick events
    // keep their program order (note-offs before the next bar's note-ons).
    std::stable_sort(timeline.begin(), timeline.end(),
                     [](const TickEvent& a, const TickEvent& b) {
                         return a.tick < b.tick;
                     });

    const uint32_t endTick = kBars * kBarTicks;
    timeline.push_back({endTick, 0xb0u, 64u, 0u});          // pedal up
    timeline.push_back({endTick, 0xffu, 0x2fu, 0x00u});     // end of track

    uint32_t lastTick = 0u;
    for (const TickEvent& event : timeline) {
        TrackEvent(track, event.tick - lastTick, event.status,
                   event.d1, event.d2);
        lastTick = event.tick;
    }

    const uint32_t trackBytes = static_cast<uint32_t>(track.size());
    smf.clear();
    const uint8_t header[14] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1,
        static_cast<uint8_t>(kDivision >> 8u), static_cast<uint8_t>(kDivision)
    };
    smf.insert(smf.end(), header, header + sizeof(header));
    const uint8_t trackHeader[8] = {
        'M', 'T', 'r', 'k',
        static_cast<uint8_t>(trackBytes >> 24u),
        static_cast<uint8_t>(trackBytes >> 16u),
        static_cast<uint8_t>(trackBytes >> 8u),
        static_cast<uint8_t>(trackBytes)
    };
    smf.insert(smf.end(), trackHeader, trackHeader + sizeof(trackHeader));
    smf.insert(smf.end(), track.begin(), track.end());
}

// ── MIDI loading ────────────────────────────────────────────────────────────
//
// The decoder emits frame-accurate events in deterministic (tick, track)
// order at the engine sample rate, which is exactly the order Dispatch()
// expects.  Events are materialized once into a vector (16 bytes each) so
// both the offline loop and the realtime pacer can iterate them; Black MIDIs
// with tens of millions of events cost proportional RAM, which is acceptable
// for a test tool running on machines that play them live.

struct DecodedSong {
    std::vector<svms::PackedMidiEvent> events;  // sorted by outputFrame
    uint64_t pieceFrames = 0u;                  // one repetition, no tail
    uint64_t noteOns = 0u;
    uint64_t peakNoteOnsPerSecond = 0u;
    uint16_t format = 0u;
    uint16_t tracks = 0u;
};

struct DecodeContext {
    std::vector<svms::PackedMidiEvent>* events = nullptr;
    uint64_t frameOffset = 0u;                  // repetition offset
    uint64_t noteOns = 0u;
};

bool DecodeSink(const svms::PackedMidiEvent& event, void* user) {
    auto* context = static_cast<DecodeContext*>(user);
    context->events->push_back(event);
    context->events->back().outputFrame += context->frameOffset;
    const uint8_t status = static_cast<uint8_t>(event.message);
    if ((status & 0xf0u) == 0x90u &&
        static_cast<uint8_t>(event.message >> 16u) != 0u) {
        ++context->noteOns;
    }
    return true;
}

bool LoadSongFile(const wchar_t* path, uint32_t repetitions,
                  DecodedSong& song, std::string& error) {
    svms::MappedMidiFile file;
    if (!file.Open(path, error)) return false;
    svms::MidiStreamDecoder decoder;
    svms::MidiStreamInfo info;
    std::atomic<bool> cancel{false};
    DecodeContext context;
    context.events = &song.events;
    if (!decoder.Decode(file, 44100u, DecodeSink, &context, &cancel,
                        &info, error)) {
        file.Close();
        return false;
    }
    file.Close();

    song.pieceFrames = info.totalFrames;
    song.noteOns = context.noteOns;
    song.peakNoteOnsPerSecond = info.peakNoteOnsPerSecond;
    song.format = info.format;
    song.tracks = info.tracks;

    // Additional repetitions: re-decode is wasteful; copy with offsets.
    const size_t firstPiece = song.events.size();
    for (uint32_t r = 1u; r < repetitions; ++r) {
        const uint64_t offset = static_cast<uint64_t>(r) * song.pieceFrames;
        DecodeContext repeatContext;
        repeatContext.events = &song.events;
        repeatContext.frameOffset = offset;
        // Re-walk the already-decoded events and shift them.
        const size_t base = song.events.size();
        song.events.resize(base + firstPiece);
        for (size_t i = 0; i < firstPiece; ++i) {
            song.events[base + i] = song.events[i];
            song.events[base + i].outputFrame += offset;
        }
        (void)repeatContext;
    }
    return true;
}

void TrimSongStart(double startSeconds, DecodedSong& song);

bool LoadSong(const Options& options, DecodedSong& song, std::string& error) {
    if (!options.midiPath.empty()) {
        if (!LoadSongFile(options.midiPath.c_str(), options.repeat,
                          song, error)) return false;
        TrimSongStart(options.startSeconds, song);
        return true;
    }
    // Built-in piece: compose to a temp file so the real decoder parses it.
    std::vector<uint8_t> smf;
    BuildDefaultMidi(smf);
    wchar_t tempPath[MAX_PATH] = {};
    wchar_t tempFile[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempPath) ||
        !GetTempFileNameW(tempPath, L"svm", 0, tempFile)) {
        error = "cannot reserve a temp path for the built-in MIDI";
        return false;
    }
    HANDLE handle = CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "cannot write the built-in MIDI to a temp file";
        return false;
    }
    DWORD written = 0u;
    WriteFile(handle, smf.data(), static_cast<DWORD>(smf.size()),
              &written, nullptr);
    CloseHandle(handle);
    const bool loaded = LoadSongFile(tempFile, options.repeat, song, error);
    DeleteFileW(tempFile);
    if (!loaded) return false;
    TrimSongStart(options.startSeconds, song);
    return true;
}

// Drop events before the requested start and shift the window to frame 0.
// Notes already sounding at the cut keep their (now unmatched) note-offs,
// which the engine treats as harmless stale releases — the saturation state
// inside the window is unaffected.
void TrimSongStart(double startSeconds, DecodedSong& song) {
    if (startSeconds <= 0.0) return;
    const uint64_t startFrame =
        static_cast<uint64_t>(startSeconds * 44100.0 + 0.5);
    if (startFrame >= song.pieceFrames) {
        song.events.clear();
        song.pieceFrames = 0u;
        song.noteOns = 0u;
        return;
    }
    size_t out = 0u;
    uint64_t noteOns = 0u;
    for (size_t i = 0; i < song.events.size(); ++i) {
        if (song.events[i].outputFrame < startFrame) continue;
        song.events[out] = song.events[i];
        song.events[out].outputFrame -= startFrame;
        const uint8_t status = static_cast<uint8_t>(song.events[out].message);
        if ((status & 0xf0u) == 0x90u &&
            static_cast<uint8_t>(song.events[out].message >> 16u) != 0u)
            ++noteOns;
        ++out;
    }
    song.events.resize(out);
    song.pieceFrames -= startFrame;
    song.noteOns = noteOns;
}

// ── WAV writer (streaming 16-bit PCM stereo) ────────────────────────────────

class WaveWriter {
public:
    bool Open(const wchar_t* path, uint32_t sampleRate) {
        file_ = _wfopen(path, L"wb");
        if (file_ == nullptr) return false;
        dataBytes_ = 0u;
        sampleRate_ = sampleRate;
        uint8_t header[44] = {};
        std::memcpy(header, "RIFF", 4u);
        StoreU32(header + 4u, 0u);              // patched at Close
        std::memcpy(header + 8u, "WAVE", 4u);
        std::memcpy(header + 12u, "fmt ", 4u);
        StoreU32(header + 16u, 16u);
        header[20u] = 1u; header[21u] = 0u;     // PCM
        header[22u] = 2u; header[23u] = 0u;     // stereo
        StoreU32(header + 24u, sampleRate);
        StoreU32(header + 28u, sampleRate * 4u);// byte rate
        header[32u] = 4u; header[33u] = 0u;     // block align
        header[34u] = 16u; header[35u] = 0u;    // bits
        std::memcpy(header + 36u, "data", 4u);
        StoreU32(header + 40u, 0u);             // patched at Close
        return std::fwrite(header, 1u, sizeof(header), file_) == sizeof(header);
    }

    bool Write(const float* left, const float* right, uint32_t frames) {
        for (uint32_t i = 0u; i < frames; ++i) {
            for (const float* channel : {left, right}) {
                float v = channel[i];
                if (!(v > -1.0f && v < 1.0f))
                    v = v >= 0.0f ? 1.0f : -1.0f;
                const int32_t s = static_cast<int32_t>(v * 32767.0f);
                const uint8_t bytes[2] = {
                    static_cast<uint8_t>(s & 0xffu),
                    static_cast<uint8_t>((s >> 8u) & 0xffu)};
                if (std::fwrite(bytes, 1u, 2u, file_) != 2u) return false;
                dataBytes_ += 2u;
            }
        }
        return true;
    }

    bool Close() {
        if (file_ == nullptr) return false;
        bool ok = true;
        if (_fseeki64(file_, 4, SEEK_SET) == 0)
            ok &= StoreU32At(36u + dataBytes_);
        if (_fseeki64(file_, 40, SEEK_SET) == 0)
            ok &= StoreU32At(dataBytes_);
        ok &= std::fflush(file_) == 0;
        ok &= std::fclose(file_) == 0;
        file_ = nullptr;
        return ok;
    }

    ~WaveWriter() { if (file_ != nullptr) std::fclose(file_); }

private:
    static void StoreU32(uint8_t* out, uint32_t value) {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8u);
        out[2] = static_cast<uint8_t>(value >> 16u);
        out[3] = static_cast<uint8_t>(value >> 24u);
    }
    bool StoreU32At(uint32_t value) {
        uint8_t bytes[4];
        StoreU32(bytes, value);
        return std::fwrite(bytes, 1u, 4u, file_) == 4u;
    }

    std::FILE* file_ = nullptr;
    uint32_t sampleRate_ = 44100u;
    uint64_t dataBytes_ = 0u;
};

// ── Offline runner ──────────────────────────────────────────────────────────

struct OfflineResult {
    bool ok = false;
    uint64_t dispatched = 0u;
    uint64_t peakActive = 0u;
    uint64_t peakTails = 0u;
    uint32_t stealTotal = 0u;
    double cpvsP50 = 0.0, cpvsP95 = 0.0, cpvsP99 = 0.0;
    double dispatchPerEventP50 = 0.0, dispatchPerEventP95 = 0.0;
    double renderCpvsP50 = 0.0, renderCpvsP95 = 0.0;
    double budgetP50 = 0.0, budgetP95 = 0.0, budgetP99 = 0.0, budgetMax = 0.0;
    float audioPeak = 0.0f;
    uint64_t missingPresets = 0u, missingRegions = 0u;
    uint64_t invalidRegions = 0u, fallbackRegions = 0u;
    std::string backend;
    std::string error;
};

svms::RenderBackend ParseBackend(const std::string& name) {
    // AVX512 is the StandaloneSynth "automatic" sentinel.
    if (name == "scalar") return svms::RenderBackend::Scalar;
    if (name == "sse2") return svms::RenderBackend::SSE2;
    if (name == "avx2") return svms::RenderBackend::AVX2;
    return svms::RenderBackend::AVX512;
}

bool RunOffline(const Options& options, const DecodedSong& song,
                const std::wstring& soundfont, OfflineResult& out) {
    svms::StandaloneSynthConfig config{};
    config.soundfont = soundfont;
    config.sampleRate = 44100u;
    config.maxVoices = options.maxVoices;
    config.renderThreads = options.renderThreads;
    config.maxBlockFrames = options.frames;
    config.masterVolume = 1.0f;
    config.backend = ParseBackend(options.backend);

    svms::StandaloneSynth synth;
    std::string error;
    if (!synth.Initialize(config, error)) {
        out.error = error;
        return false;
    }
    out.backend = synth.Backend();

    WaveWriter wave;
    const bool wantWav = !options.wavPath.empty();
    if (wantWav && !wave.Open(options.wavPath.c_str(), 44100u)) {
        out.error = "cannot open the output WAV file";
        return false;
    }

    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    std::vector<float> left(options.frames), right(options.frames);

    const uint64_t tailFrames =
        static_cast<uint64_t>(options.tailSeconds) * 44100u;
    const uint64_t totalFrames = song.pieceFrames + tailFrames;
    const uint64_t eventCount = song.events.size();
    uint64_t frame = 0u, eventIndex = 0u, dispatched = 0u;
    uint64_t nextVerboseFrame = 44100u * 2u;
    std::vector<double> cpvsSamples, budgetSamples;
    std::vector<double> dispatchPerEvent, renderOnlyCpvs;
    uint64_t peakActive = 0u, peakTails = 0u;
    float audioPeak = 0.0f;

    while (frame < totalFrames) {
        const uint32_t n = static_cast<uint32_t>(
            (std::min<uint64_t>)(options.frames, totalFrames - frame));
        LARGE_INTEGER qpcBegin{}, qpcEnd{};
        QueryPerformanceCounter(&qpcBegin);
        const uint64_t renderBegin = __rdtsc();
        uint64_t blockEvents = 0u;
        while (eventIndex < eventCount &&
               song.events[eventIndex].outputFrame < frame + n) {
            synth.Dispatch(song.events[eventIndex].message,
                           song.events[eventIndex].outputFrame);
            ++eventIndex;
            ++dispatched;
            ++blockEvents;
        }
        const uint64_t dispatchEnd = __rdtsc();
        synth.Render(left.data(), right.data(), n, frame);
        const uint64_t renderEnd = __rdtsc();
        QueryPerformanceCounter(&qpcEnd);
        if (blockEvents != 0u)
            dispatchPerEvent.push_back(
                static_cast<double>(dispatchEnd - renderBegin) /
                static_cast<double>(blockEvents));

        const uint32_t active = synth.Active();
        peakActive = (std::max<uint64_t>)(peakActive, active);
        peakTails = (std::max<uint64_t>)(peakTails, synth.Tails());
        const double budgetSeconds = static_cast<double>(n) / 44100.0;
        const double wallSeconds =
            static_cast<double>(qpcEnd.QuadPart - qpcBegin.QuadPart) /
            static_cast<double>(qpf.QuadPart);
        budgetSamples.push_back(wallSeconds / budgetSeconds * 100.0);
        if (active > 0u) {
            cpvsSamples.push_back(
                static_cast<double>(renderEnd - renderBegin) /
                (static_cast<double>(n) * static_cast<double>(active)));
            renderOnlyCpvs.push_back(
                static_cast<double>(renderEnd - dispatchEnd) /
                (static_cast<double>(n) * static_cast<double>(active)));
        }
        for (uint32_t i = 0u; i < n; i += 16u) {
            const float magnitude = (std::max)(std::fabs(left[i]),
                                               std::fabs(right[i]));
            if (!std::isfinite(magnitude)) {
                out.error = "non-finite audio sample produced";
                return false;
            }
            audioPeak = (std::max)(audioPeak, magnitude);
        }
        if (wantWav && !wave.Write(left.data(), right.data(), n)) {
            out.error = "WAV write failed";
            return false;
        }
        if (options.verbose && frame + n >= nextVerboseFrame &&
            nextVerboseFrame <= song.pieceFrames) {
            std::printf("[song] t=%5.1fs active=%4u tails=%3u cpvs=%6.2f "
                        "budget=%5.1f%%\n",
                        static_cast<double>(frame) / 44100.0, active,
                        synth.Tails(), cpvsSamples.back(),
                        budgetSamples.back());
            std::fflush(stdout);
            nextVerboseFrame += 44100u * 2u;
        }
        frame += n;
    }

    if (options.coverage) {
        std::printf("[coverage] wholeVoiceBlocks=%llu\n",
                    (unsigned long long)synth.WholeVoiceBlocksForTest());
        std::fflush(stdout);
    }

#if defined(_MSC_VER)
    {
        const auto& p = synth.dispatchProfile;
        const double onCalls = p.noteOnCalls ? (double)p.noteOnCalls : 1.0;
        const double offCalls = p.noteOffCalls ? (double)p.noteOffCalls : 1.0;
        const double ctlCalls = p.controlCalls ? (double)p.controlCalls : 1.0;
        const double bendCalls =
            p.bendCalls ? (double)p.bendCalls : 1.0;
        std::printf(
            "[dispatch-phase] on=%llu (%.0f cy: resolve=%.0f alloc=%.0f "
            "configure=%.0f) off=%llu (%.0f cy) ctl=%llu (%.0f cy: "
            "rebuild=%.0f mixgains=%.0f) bend=%llu (%.0f cy) "
            "heapBuilds=%llu\n",
            (unsigned long long)p.noteOnCalls,
            (double)p.noteOnTotal / onCalls,
            (double)p.resolve / onCalls,
            (double)p.alloc / onCalls,
            (double)p.configure / onCalls,
            (unsigned long long)p.noteOffCalls,
            (double)p.noteOffTotal / offCalls,
            (unsigned long long)p.controlCalls,
            (double)p.controlTotal / ctlCalls,
            (double)p.controlRebuild / ctlCalls,
            (double)p.controlMix / ctlCalls,
            (unsigned long long)p.bendCalls,
            (double)p.bendTotal / bendCalls,
            (unsigned long long)synth.StealHeapBuildCount());
        std::fflush(stdout);
    }
#endif

    if (eventIndex != eventCount) {
        out.error = "not all MIDI events were dispatched";
        return false;
    }
    if (wantWav && !wave.Close()) {
        out.error = "WAV finalize failed";
        return false;
    }

    out.ok = true;
    out.dispatched = dispatched;
    out.peakActive = peakActive;
    out.peakTails = peakTails;
    out.stealTotal = synth.Steals();
    out.cpvsP50 = Percentile(cpvsSamples, 0.50);
    out.cpvsP95 = Percentile(cpvsSamples, 0.95);
    out.cpvsP99 = Percentile(cpvsSamples, 0.99);
    out.dispatchPerEventP50 = Percentile(dispatchPerEvent, 0.50);
    out.dispatchPerEventP95 = Percentile(dispatchPerEvent, 0.95);
    out.renderCpvsP50 = Percentile(renderOnlyCpvs, 0.50);
    out.renderCpvsP95 = Percentile(renderOnlyCpvs, 0.95);
    out.budgetP50 = Percentile(budgetSamples, 0.50);
    out.budgetP95 = Percentile(budgetSamples, 0.95);
    out.budgetP99 = Percentile(budgetSamples, 0.99);
    out.budgetMax = budgetSamples.empty()
        ? 0.0 : *std::max_element(budgetSamples.begin(), budgetSamples.end());
    out.audioPeak = audioPeak;
    out.missingPresets = synth.MissingPresets();
    out.missingRegions = synth.MissingRegions();
    out.invalidRegions = synth.InvalidRegions();
    out.fallbackRegions = synth.FallbackRegions();
    return true;
}

// ── Realtime runner (live winmm.dll) ────────────────────────────────────────
// Requires the RuntimeLink V2 live-config client, which the XP build
// excludes; XP runs the offline gate only.

#if !defined(SVMS_XP_COMPAT)

struct RealtimeResult {
    bool ok = false;
    bool skipped = false;
    bool truncated = false;
    uint64_t sent = 0u;
    uint64_t samples = 0u;
    double cpuP50 = 0.0, cpuP95 = 0.0, cpuMax = 0.0;
    double callbackP95Max = 0.0, callbackP99Max = 0.0;
    uint32_t peakActive = 0u, peakReleasing = 0u;
    uint64_t stealsDelta = 0u, droppedDelta = 0u;
    double limiterGrMaxDb = 0.0;
    std::string error;
};

typedef UINT (WINAPI* MidiOutGetNumDevsProc)();
typedef MMRESULT (WINAPI* MidiOutOpenProc)(LPHMIDIOUT, UINT, DWORD_PTR,
                                           DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI* MidiOutShortMsgProc)(HMIDIOUT, DWORD);
typedef MMRESULT (WINAPI* MidiOutResetProc)(HMIDIOUT);
typedef MMRESULT (WINAPI* MidiOutCloseProc)(HMIDIOUT);

// ── Hang watchdog ───────────────────────────────────────────────────────────
// The realtime pacer advances a heartbeat while it runs.  If the heartbeat
// stalls (pacer blocked on a full SPSC ring, wedged dispatch, stuck telemetry
// read...), dump every thread's stack with symbols and exit — a hung gate is
// undiagnosable without this.
std::atomic<uint64_t> g_rtHeartbeat{0};
std::atomic<bool> g_rtFinished{false};

void DumpThreadStacks() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,
                                               GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot, &entry)) {
        CloseHandle(snapshot);
        return;
    }
    do {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            CloseHandle(thread);
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            std::printf("  --- thread %lu ---\n",
                        static_cast<unsigned long>(entry.th32ThreadID));
            STACKFRAME64 frame{};
#if defined(_M_X64)
            frame.AddrPC.Offset = context.Rip;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrStack.Offset = context.Rsp;
            const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
            frame.AddrPC.Offset = context.Eip;
            frame.AddrFrame.Offset = context.Ebp;
            frame.AddrStack.Offset = context.Esp;
            const DWORD machine = IMAGE_FILE_MACHINE_I386;
#endif
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Mode = AddrModeFlat;
            for (int depth = 0; depth < 64; ++depth) {
                if (!StackWalk64(machine, GetCurrentProcess(), thread,
                                 &frame, &context, nullptr,
                                 SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr)) break;
                const DWORD64 pc = frame.AddrPC.Offset;
                if (pc == 0) break;
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
                SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                if (SymFromAddr(GetCurrentProcess(), pc, &displacement,
                                symbol)) {
                    std::printf("    %s+0x%llx\n", symbol->Name,
                                static_cast<unsigned long long>(displacement));
                } else {
                    const DWORD64 base =
                        SymGetModuleBase64(GetCurrentProcess(), pc);
                    std::printf("    0x%llx (base 0x%llx)\n",
                                static_cast<unsigned long long>(pc),
                                static_cast<unsigned long long>(base));
                }
            }
            std::fflush(stdout);
        }
        ResumeThread(thread);
        CloseHandle(thread);
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    std::fflush(stdout);
}

DWORD WINAPI HangWatchdog(void*) {
    uint64_t last = 0;
    int stalledSeconds = 0;
    while (!g_rtFinished.load(std::memory_order_acquire)) {
        Sleep(1000);
        const uint64_t now = g_rtHeartbeat.load(std::memory_order_relaxed);
        if (now != last) {
            last = now;
            stalledSeconds = 0;
            continue;
        }
        if (++stalledSeconds >= 5) {
            std::printf("\n[HANG-WATCHDOG] realtime pacer stalled for %d s; "
                        "dumping all thread stacks\n", stalledSeconds);
            std::fflush(stdout);
            DumpThreadStacks();
            ExitProcess(0xDEADBEEFu);
        }
    }
    return 0;
}

// Wait until the QPC target, sampling telemetry on the way. Returns false
// when telemetry sampling failed hard (link lost).
void WaitWithTelemetry(HANDLE timer, LONGLONG targetQpc, LONGLONG qpf,
                       svms::RuntimeLinkClientV2* client,
                       svms::RuntimeLinkTelemetryV2& telemetry,
                       std::vector<double>* cpuSamples,
                       std::vector<double>* cb95Samples,
                       std::vector<double>* cb99Samples,
                       uint32_t* peakActive, uint32_t* peakReleasing,
                       uint64_t* steals, uint64_t* dropped,
                       double* limiterGrMax, uint64_t* sampleCount,
                       DWORD* nextSampleTick, DWORD* nextVerboseTick,
                       bool verbose) {
    for (;;) {
        ++g_rtHeartbeat;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= targetQpc) return;

        const DWORD tick = GetTickCount();
        if (client != nullptr && tick >= *nextSampleTick) {
            *nextSampleTick = tick + 100u;
            svms::RuntimeLinkTelemetryV2 sampled{};
            if (client->ReadTelemetry(sampled) &&
                sampled.timestampQpc != 0u) {
                ++(*sampleCount);
                const double cpu = sampled.cpuLoadPercent;
                cpuSamples->push_back(cpu);
                cb95Samples->push_back(sampled.callbackP95Percent);
                cb99Samples->push_back(sampled.callbackP99Percent);
                *peakActive = (std::max<uint32_t>)(*peakActive,
                                                    sampled.activeVoices);
                *peakReleasing = (std::max<uint32_t>)(*peakReleasing,
                                                      sampled.releasingVoices);
                *steals = (std::max)(*steals,
                                     static_cast<uint64_t>(sampled.voiceSteals));
                *dropped = (std::max)(*dropped,
                                      static_cast<uint64_t>(sampled.eventsDropped));
                *limiterGrMax = (std::max)(*limiterGrMax,
                    static_cast<double>(sampled.limiterGainReductionDb));
                telemetry = sampled;
                if (verbose && tick >= *nextVerboseTick) {
                    std::printf("[live] t=%6.2fs active=%4u rel=%3u "
                                "cpu=%5.1f%% p95=%5.1f%% p99=%5.1f%% "
                                "steals=%u drops=%llu\n",
                                static_cast<double>(now.QuadPart) /
                                    static_cast<double>(qpf),
                                sampled.activeVoices,
                                sampled.releasingVoices, cpu,
                                sampled.callbackP95Percent,
                                sampled.callbackP99Percent,
                                sampled.voiceSteals,
                                static_cast<unsigned long long>(
                                    sampled.eventsDropped));
                    std::fflush(stdout);
                    *nextVerboseTick = tick + 1000u;
                }
            }
        }

        const LONGLONG due100ns =
            static_cast<LONGLONG>(static_cast<double>(targetQpc - now.QuadPart) *
                                  10000000.0 / static_cast<double>(qpf));
        LARGE_INTEGER due{};
        due.QuadPart = -(due100ns > 1 ? due100ns : 1);
        SetWaitableTimer(timer, &due, 0u, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }
}

bool RunRealtime(const Options& options, const DecodedSong& song,
                 RealtimeResult& out) {
    std::wstring dllPath = options.dllPath;
    if (dllPath.empty()) {
        for (const wchar_t* candidate : {L"build\\V3\\bin\\winmm.dll",
                                         L"bin\\winmm.dll"}) {
            if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
                dllPath = candidate;
                break;
            }
        }
    }
    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (module == nullptr) {
        out.error = "cannot load the V3 winmm.dll (pass --dll PATH)";
        return false;
    }
    auto getNumDevs = reinterpret_cast<MidiOutGetNumDevsProc>(
        GetProcAddress(module, "midiOutGetNumDevs"));
    auto open = reinterpret_cast<MidiOutOpenProc>(
        GetProcAddress(module, "midiOutOpen"));
    auto shortMsg = reinterpret_cast<MidiOutShortMsgProc>(
        GetProcAddress(module, "midiOutShortMsg"));
    auto reset = reinterpret_cast<MidiOutResetProc>(
        GetProcAddress(module, "midiOutReset"));
    auto close = reinterpret_cast<MidiOutCloseProc>(
        GetProcAddress(module, "midiOutClose"));
    if (!getNumDevs || !open || !shortMsg || !reset || !close) {
        out.error = "required MIDI exports are missing";
        FreeLibrary(module);
        return false;
    }
    if (getNumDevs() != 1u) {
        out.skipped = true;
        out.error = "V3 DLL did not advertise one MIDI output";
        FreeLibrary(module);
        return false;
    }
    HMIDIOUT handle = nullptr;
    if (open(&handle, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        out.skipped = true;
        out.error = "MIDI_MAPPER open failed";
        FreeLibrary(module);
        return false;
    }

    // Live link: mute unless --audible, restore the original master volume
    // at exit.  The mute goes through the production ApplyLiveConfig path.
    svms::RuntimeLinkClientV2 client;
    svms::RuntimeLiveStateV2 originalLive{};
    bool linked = false;
    if (client.Open(GetCurrentProcessId())) {
        svms::RuntimeLinkTelemetryV2 t{};
        const DWORD start = GetTickCount();
        do {
            if (client.ReadTelemetry(t) && t.timestampQpc != 0u &&
                t.audioRunning != 0u) {
                originalLive = t.live;
                linked = true;
                break;
            }
            Sleep(50);
        } while (static_cast<int>(GetTickCount() - start) < 10000);
    }
    if (!linked) {
        out.skipped = true;
        out.error = "driver never published RuntimeLink telemetry";
        reset(handle);
        close(handle);
        FreeLibrary(module);
        return false;
    }
    if (!options.audible) {
        svms::RuntimeLiveStateV2 muted = originalLive;
        muted.masterVolume = 0.0f;
        char text[svms::kRuntimeLinkResultTextCapacity] = {};
        const svms::RLResult result = client.SendCommand(
            svms::RLCommandType::ApplyLiveConfig, svms::RLGroupMaster, 0u,
            muted, 4000, text);
        const DWORD start = GetTickCount();
        bool echoed = false;
        do {
            svms::RuntimeLinkTelemetryV2 t{};
            if (client.ReadTelemetry(t) && t.live.masterVolume < 0.001f) {
                echoed = true;
                break;
            }
            Sleep(25);
        } while (static_cast<int>(GetTickCount() - start) < 4000);
        if (result != svms::RLResult::Ok || !echoed) {
            out.skipped = true;
            out.error = "cannot mute the driver (masterVolume=0 not echoed); "
                        "refusing to play aloud";
            reset(handle);
            close(handle);
            FreeLibrary(module);
            return false;
        }
    }

    const uint64_t tailFrames =
        static_cast<uint64_t>(options.tailSeconds) * 44100u;
    const uint64_t totalFrames = song.pieceFrames + tailFrames;
    LARGE_INTEGER qpf{}, startQpc{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&startQpc);
    const LONGLONG capQpc = options.realtimeSecondsCap != 0u
        ? startQpc.QuadPart +
              static_cast<LONGLONG>(options.realtimeSecondsCap) * qpf.QuadPart
        : 0;

    HANDLE timer = CreateWaitableTimer(nullptr, FALSE, nullptr);
    timeBeginPeriod(1u);
    HANDLE watchdog = CreateThread(nullptr, 0, HangWatchdog, nullptr, 0, nullptr);
    g_rtHeartbeat.store(1, std::memory_order_relaxed);

    std::vector<double> cpuSamples, cb95Samples, cb99Samples;
    uint32_t peakActive = 0u, peakReleasing = 0u;
    uint64_t steals = 0u, dropped = 0u, sampleCount = 0u;
    double limiterGrMax = 0.0;
    DWORD nextSampleTick = 0u, nextVerboseTick = 1000u;
    svms::RuntimeLinkTelemetryV2 last{};
    uint64_t sent = 0u;
    bool truncated = false;

    for (const svms::PackedMidiEvent& event : song.events) {
        const LONGLONG target = startQpc.QuadPart +
            static_cast<LONGLONG>(static_cast<double>(event.outputFrame) *
                                  static_cast<double>(qpf.QuadPart) / 44100.0);
        WaitWithTelemetry(timer, target, qpf.QuadPart, &client, last,
                          &cpuSamples, &cb95Samples, &cb99Samples,
                          &peakActive, &peakReleasing, &steals, &dropped,
                          &limiterGrMax, &sampleCount, &nextSampleTick,
                          &nextVerboseTick, options.verbose);
        if (capQpc != 0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (now.QuadPart > capQpc) {
                truncated = true;
                break;
            }
        }
        shortMsg(handle, event.message);
        ++sent;
    }
    if (!truncated) {
        const LONGLONG endTarget = startQpc.QuadPart +
            static_cast<LONGLONG>(static_cast<double>(totalFrames) *
                                  static_cast<double>(qpf.QuadPart) / 44100.0);
        WaitWithTelemetry(timer, endTarget, qpf.QuadPart, &client, last,
                          &cpuSamples, &cb95Samples, &cb99Samples,
                          &peakActive, &peakReleasing, &steals, &dropped,
                          &limiterGrMax, &sampleCount, &nextSampleTick,
                          &nextVerboseTick, options.verbose);
    }
    g_rtFinished.store(true, std::memory_order_release);
    if (watchdog != nullptr) CloseHandle(watchdog);

    timeEndPeriod(1u);
    if (timer != nullptr) CloseHandle(timer);
    reset(handle);
    if (!options.audible) {
        char text[svms::kRuntimeLinkResultTextCapacity] = {};
        client.SendCommand(svms::RLCommandType::ApplyLiveConfig,
                           svms::RLGroupMaster, 0u, originalLive, 4000, text);
    }
    close(handle);
    FreeLibrary(module);

    out.ok = true;
    out.sent = sent;
    out.truncated = truncated;
    out.samples = sampleCount;
    out.cpuP50 = Percentile(cpuSamples, 0.50);
    out.cpuP95 = Percentile(cpuSamples, 0.95);
    out.cpuMax = cpuSamples.empty()
        ? 0.0 : *std::max_element(cpuSamples.begin(), cpuSamples.end());
    out.callbackP95Max = cb95Samples.empty()
        ? 0.0 : *std::max_element(cb95Samples.begin(), cb95Samples.end());
    out.callbackP99Max = cb99Samples.empty()
        ? 0.0 : *std::max_element(cb99Samples.begin(), cb99Samples.end());
    out.peakActive = peakActive;
    out.peakReleasing = peakReleasing;
    out.stealsDelta = steals;
    out.droppedDelta = dropped;
    out.limiterGrMaxDb = limiterGrMax;
    return true;
}

#endif // !SVMS_XP_COMPAT

} // namespace

#if defined(SVMS_MIDISONG_CRASHTRACE)
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
namespace {
LONG WINAPI MidiSongCrashHandler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != 0xC0000005u)
        return EXCEPTION_CONTINUE_SEARCH;
    std::printf("\n[CRASH] op=%llu access=%llX\n",
                info->ExceptionRecord->ExceptionInformation[0] == 0u ? 0ull
                : (info->ExceptionRecord->ExceptionInformation[0] == 1u ? 1ull
                                                                        : 2ull),
                static_cast<unsigned long long>(
                    info->ExceptionRecord->ExceptionInformation[1]));
    void* frames[48];
    const USHORT count = CaptureStackBackTrace(1, 48, frames, nullptr);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    for (USHORT i = 0; i < count; ++i) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const BOOL got = SymFromAddr(GetCurrentProcess(),
                                     reinterpret_cast<DWORD64>(frames[i]),
                                     &displacement, symbol);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const BOOL hasLine = SymGetLineFromAddr64(
            GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[i]),
            &lineDisp, &line);
        std::printf("[CRASH] %02u %s+0x%llX %s:%lu\n",
                    static_cast<unsigned>(i),
                    got ? symbol->Name : "?",
                    static_cast<unsigned long long>(displacement),
                    hasLine ? line.FileName : "?",
                    hasLine ? static_cast<unsigned long>(line.LineNumber) : 0u);
    }
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace
#endif

int main(int argc, char** argv) {
    Options options;
    if (!ParseArgs(argc, argv, options)) return 2;
#if defined(SVMS_MIDISONG_CRASHTRACE)
    AddVectoredExceptionHandler(1, MidiSongCrashHandler);
#endif

    DecodedSong song;
    std::string error;
    if (!LoadSong(options, song, error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 1;
    }
    if (song.events.empty() || song.pieceFrames == 0u) {
        std::fprintf(stderr, "FAIL: the MIDI file contains no playable data\n");
        return 1;
    }
    if (!options.realtime && options.realtimeSecondsCap != 0u) {
        // Offline --seconds: hard window — drop events beyond the cap so the
        // dispatch-complete assertion stays meaningful.
        const uint64_t capFrames =
            static_cast<uint64_t>(options.realtimeSecondsCap) * 44100u;
        if (capFrames < song.pieceFrames) {
            size_t out = 0u;
            for (size_t i = 0; i < song.events.size(); ++i) {
                if (song.events[i].outputFrame >= capFrames) continue;
                song.events[out++] = song.events[i];
            }
            song.events.resize(out);
            song.pieceFrames = capFrames;
        }
    }
    if (!options.quiet) {
        std::fprintf(stderr,
                     "SMF %u, %u tracks, %zu events (%llu note-ons, peak "
                     "%llu/s), %.2f min%s\n",
                     song.format, song.tracks, song.events.size(),
                     static_cast<unsigned long long>(song.noteOns),
                     static_cast<unsigned long long>(song.peakNoteOnsPerSecond),
                     static_cast<double>(song.pieceFrames) / 44100.0 / 60.0,
                     options.repeat > 1u ? " (repeated)" : "");
    }

    if (options.realtime) {
#if !defined(SVMS_XP_COMPAT)
        RealtimeResult result;
        if (!RunRealtime(options, song, result)) {
            std::fprintf(stderr, "FAIL: %s\n", result.error.c_str());
            return 1;
        }
        if (result.skipped) {
            std::fprintf(stderr, "SKIP: %s\n", result.error.c_str());
            return 77;
        }
        if (!options.quiet) {
            std::printf("realtime playback: %llu events sent%s\n"
                        "cpu load p50=%.1f%% p95=%.1f%% max=%.1f%% | "
                        "callback p95 max=%.1f%% p99 max=%.1f%%\n"
                        "voices peak active=%u releasing=%u | steals=%llu | "
                        "dropped=%llu | limiter GR max=%.1f dB | "
                        "telemetry samples=%llu\n",
                        static_cast<unsigned long long>(result.sent),
                        result.truncated ? " (TRUNCATED by --seconds)" : "",
                        result.cpuP50, result.cpuP95, result.cpuMax,
                        result.callbackP95Max, result.callbackP99Max,
                        result.peakActive, result.peakReleasing,
                        static_cast<unsigned long long>(result.stealsDelta),
                        static_cast<unsigned long long>(result.droppedDelta),
                        result.limiterGrMaxDb,
                        static_cast<unsigned long long>(result.samples));
        }
        std::printf("{\"mode\":\"realtime\",\"sent\":%llu,\"dropped\":%llu,"
                    "\"cpuP50\":%.1f,\"cpuP95\":%.1f,\"cpuMax\":%.1f,"
                    "\"cbP95Max\":%.1f,\"cbP99Max\":%.1f,\"peakActive\":%u,"
                    "\"peakReleasing\":%u,\"steals\":%llu,\"truncated\":%s}\n",
                    static_cast<unsigned long long>(result.sent),
                    static_cast<unsigned long long>(result.droppedDelta),
                    result.cpuP50, result.cpuP95, result.cpuMax,
                    result.callbackP95Max, result.callbackP99Max,
                    result.peakActive, result.peakReleasing,
                    static_cast<unsigned long long>(result.stealsDelta),
                    result.truncated ? "true" : "false");
        return 0;
#else
        std::fprintf(stderr, "SKIP: realtime playback is unavailable in the "
                             "XP build (no RuntimeLink client)\n");
        return 77;
#endif
    }

    // Offline: soundfont from --soundfont, else the user's V3 configuration.
    std::wstring soundfont = options.soundfontPath;
    if (soundfont.empty()) {
        const svms::EngineConfig config = svms::EngineConfig::Load();
        soundfont = svms::ResolveV3SoundFontPath(config, nullptr);
    }
    if (soundfont.empty() ||
        GetFileAttributesW(soundfont.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "SKIP: no soundfont available (pass --soundfont "
                             "PATH or configure one for the V3 driver)\n");
        return 77;
    }

    OfflineResult result;
    if (!RunOffline(options, song, soundfont, result)) {
        std::fprintf(stderr, "FAIL: %s\n", result.error.c_str());
        return 1;
    }
    if (!options.quiet) {
        std::printf("offline render: %llu events, peak %llu active voices, "
                    "%llu tails, %llu steals (%s)\n"
                    "cpvs p50=%.2f p95=%.2f p99=%.2f | "
                    "callback-budget p50=%.1f%% p95=%.1f%% p99=%.1f%% "
                    "max=%.1f%%\n"
                    "dispatch/event p50=%.0f p95=%.0f cycles | "
                    "render cpvs p50=%.2f p95=%.2f\n"
                    "audio peak=%.3f | missing presets=%llu regions=%llu "
                    "invalid=%llu fallback=%llu\n",
                    static_cast<unsigned long long>(result.dispatched),
                    static_cast<unsigned long long>(result.peakActive),
                    static_cast<unsigned long long>(result.peakTails),
                    static_cast<unsigned long long>(result.stealTotal),
                    result.backend.c_str(),
                    result.cpvsP50, result.cpvsP95, result.cpvsP99,
                    result.budgetP50, result.budgetP95, result.budgetP99,
                    result.budgetMax,
                    result.dispatchPerEventP50, result.dispatchPerEventP95,
                    result.renderCpvsP50, result.renderCpvsP95,
                    result.audioPeak,
                    static_cast<unsigned long long>(result.missingPresets),
                    static_cast<unsigned long long>(result.missingRegions),
                    static_cast<unsigned long long>(result.invalidRegions),
                    static_cast<unsigned long long>(result.fallbackRegions));
    }
    std::printf("{\"mode\":\"offline\",\"backend\":\"%s\",\"events\":%llu,"
                "\"noteOns\":%llu,\"peakActive\":%llu,\"peakTails\":%llu,"
                "\"steals\":%u,\"cpvsP50\":%.2f,\"cpvsP95\":%.2f,"
                "\"cpvsP99\":%.2f,\"budgetP50\":%.1f,\"budgetP95\":%.1f,"
                "\"budgetP99\":%.1f,\"budgetMax\":%.1f,\"audioPeak\":%.4f,"
                "\"missingPresets\":%llu,\"missingRegions\":%llu}\n",
                result.backend.c_str(),
                static_cast<unsigned long long>(result.dispatched),
                static_cast<unsigned long long>(song.noteOns),
                static_cast<unsigned long long>(result.peakActive),
                static_cast<unsigned long long>(result.peakTails),
                result.stealTotal,
                result.cpvsP50, result.cpvsP95, result.cpvsP99,
                result.budgetP50, result.budgetP95, result.budgetP99,
                result.budgetMax, result.audioPeak,
                static_cast<unsigned long long>(result.missingPresets),
                static_cast<unsigned long long>(result.missingRegions));

    if (result.audioPeak <= 0.0001f) {
        std::fprintf(stderr, "FAIL: the render produced silence\n");
        return 1;
    }
    return 0;
}
