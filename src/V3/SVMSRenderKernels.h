#ifndef SVMS_RENDER_KERNELS_H
#define SVMS_RENDER_KERNELS_H

#include "SVMSTypes.h"

namespace svms {

// Backend-neutral span contract.  Explicit SSE2 and AVX2 implementations can
// replace a class function without changing scheduling or voice ownership.
struct RenderSpanContext {
    VoiceSoA* voices;
    const float* sampleData;
    uint32_t sampleDataFrames;
    float* outputLeft;
    float* outputRight;
    uint32_t frameStart;
    uint32_t frameCount;
    uint32_t voiceCapacity;
};

using RenderClassKernel = void(*)(const RenderSpanContext& context,
                                  const uint32_t* handles,
                                  uint32_t handleCount);
struct RenderKernelSet {
    RenderClassKernel kernels[kVoiceRenderClassCount];
    RenderBackend backend;
    const char* name;
};

const RenderKernelSet& GetScalarRenderKernelSet();
const RenderKernelSet& GetSSE2RenderKernelSet();
#if !defined(SVMS_XP_COMPAT)
const RenderKernelSet& GetAVX2RenderKernelSet();
#endif
const RenderKernelSet& SelectBestRenderKernelSet();
const RenderKernelSet* SelectRenderKernelSet(RenderBackend backend);
bool IsRenderBackendSupported(RenderBackend backend);

uint32_t ScalarRenderSustainedLoop(
    VoiceSoA& voices, uint32_t handle, const float* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount);

uint32_t ScalarRenderSustainedOneShot(
    VoiceSoA& voices, uint32_t handle, const float* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount);

void ScalarRenderSustainedLoopShortBatch(
    VoiceSoA& voices, const uint32_t* handles, uint32_t handleCount,
    const float* sampleData, uint32_t sampleDataFrames, float* outputLeft,
    float* outputRight, uint32_t frameStart, uint32_t frameCount);

} // namespace svms

#endif
