#include "LiveConfigProtocol.h"

#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

static const UINT_PTR kLivePollTimerId = 1;
static const UINT kLivePollIntervalMs = 50;

enum ControlId {
  IDC_STATUS = 100,
  IDC_TAB,
  IDC_HOME_PANEL,
  IDC_DIAG_PANEL,
  IDC_ENGINE_LABEL,
  IDC_ENGINE_COMBO,
  IDC_TIMING_LABEL,
  IDC_TIMING_COMBO,
  IDC_SOURCE_LABEL,
  IDC_SOURCE_EDIT,
  IDC_HOME_SUMMARY,
  IDC_DIAG_SUMMARY,
  IDC_PER_CHANNEL,
  IDC_APPLY,
  IDC_RELOAD,
  IDC_RESET,
  IDC_KILL
};

struct UiState {
  HWND hwnd;
  HWND statusText;
  HWND tab;
  HWND homePanel;
  HWND diagPanel;
  HWND engineCombo;
  HWND timingCombo;
  HWND sourceEdit;
  HWND homeSummary;
  HWND diagSummary;
  HWND perChannel;
  HWND applyButton;
  HWND reloadButton;
  HWND resetButton;
  HWND killButton;
  HFONT titleFont;
  HFONT bodyFont;
  HANDLE mappingHandle;
  HANDLE mutexHandle;
  LiveBridgeSharedState *sharedState;
  LiveBridgeSharedState lastSnapshot;
  bool connected;
  bool seededEditors;
  bool versionMismatch;
  char disconnectedStatus[SVMS_MAX_STATUS_TEXT];
  char disconnectedSummary[SVMS_MAX_STATUS_TEXT];
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

void SetDisconnectedState(const char *status, const char *summary,
                          bool versionMismatch = false) {
  CopyCString(g_ui.disconnectedStatus, sizeof(g_ui.disconnectedStatus), status);
  CopyCString(g_ui.disconnectedSummary, sizeof(g_ui.disconnectedSummary),
              summary);
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
  const char *legacyMappings[] = {SVMS_LIVE_BRIDGE_MAPPING_NAME_V20,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V19,
                                  SVMS_LIVE_BRIDGE_MAPPING_NAME_V18};
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
          "Bridge version mismatch",
          "The running synth is using an older live bridge version. Rebuild "
          "and restart both the DLL and Configurators together.",
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
                         "The bridge exists, but the shared memory view "
                         "could not be opened.");
    return false;
  }

  g_ui.mutexHandle =
      OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                 SVMS_LIVE_BRIDGE_MUTEX_NAME);
  if (!g_ui.mutexHandle) {
    DisconnectBridge();
    SetDisconnectedState("Failed to open bridge mutex",
                         "The shared bridge mutex could not be opened.");
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
        "Bridge layout mismatch",
        "This Configurator V2 does not match the running synth bridge layout.",
        true);
    return false;
  }
  return snapshot.runtimeLoaded != 0 && snapshot.publisherPid != 0;
}

void SetEditText(HWND hwnd, const char *text) {
  SetWindowTextA(hwnd, text ? text : "");
}

std::string GetWindowString(HWND hwnd) {
  char buffer[SVMS_MAX_PATH_TEXT];
  GetWindowTextA(hwnd, buffer, sizeof(buffer));
  return buffer;
}

std::string GetComboSelectionString(HWND hwnd) {
  if (!hwnd)
    return std::string();
  int selected = (int)SendMessageA(hwnd, CB_GETCURSEL, 0, 0);
  if (selected != CB_ERR) {
    char buffer[SVMS_MAX_BACKEND_TEXT] = {};
    SendMessageA(hwnd, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(buffer));
    return std::string(buffer);
  }
  return GetWindowString(hwnd);
}

void SelectComboString(HWND hwnd, const char *text) {
  if (!hwnd)
    return;
  LRESULT result =
      SendMessageA(hwnd, CB_SELECTSTRING, (WPARAM)-1,
                   reinterpret_cast<LPARAM>(text ? text : ""));
  if (result == CB_ERR)
    SetWindowTextA(hwnd, text ? text : "");
}

void PopulateSamplerEngineCombo(HWND comboBox) {
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("auto"));
  SendMessageA(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("tsf"));
  SendMessageA(comboBox, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>("bassmidi"));
  SendMessageA(comboBox, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>("virtuallysuper"));
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

void SeedEditorsFromSnapshot(const LiveBridgeSharedState &snapshot) {
  SelectComboString(g_ui.engineCombo, snapshot.currentSettings.samplerEngine);
  SelectComboString(g_ui.timingCombo,
                    snapshot.currentSettings.eventTimingMode[0]
                        ? snapshot.currentSettings.eventTimingMode
                        : "accurate");
  SetEditText(g_ui.sourceEdit,
              snapshot.resolvedSoundfontPath[0]
                  ? snapshot.resolvedSoundfontPath
                  : snapshot.currentSettings.soundfontPath);
  g_ui.seededEditors = true;
}

void SelectTab(int index) {
  ShowWindow(g_ui.homePanel, index == 0 ? SW_SHOW : SW_HIDE);
  ShowWindow(g_ui.diagPanel, index == 1 ? SW_SHOW : SW_HIDE);
}

void UpdateUiFromSnapshot(const LiveBridgeSharedState &snapshot) {
  g_ui.lastSnapshot = snapshot;
  g_ui.connected = true;

  char status[256];
  sprintf(status, "Connected to PID %lu  |  Engine %s  |  %s",
          static_cast<unsigned long>(snapshot.publisherPid),
          snapshot.resolvedSamplerEngine[0] ? snapshot.resolvedSamplerEngine
                                            : snapshot.currentSettings.samplerEngine,
          snapshot.statusText);
  SetEditText(g_ui.statusText, status);

  if (!g_ui.seededEditors)
    SeedEditorsFromSnapshot(snapshot);

  char summary[1024];
  sprintf(
      summary,
      "Engine %s   Backend %s\r\n"
      "Source %s\r\n"
      "Timing %s   Async %s\r\n"
      "Render %.3f ms   Block %.3f ms / budget %.3f ms\r\n"
      "Voices %lu   Queue %lu   Overload %lu\r\n"
      "VirtuallySuper   Exact %lu   Grouped %lu   Density %lu   VoiceEq %lu   Pressure %lu   State %lu   Err %lu\r\n"
      "Warning %s",
      snapshot.resolvedSamplerEngine[0] ? snapshot.resolvedSamplerEngine
                                        : snapshot.currentSettings.samplerEngine,
      snapshot.resolvedAudioBackend[0] ? snapshot.resolvedAudioBackend
                                       : snapshot.currentSettings.audioBackend,
      snapshot.resolvedSoundfontPath[0] ? snapshot.resolvedSoundfontPath
                                        : snapshot.currentSettings.soundfontPath,
      snapshot.currentSettings.eventTimingMode[0]
          ? snapshot.currentSettings.eventTimingMode
          : "accurate",
      snapshot.currentStats.asyncNoteStartsEnabled ? "On" : "Off",
      snapshot.currentStats.synthRenderMs, snapshot.currentStats.audioBlockMs,
      snapshot.currentStats.audioBudgetMs,
      static_cast<unsigned long>(snapshot.currentStats.totalActiveVoices),
      static_cast<unsigned long>(snapshot.currentStats.queuedMidiEvents),
      static_cast<unsigned long>(snapshot.currentStats.overloadState),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperExactVoices),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperGroupedObjects),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperDensityObjects),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperVoiceEquivalent),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperPressureLevel),
      static_cast<unsigned long>(snapshot.currentStats.samplerStateCode),
      static_cast<unsigned long>(snapshot.currentStats.samplerErrorCode),
      snapshot.samplerLastWarning[0] ? snapshot.samplerLastWarning : "None");
  SetEditText(g_ui.homeSummary, summary);

  char diagnostics[1200];
  sprintf(
      diagnostics,
      "Render avg %.3f ms   peak %.3f ms\r\n"
      "Audio avg %.3f ms   peak %.3f ms\r\n"
      "Scheduler due %lu   late %lu   lag %lu samples\r\n"
      "Same-key pending %lu   max depth %lu\r\n"
      "Events processed %lu   NoteOn started %lu   NoteOff %lu\r\n"
      "VS released exact %lu   grouped %lu   density %lu   voice eq %lu\r\n"
      "VS pressure %lu   state %lu   err %lu\r\n"
      "Scheduler queue %lu   sampler warning count %lu\r\n"
      "Warning %s",
      snapshot.currentStats.synthRenderAvgMs,
      snapshot.currentStats.synthRenderPeakMs,
      snapshot.currentStats.audioBlockAvgMs,
      snapshot.currentStats.audioBlockPeakMs,
      static_cast<unsigned long>(snapshot.currentStats.schedulerDueEventsThisBlock),
      static_cast<unsigned long>(snapshot.currentStats.schedulerLateEventsThisBlock),
      static_cast<unsigned long>(snapshot.currentStats.schedulerLagSamples),
      static_cast<unsigned long>(
          snapshot.currentStats.schedulerPendingSameKeyTransitions),
      static_cast<unsigned long>(
          snapshot.currentStats.schedulerMaxSameKeyQueueDepth),
      static_cast<unsigned long>(snapshot.currentStats.eventsProcessedThisBlock),
      static_cast<unsigned long>(snapshot.currentStats.noteOnStartedThisBlock),
      static_cast<unsigned long>(snapshot.currentStats.noteOffEventsThisBlock),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperReleasedExactVoices),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperGroupedObjects),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperDensityObjects),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperVoiceEquivalent),
      static_cast<unsigned long>(
          snapshot.currentStats.virtuallySuperPressureLevel),
      static_cast<unsigned long>(snapshot.currentStats.samplerStateCode),
      static_cast<unsigned long>(snapshot.currentStats.samplerErrorCode),
      static_cast<unsigned long>(
          snapshot.currentStats.schedulerPendingSameKeyTransitions),
      static_cast<unsigned long>(snapshot.currentStats.samplerWarningCount),
      snapshot.samplerLastWarning[0] ? snapshot.samplerLastWarning : "None");
  SetEditText(g_ui.diagSummary, diagnostics);

  char perChannel[512];
  perChannel[0] = '\0';
  for (int i = 0; i < 16; ++i) {
    char line[48];
    sprintf(line, "Ch %02d   %lu\r\n", i + 1,
            static_cast<unsigned long>(snapshot.currentStats.activeVoices[i]));
    strncat(perChannel, line, sizeof(perChannel) - strlen(perChannel) - 1);
  }
  SetEditText(g_ui.perChannel, perChannel);
}

void UpdateDisconnectedUi() {
  SetEditText(g_ui.statusText,
              g_ui.disconnectedStatus[0]
                  ? g_ui.disconnectedStatus
                  : "Waiting for a live SuperVirtualMIDISynth instance...");
  SetEditText(g_ui.homeSummary,
              g_ui.disconnectedSummary[0]
                  ? g_ui.disconnectedSummary
                  : "Launch a host using SuperVirtualMIDISynth, then this "
                    "window will attach automatically.");
  SetEditText(g_ui.diagSummary, "");
  SetEditText(g_ui.perChannel, "");
  g_ui.seededEditors = false;
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
  CopyCString(settings.samplerEngine, sizeof(settings.samplerEngine),
              GetComboSelectionString(g_ui.engineCombo).c_str());
  CopyCString(settings.eventTimingMode, sizeof(settings.eventTimingMode),
              GetComboSelectionString(g_ui.timingCombo).c_str());
  settings.asyncNoteStarts =
      strcmp(settings.eventTimingMode, "legacy-sync") != 0 ? 1 : 0;
}

LONG DetermineApplyCommand(const LiveBridgeSettings &settings,
                           const LiveBridgeSharedState &snapshot) {
  if (strcmp(settings.samplerEngine, snapshot.currentSettings.samplerEngine) != 0)
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
      UpdateUiFromSnapshot(snapshot);
      return snapshot.commandResult == LIVE_RESULT_OK;
    }

    Sleep(kLivePollIntervalMs);
  }

  message = "Timed out waiting for the live driver";
  return false;
}

HFONT CreateAppFont(int size, int weight) {
  HDC screen = GetDC(NULL);
  const int logPixels = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
  if (screen)
    ReleaseDC(NULL, screen);
  return CreateFontA(-MulDiv(size, logPixels, 72), 0, 0, 0, weight, FALSE,
                     FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

HWND CreateLabel(HWND parent, const char *text, int x, int y, int w, int h,
                 DWORD style = 0) {
  return CreateWindowExA(0, "STATIC", text,
                         WS_CHILD | WS_VISIBLE | SS_NOPREFIX | style, x, y, w, h,
                         parent, NULL, GetModuleHandle(NULL), NULL);
}

HWND CreateEdit(HWND parent, int id, int x, int y, int w, int h,
                DWORD style) {
  return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, w, h,
                         parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandle(NULL), NULL);
}

HWND CreateButton(HWND parent, const char *text, int id, int x, int y, int w,
                  int h) {
  return CreateWindowExA(0, "BUTTON", text,
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x,
                         y, w, h, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandle(NULL), NULL);
}

HWND CreateCombo(HWND parent, int id, int x, int y, int w, int h) {
  return CreateWindowExA(0, "COMBOBOX", "",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                         x, y, w, h, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandle(NULL), NULL);
}

void BuildUi(HWND hwnd) {
  g_ui.hwnd = hwnd;
  g_ui.titleFont = CreateAppFont(18, FW_BOLD);
  g_ui.bodyFont = CreateAppFont(9, FW_NORMAL);
  SetDisconnectedState("Waiting for a live SuperVirtualMIDISynth instance...",
                       "Open a MIDI host that uses SuperVirtualMIDISynth, then "
                       "Configurator V2 will attach automatically.");

  HWND title = CreateLabel(hwnd, "SuperVirtualMIDISynth Configurator V2", 18, 16,
                           460, 28);
  SendMessage(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.titleFont), TRUE);
  g_ui.statusText = CreateEdit(hwnd, IDC_STATUS, 18, 48, 740, 24,
                               ES_AUTOHSCROLL | ES_READONLY);

  g_ui.tab = CreateWindowExA(0, WC_TABCONTROLA, "",
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                                 WS_TABSTOP,
                             18, 82, 740, 440, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)),
                             GetModuleHandle(NULL), NULL);

  TCITEMA item;
  memset(&item, 0, sizeof(item));
  item.mask = TCIF_TEXT;
  item.pszText = const_cast<char *>("Home");
  TabCtrl_InsertItem(g_ui.tab, 0, &item);
  item.pszText = const_cast<char *>("Diagnostics");
  TabCtrl_InsertItem(g_ui.tab, 1, &item);

  g_ui.homePanel = CreateWindowExA(0, "STATIC", "",
                                   WS_CHILD | WS_VISIBLE, 30, 112, 716, 360,
                                   hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HOME_PANEL)),
                                   GetModuleHandle(NULL), NULL);
  g_ui.diagPanel = CreateWindowExA(0, "STATIC", "",
                                   WS_CHILD, 30, 112, 716, 360, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DIAG_PANEL)),
                                   GetModuleHandle(NULL), NULL);

  HWND engineLabel = CreateLabel(g_ui.homePanel, "Sampler Engine", 10, 12, 120, 18);
  HWND timingLabel = CreateLabel(g_ui.homePanel, "Timing Mode", 210, 12, 120, 18);
  HWND sourceLabel = CreateLabel(g_ui.homePanel, "Resolved Source", 410, 12, 120, 18);
  SendMessage(engineLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.bodyFont), TRUE);
  SendMessage(timingLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.bodyFont), TRUE);
  SendMessage(sourceLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.bodyFont), TRUE);

  g_ui.engineCombo = CreateCombo(g_ui.homePanel, IDC_ENGINE_COMBO, 10, 34, 180, 240);
  PopulateSamplerEngineCombo(g_ui.engineCombo);
  g_ui.timingCombo = CreateCombo(g_ui.homePanel, IDC_TIMING_COMBO, 210, 34, 180, 240);
  PopulateTimingModeCombo(g_ui.timingCombo);
  g_ui.sourceEdit = CreateEdit(g_ui.homePanel, IDC_SOURCE_EDIT, 410, 34, 280, 24,
                               ES_AUTOHSCROLL | ES_READONLY);
  g_ui.homeSummary = CreateEdit(g_ui.homePanel, IDC_HOME_SUMMARY, 10, 72, 680,
                                240, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
                                         WS_VSCROLL);

  g_ui.diagSummary = CreateEdit(g_ui.diagPanel, IDC_DIAG_SUMMARY, 10, 12, 430, 300,
                                ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
                                    WS_VSCROLL);
  g_ui.perChannel = CreateEdit(g_ui.diagPanel, IDC_PER_CHANNEL, 460, 12, 230, 300,
                               ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
                                   WS_VSCROLL);

  g_ui.applyButton = CreateButton(hwnd, "Apply", IDC_APPLY, 322, 534, 92, 28);
  g_ui.reloadButton = CreateButton(hwnd, "Reload", IDC_RELOAD, 426, 534, 92, 28);
  g_ui.resetButton = CreateButton(hwnd, "Hard Reset", IDC_RESET, 530, 534, 104, 28);
  g_ui.killButton = CreateButton(hwnd, "Kill", IDC_KILL, 646, 534, 92, 28);

  const HWND bodyControls[] = {g_ui.statusText, g_ui.engineCombo, g_ui.timingCombo,
                               g_ui.sourceEdit,
                               g_ui.homeSummary, g_ui.diagSummary, g_ui.perChannel,
                               g_ui.applyButton, g_ui.reloadButton,
                               g_ui.resetButton, g_ui.killButton};
  for (size_t i = 0; i < sizeof(bodyControls) / sizeof(bodyControls[0]); ++i) {
    SendMessage(bodyControls[i], WM_SETFONT,
                reinterpret_cast<WPARAM>(g_ui.bodyFont), TRUE);
  }

  SelectTab(0);
  UpdateDisconnectedUi();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE: {
    BuildUi(hwnd);
    SetTimer(hwnd, kLivePollTimerId, kLivePollIntervalMs, NULL);
    return 0;
  }

  case WM_TIMER:
    if (wParam == kLivePollTimerId)
      PollLiveState();
    return 0;

  case WM_NOTIFY: {
    const NMHDR *hdr = reinterpret_cast<const NMHDR *>(lParam);
    if (hdr && hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
      SelectTab(TabCtrl_GetCurSel(g_ui.tab));
      return 0;
    }
    break;
  }

  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDC_APPLY: {
      LiveBridgeSettings settings;
      memset(&settings, 0, sizeof(settings));
      BuildSettingsFromUi(settings);
      std::string message;
      bool ok = SendCommandAndWait(
          DetermineApplyCommand(settings, g_ui.lastSnapshot), settings, message);
      SetEditText(g_ui.statusText, message.c_str());
      if (ok)
        g_ui.seededEditors = false;
      return 0;
    }
    case IDC_RELOAD: {
      LiveBridgeSettings settings;
      memset(&settings, 0, sizeof(settings));
      std::string message;
      bool ok = SendCommandAndWait(LIVE_CMD_RELOAD_CONFIG, settings, message);
      SetEditText(g_ui.statusText, message.c_str());
      if (ok)
        g_ui.seededEditors = false;
      return 0;
    }
    case IDC_RESET: {
      LiveBridgeSettings settings;
      memset(&settings, 0, sizeof(settings));
      std::string message;
      bool ok = SendCommandAndWait(LIVE_CMD_RESET_ENGINE, settings, message);
      SetEditText(g_ui.statusText, message.c_str());
      if (ok)
        g_ui.seededEditors = false;
      return 0;
    }
    case IDC_KILL: {
      LiveBridgeSettings settings;
      memset(&settings, 0, sizeof(settings));
      std::string message;
      bool ok = SendCommandAndWait(LIVE_CMD_KILL_ENGINE, settings, message);
      SetEditText(g_ui.statusText, message.c_str());
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
    if (g_ui.bodyFont)
      DeleteObject(g_ui.bodyFont);
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
  INITCOMMONCONTROLSEX icc;
  memset(&icc, 0, sizeof(icc));
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
  InitCommonControlsEx(&icc);

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
                              CW_USEDEFAULT, CW_USEDEFAULT, 790, 610, NULL,
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
