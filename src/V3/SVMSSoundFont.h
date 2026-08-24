#ifndef SVMS_SOUNDFONT_H
#define SVMS_SOUNDFONT_H

#include "SVMSTypes.h"
#include "SVMSConfig.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace svms {

struct SF2PresetZone {
    uint16_t generatorIndex;
    uint16_t modulatorIndex;
};

struct SF2InstrumentZone {
    uint16_t generatorIndex;
    uint16_t modulatorIndex;
};

struct SF2Generator {
    uint16_t genOper;
    uint16_t amount;
};

struct SF2Modulator {
    uint16_t srcOper;
    uint16_t destOper;
    int16_t amount;
    uint16_t amtSrcOper;
    uint16_t transformOper;
};

struct SF2Sample {
    char name[20];
    uint32_t start;
    uint32_t end;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint32_t sampleRate;
    uint8_t originalPitch;
    int8_t pitchCorrection;
    uint16_t sampleLink;
    uint16_t sampleType;
    uint32_t sampleDataOffset;
};

struct SFSampleRegion {
    uint16_t sampleIndex;
    uint16_t presetIndex;
    uint8_t keyLo, keyHi;
    uint8_t velLo, velHi;
    int8_t  rootKey;
    uint8_t loopMode;       // SF2 sampleModes: 0=none, 1=continuous, 2=reserved, 3=until release
    int16_t coarseTune;
    int16_t fineTune;
    int16_t scaleTuning;
    int16_t attackVolEnv;   // timecents
    int16_t decayVolEnv;    // timecents
    int16_t sustainVolEnv;  // 0-1000 (0 = 0dB)
    int16_t releaseVolEnv;  // timecents
    int16_t holdVolEnv;     // timecents
    int16_t delayVolEnv;    // timecents
    int16_t initialAttenuation; // centibels (0 = none)
    int32_t startOffset;    // sample-point offset
    int32_t endOffset;
    int32_t loopStartOffset;
    int32_t loopEndOffset;
    int32_t startCoarseOffset;
    int32_t endCoarseOffset;
    int16_t initialFilterFc;
    int16_t initialFilterQ;
    int16_t pan;
    int16_t reverbSend;
    int16_t chorusSend;
    int16_t modLfoToPitch;
    int16_t vibLfoToPitch;
    int16_t modEnvToPitch;
    int16_t modLfoToFilterFc;
    int16_t modEnvToFilterFc;
    int16_t modLfoToVolume;
    int16_t exclusiveClass;
};

enum SF2GeneratorType : uint16_t {
    Gen_StartAddrsOffset = 0,
    Gen_EndAddrsOffset = 1,
    Gen_StartLoopAddrsOffset = 2,
    Gen_EndLoopAddrsOffset = 3,
    Gen_StartAddrsCoarseOffset = 4,
    Gen_ModLfoToPitch = 5,
    Gen_VibLfoToPitch = 6,
    Gen_ModEnvToPitch = 7,
    Gen_InitialFilterFc = 8,
    Gen_InitialFilterQ = 9,
    Gen_ModLfoToFilterFc = 10,
    Gen_ModEnvToFilterFc = 11,
    Gen_EndAddrsCoarseOffset = 12,
    Gen_ModLfoToVolume = 13,
    Gen_Unused1 = 14,
    Gen_ChorusEffectsSend = 15,
    Gen_ReverbEffectsSend = 16,
    Gen_Pan = 17,
    Gen_Unused2 = 18,
    Gen_Unused3 = 19,
    Gen_Unused4 = 20,
    Gen_DelayModLFO = 21,
    Gen_FreqModLFO = 22,
    Gen_DelayVibLFO = 23,
    Gen_FreqVibLFO = 24,
    Gen_DelayModEnv = 25,
    Gen_AttackModEnv = 26,
    Gen_HoldModEnv = 27,
    Gen_DecayModEnv = 28,
    Gen_SustainModEnv = 29,
    Gen_ReleaseModEnv = 30,
    Gen_KeynumToModEnvHold = 31,
    Gen_KeynumToModEnvDecay = 32,
    Gen_DelayVolEnv = 33,
    Gen_AttackVolEnv = 34,
    Gen_HoldVolEnv = 35,
    Gen_DecayVolEnv = 36,
    Gen_SustainVolEnv = 37,
    Gen_ReleaseVolEnv = 38,
    Gen_KeynumToVolEnvHold = 39,
    Gen_KeynumToVolEnvDecay = 40,
    Gen_Instrument = 41,
    Gen_Reserved1 = 42,
    Gen_KeyRange = 43,
    Gen_VelRange = 44,
    Gen_StartLoopAddrsCoarseOffset = 45,
    Gen_Keynum = 46,
    Gen_Velocity = 47,
    Gen_InitialAttenuation = 48,
    Gen_Reserved2 = 49,
    Gen_EndLoopAddrsCoarseOffset = 50,
    Gen_CoarseTune = 51,
    Gen_FineTune = 52,
    Gen_SampleID = 53,
    Gen_SampleModes = 54,
    Gen_Reserved3 = 55,
    Gen_ScaleTuning = 56,
    Gen_ExclusiveClass = 57,
    Gen_OverridingRootKey = 58,
    Gen_Unused5 = 59,
    Gen_EndOper = 60
};

static constexpr uint16_t Gen_RootKey = 58;
// Bag and generator indices are 16-bit in SF2, and large General MIDI banks
// routinely use substantially more than the original conservative limits.
// Keep bounded, allocation-free metadata storage while covering the complete
// representable generator/index space and practical large-bank header counts.
static constexpr uint32_t kMaxPresets = 512;
static constexpr uint32_t kMaxInstruments = 4096;
static constexpr uint32_t kMaxSamples = 8192;
static constexpr uint32_t kMaxZones = 65536;
static constexpr uint32_t kMaxGenerators = 65536;

struct SF2PresetHeader {
    char name[20];
    uint16_t preset;
    uint16_t bank;
    uint16_t zoneIndex;
    uint16_t dummy;
    uint32_t library;
    uint32_t genre;
    uint32_t morphology;
};

struct SF2InstrumentHeader {
    char name[20];
    uint16_t zoneIndex;
};

struct SF2Data {
    SF2PresetHeader presets[kMaxPresets];
    uint32_t presetCount;

    SF2InstrumentHeader instruments[kMaxInstruments];
    uint32_t instrumentCount;

    SF2Sample samples[kMaxSamples];
    uint32_t sampleCount;

    SF2PresetZone presetZones[kMaxZones];
    uint32_t presetZoneCount;

    SF2InstrumentZone instrumentZones[kMaxZones];
    uint32_t instrumentZoneCount;

    SF2Generator generators[kMaxGenerators];
    uint32_t generatorCount;
    uint32_t pgenCount;

    static constexpr uint32_t kMaxCompiledRegions = 65536;
    SFSampleRegion regions[kMaxCompiledRegions];
    uint32_t regionCount;
    uint32_t presetRegionStart[kMaxPresets];
    uint32_t presetRegionCount[kMaxPresets];
    bool regionOverflow;

    int16_t* sampleData;
    uint32_t sampleDataSize;
    uint32_t sampleDataFrames;

    float* resampledData;
    uint32_t resampledDataSize;
    uint32_t resampledSampleRate;

    bool loaded;
};

struct InstrumentVoiceParams {
    uint32_t sampleStart;
    uint32_t sampleEnd;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint8_t loopMode;
    uint8_t rootKey;
    float initialAttenuation;
    float panLeft;
    float panRight;
    float filterCutoff;
    float filterResonance;
    float reverbSend;
    float chorusSend;
    EnvelopeParams volEnv;
    EnvelopeParams modEnv;
    float coarseTune;
    float fineTune;
    float scaleTuning;
    float dummy;
};

bool sf2_load(const char* path, SF2Data* outData);
bool sf2_load(const wchar_t* path, SF2Data* outData);
void sf2_free(SF2Data* data);
bool sf2_resample(SF2Data* data, uint32_t targetRate, InterpolationMode mode);

bool sf2_find_preset(const SF2Data* data, uint16_t bank, uint16_t preset,
                     uint32_t* outPresetIndex);
bool sf2_resolve_preset(const SF2Data* data, uint16_t bank, uint8_t program,
                        bool percussionChannel, uint32_t* outPresetIndex);
bool sf2_find_instrument(const SF2Data* data, uint32_t presetIndex,
                         uint8_t note, uint8_t velocity,
                         uint32_t* outInstrumentIndex, uint32_t* outSampleIndex);
bool sf2_build_voice_params(const SF2Data* data, uint32_t instrumentIndex,
                             uint32_t sampleIndex, uint8_t note, uint8_t velocity,
                             float sampleRate, InstrumentVoiceParams* outParams);
void sf2_build_regions(SF2Data* data);
uint32_t sf2_find_regions(const SF2Data* data, uint32_t presetIndex,
                          uint8_t note, uint8_t velocity,
                          const SFSampleRegion** outRegions,
                          uint32_t outCapacity);
bool sf2_validate_region(const SF2Data* data, const SFSampleRegion* region);
float sf2_region_initial_peak(const SF2Data* data, const SFSampleRegion* region,
                              uint32_t windowFrames = 512);

uint32_t sf2_read_u32(const uint8_t* buf);
uint16_t sf2_read_u16(const uint8_t* buf);
int16_t sf2_read_i16(const uint8_t* buf);

} // namespace svms

#endif
