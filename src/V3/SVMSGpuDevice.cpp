#include "SVMSGpuDevice.h"

#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)

#include <cstring>
#include <windows.h>

namespace svms {
namespace gpu {

bool GpuDevice::Create(std::string& error) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                  D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(
        nullptr,                     // default adapter
        D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
        static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        // Fall back to WARP (software rasterizer) so the proof path can run on
        // machines without a usable D3D11 driver.
        device_.Reset();
        context_.Reset();
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
            static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
            D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (FAILED(hr)) {
            error = "D3D11CreateDevice failed (hardware and WARP): " +
                    std::to_string(static_cast<unsigned long>(hr));
            return false;
        }
        adapterName_ = "WARP (software)";
    } else {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&dxgiDev))) &&
            SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                char narrow[512];
                const int len =
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                        narrow, (int)sizeof(narrow), nullptr,
                                        nullptr);
                if (len > 0) adapterName_ = narrow;
            }
        }
        if (adapterName_.empty()) adapterName_ = "hardware d3d11";
    }
    return true;
}

void GpuDevice::Destroy() {
    staging_.Reset();
    stagingCap_ = 0;
    context_.Reset();
    device_.Reset();
    adapterName_.clear();
}

bool GpuDevice::CompileCompute(const char* source, const char* entry,
                               const char* target, ID3D11ComputeShader** out,
                               std::string& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr,
                                  nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob,
                                  &errors);
    if (FAILED(hr)) {
        if (errors) {
            error.assign(static_cast<const char*>(errors->GetBufferPointer()),
                         errors->GetBufferSize());
        } else {
            error = "D3DCompile failed: " +
                    std::to_string(static_cast<unsigned long>(hr));
        }
        return false;
    }
    const HRESULT h2 =
        device_->CreateComputeShader(blob->GetBufferPointer(),
                                     blob->GetBufferSize(), nullptr, out);
    if (FAILED(h2)) {
        error = "CreateComputeShader failed: " +
                std::to_string(static_cast<unsigned long>(h2));
        return false;
    }
    return true;
}

bool GpuDevice::CreateStructBuffer(const void* data, uint32_t bytes,
                                   uint32_t stride, ID3D11Buffer** out,
                                   std::string& error) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    const HRESULT hr = device_->CreateBuffer(&desc, data ? &init : nullptr,
                                             out);
    if (FAILED(hr)) {
        error = "CreateBuffer(structured/srv) failed: " +
                std::to_string(static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool GpuDevice::CreateRWStructBuffer(uint32_t bytes, uint32_t stride,
                                     ID3D11Buffer** out, std::string& error) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // A buffer written by one pass frequently feeds a later pass as an SRV,
    // so request both UAV and shader-resource binds.
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    const HRESULT hr = device_->CreateBuffer(&desc, nullptr, out);
    if (FAILED(hr)) {
        error = "CreateBuffer(structured/uav) failed: " +
                std::to_string(static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool GpuDevice::CreateConstantBuffer(uint32_t size, ID3D11Buffer** out,
                                     std::string& error) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (size + 15u) & ~15u;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const HRESULT hr = device_->CreateBuffer(&desc, nullptr, out);
    if (FAILED(hr)) {
        error = "CreateBuffer(constant) failed: " +
                std::to_string(static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

void GpuDevice::UpdateConstants(ID3D11Buffer* buf, const void* data,
                                uint32_t bytes) {
    context_->UpdateSubresource(buf, 0, nullptr, data, 0, 0);
}

bool GpuDevice::Readback(ID3D11Buffer* src, uint32_t bytes, void* dst,
                         std::string& error) {
    if (bytes == 0) return true;
    if (stagingCap_ < bytes) {
        staging_.Reset();
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = bytes;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = device_->CreateBuffer(&desc, nullptr, &staging_);
        if (FAILED(hr)) {
            error = "CreateBuffer(staging) failed: " +
                    std::to_string(static_cast<unsigned long>(hr));
            return false;
        }
        stagingCap_ = bytes;
    }
    context_->CopyResource(staging_.Get(), src);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr =
        context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        error = "Map(staging) failed: " +
                std::to_string(static_cast<unsigned long>(hr));
        return false;
    }
    std::memcpy(dst, mapped.pData, bytes);
    context_->Unmap(staging_.Get(), 0);
    return true;
}

} // namespace gpu
} // namespace svms

#endif // !SVMS_XP_COMPAT && _WIN32