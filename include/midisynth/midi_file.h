#pragma once

#include "midisynth/soundfont.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace midisynth {

enum class MidiMessageType : std::uint8_t {
    NoteOff, NoteOn, ControlChange, ProgramChange, PitchBend
};

// A transient decoded value. MidiFile stores no array of these structs.
struct MidiMessage {
    std::uint64_t frame{};
    std::uint64_t order{};
    MidiMessageType type{};
    std::uint8_t channel{};
    std::uint8_t data1{};
    std::uint8_t data2{};
};

struct MidiStorageStats {
    std::uint64_t sourceBytes{};
    std::uint64_t compactBytes{};
    std::uint64_t timingAndGroupBytes{};
    std::uint64_t messagePayloadBytes{};
    std::uint64_t logicalMessages{};
    std::uint64_t peakParserWorkingBytes{};
    std::uint64_t legacyEstimatedPeakBytes{};
};

class CompactMidiReader {
public:
    explicit CompactMidiReader(std::span<const std::uint8_t> data);
    bool next(MidiMessage& message);

private:
    std::span<const std::uint8_t> data_;
    std::size_t position_{};
    std::uint64_t frame_{};
    std::uint64_t remainingInGroup_{};
    std::uint64_t nextOrder_{};
    bool sawGroup_{};
};

struct MidiFile {
    std::vector<std::uint8_t> compactData;
    std::uint64_t lastFrame{};
    std::vector<std::string> warnings;
    MidiStorageStats storage;

    [[nodiscard]] CompactMidiReader reader() const { return CompactMidiReader(compactData); }
};

struct PreparedPerformance {
    std::vector<Event> events;
    std::uint64_t lastFrame{};
    std::uint64_t sourceNoteOns{};
    std::vector<std::string> warnings;
};

MidiFile loadMidiFile(const std::string& path, double sampleRate);
PreparedPerformance preparePerformance(const MidiFile& midi,
                                       const PreparedSoundFont& font,
                                       double sampleRate);

} // namespace midisynth
