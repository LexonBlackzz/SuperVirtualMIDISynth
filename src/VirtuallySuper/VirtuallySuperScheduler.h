#ifndef VIRTUALLYSUPER_SCHEDULER_H
#define VIRTUALLYSUPER_SCHEDULER_H

#include "VirtuallySuperTypes.h"
#include <atomic>
#include <vector>

namespace virtuallysuper {

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    bool Initialize(const SchedulerConfig &config);
    void Reset();

    ScheduleDecision EnqueueIngressEvent(const NormalizedEvent &event);
    uint32_t FlushIngressToScheduled(uint32_t maxEvents);
    ScheduleDecision ScheduleDirect(const NormalizedEvent &event);

    size_t DrainScheduledWindow(int64_t cursorSample, int64_t blockEndSample,
                                int64_t windowEndSample, NormalizedEvent *outEvents,
                                size_t outCapacity, int64_t *renderUntilSample);

    uint32_t GetScheduledCount() const;
    uint32_t GetIngressCount() const;
    const SchedulerStats &GetStats() const;
    const ScheduledKeyState &GetKeyState(uint32_t channel, uint32_t note) const;
    uint32_t GetTransitionQueueDepth(uint32_t channel, uint32_t note) const;

private:
    // Timing wheel bucket - holds events for a specific time slot
    struct Bucket {
        std::vector<NormalizedEvent> events;
        std::atomic<bool> lock;
        
        Bucket() : lock(false) {}
        Bucket(const Bucket &other) : events(other.events), lock(false) {}
        Bucket &operator=(const Bucket &other) {
            if (this != &other) {
                events = other.events;
            }
            return *this;
        }
    };

    // Lock-free MPSC queue (ring buffer) for ingress events
    struct IngressQueue {
        std::vector<NormalizedEvent> buffer;
        std::atomic<uint32_t> head;
        std::atomic<uint32_t> tail;
        uint32_t mask;

        IngressQueue() : head(0), tail(0), mask(0) {}
        
        bool Push(const NormalizedEvent &ev);
        bool Pop(NormalizedEvent &ev);
        bool IsEmpty() const;
        void Clear();
        void Resize(uint32_t capacity);
    };

    IngressQueue ingress_;
    std::vector<Bucket> buckets_;
    uint32_t bucketMask_;
    int64_t bucketDurationSamples_;
    int64_t wheelStartSample_;
    int64_t wheelCurrentBucket_;

    // Key state tracking for note-on/off ordering
    TransitionQueue transitionQueues_[kChannelCount][kNoteCount];
    ScheduledKeyState keyStates_[kChannelCount][kNoteCount];

    SchedulerStats stats_;
    std::atomic<uint32_t> scheduledCount_;
    
    // For coalescing duplicate events
    std::vector<NormalizedEvent> coalesceBuffer_;

    bool ValidateKeyEvent(const NormalizedEvent &event) const;
    int64_t GetBucketIndex(int64_t targetSample) const;
    void RecomputeKeyState(uint8_t channel, uint8_t note);
    void RemoveTransitionSlot(uint8_t channel, uint8_t note, uint32_t slotIndex);
    int FindDuplicateInBucket(const Bucket &bucket, const NormalizedEvent &event) const;
    void AcquireBucketLock(Bucket &bucket);
    void ReleaseBucketLock(Bucket &bucket);
};

} // namespace virtuallysuper

#endif
