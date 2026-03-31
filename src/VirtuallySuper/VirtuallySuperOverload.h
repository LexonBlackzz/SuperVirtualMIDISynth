#ifndef VIRTUALLYSUPER_OVERLOAD_H
#define VIRTUALLYSUPER_OVERLOAD_H

#include "VirtuallySuperTypes.h"

namespace virtuallysuper {

class OverloadController {
public:
  OverloadController();

  bool Initialize(const OverloadConfig &config, const ExactConfig &exactConfig,
                  const SchedulerConfig &schedulerConfig);
  void Reset();
  PressureLevel Evaluate(uint32_t currentScheduledCount,
                         const SchedulerStats &schedulerStats,
                         const ExactStats &exactStats,
                         const GroupedStats &groupedStats,
                         const DensityStats &densityStats) const;

private:
  OverloadConfig config_;
  bool initialized_;
};

} // namespace virtuallysuper

#endif
