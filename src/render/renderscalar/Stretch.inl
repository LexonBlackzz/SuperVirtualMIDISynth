// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.

inline void RenderScalar::RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                                      const int16_t* sampleData,
                                      uint32_t sampleDataFrames,
                                      float* outputLeft, float* outputRight,
                                      uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                      const RenderEvent* events, uint32_t eventCount,
                                      bool correctnessMode,
                                      uint64_t blockStartFrame,
                                      float* const* channelBusLeft,
                                      float* const* channelBusRight) {
    RenderBlock(voices, channels, sampleData, nullptr, sampleDataFrames,
                outputLeft, outputRight, numFrames, cfg, events, eventCount,
                correctnessMode, blockStartFrame, channelBusLeft,
                channelBusRight);
}

inline void RenderScalar::RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                                      const int16_t* sampleData, const int16_t* hilbertData,
                                      uint32_t sampleDataFrames,
                                      float* outputLeft, float* outputRight,
                                      uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                      const RenderEvent* events, uint32_t eventCount,
                                      bool correctnessMode,
                                      uint64_t blockStartFrame,
                                      float* const* channelBusLeft,
                                      float* const* channelBusRight) {
    (void)cfg;
    voices.SetStealKeyBackend(kernelSet_->backend);
    // Gains changed during the previous block's render; the volatile steal
    // heap must rebuild at this block's first steal.  One rebuild per block
    // (not per event frame — see PopStealCandidate).
    voices.MarkStealScoresDirty();
    // Batched CC mix folds (MarkChannelMixStale) land before whole-voice
    // planning and before any voice renders this block. Mid-block CCs fold at
    // their exact dispatch boundaries instead.
    voices.FoldStaleChannelMixGains();
    // Live pool-limit changes are callback-boundary commands.  Applying one
    // before dense eligibility/snapshotting prevents a mid-plan lifecycle
    // mutation from invalidating worker-visible voice state.
    voices.ApplyRuntimeVoiceLimit(blockStartFrame);
    // Barrier events split the block: render the stretch before each one,
    // dispatch the barrier at its exact frame (same call sparse makes), and
    // continue with the rest.  Frames, order and blockCursor values are those
    // of the unsplit block.
    // Split points: a barrier event (dispatched at its frame between two
    // stretches) or the op-log budget running out (no dispatch; the split
    // lands on the start of the offending frame's event group so same-frame
    // order is preserved).  kind: 0 none, 1 barrier, 2 budget.
    // The ghost arrays hold wvGhostCapacity_ victims per stretch.  Every
    // launch layer can displace one voice (a ghost) and CC120 kills every
    // active voice, so a stretch is cut before the worst case would pass the
    // capacity.  Without this a long block (offline renders use 8192 frames)
    // of dense note-ons overflows and the displaced voices lose their
    // pre-steal audio (a cut + release pump every block).
    const uint64_t ghostCapacity = wvGhostCapacity_;
    const uint32_t layersPerLaunch = voices.GetLaunchLayersHighWater();
    const auto ghostCost = [&](const RenderEvent& event) -> uint32_t {
        switch (event.type) {
            case RenderEventType::NoteOn:
            case RenderEventType::NoteReExcite:
                return layersPerLaunch;
            case RenderEventType::ControlChange:
                return event.data1 == 120u ? voices.GetActiveCount() : 0u;
            default:
                return 0u;
        }
    };
    const auto findSplit = [&](uint32_t from, uint32_t& kind) -> uint32_t {
        uint32_t opSum = 0u;
        uint64_t ghostSum = 0u;
        // First event of the same-frame group `i` belongs to.  Tracked while
        // scanning: re-walking it per event made a frame holding N events
        // (a player catching up after a stall sends hundreds of thousands at
        // one timestamp) cost O(N^2), seconds inside one audio callback.
        uint32_t group = from;
        for (uint32_t i = from; i < eventCount; ++i) {
            if (i != from && events[i].frameOffset != events[i - 1u].frameOffset)
                group = i;
            if (IsBarrierEvent(events[i].type)) { kind = 1u; return i; }
            const uint32_t ops = WholeVoiceRowOpEstimate(events[i]);
            const uint32_t ghosts = ghostCost(events[i]);
            if (opSum + ops > wvRowOpBudget_ ||
                (ghostCapacity != 0u && ghostSum + ghosts > ghostCapacity)) {
                if (group > from) { kind = 2u; return group; }
                // One frame holds more than a whole budget of ops: keep the
                // group together and let the log grow (pathological).
            }
            opSum += ops;
            ghostSum += ghosts;
        }
        kind = 0u;
        return eventCount;
    };
    uint32_t splitKind = 0u;
    uint32_t split = findSplit(0u, splitKind);
    if (splitKind == 0u) {
        RenderBlockStretch(voices, channels, sampleData, hilbertData,
                           sampleDataFrames, outputLeft, outputRight,
                           numFrames, events, eventCount, correctnessMode,
                           blockStartFrame, channelBusLeft, channelBusRight);
        return;
    }
    uint32_t paths = 0u;
    uint32_t cursor = 0u;       // next unconsumed event
    uint32_t frameBase = 0u;    // first frame not yet rendered
    const auto renderStretch = [&](uint32_t frameEnd, uint32_t eventEnd) {
        if (frameEnd <= frameBase) return;
        splitEvents_.clear();
        for (uint32_t i = cursor; i < eventEnd; ++i) {
            splitEvents_.push_back(events[i]);
            splitEvents_.back().frameOffset -= frameBase;
        }
        float* const* busL = nullptr;
        float* const* busR = nullptr;
        if (channelBusLeft != nullptr && channelBusRight != nullptr) {
            for (uint32_t c = 0u; c < kChannelCount; ++c) {
                splitBusLeft_[c] = channelBusLeft[c] + frameBase;
                splitBusRight_[c] = channelBusRight[c] + frameBase;
            }
            busL = splitBusLeft_;
            busR = splitBusRight_;
        }
        RenderBlockStretch(voices, channels, sampleData, hilbertData,
                           sampleDataFrames, outputLeft + frameBase,
                           outputRight + frameBase, frameEnd - frameBase,
                           splitEvents_.data(),
                           static_cast<uint32_t>(splitEvents_.size()),
                           correctnessMode, blockStartFrame + frameBase,
                           busL, busR);
        paths |= lastRenderPaths_;
        frameBase = frameEnd;
    };
    while (splitKind != 0u) {
        if (splitKind == 1u) {
            const uint32_t barrier = split;
            const uint32_t barrierFrame =
                (std::min)(events[barrier].frameOffset, numFrames);
            // Events strictly before the barrier frame render with the
            // stretch.
            uint32_t stretchEnd = cursor;
            while (stretchEnd < barrier &&
                   events[stretchEnd].frameOffset < barrierFrame)
                ++stretchEnd;
            renderStretch(barrierFrame, stretchEnd);
            cursor = stretchEnd;
            // Same-frame events up to and including the barrier dispatch now.
            voices.SetCurrentFrame(blockStartFrame + barrierFrame);
            if (batchDispatcher_ != nullptr) {
                batchDispatcher_(events + cursor, barrier + 1u - cursor,
                                 barrierFrame, dispatcherUserData_);
            } else if (dispatcher_ != nullptr) {
                for (uint32_t i = cursor; i <= barrier; ++i)
                    dispatcher_(events[i], barrierFrame, dispatcherUserData_);
            }
            voices.MarkStealScoresDirty();
            voices.FoldStaleChannelMixGains();
            cursor = barrier + 1u;
            frameBase = (std::max)(frameBase, barrierFrame);
        } else {
            const uint32_t splitFrame =
                (std::min)(events[split].frameOffset, numFrames);
            renderStretch(splitFrame, split);
            cursor = split;
            frameBase = (std::max)(frameBase, splitFrame);
        }
        split = findSplit(cursor, splitKind);
    }
    renderStretch(numFrames, eventCount);
    lastRenderPaths_ = paths | 0x200u;  // bit9: block was barrier-split
}

inline void RenderScalar::RenderBlockStretch(
    VoiceManager& voices, const ChannelCache& channels,
    const int16_t* sampleData, const int16_t* hilbertData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t numFrames, const RenderEvent* events, uint32_t eventCount,
    bool correctnessMode, uint64_t blockStartFrame,
    float* const* channelBusLeft, float* const* channelBusRight) {
    if (voices.activeCount_ > scratchCapacity_ &&
        !ReserveVoiceCapacity(voices.activeCount_)) return;
    if (!classChanges_ || !retirements_) return;
    // The whole-voice renderer takes vibrato on every backend: AVX2 kernels
    // rebuild the 64-frame LFO windows internally, scalar/SSE2 render in
    // fixed windows split at every event frame (wvLfoBounds_).
    const ChannelParamsSnapshot* channelParams = channels.GetParams();
    bool vibratoActive = false;
    for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
        if (channelParams[channel].modDepth > 0.0f) {
            vibratoActive = true;
            break;
        }
    }
    lastRenderPaths_ = vibratoActive ? 0x100u : 0u;
    // ── Whole-voice whole-block renderer ────────────────────────────────
    // Single per-voice renderer: owns every note-only block, blocks with no
    // events at all, and note blocks carrying the lifecycle controllers
    // (sustain/sostenuto/all-notes-off/all-sound-off) plus the channel-wide
    // row mutators (pitch bend and CC7/10/11 mix folds), which land as
    // exact-frame channel ops applied inside the owning workers.  Vibrato
    // (CC1/channel pressure) rides this path on every backend: AVX2 kernels
    // rebuild the 64-frame LFO windows themselves; scalar/SSE2 render in
    // fixed windows, so their output is unchanged.
    {
        // Scalar/SSE2 vibrato renders on the whole-voice path in the
        // loop's exact LFO windows: every event frame and every 64 frames
        // inside a gap is a window boundary (kVibratoUpdateFrames).
        wvScalarNoVibrato_ = !vibratoActive &&
            kernelSet_->backend != RenderBackend::AVX2;
        wvScalarVibrato_ = vibratoActive &&
            kernelSet_->backend != RenderBackend::AVX2;
        if (wvScalarVibrato_) {
            wvLfoBounds_.clear();
            uint32_t windowCursor = 0u;
            uint32_t windowEvent = 0u;
            while (windowCursor < numFrames) {
                while (windowEvent < eventCount &&
                       events[windowEvent].frameOffset <= windowCursor)
                    ++windowEvent;
                uint32_t windowEnd = numFrames;
                if (windowEvent < eventCount &&
                    events[windowEvent].frameOffset < windowEnd)
                    windowEnd = events[windowEvent].frameOffset;
                if (windowEnd - windowCursor > kVibratoUpdateFrames)
                    windowEnd = windowCursor + kVibratoUpdateFrames;
                if (windowEnd <= windowCursor) break;
                wvLfoBounds_.push_back(windowEnd);
                windowCursor = windowEnd;
            }
        }
#if defined(_MSC_VER)
        const uint64_t wvPlanBegin = __rdtsc();
#endif
        const bool planned =
            PlanWholeVoiceBlock(voices, events, eventCount, blockStartFrame,
                                channelParams);
#if defined(_MSC_VER)
        wvPlanCycles_ += __rdtsc() - wvPlanBegin;
#endif
        if (planned) {
            lastRenderPaths_ |= 0x1u;
            ++pathBlocks_[0];
            wvHilbertData_ = hilbertData;
            RenderWholeVoiceBlock(voices, sampleData, sampleDataFrames,
                                  outputLeft, outputRight, numFrames,
                                  blockStartFrame, channelBusLeft,
                                  channelBusRight);
            return;
        }
    }
    // The plan refused only because a reservation failed (out of memory after
    // a late pool growth; PrepareWholeVoice reserves everything at init).  The
    // stretch is skipped.
    (void)vibratoActive;
    (void)correctnessMode;
}

// ── Whole-voice whole-block renderer ────────────────────────────────────

// Copy the render state of a displaced/killed voice into a ghost row.  Every
// dense field rides along (the macro keeps that set complete), plus the
// launch bookkeeping row (one cache line: channel, loopMode, the row-op
// inputs and the vibrato LFO state are what ghost rendering reads) and
// stealOutputGain.  birthFrame is voice-manager bookkeeping no ghost path
// touches.  A new SoA cold field that the ghost render path reads must be
// added here.
#define SVMS_COPY_VOICE_ROW_FIELD(type, name)     destination.name[destinationRow] = source.name[sourceRow];
inline void CopyGhostRow(const VoiceSoA& source, VoiceSoA& destination,
                         uint32_t sourceRow, uint32_t destinationRow) {
    SVMS_VOICE_SOA_DENSE_FIELDS(SVMS_COPY_VOICE_ROW_FIELD)
    destination.rows[destinationRow] = source.rows[sourceRow];
    destination.stealOutputGain[destinationRow] =
        source.stealOutputGain[sourceRow];
    if (source.rot != nullptr && destination.rot != nullptr)
        destination.rot[destinationRow] = source.rot[sourceRow];
}
#undef SVMS_COPY_VOICE_ROW_FIELD

// Remove a pending deferred-release op entry for a handle whose generation
// is dying (displacement overflow or an All Sound Off kill).  Block-end
// finalization must not observe the dead generation: the handle may already
// host a newer launch carrying its own release op, and a stale entry would
// double-count releasingCount_ once the new voice's own release lands.
// O(1) via wvOpIndex_: a handle holds at most one entry (the hook guards on
// wvReleaseFrame_), so this finds and swaps exactly the entry the former
// linear scan did — finalization order is unchanged.  The scan was O(pending
// releases) per steal, i.e. O(steals x pool) per block under chopped churn.
inline void RenderScalar::DropWholeVoiceReleaseOp(VoiceHandle handle) {
    const uint32_t i = wvOpIndex_[handle];
    if (i >= wvOpCount_ || wvOpHandles_[i] != handle) return;
    const uint32_t last = --wvOpCount_;
    if (i != last) {
        wvOpHandles_[i] = wvOpHandles_[last];
        wvOpFrames_[i] = wvOpFrames_[last];
        wvOpIndex_[wvOpHandles_[i]] = i;
    }
}

// Classify from raw render state; independent of the render-class lists so
// workers never touch shared scheduling metadata.
inline VoiceRenderClass ClassifyWholeVoiceRow(const VoiceSoA& v,
                                              uint32_t row) {
    if (v.state[row] == static_cast<uint8_t>(VoiceState::Free) ||
        v.sampleBacked[row] == 0u || v.relEnd[row] < 2u) {
        return VoiceRenderClass::Generic;
    }
    if (v.stealFadeInFramesRemaining[row] != 0u)
        return VoiceRenderClass::Generic;
    const bool loop = v.loopEnabled[row] != 0u;
    if (loop && (v.relLoopS[row] >= v.relLoopE[row] ||
                 v.relLoopE[row] > v.relEnd[row])) {
        return VoiceRenderClass::Generic;
    }
    if (v.state[row] == static_cast<uint8_t>(VoiceState::Releasing))
        return loop ? VoiceRenderClass::ReleaseLoop
                    : VoiceRenderClass::ReleaseOneShot;
    if (v.envelopeStage[row] == 3u)
        return loop ? VoiceRenderClass::SustainedLoop
                    : VoiceRenderClass::SustainedOneShot;
    if (v.envelopeStage[row] == 1u || v.envelopeStage[row] == 2u) {
        return loop ? VoiceRenderClass::TransientLoop
                    : VoiceRenderClass::TransientOneShot;
    }
    return VoiceRenderClass::Generic;
}

inline void RenderScalar::WholeVoiceVoiceConfiguredHook(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || handle >= renderer->wvHandleCapacity_) return;
    renderer->wvStartFrame_[handle] = renderer->wvEventFrame_;
    renderer->wvStartIngress_[handle] = renderer->wvEventOrdinal_;
}

inline void RenderScalar::WholeVoiceVoiceForkedHook(
    VoiceHandle twin, VoiceHandle source, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || twin >= renderer->wvHandleCapacity_ ||
        source >= renderer->wvHandleCapacity_) {
        return;
    }
    // The twin's row is an exact copy of the source's pre-render row, so
    // rendering it from the source's start reproduces the source's trajectory
    // up to the release frame, where it diverges onto the release envelope.
    // Inheriting the ingress ordinal keeps same-frame op qualification
    // identical for both halves.
    renderer->wvStartFrame_[twin] = renderer->wvStartFrame_[source];
    renderer->wvStartIngress_[twin] = renderer->wvStartIngress_[source];
    // The twin replays the source's trajectory up to its release, so it
    // must also hear the re-excite hits the source took so far.
    if (renderer->wvExciteHead_ != nullptr &&
        source < renderer->wvExciteCapacity_ &&
        twin < renderer->wvExciteCapacity_) {
        const VoiceSoA& rows = renderer->wvPlanVoices_->v;
        const uint32_t sourcePlay = rows.playIndex[source];
        const uint32_t twinPlay = rows.playIndex[twin];
        for (uint32_t i = renderer->WholeVoiceSkipForeignExcites(
                 renderer->wvExciteHead_[source], sourcePlay);
             i != UINT32_MAX;
             i = renderer->WholeVoiceSkipForeignExcites(
                 renderer->wvExcites_[i].next, sourcePlay)) {
            WholeVoiceExcite copy = renderer->wvExcites_[i];
            copy.playIndex = twinPlay;
            copy.next = UINT32_MAX;
            const uint32_t slot =
                static_cast<uint32_t>(renderer->wvExcites_.size());
            renderer->wvExcites_.push_back(copy);
            if (renderer->wvExciteHead_[twin] == UINT32_MAX) {
                renderer->wvExciteHead_[twin] = slot;
                renderer->wvExciteTouched_.push_back(twin);
            } else {
                renderer->wvExcites_[renderer->wvExciteTail_[twin]].next = slot;
            }
            renderer->wvExciteTail_[twin] = slot;
        }
    }
}

inline void RenderScalar::WholeVoiceReExciteHook(VoiceHandle handle,
                                                 void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || renderer->wvExciteHead_ == nullptr ||
        handle >= renderer->wvExciteCapacity_ ||
        renderer->wvPlanVoices_ == nullptr) {
        return;
    }
    VoiceManager& voices = *renderer->wvPlanVoices_;
    voices.EnsureMaterialized(handle);
    VoiceSoA& rows = voices.v;
    const uint32_t slot = static_cast<uint32_t>(renderer->wvExcites_.size());
    renderer->wvExcites_.push_back(
        {renderer->wvEventFrame_, rows.playIndex[handle], UINT32_MAX,
         rows.envelopeStage[handle], rows.attackSamplesRemaining[handle],
         rows.decaySamplesRemaining[handle], rows.attackGainStep[handle]});
    if (renderer->wvExciteHead_[handle] == UINT32_MAX) {
        renderer->wvExciteHead_[handle] = slot;
        renderer->wvExciteTouched_.push_back(handle);
    } else {
        renderer->wvExcites_[renderer->wvExciteTail_[handle]].next = slot;
    }
    renderer->wvExciteTail_[handle] = slot;
    // Steal scoring for the rest of the pre-pass sees the excited stage.
    const uint8_t stageBefore = rows.envelopeStage[handle];
    ExciteVoiceRow(rows, handle, voices.ReExciteAttackFrames());
    if (rows.envelopeStage[handle] != stageBefore)
        voices.RefreshRenderClass(handle);
}

inline bool RenderScalar::EnsureWholeVoiceExciteStorage(
    uint32_t handleCapacity) {
    if (wvExciteHead_ != nullptr && wvExciteTail_ != nullptr &&
        wvExciteCapacity_ >= handleCapacity) {
        return true;
    }
    auto* head = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(handleCapacity) * sizeof(uint32_t),
        kMixBufferAlign));
    auto* tail = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(handleCapacity) * sizeof(uint32_t),
        kMixBufferAlign));
    if (head == nullptr || tail == nullptr) {
        _aligned_free(head);
        _aligned_free(tail);
        return false;
    }
    std::fill(head, head + handleCapacity, UINT32_MAX);
    std::fill(tail, tail + handleCapacity, UINT32_MAX);
    _aligned_free(wvExciteHead_);
    _aligned_free(wvExciteTail_);
    wvExciteHead_ = head;
    wvExciteTail_ = tail;
    wvExciteCapacity_ = handleCapacity;
    wvExciteTouched_.clear();   // every list end was just reset
    return true;
}

inline uint32_t RenderScalar::WholeVoiceGhostLimit() const {
    return ghostBudget_ != 0u && ghostBudget_ < wvGhostCapacity_
        ? ghostBudget_ : wvGhostCapacity_;
}

inline void RenderScalar::WholeVoicePreTailCaptureHook(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || handle >= renderer->wvHandleCapacity_) return;
    static const bool envDrop = std::getenv("SVMS_DROP_ZERO_LIFE") != nullptr;
    const bool zeroLife = (envDrop ||
        g_dropZeroLifeVictims.load(std::memory_order_relaxed)) &&
        renderer->wvStartFrame_[handle] == renderer->wvEventFrame_;
    if (zeroLife ||
        renderer->wvGhostCount_ >= renderer->WholeVoiceGhostLimit()) {
        // Zero-life victim (opt-in) or pathological block (more steals than
        // the pool).  The displaced
        // voice's pre-steal samples are lost for this block; the launch
        // itself stays exact.  Counted for telemetry.
        if (!zeroLife) ++renderer->wvGhostOverflowCount_;
        // The displaced voice is destroyed without a ghost, so its pending
        // deferred release must not survive the displacement.  The handle is
        // immediately reused by the replacement launch, and a stale
        // wvReleaseFrame_ below the new start frame leaves the item loop in
        // WholeVoiceJobThunk unable to advance cursor (a release frame at or
        // below the cursor renders nothing and flips nothing) — a permanent
        // all-workers hang at saturated launch storms.  Drop the stale
        // release and swap its op entry out so block-end finalization cannot
        // observe the dead generation either.
        if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) {
            renderer->wvReleaseFrame_[handle] = UINT32_MAX;
            renderer->DropWholeVoiceReleaseOp(handle);
        }
        return;
    }
    const uint32_t ghost = renderer->wvGhostCount_++;
    CopyGhostRow(renderer->wvPlanVoices_->v, renderer->wvGhostState_,
                 handle, ghost);
    renderer->wvGhostStartFrame_[ghost] =
        renderer->wvStartFrame_[handle] == UINT32_MAX
            ? 0u : renderer->wvStartFrame_[handle];
    renderer->wvGhostDeathFrame_[ghost] = renderer->wvEventFrame_;
    renderer->wvGhostNoTail_[ghost] = 0u;
    // An in-place steal successor inherits this slot's rotation state, which
    // the oracle holds at the steal frame; the pre-pass only has the
    // block-start copy (the ghost renders the stretch in a worker).  Advance
    // the live slot by the samples the victim renders before it dies.
    {
        VoiceSoA& pool = renderer->wvPlanVoices_->v;
        if (pool.rot != nullptr && pool.sampleBacked[handle] != 0u &&
            renderer->wvEventFrame_ > renderer->wvGhostStartFrame_[ghost]) {
            AdvanceVoiceRotation(pool.rot[handle],
                renderer->wvEventFrame_ - renderer->wvGhostStartFrame_[ghost]);
        }
    }
    // Zero the tail record up front: if the ghost retires before its death
    // frame (envelope end mid-span) no CaptureGhostTail runs, and a record
    // left over from a previous use of this ghost slot must never be
    // adopted at block end.
    renderer->wvGhostTails_[ghost].framesRemaining = 0u;
    // The ghost inherits the victim's launch ordinal so same-frame op
    // qualification survives the displacement.
    renderer->wvGhostStartIngress_[ghost] = renderer->wvStartIngress_[handle];
    renderer->wvGhostHandle_[ghost] = handle;
    if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) {
        // A deferred release on the displaced voice transfers to the ghost.
        // The victim's op entry is dropped with the transfer: the victim is
        // dead, so block-end finalization has nothing left to observe, and
        // a kept entry would double-count if the reused handle releases
        // again later in the same block.
        renderer->wvGhostReleaseFrame_[ghost] =
            renderer->wvReleaseFrame_[handle];
        renderer->wvReleaseFrame_[handle] = UINT32_MAX;
        renderer->DropWholeVoiceReleaseOp(handle);
    } else {
        renderer->wvGhostReleaseFrame_[ghost] = UINT32_MAX;
    }
}

// CC120 (All Sound Off) hook, fired by VoiceManager::SilenceChannelImmediate
// for every channel voice just before the bookkeeping retires it.  The
// pre-kill row state becomes a ghost that renders [start, killFrame) —
// exactly the samples the frame-major renderers still hear — and then stops
// dead: no envelope release, no steal-tail fade.  The kill itself stays in
// VoiceManager, so the freed slot is reusable by later in-block launches
// exactly as live dispatch leaves it.
inline void RenderScalar::WholeVoiceSilenceVoiceHook(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || handle >= renderer->wvHandleCapacity_) return;
    if (renderer->wvGhostCount_ >= renderer->WholeVoiceGhostLimit()) {
        // Pathological block (more kills+steals than pool rows).  The killed
        // voice's pre-kill samples are lost for this block; the kill itself
        // stays exact.  Counted for telemetry.
        ++renderer->wvGhostOverflowCount_;
        if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) {
            renderer->wvReleaseFrame_[handle] = UINT32_MAX;
            renderer->DropWholeVoiceReleaseOp(handle);
        }
        return;
    }
    const uint32_t ghost = renderer->wvGhostCount_++;
    CopyGhostRow(renderer->wvPlanVoices_->v, renderer->wvGhostState_,
                 handle, ghost);
    renderer->wvGhostStartFrame_[ghost] =
        renderer->wvStartFrame_[handle] == UINT32_MAX
            ? 0u : renderer->wvStartFrame_[handle];
    renderer->wvGhostDeathFrame_[ghost] = renderer->wvEventFrame_;
    // All Sound Off is an immediate stop: no fade tail after the kill frame.
    renderer->wvGhostNoTail_[ghost] = 1u;
    renderer->wvGhostTails_[ghost].framesRemaining = 0u;
    // The kill ghost inherits the victim's launch ordinal so same-frame op
    // qualification survives the kill.
    renderer->wvGhostStartIngress_[ghost] = renderer->wvStartIngress_[handle];
    renderer->wvGhostHandle_[ghost] = handle;
    if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) {
        if (renderer->wvReleaseFrame_[handle] < renderer->wvEventFrame_) {
            // The release flip lands before the kill frame; the ghost keeps
            // rendering through it (sustained -> releasing) and dies on
            // schedule at the kill frame.
            renderer->wvGhostReleaseFrame_[ghost] =
                renderer->wvReleaseFrame_[handle];
        } else {
            // Killed before (or on) its release frame: the flip never
            // happens audibly.
            renderer->wvGhostReleaseFrame_[ghost] = UINT32_MAX;
        }
        // The handle dies here either way: drop the pending flip so a
        // reused handle cannot inherit it and block-end finalization cannot
        // double-count (same reasoning as the steal-transfer drop above).
        renderer->wvReleaseFrame_[handle] = UINT32_MAX;
        renderer->DropWholeVoiceReleaseOp(handle);
    } else {
        renderer->wvGhostReleaseFrame_[ghost] = UINT32_MAX;
    }
}

// CC120 hook for channel-owned steal tails, fired before the tail is
// unlinked.  Only the kill frame is recorded: UnlinkStealTail preserves the
// tail's fields, so RenderWholeVoiceBlock renders the tail's audio up to
// that frame and then stops exactly where live dispatch would have.
inline void RenderScalar::WholeVoiceSilenceTailHook(uint32_t slot,
                                                    void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || slot >= kStealTailReserve) return;
    if (renderer->wvTailKillFrame_[slot] == UINT32_MAX)
        renderer->wvTailKillFrame_[slot] = renderer->wvEventFrame_;
}

// CC7/10/11/121 mix-fold op: one log entry per marking event (per channel),
// not per voice — the owning worker recomputes each row's gains from its own
// per-voice gainLeft/gainRight at the op frame, with the exact
// FoldVoiceMixGains expressions (renderGain vintage = currentGain at the op
// frame, which the oracle's batch-boundary fold also observes).
inline void RenderScalar::WholeVoiceMixOpHook(uint8_t channel,
                                              float mixScaleLeft,
                                              float mixScaleRight,
                                              void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr) return;
    assert(renderer->wvRowOpCount_ < renderer->wvRowOpCapacity_ &&
           "row-op estimate undercounted a mix CC");
    if (renderer->wvRowOpCount_ >= renderer->wvRowOpCapacity_) return;
    WholeVoiceChannelOp& op = renderer->wvRowOps_[renderer->wvRowOpCount_++];
    op.frame = renderer->wvEventFrame_;
    op.ingress = renderer->wvEventOrdinal_;
    op.channel = channel;
    op.kind = 1u;
    op.a = mixScaleLeft;
    op.b = mixScaleRight;
}

// Pitch-bend op: the payload is the channel's sysex-aware semitone total
// plus the common (unscaled) ratio; the worker reapplies each row's own
// pitchBendScales exactly as VoiceManager::ApplyChannelBendRatio does
// inline outside the pre-pass.
inline void RenderScalar::WholeVoiceBendOpHook(uint8_t channel,
                                               float bendSemitones,
                                               float commonRatio,
                                               void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr) return;
    assert(renderer->wvRowOpCount_ < renderer->wvRowOpCapacity_ &&
           "row-op estimate undercounted a bend event");
    if (renderer->wvRowOpCount_ >= renderer->wvRowOpCapacity_) return;
    WholeVoiceChannelOp& op = renderer->wvRowOps_[renderer->wvRowOpCount_++];
    op.frame = renderer->wvEventFrame_;
    op.ingress = renderer->wvEventOrdinal_;
    op.channel = channel;
    op.kind = 0u;
    op.a = bendSemitones;
    op.b = commonRatio;
}

// Vibrato depth op (CC1 mod wheel / CC121 reset / channel pressure): the
// payload is the channel's post-rebuild modulation depth.  The worker
// updates its row-local depth cursor and performs the modulated ->
// unmodulated phaseIncs restore when the depth reaches zero; turning the
// LFO on needs no row write (the next segment's LFO window rebuilds the
// increment from base x bendRatio x lfoRatio).
inline void RenderScalar::WholeVoiceVibratoOpHook(uint8_t channel,
                                                  float modDepth,
                                                  void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr) return;
    assert(renderer->wvRowOpCount_ < renderer->wvRowOpCapacity_ &&
           "row-op estimate undercounted a vibrato depth change");
    if (renderer->wvRowOpCount_ >= renderer->wvRowOpCapacity_) return;
    WholeVoiceChannelOp& op = renderer->wvRowOps_[renderer->wvRowOpCount_++];
    op.frame = renderer->wvEventFrame_;
    op.ingress = renderer->wvEventOrdinal_;
    op.channel = channel;
    op.kind = 2u;
    op.a = modDepth;
    op.b = 0.0f;
}
