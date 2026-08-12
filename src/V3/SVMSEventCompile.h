#ifndef SVMS_EVENT_COMPILE_H
#define SVMS_EVENT_COMPILE_H

#include "SVMSEventScheduler.h"
#include "SVMSFrameClock.h"

#include <cstdint>

namespace svms {

inline constexpr uint32_t kInternalResetMessage = 0xFF000001u;

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
    } else {
        switch (status & 0xf0u) {
            case 0x90u:
                type = data2 != 0u ? RenderEventType::NoteOn
                                   : RenderEventType::NoteOff;
                break;
            case 0x80u: type = RenderEventType::NoteOff; break;
            case 0xb0u: type = RenderEventType::ControlChange; break;
            case 0xc0u: type = RenderEventType::ProgramChange; break;
            case 0xe0u: type = RenderEventType::PitchBend; break;
            default: return false;
        }
    }

    scheduled.type = type;
    scheduled.channel = channel;
    scheduled.data1 = data1;
    scheduled.data2 = data2;
    scheduled.targetFrame = QpcDeltaToFrames(
        static_cast<int64_t>(timed.qpcTimestamp) -
            static_cast<int64_t>(epochQPC),
        static_cast<int64_t>(qpcFrequency), sampleRate) + leadFrames;
    scheduled.sequence = timed.sequence;
    return true;
}

} // namespace svms

#endif
