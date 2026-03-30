#ifndef SAMPLER_ENGINE_H
#define SAMPLER_ENGINE_H

#include <algorithm>
#include <cctype>
#include <string>
#include <windows.h>

struct MidiEvent {
  enum Type {
    NOTE_ON,
    NOTE_OFF,
    PROGRAM_CHANGE,
    CONTROL_CHANGE,
    PITCH_BEND,
    RESET
  };
  Type type;
  int channel;
  int data1;
  int data2;
  unsigned int sequence = 0;
  long long arrivalQpc = 0;
  long long targetSample = 0;
};

enum class EventTimingMode { ACCURATE, QUANTIZED, LEGACY_SYNC };

struct RuntimeSettings {
  float velocityCurve;
  float velocityFloor;
  int velocityIgnoreBelow;
  bool asyncNoteStarts;
  EventTimingMode eventTimingMode;
};

enum class SamplerEngineId { AUTO, TSF, SFZ, BASSMIDI };

inline std::string NormalizeSamplerString(const std::string &value) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return (char)std::tolower(ch); });
  return normalized;
}

inline EventTimingMode ParseEventTimingMode(const std::string &value) {
  std::string normalized = NormalizeSamplerString(value);
  if (normalized == "legacy-sync" || normalized == "legacy")
    return EventTimingMode::LEGACY_SYNC;
  if (normalized == "quantized" || normalized == "quantized-grid")
    return EventTimingMode::QUANTIZED;
  return EventTimingMode::ACCURATE;
}

inline const char *EventTimingModeToConfigString(EventTimingMode mode) {
  switch (mode) {
  case EventTimingMode::QUANTIZED:
    return "quantized";
  case EventTimingMode::LEGACY_SYNC:
    return "legacy-sync";
  default:
    return "accurate";
  }
}

inline SamplerEngineId ParseSamplerEngineId(const std::string &value) {
  std::string normalized = NormalizeSamplerString(value);
  if (normalized == "tsf")
    return SamplerEngineId::TSF;
  if (normalized == "sfz")
    return SamplerEngineId::SFZ;
  if (normalized == "bass" || normalized == "bassmidi")
    return SamplerEngineId::BASSMIDI;
  return SamplerEngineId::AUTO;
}

inline const char *SamplerEngineIdToConfigString(SamplerEngineId engineId) {
  switch (engineId) {
  case SamplerEngineId::TSF:
    return "tsf";
  case SamplerEngineId::SFZ:
    return "sfz";
  case SamplerEngineId::BASSMIDI:
    return "bassmidi";
  default:
    return "auto";
  }
}

inline const char *SamplerEngineIdToDisplayString(SamplerEngineId engineId) {
  switch (engineId) {
  case SamplerEngineId::TSF:
    return "TSF";
  case SamplerEngineId::SFZ:
    return "SFZ";
  case SamplerEngineId::BASSMIDI:
    return "BASSMIDI";
  default:
    return "Auto";
  }
}

inline std::string DetectSourceFormat(const std::string &sourcePath) {
  size_t dot = sourcePath.find_last_of('.');
  if (dot == std::string::npos)
    return "";

  std::string extension = NormalizeSamplerString(sourcePath.substr(dot + 1));
  if (extension == "sf2" || extension == "sf3" || extension == "sfz")
    return extension;
  return extension;
}

inline SamplerEngineId ResolveSamplerEngineId(SamplerEngineId requestedEngine,
                                              const std::string &sourcePath) {
  if (requestedEngine != SamplerEngineId::AUTO)
    return requestedEngine;

  std::string format = DetectSourceFormat(sourcePath);
  if (format == "sfz")
    return SamplerEngineId::SFZ;
  return SamplerEngineId::TSF;
}

struct SamplerInitParams {
  std::string sourcePath;
  int sampleRate;
  int maxVoices;
  RuntimeSettings runtimeSettings;
};

struct SamplerDiagnostics {
  unsigned int warningCount;
  unsigned int loadedSampleCount;
  unsigned int failedSampleCount;
  unsigned int queuedMidiEvents;
  unsigned int deferredMidiEvents;
  unsigned int maxQueuedMidiEvents;
  unsigned int droppedNoteOnEvents;
  unsigned int droppedNonNoteEvents;
  unsigned int eventsProcessedThisBlock;
  unsigned int noteOnEventsThisBlock;
  unsigned int noteOnStartedThisBlock;
  unsigned int noteOnDroppedThisBlock;
  unsigned int noteOffEventsThisBlock;
  unsigned int asyncPendingNoteOns;
  unsigned int asyncStartedThisBlock;
  unsigned int asyncDroppedThisBlock;
  unsigned int asyncCoalescedThisBlock;
  unsigned int overloadNoteOnsDroppedThisBlock;
  unsigned int staleNoteOnsDroppedThisBlock;
  unsigned int preScheduleDropsThisBlock;
  unsigned int postScheduleDropsThisBlock;
  unsigned int catchupPreventedThisBlock;
  unsigned int asyncMaxQueuedNoteOns;
  unsigned int asyncQueueAgeMs;
  unsigned int asyncLagState;
  unsigned int asyncNoteStartsEnabled;
  unsigned int eventProcessorThreadActive;
  unsigned int wasapiAsyncFeedActive;
  unsigned int schedulerSliceFrames;
  unsigned int schedulerDueEventsThisBlock;
  unsigned int schedulerLateEventsThisBlock;
  unsigned int schedulerLagSamples;
  unsigned int schedulerPendingSameKeyTransitions;
  unsigned int schedulerMaxSameKeyQueueDepth;
  unsigned int schedulerNoteOnsCoalescedThisBlock;
  unsigned int schedulerNoteOffsAppliedThisBlock;
  unsigned int schedulerNoteOffsCoalescedThisBlock;
  unsigned int schedulerNoteOffsCanceledThisBlock;
  unsigned int schedulerReleaseControlsAppliedThisBlock;
  unsigned int schedulerRenderSplitsThisBlock;
  unsigned int overloadState;
  unsigned int consecutiveOverloadBlocks;
  unsigned int runtimeReloadCount;
  unsigned int accurateClockResetCount;
  unsigned int schedulerStatePreservedCount;
  unsigned int lastRestartReason;
  unsigned int accurateHardOverloadEntries;
  unsigned int accurateHardOverloadRecoveries;
  unsigned int accurateWorkerBlockedCount;
  unsigned int accuratePeakPendingEvents;
  unsigned int accuratePeakDeferredEvents;
  unsigned int accuratePeakScheduledEvents;
  unsigned int perfCountersEnabled;
  unsigned int tsfHelperContiguousBlocks;
  unsigned int tsfHelperGatherBlocks;
  unsigned int tsfHelperComplexBlocks;
  unsigned int tsfClusteredVoicesContiguous;
  unsigned int tsfClusteredVoicesGather;
  unsigned int tsfClusteredVoicesComplex;
  unsigned int tsfSingleThreadFragments;
  unsigned int tsfThreadedFragments;
  unsigned int schedulerCacheRebuilds;
  unsigned int schedulerTrimHeapTombstonePrunes;
  float midiProcessMs;
  float voiceStartMs;
  float sampleRenderMs;
  float schedulerSliceMs;
  float schedulerLagMs;
  unsigned long long schedulerBlockStartSample;
  float pitchBendRange[16];
  std::string lastWarning;

  SamplerDiagnostics()
      : warningCount(0), loadedSampleCount(0), failedSampleCount(0),
        queuedMidiEvents(0), deferredMidiEvents(0), maxQueuedMidiEvents(0),
        droppedNoteOnEvents(0), droppedNonNoteEvents(0),
        eventsProcessedThisBlock(0), noteOnEventsThisBlock(0),
        noteOnStartedThisBlock(0), noteOnDroppedThisBlock(0),
        noteOffEventsThisBlock(0), asyncPendingNoteOns(0),
        asyncStartedThisBlock(0), asyncDroppedThisBlock(0),
        asyncCoalescedThisBlock(0), overloadNoteOnsDroppedThisBlock(0),
        staleNoteOnsDroppedThisBlock(0), preScheduleDropsThisBlock(0),
        postScheduleDropsThisBlock(0), catchupPreventedThisBlock(0),
        asyncMaxQueuedNoteOns(0),
        asyncQueueAgeMs(0), asyncLagState(0), asyncNoteStartsEnabled(0),
        eventProcessorThreadActive(0), wasapiAsyncFeedActive(0),
        schedulerSliceFrames(0),
        schedulerDueEventsThisBlock(0), schedulerLateEventsThisBlock(0),
        schedulerLagSamples(0), schedulerPendingSameKeyTransitions(0),
        schedulerMaxSameKeyQueueDepth(0),
        schedulerNoteOnsCoalescedThisBlock(0),
        schedulerNoteOffsAppliedThisBlock(0),
        schedulerNoteOffsCoalescedThisBlock(0),
        schedulerNoteOffsCanceledThisBlock(0),
        schedulerReleaseControlsAppliedThisBlock(0),
        schedulerRenderSplitsThisBlock(0),
        overloadState(0),
        consecutiveOverloadBlocks(0), runtimeReloadCount(0),
        accurateClockResetCount(0), schedulerStatePreservedCount(0),
        lastRestartReason(0), accurateHardOverloadEntries(0),
        accurateHardOverloadRecoveries(0), accurateWorkerBlockedCount(0),
        accuratePeakPendingEvents(0), accuratePeakDeferredEvents(0),
        accuratePeakScheduledEvents(0), perfCountersEnabled(0),
        tsfHelperContiguousBlocks(0), tsfHelperGatherBlocks(0),
        tsfHelperComplexBlocks(0), tsfClusteredVoicesContiguous(0),
        tsfClusteredVoicesGather(0), tsfClusteredVoicesComplex(0),
        tsfSingleThreadFragments(0), tsfThreadedFragments(0),
        schedulerCacheRebuilds(0), schedulerTrimHeapTombstonePrunes(0),
        midiProcessMs(0.0f),
        voiceStartMs(0.0f), sampleRenderMs(0.0f), schedulerSliceMs(0.0f),
        schedulerLagMs(0.0f), schedulerBlockStartSample(0) {
    for (int i = 0; i < 16; ++i)
      pitchBendRange[i] = 0.0f;
  }
};

class ISamplerEngine {
public:
  virtual ~ISamplerEngine() {}

  virtual bool Initialize(const SamplerInitParams &params) = 0;
  virtual void Shutdown(bool waitForThreads) = 0;
  virtual void Reset() = 0;
  virtual void ReloadRuntimeSettings(const RuntimeSettings &settings) = 0;
  virtual void SetRealtimePressure(unsigned int overloadState,
                                   unsigned int schedulerLagState,
                                   unsigned int cadenceStreak,
                                   unsigned int scheduledPendingEvents) {}
  virtual void BeginRenderBlock() = 0;
  virtual void EndRenderBlock() {}
  virtual void ProcessMidiEvent(const MidiEvent &event) = 0;
  virtual void Render(float *output, int numFrames) = 0;
  virtual std::string GetResolvedSourcePath() const = 0;
  virtual std::string GetResolvedSourceFormat() const = 0;
  virtual std::string GetEngineName() const = 0;
  virtual DWORD GetActiveVoiceStats(DWORD *channelCounts, int count) const = 0;
  virtual SamplerDiagnostics GetDiagnostics() const = 0;
};

#endif
