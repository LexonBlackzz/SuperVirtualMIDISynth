#pragma once

#include <cstdint>

namespace svms::canonical {

enum class CanonicalEventType : std::uint8_t {
    NoteOn,
    NoteOff,
    ControlChange,
    ProgramChange,
    PitchBend
};

struct CanonicalEvent {
    std::uint64_t absoluteFrame{};
    std::uint32_t sequence{};
    CanonicalEventType type{};
    std::uint8_t channel{};
    std::uint8_t data1{};
    std::uint8_t data2{};
};

// Private preparation vocabulary used by the imported channel interpreter.
// It deliberately contains no V3 scheduler or RenderEvent types.
enum class MidiMessageType : std::uint8_t {
    NoteOff,
    NoteOn,
    ControlChange,
    ProgramChange,
    PitchBend
};

struct MidiMessage {
    std::uint64_t frame{};
    MidiMessageType type{};
    std::uint8_t channel{};
    std::uint8_t data1{};
    std::uint8_t data2{};
};

enum class CanonicalBackend : std::uint8_t { Scalar, Avx2 };

struct CanonicalEngineConfig {
    double sampleRate{44100.0};
    std::uint32_t voiceCapacity{4096};
    std::uint32_t tileSize{512};
    std::uint32_t maxBlockFrames{8192};
    std::uint32_t workerThreads{};
    std::uint32_t eventScratchCapacity{65536};
    CanonicalBackend backend{CanonicalBackend::Scalar};
};

struct CanonicalTelemetry {
    std::uint64_t sourceEvents{};
    std::uint64_t sourceNoteOns{};
    std::uint64_t expandedEvents{};
    std::uint64_t rejectedEvents{};
    std::uint64_t rejectedInvalidData{};
    std::uint64_t rejectedOrder{};
    std::uint64_t rejectedCapacity{};
    std::uint64_t rejectedInterpreter{};
    std::uint64_t launchedVoices{};
    std::uint64_t retiredVoices{};
    std::uint64_t droppedNoteOns{};
    std::uint64_t stolenVoices{};
    std::uint64_t renderedFrames{};
    std::uint32_t activeVoices{};
    std::uint32_t peakVoices{};
};

} // namespace svms::canonical
