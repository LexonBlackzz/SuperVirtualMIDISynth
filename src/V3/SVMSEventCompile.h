#ifndef SVMS_EVENT_COMPILE_H
#define SVMS_EVENT_COMPILE_H

#include "SVMSEventScheduler.h"
#include "SVMSFrameClock.h"

#include <cstdint>

namespace svms {

inline constexpr uint32_t kInternalResetMessage = 0xFF000001u;
inline constexpr uint32_t kInternalMasterVolumeTag = 0xFE000000u;
inline constexpr uint32_t kInternalRhythmPartTag = 0xFD000000u;
inline constexpr uint32_t kInternalMasterFineTuneTag = 0xFC000000u;
inline constexpr uint32_t kInternalMasterTransposeTag = 0xFB000000u;
inline constexpr uint64_t kAbsoluteFrameTimestampTag = uint64_t{1} << 63u;
inline constexpr uint64_t kAbsoluteFrameTimestampMask =
    ~kAbsoluteFrameTimestampTag;

inline constexpr uint32_t MakeInternalMasterVolumeMessage(
    uint16_t value14) noexcept {
    return kInternalMasterVolumeTag | (value14 & 0x3fffu);
}

inline constexpr uint32_t MakeInternalRhythmPartMessage(
    uint8_t channel, uint8_t map) noexcept {
    return kInternalRhythmPartTag |
        (static_cast<uint32_t>(channel & 0x0fu) << 8u) |
        static_cast<uint32_t>(map & 0x03u);
}

// Fine tuning uses the Universal SysEx 14-bit centered representation:
// 8192 is unshifted and the full range is approximately +/- one semitone.
inline constexpr uint32_t MakeInternalMasterFineTuneMessage(
    uint16_t value14) noexcept {
    return kInternalMasterFineTuneTag | (value14 & 0x3fffu);
}

// XG transpose is encoded exactly like its wire value: 64 is zero and the
// useful range is 40..88 (-24..+24 semitones).
inline constexpr uint32_t MakeInternalMasterTransposeMessage(
    uint8_t centeredValue) noexcept {
    return kInternalMasterTransposeTag |
        static_cast<uint32_t>(centeredValue & 0x7fu);
}

inline constexpr bool IsInternalEngineMessage(uint32_t message) noexcept {
    const uint32_t tag = message & 0xff000000u;
    return message == kInternalResetMessage ||
        tag == kInternalMasterVolumeTag ||
        tag == kInternalRhythmPartTag ||
        tag == kInternalMasterFineTuneTag ||
        tag == kInternalMasterTransposeTag;
}

// Controllers whose effect is pure channel state — a later value fully
// replaces an earlier one. Eligible for the opt-in compiler-side collapse of
// superseded same-(channel,controller) events (CC7/10/11 mix, CC0/32 bank
// select, RPN data entry 6/38, filter/tone row CCs 71-74, FX sends 91/93).
inline constexpr bool IsCollapsibleController(uint8_t controller) noexcept {
    switch (controller) {
        case 0: case 6: case 7: case 10: case 11: case 32: case 38:
        case 71: case 72: case 73: case 74: case 91: case 93:
            return true;
        default: return false;
    }
}

// Controllers that re-target controller state (RPN/NRPN selection) or reset
// it wholesale. Any of these on a channel invalidates the channel's pending
// collapse records: a dropped CC6 could otherwise belong to an RPN parameter
// that the selection no longer points at.
inline constexpr bool IsControllerStateReset(uint8_t controller) noexcept {
    return (controller >= 98u && controller <= 101u) ||
        controller == 120u || controller == 121u || controller == 123u;
}

inline bool CompileTimestampedEvent(const TimestampedMidiEvent& timed,
                                    uint64_t epochQPC,
                                    uint64_t qpcFrequency,
                                    uint32_t sampleRate,
                                    uint32_t leadFrames,
                                    ScheduledRenderEvent& scheduled) noexcept {
    const uint32_t message = timed.message;
    const uint8_t status = static_cast<uint8_t>(message & 0xffu);
    uint8_t data1 = static_cast<uint8_t>((message >> 8u) & 0x7fu);
    uint8_t data2 = static_cast<uint8_t>((message >> 16u) & 0x7fu);
    uint8_t channel = status & 0x0fu;
    RenderEventType type;
    if (message == kInternalResetMessage) {
        type = RenderEventType::Reset;
        channel = data1 = data2 = 0u;
    } else if ((message & 0xffffc000u) == kInternalMasterVolumeTag) {
        type = RenderEventType::MasterVolume;
        channel = 0u;
        data1 = static_cast<uint8_t>(message & 0x7fu);
        data2 = static_cast<uint8_t>((message >> 7u) & 0x7fu);
    } else if ((message & 0xff000000u) == kInternalRhythmPartTag) {
        type = RenderEventType::RhythmPart;
        channel = static_cast<uint8_t>((message >> 8u) & 0x0fu);
        data1 = static_cast<uint8_t>(message & 0x03u);
        data2 = 0u;
    } else if ((message & 0xffffc000u) == kInternalMasterFineTuneTag) {
        type = RenderEventType::MasterFineTune;
        channel = 0u;
        data1 = static_cast<uint8_t>(message & 0x7fu);
        data2 = static_cast<uint8_t>((message >> 7u) & 0x7fu);
    } else if ((message & 0xffffff80u) == kInternalMasterTransposeTag) {
        type = RenderEventType::MasterTranspose;
        channel = 0u;
        data1 = static_cast<uint8_t>(message & 0x7fu);
        data2 = 0u;
    } else {
        switch (status & 0xf0u) {
            case 0x90u:
                type = data2 != 0u ? RenderEventType::NoteOn
                                   : RenderEventType::NoteOff;
                break;
            case 0x80u: type = RenderEventType::NoteOff; break;
            case 0xb0u: type = RenderEventType::ControlChange; break;
            case 0xc0u: type = RenderEventType::ProgramChange; break;
            case 0xd0u: type = RenderEventType::ChannelPressure; break;
            case 0xe0u: type = RenderEventType::PitchBend; break;
            default: return false;
        }
    }

    scheduled.type = type;
    scheduled.channel = channel;
    scheduled.data1 = data1;
    scheduled.data2 = data2;
    if ((timed.qpcTimestamp & kAbsoluteFrameTimestampTag) != 0u) {
        scheduled.targetFrame = static_cast<int64_t>(
            timed.qpcTimestamp & kAbsoluteFrameTimestampMask);
    } else {
        scheduled.targetFrame = QpcDeltaToFrames(
            static_cast<int64_t>(timed.qpcTimestamp) -
                static_cast<int64_t>(epochQPC),
            static_cast<int64_t>(qpcFrequency), sampleRate) + leadFrames;
    }
    scheduled.sequence = timed.sequence;
    return true;
}

} // namespace svms

#endif
