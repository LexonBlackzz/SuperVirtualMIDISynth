#ifndef VIRTUALLYSUPER_TELEMETRY_SHARED_H
#define VIRTUALLYSUPER_TELEMETRY_SHARED_H

#include "VirtuallySuperTypes.h"

namespace virtuallysuper {

struct TelemetrySharedState {
  uint32_t sequence;
  TelemetrySnapshot latest;

  TelemetrySharedState() : sequence(0), latest() {}
};

} // namespace virtuallysuper

#endif
