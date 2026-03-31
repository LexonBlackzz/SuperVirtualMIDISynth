#ifndef VIRTUALLYSUPER_SOUNDFONT_RUNTIME_H
#define VIRTUALLYSUPER_SOUNDFONT_RUNTIME_H

#include "VirtuallySuperSoundFontTypes.h"
#include "VirtuallySuperTypes.h"

#include <string>

namespace virtuallysuper {

class SoundFontRuntime {
public:
  SoundFontRuntime();

  bool Load(const char *path, uint32_t outputSampleRate, std::string *warningText);
  void Reset();
  void ResetChannels();

  bool IsLoaded() const;
  bool HandleEvent(const NormalizedEvent &event);
  bool PrepareNoteOn(const NormalizedEvent &event, SoundFontNoteInfo *info) const;
  bool RefreshVoiceInfo(uint8_t channel, uint8_t note, uint8_t velocity,
                        uint8_t mappedVelocity, uint16_t regionIndex,
                        SoundFontNoteInfo *info) const;

  uint32_t GetPresetCount() const;
  uint32_t GetRegionCount() const;
  uint32_t GetSampleCount() const;
  bool IsSustainEnabled(uint8_t channel) const;
  float GetPitchBendRange(uint8_t channel) const;

private:
  void ResetChannel(uint8_t channel);
  void ResolveProgram(uint8_t channel);
  bool FillNoteInfo(uint8_t channel, uint8_t note, uint8_t velocity,
                    uint8_t mappedVelocity, const SoundFontRegion &region,
                    uint16_t regionIndex, SoundFontNoteInfo *info) const;

  SoundFontRuntimeData runtimeData_;
  SoundFontChannelState channels_[16];
  uint32_t outputSampleRate_;
};

} // namespace virtuallysuper

#endif
