#include <windows.h>
#include <mmeapi.h>
#include <timeapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>

#include "SVMSAudioOutput.h"
#include "SVMSVoiceManager.h"
#include "SVMSChannelCache.h"
#include "SVMSRenderScalar.h"
#include "SVMSSoundFont.h"
#include "SVMSConfig.h"
#include "SVMSPSCQueue.h"

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

struct LimiterState {
    static constexpr uint32_t kDelayFrames = 128;
    static constexpr float kAttackCoeff = 0.01f;
    static constexpr float kReleaseCoeff = 0.001f;
    static constexpr float kThreshold = 0.9f;
    static constexpr float kEpsilon = 0.0001f;
    static constexpr float kSoftClipGain = 1.2f;

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

            if (dL > 1.0f) dL = 1.0f; if (dL < -1.0f) dL = -1.0f;
            if (dR > 1.0f) dR = 1.0f; if (dR < -1.0f) dR = -1.0f;

            interleaved[f * channels] = dL;
            if (channels > 1) interleaved[f * channels + 1] = dR;

            delayWritePos = (delayWritePos + 1) % kDelayFrames;
        }
    }
};

class Driver {
public:
    static Driver& Instance();

    bool Initialize();
    void Shutdown();
    bool LoadSoundFont(const char* path);
    void StartAudio();
    void ResetAllVoices();
    bool IsInitialized() const;

    void SubmitShortMsg(uint32_t msg);

    bool initialized;
    uint32_t sampleRate;
    uint32_t bufferFrames;

private:
    Driver();
    ~Driver();

    static void RenderCallback(float* output, uint32_t numFrames, void* userData);

    // EventDispatcher callback — invoked by RenderScalar at each event's
    // exact sub-sample position during RenderBlock.
    static void DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                     void* userData);

    void HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, float fractionalOffset);
    void HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset);
    void HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    void HandleProgramChange(uint8_t channel, uint8_t program);
    void HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb);

    // Lock-free SPSC queue: MIDI host thread pushes, audio render thread pops.
    // Capacity 16384 (power of two) = 128 KB at 8 bytes/event.
    SPSCQueue<TimestampedMidiEvent, 16384> midiEventQueue_;

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
    svms::RenderEvent* pendingEventBuffer;
    uint32_t pendingEventCount;

    // Virtual render clock: monotonic QPC counter that tracks the audio
    // timeline.  Advances by (numFrames * qpcFreq / sampleRate) each audio
    // callback rather than using the wall-clock blockStartQPC.  This
    // ensures events pushed during the previous block's rendering window
    // are positioned with sub-block precision instead of all snapping to
    // sample 0 (the 100 Hz buffer-grid buzz).
    uint64_t virtualRenderClockQPC;
    bool clockInitialized;

    CRITICAL_SECTION cs;
};

static Driver* s_instance = nullptr;

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
      eventBuffer(nullptr), pendingEventBuffer(nullptr), pendingEventCount(0),
      virtualRenderClockQPC(0), clockInitialized(false) {
    limiter.Reset();
    eventBuffer = static_cast<svms::RenderEvent*>(
        _aligned_malloc(sizeof(svms::RenderEvent) * kEventBufferCapacity, 64));
    pendingEventBuffer = static_cast<svms::RenderEvent*>(
        _aligned_malloc(sizeof(svms::RenderEvent) * kEventBufferCapacity, 64));
    InitializeCriticalSection(&cs);
}

Driver::~Driver() {
    Shutdown();
    if (eventBuffer) { _aligned_free(eventBuffer); eventBuffer = nullptr; }
    if (pendingEventBuffer) { _aligned_free(pendingEventBuffer); pendingEventBuffer = nullptr; }
    DeleteCriticalSection(&cs);
}

bool Driver::Initialize() {
    if (initialized) return true;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFreq = freq.QuadPart;

    EngineConfig cfg = EngineConfig::Default();
    if (!cfg.Validate()) { LOG("EngineConfig validation failed"); return false; }

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

    bufferCapacity = 4096;
    leftBuffer = static_cast<float*>(malloc(bufferCapacity * sizeof(float)));
    rightBuffer = static_cast<float*>(malloc(bufferCapacity * sizeof(float)));
    if (!leftBuffer || !rightBuffer) {
        LOG("FAILED: Could not allocate render buffers");
        return false;
    }

    voiceManager = new VoiceManager();
    voiceManager->Initialize(cfg.maxVoices, sampleRate);
    LOG("VoiceManager initialized, maxVoices=%u", cfg.maxVoices);

    channelCache = new ChannelCache();
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

    audioOutput->SetRenderCallback(RenderCallback, this);

    initialized = true;
    LOG("Initialize SUCCESS");
    return true;
}

void Driver::Shutdown() {
    if (audioOutput) {
        audioOutput->Stop();
        audioOutput->Shutdown();
        delete audioOutput;
        audioOutput = nullptr;
    }
    delete voiceManager; voiceManager = nullptr;
    delete channelCache; channelCache = nullptr;
    delete renderScalar; renderScalar = nullptr;
    delete configSnapshot; configSnapshot = nullptr;
    free(leftBuffer); leftBuffer = nullptr;
    free(rightBuffer); rightBuffer = nullptr;
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

    initialized = false;
}

bool Driver::LoadSoundFont(const char* path) {
    if (!path) return false;
    LOG("LoadSoundFont: \"%s\"", path);

    EnterCriticalSection(&cs);

    SF2Data* sf2 = new SF2Data();
    if (!sf2_load(path, sf2)) {
        LOG("  sf2_load FAILED: presets=%u inst=%u samples=%u sampleData=%d frames=%u",
            sf2->presetCount, sf2->instrumentCount, sf2->sampleCount,
            sf2->sampleData ? 1 : 0, sf2->sampleDataFrames);
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
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

    if (soundFontData) {
        free(sampleDataStore); sampleDataStore = nullptr;
        free(samplesStore); samplesStore = nullptr;
        sf2_free(soundFontData);
        delete soundFontData;
    }
    soundFontData = sf2;

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

void Driver::StartAudio() {
    if (audioOutput && !audioOutput->IsRunning()) {
        LOG("StartAudio: starting WASAPI stream...");
        bool ok = audioOutput->Start();
        LOG("StartAudio: %s", ok ? "SUCCESS" : "FAILED");
    }
}

void Driver::ResetAllVoices() {
    if (voiceManager) voiceManager->Reset();
    midiEventQueue_.Reset();
    pendingEventCount = 0;
    virtualRenderClockQPC = 0;
    clockInitialized = false;
}

void Driver::SubmitShortMsg(uint32_t msg) {
    TimestampedMidiEvent evt;
    evt.message = msg;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&evt.qpcTimestamp));
    evt.targetSampleOffset = 0.0f; // computed during drain

    // Spin-then-drop backpressure: if the SPSC queue is full (audio thread
    // is behind), spin briefly to let the audio thread drain events.  At
    // most ~0.5ms of spinning.  If still full after backoff, drop the event.
    // The SPSC queue at 16384 entries is the first backpressure line; the
    // persistent pending queue on the audio thread is the second.
    static constexpr int kMaxSpins = 256;
    for (int spin = 0; spin < kMaxSpins; ++spin) {
        if (midiEventQueue_.Push(evt)) return;
        YieldProcessor();
        if ((spin & 15) == 15) {
            SwitchToThread();
        }
    }
    LOG("SubmitShortMsg: SPSC QUEUE FULL after backoff, dropping msg 0x%08X", msg);
}

void Driver::RenderCallback(float* output, uint32_t numFrames, void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self || !self->initialized) return;

    VoiceManager* vm = self->voiceManager;
    ChannelCache* cc = self->channelCache;
    RenderScalar* render = self->renderScalar;
    RuntimeConfigSnapshot* snap = self->configSnapshot;
    const float* sd = self->sampleDataStore;

    if (!vm || !cc || !render || !snap) return;

    cc->RebuildCache(*snap, static_cast<float>(self->sampleRate));

    if (!self->leftBuffer || !self->rightBuffer) {
        std::memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }
    float* leftBuf = self->leftBuffer;
    float* rightBuf = self->rightBuffer;
    std::memset(leftBuf, 0, numFrames * sizeof(float));
    std::memset(rightBuf, 0, numFrames * sizeof(float));

    if (!sd) {
        for (uint32_t i = 0; i < numFrames; ++i) {
            output[i * 2] = 0.0f;
            output[i * 2 + 1] = 0.0f;
        }
        self->limiter.Process(output, numFrames, 2);
        return;
    }

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

    const float sr = static_cast<float>(self->sampleRate);
    const float freq = static_cast<float>(self->qpcFreq);
    const float numFramesF = static_cast<float>(numFrames);

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
    const float blockAdvanceQPC = numFramesF * freq / sr;

    RenderEvent* pending = self->pendingEventBuffer;
    uint32_t& pendingCount = self->pendingEventCount;

    // ── Step 1: Drain ALL SPSC events → append to pending queue ─────────
    TimestampedMidiEvent timed;
    while (self->midiEventQueue_.TryPop(timed)) {
        if (pendingCount >= kEventBufferCapacity) {
            LOG("RenderCallback: pending queue full, events stay in SPSC");
            break;
        }

        int64_t deltaQPC = static_cast<int64_t>(timed.qpcTimestamp - self->virtualRenderClockQPC);
        float off = static_cast<float>(deltaQPC) * sr / freq;

        uint32_t msg = timed.message;
        uint8_t st = static_cast<uint8_t>(msg & 0xFF);
        uint8_t d1 = static_cast<uint8_t>((msg >> 8) & 0xFF);
        uint8_t d2 = static_cast<uint8_t>((msg >> 16) & 0xFF);
        uint8_t ch = st & 0x0F;

        RenderEventType etype;
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

        RenderEvent ev;
        ev.type = etype;
        ev.channel = ch;
        ev.data1 = d1;
        ev.data2 = d2;
        ev.sampleOffset = off;

        pending[pendingCount++] = ev;
    }

    // ── Step 2: Sort pending queue by sampleOffset ─────────────────────
    // Insertion sort.  The carry-forward portion (from previous blocks) is
    // already sorted (offset decrement preserves relative order).  Newly
    // appended SPSC events at the tail are the only unsorted elements.
    // For mostly-sorted data this is O(N) with a small constant.
    for (uint32_t i = 1; i < pendingCount; ++i) {
        RenderEvent key = pending[i];
        int32_t j = static_cast<int32_t>(i) - 1;
        while (j >= 0 && pending[j].sampleOffset > key.sampleOffset) {
            pending[j + 1] = pending[j];
            j--;
        }
        pending[j + 1] = key;
    }

    // ── Step 3: Extract all in-block events for this block ──────────────
    // Walk the sorted pending queue from offset 0.  Take every event with
    // sampleOffset < numFrames into evtBuf.  Stop at the first future
    // event (offset >= numFrames).  With in-place voice recycling keeping
    // active polyphony bounded to ~128-256 voices, the engine can process
    // all incoming events in real-time without batch-chunking artifacts.
    RenderEvent* evtBuf = self->eventBuffer;
    uint32_t evCount = 0;
    uint32_t kept = 0;

    for (uint32_t i = 0; i < pendingCount; ++i) {
        float off = pending[i].sampleOffset;

        if (off >= numFramesF) {
            for (uint32_t j = i; j < pendingCount; ++j) {
                pending[kept++] = pending[j];
            }
            break;
        }

        if (off < 0.0f) off = 0.0f;

        RenderEvent ev = pending[i];
        ev.sampleOffset = off;

        if (evCount < kEventBufferCapacity) {
            evtBuf[evCount++] = ev;
        }
    }
    pendingCount = kept;

    // ── Step 4: Decrement remaining pending offsets by numFrames ────────
    // Smooth rollover: future events advance at the true audio clock rate
    // toward their firing time without frame-boundary re-quantization.
    for (uint32_t i = 0; i < pendingCount; ++i) {
        pending[i].sampleOffset -= numFramesF;
    }

    // ── Step 5: Sort evtBuf by sampleOffset (defensive) ─────────────────
    for (uint32_t i = 1; i < evCount; ++i) {
        RenderEvent key = evtBuf[i];
        int32_t j = static_cast<int32_t>(i) - 1;
        while (j >= 0 && evtBuf[j].sampleOffset > key.sampleOffset) {
            evtBuf[j + 1] = evtBuf[j];
            j--;
        }
        evtBuf[j + 1] = key;
    }

    // ── Step 6: Render with sub-sample event slicing ───────────────────
    // RenderBlock will invoke DispatchRenderEvent at each event's exact
    // fractional sample offset.  All event handling (voice allocation,
    // release, CC updates) happens inside the render loop via the callback.
    render->RenderBlock(*vm, *cc, sd, self->sampleDataFrames,
                        leftBuf, rightBuf, numFrames, *snap,
                        evtBuf, evCount);

    // ── Advance virtual render clock for the next callback ──────────
    self->virtualRenderClockQPC += static_cast<uint64_t>(blockAdvanceQPC + 0.5f);

    // Interleave planar L/R into the interleaved output buffer
    for (uint32_t i = 0; i < numFrames; ++i) {
        output[i * 2]     = leftBuf[i] * snap->masterVolume;
        output[i * 2 + 1] = rightBuf[i] * snap->masterVolume;
    }

    self->limiter.Process(output, numFrames, 2);
}

// ── EventDispatcher callback ─────────────────────────────────────────────
// Called by RenderScalar::RenderBlock at each event's exact sub-sample
// position.  Voice allocation, release, CC updates all happen here so
// that phaseOffset and releaseStartInBlock are set at the precise frame.
void Driver::DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                  void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self) return;

    switch (event.type) {
        case RenderEventType::NoteOn: {
            // Compute the fractional phase offset for sub-sample accuracy.
            // The voice starts reading from this fractional position into
            // its first sample, eliminating quantization to integer boundaries.
            float fractionalOffset = event.sampleOffset - static_cast<float>(blockCursor);
            if (fractionalOffset < 0.0f) fractionalOffset = 0.0f;
            if (fractionalOffset >= 1.0f) fractionalOffset = 0.0f;
            self->HandleNoteOn(event.channel, event.data1, event.data2,
                                fractionalOffset);
            break;
        }
        case RenderEventType::NoteOff:
            self->HandleNoteOff(event.channel, event.data1, blockCursor);
            break;
        case RenderEventType::ControlChange:
            self->HandleControlChange(event.channel, event.data1, event.data2);
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
    }
}

void Driver::HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, float fractionalOffset) {
    if (!channelCache || !voiceManager) return;

    static int noteOnCount = 0;
    if (noteOnCount < 50) {
        LOG("HandleNoteOn: ch=%u note=%u vel=%u regionCount=%u",
            channel, note, velocity,
            soundFontData ? soundFontData->regionCount : 0);
    }
    noteOnCount++;

    channelCache->NoteOn(channel, note, velocity);

    if (!soundFontData || !samplesStore || !sampleDataStore) return;

    uint16_t bank = 0;
    uint16_t program = channelCache->GetProgram(channel);

    uint32_t presetIndex = 0;
    if (!sf2_find_preset(soundFontData, bank, program, &presetIndex)) {
        sf2_find_preset(soundFontData, 0, 0, &presetIndex);
    }

    static constexpr uint32_t kMaxMatchingRegions = 16;
    const SFSampleRegion* matchingRegions[kMaxMatchingRegions];
    uint32_t matchCount = 0;

    for (uint32_t ri = 0; ri < soundFontData->regionCount && matchCount < kMaxMatchingRegions; ++ri) {
        const SFSampleRegion& r = soundFontData->regions[ri];
        if (r.presetIndex != static_cast<uint16_t>(presetIndex)) continue;
        if (note >= r.keyLo && note <= r.keyHi && velocity >= r.velLo && velocity <= r.velHi) {
            matchingRegions[matchCount++] = &r;
        }
    }

    if (matchCount == 0) {
        int bestDist = 999;
        const SFSampleRegion* bestRegion = nullptr;
        for (uint32_t ri = 0; ri < soundFontData->regionCount; ++ri) {
            const SFSampleRegion& r = soundFontData->regions[ri];
            if (r.presetIndex != static_cast<uint16_t>(presetIndex)) continue;
            if (velocity >= r.velLo && velocity <= r.velHi) {
                int dist = 0;
                if (note < r.keyLo) dist = r.keyLo - note;
                else if (note > r.keyHi) dist = note - r.keyHi;
                if (dist < bestDist) {
                    bestDist = dist;
                    bestRegion = &r;
                }
            }
        }
        if (bestRegion && matchCount < kMaxMatchingRegions) {
            matchingRegions[matchCount++] = bestRegion;
        }
    }

    if (matchCount == 0) return;

    float sr = (float)(sampleRate > 0 ? sampleRate : 44100u);
    float pitchBendSemitones = channelCache->GetPitchBendSemitones(channel);

    for (uint32_t mi = 0; mi < matchCount; ++mi) {
        const SFSampleRegion* matchedRegion = matchingRegions[mi];
        uint32_t sampleIndex = matchedRegion->sampleIndex;
        const SF2Sample& samp = samplesStore[sampleIndex];

        VoiceHandle vh;
        bool wasStolen = false;

        vh = voiceManager->AllocateVoiceOrSteal(channel, note, velocity, &wasStolen);

        if (vh == kInvalidVoice) {
            LOG("HandleNoteOn: VOICE ALLOC FAILED ch=%u note=%u vel=%u active=%u",
                channel, note, velocity, voiceManager->GetActiveCount());
            break;
        }

        int effRootKey = (matchedRegion->rootKey >= 0)
            ? matchedRegion->rootKey : (int)samp.originalPitch;
        float rootKey = (float)effRootKey;
        float coarseTune = (float)matchedRegion->coarseTune;
        float fineTune = (float)matchedRegion->fineTune / 100.0f;
        float keyTrack = (matchedRegion->scaleTuning != 0)
            ? (float)matchedRegion->scaleTuning : 100.0f;

        float noteTuneSemitones = coarseTune + fineTune;
        float adjustedPitch = rootKey +
            (((float)note + noteTuneSemitones + pitchBendSemitones - rootKey) * (keyTrack / 100.0f));
        float semitoneOffset = adjustedPitch - rootKey;
        float pitchRatio = powf(2.0f, semitoneOffset / 12.0f);

        float srcRate = (float)(samp.sampleRate > 0 ? samp.sampleRate : 44100u);
        float outRate = (float)(sampleRate > 0 ? sampleRate : 44100u);
        float phaseStep = (outRate > 0.0f && srcRate > 0.0f) ? (srcRate / outRate) * pitchRatio : pitchRatio;

        uint32_t sStart = static_cast<uint32_t>(matchedRegion->startOffset);
        uint32_t sEnd = static_cast<uint32_t>(matchedRegion->endOffset);
        uint32_t sLoopStart = static_cast<uint32_t>(matchedRegion->loopStartOffset);
        uint32_t sLoopEnd = static_cast<uint32_t>(matchedRegion->loopEndOffset);
        uint8_t loopMode = matchedRegion->loopMode;

        voiceManager->SetVoiceSample(vh, sStart, sEnd, sLoopStart, sLoopEnd, loopMode, phaseStep, 1);

        float velGain = velocity / 127.0f;
        float initialGain = velGain;

        if (matchedRegion->initialAttenuation > 0) {
            float attenDb = matchedRegion->initialAttenuation / 10.0f;
            initialGain *= powf(10.0f, -attenDb / 20.0f);
        }

        float sustainLevel = (float)matchedRegion->sustainVolEnv / 1000.0f;
        if (sustainLevel > 1.0f) sustainLevel = 1.0f;

        auto tc2sec = [](int16_t tc) -> float {
            if (tc <= -12000) return 0.0f;
            return powf(2.0f, (float)tc / 1200.0f);
        };

        float delaySeconds = tc2sec(matchedRegion->delayVolEnv);
        float holdSeconds = tc2sec(matchedRegion->holdVolEnv);
        float attackSeconds = tc2sec(matchedRegion->attackVolEnv);
        float decaySeconds = tc2sec(matchedRegion->decayVolEnv);
        float releaseSeconds = tc2sec(matchedRegion->releaseVolEnv);

        uint32_t delaySamples = (delaySeconds > 0.0f) ? (uint32_t)(delaySeconds * sr) : 0u;
        uint32_t holdSamples = (holdSeconds > 0.0f) ? (uint32_t)(holdSeconds * sr) : 0u;
        uint32_t attackSamples = (attackSeconds > 0.0001f) ? (uint32_t)(attackSeconds * sr) : 0u;
        uint32_t decaySamples = (decaySeconds > 0.0001f) ? (uint32_t)(decaySeconds * sr) : 0u;

        // Micro-fade for stolen voices: force a 64-sample (~1.5ms @
        // 44.1kHz) attack ramp.  The ramp from currentGain=0 to
        // targetGain over 64 samples provides a perceptually smooth
        // onset without a click.
        static constexpr uint32_t kMicroFadeAttackSamples = 64;

        if (wasStolen) {
            delaySamples = 0;
            holdSamples = 0;
            attackSamples = kMicroFadeAttackSamples;
        }

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

        voiceManager->SetVoiceEnvelope(vh, initialGain, sustainLevel,
                                        delaySamples, holdSamples, attackSamples,
                                        decaySamples, attackGainStep, decaySlope, releaseDecay);
        voiceManager->SetVoiceGain(vh, 1.0f, 1.0f);

        voiceManager->v.phaseOffset[vh] = fractionalOffset;
    }
}

void Driver::HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset) {
    if (!channelCache || !voiceManager) return;

    bool sustain = channelCache->IsSustainActive(channel);

    channelCache->NoteOff(channel, note);

    // Adaptive release: scales down under pool pressure so slots free up
    // sooner for incoming notes, but is floored at kMinReleaseSeconds
    // (10-15ms). The previous flat 16-sample (~0.37ms) micro-fade cut
    // voices off before a single oscillation cycle could complete (e.g.
    // ~169 samples for Middle C at 44.1kHz), which is why dense chord
    // slams rendered as muted, silent, or clicked instead of audible notes.
    float pressure = voiceManager->GetMaxVoices() > 0
        ? (float)voiceManager->GetActiveCount() / (float)voiceManager->GetMaxVoices()
        : 0.0f;
    float releaseSeconds = ComputeAdaptiveReleaseSeconds(pressure,
                                                          0.015f,  // >90% full -> 15ms
                                                          0.050f,  // >70% full -> 50ms
                                                          0.150f,  // >50% full -> 150ms
                                                          0.500f); // otherwise  -> 500ms
    float releaseDecay = MakeReleaseDecay(releaseSeconds, sampleRate);

    for (uint32_t i = 0; i < voiceManager->GetMaxVoices(); ++i) {
        if (voiceManager->IsActive(i) && voiceManager->v.channel[i] == channel
            && voiceManager->v.note[i] == note) {
            if (sustain) {
                voiceManager->v.heldBySustain[i] = 1;
            } else {
                voiceManager->v.releaseDecay[i] = releaseDecay;
                voiceManager->v.releaseStartInBlock[i] = blockOffset;
                voiceManager->StartRelease(i);
            }
        }
    }
}

void Driver::HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (channelCache) channelCache->ControlChange(channel, controller, value);

    static int ccLogCount = 0;
    if (ccLogCount < 20) {
        LOG("HandleControlChange: ch=%u cc=%u val=%u", channel, controller, value);
        ccLogCount++;
    }

    if (controller == 64) {
        if (value < 64) {
            for (uint32_t i = 0; i < voiceManager->GetMaxVoices(); ++i) {
                if (voiceManager->IsActive(i) && voiceManager->v.channel[i] == channel
                    && voiceManager->v.heldBySustain[i]) {
                    voiceManager->v.heldBySustain[i] = 0;
                    voiceManager->StartRelease(i);
                }
            }
        }
    }

    if (controller == 120 || controller == 123) {
        for (uint32_t i = 0; i < voiceManager->GetMaxVoices(); ++i) {
            if (!voiceManager->IsActive(i)) continue;
            if (controller == 123 && voiceManager->v.channel[i] != channel) continue;
            if (controller == 120 || voiceManager->v.channel[i] == channel) {
                voiceManager->StartRelease(i);
            }
        }
    }
}

void Driver::HandleProgramChange(uint8_t channel, uint8_t program) {
    if (channelCache) channelCache->ProgramChange(channel, program);
}

void Driver::HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb) {
    if (channelCache)
        channelCache->PitchBend(channel, static_cast<int16_t>((msb << 7) | lsb));

    static int pbLogCount = 0;
    if (pbLogCount < 10) {
        LOG("HandlePitchBend: ch=%u lsb=%u msb=%u val=%u", channel, lsb, msb, (msb << 7) | lsb);
        pbLogCount++;
    }

    if (!voiceManager || !samplesStore) return;

    float pitchBendSemitones = channelCache->GetPitchBendSemitones(channel);

    for (uint32_t i = 0; i < voiceManager->GetMaxVoices(); ++i) {
        if (!voiceManager->IsActive(i)) continue;
        if (voiceManager->v.channel[i] != channel) continue;

        uint8_t note = voiceManager->v.note[i];
        uint8_t vel = voiceManager->v.velocity[i];

        uint32_t sampleIndex = 0;
        const SFSampleRegion* matchedRegion = nullptr;
        bool foundRegion = false;
        for (uint32_t ri = 0; ri < soundFontData->regionCount; ++ri) {
            const SFSampleRegion& r = soundFontData->regions[ri];
            if (note >= r.keyLo && note <= r.keyHi && vel >= r.velLo && vel <= r.velHi) {
                sampleIndex = r.sampleIndex;
                matchedRegion = &r;
                foundRegion = true;
                break;
            }
        }
        if (!foundRegion) continue;

        const SF2Sample& samp = samplesStore[sampleIndex];
        int effRootKey = (matchedRegion && matchedRegion->rootKey >= 0)
            ? matchedRegion->rootKey : (int)samp.originalPitch;
        float rootKey = (float)effRootKey;
        float coarseTune = (float)(matchedRegion ? matchedRegion->coarseTune : 0);
        float fineTune = (float)(matchedRegion ? matchedRegion->fineTune : 0) / 100.0f;
        float keyTrack = (matchedRegion && matchedRegion->scaleTuning != 0)
            ? (float)matchedRegion->scaleTuning : 100.0f;

        float noteTuneSemitones = coarseTune + fineTune;
        float adjustedPitch = rootKey +
            (((float)note + noteTuneSemitones + pitchBendSemitones - rootKey) * (keyTrack / 100.0f));
        float semitoneOffset = adjustedPitch - rootKey;
        float pitchRatio = powf(2.0f, semitoneOffset / 12.0f);

        float srcRate = (float)(samp.sampleRate > 0 ? samp.sampleRate : 44100u);
        float outRate = (float)(sampleRate > 0 ? sampleRate : 44100u);
        float phaseStep = (outRate > 0.0f && srcRate > 0.0f) ? (srcRate / outRate) * pitchRatio : pitchRatio;

        voiceManager->v.phaseIncs[i] = phaseStep;
    }
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

    char sfPath[MAX_PATH];
    bool loaded = false;

    GetModuleFileNameA(nullptr, sfPath, MAX_PATH);
    char* lastSlash = strrchr(sfPath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = 0;
    } else {
        sfPath[0] = 0;
    }

    strcat_s(sfPath, MAX_PATH, "gm.sf2");
    LOG("midiOutOpen: trying \"%s\"", sfPath);
    loaded = g_driver->LoadSoundFont(sfPath);

    if (!loaded) {
        LOG("midiOutOpen: gm.sf2 not loaded, trying gm.dls fallback");
        loaded = g_driver->LoadSoundFont("C:\\Windows\\System32\\drivers\\gm.dls");
    }

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
    char sfPath[MAX_PATH];
    GetModuleFileNameA(nullptr, sfPath, MAX_PATH);
    char* lastSlash = strrchr(sfPath, '\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    else sfPath[0] = 0;
    strcat_s(sfPath, MAX_PATH, "gm.sf2");
    if (!g_driver->LoadSoundFont(sfPath)) {
        g_driver->LoadSoundFont("C:\\Windows\\System32\\drivers\\gm.dls");
    }
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
