#include "LiveConfigProtocol.h"

#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace {

static const UINT_PTR kLivePollTimerId = 1;
static const UINT kLivePollIntervalMs = 50;
static const int kHistorySamples = 180;
static const int kGraphCount = 5;

enum ViewMode { VIEW_BASIC = 0, VIEW_ADVANCED = 1, VIEW_POWERUSER = 2, VIEW_DEVELOPER = 3 };
enum ProductTab {
  TAB_HOME = 0, TAB_SOUNDFONTS = 1, TAB_ENGINE = 2, TAB_TIMING = 3,
  TAB_PERFORMANCE = 4, TAB_FX_OUTPUT = 5, TAB_DIAGNOSTICS = 6,
  TAB_PROFILES = 7, TAB_ADVANCED = 8, TAB_DEVELOPER = 9, TAB_COUNT = 10
};
enum GraphIndex { GRAPH_RENDER = 0, GRAPH_QUEUE = 1, GRAPH_VS_MIX = 2, GRAPH_OVERLOAD = 3, GRAPH_NOTES = 4 };

enum ControlId {
  IDC_STATUS = 100, IDC_HEADER_SUMMARY, IDC_MODE_TAB, IDC_PRODUCT_TAB,
  IDC_HOME_PANEL, IDC_SOUNDFONTS_PANEL, IDC_ENGINE_PANEL, IDC_TIMING_PANEL,
  IDC_PERFORMANCE_PANEL, IDC_FX_PANEL, IDC_DIAGNOSTICS_PANEL, IDC_PROFILES_PANEL,
  IDC_ADVANCED_PANEL, IDC_DEVELOPER_PANEL, IDC_HOME_SUMMARY, IDC_HOME_HEALTH,
  IDC_HOME_APPLY, IDC_HOME_RELOAD, IDC_SF_PATH, IDC_SF_BROWSE, IDC_SF_RESOLVED,
  IDC_BACKEND_COMBO, IDC_ENGINE_COMBO, IDC_SAMPLE_RATE_COMBO, IDC_MAX_VOICES_EDIT,
  IDC_POLLING_RATE_EDIT, IDC_ASYNC_NOTE_STARTS, IDC_WASAPI_ASYNC, IDC_TIMING_COMBO,
  IDC_TIMING_SUMMARY, IDC_MASTER_VOLUME_EDIT, IDC_VELOCITY_CURVE_EDIT,
  IDC_VELOCITY_FLOOR_EDIT, IDC_VELOCITY_IGNORE_EDIT, IDC_PERFORMANCE_SUMMARY,
  IDC_REVERB_ENABLED, IDC_REVERB_MIX_EDIT, IDC_REVERB_FEEDBACK_EDIT,
  IDC_REVERB_TONE_EDIT, IDC_REVERB_WIDTH_EDIT, IDC_REVERB_BLUR_EDIT,
  IDC_LIMITER_ENABLED, IDC_LIMITER_THRESHOLD_EDIT, IDC_LIMITER_RELEASE_EDIT,
  IDC_FX_SUMMARY, IDC_DIAG_RAW, IDC_DIAG_NOTE_OFF, IDC_DIAG_PER_CHANNEL,
  IDC_GRAPH_RENDER, IDC_GRAPH_QUEUE, IDC_GRAPH_VS, IDC_GRAPH_OVERLOAD,
  IDC_GRAPH_NOTES, IDC_PROFILES_SUMMARY, IDC_ADVANCED_SUMMARY, IDC_DEVELOPER_SUMMARY,
  IDC_DEVELOPER_ENABLE, IDC_DEVELOPER_RESET, IDC_DEVELOPER_KILL
};

struct HistorySeries {
  float values[kHistorySamples];
  int count;
  int writeIndex;
  HistorySeries() : count(0), writeIndex(0) { memset(values, 0, sizeof(values)); }
};

struct ModeControl {
  HWND hwnd;
  int minMode;
};

struct UiState {
  HWND hwnd, statusText, headerSummary, modeTab, productTab, panels[TAB_COUNT];
  HWND homeSummary, homeHealth, homeApplyButton, homeReloadButton;
  HWND soundfontEdit, soundfontBrowseButton, resolvedSoundfontEdit;
  HWND backendCombo, samplerEngineCombo, sampleRateCombo, maxVoicesEdit;
  HWND pollingRateEdit, asyncNoteStartsCheck, wasapiAsyncFeedCheck;
  HWND timingCombo, timingSummary;
  HWND masterVolumeEdit, velocityCurveEdit, velocityFloorEdit, velocityIgnoreEdit, performanceSummary;
  HWND reverbEnabledCheck, reverbMixEdit, reverbFeedbackEdit, reverbToneEdit, reverbWidthEdit, reverbBlurEdit;
  HWND limiterEnabledCheck, limiterThresholdEdit, limiterReleaseEdit, fxSummary;
  HWND diagnosticsRaw, diagnosticsNoteOff, diagnosticsPerChannel, graphs[kGraphCount];
  HWND profilesSummary, advancedSummary, developerSummary, developerEnableCheck, developerResetButton, developerKillButton;
  HFONT titleFont, sectionFont, bodyFont, monoFont;
  HANDLE mappingHandle, mutexHandle;
  LiveBridgeSharedState *sharedState;
  LiveBridgeSharedState lastSnapshot;
  bool connected, seededEditors, versionMismatch, developerControlsEnabled;
  int currentMode, currentTab;
  char disconnectedStatus[SVMS_MAX_STATUS_TEXT];
  char disconnectedSummary[SVMS_MAX_STATUS_TEXT];
  std::vector<ModeControl> modeControls;
  HistorySeries historyRenderMs, historyBlockMs, historyBudgetMs;
  HistorySeries historyQueueDepth, historyDeferredDepth, historyReleaseLaneDepth;
  HistorySeries historyVsExact, historyVsReleasedExact, historyVsGrouped, historyVsDensity, historyVsVoiceEq;
  HistorySeries historyOverloadState, historyNoteOnStarted, historyNoteOnDropped;
  HistorySeries historyNoteOffApplied, historyNoteOffCanceled, historyNoteOffCoalesced;
};

UiState g_ui = {};

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

void AppendHistory(HistorySeries &series, float value) {
  series.values[series.writeIndex] = value;
  series.writeIndex = (series.writeIndex + 1) % kHistorySamples;
  if (series.count < kHistorySamples)
    ++series.count;
}

float GetHistoryValue(const HistorySeries &series, int oldestIndex) {
  if (series.count <= 0 || oldestIndex < 0 || oldestIndex >= series.count)
    return 0.0f;
  const int start = (series.writeIndex - series.count + kHistorySamples) % kHistorySamples;
  const int actual = (start + oldestIndex) % kHistorySamples;
  return series.values[actual];
}

const char *GetLoadStatus(float blockPercent) {
  if (blockPercent >= 140.0f) return "At risk";
  if (blockPercent >= 115.0f) return "Bursting";
  if (blockPercent >= 90.0f) return "Tight";
  if (blockPercent >= 60.0f) return "Busy";
  return "Healthy";
}

const char *GetOverloadStatus(DWORD overloadState) {
  switch (overloadState) {
  case 2: return "Hard";
  case 1: return "Soft";
  default: return "Off";
  }
}

const char *GetReleaseHealth(const LiveBridgeSharedState &snapshot) {
  if (snapshot.currentStats.noteOffLateThisBlock > 0 || snapshot.currentStats.releaseLaneDepth >= 64)
    return "Release path at risk";
  if (snapshot.currentStats.noteOffDeferredThisBlock > snapshot.currentStats.noteOffReleaseLaneAppliedThisBlock &&
      snapshot.currentStats.noteOffDeferredThisBlock > 0)
    return "Release path backlogged";
  if (snapshot.currentStats.schedulerNoteOffsCanceledThisBlock > 0 &&
      snapshot.currentStats.schedulerNoteOffsCanceledThisBlock >=
          snapshot.currentStats.schedulerNoteOffsAppliedThisBlock)
    return "Same-key cancellation active";
  return "Release path healthy";
}

void ResetEditScroll(HWND hwnd) {
  if (!hwnd) return;
  SendMessageA(hwnd, EM_SETSEL, 0, 0);
  SendMessageA(hwnd, EM_SCROLLCARET, 0, 0);
}

void SetEditText(HWND hwnd, const char *text, bool resetScroll = false) {
  if (!hwnd) return;
  SetWindowTextA(hwnd, text ? text : "");
  if (resetScroll)
    ResetEditScroll(hwnd);
}

void SetIntText(HWND hwnd, int value) {
  char buffer[64];
  sprintf(buffer, "%d", value);
  SetEditText(hwnd, buffer);
}

void SetFloatText(HWND hwnd, float value) {
  char buffer[64];
  sprintf(buffer, "%.3f", value);
  SetEditText(hwnd, buffer);
}

void SetCheckboxChecked(HWND hwnd, bool checked) {
  if (hwnd)
    SendMessageA(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool IsCheckboxChecked(HWND hwnd) {
  return hwnd && SendMessageA(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

std::string GetWindowString(HWND hwnd) {
  if (!hwnd) return std::string();
  char buffer[SVMS_MAX_PATH_TEXT];
  GetWindowTextA(hwnd, buffer, sizeof(buffer));
  return std::string(buffer);
}

int GetWindowIntOrDefault(HWND hwnd, int fallback) {
  char buffer[64];
  if (!hwnd || GetWindowTextA(hwnd, buffer, sizeof(buffer)) <= 0)
    return fallback;
  return atoi(buffer);
}

float GetWindowFloatOrDefault(HWND hwnd, float fallback) {
  char buffer[64];
  if (!hwnd || GetWindowTextA(hwnd, buffer, sizeof(buffer)) <= 0)
    return fallback;
  return (float)atof(buffer);
}

std::string GetComboSelectionString(HWND hwnd) {
  if (!hwnd) return std::string();
  int selected = (int)SendMessageA(hwnd, CB_GETCURSEL, 0, 0);
  if (selected != CB_ERR) {
    char buffer[SVMS_MAX_BACKEND_TEXT] = {};
    SendMessageA(hwnd, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(buffer));
    return std::string(buffer);
  }
  return GetWindowString(hwnd);
}

void SelectComboString(HWND hwnd, const char *text) {
  if (!hwnd) return;
  LRESULT result = SendMessageA(hwnd, CB_SELECTSTRING, (WPARAM)-1, reinterpret_cast<LPARAM>(text ? text : ""));
  if (result == CB_ERR)
    SetWindowTextA(hwnd, text ? text : "");
}

HFONT CreateAppFont(int pointSize, int weight, const char *faceName) {
  HDC screen = GetDC(NULL);
  const int logPixels = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
  if (screen) ReleaseDC(NULL, screen);
  return CreateFontA(-MulDiv(pointSize, logPixels, 72), 0, 0, 0, weight, FALSE,
                     FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, faceName);
}

void SetControlFont(HWND hwnd, HFONT font) {
  if (hwnd && font)
    SendMessageA(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void RegisterModeControl(HWND hwnd, int minMode) {
  if (!hwnd) return;
  ModeControl entry;
  entry.hwnd = hwnd;
  entry.minMode = minMode;
  g_ui.modeControls.push_back(entry);
}

HWND CreateModeLabel(HWND parent, const char *text, int x, int y, int w, int h,
                     int minMode, DWORD style = 0);
HWND CreateModeGroupBox(HWND parent, const char *text, int x, int y, int w, int h,
                        int minMode);
HWND CreateModeEdit(HWND parent, int id, int x, int y, int w, int h, DWORD style,
                    int minMode);
HWND CreateModeButton(HWND parent, const char *text, int id, int x, int y, int w,
                      int h, int minMode, DWORD style = BS_PUSHBUTTON);
HWND CreateModeCombo(HWND parent, int id, int x, int y, int w, int h,
                     bool allowEdit, int minMode);
HWND CreateGraphWindow(HWND parent, int controlId, int graphIndex, int x, int y,
                       int w, int h);
void CreatePlaceholderField(HWND parent, const char *label, const char *value, int x,
                            int y, int w, int minMode);
void SetDisconnectedState(const char *status, const char *summary, bool versionMismatch = false);
bool LockBridge(DWORD timeoutMs);
void UnlockBridge();
void DisconnectBridge();
bool LegacyBridgeExists();
bool EnsureBridgeConnected();
bool ReadSnapshot(LiveBridgeSharedState &snapshot);
void PopulateBackendCombo(HWND comboBox);
void PopulateSamplerEngineCombo(HWND comboBox);
void PopulateTimingModeCombo(HWND comboBox);
void PopulateSampleRateCombo(HWND comboBox);
void SeedEditorsFromSnapshot(const LiveBridgeSharedState &snapshot);
void SelectProductTab(int index);
void UpdateModeVisibility();
void UpdateDeveloperButtons();
void SelectModeTab(int index);
void BuildSettingsFromUi(LiveBridgeSettings &settings);
LONG DetermineApplyCommand(const LiveBridgeSettings &settings,
                           const LiveBridgeSharedState &snapshot);
bool SendCommandAndWait(LONG commandCode, const LiveBridgeSettings &settings,
                        std::string &message);
void BrowseForSoundfont();
void AppendSnapshotHistory(const LiveBridgeSharedState &snapshot);
void BuildHeaderSummary(const LiveBridgeSharedState &snapshot);
void BuildHomeSummary(const LiveBridgeSharedState &snapshot);
void BuildTimingSummary(const LiveBridgeSharedState &snapshot);
void BuildPerformanceSummary(const LiveBridgeSharedState &snapshot);
void BuildFxSummary(const LiveBridgeSharedState &snapshot);
void BuildDiagnosticsSummary(const LiveBridgeSharedState &snapshot);
void BuildProfilesSummary();
void BuildAdvancedSummary(const LiveBridgeSharedState &snapshot);
void BuildDeveloperSummary(const LiveBridgeSharedState &snapshot);
void UpdateUiFromSnapshot(const LiveBridgeSharedState &snapshot);
void UpdateDisconnectedUi();
void PollLiveState();
void IssueApplyCommand();
void IssueSimpleCommand(LONG commandCode);
int BuildGraphSpec(int graphIndex, const char **title, const char **labels,
                   const HistorySeries **series, COLORREF *colors,
                   float *fixedMax);
void DrawGraph(HWND hwnd, HDC dc, RECT clientRect, int graphIndex);
LRESULT CALLBACK GraphProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void BuildUi(HWND hwnd);
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

HWND CreateModeLabel(HWND parent, const char *text, int x, int y, int w, int h,
                     int minMode, DWORD style) {
  HWND hwnd = CreateWindowExA(0, "STATIC", text,
                              WS_CHILD | WS_VISIBLE | SS_NOPREFIX | style, x, y,
                              w, h, parent, NULL, GetModuleHandle(NULL), NULL);
  SetControlFont(hwnd, g_ui.bodyFont);
  RegisterModeControl(hwnd, minMode);
  return hwnd;
}

HWND CreateModeGroupBox(HWND parent, const char *text, int x, int y, int w, int h,
                        int minMode) {
  HWND hwnd = CreateWindowExA(0, "BUTTON", text,
                              WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, w, h,
                              parent, NULL, GetModuleHandle(NULL), NULL);
  SetControlFont(hwnd, g_ui.bodyFont);
  RegisterModeControl(hwnd, minMode);
  return hwnd;
}

HWND CreateModeEdit(HWND parent, int id, int x, int y, int w, int h, DWORD style,
                    int minMode) {
  HWND hwnd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y,
                              w, h, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandle(NULL), NULL);
  SetControlFont(hwnd, g_ui.bodyFont);
  RegisterModeControl(hwnd, minMode);
  return hwnd;
}

HWND CreateModeButton(HWND parent, const char *text, int id, int x, int y, int w,
                      int h, int minMode, DWORD style) {
  HWND hwnd = CreateWindowExA(0, "BUTTON", text,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y,
                              w, h, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandle(NULL), NULL);
  SetControlFont(hwnd, g_ui.bodyFont);
  RegisterModeControl(hwnd, minMode);
  return hwnd;
}

HWND CreateModeCombo(HWND parent, int id, int x, int y, int w, int h,
                     bool allowEdit, int minMode) {
  HWND hwnd = CreateWindowExA(
      0, "COMBOBOX", "",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
          (allowEdit ? CBS_DROPDOWN : CBS_DROPDOWNLIST),
      x, y, w, h, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandle(NULL), NULL);
  SetControlFont(hwnd, g_ui.bodyFont);
  RegisterModeControl(hwnd, minMode);
  return hwnd;
}

void CreatePlaceholderField(HWND parent, const char *label, const char *value, int x,
                            int y, int w, int minMode) {
  CreateModeLabel(parent, label, x, y, w, 18, minMode);
  HWND field = CreateModeEdit(parent, 0, x, y + 20, w, 22,
                              ES_AUTOHSCROLL | ES_READONLY, minMode);
  SetEditText(field, value ? value : "Not wired yet");
  EnableWindow(field, FALSE);
}

void SetDisconnectedState(const char *status, const char *summary,
                          bool versionMismatch) {
  CopyCString(g_ui.disconnectedStatus, sizeof(g_ui.disconnectedStatus), status);
  CopyCString(g_ui.disconnectedSummary, sizeof(g_ui.disconnectedSummary), summary);
  g_ui.versionMismatch = versionMismatch;
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
}

bool LegacyBridgeExists() {
  const char *legacyMappings[] = {
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V22, SVMS_LIVE_BRIDGE_MAPPING_NAME_V21,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V20, SVMS_LIVE_BRIDGE_MAPPING_NAME_V19,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V18, SVMS_LIVE_BRIDGE_MAPPING_NAME_V17,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V16, SVMS_LIVE_BRIDGE_MAPPING_NAME_V15,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V14, SVMS_LIVE_BRIDGE_MAPPING_NAME_V13,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V12, SVMS_LIVE_BRIDGE_MAPPING_NAME_V11,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V10, SVMS_LIVE_BRIDGE_MAPPING_NAME_V9,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V8,  SVMS_LIVE_BRIDGE_MAPPING_NAME_V7,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V6,  SVMS_LIVE_BRIDGE_MAPPING_NAME_V5,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V4,  SVMS_LIVE_BRIDGE_MAPPING_NAME_V3,
      SVMS_LIVE_BRIDGE_MAPPING_NAME_V2,  SVMS_LIVE_BRIDGE_MAPPING_NAME_V1};
  for (size_t i = 0; i < sizeof(legacyMappings) / sizeof(legacyMappings[0]); ++i) {
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
          "The running synth is using an older live bridge protocol. Rebuild "
          "and restart both the DLL and Configurator V2 together.",
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
                         "The shared memory bridge exists, but the view could "
                         "not be opened.");
    return false;
  }

  g_ui.mutexHandle = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                SVMS_LIVE_BRIDGE_MUTEX_NAME);
  if (!g_ui.mutexHandle) {
    DisconnectBridge();
    SetDisconnectedState("Failed to open bridge mutex",
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
        "Live bridge layout mismatch",
        "This Configurator V2 does not match the running synth bridge layout. "
        "Rebuild and restart both sides together.",
        true);
    return false;
  }
  if (!snapshot.runtimeLoaded || snapshot.publisherPid == 0)
    return false;
  return true;
}

void PopulateBackendCombo(HWND comboBox) {
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("auto"));
#ifndef SVMS_LEGACY_XP
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("wasapi-shared"));
#endif
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("waveout"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("dsound"));
}

void PopulateSamplerEngineCombo(HWND comboBox) {
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("auto"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("tsf"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("bassmidi"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("virtuallysuper"));
#ifndef SVMS_LEGACY_XP
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("sfz"));
#endif
}

void PopulateTimingModeCombo(HWND comboBox) {
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("accurate"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("quantized"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("legacy-sync"));
}

void PopulateSampleRateCombo(HWND comboBox) {
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("22050"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("44100"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("48000"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("96000"));
}

void SeedEditorsFromSnapshot(const LiveBridgeSharedState &snapshot) {
  SelectComboString(g_ui.backendCombo, snapshot.currentSettings.audioBackend);
  SelectComboString(g_ui.samplerEngineCombo, snapshot.currentSettings.samplerEngine);
  SetIntText(g_ui.sampleRateCombo, snapshot.currentSettings.sampleRate);
  SetIntText(g_ui.maxVoicesEdit, snapshot.currentSettings.maxVoices);
  SetIntText(g_ui.pollingRateEdit, snapshot.currentSettings.pollingRate);
  SelectComboString(g_ui.timingCombo,
                    snapshot.currentSettings.eventTimingMode[0]
                        ? snapshot.currentSettings.eventTimingMode
                        : "accurate");
  SetEditText(g_ui.soundfontEdit, snapshot.currentSettings.soundfontPath);
  SetEditText(g_ui.resolvedSoundfontEdit,
              snapshot.resolvedSoundfontPath[0]
                  ? snapshot.resolvedSoundfontPath
                  : snapshot.currentSettings.soundfontPath);
  SetFloatText(g_ui.masterVolumeEdit, snapshot.currentSettings.masterVolume);
  SetFloatText(g_ui.velocityCurveEdit, snapshot.currentSettings.velocityCurve);
  SetFloatText(g_ui.velocityFloorEdit, snapshot.currentSettings.velocityFloor);
  SetIntText(g_ui.velocityIgnoreEdit, snapshot.currentSettings.velocityIgnoreBelow);
  SetCheckboxChecked(g_ui.asyncNoteStartsCheck, snapshot.currentSettings.asyncNoteStarts != 0);
  SetCheckboxChecked(g_ui.wasapiAsyncFeedCheck, snapshot.currentSettings.wasapiAsyncFeed != 0);
  SetCheckboxChecked(g_ui.reverbEnabledCheck, snapshot.currentSettings.reverbEnabled != 0);
  SetFloatText(g_ui.reverbMixEdit, snapshot.currentSettings.reverbMix);
  SetFloatText(g_ui.reverbFeedbackEdit, snapshot.currentSettings.reverbFeedback);
  SetFloatText(g_ui.reverbToneEdit, snapshot.currentSettings.reverbTone);
  SetFloatText(g_ui.reverbWidthEdit, snapshot.currentSettings.reverbWidth);
  SetFloatText(g_ui.reverbBlurEdit, snapshot.currentSettings.reverbBlur);
  SetCheckboxChecked(g_ui.limiterEnabledCheck, snapshot.currentSettings.limiterEnabled != 0);
  SetFloatText(g_ui.limiterThresholdEdit, snapshot.currentSettings.limiterThreshold);
  SetFloatText(g_ui.limiterReleaseEdit, snapshot.currentSettings.limiterReleaseMs);
  g_ui.seededEditors = true;
}

void SelectProductTab(int index) {
  if (index < 0 || index >= TAB_COUNT)
    index = TAB_HOME;
  g_ui.currentTab = index;
  for (int i = 0; i < TAB_COUNT; ++i)
    ShowWindow(g_ui.panels[i], i == index ? SW_SHOW : SW_HIDE);
}

void UpdateModeVisibility() {
  for (size_t i = 0; i < g_ui.modeControls.size(); ++i) {
    ShowWindow(g_ui.modeControls[i].hwnd,
               g_ui.currentMode >= g_ui.modeControls[i].minMode ? SW_SHOW : SW_HIDE);
  }
}

void UpdateDeveloperButtons() {
  const BOOL connected = g_ui.connected ? TRUE : FALSE;
  const BOOL enabled = (g_ui.connected && g_ui.developerControlsEnabled) ? TRUE : FALSE;
  EnableWindow(g_ui.homeApplyButton, connected);
  EnableWindow(g_ui.homeReloadButton, connected);
  EnableWindow(g_ui.developerResetButton, enabled);
  EnableWindow(g_ui.developerKillButton, enabled);
}

void SelectModeTab(int index) {
  if (index < VIEW_BASIC || index > VIEW_DEVELOPER)
    index = VIEW_DEVELOPER;
  g_ui.currentMode = index;
  UpdateModeVisibility();
}

void BuildSettingsFromUi(LiveBridgeSettings &settings) {
  memset(&settings, 0, sizeof(settings));
  if (g_ui.connected)
    settings = g_ui.lastSnapshot.currentSettings;
  else {
    settings.sampleRate = 44100;
    settings.maxVoices = 500;
    settings.pollingRate = 0;
    settings.masterVolume = 1.0f;
    settings.velocityCurve = 2.4f;
    settings.velocityFloor = 0.0f;
    settings.velocityIgnoreBelow = 0;
    settings.asyncNoteStarts = 1;
    settings.wasapiAsyncFeed = 1;
    settings.reverbEnabled = 0;
    settings.reverbMix = 0.18f;
    settings.reverbFeedback = 0.72f;
    settings.reverbTone = 0.28f;
    settings.reverbWidth = 0.35f;
    settings.reverbBlur = 0.45f;
    settings.limiterEnabled = 1;
    settings.limiterThreshold = 0.98f;
    settings.limiterReleaseMs = 80.0f;
    CopyCString(settings.audioBackend, sizeof(settings.audioBackend), "auto");
    CopyCString(settings.samplerEngine, sizeof(settings.samplerEngine), "auto");
    CopyCString(settings.eventTimingMode, sizeof(settings.eventTimingMode), "accurate");
    CopyCString(settings.soundfontPath, sizeof(settings.soundfontPath), "gm.sf2");
  }

  settings.sampleRate = GetWindowIntOrDefault(g_ui.sampleRateCombo, settings.sampleRate);
  settings.maxVoices = GetWindowIntOrDefault(g_ui.maxVoicesEdit, settings.maxVoices);
  settings.pollingRate = GetWindowIntOrDefault(g_ui.pollingRateEdit, settings.pollingRate);
  settings.masterVolume = GetWindowFloatOrDefault(g_ui.masterVolumeEdit, settings.masterVolume);
  settings.velocityCurve = GetWindowFloatOrDefault(g_ui.velocityCurveEdit, settings.velocityCurve);
  settings.velocityFloor = GetWindowFloatOrDefault(g_ui.velocityFloorEdit, settings.velocityFloor);
  settings.velocityIgnoreBelow = GetWindowIntOrDefault(g_ui.velocityIgnoreEdit, settings.velocityIgnoreBelow);
  settings.asyncNoteStarts = IsCheckboxChecked(g_ui.asyncNoteStartsCheck) ? 1 : 0;
  settings.wasapiAsyncFeed = IsCheckboxChecked(g_ui.wasapiAsyncFeedCheck) ? 1 : 0;
  settings.reverbEnabled = IsCheckboxChecked(g_ui.reverbEnabledCheck) ? 1 : 0;
  settings.reverbMix = GetWindowFloatOrDefault(g_ui.reverbMixEdit, settings.reverbMix);
  settings.reverbFeedback = GetWindowFloatOrDefault(g_ui.reverbFeedbackEdit, settings.reverbFeedback);
  settings.reverbTone = GetWindowFloatOrDefault(g_ui.reverbToneEdit, settings.reverbTone);
  settings.reverbWidth = GetWindowFloatOrDefault(g_ui.reverbWidthEdit, settings.reverbWidth);
  settings.reverbBlur = GetWindowFloatOrDefault(g_ui.reverbBlurEdit, settings.reverbBlur);
  settings.limiterEnabled = IsCheckboxChecked(g_ui.limiterEnabledCheck) ? 1 : 0;
  settings.limiterThreshold = GetWindowFloatOrDefault(g_ui.limiterThresholdEdit, settings.limiterThreshold);
  settings.limiterReleaseMs = GetWindowFloatOrDefault(g_ui.limiterReleaseEdit, settings.limiterReleaseMs);
  CopyCString(settings.audioBackend, sizeof(settings.audioBackend),
              GetComboSelectionString(g_ui.backendCombo).c_str());
  CopyCString(settings.samplerEngine, sizeof(settings.samplerEngine),
              GetComboSelectionString(g_ui.samplerEngineCombo).c_str());
  CopyCString(settings.eventTimingMode, sizeof(settings.eventTimingMode),
              GetComboSelectionString(g_ui.timingCombo).c_str());
  CopyCString(settings.soundfontPath, sizeof(settings.soundfontPath),
              GetWindowString(g_ui.soundfontEdit).c_str());
}

LONG DetermineApplyCommand(const LiveBridgeSettings &settings,
                           const LiveBridgeSharedState &snapshot) {
  if (settings.sampleRate != snapshot.currentSettings.sampleRate ||
      settings.maxVoices != snapshot.currentSettings.maxVoices ||
      strcmp(settings.audioBackend, snapshot.currentSettings.audioBackend) != 0 ||
      strcmp(settings.samplerEngine, snapshot.currentSettings.samplerEngine) != 0 ||
      strcmp(settings.soundfontPath, snapshot.currentSettings.soundfontPath) != 0)
    return LIVE_CMD_APPLY_HEAVY;
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
      g_ui.lastSnapshot = snapshot;
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
    SetEditText(g_ui.soundfontEdit, pathBuffer);
}

void AppendSnapshotHistory(const LiveBridgeSharedState &snapshot) {
  AppendHistory(g_ui.historyRenderMs, snapshot.currentStats.synthRenderMs);
  AppendHistory(g_ui.historyBlockMs, snapshot.currentStats.audioBlockMs);
  AppendHistory(g_ui.historyBudgetMs, snapshot.currentStats.audioBudgetMs);
  AppendHistory(g_ui.historyQueueDepth, (float)snapshot.currentStats.queuedMidiEvents);
  AppendHistory(g_ui.historyDeferredDepth, (float)snapshot.currentStats.deferredMidiEvents);
  AppendHistory(g_ui.historyReleaseLaneDepth, (float)snapshot.currentStats.releaseLaneDepth);
  AppendHistory(g_ui.historyVsExact, (float)snapshot.currentStats.virtuallySuperExactVoices);
  AppendHistory(g_ui.historyVsReleasedExact, (float)snapshot.currentStats.virtuallySuperReleasedExactVoices);
  AppendHistory(g_ui.historyVsGrouped, (float)snapshot.currentStats.virtuallySuperGroupedObjects);
  AppendHistory(g_ui.historyVsDensity, (float)snapshot.currentStats.virtuallySuperDensityObjects);
  AppendHistory(g_ui.historyVsVoiceEq, (float)snapshot.currentStats.virtuallySuperVoiceEquivalent);
  AppendHistory(g_ui.historyOverloadState, (float)snapshot.currentStats.overloadState);
  AppendHistory(g_ui.historyNoteOnStarted, (float)snapshot.currentStats.noteOnStartedThisBlock);
  AppendHistory(g_ui.historyNoteOnDropped, (float)snapshot.currentStats.noteOnDroppedThisBlock);
  AppendHistory(g_ui.historyNoteOffApplied, (float)snapshot.currentStats.schedulerNoteOffsAppliedThisBlock);
  AppendHistory(g_ui.historyNoteOffCanceled, (float)snapshot.currentStats.schedulerNoteOffsCanceledThisBlock);
  AppendHistory(g_ui.historyNoteOffCoalesced, (float)snapshot.currentStats.schedulerNoteOffsCoalescedThisBlock);
}

void BuildHeaderSummary(const LiveBridgeSharedState &snapshot) {
  char buffer[1024];
  float loadPercent = 0.0f;
  if (snapshot.currentStats.audioBudgetMs > 0.0f)
    loadPercent = snapshot.currentStats.audioBlockMs * 100.0f / snapshot.currentStats.audioBudgetMs;
  sprintf(buffer,
          "Protocol v%lu   PID %lu   Backend %s   Engine %s   Source %s\r\n"
          "Active voices %lu   VoiceEq %lu   Overload %s   Health %s   Release %s\r\n"
          "Status %s",
          (unsigned long)snapshot.version, (unsigned long)snapshot.publisherPid,
          snapshot.resolvedAudioBackend[0] ? snapshot.resolvedAudioBackend : snapshot.currentSettings.audioBackend,
          snapshot.resolvedSamplerEngine[0] ? snapshot.resolvedSamplerEngine : snapshot.currentSettings.samplerEngine,
          snapshot.resolvedSoundfontPath[0] ? snapshot.resolvedSoundfontPath : snapshot.currentSettings.soundfontPath,
          (unsigned long)snapshot.currentStats.totalActiveVoices,
          (unsigned long)snapshot.currentStats.virtuallySuperVoiceEquivalent,
          GetOverloadStatus(snapshot.currentStats.overloadState),
          GetLoadStatus(loadPercent), GetReleaseHealth(snapshot), snapshot.statusText);
  SetEditText(g_ui.headerSummary, buffer, true);
}

void BuildHomeSummary(const LiveBridgeSharedState &snapshot) {
  char summary[2048];
  char health[1024];
  float loadPercent = 0.0f;
  if (snapshot.currentStats.audioBudgetMs > 0.0f)
    loadPercent = snapshot.currentStats.audioBlockMs * 100.0f / snapshot.currentStats.audioBudgetMs;

  sprintf(health,
          "Runtime Health\r\n"
          "Audio block %.3f ms / budget %.3f ms (%s)\r\n"
          "Render %.3f ms avg %.3f peak %.3f\r\n"
          "Queue %lu   Deferred %lu   Release lane %lu   Scheduled %lu\r\n"
          "NoteOff ingress %lu   deferred %lu   lane queued %lu   lane applied %lu   late %lu",
          snapshot.currentStats.audioBlockMs, snapshot.currentStats.audioBudgetMs,
          GetLoadStatus(loadPercent), snapshot.currentStats.synthRenderMs,
          snapshot.currentStats.synthRenderAvgMs, snapshot.currentStats.synthRenderPeakMs,
          (unsigned long)snapshot.currentStats.queuedMidiEvents,
          (unsigned long)snapshot.currentStats.deferredMidiEvents,
          (unsigned long)snapshot.currentStats.releaseLaneDepth,
          (unsigned long)snapshot.currentStats.asyncPendingNoteOns,
          (unsigned long)snapshot.currentStats.noteOffIngressThisBlock,
          (unsigned long)snapshot.currentStats.noteOffDeferredThisBlock,
          (unsigned long)snapshot.currentStats.noteOffReleaseLaneQueuedThisBlock,
          (unsigned long)snapshot.currentStats.noteOffReleaseLaneAppliedThisBlock,
          (unsigned long)snapshot.currentStats.noteOffLateThisBlock);
  SetEditText(g_ui.homeHealth, health, true);

  sprintf(summary,
          "Resolved engine %s   backend %s   source format %s\r\n"
          "Current source %s\r\n"
          "Timing %s   Async note starts %s   WASAPI async feed %s\r\n"
          "Exact %lu   Released exact %lu   Grouped %lu   Density %lu   Pressure %lu\r\n"
          "Sampler state %lu   Error %lu   Warning %s",
          snapshot.resolvedSamplerEngine[0] ? snapshot.resolvedSamplerEngine : snapshot.currentSettings.samplerEngine,
          snapshot.resolvedAudioBackend[0] ? snapshot.resolvedAudioBackend : snapshot.currentSettings.audioBackend,
          snapshot.resolvedSourceFormat[0] ? snapshot.resolvedSourceFormat : "source",
          snapshot.resolvedSoundfontPath[0] ? snapshot.resolvedSoundfontPath : snapshot.currentSettings.soundfontPath,
          snapshot.currentSettings.eventTimingMode[0] ? snapshot.currentSettings.eventTimingMode : "accurate",
          snapshot.currentStats.asyncNoteStartsEnabled ? "On" : "Off",
          snapshot.currentStats.wasapiAsyncFeedActive ? "On" : "Off",
          (unsigned long)snapshot.currentStats.virtuallySuperExactVoices,
          (unsigned long)snapshot.currentStats.virtuallySuperReleasedExactVoices,
          (unsigned long)snapshot.currentStats.virtuallySuperGroupedObjects,
          (unsigned long)snapshot.currentStats.virtuallySuperDensityObjects,
          (unsigned long)snapshot.currentStats.virtuallySuperPressureLevel,
          (unsigned long)snapshot.currentStats.samplerStateCode,
          (unsigned long)snapshot.currentStats.samplerErrorCode,
          snapshot.samplerLastWarning[0] ? snapshot.samplerLastWarning : "None");
  SetEditText(g_ui.homeSummary, summary, true);
  SetEditText(g_ui.resolvedSoundfontEdit,
              snapshot.resolvedSoundfontPath[0] ? snapshot.resolvedSoundfontPath : snapshot.currentSettings.soundfontPath);
}

void BuildTimingSummary(const LiveBridgeSharedState &snapshot) {
  char buffer[1024];
  sprintf(buffer,
          "Timing runtime\r\n"
          "Mode %s   Polling rate %ld   Scheduler slice %lu frames / %.3f ms\r\n"
          "Due %lu   Late %lu   Lag %lu samples / %.3f ms\r\n"
          "Pending same-key %lu   Max same-key depth %lu\r\n"
          "Release controls applied %lu",
          snapshot.currentSettings.eventTimingMode[0] ? snapshot.currentSettings.eventTimingMode : "accurate",
          (long)snapshot.currentSettings.pollingRate,
          (unsigned long)snapshot.currentStats.schedulerSliceFrames,
          snapshot.currentStats.schedulerSliceMs,
          (unsigned long)snapshot.currentStats.schedulerDueEventsThisBlock,
          (unsigned long)snapshot.currentStats.schedulerLateEventsThisBlock,
          (unsigned long)snapshot.currentStats.schedulerLagSamples,
          snapshot.currentStats.schedulerLagMs,
          (unsigned long)snapshot.currentStats.schedulerPendingSameKeyTransitions,
          (unsigned long)snapshot.currentStats.schedulerMaxSameKeyQueueDepth,
          (unsigned long)snapshot.currentStats.schedulerReleaseControlsAppliedThisBlock);
  SetEditText(g_ui.timingSummary, buffer, true);
}

void BuildPerformanceSummary(const LiveBridgeSharedState &snapshot) {
  char buffer[1536];
  sprintf(buffer,
          "Performance diagnostics\r\n"
          "Critical queue %lu   Realtime queue %lu   NoteOn queue %lu   Release lane %lu\r\n"
          "Events processed %lu   NoteOn attempted %lu   started %lu   dropped %lu   NoteOff processed %lu\r\n"
          "Overload %s for %lu blocks   Hard entries %lu   Hard recoveries %lu\r\n"
          "Accurate pending peak %lu   deferred peak %lu   scheduled peak %lu\r\n"
          "Async pending %lu   async dropped %lu   async coalesced %lu   catchup prevented %lu",
          (unsigned long)snapshot.currentStats.criticalQueueDepth,
          (unsigned long)snapshot.currentStats.realtimeQueueDepth,
          (unsigned long)snapshot.currentStats.noteOnQueueDepth,
          (unsigned long)snapshot.currentStats.releaseLaneDepth,
          (unsigned long)snapshot.currentStats.eventsProcessedThisBlock,
          (unsigned long)snapshot.currentStats.noteOnEventsThisBlock,
          (unsigned long)snapshot.currentStats.noteOnStartedThisBlock,
          (unsigned long)snapshot.currentStats.noteOnDroppedThisBlock,
          (unsigned long)snapshot.currentStats.noteOffEventsThisBlock,
          GetOverloadStatus(snapshot.currentStats.overloadState),
          (unsigned long)snapshot.currentStats.consecutiveOverloadBlocks,
          (unsigned long)snapshot.currentStats.accurateHardOverloadEntries,
          (unsigned long)snapshot.currentStats.accurateHardOverloadRecoveries,
          (unsigned long)snapshot.currentStats.accuratePeakPendingEvents,
          (unsigned long)snapshot.currentStats.accuratePeakDeferredEvents,
          (unsigned long)snapshot.currentStats.accuratePeakScheduledEvents,
          (unsigned long)snapshot.currentStats.asyncPendingNoteOns,
          (unsigned long)snapshot.currentStats.asyncDroppedThisBlock,
          (unsigned long)snapshot.currentStats.asyncCoalescedThisBlock,
          (unsigned long)snapshot.currentStats.catchupPreventedThisBlock);
  SetEditText(g_ui.performanceSummary, buffer, true);
}

void BuildFxSummary(const LiveBridgeSharedState &snapshot) {
  char buffer[1024];
  sprintf(buffer,
          "Output and FX runtime\r\n"
          "Master volume %.3f\r\n"
          "Limiter %s   threshold %.3f   release %.3f ms\r\n"
          "Reverb %s   mix %.3f   feedback %.3f   tone %.3f   width %.3f   blur %.3f\r\n"
          "Resolved backend %s   WASAPI async active %s",
          snapshot.currentSettings.masterVolume,
          snapshot.currentSettings.limiterEnabled ? "On" : "Off",
          snapshot.currentSettings.limiterThreshold,
          snapshot.currentSettings.limiterReleaseMs,
          snapshot.currentSettings.reverbEnabled ? "On" : "Off",
          snapshot.currentSettings.reverbMix, snapshot.currentSettings.reverbFeedback,
          snapshot.currentSettings.reverbTone, snapshot.currentSettings.reverbWidth,
          snapshot.currentSettings.reverbBlur,
          snapshot.resolvedAudioBackend[0] ? snapshot.resolvedAudioBackend : snapshot.currentSettings.audioBackend,
          snapshot.currentStats.wasapiAsyncFeedActive ? "Yes" : "No");
  SetEditText(g_ui.fxSummary, buffer, true);
}

void BuildDiagnosticsSummary(const LiveBridgeSharedState &snapshot) {
  char raw[8192];
  char notes[2048];
  char perChannel[768];
  perChannel[0] = '\0';
  sprintf(raw,
          "Current Settings\r\n"
          "sample_rate=%ld   max_voices=%ld   polling_rate=%ld\r\n"
          "master_volume=%.3f   velocity_curve=%.3f   velocity_floor=%.3f   velocity_ignore_below=%ld\r\n"
          "async_note_starts=%ld   wasapi_async_feed=%ld   timing=%s\r\n"
          "audio_backend=%s   sampler_engine=%s\r\n"
          "soundfont=%s\r\n"
          "reverb_enable=%ld   mix=%.3f   feedback=%.3f   tone=%.3f   width=%.3f   blur=%.3f\r\n"
          "limiter_enable=%ld   threshold=%.3f   release_ms=%.3f\r\n\r\n"
          "Core Stats\r\n"
          "render_ms=%.3f avg=%.3f peak=%.3f   block_ms=%.3f avg=%.3f peak=%.3f   budget_ms=%.3f   audio_age_ms=%lu\r\n"
          "midi_ms=%.3f   voice_start_ms=%.3f   sample_render_ms=%.3f\r\n"
          "active_voices_total=%lu   queued=%lu   deferred=%lu   critical=%lu   realtime=%lu   noteon=%lu   release_lane=%lu   max_queue=%lu\r\n"
          "events_processed=%lu   noteon_attempted=%lu   started=%lu   dropped=%lu   noteoff_processed=%lu\r\n"
          "noteoff_ingress=%lu   noteoff_deferred=%lu   noteoff_lane_queued=%lu   noteoff_lane_applied=%lu   noteoff_late=%lu\r\n"
          "dropped_noteon_total=%lu   dropped_non_note_total=%lu\r\n"
          "async_pending=%lu   async_started=%lu   async_dropped=%lu   async_coalesced=%lu   async_queue_age_ms=%lu   async_lag_state=%lu\r\n"
          "overload_noteon_drops=%lu   stale_noteon_drops=%lu   pre_schedule_drops=%lu   post_schedule_drops=%lu   catchup_prevented=%lu\r\n\r\n"
          "Scheduler\r\n"
          "slice_frames=%lu   due=%lu   late=%lu   lag_samples=%lu   lag_ms=%.3f   block_start=%llu\r\n"
          "samekey_pending=%lu   samekey_max_depth=%lu   noteon_coalesced=%lu\r\n"
          "noteoff_applied=%lu   noteoff_coalesced=%lu   noteoff_canceled=%lu   release_controls=%lu   render_splits=%lu\r\n"
          "clock_resets=%lu   state_preserved=%lu   last_restart_reason=%lu   cache_rebuilds=%lu   trim_tombstone_prunes=%lu\r\n\r\n"
          "Runtime\r\n"
          "overload_state=%lu   consecutive_overload_blocks=%lu   runtime_reload_count=%lu   worker_blocked=%lu   perf_debug=%lu\r\n"
          "sampler_warning_count=%lu   loaded_samples=%lu   failed_samples=%lu   sampler_state=%lu   sampler_error=%lu\r\n"
          "virtuallysuper exact=%lu   released_exact=%lu   grouped=%lu   density=%lu   voice_eq=%lu   pressure=%lu   presets=%lu   regions=%lu   exact_mode=%lu\r\n"
          "virtuallysuper fast_path=%lu   exact_visited=%lu   grouped_visited=%lu   density_visited=%lu",
          (long)snapshot.currentSettings.sampleRate, (long)snapshot.currentSettings.maxVoices,
          (long)snapshot.currentSettings.pollingRate, snapshot.currentSettings.masterVolume,
          snapshot.currentSettings.velocityCurve, snapshot.currentSettings.velocityFloor,
          (long)snapshot.currentSettings.velocityIgnoreBelow, (long)snapshot.currentSettings.asyncNoteStarts,
          (long)snapshot.currentSettings.wasapiAsyncFeed, snapshot.currentSettings.eventTimingMode,
          snapshot.currentSettings.audioBackend, snapshot.currentSettings.samplerEngine,
          snapshot.currentSettings.soundfontPath, (long)snapshot.currentSettings.reverbEnabled,
          snapshot.currentSettings.reverbMix, snapshot.currentSettings.reverbFeedback,
          snapshot.currentSettings.reverbTone, snapshot.currentSettings.reverbWidth,
          snapshot.currentSettings.reverbBlur, (long)snapshot.currentSettings.limiterEnabled,
          snapshot.currentSettings.limiterThreshold, snapshot.currentSettings.limiterReleaseMs,
          snapshot.currentStats.synthRenderMs, snapshot.currentStats.synthRenderAvgMs,
          snapshot.currentStats.synthRenderPeakMs, snapshot.currentStats.audioBlockMs,
          snapshot.currentStats.audioBlockAvgMs, snapshot.currentStats.audioBlockPeakMs,
          snapshot.currentStats.audioBudgetMs, (unsigned long)snapshot.currentStats.audioTimingAgeMs,
          snapshot.currentStats.midiProcessMs, snapshot.currentStats.voiceStartMs,
          snapshot.currentStats.sampleRenderMs, (unsigned long)snapshot.currentStats.totalActiveVoices,
          (unsigned long)snapshot.currentStats.queuedMidiEvents, (unsigned long)snapshot.currentStats.deferredMidiEvents,
          (unsigned long)snapshot.currentStats.criticalQueueDepth, (unsigned long)snapshot.currentStats.realtimeQueueDepth,
          (unsigned long)snapshot.currentStats.noteOnQueueDepth, (unsigned long)snapshot.currentStats.releaseLaneDepth,
          (unsigned long)snapshot.currentStats.maxQueuedMidiEvents, (unsigned long)snapshot.currentStats.eventsProcessedThisBlock,
          (unsigned long)snapshot.currentStats.noteOnEventsThisBlock, (unsigned long)snapshot.currentStats.noteOnStartedThisBlock,
          (unsigned long)snapshot.currentStats.noteOnDroppedThisBlock, (unsigned long)snapshot.currentStats.noteOffEventsThisBlock,
          (unsigned long)snapshot.currentStats.noteOffIngressThisBlock, (unsigned long)snapshot.currentStats.noteOffDeferredThisBlock,
          (unsigned long)snapshot.currentStats.noteOffReleaseLaneQueuedThisBlock,
          (unsigned long)snapshot.currentStats.noteOffReleaseLaneAppliedThisBlock,
          (unsigned long)snapshot.currentStats.noteOffLateThisBlock, (unsigned long)snapshot.currentStats.droppedNoteOnEvents,
          (unsigned long)snapshot.currentStats.droppedNonNoteEvents, (unsigned long)snapshot.currentStats.asyncPendingNoteOns,
          (unsigned long)snapshot.currentStats.asyncStartedThisBlock, (unsigned long)snapshot.currentStats.asyncDroppedThisBlock,
          (unsigned long)snapshot.currentStats.asyncCoalescedThisBlock, (unsigned long)snapshot.currentStats.asyncQueueAgeMs,
          (unsigned long)snapshot.currentStats.asyncLagState, (unsigned long)snapshot.currentStats.overloadNoteOnsDroppedThisBlock,
          (unsigned long)snapshot.currentStats.staleNoteOnsDroppedThisBlock, (unsigned long)snapshot.currentStats.preScheduleDropsThisBlock,
          (unsigned long)snapshot.currentStats.postScheduleDropsThisBlock, (unsigned long)snapshot.currentStats.catchupPreventedThisBlock,
          (unsigned long)snapshot.currentStats.schedulerSliceFrames, (unsigned long)snapshot.currentStats.schedulerDueEventsThisBlock,
          (unsigned long)snapshot.currentStats.schedulerLateEventsThisBlock, (unsigned long)snapshot.currentStats.schedulerLagSamples,
          snapshot.currentStats.schedulerLagMs, snapshot.currentStats.schedulerBlockStartSample,
          (unsigned long)snapshot.currentStats.schedulerPendingSameKeyTransitions,
          (unsigned long)snapshot.currentStats.schedulerMaxSameKeyQueueDepth,
          (unsigned long)snapshot.currentStats.schedulerNoteOnsCoalescedThisBlock,
          (unsigned long)snapshot.currentStats.schedulerNoteOffsAppliedThisBlock,
          (unsigned long)snapshot.currentStats.schedulerNoteOffsCoalescedThisBlock,
          (unsigned long)snapshot.currentStats.schedulerNoteOffsCanceledThisBlock,
          (unsigned long)snapshot.currentStats.schedulerReleaseControlsAppliedThisBlock,
          (unsigned long)snapshot.currentStats.schedulerRenderSplitsThisBlock,
          (unsigned long)snapshot.currentStats.accurateClockResetCount,
          (unsigned long)snapshot.currentStats.schedulerStatePreservedCount,
          (unsigned long)snapshot.currentStats.lastRestartReason,
          (unsigned long)snapshot.currentStats.schedulerCacheRebuilds,
          (unsigned long)snapshot.currentStats.schedulerTrimHeapTombstonePrunes,
          (unsigned long)snapshot.currentStats.overloadState,
          (unsigned long)snapshot.currentStats.consecutiveOverloadBlocks,
          (unsigned long)snapshot.currentStats.runtimeReloadCount,
          (unsigned long)snapshot.currentStats.accurateWorkerBlockedCount,
          (unsigned long)snapshot.currentStats.perfCountersEnabled,
          (unsigned long)snapshot.currentStats.samplerWarningCount,
          (unsigned long)snapshot.currentStats.samplerLoadedSamples,
          (unsigned long)snapshot.currentStats.samplerFailedSamples,
          (unsigned long)snapshot.currentStats.samplerStateCode,
          (unsigned long)snapshot.currentStats.samplerErrorCode,
          (unsigned long)snapshot.currentStats.virtuallySuperExactVoices,
          (unsigned long)snapshot.currentStats.virtuallySuperReleasedExactVoices,
          (unsigned long)snapshot.currentStats.virtuallySuperGroupedObjects,
          (unsigned long)snapshot.currentStats.virtuallySuperDensityObjects,
          (unsigned long)snapshot.currentStats.virtuallySuperVoiceEquivalent,
          (unsigned long)snapshot.currentStats.virtuallySuperPressureLevel,
          (unsigned long)snapshot.currentStats.virtuallySuperLoadedPresets,
          (unsigned long)snapshot.currentStats.virtuallySuperLoadedRegions,
          (unsigned long)snapshot.currentStats.virtuallySuperExactMode,
          (unsigned long)snapshot.currentStats.virtuallySuperIdleFastPathHits,
          (unsigned long)snapshot.currentStats.virtuallySuperExactVisitedThisBlock,
          (unsigned long)snapshot.currentStats.virtuallySuperGroupedVisitedThisBlock,
          (unsigned long)snapshot.currentStats.virtuallySuperDensityVisitedThisBlock);
  SetEditText(g_ui.diagnosticsRaw, raw, true);

  sprintf(notes,
          "Release-lane analysis\r\n"
          "State: %s\r\n"
          "Ingress note-offs %lu   Deferred %lu   Lane queued %lu   Lane applied %lu   Late %lu\r\n"
          "Same-key applied %lu   coalesced %lu   canceled %lu\r\n"
          "Release controls applied %lu   Queue depths critical/realtime/note-on/lane = %lu/%lu/%lu/%lu",
          GetReleaseHealth(snapshot),
          (unsigned long)snapshot.currentStats.noteOffIngressThisBlock,
          (unsigned long)snapshot.currentStats.noteOffDeferredThisBlock,
          (unsigned long)snapshot.currentStats.noteOffReleaseLaneQueuedThisBlock,
          (unsigned long)snapshot.currentStats.noteOffReleaseLaneAppliedThisBlock,
          (unsigned long)snapshot.currentStats.noteOffLateThisBlock,
          (unsigned long)snapshot.currentStats.schedulerNoteOffsAppliedThisBlock,
          (unsigned long)snapshot.currentStats.schedulerNoteOffsCoalescedThisBlock,
          (unsigned long)snapshot.currentStats.schedulerNoteOffsCanceledThisBlock,
          (unsigned long)snapshot.currentStats.schedulerReleaseControlsAppliedThisBlock,
          (unsigned long)snapshot.currentStats.criticalQueueDepth,
          (unsigned long)snapshot.currentStats.realtimeQueueDepth,
          (unsigned long)snapshot.currentStats.noteOnQueueDepth,
          (unsigned long)snapshot.currentStats.releaseLaneDepth);
  SetEditText(g_ui.diagnosticsNoteOff, notes, true);

  for (int i = 0; i < 16; ++i) {
    char line[48];
    sprintf(line, "Ch %02d  %lu\r\n", i + 1, (unsigned long)snapshot.currentStats.activeVoices[i]);
    strncat(perChannel, line, sizeof(perChannel) - strlen(perChannel) - 1);
  }
  SetEditText(g_ui.diagnosticsPerChannel, perChannel, true);
}

void BuildProfilesSummary() {
  SetEditText(g_ui.profilesSummary,
              "Profiles\r\n"
              "Visible now by documentation, but persistence is not wired in this pass.\r\n"
              "Use the live Apply / Reload flow on Home for runtime changes.\r\n"
              "Preset slots, import/export, and startup profiles stay visible elsewhere as disabled placeholders.",
              true);
}

void BuildAdvancedSummary(const LiveBridgeSharedState &snapshot) {
  char buffer[2048];
  sprintf(buffer,
          "Advanced readout\r\n"
          "This tab is intentionally read-only for real settings so each writable setting has one owner control elsewhere.\r\n"
          "Engine=%s   Backend=%s   SampleRate=%ld   MaxVoices=%ld   Polling=%ld\r\n"
          "Timing=%s   AsyncStarts=%ld   WASAPIAsync=%ld\r\n"
          "MasterVolume=%.3f   VelocityCurve=%.3f   VelocityFloor=%.3f   VelocityIgnoreBelow=%ld\r\n"
          "Reverb enabled=%ld mix=%.3f feedback=%.3f tone=%.3f width=%.3f blur=%.3f\r\n"
          "Limiter enabled=%ld threshold=%.3f release=%.3f",
          snapshot.currentSettings.samplerEngine, snapshot.currentSettings.audioBackend,
          (long)snapshot.currentSettings.sampleRate, (long)snapshot.currentSettings.maxVoices,
          (long)snapshot.currentSettings.pollingRate, snapshot.currentSettings.eventTimingMode,
          (long)snapshot.currentSettings.asyncNoteStarts, (long)snapshot.currentSettings.wasapiAsyncFeed,
          snapshot.currentSettings.masterVolume, snapshot.currentSettings.velocityCurve,
          snapshot.currentSettings.velocityFloor, (long)snapshot.currentSettings.velocityIgnoreBelow,
          (long)snapshot.currentSettings.reverbEnabled, snapshot.currentSettings.reverbMix,
          snapshot.currentSettings.reverbFeedback, snapshot.currentSettings.reverbTone,
          snapshot.currentSettings.reverbWidth, snapshot.currentSettings.reverbBlur,
          (long)snapshot.currentSettings.limiterEnabled, snapshot.currentSettings.limiterThreshold,
          snapshot.currentSettings.limiterReleaseMs);
  SetEditText(g_ui.advancedSummary, buffer, true);
}

void BuildDeveloperSummary(const LiveBridgeSharedState &snapshot) {
  char buffer[2048];
  sprintf(buffer,
          "Developer internals\r\n"
          "Protocol version %lu   struct size %lu   heartbeat tick %lu\r\n"
          "command_request=%ld   processed=%ld   code=%ld   in_progress=%ld   result=%ld\r\n"
          "publisher_pid=%lu   source_pid=%lu\r\n"
          "runtime_reload_count=%lu   accurate_clock_resets=%lu   scheduler_state_preserved=%lu\r\n"
          "worker_blocked=%lu   perf_debug=%lu   warning_count=%lu\r\n"
          "TSF helpers c/g/x %lu/%lu/%lu   clustered c/g/x %lu/%lu/%lu   fragments single/threaded %lu/%lu\r\n"
          "Last message: %s",
          (unsigned long)snapshot.version, (unsigned long)snapshot.structSize,
          (unsigned long)snapshot.publisherHeartbeatTick, (long)snapshot.commandRequestId,
          (long)snapshot.commandProcessedId, (long)snapshot.commandCode,
          (long)snapshot.commandInProgress, (long)snapshot.commandResult,
          (unsigned long)snapshot.publisherPid, (unsigned long)snapshot.commandSourcePid,
          (unsigned long)snapshot.currentStats.runtimeReloadCount,
          (unsigned long)snapshot.currentStats.accurateClockResetCount,
          (unsigned long)snapshot.currentStats.schedulerStatePreservedCount,
          (unsigned long)snapshot.currentStats.accurateWorkerBlockedCount,
          (unsigned long)snapshot.currentStats.perfCountersEnabled,
          (unsigned long)snapshot.currentStats.samplerWarningCount,
          (unsigned long)snapshot.currentStats.tsfHelperContiguousBlocks,
          (unsigned long)snapshot.currentStats.tsfHelperGatherBlocks,
          (unsigned long)snapshot.currentStats.tsfHelperComplexBlocks,
          (unsigned long)snapshot.currentStats.tsfClusteredVoicesContiguous,
          (unsigned long)snapshot.currentStats.tsfClusteredVoicesGather,
          (unsigned long)snapshot.currentStats.tsfClusteredVoicesComplex,
          (unsigned long)snapshot.currentStats.tsfSingleThreadFragments,
          (unsigned long)snapshot.currentStats.tsfThreadedFragments,
          snapshot.commandMessage);
  SetEditText(g_ui.developerSummary, buffer, true);
}

void UpdateUiFromSnapshot(const LiveBridgeSharedState &snapshot) {
  g_ui.lastSnapshot = snapshot;
  g_ui.connected = true;
  AppendSnapshotHistory(snapshot);

  char status[512];
  sprintf(status, "Connected to PID %lu   |   Protocol v%lu   |   Engine %s   |   %s",
          (unsigned long)snapshot.publisherPid, (unsigned long)snapshot.version,
          snapshot.resolvedSamplerEngine[0] ? snapshot.resolvedSamplerEngine : snapshot.currentSettings.samplerEngine,
          snapshot.statusText);
  SetEditText(g_ui.statusText, status);

  if (!g_ui.seededEditors)
    SeedEditorsFromSnapshot(snapshot);

  BuildHeaderSummary(snapshot);
  BuildHomeSummary(snapshot);
  BuildTimingSummary(snapshot);
  BuildPerformanceSummary(snapshot);
  BuildFxSummary(snapshot);
  BuildDiagnosticsSummary(snapshot);
  BuildProfilesSummary();
  BuildAdvancedSummary(snapshot);
  BuildDeveloperSummary(snapshot);
  for (int i = 0; i < kGraphCount; ++i)
    InvalidateRect(g_ui.graphs[i], NULL, TRUE);
  UpdateDeveloperButtons();
}

void UpdateDisconnectedUi() {
  SetEditText(g_ui.statusText,
              g_ui.disconnectedStatus[0] ? g_ui.disconnectedStatus
                                         : "Waiting for a live SuperVirtualMIDISynth instance...");
  SetEditText(g_ui.headerSummary,
              g_ui.disconnectedSummary[0] ? g_ui.disconnectedSummary
                                          : "Launch a host that loads SuperVirtualMIDISynth, then Configurator V2 will attach automatically.",
              true);
  SetEditText(g_ui.homeSummary, "", true);
  SetEditText(g_ui.homeHealth, "", true);
  SetEditText(g_ui.resolvedSoundfontEdit, "", true);
  SetEditText(g_ui.timingSummary, "", true);
  SetEditText(g_ui.performanceSummary, "", true);
  SetEditText(g_ui.fxSummary, "", true);
  SetEditText(g_ui.diagnosticsRaw, "", true);
  SetEditText(g_ui.diagnosticsNoteOff, "", true);
  SetEditText(g_ui.diagnosticsPerChannel, "", true);
  SetEditText(g_ui.profilesSummary, "", true);
  SetEditText(g_ui.advancedSummary, "", true);
  SetEditText(g_ui.developerSummary, "", true);
  g_ui.seededEditors = false;
  UpdateDeveloperButtons();
}

void PollLiveState() {
  LiveBridgeSharedState snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  if (ReadSnapshot(snapshot))
    UpdateUiFromSnapshot(snapshot);
  else {
    DisconnectBridge();
    UpdateDisconnectedUi();
  }
}

void IssueApplyCommand() {
  LiveBridgeSettings settings;
  std::string message;
  BuildSettingsFromUi(settings);
  bool ok = SendCommandAndWait(DetermineApplyCommand(settings, g_ui.lastSnapshot),
                               settings, message);
  SetEditText(g_ui.statusText, message.c_str());
  if (ok) {
    g_ui.seededEditors = false;
    PollLiveState();
  }
}

void IssueSimpleCommand(LONG commandCode) {
  LiveBridgeSettings settings;
  memset(&settings, 0, sizeof(settings));
  std::string message;
  bool ok = SendCommandAndWait(commandCode, settings, message);
  SetEditText(g_ui.statusText, message.c_str());
  if (ok) {
    g_ui.seededEditors = false;
    PollLiveState();
  }
}

int BuildGraphSpec(int graphIndex, const char **title, const char **labels,
                   const HistorySeries **series, COLORREF *colors,
                   float *fixedMax) {
  *fixedMax = 0.0f;
  switch (graphIndex) {
  case GRAPH_RENDER:
    *title = "Render / Block / Budget";
    labels[0] = "Render"; labels[1] = "Block"; labels[2] = "Budget";
    series[0] = &g_ui.historyRenderMs; series[1] = &g_ui.historyBlockMs; series[2] = &g_ui.historyBudgetMs;
    colors[0] = RGB(0, 102, 204); colors[1] = RGB(0, 153, 102); colors[2] = RGB(204, 102, 0);
    return 3;
  case GRAPH_QUEUE:
    *title = "Queue / Deferred / Release Lane";
    labels[0] = "Queued"; labels[1] = "Deferred"; labels[2] = "Release";
    series[0] = &g_ui.historyQueueDepth; series[1] = &g_ui.historyDeferredDepth; series[2] = &g_ui.historyReleaseLaneDepth;
    colors[0] = RGB(0, 102, 204); colors[1] = RGB(170, 0, 170); colors[2] = RGB(204, 0, 0);
    return 3;
  case GRAPH_VS_MIX:
    *title = "VirtuallySuper Mix";
    labels[0] = "Exact"; labels[1] = "Released"; labels[2] = "Grouped"; labels[3] = "Density";
    series[0] = &g_ui.historyVsExact; series[1] = &g_ui.historyVsReleasedExact; series[2] = &g_ui.historyVsGrouped; series[3] = &g_ui.historyVsDensity;
    colors[0] = RGB(0, 102, 204); colors[1] = RGB(128, 128, 128); colors[2] = RGB(0, 153, 102); colors[3] = RGB(204, 102, 0);
    return 4;
  case GRAPH_OVERLOAD:
    *title = "Overload / VoiceEq";
    labels[0] = "Overload"; labels[1] = "VoiceEq";
    series[0] = &g_ui.historyOverloadState; series[1] = &g_ui.historyVsVoiceEq;
    colors[0] = RGB(204, 0, 0); colors[1] = RGB(0, 102, 204);
    return 2;
  default:
    *title = "Notes / Release Decisions";
    labels[0] = "Started"; labels[1] = "Dropped"; labels[2] = "OffApplied"; labels[3] = "OffCanceled";
    series[0] = &g_ui.historyNoteOnStarted; series[1] = &g_ui.historyNoteOnDropped; series[2] = &g_ui.historyNoteOffApplied; series[3] = &g_ui.historyNoteOffCanceled;
    colors[0] = RGB(0, 153, 102); colors[1] = RGB(204, 0, 0); colors[2] = RGB(0, 102, 204); colors[3] = RGB(170, 0, 170);
    return 4;
  }
}

void DrawGraph(HWND, HDC dc, RECT clientRect, int graphIndex) {
  FillRect(dc, &clientRect, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
  FrameRect(dc, &clientRect, reinterpret_cast<HBRUSH>(COLOR_WINDOWFRAME + 1));

  const char *title = "";
  const char *labels[4] = {"", "", "", ""};
  const HistorySeries *series[4] = {NULL, NULL, NULL, NULL};
  COLORREF colors[4] = {0, 0, 0, 0};
  float fixedMax = 0.0f;
  const int seriesCount = BuildGraphSpec(graphIndex, &title, labels, series, colors, &fixedMax);

  RECT titleRect = clientRect;
  titleRect.left += 8; titleRect.top += 4;
  SelectObject(dc, g_ui.bodyFont);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(32, 32, 32));
  DrawTextA(dc, title, -1, &titleRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

  RECT plotRect = clientRect;
  plotRect.left += 8; plotRect.right -= 8; plotRect.top += 24; plotRect.bottom -= 10;
  if (plotRect.right <= plotRect.left || plotRect.bottom <= plotRect.top)
    return;
  FrameRect(dc, &plotRect, reinterpret_cast<HBRUSH>(COLOR_3DSHADOW + 1));

  const int width = plotRect.right - plotRect.left - 2;
  const int height = plotRect.bottom - plotRect.top - 2;
  if (width <= 1 || height <= 1)
    return;

  float maxValue = fixedMax;
  if (maxValue <= 0.0f) {
    for (int s = 0; s < seriesCount; ++s) {
      if (!series[s]) continue;
      for (int i = 0; i < series[s]->count; ++i) {
        float value = GetHistoryValue(*series[s], i);
        if (value > maxValue) maxValue = value;
      }
    }
  }
  if (graphIndex == GRAPH_OVERLOAD && maxValue < 2.0f) maxValue = 2.0f;
  if (maxValue <= 0.0f) maxValue = 1.0f;

  for (int s = 0; s < seriesCount; ++s) {
    if (!series[s] || series[s]->count < 2) continue;
    HPEN pen = CreatePen(PS_SOLID, 1, colors[s]);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    for (int i = 0; i < series[s]->count; ++i) {
      float value = GetHistoryValue(*series[s], i);
      int x = plotRect.left + 1 + ((series[s]->count <= 1) ? 0 : (i * width) / (series[s]->count - 1));
      int y = plotRect.bottom - 1 - (int)((value / maxValue) * (float)height);
      if (y < plotRect.top + 1) y = plotRect.top + 1;
      if (y > plotRect.bottom - 1) y = plotRect.bottom - 1;
      if (i == 0) MoveToEx(dc, x, y, NULL);
      else LineTo(dc, x, y);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
  }

  int legendX = clientRect.left + 8;
  const int legendY = clientRect.bottom - 18;
  for (int s = 0; s < seriesCount; ++s) {
    HPEN pen = CreatePen(PS_SOLID, 2, colors[s]);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    MoveToEx(dc, legendX, legendY + 6, NULL);
    LineTo(dc, legendX + 14, legendY + 6);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    RECT labelRect = {legendX + 18, legendY - 2, legendX + 82, legendY + 14};
    DrawTextA(dc, labels[s], -1, &labelRect, DT_LEFT | DT_SINGLELINE);
    legendX += 82;
  }
}

LRESULT CALLBACK GraphProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE: {
    const CREATESTRUCTA *create = reinterpret_cast<const CREATESTRUCTA *>(lParam);
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return 0;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rect;
    GetClientRect(hwnd, &rect);
    DrawGraph(hwnd, dc, rect, (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    EndPaint(hwnd, &ps);
    return 0;
  }
  }
  return DefWindowProcA(hwnd, msg, wParam, lParam);
}

HWND CreateGraphWindow(HWND parent, int controlId, int graphIndex, int x, int y,
                       int w, int h) {
  HWND hwnd = CreateWindowExA(0, "SVMSV2Graph", "",
                              WS_CHILD | WS_VISIBLE | WS_BORDER, x, y, w, h,
                              parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              GetModuleHandle(NULL),
                              reinterpret_cast<void *>(static_cast<INT_PTR>(graphIndex)));
  RegisterModeControl(hwnd, VIEW_BASIC);
  return hwnd;
}

void BuildUi(HWND hwnd) {
  g_ui.hwnd = hwnd;
  g_ui.titleFont = CreateAppFont(18, FW_BOLD, "Segoe UI");
  g_ui.sectionFont = CreateAppFont(11, FW_BOLD, "Segoe UI");
  g_ui.bodyFont = CreateAppFont(9, FW_NORMAL, "Segoe UI");
  g_ui.monoFont = CreateAppFont(9, FW_NORMAL, "Consolas");
  g_ui.currentMode = VIEW_DEVELOPER;
  g_ui.currentTab = TAB_HOME;
  SetDisconnectedState("Waiting for a live SuperVirtualMIDISynth instance...",
                       "Launch a host using SuperVirtualMIDISynth, then Configurator V2 will attach automatically.");

  HWND title = CreateWindowExA(0, "STATIC", "SuperVirtualMIDISynth Configurator V2",
                               WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 18, 14, 520, 28,
                               hwnd, NULL, GetModuleHandle(NULL), NULL);
  SetControlFont(title, g_ui.titleFont);

  g_ui.statusText = CreateModeEdit(hwnd, IDC_STATUS, 18, 48, 1336, 24, ES_AUTOHSCROLL | ES_READONLY, VIEW_BASIC);
  g_ui.headerSummary = CreateModeEdit(hwnd, IDC_HEADER_SUMMARY, 18, 80, 1336, 58,
                                      ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_BASIC);
  g_ui.modeTab = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 18, 148, 420, 32, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MODE_TAB)),
                                 GetModuleHandle(NULL), NULL);
  g_ui.productTab = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    18, 184, 1336, 684, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRODUCT_TAB)),
                                    GetModuleHandle(NULL), NULL);
  SetControlFont(g_ui.modeTab, g_ui.bodyFont);
  SetControlFont(g_ui.productTab, g_ui.bodyFont);

  TCITEMA item;
  memset(&item, 0, sizeof(item));
  item.mask = TCIF_TEXT;
  const char *modeNames[] = {"Basic", "Advanced", "PowerUser", "Developer"};
  for (int i = 0; i < 4; ++i) {
    item.pszText = const_cast<char *>(modeNames[i]);
    TabCtrl_InsertItem(g_ui.modeTab, i, &item);
  }
  const char *tabNames[TAB_COUNT] = {"Home", "SoundFonts", "Engine", "Timing", "Performance",
                                     "FX And Output", "Diagnostics", "Profiles",
                                     "Advanced", "Developer"};
  for (int i = 0; i < TAB_COUNT; ++i) {
    item.pszText = const_cast<char *>(tabNames[i]);
    TabCtrl_InsertItem(g_ui.productTab, i, &item);
  }
  TabCtrl_SetCurSel(g_ui.modeTab, VIEW_DEVELOPER);
  TabCtrl_SetCurSel(g_ui.productTab, TAB_HOME);

  RECT panelRect;
  GetClientRect(g_ui.productTab, &panelRect);
  TabCtrl_AdjustRect(g_ui.productTab, FALSE, &panelRect);
  for (int i = 0; i < TAB_COUNT; ++i) {
    g_ui.panels[i] = CreateWindowExA(WS_EX_CONTROLPARENT, "STATIC", "",
                                     WS_CHILD | (i == TAB_HOME ? WS_VISIBLE : 0),
                                     panelRect.left, panelRect.top,
                                     panelRect.right - panelRect.left,
                                     panelRect.bottom - panelRect.top, g_ui.productTab,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HOME_PANEL + i)),
                                     GetModuleHandle(NULL), NULL);
  }

  // Home
  HWND panel = g_ui.panels[TAB_HOME];
  CreateModeGroupBox(panel, "Quick Actions", 12, 10, 280, 82, VIEW_BASIC);
  g_ui.homeApplyButton = CreateModeButton(panel, "Apply", IDC_HOME_APPLY, 24, 34, 110, 26, VIEW_BASIC);
  g_ui.homeReloadButton = CreateModeButton(panel, "Reload Config", IDC_HOME_RELOAD, 146, 34, 118, 26, VIEW_BASIC);
  CreateModeGroupBox(panel, "Health", 306, 10, 500, 112, VIEW_BASIC);
  g_ui.homeHealth = CreateModeEdit(panel, IDC_HOME_HEALTH, 320, 34, 470, 74, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_BASIC);
  CreateModeGroupBox(panel, "Summary", 12, 132, 1280, 204, VIEW_BASIC);
  g_ui.homeSummary = CreateModeEdit(panel, IDC_HOME_SUMMARY, 24, 156, 1254, 166, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_BASIC);

  // SoundFonts
  panel = g_ui.panels[TAB_SOUNDFONTS];
  CreateModeGroupBox(panel, "Active Source", 12, 10, 1280, 126, VIEW_BASIC);
  CreateModeLabel(panel, "SoundFont / Source Path", 24, 34, 220, 18, VIEW_BASIC);
  g_ui.soundfontEdit = CreateModeEdit(panel, IDC_SF_PATH, 24, 56, 1020, 24, ES_AUTOHSCROLL, VIEW_BASIC);
  g_ui.soundfontBrowseButton = CreateModeButton(panel, "Browse", IDC_SF_BROWSE, 1054, 55, 96, 26, VIEW_BASIC);
  CreateModeLabel(panel, "Resolved Source", 24, 88, 220, 18, VIEW_BASIC);
  g_ui.resolvedSoundfontEdit = CreateModeEdit(panel, IDC_SF_RESOLVED, 24, 108, 1126, 22, ES_AUTOHSCROLL | ES_READONLY, VIEW_BASIC);
  CreateModeGroupBox(panel, "Future Source Management", 12, 150, 1280, 154, VIEW_ADVANCED);
  CreatePlaceholderField(panel, "Folder roots", "Not wired yet", 24, 176, 360, VIEW_ADVANCED);
  CreatePlaceholderField(panel, "Source order / fallback", "Not wired yet", 404, 176, 360, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Preload / metadata cache", "Not wired yet", 784, 176, 360, VIEW_POWERUSER);

  // Engine
  panel = g_ui.panels[TAB_ENGINE];
  CreateModeGroupBox(panel, "Runtime Engine", 12, 10, 600, 214, VIEW_BASIC);
  CreateModeLabel(panel, "Audio Backend", 24, 34, 120, 18, VIEW_BASIC);
  g_ui.backendCombo = CreateModeCombo(panel, IDC_BACKEND_COMBO, 24, 56, 180, 300, false, VIEW_BASIC);
  PopulateBackendCombo(g_ui.backendCombo);
  CreateModeLabel(panel, "Sampler Engine", 224, 34, 120, 18, VIEW_BASIC);
  g_ui.samplerEngineCombo = CreateModeCombo(panel, IDC_ENGINE_COMBO, 224, 56, 180, 300, false, VIEW_BASIC);
  PopulateSamplerEngineCombo(g_ui.samplerEngineCombo);
  CreateModeLabel(panel, "Sample Rate", 424, 34, 120, 18, VIEW_ADVANCED);
  g_ui.sampleRateCombo = CreateModeCombo(panel, IDC_SAMPLE_RATE_COMBO, 424, 56, 150, 240, true, VIEW_ADVANCED);
  PopulateSampleRateCombo(g_ui.sampleRateCombo);
  CreateModeLabel(panel, "Max Voices", 24, 96, 120, 18, VIEW_ADVANCED);
  g_ui.maxVoicesEdit = CreateModeEdit(panel, IDC_MAX_VOICES_EDIT, 24, 118, 150, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeLabel(panel, "Polling Rate", 194, 96, 120, 18, VIEW_ADVANCED);
  g_ui.pollingRateEdit = CreateModeEdit(panel, IDC_POLLING_RATE_EDIT, 194, 118, 150, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  g_ui.asyncNoteStartsCheck = CreateModeButton(panel, "Async note starts", IDC_ASYNC_NOTE_STARTS, 24, 162, 180, 20, VIEW_POWERUSER, BS_AUTOCHECKBOX);
  g_ui.wasapiAsyncFeedCheck = CreateModeButton(panel, "WASAPI async feed", IDC_WASAPI_ASYNC, 224, 162, 180, 20, VIEW_POWERUSER, BS_AUTOCHECKBOX);
  CreateModeGroupBox(panel, "Future Engine Controls", 630, 10, 662, 214, VIEW_ADVANCED);
  CreatePlaceholderField(panel, "Output device", "Not wired yet", 642, 34, 260, VIEW_ADVANCED);
  CreatePlaceholderField(panel, "Worker count", "Not wired yet", 922, 34, 160, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Quality profile", "Not wired yet", 1102, 34, 160, VIEW_POWERUSER);

  // Timing
  panel = g_ui.panels[TAB_TIMING];
  CreateModeGroupBox(panel, "Timing", 12, 10, 540, 130, VIEW_BASIC);
  CreateModeLabel(panel, "Event Timing Mode", 24, 34, 160, 18, VIEW_ADVANCED);
  g_ui.timingCombo = CreateModeCombo(panel, IDC_TIMING_COMBO, 24, 56, 180, 240, false, VIEW_ADVANCED);
  PopulateTimingModeCombo(g_ui.timingCombo);
  g_ui.timingSummary = CreateModeEdit(panel, IDC_TIMING_SUMMARY, 224, 34, 314, 82, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_BASIC);
  CreateModeGroupBox(panel, "Documented Policies", 12, 154, 1280, 150, VIEW_ADVANCED);
  CreatePlaceholderField(panel, "Same-key policy", "Not wired yet", 24, 180, 260, VIEW_ADVANCED);
  CreatePlaceholderField(panel, "Buzz guard / retrigger rules", "Not wired yet", 304, 180, 300, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Sustain / release policy", "Not wired yet", 624, 180, 280, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Reset / panic ordering", "Not wired yet", 924, 180, 260, VIEW_DEVELOPER);

  // Performance
  panel = g_ui.panels[TAB_PERFORMANCE];
  CreateModeGroupBox(panel, "Velocity", 12, 10, 540, 186, VIEW_BASIC);
  CreateModeLabel(panel, "Velocity Curve", 24, 34, 120, 18, VIEW_BASIC);
  g_ui.velocityCurveEdit = CreateModeEdit(panel, IDC_VELOCITY_CURVE_EDIT, 24, 56, 150, 24, ES_AUTOHSCROLL, VIEW_BASIC);
  CreateModeLabel(panel, "Velocity Floor", 194, 34, 120, 18, VIEW_ADVANCED);
  g_ui.velocityFloorEdit = CreateModeEdit(panel, IDC_VELOCITY_FLOOR_EDIT, 194, 56, 150, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeLabel(panel, "Ignore Below", 364, 34, 120, 18, VIEW_ADVANCED);
  g_ui.velocityIgnoreEdit = CreateModeEdit(panel, IDC_VELOCITY_IGNORE_EDIT, 364, 56, 150, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeGroupBox(panel, "Live Diagnostics", 570, 10, 722, 186, VIEW_BASIC);
  g_ui.performanceSummary = CreateModeEdit(panel, IDC_PERFORMANCE_SUMMARY, 582, 34, 696, 148, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_BASIC);
  CreateModeGroupBox(panel, "Future Performance Surfaces", 12, 210, 1280, 150, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Overload policy editor", "Not wired yet", 24, 236, 260, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Tile / chunk tuning", "Not wired yet", 304, 236, 260, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Grouped / density controls", "Not wired yet", 584, 236, 280, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Worker routing", "Not wired yet", 884, 236, 220, VIEW_DEVELOPER);

  // FX and Output
  panel = g_ui.panels[TAB_FX_OUTPUT];
  CreateModeGroupBox(panel, "Master And FX", 12, 10, 680, 254, VIEW_BASIC);
  CreateModeLabel(panel, "Master Volume", 24, 34, 120, 18, VIEW_BASIC);
  g_ui.masterVolumeEdit = CreateModeEdit(panel, IDC_MASTER_VOLUME_EDIT, 24, 56, 140, 24, ES_AUTOHSCROLL, VIEW_BASIC);
  g_ui.reverbEnabledCheck = CreateModeButton(panel, "Enable reverb", IDC_REVERB_ENABLED, 24, 96, 140, 20, VIEW_ADVANCED, BS_AUTOCHECKBOX);
  CreateModeLabel(panel, "Mix", 24, 126, 80, 18, VIEW_ADVANCED);
  g_ui.reverbMixEdit = CreateModeEdit(panel, IDC_REVERB_MIX_EDIT, 24, 146, 90, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeLabel(panel, "Feedback", 124, 126, 80, 18, VIEW_ADVANCED);
  g_ui.reverbFeedbackEdit = CreateModeEdit(panel, IDC_REVERB_FEEDBACK_EDIT, 124, 146, 90, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeLabel(panel, "Tone", 224, 126, 80, 18, VIEW_ADVANCED);
  g_ui.reverbToneEdit = CreateModeEdit(panel, IDC_REVERB_TONE_EDIT, 224, 146, 90, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeLabel(panel, "Width", 324, 126, 80, 18, VIEW_ADVANCED);
  g_ui.reverbWidthEdit = CreateModeEdit(panel, IDC_REVERB_WIDTH_EDIT, 324, 146, 90, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeLabel(panel, "Blur", 424, 126, 80, 18, VIEW_ADVANCED);
  g_ui.reverbBlurEdit = CreateModeEdit(panel, IDC_REVERB_BLUR_EDIT, 424, 146, 90, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  g_ui.limiterEnabledCheck = CreateModeButton(panel, "Enable limiter", IDC_LIMITER_ENABLED, 24, 188, 140, 20, VIEW_ADVANCED, BS_AUTOCHECKBOX);
  CreateModeLabel(panel, "Threshold", 24, 214, 90, 18, VIEW_ADVANCED);
  g_ui.limiterThresholdEdit = CreateModeEdit(panel, IDC_LIMITER_THRESHOLD_EDIT, 24, 234, 90, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeLabel(panel, "Release ms", 124, 214, 90, 18, VIEW_ADVANCED);
  g_ui.limiterReleaseEdit = CreateModeEdit(panel, IDC_LIMITER_RELEASE_EDIT, 124, 234, 90, 24, ES_AUTOHSCROLL, VIEW_ADVANCED);
  CreateModeGroupBox(panel, "Live Output Summary", 710, 10, 582, 254, VIEW_BASIC);
  g_ui.fxSummary = CreateModeEdit(panel, IDC_FX_SUMMARY, 722, 34, 556, 216, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_BASIC);
  CreateModeGroupBox(panel, "Future Output Shaping", 12, 278, 1280, 120, VIEW_ADVANCED);
  CreatePlaceholderField(panel, "Output device selector", "Not wired yet", 24, 304, 260, VIEW_ADVANCED);
  CreatePlaceholderField(panel, "Channel / bus shaping", "Not wired yet", 304, 304, 280, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Per-endpoint profiles", "Not wired yet", 604, 304, 260, VIEW_POWERUSER);

  // Diagnostics
  panel = g_ui.panels[TAB_DIAGNOSTICS];
  g_ui.graphs[GRAPH_RENDER] = CreateGraphWindow(panel, IDC_GRAPH_RENDER, GRAPH_RENDER, 12, 12, 400, 120);
  g_ui.graphs[GRAPH_QUEUE] = CreateGraphWindow(panel, IDC_GRAPH_QUEUE, GRAPH_QUEUE, 426, 12, 400, 120);
  g_ui.graphs[GRAPH_VS_MIX] = CreateGraphWindow(panel, IDC_GRAPH_VS, GRAPH_VS_MIX, 840, 12, 440, 120);
  g_ui.graphs[GRAPH_OVERLOAD] = CreateGraphWindow(panel, IDC_GRAPH_OVERLOAD, GRAPH_OVERLOAD, 12, 144, 634, 120);
  g_ui.graphs[GRAPH_NOTES] = CreateGraphWindow(panel, IDC_GRAPH_NOTES, GRAPH_NOTES, 660, 144, 620, 120);
  g_ui.diagnosticsRaw = CreateModeEdit(panel, IDC_DIAG_RAW, 12, 278, 820, 330,
                                       ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                       VIEW_ADVANCED);
  SetControlFont(g_ui.diagnosticsRaw, g_ui.monoFont);
  g_ui.diagnosticsNoteOff = CreateModeEdit(panel, IDC_DIAG_NOTE_OFF, 846, 278, 434, 128,
                                           ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                           VIEW_BASIC);
  g_ui.diagnosticsPerChannel = CreateModeEdit(panel, IDC_DIAG_PER_CHANNEL, 846, 422, 434, 186,
                                              ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                              VIEW_BASIC);
  SetControlFont(g_ui.diagnosticsPerChannel, g_ui.monoFont);

  // Profiles
  panel = g_ui.panels[TAB_PROFILES];
  g_ui.profilesSummary = CreateModeEdit(panel, IDC_PROFILES_SUMMARY, 12, 12, 1280, 220,
                                        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_BASIC);

  // Advanced
  panel = g_ui.panels[TAB_ADVANCED];
  g_ui.advancedSummary = CreateModeEdit(panel, IDC_ADVANCED_SUMMARY, 12, 12, 1280, 210,
                                        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_ADVANCED);
  CreateModeGroupBox(panel, "Future Advanced Surfaces", 12, 236, 1280, 144, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Engine topology knobs", "Not wired yet", 24, 262, 260, VIEW_POWERUSER);
  CreatePlaceholderField(panel, "Scheduler stress knobs", "Not wired yet", 304, 262, 280, VIEW_DEVELOPER);
  CreatePlaceholderField(panel, "Source policy presets", "Not wired yet", 604, 262, 260, VIEW_POWERUSER);

  // Developer
  panel = g_ui.panels[TAB_DEVELOPER];
  g_ui.developerSummary = CreateModeEdit(panel, IDC_DEVELOPER_SUMMARY, 12, 12, 1280, 200,
                                         ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, VIEW_DEVELOPER);
  g_ui.developerEnableCheck = CreateModeButton(panel, "Enable Developer Controls", IDC_DEVELOPER_ENABLE, 12, 228, 220, 22, VIEW_DEVELOPER, BS_AUTOCHECKBOX);
  g_ui.developerResetButton = CreateModeButton(panel, "Hard Reset", IDC_DEVELOPER_RESET, 250, 224, 120, 28, VIEW_DEVELOPER);
  g_ui.developerKillButton = CreateModeButton(panel, "Kill Engine", IDC_DEVELOPER_KILL, 382, 224, 120, 28, VIEW_DEVELOPER);
  CreateModeGroupBox(panel, "Future Developer Surfaces", 12, 270, 1280, 136, VIEW_DEVELOPER);
  CreatePlaceholderField(panel, "Raw shared-memory editor", "Not wired yet", 24, 296, 280, VIEW_DEVELOPER);
  CreatePlaceholderField(panel, "Synthetic stress harness", "Not wired yet", 324, 296, 260, VIEW_DEVELOPER);
  CreatePlaceholderField(panel, "Trace export hooks", "Not wired yet", 604, 296, 260, VIEW_DEVELOPER);

  SelectProductTab(TAB_HOME);
  SelectModeTab(VIEW_DEVELOPER);
  UpdateDisconnectedUi();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE:
    BuildUi(hwnd);
    SetTimer(hwnd, kLivePollTimerId, kLivePollIntervalMs, NULL);
    return 0;
  case WM_TIMER:
    if (wParam == kLivePollTimerId)
      PollLiveState();
    return 0;
  case WM_NOTIFY: {
    const NMHDR *hdr = reinterpret_cast<const NMHDR *>(lParam);
    if (!hdr) break;
    if (hdr->idFrom == IDC_MODE_TAB && hdr->code == TCN_SELCHANGE) {
      SelectModeTab(TabCtrl_GetCurSel(g_ui.modeTab));
      return 0;
    }
    if (hdr->idFrom == IDC_PRODUCT_TAB && hdr->code == TCN_SELCHANGE) {
      SelectProductTab(TabCtrl_GetCurSel(g_ui.productTab));
      return 0;
    }
    break;
  }
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDC_HOME_APPLY: IssueApplyCommand(); return 0;
    case IDC_HOME_RELOAD: IssueSimpleCommand(LIVE_CMD_RELOAD_CONFIG); return 0;
    case IDC_SF_BROWSE: BrowseForSoundfont(); return 0;
    case IDC_DEVELOPER_ENABLE:
      g_ui.developerControlsEnabled = IsCheckboxChecked(g_ui.developerEnableCheck);
      UpdateDeveloperButtons();
      return 0;
    case IDC_DEVELOPER_RESET: IssueSimpleCommand(LIVE_CMD_RESET_ENGINE); return 0;
    case IDC_DEVELOPER_KILL: IssueSimpleCommand(LIVE_CMD_KILL_ENGINE); return 0;
    }
    return 0;
  case WM_DESTROY:
    KillTimer(hwnd, kLivePollTimerId);
    DisconnectBridge();
    if (g_ui.titleFont) DeleteObject(g_ui.titleFont);
    if (g_ui.sectionFont) DeleteObject(g_ui.sectionFont);
    if (g_ui.bodyFont) DeleteObject(g_ui.bodyFont);
    if (g_ui.monoFont) DeleteObject(g_ui.monoFont);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
  INITCOMMONCONTROLSEX icc;
  memset(&icc, 0, sizeof(icc));
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
  InitCommonControlsEx(&icc);

  WNDCLASSA graphClass;
  memset(&graphClass, 0, sizeof(graphClass));
  graphClass.lpfnWndProc = GraphProc;
  graphClass.hInstance = hInstance;
  graphClass.hCursor = LoadCursor(NULL, IDC_ARROW);
  graphClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  graphClass.lpszClassName = "SVMSV2Graph";
  RegisterClassA(&graphClass);

  WNDCLASSA wc;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = "SVMSConfiguratorV2Window";
  if (!RegisterClassA(&wc))
    return 1;

  HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "SVMS Configurator V2",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                  WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1390, 930, NULL,
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
