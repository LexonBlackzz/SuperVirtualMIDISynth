#ifndef VIRTUALLYSUPER_SAMPLER_ENGINE_H
#define VIRTUALLYSUPER_SAMPLER_ENGINE_H

#include "../Compat.h"
#include "../SamplerEngine.h"
#include "VirtuallySuperEngine.h"
#include "VirtuallySuperSoundFontRuntime.h"

#include <atomic>
#include <string>

class VirtuallySuperSamplerEngine : public ISamplerEngine {
public:
  VirtuallySuperSamplerEngine();
  ~VirtuallySuperSamplerEngine() override;

  bool Initialize(const SamplerInitParams &params) override;
  void Shutdown(bool waitForThreads) override;
  void Reset() override;
  void ReloadRuntimeSettings(const RuntimeSettings &settings) override;
  void SetRenderWindow(unsigned long long blockStartSample, int blockFrames,
                       int sampleRate, long long blockStartQpc,
                       long long blockEndQpc,
                       bool quantizedByPollingRate) override;
  void BeginRenderBlock() override;
  void ProcessMidiEvent(const MidiEvent &event) override;
  void Render(float *output, int numFrames) override;
  std::string GetResolvedSourcePath() const override;
  std::string GetResolvedSourceFormat() const override;
  std::string GetEngineName() const override;
  DWORD GetActiveVoiceStats(DWORD *channelCounts, int count) const override;
  SamplerDiagnostics GetDiagnostics() const override;

private:
  uint8_t MapMidiVelocity(int velocity) const;
  virtuallysuper::NormalizedEvent ConvertMidiEvent(const MidiEvent &event) const;
  void ResetPerBlockStats();
  void SetState(SamplerRuntimeStateCode stateCode,
                      SamplerErrorCode errorCode, const char *warningText);
  void UpdateSoundFontDiagnostics();

  // Lock-free render window parameters (atomic for cross-thread access)
  struct RenderWindowParams {
    std::atomic<unsigned long long> blockStartSample{0};
    std::atomic<int> blockFrames{0};
    std::atomic<int> sampleRate{0};
    std::atomic<long long> blockStartQpc{0};
    std::atomic<long long> blockEndQpc{0};
    std::atomic<bool> quantized{false};
    std::atomic<bool> valid{false};
  };
  RenderWindowParams renderWindow_;

  // Atomic state variables for lock-free access from audio thread
  std::atomic<SamplerRuntimeStateCode> stateCode_{SamplerRuntimeStateCode::UNINITIALIZED};
  std::atomic<SamplerErrorCode> errorCode_{SamplerErrorCode::NONE};
  std::atomic<unsigned int> idleFastPathHits_{0};
  std::atomic<unsigned int> noteOnEventsThisBlock_{0};
  std::atomic<unsigned int> noteOffEventsThisBlock_{0};
  std::atomic<long long> renderCursorSample_{0};

  // Protected by mutex (UI thread only access)
  mutable compat::Mutex engineMutex;
  virtuallysuper::EnginePrototype prototype_;
  virtuallysuper::SoundFontRuntime soundFontRuntime_;
  bool initialized_;
  int sampleRate_;
  std::string resolvedSourcePath_;
  std::string resolvedSourceFormat_;
  RuntimeSettings runtimeSettings_;
  SamplerDiagnostics diagnostics_;
  SamplerErrorCode errorCodeUI_;  // Non-atomic for UI thread
};

#endif
