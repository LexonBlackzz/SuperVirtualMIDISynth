#include "LiveConfigProtocol.h"
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

static const UINT_PTR kLivePollTimerId = 1;
static const UINT kLivePollIntervalMs = 16;
static const float kSafeBudgetFactor = 0.95f;

enum ControlId {
  IDC_HEADER_TITLE = 1000,
  IDC_HEADER_SUBTITLE,
  IDC_STATUS,
  IDC_LIVE_SUMMARY,
  IDC_RESOLVED_SF,
  IDC_PER_CHANNEL,
  IDC_SOUND_FONT,
  IDC_BROWSE,
  IDC_BACKEND,
  IDC_SAMPLER_ENGINE,
  IDC_SAMPLE_RATE,
  IDC_MAX_VOICES,
  IDC_MASTER_VOLUME,
  IDC_POLLING_RATE,
  IDC_TIMING_MODE,
  IDC_ASYNC_NOTE_STARTS,
  IDC_VELOCITY_CURVE,
  IDC_VELOCITY_FLOOR,
  IDC_VELOCITY_IGNORE,
  IDC_REVERB_ENABLE,
  IDC_REVERB_MIX,
  IDC_REVERB_FEEDBACK,
  IDC_REVERB_TONE,
  IDC_REVERB_WIDTH,
  IDC_REVERB_BLUR,
  IDC_LIMITER_ENABLE,
  IDC_LIMITER_THRESHOLD,
  IDC_LIMITER_RELEASE,
  IDC_APPLY,
  IDC_RELOAD,
  IDC_RESET,
  IDC_KILL
};

struct UiState {
  HWND hwnd;
  HWND headerTitle;
  HWND headerSubtitle;
  HWND statusText;
  HWND liveSummary;
  HWND resolvedSoundfont;
  HWND perChannelVoices;
  HWND soundfontEdit;
  HWND browseButton;
  HWND backendCombo;
  HWND samplerEngineCombo;
  HWND sampleRateCombo;
  HWND maxVoicesEdit;
  HWND masterVolumeEdit;
  HWND pollingRateEdit;
  HWND timingModeCombo;
  HWND velocityCurveEdit;
  HWND velocityFloorEdit;
  HWND velocityIgnoreEdit;
  HWND reverbEnableCheck;
  HWND reverbMixEdit;
  HWND reverbFeedbackEdit;
  HWND reverbToneEdit;
  HWND reverbWidthEdit;
  HWND reverbBlurEdit;
  HWND limiterEnableCheck;
  HWND limiterThresholdEdit;
  HWND limiterReleaseEdit;
  HWND applyButton;
  HWND reloadButton;
  HWND resetButton;
  HWND killButton;
  HWND velocityHint;
  HFONT titleFont;
  HFONT sectionFont;
  HFONT bodyFont;
  HFONT hintFont;
  HBRUSH backgroundBrush;
  HBRUSH panelBrush;
  HBRUSH fieldBrush;
  COLORREF backgroundColor;
  COLORREF panelColor;
  COLORREF fieldColor;
  COLORREF textColor;
  COLORREF mutedTextColor;
  COLORREF accentColor;
  COLORREF borderColor;

  HANDLE mappingHandle;
  HANDLE mutexHandle;
  LiveBridgeSharedState *sharedState;
  LiveBridgeSharedState lastSnapshot;
  bool connected;
  bool seededEditors;
  bool smoothingSeeded;
  bool versionMismatch;
  float smoothedSynthRenderMs;
  float smoothedAudioBlockMs;
  float smoothedTotalVoices;
  char disconnectedStatus[SVMS_MAX_STATUS_TEXT];
  char disconnectedSummary[SVMS_MAX_STATUS_TEXT];
};

UiState g_ui = {};

const LONG_PTR kOwnerDrawCheckboxFlag = 0x1;
const LONG_PTR kOwnerDrawCheckedFlag = 0x2;

void CopyCString(char *dest, size_t capacity, const char *src) {
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

float ClampFloat(float value, float minValue, float maxValue) {
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

const char *DetectSourceFormatText(const char *path) {
  static char format[16];
  format[0] = '\0';
  if (!path || !path[0])
    return "";

  const char *dot = strrchr(path, '.');
  if (!dot || !dot[1])
    return "";

  size_t index = 0;
  for (const char *cursor = dot + 1; *cursor && index + 1 < sizeof(format);
       ++cursor) {
    format[index++] = (char)tolower((unsigned char)*cursor);
  }
  format[index] = '\0';
  return format;
}

float ComputeLoadPercent(float usedMs, float budgetMs) {
  if (budgetMs <= 0.0001f)
    return 0.0f;
  float effectiveBudgetMs = budgetMs * kSafeBudgetFactor;
  if (effectiveBudgetMs <= 0.0001f)
    effectiveBudgetMs = budgetMs;
  return (usedMs / effectiveBudgetMs) * 100.0f;
}

float SmoothDisplayValue(float current, float target, float riseAlpha,
                         float fallAlpha) {
  float alpha = (target > current) ? riseAlpha : fallAlpha;
  alpha = ClampFloat(alpha, 0.0f, 1.0f);
  return current + (target - current) * alpha;
}

float UpdateDisplayMeter(float current, float target, bool seeded,
                         float fallAlpha) {
  if (!seeded)
    return target;
  if (target >= current)
    return target;
  return SmoothDisplayValue(current, target, 1.0f, fallAlpha);
}

const char *GetLoadStatus(float blockPercent) {
  if (blockPercent >= 140.0f)
    return "At risk";
  if (blockPercent >= 115.0f)
    return "Bursting";
  if (blockPercent >= 90.0f)
    return "Tight";
  if (blockPercent >= 60.0f)
    return "Busy";
  return "Healthy";
}

const char *GetOverloadStatus(DWORD overloadState) {
  switch (overloadState) {
  case 2:
    return "Hard";
  case 1:
    return "Soft";
  default:
    return "Off";
  }
}

void SetDisconnectedState(const char *status, const char *summary,
                          bool versionMismatch = false) {
  CopyCString(g_ui.disconnectedStatus, sizeof(g_ui.disconnectedStatus), status);
  CopyCString(g_ui.disconnectedSummary, sizeof(g_ui.disconnectedSummary),
              summary);
  g_ui.versionMismatch = versionMismatch;
}

void ResetEditScroll(HWND hwnd) {
  if (!hwnd)
    return;
  SendMessageA(hwnd, EM_SETSEL, 0, 0);
  SendMessageA(hwnd, EM_SCROLLCARET, 0, 0);
}

bool LockBridge(DWORD timeoutMs) {
  if (!g_ui.mutexHandle)
    return false;
  DWORD result = WaitForSingleObject(g_ui.mutexHandle, timeoutMs);
  return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

void UnlockBridge() {
  if (g_ui.mutexHandle)
    ReleaseMutex(g_ui.mutexHandle);
}

void DisconnectBridge() {
  if (g_ui.sharedState) {
    UnmapViewOfFile(g_ui.sharedState);
    g_ui.sharedState = NULL;
  }
  if (g_ui.mappingHandle) {
    CloseHandle(g_ui.mappingHandle);
    g_ui.mappingHandle = NULL;
  }
  if (g_ui.mutexHandle) {
    CloseHandle(g_ui.mutexHandle);
    g_ui.mutexHandle = NULL;
  }
  g_ui.connected = false;
  g_ui.seededEditors = false;
  g_ui.smoothingSeeded = false;
}

bool LegacyBridgeExists() {
  const char *legacyMappings[] = {SVMS_LIVE_BRIDGE_MAPPING_NAME_V18,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V17,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V16,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V15,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V14,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V13,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V12,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V11,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V10,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V9,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V8,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V7,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V6,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V5,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V4,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V3,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V2,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V1};
  for (size_t i = 0; i < sizeof(legacyMappings) / sizeof(legacyMappings[0]);
       ++i) {
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, legacyMappings[i]);
    if (mapping) {
      CloseHandle(mapping);
      return true;
    }
  }
  return false;
}

bool EnsureBridgeConnected() {
  if (g_ui.sharedState && g_ui.mappingHandle && g_ui.mutexHandle)
    return true;

  DisconnectBridge();

  g_ui.mappingHandle =
      OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SVMS_LIVE_BRIDGE_MAPPING_NAME);
  if (!g_ui.mappingHandle) {
    if (LegacyBridgeExists()) {
      SetDisconnectedState(
          "Live bridge version mismatch",
          "The running synth is using an older live protocol. Rebuild and "
          "restart both the DLL and Configurator so they match.",
          true);
    }
    return false;
  }

  g_ui.sharedState = static_cast<LiveBridgeSharedState *>(
      MapViewOfFile(g_ui.mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0,
                    sizeof(LiveBridgeSharedState)));
  if (!g_ui.sharedState) {
    DisconnectBridge();
    SetDisconnectedState("Failed to map live bridge",
                         "The live bridge exists, but the shared memory view "
                         "could not be opened.");
    return false;
  }

  g_ui.mutexHandle =
      OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                 SVMS_LIVE_BRIDGE_MUTEX_NAME);
  if (!g_ui.mutexHandle) {
    DisconnectBridge();
    SetDisconnectedState("Failed to open live bridge mutex",
                         "The live bridge mutex could not be opened.");
    return false;
  }

  return true;
}

bool ReadSnapshot(LiveBridgeSharedState &snapshot) {
  if (!EnsureBridgeConnected())
    return false;
  if (!LockBridge(50))
    return false;

  snapshot = *g_ui.sharedState;
  UnlockBridge();

  if (snapshot.magic != SVMS_LIVE_BRIDGE_MAGIC ||
      snapshot.version != SVMS_LIVE_BRIDGE_VERSION ||
      snapshot.structSize != sizeof(LiveBridgeSharedState)) {
    DisconnectBridge();
    SetDisconnectedState(
        "Live bridge version mismatch",
        "The shared bridge layout does not match this Configurator. Rebuild "
        "and restart both sides together.",
        true);
    return false;
  }

  if (!snapshot.runtimeLoaded || snapshot.publisherPid == 0)
    return false;

  return true;
}

void SetControlFont(HWND hwnd, HFONT font) {
  if (hwnd && font)
    SendMessage(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void SetFieldMargins(HWND hwnd) {
  if (!hwnd)
    return;
  SendMessageA(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
               MAKELPARAM(8, 8));
}

bool IsOwnerDrawCheckbox(HWND hwnd) {
  if (!hwnd)
    return false;
  return (GetWindowLongPtr(hwnd, GWLP_USERDATA) & kOwnerDrawCheckboxFlag) != 0;
}

bool IsOwnerDrawCheckboxChecked(HWND hwnd) {
  if (!hwnd)
    return false;
  return (GetWindowLongPtr(hwnd, GWLP_USERDATA) & kOwnerDrawCheckedFlag) != 0;
}

void SetOwnerDrawCheckboxChecked(HWND hwnd, bool checked) {
  if (!hwnd)
    return;
  LONG_PTR state = GetWindowLongPtr(hwnd, GWLP_USERDATA);
  state |= kOwnerDrawCheckboxFlag;
  if (checked)
    state |= kOwnerDrawCheckedFlag;
  else
    state &= ~kOwnerDrawCheckedFlag;
  SetWindowLongPtr(hwnd, GWLP_USERDATA, state);
}

void SetEditText(HWND hwnd, const char *text, bool resetScroll = false) {
  SetWindowTextA(hwnd, text ? text : "");
  if (resetScroll)
    ResetEditScroll(hwnd);
}

void SetFloatText(HWND hwnd, float value) {
  char buffer[64];
  sprintf(buffer, "%.3f", value);
  SetWindowTextA(hwnd, buffer);
}

void SetIntText(HWND hwnd, int value) {
  char buffer[64];
  sprintf(buffer, "%d", value);
  SetWindowTextA(hwnd, buffer);
}

std::string GetWindowString(HWND hwnd) {
  char buffer[SVMS_MAX_PATH_TEXT];
  GetWindowTextA(hwnd, buffer, sizeof(buffer));
  return buffer;
}

int GetWindowIntOrDefault(HWND hwnd, int fallback) {
  char buffer[64];
  GetWindowTextA(hwnd, buffer, sizeof(buffer));
  return buffer[0] ? atoi(buffer) : fallback;
}

float GetWindowFloatOrDefault(HWND hwnd, float fallback) {
  char buffer[64];
  GetWindowTextA(hwnd, buffer, sizeof(buffer));
  return buffer[0] ? static_cast<float>(atof(buffer)) : fallback;
}

void EnableEditorControls(BOOL enabled) {
  const HWND controls[] = {
      g_ui.soundfontEdit,       g_ui.browseButton,        g_ui.backendCombo,
      g_ui.samplerEngineCombo,
      g_ui.sampleRateCombo,     g_ui.maxVoicesEdit,       g_ui.masterVolumeEdit,
      g_ui.pollingRateEdit,     g_ui.timingModeCombo,
      g_ui.velocityCurveEdit,   g_ui.velocityFloorEdit,
      g_ui.velocityIgnoreEdit,  g_ui.reverbEnableCheck,   g_ui.reverbMixEdit,
      g_ui.reverbFeedbackEdit,  g_ui.reverbToneEdit,      g_ui.reverbWidthEdit,
      g_ui.reverbBlurEdit,      g_ui.limiterEnableCheck,  g_ui.limiterThresholdEdit,
      g_ui.limiterReleaseEdit,  g_ui.applyButton,         g_ui.reloadButton,
      g_ui.resetButton,         g_ui.killButton};

  for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
    if (controls[i])
      EnableWindow(controls[i], enabled);
  }
}

void SeedEditorsFromSnapshot(const LiveBridgeSharedState &snapshot) {
  SetEditText(g_ui.soundfontEdit, snapshot.currentSettings.soundfontPath, true);
  SetWindowTextA(g_ui.backendCombo, snapshot.currentSettings.audioBackend);
  SetWindowTextA(g_ui.samplerEngineCombo,
                 snapshot.currentSettings.samplerEngine);
  SetIntText(g_ui.sampleRateCombo, snapshot.currentSettings.sampleRate);
  SetIntText(g_ui.maxVoicesEdit, snapshot.currentSettings.maxVoices);
  SetFloatText(g_ui.masterVolumeEdit, snapshot.currentSettings.masterVolume);
  SetIntText(g_ui.pollingRateEdit, snapshot.currentSettings.pollingRate);
  SetWindowTextA(g_ui.timingModeCombo, snapshot.currentSettings.eventTimingMode);
  SetFloatText(g_ui.velocityCurveEdit, snapshot.currentSettings.velocityCurve);
  SetFloatText(g_ui.velocityFloorEdit, snapshot.currentSettings.velocityFloor);
  SetIntText(g_ui.velocityIgnoreEdit,
             snapshot.currentSettings.velocityIgnoreBelow);
  SetOwnerDrawCheckboxChecked(g_ui.reverbEnableCheck,
                              snapshot.currentSettings.reverbEnabled != 0);
  SetFloatText(g_ui.reverbMixEdit, snapshot.currentSettings.reverbMix);
  SetFloatText(g_ui.reverbFeedbackEdit, snapshot.currentSettings.reverbFeedback);
  SetFloatText(g_ui.reverbToneEdit, snapshot.currentSettings.reverbTone);
  SetFloatText(g_ui.reverbWidthEdit, snapshot.currentSettings.reverbWidth);
  SetFloatText(g_ui.reverbBlurEdit, snapshot.currentSettings.reverbBlur);
  SetOwnerDrawCheckboxChecked(g_ui.limiterEnableCheck,
                              snapshot.currentSettings.limiterEnabled != 0);
  SetFloatText(g_ui.limiterThresholdEdit,
               snapshot.currentSettings.limiterThreshold);
  SetFloatText(g_ui.limiterReleaseEdit,
               snapshot.currentSettings.limiterReleaseMs);
  g_ui.seededEditors = true;
}

void UpdateUiFromSnapshot(const LiveBridgeSharedState &snapshot) {
  g_ui.lastSnapshot = snapshot;
  g_ui.connected = true;

  g_ui.smoothedSynthRenderMs = UpdateDisplayMeter(
      g_ui.smoothedSynthRenderMs, snapshot.currentStats.synthRenderMs,
      g_ui.smoothingSeeded, 0.12f);
  g_ui.smoothedAudioBlockMs = UpdateDisplayMeter(
      g_ui.smoothedAudioBlockMs, snapshot.currentStats.audioBlockMs,
      g_ui.smoothingSeeded, 0.12f);
  g_ui.smoothedTotalVoices = UpdateDisplayMeter(
      g_ui.smoothedTotalVoices,
      static_cast<float>(snapshot.currentStats.totalActiveVoices),
      g_ui.smoothingSeeded, 0.18f);
  g_ui.smoothingSeeded = true;

  char statusBuffer[512];
  sprintf(statusBuffer, "Live in PID %lu  |  %s",
          static_cast<unsigned long>(snapshot.publisherPid), snapshot.statusText);
  SetEditText(g_ui.statusText, statusBuffer, true);

  char summaryBuffer[1400];
  char backendSummary[128];
  char samplerSummary[96];
  char warningSuffix[320];
  const char *detectedFormat =
      snapshot.resolvedSourceFormat[0]
          ? snapshot.resolvedSourceFormat
          : DetectSourceFormatText(snapshot.currentSettings.soundfontPath);
  float budgetMs = snapshot.currentStats.audioBudgetMs;
  float displaySynthMs = g_ui.smoothedSynthRenderMs;
  float displayBlockMs = g_ui.smoothedAudioBlockMs;
  float avgSynthMs = snapshot.currentStats.synthRenderAvgMs;
  float peakSynthMs = snapshot.currentStats.synthRenderPeakMs;
  float avgBlockMs = snapshot.currentStats.audioBlockAvgMs;
  float peakBlockMs = snapshot.currentStats.audioBlockPeakMs;
  float synthPercent = ComputeLoadPercent(displaySynthMs, budgetMs);
  float blockPercent = ComputeLoadPercent(displayBlockMs, budgetMs);
  float safeBudgetMs = budgetMs * kSafeBudgetFactor;
  float marginMs = safeBudgetMs - displayBlockMs;
  DWORD timingAgeMs = snapshot.currentStats.audioTimingAgeMs;
  bool timingStale =
      budgetMs > 0.0f &&
      timingAgeMs > (DWORD)((budgetMs * 6.0f) > 120.0f ? (budgetMs * 6.0f)
                                                       : 120.0f);
  const char *loadStatus = timingStale ? "Stalled" : GetLoadStatus(blockPercent);
  const char *resolvedBackend = snapshot.resolvedAudioBackend[0]
                                    ? snapshot.resolvedAudioBackend
                                    : snapshot.currentSettings.audioBackend;
  const bool backendFallback =
      snapshot.resolvedAudioBackend[0] &&
      strcmp(snapshot.resolvedAudioBackend,
             snapshot.currentSettings.audioBackend) != 0;
  if (backendFallback) {
    sprintf(backendSummary, "%s (requested %s)", resolvedBackend,
            snapshot.currentSettings.audioBackend);
  } else {
    sprintf(backendSummary, "%s", resolvedBackend);
  }
  const char *requestedSampler = snapshot.currentSettings.samplerEngine[0]
                                     ? snapshot.currentSettings.samplerEngine
                                     : "auto";
  const char *resolvedSampler = snapshot.resolvedSamplerEngine[0]
                                    ? snapshot.resolvedSamplerEngine
                                    : requestedSampler;
  if (strcmp(requestedSampler, resolvedSampler) != 0) {
    sprintf(samplerSummary, "%s (requested %s)", resolvedSampler,
            requestedSampler);
  } else {
    sprintf(samplerSummary, "%s", resolvedSampler);
  }
  warningSuffix[0] = '\0';
  if (snapshot.samplerLastWarning[0]) {
    sprintf(warningSuffix, "   Last: %s", snapshot.samplerLastWarning);
  }
  const char *overloadStatus = GetOverloadStatus(snapshot.currentStats.overloadState);
  const char *schedulerLagStatus =
      snapshot.currentStats.asyncLagState >= 2
          ? "Overloaded"
          : (snapshot.currentStats.asyncLagState == 1 ? "Lagging" : "Healthy");
  const bool pollingInactive =
      strcmp(snapshot.currentSettings.eventTimingMode, "accurate") == 0;
  float displayPitchBendRange =
      snapshot.currentStats.pitchBendRange[0] > 0.0f
          ? snapshot.currentStats.pitchBendRange[0]
          : 12.0f;
  sprintf(summaryBuffer,
          "Audio %s   Sampler %s   Format %s\r\n"
          "Sample Rate %ld Hz   Voices %lu   Budget %.3f ms (safe %.3f ms)   Timing age %lu ms   Status %s   Samples %lu/%lu failed   Warnings %lu%s\r\n"
          "Synth %.3f / avg %.3f / peak %.3f ms (%.1f%%)   Block %.3f / avg %.3f / peak %.3f ms (%.1f%%)   Margin %.3f ms   PB Ch01 +/-%.2f st\r\n"
          "MIDI %.3f ms   Voice start %.3f ms   Sample render %.3f ms   Pending %lu   Deferred %lu   Max %lu\r\n"
          "Timing %s   Scheduler %s   Event thread %s   WASAPI async %s   Pending %lu / max %lu   Applied %lu   Dropped %lu   Coalesced %lu   Block %llu + %lu fr / %.3f ms   Age %lu ms   Lag %s\r\n"
          "Same-key pending %lu / max %lu   NoteOn coal %lu   NoteOff applied %lu   Coalesced %lu   Canceled %lu   Release ctl %lu   Splits %lu\r\n"
          "Events %lu   NoteOn attempted %lu   Started %lu   Dropped %lu   NoteOff %lu   Shed o/st/pre/post/cp %lu/%lu/%lu/%lu/%lu   Overload %s (%lu)",
          backendSummary, samplerSummary, detectedFormat[0] ? detectedFormat : "?",
          snapshot.currentSettings.sampleRate, static_cast<unsigned long>(g_ui.smoothedTotalVoices + 0.5f), budgetMs, safeBudgetMs,
          static_cast<unsigned long>(timingAgeMs), loadStatus,
          static_cast<unsigned long>(snapshot.currentStats.samplerLoadedSamples),
          static_cast<unsigned long>(snapshot.currentStats.samplerFailedSamples),
          static_cast<unsigned long>(snapshot.currentStats.samplerWarningCount),
          warningSuffix, displaySynthMs, avgSynthMs, peakSynthMs, synthPercent,
          displayBlockMs, avgBlockMs, peakBlockMs, blockPercent, marginMs,
          displayPitchBendRange,
          snapshot.currentStats.midiProcessMs, snapshot.currentStats.voiceStartMs,
          snapshot.currentStats.sampleRenderMs,
          static_cast<unsigned long>(snapshot.currentStats.queuedMidiEvents),
          static_cast<unsigned long>(snapshot.currentStats.deferredMidiEvents),
          static_cast<unsigned long>(snapshot.currentStats.maxQueuedMidiEvents),
          snapshot.currentSettings.eventTimingMode[0]
              ? snapshot.currentSettings.eventTimingMode
              : "accurate",
          snapshot.currentStats.asyncNoteStartsEnabled ? "On" : "Off",
          snapshot.currentStats.eventProcessorThreadActive ? "On" : "Off",
          snapshot.currentStats.wasapiAsyncFeedActive ? "On" : "Off",
          static_cast<unsigned long>(snapshot.currentStats.asyncPendingNoteOns),
          static_cast<unsigned long>(snapshot.currentStats.asyncMaxQueuedNoteOns),
          static_cast<unsigned long>(snapshot.currentStats.asyncStartedThisBlock),
          static_cast<unsigned long>(snapshot.currentStats.asyncDroppedThisBlock),
          static_cast<unsigned long>(snapshot.currentStats.asyncCoalescedThisBlock),
          (unsigned long long)snapshot.currentStats.schedulerBlockStartSample,
          static_cast<unsigned long>(snapshot.currentStats.schedulerSliceFrames),
          snapshot.currentStats.schedulerSliceMs,
          static_cast<unsigned long>(snapshot.currentStats.asyncQueueAgeMs),
          schedulerLagStatus,
          static_cast<unsigned long>(
              snapshot.currentStats.schedulerPendingSameKeyTransitions),
          static_cast<unsigned long>(
              snapshot.currentStats.schedulerMaxSameKeyQueueDepth),
          static_cast<unsigned long>(
              snapshot.currentStats.schedulerNoteOnsCoalescedThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.schedulerNoteOffsAppliedThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.schedulerNoteOffsCoalescedThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.schedulerNoteOffsCanceledThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.schedulerReleaseControlsAppliedThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.schedulerRenderSplitsThisBlock),
          static_cast<unsigned long>(snapshot.currentStats.eventsProcessedThisBlock),
          static_cast<unsigned long>(snapshot.currentStats.noteOnEventsThisBlock),
          static_cast<unsigned long>(snapshot.currentStats.noteOnStartedThisBlock),
          static_cast<unsigned long>(snapshot.currentStats.noteOnDroppedThisBlock),
          static_cast<unsigned long>(snapshot.currentStats.noteOffEventsThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.overloadNoteOnsDroppedThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.staleNoteOnsDroppedThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.preScheduleDropsThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.postScheduleDropsThisBlock),
          static_cast<unsigned long>(
              snapshot.currentStats.catchupPreventedThisBlock),
          overloadStatus,
          static_cast<unsigned long>(snapshot.currentStats.consecutiveOverloadBlocks));
  if (snapshot.currentStats.perfCountersEnabled != 0) {
    char perfBuffer[384];
    sprintf(
        perfBuffer,
        "\r\nPerf Debug   Helpers c/g/x %lu/%lu/%lu   Clustered voices c/g/x %lu/%lu/%lu   Fragments single/threaded %lu/%lu\r\n"
        "Scheduler cache rebuilds %lu   Trim tombstone prunes %lu",
        static_cast<unsigned long>(
            snapshot.currentStats.tsfHelperContiguousBlocks),
        static_cast<unsigned long>(snapshot.currentStats.tsfHelperGatherBlocks),
        static_cast<unsigned long>(snapshot.currentStats.tsfHelperComplexBlocks),
        static_cast<unsigned long>(
            snapshot.currentStats.tsfClusteredVoicesContiguous),
        static_cast<unsigned long>(
            snapshot.currentStats.tsfClusteredVoicesGather),
        static_cast<unsigned long>(
            snapshot.currentStats.tsfClusteredVoicesComplex),
        static_cast<unsigned long>(snapshot.currentStats.tsfSingleThreadFragments),
        static_cast<unsigned long>(snapshot.currentStats.tsfThreadedFragments),
        static_cast<unsigned long>(snapshot.currentStats.schedulerCacheRebuilds),
        static_cast<unsigned long>(
            snapshot.currentStats.schedulerTrimHeapTombstonePrunes));
    strncat(summaryBuffer, perfBuffer,
            sizeof(summaryBuffer) - strlen(summaryBuffer) - 1);
  }
  SetEditText(g_ui.liveSummary, summaryBuffer, true);
  {
    char resolvedSource[SVMS_MAX_PATH_TEXT + 32];
    sprintf(resolvedSource, "[%s] %s",
            detectedFormat[0] ? detectedFormat : "source",
            snapshot.resolvedSoundfontPath[0]
                ? snapshot.resolvedSoundfontPath
                : snapshot.currentSettings.soundfontPath);
    SetEditText(g_ui.resolvedSoundfont, resolvedSource, true);
  }

  char perChannelBuffer[512];
  perChannelBuffer[0] = '\0';
  for (int i = 0; i < 16; ++i) {
    char line[48];
    sprintf(line, "Ch %02d   %lu\r\n", i + 1,
            static_cast<unsigned long>(snapshot.currentStats.activeVoices[i]));
    size_t used = strlen(perChannelBuffer);
    if (used + 1 < sizeof(perChannelBuffer)) {
      strncat(perChannelBuffer, line,
              sizeof(perChannelBuffer) - used - 1);
    }
  }
  SetEditText(g_ui.perChannelVoices, perChannelBuffer, true);

  EnableEditorControls(TRUE);
  EnableWindow(g_ui.pollingRateEdit, pollingInactive ? FALSE : TRUE);
  if (!g_ui.seededEditors)
    SeedEditorsFromSnapshot(snapshot);
}

void UpdateDisconnectedUi() {
  g_ui.smoothingSeeded = false;
  SetEditText(g_ui.statusText,
              g_ui.disconnectedStatus[0]
                  ? g_ui.disconnectedStatus
                  : "Waiting for a live SuperVirtualMIDISynth instance...",
              true);
  SetEditText(g_ui.liveSummary,
              g_ui.disconnectedSummary[0]
                  ? g_ui.disconnectedSummary
                  : "Open a MIDI host that uses SuperVirtualMIDISynth, then "
                    "this window will attach automatically.",
              true);
  SetEditText(g_ui.resolvedSoundfont, "", true);
  SetEditText(g_ui.perChannelVoices, "", true);
  EnableEditorControls(FALSE);
}

void PollLiveState() {
  LiveBridgeSharedState snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  if (ReadSnapshot(snapshot)) {
    UpdateUiFromSnapshot(snapshot);
  } else {
    DisconnectBridge();
    UpdateDisconnectedUi();
  }
}

void BuildSettingsFromUi(LiveBridgeSettings &settings) {
  settings = g_ui.lastSnapshot.currentSettings;
  settings.sampleRate = GetWindowIntOrDefault(g_ui.sampleRateCombo,
                                              settings.sampleRate);
  settings.maxVoices = GetWindowIntOrDefault(g_ui.maxVoicesEdit,
                                             settings.maxVoices);
  settings.pollingRate = GetWindowIntOrDefault(g_ui.pollingRateEdit,
                                               settings.pollingRate);
  CopyCString(settings.eventTimingMode, sizeof(settings.eventTimingMode),
              GetWindowString(g_ui.timingModeCombo).c_str());
  settings.asyncNoteStarts =
      strcmp(settings.eventTimingMode, "legacy-sync") != 0;
  settings.masterVolume =
      GetWindowFloatOrDefault(g_ui.masterVolumeEdit, settings.masterVolume);
  settings.velocityCurve =
      GetWindowFloatOrDefault(g_ui.velocityCurveEdit, settings.velocityCurve);
  settings.velocityFloor =
      GetWindowFloatOrDefault(g_ui.velocityFloorEdit, settings.velocityFloor);
  settings.velocityIgnoreBelow =
      GetWindowIntOrDefault(g_ui.velocityIgnoreEdit, settings.velocityIgnoreBelow);
  settings.reverbEnabled = IsOwnerDrawCheckboxChecked(g_ui.reverbEnableCheck);
  settings.reverbMix =
      GetWindowFloatOrDefault(g_ui.reverbMixEdit, settings.reverbMix);
  settings.reverbFeedback = GetWindowFloatOrDefault(g_ui.reverbFeedbackEdit,
                                                    settings.reverbFeedback);
  settings.reverbTone =
      GetWindowFloatOrDefault(g_ui.reverbToneEdit, settings.reverbTone);
  settings.reverbWidth =
      GetWindowFloatOrDefault(g_ui.reverbWidthEdit, settings.reverbWidth);
  settings.reverbBlur =
      GetWindowFloatOrDefault(g_ui.reverbBlurEdit, settings.reverbBlur);
  settings.limiterEnabled = IsOwnerDrawCheckboxChecked(g_ui.limiterEnableCheck);
  settings.limiterThreshold = GetWindowFloatOrDefault(
      g_ui.limiterThresholdEdit, settings.limiterThreshold);
  settings.limiterReleaseMs = GetWindowFloatOrDefault(
      g_ui.limiterReleaseEdit, settings.limiterReleaseMs);
  CopyCString(settings.audioBackend, sizeof(settings.audioBackend),
              GetWindowString(g_ui.backendCombo).c_str());
  CopyCString(settings.samplerEngine, sizeof(settings.samplerEngine),
              GetWindowString(g_ui.samplerEngineCombo).c_str());
  CopyCString(settings.soundfontPath, sizeof(settings.soundfontPath),
              GetWindowString(g_ui.soundfontEdit).c_str());
}

LONG DetermineApplyCommand(const LiveBridgeSettings &settings,
                           const LiveBridgeSharedState &snapshot) {
  if (settings.sampleRate != snapshot.currentSettings.sampleRate ||
      settings.maxVoices != snapshot.currentSettings.maxVoices ||
      strcmp(settings.audioBackend, snapshot.currentSettings.audioBackend) != 0 ||
      strcmp(settings.samplerEngine, snapshot.currentSettings.samplerEngine) != 0 ||
      strcmp(settings.soundfontPath, snapshot.currentSettings.soundfontPath) != 0) {
    return LIVE_CMD_APPLY_HEAVY;
  }
  return LIVE_CMD_APPLY_SOFT;
}

bool SendCommandAndWait(LONG commandCode, const LiveBridgeSettings &settings,
                        std::string &message) {
  if (!EnsureBridgeConnected())
    return false;

  if (!LockBridge(100))
    return false;

  LONG requestId = g_ui.sharedState->commandRequestId + 1;
  if (requestId <= g_ui.sharedState->commandProcessedId)
    requestId = g_ui.sharedState->commandProcessedId + 1;
  if (requestId <= 0)
    requestId = 1;

  g_ui.sharedState->commandRequestId = requestId;
  g_ui.sharedState->commandCode = commandCode;
  g_ui.sharedState->commandSourcePid = GetCurrentProcessId();
  g_ui.sharedState->requestedSettings = settings;
  g_ui.sharedState->commandInProgress = 0;
  g_ui.sharedState->commandResult = LIVE_RESULT_BUSY;
  CopyCString(g_ui.sharedState->commandMessage,
              sizeof(g_ui.sharedState->commandMessage), "Submitted command...");
  UnlockBridge();

  DWORD start = GetTickCount();
  while (GetTickCount() - start < 10000) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        message = "Command canceled";
        return false;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    LiveBridgeSharedState snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (ReadSnapshot(snapshot) && snapshot.commandProcessedId == requestId) {
      message = snapshot.commandMessage;
      UpdateUiFromSnapshot(snapshot);
      return snapshot.commandResult == LIVE_RESULT_OK;
    }

    Sleep(kLivePollIntervalMs);
  }

  message = "Timed out waiting for the live driver";
  return false;
}

void BrowseForSoundfont() {
  char pathBuffer[SVMS_MAX_PATH_TEXT];
  GetWindowTextA(g_ui.soundfontEdit, pathBuffer, sizeof(pathBuffer));

  OPENFILENAMEA ofn;
  memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_ui.hwnd;
  ofn.lpstrFilter =
      "Sampler Sources (*.sf2;*.sf3;*.sfz)\0*.sf2;*.sf3;*.sfz\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = pathBuffer;
  ofn.nMaxFile = sizeof(pathBuffer);
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (GetOpenFileNameA(&ofn))
    SetEditText(g_ui.soundfontEdit, pathBuffer, true);
}

HWND CreateLabel(HWND parent, const char *text, int x, int y, int w, int h,
                 DWORD style = 0) {
  return CreateWindowExA(WS_EX_TRANSPARENT, "STATIC", text,
                         WS_CHILD | WS_VISIBLE | SS_NOPREFIX | style, x, y, w, h,
                         parent, NULL, GetModuleHandle(NULL), NULL);
}

HWND CreatePanelEdit(HWND parent, int id, int x, int y, int w, int h,
                     DWORD style) {
  HWND hwnd = CreateWindowExA(0, "EDIT", "",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                                  style,
                              x, y, w, h, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandle(NULL), NULL);
  SetFieldMargins(hwnd);
  return hwnd;
}

HWND CreateEditBox(HWND parent, int id, int x, int y, int w, int h) {
  return CreatePanelEdit(parent, id, x, y, w, h, ES_AUTOHSCROLL);
}

HWND CreateReadOnlyBox(HWND parent, int id, int x, int y, int w, int h,
                       DWORD extraStyle = 0) {
  return CreatePanelEdit(parent, id, x, y, w, h,
                         ES_AUTOHSCROLL | ES_READONLY | extraStyle);
}

HWND CreateButton(HWND parent, const char *text, int id, int x, int y, int w,
                  int h, DWORD style = BS_PUSHBUTTON) {
  const bool isCheckbox = ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX);
  HWND hwnd = CreateWindowExA(0, "BUTTON", text,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  BS_OWNERDRAW,
                              x, y, w, h, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandle(NULL), NULL);
  if (isCheckbox)
    SetOwnerDrawCheckboxChecked(hwnd, false);
  return hwnd;
}

HWND CreateComboBox(HWND parent, int id, int x, int y, int w, int h) {
  return CreateWindowExA(0, "COMBOBOX", "",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN, x, y,
                         w, h, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandle(NULL), NULL);
}

HFONT CreateAppFont(int size, int weight) {
  return CreateFontA(-MulDiv(size, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72), 0,
                     0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

HFONT CreateItalicAppFont(int size, int weight) {
  return CreateFontA(-MulDiv(size, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72), 0,
                     0, 0, weight, TRUE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

void ApplyFonts() {
  SetControlFont(g_ui.headerTitle, g_ui.titleFont);
  SetControlFont(g_ui.headerSubtitle, g_ui.bodyFont);
  SetControlFont(g_ui.statusText, g_ui.bodyFont);
  SetControlFont(g_ui.liveSummary, g_ui.bodyFont);
  SetControlFont(g_ui.resolvedSoundfont, g_ui.bodyFont);
  SetControlFont(g_ui.perChannelVoices, g_ui.bodyFont);
  SetControlFont(g_ui.velocityHint, g_ui.hintFont);

  HWND controls[] = {
      g_ui.soundfontEdit,      g_ui.browseButton,      g_ui.backendCombo,
      g_ui.samplerEngineCombo,
      g_ui.sampleRateCombo,    g_ui.maxVoicesEdit,     g_ui.masterVolumeEdit,
      g_ui.pollingRateEdit,    g_ui.timingModeCombo,
      g_ui.velocityCurveEdit, g_ui.velocityFloorEdit,
      g_ui.velocityIgnoreEdit, g_ui.reverbEnableCheck, g_ui.reverbMixEdit,
      g_ui.reverbFeedbackEdit, g_ui.reverbToneEdit,    g_ui.reverbWidthEdit,
      g_ui.reverbBlurEdit,     g_ui.limiterEnableCheck,
      g_ui.limiterThresholdEdit, g_ui.limiterReleaseEdit, g_ui.applyButton,
      g_ui.reloadButton,       g_ui.resetButton,         g_ui.killButton};
  for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i)
    SetControlFont(controls[i], g_ui.bodyFont);
}

void AddSectionTitle(HWND hwnd, const char *text, int x, int y) {
  HWND label = CreateLabel(hwnd, text, x, y, 220, 24);
  SetControlFont(label, g_ui.sectionFont);
}

void AddFieldLabel(HWND hwnd, const char *text, int x, int y, int w = 140) {
  HWND label = CreateLabel(hwnd, text, x, y, w, 18);
  SetControlFont(label, g_ui.bodyFont);
}

void PopulateBackendCombo(HWND comboBox) {
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("auto"));
#ifndef SVMS_LEGACY_XP
  SendMessageA(comboBox, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>("wasapi-shared"));
#endif
  SendMessageA(comboBox, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>("waveout"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("dsound"));
}

void PopulateSamplerEngineCombo(HWND comboBox) {
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("auto"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("tsf"));
  SendMessageA(comboBox, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>("bassmidi"));
#ifndef SVMS_LEGACY_XP
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("sfz"));
#endif
}

void PopulateTimingModeCombo(HWND comboBox) {
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("accurate"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("quantized"));
  SendMessageA(comboBox, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>("legacy-sync"));
}

void BuildUi(HWND hwnd) {
  g_ui.hwnd = hwnd;
  g_ui.backgroundColor = RGB(12, 16, 22);
  g_ui.panelColor = RGB(21, 27, 36);
  g_ui.fieldColor = RGB(14, 20, 28);
  g_ui.textColor = RGB(242, 246, 252);
  g_ui.mutedTextColor = RGB(156, 169, 186);
  g_ui.accentColor = RGB(96, 165, 250);
  g_ui.borderColor = RGB(58, 74, 94);
  g_ui.backgroundBrush = CreateSolidBrush(g_ui.backgroundColor);
  g_ui.panelBrush = CreateSolidBrush(g_ui.panelColor);
  g_ui.fieldBrush = CreateSolidBrush(g_ui.fieldColor);
  g_ui.titleFont = CreateAppFont(20, FW_BOLD);
  g_ui.sectionFont = CreateAppFont(12, FW_BOLD);
  g_ui.bodyFont = CreateAppFont(9, FW_NORMAL);
  g_ui.hintFont = CreateItalicAppFont(8, FW_NORMAL);
  SetDisconnectedState("Waiting for a live SuperVirtualMIDISynth instance...",
                       "Open a MIDI host that uses SuperVirtualMIDISynth, then "
                       "this window will attach automatically.");

  g_ui.headerTitle =
      CreateLabel(hwnd, "SuperVirtualMIDISynth Configurator", 24, 22, 520, 34);
  g_ui.headerSubtitle = CreateLabel(
      hwnd,
      "Live diagnostics, velocity shaping, and hot-reload controls for the "
      "running synth.",
      24, 56, 760, 20);

  AddSectionTitle(hwnd, "Live Monitor", 24, 104);
  g_ui.statusText = CreateReadOnlyBox(hwnd, IDC_STATUS, 24, 132, 920, 26);
  g_ui.liveSummary = CreateReadOnlyBox(
      hwnd, IDC_LIVE_SUMMARY, 24, 170, 920, 64,
      ES_MULTILINE | ES_AUTOVSCROLL);
  AddFieldLabel(hwnd, "Resolved Source", 24, 238, 160);
  g_ui.resolvedSoundfont =
      CreateReadOnlyBox(hwnd, IDC_RESOLVED_SF, 24, 258, 920, 24);

  AddSectionTitle(hwnd, "Engine", 24, 312);
  AddFieldLabel(hwnd, "Source Path", 24, 344);
  g_ui.soundfontEdit = CreateEditBox(hwnd, IDC_SOUND_FONT, 24, 364, 560, 24);
  g_ui.browseButton = CreateButton(hwnd, "Browse", IDC_BROWSE, 596, 364, 100, 24);

  AddFieldLabel(hwnd, "Backend", 24, 404, 100);
  g_ui.backendCombo = CreateComboBox(hwnd, IDC_BACKEND, 24, 424, 110, 220);
  PopulateBackendCombo(g_ui.backendCombo);

  AddFieldLabel(hwnd, "Sampler", 154, 404, 100);
  g_ui.samplerEngineCombo =
      CreateComboBox(hwnd, IDC_SAMPLER_ENGINE, 154, 424, 110, 220);
  PopulateSamplerEngineCombo(g_ui.samplerEngineCombo);

  AddFieldLabel(hwnd, "Sample Rate", 284, 404, 100);
  g_ui.sampleRateCombo =
      CreateComboBox(hwnd, IDC_SAMPLE_RATE, 284, 424, 110, 220);
  SendMessageA(g_ui.sampleRateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("22050"));
  SendMessageA(g_ui.sampleRateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("44100"));
  SendMessageA(g_ui.sampleRateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("48000"));
  SendMessageA(g_ui.sampleRateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("96000"));

  AddFieldLabel(hwnd, "Max Voices", 414, 404, 100);
  g_ui.maxVoicesEdit = CreateEditBox(hwnd, IDC_MAX_VOICES, 414, 424, 90, 24);

  AddFieldLabel(hwnd, "Master Volume", 524, 404, 90);
  g_ui.masterVolumeEdit =
      CreateEditBox(hwnd, IDC_MASTER_VOLUME, 524, 424, 90, 24);

  AddFieldLabel(hwnd, "Polling Rate", 624, 404, 90);
  g_ui.pollingRateEdit = CreateEditBox(hwnd, IDC_POLLING_RATE, 624, 424, 90, 24);
  AddFieldLabel(hwnd, "Timing Mode", 24, 452, 100);
  g_ui.timingModeCombo =
      CreateComboBox(hwnd, IDC_TIMING_MODE, 118, 448, 160, 220);
  PopulateTimingModeCombo(g_ui.timingModeCombo);

  AddSectionTitle(hwnd, "Velocity", 24, 470);
  AddFieldLabel(hwnd, "Curve", 24, 502, 80);
  g_ui.velocityCurveEdit =
      CreateEditBox(hwnd, IDC_VELOCITY_CURVE, 24, 522, 110, 24);
  AddFieldLabel(hwnd, "Floor", 154, 502, 80);
  g_ui.velocityFloorEdit =
      CreateEditBox(hwnd, IDC_VELOCITY_FLOOR, 154, 522, 110, 24);
  AddFieldLabel(hwnd, "Ignore Below", 284, 502, 100);
  g_ui.velocityIgnoreEdit =
      CreateEditBox(hwnd, IDC_VELOCITY_IGNORE, 284, 522, 110, 24);
  g_ui.velocityHint =
      CreateLabel(hwnd,
                  "Lower floor for softer pianissimo. Ignore Below skips weak "
                  "velocities entirely and does not consume a voice.",
                  414, 516, 300, 42, SS_LEFT);

  AddSectionTitle(hwnd, "FX", 24, 576);
  g_ui.reverbEnableCheck =
      CreateButton(hwnd, "Reverb", IDC_REVERB_ENABLE, 24, 608, 100, 24,
                   BS_AUTOCHECKBOX);
  AddFieldLabel(hwnd, "Mix", 136, 592, 60);
  g_ui.reverbMixEdit = CreateEditBox(hwnd, IDC_REVERB_MIX, 136, 612, 80, 24);
  AddFieldLabel(hwnd, "Feedback", 228, 592, 70);
  g_ui.reverbFeedbackEdit =
      CreateEditBox(hwnd, IDC_REVERB_FEEDBACK, 228, 612, 80, 24);
  AddFieldLabel(hwnd, "Tone", 320, 592, 60);
  g_ui.reverbToneEdit = CreateEditBox(hwnd, IDC_REVERB_TONE, 320, 612, 80, 24);
  AddFieldLabel(hwnd, "Width", 412, 592, 60);
  g_ui.reverbWidthEdit = CreateEditBox(hwnd, IDC_REVERB_WIDTH, 412, 612, 80, 24);
  AddFieldLabel(hwnd, "Blur", 504, 592, 60);
  g_ui.reverbBlurEdit = CreateEditBox(hwnd, IDC_REVERB_BLUR, 504, 612, 80, 24);

  g_ui.limiterEnableCheck =
      CreateButton(hwnd, "Limiter", IDC_LIMITER_ENABLE, 604, 608, 100, 24,
                   BS_AUTOCHECKBOX);
  AddFieldLabel(hwnd, "Threshold", 716, 592, 80);
  g_ui.limiterThresholdEdit =
      CreateEditBox(hwnd, IDC_LIMITER_THRESHOLD, 716, 612, 100, 24);
  AddFieldLabel(hwnd, "Release ms", 828, 592, 80);
  g_ui.limiterReleaseEdit =
      CreateEditBox(hwnd, IDC_LIMITER_RELEASE, 828, 612, 116, 24);

  AddSectionTitle(hwnd, "Voices", 736, 312);
  g_ui.perChannelVoices = CreateReadOnlyBox(
      hwnd, IDC_PER_CHANNEL, 736, 344, 208, 202,
      ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL);

  g_ui.applyButton = CreateButton(hwnd, "Apply Live", IDC_APPLY, 470, 680, 110, 32);
  g_ui.reloadButton =
      CreateButton(hwnd, "Reload Config", IDC_RELOAD, 592, 680, 110, 32);
  g_ui.resetButton =
      CreateButton(hwnd, "Hard Reset", IDC_RESET, 714, 680, 110, 32);
  g_ui.killButton =
      CreateButton(hwnd, "Kill Engine", IDC_KILL, 836, 680, 108, 32);

  ApplyFonts();
  EnableEditorControls(FALSE);
  UpdateDisconnectedUi();
}

void DrawPanel(HDC hdc, int left, int top, int right, int bottom) {
  RECT rect = {left, top, right, bottom};
  FillRect(hdc, &rect, g_ui.panelBrush);
  HPEN pen = CreatePen(PS_SOLID, 1, g_ui.borderColor);
  HGDIOBJ oldPen = SelectObject(hdc, pen);
  HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
  Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

bool IsInsidePanel(HWND hwndControl) {
  if (!hwndControl)
    return false;

  RECT rect;
  if (!GetWindowRect(hwndControl, &rect))
    return false;

  POINT topLeft = {rect.left, rect.top};
  POINT bottomRight = {rect.right - 1, rect.bottom - 1};
  ScreenToClient(g_ui.hwnd, &topLeft);
  ScreenToClient(g_ui.hwnd, &bottomRight);

  const RECT panels[] = {
      {20, 100, 948, 292}, {20, 308, 720, 468}, {736, 308, 948, 556},
      {20, 486, 948, 578}, {20, 576, 948, 688}};

  for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); ++i) {
    const RECT &panel = panels[i];
    if (topLeft.x >= panel.left && topLeft.y >= panel.top &&
        bottomRight.x < panel.right && bottomRight.y < panel.bottom)
      return true;
  }

  return false;
}

void PaintBackground(HDC hdc) {
  RECT clientRect;
  GetClientRect(g_ui.hwnd, &clientRect);
  FillRect(hdc, &clientRect, g_ui.backgroundBrush);
  DrawPanel(hdc, 20, 100, 948, 292);
  DrawPanel(hdc, 20, 308, 720, 468);
  DrawPanel(hdc, 736, 308, 948, 556);
  DrawPanel(hdc, 20, 486, 948, 578);
  DrawPanel(hdc, 20, 576, 948, 688);
}

LRESULT HandleStaticColor(HDC hdc, HWND hwndControl) {
  if (hwndControl == g_ui.statusText || hwndControl == g_ui.liveSummary ||
      hwndControl == g_ui.resolvedSoundfont ||
      hwndControl == g_ui.perChannelVoices) {
    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, g_ui.fieldColor);
    SetTextColor(hdc, g_ui.textColor);
    return reinterpret_cast<LRESULT>(g_ui.fieldBrush);
  }

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, hwndControl == g_ui.headerSubtitle ? g_ui.mutedTextColor
                                                       : g_ui.textColor);

  if (hwndControl == g_ui.headerTitle) {
    SetTextColor(hdc, g_ui.accentColor);
    return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
  }

  return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
}

LRESULT HandleEditColor(HDC hdc, HWND hwndControl) {
  SetBkMode(hdc, OPAQUE);
  SetBkColor(hdc, g_ui.fieldColor);
  SetTextColor(hdc, IsWindowEnabled(hwndControl) ? g_ui.textColor
                                                 : g_ui.mutedTextColor);
  return reinterpret_cast<LRESULT>(g_ui.fieldBrush);
}

void DrawOwnerDrawButton(const DRAWITEMSTRUCT *dis) {
  RECT rect = dis->rcItem;
  bool disabled = (dis->itemState & ODS_DISABLED) != 0;
  bool selected = (dis->itemState & ODS_SELECTED) != 0;
  COLORREF fill = disabled ? RGB(24, 28, 34)
                           : (selected ? RGB(19, 26, 35) : g_ui.panelColor);
  COLORREF border = disabled ? RGB(52, 58, 66)
                             : (selected ? g_ui.accentColor : g_ui.borderColor);
  COLORREF text = disabled ? RGB(111, 121, 136) : g_ui.textColor;

  HBRUSH fillBrush = CreateSolidBrush(fill);
  FillRect(dis->hDC, &rect, fillBrush);
  DeleteObject(fillBrush);

  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
  HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
  Rectangle(dis->hDC, rect.left, rect.top, rect.right, rect.bottom);
  SelectObject(dis->hDC, oldBrush);
  SelectObject(dis->hDC, oldPen);
  DeleteObject(pen);

  SetBkMode(dis->hDC, TRANSPARENT);
  SetTextColor(dis->hDC, text);
  SelectObject(dis->hDC, g_ui.bodyFont);

  char label[128];
  GetWindowTextA(dis->hwndItem, label, sizeof(label));
  if (IsOwnerDrawCheckbox(dis->hwndItem)) {
    RECT boxRect = rect;
    boxRect.left += 8;
    boxRect.top += 6;
    boxRect.right = boxRect.left + 14;
    boxRect.bottom = boxRect.top + 14;

    HBRUSH boxBrush = CreateSolidBrush(g_ui.fieldColor);
    FillRect(dis->hDC, &boxRect, boxBrush);
    DeleteObject(boxBrush);

    HPEN boxPen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBoxPen = SelectObject(dis->hDC, boxPen);
    HGDIOBJ oldBoxBrush = SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dis->hDC, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom);
    SelectObject(dis->hDC, oldBoxBrush);
    SelectObject(dis->hDC, oldBoxPen);
    DeleteObject(boxPen);

    if (IsOwnerDrawCheckboxChecked(dis->hwndItem)) {
      HPEN tickPen = CreatePen(PS_SOLID, 2, g_ui.accentColor);
      HGDIOBJ oldTickPen = SelectObject(dis->hDC, tickPen);
      MoveToEx(dis->hDC, boxRect.left + 3, boxRect.top + 7, NULL);
      LineTo(dis->hDC, boxRect.left + 6, boxRect.bottom - 4);
      LineTo(dis->hDC, boxRect.right - 3, boxRect.top + 3);
      SelectObject(dis->hDC, oldTickPen);
      DeleteObject(tickPen);
    }

    RECT textRect = rect;
    textRect.left = boxRect.right + 8;
    DrawTextA(dis->hDC, label, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  } else {
    DrawTextA(dis->hDC, label, -1, &rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE:
    BuildUi(hwnd);
    SetTimer(hwnd, kLivePollTimerId, kLivePollIntervalMs, NULL);
    return 0;

  case WM_ERASEBKGND:
    PaintBackground(reinterpret_cast<HDC>(wParam));
    return 1;

  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    PaintBackground(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_TIMER:
    if (wParam == kLivePollTimerId)
      PollLiveState();
    return 0;

  case WM_CTLCOLORDLG:
    return reinterpret_cast<LRESULT>(g_ui.backgroundBrush);

  case WM_CTLCOLORSTATIC:
    return HandleStaticColor(reinterpret_cast<HDC>(wParam),
                             reinterpret_cast<HWND>(lParam));

  case WM_CTLCOLOREDIT:
    return HandleEditColor(reinterpret_cast<HDC>(wParam),
                           reinterpret_cast<HWND>(lParam));

  case WM_CTLCOLORLISTBOX:
    return HandleEditColor(reinterpret_cast<HDC>(wParam),
                           reinterpret_cast<HWND>(lParam));

  case WM_DRAWITEM:
    DrawOwnerDrawButton(reinterpret_cast<const DRAWITEMSTRUCT *>(lParam));
    return TRUE;

  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDC_BROWSE:
      BrowseForSoundfont();
      return 0;
    case IDC_REVERB_ENABLE:
    case IDC_LIMITER_ENABLE:
      if (HIWORD(wParam) == BN_CLICKED) {
        HWND checkbox = reinterpret_cast<HWND>(lParam);
        SetOwnerDrawCheckboxChecked(checkbox,
                                    !IsOwnerDrawCheckboxChecked(checkbox));
        InvalidateRect(checkbox, NULL, TRUE);
      }
      return 0;
    case IDC_APPLY: {
      LiveBridgeSettings settings;
      memset(&settings, 0, sizeof(settings));
      BuildSettingsFromUi(settings);
      LONG commandCode = DetermineApplyCommand(settings, g_ui.lastSnapshot);
      std::string message;
      bool ok = SendCommandAndWait(commandCode, settings, message);
      SetEditText(g_ui.statusText, message.c_str(), true);
      if (ok)
        g_ui.seededEditors = false;
      return 0;
    }
    case IDC_RELOAD: {
      LiveBridgeSettings settings;
      memset(&settings, 0, sizeof(settings));
      std::string message;
      bool ok = SendCommandAndWait(LIVE_CMD_RELOAD_CONFIG, settings, message);
      SetEditText(g_ui.statusText, message.c_str(), true);
      if (ok)
        g_ui.seededEditors = false;
      return 0;
    }
    case IDC_RESET: {
      LiveBridgeSettings settings;
      memset(&settings, 0, sizeof(settings));
      std::string message;
      bool ok = SendCommandAndWait(LIVE_CMD_RESET_ENGINE, settings, message);
      SetEditText(g_ui.statusText, message.c_str(), true);
      if (ok)
        g_ui.seededEditors = false;
      return 0;
    }
    case IDC_KILL: {
      LiveBridgeSettings settings;
      memset(&settings, 0, sizeof(settings));
      std::string message;
      bool ok = SendCommandAndWait(LIVE_CMD_KILL_ENGINE, settings, message);
      SetEditText(g_ui.statusText, message.c_str(), true);
      if (ok)
        g_ui.seededEditors = false;
      return 0;
    }
    }
    return 0;

  case WM_DESTROY:
    KillTimer(hwnd, kLivePollTimerId);
    DisconnectBridge();
    if (g_ui.titleFont)
      DeleteObject(g_ui.titleFont);
    if (g_ui.sectionFont)
      DeleteObject(g_ui.sectionFont);
    if (g_ui.bodyFont)
      DeleteObject(g_ui.bodyFont);
    if (g_ui.hintFont)
      DeleteObject(g_ui.hintFont);
    if (g_ui.backgroundBrush)
      DeleteObject(g_ui.backgroundBrush);
    if (g_ui.panelBrush)
      DeleteObject(g_ui.panelBrush);
    if (g_ui.fieldBrush)
      DeleteObject(g_ui.fieldBrush);
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = NULL;
  wc.lpszClassName = "SVMSConfiguratorWindow";

  if (!RegisterClassA(&wc))
    return 1;

  HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "SVMS Configurator",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                  WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 984, 780, NULL,
                              NULL, hInstance, NULL);
  if (!hwnd)
    return 1;

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  return static_cast<int>(msg.wParam);
}
