#include "SVMSSoundFont.h"
#include <windows.h>
#include <cstdio>

namespace svms {

static uint32_t read_u32(const uint8_t* buf) {
    return static_cast<uint32_t>(buf[0]) |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
}

static uint16_t read_u16(const uint8_t* buf) {
    return static_cast<uint16_t>(buf[0]) |
           (static_cast<uint16_t>(buf[1]) << 8);
}

static int16_t read_i16(const uint8_t* buf) {
    return static_cast<int16_t>(read_u16(buf));
}

uint32_t sf2_read_u32(const uint8_t* buf) { return read_u32(buf); }
uint16_t sf2_read_u16(const uint8_t* buf) { return read_u16(buf); }
int16_t sf2_read_i16(const uint8_t* buf) { return read_i16(buf); }

static bool read_chunk_header(FILE* f, uint32_t* outId, uint32_t* outSize) {
    uint8_t header[8];
    if (fread(header, 1, 8, f) != 8) return false;
    *outId = read_u32(header);
    *outSize = read_u32(header + 4);
    return true;
}

static uint32_t seek_pad(uint32_t size) {
    return (size + 1) & ~1u;
}

bool sf2_load(const char* path, SF2Data* outData) {
    std::memset(outData, 0, sizeof(SF2Data));

    char dbg[256];
    FILE* f = fopen(path, "rb");
    if (!f) {
        sprintf(dbg, "[SVMS-SF2] fopen FAILED: \"%s\"\n", path);
        OutputDebugStringA(dbg);
        return false;
    }

    uint8_t riffHeader[12];
    if (fread(riffHeader, 1, 12, f) != 12) {
        sprintf(dbg, "[SVMS-SF2] failed to read RIFF header\n");
        OutputDebugStringA(dbg);
        fclose(f); return false;
    }

    if (read_u32(riffHeader) != 0x46464952) {  // "RIFF"
        sprintf(dbg, "[SVMS-SF2] not a RIFF file, tag=0x%08X\n", read_u32(riffHeader));
        OutputDebugStringA(dbg);
        fclose(f); return false;
    }
    if (read_u32(riffHeader + 8) != 0x6B626673) {  // "sfbk"
        sprintf(dbg, "[SVMS-SF2] not an SF2 file (sfbk missing), tag=0x%08X\n", read_u32(riffHeader + 8));
        OutputDebugStringA(dbg);
        fclose(f); return false;
    }

    sprintf(dbg, "[SVMS-SF2] RIFF sfbk found, size=%u\n", read_u32(riffHeader + 4));
    OutputDebugStringA(dbg);

    int chunkNum = 0;
    while (true) {
        uint32_t chunkId, chunkSize;
        if (!read_chunk_header(f, &chunkId, &chunkSize)) {
            sprintf(dbg, "[SVMS-SF2] end of chunks after %d\n", chunkNum);
            OutputDebugStringA(dbg);
            break;
        }
        chunkNum++;

        if (chunkId == 0x5453494C) {  // "LIST"
            uint8_t listTypeBuf[4];
            if (fread(listTypeBuf, 1, 4, f) != 4) break;
            uint32_t listType = read_u32(listTypeBuf);
            uint32_t listEnd = static_cast<uint32_t>(ftell(f)) + chunkSize - 4;

            if (listType == 0x61746470) {  // "pdta"
                uint32_t pdtaPos = static_cast<uint32_t>(ftell(f));
                uint32_t pdtaEnd = listEnd;
                sprintf(dbg, "[SVMS-SF2]  parsing pdta from %u to %u\n", pdtaPos, pdtaEnd);
                OutputDebugStringA(dbg);

                while (static_cast<uint32_t>(ftell(f)) < pdtaEnd) {
                    uint32_t subId, subSize;
                    if (!read_chunk_header(f, &subId, &subSize)) break;
                    uint32_t subEnd = static_cast<uint32_t>(ftell(f)) + subSize;

                    if (subId == 0x72646870) {  // "phdr"
                        sprintf(dbg, "[SVMS-SF2]    phdr: size=%u count=%u\n", subSize, subSize/38);
                        OutputDebugStringA(dbg);
                        uint32_t count = subSize / 38;
                        if (count > kMaxPresets) count = kMaxPresets;
                        for (uint32_t i = 0; i < count; ++i) {
                            uint8_t buf[38];
                            if (fread(buf, 1, 38, f) != 38) break;
                            std::memcpy(outData->presets[i].name, buf, 20);
                            outData->presets[i].name[19] = 0;
                            outData->presets[i].preset = read_u16(buf + 20);
                            outData->presets[i].bank = read_u16(buf + 22);
                            outData->presets[i].zoneIndex = read_u16(buf + 24);
                        }
                        outData->presetCount = count;
                    } else if (subId == 0x67616270) {  // "pbag"
                        uint32_t count = subSize / 4;
                        if (count > kMaxZones) count = kMaxZones;
                        for (uint32_t i = 0; i < count; ++i) {
                            uint8_t buf[4];
                            if (fread(buf, 1, 4, f) != 4) break;
                            outData->presetZones[i].generatorIndex = read_u16(buf);
                            outData->presetZones[i].modulatorIndex = read_u16(buf + 2);
                        }
                        outData->presetZoneCount = count;
                    } else if (subId == 0x646F6D70) {  // "pmod"
                        fseek(f, seek_pad(subSize), 1);
                    } else if (subId == 0x6E656770) {  // "pgen"
                        uint32_t count = subSize / 4;
                        if (count > kMaxGenerators) count = kMaxGenerators;
                        for (uint32_t i = 0; i < count; ++i) {
                            uint8_t buf[4];
                            if (fread(buf, 1, 4, f) != 4) break;
                            outData->generators[i].genOper = read_u16(buf);
                            outData->generators[i].amount = read_u16(buf + 2);
                        }
                        outData->generatorCount = count;
                        outData->pgenCount = count;
                    } else if (subId == 0x74736E69) {  // "inst"
                        uint32_t count = subSize / 22;
                        if (count > kMaxInstruments) count = kMaxInstruments;
                        for (uint32_t i = 0; i < count; ++i) {
                            uint8_t buf[22];
                            if (fread(buf, 1, 22, f) != 22) break;
                            std::memcpy(outData->instruments[i].name, buf, 20);
                            outData->instruments[i].name[19] = 0;
                            outData->instruments[i].zoneIndex = read_u16(buf + 20);
                        }
                        outData->instrumentCount = count;
                    } else if (subId == 0x67616269) {  // "ibag"
                        uint32_t count = subSize / 4;
                        if (count + outData->presetZoneCount > kMaxZones)
                            count = kMaxZones - outData->presetZoneCount;
                        uint16_t igenBase = static_cast<uint16_t>(outData->generatorCount);
                        for (uint32_t i = 0; i < count; ++i) {
                            uint8_t buf[4];
                            if (fread(buf, 1, 4, f) != 4) break;
                            uint32_t idx = outData->instrumentZoneCount;
                            outData->instrumentZones[idx].generatorIndex = read_u16(buf) + igenBase;
                            outData->instrumentZones[idx].modulatorIndex = read_u16(buf + 2);
                            outData->instrumentZoneCount++;
                        }
                    } else if (subId == 0x6F646D69) {  // "imod"
                        fseek(f, seek_pad(subSize), 1);
                    } else if (subId == 0x6E656769) {  // "igen"
                        uint32_t count = subSize / 4;
                        uint32_t startIdx = outData->generatorCount;
                        if (startIdx + count > kMaxGenerators)
                            count = kMaxGenerators - startIdx;
                        for (uint32_t i = 0; i < count; ++i) {
                            uint8_t buf[4];
                            if (fread(buf, 1, 4, f) != 4) break;
                            outData->generators[startIdx + i].genOper = read_u16(buf);
                            outData->generators[startIdx + i].amount = read_u16(buf + 2);
                        }
                        outData->generatorCount = startIdx + count;
                    } else if (subId == 0x72646873) {  // "shdr"
                        uint32_t count = subSize / 46;
                        if (count > kMaxSamples) count = kMaxSamples;
                        for (uint32_t i = 0; i < count; ++i) {
                            uint8_t buf[46];
                            if (fread(buf, 1, 46, f) != 46) break;
                            SF2Sample& s = outData->samples[i];
                            std::memcpy(s.name, buf, 20);
                            s.name[19] = 0;
                            s.start = read_u32(buf + 20);
                            s.end = read_u32(buf + 24);
                            s.loopStart = read_u32(buf + 28);
                            s.loopEnd = read_u32(buf + 32);
                            s.sampleRate = read_u32(buf + 36);
                            s.originalPitch = buf[40];
                            s.pitchCorrection = static_cast<int8_t>(buf[41]);
                            s.sampleLink = read_u16(buf + 42);
                            s.sampleType = read_u16(buf + 44);
                        }
                        outData->sampleCount = count;
                    } else {
                        fseek(f, seek_pad(subSize), 1);
                        fseek(f, subEnd, 0);
                    }
                }
            } else if (listType == 0x61746473) {  // "sdta"
                uint32_t sdtaPos = static_cast<uint32_t>(ftell(f));
                uint32_t sdtaEnd = listEnd;
                sprintf(dbg, "[SVMS-SF2]  parsing sdta from %u to %u\n", sdtaPos, sdtaEnd);
                OutputDebugStringA(dbg);

                while (static_cast<uint32_t>(ftell(f)) < sdtaEnd) {
                    uint32_t subId, subSize;
                    if (!read_chunk_header(f, &subId, &subSize)) break;

                    if (subId == 0x6C706D73) {  // "smpl"
                        sprintf(dbg, "[SVMS-SF2]    smpl: size=%u frames=%u\n", subSize, subSize/2);
                        OutputDebugStringA(dbg);
                        uint32_t frameCount = subSize / 2;
                        int16_t* buf = static_cast<int16_t*>(malloc(subSize));
                        if (buf && fread(buf, 1, subSize, f) == subSize) {
                            outData->sampleData = buf;
                            outData->sampleDataSize = subSize;
                            outData->sampleDataFrames = frameCount;
                            sprintf(dbg, "[SVMS-SF2]    smpl read OK: %u bytes %u frames\n", subSize, frameCount);
                            OutputDebugStringA(dbg);
                        } else {
                            sprintf(dbg, "[SVMS-SF2]    smpl read FAILED: buf=%p subSize=%u\n", buf, subSize);
                            OutputDebugStringA(dbg);
                            free(buf);
                        }
                    } else {
                        fseek(f, seek_pad(subSize), 1);
                    }
                }
            } else {
                sprintf(dbg, "[SVMS-SF2]  skipping LIST type=0x%08X size=%u\n", listType, chunkSize);
                OutputDebugStringA(dbg);
                fseek(f, chunkSize - 4, 1);
            }
        } else {
            sprintf(dbg, "[SVMS-SF2]  skipping chunk id=0x%08X size=%u\n", chunkId, chunkSize);
            OutputDebugStringA(dbg);
            fseek(f, seek_pad(chunkSize), 1);
        }
    }

    fclose(f);
    outData->loaded = outData->sampleData != nullptr && outData->presetCount > 0;
    sprintf(dbg, "[SVMS-SF2] load complete: loaded=%d presets=%u inst=%u samples=%u frames=%u\n",
            outData->loaded ? 1 : 0, outData->presetCount, outData->instrumentCount,
            outData->sampleCount, outData->sampleDataFrames);
    OutputDebugStringA(dbg);
    return outData->loaded;
}

void sf2_free(SF2Data* data) {
    free(data->sampleData);
    free(data->resampledData);
    std::memset(data, 0, sizeof(SF2Data));
}

bool sf2_resample(SF2Data* data, uint32_t targetRate, InterpolationMode mode) {
    if (!data->loaded || !data->sampleData) return false;

    uint32_t srcRate = 0;
    for (uint32_t i = 0; i < data->sampleCount; ++i) {
        if (data->samples[i].sampleRate > srcRate)
            srcRate = data->samples[i].sampleRate;
    }
    if (srcRate == 0) srcRate = 44100;
    if (srcRate == targetRate) return true;

    double ratio = static_cast<double>(targetRate) / srcRate;
    uint32_t dstFrames = static_cast<uint32_t>(data->sampleDataFrames * ratio) + 256;

    float* dst = static_cast<float*>(malloc(dstFrames * sizeof(float)));
    if (!dst) return false;

    if (mode == InterpolationMode::Nearest) {
        for (uint32_t i = 0; i < dstFrames; ++i) {
            uint32_t srcIdx = static_cast<uint32_t>(i / ratio);
            if (srcIdx >= data->sampleDataFrames) srcIdx = data->sampleDataFrames - 1;
            dst[i] = data->sampleData[srcIdx] / 32768.0f;
        }
    } else {
        for (uint32_t i = 0; i < dstFrames; ++i) {
            double srcPos = i / ratio;
            uint32_t idx0 = static_cast<uint32_t>(srcPos);
            uint32_t idx1 = idx0 + 1;
            if (idx1 >= data->sampleDataFrames) idx1 = data->sampleDataFrames - 1;
            float frac = static_cast<float>(srcPos - idx0);
            float s0 = data->sampleData[idx0] / 32768.0f;
            float s1 = data->sampleData[idx1] / 32768.0f;
            dst[i] = s0 + (s1 - s0) * frac;
        }
    }

    data->resampledData = dst;
    data->resampledDataSize = dstFrames;
    data->resampledSampleRate = targetRate;

    float rateRatio = static_cast<float>(targetRate) / srcRate;
    for (uint32_t i = 0; i < data->sampleCount; ++i) {
        SF2Sample& s = data->samples[i];
        s.sampleDataOffset = static_cast<uint32_t>(s.start * rateRatio);
    }

    return true;
}

static int16_t clamp_gen_value(SF2GeneratorType gen, int16_t amount) {
    switch (gen) {
        case Gen_KeyRange:
        case Gen_VelRange:
            return amount;
        case Gen_Pan:
            return amount > 500 ? 500 : (amount < -500 ? -500 : amount);
        case Gen_InitialAttenuation:
            return amount;
        default:
            return amount;
    }
}

bool sf2_find_preset(const SF2Data* data, uint16_t bank, uint16_t preset,
                     uint32_t* outPresetIndex) {
    for (uint32_t i = 0; i < data->presetCount; ++i) {
        if (data->presets[i].bank == bank && data->presets[i].preset == preset) {
            *outPresetIndex = i;
            return true;
        }
    }
    for (uint32_t i = 0; i < data->presetCount; ++i) {
        if (data->presets[i].preset == preset) {
            *outPresetIndex = i;
            return true;
        }
    }
    return false;
}

bool sf2_build_voice_params(const SF2Data* data, uint32_t instrumentIndex,
                             uint32_t sampleIndex, uint8_t note, uint8_t velocity,
                             float sampleRate, InstrumentVoiceParams* outParams) {
    if (!data->loaded || sampleIndex >= data->sampleCount) return false;

    std::memset(outParams, 0, sizeof(InstrumentVoiceParams));

    const SF2Sample& sample = data->samples[sampleIndex];

    if (data->resampledData) {
        uint32_t offset = sample.sampleDataOffset;
        outParams->sampleStart = offset;
        outParams->loopStart = static_cast<uint32_t>(sample.loopStart * 44100.0f / sample.sampleRate);
        outParams->loopEnd = static_cast<uint32_t>(sample.loopEnd * 44100.0f / sample.sampleRate);
        outParams->sampleEnd = static_cast<uint32_t>(sample.end * 44100.0f / sample.sampleRate);
    } else {
        outParams->sampleStart = sample.start;
        outParams->loopStart = sample.loopStart;
        outParams->loopEnd = sample.loopEnd;
        outParams->sampleEnd = sample.end;
    }

    outParams->loopMode = (sample.sampleType & 1) ? 1 : 0;
    outParams->rootKey = sample.originalPitch;

    EnvelopeParams volEnv{};
    volEnv.delay = -7800.0f;
    volEnv.attack = -7800.0f;
    volEnv.hold = -7800.0f;
    volEnv.decay = -7800.0f;
    volEnv.sustain = 0.0f;
    volEnv.release = -7800.0f;
    outParams->volEnv = volEnv;

    return true;
}

struct ZoneGenState {
    int16_t coarseTune;
    int16_t fineTune;
    int16_t keyTrack;
    int8_t  rootKey;
    uint8_t loopMode;
    int16_t attackVolEnv;
    int16_t decayVolEnv;
    int16_t sustainVolEnv;
    int16_t releaseVolEnv;
    int16_t holdVolEnv;
    int16_t delayVolEnv;
    int16_t initialAttenuation;
    uint8_t keyLo, keyHi;
    uint8_t velLo, velHi;
    int32_t startOffset, endOffset;
    int32_t loopStartOffset, loopEndOffset;
    int32_t startCoarseOffset, endCoarseOffset;
    uint16_t sampleIndex;
    bool hasSample;
    bool hasKeyRange;
    bool hasVelRange;
    bool hasRootKey;

    ZoneGenState()
        : coarseTune(0), fineTune(0), keyTrack(100), rootKey(-1), loopMode(0),
          attackVolEnv(-12000), decayVolEnv(-12000), sustainVolEnv(0),
          releaseVolEnv(-12000), holdVolEnv(-12000), delayVolEnv(-12000),
          keyLo(0), keyHi(127), velLo(0), velHi(127),
          startOffset(0), endOffset(0),
          loopStartOffset(0), loopEndOffset(0),
           startCoarseOffset(0), endCoarseOffset(0),
           sampleIndex(0), hasSample(false),
           initialAttenuation(0),
          hasKeyRange(false), hasVelRange(false), hasRootKey(false) {}
};

static void applyGenSet(ZoneGenState& st, const SF2Generator& gen) {
    switch (gen.genOper) {
        case Gen_KeyRange:
            st.keyLo = static_cast<uint8_t>(gen.amount & 0xFF);
            st.keyHi = static_cast<uint8_t>((gen.amount >> 8) & 0xFF);
            st.hasKeyRange = true;
            break;
        case Gen_VelRange:
            st.velLo = static_cast<uint8_t>(gen.amount & 0xFF);
            st.velHi = static_cast<uint8_t>((gen.amount >> 8) & 0xFF);
            st.hasVelRange = true;
            break;
        case Gen_OverridingRootKey:
            st.rootKey = static_cast<int8_t>(gen.amount);
            st.hasRootKey = true;
            break;
        case Gen_CoarseTune:
            st.coarseTune = static_cast<int16_t>(gen.amount);
            break;
        case Gen_FineTune:
            st.fineTune = static_cast<int16_t>(gen.amount);
            break;
        case Gen_ScaleTuning:
            st.keyTrack = static_cast<int16_t>(gen.amount);
            break;
        case Gen_SampleModes:
            st.loopMode = static_cast<uint8_t>(gen.amount & 0x3);
            break;
        case Gen_SampleID:
            st.sampleIndex = static_cast<uint16_t>(gen.amount);
            st.hasSample = true;
            break;
        case Gen_StartAddrsOffset:
            st.startOffset += static_cast<int16_t>(gen.amount);
            break;
        case Gen_EndAddrsOffset:
            st.endOffset += static_cast<int16_t>(gen.amount);
            break;
        case Gen_StartLoopAddrsOffset:
            st.loopStartOffset += static_cast<int16_t>(gen.amount);
            break;
        case Gen_EndLoopAddrsOffset:
            st.loopEndOffset += static_cast<int16_t>(gen.amount);
            break;
        case Gen_StartAddrsCoarseOffset:
            st.startCoarseOffset += static_cast<int16_t>(gen.amount);
            break;
        case Gen_EndAddrsCoarseOffset:
            st.endCoarseOffset += static_cast<int16_t>(gen.amount);
            break;
        case Gen_AttackVolEnv:
            st.attackVolEnv = static_cast<int16_t>(gen.amount);
            break;
        case Gen_DecayVolEnv:
            st.decayVolEnv = static_cast<int16_t>(gen.amount);
            break;
        case Gen_SustainVolEnv:
            st.sustainVolEnv = static_cast<int16_t>(gen.amount);
            break;
        case Gen_ReleaseVolEnv:
            st.releaseVolEnv = static_cast<int16_t>(gen.amount);
            break;
        case Gen_HoldVolEnv:
            st.holdVolEnv = static_cast<int16_t>(gen.amount);
            break;
        case Gen_DelayVolEnv:
            st.delayVolEnv = static_cast<int16_t>(gen.amount);
            break;
        case Gen_InitialAttenuation:
            st.initialAttenuation = static_cast<int16_t>(gen.amount);
            break;
        default:
            break;
    }
}

static void mergeZoneInto(ZoneGenState& dst, const ZoneGenState& src) {
    dst.coarseTune = static_cast<int16_t>(dst.coarseTune + src.coarseTune);
    dst.fineTune = static_cast<int16_t>(dst.fineTune + src.fineTune);
    if (src.keyTrack != 100)
        dst.keyTrack = src.keyTrack;
    if (src.hasRootKey) {
        dst.rootKey = src.rootKey;
        dst.hasRootKey = true;
    }
    if (src.hasSample) {
        dst.sampleIndex = src.sampleIndex;
        dst.hasSample = true;
    }
    if (src.hasKeyRange) {
        if (dst.hasKeyRange) {
            uint8_t lo = src.keyLo > dst.keyLo ? src.keyLo : dst.keyLo;
            uint8_t hi = src.keyHi < dst.keyHi ? src.keyHi : dst.keyHi;
            if (lo > hi) lo = hi;
            dst.keyLo = lo;
            dst.keyHi = hi;
        } else {
            dst.keyLo = src.keyLo;
            dst.keyHi = src.keyHi;
            dst.hasKeyRange = true;
        }
    }
    if (src.hasVelRange) {
        if (dst.hasVelRange) {
            uint8_t lo = src.velLo > dst.velLo ? src.velLo : dst.velLo;
            uint8_t hi = src.velHi < dst.velHi ? src.velHi : dst.velHi;
            if (lo > hi) lo = hi;
            dst.velLo = lo;
            dst.velHi = hi;
        } else {
            dst.velLo = src.velLo;
            dst.velHi = src.velHi;
            dst.hasVelRange = true;
        }
    }
    if (src.loopMode != 0)
        dst.loopMode = src.loopMode;
    if (src.attackVolEnv != -12000)
        dst.attackVolEnv = src.attackVolEnv;
    if (src.decayVolEnv != -12000)
        dst.decayVolEnv = src.decayVolEnv;
    if (src.sustainVolEnv != 0)
        dst.sustainVolEnv = src.sustainVolEnv;
    if (src.releaseVolEnv != -12000)
        dst.releaseVolEnv = src.releaseVolEnv;
    if (src.holdVolEnv != -12000)
        dst.holdVolEnv = src.holdVolEnv;
    if (src.delayVolEnv != -12000)
        dst.delayVolEnv = src.delayVolEnv;
        dst.initialAttenuation = src.initialAttenuation;
    dst.startOffset += src.startOffset;
    dst.endOffset += src.endOffset;
    dst.loopStartOffset += src.loopStartOffset;
    dst.loopEndOffset += src.loopEndOffset;
    dst.startCoarseOffset += src.startCoarseOffset;
    dst.endCoarseOffset += src.endCoarseOffset;
}

bool sf2_find_instrument(const SF2Data* data, uint32_t presetIndex,
                         uint8_t note, uint8_t velocity,
                         uint32_t* outInstrumentIndex, uint32_t* outSampleIndex) {
    if (presetIndex >= data->presetCount) return false;

    uint16_t zoneIdx = data->presets[presetIndex].zoneIndex;
    uint16_t zoneEnd = (presetIndex + 1 < data->presetCount)
                           ? data->presets[presetIndex + 1].zoneIndex
                           : static_cast<uint16_t>(data->presetZoneCount);

    for (uint16_t zi = zoneIdx; zi < zoneEnd; ++zi) {
        const SF2PresetZone& zone = data->presetZones[zi];
        uint16_t nextGenIdx = (zi + 1 < zoneEnd)
                                  ? data->presetZones[zi + 1].generatorIndex
                                  : static_cast<uint16_t>(data->pgenCount);

        if (zone.generatorIndex >= data->pgenCount || zone.generatorIndex == nextGenIdx)
            continue;

        uint16_t firstGenOper = data->generators[zone.generatorIndex].genOper;
        if (firstGenOper != Gen_Instrument)
            continue;

        uint32_t instIdx = data->generators[zone.generatorIndex].amount;
        if (instIdx >= data->instrumentCount) continue;

        uint16_t izoneIdx = data->instruments[instIdx].zoneIndex;
        uint16_t izoneEnd = (instIdx + 1 < data->instrumentCount)
                                ? data->instruments[instIdx + 1].zoneIndex
                                : static_cast<uint16_t>(data->instrumentZoneCount);

        ZoneGenState instGlobal;
        bool hasInstGlobal = false;

        for (uint16_t izi = izoneIdx; izi < izoneEnd; ++izi) {
            const SF2InstrumentZone& izone = data->instrumentZones[izi];
            uint16_t genIdx = izone.generatorIndex;
            uint16_t genNext = (izi + 1 < izoneEnd)
                                   ? data->instrumentZones[izi + 1].generatorIndex
                                   : static_cast<uint16_t>(data->generatorCount);

            if (genIdx >= data->generatorCount || genIdx == genNext)
                continue;

            ZoneGenState instZone;
            for (uint16_t g = genIdx; g < genNext; ++g) {
                applyGenSet(instZone, data->generators[g]);
            }

            if (!instZone.hasSample) {
                if (!hasInstGlobal) {
                    instGlobal = instZone;
                    hasInstGlobal = true;
                }
                continue;
            }

            ZoneGenState merged = hasInstGlobal ? instGlobal : ZoneGenState();
            for (uint16_t g = genIdx; g < genNext; ++g) {
                applyGenSet(merged, data->generators[g]);
            }

            if (!merged.hasSample || merged.sampleIndex >= data->sampleCount)
                continue;

            bool keyOk = (note >= merged.keyLo && note <= merged.keyHi);
            bool velOk = (velocity >= merged.velLo && velocity <= merged.velHi);

            if (keyOk && velOk) {
                *outInstrumentIndex = instIdx;
                *outSampleIndex = merged.sampleIndex;
                return true;
            }
        }
    }

    return false;
}


void sf2_build_regions(SF2Data* data) {
    data->regionCount = 0;

    for (uint32_t pi = 0; pi < data->presetCount; ++pi) {
        if (data->presets[pi].preset == 0xFFFF) continue;

        uint16_t zoneIdx = data->presets[pi].zoneIndex;
        uint16_t zoneEnd = (pi + 1 < data->presetCount)
            ? data->presets[pi + 1].zoneIndex
            : static_cast<uint16_t>(data->presetZoneCount);

        for (uint16_t zi = zoneIdx; zi < zoneEnd; ++zi) {
            uint16_t genIdx = data->presetZones[zi].generatorIndex;
            uint16_t genNext = (zi + 1 < zoneEnd)
                ? data->presetZones[zi + 1].generatorIndex
                : static_cast<uint16_t>(data->pgenCount);

            if (genIdx >= data->pgenCount) continue;

            uint32_t instIdx = 0xFFFFFFFF;
            for (uint16_t g = genIdx; g < genNext && g < data->pgenCount; ++g) {
                if (data->generators[g].genOper == Gen_Instrument) {
                    instIdx = data->generators[g].amount;
                    break;
                }
            }
            if (instIdx >= data->instrumentCount) continue;

            uint16_t izoneIdx = data->instruments[instIdx].zoneIndex;
            uint16_t izoneEnd = (instIdx + 1 < data->instrumentCount)
                ? data->instruments[instIdx + 1].zoneIndex
                : static_cast<uint16_t>(data->instrumentZoneCount);

            ZoneGenState instGlobal;
            bool hasInstGlobal = false;

            for (uint16_t izi = izoneIdx; izi < izoneEnd; ++izi) {
                uint16_t genIdxI = data->instrumentZones[izi].generatorIndex;
                uint16_t genNextI = (izi + 1 < izoneEnd)
                    ? data->instrumentZones[izi + 1].generatorIndex
                    : static_cast<uint16_t>(data->generatorCount);

                if (genIdxI >= data->generatorCount) continue;

                ZoneGenState instZone;
                for (uint16_t g = genIdxI; g < genNextI && g < data->generatorCount; ++g) {
                    applyGenSet(instZone, data->generators[g]);
                }

                if (!instZone.hasSample) {
                    if (!hasInstGlobal) {
                        instGlobal = instZone;
                        hasInstGlobal = true;
                    }
                    continue;
                }

                ZoneGenState merged = hasInstGlobal ? instGlobal : ZoneGenState();
                for (uint16_t g = genIdxI; g < genNextI && g < data->generatorCount; ++g) {
                    applyGenSet(merged, data->generators[g]);
                }

                for (uint16_t g = genIdx; g < genNext && g < data->pgenCount; ++g) {
                    if (data->generators[g].genOper != Gen_Instrument) {
                        if (data->generators[g].genOper == Gen_KeyRange) {
                            uint8_t pLo = static_cast<uint8_t>(data->generators[g].amount & 0xFF);
                            uint8_t pHi = static_cast<uint8_t>((data->generators[g].amount >> 8) & 0xFF);
                            uint8_t iLo = merged.keyLo > pLo ? merged.keyLo : pLo;
                            uint8_t iHi = merged.keyHi < pHi ? merged.keyHi : pHi;
                            if (iLo <= iHi) { merged.keyLo = iLo; merged.keyHi = iHi; merged.hasKeyRange = true; }
                        } else if (data->generators[g].genOper == Gen_VelRange) {
                            uint8_t pLo = static_cast<uint8_t>(data->generators[g].amount & 0xFF);
                            uint8_t pHi = static_cast<uint8_t>((data->generators[g].amount >> 8) & 0xFF);
                            uint8_t iLo = merged.velLo > pLo ? merged.velLo : pLo;
                            uint8_t iHi = merged.velHi < pHi ? merged.velHi : pHi;
                            if (iLo <= iHi) { merged.velLo = iLo; merged.velHi = iHi; merged.hasVelRange = true; }
                        } else {
                            applyGenSet(merged, data->generators[g]);
                        }
                    }
                }

                if (!merged.hasSample || merged.sampleIndex >= data->sampleCount)
                    continue;
                if (data->regionCount >= 4096) break;

                const SF2Sample& samp = data->samples[merged.sampleIndex];
                uint32_t maxFrame = data->sampleDataFrames > 0
                    ? data->sampleDataFrames - 1 : 0;

                int64_t sStart = (int64_t)samp.start + merged.startOffset
                    + (int64_t)merged.startCoarseOffset * 32768LL;
                int64_t sEnd = (int64_t)samp.end + merged.endOffset
                    + (int64_t)merged.endCoarseOffset * 32768LL;
                int64_t sLoopStart = (int64_t)samp.loopStart + merged.loopStartOffset;
                int64_t sLoopEnd = (int64_t)samp.loopEnd + merged.loopEndOffset;

                if (sStart < 0) sStart = 0;
                if (sEnd <= sStart) continue;
                if (sEnd > (int64_t)maxFrame) sEnd = maxFrame;
                if (sLoopStart < sStart) sLoopStart = sStart;
                if (sLoopEnd > sEnd) sLoopEnd = sEnd;
                if (sLoopEnd <= sLoopStart + 1)
                    merged.loopMode = 0;

                int8_t effectiveRootKey = merged.rootKey;
                if (effectiveRootKey == -1) {
                    if (samp.originalPitch == 255 || samp.originalPitch == 0)
                        effectiveRootKey = 60;
                    else
                        effectiveRootKey = static_cast<int8_t>(samp.originalPitch);
                }

                SFSampleRegion& r = data->regions[data->regionCount++];
                r.sampleIndex = merged.sampleIndex;
                r.presetIndex = static_cast<uint16_t>(pi);
                r.keyLo = merged.keyLo;
                r.keyHi = merged.keyHi;
                r.velLo = merged.velLo;
                r.velHi = merged.velHi;
                r.rootKey = effectiveRootKey;
                r.loopMode = merged.loopMode;
                r.coarseTune = merged.coarseTune;
                r.fineTune = static_cast<int16_t>(merged.fineTune + samp.pitchCorrection);
                r.scaleTuning = merged.keyTrack;
                r.attackVolEnv = merged.attackVolEnv;
                r.decayVolEnv = merged.decayVolEnv;
                r.sustainVolEnv = merged.sustainVolEnv;
                r.releaseVolEnv = merged.releaseVolEnv;
                r.holdVolEnv = merged.holdVolEnv;
                r.delayVolEnv = merged.delayVolEnv;
                r.initialAttenuation = merged.initialAttenuation;
                r.startOffset = static_cast<int32_t>(sStart);
                r.endOffset = static_cast<int32_t>(sEnd);
                r.loopStartOffset = static_cast<int32_t>(sLoopStart);
                r.loopEndOffset = static_cast<int32_t>(sLoopEnd);
                r.startCoarseOffset = 0;
                r.endCoarseOffset = 0;
            }
        }
    }

    char dbg[128];
    sprintf(dbg, "[SVMS-SF2] build_regions: %u regions from %u presets\n",
            data->regionCount, data->presetCount);
    OutputDebugStringA(dbg);

    for (uint32_t ri = 0; ri < data->regionCount && ri < 20; ++ri) {
        const SFSampleRegion& r = data->regions[ri];
        sprintf(dbg, "[SVMS-SF2]   region[%u] preset=%u key=%u-%u vel=%u-%u root=%d coarse=%d fine=%d scale=%d loop=%u sampIdx=%u\n",
                ri, r.presetIndex, r.keyLo, r.keyHi, r.velLo, r.velHi,
                r.rootKey, r.coarseTune, r.fineTune, r.scaleTuning,
                r.loopMode, r.sampleIndex);
        OutputDebugStringA(dbg);
    }
}

} // namespace svms
