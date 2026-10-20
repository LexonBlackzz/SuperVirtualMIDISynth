// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.

inline bool RenderScalar::PrepareWholeVoice(VoiceManager& voices) {
    const uint32_t handleCapacity = voices.v.GetCapacity();
    return ReserveVoiceCapacity(voices.GetMaxVoices()) &&
           ReserveWholeVoiceStorage(handleCapacity) &&
           EnsureWholeVoiceJobScratch(
               workerPool_ ? workerPool_->GetThreadCount() * kWvJobsPerThread
                           : 1u) &&
           EnsureWholeVoiceRowOpStorage(wvRowOpBudget_) &&
           EnsureWholeVoiceExciteStorage(handleCapacity) &&
           SyncWholeVoiceRotationScratch(voices.v.rot != nullptr);
}

inline bool RenderScalar::ReserveWholeVoiceStorage(uint32_t handleCapacity) {
    const uint32_t cap = scratchCapacity_;
    if (cap == 0u || handleCapacity == 0u) return false;
    if (wvStartFrame_ != nullptr && wvGhostCapacity_ >= cap &&
        wvOpCapacity_ >= cap && wvHandleCapacity_ >= handleCapacity) {
        return true;
    }
    // Per-handle timeline arrays (wvStartFrame_/wvReleaseFrame_) are indexed
    // by RAW voice handle (0..maxVoices-1), which can be far above the active
    // count in a grown or partially-filled pool.  Everything else is indexed
    // by active-count rows (0..scratchCapacity_-1).  Allocate all-or-nothing
    // so a partial failure cannot leave mixed-size arrays behind.
    auto allocU32 = [](uint32_t*& pointer, uint32_t count) {
        return static_cast<uint32_t*>(_aligned_malloc(
            static_cast<size_t>(count) * sizeof(uint32_t), kMixBufferAlign));
    };
    uint32_t* freshStart = allocU32(wvStartFrame_, handleCapacity);
    uint32_t* freshRelease = allocU32(wvReleaseFrame_, handleCapacity);
    uint32_t* freshStartIngress = allocU32(wvStartIngress_, handleCapacity);
    uint32_t* freshOpIndex = allocU32(wvOpIndex_, handleCapacity);
    uint32_t* freshSlice = allocU32(wvSliceHandles_, cap);
    uint32_t* freshOpHandles = allocU32(wvOpHandles_, cap);
    uint32_t* freshOpFrames = allocU32(wvOpFrames_, cap);
    uint32_t* freshGhostStart = allocU32(wvGhostStartFrame_, cap);
    uint32_t* freshGhostDeath = allocU32(wvGhostDeathFrame_, cap);
    uint32_t* freshGhostRelease = allocU32(wvGhostReleaseFrame_, cap);
    uint32_t* freshGhostIngress = allocU32(wvGhostStartIngress_, cap);
    uint32_t* freshGhostHandle = allocU32(wvGhostHandle_, cap);
    uint8_t* freshGhostNoTail = static_cast<uint8_t*>(_aligned_malloc(
        static_cast<size_t>(cap) * sizeof(uint8_t), kMixBufferAlign));
    if (freshStart == nullptr || freshRelease == nullptr ||
        freshStartIngress == nullptr || freshOpIndex == nullptr ||
        freshSlice == nullptr || freshOpHandles == nullptr ||
        freshOpFrames == nullptr || freshGhostStart == nullptr ||
        freshGhostDeath == nullptr || freshGhostRelease == nullptr ||
        freshGhostIngress == nullptr || freshGhostHandle == nullptr ||
        freshGhostNoTail == nullptr) {
        _aligned_free(freshStart);
        _aligned_free(freshRelease);
        _aligned_free(freshStartIngress);
        _aligned_free(freshOpIndex);
        _aligned_free(freshSlice);
        _aligned_free(freshOpHandles);
        _aligned_free(freshOpFrames);
        _aligned_free(freshGhostStart);
        _aligned_free(freshGhostDeath);
        _aligned_free(freshGhostRelease);
        _aligned_free(freshGhostIngress);
        _aligned_free(freshGhostHandle);
        _aligned_free(freshGhostNoTail);
        return false;
    }
    _aligned_free(wvStartFrame_);
    _aligned_free(wvReleaseFrame_);
    _aligned_free(wvStartIngress_);
    _aligned_free(wvSliceHandles_);
    _aligned_free(wvOpHandles_);
    _aligned_free(wvOpFrames_);
    _aligned_free(wvGhostStartFrame_);
    _aligned_free(wvGhostDeathFrame_);
    _aligned_free(wvGhostReleaseFrame_);
    _aligned_free(wvGhostStartIngress_);
    _aligned_free(wvGhostHandle_);
    _aligned_free(wvGhostNoTail_);
    wvStartFrame_ = freshStart;
    wvReleaseFrame_ = freshRelease;
    wvStartIngress_ = freshStartIngress;
    _aligned_free(wvOpIndex_);
    wvOpIndex_ = freshOpIndex;
    wvSliceHandles_ = freshSlice;
    wvOpHandles_ = freshOpHandles;
    wvOpFrames_ = freshOpFrames;
    wvGhostStartFrame_ = freshGhostStart;
    wvGhostDeathFrame_ = freshGhostDeath;
    wvGhostReleaseFrame_ = freshGhostRelease;
    wvGhostStartIngress_ = freshGhostIngress;
    wvGhostHandle_ = freshGhostHandle;
    wvGhostNoTail_ = freshGhostNoTail;
    wvHandleCapacity_ = handleCapacity;
    wvMuted_.assign(handleCapacity, uint8_t(0));
    wvMutedSince_.assign(handleCapacity, 0u);
    wvPartMixed_.assign(handleCapacity, 0u);
    wvPartMuted_.assign(handleCapacity, 0u);
    wvMutedLast_.assign(handleCapacity, 0u);
    if (wvGhostCapacity_ < cap) {
        VoiceSoA grown;
        if (!grown.Reserve(cap)) return false;
        wvGhostState_ = grown;   // copy-assign steals the storage
        wvGhostCapacity_ = cap;
        delete[] wvGhostTails_;
        wvGhostTails_ = new (std::nothrow) WholeVoiceGhostTail[cap];
        if (wvGhostTails_ == nullptr) return false;
    }
    wvOpCapacity_ = cap;
    return true;
}

inline bool RenderScalar::SyncWholeVoiceRotationScratch(bool rotationActive) {
    if (!rotationActive) {
        if (wvGhostState_.rot != nullptr) wvGhostState_.ReleaseRotation();
        for (uint32_t job = 0u; job < wvJobCapacity_; ++job) {
            if (wvTailScratch_[job].rot != nullptr)
                wvTailScratch_[job].ReleaseRotation();
        }
        return true;
    }
    if (wvGhostState_.rot == nullptr &&
        !wvGhostState_.ReserveRotation(wvGhostCapacity_)) {
        return false;
    }
    for (uint32_t job = 0u; job < wvJobCapacity_; ++job) {
        if (wvTailScratch_[job].rot == nullptr &&
            !wvTailScratch_[job].ReserveRotation(1u)) {
            return false;
        }
    }
    return true;
}

inline bool RenderScalar::EnsureWholeVoiceJobScratch(uint32_t jobCount) {
    if (jobCount == 0u) jobCount = 1u;
    if (wvJobCapacity_ >= jobCount && wvJobScratchCapacity_ == scratchCapacity_ &&
        wvJobRetirements_ != nullptr && wvTailScratch_ != nullptr) {
        return true;
    }
    SpanRetirement* retirements = static_cast<SpanRetirement*>(_aligned_malloc(
        static_cast<size_t>(jobCount) * scratchCapacity_ *
            sizeof(SpanRetirement),
        kMixBufferAlign));
    uint32_t* counts = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(jobCount) * kWvRetireCountStride * sizeof(uint32_t),
        kMixBufferAlign));
    VoiceSoA* tailScratch = new (std::nothrow) VoiceSoA[jobCount];
    WvJobSegStats* segStats = static_cast<WvJobSegStats*>(_aligned_malloc(
        static_cast<size_t>(jobCount) * sizeof(WvJobSegStats), 64u));
    if (retirements == nullptr || counts == nullptr ||
        tailScratch == nullptr || segStats == nullptr) {
        _aligned_free(retirements);
        _aligned_free(counts);
        _aligned_free(segStats);
        delete[] tailScratch;
        return false;
    }
    std::memset(segStats, 0, static_cast<size_t>(jobCount) *
                                 sizeof(WvJobSegStats));
    for (uint32_t job = 0u; job < jobCount; ++job) {
        if (!tailScratch[job].Reserve(1u)) {
            _aligned_free(retirements);
            _aligned_free(counts);
            _aligned_free(segStats);
            delete[] tailScratch;
            return false;
        }
    }
    // Counts MUST start zeroed: the thunks zero their own slot only when a
    // block actually dispatches items, and the retire stage reads the counts
    // of every empty block (active=0) too.
    std::memset(counts, 0, static_cast<size_t>(jobCount) *
                               kWvRetireCountStride * sizeof(uint32_t));
    for (uint32_t counter = 0u; counter < kWvSegCounterCount; ++counter)
        wvSegRetired_[counter] = WvSegSum(static_cast<WvSegCounter>(counter));
    _aligned_free(wvJobRetirements_);
    _aligned_free(wvJobRetireCounts_);
    _aligned_free(wvJobSeg_);
    delete[] wvTailScratch_;
    wvJobSeg_ = segStats;
    wvJobRetirements_ = retirements;
    wvJobRetireCounts_ = counts;
    wvTailScratch_ = tailScratch;
    wvJobCapacity_ = jobCount;
    wvJobScratchCapacity_ = scratchCapacity_;
    return true;
}
