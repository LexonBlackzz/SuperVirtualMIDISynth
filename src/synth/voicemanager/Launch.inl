// Part of VoiceManager.h; included from there inside namespace svms. Not standalone.

inline bool VoiceManager::SetPhaseRotationMode(uint32_t mode) {
    if (mode > 4u) mode = 0u;
    if (mode == phaseRotationMode_) return true;
    phaseRotationMode_ = mode;
    if (mode == 0u) {
        // Coherent: free the state entirely — every render path then takes
        // its original bit-exact code path (v.rot == nullptr).
        v.ReleaseRotation();
        return true;
    }
    if (!v.ReserveRotation(maxVoices_)) {
        phaseRotationMode_ = 0u;
        return false;
    }
    // Seed every currently live voice so a mid-playback mode switch never
    // produces unrotated (or zero-state) filter output.
    for (uint32_t position = 0u; position < activeCount_; ++position)
        SeedVoiceRotationForVoice(activeList_[position]);
    return true;
}

inline void VoiceManager::SeedVoiceRotationForVoice(VoiceHandle handle) {
    if (phaseRotationMode_ == 0u || v.rot == nullptr ||
        handle >= maxVoices_) {
        return;
    }
    const uint64_t seed = MakeVoiceRotationSeed(
        v.channel[handle], v.note[handle], handle, v.birthFrame[handle],
        ++rotationSeedCounter_);
    SeedVoiceRotation(v.rot[handle], phaseRotationMode_, seed,
                      static_cast<float>(sampleRate_));
}

inline VoiceHandle VoiceManager::AllocateVoice(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (voiceLimit_ == 0u || activeCount_ >= voiceLimit_ || freeTop_ == 0u)
        return kInvalidVoice;
    // Pending CC levels must land before any steal score or configure step
    // can observe premultiplied gains (no-op when nothing is marked). This is
    // the steal-only fold: renderGain rewrites stay with the vintage-exact
    // batch-boundary folds.
    FoldStaleChannelMixGainsForSteal();
    uint32_t idx = freeStack_[--freeTop_];

    InitializeVoice(static_cast<VoiceHandle>(idx), channel, note, velocity);
    SeedVoiceRotationForVoice(static_cast<VoiceHandle>(idx));
    LinkChannelKey(static_cast<VoiceHandle>(idx));
    LinkChannelActive(static_cast<VoiceHandle>(idx));
    LinkRenderClass(static_cast<VoiceHandle>(idx));

    activeList_[activeCount_] = idx;
    activePosition_[idx] = activeCount_;
    activeCount_++;
    if (stealHeapValid_)
        PushStealCandidate(static_cast<VoiceHandle>(idx),
                           activePosition_[idx]);

    return static_cast<VoiceHandle>(idx);
}

inline VoiceHandle VoiceManager::AllocateVoiceOrSteal(uint8_t channel, uint8_t note,
                                                        uint8_t velocity,
                                                        bool* outStolen,
                                                        bool deferCandidate,
                                                        bool reserveCandidateInPlace,
                                                        VoiceHandle preselectedVictim,
                                                        uint32_t preselectedVictimPosition,
                                                        VoiceHandle forcedVictim) {
    ++stealPeekEpoch_;
    // While a lowered cap is still draining its forced-release tails, do not
    // admit replacement notes that would turn those tails back into primaries.
    // The transition is bounded by the forced release (normally 50 ms).
    if (voiceLimit_ == 0u || activeCount_ > voiceLimit_) {
        if (outStolen) *outStolen = false;
        return kInvalidVoice;
    }
    // Pending CC levels must land before victim selection so a launch after a
    // same-batch CC scores victims exactly like the eager per-CC refresh did.
    // Steal-only fold — see FoldStaleChannelMixGainsForSteal.
    FoldStaleChannelMixGainsForSteal();

    VoiceHandle vh = kInvalidVoice;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t freeLifecycleBegin = BeginLaunchStageForTest();
#endif
    // A forced victim (per-key voice cap) must take the steal path: the key
    // is already at its cap, so a fresh slot would grow the pileup instead of
    // replacing the oldest key member. Skip both free-slot admission and the
    // AllocateVoice fallback while one is pending.
    if (forcedVictim == kInvalidVoice) {
        if (deferCandidate && freeTop_ != 0u && activeCount_ < voiceLimit_) {
            const uint32_t idx = static_cast<uint32_t>(freeStack_[--freeTop_]);
            vh = static_cast<VoiceHandle>(idx);
            PrefetchVoiceLines(vh);
            InitializePreparedVoice(vh, channel, note, velocity);
            SeedVoiceRotationForVoice(vh);
            LinkChannelKey(vh);
            LinkChannelActive(vh);
            activeList_[activeCount_] = idx;
            activePosition_[idx] = activeCount_++;
            // A deferred voice is invisible to the steal index until its
            // configuration transaction commits. Keep the existing heap valid.
        } else {
            vh = AllocateVoice(channel, note, velocity);
        }
    }
    if (vh != kInvalidVoice) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::Lifecycle,
                              freeLifecycleBegin);
        if (launchTestContext_.active)
            ++launchChurnStats_.freeSlotAllocations;
#endif
        stealCandidateDeferred_[vh] = deferCandidate ? 1u : 0u;
        if (outStolen) *outStolen = false;
        return vh;
    }

    // Physical pool or logical live cap is full — find the lowest-priority
    // voice to steal. Replacement happens in-place, so the live cap cannot
    // grow past its configured value.
    uint32_t bestPos = preselectedVictimPosition;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t selectionBegin = BeginLaunchStageForTest();
#endif
    // A preselected victim (batched per-launch layer allocation) was produced
    // by the caller's PopStealCandidates call — the exact reserve=false pop
    // sequence this site would perform — with full ring/tree/heap repair, so
    // only the redundant re-selection call is skipped here; the victim is
    // identical and the post-selection logic below is untouched.
    VoiceHandle bestHandle = kInvalidVoice;
    if (forcedVictim != kInvalidVoice) {
        // Per-key cap victim chosen by the caller (oldest chain member of a
        // key already at its cap). Unlike a popped candidate it is still in
        // the steal index, so it is dropped from the index below; ring
        // entries are self-invalidating hints and need no repair.
        bestHandle = forcedVictim;
    } else {
        bestHandle = preselectedVictim != kInvalidVoice
            ? preselectedVictim
            : PopStealCandidate(bestPos,
                                deferCandidate && reserveCandidateInPlace);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::VictimSelection,
                          selectionBegin);
#endif
    if (bestHandle == kInvalidVoice) return kInvalidVoice;
    // The victim's SoA lines are cold; fetch them while the sibling-group
    // retirement, tail capture, and index repair below still have work to
    // run before the first field write.
    PrefetchVoiceLines(bestHandle);
    // The pop supplies this for a fresh selection; a preselected or forced
    // victim derives it from the active list.
    bestPos = activePosition_[bestHandle];
    const uint32_t bestIdx = bestHandle;
    if (forcedVictim != kInvalidVoice) {
        // The forced victim was never popped: remove it from the index here.
        // A reserved-root flag travels with it — the slot is being consumed
        // by this replacement, not handed to a pending layer.
        stealCandidateReserved_[bestIdx] = 0u;
        RemoveStealCandidate(bestHandle);
    }

    // Every SF2 region produced by one MIDI note-on shares playIndex. Stereo
    // SoundFonts commonly use one left and one right region; stealing just one
    // side collapses the note into the opposite ear. Retire the whole physical
    // voice group atomically, leaving the selected slot for this replacement
    // and pushing its siblings onto the normal free stack for following layers.
    const uint32_t victimPlayIndex = v.playIndex[bestIdx];
    const bool hasSiblings = victimPlayIndex != UINT32_MAX &&
        (playGroupPrev_[bestIdx] >= 0 || playGroupNext_[bestIdx] >= 0);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    RecordVictimGroupForTest(bestHandle, LaunchVictimPath::General, false);
#endif
    if (hasSiblings) {
        // A reserved volatile root was intentionally left in the index for a
        // single deferred replacement. Group retirement changes its active
        // position, so remove it now and insert the configured replacement at
        // commit instead of invalidating and rebuilding the complete heap.
        if (stealCandidateReserved_[bestIdx] != 0u) {
            stealCandidateReserved_[bestIdx] = 0u;
            RemoveStealCandidate(static_cast<VoiceHandle>(bestIdx));
        }
        int32_t linked = playGroupPrev_[bestIdx];
        while (linked >= 0) {
            const VoiceHandle sibling = static_cast<VoiceHandle>(linked);
            linked = playGroupPrev_[sibling];
            RetireStolenSibling(sibling, bestHandle);
        }
        linked = playGroupNext_[bestIdx];
        while (linked >= 0) {
            const VoiceHandle sibling = static_cast<VoiceHandle>(linked);
            linked = playGroupNext_[sibling];
            RetireStolenSibling(sibling, bestHandle);
        }
        bestPos = activePosition_[bestIdx];
    }
    CaptureStealTail(bestHandle);

    // Retire the victim.
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t lifecycleBegin = BeginLaunchStageForTest();
#endif
    ++stealCount_;
    if (v.state[bestIdx] == static_cast<uint8_t>(VoiceState::Releasing))
        releasingCount_.fetch_sub(1u, std::memory_order_relaxed);
    UnlinkChannelKey(static_cast<VoiceHandle>(bestIdx));
    UnlinkExclusiveClass(static_cast<VoiceHandle>(bestIdx));
    UnlinkPlayGroup(static_cast<VoiceHandle>(bestIdx));
    UnlinkChannelActive(static_cast<VoiceHandle>(bestIdx));
    UnlinkRenderClass(static_cast<VoiceHandle>(bestIdx));
    v.state[bestIdx] = static_cast<uint8_t>(VoiceState::Free);

    // Reinitialize in-place
    if (deferCandidate)
        InitializePreparedVoice(static_cast<VoiceHandle>(bestIdx), channel,
                                note, velocity);
    else
        InitializeVoice(static_cast<VoiceHandle>(bestIdx), channel, note, velocity);
    stealCandidateDeferred_[bestIdx] = deferCandidate ? 1u : 0u;
    // The outgoing victim needs an anti-click tail. The replacement is a
    // legitimate new attack and must start at its SF2 envelope level; fading
    // every replacement in smears dense streams once stealing becomes steady.
    v.stealFadeInFramesRemaining[bestIdx] = 0;
    v.stealFadeInFramesTotal[bestIdx] = 0;
    LinkChannelKey(static_cast<VoiceHandle>(bestIdx));
    LinkChannelActive(static_cast<VoiceHandle>(bestIdx));
    if (!deferCandidate)
        LinkRenderClass(static_cast<VoiceHandle>(bestIdx));
    activeList_[bestPos] = bestIdx;  // reuse the victim's position
    activePosition_[bestIdx] = bestPos;
    if (!deferCandidate) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::Lifecycle,
                              lifecycleBegin);
        const uint64_t treeBegin = BeginLaunchStageForTest();
#endif
        PushStealCandidate(static_cast<VoiceHandle>(bestIdx), bestPos);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance,
                              treeBegin);
#endif
    } else {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::Lifecycle,
                              lifecycleBegin);
#endif
    }

    if (outStolen) *outStolen = true;
    return static_cast<VoiceHandle>(bestIdx);
}

inline void VoiceManager::CommitVoiceConfiguration(VoiceHandle handle) {
    ++stealPeekEpoch_;
    if (handle >= maxVoices_ || stealCandidateDeferred_[handle] == 0u) return;
    stealCandidateDeferred_[handle] = 0u;
    if (stealCandidateReserved_[handle] != 0u) {
        const uint8_t reservation = stealCandidateReserved_[handle];
        stealCandidateReserved_[handle] = 0u;
        if (reservation == 2u) {
            // The refresh path sifts the winner tree, which only exists
            // under Quality with a valid index. A policy switch that
            // invalidated the index between the launch transaction's
            // reservation and this commit must fall through to the
            // guarded removal/re-push below.
            if (stealHeapValid_ &&
                IsStableStealCandidate(handle) && IsLinkedActiveVoice(handle)) {
                const float score = ComputeStableStealKey(handle);
                stealStableKey_[handle] = EncodeStableWinnerKey(
                    score, activePosition_[handle]);
                stealWinnerTree_[stealTreeLeafBase_ + handle] =
                    stealStableKey_[handle];
                RefreshStealWinnerPath(handle);
            } else {
                RemoveStealCandidate(handle);
                if (stealHeapValid_)
                    PushStealCandidate(handle, activePosition_[handle]);
            }
            return;
        }
        if (bucketSteal_ && !IsStableStealCandidate(handle) &&
            IsLinkedActiveVoice(handle) && stealVolatileHeapValid_ &&
            VolHeapHas(handle) && stealVolatileHeapFrame_ == currentFrame_) {
            VolBucketSet(handle);
            return;
        }
        const uint32_t heapPosition = bucketSteal_
            ? UINT32_MAX : stealVolatileHeapPosition_[handle];
        if (!IsStableStealCandidate(handle) && IsLinkedActiveVoice(handle) &&
            heapPosition < stealVolatileHeapCount_ &&
            stealVolatileHeapFrame_ == currentFrame_) {
            stealVolatileHeapKey_[heapPosition] = EncodeStableWinnerKey(
                ComputeStableStealKey(handle), activePosition_[handle]);
            stealVolatileHeapHandle_[heapPosition] = handle;
            VolatileHeapSiftUp(heapPosition);
            VolatileHeapSiftDown(stealVolatileHeapPosition_[handle]);
            return;
        }
        // Attack/release replacements belong to the exact dynamic list,
        // while a rare immediately-stable replacement belongs to the stable
        // heap.  Reclassify those uncommon cases through the normal path.
        RemoveStealCandidate(handle);
        if (stealHeapValid_ &&
            v.state[handle] != static_cast<uint8_t>(VoiceState::Free)) {
            PushStealCandidate(handle, activePosition_[handle]);
        }
        return;
    }
    if (stealHeapValid_ &&
        v.state[handle] != static_cast<uint8_t>(VoiceState::Free)) {
        PushStealCandidate(handle, activePosition_[handle]);
    }
}

inline void VoiceManager::StartRelease(VoiceHandle handle) {
    ++stealPeekEpoch_;
    if (handle >= maxVoices_) return;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Active)) {
        // A planner hook may advance this voice to the event frame and
        // discover that it naturally retired before the requested release.
        if (v.state[handle] != static_cast<uint8_t>(VoiceState::Active))
            return;
        UnlinkExclusiveClass(handle);
        if (deferredReleaseHook_) {
            // Whole-voice pre-pass: chain/held bookkeeping happens now (so
            // later pre-pass queries observe it in ingress order); the
            // audible state flip is deferred to the event frame.
            UnlinkChannelKey(handle);
            v.heldBySustain[handle] = 0u;
            v.heldBySostenuto[handle] = 0u;
            deferredReleaseHook_(handle, deferredReleaseUserData_);
            return;
        }
        UnlinkChannelKey(handle);
        v.heldBySustain[handle] = 0u;
        v.heldBySostenuto[handle] = 0u;
        v.state[handle] = static_cast<uint8_t>(VoiceState::Releasing);
        releasingCount_.fetch_add(1u, std::memory_order_relaxed);
        // Track the voice in the Releasing-ring fast-path index. Entries are
        // validated lazily when consumed; if a voice leaves Releasing by any
        // other route its entry simply goes stale and is skipped later.
        if (releasingRingCapacity_ != 0u) {
            if (releasingRingCount_ == releasingRingCapacity_) {
                // Overwrite the oldest entry on overflow; bounded memory,
                // correctness never depends on ring contents.
                releasingRingHead_ =
                    (releasingRingHead_ + 1u) & releasingRingMask_;
                --releasingRingCount_;
            }
            releasingRing_[(releasingRingHead_ + releasingRingCount_) &
                           releasingRingMask_] =
                static_cast<uint32_t>(handle);
            ++releasingRingCount_;
        }
        // SF2 sampleModes 3 = loop during key depression: stop looping so the
        // sample plays out to its end through the release tail.
        if (v.loopMode[handle] == 3) v.loopEnabled[handle] = 0;
        RefreshRenderClass(handle);
    }
}

inline void VoiceManager::FinalizeDeferredRelease(VoiceHandle handle) {
    ++stealPeekEpoch_;
    if (handle >= maxVoices_ ||
        v.state[handle] != static_cast<uint8_t>(VoiceState::Releasing))
        return;
    releasingCount_.fetch_add(1u, std::memory_order_relaxed);
    if (releasingRingCapacity_ != 0u) {
        if (releasingRingCount_ == releasingRingCapacity_) {
            releasingRingHead_ =
                (releasingRingHead_ + 1u) & releasingRingMask_;
            --releasingRingCount_;
        }
        releasingRing_[(releasingRingHead_ + releasingRingCount_) &
                       releasingRingMask_] = static_cast<uint32_t>(handle);
        ++releasingRingCount_;
    }
    RefreshRenderClass(handle);
}

inline void VoiceManager::AdoptStealTailSlot(uint32_t slot) {
    ++stealPeekEpoch_;
    if (slot >= kStealTailReserve) return;
    if (v.stealTailFramesRemaining[slot] == 0u) return;
    if (stealTailPosition_[slot] < stealTailCount_) return;  // already live
    if (stealTailCount_ >= kStealTailReserve) return;
    const uint32_t position = stealTailCount_++;
    stealTailList_[position] = slot;
    stealTailPosition_[slot] = position;
    stealTailMinHeapValid_ = false;
}

inline void VoiceManager::RetireVoice(VoiceHandle handle) {
    ++stealPeekEpoch_;
    if (handle >= maxVoices_ || v.state[handle] == static_cast<uint8_t>(VoiceState::Free))
        return;

    ++retireCount_;
    if (GetVoiceAge(handle) < 2) ++retireImmediateCount_;

    const bool wasReleasing =
        v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing);
    if (wasReleasing)
        releasingCount_.fetch_sub(1u, std::memory_order_relaxed);

    // Dense chopped streams interleave volatile retirement with immediate
    // full-pool launches. Invalidating here made the next steal rebuild the
    // complete candidate index, turning ordinary churn into repeated O(V)
    // spikes. Remove the retiring candidate and repair only the swapped
    // handle's path; both stable and volatile indices support this exactly.
    const bool maintainStealIndex = stealHeapValid_;
    if (maintainStealIndex)
        RemoveStealCandidate(handle);
    UnlinkChannelKey(handle);
    UnlinkExclusiveClass(handle);
    UnlinkPlayGroup(handle);
    UnlinkChannelActive(handle);
    UnlinkRenderClass(handle);
    v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
    v.currentGain[handle] = 0.0f;
    freeStack_[freeTop_++] = static_cast<int32_t>(handle);
    assert(freeTop_ <= maxVoices_ && "freeStack_ overflow in RetireVoice");

    const uint32_t position = activePosition_[handle];
    assert(position < activeCount_ && activeList_[position] == handle);
    const uint32_t lastPosition = activeCount_ - 1;
    if (position != lastPosition) {
        const uint32_t moved = activeList_[lastPosition];
        activeList_[position] = moved;
        activePosition_[moved] = position;
        if (maintainStealIndex)
            UpdateStealCandidate(static_cast<VoiceHandle>(moved));
    }
    --activeCount_;
    activePosition_[handle] = UINT32_MAX;
}

inline void VoiceManager::RebuildActivePositions() {
    for (uint32_t position = 0; position < activeCount_; ++position)
        activePosition_[activeList_[position]] = position;
    stealHeapValid_ = false;
}

inline bool VoiceManager::IsActive(VoiceHandle handle) const {
    return handle < maxVoices_ && v.state[handle] != static_cast<uint8_t>(VoiceState::Free);
}

inline void VoiceManager::ConfigureVoice(
    VoiceHandle handle, const VoiceConfiguration& setup,
    const ChannelParamsSnapshot& cp, bool commitDeferred) {
    ConfigureVoice(handle, setup, setup.playIndex, cp, commitDeferred);
}

inline void VoiceManager::ConfigureVoice(
    VoiceHandle handle, const VoiceConfiguration& setup, uint32_t playIndex,
    const ChannelParamsSnapshot& cp, bool commitDeferred) {
    if (handle >= maxVoices_) return;

    UnlinkExclusiveClass(handle);
    SetVoicePlayIndex(handle, playIndex);
    if (groupStageNext_)
        ApplyVoiceConfigurationFieldsImpl<true>(handle, setup, cp,
                                                VoiceRenderClass::Generic);
    else
        ApplyVoiceConfigurationFields(handle, setup, cp);
    LinkExclusiveClass(handle);
    RefreshRenderClass(handle);
    if (commitDeferred)
        CommitVoiceConfiguration(handle);
    else
        UpdateStealCandidate(handle);
#if defined(SVMS_NOTE_TRACE)
    if (g_noteTrace)
        g_noteTrace->OnLaunch(handle, v.playIndex[handle], v.channel[handle],
                              v.note[handle], v.velocity[handle], currentFrame_);
#endif
    if (voiceConfiguredHook_)
        voiceConfiguredHook_(handle, voiceConfiguredUserData_);
}

inline void VoiceManager::ApplyVoiceConfigurationFields(
    VoiceHandle handle, const VoiceConfiguration& setup,
    const ChannelParamsSnapshot& cp, VoiceRenderClass knownClass) {
    ApplyVoiceConfigurationFieldsImpl<false>(handle, setup, cp, knownClass);
}

// kStage = true (in-place steal launch inside the whole-voice plan, flag
// `voices.launch_staging`): every launch-derived dense value that no launch-
// time reader needs goes to the VoiceLaunchRecord instead of its SoA array;
// MaterializeLaunch copies it over before the row's first render read.  The
// arithmetic is shared with the SoA sink so both layouts hold identical bits.
template <bool kStage>
inline void VoiceManager::ApplyVoiceConfigurationFieldsImpl(
    VoiceHandle handle, const VoiceConfiguration& setup,
    const ChannelParamsSnapshot& cp, VoiceRenderClass knownClass) {
    ++stealPeekEpoch_;
#define SVMS_LAUNCH_SET(name, value) \
    do { \
        if constexpr (kStage) rec->name = (value); \
        else v.name[handle] = (value); \
    } while (0)
    VoiceLaunchRecord* const rec = kStage ? v.launch + handle : nullptr;
    if constexpr (kStage) {
        v.staged[handle] = 1u;
        stagedPending_ = true;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        g_launchStagingStaged.fetch_add(1u, std::memory_order_relaxed);
#endif
    } else if (stagedPending_) {
        // Direct SoA writes below replace everything a stale record held.
        v.staged[handle] = 0u;
    }

    SVMS_LAUNCH_SET(sampleStart, setup.sampleStart);
    v.loopMode[handle] = setup.loopMode;
    SVMS_LAUNCH_SET(phaseIncs, setup.phaseStep);
    v.basePhaseIncs[handle] = setup.basePhaseStep;
    v.pitchBendScales[handle] = setup.pitchBendScale;
    SVMS_LAUNCH_SET(sampleBacked, setup.sampleBacked);

    if constexpr (!kStage) v.phases[handle] = 0.0f;

    const uint32_t relEnd = setup.sampleEnd > setup.sampleStart
        ? setup.sampleEnd - setup.sampleStart : 0u;
    const uint32_t relLoopStart = setup.loopStart > setup.sampleStart
        ? setup.loopStart - setup.sampleStart : 0u;
    const uint32_t relLoopEnd = setup.loopEnd > setup.sampleStart
        ? setup.loopEnd - setup.sampleStart : 0u;
    SVMS_LAUNCH_SET(relEnd, relEnd);
    SVMS_LAUNCH_SET(relLoopS, relLoopStart);
    SVMS_LAUNCH_SET(relLoopE, relLoopEnd);
    if constexpr (!kStage) {
        v.relLoopSF[handle] = static_cast<float>(relLoopStart);
        v.relLoopEF[handle] = static_cast<float>(relLoopEnd);
    }
    const bool knownSustained =
        knownClass == VoiceRenderClass::SustainedLoop ||
        knownClass == VoiceRenderClass::SustainedOneShot;
    v.loopEnabled[handle] = knownSustained
        ? static_cast<uint8_t>(
            knownClass == VoiceRenderClass::SustainedLoop)
        : static_cast<uint8_t>(
            (setup.loopMode == 1u || setup.loopMode == 3u) &&
            setup.loopEnd > setup.loopStart + 1u);

    v.presetIndex[handle] = setup.presetIndex;
    v.regionIndex[handle] = setup.regionIndex;
    exclusiveClass_[handle] = setup.exclusiveClass < kExclusiveClassCount
        ? static_cast<uint8_t>(setup.exclusiveClass) : 0u;
    v.targetGain[handle] = setup.initialGain;
    SVMS_LAUNCH_SET(sustainLevel, setup.sustainLevel * setup.initialGain);
    SVMS_LAUNCH_SET(delaySamplesRemaining, setup.delaySamples);
    SVMS_LAUNCH_SET(holdSamplesRemaining, setup.holdSamples);
    SVMS_LAUNCH_SET(attackSamplesRemaining, setup.attackSamples);
    SVMS_LAUNCH_SET(decaySamplesRemaining, setup.decaySamples);
    SVMS_LAUNCH_SET(attackGainStep, setup.attackGainStep);
    SVMS_LAUNCH_SET(decaySlope, setup.decaySlope);
    SVMS_LAUNCH_SET(releaseDecay, setup.releaseDecay);
    SVMS_LAUNCH_SET(releaseSamplesRemaining, setup.releaseSamples > 0u
        ? FloorLimitedReleaseSamples(setup.releaseSamples, setup.releaseDecay,
                                     setup.initialGain)
        : UINT32_MAX);
    v.currentGain[handle] = 0.0f;
    if (knownSustained) {
        v.envelopeStage[handle] = 3u;
        v.currentGain[handle] = setup.initialGain;
    } else if (setup.delaySamples > 0u) {
        v.envelopeStage[handle] = 4u;
    } else if (setup.holdSamples > 0u) {
        v.envelopeStage[handle] = 0u;
    } else if (setup.attackSamples > 0u) {
        v.envelopeStage[handle] = 1u;
    } else if (setup.decaySamples > 0u) {
        v.envelopeStage[handle] = 2u;
        v.currentGain[handle] = setup.initialGain;
    } else {
        v.envelopeStage[handle] = 3u;
        v.currentGain[handle] = setup.initialGain;
    }

    // A merged launch stands for `launchMult_` identical voices: the factor
    // rides in the pan gains, so every later fold (CC7/10/11, steal scores,
    // whole-voice ops, ghosts) carries it without knowing about merging. A
    // single launch takes the unscaled branch and stays bit-identical.
    float gainLeft = setup.gainLeft;
    float gainRight = setup.gainRight;
    if (launchMult_ > 1u) {
        const float scale = static_cast<float>(launchMult_);
        gainLeft *= scale;
        gainRight *= scale;
        v.mult[handle] = launchMult_;
    } else {
        v.mult[handle] = 1u;
    }
    v.gainLeft[handle] = gainLeft;
    v.gainRight[handle] = gainRight;
    SVMS_LAUNCH_SET(filterA0, setup.filterA0);
    SVMS_LAUNCH_SET(filterB1, setup.filterB1);
    SVMS_LAUNCH_SET(filterB2, setup.filterB2);
    if constexpr (!kStage) {
        v.filterZ1[handle] = 0.0f;
        v.filterZ2[handle] = 0.0f;
    }
    SVMS_LAUNCH_SET(filterEnabled, setup.filterEnabled);
    v.vibLfoToPitchCents[handle] = setup.vibLfoToPitchCents;
    v.vibLfoSteps[handle] = setup.vibLfoPhaseStep;
    v.vibLfoPhases[handle] = 0.0f;
    v.vibLfoDelays[handle] = setup.vibLfoDelaySamples;
    v.vibLfoModulated[handle] = 0u;
    const float mixGainL = gainLeft * cp.mixScaleLeft;
    const float mixGainR = gainRight * cp.mixScaleRight;
    SVMS_LAUNCH_SET(mixGainL, mixGainL);
    SVMS_LAUNCH_SET(mixGainR, mixGainR);
    v.stealOutputGain[handle] = std::sqrt(
        mixGainL * mixGainL + mixGainR * mixGainR);
    SVMS_LAUNCH_SET(renderGainL, v.currentGain[handle] * mixGainL);
    SVMS_LAUNCH_SET(renderGainR, v.currentGain[handle] * mixGainR);
#undef SVMS_LAUNCH_SET
}

// Copy a staged launch record into the SoA arrays (see VoiceLaunchRecord).
// Row-local: any worker may run it on a row it owns.
inline void VoiceManager::MaterializeLaunch(VoiceHandle handle) noexcept {
    // The arrays below still hold the displaced voice's values; a deferred
    // ghost snapshot must take them before they are overwritten.
    if (deferredGhostPending_ != nullptr &&
        deferredGhostPending_[static_cast<size_t>(handle) * deferredGhostStride_] != 0u)
        deferredGhostFlush_(handle, deferredGhostUserData_);
    const VoiceLaunchRecord& rec = v.launch[handle];
#define SVMS_MATERIALIZE_FIELD(type, name) v.name[handle] = rec.name;
    SVMS_VOICE_LAUNCH_FIELDS(SVMS_MATERIALIZE_FIELD)
#undef SVMS_MATERIALIZE_FIELD
    v.phases[handle] = 0.0f;
    v.filterZ1[handle] = 0.0f;
    v.filterZ2[handle] = 0.0f;
    v.relLoopSF[handle] = static_cast<float>(rec.relLoopS);
    v.relLoopEF[handle] = static_cast<float>(rec.relLoopE);
    v.staged[handle] = 0u;
}

inline uint32_t VoiceManager::CountStagedRows() const noexcept {
    uint32_t count = 0u;
    if (v.launch == nullptr) return 0u;
    for (uint32_t h = 0u; h < maxVoices_; ++h)
        count += v.staged[h] != 0u ? 1u : 0u;
    return count;
}

inline VoiceRenderClass VoiceManager::ClassifyConfiguration(
    const VoiceConfiguration& setup) {
    if (setup.sampleBacked == 0u ||
        setup.sampleEnd <= setup.sampleStart + 1u)
        return VoiceRenderClass::Generic;
    const bool loop =
        (setup.loopMode == 1u || setup.loopMode == 3u) &&
        setup.loopEnd > setup.loopStart + 1u;
    uint8_t stage = 3u;
    if (setup.delaySamples != 0u) stage = 4u;
    else if (setup.holdSamples != 0u) stage = 0u;
    else if (setup.attackSamples != 0u) stage = 1u;
    else if (setup.decaySamples != 0u) stage = 2u;
    if (stage == 3u)
        return loop ? VoiceRenderClass::SustainedLoop
                    : VoiceRenderClass::SustainedOneShot;
    if (loop && (stage == 1u || stage == 2u))
        return VoiceRenderClass::TransientLoop;
    return VoiceRenderClass::Generic;
}

inline bool VoiceManager::IsStableConfiguration(
    const VoiceConfiguration& setup) {
    const bool preDecay = setup.delaySamples != 0u ||
        setup.holdSamples != 0u || setup.attackSamples != 0u;
    return setup.sampleBacked != 0u &&
        setup.sampleEnd > setup.sampleStart + 1u &&
        (preDecay || setup.decaySamples == 0u);
}

inline float VoiceManager::ComputeNewbornStableStealKey(
    VoiceHandle handle) const {
    const float outputGain = v.stealOutputGain[handle];
    float effectiveLevel =
        std::fabs(v.targetGain[handle]) * outputGain;
    if (sampledSteal_) effectiveLevel *= StealLaneFactor(handle);
    const float score = 0.0f - effectiveLevel * kBassMidiStealGainScale;
    const float commonAgeScore =
        static_cast<float>(currentFrame_) * stealAgeScale_;
    return score - commonAgeScore;
}

SVMS_VM_FORCEINLINE bool VoiceManager::TryLaunchSingleVoiceInPlace(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration& setup, uint32_t playIndex,
    const ChannelParamsSnapshot& cp, VoiceHandle& outHandle) {
    if (freeTop_ != 0u || !IsStableConfiguration(setup)) return false;
    // Per-key voice cap: when this key is at its cap the launch must replace
    // the key's oldest member, not the globally best victim. Bail to the
    // general path, which resolves the key-local victim once per transaction
    // (kept out of this O(1) fast path's bookkeeping).
    if (perKeyVoiceCap_ != 0u &&
        channelKeyVoiceCount_[channel][note] >= perKeyVoiceCap_)
        return false;

    uint32_t selectedPosition = 0u;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const bool profileLaunch = (++launchProfileCounter_ & 4095u) == 0u;
    const uint64_t profileBegin = profileLaunch ? __rdtsc() : 0u;
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t testSelectionBegin = BeginLaunchStageForTest();
#endif
    // Releasing-ring fast path: under release-storm churn the oldest
    // releasing voice is an acceptable BASSMIDI-consistent victim, exactly
    // like PopStealCandidate's fast path. Peek first so grouped voices are
    // left untouched (they need the atomic group retirement path below).
    uint32_t ringPeekPosition = 0u;
    VoiceHandle ringPeek = kInvalidVoice;
    if (ReleasingRingEligible())
        ringPeek = NextValidReleasingRingVictim(ringPeekPosition, false);
    const bool fromRing = ringPeek != kInvalidVoice &&
        playGroupPrev_[ringPeek] < 0 && playGroupNext_[ringPeek] < 0;
    VoiceHandle handle = kInvalidVoice;
    if (fromRing) {
        handle = NextValidReleasingRingVictim(selectedPosition, true);
        ++releasingRingHits_;
    } else {
        handle = PopStealCandidate(selectedPosition, true);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::VictimSelection,
                          testSelectionBegin);
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const uint64_t profileAfterPop = profileLaunch ? __rdtsc() : 0u;
#endif
    if (handle == kInvalidVoice) return false;

    const bool volatileVictim = !fromRing &&
        stealCandidateReserved_[handle] == 1u &&
        !IsStableStealCandidate(handle) && VolHeapHas(handle);
    const bool eligible = fromRing ||
        ((stealCandidateReserved_[handle] == 2u &&
          IsStableStealCandidate(handle) &&
          stealWinnerTree_[stealTreeLeafBase_ + handle] ==
              stealStableKey_[handle]) ||
         volatileVictim);
    if (!eligible) {
        stealCandidateReserved_[handle] = 0u;
        return false;
    }

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    RecordVictimGroupForTest(handle, LaunchVictimPath::SingleInPlace, true);
#endif

    const VoiceRenderClass desiredClass = ClassifyConfiguration(setup);
    const uint8_t desiredClassValue = static_cast<uint8_t>(desiredClass);
    const bool preserveChannelIndex = v.channel[handle] == channel;
    const bool preserveRenderIndex =
        v.renderClass[handle] == desiredClassValue;

    // The successor is staged right below (same handle, same function), so the
    // victim's staged-field arrays stay intact until its record is applied.
    ghostCopyDeferrable_ = launchStagingActive_ && stealTailCaptureSuppressed_;
    CaptureStealTail(handle);
    ghostCopyDeferrable_ = false;
    if (fromRing) {
        // Mirror tier-pop bookkeeping: unlink the victim from the volatile
        // candidate index it still occupies.
        RemoveStealCandidate(handle);
    } else if (volatileVictim) {
        stealCandidateReserved_[handle] = 0u;
        RemoveReservedVolatileRoot(handle);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const uint64_t profileAfterTail = profileLaunch ? __rdtsc() : 0u;
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t testLifecycleBegin = BeginLaunchStageForTest();
#endif
    ++stealCount_;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
        releasingCount_.fetch_sub(1u, std::memory_order_relaxed);
    UnlinkChannelKey(handle);
    UnlinkExclusiveClass(handle);
    UnlinkPlayGroup(handle);
    if (!preserveChannelIndex) MoveChannelActiveInPlace(handle, channel);
    if (!preserveRenderIndex) UnlinkRenderClass(handle);

    // This atomic path has no observer between victim removal and complete
    // configuration. Initialize only lifecycle fields that survive the
    // transaction; the generic prepared initializer's placeholder sample,
    // envelope, class, links, and gains would all be overwritten below.
    v.state[handle] = static_cast<uint8_t>(VoiceState::Active);
    if (preserveChannelIndex) v.channel[handle] = channel;
    v.note[handle] = note;
    v.velocity[handle] = velocity;
    v.heldBySustain[handle] = 0u;
    v.heldBySostenuto[handle] = 0u;
    v.releaseStartInBlock[handle] = 0u;
    v.birthFrame[handle] = currentFrame_;
    v.stealFadeInFramesRemaining[handle] = 0u;
    v.stealFadeInFramesTotal[handle] = 0u;
    LinkChannelKey(handle);

    // A playIndex is unique to this MIDI note-on. The old group has already
    // been unlinked, so SetVoicePlayIndex's second unlink is unnecessary.
    v.playIndex[handle] = playIndex;
    lastLinkedPlayIndex_ = playIndex;
    lastLinkedPlayVoice_ = handle;

#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const uint64_t profileAfterLifecycle = profileLaunch ? __rdtsc() : 0u;
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Lifecycle,
                          testLifecycleBegin);
    const uint64_t testConfigureBegin = BeginLaunchStageForTest();
#endif

    if (launchStagingActive_)
        ApplyVoiceConfigurationFieldsImpl<true>(handle, setup, cp,
                                                desiredClass);
    else
        ApplyVoiceConfigurationFields(handle, setup, cp, desiredClass);
    LinkExclusiveClass(handle);
    if (preserveRenderIndex) {
        v.renderClass[handle] = desiredClassValue;
    } else {
        LinkRenderClass(handle);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const uint64_t profileAfterConfigure = profileLaunch ? __rdtsc() : 0u;
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Configuration,
                          testConfigureBegin);
    const uint64_t testTreeBegin = BeginLaunchStageForTest();
#endif

    // The stable replacement retains its leaf; a volatile victim vacated the
    // heap and inserts that same physical handle as a new stable leaf.
    // Ring-served victims likewise occupied a volatile-tier slot before.
    stealCandidateReserved_[handle] = 0u;
    const float score = ComputeNewbornStableStealKey(handle);
    stealStableKey_[handle] = EncodeStableWinnerKey(
        score, activePosition_[handle]);
    stealWinnerTree_[stealTreeLeafBase_ + handle] = stealStableKey_[handle];
    if (volatileVictim || fromRing) ++stealHeapCount_;
    RefreshStealWinnerPath(handle);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance,
                          testTreeBegin);
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    if (profileLaunch) {
        const uint64_t profileEnd = __rdtsc();
        ++launchProfileSamples_;
        launchProfilePopCycles_ += profileAfterPop - profileBegin;
        launchProfileTailCycles_ += profileAfterTail - profileAfterPop;
        launchProfileLifecycleCycles_ +=
            profileAfterLifecycle - profileAfterTail;
        launchProfileConfigureCycles_ +=
            profileAfterConfigure - profileAfterLifecycle;
        launchProfileTreeCycles_ += profileEnd - profileAfterConfigure;
    }
#endif
    outHandle = handle;
#if defined(SVMS_NOTE_TRACE)
    if (g_noteTrace)
        g_noteTrace->OnLaunch(handle, v.playIndex[handle], v.channel[handle],
                              v.note[handle], v.velocity[handle], currentFrame_);
#endif
    if (voiceConfiguredHook_)
        voiceConfiguredHook_(handle, voiceConfiguredUserData_);
    return true;
}

inline bool VoiceManager::ReuseMatchingStealGroup(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count,
    VoiceHandle* outHandles, bool& candidatesReservedInPlace) {
    candidatesReservedInPlace = false;
    // Per-key voice cap: a capped key must replace its own oldest member,
    // not re-voice a globally selected group that may belong to another key.
    if (perKeyVoiceCap_ != 0u &&
        channelKeyVoiceCount_[channel][note] >= perKeyVoiceCap_)
        return false;
    if (freeTop_ != 0u || count == 0u || count > maxVoices_ || !setups ||
        !outHandles)
        return false;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    ++groupReuseAttemptCount_;
#endif

    uint32_t selectedPosition = 0u;
    // Keep the selected leaf present until we know whether the complete
    // physical group can be replaced in-place. No other selection occurs
    // during this transaction, so the reservation is exact and private to
    // the audio thread.
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t selectionBegin = BeginLaunchStageForTest();
#endif
    VoiceHandle selected = PopStealCandidate(selectedPosition, true);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::VictimSelection,
                          selectionBegin);
#endif
    if (selected == kInvalidVoice) return false;

    uint32_t groupCount = 1u;
    outHandles[0] = selected;
    bool tooMany = false;
    int32_t linked = playGroupPrev_[selected];
    while (linked >= 0) {
        if (groupCount >= count) { tooMany = true; break; }
        outHandles[groupCount++] = static_cast<VoiceHandle>(linked);
        linked = playGroupPrev_[static_cast<uint32_t>(linked)];
    }
    if (!tooMany) {
        linked = playGroupNext_[selected];
        while (linked >= 0) {
            if (groupCount >= count) { tooMany = true; break; }
            outHandles[groupCount++] = static_cast<VoiceHandle>(linked);
            linked = playGroupNext_[static_cast<uint32_t>(linked)];
        }
    }

    if (groupCount != count || tooMany) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (tooMany || groupCount > count) ++groupReuseLargerCount_;
        else ++groupReuseSmallerCount_;
#endif
        // The caller needs a different number of slots. Restore the exact
        // candidate and use the general allocation path, which may retire a
        // larger/smaller physical group according to the existing policy.
        stealCandidateReserved_[selected] = 0u;
        return false;
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    ++groupReuseMatchCount_;
#endif

    bool canReserveInPlace = count <= 8u;
    for (uint32_t i = 0u; i < count && canReserveInPlace; ++i) {
        const VoiceHandle handle = outHandles[i];
        const VoiceConfiguration& setup = setups[i];
        canReserveInPlace = IsStableConfiguration(setup) &&
            IsStableStealCandidate(handle) &&
            stealWinnerTree_[stealTreeLeafBase_ + handle] ==
                stealStableKey_[handle];
    }

    if (canReserveInPlace) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        ++groupReuseReservedCount_;
#endif
        // The old and new voices occupy the same leaves. Preserve the leaves
        // while lifecycle/sample state is rewritten, then refresh their
        // cached scores and the union of both paths once at commit.
        candidatesReservedInPlace = true;
        for (uint32_t i = 0u; i < count; ++i) {
            stealCandidateReserved_[outHandles[i]] = 2u;
            stealCandidateDeferred_[outHandles[i]] = 1u;
        }
    } else {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        const uint64_t treeBegin = BeginLaunchStageForTest();
#endif
        const bool selectedVolatileRoot =
            stealCandidateReserved_[selected] == 1u &&
            stealVolatileHeapValid_ && stealVolatileHeapCount_ != 0u &&
            VolHeapIsRoot(selected);
        stealCandidateReserved_[selected] = 0u;
        if (selectedVolatileRoot)
            RemoveReservedVolatileRoot(selected);
        else
            RemoveStealCandidate(selected);
        for (uint32_t i = 1u; i < count; ++i)
            RemoveStealCandidate(outHandles[i]);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance,
                              treeBegin);
#endif
    }

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    RecordVictimGroupForTest(selected, LaunchVictimPath::MatchingReuse,
                             candidatesReservedInPlace);
#endif

    ReplaceGroupInPlace(channel, note, velocity, setups, count, outHandles,
                        candidatesReservedInPlace);
    return true;
}

// Capture every old layer, then replace the complete physical note in
// place. The fast path keeps its winner-tree leaves reserved; the fallback
// removed them above. Active positions and the free stack never move.
// (Shared by ReuseMatchingStealGroup and CommitPendingLaunchImpl.)
inline void VoiceManager::ReplaceGroupInPlace(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count,
    const VoiceHandle* outHandles, bool candidatesReservedInPlace) {
    ++stealPeekEpoch_;
    // Staging: the group's successors are configured into launch records right
    // after this (groupStageNext_), so the victims' staged-field arrays survive
    // until the records are applied and their ghost copies can be deferred.
    const bool stage = launchStagingActive_ && stealTailCaptureSuppressed_;
    ghostCopyDeferrable_ = stage;
    for (uint32_t i = 0u; i < count; ++i)
        CaptureStealTail(outHandles[i]);
    ghostCopyDeferrable_ = false;
    groupStageNext_ = stage;

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t lifecycleBegin = BeginLaunchStageForTest();
#endif
    for (uint32_t i = 0u; i < count; ++i) {
        const VoiceHandle handle = outHandles[i];
        // Steal-index reservation and channel-list membership are independent.
        // Replacing a grouped voice on the same channel can retain its exact
        // dense channel slot even when the victim came from the volatile heap.
        const bool preserveChannelIndex = v.channel[handle] == channel;
        const VoiceRenderClass desiredClass =
            ClassifyConfiguration(setups[i]);
        const uint8_t configuredClass = static_cast<uint8_t>(desiredClass);
        const bool preserveRenderIndex = candidatesReservedInPlace &&
            v.renderClass[handle] == configuredClass;
        ++stealCount_;
        if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
            releasingCount_.fetch_sub(1u, std::memory_order_relaxed);
        UnlinkChannelKey(handle);
        UnlinkExclusiveClass(handle);
        UnlinkPlayGroup(handle);
        if (!preserveChannelIndex) UnlinkChannelActive(handle);
        if (!preserveRenderIndex) UnlinkRenderClass(handle);
        if (!candidatesReservedInPlace)
            stealCandidateReserved_[handle] = 0u;
        v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
        v.currentGain[handle] = 0.0f;

        InitializePreparedVoice(handle, channel, note, velocity, stage);
        if (preserveRenderIndex)
            v.renderClass[handle] = configuredClass;
        stealCandidateDeferred_[handle] = 1u;
        LinkChannelKey(handle);
        if (!preserveChannelIndex) LinkChannelActive(handle);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Lifecycle, lifecycleBegin);
#endif
}

// ── Lazy same-frame launch ──────────────────────────────────────────────
// See VoiceManager::BeginLazyLaunchRun.  Mirrors ReuseMatchingStealGroup's
// success conditions exactly (stable tournament-tree winner, a play group of
// the same size, every member an in-place stable candidate), then, when that
// victim group was itself launched on this very frame, parks the new note as
// pending instead of rewriting the pool.  The victim's steal-fade tail is the
// one thing skipped (DropZeroLifeVictimsEnabled): the normal path skips it
// too for these zero-life victims.
inline void VoiceManager::FlushPendingTree() {
    PendingLaunch& p = pendingLaunch_;
    if (!p.treeDirty) return;
    p.treeDirty = false;
    for (uint32_t i = 0u; i < p.count; ++i)
        stealWinnerTree_[stealTreeLeafBase_ + p.handles[i]] =
            stealStableKey_[p.handles[i]];
    RefreshStealWinnerPaths(p.handles, p.count);
}

inline bool VoiceManager::TryLazyGroupLaunch(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count, uint32_t playIndex,
    const ChannelParamsSnapshot& cp, VoiceHandle* outHandles) {
    ++stealPeekEpoch_;
    if (count < 2u || count > 8u || perKeyVoiceCap_ != 0u || freeTop_ != 0u ||
        !stealHeapValid_ || stealHeapCount_ == 0u)
        return false;
    for (uint32_t i = 0u; i < count; ++i) {
        const VoiceConfiguration& s = setups[i];
        if (!IsStableConfiguration(s) || s.exclusiveClass != 0u ||
            s.offByClass != 0u)
            return false;
    }
    PendingLaunch& p = pendingLaunch_;
    VoiceHandle group[8];
    uint32_t groupCount = 0u;
    // Walks the play group of `winner`: winner first, then its prev chain,
    // then its next chain (the order PopStealCandidate hands out).
    const auto walkGroup = [&](VoiceHandle winner) {
        groupCount = 1u;
        group[0] = winner;
        int32_t linked = playGroupPrev_[winner];
        while (linked >= 0) {
            if (groupCount >= count) return false;
            group[groupCount++] = static_cast<VoiceHandle>(linked);
            linked = playGroupPrev_[static_cast<uint32_t>(linked)];
        }
        linked = playGroupNext_[winner];
        while (linked >= 0) {
            if (groupCount >= count) return false;
            group[groupCount++] = static_cast<VoiceHandle>(linked);
            linked = playGroupNext_[static_cast<uint32_t>(linked)];
        }
        return groupCount == count;
    };

    bool chainStep = false;
    if (p.valid && p.treeDirty && p.count != count) FlushPendingTree();
    if (p.valid && p.treeDirty) {
        // Chain mode: the pending group is the root (maxKey beats every other
        // voice), its steal-candidate state was verified when the chain
        // began and nothing has touched the pool since.
        bool ok = true;
        if (stealVolatileCount_ != 0u)
            ok = stealVolatileHeapValid_ && stealVolatileHeapCount_ != 0u &&
                 !(VolHeapRootKey() > p.maxKey);
        if (ok) ok = walkGroup(p.handles[p.winnerIdx]);
        if (ok) {
            stealCount_ += count;
            ++lazyChainSteps_;
            chainStep = true;
        } else {
            FlushPendingTree();
        }
    }

    bool samePending = chainStep;
    if (!chainStep) {
        // Victim selection exactly as PopStealCandidate(reserve = true) makes
        // it when the stable tournament root wins: root of the winner tree,
        // unless a volatile candidate outranks it (then the normal path
        // decides).
        const uint64_t rootKey = StealTreeRoot();
        if (rootKey == 0u) return false;
        const uint32_t winnerPosition =
            UINT32_MAX - static_cast<uint32_t>(rootKey);
        if (winnerPosition >= activeCount_) return false;
        const VoiceHandle winner = static_cast<VoiceHandle>(
            activeList_[winnerPosition]);
        if (activePosition_[winner] != winnerPosition ||
            stealStableKey_[winner] != rootKey)
            return false;
        if (stealVolatileCount_ != 0u) {
            if (!stealVolatileHeapValid_ || stealVolatileHeapCount_ == 0u ||
                VolHeapRootKey() > rootKey)
                return false;
        }
        if (!walkGroup(winner)) return false;
        bool zeroLife = true;
        for (uint32_t i = 0u; i < count; ++i) {
            const VoiceHandle h = group[i];
            if (!IsStableStealCandidate(h) ||
                stealWinnerTree_[stealTreeLeafBase_ + h] != stealStableKey_[h])
                return false;
            zeroLife = zeroLife && v.birthFrame[h] == currentFrame_;
        }
        if (p.valid && p.count == count) {
            for (uint32_t i = 0u; i < count; ++i)
                samePending = samePending || p.handles[i] == winner;
        }
        if (samePending) {
            stealCount_ += count;          // the pending note it replaces
            ++lazyChainSteps_;
        } else {
            // A different group: everything pending lands first, exactly as
            // the sequential launches would have left it before this pop.
            if (p.valid) CommitPendingLaunchImpl();
            if (!zeroLife) return false;
            ++lazyStarts_;
        }
    }

    // Best key outside the pending group. A chain already in progress carries
    // it; the first chain step finds it by taking the group out of the (still
    // truthful) tree.
    uint64_t runnerUp = 0u;
    if (chainStep) {
        runnerUp = p.runnerUp;
    } else if (samePending) {
        for (uint32_t i = 0u; i < count; ++i)
            stealWinnerTree_[stealTreeLeafBase_ + group[i]] = 0u;
        RefreshStealWinnerPaths(group, count);
        runnerUp = StealTreeRoot();
    }

    p.valid = true;
    p.count = count;
    p.channel = channel;
    p.note = note;
    p.velocity = velocity;
    p.playIndex = playIndex;
    p.multiplicity = launchMult_;
    p.params = &cp;

    // The new keys, from the setup alone (no pool write): the same
    // expressions ApplyVoiceConfigurationFields and
    // ComputeNewbornStableStealKey evaluate for a freshly launched voice.
    const float commonAgeScore =
        static_cast<float>(currentFrame_) * stealAgeScale_;
    float scale = 1.0f;
    if (launchMult_ > 1u) scale = static_cast<float>(launchMult_);
    uint64_t maxKey = 0u;
    uint32_t winnerIdx = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        const VoiceHandle h = group[i];
        p.handles[i] = h;
        p.setups[i] = setups[i];
        outHandles[i] = h;
        float gainLeft = setups[i].gainLeft;
        float gainRight = setups[i].gainRight;
        if (launchMult_ > 1u) {
            gainLeft *= scale;
            gainRight *= scale;
        }
        const float mixGainL = gainLeft * cp.mixScaleLeft;
        const float mixGainR = gainRight * cp.mixScaleRight;
        const float outputGain = std::sqrt(
            mixGainL * mixGainL + mixGainR * mixGainR);
        float effectiveLevel = std::fabs(setups[i].initialGain) *
            outputGain;
        if (sampledSteal_)
            effectiveLevel *= StealLaneFactorFor(velocity, note, true);
        const float score = 0.0f - effectiveLevel * kBassMidiStealGainScale;
        p.scores[i] = score;
        const uint64_t key = EncodeStableWinnerKey(
            score - commonAgeScore, activePosition_[h]);
        stealStableKey_[h] = key;
        if (key > maxKey) { maxKey = key; winnerIdx = i; }
    }
    if (samePending && maxKey > runnerUp) {
        // Still the root: leave the tree stale until the commit.
        p.treeDirty = true;
        p.maxKey = maxKey;
        p.runnerUp = runnerUp;
        p.winnerIdx = winnerIdx;
        return true;
    }
    p.treeDirty = false;
    for (uint32_t i = 0u; i < count; ++i)
        stealWinnerTree_[stealTreeLeafBase_ + group[i]] =
            stealStableKey_[group[i]];
    RefreshStealWinnerPaths(group, count);
    return true;
}

// Lands the pending note through the same code a normal group-reuse launch
// runs: reserve the group's leaves, replace it in place (the victim here is
// the zero-life group, whose tail the plan hook skips), configure, commit.
inline void VoiceManager::CommitPendingLaunchImpl() {
    ++stealPeekEpoch_;
    FlushPendingTree();
    PendingLaunch& p = pendingLaunch_;
    p.valid = false;
    for (uint32_t i = 0u; i < p.count; ++i) {
        stealCandidateReserved_[p.handles[i]] = 2u;
        stealCandidateDeferred_[p.handles[i]] = 1u;
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    uint64_t lazyKeys[8];
    for (uint32_t i = 0u; i < p.count; ++i)
        lazyKeys[i] = stealStableKey_[p.handles[i]];
#endif
    ReplaceGroupInPlace(p.channel, p.note, p.velocity, p.setups, p.count,
                        p.handles, true);
    struct MultiplicityRestore {
        uint32_t& slot;
        uint32_t saved;
        ~MultiplicityRestore() { slot = saved; }
    } multiplicityRestore{launchMult_, launchMult_};
    launchMult_ = p.multiplicity > 1u ? p.multiplicity : 1u;
    for (uint32_t layer = 0u; layer < p.count; ++layer) {
        ConfigureVoice(p.handles[layer], p.setups[layer], p.playIndex,
                       *p.params, false);
    }
    groupStageNext_ = false;
    CommitVoiceGroupConfigurations(p.handles, p.count, true);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    for (uint32_t i = 0u; i < p.count; ++i) {
        if (stealStableKey_[p.handles[i]] != lazyKeys[i])
            g_lazyKeyMismatches.fetch_add(1u, std::memory_order_relaxed);
    }
    g_lazyChainSteps.store(lazyChainSteps_, std::memory_order_relaxed);
#endif
}

inline void VoiceManager::CommitVoiceGroupConfigurations(
    const VoiceHandle* handles, uint32_t count,
    bool candidatesReservedInPlace) {
    ++stealPeekEpoch_;
    if (!candidatesReservedInPlace) {
        // A prepared SF2 note becomes visible atomically. When every layer is
        // a stable candidate, publish all leaves first and repair the union of
        // their tree paths once. No event can observe an intermediate layer,
        // and the resulting root is identical to individual commits.
        bool canBatchStable = stealHeapValid_ && count <= 8u;
        for (uint32_t layer = 0u; layer < count && canBatchStable; ++layer) {
            const VoiceHandle handle = handles[layer];
            canBatchStable = handle < maxVoices_ &&
                stealCandidateDeferred_[handle] != 0u &&
                stealCandidateReserved_[handle] == 0u &&
                IsStableStealCandidate(handle);
        }
        if (canBatchStable) {
            for (uint32_t layer = 0u; layer < count; ++layer) {
                const VoiceHandle handle = handles[layer];
                stealCandidateDeferred_[handle] = 0u;
                ++stealHeapCount_;
                const float score = ComputeStableStealKey(handle);
                stealStableKey_[handle] = EncodeStableWinnerKey(
                    score, activePosition_[handle]);
                stealWinnerTree_[stealTreeLeafBase_ + handle] =
                    stealStableKey_[handle];
            }
            RefreshStealWinnerPaths(handles, count);
            return;
        }
        for (uint32_t layer = 0u; layer < count; ++layer)
            CommitVoiceConfiguration(handles[layer]);
        return;
    }

    bool stillStable = stealHeapValid_ && count <= 8u;
    for (uint32_t layer = 0u; layer < count && stillStable; ++layer) {
        const VoiceHandle handle = handles[layer];
        stillStable = handle < maxVoices_ &&
            stealCandidateDeferred_[handle] != 0u &&
            stealCandidateReserved_[handle] == 2u &&
            IsStableStealCandidate(handle) &&
            stealWinnerTree_[stealTreeLeafBase_ + handle] ==
                stealStableKey_[handle];
    }
    if (!stillStable) {
        for (uint32_t layer = 0u; layer < count; ++layer)
            CommitVoiceConfiguration(handles[layer]);
        return;
    }

    for (uint32_t layer = 0u; layer < count; ++layer) {
        const VoiceHandle handle = handles[layer];
        stealCandidateDeferred_[handle] = 0u;
        stealCandidateReserved_[handle] = 0u;
        const float score = ComputeStableStealKey(handle);
        stealStableKey_[handle] = EncodeStableWinnerKey(
            score, activePosition_[handle]);
        stealWinnerTree_[stealTreeLeafBase_ + handle] =
            stealStableKey_[handle];
    }
    RefreshStealWinnerPaths(handles, count);
}

inline bool VoiceManager::LaunchVoiceGroup(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count,
    const ChannelParamsSnapshot& channelParams, VoiceHandle* outHandles,
    uint32_t blockOffset) {
    const uint32_t playIndex = setups && count != 0u
        ? setups[0].playIndex : UINT32_MAX;
    return LaunchVoiceGroup(channel, note, velocity, setups, count, playIndex,
                            channelParams, outHandles, blockOffset);
}

inline bool VoiceManager::LaunchVoiceGroup(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count, uint32_t playIndex,
    const ChannelParamsSnapshot& channelParams, VoiceHandle* outHandles,
    uint32_t blockOffset, uint32_t multiplicity) {
    lastLaunchRejected_ = false;
    if (!setups || !outHandles || count == 0u || count > maxVoices_ ||
        count > voiceLimit_)
        return false;
    if (count > launchLayersHighWater_) launchLayersHighWater_ = count;
    // Sampled stealing: a full pool first recycles an older sustained voice on
    // the same key, then takes the cheapest of a bounded sample.
    VoiceHandle recycleVictim = kInvalidVoice;
    if (sampledSteal_) {
        if (pendingLaunch_.valid) CommitPendingLaunchImpl();
        if (freeTop_ == 0u && activeCount_ >= voiceLimit_ &&
            perKeyVoiceCap_ == 0u) {
            const float newLevel =
                NewbornStealLevel(setups, count, channelParams, multiplicity);
            recycleVictim = SameKeyRecycleVictim(channel, note, velocity,
                                                 newLevel);
            if (recycleVictim == kInvalidVoice) {
                recycleVictim = SampledVictim(newLevel, playIndex);
                if (recycleVictim == kInvalidVoice) {
                    // Nothing cheap enough in the sample: the note is dropped.
                    lastLaunchRejected_ = true;
                    ++stealRejectCount_;
                    return false;
                }
            }
        }
    }
    const PriorityIncoming incoming{channel, note, velocity};
    if (recycleVictim == kInvalidVoice &&
        (sampledSteal_ || stealReject_.load(std::memory_order_relaxed)) &&
        ShouldRejectNoteOn(setups, count, channelParams, multiplicity,
                           sampledSteal_ ? &incoming : nullptr)) {
        // Pairing note-offs sees no voice for this generation, exactly as if
        // the note had been launched and stolen before it rendered.
        lastLaunchRejected_ = true;
        ++stealRejectCount_;
        return false;
    }
    ++stealPeekEpoch_;
    // Scope the multiplicity to this launch: ApplyVoiceConfigurationFields
    // reads launchMult_, and every return path below must restore it.
    struct MultiplicityScope {
        uint32_t& slot;
        MultiplicityScope(uint32_t& s, uint32_t m) : slot(s) {
            slot = m > 1u ? m : 1u;
        }
        ~MultiplicityScope() { slot = 1u; }
    } multiplicityScope(launchMult_, multiplicity);
    if (multiplicity > 1u) mergedVoicesMayExist_ = true;
    if (lazyRunActive_ && recycleVictim == kInvalidVoice) {
        if (TryLazyGroupLaunch(channel, note, velocity, setups, count,
                               playIndex, channelParams, outHandles))
            return true;
    }
    if (pendingLaunch_.valid) CommitPendingLaunchImpl();
    // Resolve every exclusive class once for the complete physical note.
    // Doing this before any layer allocation/configuration prevents layered
    // regions from choking siblings born by this same MIDI note-on.
    ChokeExclusiveClasses(channel, setups, count, blockOffset);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    BeginLaunchTestProfile(channel, note, setups, count);
#endif
    if (count == 1u && recycleVictim == kInvalidVoice &&
        TryLaunchSingleVoiceInPlace(
            channel, note, velocity, setups[0], playIndex, channelParams,
            outHandles[0])) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        ++groupReuseAttemptCount_;
        ++groupReuseMatchCount_;
        ++groupReuseReservedCount_;
        FinishLaunchTestProfile(true);
#endif
        return true;
    }
    bool candidatesReservedInPlace = false;
    if (recycleVictim != kInvalidVoice ||
        !ReuseMatchingStealGroup(channel, note, velocity, setups, count,
                                 outHandles, candidatesReservedInPlace)) {
        // ── FLAG (steal-batch scope) ─────────────────────────────────────
        // The victim batching is deliberately scoped to ONE launch
        // transaction: between the first and last layer selection no
        // candidate insertion can occur (commits land only after this loop),
        // so the descending victim order of PopStealCandidates is identical
        // to the sequential per-layer PopStealCandidate pops — only the
        // per-layer call overhead is saved. Prefetching victims ACROSS notes
        // of a same-frame dispatch run is NOT exact: newborn commits of
        // earlier notes re-key their slots and can outrank remaining
        // prefetched victims, which changes WHICH voice is stolen (confirmed
        // empirically during bring-up). Any future run-level scheme must
        // solve that insertion race first.
        // Per-key voice cap (syndrv-style, opt-in; 0 = off): when this key
        // already plays at its cap, the whole note-on replaces the oldest
        // key member instead of growing the pileup. Resolved once per
        // transaction so the layers of one physical note cannot steal each
        // other: layer 0 is forced onto the oldest key voice — its sibling
        // retirement frees slots that feed the remaining layers — while
        // layers 1..N-1 run the normal free-slot/steal logic. The key chain
        // holds only still-playing voices (StartRelease unlinks), so
        // release tails never count against the cap.
        VoiceHandle keyedVictim = kInvalidVoice;
        if (perKeyVoiceCap_ != 0u &&
            channelKeyVoiceCount_[channel][note] >= perKeyVoiceCap_) {
            const int32_t oldest = channelKeyVoiceOldest_[channel][note];
            if (oldest >= 0 &&
                v.state[oldest] == static_cast<uint8_t>(VoiceState::Active))
                keyedVictim = static_cast<VoiceHandle>(oldest);
        } else if (recycleVictim != kInvalidVoice) {
            keyedVictim = recycleVictim;
        }
        VoiceHandle batchVictims[kStealBatchMaxLayers];
        uint32_t batchPositions[kStealBatchMaxLayers];
        bool consumed[kStealBatchMaxLayers] = {};
        uint32_t popped = 0u;
        const bool batchSteal = stealBatchingEnabled_ &&
            count >= 2u && count <= kStealBatchMaxLayers &&
            // Releasing-ring-ordered victims cannot be re-armed (the ring
            // entry is consumed and there is no front-repush), so the batch
            // only engages while the ring fast path is inactive; once
            // inactive it stays inactive for the rest of the launch (sibling
            // retirements only move freeTop_/activeCount_ further from
            // eligibility and dispatch never pushes ring entries).
            !ReleasingRingEligible();
        if (batchSteal)
            popped = PopStealCandidates(count, batchVictims, batchPositions);
        uint32_t allocated = 0u;
        uint32_t cursor = 0u;
        for (; allocated < count; ++allocated) {
            // Mirror AllocateVoiceOrSteal's free-stack-vs-steal decision:
            // only hand a prefetched victim to a layer that is guaranteed to
            // take the steal path (the free-slot branch cannot fire), and
            // skip victims already retired as siblings of an earlier layer's
            // victim (state Free). The keyed-cap victim is skipped in every
            // pass — layer 0 consumes it through the forced steal, and it
            // must never be fed to a later layer as a fresh victim (by then
            // the handle holds this launch's own newborn).
            while (cursor < popped &&
                   (v.state[batchVictims[cursor]] !=
                        static_cast<uint8_t>(VoiceState::Active) ||
                    batchVictims[cursor] == keyedVictim))
                ++cursor;
            VoiceHandle fed = kInvalidVoice;
            uint32_t fedPosition = 0u;
            uint32_t fedCursor = UINT32_MAX;
            if (freeTop_ == 0u && activeCount_ >= voiceLimit_ &&
                cursor < popped &&
                !(allocated == 0u && keyedVictim != kInvalidVoice)) {
                fed = batchVictims[cursor];
                fedPosition = batchPositions[cursor];
                fedCursor = cursor;
                ++cursor;
            }
            outHandles[allocated] = AllocateVoiceOrSteal(
                channel, note, velocity, nullptr, true, false, fed,
                fedPosition,
                allocated == 0u ? keyedVictim : kInvalidVoice);
            if (outHandles[allocated] == kInvalidVoice) {
                // Restore the unconsumed, still-live prefetched victims so
                // the candidate index matches the per-layer path exactly,
                // then roll the layers launched so far back.
                RearmLiveBatchVictims(batchVictims, popped, consumed);
                for (uint32_t rollback = 0u; rollback < allocated; ++rollback)
                    RetireVoice(outHandles[rollback]);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
                FinishLaunchTestProfile(false);
#endif
                return false;
            }
            if (fedCursor != UINT32_MAX) consumed[fedCursor] = true;
        }
        // The launch consumed its fed victims; every other popped victim
        // must rejoin the candidate index (the per-layer path would never
        // have popped it). The keyed-cap victim was consumed by the forced
        // steal — its handle now holds the replacement, not a stealable.
        RearmLiveBatchVictims(batchVictims, popped, consumed, keyedVictim);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t configureBegin = BeginLaunchStageForTest();
#endif
    for (uint32_t layer = 0u; layer < count; ++layer) {
        // Keep every layer invisible to the steal index until the complete
        // physical note has been prepared.  The former layered path linked a
        // Generic candidate, configured it, and reclassified it one layer at
        // a time, multiplying tree/list work for stereo SoundFonts.
        ConfigureVoice(outHandles[layer], setups[layer], playIndex,
                       channelParams, false);
    }
    groupStageNext_ = false;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Configuration,
                          configureBegin);
    const uint64_t treeBegin = BeginLaunchStageForTest();
#endif
    CommitVoiceGroupConfigurations(outHandles, count,
                                   candidatesReservedInPlace);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance, treeBegin);
    FinishLaunchTestProfile(true);
#endif
    return true;
}
