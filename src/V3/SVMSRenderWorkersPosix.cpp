// POSIX render worker pool. Job semantics mirror SVMSRenderWorkers.cpp (the
// Windows pool) exactly: scalar fallback on a kernel refusal, per-job
// retirement/class-change scratch merged after the jobs, per-channel bus
// planes, fixed job-order reduction. Only the thread wake/wait primitives
// differ (futex here). Keep the two files in step.

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
constexpr uint32_t kMinimumVoicesShortSpan = 1024u;

struct RenderJob {
    RenderClassKernel kernel;
    RenderClassKernel fallback;
    const uint32_t* handles;
    uint32_t handleCount;
};

// Resolve the scalar sibling of a selected backend class kernel once on the
// coordinator thread, so workers can honor a backend refusal.
RenderClassKernel ResolveScalarFallback(RenderClassKernel kernel) noexcept {
    if (!kernel) return nullptr;
    const RenderKernelSet& scalar = GetScalarRenderKernelSet();
    const RenderKernelSet* sets[] = {&scalar, &GetSSE2RenderKernelSet(),
                                     &GetAVX2RenderKernelSet()};
    for (const RenderKernelSet* set : sets) {
        for (uint32_t i = 0u; i < kVoiceRenderClassCount; ++i) {
            if (set->kernels[i] == kernel) return scalar.kernels[i];
        }
    }
    return nullptr;
}

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
    // Bus-mode (per-MIDI-channel limiter) job planes, grown lazily on the
    // coordinator thread. Layout: [job][channel][L,R][mixStride].
    float* busStorage = nullptr;
    float** jobBusLeftPtrs = nullptr;
    float** jobBusRightPtrs = nullptr;
    uint32_t busJobCapacity = 0u;
    size_t jobBusStride = 0u;
    bool busMode = false;
    // Per-job span lifecycle scratch, merged by Execute().
    SpanRetirement* jobRetirements = nullptr;
    uint32_t* jobClassChanges = nullptr;
    uint32_t* jobRetireCounts = nullptr;
    uint32_t* jobClassChangeCounts = nullptr;
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
    float* const* JobBusLeft(uint32_t job) noexcept {
        return jobBusLeftPtrs + static_cast<size_t>(job) * kChannelCount;
    }
    float* const* JobBusRight(uint32_t job) noexcept {
        return jobBusRightPtrs + static_cast<size_t>(job) * kChannelCount;
    }

    bool EnsureBusStorage(uint32_t neededJobs) noexcept {
        if (neededJobs <= busJobCapacity) return true;
        const uint32_t newCapacity =
            (std::max)(neededJobs, static_cast<uint32_t>(16u));
        const size_t floatsPerJob = static_cast<size_t>(kChannelCount) *
            2u * mixStride;
        if (floatsPerJob > (std::numeric_limits<size_t>::max)() /
                               sizeof(float) / newCapacity) {
            return false;
        }
        float* newPlanes = static_cast<float*>(_aligned_malloc(
            floatsPerJob * newCapacity * sizeof(float), 64u));
        float** newLeft = static_cast<float**>(_aligned_malloc(
            static_cast<size_t>(newCapacity) * kChannelCount * sizeof(float*), 64u));
        float** newRight = static_cast<float**>(_aligned_malloc(
            static_cast<size_t>(newCapacity) * kChannelCount * sizeof(float*), 64u));
        if (!newPlanes || !newLeft || !newRight) {
            _aligned_free(newPlanes);
            _aligned_free(newLeft);
            _aligned_free(newRight);
            return false;
        }
        for (uint32_t job = 0u; job < newCapacity; ++job) {
            for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
                float* plane = newPlanes +
                    (static_cast<size_t>(job) * kChannelCount + channel) *
                        2u * mixStride;
                newLeft[job * kChannelCount + channel] = plane;
                newRight[job * kChannelCount + channel] = plane + mixStride;
            }
        }
        _aligned_free(busStorage);
        _aligned_free(jobBusLeftPtrs);
        _aligned_free(jobBusRightPtrs);
        busStorage = newPlanes;
        jobBusLeftPtrs = newLeft;
        jobBusRightPtrs = newRight;
        busJobCapacity = newCapacity;
        jobBusStride = floatsPerJob;
        return true;
    }

    void ZeroJobBuses(uint32_t job, uint32_t frames) noexcept {
        const size_t bytes = static_cast<size_t>(frames) * sizeof(float);
        float* const* left = JobBusLeft(job);
        float* const* right = JobBusRight(job);
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
            std::memset(left[channel], 0, bytes);
            std::memset(right[channel], 0, bytes);
        }
    }

    uint32_t ProcessJobs() noexcept {
        uint32_t claimed = 0u;
        const bool jobsBusMode = busMode;
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
            IndexedJobMix mix{left, right, nullptr, nullptr};
            if (jobsBusMode) {
                ZeroJobBuses(index, context.frameCount);
                mix.busLeft = JobBusLeft(index);
                mix.busRight = JobBusRight(index);
            }
            RenderSpanContext local = context;
            local.outputLeft = left;
            local.outputRight = right;
            if (jobsBusMode) {
                local.channelBusLeft = mix.busLeft;
                local.channelBusRight = mix.busRight;
            }
            local.frameStart = 0u;
            local.retirements = jobRetirements +
                static_cast<size_t>(index) * kHandlesPerJob;
            local.retirementCount = &jobRetireCounts[index];
            *local.retirementCount = 0u;
            local.classChangeHandles = jobClassChanges +
                static_cast<size_t>(index) * kHandlesPerJob;
            local.classChangeCount = &jobClassChangeCounts[index];
            *local.classChangeCount = 0u;
            if (indexedCallback) {
                indexedCallback(index, mix, context.frameCount,
                                indexedUserData);
            } else {
                const RenderJob& job = jobs[index];
                // False means "no mutation; use the scalar fallback".
                const bool consumed =
                    job.kernel(local, job.handles, job.handleCount);
                if (!consumed && job.fallback != job.kernel)
                    (void)job.fallback(local, job.handles, job.handleCount);
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
        for (;;) {
            const uint32_t completed =
                completedWorkers.load(std::memory_order_acquire);
            if (completed >= activeHelpers) break;
            // Shutdown can race an in-flight span: woken helpers then never
            // finish, so waiting for them would hang forever.
            if (stopping.load(std::memory_order_acquire)) break;
            FutexWait(completedWorkers, completed);
        }
    }

    // Sums job mixes (or per-channel bus planes) in fixed job order.
    void MergeMixes(uint32_t frameStart) noexcept {
        const uint32_t frames = context.frameCount;
        if (busMode) {
            for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
                float* destLeft = context.channelBusLeft[channel] + frameStart;
                float* destRight = context.channelBusRight[channel] + frameStart;
                for (uint32_t job = 0u; job < jobCount; ++job) {
                    const float* srcLeft = JobBusLeft(job)[channel];
                    const float* srcRight = JobBusRight(job)[channel];
                    for (uint32_t frame = 0u; frame < frames; ++frame) {
                        destLeft[frame] += srcLeft[frame];
                        destRight[frame] += srcRight[frame];
                    }
                }
            }
            return;
        }
        float* destLeft = context.outputLeft + frameStart;
        float* destRight = context.outputRight + frameStart;
        for (uint32_t job = 0u; job < jobCount; ++job) {
            const float* left = JobLeft(job);
            const float* right = JobRight(job);
            for (uint32_t frame = 0u; frame < frames; ++frame) {
                destLeft[frame] += left[frame];
                destRight[frame] += right[frame];
            }
        }
    }

    void ResetStorage() noexcept {
        stopping.store(true, std::memory_order_release);
        workGeneration.fetch_add(1u, std::memory_order_release);
        FutexWakeAll(workGeneration);
        for (uint32_t i = 0u; workers && i < helperCount; ++i) {
            if (workers[i].thread.joinable()) workers[i].thread.join();
        }
        _aligned_free(mixStorage);
        _aligned_free(jobs);
        _aligned_free(jobRetirements);
        _aligned_free(jobClassChanges);
        _aligned_free(jobRetireCounts);
        _aligned_free(jobClassChangeCounts);
        _aligned_free(busStorage);
        _aligned_free(jobBusLeftPtrs);
        _aligned_free(jobBusRightPtrs);
        delete[] workers;
        workers = nullptr;
        jobs = nullptr;
        mixStorage = nullptr;
        jobRetirements = nullptr;
        jobClassChanges = nullptr;
        jobRetireCounts = nullptr;
        jobClassChangeCounts = nullptr;
        busStorage = nullptr;
        jobBusLeftPtrs = nullptr;
        jobBusRightPtrs = nullptr;
        busJobCapacity = 0u;
        jobBusStride = 0u;
        busMode = false;
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
    const size_t scratchEntries =
        static_cast<size_t>(impl->jobCapacity) * kHandlesPerJob;
    impl->jobRetirements = static_cast<SpanRetirement*>(_aligned_malloc(
        scratchEntries * sizeof(SpanRetirement), 64u));
    impl->jobClassChanges = static_cast<uint32_t*>(_aligned_malloc(
        scratchEntries * sizeof(uint32_t), 64u));
    impl->jobRetireCounts = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(impl->jobCapacity) * sizeof(uint32_t), 64u));
    impl->jobClassChangeCounts = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(impl->jobCapacity) * sizeof(uint32_t), 64u));
    if (!impl->workers || !impl->jobs || !impl->mixStorage ||
        !impl->jobRetirements || !impl->jobClassChanges ||
        !impl->jobRetireCounts || !impl->jobClassChangeCounts) {
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

// Affinity policy is a Windows scheduling feature; POSIX keeps default
// scheduler placement.
void RenderWorkerPool::ApplyAffinity() noexcept {}

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
            sizeof(float) +
        static_cast<size_t>(impl_->busJobCapacity) * impl_->jobBusStride *
            sizeof(float) +
        2u * static_cast<size_t>(impl_->busJobCapacity) * kChannelCount *
            sizeof(float*);
}

size_t RenderWorkerPool::EstimateAllocatedBytes(
    uint32_t totalRenderThreads, uint32_t maxFrames,
    uint32_t voiceCapacity) noexcept {
    if (totalRenderThreads <= 1u) return 0u;
    totalRenderThreads = (std::min)(totalRenderThreads,
                                    kMaximumRenderThreads);
    if (maxFrames == 0u || voiceCapacity == 0u) return 0u;
    const uint32_t helperCount = totalRenderThreads - 1u;
    const uint32_t mixStride = (maxFrames + 15u) & ~15u;
    const size_t jobMixStride = static_cast<size_t>(mixStride) * 2u;
    const uint32_t jobCapacity =
        (voiceCapacity + kHandlesPerJob - 1u) / kHandlesPerJob +
        kVoiceRenderClassCount + totalRenderThreads;
    const size_t mixFloats = static_cast<size_t>(jobCapacity) *
        jobMixStride;
    if (mixFloats > (std::numeric_limits<size_t>::max)() / sizeof(float))
        return (std::numeric_limits<size_t>::max)();
    return sizeof(Impl) +
        static_cast<size_t>(helperCount) * sizeof(Impl::Worker) +
        static_cast<size_t>(jobCapacity) * sizeof(RenderJob) +
        static_cast<size_t>(jobCapacity) * kHandlesPerJob *
            (sizeof(SpanRetirement) + 2u * sizeof(uint32_t)) +
        mixFloats * sizeof(float);
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
    impl_->busMode = context.channelBusLeft != nullptr;
    impl_->queueValid = context.frameCount <= impl_->maxFrames;
}

bool RenderWorkerPool::AddClassRange(RenderClassKernel kernel,
                                     const uint32_t* handles,
                                     uint32_t handleCount) noexcept {
    if (!impl_ || !impl_->queueValid || !kernel || !handles) return false;
    const RenderClassKernel fallback = ResolveScalarFallback(kernel);
    if (!fallback) {
        // An unknown kernel has no defined fallback for a refusal: keep the
        // class on the caller's serial path.
        impl_->queueValid = false;
        return false;
    }
    for (uint32_t offset = 0u; offset < handleCount;
         offset += kHandlesPerJob) {
        // Bus mode caps fan-out: per-job channel planes are ~16x a legacy
        // job mix, so beyond this the fan-out cannot pay for its scratch.
        if ((impl_->busMode && impl_->jobCount >= 16u) ||
            impl_->jobCount >= impl_->jobCapacity) {
            impl_->queueValid = false;
            return false;
        }
        const uint32_t count = (std::min)(kHandlesPerJob,
                                           handleCount - offset);
        impl_->jobs[impl_->jobCount++] = {kernel, fallback, handles + offset,
                                          count};
    }
    return true;
}

bool RenderWorkerPool::Execute() noexcept {
    Impl* impl = impl_;
    if (!impl || !impl->queueValid || impl->jobCount < 2u) return false;
    if (impl->busMode && !impl->EnsureBusStorage(impl->jobCount)) {
        impl->queueValid = false;
        return false;
    }
    impl->dispatchThreads = (std::min)(impl->totalThreads, impl->jobCount);
    impl->StartWork(impl->dispatchThreads - 1u);
    const uint32_t claimed = impl->ProcessJobs();
    impl->coordinatorJobs.fetch_add(claimed, std::memory_order_relaxed);
    impl->WaitForHelpers();
    impl->MergeMixes(impl->context.frameStart);
    // Merge per-job lifecycle records. Retirement order is restored by the
    // caller's (frameOffset, capturePosition) sort; class-change refresh is
    // order-independent.
    if (impl->context.retirements != nullptr &&
        impl->context.retirementCount != nullptr) {
        const uint32_t written = *impl->context.retirementCount;
        SpanRetirement* const base = impl->context.retirements + written;
        uint32_t total = 0u;
        for (uint32_t job = 0u; job < impl->jobCount; ++job) {
            const uint32_t count = impl->jobRetireCounts[job];
            std::memcpy(base + total, impl->jobRetirements +
                static_cast<size_t>(job) * kHandlesPerJob,
                static_cast<size_t>(count) * sizeof(SpanRetirement));
            total += count;
        }
        *impl->context.retirementCount = written + total;
    }
    if (impl->context.classChangeHandles != nullptr &&
        impl->context.classChangeCount != nullptr) {
        const uint32_t written = *impl->context.classChangeCount;
        uint32_t* const base = impl->context.classChangeHandles + written;
        uint32_t total = 0u;
        for (uint32_t job = 0u; job < impl->jobCount; ++job) {
            const uint32_t count = impl->jobClassChangeCounts[job];
            std::memcpy(base + total, impl->jobClassChanges +
                static_cast<size_t>(job) * kHandlesPerJob,
                static_cast<size_t>(count) * sizeof(uint32_t));
            total += count;
        }
        *impl->context.classChangeCount = written + total;
    }
    return true;
}

bool RenderWorkerPool::ExecuteIndexed(uint32_t jobs, uint32_t frames,
                                      float* left, float* right,
                                      IndexedRenderJob callback,
                                      void* userData,
                                      float* const* channelBusLeft,
                                      float* const* channelBusRight) noexcept {
    return BeginIndexed(jobs, frames, left, right, callback, userData,
                        channelBusLeft, channelBusRight) &&
           FinishIndexed();
}

bool RenderWorkerPool::BeginIndexed(uint32_t jobs, uint32_t frames,
                                    float* left, float* right,
                                    IndexedRenderJob callback,
                                    void* userData,
                                    float* const* channelBusLeft,
                                    float* const* channelBusRight) noexcept {
    Impl* impl = impl_;
    if (!impl || !callback || !left || !right || jobs < 2u ||
        jobs > impl->jobCapacity || frames == 0u ||
        frames > impl->maxFrames || impl->indexedInFlight) return false;
    impl->context = {};
    impl->context.outputLeft = left;
    impl->context.outputRight = right;
    impl->context.frameCount = frames;
    impl->context.channelBusLeft = channelBusLeft;
    impl->context.channelBusRight = channelBusRight;
    impl->indexedCallback = callback;
    impl->indexedUserData = userData;
    impl->jobCount = jobs;
    impl->busMode = channelBusLeft != nullptr;
    if (impl->busMode && !impl->EnsureBusStorage(jobs)) return false;
    impl->queueValid = true;
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
    impl->MergeMixes(0u);
    impl->indexedCallback = nullptr;
    impl->indexedUserData = nullptr;
    impl->indexedInFlight = false;
    impl->activeHelpers = 0u;
    return true;
}

} // namespace svms
