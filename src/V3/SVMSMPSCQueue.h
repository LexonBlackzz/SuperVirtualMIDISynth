#ifndef SVMS_MPSC_QUEUE_H
#define SVMS_MPSC_QUEUE_H

#include "SVMSTypes.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace svms {

template <typename T, uint32_t Capacity>
class MPSCQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "MPSC capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);

    struct Cell {
        std::atomic<uint64_t> sequence;
        T value;
    };

public:
    MPSCQueue() : enqueue_(0), dequeue_(0) {
        cells_ = static_cast<Cell*>(::operator new[](sizeof(Cell) * Capacity,
                                                     std::align_val_t{64}));
        for (uint64_t i = 0; i < Capacity; ++i)
            new (&cells_[i]) Cell{std::atomic<uint64_t>(i), T{}};
    }

    ~MPSCQueue() {
        for (uint32_t i = 0; i < Capacity; ++i) cells_[i].~Cell();
        ::operator delete[](cells_, std::align_val_t{64});
    }

    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    bool TryPush(const T& value) noexcept {
        uint64_t position = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[position & (Capacity - 1u)];
            const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence - position);
            if (difference == 0) {
                if (enqueue_.compare_exchange_weak(position, position + 1,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    cell.value = value;
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_.load(std::memory_order_relaxed);
            }
        }
    }

    bool TryPop(T& value) noexcept {
        const uint64_t position = dequeue_.load(std::memory_order_relaxed);
        Cell& cell = cells_[position & (Capacity - 1u)];
        const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
        if (static_cast<intptr_t>(sequence - (position + 1)) != 0) return false;
        value = cell.value;
        cell.sequence.store(position + Capacity, std::memory_order_release);
        dequeue_.store(position + 1, std::memory_order_relaxed);
        return true;
    }

    uint32_t Size() const noexcept {
        const uint64_t head = enqueue_.load(std::memory_order_acquire);
        const uint64_t tail = dequeue_.load(std::memory_order_acquire);
        const uint64_t size = head - tail;
        return static_cast<uint32_t>(size > Capacity ? Capacity : size);
    }

private:
    Cell* cells_ = nullptr;
    alignas(64) std::atomic<uint64_t> enqueue_;
    alignas(64) std::atomic<uint64_t> dequeue_;
};

enum class EventLane : uint8_t { State, Loud, UpperMedium, Medium, Quiet };

template <typename T>
class PriorityEventIngress {
public:
    bool TryPush(EventLane lane, const T& event) noexcept {
        switch (lane) {
            case EventLane::State: return state_.TryPush(event);
            case EventLane::Loud: return loud_.TryPush(event);
            case EventLane::UpperMedium: return upperMedium_.TryPush(event);
            case EventLane::Medium: return medium_.TryPush(event);
            case EventLane::Quiet: return quiet_.TryPush(event);
        }
        return false;
    }

    bool TryPop(EventLane lane, T& event) noexcept {
        switch (lane) {
            case EventLane::State: return state_.TryPop(event);
            case EventLane::Loud: return loud_.TryPop(event);
            case EventLane::UpperMedium: return upperMedium_.TryPop(event);
            case EventLane::Medium: return medium_.TryPop(event);
            case EventLane::Quiet: return quiet_.TryPop(event);
        }
        return false;
    }

    bool TryPop(T& event) noexcept {
        return state_.TryPop(event) || loud_.TryPop(event) ||
               upperMedium_.TryPop(event) || medium_.TryPop(event) ||
               quiet_.TryPop(event);
    }

    // Fair consumer-side drain. Priority affects admission/backpressure, but
    // must not let a saturated state/note-off lane permanently starve loud
    // note-ons. Absolute frame and ingress sequence restore semantic order
    // after compilation, so the physical lane drain order is irrelevant.
    bool TryPopFair(T& event, uint32_t& cursor) noexcept {
        for (uint32_t attempt = 0u; attempt < 5u; ++attempt) {
            const uint32_t lane = cursor++ % 5u;
            bool popped = false;
            switch (lane) {
                case 0u: popped = state_.TryPop(event); break;
                case 1u: popped = loud_.TryPop(event); break;
                case 2u: popped = upperMedium_.TryPop(event); break;
                case 3u: popped = medium_.TryPop(event); break;
                default: popped = quiet_.TryPop(event); break;
            }
            if (popped) return true;
        }
        return false;
    }

    uint32_t TotalSize() const noexcept {
        return state_.Size() + loud_.Size() + upperMedium_.Size() +
               medium_.Size() + quiet_.Size();
    }

    uint32_t LaneSize(EventLane lane) const noexcept {
        switch (lane) {
            case EventLane::State: return state_.Size();
            case EventLane::Loud: return loud_.Size();
            case EventLane::UpperMedium: return upperMedium_.Size();
            case EventLane::Medium: return medium_.Size();
            case EventLane::Quiet: return quiet_.Size();
        }
        return 0u;
    }

    static constexpr uint32_t LaneCapacity(EventLane lane) noexcept {
        switch (lane) {
            case EventLane::State: return 131072u;
            case EventLane::Loud: return 131072u;
            case EventLane::UpperMedium: return 65536u;
            case EventLane::Medium: return 32768u;
            case EventLane::Quiet: return 32768u;
        }
        return 1u;
    }

    uint32_t DrainAvailable() noexcept {
        uint32_t drained = 0;
        T event{};
        while (TryPop(event)) ++drained;
        return drained;
    }

    static constexpr uint32_t TotalCapacity() noexcept { return 393216; }

private:
    MPSCQueue<T, 131072> state_;
    MPSCQueue<T, 131072> loud_;
    MPSCQueue<T, 65536> upperMedium_;
    MPSCQueue<T, 32768> medium_;
    MPSCQueue<T, 32768> quiet_;
};

} // namespace svms

#endif
