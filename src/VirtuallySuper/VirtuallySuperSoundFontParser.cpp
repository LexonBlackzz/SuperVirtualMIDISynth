#include "VirtuallySuperSoundFontParser.h"

#include <algorithm>
#include <fstream>
#include <math.h>
#include <string.h>

namespace {

static uint16_t ReadU16(const uint8_t *data) {
  return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static int16_t ReadS16(const uint8_t *data) {
  return (int16_t)ReadU16(data);
}

static uint32_t ReadU32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool FourCCEquals(const uint8_t *data, const char *id) {
  return memcmp(data, id, 4) == 0;
}

static float ClampFloat(float value, float minValue, float maxValue) {
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

static float TimecentsToSeconds(int timecents, float defaultValue) {
  if (timecents <= -32768)
    return defaultValue;
  if (timecents <= -12000)
    return 0.0f;
  return powf(2.0f, (float)timecents / 1200.0f);
}

static uint32_t ClampSampleOffset(int64_t value, uint32_t minValue,
                                  uint32_t maxValue) {
  if (value < (int64_t)minValue)
    return minValue;
  if (value > (int64_t)maxValue)
    return maxValue;
  return (uint32_t)value;
}

struct HydraBag {
  uint16_t genIndex;
};

struct HydraGenerator {
  uint16_t oper;
  uint16_t amount;
};

struct HydraPresetHeader {
  char name[21];
  uint16_t preset;
  uint16_t bank;
  uint16_t bagIndex;
};

struct HydraInstrument {
  char name[21];
  uint16_t bagIndex;
};

struct HydraSampleHeader {
  virtuallysuper::SoundFontSample sample;
};

struct ZoneSettings {
  int32_t startOffset;
  int32_t endOffset;
  int32_t loopStartOffset;
  int32_t loopEndOffset;
  int32_t startCoarseOffset;
  int32_t endCoarseOffset;
  int32_t loopStartCoarseOffset;
  int32_t loopEndCoarseOffset;
  int16_t coarseTune;
  int16_t fineTune;
  int16_t keyTrack;
  int16_t rootKey;
  int16_t exclusiveClass;
  int16_t attenuation;
  int16_t pan;
  int16_t attackVolEnv;
  int16_t releaseVolEnv;
  uint16_t sampleIndex;
  uint16_t instrumentIndex;
  uint16_t sampleModes;
  virtuallysuper::SoundFontRange keyRange;
  virtuallysuper::SoundFontRange velocityRange;
  bool hasInstrument;
  bool hasSample;
  bool hasSampleModes;
  bool hasRootKey;
  bool hasKeyRange;
  bool hasVelocityRange;
  bool hasAttack;
  bool hasRelease;

  ZoneSettings()
      : startOffset(0), endOffset(0), loopStartOffset(0), loopEndOffset(0),
        startCoarseOffset(0), endCoarseOffset(0), loopStartCoarseOffset(0),
        loopEndCoarseOffset(0), coarseTune(0), fineTune(0), keyTrack(100),
        rootKey(-1), exclusiveClass(0), attenuation(0), pan(0),
        attackVolEnv(-32768), releaseVolEnv(-32768),
        sampleIndex(virtuallysuper::kInvalidSoundFontIndex),
        instrumentIndex(virtuallysuper::kInvalidSoundFontIndex),
        sampleModes(0), keyRange(), velocityRange(), hasInstrument(false),
        hasSample(false), hasSampleModes(false), hasRootKey(false),
        hasKeyRange(false), hasVelocityRange(false), hasAttack(false),
        hasRelease(false) {}
};

static void CopyName21(char *dest, const uint8_t *src) {
  memcpy(dest, src, 20);
  dest[20] = '\0';
  for (int i = 19; i >= 0; --i) {
    if (dest[i] == '\0' || dest[i] == ' ')
      dest[i] = '\0';
    else
      break;
  }
}

static virtuallysuper::SoundFontRange IntersectRange(
    const virtuallysuper::SoundFontRange &a,
    const virtuallysuper::SoundFontRange &b) {
  virtuallysuper::SoundFontRange out;
  out.low = a.low > b.low ? a.low : b.low;
  out.high = a.high < b.high ? a.high : b.high;
  if (out.low > out.high)
    out.low = out.high;
  return out;
}

static void ApplyGenerator(ZoneSettings &settings,
                           const HydraGenerator &generator) {
  const uint8_t lo = (uint8_t)(generator.amount & 0xFFu);
  const uint8_t hi = (uint8_t)((generator.amount >> 8) & 0xFFu);
  const int16_t amount = (int16_t)generator.amount;

  switch (generator.oper) {
  case 0:
    settings.startOffset += amount;
    break;
  case 1:
    settings.endOffset += amount;
    break;
  case 2:
    settings.loopStartOffset += amount;
    break;
  case 3:
    settings.loopEndOffset += amount;
    break;
  case 4:
    settings.startCoarseOffset += amount;
    break;
  case 12:
    settings.endCoarseOffset += amount;
    break;
  case 17:
    settings.pan = (int16_t)(settings.pan + amount);
    break;
  case 34:
    settings.attackVolEnv = amount;
    settings.hasAttack = true;
    break;
  case 38:
    settings.releaseVolEnv = amount;
    settings.hasRelease = true;
    break;
  case 41:
    settings.instrumentIndex = (uint16_t)generator.amount;
    settings.hasInstrument = true;
    break;
  case 43:
    settings.keyRange.low = lo;
    settings.keyRange.high = hi;
    settings.hasKeyRange = true;
    break;
  case 44:
    settings.velocityRange.low = lo;
    settings.velocityRange.high = hi;
    settings.hasVelocityRange = true;
    break;
  case 48:
    settings.attenuation = (int16_t)(settings.attenuation + amount);
    break;
  case 50:
    settings.loopEndCoarseOffset += amount;
    break;
  case 51:
    settings.coarseTune = (int16_t)(settings.coarseTune + amount);
    break;
  case 52:
    settings.fineTune = (int16_t)(settings.fineTune + amount);
    break;
  case 53:
    settings.sampleIndex = (uint16_t)generator.amount;
    settings.hasSample = true;
    break;
  case 54:
    settings.sampleModes = (uint16_t)generator.amount;
    settings.hasSampleModes = true;
    break;
  case 56:
    settings.keyTrack = amount;
    break;
  case 57:
    settings.exclusiveClass = amount;
    break;
  case 58:
    settings.rootKey = amount;
    settings.hasRootKey = true;
    break;
  default:
    break;
  }
}

static ZoneSettings ReadZoneSettings(const std::vector<HydraBag> &bags,
                                     const std::vector<HydraGenerator> &gens,
                                     uint32_t bagIndex, uint32_t nextBagIndex) {
  ZoneSettings settings;
  if (bagIndex >= bags.size())
    return settings;

  const uint32_t genBegin = bags[bagIndex].genIndex;
  const uint32_t genEnd =
      nextBagIndex < bags.size() ? bags[nextBagIndex].genIndex : (uint32_t)gens.size();
  for (uint32_t genIndex = genBegin;
       genIndex < genEnd && genIndex < gens.size(); ++genIndex) {
    ApplyGenerator(settings, gens[genIndex]);
  }
  return settings;
}

static void MergeSettings(ZoneSettings *dst, const ZoneSettings &src) {
  dst->startOffset += src.startOffset;
  dst->endOffset += src.endOffset;
  dst->loopStartOffset += src.loopStartOffset;
  dst->loopEndOffset += src.loopEndOffset;
  dst->startCoarseOffset += src.startCoarseOffset;
  dst->endCoarseOffset += src.endCoarseOffset;
  dst->loopStartCoarseOffset += src.loopStartCoarseOffset;
  dst->loopEndCoarseOffset += src.loopEndCoarseOffset;
  dst->coarseTune = (int16_t)(dst->coarseTune + src.coarseTune);
  dst->fineTune = (int16_t)(dst->fineTune + src.fineTune);
  dst->attenuation = (int16_t)(dst->attenuation + src.attenuation);
  dst->pan = (int16_t)(dst->pan + src.pan);
  dst->exclusiveClass = src.exclusiveClass != 0 ? src.exclusiveClass : dst->exclusiveClass;

  if (src.hasKeyRange) {
    dst->keyRange = dst->hasKeyRange ? IntersectRange(dst->keyRange, src.keyRange)
                                     : src.keyRange;
    dst->hasKeyRange = true;
  }
  if (src.hasVelocityRange) {
    dst->velocityRange =
        dst->hasVelocityRange ? IntersectRange(dst->velocityRange, src.velocityRange)
                              : src.velocityRange;
    dst->hasVelocityRange = true;
  }
  if (src.hasSampleModes) {
    dst->sampleModes = src.sampleModes;
    dst->hasSampleModes = true;
  }
  if (src.hasRootKey) {
    dst->rootKey = src.rootKey;
    dst->hasRootKey = true;
  }
  if (src.hasAttack) {
    dst->attackVolEnv = src.attackVolEnv;
    dst->hasAttack = true;
  }
  if (src.hasRelease) {
    dst->releaseVolEnv = src.releaseVolEnv;
    dst->hasRelease = true;
  }
  if (src.hasInstrument) {
    dst->instrumentIndex = src.instrumentIndex;
    dst->hasInstrument = true;
  }
  if (src.hasSample) {
    dst->sampleIndex = src.sampleIndex;
    dst->hasSample = true;
  }
  if (src.keyTrack != 100)
    dst->keyTrack = src.keyTrack;
}

static bool BuildRuntime(const std::vector<HydraPresetHeader> &presetHeaders,
                         const std::vector<HydraBag> &presetBags,
                         const std::vector<HydraGenerator> &presetGens,
                         const std::vector<HydraInstrument> &instruments,
                         const std::vector<HydraBag> &instrumentBags,
                         const std::vector<HydraGenerator> &instrumentGens,
                         const std::vector<HydraSampleHeader> &sampleHeaders,
                         virtuallysuper::SoundFontRuntimeData *runtimeData,
                         std::string *errorText) {
  if (!runtimeData)
    return false;

  runtimeData->presets.clear();
  runtimeData->regions.clear();
  runtimeData->dispatchEntries.clear();

  if (presetHeaders.size() < 2 || instruments.size() < 2 ||
      sampleHeaders.size() < 2) {
    if (errorText)
      *errorText = "SoundFont Hydra tables are incomplete.";
    return false;
  }

  runtimeData->samples.reserve(sampleHeaders.size() - 1u);
  for (size_t sampleIndex = 0; sampleIndex + 1 < sampleHeaders.size();
       ++sampleIndex) {
    virtuallysuper::SoundFontSample sample = sampleHeaders[sampleIndex].sample;
    sample.sampleFamilyId = (uint16_t)sampleIndex;
    runtimeData->samples.push_back(sample);
  }

  for (size_t presetIndex = 0; presetIndex + 1 < presetHeaders.size();
       ++presetIndex) {
    const HydraPresetHeader &presetHeader = presetHeaders[presetIndex];
    const uint32_t presetBagBegin = presetHeader.bagIndex;
    const uint32_t presetBagEnd = presetHeaders[presetIndex + 1].bagIndex;
    if (presetBagBegin >= presetBags.size() || presetBagEnd > presetBags.size() ||
        presetBagBegin >= presetBagEnd) {
      continue;
    }

    virtuallysuper::SoundFontPreset preset;
    memcpy(preset.name, presetHeader.name, sizeof(preset.name));
    preset.bank = presetHeader.bank;
    preset.preset = presetHeader.preset;
    preset.regionOffset = (uint32_t)runtimeData->regions.size();

    ZoneSettings presetGlobal;
    bool hasPresetGlobal = false;
    for (uint32_t zone = presetBagBegin; zone < presetBagEnd; ++zone) {
      ZoneSettings presetZone =
          ReadZoneSettings(presetBags, presetGens, zone, zone + 1u);
      if (!presetZone.hasInstrument) {
        if (zone == presetBagBegin) {
          presetGlobal = presetZone;
          hasPresetGlobal = true;
        }
        continue;
      }

      if (presetZone.instrumentIndex >= instruments.size() - 1u)
        continue;

      const uint32_t instBagBegin = instruments[presetZone.instrumentIndex].bagIndex;
      const uint32_t instBagEnd =
          instruments[presetZone.instrumentIndex + 1].bagIndex;
      if (instBagBegin >= instrumentBags.size() ||
          instBagEnd > instrumentBags.size() || instBagBegin >= instBagEnd) {
        continue;
      }

      ZoneSettings instGlobal;
      bool hasInstGlobal = false;
      for (uint32_t instZone = instBagBegin; instZone < instBagEnd; ++instZone) {
        ZoneSettings instrumentZone =
            ReadZoneSettings(instrumentBags, instrumentGens, instZone, instZone + 1u);
        if (!instrumentZone.hasSample) {
          if (instZone == instBagBegin) {
            instGlobal = instrumentZone;
            hasInstGlobal = true;
          }
          continue;
        }

        if (instrumentZone.sampleIndex >= runtimeData->samples.size())
          continue;

        ZoneSettings merged;
        if (hasPresetGlobal)
          MergeSettings(&merged, presetGlobal);
        MergeSettings(&merged, presetZone);
        if (hasInstGlobal)
          MergeSettings(&merged, instGlobal);
        MergeSettings(&merged, instrumentZone);

        if (!merged.hasSample || merged.sampleIndex >= runtimeData->samples.size())
          continue;

        const virtuallysuper::SoundFontSample &sample =
            runtimeData->samples[merged.sampleIndex];
        virtuallysuper::SoundFontRegion region;
        region.sampleIndex = merged.sampleIndex;
        region.sampleFamilyId = sample.sampleFamilyId;
        region.keyRange = merged.hasKeyRange ? merged.keyRange
                                             : virtuallysuper::SoundFontRange();
        region.velocityRange =
            merged.hasVelocityRange ? merged.velocityRange
                                    : virtuallysuper::SoundFontRange();
        region.exclusiveClass = (uint16_t)merged.exclusiveClass;
        region.loopMode = merged.hasSampleModes
                              ? (uint8_t)(merged.sampleModes & 0x3u)
                              : virtuallysuper::SoundFontLoopNone;
        const uint32_t sampleMax =
            runtimeData->sampleData.empty()
                ? sample.end
                : (uint32_t)(runtimeData->sampleData.size() - 1u);
        const int64_t startValue =
            (int64_t)sample.start + merged.startOffset +
            ((int64_t)merged.startCoarseOffset * 32768ll);
        const int64_t endValue =
            (int64_t)sample.end + merged.endOffset +
            ((int64_t)merged.endCoarseOffset * 32768ll);
        const int64_t loopStartValue =
            (int64_t)sample.loopStart + merged.loopStartOffset +
            ((int64_t)merged.loopStartCoarseOffset * 32768ll);
        const int64_t loopEndValue =
            (int64_t)sample.loopEnd + merged.loopEndOffset +
            ((int64_t)merged.loopEndCoarseOffset * 32768ll);
        region.sampleStart = ClampSampleOffset(startValue, 0u, sampleMax);
        region.sampleEnd = ClampSampleOffset(endValue, region.sampleStart, sampleMax);
        region.loopStart = ClampSampleOffset(loopStartValue, region.sampleStart,
                                             region.sampleEnd);
        region.loopEnd =
            ClampSampleOffset(loopEndValue, region.loopStart, region.sampleEnd);
        region.sampleRate = sample.sampleRate;
        region.coarseTune = merged.coarseTune;
        region.fineTune = merged.fineTune;
        region.rootKey = merged.hasRootKey ? merged.rootKey : -1;
        region.keyTrack = merged.keyTrack;
        region.attenuationDb = (float)merged.attenuation * 0.1f;
        region.pan = ClampFloat((float)merged.pan / 500.0f, -1.0f, 1.0f);
        region.attackSeconds =
            TimecentsToSeconds(merged.attackVolEnv, 0.0f);
        region.releaseSeconds =
            TimecentsToSeconds(merged.releaseVolEnv, 0.03f);

        if (region.sampleEnd <= region.sampleStart + 1u)
          continue;
        if (region.loopStart < region.sampleStart)
          region.loopStart = region.sampleStart;
        if (region.loopEnd > region.sampleEnd)
          region.loopEnd = region.sampleEnd;
        if (region.loopEnd <= region.loopStart + 1u)
          region.loopMode = virtuallysuper::SoundFontLoopNone;

        runtimeData->regions.push_back(region);
        ++preset.regionCount;
      }
    }

    if (preset.regionCount > 0)
      runtimeData->presets.push_back(preset);
  }

  if (runtimeData->presets.empty() || runtimeData->regions.empty() ||
      runtimeData->samples.empty()) {
    if (errorText)
      *errorText = "SoundFont did not contain usable preset, region, or sample data.";
    return false;
  }

  return true;
}

} // namespace

namespace virtuallysuper {

bool SoundFontParser::LoadFile(const char *path, SoundFontRuntimeData &runtimeData,
                               std::string &errorText) const {
  runtimeData.Reset();
  errorText.clear();

  if (!path || !path[0]) {
    errorText = "VirtuallySuper SF2 load failed: empty source path.";
    return false;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    errorText = "VirtuallySuper SF2 load failed: could not open the file.";
    return false;
  }

  file.seekg(0, std::ios::end);
  const std::streamoff length = file.tellg();
  if (length <= 12) {
    errorText = "VirtuallySuper SF2 load failed: file is too small.";
    return false;
  }
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> bytes((size_t)length);
  file.read(reinterpret_cast<char *>(bytes.data()), length);
  if (!file) {
    errorText = "VirtuallySuper SF2 load failed: could not read the file.";
    return false;
  }

  return LoadMemory(bytes.data(), bytes.size(), runtimeData, errorText);
}

bool SoundFontParser::LoadMemory(const void *data, size_t size,
                                 SoundFontRuntimeData &runtimeData,
                                 std::string &errorText) const {
  runtimeData.Reset();
  errorText.clear();

  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  if (!bytes || size < 12) {
    errorText = "VirtuallySuper SF2 parse failed: input is empty.";
    return false;
  }

  if (!FourCCEquals(bytes, "RIFF") || !FourCCEquals(bytes + 8, "sfbk")) {
    errorText = "VirtuallySuper SF2 parse failed: RIFF sfbk header is missing.";
    return false;
  }

  const uint8_t *sdtaSmpl = 0;
  size_t sdtaSmplBytes = 0;
  std::vector<HydraPresetHeader> presets;
  std::vector<HydraBag> presetBags;
  std::vector<HydraGenerator> presetGens;
  std::vector<HydraInstrument> instruments;
  std::vector<HydraBag> instrumentBags;
  std::vector<HydraGenerator> instrumentGens;
  std::vector<HydraSampleHeader> samples;

  size_t offset = 12;
  while (offset + 8 <= size) {
    const uint8_t *chunk = bytes + offset;
    const uint32_t chunkSize = ReadU32(chunk + 4);
    const size_t chunkDataOffset = offset + 8;
    const size_t paddedChunkSize = (size_t)chunkSize + ((chunkSize & 1u) ? 1u : 0u);
    if (chunkDataOffset + chunkSize > size)
      break;

    if (FourCCEquals(chunk, "LIST") && chunkSize >= 4) {
      const uint8_t *listData = bytes + chunkDataOffset;
      const size_t listDataSize = chunkSize - 4;
      const uint8_t *listCursor = listData + 4;
      size_t listOffset = 0;

      if (FourCCEquals(listData, "sdta")) {
        while (listOffset + 8 <= listDataSize) {
          const uint8_t *subChunk = listCursor + listOffset;
          const uint32_t subSize = ReadU32(subChunk + 4);
          const size_t subDataOffset = listOffset + 8;
          const size_t subPadded = (size_t)subSize + ((subSize & 1u) ? 1u : 0u);
          if (subDataOffset + subSize > listDataSize)
            break;
          if (FourCCEquals(subChunk, "smpl")) {
            sdtaSmpl = subChunk + 8;
            sdtaSmplBytes = subSize;
          }
          listOffset = subDataOffset + subPadded;
        }
      } else if (FourCCEquals(listData, "pdta")) {
        while (listOffset + 8 <= listDataSize) {
          const uint8_t *subChunk = listCursor + listOffset;
          const uint32_t subSize = ReadU32(subChunk + 4);
          const size_t subDataOffset = listOffset + 8;
          const size_t subPadded = (size_t)subSize + ((subSize & 1u) ? 1u : 0u);
          if (subDataOffset + subSize > listDataSize)
            break;
          const uint8_t *subData = subChunk + 8;

          if (FourCCEquals(subChunk, "phdr") && subSize % 38u == 0) {
            const uint32_t count = subSize / 38u;
            presets.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
              HydraPresetHeader header = {};
              const uint8_t *record = subData + i * 38u;
              CopyName21(header.name, record);
              header.preset = ReadU16(record + 20);
              header.bank = ReadU16(record + 22);
              header.bagIndex = ReadU16(record + 24);
              presets.push_back(header);
            }
          } else if (FourCCEquals(subChunk, "pbag") && subSize % 4u == 0) {
            const uint32_t count = subSize / 4u;
            presetBags.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
              HydraBag bag = {};
              const uint8_t *record = subData + i * 4u;
              bag.genIndex = ReadU16(record);
              presetBags.push_back(bag);
            }
          } else if (FourCCEquals(subChunk, "pgen") && subSize % 4u == 0) {
            const uint32_t count = subSize / 4u;
            presetGens.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
              HydraGenerator gen = {};
              const uint8_t *record = subData + i * 4u;
              gen.oper = ReadU16(record);
              gen.amount = ReadU16(record + 2);
              presetGens.push_back(gen);
            }
          } else if (FourCCEquals(subChunk, "inst") && subSize % 22u == 0) {
            const uint32_t count = subSize / 22u;
            instruments.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
              HydraInstrument inst = {};
              const uint8_t *record = subData + i * 22u;
              CopyName21(inst.name, record);
              inst.bagIndex = ReadU16(record + 20);
              instruments.push_back(inst);
            }
          } else if (FourCCEquals(subChunk, "ibag") && subSize % 4u == 0) {
            const uint32_t count = subSize / 4u;
            instrumentBags.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
              HydraBag bag = {};
              const uint8_t *record = subData + i * 4u;
              bag.genIndex = ReadU16(record);
              instrumentBags.push_back(bag);
            }
          } else if (FourCCEquals(subChunk, "igen") && subSize % 4u == 0) {
            const uint32_t count = subSize / 4u;
            instrumentGens.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
              HydraGenerator gen = {};
              const uint8_t *record = subData + i * 4u;
              gen.oper = ReadU16(record);
              gen.amount = ReadU16(record + 2);
              instrumentGens.push_back(gen);
            }
          } else if (FourCCEquals(subChunk, "shdr") && subSize % 46u == 0) {
            const uint32_t count = subSize / 46u;
            samples.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
              HydraSampleHeader header = {};
              const uint8_t *record = subData + i * 46u;
              CopyName21(header.sample.name, record);
              header.sample.start = ReadU32(record + 20);
              header.sample.end = ReadU32(record + 24);
              header.sample.loopStart = ReadU32(record + 28);
              header.sample.loopEnd = ReadU32(record + 32);
              header.sample.sampleRate = ReadU32(record + 36);
              header.sample.originalPitch = record[40];
              header.sample.pitchCorrection = (int8_t)record[41];
              header.sample.sampleLink = ReadU16(record + 42);
              header.sample.sampleType = ReadU16(record + 44);
              samples.push_back(header);
            }
          }

          listOffset = subDataOffset + subPadded;
        }
      }
    }

    offset = chunkDataOffset + paddedChunkSize;
  }

  if (!sdtaSmpl || sdtaSmplBytes < 4u) {
    errorText = "VirtuallySuper SF2 parse failed: sdta/smpl chunk is missing.";
    return false;
  }

  if (presets.empty() || presetBags.empty() || presetGens.empty() ||
      instruments.empty() || instrumentBags.empty() || instrumentGens.empty() ||
      samples.empty()) {
    errorText = "VirtuallySuper SF2 parse failed: pdta Hydra tables are missing required chunks.";
    return false;
  }

  const size_t sampleCount = sdtaSmplBytes / sizeof(int16_t);
  runtimeData.sampleData.resize(sampleCount);
  for (size_t i = 0; i < sampleCount; ++i) {
    const int16_t sampleValue = ReadS16(sdtaSmpl + i * sizeof(int16_t));
    runtimeData.sampleData[i] = (float)sampleValue * (1.0f / 32768.0f);
  }

  if (!BuildRuntime(presets, presetBags, presetGens, instruments, instrumentBags,
                    instrumentGens, samples, &runtimeData, &errorText)) {
    runtimeData.Reset();
    return false;
  }

  return true;
}

} // namespace virtuallysuper
