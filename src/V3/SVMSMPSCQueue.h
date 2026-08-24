#ifndef SVMS_MPSC_QUEUE_H
#define SVMS_MPSC_QUEUE_H

#include "SVMSTypes.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

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

    // Reserve one contiguous producer range with a single CAS, then publish
    // each cell in FIFO order. This is all-or-nothing so callers can retain
    // their existing lossless backpressure and priority-shedding decisions.
    bool TryPushBatch(const T* values, uint32_t count) noexcept {
        if (count == 0u) return true;
        if (!values || count > Capacity) return false;
        uint64_t position = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            bool retry = false;
            for (uint32_t i = 0u; i < count; ++i) {
                Cell& cell = cells_[static_cast<size_t>(
                    (position + i) & (Capacity - 1u))];
                const uint64_t sequence =
                    cell.sequence.load(std::memory_order_acquire);
                const intptr_t difference = static_cast<intptr_t>(
                    sequence - (position + i));
                if (difference < 0) return false;
                if (difference > 0) {
                    position = enqueue_.load(std::memory_order_relaxed);
                    retry = true;
                    break;
                }
            }
            if (retry) continue;
            if (!enqueue_.compare_exchange_weak(
                    position, position + count,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                continue;
            }
            for (uint32_t i = 0u; i < count; ++i) {
                Cell& cell = cells_[static_cast<size_t>(
                    (position + i) & (Capacity - 1u))];
                cell.value = values[i];
                cell.sequence.store(position + i + 1u,
                                    std::memory_order_release);
            }
            return true;
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

// Runtime-sized bounded MPSC queue. Capacity is selected while the engine is
// stopped; producers and the single consumer then use the same sequence-cell
// algorithm as the fixed-size test queue. Modulo indexing permits the exact
// configured lane proportions instead of rounding each lane up independently.
template <typename T>
class DynamicMPSCQueue {
    static_assert(std::is_trivially_copyable_v<T>);

    struct Cell {
        std::atomic<uint64_t> sequence;
        T value;
    };

public:
    DynamicMPSCQueue() = default;
    ~DynamicMPSCQueue() { Release(); }

    DynamicMPSCQueue(const DynamicMPSCQueue&) = delete;
    DynamicMPSCQueue& operator=(const DynamicMPSCQueue&) = delete;

    bool ConfigureCapacity(uint32_t capacity) noexcept {
        if (capacity < 2u ||
            static_cast<size_t>(capacity) >
                (std::numeric_limits<size_t>::max)() / sizeof(Cell)) {
            return false;
        }
        Cell* replacement = static_cast<Cell*>(::operator new[](
            sizeof(Cell) * static_cast<size_t>(capacity),
            std::align_val_t{64}, std::nothrow));
        if (!replacement) return false;
        Release();
        cells_ = replacement;
        capacity_ = capacity;
        for (uint64_t i = 0u; i < capacity_; ++i)
            new (&cells_[i]) Cell{std::atomic<uint64_t>(i), T{}};
        enqueue_.store(0u, std::memory_order_relaxed);
        dequeue_.store(0u, std::memory_order_relaxed);
        return true;
    }

    bool TryPush(const T& value) noexcept {
        if (!cells_) return false;
        uint64_t position = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[static_cast<size_t>(position % capacity_)];
            const uint64_t sequence =
                cell.sequence.load(std::memory_order_acquire);
            const intptr_t difference =
                static_cast<intptr_t>(sequence - position);
            if (difference == 0) {
                if (enqueue_.compare_exchange_weak(
                        position, position + 1u,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    cell.value = value;
                    cell.sequence.store(position + 1u,
                                        std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_.load(std::memory_order_relaxed);
            }
        }
    }

    bool TryPushBatch(const T* values, uint32_t count) noexcept {
        if (count == 0u) return true;
        if (!cells_ || !values || count > capacity_) return false;
        uint64_t position = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            bool retry = false;
            for (uint32_t i = 0u; i < count; ++i) {
                Cell& cell = cells_[static_cast<size_t>(
                    (position + i) % capacity_)];
                const uint64_t sequence =
                    cell.sequence.load(std::memory_order_acquire);
                const intptr_t difference = static_cast<intptr_t>(
                    sequence - (position + i));
                if (difference < 0) return false;
                if (difference > 0) {
                    position = enqueue_.load(std::memory_order_relaxed);
                    retry = true;
                    break;
                }
            }
            if (retry) continue;
            if (!enqueue_.compare_exchange_weak(
                    position, position + count,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                continue;
            }
            for (uint32_t i = 0u; i < count; ++i) {
                Cell& cell = cells_[static_cast<size_t>(
                    (position + i) % capacity_)];
                cell.value = values[i];
                cell.sequence.store(position + i + 1u,
                                    std::memory_order_release);
            }
            return true;
        }
    }

    bool TryPop(T& value) noexcept {
        if (!cells_) return false;
        const uint64_t position = dequeue_.load(std::memory_order_relaxed);
        Cell& cell = cells_[static_cast<size_t>(position % capacity_)];
        const uint64_t sequence =
            cell.sequence.load(std::memory_order_acquire);
        if (static_cast<intptr_t>(sequence - (position + 1u)) != 0)
            return false;
        value = cell.value;
        cell.sequence.store(position + capacity_, std::memory_order_release);
        dequeue_.store(position + 1u, std::memory_order_relaxed);
        return true;
    }

    uint32_t Size() const noexcept {
        const uint64_t head = enqueue_.load(std::memory_order_acquire);
        const uint64_t tail = dequeue_.load(std::memory_order_acquire);
        const uint64_t size = head - tail;
        return static_cast<uint32_t>(size > capacity_ ? capacity_ : size);
    }

    uint32_t CapacityValue() const noexcept { return capacity_; }

private:
    void Release() noexcept {
        if (cells_) {
            for (uint32_t i = 0u; i < capacity_; ++i) cells_[i].~Cell();
        }
        ::operator delete[](cells_, std::align_val_t{64});
        cells_ = nullptr;
        capacity_ = 0u;
    }

    Cell* cells_ = nullptr;
    uint32_t capacity_ = 0u;
    alignas(64) std::atomic<uint64_t> enqueue_{0u};
    alignas(64) std::atomic<uint64_t> dequeue_{0u};
};

enum class EventLane : uint8_t { State, Loud, UpperMedium, Medium, Quiet };

template <typename T>
class PriorityEventIngress {
public:
    struct OrderedMergeState {
        T heads[5]{};
        uint8_t heap[5]{};
        uint8_t queued[5]{};
        uint8_t heapCount = 0u;
        uint8_t pollCountdown = 0u;
    };

    PriorityEventIngress() { (void)ConfigureCapacity(393216u); }

    bool ConfigureCapacity(uint32_t totalCapacity) noexcept {
        if (totalCapacity < 12u) return false;
        const uint32_t state = totalCapacity / 3u;
        const uint32_t loud = totalCapacity / 3u;
        const uint32_t upperMedium = totalCapacity / 6u;
        const uint32_t medium = totalCapacity / 12u;
        const uint32_t quiet =
            totalCapacity - state - loud - upperMedium - medium;
        if (!state_.ConfigureCapacity(state) ||
            !loud_.ConfigureCapacity(loud) ||
            !upperMedium_.ConfigureCapacity(upperMedium) ||
            !medium_.ConfigureCapacity(medium) ||
            !quiet_.ConfigureCapacity(quiet)) {
            return false;
        }
        laneCapacities_[0] = state;
        laneCapacities_[1] = loud;
        laneCapacities_[2] = upperMedium;
        laneCapacities_[3] = medium;
        laneCapacities_[4] = quiet;
        totalCapacity_ = totalCapacity;
        return true;
    }

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

    bool TryPushBatch(EventLane lane, const T* events,
                      uint32_t count) noexcept {
        switch (lane) {
            case EventLane::State: return state_.TryPushBatch(events, count);
            case EventLane::Loud: return loud_.TryPushBatch(events, count);
            case EventLane::UpperMedium:
                return upperMedium_.TryPushBatch(events, count);
            case EventLane::Medium: return medium_.TryPushBatch(events, count);
            case EventLane::Quiet: return quiet_.TryPushBatch(events, count);
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

    // Merge the five FIFO lane heads by the global ingress sequence. The
    // caller owns the tiny lookahead set so it persists across compiler pages.
    // This keeps adjacent immutable pages non-overlapping for ordinary hosts,
    // greatly reducing page-head winner churn without changing event order.
    bool TryPopSequenceOrdered(T& event, T (&heads)[5],
                               uint8_t (&valid)[5]) noexcept {
        for (uint32_t lane = 0u; lane < 5u; ++lane) {
            if (valid[lane]) continue;
            valid[lane] = TryPop(static_cast<EventLane>(lane), heads[lane])
                ? 1u : 0u;
        }
        uint32_t winner = UINT32_MAX;
        for (uint32_t lane = 0u; lane < 5u; ++lane) {
            if (!valid[lane]) continue;
            if (winner == UINT32_MAX ||
                static_cast<int32_t>(heads[lane].sequence -
                                     heads[winner].sequence) < 0) {
                winner = lane;
            }
        }
        if (winner == UINT32_MAX) return false;
        event = heads[winner];
        valid[winner] = 0u;
        return true;
    }

    bool TryPopSequenceOrdered(T& event,
                               OrderedMergeState& state) noexcept {
        auto earlierLane = [&state](uint8_t a, uint8_t b) noexcept {
            return static_cast<int32_t>(state.heads[a].sequence -
                                        state.heads[b].sequence) < 0;
        };
        auto siftUp = [&state, &earlierLane](uint32_t position) noexcept {
            while (position != 0u) {
                const uint32_t parent = (position - 1u) >> 1u;
                if (!earlierLane(state.heap[position], state.heap[parent]))
                    break;
                std::swap(state.heap[position], state.heap[parent]);
                position = parent;
            }
        };
        auto siftDown = [&state, &earlierLane](uint32_t position) noexcept {
            for (;;) {
                const uint32_t left = position * 2u + 1u;
                if (left >= state.heapCount) break;
                const uint32_t right = left + 1u;
                uint32_t best = left;
                if (right < state.heapCount &&
                    earlierLane(state.heap[right], state.heap[left])) {
                    best = right;
                }
                if (!earlierLane(state.heap[best], state.heap[position]))
                    break;
                std::swap(state.heap[position], state.heap[best]);
                position = best;
            }
        };

        // Saturated lanes remain in the tiny heap. Empty lanes are sampled
        // every 16 outputs; timestamps retain their exact target frames, and
        // the bounded sampling delay is far below one audio frame at dense
        // rates while avoiding three failed atomic pops per event.
        if (state.heapCount == 0u || state.pollCountdown == 0u) {
            state.pollCountdown = 15u;
            for (uint8_t lane = 0u; lane < 5u; ++lane) {
                if (state.queued[lane]) continue;
                if (!TryPop(static_cast<EventLane>(lane), state.heads[lane]))
                    continue;
                state.queued[lane] = 1u;
                const uint32_t position = state.heapCount++;
                state.heap[position] = lane;
                siftUp(position);
            }
        } else {
            --state.pollCountdown;
        }
        if (state.heapCount == 0u) return false;

        const uint8_t winner = state.heap[0];
        event = state.heads[winner];
        if (TryPop(static_cast<EventLane>(winner), state.heads[winner])) {
            siftDown(0u);
        } else {
            state.queued[winner] = 0u;
            --state.heapCount;
            if (state.heapCount != 0u) {
                state.heap[0] = state.heap[state.heapCount];
                siftDown(0u);
            }
        }
        return true;
    }

    // Drain bounded FIFO runs instead of alternating lanes for every event.
    // Admission priority and consumer fairness stay unchanged, while ordered
    // producers reach the downstream sorter as a handful of natural runs.
    template <typename Consumer>
    uint32_t DrainFairRuns(uint32_t maxEvents, uint32_t runQuota,
                           uint32_t& cursor, Consumer&& consume) noexcept {
        if (maxEvents == 0u) return 0u;
        if (runQuota == 0u) runQuota = 1u;
        uint32_t drained = 0u;
        T event{};
        while (drained < maxEvents) {
            bool madeProgress = false;
            for (uint32_t attempt = 0u;
                 attempt < 5u && drained < maxEvents; ++attempt) {
                const EventLane lane = static_cast<EventLane>(cursor++ % 5u);
                uint32_t laneCount = 0u;
                while (laneCount < runQuota && drained < maxEvents &&
                       TryPop(lane, event)) {
                    consume(event);
                    ++laneCount;
                    ++drained;
                    madeProgress = true;
                }
            }
            if (!madeProgress) break;
        }
        return drained;
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

    uint32_t LaneCapacity(EventLane lane) const noexcept {
        const uint32_t index = static_cast<uint32_t>(lane);
        return index < 5u ? laneCapacities_[index] : 1u;
    }

    uint32_t DrainAvailable() noexcept {
        uint32_t drained = 0;
        T event{};
        while (TryPop(event)) ++drained;
        return drained;
    }

    uint32_t TotalCapacity() const noexcept { return totalCapacity_; }

private:
    DynamicMPSCQueue<T> state_;
    DynamicMPSCQueue<T> loud_;
    DynamicMPSCQueue<T> upperMedium_;
    DynamicMPSCQueue<T> medium_;
    DynamicMPSCQueue<T> quiet_;
    uint32_t laneCapacities_[5]{131072u, 131072u, 65536u, 32768u, 32768u};
    uint32_t totalCapacity_ = 393216u;
};

} // namespace svms

#endif
