#include "VirtuallySuperTelemetry.h"

namespace virtuallysuper {

TelemetryPublisher::TelemetryPublisher() : sharedState_() {}

void TelemetryPublisher::Reset() { sharedState_ = TelemetrySharedState(); }

void TelemetryPublisher::Publish(const SchedulerStats &schedulerStats,
                                 const SceneStats &sceneStats,
                                 const ExactStats &exactStats,
                                 const GroupedStats &groupedStats,
                                 const DensityStats &densityStats,
                                 uint32_t schedulerQueuedEvents,
                                 uint32_t lastAppliedEvents,
                                 PressureLevel pressureLevel) {
  TelemetrySnapshot snapshot;
  snapshot.exactVoices = exactStats.activeVoices;
  snapshot.releasedExactVoices = exactStats.releasedVoices;
  snapshot.groupedObjects = groupedStats.activeGroups;
  snapshot.densityObjects = densityStats.activeObjects;
  
  // Calculate proper voice equivalent:
  // - Each exact voice = 1 voice
  // - Each grouped object represents multiple notes, weighted by representation
  // - Each density object represents a statistical cloud, weighted by activation
  const uint32_t groupedVoiceEq = groupedStats.activeGroups > 0
      ? (groupedStats.noteOnsAccumulated + groupedStats.activeGroups - 1) / groupedStats.activeGroups
      : 0;
  const uint32_t densityVoiceEq = densityStats.activeObjects > 0
      ? (densityStats.noteOnsAccumulated + densityStats.activeObjects - 1) / densityStats.activeObjects
      : 0;
  
  snapshot.voiceEquivalent = exactStats.activeVoices + 
                             groupedVoiceEq * groupedStats.activeGroups +
                             densityVoiceEq * densityStats.activeObjects;
  
  snapshot.schedulerQueuedEvents = schedulerQueuedEvents;
  snapshot.schedulerMaxTransitionQueueDepth =
      schedulerStats.maxTransitionQueueDepth;
  snapshot.schedulerCoalescedEvents = schedulerStats.coalescedEvents;
  snapshot.sceneExactActions = sceneStats.exactActions;
  snapshot.sceneGroupedObservations = sceneStats.groupedObservations;
  snapshot.sceneDensityObservations = sceneStats.densityObservations;
  snapshot.exactSteals = exactStats.steals;
  snapshot.groupedAccumulatedNotes = groupedStats.noteOnsAccumulated;
  snapshot.densityAccumulatedNotes = densityStats.noteOnsAccumulated;
  snapshot.densityPromotedClouds = densityStats.promotedClouds;
  snapshot.lastAppliedEvents = lastAppliedEvents;
  snapshot.overloadPressureLevel = (uint32_t)pressureLevel;

  ++sharedState_.sequence;
  sharedState_.latest = snapshot;
  ++sharedState_.sequence;
}

const TelemetrySnapshot &TelemetryPublisher::GetLatestSnapshot() const {
  return sharedState_.latest;
}

const TelemetrySharedState &TelemetryPublisher::GetSharedState() const {
  return sharedState_;
}

} // namespace virtuallysuper
