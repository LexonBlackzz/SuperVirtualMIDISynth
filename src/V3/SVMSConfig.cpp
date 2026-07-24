#include "SVMSConfig.h"

namespace svms {

EngineConfig EngineConfig::Default() {
    EngineConfig cfg{};
    cfg.sampleRate = kDefaultSampleRate;
    cfg.bufferFrames = kDefaultBufferFrames;
    cfg.maxVoices = kMaxVoicesDefault;
    cfg.maxSampleCacheMB = 256;
    cfg.interpolation = InterpolationMode::Linear;
    cfg.filterType = FilterType::None;
    cfg.audioBackend = AudioBackend::WASAPIShared;
    cfg.renderBackend = RenderBackend::Scalar;
    cfg.panLaw = PanLaw::ConstantPower;
    cfg.masterVolume = 0.1f;
    cfg.velocityCurve = 1.0f;
    cfg.velocityFloor = 0.0f;
    cfg.velocityIgnoreBelow = 0;
    cfg.ignoreVelocity = false;
    cfg.monoOutput = false;
    cfg.enableReverb = false;
    cfg.enableChorus = false;
    cfg.enableFilter = false;
    cfg.enableModulators = false;
    cfg.gpuDeviceIndex = 0;
    cfg.enableGPU = false;
    return cfg;
}

bool EngineConfig::Validate() const {
    if (sampleRate < 8000 || sampleRate > 384000) return false;
    if (bufferFrames < 16 || bufferFrames > 8192) return false;
    if (maxVoices < 1 || maxVoices > 1000000) return false;
    if (masterVolume < 0.0f || masterVolume > 4.0f) return false;
    if (velocityCurve < 0.1f || velocityCurve > 10.0f) return false;
    if (velocityIgnoreBelow > 127) return false;
    return true;
}

} // namespace svms
