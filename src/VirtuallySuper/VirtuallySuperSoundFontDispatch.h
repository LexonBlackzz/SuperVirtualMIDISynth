#ifndef VIRTUALLYSUPER_SOUNDFONT_DISPATCH_H
#define VIRTUALLYSUPER_SOUNDFONT_DISPATCH_H

#include "VirtuallySuperSoundFontTypes.h"

namespace virtuallysuper {

int ResolveSoundFontPresetIndex(const SoundFontRuntimeData &runtimeData,
                                uint16_t bank, uint16_t program);
bool BuildSoundFontDispatch(SoundFontRuntimeData &runtimeData);
const SoundFontRegion *ResolveSoundFontRegion(const SoundFontRuntimeData &runtimeData,
                                              uint16_t presetIndex, uint8_t note,
                                              uint8_t velocity,
                                              uint16_t *regionIndex);

} // namespace virtuallysuper

#endif
