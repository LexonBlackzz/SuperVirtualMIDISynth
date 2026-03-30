#ifndef BASS_MIDI_ENGINE_H
#define BASS_MIDI_ENGINE_H

#include "SamplerEngine.h"
#include <memory>

class BassMidiEngine : public ISamplerEngine {
public:
  BassMidiEngine();
  ~BassMidiEngine() override;

  bool Initialize(const SamplerInitParams &params) override;
  void Shutdown(bool waitForThreads) override;
  void Reset() override;
  void ReloadRuntimeSettings(const RuntimeSettings &settings) override;
  void SetRealtimePressure(unsigned int overloadState,
                           unsigned int schedulerLagState,
                           unsigned int cadenceStreak,
                           unsigned int scheduledPendingEvents) override;
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
  std::unique_ptr<Impl> impl;
};

#endif
