#ifndef SVMS_CONFIG_H
#define SVMS_CONFIG_H

#include "SVMSTypes.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace svms {

enum class LimiterAlgorithm : uint32_t {
    Classic = 0u,
    Adaptive = 1u,
};

struct EngineConfig {
    uint32_t sampleRate;
    uint32_t bufferFrames;
    uint32_t maxVoices;
    // Total voice-render threads. 1 preserves the original audio-thread-only
    // path; 0 selects a conservative automatic count.
    uint32_t renderThreads;
    uint32_t maxSampleCacheMB;
    InterpolationMode interpolation;
    FilterType filterType;
    AudioBackend audioBackend;
    RenderBackend renderBackend;
    PanLaw panLaw;
    float masterVolume;
    bool limiterEnabled;
    LimiterAlgorithm limiterAlgorithm;
    float limiterThreshold;
    float limiterLookaheadMs;
    float limiterAttackMs;
    float limiterReleaseMs;
    float velocityCurve;
    float velocityFloor;
    uint8_t velocityIgnoreBelow;
    bool ignoreVelocity;
    bool monoOutput;
    bool enableReverb;
    float reverbMix;
    float reverbRoomSize;
    float reverbDecay;
    float reverbDamping;
    float reverbWidth;
    float reverbDiffusion;
    float reverbPreDelayMs;
    float reverbEarlyLevel;
    float reverbLateLevel;
    float reverbModDepth;
    float reverbModRate;
    float reverbLowCutHz;
    float reverbHighCutHz;
    bool enableChorus;
    bool enableFilter;
    bool enableModulators;
    uint32_t gpuDeviceIndex;
    bool enableGPU;
    uint32_t eventRingCapacity;
    EventOverflowMode eventOverflowMode;
    uint32_t highPriorityVelocity;
    uint32_t shedStartPercent;
    uint32_t maxEventsPerBlock;
    bool correctnessMode;
    bool diagnosticsEnabled;
    bool diagnosticsWindow;
    bool diagnosticsDebugOutput;
    // "default" (and legacy empty values) select the current Windows default
    // render endpoint. Otherwise this is an exact (case-insensitive) WASAPI
    // endpoint friendly name.
    std::wstring audioDevice;
    std::wstring soundFontPath;
    std::wstring configPath;
    std::string configWarning;

    static EngineConfig Default();
    static EngineConfig Load();
    bool Validate() const;
};

// Active V3 configuration location. A portable config beside winmm.dll takes
// precedence over the Roaming AppData configuration.
std::wstring GetV3ConfigPath();
std::wstring GetV3LocalConfigPath();
std::wstring GetV3AppDataConfigPath();
std::wstring GetV3ModuleDirectory();
// An explicit absolute or DLL-relative synth.soundfont wins. If it is absent
// or missing, deterministically discover .sf2 files beside winmm.dll.
std::wstring ResolveV3SoundFontPath(const EngineConfig& cfg,
                                    std::string* warning = nullptr);

struct RuntimeConfigSnapshot {
    float masterVolume;
    float velocityCurve;
    float velocityFloor;
    uint8_t velocityIgnoreBelow;
    bool ignoreVelocity;
    bool monoOutput;
    bool enableReverb;
    bool enableChorus;
    bool enableFilter;
    bool enableModulators;
    InterpolationMode interpolation;
    FilterType filterType;
    PanLaw panLaw;
    bool correctnessMode;
};

// ── Atomic process-local live-config mailbox ───────────────────────────
// Every field is std::atomic so the audio thread's seqlock-guarded copy
// never performs a non-atomic read while the control thread is writing.
// Writer (control thread): bump liveMailboxSeq_ to ODD, store the fields,
//   then bump to EVEN (release) — the sequence only ever increases, so no
//   odd/even ABA and no torn capture can be mistaken for a settled one.
// Reader (audio thread): load the sequence once per render block; copy
//   only when even and unchanged after the copy; skip DSP application
//   entirely when the sequence equals the last applied value.

// Plain (non-atomic) mirror of the live mailbox, used by the control
// thread as the last-applied echo and by the audio thread as its applied
// record.  Never shared across threads directly.
struct NonAtomicLiveConfigMailbox {
    float masterVolume = 1.0f;
    bool  correctnessMode = false;
    uint32_t maxVoices = 1000u;
    bool  reverbEnabled = false;
    float reverbMix = 0.25f;
    float reverbRoomSize = 0.60f;
    float reverbDecay = 0.50f;
    float reverbDamping = 0.35f;
    float reverbWidth = 1.0f;
    float reverbDiffusion = 0.70f;
    float reverbPreDelayMs = 12.0f;
    float reverbEarlyLevel = 0.35f;
    float reverbLateLevel = 0.85f;
    float reverbModDepth = 0.30f;
    float reverbModRate = 0.35f;
    float reverbLowCutHz = 70.0f;
    float reverbHighCutHz = 16000.0f;
    bool  limiterEnabled = true;
    uint32_t limiterAlgorithm = static_cast<uint32_t>(LimiterAlgorithm::Classic);
    float limiterThreshold = 0.95f;
    float limiterAttackCoeff = 0.25f;
    float limiterReleaseCoeff = 0.001f;
    uint32_t limiterDelayFrames = 128;
};

struct LiveConfigMailbox {
    // Master / engine limits
    std::atomic<float> masterVolume{1.0f};
    std::atomic<bool>  correctnessMode{false};
    std::atomic<uint32_t> maxVoices{1000u};

    // Reverb
    std::atomic<bool>  reverbEnabled{false};
    std::atomic<float> reverbMix{0.25f};
    std::atomic<float> reverbRoomSize{0.60f};
    std::atomic<float> reverbDecay{0.50f};
    std::atomic<float> reverbDamping{0.35f};
    std::atomic<float> reverbWidth{1.0f};
    std::atomic<float> reverbDiffusion{0.70f};
    std::atomic<float> reverbPreDelayMs{12.0f};
    std::atomic<float> reverbEarlyLevel{0.35f};
    std::atomic<float> reverbLateLevel{0.85f};
    std::atomic<float> reverbModDepth{0.30f};
    std::atomic<float> reverbModRate{0.35f};
    std::atomic<float> reverbLowCutHz{70.0f};
    std::atomic<float> reverbHighCutHz{16000.0f};

    // Limiter
    std::atomic<bool>     limiterEnabled{true};
    std::atomic<uint32_t> limiterAlgorithm{static_cast<uint32_t>(LimiterAlgorithm::Classic)};
    std::atomic<float>    limiterThreshold{0.95f};
    std::atomic<float>    limiterAttackCoeff{0.25f};
    std::atomic<float>    limiterReleaseCoeff{0.001f};
    std::atomic<uint32_t> limiterDelayFrames{128};

    void InitFromEngineConfig(const EngineConfig& cfg, uint32_t sampleRate) {
        masterVolume.store(cfg.masterVolume, std::memory_order_relaxed);
        correctnessMode.store(cfg.correctnessMode, std::memory_order_relaxed);
        maxVoices.store(cfg.maxVoices, std::memory_order_relaxed);

        reverbEnabled.store(cfg.enableReverb, std::memory_order_relaxed);
        reverbMix.store(cfg.reverbMix, std::memory_order_relaxed);
        reverbRoomSize.store(cfg.reverbRoomSize, std::memory_order_relaxed);
        reverbDecay.store(cfg.reverbDecay, std::memory_order_relaxed);
        reverbDamping.store(cfg.reverbDamping, std::memory_order_relaxed);
        reverbWidth.store(cfg.reverbWidth, std::memory_order_relaxed);
        reverbDiffusion.store(cfg.reverbDiffusion, std::memory_order_relaxed);
        reverbPreDelayMs.store(cfg.reverbPreDelayMs, std::memory_order_relaxed);
        reverbEarlyLevel.store(cfg.reverbEarlyLevel, std::memory_order_relaxed);
        reverbLateLevel.store(cfg.reverbLateLevel, std::memory_order_relaxed);
        reverbModDepth.store(cfg.reverbModDepth, std::memory_order_relaxed);
        reverbModRate.store(cfg.reverbModRate, std::memory_order_relaxed);
        reverbLowCutHz.store(cfg.reverbLowCutHz, std::memory_order_relaxed);
        reverbHighCutHz.store(cfg.reverbHighCutHz, std::memory_order_relaxed);

        limiterEnabled.store(cfg.limiterEnabled, std::memory_order_relaxed);
        limiterAlgorithm.store(static_cast<uint32_t>(cfg.limiterAlgorithm),
                               std::memory_order_relaxed);
        limiterThreshold.store(cfg.limiterThreshold, std::memory_order_relaxed);
        limiterDelayFrames.store((std::min)(8192u,
            (std::max)(1u, static_cast<uint32_t>(
                cfg.limiterLookaheadMs * sampleRate * 0.001f + 0.5f))),
            std::memory_order_relaxed);
        float attackSamples = (std::max)(1.0f,
            cfg.limiterAttackMs * sampleRate * 0.001f);
        float releaseSamples = (std::max)(1.0f,
            cfg.limiterReleaseMs * sampleRate * 0.001f);
        limiterAttackCoeff.store(1.0f - std::exp(-1.0f / attackSamples),
                                 std::memory_order_relaxed);
        limiterReleaseCoeff.store(1.0f - std::exp(-1.0f / releaseSamples),
                                  std::memory_order_relaxed);
    }

    // Field-by-field copy (LiveConfigMailbox is not copy-assignable
    // because of its atomic members).  Memory_order_relaxed is correct
    // for both directions: the caller owns the seqlock sync.
    void StoreToNonAtomic(svms::NonAtomicLiveConfigMailbox& out) const {
        out.masterVolume = masterVolume.load(std::memory_order_relaxed);
        out.correctnessMode = correctnessMode.load(std::memory_order_relaxed);
        out.maxVoices = maxVoices.load(std::memory_order_relaxed);
        out.reverbEnabled = reverbEnabled.load(std::memory_order_relaxed);
        out.reverbMix = reverbMix.load(std::memory_order_relaxed);
        out.reverbRoomSize = reverbRoomSize.load(std::memory_order_relaxed);
        out.reverbDecay = reverbDecay.load(std::memory_order_relaxed);
        out.reverbDamping = reverbDamping.load(std::memory_order_relaxed);
        out.reverbWidth = reverbWidth.load(std::memory_order_relaxed);
        out.reverbDiffusion = reverbDiffusion.load(std::memory_order_relaxed);
        out.reverbPreDelayMs = reverbPreDelayMs.load(std::memory_order_relaxed);
        out.reverbEarlyLevel = reverbEarlyLevel.load(std::memory_order_relaxed);
        out.reverbLateLevel = reverbLateLevel.load(std::memory_order_relaxed);
        out.reverbModDepth = reverbModDepth.load(std::memory_order_relaxed);
        out.reverbModRate = reverbModRate.load(std::memory_order_relaxed);
        out.reverbLowCutHz = reverbLowCutHz.load(std::memory_order_relaxed);
        out.reverbHighCutHz = reverbHighCutHz.load(std::memory_order_relaxed);
        out.limiterEnabled = limiterEnabled.load(std::memory_order_relaxed);
        out.limiterAlgorithm = limiterAlgorithm.load(std::memory_order_relaxed);
        out.limiterThreshold = limiterThreshold.load(std::memory_order_relaxed);
        out.limiterAttackCoeff = limiterAttackCoeff.load(std::memory_order_relaxed);
        out.limiterReleaseCoeff = limiterReleaseCoeff.load(std::memory_order_relaxed);
        out.limiterDelayFrames = limiterDelayFrames.load(std::memory_order_relaxed);
    }
};

} // namespace svms

#endif