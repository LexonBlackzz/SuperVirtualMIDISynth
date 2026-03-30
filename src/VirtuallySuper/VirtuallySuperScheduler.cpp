#include "VirtuallySuperScheduler.h"

#include <algorithm>

namespace virtuallysuper {

Scheduler::Scheduler()
    : config_(), initialized_(false), ingress_(), ingressHead_(0),
      ingressTail_(0), ingressCount_(0), slots_(), heap_(), heapPositions_(),
      freeList_(), heapCount_(0), freeCount_(0), nextEventId_(1), stats_() {}

bool Scheduler::Initialize(const SchedulerConfig &config) {
  if (config.ingressCapacity == 0 || config.scheduledCapacity == 0)
    return false;

  config_ = config;
  ingress_.assign(config_.ingressCapacity, NormalizedEvent());
  slots_.assign(config_.scheduledCapacity, ScheduledSlot());
  heap_.assign(config_.scheduledCapacity, 0);
  heapPositions_.assign(config_.scheduledCapacity, -1);
  freeList_.assign(config_.scheduledCapacity, 0);

  initialized_ = true;
  Reset();
  return true;
}

void Scheduler::Reset() {
  ingressHead_ = 0;
  ingressTail_ = 0;
  ingressCount_ = 0;
  heapCount_ = 0;
  freeCount_ = (uint32_t)slots_.size();
  nextEventId_ = 1;
  stats_ = SchedulerStats();

  for (uint32_t i = 0; i < freeCount_; ++i) {
    slots_[i] = ScheduledSlot();
    heap_[i] = 0;
    heapPositions_[i] = -1;
    freeList_[i] = freeCount_ - 1U - i;
  }

  for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
    for (uint32_t note = 0; note < kNoteCount; ++note) {
      transitionQueues_[channel][note] = TransitionQueue();
      keyStates_[channel][note] = ScheduledKeyState();
    }
  }
}

ScheduleDecision Scheduler::EnqueueIngressEvent(const NormalizedEvent &event) {
  if (!initialized_)
    return ScheduleDecision::Dropped;

  if (ingressCount_ >= config_.ingressCapacity) {
    ++stats_.ingressDropped;
    return ScheduleDecision::Dropped;
  }

  ingress_[ingressTail_] = event;
  ingressTail_ = (ingressTail_ + 1U) % config_.ingressCapacity;
  ++ingressCount_;
  ++stats_.ingressQueued;
  stats_.maxIngressDepth = std::max(stats_.maxIngressDepth, ingressCount_);
  return ScheduleDecision::Accepted;
}

uint32_t Scheduler::FlushIngressToScheduled(uint32_t maxEvents) {
  if (!initialized_)
    return 0;

  uint32_t processed = 0;
  while (ingressCount_ > 0 && processed < maxEvents) {
    NormalizedEvent event = ingress_[ingressHead_];
    ingressHead_ = (ingressHead_ + 1U) % config_.ingressCapacity;
    --ingressCount_;
    ScheduleInternal(event);
    ++processed;
  }
  return processed;
}

ScheduleDecision Scheduler::ScheduleDirect(const NormalizedEvent &event) {
  return ScheduleInternal(event);
}

size_t Scheduler::DrainScheduledWindow(int64_t cursorSample, int64_t blockEndSample,
                                       int64_t windowEndSample,
                                       NormalizedEvent *outEvents,
                                       size_t outCapacity,
                                       int64_t *renderUntilSample) {
  if (!initialized_) {
    if (renderUntilSample)
      *renderUntilSample = blockEndSample;
    return 0;
  }

  size_t drained = 0;
  int64_t latestSample = cursorSample;

  while (heapCount_ > 0 && drained < outCapacity) {
    const ScheduledSlot &slot = slots_[heap_[0]];
    if (!slot.active || slot.event.targetSample >= windowEndSample)
      break;

    uint32_t slotIndex = HeapPop();
    NormalizedEvent event = slots_[slotIndex].event;

    if (EventUsesKey(event.kind))
      RemoveTransitionSlot(event.channel, event.note, slotIndex);

    ReleaseSlot(slotIndex);

    outEvents[drained++] = event;
    ++stats_.drainedEvents;
    if (event.targetSample > latestSample)
      latestSample = event.targetSample;
  }

  if (renderUntilSample) {
    if (drained > 0) {
      *renderUntilSample = std::min(latestSample, blockEndSample);
    } else if (heapCount_ > 0) {
      const int64_t nextSample = slots_[heap_[0]].event.targetSample;
      *renderUntilSample =
          std::max(cursorSample, std::min(nextSample, blockEndSample));
    } else {
      *renderUntilSample = blockEndSample;
    }
  }

  return drained;
}

uint32_t Scheduler::GetScheduledCount() const { return heapCount_; }

uint32_t Scheduler::GetIngressCount() const { return ingressCount_; }

const SchedulerStats &Scheduler::GetStats() const { return stats_; }

const ScheduledKeyState &Scheduler::GetKeyState(uint32_t channel,
                                                uint32_t note) const {
  return keyStates_[channel][note];
}

uint32_t Scheduler::GetTransitionQueueDepth(uint32_t channel,
                                            uint32_t note) const {
  return transitionQueues_[channel][note].count;
}

ScheduleDecision Scheduler::ScheduleInternal(const NormalizedEvent &event) {
  if (!initialized_)
    return ScheduleDecision::Dropped;

  bool replacedExisting = false;

  if (EventUsesKey(event.kind) && !ValidateKeyEvent(event)) {
    ++stats_.scheduledDropped;
    return ScheduleDecision::Dropped;
  }

  const int duplicateSlot = FindDuplicateTransitionSlot(event);
  if (duplicateSlot >= 0) {
    ++stats_.coalescedEvents;
    return ScheduleDecision::Coalesced;
  }

  if (EventUsesKey(event.kind)) {
    TransitionQueue &queue = transitionQueues_[event.channel][event.note];
    if (queue.count >= kTransitionQueueCapacity) {
      const int replacementSlot = FindOverflowReplacementSlot(event);
      if (replacementSlot < 0) {
        ++stats_.scheduledDropped;
        ++stats_.queueOverflowDrops;
        return ScheduleDecision::Dropped;
      }

      const NormalizedEvent &replaced = slots_[(uint32_t)replacementSlot].event;
      RemoveTransitionSlot(replaced.channel, replaced.note,
                           (uint32_t)replacementSlot);
      HeapRemove((uint32_t)replacementSlot);
      ReleaseSlot((uint32_t)replacementSlot);
      ++stats_.replacedEvents;
      replacedExisting = true;
    }
  }

  if (freeCount_ == 0) {
    ++stats_.scheduledDropped;
    return ScheduleDecision::Dropped;
  }

  const uint32_t slotIndex = AllocateSlot();
  ScheduledSlot &slot = slots_[slotIndex];
  slot.active = true;
  slot.eventId = nextEventId_++;
  slot.event = event;

  HeapPush(slotIndex);
  ++stats_.scheduledQueued;
  stats_.maxScheduledDepth = std::max(stats_.maxScheduledDepth, heapCount_);

  if (EventUsesKey(event.kind)) {
    TransitionQueue &queue = transitionQueues_[event.channel][event.note];
    TransitionEntry &entry = queue.entries[queue.count++];
    entry.slotIndex = slotIndex;
    entry.kind = event.kind;
    entry.velocity = event.velocity;
    entry.applyPriority = event.applyPriority;
    entry.sequence = event.sequence;
    entry.targetSample = event.targetSample;
    stats_.maxTransitionQueueDepth =
        std::max(stats_.maxTransitionQueueDepth, queue.count);
    RecomputeKeyState(event.channel, event.note);
  }

  return replacedExisting ? ScheduleDecision::ReplacedExisting
                          : ScheduleDecision::Accepted;
}

bool Scheduler::ValidateKeyEvent(const NormalizedEvent &event) const {
  return event.channel < kChannelCount && event.note < kNoteCount;
}

bool Scheduler::IsEarlier(uint32_t lhsSlot, uint32_t rhsSlot) const {
  const NormalizedEvent &lhs = slots_[lhsSlot].event;
  const NormalizedEvent &rhs = slots_[rhsSlot].event;

  if (lhs.targetSample != rhs.targetSample)
    return lhs.targetSample < rhs.targetSample;
  if (lhs.applyPriority != rhs.applyPriority)
    return lhs.applyPriority < rhs.applyPriority;
  return lhs.sequence < rhs.sequence;
}

void Scheduler::HeapSwap(uint32_t a, uint32_t b) {
  const uint32_t lhs = heap_[a];
  const uint32_t rhs = heap_[b];
  heap_[a] = rhs;
  heap_[b] = lhs;
  heapPositions_[lhs] = (int32_t)b;
  heapPositions_[rhs] = (int32_t)a;
}

void Scheduler::HeapSiftUp(uint32_t index) {
  while (index > 0) {
    const uint32_t parent = (index - 1U) / 2U;
    if (!IsEarlier(heap_[index], heap_[parent]))
      break;
    HeapSwap(index, parent);
    index = parent;
  }
}

void Scheduler::HeapSiftDown(uint32_t index) {
  while (true) {
    const uint32_t left = index * 2U + 1U;
    const uint32_t right = left + 1U;
    uint32_t smallest = index;

    if (left < heapCount_ && IsEarlier(heap_[left], heap_[smallest]))
      smallest = left;
    if (right < heapCount_ && IsEarlier(heap_[right], heap_[smallest]))
      smallest = right;
    if (smallest == index)
      break;

    HeapSwap(index, smallest);
    index = smallest;
  }
}

void Scheduler::HeapPush(uint32_t slotIndex) {
  heap_[heapCount_] = slotIndex;
  heapPositions_[slotIndex] = (int32_t)heapCount_;
  ++heapCount_;
  HeapSiftUp(heapCount_ - 1U);
}

uint32_t Scheduler::HeapPop() {
  const uint32_t slotIndex = heap_[0];
  HeapRemove(slotIndex);
  return slotIndex;
}

void Scheduler::HeapRemove(uint32_t slotIndex) {
  const int32_t position = heapPositions_[slotIndex];
  if (position < 0)
    return;

  const uint32_t index = (uint32_t)position;
  const uint32_t lastIndex = heapCount_ - 1U;
  HeapSwap(index, lastIndex);
  heapPositions_[heap_[lastIndex]] = (int32_t)lastIndex;
  heapPositions_[slotIndex] = -1;
  --heapCount_;

  if (index < heapCount_) {
    HeapSiftUp(index);
    HeapSiftDown(index);
  }
}

uint32_t Scheduler::AllocateSlot() {
  return freeList_[--freeCount_];
}

void Scheduler::ReleaseSlot(uint32_t slotIndex) {
  slots_[slotIndex] = ScheduledSlot();
  heapPositions_[slotIndex] = -1;
  freeList_[freeCount_++] = slotIndex;
}

void Scheduler::RemoveTransitionSlot(uint8_t channel, uint8_t note,
                                     uint32_t slotIndex) {
  TransitionQueue &queue = transitionQueues_[channel][note];
  for (uint32_t i = 0; i < queue.count; ++i) {
    if (queue.entries[i].slotIndex != slotIndex)
      continue;

    for (uint32_t j = i + 1; j < queue.count; ++j)
      queue.entries[j - 1U] = queue.entries[j];
    --queue.count;
    break;
  }
  RecomputeKeyState(channel, note);
}

void Scheduler::RecomputeKeyState(uint8_t channel, uint8_t note) {
  ScheduledKeyState &state = keyStates_[channel][note];
  const TransitionQueue &queue = transitionQueues_[channel][note];

  state.pendingNoteOnCount = 0;
  state.pendingNoteOffCount = 0;
  state.lastScheduledState = ScheduledKeyStateValue::Unknown;

  for (uint32_t i = 0; i < queue.count; ++i) {
    if (EventIsNoteOn(queue.entries[i].kind))
      ++state.pendingNoteOnCount;
    else if (EventIsNoteOff(queue.entries[i].kind))
      ++state.pendingNoteOffCount;
  }

  if (queue.count > 0)
    state.lastScheduledState = EventToScheduledState(queue.entries[queue.count - 1U].kind);
}

int Scheduler::FindDuplicateTransitionSlot(const NormalizedEvent &event) const {
  if (!EventUsesKey(event.kind))
    return -1;

  const TransitionQueue &queue = transitionQueues_[event.channel][event.note];
  for (uint32_t i = 0; i < queue.count; ++i) {
    const TransitionEntry &entry = queue.entries[i];
    if (entry.kind == event.kind && entry.targetSample == event.targetSample &&
        entry.applyPriority == event.applyPriority) {
      return (int)entry.slotIndex;
    }
  }
  return -1;
}

int Scheduler::FindOverflowReplacementSlot(const NormalizedEvent &event) const {
  if (!EventUsesKey(event.kind))
    return -1;

  const TransitionQueue &queue = transitionQueues_[event.channel][event.note];
  int candidateIndex = -1;
  uint8_t candidateVelocity = 0;
  int64_t candidateTarget = 0;

  for (uint32_t i = 0; i < queue.count; ++i) {
    const TransitionEntry &entry = queue.entries[i];
    if (!EventIsNoteOn(entry.kind))
      continue;

    if (candidateIndex < 0 || entry.velocity < candidateVelocity ||
        (entry.velocity == candidateVelocity &&
         entry.targetSample > candidateTarget)) {
      candidateIndex = (int)entry.slotIndex;
      candidateVelocity = entry.velocity;
      candidateTarget = entry.targetSample;
    }
  }

  if (candidateIndex < 0)
    return -1;

  if (EventIsNoteOn(event.kind) && candidateVelocity > event.velocity &&
      candidateTarget <= event.targetSample) {
    return -1;
  }

  return candidateIndex;
}

} // namespace virtuallysuper
