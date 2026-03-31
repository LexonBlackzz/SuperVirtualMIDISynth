#ifndef VIRTUALLYSUPER_GROUPED_H
#define VIRTUALLYSUPER_GROUPED_H

#include "VirtuallySuperTypes.h"

#include <vector>

namespace virtuallysuper {

class GroupedSystem {
public:
  GroupedSystem();

  bool Initialize(const GroupedConfig &config);
  void Reset();
  void BeginWindow();
  bool AccumulateEvent(const NormalizedEvent &event);

  const GroupedConfig &GetConfig() const;
  const GroupedStats &GetStats() const;
  uint32_t GetActiveGroupCount() const;
  uint32_t GetGroupCapacity() const;
  const GroupedObject *GetGroup(uint32_t handle) const;
  uint32_t GetActiveHandleCount() const;
  uint32_t GetActiveHandle(uint32_t index) const;

private:
  bool MatchesGroup(const GroupedObject &group, const NormalizedEvent &event,
                    uint32_t pitchBandId, uint32_t timingBucketId) const;
  uint32_t FindOrAllocateGroup(const NormalizedEvent &event,
                               uint32_t pitchBandId,
                               uint32_t timingBucketId);
  void ReleaseActiveGroups();

  GroupedConfig config_;
  bool initialized_;
  uint32_t nextGroupId_;
  std::vector<GroupedObject> groups_;
  std::vector<uint32_t> activeHandles_;
  std::vector<uint32_t> freeList_;
  uint32_t activeCount_;
  uint32_t freeCount_;
  GroupedStats stats_;
};

} // namespace virtuallysuper

#endif
