#ifndef VIRTUALLYSUPER_ENGINE_H
#define VIRTUALLYSUPER_ENGINE_H

#include "VirtuallySuperDensity.h"
#include "VirtuallySuperExact.h"
#include "VirtuallySuperGrouped.h"
#include "VirtuallySuperScheduler.h"

namespace virtuallysuper {

class EnginePrototype {
public:
  EnginePrototype();

  bool Initialize(const EngineConfig &config);
  bool Initialize(const SchedulerConfig &schedulerConfig);
  void Reset();

  ScheduleDecision SubmitEvent(const NormalizedEvent &event);
  uint32_t FlushPendingIngress(uint32_t maxEvents);
  size_t ApplyScheduledWindow(int64_t cursorSample, int64_t blockEndSample,
                              int64_t windowEndSample,
                              int64_t *renderUntilSample);
  size_t DrainWindow(int64_t cursorSample, int64_t blockEndSample,
                     int64_t windowEndSample, NormalizedEvent *outEvents,
                     size_t outCapacity, int64_t *renderUntilSample);

  const Scheduler &GetScheduler() const;
  Scheduler &GetScheduler();
  const ExactSystem &GetExactSystem() const;
  ExactSystem &GetExactSystem();
  const GroupedSystem &GetGroupedSystem() const;
  GroupedSystem &GetGroupedSystem();
  const DensitySystem &GetDensitySystem() const;
  DensitySystem &GetDensitySystem();

private:
  Scheduler scheduler_;
  ExactSystem exact_;
  GroupedSystem grouped_;
  DensitySystem density_;
  bool initialized_;
  NormalizedEvent drainBatch_[kDrainBatchCapacity];
};

} // namespace virtuallysuper

#endif
