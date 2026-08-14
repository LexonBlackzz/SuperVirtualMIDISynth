#ifndef SVMS_CONFIG_H
#define SVMS_CONFIG_H

#include "SVMSTypes.h"
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
    float reverbDamping;
    float reverbWidth;
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

} // namespace svms

#endif
