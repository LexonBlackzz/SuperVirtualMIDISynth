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
};

using RenderClassKernel = void(*)(const RenderSpanContext& context,
                                  const uint32_t* handles,
                                  uint32_t handleCount);

struct RenderKernelSet {
    RenderClassKernel kernels[kVoiceRenderClassCount];
};

const RenderKernelSet& GetScalarRenderKernelSet();

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
