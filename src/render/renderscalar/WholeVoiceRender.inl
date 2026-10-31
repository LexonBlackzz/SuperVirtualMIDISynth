// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.

inline bool RenderScalar::RenderWholeVoiceSegment(
    VoiceSoA& state, VoiceManager* voices, uint32_t row,
    const int16_t* sampleData, uint32_t sampleDataFrames, float* outL,
    float* outR, uint32_t segStart, uint32_t segFrames, bool isReal,
    uint32_t jobIndex, float lfoDepth, float lfoBendRatio, bool lfoActive,
    const StepBreak* breaks, uint32_t breakCount) {
    // Per-segment rdtsc profiling is opt-in: this function runs once per
    // voice per op-split segment (tens of thousands of calls per block on
    // op-dense material), and six rdtsc per call measured ~+25% on every
    // whole-voice block. The cheap call/frame counters below stay on.
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
    const uint64_t segBegin = __rdtsc();
#endif
    uint64_t* segStats = wvJobSeg_[jobIndex].value;
    ++segStats[kWvSegCalls];
    SpanRetirement* retBuf = wvJobRetirements_ +
        static_cast<size_t>(jobIndex) * scratchCapacity_;
    const uint32_t retCountBefore = wvJobRetireCounts_[jobIndex * kWvRetireCountStride];
    // Fresh class at the segment start: compared against the class after
    // rendering to detect a lifecycle transition INSIDE this segment.
    const VoiceRenderClass classBefore = ClassifyWholeVoiceRow(state, row);
    RenderClassKernel kernel = kernelSet_->kernels[
        static_cast<uint32_t>(classBefore)];
    // A muted row (decimation) is never mixed, but a row launched this block
    // always is.
    const bool muted = isReal && wvDecimating_ && wvMuted_[row] != 0u &&
                       wvStartFrame_[row] == UINT32_MAX;
    // Pitch bends fused into this span (see StepBreak).  A path that cannot
    // take them renders the span in pieces, the step set before each one,
    // which is exactly what splitting at every bend did.
    const bool fused = breakCount != 0u;
    auto renderPieces = [&]() -> bool {
        uint32_t cursor = segStart;
        for (uint32_t i = 0u; i <= breakCount; ++i) {
            const uint32_t end = i < breakCount ? segStart + breaks[i].frame
                                                : segStart + segFrames;
            if (i > 0u) state.phaseIncs[row] = breaks[i - 1u].phaseInc;
            if (end > cursor &&
                RenderWholeVoiceSegment(state, voices, row, sampleData,
                                        sampleDataFrames, outL, outR, cursor,
                                        end - cursor, isReal, jobIndex,
                                        lfoDepth, lfoBendRatio, lfoActive,
                                        nullptr, 0u))
                return true;
            cursor = end;
        }
        return false;
    };
    if (fused && (muted || segFrames < 8u || kernel == nullptr ||
                  sampleData == nullptr))
        return renderPieces();
    if (!muted && segFrames >= 8u && kernel != nullptr &&
        sampleData != nullptr) {
        ++segStats[kWvSegKernelOk];
        segStats[kWvSegKernelFrames] += segFrames;
        RenderSpanContext context{
            &state, sampleData, wvHilbertData_, sampleDataFrames, outL, outR,
            segStart, segFrames, state.GetCapacity(),
            nullptr, nullptr,
            isReal ? voices->activePosition_ : nullptr,
            isReal ? retBuf : nullptr,
            isReal ? &wvJobRetireCounts_[jobIndex * kWvRetireCountStride] : nullptr,
            lfoDepth, lfoBendRatio, lfoActive ? 1u : 0u,
            nullptr, nullptr, breaks, breakCount};
        if (kernel(context, &row, 1u)) {
            // The kernel stepped across the bends; the row keeps the last one
            // it reached (a voice that retired mid-span never saw the later
            // ones, as when the span was split at every bend).
            if (fused) {
                uint32_t reached = breakCount;
                const uint32_t retiredNow =
                    wvJobRetireCounts_[jobIndex * kWvRetireCountStride];
                if (isReal && retiredNow > retCountBefore) {
                    const uint32_t at = retBuf[retiredNow - 1u].frameOffset;
                    reached = 0u;
                    while (reached < breakCount && breaks[reached].frame <= at)
                        ++reached;
                }
                if (reached != 0u)
                    state.phaseIncs[row] = breaks[reached - 1u].phaseInc;
            }
            if (isReal) {
                for (uint32_t i = retCountBefore;
                     i < wvJobRetireCounts_[jobIndex * kWvRetireCountStride]; ++i) {
                    retBuf[i].frameOffset += segStart;
                }
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
                segStats[kWvSegCycles] += __rdtsc() - segBegin;
#endif
                return wvJobRetireCounts_[jobIndex * kWvRetireCountStride] > retCountBefore;
            }
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
            segStats[kWvSegCycles] += __rdtsc() - segBegin;
#endif
            return false;
        }
    }
    if (fused) return renderPieces();   // the kernel refused (nothing mutated)
    ++segStats[kWvSegFallback];
    segStats[kWvSegFallbackFrames] += segFrames;
    if (lfoActive) {
        // Scalar fallback segments still honor the LFO: one whole-segment
        // window (advance-then-use + ratio rebuild) keeps the row's LFO
        // phase and modulated flag consistent for the next segment.
        AdvanceVibratoLfoWindow(state, row, segFrames, lfoDepth,
                                lfoBendRatio);
    }
    const uint32_t retiredAt = RenderPrimaryVoiceSpan(
        state, row, sampleData, wvHilbertData_, sampleDataFrames, outL, outR,
        segStart, segFrames, muted ? 0u : segFrames);
    if (retiredAt != UINT32_MAX) {
        if (isReal) {
            retBuf[wvJobRetireCounts_[jobIndex * kWvRetireCountStride]++] = {
                row, segStart + retiredAt, voices->activePosition_[row]};
        }
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
        segStats[kWvSegCycles] += __rdtsc() - segBegin;
#endif
        return true;
    }
    // Post-segment class transition bake, replicating the frame-major
    // oracle's per-frame envelope-stage check: when the lifecycle moved
    // inside this segment (transient→sustained crossing above all), the
    // oracle relinks the render class at the exact crossing frame and
    // LinkRenderClass bakes renderGainL/R = currentGain x mixGain with the
    // frozen sustain level — the value the sustained class kernels read.
    // The whole-voice path advances the same state inside its segments but
    // never relinks mid-block (the block-end RefreshRenderClass pass sees
    // field == desired — the launch already configured a sustained class —
    // and early-outs), so the bake must happen here at the crossing vintage.
    // Row-local writes only, so any worker can execute them.
    const VoiceRenderClass classAfter = ClassifyWholeVoiceRow(state, row);
    if (classAfter != classBefore &&
        (classAfter == VoiceRenderClass::SustainedLoop ||
         classAfter == VoiceRenderClass::SustainedOneShot)) {
        state.renderGainL[row] =
            state.currentGain[row] * state.mixGainL[row];
        state.renderGainR[row] =
            state.currentGain[row] * state.mixGainR[row];
    }
#if defined(SVMS_WV_SEGMENT_PROFILE) && defined(_MSC_VER)
    segStats[kWvSegCycles] += __rdtsc() - segBegin;
#endif
    return false;
}

// Render a run of real voices that already proved they have one identical
// segment for the rest of the block: same exact start frame and render class,
// with no release, row-op, or vibrato boundary.  The class kernels retain the
// input handle order, so scalar accumulation order and per-row state evolution
// are unchanged; this only amortizes classification, context construction,
// kernel dispatch, and retirement-offset fixup across the run.
// Scalar/SSE2 vibrato: split the segment at the block's global LFO window
// boundaries and refresh each voice's pitch increment per window, exactly as
// the shared AdvanceVibratoSpanRow does (advance-then-use, powf ratio).  AVX2 blocks pass
// straight through to the kernels' internal windows.
inline bool RenderScalar::RenderWholeVoiceSegmentLfo(
    VoiceSoA& state, VoiceManager* voices, uint32_t row,
    const int16_t* sampleData, uint32_t sampleDataFrames, float* outL,
    float* outR, uint32_t segStart, uint32_t segFrames, bool isReal,
    uint32_t jobIndex, float lfoDepth, float lfoBendRatio, bool lfoActive,
    const StepBreak* breaks, uint32_t breakCount) {
    // A row with no LFO (step 0) is untouched by every window, so it renders
    // as one segment; windows only matter for rows that carry a vibrato LFO.
    // (Step breaks only exist on AVX2 blocks, where wvScalarVibrato_ is off.)
    if (!wvScalarVibrato_ || state.vibLfoSteps[row] == 0.0f) {
        return RenderWholeVoiceSegment(state, voices, row, sampleData,
            sampleDataFrames, outL, outR, segStart, segFrames, isReal,
            jobIndex, lfoDepth, lfoBendRatio, lfoActive, breaks, breakCount);
    }
    const uint32_t end = segStart + segFrames;
    uint32_t pos = segStart;
    // Bounds are strictly increasing and pos only advances to a bound (or to
    // the segment end), so one search finds the first window and a plain
    // increment walks the rest.
    auto it = std::upper_bound(wvLfoBounds_.begin(), wvLfoBounds_.end(), pos);
    while (pos < end) {
        const uint32_t next =
            it == wvLfoBounds_.end() ? end : (std::min)(*it, end);
        if (it != wvLfoBounds_.end() && *it == next) ++it;
        AdvanceVibratoSpanRow(state, row, next - pos, lfoDepth,
                              lfoBendRatio);
        if (RenderWholeVoiceSegment(state, voices, row, sampleData,
                sampleDataFrames, outL, outR, pos, next - pos, isReal,
                jobIndex, lfoDepth, lfoBendRatio, false)) {
            return true;
        }
        pos = next;
    }
    return false;
}

inline bool RenderScalar::RenderWholeVoiceBatch(
    VoiceSoA& state, VoiceManager& voices, const uint32_t* handles,
    uint32_t handleCount, VoiceRenderClass renderClass,
    const int16_t* sampleData, uint32_t sampleDataFrames,
    const IndexedJobMix& mix, uint32_t segStart, uint32_t segFrames,
    uint32_t jobIndex) {
    RenderClassKernel kernel =
        kernelSet_->kernels[static_cast<uint32_t>(renderClass)];
    if (handleCount < 2u || segFrames < 8u || kernel == nullptr ||
        sampleData == nullptr) {
        return false;
    }
    SpanRetirement* retBuf = wvJobRetirements_ +
        static_cast<size_t>(jobIndex) * scratchCapacity_;
    const uint32_t retCountBefore = wvJobRetireCounts_[jobIndex * kWvRetireCountStride];
    RenderSpanContext context{
        &state, sampleData, wvHilbertData_, sampleDataFrames,
        mix.outputLeft, mix.outputRight, segStart, segFrames,
        state.GetCapacity(), nullptr, nullptr, voices.activePosition_,
        retBuf, &wvJobRetireCounts_[jobIndex * kWvRetireCountStride],
        0.0f, 1.0f, 0u, mix.busLeft, mix.busRight};
    if (!kernel(context, handles, handleCount)) return false;

    {
        uint64_t* stats = wvJobSeg_[jobIndex].value;
        stats[kWvSegCalls] += handleCount;
        stats[kWvSegKernelOk] += handleCount;
        stats[kWvSegKernelFrames] +=
            static_cast<uint64_t>(segFrames) * handleCount;
    }
    for (uint32_t i = retCountBefore;
         i < wvJobRetireCounts_[jobIndex * kWvRetireCountStride]; ++i) {
        retBuf[i].frameOffset += segStart;
    }
    for (uint32_t i = 0u; i < handleCount; ++i) {
        const uint32_t handle = handles[i];
        const VoiceRenderClass classAfter =
            ClassifyWholeVoiceRow(state, handle);
        if (classAfter != renderClass &&
            (classAfter == VoiceRenderClass::SustainedLoop ||
             classAfter == VoiceRenderClass::SustainedOneShot)) {
            state.renderGainL[handle] =
                state.currentGain[handle] * state.mixGainL[handle];
            state.renderGainR[handle] =
                state.currentGain[handle] * state.mixGainR[handle];
        }
    }
    return true;
}

// Reproduce CaptureStealTail's tail record from the ghost's exact
// steal-frame state.  Stored per ghost: the VoiceSoA steal-tail arrays are
// fixed kStealTailReserve slots, so ghost rows must never index them.
inline void RenderScalar::CaptureGhostTail(uint32_t ghost) {
    VoiceSoA& v = wvGhostState_;
    WholeVoiceGhostTail& tail = wvGhostTails_[ghost];
    tail.framesRemaining = 0u;
    if (v.sampleBacked[ghost] == 0u || v.relEnd[ghost] <= 1u) return;
    const float gain = v.currentGain[ghost];
    const float mixL = v.mixGainL[ghost];
    const float mixR = v.mixGainR[ghost];
    const float outgoingLevel =
        std::fabs(gain) * (std::fabs(mixL) + std::fabs(mixR));
    if (outgoingLevel <= VoiceRetireThreshold()) return;
    tail.phaseIdx = v.phaseIdx[ghost];
    tail.phaseFrac = v.phaseFrac[ghost];
    tail.phaseInc = v.phaseIncs[ghost];
    tail.gain = gain;
    tail.mixGainL = mixL;
    tail.mixGainR = mixR;
    tail.filterA0 = v.filterA0[ghost];
    tail.filterB1 = v.filterB1[ghost];
    tail.filterB2 = v.filterB2[ghost];
    tail.filterZ1 = v.filterZ1[ghost];
    tail.filterZ2 = v.filterZ2[ghost];
    tail.filterEnabled = static_cast<uint8_t>(v.filterEnabled[ghost] != 0u);
    tail.sampleStart = v.sampleStart[ghost];
    tail.relEnd = v.relEnd[ghost];
    tail.relLoopS = v.relLoopS[ghost];
    tail.relLoopE = v.relLoopE[ghost];
    tail.relLoopSF = v.relLoopSF[ghost];
    tail.relLoopEF = v.relLoopEF[ghost];
    tail.sampleBacked = v.sampleBacked[ghost];
    tail.loopEnabled = v.loopEnabled[ghost];
    tail.framesRemaining = kStealFadeFrames;
    tail.framesTotal = kStealFadeFrames;
    if (v.rot != nullptr) tail.rot = v.rot[ghost];
}

// Stage one ghost tail record into the job's single-row scratch pool and
// render its fade through the shared RenderStealTailSpan math.
inline void RenderScalar::RenderGhostTailSpan(uint32_t ghost, VoiceSoA& s,
                                              const int16_t* sampleData,
                                              uint32_t sampleDataFrames,
                                              float* outL, float* outR,
                                              uint32_t frameStart,
                                              uint32_t frameCount) {
    const WholeVoiceGhostTail& tail = wvGhostTails_[ghost];
    if (tail.framesRemaining == 0u) return;
    s.stealTailPhaseIdx[0] = tail.phaseIdx;
    s.stealTailPhaseFrac[0] = tail.phaseFrac;
    s.stealTailPhaseInc[0] = tail.phaseInc;
    s.stealTailGain[0] = tail.gain;
    s.stealTailMixGainL[0] = tail.mixGainL;
    s.stealTailMixGainR[0] = tail.mixGainR;
    s.stealTailSampleStart[0] = tail.sampleStart;
    s.stealTailRelEnd[0] = tail.relEnd;
    s.stealTailRelLoopS[0] = tail.relLoopS;
    s.stealTailRelLoopE[0] = tail.relLoopE;
    s.stealTailRelLoopSF[0] = tail.relLoopSF;
    s.stealTailRelLoopEF[0] = tail.relLoopEF;
    s.stealTailSampleBacked[0] = tail.sampleBacked;
    s.stealTailLoopEnabled[0] = tail.loopEnabled;
    s.stealTailFramesRemaining[0] = tail.framesRemaining;
    s.stealTailFramesTotal[0] = tail.framesTotal;
    if (s.rot != nullptr) s.stealTailRot[0] = tail.rot;
    // AVX2 takes the whole 8-frame chunks; the scalar span finishes the rest.
    uint32_t done = 0u;
    if (kernelSet_->stealTailSpan != nullptr && s.rot == nullptr) {
        done = kernelSet_->stealTailSpan(s, 0, sampleData, sampleDataFrames,
                                         outL, outR, frameStart, frameCount);
    }
    if (done < frameCount) {
        RenderStealTailSpan(s, 0, sampleData, wvHilbertData_, sampleDataFrames,
                            outL, outR, frameStart + done, frameCount - done);
    }
    wvGhostTails_[ghost].framesRemaining = s.stealTailFramesRemaining[0];
    // Rotation phase/angle advanced with the fade; adoption continues it.
    if (s.rot != nullptr) wvGhostTails_[ghost].rot = s.stealTailRot[0];
}

// One real voice's whole-block timeline on the CPU: the start frame, the
// exact-frame release flip, channel-op windows, re-excite hits and vibrato
// windows split it into segments, each rendered by the class kernel.  The
// worker jobs call it per item; the GPU stage calls it for the rows of a late
// block.  `jobIndex` picks the retirement buffer the row's retirements go to.
inline void RenderScalar::RenderWholeVoiceRow(
    VoiceManager& voices, uint32_t handle, const int16_t* sampleData,
    uint32_t sampleDataFrames, float* itemLeft, float* itemRight,
    uint32_t frameCount, uint32_t jobIndex) {
    VoiceSoA& v = voices.v;
    uint32_t start = wvStartFrame_[handle];
    if (start == UINT32_MAX) start = 0u;
    uint32_t releaseFrame = wvReleaseFrame_[handle];
    // Channel row-op window: bend/mix/vibrato ops at frames inside
    // the voice's timeline split it exactly where the frame-major
    // renderers fold/bend inline at their dispatch boundaries.
    const uint8_t opChannelRaw = v.channel[handle];
    const uint32_t opChannel =
        opChannelRaw < kChannelCount ? opChannelRaw : kChannelCount;
    uint32_t opIdx = wvChanOpStart_[opChannel];
    const uint32_t opEnd = wvChanOpStart_[opChannel + 1];
    // Per-row vibrato/bend cursors: depth and bend ratio start from
    // the block-start channel snapshot and follow the op log.  Ops
    // skipped below (before the voice's window, or dispatched
    // before its same-frame launch) are already baked into the
    // row's configure-time state — tracking them keeps the cursors
    // truthful for later LFO window rebuilds.
    float vDepth = wvChanStartDepth_[opChannel];
    float vRatio;
    {
        const float rowScale = v.pitchBendScales[handle];
        const float startRatio =
            wvChanStartBendRatio_[opChannel];
        vRatio = rowScale == 1.0f && startRatio > 0.0f
            ? startRatio
            : powf(2.0f, wvChanStartBendCents_[opChannel] *
                              rowScale / 1200.0f);
    }
    while (opIdx < opEnd &&
           wvChanOps_[opIdx].frame < start) {
        const WholeVoiceChannelOp& op =
            wvChanOps_[opIdx];
        if (op.kind == 2u) {
            vDepth = op.a;
        } else if (op.kind == 0u) {
            const float scale = v.pitchBendScales[handle];
            vRatio = scale == 1.0f ? op.b
                : powf(2.0f, op.a * scale / 12.0f);
        }
        ++opIdx;
    }
    // Same-frame qualification: an op dispatched in the same frame
    // BEFORE this voice's launch never observed it (the voice reads
    // the fresh channel state at configure time instead).  Carried
    // over voices (UINT32_MAX) hear every op in the block.
    const uint32_t startIngress = wvStartIngress_[handle];
    if (startIngress != UINT32_MAX) {
        while (opIdx < opEnd &&
               wvChanOps_[opIdx].frame == start &&
               wvChanOps_[opIdx].ingress < startIngress) {
            const WholeVoiceChannelOp& op =
                wvChanOps_[opIdx];
            if (op.kind == 2u) {
                vDepth = op.a;
            } else if (op.kind == 0u) {
                const float scale = v.pitchBendScales[handle];
                vRatio = scale == 1.0f ? op.b
                    : powf(2.0f, op.a * scale / 12.0f);
            }
            ++opIdx;
        }
    }
    // Re-excite hits on this generation, in frame order: each one
    // splits the timeline and restarts the row's envelope at its
    // exact frame, before the release flip a later-ordinal note-off
    // may land on the same frame (an earlier one would have unlinked
    // the voice from the key chain, so no hit exists for it).
    uint32_t exIdx = UINT32_MAX;
    const uint32_t rowPlayIndex = v.playIndex[handle];
    if (wvExciteHead_ != nullptr &&
        handle < wvExciteCapacity_) {
        exIdx = WholeVoiceSkipForeignExcites(
            wvExciteHead_[handle], rowPlayIndex);
    }
    const uint32_t exciteFrames =
        voices.ReExciteAttackFrames();
    if (exIdx != UINT32_MAX) {
        // Undo the pre-pass overlay: render from the block-start
        // envelope and replay each hit at its exact frame.
        const WholeVoiceExcite& first = wvExcites_[exIdx];
        v.envelopeStage[handle] = first.savedStage;
        v.attackSamplesRemaining[handle] = first.savedAttackRemaining;
        v.decaySamplesRemaining[handle] = first.savedDecayRemaining;
        v.attackGainStep[handle] = first.savedAttackStep;
    }
    // A hit that lands while the previous hit's attack ramp is still
    // running changes nothing (the stage is already attack), so it
    // neither splits the timeline nor needs replaying: a flood of
    // repeats costs one segment per ramp, not one per hit.
    uint32_t exSkipUntil = 0u;
    uint32_t cursor = start;
    while (cursor < frameCount) {
        while (exIdx != UINT32_MAX &&
               wvExcites_[exIdx].frame < exSkipUntil) {
            exIdx = WholeVoiceSkipForeignExcites(
                wvExcites_[exIdx].next, rowPlayIndex);
        }
        uint32_t stop = frameCount;
        if (releaseFrame != UINT32_MAX && releaseFrame < stop)
            stop = releaseFrame;
        if (opIdx < opEnd &&
            wvChanOps_[opIdx].frame < stop)
            stop = wvChanOps_[opIdx].frame;
        if (exIdx != UINT32_MAX &&
            wvExcites_[exIdx].frame < stop)
            stop = wvExcites_[exIdx].frame;
        if (stop > cursor) {
            // Vibrato gating per segment (only on channels where a
            // depth exists at block start or arrives mid-block): an
            // LFO-bearing row renders with kernel-internal 64-frame
            // windows while its channel depth is positive (the
            // kernels advance the free-running LFO phase); while
            // delayed or unmodulated the phase free-runs linearly
            // here instead.  A delay that exhausts mid-segment
            // splits the segment so the modulation wakes at its
            // exact frame.
            bool lfoActive = false;
            uint32_t segFrames = stop - cursor;
            if (!wvScalarVibrato_ && !wvScalarNoVibrato_ &&
                wvChanVibratoActive_[opChannel] &&
                v.vibLfoSteps[handle] != 0.0f &&
                v.vibLfoToPitchCents[handle] > 0.0f) {
                const uint32_t delay = v.vibLfoDelays[handle];
                if (delay != 0u) {
                    if (delay >= segFrames) {
                        v.vibLfoDelays[handle] = delay - segFrames;
                    } else {
                        v.vibLfoDelays[handle] = 0u;
                        stop = cursor + delay;
                        segFrames = delay;
                    }
                } else if (vDepth > 0.0f) {
                    lfoActive = true;
                } else {
                    float lp = v.vibLfoPhases[handle] +
                        v.vibLfoSteps[handle] *
                            static_cast<float>(segFrames);
                    lp -= std::floor(lp);
                    v.vibLfoPhases[handle] = lp;
                }
            }
            // Consecutive pitch bends ahead of the next hard boundary ride the
            // kernel as step breaks: one span instead of one per bend.  Only
            // where nothing else needs the span cut (no vibrato LFO on the
            // row), the kernels take breaks (AVX2) and the span is long enough
            // for the chunk path; everything else splits as before.
            StepBreak fuseBuffer[kWvMaxFusedBreaks];
            uint32_t fuseCount = 0u;
            uint32_t fuseOps = 0u;       // ops consumed by the fused bends
            float fuseRatio = vRatio;
            if (bendFusion_ && !lfoActive &&
                kernelSet_->backend == RenderBackend::AVX2 &&
                opIdx < opEnd && wvChanOps_[opIdx].kind == 0u &&
                wvChanOps_[opIdx].frame > cursor &&
                !(!wvScalarVibrato_ && !wvScalarNoVibrato_ &&
                  wvChanVibratoActive_[opChannel] &&
                  v.vibLfoSteps[handle] != 0.0f &&
                  v.vibLfoToPitchCents[handle] > 0.0f)) {
                // Hard boundary: the release flip, a re-excite or the block
                // end; a non-bend op also ends the fused run at its frame.
                uint32_t hardStop = frameCount;
                if (releaseFrame != UINT32_MAX && releaseFrame < hardStop)
                    hardStop = releaseFrame;
                if (exIdx != UINT32_MAX && wvExcites_[exIdx].frame < hardStop)
                    hardStop = wvExcites_[exIdx].frame;
                uint32_t runStop = hardStop;
                for (uint32_t j = opIdx; j < opEnd; ++j) {
                    if (wvChanOps_[j].frame >= hardStop) break;
                    if (wvChanOps_[j].kind != 0u) {
                        runStop = wvChanOps_[j].frame;
                        break;
                    }
                }
                const float scale = v.pitchBendScales[handle];
                uint32_t j = opIdx;
                for (; j < opEnd && wvChanOps_[j].frame < runStop &&
                       wvChanOps_[j].kind == 0u; ++j) {
                    const WholeVoiceChannelOp& op = wvChanOps_[j];
                    const uint32_t rel = op.frame - cursor;
                    const bool sameFrame =
                        fuseCount != 0u && fuseBuffer[fuseCount - 1u].frame == rel;
                    if (!sameFrame && fuseCount == kWvMaxFusedBreaks) {
                        runStop = op.frame;   // the next span starts at this bend
                        break;
                    }
                    fuseRatio = scale == 1.0f
                        ? op.b : powf(2.0f, op.a * scale / 12.0f);
                    const float inc = v.basePhaseIncs[handle] * fuseRatio;
                    StepBreak& slot = fuseBuffer[sameFrame ? fuseCount - 1u
                                                           : fuseCount++];
                    slot.frame = rel;
                    slot.phaseInc = inc;
                    slot.step = phasefx::FromFloat(inc);
                }
                if (fuseCount != 0u && runStop - cursor >= 8u) {
                    fuseOps = j - opIdx;
                    stop = runStop;
                    bendFusedSpans_.fetch_add(1u, std::memory_order_relaxed);
                } else {
                    fuseCount = 0u;   // too short to be worth it: split as before
                }
            }
            if (RenderWholeVoiceSegmentLfo(
                    v, &voices, handle, sampleData,
                    sampleDataFrames, itemLeft, itemRight,
                    cursor, stop - cursor, true, jobIndex,
                    vDepth, vRatio, lfoActive,
                    fuseCount != 0u ? fuseBuffer : nullptr, fuseCount)) {
                break;  // retired; coordinator applies it
            }
            if (fuseCount != 0u) {
                opIdx += fuseOps;
                vRatio = fuseRatio;
            }
            cursor = stop;
        }
        while (exIdx != UINT32_MAX &&
               wvExcites_[exIdx].frame <= cursor) {
            if (wvExcites_[exIdx].frame == cursor &&
                cursor >= exSkipUntil) {
                ExciteVoiceRow(v, handle, exciteFrames);
                if (v.envelopeStage[handle] == 1u &&
                    v.attackSamplesRemaining[handle] != 0u) {
                    exSkipUntil =
                        cursor + v.attackSamplesRemaining[handle];
                }
            }
            exIdx = WholeVoiceSkipForeignExcites(
                wvExcites_[exIdx].next, rowPlayIndex);
        }
        if (releaseFrame != UINT32_MAX && cursor == releaseFrame &&
            cursor < frameCount) {
            // Exact-frame deferred release flip.
            v.state[handle] =
                static_cast<uint8_t>(VoiceState::Releasing);
            if (v.loopMode[handle] == 3u)
                v.loopEnabled[handle] = 0u;
            releaseFrame = UINT32_MAX;
        }
        // Apply every op landing on this frame (ingress order).
        while (opIdx < opEnd &&
               wvChanOps_[opIdx].frame == cursor) {
            const WholeVoiceChannelOp& op =
                wvChanOps_[opIdx];
            if (op.kind == 2u) {
                vDepth = op.a;
                if (!(vDepth > 0.0f) &&
                    v.vibLfoModulated[handle] != 0u) {
                    // Modulation ended: restore the exact
                    // unmodulated increment once.
                    v.vibLfoModulated[handle] = 0u;
                    v.phaseIncs[handle] =
                        v.basePhaseIncs[handle] * vRatio;
                }
            } else {
                if (op.kind == 0u) {
                    const float scale = v.pitchBendScales[handle];
                    vRatio = scale == 1.0f ? op.b
                        : powf(2.0f, op.a * scale / 12.0f);
                }
                ApplyWholeVoiceRowOp(v, handle, op, vRatio,
                                     opIdx == wvChanLastMix_[opChannel]);
            }
            ++opIdx;
        }
    }
}

inline void RenderScalar::WholeVoiceJobThunk(uint32_t jobIndex,
                                             const IndexedJobMix& mix,
                                             uint32_t frameCount,
                                             void* userData) {
    WholeVoiceJobContext* ctx = static_cast<WholeVoiceJobContext*>(userData);
    RenderScalar* renderer = ctx->renderer;
    VoiceSoA& v = ctx->voices->v;
    const uint32_t itemCount = ctx->voiceItemCount + ctx->ghostItemCount;
    const uint32_t begin = itemCount * jobIndex / ctx->jobCount;
    const uint32_t end = itemCount * (jobIndex + 1u) / ctx->jobCount;
    renderer->wvJobRetireCounts_[jobIndex * kWvRetireCountStride] = 0u;
    // Launch staging: this job owns its rows, so it applies their records
    // before any dense-array read (including the batch look-ahead below).
    VoiceManager& poolVoices = *ctx->voices;
    const bool stagedPending = poolVoices.StagedPending();
    for (uint32_t item = begin; item < end; ++item) {
        if (item < ctx->voiceItemCount) {
            const uint32_t handle = renderer->wvItemHandles_[item];
            if (v.state[handle] == static_cast<uint8_t>(VoiceState::Free))
                continue;
            if (stagedPending && v.staged[handle] != 0u)
                poolVoices.MaterializeLaunch(handle);
            // Muted (decimated) carried-over row with one plain whole-block
            // segment: nothing mixes, no release/op/excite/vibrato boundary
            // can split it, so skip the per-item timeline setup (op window,
            // bend-ratio powf, excite walk, batch probing) and go straight
            // to the closed-form advance.  Same call the general path ends
            // in for this row, so state and retire frames are identical.
            if (renderer->wvDecimating_ && renderer->wvMuted_[handle] != 0u &&
                renderer->wvStartFrame_[handle] == UINT32_MAX &&
                renderer->wvReleaseFrame_[handle] == UINT32_MAX &&
                !renderer->wvScalarVibrato_) {
                const uint8_t fastChannelRaw = v.channel[handle];
                const uint32_t fastChannel = fastChannelRaw < kChannelCount
                    ? fastChannelRaw : kChannelCount;
                if (renderer->wvChanOpStart_[fastChannel] ==
                        renderer->wvChanOpStart_[fastChannel + 1u] &&
                    !renderer->wvChanVibratoActive_[fastChannel] &&
                    (renderer->wvExciteHead_ == nullptr ||
                     handle >= renderer->wvExciteCapacity_ ||
                     renderer->wvExciteHead_[handle] == UINT32_MAX)) {
                    if (renderer->wvCullBlocks_ != 0u &&
                        v.state[handle] ==
                            static_cast<uint8_t>(VoiceState::Releasing)) {
                        // Muted-tail cull: a run of consecutive muted blocks
                        // (block numbers start at 1, so 0 = never muted).
                        const uint32_t blk = renderer->wholeVoiceBlocks_;
                        const uint32_t lastMuted = renderer->wvMutedLast_[handle];
                        if (lastMuted == 0u || lastMuted + 1u != blk)
                            renderer->wvMutedSince_[handle] = blk;
                        renderer->wvMutedLast_[handle] = blk;
                        if (blk - renderer->wvMutedSince_[handle] >=
                            renderer->wvCullBlocks_) {
                            // Retire at frame 0 through the normal list; the
                            // row was never going to mix again this block.
                            renderer->wvJobRetirements_[
                                static_cast<size_t>(jobIndex) *
                                    renderer->scratchCapacity_ +
                                renderer->wvJobRetireCounts_[
                                    jobIndex * kWvRetireCountStride]++] = {
                                handle, 0u,
                                ctx->voices->activePosition_[handle]};
                            continue;
                        }
                    }
                    renderer->RenderWholeVoiceSegment(
                        v, ctx->voices, handle, ctx->sampleData,
                        ctx->sampleDataFrames, mix.outputLeft,
                        mix.outputRight, 0u, frameCount, true, jobIndex,
                        0.0f, 1.0f, false);
                    continue;
                }
            }
            // Common whole-block case: consecutive carried/new voices with
            // the same single segment can share one class-kernel dispatch.
            // Runs never cross a job boundary, and handles remain in active-
            // list order to preserve scalar mixing exactly.
            uint32_t batchStart = renderer->wvStartFrame_[handle];
            if (batchStart == UINT32_MAX) batchStart = 0u;
            const uint8_t batchChannelRaw = v.channel[handle];
            const uint32_t batchChannel = batchChannelRaw < kChannelCount
                ? batchChannelRaw : kChannelCount;
            const VoiceRenderClass batchClass =
                ClassifyWholeVoiceRow(v, handle);
            const uint32_t batchFilterEnabled = v.filterEnabled[handle];
            const bool batchHeadEligible = batchStart < frameCount &&
                frameCount - batchStart >= 8u &&
                ctx->sampleData != nullptr &&
                renderer->kernelSet_->kernels[
                    static_cast<uint32_t>(batchClass)] != nullptr &&
                renderer->wvReleaseFrame_[handle] == UINT32_MAX &&
                (renderer->wvExciteHead_ == nullptr ||
                 handle >= renderer->wvExciteCapacity_ ||
                 renderer->wvExciteHead_[handle] == UINT32_MAX) &&
                renderer->wvChanOpStart_[batchChannel] ==
                    renderer->wvChanOpStart_[batchChannel + 1u] &&
                !renderer->wvScalarVibrato_ &&
                !renderer->WholeVoiceRowMuted(handle) &&
                !renderer->wvChanVibratoActive_[batchChannel];
            if (batchHeadEligible) {
                uint32_t batchEnd = item + 1u;
                while (batchEnd < end &&
                       batchEnd < ctx->voiceItemCount &&
                       batchEnd - item < 256u) {
                    const uint32_t next =
                        renderer->wvItemHandles_[batchEnd];
                    if (stagedPending && v.state[next] !=
                            static_cast<uint8_t>(VoiceState::Free) &&
                        v.staged[next] != 0u)
                        poolVoices.MaterializeLaunch(next);
                    if (v.state[next] ==
                        static_cast<uint8_t>(VoiceState::Free) ||
                        renderer->WholeVoiceRowMuted(next) ||
                        renderer->wvReleaseFrame_[next] != UINT32_MAX ||
                        (renderer->wvExciteHead_ != nullptr &&
                         next < renderer->wvExciteCapacity_ &&
                         renderer->wvExciteHead_[next] != UINT32_MAX))
                        break;
                    uint32_t nextStart = renderer->wvStartFrame_[next];
                    if (nextStart == UINT32_MAX) nextStart = 0u;
                    const uint8_t nextChannelRaw = v.channel[next];
                    const uint32_t nextChannel =
                        nextChannelRaw < kChannelCount
                            ? nextChannelRaw : kChannelCount;
                    if (nextStart != batchStart ||
                        ClassifyWholeVoiceRow(v, next) != batchClass ||
                        v.filterEnabled[next] != batchFilterEnabled ||
                        renderer->wvChanOpStart_[nextChannel] !=
                            renderer->wvChanOpStart_[nextChannel + 1u] ||
                        renderer->wvChanVibratoActive_[nextChannel])
                        break;
                    ++batchEnd;
                }
                const uint32_t batchCount = batchEnd - item;
                if (renderer->RenderWholeVoiceBatch(
                        v, *ctx->voices,
                        renderer->wvItemHandles_ + item, batchCount,
                        batchClass, ctx->sampleData,
                        ctx->sampleDataFrames, mix, batchStart,
                        frameCount - batchStart, jobIndex)) {
                    item = batchEnd - 1u;
                    continue;
                }
            }
            // Bus mode: this item's channel plane is its mix destination.
            float* itemLeft = mix.outputLeft;
            float* itemRight = mix.outputRight;
            if (mix.busLeft != nullptr) {
                const uint32_t channel = v.channel[handle];
                const uint32_t safeChannel =
                    channel < kChannelCount ? channel : 0u;
                itemLeft = mix.busLeft[safeChannel];
                itemRight = mix.busRight[safeChannel];
            }
            renderer->RenderWholeVoiceRow(*ctx->voices, handle, ctx->sampleData,
                                ctx->sampleDataFrames, itemLeft, itemRight,
                                frameCount, jobIndex);
        } else {
            const uint32_t ghost = item - ctx->voiceItemCount;
            if (ghost >= renderer->wvGhostCount_) continue;
            const uint32_t start = renderer->wvGhostStartFrame_[ghost];
            const uint32_t death = renderer->wvGhostDeathFrame_[ghost];
            if (start >= frameCount) continue;
            renderer->EnsureGhostCopied(ghost);
            uint32_t releaseFrame = renderer->wvGhostReleaseFrame_[ghost];
            VoiceSoA& g = renderer->wvGhostState_;
            // Bus mode: the ghost's snapshot row carries its channel.
            float* itemLeft = mix.outputLeft;
            float* itemRight = mix.outputRight;
            if (mix.busLeft != nullptr) {
                const uint32_t channel = g.channel[ghost];
                const uint32_t safeChannel =
                    channel < kChannelCount ? channel : 0u;
                itemLeft = mix.busLeft[safeChannel];
                itemRight = mix.busRight[safeChannel];
            }
            // Ghosts walk the same channel ops within their own window, so
            // a displaced or killed voice hears the bend/fold events that
            // fired while it was still sounding.  Ops at (or past) the
            // death frame never apply — the voice is gone by then.
            const uint8_t opChannelRaw = g.channel[ghost];
            const uint32_t opChannel =
                opChannelRaw < kChannelCount ? opChannelRaw : kChannelCount;
            uint32_t opIdx = renderer->wvChanOpStart_[opChannel];
            const uint32_t opEnd = renderer->wvChanOpStart_[opChannel + 1];
            // Same per-row vibrato/bend cursors as real rows; skipped ops
            // are baked into the ghost's captured state and only update the
            // cursors.
            float vDepth = renderer->wvChanStartDepth_[opChannel];
            float vRatio;
            {
                const float rowScale = g.pitchBendScales[ghost];
                const float startRatio =
                    renderer->wvChanStartBendRatio_[opChannel];
                vRatio = rowScale == 1.0f && startRatio > 0.0f
                    ? startRatio
                    : powf(2.0f, renderer->wvChanStartBendCents_[opChannel] *
                                      rowScale / 1200.0f);
            }
            while (opIdx < opEnd &&
                   renderer->wvChanOps_[opIdx].frame < start) {
                const WholeVoiceChannelOp& op =
                    renderer->wvChanOps_[opIdx];
                if (op.kind == 2u) {
                    vDepth = op.a;
                } else if (op.kind == 0u) {
                    const float scale = g.pitchBendScales[ghost];
                    vRatio = scale == 1.0f ? op.b
                        : powf(2.0f, op.a * scale / 12.0f);
                }
                ++opIdx;
            }
            const uint32_t startIngress =
                renderer->wvGhostStartIngress_[ghost];
            if (startIngress != UINT32_MAX) {
                while (opIdx < opEnd &&
                       renderer->wvChanOps_[opIdx].frame == start &&
                       renderer->wvChanOps_[opIdx].ingress < startIngress) {
                    const WholeVoiceChannelOp& op =
                        renderer->wvChanOps_[opIdx];
                    if (op.kind == 2u) {
                        vDepth = op.a;
                    } else if (op.kind == 0u) {
                        const float scale = g.pitchBendScales[ghost];
                        vRatio = scale == 1.0f ? op.b
                            : powf(2.0f, op.a * scale / 12.0f);
                    }
                    ++opIdx;
                }
            }
            // Re-excite hits the ghost took while it was still sounding.
            uint32_t exIdx = UINT32_MAX;
            const uint32_t ghostPlayIndex = g.playIndex[ghost];
            const uint32_t ghostHandle = renderer->wvGhostHandle_[ghost];
            if (renderer->wvExciteHead_ != nullptr &&
                ghostHandle < renderer->wvExciteCapacity_) {
                exIdx = renderer->WholeVoiceSkipForeignExcites(
                    renderer->wvExciteHead_[ghostHandle], ghostPlayIndex);
            }
            const uint32_t exciteFrames =
                ctx->voices->ReExciteAttackFrames();
            if (exIdx != UINT32_MAX) {
                const WholeVoiceExcite& first = renderer->wvExcites_[exIdx];
                g.envelopeStage[ghost] = first.savedStage;
                g.attackSamplesRemaining[ghost] = first.savedAttackRemaining;
                g.decaySamplesRemaining[ghost] = first.savedDecayRemaining;
                g.attackGainStep[ghost] = first.savedAttackStep;
            }
            uint32_t exSkipUntil = 0u;   // see the real-voice loop
            uint32_t cursor = start;
            bool retired = false;
            while (cursor < death && cursor < frameCount) {
                while (exIdx != UINT32_MAX &&
                       renderer->wvExcites_[exIdx].frame < exSkipUntil) {
                    exIdx = renderer->WholeVoiceSkipForeignExcites(
                        renderer->wvExcites_[exIdx].next, ghostPlayIndex);
                }
                uint32_t stop = death < frameCount ? death : frameCount;
                if (releaseFrame != UINT32_MAX && releaseFrame < stop)
                    stop = releaseFrame;
                if (opIdx < opEnd &&
                    renderer->wvChanOps_[opIdx].frame < stop)
                    stop = renderer->wvChanOps_[opIdx].frame;
                if (exIdx != UINT32_MAX &&
                    renderer->wvExcites_[exIdx].frame < stop)
                    stop = renderer->wvExcites_[exIdx].frame;
                if (stop > cursor) {
                    bool lfoActive = false;
                    uint32_t segFrames = stop - cursor;
                    if (!renderer->wvScalarVibrato_ && !renderer->wvScalarNoVibrato_ &&
                        renderer->wvChanVibratoActive_[opChannel] &&
                        g.vibLfoSteps[ghost] != 0.0f &&
                        g.vibLfoToPitchCents[ghost] > 0.0f) {
                        const uint32_t delay = g.vibLfoDelays[ghost];
                        if (delay != 0u) {
                            if (delay >= segFrames) {
                                g.vibLfoDelays[ghost] = delay - segFrames;
                            } else {
                                g.vibLfoDelays[ghost] = 0u;
                                stop = cursor + delay;
                                segFrames = delay;
                            }
                        } else if (vDepth > 0.0f) {
                            lfoActive = true;
                        } else {
                            float lp = g.vibLfoPhases[ghost] +
                                g.vibLfoSteps[ghost] *
                                    static_cast<float>(segFrames);
                            lp -= std::floor(lp);
                            g.vibLfoPhases[ghost] = lp;
                        }
                    }
                    if (renderer->RenderWholeVoiceSegmentLfo(
                            g, nullptr, ghost, ctx->sampleData,
                            ctx->sampleDataFrames, itemLeft, itemRight,
                            cursor, stop - cursor, false, jobIndex,
                            vDepth, vRatio, lfoActive)) {
                        retired = true;
                        break;
                    }
                    cursor = stop;
                }
                while (exIdx != UINT32_MAX &&
                       renderer->wvExcites_[exIdx].frame <= cursor) {
                    if (renderer->wvExcites_[exIdx].frame == cursor &&
                        cursor < death && cursor >= exSkipUntil) {
                        ExciteVoiceRow(g, ghost, exciteFrames);
                        if (g.envelopeStage[ghost] == 1u &&
                            g.attackSamplesRemaining[ghost] != 0u) {
                            exSkipUntil =
                                cursor + g.attackSamplesRemaining[ghost];
                        }
                    }
                    exIdx = renderer->WholeVoiceSkipForeignExcites(
                        renderer->wvExcites_[exIdx].next, ghostPlayIndex);
                }
                if (releaseFrame != UINT32_MAX && cursor == releaseFrame) {
                    g.state[ghost] =
                        static_cast<uint8_t>(VoiceState::Releasing);
                    if (g.loopMode[ghost] == 3u)
                        g.loopEnabled[ghost] = 0u;
                    releaseFrame = UINT32_MAX;
                }
                while (opIdx < opEnd &&
                       renderer->wvChanOps_[opIdx].frame == cursor &&
                       cursor < death) {
                    const WholeVoiceChannelOp& op =
                        renderer->wvChanOps_[opIdx];
                    if (op.kind == 2u) {
                        vDepth = op.a;
                        if (!(vDepth > 0.0f) &&
                            g.vibLfoModulated[ghost] != 0u) {
                            g.vibLfoModulated[ghost] = 0u;
                            g.phaseIncs[ghost] =
                                g.basePhaseIncs[ghost] * vRatio;
                        }
                    } else {
                        if (op.kind == 0u) {
                            const float scale = g.pitchBendScales[ghost];
                            vRatio = scale == 1.0f ? op.b
                                : powf(2.0f, op.a * scale / 12.0f);
                        }
                        renderer->ApplyWholeVoiceRowOp(g, ghost, op, vRatio,
                                                       true);
                    }
                    ++opIdx;
                }
            }
            if (retired || cursor < death) continue;  // ghost ended early
            if (renderer->wvGhostNoTail_[ghost]) {
                // All Sound Off kill reached: hard stop, no fade tail.
                // (The tail record was zeroed at capture; nothing to adopt.)
                continue;
            }
            // Steal frame reached: reproduce the 64-frame tail fade from
            // the ghost's exact steal-frame state, then keep rendering it.
            renderer->CaptureGhostTail(ghost);
            renderer->RenderGhostTailSpan(ghost,
                                          renderer->wvTailScratch_[jobIndex],
                                          ctx->sampleData,
                                          ctx->sampleDataFrames, itemLeft,
                                          itemRight, death,
                                          frameCount - death);
        }
    }
}

inline void RenderScalar::RenderWholeVoiceBlock(
    VoiceManager& voices, const int16_t* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t numFrames, uint64_t blockStartFrame,
    float* const* channelBusLeft, float* const* channelBusRight) {
    const uint32_t activeCount = voices.GetActiveCount();
    ++wholeVoiceBlocks_;
    std::memcpy(wvSliceHandles_, voices.activeList_,
                static_cast<size_t>(activeCount) * sizeof(uint32_t));
    // Decimation: rank the active voices by velocity once and mute the
    // quietest beyond activeCount / step for this block.  Muted rows keep
    // their state advancing (scalar span with nothing mixed), so they retire
    // and resume exactly as they would unmuted.
    wvDecimating_ = false;
    wvCullBlocks_ = mutedCullBlocks_.load(std::memory_order_relaxed);
    if (wvMutedSince_.size() < wvMuted_.size()) wvCullBlocks_ = 0u;
    if (wvMutedDirty_) {
        std::fill(wvMuted_.begin(), wvMuted_.end(), uint8_t(0));
        wvMutedDirty_ = false;
    }
    if (activeCount > 1u) {
        const uint32_t step = DecimationStep(activeCount);
        const uint32_t keep = activeCount / step;
        if (step > 1u && keep < activeCount &&
            wvMuted_.size() >= voices.v.GetCapacity()) {
            const VoiceSoA& rows = voices.v;
            // Keep the `keep` loudest voices, ties broken toward the lower
            // handle. A velocity histogram finds the threshold velocity in one
            // pass; only the voices sitting exactly on it need an order. The
            // muted set is identical to a full nth_element ranking by
            // (velocity desc, handle asc), at about a quarter of the cost.
            // Rank key, 0..255, larger = louder.  Velocity by default; with
            // by-level ranking, the float bits of |envelope level| x output
            // gain (monotone for positive floats) at 1/8-octave resolution
            // over 2^-32..1.  Pre-decay voices rank by their target gain, as
            // the steal score does.
            const bool byLevel =
                decimationByLevel_.load(std::memory_order_relaxed);
            auto rankKey = [&](uint32_t handle) -> uint32_t {
                if (!byLevel) return rows.velocity[handle];
                const uint8_t stage = rows.envelopeStage[handle];
                const bool preDecay =
                    rows.state[handle] ==
                        static_cast<uint8_t>(VoiceState::Active) &&
                    (stage == 4u || stage == 0u || stage == 1u);
                const float level =
                    std::fabs(preDecay ? rows.targetGain[handle]
                                       : rows.currentGain[handle]) *
                    rows.stealOutputGain[handle];
                uint32_t bits;
                std::memcpy(&bits, &level, sizeof(bits));
                const uint32_t key = bits >> 20;
                return key <= 760u ? 0u : (key >= 1015u ? 255u : key - 760u);
            };
            uint32_t histogram[256] = {};
            if (byLevel) {
                // The key reads five scattered SoA arrays per voice: compute
                // it once (prefetching ahead) and replay it sequentially.
                wvDecimKey_.resize(activeCount);
                for (uint32_t i = 0u; i < activeCount; ++i) {
#if defined(SVMS_VM_HAS_PREFETCH)
                    if (i + 16u < activeCount) {
                        const uint32_t ahead = wvSliceHandles_[i + 16u];
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.currentGain[ahead]), _MM_HINT_T0);
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.stealOutputGain[ahead]), _MM_HINT_T0);
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.targetGain[ahead]), _MM_HINT_T0);
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.envelopeStage[ahead]), _MM_HINT_T0);
                        _mm_prefetch(reinterpret_cast<const char*>(
                                         &rows.state[ahead]), _MM_HINT_T0);
                    }
#endif
                    wvDecimKey_[i] =
                        static_cast<uint8_t>(rankKey(wvSliceHandles_[i]));
                    ++histogram[wvDecimKey_[i]];
                }
            } else {
                for (uint32_t i = 0u; i < activeCount; ++i)
                    ++histogram[rows.velocity[wvSliceHandles_[i]]];
            }
            uint32_t above = 0u;
            uint32_t threshold = 255u;
            for (;; --threshold) {
                if (above + histogram[threshold] >= keep || threshold == 0u)
                    break;
                above += histogram[threshold];
            }
            const uint32_t tiesKept = keep - above;
            wvDecimRank_.clear();
            // Even spread of the kept ties (Bresenham over the tie sequence):
            // the render jobs split the active list into contiguous slices,
            // so keeping the first N would pile all the work onto one job.
            const uint32_t levelTieTotal = histogram[threshold];
            uint64_t levelTieSeen = 0u;
            for (uint32_t i = 0u; i < activeCount; ++i) {
                const uint32_t handle = wvSliceHandles_[i];
                const uint32_t velocity =
                    byLevel ? wvDecimKey_[i] : rows.velocity[handle];
                if (velocity < threshold)
                    wvMuted_[handle] = 1u;
                else if (velocity == threshold) {
                    // Level bins are coarse and real pools tie heavily, so
                    // by-level ranking picks its kept ties by an even,
                    // deterministic spread over the list (O(n)) instead of
                    // ordering the tie bin by handle.
                    if (!byLevel) {
                        wvDecimRank_.push_back(handle);
                    } else {
                        const uint64_t before =
                            levelTieSeen * tiesKept / levelTieTotal;
                        ++levelTieSeen;
                        if (levelTieSeen * tiesKept / levelTieTotal == before)
                            wvMuted_[handle] = 1u;
                    }
                }
            }
            if (tiesKept < wvDecimRank_.size()) {
                std::nth_element(wvDecimRank_.begin(),
                                 wvDecimRank_.begin() + tiesKept,
                                 wvDecimRank_.end());
                for (size_t k = tiesKept; k < wvDecimRank_.size(); ++k)
                    wvMuted_[wvDecimRank_[k]] = 1u;
            }
            wvDecimating_ = true;
            wvMutedDirty_ = true;
        }
    }
    const uint32_t ghostCount = wvGhostCount_;
    wvItemHandles_ = wvSliceHandles_;   // the CPU job stage's rows (the GPU stage may substitute its CPU share)
    // GPU stage: eligible sustained rows move to the tail of wvSliceHandles_
    // and render on the GPU while the pool renders the rest.
    uint32_t gpuCount = 0u;
    bool gpuSubmitted = false;
#if defined(SVMS_GPU_STAGE)
    if (gpuStage_ != nullptr) {
        gpuCount = TryGpuSubmit(voices, sampleData, sampleDataFrames,
                                outputLeft, outputRight, numFrames,
                                activeCount, channelBusLeft, gpuSubmitted);
    }
#else
    (void)gpuSubmitted;
#endif
    const uint32_t cpuVoiceCount = activeCount - gpuCount;
    const uint32_t itemCount = cpuVoiceCount + ghostCount;
    WholeVoiceJobContext ctx{this, &voices, sampleData, sampleDataFrames,
                             numFrames, cpuVoiceCount, ghostCount, 1u};
    // Retire-stage input must be zeroed for THIS block: empty blocks
    // (itemCount == 0) dispatch no thunk, so every count slot they will read
    // has to start at zero (thunks re-zero their own slot when they run).
    if (wvJobRetireCounts_ != nullptr && wvJobCapacity_ != 0u)
        std::fill(wvJobRetireCounts_,
                  wvJobRetireCounts_ +
                      static_cast<size_t>(wvJobCapacity_) * kWvRetireCountStride,
                  0u);
    if (itemCount != 0u) {
        // Oversubscribe the pool (jobs are claimed from an atomic queue):
        // one static slice per thread left the slowest core (E-core, noisy
        // neighbour) as the critical path while fast cores idled.  Merge and
        // retirement order are per-job-index, so assignment does not matter.
        uint32_t jobCount = 1u;
        if (workerPool_) {
            const uint32_t threads = workerPool_->GetThreadCount();
            jobCount = (std::min)(threads, itemCount);
            const uint32_t fine = (std::min)(
                (std::min)(threads * kWvJobsPerThread,
                           workerPool_->GetJobCapacity()),
                itemCount / kWvMinItemsPerJob);
            if (fine > jobCount) jobCount = fine;
        }
        if (jobCount < 2u) jobCount = 1u;
        ctx.jobCount = jobCount;
        if (wvDecimating_ && activeCount > 1u &&
            wvPartMixed_.size() >= activeCount)
            PartitionMutedRows(activeCount, itemCount, jobCount);
#if defined(_MSC_VER)
        const uint64_t wvJobBegin = __rdtsc();
#endif
        bool dispatched = false;
        if (jobCount >= 2u && workerPool_ != nullptr) {
            dispatched = workerPool_->ExecuteIndexed(
                jobCount, numFrames, outputLeft, outputRight,
                &WholeVoiceJobThunk, &ctx, channelBusLeft, channelBusRight);
        }
        ++(dispatched ? wvParallelJobs_ : wvSerialJobs_);
        if (!dispatched) {
            // Serial: one job over every item, mixing directly.  In bus
            // mode the serial job writes the block buses (sequential, so
            // no per-job planes are needed).
            ctx.jobCount = 1u;
            IndexedJobMix serialMix{outputLeft, outputRight,
                                    channelBusLeft, channelBusRight};
            WholeVoiceJobThunk(0u, serialMix, numFrames, &ctx);
        }
#if defined(_MSC_VER)
        wvJobCycles_ += __rdtsc() - wvJobBegin;
#endif
    }
#if defined(SVMS_GPU_STAGE)
    if (gpuCount != 0u) {
        FinishGpuRows(voices, sampleData, sampleDataFrames, outputLeft,
                      outputRight, numFrames, gpuCount, cpuVoiceCount,
                      gpuSubmitted);
    }
#endif

    // Every active row went through a job item, so no launch record is left
    // to apply.  Test builds verify that (and repair a leak so a bug shows as
    // a counter, not as a stale row).
    if (voices.StagedPending()) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (voices.CountStagedRows() != 0u) {
            ++wvStagedLeaks_;
            g_launchStagingLeaks.fetch_add(1u, std::memory_order_relaxed);
            for (uint32_t i = 0u; i < activeCount; ++i)
                voices.EnsureMaterialized(
                    static_cast<VoiceHandle>(wvSliceHandles_[i]));
        }
#endif
        voices.ClearStagedPending();
    }
    voices.SetDeferredGhostFlush(nullptr, 1u, nullptr, nullptr);
    wvDeferredPool_ = nullptr;

    // Retirement in exact (frame, capture-position) order — identical to
    // the frame-major oracle's inline retirement.
    uint32_t retireTotal = 0u;
    for (uint32_t job = 0u; job < ctx.jobCount; ++job) {
        const uint32_t count = wvJobRetireCounts_[job * kWvRetireCountStride];
        std::memcpy(retirements_ + retireTotal,
                    wvJobRetirements_ + static_cast<size_t>(job) *
                        scratchCapacity_,
                    static_cast<size_t>(count) * sizeof(SpanRetirement));
        retireTotal += count;
    }
    std::sort(retirements_, retirements_ + retireTotal,
        [](const SpanRetirement& a, const SpanRetirement& b) {
            if (a.frameOffset != b.frameOffset)
                return a.frameOffset < b.frameOffset;
            return a.capturePosition < b.capturePosition;
        });
    for (uint32_t i = 0u; i < retireTotal; ++i) {
        voices.SetCurrentFrame(blockStartFrame + retirements_[i].frameOffset);
        const VoiceHandle retiring =
            static_cast<VoiceHandle>(retirements_[i].handle);
        // A voice released and retired inside this block flipped to
        // Releasing in the worker, but its releasing-counter increment is
        // still pending in the deferred-release list.  Finalize it first so
        // RetireVoice's decrement balances (else the counter underflows).
        const uint32_t opSlot = wvOpIndex_[retiring];
        if (opSlot < wvOpCount_ && wvOpHandles_[opSlot] == retiring)
            voices.FinalizeDeferredRelease(retiring);
        voices.RetireVoice(retiring);
    }

#if defined(_MSC_VER)
    const uint64_t wvPostBegin = __rdtsc();
#endif
    // Deferred release flips: counters, Releasing-ring and class lists.
    for (uint32_t i = 0u; i < wvOpCount_; ++i)
        voices.FinalizeDeferredRelease(
            static_cast<VoiceHandle>(wvOpHandles_[i]));

    // Render-class lists track lifecycle (transient→sustained crossings
    // included).
#if defined(SVMS_GPU_STAGE)
    // GPU rows keep their class unless the kernel moved them across a stage.
    const uint32_t refreshCount = gpuSkipRefresh_ != 0u ? cpuVoiceCount : activeCount;
    const uint32_t* refreshRows = gpuSkipRefresh_ != 0u ? wvItemHandles_ : wvSliceHandles_;
    for (uint32_t handle : gpuRefreshHandles_)
        voices.RefreshRenderClass(static_cast<VoiceHandle>(handle));
#else
    const uint32_t refreshCount = activeCount;
    const uint32_t* refreshRows = wvSliceHandles_;
#endif
    for (uint32_t i = 0u; i < refreshCount; ++i)
        voices.RefreshRenderClass(static_cast<VoiceHandle>(refreshRows[i]));

    // Row-op channels had no masks set during the pre-pass (a launch-entry
    // steal fold would have corrupted pre-CC ghost captures), so refresh
    // their steal candidates here with writeRenderGain=false: the workers
    // already wrote mixGain/renderGain at the op frames with the exact
    // currentGain vintage the oracle's batch-boundary folds produce, and
    // the scales have not moved since — the value writes are idempotent,
    // only the cached candidate keys move.
    voices.FoldStealGainsForChannels(wvRowOpChannelMask_);

    // Pre-existing steal tails (born before this block) render exactly as
    // the oracle's tail section does.  A tail killed by an in-block
    // All Sound Off was unlinked during the pre-pass, but UnlinkStealTail
    // preserves its fields: render the audio up to the recorded kill frame,
    // then stop exactly where live dispatch would have stopped it.
#if defined(_MSC_VER)
    wvPostCycles_ += __rdtsc() - wvPostBegin;
#endif
    const uint32_t voiceCapacity = voices.GetMaxVoices();
    const uint32_t tailCapacity =
        (std::min)(voiceCapacity, static_cast<uint32_t>(kStealTailReserve));
    for (uint32_t slot = 0u; slot < tailCapacity; ++slot) {
        const uint32_t killFrame = wvTailKillFrame_[slot];
        float* tailOutLeft = outputLeft;
        float* tailOutRight = outputRight;
        if (channelBusLeft != nullptr) {
            const uint32_t channel = voices.v.stealTailChannel[slot];
            const uint32_t safeChannel =
                channel < kChannelCount ? channel : 0u;
            tailOutLeft = channelBusLeft[safeChannel];
            tailOutRight = channelBusRight[safeChannel];
        }
        if (voices.v.stealTailFramesRemaining[slot] == 0u) {
            if (killFrame == UINT32_MAX) continue;
            RenderStealTailSpan(voices.v, slot, sampleData, wvHilbertData_,
                                sampleDataFrames,
                                tailOutLeft, tailOutRight, 0u, killFrame);
            continue;
        }
        // A live tail never carries a kill frame; the min() only defends
        // against a plan/render mismatch.
        const uint32_t tailFrames = killFrame == UINT32_MAX
            ? numFrames : (std::min)(killFrame, numFrames);
        RenderStealTailSpan(voices.v, slot, sampleData, wvHilbertData_,
                            sampleDataFrames,
                            tailOutLeft, tailOutRight, 0u, tailFrames);
        voices.RefreshStealTail(static_cast<VoiceHandle>(slot));
    }

    // Ghost tails that outlive the block are adopted into real tail slots
    // so their fade continues into the next block exactly as an in-band
    // capture would.
    // Adoption never frees a slot, so every slot below the last one taken is
    // known to be busy: the scan resumes there instead of restarting at 0
    // (that restart made a storm of surviving tails quadratic).
    uint32_t firstFreeTailSlot = 0u;
    for (uint32_t ghost = 0u; ghost < ghostCount; ++ghost) {
        const WholeVoiceGhostTail& tail = wvGhostTails_[ghost];
        if (tail.framesRemaining == 0u) continue;
        if (wvGhostDeathFrame_[ghost] >= numFrames) continue;
        for (uint32_t slot = firstFreeTailSlot; slot < tailCapacity; ++slot) {
            if (voices.v.stealTailFramesRemaining[slot] != 0u) continue;
            voices.v.stealTailPhaseIdx[slot] = tail.phaseIdx;
            voices.v.stealTailPhaseFrac[slot] = tail.phaseFrac;
            voices.v.stealTailPhaseInc[slot] = tail.phaseInc;
            voices.v.stealTailGain[slot] = tail.gain;
            voices.v.stealTailMixGainL[slot] = tail.mixGainL;
            voices.v.stealTailMixGainR[slot] = tail.mixGainR;
            voices.v.stealTailFilterA0[slot] = tail.filterA0;
            voices.v.stealTailFilterB1[slot] = tail.filterB1;
            voices.v.stealTailFilterB2[slot] = tail.filterB2;
            voices.v.stealTailFilterZ1[slot] = tail.filterZ1;
            voices.v.stealTailFilterZ2[slot] = tail.filterZ2;
            voices.v.stealTailFilterEnabled[slot] = tail.filterEnabled;
            voices.v.stealTailSampleStart[slot] = tail.sampleStart;
            voices.v.stealTailRelEnd[slot] = tail.relEnd;
            voices.v.stealTailRelLoopS[slot] = tail.relLoopS;
            voices.v.stealTailRelLoopE[slot] = tail.relLoopE;
            voices.v.stealTailRelLoopSF[slot] = tail.relLoopSF;
            voices.v.stealTailRelLoopEF[slot] = tail.relLoopEF;
            voices.v.stealTailSampleBacked[slot] = tail.sampleBacked;
            voices.v.stealTailLoopEnabled[slot] = tail.loopEnabled;
            voices.v.stealTailFramesRemaining[slot] = tail.framesRemaining;
            voices.v.stealTailFramesTotal[slot] = tail.framesTotal;
            if (voices.v.rot != nullptr) voices.v.stealTailRot[slot] = tail.rot;
            voices.AdoptStealTailSlot(slot);
            firstFreeTailSlot = slot + 1u;
            break;
        }
    }

    voices.SetCurrentFrame(blockStartFrame + numFrames);
}
