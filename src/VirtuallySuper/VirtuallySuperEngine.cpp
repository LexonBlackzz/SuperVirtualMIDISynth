#include "VirtuallySuperEngine.h"

namespace virtuallysuper {

EnginePrototype::EnginePrototype() : scheduler_(), initialized_(false) {}

bool EnginePrototype::Initialize(const SchedulerConfig &schedulerConfig) {
  initialized_ = scheduler_.Initialize(schedulerConfig);
  return initialized_;
}

void EnginePrototype::Reset() {
  if (!initialized_)
    return;
  scheduler_.Reset();
}

ScheduleDecision EnginePrototype::SubmitEvent(const NormalizedEvent &event) {
  if (!initialized_)
    return ScheduleDecision::Dropped;
  return scheduler_.EnqueueIngressEvent(event);
}

uint32_t EnginePrototype::FlushPendingIngress(uint32_t maxEvents) {
  if (!initialized_)
    return 0;
  return scheduler_.FlushIngressToScheduled(maxEvents);
}

size_t EnginePrototype::DrainWindow(int64_t cursorSample, int64_t blockEndSample,
                                    int64_t windowEndSample,
                                    NormalizedEvent *outEvents,
                                    size_t outCapacity,
                                    int64_t *renderUntilSample) {
  if (!initialized_) {
    if (renderUntilSample)
      *renderUntilSample = blockEndSample;
    return 0;
  }
  return scheduler_.DrainScheduledWindow(cursorSample, blockEndSample,
                                         windowEndSample, outEvents,
                                         outCapacity, renderUntilSample);
}

const Scheduler &EnginePrototype::GetScheduler() const { return scheduler_; }

Scheduler &EnginePrototype::GetScheduler() { return scheduler_; }

} // namespace virtuallysuper
