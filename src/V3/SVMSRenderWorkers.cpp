#include "SVMSRenderWorkers.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <new>
#include <xmmintrin.h>

namespace svms {
namespace {

constexpr uint32_t kHandlesPerJob = 256u;
constexpr uint32_t kMaximumRenderThreads = 64u;
constexpr uint64_t kMinimumParallelVoiceSamples = 65'536u;
constexpr uint32_t kMinimumParallelFrames = 8u;

struct RenderJob {
    RenderClassKernel kernel;
    const uint32_t* handles;
    uint32_t handleCount;
};

} // namespace

struct RenderWorkerPool::Impl {
    struct Worker {
        Impl* owner = nullptr;
        uint32_t lane = 0u;
        HANDLE wakeEvent = nullptr;
        HANDLE readyEvent = nullptr;
        HANDLE thread = nullptr;
    };

    Worker* workers = nullptr;
    RenderJob* jobs = nullptr;
    float* mixStorage = nullptr;
    HANDLE doneEvent = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> pendingWorkers{0u};
    RenderSpanContext context{};
    uint32_t totalThreads = 1u;
    uint32_t helperCount = 0u;
    uint32_t dispatchThreads = 1u;
    uint32_t maxFrames = 0u;
    uint32_t mixStride = 0u;
    uint32_t jobCapacity = 0u;
    uint32_t jobCount = 0u;
    bool queueValid = true;

    static DWORD WINAPI ThreadEntry(void* parameter) noexcept {
        Worker* worker = static_cast<Worker*>(parameter);
        Impl* self = worker->owner;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        _mm_setcsr(_mm_getcsr() | 0x8040u); // FTZ + DAZ
        HMODULE avrt = LoadLibraryW(L"avrt.dll");
        using SetMmcss = HANDLE (WINAPI*)(LPCWSTR, LPDWORD);
        using RevertMmcss = BOOL (WINAPI*)(HANDLE);
        SetMmcss setMmcss = avrt ? reinterpret_cast<SetMmcss>(
            GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) : nullptr;
        RevertMmcss revertMmcss = avrt ? reinterpret_cast<RevertMmcss>(
            GetProcAddress(avrt, "AvRevertMmThreadCharacteristics")) : nullptr;
        DWORD taskIndex = 0u;
        HANDLE mmcss = setMmcss ? setMmcss(L"Pro Audio", &taskIndex) : nullptr;
        SetEvent(worker->readyEvent);
        for (;;) {
            WaitForSingleObject(worker->wakeEvent, INFINITE);
            if (self->stopping.load(std::memory_order_acquire)) break;
            self->ProcessLane(worker->lane);
            if (self->pendingWorkers.fetch_sub(
                    1u, std::memory_order_acq_rel) == 1u) {
                SetEvent(self->doneEvent);
            }
        }
        if (mmcss && revertMmcss) revertMmcss(mmcss);
        if (avrt) FreeLibrary(avrt);
        return 0u;
    }

    float* LaneLeft(uint32_t lane) noexcept {
        return mixStorage + static_cast<size_t>(lane) * mixStride * 2u;
    }
    float* LaneRight(uint32_t lane) noexcept {
        return LaneLeft(lane) + mixStride;
    }

    void ProcessLane(uint32_t lane) noexcept {
        float* left = LaneLeft(lane);
        float* right = LaneRight(lane);
        std::memset(left, 0, static_cast<size_t>(context.frameCount) * sizeof(float));
        std::memset(right, 0, static_cast<size_t>(context.frameCount) * sizeof(float));
        RenderSpanContext local = context;
        local.outputLeft = left;
        local.outputRight = right;
        local.frameStart = 0u;
        // Parallel classes are deliberately restricted to classes whose
        // kernels cannot retire or transition during the span.
        local.classChangeHandles = nullptr;
        local.classChangeCount = nullptr;
        for (uint32_t index = lane; index < jobCount; index += dispatchThreads) {
            const RenderJob& job = jobs[index];
            job.kernel(local, job.handles, job.handleCount);
        }
    }

    void ResetStorage() noexcept {
        if (workers) {
            for (uint32_t index = 0u; index < helperCount; ++index) {
                if (workers[index].wakeEvent) {
                    SetEvent(workers[index].wakeEvent);
                }
            }
            for (uint32_t index = 0u; index < helperCount; ++index) {
                if (workers[index].thread) {
                    WaitForSingleObject(workers[index].thread, INFINITE);
                    CloseHandle(workers[index].thread);
                }
                if (workers[index].wakeEvent)
                    CloseHandle(workers[index].wakeEvent);
                if (workers[index].readyEvent)
                    CloseHandle(workers[index].readyEvent);
            }
        }
        if (doneEvent) CloseHandle(doneEvent);
        _aligned_free(mixStorage);
        _aligned_free(jobs);
        delete[] workers;
        workers = nullptr;
        jobs = nullptr;
        mixStorage = nullptr;
        doneEvent = nullptr;
        totalThreads = 1u;
        helperCount = 0u;
        dispatchThreads = 1u;
        maxFrames = 0u;
        mixStride = 0u;
        jobCapacity = 0u;
        jobCount = 0u;
    }
};

RenderWorkerPool::RenderWorkerPool() noexcept : impl_(nullptr) {}

RenderWorkerPool::~RenderWorkerPool() {
    Shutdown();
}

bool RenderWorkerPool::Initialize(uint32_t totalRenderThreads,
                                  uint32_t maxFrames,
                                  uint32_t voiceCapacity) {
    Shutdown();
    if (totalRenderThreads <= 1u) return true;
    totalRenderThreads = (std::min)(totalRenderThreads, kMaximumRenderThreads);
    if (maxFrames == 0u || voiceCapacity == 0u) return false;

    Impl* impl = new (std::nothrow) Impl();
    if (!impl) return false;
    impl->totalThreads = totalRenderThreads;
    impl->helperCount = totalRenderThreads - 1u;
    impl->maxFrames = maxFrames;
    impl->mixStride = (maxFrames + 15u) & ~15u;
    impl->jobCapacity =
        (voiceCapacity + kHandlesPerJob - 1u) / kHandlesPerJob +
        kVoiceRenderClassCount + totalRenderThreads;

    const size_t mixFloats = static_cast<size_t>(totalRenderThreads) *
        impl->mixStride * 2u;
    if (mixFloats > (std::numeric_limits<size_t>::max)() / sizeof(float)) {
        delete impl;
        return false;
    }
    impl->workers = new (std::nothrow) Impl::Worker[impl->helperCount]{};
    impl->jobs = static_cast<RenderJob*>(_aligned_malloc(
        static_cast<size_t>(impl->jobCapacity) * sizeof(RenderJob), 64u));
    impl->mixStorage = static_cast<float*>(_aligned_malloc(
        mixFloats * sizeof(float), 64u));
    impl->doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl->workers || !impl->jobs || !impl->mixStorage ||
        !impl->doneEvent) {
        impl->stopping.store(true, std::memory_order_release);
        impl->ResetStorage();
        delete impl;
        return false;
    }

    for (uint32_t index = 0u; index < impl->helperCount; ++index) {
        Impl::Worker& worker = impl->workers[index];
        worker.owner = impl;
        worker.lane = index + 1u;
        worker.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        worker.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (worker.wakeEvent && worker.readyEvent) {
            worker.thread = CreateThread(nullptr, 0u, Impl::ThreadEntry,
                                         &worker, 0u, nullptr);
        }
        if (!worker.wakeEvent || !worker.readyEvent || !worker.thread) {
            impl->stopping.store(true, std::memory_order_release);
            impl->ResetStorage();
            delete impl;
            return false;
        }
        WaitForSingleObject(worker.readyEvent, INFINITE);
        CloseHandle(worker.readyEvent);
        worker.readyEvent = nullptr;
    }
    impl_ = impl;
    return true;
}

void RenderWorkerPool::Shutdown() noexcept {
    Impl* impl = impl_;
    if (!impl) return;
    impl_ = nullptr;
    impl->stopping.store(true, std::memory_order_release);
    impl->ResetStorage();
    delete impl;
}

uint32_t RenderWorkerPool::GetThreadCount() const noexcept {
    return impl_ ? impl_->totalThreads : 1u;
}

size_t RenderWorkerPool::GetAllocatedBytes() const noexcept {
    if (!impl_) return 0u;
    return sizeof(Impl) +
        static_cast<size_t>(impl_->helperCount) * sizeof(Impl::Worker) +
        static_cast<size_t>(impl_->jobCapacity) * sizeof(RenderJob) +
        static_cast<size_t>(impl_->totalThreads) * impl_->mixStride *
            2u * sizeof(float);
}

bool RenderWorkerPool::ShouldParallelize(uint32_t voiceCount,
                                         uint32_t frameCount) const noexcept {
    return impl_ && frameCount >= kMinimumParallelFrames &&
        voiceCount >= kHandlesPerJob * 2u &&
        static_cast<uint64_t>(voiceCount) * frameCount >=
            kMinimumParallelVoiceSamples;
}

void RenderWorkerPool::BeginSpan(const RenderSpanContext& context) noexcept {
    if (!impl_) return;
    impl_->context = context;
    impl_->jobCount = 0u;
    impl_->queueValid = context.frameCount <= impl_->maxFrames;
}

bool RenderWorkerPool::AddClassRange(RenderClassKernel kernel,
                                     const uint32_t* handles,
                                     uint32_t handleCount) noexcept {
    if (!impl_ || !impl_->queueValid || !kernel || !handles) return false;
    for (uint32_t offset = 0u; offset < handleCount;
         offset += kHandlesPerJob) {
        if (impl_->jobCount >= impl_->jobCapacity) {
            impl_->queueValid = false;
            return false;
        }
        const uint32_t count =
            (std::min)(kHandlesPerJob, handleCount - offset);
        impl_->jobs[impl_->jobCount++] = {kernel, handles + offset, count};
    }
    return true;
}

bool RenderWorkerPool::Execute() noexcept {
    Impl* impl = impl_;
    if (!impl || !impl->queueValid || impl->jobCount < 2u) return false;
    impl->dispatchThreads = (std::min)(impl->totalThreads, impl->jobCount);
    const uint32_t activeHelpers = impl->dispatchThreads - 1u;
    ResetEvent(impl->doneEvent);
    impl->pendingWorkers.store(activeHelpers, std::memory_order_release);
    for (uint32_t index = 0u; index < activeHelpers; ++index)
        SetEvent(impl->workers[index].wakeEvent);

    impl->ProcessLane(0u);
    WaitForSingleObject(impl->doneEvent, INFINITE);

    const uint32_t frames = impl->context.frameCount;
    float* destinationLeft = impl->context.outputLeft + impl->context.frameStart;
    float* destinationRight = impl->context.outputRight + impl->context.frameStart;
    // Fixed lane order makes each run deterministic even though worker wake
    // and completion order are intentionally unconstrained.
    for (uint32_t lane = 0u; lane < impl->dispatchThreads; ++lane) {
        const float* left = impl->LaneLeft(lane);
        const float* right = impl->LaneRight(lane);
        for (uint32_t frame = 0u; frame < frames; ++frame) {
            destinationLeft[frame] += left[frame];
            destinationRight[frame] += right[frame];
        }
    }
    return true;
}

} // namespace svms
