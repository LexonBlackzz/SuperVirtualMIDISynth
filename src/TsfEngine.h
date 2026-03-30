#ifndef TSF_ENGINE_H
#define TSF_ENGINE_H

#include "Compat.h"
#include "SamplerEngine.h"
#include <atomic>
#include <string>
#include <vector>

#ifndef SVMS_LEGACY_XP
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

struct tsf;

class TsfEngine : public ISamplerEngine {
public:
  TsfEngine();
  ~TsfEngine() override;

  bool Initialize(const SamplerInitParams &params) override;
  void Shutdown(bool waitForThreads) override;
  void Reset() override;
  void ReloadRuntimeSettings(const RuntimeSettings &settings) override;
  void SetRealtimePressure(unsigned int overloadState,
                           unsigned int schedulerLagState,
                           unsigned int cadenceStreak,
                           unsigned int scheduledPendingEvents) override;
  void BeginRenderBlock() override;
  void EndRenderBlock() override;
  void ProcessMidiEvent(const MidiEvent &event) override;
  void Render(float *output, int numFrames) override;
  std::string GetResolvedSourcePath() const override;
  std::string GetResolvedSourceFormat() const override;
  std::string GetEngineName() const override;
  DWORD GetActiveVoiceStats(DWORD *channelCounts, int count) const override;
  SamplerDiagnostics GetDiagnostics() const override;

private:
  void ApplyDefaultChannelStateLocked(int channel);
  void ResetChannelsLocked();
  void UpdateVoiceStatsLocked();
  float MapMidiVelocity(int velocity) const;
  void EnsureVoiceScratchCapacityLocked(int voiceCapacity);
  void ClusterRenderVoicesLocked(int voiceCount, int numFrames);
  void ResetPerfCountersLocked();
  int PrepareRenderVoicesLocked(int voiceCount);

#ifndef SVMS_LEGACY_XP
  void EnsureWorkerBuffersCapacityLocked(int numFrames);
  void WorkerThread(int id);
#endif

  mutable compat::Mutex engineMutex;
  tsf *handle;
  int sampleRate;
  std::string resolvedSourcePath;
  std::string resolvedSourceFormat;
  RuntimeSettings runtimeSettings;
  DWORD activeVoiceCount;
  DWORD activeVoiceChannels[16];
  float lastVoiceStartMs;
  float lastSampleRenderMs;
  unsigned int lastNoteOnEvents;
  unsigned int lastNoteOffEvents;
  unsigned int realtimeOverloadState;
  unsigned int realtimeSchedulerLagState;
  unsigned int realtimeCadenceStreak;
  unsigned int realtimeScheduledPending;

  static const int kVoicesPerWorker = 256;
  static const int kHardVoiceRenderLimit = 50000;
  static const int kMinThreadedFrames = 96;
  static const int kMinThreadedWork = 196608;
  static const int kThreadRenderChunkFrames = 256;
  static const int kMinClusterVoices = 256;
  static const int kMinClusterWork = 32768;

  std::vector<int> renderVoiceIndices;
  std::vector<uint32_t> renderVoiceKeys;
  std::vector<int> renderVoiceTempIndices;
  std::vector<uint32_t> renderVoiceTempKeys;
  std::vector<unsigned char> renderVoiceHelperClass;
  std::vector<unsigned char> renderVoiceTempHelperClass;
  bool cleanupPending = false;
  unsigned int perfHelperContiguousBlocks = 0;
  unsigned int perfHelperGatherBlocks = 0;
  unsigned int perfHelperComplexBlocks = 0;
  unsigned int perfClusteredVoicesContiguous = 0;
  unsigned int perfClusteredVoicesGather = 0;
  unsigned int perfClusteredVoicesComplex = 0;
  unsigned int perfSingleThreadFragments = 0;
  unsigned int perfThreadedFragments = 0;

#ifndef SVMS_LEGACY_XP
  std::vector<std::thread> threads;
  compat::Mutex workerMutex;
  std::condition_variable workerCV;
  std::condition_variable masterCV;
  int numThreads;
  std::atomic<int> currentGeneration;
  std::atomic<bool> stopThreads;
  std::atomic<bool> shutdownRequested;
  int currentWorkerCount;
  int currentNumFrames;
  int currentRenderVoiceCount;
  std::vector<int> workerFinishedGen;

  struct WorkerData {
    std::vector<float> buffer;
  };
  std::vector<WorkerData> workerData;
#else
  std::atomic<bool> stopThreads;
  std::atomic<bool> shutdownRequested;
#endif
};

#endif
