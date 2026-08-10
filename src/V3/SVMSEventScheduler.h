#ifndef SVMS_EVENT_SCHEDULER_H
#define SVMS_EVENT_SCHEDULER_H

#include "SVMSRenderScalar.h"
#include <vector>
#include <algorithm>

namespace svms {

struct ScheduledRenderEvent {
    RenderEvent event{};
    int64_t targetFrame = 0;
    uint32_t sequence = 0;
};

// Audio-thread-owned min-heap.  Events are ordered by absolute sample and
// then ingress sequence, so equal-time note transitions stay deterministic.
class EventScheduler {
public:
    explicit EventScheduler(uint32_t capacity = kDefaultEventRingCapacity)
        : capacity_(capacity) { heap_.reserve(capacity_); }

    bool Enqueue(const ScheduledRenderEvent& ev) {
        if (heap_.size() >= capacity_) return false;
        heap_.push_back(ev);
        std::push_heap(heap_.begin(), heap_.end(), LaterFirst);
        if (heap_.size() > highWater_) highWater_ = static_cast<uint32_t>(heap_.size());
        return true;
    }

    bool Empty() const { return heap_.empty(); }
    uint32_t Size() const { return static_cast<uint32_t>(heap_.size()); }
    uint32_t HighWater() const { return highWater_; }

    bool PopBefore(int64_t endSample, ScheduledRenderEvent& out) {
        if (heap_.empty() || heap_.front().targetFrame >= endSample) return false;
        std::pop_heap(heap_.begin(), heap_.end(), LaterFirst);
        out = heap_.back();
        heap_.pop_back();
        return true;
    }

    void Reset() { heap_.clear(); highWater_ = 0; }

private:
    static bool LaterFirst(const ScheduledRenderEvent& a, const ScheduledRenderEvent& b) {
        if (a.targetFrame != b.targetFrame) return a.targetFrame > b.targetFrame;
        return a.sequence > b.sequence;
    }

    uint32_t capacity_;
    uint32_t highWater_ = 0;
    std::vector<ScheduledRenderEvent> heap_;
};

} // namespace svms

#endif
