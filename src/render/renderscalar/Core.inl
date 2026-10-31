// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.

inline RenderScalar::RenderScalar()
    : dispatcher_(nullptr), batchDispatcher_(nullptr), dispatcherUserData_(nullptr),
      kernelSet_(&SelectBestRenderKernelSet()), classChanges_(nullptr),
      retirements_(nullptr), scratchCapacity_(0u),
      workerPool_(new (std::nothrow) RenderWorkerPool())
      {
    const bool reserved = ReserveVoiceCapacity(kMaxVoicesDefault);
    assert(reserved);
    (void)reserved;
}

inline RenderScalar::~RenderScalar() {
    delete workerPool_;
    _aligned_free(classChanges_);
    _aligned_free(retirements_);
    _aligned_free(wvExciteHead_);
    _aligned_free(wvExciteTail_);
    _aligned_free(wvPlan_);
    _aligned_free(wvSliceHandles_);
    _aligned_free(wvOpHandles_);
    _aligned_free(wvOpFrames_);
    _aligned_free(wvOpIndex_);
    _aligned_free(wvGhostStartFrame_);
    _aligned_free(wvGhostHandle_);
    _aligned_free(wvGhostStartIngress_);
    _aligned_free(wvGhostDeathFrame_);
    _aligned_free(wvGhostReleaseFrame_);
    _aligned_free(wvGhostNoTail_);
    _aligned_free(wvRowOps_);
    _aligned_free(wvChanOps_);
    _aligned_free(wvJobRetirements_);
    _aligned_free(wvJobRetireCounts_);
    _aligned_free(wvJobSeg_);
    delete[] wvGhostTails_;
    delete[] wvTailScratch_;
}

inline bool RenderScalar::ConfigureRenderThreads(
    uint32_t totalRenderThreads, uint32_t maximumBlockFrames) {
    if (!workerPool_) return totalRenderThreads <= 1u;
    // Size deterministic tile storage for the capacity already reserved by
    // the engine. A later live grow safely falls back to serial rendering
    // until the worker pool is rebuilt at restart.
    return workerPool_->Initialize(totalRenderThreads, maximumBlockFrames,
                                   scratchCapacity_);
}

inline uint32_t RenderScalar::GetRenderThreadCount() const {
    return workerPool_ ? workerPool_->GetThreadCount() : 1u;
}

inline bool RenderScalar::ReserveVoiceCapacity(uint32_t voiceCapacity) {
    if (voiceCapacity <= scratchCapacity_) return true;
    if (voiceCapacity == 0u || voiceCapacity > kMaxPolyphony) return false;
    uint32_t* classChanges = static_cast<uint32_t*>(_aligned_malloc(
        static_cast<size_t>(voiceCapacity) * sizeof(uint32_t),
        kMixBufferAlign));
    SpanRetirement* retirements = static_cast<SpanRetirement*>(_aligned_malloc(
        static_cast<size_t>(voiceCapacity) * sizeof(SpanRetirement),
        kMixBufferAlign));
    if (!classChanges || !retirements) {
        _aligned_free(classChanges);
        _aligned_free(retirements);
        return false;
    }
    _aligned_free(classChanges_);
    _aligned_free(retirements_);
    classChanges_ = classChanges;
    retirements_ = retirements;
    scratchCapacity_ = voiceCapacity;
    return true;
}

inline bool RenderScalar::SetRenderBackend(RenderBackend backend) {
    const RenderKernelSet* selected = SelectRenderKernelSet(backend);
    if (selected == nullptr) return false;
    kernelSet_ = selected;
    return true;
}

inline void RenderScalar::SetEventDispatcher(EventDispatcher dispatcher, void* userData) {
    dispatcher_ = dispatcher;
    dispatcherUserData_ = userData;
}

inline void RenderScalar::SetEventBatchDispatcher(EventBatchDispatcher dispatcher,
                                                   void* userData) {
    batchDispatcher_ = dispatcher;
    dispatcherUserData_ = userData;
}



// ════════════════════════════════════════════════════════════════════════
// RenderBlock — per-sample dispatch + decimation-stride render.
//
// Walk the frame range one sample at a time.  At each sample:
//   1. Dispatch all events whose integer floor equals the current frame.
//   2. Compute decimation step from the active voice count.
//   3. Process every active voice for this sample (fused per-voice body).
//
// The per-voice DSP body is fused into the loop below (it used to be a
// separate ProcessVoiceOneSample call — see class comment for why).
// `mixAudio` gates only the sample fetch + output accumulation; envelope,
// phase and retirement always advance so decimated voices don't freeze.
// ════════════════════════════════════════════════════════════════════════
inline void RenderScalar::RenderBlockFrameMajor(VoiceManager& voices, const ChannelCache& channels,
                                        const int16_t* sampleData,
                                        const int16_t* hilbertData,
                                        uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                        const RenderEvent* events, uint32_t eventCount,
                                        bool correctnessMode,
                                        uint64_t blockStartFrame,
                                        float* const* channelBusLeft,
                                        float* const* channelBusRight) {
    voices.ApplyRuntimeVoiceLimit(blockStartFrame);
    // Scratch is indexed by the number of voices that can retire/reclassify in
    // one span, not by the physical handle ceiling. A live VoiceSoA grow may
    // therefore safely leave scratch at its old size until active polyphony
    // actually crosses it.
    if (voices.activeCount_ > scratchCapacity_ &&
        !ReserveVoiceCapacity(voices.activeCount_)) return;
    if (!classChanges_ || !retirements_) return;
    (void)cfg;
    (void)channels;
    VoiceSoA& v = voices.v;

    // Premultiplied gains are refreshed at note-on and by the exact-frame
    // controller dispatcher.  A callback-wide refresh is both redundant and
    // disproportionately expensive at small buffers on legacy processors.

    uint32_t eventIdx = 0;

    uint32_t step = DecimationStep(voices.activeCount_);

    // ── Sort activeList_ by velocity descending ───────────────────────
    // Only needed when decimation is active (step > 1).  At step=1 all
    // voices are mixed regardless of position, so the sort is dead weight.
    if (step > 1 && voices.activeCount_ > 1) {
        std::sort(voices.activeList_, voices.activeList_ + voices.activeCount_,
            [&v](uint32_t a, uint32_t b) {
                return v.velocity[a] > v.velocity[b];
            });
        voices.RebuildActivePositions();
    }

    // Running float copy of the frame index for the event-floor compare
    // (avoids a float→int conversion per event check per sample).
    uint32_t frameCursor = 0;

    for (uint32_t f = 0; f < numFrames; ++f, ++frameCursor) {
        voices.SetCurrentFrame(blockStartFrame + f);
        // ── 1. Dispatch events at this frame ──────────────────────────
        // floor(sampleOffset) <= f  <=>  sampleOffset < f + 1
        const uint32_t batchBegin = eventIdx;
        while (eventIdx < eventCount) {
            if (events[eventIdx].frameOffset > frameCursor) break;
            ++eventIdx;
        }
        if (eventIdx != batchBegin) {
            if (batchDispatcher_) {
                batchDispatcher_(events + batchBegin, eventIdx - batchBegin,
                                 f, dispatcherUserData_);
            } else if (dispatcher_) {
                for (uint32_t i = batchBegin; i < eventIdx; ++i)
                    dispatcher_(events[i], f, dispatcherUserData_);
            }
            // Exact-frame CC dispatch may have marked channels stale; fold
            // before this frame's voices render (reference path).
            voices.FoldStaleChannelMixGains();
        }
        if (voices.activeCount_ > scratchCapacity_ &&
            !ReserveVoiceCapacity(voices.activeCount_)) return;

        // ── 2. Determine decimation step ──────────────────────────────
        // At step=1 every voice mixes: mixLimit and the newborn check are
        // dead work, so they are skipped entirely (fullMix fast path).
        // mixLimit is hoisted per-sample; staleness by one retirement is
        // harmless (at most one extra voice mixes for one sample).
        uint32_t stepNow = DecimationStep(voices.activeCount_);
        const bool fullMix = (stepNow == 1);
        const uint32_t mixLimit = fullMix ? 0u : (voices.activeCount_ / stepNow);

        // ── 3. Process active voices ──────────────────────────────────
        float* outL = outputLeft + f;
        float* outR = outputRight + f;

        // Tail slots are independent from active voice handles. Iterate the
        // dense reserve backwards so completed-tail swap removal is safe.
        const uint32_t tailCount = voices.GetStealTailCount();
        const uint32_t* tailHandles = voices.GetStealTailList();
        for (uint32_t position = tailCount; position > 0u; --position) {
            const uint32_t tailSlot = tailHandles[position - 1u];
            float* tailOutL = outL;
            float* tailOutR = outR;
            if (channelBusLeft != nullptr) {
                const uint32_t channel = v.stealTailChannel[tailSlot];
                const uint32_t safeChannel =
                    channel < kChannelCount ? channel : 0u;
                tailOutL = channelBusLeft[safeChannel] + f;
                tailOutR = channelBusRight[safeChannel] + f;
            }
            RenderStealTailSample(v, tailSlot, sampleData, hilbertData,
                                  sampleDataFrames, tailOutL, tailOutR);
            voices.RefreshStealTail(static_cast<VoiceHandle>(tailSlot));
        }

        for (uint32_t i = 0; i < voices.activeCount_; ) {
            uint32_t idx = voices.activeList_[i];

            if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) {
                // Retired earlier but not yet cleaned from activeList_ by
                // the swap-remove in RetireVoice.  Skip; the list compacts
                // naturally as retirements shrink activeCount_.
                ++i;
                continue;
            }

            const bool mixAudio = fullMix
                || (i < mixLimit)
                || (voices.GetVoiceAge(static_cast<VoiceHandle>(idx)) <
                    kNewbornProtectSamples);

            // ── Fused per-voice body ──────────────────────────────────
            uint64_t pos         = phasefx::Load(v, idx);
            const uint64_t step  = phasefx::FromFloat(v.phaseIncs[idx]);
            float gain           = v.currentGain[idx];
            const uint8_t voiceState = v.state[idx];
            const uint32_t sStart = v.sampleStart[idx];

            const bool isSampleBacked = (v.sampleBacked[idx] != 0 && sampleData != nullptr);
            const bool isReleased = (voiceState == static_cast<uint8_t>(VoiceState::Releasing));
            const uint8_t envelopeStageBefore = v.envelopeStage[idx];

            // Precomputed region constants (see VoiceManager::SetVoiceSample).
            const uint32_t relEnd    = v.relEnd[idx];
            const uint32_t relLoopS  = v.relLoopS[idx];
            const uint32_t relLoopE  = v.relLoopE[idx];
            const uint64_t loopS64   = phasefx::Pack(relLoopS, 0u);
            const uint64_t loopE64   = phasefx::Pack(relLoopE, 0u);
            const bool loop = isSampleBacked && (v.loopEnabled[idx] != 0);

            float sample = 0.0f;
            // Sample-fetch coordinates, captured where the fetch happens so
            // the Hilbert companion can be read with identical index math at
            // the mix site below (the goto skips the envelope, so locals
            // from the fetch block are not in scope there).
            uint32_t fetchBaseIndex = 0u;
            uint32_t fetchNextIndex = 0u;
            float fetchFrac = 0.0f;
            bool retireVoice = false;
            bool releaseFinished = false;
            bool sustain = false;

            if (isSampleBacked) {
                uint32_t baseOffset = phasefx::Index(pos);
                if (baseOffset + 1u >= relEnd) {
                    if (!loop) {
                        retireVoice = true;
                        goto done;
                    }
                    pos = loopS64;
                    baseOffset = relLoopS;
                }

                {
                    const uint32_t baseIdx = sStart + baseOffset;
                    uint32_t nextRel = baseOffset + 1u;
                    if (loop && nextRel >= relLoopE) nextRel = relLoopS;
                    if (nextRel >= relEnd) nextRel = relEnd - 1u;
                    const uint32_t nextIdx = sStart + nextRel;

                    const float frac = phasefx::Weight(phasefx::Frac(pos));

                    // Skip expensive sample memory access when decimated.
                    // The voice still advances phase and envelope so it
                    // retires correctly; only the audio output is skipped.
                    if (mixAudio) {
                        sample = InterpolateSample(
                            sampleData, baseIdx, nextIdx, frac);
                        fetchBaseIndex = baseIdx;
                        fetchNextIndex = nextIdx;
                        fetchFrac = frac;
                    }
                }

                // ── Envelope — 3-way early dispatch ───────────────────
                // Sustain (stage 3, steady-state majority): no envelope
                // work at all and no currentGain store.  Release: one
                // multiply.  Transient stages run the sequential chain.
                {
                    const uint8_t stage = v.envelopeStage[idx];
                    sustain = !isReleased && (stage == 3);

                    if (isReleased) {
                        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
                        if (releaseRemaining == 0u) {
                            releaseFinished = true;
                        } else {
                            gain *= v.releaseDecay[idx];
                            if (releaseRemaining != UINT32_MAX) {
                                --releaseRemaining;
                                v.releaseSamplesRemaining[idx] = releaseRemaining;
                                releaseFinished = (releaseRemaining == 0u);
                            }
                        }
                    } else if (!sustain) {
                        if (v.envelopeStage[idx] == 4) {
                            if (v.delaySamplesRemaining[idx] > 0) {
                                --v.delaySamplesRemaining[idx];
                                gain = 0.0f;
                            } else {
                                v.envelopeStage[idx] = 0;
                            }
                        }
                        if (v.envelopeStage[idx] == 0) {
                            if (v.holdSamplesRemaining[idx] > 0) {
                                --v.holdSamplesRemaining[idx];
                                gain = v.targetGain[idx];
                            } else {
                                v.envelopeStage[idx] = 1;
                            }
                        }
                        if (v.envelopeStage[idx] == 1) {
                            if (v.attackSamplesRemaining[idx] > 0) {
                                gain += v.attackGainStep[idx];
                                --v.attackSamplesRemaining[idx];
                                if (gain > v.targetGain[idx]) gain = v.targetGain[idx];
                            } else {
                                gain = v.targetGain[idx];
                            }
                            if (v.attackSamplesRemaining[idx] == 0)
                                v.envelopeStage[idx] = (v.decaySamplesRemaining[idx] > 0) ? 2 : 3;
                        }
                        if (v.envelopeStage[idx] == 2) {
                            if (v.decaySamplesRemaining[idx] > 0) {
                                gain *= v.decaySlope[idx];
                                --v.decaySamplesRemaining[idx];
                                if (gain < v.sustainLevel[idx]) gain = v.sustainLevel[idx];
                            } else {
                                gain = v.sustainLevel[idx];
                            }
                            if (v.decaySamplesRemaining[idx] == 0)
                                v.envelopeStage[idx] = 3;
                        }
                    }
                }

                pos += step;
                if (loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
            }

        done:
            phasefx::Store(v, idx, pos);
            if (!sustain) v.currentGain[idx] = gain;

            float stealFadeIn = 1.0f;
            uint32_t fadeInRemaining = v.stealFadeInFramesRemaining[idx];
            if (fadeInRemaining > 0u) {
                const uint32_t fadeInTotal = v.stealFadeInFramesTotal[idx];
                stealFadeIn = fadeInTotal > 0u
                    ? static_cast<float>(fadeInTotal - fadeInRemaining + 1u) /
                      static_cast<float>(fadeInTotal)
                    : 1.0f;
                v.stealFadeInFramesRemaining[idx] = fadeInRemaining - 1u;
            }

            if (mixAudio) {
                float rotated = sample;
                if (v.rot)
                    rotated = RotateVoiceSample(v.rot[idx], rotated,
                                                hilbertData, fetchBaseIndex,
                                                fetchNextIndex, fetchFrac);
                rotated = ProcessVoiceFilterSample(v, idx, rotated);
                const float scaled = rotated * gain * stealFadeIn;
                if (channelBusLeft != nullptr) {
                    // Bus mode: mix into this voice's channel plane.
                    const uint32_t channel = v.channel[idx];
                    const uint32_t safeChannel =
                        channel < kChannelCount ? channel : 0u;
                    channelBusLeft[safeChannel][f] +=
                        scaled * v.mixGainL[idx];
                    channelBusRight[safeChannel][f] +=
                        scaled * v.mixGainR[idx];
                } else {
                    *outL += scaled * v.mixGainL[idx];
                    *outR += scaled * v.mixGainR[idx];
                }
            }

            const bool thresholdReleaseFinished = isReleased &&
                v.releaseSamplesRemaining[idx] == UINT32_MAX &&
                gain < VoiceRetireThreshold();
            if (retireVoice || releaseFinished || thresholdReleaseFinished) {
                voices.RetireVoice(static_cast<VoiceHandle>(idx));
                // RetireVoice swap-removes: the voice at the end of
                // activeList_ moves into position i.  Don't advance i —
                // re-process the swapped-in voice at this frame.
            } else {
                if (v.envelopeStage[idx] != envelopeStageBefore)
                    voices.RefreshRenderClass(static_cast<VoiceHandle>(idx));
                ++i;
            }
        }
    }

    // ── Batched age update ────────────────────────────────────────────
    // Whole block ran at step=1: per-sample age increments were skipped
    // in the loop, so add the block length once here.  Voices that
    // retired mid-block keep their (stale) block-start age — this makes
    // retireImmediateCount_ over-count voices that died young but past
    // their first 2 samples within the same block.  Diagnostic only.
    voices.SetCurrentFrame(blockStartFrame + numFrames);
}

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
inline void RenderScalar::RenderBlockReference(VoiceManager& voices,
                                        const ChannelCache& channels,
                                        const int16_t* sampleData,
                                        uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames,
                                        const RuntimeConfigSnapshot& cfg,
                                        const RenderEvent* events,
                                        uint32_t eventCount,
                                        bool correctnessMode,
                                        uint64_t blockStartFrame,
                                        float* const* channelBusLeft,
                                        float* const* channelBusRight) {
    RenderBlockReference(voices, channels, sampleData, nullptr,
                         sampleDataFrames, outputLeft, outputRight, numFrames,
                         cfg, events, eventCount, correctnessMode,
                         blockStartFrame, channelBusLeft, channelBusRight);
}

inline void RenderScalar::RenderBlockReference(VoiceManager& voices,
                                        const ChannelCache& channels,
                                        const int16_t* sampleData,
                                        const int16_t* hilbertData,
                                        uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames,
                                        const RuntimeConfigSnapshot& cfg,
                                        const RenderEvent* events,
                                        uint32_t eventCount,
                                        bool correctnessMode,
                                        uint64_t blockStartFrame,
                                        float* const* channelBusLeft,
                                        float* const* channelBusRight) {
    RenderBlockFrameMajor(voices, channels, sampleData, hilbertData,
                          sampleDataFrames,
                          outputLeft, outputRight, numFrames, cfg, events,
                          eventCount, correctnessMode, blockStartFrame,
                          channelBusLeft, channelBusRight);
}
#endif
