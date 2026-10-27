// Part of VoiceManager.h; included from there inside namespace svms. Not standalone.

inline void VoiceManager::LinkStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_ || handle >= kStealTailReserve ||
        v.stealTailFramesRemaining[handle] == 0u ||
        stealTailPosition_[handle] < stealTailCount_) return;
    const uint32_t position = stealTailCount_++;
    stealTailList_[position] = handle;
    stealTailPosition_[handle] = position;
    stealTailMinHeapValid_ = false;
}

inline void VoiceManager::UnlinkStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_ || handle >= kStealTailReserve) return;
    const uint32_t position = stealTailPosition_[handle];
    if (position >= stealTailCount_) return;
    const uint32_t lastPosition = --stealTailCount_;
    if (position != lastPosition) {
        const uint32_t moved = stealTailList_[lastPosition];
        stealTailList_[position] = moved;
        stealTailPosition_[moved] = position;
    }
    stealTailPosition_[handle] = UINT32_MAX;
    stealTailMinHeapValid_ = false;
}

inline void VoiceManager::RefreshStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_ || handle >= kStealTailReserve) return;
    if (v.stealTailFramesRemaining[handle] != 0u)
        LinkStealTail(handle);
    else
        UnlinkStealTail(handle);
}

inline float VoiceManager::ComputeTailLevel(uint32_t tailSlot) const {
    if (tailSlot >= maxVoices_ || tailSlot >= kStealTailReserve ||
        v.stealTailFramesRemaining[tailSlot] == 0u) return 0.0f;
    const uint32_t total = v.stealTailFramesTotal[tailSlot];
    const float fade = total > 1u
        ? static_cast<float>(v.stealTailFramesRemaining[tailSlot] - 1u) /
          static_cast<float>(total - 1u)
        : 0.0f;
    const float gain = std::fabs(v.stealTailGain[tailSlot]) * fade;
    return gain * (std::fabs(v.stealTailMixGainL[tailSlot]) +
                   std::fabs(v.stealTailMixGainR[tailSlot]));
}

inline void VoiceManager::StealTailHeapSiftDown(uint32_t position) {
    for (;;) {
        const uint32_t left = position * 2u + 1u;
        if (left >= stealTailMinHeapCount_) break;
        const uint32_t right = left + 1u;
        uint32_t quietest = left;
        if (right < stealTailMinHeapCount_ &&
            stealTailMinHeapKey_[right] < stealTailMinHeapKey_[left])
            quietest = right;
        if (stealTailMinHeapKey_[quietest] >=
            stealTailMinHeapKey_[position]) break;
        const uint64_t temporaryKey = stealTailMinHeapKey_[position];
        stealTailMinHeapKey_[position] = stealTailMinHeapKey_[quietest];
        stealTailMinHeapKey_[quietest] = temporaryKey;
        position = quietest;
    }
}

inline void VoiceManager::BuildStealTailMinHeap() {
    stealTailMinHeapCount_ = (std::min)(stealTailCount_, kStealTailReserve);
    for (uint32_t i = 0; i < stealTailMinHeapCount_; ++i) {
        const uint32_t tailSlot = stealTailList_[i];
        const float level = ComputeTailLevel(tailSlot);
        uint32_t levelBits = 0u;
        std::memcpy(&levelBits, &level, sizeof(levelBits));
        // Tail levels are finite and non-negative, so IEEE-754 bit order is
        // numeric order. The low word preserves the existing list-position
        // tie rule without a second load or floating-point branch.
        stealTailMinHeapKey_[i] =
            (static_cast<uint64_t>(levelBits) << 32u) |
            stealTailPosition_[tailSlot];
    }
    if (stealTailMinHeapCount_ > 1u) {
        for (uint32_t position = stealTailMinHeapCount_ / 2u;
             position-- > 0u;) StealTailHeapSiftDown(position);
    }
    stealTailMinHeapFrame_ = currentFrame_;
    stealTailMinHeapValid_ = true;
}

inline uint32_t VoiceManager::SelectStealTailSlot(
    float outgoingLevel, bool& replacingHeapRoot) {
    replacingHeapRoot = false;
    const uint32_t reserveLimit = (std::min)(maxVoices_, kStealTailReserve);
    if (reserveLimit == 0u || outgoingLevel <= 0.0f) return UINT32_MAX;
    if (stealTailCount_ < reserveLimit) {
        for (uint32_t slot = 0; slot < reserveLimit; ++slot) {
            if (v.stealTailFramesRemaining[slot] == 0u) return slot;
        }
        return UINT32_MAX;
    }

    if (!stealTailMinHeapValid_ || stealTailMinHeapFrame_ != currentFrame_)
        BuildStealTailMinHeap();
    const uint64_t quietestKey = stealTailMinHeapKey_[0];
    const uint32_t quietestBits = static_cast<uint32_t>(quietestKey >> 32u);
    float quietestLevel = 0.0f;
    std::memcpy(&quietestLevel, &quietestBits, sizeof(quietestLevel));
    if (outgoingLevel <= quietestLevel) return UINT32_MAX;
    replacingHeapRoot = true;
    return stealTailList_[static_cast<uint32_t>(quietestKey)];
}

inline void VoiceManager::CaptureStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    // The ghost copy (preTailCaptureHook_) and the tail capture both read the
    // victim's dense arrays: a victim launched earlier in this plan must have
    // its record applied first.
    EnsureMaterialized(handle);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t tailBegin = BeginLaunchStageForTest();
    if (launchTestContext_.active)
        ++launchChurnStats_.tailCaptureAttempts;
#endif
    if (preTailCaptureHook_)
        preTailCaptureHook_(handle, preTailCaptureUserData_);
    if (stealTailCaptureSuppressed_) {
        // Whole-voice renderer: the victim lives on as a ghost snapshot with
        // exact-frame retirement; the tail fade is reproduced from the
        // ghost's state at the steal frame.  Count the attempt so telemetry
        // stays total.
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (launchTestContext_.active)
            ++launchChurnStats_.tailCaptureRejected;
        EndLaunchStageForTest(LaunchProfileStage::TailCapture, tailBegin);
#endif
        return;
    }
    const float gain = v.currentGain[handle];
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const float outgoingLevel = std::fabs(gain) *
        (std::fabs(mixL) + std::fabs(mixR));
    if (v.sampleBacked[handle] == 0u || v.relEnd[handle] <= 1u ||
        outgoingLevel <= VoiceRetireThreshold()) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (launchTestContext_.active)
            ++launchChurnStats_.tailCaptureIneligible;
        EndLaunchStageForTest(LaunchProfileStage::TailCapture, tailBegin);
#endif
        return;
    }
    bool replacingHeapRoot = false;
    const uint32_t tailSlot = SelectStealTailSlot(
        outgoingLevel, replacingHeapRoot);
    if (tailSlot == UINT32_MAX) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (launchTestContext_.active)
            ++launchChurnStats_.tailCaptureRejected;
        EndLaunchStageForTest(LaunchProfileStage::TailCapture, tailBegin);
#endif
        return;
    }

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const bool replacingTail =
        v.stealTailFramesRemaining[tailSlot] != 0u;
    if (launchTestContext_.active) {
        ++launchChurnStats_.tailCaptureAccepted;
        if (replacingTail) ++launchChurnStats_.tailCaptureReplaced;
    }
#endif

    v.stealTailPhase[tailSlot] = v.phases[handle];
    v.stealTailPhaseInc[tailSlot] = v.phaseIncs[handle];
    v.stealTailGain[tailSlot] = gain;
    v.stealTailMixGainL[tailSlot] = mixL;
    v.stealTailMixGainR[tailSlot] = mixR;
    v.stealTailFilterA0[tailSlot] = v.filterA0[handle];
    v.stealTailFilterB1[tailSlot] = v.filterB1[handle];
    v.stealTailFilterB2[tailSlot] = v.filterB2[handle];
    v.stealTailFilterZ1[tailSlot] = v.filterZ1[handle];
    v.stealTailFilterZ2[tailSlot] = v.filterZ2[handle];
    v.stealTailFilterEnabled[tailSlot] =
        static_cast<uint8_t>(v.filterEnabled[handle] != 0u);
    v.stealTailSampleStart[tailSlot] = v.sampleStart[handle];
    v.stealTailRelEnd[tailSlot] = v.relEnd[handle];
    v.stealTailRelLoopS[tailSlot] = v.relLoopS[handle];
    v.stealTailRelLoopE[tailSlot] = v.relLoopE[handle];
    v.stealTailRelLoopSF[tailSlot] = v.relLoopSF[handle];
    v.stealTailRelLoopEF[tailSlot] = v.relLoopEF[handle];
    v.stealTailSampleBacked[tailSlot] = v.sampleBacked[handle];
    v.stealTailLoopEnabled[tailSlot] = v.loopEnabled[handle];
    v.stealTailChannel[tailSlot] = v.channel[handle];
    v.stealTailFramesRemaining[tailSlot] = stealFadeFrames_;
    v.stealTailFramesTotal[tailSlot] = stealFadeFrames_;
    // Carry the victim's rotation state over so the tail keeps the same
    // phase profile — no spectral jump at the steal boundary.
    if (v.rot) v.stealTailRot[tailSlot] = v.rot[handle];
    if (replacingHeapRoot) {
        // SelectStealTailSlot accepts a replacement only when it is louder
        // than the current root, so the updated root can move only downward.
        uint32_t levelBits = 0u;
        std::memcpy(&levelBits, &outgoingLevel, sizeof(levelBits));
        stealTailMinHeapKey_[0] =
            (static_cast<uint64_t>(levelBits) << 32u) |
            stealTailPosition_[tailSlot];
        StealTailHeapSiftDown(0u);
    } else {
        LinkStealTail(static_cast<VoiceHandle>(tailSlot));
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::TailCapture, tailBegin);
#endif
}

inline void VoiceManager::RetireStolenSibling(VoiceHandle handle,
                                               VoiceHandle selectedVictim) {
    if (handle >= maxVoices_ ||
        v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t treeBegin = BeginLaunchStageForTest();
#endif
    if (stealHeapValid_) RemoveStealCandidate(handle);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance, treeBegin);
#endif
    CaptureStealTail(handle);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t lifecycleBegin = BeginLaunchStageForTest();
#endif
    ++stealCount_;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
        releasingCount_.fetch_sub(1u, std::memory_order_relaxed);
    UnlinkChannelKey(handle);
    UnlinkExclusiveClass(handle);
    UnlinkPlayGroup(handle);
    UnlinkChannelActive(handle);
    UnlinkRenderClass(handle);
    stealCandidateDeferred_[handle] = 0u;
    stealCandidateReserved_[handle] = 0u;
    v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
    v.currentGain[handle] = 0.0f;
    freeStack_[freeTop_++] = static_cast<int32_t>(handle);
    assert(freeTop_ <= maxVoices_ && "freeStack_ overflow in grouped stealing");

    const uint32_t position = activePosition_[handle];
    assert(position < activeCount_ && activeList_[position] == handle);
    const uint32_t lastPosition = --activeCount_;
    if (position != lastPosition) {
        const uint32_t moved = activeList_[lastPosition];
        activeList_[position] = moved;
        activePosition_[moved] = position;
        // Heap ties use active-list position. The already-popped selected
        // victim deliberately remains absent until its replacement is ready.
        if (stealHeapValid_ && moved != selectedVictim)
            UpdateStealCandidate(static_cast<VoiceHandle>(moved));
    }
    activePosition_[handle] = UINT32_MAX;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Lifecycle, lifecycleBegin);
#endif
}

inline bool VoiceManager::HigherPriorityCandidate(const StealCandidate& a,
                                                   const StealCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.activePosition < b.activePosition;
}

SVMS_VM_FORCEINLINE uint64_t VoiceManager::EncodeStableWinnerKey(
    float score, uint32_t activePosition) {
    if (score == 0.0f) score = 0.0f;
    uint32_t bits = 0u;
    std::memcpy(&bits, &score, sizeof(bits));
    const uint32_t orderedScore = (bits & 0x80000000u) != 0u
        ? ~bits : bits ^ 0x80000000u;
    return (static_cast<uint64_t>(orderedScore) << 32u) |
        (UINT32_MAX - activePosition);
}

SVMS_VM_FORCEINLINE void VoiceManager::BucketPush(VoiceHandle handle) {
    const uint64_t key = stealWinnerTree_[stealTreeLeafBase_ + handle];
    if (key == 0u) {
        stableBuckets_.DropIfHead(handle);
        return;
    }
    // Pool exhausted by stale entries: rebuild from the leaves, which
    // already include this handle.
    if (!stableBuckets_.Push(handle, key, bucketFront_)) BuildBuckets();
}

inline void VoiceManager::BuildBuckets() {
    stableBuckets_.Clear();
    // Active-list order, so a rebuild keeps the oldest-first order among
    // equal levels instead of reshuffling it by handle number.
    for (uint32_t position = 0u; position < activeCount_; ++position) {
        const uint32_t handle = activeList_[position];
        const uint64_t key = stealWinnerTree_[stealTreeLeafBase_ + handle];
        if (key != 0u) stableBuckets_.PushUnchecked(handle, key);
    }
}

inline uint64_t VoiceManager::BucketRoot() {
    return stableBuckets_.Root(stealWinnerTree_ + stealTreeLeafBase_, nullptr);
}

// Volatile (decaying / releasing) candidates in bucket mode: volKey_[handle]
// is the key while the handle is in the current block's set, 0 otherwise.
inline void VoiceManager::VolBucketSet(VoiceHandle handle) {
    const bool had = volKey_[handle] != 0u;
    const uint64_t key = EncodeStableWinnerKey(
        ComputeStableStealKey(handle), activePosition_[handle]);
    volKey_[handle] = key;
    if (!had) ++stealVolatileHeapCount_;
    if (!volBuckets_.Push(handle, key, bucketFront_)) RebuildVolBuckets();
}

inline void VoiceManager::RebuildVolBuckets() {
    volBuckets_.Clear();
    for (uint32_t i = 0u; i < stealVolatileCount_; ++i) {
        const uint32_t handle = stealVolatileList_[i];
        if (volKey_[handle] != 0u)
            volBuckets_.PushUnchecked(handle, volKey_[handle]);
    }
}

// Best volatile candidate: (key, handle), false when there is none.
inline bool VoiceManager::VolHeapRoot(uint64_t& key, VoiceHandle& handle) {
    key = 0u;
    handle = kInvalidVoice;
    if (stealVolatileHeapCount_ == 0u) return false;
    if (bucketSteal_) {
        uint32_t found = 0u;
        key = volBuckets_.Root(volKey_, &found);
        if (key == 0u) return false;
        handle = static_cast<VoiceHandle>(found);
        return true;
    }
    key = stealVolatileHeapKey_[0];
    handle = stealVolatileHeapHandle_[0];
    return true;
}

SVMS_VM_FORCEINLINE void VoiceManager::RefreshStealWinnerPath(
    VoiceHandle handle) {
    if (bucketSteal_) {
        BucketPush(handle);
        return;
    }
    uint32_t node = stealTreeLeafBase_ + handle;
    uint64_t winner = stealWinnerTree_[node];
    while (node > 1u) {
        const uint64_t sibling = stealWinnerTree_[node ^ 1u];
        winner = winner > sibling ? winner : sibling;
        node >>= 1u;
        stealWinnerTree_[node] = winner;
    }
}

inline void VoiceManager::RefreshStealWinnerPaths(
    const VoiceHandle* handles, uint32_t count) {
    // Cached launch plans cover at most eight layers. Recompute the union of
    // their tournament-tree paths bottom-up so shared ancestors are touched
    // once instead of once per physical voice.
    static constexpr uint32_t kBatchPaths = 8u;
    if (!handles || count == 0u) return;
    if (bucketSteal_) {
        for (uint32_t i = 0u; i < count; ++i) BucketPush(handles[i]);
        return;
    }
    if (count == 1u) {
        RefreshStealWinnerPath(handles[0]);
        return;
    }
    if (count > kBatchPaths) {
        for (uint32_t i = 0u; i < count; ++i)
            RefreshStealWinnerPath(handles[i]);
        return;
    }

    uint32_t nodes[kBatchPaths]{};
    uint32_t nodeCount = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t node = (stealTreeLeafBase_ + handles[i]) >> 1u;
        bool duplicate = false;
        for (uint32_t existing = 0u; existing < nodeCount; ++existing)
            duplicate |= nodes[existing] == node;
        if (!duplicate) nodes[nodeCount++] = node;
    }

    while (nodeCount != 0u) {
        uint32_t parentCount = 0u;
        for (uint32_t i = 0u; i < nodeCount; ++i) {
            const uint32_t node = nodes[i];
            const uint64_t left = stealWinnerTree_[node << 1u];
            const uint64_t right = stealWinnerTree_[(node << 1u) + 1u];
            stealWinnerTree_[node] = left > right ? left : right;
            if (node == 1u) continue;
            const uint32_t parent = node >> 1u;
            bool duplicate = false;
            for (uint32_t existing = 0u; existing < parentCount; ++existing)
                duplicate |= nodes[existing] == parent;
            if (!duplicate) nodes[parentCount++] = parent;
        }
        nodeCount = parentCount;
    }
}

inline void VoiceManager::RebuildStableWinnerTree() {
    if (bucketSteal_) {
        BuildBuckets();
        return;
    }
    for (uint32_t node = stealTreeLeafBase_; node-- > 1u;) {
        const uint64_t left = stealWinnerTree_[node << 1u];
        const uint64_t right = stealWinnerTree_[(node << 1u) + 1u];
        stealWinnerTree_[node] = left > right ? left : right;
    }
}

inline void VoiceManager::BuildStealHeap() {
    ++stealHeapBuildCount_;
    stealHeapCount_ = 0u;
    stealVolatileCount_ = 0u;
    stealVolatileHeapCount_ = 0u;
    stealVolatileHeapValid_ = false;
    std::memset(stealWinnerTree_, 0,
                sizeof(*stealWinnerTree_) * stealTreeLeafBase_ * 2u);
    for (uint32_t i = 0u; i < maxVoices_; ++i)
        stealVolatilePosition_[i] = UINT32_MAX;
    if (bucketSteal_) std::memset(volKey_, 0, sizeof(*volKey_) * maxVoices_);
    for (uint32_t position = 0; position < activeCount_; ++position) {
        const uint32_t handle = activeList_[position];
        if (IsStableStealCandidate(handle)) {
            ++stealHeapCount_;
            const float score = ComputeStableStealKey(handle);
            stealStableKey_[handle] = EncodeStableWinnerKey(
                score, position);
            stealWinnerTree_[stealTreeLeafBase_ + handle] =
                stealStableKey_[handle];
        } else {
            LinkVolatileCandidate(static_cast<VoiceHandle>(handle));
        }
    }
    RebuildStableWinnerTree();
    stealHeapValid_ = true;
}

inline VoiceHandle VoiceManager::PopStealCandidate(uint32_t& activePosition,
                                                    bool reserveVolatileRoot) {
    if (!stealHeapValid_) BuildStealHeap();

    // Releasing-ring fast path. Under saturated chopped-note churn the pool
    // is dominated by end-of-life Releasing voices, so the exhaustive
    // winner search adds nothing there: any Releasing voice sits at the
    // bottom of BASSMIDI's effective-level ranking anyway (its envelope has
    // left sustain). Engages only on a full free stack with the pool ≥85%
    // occupied; every other case falls through to the exact existing tier
    // selection unchanged, keeping sparse/mixed behavior byte-identical.
    // Root-reserving probes keep their exact tier semantics.
    if (!reserveVolatileRoot && ReleasingRingEligible()) {
        uint32_t ringPosition = 0u;
        const VoiceHandle ringVictim =
            NextValidReleasingRingVictim(ringPosition, true);
        if (ringVictim != kInvalidVoice) {
            // Repair the incremental index: the victim is unlinked from
            // whichever tier held it, mirroring tier-pop side effects.
            PrefetchVoiceLines(ringVictim);
            RemoveStealCandidate(ringVictim);
            ++releasingRingHits_;
            activePosition = ringPosition;
            return ringVictim;
        }
    }

    // Saturated sustained playback has no decay/release candidates. The
    // tournament root is already the exact exhaustive winner, including the
    // active-position tie, so avoid constructing/comparing generic candidate
    // records and touching the empty volatile heap on every note launch.
    if (stealHeapCount_ > 0u && stealVolatileCount_ == 0u) {
        const uint64_t rootKey = StealTreeRoot();
        const uint32_t winnerPosition =
            UINT32_MAX - static_cast<uint32_t>(rootKey);
        if (rootKey == 0u || winnerPosition >= activeCount_) {
            // Steal-index invariant violated: a stale or unlinked key reached
            // the winner-tree root. Release builds compile the invariant
            // asserts out, and the direct activeList_[winnerPosition] read
            // below faults (observed as 0xC0000005 on the audio thread after
            // CC120/123 kills or a live voice-limit reduction). Rebuild the
            // index from live pool state and fall through to the general
            // selection instead. Cheap bounds test only — the full linkage
            // validation runs on the general path below.
            stealHeapValid_ = false;
            ++stealIndexRepairCount_;
            BuildStealHeap();
        } else {
            const VoiceHandle winner = static_cast<VoiceHandle>(
                activeList_[winnerPosition]);
            assert(stealStableKey_[winner] == rootKey);
            // Fetch the victim's SoA lines while the winner-path repair
            // below walks its ~log2(pool) scattered nodes.
            PrefetchVoiceLines(winner);
            activePosition = winnerPosition;
            if (reserveVolatileRoot) {
                stealCandidateReserved_[winner] = 2u;
            } else {
                --stealHeapCount_;
                stealWinnerTree_[stealTreeLeafBase_ + winner] = 0u;
                RefreshStealWinnerPath(winner);
            }
            return winner;
        }
    }

    // Transient gains change while samples render — not between equal-frame
    // MIDI events.  RenderBlock invalidates the heap once per block, so the
    // first steal after a render rebuilds and the rest of that dispatch run
    // reuses it.  The former per-frame stamp check rebuilt on EVERY event
    // whose frame advanced (the whole-voice plan loop advances the frame per
    // event), costing O(V log V) per note-on at saturated pools.  Steal keys
    // are frame-free (ComputeStableStealKey's age term cancels), so keys
    // inserted at any frame compare exactly against the heap's contents.
    if (!stealVolatileHeapValid_)
        BuildVolatileStealHeap();

    uint64_t bestKey = 0u;
    uint32_t bestHandle = UINT32_MAX;
    uint32_t bestPosition = 0u;
    bool haveBest = false;
    bool bestIsVolatile = false;
    if (stealHeapCount_ > 0u) {
        const uint64_t rootKey = StealTreeRoot();
        const uint32_t winnerPosition =
            UINT32_MAX - static_cast<uint32_t>(rootKey);
        if (rootKey == 0u || winnerPosition >= activeCount_ ||
            activePosition_[activeList_[winnerPosition]] != winnerPosition) {
            // Same invariant repair as the stable-heap fast path above.
            // BuildStealHeap resets the volatile heap, so rebuild it here
            // before the volatile candidate read below.
            stealHeapValid_ = false;
            ++stealIndexRepairCount_;
            BuildStealHeap();
            if (!stealVolatileHeapValid_) BuildVolatileStealHeap();
        }
        if (stealHeapCount_ > 0u) {
            const uint64_t rootKey = StealTreeRoot();
            const uint32_t winnerPosition =
                UINT32_MAX - static_cast<uint32_t>(rootKey);
            assert(rootKey != 0u && winnerPosition < activeCount_);
            const VoiceHandle stableWinner = static_cast<VoiceHandle>(
                activeList_[winnerPosition]);
            assert(stealStableKey_[stableWinner] == rootKey);
            bestKey = rootKey;
            bestHandle = stableWinner;
            bestPosition = winnerPosition;
            haveBest = true;
        }
    }
    if (stealVolatileHeapCount_ > 0u) {
        uint64_t candidateKey = 0u;
        VoiceHandle candidateHandle = kInvalidVoice;
        VolHeapRoot(candidateKey, candidateHandle);
        if (candidateKey != 0u && (!haveBest || candidateKey > bestKey)) {
            const uint32_t candidatePosition =
                UINT32_MAX - static_cast<uint32_t>(candidateKey);
            // Same linkage validation as the stable-tree root: a stale
            // volatile entry must be dropped, not returned.
            if (candidateKey != 0u && candidatePosition < activeCount_ &&
                activePosition_[candidateHandle] == candidatePosition &&
                activeList_[candidatePosition] == candidateHandle) {
                bestKey = candidateKey;
                bestHandle = candidateHandle;
                bestPosition = candidatePosition;
                haveBest = true;
                bestIsVolatile = true;
            } else {
                UnlinkVolatileCandidate(candidateHandle);
            }
        }
    }
    // A valid current-frame heap contains every linked volatile candidate.
    // LinkVolatileCandidate inserts into it, UnlinkVolatileCandidate removes
    // from both structures, and a reserved root deliberately remains in both
    // until its launch transaction commits. The former defensive list walk
    // therefore inspected the entire volatile population for every steal and
    // never contributed a candidate.
    assert(stealVolatileHeapCount_ == stealVolatileCount_);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    // Retain the redundant predecessor scan as an explicit benchmark oracle,
    // disabled by default and absent from production builds.
    if (volatileFallbackScanForTest_) {
        for (uint32_t i = 0u; i < stealVolatileCount_; ++i) {
            const uint32_t handle = stealVolatileList_[i];
            if (stealVolatileHeapPosition_[handle] < stealVolatileHeapCount_)
                continue;
            const uint32_t candidatePosition = activePosition_[handle];
            const uint64_t candidateKey = EncodeStableWinnerKey(
                ComputeStableStealKey(static_cast<VoiceHandle>(handle)),
                candidatePosition);
            if (!haveBest || candidateKey > bestKey) {
                bestKey = candidateKey;
                bestHandle = handle;
                bestPosition = candidatePosition;
                haveBest = true;
                bestIsVolatile = true;
            }
        }
    }
#endif
    if (!haveBest) return kInvalidVoice;
    // Same overlap as the fast paths: the general tier-selection tail
    // repairs the index before returning, so fetch the victim's lines now.
    PrefetchVoiceLines(static_cast<VoiceHandle>(bestHandle));
    activePosition = bestPosition;
    if (bestIsVolatile) {
        const bool canReserve = reserveVolatileRoot &&
            VolHeapIsRoot(static_cast<VoiceHandle>(bestHandle));
        if (canReserve) {
            stealCandidateReserved_[bestHandle] = 1u;
        } else {
            UnlinkVolatileCandidate(static_cast<VoiceHandle>(bestHandle));
        }
    } else {
        if (reserveVolatileRoot) {
            // A single transactional note launch overwrites this handle and
            // commits before another steal. Keep its leaf in place so commit
            // changes one winner-tree path instead of remove + insert paths.
            stealCandidateReserved_[bestHandle] = 2u;
        } else {
            --stealHeapCount_;
            stealWinnerTree_[stealTreeLeafBase_ + bestHandle] = 0u;
            RefreshStealWinnerPath(static_cast<VoiceHandle>(bestHandle));
        }
    }
    return static_cast<VoiceHandle>(bestHandle);
}

// Thin batch wrapper over PopStealCandidate(pos, false). Deliberately NOT a
// heap-algorithm optimization: every victim is produced by one complete
// single-victim pop, including full tree/ring/heap repair, so victim k sees
// exactly the structure state that the k-th of K sequential pops would see.
// This guarantees the batch picks the same N voices, in the same priority
// order, as K independent pops. The only saving is call-overhead amortization
// for the caller (a same-frame run of K note-ons currently re-enters
// LaunchVoiceGroup -> AllocateVoiceOrSteal -> PopStealCandidate per note).
// Note: a true algorithmic batching (bulk-extracting the N smallest keys with
// fewer than N repair operations) could exist in principle, but it would risk
// changing selection order and is explicitly out of scope for this task.
inline uint32_t VoiceManager::PopStealCandidates(uint32_t count,
                                                 VoiceHandle* outHandles,
                                                 uint32_t* outActivePositions) {
    uint32_t found = 0u;
    while (found < count) {
        const VoiceHandle victim =
            PopStealCandidate(outActivePositions[found], false);
        if (victim == kInvalidVoice) break;
        outHandles[found] = victim;
        ++found;
    }
    return found;
}

// Restores exactly the tier-index entry a reserve=false steal pop erased.
// Stable victims: rewrite the winner-tree leaf with the key the pop cleared,
// restore the heap count and repair the winner path — the precise inverse of
// the pop's `--stealHeapCount_; leaf = 0; RefreshStealWinnerPath` sequence.
// Volatile victims: LinkVolatileCandidate re-inserts into the candidate list
// and (same-frame valid) heap; the victim was the heap maximum when popped
// and no candidate with a larger key can appear within one exact-frame launch
// transaction (keys are frozen per frame and commits land only after the
// allocation loop), so it sifts back to the root the pop assumed. Heap-array
// order may differ from the pre-pop arrangement, but the heap is fully
// ordered by unique keys, so every subsequent selection is identical.
inline float VoiceManager::NewbornStealLevel(
    const VoiceConfiguration* setups, uint32_t count,
    const ChannelParamsSnapshot& cp, uint32_t multiplicity) const {
    const float scale = multiplicity > 1u ? static_cast<float>(multiplicity) : 1.0f;
    float level = 0.0f;
    for (uint32_t layer = 0u; layer < count; ++layer) {
        const float mixL = setups[layer].gainLeft * scale * cp.mixScaleLeft;
        const float mixR = setups[layer].gainRight * scale * cp.mixScaleRight;
        level = (std::max)(level, std::fabs(setups[layer].initialGain) *
                                      std::sqrt(mixL * mixL + mixR * mixR));
    }
    return level;
}

inline VoiceHandle VoiceManager::SameKeyRecycleVictim(
    uint8_t channel, uint8_t note, uint8_t velocity, float newLevel) const {
    // A release tail left on this key goes first once it is quiet enough.
    const int32_t tail = lastUnlinkedByKey_[channel][note];
    if (tail >= 0 && static_cast<uint32_t>(tail) < maxVoices_ &&
        v.state[tail] == static_cast<uint8_t>(VoiceState::Releasing) &&
        v.channel[tail] == channel && v.note[tail] == note &&
        (priorityStrong_ ||
         ComputeEffectiveStealLevel(static_cast<VoiceHandle>(tail)) <=
            0.8f * newLevel))
        return static_cast<VoiceHandle>(tail);
    const int32_t oldest = channelKeyVoiceOldest_[channel][note];
    if (oldest < 0) return kInvalidVoice;
    const VoiceHandle h = static_cast<VoiceHandle>(oldest);
    if (h >= maxVoices_ ||
        v.state[h] != static_cast<uint8_t>(VoiceState::Active))
        return kInvalidVoice;
    const uint8_t stage = v.envelopeStage[h];
    if (stage == 4u || stage == 0u || stage == 1u) return kInvalidVoice;
    if (v.velocity[h] > velocity) return kInvalidVoice;
    if (!priorityStrong_ && !(ComputeEffectiveStealLevel(h) < 0.5f * newLevel))
        return kInvalidVoice;
    return h;
}

inline bool VoiceManager::ShouldRejectNoteOn(
    const VoiceConfiguration* setups, uint32_t count,
    const ChannelParamsSnapshot& cp, uint32_t multiplicity,
    const PriorityIncoming* incoming) {
    // A layered note is admitted or skipped as a whole, judged by its loudest
    // layer (BASSMIDI judges each region voice alone; layers of one note share
    // a level closely enough that the whole-note call is the same in practice).
    for (uint32_t layer = 0u; layer < count; ++layer)
        if (setups[layer].exclusiveClass != 0u) return false;
    if (perKeyVoiceCap_ != 0u ||
        freeTop_ >= count || activeCount_ + count <= voiceLimit_ ||
        activeCount_ == 0u)
        return false;
    if (!stealHeapValid_) BuildStealHeap();
    VoiceHandle victim = kInvalidVoice;
    uint64_t floorKey = 0u;
    if (ReleasingRingEligible()) {
        uint32_t position = 0u;
        victim = NextValidReleasingRingVictim(position, false);
    } else {
        if (!stealVolatileHeapValid_) BuildVolatileStealHeap();
        uint64_t bestKey = 0u;
        if (stealHeapCount_ > 0u) {
            const uint64_t rootKey = StealRootKey();
            const uint32_t position = UINT32_MAX - static_cast<uint32_t>(rootKey);
            if (rootKey == 0u || position >= activeCount_) return false;
            victim = static_cast<VoiceHandle>(activeList_[position]);
            bestKey = rootKey;
            floorKey = rootKey;
        }
        if (stealVolatileHeapCount_ > 0u) {
            uint64_t key = 0u;
            VoiceHandle rootHandle = kInvalidVoice;
            VolHeapRoot(key, rootHandle);
            if (key != 0u && (victim == kInvalidVoice || key > bestKey)) {
                const uint32_t position = UINT32_MAX - static_cast<uint32_t>(key);
                if (position >= activeCount_) return false;
                victim = rootHandle;
                floorKey = key;
            }
        }
    }
    if (victim == kInvalidVoice || victim >= maxVoices_) return false;
    // Newborn score: age 0, level = |initialGain| * stereo energy norm of the
    // mix gains (what ApplyVoiceConfigurationFields caches as stealOutputGain,
    // merged-launch multiplicity included).
    const float level = NewbornStealLevel(setups, count, cp, multiplicity);
    if (incoming != nullptr) {
        // Velocity-lane admission: the best victim decides.  A tail goes unless
        // it is much louder than the note; anything else needs a strictly
        // higher lane, and bass and voices still in their attack are kept.
        if (v.state[victim] == static_cast<uint8_t>(VoiceState::Free))
            return false;
        if (pendingLaunch_.valid) {
            const PendingLaunch& p = pendingLaunch_;
            for (uint32_t i = 0u; i < p.count; ++i)
                if (p.handles[i] == victim) return true;
        }
        if (v.state[victim] == static_cast<uint8_t>(VoiceState::Releasing))
            return ComputeEffectiveStealLevel(victim) > level * 2.0f;
        if (v.channel[victim] == incoming->channel &&
            v.note[victim] == incoming->note)
            return false;
        const uint8_t stage = v.envelopeStage[victim];
        if (stage == 4u || stage == 0u || stage == 1u) return true;
        if (v.note[victim] < 48u && incoming->note > v.note[victim])
            return true;
        return StealLaneOfVelocity(v.velocity[victim]) >=
               StealLaneOfVelocity(incoming->velocity);
    }
    const float newbornScore = 0.0f - level * kBassMidiStealGainScale;
    if (bucketSteal_ && floorKey != 0u && stealAgeScale_ == 0.0f &&
        !pendingLaunch_.valid) {
        // The index already holds the floor's score in its key (no age term
        // when ranking is ageless), so the victim's row is never touched.
        const uint32_t ordered = static_cast<uint32_t>(floorKey >> 32u);
        const uint32_t bits = (ordered & 0x80000000u) != 0u
            ? ordered ^ 0x80000000u : ~ordered;
        float floorScore = 0.0f;
        std::memcpy(&floorScore, &bits, sizeof(floorScore));
        return newbornScore >= floorScore * (1.0f + 1e-5f);
    }
    if (v.state[victim] == static_cast<uint8_t>(VoiceState::Free))
        return false;
    // BASSMIDI 2018 (ageless ranking): a newcomer that is not strictly louder than
    // the quietest voice is dropped, so an equal level loses. The newborn level is
    // computed from the setup and the victim's from its cached envelope, so equal
    // notes differ by rounding; the tolerance keeps them equal.
    // A victim that is the layer of a lazily pending launch has not been
    // written to the pool yet: its row still holds the previous occupant, so
    // its score comes from the pending setup (age 0, same expression as above).
    float victimScore = 0.0f;
    bool pendingVictim = false;
    if (pendingLaunch_.valid) {
        const PendingLaunch& p = pendingLaunch_;
        for (uint32_t i = 0u; i < p.count && !pendingVictim; ++i) {
            if (p.handles[i] != victim) continue;
            victimScore = p.scores[i];
            pendingVictim = true;
        }
    }
    if (!pendingVictim) victimScore = ComputeStealScore(victim);
    if (stealAgeScale_ == 0.0f)
        return newbornScore >= victimScore * (1.0f + 1e-5f);
    return newbornScore > victimScore;
}

inline void VoiceManager::InsertPreselectedVictim(VoiceHandle victim) {
    // The sequential path builds the volatile heap before any selection that
    // can observe volatile candidates (tier c inside PopStealCandidate), so
    // the re-arm must guarantee the same precondition before re-linking a
    // volatile victim; otherwise a later probe's eligibility check would see
    // a stale heap and take a different launch path than the sequential one.
    if (!stealVolatileHeapValid_ || stealVolatileHeapFrame_ != currentFrame_)
        BuildVolatileStealHeap();
    if (IsStableStealCandidate(victim)) {
        stealWinnerTree_[stealTreeLeafBase_ + victim] =
            stealStableKey_[victim];
        ++stealHeapCount_;
        RefreshStealWinnerPath(victim);
    } else {
        LinkVolatileCandidate(victim);
    }
}

// Re-inserts every batch-popped victim of one launch transaction that was
// never consumed (fed to a layer) and is still live. Victims retired as
// play-group siblings of an earlier layer's victim are Free and are skipped;
// consumed victims were replaced in place and are skipped via `consumed`.
// `consumedByForcedSteal` marks the keyed-cap victim the caller consumed
// through a forced steal — the handle is live again but holds the launch's
// own replacement, so it must never be re-armed as a stealable.
inline void VoiceManager::RearmLiveBatchVictims(const VoiceHandle* victims,
                                                uint32_t popped,
                                                const bool* consumed,
                                                VoiceHandle consumedByForcedSteal) {
    if (!victims || !consumed) return;
    // Re-armed victims go back to the head of their buckets, in reverse pop
    // order, so the first popped victim is first again, exactly where
    // one-at-a-time pops left it.
    bucketFront_ = bucketSteal_;
    struct FrontRestore {
        bool& flag;
        ~FrontRestore() { flag = false; }
    } frontRestore{bucketFront_};
    for (uint32_t n = 0u; n < popped; ++n) {
        const uint32_t i = bucketSteal_ ? popped - 1u - n : n;
        const VoiceHandle victim = victims[i];
        if (victim == kInvalidVoice || consumed[i] ||
            victim == consumedByForcedSteal ||
            victim >= maxVoices_ ||
            v.state[victim] != static_cast<uint8_t>(VoiceState::Active))
            continue;
        InsertPreselectedVictim(victim);
    }
}

inline void VoiceManager::PushStealCandidate(VoiceHandle handle,
                                              uint32_t activePosition) {
    if (!stealHeapValid_ || handle >= maxVoices_) return;
    if (!IsLinkedActiveVoice(handle)) return;  // unlinked: not indexable yet
    if (!IsStableStealCandidate(handle)) {
        LinkVolatileCandidate(handle);
        return;
    }
    ++stealHeapCount_;
    const float score = ComputeStableStealKey(handle);
    stealStableKey_[handle] = EncodeStableWinnerKey(
        score, activePosition);
    stealWinnerTree_[stealTreeLeafBase_ + handle] = stealStableKey_[handle];
    RefreshStealWinnerPath(handle);
}

inline void VoiceManager::UpdateStealCandidate(VoiceHandle handle) {
    if (!stealHeapValid_ || handle >= maxVoices_ ||
        stealCandidateDeferred_[handle] != 0u) return;

    const uint32_t leaf = stealTreeLeafBase_ + handle;
    const bool indexedStable = stealWinnerTree_[leaf] != 0u;
    const bool indexedVolatile =
        stealVolatilePosition_[handle] < stealVolatileCount_;
    const bool alive =
        v.state[handle] != static_cast<uint8_t>(VoiceState::Free);
    // A key embeds the active-list position, so only a linked voice may hold
    // one. An Active-but-unlinked voice (launch-transaction interior, fence
    // kill, or live voice-limit reduction) must leave the index until it is
    // linked again; writing its key would encode position UINT32_MAX.
    const bool linked = IsLinkedActiveVoice(handle);
    const bool wantsStable = alive && linked && IsStableStealCandidate(handle);

    if (wantsStable) {
        if (indexedVolatile) UnlinkVolatileCandidate(handle);
        if (!indexedStable) ++stealHeapCount_;
        const float score = ComputeStableStealKey(handle);
        stealStableKey_[handle] = EncodeStableWinnerKey(
            score, activePosition_[handle]);
        stealWinnerTree_[leaf] = stealStableKey_[handle];
        // Updating an existing leaf used to clear and repair this path, then
        // insert and repair it a second time. One repair is exact because no
        // victim selection can observe the intermediate key on the audio
        // thread.
        RefreshStealWinnerPath(handle);
        return;
    }

    if (indexedStable) {
        stealWinnerTree_[leaf] = 0u;
        assert(stealHeapCount_ > 0u);
        --stealHeapCount_;
        RefreshStealWinnerPath(handle);
    }
    if (!alive || !linked) {
        if (indexedVolatile) UnlinkVolatileCandidate(handle);
        return;
    }
    if (!indexedVolatile) {
        LinkVolatileCandidate(handle);
        return;
    }

    if (bucketSteal_) {
        if (stealVolatileHeapValid_ &&
            stealVolatileHeapFrame_ == currentFrame_ && VolHeapHas(handle))
            VolBucketSet(handle);
        return;
    }
    // A volatile candidate can change gain or active-position without
    // changing class. Refresh its current-frame heap entry in place instead
    // of unlinking it from both the heap and compact candidate list.
    if (stealVolatileHeapValid_ &&
        stealVolatileHeapFrame_ == currentFrame_) {
        const uint32_t heapPosition = stealVolatileHeapPosition_[handle];
        if (heapPosition < stealVolatileHeapCount_) {
            stealVolatileHeapKey_[heapPosition] = EncodeStableWinnerKey(
                ComputeStableStealKey(handle), activePosition_[handle]);
            VolatileHeapSiftUp(heapPosition);
            VolatileHeapSiftDown(stealVolatileHeapPosition_[handle]);
        }
    }
}

inline bool VoiceManager::IsStableStealCandidate(VoiceHandle handle) const {
    if (handle >= maxVoices_ ||
        v.state[handle] != static_cast<uint8_t>(VoiceState::Active)) {
        return false;
    }
    // ComputeEffectiveStealLevel uses targetGain throughout delay, hold and
    // attack, so after removing the shared current-frame age term their key is
    // constant just like sustain. Decay is the only active stage whose
    // effective level changes every rendered frame.
    return v.envelopeStage[handle] != 2u;
}

// A steal-index key embeds the voice's active-list position, so a key may
// only exist for a voice that is linked into that list. Writing a key for an
// unlinked voice (activePosition_ == UINT32_MAX inside a launch transaction,
// after a fence kill, or during a live voice-limit reduction) encodes
// position UINT32_MAX, which later reaches PopStealCandidate as a
// winner-tree root and faults at activeList_[UINT32_MAX] in release builds
// (the invariant asserts there are compiled out).
SVMS_VM_FORCEINLINE bool VoiceManager::IsLinkedActiveVoice(
    VoiceHandle handle) const {
    return handle < maxVoices_ &&
        activePosition_[handle] < activeCount_ &&
        activeList_[activePosition_[handle]] == handle;
}

inline float VoiceManager::ComputeStableStealKey(VoiceHandle handle) const {
    const float commonAgeScore =
        static_cast<float>(currentFrame_) * stealAgeScale_;
    return ComputeStealScore(handle) - commonAgeScore;
}

inline void VoiceManager::LinkVolatileCandidate(VoiceHandle handle) {
    if (handle >= maxVoices_ ||
        stealVolatilePosition_[handle] < stealVolatileCount_) return;
    const uint32_t position = stealVolatileCount_++;
    stealVolatileList_[position] = handle;
    stealVolatilePosition_[handle] = position;
    if (stealVolatileHeapValid_) {
        if (bucketSteal_) {
            VolBucketSet(handle);
            return;
        }
        const uint32_t heapPosition = stealVolatileHeapCount_++;
        stealVolatileHeapKey_[heapPosition] = EncodeStableWinnerKey(
            ComputeStableStealKey(handle), activePosition_[handle]);
        stealVolatileHeapHandle_[heapPosition] = handle;
        stealVolatileHeapPosition_[handle] = heapPosition;
        VolatileHeapSiftUp(heapPosition);
    }
}

inline void VoiceManager::UnlinkVolatileCandidate(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    RemoveVolatileHeapCandidate(handle);
    const uint32_t position = stealVolatilePosition_[handle];
    if (position >= stealVolatileCount_) return;
    const uint32_t last = --stealVolatileCount_;
    if (position != last) {
        const uint32_t moved = stealVolatileList_[last];
        stealVolatileList_[position] = moved;
        stealVolatilePosition_[moved] = position;
    }
    stealVolatilePosition_[handle] = UINT32_MAX;
}

inline void VoiceManager::VolatileHeapSwap(uint32_t a, uint32_t b) {
    const uint64_t temporaryKey = stealVolatileHeapKey_[a];
    stealVolatileHeapKey_[a] = stealVolatileHeapKey_[b];
    stealVolatileHeapKey_[b] = temporaryKey;
    const uint32_t temporaryHandle = stealVolatileHeapHandle_[a];
    stealVolatileHeapHandle_[a] = stealVolatileHeapHandle_[b];
    stealVolatileHeapHandle_[b] = temporaryHandle;
    stealVolatileHeapPosition_[stealVolatileHeapHandle_[a]] = a;
    stealVolatileHeapPosition_[stealVolatileHeapHandle_[b]] = b;
}

inline void VoiceManager::VolatileHeapSiftUp(uint32_t position) {
    while (position > 0u) {
        const uint32_t parent = (position - 1u) >> 1u;
        if (stealVolatileHeapKey_[position] <=
            stealVolatileHeapKey_[parent]) break;
        VolatileHeapSwap(position, parent);
        position = parent;
    }
}

inline void VoiceManager::VolatileHeapSiftDown(uint32_t position) {
    for (;;) {
        const uint32_t left = position * 2u + 1u;
        if (left >= stealVolatileHeapCount_) break;
        const uint32_t right = left + 1u;
        uint32_t best = left;
        if (right < stealVolatileHeapCount_ &&
            stealVolatileHeapKey_[right] > stealVolatileHeapKey_[left])
            best = right;
        if (stealVolatileHeapKey_[best] <=
            stealVolatileHeapKey_[position]) break;
        VolatileHeapSwap(position, best);
        position = best;
    }
}

inline void VoiceManager::BuildVolatileStealHeap() {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    // Sampled rdtsc pair: measures this rebuild without distorting every call.
    const bool profileThisBuild =
        (++volatileHeapProfileCounter_ & 15u) == 0u;
    const uint64_t profileBegin = profileThisBuild ? __rdtsc() : 0u;
#endif
    // The linked volatile set and the previous heap contain the same handles;
    // removals update both structures immediately. Every surviving handle's
    // inverse position is overwritten below, so clearing the old heap first
    // was a redundant second pass over the hottest dense-stealing population.
    stealVolatileHeapCount_ = 0u;
    bool vectorized = false;
#if !defined(SVMS_XP_COMPAT)
    if (stealKeyBackend_ == RenderBackend::AVX2 && !priorityPolicy_ &&
        stealVolatileCount_ >= 8u) {
        constexpr uint32_t kParallelMinCandidates = 32768u;
        constexpr uint32_t kParallelChunk = 16384u;
        if (parallelFor_ != nullptr && stealVolatileCount_ >= kParallelMinCandidates &&
            currentFrame_ <= static_cast<uint64_t>(INT32_MAX)) {
            // The key build is one independent gather-heavy pass per candidate
            // (cache-miss bound on a big pool): split it over the worker pool.
            struct Build {
                VoiceManager* self;
                uint32_t count;
                uint32_t chunk;
                std::atomic<uint32_t> failed;
            } build{this, stealVolatileCount_, kParallelChunk, {0u}};
            const uint32_t jobs = (stealVolatileCount_ + kParallelChunk - 1u) / kParallelChunk;
            parallelFor_(parallelForUser_, jobs,
                [](void* context, uint32_t index) {
                    Build& b = *static_cast<Build*>(context);
                    VoiceManager& m = *b.self;
                    const uint32_t begin = index * b.chunk;
                    const uint32_t count = (std::min)(b.chunk, b.count - begin);
                    if (!BuildVolatileStealKeysAVX2(
                            m.stealVolatileList_ + begin, count, m.v.birthFrame,
                            m.v.currentGain, m.v.stealOutputGain, m.activePosition_,
                            m.currentFrame_, kBassMidiStealGainScale,
                            m.stealAgeScale_, m.stealVolatileHeapKey_ + begin,
                            m.stealVolatileHeapHandle_ + begin,
                            m.stealVolatileHeapPosition_, begin)) {
                        b.failed.store(1u, std::memory_order_relaxed);
                    }
                }, &build);
            vectorized = build.failed.load(std::memory_order_relaxed) == 0u;
        }
        if (!vectorized)
        vectorized = BuildVolatileStealKeysAVX2(
            stealVolatileList_, stealVolatileCount_, v.birthFrame,
            v.currentGain, v.stealOutputGain, activePosition_, currentFrame_,
            kBassMidiStealGainScale, stealAgeScale_, stealVolatileHeapKey_,
            stealVolatileHeapHandle_, stealVolatileHeapPosition_);
    }
#endif
    if (!vectorized) {
        const float commonAgeScore =
            static_cast<float>(currentFrame_) * stealAgeScale_;
        for (uint32_t position = 0; position < stealVolatileCount_; ++position) {
            const uint32_t handle = stealVolatileList_[position];
#if defined(SVMS_VM_HAS_PREFETCH)
            // Linear scan over stealVolatileList_: the next handle's voice
            // data is knowable now. Prefetch the SoA lines the body reads.
            if (position + 1u < stealVolatileCount_) {
                const uint32_t next = stealVolatileList_[position + 1u];
                _mm_prefetch(reinterpret_cast<const char*>(&v.birthFrame[next]), _MM_HINT_T0);
                _mm_prefetch(reinterpret_cast<const char*>(&v.currentGain[next]), _MM_HINT_T0);
                _mm_prefetch(reinterpret_cast<const char*>(&v.stealOutputGain[next]), _MM_HINT_T0);
                _mm_prefetch(reinterpret_cast<const char*>(&activePosition_[next]), _MM_HINT_T0);
            }
#endif
            // Volatile means active decay or releasing, so its effective
            // level is always currentGain * outputGain. Keep the original
            // arithmetic order so ties and floating-point rounding remain
            // unchanged.
            const uint64_t rawAge = currentFrame_ > v.birthFrame[handle]
                ? currentFrame_ - v.birthFrame[handle] : 0u;
            const uint32_t age = rawAge > UINT32_MAX
                ? UINT32_MAX : static_cast<uint32_t>(rawAge);
            const float ageUnits =
                static_cast<float>(age) * stealAgeScale_;
            float effectiveLevel = std::fabs(v.currentGain[handle]) *
                v.stealOutputGain[handle];
            if (priorityPolicy_) effectiveLevel *= StealLaneFactor(handle);
            const float score = ageUnits -
                effectiveLevel * kBassMidiStealGainScale;
            stealVolatileHeapKey_[position] = EncodeStableWinnerKey(
                score - commonAgeScore, activePosition_[handle]);
            stealVolatileHeapHandle_[position] = handle;
            stealVolatileHeapPosition_[handle] = position;
        }
    }
    stealVolatileHeapCount_ = stealVolatileCount_;
    if (bucketSteal_) {
        volBuckets_.Clear();
        for (uint32_t position = 0u; position < stealVolatileHeapCount_;
             ++position) {
            const uint32_t handle = stealVolatileHeapHandle_[position];
            volKey_[handle] = stealVolatileHeapKey_[position];
            volBuckets_.PushUnchecked(handle, volKey_[handle]);
        }
    } else if (stealVolatileHeapCount_ > 1u) {
        for (uint32_t position = stealVolatileHeapCount_ / 2u;
             position-- > 0u;) VolatileHeapSiftDown(position);
    }
    stealVolatileHeapFrame_ = currentFrame_;
    stealVolatileHeapValid_ = true;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    if (profileThisBuild) {
        ++volatileHeapProfileBuilds_;
        ++volatileHeapProfileSamples_;
        volatileHeapProfileCycles_ += __rdtsc() - profileBegin;
        volatileHeapProfileCandidates_ += stealVolatileHeapCount_;
    }
#endif
}

inline void VoiceManager::RemoveVolatileHeapCandidate(VoiceHandle handle) {
    if (bucketSteal_) {
        if (handle >= maxVoices_ || volKey_[handle] == 0u) return;
        volKey_[handle] = 0u;
        volBuckets_.DropIfHead(handle);
        if (stealVolatileHeapValid_ && stealVolatileHeapCount_ != 0u)
            --stealVolatileHeapCount_;
        return;
    }
    if (!stealVolatileHeapValid_ || handle >= maxVoices_) return;
    const uint32_t position = stealVolatileHeapPosition_[handle];
    if (position >= stealVolatileHeapCount_) return;
    const uint32_t last = --stealVolatileHeapCount_;
    stealVolatileHeapPosition_[handle] = UINT32_MAX;
    if (position != last) {
        stealVolatileHeapKey_[position] = stealVolatileHeapKey_[last];
        const uint32_t movedHandle = stealVolatileHeapHandle_[last];
        stealVolatileHeapHandle_[position] = movedHandle;
        stealVolatileHeapPosition_[movedHandle] = position;
        VolatileHeapSiftUp(position);
        const uint32_t adjusted = stealVolatileHeapPosition_[movedHandle];
        VolatileHeapSiftDown(adjusted);
    }
}

inline void VoiceManager::RemoveReservedVolatileRoot(VoiceHandle handle) {
    if (bucketSteal_) {
        UnlinkVolatileCandidate(handle);
        return;
    }
    assert(handle < maxVoices_ && stealVolatileHeapValid_ &&
           stealVolatileHeapCount_ != 0u &&
           stealVolatileHeapHandle_[0] == handle &&
           stealVolatileHeapPosition_[handle] == 0u);

    const uint32_t lastHeap = --stealVolatileHeapCount_;
    stealVolatileHeapPosition_[handle] = UINT32_MAX;
    if (lastHeap != 0u) {
        stealVolatileHeapKey_[0] = stealVolatileHeapKey_[lastHeap];
        const uint32_t moved = stealVolatileHeapHandle_[lastHeap];
        stealVolatileHeapHandle_[0] = moved;
        stealVolatileHeapPosition_[moved] = 0u;
        VolatileHeapSiftDown(0u);
    }

    const uint32_t listPosition = stealVolatilePosition_[handle];
    assert(listPosition < stealVolatileCount_);
    const uint32_t lastList = --stealVolatileCount_;
    if (listPosition != lastList) {
        const uint32_t moved = stealVolatileList_[lastList];
        stealVolatileList_[listPosition] = moved;
        stealVolatilePosition_[moved] = listPosition;
    }
    stealVolatilePosition_[handle] = UINT32_MAX;
}

inline void VoiceManager::RemoveStealCandidate(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    UnlinkVolatileCandidate(handle);
    const uint32_t leaf = stealTreeLeafBase_ + handle;
    if (stealWinnerTree_[leaf] == 0u) return;
    stealWinnerTree_[leaf] = 0u;
    assert(stealHeapCount_ > 0u);
    --stealHeapCount_;
    RefreshStealWinnerPath(handle);
}

// Consume (or peek) the best Releasing entry from the ring fast path.
// Ring contents are hints: a voice may have been retired, re-stolen
// through the normal tiers, or become a deferred replacement after its
// entry was pushed. Stale entries are dropped (oldest-first) without
// repairing the rest of the ring mid-search; live pool state is the only
// source of truth. Instead of popping the head unconditionally (pure FIFO),
// a bounded 12-entry window starting at the head is scanned and the single
// best victim is selected with ComputeStableStealKey, compared exactly like
// the winner-tree root (largest encoded key wins), so quality ranking
// survives the O(1)-ish fast path. With consume=false nothing is mutated on
// success, so callers can predict the fast-path outcome before committing
// to it. If no valid entry exists in the entire ring, kInvalidVoice is
// returned and callers fall through to the exact winner-tree path.
inline VoiceHandle VoiceManager::NextValidReleasingRingVictim(
    uint32_t& victimPosition, const bool consume) {
    if (releasingRingCapacity_ == 0u) return kInvalidVoice;
    const auto releasingState = static_cast<uint8_t>(VoiceState::Releasing);

    // Bounded quality window: up to 12 entries from the head, non-mutating.
    VoiceHandle bestHandle = kInvalidVoice;
    uint32_t bestPosition = 0u;
    uint64_t bestKey = 0u;
    uint32_t bestSlot = 0u;
    // A rejected note-on mutates nothing, so the next one scans the identical
    // window.  Reuse the previous winner while the fingerprint below (every
    // mutator bumps stealPeekEpoch_) still matches, after confirming the
    // winner itself is still the live Releasing voice at that position.
    RingPeekCache& cache = ringPeekCache_;
    bool cacheHit = false;
    if (cache.valid && cache.epoch == stealPeekEpoch_ &&
        cache.frame == currentFrame_ && cache.head == releasingRingHead_ &&
        cache.count == releasingRingCount_ &&
        cache.activeCount == activeCount_ && cache.freeTop == freeTop_ &&
        cache.ageScale == stealAgeScale_ && cache.handle < maxVoices_ &&
        v.state[cache.handle] == releasingState &&
        activePosition_[cache.handle] == cache.position) {
        bestHandle = static_cast<VoiceHandle>(cache.handle);
        bestPosition = cache.position;
        bestSlot = cache.slot;
        cacheHit = true;
    }
    if (!cacheHit) {
        const uint32_t window =
            releasingRingCount_ < 12u ? releasingRingCount_ : 12u;
        for (uint32_t i = 0u; i < window; ++i) {
            const uint32_t slot =
                (releasingRingHead_ + i) & releasingRingMask_;
            const uint32_t handle = releasingRing_[slot];
            if (handle >= maxVoices_) continue; // stale hint
            const uint32_t position = activePosition_[handle];
            const bool valid = handle < maxVoices_ &&
                v.state[handle] == releasingState &&
                position < activeCount_ && activeList_[position] == handle &&
                stealCandidateDeferred_[handle] == 0u &&
                stealCandidateReserved_[handle] == 0u;
            if (!valid) continue; // stale: skipped, dropped below if at head
            // Same comparison direction as the winner-tree combine and the
            // volatile fallback scan: largest encoded key is the victim.
            const uint64_t key = EncodeStableWinnerKey(
                ComputeStableStealKey(static_cast<VoiceHandle>(handle)),
                position);
            if (bestHandle == kInvalidVoice || key > bestKey) {
                bestHandle = static_cast<VoiceHandle>(handle);
                bestPosition = position;
                bestKey = key;
                bestSlot = slot;
            }
        }
    }
    if (bestHandle != kInvalidVoice) {
        if (!cacheHit) {
            cache.valid = true;
            cache.epoch = stealPeekEpoch_;
            cache.frame = currentFrame_;
            cache.head = releasingRingHead_;
            cache.count = releasingRingCount_;
            cache.activeCount = activeCount_;
            cache.freeTop = freeTop_;
            cache.ageScale = stealAgeScale_;
            cache.handle = bestHandle;
            cache.position = bestPosition;
            cache.slot = bestSlot;
        }
        if (!consume) {
            victimPosition = bestPosition;
            return bestHandle;
        }
        // Remove the winner: if it is not at the head, swap it with the
        // head entry (avoids shifting the ring), then pop the head.
        if (bestSlot != releasingRingHead_) {
            releasingRing_[bestSlot] = releasingRing_[releasingRingHead_];
        }
        releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
        --releasingRingCount_;
        victimPosition = bestPosition;
        // Stale-drop: drain any stale entries the swap/win left at the head
        // (mirrors the old search-time stale-drop; never drops a live voice).
        while (releasingRingCount_ != 0u) {
            const uint32_t head = releasingRing_[releasingRingHead_];
            if (head < maxVoices_) {
                const uint32_t position = activePosition_[head];
                const bool valid = v.state[head] == releasingState &&
                    position < activeCount_ &&
                    activeList_[position] == head &&
                    stealCandidateDeferred_[head] == 0u &&
                    stealCandidateReserved_[head] == 0u;
                if (valid) break;
            }
            releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
            --releasingRingCount_;
        }
        return bestHandle;
    }

    // Window (and possibly ring beyond it) holds no quality candidate:
    // fall back to the original oldest-first drain so the caller falls
    // through to the winner-tree path when the ring is fully stale.
    while (releasingRingCount_ != 0u) {
        const uint32_t handle = releasingRing_[releasingRingHead_];
        if (handle >= maxVoices_) {
            // Stale entry: drop it and keep scanning.
            releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
            --releasingRingCount_;
            continue;
        }
        const uint32_t position = activePosition_[handle];
        const bool valid = handle < maxVoices_ &&
            v.state[handle] == releasingState &&
            position < activeCount_ && activeList_[position] == handle &&
            stealCandidateDeferred_[handle] == 0u &&
            stealCandidateReserved_[handle] == 0u;
        if (valid) {
            if (!consume) return static_cast<VoiceHandle>(handle);
            releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
            --releasingRingCount_;
            victimPosition = position;
            return static_cast<VoiceHandle>(handle);
        }
        // Stale entry: drop it and keep scanning.
        releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
        --releasingRingCount_;
    }
    return kInvalidVoice;
}

inline bool VoiceManager::ReleasingRingEligible() const {
    return enableReleasingRing_ && freeTop_ == 0u &&
           releasingRingCapacity_ != 0u && releasingRingCount_ != 0u &&
           activeCount_ * 20u >= voiceLimit_ * 17u;
}

// Unit-test oracle mirroring AllocateVoiceOrSteal's selection stage.
inline VoiceHandle VoiceManager::PredictStealVictimForTest(
    uint32_t& bestPos) {
    if (ReleasingRingEligible()) {
        const VoiceHandle ringVictim =
            NextValidReleasingRingVictim(bestPos, false);
        if (ringVictim != kInvalidVoice) return ringVictim;
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    return FindStealVictimExhaustiveForTest();
#else
    return kInvalidVoice;
#endif
}
