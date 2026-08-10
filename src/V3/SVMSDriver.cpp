#include <windows.h>
#include <mmreg.h>
#include <mmeapi.h>
#include <timeapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <algorithm>

#include "SVMSAudioOutput.h"
#include "SVMSVoiceManager.h"
#include "SVMSChannelCache.h"
#include "SVMSRenderScalar.h"
#include "SVMSSoundFont.h"
#include "SVMSConfig.h"
#include "SVMSMPSCQueue.h"
#include "SVMSEventScheduler.h"
#include "SVMSFrameClock.h"
#include "SVMSDiagWindow.h"

// ── Logging ────────────────────────────────────────────────────────────

#define SVMS_VERBOSE_LOG 0

static FILE* g_logFile = nullptr;
static CRITICAL_SECTION g_logLock;
static bool g_logInit = false;

static void LogInit() {
    if (g_logInit) return;
    InitializeCriticalSection(&g_logLock);
    g_logInit = true;

#if SVMS_VERBOSE_LOG
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(logPath, MAX_PATH, "svms_v3.log");
    g_logFile = fopen(logPath, "w");
    if (g_logFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_logFile, "=== SVMS V3 Log [%04d-%02d-%02d %02d:%02d:%02d] ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(g_logFile);
    }
#endif
}

static void Log(const char* fmt, ...) {
#if !SVMS_VERBOSE_LOG
    (void)fmt;
#endif
#if SVMS_VERBOSE_LOG
    if (!g_logInit) LogInit();
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    OutputDebugStringA(buf);
    EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        fputs(buf, g_logFile);
        fflush(g_logFile);
    }
    LeaveCriticalSection(&g_logLock);
#endif
}

#define LOG(fmt, ...) Log("[SVMS] " fmt "\n", ##__VA_ARGS__)

namespace svms {

static constexpr uint32_t kInternalResetMessage = 0xFF000001u;

// WaitOnAddress is available on Windows 8 and newer, but some older Windows
// SDK import libraries do not expose the API-set forwarding symbols. Resolve
// it once during engine initialization so the audio callback never invokes
// the loader. The compatibility fallback merely yields; cancellation is still
// observed on every retry.
using WaitOnAddressProc = BOOL (WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressAllProc = VOID (WINAPI*)(PVOID);
static WaitOnAddressProc g_waitOnAddress = nullptr;
static WakeByAddressAllProc g_wakeByAddressAll = nullptr;

static void ResolveAddressWaitApi() noexcept {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return;
    g_waitOnAddress = reinterpret_cast<WaitOnAddressProc>(
        GetProcAddress(kernel32, "WaitOnAddress"));
    g_wakeByAddressAll = reinterpret_cast<WakeByAddressAllProc>(
        GetProcAddress(kernel32, "WakeByAddressAll"));
}

static void WaitForAddressChange(std::atomic<uint32_t>& address,
                                 uint32_t observed) noexcept {
    if (g_waitOnAddress) {
        g_waitOnAddress(reinterpret_cast<volatile VOID*>(&address),
                        &observed, sizeof(observed), 50);
    } else {
        SwitchToThread();
    }
}

static void WakeAddressWaiters(std::atomic<uint32_t>& address) noexcept {
    if (g_wakeByAddressAll) {
        g_wakeByAddressAll(reinterpret_cast<PVOID>(&address));
    }
}

static void PublishTerminationFence(std::atomic<uint64_t>& destination,
                                    uint32_t sequence) noexcept {
    const uint64_t encoded = static_cast<uint64_t>(sequence) + 1u;
    uint64_t observed = destination.load(std::memory_order_relaxed);
    for (;;) {
        if (observed != 0u) {
            const uint32_t current = static_cast<uint32_t>(observed - 1u);
            if (!SequenceAtOrBefore(current, sequence)) return;
        }
        if (destination.compare_exchange_weak(observed, encoded,
                std::memory_order_release, std::memory_order_relaxed)) {
            return;
        }
    }
}

static bool FenceSuppresses(uint32_t sequence, uint64_t encodedFence) noexcept {
    return encodedFence != 0u &&
           SequenceAtOrBefore(sequence, static_cast<uint32_t>(encodedFence - 1u));
}

// ── Velocity→gain LUT (SnappySynth pattern) ────────────────────────────
// Maps MIDI velocity 0-127 to squared gain for natural loudness perception.
// Linear mapping (vel/127) sounds thin; squared gives proper acoustic feel.
static const float g_velGainLUT[128] = {
    0.000000f, 0.000062f, 0.000248f, 0.000558f, 0.000992f, 0.001550f, 0.002232f, 0.003038f,
    0.003968f, 0.005022f, 0.006200f, 0.007501f, 0.008927f, 0.010476f, 0.012150f, 0.013947f,
    0.015868f, 0.017913f, 0.020082f, 0.022374f, 0.024791f, 0.027331f, 0.029996f, 0.032784f,
    0.035696f, 0.038732f, 0.041892f, 0.045175f, 0.048583f, 0.052114f, 0.055770f, 0.059549f,
    0.063452f, 0.067478f, 0.071629f, 0.075903f, 0.080302f, 0.084824f, 0.089470f, 0.094240f,
    0.099133f, 0.104151f, 0.109292f, 0.114558f, 0.119947f, 0.125460f, 0.131097f, 0.136857f,
    0.142742f, 0.148750f, 0.154883f, 0.161139f, 0.167519f, 0.174023f, 0.180650f, 0.187402f,
    0.194277f, 0.201277f, 0.208400f, 0.215647f, 0.223018f, 0.230512f, 0.238131f, 0.245873f,
    0.253740f, 0.261730f, 0.269844f, 0.278082f, 0.286444f, 0.294929f, 0.303539f, 0.312272f,
    0.321130f, 0.330111f, 0.339216f, 0.348445f, 0.357798f, 0.367275f, 0.376875f, 0.386599f,
    0.396448f, 0.406419f, 0.416515f, 0.426735f, 0.437078f, 0.447546f, 0.458137f, 0.468852f,
    0.479691f, 0.490654f, 0.501740f, 0.512951f, 0.524285f, 0.535743f, 0.547325f, 0.559030f,
    0.570860f, 0.582813f, 0.594890f, 0.607091f, 0.619416f, 0.631864f, 0.644437f, 0.657133f,
    0.669953f, 0.682897f, 0.695965f, 0.709156f, 0.722471f, 0.735910f, 0.749473f, 0.763160f,
    0.776970f, 0.790904f, 0.804962f, 0.819144f, 0.833449f, 0.847879f, 0.862432f, 0.877109f,
    0.891909f, 0.906834f, 0.921882f, 0.937054f, 0.952350f, 0.967770f, 0.983313f, 1.000000f,
};

struct LimiterState {
    static constexpr uint32_t kDelayFrames = 128;
    // The delay line provides look-ahead.  React quickly enough that a dense
    // Black MIDI chord does not spend the whole look-ahead window clipping.
    static constexpr float kAttackCoeff = 0.25f;
    static constexpr float kReleaseCoeff = 0.001f;
    static constexpr float kThreshold = 0.9f;
    static constexpr float kEpsilon = 0.0001f;
    static constexpr float kSoftClipGain = 1.0f;

    float delayBuffer[kDelayFrames * 2];
    uint32_t delayWritePos;
    float envelope;
    bool enabled;

    void Reset() {
        std::memset(delayBuffer, 0, sizeof(delayBuffer));
        delayWritePos = 0;
        envelope = 1.0f;
        enabled = true;
    }

    void Process(float* interleaved, uint32_t numFrames, uint32_t channels) {
        if (!enabled) return;

        for (uint32_t f = 0; f < numFrames; ++f) {
            float inL = interleaved[f * channels];
            float inR = (channels > 1) ? interleaved[f * channels + 1] : inL;

            float absL = inL > 0 ? inL : -inL;
            float absR = inR > 0 ? inR : -inR;
            float peak = absL > absR ? absL : absR;

            if (peak > envelope) {
                envelope += kAttackCoeff * (peak - envelope);
            } else {
                envelope += kReleaseCoeff * (peak - envelope);
            }

            float gain = 1.0f;
            if (envelope > kThreshold) {
                gain = kThreshold / envelope;
            }
            gain *= kSoftClipGain;

            uint32_t dw = delayWritePos * channels;
            float dL = delayBuffer[dw];
            float dR = delayBuffer[dw + 1];

            delayBuffer[dw] = inL;
            delayBuffer[dw + 1] = inR;

            dL *= gain;
            dR *= gain;

            // Soft-knee saturation is only used above the limiter knee; it
            // avoids the former hard clip that made dense reference renders
            // visibly flatten while still guaranteeing bounded output.
            auto softLimit = [](float x) {
                const float ax = std::fabs(x);
                if (ax <= 0.9f) return x;
                const float compressed = 0.9f + 0.1f * std::tanh((ax - 0.9f) * 10.0f);
                return x < 0.0f ? -compressed : compressed;
            };
            dL = softLimit(dL);
            dR = softLimit(dR);

            interleaved[f * channels] = dL;
            if (channels > 1) interleaved[f * channels + 1] = dR;

            delayWritePos = (delayWritePos + 1) % kDelayFrames;
        }
    }
};

// Allocation-free rolling callback histogram. One-percent bins are precise
// enough for the diagnostic/acceptance thresholds and make percentile reads a
// bounded 201-bin scan instead of sorting on the audio thread.
struct CallbackTimingWindow {
    static constexpr uint32_t kWindowSize = 1024;
    static constexpr uint32_t kBinCount = 201; // 0..199%, 200 = 200%+

    uint16_t samples[kWindowSize]{};
    uint16_t bins[kBinCount]{};
    uint32_t cursor = 0;
    uint32_t count = 0;
    uint64_t overBudgetCallbacks = 0;
    uint32_t consecutiveOverBudget = 0;
    uint32_t maxConsecutiveOverBudget = 0;

    void Reset() noexcept { *this = CallbackTimingWindow{}; }

    void Observe(float percent) noexcept {
        uint32_t bin = percent > 0.0f ? static_cast<uint32_t>(percent + 0.5f) : 0u;
        if (bin >= kBinCount) bin = kBinCount - 1u;
        if (count == kWindowSize) {
            --bins[samples[cursor]];
        } else {
            ++count;
        }
        samples[cursor] = static_cast<uint16_t>(bin);
        ++bins[bin];
        cursor = (cursor + 1u) & (kWindowSize - 1u);

        if (percent > 100.0f) {
            ++overBudgetCallbacks;
            ++consecutiveOverBudget;
            maxConsecutiveOverBudget = (std::max)(maxConsecutiveOverBudget,
                                                   consecutiveOverBudget);
        } else {
            consecutiveOverBudget = 0;
        }
    }

    float Percentile(uint32_t numerator, uint32_t denominator) const noexcept {
        if (count == 0u) return 0.0f;
        const uint32_t rank = (count * numerator + denominator - 1u) / denominator;
        uint32_t accumulated = 0u;
        for (uint32_t bin = 0; bin < kBinCount; ++bin) {
            accumulated += bins[bin];
            if (accumulated >= rank) return static_cast<float>(bin);
        }
        return static_cast<float>(kBinCount - 1u);
    }
};

class Driver {
public:
    static Driver& Instance();

    bool Initialize();
    void Shutdown();
    bool LoadSoundFont(const wchar_t* path);
    bool LoadConfiguredSoundFont();
    void StartAudio();
    void ResetAllVoices();
    bool IsInitialized() const;
    void CopyDebugInfo(DriverDebugInfo& out) const;

    void SubmitShortMsg(uint32_t msg);

    bool initialized;
    uint32_t sampleRate;
    uint32_t bufferFrames;

private:
    Driver();
    ~Driver();

    static void RenderCallback(float* output, uint32_t numFrames, void* userData);

    // EventDispatcher callback — invoked by RenderScalar at each event's
    // exact integer output frame during RenderBlock.
    static void DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                     void* userData);

    void HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset);
    void HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value,
                             uint32_t blockOffset);
    void HandleProgramChange(uint8_t channel, uint8_t program);
    void HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb);

    PriorityEventIngress<TimestampedMidiEvent> midiIngress_;
    EventScheduler eventScheduler_;
    EventOverflowMode overflowMode_;
    bool correctnessMode_;
    uint32_t highPriorityVelocity_;
    uint32_t shedStartPercent_;
    uint32_t maxEventsPerBlock_;
    bool diagnosticsEnabled_;
    bool diagnosticsWindow_;
    bool diagnosticsDebugOutput_;
    std::atomic<uint32_t> nextEventSequence_;
    // A state event can overtake older note-ons held in another priority
    // lane.  These producer-published fences prevent those stale note-ons
    // from sounding after a reset or per-channel termination controller.
    std::atomic<uint64_t> globalTerminationFence_;
    std::atomic<uint64_t> channelTerminationFence_[kChannelCount];
    std::atomic<bool> cancelProducers_;
    std::atomic<uint32_t> producerWakeEpoch_;
    std::atomic<uint32_t> scheduledSizePublished_;
    std::atomic<uint64_t> submittedAtomic_;
    std::atomic<uint64_t> acceptedAtomic_;
    std::atomic<uint64_t> shedAtomic_;
    std::atomic<uint64_t> cancelledAtomic_;
    std::atomic<uint32_t> currentVelocityCutoffAtomic_;
    std::atomic<uint64_t> shedByVelocityAtomic_[128];
    EventTelemetry telemetry_;
    LiveSF2Telemetry sf2Telemetry_;
    DriverDebugInfo debugSnapshots_[2];
    std::atomic<uint32_t> debugSnapshotIndex_;
    uint64_t callbackCount_;
    CallbackTimingWindow callbackTiming_;

    AudioOutput* audioOutput;
    VoiceManager* voiceManager;
    ChannelCache* channelCache;
    RenderScalar* renderScalar;
    SF2Data* soundFontData;
    RuntimeConfigSnapshot* configSnapshot;
    float* sampleDataStore;
    SF2Sample* samplesStore;
    uint32_t sampleStoreCount;
    uint32_t sampleDataFrames;
    uint64_t qpcFreq;

    float* leftBuffer;
    float* rightBuffer;
    uint32_t bufferCapacity;
    LimiterState limiter;

    // Per-block render-event queue.  Allocated as a flat array so the
    // whole working set stays in L1/L2/L3 during dispatch.  Size is
    // kEventBufferCapacity (see SVMSTypes.h).  65536 entries = 512 KB.
    svms::RenderEvent* eventBuffer;

    // Persistent audio-thread-only overflow queue.  Events whose
    // sampleOffset lands beyond the current block (sampleOffset >=
    // numFrames) are kept here — NOT pushed back into the lock-free SPSC
    // queue — with their sampleOffset re-based to the next block's frame
    // of reference at block completion.  This is what lets a future event
    // roll over smoothly across block boundaries instead of snapping to
    // sample 0 of the next callback (BUG1: 100Hz buffer-grid buzzing).
    // Absolute QPC/output-frame epoch. Conversion is always made from this
    // fixed epoch, so callback rounding cannot accumulate clock drift.
    uint64_t virtualRenderClockQPC;
    int64_t virtualRenderSample_;
    bool clockInitialized;
    uint32_t nextPlayIndex_;
    EngineConfig engineConfig_;

    CRITICAL_SECTION cs;
};

static Driver* s_instance = nullptr;

// Resolve a channel's active preset using the same bank/program rules as TSF:
// normal channels use their selected bank and program; MIDI channel 10 first
// searches the percussion bank (128 | bank), then the standard percussion
// fallbacks.  The caller only commits a successful result, so an invalid
// program change leaves the previously selected preset untouched.
static bool ResolveChannelPreset(const SF2Data* data, const ChannelCache& cache,
                                 uint8_t channel, uint32_t* outPresetIndex) {
    if (!data || !outPresetIndex || channel >= kChannelCount) return false;
    return sf2_resolve_preset(data, cache.GetBank(channel), cache.GetProgram(channel),
                              channel == 9, outPresetIndex);
}

Driver& Driver::Instance() {
    if (!s_instance) {
        s_instance = new Driver();
    }
    return *s_instance;
}

Driver::Driver()
    : initialized(false), sampleRate(44100), bufferFrames(512),
      audioOutput(nullptr), voiceManager(nullptr), channelCache(nullptr),
      renderScalar(nullptr), soundFontData(nullptr), configSnapshot(nullptr),
      sampleDataStore(nullptr), samplesStore(nullptr), sampleStoreCount(0), sampleDataFrames(0),
      qpcFreq(1),
      leftBuffer(nullptr), rightBuffer(nullptr), bufferCapacity(0),
      eventBuffer(nullptr),
      eventScheduler_(kDefaultEventRingCapacity),
      overflowMode_(EventOverflowMode::PriorityVelocity), correctnessMode_(false),
      highPriorityVelocity_(96), shedStartPercent_(70), maxEventsPerBlock_(65536),
      diagnosticsEnabled_(false), diagnosticsWindow_(false), diagnosticsDebugOutput_(false),
      nextEventSequence_(0), globalTerminationFence_(0), cancelProducers_(false),
      producerWakeEpoch_(0),
      scheduledSizePublished_(0), submittedAtomic_(0), acceptedAtomic_(0), shedAtomic_(0),
      cancelledAtomic_(0), currentVelocityCutoffAtomic_(1),
      telemetry_{}, debugSnapshotIndex_(0), callbackCount_(0),
      virtualRenderClockQPC(0),
      virtualRenderSample_(0), clockInitialized(false), nextPlayIndex_(1) {
    for (auto& counter : shedByVelocityAtomic_) counter.store(0, std::memory_order_relaxed);
    for (auto& fence : channelTerminationFence_) fence.store(0, std::memory_order_relaxed);
    limiter.Reset();
    eventBuffer = static_cast<svms::RenderEvent*>(
        _aligned_malloc(sizeof(svms::RenderEvent) * kEventBufferCapacity, 64));
    InitializeCriticalSection(&cs);
}

Driver::~Driver() {
    Shutdown();
    if (eventBuffer) { _aligned_free(eventBuffer); eventBuffer = nullptr; }
    DeleteCriticalSection(&cs);
}

bool Driver::Initialize() {
    if (initialized) return true;

    ResolveAddressWaitApi();
    midiIngress_.DrainAvailable();
    eventScheduler_.Reset();
    globalTerminationFence_.store(0, std::memory_order_relaxed);
    for (auto& fence : channelTerminationFence_) fence.store(0, std::memory_order_relaxed);
    virtualRenderClockQPC = 0;
    virtualRenderSample_ = 0;
    clockInitialized = false;
    callbackTiming_.Reset();

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFreq = freq.QuadPart;

    EngineConfig cfg = EngineConfig::Load();
    if (!cfg.configWarning.empty()) {
        std::string warning = "[SVMS] configuration warning: " +
                              cfg.configWarning + "\n";
        OutputDebugStringA(warning.c_str());
    }
    if (!cfg.Validate()) { LOG("EngineConfig validation failed"); return false; }

    engineConfig_ = cfg;

    overflowMode_ = cfg.eventOverflowMode;
    correctnessMode_ = cfg.correctnessMode;
    highPriorityVelocity_ = cfg.highPriorityVelocity;
    shedStartPercent_ = cfg.shedStartPercent;
    maxEventsPerBlock_ = cfg.maxEventsPerBlock;
    diagnosticsEnabled_ = cfg.diagnosticsEnabled;
    diagnosticsWindow_ = cfg.diagnosticsEnabled && cfg.diagnosticsWindow;
    diagnosticsDebugOutput_ = cfg.diagnosticsEnabled && cfg.diagnosticsDebugOutput;
    cancelProducers_.store(false, std::memory_order_release);

    sampleRate = cfg.sampleRate;
    bufferFrames = cfg.bufferFrames;
    LOG("Initialize: sampleRate=%u bufferFrames=%u maxVoices=%u", sampleRate, bufferFrames, cfg.maxVoices);

    audioOutput = new AudioOutput();
    if (!audioOutput->Initialize(sampleRate, bufferFrames)) {
        HRESULT hr = audioOutput->GetLastError();
        LOG("FAILED: AudioOutput::Initialize hr=0x%08X", (unsigned)hr);
        delete audioOutput;
        audioOutput = nullptr;
        return false;
    }
    bufferFrames = audioOutput->GetBufferFrames();
    sampleRate = audioOutput->GetSampleRate();
    LOG("AudioOutput initialized, rate=%u bufferFrames=%u", sampleRate, bufferFrames);

    bufferCapacity = bufferFrames;
    leftBuffer = static_cast<float*>(_aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
    rightBuffer = static_cast<float*>(_aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
    if (!leftBuffer || !rightBuffer) {
        LOG("FAILED: Could not allocate render buffers");
        return false;
    }

    voiceManager = new VoiceManager();
    voiceManager->Initialize(cfg.maxVoices, sampleRate);
    LOG("VoiceManager initialized, maxVoices=%u", cfg.maxVoices);

    channelCache = new ChannelCache();
    channelCache->SetMasterVolume(cfg.masterVolume);
    renderScalar = new RenderScalar();

    // Register the EventDispatcher callback so RenderScalar can dispatch
    // MIDI events at their exact sub-sample positions during RenderBlock.
    renderScalar->SetEventDispatcher(DispatchRenderEvent, this);

    configSnapshot = new RuntimeConfigSnapshot();
    std::memset(configSnapshot, 0, sizeof(RuntimeConfigSnapshot));
    configSnapshot->masterVolume = cfg.masterVolume;
    configSnapshot->velocityCurve = cfg.velocityCurve;
    configSnapshot->velocityFloor = cfg.velocityFloor;
    configSnapshot->velocityIgnoreBelow = cfg.velocityIgnoreBelow;
    configSnapshot->ignoreVelocity = cfg.ignoreVelocity;
    configSnapshot->monoOutput = cfg.monoOutput;
    configSnapshot->enableReverb = cfg.enableReverb;
    configSnapshot->enableChorus = cfg.enableChorus;
    configSnapshot->enableFilter = cfg.enableFilter;
    configSnapshot->enableModulators = cfg.enableModulators;
    configSnapshot->interpolation = cfg.interpolation;
    configSnapshot->filterType = cfg.filterType;
    configSnapshot->panLaw = cfg.panLaw;
    configSnapshot->correctnessMode = cfg.correctnessMode;

    audioOutput->SetRenderCallback(RenderCallback, this);

    initialized = true;
    LOG("Initialize SUCCESS");

    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_))
        DiagWindow_Create(diagnosticsWindow_, diagnosticsDebugOutput_);

    return true;
}

void Driver::Shutdown() {
    cancelProducers_.store(true, std::memory_order_release);
    producerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(producerWakeEpoch_);
    if (audioOutput) {
        audioOutput->Stop();
        audioOutput->Shutdown();
        delete audioOutput;
        audioOutput = nullptr;
    }
    midiIngress_.DrainAvailable();
    eventScheduler_.Reset();
    scheduledSizePublished_.store(0, std::memory_order_release);
    delete voiceManager; voiceManager = nullptr;
    delete channelCache; channelCache = nullptr;
    delete renderScalar; renderScalar = nullptr;
    delete configSnapshot; configSnapshot = nullptr;
    _aligned_free(leftBuffer); leftBuffer = nullptr;
    _aligned_free(rightBuffer); rightBuffer = nullptr;
    bufferCapacity = 0;

    if (soundFontData) {
        bool ownsResampled = (sampleDataStore == soundFontData->resampledData);
        bool ownsRawSamples = (sampleDataStore && !ownsResampled);
        if (ownsRawSamples && sampleDataStore != (void*)(soundFontData->sampleData)) {
            free(sampleDataStore);
        }
        sampleDataStore = nullptr;
        soundFontData->resampledData = nullptr;
        free(samplesStore); samplesStore = nullptr;
        sf2_free(soundFontData);
        delete soundFontData;
        soundFontData = nullptr;
    } else {
        free(sampleDataStore); sampleDataStore = nullptr;
        free(samplesStore); samplesStore = nullptr;
    }

    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_))
        DiagWindow_Destroy();

    initialized = false;
}

bool Driver::LoadConfiguredSoundFont() {
    const std::wstring widePath = ResolveV3SoundFontPath(engineConfig_);
    if (widePath.empty()) return false;
    return LoadSoundFont(widePath.c_str());
}

bool Driver::LoadSoundFont(const wchar_t* path) {
    if (!path) return false;

    // Parsing and sample conversion happen on the calling thread. If a host
    // requests a reload while audio is active, stop and join first so the
    // callback can never observe partially replaced SF2/sample storage.
    const bool restartAudio = audioOutput && audioOutput->IsRunning();
    if (restartAudio) audioOutput->Stop();
    struct RestartAudioGuard {
        AudioOutput* output;
        bool restart;
        ~RestartAudioGuard() { if (restart && output) output->Start(); }
    } restartGuard{audioOutput, restartAudio};

    EnterCriticalSection(&cs);

    SF2Data* sf2 = new SF2Data();
    if (!sf2_load(path, sf2)) {
        LOG("  sf2_load FAILED: presets=%u inst=%u samples=%u sampleData=%d frames=%u",
            sf2->presetCount, sf2->instrumentCount, sf2->sampleCount,
            sf2->sampleData ? 1 : 0, sf2->sampleDataFrames);
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(h, nullptr);
            LOG("  File exists and readable, size=%u bytes", sz);
            CloseHandle(h);
        } else {
            LOG("  File access error: %u", (unsigned)GetLastError());
        }
        delete sf2;
        LeaveCriticalSection(&cs);
        return false;
    }
    LOG("  Parsed: %u presets, %u instruments, %u samples, %u frames",
        sf2->presetCount, sf2->instrumentCount, sf2->sampleCount, sf2->sampleDataFrames);

    sf2_build_regions(sf2);
    LOG("  Built %u regions", sf2->regionCount);
    if (sf2->regionOverflow) {
        LOG("  FAILED: SoundFont compiled region capacity exceeded");
        sf2_free(sf2);
        delete sf2;
        LeaveCriticalSection(&cs);
        return false;
    }

    if (soundFontData) {
        free(sampleDataStore); sampleDataStore = nullptr;
        free(samplesStore); samplesStore = nullptr;
        sf2_free(soundFontData);
        delete soundFontData;
    }
    soundFontData = sf2;

    // Establish the initial selected preset for every channel.  Future
    // invalid program changes do not overwrite this committed selection.
    if (channelCache) {
        for (uint8_t ch = 0; ch < kChannelCount; ++ch) {
            uint32_t presetIndex = 0;
            if (ResolveChannelPreset(soundFontData, *channelCache, ch, &presetIndex)) {
                channelCache->SetSelectedPreset(ch, static_cast<uint16_t>(presetIndex));
            } else {
                channelCache->SetSelectedPreset(ch, UINT16_MAX);
            }
        }
    }

    if (sf2->sampleData) {
        uint32_t frames = sf2->sampleDataFrames;
        float* fbuf = static_cast<float*>(malloc(frames * sizeof(float)));
        if (fbuf) {
            for (uint32_t i = 0; i < frames; ++i) {
                fbuf[i] = sf2->sampleData[i] / 32768.0f;
            }
            sampleDataStore = fbuf;
        }
        sampleDataFrames = frames;
    }

    uint32_t sampCount = sf2->sampleCount;
    SF2Sample* sampBuf = static_cast<SF2Sample*>(malloc(sampCount * sizeof(SF2Sample)));
    if (sampBuf) {
        std::memcpy(sampBuf, sf2->samples, sampCount * sizeof(SF2Sample));
    }
    samplesStore = sampBuf;
    sampleStoreCount = sampCount;

    LOG("  LoadSoundFont SUCCESS: %u samples cached", sampCount);
    LeaveCriticalSection(&cs);
    return true;
}

bool Driver::IsInitialized() const {
    return initialized;
}

void Driver::CopyDebugInfo(DriverDebugInfo& out) const {
    const uint32_t index = debugSnapshotIndex_.load(std::memory_order_acquire) & 1u;
    out = debugSnapshots_[index];
    // These fields are useful even before the first render callback publishes
    // a snapshot (for example when WASAPI failed to start its event loop).
    out.soundFontLoaded = soundFontData && sampleDataStore ? 1u : 0u;
    out.sampleDataFrames = sampleDataFrames;
    out.sampleCount = sampleStoreCount;
    out.audioRunning = audioOutput && audioOutput->IsRunning() ? 1u : 0u;
    out.audioHResult = audioOutput ? static_cast<int32_t>(audioOutput->GetLastError()) : 0;
}

void Driver::StartAudio() {
    if (audioOutput && !audioOutput->IsRunning()) {
        LOG("StartAudio: starting WASAPI stream...");
        bool ok = audioOutput->Start();
        LOG("StartAudio: %s", ok ? "SUCCESS" : "FAILED");
    }
}

void Driver::ResetAllVoices() {
    if (audioOutput && audioOutput->IsRunning()) {
        SubmitShortMsg(kInternalResetMessage);
        return;
    }
    if (voiceManager) voiceManager->Reset();
    if (channelCache) channelCache->Reset();
    nextPlayIndex_ = 1;
    eventScheduler_.Reset();
    limiter.Reset();
}

void Driver::SubmitShortMsg(uint32_t msg) {
    submittedAtomic_.fetch_add(1, std::memory_order_relaxed);
    TimestampedMidiEvent evt{};
    evt.message = msg;
    evt.sequence = nextEventSequence_.fetch_add(1, std::memory_order_relaxed);
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&evt.qpcTimestamp));

    const uint8_t status = static_cast<uint8_t>(msg & 0xffu);
    const uint8_t data1 = static_cast<uint8_t>((msg >> 8) & 0x7fu);
    const uint8_t velocity = static_cast<uint8_t>((msg >> 16) & 0x7fu);
    const bool noteOn = msg != kInternalResetMessage &&
                        (status & 0xf0u) == 0x90u && velocity != 0;

    // Publish the cutoff before waiting on the lossless state lane.  If that
    // lane overtakes a backlog of older note-ons, those notes are rejected at
    // dispatch instead of resurrecting sound after pause/mute.
    if (msg == kInternalResetMessage) {
        PublishTerminationFence(globalTerminationFence_, evt.sequence);
    } else if ((status & 0xf0u) == 0xb0u &&
               (data1 == 120u || data1 == 123u)) {
        PublishTerminationFence(channelTerminationFence_[status & 0x0fu], evt.sequence);
    }

    EventLane lane = EventLane::State;
    if (noteOn) {
        if (velocity >= highPriorityVelocity_) lane = EventLane::Loud;
        else if (velocity >= 64) lane = EventLane::UpperMedium;
        else if (velocity >= 32) lane = EventLane::Medium;
        else lane = EventLane::Quiet;
    }

    const uint32_t ingress = midiIngress_.TotalSize();
    const uint32_t scheduled = scheduledSizePublished_.load(std::memory_order_acquire);
    const uint32_t ingressPressure = ingress * 100u / PriorityEventIngress<TimestampedMidiEvent>::TotalCapacity();
    const uint32_t scheduledPressure = scheduled * 100u / kEventBufferCapacity;
    const uint32_t pressure = (std::max)(ingressPressure, scheduledPressure);
    uint32_t cutoff = 1;
    if (pressure > shedStartPercent_) {
        const uint32_t range = 100u - shedStartPercent_;
        cutoff = 1u + ((std::min)(pressure - shedStartPercent_, range) * 94u) / range;
    }
    currentVelocityCutoffAtomic_.store(cutoff, std::memory_order_relaxed);

    if (noteOn && velocity < cutoff &&
        overflowMode_ == EventOverflowMode::PriorityVelocity) {
        shedAtomic_.fetch_add(1, std::memory_order_relaxed);
        shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const bool lossless = !noteOn || velocity >= highPriorityVelocity_ ||
                          overflowMode_ == EventOverflowMode::LosslessBackpressure;
    for (;;) {
        if (midiIngress_.TryPush(lane, evt)) {
            acceptedAtomic_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!lossless) {
            shedAtomic_.fetch_add(1, std::memory_order_relaxed);
            shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (cancelProducers_.load(std::memory_order_acquire)) {
            cancelledAtomic_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        uint32_t observed = producerWakeEpoch_.load(std::memory_order_acquire);
        WaitForAddressChange(producerWakeEpoch_, observed);
    }
}

void Driver::RenderCallback(float* output, uint32_t numFrames, void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self || !self->initialized) return;
    ++self->callbackCount_;

    VoiceManager* vm = self->voiceManager;
    ChannelCache* cc = self->channelCache;
    RenderScalar* render = self->renderScalar;
    RuntimeConfigSnapshot* snap = self->configSnapshot;
    const float* sd = self->sampleDataStore;

    if (!vm || !cc || !render || !snap) return;

    LARGE_INTEGER renderStartQPC;
    QueryPerformanceCounter(&renderStartQPC);

    cc->RebuildCache(*snap, static_cast<float>(self->sampleRate));

    if (!self->leftBuffer || !self->rightBuffer) {
        std::memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }
    float* leftBuf = self->leftBuffer;
    float* rightBuf = self->rightBuffer;
    std::memset(leftBuf, 0, numFrames * sizeof(float));
    std::memset(rightBuf, 0, numFrames * sizeof(float));

    // ── Diagnostic: voice retire stats ──────────────────────────────

    // ── Persistent Pending Queue — Unified Event Pipeline ──────────────
    // The SPSC queue carries TimestampedMidiEvent structs from the MIDI
    // host thread.  We drain ALL of them into a persistent, audio-thread-
    // only `pendingEventBuffer`.  Events are converted to RenderEvent with
    // a fractional sampleOffset computed ONCE against this block's start
    // QPC.  From the pending queue we budget-extract at most
    // kMaxEventsPerBlock events that fall within [0, numFrames) into
    // evtBuf for dispatch.  Remaining events stay in the pending queue
    // and have their offsets decremented by numFrames at block end (smooth
    // rollover).  This guarantees:
    //
    //   a) Zero event loss — events beyond the budget simply queue up and
    //      fire in subsequent blocks (natural time-stretching).
    //   b) No frame-0 clumping — pending offsets are decremented uniformly,
    //      never re-computed from QPC.
    //   c) Bounded per-block work — the DSP thread processes at most
    //      kMaxEventsPerBlock events regardless of input density.
    uint64_t blockStartQPC;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&blockStartQPC));

    // ── Virtual render clock ─────────────────────────────────────────
    // Monotonically advances by (numFrames * qpcFreq / sr) each callback
    // so event offsets are relative to the audio timeline, not the
    // wall-clock callback time.  Without this, every SPSC event pushed
    // during the previous block has a negative deltaQPC and snaps to
    // sample 0 — producing the 100 Hz buffer-grid buzz.
    if (!self->clockInitialized) {
        self->virtualRenderClockQPC = blockStartQPC;
        self->clockInitialized = true;
    }

    const int64_t wallRenderSample = QpcDeltaToFrames(
        static_cast<int64_t>(blockStartQPC) -
            static_cast<int64_t>(self->virtualRenderClockQPC),
        static_cast<int64_t>(self->qpcFreq), self->sampleRate);
    const int64_t recoveredRenderSample = RecoverRealtimeRenderFrame(
        self->virtualRenderSample_, wallRenderSample, numFrames);
    if (recoveredRenderSample > self->virtualRenderSample_) {
        self->telemetry_.skippedOutputFrames += static_cast<uint64_t>(
            recoveredRenderSample - self->virtualRenderSample_);
        self->virtualRenderSample_ = recoveredRenderSample;
    }

    // ── Drift recovery ──────────────────────────────────────────────
    // If the render takes >100% CPU, the virtual clock falls behind
    // wall time.  Left unchecked this causes permanent desync: event
    // offsets inflate forever, the pending queue never drains, and the
    // audio is delayed until reset.  Fast-forward the clock to catch
    // up and rescale queued event offsets so they land in this block.
    // ── Step 1: Drain ALL SPSC events → append to pending queue ─────────
    TimestampedMidiEvent timed;
    uint32_t drainedEvents = 0;
    while (drainedEvents < self->maxEventsPerBlock_ &&
           self->eventScheduler_.Size() < kEventBufferCapacity &&
           self->midiIngress_.TryPop(timed)) {
        ++drainedEvents;
        if (self->eventScheduler_.Size() >= kEventBufferCapacity) {
            break;
        }

        int64_t deltaQPC = static_cast<int64_t>(timed.qpcTimestamp) -
                           static_cast<int64_t>(self->virtualRenderClockQPC);

        uint32_t msg = timed.message;
        uint8_t st = static_cast<uint8_t>(msg & 0xFF);
        uint8_t d1 = static_cast<uint8_t>((msg >> 8) & 0xFF);
        uint8_t d2 = static_cast<uint8_t>((msg >> 16) & 0xFF);
        uint8_t ch = st & 0x0F;

        RenderEventType etype;
        if (msg == kInternalResetMessage) {
            etype = RenderEventType::Reset;
            ch = d1 = d2 = 0;
        } else {
            switch (st & 0xF0) {
                case 0x90:
                    if (d2 > 0) etype = RenderEventType::NoteOn;
                    else        { etype = RenderEventType::NoteOff; d2 = 0; }
                    break;
                case 0x80: etype = RenderEventType::NoteOff; break;
                case 0xB0: etype = RenderEventType::ControlChange; break;
                case 0xC0: etype = RenderEventType::ProgramChange; break;
                case 0xE0: etype = RenderEventType::PitchBend; break;
                default: continue;
            }
        }

        RenderEvent ev;
        ev.type = etype;
        ev.channel = ch;
        ev.data1 = d1;
        ev.data2 = d2;
        ev.frameOffset = 0;
        ev.ingressSequence = timed.sequence;

        // Keep the legacy scratch array empty in the new path; the bounded
        // audio-thread heap owns future events and orders by sample/sequence.
        const int64_t deltaFrames = QpcDeltaToFrames(
            deltaQPC, static_cast<int64_t>(self->qpcFreq), self->sampleRate);
        ScheduledRenderEvent scheduled;
        scheduled.event = ev;
        scheduled.targetFrame = deltaFrames + static_cast<int64_t>(self->bufferFrames);
        scheduled.sequence = timed.sequence;
        if (etype == RenderEventType::NoteOn &&
            IsObsoleteNoteOn(scheduled.targetFrame, self->virtualRenderSample_,
                             self->bufferFrames)) {
            ++self->telemetry_.late;
            ++self->telemetry_.staleNoteOnsSkipped;
            continue;
        }
        if (!self->eventScheduler_.Enqueue(scheduled)) {
            ++self->telemetry_.dropped;
            break;
        }
        self->telemetry_.scheduledHighWater =
            (std::max)(self->telemetry_.scheduledHighWater,
                       static_cast<uint64_t>(self->eventScheduler_.Size()));
    }

    self->producerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(self->producerWakeEpoch_);
    self->scheduledSizePublished_.store(self->eventScheduler_.Size(),
                                        std::memory_order_release);

    // Extract only this render window.  Future events remain in the heap.
    RenderEvent* evtBuf = self->eventBuffer;
    uint32_t evCount = 0;
    uint32_t examinedCount = 0;
    const uint32_t eventBudget =
        (std::min)(self->maxEventsPerBlock_, kEventBufferCapacity);
    ScheduledRenderEvent scheduledOut;
    while (examinedCount < eventBudget &&
           self->eventScheduler_.PopBefore(self->virtualRenderSample_ + numFrames,
                                           scheduledOut)) {
        ++examinedCount;
        if (scheduledOut.event.type == RenderEventType::NoteOn &&
            IsObsoleteNoteOn(scheduledOut.targetFrame,
                             self->virtualRenderSample_, self->bufferFrames)) {
            ++self->telemetry_.late;
            ++self->telemetry_.staleNoteOnsSkipped;
            continue;
        }
        int64_t offset = scheduledOut.targetFrame - self->virtualRenderSample_;
        if (offset < 0) { ++self->telemetry_.late; offset = 0; }
        scheduledOut.event.frameOffset = static_cast<uint32_t>(offset);
        evtBuf[evCount++] = scheduledOut.event;
        ++self->telemetry_.dispatched;
    }
    self->scheduledSizePublished_.store(self->eventScheduler_.Size(),
                                        std::memory_order_release);

    // ── Step 2: Sort pending queue by sampleOffset ─────────────────────
    // Insertion sort.  The carry-forward portion (from previous blocks) is
    // already sorted (offset decrement preserves relative order).  Newly
    // appended SPSC events at the tail are the only unsorted elements.
    // For mostly-sorted data this is O(N) with a small constant.

    // ── Step 3: Extract all in-block events for this block ──────────────
    // Walk the sorted pending queue from offset 0.  Take every event with
    // sampleOffset < numFrames into evtBuf.  Stop at the first future
    // event (offset >= numFrames).  With in-place voice recycling keeping
    // active polyphony bounded to ~128-256 voices, the engine can process
    // all incoming events in real-time without batch-chunking artifacts.
    // ── Step 4: Decrement remaining pending offsets by numFrames ────────
    // Smooth rollover: future events advance at the true audio clock rate
    // toward their firing time without frame-boundary re-quantization.
    // ── Step 5: Sort evtBuf by sampleOffset (defensive) ─────────────────

    // ── Step 6: Render with sub-sample event slicing ───────────────────
    // RenderBlock will invoke DispatchRenderEvent at each event's exact
    // fractional sample offset.  All event handling (voice allocation,
    // release, CC updates) happens inside the render loop via the callback.
    render->RenderBlock(*vm, *cc, sd, self->sampleDataFrames,
                        leftBuf, rightBuf, numFrames, *snap,
                        evtBuf, evCount, self->correctnessMode_,
                        static_cast<uint64_t>(self->virtualRenderSample_));

    float renderPeak = 0.0f;
    for (uint32_t i = 0; i < numFrames; ++i) {
        renderPeak = (std::max)(renderPeak, std::fabs(leftBuf[i]));
        renderPeak = (std::max)(renderPeak, std::fabs(rightBuf[i]));
    }
    self->sf2Telemetry_.renderPeak = renderPeak;
    if (self->sf2Telemetry_.lastVoiceHandle < vm->GetMaxVoices()) {
        const uint32_t h = self->sf2Telemetry_.lastVoiceHandle;
        self->sf2Telemetry_.lastPhase = vm->v.phases[h];
    }

    // ── Advance virtual render clock for the next callback ──────────
    self->virtualRenderSample_ += static_cast<int64_t>(numFrames);

    // Interleave planar L/R into the interleaved output buffer
    for (uint32_t i = 0; i < numFrames; ++i) {
        // masterVolume is already included in ChannelCache's per-channel
        // mix gains. Applying it here as well attenuates the final signal a
        // second time (the default 0.1 becomes 0.01).
        output[i * 2]     = leftBuf[i];
        output[i * 2 + 1] = rightBuf[i];
    }

    self->limiter.Process(output, numFrames, 2);

    LARGE_INTEGER renderEndQPC;
    QueryPerformanceCounter(&renderEndQPC);

    double elapsedUs = (double)(renderEndQPC.QuadPart - renderStartQPC.QuadPart)
                     / (double)self->qpcFreq * 1e6;
    double budgetUs = (double)numFrames / (double)self->sampleRate * 1e6;
    float cpuPct = (budgetUs > 0.0) ? (float)(elapsedUs / budgetUs * 100.0) : 0.0f;
    self->callbackTiming_.Observe(cpuPct);
    self->telemetry_.maxCallbackQPC = (std::max)(self->telemetry_.maxCallbackQPC,
        static_cast<uint64_t>(renderEndQPC.QuadPart - renderStartQPC.QuadPart));
    self->telemetry_.callbackP95Percent = self->callbackTiming_.Percentile(95u, 100u);
    self->telemetry_.callbackP99Percent = self->callbackTiming_.Percentile(99u, 100u);
    self->telemetry_.callbackP999Percent = self->callbackTiming_.Percentile(999u, 1000u);
    self->telemetry_.overBudgetCallbacks = self->callbackTiming_.overBudgetCallbacks;
    self->telemetry_.maxConsecutiveOverBudget =
        self->callbackTiming_.maxConsecutiveOverBudget;
    self->telemetry_.immediateRetirements = vm->retireImmediateCount_;
    self->telemetry_.voiceSteals = vm->stealCount_;
    self->telemetry_.submitted = self->submittedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.accepted = self->acceptedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.dropped = self->shedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.shedNoteOns = self->telemetry_.dropped;
    self->telemetry_.cancelledSubmissions =
        self->cancelledAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.currentVelocityCutoff =
        self->currentVelocityCutoffAtomic_.load(std::memory_order_relaxed);

    const uint32_t nextDebugIndex =
        (self->debugSnapshotIndex_.load(std::memory_order_relaxed) + 1u) & 1u;
    DriverDebugInfo& debug = self->debugSnapshots_[nextDebugIndex];
    debug = DriverDebugInfo{};
    debug.callbackCount = self->callbackCount_;
    debug.submitted = self->telemetry_.submitted;
    debug.accepted = self->telemetry_.accepted;
    debug.dispatched = self->telemetry_.dispatched;
    debug.noteOns = self->sf2Telemetry_.noteOns;
    debug.matchedRegions = self->sf2Telemetry_.exactRegionMatches;
    debug.configuredVoices = self->sf2Telemetry_.configuredVoices;
    debug.activeVoices = vm->activeCount_;
    debug.sampleDataFrames = self->sampleDataFrames;
    debug.sampleCount = self->sampleStoreCount;
    debug.soundFontLoaded = self->soundFontData && self->sampleDataStore ? 1u : 0u;
    debug.audioRunning = self->audioOutput && self->audioOutput->IsRunning() ? 1u : 0u;
    debug.audioHResult = self->audioOutput
        ? static_cast<int32_t>(self->audioOutput->GetLastError()) : 0;
    debug.renderPeak = self->sf2Telemetry_.renderPeak;
    self->debugSnapshotIndex_.store(nextDebugIndex, std::memory_order_release);
    if (self->diagnosticsEnabled_) {
        for (uint32_t velocity = 0; velocity < 128; ++velocity) {
            self->telemetry_.shedByVelocity[velocity] =
                self->shedByVelocityAtomic_[velocity].load(std::memory_order_relaxed);
        }
    }

    // Smooth CPU reading with a simple low-pass
    static float s_cpuSmoothed = 0.0f;
    s_cpuSmoothed += 0.1f * (cpuPct - s_cpuSmoothed);

    if (self->diagnosticsEnabled_) {
        static int diagTick = 0;
        if (++diagTick >= 1) {
            diagTick = 0;
            if (self->diagnosticsWindow_ || self->diagnosticsDebugOutput_) {
                uint32_t releasingVoices = 0;
                uint32_t sustainHeldVoices = 0;
                for (uint32_t position = 0; position < vm->activeCount_; ++position) {
                    const uint32_t voice = vm->activeList_[position];
                    releasingVoices += vm->v.state[voice] ==
                        static_cast<uint8_t>(VoiceState::Releasing);
                    sustainHeldVoices += vm->v.heldBySustain[voice] != 0;
                }
                DiagWindow_Update(vm->activeCount_, vm->GetMaxVoices(),
                                  releasingVoices, sustainHeldVoices,
                                  vm->stealCount_,
                                  s_cpuSmoothed, self->correctnessMode_ ? 1u
                                      : ComputeDecimationStep(vm->activeCount_),
                                  self->telemetry_.callbackP95Percent,
                                  self->telemetry_.callbackP99Percent,
                                  self->telemetry_.callbackP999Percent,
                                  self->telemetry_.overBudgetCallbacks,
                                  self->telemetry_.maxConsecutiveOverBudget,
                                  vm->retireCount_, vm->retireImmediateCount_,
                                  self->sf2Telemetry_);
            }
        }
    }
}

// ── EventDispatcher callback ─────────────────────────────────────────────
// Called by RenderScalar::RenderBlock at each event's exact sub-sample
// position.  Voice allocation, release, CC updates all happen here so
// that the sub-sample phase offset and releaseStartInBlock are set at the precise frame.
void Driver::DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                  void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self) return;

    switch (event.type) {
        case RenderEventType::NoteOn: {
            const uint64_t globalFence =
                self->globalTerminationFence_.load(std::memory_order_acquire);
            const uint64_t channelFence = event.channel < kChannelCount
                ? self->channelTerminationFence_[event.channel].load(std::memory_order_acquire)
                : 0u;
            if (FenceSuppresses(event.ingressSequence, globalFence) ||
                FenceSuppresses(event.ingressSequence, channelFence)) {
                break;
            }
            self->HandleNoteOn(event.channel, event.data1, event.data2);
            break;
        }
        case RenderEventType::NoteOff:
            self->HandleNoteOff(event.channel, event.data1, blockCursor);
            break;
        case RenderEventType::ControlChange:
            self->HandleControlChange(event.channel, event.data1, event.data2,
                                      blockCursor);
            break;
        case RenderEventType::ProgramChange:
            self->HandleProgramChange(event.channel, event.data1);
            break;
        case RenderEventType::PitchBend:
            self->HandlePitchBend(event.channel, event.data1, event.data2);
            break;
        case RenderEventType::AllNotesOff:
        case RenderEventType::AllSoundOff:
            // [HOOK] Overload ladder: these can trigger hard/panic release.
            break;
        case RenderEventType::Reset:
            if (self->voiceManager) self->voiceManager->Reset();
            if (self->channelCache) self->channelCache->Reset();
            self->nextPlayIndex_ = 1;
            self->limiter.Reset();
            break;
    }
}

void Driver::HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!channelCache || !voiceManager) return;

    ++sf2Telemetry_.noteOns;
    sf2Telemetry_.lastChannel = channel;
    sf2Telemetry_.lastNote = note;
    sf2Telemetry_.lastVelocity = velocity;

    const float mappedVelocity = configSnapshot
        ? channelCache->ComputeVelocity(velocity, *configSnapshot)
        : static_cast<float>(velocity) / 127.0f;
    if (mappedVelocity <= 0.0f) return;

    channelCache->NoteOn(channel, note, velocity);

    if (!soundFontData || !samplesStore || !sampleDataStore) return;

    // The selected-preset cache is a committed optimization, not the source
    // of truth. Re-resolve current bank/program at every note-on so a stale
    // cache can never turn a live piano note into a silent allocation.
    uint32_t presetIndex = UINT32_MAX;
    if (!ResolveChannelPreset(soundFontData, *channelCache, channel, &presetIndex)) {
        ++sf2Telemetry_.invalidPresets;
        sf2Telemetry_.lastPreset = UINT16_MAX;
        return;
    }
    if (channelCache->GetSelectedPreset(channel) != presetIndex)
        channelCache->SetSelectedPreset(channel, static_cast<uint16_t>(presetIndex));
    sf2Telemetry_.lastPreset = static_cast<uint16_t>(presetIndex);

    static constexpr uint32_t kMaxMatchingRegions = 512;
    const SFSampleRegion* matchingRegions[kMaxMatchingRegions];
    uint32_t matchCount = sf2_find_regions(soundFontData, presetIndex, note, velocity,
                                           matchingRegions, kMaxMatchingRegions);
    if (matchCount > kMaxMatchingRegions) {
        ++telemetry_.allocationFailures;
        return;
    }

    if (matchCount == 0) {
        ++telemetry_.zeroMatchedRegions;
        ++sf2Telemetry_.zeroMatchedRegions;
        return;
    }

    // Validate every layer before mutating the voice pool. A malformed
    // SoundFont region must reject the complete generation, never leave a
    // partial instrument whose remaining layers become audible artifacts.
    for (uint32_t mi = 0; mi < matchCount; ++mi) {
        const SFSampleRegion* region = matchingRegions[mi];
        if (!region || region->sampleIndex >= sampleStoreCount) {
            ++sf2Telemetry_.invalidRegions;
            return;
        }
        if (!sf2_validate_region(soundFontData, region)) {
            ++sf2Telemetry_.invalidSampleRanges;
            return;
        }
    }
    sf2Telemetry_.exactRegionMatches += matchCount;

    // All regions layered by this one MIDI note-on share a generation.  A
    // later note-off must release only this generation's oldest outstanding
    // retrigger, not every voice with the same channel/key.
    if (nextPlayIndex_ == 0 || nextPlayIndex_ >= UINT32_MAX - 1)
        nextPlayIndex_ = 1;
    const uint32_t playIndex = nextPlayIndex_++;

    float sr = (float)(sampleRate > 0 ? sampleRate : 44100u);
    float pitchBendSemitones = channelCache->GetPitchBendSemitones(channel);

    VoiceHandle allocatedVoices[kMaxMatchingRegions];
    uint32_t allocatedCount = 0;
    for (; allocatedCount < matchCount; ++allocatedCount) {
        allocatedVoices[allocatedCount] = voiceManager->AllocateVoiceOrSteal(
            channel, note, velocity);
        if (allocatedVoices[allocatedCount] == kInvalidVoice) {
            ++telemetry_.allocationFailures;
            for (uint32_t rollback = 0; rollback < allocatedCount; ++rollback)
                voiceManager->RetireVoice(allocatedVoices[rollback]);
            return;
        }
    }

    for (uint32_t mi = 0; mi < matchCount; ++mi) {
        const SFSampleRegion* matchedRegion = matchingRegions[mi];
        uint32_t sampleIndex = matchedRegion->sampleIndex;
        const SF2Sample& samp = samplesStore[sampleIndex];
        const VoiceHandle vh = allocatedVoices[mi];

        voiceManager->SetVoiceSoundFontIdentity(
            vh, static_cast<uint16_t>(presetIndex),
            static_cast<uint16_t>(matchedRegion - soundFontData->regions));
        voiceManager->SetVoicePlayIndex(vh, playIndex);

        int effRootKey = (matchedRegion->rootKey >= 0)
            ? matchedRegion->rootKey : (int)samp.originalPitch;
        float rootKey = (float)effRootKey;
        float coarseTune = (float)matchedRegion->coarseTune;
        float fineTune = (float)matchedRegion->fineTune / 100.0f;
        float keyTrack = (matchedRegion->scaleTuning != 0)
            ? (float)matchedRegion->scaleTuning : 100.0f;

        float noteTuneSemitones = coarseTune + fineTune;
        const float bendScale = keyTrack / 100.0f;
        const float baseSemitoneOffset =
            ((float)note + noteTuneSemitones - rootKey) * bendScale;
        const float basePitchRatio = powf(2.0f, baseSemitoneOffset / 12.0f);
        const float bendRatio = powf(
            2.0f, pitchBendSemitones * bendScale / 12.0f);

        float srcRate = (float)(samp.sampleRate > 0 ? samp.sampleRate : 44100u);
        float outRate = (float)(sampleRate > 0 ? sampleRate : 44100u);
        const float rateRatio = (outRate > 0.0f && srcRate > 0.0f)
            ? srcRate / outRate : 1.0f;
        float basePhaseStep = rateRatio * basePitchRatio;
        float phaseStep = basePhaseStep * bendRatio;
        // A corrupt SF2 pitch generator must not poison the scalar loop with
        // NaN/Inf phase values: float-to-integer conversion then pins sample
        // lookup unpredictably and silently poisons the mixed output.
        if (!std::isfinite(phaseStep) || phaseStep <= 0.0f) {
            phaseStep = 1.0f;
        }

        uint32_t sStart = static_cast<uint32_t>(matchedRegion->startOffset);
        uint32_t sEnd = static_cast<uint32_t>(matchedRegion->endOffset);
        uint32_t sLoopStart = static_cast<uint32_t>(matchedRegion->loopStartOffset);
        uint32_t sLoopEnd = static_cast<uint32_t>(matchedRegion->loopEndOffset);
        uint8_t loopMode = matchedRegion->loopMode;

        voiceManager->SetVoiceSample(vh, sStart, sEnd, sLoopStart, sLoopEnd,
                                     loopMode, phaseStep, 1);
        voiceManager->SetVoicePitchBase(vh, basePhaseStep, bendScale);

        uint32_t mappedVelocityIndex = static_cast<uint32_t>(
            mappedVelocity * 127.0f + 0.5f);
        if (mappedVelocityIndex < 1u) mappedVelocityIndex = 1u;
        if (mappedVelocityIndex > 127u) mappedVelocityIndex = 127u;
        float velGain = g_velGainLUT[mappedVelocityIndex];
        float initialGain = velGain;

        if (matchedRegion->initialAttenuation > 0) {
            float attenDb = matchedRegion->initialAttenuation / 10.0f;
            initialGain *= InitialAttenuationToGain(attenDb * 10.0f);
        }

        const float sustainCentibels = (std::max)(0.0f,
            static_cast<float>(matchedRegion->sustainVolEnv));
        float sustainLevel = SustainAttenuationToGain(sustainCentibels);
        if (sustainLevel > 1.0f) sustainLevel = 1.0f;

        const float delaySeconds = TimecentsToSeconds(matchedRegion->delayVolEnv);
        const float holdSeconds = TimecentsToSeconds(matchedRegion->holdVolEnv);
        const float attackSeconds = TimecentsToSeconds(matchedRegion->attackVolEnv);
        const float decaySeconds = TimecentsToSeconds(matchedRegion->decayVolEnv);
        const float releaseSeconds = TimecentsToSeconds(matchedRegion->releaseVolEnv);

        uint32_t delaySamples = (delaySeconds > 0.0f) ? (uint32_t)(delaySeconds * sr) : 0u;
        uint32_t holdSamples = (holdSeconds > 0.0f) ? (uint32_t)(holdSeconds * sr) : 0u;
        uint32_t attackSamples = (attackSeconds > 0.0001f) ? (uint32_t)(attackSeconds * sr) : 0u;
        uint32_t decaySamples = (decaySeconds > 0.0001f) ? (uint32_t)(decaySeconds * sr) : 0u;

        float attackGainStep = 0.0f;
        float decaySlope = 1.0f;

        if (attackSamples > 0) {
            attackGainStep = initialGain / (float)attackSamples;
        }

        if (decaySamples > 0) {
            float mysterySlope = -9.226f / (float)decaySamples;
            decaySlope = expf(mysterySlope);
            if (sustainLevel > 0.0f && sustainLevel < 1.0f) {
                decaySamples = (uint32_t)(logf(sustainLevel) / mysterySlope);
            }
        }

        float releaseDecay = MakeReleaseDecay(releaseSeconds, sampleRate);
        const uint32_t releaseSamples = MakeReleaseSamples(releaseSeconds, sampleRate);

        voiceManager->SetVoiceEnvelope(vh, initialGain, sustainLevel,
                                        delaySamples, holdSamples, attackSamples,
                                        decaySamples, attackGainStep, decaySlope, releaseDecay,
                                        releaseSamples);
        float regionPanLeft = 1.0f;
        float regionPanRight = 1.0f;
        channelCache->ComputeSoundFontPan(matchedRegion->pan,
                                          regionPanLeft, regionPanRight);
        voiceManager->SetVoiceGain(vh, regionPanLeft, regionPanRight);
        voiceManager->RefreshMixGain(vh, channelCache->GetParams()[channel]);

        // Sub-sample start position: folded directly into the initial phase
        // (on top of the hash randomization from SetVoiceSample).
        ++sf2Telemetry_.configuredVoices;
        sf2Telemetry_.lastRegion = static_cast<uint16_t>(matchedRegion - soundFontData->regions);
        sf2Telemetry_.lastSample = static_cast<uint16_t>(sampleIndex);
        sf2Telemetry_.lastSampleStart = sStart;
        sf2Telemetry_.lastSampleEnd = sEnd;
        sf2Telemetry_.lastInitialPeak = sf2_region_initial_peak(soundFontData, matchedRegion);
        sf2Telemetry_.lastVoiceGain = initialGain;
        sf2Telemetry_.lastMixGainL = voiceManager->v.mixGainL[vh];
        sf2Telemetry_.lastMixGainR = voiceManager->v.mixGainR[vh];
        sf2Telemetry_.lastDelaySamples = delaySamples;
        sf2Telemetry_.lastAttackSamples = attackSamples;
        sf2Telemetry_.lastFloatSample = sampleDataStore[sStart];
        sf2Telemetry_.lastPhaseStep = phaseStep;
        sf2Telemetry_.lastPhase = voiceManager->v.phases[vh];
        sf2Telemetry_.lastRelativeEnd = voiceManager->v.relEnd[vh];
        sf2Telemetry_.lastSampleBacked = voiceManager->v.sampleBacked[vh];
        sf2Telemetry_.lastVoiceHandle = vh;
    }
}

void Driver::HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset) {
    if (!channelCache || !voiceManager) return;

    bool sustain = channelCache->IsSustainActive(channel);
    channelCache->NoteOff(channel, note);

    // Use the voice's SF2-defined release — no adaptive release based on
    // pool pressure.  BASSMIDI and SnappySynth both use SF2-specified
    // release times; adaptive release causes audible inconsistency under
    // varying polyphony loads.
    const uint32_t playIndex = voiceManager->FindOldestPlayIndex(channel, note);
    if (playIndex == UINT32_MAX) return;
    voiceManager->NoteOffPlayIndex(channel, note, playIndex, sustain, blockOffset);
}

void Driver::HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value,
                                 uint32_t blockOffset) {
    const bool sustainWasActive = channelCache && channelCache->IsSustainActive(channel);
    if (channelCache) channelCache->ControlChange(channel, controller, value);

    if (controller == 64) {
        if (value < 64) {
            const uint32_t count = voiceManager->GetChannelActiveCount(channel);
            const uint32_t* handles = voiceManager->GetChannelActiveList(channel);
            for (uint32_t position = 0; position < count; ++position) {
                const uint32_t i = handles[position];
                if (voiceManager->v.heldBySustain[i]) {
                    voiceManager->v.heldBySustain[i] = 0;
                    voiceManager->StartRelease(i);
                }
            }
        }
    }

    if (controller == 120) {
        voiceManager->SilenceChannelImmediate(channel);
    } else if (controller == 123) {
        voiceManager->ReleaseChannel(channel, blockOffset);
    } else if (controller == 121 && sustainWasActive) {
        const uint32_t count = voiceManager->GetChannelActiveCount(channel);
        const uint32_t* handles = voiceManager->GetChannelActiveList(channel);
        for (uint32_t position = 0; position < count; ++position) {
            const uint32_t i = handles[position];
            if (voiceManager->v.heldBySustain[i]) {
                voiceManager->v.heldBySustain[i] = 0;
                voiceManager->StartRelease(i);
            }
        }
    }

    if (controller == 121) {
        // Reset All Controllers also centers the wheel. Recompute the phase
        // increment of already sounding voices at this exact event frame.
        HandlePitchBend(channel, 0, 64);
    }

    // Controller events are dispatched before the sample at their target
    // frame. Rebuild now so existing voices and same-frame note-ons observe
    // the new channel state rather than waiting for the next callback.
    if (channelCache && configSnapshot) {
        channelCache->RebuildCache(*configSnapshot, static_cast<float>(sampleRate));
        if (controller == 7 || controller == 10 || controller == 11 ||
            controller == 121) {
            voiceManager->RefreshMixGainsForChannel(
                channel, channelCache->GetParams()[channel]);
        }
    }
}

void Driver::HandleProgramChange(uint8_t channel, uint8_t program) {
    if (!channelCache || !soundFontData || channel >= kChannelCount) return;

    // Validate against the bank currently selected on this channel before
    // committing the new program. Existing voices retain their stored region.
    const uint8_t oldProgram = channelCache->GetProgram(channel);
    channelCache->ProgramChange(channel, program);

    uint32_t presetIndex = 0;
    if (ResolveChannelPreset(soundFontData, *channelCache, channel, &presetIndex)) {
        channelCache->SetSelectedPreset(channel, static_cast<uint16_t>(presetIndex));
        return;
    }

    // Invalid selection: restore the prior program and leave the prior preset
    // active, matching TSF's failed preset-selection behavior.
    channelCache->ProgramChange(channel, oldProgram);
}

void Driver::HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb) {
    if (channelCache)
        channelCache->PitchBend(channel, static_cast<int16_t>((msb << 7) | lsb));

    if (!voiceManager || !channelCache || channel >= kChannelCount) return;

    const float bendSemitones = channelCache->GetPitchBendSemitones(channel);
    const float commonRatio = powf(2.0f, bendSemitones / 12.0f);
    const uint32_t count = voiceManager->GetChannelActiveCount(channel);
    const uint32_t* handles = voiceManager->GetChannelActiveList(channel);
    for (uint32_t position = 0; position < count; ++position) {
        const uint32_t i = handles[position];
        const float scale = voiceManager->v.pitchBendScales[i];
        const float ratio = scale == 1.0f
            ? commonRatio : powf(2.0f, bendSemitones * scale / 12.0f);
        voiceManager->v.phaseIncs[i] = voiceManager->v.basePhaseIncs[i] * ratio;
    }
    voiceManager->InvalidateStealCandidates();
}

} // namespace svms

static svms::Driver* g_driver = nullptr;

extern "C" {

UINT WINAPI midiOutGetNumDevs(void) {
    LOG("midiOutGetNumDevs -> 1");
    return 1;
}

MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR uDeviceID, LPMIDIOUTCAPSA lpCaps, UINT cbCaps) {
    if (uDeviceID != 0 || !lpCaps || cbCaps < sizeof(MIDIOUTCAPSA))
        return MMSYSERR_BADDEVICEID;
    std::memset(lpCaps, 0, cbCaps);
    lpCaps->wMid = 1;
    lpCaps->wPid = 1;
    lpCaps->vDriverVersion = 0x0300;
    std::memcpy(lpCaps->szPname, "SuperVirtualMIDISynth V3", 25);
    lpCaps->wTechnology = MOD_SWSYNTH;
    lpCaps->wVoices = 64;
    lpCaps->wNotes = 64;
    lpCaps->wChannelMask = 0xFFFF;
    lpCaps->dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR uDeviceID, LPMIDIOUTCAPSW lpCaps, UINT cbCaps) {
    if (uDeviceID != 0 || !lpCaps || cbCaps < sizeof(MIDIOUTCAPSW))
        return MMSYSERR_BADDEVICEID;
    std::memset(lpCaps, 0, cbCaps);
    lpCaps->wMid = 1;
    lpCaps->wPid = 1;
    lpCaps->vDriverVersion = 0x0300;
    const wchar_t name[] = L"SuperVirtualMIDISynth V3";
    std::memcpy(lpCaps->szPname, name, sizeof(name));
    lpCaps->wTechnology = MOD_SWSYNTH;
    lpCaps->wVoices = 64;
    lpCaps->wNotes = 64;
    lpCaps->wChannelMask = 0xFFFF;
    lpCaps->dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutOpen(LPHMIDIOUT phmo, UINT uDeviceID,
    DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    LOG("midiOutOpen: uDeviceID=%u", uDeviceID);
    if (uDeviceID != 0) return MMSYSERR_BADDEVICEID;
    if (!phmo) return MMSYSERR_INVALPARAM;

    if (!g_driver || !g_driver->initialized) {
        if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
        g_driver = &svms::Driver::Instance();
        if (!g_driver->Initialize()) {
            LOG("midiOutOpen: Initialize FAILED");
            g_driver->Shutdown();
            g_driver = nullptr;
            return MMSYSERR_NOMEM;
        }
    }

    const bool loaded = g_driver->LoadConfiguredSoundFont();

    LOG("midiOutOpen: SF loaded=%d", loaded);
    g_driver->StartAudio();

    LOG("midiOutOpen: SUCCESS, returning handle");
    *phmo = reinterpret_cast<HMIDIOUT>(1);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutClose(HMIDIOUT hmo) {
    if (hmo != reinterpret_cast<HMIDIOUT>(1)) return MMSYSERR_INVALHANDLE;
    if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutShortMsg(HMIDIOUT hmo, DWORD dwMsg) {
    if (hmo != reinterpret_cast<HMIDIOUT>(1)) return MMSYSERR_INVALHANDLE;
    if (g_driver) {
        static int msgCount = 0;
        if (msgCount < 15) {
            LOG("midiOutShortMsg #%d: 0x%08X", msgCount, dwMsg);
        }
        msgCount++;
        g_driver->SubmitShortMsg(dwMsg);
    }
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutLongMsg(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmo; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutReset(HMIDIOUT hmo) {
    if (hmo != reinterpret_cast<HMIDIOUT>(1)) return MMSYSERR_INVALHANDLE;
    if (g_driver) g_driver->ResetAllVoices();
    LOG("midiOutReset: all voices released");
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmo; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmo; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetVolume(HMIDIOUT hmo, LPDWORD pdwVolume) {
    (void)hmo;
    if (!pdwVolume) return MMSYSERR_INVALPARAM;
    *pdwVolume = 0xFFFFFFFF;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutSetVolume(HMIDIOUT hmo, DWORD dwVolume) {
    (void)hmo; (void)dwVolume;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT mmrError, LPSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const char* msg = "Unknown error";
    switch (mmrError) {
        case MMSYSERR_NOERROR: msg = "No error"; break;
        case MMSYSERR_BADDEVICEID: msg = "Bad device ID"; break;
        case MMSYSERR_INVALHANDLE: msg = "Invalid handle"; break;
        case MMSYSERR_INVALPARAM: msg = "Invalid parameter"; break;
        case MMSYSERR_NOMEM: msg = "Out of memory"; break;
    }
    size_t len = std::strlen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len);
    lpText[len] = '\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetErrorTextW(MMRESULT mmrError, LPWSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const wchar_t* msg = L"Unknown error";
    switch (mmrError) {
        case MMSYSERR_NOERROR: msg = L"No error"; break;
        case MMSYSERR_BADDEVICEID: msg = L"Bad device ID"; break;
        case MMSYSERR_INVALHANDLE: msg = L"Invalid handle"; break;
        case MMSYSERR_INVALPARAM: msg = L"Invalid parameter"; break;
        case MMSYSERR_NOMEM: msg = L"Out of memory"; break;
    }
    size_t len = wcslen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len * sizeof(wchar_t));
    lpText[len] = L'\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutMessage(HMIDIOUT hmo, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hmo; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_NOERROR;
}

// ── KDMAPI / OmniMIDI extensions ──────────────────────────────────────

static bool g_kdmapiInitialized = false;

static bool EnsureDriverInitialized() {
    if (g_driver && g_driver->initialized) return true;
    if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
    g_driver = &svms::Driver::Instance();
    if (!g_driver->Initialize()) {
        LOG("KDMAPI: Initialize FAILED");
        g_driver->Shutdown();
        g_driver = nullptr;
        return false;
    }
    g_driver->LoadConfiguredSoundFont();
    g_driver->StartAudio();
    return true;
}

BOOL WINAPI IsKDMAPIAvailable(void) {
    return TRUE;
}

LPVOID WINAPI InitializeKDMAPIStream(void) {
    if (!EnsureDriverInitialized()) return nullptr;
    g_kdmapiInitialized = true;
    return reinterpret_cast<LPVOID>(1);
}

void WINAPI TerminateKDMAPIStream(void) {
    if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
    g_kdmapiInitialized = false;
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
    if (!g_kdmapiInitialized) return;
    if (g_driver) g_driver->SubmitShortMsg(dwMsg);
}

void WINAPI SendDirectDataNoBuf(DWORD dwMsg) {
    SendDirectData(dwMsg);
}

UINT WINAPI SendCustomEvent(DWORD dwEvent, LPVOID pData, DWORD dwSize) {
    (void)pData; (void)dwSize;
    if (!g_kdmapiInitialized) return 0;
    if (g_driver) g_driver->SubmitShortMsg(static_cast<DWORD>(dwEvent));
    return 1;
}

void WINAPI SendDirectLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)lpMidiHdr; (void)cbMidiHdr;
}

void WINAPI SendDirectLongDataNoBuf(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)lpMidiHdr; (void)cbMidiHdr;
}

MMRESULT WINAPI PrepareLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI UnprepareLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_NOERROR;
}

UINT WINAPI DriverSettings(DWORD dwSetting, LPVOID pValue, DWORD dwSize, LPVOID pOut) {
    (void)dwSetting; (void)pValue; (void)dwSize; (void)pOut;
    return 1;
}

DWORD WINAPI GetDriverDebugInfo(LPVOID pMem) {
    (void)pMem;
    return 0;
}

DWORD WINAPI SVMSGetDriverDebugInfoV1(LPVOID pMem, DWORD cbMem) {
    if (!pMem || cbMem < sizeof(svms::DriverDebugInfo) || !g_driver) return 0;
    svms::DriverDebugInfo info;
    g_driver->CopyDebugInfo(info);
    std::memcpy(pMem, &info, sizeof(info));
    return static_cast<DWORD>(sizeof(info));
}

BOOL WINAPI LoadCustomSoundFontsList(LPVOID pList) {
    (void)pList;
    return TRUE;
}

ULONGLONG WINAPI timeGetTime64(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (ULONGLONG)(cnt.QuadPart * 1000000ULL / freq.QuadPart);
}

// ── MIDI Input ────────────────────────────────────────────────────────

UINT WINAPI midiInGetNumDevs(void) { return 0; }

MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR uDeviceID, LPMIDIINCAPSA lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR uDeviceID, LPMIDIINCAPSW lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInOpen(LPHMIDIIN phmi, UINT uDeviceID, DWORD_PTR dwCallback,
                           DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)phmi; (void)uDeviceID; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInClose(HMIDIIN hmi) {
    (void)hmi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInPrepareHeader(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmi; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInUnprepareHeader(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmi; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInAddBuffer(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmi; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInStart(HMIDIIN hmi) {
    (void)hmi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInStop(HMIDIIN hmi) {
    (void)hmi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInReset(HMIDIIN hmi) {
    (void)hmi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInMessage(HMIDIIN hmi, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hmi; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

// ── Wave Input ─────────────────────────────────────────────────────────

UINT WINAPI waveInGetNumDevs(void) { return 0; }

MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR uDeviceID, LPWAVEINCAPSA lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR uDeviceID, LPWAVEINCAPSW lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInOpen(LPHWAVEIN phwi, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                           DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)phwi; (void)uDeviceID; (void)pwfx; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInClose(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInPrepareHeader(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInAddBuffer(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInStart(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInStop(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInReset(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInMessage(HWAVEIN hwi, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hwi; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInGetPosition(HWAVEIN hwi, LPMMTIME pmmt, UINT cbmmt) {
    (void)hwi; (void)pmmt; (void)cbmmt;
    return MMSYSERR_ERROR;
}

// ── Mixer ──────────────────────────────────────────────────────────────

UINT WINAPI mixerGetNumDevs(void) { return 0; }

MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR uMxId, LPMIXERCAPSA lpCaps, UINT cbCaps) {
    (void)uMxId; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR uMxId, LPMIXERCAPSW lpCaps, UINT cbCaps) {
    (void)uMxId; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerOpen(LPHMIXER phmx, UINT uMxId, DWORD_PTR dwCallback,
                          DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)phmx; (void)uMxId; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerClose(HMIXER hmx) {
    (void)hmx;
    return MMSYSERR_BADDEVICEID;
}

DWORD WINAPI mixerMessage(HMIXER hmx, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hmx; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineInfoA(HMIXEROBJ hmxobj, LPMIXERLINEA pmxl, DWORD fdwInfo) {
    (void)hmxobj; (void)pmxl; (void)fdwInfo;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineInfoW(HMIXEROBJ hmxobj, LPMIXERLINEW pmxl, DWORD fdwInfo) {
    (void)hmxobj; (void)pmxl; (void)fdwInfo;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetID(HMIXEROBJ hmxobj, LPUINT puMxId, DWORD fdwId) {
    (void)hmxobj; (void)puMxId; (void)fdwId;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineControlsA(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSA pmxlc, DWORD fdwControls) {
    (void)hmxobj; (void)pmxlc; (void)fdwControls;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineControlsW(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSW pmxlc, DWORD fdwControls) {
    (void)hmxobj; (void)pmxlc; (void)fdwControls;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetControlDetailsA(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetControlDetailsW(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerSetControlDetails(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

// ── Wave Output ────────────────────────────────────────────────────────

UINT WINAPI waveOutGetNumDevs(void) { return 0; }

MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR uDeviceID, LPWAVEOUTCAPSA lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR uDeviceID, LPWAVEOUTCAPSW lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutOpen(LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                            DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)phwo; (void)uDeviceID; (void)pwfx; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutClose(HWAVEOUT hwo) {
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutWrite(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutReset(HWAVEOUT hwo) {
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutRestart(HWAVEOUT hwo) {
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutPause(HWAVEOUT hwo) {
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT hwo) {
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutGetPosition(HWAVEOUT hwo, LPMMTIME pmmt, UINT cbmmt) {
    (void)hwo; (void)pmmt; (void)cbmmt;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutGetVolume(HWAVEOUT hwo, LPDWORD pdwVolume) {
    (void)hwo;
    if (pdwVolume) *pdwVolume = 0xFFFFFFFF;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetVolume(HWAVEOUT hwo, DWORD dwVolume) {
    (void)hwo; (void)dwVolume;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetPitch(HWAVEOUT hwo, LPDWORD pdwPitch) {
    (void)hwo;
    if (pdwPitch) *pdwPitch = 0x00010000;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetPitch(HWAVEOUT hwo, DWORD dwPitch) {
    (void)hwo; (void)dwPitch;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetPlaybackRate(HWAVEOUT hwo, LPDWORD pdwRate) {
    (void)hwo;
    if (pdwRate) *pdwRate = 0x00010000;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetPlaybackRate(HWAVEOUT hwo, DWORD dwRate) {
    (void)hwo; (void)dwRate;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetID(HWAVEOUT hwo, LPUINT puDeviceID) {
    (void)hwo;
    if (puDeviceID) *puDeviceID = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT mmrError, LPSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const char* msg = "Unknown error";
    size_t len = std::strlen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len);
    lpText[len] = '\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT mmrError, LPWSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const wchar_t* msg = L"Unknown error";
    size_t len = wcslen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len * sizeof(wchar_t));
    lpText[len] = L'\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutMessage(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hwo; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

// ── Multimedia Timer ───────────────────────────────────────────────────

static LARGE_INTEGER g_timeFreq;
static BOOL g_timeInitialized = FALSE;

DWORD WINAPI timeGetTime(void) {
    if (!g_timeInitialized) {
        QueryPerformanceFrequency(&g_timeFreq);
        g_timeInitialized = TRUE;
    }
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return (DWORD)(cnt.QuadPart * 1000ULL / g_timeFreq.QuadPart);
}

MMRESULT WINAPI timeBeginPeriod(UINT uPeriod) {
    (void)uPeriod;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeEndPeriod(UINT uPeriod) {
    (void)uPeriod;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc) {
    if (!ptc || cbtc < sizeof(TIMECAPS))
        return TIMERR_NOCANDO;
    ptc->wPeriodMin = 1;
    ptc->wPeriodMax = 65535;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeSetEvent(UINT uDelay, UINT uResolution,
                             void (CALLBACK *fptc)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR),
                             DWORD_PTR dwUser, UINT fuEvent) {
    (void)uDelay; (void)uResolution; (void)fptc; (void)dwUser; (void)fuEvent;
    return 0;
}

MMRESULT WINAPI timeKillEvent(UINT uTimerID) {
    (void)uTimerID;
    return TIMERR_NOERROR;
}

// ── DLL Entry ──────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        LogInit();
        LOG("DLL_PROCESS_ATTACH");
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        LOG("DLL_PROCESS_DETACH");
    }
    return TRUE;
}

} // extern "C"
