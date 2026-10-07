// SoundFont bundles: preparation, Hilbert pair store, loader thread, swap.

#include "SVMSDriverInternal.h"

namespace svms {

static void DestroySoundFontBundle(SoundFontBundle* bundle) noexcept {
    if (!bundle) return;
    if (bundle->bankCount != 0u) {
        for (uint32_t i = 0u; i < bundle->bankCount; ++i)
            DestroySoundFontBundle(bundle->banks[i]);
        free(bundle->sampleData);
        free(bundle->hilbertData);
        delete bundle;
        return;
    }
    free(bundle->regionInitialPeaks);
    free(bundle->preparedRegions);
    free(bundle->sampleData);
    free(bundle->hilbertData);
    free(bundle->samples);
    if (bundle->data) {
        sf2_free(bundle->data);
        delete bundle->data;
    }
    delete bundle;
}

static void PrepareSF2Region(const SF2Data* data, const SFSampleRegion& region,
                             uint32_t outputRate, ChannelCache* channelCache,
                             PreparedSF2Region& out) {
    std::memset(&out, 0, sizeof(out));
    if (!data || region.sampleIndex >= data->sampleCount ||
        !sf2_validate_region(data, &region)) return;

    const SF2Sample& sample = data->samples[region.sampleIndex];
    const int rootKey = region.rootKey >= 0
        ? static_cast<int>(region.rootKey)
        : static_cast<int>(sample.originalPitch);
    const float tune = static_cast<float>(region.coarseTune) +
        static_cast<float>(region.fineTune) / 100.0f;
    out.bendScale = static_cast<float>(
        region.scaleTuning != 0 ? region.scaleTuning : 100) / 100.0f;
    const float sourceRate = static_cast<float>(
        sample.sampleRate > 0u ? sample.sampleRate : 44100u);
    const float targetRate = static_cast<float>(
        outputRate > 0u ? outputRate : 44100u);
    const float rateRatio = sourceRate / targetRate;
    for (uint32_t note = 0; note < kNoteCount; ++note) {
        const float semitones =
            (static_cast<float>(note) + tune - static_cast<float>(rootKey)) *
            out.bendScale;
        out.basePhaseStep[note] =
            rateRatio * powf(2.0f, semitones / 12.0f);
    }

    out.attenuationGain = (region.initialAttenuation > 0 ||
                           (data->isSfz && region.initialAttenuation != 0))
        ? InitialAttenuationToGain(static_cast<float>(region.initialAttenuation))
        : 1.0f;
    out.sustainLevel = SustainAttenuationToGain((std::max)(
        0.0f, static_cast<float>(region.sustainVolEnv)));
    if (out.sustainLevel > 1.0f) out.sustainLevel = 1.0f;

    const float rate = static_cast<float>(outputRate > 0u ? outputRate : 44100u);
    const float delaySeconds = TimecentsToSeconds(region.delayVolEnv);
    const float holdSeconds = TimecentsToSeconds(region.holdVolEnv);
    const float attackSeconds = TimecentsToSeconds(region.attackVolEnv);
    const float decaySeconds = TimecentsToSeconds(region.decayVolEnv);
    const float releaseSeconds = TimecentsToSeconds(region.releaseVolEnv);
    out.delaySamples = delaySeconds > 0.0f
        ? static_cast<uint32_t>(delaySeconds * rate) : 0u;
    out.holdSamples = holdSeconds > 0.0f
        ? static_cast<uint32_t>(holdSeconds * rate) : 0u;
    out.attackSamples = attackSeconds > 0.0001f
        ? static_cast<uint32_t>(attackSeconds * rate) : 0u;
    out.decaySamples = decaySeconds > 0.0001f
        ? static_cast<uint32_t>(decaySeconds * rate) : 0u;
    out.decaySlope = 1.0f;
    if (out.decaySamples > 0u) {
        const float slope = -9.226f / static_cast<float>(out.decaySamples);
        out.decaySlope = expf(slope);
        if (out.sustainLevel > 0.0f && out.sustainLevel < 1.0f)
            out.decaySamples = static_cast<uint32_t>(logf(out.sustainLevel) / slope);
    }
    out.releaseDecay = MakeReleaseDecay(releaseSeconds, outputRate);
    out.releaseSamples = MakeReleaseSamples(releaseSeconds, outputRate);
    out.vibLfoToPitchCents = static_cast<float>(region.vibLfoToPitch);
    out.vibLfoPhaseStep =
        powf(2.0f, static_cast<float>(region.freqVibLfo) / 1200.0f) / rate;
    const float vibDelaySeconds = TimecentsToSeconds(region.delayVibLfo);
    out.vibLfoDelaySamples = vibDelaySeconds > 0.0f
        ? static_cast<uint32_t>(vibDelaySeconds * rate) : 0u;
    out.filter = PrepareVoiceLowPass(
        region.filterType, region.initialFilterFc, region.initialFilterQ,
        outputRate);
    out.panLeft = 1.0f;
    out.panRight = 1.0f;
    if (channelCache)
        channelCache->ComputeSoundFontPan(region.pan, out.panLeft, out.panRight);
    out.valid = 1u;
}

bool Driver::LoadConfiguredSoundFont() {
    std::string resolutionWarning;
    const std::vector<std::wstring> paths =
        ResolveV3SoundFontPaths(engineConfig_, &resolutionWarning);
    if (!resolutionWarning.empty()) {
        const std::string message =
            "[SVMS] SoundFont configuration warning: " +
            resolutionWarning + "\n";
        OutputDebugStringA(message.c_str());
    }
    bool loaded = false;
    if (!paths.empty()) {
        const uint64_t requestId = soundFontRequestId_.fetch_add(
            1u, std::memory_order_acq_rel) + 1u;
        std::string error;
        EnterCriticalSection(&soundFontBuildCs_);
        SoundFontBundle* bundle = BuildSoundFontStackBundle(
            paths, engineConfig_.soundFontRoutes, requestId, error);
        LeaveCriticalSection(&soundFontBuildCs_);
        if (bundle) {
            PublishSoundFontBundle(bundle);
            if (!audioOutput || !audioOutput->IsRunning()) {
                ActivatePendingSoundFontAtBlockBoundary();
                ReclaimRetiredSoundFonts();
            }
            loaded = true;
        } else if (!error.empty()) {
            OutputDebugStringA(("[SVMS] SoundFont stack load failed: " +
                                error + "\n").c_str());
        }
    }
    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_)) {
        DiagWindow_UpdateStartup(audioOutput && audioOutput->IsRunning(),
                                 audioOutput
                                     ? static_cast<int32_t>(audioOutput->GetLastError())
                                     : 0,
                                 loaded, sampleRate, bufferFrames,
                                 engineConfig_.masterVolume,
                                 UsesXPWaveOut(audioOutput));
    }
    return loaded;
}


// ── Hilbert-pair store construction ─────────────────────────────────────
// One slice transform task; SVMSPhaseRotation.h owns the math. Thread fan-
// out is a plain CreateThread pool (XP-safe, no SRWLock/std::thread): each
// slice is transformed by exactly one thread with the fixed-order double
// FFT, so the store is bit-identical to a serial build regardless of the
// thread count.
namespace {
struct HilbertSliceJob {
    const int16_t* src;
    int16_t* dst;
    const uint32_t* starts;
    const uint32_t* counts;
    std::atomic<uint32_t>* next;
    uint32_t taskCount;
};

DWORD WINAPI HilbertSliceWorker(LPVOID param) noexcept {
    HilbertSliceJob* job = static_cast<HilbertSliceJob*>(param);
    for (;;) {
        const uint32_t task = job->next->fetch_add(1u,
            std::memory_order_relaxed);
        if (task >= job->taskCount) break;
        svms::HilbertTransformSlice(job->src + job->starts[task],
                                    job->dst + job->starts[task],
                                    job->counts[task]);
    }
    return 0u;
}
}  // namespace

static void BuildHilbertPairStore(const SF2Data* sf2, int16_t* hilbert) {
    const uint32_t sampleCount = sf2->sampleCount;
    std::vector<uint32_t> starts;
    std::vector<uint32_t> counts;
    starts.reserve(sampleCount);
    counts.reserve(sampleCount);
    for (uint32_t i = 0u; i < sampleCount; ++i) {
        const SF2Sample& s = sf2->samples[i];
        if (s.end > s.start && s.start < sf2->sampleDataFrames &&
            s.end <= sf2->sampleDataFrames) {
            starts.push_back(s.start);
            counts.push_back(s.end - s.start);
        }
    }
    const uint32_t taskCount = static_cast<uint32_t>(starts.size());
    if (taskCount == 0u) return;

    std::atomic<uint32_t> next{0u};
    HilbertSliceJob job{sf2->sampleData, hilbert, starts.data(), counts.data(),
                        &next, taskCount};

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uint32_t threadCount = sysInfo.dwNumberOfProcessors;
    if (threadCount > 16u) threadCount = 16u;
    if (threadCount > taskCount) threadCount = taskCount;

    if (threadCount <= 1u) {
        HilbertSliceWorker(&job);
        return;
    }
    // WaitForMultipleObjects caps at 64 handles; 16 threads + main is safe.
    HANDLE handles[17];
    uint32_t spawned = 0u;
    for (; spawned + 1u < threadCount; ++spawned) {
        handles[spawned] = CreateThread(nullptr, 0, HilbertSliceWorker,
                                        &job, 0, nullptr);
        if (!handles[spawned]) break;
    }
    HilbertSliceWorker(&job);
    if (spawned != 0u)
        WaitForMultipleObjects(spawned, handles, TRUE, INFINITE);
    while (spawned != 0u) CloseHandle(handles[--spawned]);
}

SoundFontBundle* Driver::BuildSoundFontBundle(const wchar_t* path,
                                               uint64_t requestId,
                                               std::string& error) {
    error.clear();
    if (!path || !*path) {
        error = "no SoundFont path supplied";
        return nullptr;
    }
    SoundFontBundle* bundle = new (std::nothrow) SoundFontBundle();
    SF2Data* sf2 = new (std::nothrow) SF2Data();
    if (!bundle || !sf2) {
        delete bundle;
        delete sf2;
        error = "not enough memory to load SoundFont";
        return nullptr;
    }
    bundle->data = sf2;
    bundle->requestId = requestId;
    bundle->path = path;
    if (!soundfont_load(path, sf2)) {
        LOG("  soundfont_load FAILED: presets=%u inst=%u samples=%u sampleData=%d frames=%u",
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
        error = "SoundFont parse failed";
        DestroySoundFontBundle(bundle);
        return nullptr;
    }
    LOG("  Parsed: %u presets, %u instruments, %u samples, %u frames",
        sf2->presetCount, sf2->instrumentCount, sf2->sampleCount, sf2->sampleDataFrames);

    sf2_build_regions(sf2);
    LOG("  Built %u regions", sf2->regionCount);
    if (sf2->regionOverflow) {
        LOG("  FAILED: SoundFont compiled region capacity exceeded");
        error = "SoundFont compiled region capacity exceeded";
        DestroySoundFontBundle(bundle);
        return nullptr;
    }
    // Diagnostic peak inspection walks up to 512 source samples.  Doing
    // that for every configured voice made diagnostics catastrophically
    // expensive in dense MIDI.  Compile the immutable values once while
    // loading off the audio thread; note-on becomes a single cached read.
    if (sf2->regionCount != 0u) {
        bundle->regionInitialPeaks = static_cast<float*>(
            malloc(static_cast<size_t>(sf2->regionCount) * sizeof(float)));
        if (bundle->regionInitialPeaks) {
            bundle->regionInitialPeakCount = sf2->regionCount;
            for (uint32_t region = 0; region < sf2->regionCount; ++region) {
                bundle->regionInitialPeaks[region] =
                    sf2_region_initial_peak(sf2, &sf2->regions[region]);
            }
        }
        bundle->preparedRegions = static_cast<PreparedSF2Region*>(malloc(
            static_cast<size_t>(sf2->regionCount) * sizeof(PreparedSF2Region)));
        if (bundle->preparedRegions) {
            bundle->preparedRegionCount = sf2->regionCount;
            for (uint32_t region = 0; region < sf2->regionCount; ++region) {
                PrepareSF2Region(sf2, sf2->regions[region], sampleRate,
                                 channelCache, bundle->preparedRegions[region]);
            }
        }
        if (!bundle->preparedRegions) {
            error = "not enough memory to prepare SoundFont regions";
            DestroySoundFontBundle(bundle);
            return nullptr;
        }
    }

    if (sf2->sampleData) {
        const uint32_t frames = sf2->sampleDataFrames;
        // 16-bit sample store.  The previous float store held exact
        // float(int16) values, so rendering converts on load with identical
        // math at half the cache footprint.  Eight zero elements of
        // trailing padding keep the AVX2 pair-word gather in bounds.
        int16_t* sbuf = static_cast<int16_t*>(malloc(
            (static_cast<size_t>(frames) + 8u) * sizeof(int16_t)));
        if (!sbuf) {
            error = "not enough memory to convert SoundFont samples";
            DestroySoundFontBundle(bundle);
            return nullptr;
        }
        std::memcpy(sbuf, sf2->sampleData,
                    static_cast<size_t>(frames) * sizeof(int16_t));
        std::memset(sbuf + frames, 0, 8u * sizeof(int16_t));
        bundle->sampleData = sbuf;
        bundle->sampleDataFrames = frames;

        // Exact analytic companion store for per-voice Hilbert rotation.
        // Build it unconditionally at SoundFont load time so the live
        // Coherent -> Analytic/Sweep/Random switch can NEVER silently
        // downgrade to the quadrature-allpass approximation. The load is
        // off the audio callback and the bundle publishes atomically.
        int16_t* hbuf = static_cast<int16_t*>(malloc(
            (static_cast<size_t>(frames) + 8u) * sizeof(int16_t)));
        if (!hbuf) {
            error = "not enough memory to build SoundFont Hilbert pair";
            DestroySoundFontBundle(bundle);
            return nullptr;
        }
        std::memset(hbuf, 0,
                    (static_cast<size_t>(frames) + 8u) * sizeof(int16_t));
        BuildHilbertPairStore(sf2, hbuf);
        bundle->hilbertData = hbuf;
        LOG("  Hilbert pair built: %u frames", frames);
    }

    const uint32_t sampCount = sf2->sampleCount;
    if (sampCount != 0u) {
        bundle->samples = static_cast<SF2Sample*>(
            malloc(static_cast<size_t>(sampCount) * sizeof(SF2Sample)));
        if (!bundle->samples) {
            error = "not enough memory to cache SoundFont sample headers";
            DestroySoundFontBundle(bundle);
            return nullptr;
        }
        std::memcpy(bundle->samples, sf2->samples,
                    static_cast<size_t>(sampCount) * sizeof(SF2Sample));
    }
    bundle->sampleCount = sampCount;
    if (!bundle->sampleData || bundle->sampleDataFrames == 0u ||
        !bundle->samples || bundle->sampleCount == 0u) {
        error = "SoundFont contains no usable sample data";
        DestroySoundFontBundle(bundle);
        return nullptr;
    }
    // Region compilation and diagnostic peaks are complete. Rendering uses
    // the immutable 16-bit store, so retaining the original 16-bit RIFF blob
    // would only duplicate every loaded bank for its entire lifetime.
    free(sf2->sampleData);
    sf2->sampleData = nullptr;
    sf2->sampleDataSize = 0u;
    LOG("  SoundFont bundle ready: %u samples cached", sampCount);
    return bundle;
}

SoundFontBundle* Driver::BuildSoundFontStackBundle(
    const std::vector<std::wstring>& paths,
    const std::vector<SoundFontRoute>& routes,
    uint64_t requestId, std::string& error) {
    if (paths.empty()) {
        error = "no SoundFont path supplied";
        return nullptr;
    }
    if (paths.size() == 1u && routes.empty())
        return BuildSoundFontBundle(paths.front().c_str(), requestId, error);

    SoundFontBundle* stack = new (std::nothrow) SoundFontBundle();
    if (!stack) {
        error = "not enough memory to create SoundFont stack";
        return nullptr;
    }
    stack->requestId = requestId;
    stack->path = paths.front();
    uint64_t totalFrames = 0u;
    const uint32_t requestedCount = static_cast<uint32_t>((std::min)(
        paths.size(), static_cast<size_t>(kMaxSoundFontStackEntries)));
    for (uint32_t i = 0u; i < requestedCount; ++i) {
        std::string bankError;
        SoundFontBundle* bank = BuildSoundFontBundle(
            paths[i].c_str(), requestId, bankError);
        if (!bank) {
            error = "SoundFont " + std::to_string(i + 1u) + " failed: " +
                    (bankError.empty() ? "load failed" : bankError);
            DestroySoundFontBundle(stack);
            return nullptr;
        }
        if (totalFrames + bank->sampleDataFrames > UINT32_MAX) {
            error = "combined SoundFont sample data exceeds 32-bit offsets";
            DestroySoundFontBundle(bank);
            DestroySoundFontBundle(stack);
            return nullptr;
        }
        bank->sampleBase = static_cast<uint32_t>(totalFrames);
        const uint64_t nextFrames = totalFrames + bank->sampleDataFrames;
        int16_t* grown = static_cast<int16_t*>(realloc(
            stack->sampleData,
            (static_cast<size_t>(nextFrames) + 8u) * sizeof(int16_t)));
        if (!grown) {
            error = "not enough memory to combine SoundFont samples";
            DestroySoundFontBundle(bank);
            DestroySoundFontBundle(stack);
            return nullptr;
        }
        stack->sampleData = grown;
        std::memcpy(stack->sampleData + bank->sampleBase, bank->sampleData,
                    static_cast<size_t>(bank->sampleDataFrames) * sizeof(int16_t));
        std::memset(stack->sampleData + nextFrames, 0, 8u * sizeof(int16_t));
        free(bank->sampleData);
        bank->sampleData = nullptr;
        if (bank->hilbertData) {
            int16_t* grownH = static_cast<int16_t*>(realloc(
                stack->hilbertData,
                (static_cast<size_t>(nextFrames) + 8u) * sizeof(int16_t)));
            if (!grownH) {
                error = "not enough memory to combine SoundFont Hilbert pairs";
                DestroySoundFontBundle(bank);
                DestroySoundFontBundle(stack);
                return nullptr;
            }
            stack->hilbertData = grownH;
            std::memcpy(stack->hilbertData + bank->sampleBase,
                        bank->hilbertData,
                        static_cast<size_t>(bank->sampleDataFrames) *
                            sizeof(int16_t));
            std::memset(stack->hilbertData + nextFrames, 0,
                        8u * sizeof(int16_t));
            free(bank->hilbertData);
            bank->hilbertData = nullptr;
        }
        totalFrames = nextFrames;
        stack->sampleDataFrames = static_cast<uint32_t>(totalFrames);
        stack->banks[stack->bankCount++] = bank;
    }
    for (const SoundFontRoute& route : routes) {
        if (stack->routeCount >= kMaxSoundFontRoutes) break;
        if (route.soundFontIndex >= stack->bankCount) continue;
        stack->routes[stack->routeCount++] = route;
    }
    LOG("  SoundFont stack ready: %u banks, %u routes, %u frames",
        stack->bankCount, stack->routeCount, stack->sampleDataFrames);
    return stack;
}

void Driver::RetireSoundFontBundle(SoundFontBundle* bundle) noexcept {
    if (!bundle) return;
    SoundFontBundle* head = retiredSoundFontBundles_.load(
        std::memory_order_relaxed);
    do {
        bundle->retiredNext = head;
    } while (!retiredSoundFontBundles_.compare_exchange_weak(
        head, bundle, std::memory_order_release, std::memory_order_relaxed));
}

void Driver::ReclaimRetiredSoundFonts() noexcept {
    SoundFontBundle* bundle = retiredSoundFontBundles_.exchange(
        nullptr, std::memory_order_acquire);
    while (bundle) {
        SoundFontBundle* next = bundle->retiredNext;
        DestroySoundFontBundle(bundle);
        bundle = next;
    }
}

std::wstring Driver::CopyActiveSoundFontPath() const {
    SoundFontBundle* bundle = activeSoundFontBundle_.load(
        std::memory_order_acquire);
    return bundle ? bundle->path : std::wstring();
}

void Driver::PublishSoundFontBundle(SoundFontBundle* bundle) noexcept {
    SoundFontBundle* superseded = pendingSoundFontBundle_.exchange(
        bundle, std::memory_order_acq_rel);
    // Publication runs off the callback. A pending bundle that lost the race
    // was never visible to rendering and can be reclaimed immediately here.
    DestroySoundFontBundle(superseded);
}

void Driver::ActivatePendingSoundFontAtBlockBoundary() noexcept {
    SoundFontBundle* next = pendingSoundFontBundle_.exchange(
        nullptr, std::memory_order_acquire);
    if (!next) return;

    // Voice sample locations are offsets into one bundle. Old voices must not
    // survive into the new sample bank; MIDI channel/program/controller state
    // remains intact and is remapped below.
    if (voiceManager) voiceManager->Reset();

    activeSoundFontStack_ = next;
    SoundFontBundle* primary = SoundFontBankAt(next, 0u);
    soundFontData = primary ? primary->data : nullptr;
    sampleDataStore = next->sampleData;
    hilbertDataStore = next->hilbertData;
    // Voices were just reset; new launches seed from the pair availability
    // of the bundle that is becoming active.
    if (voiceManager)
        voiceManager->SetHilbertPairAvailable(next->hilbertData != nullptr);
    samplesStore = primary ? primary->samples : nullptr;
    regionInitialPeaks = primary ? primary->regionInitialPeaks : nullptr;
    preparedRegions = primary ? primary->preparedRegions : nullptr;
    sampleStoreCount = primary ? primary->sampleCount : 0u;
    sampleDataFrames = next->sampleDataFrames;
    regionInitialPeakCount = primary ? primary->regionInitialPeakCount : 0u;
    preparedRegionCount = primary ? primary->preparedRegionCount : 0u;

    if (++soundFontGeneration_ == 0u) {
        soundFontGeneration_ = 1u;
        std::memset(noteLaunchPlanCache_, 0, sizeof(noteLaunchPlanCache_));
    }
    std::memset(noteRegionCache_, 0xff, sizeof(noteRegionCache_));
    RefreshSelectedPresets();
    for (uint32_t channel = 0; channel < kChannelCount; ++channel)
        ++channelLaunchRevision_[channel];
    nextPlayIndex_ = 1u;

    SoundFontBundle* old = activeSoundFontBundle_.exchange(
        next, std::memory_order_release);
    soundFontActivatedId_.store(next->requestId, std::memory_order_release);
#if !defined(SVMS_XP_COMPAT)
    if (soundFontRequestId_.load(std::memory_order_acquire) == next->requestId)
        soundFontLoadState_.store(3u, std::memory_order_release);
#endif
    RetireSoundFontBundle(old);
}

void Driver::DestroyAllSoundFontBundles() noexcept {
    DestroySoundFontBundle(pendingSoundFontBundle_.exchange(
        nullptr, std::memory_order_acq_rel));
    DestroySoundFontBundle(activeSoundFontBundle_.exchange(
        nullptr, std::memory_order_acq_rel));
    ReclaimRetiredSoundFonts();
    soundFontData = nullptr;
    activeSoundFontStack_ = nullptr;
    sampleDataStore = nullptr;
    hilbertDataStore = nullptr;
    samplesStore = nullptr;
    regionInitialPeaks = nullptr;
    preparedRegions = nullptr;
    sampleStoreCount = sampleDataFrames = 0u;
    regionInitialPeakCount = preparedRegionCount = 0u;
}

bool Driver::LoadSoundFont(const wchar_t* path) {
    if (!path || !*path) return false;
    const uint64_t requestId = soundFontRequestId_.fetch_add(
        1u, std::memory_order_acq_rel) + 1u;
    std::string error;
    EnterCriticalSection(&soundFontBuildCs_);
    SoundFontBundle* bundle = BuildSoundFontBundle(path, requestId, error);
    LeaveCriticalSection(&soundFontBuildCs_);
    if (!bundle) return false;
#if !defined(SVMS_XP_COMPAT)
    soundFontLoadState_.store(2u, std::memory_order_release);
#endif
    PublishSoundFontBundle(bundle);
    if (!audioOutput || !audioOutput->IsRunning()) {
        ActivatePendingSoundFontAtBlockBoundary();
#if defined(SVMS_XP_COMPAT)
        ReclaimRetiredSoundFonts();
#endif
    }

    return true;
}

#if !defined(SVMS_XP_COMPAT)
bool Driver::QueueSoundFontLoad(const std::wstring& path,
                                uint64_t& requestId) {
    requestId = 0u;
    if (path.empty() || !soundFontLoadEvent_ ||
        soundFontLoaderStop_.load(std::memory_order_acquire)) return false;
    requestId = soundFontRequestId_.fetch_add(
        1u, std::memory_order_acq_rel) + 1u;
    EnterCriticalSection(&cs);
    requestedSoundFontPath_ = path;
    requestedSoundFontId_ = requestId;
    soundFontLoadError_.clear();
    soundFontLoadState_.store(1u, std::memory_order_release);
    LeaveCriticalSection(&cs);
    SetEvent(soundFontLoadEvent_);
    return true;
}

void Driver::SoundFontLoaderLoop() {
    uint64_t handledId = 0u;
    for (;;) {
        WaitForSingleObject(soundFontLoadEvent_, INFINITE);
        if (soundFontLoaderStop_.load(std::memory_order_acquire)) break;

        for (;;) {
            std::wstring path;
            uint64_t requestId = 0u;
            EnterCriticalSection(&cs);
            requestId = requestedSoundFontId_;
            path = requestedSoundFontPath_;
            LeaveCriticalSection(&cs);
            if (requestId == 0u || requestId == handledId) break;
            handledId = requestId;

            std::string error;
            EnterCriticalSection(&soundFontBuildCs_);
            SoundFontBundle* bundle = BuildSoundFontBundle(
                path.c_str(), requestId, error);
            LeaveCriticalSection(&soundFontBuildCs_);

            if (soundFontLoaderStop_.load(std::memory_order_acquire)) {
                DestroySoundFontBundle(bundle);
                return;
            }
            if (soundFontRequestId_.load(std::memory_order_acquire) !=
                    requestId) {
                DestroySoundFontBundle(bundle);
                continue;
            }
            if (!bundle) {
                EnterCriticalSection(&cs);
                soundFontLoadError_ = error.empty()
                    ? "SoundFont load failed" : error;
                LeaveCriticalSection(&cs);
                soundFontLoadState_.store(4u, std::memory_order_release);
                continue;
            }

            soundFontLoadState_.store(2u, std::memory_order_release);
            PublishSoundFontBundle(bundle);
            if (!audioOutput || !audioOutput->IsRunning()) {
                ActivatePendingSoundFontAtBlockBoundary();
            }
        }
    }
}
#endif

} // namespace svms
