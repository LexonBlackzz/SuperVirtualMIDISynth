#ifndef SVMS_CONFIG_H
#define SVMS_CONFIG_H

#include "SVMSTypes.h"

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

    static EngineConfig Default();
    bool Validate() const;
};

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
};

} // namespace svms

#endif
