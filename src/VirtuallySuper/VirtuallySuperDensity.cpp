#include "VirtuallySuperDensity.h"

namespace virtuallysuper {

DensitySystem::DensitySystem()
    : config_(), initialized_(false), nextDensityId_(1), objects_(), freeList_(),
      freeCount_(0), stats_() {}

bool DensitySystem::Initialize(const DensityConfig &config) {
  if (config.maxObjects == 0 || config.pitchBandSemitones == 0 ||
      config.timingBucketSamples <= 0 || config.saturationK == 0) {
    return false;
  }

  config_ = config;
  objects_.assign(config_.maxObjects, DensityObject());
  freeList_.assign(config_.maxObjects, 0);
  initialized_ = true;
  Reset();
  return true;
}

void DensitySystem::Reset() {
  nextDensityId_ = 1;
  freeCount_ = (uint32_t)objects_.size();
  stats_ = DensityStats();

  for (uint32_t i = 0; i < freeCount_; ++i) {
    objects_[i] = DensityObject();
    freeList_[i] = freeCount_ - 1U - i;
  }
}

void DensitySystem::BeginWindow() {
  if (!initialized_)
    return;

  freeCount_ = (uint32_t)objects_.size();
  stats_.activeObjects = 0;
  for (uint32_t i = 0; i < freeCount_; ++i) {
    objects_[i] = DensityObject();
    freeList_[i] = freeCount_ - 1U - i;
  }
}

bool DensitySystem::AccumulateEvent(const NormalizedEvent &event) {
  if (!initialized_ || event.kind != EventKind::NoteOn)
    return false;

  const uint32_t pitchBandId = event.note / config_.pitchBandSemitones;
  const uint32_t timingBucketId =
      (uint32_t)(event.targetSample / config_.timingBucketSamples);

  const uint32_t handle =
      FindOrAllocateObject(event, pitchBandId, timingBucketId);
  if (handle == kInvalidVoiceHandle) {
    ++stats_.droppedNoteOns;
    return false;
  }

  DensityObject &object = objects_[handle];
  ++object.representedNoteCount;
  ++object.representedLayerCount;
  UpdateDerivedState(object, event);
  ++stats_.noteOnsAccumulated;
  if (object.representedNoteCount == config_.activationThreshold)
    ++stats_.promotedClouds;
  return true;
}

const DensityStats &DensitySystem::GetStats() const { return stats_; }

uint32_t DensitySystem::GetActiveObjectCount() const { return stats_.activeObjects; }

const DensityObject *DensitySystem::GetDensityObject(uint32_t handle) const {
  if (handle >= objects_.size())
    return 0;
  return &objects_[handle];
}

bool DensitySystem::MatchesObject(const DensityObject &object,
                                  const NormalizedEvent &event,
                                  uint32_t pitchBandId,
                                  uint32_t timingBucketId) const {
  return object.active != 0 && object.channel == event.channel &&
         object.pitchBandId == pitchBandId &&
         object.timingBucketId == timingBucketId && object.sampleFamilyId == 0;
}

uint32_t DensitySystem::FindOrAllocateObject(const NormalizedEvent &event,
                                             uint32_t pitchBandId,
                                             uint32_t timingBucketId) {
  for (uint32_t i = 0; i < objects_.size(); ++i) {
    if (MatchesObject(objects_[i], event, pitchBandId, timingBucketId))
      return i;
  }

  if (freeCount_ == 0)
    return kInvalidVoiceHandle;

  const uint32_t handle = freeList_[--freeCount_];
  DensityObject &object = objects_[handle];
  object.active = 1;
  object.channel = event.channel;
  object.pitchBandId = (uint8_t)pitchBandId;
  object.activationThreshold = config_.activationThreshold;
  object.sampleFamilyId = 0;
  object.densityId = nextDensityId_++;
  object.timingBucketId = timingBucketId;
  object.grainJitterSeed = MakeDeterministicSeed(event, pitchBandId, timingBucketId);

  ++stats_.activeObjects;
  if (stats_.activeObjects > stats_.peakActiveObjects)
    stats_.peakActiveObjects = stats_.activeObjects;
  return handle;
}

void DensitySystem::UpdateDerivedState(DensityObject &object,
                                       const NormalizedEvent &event) {
  object.energyLevel = (float)object.representedNoteCount * (float)event.velocity;
  const float represented = (float)object.representedNoteCount;
  const float k = (float)config_.saturationK;
  object.saturatedGain = represented / (represented + k);
}

uint32_t DensitySystem::MakeDeterministicSeed(const NormalizedEvent &event,
                                              uint32_t pitchBandId,
                                              uint32_t timingBucketId) const {
  uint32_t seed = event.sequence * 2654435761u;
  seed ^= ((uint32_t)event.channel << 24);
  seed ^= ((uint32_t)event.note << 16);
  seed ^= (pitchBandId << 8);
  seed ^= timingBucketId;
  if (seed == 0)
    seed = 1;
  return seed;
}

} // namespace virtuallysuper
