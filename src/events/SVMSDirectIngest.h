#ifndef SVMS_DIRECT_INGEST_H
#define SVMS_DIRECT_INGEST_H

// Direct page feed: a player that already has its events in time order can
// compile them straight into a CompiledEventPage on its own thread, skipping
// the ingress ring, the per-event submit bookkeeping and the compiler thread.
// This header is the pure part (SVMS_ShortEvent run -> ScheduledRenderEvent
// run); Driver::SubmitShortBatchDirect owns page acquisition and publishing.
//
// Contract: the result is event-for-event what the ring path produces for the
// same messages (CompileTimestampedEvent), with sequences seqBase, seqBase+1...
// so dispatch order, exact frames and termination fences are unchanged.

#include "events/SVMSEventCompile.h"
#include "events/SVMSFrameClock.h"
#include "include/svmsapi.h"

#include <cstdint>

namespace svms {

struct DirectCompileContext {
    uint64_t epochQpc;
    int64_t qpcFrequency;
    uint32_t sampleRate;
    uint32_t leadFrames;
};

// Compiles events[0..count) into out[]. Event i gets sequence seqBase + i, also
// when an earlier event did not compile (the ring path numbers at submit).
// `terminate(global, channel, sequence)` is called for reset / CC120 / CC123,
// which publish a termination fence on the ring path. A plain note-on with a
// velocity below `shedBelow` is dropped (priority-velocity overflow shedding,
// 0 = none) and reported through `shed(velocity)`; it still uses its sequence.
// Returns events written.
template <typename TerminateFn, typename ShedFn>
inline uint32_t CompileDirectRun(const SVMS_ShortEvent* events, uint32_t count,
                                 uint64_t immediateQpc, uint32_t seqBase,
                                 const DirectCompileContext& ctx,
                                 ScheduledRenderEvent* out,
                                 TerminateFn&& terminate, uint32_t shedBelow,
                                 ShedFn&& shed) noexcept {
    uint32_t written = 0u;
    // Players send many events per timestamp; convert each distinct one once.
    uint64_t lastQpc = 0u;
    int64_t lastFrame = 0;
    bool haveLast = false;
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t message = events[i].packed_message;
        const uint64_t qpc = events[i].timestamp_qpc != 0u
            ? events[i].timestamp_qpc : immediateQpc;
        const uint32_t sequence = seqBase + i;
        const uint8_t status = static_cast<uint8_t>(message & 0xffu);

        if ((message & 0xff000000u) == 0u && (status & 0xe0u) == 0x80u &&
            (qpc & kAbsoluteFrameTimestampTag) == 0u) {
            // Plain note-on / note-off: the common case, no tag tests.
            if (!haveLast || qpc != lastQpc) {
                lastFrame = QpcDeltaToFrames(
                    static_cast<int64_t>(qpc) -
                        static_cast<int64_t>(ctx.epochQpc),
                    ctx.qpcFrequency, ctx.sampleRate) + ctx.leadFrames;
                lastQpc = qpc;
                haveLast = true;
            }
            const uint8_t velocity = static_cast<uint8_t>((message >> 16u) & 0x7fu);
            const bool noteOn = (status & 0xf0u) == 0x90u && velocity != 0u;
            if (noteOn && velocity < shedBelow) {
                shed(velocity);
                continue;
            }
            ScheduledRenderEvent& e = out[written++];
            e.targetFrame = lastFrame;
            e.sequence = sequence;
            e.type = noteOn ? RenderEventType::NoteOn : RenderEventType::NoteOff;
            e.channel = status & 0x0fu;
            e.data1 = static_cast<uint8_t>((message >> 8u) & 0xffu);
            e.data2 = velocity;
            continue;
        }

        TimestampedMidiEvent timed{};
        timed.message = message;
        timed.qpcTimestamp = qpc;
        timed.sequence = sequence;
        ScheduledRenderEvent scheduled{};
        if (!CompileTimestampedEvent(timed, ctx.epochQpc,
                                     static_cast<uint64_t>(ctx.qpcFrequency),
                                     ctx.sampleRate, ctx.leadFrames, scheduled))
            continue;
        if (message == kInternalResetMessage) {
            terminate(true, static_cast<uint8_t>(0u), sequence);
        } else if ((status & 0xf0u) == 0xb0u) {
            const uint8_t controller = static_cast<uint8_t>((message >> 8u) & 0x7fu);
            if (controller == 120u || controller == 123u)
                terminate(false, static_cast<uint8_t>(status & 0x0fu), sequence);
        }
        out[written++] = scheduled;
    }
    return written;
}

} // namespace svms

#endif
