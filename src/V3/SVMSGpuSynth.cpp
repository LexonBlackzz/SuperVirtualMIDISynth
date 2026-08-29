// V3 GPU synthesis stage. The host retains all scheduling and event-timing
// semantics; the GPU is a pure per-voice sample-synthesis engine.

#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)

#include "SVMSGpuSynth.h"
#include "SVMSEnvelope.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace svms {

namespace {
// Diagnostics: the GPU render path reads silence as dead-quiet output, so any
// dropped block must be loud in the log, not silent.
void GpuTraceOnce(const char* what, const std::string& error) {
    static volatile long fired = 0;
    char line[512];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[SVMS GPU] %s failed: %s\n",
                what, error.c_str());
    std::fputs(line, stderr);
    OutputDebugStringA(line);
    (void)fired;
}

// Per-voice per-frame partials are O(numVoices * chunkFrames * 16 bytes).
// D3D11 caps buffer resources at 128 MB, and anything above a few dozen MB is
// also a VRAM mis-size for a per-block scratch. 64 MB â†’ 4096 voices Ã— 1024
// frames per dispatch; larger host blocks are chunked (retirement then also
// resolves at chunk boundaries instead of at the end of a 1.5 s block).
constexpr uint32_t kPartialBufBudgetBytes = 64u * 1024u * 1024u;
constexpr uint32_t kChunkFrameCeiling = 2048u;
} // namespace

// GPU voice input (mirrors DenseVoiceSnapshot fields in shader-friendly
// layout).  All fields are 4 bytes so the C++ packed layout, the HLSL
// struct packing and the structured-buffer stride agree; the trailing pad
// keeps the stride a multiple of 16 for the structured-buffer bind.
struct GpuVoiceIn {
    float phase, phaseInc, currentGain, targetGain, sustainLevel;
    float attackGainStep, decaySlope, releaseDecay;
    float mixGainL, mixGainR, relLoopSF, relLoopEF;
    uint32_t sampleStart, relEnd, relLoopS, relLoopE;
    uint32_t delayRemaining, holdRemaining, attackRemaining, decayRemaining;
    uint32_t releaseRemaining, fadeRemaining, fadeTotal;
    uint32_t envelopeStage, loopEnabled, state, sampleBacked;
    uint32_t pad;
};
static_assert(sizeof(GpuVoiceIn) % 16 == 0,
              "GpuVoiceIn stride must be a multiple of 16");

// Per-voice state read back from the GPU after rendering.
struct GpuVoiceOut {
    float phase, currentGain;
    uint32_t delayRemaining, holdRemaining, attackRemaining, decayRemaining;
    uint32_t releaseRemaining, fadeRemaining, envelopeStage, state;
    uint32_t pad0, pad1;
};
static_assert(sizeof(GpuVoiceOut) % 16 == 0,
              "GpuVoiceOut stride must be a multiple of 16");

const char* kGpuVoiceShader = R"HLSL(
// Frame-exact transcription of the CPU fused per-voice body
// (RenderScalar::RenderBlockFrameMajor).  Same op order, same retire frame,
// no gate tiles: a retiring voice contributes exactly the frames the CPU
// would and then writes STATE_FREE for the host to retire.
#define ENV_HOLD     0
#define ENV_ATTACK   1
#define ENV_DECAY    2
#define ENV_SUSTAIN  3
#define ENV_DELAY    4
#define STATE_FREE    0
#define STATE_ACTIVE  1
#define STATE_RELEASE 2
#define RETIRE_THRESHOLD 0.00015f
#define RELEASE_INFINITE 0xFFFFFFFFu

struct VoiceIn {
    float phase, phaseInc, currentGain, targetGain, sustainLevel;
    float attackGainStep, decaySlope, releaseDecay;
    float mixGainL, mixGainR, relLoopSF, relLoopEF;
    uint sampleStart, relEnd, relLoopS, relLoopE;
    uint delayRemaining, holdRemaining, attackRemaining, decayRemaining;
    uint releaseRemaining, fadeRemaining, fadeTotal;
    uint envelopeStage, loopEnabled, state, sampleBacked;
    uint pad;
};
struct VoiceOut {
    float phase, currentGain;
    uint delayRemaining, holdRemaining, attackRemaining, decayRemaining;
    uint releaseRemaining, fadeRemaining, envelopeStage, state;
    uint pad0, pad1;
};

cbuffer Params : register(b0) { uint numVoices, numFrames, poolFrames, pad; }
StructuredBuffer<VoiceIn> gVoiceIn : register(t0);
StructuredBuffer<float> gPool : register(t1);
RWStructuredBuffer<float4> gPartial : register(u0);
RWStructuredBuffer<VoiceOut> gVoiceOut : register(u1);

[numthreads(64,1,1)]
void VoiceCS(uint3 id : SV_DispatchThreadID) {
    uint v = id.x;
    if (v >= numVoices) return;
    VoiceIn In = gVoiceIn[v];
    float phase = In.phase;
    const float phaseInc = In.phaseInc;
    float gain = In.currentGain;
    const float targetGain = In.targetGain;
    const float sustainLev = In.sustainLevel;
    const float attackStep = In.attackGainStep;
    const float decaySlp = In.decaySlope;
    const float relDecay = In.releaseDecay;
    const float mixL = In.mixGainL, mixR = In.mixGainR;
    uint stage = In.envelopeStage;
    uint state = In.state;
    uint delayRem = In.delayRemaining, holdRem = In.holdRemaining;
    uint attackRem = In.attackRemaining, decayRem = In.decayRemaining;
    uint releaseRem = In.releaseRemaining, fadeRem = In.fadeRemaining;
    const uint fadeTotal = In.fadeTotal;
    const bool sampleBacked = (In.sampleBacked != 0u);
    // CPU: loop = isSampleBacked && (loopEnabled != 0).
    const bool looping = sampleBacked && (In.loopEnabled != 0u);
    const uint base = In.sampleStart;
    const uint relEnd = In.relEnd;
    const uint relLoopS = In.relLoopS;
    const uint relLoopE = In.relLoopE;
    const float relLoopSF = In.relLoopSF, relLoopEF = In.relLoopEF;
    const bool isReleased = (state == STATE_RELEASE);

    bool dead = false;
    for (uint f = 0; f < numFrames; ++f) {
        if (dead) {
            gPartial[f * numVoices + v] = float4(0, 0, 0, 0);
            continue;
        }

        // ── Dereference (RenderBlockFrameMajor fused body) ──
        float sample = 0.0f;
        bool retireVoice = false;
        bool releaseFinished = false;

        if (sampleBacked) {
            if (phase < 0.0f) phase = 0.0f;
            uint baseOffset = (uint)phase;
            if (baseOffset + 1u >= relEnd) {
                if (!looping) {
                    // CPU: retireVoice = true; goto done.  The final sample
                    // is never emitted and the envelope/phase do not advance.
                    retireVoice = true;
                } else {
                    // CPU snaps the phase to the loop start on deref.
                    phase = relLoopSF;
                    baseOffset = relLoopS;
                    uint nextRel = baseOffset + 1u;
                    if (nextRel >= relLoopE) nextRel = relLoopS;
                    if (nextRel >= relEnd) nextRel = relEnd - 1u;
                    const float s0 = gPool[base + baseOffset];
                    const float s1 = gPool[base + nextRel];
                    const float fr = phase - (float)baseOffset;
                    sample = s0 + (s1 - s0) * fr;
                }
            } else {
                uint nextRel = baseOffset + 1u;
                if (looping && nextRel >= relLoopE) nextRel = relLoopS;
                if (nextRel >= relEnd) nextRel = relEnd - 1u;
                const float s0 = gPool[base + baseOffset];
                const float s1 = gPool[base + nextRel];
                const float fr = phase - (float)baseOffset;
                sample = s0 + (s1 - s0) * fr;
            }
        }


        // ── Envelope + phase advance (skipped on the retire frame) ──
        if (!retireVoice) {
            const bool sustain = !isReleased && (stage == ENV_SUSTAIN);
            if (isReleased) {
                if (releaseRem == 0u) {
                    releaseFinished = true;
                } else {
                    gain *= relDecay;
                    if (releaseRem != RELEASE_INFINITE) {
                        --releaseRem;
                        if (releaseRem == 0u) releaseFinished = true;
                    }
                }
            } else if (!sustain) {
                if (stage == ENV_DELAY) {
                    if (delayRem > 0u) { --delayRem; gain = 0.0f; }
                    else stage = ENV_HOLD;
                }
                if (stage == ENV_HOLD) {
                    if (holdRem > 0u) { --holdRem; gain = targetGain; }
                    else stage = ENV_ATTACK;
                }
                if (stage == ENV_ATTACK) {
                    if (attackRem > 0u) {
                        gain += attackStep;
                        --attackRem;
                        if (gain > targetGain) gain = targetGain;
                    } else gain = targetGain;
                    if (attackRem == 0u)
                        stage = (decayRem > 0u) ? ENV_DECAY : ENV_SUSTAIN;
                }
                if (stage == ENV_DECAY) {
                    if (decayRem > 0u) {
                        gain *= decaySlp;
                        --decayRem;
                        if (gain < sustainLev) gain = sustainLev;
                    } else gain = sustainLev;
                    if (decayRem == 0u) stage = ENV_SUSTAIN;
                }
            }
            phase += phaseInc;
            if (looping && phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                const float loopLength = relLoopEF - relLoopSF;
                if (loopLength > 0.0f && overflow >= loopLength)
                    overflow -= floor(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }
        }

        // ── Steal fade-in (CPU applies it even on the retire frame) ──
        float fadeIn = 1.0f;
        if (fadeRem > 0u) {
            fadeIn = fadeTotal > 0u
                ? (float)(fadeTotal - fadeRem + 1u) / (float)fadeTotal
                : 1.0f;
            --fadeRem;
        }

        // ── Mix (CPU multiply order: (sample*gain*fade)*mix) ──
        const float scaled = sample * gain * fadeIn;
        gPartial[f * numVoices + v] = float4(scaled * mixL, scaled * mixR,
                                             0.0f, 0.0f);

        // ── Retirement check (CPU: after mix, before next frame) ──
        const bool thresholdFinish = isReleased &&
            releaseRem == RELEASE_INFINITE && gain < RETIRE_THRESHOLD;
        if (retireVoice || releaseFinished || thresholdFinish) {
            state = STATE_FREE;
            dead = true;
        }
    }

    VoiceOut outv;
    outv.phase = phase;
    outv.currentGain = gain;
    outv.delayRemaining = delayRem;
    outv.holdRemaining = holdRem;
    outv.attackRemaining = attackRem;
    outv.decayRemaining = decayRem;
    outv.releaseRemaining = releaseRem;
    outv.fadeRemaining = fadeRem;
    outv.envelopeStage = stage;
    outv.state = state;
    outv.pad0 = 0u;
    outv.pad1 = 0u;
    gVoiceOut[v] = outv;
}
)HLSL";

const char* kGpuReduceShader = R"HLSL(
cbuffer Params : register(b0) { uint numVoices, numFrames, poolFrames, pad; }

StructuredBuffer<float4> gPartial : register(t0);
RWStructuredBuffer<float2> gOut : register(u0);

[numthreads(64,1,1)]
void ReduceCS(uint3 id : SV_DispatchThreadID) {
    uint f = id.x;
    if (f >= numFrames) return;
    float4 acc = (float4)0;
    for (uint v = 0; v < numVoices; ++v) acc += gPartial[f * numVoices + v];
    gOut[f] = float2(acc.x, acc.y);
}
)HLSL";

// â”€â”€ GpuSynth implementation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

bool GpuSynth::Initialize(const float* sampleData, uint32_t sampleDataFrames,
                          uint32_t maxVoices, uint32_t blockFrames,
                          std::string& error) {
    Destroy();
    maxVoices_ = maxVoices;
    blockFrames_ = blockFrames;
    sampleData_.assign(sampleData, sampleData + sampleDataFrames);

    if (!device_.Create(error)) return false;
    if (!BuildShaders(error)) return false;

    const uint32_t poolBytes = sampleDataFrames * sizeof(float);
    if (!device_.CreateStructBuffer(sampleData_.data(), poolBytes,
                                    sizeof(float), &poolBuf_, error))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    srv.BufferEx.FirstElement = 0;
    srv.BufferEx.NumElements = sampleDataFrames;
    if (FAILED(device_.Device()->CreateShaderResourceView(
            poolBuf_.Get(), &srv, &poolSRV_))) {
        error = "CreateSRV(pool) failed";
        return false;
    }
    if (!device_.CreateConstantBuffer(16, &constBuf_, error)) return false;
    // Event query used to serialize the reduce dispatch after the voice
    // dispatch. D3D11 does not implicitly insert a UAV->SRV barrier between
    // separate compute dispatches on the same context.
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    if (FAILED(device_.Device()->CreateQuery(&qd, &fenceQuery_))) {
        error = "CreateQuery(fence) failed";
        return false;
    }
    initialized_ = true;
    return true;
}

void GpuSynth::Destroy() {
    voiceOutUAV_.Reset(); outUAV_.Reset();
    partialUAV_.Reset(); partialSRV_.Reset();
    voiceInSRV_.Reset(); poolSRV_.Reset();
    outBuf_.Reset(); voiceOutBuf_.Reset();
    partialBuf_.Reset(); voiceInBuf_.Reset(); poolBuf_.Reset();
    constBuf_.Reset(); voiceCS_.Reset(); reduceCS_.Reset();
    fenceQuery_.Reset();
    device_.Destroy();
    initialized_ = false;
}

bool GpuSynth::BuildShaders(std::string& error) {
    if (!device_.CompileCompute(kGpuVoiceShader, "VoiceCS", "cs_5_0",
                                &voiceCS_, error))
        return false;
    if (!device_.CompileCompute(kGpuReduceShader, "ReduceCS", "cs_5_0",
                                &reduceCS_, error))
        return false;
    return true;
}

bool GpuSynth::EnsureBuffers(uint32_t activeCount, uint32_t blockFrames,
                             std::string& error) {
    ID3D11Device* dev = device_.Device();
    const uint32_t viBytes = activeCount * sizeof(GpuVoiceIn);
    const uint32_t voBytes = activeCount * sizeof(GpuVoiceOut);
    const uint32_t pBytes = blockFrames * activeCount * 4u * 4u;
    const uint32_t oBytes = blockFrames * 2u * 4u;

    bool buffersRegrown = false;
    if (voiceInBufCap_ != viBytes || voiceOutBufCap_ != voBytes ||
        partialBufCap_ != pBytes || outBufCap_ != oBytes) {
        // Views reference the old buffers; release them before regrow.
        voiceInSRV_.Reset(); voiceOutUAV_.Reset();
        partialUAV_.Reset(); partialSRV_.Reset(); outUAV_.Reset();
        voiceInBuf_.Reset(); voiceOutBuf_.Reset();
        partialBuf_.Reset(); outBuf_.Reset();

        if (!device_.CreateStructBuffer(nullptr, viBytes,
                                        sizeof(GpuVoiceIn), &voiceInBuf_,
                                        error)) return false;
        if (!device_.CreateRWStructBuffer(voBytes, sizeof(GpuVoiceOut),
                                          &voiceOutBuf_, error)) return false;
        if (!device_.CreateRWStructBuffer(pBytes, 16u, &partialBuf_, error))
            return false;
        if (!device_.CreateRWStructBuffer(oBytes, 8u, &outBuf_, error))
            return false;

        voiceInBufCap_ = viBytes;
        voiceOutBufCap_ = voBytes;
        partialBufCap_ = pBytes;
        outBufCap_ = oBytes;
        buffersRegrown = true;
    }

    // Views must be exact-sized for this dispatch: reusing a view keyed on
    // an earlier chunk's (activeCount, frames) silently zero-reads past the
    // old NumElements.
    if (!buffersRegrown && viewVoicesKey_ == activeCount &&
        viewFramesKey_ == blockFrames)
        return true;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    srv.BufferEx.FirstElement = 0;
    srv.BufferEx.NumElements = activeCount;
    if (FAILED(dev->CreateShaderResourceView(voiceInBuf_.Get(), &srv,
                                             &voiceInSRV_))) {
        error = "CreateSRV(voiceIn) failed"; return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = activeCount;
    if (FAILED(dev->CreateUnorderedAccessView(voiceOutBuf_.Get(), &uav,
                                              &voiceOutUAV_))) {
        error = "CreateUAV(voiceOut) failed"; return false;
    }
    uav.Buffer.NumElements = blockFrames * activeCount;
    if (FAILED(dev->CreateUnorderedAccessView(partialBuf_.Get(), &uav,
                                              &partialUAV_))) {
        error = "CreateUAV(partial) failed"; return false;
    }
    srv.BufferEx.NumElements = blockFrames * activeCount;
    if (FAILED(dev->CreateShaderResourceView(partialBuf_.Get(), &srv,
                                             &partialSRV_))) {
        error = "CreateSRV(partial) failed"; return false;
    }
    uav.Buffer.NumElements = blockFrames;
    if (FAILED(dev->CreateUnorderedAccessView(outBuf_.Get(), &uav, &outUAV_))) {
        error = "CreateUAV(out) failed"; return false;
    }
    viewVoicesKey_ = activeCount;
    viewFramesKey_ = blockFrames;
    return true;
}

void GpuSynth::RenderBlock(VoiceManager& voices, float* outL, float* outR,
                           uint32_t numFrames, uint64_t currentFrame) {
    if (!initialized_) return;
    VoiceSoA& v = voices.v;
    if (voices.activeCount_ == 0 || numFrames == 0) return;

    std::string error;
    std::vector<GpuVoiceIn> packed;
    std::vector<uint32_t> handles;
    std::vector<float> chunkOut;
    std::vector<GpuVoiceOut> voiceOut;

    ID3D11DeviceContext* ctx = device_.Context();
    ID3D11UnorderedAccessView* nullUAV[] = {nullptr, nullptr};
    ID3D11ShaderResourceView* nullSRV[] = {nullptr, nullptr};

    // The per-voice partial buffer is O(activeCount * frames * 16 bytes).
    // Chunk frames so it stays under the budget; before this existed, one
    // 8192-frame block at full pool demand exceeded the 128 MB D3D11 buffer
    // cap, EnsureBuffers failed, and every block came out dead silent.
    uint32_t cursor = 0;
    while (cursor < numFrames && voices.activeCount_ > 0) {
        const uint32_t activeCount = voices.activeCount_;
        const uint32_t framesLeft = numFrames - cursor;
        const uint64_t perFrameBytes = uint64_t(activeCount) * 16u;
        uint64_t chunkFrames = perFrameBytes
            ? kPartialBufBudgetBytes / perFrameBytes : framesLeft;
        chunkFrames = (std::min)(chunkFrames, uint64_t(kChunkFrameCeiling));
        chunkFrames = (std::max)(chunkFrames, uint64_t(1u));
        const uint32_t n =
            uint32_t((std::min)(chunkFrames, uint64_t(framesLeft)));

        if (!EnsureBuffers(activeCount, n, error)) {
            GpuTraceOnce("EnsureBuffers", error);
            return;
        }

        // Pack the active list.  Skip stale Free entries the way the CPU span
        // loop does; the handle order doubles as the GPU readback order.
        handles.resize(activeCount);
        packed.resize(activeCount);
        uint32_t upCount = 0;
        for (uint32_t i = 0; i < activeCount; ++i) {
            const uint32_t h = voices.activeList_[i];
            if (v.state[h] == static_cast<uint8_t>(VoiceState::Free))
                continue;
            handles[upCount] = h;
            GpuVoiceIn& p = packed[upCount++];
            p.phase = v.phases[h];
            p.phaseInc = v.phaseIncs[h];
            p.currentGain = v.currentGain[h];
            p.targetGain = v.targetGain[h];
            p.sustainLevel = v.sustainLevel[h];
            p.attackGainStep = v.attackGainStep[h];
            p.decaySlope = v.decaySlope[h];
            p.releaseDecay = v.releaseDecay[h];
            p.mixGainL = v.mixGainL[h];
            p.mixGainR = v.mixGainR[h];
            p.relLoopSF = v.relLoopSF[h];
            p.relLoopEF = v.relLoopEF[h];
            p.sampleStart = v.sampleStart[h];
            p.relEnd = v.relEnd[h];
            p.relLoopS = v.relLoopS[h];
            p.relLoopE = v.relLoopE[h];
            p.delayRemaining = v.delaySamplesRemaining[h];
            p.holdRemaining = v.holdSamplesRemaining[h];
            p.attackRemaining = v.attackSamplesRemaining[h];
            p.decayRemaining = v.decaySamplesRemaining[h];
            p.releaseRemaining = v.releaseSamplesRemaining[h];
            p.fadeRemaining = v.stealFadeInFramesRemaining[h];
            p.fadeTotal = v.stealFadeInFramesTotal[h];
            p.envelopeStage = static_cast<uint32_t>(v.envelopeStage[h]);
            p.loopEnabled = v.loopEnabled[h] ? 1u : 0u;
            p.state = static_cast<uint32_t>(v.state[h]);
            p.sampleBacked = static_cast<uint32_t>(v.sampleBacked[h]);
            p.pad = 0u;
        }
        if (upCount == 0) break;
        ctx->UpdateSubresource(voiceInBuf_.Get(), 0, nullptr, packed.data(),
                               0, 0);

        uint32_t params[4] = {upCount, n,
                              static_cast<uint32_t>(sampleData_.size()), 0};
        device_.UpdateConstants(constBuf_.Get(), params, sizeof(params));

        // Pass 1: per-voice synthesis
        ID3D11ShaderResourceView* srvVoice[] = {voiceInSRV_.Get(),
                                                poolSRV_.Get()};
        ID3D11UnorderedAccessView* uavVoice[] = {partialUAV_.Get(),
                                                 voiceOutUAV_.Get()};
        ctx->CSSetConstantBuffers(0, 1, constBuf_.GetAddressOf());
        ctx->CSSetShaderResources(0, 2, srvVoice);
        ctx->CSSetUnorderedAccessViews(0, 2, uavVoice, nullptr);
        ctx->CSSetShader(voiceCS_.Get(), nullptr, 0);
        ctx->Dispatch((upCount + 63u) / 64u, 1, 1);

        // D3D11 does not implicitly insert a UAV->SRV barrier between
        // separate compute dispatches, so the reduce pass' SRV read of
        // gPartial would observe zeros. Force the voice dispatch to complete
        // on the GPU before the reduce pass is submitted.
        ctx->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
        ctx->CSSetShaderResources(0, 2, nullSRV);
        ctx->CSSetShader(nullptr, nullptr, 0);
        if (fenceQuery_) {
            ctx->End(fenceQuery_.Get());
            BOOL done = FALSE;
            while (SUCCEEDED(ctx->GetData(fenceQuery_.Get(), &done,
                                          sizeof(done), 0)) &&
                   !done) {
                // Busy-wait until the GPU drains the voice dispatch.
            }
        }

        // Pass 2: reduce
        ID3D11ShaderResourceView* srvPartial[] = {partialSRV_.Get()};
        ID3D11UnorderedAccessView* uavOut[] = {outUAV_.Get()};
        ctx->CSSetShaderResources(0, 1, srvPartial);
        ctx->CSSetUnorderedAccessViews(0, 1, uavOut, nullptr);
        ctx->CSSetShader(reduceCS_.Get(), nullptr, 0);
        ctx->Dispatch((n + 63u) / 64u, 1, 1);
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        ctx->CSSetShaderResources(0, 1, nullSRV);
        ctx->CSSetShader(nullptr, nullptr, 0);
        device_.DumpErrors("after-reduce");

        // Chunk audio readback.  Additive: the host mixes CPU steal tails
        // into the buffers first, matching the CPU span's accumulation order.
        chunkOut.assign(n * 2u, 0.0f);
        if (!device_.Readback(outBuf_.Get(), n * 2u * 4u, chunkOut.data(),
                              error)) {
            GpuTraceOnce("Readback(audio)", error);
            return;
        }
        device_.DumpErrors("after-audio-readback");
        for (uint32_t f = 0; f < n; ++f) {
            outL[cursor + f] += chunkOut[f * 2u];
            outR[cursor + f] += chunkOut[f * 2u + 1u];
        }

        // Voice state readback.  Iterate the pre-dispatch handle snapshot:
        // RetireVoice swap-removes from activeList_, so applying survivors
        // and retiring in one walk would corrupt later entries.
        voiceOut.resize(upCount);
        if (!device_.Readback(voiceOutBuf_.Get(),
                              upCount * sizeof(GpuVoiceOut),
                              voiceOut.data(), error)) {
            GpuTraceOnce("Readback(voice)", error);
            return;
        }
        voices.SetCurrentFrame(currentFrame + cursor + n);
        for (uint32_t i = 0; i < upCount; ++i) {
            const GpuVoiceOut& o = voiceOut[i];
            if (o.state == 0u) continue; // retired; skipped in apply pass
            const uint32_t h = handles[i];
            const uint8_t oldStage = v.envelopeStage[h];
            v.phases[h] = o.phase;
            v.currentGain[h] = o.currentGain;
            v.delaySamplesRemaining[h] = o.delayRemaining;
            v.holdSamplesRemaining[h] = o.holdRemaining;
            v.attackSamplesRemaining[h] = o.attackRemaining;
            v.decaySamplesRemaining[h] = o.decayRemaining;
            v.releaseSamplesRemaining[h] = o.releaseRemaining;
            v.stealFadeInFramesRemaining[h] = o.fadeRemaining;
            v.envelopeStage[h] = static_cast<uint8_t>(o.envelopeStage);
            if (o.envelopeStage != oldStage)
                voices.RefreshRenderClass(static_cast<VoiceHandle>(h));
        }
        // RetireVoice reads state != Free before clearing the slot, so keep
        // the host-side state untouched for finished voices and let it do the
        // bookkeeping (channel/key unlink, free-stack push, swap-remove).
        for (uint32_t i = 0; i < upCount; ++i) {
            if (voiceOut[i].state != 0u) continue;
            voices.RetireVoice(static_cast<VoiceHandle>(handles[i]));
        }

        cursor += n;
    }
}

} // namespace svms

#endif // !SVMS_XP_COMPAT && _WIN32
