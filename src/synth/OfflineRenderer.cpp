#if defined(_WIN32)
#include "synth/Tuning.h"
#include <windows.h>
#else
#include <codecvt>
#include <sys/stat.h>
#include <locale>
#endif
#include "events/MidiStream.h"
#include "synth/SoundFont.h"
#include "synth/VoiceManager.h"
#include "synth/ChannelCache.h"
#include "render/RenderScalar.h"
#include "render/PostFilter.h"
#include "render/Limiter.h"
#include "render/Envelope.h"
#include "synth/Config.h"
#include "synth/StandaloneSynth.h"
#include "render/RealtimeBlock.h"
#include "canonical/CanonicalEngine.h"
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
#include "render/GpuSynth.h"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <ctime>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <queue>
#include <functional>
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace svms {
namespace {

struct Options {
    std::wstring midi, soundfont, output;
    uint32_t sampleRate = 44100;
    uint32_t blockFrames = 8192;
    uint32_t maxVoices = 4096;
    uint32_t renderThreads = 1;
    uint32_t eventBufferMB = 128;
    uint32_t maxTailSeconds = 30;
    float masterVolume = 1.0f;
    bool limiterEnabled = true;
    LimiterAlgorithm limiterAlgorithm = LimiterAlgorithm::Classic;
    float limiterThreshold = 0.95f;
    float limiterLookaheadMs = 3.0f;
    float limiterAttackMs = 0.5f;
    float limiterReleaseMs = 100.0f;
    uint32_t phaseRotationMode = 0u;
    uint32_t tuningEdo = 12u;
    RenderBackend backend = RenderBackend::AVX512; // sentinel: automatic
    bool quiet = false;
    bool scanOnly = false;
    bool machineProgress = false;
    bool fullVelocity = false;         // --full-velocity: every note-on at velocity 127
    int32_t minNoteMs = -1;            // --min-note-ms: minimum note length (-1 = engine config)
    bool bassMode = false;             // --bassmidi-mode: KMC-style BASSMIDI behaviour (note-table renderer)
    bool streamNoteTable = false;      // --bassmidi-stream: the windowed note-table renderer
    bool noteTable = false;            // --note-table CAP: experimental note-table renderer
    uint32_t noteTableCap = 0u;        //   voice limit for it (0 = unlimited)
    uint32_t oversample = 1u;          // --oversample: sample-store oversampling (1 = off)
    uint32_t shards = 1u;              // --shards: key-sharded parallel planning
    uint32_t maxSeconds = 0u;          // --max-seconds: stop the MIDI after N s (0 = all)
    bool canonicalSynth = false;
    std::wstring cancelEvent;
    std::vector<uint32_t> stemTracks;  // --tracks: keep notes of these tracks only
    std::wstring stems;                // --stems separate|merge|both: one instance per track
    const EngineConfig* tuning = nullptr;  // live-synth tuning (shared config)
};

// The machine stream is deliberately tiny and line-oriented so the standalone
// renderer remains easy to launch from non-SVMS frontends as well. Fields are
// tab separated and every record is flushed immediately.
void MachineStatus(const Options& o, const char* state,
                   const std::string& message = {}) {
    if (!o.machineProgress) return;
    std::string clean = message;
    for (char& c : clean) {
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    std::fprintf(stdout, "SVMS3\tSTATUS\t%s\t%s\n", state, clean.c_str());
    std::fflush(stdout);
}

void MachinePhase(const Options& o, int step, int steps, const char* name) {
    if (!o.machineProgress) return;
    std::fprintf(stdout, "SVMS3\tPHASE\t%d\t%d\t%s\n", step, steps, name);
    std::fflush(stdout);
}

void MachineLoadProgress(const Options& o, double progress, double elapsed,
                         double eta, uint64_t processed, uint64_t total) {
    if (!o.machineProgress) return;
    std::fprintf(stdout,
                 "SVMS3\tLOAD\t%.9f\t%.6f\t%.6f\t%llu\t%llu\n",
                 progress, elapsed, eta,
                 static_cast<unsigned long long>(processed),
                 static_cast<unsigned long long>(total));
    std::fflush(stdout);
}

void MachineRenderProgress(const Options& o, double progress, double elapsed,
                           double rendered, double total, double speed,
                           double eta, uint32_t active, uint32_t peak,
                           uint64_t steals, uint64_t events, bool tail) {
    if (!o.machineProgress) return;
    std::fprintf(stdout,
                 "SVMS3\tRENDER\t%.9f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f"
                 "\t%u\t%u\t%llu\t%llu\t%s\n",
                 progress, elapsed, rendered, total, speed, eta,
                 active, peak,
                 static_cast<unsigned long long>(steals),
                 static_cast<unsigned long long>(events),
                 tail ? "tail" : "midi");
    std::fflush(stdout);
}

class ExternalCancellation {
public:
    ~ExternalCancellation() {
#if defined(_WIN32)
        if (event_) CloseHandle(event_);
#endif
    }

    bool Open(const std::wstring& name, std::string& error) {
        if (name.empty()) return true;
#if defined(_WIN32)
        event_ = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
        if (!event_) {
            error = "cannot open cancellation event";
            return false;
        }
        return true;
#else
        (void)name;
        error = "external cancellation events are only supported on Windows";
        return false;
#endif
    }

    bool Requested() const {
#if defined(_WIN32)
        return event_ && WaitForSingleObject(event_, 0) == WAIT_OBJECT_0;
#else
        return false;
#endif
    }

private:
#if defined(_WIN32)
    HANDLE event_ = nullptr;
#endif
};

struct ScanProgressContext {
    const Options* options = nullptr;
    ExternalCancellation* externalCancel = nullptr;
    std::atomic<bool>* cancel = nullptr;
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point last{};
};

bool ReportScanProgress(uint64_t processed, uint64_t total,
                        uint64_t, void* user) {
    auto& context = *static_cast<ScanProgressContext*>(user);
    if (context.externalCancel->Requested())
        context.cancel->store(true, std::memory_order_relaxed);
    if (context.cancel->load(std::memory_order_relaxed)) return false;

    const auto now = std::chrono::steady_clock::now();
    if (processed < total && now - context.last < std::chrono::milliseconds(100))
        return true;
    context.last = now;
    const double elapsed = std::chrono::duration<double>(now - context.start).count();
    const double fraction = total
        ? (std::min)(1.0, static_cast<double>(processed) /
                           static_cast<double>(total))
        : 1.0;
    const double eta = fraction > 0.0
        ? (std::max)(0.0, elapsed * (1.0 - fraction) / fraction)
        : 0.0;
    MachineLoadProgress(*context.options, fraction, elapsed, eta,
                        processed, total);
    return true;
}

void ApplyConfigDefaults(const EngineConfig& cfg, Options& o) {
    o.tuningEdo = cfg.tuningEdo;
    o.phaseRotationMode = cfg.phaseRotationMode;
    if (cfg.renderBackend == RenderBackend::Scalar ||
        cfg.renderBackend == RenderBackend::SSE2 ||
        cfg.renderBackend == RenderBackend::AVX2)
        o.backend = cfg.renderBackend;  // else stays the automatic sentinel
    // The standalone renderer is still explicitly given a MIDI, SoundFont and
    // output path, but its engine-facing defaults should match the current V3
    // configuration. Command-line options below remain authoritative.
    o.sampleRate = cfg.sampleRate;
    o.maxVoices = cfg.maxVoices;
    o.renderThreads = cfg.renderThreads;
    o.masterVolume = cfg.masterVolume;
    o.limiterEnabled = cfg.limiterEnabled;
    o.limiterAlgorithm = cfg.limiterAlgorithm;
    o.limiterThreshold = cfg.limiterThreshold;
    o.limiterLookaheadMs = cfg.limiterLookaheadMs;
    o.limiterAttackMs = cfg.limiterAttackMs;
    o.limiterReleaseMs = cfg.limiterReleaseMs;
}

std::wstring FormatTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) return L"--:--:--";
    const uint64_t s = static_cast<uint64_t>(seconds + 0.5);
    wchar_t text[32];
#if defined(_WIN32)
    swprintf_s(text, L"%02llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
#else
    std::swprintf(text, sizeof(text) / sizeof(text[0]),
                  L"%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>((s / 60) % 60),
                  static_cast<unsigned long long>(s % 60));
#endif
    return text;
}

bool ParseU32(const wchar_t* s, uint32_t lo, uint32_t hi, uint32_t& out) {
    wchar_t* end = nullptr; const unsigned long v = wcstoul(s, &end, 10);
    if (!s[0] || *end || v < lo || v > hi) return false;
    out = static_cast<uint32_t>(v); return true;
}

bool ParseFloat(const wchar_t* s, float lo, float hi, float& out) {
    if (!s || !s[0]) return false;
    wchar_t* end = nullptr;
    const double v = wcstod(s, &end);
    if (*end || !std::isfinite(v) || v < lo || v > hi) return false;
    out = static_cast<float>(v);
    return true;
}

bool ParseOnOff(const wchar_t* s, bool& out) {
    if (!s) return false;
    const std::wstring value = s;
    if (value == L"on" || value == L"true" || value == L"1") {
        out = true;
        return true;
    }
    if (value == L"off" || value == L"false" || value == L"0") {
        out = false;
        return true;
    }
    return false;
}

bool ParseLimiterAlgorithm(const wchar_t* s, LimiterAlgorithm& out) {
    if (!s) return false;
    const std::wstring value = s;
    if (value == L"classic") {
        out = LimiterAlgorithm::Classic;
        return true;
    }
    if (value == L"adaptive") {
        out = LimiterAlgorithm::Adaptive;
        return true;
    }
    return false;
}

bool ParsePhaseRotationMode(const wchar_t* s, uint32_t& out) {
    if (!s) return false;
    const std::wstring value = s;
    if (value == L"coherent") { out = 0u; return true; }
    if (value == L"analytic") { out = 1u; return true; }
    if (value == L"sweep")    { out = 2u; return true; }
    if (value == L"diffuse")  { out = 3u; return true; }
    if (value == L"random")   { out = 4u; return true; }
    return false;
}


void Usage() {
    fputws(L"SuperVirtualMIDISynth V3 offline renderer\n\n"
           L"svms_v3_render <input.mid> <instrument.sf2|.sfz> <output.wav> [options]\n\n"
           L"svms_v3_render <input.mid> --scan-only\n\n"
           L"Engine-facing defaults are loaded from the active V3 config; CLI options override them.\n\n"
           L"  --sample-rate N       Output rate (default: V3 config)\n"
           L"  --max-voices N        Voice limit, 1-524288 (default: V3 config)\n"
           L"  --render-threads N     Total voice-render threads, 0-64 (default: V3 config)\n"
           L"  --synth legacy|canonical  Temporary offline synthesis selector\n"
           L"  --event-buffer-mb N   Parsed-event ring (default 128 MiB)\n"
           L"  --block-frames N      Render block size (default 8192)\n"
           L"  --tail-seconds N      Maximum natural-release tail (default 30)\n"
           L"  --master-volume F     Linear master gain, 0-4 (default: V3 config)\n"
           L"  --limiter on|off      Override V3 limiter enabled state\n"
           L"  --limiter-algorithm classic|adaptive  Override limiter algorithm\n"
           L"  --no-limiter          Alias for --limiter off\n"
           L"  --limiter-threshold F Override limiter threshold, 0.1-1.0\n"
           L"  --limiter-lookahead-ms F  Override lookahead, 0-20 ms\n"
           L"  --limiter-attack-ms F     Override attack, 0.01-100 ms\n"
           L"  --limiter-release-ms F    Override release, 1-5000 ms\n"
           L"  --phase-rotation coherent|analytic|sweep|diffuse|random  Post-mix hum-killing phase rotation (default coherent/off)\n"
                       L"  --backend auto|scalar|sse2|avx2|gpu\n"
           L"  --tuning-edo 12|31    Melodic tuning (31EDO key 155 = middle C)\n"
           L"  --config FILE         Use this V3 config JSON instead of the active one\n"
           L"  --tracks A,B,...      Stem: keep notes of these 0-based MIDI tracks only (length unchanged)\n"
           L"  --stems MODE          One synth instance per note-bearing track, up to --render-threads at\n"
           L"                        once. separate: <out>_trackN.wav each; merge: summed into <out>; both\n"
           L"  --full-velocity       Play every note at velocity 127 (also makes note merging fold more)\n"
           L"  --min-note-ms N       Hold every note at least N ms (KMC 18.2.8 BASSMIDI does 4); note-table renderer only for now\n"
           L"  --bassmidi-mode       Approximate KMC/BASSMIDI: 4 ms min note, voice limit drops notes that are not louder, DC kept; renders the MIDI length + 3 s unless --max-seconds is given\n"
           L"  --note-table CAP      Experimental: render from a note table, not the live engine (MIDI length + 3 s unless --max-seconds)\n"
           L"  --oversample N        Experimental: upsample every sample Nx (2/4/8) with a 16-point sinc at load\n"
           L"  --shards N            Key-sharded render: N synth cores plan in parallel (voices split between them)\n"
           L"  --max-seconds N       Render only the first N seconds of the MIDI (then the release tail)\n"
           L"  --scan-only           Validate/count without loading an instrument or rendering\n"
           L"  --quiet               Disable once-per-second telemetry\n"
           L"  --machine-progress    Emit tab-separated progress records\n", stderr);
}

bool ParseOptions(int argc, wchar_t** argv, Options& o) {
    if (argc >= 3 && std::wstring(argv[2]) == L"--scan-only") {
        o.midi = argv[1]; o.scanOnly = true;
    } else {
        if (argc < 4) return false;
        o.midi = argv[1]; o.soundfont = argv[2]; o.output = argv[3];
    }
    const int firstOption = o.scanOnly ? 3 : 4;
    for (int i = firstOption; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto value = [&]() -> const wchar_t* { return ++i < argc ? argv[i] : nullptr; };
        if (arg == L"--quiet") o.quiet = true;
        else if (arg == L"--config") { if (!value()) return false; }  // handled before Load()
        else if (arg == L"--machine-progress") o.machineProgress = true;
        else if (arg == L"--cancel-event") {
            const auto p=value(); if (!p || !p[0]) return false;
            o.cancelEvent = p;
        }
        else if (arg == L"--scan-only") o.scanOnly = true;
        else if (arg == L"--full-velocity") o.fullVelocity = true;
        else if (arg == L"--min-note-ms") { uint32_t v = 0; const auto p=value(); if (!p || !ParseU32(p,0,200,v)) return false; o.minNoteMs = static_cast<int32_t>(v); }
        else if (arg == L"--bassmidi-mode") { o.bassMode = true; o.noteTable = true; o.minNoteMs = 4; g_keepDcOffset = true; }
        else if (arg == L"--bassmidi-stream") { o.bassMode = true; o.noteTable = true; o.streamNoteTable = true; o.minNoteMs = 4; g_keepDcOffset = true; }
        else if (arg == L"--note-table") { const auto p=value(); if (!p || !ParseU32(p,0,4000000,o.noteTableCap)) return false; o.noteTable = true; }
        else if (arg == L"--oversample") { const auto p=value(); if (!p || !ParseU32(p,1,8,o.oversample)) return false; }
        else if (arg == L"--shards") { const auto p=value(); if (!p || !ParseU32(p,1,64,o.shards)) return false; }
        else if (arg == L"--max-seconds") { const auto p=value(); if (!p || !ParseU32(p,1,86400,o.maxSeconds)) return false; }
        else if (arg == L"--stems") {
            const auto p=value(); if (!p) return false;
            o.stems = p;
            if (o.stems != L"separate" && o.stems != L"merge" && o.stems != L"both") return false;
        }
        else if (arg == L"--tracks") {
            const auto p=value(); if (!p) return false;
            o.stemTracks.clear();
            for (wchar_t* s = const_cast<wchar_t*>(p); *s;) {
                wchar_t* end = nullptr;
                const unsigned long v = wcstoul(s, &end, 10);
                if (end == s) return false;
                o.stemTracks.push_back(static_cast<uint32_t>(v));
                s = (*end == L',') ? end + 1 : end;
                if (s == end && *end) return false;
            }
            if (o.stemTracks.empty()) return false;
        }
        else if (arg == L"--synth") {
            const auto p=value(); if (!p) return false;
            const std::wstring name=p;
            if (name == L"legacy") o.canonicalSynth=false;
            else if (name == L"canonical") o.canonicalSynth=true;
            else return false;
        }
        else if (arg == L"--tuning-edo") { const auto p=value(); if (!p || !ParseU32(p,12,31,o.tuningEdo) || (o.tuningEdo!=12 && o.tuningEdo!=31)) return false; }
        else if (arg == L"--no-limiter") o.limiterEnabled = false;
        else if (arg == L"--sample-rate") { const auto p=value(); if (!p || !ParseU32(p,8000,384000,o.sampleRate)) return false; }
        else if (arg == L"--max-voices") { const auto p=value(); if (!p || !ParseU32(p,1,kMaxPolyphony,o.maxVoices)) return false; }
        else if (arg == L"--render-threads") { const auto p=value(); if (!p || !ParseU32(p,0,64,o.renderThreads)) return false; }
        else if (arg == L"--event-buffer-mb") { const auto p=value(); if (!p || !ParseU32(p,1,4096,o.eventBufferMB)) return false; }
        else if (arg == L"--block-frames") { const auto p=value(); if (!p || !ParseU32(p,16,1048576,o.blockFrames)) return false; }
        else if (arg == L"--tail-seconds") { const auto p=value(); if (!p || !ParseU32(p,0,3600,o.maxTailSeconds)) return false; }
        else if (arg == L"--master-volume") {
            const auto p=value(); if (!p || !ParseFloat(p,0.0f,4.0f,o.masterVolume)) return false;
        } else if (arg == L"--limiter") {
            const auto p=value(); if (!p || !ParseOnOff(p,o.limiterEnabled)) return false;
        } else if (arg == L"--limiter-algorithm") {
            const auto p=value(); if (!p || !ParseLimiterAlgorithm(p,o.limiterAlgorithm)) return false;
        } else if (arg == L"--limiter-threshold") {
            const auto p=value(); if (!p || !ParseFloat(p,0.1f,1.0f,o.limiterThreshold)) return false;
        } else if (arg == L"--limiter-lookahead-ms") {
            const auto p=value(); if (!p || !ParseFloat(p,0.0f,20.0f,o.limiterLookaheadMs)) return false;
        } else if (arg == L"--limiter-attack-ms") {
            const auto p=value(); if (!p || !ParseFloat(p,0.01f,100.0f,o.limiterAttackMs)) return false;
        } else if (arg == L"--limiter-release-ms") {
            const auto p=value(); if (!p || !ParseFloat(p,1.0f,5000.0f,o.limiterReleaseMs)) return false;
        } else if (arg == L"--phase-rotation") {
            const auto p=value(); if (!p || !ParsePhaseRotationMode(p,o.phaseRotationMode)) return false;
        } else if (arg == L"--backend") {
            const auto p=value(); if (!p) return false; const std::wstring name=p;
            if (name == L"auto") o.backend=RenderBackend::AVX512;
            else if (name == L"scalar") o.backend=RenderBackend::Scalar;
            else if (name == L"sse2") o.backend=RenderBackend::SSE2;
            else if (name == L"avx2") o.backend=RenderBackend::AVX2;
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
            else if (name == L"gpu") o.backend=RenderBackend::GPU;
#endif
            else return false;
        } else return false;
    }
    return true;
}

class WaveWriter {
public:
    ~WaveWriter() { Close(); }
    bool Open(const wchar_t* path, uint32_t rate, uint64_t estimatedFrames) {
        Close(); rate_ = rate;
#if defined(_WIN32)
        file_ = _wfopen(path, L"wb+"); if (!file_) return false;
#else
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        const std::string utf8 = converter.to_bytes(path);
        file_ = std::fopen(utf8.c_str(), "wb+"); if (!file_) return false;
#endif
        (void)estimatedFrames;
        // The length is unknown while streaming, so reserve the ds64 slot as a
        // JUNK chunk; Close() either leaves it (plain RIFF) or turns the file
        // into RF64 when it outgrew 4 GB.
        fwrite("RIFF",1,4,file_); U32(0); fwrite("WAVE",1,4,file_);
        fwrite("JUNK",1,4,file_); U32(28); ds64Pos_=Tell();
        U64(0); U64(0); U64(0); U32(0);
        fwrite("fmt ",1,4,file_); U32(16); U16(3); U16(2); U32(rate); U32(rate*8); U16(8); U16(32);
        fwrite("data",1,4,file_); dataSizePos_=Tell(); U32(0);
        return true;
    }
    bool Write(const float* left, const float* right, uint32_t frames) {
        scratch_.resize(size_t(frames)*2);
        for (uint32_t i=0;i<frames;++i) { scratch_[i*2]=left[i]; scratch_[i*2+1]=right[i]; }
        if (fwrite(scratch_.data(), sizeof(float)*2, frames, file_) != frames) return false;
        frames_ += frames; return true;
    }
    bool Close() {
        if (!file_) return true;
        const uint64_t bytes=frames_*8, end=Tell();
        if (end - 8 > 0xffffffffull || bytes > 0xffffffffull) {
            Seek(0); fwrite("RF64",1,4,file_); U32(0xffffffffu);
            Seek(12); fwrite("ds64",1,4,file_);
            Seek(ds64Pos_); U64(end-8); U64(bytes); U64(frames_);
            Seek(dataSizePos_); U32(0xffffffffu);
        } else {
            Seek(4); U32(static_cast<uint32_t>(end-8));
            Seek(dataSizePos_); U32(static_cast<uint32_t>(bytes));
        }
        const bool ok=fclose(file_)==0; file_=nullptr; return ok;
    }
    uint64_t Frames() const { return frames_; }
private:
    uint64_t Tell() const {
#if defined(_WIN32)
        return static_cast<uint64_t>(_ftelli64(file_));
#else
        return static_cast<uint64_t>(ftello(file_));
#endif
    }
    void Seek(uint64_t offset) {
#if defined(_WIN32)
        _fseeki64(file_, static_cast<__int64>(offset), SEEK_SET);
#else
        fseeko(file_, static_cast<off_t>(offset), SEEK_SET);
#endif
    }
    void U16(uint16_t v) { fwrite(&v,2,1,file_); }
    void U32(uint32_t v) { fwrite(&v,4,1,file_); }
    void U64(uint64_t v) { fwrite(&v,8,1,file_); }
    FILE* file_=nullptr; uint32_t rate_=0;
    uint64_t frames_=0, ds64Pos_=0, dataSizePos_=0; std::vector<float> scratch_;
};

#if 0
// Retained temporarily as a readable oracle while the shared standalone
// facade proves itself on both platforms. Production uses StandaloneSynth.
struct PreparedRegion {
    float baseStep[kNoteCount]; float bendScale, attenuation, sustain, decaySlope, releaseDecay, panL, panR;
    uint32_t delay, hold, attack, decay, release; bool valid;
};

class OfflineSynth {
public:
    bool Initialize(const Options& o, std::string& error) {
        rate_=o.sampleRate; maxVoices_=o.maxVoices; master_=o.masterVolume;
        // () not {}: SF2Data holds 65536 SFSampleRegions with member initializers;
        // brace-init costs cl.exe ~3 s and far more memory in every TU that sees it.
        sf2_.reset(new SF2Data());
        if (!soundfont_load(o.soundfont.c_str(), sf2_.get())) { error="failed to load SoundFont"; return false; }
        sf2_build_regions(sf2_.get());
        if (sf2_->regionOverflow || sf2_->regionCount == 0) {
            error="SoundFont has no usable compiled regions"; return false;
        }
        sampleData_.resize(sf2_->sampleDataFrames + 8u);
        std::memcpy(sampleData_.data(), sf2_->sampleData,
                    static_cast<size_t>(sf2_->sampleDataFrames) * sizeof(int16_t));
        std::memset(sampleData_.data() + sf2_->sampleDataFrames, 0,
                    8u * sizeof(int16_t));
        sampleFrames_ = sf2_->sampleDataFrames;
        prepared_.resize(sf2_->regionCount);
        if (!voices_.Initialize(maxVoices_,rate_)) { error="cannot allocate voice storage"; return false; }
        if (!renderer_.ReserveVoiceCapacity(maxVoices_)) { error="cannot allocate renderer scratch"; return false; }
        if (!renderer_.ConfigureRenderThreads(o.renderThreads, o.blockFrames)) {
            error="cannot initialize render workers"; return false;
        }
        if (!renderer_.PrepareWholeVoice(voices_)) {
            error="cannot reserve whole-voice renderer storage"; return false;
        }
        channels_.Reset(); channels_.SetMasterVolume(master_);
        cfg_={master_,1.0f,0.0f,0,false,false,false,false,false,false,
              InterpolationMode::Linear,FilterType::None,PanLaw::ConstantPower,true};
        channels_.RebuildCache(cfg_,static_cast<float>(rate_));
        for (uint32_t i=0;i<sf2_->regionCount;++i) Prepare(sf2_->regions[i],prepared_[i]);
        for (uint8_t ch=0;ch<kChannelCount;++ch) RefreshPreset(ch);
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
        if (o.backend == RenderBackend::GPU) {
            std::string gpuErr;
            if (!gpuSynth_.Initialize(sampleData_.data(),
                                      sampleFrames_,
                                      o.maxVoices, o.blockFrames, gpuErr)) {
                error="GPU init failed: " + gpuErr; return false;
            }
            gpuEnabled_ = true;
        } else
#endif
        {
            if (o.backend != RenderBackend::AVX512 && !renderer_.SetRenderBackend(o.backend)) {
                error="requested render backend is unsupported on this CPU"; return false;
            }
            gpuEnabled_ = false;
        }
        for (float& r:bendRatio_) r=1.0f;
        postHighPass_.Initialize(rate_);
        postHighPass_.SetPassThrough(g_keepDcOffset || std::getenv("SVMS_NO_DCBLOCK") != nullptr);
        postHighPass_.SetPassThrough(g_keepDcOffset || std::getenv("SVMS_NO_DCBLOCK") != nullptr);
        voices_.SetPhaseRotationMode(o.phaseRotationMode);
        EngineConfig limiterConfig{};
        limiterConfig.limiterEnabled = o.limiterEnabled;
        limiterConfig.limiterAlgorithm = o.limiterAlgorithm;
        limiterConfig.limiterThreshold = o.limiterThreshold;
        limiterConfig.limiterLookaheadMs = o.limiterLookaheadMs;
        limiterConfig.limiterAttackMs = o.limiterAttackMs;
        limiterConfig.limiterReleaseMs = o.limiterReleaseMs;
        limiter_.Configure(rate_, limiterConfig);
        return true;
    }
    ~OfflineSynth() { if (sf2_) sf2_free(sf2_.get()); }
    void Dispatch(uint32_t msg, uint64_t frame) {
        voices_.SetCurrentFrame(frame);
        const uint8_t status=uint8_t(msg), ch=status&15, a=uint8_t(msg>>8), b=uint8_t(msg>>16);
        switch(status&0xf0) {
        case 0x80: NoteOff(ch,a); break;
        case 0x90: if (b) NoteOn(ch,a,b); else NoteOff(ch,a); break;
        case 0xb0: Control(ch,a,b); break;
        case 0xc0: Program(ch,a); break;
        case 0xe0: Bend(ch,a,b); break;
        default: break;
        }
    }
    void Render(float* l,float* r,uint32_t n,uint64_t frame) {
        std::fill(l,l+n,0.0f); std::fill(r,r+n,0.0f);
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
        if (gpuEnabled_) {
            voices_.SetCurrentFrame(frame);
            gpuSynth_.RenderBlock(voices_, l, r, n, frame);
        } else
#endif
        {
            renderer_.RenderBlock(voices_,channels_,sampleData_.data(),
                                  sampleFrames_,l,r,n,cfg_,
                                  nullptr,0,true,frame);
        }
        limiter_.ProcessPlanar(l,r,n,postHighPass_);
    }
    void ReleaseAll() { for(uint8_t ch=0;ch<kChannelCount;++ch) voices_.ReleaseChannel(ch,0); }
    uint32_t Active() const { return voices_.GetActiveCount(); }
    uint32_t Tails() const { return voices_.GetStealTailCount(); }
    uint32_t Steals() const { return voices_.stealCount_; }
    uint32_t Free() const { return maxVoices_-voices_.GetActiveCount(); }
    uint64_t NoteCalls() const { return noteCalls_; }
    uint64_t MatchedNotes() const { return notes_; }
    uint64_t MissingPresets() const { return missingPresets_; }
    uint64_t MissingRegions() const { return missingRegions_; }
    uint64_t FallbackRegions() const { return fallbackRegions_; }
    uint64_t InvalidRegions() const { return invalidRegions_; }
    const char* Backend() const {
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
        if (gpuEnabled_) return "gpu (d3d11)";
#endif
        return renderer_.GetRenderBackendName();
    }
private:
    struct RegionCacheEntry { uint32_t tag=UINT32_MAX;uint16_t count=0;uint16_t reserved=0;uint32_t indices[8]{}; };
    uint32_t ResolveRegions(uint32_t preset,uint8_t note,uint8_t velocity,const SFSampleRegion** out,uint32_t capacity){const uint32_t tag=(preset<<15u)|(uint32_t(note)<<7u)|velocity;uint32_t hash=tag;hash^=hash>>16u;hash*=0x7feb352du;hash^=hash>>15u;RegionCacheEntry& cached=regionCache_[hash&4095u];if(cached.tag==tag&&cached.count<=8u){const uint32_t copied=(std::min)(uint32_t(cached.count),capacity);for(uint32_t i=0;i<copied;++i)out[i]=&sf2_->regions[cached.indices[i]];return cached.count;}const uint32_t count=sf2_find_regions(sf2_.get(),preset,note,velocity,out,capacity);if(count<=8u&&count<=capacity){cached.tag=tag;cached.count=uint16_t(count);for(uint32_t i=0;i<count;++i)cached.indices[i]=uint32_t(out[i]-sf2_->regions);}return count;}
    void Prepare(const SFSampleRegion& rg, PreparedRegion& p) {
        p={}; if(!sf2_validate_region(sf2_.get(),&rg)||rg.sampleIndex>=sf2_->sampleCount)return;
        const SF2Sample& s=sf2_->samples[rg.sampleIndex]; const int root=rg.rootKey>=0?rg.rootKey:s.originalPitch;
        p.bendScale=(rg.scaleTuning?rg.scaleTuning:100)/100.0f;
        const float ratio=float(s.sampleRate?s.sampleRate:44100)/float(rate_);
        for(uint32_t n=0;n<kNoteCount;++n) p.baseStep[n]=ratio*powf(2.0f,((float(n)+rg.coarseTune+rg.fineTune/100.0f-root)*p.bendScale)/12.0f);
        p.attenuation=(rg.initialAttenuation>0||(sf2_->isSfz&&rg.initialAttenuation!=0))?InitialAttenuationToGain(float(rg.initialAttenuation)):1.0f;
        p.sustain=(std::min)(1.0f,SustainAttenuationToGain((std::max)(0.0f,float(rg.sustainVolEnv))));
        auto samples=[&](int16_t tc){const float s=TimecentsToSeconds(tc);return s>0?uint32_t(s*rate_):0u;};
        p.delay=samples(rg.delayVolEnv);p.hold=samples(rg.holdVolEnv);
        const float attackSeconds=TimecentsToSeconds(rg.attackVolEnv),decaySeconds=TimecentsToSeconds(rg.decayVolEnv);
        p.attack=attackSeconds>0.0001f?uint32_t(attackSeconds*rate_):0u;
        p.decay=decaySeconds>0.0001f?uint32_t(decaySeconds*rate_):0u;
        p.decaySlope=1.0f;if(p.decay){const float slope=-9.226f/p.decay;p.decaySlope=expf(slope);if(p.sustain>0&&p.sustain<1)p.decay=uint32_t(logf(p.sustain)/slope);}
        const float releaseSeconds=TimecentsToSeconds(rg.releaseVolEnv);p.releaseDecay=MakeReleaseDecay(releaseSeconds,rate_);p.release=MakeReleaseSamples(releaseSeconds,rate_);
        channels_.ComputeSoundFontPan(rg.pan,p.panL,p.panR);p.valid=true;
    }
    bool Resolve(uint8_t ch,uint32_t& preset) { return sf2_resolve_preset(sf2_.get(),channels_.GetBankMSB(ch),channels_.GetProgram(ch),channels_.IsPercussion(ch),&preset); }
    void RefreshPreset(uint8_t ch) { uint32_t p=0;channels_.SetSelectedPreset(ch,Resolve(ch,p)?uint16_t(p):UINT16_MAX); }
    uint32_t tuningEdo_ = EngineConfig::Load().tuningEdo;
    void NoteOn(uint8_t ch,uint8_t note,uint8_t vel) {
        ++noteCalls_;if(ch>=kChannelCount)return;channels_.NoteOn(ch,note,vel);uint32_t pi=channels_.GetSelectedPreset(ch);if(pi>=sf2_->presetCount){if(!Resolve(ch,pi)){++missingPresets_;return;}channels_.SetSelectedPreset(ch,uint16_t(pi));}
        const bool edo31=tuningEdo_==31u&&!channels_.IsPercussion(ch);
        const uint8_t regionNote=MidiRegionKey(note,edo31);
        const SFSampleRegion* regions[512];uint32_t count=ResolveRegions(pi,regionNote,vel,regions,512);
        if(!count||count>512){const uint16_t fb=sf2_->fallbackPresetIndex;
            if(fb<sf2_->presetCount&&fb!=pi)count=ResolveRegions(fb,regionNote,vel,regions,512);
            if(!count||count>512){++missingRegions_;return;}++fallbackRegions_;}
        for(uint32_t i=0;i<count;++i){const uint32_t ri=uint32_t(regions[i]-sf2_->regions);if(ri>=prepared_.size()||!prepared_[ri].valid){++invalidRegions_;return;}}
        if(playIndex_==0||playIndex_>=UINT32_MAX-1)playIndex_=1;const uint32_t generation=playIndex_++; VoiceHandle handles[512];
        const float velocityGain=float(vel)*float(vel)/(127.0f*127.0f);
        const float bend=channels_.GetPitchBendSemitones(ch);
        for(uint32_t i=0;i<count;++i){const auto& rg=*regions[i];const uint32_t ri=uint32_t(regions[i]-sf2_->regions);const auto& p=prepared_[ri];const float br=p.bendScale==1.0f?bendRatio_[ch]:powf(2.0f,bend*p.bendScale/12.0f);const float gain=velocityGain*p.attenuation;
            VoiceConfiguration& c=launchSetups_[i];c={};c.sampleStart=uint32_t(rg.startOffset);c.sampleEnd=uint32_t(rg.endOffset);c.loopStart=uint32_t(rg.loopStartOffset);c.loopEnd=uint32_t(rg.loopEndOffset);c.loopMode=rg.loopMode;c.playIndex=generation;c.delaySamples=p.delay;c.holdSamples=p.hold;c.attackSamples=p.attack;c.decaySamples=p.decay;c.releaseSamples=p.release;c.basePhaseStep=RetunePhaseStep(p.baseStep[note],note,p.bendScale,edo31);c.phaseStep=c.basePhaseStep*br;c.pitchBendScale=p.bendScale;c.initialGain=gain;c.sustainLevel=p.sustain;c.attackGainStep=p.attack?gain/p.attack:0;c.decaySlope=p.decaySlope;c.releaseDecay=p.releaseDecay;c.gainLeft=p.panL;c.gainRight=p.panR;c.presetIndex=uint16_t(pi);c.regionIndex=uint16_t(ri);c.exclusiveClass=rg.exclusiveClass>0?uint16_t(rg.exclusiveClass):0u;c.offByClass=rg.offByClass<0?UINT16_MAX:(rg.offByClass>0?uint16_t(rg.offByClass):0u);c.sampleBacked=1;}
        if(!voices_.LaunchVoiceGroup(ch,note,vel,launchSetups_,count,generation,channels_.GetParams()[ch],handles))return;
        ++notes_;
    }
    void NoteOff(uint8_t ch,uint8_t note){const bool sustain=channels_.IsSustainActive(ch);channels_.NoteOff(ch,note);voices_.NoteOffOldestPlayIndices(ch,note,1u,sustain,0u);}
    void Control(uint8_t ch,uint8_t cc,uint8_t value){const bool sustain=channels_.IsSustainActive(ch);channels_.ControlChange(ch,cc,value);if(cc==0||cc==32)RefreshPreset(ch);if(cc==64&&value<64){voices_.ForEachChannelActive(ch,[&](VoiceHandle h){if(voices_.v.heldBySustain[h]){voices_.v.heldBySustain[h]=0;voices_.StartRelease(h);}});}
        if(cc==120)voices_.SilenceChannelImmediate(ch);else if(cc==123)voices_.ReleaseChannel(ch,0);else if(cc==121&&sustain){voices_.ForEachChannelActive(ch,[&](VoiceHandle h){if(voices_.v.heldBySustain[h]){voices_.v.heldBySustain[h]=0;voices_.StartRelease(h);}});}
        if(cc==121)Bend(ch,0,64);if(cc==7||cc==10||cc==11||cc==64||cc==121){channels_.RebuildChannel(ch,cfg_,float(rate_));if(cc==7||cc==10||cc==11||cc==121)voices_.MarkChannelMixStale(ch,channels_.GetParams()[ch]);}}
    void Program(uint8_t ch,uint8_t p){const uint8_t old=channels_.GetProgram(ch);channels_.ProgramChange(ch,p);uint32_t pi;(void)old;if(Resolve(ch,pi))channels_.SetSelectedPreset(ch,uint16_t(pi));else channels_.SetSelectedPreset(ch,UINT16_MAX);}
    void Bend(uint8_t ch,uint8_t lo,uint8_t hi){channels_.PitchBend(ch,int16_t((hi<<7)|lo));const float semis=channels_.GetPitchBendSemitones(ch);const float common=powf(2.0f,semis/12.0f);bendRatio_[ch]=common;voices_.ForEachChannelActive(ch,[&](VoiceHandle v){const float scale=voices_.v.pitchBendScales[v];voices_.v.phaseIncs[v]=voices_.v.basePhaseIncs[v]*(scale==1?common:powf(2.0f,semis*scale/12.0f));});}
    uint32_t rate_=0,maxVoices_=0,playIndex_=0;float master_=0,bendRatio_[16]{};bool gpuEnabled_=false;uint64_t notes_=0,noteCalls_=0,missingPresets_=0,missingRegions_=0,invalidRegions_=0,fallbackRegions_=0;
    std::unique_ptr<SF2Data> sf2_;std::vector<int16_t> sampleData_;uint32_t sampleFrames_ = 0;std::vector<PreparedRegion> prepared_;
    VoiceManager voices_;ChannelCache channels_;RenderScalar renderer_;RuntimeConfigSnapshot cfg_{};PostHighPass3Hz postHighPass_{};LimiterRouterState limiter_{};RegionCacheEntry regionCache_[4096]{};VoiceConfiguration launchSetups_[512]{};
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
    GpuSynth gpuSynth_;
#endif
};

#endif

class OfflineSessionSynth {
public:
    virtual ~OfflineSessionSynth() = default;
    virtual bool Initialize(const Options&, std::string&) = 0;
    virtual void Dispatch(uint32_t message, uint64_t frame) = 0;
    virtual void Render(float* left, float* right, uint32_t frames,
                        uint64_t absoluteFrame) = 0;
    virtual void ReleaseAll() = 0;
    // Whole-block event rendering (one RenderBlock per block, events at their
    // exact frames). Sessions that cannot do it return false and the caller
    // uses the per-event Dispatch/Render loop.
    virtual bool RenderEvents(const RealtimeEvent*, size_t, float*, float*,
                              uint32_t, uint64_t) { return false; }
    // Cumulative whole-voice phase cycles {plan, jobs, post}; zeros if unknown.
    virtual void PhaseCycles(uint64_t (&out)[3]) const { out[0] = out[1] = out[2] = 0; }
    // {note-on collapse coalesced, re-excited, exact-merge folded}
    virtual void CollapseCounts(uint64_t (&out)[3]) const { out[0] = out[1] = out[2] = 0; }
    virtual bool ReverbActive() const { return false; }
    virtual uint32_t Active() const = 0;
    virtual uint32_t Tails() const = 0;
    virtual uint32_t Steals() const = 0;
    virtual uint32_t Free() const = 0;
    virtual uint64_t NoteCalls() const = 0;
    virtual uint64_t MatchedNotes() const = 0;
    virtual uint64_t PhysicalLaunches() const = 0;
    virtual uint64_t RetiredVoices() const = 0;
    virtual uint64_t DroppedVoices() const = 0;
    virtual canonical::CanonicalTelemetry CanonicalDetails() const { return {}; }
    virtual uint64_t MissingPresets() const = 0;
    virtual uint64_t MissingRegions() const = 0;
    virtual uint64_t InvalidRegions() const = 0;
    virtual uint64_t FallbackRegions() const = 0;
    virtual const char* Backend() const = 0;
};

class LegacyOfflineSession final : public OfflineSessionSynth {
public:
    bool Initialize(const Options& o, std::string& error) override {
        StandaloneSynthConfig config{};
        config.soundfont = o.soundfont;
        config.sampleRate = o.sampleRate;
        config.maxVoices = o.maxVoices;
        config.renderThreads = o.renderThreads;
        config.maxBlockFrames = o.blockFrames;
        config.masterVolume = o.masterVolume;
        config.limiterEnabled = o.limiterEnabled;
        config.limiterAlgorithm = o.limiterAlgorithm;
        config.limiterThreshold = o.limiterThreshold;
        config.limiterLookaheadMs = o.limiterLookaheadMs;
        config.limiterAttackMs = o.limiterAttackMs;
        config.limiterReleaseMs = o.limiterReleaseMs;
        config.backend = o.backend;
        config.phaseRotationMode = o.phaseRotationMode;
        config.tuningEdo = o.tuningEdo;
        config.oversample = o.oversample;
        config.tuning = o.tuning;
        return synth_.Initialize(config, error);
    }
    void Dispatch(uint32_t message, uint64_t frame) override {
        synth_.Dispatch(message, frame);
    }
    void Render(float* left, float* right, uint32_t frames,
                uint64_t frame) override {
        synth_.Render(left, right, frames, frame);
    }
    void ReleaseAll() override { synth_.ReleaseAll(); }
    void SetRawMix(bool raw) { synth_.SetRawMix(raw); }
    float StealLevel(uint8_t ch, uint8_t key) const { return synth_.StealLevel(ch, key); }
    uint32_t VoiceCount(uint8_t ch, uint8_t key) const { return synth_.VoiceCount(ch, key); }
    void PostProcess(float* l, float* r, uint32_t n) { synth_.PostProcess(l, r, n); }
    bool RenderEvents(const RealtimeEvent* events, size_t count, float* left,
                      float* right, uint32_t frames, uint64_t frame) override {
        block_.Render(synth_, events, count, left, right, frames, frame);
        return true;
    }
    void PhaseCycles(uint64_t (&out)[3]) const override {
        out[0] = synth_.WvPlanCycles(); out[1] = synth_.WvJobCycles();
        out[2] = synth_.WvPostCycles();
    }
    void CollapseCounts(uint64_t (&out)[3]) const override {
        out[0] = synth_.CoalescedNoteOns(); out[1] = synth_.ReExcitedNoteOns();
        out[2] = synth_.MergedNoteOns();
    }
    bool ReverbActive() const override { return synth_.ReverbActive(); }
    uint32_t Active() const override { return synth_.Active(); }
    uint32_t Tails() const override { return synth_.Tails(); }
    uint32_t Steals() const override { return synth_.Steals(); }
    uint32_t Free() const override { return synth_.Free(); }
    uint64_t NoteCalls() const override { return synth_.NoteCalls(); }
    uint64_t MatchedNotes() const override { return synth_.MatchedNotes(); }
    uint64_t PhysicalLaunches() const override { return synth_.PhysicalLaunches(); }
    uint64_t RetiredVoices() const override { return synth_.RetiredVoices(); }
    uint64_t DroppedVoices() const override { return 0; }
    uint64_t MissingPresets() const override { return synth_.MissingPresets(); }
    uint64_t MissingRegions() const override { return synth_.MissingRegions(); }
    uint64_t InvalidRegions() const override { return synth_.InvalidRegions(); }
    uint64_t FallbackRegions() const override { return synth_.FallbackRegions(); }
    const char* Backend() const override { return synth_.Backend(); }
private:
    StandaloneSynth synth_;
    RealtimeBlockRenderer block_;
};

// Key-sharded render (the offline twin of the driver's synth.shards): note
// events go to the core that owns (channel,key), everything else to all
// cores; cores render raw mixes in parallel, then the sum gets reverb and
// the limiter once. The serial plan loop (note launch/steal) is what bounds
// Black MIDI speed, and this splits it. Voices are divided between cores.
// ponytail: each core loads its own SoundFont copy; share one bundle if RAM bites.
class ShardedOfflineSession final : public OfflineSessionSynth {
public:
    ~ShardedOfflineSession() override {
        StopWorkers();
        if (std::getenv("SVMS_OFFLINE_PROFILE"))
            for (size_t i = 0; i < filterNs_.size(); ++i)
                std::fprintf(stderr, "\n[shard %zu] filter %.2fs render %.2fs events %llu",
                             i, filterNs_[i] * 1e-9, renderNs_[i] * 1e-9,
                             (unsigned long long)keptEvents_[i]);
    }
    bool Initialize(const Options& o, std::string& error) override {
        const uint32_t n = o.shards;
        Options c = o;
        c.maxVoices = (std::max)(1u, (o.maxVoices + n - 1u) / n);
        c.renderThreads = (std::max)(1u, (o.renderThreads
            ? o.renderThreads
            : (std::max)(1u, std::thread::hardware_concurrency())) / n);
        shards_.resize(n);
        std::vector<std::string> errors(n);
        std::vector<char> ok(n, 0);
        std::vector<std::thread> loaders;
        for (uint32_t i = 0; i < n; ++i)
            loaders.emplace_back([&, i] {
                shards_[i] = std::make_unique<LegacyOfflineSession>();
                ok[i] = shards_[i]->Initialize(c, errors[i]);
            });
        for (auto& t : loaders) t.join();
        for (uint32_t i = 0; i < n; ++i)
            if (!ok[i]) { error = errors[i]; return false; }
        for (auto& s : shards_) s->SetRawMix(true);
        mine_.resize(n);
        filterNs_.assign(n, 0); renderNs_.assign(n, 0); keptEvents_.assign(n, 0);
        planeL_.resize(n); planeR_.resize(n);
        for (uint32_t i = 1; i < n; ++i) {
            planeL_[i].resize(o.blockFrames); planeR_[i].resize(o.blockFrames);
            workers_.emplace_back([this, i] { WorkerLoop(i); });
        }
        return true;
    }
    void Dispatch(uint32_t message, uint64_t frame) override {
        for (auto& s : shards_) s->Dispatch(message, frame);
    }
    void Render(float* left, float* right, uint32_t frames,
                uint64_t absoluteFrame) override {
        RenderEvents(nullptr, 0, left, right, frames, absoluteFrame);
    }
    void ReleaseAll() override { for (auto& s : shards_) s->ReleaseAll(); }
    bool RenderEvents(const RealtimeEvent* events, size_t count, float* left,
                      float* right, uint32_t frames, uint64_t frame) override {
        if (!left) return true;  // capability probe
        job_ = {events, count, frames, frame};
        outL_ = left; outR_ = right;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = static_cast<uint32_t>(workers_.size());
            ++generation_;
        }
        wake_.notify_all();
        RunShard(0);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            done_.wait(lock, [&] { return pending_ == 0; });
        }
        for (size_t i = 1; i < shards_.size(); ++i)
            for (uint32_t f = 0; f < frames; ++f) {
                left[f] += planeL_[i][f];
                right[f] += planeR_[i][f];
            }
        shards_[0]->PostProcess(left, right, frames);
        return true;
    }
    void PhaseCycles(uint64_t (&out)[3]) const override {
        out[0] = out[1] = out[2] = 0;
        for (auto& s : shards_) {
            uint64_t c[3]; s->PhaseCycles(c);
            for (int k = 0; k < 3; ++k) out[k] += c[k];
        }
    }
    void CollapseCounts(uint64_t (&out)[3]) const override {
        out[0] = out[1] = out[2] = 0;
        for (auto& s : shards_) {
            uint64_t c[3]; s->CollapseCounts(c);
            for (int k = 0; k < 3; ++k) out[k] += c[k];
        }
    }
    bool ReverbActive() const override { return shards_[0]->ReverbActive(); }
#define SVMS_SHARD_SUM(type, name) \
    type name() const override { type t = 0; for (auto& s : shards_) t += s->name(); return t; }
    SVMS_SHARD_SUM(uint32_t, Active) SVMS_SHARD_SUM(uint32_t, Tails)
    SVMS_SHARD_SUM(uint32_t, Steals) SVMS_SHARD_SUM(uint32_t, Free)
    SVMS_SHARD_SUM(uint64_t, NoteCalls) SVMS_SHARD_SUM(uint64_t, MatchedNotes)
    SVMS_SHARD_SUM(uint64_t, PhysicalLaunches) SVMS_SHARD_SUM(uint64_t, RetiredVoices)
    SVMS_SHARD_SUM(uint64_t, DroppedVoices) SVMS_SHARD_SUM(uint64_t, MissingPresets)
    SVMS_SHARD_SUM(uint64_t, MissingRegions) SVMS_SHARD_SUM(uint64_t, InvalidRegions)
    SVMS_SHARD_SUM(uint64_t, FallbackRegions)
#undef SVMS_SHARD_SUM
    const char* Backend() const override { return shards_[0]->Backend(); }
private:
    struct Job { const RealtimeEvent* events; size_t count; uint32_t frames; uint64_t frame; };

    static uint32_t ShardFor(uint32_t channel, uint32_t note, uint32_t n) {
        return (((channel * 128u + note) * 2654435761u) >> 16u) % n;
    }
    void RunShard(uint32_t index) {
        const uint32_t n = static_cast<uint32_t>(shards_.size());
        auto& mine = mine_[index];
        const auto t0 = std::chrono::steady_clock::now();
        mine.clear();
        for (size_t i = 0; i < job_.count; ++i) {
            const RealtimeEvent& e = job_.events[i];
            const uint8_t status = static_cast<uint8_t>(e.message);
            const uint8_t type = status & 0xf0u;
            if (!e.reset && (type == 0x80u || type == 0x90u) &&
                !IsInternalEngineMessage(e.message) &&
                ShardFor(status & 0x0fu, (e.message >> 8u) & 0x7fu, n) != index)
                continue;
            mine.push_back(e);
        }
        float* l = index == 0 ? outL_ : planeL_[index].data();
        float* r = index == 0 ? outR_ : planeR_[index].data();
        const auto t1 = std::chrono::steady_clock::now();
        shards_[index]->RenderEvents(mine.data(), mine.size(), l, r,
                                     job_.frames, job_.frame);
        const auto t2 = std::chrono::steady_clock::now();
        filterNs_[index] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        renderNs_[index] += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        keptEvents_[index] += mine.size();
    }
    void WorkerLoop(uint32_t index) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            RunShard(index);
            std::lock_guard<std::mutex> lock(mutex_);
            if (--pending_ == 0) done_.notify_one();
        }
    }
    void StopWorkers() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        wake_.notify_all();
        for (auto& t : workers_) t.join();
        workers_.clear();
    }

    std::vector<std::unique_ptr<LegacyOfflineSession>> shards_;
    std::vector<std::vector<RealtimeEvent>> mine_;
    std::vector<std::vector<float>> planeL_, planeR_;
    std::vector<uint64_t> filterNs_, renderNs_, keptEvents_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    uint64_t generation_ = 0;
    uint32_t pending_ = 0;
    bool stop_ = false;
    Job job_{};
    float* outL_ = nullptr;
    float* outR_ = nullptr;
};

class CanonicalOfflineSession final : public OfflineSessionSynth {
public:
    bool Initialize(const Options& o, std::string& error) override {
        if (o.phaseRotationMode != 0u || o.tuningEdo != 12u) {
            error = "canonical offline mode does not yet support phase rotation or 31-EDO";
            return false;
        }
        canonical::CanonicalEngineConfig config{};
        config.sampleRate = o.sampleRate;
        config.voiceCapacity = o.maxVoices;
        config.maxBlockFrames = o.blockFrames;
        config.workerThreads = o.renderThreads;
        if (config.workerThreads == 0u) {
            config.workerThreads = (std::min)(16u,
                (std::max)(1u, std::thread::hardware_concurrency()));
        }
        config.eventScratchCapacity = (std::max)(65536u, o.maxVoices * 4u);
        if (o.backend == RenderBackend::Scalar) {
            config.backend = canonical::CanonicalBackend::Scalar;
        } else if (o.backend == RenderBackend::AVX2 ||
                   o.backend == RenderBackend::AVX512) {
            config.backend = canonical::CanonicalEngine::avx2Supported()
                ? canonical::CanonicalBackend::Avx2
                : canonical::CanonicalBackend::Scalar;
        } else {
            error = "canonical offline mode supports only scalar, AVX2, or auto";
            return false;
        }
        try {
            engine_ = std::make_unique<canonical::CanonicalEngine>(
                canonical::Path(o.soundfont), config);
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
        maxVoices_ = o.maxVoices;
        masterVolume_ = o.masterVolume;
        EngineConfig limiterConfig{};
        limiterConfig.limiterEnabled = o.limiterEnabled;
        limiterConfig.limiterAlgorithm = o.limiterAlgorithm;
        limiterConfig.limiterThreshold = o.limiterThreshold;
        limiterConfig.limiterLookaheadMs = o.limiterLookaheadMs;
        limiterConfig.limiterAttackMs = o.limiterAttackMs;
        limiterConfig.limiterReleaseMs = o.limiterReleaseMs;
        highPass_.Initialize(o.sampleRate);
        highPass_.SetPassThrough(g_keepDcOffset || std::getenv("SVMS_NO_DCBLOCK") != nullptr);
        highPass_.SetPassThrough(g_keepDcOffset || std::getenv("SVMS_NO_DCBLOCK") != nullptr);
        limiter_.Configure(o.sampleRate, limiterConfig);
        return true;
    }

    void Dispatch(uint32_t message, uint64_t frame) override {
        const uint8_t status = static_cast<uint8_t>(message);
        canonical::CanonicalEvent event{};
        event.absoluteFrame = frame;
        event.sequence = sequence_++;
        event.channel = status & 15u;
        event.data1 = static_cast<uint8_t>(message >> 8);
        event.data2 = static_cast<uint8_t>(message >> 16);
        switch (status & 0xf0u) {
        case 0x80: event.type = canonical::CanonicalEventType::NoteOff; break;
        case 0x90: event.type = event.data2
            ? canonical::CanonicalEventType::NoteOn
            : canonical::CanonicalEventType::NoteOff; break;
        case 0xb0: event.type = canonical::CanonicalEventType::ControlChange; break;
        case 0xc0: event.type = canonical::CanonicalEventType::ProgramChange; break;
        case 0xe0: event.type = canonical::CanonicalEventType::PitchBend; break;
        default: return;
        }
        engine_->dispatch(event);
    }

    void Render(float* left, float* right, uint32_t frames,
                uint64_t frame) override {
        engine_->renderBlock(frame, {left, frames}, {right, frames});
        if (masterVolume_ != 1.0f) {
            for (uint32_t i = 0; i < frames; ++i) {
                left[i] *= masterVolume_;
                right[i] *= masterVolume_;
            }
        }
        limiter_.ProcessPlanar(left, right, frames, highPass_);
        position_ = frame + frames;
    }

    void ReleaseAll() override { engine_->releaseAll(position_); }
    uint32_t Active() const override { return engine_->telemetry().activeVoices; }
    uint32_t Tails() const override { return 0; }
    uint32_t Steals() const override {
        return static_cast<uint32_t>((std::min<std::uint64_t>)(
            engine_->telemetry().stolenVoices, UINT32_MAX));
    }
    uint32_t Free() const override { return maxVoices_ - Active(); }
    uint64_t NoteCalls() const override { return engine_->telemetry().sourceNoteOns; }
    uint64_t MatchedNotes() const override { return engine_->telemetry().sourceNoteOns; }
    uint64_t PhysicalLaunches() const override {
        return engine_->telemetry().launchedVoices;
    }
    uint64_t RetiredVoices() const override {
        return engine_->telemetry().retiredVoices;
    }
    uint64_t DroppedVoices() const override {
        const auto telemetry = engine_->telemetry();
        return telemetry.droppedNoteOns + telemetry.rejectedEvents;
    }
    canonical::CanonicalTelemetry CanonicalDetails() const override {
        return engine_->telemetry();
    }
    uint64_t MissingPresets() const override { return 0; }
    uint64_t MissingRegions() const override { return 0; }
    uint64_t InvalidRegions() const override { return 0; }
    uint64_t FallbackRegions() const override { return 0; }
    const char* Backend() const override { return engine_->backendName(); }
private:
    std::unique_ptr<canonical::CanonicalEngine> engine_;
    LimiterRouterState limiter_{};
    PostHighPass3Hz highPass_{};
    uint64_t position_{};
    uint32_t sequence_{};
    uint32_t maxVoices_{};
    float masterVolume_{1.0f};
};

struct ProducerContext { ParsedEventRing* ring; const MappedMidiFile* file; uint32_t rate; std::atomic<bool>* cancel; std::atomic<bool>* done; std::atomic<uint64_t>* decoded; std::string* error; };
bool RingSink(const PackedMidiEvent& e,void* p){auto& c=*static_cast<ProducerContext*>(p);if(!c.ring->Push(e,*c.cancel))return false;c.decoded->fetch_add(1,std::memory_order_relaxed);return true;}

} // namespace
} // namespace svms

static int RenderOne(svms::Options o, const svms::EngineConfig& engineConfig);

namespace {
std::string NarrowPath(const std::wstring& path) {
#if defined(_WIN32)
    return std::string(path.begin(), path.end());  // unused on Windows
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(path);
#endif
}
FILE* OpenPath(const std::wstring& path, const char* mode) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
    return std::fopen(NarrowPath(path).c_str(), mode);
#endif
}
void RemovePath(const std::wstring& path) {
#if defined(_WIN32)
    _wremove(path.c_str());
#else
    std::remove(NarrowPath(path).c_str());
#endif
}
bool SeekEnd(FILE* f, uint64_t& size) {
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) return false;
    size = static_cast<uint64_t>(_ftelli64(f));
    return true;
#else
    if (fseeko(f, 0, SEEK_END) != 0) return false;
    size = static_cast<uint64_t>(ftello(f));
    return true;
#endif
}
bool SeekTo(FILE* f, uint64_t pos) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<__int64>(pos), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

std::wstring StemPath(const std::wstring& out, uint32_t track) {
    std::wstring base = out;
    const size_t dot = base.rfind(L'.');
    const size_t sep = base.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep))
        base.resize(dot);
    return base + L"_track" + std::to_wstring(track) + L".wav";
}

// Sum float32-stereo WAVs written by WaveWriter (fixed 80-byte header) into
// `out`. Stems differ in length by their release tails; short ones are padded.
bool MergeStems(const std::vector<std::wstring>& paths, const std::wstring& out,
                uint32_t rate) {
    using svms::WaveWriter;
#if defined(_WIN32)
    _setmaxstdio(8192);
#endif
    constexpr uint64_t kHeader = 80;
    std::vector<FILE*> files;
    uint64_t frames = 0;
    bool ok = true;
    for (const auto& p : paths) {
        FILE* f = OpenPath(p, "rb");
        uint64_t size = 0;
        if (!f || !SeekEnd(f, size) || size < kHeader || !SeekTo(f, kHeader)) {
            if (f) std::fclose(f);
            ok = false; break;
        }
        files.push_back(f);
        frames = (std::max)(frames, (size - kHeader) / 8u);
    }
    WaveWriter wave;
    if (ok) ok = wave.Open(out.c_str(), rate, 0);
    const uint32_t kBlock = 16384;
    std::vector<float> in(size_t(kBlock) * 2), left(kBlock), right(kBlock);
    for (uint64_t done = 0; ok && done < frames; done += kBlock) {
        const uint32_t n = static_cast<uint32_t>((std::min<uint64_t>)(kBlock, frames - done));
        std::fill(left.begin(), left.begin() + n, 0.0f);
        std::fill(right.begin(), right.begin() + n, 0.0f);
        for (FILE* f : files) {
            const size_t got = std::fread(in.data(), sizeof(float) * 2, n, f);
            for (size_t i = 0; i < got; ++i) {
                left[i] += in[i * 2];
                right[i] += in[i * 2 + 1];
            }
        }
        ok = wave.Write(left.data(), right.data(), n);
    }
    for (FILE* f : files) std::fclose(f);
    return wave.Close() && ok;
}

// Creates <output dir>/<midi name>/ for separate stems; if that already exists,
// <output dir>/<YYYYMMDD_HHMMSS>_<midi name>/. Returns "" on failure.
std::wstring MakeStemDir(const std::wstring& out, const std::wstring& midi) {
    auto split = [](const std::wstring& p, std::wstring& dir, std::wstring& name) {
        const size_t sep = p.find_last_of(L"\\/");
        dir = sep == std::wstring::npos ? L"" : p.substr(0, sep + 1);
        name = sep == std::wstring::npos ? p : p.substr(sep + 1);
    };
    std::wstring dir, name, d2, file;
    split(out, dir, file);
    split(midi, d2, name);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    if (name.empty()) name = L"render";
    auto make = [](const std::wstring& p) {
#if defined(_WIN32)
        return CreateDirectoryW(p.c_str(), nullptr) != 0;
#else
        return mkdir(NarrowPath(p).c_str(), 0777) == 0;
#endif
    };
    std::wstring path = dir + name;
    if (make(path)) return path + L"/";
    wchar_t stamp[32];
    const std::time_t now = std::time(nullptr);
    std::wcsftime(stamp, 32, L"%Y%m%d_%H%M%S_", std::localtime(&now));
    path = dir + stamp + name;
    return make(path) ? path + L"/" : L"";
}

// ponytail: every instance loads its own SoundFont copy; share one bundle if RAM bites.
int RunStems(const svms::Options& o, const svms::EngineConfig& cfg) {
    using namespace svms;
    std::string error;
    std::vector<uint32_t> tracks;
    {
        MappedMidiFile midi;
        if (!midi.Open(o.midi.c_str(), error) ||
            !ParallelMidiDecoder::NoteTracks(midi, tracks, error)) {
            MachineStatus(o, "ERROR", error);
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
    }
    if (tracks.empty()) {
        MachineStatus(o, "ERROR", "no track contains notes");
        std::fprintf(stderr, "error: no track contains notes\n");
        return 1;
    }
    const uint32_t threads = o.renderThreads
        ? o.renderThreads
        : (std::min)(16u, (std::max)(1u, std::thread::hardware_concurrency()));
    const uint32_t concurrent = static_cast<uint32_t>(
        (std::min<size_t>)(threads, tracks.size()));
    const uint32_t perInstance = (std::max)(1u, threads / concurrent);
    const bool human = !o.quiet && !o.machineProgress;
    if (human)
        std::fprintf(stderr, "Stems: %zu tracks, %u at a time, %u render thread(s) each\n",
                     tracks.size(), concurrent, perInstance);
    MachineStatus(o, "RENDERING", "Rendering tracks");

    // Merge-only stems are temporary: leave them beside the output. Otherwise
    // they get their own folder, so repeated renders never overwrite each other.
    std::wstring stemOut = o.output;
    if (o.stems != L"merge") {
        const std::wstring dir = MakeStemDir(o.output, o.midi);
        if (!dir.empty()) {
            const size_t sep = o.output.find_last_of(L"\\/");
            stemOut = dir + (sep == std::wstring::npos ? o.output : o.output.substr(sep + 1));
        }
    }

    const auto start = std::chrono::steady_clock::now();
    std::atomic<size_t> next{0};
    std::atomic<uint32_t> finished{0};
    std::atomic<int> failure{0};
    std::vector<std::thread> pool;
    for (uint32_t w = 0; w < concurrent; ++w) {
        pool.emplace_back([&] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= tracks.size() || failure.load() != 0) return;
                Options c = o;
                c.stems.clear();
                c.stemTracks = {tracks[i]};
                c.output = StemPath(stemOut, tracks[i]);
                c.quiet = true;
                c.machineProgress = false;
                c.renderThreads = perInstance;
                c.eventBufferMB = (std::max)(8u, o.eventBufferMB / concurrent);
                const int r = RenderOne(c, cfg);
                if (r != 0) {
                    int none = 0;
                    failure.compare_exchange_strong(none, r);
                    return;
                }
                const uint32_t d = finished.fetch_add(1) + 1;
                if (human)
                    std::fprintf(stderr, "track %u done (%u/%zu)\n", tracks[i], d, tracks.size());
                MachineRenderProgress(
                    o, double(d) / double(tracks.size()),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    0.0, 0.0, 0.0, 0.0, 0, 0, 0, 0, false);
            }
        });
    }
    for (auto& t : pool) t.join();

    std::vector<std::wstring> paths;
    for (uint32_t t : tracks) paths.push_back(StemPath(stemOut, t));
    int rc = failure.load();
    if (rc == 0 && o.stems != L"separate") {
        MachineStatus(o, "RENDERING", "Merging track stems");
        if (!MergeStems(paths, o.output, o.sampleRate)) {
            MachineStatus(o, "ERROR", "merging stems failed");
            std::fprintf(stderr, "error: merging stems failed\n");
            rc = 1;
        }
    }
    if (rc == 0 && o.stems == L"merge")
        for (const auto& p : paths) RemovePath(p);
    if (rc == 0) MachineStatus(o, "COMPLETE", "Render completed successfully");
    else if (rc == 3) MachineStatus(o, "CANCELLED", "Render cancelled; partial WAVs retained");
    return rc;
}
#include "synth/NoteTableRender.inl"
#include "synth/NoteTableStream.inl"
} // namespace

int RendererMain(int argc, wchar_t** argv) {
    using namespace svms;
#if defined(_WIN32)
    // --config <file>: render with that JSON instead of the on-disk config
    // (the Configurator passes its unsaved working settings this way). Must
    // be in the environment before EngineConfig::Load() resolves its path.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--config") {
            SetEnvironmentVariableW(L"SVMS_TEST_LOCAL_CONFIG_PATH", argv[i + 1]);
            break;
        }
    }
    const EngineConfig engineConfig = EngineConfig::Load();
#else
    EngineConfig engineConfig{};
    engineConfig.sampleRate = kDefaultSampleRate;
    engineConfig.maxVoices = 4096u;
    engineConfig.renderThreads = 1u;
    engineConfig.masterVolume = 1.0f;
    engineConfig.limiterEnabled = true;
    engineConfig.limiterAlgorithm = LimiterAlgorithm::Classic;
    engineConfig.limiterThreshold = 0.95f;
    engineConfig.limiterLookaheadMs = 3.0f;
    engineConfig.limiterAttackMs = 0.5f;
    engineConfig.limiterReleaseMs = 100.0f;
#endif

    Options o;
    ApplyConfigDefaults(engineConfig, o);
    o.tuning = &engineConfig;
    if (!ParseOptions(argc, argv, o)) {
        Usage();
        return 2;
    }
    if (o.noteTable) return o.streamNoteTable ? RunNoteTableStream(o, engineConfig) : RunNoteTable(o, engineConfig);
    if (!o.stems.empty() && !o.scanOnly) return RunStems(o, engineConfig);
    return RenderOne(o, engineConfig);
}

// One complete render (decode -> synth -> WAV) of `o`. Stem mode runs several
// of these side by side, each with its own synth instance.
static int RenderOne(svms::Options o, const svms::EngineConfig& engineConfig) {
    using namespace svms;
    auto fail = [&](const std::string& message) {
        MachineStatus(o, "ERROR", message);
        std::fprintf(stderr, "error: %s\n", message.c_str());
        return 1;
    };

    const bool humanProgress = !o.quiet && !o.machineProgress;
    if (humanProgress && !engineConfig.configWarning.empty())
        std::fprintf(stderr, "config warning: %s\n",
                     engineConfig.configWarning.c_str());

    std::string error;
    ExternalCancellation externalCancel;
    if (!externalCancel.Open(o.cancelEvent, error)) return fail(error);
    std::atomic<bool> cancel{false};
    auto pollCancel = [&]() {
        if (externalCancel.Requested())
            cancel.store(true, std::memory_order_relaxed);
        return cancel.load(std::memory_order_relaxed);
    };

    MappedMidiFile midi;
    if (!midi.Open(o.midi.c_str(), error)) return fail(error);

    MidiStreamDecoder decoder;
    MidiStreamInfo info;
    // Rendering is single-pass: ParallelMidiDecoder streams events while the
    // renderer consumes them, so only --scan-only walks the file up front.
    if (o.scanOnly) {
        MachineStatus(o, "LOADING", "Scanning MIDI file");
        ScanProgressContext scanContext{
            &o, &externalCancel, &cancel,
            std::chrono::steady_clock::now(),
            std::chrono::steady_clock::time_point{}};
        if (!decoder.Scan(midi, o.sampleRate, info, error,
                          (o.machineProgress || !o.cancelEvent.empty())
                              ? ReportScanProgress : nullptr,
                          &scanContext, &cancel)) {
            if (pollCancel()) {
                MachineStatus(o, "CANCELLED", "Cancelled while loading MIDI");
                return 3;
            }
            return fail(error.empty() ? "MIDI scan failed" : error);
        }
    }

    if (o.scanOnly) {
        if (!o.machineProgress) {
            fwprintf(stdout,L"SMF %u, %u tracks, %llu channel events, %llu note-ons, %llu frames (%s at %u Hz)\nPeak 1s: %llu events at %s, %llu note-ons at %s; peak frame: %llu events (%llu note-ons) at frame %llu\nExact-frame repetition: %llu exact duplicates total (peak %llu/frame), %llu channel/key duplicates total (peak %llu/frame), across %llu note-on frames\nWithin uninterrupted note-on runs: %llu exact duplicates (peak %llu/frame), %llu immediately adjacent\n",info.format,info.tracks,info.eventCount,info.noteOnCount,info.totalFrames,FormatTime(double(info.totalFrames)/o.sampleRate).c_str(),o.sampleRate,info.peakEventsPerSecond,FormatTime(double(info.peakEventSecond)).c_str(),info.peakNoteOnsPerSecond,FormatTime(double(info.peakNoteOnSecond)).c_str(),info.peakEventsAtFrame,info.peakNoteOnsAtFrame,info.peakFrame,info.exactDuplicateNoteOnCount,info.peakExactDuplicateNoteOnsAtFrame,info.keyDuplicateNoteOnCount,info.peakKeyDuplicateNoteOnsAtFrame,info.noteOnFrameCount,info.noteRunExactDuplicateCount,info.peakNoteRunExactDuplicatesAtFrame,info.adjacentExactDuplicateNoteOnCount);
        }
        MachineStatus(o, "COMPLETE", "MIDI scan complete");
        return 0;
    }

    if (pollCancel()) {
        MachineStatus(o, "CANCELLED", "Cancelled before rendering");
        return 3;
    }

    MachineStatus(o, "PREPARING", "Loading SoundFont and renderer");
    ParsedEventRing ring(o.eventBufferMB);
    if (!ring.IsValid()) return fail("cannot allocate parsed-event ring");

    std::unique_ptr<OfflineSessionSynth> synth;
    if (o.canonicalSynth)
        synth = std::make_unique<CanonicalOfflineSession>();
    else
        synth = o.shards > 1u ? std::unique_ptr<OfflineSessionSynth>(std::make_unique<ShardedOfflineSession>())
                              : std::make_unique<LegacyOfflineSession>();
    if (!synth->Initialize(o, error)) return fail(error);
    if (pollCancel()) {
        MachineStatus(o, "CANCELLED", "Cancelled while preparing renderer");
        return 3;
    }

    WaveWriter wave;
    if (!wave.Open(o.output.c_str(), o.sampleRate, 0))
        return fail("cannot create output file");

    // Exact length up front (parallel skim of tempo + end ticks, like the
    // player's parser) so progress and ETA are real; if it fails the decoder
    // reports the problem itself.
    uint64_t expectedFrames = 0;
    double measureSeconds = 0.0;
    {
        MachineStatus(o, "LOADING", "Measuring MIDI duration");
        const auto measureStart = std::chrono::steady_clock::now();
        std::string measureError;
        if (!ParallelMidiDecoder::MeasureDuration(midi, o.sampleRate,
                                                  expectedFrames, measureError,
                                                  &cancel))
            expectedFrames = 0;
        measureSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - measureStart).count();
        if (pollCancel()) {
            MachineStatus(o, "CANCELLED", "Cancelled while loading MIDI");
            return 3;
        }
        MachineLoadProgress(o, 1.0, measureSeconds, 0.0, midi.Size(), midi.Size());
    }

    std::atomic<bool> done{false};
    std::atomic<uint64_t> decoded{0};
    std::atomic<uint64_t> bytesDone{0}, bytesTotal{0};
    std::string producerError;
    ProducerContext context{
        &ring, &midi, o.sampleRate, &cancel, &done, &decoded, &producerError};
    ParallelMidiDecoder parallelDecoder;
    parallelDecoder.keepNoteTracks = o.stemTracks;
    std::thread producer([&] {
        // `info` is complete once `done` is published (release/acquire).
        parallelDecoder.Decode(midi, o.sampleRate, RingSink, &context, &cancel,
                               &info, producerError, &bytesDone, &bytesTotal);
        done.store(true, std::memory_order_release);
    });

    std::vector<float> left(o.blockFrames), right(o.blockFrames);
    uint64_t frame = 0;
    uint64_t events = 0;
    uint64_t renderNs = 0;
    uint64_t lastRenderNs = 0;
    uint64_t lastFrame = 0;
    uint32_t peakVoices = 0;
    uint64_t sourceNoteOns = 0, sourceNoteOffs = 0;
    uint64_t sourcePrograms = 0, sourceControls = 0, sourcePitchBends = 0;
    uint64_t controllerCounts[128]{};
    uint32_t activeAtMidiEnd = 0, activeAfterReleaseAll = 0;
    uint64_t tailFrames = 0;
    bool ioOk = true;
    bool renderingTail = false;
    const auto start = std::chrono::steady_clock::now();
    auto lastHuman = start;
    auto lastMachine = std::chrono::steady_clock::time_point{};

    if (humanProgress) {
        fwprintf(stderr,L"%.2f GB MIDI, %ls long (measured in %.1f s), single-pass parallel decode | %hs | ring %llu events\n",double(midi.Size())/1073741824.0,FormatTime(double(expectedFrames)/o.sampleRate).c_str(),measureSeconds,synth->Backend(),ring.Capacity());
        fwprintf(stderr,L"Render config: %u Hz | voices %u | threads %u | master %.3f | limiter %ls [%ls]",o.sampleRate,o.maxVoices,o.renderThreads,o.masterVolume,o.limiterEnabled?L"ON":L"OFF",o.limiterAlgorithm==LimiterAlgorithm::Adaptive?L"Adaptive":L"Classic");
        if(o.limiterEnabled)fwprintf(stderr,L" (threshold %.3f, lookahead %.2f ms, attack %.2f ms, release %.1f ms)",o.limiterThreshold,o.limiterLookaheadMs,o.limiterAttackMs,o.limiterReleaseMs);
        fputws(L"\n",stderr);
        fwprintf(stderr,L"Live tuning: decimation %ls%ls | ghost budget %u | per-key cap %u | steal policy %u | min note %u ms | note merge %ls | fast chunk math %ls\n",engineConfig.decimation?L"ON":L"off",engineConfig.decimationByLevel?L" (by level)":L"",engineConfig.ghostBudget,engineConfig.perKeyVoiceCap,engineConfig.stealPolicy,engineConfig.minNoteMs,engineConfig.noteMerge?L"ON":L"off",engineConfig.fastChunkMath?L"ON":L"off");
        fwprintf(stderr,L"Compiler-side (not yet in offline): cc collapse %ls | bend collapse %ls | note-on collapse threshold %u | block timing %ls | channel limiter %ls | reverb %ls\n",engineConfig.ccCollapse?L"ON":L"off",engineConfig.bendCollapse?L"ON":L"off",engineConfig.noteOnCollapseThreshold,engineConfig.blockTimingMode?L"ON":L"off",engineConfig.channelLimiterEnabled?L"ON":L"off",engineConfig.enableReverb?L"ON":L"off");
    }
    MachineStatus(o, "RENDERING", "Rendering MIDI");

    auto reportRender = [&](bool force) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const double renderedSeconds = static_cast<double>(frame) / o.sampleRate;
        // The total is only exact once the decoder finishes; until then it is
        // extrapolated from how far through the file's bytes the parse is.
        const bool known = done.load(std::memory_order_acquire);
        const uint64_t bt = bytesTotal.load(std::memory_order_relaxed);
        const double byteFraction = bt
            ? (std::min)(1.0, static_cast<double>(bytesDone.load(std::memory_order_relaxed)) /
                               static_cast<double>(bt))
            : 0.0;
        const double totalSeconds = known
            ? static_cast<double>(info.totalFrames) / o.sampleRate
            : (expectedFrames ? static_cast<double>(expectedFrames) / o.sampleRate
               : (byteFraction > 0.01 ? renderedSeconds / byteFraction : 0.0));
        const double speed = elapsed > 0.0 ? renderedSeconds / elapsed : 0.0;
        const double remaining = totalSeconds > renderedSeconds
            ? totalSeconds - renderedSeconds : 0.0;
        const double eta = speed > 0.0 ? remaining / speed : 0.0;
        const double fraction = totalSeconds > 0.0
            ? (std::min)(1.0, renderedSeconds / totalSeconds) : 0.0;

        if (o.machineProgress &&
            (force || now - lastMachine >= std::chrono::milliseconds(100))) {
            MachineRenderProgress(o, fraction, elapsed, renderedSeconds,
                                  totalSeconds, speed, eta, synth->Active(),
                                  peakVoices, synth->Steals(), events,
                                  renderingTail);
            lastMachine = now;
        }

        if (humanProgress &&
            (force || now - lastHuman >= std::chrono::seconds(1))) {
            const double audioDelta = static_cast<double>(frame - lastFrame) /
                                      o.sampleRate;
            const double wallDelta =
                std::chrono::duration<double>(now - lastHuman).count();
            const double recent = wallDelta > 0.0 ? audioDelta / wallDelta : 0.0;
            const double dspMsPerAudioSecond = audioDelta > 0.0
                ? static_cast<double>(renderNs - lastRenderNs) /
                      1000000.0 / audioDelta
                : 0.0;
            fwprintf(stderr,L"\r%s / %s  %6.2f%% | voices %u free %u peak %u steals %u | events %llu/%llu | %.2fx | DSP %.1f ms/audio-s | ETA %s   ",FormatTime(renderedSeconds).c_str(),FormatTime(totalSeconds).c_str(),fraction*100.0,synth->Active(),synth->Free(),peakVoices,synth->Steals(),events,known?info.eventCount:events,recent,dspMsPerAudioSecond,FormatTime(eta).c_str());
            if (std::getenv("SVMS_OFFLINE_PROFILE")) {
                uint64_t k[3]; synth->CollapseCounts(k);
                fwprintf(stderr, L"\n[collapse] coalesced %llu re-excited %llu merged %llu",
                         (unsigned long long)k[0], (unsigned long long)k[1],
                         (unsigned long long)k[2]);
                uint64_t c[3]; synth->PhaseCycles(c);
                const double sum = double(c[0] + c[1] + c[2]) + 1.0;
                fwprintf(stderr, L"\n[profile] plan %.0f%% jobs %.0f%% post %.0f%% ring %llu\n",
                         100.0 * c[0] / sum, 100.0 * c[1] / sum, 100.0 * c[2] / sum,
                         (unsigned long long)ring.Size());
            }
            lastHuman = now;
            lastFrame = frame;
            lastRenderNs = renderNs;
        }
    };

    // With reverb on, keep rendering the tail until it has faded (-100 dB).
    const bool reverbRinging = synth->ReverbActive();
    float lastPeak = 1.0f;
    auto renderTo = [&](uint64_t target) {
        while (frame < target && ioOk && !pollCancel()) {
            const uint32_t n = static_cast<uint32_t>(
                (std::min<uint64_t>)(o.blockFrames, target - frame));
            const auto dspStart = std::chrono::steady_clock::now();
            synth->Render(left.data(), right.data(), n, frame);
            const auto dspEnd = std::chrono::steady_clock::now();
            renderNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    dspEnd - dspStart).count());
            ioOk = wave.Write(left.data(), right.data(), n);
            frame += n;
            if (reverbRinging) {
                float peak = 0.0f;
                for (uint32_t i = 0; i < n; ++i)
                    peak = (std::max)(peak, (std::max)(std::fabs(left[i]), std::fabs(right[i])));
                lastPeak = peak;
            }
            peakVoices = (std::max)(peakVoices, synth->Active());
            reportRender(false);
        }
    };

    // --full-velocity: rewrite before counting/merging so equal-key note-ons fold.
    auto fullVel = [&](PackedMidiEvent& source) {
        if (o.fullVelocity && (source.message & 0xf0u) == 0x90u &&
            (source.message & 0xff0000u) != 0u)
            source.message |= 0x7f0000u;
    };
    auto countSourceEvent = [&](const PackedMidiEvent& source) {
        const uint8_t status = static_cast<uint8_t>(source.message);
        const uint8_t type = status & 0xf0u;
        const uint8_t data1 = static_cast<uint8_t>(source.message >> 8u);
        const uint8_t data2 = static_cast<uint8_t>(source.message >> 16u);
        if (type == 0x90u && data2 != 0u) ++sourceNoteOns;
        else if (type == 0x80u || (type == 0x90u && data2 == 0u)) ++sourceNoteOffs;
        else if (type == 0xc0u) ++sourcePrograms;
        else if (type == 0xe0u) ++sourcePitchBends;
        else if (type == 0xb0u) {
            ++sourceControls;
            ++controllerCounts[data1];
        }
    };
    auto dispatchSourceEvent = [&](const PackedMidiEvent& source) {
        PackedMidiEvent fv = source;
        fullVel(fv);
        countSourceEvent(fv);
        synth->Dispatch(fv.message, fv.outputFrame);
        ++events;
    };

    // Batched path: every event inside a block goes to ONE RenderBlock call at
    // its exact frame (what the realtime driver does). The per-event loop
    // below re-enters RenderBlock for every event, which at Black MIDI
    // densities is a call every few frames.
    const bool batched = std::getenv("SVMS_OFFLINE_PER_EVENT") == nullptr &&
                         synth->RenderEvents(nullptr, 0, nullptr, nullptr, 0, 0);
    const uint64_t maxFrames = o.maxSeconds
        ? uint64_t(o.maxSeconds) * o.sampleRate : ~0ull;
    const bool truncated = o.maxSeconds && maxFrames < info.totalFrames;
    PackedMidiEvent event{};
    if (batched) {
        std::vector<RealtimeEvent> block;
        const uint32_t jitterFrames = [] {
            const char* v = std::getenv("SVMS_OFFLINE_JITTER_FRAMES");
            return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
        }();
        struct JitterEvent {
            uint64_t frame, seq; uint32_t message;
            bool operator<(const JitterEvent& o) const {
                return frame != o.frame ? frame > o.frame : seq > o.seq;
            }
        };
        std::priority_queue<JitterEvent> jitterHeap;
        uint64_t jitterState = 88172645463325252ull;
        while (ioOk && !pollCancel() && frame < maxFrames) {
            uint32_t n = o.blockFrames;
            const uint64_t limit = frame + n;
            block.clear();
            bool exhausted = false;
            if (jitterFrames != 0u) {
                // Diagnostic: delay every event by 0..jitterFrames frames, as a
                // realtime sender's arrival jitter would, then emit in order.
                for (;;) {
                    PackedMidiEvent next{};
                    while (!ring.Peek(next) &&
                           !done.load(std::memory_order_acquire) && !pollCancel())
                        std::this_thread::yield();
                    if (pollCancel() || !ring.Peek(next) ||
                        next.outputFrame >= limit + jitterFrames) break;
                    ring.Pop(event);
                    fullVel(event);
                    countSourceEvent(event);
                    ++events;
                    jitterState ^= jitterState << 13u; jitterState ^= jitterState >> 7u;
                    jitterState ^= jitterState << 17u;
                    jitterHeap.push({event.outputFrame + jitterState % (jitterFrames + 1u),
                                     event.sequence, event.message});
                }
                if (pollCancel()) break;
                while (!jitterHeap.empty() && jitterHeap.top().frame < limit) {
                    const auto top = jitterHeap.top(); jitterHeap.pop();
                    block.push_back({top.frame, top.seq, top.message, false});
                }
                exhausted = jitterHeap.empty() && !ring.Peek(event) &&
                            done.load(std::memory_order_acquire);
            } else for (;;) {
                PackedMidiEvent next{};
                while (!ring.Peek(next) &&
                       !done.load(std::memory_order_acquire) && !pollCancel())
                    std::this_thread::yield();
                if (pollCancel()) break;
                if (!ring.Peek(next)) { exhausted = true; break; }
                if (next.outputFrame >= limit) break;
                ring.Pop(event);
                fullVel(event);
                countSourceEvent(event);
                ++events;
                block.push_back({event.outputFrame, event.sequence,
                                 event.message, false});
            }
            if (pollCancel()) break;
            if (exhausted) {
                // Decoder finished: the total is now exact. Events sitting
                // exactly on the final frame fall outside the last block.
                if (frame >= info.totalFrames) break;
                n = static_cast<uint32_t>(
                    (std::min<uint64_t>)(n, info.totalFrames - frame));
                while (!block.empty() && block.back().targetFrame >= frame + n)
                    block.pop_back();
            }
            const auto dspStart = std::chrono::steady_clock::now();
            synth->RenderEvents(block.data(), block.size(), left.data(),
                                right.data(), n, frame);
            renderNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - dspStart).count());
            ioOk = wave.Write(left.data(), right.data(), n);
            frame += n;
            peakVoices = (std::max)(peakVoices, synth->Active());
            reportRender(false);
        }
    }
    while (!batched && ioOk && !pollCancel()) {
        bool have = false;
        while (!(have = ring.Pop(event)) &&
               !done.load(std::memory_order_acquire) && !pollCancel()) {
            std::this_thread::yield();
        }
        if (!have) break;
        renderTo(event.outputFrame);
        if (pollCancel()) break;
        dispatchSourceEvent(event);

        // Do not advance audio until the producer has exposed every possible
        // equal-frame successor; this preserves global sequence ordering.
        for (;;) {
            PackedMidiEvent next{};
            while (!ring.Peek(next) &&
                   !done.load(std::memory_order_acquire) && !pollCancel()) {
                std::this_thread::yield();
            }
            if (pollCancel() || !ring.Peek(next) ||
                next.outputFrame != event.outputFrame)
                break;
            ring.Pop(event);
            dispatchSourceEvent(event);
        }
    }

    if (ioOk && !pollCancel()) {
        if (!truncated) renderTo(info.totalFrames);
        if (!pollCancel()) {
            activeAtMidiEnd = synth->Active();
            synth->ReleaseAll();
            activeAfterReleaseAll = synth->Active();
            renderingTail = true;
            MachineStatus(o, "TAIL", "Rendering natural release tail");
            const uint64_t tailEnd = frame +
                uint64_t(o.maxTailSeconds) * o.sampleRate;
            while ((synth->Active() || synth->Tails() ||
                    (reverbRinging && lastPeak > 1e-5f)) && frame < tailEnd &&
                   !pollCancel()) {
                renderTo((std::min<uint64_t>)(
                    tailEnd, frame + o.blockFrames));
            }
            tailFrames = frame > info.totalFrames
                ? frame - info.totalFrames : 0u;
        }
    }

    const bool wasCancelled = pollCancel();
    cancel.store(true, std::memory_order_relaxed);
    producer.join();
    if (!wave.Close()) ioOk = false;
    reportRender(true);

    if (humanProgress) {
        fwprintf(stderr,L"\nRendered %s (%llu frames), %llu MIDI events, %u steals. Notes: %llu received, %llu matched; rejects preset=%llu region=%llu invalid=%llu; region-fallback notes=%llu.\n",FormatTime(double(wave.Frames())/o.sampleRate).c_str(),wave.Frames(),events,synth->Steals(),synth->NoteCalls(),synth->MatchedNotes(),synth->MissingPresets(),synth->MissingRegions(),synth->InvalidRegions(),synth->FallbackRegions());
        fwprintf(stderr,L"Differential: note-on=%llu note-off=%llu program=%llu cc=%llu bend=%llu | regions/physical=%llu retired=%llu dropped=%llu peak=%u | active@end=%u active@release=%u tail=%llu frames | CC64=%llu CC120=%llu CC121=%llu CC123=%llu.\n",sourceNoteOns,sourceNoteOffs,sourcePrograms,sourceControls,sourcePitchBends,synth->PhysicalLaunches(),synth->RetiredVoices(),synth->DroppedVoices(),peakVoices,activeAtMidiEnd,activeAfterReleaseAll,tailFrames,controllerCounts[64],controllerCounts[120],controllerCounts[121],controllerCounts[123]);
        const auto canonicalDetails = synth->CanonicalDetails();
        if (canonicalDetails.rejectedEvents != 0) {
            fwprintf(stderr, L"Canonical rejects: invalid=%llu order=%llu capacity=%llu interpreter=%llu.\n",
                canonicalDetails.rejectedInvalidData, canonicalDetails.rejectedOrder,
                canonicalDetails.rejectedCapacity, canonicalDetails.rejectedInterpreter);
        }
    }
    if (wasCancelled) {
        MachineStatus(o, "CANCELLED", "Render cancelled; partial WAV retained");
        return 3;
    }
    if (expectedFrames && !wasCancelled && !truncated && info.totalFrames != expectedFrames)
        std::fprintf(stderr, "warning: measured length %llu frames differs from decoded %llu\n",
                     static_cast<unsigned long long>(expectedFrames),
                     static_cast<unsigned long long>(info.totalFrames));
    if (!truncated && !producerError.empty() && events != info.eventCount)
        return fail("decoder error: " + producerError);
    if (!ioOk) return fail("output write failed");
    MachineStatus(o, "COMPLETE", "Render completed successfully");
    return 0;
}

#if defined(SVMS_OFFLINE_AS_LIBRARY)
// bass.dll build: the KMC/BASSMIDI imitation runs the renderer in process.
extern "C" const SvmsKmcApi* SvmsBassKmcApi() {
    static const SvmsKmcApi api{KmcOpen, KmcRead, KmcActive, KmcClose, KmcChannels};
    return &api;
}
#elif defined(_WIN32)
int wmain(int argc, wchar_t** argv) { return RendererMain(argc, argv); }
#else
int main(int argc, char** argv) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::vector<std::wstring> storage;
    std::vector<wchar_t*> wideArguments;
    storage.reserve(static_cast<size_t>(argc));
    wideArguments.reserve(static_cast<size_t>(argc));
    try {
        for (int index = 0; index < argc; ++index)
            storage.push_back(converter.from_bytes(argv[index]));
    } catch (const std::range_error&) {
        std::fprintf(stderr, "error: command line is not valid UTF-8\n");
        return 2;
    }
    for (std::wstring& value : storage) wideArguments.push_back(&value[0]);
    return RendererMain(argc, wideArguments.data());
}
#endif
