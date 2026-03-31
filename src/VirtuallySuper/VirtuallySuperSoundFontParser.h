#ifndef VIRTUALLYSUPER_SOUNDFONT_PARSER_H
#define VIRTUALLYSUPER_SOUNDFONT_PARSER_H

#include "VirtuallySuperSoundFontTypes.h"

#include <string>

namespace virtuallysuper {

class SoundFontParser {
public:
  bool LoadFile(const char *path, SoundFontRuntimeData &runtimeData,
                std::string &errorText) const;
  bool LoadMemory(const void *data, size_t size, SoundFontRuntimeData &runtimeData,
                  std::string &errorText) const;
};

} // namespace virtuallysuper

#endif
