#ifndef SVMS_RENDER_KERNELS_H
#define SVMS_RENDER_KERNELS_H

#include "SVMSTypes.h"

namespace svms {

struct SpanRetirement {
    uint32_t handle;
    uint32_t frameOffset;
    uint32_t capturePosition;
};

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
    uint32_t* classChangeHandles;
    uint32_t* classChangeCount;
    const uint32_t* activePositions;
    SpanRetirement* retirements;
    uint32_t* retirementCount;
};

// A backend returns false without mutating state when it cannot safely consume
// the complete class, allowing the established scalar voice path to take over.
using RenderClassKernel = bool(*)(const RenderSpanContext& context,
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

#if !defined(SVMS_XP_COMPAT)
// Builds the exact packed keys used by the volatile steal heap. Returns false
// when the absolute frame is outside the vector kernel's exact 32-bit age
// range, allowing the caller to retain the scalar path.
bool BuildVolatileStealKeysAVX2(
    const uint32_t* handles, uint32_t handleCount,
    const uint64_t* birthFrames, const float* currentGains,
    const float* outputGains, const uint32_t* activePositions,
    uint64_t currentFrame, float gainScale, uint64_t* outputKeys,
    uint32_t* outputHandles, uint32_t* inverseHeapPositions);
#endif

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

bool ScalarRenderTransientLoopClass(const RenderSpanContext& context,
                                    const uint32_t* handles,
                                    uint32_t handleCount);

} // namespace svms

#endif
