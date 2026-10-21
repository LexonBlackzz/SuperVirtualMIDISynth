// SoundFont bundles: preparation, Hilbert pair store, loader thread, swap.

#include "driver/DriverInternal.h"
#include "render/HilbertPair.h"

namespace svms {

// Bundles are refcounted: a bank reused by a derived stack is a view holding
// a reference on the original, so the parsed data outlives whichever stack
// is retired first.
static void DestroySoundFontBundle(SoundFontBundle* bundle) noexcept {
    if (!bundle) return;
    if (bundle->refs.fetch_sub(1u, std::memory_order_acq_rel) != 1u) return;
    if (bundle->viewOf) {
        DestroySoundFontBundle(bundle->viewOf);
        delete bundle;
        return;
    }
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
#if !defined(SVMS_XP_COMPAT)
        EnterCriticalSection(&cs);
        desiredSoundFontPaths_ = paths;
        std::memcpy(desiredChannelFont_, engineConfig_.soundFontChannels,
                    sizeof(desiredChannelFont_));
        LeaveCriticalSection(&cs);
#endif
        EnterCriticalSection(&soundFontBuildCs_);
        SoundFontBundle* bundle = BuildSoundFontStackBundle(
            paths, engineConfig_.soundFontRoutes,
            engineConfig_.soundFontChannels, nullptr, -1, requestId, error);
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
// One slice per task; HilbertPair.h owns the math. Thread fan-out is a
// plain CreateThread pool (XP-safe, no SRWLock/std::thread): each slice is
// transformed by exactly one thread with the fixed-order double FFT, so the
// store is bit-identical to a serial build regardless of the thread count.
namespace {
struct HilbertSliceJob {
    const int16_t* src;
    int16_t* dst;
    const svms::HilbertSliceSpec* specs;
    std::atomic<uint32_t>* next;
    uint32_t taskCount;
};

DWORD WINAPI HilbertSliceWorker(LPVOID param) noexcept {
    HilbertSliceJob* job = static_cast<HilbertSliceJob*>(param);
    svms::hilbert_detail::Scratch scratch;  // reused for every slice we build
    for (;;) {
        const uint32_t task = job->next->fetch_add(1u,
            std::memory_order_relaxed);
        if (task >= job->taskCount) break;
        svms::BuildHilbertSlice(job->src, job->dst, job->specs[task], &scratch);
    }
    return 0u;
}
}  // namespace

// `src`/`hilbert` point at this SoundFont's slice of the (possibly stacked)
// sample store, so spec offsets stay bank-relative.
static void BuildHilbertPairStore(const SF2Data* sf2, const int16_t* src,
                                  int16_t* hilbert) {
    // The shared FFT twiddle tables (tens of MB for long samples) only live
    // for the duration of a build.
    struct DropTables {
        ~DropTables() { svms::hilbert_detail::ClearTwiddleCache(); }
    } dropTables;
    const std::vector<svms::HilbertSliceSpec> specs =
        svms::PlanHilbertSlices(*sf2);
    const uint32_t taskCount = static_cast<uint32_t>(specs.size());
    if (taskCount == 0u) return;

    std::atomic<uint32_t> next{0u};
    HilbertSliceJob job{src, hilbert, specs.data(), &next, taskCount};

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
        // It is the most expensive part of a load (all-core FFTs), so it is
        // built here only when a rotation mode that reads it is live.
        // Switching to Analytic/Sweep/Random later builds it in the
        // background (EnsureHilbertStoreAsync) and the audio thread adopts
        // it at a block boundary.
        if (RotationNeedsHilbert()) {
            int16_t* hbuf = static_cast<int16_t*>(malloc(
                (static_cast<size_t>(frames) + 8u) * sizeof(int16_t)));
            if (!hbuf) {
                error = "not enough memory to build SoundFont Hilbert pair";
                DestroySoundFontBundle(bundle);
                return nullptr;
            }
            std::memset(hbuf, 0,
                        (static_cast<size_t>(frames) + 8u) * sizeof(int16_t));
            BuildHilbertPairStore(sf2, sbuf, hbuf);
            bundle->hilbertData = hbuf;
            LOG("  Hilbert pair built: %u frames", frames);
        }
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

static int16_t* BuildHilbertStoreFor(const SoundFontBundle* bundle);

// A bank carried into a derived stack: shares the original's parsed data
// (refcounted through `viewOf`) but takes its own sampleBase.
static SoundFontBundle* MakeBankView(SoundFontBundle* source) {
    SoundFontBundle* root = source->viewOf ? source->viewOf : source;
    SoundFontBundle* view = new (std::nothrow) SoundFontBundle();
    if (!view) return nullptr;
    root->refs.fetch_add(1u, std::memory_order_relaxed);
    view->viewOf = root;
    view->data = source->data;
    view->samples = source->samples;
    view->regionInitialPeaks = source->regionInitialPeaks;
    view->preparedRegions = source->preparedRegions;
    view->sampleCount = source->sampleCount;
    view->sampleDataFrames = source->sampleDataFrames;
    view->regionInitialPeakCount = source->regionInitialPeakCount;
    view->preparedRegionCount = source->preparedRegionCount;
    view->requestId = source->requestId;
    view->path = source->path;
    return view;
}

SoundFontBundle* Driver::BuildSoundFontStackBundle(
    const std::vector<std::wstring>& paths,
    const std::vector<SoundFontRoute>& routes,
    const uint8_t* channelFont, const SoundFontBundle* reuse, int forceReload,
    uint64_t requestId, std::string& error) {
    if (paths.empty()) {
        error = "no SoundFont path supplied";
        return nullptr;
    }
    bool anyChannelFont = false;
    for (uint32_t c = 0u; channelFont && c < kChannelCount; ++c)
        anyChannelFont = anyChannelFont || channelFont[c] != 0u;
    if (paths.size() == 1u && routes.empty() && !anyChannelFont)
        return BuildSoundFontBundle(paths.front().c_str(), requestId, error);

    SoundFontBundle* stack = new (std::nothrow) SoundFontBundle();
    if (!stack) {
        error = "not enough memory to create SoundFont stack";
        return nullptr;
    }
    stack->requestId = requestId;
    stack->path = paths.front();
    uint64_t totalFrames = 0u;
    bool allHilbert = true;
    const uint32_t requestedCount = static_cast<uint32_t>((std::min)(
        paths.size(), static_cast<size_t>(kMaxSoundFontStackEntries)));
    for (uint32_t i = 0u; i < requestedCount; ++i) {
        // Entries already in the running stack are reused (no parse, no
        // region preparation); only new or force-reloaded paths are built.
        SoundFontBundle* reusable = nullptr;
        for (uint32_t j = 0u; reuse && j < SoundFontBankCount(reuse) &&
                              !reusable && static_cast<int>(i) != forceReload;
             ++j) {
            SoundFontBundle* old = SoundFontBankAt(reuse, j);
            if (old && old->data && _wcsicmp(old->path.c_str(),
                                             paths[i].c_str()) == 0)
                reusable = old;
        }
        const int16_t* srcSamples = nullptr;
        const int16_t* srcHilbert = nullptr;
        SoundFontBundle* bank = nullptr;
        if (reusable) {
            bank = MakeBankView(reusable);
            if (bank) {
                srcSamples = reuse->sampleData + reusable->sampleBase;
                srcHilbert = reuse->hilbertData
                    ? reuse->hilbertData + reusable->sampleBase : nullptr;
            }
        } else {
            std::string bankError;
            bank = BuildSoundFontBundle(paths[i].c_str(), requestId, bankError);
            if (!bank) {
                error = "SoundFont " + std::to_string(i + 1u) + " failed: " +
                        (bankError.empty() ? "load failed" : bankError);
                DestroySoundFontBundle(stack);
                return nullptr;
            }
            srcSamples = bank->sampleData;
            srcHilbert = bank->hilbertData;
        }
        if (!bank) {
            error = "not enough memory to create SoundFont stack entry";
            DestroySoundFontBundle(stack);
            return nullptr;
        }
        if (!srcHilbert) allHilbert = false;
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
        std::memcpy(stack->sampleData + bank->sampleBase, srcSamples,
                    static_cast<size_t>(bank->sampleDataFrames) * sizeof(int16_t));
        std::memset(stack->sampleData + nextFrames, 0, 8u * sizeof(int16_t));
        if (!reusable) {
            free(bank->sampleData);
            bank->sampleData = nullptr;
        }
        if (srcHilbert) {
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
            std::memcpy(stack->hilbertData + bank->sampleBase, srcHilbert,
                        static_cast<size_t>(bank->sampleDataFrames) *
                            sizeof(int16_t));
            std::memset(stack->hilbertData + nextFrames, 0,
                        8u * sizeof(int16_t));
            if (!reusable) {
                free(bank->hilbertData);
                bank->hilbertData = nullptr;
            }
        }
        totalFrames = nextFrames;
        stack->sampleDataFrames = static_cast<uint32_t>(totalFrames);
        stack->banks[stack->bankCount++] = bank;
    }
    // A companion store with a hole would attenuate that bank's rotated
    // voices (x*cos with no x-hat term): keep it all-or-nothing and let the
    // on-demand builder fill it in.
    if (!allHilbert) {
        free(stack->hilbertData);
        stack->hilbertData = nullptr;
        if (RotationNeedsHilbert())
            stack->hilbertData = BuildHilbertStoreFor(stack);
    }
    for (const SoundFontRoute& route : routes) {
        if (stack->routeCount >= kMaxSoundFontRoutes) break;
        if (route.soundFontIndex >= stack->bankCount) continue;
        stack->routes[stack->routeCount++] = route;
    }
    for (uint32_t c = 0u; channelFont && c < kChannelCount; ++c)
        stack->channelFont[c] = channelFont[c] <= stack->bankCount
            ? channelFont[c] : 0u;
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

// ── Lazy Hilbert store ──────────────────────────────────────────────────
// Builds the companion for the ACTIVE bundle on a background thread. Holding
// soundFontBuildCs_ keeps loads and bundle reclamation (which takes the same
// lock) from freeing the bundle mid-build.
// Companion store (malloc'd, 8-element padded) for a plain or stack bundle.
static int16_t* BuildHilbertStoreFor(const SoundFontBundle* bundle) {
    const size_t padded = static_cast<size_t>(bundle->sampleDataFrames) + 8u;
    int16_t* hbuf = static_cast<int16_t*>(malloc(padded * sizeof(int16_t)));
    if (!hbuf) return nullptr;
    std::memset(hbuf, 0, padded * sizeof(int16_t));
    if (bundle->bankCount == 0u) {
        BuildHilbertPairStore(bundle->data, bundle->sampleData, hbuf);
    } else {
        for (uint32_t i = 0u; i < bundle->bankCount; ++i) {
            const SoundFontBundle* bank = bundle->banks[i];
            if (!bank || !bank->data) continue;
            BuildHilbertPairStore(bank->data,
                                  bundle->sampleData + bank->sampleBase,
                                  hbuf + bank->sampleBase);
        }
    }
    return hbuf;
}

void Driver::BuildHilbertForActiveBundle() {
    EnterCriticalSection(&soundFontBuildCs_);
    SoundFontBundle* bundle = activeSoundFontBundle_.load(
        std::memory_order_acquire);
    if (bundle && !bundle->hilbertData && bundle->sampleData &&
        bundle->sampleDataFrames != 0u) {
        int16_t* hbuf = BuildHilbertStoreFor(bundle);
        if (hbuf) {
            free(discardedHilbertStore_.exchange(nullptr,
                                                 std::memory_order_acq_rel));
            free(pendingHilbertStore_.exchange(nullptr,
                                               std::memory_order_acq_rel));
            pendingHilbertFor_ = bundle;
            pendingHilbertStore_.store(hbuf, std::memory_order_release);
            LOG("  Hilbert pair built on demand: %u frames",
                bundle->sampleDataFrames);
        }
    }
    LeaveCriticalSection(&soundFontBuildCs_);
}

static DWORD WINAPI HilbertBuilderThread(LPVOID param) noexcept {
    Driver* driver = static_cast<Driver*>(param);
    driver->BuildHilbertForActiveBundle();
    return 0u;
}

void Driver::JoinHilbertBuilder() noexcept {
    if (!hilbertThread_) return;
    WaitForSingleObject(hilbertThread_, INFINITE);
    CloseHandle(hilbertThread_);
    hilbertThread_ = nullptr;
    hilbertBuildBusy_.store(false, std::memory_order_release);
}

// Control-thread call after a rotation mode that needs the store goes live.
void Driver::EnsureHilbertStoreAsync() {
    SoundFontBundle* bundle = activeSoundFontBundle_.load(
        std::memory_order_acquire);
    if (bundle && bundle->hilbertData) return;
    if (hilbertThread_ && WaitForSingleObject(hilbertThread_, 0) ==
            WAIT_OBJECT_0) {
        CloseHandle(hilbertThread_);
        hilbertThread_ = nullptr;
    }
    if (hilbertThread_) return;  // a build is already running
    hilbertThread_ = CreateThread(nullptr, 0, HilbertBuilderThread, this, 0,
                                  nullptr);
}

void Driver::AdoptPendingHilbertAtBlockBoundary() noexcept {
    int16_t* store = pendingHilbertStore_.exchange(
        nullptr, std::memory_order_acquire);
    if (!store) return;
    SoundFontBundle* bundle = activeSoundFontBundle_.load(
        std::memory_order_acquire);
    if (bundle != pendingHilbertFor_ || bundle->hilbertData) {
        // The bank changed (or already has one) while this was building.
        // Park it for the builder to free; no free() on the audio thread.
        discardedHilbertStore_.store(store, std::memory_order_release);
        return;
    }
    bundle->hilbertData = store;
    hilbertDataStore = store;
    for (Driver* shard : shards_) shard->hilbertDataStore = store;
}

void Driver::ReclaimRetiredSoundFonts() noexcept {
    EnterCriticalSection(&soundFontBuildCs_);
    SoundFontBundle* bundle = retiredSoundFontBundles_.exchange(
        nullptr, std::memory_order_acquire);
    while (bundle) {
        SoundFontBundle* next = bundle->retiredNext;
        DestroySoundFontBundle(bundle);
        bundle = next;
    }
    LeaveCriticalSection(&soundFontBuildCs_);
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
    AdoptPendingHilbertAtBlockBoundary();
    SoundFontBundle* next = pendingSoundFontBundle_.exchange(
        nullptr, std::memory_order_acquire);
    if (!next) return;

    ApplySoundFontStack(next);
    // Headless shards render from the same immutable bundle; they adopt it
    // here, before any shard renders another block.
    for (Driver* shard : shards_) shard->ApplySoundFontStack(next);

    SoundFontBundle* old = activeSoundFontBundle_.exchange(
        next, std::memory_order_release);
    soundFontActivatedId_.store(next->requestId, std::memory_order_release);
#if !defined(SVMS_XP_COMPAT)
    if (soundFontRequestId_.load(std::memory_order_acquire) == next->requestId)
        soundFontLoadState_.store(3u, std::memory_order_release);
#endif
    RetireSoundFontBundle(old);
}

// Points this engine core at `next` (borrowed, not owned) and resets what
// depends on the bank. Shared by master activation and shard adoption.
void Driver::ApplySoundFontStack(SoundFontBundle* next) noexcept {
    // Voice sample locations are offsets into one bundle. Old voices must not
    // survive into the new sample bank; MIDI channel/program/controller state
    // remains intact and is remapped below.
    if (voiceManager) voiceManager->Reset();

    activeSoundFontStack_ = next;
    // Note merging may reorder note-ons within one frame (NoteMerge.h);
    // an exclusive class (hi-hat choke) makes launch order audible, so such
    // a SoundFont limits merging to adjacent note-ons.
    {
        bool exclusiveClasses = false;
        for (uint32_t bankIndex = 0u;
             bankIndex < SoundFontBankCount(next) && !exclusiveClasses;
             ++bankIndex) {
            const SoundFontBundle* bank = SoundFontBankAt(next, bankIndex);
            const SF2Data* data = bank ? bank->data : nullptr;
            if (!data || !data->regions) continue;
            for (uint32_t region = 0u; region < data->regionCount; ++region) {
                if (data->regions[region].exclusiveClass != 0 ||
                    data->regions[region].offByClass != 0) {
                    exclusiveClasses = true;
                    break;
                }
            }
        }
        noteMergeHoist_.store(!exclusiveClasses, std::memory_order_relaxed);
    }
    SoundFontBundle* primary = SoundFontBankAt(next, 0u);
    soundFontData = primary ? primary->data : nullptr;
    sampleDataStore = next->sampleData;
    hilbertDataStore = next->hilbertData;
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
}

void Driver::DestroyAllSoundFontBundles() noexcept {
    JoinHilbertBuilder();
    free(pendingHilbertStore_.exchange(nullptr));
    free(discardedHilbertStore_.exchange(nullptr));
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
    EnterCriticalSection(&cs);
    desiredSoundFontPaths_.assign(1u, path);
    LeaveCriticalSection(&cs);
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
    requestedIsStack_ = false;
    requestedSoundFontId_ = requestId;
    desiredSoundFontPaths_.assign(1u, path);
    soundFontLoadError_.clear();
    soundFontLoadState_.store(1u, std::memory_order_release);
    LeaveCriticalSection(&cs);
    SetEvent(soundFontLoadEvent_);
    return true;
}

bool Driver::QueueSoundFontStack(uint64_t& requestId, int forceReload) {
    requestId = 0u;
    if (!soundFontLoadEvent_ ||
        soundFontLoaderStop_.load(std::memory_order_acquire)) return false;
    requestId = soundFontRequestId_.fetch_add(
        1u, std::memory_order_acq_rel) + 1u;
    EnterCriticalSection(&cs);
    requestedIsStack_ = true;
    requestedForceReload_ = forceReload;
    requestedStackPaths_ = desiredSoundFontPaths_;
    std::memcpy(requestedChannelFont_, desiredChannelFont_,
                sizeof(requestedChannelFont_));
    requestedSoundFontId_ = requestId;
    soundFontLoadError_.clear();
    soundFontLoadState_.store(1u, std::memory_order_release);
    LeaveCriticalSection(&cs);
    SetEvent(soundFontLoadEvent_);
    return true;
}

bool Driver::EditSoundFontEntry(uint32_t entry, const std::wstring& path,
                                std::string& text) {
    uint64_t requestId = 0u;
    EnterCriticalSection(&cs);
    const uint32_t count = static_cast<uint32_t>(
        desiredSoundFontPaths_.size());
    bool ok = true;
    if (entry > count || entry >= kMaxSoundFontStackEntries) {
        text = "SoundFont entry index out of range";
        ok = false;
    } else if (path.empty()) {
        if (entry >= count || count == 1u) {
            text = entry >= count ? "no such SoundFont entry"
                                  : "cannot remove the only SoundFont";
            ok = false;
        } else {
            desiredSoundFontPaths_.erase(desiredSoundFontPaths_.begin() + entry);
            // Assignments to the removed entry go back to automatic; later
            // entries shift down by one.
            for (uint32_t c = 0u; c < kChannelCount; ++c) {
                if (desiredChannelFont_[c] == entry + 1u) desiredChannelFont_[c] = 0u;
                else if (desiredChannelFont_[c] > entry + 1u) --desiredChannelFont_[c];
            }
        }
    } else if (entry == count) {
        desiredSoundFontPaths_.push_back(path);
    } else {
        desiredSoundFontPaths_[entry] = path;
    }
    LeaveCriticalSection(&cs);
    if (!ok) return false;
    // A replace always rebuilds that entry, even for an unchanged path
    // (re-read a file edited on disk).
    const int force = (!path.empty() && entry < count)
        ? static_cast<int>(entry) : -1;
    if (!QueueSoundFontStack(requestId, force)) {
        text = "SoundFont loader is unavailable";
        return false;
    }
    text = "loading request " + std::to_string(requestId);
    return true;
}

bool Driver::AssignChannelSoundFont(uint32_t channel, uint32_t font,
                                    std::string& text) {
    uint64_t requestId = 0u;
    EnterCriticalSection(&cs);
    bool ok = channel < kChannelCount &&
              font <= desiredSoundFontPaths_.size();
    if (ok) desiredChannelFont_[channel] = static_cast<uint8_t>(font);
    LeaveCriticalSection(&cs);
    if (!ok) {
        text = "channel or SoundFont entry out of range";
        return false;
    }
    if (!QueueSoundFontStack(requestId, -1)) {
        text = "SoundFont loader is unavailable";
        return false;
    }
    text = "loading request " + std::to_string(requestId);
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
            bool isStack = false;
            int forceReload = -1;
            std::vector<std::wstring> stackPaths;
            uint8_t channelFont[kChannelCount];
            EnterCriticalSection(&cs);
            requestId = requestedSoundFontId_;
            path = requestedSoundFontPath_;
            isStack = requestedIsStack_;
            forceReload = requestedForceReload_;
            stackPaths = requestedStackPaths_;
            std::memcpy(channelFont, requestedChannelFont_, sizeof(channelFont));
            LeaveCriticalSection(&cs);
            if (requestId == 0u || requestId == handledId) break;
            handledId = requestId;

            std::string error;
            EnterCriticalSection(&soundFontBuildCs_);
            SoundFontBundle* bundle = isStack
                ? BuildSoundFontStackBundle(
                      stackPaths, engineConfig_.soundFontRoutes, channelFont,
                      activeSoundFontBundle_.load(std::memory_order_acquire),
                      forceReload, requestId, error)
                : BuildSoundFontBundle(path.c_str(), requestId, error);
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
