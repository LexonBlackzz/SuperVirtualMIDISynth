#ifndef SVMS_SPSC_QUEUE_H
#define SVMS_SPSC_QUEUE_H

#include "SVMSTypes.h"
#include <atomic>
#include <cstring>

namespace svms {

// ── Timestamped MIDI event for the SPSC pipeline ──────────────────────
// Carries the raw MIDI message, a high-precision QPC timestamp from the
// host thread, and the pre-computed fractional sample offset within the
// current render block.  The render thread consumes these directly.
struct TimestampedMidiEvent {
    uint64_t qpcTimestamp;       // QueryPerformanceCounter value
    uint32_t message;            // raw MIDI message (status | data1<<8 | data2<<16)
    float    targetSampleOffset; // fractional sample position within the block
};

// ── Cache line size for false-sharing prevention ──────────────────────
static constexpr uint32_t kCacheLineSize = 64;

// ════════════════════════════════════════════════════════════════════════
// Lock-free Single-Producer Single-Consumer (SPSC) bounded queue.
//
// Producer (MIDI/host thread) calls Push().
// Consumer (audio render thread) calls TryPop() / Pop().
//
// Capacity is rounded up to the next power of two internally so that
// the modulo operation becomes a fast bitmask.
//
// head_ and tail_ sit on separate cache lines to eliminate false sharing
// between the producer and consumer cores.
// ════════════════════════════════════════════════════════════════════════
template <typename T, uint32_t Capacity>
class SPSCQueue {
public:
    SPSCQueue() : head_(0), tail_(0) {
        static_assert((Capacity & (Capacity - 1)) == 0,
                      "SPSCQueue capacity must be a power of two");
        storage_ = static_cast<T*>(::operator new(sizeof(T) * Capacity,
                                                    std::align_val_t{alignof(T)}));
    }

    ~SPSCQueue() {
        // Call destructors for any live elements.
        uint32_t h = head_.load(std::memory_order_relaxed);
        uint32_t t = tail_.load(std::memory_order_acquire);
        while (t != h) {
            storage_[t & kMask].~T();
            ++t;
        }
        ::operator delete(storage_, std::align_val_t{alignof(T)});
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // ── Producer side ───────────────────────────────────────────────────
    // Returns true if the event was enqueued; false if the queue is full.
    // Callable from any thread (the MIDI host thread).
    bool Push(const T& event) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t next = h + 1;
        if ((next - tail_.load(std::memory_order_acquire)) > Capacity)
            return false; // full
        storage_[h & kMask] = event;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // ── Consumer side ───────────────────────────────────────────────────
    // Non-blocking pop.  Returns true if an event was dequeued into `out`.
    // Callable only from the audio render thread.
    bool TryPop(T& out) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return false; // empty
        out = storage_[t & kMask];
        storage_[t & kMask].~T();
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // ── Consumer pushback ───────────────────────────────────────────────
    // Push an event back to the front of the queue (consumer side only).
    // Used to return overflow events that belong in a future render block.
    // The pushed event will be the next item popped.
    // Returns false only if the queue is completely full.
    bool TryPushBack(const T& event) {
        // Same as Push — the SPSC queue is FIFO, so pushing back an
        // event that was just popped places it at the logical front
        // of the remaining queue (after any events already drained
        // into the local event buffer for this block).
        return Push(event);
    }

    // Drain up to `maxEvents` into `outBuffer`.  Returns count drained.
    uint32_t Drain(T* outBuffer, uint32_t maxEvents) {
        uint32_t count = 0;
        while (count < maxEvents) {
            if (!TryPop(outBuffer[count]))
                break;
            ++count;
        }
        return count;
    }

    // ── Queries (safe from either thread) ───────────────────────────────
    bool IsEmpty() const {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    uint32_t Size() const {
        const uint32_t h = head_.load(std::memory_order_acquire);
        const uint32_t t = tail_.load(std::memory_order_acquire);
        return h - t;
    }

    static constexpr uint32_t CapacityValue() { return Capacity; }

    // Reset the queue.  MUST only be called when no concurrent access is
    // happening (e.g. during engine reset / shutdown).
    void Reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kMask = Capacity - 1;

    // Producer cache line.
    alignas(kCacheLineSize) std::atomic<uint32_t> head_;

    // Consumer cache line.
    alignas(kCacheLineSize) std::atomic<uint32_t> tail_;

    // Storage.
    T* storage_;
};

} // namespace svms

#endif
