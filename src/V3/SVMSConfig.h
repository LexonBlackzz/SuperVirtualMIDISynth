#ifndef SVMS_CONFIG_H
#define SVMS_CONFIG_H

#include "SVMSTypes.h"
#include <string>

namespace svms {

struct EngineConfig {
    uint32_t sampleRate;
    uint32_t bufferFrames;
    uint32_t maxVoices;
    uint32_t maxSampleCacheMB;
    InterpolationMode interpolation;
    FilterType filterType;
    AudioBackend audioBackend;
    RenderBackend renderBackend;
    PanLaw panLaw;
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
std::wstring ResolveV3SoundFontPath(const EngineConfig& cfg);

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
