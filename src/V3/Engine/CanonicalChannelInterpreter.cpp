#include "CanonicalChannelInterpreter.h"

#include <algorithm>
#include <stdexcept>

namespace svms::canonical::detail {

ChannelInterpreter::ChannelInterpreter(const PreparedSoundFont& font,
                                       double sampleRate,
                                       std::size_t logicalNoteCapacity,
                                       bool collectWarnings)
    : font_(font), sampleRate_(sampleRate),
      logicalNoteCapacity_(logicalNoteCapacity),
      collectWarnings_(collectWarnings) {
    if (!(sampleRate > 0.0)) throw std::invalid_argument("sample rate must be positive");
    if (logicalNoteCapacity_) instances_.reserve(logicalNoteCapacity_);
    initializeChannels();
}

void ChannelInterpreter::reset() {
    channels_ = {};
    initializeChannels();
    instances_.clear();
    freeInstance_ = noNode;
    nextNoteInstance_ = 1;
    sourceNoteOns_ = 0;
    missing_.clear();
}

void ChannelInterpreter::initializeChannels() noexcept {
    for (auto& channel : channels_) {
        channel.keyHead.fill(noNode);
        channel.keyTail.fill(noNode);
    }
}

bool ChannelInterpreter::appendInstance(Channel& state, std::uint8_t note,
                                        std::uint32_t id) {
    std::uint32_t index{};
    if (freeInstance_ != noNode) {
        index = freeInstance_;
        freeInstance_ = instances_[index].next;
        instances_[index] = {id, noNode, true};
    } else {
        if (logicalNoteCapacity_ && instances_.size() >= logicalNoteCapacity_)
            return false;
        index = static_cast<std::uint32_t>(instances_.size());
        instances_.push_back({id, noNode, true});
    }
    if (state.keyTail[note] == noNode) state.keyHead[note] = index;
    else instances_[state.keyTail[note]].next = index;
    state.keyTail[note] = index;
    return true;
}

void ChannelInterpreter::removeInstance(Channel& state, std::uint8_t note,
                                        std::uint32_t previous,
                                        std::uint32_t index) {
    const auto next = instances_[index].next;
    if (previous == noNode) state.keyHead[note] = next;
    else instances_[previous].next = next;
    if (state.keyTail[note] == index) state.keyTail[note] = previous;
    instances_[index].next = freeInstance_;
    freeInstance_ = index;
}

void ChannelInterpreter::clearInstances(Channel& state) noexcept {
    for (std::uint16_t note = 0; note < 128; ++note) {
        auto index = state.keyHead[note];
        while (index != noNode) {
            const auto next = instances_[index].next;
            instances_[index].next = freeInstance_;
            freeInstance_ = index;
            index = next;
        }
        state.keyHead[note] = state.keyTail[note] = noNode;
    }
}

const PreparedPreset* ChannelInterpreter::findPreset(
    std::uint8_t channel, const Channel& state) const {
    const auto bank = static_cast<std::uint16_t>((state.msb << 7) | state.lsb);
    if (channel == 9 && bank == 0) {
        if (auto* preset = font_.findPreset(128, state.program)) return preset;
    }
    if (auto* preset = font_.findPreset(bank, state.program)) return preset;
    return bank != 0 ? font_.findPreset(0, state.program) : nullptr;
}

void ChannelInterpreter::releaseDeferred(
    std::uint8_t channel, Channel& state, std::uint64_t frame,
    std::uint32_t& sequence, std::vector<Event>& output) {
    for (std::uint16_t note = 0; note < 128; ++note) {
        auto previous = noNode;
        auto index = state.keyHead[note];
        while (index != noNode) {
            const auto next = instances_[index].next;
            if (!instances_[index].held) {
                output.push_back(Event::noteOff(frame, sequence++,
                    static_cast<std::uint8_t>(note), channel,
                    instances_[index].id));
                removeInstance(state, static_cast<std::uint8_t>(note),
                               previous, index);
            } else {
                previous = index;
            }
            index = next;
        }
    }
}

void ChannelInterpreter::noteOff(
    std::uint8_t channel, Channel& state, std::uint8_t note,
    std::uint64_t frame, std::uint32_t& sequence, std::vector<Event>& output) {
    auto previous = noNode;
    auto index = state.keyHead[note];
    while (index != noNode && !instances_[index].held) {
        previous = index;
        index = instances_[index].next;
    }
    if (index == noNode) return;
    instances_[index].held = false;
    if (!state.sustain) {
        output.push_back(Event::noteOff(frame, sequence++, note, channel,
                                        instances_[index].id));
        removeInstance(state, note, previous, index);
    }
}

bool ChannelInterpreter::process(const MidiMessage& message,
                                 std::uint32_t& sequence,
                                 std::vector<Event>& output) {
    if (message.channel >= 16 || message.data1 >= 128 || message.data2 >= 128) {
        throw std::invalid_argument("invalid MIDI channel message");
    }
    auto& channel = channels_[message.channel];
    switch (message.type) {
    case MidiMessageType::ProgramChange:
        channel.program = message.data1;
        break;
    case MidiMessageType::ControlChange:
        if (message.data1 == 0) channel.msb = message.data2;
        else if (message.data1 == 32) channel.lsb = message.data2;
        else if (message.data1 == 1) output.push_back(Event::channelControl(
            message.frame, sequence++, EventType::ModulationWheel,
            message.channel, message.data2));
        else if (message.data1 == 7) output.push_back(Event::channelControl(
            message.frame, sequence++, EventType::ChannelVolume,
            message.channel, message.data2));
        else if (message.data1 == 10) output.push_back(Event::channelControl(
            message.frame, sequence++, EventType::ChannelPan,
            message.channel, message.data2));
        else if (message.data1 == 11) output.push_back(Event::channelControl(
            message.frame, sequence++, EventType::ChannelExpression,
            message.channel, message.data2));
        else if (message.data1 == 64) {
            const bool down = message.data2 >= 64;
            if (channel.sustain && !down) {
                releaseDeferred(message.channel, channel, message.frame,
                                sequence, output);
            }
            channel.sustain = down;
        } else if (message.data1 == 101) channel.rpnMsb = message.data2;
        else if (message.data1 == 100) channel.rpnLsb = message.data2;
        else if (message.data1 == 6 || message.data1 == 38) {
            if (message.data1 == 6) channel.dataMsb = message.data2;
            else channel.dataLsb = message.data2;
            if (channel.rpnMsb == 0 && channel.rpnLsb == 0) {
                const auto cents = static_cast<std::uint16_t>(
                    channel.dataMsb * 100 +
                    std::min<std::uint8_t>(channel.dataLsb, 99));
                output.push_back(Event::channelControl(message.frame, sequence++,
                    EventType::PitchBendRange, message.channel, cents));
            }
        } else if (message.data1 == 120) {
            output.push_back(Event::channelControl(message.frame, sequence++,
                EventType::AllSoundOff, message.channel, 0));
            clearInstances(channel);
        } else if (message.data1 == 121) {
            if (channel.sustain) {
                releaseDeferred(message.channel, channel, message.frame,
                                sequence, output);
            }
            channel.sustain = false;
            channel.rpnMsb = channel.rpnLsb = 127;
            channel.dataMsb = 2;
            channel.dataLsb = 0;
            output.push_back(Event::channelControl(message.frame, sequence++,
                EventType::ResetControllers, message.channel, 0));
        } else if (message.data1 == 123) {
            for (std::uint16_t note = 0; note < 128; ++note) {
                auto index = channel.keyHead[note];
                while (index != noNode) {
                    instances_[index].held = false;
                    index = instances_[index].next;
                }
            }
            if (!channel.sustain) {
                releaseDeferred(message.channel, channel, message.frame,
                                sequence, output);
            }
        }
        break;
    case MidiMessageType::NoteOn: {
        const auto instance = nextNoteInstance_++;
        if (nextNoteInstance_ == 0) ++nextNoteInstance_;
        if (!appendInstance(channel, message.data1, instance)) return false;
        ++sourceNoteOns_;
        const auto* preset = findPreset(message.channel, channel);
        if (preset) {
            appendPreparedNoteOn(output, *preset, font_, message.frame,
                sequence, message.channel, message.data1, message.data2,
                sampleRate_, instance);
        } else {
            if (collectWarnings_) missing_.emplace(static_cast<std::uint16_t>(
                (channel.msb << 7) | channel.lsb), channel.program);
        }
        break;
    }
    case MidiMessageType::NoteOff:
        noteOff(message.channel, channel, message.data1, message.frame,
                sequence, output);
        break;
    case MidiMessageType::PitchBend:
        output.push_back(Event::channelControl(message.frame, sequence++,
            EventType::PitchBend, message.channel,
            static_cast<std::uint16_t>(message.data1 | (message.data2 << 7))));
        break;
    }
    return true;
}

std::size_t ChannelInterpreter::requiredEventCapacity(
    const MidiMessage& message) const noexcept {
    if (message.channel >= channels_.size() || message.data1 >= 128 ||
        message.data2 >= 128) return 0;
    const auto& channel = channels_[message.channel];
    auto instanceCount = [&](bool deferredOnly) {
        std::size_t count = 0;
        for (std::uint16_t note = 0; note < 128; ++note) {
            auto index = channel.keyHead[note];
            while (index != noNode) {
                if (!deferredOnly || !instances_[index].held) ++count;
                index = instances_[index].next;
            }
        }
        return count;
    };
    switch (message.type) {
    case MidiMessageType::NoteOn:
        if (const auto* preset = findPreset(message.channel, channel))
            return font_.matchingRegions(*preset, message.data1, message.data2).size();
        return 0;
    case MidiMessageType::NoteOff:
        return 1;
    case MidiMessageType::PitchBend:
        return 1;
    case MidiMessageType::ProgramChange:
        return 0;
    case MidiMessageType::ControlChange:
        if (message.data1 == 1 || message.data1 == 7 || message.data1 == 10 || message.data1 == 11 ||
            message.data1 == 120) return 1;
        if (message.data1 == 64)
            return channel.sustain && message.data2 < 64 ? instanceCount(true) : 0;
        if (message.data1 == 121)
            return 1 + (channel.sustain ? instanceCount(true) : 0);
        if (message.data1 == 123)
            return channel.sustain ? 0 : instanceCount(false);
        if ((message.data1 == 6 || message.data1 == 38) &&
            channel.rpnMsb == 0 && channel.rpnLsb == 0) return 1;
        return 0;
    }
    return 0;
}

std::size_t ChannelInterpreter::requiredReleaseAllCapacity() const noexcept {
    std::size_t count = 0;
    for (const auto& channel : channels_) {
        for (const auto head : channel.keyHead) {
            auto index = head;
            while (index != noNode) {
                ++count;
                index = instances_[index].next;
            }
        }
    }
    return count;
}

void ChannelInterpreter::appendReleaseAll(std::uint64_t frame,
                                          std::uint32_t& sequence,
                                          std::vector<Event>& output) {
    // Offline end-of-song release is unconditional in V3, including notes
    // still owned by a depressed sustain pedal. Keep this lifecycle operation
    // inside the canonical interpreter rather than synthesizing source CCs.
    for (std::uint8_t channelIndex = 0; channelIndex < channels_.size();
         ++channelIndex) {
        auto& channel = channels_[channelIndex];
        channel.sustain = false;
        for (const auto head : channel.keyHead) {
            auto index = head;
            while (index != noNode) {
                instances_[index].held = false;
                index = instances_[index].next;
            }
        }
        releaseDeferred(channelIndex, channel, frame, sequence, output);
    }
}

void ChannelInterpreter::appendWarnings(std::vector<std::string>& output) const {
    for (const auto& [bank, program] : missing_) {
        output.push_back("missing SoundFont preset bank=" +
            std::to_string(bank) + " program=" + std::to_string(program));
    }
}

} // namespace svms::canonical::detail
