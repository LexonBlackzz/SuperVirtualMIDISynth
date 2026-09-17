#include "midisynth/soundfont.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace midisynth {

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

std::span<const std::uint32_t> PreparedSoundFont::matchingRegions(
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
    for (const auto index : font.matchingRegions(preset, key, velocity)) {
        const auto& r = preset.regions[index];
        const double cents = (static_cast<int>(key) - r.rootKey) * r.scaleTuning +
            r.tuningCents;
        const float increment = static_cast<float>(r.sampleRate / outputSampleRate *
            std::exp2(cents / 1200.0));
        const float pan = std::clamp(r.pan, -1.0F, 1.0F);
        const float angle = (pan + 1.0F) * (std::numbers::pi_v<float> * 0.25F);
        const float normalizedVelocity = static_cast<float>(velocity) / 127.0F;
        const float velocityGain = normalizedVelocity * normalizedVelocity;
        const auto frames = [&](double seconds) {
            return static_cast<std::uint32_t>(std::clamp(
                std::llround(seconds * outputSampleRate), 0LL,
                static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));
        };
        const double hold = r.holdSeconds == 0.0F ? 0.0 :
            r.holdSeconds * std::exp2(r.keynumToHoldTimecents *
                (60.0 - static_cast<double>(key)) / 1200.0);
        const double decay = r.decaySeconds == 0.0F ? 0.0 :
            r.decaySeconds * std::exp2(r.keynumToDecayTimecents *
                (60.0 - static_cast<double>(key)) / 1200.0);
        auto event = Event::noteOn(frame, sequence++, key, r.sample, increment,
            r.gain * velocityGain * std::cos(angle),
            r.gain * velocityGain * std::sin(angle),
            frames(r.attackSeconds), frames(r.releaseSeconds), channel);
        event.delayFrames = frames(r.delaySeconds);
        event.holdFrames = frames(hold);
        event.decayFrames = frames(decay);
        event.sustainGain = r.sustainGain;
        event.noteInstance = noteInstance;
        event.logarithmicEnvelope = r.logarithmicEnvelope;
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
    case SfGenerator::ScaleTuning: case SfGenerator::OverridingRootKey:
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

} // namespace

PreparedSoundFont loadSoundFont(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open SoundFont: " + path);
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
    const auto pbags = readBags(pbagC), ibags = readBags(ibagC);
    const auto pgens = readGens(pgenC), igens = readGens(igenC);
    std::vector<SampleHeader> shdr;
    for (std::size_t p = shdrC.data; p < shdrC.data + shdrC.size; p += 46) {
        shdr.push_back({name20(b,p),u32(b,p+20),u32(b,p+24),u32(b,p+28),u32(b,p+32),
                        u32(b,p+36),b[p+40],static_cast<std::int8_t>(b[p+41]),u16(b,p+44)});
    }
    if (phdr.size() < 2 || inst.size() < 2 || shdr.size() < 2) throw std::runtime_error("SF2 terminal records missing");
    std::vector<float> pcm(smplC.size / 2);
    for (std::size_t i=0;i<pcm.size();++i) pcm[i] = s16(b, smplC.data + i*2) / 32768.0F;
    PreparedSoundFont result;
    result.warnings_.push_back(
        "SF2 default velocity-to-attenuation modulation is intentionally replaced by "
        "engine velocityGain=(velocity/127)^2; the two curves are not stacked");
    result.warnings_.push_back(
        "SF2 default modulators other than independently handled MIDI pitch bend "
        "are not rendered");
    if (pmodC.size > 10 || imodC.size > 10) {
        result.warnings_.push_back(
            "SF2 explicit pmod/imod modulators are present but not rendered");
    }
    const auto storage = result.samples_.appendSamples(pcm);
    std::set<std::uint16_t> unsupported;
    bool releaseLoopUnsupported = false;
    std::map<std::tuple<std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,bool>,SampleId> sampleIds;

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
        for (std::size_t pz=phdr[pi].bag; pz<phdr[pi+1].bag; ++pz) {
            if (pbags[pz].gen > pbags[pz+1].gen || pbags[pz+1].gen > pgens.size()) throw std::runtime_error("invalid pgen index");
            auto pzone = readZone(pgens, pbags[pz].gen, pbags[pz+1].gen, unsupported);
            const int pzoneInstrument = get(pzone, SfGenerator::Instrument, -1);
            if (pzoneInstrument < 0) { if (pz == phdr[pi].bag) pglobal = pzone; continue; }
            auto pcombined = overrideZone(pglobal, pzone);
            const int instrumentIndex = get(pcombined, SfGenerator::Instrument, -1);
            if (instrumentIndex < 0 || static_cast<std::size_t>(instrumentIndex + 1) >= inst.size()) continue;
            const auto ii = static_cast<std::size_t>(instrumentIndex);
            if (inst[ii].bag > inst[ii+1].bag || inst[ii+1].bag >= ibags.size()) throw std::runtime_error("invalid instrument bag index");
            GenSet iglobal;
            for (std::size_t iz=inst[ii].bag; iz<inst[ii+1].bag; ++iz) {
                if (ibags[iz].gen > ibags[iz+1].gen || ibags[iz+1].gen > igens.size()) throw std::runtime_error("invalid igen index");
                auto izone = readZone(igens, ibags[iz].gen, ibags[iz+1].gen, unsupported);
                const int zoneSample = get(izone, SfGenerator::SampleId, -1);
                if (zoneSample < 0) { if (iz == inst[ii].bag) iglobal = izone; continue; }
                const auto instrumentZone = overrideZone(iglobal, izone);
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
                if (mode == 3) releaseLoopUnsupported = true;
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
                r.delaySeconds=static_cast<float>(timecents(combined(SfGenerator::DelayVolEnv,-12000)));
                r.attackSeconds=static_cast<float>(timecents(combined(SfGenerator::AttackVolEnv,-12000)));
                r.holdSeconds=static_cast<float>(timecents(combined(SfGenerator::HoldVolEnv,-12000)));
                r.decaySeconds=static_cast<float>(timecents(combined(SfGenerator::DecayVolEnv,-12000)) *
                    std::min(sustainCb,1000) / 1000.0);
                r.sustainGain=std::pow(10.0F,-sustainCb/200.0F);
                r.releaseSeconds=static_cast<float>(timecents(combined(SfGenerator::ReleaseVolEnv,-12000)));
                r.keynumToHoldTimecents=static_cast<float>(combined(SfGenerator::KeynumToVolEnvHold,0));
                r.keynumToDecayTimecents=static_cast<float>(combined(SfGenerator::KeynumToVolEnvDecay,0));
                r.logarithmicEnvelope=true;
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
    if (releaseLoopUnsupported) {
        result.warnings_.push_back(
            "SF2 sampleModes=3 is present; release-loop exit is unsupported and "
            "those regions use continuous looping");
    }
    if (result.presets_.empty()) throw std::runtime_error("SoundFont contains no usable presets");
    result.finalize();
    return result;
}

} // namespace midisynth
