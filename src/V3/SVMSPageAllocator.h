#ifndef SVMS_PAGE_ALLOCATOR_H
#define SVMS_PAGE_ALLOCATOR_H

#include "SVMSTypes.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace svms {

class PageAllocator {
public:
    PageAllocator();
    ~PageAllocator();

    bool Initialize(uint32_t maxPages, uint32_t sampleRate);
    void Reset();

    uint32_t AllocatePage();
    void ReleasePage(uint32_t pageId);
    SamplePage* GetPage(uint32_t pageId);
    const SamplePage* GetPage(uint32_t pageId) const;

    uint32_t GetMaxPages() const;
    uint32_t GetUsedPages() const;
    uint32_t GetSampleRate() const;

private:
    SamplePage* pages_;
    uint32_t* freeList_;
    uint32_t freeCount_;
    uint32_t maxPages_;
    uint32_t usedPages_;
    uint32_t sampleRate_;
};

inline PageAllocator::PageAllocator()
    : pages_(nullptr), freeList_(nullptr), freeCount_(0),
      maxPages_(0), usedPages_(0), sampleRate_(0) {}

inline PageAllocator::~PageAllocator() {
    free(pages_);
    free(freeList_);
}

inline bool PageAllocator::Initialize(uint32_t maxPages, uint32_t sampleRate) {
    pages_ = static_cast<SamplePage*>(calloc(maxPages, sizeof(SamplePage)));
    freeList_ = static_cast<uint32_t*>(malloc(maxPages * sizeof(uint32_t)));
    if (!pages_ || !freeList_) {
        free(pages_);
        free(freeList_);
        pages_ = nullptr;
        freeList_ = nullptr;
        return false;
    }
    maxPages_ = maxPages;
    sampleRate_ = sampleRate;
    Reset();
    return true;
}

inline void PageAllocator::Reset() {
    for (uint32_t i = 0; i < maxPages_; ++i) {
        freeList_[i] = i;
    }
    freeCount_ = maxPages_;
    usedPages_ = 0;
}

inline uint32_t PageAllocator::AllocatePage() {
    if (freeCount_ == 0) return UINT32_MAX;
    uint32_t pageId = freeList_[--freeCount_];
    std::memset(&pages_[pageId], 0, sizeof(SamplePage));
    pages_[pageId].sampleRate = sampleRate_;
    ++usedPages_;
    return pageId;
}

inline void PageAllocator::ReleasePage(uint32_t pageId) {
    if (pageId < maxPages_) {
        freeList_[freeCount_++] = pageId;
        --usedPages_;
    }
}

inline SamplePage* PageAllocator::GetPage(uint32_t pageId) {
    return (pageId < maxPages_) ? &pages_[pageId] : nullptr;
}

inline const SamplePage* PageAllocator::GetPage(uint32_t pageId) const {
    return (pageId < maxPages_) ? &pages_[pageId] : nullptr;
}

inline uint32_t PageAllocator::GetMaxPages() const { return maxPages_; }
inline uint32_t PageAllocator::GetUsedPages() const { return usedPages_; }
inline uint32_t PageAllocator::GetSampleRate() const { return sampleRate_; }

} // namespace svms

#endif
