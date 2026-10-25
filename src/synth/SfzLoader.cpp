#include "synth/SoundFont.h"
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
#if __has_include(<filesystem>)
#include <filesystem>
namespace svms_fs = std::filesystem;
#define SVMS_FILESYSTEM_TS 0
#else
// GCC 7 (the Ubuntu 18.04 Linux build floor) ships only the Filesystem TS,
// which lacks lexically_normal() and the error_code absolute().
#include <experimental/filesystem>
namespace svms_fs = std::experimental::filesystem;
#define SVMS_FILESYSTEM_TS 1
#endif
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


namespace {

namespace fs = svms_fs;

fs::path LexicallyNormal(const fs::path& path) {
#if SVMS_FILESYSTEM_TS
    fs::path normal;
    for (const fs::path& part : path) {
        if (part == ".") continue;
        if (part == ".." && !normal.empty() && normal.filename() != "..") {
            normal = normal.parent_path();
            continue;
        }
        normal /= part;
    }
    return normal;
#else
    return path.lexically_normal();
#endif
}

fs::path AbsolutePath(const fs::path& path, std::error_code& error) {
#if SVMS_FILESYSTEM_TS
    error.clear();
    try {
        return fs::absolute(path);
    } catch (...) {
        error = std::make_error_code(std::errc::invalid_argument);
        return path;
    }
#else
    return fs::absolute(path, error);
#endif
}

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
    double cutoffHz = 0.0;
    double resonanceDb = 0.0;
    uint8_t filterType = static_cast<uint8_t>(FilterType::None);
    bool cutoffSet = false;
    bool resonanceSet = false;
    bool filterTypeExplicit = false;
    int group = 0;
    int offBy = 0;
    double filVeltrack = 0.0; // cents of cutoff at velocity 127
    double filKeytrack = 0.0; // cents of cutoff per key
    int filKeycenter = 60;
    double pitchVeltrack = 0.0; // cents of pitch at velocity 127
    double pitchLfoDepth = 0.0; // cents
    double pitchLfoFreq = 0.0;  // Hz
    double pitchLfoDelay = 0.0;
    double effect1 = 0.0;       // percent, reverb send
    double effect2 = 0.0;       // percent, chorus send
    int swLast = -1;
    int swDefault = -1;
    double loRand = 0.0;
    int seqPosition = 1;
    bool skip = false;          // release triggers never play on note-on
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
        op == "ampeg_release" || op == "cutoff" || op == "resonance") {
        if (!ParseNumber(token.value, number)) return false;
        if (op == "volume") zone.volumeDb = number;
        else if (op == "pan") zone.pan = number;
        else if (op == "ampeg_delay") zone.ampDelay = number;
        else if (op == "ampeg_attack") zone.ampAttack = number;
        else if (op == "ampeg_hold") zone.ampHold = number;
        else if (op == "ampeg_decay") zone.ampDecay = number;
        else if (op == "ampeg_sustain") zone.ampSustain = number;
        else if (op == "ampeg_release") zone.ampRelease = number;
        else if (op == "cutoff") {
            zone.cutoffHz = number;
            zone.cutoffSet = true;
            if (!zone.filterTypeExplicit && zone.filterType == static_cast<uint8_t>(FilterType::None))
                zone.filterType = static_cast<uint8_t>(FilterType::LowPass2Pole);
        } else {
            zone.resonanceDb = number;
            zone.resonanceSet = true;
            if (!zone.filterTypeExplicit && zone.filterType == static_cast<uint8_t>(FilterType::None))
                zone.filterType = static_cast<uint8_t>(FilterType::LowPass2Pole);
        }
        return true;
    }
    if (op == "fil_veltrack" || op == "fil_keytrack" || op == "pitch_veltrack" ||
        op == "pitchlfo_depth" || op == "pitchlfo_freq" || op == "pitchlfo_delay" ||
        op == "effect1" || op == "effect2" || op == "lorand" || op == "cents") {
        if (!ParseNumber(token.value, number)) return false;
        if (op == "fil_veltrack") zone.filVeltrack = number;
        else if (op == "fil_keytrack") zone.filKeytrack = number;
        else if (op == "pitch_veltrack") zone.pitchVeltrack = number;
        else if (op == "pitchlfo_depth") zone.pitchLfoDepth = number;
        else if (op == "pitchlfo_freq") zone.pitchLfoFreq = number;
        else if (op == "pitchlfo_delay") zone.pitchLfoDelay = number;
        else if (op == "effect1") zone.effect1 = number;
        else if (op == "effect2") zone.effect2 = number;
        else if (op == "lorand") zone.loRand = number;
        else zone.tune = static_cast<int>(std::lround(number));
        return true;
    }
    if (op == "fil_keycenter" || op == "sw_last" || op == "sw_default") {
        int key = 0; if (!ParseSfzKey(token.value, key)) return false;
        if (op == "fil_keycenter") zone.filKeycenter = key;
        else if (op == "sw_last") zone.swLast = key;
        else zone.swDefault = key;
        return true;
    }
    if (op == "seq_position") {
        if (!ParseInteger(token.value, integer)) return false;
        zone.seqPosition = static_cast<int>(integer);
        return true;
    }
    if (op == "trigger") {
        const std::string mode = LowerAscii(token.value);
        if (mode != "attack" && mode != "first" && mode != "legato") zone.skip = true;
        return true;
    }
    if (op == "loopmode") {
        SfzToken alias = token;
        alias.name = "loop_mode";
        return ApplySfzOpcode(zone, alias, unsupported);
    }
    if (op == "fil_type") {
        const std::string type = LowerAscii(token.value);
        zone.filterTypeExplicit = true;
        if (type == "lpf_2p" || type == "lpf_1p" || type == "lpf_4p" || type == "lpf_6p") {
            zone.filterType = static_cast<uint8_t>(FilterType::LowPass2Pole);
        } else if (type == "none" || type == "no_filter") {
            zone.filterType = static_cast<uint8_t>(FilterType::None);
        } else {
            unsupported.insert(op + "=" + type);
        }
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

static int16_t HertzToAbsoluteCents(double hz) {
    if (!(hz > 0.0) || !std::isfinite(hz)) return -12000;
    const double cents = 1200.0 * std::log2(hz / 8.176);
    return static_cast<int16_t>((std::max)(-12000.0,
        (std::min)(13500.0, std::round(cents))));
}

static int16_t ResonanceDbToCentibels(double db) {
    if (!std::isfinite(db)) return 0;
    return static_cast<int16_t>(std::round((std::max)(
        0.0, (std::min)(96.0, db)) * 10.0));
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
    std::wstring key = LexicallyNormal(path).wstring();
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
    fs::path absolute = AbsolutePath(path, error);
    if (error) absolute = path;
    std::wstring key = LexicallyNormal(absolute).wstring();
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
            child = LexicallyNormal(child);

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

// `#define $NAME value` lines: drop them and substitute $NAME in the rest.
static void ExpandSfzDefines(std::string& source) {
    std::vector<std::pair<std::string, std::string>> defines;
    std::string out;
    size_t pos = 0;
    while (pos < source.size()) {
        size_t eol = source.find('\n', pos);
        if (eol == std::string::npos) eol = source.size();
        const std::string line = source.substr(pos, eol - pos);
        const std::string trimmed = TrimAscii(line);
        if (trimmed.compare(0, 7, "#define") == 0) {
            const std::string rest = TrimAscii(trimmed.substr(7));
            const size_t space = rest.find_first_of(" \t");
            if (space != std::string::npos)
                defines.emplace_back(rest.substr(0, space), TrimAscii(rest.substr(space)));
        } else {
            out += line;
        }
        out.push_back('\n');
        pos = eol + 1;
    }
    if (defines.empty()) return;
    std::sort(defines.begin(), defines.end(), [](const auto& l, const auto& r) {
        return l.first.size() > r.first.size(); });
    for (const auto& d : defines) {
        for (size_t at = 0; (at = out.find(d.first, at)) != std::string::npos;
             at += d.second.size())
            out.replace(at, d.first.size(), d.second);
    }
    source.swap(out);
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
        ExpandSfzDefines(source);
        const std::vector<SfzToken> tokens = TokenizeSfz(source);
        SfzZone global;
        SfzZone master;
        bool haveMaster = false;
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
                    haveMaster = false;
                } else if (section == "master") {
                    master = global;
                    haveMaster = true;
                    haveGroup = false;
                } else if (section == "group") {
                    group = haveMaster ? master : global;
                    haveGroup = true;
                } else if (section == "region") {
                    region = haveGroup ? group : (haveMaster ? master : global);
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
            else if (section == "master") target = &master;
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

        // Cutoff and pitch are baked per region, so velocity/key tracking is
        // applied by splitting a region into velocity bands and key groups
        // that each get their own value.  Release triggers, round robins
        // beyond the first and non-default keyswitches never play on note-on.
        {
            constexpr int kBands = 8;
            constexpr int kKeyGroup = 3;
            std::vector<SfzZone> expanded;
            expanded.reserve(regions.size());
            for (const SfzZone& zone : regions) {
                if (zone.skip || zone.loRand > 0.0 || zone.seqPosition > 1 ||
                    (zone.swLast >= 0 && zone.swLast != zone.swDefault))
                    continue;
                const bool filtered = zone.cutoffSet &&
                    zone.filterType == static_cast<uint8_t>(FilterType::LowPass2Pole);
                const bool velSplit = (filtered && zone.filVeltrack != 0.0) ||
                                      zone.pitchVeltrack != 0.0;
                const bool keySplit = filtered && zone.filKeytrack != 0.0;
                if (!velSplit && !keySplit) { expanded.push_back(zone); continue; }
                const int bands = velSplit ? kBands : 1;
                const int keyStep = keySplit ? kKeyGroup : 128;
                for (int band = 0; band < bands; ++band) {
                    SfzZone velZone = zone;
                    if (velSplit) {
                        velZone.velLo = (std::max)(zone.velLo, band * 128 / kBands);
                        velZone.velHi = (std::min)(zone.velHi, (band + 1) * 128 / kBands - 1);
                        if (velZone.velLo > velZone.velHi) continue;
                    }
                    const double vel = 0.5 * (velZone.velLo + velZone.velHi);
                    for (int key = zone.keyLo; key <= zone.keyHi; key += keyStep) {
                        SfzZone split = velZone;
                        if (keySplit) {
                            split.keyLo = key;
                            split.keyHi = (std::min)(zone.keyHi, key + keyStep - 1);
                        }
                        if (filtered) {
                            double cents = zone.filVeltrack * vel / 127.0;
                            if (keySplit)
                                cents += zone.filKeytrack *
                                    (0.5 * (split.keyLo + split.keyHi) - zone.filKeycenter);
                            split.cutoffHz = (std::min)(24000.0,
                                zone.cutoffHz * std::pow(2.0, cents / 1200.0));
                        }
                        split.tune += static_cast<int>(
                            std::lround(zone.pitchVeltrack * vel / 127.0));
                        expanded.push_back(split);
                    }
                }
            }
            regions.swap(expanded);
            if (regions.empty()) return false;
        }

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
            samplePath = LexicallyNormal(samplePath);
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
                if (zone.filterType ==
                    static_cast<uint8_t>(FilterType::LowPass2Pole)) {
                    compiled.filterType =
                        static_cast<uint8_t>(FilterType::LowPass2Pole);
                    compiled.initialFilterFc = zone.cutoffSet
                        ? HertzToAbsoluteCents(zone.cutoffHz)
                        : int16_t(13500);
                    compiled.initialFilterQ = zone.resonanceSet
                        ? ResonanceDbToCentibels(zone.resonanceDb)
                        : int16_t(0);
                }
                if (zone.pitchLfoDepth != 0.0 && zone.pitchLfoFreq > 0.0) {
                    compiled.vibLfoToPitch = static_cast<int16_t>((std::max)(-12000.0,
                        (std::min)(12000.0, zone.pitchLfoDepth)));
                    compiled.freqVibLfo = static_cast<int16_t>(std::lround(
                        1200.0 * std::log2(zone.pitchLfoFreq)));
                    compiled.delayVibLfo = SecondsToTimecents(zone.pitchLfoDelay);
                }
                compiled.reverbSend = static_cast<int16_t>((std::max)(0.0,
                    (std::min)(1000.0, zone.effect1 * 10.0)));
                compiled.chorusSend = static_cast<int16_t>((std::max)(0.0,
                    (std::min)(1000.0, zone.effect2 * 10.0)));
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

} // namespace svms
