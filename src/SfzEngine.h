#ifndef SFZ_ENGINE_H
#define SFZ_ENGINE_H

#include "Compat.h"
#include "SamplerEngine.h"
#include "WavLoader.h"
#include <set>
#include <string>
#include <vector>

class SfzEngine : public ISamplerEngine {
public:
  SfzEngine();
  ~SfzEngine() override;

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
  struct Impl;
  Impl *impl;
};

#endif
