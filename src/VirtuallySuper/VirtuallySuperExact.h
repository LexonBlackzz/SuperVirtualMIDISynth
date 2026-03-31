#ifndef VIRTUALLYSUPER_EXACT_H
#define VIRTUALLYSUPER_EXACT_H

#include "VirtuallySuperTypes.h"

#include <vector>

namespace virtuallysuper {

class ExactSystem {
public:
  ExactSystem();

  bool Initialize(const ExactConfig &config);
  void Reset();

  bool ApplyEvent(const NormalizedEvent &event);
  bool NoteOn(const NormalizedEvent &event);
  uint32_t NoteOff(uint8_t channel, uint8_t note);

  uint32_t GetActiveVoiceCount() const;
  uint32_t GetReleasedVoiceCount() const;
  const ExactStats &GetStats() const;
  const ExactConfig &GetConfig() const;
  const ExactVoice *GetVoice(uint32_t handle) const;
  ExactVoice *GetMutableVoice(uint32_t handle);
  uint32_t GetKeyHead(uint32_t channel, uint32_t note) const;
  uint32_t GetQueueHead(ExactQueueClass queueClass) const;
  void RetireVoice(uint32_t handle);

private:
  struct QueueState {
    uint32_t head;
    uint32_t tail;

    QueueState() : head(kInvalidVoiceHandle), tail(kInvalidVoiceHandle) {}
  };

  bool AllocateVoiceHandle(uint32_t *handle, bool *stolen);
  bool SelectStealCandidate(uint32_t *handle, bool *quiet, bool *released) const;
  void ReleaseVoiceHandle(uint32_t handle);
  void ActivateVoice(uint32_t handle, const NormalizedEvent &event);
  void TransitionVoiceToReleased(uint32_t handle);
  void InsertKeyVoice(uint32_t handle);
  void RemoveKeyVoice(uint32_t handle);
  void LinkQueueTail(ExactQueueClass queueClass, uint32_t handle);
  void UnlinkQueue(uint32_t handle);
  ExactQueueClass ClassifyQueue(const ExactVoice &voice) const;
  void ReclassifyVoiceQueue(uint32_t handle);
  void AddVoiceState(ExactLifecycleState state);
  void RemoveVoiceState(ExactLifecycleState state);

  ExactConfig config_;
  bool initialized_;
  uint32_t nextVoiceId_;
  uint32_t generationCounters_[kChannelCount][kNoteCount];
  uint32_t keyHeads_[kChannelCount][kNoteCount];
  QueueState queues_[5];
  std::vector<ExactVoice> voices_;
  std::vector<uint32_t> freeList_;
  uint32_t freeCount_;
  ExactStats stats_;
};

} // namespace virtuallysuper

#endif
