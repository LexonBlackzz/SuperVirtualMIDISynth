#ifndef SYNTH_H
#define SYNTH_H

#include "Compat.h"
#include "SamplerEngine.h"
#include <atomic>
#include <vector>
#include <memory>
#include <string>

class Synth {
public:
  static Synth &Instance();

  void Initialize();
  void Render(float *output, int numFrames);
  void Shutdown(bool waitForThreads = true);
  void ForceShutdown(bool waitForThreads = true);
  void ReloadRuntimeSettings();
  void SetRealtimeBudgetMs(float audioBudgetMs);
  void SetRenderBlockContext(unsigned long long blockStartSample,
                             int blockFrames, int sampleRate,
                             long long blockStartQpc, long long blockEndQpc,
                             bool quantizedByPollingRate);
  int GetRefCount();
  std::string GetResolvedSoundfontPath();
  std::string GetResolvedSamplerEngineName();
  std::string GetRequestedSamplerEngineName();
  std::string GetResolvedSourceFormat();
  std::string GetLastInitStatus();
  DWORD GetActiveVoiceStats(DWORD *channelCounts, int count);
  SamplerDiagnostics GetSamplerDiagnostics();

  // MIDI thread safe (producer-safe queued push)
  void NoteOn(int channel, int note, int velocity);
  void NoteOff(int channel, int note);
  void ProgramChange(int channel, int program);
  void ControlChange(int channel, int control, int value);
  void PitchBend(int channel, int value);
  void Reset();
  void SetRestartReason(unsigned int reasonCode);

private:
  struct ScheduledTimedEvent;
  struct ScheduledNoteOnTrimEntry;
  struct EventRingBuffer;
  struct FlatMidiQueue;
  struct PendingTransitionQueue;

  Synth();
  ~Synth();

  void PushEvent(MidiEvent ev);
  void ProcessEventsLocked();
  RuntimeSettings LoadRuntimeSettingsLocked() const;
  void InitializeEventRing(EventRingBuffer &ring, unsigned int capacity);
  void ResetEventRing(EventRingBuffer &ring);
  void InitializeFlatMidiQueue(FlatMidiQueue &queue, unsigned int capacity);
  void ResetFlatMidiQueue(FlatMidiQueue &queue);
  bool TryPushEventRing(EventRingBuffer &ring, const MidiEvent &event,
                        std::atomic<unsigned int> &depthCounter);
  unsigned int DrainEventRingToPendingLocked(EventRingBuffer &ring,
                                             std::atomic<unsigned int> &depthCounter,
                                             bool accurateRoute,
                                             unsigned int maxCount);
  unsigned int GetIngressDepthLocked() const;
  static int GetEventPriority(const MidiEvent &event);
  static bool IsScheduledTimingEvent(const MidiEvent &event);
  static bool IsPositiveNoteOn(const MidiEvent &event);
  static bool IsActualNoteOffEvent(const MidiEvent &event);
  static bool IsNoteTransitionEvent(const MidiEvent &event);
  void EnqueueDeferredEvent(const MidiEvent &event);
  void EnqueuePendingEventLocked(const MidiEvent &event);
  void EnqueueReleaseLaneEventLocked(const MidiEvent &event);
  unsigned int PopReleaseLaneEventsLocked(std::vector<MidiEvent> &events,
                                          unsigned int maxCount);
  unsigned int DropDeferredNoteOnsForKey(int channel, int note);
  unsigned int DropOldestDeferredNoteOns(unsigned int count);
  unsigned int DropOldestPendingVectorNoteOnsLocked(std::vector<MidiEvent> &queue,
                                                    unsigned int count,
                                                    int velocityFloor = -1);
  unsigned int DropOldestIngressNoteOnsLocked(unsigned int count,
                                              int velocityFloor = -1);
  unsigned int ExtractAccurateWorksetLocked(std::vector<MidiEvent> &events,
                                            unsigned int maxCount,
                                            unsigned int overloadState);
  bool PopNextDeferredEvent(MidiEvent &event, int priorityClass);
  bool PopNextIncomingEvent(MidiEvent &event, int priorityClass);
  bool IsCriticalControlEvent(const MidiEvent &event) const;
  float GetMidiBudgetMsLocked() const;
  bool IsSchedulerEnabledLocked() const;
  bool IsStrictAccurateModeLocked() const;
  EventTimingMode GetEventTimingModeFast() const;
  long long GetRenderBlockEndSampleLocked() const;
  int GetQuantizeGridFramesLocked() const;
  void ResetScheduledStateLocked();
  void FlushScheduledEventsLocked();
  void CollectPendingScheduledEventsLocked(std::vector<MidiEvent> &events);
  long long QuantizeTargetSampleLocked(long long sample) const;
  void InsertScheduledTimedEventLocked(const MidiEvent &event,
                                       bool hardOverload);
  void ScheduleEventsLocked(const std::vector<MidiEvent> &events,
                            bool hardOverload);
  void EnsureScheduledEventIndexCapacityLocked(uint32_t eventId);
  void RefreshScheduledCacheLocked() const;
  void NoteScheduledEventInsertedLocked(const ScheduledTimedEvent &event);
  void NoteScheduledEventRemovedLocked(const ScheduledTimedEvent &event);
  void NoteScheduledEventMutatedLocked(const ScheduledTimedEvent &oldEvent,
                                       const ScheduledTimedEvent &newEvent);
  static bool ScheduledEventLess(const ScheduledTimedEvent &lhs,
                                 const ScheduledTimedEvent &rhs);
  static bool ScheduledNoteOnTrimLess(const ScheduledNoteOnTrimEntry &lhs,
                                      const ScheduledNoteOnTrimEntry &rhs);
  void SwapScheduledHeapNodesLocked(uint32_t lhs, uint32_t rhs);
  void SiftScheduledHeapUpLocked(uint32_t index);
  void SiftScheduledHeapDownLocked(uint32_t index);
  void PruneScheduledHeapTopLocked();
  void PushScheduledNoteOnTrimLocked(uint32_t eventId, ULONGLONG enqueueTick,
                                     long long targetSample,
                                     unsigned int sequence,
                                     unsigned int velocity);
  bool PopScheduledNoteOnTrimLocked(uint32_t *eventId);
  uint32_t PushScheduledEventLocked(const ScheduledTimedEvent &event);
  bool PopScheduledEventLocked(ScheduledTimedEvent &event);
  ScheduledTimedEvent *GetScheduledEventLocked(uint32_t eventId);
  const ScheduledTimedEvent *GetScheduledEventLocked(uint32_t eventId) const;
  void CancelScheduledEventLocked(uint32_t eventId);
  void UpdateScheduledKeyStateFromRefsLocked(int channel, int note);
  void ResetScheduledAppliedStateLocked();
  void CompactPendingTransitionQueueLocked(int channel, int note);
  bool RemovePendingTransitionEventLocked(int channel, int note,
                                          uint32_t eventId);
  bool AppendPendingTransitionEventLocked(int channel, int note,
                                          const ScheduledTimedEvent &event,
                                          bool hardOverload,
                                          bool &droppedNewEvent);
  int FindQueuedSameStateTransitionLocked(int channel, int note, int stateIndex,
                                          long long targetSample) const;
  int FindQueuedOppositeStateSameSampleLocked(int channel, int note,
                                              int stateIndex,
                                              long long targetSample) const;
  unsigned int ComputeScheduledQueueAgeMsLocked() const;
  unsigned int ComputeScheduledLagStateLocked() const;
  unsigned int ComputeScheduledPendingCountLocked() const;
  unsigned int ComputeScheduledLagSamplesLocked() const;
  bool DrainScheduledWindowLocked(long long cursorSample,
                                  long long blockEndSample,
                                  long long windowEndSample,
                                  std::vector<MidiEvent> &dueEvents,
                                  long long &renderUntilSample);
  void ApplyScheduledEventsLocked(const std::vector<MidiEvent> &events,
                                  unsigned int &appliedCount);
  static bool IsReleaseAffectingControlEvent(const MidiEvent &event);
  static bool IsReleaseLikeEvent(const MidiEvent &event);
  static int GetScheduledEventApplyPriority(const MidiEvent &event);
  unsigned int ComputeSameKeyPendingTransitionCountLocked() const;
  unsigned int ComputeMaxSameKeyQueueDepthLocked() const;
  bool IsAccurateEventThreadEnabledLocked() const;
  void EnsureAccurateEventThreadLocked();
  void StopAccurateEventThreadLocked(bool waitForThread);
  void SignalAccurateEventThread();
  void ProcessAccuratePendingEvents();
  void PrepareAccurateTimedEvents(std::vector<MidiEvent> &events);
  unsigned int DropOldestPendingNoteOnsLocked(FlatMidiQueue &queue,
                                              unsigned int count,
                                              int velocityFloor = -1);
  unsigned int DropScheduledPendingNoteOnsLocked(unsigned int count,
                                                 int velocityFloor = -1);
  unsigned int TrimAccurateOverloadQueuesLocked(unsigned int overloadState);
  void FilterDueEventsForOverloadLocked(std::vector<MidiEvent> &events,
                                        long long currentSample,
                                        unsigned int &droppedCount);
  static DWORD WINAPI AccurateEventThreadProc(LPVOID param);

  struct ScheduledTimedEvent {
    MidiEvent event;
    long long targetSample;
    ULONGLONG enqueueTick;
    uint32_t eventId;
    uint8_t applyPriority;
    uint8_t kind;
    uint8_t channel;
    uint8_t note;
    bool cancelled;
  };

#ifdef SVMS_LEGACY_XP
  enum { kPendingTransitionsPerKey = 4 };
#else
  enum { kPendingTransitionsPerKey = 8 };
#endif

  struct PendingTransitionQueue {
    uint32_t eventIds[kPendingTransitionsPerKey];
    unsigned char count;

    PendingTransitionQueue() : count(0) {
      for (unsigned int i = 0; i < kPendingTransitionsPerKey; ++i)
        eventIds[i] = 0u;
    }
  };

  struct ScheduledNoteOnTrimEntry {
    ULONGLONG enqueueTick;
    long long targetSample;
    unsigned int sequence;
    unsigned char velocity;
    uint32_t eventId;
  };

  struct ScheduledKeyState {
    unsigned int soundingGenerations;
    unsigned int pendingNoteOnCount;
    unsigned int pendingNoteOffCount;
    bool lastScheduledState;
    bool lastAppliedState;

    ScheduledKeyState()
        : soundingGenerations(0), pendingNoteOnCount(0), pendingNoteOffCount(0),
          lastScheduledState(false), lastAppliedState(false) {}
  };

  struct EventRingBuffer {
    std::vector<MidiEvent> slots;
    volatile LONG readIndex;
    volatile LONG writeIndex;
    LONG capacity;
    LONG mask;

    EventRingBuffer()
        : readIndex(0), writeIndex(0), capacity(0), mask(0) {}
  };

  struct FlatMidiQueue {
    std::vector<MidiEvent> slots;
    uint32_t head;
    uint32_t tail;
    uint32_t mask;
    uint32_t count;

    FlatMidiQueue() : head(0), tail(0), mask(0), count(0) {}

    void clear() {
      head = 0;
      tail = 0;
      count = 0;
    }

    bool empty() const { return count == 0; }
    uint32_t size() const { return count; }

    MidiEvent &front() { return slots[head]; }
    const MidiEvent &front() const { return slots[head]; }

    MidiEvent &back() {
      return slots[(tail - 1u) & mask];
    }
    const MidiEvent &back() const {
      return slots[(tail - 1u) & mask];
    }

    MidiEvent &at(uint32_t index) {
      return slots[(head + index) & mask];
    }
    const MidiEvent &at(uint32_t index) const {
      return slots[(head + index) & mask];
    }

    bool push_back(const MidiEvent &event) {
      if (slots.empty() || count >= slots.size())
        return false;
      slots[tail] = event;
      tail = (tail + 1u) & mask;
      ++count;
      return true;
    }

    bool push_front(const MidiEvent &event) {
      if (slots.empty() || count >= slots.size())
        return false;
      head = (head - 1u) & mask;
      slots[head] = event;
      ++count;
      return true;
    }

    bool pop_front() {
      if (!count)
        return false;
      head = (head + 1u) & mask;
      --count;
      return true;
    }

    bool pop_back() {
      if (!count)
        return false;
      tail = (tail - 1u) & mask;
      --count;
      return true;
    }
  };

  std::unique_ptr<ISamplerEngine> engine;
  compat::Mutex synthMutex;
  compat::Mutex eventQueueMutex;
  int refCount = 0;
  std::atomic<unsigned int> acceptingEvents{0};
  RuntimeSettings runtimeSettings;
  int configuredMaxVoices = 500;
  std::string requestedSamplerEngineName;
  std::string lastInitStatus;
  std::atomic<int> eventTimingModeFast;
  EventRingBuffer realtimeIngressRing;
  EventRingBuffer accurateIngressRing;
  std::vector<MidiEvent> pendingCriticalEvents;
  std::vector<MidiEvent> pendingRealtimeEvents;
  std::vector<MidiEvent> pendingNoteOnEvents;
  FlatMidiQueue deferredCriticalEvents;
  FlatMidiQueue deferredRealtimeEvents;
  FlatMidiQueue deferredNoteOnEvents;
  FlatMidiQueue overloadReleaseEvents;
  std::vector<MidiEvent> incomingCriticalEvents;
  std::vector<MidiEvent> incomingRealtimeEvents;
  std::vector<MidiEvent> incomingNoteOnEvents;
  FlatMidiQueue accuratePendingEvents;
  std::vector<ScheduledTimedEvent> scheduledHeap;
  std::vector<ScheduledNoteOnTrimEntry> scheduledNoteOnTrimHeap;
  PendingTransitionQueue pendingTransition[16][128];
  std::vector<uint32_t> heapIndexByEventId;
  ScheduledKeyState scheduledKeyStates[16][128];
  unsigned int scheduledPendingCount = 0;
  mutable bool scheduledCacheDirty = false;
  mutable bool scheduledCacheHasValue = false;
  mutable ULONGLONG scheduledOldestEnqueueTick = 0;
  mutable long long scheduledEarliestTargetSample = 0;
  size_t incomingCriticalIndex = 0;
  size_t incomingRealtimeIndex = 0;
  size_t incomingNoteOnIndex = 0;
  std::vector<MidiEvent> dueEventsScratch;
  std::vector<MidiEvent> releaseEventsScratch;
  std::vector<MidiEvent> scheduledIngressScratch;
  std::vector<MidiEvent> accurateWorksetScratch;
  std::vector<unsigned char> dueEventKeepFlagsScratch;
  std::vector<size_t> dueEventOnTimeStrongScratch;
  std::vector<size_t> dueEventOnTimeQuietScratch;
  std::vector<size_t> dueEventOverdueStrongScratch;
  std::vector<size_t> dueEventOverdueQuietScratch;
  EventTimingMode eventTimingMode = EventTimingMode::ACCURATE;
  std::atomic<unsigned int> nextEventSequence{1};
  std::atomic<unsigned int> droppedNoteOnEvents{0};
  std::atomic<unsigned int> droppedNonNoteEvents{0};
  std::atomic<unsigned int> maxObservedQueueDepth{0};
  std::atomic<unsigned int> maxAsyncQueueDepth{0};
  std::atomic<unsigned int> noteOffIngressCounter{0};
  std::atomic<unsigned int> noteOffDeferredCounter{0};
  std::atomic<unsigned int> noteOffReleaseLaneQueuedCounter{0};
  std::atomic<unsigned int> noteOffReleaseLaneAppliedCounter{0};
  unsigned int lastPendingQueueDepth = 0;
  unsigned int lastDeferredQueueDepth = 0;
  unsigned int lastCriticalQueueDepth = 0;
  unsigned int lastRealtimeQueueDepth = 0;
  unsigned int lastNoteOnQueueDepth = 0;
  unsigned int lastEventsProcessed = 0;
  unsigned int lastNoteOnsAttempted = 0;
  unsigned int lastNoteOnsStarted = 0;
  unsigned int lastNoteOnsDropped = 0;
  unsigned int lastNoteOffsProcessed = 0;
  unsigned int lastNoteOffIngress = 0;
  unsigned int lastNoteOffDeferred = 0;
  unsigned int lastNoteOffReleaseLaneQueued = 0;
  unsigned int lastNoteOffReleaseLaneApplied = 0;
  unsigned int lastNoteOffLate = 0;
  unsigned int lastAsyncPendingNoteOns = 0;
  unsigned int lastAsyncStarted = 0;
  unsigned int lastAsyncDropped = 0;
  unsigned int lastAsyncCoalesced = 0;
  unsigned int lastOverloadNoteOnsDropped = 0;
  unsigned int lastStaleNoteOnsDropped = 0;
  unsigned int lastPreScheduleDrops = 0;
  unsigned int lastPostScheduleDrops = 0;
  unsigned int lastCatchupPrevented = 0;
  unsigned int lastAsyncQueueAgeMs = 0;
  unsigned int lastAsyncLagState = 0;
  unsigned int lastSchedulerDueEvents = 0;
  unsigned int lastSchedulerLateEvents = 0;
  unsigned int lastSchedulerLagSamples = 0;
  unsigned int lastSchedulerPendingSameKeyTransitions = 0;
  unsigned int lastSchedulerMaxSameKeyQueueDepth = 0;
  unsigned int lastSchedulerNoteOnsCoalesced = 0;
  unsigned int lastSchedulerNoteOffsApplied = 0;
  unsigned int lastSchedulerNoteOffsCoalesced = 0;
  unsigned int lastSchedulerNoteOffsCanceled = 0;
  unsigned int lastSchedulerReleaseControlsApplied = 0;
  unsigned int lastSchedulerRenderSplits = 0;
  unsigned int overloadState = 0;
  unsigned int consecutiveOverloadBlocks = 0;
  unsigned int lastLoggedOverloadState = 0;
  float lastMidiProcessMs = 0.0f;
  float currentAudioBudgetMs = 0.0f;
  float lastSampleRenderEstimateMs = 0.0f;
  unsigned int lastSchedulerBlockFrames = 0;
  float lastSchedulerBlockMs = 0.0f;
  unsigned long long currentRenderBlockStartSample = 0;
  long long currentRenderBlockStartQpc = 0;
  long long currentRenderBlockEndQpc = 0;
  int currentRenderSampleRate = 0;
  bool currentBlockQuantized = false;
  HANDLE accurateEventThread = NULL;
  HANDLE accurateEventWakeEvent = NULL;
  HANDLE accurateEventStopEvent = NULL;
  std::atomic<long long> accurateEventClockBaseSample{0};
  std::atomic<long long> accurateEventClockBaseQpc{0};
  std::atomic<int> accurateEventClockSampleRate{0};
  std::atomic<unsigned int> realtimeIngressDepth{0};
  std::atomic<unsigned int> accurateIngressDepthAtomic{0};
  std::atomic<long long> renderProgressSample{0};
  std::atomic<long long> renderProgressEndSample{0};
  std::atomic<unsigned int> eventProcessorThreadActive{0};
  std::atomic<long long> schedulerAnchorSample{0};
  std::atomic<long long> schedulerAnchorQpc{0};
  std::atomic<int> schedulerAnchorSampleRate{0};
  unsigned int consecutiveAccurateBlockStartApplies = 0;
  unsigned int schedulerWarmupBlocks = 0;
  unsigned int runtimeReloadCount = 0;
  unsigned int accurateClockResetCount = 0;
  unsigned int schedulerStatePreservedCount = 0;
  unsigned int lastRestartReason = 0;
  unsigned int accurateHardOverloadEntries = 0;
  unsigned int accurateHardOverloadRecoveries = 0;
  unsigned int accurateWorkerBlockedCount = 0;
  unsigned int accurateReleaseLaneDepth = 0;
  unsigned int accurateIngressDepth = 0;
  unsigned int accurateControlsCoalesced = 0;
  unsigned int accurateCatchupPrevented = 0;
  unsigned int accuratePeakPendingEvents = 0;
  unsigned int accuratePeakDeferredEvents = 0;
  unsigned int accuratePeakScheduledEvents = 0;
  unsigned int perfSchedulerCacheRebuilds = 0;
  unsigned int perfSchedulerTrimHeapTombstonePrunes = 0;
};

#endif
