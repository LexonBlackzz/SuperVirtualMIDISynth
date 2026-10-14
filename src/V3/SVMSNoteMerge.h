#ifndef SVMS_NOTE_MERGE_H
#define SVMS_NOTE_MERGE_H

#include "SVMSEventScheduler.h"

#include <cstdint>
#include <vector>

namespace svms {

// Exact note-on merging (opt-in).
//
// N note-ons with the same channel, key and velocity on the same frame are N
// identical voices; identical voices sum linearly, so one voice with N times
// the gain sounds the same. This pass folds such duplicates in an already
// sorted run of events into one event carrying a count (in the type byte of a
// ScheduledRenderEvent, see kMergedNoteOnTypeBase; in RenderEvent::count).
// Nothing is approximated: the launch frame is untouched, and the engine
// releases the merged voice one unit per note-off, forking a twin when the
// copies end at different times (VoiceManager::ForkReleasedUnits).
//
// This is NOT the time-window coalescing gate (SVMSNoteOnCollapse.h): that
// one drops note-ons across a 20 ms window and approximates their loudness.
//
// Ordering. Within one frame the pass pulls a later duplicate up to the
// first one ("hoisting"), so note-ons of other keys that sat between them
// now launch after the merged voice. That is unobservable unless voices are
// stolen (a different victim can be chosen) or a SoundFont has exclusive
// classes (a hi-hat choke can land on the other side). Callers with
// exclusive classes pass allowHoist=false, which only merges note-ons that
// are adjacent on their channel.
//
// A merge never crosses an event that could change what a launch sees: any
// non-note-on event on the same channel (note-off, controller, program,
// pitch bend, pressure) or any global event (reset, master controls) ends
// the merge window for the affected channels.
class NoteMerger {
public:
    NoteMerger() { slots_.resize(kSlotCount); }

    // Folds duplicates inside compiled-page events (sorted by targetFrame,
    // sequence). Returns the new event count and adds the number of note-ons
    // folded away to `removed`.
    uint32_t Merge(ScheduledRenderEvent* events, uint32_t count,
                   bool allowHoist, uint64_t& removed) noexcept {
        return MergeImpl<ScheduledTraits>(events, count, allowHoist, removed);
    }

    // The same pass over a block of RenderEvents (offline/prerender), sorted
    // by frameOffset with ties in dispatch order.
    uint32_t MergeEvents(RenderEvent* events, uint32_t count, bool allowHoist,
                         uint64_t& removed) noexcept {
        return MergeImpl<RenderTraits>(events, count, allowHoist, removed);
    }

private:
    static constexpr uint32_t kChannels = 16u;
    // A compiler page holds 8192 events, so a run never has more than 8192
    // distinct keys; twice that keeps the open-addressing probe short.
    static constexpr uint32_t kSlotCount = 16384u;

    struct ScheduledTraits {
        using Event = ScheduledRenderEvent;
        static int64_t Frame(const Event& e) noexcept { return e.targetFrame; }
        // A merged note-on has a type >= kMergedNoteOnTypeBase, so it never
        // compares equal to NoteOn and acts as a channel barrier.
        static RenderEventType Type(const Event& e) noexcept { return e.type; }
        static uint32_t Count(const Event& e) noexcept { return e.NoteCount(); }
        static void SetCount(Event& e, uint32_t n) noexcept { e.SetNoteCount(n); }
    };
    struct RenderTraits {
        using Event = RenderEvent;
        static int64_t Frame(const Event& e) noexcept { return e.frameOffset; }
        // An already-merged RenderEvent is reported with a non-NoteOn type so
        // it is a barrier rather than a merge head.
        static RenderEventType Type(const Event& e) noexcept {
            return e.type == RenderEventType::NoteOn && e.count > 1u
                ? RenderEventType::AllNotesOff : e.type;
        }
        static uint32_t Count(const Event& e) noexcept {
            return e.count > 1u ? e.count : 1u;
        }
        static void SetCount(Event& e, uint32_t n) noexcept { e.count = n; }
    };

    struct Slot {
        uint32_t generation = 0u;
        uint32_t key = 0u;
        uint32_t epoch = 0u;
        uint32_t head = 0u;
    };

    template <class Traits>
    uint32_t MergeImpl(typename Traits::Event* events, uint32_t count,
                       bool allowHoist, uint64_t& removed) noexcept {
        using Event = typename Traits::Event;
        if (count < 2u) return count;
        uint32_t write = 0u;
        int64_t runFrame = Traits::Frame(events[0]);
        BeginRun();
        for (uint32_t read = 0u; read < count; ++read) {
            const Event event = events[read];
            if (Traits::Frame(event) != runFrame) {
                runFrame = Traits::Frame(event);
                BeginRun();
            }
            const RenderEventType type = Traits::Type(event);
            if (type == RenderEventType::NoteOn && event.channel < kChannels) {
                const uint32_t key =
                    (static_cast<uint32_t>(event.channel) << 16u) |
                    (static_cast<uint32_t>(event.data1) << 8u) | event.data2;
                Slot& slot = Find(key);
                if (slot.generation == generation_ && slot.key == key &&
                    slot.epoch == epoch_[event.channel]) {
                    const uint32_t merged = Traits::Count(events[slot.head]);
                    if (merged < kMergedNoteOnMaxCount) {
                        Traits::SetCount(events[slot.head], merged + 1u);
                        ++removed;
                        continue;
                    }
                }
                // Not mergeable into an open head: this event becomes one.
                // Without hoisting, an unrelated note-on ends every other
                // open head on its channel.
                if (!allowHoist) ++epoch_[event.channel];
                const bool fresh = slot.generation != generation_;
                if (!fresh || liveInRun_ < kSlotCount / 2u) {
                    slot.generation = generation_;
                    slot.key = key;
                    slot.epoch = epoch_[event.channel];
                    slot.head = write;
                    if (fresh) ++liveInRun_;
                }
            } else if (event.channel < kChannels &&
                       type != RenderEventType::Reset &&
                       type != RenderEventType::MasterVolume &&
                       type != RenderEventType::MasterFineTune &&
                       type != RenderEventType::MasterTranspose) {
                ++epoch_[event.channel];  // channel-scoped barrier
            } else {
                for (uint32_t c = 0u; c < kChannels; ++c) ++epoch_[c];
            }
            events[write++] = event;
        }
        return write;
    }

    void BeginRun() noexcept {
        liveInRun_ = 0u;
        if (++generation_ == 0u) {  // wrapped: stale slots would look live
            for (Slot& slot : slots_) slot.generation = 0u;
            generation_ = 1u;
        }
    }

    // Returns the slot holding `key` for this run, or the free slot where it
    // belongs (generation != current).
    Slot& Find(uint32_t key) noexcept {
        uint32_t index = (key * 0x9e3779b1u) >> (32u - 14u);
        for (;;) {
            Slot& slot = slots_[index];
            if (slot.generation != generation_ || slot.key == key) return slot;
            index = (index + 1u) & (kSlotCount - 1u);
        }
    }

    std::vector<Slot> slots_;
    uint32_t generation_ = 0u;
    // Distinct open heads in the current run. Past half the table a new key
    // is simply not recorded (it passes through unmerged), so the probe loop
    // always finds a free slot however large the caller's run is.
    uint32_t liveInRun_ = 0u;
    uint32_t epoch_[kChannels] = {};
};

} // namespace svms

#endif
