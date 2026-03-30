#ifndef VIRTUALLYSUPER_ENGINE_H
#define VIRTUALLYSUPER_ENGINE_H

#include "VirtuallySuperScheduler.h"

namespace virtuallysuper {

class EnginePrototype {
public:
  EnginePrototype();

  bool Initialize(const SchedulerConfig &schedulerConfig);
  void Reset();

  ScheduleDecision SubmitEvent(const NormalizedEvent &event);
  uint32_t FlushPendingIngress(uint32_t maxEvents);
  size_t DrainWindow(int64_t cursorSample, int64_t blockEndSample,
                     int64_t windowEndSample, NormalizedEvent *outEvents,
                     size_t outCapacity, int64_t *renderUntilSample);

  const Scheduler &GetScheduler() const;
  Scheduler &GetScheduler();

private:
  Scheduler scheduler_;
  bool initialized_;
};

} // namespace virtuallysuper

#endif
