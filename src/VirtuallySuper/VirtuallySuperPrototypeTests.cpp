#include "VirtuallySuperEngine.h"
#include "VirtuallySuperGrouped.h"

#include <stdio.h>

using namespace virtuallysuper;

static NormalizedEvent MakeEvent(EventKind kind, uint8_t channel, uint8_t note,
                                 uint8_t velocity, int64_t targetSample,
                                 uint32_t sequence,
                                 uint8_t applyPriority = 0) {
  NormalizedEvent event;
  event.kind = kind;
  event.channel = channel;
  event.note = note;
  event.velocity = velocity;
  event.targetSample = targetSample;
  event.sequence = sequence;
  event.applyPriority = applyPriority;
  return event;
}

static bool TestSchedulerOrderingAndCoalescing() {
  Scheduler scheduler;
  SchedulerConfig config;
  config.ingressCapacity = 16;
  config.scheduledCapacity = 16;
  if (!scheduler.Initialize(config))
    return false;

  if (scheduler.ScheduleDirect(
          MakeEvent(EventKind::NoteOn, 0, 60, 100, 20, 2)) !=
      ScheduleDecision::Accepted)
    return false;
  if (scheduler.ScheduleDirect(
          MakeEvent(EventKind::NoteOn, 0, 60, 110, 20, 3)) !=
      ScheduleDecision::Coalesced)
    return false;
  if (scheduler.ScheduleDirect(
          MakeEvent(EventKind::NoteOff, 0, 60, 0, 10, 1)) !=
      ScheduleDecision::Accepted)
    return false;

  NormalizedEvent drained[4];
  int64_t renderUntil = 0;
  const size_t count =
      scheduler.DrainScheduledWindow(0, 100, 100, drained, 4, &renderUntil);
  if (count != 2)
    return false;
  if (drained[0].kind != EventKind::NoteOff || drained[0].targetSample != 10)
    return false;
  if (drained[1].kind != EventKind::NoteOn || drained[1].targetSample != 20)
    return false;
  if (scheduler.GetStats().coalescedEvents != 1)
    return false;
  return true;
}

static bool TestEngineApplyWindowAndNoteOff() {
  EnginePrototype engine;
  EngineConfig config;
  config.scheduler.ingressCapacity = 16;
  config.scheduler.scheduledCapacity = 16;
  config.exact.maxVoices = 4;
  if (!engine.Initialize(config))
    return false;

  if (engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 60, 100, 10, 1)) ==
      ScheduleDecision::Dropped)
    return false;
  if (engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 61, 90, 12, 2)) ==
      ScheduleDecision::Dropped)
    return false;
  engine.FlushPendingIngress(16);

  int64_t renderUntil = 0;
  if (engine.ApplyScheduledWindow(0, 100, 100, &renderUntil) != 2)
    return false;
  if (engine.GetExactSystem().GetActiveVoiceCount() != 2)
    return false;

  engine.SubmitEvent(MakeEvent(EventKind::NoteOff, 0, 60, 0, 20, 3));
  engine.FlushPendingIngress(16);
  if (engine.ApplyScheduledWindow(12, 100, 100, &renderUntil) != 1)
    return false;
  if (engine.GetExactSystem().GetActiveVoiceCount() != 1)
    return false;
  if (engine.GetExactSystem().GetReleasedVoiceCount() != 1)
    return false;
  if (engine.GetExactSystem().GetStats().noteOffsApplied != 1)
    return false;
  return true;
}

static bool TestQuietReleaseSteal() {
  EnginePrototype engine;
  EngineConfig config;
  config.scheduler.ingressCapacity = 16;
  config.scheduler.scheduledCapacity = 16;
  config.exact.maxVoices = 2;
  config.exact.quietVelocityThreshold = 60;
  if (!engine.Initialize(config))
    return false;

  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 60, 100, 0, 1));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 61, 20, 1, 2));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOff, 0, 61, 0, 2, 3));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 62, 120, 3, 4));
  engine.FlushPendingIngress(16);

  int64_t renderUntil = 0;
  if (engine.ApplyScheduledWindow(0, 100, 100, &renderUntil) != 4)
    return false;
  if (engine.GetExactSystem().GetActiveVoiceCount() != 2)
    return false;
  if (engine.GetExactSystem().GetReleasedVoiceCount() != 0)
    return false;
  if (engine.GetExactSystem().GetStats().steals != 1)
    return false;
  if (engine.GetExactSystem().GetStats().quietSteals != 1)
    return false;
  if (engine.GetExactSystem().GetStats().releaseSteals != 1)
    return false;
  if (engine.GetExactSystem().GetKeyHead(0, 61) != kInvalidVoiceHandle)
    return false;
  return true;
}

static bool TestGroupedPrototypeBuckets() {
  GroupedSystem grouped;
  GroupedConfig config;
  config.maxGroups = 8;
  config.pitchBandSemitones = 12;
  config.timingBucketSamples = 16;
  if (!grouped.Initialize(config))
    return false;

  grouped.BeginWindow();
  if (!grouped.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 60, 90, 0, 1)))
    return false;
  if (!grouped.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 61, 80, 4, 2)))
    return false;
  if (!grouped.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 72, 70, 20, 3)))
    return false;

  if (grouped.GetActiveGroupCount() != 2)
    return false;
  if (grouped.GetStats().noteOnsAccumulated != 3)
    return false;
  return true;
}

int main() {
  const struct {
    const char *name;
    bool (*fn)();
  } tests[] = {
      {"scheduler ordering and coalescing", TestSchedulerOrderingAndCoalescing},
      {"engine apply window and note off", TestEngineApplyWindowAndNoteOff},
      {"quiet release steal", TestQuietReleaseSteal},
      {"grouped prototype buckets", TestGroupedPrototypeBuckets},
  };

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    if (!tests[i].fn()) {
      fprintf(stderr, "FAILED: %s\n", tests[i].name);
      return 1;
    }
  }

  printf("VirtuallySuper prototype tests passed.\n");
  return 0;
}
