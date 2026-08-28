// V3 GPU synthesis proof spike.
//
// Renders a small synthetic voice set entirely on the GPU (D3D11 compute),
// writes a WAV, runs the same voice set through a CPU scalar reference, and
// prints the peak difference in dB against the agreed -85 dB gate.
//
// This is deliberately decoupled from RenderBlock: it de-risks the shader DSP
// and device plumbing in isolation before the offline/integration milestone.
// The GPU is a pure synthesis stage — the host supplies a fixed voice plan and
// a sample pool; there is no scheduler or MIDI semantics here.

#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)

#include "SVMSGpuDevice.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kNumVoices = 64;
constexpr uint32_t kFrames = 2048;
constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kPoolFrames = 512; // synthetic looped waveform

// HLSL and the CPU reference must agree on this layout and on the phase math.
struct SpikeVoice {
    float phase{};
    float step{};
    float gainL{};
    float gainR{};
    float envLevel{};
    uint32_t sampleBase{};
    uint32_t loopStart{};
    uint32_t loopEnd{};
    uint32_t loopEnabled{};
    uint32_t len{};
};

const char* kShader = R"HLSL(
struct SpikeVoice {
    float phase;
    float step;
    float gainL;
    float gainR;
    float envLevel;
    uint  sampleBase;
    uint  loopStart;
    uint  loopEnd;
    uint  loopEnabled;
    uint  len;
};
cbuffer Params : register(b0) {
    uint numVoices;
    uint numFrames;
    uint numPool;
    uint pad;
};
StructuredBuffer<SpikeVoice> gVoices : register(t0);
StructuredBuffer<float> gPool : register(t1);
RWStructuredBuffer<float4> gPartial : register(u0);

[numthreads(64,1,1)]
void VoiceCS(uint3 id : SV_DispatchThreadID) {
    uint v = id.x;
    if (v >= numVoices) return;
    SpikeVoice s = gVoices[v];
    float phase = s.phase;
    uint base = s.sampleBase;
    uint len = s.len;
    uint ls = s.loopStart;
    uint le = s.loopEnd;
    bool looping = (s.loopEnabled != 0);
    for (uint f = 0; f < numFrames; ++f) {
        uint i = (uint)phase;
        float frac = phase - (float)i;
        uint i0 = (i >= len) ? (len - 1) : i;
        uint i1 = (i + 1 >= len) ? (len - 1) : (i + 1);
        float s0 = gPool[base + i0];
        float s1 = gPool[base + i1];
        float smpl = s0 + (s1 - s0) * frac;
        float env = s.envLevel;
        float L = smpl * env * s.gainL;
        float R = smpl * env * s.gainR;
        gPartial[f * numVoices + v] = float4(L, R, 0.0, 0.0);
        phase += s.step;
        if (looping) {
            if (phase >= (float)le) {
                float span = (float)(le - ls);
                phase = phase - span;
                if (phase >= (float)le) phase = (float)ls + fmod(phase - (float)ls, span);
            }
        } else if (phase >= (float)len) {
            phase = (float)(len - 1);
        }
    }
}
)HLSL";

// Reduce shader reads the per-voice partials as an SRV and deterministically
// sums them per frame (voice order 0..N-1) into the final stereo output.
const char* kReduceShader = R"HLSL(
cbuffer Params : register(b0) {
    uint numVoices;
    uint numFrames;
    uint numPool;
    uint pad;
};
StructuredBuffer<float4> gPartial : register(t0);
RWStructuredBuffer<float2> gOut : register(u0);

[numthreads(64,1,1)]
void ReduceCS(uint3 id : SV_DispatchThreadID) {
    uint f = id.x;
    if (f >= numFrames) return;
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (uint v = 0; v < numVoices; ++v) acc += gPartial[f * numVoices + v];
    gOut[f] = float2(acc.x, acc.y);
}
)HLSL";

bool WriteFloatWave(const char* path, const float* interleaved, uint32_t frames) {
    const uint32_t bytes = frames * 2u * 4u;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    auto W32 = [&](uint32_t v) { fwrite(&v, 1, 4, f); };
    auto W16 = [&](uint16_t v) { fwrite(&v, 1, 2, f); };
    fwrite("RIFF", 1, 4, f);
    W32(36 + bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    W32(16);
    W16(3);                       // IEEE float
    W16(2);                       // stereo
    W32(kSampleRate);
    W32(kSampleRate * 2u * 4u);   // avg bytes/sec
    W16(8);                       // block align
    W16(32);                      // bits per sample
    fwrite("data", 1, 4, f);
    W32(bytes);
    fwrite(interleaved, 1, bytes, f);
    fclose(f);
    return true;
}

void RunCpuReference(const std::vector<SpikeVoice>& voices,
                     const std::vector<float>& pool, std::vector<float>& outL,
                     std::vector<float>& outR) {
    outL.assign(kFrames, 0.0f);
    outR.assign(kFrames, 0.0f);
    for (const auto& s : voices) {
        float phase = s.phase;
        const uint32_t base = s.sampleBase;
        const uint32_t slen = s.len;
        const uint32_t ls = s.loopStart;
        const uint32_t le = s.loopEnd;
        const bool looping = s.loopEnabled != 0;
        for (uint32_t f = 0; f < kFrames; ++f) {
            const uint32_t i = static_cast<uint32_t>(phase);
            const float frac = phase - static_cast<float>(i);
            const uint32_t i0 = (i >= slen) ? slen - 1 : i;
            uint32_t i1 = i + 1;
            if (i1 >= slen) i1 = slen - 1;
            const float s0 = pool[base + i0];
            const float s1 = pool[base + i1];
            const float smpl = s0 + (s1 - s0) * frac;
            const float env = s.envLevel;
            outL[f] += smpl * env * s.gainL;
            outR[f] += smpl * env * s.gainR;
            phase += s.step;
            if (looping) {
                if (phase >= static_cast<float>(le)) {
                    const float span = static_cast<float>(le - ls);
                    phase = phase - span;
                    if (phase >= static_cast<float>(le))
                        phase = static_cast<float>(ls) +
                                fmodf(phase - static_cast<float>(ls), span);
                }
            } else if (phase >= static_cast<float>(slen)) {
                phase = static_cast<float>(slen - 1);
            }
        }
    }
}

void FillPool(std::vector<float>& pool) {
    pool.resize(kPoolFrames);
    for (uint32_t i = 0; i < kPoolFrames; ++i) {
        // A detuned pair of sine waves so the loop is aperiodic over its
        // length and interpolation is exercised meaningfully.
        const float t = static_cast<float>(i);
        pool[i] = 0.5f * sinf(2.0f * 3.14159265f * t * (440.0f / kSampleRate)) +
                  0.5f * sinf(2.0f * 3.14159265f * t * (554.0f / kSampleRate));
    }
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    std::vector<SpikeVoice> voices(kNumVoices);
    std::vector<float> pool;
    FillPool(pool);

    for (uint32_t v = 0; v < kNumVoices; ++v) {
        SpikeVoice& s = voices[v];
        s.phase = static_cast<float>(v % 7);            // spread starts
        s.step = 440.0f * (1.0f + 0.05f * (v % 9)) / kSampleRate; // pitches
        const float pan = static_cast<float>(v & 1u);   // alternate channels
        s.gainL = (pan ? 0.2f : 1.0f) * 0.6f;
        s.gainR = (pan ? 1.0f : 0.2f) * 0.6f;
        s.envLevel = 0.5f + 0.5f * (static_cast<float>(v % 5) / 4.0f);
        s.sampleBase = 0;
        s.loopStart = 0;
        s.loopEnd = kPoolFrames;
        s.loopEnabled = 1;
        s.len = kPoolFrames;
    }

    std::vector<float> refL, refR;
    RunCpuReference(voices, pool, refL, refR);

    std::string error;
    svms::gpu::GpuDevice device;
    if (!device.Create(error)) {
        std::fprintf(stderr, "gpu spike: device init failed: %s\n",
                     error.c_str());
        return 1;
    }
    std::printf("adapter: %s\n", device.AdapterName());

    // Compile the two compute shaders.
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> voiceCS, reduceCS;
    if (!device.CompileCompute(kShader, "VoiceCS", "cs_5_0", &voiceCS, error) ||
        !device.CompileCompute(kReduceShader, "ReduceCS", "cs_5_0", &reduceCS,
                               error)) {
        std::fprintf(stderr, "gpu spike: shader compile failed:\n%s\n",
                     error.c_str());
        return 1;
    }

    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* ctx = device.Context();

    auto fail = [&](const char* what, HRESULT hr) {
        std::fprintf(stderr, "gpu spike: %s failed: 0x%lX\n", what,
                     static_cast<unsigned long>(hr));
        std::exit(1);
    };

    // Buffers.
    Microsoft::WRL::ComPtr<ID3D11Buffer> voiceBuf, poolBuf, constBuf,
        partialBuf, outBuf;
    const uint32_t voiceBytes = kNumVoices * sizeof(SpikeVoice);
    const uint32_t poolBytes =
        static_cast<uint32_t>(pool.size() * sizeof(float));
    if (!device.CreateStructBuffer(voices.data(), voiceBytes,
                                   sizeof(SpikeVoice), &voiceBuf, error) ||
        !device.CreateStructBuffer(pool.data(), poolBytes, sizeof(float),
                                   &poolBuf, error) ||
        !device.CreateRWStructBuffer(kFrames * kNumVoices * 4u * sizeof(float),
                                     16u, &partialBuf, error) ||
        !device.CreateRWStructBuffer(kFrames * 2u * sizeof(float), 8u,
                                     &outBuf, error) ||
        !device.CreateConstantBuffer(16, &constBuf, error)) {
        std::fprintf(stderr, "gpu spike: buffer create failed: %s\n",
                     error.c_str());
        return 1;
    }

    struct Params {
        uint32_t numVoices, numFrames, numPool, pad;
    };
    Params params{kNumVoices, kFrames, kPoolFrames, 0};

    // Views.
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    srv.BufferEx.FirstElement = 0;
    srv.BufferEx.NumElements = kNumVoices;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> voiceSRV;
    if (FAILED(dev->CreateShaderResourceView(voiceBuf.Get(), &srv, &voiceSRV)))
        fail("CreateShaderResourceView(voices)", E_FAIL);
    srv.BufferEx.NumElements = kPoolFrames;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> poolSRV;
    if (FAILED(dev->CreateShaderResourceView(poolBuf.Get(), &srv, &poolSRV)))
        fail("CreateShaderResourceView(pool)", E_FAIL);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;

    uav.Buffer.NumElements = kFrames * kNumVoices;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> partialUAV;
    if (FAILED(dev->CreateUnorderedAccessView(partialBuf.Get(), &uav,
                                              &partialUAV)))
        fail("CreateUnorderedAccessView(partial)", E_FAIL);
    uav.Buffer.NumElements = kFrames;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outUAV;
    if (FAILED(dev->CreateUnorderedAccessView(outBuf.Get(), &uav, &outUAV)))
        fail("CreateUnorderedAccessView(out)", E_FAIL);

    // The reduce pass reads the partials as an SRV, so the partial buffer
    // needs its own SRV (a UAV cannot be rebound as an SRV).
    srv.BufferEx.NumElements = kFrames * kNumVoices;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> partialSRV;
    if (FAILED(dev->CreateShaderResourceView(partialBuf.Get(), &srv,
                                             &partialSRV)))
        fail("CreateShaderResourceView(partial)", E_FAIL);

    device.UpdateConstants(constBuf.Get(), &params, sizeof(params));

    // ---- Pass 1: one thread per voice writes per-voice per-frame partials ----
    ID3D11ShaderResourceView* srvVoice[] = {voiceSRV.Get(), poolSRV.Get()};
    ID3D11UnorderedAccessView* uavPartial[] = {partialUAV.Get()};
    ctx->CSSetConstantBuffers(0, 1, constBuf.GetAddressOf());
    ctx->CSSetShaderResources(0, 2, srvVoice);
    ctx->CSSetUnorderedAccessViews(0, 1, uavPartial, nullptr);
    ctx->CSSetShader(voiceCS.Get(), nullptr, 0);
    ctx->Dispatch((kNumVoices + 63u) / 64u, 1, 1);

    // Fully release the voice pass's bindings so the partial buffer can be
    // rebound as an SRV for the reduce pass. This is the UAV->SRV handoff and
    // must happen before the reduce dispatch reads the partials.
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ctx->CSSetShaderResources(0, 2, nullSRVs);
    ctx->CSSetShader(nullptr, nullptr, 0);

    // ---- Pass 2: deterministic per-frame reduce over the voices ----
    ID3D11ShaderResourceView* srvPartial[] = {partialSRV.Get(), nullptr};
    ID3D11UnorderedAccessView* uavOut[] = {outUAV.Get()};
    ctx->CSSetShaderResources(0, 2, srvPartial);
    ctx->CSSetUnorderedAccessViews(0, 1, uavOut, nullptr);
    ctx->CSSetShader(reduceCS.Get(), nullptr, 0);
    ctx->Dispatch((kFrames + 63u) / 64u, 1, 1);

    ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ctx->CSSetShaderResources(0, 2, nullSRVs);
    ctx->CSSetShader(nullptr, nullptr, 0);

    std::vector<float> outFrames(kFrames * 2u);
    if (!device.Readback(outBuf.Get(), sizeof(float) * kFrames * 2u,
                         outFrames.data(), error)) {
        std::fprintf(stderr, "gpu spike: readback failed: %s\n",
                     error.c_str());
        return 1;
    }

    // Compare GPU vs CPU reference.
    float maxDiff = 0.0f, maxRef = 0.0f;
    for (uint32_t f = 0; f < kFrames; ++f) {
        const float gL = outFrames[f * 2u];
        const float gR = outFrames[f * 2u + 1u];
        maxDiff = (std::max)(maxDiff, std::fabsf(gL - refL[f]));
        maxDiff = (std::max)(maxDiff, std::fabsf(gR - refR[f]));
        maxRef = (std::max)(maxRef, std::fabsf(refL[f]));
        maxRef = (std::max)(maxRef, std::fabsf(refR[f]));
    }
    const float db = maxRef > 0.0f
                         ? 20.0f * log10f(maxDiff / (maxRef + 1e-12f))
                         : -300.0f;

    for (uint32_t q = 0; q < 4 && q < kFrames; ++q)
        std::printf("f%u gpu=(%.6f,%.6f) cpu=(%.6f,%.6f)\n", q,
                    outFrames[q * 2u], outFrames[q * 2u + 1u], refL[q],
                    refR[q]);
    std::printf("gpu spike: maxDiff=%.6f maxRef=%.6f\n", maxDiff, maxRef);

    // Write interleaved WAVs for both the GPU and CPU results.
    std::vector<float> gpuIo(kFrames * 2u), cpuIo(kFrames * 2u);
    for (uint32_t f = 0; f < kFrames; ++f) {
        gpuIo[f * 2u] = outFrames[f * 2u];
        gpuIo[f * 2u + 1u] = outFrames[f * 2u + 1u];
        cpuIo[f * 2u] = refL[f];
        cpuIo[f * 2u + 1u] = refR[f];
    }
    WriteFloatWave("gpu_spike_gpu.wav", gpuIo.data(), kFrames);
    WriteFloatWave("gpu_spike_cpu.wav", cpuIo.data(), kFrames);

    std::printf("gpu spike: voices=%u frames=%u peakDiff=%.3f dB "
                "(gate: -85 dB ok=%s)\n",
                kNumVoices, kFrames, db, db <= -85.0f ? "yes" : "NO");
    std::printf("gpu spike: wrote gpu_spike_gpu.wav, gpu_spike_cpu.wav\n");

    device.Destroy();
    return db <= -85.0f ? 0 : 2;
}

#else
int main() { return 0; }
#endif // !SVMS_XP_COMPAT && _WIN32