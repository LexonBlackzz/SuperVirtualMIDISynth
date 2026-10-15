#ifndef SVMS_REALTIME_BLOCK_H
#define SVMS_REALTIME_BLOCK_H

// One realtime audio block for StandaloneSynth hosts (the Linux KDMAPI
// runtime): every event due inside the block goes to the whole-voice
// RenderBlock machinery in ONE call, at its exact frame, with the worker pool
// and batched dispatch -- the same path the offline renderer uses.
//
// The previous Linux loop rendered up to the next event, dispatched it, and
// re-entered RenderBlock for every event. At Black MIDI densities that is a
// RenderBlock call every few frames (the offline path's comment measured it
// at a fraction of realtime).
//
// Anything RenderBlock cannot take (Reset, XG/SysEx-derived internal
// messages, unmapped statuses) is a barrier: the block is split at its exact
// frame, exactly like the driver's barrier splitting.

#include "SVMSRenderEvent.h"
#include "SVMSStandaloneSynth.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace svms {

struct RealtimeEvent {
    uint64_t targetFrame = 0u;   // absolute output frame
    uint64_t sequence = 0u;      // ingress order (ties and note/control fences)
    uint32_t message = 0u;       // packed MIDI (ignored for resets)
    bool reset = false;
};

class RealtimeBlockRenderer {
public:
    // `events` must be sorted by (targetFrame, sequence) and hold only events
    // with targetFrame < outputFrame + frames. Output is planar left/right.
    void Render(StandaloneSynth& synth, const RealtimeEvent* events,
                size_t eventCount, float* left, float* right, uint32_t frames,
                uint64_t outputFrame) {
        cursor_ = 0u;
        batch_.clear();
        for (size_t i = 0u; i < eventCount; ++i) {
            const RealtimeEvent& due = events[i];
            // Never place an event before frames already committed.
            const uint64_t writable = outputFrame + cursor_;
            const uint64_t target = (std::max)(due.targetFrame, writable);
            const uint32_t at = static_cast<uint32_t>(target - outputFrame);
            RenderEvent ev{};
            if (!due.reset && Convert(due, ev)) {
                ev.frameOffset = at - cursor_;
                batch_.push_back(ev);
                continue;
            }
            Flush(synth, left, right, at, outputFrame);
            if (due.reset) synth.ResetAll(target);
            else synth.Dispatch(due.message, target);
        }
        Flush(synth, left, right, frames, outputFrame);
    }

private:
    static bool Convert(const RealtimeEvent& due, RenderEvent& ev) {
        const uint32_t message = due.message;
        const uint8_t status = static_cast<uint8_t>(message & 0xffu);
        RenderEventType type;
        switch (status & 0xf0u) {
        case 0x90u:
            type = (message & 0xff0000u) != 0u ? RenderEventType::NoteOn
                                               : RenderEventType::NoteOff;
            break;
        case 0x80u: type = RenderEventType::NoteOff; break;
        case 0xb0u: type = RenderEventType::ControlChange; break;
        case 0xc0u: type = RenderEventType::ProgramChange; break;
        case 0xd0u: type = RenderEventType::ChannelPressure; break;
        case 0xe0u: type = RenderEventType::PitchBend; break;
        default: return false;
        }
        // Engine-internal tagged messages (XG master volume, tuning, ...) are
        // not MIDI; Dispatch() decodes them.
        if (IsInternalEngineMessage(message)) return false;
        ev.type = type;
        ev.channel = status & 0x0fu;
        ev.data1 = static_cast<uint8_t>(
            (message >> 8u) & (((status & 0xe0u) == 0x80u) ? 0xffu : 0x7fu));
        ev.data2 = static_cast<uint8_t>((message >> 16u) & 0x7fu);
        ev.ingressSequence = static_cast<uint32_t>(due.sequence);
        return true;
    }

    // Renders [cursor_, upTo) with the collected events. Events landing on the
    // sub-block's end belong to the next stretch: they apply right after it.
    void Flush(StandaloneSynth& synth, float* left, float* right,
               uint32_t upTo, uint64_t outputFrame) {
        const uint32_t span = upTo - cursor_;
        size_t inBlock = batch_.size();
        while (inBlock && batch_[inBlock - 1u].frameOffset >= span) --inBlock;
        if (span != 0u) {
            synth.RenderWithEvents(batch_.data(), static_cast<uint32_t>(inBlock),
                                   left + cursor_, right + cursor_, span,
                                   outputFrame + cursor_);
        }
        if (inBlock != batch_.size()) {
            synth.SetCursor(outputFrame + upTo);
            for (size_t i = inBlock; i < batch_.size(); ++i)
                synth.ApplyRenderEvent(batch_[i]);
        }
        batch_.clear();
        cursor_ = upTo;
    }

    std::vector<RenderEvent> batch_;
    uint32_t cursor_ = 0u;
};

} // namespace svms

#endif
