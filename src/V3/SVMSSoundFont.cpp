#include "SVMSSoundFont.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#endif
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

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

static bool sf2_load_file(FILE* f, SF2Data* outData) {
    std::memset(outData, 0, sizeof(SF2Data));

    char dbg[256];

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
    const uint32_t formType = read_u32(riffHeader + 8);
    // A small set of older Creative-era banks uses "sfpk" despite carrying
    // the ordinary uncompressed INFO/sdta/pdta SoundFont layout. Accept that
    // legacy tag only here; the required chunks below still validate the bank.
    if (formType != 0x6B626673 && formType != 0x6B706673) { // "sfbk", "sfpk"
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
                        if (count > kMaxZones) count = kMaxZones;
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

                    // Always land on the next chunk even when a malformed or
                    // oversized table had to be capped. Previously the unread
                    // tail was interpreted as a chunk header, which prevented
                    // the later shdr table from ever being discovered.
                    const uint32_t paddedSubEnd = subEnd + (subSize & 1u);
                    if (fseek(f, static_cast<long>(paddedSubEnd), SEEK_SET) != 0)
                        break;
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

bool sf2_load(const char* path, SF2Data* outData) {
    if (!path || !outData) return false;
    FILE* file = fopen(path, "rb");
    if (!file) {
        std::memset(outData, 0, sizeof(SF2Data));
        OutputDebugStringA("[SVMS-SF2] unable to open SoundFont\n");
        return false;
    }
    return sf2_load_file(file, outData);
}

bool sf2_load(const wchar_t* path, SF2Data* outData) {
    if (!path || !outData) return false;
#if defined(_WIN32)
    FILE* file = _wfopen(path, L"rb");
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    const std::string utf8 = converter.to_bytes(path);
    FILE* file = std::fopen(utf8.c_str(), "rb");
#endif
    if (!file) {
        std::memset(outData, 0, sizeof(SF2Data));
        OutputDebugStringA("[SVMS-SF2] unable to open Unicode SoundFont path\n");
        return false;
    }
    return sf2_load_file(file, outData);
}

namespace {

namespace fs = std::filesystem;

struct SfzZone {
    std::string sample;
    int keyLo = 0;
    int keyHi = 127;
    int velLo = 0;
    int velHi = 127;
    int rootKey = -1;
    int transpose = 0;
    int tune = 0;
    int keyTrack = 100;
    double volumeDb = 0.0;
    double pan = 0.0;
    int64_t offset = 0;
    int64_t end = -1;
    int64_t loopStart = -1;
    int64_t loopEnd = -1;
    int loopMode = 0;
    double ampDelay = 0.0;
    double ampAttack = 0.0;
    double ampHold = 0.0;
    double ampDecay = 0.0;
    double ampSustain = 100.0;
    double ampRelease = 0.0;
    int group = 0;
    int offBy = 0;
};

struct SfzToken {
    bool header = false;
    std::string name;
    std::string value;
};

static std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static std::string TrimAscii(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

static std::vector<SfzToken> TokenizeSfz(const std::string& source) {
    std::vector<SfzToken> result;
    size_t pos = 0;
    const auto isName = [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '$';
    };
    while (pos < source.size()) {
        while (pos < source.size() &&
               std::isspace(static_cast<unsigned char>(source[pos]))) ++pos;
        if (pos >= source.size()) break;
        if (source.compare(pos, 2, "//") == 0) {
            pos = source.find('\n', pos + 2);
            if (pos == std::string::npos) break;
            continue;
        }
        if (source.compare(pos, 2, "/*") == 0) {
            pos = source.find("*/", pos + 2);
            if (pos == std::string::npos) break;
            pos += 2;
            continue;
        }
        if (source[pos] == '<') {
            const size_t close = source.find('>', pos + 1);
            if (close == std::string::npos) break;
            result.push_back({true,
                LowerAscii(TrimAscii(source.substr(pos + 1,
                                                   close - pos - 1))), {}});
            pos = close + 1;
            continue;
        }
        const size_t nameBegin = pos;
        while (pos < source.size() &&
               isName(static_cast<unsigned char>(source[pos]))) ++pos;
        const size_t nameEnd = pos;
        while (pos < source.size() &&
               std::isspace(static_cast<unsigned char>(source[pos]))) ++pos;
        if (nameEnd == nameBegin || pos >= source.size() || source[pos] != '=') {
            while (pos < source.size() &&
                   !std::isspace(static_cast<unsigned char>(source[pos]))) ++pos;
            continue;
        }
        ++pos;
        while (pos < source.size() &&
               std::isspace(static_cast<unsigned char>(source[pos]))) ++pos;
        size_t valueBegin = pos;
        size_t valueEnd = pos;
        if (pos < source.size() && source[pos] == '"') {
            valueBegin = ++pos;
            valueEnd = source.find('"', pos);
            if (valueEnd == std::string::npos) valueEnd = source.size();
            pos = valueEnd < source.size() ? valueEnd + 1 : valueEnd;
        } else {
            valueEnd = source.size();
            size_t scan = pos;
            while (scan < source.size()) {
                if (source[scan] == '<' || source.compare(scan, 2, "//") == 0 ||
                    source.compare(scan, 2, "/*") == 0) {
                    valueEnd = scan;
                    break;
                }
                if (std::isspace(static_cast<unsigned char>(source[scan]))) {
                    size_t next = scan;
                    while (next < source.size() &&
                           std::isspace(static_cast<unsigned char>(source[next]))) ++next;
                    size_t candidate = next;
                    while (candidate < source.size() &&
                           isName(static_cast<unsigned char>(source[candidate]))) ++candidate;
                    size_t equals = candidate;
                    while (equals < source.size() &&
                           std::isspace(static_cast<unsigned char>(source[equals]))) ++equals;
                    if (candidate > next && equals < source.size() &&
                        source[equals] == '=') {
                        valueEnd = scan;
                        break;
                    }
                }
                ++scan;
            }
            pos = valueEnd;
        }
        result.push_back({false,
            LowerAscii(source.substr(nameBegin, nameEnd - nameBegin)),
            TrimAscii(source.substr(valueBegin, valueEnd - valueBegin))});
    }
    return result;
}

static bool ParseInteger(const std::string& text, int64_t& value) {
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || errno == ERANGE) return false;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end) return false;
    value = static_cast<int64_t>(parsed);
    return true;
}

static bool ParseNumber(const std::string& text, double& value) {
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || errno == ERANGE || !std::isfinite(parsed))
        return false;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end) return false;
    value = parsed;
    return true;
}

static bool ParseSfzKey(const std::string& text, int& value) {
    int64_t numeric = 0;
    if (ParseInteger(text, numeric)) {
        value = static_cast<int>((std::max)(int64_t(0),
                    (std::min)(int64_t(127), numeric)));
        return true;
    }
    if (text.size() < 2) return false;
    const std::string lower = LowerAscii(text);
    static const int semitones[7] = {9, 11, 0, 2, 4, 5, 7}; // A..G
    const char letter = lower[0];
    if (letter < 'a' || letter > 'g') return false;
    int semitone = semitones[letter - 'a'];
    size_t at = 1;
    if (at < lower.size() && (lower[at] == '#' || lower[at] == 'b'))
        semitone += lower[at++] == '#' ? 1 : -1;
    int64_t octave = 0;
    if (!ParseInteger(lower.substr(at), octave)) return false;
    // SFZ follows the common MIDI convention C4 = 60.
    const int note = static_cast<int>((octave + 1) * 12 + semitone);
    value = (std::max)(0, (std::min)(127, note));
    return true;
}

static bool ApplySfzOpcode(SfzZone& zone, const SfzToken& token,
                           std::unordered_set<std::string>& unsupported) {
    const std::string& op = token.name;
    int64_t integer = 0;
    double number = 0.0;
    if (op == "sample") { zone.sample = token.value; return true; }
    if (op == "key") {
        int key = 0; if (!ParseSfzKey(token.value, key)) return false;
        zone.keyLo = zone.keyHi = zone.rootKey = key; return true;
    }
    if (op == "lokey" || op == "hikey" || op == "pitch_keycenter") {
        int key = 0; if (!ParseSfzKey(token.value, key)) return false;
        if (op == "lokey") zone.keyLo = key;
        else if (op == "hikey") zone.keyHi = key;
        else zone.rootKey = key;
        return true;
    }
    if (op == "lovel" || op == "hivel" || op == "transpose" ||
        op == "tune" || op == "pitch_keytrack" || op == "offset" ||
        op == "end" || op == "loop_start" || op == "loop_end" ||
        op == "group" || op == "off_by") {
        if (!ParseInteger(token.value, integer)) return false;
        if (op == "lovel") zone.velLo = static_cast<int>(integer);
        else if (op == "hivel") zone.velHi = static_cast<int>(integer);
        else if (op == "transpose") zone.transpose = static_cast<int>(integer);
        else if (op == "tune") zone.tune = static_cast<int>(integer);
        else if (op == "pitch_keytrack") zone.keyTrack = static_cast<int>(integer);
        else if (op == "offset") zone.offset = integer;
        else if (op == "end") zone.end = integer;
        else if (op == "loop_start") zone.loopStart = integer;
        else if (op == "loop_end") zone.loopEnd = integer;
        else if (op == "group") zone.group = static_cast<int>(integer);
        else zone.offBy = static_cast<int>(integer);
        return true;
    }
    if (op == "volume" || op == "pan" || op == "ampeg_delay" ||
        op == "ampeg_attack" || op == "ampeg_hold" ||
        op == "ampeg_decay" || op == "ampeg_sustain" ||
        op == "ampeg_release") {
        if (!ParseNumber(token.value, number)) return false;
        if (op == "volume") zone.volumeDb = number;
        else if (op == "pan") zone.pan = number;
        else if (op == "ampeg_delay") zone.ampDelay = number;
        else if (op == "ampeg_attack") zone.ampAttack = number;
        else if (op == "ampeg_hold") zone.ampHold = number;
        else if (op == "ampeg_decay") zone.ampDecay = number;
        else if (op == "ampeg_sustain") zone.ampSustain = number;
        else zone.ampRelease = number;
        return true;
    }
    if (op == "loop_mode") {
        const std::string mode = LowerAscii(token.value);
        if (mode == "loop_continuous") zone.loopMode = 1;
        else if (mode == "loop_sustain") zone.loopMode = 3;
        else if (mode == "no_loop" || mode == "one_shot") zone.loopMode = 0;
        else return false;
        return true;
    }
    unsupported.insert(op);
    return true;
}

struct DecodedWave {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint32_t frames = 0;
    int rootKey = 60;
    int64_t loopStart = -1;
    int64_t loopEnd = -1; // exclusive
    std::vector<int16_t> planar;
};

static int32_t ReadSigned24(const uint8_t* p) {
    int32_t value = static_cast<int32_t>(p[0]) |
        (static_cast<int32_t>(p[1]) << 8) |
        (static_cast<int32_t>(p[2]) << 16);
    if ((value & 0x800000) != 0) value |= ~0xffffff;
    return value;
}

static bool DecodeWave(const fs::path& path, DecodedWave& wave) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 12 || size > static_cast<std::streamoff>(UINT32_MAX)) return false;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return false;
    if (read_u32(bytes.data()) != 0x46464952u ||
        read_u32(bytes.data() + 8) != 0x45564157u) return false; // RIFF/WAVE

    uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
    uint32_t sampleRate = 0;
    const uint8_t* sampleBytes = nullptr;
    uint32_t sampleByteCount = 0;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const uint32_t id = read_u32(bytes.data() + at);
        const uint32_t length = read_u32(bytes.data() + at + 4);
        const size_t dataAt = at + 8;
        if (dataAt + length > bytes.size()) return false;
        const uint8_t* chunk = bytes.data() + dataAt;
        if (id == 0x20746d66u && length >= 16u) { // fmt
            format = read_u16(chunk);
            channels = read_u16(chunk + 2);
            sampleRate = read_u32(chunk + 4);
            blockAlign = read_u16(chunk + 12);
            bits = read_u16(chunk + 14);
            if (format == 0xfffeu && length >= 40u) format = read_u16(chunk + 24);
        } else if (id == 0x61746164u) { // data
            sampleBytes = chunk;
            sampleByteCount = length;
        } else if (id == 0x6c706d73u && length >= 36u) { // smpl
            const uint32_t unity = read_u32(chunk + 12);
            if (unity <= 127u) wave.rootKey = static_cast<int>(unity);
            const uint32_t loopCount = read_u32(chunk + 28);
            if (loopCount != 0u && length >= 60u) {
                wave.loopStart = read_u32(chunk + 44);
                wave.loopEnd = static_cast<int64_t>(read_u32(chunk + 48)) + 1;
            }
        }
        at = dataAt + length + (length & 1u);
    }
    if (!sampleBytes || sampleRate == 0u || channels == 0u || channels > 2u ||
        blockAlign == 0u || ((format != 1u) && (format != 3u))) return false;
    if ((format == 1u && bits != 8u && bits != 16u && bits != 24u && bits != 32u) ||
        (format == 3u && bits != 32u)) return false;
    const uint32_t frames = sampleByteCount / blockAlign;
    if (frames < 2u) return false;
    wave.sampleRate = sampleRate;
    wave.channels = channels;
    wave.frames = frames;
    wave.planar.resize(static_cast<size_t>(frames) * channels);
    const uint32_t bytesPerSample = bits / 8u;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const uint8_t* source = sampleBytes +
                static_cast<size_t>(frame) * blockAlign + channel * bytesPerSample;
            int32_t value = 0;
            if (format == 3u) {
                float f = 0.0f;
                std::memcpy(&f, source, sizeof(f));
                if (!std::isfinite(f)) f = 0.0f;
                f = (std::max)(-1.0f, (std::min)(1.0f, f));
                value = static_cast<int32_t>(std::lrint(f * 32767.0f));
            } else if (bits == 8u) {
                value = (static_cast<int32_t>(*source) - 128) << 8;
            } else if (bits == 16u) {
                value = read_i16(source);
            } else if (bits == 24u) {
                value = ReadSigned24(source) >> 8;
            } else {
                value = static_cast<int32_t>(read_u32(source)) >> 16;
            }
            wave.planar[static_cast<size_t>(channel) * frames + frame] =
                static_cast<int16_t>((std::max)(-32768, (std::min)(32767, value)));
        }
    }
    return true;
}

static int16_t SecondsToTimecents(double seconds) {
    if (!(seconds > 0.0)) return -12000;
    const double value = 1200.0 * std::log2(seconds);
    return static_cast<int16_t>((std::max)(-12000.0,
        (std::min)(8000.0, std::round(value))));
}

static int16_t DbToCentibels(double db) {
    return static_cast<int16_t>((std::max)(-1440.0,
        (std::min)(1440.0, std::round(-db * 10.0))));
}

static int16_t SustainToCentibels(double percent) {
    percent = (std::max)(0.0, (std::min)(100.0, percent));
    if (percent >= 100.0) return 0;
    if (percent <= 0.0) return 1440;
    return static_cast<int16_t>((std::min)(1440.0,
        std::round(-200.0 * std::log10(percent / 100.0))));
}

struct SfzCachedSample {
    uint16_t indices[2]{};
    uint16_t channels = 0;
    uint32_t frames = 0;
    int rootKey = 60;
    int64_t loopStart = -1;
    int64_t loopEnd = -1;
};

static std::wstring SampleCacheKey(const fs::path& path) {
    std::wstring key = path.lexically_normal().wstring();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
#endif
    return key;
}

static fs::path Utf8Path(const std::string& text) {
#if defined(__cpp_char8_t)
    std::u8string utf8(text.size(), u8'\0');
    std::memcpy(utf8.data(), text.data(), text.size());
    return fs::path(utf8);
#else
    return fs::u8path(text);
#endif
}

static std::wstring SfzIncludePathKey(const fs::path& path) {
    std::error_code error;
    fs::path absolute = fs::absolute(path, error);
    if (error) absolute = path;
    std::wstring key = absolute.lexically_normal().wstring();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
#endif
    return key;
}

// Return a same-sized view of a line with comments blanked out. This lets
// #include detection ignore commented directives while preserving the original
// source verbatim for ordinary SFZ parsing.
static std::string SfzDirectiveView(const std::string& line,
                                    bool& inBlockComment) {
    std::string visible(line.size(), ' ');
    for (size_t i = 0; i < line.size();) {
        if (inBlockComment) {
            if (i + 1 < line.size() &&
                line[i] == '*' && line[i + 1] == '/') {
                inBlockComment = false;
                i += 2;
            } else {
                ++i;
            }
            continue;
        }
        if (i + 1 < line.size() &&
            line[i] == '/' && line[i + 1] == '*') {
            inBlockComment = true;
            i += 2;
            continue;
        }
        if (i + 1 < line.size() &&
            line[i] == '/' && line[i + 1] == '/')
            break;
        visible[i] = line[i];
        ++i;
    }
    return visible;
}

static bool ParseSfzIncludeDirective(const std::string& visible,
                                     bool& isInclude,
                                     std::string& includePath) {
    isInclude = false;
    includePath.clear();

    const std::string trimmed = TrimAscii(visible);
    static const std::string directive = "#include";
    if (trimmed.compare(0, directive.size(), directive) != 0)
        return true;
    if (trimmed.size() > directive.size() &&
        !std::isspace(static_cast<unsigned char>(
            trimmed[directive.size()])))
        return true;

    isInclude = true;
    size_t at = directive.size();
    while (at < trimmed.size() &&
           std::isspace(static_cast<unsigned char>(trimmed[at]))) ++at;
    if (at >= trimmed.size()) return false;

    const char open = trimmed[at];
    const char close = open == '"' ? '"' : (open == '<' ? '>' : '\0');
    if (!close) return false;
    const size_t end = trimmed.find(close, at + 1);
    if (end == std::string::npos || end == at + 1) return false;
    if (!TrimAscii(trimmed.substr(end + 1)).empty()) return false;

    includePath = trimmed.substr(at + 1, end - at - 1);
    return true;
}

static void LogSfzIncludeFailure(const char* reason,
                                 const std::string& detail = {}) {
    char message[768];
    if (detail.empty()) {
        std::snprintf(message, sizeof(message),
                      "[SVMS-SFZ] %s\n", reason);
    } else {
        std::snprintf(message, sizeof(message),
                      "[SVMS-SFZ] %s: %s\n", reason, detail.c_str());
    }
    OutputDebugStringA(message);
}

static bool ExpandSfzIncludes(
    const fs::path& path, std::string& output,
    std::unordered_set<std::wstring>& activePaths,
    uint32_t depth, size_t& expandedBytes) {
    constexpr uint32_t kMaxIncludeDepth = 32u;
    constexpr size_t kMaxExpandedBytes = 64u * 1024u * 1024u;

    if (depth > kMaxIncludeDepth) {
        LogSfzIncludeFailure("include depth limit exceeded");
        return false;
    }

    const std::wstring pathKey = SfzIncludePathKey(path);
    if (!activePaths.insert(pathKey).second) {
        LogSfzIncludeFailure("include cycle detected");
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        LogSfzIncludeFailure(depth == 0u
            ? "unable to open SFZ"
            : "unable to open included SFZ file");
        activePaths.erase(pathKey);
        return false;
    }

    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    if (source.size() >= 3u &&
        static_cast<unsigned char>(source[0]) == 0xefu &&
        static_cast<unsigned char>(source[1]) == 0xbbu &&
        static_cast<unsigned char>(source[2]) == 0xbfu) {
        source.erase(0, 3);
    }
    if (source.size() > kMaxExpandedBytes - expandedBytes) {
        LogSfzIncludeFailure("expanded include text exceeds 64 MiB");
        activePaths.erase(pathKey);
        return false;
    }
    expandedBytes += source.size();

    bool inBlockComment = false;
    size_t lineBegin = 0;
    while (lineBegin < source.size()) {
        const size_t newline = source.find('\n', lineBegin);
        const bool hasNewline = newline != std::string::npos;
        const size_t lineEnd = hasNewline ? newline : source.size();
        const std::string line =
            source.substr(lineBegin, lineEnd - lineBegin);
        const std::string visible =
            SfzDirectiveView(line, inBlockComment);

        bool isInclude = false;
        std::string includePath;
        if (!ParseSfzIncludeDirective(visible, isInclude, includePath)) {
            LogSfzIncludeFailure("malformed #include directive", line);
            activePaths.erase(pathKey);
            return false;
        }

        if (isInclude) {
            std::replace(includePath.begin(), includePath.end(), '\\', '/');
            fs::path child = Utf8Path(includePath);
            if (child.is_relative())
                child = path.parent_path() / child;
            child = child.lexically_normal();

            // Includes are textual. Do not de-duplicate them: SFZs such as
            // CFaz deliberately include the same region map once per group.
            if (!output.empty() && output.back() != '\n')
                output.push_back('\n');
            if (!ExpandSfzIncludes(child, output, activePaths,
                                   depth + 1u, expandedBytes)) {
                activePaths.erase(pathKey);
                return false;
            }
            if (!output.empty() && output.back() != '\n')
                output.push_back('\n');
        } else {
            output.append(line);
            if (hasNewline) output.push_back('\n');
        }

        if (!hasNewline) break;
        lineBegin = newline + 1;
    }

    activePaths.erase(pathKey);
    return true;
}

static bool LoadSfzPath(const fs::path& sfzPath, SF2Data* outData) {
    if (!outData) return false;
    std::memset(outData, 0, sizeof(*outData));
    try {
        std::string source;
        std::unordered_set<std::wstring> activeIncludes;
        size_t expandedBytes = 0;
        if (!ExpandSfzIncludes(sfzPath, source, activeIncludes,
                               0u, expandedBytes))
            return false;
        const std::vector<SfzToken> tokens = TokenizeSfz(source);
        SfzZone global;
        SfzZone group = global;
        SfzZone region;
        bool inRegion = false;
        std::string section;
        std::string defaultPath;
        std::vector<SfzZone> regions;
        std::unordered_set<std::string> unsupported;
        bool haveGroup = false;
        auto finishRegion = [&]() {
            if (inRegion && !region.sample.empty()) regions.push_back(region);
            inRegion = false;
        };
        for (const SfzToken& token : tokens) {
            if (token.header) {
                finishRegion();
                section = token.name;
                if (section == "global") {
                    global = SfzZone{};
                    haveGroup = false;
                } else if (section == "group") {
                    group = global;
                    haveGroup = true;
                } else if (section == "region") {
                    region = haveGroup ? group : global;
                    inRegion = true;
                }
                continue;
            }
            if (section == "control" && token.name == "default_path") {
                defaultPath = token.value;
                continue;
            }
            SfzZone* target = nullptr;
            if (section == "global") target = &global;
            else if (section == "group") target = &group;
            else if (section == "region" && inRegion) target = &region;
            if (target && !ApplySfzOpcode(*target, token, unsupported)) {
                char message[256];
                std::snprintf(message, sizeof(message),
                    "[SVMS-SFZ] ignoring invalid %s=%s\n",
                    token.name.c_str(), token.value.c_str());
                OutputDebugStringA(message);
            }
        }
        finishRegion();
        if (regions.empty()) return false;

        std::vector<int16_t> sampleStore;
        std::unordered_map<std::wstring, SfzCachedSample> cache;
        std::unordered_map<int, int16_t> classMap;
        int16_t nextClass = 1;
        auto mapClass = [&](int raw) -> int16_t {
            if (raw <= 0) return 0;
            const auto found = classMap.find(raw);
            if (found != classMap.end()) return found->second;
            if (nextClass >= 128) return 0;
            const int16_t mapped = nextClass++;
            classMap.emplace(raw, mapped);
            return mapped;
        };

        std::strncpy(outData->presets[0].name, "SFZ instrument", 19);
        outData->presets[0].bank = 0;
        outData->presets[0].preset = 0;
        outData->presetCount = 1;
        outData->presetRegionStart[0] = 0;

        for (const SfzZone& zone : regions) {
            if (zone.keyLo > zone.keyHi || zone.velLo > zone.velHi) continue;
            std::string sampleText = zone.sample;
            std::replace(sampleText.begin(), sampleText.end(), '\\', '/');
            std::string defaultText = defaultPath;
            std::replace(defaultText.begin(), defaultText.end(), '\\', '/');
            fs::path samplePath = Utf8Path(sampleText);
            if (samplePath.is_relative())
                samplePath = sfzPath.parent_path() / Utf8Path(defaultText) /
                             samplePath;
            samplePath = samplePath.lexically_normal();
            const std::wstring key = SampleCacheKey(samplePath);
            SfzCachedSample cached{};
            const auto found = cache.find(key);
            if (found != cache.end()) {
                cached = found->second;
            } else {
                DecodedWave wave;
                if (!DecodeWave(samplePath, wave)) {
                    char message[512];
                    std::snprintf(message, sizeof(message),
                        "[SVMS-SFZ] skipping unreadable WAV: %s\n",
                        sampleText.c_str());
                    OutputDebugStringA(message);
                    continue;
                }
                if (outData->sampleCount + wave.channels > kMaxSamples)
                    break;
                cached.channels = wave.channels;
                cached.frames = wave.frames;
                cached.rootKey = wave.rootKey;
                cached.loopStart = wave.loopStart;
                cached.loopEnd = wave.loopEnd;
                for (uint32_t channel = 0; channel < wave.channels; ++channel) {
                    const uint16_t index = static_cast<uint16_t>(outData->sampleCount++);
                    cached.indices[channel] = index;
                    SF2Sample& sample = outData->samples[index];
                    std::memset(&sample, 0, sizeof(sample));
                    const std::string filename = samplePath.filename().string();
                    std::strncpy(sample.name, filename.c_str(), 19);
                    sample.start = static_cast<uint32_t>(sampleStore.size());
                    sampleStore.insert(sampleStore.end(),
                        wave.planar.begin() + static_cast<size_t>(channel) * wave.frames,
                        wave.planar.begin() + static_cast<size_t>(channel + 1u) * wave.frames);
                    sampleStore.push_back(0); // interpolation guard
                    sample.end = sample.start + wave.frames;
                    sample.loopStart = wave.loopStart >= 0
                        ? sample.start + static_cast<uint32_t>(wave.loopStart) : 0u;
                    sample.loopEnd = wave.loopEnd >= 0
                        ? sample.start + static_cast<uint32_t>(wave.loopEnd) : 0u;
                    sample.sampleRate = wave.sampleRate;
                    sample.originalPitch = static_cast<uint8_t>(wave.rootKey);
                    sample.sampleType = wave.channels == 1u ? 1u
                        : static_cast<uint16_t>(channel == 0u ? 4u : 2u);
                }
                cache.emplace(key, cached);
            }

            const int16_t groupClass = mapClass(zone.group);
            const int16_t offByClass = mapClass(zone.offBy);
            for (uint32_t channel = 0; channel < cached.channels; ++channel) {
                if (outData->regionCount >= SF2Data::kMaxCompiledRegions) {
                    outData->regionOverflow = true;
                    break;
                }
                const SF2Sample& sample = outData->samples[cached.indices[channel]];
                const int64_t startRel = (std::max)(int64_t(0),
                    (std::min)(static_cast<int64_t>(cached.frames - 1u), zone.offset));
                int64_t endRel = zone.end >= 0 ? zone.end + 1
                                               : cached.frames;
                endRel = (std::max)(startRel + 1,
                    (std::min)(static_cast<int64_t>(cached.frames), endRel));
                int64_t loopStart = zone.loopStart >= 0
                    ? zone.loopStart : cached.loopStart;
                int64_t loopEnd = zone.loopEnd >= 0
                    ? zone.loopEnd + 1 : cached.loopEnd;
                loopStart = (std::max)(startRel, loopStart);
                loopEnd = (std::min)(endRel, loopEnd);
                uint8_t loopMode = static_cast<uint8_t>(zone.loopMode);
                if (loopEnd <= loopStart + 1) loopMode = 0u;

                SFSampleRegion& compiled = outData->regions[outData->regionCount++];
                std::memset(&compiled, 0, sizeof(compiled));
                compiled.sampleIndex = cached.indices[channel];
                compiled.presetIndex = 0;
                compiled.keyLo = static_cast<uint8_t>((std::max)(0, (std::min)(127, zone.keyLo)));
                compiled.keyHi = static_cast<uint8_t>((std::max)(0, (std::min)(127, zone.keyHi)));
                compiled.velLo = static_cast<uint8_t>((std::max)(0, (std::min)(127, zone.velLo)));
                compiled.velHi = static_cast<uint8_t>((std::max)(0, (std::min)(127, zone.velHi)));
                compiled.rootKey = static_cast<int8_t>(zone.rootKey >= 0
                    ? zone.rootKey : cached.rootKey);
                compiled.coarseTune = static_cast<int16_t>((std::max)(-127,
                    (std::min)(127, zone.transpose)));
                compiled.fineTune = static_cast<int16_t>((std::max)(-1200,
                    (std::min)(1200, zone.tune)));
                compiled.scaleTuning = static_cast<int16_t>((std::max)(0,
                    (std::min)(1200, zone.keyTrack)));
                compiled.startOffset = static_cast<int32_t>(sample.start + startRel);
                compiled.endOffset = static_cast<int32_t>(sample.start + endRel);
                compiled.loopStartOffset = loopMode != 0u
                    ? static_cast<int32_t>(sample.start + loopStart) : 0;
                compiled.loopEndOffset = loopMode != 0u
                    ? static_cast<int32_t>(sample.start + loopEnd) : 0;
                compiled.loopMode = loopMode;
                compiled.delayVolEnv = SecondsToTimecents(zone.ampDelay);
                compiled.attackVolEnv = SecondsToTimecents(zone.ampAttack);
                compiled.holdVolEnv = SecondsToTimecents(zone.ampHold);
                compiled.decayVolEnv = zone.ampSustain >= 100.0
                    ? -12000 : SecondsToTimecents(zone.ampDecay);
                compiled.sustainVolEnv = SustainToCentibels(zone.ampSustain);
                compiled.releaseVolEnv = SecondsToTimecents(zone.ampRelease);
                double channelDb = zone.volumeDb;
                if (cached.channels == 2u) {
                    const double normalizedPan = (std::max)(-1.0,
                        (std::min)(1.0, zone.pan / 100.0));
                    const double channelGain = channel == 0u
                        ? std::sqrt(1.0 - (std::max)(0.0, normalizedPan))
                        : std::sqrt(1.0 + (std::min)(0.0, normalizedPan));
                    channelDb += channelGain > 0.0
                        ? 20.0 * std::log10(channelGain) : -144.0;
                    compiled.pan = channel == 0u ? -500 : 500;
                } else {
                    compiled.pan = static_cast<int16_t>(std::round(
                        (std::max)(-100.0, (std::min)(100.0, zone.pan)) * 5.0));
                }
                compiled.initialAttenuation = DbToCentibels(channelDb);
                compiled.exclusiveClass = groupClass;
                // A negative value distinguishes an SFZ membership-only
                // group from SF2's implicit exclusive-class self-choke.
                compiled.offByClass = zone.offBy > 0 ? offByClass
                    : (zone.group > 0 ? int16_t(-1) : int16_t(0));
            }
            if (outData->regionOverflow) break;
        }
        if (outData->regionCount == 0u || outData->sampleCount == 0u ||
            sampleStore.empty()) return false;
        sampleStore.insert(sampleStore.end(), 8u, 0);
        outData->sampleDataFrames = static_cast<uint32_t>(sampleStore.size() - 8u);
        outData->sampleDataSize = outData->sampleDataFrames * sizeof(int16_t);
        outData->sampleData = static_cast<int16_t*>(std::malloc(
            sampleStore.size() * sizeof(int16_t)));
        if (!outData->sampleData) return false;
        std::memcpy(outData->sampleData, sampleStore.data(),
                    sampleStore.size() * sizeof(int16_t));
        outData->presetRegionCount[0] = outData->regionCount;
        outData->fallbackPresetIndex = 0;
        outData->loaded = true;
        outData->isSfz = true;
        char message[256];
        std::snprintf(message, sizeof(message),
            "[SVMS-SFZ] compiled %u regions, %u samples, %u frames; %u unsupported opcodes ignored\n",
            outData->regionCount, outData->sampleCount,
            outData->sampleDataFrames,
            static_cast<unsigned>(unsupported.size()));
        OutputDebugStringA(message);
        return true;
    } catch (const std::exception&) {
        sf2_free(outData);
        return false;
    }
}

static bool HasExtension(const fs::path& path, const wchar_t* extension) {
    std::wstring actual = path.extension().wstring();
    std::transform(actual.begin(), actual.end(), actual.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return actual == extension;
}

} // namespace

bool sfz_load(const char* path, SF2Data* outData) {
    if (!path || !outData) return false;
    return LoadSfzPath(Utf8Path(path), outData);
}

bool sfz_load(const wchar_t* path, SF2Data* outData) {
    if (!path || !outData) return false;
    return LoadSfzPath(fs::path(path), outData);
}

bool soundfont_load(const char* path, SF2Data* outData) {
    if (!path || !outData) return false;
    return HasExtension(Utf8Path(path), L".sfz")
        ? sfz_load(path, outData) : sf2_load(path, outData);
}

bool soundfont_load(const wchar_t* path, SF2Data* outData) {
    if (!path || !outData) return false;
    return HasExtension(fs::path(path), L".sfz")
        ? sfz_load(path, outData) : sf2_load(path, outData);
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
    if (!data || !outPresetIndex) return false;
    if (data->isSfz && data->presetCount != 0u) {
        *outPresetIndex = 0u;
        return true;
    }
    for (uint32_t i = 0; i < data->presetCount; ++i) {
        if (data->presets[i].bank == bank && data->presets[i].preset == preset) {
            *outPresetIndex = i;
            return true;
        }
    }
    return false;
}

bool sf2_resolve_preset(const SF2Data* data, uint16_t bank, uint8_t program,
                        bool percussionChannel, uint32_t* outPresetIndex) {
    if (!data || !outPresetIndex) return false;

    if (data->isSfz && data->presetCount != 0u) {
        *outPresetIndex = 0u;
        return true;
    }

    if (percussionChannel) {
        if (sf2_find_preset(data, static_cast<uint16_t>(128u | bank), program,
                            outPresetIndex)) return true;
        if (sf2_find_preset(data, 128, program, outPresetIndex)) return true;
        if (sf2_find_preset(data, 128, 0, outPresetIndex)) return true;
    }

    if (sf2_find_preset(data, bank, program, outPresetIndex)) return true;
    return bank != 0 && sf2_find_preset(data, 0, program, outPresetIndex);
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
    int32_t attackVolEnv;
    int32_t decayVolEnv;
    int32_t sustainVolEnv;
    int32_t releaseVolEnv;
    int32_t holdVolEnv;
    int32_t delayVolEnv;
    int16_t initialAttenuation;
    int16_t initialFilterFc, initialFilterQ, pan, reverbSend, chorusSend;
    int16_t modLfoToPitch, vibLfoToPitch, modEnvToPitch;
    int16_t delayVibLfo, freqVibLfo;
    int16_t modLfoToFilterFc, modEnvToFilterFc, modLfoToVolume;
    int16_t exclusiveClass;
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
    bool hasLoopMode;
    bool hasCoarseTune;
    bool hasFineTune;
    bool hasScaleTuning;
    bool hasInitialAttenuation;
    bool hasAttackVolEnv;
    bool hasDecayVolEnv;
    bool hasSustainVolEnv;
    bool hasReleaseVolEnv;
    bool hasHoldVolEnv;
    bool hasDelayVolEnv;
    bool hasInitialFilterFc;
    bool hasInitialFilterQ;
    bool hasPan;
    bool hasReverbSend;
    bool hasChorusSend;
    bool hasModLfoToPitch;
    bool hasVibLfoToPitch;
    bool hasDelayVibLfo;
    bool hasFreqVibLfo;
    bool hasModEnvToPitch;
    bool hasModLfoToFilterFc;
    bool hasModEnvToFilterFc;
    bool hasModLfoToVolume;
    bool hasExclusiveClass;

    ZoneGenState()
        : coarseTune(0), fineTune(0), keyTrack(100), rootKey(-1), loopMode(0),
          attackVolEnv(-12000), decayVolEnv(-12000), sustainVolEnv(0),
          releaseVolEnv(-12000), holdVolEnv(-12000), delayVolEnv(-12000),
          keyLo(0), keyHi(127), velLo(0), velHi(127),
          startOffset(0), endOffset(0),
          loopStartOffset(0), loopEndOffset(0),
           startCoarseOffset(0), endCoarseOffset(0),
           sampleIndex(0), hasSample(false),
           initialAttenuation(0), initialFilterFc(0), initialFilterQ(0), pan(0),
           reverbSend(0), chorusSend(0), modLfoToPitch(0), vibLfoToPitch(0),
           // delayVibLfo is timecents (-12000 = 0 s) but freqVibLfo is
           // absolute cents where the SF2 default of 0 means 1 Hz. Treating
           // an absent frequency as -12000 would freeze the LFO at ~0.001 Hz
           // and silence vibrato entirely.
           delayVibLfo(-12000), freqVibLfo(0),
           modEnvToPitch(0), modLfoToFilterFc(0), modEnvToFilterFc(0),
           modLfoToVolume(0), exclusiveClass(0),
           hasKeyRange(false), hasVelRange(false), hasRootKey(false),
           hasLoopMode(false), hasCoarseTune(false), hasFineTune(false),
           hasScaleTuning(false), hasInitialAttenuation(false),
           hasAttackVolEnv(false), hasDecayVolEnv(false), hasSustainVolEnv(false),
           hasReleaseVolEnv(false), hasHoldVolEnv(false), hasDelayVolEnv(false),
           hasInitialFilterFc(false), hasInitialFilterQ(false), hasPan(false),
           hasReverbSend(false), hasChorusSend(false),
           hasModLfoToPitch(false), hasVibLfoToPitch(false),
           hasDelayVibLfo(false), hasFreqVibLfo(false),
           hasModEnvToPitch(false), hasModLfoToFilterFc(false),
           hasModEnvToFilterFc(false), hasModLfoToVolume(false),
           hasExclusiveClass(false) {}
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
            st.hasCoarseTune = true;
            break;
        case Gen_FineTune:
            st.fineTune = static_cast<int16_t>(gen.amount);
            st.hasFineTune = true;
            break;
        case Gen_ScaleTuning:
            st.keyTrack = static_cast<int16_t>(gen.amount);
            st.hasScaleTuning = true;
            break;
        case Gen_SampleModes:
            st.loopMode = static_cast<uint8_t>(gen.amount & 0x3);
            st.hasLoopMode = true;
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
            st.hasAttackVolEnv = true;
            break;
        case Gen_DecayVolEnv:
            st.decayVolEnv = static_cast<int16_t>(gen.amount);
            st.hasDecayVolEnv = true;
            break;
        case Gen_SustainVolEnv:
            st.sustainVolEnv = static_cast<int16_t>(gen.amount);
            st.hasSustainVolEnv = true;
            break;
        case Gen_ReleaseVolEnv:
            st.releaseVolEnv = static_cast<int16_t>(gen.amount);
            st.hasReleaseVolEnv = true;
            break;
        case Gen_HoldVolEnv:
            st.holdVolEnv = static_cast<int16_t>(gen.amount);
            st.hasHoldVolEnv = true;
            break;
        case Gen_DelayVolEnv:
            st.delayVolEnv = static_cast<int16_t>(gen.amount);
            st.hasDelayVolEnv = true;
            break;
        case Gen_InitialAttenuation:
            st.initialAttenuation = static_cast<int16_t>(gen.amount);
            st.hasInitialAttenuation = true;
            break;
        case Gen_InitialFilterFc:
            st.initialFilterFc = static_cast<int16_t>(gen.amount);
            st.hasInitialFilterFc = true;
            break;
        case Gen_InitialFilterQ:
            st.initialFilterQ = static_cast<int16_t>(gen.amount);
            st.hasInitialFilterQ = true;
            break;
        case Gen_Pan:
            st.pan = static_cast<int16_t>(gen.amount);
            st.hasPan = true;
            break;
        case Gen_ReverbEffectsSend:
            st.reverbSend = static_cast<int16_t>(gen.amount);
            st.hasReverbSend = true;
            break;
        case Gen_ChorusEffectsSend:
            st.chorusSend = static_cast<int16_t>(gen.amount);
            st.hasChorusSend = true;
            break;
        case Gen_ModLfoToPitch:
            st.modLfoToPitch = static_cast<int16_t>(gen.amount);
            st.hasModLfoToPitch = true;
            break;
        case Gen_VibLfoToPitch:
            st.vibLfoToPitch = static_cast<int16_t>(gen.amount);
            st.hasVibLfoToPitch = true;
            break;
        case Gen_DelayVibLFO:
            st.delayVibLfo = static_cast<int16_t>(gen.amount);
            st.hasDelayVibLfo = true;
            break;
        case Gen_FreqVibLFO:
            st.freqVibLfo = static_cast<int16_t>(gen.amount);
            st.hasFreqVibLfo = true;
            break;
        case Gen_ModEnvToPitch:
            st.modEnvToPitch = static_cast<int16_t>(gen.amount);
            st.hasModEnvToPitch = true;
            break;
        case Gen_ModLfoToFilterFc:
            st.modLfoToFilterFc = static_cast<int16_t>(gen.amount);
            st.hasModLfoToFilterFc = true;
            break;
        case Gen_ModEnvToFilterFc:
            st.modEnvToFilterFc = static_cast<int16_t>(gen.amount);
            st.hasModEnvToFilterFc = true;
            break;
        case Gen_ModLfoToVolume:
            st.modLfoToVolume = static_cast<int16_t>(gen.amount);
            st.hasModLfoToVolume = true;
            break;
        case Gen_ExclusiveClass:
            st.exclusiveClass = static_cast<int16_t>(gen.amount);
            st.hasExclusiveClass = true;
            break;
        default:
            break;
    }
}

static void mergeZoneInto(ZoneGenState& dst, const ZoneGenState& src) {
    // SF2 ranges intersect; additive generators accumulate; overriding
    // generators use the later (more local) zone's value.
    dst.coarseTune = static_cast<int16_t>(dst.coarseTune + src.coarseTune);
    dst.fineTune = static_cast<int16_t>(dst.fineTune + src.fineTune);
    if (src.hasCoarseTune) dst.hasCoarseTune = true;
    if (src.hasFineTune) dst.hasFineTune = true;
    if (src.hasScaleTuning) {
        dst.keyTrack = static_cast<int16_t>(dst.keyTrack + src.keyTrack - 100);
        dst.hasScaleTuning = true;
    }
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
            if (lo > hi) {
                // Preserve an empty intersection. Collapsing it to a
                // single key creates a region that does not exist in SF2.
                dst.keyLo = 1;
                dst.keyHi = 0;
            } else {
                dst.keyLo = lo;
                dst.keyHi = hi;
            }
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
            if (lo > hi) {
                dst.velLo = 1;
                dst.velHi = 0;
            } else {
                dst.velLo = lo;
                dst.velHi = hi;
            }
        } else {
            dst.velLo = src.velLo;
            dst.velHi = src.velHi;
            dst.hasVelRange = true;
        }
    }
    if (src.hasLoopMode) {
        dst.loopMode = src.loopMode;
        dst.hasLoopMode = true;
    }
    if (src.hasAttackVolEnv) { dst.attackVolEnv = dst.hasAttackVolEnv ? dst.attackVolEnv + src.attackVolEnv : src.attackVolEnv; dst.hasAttackVolEnv = true; }
    if (src.hasDecayVolEnv) { dst.decayVolEnv = dst.hasDecayVolEnv ? dst.decayVolEnv + src.decayVolEnv : src.decayVolEnv; dst.hasDecayVolEnv = true; }
    if (src.hasSustainVolEnv) { dst.sustainVolEnv += src.sustainVolEnv; dst.hasSustainVolEnv = true; }
    if (src.hasReleaseVolEnv) { dst.releaseVolEnv = dst.hasReleaseVolEnv ? dst.releaseVolEnv + src.releaseVolEnv : src.releaseVolEnv; dst.hasReleaseVolEnv = true; }
    if (src.hasHoldVolEnv) { dst.holdVolEnv = dst.hasHoldVolEnv ? dst.holdVolEnv + src.holdVolEnv : src.holdVolEnv; dst.hasHoldVolEnv = true; }
    if (src.hasDelayVolEnv) { dst.delayVolEnv = dst.hasDelayVolEnv ? dst.delayVolEnv + src.delayVolEnv : src.delayVolEnv; dst.hasDelayVolEnv = true; }
    if (src.hasInitialAttenuation) { dst.initialAttenuation = static_cast<int16_t>(dst.initialAttenuation + src.initialAttenuation); dst.hasInitialAttenuation = true; }
    if (src.hasInitialFilterFc) { dst.initialFilterFc = src.initialFilterFc; dst.hasInitialFilterFc = true; }
    if (src.hasInitialFilterQ) { dst.initialFilterQ = src.initialFilterQ; dst.hasInitialFilterQ = true; }
    if (src.hasPan) { dst.pan = static_cast<int16_t>(dst.pan + src.pan); dst.hasPan = true; }
    if (src.hasReverbSend) { dst.reverbSend = static_cast<int16_t>(dst.reverbSend + src.reverbSend); dst.hasReverbSend = true; }
    if (src.hasChorusSend) { dst.chorusSend = static_cast<int16_t>(dst.chorusSend + src.chorusSend); dst.hasChorusSend = true; }
    if (src.hasModLfoToPitch) { dst.modLfoToPitch = static_cast<int16_t>(dst.modLfoToPitch + src.modLfoToPitch); dst.hasModLfoToPitch = true; }
    if (src.hasVibLfoToPitch) { dst.vibLfoToPitch = static_cast<int16_t>(dst.vibLfoToPitch + src.vibLfoToPitch); dst.hasVibLfoToPitch = true; }
    if (src.hasDelayVibLfo) { dst.delayVibLfo = dst.hasDelayVibLfo ? dst.delayVibLfo + src.delayVibLfo : src.delayVibLfo; dst.hasDelayVibLfo = true; }
    if (src.hasFreqVibLfo) { dst.freqVibLfo = dst.hasFreqVibLfo ? dst.freqVibLfo + src.freqVibLfo : src.freqVibLfo; dst.hasFreqVibLfo = true; }
    if (src.hasModEnvToPitch) { dst.modEnvToPitch = static_cast<int16_t>(dst.modEnvToPitch + src.modEnvToPitch); dst.hasModEnvToPitch = true; }
    if (src.hasModLfoToFilterFc) { dst.modLfoToFilterFc = static_cast<int16_t>(dst.modLfoToFilterFc + src.modLfoToFilterFc); dst.hasModLfoToFilterFc = true; }
    if (src.hasModEnvToFilterFc) { dst.modEnvToFilterFc = static_cast<int16_t>(dst.modEnvToFilterFc + src.modEnvToFilterFc); dst.hasModEnvToFilterFc = true; }
    if (src.hasModLfoToVolume) { dst.modLfoToVolume = static_cast<int16_t>(dst.modLfoToVolume + src.modLfoToVolume); dst.hasModLfoToVolume = true; }
    if (src.hasExclusiveClass) { dst.exclusiveClass = src.exclusiveClass; dst.hasExclusiveClass = true; }
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
        uint16_t nextGenIdx = (static_cast<uint32_t>(zi) + 1u < data->presetZoneCount)
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
            uint16_t genNext = (static_cast<uint32_t>(izi) + 1u < data->instrumentZoneCount)
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


static ZoneGenState ParseZone(const SF2Data* data, uint16_t generatorIndex,
                              uint16_t generatorEnd) {
    ZoneGenState state;
    if (generatorIndex >= data->generatorCount) return state;
    if (generatorEnd > data->generatorCount) generatorEnd = static_cast<uint16_t>(data->generatorCount);
    for (uint16_t g = generatorIndex; g < generatorEnd; ++g)
        applyGenSet(state, data->generators[g]);
    return state;
}

static bool ZoneHasGenerator(const SF2Data* data, uint16_t generatorIndex,
                             uint16_t generatorEnd, uint16_t operation) {
    if (generatorIndex >= data->generatorCount) return false;
    generatorEnd = (std::min)(generatorEnd, static_cast<uint16_t>(data->generatorCount));
    for (uint16_t g = generatorIndex; g < generatorEnd; ++g)
        if (data->generators[g].genOper == operation) return true;
    return false;
}

static int16_t ClampEnvelopeTimecents(int32_t value) {
    if (value < -12000) value = -12000;
    if (value > 8000) value = 8000;
    return static_cast<int16_t>(value);
}

static int16_t ClampVolumeEnvelopeSustain(int32_t value) {
    if (value < 0) value = 0;
    if (value > 1440) value = 1440;
    return static_cast<int16_t>(value);
}

static void AppendCompiledRegion(SF2Data* data, uint32_t presetIndex,
                                 const ZoneGenState& merged) {
    if (merged.sampleIndex >= data->sampleCount) return;
    if (data->regionCount >= SF2Data::kMaxCompiledRegions) {
        data->regionOverflow = true;
        return;
    }

    const SF2Sample& sample = data->samples[merged.sampleIndex];
    const uint32_t maxFrame = data->sampleDataFrames > 0 ? data->sampleDataFrames - 1 : 0;
    int64_t start = static_cast<int64_t>(sample.start) + merged.startOffset +
                    static_cast<int64_t>(merged.startCoarseOffset) * 32768;
    int64_t end = static_cast<int64_t>(sample.end) + merged.endOffset +
                  static_cast<int64_t>(merged.endCoarseOffset) * 32768;
    int64_t loopStart = static_cast<int64_t>(sample.loopStart) + merged.loopStartOffset;
    int64_t loopEnd = static_cast<int64_t>(sample.loopEnd) + merged.loopEndOffset;

    start = (std::max)(int64_t(0), start);
    end = (std::min)(static_cast<int64_t>(maxFrame), end);
    loopStart = (std::max)(start, loopStart);
    loopEnd = (std::min)(end, loopEnd);
    if (end <= start) return;
    if (loopEnd <= loopStart + 1) loopStart = loopEnd = 0;

    int8_t root = merged.rootKey;
    if (root < 0) root = (sample.originalPitch == 0 || sample.originalPitch == 255)
        ? 60 : static_cast<int8_t>(sample.originalPitch);

    SFSampleRegion& region = data->regions[data->regionCount++];
    std::memset(&region, 0, sizeof(region));
    region.sampleIndex = merged.sampleIndex;
    region.presetIndex = static_cast<uint16_t>(presetIndex);
    region.keyLo = merged.keyLo; region.keyHi = merged.keyHi;
    region.velLo = merged.velLo; region.velHi = merged.velHi;
    region.rootKey = root;
    region.loopMode = (loopEnd > loopStart + 1) ? merged.loopMode : 0;
    region.coarseTune = merged.coarseTune;
    region.fineTune = static_cast<int16_t>(merged.fineTune + sample.pitchCorrection);
    region.scaleTuning = merged.keyTrack;
    region.attackVolEnv = ClampEnvelopeTimecents(merged.attackVolEnv);
    region.decayVolEnv = ClampEnvelopeTimecents(merged.decayVolEnv);
    region.sustainVolEnv = ClampVolumeEnvelopeSustain(merged.sustainVolEnv);
    region.releaseVolEnv = ClampEnvelopeTimecents(merged.releaseVolEnv);
    region.holdVolEnv = ClampEnvelopeTimecents(merged.holdVolEnv);
    region.delayVolEnv = ClampEnvelopeTimecents(merged.delayVolEnv);
    region.initialAttenuation = merged.initialAttenuation;
    region.startOffset = static_cast<int32_t>(start);
    region.endOffset = static_cast<int32_t>(end);
    region.loopStartOffset = static_cast<int32_t>(loopStart);
    region.loopEndOffset = static_cast<int32_t>(loopEnd);
    region.initialFilterFc = merged.initialFilterFc;
    region.initialFilterQ = merged.initialFilterQ;
    region.pan = merged.pan;
    region.reverbSend = merged.reverbSend;
    region.chorusSend = merged.chorusSend;
    region.modLfoToPitch = merged.modLfoToPitch;
    // SF2's built-in default modulator ("vibrato LFO → initial pitch",
    // ±50 cents driven by CC1/channel pressure) supplies the depth on
    // virtually every SoundFont; the vibLfoToPitch generator itself
    // defaults to zero and appears only when a font overrides it.
    region.vibLfoToPitch = merged.hasVibLfoToPitch
        ? merged.vibLfoToPitch : 50;
    region.delayVibLfo = ClampEnvelopeTimecents(merged.delayVibLfo);
    region.freqVibLfo = merged.freqVibLfo;
    region.modEnvToPitch = merged.modEnvToPitch;
    region.modLfoToFilterFc = merged.modLfoToFilterFc;
    region.modEnvToFilterFc = merged.modEnvToFilterFc;
    region.modLfoToVolume = merged.modLfoToVolume;
    region.exclusiveClass = merged.exclusiveClass;
    region.offByClass = merged.exclusiveClass;
}

void sf2_build_regions(SF2Data* data) {
    // SFZ bypasses the SF2 bag/generator compiler but produces the identical
    // final region tables. Keep those direct compiled regions intact.
    if (data && data->isSfz) return;
    data->regionCount = 0;
    std::memset(data->presetRegionStart, 0, sizeof(data->presetRegionStart));
    std::memset(data->presetRegionCount, 0, sizeof(data->presetRegionCount));
    data->regionOverflow = false;

    for (uint32_t pi = 0; pi < data->presetCount; ++pi) {
        data->presetRegionStart[pi] = data->regionCount;
        if (data->presets[pi].preset == 0xFFFF) continue;
        const uint16_t pBegin = data->presets[pi].zoneIndex;
        const uint16_t pEnd = (pi + 1 < data->presetCount)
            ? data->presets[pi + 1].zoneIndex
            : static_cast<uint16_t>(data->presetZoneCount);
        ZoneGenState presetGlobal;
        std::vector<std::pair<uint16_t, ZoneGenState>> presetLocal;

        for (uint16_t zi = pBegin; zi < pEnd; ++zi) {
            // The bag at pEnd belongs to the next preset (or the terminal
            // record), but its generator index is precisely the end of this
            // preset's last zone. Do not let that zone consume later pgen.
            const uint16_t next = (static_cast<uint32_t>(zi) + 1u < data->presetZoneCount)
                ? data->presetZones[zi + 1].generatorIndex
                : static_cast<uint16_t>(data->pgenCount);
            ZoneGenState zone = ParseZone(data, data->presetZones[zi].generatorIndex, next);
            if (!ZoneHasGenerator(data, data->presetZones[zi].generatorIndex, next, Gen_Instrument))
                mergeZoneInto(presetGlobal, zone);
            else {
                uint32_t instrumentIndex = UINT32_MAX;
                for (uint16_t g = data->presetZones[zi].generatorIndex; g < next; ++g) {
                    if (data->generators[g].genOper == Gen_Instrument) {
                        instrumentIndex = data->generators[g].amount;
                        break;
                    }
                }
                if (instrumentIndex != UINT32_MAX)
                    presetLocal.emplace_back(static_cast<uint16_t>(instrumentIndex), zone);
            }
        }

        for (const auto& presetZone : presetLocal) {
            const uint32_t instrumentIndex = presetZone.first;
            if (instrumentIndex >= data->instrumentCount) continue;
            const uint16_t iBegin = data->instruments[instrumentIndex].zoneIndex;
            const uint16_t iEnd = (instrumentIndex + 1 < data->instrumentCount)
                ? data->instruments[instrumentIndex + 1].zoneIndex
                : static_cast<uint16_t>(data->instrumentZoneCount);
            ZoneGenState instrumentGlobal;

            for (uint16_t izi = iBegin; izi < iEnd; ++izi) {
                // As with pbag, the first bag of the next instrument is the
                // sentinel that terminates this instrument's final zone.
                const uint16_t next = (static_cast<uint32_t>(izi) + 1u < data->instrumentZoneCount)
                    ? data->instrumentZones[izi + 1].generatorIndex
                    : static_cast<uint16_t>(data->generatorCount);
                ZoneGenState zone = ParseZone(data, data->instrumentZones[izi].generatorIndex, next);
                if (!ZoneHasGenerator(data, data->instrumentZones[izi].generatorIndex, next, Gen_SampleID)) {
                    mergeZoneInto(instrumentGlobal, zone);
                    continue;
                }
                ZoneGenState merged;
                mergeZoneInto(merged, presetGlobal);
                mergeZoneInto(merged, instrumentGlobal);
                mergeZoneInto(merged, zone);
                mergeZoneInto(merged, presetZone.second);
                if (merged.keyLo <= merged.keyHi && merged.velLo <= merged.velHi)
                    AppendCompiledRegion(data, pi, merged);
            }
        }
        data->presetRegionCount[pi] =
            data->regionCount - data->presetRegionStart[pi];
    }

    /* Legacy compiler retained below for reference during migration. */
#if 0
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
                r.attackVolEnv = ClampEnvelopeTimecents(merged.attackVolEnv);
                r.decayVolEnv = ClampEnvelopeTimecents(merged.decayVolEnv);
                r.sustainVolEnv = ClampVolumeEnvelopeSustain(merged.sustainVolEnv);
                r.releaseVolEnv = ClampEnvelopeTimecents(merged.releaseVolEnv);
                r.holdVolEnv = ClampEnvelopeTimecents(merged.holdVolEnv);
                r.delayVolEnv = ClampEnvelopeTimecents(merged.delayVolEnv);
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

#endif
    char dbg[128];
    sprintf(dbg, "[SVMS-SF2] build_regions: %u regions from %u presets\n",
            data->regionCount, data->presetCount);
    OutputDebugStringA(dbg);
    if (data->regionOverflow)
        OutputDebugStringA("[SVMS-SF2] ERROR: compiled region capacity exceeded; SoundFont rejected partially\n");

    // Pick the widest-keyboard-coverage preset for region fallback. Ties
    // resolve to the lowest preset index so the choice is deterministic
    // across loads of the same file.
    {
        uint32_t bestCoverage = 0u;
        uint32_t keyCoverage[128];
        data->fallbackPresetIndex = UINT16_MAX;
        for (uint32_t pi = 0; pi < data->presetCount &&
                 pi < kMaxPresets; ++pi) {
            for (uint32_t k = 0; k < 128; ++k) keyCoverage[k] = 0u;
            const uint32_t begin = data->presetRegionStart[pi];
            const uint32_t end = begin + data->presetRegionCount[pi];
            if (begin > data->regionCount || end > data->regionCount)
                continue;
            uint32_t covered = 0u;
            for (uint32_t ri = begin; ri < end; ++ri) {
                const SFSampleRegion& r = data->regions[ri];
                const uint32_t lo = r.keyLo < 128u ? r.keyLo : 127u;
                const uint32_t hi = r.keyHi < 128u ? r.keyHi : 127u;
                for (uint32_t k = lo; k <= hi; ++k) {
                    if (keyCoverage[k]++ == 0u) ++covered;
                }
            }
            if (covered > bestCoverage) {
                bestCoverage = covered;
                data->fallbackPresetIndex = static_cast<uint16_t>(pi);
            }
        }
        sprintf(dbg, "[SVMS-SF2] region fallback preset: %u (%u keys)\n",
                data->fallbackPresetIndex, bestCoverage);
        OutputDebugStringA(dbg);
    }

    for (uint32_t ri = 0; ri < data->regionCount && ri < 20; ++ri) {
        const SFSampleRegion& r = data->regions[ri];
        sprintf(dbg, "[SVMS-SF2]   region[%u] preset=%u key=%u-%u vel=%u-%u root=%d coarse=%d fine=%d scale=%d loop=%u sampIdx=%u\n",
                ri, r.presetIndex, r.keyLo, r.keyHi, r.velLo, r.velHi,
                r.rootKey, r.coarseTune, r.fineTune, r.scaleTuning,
                r.loopMode, r.sampleIndex);
        OutputDebugStringA(dbg);
    }
}

uint32_t sf2_find_regions(const SF2Data* data, uint32_t presetIndex,
                          uint8_t note, uint8_t velocity,
                          const SFSampleRegion** outRegions,
                          uint32_t outCapacity) {
    if (!data || !outRegions || presetIndex >= data->presetCount) return 0;
    uint32_t count = 0;
    const uint32_t begin = data->presetRegionStart[presetIndex];
    const uint32_t end = begin + data->presetRegionCount[presetIndex];
    if (begin > data->regionCount || end > data->regionCount) return 0;
    if (note > 127u) {
        uint8_t highest = 0;
        for (uint32_t i = begin; i < end; ++i)
            highest = (std::max)(highest, data->regions[i].keyHi);
        if (note > highest) note = highest;
    }
    for (uint32_t i = begin; i < end; ++i) {
        const SFSampleRegion& region = data->regions[i];
        if (note < region.keyLo || note > region.keyHi ||
            velocity < region.velLo || velocity > region.velHi) continue;
        if (count < outCapacity) outRegions[count] = &region;
        ++count;
    }
    return count;
}

bool sf2_validate_region(const SF2Data* data, const SFSampleRegion* region) {
    if (!data || !region ||
        region->sampleIndex >= data->sampleCount || data->sampleDataFrames < 2)
        return false;

    const int64_t start = region->startOffset;
    const int64_t end = region->endOffset;
    const int64_t loopStart = region->loopStartOffset;
    const int64_t loopEnd = region->loopEndOffset;
    const int64_t frames = data->sampleDataFrames;

    if (start < 0 || end <= start || end >= frames) return false;
    if (region->loopMode != 0 &&
        (loopStart < start || loopEnd <= loopStart + 1 || loopEnd > end))
        return false;
    return true;
}

float sf2_region_initial_peak(const SF2Data* data, const SFSampleRegion* region,
                              uint32_t windowFrames) {
    if (!sf2_validate_region(data, region) || !data->sampleData ||
        windowFrames == 0) return 0.0f;
    const uint32_t start = static_cast<uint32_t>(region->startOffset);
    const uint32_t end = static_cast<uint32_t>(region->endOffset);
    const uint32_t count = (std::min)(windowFrames, end - start);
    int32_t peak = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t value = data->sampleData[start + i];
        const int32_t magnitude = value < 0 ? -value : value;
        if (magnitude > peak) peak = magnitude;
    }
    return static_cast<float>(peak) / 32768.0f;
}

} // namespace svms
