#ifndef SVMS_EVENT_SCHEDULER_H
#define SVMS_EVENT_SCHEDULER_H

#include "SVMSRenderScalar.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace svms {

struct ScheduledRenderEvent {
    RenderEvent event{};
    int64_t targetFrame = 0;
    uint32_t sequence = 0;
};

// Audio-thread-owned ordered event store. Incoming priority lanes are not
// globally ordered, so each callback must restore absolute-frame/sequence
// order. The old binary heap made every extracted event pay O(log N), which
// dominates the callback at million-event-per-second rates. This store uses
// a fixed-allocation stable radix pass when a batch is finalized and then
// extracts the ordered prefix in O(1) per event.
class EventScheduler {
public:
    explicit EventScheduler(uint32_t capacity = kDefaultEventRingCapacity)
        : capacity_(capacity) {
        events_.reserve(capacity_);
        scratch_.resize(capacity_);
    }

    bool Enqueue(const ScheduledRenderEvent& ev) {
        FinalizeBatch();
        CompactConsumed();
        if (events_.size() >= capacity_) return false;
        const auto position = std::lower_bound(
            events_.begin(), events_.end(), ev, EarlierFirst);
        events_.insert(position, ev);
        if (events_.size() > highWater_)
            highWater_ = static_cast<uint32_t>(events_.size());
        return true;
    }

    bool EnqueueBatched(const ScheduledRenderEvent& ev) {
        if (!batchDirty_) CompactConsumed();
        if (events_.size() >= capacity_) return false;
        events_.push_back(ev);
        batchDirty_ = true;
        if (events_.size() > highWater_)
            highWater_ = static_cast<uint32_t>(events_.size());
        return true;
    }

    void FinalizeBatch() {
        if (!batchDirty_) return;
        CompactConsumed();
        // The compiler worker emits individually ordered chunks.  Existing
        // future events and newly appended chunks therefore form only a few
        // natural runs; merge those linearly on the audio thread.  Direct or
        // adversarial callers with many inversions retain the exact radix
        // fallback.
        if (!MergeNaturalRuns()) RadixSort();
        batchDirty_ = false;
    }

    bool Empty() const { return Size() == 0u; }
    uint32_t Size() const {
        return static_cast<uint32_t>(events_.size() - readIndex_);
    }
    uint32_t HighWater() const { return highWater_; }

    bool PopBefore(int64_t endSample, ScheduledRenderEvent& out) {
        FinalizeBatch();
        if (readIndex_ >= events_.size() ||
            events_[readIndex_].targetFrame >= endSample) {
            return false;
        }
        out = events_[readIndex_++];
        if (readIndex_ == events_.size()) {
            events_.clear();
            readIndex_ = 0u;
        }
        return true;
    }

    void Reset() {
        events_.clear();
        readIndex_ = 0u;
        highWater_ = 0u;
        batchDirty_ = false;
    }

private:
    static bool EarlierFirst(const ScheduledRenderEvent& a,
                             const ScheduledRenderEvent& b) {
        if (a.targetFrame != b.targetFrame)
            return a.targetFrame < b.targetFrame;
        return a.sequence < b.sequence;
    }

    static uint8_t RadixByte(const ScheduledRenderEvent& event,
                             uint32_t pass) {
        // Sequence is the secondary key, so its four least-significant-digit
        // passes come first. Signed targetFrame is the primary key; flipping
        // its sign bit maps signed ordering to unsigned radix ordering.
        if (pass < 4u) {
            return static_cast<uint8_t>(event.sequence >> (pass * 8u));
        }
        const uint32_t frameByte = pass - 4u;
        uint8_t value = static_cast<uint8_t>(
            static_cast<uint64_t>(event.targetFrame) >> (frameByte * 8u));
        if (frameByte == 7u) value ^= 0x80u;
        return value;
    }

    void CompactConsumed() {
        if (readIndex_ == 0u) return;
        const std::size_t remaining = events_.size() - readIndex_;
        if (remaining != 0u) {
            std::memmove(events_.data(), events_.data() + readIndex_,
                         remaining * sizeof(ScheduledRenderEvent));
        }
        events_.resize(remaining);
        readIndex_ = 0u;
    }

    bool MergeNaturalRuns() {
        const std::size_t count = events_.size();
        if (count < 2u) return true;
        static constexpr uint32_t kMaxNaturalRuns = 64u;
        std::size_t beginsA[kMaxNaturalRuns]{};
        std::size_t endsA[kMaxNaturalRuns]{};
        std::size_t beginsB[kMaxNaturalRuns]{};
        std::size_t endsB[kMaxNaturalRuns]{};
        uint32_t runCount = 1u;
        beginsA[0] = 0u;
        for (std::size_t i = 1u; i < count; ++i) {
            if (!EarlierFirst(events_[i], events_[i - 1u])) continue;
            endsA[runCount - 1u] = i;
            if (runCount >= kMaxNaturalRuns) return false;
            beginsA[runCount++] = i;
        }
        endsA[runCount - 1u] = count;
        if (runCount == 1u) return true;

        ScheduledRenderEvent* source = events_.data();
        ScheduledRenderEvent* destination = scratch_.data();
        std::size_t* begins = beginsA;
        std::size_t* ends = endsA;
        std::size_t* nextBegins = beginsB;
        std::size_t* nextEnds = endsB;
        while (runCount > 1u) {
            uint32_t nextCount = 0u;
            for (uint32_t run = 0u; run < runCount; run += 2u) {
                const std::size_t outputBegin = begins[run];
                nextBegins[nextCount] = outputBegin;
                if (run + 1u >= runCount) {
                    const std::size_t outputEnd = ends[run];
                    std::memcpy(destination + outputBegin, source + outputBegin,
                                (outputEnd - outputBegin) *
                                    sizeof(ScheduledRenderEvent));
                    nextEnds[nextCount++] = outputEnd;
                    continue;
                }
                std::size_t left = begins[run];
                const std::size_t leftEnd = ends[run];
                std::size_t right = begins[run + 1u];
                const std::size_t rightEnd = ends[run + 1u];
                std::size_t output = outputBegin;
                while (left < leftEnd && right < rightEnd) {
                    if (EarlierFirst(source[right], source[left]))
                        destination[output++] = source[right++];
                    else
                        destination[output++] = source[left++];
                }
                if (left < leftEnd) {
                    std::memcpy(destination + output, source + left,
                                (leftEnd - left) * sizeof(ScheduledRenderEvent));
                } else if (right < rightEnd) {
                    std::memcpy(destination + output, source + right,
                                (rightEnd - right) * sizeof(ScheduledRenderEvent));
                }
                nextEnds[nextCount++] = rightEnd;
            }
            std::swap(source, destination);
            std::swap(begins, nextBegins);
            std::swap(ends, nextEnds);
            runCount = nextCount;
        }
        if (source != events_.data()) {
            std::memcpy(events_.data(), source,
                        count * sizeof(ScheduledRenderEvent));
        }
        return true;
    }

    void RadixSort() {
        const std::size_t count = events_.size();
        if (count < 2u) return;

        int64_t minimumFrame = events_[0].targetFrame;
        int64_t maximumFrame = minimumFrame;
        for (std::size_t i = 1u; i < count; ++i) {
            minimumFrame = (std::min)(minimumFrame, events_[i].targetFrame);
            maximumFrame = (std::max)(maximumFrame, events_[i].targetFrame);
        }
        const bool compactFrame = static_cast<uint64_t>(
            maximumFrame - minimumFrame) <= UINT32_MAX;
        const uint32_t passCount = compactFrame ? 8u : 12u;

        ScheduledRenderEvent* source = events_.data();
        ScheduledRenderEvent* destination = scratch_.data();
        for (uint32_t pass = 0u; pass < passCount; ++pass) {
            std::size_t offsets[256]{};
            auto byteFor = [pass, compactFrame, minimumFrame](
                               const ScheduledRenderEvent& event) {
                if (pass < 4u)
                    return static_cast<uint8_t>(event.sequence >> (pass * 8u));
                if (compactFrame) {
                    const uint32_t relative = static_cast<uint32_t>(
                        event.targetFrame - minimumFrame);
                    return static_cast<uint8_t>(relative >> ((pass - 4u) * 8u));
                }
                return RadixByte(event, pass);
            };
            for (std::size_t i = 0u; i < count; ++i)
                ++offsets[byteFor(source[i])];

            std::size_t running = 0u;
            for (uint32_t bucket = 0u; bucket < 256u; ++bucket) {
                const std::size_t bucketCount = offsets[bucket];
                offsets[bucket] = running;
                running += bucketCount;
            }
            for (std::size_t i = 0u; i < count; ++i) {
                const uint8_t bucket = byteFor(source[i]);
                destination[offsets[bucket]++] = source[i];
            }
            std::swap(source, destination);
        }
        // Both the compact eight-pass path and full twelve-pass path are
        // even, so the final source is events_.data().
    }

    uint32_t capacity_;
    uint32_t highWater_ = 0u;
    std::size_t readIndex_ = 0u;
    bool batchDirty_ = false;
    std::vector<ScheduledRenderEvent> events_;
    std::vector<ScheduledRenderEvent> scratch_;
};

} // namespace svms

#endif
