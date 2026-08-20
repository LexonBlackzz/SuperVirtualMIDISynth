#include "SVMSEventCompile.h"
#include "SVMSMPSCQueue.h"
#include "SVMSPSCQueue.h"
#include "SVMSEventPages.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

constexpr uint32_t kChunkCapacity = 8192u;

uint32_t ParseU32(const char* value, uint32_t fallback) {
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return end && *end == '\0' && parsed <= UINT32_MAX
        ? static_cast<uint32_t>(parsed) : fallback;
}

double Milliseconds(std::chrono::steady_clock::time_point begin,
                    std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

} // namespace

int main(int argc, char** argv) {
    uint32_t eventCount = 131072u;
    uint32_t iterations = 10u;
    uint32_t eventRate = 1886000u;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--events") == 0 && i + 1 < argc)
            eventCount = ParseU32(argv[++i], eventCount);
        else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
            iterations = ParseU32(argv[++i], iterations);
        else if (std::strcmp(argv[i], "--event-rate") == 0 && i + 1 < argc)
            eventRate = ParseU32(argv[++i], eventRate);
        else {
            std::fprintf(stderr,
                "usage: svms_v3_event_bench [--events 1..262144] "
                "[--iterations N] [--event-rate N]\n");
            return 1;
        }
    }
    // This workload alternates full-velocity note-ons and note-offs, so its
    // real burst ceiling is the loud + state lane capacity (262,144).
    if (eventCount == 0u || eventCount > 262144u ||
        iterations == 0u || eventRate == 0u) {
        std::fprintf(stderr, "invalid benchmark arguments\n");
        return 1;
    }

    auto ingress = std::make_unique<
        svms::PriorityEventIngress<svms::TimestampedMidiEvent>>();
    auto pages = std::make_unique<svms::CompiledEventPagePool>();
    auto scheduler = std::make_unique<svms::PagedEventScheduler>();
    if (!pages->ConfigureCapacity(eventCount) ||
        !scheduler->Configure(pages.get(), eventCount)) return 2;

    constexpr uint64_t qpcFrequency = 10000000u;
    constexpr uint64_t epoch = 100000000u;
    constexpr uint32_t sampleRate = 44100u;
    constexpr uint32_t leadFrames = 2048u;
    double producerMs = 0.0;
    double compilerMs = 0.0;
    double compilerSortMs = 0.0;
    double compilerPublishMs = 0.0;
    double callbackMs = 0.0;
    uint64_t checksum = 0u;
    uint64_t callbackRuns = 0u;

    for (uint32_t iteration = 0u; iteration < iterations; ++iteration) {
        ingress->DrainAvailable();
        scheduler->Reset();

        const auto producerBegin = std::chrono::steady_clock::now();
        for (uint32_t sequence = 0u; sequence < eventCount; ++sequence) {
            svms::TimestampedMidiEvent event{};
            event.sequence = sequence;
            event.qpcTimestamp = epoch +
                static_cast<uint64_t>(sequence) * qpcFrequency / eventRate;
            const uint32_t note = sequence & 0x7fu;
            if ((sequence & 1u) == 0u) {
                event.message = 0x90u | (note << 8u) | (127u << 16u);
                if (!ingress->TryPush(svms::EventLane::Loud, event)) return 2;
            } else {
                event.message = 0x80u | (note << 8u);
                if (!ingress->TryPush(svms::EventLane::State, event)) return 2;
            }
        }
        const auto producerEnd = std::chrono::steady_clock::now();

        const auto compilerBegin = producerEnd;
        svms::PriorityEventIngress<svms::TimestampedMidiEvent>::
            OrderedMergeState mergeState{};
        uint32_t remaining = eventCount;
        while (remaining != 0u) {
            const uint32_t count = (std::min)(remaining, kChunkCapacity);
            uint32_t pageIndex = svms::kInvalidEventPage;
            if (!pages->AcquireForCompiler(pageIndex)) return 3;
            auto& page = pages->Page(pageIndex);
            uint32_t drained = 0u;
            auto compileOne = [&](const svms::TimestampedMidiEvent& timed) {
                svms::ScheduledRenderEvent scheduled{};
                if (!svms::CompileTimestampedEvent(
                        timed, epoch, qpcFrequency, sampleRate, leadFrames,
                        scheduled)) {
                    return false;
                }
                page.events[drained++] = scheduled;
                return true;
            };
            svms::TimestampedMidiEvent timed{};
            if (!ingress->TryPopSequenceOrdered(timed, mergeState) ||
                !compileOne(timed)) return 3;
            bool compileFailed = false;
            uint32_t additionallyDrained = 0u;
            while (drained < count &&
                   ingress->TryPopSequenceOrdered(timed, mergeState)) {
                if (!compileOne(timed)) compileFailed = true;
                ++additionallyDrained;
            }
            if (compileFailed || additionallyDrained != count - 1u ||
                drained != count) return 3;
            const auto sortBegin = std::chrono::steady_clock::now();
            svms::SortCompiledEventPage(page.events, pages->SortScratch(), count);
            const auto sortEnd = std::chrono::steady_clock::now();
            if (!pages->PublishFromCompiler(pageIndex, count)) return 3;
            const auto publishEnd = std::chrono::steady_clock::now();
            compilerSortMs += Milliseconds(sortBegin, sortEnd);
            compilerPublishMs += Milliseconds(sortEnd, publishEnd);
            remaining -= count;
        }
        const auto compilerEnd = std::chrono::steady_clock::now();

        const auto callbackBegin = compilerEnd;
        svms::ScheduledRenderEvent scheduled{};
        scheduler->ImportAllReady();
        int64_t previousFrame = INT64_MIN;
        uint32_t previousSequence = 0u;
        uint32_t extracted = 0u;
        for (;;) {
            const svms::ScheduledRenderEvent* run = nullptr;
            const uint32_t runCount = scheduler->PeekRunBefore(
                INT64_MAX, eventCount - extracted, run);
            if (runCount == 0u) break;
            ++callbackRuns;
            for (uint32_t i = 0u; i < runCount; ++i) {
                scheduled = run[i];
                if (scheduled.targetFrame < previousFrame ||
                    (scheduled.targetFrame == previousFrame &&
                     extracted != 0u &&
                     static_cast<int32_t>(scheduled.sequence -
                                          previousSequence) < 0)) return 5;
                previousFrame = scheduled.targetFrame;
                previousSequence = scheduled.sequence;
                checksum += scheduled.sequence +
                    static_cast<uint64_t>(scheduled.targetFrame);
                ++extracted;
            }
            scheduler->ConsumeRun(runCount);
        }
        if (extracted != eventCount) return 5;
        const auto callbackEnd = std::chrono::steady_clock::now();

        producerMs += Milliseconds(producerBegin, producerEnd);
        compilerMs += Milliseconds(compilerBegin, compilerEnd);
        callbackMs += Milliseconds(callbackBegin, callbackEnd);
    }

    const double totalEvents = static_cast<double>(eventCount) * iterations;
    const auto rate = [totalEvents](double milliseconds) {
        return milliseconds > 0.0 ? totalEvents / (milliseconds * 1000.0) : 0.0;
    };
    const double callbackPerBatch = callbackMs / iterations;
    const double audioDeadlineMs = 2048.0 * 1000.0 / sampleRate;
    std::printf(
        "events=%u iterations=%u chunk=%u scheduled_bytes=%zu\n"
        "producer_ms=%.3f producer_Mevents_s=%.3f\n"
        "compiler_ms=%.3f compiler_Mevents_s=%.3f\n"
        "compiler_drain_compile_ms=%.3f sort_ms=%.3f publish_ms=%.3f\n"
        "callback_order_ms=%.3f callback_Mevents_s=%.3f "
        "callback_budget_pct=%.2f merge_runs=%llu checksum=%llu\n",
        eventCount, iterations, kChunkCapacity,
        sizeof(svms::ScheduledRenderEvent),
        producerMs, rate(producerMs), compilerMs, rate(compilerMs),
        compilerMs - compilerSortMs - compilerPublishMs,
        compilerSortMs, compilerPublishMs,
        callbackMs, rate(callbackMs),
        callbackPerBatch * 100.0 / audioDeadlineMs,
        static_cast<unsigned long long>(callbackRuns),
        static_cast<unsigned long long>(checksum));
    return 0;
}
