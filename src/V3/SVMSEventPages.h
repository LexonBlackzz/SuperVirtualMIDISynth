#ifndef SVMS_EVENT_PAGES_H
#define SVMS_EVENT_PAGES_H

#include "SVMSEventScheduler.h"
#include "SVMSPSCQueue.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace svms {

static constexpr uint32_t kCompiledEventPageCapacity = 8192u;
static constexpr uint32_t kInvalidEventPage = UINT32_MAX;

inline bool ScheduledEventEarlier(const ScheduledRenderEvent& a,
                                  const ScheduledRenderEvent& b) noexcept {
    if (a.targetFrame != b.targetFrame) return a.targetFrame < b.targetFrame;
    // A live queue can contain far fewer than 2^31 events, so signed modular
    // subtraction gives the intended order even when the ingress counter
    // wraps from UINT32_MAX to zero.
    return static_cast<int32_t>(a.sequence - b.sequence) < 0;
}

struct alignas(64) CompiledEventPage {
    uint32_t count = 0u;
    uint32_t reserved[15]{};
    ScheduledRenderEvent events[kCompiledEventPageCapacity];
};

// Stable allocation-free radix ordering for one compiler page. targetFrame is
// the primary key and the modular ingress sequence is the equal-frame key.
// Both supported paths use an even pass count, so the result finishes in the
// original page rather than requiring another payload copy.
inline void SortCompiledEventPage(ScheduledRenderEvent* events,
                                  ScheduledRenderEvent* scratch,
                                  uint32_t count) noexcept {
    if (!events || !scratch || count < 2u) return;

    // Normal compiler pages are a concatenation of bounded FIFO lane runs.
    // Merge that tiny run set instead of paying eight full radix passes.
    static constexpr uint32_t kMaxNaturalRuns = 64u;
    uint32_t beginsA[kMaxNaturalRuns]{};
    uint32_t endsA[kMaxNaturalRuns]{};
    uint32_t beginsB[kMaxNaturalRuns]{};
    uint32_t endsB[kMaxNaturalRuns]{};
    uint32_t runCount = 1u;
    beginsA[0] = 0u;
    bool tooManyRuns = false;
    for (uint32_t i = 1u; i < count; ++i) {
        if (!ScheduledEventEarlier(events[i], events[i - 1u])) continue;
        endsA[runCount - 1u] = i;
        if (runCount == kMaxNaturalRuns) {
            tooManyRuns = true;
            break;
        }
        beginsA[runCount++] = i;
    }
    if (!tooManyRuns) {
        endsA[runCount - 1u] = count;
        if (runCount == 1u) return;
        ScheduledRenderEvent* source = events;
        ScheduledRenderEvent* destination = scratch;
        uint32_t* begins = beginsA;
        uint32_t* ends = endsA;
        uint32_t* nextBegins = beginsB;
        uint32_t* nextEnds = endsB;
        while (runCount > 1u) {
            uint32_t nextCount = 0u;
            for (uint32_t run = 0u; run < runCount; run += 2u) {
                const uint32_t outputBegin = begins[run];
                nextBegins[nextCount] = outputBegin;
                if (run + 1u == runCount) {
                    const uint32_t outputEnd = ends[run];
                    std::memcpy(destination + outputBegin,
                                source + outputBegin,
                                static_cast<size_t>(outputEnd - outputBegin) *
                                    sizeof(ScheduledRenderEvent));
                    nextEnds[nextCount++] = outputEnd;
                    continue;
                }
                uint32_t left = begins[run];
                const uint32_t leftEnd = ends[run];
                uint32_t right = begins[run + 1u];
                const uint32_t rightEnd = ends[run + 1u];
                uint32_t output = outputBegin;
                while (left < leftEnd && right < rightEnd) {
                    if (ScheduledEventEarlier(source[right], source[left]))
                        destination[output++] = source[right++];
                    else
                        destination[output++] = source[left++];
                }
                if (left < leftEnd) {
                    std::memcpy(destination + output, source + left,
                        static_cast<size_t>(leftEnd - left) *
                            sizeof(ScheduledRenderEvent));
                } else if (right < rightEnd) {
                    std::memcpy(destination + output, source + right,
                        static_cast<size_t>(rightEnd - right) *
                            sizeof(ScheduledRenderEvent));
                }
                nextEnds[nextCount++] = rightEnd;
            }
            std::swap(source, destination);
            std::swap(begins, nextBegins);
            std::swap(ends, nextEnds);
            runCount = nextCount;
        }
        if (source != events) {
            std::memcpy(events, source,
                        static_cast<size_t>(count) *
                            sizeof(ScheduledRenderEvent));
        }
        return;
    }

    int64_t minimumFrame = events[0].targetFrame;
    int64_t maximumFrame = minimumFrame;
    uint32_t sequenceBase = events[0].sequence;
    for (uint32_t i = 1u; i < count; ++i) {
        minimumFrame = (std::min)(minimumFrame, events[i].targetFrame);
        maximumFrame = (std::max)(maximumFrame, events[i].targetFrame);
        if (static_cast<int32_t>(events[i].sequence - sequenceBase) < 0)
            sequenceBase = events[i].sequence;
    }

    const uint64_t orderedMinimum =
        static_cast<uint64_t>(minimumFrame) ^ (uint64_t{1} << 63u);
    const uint64_t orderedMaximum =
        static_cast<uint64_t>(maximumFrame) ^ (uint64_t{1} << 63u);
    const bool compactFrame = orderedMaximum - orderedMinimum <= UINT32_MAX;
    const uint32_t passCount = compactFrame ? 8u : 12u;

    ScheduledRenderEvent* source = events;
    ScheduledRenderEvent* destination = scratch;
    for (uint32_t pass = 0u; pass < passCount; ++pass) {
        size_t offsets[256]{};
        auto byteFor = [pass, compactFrame, minimumFrame, sequenceBase](
                           const ScheduledRenderEvent& event) noexcept {
            if (pass < 4u) {
                const uint32_t relativeSequence =
                    event.sequence - sequenceBase;
                return static_cast<uint8_t>(relativeSequence >> (pass * 8u));
            }
            if (compactFrame) {
                const uint32_t relativeFrame = static_cast<uint32_t>(
                    event.targetFrame - minimumFrame);
                return static_cast<uint8_t>(
                    relativeFrame >> ((pass - 4u) * 8u));
            }
            const uint32_t frameByte = pass - 4u;
            uint8_t value = static_cast<uint8_t>(
                static_cast<uint64_t>(event.targetFrame) >>
                (frameByte * 8u));
            if (frameByte == 7u) value ^= 0x80u;
            return value;
        };

        for (uint32_t i = 0u; i < count; ++i)
            ++offsets[byteFor(source[i])];
        size_t running = 0u;
        for (uint32_t bucket = 0u; bucket < 256u; ++bucket) {
            const size_t bucketCount = offsets[bucket];
            offsets[bucket] = running;
            running += bucketCount;
        }
        for (uint32_t i = 0u; i < count; ++i) {
            const uint8_t bucket = byteFor(source[i]);
            destination[offsets[bucket]++] = source[i];
        }
        std::swap(source, destination);
    }
}

// One compiler producer and one audio-thread consumer own this pool. Page
// payloads are immutable between Publish and Recycle. The two index queues are
// SPSC, so publication and recycling require no locks or allocation.
class CompiledEventPagePool {
public:
    CompiledEventPagePool() = default;
    ~CompiledEventPagePool() { ReleaseStorage(); }

    CompiledEventPagePool(const CompiledEventPagePool&) = delete;
    CompiledEventPagePool& operator=(const CompiledEventPagePool&) = delete;

    bool ConfigureCapacity(uint32_t eventCapacity) noexcept {
        ReleaseStorage();
        if (eventCapacity == 0u) return false;
        const uint64_t requiredPages =
            (static_cast<uint64_t>(eventCapacity) +
             kCompiledEventPageCapacity - 1u) /
            kCompiledEventPageCapacity;
        const uint64_t pageCount64 = requiredPages + 2u;
        if (pageCount64 > UINT32_MAX ||
            pageCount64 > (std::numeric_limits<size_t>::max)() /
                              sizeof(CompiledEventPage)) {
            return false;
        }
        const uint32_t pageCount = static_cast<uint32_t>(pageCount64);
        pages_ = static_cast<CompiledEventPage*>(::operator new(
            sizeof(CompiledEventPage) * static_cast<size_t>(pageCount),
            std::align_val_t{64}, std::nothrow));
        scratch_ = static_cast<ScheduledRenderEvent*>(::operator new(
            sizeof(ScheduledRenderEvent) * kCompiledEventPageCapacity,
            std::align_val_t{64}, std::nothrow));
        if (!pages_ || !scratch_ || !ready_.ConfigureCapacity(pageCount) ||
            !recycled_.ConfigureCapacity(pageCount)) {
            ReleaseStorage();
            return false;
        }
        try {
            compilerFree_.resize(pageCount);
        } catch (...) {
            ReleaseStorage();
            return false;
        }
        pageCount_ = pageCount;
        compilerFreeTop_ = pageCount;
        for (uint32_t i = 0u; i < pageCount; ++i) {
            new (&pages_[i]) CompiledEventPage{};
            compilerFree_[i] = pageCount - 1u - i;
        }
        readyEventCount_.store(0u, std::memory_order_relaxed);
        highWaterPages_.store(0u, std::memory_order_relaxed);
        return true;
    }

    bool AcquireForCompiler(uint32_t& pageIndex) noexcept {
        if (recycled_.TryPop(pageIndex)) return true;
        if (compilerFreeTop_ == 0u) return false;
        pageIndex = compilerFree_[--compilerFreeTop_];
        return true;
    }

    bool PublishFromCompiler(uint32_t pageIndex, uint32_t count) noexcept {
        if (pageIndex >= pageCount_ || count == 0u ||
            count > kCompiledEventPageCapacity) {
            return false;
        }
        pages_[pageIndex].count = count;
        if (!ready_.Push(pageIndex)) return false;
        readyEventCount_.fetch_add(count, std::memory_order_release);
        const uint32_t pagesReady = ready_.Size();
        uint32_t high = highWaterPages_.load(std::memory_order_relaxed);
        while (pagesReady > high &&
               !highWaterPages_.compare_exchange_weak(
                   high, pagesReady, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {}
        return true;
    }

    void ReturnUnusedFromCompiler(uint32_t pageIndex) noexcept {
        if (pageIndex < pageCount_ && compilerFreeTop_ < pageCount_) {
            pages_[pageIndex].count = 0u;
            compilerFree_[compilerFreeTop_++] = pageIndex;
        }
    }

    bool TakeReadyForAudio(uint32_t& pageIndex) noexcept {
        if (!ready_.TryPop(pageIndex)) return false;
        readyEventCount_.fetch_sub(pages_[pageIndex].count,
                                   std::memory_order_relaxed);
        return true;
    }

    bool RecycleFromAudio(uint32_t pageIndex) noexcept {
        if (pageIndex >= pageCount_) return false;
        pages_[pageIndex].count = 0u;
        return recycled_.Push(pageIndex);
    }

    CompiledEventPage& Page(uint32_t index) noexcept { return pages_[index]; }
    const CompiledEventPage& Page(uint32_t index) const noexcept {
        return pages_[index];
    }
    ScheduledRenderEvent* SortScratch() noexcept { return scratch_; }
    uint32_t PageCount() const noexcept { return pageCount_; }
    uint32_t StorageEventCapacity() const noexcept {
        const uint64_t capacity = static_cast<uint64_t>(pageCount_) *
                                  kCompiledEventPageCapacity;
        return capacity > UINT32_MAX ? UINT32_MAX
                                    : static_cast<uint32_t>(capacity);
    }
    uint32_t ReadyPageCount() const noexcept { return ready_.Size(); }
    uint32_t ReadyEventCount() const noexcept {
        return readyEventCount_.load(std::memory_order_acquire);
    }
    uint32_t HighWaterPages() const noexcept {
        return highWaterPages_.load(std::memory_order_relaxed);
    }

private:
    void ReleaseStorage() noexcept {
        if (pages_) {
            for (uint32_t i = 0u; i < pageCount_; ++i)
                pages_[i].~CompiledEventPage();
        }
        ::operator delete(pages_, std::align_val_t{64});
        ::operator delete(scratch_, std::align_val_t{64});
        pages_ = nullptr;
        scratch_ = nullptr;
        pageCount_ = 0u;
        compilerFreeTop_ = 0u;
        compilerFree_.clear();
        readyEventCount_.store(0u, std::memory_order_relaxed);
    }

    CompiledEventPage* pages_ = nullptr;
    ScheduledRenderEvent* scratch_ = nullptr;
    uint32_t pageCount_ = 0u;
    std::vector<uint32_t> compilerFree_;
    uint32_t compilerFreeTop_ = 0u;
    DynamicSPSCQueue<uint32_t> ready_;
    DynamicSPSCQueue<uint32_t> recycled_;
    std::atomic<uint32_t> readyEventCount_{0u};
    std::atomic<uint32_t> highWaterPages_{0u};
};

// Audio-thread-owned k-way merge over immutable compiler pages. Tree leaves
// hold page cursors, and internal nodes hold the page whose current head wins.
// Future payloads remain where the compiler wrote them.
class PagedEventScheduler {
public:
    bool Configure(CompiledEventPagePool* pool,
                   uint32_t eventCapacity) noexcept {
        pool_ = pool;
        capacity_ = eventCapacity;
        if (!pool_ || pool_->PageCount() == 0u || capacity_ == 0u)
            return false;
        const uint32_t slots = pool_->PageCount();
        capacity_ = (std::max)(capacity_, pool_->StorageEventCapacity());
        uint32_t leafCount = 1u;
        while (leafCount < slots) {
            if (leafCount > UINT32_MAX / 2u) return false;
            leafCount *= 2u;
        }
        try {
            cursors_.assign(slots, PageCursor{});
            freeSlots_.resize(slots);
            tree_.assign(static_cast<size_t>(leafCount) * 2u,
                         kInvalidEventPage);
        } catch (...) {
            return false;
        }
        leafCount_ = leafCount;
        freeTop_ = slots;
        for (uint32_t i = 0u; i < slots; ++i)
            freeSlots_[i] = slots - 1u - i;
        size_ = 0u;
        highWater_ = 0u;
        activePages_ = 0u;
        return true;
    }

    bool ImportReadyPage() noexcept {
        if (!pool_ || freeTop_ == 0u) return false;
        uint32_t pageIndex = kInvalidEventPage;
        if (!pool_->TakeReadyForAudio(pageIndex)) return false;
        const uint32_t count = pool_->Page(pageIndex).count;
        if (count == 0u || size_ + count > capacity_) {
            // A page can be temporarily refused only when logical scheduled
            // capacity is exhausted. It remains safe to recycle because the
            // pool itself is sized from that same capacity plus two pages;
            // normal operation cannot reach this branch.
            pool_->RecycleFromAudio(pageIndex);
            return false;
        }
        const uint32_t slot = freeSlots_[--freeTop_];
        cursors_[slot] = {pageIndex, 0u, count, true};
        size_ += count;
        highWater_ = (std::max)(highWater_, size_);
        ++activePages_;
        UpdateLeaf(slot);
        return true;
    }

    uint32_t ImportAllReady() noexcept {
        uint32_t imported = 0u;
        while (ImportReadyPage()) ++imported;
        return imported;
    }

    bool PopBefore(int64_t endFrame, ScheduledRenderEvent& out) noexcept {
        const ScheduledRenderEvent* run = nullptr;
        if (PeekRunBefore(endFrame, 1u, run) == 0u) return false;
        out = *run;
        ConsumeRun(1u);
        return true;
    }

    // Return the longest contiguous prefix of the current winning page that
    // remains ahead of every other page head. Consumers can walk the immutable
    // payload directly and then update the tree once for the entire run.
    uint32_t PeekRunBefore(int64_t endFrame, uint32_t maximumCount,
                           const ScheduledRenderEvent*& events) const noexcept {
        events = nullptr;
        if (tree_.empty() || maximumCount == 0u) return 0u;
        const uint32_t slot = tree_[1u];
        if (slot == kInvalidEventPage) return 0u;
        const PageCursor& cursor = cursors_[slot];
        const CompiledEventPage& page = pool_->Page(cursor.pageIndex);
        if (page.events[cursor.offset].targetFrame >= endFrame) return 0u;

        uint32_t runner = kInvalidEventPage;
        uint32_t node = leafCount_ + slot;
        while (node > 1u) {
            const uint32_t sibling = node ^ 1u;
            runner = BetterSlot(runner, tree_[sibling]);
            node >>= 1u;
        }
        const ScheduledRenderEvent* runnerEvent = nullptr;
        if (runner != kInvalidEventPage) {
            const PageCursor& other = cursors_[runner];
            runnerEvent = &pool_->Page(other.pageIndex).events[other.offset];
        }

        const uint32_t available = cursor.count - cursor.offset;
        const uint32_t limit = (std::min)(available, maximumCount);
        uint32_t count = 0u;
        const ScheduledRenderEvent* begin = page.events + cursor.offset;
        while (count < limit && begin[count].targetFrame < endFrame &&
               (!runnerEvent ||
                ScheduledEventEarlier(begin[count], *runnerEvent))) {
            ++count;
        }
        // The root always wins against the runner, including the equal-key
        // deterministic left-page tie, so at least its head is consumable.
        if (count == 0u) count = 1u;
        events = begin;
        return count;
    }

    void ConsumeRun(uint32_t count) noexcept {
        if (count == 0u || tree_.empty()) return;
        const uint32_t slot = tree_[1u];
        if (slot == kInvalidEventPage) return;
        PageCursor& cursor = cursors_[slot];
        const uint32_t available = cursor.count - cursor.offset;
        if (count > available) count = available;
        cursor.offset += count;
        size_ -= count;
        if (cursor.offset == cursor.count) {
            const uint32_t pageIndex = cursor.pageIndex;
            cursor = PageCursor{};
            freeSlots_[freeTop_++] = slot;
            --activePages_;
            UpdateLeaf(slot);
            const bool recycled = pool_->RecycleFromAudio(pageIndex);
            (void)recycled;
        } else {
            UpdateLeaf(slot);
        }
    }

    uint32_t Size() const noexcept { return size_; }
    uint32_t Capacity() const noexcept { return capacity_; }
    uint32_t HighWater() const noexcept { return highWater_; }
    uint32_t ActivePages() const noexcept { return activePages_; }

    void Reset() noexcept {
        if (pool_) {
            for (PageCursor& cursor : cursors_) {
                if (cursor.active)
                    pool_->RecycleFromAudio(cursor.pageIndex);
            }
        }
        for (PageCursor& cursor : cursors_) cursor = PageCursor{};
        const uint32_t slots = static_cast<uint32_t>(cursors_.size());
        freeTop_ = slots;
        for (uint32_t i = 0u; i < slots; ++i)
            freeSlots_[i] = slots - 1u - i;
        std::fill(tree_.begin(), tree_.end(), kInvalidEventPage);
        size_ = 0u;
        highWater_ = 0u;
        activePages_ = 0u;
    }

private:
    struct PageCursor {
        uint32_t pageIndex = kInvalidEventPage;
        uint32_t offset = 0u;
        uint32_t count = 0u;
        bool active = false;
    };

    uint32_t BetterSlot(uint32_t a, uint32_t b) const noexcept {
        if (a == kInvalidEventPage) return b;
        if (b == kInvalidEventPage) return a;
        const PageCursor& ca = cursors_[a];
        const PageCursor& cb = cursors_[b];
        const ScheduledRenderEvent& ea =
            pool_->Page(ca.pageIndex).events[ca.offset];
        const ScheduledRenderEvent& eb =
            pool_->Page(cb.pageIndex).events[cb.offset];
        return ScheduledEventEarlier(eb, ea) ? b : a;
    }

    void UpdateLeaf(uint32_t slot) noexcept {
        uint32_t node = leafCount_ + slot;
        tree_[node] = cursors_[slot].active ? slot : kInvalidEventPage;
        while (node > 1u) {
            node >>= 1u;
            tree_[node] = BetterSlot(tree_[node << 1u],
                                     tree_[(node << 1u) | 1u]);
        }
    }

    CompiledEventPagePool* pool_ = nullptr;
    uint32_t capacity_ = 0u;
    uint32_t leafCount_ = 0u;
    uint32_t freeTop_ = 0u;
    uint32_t size_ = 0u;
    uint32_t highWater_ = 0u;
    uint32_t activePages_ = 0u;
    std::vector<PageCursor> cursors_;
    std::vector<uint32_t> freeSlots_;
    std::vector<uint32_t> tree_;
};

} // namespace svms

#endif
