#ifndef SVMS_RENDER_WORKERS_H
#define SVMS_RENDER_WORKERS_H

#include "SVMSRenderKernels.h"

#include <cstddef>
#include <cstdint>

namespace svms {

// Per-job mix destinations for indexed render jobs.  Legacy jobs write the
// single stereo mix; bus-mode jobs (per-MIDI-channel limiter active) write
// per-channel planes instead — the job's private planes from the pool, or
// the block buses when the job runs serially on the calling thread.
struct IndexedJobMix {
    float* outputLeft;
    float* outputRight;
    float* const* busLeft;    // kChannelCount plane pointers; null = legacy
    float* const* busRight;
};

using IndexedRenderJob = void(*)(uint32_t jobIndex, const IndexedJobMix& mix,
                                 uint32_t frameCount, void* userData);

enum class RenderParallelRejectReason : uint8_t {
    None,
    Unavailable,
    TooFewFrames,
    TooFewVoices,
    TooFewVoiceSamples
};

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
    // Re-applies the configured thread-affinity mode to the worker threads
    // (no-op on XP/POSIX or when the pool is not running).
    void ApplyAffinity() noexcept;
    static size_t EstimateAllocatedBytes(uint32_t totalRenderThreads,
                                         uint32_t maxFrames,
                                         uint32_t voiceCapacity) noexcept;
    bool ShouldParallelize(uint32_t voiceCount, uint32_t frameCount) const noexcept;
    RenderParallelRejectReason ClassifyParallelization(
        uint32_t voiceCount, uint32_t frameCount) const noexcept;

    void BeginSpan(const RenderSpanContext& context) noexcept;
    bool AddClassRange(RenderClassKernel kernel, const uint32_t* handles,
                       uint32_t handleCount) noexcept;
    // Returns false without invoking a kernel when the queued work is too
    // small or could not be represented. The caller can then render serially.
    bool Execute() noexcept;

    // Execute fixed logical jobs with dynamic worker claiming. Each job gets
    // a deterministic private mix buffer; reduction is always job-index order.
    // When channelBusLeft/Right are non-null, jobs additionally receive
    // private per-channel bus planes (per-MIDI-channel limiter support) and
    // the merge sums planes per channel.
    bool ExecuteIndexed(uint32_t jobCount, uint32_t frameCount,
                        float* outputLeft, float* outputRight,
                        IndexedRenderJob callback, void* userData,
                        float* const* channelBusLeft = nullptr,
                        float* const* channelBusRight = nullptr) noexcept;
    bool BeginIndexed(uint32_t jobCount, uint32_t frameCount,
                      float* outputLeft, float* outputRight,
                      IndexedRenderJob callback, void* userData,
                      float* const* channelBusLeft = nullptr,
                      float* const* channelBusRight = nullptr) noexcept;
    bool FinishIndexed() noexcept;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace svms

#endif
