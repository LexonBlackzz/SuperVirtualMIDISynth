#include "LiveConfigProtocol.h"
#include "OmniMIDI.h"
#include <algorithm>
#include <cmath>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <windows.h>

namespace {

enum ControlId {
  IDC_MODE = 100,
  IDC_PATTERN,
  IDC_TIMING_MODE,
  IDC_HZ,
  IDC_LOW_NOTE,
  IDC_HIGH_NOTE,
  IDC_NOTE_COUNT,
  IDC_VELOCITY,
  IDC_DUTY,
  IDC_DURATION,
  IDC_SUSTAIN,
  IDC_PITCH_SWEEP,
  IDC_SAME_SAMPLE,
  IDC_LOOP,
  IDC_LOAD_MIDI,
  IDC_PLAY,
  IDC_STOP,
  IDC_PANIC,
  IDC_STATUS,
  IDC_DEBUG
};

enum PlaybackMode {
  PLAYBACK_SYNTHETIC = 0,
  PLAYBACK_MIDI = 1
};

enum PatternMode {
  PATTERN_SAME_KEY = 0,
  PATTERN_ROUND_ROBIN = 1,
  PATTERN_CHORD = 2
};

struct ScheduledMessage {
  long long timeUs;
  DWORD shortMsg;
};

struct ParsedMidiEvent {
  unsigned int sequence;
  unsigned int tick;
  bool isTempo;
  unsigned int tempoUsPerQuarter;
  DWORD shortMsg;
};

struct KdmApi {
  HMODULE module;
  BOOL(WINAPI *InitializeKDMAPIStream)();
  BOOL(WINAPI *TerminateKDMAPIStream)();
  VOID(WINAPI *ResetKDMAPIStream)();
  VOID(WINAPI *SendDirectData)(DWORD);

  KdmApi()
      : module(NULL), InitializeKDMAPIStream(NULL),
        TerminateKDMAPIStream(NULL), ResetKDMAPIStream(NULL),
        SendDirectData(NULL) {}
};

struct LiveBridgeClient {
  HANDLE mapping;
  HANDLE mutex;
  LiveBridgeSharedState *sharedState;

  LiveBridgeClient() : mapping(NULL), mutex(NULL), sharedState(NULL) {}
};

struct UiState {
  HWND hwnd;
  DWORD uiThreadId;
  HWND modeCombo;
  HWND patternCombo;
  HWND timingModeCombo;
  HWND hzEdit;
  HWND lowNoteEdit;
  HWND highNoteEdit;
  HWND noteCountEdit;
  HWND velocityEdit;
  HWND dutyEdit;
  HWND durationEdit;
  HWND sustainCheck;
  HWND pitchSweepCheck;
  HWND sameSampleCheck;
  HWND loopCheck;
  HWND loadMidiButton;
  HWND playButton;
  HWND stopButton;
  HWND panicButton;
  HWND statusText;
  HWND debugText;
  HFONT font;
  bool playing;
  volatile bool stopRequested;
  HANDLE workerThread;
  DWORD workerThreadId;
  CRITICAL_SECTION metricsLock;
  std::string loadedMidiPath;
  std::string lastStatus;
  unsigned int eventsPerSecond;
  unsigned int noteOnsPerSecond;
  unsigned int noteOffsPerSecond;
  unsigned int activeNotesEstimate;
  bool sawAudioActivity;
  bool initSucceeded;
  bool liveBridgeReady;
  bool likelyStaleBinary;
  DWORD liveBridgeVersion;
  std::string loadedDllPath;
  std::string loadedDllStamp;
  std::string playerExePath;
  std::string playerExeStamp;
  std::string verifyDllPath;
  std::string verifyDllStamp;
  std::string verifyPlayerPath;
  std::string verifyPlayerStamp;
  std::string handshakeStatus;
};

UiState g_ui = {};
KdmApi g_kdmapi;
LiveBridgeClient g_liveBridge;

static const UINT_PTR kUiTimerId = 1;
static const UINT kUiTimerIntervalMs = 100;
static const UINT kStatusUpdateMessage = WM_APP + 1;

bool ReadLiveSnapshot(LiveBridgeSharedState &snapshot);

std::string GetExeDirectory() {
  char path[MAX_PATH];
  DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (!length || length >= MAX_PATH)
    return std::string(".");
  char *slash = strrchr(path, '\\');
  if (!slash)
    return std::string(".");
  *slash = '\0';
  return std::string(path);
}

std::string JoinPath(const std::string &a, const std::string &b) {
  if (a.empty())
    return b;
  if (a[a.size() - 1] == '\\')
    return a + b;
  return a + "\\" + b;
}

std::string GetModulePath(HMODULE module) {
  char path[MAX_PATH];
  DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
  if (!length || length >= MAX_PATH)
    return std::string();
  return std::string(path, path + length);
}

std::string FormatFileTimestamp(const std::string &path) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
    return "missing";

  FILETIME localFt = {};
  SYSTEMTIME st = {};
  if (!FileTimeToLocalFileTime(&data.ftLastWriteTime, &localFt) ||
      !FileTimeToSystemTime(&localFt, &st))
    return "present";

  char buffer[64];
  sprintf(buffer, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth,
          st.wDay, st.wHour, st.wMinute, st.wSecond);
  return buffer;
}

bool FileExists(const std::string &path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool GetLastWriteTimeUtc(const std::string &path, FILETIME &ft) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
    return false;
  ft = data.ftLastWriteTime;
  return true;
}

bool IsFileOlderThan(const std::string &lhs, const std::string &rhs) {
  FILETIME lhsFt = {};
  FILETIME rhsFt = {};
  if (!GetLastWriteTimeUtc(lhs, lhsFt) || !GetLastWriteTimeUtc(rhs, rhsFt))
    return false;
  return CompareFileTime(&lhsFt, &rhsFt) < 0;
}

std::string GetFileNameOnly(const std::string &path) {
  const char *slash = strrchr(path.c_str(), '\\');
  return slash ? std::string(slash + 1) : path;
}

bool ContainsInsensitive(const std::string &text, const char *needle) {
  if (!needle || !needle[0])
    return false;
  std::string lowerText = text;
  std::string lowerNeedle = needle;
  std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return lowerText.find(lowerNeedle) != std::string::npos;
}

bool IsStatusFailureText(const char *text) {
  std::string value = text ? text : "";
  return ContainsInsensitive(value, "fail") || ContainsInsensitive(value, "error") ||
         ContainsInsensitive(value, "unsupported");
}

bool IsVerifyBuildName(const std::string &path) {
  return ContainsInsensitive(GetFileNameOnly(path), "_verify");
}

std::string ChooseDllPath() {
  std::string exePath = GetModulePath(NULL);
  std::string exeDir = GetExeDirectory();
  bool preferVerify = IsVerifyBuildName(exePath);
  std::vector<std::string> candidates;
  if (preferVerify) {
    candidates.push_back(JoinPath(exeDir, "SuperVirtualMIDISynthx64_verify.dll"));
    candidates.push_back(JoinPath(exeDir, "SuperVirtualMIDISynthx64.dll"));
    candidates.push_back(JoinPath(exeDir, "..\\SuperVirtualMIDISynthx64.dll"));
  } else {
    candidates.push_back(JoinPath(exeDir, "SuperVirtualMIDISynthx64.dll"));
    candidates.push_back(JoinPath(exeDir, "SuperVirtualMIDISynthx64_verify.dll"));
    candidates.push_back(JoinPath(exeDir, "..\\build\\SuperVirtualMIDISynthx64_verify.dll"));
  }
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (FileExists(candidates[i]))
      return candidates[i];
  }
  return JoinPath(exeDir, "SuperVirtualMIDISynthx64.dll");
}

void RefreshBuildIdentity() {
  g_ui.playerExePath = GetModulePath(NULL);
  g_ui.playerExeStamp = FormatFileTimestamp(g_ui.playerExePath);
  g_ui.verifyDllPath = JoinPath(GetExeDirectory(), "SuperVirtualMIDISynthx64_verify.dll");
  g_ui.verifyPlayerPath = JoinPath(GetExeDirectory(), "SVMSBuzzTestPlayer_verify.exe");
  if (!FileExists(g_ui.verifyDllPath))
    g_ui.verifyDllPath = JoinPath(GetExeDirectory(), "..\\build\\SuperVirtualMIDISynthx64_verify.dll");
  if (!FileExists(g_ui.verifyPlayerPath))
    g_ui.verifyPlayerPath = JoinPath(GetExeDirectory(), "..\\build\\SVMSBuzzTestPlayer_verify.exe");
  g_ui.verifyDllStamp = FileExists(g_ui.verifyDllPath) ? FormatFileTimestamp(g_ui.verifyDllPath) : "missing";
  g_ui.verifyPlayerStamp = FileExists(g_ui.verifyPlayerPath) ? FormatFileTimestamp(g_ui.verifyPlayerPath) : "missing";

  bool newerVerifyDll =
      !g_ui.loadedDllPath.empty() && FileExists(g_ui.verifyDllPath) &&
      IsFileOlderThan(g_ui.loadedDllPath, g_ui.verifyDllPath);
  bool newerVerifyPlayer =
      !g_ui.playerExePath.empty() && FileExists(g_ui.verifyPlayerPath) &&
      IsFileOlderThan(g_ui.playerExePath, g_ui.verifyPlayerPath);
  if (newerVerifyDll || newerVerifyPlayer)
    g_ui.likelyStaleBinary = true;
}

void SetStatus(const char *text) {
  EnterCriticalSection(&g_ui.metricsLock);
  g_ui.lastStatus = text ? text : "";
  bool onUiThread = GetCurrentThreadId() == g_ui.uiThreadId;
  LeaveCriticalSection(&g_ui.metricsLock);

  if (!g_ui.statusText || !IsWindow(g_ui.statusText))
    return;

  if (onUiThread) {
    SetWindowTextA(g_ui.statusText, g_ui.lastStatus.c_str());
  } else if (g_ui.hwnd && IsWindow(g_ui.hwnd)) {
    PostMessageA(g_ui.hwnd, kStatusUpdateMessage, 0, 0);
  }
}

int GetWindowInt(HWND hwnd, int fallback) {
  char buffer[64];
  GetWindowTextA(hwnd, buffer, sizeof(buffer));
  return buffer[0] ? atoi(buffer) : fallback;
}

double GetWindowDouble(HWND hwnd, double fallback) {
  char buffer[64];
  GetWindowTextA(hwnd, buffer, sizeof(buffer));
  return buffer[0] ? atof(buffer) : fallback;
}

bool IsChecked(HWND hwnd) {
  return SendMessage(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

DWORD MakeShortMsg(BYTE status, BYTE data1, BYTE data2) {
  return static_cast<DWORD>(status | (data1 << 8) | (data2 << 16));
}

long long QueryNowUs() {
  LARGE_INTEGER counter = {};
  LARGE_INTEGER freq = {};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&freq);
  if (freq.QuadPart <= 0)
    return 0;
  return static_cast<long long>((counter.QuadPart * 1000000LL) / freq.QuadPart);
}

HANDLE GetPrecisionWaitableTimer() {
  static HANDLE timer = CreateWaitableTimerA(NULL, TRUE, NULL);
  return timer;
}

void PreciseSleepUntil(long long targetUs) {
  for (;;) {
    long long nowUs = QueryNowUs();
    long long remainingUs = targetUs - nowUs;
    if (remainingUs <= 0)
      return;
    if (remainingUs > 2000) {
      HANDLE timer = GetPrecisionWaitableTimer();
      if (timer) {
        LARGE_INTEGER dueTime = {};
        long long waitUs = remainingUs - 800;
        if (waitUs < 200)
          waitUs = 200;
        dueTime.QuadPart = -waitUs * 10;
        if (SetWaitableTimer(timer, &dueTime, 0, NULL, NULL, FALSE)) {
          WaitForSingleObject(timer, INFINITE);
          continue;
        }
      }
      Sleep(static_cast<DWORD>((remainingUs - 1000) / 1000));
    } else if (remainingUs > 300) {
      Sleep(0);
    } else {
      YieldProcessor();
    }
  }
}

bool LoadKdmApi() {
  if (g_kdmapi.module)
    return true;

  g_ui.loadedDllPath = ChooseDllPath();
  g_ui.loadedDllStamp = FormatFileTimestamp(g_ui.loadedDllPath);
  g_ui.likelyStaleBinary = false;
  std::string dllPath = g_ui.loadedDllPath;
  g_kdmapi.module = LoadLibraryA(dllPath.c_str());
  if (!g_kdmapi.module)
    return false;

  g_kdmapi.InitializeKDMAPIStream =
      reinterpret_cast<BOOL(WINAPI *)()>(GetProcAddress(g_kdmapi.module, "InitializeKDMAPIStream"));
  g_kdmapi.TerminateKDMAPIStream =
      reinterpret_cast<BOOL(WINAPI *)()>(GetProcAddress(g_kdmapi.module, "TerminateKDMAPIStream"));
  g_kdmapi.ResetKDMAPIStream =
      reinterpret_cast<VOID(WINAPI *)()>(GetProcAddress(g_kdmapi.module, "ResetKDMAPIStream"));
  g_kdmapi.SendDirectData =
      reinterpret_cast<VOID(WINAPI *)(DWORD)>(GetProcAddress(g_kdmapi.module, "SendDirectData"));

  if (!g_kdmapi.InitializeKDMAPIStream || !g_kdmapi.ResetKDMAPIStream ||
      !g_kdmapi.SendDirectData) {
    FreeLibrary(g_kdmapi.module);
    g_kdmapi = KdmApi();
    return false;
  }

  return true;
}

bool WaitForRuntimeReady(DWORD timeoutMs, std::string &failureReason) {
  DWORD start = GetTickCount();
  failureReason.clear();
  g_ui.liveBridgeReady = false;
  g_ui.sawAudioActivity = false;

  while (GetTickCount() - start < timeoutMs) {
    LiveBridgeSharedState snapshot;
    if (ReadLiveSnapshot(snapshot)) {
      g_ui.liveBridgeVersion = snapshot.version;
      g_ui.liveBridgeReady = true;
      if (snapshot.runtimeLoaded == 0) {
        failureReason = "Live bridge connected but runtime not loaded";
      } else if (IsStatusFailureText(snapshot.statusText)) {
        failureReason = snapshot.statusText;
      } else if (snapshot.currentStats.audioBudgetMs > 0.0001f ||
                 snapshot.currentStats.audioBlockMs > 0.0001f ||
                 snapshot.currentStats.audioBlockAvgMs > 0.0001f ||
                 snapshot.currentStats.totalActiveVoices > 0) {
        g_ui.sawAudioActivity = true;
        g_ui.handshakeStatus = "Runtime handshake OK";
        return true;
      } else if (snapshot.resolvedAudioBackend[0] &&
                 !ContainsInsensitive(snapshot.resolvedAudioBackend, "idle")) {
        failureReason = "Audio backend selected but no active audio budget yet";
      } else {
        failureReason = "Audio backend is still idle after KDMAPI init";
      }
    } else {
      failureReason = "Live bridge unavailable";
    }
    Sleep(25);
  }

  if (failureReason.empty())
    failureReason = "Timed out waiting for active audio runtime";
  g_ui.handshakeStatus = failureReason;
  return false;
}

bool EnsureLiveBridge() {
  if (g_liveBridge.sharedState && g_liveBridge.mapping && g_liveBridge.mutex)
    return true;

  if (g_liveBridge.sharedState) {
    UnmapViewOfFile(g_liveBridge.sharedState);
    g_liveBridge.sharedState = NULL;
  }
  if (g_liveBridge.mapping) {
    CloseHandle(g_liveBridge.mapping);
    g_liveBridge.mapping = NULL;
  }
  if (g_liveBridge.mutex) {
    CloseHandle(g_liveBridge.mutex);
    g_liveBridge.mutex = NULL;
  }

  g_liveBridge.mapping =
      OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SVMS_LIVE_BRIDGE_MAPPING_NAME);
  if (!g_liveBridge.mapping)
    return false;

  g_liveBridge.sharedState = static_cast<LiveBridgeSharedState *>(
      MapViewOfFile(g_liveBridge.mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                    sizeof(LiveBridgeSharedState)));
  if (!g_liveBridge.sharedState)
    return false;

  g_liveBridge.mutex =
      OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, SVMS_LIVE_BRIDGE_MUTEX_NAME);
  return g_liveBridge.mutex != NULL;
}

bool ReadLiveSnapshot(LiveBridgeSharedState &snapshot) {
  if (!EnsureLiveBridge())
    return false;
  DWORD wait = WaitForSingleObject(g_liveBridge.mutex, 50);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    return false;
  snapshot = *g_liveBridge.sharedState;
  ReleaseMutex(g_liveBridge.mutex);
  return snapshot.magic == SVMS_LIVE_BRIDGE_MAGIC &&
         snapshot.version == SVMS_LIVE_BRIDGE_VERSION &&
         snapshot.structSize == sizeof(LiveBridgeSharedState);
}

bool ApplyTimingMode(const char *mode) {
  LiveBridgeSharedState snapshot;
  if (!ReadLiveSnapshot(snapshot))
    return false;
  DWORD wait = WaitForSingleObject(g_liveBridge.mutex, 100);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    return false;

  LONG requestId = g_liveBridge.sharedState->commandRequestId + 1;
  if (requestId <= g_liveBridge.sharedState->commandProcessedId)
    requestId = g_liveBridge.sharedState->commandProcessedId + 1;
  if (requestId <= 0)
    requestId = 1;

  LiveBridgeSettings settings = snapshot.currentSettings;
  strncpy(settings.eventTimingMode, mode, sizeof(settings.eventTimingMode) - 1);
  settings.eventTimingMode[sizeof(settings.eventTimingMode) - 1] = '\0';
  settings.asyncNoteStarts = strcmp(mode, "legacy-sync") != 0;

  g_liveBridge.sharedState->commandRequestId = requestId;
  g_liveBridge.sharedState->commandCode = LIVE_CMD_APPLY_SOFT;
  g_liveBridge.sharedState->commandSourcePid = GetCurrentProcessId();
  g_liveBridge.sharedState->requestedSettings = settings;
  g_liveBridge.sharedState->commandInProgress = 0;
  g_liveBridge.sharedState->commandResult = LIVE_RESULT_BUSY;
  ReleaseMutex(g_liveBridge.mutex);

  DWORD start = GetTickCount();
  while (GetTickCount() - start < 3000) {
    LiveBridgeSharedState current;
    if (ReadLiveSnapshot(current) && current.commandProcessedId == requestId)
      return current.commandResult == LIVE_RESULT_OK;
    Sleep(16);
  }
  return false;
}

unsigned int ReadBe16(const BYTE *data) {
  return (static_cast<unsigned int>(data[0]) << 8) | data[1];
}

unsigned int ReadBe32(const BYTE *data) {
  return (static_cast<unsigned int>(data[0]) << 24) |
         (static_cast<unsigned int>(data[1]) << 16) |
         (static_cast<unsigned int>(data[2]) << 8) | data[3];
}

bool ReadVarLen(const std::vector<BYTE> &data, size_t &offset, unsigned int &value) {
  value = 0;
  for (int i = 0; i < 4; ++i) {
    if (offset >= data.size())
      return false;
    BYTE byte = data[offset++];
    value = (value << 7) | (byte & 0x7F);
    if ((byte & 0x80) == 0)
      return true;
  }
  return true;
}

bool LoadFileBytes(const std::string &path, std::vector<BYTE> &data) {
  FILE *file = NULL;
  fopen_s(&file, path.c_str(), "rb");
  if (!file)
    return false;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    fclose(file);
    return false;
  }
  data.resize(static_cast<size_t>(size));
  size_t read = fread(&data[0], 1, data.size(), file);
  fclose(file);
  return read == data.size();
}

bool ParseMidiFile(const std::string &path, std::vector<ScheduledMessage> &messages,
                   std::string &errorText) {
  std::vector<BYTE> bytes;
  if (!LoadFileBytes(path, bytes)) {
    errorText = "Failed to open MIDI file";
    return false;
  }
  if (bytes.size() < 14 || memcmp(&bytes[0], "MThd", 4) != 0) {
    errorText = "Invalid MIDI header";
    return false;
  }

  unsigned int headerLength = ReadBe32(&bytes[4]);
  unsigned int division = ReadBe16(&bytes[12]);
  unsigned int trackCount = ReadBe16(&bytes[10]);
  if ((division & 0x8000) != 0 || division == 0) {
    errorText = "Unsupported SMPTE MIDI timing";
    return false;
  }

  size_t offset = 8 + headerLength;
  std::vector<ParsedMidiEvent> parsed;
  parsed.reserve(65536);
  unsigned int globalSequence = 0;

  for (unsigned int track = 0; track < trackCount; ++track) {
    if (offset + 8 > bytes.size() || memcmp(&bytes[offset], "MTrk", 4) != 0) {
      errorText = "Invalid MIDI track chunk";
      return false;
    }
    unsigned int trackLength = ReadBe32(&bytes[offset + 4]);
    offset += 8;
    size_t trackEnd = offset + trackLength;
    if (trackEnd > bytes.size()) {
      errorText = "Corrupt MIDI track";
      return false;
    }

    unsigned int tick = 0;
    BYTE runningStatus = 0;
    while (offset < trackEnd) {
      unsigned int delta = 0;
      if (!ReadVarLen(bytes, offset, delta)) {
        errorText = "Invalid MIDI delta time";
        return false;
      }
      tick += delta;
      if (offset >= trackEnd)
        break;

      BYTE status = bytes[offset++];
      if (status < 0x80) {
        if (!runningStatus) {
          errorText = "Missing running status";
          return false;
        }
        --offset;
        status = runningStatus;
      } else if (status < 0xF0) {
        runningStatus = status;
      }

      if ((status & 0xF0) == 0x80 || (status & 0xF0) == 0x90 ||
          (status & 0xF0) == 0xB0 || (status & 0xF0) == 0xE0) {
        if (offset + 2 > trackEnd) {
          errorText = "Truncated MIDI event";
          return false;
        }
        BYTE data1 = bytes[offset++];
        BYTE data2 = bytes[offset++];
        parsed.push_back({globalSequence++, tick, false, 0,
                          MakeShortMsg(status, data1, data2)});
      } else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
        if (offset + 1 > trackEnd) {
          errorText = "Truncated MIDI event";
          return false;
        }
        BYTE data1 = bytes[offset++];
        parsed.push_back({globalSequence++, tick, false, 0,
                          MakeShortMsg(status, data1, 0)});
      } else if (status == 0xFF) {
        if (offset >= trackEnd) {
          errorText = "Truncated MIDI meta event";
          return false;
        }
        BYTE metaType = bytes[offset++];
        unsigned int length = 0;
        if (!ReadVarLen(bytes, offset, length) || offset + length > trackEnd) {
          errorText = "Invalid MIDI meta length";
          return false;
        }
        if (metaType == 0x51 && length == 3) {
          unsigned int tempo = (bytes[offset] << 16) | (bytes[offset + 1] << 8) |
                               bytes[offset + 2];
          parsed.push_back({globalSequence++, tick, true, tempo, 0});
        }
        offset += length;
      } else if (status == 0xF0 || status == 0xF7) {
        unsigned int length = 0;
        if (!ReadVarLen(bytes, offset, length) || offset + length > trackEnd) {
          errorText = "Invalid SysEx event";
          return false;
        }
        offset += length;
      } else {
        errorText = "Unsupported MIDI event";
        return false;
      }
    }

    offset = trackEnd;
  }

  std::sort(parsed.begin(), parsed.end(),
            [](const ParsedMidiEvent &a, const ParsedMidiEvent &b) {
              if (a.tick != b.tick)
                return a.tick < b.tick;
              return a.sequence < b.sequence;
            });

  messages.clear();
  messages.reserve(parsed.size());
  unsigned int currentTempo = 500000;
  unsigned int lastTick = 0;
  long long currentUs = 0;
  for (size_t i = 0; i < parsed.size(); ++i) {
    currentUs += static_cast<long long>(parsed[i].tick - lastTick) *
                 static_cast<long long>(currentTempo) / division;
    lastTick = parsed[i].tick;
    if (parsed[i].isTempo)
      currentTempo = parsed[i].tempoUsPerQuarter;
    else
      messages.push_back({currentUs, parsed[i].shortMsg});
  }

  errorText.clear();
  return true;
}

void AddSyntheticNotePair(std::vector<ScheduledMessage> &messages, long long startUs,
                          long long offUs, BYTE note, BYTE velocity,
                          bool sameSampleRetrigger) {
  if (sameSampleRetrigger) {
    messages.push_back({startUs, MakeShortMsg(0x80, note, 0)});
    messages.push_back({startUs, MakeShortMsg(0x90, note, velocity)});
  } else {
    messages.push_back({startUs, MakeShortMsg(0x90, note, velocity)});
  }
  messages.push_back({offUs, MakeShortMsg(0x80, note, 0)});
}

void BuildSyntheticPattern(std::vector<ScheduledMessage> &messages) {
  messages.clear();
  int pattern = static_cast<int>(SendMessage(g_ui.patternCombo, CB_GETCURSEL, 0, 0));
  double hz = GetWindowDouble(g_ui.hzEdit, 32.0);
  int lowNote = GetWindowInt(g_ui.lowNoteEdit, 60);
  int highNote = GetWindowInt(g_ui.highNoteEdit, 72);
  int noteCount = GetWindowInt(g_ui.noteCountEdit, 1);
  int velocity = GetWindowInt(g_ui.velocityEdit, 110);
  double duty = GetWindowDouble(g_ui.dutyEdit, 50.0) / 100.0;
  double durationSeconds = GetWindowDouble(g_ui.durationEdit, 5.0);
  bool sustain = IsChecked(g_ui.sustainCheck);
  bool pitchSweep = IsChecked(g_ui.pitchSweepCheck);
  bool sameSample = IsChecked(g_ui.sameSampleCheck);

  if (hz < 0.1)
    hz = 0.1;
  if (lowNote < 0)
    lowNote = 0;
  if (highNote > 127)
    highNote = 127;
  if (highNote < lowNote)
    highNote = lowNote;
  if (noteCount < 1)
    noteCount = 1;
  if (velocity < 1)
    velocity = 1;
  if (velocity > 127)
    velocity = 127;
  if (duty < 0.01)
    duty = 0.01;
  if (duty > 0.99)
    duty = 0.99;
  if (durationSeconds < 0.1)
    durationSeconds = 0.1;

  std::vector<BYTE> notes;
  int span = highNote - lowNote + 1;
  for (int i = 0; i < noteCount; ++i)
    notes.push_back(static_cast<BYTE>(lowNote + (i % span)));
  if (notes.empty())
    notes.push_back(static_cast<BYTE>(lowNote));

  long long totalUs = static_cast<long long>(durationSeconds * 1000000.0);
  long long periodUs = static_cast<long long>(1000000.0 / hz);
  if (periodUs < 100)
    periodUs = 100;
  long long noteLengthUs = static_cast<long long>(periodUs * duty);
  if (noteLengthUs < 1)
    noteLengthUs = 1;

  messages.push_back({0, MakeShortMsg(0xB0, 121, 0)});
  if (sustain)
    messages.push_back({0, MakeShortMsg(0xB0, 64, 127)});

  for (long long t = 0, cycle = 0; t < totalUs; t += periodUs, ++cycle) {
    if (pattern == PATTERN_SAME_KEY) {
      AddSyntheticNotePair(messages, t, t + noteLengthUs, notes[0],
                           static_cast<BYTE>(velocity), sameSample);
    } else if (pattern == PATTERN_ROUND_ROBIN) {
      AddSyntheticNotePair(messages, t, t + noteLengthUs,
                           notes[cycle % notes.size()],
                           static_cast<BYTE>(velocity), sameSample);
    } else {
      for (size_t i = 0; i < notes.size(); ++i)
        AddSyntheticNotePair(messages, t, t + noteLengthUs, notes[i],
                             static_cast<BYTE>(velocity), sameSample);
    }
  }

  if (pitchSweep) {
    for (long long t = 0; t < totalUs; t += 8000) {
      double phase = static_cast<double>(t) / static_cast<double>(totalUs);
      int bend = static_cast<int>(8192 + std::sin(phase * 6.28318530718) * 8191.0);
      if (bend < 0)
        bend = 0;
      if (bend > 16383)
        bend = 16383;
      messages.push_back({t, MakeShortMsg(0xE0, bend & 0x7F, (bend >> 7) & 0x7F)});
    }
    messages.push_back({totalUs, MakeShortMsg(0xE0, 0, 64)});
  }

  if (sustain)
    messages.push_back({totalUs, MakeShortMsg(0xB0, 64, 0)});
  messages.push_back({totalUs + 1000, MakeShortMsg(0xB0, 123, 0)});

  std::sort(messages.begin(), messages.end(),
            [](const ScheduledMessage &a, const ScheduledMessage &b) {
              if (a.timeUs != b.timeUs)
                return a.timeUs < b.timeUs;
              return a.shortMsg < b.shortMsg;
            });
}

void SendPanic() {
  if (!LoadKdmApi())
    return;
  for (int channel = 0; channel < 16; ++channel) {
    g_kdmapi.SendDirectData(MakeShortMsg(static_cast<BYTE>(0xB0 | channel), 64, 0));
    g_kdmapi.SendDirectData(MakeShortMsg(static_cast<BYTE>(0xB0 | channel), 123, 0));
    g_kdmapi.SendDirectData(MakeShortMsg(static_cast<BYTE>(0xB0 | channel), 121, 0));
  }
  if (g_kdmapi.ResetKDMAPIStream)
    g_kdmapi.ResetKDMAPIStream();
}

void UpdatePlaybackMetrics(DWORD shortMsg, unsigned int &events, unsigned int &ons,
                           unsigned int &offs, int &activeNotes) {
  ++events;
  BYTE status = static_cast<BYTE>(shortMsg & 0xFF);
  BYTE command = status & 0xF0;
  BYTE velocity = static_cast<BYTE>((shortMsg >> 16) & 0x7F);
  if (command == 0x90 && velocity > 0) {
    ++ons;
    ++activeNotes;
  } else if (command == 0x80 || (command == 0x90 && velocity == 0)) {
    ++offs;
    if (activeNotes > 0)
      --activeNotes;
  }
}

DWORD WINAPI PlaybackThreadProc(LPVOID) {
  timeBeginPeriod(1);
  g_ui.stopRequested = false;
  g_ui.initSucceeded = false;
  g_ui.liveBridgeReady = false;
  g_ui.sawAudioActivity = false;
  g_ui.handshakeStatus = "Initializing...";

  if (!LoadKdmApi()) {
    SetStatus("Failed to load SuperVirtualMIDISynth DLL");
    g_ui.playing = false;
    timeEndPeriod(1);
    return 1;
  }
  if (!g_kdmapi.InitializeKDMAPIStream || !g_kdmapi.InitializeKDMAPIStream()) {
    g_ui.handshakeStatus = "InitializeKDMAPIStream failed";
    SetStatus("Failed to initialize KDMAPI stream");
    if (g_kdmapi.TerminateKDMAPIStream)
      g_kdmapi.TerminateKDMAPIStream();
    g_ui.playing = false;
    timeEndPeriod(1);
    return 1;
  }
  g_ui.initSucceeded = true;

  std::string runtimeFailure;
  if (!WaitForRuntimeReady(2500, runtimeFailure)) {
    g_ui.likelyStaleBinary =
        ContainsInsensitive(runtimeFailure, "idle") ||
        ContainsInsensitive(runtimeFailure, "budget") ||
        ContainsInsensitive(runtimeFailure, "unavailable");
    SetStatus(runtimeFailure.c_str());
    SendPanic();
    if (g_kdmapi.TerminateKDMAPIStream)
      g_kdmapi.TerminateKDMAPIStream();
    g_ui.playing = false;
    timeEndPeriod(1);
    return 1;
  }

  static const char *timingModes[] = {"accurate", "quantized", "legacy-sync"};
  int timingSelection =
      static_cast<int>(SendMessage(g_ui.timingModeCombo, CB_GETCURSEL, 0, 0));
  if (timingSelection < 0 || timingSelection > 2)
    timingSelection = 0;
  ApplyTimingMode(timingModes[timingSelection]);
  SetStatus("Playback active");

  unsigned int eventsPerSecond = 0;
  unsigned int noteOnsPerSecond = 0;
  unsigned int noteOffsPerSecond = 0;
  int activeNotes = 0;
  DWORD secondWindowStart = GetTickCount();
  bool loop = IsChecked(g_ui.loopCheck);

  do {
    std::vector<ScheduledMessage> schedule;
    if (SendMessage(g_ui.modeCombo, CB_GETCURSEL, 0, 0) == PLAYBACK_MIDI) {
      std::string errorText;
      if (g_ui.loadedMidiPath.empty() ||
          !ParseMidiFile(g_ui.loadedMidiPath, schedule, errorText)) {
        SetStatus(errorText.empty() ? "Failed to parse MIDI file" : errorText.c_str());
        break;
      }
    } else {
      BuildSyntheticPattern(schedule);
    }

    if (schedule.empty()) {
      SetStatus("Nothing scheduled to play");
      break;
    }

    long long baseUs = QueryNowUs();
    for (size_t i = 0; i < schedule.size(); ++i) {
      if (g_ui.stopRequested)
        break;
      PreciseSleepUntil(baseUs + schedule[i].timeUs);
      g_kdmapi.SendDirectData(schedule[i].shortMsg);
      UpdatePlaybackMetrics(schedule[i].shortMsg, eventsPerSecond, noteOnsPerSecond,
                            noteOffsPerSecond, activeNotes);

      DWORD nowTick = GetTickCount();
      if (nowTick - secondWindowStart >= 1000) {
        EnterCriticalSection(&g_ui.metricsLock);
        g_ui.eventsPerSecond = eventsPerSecond;
        g_ui.noteOnsPerSecond = noteOnsPerSecond;
        g_ui.noteOffsPerSecond = noteOffsPerSecond;
        g_ui.activeNotesEstimate =
            activeNotes > 0 ? static_cast<unsigned int>(activeNotes) : 0;
        LeaveCriticalSection(&g_ui.metricsLock);
        secondWindowStart = nowTick;
        eventsPerSecond = 0;
        noteOnsPerSecond = 0;
        noteOffsPerSecond = 0;
      }
    }
  } while (!g_ui.stopRequested && loop);

  SendPanic();
  if (g_kdmapi.TerminateKDMAPIStream)
    g_kdmapi.TerminateKDMAPIStream();

  EnterCriticalSection(&g_ui.metricsLock);
  g_ui.activeNotesEstimate = 0;
  LeaveCriticalSection(&g_ui.metricsLock);

  g_ui.playing = false;
  if (g_ui.stopRequested)
    SetStatus("Stopped");
  else if (!g_ui.sawAudioActivity)
    SetStatus("Playback blocked: audio runtime never became active");
  else
    SetStatus("Playback finished");
  timeEndPeriod(1);
  return 0;
}

void StartPlayback() {
  if (g_ui.playing)
    return;
  g_ui.playing = true;
  g_ui.stopRequested = false;
  SetStatus("Playing...");
  g_ui.workerThread =
      CreateThread(NULL, 0, PlaybackThreadProc, NULL, 0, &g_ui.workerThreadId);
  if (!g_ui.workerThread) {
    g_ui.playing = false;
    SetStatus("Failed to create playback thread");
  }
}

void StopPlayback() {
  if (!g_ui.playing)
    return;
  g_ui.stopRequested = true;
  HANDLE thread = g_ui.workerThread;
  if (thread) {
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    if (g_ui.workerThread == thread)
      g_ui.workerThread = NULL;
  }
  g_ui.playing = false;
  SendPanic();
}

void UpdateDebugView() {
  std::string text;
  char line[512];

  RefreshBuildIdentity();

  EnterCriticalSection(&g_ui.metricsLock);
  sprintf(line,
          "Player: %s   Init: %s   Bridge: %s   Audio seen: %s\r\nEvents/s: %u   NoteOns/s: %u   NoteOffs/s: %u   Active est: %u\r\n",
          g_ui.playing ? "Running" : "Idle",
          g_ui.initSucceeded ? "ok" : "no",
          g_ui.liveBridgeReady ? "yes" : "no",
          g_ui.sawAudioActivity ? "yes" : "no",
          g_ui.eventsPerSecond,
          g_ui.noteOnsPerSecond, g_ui.noteOffsPerSecond,
          g_ui.activeNotesEstimate);
  text += line;
  LeaveCriticalSection(&g_ui.metricsLock);

  sprintf(line,
          "Loaded DLL: %s\r\nDLL stamp: %s   EXE stamp: %s   Bridge v%lu\r\nHandshake: %s\r\n",
          g_ui.loadedDllPath.empty() ? "(not loaded yet)" : g_ui.loadedDllPath.c_str(),
          g_ui.loadedDllStamp.empty() ? "n/a" : g_ui.loadedDllStamp.c_str(),
          g_ui.playerExeStamp.empty() ? "n/a" : g_ui.playerExeStamp.c_str(),
          static_cast<unsigned long>(g_ui.liveBridgeVersion),
          g_ui.handshakeStatus.empty() ? "(idle)" : g_ui.handshakeStatus.c_str());
  text += line;

  if (FileExists(g_ui.verifyDllPath) || FileExists(g_ui.verifyPlayerPath)) {
    sprintf(line,
            "Verify pair: DLL %s (%s)   EXE %s (%s)\r\n",
            g_ui.verifyDllPath.c_str(), g_ui.verifyDllStamp.c_str(),
            g_ui.verifyPlayerPath.c_str(), g_ui.verifyPlayerStamp.c_str());
    text += line;
  }

  if (g_ui.likelyStaleBinary)
    text += "Warning: runtime still looks idle after init; this is likely a stale or locked binary pair.\r\n";

  LiveBridgeSharedState snapshot;
  if (ReadLiveSnapshot(snapshot)) {
    if (snapshot.currentStats.audioBudgetMs > 0.0001f ||
        snapshot.currentStats.audioBlockMs > 0.0001f ||
        snapshot.currentStats.totalActiveVoices > 0)
      g_ui.sawAudioActivity = true;
    sprintf(line,
            "Live: %s / %s   Timing %s   Runtime %ld\r\nStatus: %s\r\nPending %lu   Same-key %lu   NoteOn coal %lu   NoteOff applied %lu   Coalesced %lu   Canceled %lu   Release ctl %lu\r\nLag %.3f ms   Synth %.3f ms   Block %.3f ms   Budget %.3f ms   Voices %lu   Late %lu   Splits %lu   Due %lu\r\n",
            snapshot.resolvedAudioBackend[0] ? snapshot.resolvedAudioBackend : "?",
            snapshot.resolvedSamplerEngine[0] ? snapshot.resolvedSamplerEngine : "?",
            snapshot.currentSettings.eventTimingMode[0]
                ? snapshot.currentSettings.eventTimingMode
                : "?",
            static_cast<long>(snapshot.runtimeLoaded),
            snapshot.statusText[0] ? snapshot.statusText : "(none)",
            static_cast<unsigned long>(snapshot.currentStats.asyncPendingNoteOns),
            static_cast<unsigned long>(
                snapshot.currentStats.schedulerPendingSameKeyTransitions),
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
            snapshot.currentStats.schedulerLagMs,
            snapshot.currentStats.synthRenderMs,
            snapshot.currentStats.audioBlockMs,
            snapshot.currentStats.audioBudgetMs,
            static_cast<unsigned long>(snapshot.currentStats.totalActiveVoices),
            static_cast<unsigned long>(
                snapshot.currentStats.schedulerLateEventsThisBlock),
            static_cast<unsigned long>(
                snapshot.currentStats.schedulerRenderSplitsThisBlock),
            static_cast<unsigned long>(
                snapshot.currentStats.schedulerDueEventsThisBlock));
    text += line;
  } else {
    text += "Live: bridge unavailable\r\n";
  }

  if (!g_ui.loadedMidiPath.empty()) {
    text += "MIDI: ";
    text += g_ui.loadedMidiPath;
    text += "\r\n";
  }

  SetWindowTextA(g_ui.debugText, text.c_str());
}

void LoadDefaultMidi() {
  std::string candidate = JoinPath(GetExeDirectory(), "BPM=RT Audio.mid");
  if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
    g_ui.loadedMidiPath = candidate;
}

void BrowseMidiFile() {
  char file[MAX_PATH] = "";
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_ui.hwnd;
  ofn.lpstrFilter = "MIDI Files\0*.mid;*.midi\0All Files\0*.*\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST;
  if (GetOpenFileNameA(&ofn)) {
    g_ui.loadedMidiPath = file;
    SetStatus("Loaded MIDI file");
  }
}

HWND CreateControl(DWORD exStyle, const char *className, const char *text,
                   DWORD style, int x, int y, int w, int h, HWND parent,
                   int id) {
  return CreateWindowExA(exStyle, className, text, style, x, y, w, h, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandle(NULL), NULL);
}

HWND AddLabeledControl(HWND parent, const char *label, int x, int y, int labelW,
                       int controlX, int controlW, int id, const char *initial) {
  HWND labelWnd = CreateControl(0, "STATIC", label,
                                WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, labelW,
                                20, parent, 0);
  SendMessageA(labelWnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);
  HWND edit = CreateControl(WS_EX_CLIENTEDGE, "EDIT", initial,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                            controlX, y - 2, controlW, 24, parent, id);
  SendMessageA(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);
  return edit;
}

HWND CreateEdit(HWND parent, int id, int x, int y, int w, const char *initial) {
  HWND edit = CreateControl(WS_EX_CLIENTEDGE, "EDIT", initial,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                            x, y, w, 24, parent, id);
  SendMessageA(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);
  return edit;
}

HWND CreateCheck(HWND parent, int id, int x, int y, int w, const char *text) {
  HWND check = CreateControl(0, "BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 BS_AUTOCHECKBOX,
                             x, y, w, 22, parent, id);
  SendMessageA(check, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);
  return check;
}

void PopulateCombo(HWND combo, const char *const *items, size_t count) {
  for (size_t i = 0; i < count; ++i)
    SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
  SendMessage(combo, CB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE: {
    g_ui.hwnd = hwnd;
    g_ui.uiThreadId = GetCurrentThreadId();
    g_ui.font = CreateFontA(-MulDiv(9, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72),
                            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    HWND title = CreateControl(0, "STATIC", "SVMS Buzz Test Player",
                               WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 12, 300, 24,
                               hwnd, 0);
    SendMessageA(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);

    HWND subtitle = CreateControl(
        0, "STATIC",
        "Controlled-Hz retrigger generator plus direct MIDI playback for realtime scheduler listening.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 36, 760, 18, hwnd, 0);
    SendMessageA(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);

    HWND modeLabel = CreateControl(0, "STATIC", "Mode",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 74, 70,
                                   20, hwnd, 0);
    SendMessageA(modeLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);
    g_ui.modeCombo = CreateControl(0, "COMBOBOX", "",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       CBS_DROPDOWNLIST,
                                   90, 70, 150, 300, hwnd, IDC_MODE);
    SendMessageA(g_ui.modeCombo, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font),
                 TRUE);
    const char *modeItems[] = {"Synthetic", "MIDI File"};
    PopulateCombo(g_ui.modeCombo, modeItems, 2);

    HWND patternLabel = CreateControl(0, "STATIC", "Pattern",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT, 260, 74,
                                      70, 20, hwnd, 0);
    SendMessageA(patternLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font),
                 TRUE);
    g_ui.patternCombo = CreateControl(0, "COMBOBOX", "",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                          CBS_DROPDOWNLIST,
                                      330, 70, 150, 300, hwnd, IDC_PATTERN);
    SendMessageA(g_ui.patternCombo, WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_ui.font), TRUE);
    const char *patternItems[] = {"Same key", "Round robin", "Chord"};
    PopulateCombo(g_ui.patternCombo, patternItems, 3);

    HWND timingLabel = CreateControl(0, "STATIC", "Timing",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT, 500, 74,
                                     70, 20, hwnd, 0);
    SendMessageA(timingLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font),
                 TRUE);
    g_ui.timingModeCombo = CreateControl(0, "COMBOBOX", "",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                             CBS_DROPDOWNLIST,
                                         570, 70, 150, 300, hwnd,
                                         IDC_TIMING_MODE);
    SendMessageA(g_ui.timingModeCombo, WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_ui.font), TRUE);
    const char *timingItems[] = {"accurate", "quantized", "legacy-sync"};
    PopulateCombo(g_ui.timingModeCombo, timingItems, 3);

    g_ui.hzEdit = AddLabeledControl(hwnd, "Hz", 16, 116, 70, 90, 90, IDC_HZ,
                                    "32.0");
    g_ui.lowNoteEdit = AddLabeledControl(hwnd, "Low note", 200, 116, 70, 274,
                                         70, IDC_LOW_NOTE, "60");
    g_ui.highNoteEdit = AddLabeledControl(hwnd, "High note", 370, 116, 70, 444,
                                          70, IDC_HIGH_NOTE, "72");
    g_ui.noteCountEdit = AddLabeledControl(hwnd, "Note count", 530, 116, 80, 618,
                                           70, IDC_NOTE_COUNT, "1");
    g_ui.velocityEdit = AddLabeledControl(hwnd, "Velocity", 16, 152, 70, 90, 90,
                                          IDC_VELOCITY, "110");
    g_ui.dutyEdit = AddLabeledControl(hwnd, "Duty %", 200, 152, 70, 274, 70,
                                      IDC_DUTY, "50");
    g_ui.durationEdit = AddLabeledControl(hwnd, "Duration s", 370, 152, 70, 444,
                                          70, IDC_DURATION, "5.0");

    g_ui.sustainCheck =
        CreateCheck(hwnd, IDC_SUSTAIN, 16, 192, 110, "Sustain");
    g_ui.pitchSweepCheck =
        CreateCheck(hwnd, IDC_PITCH_SWEEP, 136, 192, 120, "Pitch sweep");
    g_ui.sameSampleCheck =
        CreateCheck(hwnd, IDC_SAME_SAMPLE, 266, 192, 170, "Same-sample off/on");
    g_ui.loopCheck = CreateCheck(hwnd, IDC_LOOP, 446, 192, 80, "Loop");

    g_ui.loadMidiButton =
        CreateControl(0, "BUTTON", "Load MIDI",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 16, 230,
                      100, 28, hwnd, IDC_LOAD_MIDI);
    g_ui.playButton = CreateControl(
        0, "BUTTON", "Play",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 126, 230, 100,
        28, hwnd, IDC_PLAY);
    g_ui.stopButton =
        CreateControl(0, "BUTTON", "Stop",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 236, 230,
                      100, 28, hwnd, IDC_STOP);
    g_ui.panicButton =
        CreateControl(0, "BUTTON", "Panic",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 346, 230,
                      100, 28, hwnd, IDC_PANIC);
    SendMessageA(g_ui.loadMidiButton, WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_ui.font), TRUE);
    SendMessageA(g_ui.playButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font),
                 TRUE);
    SendMessageA(g_ui.stopButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font),
                 TRUE);
    SendMessageA(g_ui.panicButton, WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_ui.font), TRUE);

    HWND midiLabel = CreateControl(0, "STATIC", "MIDI file",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 276, 70,
                                   20, hwnd, 0);
    SendMessageA(midiLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font), TRUE);
    g_ui.statusText = CreateControl(WS_EX_CLIENTEDGE, "EDIT", "",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                                        ES_READONLY,
                                    16, 300, 760, 24, hwnd, IDC_STATUS);
    SendMessageA(g_ui.statusText, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font),
                 TRUE);

    g_ui.debugText = CreateControl(WS_EX_CLIENTEDGE, "EDIT", "",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                       ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                   16, 336, 760, 260, hwnd, IDC_DEBUG);
    SendMessageA(g_ui.debugText, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.font),
                 TRUE);

    InitializeCriticalSection(&g_ui.metricsLock);
    LoadDefaultMidi();
    SetStatus("Ready");
    SetTimer(hwnd, kUiTimerId, kUiTimerIntervalMs, NULL);
    UpdateDebugView();
    return 0;
  }

  case kStatusUpdateMessage:
    if (g_ui.statusText && IsWindow(g_ui.statusText))
      SetWindowTextA(g_ui.statusText, g_ui.lastStatus.c_str());
    return 0;

  case WM_TIMER:
    if (wParam == kUiTimerId)
      UpdateDebugView();
    return 0;

  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDC_LOAD_MIDI:
      BrowseMidiFile();
      UpdateDebugView();
      return 0;
    case IDC_PLAY:
      StartPlayback();
      return 0;
    case IDC_STOP:
      StopPlayback();
      return 0;
    case IDC_PANIC:
      SendPanic();
      SetStatus("Panic sent");
      return 0;
    case IDC_MODE:
      if (HIWORD(wParam) == CBN_SELCHANGE)
        UpdateDebugView();
      return 0;
    }
    break;

  case WM_CLOSE:
    StopPlayback();
    DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY:
    KillTimer(hwnd, kUiTimerId);
    StopPlayback();
    if (g_ui.font)
      DeleteObject(g_ui.font);
    DeleteCriticalSection(&g_ui.metricsLock);
    if (g_liveBridge.sharedState)
      UnmapViewOfFile(g_liveBridge.sharedState);
    if (g_liveBridge.mapping)
      CloseHandle(g_liveBridge.mapping);
    if (g_liveBridge.mutex)
      CloseHandle(g_liveBridge.mutex);
    if (g_kdmapi.module)
      FreeLibrary(g_kdmapi.module);
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
  WNDCLASSA wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = "SVMSBuzzTestPlayerWindow";
  if (!RegisterClassA(&wc))
    return 1;

  HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "SVMS Buzz Test Player",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                  WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 810, 660, NULL,
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
