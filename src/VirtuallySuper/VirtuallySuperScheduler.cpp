#include "VirtuallySuperScheduler.h"

#include <algorithm>
#include <cstring>

namespace virtuallysuper {

// ------------------------------------------------------------
// IngressQueue (lock-free ring buffer) implementation
// ------------------------------------------------------------
bool Scheduler::IngressQueue::Push(const NormalizedEvent &ev) {
    uint32_t t = tail.load(std::memory_order_relaxed);
    uint32_t next = (t + 1) & mask;
    if (next == head.load(std::memory_order_acquire)) {
        return false; // Queue is full
    }
    buffer[t] = ev;
    tail.store(next, std::memory_order_release);
    return true;
}

bool Scheduler::IngressQueue::Pop(NormalizedEvent &ev) {
    uint32_t h = head.load(std::memory_order_relaxed);
    if (h == tail.load(std::memory_order_acquire)) {
        return false; // Queue is empty
    }
    ev = buffer[h];
    head.store((h + 1) & mask, std::memory_order_release);
    return true;
}

bool Scheduler::IngressQueue::IsEmpty() const {
    return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
}

void Scheduler::IngressQueue::Clear() {
    head.store(0, std::memory_order_release);
    tail.store(0, std::memory_order_release);
}

void Scheduler::IngressQueue::Resize(uint32_t capacity) {
    // Ensure capacity is power of 2
    uint32_t actualCapacity = 1;
    while (actualCapacity < capacity) {
        actualCapacity <<= 1;
    }
    buffer.resize(actualCapacity);
    mask = actualCapacity - 1;
    head.store(0, std::memory_order_release);
    tail.store(0, std::memory_order_release);
}

// ------------------------------------------------------------
// Scheduler implementation
// ------------------------------------------------------------
Scheduler::Scheduler() 
    : bucketMask_(0), 
      bucketDurationSamples_(0), 
      wheelStartSample_(0), 
      wheelCurrentBucket_(0),
      scheduledCount_(0) {}

Scheduler::~Scheduler() {}

bool Scheduler::Initialize(const SchedulerConfig &config) {
    if (config.ingressCapacity == 0 || config.scheduledCapacity == 0) {
        return false;
    }

    // Initialize ingress queue with power-of-2 capacity
    ingress_.Resize(config.ingressCapacity);

    // Initialize timing wheel
    // Number of buckets based on scheduled capacity and average events per bucket
    // We use a fixed number of buckets (power of two) for efficient modulo
    bucketDurationSamples_ = config.coalesceWindowSamples;
    if (bucketDurationSamples_ <= 0) {
        bucketDurationSamples_ = kDefaultCoalesceWindowSamples;
    }
    
    // Calculate bucket count - aim for ~64 events per bucket on average
    uint32_t targetBucketCount = config.scheduledCapacity / 64;
    if (targetBucketCount < 64) targetBucketCount = 64;
    if (targetBucketCount > 1024) targetBucketCount = 1024;
    
    // Round up to power of 2
    uint32_t bucketCount = 64;
    while (bucketCount < targetBucketCount) {
        bucketCount <<= 1;
    }
    
    bucketMask_ = bucketCount - 1;
    buckets_.resize(bucketCount);
    wheelStartSample_ = 0;
    wheelCurrentBucket_ = 0;

    // Initialize coalesce buffer
    coalesceBuffer_.reserve(16);

    // Reset key states
    for (uint32_t ch = 0; ch < kChannelCount; ++ch) {
        for (uint32_t n = 0; n < kNoteCount; ++n) {
            transitionQueues_[ch][n].count = 0;
            keyStates_[ch][n] = ScheduledKeyState();
        }
    }

    stats_ = SchedulerStats();
    scheduledCount_.store(0, std::memory_order_release);
    return true;
}

void Scheduler::Reset() {
    // Clear all buckets
    for (auto &b : buckets_) {
        AcquireBucketLock(b);
        b.events.clear();
        ReleaseBucketLock(b);
    }
    scheduledCount_.store(0, std::memory_order_release);
    
    // Reset ingress queue
    ingress_.Clear();
    
    // Reset wheel position
    wheelStartSample_ = 0;
    wheelCurrentBucket_ = 0;
    
    // Reset key state
    for (uint32_t ch = 0; ch < kChannelCount; ++ch) {
        for (uint32_t n = 0; n < kNoteCount; ++n) {
            transitionQueues_[ch][n].count = 0;
            keyStates_[ch][n] = ScheduledKeyState();
        }
    }

    stats_ = SchedulerStats();
    coalesceBuffer_.clear();
}

void Scheduler::AcquireBucketLock(Bucket &bucket) {
    // Spin-lock with memory ordering
    while (bucket.lock.exchange(true, std::memory_order_acquire)) {
        // Spin - on x86/x64 this is very fast
    }
}

void Scheduler::ReleaseBucketLock(Bucket &bucket) {
    bucket.lock.store(false, std::memory_order_release);
}

ScheduleDecision Scheduler::EnqueueIngressEvent(const NormalizedEvent &event) {
    if (!ingress_.Push(event)) {
        ++stats_.ingressDropped;
        return ScheduleDecision::Dropped;
    }
    ++stats_.ingressQueued;
    stats_.maxIngressDepth = std::max(stats_.maxIngressDepth, GetIngressCount());
    return ScheduleDecision::Accepted;
}

uint32_t Scheduler::FlushIngressToScheduled(uint32_t maxEvents) {
    uint32_t processed = 0;
    NormalizedEvent ev;
    while (processed < maxEvents && ingress_.Pop(ev)) {
        ScheduleDirect(ev);
        ++processed;
    }
    return processed;
}

ScheduleDecision Scheduler::ScheduleDirect(const NormalizedEvent &event) {
    if (!ValidateKeyEvent(event)) {
        ++stats_.scheduledDropped;
        return ScheduleDecision::Dropped;
    }

    // Determine bucket index for this event
    int64_t bucketIdx = GetBucketIndex(event.targetSample);
    if (bucketIdx < 0) {
        ++stats_.scheduledDropped;
        return ScheduleDecision::Dropped;
    }

    // Get the bucket (with wrap-around)
    uint32_t bucketPos = (uint32_t)(bucketIdx & (int64_t)bucketMask_);
    Bucket &bucket = buckets_[bucketPos];

    // Check for duplicate events before adding (coalescing)
    AcquireBucketLock(bucket);
    
    // Check for duplicates in the same bucket
    bool isDuplicate = false;
    if (EventUsesKey(event.kind)) {
        for (const auto &existing : bucket.events) {
            if (existing.kind == event.kind &&
                existing.channel == event.channel &&
                existing.note == event.note &&
                existing.targetSample == event.targetSample &&
                existing.applyPriority == event.applyPriority) {
                isDuplicate = true;
                break;
            }
        }
    }
    
    if (isDuplicate) {
        ReleaseBucketLock(bucket);
        ++stats_.coalescedEvents;
        return ScheduleDecision::Coalesced;
    }

    // Add event to bucket
    bucket.events.push_back(event);
    ReleaseBucketLock(bucket);

    // Update key state for note events
    if (EventUsesKey(event.kind)) {
        ScheduledKeyState &keyState = keyStates_[event.channel][event.note];
        if (event.kind == EventKind::NoteOn) {
            ++keyState.pendingNoteOnCount;
        } else if (event.kind == EventKind::NoteOff) {
            ++keyState.pendingNoteOffCount;
        }
        keyState.lastScheduledState = EventToScheduledState(event.kind);
        
        // Update transition queue stats
        const TransitionQueue &queue = transitionQueues_[event.channel][event.note];
        stats_.maxTransitionQueueDepth = std::max(stats_.maxTransitionQueueDepth, queue.count);
    }

    ++stats_.scheduledQueued;
    scheduledCount_.fetch_add(1, std::memory_order_relaxed);
    stats_.maxScheduledDepth = std::max(stats_.maxScheduledDepth, GetScheduledCount());
    
    return ScheduleDecision::Accepted;
}

size_t Scheduler::DrainScheduledWindow(int64_t cursorSample, int64_t blockEndSample,
                                       int64_t windowEndSample, NormalizedEvent *outEvents,
                                       size_t outCapacity, int64_t *renderUntilSample) {
    // Initialize wheel start sample if not set
    if (wheelStartSample_ == 0) {
        wheelStartSample_ = cursorSample;
        wheelCurrentBucket_ = 0;
    }

    size_t drained = 0;
    int64_t latestSample = cursorSample;

    // Calculate starting bucket
    int64_t startBucketIdx = GetBucketIndex(cursorSample);
    int64_t endBucketIdx = GetBucketIndex(windowEndSample);
    
    // Process buckets in order
    for (int64_t bucketIdx = startBucketIdx; 
         bucketIdx <= endBucketIdx && drained < outCapacity; 
         ++bucketIdx) {
        
        uint32_t bucketPos = (uint32_t)(bucketIdx & (int64_t)bucketMask_);
        Bucket &bucket = buckets_[bucketPos];

        // Process events in this bucket
        AcquireBucketLock(bucket);
        
        // Sort events by targetSample within the bucket for ordered draining
        // (only if there are multiple events)
        if (bucket.events.size() > 1) {
            std::sort(bucket.events.begin(), bucket.events.end(),
                [](const NormalizedEvent &a, const NormalizedEvent &b) {
                    if (a.targetSample != b.targetSample)
                        return a.targetSample < b.targetSample;
                    if (a.applyPriority != b.applyPriority)
                        return a.applyPriority < b.applyPriority;
                    return a.sequence < b.sequence;
                });
        }
        
        // Drain events that fall within the window
        for (auto it = bucket.events.begin(); it != bucket.events.end() && drained < outCapacity;) {
            if (it->targetSample >= windowEndSample) {
                break; // Events are sorted, so we can stop early
            }
            
            // Copy event to output
            outEvents[drained++] = *it;
            latestSample = std::max(latestSample, it->targetSample);
            
            // Update key state
            if (EventUsesKey(it->kind)) {
                ScheduledKeyState &ks = keyStates_[it->channel][it->note];
                if (it->kind == EventKind::NoteOn && ks.pendingNoteOnCount > 0) {
                    --ks.pendingNoteOnCount;
                } else if (it->kind == EventKind::NoteOff && ks.pendingNoteOffCount > 0) {
                    --ks.pendingNoteOffCount;
                }
            }
            
            it = bucket.events.erase(it);
            scheduledCount_.fetch_sub(1, std::memory_order_relaxed);
            ++stats_.drainedEvents;
        }
        
        ReleaseBucketLock(bucket);
    }

    if (renderUntilSample) {
        if (drained > 0) {
            *renderUntilSample = std::min(latestSample, blockEndSample);
        } else {
            // No events drained - render until end of block or next scheduled event
            *renderUntilSample = blockEndSample;
        }
    }

    return drained;
}

uint32_t Scheduler::GetScheduledCount() const {
    return scheduledCount_.load(std::memory_order_acquire);
}

uint32_t Scheduler::GetIngressCount() const {
    uint32_t t = ingress_.tail.load(std::memory_order_acquire);
    uint32_t h = ingress_.head.load(std::memory_order_acquire);
    return (t - h) & ingress_.mask;
}

const SchedulerStats &Scheduler::GetStats() const {
    return stats_;
}

const ScheduledKeyState &Scheduler::GetKeyState(uint32_t channel, uint32_t note) const {
    return keyStates_[channel][note];
}

uint32_t Scheduler::GetTransitionQueueDepth(uint32_t channel, uint32_t note) const {
    return transitionQueues_[channel][note].count;
}

bool Scheduler::ValidateKeyEvent(const NormalizedEvent &event) const {
    return event.channel < kChannelCount && event.note < kNoteCount;
}

int64_t Scheduler::GetBucketIndex(int64_t targetSample) const {
    if (bucketDurationSamples_ <= 0) {
        return -1;
    }
    int64_t offset = targetSample - wheelStartSample_;
    if (offset < 0) {
        offset = 0;
    }
    return offset / bucketDurationSamples_;
}

void Scheduler::RecomputeKeyState(uint8_t channel, uint8_t note) {
    ScheduledKeyState &state = keyStates_[channel][note];
    const TransitionQueue &queue = transitionQueues_[channel][note];

    state.pendingNoteOnCount = 0;
    state.pendingNoteOffCount = 0;
    state.soundingGenerations = 0;
    state.lastScheduledState = ScheduledKeyStateValue::Unknown;

    for (uint32_t i = 0; i < queue.count; ++i) {
        if (EventIsNoteOn(queue.entries[i].kind)) {
            ++state.pendingNoteOnCount;
        } else if (EventIsNoteOff(queue.entries[i].kind)) {
            ++state.pendingNoteOffCount;
        }
    }

    if (queue.count > 0) {
        state.lastScheduledState = EventToScheduledState(queue.entries[queue.count - 1].kind);
    }
}

void Scheduler::RemoveTransitionSlot(uint8_t channel, uint8_t note, uint32_t slotIndex) {
    TransitionQueue &queue = transitionQueues_[channel][note];
    for (uint32_t i = 0; i < queue.count; ++i) {
        if (queue.entries[i].slotIndex != slotIndex) {
            continue;
        }
        for (uint32_t j = i + 1; j < queue.count; ++j) {
            queue.entries[j - 1] = queue.entries[j];
        }
        --queue.count;
        break;
    }
    RecomputeKeyState(channel, note);
}

int Scheduler::FindDuplicateInBucket(const Bucket &bucket, const NormalizedEvent &event) const {
    if (!EventUsesKey(event.kind)) {
        return -1;
    }
    
    for (size_t i = 0; i < bucket.events.size(); ++i) {
        const NormalizedEvent &existing = bucket.events[i];
        if (existing.kind == event.kind &&
            existing.channel == event.channel &&
            existing.note == event.note &&
            existing.targetSample == event.targetSample &&
            existing.applyPriority == event.applyPriority) {
            return (int)i;
        }
    }
    return -1;
}

} // namespace virtuallysuper
