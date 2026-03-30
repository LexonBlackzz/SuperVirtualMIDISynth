#ifndef LIVE_RUNTIME_H
#define LIVE_RUNTIME_H

#include "Compat.h"
#include "LiveConfigProtocol.h"
#include <cstddef>

class LiveRuntime {
public:
  static LiveRuntime &Instance();

  void Initialize();
  void Shutdown(bool waitForThread);

  void UpdateAudioTimings(float synthRenderMs, float audioBlockMs,
                          float audioBudgetMs);
  void PublishStatus(const char *statusText);
  void PublishError(const char *statusText);
  void ResetAudioTimings();

private:
  LiveRuntime();
  ~LiveRuntime();
  LiveRuntime(const LiveRuntime &) = delete;
  LiveRuntime &operator=(const LiveRuntime &) = delete;

  static DWORD WINAPI WorkerThreadProc(LPVOID param);
  void WorkerLoop();
  void PublishSnapshot();
  void ProcessPendingCommand();
  bool ExecuteCommand(LONG commandCode, const LiveBridgeSettings &settings,
                      char *message, size_t messageCapacity);
  bool ApplySettings(const LiveBridgeSettings &settings, bool includeSoft,
                     bool includeHeavy, char *message,
                     size_t messageCapacity);
  bool ReloadConfig(char *message, size_t messageCapacity);
  bool RestartEngine(bool reloadConfig, unsigned int reasonCode,
                     const char *reasonText, char *message,
                     size_t messageCapacity);
  bool KillEngine(char *message, size_t messageCapacity);
  void WriteStatusLocked(const char *statusText);
  void CopyMessage(char *dest, size_t capacity, const char *text);
  bool LockBridge(DWORD timeoutMs);
  void UnlockBridge();
  void PopulateCurrentSettings(LiveBridgeSettings &settings);

  compat::Mutex stateMutex;
  compat::Mutex timingMutex;
  HANDLE mappingHandle;
  HANDLE bridgeMutexHandle;
  HANDLE stopEvent;
  HANDLE workerThread;
  LiveBridgeSharedState *sharedState;
  bool initialized;
  float lastSynthRenderMs;
  float averageSynthRenderMs;
  float peakSynthRenderMs;
  float lastAudioBlockMs;
  float averageAudioBlockMs;
  float peakAudioBlockMs;
  float lastAudioBudgetMs;
  DWORD lastAudioTimingTick;
  char lastStatusText[SVMS_MAX_STATUS_TEXT];
};

#endif
