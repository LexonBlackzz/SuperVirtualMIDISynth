#ifndef VIRTUALLYSUPER_TELEMETRY_H
#define VIRTUALLYSUPER_TELEMETRY_H

#include "VirtuallySuperTelemetryShared.h"

namespace virtuallysuper {

class TelemetryPublisher {
public:
  TelemetryPublisher();

  void Reset();
  void Publish(const SchedulerStats &schedulerStats, const SceneStats &sceneStats,
               const ExactStats &exactStats, const GroupedStats &groupedStats,
               const DensityStats &densityStats, uint32_t schedulerQueuedEvents,
               uint32_t lastAppliedEvents);

  const TelemetrySnapshot &GetLatestSnapshot() const;
  const TelemetrySharedState &GetSharedState() const;

private:
  TelemetrySharedState sharedState_;
};

} // namespace virtuallysuper

#endif
