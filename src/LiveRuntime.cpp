#include "LiveRuntime.h"
#include "AudioOutput.h"
#include "Config.h"
#include "Synth.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#define KDMAPI_ONLYSTRUCTS
#include "OmniMIDI.h"

extern "C" {
extern DebugInfo g_DebugInfo;
}

namespace {

struct PendingCommand {
  LONG requestId;
  LONG commandCode;
  LiveBridgeSettings settings;
};

enum RestartReasonCode {
  RESTART_REASON_NONE = 0,
  RESTART_REASON_APPLY_HEAVY = 1,
  RESTART_REASON_RELOAD_HEAVY = 2,
  RESTART_REASON_HARD_RESET = 3
};

struct SettingsChangeSummary {
  bool hotSafeChanged;
  bool rebuildRequired;

  SettingsChangeSummary() : hotSafeChanged(false), rebuildRequired(false) {}
};

static void CopyCString(char *dest, size_t capacity, const char *src) {
  if (!dest || capacity == 0)
    return;

  if (!src) {
    dest[0] = '\0';
    return;
  }

  size_t length = strlen(src);
  if (length >= capacity)
    length = capacity - 1;
  memcpy(dest, src, length);
  dest[length] = '\0';
}

static float ClampFloatValue(float value, float minValue, float maxValue) {
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

static bool StringsDiffer(const char *a, const char *b) {
  const char *lhs = a ? a : "";
  const char *rhs = b ? b : "";
  return strcmp(lhs, rhs) != 0;
}

static SettingsChangeSummary ClassifySettingsChange(
    const LiveBridgeSettings &before, const LiveBridgeSettings &after) {
  SettingsChangeSummary summary;
  summary.hotSafeChanged =
      before.pollingRate != after.pollingRate ||
      before.masterVolume != after.masterVolume ||
      before.velocityCurve != after.velocityCurve ||
      before.velocityFloor != after.velocityFloor ||
      before.velocityIgnoreBelow != after.velocityIgnoreBelow ||
      before.asyncNoteStarts != after.asyncNoteStarts ||
      before.wasapiAsyncFeed != after.wasapiAsyncFeed ||
      before.reverbEnabled != after.reverbEnabled ||
      before.reverbMix != after.reverbMix ||
      before.reverbFeedback != after.reverbFeedback ||
      before.reverbTone != after.reverbTone ||
      before.reverbWidth != after.reverbWidth ||
      before.reverbBlur != after.reverbBlur ||
      before.limiterEnabled != after.limiterEnabled ||
      before.limiterThreshold != after.limiterThreshold ||
      before.limiterReleaseMs != after.limiterReleaseMs ||
      StringsDiffer(before.eventTimingMode, after.eventTimingMode);

  summary.rebuildRequired =
      before.sampleRate != after.sampleRate ||
      before.maxVoices != after.maxVoices ||
      StringsDiffer(before.audioBackend, after.audioBackend) ||
      StringsDiffer(before.samplerEngine, after.samplerEngine) ||
      StringsDiffer(before.soundfontPath, after.soundfontPath);
  return summary;
}

} // namespace

LiveRuntime &LiveRuntime::Instance() {
  static LiveRuntime instance;
  return instance;
}

LiveRuntime::LiveRuntime()
    : mappingHandle(NULL), bridgeMutexHandle(NULL), stopEvent(NULL),
      workerThread(NULL), sharedState(NULL), initialized(false),
      lastSynthRenderMs(0.0f), averageSynthRenderMs(0.0f),
      peakSynthRenderMs(0.0f), lastAudioBlockMs(0.0f),
      averageAudioBlockMs(0.0f), peakAudioBlockMs(0.0f),
      lastAudioBudgetMs(0.0f), lastAudioTimingTick(0) {
  lastStatusText[0] = '\0';
}

LiveRuntime::~LiveRuntime() { Shutdown(true); }

void LiveRuntime::Initialize() {
  compat::LockGuard<compat::Mutex> lock(stateMutex);
  if (initialized)
    return;

  mappingHandle =
      CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                         sizeof(LiveBridgeSharedState),
                         SVMS_LIVE_BRIDGE_MAPPING_NAME);
  DWORD mappingError = GetLastError();
  if (!mappingHandle) {
    OutputDebugStringA("SVMS: Failed to create live bridge mapping\n");
    return;
  }

  sharedState = static_cast<LiveBridgeSharedState *>(
      MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0,
                    sizeof(LiveBridgeSharedState)));
  if (!sharedState) {
    OutputDebugStringA("SVMS: Failed to map live bridge view\n");
    CloseHandle(mappingHandle);
    mappingHandle = NULL;
    return;
  }

  bridgeMutexHandle = CreateMutexA(NULL, FALSE, SVMS_LIVE_BRIDGE_MUTEX_NAME);
  if (!bridgeMutexHandle) {
    OutputDebugStringA("SVMS: Failed to create live bridge mutex\n");
    UnmapViewOfFile(sharedState);
    sharedState = NULL;
    CloseHandle(mappingHandle);
    mappingHandle = NULL;
    return;
  }

  stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (!stopEvent) {
    OutputDebugStringA("SVMS: Failed to create live bridge stop event\n");
    CloseHandle(bridgeMutexHandle);
    bridgeMutexHandle = NULL;
    UnmapViewOfFile(sharedState);
    sharedState = NULL;
    CloseHandle(mappingHandle);
    mappingHandle = NULL;
    return;
  }

  bool shouldReset =
      (mappingError != ERROR_ALREADY_EXISTS || sharedState->magic !=
                                                   SVMS_LIVE_BRIDGE_MAGIC ||
       sharedState->version != SVMS_LIVE_BRIDGE_VERSION ||
       sharedState->structSize != sizeof(LiveBridgeSharedState));
  if (shouldReset)
    memset(sharedState, 0, sizeof(LiveBridgeSharedState));

  if (LockBridge(100)) {
    sharedState->magic = SVMS_LIVE_BRIDGE_MAGIC;
    sharedState->version = SVMS_LIVE_BRIDGE_VERSION;
    sharedState->structSize = sizeof(LiveBridgeSharedState);
    sharedState->runtimeLoaded = 1;
    sharedState->publisherPid = GetCurrentProcessId();
    sharedState->publisherHeartbeatTick = GetTickCount();
    sharedState->commandRequestId = 0;
    sharedState->commandProcessedId = 0;
    sharedState->commandCode = LIVE_CMD_NONE;
    sharedState->commandInProgress = 0;
    sharedState->commandResult = LIVE_RESULT_NONE;
    sharedState->commandSourcePid = 0;
    CopyCString(sharedState->commandMessage,
                sizeof(sharedState->commandMessage), "Idle");
    WriteStatusLocked("Runtime initialized");
    CopyCString(sharedState->statusText, sizeof(sharedState->statusText),
                "Runtime initialized");
    UnlockBridge();
  }

  ResetAudioTimings();
  workerThread = CreateThread(NULL, 0, &LiveRuntime::WorkerThreadProc, this, 0,
                              NULL);
  if (!workerThread) {
    OutputDebugStringA("SVMS: Failed to create live bridge worker thread\n");
    CloseHandle(stopEvent);
    stopEvent = NULL;
    CloseHandle(bridgeMutexHandle);
    bridgeMutexHandle = NULL;
    UnmapViewOfFile(sharedState);
    sharedState = NULL;
    CloseHandle(mappingHandle);
    mappingHandle = NULL;
    return;
  }

  initialized = true;
}

void LiveRuntime::Shutdown(bool waitForThread) {
  if (!waitForThread) {
    if (stopEvent)
      SetEvent(stopEvent);

    if (sharedState && LockBridge(25)) {
      if (sharedState->publisherPid == GetCurrentProcessId()) {
        sharedState->runtimeLoaded = 0;
        sharedState->publisherPid = 0;
        sharedState->publisherHeartbeatTick = 0;
        CopyCString(sharedState->statusText, sizeof(sharedState->statusText),
                    "Runtime stopping");
      }
      UnlockBridge();
    }
    initialized = false;
    return;
  }

  HANDLE localWorker = NULL;
  HANDLE localStopEvent = NULL;
  HANDLE localBridgeMutex = NULL;
  HANDLE localMapping = NULL;
  LiveBridgeSharedState *localShared = NULL;

  {
    compat::LockGuard<compat::Mutex> lock(stateMutex);
    if (!mappingHandle && !workerThread && !initialized)
      return;

    localWorker = workerThread;
    localStopEvent = stopEvent;
    localBridgeMutex = bridgeMutexHandle;
    localMapping = mappingHandle;
    localShared = sharedState;

    workerThread = NULL;
    stopEvent = NULL;
    bridgeMutexHandle = NULL;
    mappingHandle = NULL;
    sharedState = NULL;
    initialized = false;
  }

  if (localStopEvent)
    SetEvent(localStopEvent);

  if (localWorker) {
    if (waitForThread)
      WaitForSingleObject(localWorker, INFINITE);
    CloseHandle(localWorker);
  }

  HANDLE previousMutex = bridgeMutexHandle;
  LiveBridgeSharedState *previousShared = sharedState;
  bridgeMutexHandle = localBridgeMutex;
  sharedState = localShared;
  if (localShared && LockBridge(100)) {
    if (localShared->publisherPid == GetCurrentProcessId()) {
      localShared->runtimeLoaded = 0;
      localShared->publisherPid = 0;
      localShared->publisherHeartbeatTick = 0;
      CopyCString(localShared->statusText, sizeof(localShared->statusText),
                  "Runtime stopped");
    }
    UnlockBridge();
  }
  bridgeMutexHandle = previousMutex;
  sharedState = previousShared;

  if (localStopEvent)
    CloseHandle(localStopEvent);
  if (localBridgeMutex)
    CloseHandle(localBridgeMutex);
  if (localShared)
    UnmapViewOfFile(localShared);
  if (localMapping)
    CloseHandle(localMapping);
}

void LiveRuntime::UpdateAudioTimings(float synthRenderMs, float audioBlockMs,
                                     float audioBudgetMs) {
  compat::LockGuard<compat::Mutex> lock(timingMutex);
  lastSynthRenderMs = synthRenderMs;
  lastAudioBlockMs = audioBlockMs;
  if (averageSynthRenderMs <= 0.0001f)
    averageSynthRenderMs = synthRenderMs;
  else
    averageSynthRenderMs = averageSynthRenderMs * 0.85f + synthRenderMs * 0.15f;
  if (averageAudioBlockMs <= 0.0001f)
    averageAudioBlockMs = audioBlockMs;
  else
    averageAudioBlockMs = averageAudioBlockMs * 0.85f + audioBlockMs * 0.15f;
  peakSynthRenderMs = (std::max)(synthRenderMs, peakSynthRenderMs * 0.97f);
  peakAudioBlockMs = (std::max)(audioBlockMs, peakAudioBlockMs * 0.97f);
  lastAudioBudgetMs = audioBudgetMs;
  lastAudioTimingTick = GetTickCount();
}

void LiveRuntime::PublishStatus(const char *statusText) {
  {
    compat::LockGuard<compat::Mutex> lock(stateMutex);
    CopyCString(lastStatusText, sizeof(lastStatusText), statusText);
  }

  if (sharedState && LockBridge(25)) {
    CopyCString(sharedState->statusText, sizeof(sharedState->statusText),
                statusText);
    UnlockBridge();
  }
}

void LiveRuntime::PublishError(const char *statusText) {
  PublishStatus(statusText);
}

void LiveRuntime::ResetAudioTimings() {
  compat::LockGuard<compat::Mutex> lock(timingMutex);
  lastSynthRenderMs = 0.0f;
  averageSynthRenderMs = 0.0f;
  peakSynthRenderMs = 0.0f;
  lastAudioBlockMs = 0.0f;
  averageAudioBlockMs = 0.0f;
  peakAudioBlockMs = 0.0f;
  lastAudioBudgetMs = 0.0f;
  lastAudioTimingTick = 0;
}

DWORD WINAPI LiveRuntime::WorkerThreadProc(LPVOID param) {
  static_cast<LiveRuntime *>(param)->WorkerLoop();
  return 0;
}

void LiveRuntime::WorkerLoop() {
  PublishSnapshot();
  while (WaitForSingleObject(stopEvent, 16) == WAIT_TIMEOUT) {
    ProcessPendingCommand();
    PublishSnapshot();
  }
}

void LiveRuntime::PublishSnapshot() {
  if (!sharedState)
    return;

  LiveBridgeSettings settings;
  memset(&settings, 0, sizeof(settings));
  PopulateCurrentSettings(settings);

  DWORD activeVoices[16] = {0};
  DWORD totalActiveVoices =
      Synth::Instance().GetActiveVoiceStats(activeVoices, 16);
  std::string resolvedBackend = AudioOutput::Instance().GetResolvedBackendName();
  std::string resolvedSoundfont = Synth::Instance().GetResolvedSoundfontPath();
  std::string resolvedSamplerEngine =
      Synth::Instance().GetResolvedSamplerEngineName();
  std::string resolvedSourceFormat = Synth::Instance().GetResolvedSourceFormat();
  SamplerDiagnostics samplerDiagnostics =
      Synth::Instance().GetSamplerDiagnostics();
  samplerDiagnostics.wasapiAsyncFeedActive =
      AudioOutput::Instance().IsWasapiAsyncFeedActive() ? 1u : 0u;

  float synthRenderMs = 0.0f;
  float synthRenderAvgMs = 0.0f;
  float synthRenderPeakMs = 0.0f;
  float audioBlockMs = 0.0f;
  float audioBlockAvgMs = 0.0f;
  float audioBlockPeakMs = 0.0f;
  float audioBudgetMs = 0.0f;
  DWORD audioTimingAgeMs = 0;
  {
    compat::LockGuard<compat::Mutex> lock(timingMutex);
    synthRenderMs = lastSynthRenderMs;
    synthRenderAvgMs = averageSynthRenderMs;
    synthRenderPeakMs = peakSynthRenderMs;
    audioBlockMs = lastAudioBlockMs;
    audioBlockAvgMs = averageAudioBlockMs;
    audioBlockPeakMs = peakAudioBlockMs;
    audioBudgetMs = lastAudioBudgetMs;
    if (lastAudioTimingTick != 0 && audioBudgetMs > 0.0f) {
      audioTimingAgeMs = GetTickCount() - lastAudioTimingTick;
    }
  }

  char statusText[SVMS_MAX_STATUS_TEXT];
  {
    compat::LockGuard<compat::Mutex> lock(stateMutex);
    CopyCString(statusText, sizeof(statusText), lastStatusText);
  }

  if (!LockBridge(25))
    return;

  sharedState->magic = SVMS_LIVE_BRIDGE_MAGIC;
  sharedState->version = SVMS_LIVE_BRIDGE_VERSION;
  sharedState->structSize = sizeof(LiveBridgeSharedState);
  sharedState->runtimeLoaded = 1;
  sharedState->publisherPid = GetCurrentProcessId();
  sharedState->publisherHeartbeatTick = GetTickCount();
  sharedState->currentSettings = settings;
  sharedState->currentStats.synthRenderMs = synthRenderMs;
  sharedState->currentStats.synthRenderAvgMs = synthRenderAvgMs;
  sharedState->currentStats.synthRenderPeakMs = synthRenderPeakMs;
  sharedState->currentStats.audioBlockMs = audioBlockMs;
  sharedState->currentStats.audioBlockAvgMs = audioBlockAvgMs;
  sharedState->currentStats.audioBlockPeakMs = audioBlockPeakMs;
  sharedState->currentStats.audioBudgetMs = audioBudgetMs;
  sharedState->currentStats.audioTimingAgeMs = audioTimingAgeMs;
  sharedState->currentStats.midiProcessMs = samplerDiagnostics.midiProcessMs;
  sharedState->currentStats.voiceStartMs = samplerDiagnostics.voiceStartMs;
  sharedState->currentStats.sampleRenderMs = samplerDiagnostics.sampleRenderMs;
  sharedState->currentStats.totalActiveVoices = totalActiveVoices;
  for (int i = 0; i < 16; ++i)
    sharedState->currentStats.activeVoices[i] = activeVoices[i];
  sharedState->currentStats.queuedMidiEvents =
      samplerDiagnostics.queuedMidiEvents;
  sharedState->currentStats.deferredMidiEvents =
      samplerDiagnostics.deferredMidiEvents;
  sharedState->currentStats.criticalQueueDepth =
      samplerDiagnostics.criticalQueueDepth;
  sharedState->currentStats.realtimeQueueDepth =
      samplerDiagnostics.realtimeQueueDepth;
  sharedState->currentStats.noteOnQueueDepth =
      samplerDiagnostics.noteOnQueueDepth;
  sharedState->currentStats.releaseLaneDepth =
      samplerDiagnostics.releaseLaneDepth;
  sharedState->currentStats.maxQueuedMidiEvents =
      samplerDiagnostics.maxQueuedMidiEvents;
  sharedState->currentStats.droppedNoteOnEvents =
      samplerDiagnostics.droppedNoteOnEvents;
  sharedState->currentStats.droppedNonNoteEvents =
      samplerDiagnostics.droppedNonNoteEvents;
  sharedState->currentStats.eventsProcessedThisBlock =
      samplerDiagnostics.eventsProcessedThisBlock;
  sharedState->currentStats.noteOnEventsThisBlock =
      samplerDiagnostics.noteOnEventsThisBlock;
  sharedState->currentStats.noteOnStartedThisBlock =
      samplerDiagnostics.noteOnStartedThisBlock;
  sharedState->currentStats.noteOnDroppedThisBlock =
      samplerDiagnostics.noteOnDroppedThisBlock;
  sharedState->currentStats.noteOffEventsThisBlock =
      samplerDiagnostics.noteOffEventsThisBlock;
  sharedState->currentStats.noteOffIngressThisBlock =
      samplerDiagnostics.noteOffIngressThisBlock;
  sharedState->currentStats.noteOffDeferredThisBlock =
      samplerDiagnostics.noteOffDeferredThisBlock;
  sharedState->currentStats.noteOffReleaseLaneQueuedThisBlock =
      samplerDiagnostics.noteOffReleaseLaneQueuedThisBlock;
  sharedState->currentStats.noteOffReleaseLaneAppliedThisBlock =
      samplerDiagnostics.noteOffReleaseLaneAppliedThisBlock;
  sharedState->currentStats.noteOffLateThisBlock =
      samplerDiagnostics.noteOffLateThisBlock;
  sharedState->currentStats.asyncPendingNoteOns =
      samplerDiagnostics.asyncPendingNoteOns;
  sharedState->currentStats.asyncStartedThisBlock =
      samplerDiagnostics.asyncStartedThisBlock;
  sharedState->currentStats.asyncDroppedThisBlock =
      samplerDiagnostics.asyncDroppedThisBlock;
  sharedState->currentStats.asyncCoalescedThisBlock =
      samplerDiagnostics.asyncCoalescedThisBlock;
  sharedState->currentStats.overloadNoteOnsDroppedThisBlock =
      samplerDiagnostics.overloadNoteOnsDroppedThisBlock;
  sharedState->currentStats.staleNoteOnsDroppedThisBlock =
      samplerDiagnostics.staleNoteOnsDroppedThisBlock;
  sharedState->currentStats.preScheduleDropsThisBlock =
      samplerDiagnostics.preScheduleDropsThisBlock;
  sharedState->currentStats.postScheduleDropsThisBlock =
      samplerDiagnostics.postScheduleDropsThisBlock;
  sharedState->currentStats.catchupPreventedThisBlock =
      samplerDiagnostics.catchupPreventedThisBlock;
  sharedState->currentStats.asyncMaxQueuedNoteOns =
      samplerDiagnostics.asyncMaxQueuedNoteOns;
  sharedState->currentStats.asyncQueueAgeMs =
      samplerDiagnostics.asyncQueueAgeMs;
  sharedState->currentStats.asyncLagState = samplerDiagnostics.asyncLagState;
  sharedState->currentStats.asyncNoteStartsEnabled =
      samplerDiagnostics.asyncNoteStartsEnabled;
  sharedState->currentStats.eventProcessorThreadActive =
      samplerDiagnostics.eventProcessorThreadActive;
  sharedState->currentStats.wasapiAsyncFeedActive =
      samplerDiagnostics.wasapiAsyncFeedActive;
  sharedState->currentStats.schedulerSliceFrames =
      samplerDiagnostics.schedulerSliceFrames;
  sharedState->currentStats.schedulerDueEventsThisBlock =
      samplerDiagnostics.schedulerDueEventsThisBlock;
  sharedState->currentStats.schedulerLateEventsThisBlock =
      samplerDiagnostics.schedulerLateEventsThisBlock;
  sharedState->currentStats.schedulerLagSamples =
      samplerDiagnostics.schedulerLagSamples;
  sharedState->currentStats.schedulerPendingSameKeyTransitions =
      samplerDiagnostics.schedulerPendingSameKeyTransitions;
  sharedState->currentStats.schedulerMaxSameKeyQueueDepth =
      samplerDiagnostics.schedulerMaxSameKeyQueueDepth;
  sharedState->currentStats.schedulerNoteOnsCoalescedThisBlock =
      samplerDiagnostics.schedulerNoteOnsCoalescedThisBlock;
  sharedState->currentStats.schedulerNoteOffsAppliedThisBlock =
      samplerDiagnostics.schedulerNoteOffsAppliedThisBlock;
  sharedState->currentStats.schedulerNoteOffsCoalescedThisBlock =
      samplerDiagnostics.schedulerNoteOffsCoalescedThisBlock;
  sharedState->currentStats.schedulerNoteOffsCanceledThisBlock =
      samplerDiagnostics.schedulerNoteOffsCanceledThisBlock;
  sharedState->currentStats.schedulerReleaseControlsAppliedThisBlock =
      samplerDiagnostics.schedulerReleaseControlsAppliedThisBlock;
  sharedState->currentStats.schedulerRenderSplitsThisBlock =
      samplerDiagnostics.schedulerRenderSplitsThisBlock;
  sharedState->currentStats.overloadState = samplerDiagnostics.overloadState;
  sharedState->currentStats.consecutiveOverloadBlocks =
      samplerDiagnostics.consecutiveOverloadBlocks;
  sharedState->currentStats.samplerWarningCount =
      samplerDiagnostics.warningCount;
  sharedState->currentStats.samplerLoadedSamples =
      samplerDiagnostics.loadedSampleCount;
  sharedState->currentStats.samplerFailedSamples =
      samplerDiagnostics.failedSampleCount;
  sharedState->currentStats.runtimeReloadCount =
      samplerDiagnostics.runtimeReloadCount;
  sharedState->currentStats.accurateClockResetCount =
      samplerDiagnostics.accurateClockResetCount;
  sharedState->currentStats.schedulerStatePreservedCount =
      samplerDiagnostics.schedulerStatePreservedCount;
  sharedState->currentStats.lastRestartReason =
      samplerDiagnostics.lastRestartReason;
  sharedState->currentStats.accurateHardOverloadEntries =
      samplerDiagnostics.accurateHardOverloadEntries;
  sharedState->currentStats.accurateHardOverloadRecoveries =
      samplerDiagnostics.accurateHardOverloadRecoveries;
  sharedState->currentStats.accurateWorkerBlockedCount =
      samplerDiagnostics.accurateWorkerBlockedCount;
  sharedState->currentStats.accuratePeakPendingEvents =
      samplerDiagnostics.accuratePeakPendingEvents;
  sharedState->currentStats.accuratePeakDeferredEvents =
      samplerDiagnostics.accuratePeakDeferredEvents;
  sharedState->currentStats.accuratePeakScheduledEvents =
      samplerDiagnostics.accuratePeakScheduledEvents;
  sharedState->currentStats.perfCountersEnabled =
      samplerDiagnostics.perfCountersEnabled;
  sharedState->currentStats.tsfHelperContiguousBlocks =
      samplerDiagnostics.tsfHelperContiguousBlocks;
  sharedState->currentStats.tsfHelperGatherBlocks =
      samplerDiagnostics.tsfHelperGatherBlocks;
  sharedState->currentStats.tsfHelperComplexBlocks =
      samplerDiagnostics.tsfHelperComplexBlocks;
  sharedState->currentStats.tsfClusteredVoicesContiguous =
      samplerDiagnostics.tsfClusteredVoicesContiguous;
  sharedState->currentStats.tsfClusteredVoicesGather =
      samplerDiagnostics.tsfClusteredVoicesGather;
  sharedState->currentStats.tsfClusteredVoicesComplex =
      samplerDiagnostics.tsfClusteredVoicesComplex;
  sharedState->currentStats.tsfSingleThreadFragments =
      samplerDiagnostics.tsfSingleThreadFragments;
  sharedState->currentStats.tsfThreadedFragments =
      samplerDiagnostics.tsfThreadedFragments;
  sharedState->currentStats.schedulerCacheRebuilds =
      samplerDiagnostics.schedulerCacheRebuilds;
  sharedState->currentStats.schedulerTrimHeapTombstonePrunes =
      samplerDiagnostics.schedulerTrimHeapTombstonePrunes;
  sharedState->currentStats.virtuallySuperExactVoices =
      samplerDiagnostics.virtuallySuperExactVoices;
  sharedState->currentStats.virtuallySuperReleasedExactVoices =
      samplerDiagnostics.virtuallySuperReleasedExactVoices;
  sharedState->currentStats.virtuallySuperGroupedObjects =
      samplerDiagnostics.virtuallySuperGroupedObjects;
  sharedState->currentStats.virtuallySuperDensityObjects =
      samplerDiagnostics.virtuallySuperDensityObjects;
  sharedState->currentStats.virtuallySuperVoiceEquivalent =
      samplerDiagnostics.virtuallySuperVoiceEquivalent;
  sharedState->currentStats.virtuallySuperPressureLevel =
      samplerDiagnostics.virtuallySuperPressureLevel;
  sharedState->currentStats.virtuallySuperLoadedPresets =
      samplerDiagnostics.virtuallySuperLoadedPresets;
  sharedState->currentStats.virtuallySuperLoadedRegions =
      samplerDiagnostics.virtuallySuperLoadedRegions;
  sharedState->currentStats.virtuallySuperExactMode =
      samplerDiagnostics.virtuallySuperExactMode;
  sharedState->currentStats.samplerStateCode =
      samplerDiagnostics.samplerStateCode;
  sharedState->currentStats.samplerErrorCode =
      samplerDiagnostics.samplerErrorCode;
  sharedState->currentStats.schedulerSliceMs =
      samplerDiagnostics.schedulerSliceMs;
  sharedState->currentStats.schedulerLagMs =
      samplerDiagnostics.schedulerLagMs;
  sharedState->currentStats.schedulerBlockStartSample =
      samplerDiagnostics.schedulerBlockStartSample;
  for (int i = 0; i < 16; ++i)
    sharedState->currentStats.pitchBendRange[i] =
        samplerDiagnostics.pitchBendRange[i];
  CopyCString(sharedState->resolvedAudioBackend,
              sizeof(sharedState->resolvedAudioBackend),
              resolvedBackend.c_str());
  CopyCString(sharedState->resolvedSamplerEngine,
              sizeof(sharedState->resolvedSamplerEngine),
              resolvedSamplerEngine.c_str());
  CopyCString(sharedState->resolvedSourceFormat,
              sizeof(sharedState->resolvedSourceFormat),
              resolvedSourceFormat.c_str());
  CopyCString(sharedState->resolvedSoundfontPath,
              sizeof(sharedState->resolvedSoundfontPath),
              resolvedSoundfont.c_str());
  CopyCString(sharedState->samplerLastWarning,
              sizeof(sharedState->samplerLastWarning),
              samplerDiagnostics.lastWarning.c_str());
  CopyCString(sharedState->statusText, sizeof(sharedState->statusText),
              statusText);

  g_DebugInfo.RenderingTime = synthRenderMs;
  for (int i = 0; i < 16; ++i)
    g_DebugInfo.ActiveVoices[i] = activeVoices[i];

  UnlockBridge();
}

void LiveRuntime::ProcessPendingCommand() {
  if (!sharedState)
    return;

  PendingCommand pending;
  memset(&pending, 0, sizeof(pending));

  if (!LockBridge(25))
    return;

  if (sharedState->publisherPid != GetCurrentProcessId() ||
      sharedState->commandRequestId == 0 ||
      sharedState->commandProcessedId == sharedState->commandRequestId ||
      sharedState->commandInProgress ||
      sharedState->commandSourcePid == 0 ||
      sharedState->commandSourcePid == GetCurrentProcessId()) {
    UnlockBridge();
    return;
  }

  pending.requestId = sharedState->commandRequestId;
  pending.commandCode = sharedState->commandCode;
  pending.settings = sharedState->requestedSettings;
  sharedState->commandInProgress = 1;
  sharedState->commandResult = LIVE_RESULT_BUSY;
  CopyCString(sharedState->commandMessage, sizeof(sharedState->commandMessage),
              "Processing command...");
  UnlockBridge();

  char message[SVMS_MAX_STATUS_TEXT];
  memset(message, 0, sizeof(message));
  bool ok =
      ExecuteCommand(pending.commandCode, pending.settings, message,
                     sizeof(message));

  if (LockBridge(100)) {
    if (sharedState->commandRequestId == pending.requestId) {
      sharedState->commandProcessedId = pending.requestId;
      sharedState->commandResult = ok ? LIVE_RESULT_OK : LIVE_RESULT_FAILED;
      sharedState->commandInProgress = 0;
      CopyCString(sharedState->commandMessage,
                  sizeof(sharedState->commandMessage), message);
      CopyCString(sharedState->statusText, sizeof(sharedState->statusText),
                  message);
    }
    UnlockBridge();
  }

  PublishStatus(message);
}

bool LiveRuntime::ExecuteCommand(LONG commandCode,
                                 const LiveBridgeSettings &settings,
                                 char *message, size_t messageCapacity) {
  switch (commandCode) {
  case LIVE_CMD_REFRESH:
    CopyMessage(message, messageCapacity, "Status refreshed");
    return true;
  case LIVE_CMD_APPLY_SOFT:
    return ApplySettings(settings, true, false, message, messageCapacity);
  case LIVE_CMD_APPLY_HEAVY:
    return ApplySettings(settings, true, true, message, messageCapacity);
  case LIVE_CMD_RESET_ENGINE:
    return RestartEngine(true, RESTART_REASON_HARD_RESET, "hard-reset", message,
                         messageCapacity);
  case LIVE_CMD_RELOAD_CONFIG:
    return ReloadConfig(message, messageCapacity);
  case LIVE_CMD_KILL_ENGINE:
    return KillEngine(message, messageCapacity);
  default:
    CopyMessage(message, messageCapacity, "Unknown command");
    return false;
  }
}

bool LiveRuntime::ApplySettings(const LiveBridgeSettings &settings,
                                bool includeSoft, bool includeHeavy,
                                char *message, size_t messageCapacity) {
  LiveBridgeSettings previousSettings;
  memset(&previousSettings, 0, sizeof(previousSettings));
  PopulateCurrentSettings(previousSettings);
  bool ok = true;
  if (includeSoft) {
    ok = ok && Config::Instance().SetFloat(
                   "master_volume",
                   ClampFloatValue(settings.masterVolume, 0.0f, 4.0f));
    ok = ok && Config::Instance().SetFloat(
                   "velocity_curve",
                   ClampFloatValue(settings.velocityCurve, 0.25f, 6.0f));
    ok = ok && Config::Instance().SetFloat(
                   "velocity_floor",
                   ClampFloatValue(settings.velocityFloor, 0.0f, 0.5f));
    ok = ok && Config::Instance().SetInt(
                   "velocity_ignore_below",
                   settings.velocityIgnoreBelow < 0
                       ? 0
                       : (settings.velocityIgnoreBelow > 126
                              ? 126
                              : settings.velocityIgnoreBelow));
    ok = ok && Config::Instance().SetBool("async_note_starts",
                                          settings.asyncNoteStarts != 0);
    ok = ok && Config::Instance().SetBool("wasapi_async_feed",
                                          settings.wasapiAsyncFeed != 0);
    ok = ok && Config::Instance().SetString("event_timing_mode",
                                            settings.eventTimingMode);
    ok = ok && Config::Instance().SetBool("reverb_enable",
                                          settings.reverbEnabled != 0);
    ok = ok && Config::Instance().SetFloat(
                   "reverb_mix", ClampFloatValue(settings.reverbMix, 0.0f, 1.0f));
    ok = ok && Config::Instance().SetFloat(
                   "reverb_feedback",
                   ClampFloatValue(settings.reverbFeedback, 0.0f, 0.97f));
    ok = ok && Config::Instance().SetFloat(
                   "reverb_tone", ClampFloatValue(settings.reverbTone, 0.02f, 0.95f));
    ok = ok && Config::Instance().SetFloat(
                   "reverb_width",
                   ClampFloatValue(settings.reverbWidth, 0.0f, 1.0f));
    ok = ok && Config::Instance().SetFloat(
                   "reverb_blur", ClampFloatValue(settings.reverbBlur, 0.0f, 1.0f));
    ok = ok && Config::Instance().SetBool("limiter_enable",
                                          settings.limiterEnabled != 0);
    ok = ok && Config::Instance().SetFloat(
                   "limiter_threshold",
                   ClampFloatValue(settings.limiterThreshold, 0.1f, 1.0f));
    ok = ok && Config::Instance().SetFloat(
                   "limiter_release_ms",
                   ClampFloatValue(settings.limiterReleaseMs, 5.0f, 500.0f));
    ok = ok &&
         Config::Instance().SetInt("polling_rate",
                                   settings.pollingRate < 0 ? 0
                                                            : settings.pollingRate);
  }

  if (includeHeavy) {
    ok = ok && Config::Instance().SetInt(
                   "sample_rate", settings.sampleRate < 8000 ? 8000
                                                              : settings.sampleRate);
    ok = ok && Config::Instance().SetInt(
                   "max_voices", settings.maxVoices < 1 ? 1 : settings.maxVoices);
    ok = ok && Config::Instance().SetString("audio_backend",
                                            settings.audioBackend);
    ok = ok && Config::Instance().SetString("sampler_engine",
                                            settings.samplerEngine);
    ok = ok &&
         Config::Instance().SetString("sound_source", settings.soundfontPath);
    ok = ok && Config::Instance().SetString("soundfont", settings.soundfontPath);
  }

  if (!ok || !Config::Instance().Save()) {
    CopyMessage(message, messageCapacity, "Failed to save config");
    return false;
  }

  LiveBridgeSettings currentSettings;
  memset(&currentSettings, 0, sizeof(currentSettings));
  PopulateCurrentSettings(currentSettings);
  SettingsChangeSummary summary =
      ClassifySettingsChange(previousSettings, currentSettings);

  if (summary.rebuildRequired || includeHeavy) {
    OutputDebugStringA(
        "SVMS: Live apply classified as rebuild-required\n");
    return RestartEngine(true, RESTART_REASON_APPLY_HEAVY, "apply-heavy",
                         message, messageCapacity);
  }

  Config::Instance().ForceReload();
  AudioOutput::Instance().RequestRuntimeConfigRefresh();
  OutputDebugStringA("SVMS: Live apply classified as hot-safe runtime refresh\n");
  CopyMessage(message, messageCapacity, summary.hotSafeChanged
                                           ? "Hot-safe settings applied"
                                           : "No live runtime changes detected");
  return true;
}

bool LiveRuntime::ReloadConfig(char *message, size_t messageCapacity) {
  LiveBridgeSettings previousSettings;
  memset(&previousSettings, 0, sizeof(previousSettings));
  PopulateCurrentSettings(previousSettings);

  Config::Instance().ForceReload();

  LiveBridgeSettings currentSettings;
  memset(&currentSettings, 0, sizeof(currentSettings));
  PopulateCurrentSettings(currentSettings);
  SettingsChangeSummary summary =
      ClassifySettingsChange(previousSettings, currentSettings);

  if (summary.rebuildRequired) {
    OutputDebugStringA(
        "SVMS: Reload config classified as rebuild-required\n");
    return RestartEngine(true, RESTART_REASON_RELOAD_HEAVY, "reload-heavy",
                         message, messageCapacity);
  }

  AudioOutput::Instance().RequestRuntimeConfigRefresh();
  OutputDebugStringA(
      "SVMS: Reload config classified as hot-safe runtime refresh\n");
  CopyMessage(message, messageCapacity,
              summary.hotSafeChanged ? "Config reloaded live"
                                     : "Config reloaded; no runtime changes");
  return true;
}

bool LiveRuntime::RestartEngine(bool reloadConfig, unsigned int reasonCode,
                                const char *reasonText, char *message,
                                size_t messageCapacity) {
  int audioRefs = AudioOutput::Instance().GetRefCount();
  int synthRefs = Synth::Instance().GetRefCount();

  char reasonBuf[192];
  sprintf(reasonBuf, "SVMS: RestartEngine reason=%s reloadConfig=%u\n",
          reasonText ? reasonText : "unknown", reloadConfig ? 1u : 0u);
  OutputDebugStringA(reasonBuf);

  for (int i = 0; i < audioRefs; ++i)
    AudioOutput::Instance().Stop();
  for (int i = 0; i < synthRefs; ++i)
    Synth::Instance().Shutdown();

  if (reloadConfig)
    Config::Instance().ForceReload();

  for (int i = 0; i < synthRefs; ++i)
    Synth::Instance().Initialize();

  Synth::Instance().SetRestartReason(reasonCode);

  bool started = true;
  if (Synth::Instance().GetRefCount() != synthRefs)
    started = false;

  if (started) {
    for (int i = 0; i < audioRefs; ++i) {
      if (!AudioOutput::Instance().Start())
        started = false;
    }
  }

  ResetAudioTimings();

  bool ok = (Synth::Instance().GetRefCount() == synthRefs &&
             AudioOutput::Instance().GetRefCount() == audioRefs && started);
  if (ok) {
    char okBuf[224];
    sprintf(okBuf,
            "SVMS: RestartEngine completed reason=%s backend=%s accurate=%u wasapiAsync=%u\n",
            reasonText ? reasonText : "unknown",
            AudioOutput::Instance().GetResolvedBackendName().c_str(),
            ParseEventTimingMode(
                Config::Instance().GetString("event_timing_mode", "accurate")) ==
                    EventTimingMode::ACCURATE
                ? 1u
                : 0u,
            AudioOutput::Instance().IsWasapiAsyncFeedActive() ? 1u : 0u);
    OutputDebugStringA(okBuf);
  }
  CopyMessage(message, messageCapacity,
              ok ? "Engine restarted cleanly" : "Engine restart failed");
  return ok;
}

bool LiveRuntime::KillEngine(char *message, size_t messageCapacity) {
  AudioOutput::Instance().ForceStop(false);
  Synth::Instance().ForceShutdown(false);
  ResetAudioTimings();
  CopyMessage(message, messageCapacity,
              "Engine killed. Reopen or hard-reset the host to start it again.");
  return true;
}

void LiveRuntime::WriteStatusLocked(const char *statusText) {
  CopyCString(lastStatusText, sizeof(lastStatusText), statusText);
}

void LiveRuntime::CopyMessage(char *dest, size_t capacity, const char *text) {
  CopyCString(dest, capacity, text);
}

bool LiveRuntime::LockBridge(DWORD timeoutMs) {
  if (!bridgeMutexHandle)
    return false;
  DWORD result = WaitForSingleObject(bridgeMutexHandle, timeoutMs);
  return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

void LiveRuntime::UnlockBridge() {
  if (bridgeMutexHandle)
    ReleaseMutex(bridgeMutexHandle);
}

void LiveRuntime::PopulateCurrentSettings(LiveBridgeSettings &settings) {
  settings.sampleRate = Config::Instance().GetInt("sample_rate", 44100);
  settings.maxVoices = Config::Instance().GetInt("max_voices", 500);
  settings.pollingRate = Config::Instance().GetInt("polling_rate", 0);
  settings.masterVolume = Config::Instance().GetFloat("master_volume", 1.0f);
  settings.velocityCurve = Config::Instance().GetFloat("velocity_curve", 2.4f);
  settings.velocityFloor = Config::Instance().GetFloat("velocity_floor", 0.0f);
  settings.velocityIgnoreBelow =
      Config::Instance().GetInt("velocity_ignore_below", 0);
  settings.asyncNoteStarts =
      Config::Instance().GetBool("async_note_starts", true) ? 1 : 0;
  settings.wasapiAsyncFeed =
      Config::Instance().GetBool("wasapi_async_feed", true) ? 1 : 0;
  CopyCString(settings.eventTimingMode, sizeof(settings.eventTimingMode),
              Config::Instance()
                  .GetString("event_timing_mode",
                             settings.asyncNoteStarts ? "accurate"
                                                     : "legacy-sync")
                  .c_str());
  settings.reverbEnabled = Config::Instance().GetBool("reverb_enable", false);
  settings.reverbMix = Config::Instance().GetFloat("reverb_mix", 0.18f);
  settings.reverbFeedback =
      Config::Instance().GetFloat("reverb_feedback", 0.72f);
  settings.reverbTone = Config::Instance().GetFloat("reverb_tone", 0.28f);
  settings.reverbWidth = Config::Instance().GetFloat("reverb_width", 0.35f);
  settings.reverbBlur = Config::Instance().GetFloat("reverb_blur", 0.45f);
  settings.limiterEnabled =
      Config::Instance().GetBool("limiter_enable", true);
  settings.limiterThreshold =
      Config::Instance().GetFloat("limiter_threshold", 0.98f);
  settings.limiterReleaseMs =
      Config::Instance().GetFloat("limiter_release_ms", 80.0f);
  CopyCString(settings.audioBackend, sizeof(settings.audioBackend),
              Config::Instance().GetString("audio_backend", "auto").c_str());
  CopyCString(settings.samplerEngine, sizeof(settings.samplerEngine),
              Config::Instance().GetString("sampler_engine", "auto").c_str());
  CopyCString(settings.soundfontPath, sizeof(settings.soundfontPath),
              Config::Instance()
                  .GetString("sound_source",
                             Config::Instance().GetString("soundfont", "gm.sf2"))
                  .c_str());
}
