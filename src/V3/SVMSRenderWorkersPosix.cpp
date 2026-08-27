#include "SVMSRenderWorkers.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <linux/futex.h>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <xmmintrin.h>

namespace svms {
namespace {

constexpr uint32_t kHandlesPerJob = 256u;
constexpr uint32_t kMaximumRenderThreads = 64u;
constexpr uint64_t kMinimumParallelVoiceSamples = 65'536u;
constexpr uint32_t kMinimumParallelFrames = 8u;
// At or above this pool occupancy, short event-fragmented spans still pay
// for their fan-out handshake many times over.
constexpr uint32_t kLargePoolShortSpanVoices = 1024u;
// Spans shorter than kMinimumParallelFrames need an even larger voice pool
// before the wake/sync handshake pays off (chopped buzz workloads where
// event-fragmented per-class spans are 1-7 frames).
constexpr uint32_t kMinimumVoicesShortSpan = 2048u;

struct RenderJob {
    RenderClassKernel kernel = nullptr;
    const uint32_t* handles = nullptr;
    uint32_t handleCount = 0u;
};

int FutexWait(std::atomic<uint32_t>& value, uint32_t expected) noexcept {
    return static_cast<int>(syscall(SYS_futex,
        reinterpret_cast<uint32_t*>(&value), FUTEX_WAIT_PRIVATE, expected,
        nullptr, nullptr, 0));
}

void FutexWakeAll(std::atomic<uint32_t>& value) noexcept {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&value),
            FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

} // namespace

struct RenderWorkerPool::Impl {
    struct Worker {
        Impl* owner = nullptr;
        uint32_t lane = 0u;
        std::thread thread;
    };

    Worker* workers = nullptr;
    RenderJob* jobs = nullptr;
    float* mixStorage = nullptr;
    std::atomic<bool> stopping{false};
    alignas(64) std::atomic<uint32_t> workGeneration{0u};
    alignas(64) std::atomic<uint32_t> readyWorkers{0u};
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

    float* JobLeft(uint32_t job) noexcept {
        return mixStorage + static_cast<size_t>(job) * jobMixStride;
    }
    float* JobRight(uint32_t job) noexcept { return JobLeft(job) + mixStride; }

    uint32_t ProcessJobs() noexcept {
        uint32_t claimed = 0u;
        for (;;) {
            const uint32_t index = nextJob.fetch_add(1u,
                std::memory_order_relaxed);
            if (index >= jobCount) break;
            ++claimed;
            float* left = JobLeft(index);
            float* right = JobRight(index);
            std::memset(left, 0, static_cast<size_t>(context.frameCount) *
                                     sizeof(float));
            std::memset(right, 0, static_cast<size_t>(context.frameCount) *
                                      sizeof(float));
            RenderSpanContext local = context;
            local.outputLeft = left;
            local.outputRight = right;
            local.frameStart = 0u;
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

    static void ThreadEntry(Worker* worker) noexcept {
        Impl* self = worker->owner;
        _mm_setcsr(_mm_getcsr() | 0x8040u);
        sched_param parameters{};
        parameters.sched_priority = 1;
        (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters);
        uint32_t observed = self->workGeneration.load(
            std::memory_order_acquire);
        self->readyWorkers.fetch_add(1u, std::memory_order_release);
        FutexWakeAll(self->readyWorkers);
        for (;;) {
            while (!self->stopping.load(std::memory_order_acquire) &&
                   self->workGeneration.load(std::memory_order_acquire) ==
                       observed) {
                FutexWait(self->workGeneration, observed);
            }
            if (self->stopping.load(std::memory_order_acquire)) break;
            observed = self->workGeneration.load(std::memory_order_acquire);
            if (worker->lane >= self->dispatchThreads) continue;
            const uint32_t claimed = self->ProcessJobs();
            self->helperJobs.fetch_add(claimed, std::memory_order_relaxed);
            self->completedWorkers.fetch_add(1u, std::memory_order_release);
            FutexWakeAll(self->completedWorkers);
        }
    }

    void StartWork(uint32_t helpers) noexcept {
        activeHelpers = helpers;
        nextJob.store(0u, std::memory_order_relaxed);
        completedWorkers.store(0u, std::memory_order_relaxed);
        workGeneration.fetch_add(1u, std::memory_order_release);
        FutexWakeAll(workGeneration);
    }

    void WaitForHelpers() noexcept {
        while (completedWorkers.load(std::memory_order_acquire) <
               activeHelpers) {
            const uint32_t completed = completedWorkers.load(
                std::memory_order_relaxed);
            FutexWait(completedWorkers, completed);
        }
    }

    void ResetStorage() noexcept {
        stopping.store(true, std::memory_order_release);
        workGeneration.fetch_add(1u, std::memory_order_release);
        FutexWakeAll(workGeneration);
        for (uint32_t i = 0u; i < helperCount; ++i) {
            if (workers[i].thread.joinable()) workers[i].thread.join();
        }
        _aligned_free(mixStorage);
        _aligned_free(jobs);
        delete[] workers;
        workers = nullptr;
        jobs = nullptr;
        mixStorage = nullptr;
        totalThreads = 1u;
        helperCount = 0u;
        indexedInFlight = false;
    }
};

RenderWorkerPool::RenderWorkerPool() noexcept : impl_(nullptr) {}
RenderWorkerPool::~RenderWorkerPool() { Shutdown(); }

bool RenderWorkerPool::Initialize(uint32_t totalRenderThreads,
                                  uint32_t maxFrames,
                                  uint32_t voiceCapacity) {
    Shutdown();
    if (totalRenderThreads <= 1u) return true;
    totalRenderThreads = (std::min)(totalRenderThreads,
                                    kMaximumRenderThreads);
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
    if (!impl->workers || !impl->jobs || !impl->mixStorage) {
        impl->ResetStorage();
        delete impl;
        return false;
    }
    try {
        for (uint32_t i = 0u; i < impl->helperCount; ++i) {
            impl->workers[i].owner = impl;
            impl->workers[i].lane = i + 1u;
            impl->workers[i].thread = std::thread(
                Impl::ThreadEntry, &impl->workers[i]);
        }
        while (impl->readyWorkers.load(std::memory_order_acquire) <
               impl->helperCount) {
            const uint32_t ready = impl->readyWorkers.load(
                std::memory_order_relaxed);
            FutexWait(impl->readyWorkers, ready);
        }
    } catch (...) {
        impl->ResetStorage();
        delete impl;
        return false;
    }
    impl_ = impl;
    return true;
}

void RenderWorkerPool::Shutdown() noexcept {
    Impl* impl = impl_;
    if (!impl) return;
    impl_ = nullptr;
    impl->ResetStorage();
    delete impl;
}

uint32_t RenderWorkerPool::GetThreadCount() const noexcept {
    return impl_ ? impl_->totalThreads : 1u;
}

float RenderWorkerPool::GetHelperJobPercent() const noexcept {
    if (!impl_) return 0.0f;
    const uint64_t helper = impl_->helperJobs.load(std::memory_order_relaxed);
    const uint64_t coordinator = impl_->coordinatorJobs.load(
        std::memory_order_relaxed);
    const uint64_t total = helper + coordinator;
    return total ? static_cast<float>(static_cast<double>(helper) * 100.0 /
                                      static_cast<double>(total)) : 0.0f;
}

size_t RenderWorkerPool::GetAllocatedBytes() const noexcept {
    if (!impl_) return 0u;
    return sizeof(Impl) +
        static_cast<size_t>(impl_->helperCount) * sizeof(Impl::Worker) +
        static_cast<size_t>(impl_->jobCapacity) * sizeof(RenderJob) +
        static_cast<size_t>(impl_->jobCapacity) * impl_->jobMixStride *
            sizeof(float);
}

bool RenderWorkerPool::ShouldParallelize(uint32_t voices,
                                         uint32_t frames) const noexcept {
    return ClassifyParallelization(voices, frames) ==
        RenderParallelRejectReason::None;
}

RenderParallelRejectReason RenderWorkerPool::ClassifyParallelization(
    uint32_t voices, uint32_t frames) const noexcept {
    if (!impl_ || impl_->totalThreads <= 1u)
        return RenderParallelRejectReason::Unavailable;
    if (frames == 0u)
        return RenderParallelRejectReason::TooFewFrames;
    if (voices < kHandlesPerJob)
        return RenderParallelRejectReason::TooFewVoices;
    if (static_cast<uint64_t>(voices) * frames <
        kMinimumParallelVoiceSamples) {
        // Short spans (< kMinimumParallelFrames) need a very large voice
        // pool to amortize wake/sync overhead (chopped buzz workloads).
        if (frames < kMinimumParallelFrames) {
            if (voices < kMinimumVoicesShortSpan)
                return RenderParallelRejectReason::TooFewVoiceSamples;
        } else {
            if (voices < kLargePoolShortSpanVoices)
                return RenderParallelRejectReason::TooFewVoiceSamples;
        }
    }
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
        const uint32_t count = (std::min)(kHandlesPerJob,
                                           handleCount - offset);
        impl_->jobs[impl_->jobCount++] = {kernel, handles + offset, count};
    }
    return true;
}

bool RenderWorkerPool::Execute() noexcept {
    Impl* impl = impl_;
    if (!impl || !impl->queueValid || impl->jobCount < 2u) return false;
    impl->dispatchThreads = (std::min)(impl->totalThreads, impl->jobCount);
    impl->StartWork(impl->dispatchThreads - 1u);
    const uint32_t claimed = impl->ProcessJobs();
    impl->coordinatorJobs.fetch_add(claimed, std::memory_order_relaxed);
    impl->WaitForHelpers();
    const uint32_t frames = impl->context.frameCount;
    float* destinationLeft = impl->context.outputLeft + impl->context.frameStart;
    float* destinationRight = impl->context.outputRight + impl->context.frameStart;
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

bool RenderWorkerPool::ExecuteIndexed(uint32_t jobs, uint32_t frames,
                                      float* left, float* right,
                                      IndexedRenderJob callback,
                                      void* userData) noexcept {
    return BeginIndexed(jobs, frames, left, right, callback, userData) &&
           FinishIndexed();
}

bool RenderWorkerPool::BeginIndexed(uint32_t jobs, uint32_t frames,
                                    float* left, float* right,
                                    IndexedRenderJob callback,
                                    void* userData) noexcept {
    Impl* impl = impl_;
    if (!impl || !callback || !left || !right || jobs < 2u ||
        jobs > impl->jobCapacity || frames == 0u ||
        frames > impl->maxFrames || impl->indexedInFlight) return false;
    impl->context = {};
    impl->context.outputLeft = left;
    impl->context.outputRight = right;
    impl->context.frameCount = frames;
    impl->indexedCallback = callback;
    impl->indexedUserData = userData;
    impl->jobCount = jobs;
    impl->dispatchThreads = (std::min)(impl->totalThreads, jobs);
    impl->indexedInFlight = true;
    impl->StartWork(impl->dispatchThreads - 1u);
    return true;
}

bool RenderWorkerPool::FinishIndexed() noexcept {
    Impl* impl = impl_;
    if (!impl || !impl->indexedInFlight) return false;
    const uint32_t claimed = impl->ProcessJobs();
    impl->coordinatorJobs.fetch_add(claimed, std::memory_order_relaxed);
    impl->WaitForHelpers();
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
