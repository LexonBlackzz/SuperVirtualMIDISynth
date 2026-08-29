#ifndef SVMS_NOTE_ON_COLLAPSE_H
#define SVMS_NOTE_ON_COLLAPSE_H

#include <atomic>
#include <cstdint>
#include "SVMSTypes.h"

namespace svms {

// Same-key note-on coalescing gate (SnappySynth stack-count pattern).
//
// Black-MIDI material hammers the same (channel, note) thousands of times
// per second. Spawning one voice per event melts the audio thread in the
// steal cascade and collapses effective polyphony to roughly "how many
// steals fit in one block". Instead, producers keep a per-key hit
// accumulator: the first hit after a reset spawns normally, then one hit
// per `threshold` repeats spawns. Note-off, all-notes-off/all-sound-off,
// and engine reset clear the accumulator so legitimate musical repeats
// always keep full polyphony. The threshold is rounded to a power of two
// so the hot path is a mask, not a division, and is runtime-adjustable
// (SetThreshold) for live tuning.
class NoteOnCollapseGate {
public:
    // Sets the spawn interval. The requested value is rounded down to a
    // power of two (minimum 2); 0 or 1 disables coalescing so every hit
    // spawns. Safe to call while the synth is running.
    void SetThreshold(uint32_t requested) noexcept {
        uint32_t t = requested;
        if (t < 2u) {
            threshold_.store(1u, std::memory_order_relaxed);
            return;
        }
        while (t & (t - 1u)) t &= t - 1u; // round down to a power of two
        threshold_.store(t, std::memory_order_relaxed);
    }

    uint32_t Threshold() const noexcept {
        return threshold_.load(std::memory_order_relaxed);
    }

    static constexpr uint32_t kKeyCount = kChannelCount * kNoteCount;

    // Returns true when the caller must push the event through ingress.
    bool OnNoteOn(uint32_t keyIndex) noexcept {
        uint32_t ignoredStack = 0u;
        return OnNoteOn(keyIndex, ignoredStack);
    }

    // Overload that also reports velocity-stacking state: `outStack`
    // receives the number of note-ons accumulated for this key since the
    // last spawn (inclusive of the current hit) when the event spawns, and
    // the running count when it collapses. Callers convert that density
    // into loudness on the spawned voice (SnappySynth stacks it the same
    // way) instead of losing it entirely.
    bool OnNoteOn(uint32_t keyIndex, uint32_t& outStack) noexcept {
        const uint32_t t = threshold_.load(std::memory_order_relaxed);
        const uint32_t idx = keyIndex & (kKeyCount - 1u);
        if (t <= 1u) {
            outStack = 1u; // disabled: every hit spawns on its own
            return true;
        }
        const uint32_t hit = accumulators_[idx].fetch_add(
            1u, std::memory_order_relaxed);
        const bool spawn = hit == 0u || (hit & (t - 1u)) == 0u;
        if (spawn) {
            outStack = stacks_[idx].exchange(0u,
                std::memory_order_relaxed) + 1u;
        } else {
            outStack = stacks_[idx].fetch_add(1u,
                std::memory_order_relaxed) + 1u;
        }
        return spawn;
    }

    void ResetKey(uint32_t keyIndex) noexcept {
        accumulators_[keyIndex & (kKeyCount - 1u)].store(
            0u, std::memory_order_relaxed);
        stacks_[keyIndex & (kKeyCount - 1u)].store(
            0u, std::memory_order_relaxed);
    }

    void ResetChannel(uint32_t channel) noexcept {
        const uint32_t base =
            (channel & (kChannelCount - 1u)) * kNoteCount;
        for (uint32_t n = 0; n < kNoteCount; ++n) {
            accumulators_[base + n].store(0u, std::memory_order_relaxed);
            stacks_[base + n].store(0u, std::memory_order_relaxed);
        }
    }

    void ResetAll() noexcept {
        for (uint32_t k = 0; k < kKeyCount; ++k) {
            accumulators_[k].store(0u, std::memory_order_relaxed);
            stacks_[k].store(0u, std::memory_order_relaxed);
        }
    }

private:
    std::atomic<uint32_t> threshold_{32u};
    std::atomic<uint32_t> accumulators_[kKeyCount]{};
    std::atomic<uint32_t> stacks_[kKeyCount]{};
};

} // namespace svms

#endif
