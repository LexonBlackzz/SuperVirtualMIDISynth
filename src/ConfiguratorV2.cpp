#include "LiveConfigProtocol.h"

#include <commdlg.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <d3d9.h>
#include <string>
#include <vector>
#include <windows.h>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace {

static const UINT_PTR kLivePollTimerId = 1;
static const UINT kLivePollIntervalMs = 50;
static const int kHistorySamples = 180;

enum ViewMode {
  VIEW_BASIC = 0,
  VIEW_ADVANCED = 1,
  VIEW_POWERUSER = 2,
  VIEW_DEVELOPER = 3
};

struct HistorySeries {
  float values[kHistorySamples];
  int count;
  int writeIndex;

  HistorySeries() : count(0), writeIndex(0) { memset(values, 0, sizeof(values)); }
};

struct UiState {
  HWND hwnd;
  HANDLE mappingHandle;
  HANDLE mutexHandle;
  LiveBridgeSharedState *sharedState;
  LiveBridgeSharedState lastSnapshot;
  LiveBridgeSettings pendingSettings;
  bool connected;
  bool needsReseed;
  bool versionMismatch;
  bool developerControlsEnabled;
  int currentMode;
  char disconnectedStatus[SVMS_MAX_STATUS_TEXT];
  char disconnectedSummary[SVMS_MAX_STATUS_TEXT];
  char actionMessage[SVMS_MAX_STATUS_TEXT];

  HistorySeries historyRenderMs;
  HistorySeries historyBlockMs;
  HistorySeries historyBudgetMs;
  HistorySeries historyQueueDepth;
  HistorySeries historyDeferredDepth;
  HistorySeries historyReleaseLaneDepth;
  HistorySeries historyVsExact;
  HistorySeries historyVsGrouped;
  HistorySeries historyVsDensity;
  HistorySeries historyVsVoiceEq;
  HistorySeries historyOverload;
  HistorySeries historyNoteOnStarted;
  HistorySeries historyNoteOnDropped;
  HistorySeries historyNoteOffApplied;
  HistorySeries historyNoteOffCanceled;
  HistorySeries historyNoteOffCoalesced;
};

UiState g_ui = {};
LPDIRECT3D9 g_pD3D = NULL;
LPDIRECT3DDEVICE9 g_pd3dDevice = NULL;
D3DPRESENT_PARAMETERS g_d3dpp = {};

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
  const int start =
      (series.writeIndex - series.count + kHistorySamples) % kHistorySamples;
  const int actual = (start + oldestIndex) % kHistorySamples;
  return series.values[actual];
}

void CopyHistory(const HistorySeries &series, std::vector<float> &dst) {
  dst.clear();
  for (int i = 0; i < series.count; ++i)
    dst.push_back(GetHistoryValue(series, i));
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

const char *GetReleaseHealth(const LiveBridgeSharedState &snapshot) {
  if (snapshot.currentStats.noteOffLateThisBlock > 0 ||
      snapshot.currentStats.releaseLaneDepth >= 64)
    return "Release path at risk";
  if (snapshot.currentStats.noteOffDeferredThisBlock >
          snapshot.currentStats.noteOffReleaseLaneAppliedThisBlock &&
      snapshot.currentStats.noteOffDeferredThisBlock > 0)
    return "Release path backlogged";
  if (snapshot.currentStats.schedulerNoteOffsCanceledThisBlock > 0 &&
      snapshot.currentStats.schedulerNoteOffsCanceledThisBlock >=
          snapshot.currentStats.schedulerNoteOffsAppliedThisBlock)
    return "Same-key cancellation active";
  return "Release path healthy";
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
  g_ui.needsReseed = true;
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

bool EnsureBridgeConnected();
bool ReadSnapshot(LiveBridgeSharedState &snapshot);
void SyncPendingSettingsFromSnapshot(const LiveBridgeSharedState &snapshot);
void AppendSnapshotHistory(const LiveBridgeSharedState &snapshot);
LONG DetermineApplyCommand(const LiveBridgeSettings &settings,
                           const LiveBridgeSharedState &snapshot);
bool SendCommandAndWait(LONG commandCode, const LiveBridgeSettings &settings,
                        std::string &message);
void BrowseForSoundfont();
void PollLiveState();
bool CreateDeviceD3D(HWND hwnd);
void CleanupDeviceD3D();
void ResetDevice();
void ApplyImGuiStyle();
void LoadUiFont();
void RenderUI();
void RenderHeaderBar();
void RenderHomeTab();
void RenderSoundFontsTab();
void RenderEngineTab();
void RenderTimingTab();
void RenderPerformanceTab();
void RenderFxTab();
void RenderDiagnosticsTab();
void RenderProfilesTab();
void RenderAdvancedTab();
void RenderDeveloperTab();
void BeginCard(const char *id, const char *title, float width, float height);
void EndCard();
void PlotHistory(const char *label, const HistorySeries &series, float maxValue,
                 ImVec2 size);
void HelpText(const char *text);
void DrawPlaceholderField(const char *label, const char *value);
void DrawModeSelector();
const char *ModeName(int mode);
void RenderStatusPill(const char *label, const ImVec4 &color);
bool DrawActionButtonWrapped(const char *label, const ImVec2 &size);
void RenderSparklineCard(const char *id, const char *title,
                         const HistorySeries &series, float maxValue,
                         const char *primary, const char *secondary,
                         const char *footer);
bool ComboFromValue(const char *label, char *buffer, size_t capacity,
                    const char *const *items, int itemCount);
void InputIntClamped(const char *label, LONG *value, LONG minValue,
                     LONG maxValue, LONG step);
void InputFloatClamped(const char *label, FLOAT *value, FLOAT minValue,
                       FLOAT maxValue, float step);
bool ToggleButton(const char *label, LONG *value);
void BuildDiagnosticsText(char *buffer, size_t capacity);
void BuildDeveloperText(char *buffer, size_t capacity);
void BuildAdvancedText(char *buffer, size_t capacity);

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
          "and restart both the DLL and Configurator together.",
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
        "Live bridge layout mismatch",
        "This Configurator V2 does not match the running synth bridge layout.",
        true);
    return false;
  }
  return snapshot.runtimeLoaded != 0 && snapshot.publisherPid != 0;
}

void SyncPendingSettingsFromSnapshot(const LiveBridgeSharedState &snapshot) {
  g_ui.pendingSettings = snapshot.currentSettings;
  g_ui.needsReseed = false;
}

void AppendSnapshotHistory(const LiveBridgeSharedState &snapshot) {
  AppendHistory(g_ui.historyRenderMs, snapshot.currentStats.synthRenderMs);
  AppendHistory(g_ui.historyBlockMs, snapshot.currentStats.audioBlockMs);
  AppendHistory(g_ui.historyBudgetMs, snapshot.currentStats.audioBudgetMs);
  AppendHistory(g_ui.historyQueueDepth, (float)snapshot.currentStats.queuedMidiEvents);
  AppendHistory(g_ui.historyDeferredDepth, (float)snapshot.currentStats.deferredMidiEvents);
  AppendHistory(g_ui.historyReleaseLaneDepth, (float)snapshot.currentStats.releaseLaneDepth);
  AppendHistory(g_ui.historyVsExact, (float)snapshot.currentStats.virtuallySuperExactVoices);
  AppendHistory(g_ui.historyVsGrouped, (float)snapshot.currentStats.virtuallySuperGroupedObjects);
  AppendHistory(g_ui.historyVsDensity, (float)snapshot.currentStats.virtuallySuperDensityObjects);
  AppendHistory(g_ui.historyVsVoiceEq, (float)snapshot.currentStats.virtuallySuperVoiceEquivalent);
  AppendHistory(g_ui.historyOverload, (float)snapshot.currentStats.overloadState);
  AppendHistory(g_ui.historyNoteOnStarted, (float)snapshot.currentStats.noteOnStartedThisBlock);
  AppendHistory(g_ui.historyNoteOnDropped, (float)snapshot.currentStats.noteOnDroppedThisBlock);
  AppendHistory(g_ui.historyNoteOffApplied, (float)snapshot.currentStats.schedulerNoteOffsAppliedThisBlock);
  AppendHistory(g_ui.historyNoteOffCanceled, (float)snapshot.currentStats.schedulerNoteOffsCanceledThisBlock);
  AppendHistory(g_ui.historyNoteOffCoalesced, (float)snapshot.currentStats.schedulerNoteOffsCoalescedThisBlock);
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
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      if (msg.message == WM_QUIT) {
        message = "Command canceled";
        return false;
      }
    }

    LiveBridgeSharedState snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (ReadSnapshot(snapshot) && snapshot.commandProcessedId == requestId) {
      message = snapshot.commandMessage;
      g_ui.lastSnapshot = snapshot;
      g_ui.needsReseed = true;
      return snapshot.commandResult == LIVE_RESULT_OK;
    }
    Sleep(kLivePollIntervalMs);
  }

  message = "Timed out waiting for the live driver";
  return false;
}

void BrowseForSoundfont() {
  char pathBuffer[SVMS_MAX_PATH_TEXT];
  CopyCString(pathBuffer, sizeof(pathBuffer), g_ui.pendingSettings.soundfontPath);

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
    CopyCString(g_ui.pendingSettings.soundfontPath,
                sizeof(g_ui.pendingSettings.soundfontPath), pathBuffer);
}

void PollLiveState() {
  LiveBridgeSharedState snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  if (ReadSnapshot(snapshot)) {
    g_ui.connected = true;
    g_ui.lastSnapshot = snapshot;
    if (g_ui.needsReseed)
      SyncPendingSettingsFromSnapshot(snapshot);
    AppendSnapshotHistory(snapshot);
  } else {
    DisconnectBridge();
  }
}

bool CreateDeviceD3D(HWND hwnd) {
  if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == NULL)
    return false;

  ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
  g_d3dpp.Windowed = TRUE;
  g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
  g_d3dpp.EnableAutoDepthStencil = TRUE;
  g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
  g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                           D3DCREATE_HARDWARE_VERTEXPROCESSING,
                           &g_d3dpp, &g_pd3dDevice) < 0) {
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                             D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                             &g_d3dpp, &g_pd3dDevice) < 0) {
      return false;
    }
  }
  return true;
}

void CleanupDeviceD3D() {
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = NULL;
  }
  if (g_pD3D) {
    g_pD3D->Release();
    g_pD3D = NULL;
  }
}

void ResetDevice() {
  ImGui_ImplDX9_InvalidateDeviceObjects();
  HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
  if (hr == D3DERR_INVALIDCALL)
    IM_ASSERT(0);
  ImGui_ImplDX9_CreateDeviceObjects();
}

void ApplyImGuiStyle() {
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 8.0f;
  style.ChildRounding = 8.0f;
  style.FrameRounding = 6.0f;
  style.GrabRounding = 6.0f;
  style.TabRounding = 6.0f;
  style.PopupRounding = 6.0f;
  style.ScrollbarRounding = 6.0f;
  style.FramePadding = ImVec2(6.0f, 6.0f);
  style.ItemSpacing = ImVec2(8.0f, 8.0f);
  style.WindowPadding = ImVec2(12.0f, 12.0f);
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;

  ImVec4 *colors = style.Colors;
  colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.16f, 1.00f);
  colors[ImGuiCol_Border] = ImVec4(0.23f, 0.25f, 0.31f, 0.85f);
  colors[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.96f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.53f, 0.58f, 0.66f, 1.00f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.26f, 1.00f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.29f, 1.00f);
  colors[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.28f, 0.33f, 1.00f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.29f, 0.32f, 0.39f, 1.00f);
  colors[ImGuiCol_Header] = ImVec4(0.24f, 0.33f, 0.62f, 0.80f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.40f, 0.75f, 0.85f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.34f, 0.44f, 0.82f, 1.00f);
  colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.17f, 0.22f, 1.00f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.25f, 0.34f, 0.67f, 1.00f);
  colors[ImGuiCol_TabActive] = ImVec4(0.27f, 0.36f, 0.72f, 1.00f);
  colors[ImGuiCol_TabUnfocused] = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
  colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.23f, 0.40f, 1.00f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.13f, 0.17f, 1.00f);
  colors[ImGuiCol_PlotLines] = ImVec4(0.36f, 0.72f, 0.96f, 1.00f);
  colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.86f, 0.64f, 0.25f, 1.00f);
  colors[ImGuiCol_Separator] = ImVec4(0.26f, 0.30f, 0.38f, 1.00f);
}

void LoadUiFont() {
  ImGuiIO &io = ImGui::GetIO();
  char windowsDir[MAX_PATH];
  char path[MAX_PATH];
  if (GetWindowsDirectoryA(windowsDir, sizeof(windowsDir)) > 0) {
    sprintf(path, "%s\\Fonts\\segoeui.ttf", windowsDir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES &&
        io.Fonts->AddFontFromFileTTF(path, 17.0f))
      return;
    sprintf(path, "%s\\Fonts\\tahoma.ttf", windowsDir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES &&
        io.Fonts->AddFontFromFileTTF(path, 17.0f))
      return;
  }
  io.Fonts->AddFontDefault();
}

const char *ModeName(int mode) {
  switch (mode) {
  case VIEW_ADVANCED:
    return "Advanced";
  case VIEW_POWERUSER:
    return "PowerUser";
  case VIEW_DEVELOPER:
    return "Developer";
  default:
    return "Basic";
  }
}

void HelpText(const char *text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.70f, 0.78f, 1.00f));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void BeginCard(const char *id, const char *title, float width, float height) {
  ImGuiChildFlags childFlags =
      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
  ImGuiWindowFlags windowFlags =
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  if (height <= 0.0f)
    childFlags |= ImGuiChildFlags_AutoResizeY;
  ImGui::BeginChild(id, ImVec2(width, height), childFlags, windowFlags);
  ImGui::TextUnformatted(title);
  ImGui::Separator();
}

void EndCard() { ImGui::EndChild(); }

void DrawPlaceholderField(const char *label, const char *value) {
  char buffer[128];
  CopyCString(buffer, sizeof(buffer), value ? value : "Not wired yet");
  ImGui::BeginDisabled();
  ImGui::InputText(label, buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
  ImGui::EndDisabled();
}

bool ComboFromValue(const char *label, char *buffer, size_t capacity,
                    const char *const *items, int itemCount) {
  int current = 0;
  for (int i = 0; i < itemCount; ++i) {
    if (strcmp(buffer, items[i]) == 0) {
      current = i;
      break;
    }
  }
  if (ImGui::BeginCombo(label, items[current])) {
    for (int i = 0; i < itemCount; ++i) {
      const bool selected = (current == i);
      if (ImGui::Selectable(items[i], selected)) {
        CopyCString(buffer, capacity, items[i]);
        current = i;
      }
      if (selected)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return true;
}

void InputIntClamped(const char *label, LONG *value, LONG minValue,
                     LONG maxValue, LONG step) {
  int v = (int)(*value);
  if (ImGui::InputInt(label, &v, (int)step, (int)step * 4)) {
    if (v < minValue)
      v = minValue;
    if (v > maxValue)
      v = maxValue;
    *value = (LONG)v;
  }
}

void InputFloatClamped(const char *label, FLOAT *value, FLOAT minValue,
                       FLOAT maxValue, float step) {
  float v = *value;
  if (ImGui::InputFloat(label, &v, step, step * 4.0f, "%.3f")) {
    if (v < minValue)
      v = minValue;
    if (v > maxValue)
      v = maxValue;
    *value = v;
  }
}

bool ToggleButton(const char *label, LONG *value) {
  bool checked = (*value != 0);
  if (ImGui::Checkbox(label, &checked)) {
    *value = checked ? 1 : 0;
    return true;
  }
  return false;
}

void RenderSparklineCard(const char *id, const char *title,
                         const HistorySeries &series, float maxValue,
                         const char *primary, const char *secondary,
                         const char *footer) {
  ImGui::BeginChild(
      id, ImVec2(0, 156),
      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::TextUnformatted(title);
  ImGui::Separator();
  ImGui::TextWrapped("%s", primary ? primary : "");
  if (secondary && secondary[0])
    HelpText(secondary);

  std::vector<float> values;
  CopyHistory(series, values);
  if (!values.empty()) {
    char plotId[64];
    sprintf(plotId, "##%s_plot", id);
    ImGui::PlotLines(plotId, &values[0], (int)values.size(), 0, NULL, 0.0f,
                     maxValue, ImVec2(-1, 56));
  } else {
    ImGui::Dummy(ImVec2(0.0f, 56.0f));
    ImGui::TextDisabled("No history yet");
  }

  if (footer && footer[0]) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", footer);
  }
  ImGui::EndChild();
}

void PlotHistory(const char *label, const HistorySeries &series, float maxValue,
                 ImVec2 size) {
  std::vector<float> values;
  CopyHistory(series, values);
  if (values.empty()) {
    ImGui::TextDisabled("%s: no data", label);
    return;
  }
  ImGui::PlotLines(label, &values[0], (int)values.size(), 0, NULL, 0.0f,
                   maxValue, size);
}

void BuildDiagnosticsText(char *buffer, size_t capacity) {
  const LiveBridgeSharedState &s = g_ui.lastSnapshot;
  sprintf(
      buffer,
      "render=%.3f avg=%.3f peak=%.3f   block=%.3f avg=%.3f peak=%.3f   budget=%.3f\r\n"
      "queue=%lu deferred=%lu critical=%lu realtime=%lu noteon=%lu release_lane=%lu max=%lu\r\n"
      "events=%lu noteon_attempted=%lu started=%lu dropped=%lu noteoff=%lu\r\n"
      "noteoff_ingress=%lu deferred=%lu lane_queued=%lu lane_applied=%lu late=%lu\r\n"
      "scheduler due=%lu late=%lu lag=%lu pending_samekey=%lu max_samekey=%lu\r\n"
      "scheduler noteoff applied=%lu coalesced=%lu canceled=%lu release_controls=%lu\r\n"
      "overload=%lu blocks=%lu hard_entries=%lu hard_recoveries=%lu worker_blocked=%lu\r\n"
      "vs exact=%lu grouped=%lu density=%lu voice_eq=%lu released_exact=%lu pressure=%lu\r\n"
      "sampler state=%lu error=%lu warnings=%lu loaded=%lu failed=%lu",
      s.currentStats.synthRenderMs, s.currentStats.synthRenderAvgMs,
      s.currentStats.synthRenderPeakMs, s.currentStats.audioBlockMs,
      s.currentStats.audioBlockAvgMs, s.currentStats.audioBlockPeakMs,
      s.currentStats.audioBudgetMs, (unsigned long)s.currentStats.queuedMidiEvents,
      (unsigned long)s.currentStats.deferredMidiEvents,
      (unsigned long)s.currentStats.criticalQueueDepth,
      (unsigned long)s.currentStats.realtimeQueueDepth,
      (unsigned long)s.currentStats.noteOnQueueDepth,
      (unsigned long)s.currentStats.releaseLaneDepth,
      (unsigned long)s.currentStats.maxQueuedMidiEvents,
      (unsigned long)s.currentStats.eventsProcessedThisBlock,
      (unsigned long)s.currentStats.noteOnEventsThisBlock,
      (unsigned long)s.currentStats.noteOnStartedThisBlock,
      (unsigned long)s.currentStats.noteOnDroppedThisBlock,
      (unsigned long)s.currentStats.noteOffEventsThisBlock,
      (unsigned long)s.currentStats.noteOffIngressThisBlock,
      (unsigned long)s.currentStats.noteOffDeferredThisBlock,
      (unsigned long)s.currentStats.noteOffReleaseLaneQueuedThisBlock,
      (unsigned long)s.currentStats.noteOffReleaseLaneAppliedThisBlock,
      (unsigned long)s.currentStats.noteOffLateThisBlock,
      (unsigned long)s.currentStats.schedulerDueEventsThisBlock,
      (unsigned long)s.currentStats.schedulerLateEventsThisBlock,
      (unsigned long)s.currentStats.schedulerLagSamples,
      (unsigned long)s.currentStats.schedulerPendingSameKeyTransitions,
      (unsigned long)s.currentStats.schedulerMaxSameKeyQueueDepth,
      (unsigned long)s.currentStats.schedulerNoteOffsAppliedThisBlock,
      (unsigned long)s.currentStats.schedulerNoteOffsCoalescedThisBlock,
      (unsigned long)s.currentStats.schedulerNoteOffsCanceledThisBlock,
      (unsigned long)s.currentStats.schedulerReleaseControlsAppliedThisBlock,
      (unsigned long)s.currentStats.overloadState,
      (unsigned long)s.currentStats.consecutiveOverloadBlocks,
      (unsigned long)s.currentStats.accurateHardOverloadEntries,
      (unsigned long)s.currentStats.accurateHardOverloadRecoveries,
      (unsigned long)s.currentStats.accurateWorkerBlockedCount,
      (unsigned long)s.currentStats.virtuallySuperExactVoices,
      (unsigned long)s.currentStats.virtuallySuperGroupedObjects,
      (unsigned long)s.currentStats.virtuallySuperDensityObjects,
      (unsigned long)s.currentStats.virtuallySuperVoiceEquivalent,
      (unsigned long)s.currentStats.virtuallySuperReleasedExactVoices,
      (unsigned long)s.currentStats.virtuallySuperPressureLevel,
      (unsigned long)s.currentStats.samplerStateCode,
      (unsigned long)s.currentStats.samplerErrorCode,
      (unsigned long)s.currentStats.samplerWarningCount,
      (unsigned long)s.currentStats.samplerLoadedSamples,
      (unsigned long)s.currentStats.samplerFailedSamples);
}

void BuildDeveloperText(char *buffer, size_t capacity) {
  const LiveBridgeSharedState &s = g_ui.lastSnapshot;
  sprintf(
      buffer,
      "protocol=%lu struct=%lu heartbeat=%lu\r\n"
      "command request=%ld processed=%ld code=%ld in_progress=%ld result=%ld\r\n"
      "publisher_pid=%lu source_pid=%lu\r\n"
      "reload_count=%lu clock_resets=%lu state_preserved=%lu last_restart=%lu\r\n"
      "tsf helpers=%lu/%lu/%lu clustered=%lu/%lu/%lu fragments=%lu/%lu\r\n"
      "virtuallysuper fast=%lu exact_visited=%lu grouped_visited=%lu density_visited=%lu",
      (unsigned long)s.version, (unsigned long)s.structSize,
      (unsigned long)s.publisherHeartbeatTick, (long)s.commandRequestId,
      (long)s.commandProcessedId, (long)s.commandCode, (long)s.commandInProgress,
      (long)s.commandResult, (unsigned long)s.publisherPid,
      (unsigned long)s.commandSourcePid, (unsigned long)s.currentStats.runtimeReloadCount,
      (unsigned long)s.currentStats.accurateClockResetCount,
      (unsigned long)s.currentStats.schedulerStatePreservedCount,
      (unsigned long)s.currentStats.lastRestartReason,
      (unsigned long)s.currentStats.tsfHelperContiguousBlocks,
      (unsigned long)s.currentStats.tsfHelperGatherBlocks,
      (unsigned long)s.currentStats.tsfHelperComplexBlocks,
      (unsigned long)s.currentStats.tsfClusteredVoicesContiguous,
      (unsigned long)s.currentStats.tsfClusteredVoicesGather,
      (unsigned long)s.currentStats.tsfClusteredVoicesComplex,
      (unsigned long)s.currentStats.tsfSingleThreadFragments,
      (unsigned long)s.currentStats.tsfThreadedFragments,
      (unsigned long)s.currentStats.virtuallySuperIdleFastPathHits,
      (unsigned long)s.currentStats.virtuallySuperExactVisitedThisBlock,
      (unsigned long)s.currentStats.virtuallySuperGroupedVisitedThisBlock,
      (unsigned long)s.currentStats.virtuallySuperDensityVisitedThisBlock);
}

void BuildAdvancedText(char *buffer, size_t capacity) {
  const LiveBridgeSettings &s = g_ui.pendingSettings;
  sprintf(
      buffer,
      "Engine=%s   Backend=%s   Source=%s\r\n"
      "SampleRate=%ld   MaxVoices=%ld   Polling=%ld   Timing=%s\r\n"
      "AsyncStarts=%ld   WasapiAsync=%ld   Master=%.3f\r\n"
      "Velocity curve=%.3f floor=%.3f ignore_below=%ld\r\n"
      "Reverb enabled=%ld mix=%.3f feedback=%.3f tone=%.3f width=%.3f blur=%.3f\r\n"
      "Limiter enabled=%ld threshold=%.3f release=%.3f",
      s.samplerEngine, s.audioBackend, s.soundfontPath, (long)s.sampleRate,
      (long)s.maxVoices, (long)s.pollingRate, s.eventTimingMode,
      (long)s.asyncNoteStarts, (long)s.wasapiAsyncFeed, s.masterVolume,
      s.velocityCurve, s.velocityFloor, (long)s.velocityIgnoreBelow,
      (long)s.reverbEnabled, s.reverbMix, s.reverbFeedback, s.reverbTone,
      s.reverbWidth, s.reverbBlur, (long)s.limiterEnabled,
      s.limiterThreshold, s.limiterReleaseMs);
}

void DrawModeSelector() {
  static const int modes[] = {VIEW_BASIC, VIEW_ADVANCED, VIEW_POWERUSER, VIEW_DEVELOPER};
  const float availableWidth = ImGui::GetContentRegionAvail().x;
  float buttonWidth = (availableWidth - 24.0f) / 4.0f;
  if (buttonWidth < 88.0f)
    buttonWidth = 88.0f;
  if (buttonWidth > 132.0f)
    buttonWidth = 132.0f;
  for (int i = 0; i < 4; ++i) {
    if (i > 0)
      ImGui::SameLine();
    const bool selected = g_ui.currentMode == modes[i];
    if (selected)
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.27f, 0.36f, 0.72f, 1.0f));
    if (ImGui::Button(ModeName(modes[i]), ImVec2(buttonWidth, 0.0f)))
      g_ui.currentMode = modes[i];
    if (selected)
      ImGui::PopStyleColor();
  }
}

void RenderStatusPill(const char *label, const ImVec4 &color) {
  ImGui::PushStyleColor(ImGuiCol_Button, color);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 4.0f));
  ImGui::Button(label, ImVec2(0.0f, 0.0f));
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(3);
}

bool DrawActionButtonWrapped(const char *label, const ImVec2 &size) {
  const float neededWidth =
      size.x + ImGui::GetStyle().ItemSpacing.x;
  if (ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x &&
      neededWidth > ImGui::GetContentRegionAvail().x) {
    ImGui::NewLine();
  }
  const bool pressed = ImGui::Button(label, size);
  if (ImGui::GetContentRegionAvail().x > neededWidth)
    ImGui::SameLine();
  return pressed;
}

void RenderHeaderBar() {
  ImGui::BeginChild("HeaderBar", ImVec2(0, 128), true, ImGuiWindowFlags_NoScrollbar);
  const bool online = g_ui.connected;
  const ImVec4 onlineColor =
      online ? ImVec4(0.18f, 0.65f, 0.38f, 1.0f)
             : ImVec4(0.78f, 0.24f, 0.22f, 1.0f);
  const ImVec4 protocolColor =
      g_ui.versionMismatch ? ImVec4(0.75f, 0.24f, 0.22f, 1.0f)
                           : ImVec4(0.24f, 0.42f, 0.76f, 1.0f);
  const char *engineName =
      online ? (g_ui.lastSnapshot.resolvedSamplerEngine[0]
                    ? g_ui.lastSnapshot.resolvedSamplerEngine
                    : g_ui.lastSnapshot.currentSettings.samplerEngine)
             : "Unavailable";
  const char *backendName =
      online ? (g_ui.lastSnapshot.resolvedAudioBackend[0]
                    ? g_ui.lastSnapshot.resolvedAudioBackend
                    : g_ui.lastSnapshot.currentSettings.audioBackend)
             : "Unavailable";
  const char *sourcePath =
      online ? (g_ui.lastSnapshot.resolvedSoundfontPath[0]
                    ? g_ui.lastSnapshot.resolvedSoundfontPath
                    : g_ui.lastSnapshot.currentSettings.soundfontPath)
             : "Unavailable";
  float loadPercent = 0.0f;
  if (online && g_ui.lastSnapshot.currentStats.audioBudgetMs > 0.0f) {
    loadPercent = g_ui.lastSnapshot.currentStats.audioBlockMs * 100.0f /
                  g_ui.lastSnapshot.currentStats.audioBudgetMs;
  }

  if (ImGui::BeginTable("HeaderLayout", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch, 1.0f);

    ImGui::TableNextColumn();
    ImGui::TextUnformatted("VirtuallySuper Control Center");
    ImGui::SameLine();
    RenderStatusPill(online ? "ONLINE" : "OFFLINE", onlineColor);
    ImGui::SameLine();
    RenderStatusPill(g_ui.versionMismatch ? "PROTOCOL MISMATCH" : "PROTOCOL OK",
                     protocolColor);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.67f, 0.71f, 0.79f, 1.0f));
    ImGui::TextWrapped("Engine %s | Profile Not wired yet | Output %s @ %ld Hz | %s",
                       engineName, backendName, (long)g_ui.pendingSettings.sampleRate,
                       online ? "Live bridge attached" : "Waiting for bridge");
    if (online) {
      ImGui::TextWrapped("Voices %lu | VoiceEq %lu | Load %s | Overload %s",
                         (unsigned long)g_ui.lastSnapshot.currentStats.totalActiveVoices,
                         (unsigned long)g_ui.lastSnapshot.currentStats.virtuallySuperVoiceEquivalent,
                         GetLoadStatus(loadPercent),
                         GetOverloadStatus(g_ui.lastSnapshot.currentStats.overloadState));
    } else {
      ImGui::TextWrapped("Voices unavailable");
    }
    ImGui::PopStyleColor();

    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Mode");
    DrawModeSelector();
    ImGui::TextDisabled("%s surfaces are visible.", ModeName(g_ui.currentMode));

    ImGui::EndTable();
  }
  ImGui::Separator();
  ImGui::TextWrapped(
      "%s",
      online ? g_ui.lastSnapshot.statusText
             : (g_ui.disconnectedStatus[0] ? g_ui.disconnectedStatus
                                           : "Waiting for synth"));
  ImGui::TextWrapped("Source: %s", sourcePath);
  ImGui::EndChild();
}

void RenderHomeTab() {
  BeginCard("HomeOverview", "Overview", 0, 150);
  if (g_ui.connected) {
    const float loadPercent = g_ui.lastSnapshot.currentStats.audioBudgetMs > 0.0f
                                  ? g_ui.lastSnapshot.currentStats.audioBlockMs * 100.0f /
                                        g_ui.lastSnapshot.currentStats.audioBudgetMs
                                  : 0.0f;
    ImGui::Text("Backend: %s", g_ui.lastSnapshot.resolvedAudioBackend[0]
                                   ? g_ui.lastSnapshot.resolvedAudioBackend
                                   : g_ui.lastSnapshot.currentSettings.audioBackend);
    ImGui::Text("Source: %s", g_ui.lastSnapshot.resolvedSoundfontPath[0]
                                  ? g_ui.lastSnapshot.resolvedSoundfontPath
                                  : g_ui.lastSnapshot.currentSettings.soundfontPath);
    ImGui::Text("Voices: %lu  VoiceEq: %lu  Overload: %s",
                (unsigned long)g_ui.lastSnapshot.currentStats.totalActiveVoices,
                (unsigned long)g_ui.lastSnapshot.currentStats.virtuallySuperVoiceEquivalent,
                GetOverloadStatus(g_ui.lastSnapshot.currentStats.overloadState));
    ImGui::Text("Audio: %.3f / %.3f ms  Health: %s",
                g_ui.lastSnapshot.currentStats.audioBlockMs,
                g_ui.lastSnapshot.currentStats.audioBudgetMs,
                GetLoadStatus(loadPercent));
    ImGui::Text("Release: %s", GetReleaseHealth(g_ui.lastSnapshot));
  } else {
    HelpText(g_ui.disconnectedSummary);
  }
  EndCard();

  BeginCard("HomeActions", "Quick Actions", 0, 0);
  const LONG applyCommand =
      g_ui.connected ? DetermineApplyCommand(g_ui.pendingSettings, g_ui.lastSnapshot)
                     : LIVE_CMD_APPLY_SOFT;
  const char *applyLabel =
      applyCommand == LIVE_CMD_APPLY_HEAVY ? "Apply Heavy" : "Apply Soft";
  if (DrawActionButtonWrapped(applyLabel, ImVec2(120, 0)) && g_ui.connected) {
    std::string message;
    SendCommandAndWait(applyCommand, g_ui.pendingSettings, message);
    CopyCString(g_ui.actionMessage, sizeof(g_ui.actionMessage), message.c_str());
  }
  if (DrawActionButtonWrapped("Reload Config", ImVec2(140, 0)) && g_ui.connected) {
    LiveBridgeSettings emptySettings;
    memset(&emptySettings, 0, sizeof(emptySettings));
    std::string message;
    SendCommandAndWait(LIVE_CMD_RELOAD_CONFIG, emptySettings, message);
    CopyCString(g_ui.actionMessage, sizeof(g_ui.actionMessage), message.c_str());
  }
  if (DrawActionButtonWrapped("Hard Reset", ImVec2(120, 0)) && g_ui.connected) {
    LiveBridgeSettings emptySettings;
    memset(&emptySettings, 0, sizeof(emptySettings));
    std::string message;
    SendCommandAndWait(LIVE_CMD_RESET_ENGINE, emptySettings, message);
    CopyCString(g_ui.actionMessage, sizeof(g_ui.actionMessage), message.c_str());
  }
  ImGui::BeginDisabled();
  DrawActionButtonWrapped("Rescan SoundFonts", ImVec2(150, 0));
  DrawActionButtonWrapped("Open Debug Window", ImVec2(150, 0));
  ImGui::EndDisabled();
  if (g_ui.actionMessage[0])
    HelpText(g_ui.actionMessage);
  HelpText("This front page stays shallow. Heavy analysis and chart history are computed here in the configurator, not in the synth.");
  EndCard();
}

void RenderSoundFontsTab() {
  BeginCard("SFSource", "Active Source", 0, 0);
  ImGui::InputText("SoundFont / Source Path", g_ui.pendingSettings.soundfontPath,
                   sizeof(g_ui.pendingSettings.soundfontPath));
  ImGui::SameLine();
  if (ImGui::Button("Browse"))
    BrowseForSoundfont();
  ImGui::TextWrapped("Resolved: %s", g_ui.connected
                                       ? (g_ui.lastSnapshot.resolvedSoundfontPath[0]
                                              ? g_ui.lastSnapshot.resolvedSoundfontPath
                                              : g_ui.lastSnapshot.currentSettings.soundfontPath)
                                       : "Unavailable");
  EndCard();
  if (g_ui.currentMode >= VIEW_ADVANCED) {
    BeginCard("SFFuture", "Source Management", 0, 0);
    DrawPlaceholderField("Folder roots", "Not wired yet");
    if (g_ui.currentMode >= VIEW_POWERUSER)
      DrawPlaceholderField("Source order / fallback", "Not wired yet");
    if (g_ui.currentMode >= VIEW_POWERUSER)
      DrawPlaceholderField("Preload / metadata cache", "Not wired yet");
    EndCard();
  }
}

void RenderEngineTab() {
  static const char *const backends[] = {"auto", "wasapi-shared", "waveout", "dsound"};
  static const char *const engines[] = {"auto", "tsf", "bassmidi", "virtuallysuper", "sfz"};
  BeginCard("EngineRuntime", "Runtime Engine", 0, 0);
  ComboFromValue("Audio Backend", g_ui.pendingSettings.audioBackend,
                 sizeof(g_ui.pendingSettings.audioBackend), backends, 4);
  ComboFromValue("Sampler Engine", g_ui.pendingSettings.samplerEngine,
                 sizeof(g_ui.pendingSettings.samplerEngine), engines, 5);
  if (g_ui.currentMode >= VIEW_ADVANCED) {
    InputIntClamped("Sample Rate", &g_ui.pendingSettings.sampleRate, 8000, 192000, 1000);
    InputIntClamped("Max Voices", &g_ui.pendingSettings.maxVoices, 1, 500000, 10);
    InputIntClamped("Polling Rate", &g_ui.pendingSettings.pollingRate, 0, 1000, 5);
  }
  if (g_ui.currentMode >= VIEW_POWERUSER) {
    ToggleButton("Async note starts", &g_ui.pendingSettings.asyncNoteStarts);
    ToggleButton("WASAPI async feed", &g_ui.pendingSettings.wasapiAsyncFeed);
  }
  EndCard();
  if (g_ui.currentMode >= VIEW_ADVANCED) {
    BeginCard("EngineFuture", "Future Engine Controls", 0, 0);
    DrawPlaceholderField("Output device", "Not wired yet");
    if (g_ui.currentMode >= VIEW_POWERUSER)
      DrawPlaceholderField("Worker count", "Not wired yet");
    if (g_ui.currentMode >= VIEW_POWERUSER)
      DrawPlaceholderField("Quality profile", "Not wired yet");
    EndCard();
  }
}

void RenderTimingTab() {
  static const char *const timingModes[] = {"accurate", "quantized", "legacy-sync"};
  BeginCard("Timing", "Timing", 0, 0);
  if (g_ui.currentMode >= VIEW_ADVANCED)
    ComboFromValue("Event Timing Mode", g_ui.pendingSettings.eventTimingMode,
                   sizeof(g_ui.pendingSettings.eventTimingMode), timingModes, 3);
  if (g_ui.connected) {
    ImGui::Text("Due: %lu  Late: %lu  Lag: %lu samples",
                (unsigned long)g_ui.lastSnapshot.currentStats.schedulerDueEventsThisBlock,
                (unsigned long)g_ui.lastSnapshot.currentStats.schedulerLateEventsThisBlock,
                (unsigned long)g_ui.lastSnapshot.currentStats.schedulerLagSamples);
    ImGui::Text("Same-key pending: %lu  Max depth: %lu",
                (unsigned long)g_ui.lastSnapshot.currentStats.schedulerPendingSameKeyTransitions,
                (unsigned long)g_ui.lastSnapshot.currentStats.schedulerMaxSameKeyQueueDepth);
  }
  EndCard();
  if (g_ui.currentMode >= VIEW_ADVANCED) {
    BeginCard("TimingPolicies", "Documented Policies", 0, 0);
    DrawPlaceholderField("Same-key policy", "Not wired yet");
    if (g_ui.currentMode >= VIEW_POWERUSER)
      DrawPlaceholderField("Buzz guard / retrigger rules", "Not wired yet");
    if (g_ui.currentMode >= VIEW_POWERUSER)
      DrawPlaceholderField("Sustain / release policy", "Not wired yet");
    if (g_ui.currentMode >= VIEW_DEVELOPER)
      DrawPlaceholderField("Reset / panic ordering", "Not wired yet");
    EndCard();
  }
}

void RenderPerformanceTab() {
  BeginCard("Velocity", "Velocity", 0, 0);
  InputFloatClamped("Velocity Curve", &g_ui.pendingSettings.velocityCurve, 0.25f, 6.0f, 0.05f);
  if (g_ui.currentMode >= VIEW_ADVANCED)
    InputFloatClamped("Velocity Floor", &g_ui.pendingSettings.velocityFloor, 0.0f, 0.5f, 0.01f);
  if (g_ui.currentMode >= VIEW_ADVANCED)
    InputIntClamped("Ignore Below", &g_ui.pendingSettings.velocityIgnoreBelow, 0, 126, 1);
  EndCard();
  BeginCard("PerfDiag", "Live Diagnostics", 0, 0);
  if (g_ui.connected) {
    ImGui::Text("Critical: %lu  Realtime: %lu  NoteOn: %lu  Release lane: %lu",
                (unsigned long)g_ui.lastSnapshot.currentStats.criticalQueueDepth,
                (unsigned long)g_ui.lastSnapshot.currentStats.realtimeQueueDepth,
                (unsigned long)g_ui.lastSnapshot.currentStats.noteOnQueueDepth,
                (unsigned long)g_ui.lastSnapshot.currentStats.releaseLaneDepth);
    ImGui::Text("Events: %lu  Started: %lu  Dropped: %lu  NoteOff: %lu",
                (unsigned long)g_ui.lastSnapshot.currentStats.eventsProcessedThisBlock,
                (unsigned long)g_ui.lastSnapshot.currentStats.noteOnStartedThisBlock,
                (unsigned long)g_ui.lastSnapshot.currentStats.noteOnDroppedThisBlock,
                (unsigned long)g_ui.lastSnapshot.currentStats.noteOffEventsThisBlock);
  }
  EndCard();
}

void RenderFxTab() {
  BeginCard("FX", "Master And FX", 0, 0);
  InputFloatClamped("Master Volume", &g_ui.pendingSettings.masterVolume, 0.0f, 4.0f, 0.05f);
  if (g_ui.currentMode >= VIEW_ADVANCED) {
    ToggleButton("Enable Reverb", &g_ui.pendingSettings.reverbEnabled);
    InputFloatClamped("Reverb Mix", &g_ui.pendingSettings.reverbMix, 0.0f, 1.0f, 0.01f);
    InputFloatClamped("Reverb Feedback", &g_ui.pendingSettings.reverbFeedback, 0.0f, 0.97f, 0.01f);
    InputFloatClamped("Reverb Tone", &g_ui.pendingSettings.reverbTone, 0.02f, 0.95f, 0.01f);
    InputFloatClamped("Reverb Width", &g_ui.pendingSettings.reverbWidth, 0.0f, 1.0f, 0.01f);
    InputFloatClamped("Reverb Blur", &g_ui.pendingSettings.reverbBlur, 0.0f, 1.0f, 0.01f);
    ToggleButton("Enable Limiter", &g_ui.pendingSettings.limiterEnabled);
    InputFloatClamped("Limiter Threshold", &g_ui.pendingSettings.limiterThreshold, 0.1f, 1.0f, 0.01f);
    InputFloatClamped("Limiter Release ms", &g_ui.pendingSettings.limiterReleaseMs, 5.0f, 500.0f, 1.0f);
  }
  EndCard();
}

void RenderDiagnosticsTab() {
  BeginCard("DiagSummary", "Diagnostics Overview", 0, 0);
  HelpText("Hot stats are summarized here as compact trend cards. The synth only publishes raw counters; smoothing, graph history, and status interpretation stay in the configurator.");
  EndCard();

  if (g_ui.connected) {
    const LiveBridgeStats &stats = g_ui.lastSnapshot.currentStats;
    char primary[256];
    char secondary[256];
    char footer[256];

    if (ImGui::BeginTable("DiagnosticsGrid", 2,
                          ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_BordersInnerV)) {
      ImGui::TableNextColumn();
      sprintf(primary, "Block %.3f ms of %.3f ms budget | Render avg %.3f ms",
              stats.audioBlockMs, stats.audioBudgetMs, stats.synthRenderAvgMs);
      sprintf(secondary, "Peak block %.3f ms | Peak render %.3f ms",
              stats.audioBlockPeakMs, stats.synthRenderPeakMs);
      sprintf(footer, "Load state: %s", GetLoadStatus(
                          stats.audioBudgetMs > 0.0f
                              ? stats.audioBlockMs * 100.0f / stats.audioBudgetMs
                              : 0.0f));
      RenderSparklineCard("DiagAudio", "Audio Load", g_ui.historyBlockMs, 10.0f,
                          primary, secondary, footer);

      ImGui::TableNextColumn();
      sprintf(primary, "Queued %lu | Deferred %lu | Release lane %lu",
              (unsigned long)stats.queuedMidiEvents,
              (unsigned long)stats.deferredMidiEvents,
              (unsigned long)stats.releaseLaneDepth);
      sprintf(secondary, "Critical %lu | Realtime %lu | NoteOn %lu",
              (unsigned long)stats.criticalQueueDepth,
              (unsigned long)stats.realtimeQueueDepth,
              (unsigned long)stats.noteOnQueueDepth);
      sprintf(footer, "Queue max seen: %lu",
              (unsigned long)stats.maxQueuedMidiEvents);
      RenderSparklineCard("DiagQueues", "Queue Health", g_ui.historyQueueDepth,
                          4096.0f, primary, secondary, footer);

      ImGui::TableNextColumn();
      sprintf(primary, "VoiceEq %lu | Exact %lu | Grouped %lu | Density %lu",
              (unsigned long)stats.virtuallySuperVoiceEquivalent,
              (unsigned long)stats.virtuallySuperExactVoices,
              (unsigned long)stats.virtuallySuperGroupedObjects,
              (unsigned long)stats.virtuallySuperDensityObjects);
      sprintf(secondary, "Released exact %lu | Pressure %lu",
              (unsigned long)stats.virtuallySuperReleasedExactVoices,
              (unsigned long)stats.virtuallySuperPressureLevel);
      sprintf(footer, "Tier mix reflects current live scene state");
      RenderSparklineCard("DiagScene", "Scene Activity", g_ui.historyVsVoiceEq,
                          2048.0f, primary, secondary, footer);

      ImGui::TableNextColumn();
      sprintf(primary, "Overload %s | Consecutive blocks %lu",
              GetOverloadStatus(stats.overloadState),
              (unsigned long)stats.consecutiveOverloadBlocks);
      sprintf(secondary, "Hard entries %lu | Recoveries %lu | Worker blocked %lu",
              (unsigned long)stats.accurateHardOverloadEntries,
              (unsigned long)stats.accurateHardOverloadRecoveries,
              (unsigned long)stats.accurateWorkerBlockedCount);
      sprintf(footer, "Pressure level %lu", (unsigned long)stats.virtuallySuperPressureLevel);
      RenderSparklineCard("DiagOverload", "Overload", g_ui.historyOverload, 2.0f,
                          primary, secondary, footer);

      ImGui::TableNextColumn();
      sprintf(primary, "Started %lu | Dropped %lu | Attempted %lu",
              (unsigned long)stats.noteOnStartedThisBlock,
              (unsigned long)stats.noteOnDroppedThisBlock,
              (unsigned long)stats.noteOnEventsThisBlock);
      sprintf(secondary, "Async started %lu | Async dropped %lu | Coalesced %lu",
              (unsigned long)stats.asyncStartedThisBlock,
              (unsigned long)stats.asyncDroppedThisBlock,
              (unsigned long)stats.asyncCoalescedThisBlock);
      sprintf(footer, "Dropped note-ons under overload: %lu",
              (unsigned long)stats.overloadNoteOnsDroppedThisBlock);
      RenderSparklineCard("DiagNoteOn", "Note-On Flow", g_ui.historyNoteOnStarted,
                          512.0f, primary, secondary, footer);

      ImGui::TableNextColumn();
      sprintf(primary, "Applied %lu | Canceled %lu | Coalesced %lu",
              (unsigned long)stats.schedulerNoteOffsAppliedThisBlock,
              (unsigned long)stats.schedulerNoteOffsCanceledThisBlock,
              (unsigned long)stats.schedulerNoteOffsCoalescedThisBlock);
      sprintf(secondary, "Ingress %lu | Deferred %lu | Late %lu",
              (unsigned long)stats.noteOffIngressThisBlock,
              (unsigned long)stats.noteOffDeferredThisBlock,
              (unsigned long)stats.noteOffLateThisBlock);
      sprintf(footer, "Release state: %s", GetReleaseHealth(g_ui.lastSnapshot));
      RenderSparklineCard("DiagRelease", "Release Flow", g_ui.historyNoteOffApplied,
                          512.0f, primary, secondary, footer);
      ImGui::EndTable();
    }
  } else {
    BeginCard("DiagOffline", "Diagnostics", 0, 0);
    HelpText(g_ui.disconnectedSummary[0] ? g_ui.disconnectedSummary
                                         : "No live bridge is attached.");
    EndCard();
  }

  if (g_ui.currentMode >= VIEW_ADVANCED) {
    BeginCard("RawStats", "Raw Stats", 0, 0);
    char buffer[4096];
    BuildDiagnosticsText(buffer, sizeof(buffer));
    ImGui::InputTextMultiline("##raw", buffer, sizeof(buffer), ImVec2(-1, 220),
                              ImGuiInputTextFlags_ReadOnly);
    EndCard();
  }
}

void RenderProfilesTab() {
  BeginCard("Profiles", "Profiles", 0, 0);
  HelpText("Profiles are intentionally visible now, but persistence and import/export are not wired yet.");
  ImGui::BeginDisabled();
  static int dummyProfile = 0;
  const char *items[] = {"Reference", "Realtime", "Black MIDI", "Extreme"};
  ImGui::Combo("Built-in profiles", &dummyProfile, items, 4);
  ImGui::Button("Import");
  ImGui::SameLine();
  ImGui::Button("Export");
  ImGui::EndDisabled();
  EndCard();
}

void RenderAdvancedTab() {
  BeginCard("Advanced", "Advanced Readout", 0, 0);
  char buffer[2048];
  BuildAdvancedText(buffer, sizeof(buffer));
  ImGui::InputTextMultiline("##advanced", buffer, sizeof(buffer), ImVec2(-1, 180),
                            ImGuiInputTextFlags_ReadOnly);
  if (g_ui.currentMode >= VIEW_POWERUSER) {
    DrawPlaceholderField("Engine topology knobs", "Not wired yet");
    DrawPlaceholderField("Scheduler stress knobs", "Not wired yet");
    DrawPlaceholderField("Source policy presets", "Not wired yet");
  }
  EndCard();
}

void RenderDeveloperTab() {
  BeginCard("Developer", "Developer", 0, 0);
  ImGui::TextColored(ImVec4(0.92f, 0.68f, 0.18f, 1.0f),
                     "Developer mode is opt-in and may expose unstable controls.");
  bool enabled = g_ui.developerControlsEnabled;
  if (ImGui::Checkbox("Enable Developer Controls", &enabled))
    g_ui.developerControlsEnabled = enabled;
  ImGui::BeginDisabled(!g_ui.developerControlsEnabled || !g_ui.connected);
  if (ImGui::Button("Hard Reset")) {
    LiveBridgeSettings emptySettings;
    memset(&emptySettings, 0, sizeof(emptySettings));
    std::string message;
    SendCommandAndWait(LIVE_CMD_RESET_ENGINE, emptySettings, message);
  }
  ImGui::SameLine();
  if (ImGui::Button("Kill Engine")) {
    LiveBridgeSettings emptySettings;
    memset(&emptySettings, 0, sizeof(emptySettings));
    std::string message;
    SendCommandAndWait(LIVE_CMD_KILL_ENGINE, emptySettings, message);
  }
  ImGui::EndDisabled();
  char buffer[2048];
  BuildDeveloperText(buffer, sizeof(buffer));
  ImGui::InputTextMultiline("##developer", buffer, sizeof(buffer), ImVec2(-1, 220),
                            ImGuiInputTextFlags_ReadOnly);
  DrawPlaceholderField("Raw shared-memory editor", "Not wired yet");
  DrawPlaceholderField("Synthetic stress harness", "Not wired yet");
  DrawPlaceholderField("Trace export hooks", "Not wired yet");
  EndCard();
}

void RenderUI() {
  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::Begin("SVMSRoot", NULL, flags);
  RenderHeaderBar();

  if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_Reorderable)) {
    if (ImGui::BeginTabItem("Home")) { RenderHomeTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("SoundFonts")) { RenderSoundFontsTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Engine")) { RenderEngineTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Timing")) { RenderTimingTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Performance")) { RenderPerformanceTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("FX And Output")) { RenderFxTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Diagnostics")) { RenderDiagnosticsTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Profiles")) { RenderProfilesTab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Advanced")) { RenderAdvancedTab(); ImGui::EndTabItem(); }
    if (g_ui.currentMode == VIEW_DEVELOPER && ImGui::BeginTabItem("Developer")) {
      RenderDeveloperTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    return true;

  switch (msg) {
  case WM_SIZE:
    if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
      g_d3dpp.BackBufferWidth = LOWORD(lParam);
      g_d3dpp.BackBufferHeight = HIWORD(lParam);
      ResetDevice();
    }
    return 0;
  case WM_TIMER:
    if (wParam == kLivePollTimerId)
      PollLiveState();
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_SYSCOMMAND:
    if ((wParam & 0xfff0) == SC_KEYMENU)
      return 0;
    break;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hWnd, msg, wParam, lParam);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
  SetDisconnectedState("Waiting for a live SuperVirtualMIDISynth instance...",
                       "Launch a host using SuperVirtualMIDISynth, then the "
                       "configurator will attach automatically.");
  g_ui.currentMode = VIEW_BASIC;
  g_ui.needsReseed = true;
  g_ui.actionMessage[0] = '\0';

  WNDCLASSEXA wc;
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.lpszClassName = "SVMSConfiguratorV2ImGuiWindow";
  RegisterClassExA(&wc);

  g_ui.hwnd = CreateWindowA(wc.lpszClassName, "SVMS Configurator V2",
                            WS_OVERLAPPEDWINDOW, 100, 100, 1420, 980, NULL,
                            NULL, wc.hInstance, NULL);
  if (!g_ui.hwnd)
    return 1;
  if (!CreateDeviceD3D(g_ui.hwnd)) {
    CleanupDeviceD3D();
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = NULL;
  io.LogFilename = NULL;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ApplyImGuiStyle();
  LoadUiFont();

  ImGui_ImplWin32_Init(g_ui.hwnd);
  ImGui_ImplDX9_Init(g_pd3dDevice);

  ShowWindow(g_ui.hwnd, nCmdShow);
  UpdateWindow(g_ui.hwnd);
  SetTimer(g_ui.hwnd, kLivePollTimerId, kLivePollIntervalMs, NULL);
  PollLiveState();

  MSG msg;
  memset(&msg, 0, sizeof(msg));
  while (msg.message != WM_QUIT) {
    while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    if (msg.message == WM_QUIT)
      break;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RenderUI();

    ImGui::EndFrame();
    g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    D3DCOLOR clearColor = D3DCOLOR_RGBA(18, 20, 26, 255);
    g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                        clearColor, 1.0f, 0);
    if (g_pd3dDevice->BeginScene() >= 0) {
      ImGui::Render();
      ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
      g_pd3dDevice->EndScene();
    }
    HRESULT result = g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
    if (result == D3DERR_DEVICELOST &&
        g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
      ResetDevice();
    }
  }

  KillTimer(g_ui.hwnd, kLivePollTimerId);
  DisconnectBridge();
  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  CleanupDeviceD3D();
  DestroyWindow(g_ui.hwnd);
  UnregisterClassA(wc.lpszClassName, wc.hInstance);
  return 0;
}
