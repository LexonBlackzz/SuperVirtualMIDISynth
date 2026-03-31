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
static const uint32_t kDefaultRenderTileFrames = 128;
static const uint32_t kMaxRenderTileFrames = 256;
static const uint32_t kInvalidVoiceHandle = 0xFFFFFFFFu;
static const uint16_t kInvalidSoundFontIndex = 0xFFFFu;

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

enum class SceneActionKind : uint8_t {
  None = 0,
  SpawnExactVoice,
  ReleaseExactVoice,
  ResetScene
};

enum class PressureLevel : uint8_t {
  Normal = 0,
  Soft,
  Hard,
  Panic
};

struct NormalizedEvent {
  EventKind kind;
  uint8_t channel;
  uint8_t note;
  uint8_t value;
  uint8_t velocity;
  uint8_t mappedVelocity;
  uint8_t applyPriority;
  uint8_t flags;
  uint8_t reserved;
  uint32_t sequence;
  uint32_t sourceTrack;
  int64_t targetSample;

  NormalizedEvent()
      : kind(EventKind::Invalid), channel(0), note(0), value(0), velocity(0),
        mappedVelocity(0), applyPriority(0), flags(0), reserved(0),
        sequence(0), sourceTrack(0), targetSample(0) {}
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
  uint32_t sampleRate;
  uint8_t quietVelocityThreshold;
  uint8_t reserved0[3];

  ExactConfig()
      : maxVoices(kDefaultExactVoiceCapacity), sampleRate(44100),
        quietVelocityThreshold(60), reserved0() {}
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
  uint8_t mappedVelocity;
  uint8_t layerTemplateId;
  uint8_t protectedAttack;
  uint8_t releaseShortened;
  uint8_t sampleBacked;
  uint8_t heldBySustain;
  uint16_t regionIndex;
  uint16_t sampleIndex;
  uint32_t nextSameKey;
  uint32_t prevSameKey;
  uint32_t nextQueue;
  uint32_t prevQueue;
  const float *sampleData;
  uint32_t sampleStart;
  uint32_t sampleEnd;
  uint32_t loopStart;
  uint32_t loopEnd;
  uint8_t loopMode;
  uint8_t reserved;
  uint16_t attackSamplesRemaining;
  float phase;
  float frequencyHz;
  float phaseStep;
  float currentGain;
  float targetGain;
  float attackGainStep;
  float releaseDecay;
  float leftGain;
  float rightGain;

  ExactVoice()
      : voiceId(0), generation(0), startSequence(0),
        state(ExactLifecycleState::Free), queueClass(ExactQueueClass::None),
        channel(0), note(0), velocity(0), mappedVelocity(0), layerTemplateId(0),
        protectedAttack(0), releaseShortened(0), sampleBacked(0),
        heldBySustain(0), regionIndex(kInvalidSoundFontIndex),
        sampleIndex(kInvalidSoundFontIndex),
        nextSameKey(kInvalidVoiceHandle), prevSameKey(kInvalidVoiceHandle),
        nextQueue(kInvalidVoiceHandle), prevQueue(kInvalidVoiceHandle),
        sampleData(0), sampleStart(0), sampleEnd(0), loopStart(0), loopEnd(0),
        loopMode(0), reserved(0), attackSamplesRemaining(0), phase(0.0f),
        frequencyHz(0.0f), phaseStep(0.0f), currentGain(0.0f),
        targetGain(0.0f), attackGainStep(0.0f), releaseDecay(0.0f),
        leftGain(0.0f), rightGain(0.0f) {}
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
  uint32_t sampleRate;
  uint8_t pitchBandSemitones;
  uint8_t reserved0;
  uint16_t reserved1;
  int64_t timingBucketSamples;

  GroupedConfig()
      : maxGroups(kDefaultGroupedCapacity), sampleRate(44100),
        pitchBandSemitones(6), reserved0(0), reserved1(0),
        timingBucketSamples(32) {}
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

struct OverloadConfig {
  uint32_t softExactVoiceThreshold;
  uint32_t hardExactVoiceThreshold;
  uint32_t panicExactVoiceThreshold;
  uint32_t softSchedulerQueueThreshold;
  uint32_t hardSchedulerQueueThreshold;
  uint32_t panicSchedulerQueueThreshold;

  OverloadConfig()
      : softExactVoiceThreshold(0), hardExactVoiceThreshold(0),
        panicExactVoiceThreshold(0), softSchedulerQueueThreshold(0),
        hardSchedulerQueueThreshold(0), panicSchedulerQueueThreshold(0) {}
};

struct EngineConfig {
  SchedulerConfig scheduler;
  ExactConfig exact;
  GroupedConfig grouped;
  DensityConfig density;
  OverloadConfig overload;
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
  uint32_t activeListIndex;
  int64_t lastTargetSample;
  float phase;
  float phaseStep;
  float leftGain;
  float rightGain;
  float gain;

  GroupedObject()
      : active(0), channel(0), pitchBandId(0), layerTemplateId(0),
        sampleFamilyId(0), reserved(0), groupId(0), timingBucketId(0),
        representedNoteCount(0), representedLayerCount(0), noteOnCount(0),
        activeListIndex(kInvalidVoiceHandle), lastTargetSample(0), phase(0.0f),
        phaseStep(0.0f), leftGain(0.0f), rightGain(0.0f), gain(0.0f) {}
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
  uint32_t activeListIndex;
  uint32_t grainJitterSeed;
  float energyLevel;
  float saturatedGain;
  float leftGain;
  float rightGain;

  DensityObject()
      : active(0), channel(0), pitchBandId(0), activationThreshold(0),
        sampleFamilyId(0), reserved(0), densityId(0), timingBucketId(0),
        representedNoteCount(0), representedLayerCount(0),
        activeListIndex(kInvalidVoiceHandle), grainJitterSeed(0),
        energyLevel(0.0f), saturatedGain(0.0f), leftGain(0.0f),
        rightGain(0.0f) {}
};

struct RenderStats {
  uint32_t exactVoicesVisited;
  uint32_t groupedObjectsVisited;
  uint32_t densityObjectsVisited;

  RenderStats()
      : exactVoicesVisited(0), groupedObjectsVisited(0),
        densityObjectsVisited(0) {}
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

struct SceneAction {
  SceneActionKind kind;
  uint16_t importanceScore;
  uint8_t observeGrouped;
  uint8_t observeDensity;
  uint8_t protectedAttack;
  uint8_t reserved0;
  uint16_t reserved1;
  NormalizedEvent event;

  SceneAction()
      : kind(SceneActionKind::None), importanceScore(0), observeGrouped(0),
        observeDensity(0), protectedAttack(0), reserved0(0), reserved1(0),
        event() {}
};

struct SceneStats {
  uint32_t exactActions;
  uint32_t groupedObservations;
  uint32_t densityObservations;
  uint32_t resetActions;
  uint32_t protectedAttacks;
  uint32_t groupedOnlyActions;
  uint32_t densityOnlyActions;
  uint32_t panicDecisions;

  SceneStats()
      : exactActions(0), groupedObservations(0), densityObservations(0),
        resetActions(0), protectedAttacks(0), groupedOnlyActions(0),
        densityOnlyActions(0), panicDecisions(0) {}
};

struct TelemetrySnapshot {
  uint32_t exactVoices;
  uint32_t releasedExactVoices;
  uint32_t groupedObjects;
  uint32_t densityObjects;
  uint32_t voiceEquivalent;
  uint32_t schedulerQueuedEvents;
  uint32_t schedulerMaxTransitionQueueDepth;
  uint32_t schedulerCoalescedEvents;
  uint32_t sceneExactActions;
  uint32_t sceneGroupedObservations;
  uint32_t sceneDensityObservations;
  uint32_t exactSteals;
  uint32_t groupedAccumulatedNotes;
  uint32_t densityAccumulatedNotes;
  uint32_t densityPromotedClouds;
  uint32_t lastAppliedEvents;
  uint32_t overloadPressureLevel;

  TelemetrySnapshot()
      : exactVoices(0), releasedExactVoices(0), groupedObjects(0),
        densityObjects(0), voiceEquivalent(0), schedulerQueuedEvents(0),
        schedulerMaxTransitionQueueDepth(0), schedulerCoalescedEvents(0),
        sceneExactActions(0), sceneGroupedObservations(0),
        sceneDensityObservations(0), exactSteals(0),
        groupedAccumulatedNotes(0), densityAccumulatedNotes(0),
        densityPromotedClouds(0), lastAppliedEvents(0),
        overloadPressureLevel(0) {}
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
