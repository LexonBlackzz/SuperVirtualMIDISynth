#include "CanonicalSoundFont.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace svms::canonical {
namespace {
// std::numbers::pi_v<float> is C++20; this literal rounds to the same float.
constexpr float kPi = 3.14159265358979323846F;
} // namespace

SampleBank& PreparedSoundFont::preparationSamples() {
    if (finalized_) throw std::logic_error("prepared SoundFont is immutable after finalize");
    return samples_;
}

PreparedPreset& PreparedSoundFont::addPreset(std::uint16_t bank, std::uint8_t program,
                                             std::string name) {
    if (finalized_) throw std::logic_error("prepared SoundFont is immutable after finalize");
    presets_.push_back({});
    auto& preset = presets_.back(); preset.bank=bank; preset.program=program; preset.name=std::move(name);
    return preset;
}

void PreparedSoundFont::finalize() {
    if (finalized_) return;
    for (auto& preset : presets_) {
        preset.lookupRegionIndices.clear();
        std::size_t cell = 0;
        for (std::uint32_t key = 0; key < 128; ++key) {
            for (std::uint32_t velocity = 0; velocity < 128; ++velocity, ++cell) {
                preset.lookupOffsets[cell] = static_cast<std::uint32_t>(
                    preset.lookupRegionIndices.size());
                for (std::uint32_t i = 0; i < preset.regions.size(); ++i) {
                    const auto& r = preset.regions[i];
                    if (key >= r.keyLow && key <= r.keyHigh &&
                        velocity >= r.velocityLow && velocity <= r.velocityHigh) {
                        preset.lookupRegionIndices.push_back(i);
                    }
                }
            }
        }
        preset.lookupOffsets[cell] = static_cast<std::uint32_t>(
            preset.lookupRegionIndices.size());
    }
    std::sort(presets_.begin(), presets_.end(), [](const auto& a, const auto& b) {
        return std::tie(a.bank, a.program) < std::tie(b.bank, b.program);
    });
    finalized_ = true;
}

const PreparedPreset* PreparedSoundFont::findPreset(std::uint16_t bank,
                                                     std::uint8_t program) const noexcept {
    const auto it = std::lower_bound(presets_.begin(), presets_.end(), std::pair{bank, program},
        [](const PreparedPreset& p, const auto& key) {
            return std::pair{p.bank, p.program} < key;
        });
    return it != presets_.end() && it->bank == bank && it->program == program ? &*it : nullptr;
}

Span<const std::uint32_t> PreparedSoundFont::matchingRegions(
    const PreparedPreset& preset, std::uint8_t key, std::uint8_t velocity) const {
    const auto cell = static_cast<std::size_t>(key) * 128 + velocity;
    const auto begin = preset.lookupOffsets[cell];
    const auto end = preset.lookupOffsets[cell + 1];
    return {preset.lookupRegionIndices.data() + begin, end - begin};
}

void appendPreparedNoteOn(std::vector<Event>& output, const PreparedPreset& preset,
                          const PreparedSoundFont& font, std::uint64_t frame,
                          std::uint32_t& sequence, std::uint8_t channel,
                          std::uint8_t key, std::uint8_t velocity,
                          double outputSampleRate, std::uint32_t noteInstance) {
    if (!(outputSampleRate > 0.0)) throw std::invalid_argument("sample rate must be positive");
    const auto matches = font.matchingRegions(preset, key, velocity);
    std::uint64_t exclusiveMaskLow = 0;
    std::uint64_t exclusiveMaskHigh = 0;
    for (const auto index : matches) {
        const auto exclusive = preset.regions[index].exclusiveClass;
        if (exclusive == 0) continue;
        if (exclusive < 64) exclusiveMaskLow |= std::uint64_t{1} << exclusive;
        else exclusiveMaskHigh |= std::uint64_t{1} << (exclusive - 64);
    }
    bool firstRegion = true;
    for (const auto index : matches) {
        const auto& r = preset.regions[index];
        const auto effectiveKey = r.forcedKey >= 0
            ? static_cast<std::uint8_t>(r.forcedKey) : key;
        const auto effectiveVelocity = r.forcedVelocity >= 0
            ? static_cast<std::uint8_t>(r.forcedVelocity) : velocity;
        std::array<float, 61> modAdds{};
        const auto sourceValue = [&](std::uint16_t source) {
            if (source == 0) return 1.0F;
            const auto index = source & 0x7fU;
            const bool midiCc = (source & 0x80U) != 0;
            const bool descending = (source & 0x100U) != 0;
            const bool bipolar = (source & 0x200U) != 0;
            const auto curve = source >> 10;
            if (midiCc || curve != 0 || (index != 2 && index != 3)) return 0.0F;
            const float native = static_cast<float>(index == 2
                ? effectiveVelocity : effectiveKey);
            float value = bipolar ? (native - 64.0F) / 64.0F : native / 128.0F;
            if (descending) value = bipolar ? -value : 1.0F - value;
            return value;
        };
        for (const auto& modulator : r.modulators) {
            if (modulator.destination >= modAdds.size()) continue;
            float value = static_cast<float>(modulator.amount) *
                sourceValue(modulator.source) * sourceValue(modulator.amountSource);
            if (modulator.transform == 2) value = std::fabs(value);
            modAdds[modulator.destination] += value;
        }
        const double cents = (static_cast<int>(effectiveKey) - r.rootKey) * r.scaleTuning +
            r.tuningCents + modAdds[51] * 100.0F + modAdds[52];
        const float increment = static_cast<float>(r.sampleRate / outputSampleRate *
            std::exp2(cents / 1200.0));
        const float pan = std::clamp(r.pan + modAdds[17] / 500.0F, -1.0F, 1.0F);
        const float angle = (pan + 1.0F) * (kPi * 0.25F);
        const float normalizedVelocity = static_cast<float>(effectiveVelocity) / 127.0F;
        const float velocityGain = normalizedVelocity * normalizedVelocity;
        const auto frames = [&](double seconds) {
            return static_cast<std::uint32_t>(std::clamp(
                std::llround(seconds * outputSampleRate), 0LL,
                static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));
        };
        const double hold = r.holdSeconds == 0.0F ? 0.0 :
            r.holdSeconds * std::exp2(r.keynumToHoldTimecents *
                (60.0 - static_cast<double>(effectiveKey)) / 1200.0);
        const double decay = r.decaySeconds == 0.0F ? 0.0 :
            r.decaySeconds * std::exp2(r.keynumToDecayTimecents *
                (60.0 - static_cast<double>(effectiveKey)) / 1200.0);
        const float modulatedGain = r.gain * std::pow(10.0F,
            -modAdds[48] / 200.0F);
        const auto releaseFrames = (std::max)(frames(0.010),
            frames(r.releaseSeconds * std::exp2(modAdds[38] / 1200.0F)));
        auto event = Event::noteOn(frame, sequence++, key, r.sample, increment,
            modulatedGain * velocityGain * std::cos(angle),
            modulatedGain * velocityGain * std::sin(angle),
            frames(r.attackSeconds * std::exp2(modAdds[34] / 1200.0F)),
            releaseFrames, channel);
        event.delayFrames = frames(r.delaySeconds * std::exp2(modAdds[33] / 1200.0F));
        event.holdFrames = frames(hold * std::exp2(modAdds[35] / 1200.0F));
        event.decayFrames = frames(decay * std::exp2(modAdds[36] / 1200.0F));
        event.sustainGain = std::clamp(r.sustainGain *
            std::pow(10.0F, -modAdds[37] / 200.0F), 0.0F, 1.0F);
        event.noteInstance = noteInstance;
        event.logarithmicEnvelope = r.logarithmicEnvelope;
        event.sampleMode = r.sampleMode;
        event.exclusiveClass = r.exclusiveClass;
        if (firstRegion) {
            event.exclusiveMaskLow = exclusiveMaskLow;
            event.exclusiveMaskHigh = exclusiveMaskHigh;
            event.exclusiveReleaseFrames = frames(0.010);
            firstRegion = false;
        }
        // Preserve SVMS's square velocity loudness law, but retain the other
        // applicable implicit SF2 velocity modulator: velocity -> filter Fc.
        const float velocityFilterCents = r.defaultVelocityToFilterCents *
            (127.0F - static_cast<float>(effectiveVelocity)) / 128.0F;
        const float cutoffCents = std::clamp(
            static_cast<float>(r.filterCutoffCents) + velocityFilterCents + modAdds[8],
            -12000.0F, 13500.0F);
        event.baseFilterCutoffCents = cutoffCents;
        event.filterResonanceCentibels = std::clamp(
            static_cast<float>(r.filterResonanceCentibels) + modAdds[9], 0.0F, 960.0F);
        const float cutoffHz = 8.176F * std::pow(2.0F, cutoffCents / 1200.0F);
        const float normalizedCutoff = cutoffHz / static_cast<float>(outputSampleRate);
        if (normalizedCutoff > 0.0F && normalizedCutoff < 0.499F) {
            const float qDb = event.filterResonanceCentibels * 0.1F;
            const float qInv = 1.0F / std::pow(10.0F, qDb / 20.0F);
            const float k = std::tan(kPi * normalizedCutoff);
            const float kk = k * k;
            const float norm = 1.0F / (1.0F + k * qInv + kk);
            event.filterA0 = kk * norm;
            event.filterB1 = 2.0F * (kk - 1.0F) * norm;
            event.filterB2 = (1.0F - k * qInv + kk) * norm;
            event.filterEnabled = true;
        }
        event.modLfoDelayFrames = frames(r.modLfoDelaySeconds * std::exp2(modAdds[21] / 1200.0F));
        event.vibLfoDelayFrames = frames(r.vibLfoDelaySeconds * std::exp2(modAdds[23] / 1200.0F));
        event.modLfoPhaseStep = 4.0F * r.modLfoFrequencyHz * std::exp2(modAdds[22] / 1200.0F) /
            static_cast<float>(outputSampleRate);
        event.vibLfoPhaseStep = 4.0F * r.vibLfoFrequencyHz * std::exp2(modAdds[24] / 1200.0F) /
            static_cast<float>(outputSampleRate);
        event.modLfoToPitchCents = r.modLfoToPitchCents + modAdds[5];
        event.vibLfoToPitchCents = r.vibLfoToPitchCents + modAdds[6];
        event.modWheelToVibPitchCents = r.defaultModWheelToVibPitchCents;
        event.modLfoToFilterCents = r.modLfoToFilterCents + modAdds[10];
        event.modLfoToVolumeCentibels = r.modLfoToVolumeCentibels + modAdds[13];
        event.modEnvDelayFrames = frames(r.modEnvDelaySeconds * std::exp2(modAdds[25] / 1200.0F));
        event.modEnvAttackFrames = frames(r.modEnvAttackSeconds * std::exp2(modAdds[26] / 1200.0F));
        event.modEnvHoldFrames = frames(r.modEnvHoldSeconds * std::exp2(modAdds[27] / 1200.0F) * std::exp2(
            (r.modEnvKeynumToHoldTimecents + modAdds[31]) *
            (60.0 - static_cast<double>(effectiveKey)) / 1200.0));
        event.modEnvDecayFrames = frames(r.modEnvDecaySeconds * std::exp2(modAdds[28] / 1200.0F) * std::exp2(
            (r.modEnvKeynumToDecayTimecents + modAdds[32]) *
            (60.0 - static_cast<double>(effectiveKey)) / 1200.0));
        event.modEnvReleaseFrames = frames(r.modEnvReleaseSeconds * std::exp2(modAdds[30] / 1200.0F));
        event.modEnvSustain = std::clamp(r.modEnvSustain - modAdds[29] / 1000.0F, 0.0F, 1.0F);
        event.modEnvToPitchCents = r.modEnvToPitchCents + modAdds[7];
        event.modEnvToFilterCents = r.modEnvToFilterCents + modAdds[11];
        output.push_back(event);
    }
}

namespace {

std::uint16_t u16(const std::vector<std::uint8_t>& b, std::size_t p) {
    if (p + 2 > b.size()) throw std::runtime_error("truncated SF2 field");
    return static_cast<std::uint16_t>(b[p] | (b[p + 1] << 8));
}
std::int16_t s16(const std::vector<std::uint8_t>& b, std::size_t p) {
    return static_cast<std::int16_t>(u16(b, p));
}
std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t p) {
    if (p + 4 > b.size()) throw std::runtime_error("truncated SF2 field");
    return static_cast<std::uint32_t>(b[p] | (b[p + 1] << 8) |
        (b[p + 2] << 16) | (b[p + 3] << 24));
}
bool idAt(const std::vector<std::uint8_t>& b, std::size_t p, const char* id) {
    return p + 4 <= b.size() && std::memcmp(b.data() + p, id, 4) == 0;
}
std::string name20(const std::vector<std::uint8_t>& b, std::size_t p) {
    std::size_t n = 0;
    while (n < 20 && b[p + n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(b.data() + p), n);
}

struct Chunk { std::size_t data{}; std::size_t size{}; };

Chunk findList(const std::vector<std::uint8_t>& b, const char* type) {
    std::size_t p = 12;
    while (p + 8 <= b.size()) {
        const auto size = u32(b, p + 4);
        const auto data = p + 8;
        if (size > b.size() - data) throw std::runtime_error("SF2 chunk exceeds file");
        if (idAt(b, p, "LIST") && size >= 4 && idAt(b, data, type)) return {data + 4, size - 4};
        p = data + size + (size & 1U);
    }
    throw std::runtime_error(std::string("missing SF2 LIST ") + type);
}

Chunk findChunk(const std::vector<std::uint8_t>& b, Chunk list, const char* id) {
    std::size_t p = list.data;
    const auto end = list.data + list.size;
    while (p + 8 <= end) {
        const auto size = u32(b, p + 4);
        const auto data = p + 8;
        if (size > end - data) throw std::runtime_error("SF2 subchunk exceeds LIST");
        if (idAt(b, p, id)) return {data, size};
        p = data + size + (size & 1U);
    }
    throw std::runtime_error(std::string("missing SF2 chunk ") + id);
}

Chunk findOptionalChunk(const std::vector<std::uint8_t>& b, Chunk list, const char* id) {
    std::size_t p = list.data;
    const auto end = list.data + list.size;
    while (p + 8 <= end) {
        const auto size = u32(b, p + 4);
        const auto data = p + 8;
        if (size > end - data) throw std::runtime_error("SF2 subchunk exceeds LIST");
        if (idAt(b, p, id)) return {data, size};
        p = data + size + (size & 1U);
    }
    return {};
}

struct Bag { std::uint16_t gen{}, mod{}; };
struct Generator { std::uint16_t op{}; std::uint16_t raw{}; };
struct Header { std::string name; std::uint16_t value{}; std::uint16_t bank{}; std::uint16_t bag{}; };
struct SampleHeader {
    std::string name; std::uint32_t start{}, end{}, loopStart{}, loopEnd{}, rate{};
    std::uint8_t pitch{}; std::int8_t correction{}; std::uint16_t type{};
};

enum class SfGenerator : std::uint16_t {
    StartAddrsOffset=0, EndAddrsOffset=1, StartloopAddrsOffset=2,
    EndloopAddrsOffset=3, StartAddrsCoarseOffset=4, ModLfoToPitch=5,
    VibLfoToPitch=6, ModEnvToPitch=7, InitialFilterFc=8, InitialFilterQ=9,
    ModLfoToFilterFc=10, ModEnvToFilterFc=11, EndAddrsCoarseOffset=12,
    ModLfoToVolume=13, Unused1=14, ChorusEffectsSend=15, ReverbEffectsSend=16,
    Pan=17, Unused2=18, Unused3=19, Unused4=20, DelayModLFO=21,
    FreqModLFO=22, DelayVibLFO=23, FreqVibLFO=24, DelayModEnv=25,
    AttackModEnv=26, HoldModEnv=27, DecayModEnv=28, SustainModEnv=29,
    ReleaseModEnv=30, KeynumToModEnvHold=31, KeynumToModEnvDecay=32,
    DelayVolEnv=33, AttackVolEnv=34, HoldVolEnv=35, DecayVolEnv=36,
    SustainVolEnv=37, ReleaseVolEnv=38, KeynumToVolEnvHold=39,
    KeynumToVolEnvDecay=40, Instrument=41, Reserved1=42, KeyRange=43,
    VelRange=44, StartloopAddrsCoarseOffset=45, Keynum=46, Velocity=47,
    InitialAttenuation=48, Reserved2=49, EndloopAddrsCoarseOffset=50,
    CoarseTune=51, FineTune=52, SampleId=53, SampleModes=54, Reserved3=55,
    ScaleTuning=56, ExclusiveClass=57, OverridingRootKey=58, Unused5=59,
    EndOper=60
};

constexpr unsigned op(SfGenerator value) { return static_cast<unsigned>(value); }

struct GenSet {
    std::uint64_t present{};
    std::array<int, 61> amount{};
};

bool has(const GenSet& g, unsigned generator) {
    return generator < 61 && (g.present & (std::uint64_t{1} << generator)) != 0;
}
int get(const GenSet& g, SfGenerator generator, int fallback = 0) {
    return has(g, op(generator)) ? g.amount[op(generator)] : fallback;
}

GenSet overrideZone(const GenSet& global, const GenSet& local) {
    GenSet result = global;
    for (unsigned generator = 0; generator < result.amount.size(); ++generator) {
        if (has(local, generator)) {
            result.amount[generator] = local.amount[generator];
            result.present |= std::uint64_t{1} << generator;
        }
    }
    return result;
}

const char* generatorName(std::uint16_t generator) {
    static constexpr const char* names[] = {
        "startAddrsOffset","endAddrsOffset","startloopAddrsOffset","endloopAddrsOffset",
        "startAddrsCoarseOffset","modLfoToPitch","vibLfoToPitch","modEnvToPitch",
        "initialFilterFc","initialFilterQ","modLfoToFilterFc","modEnvToFilterFc",
        "endAddrsCoarseOffset","modLfoToVolume","unused1","chorusEffectsSend",
        "reverbEffectsSend","pan","unused2","unused3","unused4","delayModLFO",
        "freqModLFO","delayVibLFO","freqVibLFO","delayModEnv","attackModEnv",
        "holdModEnv","decayModEnv","sustainModEnv","releaseModEnv",
        "keynumToModEnvHold","keynumToModEnvDecay","delayVolEnv","attackVolEnv",
        "holdVolEnv","decayVolEnv","sustainVolEnv","releaseVolEnv",
        "keynumToVolEnvHold","keynumToVolEnvDecay","instrument","reserved1",
        "keyRange","velRange","startloopAddrsCoarseOffset","keynum","velocity",
        "initialAttenuation","reserved2","endloopAddrsCoarseOffset","coarseTune",
        "fineTune","sampleID","sampleModes","reserved3","scaleTuning",
        "exclusiveClass","overridingRootKey","unused5","endOper"};
    return generator < std::size(names) ? names[generator] : "unknownGenerator";
}

bool isReserved(std::uint16_t generator) {
    switch (static_cast<SfGenerator>(generator)) {
    case SfGenerator::Unused1: case SfGenerator::Unused2:
    case SfGenerator::Unused3: case SfGenerator::Unused4:
    case SfGenerator::Reserved1: case SfGenerator::Reserved2:
    case SfGenerator::Reserved3: case SfGenerator::Unused5:
    case SfGenerator::EndOper: return true;
    default: return false;
    }
}

bool isSupported(std::uint16_t generator) {
    switch (static_cast<SfGenerator>(generator)) {
    case SfGenerator::StartAddrsOffset: case SfGenerator::EndAddrsOffset:
    case SfGenerator::StartloopAddrsOffset: case SfGenerator::EndloopAddrsOffset:
    case SfGenerator::StartAddrsCoarseOffset: case SfGenerator::EndAddrsCoarseOffset:
    case SfGenerator::ModLfoToPitch: case SfGenerator::VibLfoToPitch:
    case SfGenerator::ModEnvToPitch:
    case SfGenerator::InitialFilterFc: case SfGenerator::InitialFilterQ:
    case SfGenerator::ModLfoToFilterFc: case SfGenerator::ModEnvToFilterFc:
    case SfGenerator::ModLfoToVolume:
    case SfGenerator::DelayModLFO: case SfGenerator::FreqModLFO:
    case SfGenerator::DelayVibLFO: case SfGenerator::FreqVibLFO:
    case SfGenerator::DelayModEnv: case SfGenerator::AttackModEnv:
    case SfGenerator::HoldModEnv: case SfGenerator::DecayModEnv:
    case SfGenerator::SustainModEnv: case SfGenerator::ReleaseModEnv:
    case SfGenerator::KeynumToModEnvHold: case SfGenerator::KeynumToModEnvDecay:
    case SfGenerator::Pan: case SfGenerator::DelayVolEnv:
    case SfGenerator::AttackVolEnv: case SfGenerator::HoldVolEnv:
    case SfGenerator::DecayVolEnv: case SfGenerator::SustainVolEnv:
    case SfGenerator::ReleaseVolEnv: case SfGenerator::KeynumToVolEnvHold:
    case SfGenerator::KeynumToVolEnvDecay: case SfGenerator::Instrument:
    case SfGenerator::KeyRange: case SfGenerator::VelRange:
    case SfGenerator::StartloopAddrsCoarseOffset:
    case SfGenerator::InitialAttenuation: case SfGenerator::EndloopAddrsCoarseOffset:
    case SfGenerator::CoarseTune: case SfGenerator::FineTune:
    case SfGenerator::SampleId: case SfGenerator::SampleModes:
    case SfGenerator::ScaleTuning: case SfGenerator::ExclusiveClass:
    case SfGenerator::Keynum: case SfGenerator::Velocity:
    case SfGenerator::OverridingRootKey:
        return true;
    default: return isReserved(generator);
    }
}

GenSet readZone(const std::vector<Generator>& gens, std::size_t begin, std::size_t end,
                std::set<std::uint16_t>& unsupported) {
    if (begin > end || end > gens.size()) throw std::runtime_error("invalid SF2 generator range");
    GenSet r;
    for (auto i = begin; i < end; ++i) {
        const auto op = gens[i].op; const auto raw = gens[i].raw; const auto amount = static_cast<std::int16_t>(raw);
        if (op < 61) {
            r.present |= std::uint64_t{1} << op;
            const auto generator = static_cast<SfGenerator>(op);
            r.amount[op] = (generator == SfGenerator::Instrument ||
                generator == SfGenerator::KeyRange || generator == SfGenerator::VelRange ||
                generator == SfGenerator::SampleId || generator == SfGenerator::SampleModes)
                ? raw : amount;
        }
        if (!isSupported(op)) {
            unsupported.insert(op);
        }
    }
    return r;
}

double timecents(int value) {
    if (value <= -32768) return 0.0;
    return std::exp2(std::clamp(value, -12000, 8000) / 1200.0);
}

double volumeTimecents(int value) {
    // Match V3's volume-envelope near-zero pin. Its SF2 default -12000
    // begins immediately instead of delaying the first sample by ~1 ms.
    if (value < -11950) return 0.0;
    return timecents(value);
}

} // namespace

PreparedSoundFont loadSoundFont(const Path& path) {
#if SVMS_CANONICAL_FILESYSTEM_TS
    std::ifstream file(path.c_str(), std::ios::binary);
#else
    std::ifstream file(path, std::ios::binary);
#endif
    if (!file) throw std::runtime_error("cannot open SoundFont: " + path.string());
    file.seekg(0, std::ios::end); const auto size = file.tellg(); file.seekg(0);
    if (size < 12) throw std::runtime_error("SoundFont is too small");
    std::vector<std::uint8_t> b(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(b.data()), size)) throw std::runtime_error("failed reading SoundFont");
    if (!idAt(b, 0, "RIFF") || !idAt(b, 8, "sfbk") || u32(b, 4) > b.size() - 8) {
        throw std::runtime_error("not a valid RIFF sfbk file");
    }
    const auto pdta = findList(b, "pdta"), sdta = findList(b, "sdta");
    const auto phdrC = findChunk(b, pdta, "phdr"), pbagC = findChunk(b, pdta, "pbag");
    const auto pgenC = findChunk(b, pdta, "pgen"), instC = findChunk(b, pdta, "inst");
    const auto ibagC = findChunk(b, pdta, "ibag"), igenC = findChunk(b, pdta, "igen");
    const auto pmodC = findOptionalChunk(b, pdta, "pmod"), imodC = findOptionalChunk(b, pdta, "imod");
    const auto shdrC = findChunk(b, pdta, "shdr"), smplC = findChunk(b, sdta, "smpl");
    auto requireMultiple = [](Chunk c, std::size_t n, const char* name) {
        if (c.size < n || c.size % n != 0) throw std::runtime_error(std::string("invalid ") + name + " chunk size");
    };
    requireMultiple(phdrC, 38, "phdr"); requireMultiple(pbagC, 4, "pbag");
    requireMultiple(pgenC, 4, "pgen"); requireMultiple(instC, 22, "inst");
    requireMultiple(ibagC, 4, "ibag"); requireMultiple(igenC, 4, "igen");
    if (pmodC.size != 0) requireMultiple(pmodC, 10, "pmod");
    if (imodC.size != 0) requireMultiple(imodC, 10, "imod");
    requireMultiple(shdrC, 46, "shdr");
    if (smplC.size < 2 || smplC.size % 2) throw std::runtime_error("invalid smpl chunk size");

    std::vector<Header> phdr, inst;
    for (std::size_t p = phdrC.data; p < phdrC.data + phdrC.size; p += 38)
        phdr.push_back({name20(b, p), u16(b, p + 20), u16(b, p + 22), u16(b, p + 24)});
    for (std::size_t p = instC.data; p < instC.data + instC.size; p += 22)
        inst.push_back({name20(b, p), 0, 0, u16(b, p + 20)});
    auto readBags = [&](Chunk c) { std::vector<Bag> v; for (std::size_t p=c.data;p<c.data+c.size;p+=4) v.push_back({u16(b,p),u16(b,p+2)}); return v; };
    auto readGens = [&](Chunk c) { std::vector<Generator> v; for (std::size_t p=c.data;p<c.data+c.size;p+=4) v.push_back({u16(b,p),u16(b,p+2)}); return v; };
    // Modulators are converted once into pointer-free immutable numeric rows.
    auto readMods = [&](Chunk c) { std::vector<PreparedModulator> v; for (std::size_t p=c.data;p<c.data+c.size;p+=10) v.push_back({u16(b,p),u16(b,p+2),s16(b,p+4),u16(b,p+6),u16(b,p+8)}); return v; };
    const auto pbags = readBags(pbagC), ibags = readBags(ibagC);
    const auto pgens = readGens(pgenC), igens = readGens(igenC);
    const auto pmods = readMods(pmodC), imods = readMods(imodC);
    std::vector<SampleHeader> shdr;
    for (std::size_t p = shdrC.data; p < shdrC.data + shdrC.size; p += 46) {
        shdr.push_back({name20(b,p),u32(b,p+20),u32(b,p+24),u32(b,p+28),u32(b,p+32),
                        u32(b,p+36),b[p+40],static_cast<std::int8_t>(b[p+41]),u16(b,p+44)});
    }
    if (phdr.size() < 2 || inst.size() < 2 || shdr.size() < 2) throw std::runtime_error("SF2 terminal records missing");
    std::vector<std::int16_t> pcm(smplC.size / 2);
    for (std::size_t i=0;i<pcm.size();++i) pcm[i] = s16(b, smplC.data + i*2);
    PreparedSoundFont result;
    result.warnings_.push_back(
        "SF2 default velocity-to-attenuation modulation is intentionally replaced by "
        "engine velocityGain=(velocity/127)^2; the two curves are not stacked");
    result.warnings_.push_back(
        "SF2 CC7/CC10/CC11 and pitch bend defaults are implemented by canonical "
        "channel semantics and are not stacked as duplicate modulators");
    const auto storage = result.samples_.appendPcm16(pcm);
    std::set<std::uint16_t> unsupported;
    std::set<std::string> unsupportedModulators;
    std::map<std::tuple<std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,bool>,SampleId> sampleIds;
    const auto mergeMods = [](std::vector<PreparedModulator> base,
                              const std::vector<PreparedModulator>& local) {
        for (const auto& mod : local) {
            const auto found = std::find_if(base.begin(), base.end(), [&](const auto& value) {
                return value.source == mod.source && value.destination == mod.destination &&
                    value.amountSource == mod.amountSource && value.transform == mod.transform;
            });
            if (found == base.end()) base.push_back(mod); else *found = mod;
        }
        return base;
    };

    for (std::size_t pi=0; pi+1<phdr.size(); ++pi) {
        if (phdr[pi].value > 127) {
            result.warnings_.push_back("preset with invalid program skipped: " + phdr[pi].name);
            continue;
        }
        if (phdr[pi].bag > phdr[pi+1].bag || phdr[pi+1].bag >= pbags.size()) throw std::runtime_error("invalid preset bag index");
        PreparedPreset preset;
        preset.bank = phdr[pi].bank;
        preset.program = static_cast<std::uint8_t>(phdr[pi].value);
        preset.name = phdr[pi].name;
        GenSet pglobal;
        std::vector<PreparedModulator> pglobalMods;
        for (std::size_t pz=phdr[pi].bag; pz<phdr[pi+1].bag; ++pz) {
            if (pbags[pz].gen > pbags[pz+1].gen || pbags[pz+1].gen > pgens.size()) throw std::runtime_error("invalid pgen index");
            if (pbags[pz].mod > pbags[pz+1].mod || pbags[pz+1].mod > pmods.size()) throw std::runtime_error("invalid pmod index");
            auto pzone = readZone(pgens, pbags[pz].gen, pbags[pz+1].gen, unsupported);
            std::vector<PreparedModulator> pzoneMods(pmods.begin()+pbags[pz].mod,
                pmods.begin()+pbags[pz+1].mod);
            const int pzoneInstrument = get(pzone, SfGenerator::Instrument, -1);
            if (pzoneInstrument < 0) { if (pz == phdr[pi].bag) { pglobal = pzone; pglobalMods=std::move(pzoneMods); } continue; }
            auto pcombined = overrideZone(pglobal, pzone);
            const auto pcombinedMods = mergeMods(pglobalMods, pzoneMods);
            const int instrumentIndex = get(pcombined, SfGenerator::Instrument, -1);
            if (instrumentIndex < 0 || static_cast<std::size_t>(instrumentIndex + 1) >= inst.size()) continue;
            const auto ii = static_cast<std::size_t>(instrumentIndex);
            if (inst[ii].bag > inst[ii+1].bag || inst[ii+1].bag >= ibags.size()) throw std::runtime_error("invalid instrument bag index");
            GenSet iglobal;
            std::vector<PreparedModulator> iglobalMods;
            for (std::size_t iz=inst[ii].bag; iz<inst[ii+1].bag; ++iz) {
                if (ibags[iz].gen > ibags[iz+1].gen || ibags[iz+1].gen > igens.size()) throw std::runtime_error("invalid igen index");
                if (ibags[iz].mod > ibags[iz+1].mod || ibags[iz+1].mod > imods.size()) throw std::runtime_error("invalid imod index");
                auto izone = readZone(igens, ibags[iz].gen, ibags[iz+1].gen, unsupported);
                std::vector<PreparedModulator> izoneMods(imods.begin()+ibags[iz].mod,
                    imods.begin()+ibags[iz+1].mod);
                const int zoneSample = get(izone, SfGenerator::SampleId, -1);
                if (zoneSample < 0) { if (iz == inst[ii].bag) { iglobal = izone; iglobalMods=std::move(izoneMods); } continue; }
                const auto instrumentZone = overrideZone(iglobal, izone);
                const auto instrumentMods = mergeMods(iglobalMods, izoneMods);
                const int sampleIndex = get(instrumentZone, SfGenerator::SampleId, -1);
                const auto range = [](const GenSet& set, SfGenerator generator) {
                    const int packed = get(set, generator, 0x7f00);
                    return std::pair{packed & 255, (packed >> 8) & 255};
                };
                const auto [presetKeyLo, presetKeyHi] = range(pcombined, SfGenerator::KeyRange);
                const auto [instrumentKeyLo, instrumentKeyHi] = range(instrumentZone, SfGenerator::KeyRange);
                const auto [presetVelLo, presetVelHi] = range(pcombined, SfGenerator::VelRange);
                const auto [instrumentVelLo, instrumentVelHi] = range(instrumentZone, SfGenerator::VelRange);
                const int keyLo = std::max(presetKeyLo, instrumentKeyLo);
                const int keyHi = std::min(presetKeyHi, instrumentKeyHi);
                const int velLo = std::max(presetVelLo, instrumentVelLo);
                const int velHi = std::min(presetVelHi, instrumentVelHi);
                if (keyLo > keyHi || velLo > velHi || sampleIndex < 0 ||
                    static_cast<std::size_t>(sampleIndex + 1) >= shdr.size()) continue;
                const auto& s = shdr[static_cast<std::size_t>(sampleIndex)];
                if ((s.type & 0x8000U) != 0) { result.warnings_.push_back("ROM sample skipped: " + s.name); continue; }
                if (s.rate == 0) { result.warnings_.push_back("zero-rate sample skipped: " + s.name); continue; }
                const auto sampleOffset = [&](SfGenerator fine, SfGenerator coarse) {
                    return static_cast<std::int64_t>(get(instrumentZone, fine)) +
                        static_cast<std::int64_t>(get(instrumentZone, coarse)) * 32768;
                };
                const auto start64=static_cast<std::int64_t>(s.start)+sampleOffset(SfGenerator::StartAddrsOffset,SfGenerator::StartAddrsCoarseOffset);
                const auto end64=static_cast<std::int64_t>(s.end)+sampleOffset(SfGenerator::EndAddrsOffset,SfGenerator::EndAddrsCoarseOffset);
                const auto ls64=static_cast<std::int64_t>(s.loopStart)+sampleOffset(SfGenerator::StartloopAddrsOffset,SfGenerator::StartloopAddrsCoarseOffset);
                const auto le64=static_cast<std::int64_t>(s.loopEnd)+sampleOffset(SfGenerator::EndloopAddrsOffset,SfGenerator::EndloopAddrsCoarseOffset);
                if (start64 < 0 || end64 <= start64 || static_cast<std::uint64_t>(end64) > pcm.size()) {
                    result.warnings_.push_back("invalid sample range skipped: " + s.name); continue;
                }
                const auto start=static_cast<std::uint32_t>(start64), length=static_cast<std::uint32_t>(end64-start64);
                const int mode = get(instrumentZone, SfGenerator::SampleModes, 0) & 3;
                bool looping = mode == 1 || mode == 3;
                std::uint32_t ls=0, le=length;
                if (looping) {
                    if (ls64 < start64 || le64 <= ls64 || le64 > end64) { result.warnings_.push_back("invalid loop disabled: " + s.name); looping=false; }
                    else { ls=static_cast<std::uint32_t>(ls64-start64); le=static_cast<std::uint32_t>(le64-start64); }
                }
                const auto key=std::tuple{start,length,ls,le,looping};
                auto found=sampleIds.find(key); SampleId id;
                if(found==sampleIds.end()) { id=looping?result.samples_.addLoopView(storage+start,length,ls,le):result.samples_.addOneShotView(storage+start,length); sampleIds.emplace(key,id); } else id=found->second;
                const auto combined = [&](SfGenerator generator, int defaultValue) {
                    return get(instrumentZone, generator, defaultValue) +
                        get(pcombined, generator, 0);
                };
                PreparedRegion r; r.sample=id; r.keyLow=static_cast<std::uint8_t>(keyLo); r.keyHigh=static_cast<std::uint8_t>(keyHi);
                r.velocityLow=static_cast<std::uint8_t>(velLo); r.velocityHigh=static_cast<std::uint8_t>(velHi);
                const int headerRoot = s.pitch <= 127 ? s.pitch : 60;
                const int root = get(instrumentZone, SfGenerator::OverridingRootKey, -1);
                r.rootKey=static_cast<std::uint8_t>(root>=0?std::clamp(root,0,127):headerRoot);
                r.sampleRate=static_cast<float>(s.rate);
                r.scaleTuning=static_cast<float>(combined(SfGenerator::ScaleTuning,100));
                r.tuningCents=static_cast<float>(combined(SfGenerator::CoarseTune,0)*100+
                    combined(SfGenerator::FineTune,0)+s.correction);
                r.gain=std::pow(10.0F,-std::max(0,combined(SfGenerator::InitialAttenuation,0))/200.0F);
                r.pan=std::clamp(combined(SfGenerator::Pan,0)/500.0F,-1.0F,1.0F);
                const int sustainCb = std::clamp(combined(SfGenerator::SustainVolEnv,0),0,1440);
                r.delaySeconds=static_cast<float>(volumeTimecents(combined(SfGenerator::DelayVolEnv,-12000)));
                r.attackSeconds=static_cast<float>(volumeTimecents(combined(SfGenerator::AttackVolEnv,-12000)));
                r.holdSeconds=static_cast<float>(volumeTimecents(combined(SfGenerator::HoldVolEnv,-12000)));
                r.decaySeconds=static_cast<float>(volumeTimecents(combined(SfGenerator::DecayVolEnv,-12000)) *
                    std::min(sustainCb,1000) / 1000.0);
                r.sustainGain=std::pow(10.0F,-sustainCb/200.0F);
                r.releaseSeconds=static_cast<float>(volumeTimecents(combined(SfGenerator::ReleaseVolEnv,-12000)));
                r.keynumToHoldTimecents=static_cast<float>(combined(SfGenerator::KeynumToVolEnvHold,0));
                r.keynumToDecayTimecents=static_cast<float>(combined(SfGenerator::KeynumToVolEnvDecay,0));
                r.logarithmicEnvelope=true;
                r.filterCutoffCents=static_cast<std::int16_t>(std::clamp(
                    combined(SfGenerator::InitialFilterFc,13500),-12000,13500));
                r.filterResonanceCentibels=static_cast<std::int16_t>(std::clamp(
                    combined(SfGenerator::InitialFilterQ,0),0,960));
                r.sampleMode=static_cast<std::uint8_t>(looping ? mode : 0);
                r.exclusiveClass=static_cast<std::uint8_t>(std::clamp(
                    get(instrumentZone,SfGenerator::ExclusiveClass,0),0,127));
                r.forcedKey=static_cast<std::int16_t>(std::clamp(
                    get(instrumentZone,SfGenerator::Keynum,-1),-1,127));
                r.forcedVelocity=static_cast<std::int16_t>(std::clamp(
                    get(instrumentZone,SfGenerator::Velocity,-1),-1,127));
                r.modLfoDelaySeconds=static_cast<float>(timecents(
                    combined(SfGenerator::DelayModLFO,-12000)));
                r.vibLfoDelaySeconds=static_cast<float>(timecents(
                    combined(SfGenerator::DelayVibLFO,-12000)));
                r.modLfoFrequencyHz=8.176F*std::exp2(
                    combined(SfGenerator::FreqModLFO,0)/1200.0F);
                r.vibLfoFrequencyHz=8.176F*std::exp2(
                    combined(SfGenerator::FreqVibLFO,0)/1200.0F);
                r.modLfoToPitchCents=static_cast<float>(combined(SfGenerator::ModLfoToPitch,0));
                r.vibLfoToPitchCents=static_cast<float>(combined(SfGenerator::VibLfoToPitch,0));
                r.modLfoToFilterCents=static_cast<float>(combined(SfGenerator::ModLfoToFilterFc,0));
                r.modLfoToVolumeCentibels=static_cast<float>(combined(SfGenerator::ModLfoToVolume,0));
                r.modEnvDelaySeconds=static_cast<float>(timecents(combined(SfGenerator::DelayModEnv,-12000)));
                r.modEnvAttackSeconds=static_cast<float>(timecents(combined(SfGenerator::AttackModEnv,-12000)));
                r.modEnvHoldSeconds=static_cast<float>(timecents(combined(SfGenerator::HoldModEnv,-12000)));
                r.modEnvDecaySeconds=static_cast<float>(timecents(combined(SfGenerator::DecayModEnv,-12000)));
                r.modEnvReleaseSeconds=static_cast<float>(timecents(combined(SfGenerator::ReleaseModEnv,-12000)));
                r.modEnvSustain=1.0F-std::clamp(combined(SfGenerator::SustainModEnv,0),0,1000)/1000.0F;
                r.modEnvKeynumToHoldTimecents=static_cast<float>(combined(SfGenerator::KeynumToModEnvHold,0));
                r.modEnvKeynumToDecayTimecents=static_cast<float>(combined(SfGenerator::KeynumToModEnvDecay,0));
                r.modEnvToPitchCents=static_cast<float>(combined(SfGenerator::ModEnvToPitch,0));
                r.modEnvToFilterCents=static_cast<float>(combined(SfGenerator::ModEnvToFilterFc,0));
                const auto acceptsSource = [](std::uint16_t source) {
                    if (source == 0) return true;
                    const auto index = source & 0x7fU;
                    return (source & 0x80U) == 0 && (source >> 10) == 0 &&
                        (index == 2 || index == 3);
                };
                const auto acceptsDestination = [](std::uint16_t destination) {
                    switch (destination) {
                    case 5: case 6: case 7: case 8: case 9: case 10: case 11:
                    case 13: case 17: case 21: case 22: case 23: case 24:
                    case 25: case 26: case 27: case 28: case 29: case 30:
                    case 31: case 32: case 33: case 34: case 35: case 36:
                    case 37: case 38: case 48: case 51: case 52: return true;
                    default: return false;
                    }
                };
                const auto addModulator = [&](const PreparedModulator& mod,
                                              bool instrumentLevel) {
                    const bool velocityFilterDefault = instrumentLevel &&
                        mod.source == 0x0102 &&
                        mod.destination == 8 && mod.amountSource == 0 && mod.transform == 0;
                    if (velocityFilterDefault) {
                        r.defaultVelocityToFilterCents = static_cast<float>(mod.amount);
                        return;
                    }
                    const bool modWheelVibratoDefault = instrumentLevel &&
                        mod.source == 0x0081 &&
                        mod.destination == 6 && mod.amountSource == 0 && mod.transform == 0;
                    if (modWheelVibratoDefault) {
                        r.defaultModWheelToVibPitchCents = static_cast<float>(mod.amount);
                        return;
                    }
                    if (!acceptsSource(mod.source) || !acceptsSource(mod.amountSource) ||
                        !acceptsDestination(mod.destination) ||
                        (mod.transform != 0 && mod.transform != 2)) {
                        unsupportedModulators.insert("source=" + std::to_string(mod.source) +
                            " destination=" + std::to_string(mod.destination) +
                            " amountSource=" + std::to_string(mod.amountSource) +
                            " transform=" + std::to_string(mod.transform));
                        return;
                    }
                    r.modulators.push_back(mod);
                };
                for (const auto& mod : instrumentMods) addModulator(mod, true);
                for (const auto& mod : pcombinedMods) addModulator(mod, false);
                preset.regions.push_back(r);
            }
        }
        if (!preset.regions.empty()) result.presets_.push_back(std::move(preset));
    }
    if (!unsupported.empty()) {
        std::string warning = "SF2 recognized but not rendered generators:";
        for (const auto generator : unsupported) {
            warning += "\n  ";
            warning += generatorName(generator);
            warning += " (" + std::to_string(generator) + ")";
        }
        result.warnings_.push_back(std::move(warning));
    }
    if (!unsupportedModulators.empty()) {
        std::string warning = "SF2 explicit modulators not rendered by the supported static subset:";
        for (const auto& mod : unsupportedModulators) warning += "\n  " + mod;
        result.warnings_.push_back(std::move(warning));
    }
    if (result.presets_.empty()) throw std::runtime_error("SoundFont contains no usable presets");
    result.finalize();
    return result;
}

PreparedSoundFont loadSoundFont(const std::string& path) {
    return loadSoundFont(Path(path));
}

} // namespace svms::canonical
