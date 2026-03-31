#include "VirtuallySuperGrouped.h"

namespace virtuallysuper {

GroupedSystem::GroupedSystem()
    : config_(), initialized_(false), nextGroupId_(1), groups_(), freeList_(),
      freeCount_(0), stats_() {}

bool GroupedSystem::Initialize(const GroupedConfig &config) {
  if (config.maxGroups == 0 || config.pitchBandSemitones == 0 ||
      config.timingBucketSamples <= 0) {
    return false;
  }

  config_ = config;
  groups_.assign(config_.maxGroups, GroupedObject());
  freeList_.assign(config_.maxGroups, 0);
  initialized_ = true;
  Reset();
  return true;
}

void GroupedSystem::Reset() {
  nextGroupId_ = 1;
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

  freeCount_ = (uint32_t)groups_.size();
  stats_.activeGroups = 0;
  for (uint32_t i = 0; i < freeCount_; ++i) {
    groups_[i] = GroupedObject();
    freeList_[i] = freeCount_ - 1U - i;
  }
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
  for (uint32_t i = 0; i < groups_.size(); ++i) {
    if (MatchesGroup(groups_[i], event, pitchBandId, timingBucketId))
      return i;
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
  group.lastTargetSample = event.targetSample;

  ++stats_.activeGroups;
  if (stats_.activeGroups > stats_.peakActiveGroups)
    stats_.peakActiveGroups = stats_.activeGroups;
  return handle;
}

} // namespace virtuallysuper
