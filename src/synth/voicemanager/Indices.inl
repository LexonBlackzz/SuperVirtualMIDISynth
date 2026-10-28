// Part of VoiceManager.h; included from there inside namespace svms. Not standalone.

inline uint32_t VoiceManager::GetChannelActiveCount(uint8_t channel) const {
    return channel < kChannelCount ? channelActiveCount_[channel] : 0u;
}

template <typename Consumer>
inline void VoiceManager::ForEachChannelActive(
    uint8_t channel, Consumer&& consume) const noexcept {
    if (channel >= kChannelCount) return;
    uint32_t block = channelActiveHead_[channel];
    while (block != UINT32_MAX) {
        const ChannelIndexBlock& page = channelIndexBlocks_[block];
        for (uint32_t offset = 0u; offset < page.count; ++offset)
            consume(static_cast<VoiceHandle>(page.handles[offset]));
        block = page.next;
    }
}

inline void VoiceManager::InvalidateStealCandidates() {
    ++stealPeekEpoch_;
    stealHeapValid_ = false;
}

// Gains evolve while samples render, which changes per-voice steal scores
// outside the incremental index maintenance.  Call once per render pass
// (RenderBlock start) so the volatile steal heap rebuilds on the next
// block's first steal instead of on every event frame.
inline void VoiceManager::MarkStealScoresDirty() {
    stealVolatileHeapValid_ = false;
}

inline uint32_t VoiceManager::GetRenderClassCount(
    VoiceRenderClass renderClass) const {
    const uint32_t index = static_cast<uint32_t>(renderClass);
    return index < kVoiceRenderClassCount ? renderClassCount_[index] : 0u;
}

template <typename Consumer>
inline void VoiceManager::ForEachChannelKeyVoice(
    uint8_t channel, uint8_t note, Consumer&& consume) const noexcept {
    if (channel >= kChannelCount || note >= kNoteCount) return;
    int32_t link = channelKeyVoiceHead_[channel][note];
    while (link != -1) {
        const int32_t next = v.nextChannelKeyVoice[link];
        consume(static_cast<VoiceHandle>(link));
        link = next;
    }
}

inline uint32_t VoiceManager::GetVoiceAge(VoiceHandle handle) const {
    if (handle >= maxVoices_ || currentFrame_ <= v.birthFrame[handle]) return 0;
    const uint64_t age = currentFrame_ - v.birthFrame[handle];
    return age > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age);
}

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
inline VoiceHandle VoiceManager::FindStealVictimExhaustiveForTest() const {
    StealCandidate best{};
    bool found = false;
    for (uint32_t position = 0; position < activeCount_; ++position) {
        const uint32_t handle = activeList_[position];
        const StealCandidate candidate{
            ComputeStealScore(handle), handle, position};
        if (!found || HigherPriorityCandidate(candidate, best)) {
            best = candidate;
            found = true;
        }
    }
    return found ? static_cast<VoiceHandle>(best.handle) : kInvalidVoice;
}
#endif

inline float VoiceManager::ComputeEffectiveStealLevel(
    VoiceHandle handle) const {
    // BASS ranks a control-derived level before waveform sampling. The
    // cached stereo energy norm removes constant-power pan from that estimate.
    // It changes only with region/channel gain, while volatile decay/release
    // voices are rescored every output frame.
    const float outputGain = v.stealOutputGain[handle];
    const uint8_t stage = v.envelopeStage[handle];
    const bool preDecay =
        v.state[handle] == static_cast<uint8_t>(VoiceState::Active) &&
        (stage == 4u || stage == 0u || stage == 1u);
    const float envelopeGain = preDecay
        ? v.targetGain[handle] : v.currentGain[handle];
    return std::fabs(envelopeGain) * outputGain;
}

inline float VoiceManager::ComputeStealScore(uint32_t idx) const {
    // Reversed form of BASSMIDI's minimum-priority scan:
    //   int(effectiveLevel * 42000) - (ageSamples >> 8)
    // A continuous /256 age term keeps the persistent heap invariant between
    // voice-state changes while preserving the same ordering and scale.
    const float ageUnits = static_cast<float>(GetVoiceAge(
        static_cast<VoiceHandle>(idx))) * stealAgeScale_;
    float level = ComputeEffectiveStealLevel(static_cast<VoiceHandle>(idx));
    if (sampledSteal_) level *= StealLaneFactor(static_cast<VoiceHandle>(idx));
    return ageUnits - level * kBassMidiStealGainScale;
}

inline float VoiceManager::StealLaneFactor(VoiceHandle handle) const {
    // Released tails are the cheapest victims; everything else is weighted by
    // its velocity lane, with low keys and voices still in delay/hold/attack
    // protected further.
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
        return 0.5f;
    const uint8_t stage = v.envelopeStage[handle];
    const bool inAttack = stage == 4u || stage == 0u || stage == 1u;
    return StealLaneFactorFor(v.velocity[handle], v.note[handle], inAttack);
}

// Pull the per-handle SoA lines a launch writes and the render kernels read
// into cache while the caller still has transaction work (steal-tree repair,
// sibling retirement, tail capture) to run. One line per array; the arrays
// are capacity-scaled SoA, so each is a distinct cache line at any realistic
// pool size.
inline void VoiceManager::PrefetchVoiceLines(VoiceHandle handle) {
#if defined(SVMS_VM_HAS_PREFETCH)
    const char* base = reinterpret_cast<const char*>(&v.state[handle]);
    // state/channel/note/velocity share neighborhood layout only per array;
    // each SoA array is separately allocated, so prefetch each line.
    _mm_prefetch(base, _MM_HINT_T0);
#define SVMS_PREFETCH_FIELD(field) \
    _mm_prefetch(reinterpret_cast<const char*>(&v.field[handle]), _MM_HINT_T0)
    SVMS_PREFETCH_FIELD(phases);
    SVMS_PREFETCH_FIELD(phaseIncs);
    SVMS_PREFETCH_FIELD(currentGain);
    SVMS_PREFETCH_FIELD(targetGain);
    SVMS_PREFETCH_FIELD(sustainLevel);
    SVMS_PREFETCH_FIELD(attackGainStep);
    SVMS_PREFETCH_FIELD(releaseDecay);
    SVMS_PREFETCH_FIELD(mixGainL);
    SVMS_PREFETCH_FIELD(mixGainR);
    SVMS_PREFETCH_FIELD(renderGainL);
    SVMS_PREFETCH_FIELD(renderGainR);
    SVMS_PREFETCH_FIELD(sampleStart);
    SVMS_PREFETCH_FIELD(relEnd);
    SVMS_PREFETCH_FIELD(relLoopS);
    SVMS_PREFETCH_FIELD(relLoopE);
    SVMS_PREFETCH_FIELD(holdSamplesRemaining);
    SVMS_PREFETCH_FIELD(attackSamplesRemaining);
    SVMS_PREFETCH_FIELD(decaySamplesRemaining);
    SVMS_PREFETCH_FIELD(delaySamplesRemaining);
    SVMS_PREFETCH_FIELD(releaseSamplesRemaining);
    SVMS_PREFETCH_FIELD(decaySlope);
    SVMS_PREFETCH_FIELD(envelopeStage);
    SVMS_PREFETCH_FIELD(renderClass);
    _mm_prefetch(reinterpret_cast<const char*>(&v.rows[handle]), _MM_HINT_T0);
    SVMS_PREFETCH_FIELD(birthFrame);
    SVMS_PREFETCH_FIELD(stealOutputGain);
    SVMS_PREFETCH_FIELD(loopEnabled);
    SVMS_PREFETCH_FIELD(relLoopSF);
    SVMS_PREFETCH_FIELD(relLoopEF);
#undef SVMS_PREFETCH_FIELD
    _mm_prefetch(reinterpret_cast<const char*>(&stealCandidateDeferred_[handle]),
                 _MM_HINT_T0);
#else
    (void)handle;
#endif
}

inline void VoiceManager::InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity) {
    ++stealPeekEpoch_;
    v.state[handle]        = static_cast<uint8_t>(VoiceState::Active);
    v.channel[handle]      = channel;
    v.note[handle]         = note;
    v.velocity[handle]     = velocity;
    v.staged[handle]       = 0u;
    v.phases[handle]       = 0.0f;
    v.phaseIncs[handle]    = 0.0f;
    v.basePhaseIncs[handle] = 0.0f;
    v.pitchBendScales[handle] = 1.0f;
    v.currentGain[handle]  = 0.0f;
    v.targetGain[handle]   = 1.0f;
    v.sustainLevel[handle] = 0.7f;
    v.attackGainStep[handle] = 0.0f;
    v.releaseDecay[handle]   = kDefaultReleaseDecay;
    v.gainLeft[handle]     = 1.0f;
    v.gainRight[handle]    = 1.0f;
    v.sampleStart[handle]  = 0;
    v.loopMode[handle]     = 0;
    v.sampleBacked[handle] = 0;
    v.presetIndex[handle] = UINT16_MAX;
    v.regionIndex[handle] = UINT16_MAX;
    v.playIndex[handle] = UINT32_MAX;
    v.mult[handle] = 1u;
    exclusiveClass_[handle] = 0u;
    exclusiveClassNext_[handle] = -1;
    exclusiveClassPrev_[handle] = -1;
    v.holdSamplesRemaining[handle]   = 0;
    v.attackSamplesRemaining[handle] = 0;
    v.decaySamplesRemaining[handle]  = 0;
    v.delaySamplesRemaining[handle]  = 0;
    v.releaseSamplesRemaining[handle] = UINT32_MAX;
    v.decaySlope[handle]        = 1.0f;
    v.envelopeStage[handle]     = 0;
    v.renderClass[handle] = static_cast<uint8_t>(VoiceRenderClass::Generic);
    v.heldBySustain[handle]     = 0;
    v.heldBySostenuto[handle]   = 0;
    v.releaseStartInBlock[handle] = 0;
    v.nextChannelKeyVoice[handle] = -1;
    v.prevChannelKeyVoice[handle] = -1;
    playGroupNext_[handle] = -1;
    playGroupPrev_[handle] = -1;
    v.mixGainL[handle]          = 0.0f;
    v.mixGainR[handle]          = 0.0f;
    v.renderGainL[handle]       = 0.0f;
    v.renderGainR[handle]       = 0.0f;
    v.stealOutputGain[handle]   = 0.0f;
    v.vibLfoToPitchCents[handle] = 0.0f;
    v.vibLfoSteps[handle]       = 0.0f;
    v.vibLfoPhases[handle]      = 0.0f;
    v.vibLfoDelays[handle]      = 0u;
    v.vibLfoModulated[handle]   = 0u;
    v.filterA0[handle]          = 0.0f;
    v.filterB1[handle]          = 0.0f;
    v.filterB2[handle]          = 0.0f;
    v.filterZ1[handle]          = 0.0f;
    v.filterZ2[handle]          = 0.0f;
    v.filterEnabled[handle]     = 0u;
    v.relEnd[handle]            = 0;
    v.relLoopS[handle]          = 0;
    v.relLoopE[handle]          = 0;
    v.relLoopSF[handle]         = 0.0f;
    v.relLoopEF[handle]         = 0.0f;
    v.loopEnabled[handle]       = 0;
    v.birthFrame[handle]        = currentFrame_;
    v.stealFadeInFramesRemaining[handle] = 0;
    v.stealFadeInFramesTotal[handle] = 0;
    stealCandidateDeferred_[handle] = 0u;
}

inline void VoiceManager::InitializePreparedVoice(
    VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity,
    bool staged) {
    ++stealPeekEpoch_;
    // ConfigureVoice immediately supplies every sample/envelope/gain field.
    // Initialize only state observed by lifecycle indices before that
    // transaction commits, avoiding dozens of default stores per SF2 note.
    v.state[handle] = static_cast<uint8_t>(VoiceState::Active);
    v.channel[handle] = channel;
    v.note[handle] = note;
    v.velocity[handle] = velocity;
    if (!staged) {
        v.sampleBacked[handle] = 0u;
        v.relEnd[handle] = 0u;
    }
    v.staged[handle] = 0u;
    v.envelopeStage[handle] = 0u;
    v.renderClass[handle] = static_cast<uint8_t>(VoiceRenderClass::Generic);
    v.heldBySustain[handle] = 0u;
    v.heldBySostenuto[handle] = 0u;
    v.releaseStartInBlock[handle] = 0u;
    v.nextChannelKeyVoice[handle] = -1;
    v.prevChannelKeyVoice[handle] = -1;
    v.playIndex[handle] = UINT32_MAX;
    v.mult[handle] = 1u;
    playGroupNext_[handle] = -1;
    playGroupPrev_[handle] = -1;
    exclusiveClass_[handle] = 0u;
    exclusiveClassNext_[handle] = -1;
    exclusiveClassPrev_[handle] = -1;
    v.birthFrame[handle] = currentFrame_;
    v.stealFadeInFramesRemaining[handle] = 0u;
    v.stealFadeInFramesTotal[handle] = 0u;
    if (!staged) {
        v.filterA0[handle] = 0.0f;
        v.filterB1[handle] = 0.0f;
        v.filterB2[handle] = 0.0f;
        v.filterZ1[handle] = 0.0f;
        v.filterZ2[handle] = 0.0f;
        v.filterEnabled[handle] = 0u;
    }
    stealCandidateDeferred_[handle] = 0u;
}

inline void VoiceManager::LinkChannelKey(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    uint8_t ch = v.channel[handle];
    uint8_t nt = v.note[handle];
    const int32_t previousHead = channelKeyVoiceHead_[ch][nt];
    v.prevChannelKeyVoice[handle] = -1;
    v.nextChannelKeyVoice[handle] = previousHead;
    if (previousHead >= 0)
        v.prevChannelKeyVoice[static_cast<uint32_t>(previousHead)] =
            static_cast<int32_t>(handle);
    else
        channelKeyVoiceOldest_[ch][nt] = static_cast<int32_t>(handle);
    channelKeyVoiceHead_[ch][nt] = static_cast<int32_t>(handle);
    ++channelKeyVoiceCount_[ch][nt];
}

inline void VoiceManager::UnlinkChannelKey(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t ch = v.channel[handle];
    const uint8_t nt = v.note[handle];
    if (sampledSteal_) lastUnlinkedByKey_[ch][nt] = static_cast<int32_t>(handle);
    const int32_t previous = v.prevChannelKeyVoice[handle];
    const int32_t next = v.nextChannelKeyVoice[handle];
    if (previous >= 0) {
        v.nextChannelKeyVoice[static_cast<uint32_t>(previous)] = next;
        --channelKeyVoiceCount_[ch][nt];
    } else if (channelKeyVoiceHead_[ch][nt] == static_cast<int32_t>(handle)) {
        channelKeyVoiceHead_[ch][nt] = next;
        --channelKeyVoiceCount_[ch][nt];
    } else {
        return;  // not linked — nothing to count
    }
    if (next >= 0)
        v.prevChannelKeyVoice[static_cast<uint32_t>(next)] = previous;
    else if (channelKeyVoiceOldest_[ch][nt] == static_cast<int32_t>(handle))
        channelKeyVoiceOldest_[ch][nt] = previous;
    v.nextChannelKeyVoice[handle] = -1;
    v.prevChannelKeyVoice[handle] = -1;
}

inline void VoiceManager::UnlinkPlayGroup(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const int32_t previous = playGroupPrev_[handle];
    const int32_t next = playGroupNext_[handle];
    if (previous >= 0)
        playGroupNext_[static_cast<uint32_t>(previous)] = next;
    if (next >= 0)
        playGroupPrev_[static_cast<uint32_t>(next)] = previous;
    if (lastLinkedPlayVoice_ == handle) {
        if (previous >= 0) {
            lastLinkedPlayVoice_ = static_cast<VoiceHandle>(previous);
        } else {
            lastLinkedPlayVoice_ = kInvalidVoice;
            lastLinkedPlayIndex_ = UINT32_MAX;
        }
    }
    playGroupPrev_[handle] = -1;
    playGroupNext_[handle] = -1;
}

inline void VoiceManager::LinkExclusiveClass(VoiceHandle handle) {
    if (handle >= maxVoices_ ||
        v.state[handle] != static_cast<uint8_t>(VoiceState::Active))
        return;
    const uint8_t exclusive = exclusiveClass_[handle];
    if (exclusive == 0u || exclusive >= kExclusiveClassCount) return;
    const uint8_t channel = v.channel[handle];
    const int32_t previousHead = exclusiveClassHead_[channel][exclusive];
    exclusiveClassPrev_[handle] = -1;
    exclusiveClassNext_[handle] = previousHead;
    if (previousHead >= 0)
        exclusiveClassPrev_[static_cast<uint32_t>(previousHead)] =
            static_cast<int32_t>(handle);
    exclusiveClassHead_[channel][exclusive] = static_cast<int32_t>(handle);
}

inline void VoiceManager::UnlinkExclusiveClass(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t exclusive = exclusiveClass_[handle];
    if (exclusive == 0u || exclusive >= kExclusiveClassCount) {
        exclusiveClassNext_[handle] = -1;
        exclusiveClassPrev_[handle] = -1;
        return;
    }
    const uint8_t channel = v.channel[handle];
    const int32_t previous = exclusiveClassPrev_[handle];
    const int32_t next = exclusiveClassNext_[handle];
    if (previous >= 0) {
        exclusiveClassNext_[static_cast<uint32_t>(previous)] = next;
    } else if (exclusiveClassHead_[channel][exclusive] ==
               static_cast<int32_t>(handle)) {
        exclusiveClassHead_[channel][exclusive] = next;
    } else {
        return;
    }
    if (next >= 0)
        exclusiveClassPrev_[static_cast<uint32_t>(next)] = previous;
    exclusiveClassNext_[handle] = -1;
    exclusiveClassPrev_[handle] = -1;
}

inline void VoiceManager::ChokeExclusiveClasses(
    uint8_t channel, const VoiceConfiguration* setups, uint32_t count,
    uint32_t blockOffset) {
    uint64_t incoming[2]{};
    bool anyVictim = false;
    for (uint32_t layer = 0u; layer < count; ++layer) {
        const uint16_t value = setups[layer].offByClass != 0u
            ? setups[layer].offByClass : setups[layer].exclusiveClass;
        if (value == 0u || value >= kExclusiveClassCount)
            continue;
        const uint64_t bit = uint64_t(1) << (value & 63u);
        uint64_t& word = incoming[value >> 6u];
        if ((word & bit) != 0u) continue;
        word |= bit;
        anyVictim |= exclusiveClassHead_[channel][value] >= 0;
    }
    if (!anyVictim) return;

    const float releaseDecay = MakeReleaseDecay(kMinReleaseSeconds,
                                                 sampleRate_);
    const uint32_t releaseSamples = MakeReleaseSamples(kMinReleaseSeconds,
                                                       sampleRate_);
    for (uint32_t exclusive = 1u; exclusive < kExclusiveClassCount;
         ++exclusive) {
        if ((incoming[exclusive >> 6u] &
             (uint64_t(1) << (exclusive & 63u))) == 0u)
            continue;
        while (exclusiveClassHead_[channel][exclusive] >= 0) {
            const VoiceHandle victim = static_cast<VoiceHandle>(
                exclusiveClassHead_[channel][exclusive]);
            VoiceHandle first = victim;
            while (playGroupPrev_[first] >= 0)
                first = static_cast<VoiceHandle>(playGroupPrev_[first]);

            VoiceHandle current = first;
            while (current != kInvalidVoice) {
                const int32_t next = playGroupNext_[current];
                if (v.state[current] ==
                    static_cast<uint8_t>(VoiceState::Active)) {
                    EnsureMaterialized(current);
                    v.releaseDecay[current] = releaseDecay;
                    v.releaseSamplesRemaining[current] = releaseSamples;
                    v.releaseStartInBlock[current] = blockOffset;
                    StartRelease(current);
                } else {
                    UnlinkExclusiveClass(current);
                }
                current = next >= 0
                    ? static_cast<VoiceHandle>(next) : kInvalidVoice;
            }
            // A malformed/no-playIndex caller can leave a singleton outside
            // a group. StartRelease above must still have removed the head.
            if (exclusiveClassHead_[channel][exclusive] ==
                static_cast<int32_t>(victim))
                UnlinkExclusiveClass(victim);
        }
    }
}

inline uint32_t VoiceManager::AllocateChannelIndexBlock() {
    assert(channelIndexFreeTop_ != 0u);
    if (channelIndexFreeTop_ == 0u) return UINT32_MAX;
    const uint32_t block = channelIndexFreeStack_[--channelIndexFreeTop_];
    channelIndexBlocks_[block].count = 0u;
    channelIndexBlocks_[block].previous = UINT32_MAX;
    channelIndexBlocks_[block].next = UINT32_MAX;
    return block;
}

inline void VoiceManager::FreeChannelIndexBlock(uint32_t block) {
    if (block >= channelIndexBlockCount_) return;
    channelIndexBlocks_[block].count = 0u;
    channelIndexBlocks_[block].previous = UINT32_MAX;
    channelIndexBlocks_[block].next = UINT32_MAX;
    assert(channelIndexFreeTop_ < channelIndexBlockCount_);
    channelIndexFreeStack_[channelIndexFreeTop_++] = block;
}

inline VoiceHandle VoiceManager::LastChannelActive(uint8_t channel) const {
    if (channel >= kChannelCount) return kInvalidVoice;
    const uint32_t tail = channelActiveTail_[channel];
    if (tail == UINT32_MAX || channelIndexBlocks_[tail].count == 0u)
        return kInvalidVoice;
    return static_cast<VoiceHandle>(channelIndexBlocks_[tail].handles[
        channelIndexBlocks_[tail].count - 1u]);
}

inline void VoiceManager::LinkChannelActive(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t channel = v.channel[handle];
    uint32_t tail = channelActiveTail_[channel];
    if (tail == UINT32_MAX ||
        channelIndexBlocks_[tail].count == kChannelIndexBlockSize) {
        const uint32_t block = AllocateChannelIndexBlock();
        assert(block != UINT32_MAX);
        if (block == UINT32_MAX) return;
        channelIndexBlocks_[block].previous = tail;
        if (tail != UINT32_MAX)
            channelIndexBlocks_[tail].next = block;
        else
            channelActiveHead_[channel] = block;
        channelActiveTail_[channel] = block;
        tail = block;
    }
    ChannelIndexBlock& page = channelIndexBlocks_[tail];
    const uint32_t offset = page.count++;
    page.handles[offset] = handle;
    channelActiveBlock_[handle] = tail;
    channelActiveOffset_[handle] = static_cast<uint8_t>(offset);
    ++channelActiveCount_[channel];
}

inline void VoiceManager::UnlinkChannelActive(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t channel = v.channel[handle];
    const uint32_t block = channelActiveBlock_[handle];
    const uint32_t offset = channelActiveOffset_[handle];
    const uint32_t tail = channelActiveTail_[channel];
    if (block >= channelIndexBlockCount_ || tail >= channelIndexBlockCount_ ||
        offset >= channelIndexBlocks_[block].count) return;

    ChannelIndexBlock& tailPage = channelIndexBlocks_[tail];
    const uint32_t lastOffset = tailPage.count - 1u;
    const uint32_t moved = tailPage.handles[lastOffset];
    if (block != tail || offset != lastOffset) {
        channelIndexBlocks_[block].handles[offset] = moved;
        channelActiveBlock_[moved] = block;
        channelActiveOffset_[moved] = static_cast<uint8_t>(offset);
    }
    --tailPage.count;
    --channelActiveCount_[channel];
    channelActiveBlock_[handle] = UINT32_MAX;
    channelActiveOffset_[handle] = UINT8_MAX;

    if (tailPage.count == 0u) {
        const uint32_t previous = tailPage.previous;
        if (previous != UINT32_MAX)
            channelIndexBlocks_[previous].next = UINT32_MAX;
        else
            channelActiveHead_[channel] = UINT32_MAX;
        channelActiveTail_[channel] = previous;
        FreeChannelIndexBlock(tail);
    }
}

inline void VoiceManager::MoveChannelActiveInPlace(VoiceHandle handle,
                                                    uint8_t newChannel) {
    assert(handle < maxVoices_ && newChannel < kChannelCount);
    const uint8_t oldChannel = v.channel[handle];
    assert(oldChannel != newChannel);

    // Remove from the old dense channel pages, but keep this handle live and
    // transfer it directly into the new channel instead of publishing an
    // unindexed intermediate state.
    const uint32_t oldBlock = channelActiveBlock_[handle];
    const uint32_t oldOffset = channelActiveOffset_[handle];
    const uint32_t oldTail = channelActiveTail_[oldChannel];
    assert(oldBlock < channelIndexBlockCount_ &&
           oldTail < channelIndexBlockCount_ &&
           oldOffset < channelIndexBlocks_[oldBlock].count);
    ChannelIndexBlock& oldTailPage = channelIndexBlocks_[oldTail];
    const uint32_t oldLastOffset = oldTailPage.count - 1u;
    const uint32_t moved = oldTailPage.handles[oldLastOffset];
    if (oldBlock != oldTail || oldOffset != oldLastOffset) {
        channelIndexBlocks_[oldBlock].handles[oldOffset] = moved;
        channelActiveBlock_[moved] = oldBlock;
        channelActiveOffset_[moved] = static_cast<uint8_t>(oldOffset);
    }
    --oldTailPage.count;
    --channelActiveCount_[oldChannel];
    if (oldTailPage.count == 0u) {
        const uint32_t previous = oldTailPage.previous;
        if (previous != UINT32_MAX)
            channelIndexBlocks_[previous].next = UINT32_MAX;
        else
            channelActiveHead_[oldChannel] = UINT32_MAX;
        channelActiveTail_[oldChannel] = previous;
        FreeChannelIndexBlock(oldTail);
    }

    v.channel[handle] = newChannel;
    uint32_t newTail = channelActiveTail_[newChannel];
    if (newTail == UINT32_MAX ||
        channelIndexBlocks_[newTail].count == kChannelIndexBlockSize) {
        const uint32_t block = AllocateChannelIndexBlock();
        assert(block != UINT32_MAX);
        channelIndexBlocks_[block].previous = newTail;
        if (newTail != UINT32_MAX)
            channelIndexBlocks_[newTail].next = block;
        else
            channelActiveHead_[newChannel] = block;
        channelActiveTail_[newChannel] = block;
        newTail = block;
    }
    ChannelIndexBlock& newPage = channelIndexBlocks_[newTail];
    const uint32_t newOffset = newPage.count++;
    newPage.handles[newOffset] = handle;
    channelActiveBlock_[handle] = newTail;
    channelActiveOffset_[handle] = static_cast<uint8_t>(newOffset);
    ++channelActiveCount_[newChannel];
}

inline VoiceRenderClass VoiceManager::ClassifyVoice(VoiceHandle handle) const {
    if (handle >= maxVoices_ ||
        v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) {
        return VoiceRenderClass::Generic;
    }
    // A staged row's sample bounds live in its launch record.
    uint8_t sampleBacked;
    uint32_t relEnd, relLoopS, relLoopE;
    if (stagedPending_ && v.staged[handle] != 0u) {
        const VoiceLaunchRecord& rec = v.launch[handle];
        sampleBacked = rec.sampleBacked;
        relEnd = rec.relEnd;
        relLoopS = rec.relLoopS;
        relLoopE = rec.relLoopE;
    } else {
        sampleBacked = v.sampleBacked[handle];
        relEnd = v.relEnd[handle];
        relLoopS = v.relLoopS[handle];
        relLoopE = v.relLoopE[handle];
    }
    if (sampleBacked == 0u || relEnd < 2u)
        return VoiceRenderClass::Generic;
    if (v.stealFadeInFramesRemaining[handle] != 0u)
        return VoiceRenderClass::Generic;
    const bool loop = v.loopEnabled[handle] != 0u;
    if (loop && (relLoopS >= relLoopE || relLoopE > relEnd)) {
        return VoiceRenderClass::Generic;
    }
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
        return loop ? VoiceRenderClass::ReleaseLoop
                    : VoiceRenderClass::ReleaseOneShot;
    if (v.envelopeStage[handle] == 3u)
        return loop ? VoiceRenderClass::SustainedLoop
                    : VoiceRenderClass::SustainedOneShot;
    if (v.envelopeStage[handle] == 1u || v.envelopeStage[handle] == 2u) {
        return loop ? VoiceRenderClass::TransientLoop
                    : VoiceRenderClass::TransientOneShot;
    }
    return VoiceRenderClass::Generic;
}

inline void VoiceManager::LinkRenderClass(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const VoiceRenderClass renderClass = ClassifyVoice(handle);
    const uint32_t classIndex = static_cast<uint32_t>(renderClass);
    ++renderClassCount_[classIndex];
    renderClassLinked_[handle] = 1u;
    v.renderClass[handle] = static_cast<uint8_t>(renderClass);
    if (renderClass == VoiceRenderClass::SustainedLoop ||
        renderClass == VoiceRenderClass::SustainedOneShot) {
        if (stagedPending_ && v.staged[handle] != 0u) {
            VoiceLaunchRecord& rec = v.launch[handle];
            rec.renderGainL = v.currentGain[handle] * rec.mixGainL;
            rec.renderGainR = v.currentGain[handle] * rec.mixGainR;
        } else {
            v.renderGainL[handle] = v.currentGain[handle] * v.mixGainL[handle];
            v.renderGainR[handle] = v.currentGain[handle] * v.mixGainR[handle];
        }
    }
}

inline void VoiceManager::UnlinkRenderClass(VoiceHandle handle) {
    if (handle >= maxVoices_ || renderClassLinked_[handle] == 0u) return;
    const uint32_t classIndex = v.renderClass[handle];
    if (classIndex >= kVoiceRenderClassCount) return;
    --renderClassCount_[classIndex];
    renderClassLinked_[handle] = 0u;
}

inline void VoiceManager::RefreshRenderClass(VoiceHandle handle) {
    if (handle >= maxVoices_ ||
        v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return;
    const VoiceRenderClass desired = ClassifyVoice(handle);
    if (v.renderClass[handle] == static_cast<uint8_t>(desired) &&
        renderClassLinked_[handle] != 0u) {
        // Attack and decay intentionally share the transient render class,
        // but only attack has a time-invariant protected steal level. Move
        // the candidate between the persistent tree and volatile heap even
        // when no render-list migration is required.
        if (stealHeapValid_ && stealCandidateDeferred_[handle] == 0u) {
            const bool indexedStable =
                stealWinnerTree_[stealTreeLeafBase_ + handle] != 0u;
            if (indexedStable != IsStableStealCandidate(handle))
                UpdateStealCandidate(handle);
        }
        return;
    }
    UnlinkRenderClass(handle);
    LinkRenderClass(handle);
    // Envelope transitions can move a voice from the exact volatile set to
    // the persistent sustained heap without any MIDI event touching it.
    UpdateStealCandidate(handle);
}
