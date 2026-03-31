#include "VirtuallySuperGrouped.h"

#include <math.h>

namespace {

static float GroupNoteToFrequencyHz(float note) {
  return 440.0f * powf(2.0f, (note - 69.0f) * (1.0f / 12.0f));
}

static float GroupPan(uint32_t channel, uint32_t note) {
  const uint32_t index = (channel * 17u + note * 13u) & 15u;
  return ((float)index / 7.5f) - 1.0f;
}

static void GroupStereoGains(float pan, float *left, float *right) {
  const float clamped = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
  *left = 0.5f * (1.0f - clamped * 0.35f);
  *right = 0.5f * (1.0f + clamped * 0.35f);
}

} // namespace

namespace virtuallysuper {

GroupedSystem::GroupedSystem()
    : config_(), initialized_(false), nextGroupId_(1), groups_(), freeList_(),
      activeHandles_(), activeCount_(0), freeCount_(0), stats_() {}

bool GroupedSystem::Initialize(const GroupedConfig &config) {
  if (config.maxGroups == 0 || config.pitchBandSemitones == 0 ||
      config.timingBucketSamples <= 0) {
    return false;
  }

  config_ = config;
  groups_.assign(config_.maxGroups, GroupedObject());
  activeHandles_.assign(config_.maxGroups, 0);
  freeList_.assign(config_.maxGroups, 0);
  initialized_ = true;
  Reset();
  return true;
}

void GroupedSystem::Reset() {
  nextGroupId_ = 1;
  activeCount_ = 0;
  freeCount_ = (uint32_t)groups_.size();
  stats_ = GroupedStats();

  for (uint32_t i = 0; i < freeCount_; ++i) {
    groups_[i] = GroupedObject();
    freeList_[i] = freeCount_ - 1U - i;
  }
}

void GroupedSystem::BeginWindow() {
  if (!initialized_)
    return;

  ReleaseActiveGroups();
  stats_.activeGroups = 0;
}

bool GroupedSystem::AccumulateEvent(const NormalizedEvent &event) {
  if (!initialized_ || event.kind != EventKind::NoteOn)
    return false;

  const uint32_t pitchBandId = event.note / config_.pitchBandSemitones;
  const uint32_t timingBucketId =
      (uint32_t)(event.targetSample / config_.timingBucketSamples);

  const uint32_t handle =
      FindOrAllocateGroup(event, pitchBandId, timingBucketId);
  if (handle == kInvalidVoiceHandle) {
    ++stats_.droppedNoteOns;
    return false;
  }

  GroupedObject &group = groups_[handle];
  ++group.representedNoteCount;
  ++group.representedLayerCount;
  ++group.noteOnCount;
  group.lastTargetSample = event.targetSample;
  group.gain =
      ((float)group.representedNoteCount * 0.0012f) > 0.02f
          ? 0.02f
          : (float)group.representedNoteCount * 0.0012f;

  ++stats_.noteOnsAccumulated;
  return true;
}

const GroupedConfig &GroupedSystem::GetConfig() const { return config_; }

const GroupedStats &GroupedSystem::GetStats() const { return stats_; }

uint32_t GroupedSystem::GetActiveGroupCount() const { return stats_.activeGroups; }

uint32_t GroupedSystem::GetGroupCapacity() const {
  return (uint32_t)groups_.size();
}

const GroupedObject *GroupedSystem::GetGroup(uint32_t handle) const {
  if (handle >= groups_.size())
    return 0;
  return &groups_[handle];
}

uint32_t GroupedSystem::GetActiveHandleCount() const { return activeCount_; }

uint32_t GroupedSystem::GetActiveHandle(uint32_t index) const {
  if (index >= activeCount_)
    return kInvalidVoiceHandle;
  return activeHandles_[index];
}

bool GroupedSystem::MatchesGroup(const GroupedObject &group,
                                 const NormalizedEvent &event,
                                 uint32_t pitchBandId,
                                 uint32_t timingBucketId) const {
  return group.active != 0 && group.channel == event.channel &&
         group.pitchBandId == pitchBandId &&
         group.timingBucketId == timingBucketId && group.layerTemplateId == 0 &&
         group.sampleFamilyId == 0;
}

uint32_t GroupedSystem::FindOrAllocateGroup(const NormalizedEvent &event,
                                            uint32_t pitchBandId,
                                            uint32_t timingBucketId) {
  for (uint32_t i = 0; i < activeCount_; ++i) {
    const uint32_t handle = activeHandles_[i];
    if (MatchesGroup(groups_[handle], event, pitchBandId, timingBucketId))
      return handle;
  }

  if (freeCount_ == 0)
    return kInvalidVoiceHandle;

  const uint32_t handle = freeList_[--freeCount_];
  GroupedObject &group = groups_[handle];
  group.active = 1;
  group.channel = event.channel;
  group.pitchBandId = (uint8_t)pitchBandId;
  group.layerTemplateId = 0;
  group.sampleFamilyId = 0;
  group.groupId = nextGroupId_++;
  group.timingBucketId = timingBucketId;
  group.activeListIndex = activeCount_;
  group.lastTargetSample = event.targetSample;
  const float centerNote =
      (float)(pitchBandId * config_.pitchBandSemitones) +
      (float)config_.pitchBandSemitones * 0.5f;
  group.phase =
      (float)((group.groupId * 1103515245u + 12345u) & 1023u) / 1024.0f;
  group.phaseStep =
      GroupNoteToFrequencyHz(centerNote) /
      (float)(config_.sampleRate > 0 ? config_.sampleRate : 44100u);
  GroupStereoGains(GroupPan(event.channel, (uint32_t)centerNote),
                   &group.leftGain, &group.rightGain);
  group.gain = 0.0f;
  activeHandles_[activeCount_++] = handle;

  ++stats_.activeGroups;
  if (stats_.activeGroups > stats_.peakActiveGroups)
    stats_.peakActiveGroups = stats_.activeGroups;
  return handle;
}

void GroupedSystem::ReleaseActiveGroups() {
  for (uint32_t i = 0; i < activeCount_; ++i) {
    const uint32_t handle = activeHandles_[i];
    groups_[handle] = GroupedObject();
    freeList_[freeCount_++] = handle;
  }
  activeCount_ = 0;
}

} // namespace virtuallysuper
