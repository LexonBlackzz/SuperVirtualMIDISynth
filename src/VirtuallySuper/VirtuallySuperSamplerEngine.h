#ifndef VIRTUALLYSUPER_SAMPLER_ENGINE_H
#define VIRTUALLYSUPER_SAMPLER_ENGINE_H

#include "../Compat.h"
#include "../SamplerEngine.h"
#include "VirtuallySuperEngine.h"
#include "VirtuallySuperSoundFontRuntime.h"

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
  void ResetPerBlockStatsLocked();
  void SetStateLocked(SamplerRuntimeStateCode stateCode,
                      SamplerErrorCode errorCode, const char *warningText);
  void UpdateSoundFontDiagnosticsLocked();

  mutable compat::Mutex engineMutex;
  virtuallysuper::EnginePrototype prototype_;
  virtuallysuper::SoundFontRuntime soundFontRuntime_;
  bool initialized_;
  int sampleRate_;
  std::string resolvedSourcePath_;
  std::string resolvedSourceFormat_;
  RuntimeSettings runtimeSettings_;
  SamplerDiagnostics diagnostics_;
  SamplerRuntimeStateCode stateCode_;
  SamplerErrorCode errorCode_;
  unsigned int idleFastPathHits_;
  unsigned int noteOnEventsThisBlock_;
  unsigned int noteOffEventsThisBlock_;
  unsigned long long currentBlockStartSample_;
  int currentBlockFrames_;
  long long currentBlockStartQpc_;
  long long currentBlockEndQpc_;
  bool currentBlockQuantized_;
  bool hasRenderWindow_;
  long long renderCursorSample_;
};

#endif
