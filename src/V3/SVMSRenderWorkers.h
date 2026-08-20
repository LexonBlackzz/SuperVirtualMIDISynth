#ifndef SVMS_RENDER_WORKERS_H
#define SVMS_RENDER_WORKERS_H

#include "SVMSRenderKernels.h"

#include <cstddef>
#include <cstdint>

namespace svms {

using IndexedRenderJob = void(*)(uint32_t jobIndex, float* outputLeft,
                                 float* outputRight, uint32_t frameCount,
                                 void* userData);

// Persistent, allocation-free-at-render-time voice mixing workers. MIDI
// dispatch and every lifecycle mutation remain on the audio thread; workers
// only render disjoint handle ranges into private buffers.
class RenderWorkerPool {
public:
    RenderWorkerPool() noexcept;
    ~RenderWorkerPool();
    RenderWorkerPool(const RenderWorkerPool&) = delete;
    RenderWorkerPool& operator=(const RenderWorkerPool&) = delete;

    bool Initialize(uint32_t totalRenderThreads, uint32_t maxFrames,
                    uint32_t voiceCapacity);
    void Shutdown() noexcept;

    uint32_t GetThreadCount() const noexcept;
    float GetHelperJobPercent() const noexcept;
    size_t GetAllocatedBytes() const noexcept;
    bool ShouldParallelize(uint32_t voiceCount, uint32_t frameCount) const noexcept;

    void BeginSpan(const RenderSpanContext& context) noexcept;
    bool AddClassRange(RenderClassKernel kernel, const uint32_t* handles,
                       uint32_t handleCount) noexcept;
    // Returns false without invoking a kernel when the queued work is too
    // small or could not be represented. The caller can then render serially.
    bool Execute() noexcept;

    // Execute fixed logical jobs with dynamic worker claiming. Each job gets
    // a deterministic private mix buffer; reduction is always job-index order.
    bool ExecuteIndexed(uint32_t jobCount, uint32_t frameCount,
                        float* outputLeft, float* outputRight,
                        IndexedRenderJob callback, void* userData) noexcept;
    bool BeginIndexed(uint32_t jobCount, uint32_t frameCount,
                      float* outputLeft, float* outputRight,
                      IndexedRenderJob callback, void* userData) noexcept;
    bool FinishIndexed() noexcept;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace svms

#endif
