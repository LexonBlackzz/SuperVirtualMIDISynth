#include "VirtuallySuperEngine.h"

namespace virtuallysuper {

EnginePrototype::EnginePrototype()
    : scheduler_(), scene_(), exact_(), grouped_(), density_(),
      initialized_(false), drainBatch_() {}

bool EnginePrototype::Initialize(const EngineConfig &config) {
  initialized_ =
      scheduler_.Initialize(config.scheduler) && exact_.Initialize(config.exact) &&
      grouped_.Initialize(config.grouped) && density_.Initialize(config.density);
  return initialized_;
}

bool EnginePrototype::Initialize(const SchedulerConfig &schedulerConfig) {
  EngineConfig config;
  config.scheduler = schedulerConfig;
  return Initialize(config);
}

void EnginePrototype::Reset() {
  if (!initialized_)
    return;
  scheduler_.Reset();
  scene_.Reset();
  exact_.Reset();
  grouped_.Reset();
  density_.Reset();
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

size_t EnginePrototype::ApplyScheduledWindow(int64_t cursorSample,
                                             int64_t blockEndSample,
                                             int64_t windowEndSample,
                                             int64_t *renderUntilSample) {
  if (!initialized_) {
    if (renderUntilSample)
      *renderUntilSample = blockEndSample;
    return 0;
  }

  size_t totalApplied = 0;
  int64_t localRenderUntil = blockEndSample;
  scene_.BeginWindow();
  grouped_.BeginWindow();
  density_.BeginWindow();

  while (true) {
    const size_t drained = scheduler_.DrainScheduledWindow(
        cursorSample, blockEndSample, windowEndSample, drainBatch_,
        kDrainBatchCapacity, &localRenderUntil);

    if (drained == 0) {
      if (renderUntilSample)
        *renderUntilSample = localRenderUntil;
      return totalApplied;
    }

    for (size_t i = 0; i < drained; ++i) {
      const virtuallysuper::SceneAction action =
          scene_.CompileEvent(drainBatch_[i], exact_.GetStats());

      switch (action.kind) {
      case virtuallysuper::SceneActionKind::SpawnExactVoice:
      case virtuallysuper::SceneActionKind::ReleaseExactVoice:
      case virtuallysuper::SceneActionKind::ResetScene:
        exact_.ApplyEvent(action.event);
        break;
      default:
        break;
      }

      if (action.observeGrouped != 0)
        grouped_.AccumulateEvent(action.event);
      if (action.observeDensity != 0)
        density_.AccumulateEvent(action.event);
    }

    totalApplied += drained;
    cursorSample = localRenderUntil;
  }
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

const ExactSystem &EnginePrototype::GetExactSystem() const { return exact_; }

ExactSystem &EnginePrototype::GetExactSystem() { return exact_; }

const GroupedSystem &EnginePrototype::GetGroupedSystem() const {
  return grouped_;
}

GroupedSystem &EnginePrototype::GetGroupedSystem() { return grouped_; }

const DensitySystem &EnginePrototype::GetDensitySystem() const {
  return density_;
}

DensitySystem &EnginePrototype::GetDensitySystem() { return density_; }

const SceneCompiler &EnginePrototype::GetSceneCompiler() const { return scene_; }

SceneCompiler &EnginePrototype::GetSceneCompiler() { return scene_; }

} // namespace virtuallysuper
