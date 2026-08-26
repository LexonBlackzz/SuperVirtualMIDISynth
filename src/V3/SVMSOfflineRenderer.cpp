#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#endif
#include "SVMSMidiStream.h"
#include "SVMSSoundFont.h"
#include "SVMSVoiceManager.h"
#include "SVMSChannelCache.h"
#include "SVMSRenderScalar.h"
#include "SVMSPostFilter.h"
#include "SVMSLimiter.h"
#include "SVMSEnvelope.h"
#include "SVMSConfig.h"
#include "SVMSStandaloneSynth.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <memory>
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
    RenderBackend backend = RenderBackend::AVX512; // sentinel: automatic
    bool quiet = false;
    bool scanOnly = false;
    bool machineProgress = false;
    std::wstring cancelEvent;
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

void Usage() {
    fputws(L"SuperVirtualMIDISynth V3 offline renderer\n\n"
           L"svms_v3_render <input.mid> <soundfont.sf2> <output.wav> [options]\n\n"
           L"svms_v3_render <input.mid> --scan-only\n\n"
           L"Engine-facing defaults are loaded from the active V3 config; CLI options override them.\n\n"
           L"  --sample-rate N       Output rate (default: V3 config)\n"
           L"  --max-voices N        Voice limit, 1-524288 (default: V3 config)\n"
           L"  --render-threads N     Total voice-render threads, 0-64 (default: V3 config)\n"
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
           L"  --backend auto|scalar|sse2|avx2\n"
           L"  --scan-only           Validate/count without loading SF2 or rendering\n"
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
        else if (arg == L"--machine-progress") o.machineProgress = true;
        else if (arg == L"--cancel-event") {
            const auto p=value(); if (!p || !p[0]) return false;
            o.cancelEvent = p;
        }
        else if (arg == L"--scan-only") o.scanOnly = true;
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
        } else if (arg == L"--backend") {
            const auto p=value(); if (!p) return false; const std::wstring name=p;
            if (name == L"auto") o.backend=RenderBackend::AVX512;
            else if (name == L"scalar") o.backend=RenderBackend::Scalar;
            else if (name == L"sse2") o.backend=RenderBackend::SSE2;
            else if (name == L"avx2") o.backend=RenderBackend::AVX2;
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
        rf64_ = estimatedFrames > (0xffffffffull - 80ull) / 8ull;
        if (rf64_) {
            fwrite("RF64",1,4,file_); U32(0xffffffffu); fwrite("WAVE",1,4,file_);
            fwrite("ds64",1,4,file_); U32(28); ds64Pos_=Tell();
            U64(0); U64(0); U64(0); U32(0);
        } else { fwrite("RIFF",1,4,file_); U32(0); fwrite("WAVE",1,4,file_); }
        fwrite("fmt ",1,4,file_); U32(16); U16(3); U16(2); U32(rate); U32(rate*8); U16(8); U16(32);
        fwrite("data",1,4,file_); dataSizePos_=Tell(); U32(rf64_ ? 0xffffffffu : 0u);
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
        if (rf64_) {
            Seek(ds64Pos_);
            U64(end-8); U64(bytes); U64(frames_);
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
    FILE* file_=nullptr; uint32_t rate_=0; bool rf64_=false;
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
        sf2_.reset(new SF2Data{});
        if (!sf2_load(o.soundfont.c_str(), sf2_.get())) { error="failed to load SoundFont"; return false; }
        sf2_build_regions(sf2_.get());
        if (sf2_->regionOverflow || sf2_->regionCount == 0) {
            error="SoundFont has no usable compiled regions"; return false;
        }
        sampleData_.resize(sf2_->sampleDataFrames);
        for (uint32_t i=0;i<sf2_->sampleDataFrames;++i) sampleData_[i]=sf2_->sampleData[i]/32768.0f;
        prepared_.resize(sf2_->regionCount);
        if (!voices_.Initialize(maxVoices_,rate_)) { error="cannot allocate voice storage"; return false; }
        if (!renderer_.ReserveVoiceCapacity(maxVoices_)) { error="cannot allocate renderer scratch"; return false; }
        if (!renderer_.ConfigureRenderThreads(o.renderThreads, o.blockFrames)) {
            error="cannot initialize render workers"; return false;
        }
        channels_.Reset(); channels_.SetMasterVolume(master_);
        cfg_={master_,1.0f,0.0f,0,false,false,false,false,false,false,
              InterpolationMode::Linear,FilterType::None,PanLaw::ConstantPower,true};
        channels_.RebuildCache(cfg_,static_cast<float>(rate_));
        for (uint32_t i=0;i<sf2_->regionCount;++i) Prepare(sf2_->regions[i],prepared_[i]);
        for (uint8_t ch=0;ch<kChannelCount;++ch) RefreshPreset(ch);
        if (o.backend != RenderBackend::AVX512 && !renderer_.SetRenderBackend(o.backend)) {
            error="requested render backend is unsupported on this CPU"; return false;
        }
        for (float& r:bendRatio_) r=1.0f;
        postHighPass_.Initialize(rate_);
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
        renderer_.RenderBlock(voices_,channels_,sampleData_.data(),uint32_t(sampleData_.size()),l,r,n,cfg_,nullptr,0,true,frame);
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
    const char* Backend() const { return renderer_.GetRenderBackendName(); }
private:
    struct RegionCacheEntry { uint32_t tag=UINT32_MAX;uint16_t count=0;uint16_t reserved=0;uint32_t indices[8]{}; };
    uint32_t ResolveRegions(uint32_t preset,uint8_t note,uint8_t velocity,const SFSampleRegion** out,uint32_t capacity){const uint32_t tag=(preset<<14u)|(uint32_t(note)<<7u)|velocity;uint32_t hash=tag;hash^=hash>>16u;hash*=0x7feb352du;hash^=hash>>15u;RegionCacheEntry& cached=regionCache_[hash&4095u];if(cached.tag==tag&&cached.count<=8u){const uint32_t copied=(std::min)(uint32_t(cached.count),capacity);for(uint32_t i=0;i<copied;++i)out[i]=&sf2_->regions[cached.indices[i]];return cached.count;}const uint32_t count=sf2_find_regions(sf2_.get(),preset,note,velocity,out,capacity);if(count<=8u&&count<=capacity){cached.tag=tag;cached.count=uint16_t(count);for(uint32_t i=0;i<count;++i)cached.indices[i]=uint32_t(out[i]-sf2_->regions);}return count;}
    void Prepare(const SFSampleRegion& rg, PreparedRegion& p) {
        p={}; if(!sf2_validate_region(sf2_.get(),&rg)||rg.sampleIndex>=sf2_->sampleCount)return;
        const SF2Sample& s=sf2_->samples[rg.sampleIndex]; const int root=rg.rootKey>=0?rg.rootKey:s.originalPitch;
        p.bendScale=(rg.scaleTuning?rg.scaleTuning:100)/100.0f;
        const float ratio=float(s.sampleRate?s.sampleRate:44100)/float(rate_);
        for(uint32_t n=0;n<kNoteCount;++n) p.baseStep[n]=ratio*powf(2.0f,((float(n)+rg.coarseTune+rg.fineTune/100.0f-root)*p.bendScale)/12.0f);
        p.attenuation=rg.initialAttenuation>0?InitialAttenuationToGain(float(rg.initialAttenuation)):1.0f;
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
    void NoteOn(uint8_t ch,uint8_t note,uint8_t vel) {
        ++noteCalls_;if(note>=128)return;channels_.NoteOn(ch,note,vel);uint32_t pi=channels_.GetSelectedPreset(ch);if(pi>=sf2_->presetCount){if(!Resolve(ch,pi)){++missingPresets_;return;}channels_.SetSelectedPreset(ch,uint16_t(pi));}
        const SFSampleRegion* regions[512];uint32_t count=ResolveRegions(pi,note,vel,regions,512);
        if(!count||count>512){const uint16_t fb=sf2_->fallbackPresetIndex;
            if(fb<sf2_->presetCount&&fb!=pi)count=ResolveRegions(fb,note,vel,regions,512);
            if(!count||count>512){++missingRegions_;return;}++fallbackRegions_;}
        for(uint32_t i=0;i<count;++i){const uint32_t ri=uint32_t(regions[i]-sf2_->regions);if(ri>=prepared_.size()||!prepared_[ri].valid){++invalidRegions_;return;}}
        if(playIndex_==0||playIndex_>=UINT32_MAX-1)playIndex_=1;const uint32_t generation=playIndex_++; VoiceHandle handles[512];uint32_t made=0;
        for(;made<count;++made){handles[made]=voices_.AllocateVoiceOrSteal(ch,note,vel,nullptr,count==1);if(handles[made]==kInvalidVoice){for(uint32_t j=0;j<made;++j)voices_.RetireVoice(handles[j]);return;}}
        const float velocityGain=float(vel)*float(vel)/(127.0f*127.0f);
        const float bend=channels_.GetPitchBendSemitones(ch);
        for(uint32_t i=0;i<count;++i){const auto& rg=*regions[i];const uint32_t ri=uint32_t(regions[i]-sf2_->regions);const auto& p=prepared_[ri];const float br=p.bendScale==1.0f?bendRatio_[ch]:powf(2.0f,bend*p.bendScale/12.0f);const float gain=velocityGain*p.attenuation;
            VoiceConfiguration c{};c.sampleStart=uint32_t(rg.startOffset);c.sampleEnd=uint32_t(rg.endOffset);c.loopStart=uint32_t(rg.loopStartOffset);c.loopEnd=uint32_t(rg.loopEndOffset);c.loopMode=rg.loopMode;c.playIndex=generation;c.delaySamples=p.delay;c.holdSamples=p.hold;c.attackSamples=p.attack;c.decaySamples=p.decay;c.releaseSamples=p.release;c.phaseStep=p.baseStep[note]*br;c.basePhaseStep=p.baseStep[note];c.pitchBendScale=p.bendScale;c.initialGain=gain;c.sustainLevel=p.sustain;c.attackGainStep=p.attack?gain/p.attack:0;c.decaySlope=p.decaySlope;c.releaseDecay=p.releaseDecay;c.gainLeft=p.panL;c.gainRight=p.panR;c.presetIndex=uint16_t(pi);c.regionIndex=uint16_t(ri);c.sampleBacked=1;voices_.ConfigureVoice(handles[i],c,channels_.GetParams()[ch],count==1);}
        ++notes_;
    }
    void NoteOff(uint8_t ch,uint8_t note){const bool sustain=channels_.IsSustainActive(ch);channels_.NoteOff(ch,note);const uint32_t p=voices_.FindOldestPlayIndex(ch,note);if(p!=UINT32_MAX)voices_.NoteOffPlayIndex(ch,note,p,sustain,0);}
    void Control(uint8_t ch,uint8_t cc,uint8_t value){const bool sustain=channels_.IsSustainActive(ch);channels_.ControlChange(ch,cc,value);if(cc==0||cc==32)RefreshPreset(ch);if(cc==64&&value<64){voices_.ForEachChannelActive(ch,[&](VoiceHandle h){if(voices_.v.heldBySustain[h]){voices_.v.heldBySustain[h]=0;voices_.StartRelease(h);}});}
        if(cc==120)voices_.SilenceChannelImmediate(ch);else if(cc==123)voices_.ReleaseChannel(ch,0);else if(cc==121&&sustain){voices_.ForEachChannelActive(ch,[&](VoiceHandle h){if(voices_.v.heldBySustain[h]){voices_.v.heldBySustain[h]=0;voices_.StartRelease(h);}});}
        if(cc==121)Bend(ch,0,64);if(cc==7||cc==10||cc==11||cc==64||cc==121){channels_.RebuildChannel(ch,cfg_,float(rate_));if(cc==7||cc==10||cc==11||cc==121)voices_.RefreshMixGainsForChannel(ch,channels_.GetParams()[ch]);}}
    void Program(uint8_t ch,uint8_t p){const uint8_t old=channels_.GetProgram(ch);channels_.ProgramChange(ch,p);uint32_t pi;if(Resolve(ch,pi))channels_.SetSelectedPreset(ch,uint16_t(pi));else channels_.ProgramChange(ch,old);}
    void Bend(uint8_t ch,uint8_t lo,uint8_t hi){channels_.PitchBend(ch,int16_t((hi<<7)|lo));const float semis=channels_.GetPitchBendSemitones(ch);const float common=powf(2.0f,semis/12.0f);bendRatio_[ch]=common;voices_.ForEachChannelActive(ch,[&](VoiceHandle v){const float scale=voices_.v.pitchBendScales[v];voices_.v.phaseIncs[v]=voices_.v.basePhaseIncs[v]*(scale==1?common:powf(2.0f,semis*scale/12.0f));});}
    uint32_t rate_=0,maxVoices_=0,playIndex_=0;float master_=0,bendRatio_[16]{};uint64_t notes_=0,noteCalls_=0,missingPresets_=0,missingRegions_=0,invalidRegions_=0,fallbackRegions_=0;
    std::unique_ptr<SF2Data> sf2_;std::vector<float> sampleData_;std::vector<PreparedRegion> prepared_;
    VoiceManager voices_;ChannelCache channels_;RenderScalar renderer_;RuntimeConfigSnapshot cfg_{};PostHighPass3Hz postHighPass_{};LimiterRouterState limiter_{};RegionCacheEntry regionCache_[4096]{};
};

#endif

struct ProducerContext { ParsedEventRing* ring; const MappedMidiFile* file; uint32_t rate; std::atomic<bool>* cancel; std::atomic<bool>* done; std::atomic<uint64_t>* decoded; std::string* error; };
bool RingSink(const PackedMidiEvent& e,void* p){auto& c=*static_cast<ProducerContext*>(p);if(!c.ring->Push(e,*c.cancel))return false;c.decoded->fetch_add(1,std::memory_order_relaxed);return true;}

} // namespace
} // namespace svms

int RendererMain(int argc, wchar_t** argv) {
    using namespace svms;
#if defined(_WIN32)
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
    if (!ParseOptions(argc, argv, o)) {
        Usage();
        return 2;
    }

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

    MachineStatus(o, "LOADING", "Scanning MIDI file");
    MidiStreamDecoder decoder;
    MidiStreamInfo info;
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

    StandaloneSynthConfig synthConfig{};
    synthConfig.soundfont = o.soundfont;
    synthConfig.sampleRate = o.sampleRate;
    synthConfig.maxVoices = o.maxVoices;
    synthConfig.renderThreads = o.renderThreads;
    synthConfig.maxBlockFrames = o.blockFrames;
    synthConfig.masterVolume = o.masterVolume;
    synthConfig.limiterEnabled = o.limiterEnabled;
    synthConfig.limiterAlgorithm = o.limiterAlgorithm;
    synthConfig.limiterThreshold = o.limiterThreshold;
    synthConfig.limiterLookaheadMs = o.limiterLookaheadMs;
    synthConfig.limiterAttackMs = o.limiterAttackMs;
    synthConfig.limiterReleaseMs = o.limiterReleaseMs;
    synthConfig.backend = o.backend;
    auto synth = std::make_unique<StandaloneSynth>();
    if (!synth->Initialize(synthConfig, error)) return fail(error);
    if (pollCancel()) {
        MachineStatus(o, "CANCELLED", "Cancelled while preparing renderer");
        return 3;
    }

    WaveWriter wave;
    if (!wave.Open(o.output.c_str(), o.sampleRate,
                   info.totalFrames + uint64_t(o.maxTailSeconds) * o.sampleRate))
        return fail("cannot create output file");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> decoded{0};
    std::string producerError;
    ProducerContext context{
        &ring, &midi, o.sampleRate, &cancel, &done, &decoded, &producerError};
    std::thread producer([&] {
        MidiStreamInfo ignored;
        decoder.Decode(midi, o.sampleRate, RingSink, &context, &cancel,
                       &ignored, producerError);
        done.store(true, std::memory_order_release);
    });

    std::vector<float> left(o.blockFrames), right(o.blockFrames);
    uint64_t frame = 0;
    uint64_t events = 0;
    uint64_t renderNs = 0;
    uint64_t lastRenderNs = 0;
    uint64_t lastFrame = 0;
    uint32_t peakVoices = 0;
    bool ioOk = true;
    bool renderingTail = false;
    const auto start = std::chrono::steady_clock::now();
    auto lastHuman = start;
    auto lastMachine = std::chrono::steady_clock::time_point{};

    if (humanProgress) {
        fwprintf(stderr,L"SMF %u, %u tracks, %llu channel events (%llu note-ons) | %.2f min | %hs | ring %llu events\n",info.format,info.tracks,info.eventCount,info.noteOnCount,double(info.totalFrames)/o.sampleRate/60.0,synth->Backend(),ring.Capacity());
        fwprintf(stderr,L"Render config: %u Hz | voices %u | threads %u | master %.3f | limiter %ls [%ls]",o.sampleRate,o.maxVoices,o.renderThreads,o.masterVolume,o.limiterEnabled?L"ON":L"OFF",o.limiterAlgorithm==LimiterAlgorithm::Adaptive?L"Adaptive":L"Classic");
        if(o.limiterEnabled)fwprintf(stderr,L" (threshold %.3f, lookahead %.2f ms, attack %.2f ms, release %.1f ms)",o.limiterThreshold,o.limiterLookaheadMs,o.limiterAttackMs,o.limiterReleaseMs);
        fputws(L"\n",stderr);
    }
    MachineStatus(o, "RENDERING", "Rendering MIDI");

    auto reportRender = [&](bool force) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const double renderedSeconds = static_cast<double>(frame) / o.sampleRate;
        const double totalSeconds = static_cast<double>(info.totalFrames) / o.sampleRate;
        const double speed = elapsed > 0.0 ? renderedSeconds / elapsed : 0.0;
        const double remaining = static_cast<double>(
            info.totalFrames > frame ? info.totalFrames - frame : 0u) / o.sampleRate;
        const double eta = speed > 0.0 ? remaining / speed : 0.0;
        const double fraction = info.totalFrames
            ? (std::min)(1.0, static_cast<double>(frame) /
                               static_cast<double>(info.totalFrames))
            : 1.0;

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
            fwprintf(stderr,L"\r%s / %s  %6.2f%% | voices %u free %u peak %u steals %u | events %llu/%llu | %.2fx | DSP %.1f ms/audio-s | ETA %s   ",FormatTime(renderedSeconds).c_str(),FormatTime(totalSeconds).c_str(),fraction*100.0,synth->Active(),synth->Free(),peakVoices,synth->Steals(),events,info.eventCount,recent,dspMsPerAudioSecond,FormatTime(eta).c_str());
            lastHuman = now;
            lastFrame = frame;
            lastRenderNs = renderNs;
        }
    };

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
            peakVoices = (std::max)(peakVoices, synth->Active());
            reportRender(false);
        }
    };

    PackedMidiEvent event{};
    while (ioOk && !pollCancel()) {
        bool have = false;
        while (!(have = ring.Pop(event)) &&
               !done.load(std::memory_order_acquire) && !pollCancel()) {
            std::this_thread::yield();
        }
        if (!have) break;
        renderTo(event.outputFrame);
        if (pollCancel()) break;
        synth->Dispatch(event.message, event.outputFrame);
        ++events;

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
            synth->Dispatch(event.message, event.outputFrame);
            ++events;
        }
    }

    if (ioOk && !pollCancel()) {
        renderTo(info.totalFrames);
        if (!pollCancel()) {
            synth->ReleaseAll();
            renderingTail = true;
            MachineStatus(o, "TAIL", "Rendering natural release tail");
            const uint64_t tailEnd = frame +
                uint64_t(o.maxTailSeconds) * o.sampleRate;
            while ((synth->Active() || synth->Tails()) && frame < tailEnd &&
                   !pollCancel()) {
                renderTo((std::min<uint64_t>)(
                    tailEnd, frame + o.blockFrames));
            }
        }
    }

    const bool wasCancelled = pollCancel();
    cancel.store(true, std::memory_order_relaxed);
    producer.join();
    if (!wave.Close()) ioOk = false;
    reportRender(true);

    if (humanProgress) {
        fwprintf(stderr,L"\nRendered %s (%llu frames), %llu MIDI events, %u steals. Notes: %llu received, %llu matched; rejects preset=%llu region=%llu invalid=%llu; region-fallback notes=%llu.\n",FormatTime(double(wave.Frames())/o.sampleRate).c_str(),wave.Frames(),events,synth->Steals(),synth->NoteCalls(),synth->MatchedNotes(),synth->MissingPresets(),synth->MissingRegions(),synth->InvalidRegions(),synth->FallbackRegions());
    }
    if (wasCancelled) {
        MachineStatus(o, "CANCELLED", "Render cancelled; partial WAV retained");
        return 3;
    }
    if (!producerError.empty() && events != info.eventCount)
        return fail("decoder error: " + producerError);
    if (!ioOk) return fail("output write failed");
    MachineStatus(o, "COMPLETE", "Render completed successfully");
    return 0;
}

#if defined(_WIN32)
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
