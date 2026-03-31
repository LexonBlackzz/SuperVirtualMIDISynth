#ifndef VIRTUALLYSUPER_ENGINE_H
#define VIRTUALLYSUPER_ENGINE_H

#include "VirtuallySuperDensity.h"
#include "VirtuallySuperExact.h"
#include "VirtuallySuperGrouped.h"
#include "VirtuallySuperOverload.h"
#include "VirtuallySuperRender.h"
#include "VirtuallySuperScheduler.h"
#include "VirtuallySuperScene.h"
#include "VirtuallySuperTelemetry.h"

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
  void RenderBlock(float *output, int numFrames, int sampleRate);
  bool CanIdleFastPath() const;

  const Scheduler &GetScheduler() const;
  Scheduler &GetScheduler();
  const ExactSystem &GetExactSystem() const;
  ExactSystem &GetExactSystem();
  const GroupedSystem &GetGroupedSystem() const;
  GroupedSystem &GetGroupedSystem();
  const DensitySystem &GetDensitySystem() const;
  DensitySystem &GetDensitySystem();
  const OverloadController &GetOverloadController() const;
  OverloadController &GetOverloadController();
  const SceneCompiler &GetSceneCompiler() const;
  SceneCompiler &GetSceneCompiler();
  const TelemetryPublisher &GetTelemetryPublisher() const;
  TelemetryPublisher &GetTelemetryPublisher();
  const TelemetrySnapshot &GetLatestTelemetrySnapshot() const;
  const RenderStats &GetLatestRenderStats() const;

private:
  Scheduler scheduler_;
  SceneCompiler scene_;
  ExactSystem exact_;
  GroupedSystem grouped_;
  DensitySystem density_;
  OverloadController overload_;
  RenderSystem render_;
  TelemetryPublisher telemetry_;
  bool initialized_;
  NormalizedEvent drainBatch_[kDrainBatchCapacity];
};

} // namespace virtuallysuper

#endif
