#ifndef SVMS_GPU_DEVICE_H
#define SVMS_GPU_DEVICE_H

#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace svms {
namespace gpu {

// Minimal D3D11 compute abstraction. The GPU is a pure sample-synthesis stage:
// it owns no scheduler state, no voice lifecycle, and no MIDI semantics. A
// host-supplied, fully-resolved per-block voice plan is uploaded and a fixed
// compute pipeline renders it. Everything here is explicitly non-realtime for
// the offline proof path; the realtime integration adds pipelining later.
class GpuDevice {
public:
    bool Create(std::string& error);
    void Destroy();

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

    // Compile a compute shader from HLSL source. target e.g. "cs_5_0".
    bool CompileCompute(const char* source, const char* entry,
                        const char* target, ID3D11ComputeShader** out,
                        std::string& error);

    // StructuredBuffer helpers.
    bool CreateStructBuffer(const void* data, uint32_t bytes,
                            uint32_t stride, ID3D11Buffer** out,
                            std::string& error);
    bool CreateRWStructBuffer(uint32_t bytes, uint32_t stride,
                              ID3D11Buffer** out, std::string& error);
    // A dynamic uniform buffer holding `size` bytes (caller uploads via
    // UpdateConstants).
    bool CreateConstantBuffer(uint32_t size, ID3D11Buffer** out,
                              std::string& error);
    void UpdateConstants(ID3D11Buffer* buf, const void* data, uint32_t bytes);

    // Read back the whole contents of a staging copy of `src` into `dst`.
    // `dst` must be at least `bytes` long.
    bool Readback(ID3D11Buffer* src, uint32_t bytes, void* dst,
                  std::string& error);

    const char* AdapterName() const { return adapterName_.c_str(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> staging_;
    uint32_t stagingCap_ = 0;
    std::string adapterName_;
};

} // namespace gpu
} // namespace svms

#endif // !SVMS_XP_COMPAT && _WIN32
#endif // SVMS_GPU_DEVICE_H