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
    using WaitOnAddressProc = BOOL (WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
    using WakeByAddressAllProc = VOID (WINAPI*)(PVOID);
    WaitOnAddressProc waitOnAddress = nullptr;
    WakeByAddressAllProc wakeByAddressAll = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> pendingWorkers{0u};
    alignas(64) std::atomic<uint32_t> workGeneration{0u};
    alignas(64) std::atomic<uint32_t> nextJob{0u};
    alignas(64) std::atomic<uint32_t> completedWorkers{0u};
    alignas(64) std::atomic<uint64_t> helperJobs{0u};
    alignas(64) std::atomic<uint64_t> coordinatorJobs{0u};
    RenderSpanContext context{};
    IndexedRenderJob indexedCallback = nullptr;
    void* indexedUserData = nullptr;
    uint32_t totalThreads = 1u;
    uint32_t helperCount = 0u;
    uint32_t dispatchThreads = 1u;
    uint32_t maxFrames = 0u;
    uint32_t mixStride = 0u;
    uint32_t jobCapacity = 0u;
    uint32_t jobCount = 0u;
    uint32_t activeHelpers = 0u;
    size_t jobMixStride = 0u;
    bool queueValid = true;
    bool indexedInFlight = false;

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
        uint32_t observedGeneration =
            self->workGeneration.load(std::memory_order_acquire);
        for (;;) {
#if !defined(SVMS_XP_COMPAT)
            if (self->waitOnAddress) {
                while (!self->stopping.load(std::memory_order_acquire) &&
                       self->workGeneration.load(std::memory_order_acquire) ==
                           observedGeneration) {
                    self->waitOnAddress(&self->workGeneration,
                                        &observedGeneration,
                                        sizeof(observedGeneration), INFINITE);
                }
            } else
#endif
            {
                WaitForSingleObject(worker->wakeEvent, INFINITE);
            }
            if (self->stopping.load(std::memory_order_acquire)) break;
            observedGeneration =
                self->workGeneration.load(std::memory_order_acquire);
            if (worker->lane >= self->dispatchThreads) continue;
            const uint32_t claimed = self->ProcessJobs();
            self->helperJobs.fetch_add(claimed, std::memory_order_relaxed);
            self->completedWorkers.fetch_add(1u, std::memory_order_release);
#if !defined(SVMS_XP_COMPAT)
            if (self->wakeByAddressAll)
                self->wakeByAddressAll(&self->completedWorkers);
#endif
            if (self->pendingWorkers.fetch_sub(1u,
                                               std::memory_order_acq_rel) == 1u) {
                SetEvent(self->doneEvent);
            }
        }
        if (mmcss && revertMmcss) revertMmcss(mmcss);
        if (avrt) FreeLibrary(avrt);
        return 0u;
    }

    float* JobLeft(uint32_t job) noexcept {
        return mixStorage + static_cast<size_t>(job) * jobMixStride;
    }
    float* JobRight(uint32_t job) noexcept {
        return JobLeft(job) + mixStride;
    }

    uint32_t ProcessJobs() noexcept {
        uint32_t claimed = 0u;
        for (;;) {
            const uint32_t index =
                nextJob.fetch_add(1u, std::memory_order_relaxed);
            if (index >= jobCount) break;
            ++claimed;
            float* left = JobLeft(index);
            float* right = JobRight(index);
            std::memset(left, 0,
                        static_cast<size_t>(context.frameCount) * sizeof(float));
            std::memset(right, 0,
                        static_cast<size_t>(context.frameCount) * sizeof(float));
            RenderSpanContext local = context;
            local.outputLeft = left;
            local.outputRight = right;
            local.frameStart = 0u;
            // Parallel jobs currently contain only classes whose kernels do
            // not retire or change render class during the span.
            local.classChangeHandles = nullptr;
            local.classChangeCount = nullptr;
            if (indexedCallback) {
                indexedCallback(index, left, right, context.frameCount,
                                indexedUserData);
            } else {
                const RenderJob& job = jobs[index];
                job.kernel(local, job.handles, job.handleCount);
            }
        }
        return claimed;
    }

    void ResetStorage() noexcept {
        if (workers) {
#if !defined(SVMS_XP_COMPAT)
            if (wakeByAddressAll) {
                workGeneration.fetch_add(1u, std::memory_order_release);
                wakeByAddressAll(&workGeneration);
            }
#endif
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
        activeHelpers = 0u;
        jobMixStride = 0u;
        indexedInFlight = false;
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
    impl->jobMixStride = static_cast<size_t>(impl->mixStride) * 2u;
    impl->jobCapacity =
        (voiceCapacity + kHandlesPerJob - 1u) / kHandlesPerJob +
        kVoiceRenderClassCount + totalRenderThreads;

    const size_t mixFloats = static_cast<size_t>(impl->jobCapacity) *
        impl->jobMixStride;
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
#if !defined(SVMS_XP_COMPAT)
    if (HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll")) {
        impl->waitOnAddress = reinterpret_cast<Impl::WaitOnAddressProc>(
            GetProcAddress(kernel32, "WaitOnAddress"));
        impl->wakeByAddressAll = reinterpret_cast<Impl::WakeByAddressAllProc>(
            GetProcAddress(kernel32, "WakeByAddressAll"));
    }
#endif
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
#if !defined(SVMS_XP_COMPAT)
            if (worker.thread) {
                SYSTEM_INFO systemInfo{};
                GetSystemInfo(&systemInfo);
                if (systemInfo.dwNumberOfProcessors != 0u) {
                    SetThreadIdealProcessor(
                        worker.thread,
                        (index + 1u) % systemInfo.dwNumberOfProcessors);
                }
            }
#endif
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

float RenderWorkerPool::GetHelperJobPercent() const noexcept {
    if (!impl_) return 0.0f;
    const uint64_t helper =
        impl_->helperJobs.load(std::memory_order_relaxed);
    const uint64_t coordinator =
        impl_->coordinatorJobs.load(std::memory_order_relaxed);
    const uint64_t total = helper + coordinator;
    return total != 0u
        ? static_cast<float>(static_cast<double>(helper) * 100.0 /
                             static_cast<double>(total))
        : 0.0f;
}

size_t RenderWorkerPool::GetAllocatedBytes() const noexcept {
    if (!impl_) return 0u;
    return sizeof(Impl) +
        static_cast<size_t>(impl_->helperCount) * sizeof(Impl::Worker) +
        static_cast<size_t>(impl_->jobCapacity) * sizeof(RenderJob) +
        static_cast<size_t>(impl_->jobCapacity) * impl_->jobMixStride *
            sizeof(float);
}

bool RenderWorkerPool::ShouldParallelize(uint32_t voiceCount,
                                         uint32_t frameCount) const noexcept {
    return ClassifyParallelization(voiceCount, frameCount) ==
        RenderParallelRejectReason::None;
}

RenderParallelRejectReason RenderWorkerPool::ClassifyParallelization(
    uint32_t voiceCount, uint32_t frameCount) const noexcept {
    if (!impl_ || impl_->totalThreads <= 1u)
        return RenderParallelRejectReason::Unavailable;
    if (frameCount < kMinimumParallelFrames)
        return RenderParallelRejectReason::TooFewFrames;
    if (voiceCount < kHandlesPerJob * 2u)
        return RenderParallelRejectReason::TooFewVoices;
    if (static_cast<uint64_t>(voiceCount) * frameCount <
        kMinimumParallelVoiceSamples)
        return RenderParallelRejectReason::TooFewVoiceSamples;
    return RenderParallelRejectReason::None;
}

void RenderWorkerPool::BeginSpan(const RenderSpanContext& context) noexcept {
    if (!impl_) return;
    impl_->context = context;
    impl_->indexedCallback = nullptr;
    impl_->indexedUserData = nullptr;
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
    impl->nextJob.store(0u, std::memory_order_relaxed);
    impl->completedWorkers.store(0u, std::memory_order_relaxed);
    impl->pendingWorkers.store(activeHelpers, std::memory_order_release);
    impl->workGeneration.fetch_add(1u, std::memory_order_release);
#if !defined(SVMS_XP_COMPAT)
    if (impl->wakeByAddressAll) {
        impl->wakeByAddressAll(&impl->workGeneration);
    } else
#endif
    {
        for (uint32_t index = 0u; index < activeHelpers; ++index)
            SetEvent(impl->workers[index].wakeEvent);
    }

    const uint32_t coordinatorClaimed = impl->ProcessJobs();
    impl->coordinatorJobs.fetch_add(coordinatorClaimed,
                                    std::memory_order_relaxed);
    if (activeHelpers != 0u) {
#if !defined(SVMS_XP_COMPAT)
        if (impl->waitOnAddress) {
            for (;;) {
                uint32_t completed =
                    impl->completedWorkers.load(std::memory_order_acquire);
                if (completed >= activeHelpers) break;
                impl->waitOnAddress(&impl->completedWorkers, &completed,
                                    sizeof(completed), INFINITE);
            }
        } else
#endif
        {
            WaitForSingleObject(impl->doneEvent, INFINITE);
        }
    }

    const uint32_t frames = impl->context.frameCount;
    float* destinationLeft = impl->context.outputLeft + impl->context.frameStart;
    float* destinationRight = impl->context.outputRight + impl->context.frameStart;
    // Fixed lane order makes each run deterministic even though worker wake
    // and completion order are intentionally unconstrained.
    for (uint32_t job = 0u; job < impl->jobCount; ++job) {
        const float* left = impl->JobLeft(job);
        const float* right = impl->JobRight(job);
        for (uint32_t frame = 0u; frame < frames; ++frame) {
            destinationLeft[frame] += left[frame];
            destinationRight[frame] += right[frame];
        }
    }
    return true;
}

bool RenderWorkerPool::ExecuteIndexed(uint32_t jobCount, uint32_t frameCount,
                                      float* outputLeft, float* outputRight,
                                      IndexedRenderJob callback,
                                      void* userData) noexcept {
    if (!BeginIndexed(jobCount, frameCount, outputLeft, outputRight,
                      callback, userData)) {
        return false;
    }
    return FinishIndexed();
}

bool RenderWorkerPool::BeginIndexed(uint32_t jobCount, uint32_t frameCount,
                                    float* outputLeft, float* outputRight,
                                    IndexedRenderJob callback,
                                    void* userData) noexcept {
    Impl* impl = impl_;
    if (!impl || !callback || !outputLeft || !outputRight || jobCount < 2u ||
        jobCount > impl->jobCapacity || frameCount == 0u ||
        frameCount > impl->maxFrames || impl->indexedInFlight) {
        return false;
    }
    impl->context = {};
    impl->context.outputLeft = outputLeft;
    impl->context.outputRight = outputRight;
    impl->context.frameCount = frameCount;
    impl->indexedCallback = callback;
    impl->indexedUserData = userData;
    impl->jobCount = jobCount;
    impl->queueValid = true;
    impl->dispatchThreads = (std::min)(impl->totalThreads, jobCount);
    impl->activeHelpers = impl->dispatchThreads - 1u;
    ResetEvent(impl->doneEvent);
    impl->nextJob.store(0u, std::memory_order_relaxed);
    impl->completedWorkers.store(0u, std::memory_order_relaxed);
    impl->pendingWorkers.store(impl->activeHelpers,
                               std::memory_order_release);
    impl->indexedInFlight = true;
    impl->workGeneration.fetch_add(1u, std::memory_order_release);
#if !defined(SVMS_XP_COMPAT)
    if (impl->wakeByAddressAll) {
        impl->wakeByAddressAll(&impl->workGeneration);
    } else
#endif
    {
        for (uint32_t index = 0u; index < impl->activeHelpers; ++index)
            SetEvent(impl->workers[index].wakeEvent);
    }
    return true;
}

bool RenderWorkerPool::FinishIndexed() noexcept {
    Impl* impl = impl_;
    if (!impl || !impl->indexedInFlight) return false;

    // Once planning is complete, the coordinator joins the same atomic job
    // queue instead of waiting idle for helpers to drain it.
    const uint32_t coordinatorClaimed = impl->ProcessJobs();
    impl->coordinatorJobs.fetch_add(coordinatorClaimed,
                                    std::memory_order_relaxed);
    if (impl->activeHelpers != 0u) {
#if !defined(SVMS_XP_COMPAT)
        if (impl->waitOnAddress) {
            for (;;) {
                uint32_t completed =
                    impl->completedWorkers.load(std::memory_order_acquire);
                if (completed >= impl->activeHelpers) break;
                impl->waitOnAddress(&impl->completedWorkers, &completed,
                                    sizeof(completed), INFINITE);
            }
        } else
#endif
        {
            WaitForSingleObject(impl->doneEvent, INFINITE);
        }
    }

    const uint32_t frames = impl->context.frameCount;
    for (uint32_t job = 0u; job < impl->jobCount; ++job) {
        const float* left = impl->JobLeft(job);
        const float* right = impl->JobRight(job);
        for (uint32_t frame = 0u; frame < frames; ++frame) {
            impl->context.outputLeft[frame] += left[frame];
            impl->context.outputRight[frame] += right[frame];
        }
    }
    impl->indexedCallback = nullptr;
    impl->indexedUserData = nullptr;
    impl->indexedInFlight = false;
    impl->activeHelpers = 0u;
    return true;
}

} // namespace svms
