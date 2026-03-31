#include "VirtuallySuperDensity.h"

namespace {

static float DensityPan(uint32_t channel, uint32_t note) {
  const uint32_t index = (channel * 17u + note * 13u) & 15u;
  return ((float)index / 7.5f) - 1.0f;
}

static void DensityStereoGains(float pan, float *left, float *right) {
  const float clamped = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
  *left = 0.5f * (1.0f - clamped * 0.35f);
  *right = 0.5f * (1.0f + clamped * 0.35f);
}

} // namespace

namespace virtuallysuper {

DensitySystem::DensitySystem()
    : config_(), initialized_(false), nextDensityId_(1), objects_(), freeList_(),
      activeHandles_(), activeCount_(0), freeCount_(0), stats_() {}

bool DensitySystem::Initialize(const DensityConfig &config) {
  if (config.maxObjects == 0 || config.pitchBandSemitones == 0 ||
      config.timingBucketSamples <= 0 || config.saturationK == 0) {
    return false;
  }

  config_ = config;
  objects_.assign(config_.maxObjects, DensityObject());
  activeHandles_.assign(config_.maxObjects, 0);
  freeList_.assign(config_.maxObjects, 0);
  initialized_ = true;
  Reset();
  return true;
}

void DensitySystem::Reset() {
  nextDensityId_ = 1;
  activeCount_ = 0;
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

  ReleaseActiveObjects();
  stats_.activeObjects = 0;
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

const DensityConfig &DensitySystem::GetConfig() const { return config_; }

const DensityStats &DensitySystem::GetStats() const { return stats_; }

uint32_t DensitySystem::GetActiveObjectCount() const { return stats_.activeObjects; }

uint32_t DensitySystem::GetObjectCapacity() const {
  return (uint32_t)objects_.size();
}

const DensityObject *DensitySystem::GetDensityObject(uint32_t handle) const {
  if (handle >= objects_.size())
    return 0;
  return &objects_[handle];
}

uint32_t DensitySystem::GetActiveHandleCount() const { return activeCount_; }

uint32_t DensitySystem::GetActiveHandle(uint32_t index) const {
  if (index >= activeCount_)
    return kInvalidVoiceHandle;
  return activeHandles_[index];
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
  for (uint32_t i = 0; i < activeCount_; ++i) {
    const uint32_t handle = activeHandles_[i];
    if (MatchesObject(objects_[handle], event, pitchBandId, timingBucketId))
      return handle;
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
  object.activeListIndex = activeCount_;
  object.grainJitterSeed = MakeDeterministicSeed(event, pitchBandId, timingBucketId);
  DensityStereoGains(DensityPan(event.channel, pitchBandId * 12u),
                     &object.leftGain, &object.rightGain);
  activeHandles_[activeCount_++] = handle;

  ++stats_.activeObjects;
  if (stats_.activeObjects > stats_.peakActiveObjects)
    stats_.peakActiveObjects = stats_.activeObjects;
  return handle;
}

void DensitySystem::UpdateDerivedState(DensityObject &object,
                                       const NormalizedEvent &event) {
  const uint8_t effectiveVelocity =
      event.mappedVelocity > 0 ? event.mappedVelocity : event.velocity;
  object.energyLevel =
      (float)object.representedNoteCount * (float)effectiveVelocity;
  const float represented = (float)object.representedNoteCount;
  const float k = (float)config_.saturationK;
  object.saturatedGain =
      (represented / (represented + k)) * ((float)effectiveVelocity / 127.0f);
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

void DensitySystem::ReleaseActiveObjects() {
  for (uint32_t i = 0; i < activeCount_; ++i) {
    const uint32_t handle = activeHandles_[i];
    objects_[handle] = DensityObject();
    freeList_[freeCount_++] = handle;
  }
  activeCount_ = 0;
}

} // namespace virtuallysuper
