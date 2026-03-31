#ifndef VIRTUALLYSUPER_SAMPLER_ENGINE_H
#define VIRTUALLYSUPER_SAMPLER_ENGINE_H

#include "../Compat.h"
#include "../SamplerEngine.h"
#include "VirtuallySuperEngine.h"

#include <string>

class VirtuallySuperSamplerEngine : public ISamplerEngine {
public:
  VirtuallySuperSamplerEngine();
  ~VirtuallySuperSamplerEngine() override;

  bool Initialize(const SamplerInitParams &params) override;
  void Shutdown(bool waitForThreads) override;
  void Reset() override;
  void ReloadRuntimeSettings(const RuntimeSettings &settings) override;
  void BeginRenderBlock() override;
  void ProcessMidiEvent(const MidiEvent &event) override;
  void Render(float *output, int numFrames) override;
  std::string GetResolvedSourcePath() const override;
  std::string GetResolvedSourceFormat() const override;
  std::string GetEngineName() const override;
  DWORD GetActiveVoiceStats(DWORD *channelCounts, int count) const override;
  SamplerDiagnostics GetDiagnostics() const override;

private:
  virtuallysuper::NormalizedEvent ConvertMidiEvent(const MidiEvent &event) const;
  void ResetPerBlockStatsLocked();

  mutable compat::Mutex engineMutex;
  virtuallysuper::EnginePrototype prototype_;
  bool initialized_;
  int sampleRate_;
  std::string resolvedSourcePath_;
  std::string resolvedSourceFormat_;
  RuntimeSettings runtimeSettings_;
  SamplerDiagnostics diagnostics_;
  unsigned int noteOnEventsThisBlock_;
  unsigned int noteOffEventsThisBlock_;
};

#endif
