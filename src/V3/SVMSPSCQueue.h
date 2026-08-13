#ifndef SVMS_SPSC_QUEUE_H
#define SVMS_SPSC_QUEUE_H

#include "SVMSTypes.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace svms {

// ── Timestamped MIDI event for the SPSC pipeline ──────────────────────
// Carries the raw MIDI message, QPC timestamp, and an ingress sequence.
// The sequence replaces the old unused targetSampleOffset field, keeping
// this structure at 16 bytes for the normal ABI.
// ── Cache line size for false-sharing prevention ──────────────────────
static constexpr uint32_t kCacheLineSize = 64;

// ════════════════════════════════════════════════════════════════════════
// Lock-free Single-Producer Single-Consumer (SPSC) bounded queue.
//
// Producer (MIDI/host thread) calls Push().
// Consumer (audio render thread) calls TryPop() / Pop().
//
// Capacity is exact. Monotonic counters make full/empty unambiguous even
// when the logical capacity is not a power of two (393216 is intentional).
//
// head_ and tail_ sit on separate cache lines to eliminate false sharing
// between the producer and consumer cores.
// ════════════════════════════════════════════════════════════════════════
template <typename T, uint32_t Capacity>
class SPSCQueue {
public:
    SPSCQueue() : head_(0), tail_(0) {
        static_assert(Capacity > 0, "SPSCQueue capacity must be non-zero");
        storage_ = static_cast<T*>(::operator new(sizeof(T) * Capacity,
                                                    std::align_val_t{alignof(T)}));
    }

    ~SPSCQueue() {
        // Call destructors for any live elements.
        uint32_t h = head_.load(std::memory_order_relaxed);
        uint32_t t = tail_.load(std::memory_order_acquire);
        while (t != h) {
            storage_[Index(t)].~T();
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
        storage_[Index(h)] = event;
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
        out = storage_[Index(t)];
        storage_[Index(t)].~T();
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
    static constexpr uint32_t Index(uint32_t counter) {
        return counter % Capacity;
    }

    // Producer cache line.
    alignas(kCacheLineSize) std::atomic<uint32_t> head_;

    // Consumer cache line.
    alignas(kCacheLineSize) std::atomic<uint32_t> tail_;

    // Storage.
    T* storage_;
};

// Runtime-sized counterpart used by configuration-controlled queues.  It is
// configured only while the engine is stopped, then has the same allocation-
// free SPSC hot path as the fixed-capacity queue above.  64-bit monotonic
// cursors keep even a UINT32_MAX-sized logical capacity unambiguous.
template <typename T>
class DynamicSPSCQueue {
public:
    DynamicSPSCQueue() = default;
    ~DynamicSPSCQueue() {
        ::operator delete(storage_, std::align_val_t{alignof(T)});
    }

    DynamicSPSCQueue(const DynamicSPSCQueue&) = delete;
    DynamicSPSCQueue& operator=(const DynamicSPSCQueue&) = delete;

    bool ConfigureCapacity(uint32_t capacity) noexcept {
        static_assert(std::is_trivially_copyable<T>::value,
                      "DynamicSPSCQueue requires trivial records");
        if (capacity == 0u ||
            static_cast<size_t>(capacity) >
                (std::numeric_limits<size_t>::max)() / sizeof(T)) {
            return false;
        }
        T* replacement = static_cast<T*>(::operator new(
            sizeof(T) * static_cast<size_t>(capacity),
            std::align_val_t{alignof(T)}, std::nothrow));
        if (!replacement) return false;
        ::operator delete(storage_, std::align_val_t{alignof(T)});
        storage_ = replacement;
        capacity_ = capacity;
        Reset();
        return true;
    }

    bool Push(const T& event) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= capacity_)
            return false;
        storage_[Index(head)] = event;
        head_.store(head + 1u, std::memory_order_release);
        return true;
    }

    bool TryPop(T& out) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = storage_[Index(tail)];
        tail_.store(tail + 1u, std::memory_order_release);
        return true;
    }

    bool IsEmpty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    uint32_t Size() const noexcept {
        const uint64_t size = head_.load(std::memory_order_acquire) -
                              tail_.load(std::memory_order_acquire);
        return static_cast<uint32_t>(size);
    }

    uint32_t CapacityValue() const noexcept { return capacity_; }

    void Reset() noexcept {
        head_.store(0u, std::memory_order_relaxed);
        tail_.store(0u, std::memory_order_relaxed);
    }

private:
    size_t Index(uint64_t cursor) const noexcept {
        return static_cast<size_t>(cursor % capacity_);
    }

    alignas(kCacheLineSize) std::atomic<uint64_t> head_{0u};
    alignas(kCacheLineSize) std::atomic<uint64_t> tail_{0u};
    T* storage_ = nullptr;
    uint32_t capacity_ = 0u;
};

} // namespace svms

#endif
