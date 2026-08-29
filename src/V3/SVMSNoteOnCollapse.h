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
// steals fit in one block". When enabled, producers keep a per-key hit
// accumulator anchored to a FIXED TIME WINDOW (QPC ticks): the first hit
// outside the window spawns normally, then one hit per `threshold`
// repeats inside the window spawns, each spawn re-anchoring the window.
//
// Frame-size independence is a hard requirement: the window is expressed
// in QPC time, never in render blocks, so collapsing behavior is
// IDENTICAL at 1024, 2048 or 16384 WASAPI frames. A collapsed stream of
// retriggered notes still buzzes at a deterministic floor of
// `threshold / window` Hz (e.g. 32 hits per 20 ms window = 1600 Hz).
//
// DEFAULT IS OFF (threshold 1 / window 0): every note-on spawns at its
// exact QPC timestamp and every retrigger rate is reproduced faithfully.
// Coalescing must be explicitly enabled at runtime.
//
// The threshold is rounded to a power of two so the hot path is a mask,
// not a division, and is runtime-adjustable (SetThreshold) for live
// tuning. Producers may call concurrently; all state is relaxed atomics
// and races are benign (worst case one extra or one missed spawn).
class NoteOnCollapseGate {
public:
    // Sets the spawn interval inside a window. The requested value is
    // rounded down to a power of two (minimum 2); 0 or 1 disables
    // coalescing so every hit spawns. Safe to call while running.
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

    // Sets the collapse window in QPC ticks (caller converts from
    // milliseconds using the real QPC frequency; see kCollapseWindowMs in
    // the driver). 0 disables coalescing regardless of threshold.
    void SetWindowTicks(uint64_t ticks) noexcept {
        windowTicks_.store(ticks, std::memory_order_relaxed);
    }

    uint64_t WindowTicks() const noexcept {
        return windowTicks_.load(std::memory_order_relaxed);
    }

    bool Enabled() const noexcept {
        return threshold_.load(std::memory_order_relaxed) > 1u &&
               windowTicks_.load(std::memory_order_relaxed) != 0u;
    }

    static constexpr uint32_t kKeyCount = kChannelCount * kNoteCount;
    // SnappySynth caps its stack counter at 30000; the spawned voice's
    // velocity boost saturates long before this matters, but the cap
    // keeps the counter meaningful for arbitrarily long floods.
    static constexpr uint32_t kStackCap = 30000u;

    // Returns true when the caller must push the event through ingress.
    // `qpc` is the event's raw QPC timestamp. `outStack` reports
    // velocity-stacking state: the number of note-ons this spawned voice
    // represents (inclusive of the current hit) on spawn, or the running
    // collapsed count otherwise.
    bool OnNoteOn(uint32_t keyIndex, uint64_t qpc,
                  uint32_t& outStack) noexcept {
        const uint32_t t = threshold_.load(std::memory_order_relaxed);
        if (t <= 1u) {
            outStack = 1u; // disabled: every hit spawns on its own
            return true;
        }
        const uint64_t w = windowTicks_.load(std::memory_order_relaxed);
        if (w == 0u) {
            outStack = 1u;
            return true;
        }
        const uint32_t idx = keyIndex & (kKeyCount - 1u);
        const uint64_t anchor = lastSpawnQpc_[idx].load(
            std::memory_order_relaxed);
        if (qpc - anchor >= w) {
            // New window: spawn immediately, re-anchor at this hit.
            lastSpawnQpc_[idx].store(qpc, std::memory_order_relaxed);
            windowHits_[idx].store(0u, std::memory_order_relaxed);
            stacks_[idx].store(0u, std::memory_order_relaxed);
            outStack = 1u;
            return true;
        }
        // Inside the window: spawn on every `t`-th hit, re-anchoring the
        // window at each spawn so the sustained spawn rate is exactly
        // t / window, independent of buffer size or block boundaries.
        const uint32_t hit = windowHits_[idx].fetch_add(
            1u, std::memory_order_relaxed) + 1u;
        if ((hit & (t - 1u)) == 0u) {
            lastSpawnQpc_[idx].store(qpc, std::memory_order_relaxed);
            windowHits_[idx].store(0u, std::memory_order_relaxed);
            outStack = stacks_[idx].exchange(0u,
                std::memory_order_relaxed) + 1u;
            return true;
        }
        const uint32_t prev = stacks_[idx].fetch_add(
            1u, std::memory_order_relaxed);
        outStack = prev + 1u < kStackCap ? prev + 1u : kStackCap;
        return false;
    }

    // Timestamp-only form (no stack report) for callers that collapse
    // without velocity stacking.
    bool OnNoteOn(uint32_t keyIndex, uint64_t qpc) noexcept {
        uint32_t ignoredStack = 0u;
        return OnNoteOn(keyIndex, qpc, ignoredStack);
    }

    void ResetKey(uint32_t keyIndex) noexcept {
        const uint32_t idx = keyIndex & (kKeyCount - 1u);
        lastSpawnQpc_[idx].store(0u, std::memory_order_relaxed);
        windowHits_[idx].store(0u, std::memory_order_relaxed);
        stacks_[idx].store(0u, std::memory_order_relaxed);
    }

    void ResetChannel(uint32_t channel) noexcept {
        const uint32_t base =
            (channel & (kChannelCount - 1u)) * kNoteCount;
        for (uint32_t n = 0; n < kNoteCount; ++n) {
            ResetKey(base + n);
        }
    }

    void ResetAll() noexcept {
        for (uint32_t k = 0; k < kKeyCount; ++k) {
            lastSpawnQpc_[k].store(0u, std::memory_order_relaxed);
            windowHits_[k].store(0u, std::memory_order_relaxed);
            stacks_[k].store(0u, std::memory_order_relaxed);
        }
    }

private:
    std::atomic<uint32_t> threshold_{1u}; // default: OFF
    std::atomic<uint64_t> windowTicks_{0u}; // default: OFF
    std::atomic<uint64_t> lastSpawnQpc_[kKeyCount]{};
    std::atomic<uint32_t> windowHits_[kKeyCount]{};
    std::atomic<uint32_t> stacks_[kKeyCount]{};
};

} // namespace svms

#endif
