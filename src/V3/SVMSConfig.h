#ifndef SVMS_CONFIG_H
#define SVMS_CONFIG_H

#include "SVMSTypes.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace svms {

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
// POD struct of all live-tweakable parameters.  Published via a double-
// buffer + atomic pointer pattern so the audio thread reads a consistent
// snapshot without locks or spinlocks.
//
// Writer (control thread):  write to back buffer, then atomic_store the
//   front pointer with release semantics.
// Reader (audio thread):    atomic_load the front pointer with acquire
//   semantics once per render block.
struct LiveConfigMailbox {
    // Master
    float masterVolume    = 1.0f;
    bool correctnessMode  = false;

    // Reverb
    bool  reverbEnabled   = false;
    float reverbMix       = 0.25f;
    float reverbRoomSize  = 0.60f;
    float reverbDecay     = 0.50f;
    float reverbDamping   = 0.35f;
    float reverbWidth     = 1.0f;
    float reverbDiffusion = 0.70f;
    float reverbPreDelayMs    = 12.0f;
    float reverbEarlyLevel    = 0.35f;
    float reverbLateLevel     = 0.85f;
    float reverbModDepth      = 0.30f;
    float reverbModRate       = 0.35f;
    float reverbLowCutHz      = 70.0f;
    float reverbHighCutHz     = 16000.0f;

    // Limiter
    bool     limiterEnabled      = true;
    float    limiterThreshold    = 0.95f;
    float    limiterAttackCoeff  = 0.25f;
    float    limiterReleaseCoeff = 0.001f;
    uint32_t limiterDelayFrames  = 128;

    void InitFromEngineConfig(const EngineConfig& cfg, uint32_t sampleRate) {
        masterVolume   = cfg.masterVolume;
        correctnessMode = cfg.correctnessMode;

        reverbEnabled   = cfg.enableReverb;
        reverbMix       = cfg.reverbMix;
        reverbRoomSize  = cfg.reverbRoomSize;
        reverbDecay     = cfg.reverbDecay;
        reverbDamping   = cfg.reverbDamping;
        reverbWidth     = cfg.reverbWidth;
        reverbDiffusion = cfg.reverbDiffusion;
        reverbPreDelayMs    = cfg.reverbPreDelayMs;
        reverbEarlyLevel    = cfg.reverbEarlyLevel;
        reverbLateLevel     = cfg.reverbLateLevel;
        reverbModDepth      = cfg.reverbModDepth;
        reverbModRate       = cfg.reverbModRate;
        reverbLowCutHz      = cfg.reverbLowCutHz;
        reverbHighCutHz     = cfg.reverbHighCutHz;

        limiterEnabled      = cfg.limiterEnabled;
        limiterThreshold    = cfg.limiterThreshold;
        limiterDelayFrames  = (std::min)(128u,
            (std::max)(1u, static_cast<uint32_t>(
                cfg.limiterLookaheadMs * sampleRate * 0.001f + 0.5f)));
        float attackSamples = (std::max)(1.0f,
            cfg.limiterAttackMs * sampleRate * 0.001f);
        float releaseSamples = (std::max)(1.0f,
            cfg.limiterReleaseMs * sampleRate * 0.001f);
        limiterAttackCoeff  = 1.0f - std::exp(-1.0f / attackSamples);
        limiterReleaseCoeff = 1.0f - std::exp(-1.0f / releaseSamples);
    }
};

} // namespace svms

#endif
