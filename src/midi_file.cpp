#include "midisynth/midi_file.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>

namespace midisynth {
namespace {

std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t p) {
    if (p > b.size() || b.size() - p < 2) throw std::runtime_error("truncated MIDI field");
    return static_cast<std::uint16_t>((b[p] << 8) | b[p + 1]);
}
std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t p) {
    if (p > b.size() || b.size() - p < 4) throw std::runtime_error("truncated MIDI field");
    return (static_cast<std::uint32_t>(b[p]) << 24) |
        (static_cast<std::uint32_t>(b[p + 1]) << 16) |
        (static_cast<std::uint32_t>(b[p + 2]) << 8) | b[p + 3];
}
bool hasId(std::span<const std::uint8_t> b, std::size_t p, const char* id) {
    return p <= b.size() && b.size() - p >= 4 &&
        std::equal(id, id + 4, b.begin() + static_cast<std::ptrdiff_t>(p));
}
std::uint32_t readMidiVlq(std::span<const std::uint8_t> b, std::size_t& p, std::size_t end) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (p >= end) throw std::runtime_error("truncated MIDI variable-length value");
        const auto byte = b[p++];
        value = (value << 7) | (byte & 0x7fU);
        if ((byte & 0x80U) == 0) return value;
    }
    throw std::runtime_error("invalid MIDI variable-length value");
}
std::uint64_t readUleb(std::span<const std::uint8_t> data, std::size_t& p) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (p >= data.size()) throw std::runtime_error("truncated compact MIDI varint");
        const auto byte = data[p++];
        if (shift == 63 && (byte & 0xfeU) != 0) {
            throw std::runtime_error("compact MIDI varint overflow");
        }
        value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) return value;
    }
    throw std::runtime_error("compact MIDI varint overflow");
}

enum class RawKind : std::uint8_t {
    NoteOff, NoteOn, ControlChange, ProgramChange, PitchBend, Tempo
};
struct CursorEvent {
    std::uint64_t tick{};
    RawKind kind{};
    std::uint8_t channel{}, data1{}, data2{};
    std::uint32_t tempo{};
};
struct TrackCursor {
    std::span<const std::uint8_t> bytes;
    std::size_t position{}, end{};
    std::uint64_t tick{};
    std::uint32_t track{};
    std::uint64_t eventInTrack{};
    std::uint8_t runningStatus{};
    CursorEvent current{};

    bool advance() {
        while (position < end) {
            const auto delta = readMidiVlq(bytes, position, end);
            if (delta > std::numeric_limits<std::uint64_t>::max() - tick) {
                throw std::runtime_error("MIDI tick position overflow");
            }
            tick += delta;
            if (position >= end) throw std::runtime_error("truncated MIDI event");
            std::uint8_t status = bytes[position++];
            if (status < 0x80) {
                if (runningStatus == 0) throw std::runtime_error("MIDI running status without status");
                --position;
                status = runningStatus;
            }
            if (status == 0xff) {
                runningStatus = 0;
                if (position >= end) throw std::runtime_error("truncated MIDI meta event");
                const auto type = bytes[position++];
                const auto length = readMidiVlq(bytes, position, end);
                if (length > end - position) throw std::runtime_error("MIDI meta event exceeds track");
                if (type == 0x51 && length == 3) {
                    current = {tick, RawKind::Tempo, 0, 0, 0,
                        (static_cast<std::uint32_t>(bytes[position]) << 16) |
                        (static_cast<std::uint32_t>(bytes[position + 1]) << 8) |
                        bytes[position + 2]};
                    position += length;
                    ++eventInTrack;
                    return true;
                }
                position += length;
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {
                runningStatus = 0;
                const auto length = readMidiVlq(bytes, position, end);
                if (length > end - position) throw std::runtime_error("MIDI SysEx exceeds track");
                position += length;
                continue;
            }
            if (status >= 0xf0) throw std::runtime_error("unsupported MIDI system event in SMF");
            runningStatus = status;
            const auto high = status & 0xf0U;
            const auto channel = static_cast<std::uint8_t>(status & 0x0fU);
            const unsigned count = (high == 0xc0 || high == 0xd0) ? 1U : 2U;
            if (count > end - position) throw std::runtime_error("truncated MIDI channel event");
            const auto a = bytes[position++];
            const auto b = static_cast<std::uint8_t>(count == 2 ? bytes[position++] : 0);
            if (a >= 0x80 || b >= 0x80) throw std::runtime_error("MIDI channel data byte has high bit set");
            RawKind kind{};
            bool supported = true;
            if (high == 0x80) kind = RawKind::NoteOff;
            else if (high == 0x90) kind = b == 0 ? RawKind::NoteOff : RawKind::NoteOn;
            else if (high == 0xb0) kind = RawKind::ControlChange;
            else if (high == 0xc0) kind = RawKind::ProgramChange;
            else if (high == 0xe0) kind = RawKind::PitchBend;
            else supported = false;
            if (!supported) continue;
            current = {tick, kind, channel, a, b, 0};
            ++eventInTrack;
            return true;
        }
        return false;
    }
};
struct CursorLater {
    const std::vector<TrackCursor>* cursors{};
    bool operator()(std::size_t left, std::size_t right) const {
        const auto& a = (*cursors)[left];
        const auto& b = (*cursors)[right];
        if (a.current.tick != b.current.tick) return a.current.tick > b.current.tick;
        if (a.track != b.track) return a.track > b.track;
        return a.eventInTrack > b.eventInTrack;
    }
};
std::uint8_t opcode(RawKind kind) {
    switch (kind) {
    case RawKind::NoteOff: return 0x00;
    case RawKind::NoteOn: return 0x10;
    case RawKind::ControlChange: return 0x20;
    case RawKind::ProgramChange: return 0x30;
    case RawKind::PitchBend: return 0x40;
    case RawKind::Tempo: break;
    }
    throw std::logic_error("tempo has no compact opcode");
}

class GroupEncoder {
public:
    explicit GroupEncoder(MidiFile& file) : file_(file) {}
    void append(std::uint64_t frame, const CursorEvent& event) {
        if (!open_ || frame != frame_) {
            finish();
            open_ = true;
            frame_ = frame;
            count_ = 0;
            headerPosition_ = file_.compactData.size();
            file_.compactData.resize(headerPosition_ + reservedHeaderBytes);
        }
        file_.compactData.push_back(static_cast<std::uint8_t>(opcode(event.kind) | event.channel));
        file_.compactData.push_back(event.data1);
        if (event.kind == RawKind::NoteOn || event.kind == RawKind::ControlChange ||
            event.kind == RawKind::PitchBend) {
            file_.compactData.push_back(event.data2);
        }
        ++count_;
        ++file_.storage.logicalMessages;
    }
    void finish() {
        if (!open_) return;
        std::array<std::uint8_t, reservedHeaderBytes> header{};
        std::size_t headerSize = 0;
        auto encode = [&](std::uint64_t value) {
            do {
                auto byte = static_cast<std::uint8_t>(value & 0x7fU);
                value >>= 7;
                if (value != 0) byte |= 0x80U;
                header[headerSize++] = byte;
            } while (value != 0);
        };
        encode(frame_ - previousFrame_);
        encode(count_);
        const auto payloadPosition = headerPosition_ + reservedHeaderBytes;
        const auto payloadBytes = file_.compactData.size() - payloadPosition;
        std::memmove(file_.compactData.data() + headerPosition_ + headerSize,
                     file_.compactData.data() + payloadPosition, payloadBytes);
        std::copy_n(header.begin(), headerSize,
                  file_.compactData.begin() + static_cast<std::ptrdiff_t>(headerPosition_));
        file_.compactData.resize(headerPosition_ + headerSize + payloadBytes);
        file_.storage.timingAndGroupBytes += headerSize;
        file_.storage.messagePayloadBytes += payloadBytes;
        previousFrame_ = frame_;
        open_ = false;
    }
private:
    static constexpr std::size_t reservedHeaderBytes = 20;
    MidiFile& file_;
    std::size_t headerPosition_{};
    std::uint64_t previousFrame_{}, frame_{}, count_{};
    bool open_{};
};

} // namespace

CompactMidiReader::CompactMidiReader(std::span<const std::uint8_t> data) : data_(data) {}
bool CompactMidiReader::next(MidiMessage& message) {
    if (remainingInGroup_ == 0) {
        if (position_ == data_.size()) return false;
        const auto delta = readUleb(data_, position_);
        if (sawGroup_ && delta == 0) throw std::runtime_error("compact MIDI contains duplicate frame groups");
        if (delta > std::numeric_limits<std::uint64_t>::max() - frame_) {
            throw std::runtime_error("compact MIDI frame delta overflow");
        }
        frame_ += delta;
        remainingInGroup_ = readUleb(data_, position_);
        if (remainingInGroup_ == 0) throw std::runtime_error("compact MIDI group is empty");
        if (remainingInGroup_ > (data_.size() - position_) / 2) {
            throw std::runtime_error("compact MIDI group count exceeds remaining data");
        }
        sawGroup_ = true;
    }
    if (data_.size() - position_ < 2) throw std::runtime_error("truncated compact MIDI message");
    const auto tag = data_[position_++];
    const auto operation = tag >> 4;
    message = {};
    message.frame = frame_;
    message.order = nextOrder_++;
    message.channel = tag & 0x0fU;
    message.data1 = data_[position_++];
    if (message.data1 >= 0x80) throw std::runtime_error("invalid compact MIDI data byte");
    if (operation == 0) message.type = MidiMessageType::NoteOff;
    else if (operation == 1) message.type = MidiMessageType::NoteOn;
    else if (operation == 2) message.type = MidiMessageType::ControlChange;
    else if (operation == 3) message.type = MidiMessageType::ProgramChange;
    else if (operation == 4) message.type = MidiMessageType::PitchBend;
    else throw std::runtime_error("unknown compact MIDI opcode");
    if (message.type == MidiMessageType::NoteOn || message.type == MidiMessageType::ControlChange ||
        message.type == MidiMessageType::PitchBend) {
        if (position_ == data_.size()) throw std::runtime_error("truncated compact MIDI message");
        message.data2 = data_[position_++];
        if (message.data2 >= 0x80) throw std::runtime_error("invalid compact MIDI data byte");
    }
    --remainingInGroup_;
    return true;
}

MidiFile loadMidiFile(const std::string& path, double sampleRate) {
    if (!(sampleRate > 0.0)) throw std::invalid_argument("sample rate must be positive");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open MIDI: " + path);
    input.seekg(0, std::ios::end);
    const auto streamSize = input.tellg();
    input.seekg(0);
    if (streamSize < 14) throw std::runtime_error("MIDI file is too small");
    if (static_cast<std::uintmax_t>(streamSize) > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("MIDI file is too large for this process");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(streamSize));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), streamSize)) throw std::runtime_error("failed reading MIDI");
    const std::span<const std::uint8_t> view(bytes);
    if (!hasId(view, 0, "MThd")) throw std::runtime_error("invalid MIDI header");
    const auto headerLength = be32(view, 4);
    if (headerLength < 6 || headerLength > bytes.size() - 8) throw std::runtime_error("invalid MIDI header length");
    const auto format = be16(view, 8), trackCount = be16(view, 10), division = be16(view, 12);
    if (format > 1) throw std::runtime_error("SMF format 2 is unsupported");
    if (format == 0 && trackCount != 1) throw std::runtime_error("SMF format 0 must contain one track");
    if ((division & 0x8000U) != 0 || division == 0) throw std::runtime_error("SMPTE MIDI time division is unsupported");

    std::vector<TrackCursor> cursors;
    cursors.reserve(trackCount);
    std::size_t position = 8 + headerLength;
    for (std::uint16_t track = 0; track < trackCount; ++track) {
        if (position > bytes.size() || bytes.size() - position < 8 || !hasId(view, position, "MTrk")) {
            throw std::runtime_error("missing MIDI track chunk");
        }
        const auto length = be32(view, position + 4);
        position += 8;
        if (length > bytes.size() - position) throw std::runtime_error("MIDI track exceeds file");
        const auto end = position + length;
        cursors.push_back({view, position, end, 0, track});
        position = end;
    }

    MidiFile result;
    result.storage.sourceBytes = bytes.size();
    result.compactData.reserve(bytes.size());
    CursorLater later{&cursors};
    std::priority_queue<std::size_t, std::vector<std::size_t>, CursorLater> pending(later);
    for (std::size_t i = 0; i < cursors.size(); ++i) if (cursors[i].advance()) pending.push(i);
    GroupEncoder encoder(result);
    std::uint64_t previousTick = 0;
    std::uint32_t tempo = 500000;
    long double microseconds = 0;
    std::set<std::uint8_t> unsupportedControllers;
    while (!pending.empty()) {
        const auto index = pending.top();
        pending.pop();
        auto& cursor = cursors[index];
        const auto event = cursor.current;
        microseconds += static_cast<long double>(event.tick - previousTick) * tempo / division;
        previousTick = event.tick;
        const auto exactFrame = microseconds * sampleRate / 1000000.0L;
        if (exactFrame < 0 || exactFrame > static_cast<long double>(
                std::numeric_limits<long long>::max())) {
            throw std::runtime_error("MIDI frame conversion overflow");
        }
        const auto rounded = std::llround(exactFrame);
        const auto frame = static_cast<std::uint64_t>(rounded);
        result.lastFrame = std::max(result.lastFrame, frame);
        if (event.kind == RawKind::Tempo) {
            if (event.tempo != 0) tempo = event.tempo;
        } else {
            if (event.kind == RawKind::ControlChange) {
                switch (event.data1) {
                case 0: case 6: case 7: case 10: case 11: case 32: case 38:
                case 64: case 100: case 101: case 120: case 121: case 123:
                    break;
                default: unsupportedControllers.insert(event.data1); break;
                }
            }
            encoder.append(frame, event);
        }
        if (cursor.advance()) pending.push(index);
    }
    encoder.finish();
    result.storage.compactBytes = result.compactData.size();
    const auto cursorBytes = cursors.capacity() * sizeof(TrackCursor) + trackCount * sizeof(std::size_t);
    result.storage.peakParserWorkingBytes = bytes.capacity() + result.compactData.capacity() + cursorBytes;
    struct LegacyRawLayout {
        std::uint64_t tick; std::uint32_t order; int kind;
        std::uint8_t channel, data1, data2; std::uint32_t tempo;
    };
    struct LegacyMessageLayout {
        std::uint64_t frame; std::uint32_t order; MidiMessageType type;
        std::uint8_t channel, data1, data2;
    };
    result.storage.legacyEstimatedPeakBytes = bytes.size() +
        result.storage.logicalMessages *
            (sizeof(LegacyRawLayout) + sizeof(LegacyMessageLayout));
    for (const auto controller : unsupportedControllers) {
        result.warnings.push_back("unsupported MIDI controller ignored: CC " + std::to_string(controller));
    }
    return result;
}

PreparedPerformance preparePerformance(const MidiFile& midi, const PreparedSoundFont& font,
                                       double sampleRate) {
    struct KeyInstance { std::uint32_t id{}; bool held{}; };
    struct Channel {
        std::uint8_t msb{}, lsb{}, program{};
        std::uint8_t rpnMsb{127}, rpnLsb{127}, dataMsb{2}, dataLsb{};
        bool sustain{};
        std::array<std::deque<KeyInstance>, 128> keys;
    };
    std::array<Channel, 16> channels{};
    PreparedPerformance output;
    output.lastFrame = midi.lastFrame;
    output.warnings = midi.warnings;
    std::uint32_t sequence = 0;
    std::uint32_t nextNoteInstance = 1;
    std::set<std::pair<std::uint16_t, std::uint8_t>> missing;
    auto findPreset = [&](std::uint8_t ch, const Channel& c) -> const PreparedPreset* {
        const auto bank = static_cast<std::uint16_t>((c.msb << 7) | c.lsb);
        if (ch == 9 && bank == 0) if (auto* p = font.findPreset(128, c.program)) return p;
        if (auto* p = font.findPreset(bank, c.program)) return p;
        return bank != 0 ? font.findPreset(0, c.program) : nullptr;
    };
    auto reader = midi.reader();
    MidiMessage message;
    auto releaseDeferred = [&](std::uint8_t channel, Channel& state,
                               std::uint64_t frame) {
        for (std::uint16_t note = 0; note < 128; ++note) {
            auto& instances = state.keys[note];
            for (auto it = instances.begin(); it != instances.end();) {
                if (!it->held) {
                    output.events.push_back(Event::noteOff(frame, sequence++,
                        static_cast<std::uint8_t>(note), channel, it->id));
                    it = instances.erase(it);
                } else {
                    ++it;
                }
            }
        }
    };
    auto noteOff = [&](std::uint8_t channel, Channel& state, std::uint8_t note,
                       std::uint64_t frame) {
        auto& instances = state.keys[note];
        const auto it = std::find_if(instances.begin(), instances.end(),
            [](const KeyInstance& instance) { return instance.held; });
        if (it == instances.end()) return;
        it->held = false;
        if (!state.sustain) {
            output.events.push_back(Event::noteOff(frame, sequence++, note, channel, it->id));
            instances.erase(it);
        }
    };
    while (reader.next(message)) {
        auto& c = channels[message.channel];
        switch (message.type) {
        case MidiMessageType::ProgramChange: c.program = message.data1; break;
        case MidiMessageType::ControlChange:
            if (message.data1 == 0) c.msb = message.data2;
            else if (message.data1 == 32) c.lsb = message.data2;
            else if (message.data1 == 7) output.events.push_back(Event::channelControl(
                message.frame, sequence++, EventType::ChannelVolume, message.channel, message.data2));
            else if (message.data1 == 10) output.events.push_back(Event::channelControl(
                message.frame, sequence++, EventType::ChannelPan, message.channel, message.data2));
            else if (message.data1 == 11) output.events.push_back(Event::channelControl(
                message.frame, sequence++, EventType::ChannelExpression, message.channel, message.data2));
            else if (message.data1 == 64) {
                const bool down = message.data2 >= 64;
                if (c.sustain && !down) releaseDeferred(message.channel, c, message.frame);
                c.sustain = down;
            } else if (message.data1 == 101) c.rpnMsb = message.data2;
            else if (message.data1 == 100) c.rpnLsb = message.data2;
            else if (message.data1 == 6 || message.data1 == 38) {
                if (message.data1 == 6) c.dataMsb = message.data2;
                else c.dataLsb = message.data2;
                if (c.rpnMsb == 0 && c.rpnLsb == 0) {
                    const auto cents = static_cast<std::uint16_t>(c.dataMsb * 100 +
                        std::min<std::uint8_t>(c.dataLsb, 99));
                    output.events.push_back(Event::channelControl(message.frame, sequence++,
                        EventType::PitchBendRange, message.channel, cents));
                }
            } else if (message.data1 == 120) {
                output.events.push_back(Event::channelControl(message.frame, sequence++,
                    EventType::AllSoundOff, message.channel, 0));
                for (auto& instances : c.keys) instances.clear();
            } else if (message.data1 == 121) {
                if (c.sustain) releaseDeferred(message.channel, c, message.frame);
                c.sustain = false;
                c.rpnMsb = c.rpnLsb = 127;
                c.dataMsb = 2; c.dataLsb = 0;
                output.events.push_back(Event::channelControl(message.frame, sequence++,
                    EventType::ResetControllers, message.channel, 0));
            } else if (message.data1 == 123) {
                for (std::uint16_t note = 0; note < 128; ++note) {
                    for (auto& instance : c.keys[note]) instance.held = false;
                }
                if (!c.sustain) releaseDeferred(message.channel, c, message.frame);
            }
            break;
        case MidiMessageType::NoteOn: {
            ++output.sourceNoteOns;
            const auto instance = nextNoteInstance++;
            if (nextNoteInstance == 0) ++nextNoteInstance;
            c.keys[message.data1].push_back({instance, true});
            const auto* preset = findPreset(message.channel, c);
            if (preset) appendPreparedNoteOn(output.events, *preset, font, message.frame, sequence,
                message.channel, message.data1, message.data2, sampleRate, instance);
            else missing.emplace(static_cast<std::uint16_t>((c.msb << 7) | c.lsb), c.program);
            break;
        }
        case MidiMessageType::NoteOff:
            noteOff(message.channel, c, message.data1, message.frame);
            break;
        case MidiMessageType::PitchBend:
            output.events.push_back(Event::channelControl(message.frame, sequence++,
                EventType::PitchBend, message.channel,
                static_cast<std::uint16_t>(message.data1 | (message.data2 << 7))));
            break;
        }
    }
    for (const auto& [bank, program] : missing) {
        output.warnings.push_back("missing SoundFont preset bank=" + std::to_string(bank) +
                                  " program=" + std::to_string(program));
    }
    return output;
}

} // namespace midisynth
