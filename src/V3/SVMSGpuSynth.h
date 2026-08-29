#ifndef SVMS_GPU_SYNTH_H
#define SVMS_GPU_SYNTH_H

#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)

#include "SVMSGpuDevice.h"
#include "SVMSTypes.h"
#include "SVMSVoiceManager.h"
#include "SVMSLimiter.h"
#include "SVMSPostFilter.h"
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace svms {

// GPU synthesis stage for the offline renderer. The host retains all
// scheduling, voice-lifecycle, and event-timing semantics; the GPU is a pure
// per-voice sample-synthesis engine.  Each RenderBlock call is for a
// contiguous event-free span (the host dispatches events before/after), so
// no mid-block note-on splitting is needed.
class GpuSynth {
public:
    bool Initialize(const int16_t* sampleData, uint32_t sampleDataFrames,
                    uint32_t maxVoices, uint32_t blockFrames,
                    std::string& error);
    void Destroy();

    void RenderBlock(VoiceManager& voices, float* outL, float* outR,
                     uint32_t numFrames, uint64_t currentFrame);

    const char* AdapterName() const { return device_.AdapterName(); }

private:
    bool EnsureBuffers(uint32_t activeCount, uint32_t blockFrames,
                       std::string& error);
    bool BuildShaders(std::string& error);

    gpu::GpuDevice device_;
    bool initialized_ = false;

    // D3D11 resources.
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> voiceCS_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> reduceCS_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constBuf_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> voiceInBuf_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> voiceOutBuf_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> poolBuf_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> partialBuf_;
    Microsoft::WRL::ComPtr<ID3D11Query> fenceQuery_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> outBuf_;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> voiceInSRV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> poolSRV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> partialSRV_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> partialUAV_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> voiceOutUAV_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outUAV_;

    uint32_t maxVoices_ = 0;
    uint32_t blockFrames_ = 0;
    uint32_t voiceInBufCap_ = 0;
    uint32_t voiceOutBufCap_ = 0;
    uint32_t partialBufCap_ = 0;
    uint32_t outBufCap_ = 0;
    uint32_t viewVoicesKey_ = 0xFFFFFFFFu;
    uint32_t viewFramesKey_ = 0xFFFFFFFFu;
    std::vector<float> sampleData_;
};

} // namespace svms

#endif // !SVMS_XP_COMPAT && _WIN32
#endif // SVMS_GPU_SYNTH_H