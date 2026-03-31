#ifndef VIRTUALLYSUPER_SOUNDFONT_TYPES_H
#define VIRTUALLYSUPER_SOUNDFONT_TYPES_H

#include "VirtuallySuperTypes.h"

#include <stdint.h>
#include <vector>

namespace virtuallysuper {

enum SoundFontLoopMode {
  SoundFontLoopNone = 0,
  SoundFontLoopContinuous = 1,
  SoundFontLoopSustain = 3
};

struct SoundFontRange {
  uint8_t low;
  uint8_t high;

  SoundFontRange() : low(0), high(127) {}
};

struct SoundFontSample {
  char name[21];
  uint32_t start;
  uint32_t end;
  uint32_t loopStart;
  uint32_t loopEnd;
  uint32_t sampleRate;
  uint8_t originalPitch;
  int8_t pitchCorrection;
  uint16_t sampleLink;
  uint16_t sampleType;
  uint16_t sampleFamilyId;
  uint16_t reserved;

  SoundFontSample()
      : start(0), end(0), loopStart(0), loopEnd(0), sampleRate(0),
        originalPitch(60), pitchCorrection(0), sampleLink(0), sampleType(0),
        sampleFamilyId(0), reserved(0) {
    name[0] = '\0';
  }
};

struct SoundFontRegion {
  SoundFontRange keyRange;
  SoundFontRange velocityRange;
  uint16_t sampleIndex;
  uint16_t sampleFamilyId;
  uint16_t exclusiveClass;
  uint8_t loopMode;
  uint8_t reserved0;
  uint32_t sampleStart;
  uint32_t sampleEnd;
  uint32_t loopStart;
  uint32_t loopEnd;
  uint32_t sampleRate;
  int16_t coarseTune;
  int16_t fineTune;
  int16_t rootKey;
  int16_t keyTrack;
  float attenuationDb;
  float pan;
  float attackSeconds;
  float releaseSeconds;

  SoundFontRegion()
      : sampleIndex(kInvalidSoundFontIndex),
        sampleFamilyId(kInvalidSoundFontIndex), exclusiveClass(0),
        loopMode(SoundFontLoopNone), reserved0(0), sampleStart(0),
        sampleEnd(0), loopStart(0), loopEnd(0), sampleRate(0), coarseTune(0),
        fineTune(0), rootKey(-1), keyTrack(100), attenuationDb(0.0f),
        pan(0.0f), attackSeconds(0.0f), releaseSeconds(0.03f) {}
};

struct SoundFontDispatchEntry {
  uint16_t regionIndex;
  uint16_t reserved;

  SoundFontDispatchEntry() : regionIndex(kInvalidSoundFontIndex), reserved(0) {}
};

struct SoundFontPreset {
  char name[21];
  uint16_t bank;
  uint16_t preset;
  uint32_t regionOffset;
  uint32_t regionCount;
  uint32_t dispatchOffset[128];
  uint16_t dispatchCount[128];

  SoundFontPreset() : bank(0), preset(0), regionOffset(0), regionCount(0) {
    name[0] = '\0';
    for (int i = 0; i < 128; ++i) {
      dispatchOffset[i] = 0;
      dispatchCount[i] = 0;
    }
  }
};

struct SoundFontRuntimeData {
  std::vector<float> sampleData;
  std::vector<SoundFontSample> samples;
  std::vector<SoundFontRegion> regions;
  std::vector<SoundFontPreset> presets;
  std::vector<SoundFontDispatchEntry> dispatchEntries;

  void Reset() {
    sampleData.clear();
    samples.clear();
    regions.clear();
    presets.clear();
    dispatchEntries.clear();
  }
};

struct SoundFontChannelState {
  uint16_t bankMsb;
  uint16_t bankLsb;
  uint16_t resolvedBank;
  uint16_t program;
  uint16_t presetIndex;
  uint16_t pitchWheel;
  float volume;
  float expression;
  float pan;
  float tuning;
  float pitchRange;
  uint8_t sustain;
  uint8_t reserved[3];

  SoundFontChannelState()
      : bankMsb(0), bankLsb(0), resolvedBank(0), program(0),
        presetIndex(kInvalidSoundFontIndex), pitchWheel(8192), volume(1.0f),
        expression(1.0f), pan(0.5f), tuning(0.0f), pitchRange(2.0f),
        sustain(0), reserved() {}
};

struct SoundFontNoteInfo {
  bool valid;
  uint16_t regionIndex;
  uint16_t sampleIndex;
  const float *sampleData;
  uint32_t sampleStart;
  uint32_t sampleEnd;
  uint32_t loopStart;
  uint32_t loopEnd;
  uint8_t loopMode;
  float phaseStep;
  float initialGain;
  float leftGain;
  float rightGain;
  float attackSeconds;
  float releaseDecay;

  SoundFontNoteInfo()
      : valid(false), regionIndex(kInvalidSoundFontIndex),
        sampleIndex(kInvalidSoundFontIndex), sampleData(0), sampleStart(0),
        sampleEnd(0), loopStart(0), loopEnd(0), loopMode(SoundFontLoopNone),
        phaseStep(0.0f), initialGain(0.0f), leftGain(0.0f), rightGain(0.0f),
        attackSeconds(0.0f), releaseDecay(0.0f) {}
};

} // namespace virtuallysuper

#endif
