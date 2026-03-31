#include "VirtuallySuperSoundFontDispatch.h"

namespace virtuallysuper {

int ResolveSoundFontPresetIndex(const SoundFontRuntimeData &runtimeData,
                                uint16_t bank, uint16_t program) {
  for (size_t i = 0; i < runtimeData.presets.size(); ++i) {
    const SoundFontPreset &preset = runtimeData.presets[i];
    if (preset.bank == bank && preset.preset == program)
      return (int)i;
  }

  for (size_t i = 0; i < runtimeData.presets.size(); ++i) {
    const SoundFontPreset &preset = runtimeData.presets[i];
    if (preset.bank == 0 && preset.preset == program)
      return (int)i;
  }

  return -1;
}

bool BuildSoundFontDispatch(SoundFontRuntimeData &runtimeData) {
  runtimeData.dispatchEntries.clear();

  for (size_t presetIndex = 0; presetIndex < runtimeData.presets.size();
       ++presetIndex) {
    SoundFontPreset &preset = runtimeData.presets[presetIndex];
    for (uint32_t note = 0; note < 128; ++note) {
      preset.dispatchOffset[note] = (uint32_t)runtimeData.dispatchEntries.size();
      preset.dispatchCount[note] = 0;

      const uint32_t regionBegin = preset.regionOffset;
      const uint32_t regionEnd = preset.regionOffset + preset.regionCount;
      for (uint32_t regionSlot = regionBegin; regionSlot < regionEnd;
           ++regionSlot) {
        const SoundFontRegion &region = runtimeData.regions[regionSlot];
        if (note < region.keyRange.low || note > region.keyRange.high)
          continue;

        SoundFontDispatchEntry entry;
        entry.regionIndex = (uint16_t)regionSlot;
        runtimeData.dispatchEntries.push_back(entry);
        ++preset.dispatchCount[note];
      }
    }
  }

  return true;
}

const SoundFontRegion *
ResolveSoundFontRegion(const SoundFontRuntimeData &runtimeData,
                       uint16_t presetIndex, uint8_t note, uint8_t velocity,
                       uint16_t *regionIndex) {
  if (regionIndex)
    *regionIndex = kInvalidSoundFontIndex;

  if (presetIndex >= runtimeData.presets.size() || note >= 128)
    return 0;

  const SoundFontPreset &preset = runtimeData.presets[presetIndex];
  const uint32_t offset = preset.dispatchOffset[note];
  const uint32_t count = preset.dispatchCount[note];
  for (uint32_t i = 0; i < count; ++i) {
    const SoundFontDispatchEntry &entry = runtimeData.dispatchEntries[offset + i];
    if (entry.regionIndex >= runtimeData.regions.size())
      continue;

    const SoundFontRegion &region = runtimeData.regions[entry.regionIndex];
    if (velocity < region.velocityRange.low ||
        velocity > region.velocityRange.high) {
      continue;
    }

    if (regionIndex)
      *regionIndex = entry.regionIndex;
    return &region;
  }

  return 0;
}

} // namespace virtuallysuper
