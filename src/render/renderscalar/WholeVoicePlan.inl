// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.

inline bool RenderScalar::EnsureWholeVoiceRowOpStorage(uint32_t opCount) {
    if (opCount == 0u) return true;
    if (wvRowOps_ != nullptr && wvChanOps_ != nullptr &&
        wvRowOpCapacity_ >= opCount) {
        return true;
    }
    auto* freshOps = static_cast<WholeVoiceChannelOp*>(_aligned_malloc(
        static_cast<size_t>(opCount) * sizeof(WholeVoiceChannelOp),
        kMixBufferAlign));
    auto* freshChan = static_cast<WholeVoiceChannelOp*>(_aligned_malloc(
        static_cast<size_t>(opCount) * sizeof(WholeVoiceChannelOp),
        kMixBufferAlign));
    if (freshOps == nullptr || freshChan == nullptr) {
        _aligned_free(freshOps);
        _aligned_free(freshChan);
        return false;
    }
    _aligned_free(wvRowOps_);
    _aligned_free(wvChanOps_);
    wvRowOps_ = freshOps;
    wvChanOps_ = freshChan;
    wvRowOpCapacity_ = opCount;
    return true;
}

// Apply one channel op to a single render row (real voice or ghost).
// Row-local only, so any worker can execute it.  The gain expressions
// mirror VoiceManager::FoldVoiceMixGains and the bend expression mirrors
// VoiceManager::ApplyChannelBendRatio bit-for-bit.
inline void RenderScalar::ApplyWholeVoiceRowOp(VoiceSoA& state, uint32_t row,
                                               const WholeVoiceChannelOp& op) {
    if (op.kind == 0u) {
        const float scale = state.pitchBendScales[row];
        const float ratio = scale == 1.0f
            ? op.b : powf(2.0f, op.a * scale / 12.0f);
        state.phaseIncs[row] = state.basePhaseIncs[row] * ratio;
        return;
    }
    state.mixGainL[row] = state.gainLeft[row] * op.a;
    state.mixGainR[row] = state.gainRight[row] * op.b;
    state.stealOutputGain[row] = std::sqrt(
        state.mixGainL[row] * state.mixGainL[row] +
        state.mixGainR[row] * state.mixGainR[row]);
    state.renderGainL[row] = state.currentGain[row] * state.mixGainL[row];
    state.renderGainR[row] = state.currentGain[row] * state.mixGainR[row];
}

inline void RenderScalar::WholeVoiceDeferredReleaseHook(
    VoiceHandle handle, void* userData) {
    RenderScalar* renderer = static_cast<RenderScalar*>(userData);
    if (renderer == nullptr || handle >= renderer->wvHandleCapacity_) return;
    if (renderer->wvReleaseFrame_[handle] != UINT32_MAX) return;
    // One deferred flip per voice per block: releases never re-target an
    // already-released generation because StartRelease unlinked its key
    // chain before this hook ran.
    renderer->wvReleaseFrame_[handle] = renderer->wvEventFrame_;
    renderer->wvOpIndex_[handle] = renderer->wvOpCount_;
    renderer->wvOpHandles_[renderer->wvOpCount_] = handle;
    renderer->wvOpFrames_[renderer->wvOpCount_] = renderer->wvEventFrame_;
    ++renderer->wvOpCount_;
}

inline bool RenderScalar::PlanWholeVoiceBlock(
    VoiceManager& voices, const RenderEvent* events, uint32_t eventCount,
    uint64_t blockStartFrame, const ChannelParamsSnapshot* channelParams) {
    // Rotation state lives in v.rot (per row) and the ghost/tail scratch
    // mirrors below; ReserveWholeVoiceStorage-era reservations are checked
    // with the other scratch further down.
    // Eligibility is decided before any state changes, so a refusal leaves the
    // voice pool and renderer untouched.
    bool hasEvents = false;
    bool hasReExcite = false;
    uint32_t rowOpEstimate = 0u;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        rowOpEstimate += WholeVoiceRowOpEstimate(events[i]);
        switch (events[i].type) {
            case RenderEventType::NoteReExcite:
                hasReExcite = true;
                [[fallthrough]];
            case RenderEventType::NoteOn:
            case RenderEventType::NoteOff:
            case RenderEventType::StaleNoteOffBatch:
                hasEvents = true;
                break;
            case RenderEventType::ControlChange: {
                const uint8_t controller = events[i].data1;
                switch (controller) {
                    case 1u:
                        // Mod wheel -> vibrato depth op (the driver reports
                        // the post-rebuild depth through the vibrato hook).
                        // The LFO itself advances inside the AVX2 kernels'
                        // 64-frame windows; see RenderSpanContext::lfoActive.
                        break;
                    default:
                    case 0u:
                    case 32u:
                        // Bank select: launch-time state only.  Unmapped
                        // controllers share this: they have no engine
                        // effect, so treating them as no-ops keeps CC-heavy
                        // material on this path without audio change.
                        break;
                    case 64u:
                    case 66u:
                    case 120u:
                    case 123u:
                        // Lifecycle: deferred-release / kill hooks.
                        break;
                    case 6u:
                    case 38u:
                    case 96u:
                    case 97u:
                        // RPN data wheel -> pitch-bend op.
                        break;
                    case 7u:
                    case 10u:
                    case 11u:
                        // Volume/pan/expression -> mix-gain fold op.
                        break;
                    case 121u:
                        // Reset All Controllers -> bend-center, mix fold AND
                        // mod-depth reset (channelModWheel_ clears).
                        break;
                }
                hasEvents = true;
                break;
            }
            case RenderEventType::PitchBend:
                hasEvents = true;
                break;
            case RenderEventType::ChannelPressure:
                // Channel aftertouch scales the vibrato depth via the SF2
                // default modulator -> vibrato depth op.
                hasEvents = true;
                break;
            case RenderEventType::ProgramChange:
            case RenderEventType::RhythmPart:
                // Launch-time state only: preset/kit selection is captured at
                // configure time and sounding voices are untouched (the same
                // category as bank select above).
                hasEvents = true;
                break;
            case RenderEventType::MasterVolume:
                // SysEx master volume: the driver marks every channel's mix
                // gains stale, which the pre-pass hooks turn into one
                // mix-gain op per channel.
                hasEvents = true;
                break;
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
                // SysEx master tune/transpose: RefreshAllPitchIncrements
                // rewrites every channel's phaseIncs, which the pre-pass
                // hooks turn into one bend op per channel.
                hasEvents = true;
                break;
            default:
                lastRefusalType_ = static_cast<uint8_t>(events[i].type);
                lastRefusalData1_ = events[i].data1;
                ++refusalCounts_[kRefuseEventType];
                assert(false && "barrier events are split out of the plan");
                return false;
        }
    }
    // No dispatcher (tests/bench only): events are ignored, voices render.
    // Launches dispatched below can grow the active set into free pool slots
    // (steals replace, free-slot launches add).  Worst case: one new voice per
    // event.  Reserve up front so every active-count-sized structure
    // (wvSliceHandles_, op/ghost arrays, retirement buffers) covers the
    // post-dispatch population; on failure the plan refuses cleanly (nothing
    // has been dispatched yet).
    //
    // The post-dispatch population is bounded by the PHYSICAL pool (steals
    // replace, free-slot launches cap out), never by activeCount + eventCount.
    // Reserving the pool size once — instead of active + events per dense
    // block — keeps every O(capacity) structure (ghost VoiceSoA, per-job
    // retirement scratch, dense mutation state) at pool size and lets the
    // early-out fire permanently after the first plan.
    if (voices.GetMaxVoices() > scratchCapacity_ &&
        !ReserveVoiceCapacity(voices.GetMaxVoices())) {
        ++refusalCounts_[kRefuseReservation];
        return false;
    }
    // Per-handle timeline arrays are indexed by raw voice handle, which in a
    // grown or partially-filled pool can be far above the active count.
    const uint32_t handleCapacity = voices.v.GetCapacity();
    if (!ReserveWholeVoiceStorage(handleCapacity)) {
        ++refusalCounts_[kRefuseReservation];
        return false;
    }
    if (!EnsureWholeVoiceJobScratch(
            workerPool_ ? workerPool_->GetThreadCount() * kWvJobsPerThread
                        : 1u) ||
        !SyncWholeVoiceRotationScratch(voices.v.rot != nullptr)) {
        ++refusalCounts_[kRefuseReservation];
        return false;
    }
    if (!EnsureWholeVoiceRowOpStorage(rowOpEstimate) ||
        (hasReExcite && !EnsureWholeVoiceExciteStorage(handleCapacity))) {
        ++refusalCounts_[kRefuseReservation];
        return false;
    }
    // Forget the previous block's hits (only the touched handles' list ends
    // are dirty).
    if (wvExciteHead_ != nullptr) {
        for (uint32_t touched : wvExciteTouched_) {
            if (touched < wvExciteCapacity_)
                wvExciteHead_[touched] = wvExciteTail_[touched] = UINT32_MAX;
        }
    }
    wvExciteTouched_.clear();
    wvExcites_.clear();

    std::fill(wvStartFrame_, wvStartFrame_ + handleCapacity, UINT32_MAX);
    std::fill(wvReleaseFrame_, wvReleaseFrame_ + handleCapacity, UINT32_MAX);
    std::fill(wvStartIngress_, wvStartIngress_ + handleCapacity, UINT32_MAX);
    std::fill(wvTailKillFrame_, wvTailKillFrame_ + kStealTailReserve,
              UINT32_MAX);
    wvOpCount_ = 0u;
    wvGhostCount_ = 0u;
    wvRowOpCount_ = 0u;
    wvPlanVoices_ = &voices;
    // Worker vibrato/bend cursors start from the block-START channel state
    // (captured before any dispatch mutates the live snapshot) and follow
    // the op log from there.
    if (channelParams != nullptr) {
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
            wvChanStartDepth_[channel] = channelParams[channel].modDepth;
            wvChanStartBendRatio_[channel] = channelParams[channel].bendRatio;
            wvChanStartBendCents_[channel] =
                channelParams[channel].pitchBendCents;
            wvChanVibratoActive_[channel] =
                channelParams[channel].modDepth > 0.0f;
        }
    } else {
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
            wvChanStartDepth_[channel] = 0.0f;
            wvChanStartBendRatio_[channel] = 0.0f;
            wvChanStartBendCents_[channel] = 0.0f;
            wvChanVibratoActive_[channel] = false;
        }
    }

    voices.SetDeferredReleaseHook(&WholeVoiceDeferredReleaseHook, this);
    voices.SetVoiceConfiguredHook(&WholeVoiceVoiceConfiguredHook, this);
    voices.SetVoiceForkedHook(&WholeVoiceVoiceForkedHook, this);
    voices.SetPreTailCaptureHook(&WholeVoicePreTailCaptureHook, this);
    voices.SetSilenceChannelHooks(&WholeVoiceSilenceVoiceHook,
                                  &WholeVoiceSilenceTailHook, this);
    voices.SetRowOpHooks(&WholeVoiceMixOpHook, &WholeVoiceBendOpHook,
                         &WholeVoiceVibratoOpHook, this);
    voices.SetStealTailCaptureSuppressed(true);
    if (hasReExcite)
        voices.SetReExciteHook(&WholeVoiceReExciteHook, this);
    // Launch staging (voices.launch_staging): in-place steal launches in this
    // window write a record the owning render item applies; no-op when the
    // pool was built without records.
    voices.SetLaunchStagingActive(true);

    // Ingress-order dispatch with the render clock at each event's exact
    // frame: birth ages, steal scores and every driver-side decision see
    // the same values they would see mid-block today.
    //
    // A maximal same-frame run of note events goes to the batch dispatcher
    // in one call, so the driver's exact-frame batching (launch-plan reuse,
    // adjacent note-off aggregation) engages here.
    // Every launch in the run is stamped with the run's first ordinal.  That
    // is exact for same-frame op qualification: ordinals are only ever
    // compared against row-op ordinals, row-op events never join a run, so
    // for any op ordinal o outside [begin, end) "o < i" == "o < begin".
    const auto isNoteEvent = [](RenderEventType type) {
        return type == RenderEventType::NoteOn ||
               type == RenderEventType::NoteOff ||
               type == RenderEventType::StaleNoteOffBatch;
    };
    for (uint32_t i = 0u; i < eventCount;) {
        const RenderEvent& event = events[i];
        wvEventFrame_ = event.frameOffset;
        wvEventOrdinal_ = i;
        voices.SetCurrentFrame(blockStartFrame + event.frameOffset);
        if (batchDispatcher_ != nullptr) {
            uint32_t end = i + 1u;
            if (isNoteEvent(event.type)) {
                while (end < eventCount &&
                       events[end].frameOffset == event.frameOffset &&
                       isNoteEvent(events[end].type)) {
                    ++end;
                }
            }
            batchDispatcher_(&event, end - i, event.frameOffset,
                             dispatcherUserData_);
            i = end;
        } else {
            if (dispatcher_ != nullptr)
                dispatcher_(event, event.frameOffset, dispatcherUserData_);
            ++i;
        }
    }

    voices.SetLaunchStagingActive(false);
    voices.SetDeferredReleaseHook(nullptr, nullptr);
    voices.SetVoiceConfiguredHook(nullptr, nullptr);
    voices.SetVoiceForkedHook(nullptr, nullptr);
    voices.SetPreTailCaptureHook(nullptr, nullptr);
    voices.SetSilenceChannelHooks(nullptr, nullptr, nullptr);
    voices.SetRowOpHooks(nullptr, nullptr, nullptr, nullptr);
    voices.SetReExciteHook(nullptr, nullptr);
    voices.SetStealTailCaptureSuppressed(false);
    wvPlanVoices_ = nullptr;

    // Regroup the op log per channel (stable → frame-sorted within each
    // channel) so worker items can binary-walk their own row's window.
    wvRowOpChannelMask_ = 0u;
    {
        uint32_t chanCounts[kChannelCount] = {};
        for (uint32_t i = 0u; i < wvRowOpCount_; ++i) {
            ++chanCounts[wvRowOps_[i].channel];
            // Only mix ops fold at block end: their per-channel scales live
            // in the VoiceManager cache (refreshed by every mark).  Bend ops
            // must NOT widen the fold mask — a channel whose cache was never
            // marked still holds its construction-time zero, and folding it
            // would zero every active row's mixGain with writeRenderGain
            // disabled.
            if (wvRowOps_[i].kind == 1u)
                wvRowOpChannelMask_ |= uint64_t(1) << wvRowOps_[i].channel;
            else if (wvRowOps_[i].kind == 2u)
                wvChanVibratoActive_[wvRowOps_[i].channel] = true;
        }
        uint32_t running = 0u;
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel) {
            wvChanOpStart_[channel] = running;
            running += chanCounts[channel];
        }
        wvChanOpStart_[kChannelCount] = running;
        wvChanOpStart_[kChannelCount + 1] = running;
        uint32_t writeCursor[kChannelCount];
        for (uint32_t channel = 0u; channel < kChannelCount; ++channel)
            writeCursor[channel] = wvChanOpStart_[channel];
        for (uint32_t i = 0u; i < wvRowOpCount_; ++i) {
            const WholeVoiceChannelOp& op = wvRowOps_[i];
            wvChanOps_[writeCursor[op.channel]++] = op;
        }
    }
    return true;
}

// Decimation leaves the mixed rows scattered through the active list, so the
// batch kernels (which need a run of consecutive rows) never fire and every
// kept voice pays full per-voice setup.  Re-lay the slice so that inside each
// job's range the mixed rows come first, in their original relative order,
// then the muted ones.  Muted rows add nothing to the mix, so the order the
// mixed rows accumulate in is unchanged (each job still mixes a contiguous
// piece of the mixed sequence), and the job ranges keep their sizes.
inline void RenderScalar::PartitionMutedRows(uint32_t activeCount,
                                             uint32_t itemCount,
                                             uint32_t jobCount) {
    uint32_t mixedTotal = 0u, mutedTotal = 0u;
    for (uint32_t i = 0u; i < activeCount; ++i) {
        const uint32_t handle = wvSliceHandles_[i];
        if (WholeVoiceRowMuted(handle))
            wvPartMuted_[mutedTotal++] = handle;
        else
            wvPartMixed_[mixedTotal++] = handle;
    }
    uint32_t mixedUsed = 0u, mutedUsed = 0u, mixedGiven = 0u;
    uint64_t voicesSoFar = 0u;
    for (uint32_t job = 0u; job < jobCount; ++job) {
        const uint32_t begin = static_cast<uint32_t>(
            static_cast<uint64_t>(itemCount) * job / jobCount);
        const uint32_t end = static_cast<uint32_t>(
            static_cast<uint64_t>(itemCount) * (job + 1u) / jobCount);
        const uint32_t vb = (std::min)(begin, activeCount);
        const uint32_t ve = (std::min)(end, activeCount);
        const uint32_t n = ve > vb ? ve - vb : 0u;
        voicesSoFar += n;
        const uint32_t target = static_cast<uint32_t>(
            static_cast<uint64_t>(mixedTotal) * voicesSoFar / activeCount);
        const uint32_t mixedHere = target - mixedGiven;
        mixedGiven = target;
        uint32_t pos = vb;
        for (uint32_t k = 0u; k < mixedHere; ++k)
            wvSliceHandles_[pos++] = wvPartMixed_[mixedUsed++];
        for (uint32_t k = mixedHere; k < n; ++k)
            wvSliceHandles_[pos++] = wvPartMuted_[mutedUsed++];
    }
}
