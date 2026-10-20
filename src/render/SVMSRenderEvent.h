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
    // A repeat hit that note-on coalescing would drop, kept at its exact
    // frame instead (opt-in, noteOnReexcite). The dispatcher restarts the
    // key's live voices' envelopes rather than launching a new voice, and
    // launches normally when nothing is sounding on the key. The whole-voice
    // renderer models it (exact-frame excite overlay).
    NoteReExcite = 14,
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
    // NoteOn only: how many exact-duplicate note-ons (same channel, key,
    // velocity and frame) this event stands for. 0 and 1 both mean an
    // ordinary single note-on, so zero-initialised buffers stay valid.
    // Produced by the note-merge pass (SVMSNoteMerge.h); a consumer that
    // cannot launch a merged voice must expand it into `count` launches.
    uint32_t count = 1u;
};

// A merged note-on travels through the 16-byte scheduled-event pages as a
// NoteOn whose type byte carries the count: kMergedNoteOnTypeBase + count - 2.
// Any consumer that does not know the encoding sees an unknown event type and
// ignores it, so a missed consumer drops the note rather than misreading it.
inline constexpr uint8_t kMergedNoteOnTypeBase = 16u;
inline constexpr uint32_t kMergedNoteOnMinCount = 2u;
inline constexpr uint32_t kMergedNoteOnMaxCount =
    255u - kMergedNoteOnTypeBase + kMergedNoteOnMinCount;  // 241

using EventDispatcher = void(*)(const RenderEvent& event, uint32_t blockCursor,
                                 void* userData);
using EventBatchDispatcher = void(*)(const RenderEvent* events, uint32_t eventCount,
                                     uint32_t blockCursor, void* userData);

} // namespace svms
