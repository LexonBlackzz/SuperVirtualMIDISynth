#ifndef VIRTUALLYSUPER_TYPES_H
#define VIRTUALLYSUPER_TYPES_H

#include <stddef.h>
#include <stdint.h>

namespace virtuallysuper {

static const uint32_t kChannelCount = 16;
static const uint32_t kNoteCount = 128;

#ifdef SVMS_LEGACY_XP
static const uint32_t kTransitionQueueCapacity = 4;
#else
static const uint32_t kTransitionQueueCapacity = 8;
#endif

static const uint32_t kDefaultIngressCapacity = 4096;
static const uint32_t kDefaultScheduledCapacity = 16384;
static const int64_t kDefaultCoalesceWindowSamples = 8;
static const uint32_t kDefaultExactVoiceCapacity = 256;
static const uint32_t kDefaultGroupedCapacity = 128;
static const uint32_t kDefaultDensityCapacity = 64;
static const uint32_t kDrainBatchCapacity = 256;
static const uint32_t kInvalidVoiceHandle = 0xFFFFFFFFu;

enum class EventKind : uint8_t {
  Invalid = 0,
  NoteOn,
  NoteOff,
  ProgramChange,
  ControlChange,
  PitchBend,
  Reset
};

enum class ScheduleDecision : uint8_t {
  Accepted = 0,
  Coalesced,
  ReplacedExisting,
  Dropped
};

enum class ScheduledKeyStateValue : uint8_t {
  Unknown = 0,
  Off,
  On
};

enum class ExactLifecycleState : uint8_t {
  Free = 0,
  Active,
  Released
};

enum class ExactQueueClass : uint8_t {
  None = 0,
  QuietActive,
  LoudActive,
  QuietRelease,
  LoudRelease
};

struct NormalizedEvent {
  EventKind kind;
  uint8_t channel;
  uint8_t note;
  uint8_t value;
  uint8_t velocity;
  uint8_t applyPriority;
  uint8_t flags;
  uint16_t reserved;
  uint32_t sequence;
  uint32_t sourceTrack;
  int64_t targetSample;

  NormalizedEvent()
      : kind(EventKind::Invalid), channel(0), note(0), value(0), velocity(0),
        applyPriority(0), flags(0), reserved(0), sequence(0), sourceTrack(0),
        targetSample(0) {}
};

struct TransitionEntry {
  uint32_t slotIndex;
  EventKind kind;
  uint8_t velocity;
  uint8_t applyPriority;
  uint32_t sequence;
  int64_t targetSample;

  TransitionEntry()
      : slotIndex(0), kind(EventKind::Invalid), velocity(0), applyPriority(0),
        sequence(0), targetSample(0) {}
};

struct TransitionQueue {
  uint32_t count;
  TransitionEntry entries[kTransitionQueueCapacity];

  TransitionQueue() : count(0) {}
};

struct ScheduledKeyState {
  uint32_t pendingNoteOnCount;
  uint32_t pendingNoteOffCount;
  uint32_t soundingGenerations;
  ScheduledKeyStateValue lastScheduledState;

  ScheduledKeyState()
      : pendingNoteOnCount(0), pendingNoteOffCount(0), soundingGenerations(0),
        lastScheduledState(ScheduledKeyStateValue::Unknown) {}
};

struct SchedulerConfig {
  uint32_t ingressCapacity;
  uint32_t scheduledCapacity;
  int64_t coalesceWindowSamples;

  SchedulerConfig()
      : ingressCapacity(kDefaultIngressCapacity),
        scheduledCapacity(kDefaultScheduledCapacity),
        coalesceWindowSamples(kDefaultCoalesceWindowSamples) {}
};

struct ExactConfig {
  uint32_t maxVoices;
  uint8_t quietVelocityThreshold;

  ExactConfig() : maxVoices(kDefaultExactVoiceCapacity),
                  quietVelocityThreshold(60) {}
};

struct SchedulerStats {
  uint32_t ingressQueued;
  uint32_t ingressDropped;
  uint32_t scheduledQueued;
  uint32_t scheduledDropped;
  uint32_t coalescedEvents;
  uint32_t replacedEvents;
  uint32_t drainedEvents;
  uint32_t queueOverflowDrops;
  uint32_t maxScheduledDepth;
  uint32_t maxIngressDepth;
  uint32_t maxTransitionQueueDepth;

  SchedulerStats()
      : ingressQueued(0), ingressDropped(0), scheduledQueued(0),
        scheduledDropped(0), coalescedEvents(0), replacedEvents(0),
        drainedEvents(0), queueOverflowDrops(0), maxScheduledDepth(0),
        maxIngressDepth(0), maxTransitionQueueDepth(0) {}
};

struct ExactVoice {
  uint32_t voiceId;
  uint32_t generation;
  uint32_t startSequence;
  ExactLifecycleState state;
  ExactQueueClass queueClass;
  uint8_t channel;
  uint8_t note;
  uint8_t velocity;
  uint8_t layerTemplateId;
  uint8_t protectedAttack;
  uint8_t releaseShortened;
  uint16_t reserved;
  uint32_t nextSameKey;
  uint32_t prevSameKey;
  uint32_t nextQueue;
  uint32_t prevQueue;

  ExactVoice()
      : voiceId(0), generation(0), startSequence(0),
        state(ExactLifecycleState::Free), queueClass(ExactQueueClass::None),
        channel(0), note(0), velocity(0), layerTemplateId(0),
        protectedAttack(0), releaseShortened(0), reserved(0),
        nextSameKey(kInvalidVoiceHandle), prevSameKey(kInvalidVoiceHandle),
        nextQueue(kInvalidVoiceHandle), prevQueue(kInvalidVoiceHandle) {}
};

struct ExactStats {
  uint32_t activeVoices;
  uint32_t releasedVoices;
  uint32_t peakActiveVoices;
  uint32_t noteOnsApplied;
  uint32_t noteOffsApplied;
  uint32_t steals;
  uint32_t quietSteals;
  uint32_t releaseSteals;

  ExactStats()
      : activeVoices(0), releasedVoices(0), peakActiveVoices(0),
        noteOnsApplied(0), noteOffsApplied(0), steals(0), quietSteals(0),
        releaseSteals(0) {}
};

struct GroupedConfig {
  uint32_t maxGroups;
  uint8_t pitchBandSemitones;
  uint8_t reserved0;
  uint16_t reserved1;
  int64_t timingBucketSamples;

  GroupedConfig()
      : maxGroups(kDefaultGroupedCapacity), pitchBandSemitones(6),
        reserved0(0), reserved1(0), timingBucketSamples(32) {}
};

struct DensityConfig {
  uint32_t maxObjects;
  uint8_t pitchBandSemitones;
  uint8_t activationThreshold;
  uint16_t reserved0;
  uint32_t saturationK;
  int64_t timingBucketSamples;

  DensityConfig()
      : maxObjects(kDefaultDensityCapacity), pitchBandSemitones(12),
        activationThreshold(4), reserved0(0), saturationK(4),
        timingBucketSamples(32) {}
};

struct EngineConfig {
  SchedulerConfig scheduler;
  ExactConfig exact;
  GroupedConfig grouped;
  DensityConfig density;
};

struct GroupedObject {
  uint8_t active;
  uint8_t channel;
  uint8_t pitchBandId;
  uint8_t layerTemplateId;
  uint16_t sampleFamilyId;
  uint16_t reserved;
  uint32_t groupId;
  uint32_t timingBucketId;
  uint32_t representedNoteCount;
  uint32_t representedLayerCount;
  uint32_t noteOnCount;
  int64_t lastTargetSample;

  GroupedObject()
      : active(0), channel(0), pitchBandId(0), layerTemplateId(0),
        sampleFamilyId(0), reserved(0), groupId(0), timingBucketId(0),
        representedNoteCount(0), representedLayerCount(0), noteOnCount(0),
        lastTargetSample(0) {}
};

struct GroupedStats {
  uint32_t activeGroups;
  uint32_t peakActiveGroups;
  uint32_t noteOnsAccumulated;
  uint32_t droppedNoteOns;

  GroupedStats()
      : activeGroups(0), peakActiveGroups(0), noteOnsAccumulated(0),
        droppedNoteOns(0) {}
};

struct DensityObject {
  uint8_t active;
  uint8_t channel;
  uint8_t pitchBandId;
  uint8_t activationThreshold;
  uint16_t sampleFamilyId;
  uint16_t reserved;
  uint32_t densityId;
  uint32_t timingBucketId;
  uint32_t representedNoteCount;
  uint32_t representedLayerCount;
  uint32_t grainJitterSeed;
  float energyLevel;
  float saturatedGain;

  DensityObject()
      : active(0), channel(0), pitchBandId(0), activationThreshold(0),
        sampleFamilyId(0), reserved(0), densityId(0), timingBucketId(0),
        representedNoteCount(0), representedLayerCount(0), grainJitterSeed(0),
        energyLevel(0.0f), saturatedGain(0.0f) {}
};

struct DensityStats {
  uint32_t activeObjects;
  uint32_t peakActiveObjects;
  uint32_t noteOnsAccumulated;
  uint32_t promotedClouds;
  uint32_t droppedNoteOns;

  DensityStats()
      : activeObjects(0), peakActiveObjects(0), noteOnsAccumulated(0),
        promotedClouds(0), droppedNoteOns(0) {}
};

inline bool EventUsesKey(EventKind kind) {
  return kind == EventKind::NoteOn || kind == EventKind::NoteOff;
}

inline bool EventIsNoteOn(EventKind kind) { return kind == EventKind::NoteOn; }

inline bool EventIsNoteOff(EventKind kind) { return kind == EventKind::NoteOff; }

inline ScheduledKeyStateValue EventToScheduledState(EventKind kind) {
  if (kind == EventKind::NoteOn)
    return ScheduledKeyStateValue::On;
  if (kind == EventKind::NoteOff)
    return ScheduledKeyStateValue::Off;
  return ScheduledKeyStateValue::Unknown;
}

} // namespace virtuallysuper

#endif
