#ifndef LIVE_CONFIG_PROTOCOL_H
#define LIVE_CONFIG_PROTOCOL_H

#include <windows.h>

#define SVMS_LIVE_BRIDGE_MAGIC 0x53564D53u
#define SVMS_LIVE_BRIDGE_VERSION 22u
#define SVMS_LIVE_BRIDGE_MAPPING_NAME "Local\\SVMS_LiveBridge_v22"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME "Local\\SVMS_LiveBridgeMutex_v22"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V21 "Local\\SVMS_LiveBridge_v21"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V21 "Local\\SVMS_LiveBridgeMutex_v21"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V20 "Local\\SVMS_LiveBridge_v20"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V20 "Local\\SVMS_LiveBridgeMutex_v20"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V19 "Local\\SVMS_LiveBridge_v19"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V19 "Local\\SVMS_LiveBridgeMutex_v19"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V18 "Local\\SVMS_LiveBridge_v18"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V18 "Local\\SVMS_LiveBridgeMutex_v18"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V17 "Local\\SVMS_LiveBridge_v17"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V17 "Local\\SVMS_LiveBridgeMutex_v17"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V16 "Local\\SVMS_LiveBridge_v16"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V16 "Local\\SVMS_LiveBridgeMutex_v16"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V15 "Local\\SVMS_LiveBridge_v15"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V15 "Local\\SVMS_LiveBridgeMutex_v15"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V14 "Local\\SVMS_LiveBridge_v14"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V14 "Local\\SVMS_LiveBridgeMutex_v14"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V13 "Local\\SVMS_LiveBridge_v13"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V13 "Local\\SVMS_LiveBridgeMutex_v13"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V12 "Local\\SVMS_LiveBridge_v12"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V12 "Local\\SVMS_LiveBridgeMutex_v12"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V11 "Local\\SVMS_LiveBridge_v11"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V11 "Local\\SVMS_LiveBridgeMutex_v11"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V10 "Local\\SVMS_LiveBridge_v10"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V10 "Local\\SVMS_LiveBridgeMutex_v10"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V9 "Local\\SVMS_LiveBridge_v9"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V9 "Local\\SVMS_LiveBridgeMutex_v9"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V8 "Local\\SVMS_LiveBridge_v8"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V8 "Local\\SVMS_LiveBridgeMutex_v8"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V7 "Local\\SVMS_LiveBridge_v7"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V7 "Local\\SVMS_LiveBridgeMutex_v7"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V6 "Local\\SVMS_LiveBridge_v6"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V6 "Local\\SVMS_LiveBridgeMutex_v6"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V5 "Local\\SVMS_LiveBridge_v5"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V5 "Local\\SVMS_LiveBridgeMutex_v5"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V4 "Local\\SVMS_LiveBridge_v4"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V4 "Local\\SVMS_LiveBridgeMutex_v4"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V3 "Local\\SVMS_LiveBridge_v3"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V3 "Local\\SVMS_LiveBridgeMutex_v3"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V2 "Local\\SVMS_LiveBridge_v2"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V2 "Local\\SVMS_LiveBridgeMutex_v2"
#define SVMS_LIVE_BRIDGE_MAPPING_NAME_V1 "Local\\SVMS_LiveBridge_v1"
#define SVMS_LIVE_BRIDGE_MUTEX_NAME_V1 "Local\\SVMS_LiveBridgeMutex_v1"

#define SVMS_MAX_STATUS_TEXT 256
#define SVMS_MAX_BACKEND_TEXT 32
#define SVMS_MAX_PATH_TEXT MAX_PATH

enum LiveBridgeCommandType {
  LIVE_CMD_NONE = 0,
  LIVE_CMD_REFRESH = 1,
  LIVE_CMD_APPLY_SOFT = 2,
  LIVE_CMD_APPLY_HEAVY = 3,
  LIVE_CMD_RESET_ENGINE = 4,
  LIVE_CMD_RELOAD_CONFIG = 5,
  LIVE_CMD_KILL_ENGINE = 6
};

enum LiveBridgeResultCode {
  LIVE_RESULT_NONE = 0,
  LIVE_RESULT_OK = 1,
  LIVE_RESULT_BUSY = 2,
  LIVE_RESULT_FAILED = 3
};

#pragma pack(push, 4)

struct LiveBridgeSettings {
  LONG sampleRate;
  LONG maxVoices;
  LONG pollingRate;
  FLOAT masterVolume;
  FLOAT velocityCurve;
  FLOAT velocityFloor;
  LONG velocityIgnoreBelow;
  LONG asyncNoteStarts;
  LONG wasapiAsyncFeed;
  LONG reverbEnabled;
  FLOAT reverbMix;
  FLOAT reverbFeedback;
  FLOAT reverbTone;
  FLOAT reverbWidth;
  FLOAT reverbBlur;
  LONG limiterEnabled;
  FLOAT limiterThreshold;
  FLOAT limiterReleaseMs;
  CHAR audioBackend[SVMS_MAX_BACKEND_TEXT];
  CHAR samplerEngine[SVMS_MAX_BACKEND_TEXT];
  CHAR eventTimingMode[SVMS_MAX_BACKEND_TEXT];
  CHAR soundfontPath[SVMS_MAX_PATH_TEXT];
};

struct LiveBridgeStats {
  FLOAT synthRenderMs;
  FLOAT synthRenderAvgMs;
  FLOAT synthRenderPeakMs;
  FLOAT audioBlockMs;
  FLOAT audioBlockAvgMs;
  FLOAT audioBlockPeakMs;
  FLOAT audioBudgetMs;
  DWORD audioTimingAgeMs;
  FLOAT midiProcessMs;
  FLOAT voiceStartMs;
  FLOAT sampleRenderMs;
  DWORD totalActiveVoices;
  DWORD activeVoices[16];
  DWORD queuedMidiEvents;
  DWORD deferredMidiEvents;
  DWORD maxQueuedMidiEvents;
  DWORD droppedNoteOnEvents;
  DWORD droppedNonNoteEvents;
  DWORD eventsProcessedThisBlock;
  DWORD noteOnEventsThisBlock;
  DWORD noteOnStartedThisBlock;
  DWORD noteOnDroppedThisBlock;
  DWORD noteOffEventsThisBlock;
  DWORD asyncPendingNoteOns;
  DWORD asyncStartedThisBlock;
  DWORD asyncDroppedThisBlock;
  DWORD asyncCoalescedThisBlock;
  DWORD overloadNoteOnsDroppedThisBlock;
  DWORD staleNoteOnsDroppedThisBlock;
  DWORD preScheduleDropsThisBlock;
  DWORD postScheduleDropsThisBlock;
  DWORD catchupPreventedThisBlock;
  DWORD asyncMaxQueuedNoteOns;
  DWORD asyncQueueAgeMs;
  DWORD asyncLagState;
  DWORD asyncNoteStartsEnabled;
  DWORD eventProcessorThreadActive;
  DWORD wasapiAsyncFeedActive;
  DWORD schedulerSliceFrames;
  DWORD schedulerDueEventsThisBlock;
  DWORD schedulerLateEventsThisBlock;
  DWORD schedulerLagSamples;
  DWORD schedulerPendingSameKeyTransitions;
  DWORD schedulerMaxSameKeyQueueDepth;
  DWORD schedulerNoteOnsCoalescedThisBlock;
  DWORD schedulerNoteOffsAppliedThisBlock;
  DWORD schedulerNoteOffsCoalescedThisBlock;
  DWORD schedulerNoteOffsCanceledThisBlock;
  DWORD schedulerReleaseControlsAppliedThisBlock;
  DWORD schedulerRenderSplitsThisBlock;
  DWORD overloadState;
  DWORD consecutiveOverloadBlocks;
  DWORD samplerWarningCount;
  DWORD samplerLoadedSamples;
  DWORD samplerFailedSamples;
  DWORD runtimeReloadCount;
  DWORD accurateClockResetCount;
  DWORD schedulerStatePreservedCount;
  DWORD lastRestartReason;
  DWORD accurateHardOverloadEntries;
  DWORD accurateHardOverloadRecoveries;
  DWORD accurateWorkerBlockedCount;
  DWORD accuratePeakPendingEvents;
  DWORD accuratePeakDeferredEvents;
  DWORD accuratePeakScheduledEvents;
  DWORD perfCountersEnabled;
  DWORD tsfHelperContiguousBlocks;
  DWORD tsfHelperGatherBlocks;
  DWORD tsfHelperComplexBlocks;
  DWORD tsfClusteredVoicesContiguous;
  DWORD tsfClusteredVoicesGather;
  DWORD tsfClusteredVoicesComplex;
  DWORD tsfSingleThreadFragments;
  DWORD tsfThreadedFragments;
  DWORD schedulerCacheRebuilds;
  DWORD schedulerTrimHeapTombstonePrunes;
  DWORD virtuallySuperExactVoices;
  DWORD virtuallySuperReleasedExactVoices;
  DWORD virtuallySuperGroupedObjects;
  DWORD virtuallySuperDensityObjects;
  DWORD virtuallySuperVoiceEquivalent;
  DWORD virtuallySuperPressureLevel;
  DWORD virtuallySuperLoadedPresets;
  DWORD virtuallySuperLoadedRegions;
  DWORD virtuallySuperExactMode;
  DWORD samplerStateCode;
  DWORD samplerErrorCode;
  FLOAT schedulerSliceMs;
  FLOAT schedulerLagMs;
  ULONGLONG schedulerBlockStartSample;
  FLOAT pitchBendRange[16];
};

struct LiveBridgeSharedState {
  DWORD magic;
  DWORD version;
  DWORD structSize;
  volatile LONG runtimeLoaded;
  DWORD publisherPid;
  DWORD publisherHeartbeatTick;

  LiveBridgeSettings currentSettings;
  LiveBridgeStats currentStats;
  CHAR resolvedAudioBackend[SVMS_MAX_BACKEND_TEXT];
  CHAR resolvedSamplerEngine[SVMS_MAX_BACKEND_TEXT];
  CHAR resolvedSourceFormat[16];
  CHAR resolvedSoundfontPath[SVMS_MAX_PATH_TEXT];
  CHAR samplerLastWarning[SVMS_MAX_STATUS_TEXT];
  CHAR statusText[SVMS_MAX_STATUS_TEXT];

  volatile LONG commandRequestId;
  volatile LONG commandProcessedId;
  volatile LONG commandCode;
  volatile LONG commandInProgress;
  volatile LONG commandResult;
  DWORD commandSourcePid;
  LiveBridgeSettings requestedSettings;
  CHAR commandMessage[SVMS_MAX_STATUS_TEXT];

  DWORD reserved[32];
};

#pragma pack(pop)

#endif
