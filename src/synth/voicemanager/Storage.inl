// Part of VoiceManager.h; included from there inside namespace svms. Not standalone.

inline VoiceManager::VoiceManager()
    : activeCount_(0), activeList_(nullptr), activePosition_(nullptr),
      freeTop_(0), maxVoices_(0), voiceLimit_(0), sampleRate_(44100),
      stealFadeFrames_(kStealFadeFrames),
      currentFrame_(0), lastVoiceLimitEnforceFrame_(UINT64_MAX),
      freeStack_(nullptr),
      channelIndexBlocks_(nullptr), channelIndexFreeStack_(nullptr),
      channelIndexBlockCount_(0u), channelIndexFreeTop_(0u),
      retireCount_(0), retireImmediateCount_(0), stealCount_(0),
      stealTailCount_(0), stealTailMinHeapCount_(0),
      stealTailMinHeapFrame_(UINT64_MAX), stealTailMinHeapValid_(false),
      stealStableKey_(nullptr), stealWinnerTree_(nullptr),
      stealTreeLeafBase_(1u), stealHeapCount_(0), stealHeapValid_(false),
      stealHeapBuildCount_(0),
      stealIndexRepairCount_(0),
      stealVolatileList_(nullptr),
      stealVolatileCount_(0), stealVolatileHeapKey_(nullptr),
      stealVolatileHeapHandle_(nullptr),
      stealVolatileHeapPosition_(nullptr), stealVolatileHeapCount_(0),
      stealVolatileHeapFrame_(UINT64_MAX), stealVolatileHeapValid_(false),
      stealKeyBackend_(RenderBackend::Scalar) {
    stealCandidateDeferred_ = nullptr;
    stealCandidateReserved_ = nullptr;
    metadataStorage_ = nullptr;
    metadataBytes_ = 0u;
    v.Reset();
    std::memset(channelActiveCount_, 0, sizeof(channelActiveCount_));
    std::memset(channelActiveHead_, 0xff, sizeof(channelActiveHead_));
    std::memset(channelActiveTail_, 0xff, sizeof(channelActiveTail_));
    std::memset(renderClassCount_, 0, sizeof(renderClassCount_));
    std::memset(stealTailPosition_, 0xff, sizeof(stealTailPosition_));
    std::memset(exclusiveClassHead_, 0xff, sizeof(exclusiveClassHead_));
    lastLinkedPlayIndex_ = UINT32_MAX;
    lastLinkedPlayVoice_ = kInvalidVoice;
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n) {
            channelKeyVoiceHead_[ch][n] = channelKeyVoiceOldest_[ch][n] =
                lastUnlinkedByKey_[ch][n] = -1;
            channelKeyVoiceCount_[ch][n] = 0u;
        }
    std::memset(channelMixScaleL_, 0, sizeof(channelMixScaleL_));
    std::memset(channelMixScaleR_, 0, sizeof(channelMixScaleR_));
    channelMixStealMask_ = 0u;
    channelMixDirtyMask_ = 0u;
}

inline VoiceManager::VoiceManager(const VoiceManager& other)
    : VoiceManager() {
    if (!Initialize(other.maxVoices_, other.sampleRate_))
        throw std::bad_alloc();
    CopyFrom(other);
}

inline VoiceManager::~VoiceManager() {
    _aligned_free(metadataStorage_);
}

inline size_t VoiceManager::EstimateAllocatedBytes(
    uint32_t capacity) noexcept {
    if (capacity == 0u || capacity > kMaxPolyphony) return 0u;
    uint32_t treeLeaves = 1u;
    while (treeLeaves < capacity) treeLeaves <<= 1u;
    const uint32_t channelBlocks =
        (capacity + kChannelIndexBlockSize - 1u) /
            kChannelIndexBlockSize + kChannelCount;
    size_t metadataBytes = 0u;
    auto add = [&](size_t elementSize, size_t count) {
        metadataBytes = (metadataBytes + kMixBufferAlign - 1u) &
                ~(static_cast<size_t>(kMixBufferAlign) - 1u);
        if (count > ((std::numeric_limits<size_t>::max)() - metadataBytes) /
                        elementSize) {
            metadataBytes = (std::numeric_limits<size_t>::max)();
            return;
        }
        metadataBytes += elementSize * count;
    };
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(int32_t), capacity);
    {
        uint32_t ringCapacity = 2u;
        while (ringCapacity < static_cast<uint32_t>(capacity) * 2u)
            ringCapacity <<= 1u;
        add(sizeof(uint32_t), ringCapacity); // releasing ring
    }
    add(sizeof(ChannelIndexBlock), channelBlocks);
    add(sizeof(uint32_t), channelBlocks);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint8_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint16_t), capacity);
    add(sizeof(uint64_t), capacity);
    add(sizeof(uint64_t), static_cast<size_t>(treeLeaves) * 2u);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint64_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint8_t), capacity);
    add(sizeof(uint8_t), capacity);
    {
        const size_t bucketNodes = static_cast<size_t>(capacity) * 2u + 4096u;
        for (int set = 0; set < 2; ++set) {
            add(sizeof(uint32_t), 65536u);
            add(sizeof(uint32_t), 65536u);
            add(sizeof(uint32_t), bucketNodes);
            add(sizeof(uint32_t), bucketNodes);
            add(sizeof(uint32_t), bucketNodes);
            add(sizeof(uint32_t), capacity);
            add(sizeof(uint64_t), 1024u);
            add(sizeof(uint64_t), 16u);
        }
        add(sizeof(uint64_t), capacity);
    }
    add(sizeof(int32_t), capacity);
    add(sizeof(int32_t), capacity);
    add(sizeof(uint8_t), capacity);
    add(sizeof(int32_t), capacity);
    add(sizeof(int32_t), capacity);
    if (metadataBytes == (std::numeric_limits<size_t>::max)())
        return metadataBytes;
    const size_t voiceBytes = VoiceSoA::EstimateStorageBytes(
        capacity, LaunchStagingRequested());
    if (voiceBytes > (std::numeric_limits<size_t>::max)() -
                         sizeof(VoiceManager) - metadataBytes)
        return (std::numeric_limits<size_t>::max)();
    return sizeof(VoiceManager) + voiceBytes + metadataBytes;
}

inline bool VoiceManager::ReserveMetadata(uint32_t capacity) {
    if (capacity == 0u || capacity > kMaxPolyphony) return false;
    uint32_t treeLeaves = 1u;
    while (treeLeaves < capacity) treeLeaves <<= 1u;
    const uint32_t channelBlocks =
        (capacity + kChannelIndexBlockSize - 1u) /
            kChannelIndexBlockSize + kChannelCount;
    uint32_t ringCapacity = 2u;
    while (ringCapacity < static_cast<uint32_t>(capacity) * 2u)
        ringCapacity <<= 1u;

    size_t bytes = 0u;
    auto add = [&](size_t elementSize, size_t count) {
        bytes = (bytes + kMixBufferAlign - 1u) &
                ~(static_cast<size_t>(kMixBufferAlign) - 1u);
        if (count > ((std::numeric_limits<size_t>::max)() - bytes) /
                        elementSize) {
            bytes = (std::numeric_limits<size_t>::max)();
            return;
        }
        bytes += elementSize * count;
    };
    add(sizeof(uint32_t), capacity); // activeList
    add(sizeof(uint32_t), capacity); // activePosition
    add(sizeof(int32_t), capacity);  // freeStack
    add(sizeof(uint32_t), ringCapacity); // releasing ring
    add(sizeof(ChannelIndexBlock), channelBlocks);
    add(sizeof(uint32_t), channelBlocks);
    add(sizeof(uint64_t), capacity); // stealStableKey
    add(sizeof(uint64_t), static_cast<size_t>(treeLeaves) * 2u);
    add(sizeof(uint32_t), capacity); // volatile list
    add(sizeof(uint64_t), capacity); // volatile heap ordered key
    add(sizeof(uint32_t), capacity); // volatile heap handle
    add(sizeof(uint32_t), capacity); // volatile heap position
    add(sizeof(uint8_t), capacity);  // deferred
    add(sizeof(uint8_t), capacity);  // reserved
    const uint32_t bucketNodes = capacity * 2u + 4096u;
    for (int set = 0; set < 2; ++set) {  // sustained set, volatile set
        add(sizeof(uint32_t), 65536u);       // bucket heads
        add(sizeof(uint32_t), 65536u);       // bucket tails
        add(sizeof(uint32_t), bucketNodes);  // bucket node handles
        add(sizeof(uint32_t), bucketNodes);  // bucket node links
        add(sizeof(uint32_t), bucketNodes);  // bucket node stamps
        add(sizeof(uint32_t), capacity);     // live stamp per handle
        add(sizeof(uint64_t), 1024u);        // non-empty bucket bits
        add(sizeof(uint64_t), 16u);          // non-empty word bits
    }
    add(sizeof(uint64_t), capacity);     // volatile keys by handle
    add(sizeof(VoiceLinkRow), capacity);
    if (bytes == (std::numeric_limits<size_t>::max)()) return false;

    void* allocation = _aligned_malloc(bytes, kMixBufferAlign);
    if (!allocation) return false;
    _aligned_free(metadataStorage_);
    metadataStorage_ = allocation;
    metadataBytes_ = bytes;
    channelIndexBlockCount_ = channelBlocks;
    stealTreeLeafBase_ = treeLeaves;
    releasingRingCapacity_ = ringCapacity;
    releasingRingMask_ = ringCapacity - 1u;
    releasingRingHead_ = 0u;
    releasingRingCount_ = 0u;

    size_t offset = 0u;
    uint8_t* base = static_cast<uint8_t*>(metadataStorage_);
    auto take = [&](size_t elementSize, size_t count) -> void* {
        offset = (offset + kMixBufferAlign - 1u) &
                 ~(static_cast<size_t>(kMixBufferAlign) - 1u);
        void* result = base + offset;
        offset += elementSize * count;
        return result;
    };
    activeList_ = static_cast<uint32_t*>(take(sizeof(uint32_t), capacity));
    activePosition_ = static_cast<uint32_t*>(take(sizeof(uint32_t), capacity));
    freeStack_ = static_cast<int32_t*>(take(sizeof(int32_t), capacity));
    releasingRing_ = static_cast<uint32_t*>(take(sizeof(uint32_t),
                                                 releasingRingCapacity_));
    channelIndexBlocks_ = static_cast<ChannelIndexBlock*>(
        take(sizeof(ChannelIndexBlock), channelBlocks));
    channelIndexFreeStack_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), channelBlocks));
    stealStableKey_ = static_cast<uint64_t*>(
        take(sizeof(uint64_t), capacity));
    stealWinnerTree_ = static_cast<uint64_t*>(take(
        sizeof(uint64_t), static_cast<size_t>(treeLeaves) * 2u));
    stealVolatileList_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    stealVolatileHeapKey_ = static_cast<uint64_t*>(
        take(sizeof(uint64_t), capacity));
    stealVolatileHeapHandle_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    stealVolatileHeapPosition_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    stealCandidateDeferred_ = static_cast<uint8_t*>(
        take(sizeof(uint8_t), capacity));
    stealCandidateReserved_ = static_cast<uint8_t*>(
        take(sizeof(uint8_t), capacity));
    for (StealBucketSet* set : {&stableBuckets_, &volBuckets_}) {
        set->head = static_cast<uint32_t*>(take(sizeof(uint32_t), 65536u));
        set->tail = static_cast<uint32_t*>(take(sizeof(uint32_t), 65536u));
        set->nodeHandle = static_cast<uint32_t*>(
            take(sizeof(uint32_t), bucketNodes));
        set->nodeNext = static_cast<uint32_t*>(
            take(sizeof(uint32_t), bucketNodes));
        set->nodeBucket = static_cast<uint32_t*>(
            take(sizeof(uint32_t), bucketNodes));
        set->handleNode = static_cast<uint32_t*>(
            take(sizeof(uint32_t), capacity));
        set->bits = static_cast<uint64_t*>(take(sizeof(uint64_t), 1024u));
        set->summary = static_cast<uint64_t*>(take(sizeof(uint64_t), 16u));
        set->cap = bucketNodes;
        set->handles = capacity;
        set->used = 0u;
        set->freeHead = 0u;
    }
    volKey_ = static_cast<uint64_t*>(take(sizeof(uint64_t), capacity));
    BindLinkRows(static_cast<VoiceLinkRow*>(
        take(sizeof(VoiceLinkRow), capacity)));
    return offset <= metadataBytes_;
}

inline void VoiceManager::CopyFrom(const VoiceManager& other) {
    ++stealPeekEpoch_;
    v = other.v;
    std::memcpy(metadataStorage_, other.metadataStorage_, metadataBytes_);
    activeCount_ = other.activeCount_;
    freeTop_ = other.freeTop_;
    retireCount_ = other.retireCount_;
    retireImmediateCount_ = other.retireImmediateCount_;
    stealCount_ = other.stealCount_;
    voiceLimit_ = other.voiceLimit_;
    stealFadeFrames_ = other.stealFadeFrames_;
    currentFrame_ = other.currentFrame_;
    lastVoiceLimitEnforceFrame_ = other.lastVoiceLimitEnforceFrame_;
    std::memcpy(channelActiveCount_, other.channelActiveCount_,
                sizeof(channelActiveCount_));
    std::memcpy(channelActiveHead_, other.channelActiveHead_,
                sizeof(channelActiveHead_));
    std::memcpy(channelActiveTail_, other.channelActiveTail_,
                sizeof(channelActiveTail_));
    channelIndexFreeTop_ = other.channelIndexFreeTop_;
    std::memcpy(renderClassCount_, other.renderClassCount_,
                sizeof(renderClassCount_));
    std::memcpy(stealTailList_, other.stealTailList_, sizeof(stealTailList_));
    std::memcpy(stealTailPosition_, other.stealTailPosition_,
                sizeof(stealTailPosition_));
    stealTailCount_ = other.stealTailCount_;
    std::memcpy(stealTailMinHeapKey_, other.stealTailMinHeapKey_,
                sizeof(stealTailMinHeapKey_));
    stealTailMinHeapCount_ = other.stealTailMinHeapCount_;
    stealTailMinHeapFrame_ = other.stealTailMinHeapFrame_;
    stealTailMinHeapValid_ = other.stealTailMinHeapValid_;
    stealHeapCount_ = other.stealHeapCount_;
    stealHeapValid_ = other.stealHeapValid_;
    stealHeapBuildCount_ = other.stealHeapBuildCount_;
    stealIndexRepairCount_ = other.stealIndexRepairCount_;
    bucketSteal_ = other.bucketSteal_;
    sampledSteal_ = other.sampledSteal_;
    sampleCursor_ = other.sampleCursor_;
    sampleFailFrame_ = other.sampleFailFrame_;
    sampleFailLevel_ = other.sampleFailLevel_;
    stableBuckets_.used = other.stableBuckets_.used;
    stableBuckets_.freeHead = other.stableBuckets_.freeHead;
    volBuckets_.used = other.volBuckets_.used;
    volBuckets_.freeHead = other.volBuckets_.freeHead;
    stealVolatileCount_ = other.stealVolatileCount_;
    stealVolatileHeapCount_ = other.stealVolatileHeapCount_;
    stealVolatileHeapFrame_ = other.stealVolatileHeapFrame_;
    stealVolatileHeapValid_ = other.stealVolatileHeapValid_;
    stealKeyBackend_ = other.stealKeyBackend_;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    groupReuseAttemptCount_ = other.groupReuseAttemptCount_;
    groupReuseMatchCount_ = other.groupReuseMatchCount_;
    groupReuseReservedCount_ = other.groupReuseReservedCount_;
    groupReuseSmallerCount_ = other.groupReuseSmallerCount_;
    groupReuseLargerCount_ = other.groupReuseLargerCount_;
    launchProfileCounter_ = other.launchProfileCounter_;
    launchProfileSamples_ = other.launchProfileSamples_;
    launchProfilePopCycles_ = other.launchProfilePopCycles_;
    launchProfileTailCycles_ = other.launchProfileTailCycles_;
    launchProfileLifecycleCycles_ = other.launchProfileLifecycleCycles_;
    launchProfileConfigureCycles_ = other.launchProfileConfigureCycles_;
    launchProfileTreeCycles_ = other.launchProfileTreeCycles_;
    launchChurnProfilingEnabled_ = other.launchChurnProfilingEnabled_;
    volatileFallbackScanForTest_ = other.volatileFallbackScanForTest_;
    launchChurnStats_ = other.launchChurnStats_;
    launchTestContext_ = LaunchTestContext{};
    launchTestTrackedFrame_ = other.launchTestTrackedFrame_;
    launchTestProvisionalGroups_ = other.launchTestProvisionalGroups_;
    launchTestProvisionalPhysicalVoices_ =
        other.launchTestProvisionalPhysicalVoices_;
#endif
    std::memcpy(channelKeyVoiceHead_, other.channelKeyVoiceHead_,
                sizeof(channelKeyVoiceHead_));
    std::memcpy(channelKeyVoiceOldest_, other.channelKeyVoiceOldest_,
                sizeof(channelKeyVoiceOldest_));
    std::memcpy(lastUnlinkedByKey_, other.lastUnlinkedByKey_,
                sizeof(lastUnlinkedByKey_));
    std::memcpy(channelKeyVoiceCount_, other.channelKeyVoiceCount_,
                sizeof(channelKeyVoiceCount_));
    std::memcpy(exclusiveClassHead_, other.exclusiveClassHead_,
                sizeof(exclusiveClassHead_));
    std::memcpy(channelMixScaleL_, other.channelMixScaleL_,
                sizeof(channelMixScaleL_));
    std::memcpy(channelMixScaleR_, other.channelMixScaleR_,
                sizeof(channelMixScaleR_));
    channelMixStealMask_ = other.channelMixStealMask_;
    channelMixDirtyMask_ = other.channelMixDirtyMask_;
    lastLinkedPlayIndex_ = other.lastLinkedPlayIndex_;
    lastLinkedPlayVoice_ = other.lastLinkedPlayVoice_;
    mergedVoicesMayExist_ = other.mergedVoicesMayExist_;
    nextForkPlayIndex_ = other.nextForkPlayIndex_;
    stagedPending_ = other.stagedPending_;
    launchStagingMode_ = other.launchStagingMode_;
}

inline bool VoiceManager::Initialize(uint32_t maxVoices, uint32_t sampleRate) {
    const uint32_t requested = maxVoices < kMaxPolyphony
        ? maxVoices : kMaxPolyphony;
    if (launchStagingMode_ < 0)
        launchStagingMode_ = LaunchStagingRequested() ? 1 : 0;
    v.launchStaging = launchStagingMode_ != 0;
    bucketSteal_ = StealIndexBucketsRequested();
    sampledSteal_ = SampledStealRequested();
    if (requested == 0u || !v.Reserve(requested) ||
        !ReserveMetadata(requested)) {
        maxVoices_ = 0u;
        voiceLimit_ = 0u;
        return false;
    }
    // The releasing ring is a stand-in for what the bucketed index does for
    // every volatile voice; a zero capacity keeps it out of the way.
    if (bucketSteal_) {
        releasingRingCapacity_ = 0u;
        releasingRingMask_ = 0u;
    }
    maxVoices_ = requested;
    voiceLimit_ = requested;
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100;
    stealFadeFrames_ = kStealFadeFrames;
    Reset();
    return true;
}

inline void VoiceManager::Reset() {
    ++stealPeekEpoch_;
    v.Reset();
    std::memset(activeList_, 0, sizeof(*activeList_) * maxVoices_);
    std::memset(activePosition_, 0xff,
                sizeof(*activePosition_) * maxVoices_);
    std::memset(channelActiveCount_, 0, sizeof(channelActiveCount_));
    std::memset(channelActiveHead_, 0xff, sizeof(channelActiveHead_));
    std::memset(channelActiveTail_, 0xff, sizeof(channelActiveTail_));
    for (uint32_t i = 0u; i < maxVoices_; ++i) {
        channelActiveBlock_[i] = UINT32_MAX;
        channelActiveOffset_[i] = UINT8_MAX;
    }
    channelIndexFreeTop_ = channelIndexBlockCount_;
    for (uint32_t block = 0u; block < channelIndexBlockCount_; ++block) {
        channelIndexFreeStack_[block] = channelIndexBlockCount_ - 1u - block;
        channelIndexBlocks_[block].count = 0u;
        channelIndexBlocks_[block].previous = UINT32_MAX;
        channelIndexBlocks_[block].next = UINT32_MAX;
    }
    std::memset(renderClassCount_, 0, sizeof(renderClassCount_));
    for (uint32_t i = 0u; i < maxVoices_; ++i)
        renderClassLinked_[i] = 0u;
    std::memset(stealTailPosition_, 0xff, sizeof(stealTailPosition_));
    std::memset(stealWinnerTree_, 0,
                sizeof(*stealWinnerTree_) * stealTreeLeafBase_ * 2u);
    std::memset(stealStableKey_, 0,
                sizeof(*stealStableKey_) * maxVoices_);
    for (uint32_t i = 0u; i < maxVoices_; ++i)
        stealVolatilePosition_[i] = UINT32_MAX;
    std::memset(stealVolatileHeapPosition_, 0xff,
                sizeof(*stealVolatileHeapPosition_) * maxVoices_);
    std::memset(stealCandidateDeferred_, 0,
                sizeof(*stealCandidateDeferred_) * maxVoices_);
    std::memset(stealCandidateReserved_, 0,
                sizeof(*stealCandidateReserved_) * maxVoices_);
    stableBuckets_.Reset();
    volBuckets_.Reset();
    std::memset(volKey_, 0, sizeof(*volKey_) * maxVoices_);
    for (uint32_t i = 0u; i < maxVoices_; ++i) {
        playGroupNext_[i] = -1;
        playGroupPrev_[i] = -1;
        exclusiveClass_[i] = 0u;
        exclusiveClassNext_[i] = -1;
        exclusiveClassPrev_[i] = -1;
    }
    std::memset(exclusiveClassHead_, 0xff, sizeof(exclusiveClassHead_));
    activeCount_ = 0;
    currentFrame_ = 0;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    launchChurnStats_ = LaunchChurnStats{};
    launchTestContext_ = LaunchTestContext{};
    launchTestTrackedFrame_ = 0u;
    launchTestProvisionalGroups_ = 0;
    launchTestProvisionalPhysicalVoices_ = 0;
#endif
    lastVoiceLimitEnforceFrame_ = UINT64_MAX;
    retireCount_ = 0;
    retireImmediateCount_ = 0;
    stealCount_ = 0;
    stealTailCount_ = 0;
    stealTailMinHeapCount_ = 0;
    stealTailMinHeapFrame_ = UINT64_MAX;
    stealTailMinHeapValid_ = false;
    stealHeapCount_ = 0;
    stealHeapValid_ = false;
    stealHeapBuildCount_ = 0;
    stealIndexRepairCount_ = 0;
    stealVolatileCount_ = 0;
    stealVolatileHeapCount_ = 0;
    stealVolatileHeapFrame_ = UINT64_MAX;
    stealVolatileHeapValid_ = false;
    lastLinkedPlayIndex_ = UINT32_MAX;
    lastLinkedPlayVoice_ = kInvalidVoice;
    mergedVoicesMayExist_ = false;
    launchMult_ = 1u;
    freeTop_ = maxVoices_;
    releasingCount_.store(0u, std::memory_order_relaxed);
    releasingRingHead_ = 0u;
    releasingRingCount_ = 0u;
    releasingRingHits_ = 0u;
    for (uint32_t i = 0; i < maxVoices_; ++i) {
        v.state[i] = static_cast<uint8_t>(VoiceState::Free);
        v.nextChannelKeyVoice[i] = -1;
        v.prevChannelKeyVoice[i] = -1;
        freeStack_[i] = static_cast<int32_t>(i);
    }
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n) {
            channelKeyVoiceHead_[ch][n] = channelKeyVoiceOldest_[ch][n] =
                lastUnlinkedByKey_[ch][n] = -1;
            channelKeyVoiceCount_[ch][n] = 0u;
        }
}

inline bool VoiceManager::GrowCapacity(uint32_t capacity) {
    ++stealPeekEpoch_;
    if (capacity <= maxVoices_) return capacity != 0u;
    if (capacity == 0u || capacity > kMaxPolyphony) return false;

    const uint32_t oldCapacity = maxVoices_;
    // Staged rows keep their records across the grow (copied below), but the
    // grown manager must carry the same storage layout.
    VoiceManager grown;
    grown.launchStagingMode_ = launchStagingMode_;
    if (!grown.Initialize(capacity, sampleRate_)) return false;

    // Preserve every render-visible per-voice field. Handles are stable: old
    // voices keep the same array index, while the newly added range remains
    // Free from grown.Initialize().
#define SVMS_COPY_GROWN_VOICE_FIELD(name) \
    std::memcpy(grown.v.name, v.name, \
                static_cast<size_t>(oldCapacity) * sizeof(*v.name))
    SVMS_COPY_GROWN_VOICE_FIELD(state);
    SVMS_COPY_GROWN_VOICE_FIELD(envelopeStage);
    SVMS_COPY_GROWN_VOICE_FIELD(sampleBacked);
    SVMS_COPY_GROWN_VOICE_FIELD(renderClass);
    SVMS_COPY_GROWN_VOICE_FIELD(phaseIdx);
    SVMS_COPY_GROWN_VOICE_FIELD(phaseFrac);
    SVMS_COPY_GROWN_VOICE_FIELD(phaseIncs);
    SVMS_COPY_GROWN_VOICE_FIELD(currentGain);
    SVMS_COPY_GROWN_VOICE_FIELD(targetGain);
    SVMS_COPY_GROWN_VOICE_FIELD(sustainLevel);
    SVMS_COPY_GROWN_VOICE_FIELD(attackGainStep);
    SVMS_COPY_GROWN_VOICE_FIELD(releaseDecay);
    SVMS_COPY_GROWN_VOICE_FIELD(mixGainL);
    SVMS_COPY_GROWN_VOICE_FIELD(mixGainR);
    SVMS_COPY_GROWN_VOICE_FIELD(renderGainL);
    SVMS_COPY_GROWN_VOICE_FIELD(renderGainR);
    SVMS_COPY_GROWN_VOICE_FIELD(filterA0);
    SVMS_COPY_GROWN_VOICE_FIELD(filterB1);
    SVMS_COPY_GROWN_VOICE_FIELD(filterB2);
    SVMS_COPY_GROWN_VOICE_FIELD(filterZ1);
    SVMS_COPY_GROWN_VOICE_FIELD(filterZ2);
    SVMS_COPY_GROWN_VOICE_FIELD(filterEnabled);
    SVMS_COPY_GROWN_VOICE_FIELD(stealOutputGain);
    SVMS_COPY_GROWN_VOICE_FIELD(sampleStart);
    SVMS_COPY_GROWN_VOICE_FIELD(loopEnabled);
    SVMS_COPY_GROWN_VOICE_FIELD(relEnd);
    SVMS_COPY_GROWN_VOICE_FIELD(relLoopS);
    SVMS_COPY_GROWN_VOICE_FIELD(relLoopE);
    SVMS_COPY_GROWN_VOICE_FIELD(relLoopSF);
    SVMS_COPY_GROWN_VOICE_FIELD(relLoopEF);
    SVMS_COPY_GROWN_VOICE_FIELD(holdSamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(attackSamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(decaySamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(delaySamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(releaseSamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(decaySlope);
    SVMS_COPY_GROWN_VOICE_FIELD(birthFrame);
    SVMS_COPY_GROWN_VOICE_FIELD(stealFadeInFramesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(stealFadeInFramesTotal);
#undef SVMS_COPY_GROWN_VOICE_FIELD
    std::memcpy(grown.v.rows, v.rows,
                static_cast<size_t>(oldCapacity) * sizeof(VoiceRow));
    if (grown.v.launch != nullptr && v.launch != nullptr)
        std::memcpy(grown.v.launch, v.launch,
                    static_cast<size_t>(oldCapacity) *
                        sizeof(VoiceLaunchRecord));

#define SVMS_COPY_GROWN_TAIL_FIELD(name) \
    std::memcpy(grown.v.name, v.name, sizeof(v.name))
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailPhaseIdx);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailPhaseFrac);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailPhaseInc);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailGain);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailMixGainL);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailMixGainR);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailSampleStart);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelEnd);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelLoopS);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelLoopE);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelLoopSF);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelLoopEF);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailFramesRemaining);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailFramesTotal);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailSampleBacked);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailLoopEnabled);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailChannel);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRot);
#undef SVMS_COPY_GROWN_TAIL_FIELD
    grown.v.pad16 = v.pad16;

    // Preserve per-voice rotation state across the growth (handles are
    // stable, so a straight prefix copy is exact).
    grown.phaseRotationMode_ = phaseRotationMode_;
    grown.rotationSeedCounter_ = rotationSeedCounter_;
    if (phaseRotationMode_ != 0u && v.rot &&
        grown.v.ReserveRotation(oldCapacity)) {
        std::memcpy(grown.v.rot, v.rot,
                    static_cast<size_t>(oldCapacity) *
                        sizeof(VoiceRotationState));
    }

    grown.activeCount_ = activeCount_;
    if (activeCount_ != 0u)
        std::memcpy(grown.activeList_, activeList_,
                    static_cast<size_t>(activeCount_) * sizeof(*activeList_));
    std::memset(grown.activePosition_, 0xff,
                static_cast<size_t>(capacity) * sizeof(*grown.activePosition_));
    for (uint32_t position = 0u; position < grown.activeCount_; ++position)
        grown.activePosition_[grown.activeList_[position]] = position;

    // Rebuild the free stack from copied voice state. The order of free slots
    // is not semantic; rebuilding also naturally includes the newly grown
    // handle range without touching any active handle.
    grown.freeTop_ = 0u;
    for (uint32_t handle = 0u; handle < capacity; ++handle) {
        if (grown.v.state[handle] == static_cast<uint8_t>(VoiceState::Free))
            grown.freeStack_[grown.freeTop_++] = static_cast<int32_t>(handle);
    }

    std::memcpy(grown.channelKeyVoiceHead_, channelKeyVoiceHead_,
                sizeof(channelKeyVoiceHead_));
    std::memcpy(grown.channelKeyVoiceOldest_, channelKeyVoiceOldest_,
                sizeof(channelKeyVoiceOldest_));
    std::memcpy(grown.lastUnlinkedByKey_, lastUnlinkedByKey_,
                sizeof(lastUnlinkedByKey_));
    std::memcpy(grown.channelMixScaleL_, channelMixScaleL_,
                sizeof(channelMixScaleL_));
    std::memcpy(grown.channelMixScaleR_, channelMixScaleR_,
                sizeof(channelMixScaleR_));
    grown.channelMixStealMask_ = channelMixStealMask_;
    grown.channelMixDirtyMask_ = channelMixDirtyMask_;
    for (uint32_t i = 0u; i < oldCapacity; ++i) {
        grown.playGroupNext_[i] = playGroupNext_[i];
        grown.playGroupPrev_[i] = playGroupPrev_[i];
        grown.exclusiveClass_[i] = exclusiveClass_[i];
        grown.exclusiveClassNext_[i] = exclusiveClassNext_[i];
        grown.exclusiveClassPrev_[i] = exclusiveClassPrev_[i];
    }
    std::memcpy(grown.exclusiveClassHead_, exclusiveClassHead_,
                sizeof(exclusiveClassHead_));
    grown.lastLinkedPlayIndex_ = lastLinkedPlayIndex_;
    grown.lastLinkedPlayVoice_ = lastLinkedPlayVoice_;

    // The dense channel/render indices depend on allocation capacity, so
    // rebuild them from the stable active list instead of copying pages whose
    // geometry changed. Steal candidates are rebuilt lazily on the next steal.
    for (uint32_t position = 0u; position < grown.activeCount_; ++position) {
        const VoiceHandle handle = static_cast<VoiceHandle>(grown.activeList_[position]);
        grown.LinkChannelActive(handle);
        grown.LinkRenderClass(handle);
    }
    grown.stealHeapCount_ = 0u;
    grown.stealHeapValid_ = false;
    grown.stealVolatileCount_ = 0u;
    grown.stealVolatileHeapCount_ = 0u;
    grown.stealVolatileHeapFrame_ = UINT64_MAX;
    grown.stealVolatileHeapValid_ = false;
    grown.stealKeyBackend_ = stealKeyBackend_;
    std::memset(grown.stealWinnerTree_, 0,
                sizeof(*grown.stealWinnerTree_) * grown.stealTreeLeafBase_ * 2u);
    std::memset(grown.stealStableKey_, 0,
                static_cast<size_t>(capacity) * sizeof(*grown.stealStableKey_));
    for (uint32_t i = 0u; i < capacity; ++i)
        grown.stealVolatilePosition_[i] = UINT32_MAX;
    std::memset(grown.stealVolatileHeapPosition_, 0xff,
                static_cast<size_t>(capacity) * sizeof(*grown.stealVolatileHeapPosition_));
    std::memset(grown.stealCandidateDeferred_, 0,
                static_cast<size_t>(capacity) * sizeof(*grown.stealCandidateDeferred_));
    std::memset(grown.stealCandidateReserved_, 0,
                static_cast<size_t>(capacity) * sizeof(*grown.stealCandidateReserved_));

    std::memcpy(grown.stealTailList_, stealTailList_, sizeof(stealTailList_));
    std::memcpy(grown.stealTailPosition_, stealTailPosition_,
                sizeof(stealTailPosition_));
    grown.stealTailCount_ = stealTailCount_;
    grown.stealTailMinHeapCount_ = 0u;
    grown.stealTailMinHeapFrame_ = UINT64_MAX;
    grown.stealTailMinHeapValid_ = false;

    grown.retireCount_ = retireCount_;
    grown.retireImmediateCount_ = retireImmediateCount_;
    grown.stealCount_ = stealCount_;
    grown.releasingCount_.store(GetReleasingCount(), std::memory_order_relaxed);
    grown.releasingRingHits_ = releasingRingHits_;
    grown.releasingRingHead_ = 0u;
    grown.releasingRingCount_ = releasingRingCount_;
    for (uint32_t i = 0u; i < releasingRingCount_; ++i)
        grown.releasingRing_[i & grown.releasingRingMask_] =
            releasingRing_[(releasingRingHead_ + i) & releasingRingMask_];
    grown.voiceLimit_ = voiceLimit_;
    grown.currentFrame_ = currentFrame_;
    grown.lastVoiceLimitEnforceFrame_ = lastVoiceLimitEnforceFrame_;
    grown.stealFadeFrames_ = stealFadeFrames_;
    grown.stealHeapBuildCount_ = stealHeapBuildCount_;
    grown.stealIndexRepairCount_ = stealIndexRepairCount_;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    grown.groupReuseAttemptCount_ = groupReuseAttemptCount_;
    grown.groupReuseMatchCount_ = groupReuseMatchCount_;
    grown.groupReuseReservedCount_ = groupReuseReservedCount_;
    grown.groupReuseSmallerCount_ = groupReuseSmallerCount_;
    grown.groupReuseLargerCount_ = groupReuseLargerCount_;
    grown.launchProfileCounter_ = launchProfileCounter_;
    grown.launchProfileSamples_ = launchProfileSamples_;
    grown.launchProfilePopCycles_ = launchProfilePopCycles_;
    grown.launchProfileTailCycles_ = launchProfileTailCycles_;
    grown.launchProfileLifecycleCycles_ = launchProfileLifecycleCycles_;
    grown.launchProfileConfigureCycles_ = launchProfileConfigureCycles_;
    grown.launchProfileTreeCycles_ = launchProfileTreeCycles_;
    grown.launchChurnProfilingEnabled_ = launchChurnProfilingEnabled_;
    grown.volatileFallbackScanForTest_ = volatileFallbackScanForTest_;
    grown.launchChurnStats_ = launchChurnStats_;
    grown.launchTestContext_ = LaunchTestContext{};
    grown.launchTestTrackedFrame_ = launchTestTrackedFrame_;
    grown.launchTestProvisionalGroups_ = launchTestProvisionalGroups_;
    grown.launchTestProvisionalPhysicalVoices_ =
        launchTestProvisionalPhysicalVoices_;
#endif

    // The staging manager now owns a complete larger representation. Move
    // only its owning allocations/pointers into this instance; the old
    // metadata and VoiceSoA storage are freed after the swap, while all fixed
    // counters and indices are copied below. No live voice is re-launched.
    void* oldMetadataStorage = metadataStorage_;
    VoiceSoA oldVoices = std::move(v);
    v = std::move(grown.v);

    activeList_ = grown.activeList_;
    activePosition_ = grown.activePosition_;
    freeStack_ = grown.freeStack_;
    releasingRing_ = grown.releasingRing_;
    channelIndexBlocks_ = grown.channelIndexBlocks_;
    channelIndexFreeStack_ = grown.channelIndexFreeStack_;
    stealStableKey_ = grown.stealStableKey_;
    stealWinnerTree_ = grown.stealWinnerTree_;
    stealVolatileList_ = grown.stealVolatileList_;
    stealVolatileHeapKey_ = grown.stealVolatileHeapKey_;
    stealVolatileHeapHandle_ = grown.stealVolatileHeapHandle_;
    stealVolatileHeapPosition_ = grown.stealVolatileHeapPosition_;
    stealCandidateDeferred_ = grown.stealCandidateDeferred_;
    stealCandidateReserved_ = grown.stealCandidateReserved_;
    stableBuckets_ = grown.stableBuckets_;
    volBuckets_ = grown.volBuckets_;
    volKey_ = grown.volKey_;
    BindLinkRows(grown.linkRows_);
    metadataStorage_ = grown.metadataStorage_;
    metadataBytes_ = grown.metadataBytes_;
    grown.metadataStorage_ = nullptr;
    grown.metadataBytes_ = 0u;

    maxVoices_ = capacity;
    voiceLimit_ = grown.voiceLimit_;
    activeCount_ = grown.activeCount_;
    freeTop_ = grown.freeTop_;
    channelIndexBlockCount_ = grown.channelIndexBlockCount_;
    channelIndexFreeTop_ = grown.channelIndexFreeTop_;
    std::memcpy(channelActiveCount_, grown.channelActiveCount_,
                sizeof(channelActiveCount_));
    std::memcpy(channelActiveHead_, grown.channelActiveHead_,
                sizeof(channelActiveHead_));
    std::memcpy(channelActiveTail_, grown.channelActiveTail_,
                sizeof(channelActiveTail_));
    std::memcpy(exclusiveClassHead_, grown.exclusiveClassHead_,
                sizeof(exclusiveClassHead_));
    std::memcpy(renderClassCount_, grown.renderClassCount_,
                sizeof(renderClassCount_));
    stealTreeLeafBase_ = grown.stealTreeLeafBase_;
    stealHeapCount_ = grown.stealHeapCount_;
    stealHeapValid_ = grown.stealHeapValid_;
    stealVolatileCount_ = grown.stealVolatileCount_;
    stealVolatileHeapCount_ = grown.stealVolatileHeapCount_;
    stealVolatileHeapFrame_ = grown.stealVolatileHeapFrame_;
    stealVolatileHeapValid_ = grown.stealVolatileHeapValid_;
    stealKeyBackend_ = grown.stealKeyBackend_;
    std::memcpy(stealTailList_, grown.stealTailList_, sizeof(stealTailList_));
    std::memcpy(stealTailPosition_, grown.stealTailPosition_,
                sizeof(stealTailPosition_));
    stealTailCount_ = grown.stealTailCount_;
    stealTailMinHeapCount_ = grown.stealTailMinHeapCount_;
    stealTailMinHeapFrame_ = grown.stealTailMinHeapFrame_;
    stealTailMinHeapValid_ = grown.stealTailMinHeapValid_;
    lastLinkedPlayIndex_ = grown.lastLinkedPlayIndex_;
    lastLinkedPlayVoice_ = grown.lastLinkedPlayVoice_;
    currentFrame_ = grown.currentFrame_;
    lastVoiceLimitEnforceFrame_ = grown.lastVoiceLimitEnforceFrame_;
    retireCount_ = grown.retireCount_;
    retireImmediateCount_ = grown.retireImmediateCount_;
    stealCount_ = grown.stealCount_;
    releasingCount_.store(grown.GetReleasingCount(), std::memory_order_relaxed);
    releasingRingHits_ = grown.releasingRingHits_;
    releasingRingHead_ = 0u;
    releasingRingCount_ = grown.releasingRingCount_;
    releasingRingCapacity_ = grown.releasingRingCapacity_;
    releasingRingMask_ = grown.releasingRingMask_;
    for (uint32_t i = 0u; i < grown.releasingRingCount_; ++i)
        releasingRing_[i & releasingRingMask_] =
            grown.releasingRing_[i & grown.releasingRingMask_];

    _aligned_free(oldMetadataStorage);
    PublishRuntimeVoicePoolCapacity(capacity);
    return true;
}

inline bool VoiceManager::SetVoiceLimit(uint32_t limit) {
    if (limit == 0u || limit > maxVoices_) return false;
    voiceLimit_ = limit;
    PublishAppliedRuntimeVoiceLimit(limit);
    return true;
}

inline uint32_t VoiceManager::EnforceVoiceLimit(uint32_t maxReleases,
                                                 float releaseSeconds) {
    ++stealPeekEpoch_;
    if (voiceLimit_ == 0u || maxReleases == 0u) return 0u;
    const uint32_t releasing = GetReleasingCount();
    const uint32_t primaryVoices = activeCount_ > releasing
        ? activeCount_ - releasing : 0u;
    if (primaryVoices <= voiceLimit_) return 0u;

    uint32_t remaining = primaryVoices - voiceLimit_;
    remaining = (std::min)(remaining, maxReleases);
    const float seconds = (std::max)(0.005f, (std::min)(0.250f, releaseSeconds));
    const float releaseDecay = MakeReleaseDecay(seconds, sampleRate_);
    const uint32_t releaseSamples = MakeReleaseSamples(seconds, sampleRate_);

    uint32_t released = 0u;
    while (released < remaining) {
        uint32_t position = 0u;
        const VoiceHandle victim = PopStealCandidate(position, false);
        if (victim == kInvalidVoice) break;
        if (v.state[victim] != static_cast<uint8_t>(VoiceState::Active))
            continue;

        EnsureMaterialized(victim);
        v.heldBySustain[victim] = 0u;
        v.heldBySostenuto[victim] = 0u;
        v.releaseStartInBlock[victim] = 0u;
        v.releaseDecay[victim] = releaseDecay;
        v.releaseSamplesRemaining[victim] = releaseSamples;
        // PopStealCandidate already removed this victim.  Suppress the
        // release reclassification's candidate update so a large live cap
        // reduction does not remove, reinsert, then remove every victim.
        stealCandidateDeferred_[victim] = 1u;
        StartRelease(victim);
        stealCandidateDeferred_[victim] = 0u;
        ++released;
    }

    stealHeapValid_ = false;
    stealVolatileHeapValid_ = false;
    return released;
}

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
inline void VoiceManager::TrackLaunchFrameForTest(uint64_t frame) {
    if (!launchChurnProfilingEnabled_) return;
    if (frame == launchTestTrackedFrame_) return;
    if (launchTestProvisionalGroups_ > 0) {
        launchChurnStats_.nextFrameSurvivingGroups +=
            static_cast<uint64_t>(launchTestProvisionalGroups_);
    }
    if (launchTestProvisionalPhysicalVoices_ > 0) {
        launchChurnStats_.nextFrameSurvivingPhysicalVoices +=
            static_cast<uint64_t>(launchTestProvisionalPhysicalVoices_);
    }
    launchTestTrackedFrame_ = frame;
    launchTestProvisionalGroups_ = 0;
    launchTestProvisionalPhysicalVoices_ = 0;
}

inline uint64_t VoiceManager::BeginLaunchStageForTest() const {
#if defined(_MSC_VER)
    return launchTestContext_.active && launchTestContext_.sampled
        ? __rdtsc() : 0u;
#else
    return 0u;
#endif
}

inline void VoiceManager::EndLaunchStageForTest(LaunchProfileStage stage,
                                                 uint64_t begin) {
#if defined(_MSC_VER)
    if (begin == 0u || !launchTestContext_.active ||
        !launchTestContext_.sampled) return;
    const uint32_t index = static_cast<uint32_t>(stage);
    if (index < static_cast<uint32_t>(LaunchProfileStage::Count))
        launchTestContext_.stageCycles[index] += __rdtsc() - begin;
#else
    (void)stage;
    (void)begin;
#endif
}

inline void VoiceManager::BeginLaunchTestProfile(
    uint8_t channel, uint8_t note, const VoiceConfiguration* setups,
    uint32_t count) {
    if (!launchChurnProfilingEnabled_) {
        launchTestContext_.active = false;
        return;
    }
    TrackLaunchFrameForTest(currentFrame_);
    ++launchChurnStats_.logicalLaunches;
    launchChurnStats_.physicalVoicesRequested += count;
    launchTestContext_ = LaunchTestContext{};
    launchTestContext_.active = true;
    launchTestContext_.sampled =
        ((launchChurnStats_.logicalLaunches - 1u) & 4095u) == 0u;
    launchTestContext_.incomingChannel = channel;
    launchTestContext_.incomingNote = note;
    launchTestContext_.incomingCount = count;
    launchTestContext_.incomingSetups = setups;
#if defined(_MSC_VER)
    if (launchTestContext_.sampled)
        launchTestContext_.beginCycles = __rdtsc();
#endif
}

inline void VoiceManager::FinishLaunchTestProfile(bool success) {
    if (!launchTestContext_.active) return;
    if (success) {
        ++launchChurnStats_.successfulLaunches;
        launchChurnStats_.physicalVoicesConfigured +=
            launchTestContext_.incomingCount;
        ++launchTestProvisionalGroups_;
        launchTestProvisionalPhysicalVoices_ +=
            launchTestContext_.incomingCount;
    } else {
        ++launchChurnStats_.failedLaunches;
    }
    if (launchTestContext_.victimGroups != 0u)
        ++launchChurnStats_.stealTransactions;

    uint32_t bucket = LaunchChurnStats::kClassificationBuckets - 1u;
    if (launchTestContext_.victimGroups != 0u) {
        bucket = 0u;
        if (launchTestContext_.allVictimsSameFrame) bucket |= 1u;
        if (launchTestContext_.anyLayeredVictim) bucket |= 2u;
        if (launchTestContext_.anyVolatileVictim) bucket |= 4u;
        if (launchTestContext_.anyGeneralVictim) bucket |= 8u;
    }
    if (launchTestContext_.sampled &&
        bucket < LaunchChurnStats::kClassificationBuckets) {
        LaunchChurnBucketStats& result = launchChurnStats_.buckets[bucket];
        ++result.samples;
#if defined(_MSC_VER)
        result.totalCycles += __rdtsc() - launchTestContext_.beginCycles;
#endif
        for (uint32_t stage = 0u;
             stage < static_cast<uint32_t>(LaunchProfileStage::Count);
             ++stage) {
            result.stageCycles[stage] +=
                launchTestContext_.stageCycles[stage];
        }
    }
    launchTestContext_.active = false;
}

inline void VoiceManager::RecordVictimGroupForTest(
    VoiceHandle selected, LaunchVictimPath path, bool reservedInPlace) {
    if (!launchTestContext_.active || selected >= maxVoices_ ||
        v.state[selected] == static_cast<uint8_t>(VoiceState::Free)) return;

    VoiceHandle smallGroup[8]{};
    uint32_t smallCount = 0u;
    uint32_t groupCount = 0u;
    uint32_t sameFrameCount = 0u;
    bool allStable = true;
    auto observe = [&](VoiceHandle handle) {
        if (handle >= maxVoices_ || groupCount >= maxVoices_) return;
        if (smallCount < 8u) smallGroup[smallCount++] = handle;
        ++groupCount;
        if (v.birthFrame[handle] == currentFrame_) ++sameFrameCount;
        if (!IsStableStealCandidate(handle)) allStable = false;
    };

    int32_t linked = playGroupPrev_[selected];
    while (linked >= 0 && groupCount < maxVoices_) {
        const VoiceHandle handle = static_cast<VoiceHandle>(linked);
        linked = playGroupPrev_[handle];
        observe(handle);
    }
    observe(selected);
    linked = playGroupNext_[selected];
    while (linked >= 0 && groupCount < maxVoices_) {
        const VoiceHandle handle = static_cast<VoiceHandle>(linked);
        linked = playGroupNext_[handle];
        observe(handle);
    }
    if (groupCount == 0u) return;

    const bool allSameFrame = sameFrameCount == groupCount;
    ++launchChurnStats_.victimGroups;
    launchChurnStats_.physicalVictims += groupCount;
    launchChurnStats_.sameFramePhysicalVictims += sameFrameCount;
    if (allSameFrame) ++launchChurnStats_.sameFrameVictimGroups;
    if (groupCount == 1u) ++launchChurnStats_.monoVictimGroups;
    else ++launchChurnStats_.layeredVictimGroups;
    if (groupCount == launchTestContext_.incomingCount)
        ++launchChurnStats_.matchingSizeVictimGroups;
    else
        ++launchChurnStats_.mismatchedSizeVictimGroups;
    if (allStable) ++launchChurnStats_.stableVictimGroups;
    else ++launchChurnStats_.volatileVictimGroups;
    if (v.channel[selected] == launchTestContext_.incomingChannel &&
        v.note[selected] == launchTestContext_.incomingNote) {
        ++launchChurnStats_.sameChannelKeyVictimGroups;
    }

    bool matchingPlan = groupCount == launchTestContext_.incomingCount &&
        groupCount <= 8u && smallCount == groupCount &&
        launchTestContext_.incomingSetups != nullptr;
    if (matchingPlan) {
        uint32_t oldIdentities[8]{};
        uint32_t newIdentities[8]{};
        for (uint32_t i = 0u; i < groupCount; ++i) {
            oldIdentities[i] =
                (static_cast<uint32_t>(v.presetIndex[smallGroup[i]]) << 16u) |
                v.regionIndex[smallGroup[i]];
            newIdentities[i] =
                (static_cast<uint32_t>(
                    launchTestContext_.incomingSetups[i].presetIndex) << 16u) |
                launchTestContext_.incomingSetups[i].regionIndex;
        }
        std::sort(oldIdentities, oldIdentities + groupCount);
        std::sort(newIdentities, newIdentities + groupCount);
        matchingPlan = std::equal(oldIdentities,
                                  oldIdentities + groupCount,
                                  newIdentities);
    }
    if (matchingPlan) ++launchChurnStats_.matchingPlanVictimGroups;

    switch (path) {
        case LaunchVictimPath::SingleInPlace:
            ++launchChurnStats_.singleInPlaceVictimGroups;
            break;
        case LaunchVictimPath::MatchingReuse:
            ++launchChurnStats_.matchingReuseVictimGroups;
            if (reservedInPlace)
                ++launchChurnStats_.reservedReuseVictimGroups;
            break;
        case LaunchVictimPath::General:
            ++launchChurnStats_.generalVictimGroups;
            break;
    }

    ++launchTestContext_.victimGroups;
    launchTestContext_.sameFramePhysicalVictims += sameFrameCount;
    launchTestContext_.allVictimsSameFrame &= allSameFrame;
    launchTestContext_.anyLayeredVictim |= groupCount > 1u;
    launchTestContext_.anyVolatileVictim |= !allStable;
    launchTestContext_.anyGeneralVictim |=
        path == LaunchVictimPath::General || !reservedInPlace;

    launchTestProvisionalPhysicalVoices_ -= sameFrameCount;
    if (allSameFrame) --launchTestProvisionalGroups_;
}
#endif

inline void VoiceManager::ApplyRuntimeVoiceLimit(uint64_t frame) {
    ++stealPeekEpoch_;
    SetCurrentFrame(frame);
    // RuntimeLink publishes only a process-local atomic request. Applying it
    // here keeps all VoiceManager mutation on the audio/render thread. This
    // method is called once before a render block is planned, never while
    // render workers are consuming the block.
    uint32_t requestedLimit = RequestedRuntimeVoiceLimit();
    if (requestedLimit > RuntimeVoiceGrowthCeiling()) {
        RequestRuntimeVoiceLimit(voiceLimit_);
        requestedLimit = voiceLimit_;
    }
    if (requestedLimit > maxVoices_ && requestedLimit <= kMaxPolyphony) {
        if (!GrowCapacity(requestedLimit)) {
            // Do not retry a failed large allocation every render boundary.
            // Keep the last applied cap and let a later user action request
            // growth again explicitly.
            RequestRuntimeVoiceLimit(voiceLimit_);
            requestedLimit = voiceLimit_;
        }
    }
    if (requestedLimit != 0u && requestedLimit <= maxVoices_ &&
        requestedLimit != voiceLimit_) {
        SetVoiceLimit(requestedLimit);
        lastVoiceLimitEnforceFrame_ = UINT64_MAX; // enforce this boundary
    }

    if (voiceLimit_ != 0u && activeCount_ > voiceLimit_) {
        const uint64_t enforceInterval =
            (std::max)(uint64_t{1}, static_cast<uint64_t>(sampleRate_) / 100u);
        if (lastVoiceLimitEnforceFrame_ == UINT64_MAX ||
            frame >= lastVoiceLimitEnforceFrame_ + enforceInterval) {
            // Bound each reduction pass so a giant 100k -> 1k change cannot
            // spend an unbounded amount of one callback in victim selection.
            // Repeated boundaries continue shedding until the logical cap is
            // reached; each victim receives the requested ~50 ms release.
            EnforceVoiceLimit(256u, 0.050f);
            lastVoiceLimitEnforceFrame_ = frame;
        }
    }
}

inline void VoiceManager::SetCurrentFrame(uint64_t frame) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    TrackLaunchFrameForTest(frame);
#endif
    if (frame != currentFrame_) stealTailMinHeapValid_ = false;
    currentFrame_ = frame;
}
