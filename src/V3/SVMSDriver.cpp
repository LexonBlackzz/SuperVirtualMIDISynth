// Driver core: singleton, init/shutdown, audio device lifecycle.

#include "SVMSDriverInternal.h"

namespace svms {

static uint32_t SelectRenderLanesForPhysicalCores(uint32_t cores) {
    // Dense tile rendering streams sample data through the shared memory
    // hierarchy from every lane; past roughly cores-2 lanes, SMT siblings
    // and per-chunk join overhead cost more than they add (measured on an
    // 8C/16T part: throughput peaks at 6 lanes and falls monotonically to
    // 16). Reserving two physical lanes also leaves headroom for the
    // audio/WASAPI thread and OS. SVMS_RENDER_THREADS overrides this.
    return cores > 2u ? (std::min)(16u, cores - 2u) : 1u;
}

static uint32_t SelectAutomaticRenderThreadCount() {
#if defined(SVMS_XP_COMPAT)
    return 1u;
#else
    // Prefer one renderer lane per physical core. On hybrid processors the
    // CPU-set efficiency class lets us select only the fastest core class;
    // older systems fall back to the physical-core topology, then to a
    // conservative logical-processor count.
    using GetCpuSetsProc = BOOL (WINAPI*)(
        PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    GetCpuSetsProc getCpuSets = kernel32
        ? reinterpret_cast<GetCpuSetsProc>(
              GetProcAddress(kernel32, "GetSystemCpuSetInformation"))
        : nullptr;
    if (getCpuSets) {
        ULONG bytes = 0u;
        getCpuSets(nullptr, 0u, &bytes, nullptr, 0u);
        unsigned char* storage = bytes != 0u
            ? static_cast<unsigned char*>(std::malloc(bytes)) : nullptr;
        if (storage && getCpuSets(
                reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage), bytes,
                &bytes, nullptr, 0u)) {
            BYTE fastestClass = 0u;
            for (ULONG offset = 0u; offset < bytes;) {
                const auto* info = reinterpret_cast<
                    const SYSTEM_CPU_SET_INFORMATION*>(storage + offset);
                if (info->Size == 0u || offset + info->Size > bytes) break;
                if (info->Type == CpuSetInformation)
                    fastestClass = (std::max)(
                        fastestClass, info->CpuSet.EfficiencyClass);
                offset += info->Size;
            }
            uint16_t cores[64]{};
            uint32_t coreCount = 0u;
            for (ULONG offset = 0u; offset < bytes && coreCount < 16u;) {
                const auto* info = reinterpret_cast<
                    const SYSTEM_CPU_SET_INFORMATION*>(storage + offset);
                if (info->Size == 0u || offset + info->Size > bytes) break;
                if (info->Type == CpuSetInformation &&
                    info->CpuSet.EfficiencyClass == fastestClass) {
                    const uint16_t key = static_cast<uint16_t>(
                        (static_cast<uint16_t>(info->CpuSet.Group) << 8u) |
                        info->CpuSet.CoreIndex);
                    bool found = false;
                    for (uint32_t index = 0u; index < coreCount; ++index)
                        found |= cores[index] == key;
                    if (!found) cores[coreCount++] = key;
                }
                offset += info->Size;
            }
        std::free(storage);
        if (coreCount != 0u)
            return SelectRenderLanesForPhysicalCores(coreCount);
        } else {
            std::free(storage);
        }
    }

    DWORD bytes = 0u;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    auto* topology = bytes != 0u
        ? static_cast<unsigned char*>(std::malloc(bytes)) : nullptr;
    if (topology && GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                topology), &bytes)) {
        uint32_t cores = 0u;
        for (DWORD offset = 0u; offset < bytes;) {
            const auto* info = reinterpret_cast<
                const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                    topology + offset);
            if (info->Size == 0u || offset + info->Size > bytes) break;
            if (info->Relationship == RelationProcessorCore) ++cores;
            offset += info->Size;
        }
        std::free(topology);
        if (cores != 0u)
            return SelectRenderLanesForPhysicalCores(
                (std::min)(16u, cores));
    } else {
        std::free(topology);
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    return (std::max)(1u, (std::min)(8u,
        static_cast<uint32_t>(systemInfo.dwNumberOfProcessors)));
#endif
}

static size_t EstimateRuntimeVoiceMemoryBytes(
    uint32_t voiceCapacity, uint32_t renderThreads,
    uint32_t maximumBlockFrames) noexcept {
    const size_t manager = VoiceManager::EstimateAllocatedBytes(voiceCapacity);
    const size_t renderer = RenderScalar::EstimateAllocatedBytes(
        voiceCapacity, renderThreads, maximumBlockFrames);
    if (manager == (std::numeric_limits<size_t>::max)() ||
        renderer > (std::numeric_limits<size_t>::max)() - manager)
        return (std::numeric_limits<size_t>::max)();
    return manager + renderer;
}

static uint32_t LargestVoiceCapacityInRange(
    uint64_t budgetBytes, uint32_t renderThreads, uint32_t blockFrames,
    uint32_t first, uint32_t last) noexcept {
    if (first > last ||
        EstimateRuntimeVoiceMemoryBytes(first, renderThreads, blockFrames) >
            budgetBytes)
        return 0u;
    uint32_t low = first;
    uint32_t high = last;
    while (low < high) {
        const uint32_t middle = low + (high - low + 1u) / 2u;
        if (EstimateRuntimeVoiceMemoryBytes(
                middle, renderThreads, blockFrames) <= budgetBytes)
            low = middle;
        else
            high = middle - 1u;
    }
    return low;
}

static uint32_t LargestInitialVoiceCapacityForBudget(
    uint64_t budgetBytes, uint32_t renderThreads,
    uint32_t blockFrames) noexcept {
    // Dense-planner storage exists only through 8,192 voices, so the estimate
    // has one intentional downward step at 8,193. Search both monotonic ranges
    // instead of assuming a globally monotonic function.
    const uint32_t highRange = LargestVoiceCapacityInRange(
        budgetBytes, renderThreads, blockFrames,
        kDenseRenderMaximumVoices + 1u, kMaxPolyphony);
    if (highRange != 0u) return highRange;
    return LargestVoiceCapacityInRange(
        budgetBytes, renderThreads, blockFrames, 1u,
        kDenseRenderMaximumVoices);
}

static size_t EstimateRuntimeVoiceMemoryAfterGrowth(
    uint32_t initialCapacity, uint32_t grownCapacity,
    uint32_t renderThreads, uint32_t maximumBlockFrames) noexcept {
    if (grownCapacity < initialCapacity) grownCapacity = initialCapacity;
    const size_t manager = VoiceManager::EstimateAllocatedBytes(grownCapacity);
    const size_t initialRenderer = RenderScalar::EstimateAllocatedBytes(
        initialCapacity, renderThreads, maximumBlockFrames);
    const size_t initialSerialRenderer = RenderScalar::EstimateAllocatedBytes(
        initialCapacity, 1u, maximumBlockFrames);
    const size_t grownSerialRenderer = RenderScalar::EstimateAllocatedBytes(
        grownCapacity, 1u, maximumBlockFrames);
    const size_t scratchGrowth = grownSerialRenderer >= initialSerialRenderer
        ? grownSerialRenderer - initialSerialRenderer : 0u;
    if (initialRenderer > (std::numeric_limits<size_t>::max)() - manager ||
        scratchGrowth > (std::numeric_limits<size_t>::max)() -
                            manager - initialRenderer)
        return (std::numeric_limits<size_t>::max)();
    return manager + initialRenderer + scratchGrowth;
}

static uint32_t LargestGrowthCapacityForBudget(
    uint64_t budgetBytes, uint32_t initialCapacity,
    uint32_t renderThreads, uint32_t blockFrames) noexcept {
    if (EstimateRuntimeVoiceMemoryAfterGrowth(
            initialCapacity, initialCapacity, renderThreads, blockFrames) >
        budgetBytes)
        return 0u;
    uint32_t low = initialCapacity;
    uint32_t high = kMaxPolyphony;
    while (low < high) {
        const uint32_t middle = low + (high - low + 1u) / 2u;
        if (EstimateRuntimeVoiceMemoryAfterGrowth(
                initialCapacity, middle, renderThreads, blockFrames) <=
            budgetBytes)
            low = middle;
        else
            high = middle - 1u;
    }
    return low;
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

static Driver* s_instance = nullptr;

// ── RuntimeLink V2 IPC (driver side) ───────────────────────────────────────
// The control thread polls the command mailbox every ~33 ms, applies
// live config changes (grouped ApplyLiveConfig), runs the reload/reset
// commands, and publishes telemetry at ~30 Hz.  The audio thread never
// touches IPC; it only updates the process-local g_audioSnapshot, which
// the control thread reads at publish time.
#if !defined(SVMS_XP_COMPAT)
static svms::RuntimeLinkDriverV2 g_rlDriver;
#endif

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
      sampleDataStore(nullptr), hilbertDataStore(nullptr), samplesStore(nullptr), regionInitialPeaks(nullptr),
      regionInitialPeakCount(0), preparedRegions(nullptr), preparedRegionCount(0),
      soundFontGeneration_(1u),
      sampleStoreCount(0), sampleDataFrames(0),
      qpcFreq(1),
      leftBuffer(nullptr), rightBuffer(nullptr), bufferCapacity(0),
      channelBusPlanes(nullptr), channelBusCapacity(0),
      eventBuffer(nullptr), eventBufferCapacity_(0u),
      eventScheduler_(1u),
      overflowMode_(EventOverflowMode::PriorityVelocity), correctnessMode_(false),
      highPriorityVelocity_(96), shedStartPercent_(70), maxEventsPerBlock_(65536),
      diagnosticsEnabled_(false), diagnosticsWindow_(false), diagnosticsDebugOutput_(false),
      nextEventSequence_(0), globalTerminationFence_(0), cancelProducers_(false),
      producerWakeEpoch_(0),
      scheduledSizePublished_(0), submittedAtomic_(0), acceptedAtomic_(0), shedAtomic_(0),
      cancelledAtomic_(0), currentVelocityCutoffAtomic_(1),
      compilerEpochQPC_(0), compilerWakeEpoch_(0), compilerSleeping_(false),
      useEventCompiler_(false),
      telemetry_{}, debugSnapshotIndex_(0), callbackCount_(0),
      virtualRenderClockQPC(0),
      virtualRenderSample_(0), clockInitialized(false), nextPlayIndex_(1) {
    std::memset(noteRegionCache_, 0xff, sizeof(noteRegionCache_));
    std::memset(noteLaunchPlanCache_, 0, sizeof(noteLaunchPlanCache_));
    std::memset(noteLaunchHotCache_, 0, sizeof(noteLaunchHotCache_));
    std::fill(std::begin(channelLaunchRevision_),
              std::end(channelLaunchRevision_), 1u);
    std::memset(configuredVelocityGain_, 0, sizeof(configuredVelocityGain_));
    std::fill(std::begin(channelPitchBendRatio_),
              std::end(channelPitchBendRatio_), 1.0f);
    for (auto& counter : shedByVelocityAtomic_) counter.store(0, std::memory_order_relaxed);
    for (auto& fence : channelTerminationFence_) fence.store(0, std::memory_order_relaxed);
    reverb.Reset();
    limiter.Reset();
    InitializeCriticalSection(&cs);
    InitializeCriticalSection(&soundFontBuildCs_);
}

Driver::~Driver() {
    Shutdown();
    if (eventBuffer) { _aligned_free(eventBuffer); eventBuffer = nullptr; }
    DeleteCriticalSection(&soundFontBuildCs_);
    DeleteCriticalSection(&cs);
}

bool Driver::Initialize() {
    if (initialized) return true;

    ResolveAddressWaitApi();
    midiIngress_.DrainAvailable();
    eventScheduler_.Reset();
    compilerEpochQPC_.store(0u, std::memory_order_relaxed);
    globalTerminationFence_.store(0, std::memory_order_relaxed);
    for (auto& fence : channelTerminationFence_) fence.store(0, std::memory_order_relaxed);
    virtualRenderClockQPC = 0;
    virtualRenderSample_ = 0;
    outputFramePublished_.store(0u, std::memory_order_relaxed);
    clockInitialized = false;
    callbackTiming_.Reset();

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFreq = freq.QuadPart;
    tscClock_.Initialize();

    // Note-on collapse window in QPC ticks (fixed 20 ms, frame-size
    // independent). Coalescing itself defaults OFF; this only defines the
    // window used once it is enabled at runtime.
    noteOnCollapse_.SetWindowTicks(
        freq.QuadPart * kNoteOnCollapseWindowMs / 1000u);

    EngineConfig cfg = EngineConfig::Load();
    if (!cfg.configWarning.empty()) {
        std::string warning = "[SVMS] configuration warning: " +
                              cfg.configWarning + "\n";
        OutputDebugStringA(warning.c_str());
    }
    if (!cfg.Validate()) { LOG("EngineConfig validation failed"); return false; }

    // Threshold comes from config (default 1 = disabled). Applied before
    // the audio thread starts, so no torn first-block state.
    noteOnCollapse_.SetThreshold(cfg.noteOnCollapseThreshold);

    overflowMode_ = cfg.eventOverflowMode;
    correctnessMode_ = cfg.correctnessMode;
    highPriorityVelocity_ = cfg.highPriorityVelocity;
    shedStartPercent_ = cfg.shedStartPercent;
    diagnosticsEnabled_ = cfg.diagnosticsEnabled;
    diagnosticsWindow_ = cfg.diagnosticsEnabled && cfg.diagnosticsWindow;
    diagnosticsDebugOutput_ = cfg.diagnosticsEnabled && cfg.diagnosticsDebugOutput;
    cancelProducers_.store(false, std::memory_order_release);
    compilerSleeping_.store(false, std::memory_order_relaxed);

    auto configureEventStorage = [this](uint32_t ringCapacity,
                                        uint32_t blockCapacity) -> bool {
        if (!midiIngress_.ConfigureCapacity(ringCapacity)) return false;
        if (!compiledPages_.ConfigureCapacity(ringCapacity)) return false;
        if (!pagedScheduler_.Configure(&compiledPages_, ringCapacity))
            return false;
        try {
            eventScheduler_.ConfigureCapacity(ringCapacity);
        } catch (...) {
            return false;
        }
        const uint32_t actualBlockCapacity = blockCapacity;
        if (static_cast<size_t>(actualBlockCapacity) >
            (std::numeric_limits<size_t>::max)() / sizeof(svms::RenderEvent)) {
            return false;
        }
        svms::RenderEvent* replacement =
            static_cast<svms::RenderEvent*>(_aligned_malloc(
                sizeof(svms::RenderEvent) *
                    static_cast<size_t>(actualBlockCapacity),
                64));
        if (!replacement) return false;
        if (eventBuffer) _aligned_free(eventBuffer);
        eventBuffer = replacement;
        eventBufferCapacity_ = actualBlockCapacity;
        return true;
    };

    if (!configureEventStorage(cfg.eventRingCapacity,
                               cfg.maxEventsPerBlock)) {
        char warning[256]{};
        std::snprintf(
            warning, sizeof(warning),
            "[SVMS] configuration warning: event capacity %u / block %u "
            "could not be allocated; using %u / %u\n",
            cfg.eventRingCapacity, cfg.maxEventsPerBlock,
            kDefaultEventRingCapacity, 65536u);
        OutputDebugStringA(warning);
        LOG("Configuration warning: event capacity %u / block %u could not "
            "be allocated; falling back to %u / %u",
            cfg.eventRingCapacity, cfg.maxEventsPerBlock,
            kDefaultEventRingCapacity, 65536u);
        cfg.eventRingCapacity = kDefaultEventRingCapacity;
        cfg.maxEventsPerBlock = 65536u;
        if (!configureEventStorage(cfg.eventRingCapacity,
                                   cfg.maxEventsPerBlock)) {
            LOG("FAILED: Could not allocate default event storage");
            return false;
        }
    }
    maxEventsPerBlock_ = cfg.maxEventsPerBlock;
    engineConfig_ = cfg;
    svms::g_voiceRetireThreshold.store(
        cfg.voiceRetireThreshold, std::memory_order_relaxed);
    svms::g_threadAffinityMode.store(cfg.threadAffinityMode,
                                     std::memory_order_relaxed);
    ccCollapseEnabled_.store(cfg.ccCollapse, std::memory_order_relaxed);
    blockTimingEnabled_.store(cfg.blockTimingMode, std::memory_order_relaxed);
    unboundedRenderEnabled_.store(cfg.unboundedRender,
                                  std::memory_order_relaxed);
    svms::g_largePagesEnabled.store(cfg.largePages,
                                    std::memory_order_relaxed);

    sampleRate = cfg.sampleRate;
    bufferFrames = cfg.bufferFrames;
    LOG("Initialize: sampleRate=%u bufferFrames=%u maxVoices=%u", sampleRate, bufferFrames, cfg.maxVoices);

    // Start diagnostics before the backend so an XP DirectSound failure is
    // visible rather than returning from midiOutOpen with no evidence.
    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_)) {
        DiagWindow_Create(diagnosticsWindow_, diagnosticsDebugOutput_);
        DiagWindow_UpdateStartup(false, 0, false, sampleRate, bufferFrames,
                                 cfg.masterVolume);
    }

#if !defined(SVMS_XP_COMPAT) && defined(SVMS_WITH_ASIO)
    if (cfg.audioBackend == AudioBackend::ASIO) {
        AudioOutputASIO* asio = new AudioOutputASIO();
        asio->SetFormatChangedCallback(&Driver::OnAsioFormatChanged, this);
        asio->SetRebuildCallback(&Driver::OnAsioRebuildRequested, this);
        if (asio->Initialize(sampleRate, bufferFrames, cfg.audioDevice)) {
            audioOutput = asio;
        } else {
            LOG("ASIO backend init failed (%s), falling back to WASAPI shared",
                asio->GetLastErrorText());
            delete asio;
        }
    }
    if (!audioOutput)
#endif
    {
        AudioOutput* wasapi = new AudioOutput();
        audioOutput = wasapi;
#if defined(SVMS_XP_COMPAT)
        if (!wasapi->Initialize(sampleRate, bufferFrames)) {
#else
        if (!wasapi->Initialize(sampleRate, bufferFrames, cfg.audioDevice)) {
#endif
            HRESULT hr = wasapi->GetLastError();
            LOG("FAILED: AudioOutput::Initialize hr=0x%08X", (unsigned)hr);
            XPBootstrapTrace("[SVMS XP] DirectSound initialization FAILED\r\n");
            DiagWindow_UpdateStartup(false, static_cast<int32_t>(hr), false,
                                     sampleRate, bufferFrames, cfg.masterVolume,
                                     UsesXPWaveOut(audioOutput));
#if !defined(SVMS_XP_COMPAT)
            delete wasapi;
            audioOutput = nullptr;
#endif
            return false;
        }
    }
    bufferFrames = audioOutput->GetBufferFrames();
    sampleRate = audioOutput->GetSampleRate();
    postHighPass.Initialize(sampleRate);
    reverb.Configure(sampleRate, cfg);
    limiter.Configure(sampleRate, cfg);
    channelLimiter.Configure(sampleRate, cfg);
    {
        bool isAsio = false;
#if !defined(SVMS_XP_COMPAT)
        isAsio = dynamic_cast<AudioOutputASIO*>(audioOutput) != nullptr;
#endif
        DiagWindow_SetBackendLabel(isAsio ? L"ASIO" : L"WASAPI shared");
    }
    LOG("AudioOutput initialized, rate=%u bufferFrames=%u", sampleRate, bufferFrames);

    bufferCapacity = bufferFrames;
    leftBuffer = static_cast<float*>(_aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
    rightBuffer = static_cast<float*>(_aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
    if (!leftBuffer || !rightBuffer) {
        LOG("FAILED: Could not allocate render buffers");
        return false;
    }
    if (!AllocateChannelBuses(bufferCapacity)) {
        LOG("FAILED: Could not allocate per-channel limiter buses");
        return false;
    }

    uint32_t renderThreads = cfg.renderThreads;
    if (renderThreads == 0u)
        renderThreads = SelectAutomaticRenderThreadCount();
    uint32_t voiceGrowthCeiling = kMaxPolyphony;
    if (cfg.voiceMemoryBudgetMB != 0u) {
        const uint64_t budgetBytes =
            static_cast<uint64_t>(cfg.voiceMemoryBudgetMB) << 20u;
        if (renderThreads > 1u &&
            cfg.maxVoices <= kDenseRenderMaximumVoices &&
            EstimateRuntimeVoiceMemoryBytes(
                cfg.maxVoices, renderThreads, bufferCapacity) > budgetBytes) {
            LOG("Voice memory budget %u MiB cannot fit the dense %u-thread "
                "renderer at %u voices; falling back to one render thread",
                cfg.voiceMemoryBudgetMB, renderThreads, cfg.maxVoices);
            renderThreads = 1u;
        }
        voiceGrowthCeiling = LargestInitialVoiceCapacityForBudget(
            budgetBytes, renderThreads, bufferCapacity);
        if (voiceGrowthCeiling == 0u && renderThreads > 1u) {
            LOG("Voice memory budget %u MiB cannot fit %u-thread renderer; "
                "falling back to one render thread",
                cfg.voiceMemoryBudgetMB, renderThreads);
            renderThreads = 1u;
            voiceGrowthCeiling = LargestInitialVoiceCapacityForBudget(
                budgetBytes, renderThreads, bufferCapacity);
        }
        if (voiceGrowthCeiling == 0u) {
            LOG("FAILED: voice memory budget %u MiB is below minimum runtime "
                "storage", cfg.voiceMemoryBudgetMB);
            return false;
        }
        if (cfg.maxVoices > voiceGrowthCeiling) {
            char warning[256]{};
            std::snprintf(warning, sizeof(warning),
                "[SVMS] configuration warning: synth.max_voices %u exceeds "
                "the %u MiB voice-memory budget; using %u voices\n",
                cfg.maxVoices, cfg.voiceMemoryBudgetMB, voiceGrowthCeiling);
            OutputDebugStringA(warning);
            LOG("Voice memory budget clamped maxVoices %u -> %u",
                cfg.maxVoices, voiceGrowthCeiling);
            cfg.maxVoices = voiceGrowthCeiling;
        }
        voiceGrowthCeiling = LargestGrowthCapacityForBudget(
            budgetBytes, cfg.maxVoices, renderThreads, bufferCapacity);
        if (voiceGrowthCeiling == 0u) {
            LOG("FAILED: voice memory budget %u MiB cannot represent the "
                "selected runtime layout", cfg.voiceMemoryBudgetMB);
            return false;
        }
        const size_t startupBytes = EstimateRuntimeVoiceMemoryBytes(
            cfg.maxVoices, renderThreads, bufferCapacity);
        LOG("Voice memory budget %u MiB: startup %.2f MiB, live-growth "
            "ceiling %u voices", cfg.voiceMemoryBudgetMB,
            static_cast<double>(startupBytes) / (1024.0 * 1024.0),
            voiceGrowthCeiling);
    }
    ConfigureRuntimeVoiceGrowthCeiling(voiceGrowthCeiling);
    engineConfig_ = cfg;
    svms::g_voiceRetireThreshold.store(
        cfg.voiceRetireThreshold, std::memory_order_relaxed);
    svms::g_threadAffinityMode.store(cfg.threadAffinityMode,
                                     std::memory_order_relaxed);
    ccCollapseEnabled_.store(cfg.ccCollapse, std::memory_order_relaxed);
    blockTimingEnabled_.store(cfg.blockTimingMode, std::memory_order_relaxed);
    unboundedRenderEnabled_.store(cfg.unboundedRender,
                                  std::memory_order_relaxed);
    svms::g_largePagesEnabled.store(cfg.largePages,
                                    std::memory_order_relaxed);

    voiceManager = new VoiceManager();
    voiceManager->SetStealPolicy(cfg.stealPolicy);
    voiceManager->SetPerKeyVoiceCap(cfg.perKeyVoiceCap);
    if (!voiceManager->Initialize(cfg.maxVoices, sampleRate)) {
        LOG("FAILED: Could not allocate voice storage maxVoices=%u",
            cfg.maxVoices);
        return false;
    }
    // Per-voice phase rotation lives in the VoiceManager (SVMSPhaseRotation.h);
    // mode 0 (Coherent) keeps the render path bit-exact.
    if (!voiceManager->SetPhaseRotationMode(cfg.phaseRotationMode)) {
        LOG("WARNING: Could not allocate phase rotation state; running Coherent");
    }

    for (uint32_t index = 0; index < 2u; ++index) {
        voiceStatisticsSnapshots_[index] = SnappyVoiceStatistics{};
        voiceStatisticsSnapshots_[index].freeVoices = cfg.maxVoices;
        legacyDebugSnapshots_[index] = LegacyDriverDebugInfo{};
        legacyDebugSnapshots_[index].audioLatency =
            static_cast<double>(bufferFrames) * 1000.0 /
            static_cast<double>(sampleRate);
        legacyDebugSnapshots_[index].audioBufferSize = bufferFrames;
        renderingTimeSnapshots_[index] = 0.0f;
    }
    debugSnapshotIndex_.store(0u, std::memory_order_release);
    LOG("VoiceManager initialized, maxVoices=%u", cfg.maxVoices);

    channelCache = new ChannelCache();
    channelCache->SetMasterVolume(cfg.masterVolume);
    renderScalar = new RenderScalar();
    renderScalar->SetGhostBudget(cfg.ghostBudget);
    // Honor the configured render backend. The constructor already selects
    // the best set (Auto behavior); an explicit non-Auto request overrides
    // it, falling back to the best available set when unsupported.
    if (cfg.renderBackend != RenderBackend::Auto) {
        if (!renderScalar->SetRenderBackend(cfg.renderBackend)) {
            LOG("Configuration warning: render backend %u not supported on "
                "this CPU; using best available",
                static_cast<uint32_t>(cfg.renderBackend));
        }
    }
    if (!renderScalar->ReserveVoiceCapacity(cfg.maxVoices)) {
        LOG("FAILED: Could not allocate renderer scratch maxVoices=%u",
            cfg.maxVoices);
        return false;
    }
    if (!renderScalar->ConfigureRenderThreads(renderThreads, bufferCapacity)) {
        LOG("Configuration warning: could not start %u render threads; "
            "using the audio thread only", renderThreads);
        renderScalar->ConfigureRenderThreads(1u, bufferCapacity);
    }
    LOG("Voice renderer initialized: backend=%s threads=%u",
        renderScalar->GetRenderBackendName(),
        renderScalar->GetRenderThreadCount());

    // Register the EventDispatcher callback so RenderScalar can dispatch
    // MIDI events at their exact sub-sample positions during RenderBlock.
    renderScalar->SetEventDispatcher(DispatchRenderEvent, this);
    renderScalar->SetEventBatchDispatcher(DispatchRenderEventBatch, this);

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

    // Initialize both mailbox buffers with the same starting state.
    liveMailbox_.InitFromEngineConfig(cfg, sampleRate);
    liveMailbox_.StoreToNonAtomic(appliedMailbox_);
    liveMailboxSeq_.store(2u, std::memory_order_release);
    appliedSeq_.store(2u, std::memory_order_release);
    lastAppliedLiveSeq_ = 0u;
    appliedMasterVolume_ = cfg.masterVolume;

    // Velocity curve/floor are restart-only configuration.  Preserve the
    // exact historical quantization into g_velGainLUT, but pay powf once at
    // initialization instead of once per note-on.
    for (uint32_t velocity = 0; velocity < 128u; ++velocity) {
        const float mapped = channelCache->ComputeVelocity(
            static_cast<uint8_t>(velocity), *configSnapshot);
        if (mapped <= 0.0f) {
            configuredVelocityGain_[velocity] = 0.0f;
            continue;
        }
        uint32_t mappedIndex = static_cast<uint32_t>(mapped * 127.0f + 0.5f);
        mappedIndex = (std::max)(1u, (std::min)(127u, mappedIndex));
        configuredVelocityGain_[velocity] = g_velGainLUT[mappedIndex];
    }

    audioOutput->SetRenderCallback(RenderCallback, this);

#if !defined(SVMS_XP_COMPAT)
    useEventCompiler_ = std::thread::hardware_concurrency() >= 2u;
    if (useEventCompiler_) {
        try {
            eventCompilerThread_ = std::thread(&Driver::EventCompilerLoop, this);
            // MSVC's native_handle is the Win32 thread handle. Applied after
            // creation; a same-instant race into the compiler loop is benign
            // (the pin only changes scheduler placement).
            svms::ApplyThreadAffinity(eventCompilerThread_.native_handle(),
                                      svms::AffinityRole::Compiler);
        } catch (...) {
            useEventCompiler_ = false;
        }
    }

    soundFontLoaderStop_.store(false, std::memory_order_release);
    soundFontLoadState_.store(0u, std::memory_order_relaxed);
    soundFontLoadEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (soundFontLoadEvent_) {
        try {
            soundFontLoaderThread_ =
                std::thread(&Driver::SoundFontLoaderLoop, this);
        } catch (...) {
            CloseHandle(soundFontLoadEvent_);
            soundFontLoadEvent_ = nullptr;
        }
    }
#endif

    initialized = true;
    LOG("Initialize SUCCESS");

    // External synth backend (api.backend): loaded once at init; on any
    // failure the driver falls back to the in-process SVMS engine.
    if (!InitializeExternalBackend())
        LOG("External backend unavailable — using the in-process SVMS engine");

    // Start RuntimeLink V2 IPC so the V3 Configurator can connect.
    // Optional: failure must never break midiOutOpen/KDMAPI/audio.
#if !defined(SVMS_XP_COMPAT)
    if (g_rlDriver.Initialize()) {
        g_rlDriver.StartControlThread(
            [this]() { return BuildRuntimeLinkTelemetry(); },
            [this](const svms::RuntimeLinkCommandV2& cmd, char* resultText) {
                return HandleRuntimeLinkCommand(cmd, resultText);
            });
        LOG("RuntimeLink V2%s initialized: PID=%u session=%016llX",
            g_rlDriver.IsV3Initialized() ? "/V3" : "",
            g_rlDriver.GetPID(),
            static_cast<unsigned long long>(g_rlDriver.GetSessionId()));
    } else {
        LOG("RuntimeLink V2 initialization skipped (non-fatal)");
    }
#endif

    return true;
}

void Driver::Shutdown() {
    ShutdownExternalBackend();
#if !defined(SVMS_XP_COMPAT)
    g_rlDriver.Shutdown();
    liveRecorder_.Stop();
    soundFontLoaderStop_.store(true, std::memory_order_release);
    if (soundFontLoadEvent_) SetEvent(soundFontLoadEvent_);
    if (soundFontLoaderThread_.joinable()) soundFontLoaderThread_.join();
    if (soundFontLoadEvent_) {
        CloseHandle(soundFontLoadEvent_);
        soundFontLoadEvent_ = nullptr;
    }
#endif

    cancelProducers_.store(true, std::memory_order_release);
    producerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(producerWakeEpoch_);
    compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(compilerWakeEpoch_);
    compilerSleeping_.store(false, std::memory_order_release);
    StopConfiguredMidiInput();
#if !defined(SVMS_XP_COMPAT) && defined(SVMS_WITH_ASIO)
    audioRebuildShutdown_.store(true, std::memory_order_release);
    if (audioRebuildThread_.joinable()) audioRebuildThread_.join();
#endif
    if (audioOutput) {
        audioOutput->Stop();
        audioOutput->Shutdown();
        delete audioOutput;
        audioOutput = nullptr;
    }
#if !defined(SVMS_XP_COMPAT)
    // Torn-down rebuild victims: their watcher threads were parked on
    // rebuildRequested_ and are now joined by Shutdown() above the park.
    for (AudioOutputBase* retired : retiredOutputs_) {
        if (!retired) continue;
        retired->Stop();
        retired->Shutdown();
        delete retired;
    }
    retiredOutputs_.clear();
#endif
    if (eventCompilerThread_.joinable()) eventCompilerThread_.join();
    useEventCompiler_ = false;
    midiIngress_.DrainAvailable();
    pagedScheduler_.Reset();
    eventScheduler_.Reset();
    scheduledSizePublished_.store(0, std::memory_order_release);
    if (eventBuffer) {
        _aligned_free(eventBuffer);
        eventBuffer = nullptr;
        eventBufferCapacity_ = 0u;
    }
    delete voiceManager; voiceManager = nullptr;
    delete channelCache; channelCache = nullptr;
    delete renderScalar; renderScalar = nullptr;
    ConfigureRuntimeVoiceGrowthCeiling(kRuntimeVoiceGrowthCeiling);
    delete configSnapshot; configSnapshot = nullptr;
    _aligned_free(leftBuffer); leftBuffer = nullptr;
    _aligned_free(rightBuffer); rightBuffer = nullptr;
    bufferCapacity = 0;
    FreeChannelBuses();

    DestroyAllSoundFontBundles();

    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_))
        DiagWindow_Destroy();

    initialized = false;
}

// ── Per-MIDI-channel limiter bus planes ──────────────────────────────────
// 16 stereo planes sized to the mix-buffer capacity, plus two pointer
// tables handed to RenderBlock/ChannelLimiterState.  Called from non-audio
// init/rebuild paths only, so the callback never allocates.
bool Driver::AllocateChannelBuses(uint32_t capacity) {
    if (capacity == 0u) return false;
    if (channelBusPlanes && channelBusCapacity >= capacity) return true;
    FreeChannelBuses();
    const size_t planeFloats =
        static_cast<size_t>(capacity) * kChannelCount * 2u;
    channelBusPlanes = static_cast<float*>(
        _aligned_malloc(planeFloats * sizeof(float), kMixBufferAlign));
    if (!channelBusPlanes) {
        channelBusCapacity = 0u;
        return false;
    }
    for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
        float* plane = channelBusPlanes +
            static_cast<size_t>(channel) * 2u * capacity;
        channelBusLeftTable[channel] = plane;
        channelBusRightTable[channel] = plane + capacity;
    }
    channelBusCapacity = capacity;
    return true;
}

void Driver::FreeChannelBuses() {
    _aligned_free(channelBusPlanes);
    channelBusPlanes = nullptr;
    channelBusCapacity = 0u;
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

void Driver::CopyVoiceStatistics(SnappyVoiceStatistics& out) const {
    const uint32_t index = debugSnapshotIndex_.load(std::memory_order_acquire) & 1u;
    out = voiceStatisticsSnapshots_[index];
}

float Driver::GetRenderingTimeMilliseconds() const {
    const uint32_t index = debugSnapshotIndex_.load(std::memory_order_acquire) & 1u;
    return renderingTimeSnapshots_[index];
}

const LegacyDriverDebugInfo* Driver::GetLegacyDebugInfo() const {
    const uint32_t index = debugSnapshotIndex_.load(std::memory_order_acquire) & 1u;
    return &legacyDebugSnapshots_[index];
}

bool Driver::StartAudio() {
    if (audioOutput && !audioOutput->IsRunning()) {
        LOG("StartAudio: starting audio stream...");
        const bool ok = audioOutput->Start();
        LOG("StartAudio: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) {
            XPBootstrapTrace("[SVMS XP] audio stream start FAILED\r\n");
            DiagWindow_UpdateStartup(false,
                                     static_cast<int32_t>(audioOutput->GetLastError()),
                                     soundFontData && sampleDataStore,
                                     sampleRate, bufferFrames,
                                     engineConfig_.masterVolume,
                                     UsesXPWaveOut(audioOutput));
        }
        if (ok && engineConfig_.midiInputEnabled && !midiInput_)
            (void)StartConfiguredMidiInput();
        return ok;
    }
    const bool running = audioOutput && audioOutput->IsRunning();
    if (running && engineConfig_.midiInputEnabled && !midiInput_)
        (void)StartConfiguredMidiInput();
    return running;
}

void Driver::OnAsioFormatChanged(uint32_t sampleRate, uint32_t bufferFrames,
                                 void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self) return;
    self->pendingFormatRate_.store(sampleRate, std::memory_order_release);
    self->pendingFormatFrames_.store(bufferFrames, std::memory_order_release);
    LOG("ASIO format change requested: rate=%u frames=%u", sampleRate, bufferFrames);
}

#if !defined(SVMS_XP_COMPAT) && defined(SVMS_WITH_ASIO)
// Escalation from AudioOutputASIO's watcher thread after repeated in-place
// reopen failures. The failed instance is already parked (its g_instance is
// null and its watcher spins on rebuildRequested_) — nothing to park here.
// Hand the actual rebuild to a dedicated thread so the watcher is never
// blocked behind teardown work and stays joinable for Shutdown().
void Driver::OnAsioRebuildRequested(void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self) return;
    bool expected = false;
    if (!self->audioRebuildActive_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return;
    if (self->audioRebuildThread_.joinable())
        self->audioRebuildThread_.join();
    try {
        self->audioRebuildThread_ = std::thread(&Driver::RebuildAudioOutput, self);
    } catch (...) {
        LOG("FAILED: ASIO rebuild thread spawn failed");
        self->audioRebuildActive_.store(false, std::memory_order_release);
    }
}

// Object-level output rebuild: retire the dead output (never delete it
// here — the parked ASIO instance may still have in-flight bufferSwitch on
// its buffers; retired objects are torn down in Driver::Shutdown), then
// construct, initialize, and start a brand-new one. Runs on the dedicated
// rebuild thread; no output is delivering callbacks at this point (the old
// one is parked, the new one is not started yet), so updating the mix
// buffers / DSP configuration here is race-free.
void Driver::RebuildAudioOutput() {
    std::lock_guard<std::mutex> lock(audioOutputMutex_);
    if (audioRebuildShutdown_.load(std::memory_order_acquire)) {
        audioRebuildActive_.store(false, std::memory_order_release);
        return;
    }
    const EngineConfig cfg = engineConfig_;
    LOG("Audio output rebuild: retiring dead output, backend=%d",
        static_cast<int>(cfg.audioBackend));

    if (audioOutput) {
        audioOutput->Stop();
        retiredOutputs_.push_back(audioOutput);
        audioOutput = nullptr;
    }
    pendingFormatRate_.store(0, std::memory_order_release);
    pendingFormatFrames_.store(0, std::memory_order_release);

    // Two ASIO attempts, then the same WASAPI-shared fallback the init
    // path uses, so a wedged driver never leaves the engine without audio.
    AudioOutputBase* replacement = nullptr;
    bool replacementIsAsio = false;
    for (int attempt = 0; attempt < 3 && !replacement; ++attempt) {
        if (cfg.audioBackend == AudioBackend::ASIO && attempt < 2) {
            AudioOutputASIO* asio = new AudioOutputASIO();
            asio->SetFormatChangedCallback(&Driver::OnAsioFormatChanged, this);
            asio->SetRebuildCallback(&Driver::OnAsioRebuildRequested, this);
            asio->SetRenderCallback(RenderCallback, this);
            if (asio->Initialize(sampleRate, bufferFrames, cfg.audioDevice)) {
                replacement = asio;
                replacementIsAsio = true;
            } else {
                LOG("ASIO rebuild attempt %d failed (%s)", attempt + 1,
                    asio->GetLastErrorText());
                delete asio;
            }
        } else {
            AudioOutput* wasapi = new AudioOutput();
            wasapi->SetRenderCallback(RenderCallback, this);
            if (wasapi->Initialize(sampleRate, bufferFrames, cfg.audioDevice)) {
                replacement = wasapi;
            } else {
                LOG("FAILED: WASAPI rebuild attempt %d hr=0x%08X", attempt + 1,
                    static_cast<unsigned>(wasapi->GetLastError()));
                delete wasapi;
            }
        }
    }

    if (!replacement) {
        LOG("FAILED: audio output rebuild could not create any backend; "
            "audio remains down");
        audioRebuildActive_.store(false, std::memory_order_release);
        return;
    }
    audioOutput = replacement;

    // Mirror the init path: adopt the backend's actual format, grow the mix
    // buffers if needed, reconfigure the per-rate DSP chain.
    bufferFrames = replacement->GetBufferFrames();
    sampleRate = replacement->GetSampleRate();
    if (bufferFrames > bufferCapacity) {
        _aligned_free(leftBuffer);
        _aligned_free(rightBuffer);
        bufferCapacity = bufferFrames;
        leftBuffer = static_cast<float*>(
            _aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
        rightBuffer = static_cast<float*>(
            _aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
        AllocateChannelBuses(bufferCapacity);
        LOG("Rebuild mix buffers resized: capacity=%u", bufferCapacity);
    }
    postHighPass.Initialize(sampleRate);
    reverb.Configure(sampleRate, engineConfig_);
    limiter.Configure(sampleRate, engineConfig_);
    channelLimiter.Configure(sampleRate, engineConfig_);
    if (voiceManager) voiceManager->SetSampleRate(sampleRate);
    {
        bool isAsio = replacementIsAsio;
#if !defined(SVMS_XP_COMPAT)
        isAsio = dynamic_cast<AudioOutputASIO*>(replacement) != nullptr;
#endif
        DiagWindow_SetBackendLabel(isAsio ? L"ASIO" : L"WASAPI shared");
    }

    if (!replacement->Start()) {
        LOG("FAILED: rebuilt audio output start failed (%s) hr=0x%08X",
            replacement->GetLastErrorText(),
            static_cast<unsigned>(replacement->GetLastError()));
    } else {
        LOG("[SVMS] audio output rebuilt: rate=%u bufferFrames=%u, stream live",
            sampleRate, bufferFrames);
    }
    audioRebuildActive_.store(false, std::memory_order_release);
}
#endif

// Runs on the audio thread only (dispatched from RenderCallback). The ASIO
// driver has already stopped delivering buffers of the old size by the time
// the notification arrives, and the ASIO output re-creates its buffers
// before returning into bufferSwitch — so by the time we get here the
// backend's GetBufferFrames/GetSampleRate report the new format and this
// block's numFrames matches it. Everything downstream of the render callback
// is per-block; only the persistent state below captures the format.
void Driver::ApplyPendingAudioFormat() {
    const uint32_t newRate =
        pendingFormatRate_.exchange(0, std::memory_order_acq_rel);
    const uint32_t newFrames =
        pendingFormatFrames_.exchange(0, std::memory_order_acq_rel);

    if (newFrames && newFrames != bufferFrames) {
        bufferFrames = newFrames;
        if (newFrames > bufferCapacity) {
            _aligned_free(leftBuffer);
            _aligned_free(rightBuffer);
            bufferCapacity = newFrames;
            leftBuffer = static_cast<float*>(
                _aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
            rightBuffer = static_cast<float*>(
                _aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
            AllocateChannelBuses(bufferCapacity);
            LOG("ASIO buffer resized: capacity=%u", bufferCapacity);
        }
    }
    if (newRate && newRate != sampleRate) {
        sampleRate = newRate;
        postHighPass.Initialize(sampleRate);
        reverb.Configure(sampleRate, engineConfig_);
        limiter.Configure(sampleRate, engineConfig_);
    channelLimiter.Configure(sampleRate, engineConfig_);
        LOG("ASIO sample rate changed: %u", sampleRate);
        // Active voices carry envelope/phase state in the old rate's units;
        // they retire naturally within a second or two. New configuration
        // uses the new rate from the next note-on (same trade-off every
        // host makes on device rate changes).
        if (voiceManager) voiceManager->SetSampleRate(sampleRate);
    }
}

} // namespace svms
