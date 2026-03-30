#ifndef VIRTUALLYSUPER_SCHEDULER_H
#define VIRTUALLYSUPER_SCHEDULER_H

#include "VirtuallySuperTypes.h"

#include <vector>

namespace virtuallysuper {

class Scheduler {
public:
  Scheduler();

  bool Initialize(const SchedulerConfig &config);
  void Reset();

  ScheduleDecision EnqueueIngressEvent(const NormalizedEvent &event);
  uint32_t FlushIngressToScheduled(uint32_t maxEvents);
  ScheduleDecision ScheduleDirect(const NormalizedEvent &event);

  size_t DrainScheduledWindow(int64_t cursorSample, int64_t blockEndSample,
                              int64_t windowEndSample,
                              NormalizedEvent *outEvents, size_t outCapacity,
                              int64_t *renderUntilSample);

  uint32_t GetScheduledCount() const;
  uint32_t GetIngressCount() const;
  const SchedulerStats &GetStats() const;
  const ScheduledKeyState &GetKeyState(uint32_t channel, uint32_t note) const;
  uint32_t GetTransitionQueueDepth(uint32_t channel, uint32_t note) const;

private:
  struct ScheduledSlot {
    bool active;
    uint32_t eventId;
    NormalizedEvent event;

    ScheduledSlot() : active(false), eventId(0), event() {}
  };

  ScheduleDecision ScheduleInternal(const NormalizedEvent &event);
  bool ValidateKeyEvent(const NormalizedEvent &event) const;
  bool IsEarlier(uint32_t lhsSlot, uint32_t rhsSlot) const;
  void HeapSwap(uint32_t a, uint32_t b);
  void HeapSiftUp(uint32_t index);
  void HeapSiftDown(uint32_t index);
  void HeapPush(uint32_t slotIndex);
  uint32_t HeapPop();
  void HeapRemove(uint32_t slotIndex);
  uint32_t AllocateSlot();
  void ReleaseSlot(uint32_t slotIndex);
  void RemoveTransitionSlot(uint8_t channel, uint8_t note, uint32_t slotIndex);
  void RecomputeKeyState(uint8_t channel, uint8_t note);
  int FindDuplicateTransitionSlot(const NormalizedEvent &event) const;
  int FindOverflowReplacementSlot(const NormalizedEvent &event) const;

  SchedulerConfig config_;
  bool initialized_;

  std::vector<NormalizedEvent> ingress_;
  uint32_t ingressHead_;
  uint32_t ingressTail_;
  uint32_t ingressCount_;

  std::vector<ScheduledSlot> slots_;
  std::vector<uint32_t> heap_;
  std::vector<int32_t> heapPositions_;
  std::vector<uint32_t> freeList_;
  uint32_t heapCount_;
  uint32_t freeCount_;
  uint32_t nextEventId_;

  TransitionQueue transitionQueues_[kChannelCount][kNoteCount];
  ScheduledKeyState keyStates_[kChannelCount][kNoteCount];
  SchedulerStats stats_;
};

} // namespace virtuallysuper

#endif
