#pragma once

#include <cstdint>

namespace svms {

// ════════════════════════════════════════════════════════════════════════
// RenderEvent — sub-sample-precise MIDI event descriptor.
// ════════════════════════════════════════════════════════════════════════
enum class RenderEventType : uint8_t {
    NoteOn       = 0,
    NoteOff      = 1,
    ControlChange= 2,
    ProgramChange= 3,
    PitchBend    = 4,
    AllNotesOff  = 5,
    AllSoundOff  = 6,
    Reset         = 7,
    // Internal overload-recovery command. data2 carries a count from 1..255.
    // It is never created for events that still have a writable exact frame.
    StaleNoteOffBatch = 8,
    MasterVolume = 9,
    RhythmPart = 10,
    MasterFineTune = 11,
    MasterTranspose = 12,
    // Channel aftertouch (0xD0). Feeds the SF2 default channel-pressure
    // modulator that scales per-voice vibrato LFO depth.
    ChannelPressure = 13,
};

struct RenderEvent {
    RenderEventType type;
    uint8_t  channel;
    uint8_t  data1;
    uint8_t  data2;
    uint32_t frameOffset;
    // Original producer order.  Termination fences use this to reject note
    // ons that were queued before a later CC120/CC123/reset but reached the
    // audio thread afterward through another priority lane.
    uint32_t ingressSequence;
};

using EventDispatcher = void(*)(const RenderEvent& event, uint32_t blockCursor,
                                 void* userData);
using EventBatchDispatcher = void(*)(const RenderEvent* events, uint32_t eventCount,
                                     uint32_t blockCursor, void* userData);

} // namespace svms
