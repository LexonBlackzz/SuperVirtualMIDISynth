#ifndef VIRTUALLYSUPER_DENSITY_H
#define VIRTUALLYSUPER_DENSITY_H

#include "VirtuallySuperTypes.h"

#include <vector>

namespace virtuallysuper {

class DensitySystem {
public:
  DensitySystem();

  bool Initialize(const DensityConfig &config);
  void Reset();
  void BeginWindow();
  bool AccumulateEvent(const NormalizedEvent &event);

  const DensityStats &GetStats() const;
  uint32_t GetActiveObjectCount() const;
  const DensityObject *GetDensityObject(uint32_t handle) const;

private:
  bool MatchesObject(const DensityObject &object, const NormalizedEvent &event,
                     uint32_t pitchBandId, uint32_t timingBucketId) const;
  uint32_t FindOrAllocateObject(const NormalizedEvent &event,
                                uint32_t pitchBandId,
                                uint32_t timingBucketId);
  void UpdateDerivedState(DensityObject &object, const NormalizedEvent &event);
  uint32_t MakeDeterministicSeed(const NormalizedEvent &event,
                                 uint32_t pitchBandId,
                                 uint32_t timingBucketId) const;

  DensityConfig config_;
  bool initialized_;
  uint32_t nextDensityId_;
  std::vector<DensityObject> objects_;
  std::vector<uint32_t> freeList_;
  uint32_t freeCount_;
  DensityStats stats_;
};

} // namespace virtuallysuper

#endif
