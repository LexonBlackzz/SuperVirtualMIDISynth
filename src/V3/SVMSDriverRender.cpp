// Audio callback and MIDI dispatch: RenderCallback, DispatchRenderEvent*,
// Handle*. Kept in one file so the per-event calls stay inlinable.

#include "SVMSDriverInternal.h"

namespace svms {

void Driver::RenderCallback(float* output, uint32_t numFrames, void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self || !self->initialized) return;
    ++self->callbackCount_;

    // A driver-initiated format change (ASIO buffer size / sample rate) is
    // applied here — the audio thread, which owns the mix buffers — before
    // this block renders, so no span ever sees a stale capacity.
    if (self->pendingFormatFrames_.load(std::memory_order_acquire) ||
        self->pendingFormatRate_.load(std::memory_order_acquire))
        self->ApplyPendingAudioFormat();

    // The immutable bundle was fully parsed and prepared by a non-audio
    // thread.  Activation is one pointer handoff at the callback boundary;
    // no allocation, file access, lock, or device restart occurs here.
    self->ActivatePendingSoundFontAtBlockBoundary();

    VoiceManager* vm = self->voiceManager;
    ChannelCache* cc = self->channelCache;
    RenderScalar* render = self->renderScalar;
    RuntimeConfigSnapshot* snap = self->configSnapshot;
    const int16_t* sd = self->sampleDataStore;
    const int16_t* hd = self->hilbertDataStore;

    if (!vm || !cc || !render || !snap) return;

    // ── Mailbox sync: dirty-gated monotonic seqlock read ───────────
    // The control thread publishes only when the user actually moves a
    // knob; the mailbox is skipped entirely when the sequence matches
    // lastAppliedLiveSeq_ (the common per-block case).  On a torn (odd
    // or mid-flight) read we fall back to appliedMailbox_, the last
    // state this audio thread applied.  The DSP apply (incl.
    // reverb.UpdateDerived and the limiter glide targets) runs only on
    // change, so the per-block overhead of the live path is one atomic
    // load and one compare.
    const uint32_t seq = self->liveMailboxSeq_.load(std::memory_order_acquire);
    if (seq != self->lastAppliedLiveSeq_) {
        NonAtomicLiveConfigMailbox mb;
        uint32_t appliedSeq = seq;
        if ((seq & 1u) == 0u) {
            self->liveMailbox_.StoreToNonAtomic(mb);
            if (self->liveMailboxSeq_.load(std::memory_order_acquire) != seq) {
                mb = self->appliedMailbox_;
                appliedSeq = self->appliedSeq_.load(std::memory_order_acquire);
            }
        } else {
            mb = self->appliedMailbox_;
            appliedSeq = self->appliedSeq_.load(std::memory_order_acquire);
        }

        snap->masterVolume   = mb.masterVolume;
        snap->correctnessMode = mb.correctnessMode;
        snap->enableReverb   = mb.reverbEnabled;
        self->correctnessMode_ = mb.correctnessMode;

        self->reverb.enabled     = mb.reverbEnabled;
        self->reverb.mix         = mb.reverbMix;
        self->reverb.roomSize    = mb.reverbRoomSize;
        self->reverb.decay       = mb.reverbDecay;
        self->reverb.damping     = mb.reverbDamping;
        self->reverb.width       = mb.reverbWidth;
        self->reverb.diffusion   = mb.reverbDiffusion;
        self->reverb.preDelayMs  = mb.reverbPreDelayMs;
        self->reverb.earlyLevel  = mb.reverbEarlyLevel;
        self->reverb.lateLevel   = mb.reverbLateLevel;
        self->reverb.modDepth    = mb.reverbModDepth;
        self->reverb.modRate     = mb.reverbModRate;
        self->reverb.lowCutHz    = mb.reverbLowCutHz;
        self->reverb.highCutHz   = mb.reverbHighCutHz;

        // Derived parameters (FDN feedback, tap gains, lengths, LFO
        // increments) recompute WITHOUT clearing the delay lines, so a
        // live change morphs the tail instead of cutting it dead.
        self->reverb.UpdateDerived();

        self->limiter.enabled           = mb.limiterEnabled;
        self->limiter.algorithmTarget   = mb.limiterAlgorithm;
        self->limiter.thresholdTarget   = mb.limiterThreshold;
        self->limiter.delayFramesTarget = mb.limiterDelayFrames;
        self->limiter.attackCoeff       = mb.limiterAttackCoeff;
        self->limiter.releaseCoeff      = mb.limiterReleaseCoeff;

        // Per-MIDI-channel limiter: glide targets only (threshold glides
        // per frame inside ProcessAndSum).  Re-enable clears stale
        // envelope state from a previous session.
        {
            const bool wasEnabled = self->channelLimiter.enabled;
            self->channelLimiter.SetLiveTargets(
                mb.channelLimiterEnabled, mb.channelLimiterThreshold,
                mb.channelLimiterReleaseMs, self->sampleRate);
            if (self->channelLimiter.enabled && !wasEnabled)
                self->channelLimiter.Reset();
        }

        // Per-voice phase rotation mode (0 = Coherent bit-exact bypass).
        self->voiceManager->SetPhaseRotationMode(mb.phaseRotationMode);

        self->channelCache->SetMasterVolume(
            mb.masterVolume * self->sysexMasterVolume_);

        // Echo the applied state to the control thread (telemetry audit).
        // The echo must be coherent BEFORE lastAppliedLiveSeq_ advances
        // so no reader can ever observe a newer appliedSeq_ with an
        // older mailbox.
        self->appliedMailbox_ = mb;
        self->appliedSeq_.store(appliedSeq, std::memory_order_release);
        self->lastAppliedLiveSeq_ = appliedSeq;
    }

    LARGE_INTEGER renderStartQPC;
    QueryPerformanceCounter(&renderStartQPC);
    const bool profileCallback = self->diagnosticsEnabled_;
    // Per-callback trace (SVMS_CAP_CALLBACK_TRACE): two extra QPC reads and
    // one record store per callback, only while a client has it enabled.
    CallbackTraceRing* const traceRing =
        self->callbackTraceEnabled_.load(std::memory_order_relaxed)
            ? self->callbackTrace_.load(std::memory_order_acquire)
            : nullptr;
    const uint64_t traceCycleStart = traceRing ? __rdtsc() : 0u;
    const uint64_t traceWvPlan = traceRing ? render->GetWvPlanCycles() : 0u;
    const uint64_t traceWvJobs = traceRing ? render->GetWvJobCycles() : 0u;
    const uint64_t traceWvPost = traceRing ? render->GetWvPostCycles() : 0u;
    const uint64_t profileCycleStart = profileCallback ? __rdtsc() : 0u;

    cc->RebuildCache(*snap, static_cast<float>(self->sampleRate));

    // Fold a live master-volume change into ALL playing voices (their
    // mixGainL/R are only refreshed at note-on otherwise).  Per-channel
    // refresh over the active list — done only when the value actually
    // changed, never per block at 500K voices.
    const float effectiveMasterVolume =
        snap->masterVolume * self->sysexMasterVolume_;
    if (self->appliedMasterVolume_ != effectiveMasterVolume) {
        self->appliedMasterVolume_ = effectiveMasterVolume;
        for (uint32_t ch = 0u; ch < kChannelCount; ++ch) {
            vm->MarkChannelMixStale(
                static_cast<uint8_t>(ch), cc->GetParams()[ch]);
        }
    }

    if (!self->leftBuffer || !self->rightBuffer) {
        std::memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }
    float* leftBuf = self->leftBuffer;
    float* rightBuf = self->rightBuffer;
    std::memset(leftBuf, 0, numFrames * sizeof(float));
    std::memset(rightBuf, 0, numFrames * sizeof(float));
    // Per-MIDI-channel limiter (purely post-render): when enabled the block
    // renders into the 16 channel buses, which the limiter then limits per
    // channel and sums into the master mix.  Nothing upstream of the mix
    // changes; disabled = the direct master-mix path, bit-identical.
    const bool channelLimiterActive =
        self->channelLimiter.enabled &&
        self->channelBusPlanes != nullptr &&
        self->channelBusCapacity >= numFrames;
    if (channelLimiterActive) {
        ChannelLimiterState::ClearBuses(self->channelBusLeftTable,
                                        self->channelBusRightTable,
                                        numFrames);
    }

    // ── Diagnostic: voice retire stats ──────────────────────────────

    // ── Persistent Pending Queue — Unified Event Pipeline ──────────────
    // The SPSC queue carries TimestampedMidiEvent structs from the MIDI
    // host thread.  We drain ALL of them into a persistent, audio-thread-
    // only `pendingEventBuffer`.  Events are converted to RenderEvent with
    // a fractional sampleOffset computed ONCE against this block's start
    // QPC.  From the pending queue we budget-extract at most
    // kMaxEventsPerBlock events that fall within [0, numFrames) into
    // evtBuf for dispatch.  Remaining events stay in the pending queue
    // and have their offsets decremented by numFrames at block end (smooth
    // rollover).  This guarantees:
    //
    //   a) Zero event loss — events beyond the budget simply queue up and
    //      fire in subsequent blocks (natural time-stretching).
    //   b) No frame-0 clumping — pending offsets are decremented uniformly,
    //      never re-computed from QPC.
    //   c) Bounded per-block work — the DSP thread processes at most
    //      kMaxEventsPerBlock events regardless of input density.
    uint64_t blockStartQPC;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&blockStartQPC));
    self->tscClock_.Refresh(blockStartQPC, __rdtsc());

    // ── Virtual render clock ─────────────────────────────────────────
    // Monotonically advances by (numFrames * qpcFreq / sr) each callback
    // so event offsets are relative to the audio timeline, not the
    // wall-clock callback time.  Without this, every SPSC event pushed
    // during the previous block has a negative deltaQPC and snaps to
    // sample 0 — producing the 100 Hz buffer-grid buzz.
    if (!self->clockInitialized) {
        self->virtualRenderClockQPC = blockStartQPC;
        self->clockInitialized = true;
        self->compilerEpochQPC_.store(blockStartQPC, std::memory_order_release);
        self->compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
        WakeAddressWaiters(self->compilerWakeEpoch_);
    }

    const int64_t wallRenderSample = QpcDeltaToFrames(
        static_cast<int64_t>(blockStartQPC) -
            static_cast<int64_t>(self->virtualRenderClockQPC),
        static_cast<int64_t>(self->qpcFreq), self->sampleRate);
    // Unbounded render (opt-in): the wall-time recovery jump is the
    // "breaks down and loses all accuracy" cliff — it skips the schedule
    // forward and clamps the backlog onto the block-start sample. With the
    // toggle on, the render cursor stays on its own timeline: events keep
    // their exact frames in exact order, playback runs at whatever speed
    // the engine manages, and WASAPI glitches instead of the schedule.
    const uint64_t skippedFramesBefore = self->telemetry_.skippedOutputFrames;
    if (!self->unboundedRenderEnabled_.load(std::memory_order_relaxed)) {
        const int64_t recoveredRenderSample = RecoverRealtimeRenderFrame(
            self->virtualRenderSample_, wallRenderSample, numFrames);
        if (recoveredRenderSample > self->virtualRenderSample_) {
            self->telemetry_.skippedOutputFrames += static_cast<uint64_t>(
                recoveredRenderSample - self->virtualRenderSample_);
            self->virtualRenderSample_ = recoveredRenderSample;
        }
    }

    // First frame this block actually renders (after any recovery jump).
    const uint64_t traceOutputFrame =
        static_cast<uint64_t>(self->virtualRenderSample_);

    // ── Drift recovery ──────────────────────────────────────────────
    // If the render takes >100% CPU, the virtual clock falls behind
    // wall time.  Left unchecked this causes permanent desync: event
    // offsets inflate forever, the pending queue never drains, and the
    // audio is delayed until reset.  Fast-forward the clock to catch
    // up and rescale queued event offsets so they land in this block.
    // ── Step 1: Drain ALL SPSC events → append to pending queue ─────────
    uint32_t scannedIngress = 0;
    uint32_t admittedEvents = 0;
    std::memset(self->staleRecoveryValid_, 0,
                sizeof(self->staleRecoveryValid_));
    std::memset(self->staleRecoveryNoteOffValid_, 0,
                sizeof(self->staleRecoveryNoteOffValid_));
    std::memset(self->staleRecoveryNoteOffCount_, 0,
                sizeof(self->staleRecoveryNoteOffCount_));
        // Apply a parked steal-policy switch at the block boundary: launch
    // transactions never span callbacks, so the index rebuild is atomic
    // with respect to every reserved-candidate commit.
    {
        const uint32_t pendingPolicy =
            self->pendingStealPolicy_.exchange(UINT32_MAX,
                                               std::memory_order_acq_rel);
        if (pendingPolicy != UINT32_MAX && self->voiceManager)
            self->voiceManager->SetStealPolicy(pendingPolicy);
    }
const uint32_t importedPages = self->useEventCompiler_
        ? self->pagedScheduler_.ImportAllReady()
        : 0u;
    if (self->useEventCompiler_) {
        self->telemetry_.scheduledHighWater = (std::max)(
            self->telemetry_.scheduledHighWater,
            static_cast<uint64_t>(self->pagedScheduler_.HighWater()));
    }
    const uint32_t ingressScanBudget = self->eventScheduler_.Capacity();
    while (!self->useEventCompiler_ &&
           scannedIngress < ingressScanBudget &&
           admittedEvents < self->maxEventsPerBlock_ &&
           self->eventScheduler_.Size() < self->eventScheduler_.Capacity()) {
        ScheduledRenderEvent scheduled{};
        TimestampedMidiEvent timed{};
        if (!self->midiIngress_.TryPop(timed)) break;
        if (!CompileTimestampedEvent(
                timed, self->virtualRenderClockQPC, self->qpcFreq,
                self->sampleRate, self->bufferFrames, scheduled)) {
            ++scannedIngress;
            continue;
        }
        ++scannedIngress;
        if (self->eventScheduler_.Size() >= self->eventScheduler_.Capacity()) {
            break;
        }

        RenderEvent ev = scheduled.ToRenderEvent();
        const RenderEventType etype = ev.type;
        const uint8_t ch = ev.channel;
        const uint8_t d1 = ev.data1;
        const uint32_t sequence = scheduled.sequence;
#if defined(SVMS_XP_COMPAT)
        // DirectSound's notification cursor and the QPC playback position can
        // differ by several ring segments on XP. A live event behind the next
        // writable frame is late, not obsolete: dispatch it at that frame.
        if (scheduled.targetFrame < self->virtualRenderSample_) {
            ++self->telemetry_.late;
            scheduled.targetFrame = self->virtualRenderSample_;
        }
#endif
        const uint32_t recoveryKey =
            static_cast<uint32_t>(ch) * kNoteCount + d1;
        if (etype == RenderEventType::NoteOff && d1 < kNoteCount &&
            scheduled.targetFrame < self->virtualRenderSample_) {
            if (self->staleRecoveryNoteOffCount_[recoveryKey] <
                kMaxPolyphony) {
                ++self->staleRecoveryNoteOffCount_[recoveryKey];
            }
            if (!self->staleRecoveryNoteOffValid_[recoveryKey] ||
                !SequenceAtOrBefore(
                    sequence,
                    self->staleRecoveryNoteOffSequence_[recoveryKey])) {
                self->staleRecoveryNoteOffSequence_[recoveryKey] = sequence;
                self->staleRecoveryNoteOffFrame_[recoveryKey] =
                    scheduled.targetFrame;
                self->staleRecoveryNoteOffValid_[recoveryKey] = 1u;
            }
            if (self->staleRecoveryValid_[recoveryKey] &&
                SequenceAtOrBefore(
                    self->staleRecoveryEvents_[recoveryKey].ingressSequence,
                    sequence)) {
                self->staleRecoveryValid_[recoveryKey] = 0u;
                ++self->telemetry_.staleNoteOnsSkipped;
            }
            ++self->telemetry_.staleNoteOffsCompacted;
            continue;
        }
        if (etype == RenderEventType::NoteOn &&
            IsObsoleteNoteOn(scheduled.targetFrame, self->virtualRenderSample_,
                             self->bufferFrames)) {
            ++self->telemetry_.late;
            if (d1 >= kNoteCount ||
                (self->staleRecoveryNoteOffValid_[recoveryKey] &&
                 SequenceAtOrBefore(
                     sequence,
                     self->staleRecoveryNoteOffSequence_[recoveryKey]))) {
                ++self->telemetry_.staleNoteOnsSkipped;
                continue;
            }
            if (self->staleRecoveryValid_[recoveryKey]) {
                if (SequenceAtOrBefore(
                        sequence,
                        self->staleRecoveryEvents_[recoveryKey].ingressSequence)) {
                    ++self->telemetry_.staleNoteOnsSkipped;
                    continue;
                }
                ++self->telemetry_.staleNoteOnsSkipped;
            }
            ev.frameOffset = 0u;
            self->staleRecoveryEvents_[recoveryKey] = ev;
            self->staleRecoveryValid_[recoveryKey] = 1u;
            continue;
        }
        if (!self->eventScheduler_.EnqueueBatched(scheduled)) {
            ++self->telemetry_.dropped;
            break;
        }
        ++admittedEvents;
        self->telemetry_.scheduledHighWater =
            (std::max)(self->telemetry_.scheduledHighWater,
                       static_cast<uint64_t>(self->eventScheduler_.Size()));
    }
    // Recovered notes all become writable at this block's first frame. Late
    // note-offs are replayed in batches of at most 255, capped by the current
    // maximum possible voice generations. This retains note-off multiplicity
    // without allowing millions of dead historical messages to consume the
    // callback quota. The scheduler restores frame/sequence order even though
    // the compact set is walked by channel/key here.
    for (uint32_t key = 0;
         !self->useEventCompiler_ &&
         key < Driver::kStaleRecoveryKeys &&
         admittedEvents < self->maxEventsPerBlock_ &&
         self->eventScheduler_.Size() < self->eventScheduler_.Capacity();
         ++key) {
        uint32_t remainingNoteOffs =
            self->staleRecoveryNoteOffValid_[key]
                ? self->staleRecoveryNoteOffCount_[key]
                : 0u;
        while (remainingNoteOffs != 0u &&
               admittedEvents < self->maxEventsPerBlock_ &&
               self->eventScheduler_.Size() <
                   self->eventScheduler_.Capacity()) {
            const uint32_t batch = (std::min)(remainingNoteOffs, 255u);
            ScheduledRenderEvent recoveredOff{};
            recoveredOff.targetFrame =
                self->staleRecoveryNoteOffFrame_[key];
            recoveredOff.sequence =
                self->staleRecoveryNoteOffSequence_[key];
            recoveredOff.type = RenderEventType::StaleNoteOffBatch;
            recoveredOff.channel = static_cast<uint8_t>(key / kNoteCount);
            recoveredOff.data1 = static_cast<uint8_t>(key % kNoteCount);
            recoveredOff.data2 = static_cast<uint8_t>(batch);
            if (!self->eventScheduler_.EnqueueBatched(recoveredOff)) break;
            remainingNoteOffs -= batch;
            ++admittedEvents;
        }
        if (!self->staleRecoveryValid_[key]) continue;
        ScheduledRenderEvent recovered;
        recovered.SetRenderEvent(self->staleRecoveryEvents_[key]);
        recovered.targetFrame = self->virtualRenderSample_;
        recovered.sequence = self->staleRecoveryEvents_[key].ingressSequence;
        if (!self->eventScheduler_.EnqueueBatched(recovered)) {
            ++self->telemetry_.staleNoteOnsSkipped;
            break;
        }
        ++admittedEvents;
    }

    if (!self->useEventCompiler_) self->eventScheduler_.FinalizeBatch();

    self->producerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(self->producerWakeEpoch_);
    const uint32_t scheduledBeforeDispatch = self->useEventCompiler_
        ? self->pagedScheduler_.Size()
        : self->eventScheduler_.Size();
    self->scheduledSizePublished_.store(scheduledBeforeDispatch,
                                        std::memory_order_release);

    // Extract only this render window.  Future events remain in the heap.
    RenderEvent* evtBuf = self->eventBuffer;
    uint32_t evCount = 0;
    uint32_t examinedCount = 0;
    // Events clamped to frame 0 in this block (lateClamped telemetry input).
    uint32_t clampedThisBlock = 0;
    uint64_t maxLatenessThisBlock = 0u;
    // Per-block admission cap for LATE events. Hosts recovering from a seek
    // or a stall burst-submit millions of events, every one of them already
    // late (clamped to frame 0 or stale-dropped under PriorityVelocity).
    // Admitting all of them into one render block explodes the whole-voice
    // plan (per-event scratch, millions of launches) and forces the sparse
    // fallback, at 3x+ over budget. Late events past this cap simply stay
    // scheduled and are admitted over the following blocks — no loss, the
    // clock-stretch machinery already treats them as late.
    // Events still on time are not counted: capping them made a sustained
    // stream above cap/callback (~13M events/s at 10 ms callbacks) go late
    // by design, with the render thread mostly idle. They stay bounded by
    // the configured max_events_per_block.
    static constexpr uint32_t kBlockDispatchSoftCap = 1u << 17u;
    const uint32_t eventBudget =
        (std::min)(self->eventBufferCapacity_, self->maxEventsPerBlock_);
    // Unbounded render lifts the late cap as well: the scheduler drains the
    // backlog in strict order, whatever the callback costs.
    const uint32_t lateBudget = self->unboundedRenderEnabled_.load(
        std::memory_order_relaxed)
        ? eventBudget
        : (std::min)(eventBudget, kBlockDispatchSoftCap);
    uint32_t lateDispatched = 0u;
    auto admitScheduled = [&](const ScheduledRenderEvent& scheduledOut) {
        ++examinedCount;
        if (self->overflowMode_.load(std::memory_order_relaxed) ==
                EventOverflowMode::PriorityVelocity &&
            scheduledOut.type == RenderEventType::NoteOn &&
            IsObsoleteNoteOn(scheduledOut.targetFrame,
                             self->virtualRenderSample_, self->bufferFrames)) {
            ++self->telemetry_.late;
            ++self->telemetry_.staleNoteOnsSkipped;
            return;
        }
        // External fire-now backends (WinMM devices, KDMAPI/SVMS-API synths)
        // carry bounded internal voice pools — the GS wavetable holds 32 —
        // and die outright when fed a stale backlog. Kiva keeps such synths
        // alive with one rule, implemented here: a note-on more than ~one
        // second behind playback is dropped unless it is high-priority
        // velocity. Controls and note-offs always pass; the SVMS engine's
        // own lossless admission default is untouched.
        if (self->externalBackendKind_.load(std::memory_order_relaxed) != 0u &&
            scheduledOut.type == RenderEventType::NoteOn &&
            scheduledOut.data2 < self->engineConfig_.highPriorityVelocity &&
            self->virtualRenderSample_ - scheduledOut.targetFrame >
                static_cast<int64_t>(self->sampleRate)) {
            ++self->telemetry_.shedNoteOns;
            self->shedAtomic_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        int64_t offset = scheduledOut.targetFrame - self->virtualRenderSample_;
        if (offset < 0) ++self->telemetry_.late;
        if (self->blockTimingEnabled_.load(std::memory_order_relaxed)) {
            // Block-granular dispatch (opt-in, syndrv-style): every event due
            // in this block fires at block start. One launch burst, zero
            // mid-block span splits; intra-block offsets are relinquished.
            offset = 0;
        } else if (offset < 0) {
            ++self->telemetry_.lateClamped;
            ++clampedThisBlock;
            const int64_t lateness = -offset;
            maxLatenessThisBlock = (std::max)(
                maxLatenessThisBlock, static_cast<uint64_t>(lateness));
            if (static_cast<uint64_t>(lateness) >
                self->telemetry_.lateClampMaxLateness)
                self->telemetry_.lateClampMaxLateness =
                    static_cast<uint64_t>(lateness);
            offset = 0;
        }
        evtBuf[evCount++] = scheduledOut.ToRenderEvent(
            static_cast<uint32_t>(offset));
        ++self->telemetry_.dispatched;
    };
    if (self->useEventCompiler_) {
        for (;;) {
            const ScheduledRenderEvent* run = nullptr;
            uint32_t runCount = 0u;
            // Direct-dispatch fast path: when the merge root exclusively
            // owns everything due before this block's end, read the events
            // straight from the immutable payload — no tree walk and no
            // cross-page comparisons. The merge loop below remains the
            // exact fallback whenever another page interleaves.
            if (!self->pagedScheduler_.ExclusiveRunBefore(
                    self->virtualRenderSample_ + numFrames,
                    eventBudget - examinedCount, run, runCount)) {
                runCount = self->pagedScheduler_.PeekRunBefore(
                    self->virtualRenderSample_ + numFrames,
                    eventBudget - examinedCount, run);
                if (runCount == 0u) break;
            }
            // Runs are frame-ordered, so late events come first. Only late
            // events that are actually dispatched count against the cap:
            // obsolete note-ons skipped by admitScheduled cost nothing
            // downstream, and counting them kept a backlog that was behind
            // from draining faster than a dense stream refilled it.
            uint32_t taken = 0u;
            for (; taken < runCount; ++taken) {
                if (run[taken].targetFrame < self->virtualRenderSample_) {
                    if (lateDispatched >= lateBudget) break;
                    const uint32_t before = evCount;
                    admitScheduled(run[taken]);
                    lateDispatched += evCount - before;
                } else {
                    admitScheduled(run[taken]);
                }
            }
            self->pagedScheduler_.ConsumeRun(taken);
            if (taken < runCount || examinedCount == eventBudget) break;
        }
    } else {
        ScheduledRenderEvent scheduledOut{};
        while (examinedCount < lateBudget &&
               self->eventScheduler_.PopBefore(
                   self->virtualRenderSample_ + numFrames, scheduledOut)) {
            admitScheduled(scheduledOut);
        }
    }
    // Worst same-sample pileup of frame-0-clamped events in one block. Every
    // clamped event lands on the block-start sample, so this is the direct
    // measure of the block-grid transient concentration.
    if (clampedThisBlock > self->telemetry_.lateClampBlockPileupMax)
        self->telemetry_.lateClampBlockPileupMax = clampedThisBlock;
    const uint32_t scheduledAfterDispatch = self->useEventCompiler_
        ? self->pagedScheduler_.Size()
        : self->eventScheduler_.Size();
    self->scheduledSizePublished_.store(scheduledAfterDispatch,
                                        std::memory_order_release);
    if (importedPages != 0u || examinedCount != 0u) {
        self->compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
        WakeAddressWaiters(self->compilerWakeEpoch_);
    }

    // ── Step 2: Sort pending queue by sampleOffset ─────────────────────
    // Insertion sort.  The carry-forward portion (from previous blocks) is
    // already sorted (offset decrement preserves relative order).  Newly
    // appended SPSC events at the tail are the only unsorted elements.
    // For mostly-sorted data this is O(N) with a small constant.

    // ── Step 3: Extract all in-block events for this block ──────────────
    // Walk the sorted pending queue from offset 0.  Take every event with
    // sampleOffset < numFrames into evtBuf.  Stop at the first future
    // event (offset >= numFrames).  With in-place voice recycling keeping
    // active polyphony bounded to ~128-256 voices, the engine can process
    // all incoming events in real-time without batch-chunking artifacts.
    // ── Step 4: Decrement remaining pending offsets by numFrames ────────
    // Smooth rollover: future events advance at the true audio clock rate
    // toward their firing time without frame-boundary re-quantization.
    // ── Step 5: Sort evtBuf by sampleOffset (defensive) ─────────────────

    // ── Same-key same-frame note-off run compaction ─────────────────────
    // The compiler path delivers one event per note-off. Black-MIDI note-off
    // avalanches then cost twice: once as per-event dispatch, once as
    // whole-voice render fragmentation, because every event splits the block
    // at its exact frame. Consecutive NoteOff events with identical
    // (channel, note, frameOffset) collapse into StaleNoteOffBatch events
    // (255 offs each). Equivalence: HandleStaleNoteOffBatch performs the same
    // idempotent channel-cache write once, then NoteOffOldestPlayIndices
    // loops FindOldestPlayIndex/NoteOffPlayIndex exactly count times — the
    // same state transitions as count sequential HandleNoteOff calls with no
    // intervening event. A run never crosses a different event, so channel
    // state (sustain/sostenuto) is constant inside it. Fences only suppress
    // note-ons, so the batch's ingressSequence (last member of the run) is
    // not consulted for note-offs.
    const uint32_t admittedThisBlock = evCount;
    uint32_t compactedWrite = 0u;
    for (uint32_t readIndex = 0u; readIndex < evCount;) {
        const RenderEvent& head = evtBuf[readIndex];
        if (head.type != RenderEventType::NoteOff) {
            evtBuf[compactedWrite++] = head;
            ++readIndex;
            continue;
        }
        uint32_t runEnd = readIndex + 1u;
        while (runEnd < evCount &&
               evtBuf[runEnd].type == RenderEventType::NoteOff &&
               evtBuf[runEnd].channel == head.channel &&
               evtBuf[runEnd].data1 == head.data1 &&
               evtBuf[runEnd].frameOffset == head.frameOffset) {
            ++runEnd;
        }
        const uint32_t runCount = runEnd - readIndex;
        if (runCount == 1u) {
            evtBuf[compactedWrite++] = head;
            ++readIndex;
            continue;
        }
        uint32_t remaining = runCount;
        while (remaining != 0u) {
            const uint8_t batch = static_cast<uint8_t>(
                (std::min)(remaining, 255u));
            RenderEvent batchEvent = evtBuf[runEnd - 1u];
            batchEvent.type = RenderEventType::StaleNoteOffBatch;
            batchEvent.data2 = batch;
            evtBuf[compactedWrite++] = batchEvent;
            remaining -= batch;
        }
        readIndex = runEnd;
    }
    evCount = compactedWrite;

    // ── Step 6: Render with sub-sample event slicing ───────────────────
    // RenderBlock will invoke DispatchRenderEvent at each event's exact
    // fractional sample offset.  All event handling (voice allocation,
    // release, CC updates) happens inside the render loop via the callback.
    const uint64_t profileScheduleEnd = profileCallback ? __rdtsc() : 0u;
    LARGE_INTEGER traceScheduleEndQPC{};
    if (traceRing) QueryPerformanceCounter(&traceScheduleEndQPC);
    self->dispatchCyclesCurrent_ = 0u;
    // The diagnostic UI publishes once per callback. Capture one successful
    // voice launch for its detailed SF2 probe instead of rewriting ~20 fields
    // for every dense note-on; lifetime counters remain exact below.
    self->captureSf2Detail_ = self->diagnosticsEnabled_;
    if (self->externalBackendKind_.load(std::memory_order_relaxed) != 0u) {
        // External backend routing: forward the block's admitted events to
        // the loaded sink in ingress order and render silence — the sink
        // owns audio output. The in-process voice machinery stays idle.
        self->ForwardBlockToBackend(evtBuf, evCount);
        std::memset(leftBuf, 0, sizeof(float) * numFrames);
        std::memset(rightBuf, 0, sizeof(float) * numFrames);
    } else {
        render->RenderBlock(*vm, *cc, sd, hd, self->sampleDataFrames,
                            leftBuf, rightBuf, numFrames, *snap,
                            evtBuf, evCount, self->correctnessMode_,
                            static_cast<uint64_t>(self->virtualRenderSample_),
                            channelLimiterActive
                                ? self->channelBusLeftTable : nullptr,
                            channelLimiterActive
                                ? self->channelBusRightTable : nullptr);
    }
    if (channelLimiterActive) {
        self->channelLimiter.ProcessAndSum(
            self->channelBusLeftTable, self->channelBusRightTable,
            leftBuf, rightBuf, numFrames);
    }
    const uint64_t profileRenderEnd = profileCallback ? __rdtsc() : 0u;
    LARGE_INTEGER traceRenderEndQPC{};
    if (traceRing) QueryPerformanceCounter(&traceRenderEndQPC);

    // ── Advance virtual render clock for the next callback ──────────
    self->virtualRenderSample_ += static_cast<int64_t>(numFrames);
    self->outputFramePublished_.store(
        static_cast<uint64_t>(self->virtualRenderSample_),
        std::memory_order_release);

    // masterVolume is already included in ChannelCache's per-channel mix
    // gains. Applying it here again would attenuate the output twice.
    float renderPeak = 0.0f;
    const bool collectRenderPeak = self->diagnosticsEnabled_;
    for (uint32_t i = 0; i < numFrames; ++i) {
        const float left = leftBuf[i];
        const float right = rightBuf[i];
        output[i * 2u] = left;
        output[i * 2u + 1u] = right;
        if (collectRenderPeak) {
            renderPeak = (std::max)(renderPeak, std::fabs(left));
            renderPeak = (std::max)(renderPeak, std::fabs(right));
        }
    }
    if (collectRenderPeak) {
        self->sf2Telemetry_.renderPeak = renderPeak;
        if (self->sf2Telemetry_.lastVoiceHandle < vm->GetMaxVoices()) {
            const uint32_t h = self->sf2Telemetry_.lastVoiceHandle;
            self->sf2Telemetry_.lastPhase = vm->v.phases[h];
        }
    }
    // Periodic pool census moved below: it needs the block's cpu/percent
    // fields, which are only known after the post-processing timing pass.
    // Scheduler-lateness census, same 64-block cadence. Deltas since the
    // previous census keep the numbers readable while the stream runs.
    if (self->diagnosticsEnabled_ && (self->callbackCount_ & 63u) == 0u) {
        static uint64_t censusLastClamped = 0;
        static uint64_t censusLastLate = 0;
        static uint64_t censusLastDispatched = 0;
        char schedCensus[192];
        std::snprintf(schedCensus, sizeof(schedCensus),
            "[SVMS] sched: clamped+%llu late+%llu disp+%llu "
            "maxLate=%.2fms pileupMax=%u\n",
            (unsigned long long)(self->telemetry_.lateClamped - censusLastClamped),
            (unsigned long long)(self->telemetry_.late - censusLastLate),
            (unsigned long long)(self->telemetry_.dispatched - censusLastDispatched),
            (double)self->telemetry_.lateClampMaxLateness /
                (double)(self->sampleRate > 0u ? self->sampleRate : 1u) * 1000.0,
            (unsigned)self->telemetry_.lateClampBlockPileupMax);
#if SVMS_AUDIO_CENSUS
        OutputDebugStringA(schedCensus);
#endif
        censusLastClamped = self->telemetry_.lateClamped;
        censusLastLate = self->telemetry_.late;
        censusLastDispatched = self->telemetry_.dispatched;
    }
    // Note-on flow census, same 64-block cadence. Separates "events arrive
    // but note-ons never spawn" causes: fence suppression (fence values
    // pinned behind the ingress sequence), obsolete-note-on drops (stale+),
    // velocity shedding (shed+), or genuinely no spawns (on+ frozen while
    // nothing else climbs). This is the permanent-silence diagnostic.
    if (self->diagnosticsEnabled_ && (self->callbackCount_ & 63u) == 0u) {
        static uint64_t censusLastNoteOns = 0;
        static uint64_t censusLastFenceDrop = 0;
        static uint64_t censusLastStale = 0;
        static uint64_t censusLastShed = 0;
        static uint64_t censusLastCc = 0;
        uint32_t fencedChannels = 0u;
        uint32_t maxChannelFence = 0u;
        for (auto& fence : self->channelTerminationFence_) {
            const uint64_t encoded = fence.load(std::memory_order_relaxed);
            if (encoded == 0u) continue;
            ++fencedChannels;
            maxChannelFence = (std::max)(maxChannelFence,
                static_cast<uint32_t>(encoded - 1u));
        }
        const uint64_t globalEncoded =
            self->globalTerminationFence_.load(std::memory_order_relaxed);
        char flowCensus[224];
        std::snprintf(flowCensus, sizeof(flowCensus),
            "[SVMS] flow: on+%llu fenceDrop+%llu stale+%llu shed+%llu "
            "cc+%llu seq=%u fenceG=%u fencedCh=%u maxChFence=%u q=%u blk=%u act=%u\n",
            (unsigned long long)(self->sf2Telemetry_.noteOns - censusLastNoteOns),
            (unsigned long long)(self->fenceSuppressedNoteOns_ - censusLastFenceDrop),
            (unsigned long long)(self->telemetry_.staleNoteOnsSkipped - censusLastStale),
            (unsigned long long)(self->shedAtomic_.load(std::memory_order_relaxed) - censusLastShed),
            (unsigned long long)(self->ccCollapsedCount_ - censusLastCc),
            self->nextEventSequence_.load(std::memory_order_relaxed),
            (unsigned)(globalEncoded ? globalEncoded - 1u : 0u),
            fencedChannels, maxChannelFence,
            (unsigned)self->midiIngress_.TotalSize(),
            (unsigned)self->scheduledSizePublished_.load(std::memory_order_relaxed),
            (unsigned)vm->activeCount_);
#if SVMS_AUDIO_CENSUS
        OutputDebugStringA(flowCensus);
#endif
        censusLastNoteOns = self->sf2Telemetry_.noteOns;
        censusLastFenceDrop = self->fenceSuppressedNoteOns_;
        censusLastStale = self->telemetry_.staleNoteOnsSkipped;
        censusLastShed = self->shedAtomic_.load(std::memory_order_relaxed);
        censusLastCc = self->ccCollapsedCount_;
    }
    // Per-voice phase rotation is applied inside RenderBlock (per-voice, at
    // each mix site), so Coherent mode (no state allocated) stays bit-exact
    // and non-Coherent modes never touch loudness or gain-reduction inputs.
    // Filter the final limited samples in the same loop so the 3 Hz cutoff
    // neither changes gain detection nor requires another memory pass.
    self->reverb.Process(output, numFrames, 2);
    self->limiter.Process(output, numFrames, 2, self->postHighPass);
#if !defined(SVMS_XP_COMPAT)
    self->liveRecorder_.Capture(output, numFrames);
#endif

    const uint64_t profilePostEnd = profileCallback ? __rdtsc() : 0u;

    LARGE_INTEGER renderEndQPC;
    QueryPerformanceCounter(&renderEndQPC);

    double elapsedUs = (double)(renderEndQPC.QuadPart - renderStartQPC.QuadPart)
                     / (double)self->qpcFreq * 1e6;
    double budgetUs = (double)numFrames / (double)self->sampleRate * 1e6;
    float cpuPct = (budgetUs > 0.0) ? (float)(elapsedUs / budgetUs * 100.0) : 0.0f;
    float schedulerPercent = 0.0f;
    float dispatchPercent = 0.0f;
    float synthesisPercent = 0.0f;
    float postPercent = 0.0f;
    if (profileCallback && profilePostEnd > profileCycleStart) {
        const uint64_t totalCycles = profilePostEnd - profileCycleStart;
        const uint64_t schedulerCycles = profileScheduleEnd - profileCycleStart;
        const uint64_t renderCycles = profileRenderEnd - profileScheduleEnd;
        const uint64_t dispatchCycles = (std::min)(
            self->dispatchCyclesCurrent_, renderCycles);
        const float scale = cpuPct / static_cast<float>(totalCycles);
        schedulerPercent = static_cast<float>(schedulerCycles) * scale;
        dispatchPercent = static_cast<float>(dispatchCycles) * scale;
        synthesisPercent = static_cast<float>(renderCycles - dispatchCycles) * scale;
        postPercent = static_cast<float>(profilePostEnd - profileRenderEnd) * scale;
    }
    self->callbackTiming_.Observe(cpuPct);
    if (traceRing) {
        const uint64_t freq = self->qpcFreq;
        const auto ns = [freq](int64_t ticks) -> uint32_t {
            if (ticks <= 0 || freq == 0u) return 0u;
            const uint64_t value =
                static_cast<uint64_t>(ticks) * 1000000000ull / freq;
            return static_cast<uint32_t>((std::min)(value, uint64_t{UINT32_MAX}));
        };
        const uint64_t waitTicks =
            self->producerWaitQpc_.load(std::memory_order_relaxed);
        const uint64_t index = traceRing->head.load(std::memory_order_relaxed);
        SVMS_CallbackTrace& r = traceRing->records[
            index & (CallbackTraceRing::kCapacity - 1u)];
        r.index = index;
        r.callback = self->callbackCount_;
        r.start_qpc = static_cast<uint64_t>(renderStartQPC.QuadPart);
        r.output_frame = traceOutputFrame;
        r.frames = numFrames;
        r.total_ns = ns(renderEndQPC.QuadPart - renderStartQPC.QuadPart);
        r.schedule_ns = ns(traceScheduleEndQPC.QuadPart - renderStartQPC.QuadPart);
        r.render_ns = ns(traceRenderEndQPC.QuadPart - traceScheduleEndQPC.QuadPart);
        r.post_ns = ns(renderEndQPC.QuadPart - traceRenderEndQPC.QuadPart);
        r.events = admittedThisBlock;
        r.render_events = evCount;
        r.late_events = clampedThisBlock;
        r.max_lateness_frames = static_cast<uint32_t>(
            (std::min)(maxLatenessThisBlock, uint64_t{UINT32_MAX}));
        r.skipped_frames = static_cast<uint32_t>(
            self->telemetry_.skippedOutputFrames - skippedFramesBefore);
        r.scheduled_backlog = scheduledAfterDispatch;
        r.ingress_backlog = static_cast<uint32_t>(self->midiIngress_.TotalSize());
        r.compiled_backlog =
            static_cast<uint32_t>(self->compiledPages_.ReadyEventCount());
        r.active_voices = vm->activeCount_;
        r.releasing_voices = vm->GetReleasingCount();
        r.render_paths = render->GetLastRenderPaths();
        r.note_ons = self->sf2Telemetry_.noteOns;
        r.voice_steals = vm->stealCount_;
        r.producer_waits = self->producerWaits_.load(std::memory_order_relaxed);
        r.cycles = __rdtsc() - traceCycleStart;
        r.wv_plan_cycles = render->GetWvPlanCycles() - traceWvPlan;
        r.wv_jobs_cycles = render->GetWvJobCycles() - traceWvJobs;
        r.wv_post_cycles = render->GetWvPostCycles() - traceWvPost;
        r.producer_wait_ns = freq != 0u
            ? waitTicks / freq * 1000000000ull +
                  waitTicks % freq * 1000000000ull / freq
            : 0u;
        traceRing->head.store(index + 1u, std::memory_order_release);
    }
    self->telemetry_.maxCallbackQPC = (std::max)(self->telemetry_.maxCallbackQPC,
        static_cast<uint64_t>(renderEndQPC.QuadPart - renderStartQPC.QuadPart));
    self->telemetry_.callbackP95Percent = self->callbackTiming_.Percentile(95u, 100u);
    self->telemetry_.callbackP99Percent = self->callbackTiming_.Percentile(99u, 100u);
    self->telemetry_.callbackP999Percent = self->callbackTiming_.Percentile(999u, 1000u);
    self->telemetry_.overBudgetCallbacks = self->callbackTiming_.overBudgetCallbacks;
    self->telemetry_.maxConsecutiveOverBudget =
        self->callbackTiming_.maxConsecutiveOverBudget;
    self->telemetry_.immediateRetirements = vm->retireImmediateCount_;
    self->telemetry_.voiceSteals = vm->stealCount_;
    self->telemetry_.submitted = self->submittedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.accepted = self->acceptedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.dropped = self->shedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.shedNoteOns = self->telemetry_.dropped;
    self->telemetry_.cancelledSubmissions =
        self->cancelledAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.currentVelocityCutoff =
        self->currentVelocityCutoffAtomic_.load(std::memory_order_relaxed);

    const uint32_t nextDebugIndex =
        (self->debugSnapshotIndex_.load(std::memory_order_relaxed) + 1u) & 1u;
    DriverDebugInfo& debug = self->debugSnapshots_[nextDebugIndex];
    debug = DriverDebugInfo{};
    debug.callbackCount = self->callbackCount_;
    debug.submitted = self->telemetry_.submitted;
    debug.accepted = self->telemetry_.accepted;
    debug.dispatched = self->telemetry_.dispatched;
    debug.noteOns = self->sf2Telemetry_.noteOns;
    debug.matchedRegions = self->sf2Telemetry_.exactRegionMatches;
    debug.configuredVoices = self->sf2Telemetry_.configuredVoices;
    debug.activeVoices = vm->activeCount_;
    debug.sampleDataFrames = self->sampleDataFrames;
    debug.sampleCount = self->sampleStoreCount;
    debug.soundFontLoaded = self->soundFontData && self->sampleDataStore ? 1u : 0u;
    debug.audioRunning = self->audioOutput && self->audioOutput->IsRunning() ? 1u : 0u;
    debug.audioHResult = self->audioOutput
        ? static_cast<int32_t>(self->audioOutput->GetLastError()) : 0;
    debug.renderPeak = self->sf2Telemetry_.renderPeak;

    SnappyVoiceStatistics& voiceStats =
        self->voiceStatisticsSnapshots_[nextDebugIndex];
    voiceStats.activeVoices = vm->activeCount_;
    voiceStats.freeVoices = vm->GetMaxVoices() - vm->activeCount_;
    voiceStats.voiceSteals = vm->stealCount_;

    LegacyDriverDebugInfo& legacy =
        self->legacyDebugSnapshots_[nextDebugIndex];
    legacy = LegacyDriverDebugInfo{};
    legacy.renderingTime = static_cast<float>(elapsedUs / 1000.0);
    for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
        legacy.activeVoices[channel] = vm->GetChannelActiveCount(channel);
    }
    legacy.audioLatency = budgetUs / 1000.0;
    legacy.audioBufferSize = numFrames;
    self->renderingTimeSnapshots_[nextDebugIndex] = legacy.renderingTime;
    self->debugSnapshotIndex_.store(nextDebugIndex, std::memory_order_release);
    if (self->diagnosticsEnabled_) {
        for (uint32_t velocity = 0; velocity < 128; ++velocity) {
            self->telemetry_.shedByVelocity[velocity] =
                self->shedByVelocityAtomic_[velocity].load(std::memory_order_relaxed);
        }
    }

    // Smooth CPU reading with a simple low-pass
    static float s_cpuSmoothed = 0.0f;
    s_cpuSmoothed += 0.1f * (cpuPct - s_cpuSmoothed);
    static float s_schedulerSmoothed = 0.0f;
    static float s_dispatchSmoothed = 0.0f;
    static float s_synthesisSmoothed = 0.0f;
    static float s_postSmoothed = 0.0f;
    s_schedulerSmoothed += 0.1f * (schedulerPercent - s_schedulerSmoothed);
    s_dispatchSmoothed += 0.1f * (dispatchPercent - s_dispatchSmoothed);
    s_synthesisSmoothed += 0.1f * (synthesisPercent - s_synthesisSmoothed);
    s_postSmoothed += 0.1f * (postPercent - s_postSmoothed);

    // Merged pool/load census, same 64-block cadence as sched/flow. This is
    // the only per-cadence load line: it folds the former diagnostics-window
    // timer flood (voices=/retire=/cpu=/p99=/over=/coalesced=) into one
    // census pinned to real audio callbacks.
    if (self->diagnosticsEnabled_ && (self->callbackCount_ & 63u) == 0u) {
        static uint64_t censusLastCoalesced = 0;
        char poolCensus[384];
        const uint32_t renderPaths = render->GetLastRenderPaths();
        char pathBuf[8];
        {
            char* p = pathBuf;
            if (renderPaths & 0x1u) *p++ = 'w';
            if (renderPaths & 0x2u) *p++ = 'd';
            if (renderPaths & 0x4u) *p++ = 's';
            if (p == pathBuf) *p++ = '-';
            *p = '\0';
        }
        std::snprintf(poolCensus, sizeof(poolCensus),
            "[SVMS] pool active=%u/%u retire=%u step=%u cpu=%.1f%% "
            "(disp=%.0f synth=%.0f sched=%.0f post=%.0f) p99=%.0f%% "
            "over=%llu coalesced+%llu(1/%u) path=%s vib=%u "
            "sos=%u sloop=%u tloop=%u rloop=%u gen=%u\n",
            (unsigned)vm->activeCount_, (unsigned)vm->GetMaxVoices(),
            (unsigned)vm->retireCount_,
            (unsigned)(self->correctnessMode_ ? 1u
                : ComputeDecimationStep(vm->activeCount_)),
            static_cast<double>(s_cpuSmoothed),
            static_cast<double>(s_dispatchSmoothed),
            static_cast<double>(s_synthesisSmoothed),
            static_cast<double>(s_schedulerSmoothed),
            static_cast<double>(s_postSmoothed),
            static_cast<double>(self->telemetry_.callbackP99Percent),
            (unsigned long long)self->telemetry_.overBudgetCallbacks,
            (unsigned long long)(self->coalescedAtomic_.load(
                std::memory_order_relaxed) - censusLastCoalesced),
            (unsigned)self->noteOnCollapse_.Threshold(),
            pathBuf,
            (unsigned)((renderPaths & 0x100u) != 0u),
            (unsigned)vm->GetRenderClassCount(svms::VoiceRenderClass::SustainedOneShot),
            (unsigned)vm->GetRenderClassCount(svms::VoiceRenderClass::SustainedLoop),
            (unsigned)vm->GetRenderClassCount(svms::VoiceRenderClass::TransientLoop),
            (unsigned)vm->GetRenderClassCount(svms::VoiceRenderClass::ReleaseLoop),
            (unsigned)vm->GetRenderClassCount(svms::VoiceRenderClass::Generic));
#if SVMS_AUDIO_CENSUS
        OutputDebugStringA(poolCensus);
#endif
        censusLastCoalesced = self->coalescedAtomic_.load(
            std::memory_order_relaxed);
        // Sparse fallback without the whole-voice path: name the event that
        // made the plan refuse (type value + data1/controller) so the
        // whitelist can be extended for the material actually played.
        if ((renderPaths & 0x4u) != 0u && (renderPaths & 0x1u) == 0u) {
            uint8_t refuseType = 0u;
            uint8_t refuseData1 = 0u;
            render->GetLastPlanRefusal(refuseType, refuseData1);
            char refuseCensus[96];
            std::snprintf(refuseCensus, sizeof(refuseCensus),
                "[SVMS] planRefuse: type=%u ctrl=%u\n",
                (unsigned)refuseType, (unsigned)refuseData1);
    #if SVMS_AUDIO_CENSUS
        OutputDebugStringA(refuseCensus);
#endif
        }
    }

    if (self->diagnosticsEnabled_) {
        static int diagTick = 0;
        if (++diagTick >= 1) {
            diagTick = 0;
            if (self->diagnosticsWindow_ || self->diagnosticsDebugOutput_) {
                // Extreme-polyphony rule: never scan activeList for
                // diagnostics; the releasing count comes from the exact
                // transition counter (VoiceManager::releasingCount_), so
                // only the sustain-held tally still needs a walk.
                const uint32_t releasingVoices = vm->GetReleasingCount();
                uint32_t sustainHeldVoices = 0;
                for (uint32_t position = 0; position < vm->activeCount_; ++position) {
                    const uint32_t voice = vm->activeList_[position];
                    sustainHeldVoices += vm->v.heldBySustain[voice] != 0;
                }
                DiagWindow_Update(vm->activeCount_, vm->GetMaxVoices(),
                                  releasingVoices, sustainHeldVoices,
                                  vm->stealCount_,
                                  self->coalescedAtomic_.load(
                                      std::memory_order_relaxed),
                                  self->noteOnCollapse_.Threshold(),
                                  s_cpuSmoothed, self->correctnessMode_ ? 1u
                                      : ComputeDecimationStep(vm->activeCount_),
                                  self->telemetry_.callbackP95Percent,
                                  self->telemetry_.callbackP99Percent,
                                  self->telemetry_.callbackP999Percent,
                                  self->telemetry_.overBudgetCallbacks,
                                  self->telemetry_.maxConsecutiveOverBudget,
                                  vm->retireCount_, vm->retireImmediateCount_,
                                  self->audioOutput && self->audioOutput->IsRunning(),
                                  self->audioOutput
                                      ? static_cast<int32_t>(self->audioOutput->GetLastError())
                                      : 0,
                                  self->soundFontData && self->sampleDataStore,
                                   self->sampleRate, self->bufferFrames,
                                   snap->masterVolume,
                                  UsesXPWaveOut(self->audioOutput),
                                  render->GetRenderBackend(),
                                  render->GetRenderThreadCount(),
                                  render->GetMulticoreEffectiveness(),
                                  s_schedulerSmoothed, s_dispatchSmoothed,
                                  s_synthesisSmoothed, s_postSmoothed,
                                  evCount, scheduledAfterDispatch,
                                  self->sf2Telemetry_);
            }
        }
    }

    // ── Audio→control snapshot (RuntimeLink V2) ────────────────────
    // The control thread publishes at ~30 Hz from this process-local
    // snapshot; the audio thread never touches shared memory.  The
    // releasing-voice count comes from the exact transition counter
    // (VoiceManager::releasingCount_), so no O(activeN) scan is needed;
    // the only remaining walk is the diag window's sustain tally.
    // Writes are relaxed atomics wrapped in a monotonic odd/even
    // sequence (2, 4, 6, ...): odd = writer inside, even = settled.
#if !defined(SVMS_XP_COMPAT)
    {
        svms::RuntimeAudioSnapshot& as = g_audioSnapshot;
        const uint32_t odd = as.sequence.load(std::memory_order_relaxed) | 1u;
        as.sequence.store(odd, std::memory_order_relaxed);
        RLV2_MemBarrier();
        as.tickMs.store(static_cast<uint32_t>(GetTickCount()),
                        std::memory_order_relaxed);
        as.activeVoices.store(vm->activeCount_, std::memory_order_relaxed);
        as.releasingVoices.store(vm->GetReleasingCount(), std::memory_order_relaxed);
        as.freeTop.store(vm->freeTop_, std::memory_order_relaxed);
        as.voiceSteals.store(vm->stealCount_, std::memory_order_relaxed);
        as.retiredCount.store(vm->retireCount_, std::memory_order_relaxed);
        as.retiredImmediateCount.store(vm->retireImmediateCount_, std::memory_order_relaxed);
        as.decimationStep.store(self->correctnessMode_ ? 1u
            : svms::ComputeDecimationStep(vm->activeCount_),
            std::memory_order_relaxed);
        as.renderPeakBits.store(FloatToU32Bits(self->sf2Telemetry_.renderPeak),
                                std::memory_order_relaxed);
        as.audioRunning.store(self->audioOutput && self->audioOutput->IsRunning()
            ? 1u : 0u, std::memory_order_relaxed);
        as.soundFontLoaded.store(self->soundFontData && self->sampleDataStore
            ? 1u : 0u, std::memory_order_relaxed);
        as.audioHResult.store(self->audioOutput
            ? static_cast<int32_t>(self->audioOutput->GetLastError()) : 0,
            std::memory_order_relaxed);
        as.cpuLoadPercentBits.store(FloatToU32Bits(s_cpuSmoothed),
                                    std::memory_order_relaxed);
        as.callbackP95PercentBits.store(
            FloatToU32Bits(self->telemetry_.callbackP95Percent),
            std::memory_order_relaxed);
        as.callbackP99PercentBits.store(
            FloatToU32Bits(self->telemetry_.callbackP99Percent),
            std::memory_order_relaxed);
        as.callbackP999PercentBits.store(
            FloatToU32Bits(self->telemetry_.callbackP999Percent),
            std::memory_order_relaxed);
        as.maxConsecutiveOverBudget.store(
            self->telemetry_.maxConsecutiveOverBudget,
            std::memory_order_relaxed);
        as.overBudgetCallbacks.store(self->telemetry_.overBudgetCallbacks,
                                     std::memory_order_relaxed);
        as.eventsSubmitted.store(self->telemetry_.submitted,
                                 std::memory_order_relaxed);
        as.eventsAccepted.store(self->telemetry_.accepted,
                                std::memory_order_relaxed);
        as.eventsDropped.store(self->telemetry_.dropped,
                               std::memory_order_relaxed);
        as.eventsDispatched.store(self->telemetry_.dispatched,
                                  std::memory_order_relaxed);
        as.limiterInputPeakLBits.store(FloatToU32Bits(self->limiter.inputPeakL),
                                       std::memory_order_relaxed);
        as.limiterInputPeakRBits.store(FloatToU32Bits(self->limiter.inputPeakR),
                                       std::memory_order_relaxed);
        as.limiterOutputPeakLBits.store(FloatToU32Bits(self->limiter.outputPeakL),
                                        std::memory_order_relaxed);
        as.limiterOutputPeakRBits.store(FloatToU32Bits(self->limiter.outputPeakR),
                                        std::memory_order_relaxed);
        as.limiterGainReductionDbBits.store(
            FloatToU32Bits(self->limiter.gainReductionDb),
            std::memory_order_relaxed);
        as.channelLimiterEnabled.store(self->channelLimiter.enabled ? 1u : 0u,
                                       std::memory_order_relaxed);
        for (uint32_t clChannel = 0u; clChannel < kChannelCount; ++clChannel) {
            as.channelLimiterGainReductionDbBits[clChannel].store(
                FloatToU32Bits(
                    self->channelLimiter.channel[clChannel].gainReductionDb),
                std::memory_order_relaxed);
            as.channelLimiterInputPeakBits[clChannel].store(
                FloatToU32Bits(
                    self->channelLimiter.channel[clChannel].inputPeak),
                std::memory_order_relaxed);
        }
        as.schedulerPercentBits.store(FloatToU32Bits(s_schedulerSmoothed),
                                      std::memory_order_relaxed);
        as.eventDispatchPercentBits.store(FloatToU32Bits(s_dispatchSmoothed),
                                          std::memory_order_relaxed);
        as.rawIngressCount.store(self->midiIngress_.TotalSize(),
                                 std::memory_order_relaxed);
        as.compiledPagedCount.store(self->compiledPages_.ReadyEventCount(),
                                    std::memory_order_relaxed);
        as.scheduledBacklogCount.store(scheduledAfterDispatch,
                                       std::memory_order_relaxed);
        RLV2_MemBarrier();
        as.sequence.store(odd + 1u, std::memory_order_release);
    }
#endif
}

// ── EventDispatcher callback ─────────────────────────────────────────────
// Called by RenderScalar::RenderBlock at each event's exact sub-sample
// position.  Voice allocation, release, CC updates all happen here so
// that the sub-sample phase offset and releaseStartInBlock are set at the precise frame.
void Driver::DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                  void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self) return;

    switch (event.type) {
        case RenderEventType::NoteOn: {
            const uint64_t globalFence =
                self->globalTerminationFence_.load(std::memory_order_acquire);
            const uint64_t channelFence = event.channel < kChannelCount
                ? self->channelTerminationFence_[event.channel].load(std::memory_order_acquire)
                : 0u;
            if (FenceSuppresses(event.ingressSequence, globalFence) ||
                FenceSuppresses(event.ingressSequence, channelFence)) {
                ++self->fenceSuppressedNoteOns_;
                break;
            }
            self->HandleNoteOn(event.channel, event.data1, event.data2,
                               false, nullptr, blockCursor);
            break;
        }
        case RenderEventType::NoteOff:
            self->HandleNoteOff(event.channel, event.data1, blockCursor);
            break;
        case RenderEventType::StaleNoteOffBatch:
            self->HandleStaleNoteOffBatch(event.channel, event.data1,
                                          event.data2, blockCursor);
            break;
        case RenderEventType::ControlChange:
            self->HandleControlChange(event.channel, event.data1, event.data2,
                                      blockCursor);
            break;
        case RenderEventType::ProgramChange:
            self->HandleProgramChange(event.channel, event.data1);
            break;
        case RenderEventType::PitchBend:
            self->HandlePitchBend(event.channel, event.data1, event.data2);
            break;
        case RenderEventType::ChannelPressure:
            self->HandleChannelPressure(event.channel, event.data1);
            break;
        case RenderEventType::AllNotesOff:
        case RenderEventType::AllSoundOff:
            // [HOOK] Overload ladder: these can trigger hard/panic release.
            break;
        case RenderEventType::Reset:
            if (self->voiceManager) self->voiceManager->Reset();
            if (self->channelCache) self->channelCache->Reset();
            self->sysexMasterVolume_ = 1.0f;
            self->sysexMasterFineTune_ = 0.0f;
            self->sysexMasterTranspose_ = 0.0f;
            if (self->channelCache && self->configSnapshot) {
                self->channelCache->SetMasterVolume(
                    self->configSnapshot->masterVolume);
                self->channelCache->RebuildCache(
                    *self->configSnapshot,
                    static_cast<float>(self->sampleRate));
            }
            self->RefreshSelectedPresets();
            std::fill(std::begin(self->channelPitchBendRatio_),
                      std::end(self->channelPitchBendRatio_), 1.0f);
            for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
                self->channelCache->SetBendRatio(
                    static_cast<uint8_t>(channel), 1.0f);
                ++self->channelLaunchRevision_[channel];
            }
            self->nextPlayIndex_ = 1;
            self->postHighPass.Reset();
            self->reverb.Reset();
            self->limiter.Reset();
            break;
        case RenderEventType::MasterVolume: {
            const uint16_t value = static_cast<uint16_t>(
                event.data1 | (static_cast<uint16_t>(event.data2) << 7u));
            self->sysexMasterVolume_ =
                static_cast<float>(value) / 16383.0f;
            if (self->channelCache && self->configSnapshot) {
                const float effective = self->configSnapshot->masterVolume *
                    self->sysexMasterVolume_;
                self->channelCache->SetMasterVolume(effective);
                self->channelCache->RebuildCache(
                    *self->configSnapshot,
                    static_cast<float>(self->sampleRate));
                if (self->voiceManager) {
                    for (uint8_t channel = 0u;
                         channel < kChannelCount; ++channel) {
                        self->voiceManager->MarkChannelMixStale(
                            channel,
                            self->channelCache->GetParams()[channel]);
                        self->channelCache->SetBendRatio(
                            channel, self->channelPitchBendRatio_[channel]);
                    }
                }
                self->appliedMasterVolume_ = effective;
            }
            break;
        }
        case RenderEventType::MasterFineTune: {
            const uint16_t value = static_cast<uint16_t>(
                event.data1 | (static_cast<uint16_t>(event.data2) << 7u));
            self->sysexMasterFineTune_ =
                static_cast<float>(static_cast<int32_t>(value) - 8192) /
                8192.0f;
            self->RefreshAllPitchIncrements();
            break;
        }
        case RenderEventType::MasterTranspose:
            self->sysexMasterTranspose_ = static_cast<float>(
                static_cast<int32_t>(event.data1) - 64);
            self->RefreshAllPitchIncrements();
            break;
        case RenderEventType::RhythmPart:
            if (self->channelCache && event.channel < kChannelCount) {
                self->channelCache->SetRhythmPart(event.channel, event.data1);
                uint32_t presetIndex = 0u;
                uint8_t soundFontIndex = 0u;
                if (ResolveChannelPreset(self->activeSoundFontStack_,
                        *self->channelCache, event.channel,
                        &soundFontIndex, &presetIndex)) {
                    self->channelSoundFontIndex_[event.channel] =
                        soundFontIndex;
                    self->channelCache->SetSelectedPreset(
                        event.channel, static_cast<uint16_t>(presetIndex));
                } else {
                    self->channelCache->SetSelectedPreset(
                        event.channel, UINT16_MAX);
                }
                ++self->channelLaunchRevision_[event.channel];
            }
            break;
    }
}

void Driver::DispatchRenderEventBatch(const RenderEvent* events,
                                      uint32_t eventCount,
                                      uint32_t blockCursor, void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self || !events) return;
    const uint64_t profileBegin = self->diagnosticsEnabled_ ? __rdtsc() : 0u;

    // The renderer has already grouped this range by exact output frame and
    // ingress sequence. Process maximal note-on runs directly; any state or
    // termination event breaks the run and goes through the full dispatcher.
    uint32_t index = 0u;
    uint64_t deferredNoteOns = 0u;
    uint64_t deferredMatches = 0u;
    uint64_t deferredConfigured = 0u;
    const bool allowStateCoalescing =
        self->overflowMode_.load(std::memory_order_relaxed) ==
        EventOverflowMode::PriorityVelocity;
    const auto isCoalescibleStateWrite = [](const RenderEvent& event) {
        switch (event.type) {
            case RenderEventType::ControlChange:
                // These controllers only replace channel state. Sustain,
                // reset and termination controllers have lifecycle side
                // effects and must always remain literal events.
                return event.data1 == 0u || event.data1 == 7u ||
                       event.data1 == 10u || event.data1 == 11u ||
                       event.data1 == 32u;
            case RenderEventType::ProgramChange:
            case RenderEventType::PitchBend:
            case RenderEventType::MasterVolume:
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
            case RenderEventType::RhythmPart:
                return true;
            default:
                return false;
        }
    };
    const auto hasSameStateTarget = [](const RenderEvent& left,
                                       const RenderEvent& right) {
        if (left.type != right.type) return false;
        switch (left.type) {
            case RenderEventType::ControlChange:
                return left.channel == right.channel &&
                       left.data1 == right.data1;
            case RenderEventType::ProgramChange:
            case RenderEventType::PitchBend:
            case RenderEventType::RhythmPart:
                return left.channel == right.channel;
            case RenderEventType::MasterVolume:
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
                return true;
            default:
                return false;
        }
    };
    while (index < eventCount) {
        if (events[index].type == RenderEventType::NoteOff) {
            // Strict lossless mode retains literal ingress ordering. It may
            // still combine an adjacent identical channel/key run into one
            // counted operation because no event can observe an intermediate
            // state, but it never reorders interleaved keys.
            if (self->overflowMode_.load(std::memory_order_relaxed) !=
                EventOverflowMode::PriorityVelocity) {
                const uint8_t channel = events[index].channel;
                const uint8_t note = events[index].data1;
                uint32_t runEnd = index + 1u;
                while (runEnd < eventCount &&
                       events[runEnd].type == RenderEventType::NoteOff &&
                       events[runEnd].channel == channel &&
                       events[runEnd].data1 == note) {
                    ++runEnd;
                }
                uint32_t remaining = runEnd - index;
                while (remaining != 0u) {
                    const uint8_t batch = static_cast<uint8_t>(
                        (std::min)(remaining, 255u));
                    self->HandleStaleNoteOffBatch(channel, note, batch,
                                                  blockCursor);
                    remaining -= batch;
                }
                index = runEnd;
                continue;
            }
            // All events in this callback invocation share an exact output
            // frame. Aggregate a maximal note-off-only run by channel/key;
            // controllers, note-ons and termination events remain hard
            // boundaries. Releasing A,B,A is observably identical to A,A,B
            // before the next boundary, while avoiding three oldest-
            // generation traversals when one counted operation is enough.
            uint32_t generation = ++self->noteOffBatchGeneration_;
            if (generation == 0u) {
                std::memset(self->noteOffBatchStamp_, 0,
                            sizeof(self->noteOffBatchStamp_));
                generation = ++self->noteOffBatchGeneration_;
            }
            uint32_t keyCount = 0u;
            uint32_t runEnd = index;
            const uint32_t multiplicityLimit = self->voiceManager
                ? self->voiceManager->GetMaxVoices() : kMaxPolyphony;
            while (runEnd < eventCount &&
                   events[runEnd].type == RenderEventType::NoteOff) {
                const RenderEvent& event = events[runEnd++];
                if (event.channel >= kChannelCount || event.data1 >= kNoteCount)
                    continue;
                const uint32_t key =
                    static_cast<uint32_t>(event.channel) * kNoteCount +
                    event.data1;
                if (self->noteOffBatchStamp_[key] != generation) {
                    self->noteOffBatchStamp_[key] = generation;
                    self->noteOffBatchCount_[key] = 0u;
                    self->noteOffBatchKeys_[keyCount++] =
                        static_cast<uint16_t>(key);
                }
                if (self->noteOffBatchCount_[key] < multiplicityLimit)
                    ++self->noteOffBatchCount_[key];
            }
            for (uint32_t keyIndex = 0u; keyIndex < keyCount; ++keyIndex) {
                const uint32_t key = self->noteOffBatchKeys_[keyIndex];
                uint32_t remaining = self->noteOffBatchCount_[key];
                const uint8_t channel = static_cast<uint8_t>(key / kNoteCount);
                const uint8_t note = static_cast<uint8_t>(key % kNoteCount);
                while (remaining != 0u) {
                    const uint8_t batch = static_cast<uint8_t>(
                        (std::min)(remaining, 255u));
                    self->HandleStaleNoteOffBatch(channel, note, batch,
                                                  blockCursor);
                    remaining -= batch;
                }
            }
            index = runEnd;
            continue;
        }
        if (events[index].type != RenderEventType::NoteOn) {
            // This callback contains one exact output frame in established
            // ingress order. Adjacent writes to the same stateless target
            // have no observable intermediate sample, so priority mode can
            // apply only the final value. Never cross another event, and keep
            // strict-lossless mode completely literal.
            if (allowStateCoalescing &&
                isCoalescibleStateWrite(events[index])) {
                uint32_t runEnd = index + 1u;
                while (runEnd < eventCount &&
                       hasSameStateTarget(events[index], events[runEnd])) {
                    ++runEnd;
                }
                DispatchRenderEvent(events[runEnd - 1u], blockCursor, self);
                index = runEnd;
                continue;
            }
            DispatchRenderEvent(events[index++], blockCursor, self);
            continue;
        }
        const uint64_t globalFence =
            self->globalTerminationFence_.load(std::memory_order_acquire);
        while (index < eventCount &&
               events[index].type == RenderEventType::NoteOn) {
            const uint8_t runChannel = events[index].channel;
            const uint8_t runNote = events[index].data1;
            const uint8_t runVelocity = events[index].data2;
            uint32_t runEnd = index + 1u;
            while (runEnd < eventCount &&
                   events[runEnd].type == RenderEventType::NoteOn &&
                   events[runEnd].channel == runChannel &&
                   events[runEnd].data1 == runNote &&
                   events[runEnd].data2 == runVelocity) {
                ++runEnd;
            }

            // A repeated chopped note on one exact frame has immutable SF2,
            // preset, pitch and channel state. Resolve its prepared launch
            // plan once, then reuse it for the rest of this run. No event is
            // moved and a state event above remains a hard batch boundary.
            const NoteLaunchPlanCacheEntry* exactFramePlan = nullptr;

            // ── Batched steal-candidate selection (hot-toggleable) ─────────
            // correctnessMode_ is the established optimized-vs-exact toggle
            // (plain audio-thread bool, read exactly like the render callback
            // and telemetry paths do). When it is OFF, each note launch in
            // this run is allowed to batch its steal-victim selection through
            // VoiceManager::PopStealCandidates inside LaunchVoiceGroup —
            // batched per launch transaction, where the victim order is
            // provably identical to the sequential per-layer pops (no
            // candidate insertions happen inside one launch; commits land
            // only after the allocation loop). See the FLAG note in
            // LaunchVoiceGroup for why the batching is deliberately scoped
            // per launch instead of per run. With correctness mode ON the
            // flag below is cleared and every selection takes the unchanged
            // per-layer PopStealCandidate path — zero behavioral or
            // performance difference.
            if (self->voiceManager)
                self->voiceManager->SetStealBatchingEnabled(
                    !self->correctnessMode_);
            // The fence cannot move inside a run: runs break on any
            // non-NoteOn event, and only a dispatched CC120/CC123/reset can
            // publish a fence — so one load covers the whole run.
            const uint64_t runChannelFence =
                runChannel < kChannelCount
                    ? self->channelTerminationFence_[runChannel].load(
                        std::memory_order_acquire)
                    : 0u;
            for (; index < runEnd; ++index) {
                const RenderEvent& event = events[index];
                if (FenceSuppresses(event.ingressSequence, globalFence) ||
                    FenceSuppresses(event.ingressSequence, runChannelFence) ||
                    event.channel >= kChannelCount || event.data1 >= kNoteCount) {
                    if (FenceSuppresses(event.ingressSequence, globalFence) ||
                        FenceSuppresses(event.ingressSequence, runChannelFence)) {
                        ++self->fenceSuppressedNoteOns_;
                    }
                    continue;
                }
                ++deferredNoteOns;
                const uint64_t delta = self->HandleNoteOn(
                    event.channel, event.data1, event.data2, true,
                    exactFramePlan, blockCursor);
                deferredMatches += static_cast<uint32_t>(delta);
                deferredConfigured += static_cast<uint32_t>(delta >> 32u);

                if (!exactFramePlan && self->activeSoundFontStack_ &&
                    self->channelCache) {
                    const NoteLaunchPlanCacheEntry* candidate =
                        self->noteLaunchHotCache_[runChannel][runNote];
                    const uint32_t preset =
                        self->channelCache->GetSelectedPreset(runChannel);
                    const uint8_t soundFontIndex =
                        self->channelSoundFontIndex_[runChannel];
                    if (candidate && candidate->soundFontGeneration ==
                            self->soundFontGeneration_ &&
                        candidate->channelRevision ==
                            self->channelLaunchRevision_[runChannel] &&
                        candidate->presetIndex == preset &&
                        candidate->soundFontIndex == soundFontIndex &&
                        candidate->channel == runChannel &&
                        candidate->note == runNote &&
                        candidate->velocity == (runVelocity & 0x7fu) &&
                        candidate->count != 0u &&
                        candidate->count <= kNoteRegionCacheLayers) {
                        exactFramePlan = candidate;
                    }
                }
            }
        }
    }
    self->sf2Telemetry_.noteOns += deferredNoteOns;
    self->sf2Telemetry_.exactRegionMatches += deferredMatches;
    self->sf2Telemetry_.configuredVoices += deferredConfigured;
    if (self->diagnosticsEnabled_)
        self->dispatchCyclesCurrent_ += __rdtsc() - profileBegin;
}

uint32_t Driver::ResolveNoteRegions(const SoundFontBundle* bank,
                                    uint8_t soundFontIndex,
                                    uint32_t presetIndex, uint8_t note,
                                    uint8_t velocity,
                                    const SFSampleRegion** outRegions,
                                    uint32_t outCapacity) {
    const SF2Data* data = bank ? bank->data : nullptr;
    if (!data || !outRegions || presetIndex >= data->presetCount)
        return 0u;

    const uint32_t tag = (static_cast<uint32_t>(soundFontIndex) << 23u) |
        (presetIndex << 14u) |
        (static_cast<uint32_t>(note) << 7u) | velocity;
    uint32_t hash = tag;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    const uint32_t slot = hash & (kNoteRegionCacheSize - 1u);
    NoteRegionCacheEntry& cached = noteRegionCache_[slot];
    if (cached.tag == tag && cached.count <= kNoteRegionCacheLayers) {
        ++telemetry_.noteRegionCacheHits;
        const uint32_t count = cached.count;
        const uint32_t copied = (std::min)(count, outCapacity);
        for (uint32_t i = 0; i < copied; ++i)
            outRegions[i] = &data->regions[cached.regionIndices[i]];
        return count;
    }

    ++telemetry_.noteRegionCacheMisses;
    const uint32_t count = sf2_find_regions(data, presetIndex, note,
                                            velocity, outRegions, outCapacity);
    if (count <= kNoteRegionCacheLayers && count <= outCapacity) {
        cached.tag = tag;
        cached.count = static_cast<uint16_t>(count);
        cached.reserved = 0u;
        for (uint32_t i = 0; i < count; ++i) {
            cached.regionIndices[i] = static_cast<uint32_t>(
                outRegions[i] - data->regions);
        }
    }
    return count;
}

void Driver::RefreshSelectedPresets() {
    if (!channelCache || !activeSoundFontStack_) return;
    for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
        uint32_t presetIndex = 0u;
        uint8_t soundFontIndex = 0u;
        if (ResolveChannelPreset(activeSoundFontStack_, *channelCache, channel,
                                 &soundFontIndex, &presetIndex)) {
            channelSoundFontIndex_[channel] = soundFontIndex;
            channelCache->SetSelectedPreset(channel,
                static_cast<uint16_t>(presetIndex));
        } else {
            channelSoundFontIndex_[channel] = 0u;
            channelCache->SetSelectedPreset(channel, UINT16_MAX);
        }
    }
}

uint64_t Driver::HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity,
                              bool deferLifetimeCounters,
                              const NoteLaunchPlanCacheEntry* exactFramePlan,
                              uint32_t blockOffset) {
    if (!channelCache || !voiceManager) return 0u;
    if (channel >= kChannelCount || note >= kNoteCount) return 0u;
    velocity &= 0x7fu;

    if (!deferLifetimeCounters) ++sf2Telemetry_.noteOns;
    const bool captureDetail = captureSf2Detail_;

    const float velGain = configuredVelocityGain_[velocity];
    if (velGain <= 0.0f) return 0u;

    channelCache->NoteOn(channel, note, velocity);

    if (!activeSoundFontStack_ || !sampleDataStore) return 0u;

    // Bank/program handlers commit this cache at their exact event frame.
    // The fallback covers reset and SoundFont-swap boundaries only; the
    // multi-million-NPS path therefore avoids scanning the preset table for
    // every repeated note.
    uint32_t presetIndex = channelCache->GetSelectedPreset(channel);
    uint8_t soundFontIndex = channelSoundFontIndex_[channel];
    SoundFontBundle* bank = SoundFontBankAt(activeSoundFontStack_,
                                            soundFontIndex);
    if (!bank || !bank->data || !bank->samples ||
        presetIndex >= bank->data->presetCount) {
        if (!ResolveChannelPreset(activeSoundFontStack_, *channelCache, channel,
                                  &soundFontIndex, &presetIndex)) {
            ++sf2Telemetry_.invalidPresets;
            return 0u;
        }
        channelSoundFontIndex_[channel] = soundFontIndex;
        channelCache->SetSelectedPreset(channel,
                                        static_cast<uint16_t>(presetIndex));
        bank = SoundFontBankAt(activeSoundFontStack_, soundFontIndex);
        if (!bank || !bank->data || !bank->samples) return 0u;
    }
    SF2Data* data = bank->data;
    // Probe the complete launch-plan cache before doing even the cached
    // region lookup.  The former ordering resolved/copied regions and
    // revalidated every layer before discovering that the fully prepared
    // plan was already present.  At Black-MIDI rates that redundant work is
    // paid close to a million times per second.
    auto planMatches = [&](const NoteLaunchPlanCacheEntry* entry) {
        return entry &&
            entry->soundFontGeneration == soundFontGeneration_ &&
            entry->channelRevision == channelLaunchRevision_[channel] &&
            entry->presetIndex == presetIndex &&
            entry->soundFontIndex == soundFontIndex &&
            entry->channel == channel &&
            entry->note == note && entry->velocity == velocity &&
            entry->count != 0u &&
            entry->count <= kNoteRegionCacheLayers;
    };
    NoteLaunchPlanCacheEntry* launchCache = const_cast<
        NoteLaunchPlanCacheEntry*>(exactFramePlan);
    bool launchCacheHit = exactFramePlan != nullptr;
    if (!launchCacheHit)
        launchCache = noteLaunchHotCache_[channel][note];
    if (!launchCacheHit) launchCacheHit = planMatches(launchCache);
    if (!launchCacheHit) {
        uint32_t launchHash = presetIndex * 0x9e3779b9u;
        launchHash ^= static_cast<uint32_t>(note) * 0x85ebca6bu;
        launchHash ^= static_cast<uint32_t>(velocity) * 0xc2b2ae35u;
        launchHash ^= static_cast<uint32_t>(channel) * 0x27d4eb2fu;
        launchHash ^= static_cast<uint32_t>(soundFontIndex) * 0xd3a2646cu;
        launchHash ^= channelLaunchRevision_[channel] * 0x165667b1u;
        launchHash ^= launchHash >> 16u;
        launchCache = &noteLaunchPlanCache_[
            launchHash & (kNoteRegionCacheSize - 1u)];
        launchCacheHit = planMatches(launchCache);
        noteLaunchHotCache_[channel][note] = launchCache;
    }

    const bool edo31 = engineConfig_.tuningEdo == 31u && !channelCache->IsPercussion(channel);
    const uint8_t regionNote = MidiRegionKey(note, edo31);
    uint32_t matchCount = launchCacheHit ? launchCache->count
        : ResolveNoteRegions(bank, soundFontIndex, presetIndex, regionNote, velocity,
                             noteRegionScratch_, kMaxMatchingRegions);
    if (matchCount > kMaxMatchingRegions) {
        ++telemetry_.allocationFailures;
        return 0u;
    }

    if (matchCount == 0) {
        // Region fallback: some SoundFont presets cover only part of the
        // keyboard (or lack velocity layers at this spot). Resolve against
        // the bank's widest-coverage preset so incomplete instruments stay
        // audible instead of silently dropping notes. The launch-plan cache
        // still stores the result under the original preset tag, so repeat
        // note-ons pay no repeated lookup.
        const uint16_t fallbackPreset = data->fallbackPresetIndex;
        if (fallbackPreset < data->presetCount &&
            fallbackPreset != presetIndex) {
            matchCount = ResolveNoteRegions(bank, soundFontIndex,
                fallbackPreset, regionNote, velocity, noteRegionScratch_,
                kMaxMatchingRegions);
        }
        if (matchCount == 0) {
            ++telemetry_.zeroMatchedRegions;
            ++sf2Telemetry_.zeroMatchedRegions;
            return 0u;
        }
        if (!deferLifetimeCounters)
            ++sf2Telemetry_.fallbackRegionMatches;
    }

    // Validate every layer before mutating the voice pool. A malformed
    // SoundFont region must reject the complete generation, never leave a
    // partial instrument whose remaining layers become audible artifacts.
    if (!launchCacheHit) {
        for (uint32_t mi = 0; mi < matchCount; ++mi) {
            const SFSampleRegion* region = noteRegionScratch_[mi];
            if (!region || region->sampleIndex >= bank->sampleCount) {
                ++sf2Telemetry_.invalidRegions;
                return 0u;
            }
            const uint32_t regionIndex = static_cast<uint32_t>(
                region - data->regions);
            const bool valid = bank->preparedRegions &&
                    regionIndex < bank->preparedRegionCount
                ? bank->preparedRegions[regionIndex].valid != 0u
                : sf2_validate_region(data, region);
            if (!valid) {
                ++sf2Telemetry_.invalidSampleRanges;
                return 0u;
            }
        }
    }
    if (!deferLifetimeCounters)
        sf2Telemetry_.exactRegionMatches += matchCount;

    // All regions layered by this one MIDI note-on share a generation.  A
    // later note-off must release only this generation's oldest outstanding
    // retrigger, not every voice with the same channel/key.
    if (nextPlayIndex_ == 0 || nextPlayIndex_ >= UINT32_MAX - 1)
        nextPlayIndex_ = 1;
    const uint32_t playIndex = nextPlayIndex_++;

    const float sr = static_cast<float>(
        sampleRate > 0 ? sampleRate : 44100u);
    const float pitchBendSemitones =
        channelCache->GetPitchBendSemitones(channel) +
        sysexMasterFineTune_ + sysexMasterTranspose_;
    const float commonBendRatio = channelPitchBendRatio_[channel];
    const VoiceConfiguration* launchSetups = noteLaunchScratch_;
    if (launchCacheHit) {
        // The cached setup is immutable. playIndex is the only per-note field;
        // pass it separately instead of copying every layer into scratch just
        // to patch four bytes at multi-million-note rates.
        launchSetups = launchCache->setup;
    } else for (uint32_t mi = 0; mi < matchCount; ++mi) {
        const SFSampleRegion* matchedRegion = noteRegionScratch_[mi];
        const uint32_t matchedRegionIndex = static_cast<uint32_t>(
            matchedRegion - data->regions);
        const PreparedSF2Region* prepared =
            bank->preparedRegions &&
                    matchedRegionIndex < bank->preparedRegionCount
                ? &bank->preparedRegions[matchedRegionIndex] : nullptr;
        uint32_t sampleIndex = matchedRegion->sampleIndex;
        const SF2Sample& samp = bank->samples[sampleIndex];

        const float bendScale = prepared ? prepared->bendScale
            : static_cast<float>(matchedRegion->scaleTuning != 0
                ? matchedRegion->scaleTuning : 100) / 100.0f;
        float basePhaseStep;
        if (prepared) {
            basePhaseStep = prepared->basePhaseStep[note];
        } else {
            const int rootKey = matchedRegion->rootKey >= 0
                ? matchedRegion->rootKey : static_cast<int>(samp.originalPitch);
            const float tune = static_cast<float>(matchedRegion->coarseTune) +
                static_cast<float>(matchedRegion->fineTune) / 100.0f;
            const float semitones =
                (static_cast<float>(note) + tune - static_cast<float>(rootKey)) *
                bendScale;
            const float sourceRate = static_cast<float>(
                samp.sampleRate > 0 ? samp.sampleRate : 44100u);
            const float outputRate = static_cast<float>(
                sampleRate > 0 ? sampleRate : 44100u);
            basePhaseStep = sourceRate / outputRate *
                powf(2.0f, semitones / 12.0f);
        }
        basePhaseStep = RetunePhaseStep(basePhaseStep, note, bendScale, edo31);
        const float bendRatio = bendScale == 1.0f || pitchBendSemitones == 0.0f
            ? commonBendRatio
            : powf(2.0f, pitchBendSemitones * bendScale / 12.0f);
        float phaseStep = basePhaseStep * bendRatio;
        // A corrupt SF2 pitch generator must not poison the scalar loop with
        // NaN/Inf phase values: float-to-integer conversion then pins sample
        // lookup unpredictably and silently poisons the mixed output.
        if (!std::isfinite(phaseStep) || phaseStep <= 0.0f) {
            phaseStep = 1.0f;
        }

        uint32_t sStart = bank->sampleBase +
            static_cast<uint32_t>(matchedRegion->startOffset);
        uint32_t sEnd = bank->sampleBase +
            static_cast<uint32_t>(matchedRegion->endOffset);
        uint32_t sLoopStart = bank->sampleBase +
            static_cast<uint32_t>(matchedRegion->loopStartOffset);
        uint32_t sLoopEnd = bank->sampleBase +
            static_cast<uint32_t>(matchedRegion->loopEndOffset);
        uint8_t loopMode = matchedRegion->loopMode;

        float initialGain;
        float sustainLevel;
        uint32_t delaySamples;
        uint32_t holdSamples;
        uint32_t attackSamples;
        uint32_t decaySamples;
        float decaySlope;
        float releaseDecay;
        uint32_t releaseSamples;
        float regionPanLeft;
        float regionPanRight;
        if (prepared) {
            initialGain = velGain * prepared->attenuationGain;
            sustainLevel = prepared->sustainLevel;
            delaySamples = prepared->delaySamples;
            holdSamples = prepared->holdSamples;
            attackSamples = prepared->attackSamples;
            decaySamples = prepared->decaySamples;
            decaySlope = prepared->decaySlope;
            releaseDecay = prepared->releaseDecay;
            releaseSamples = prepared->releaseSamples;
            regionPanLeft = prepared->panLeft;
            regionPanRight = prepared->panRight;
        } else {
            initialGain = velGain;
            if (matchedRegion->initialAttenuation > 0 ||
                (data->isSfz && matchedRegion->initialAttenuation != 0))
                initialGain *= InitialAttenuationToGain(
                    static_cast<float>(matchedRegion->initialAttenuation));
            sustainLevel = SustainAttenuationToGain((std::max)(
                0.0f, static_cast<float>(matchedRegion->sustainVolEnv)));
            if (sustainLevel > 1.0f) sustainLevel = 1.0f;
            const float delaySeconds = TimecentsToSeconds(matchedRegion->delayVolEnv);
            const float holdSeconds = TimecentsToSeconds(matchedRegion->holdVolEnv);
            const float attackSeconds = TimecentsToSeconds(matchedRegion->attackVolEnv);
            const float decaySeconds = TimecentsToSeconds(matchedRegion->decayVolEnv);
            const float releaseSeconds = TimecentsToSeconds(matchedRegion->releaseVolEnv);
            delaySamples = delaySeconds > 0.0f
                ? static_cast<uint32_t>(delaySeconds * sr) : 0u;
            holdSamples = holdSeconds > 0.0f
                ? static_cast<uint32_t>(holdSeconds * sr) : 0u;
            attackSamples = attackSeconds > 0.0001f
                ? static_cast<uint32_t>(attackSeconds * sr) : 0u;
            decaySamples = decaySeconds > 0.0001f
                ? static_cast<uint32_t>(decaySeconds * sr) : 0u;
            decaySlope = 1.0f;
            if (decaySamples > 0u) {
                const float slope = -9.226f / static_cast<float>(decaySamples);
                decaySlope = expf(slope);
                if (sustainLevel > 0.0f && sustainLevel < 1.0f)
                    decaySamples = static_cast<uint32_t>(logf(sustainLevel) / slope);
            }
            releaseDecay = MakeReleaseDecay(releaseSeconds, sampleRate);
            releaseSamples = MakeReleaseSamples(releaseSeconds, sampleRate);
            regionPanLeft = 1.0f;
            regionPanRight = 1.0f;
            channelCache->ComputeSoundFontPan(matchedRegion->pan,
                                              regionPanLeft, regionPanRight);
        }
        const float attackGainStep = attackSamples > 0u
            ? initialGain / static_cast<float>(attackSamples) : 0.0f;

        VoiceConfiguration setup{};
        setup.sampleStart = sStart;
        setup.sampleEnd = sEnd;
        setup.loopStart = sLoopStart;
        setup.loopEnd = sLoopEnd;
        setup.delaySamples = delaySamples;
        setup.holdSamples = holdSamples;
        setup.attackSamples = attackSamples;
        setup.decaySamples = decaySamples;
        setup.releaseSamples = releaseSamples;
        setup.phaseStep = phaseStep;
        setup.basePhaseStep = basePhaseStep;
        setup.pitchBendScale = bendScale;
        setup.initialGain = initialGain;
        setup.sustainLevel = sustainLevel;
        setup.attackGainStep = attackGainStep;
        setup.decaySlope = decaySlope;
        setup.releaseDecay = releaseDecay;
        setup.gainLeft = regionPanLeft;
        setup.gainRight = regionPanRight;
        setup.presetIndex = static_cast<uint16_t>(presetIndex);
        setup.regionIndex = static_cast<uint16_t>(matchedRegionIndex);
        setup.exclusiveClass = matchedRegion->exclusiveClass > 0
            ? static_cast<uint16_t>(matchedRegion->exclusiveClass) : 0u;
        setup.offByClass = matchedRegion->offByClass < 0
            ? UINT16_MAX
            : (matchedRegion->offByClass > 0
                ? static_cast<uint16_t>(matchedRegion->offByClass) : 0u);
        setup.loopMode = loopMode;
        setup.sampleBacked = 1u;
        if (prepared) {
            setup.vibLfoToPitchCents = prepared->vibLfoToPitchCents;
            setup.vibLfoPhaseStep = prepared->vibLfoPhaseStep;
            setup.vibLfoDelaySamples = prepared->vibLfoDelaySamples;
            setup.filterA0 = prepared->filter.a0;
            setup.filterB1 = prepared->filter.b1;
            setup.filterB2 = prepared->filter.b2;
            setup.filterEnabled = prepared->filter.enabled;
        } else {
            const float vibDelaySeconds =
                TimecentsToSeconds(matchedRegion->delayVibLfo);
            setup.vibLfoToPitchCents =
                static_cast<float>(matchedRegion->vibLfoToPitch);
            setup.vibLfoPhaseStep =
                powf(2.0f, static_cast<float>(
                           matchedRegion->freqVibLfo) / 1200.0f) / sr;
            setup.vibLfoDelaySamples = vibDelaySeconds > 0.0f
                ? static_cast<uint32_t>(vibDelaySeconds * sr) : 0u;
            const PreparedVoiceFilter filter = PrepareVoiceLowPass(
                matchedRegion->filterType, matchedRegion->initialFilterFc,
                matchedRegion->initialFilterQ,
                static_cast<uint32_t>(sr));
            setup.filterA0 = filter.a0;
            setup.filterB1 = filter.b1;
            setup.filterB2 = filter.b2;
            setup.filterEnabled = filter.enabled;
        }
        noteLaunchScratch_[mi] = setup;
    }

    if (!launchCacheHit && matchCount <= kNoteRegionCacheLayers) {
        launchCache->soundFontGeneration = soundFontGeneration_;
        launchCache->channelRevision = channelLaunchRevision_[channel];
        launchCache->presetIndex = static_cast<uint16_t>(presetIndex);
        launchCache->soundFontIndex = soundFontIndex;
        launchCache->channel = channel;
        launchCache->note = note;
        launchCache->velocity = velocity;
        launchCache->count = static_cast<uint8_t>(matchCount);
        for (uint32_t layer = 0u; layer < matchCount; ++layer)
            launchCache->setup[layer] = noteLaunchScratch_[layer];
    }

    if (!voiceManager->LaunchVoiceGroup(
            channel, note, velocity, launchSetups, matchCount, playIndex,
            channelCache->GetParams()[channel], noteLaunchHandles_,
            blockOffset)) {
        ++telemetry_.allocationFailures;
        return matchCount;
    }

    if (!deferLifetimeCounters) sf2Telemetry_.configuredVoices += matchCount;
    const uint64_t lifetimeDelta = static_cast<uint64_t>(matchCount) |
        (static_cast<uint64_t>(matchCount) << 32u);
    if (!captureDetail) return lifetimeDelta;
    captureSf2Detail_ = false;
    sf2Telemetry_.lastChannel = channel;
    sf2Telemetry_.lastNote = note;
    sf2Telemetry_.lastVelocity = velocity;
    sf2Telemetry_.lastPreset = static_cast<uint16_t>(presetIndex);
    const uint32_t last = matchCount - 1u;
    const VoiceConfiguration& lastSetup = launchSetups[last];
    const VoiceHandle lastVoice = noteLaunchHandles_[last];
    const SFSampleRegion* lastRegion =
        &data->regions[lastSetup.regionIndex];
    sf2Telemetry_.lastRegion = lastSetup.regionIndex;
    sf2Telemetry_.lastSample = static_cast<uint16_t>(lastRegion->sampleIndex);
    sf2Telemetry_.lastSampleStart = lastSetup.sampleStart;
    sf2Telemetry_.lastSampleEnd = lastSetup.sampleEnd;
    sf2Telemetry_.lastInitialPeak =
        bank->regionInitialPeaks &&
                lastSetup.regionIndex < bank->regionInitialPeakCount
            ? bank->regionInitialPeaks[lastSetup.regionIndex] : 0.0f;
    sf2Telemetry_.lastVoiceGain = lastSetup.initialGain;
    sf2Telemetry_.lastMixGainL = voiceManager->v.mixGainL[lastVoice];
    sf2Telemetry_.lastMixGainR = voiceManager->v.mixGainR[lastVoice];
    sf2Telemetry_.lastDelaySamples = lastSetup.delaySamples;
    sf2Telemetry_.lastAttackSamples = lastSetup.attackSamples;
    sf2Telemetry_.lastFloatSample = static_cast<float>(sampleDataStore[lastSetup.sampleStart]) / 32768.0f;
    sf2Telemetry_.lastPhaseStep = lastSetup.phaseStep;
    sf2Telemetry_.lastPhase = voiceManager->v.phases[lastVoice];
    sf2Telemetry_.lastRelativeEnd = voiceManager->v.relEnd[lastVoice];
    sf2Telemetry_.lastSampleBacked = voiceManager->v.sampleBacked[lastVoice];
    sf2Telemetry_.lastVoiceHandle = lastVoice;
    return lifetimeDelta;
}

void Driver::HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset) {
    if (!channelCache || !voiceManager) return;

    bool sustain = channelCache->IsSustainActive(channel);
    channelCache->NoteOff(channel, note);

    // Use the voice's SF2-defined release — no adaptive release based on
    // pool pressure.  BASSMIDI and SnappySynth both use SF2-specified
    // release times; adaptive release causes audible inconsistency under
    // varying polyphony loads.
    const uint32_t playIndex = voiceManager->FindOldestPlayIndex(channel, note);
    if (playIndex == UINT32_MAX) return;
    voiceManager->NoteOffPlayIndex(channel, note, playIndex, sustain, blockOffset);
}

void Driver::HandleStaleNoteOffBatch(uint8_t channel, uint8_t note,
                                     uint8_t count,
                                     uint32_t blockOffset) {
    if (!channelCache || !voiceManager || count == 0u) return;
    const bool sustain = channelCache->IsSustainActive(channel);
    channelCache->NoteOff(channel, note);
    voiceManager->NoteOffOldestPlayIndices(channel, note, count, sustain,
                                           blockOffset);
}

void Driver::HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value,
                                 uint32_t blockOffset) {
    const bool sustainWasActive = channelCache && channelCache->IsSustainActive(channel);
    const bool sostenutoWasActive =
        channelCache && channelCache->IsSostenutoActive(channel);
    if (channelCache) channelCache->ControlChange(channel, controller, value);
    if (channel < kChannelCount &&
        (controller == 0u || controller == 32u || controller == 6u ||
         controller == 38u || controller == 96u || controller == 97u ||
         controller == 100u || controller == 101u)) {
        ++channelLaunchRevision_[channel];
    }

    if ((controller == 0 || controller == 32) && channelCache &&
        activeSoundFontStack_ &&
        channel < kChannelCount) {
        uint32_t presetIndex = 0u;
        uint8_t soundFontIndex = 0u;
        if (ResolveChannelPreset(activeSoundFontStack_, *channelCache, channel,
                                 &soundFontIndex, &presetIndex)) {
            channelSoundFontIndex_[channel] = soundFontIndex;
            channelCache->SetSelectedPreset(channel,
                                            static_cast<uint16_t>(presetIndex));
        } else {
            channelCache->SetSelectedPreset(channel, UINT16_MAX);
        }
    }

    if (controller == 64) {
        if (value < 64) voiceManager->ReleaseSustain(channel, blockOffset);
    } else if (controller == 66) {
        const bool sostenutoIsActive = value >= 64;
        if (!sostenutoWasActive && sostenutoIsActive)
            voiceManager->CaptureSostenuto(channel);
        else if (sostenutoWasActive && !sostenutoIsActive)
            voiceManager->ReleaseSostenuto(channel, blockOffset);
    }

    if (controller == 120) {
        voiceManager->SilenceChannelImmediate(channel);
    } else if (controller == 123) {
        voiceManager->ReleaseChannel(channel, blockOffset);
    } else if (controller == 121) {
        if (sustainWasActive)
            voiceManager->ReleaseSustain(channel, blockOffset);
        if (sostenutoWasActive)
            voiceManager->ReleaseSostenuto(channel, blockOffset);
    }

    if (controller == 121) {
        // Reset All Controllers also centers the wheel. Recompute the phase
        // increment of already sounding voices at this exact event frame.
        HandlePitchBend(channel, 0, 64);
    }

    if (channelCache && (controller == 6u || controller == 38u ||
                         controller == 96u || controller == 97u)) {
        const uint16_t wheel = channelCache->GetPitchBendValue(channel);
        HandlePitchBend(channel, static_cast<uint8_t>(wheel & 0x7fu),
                        static_cast<uint8_t>(wheel >> 7u));
    }

    // Controller events are dispatched before the sample at their target
    // frame. Rebuild now so existing voices and same-frame note-ons observe
    // the new channel state rather than waiting for the next callback.
    if (channelCache && configSnapshot &&
        (controller == 7 || controller == 10 || controller == 11 ||
         controller == 64 || controller == 121 || controller == 6 ||
         controller == 38 || controller == 96 || controller == 97 ||
         controller == 1)) {
        channelCache->RebuildChannel(channel, *configSnapshot,
                                     static_cast<float>(sampleRate));
        // Keep the vibrato pass on the exact sysex-aware common ratio even
        // when this rebuild recomputed the snapshot from channel cents.
        channelCache->SetBendRatio(channel, channelPitchBendRatio_[channel]);
        if (controller == 7 || controller == 10 || controller == 11 ||
            controller == 121) {
            // O(1): the per-voice fold is batched to the exact-frame
            // boundaries and launch entries (see MarkChannelMixStale).
            voiceManager->MarkChannelMixStale(
                channel, channelCache->GetParams()[channel]);
        }
        if (controller == 1 || controller == 121) {
            // Whole-voice pre-pass: report the post-rebuild modulation
            // depth as a vibrato row-op so the LFO gates at the exact
            // event frame inside the owning worker.  No-op elsewhere
            // (AdvanceVibratoSpan reads the snapshot per span).
            voiceManager->MarkChannelVibratoDepth(
                channel, channelCache->GetParams()[channel].modDepth);
        }
    }
}

void Driver::HandleChannelPressure(uint8_t channel, uint8_t value) {
    if (!channelCache || channel >= kChannelCount) return;
    channelCache->ChannelPressure(channel, value);
    if (!configSnapshot) return;
    channelCache->RebuildChannel(channel, *configSnapshot,
                                 static_cast<float>(sampleRate));
    channelCache->SetBendRatio(channel, channelPitchBendRatio_[channel]);
    if (voiceManager)
        voiceManager->MarkChannelVibratoDepth(
            channel, channelCache->GetParams()[channel].modDepth);
}

void Driver::HandleProgramChange(uint8_t channel, uint8_t program) {
    if (!channelCache || !activeSoundFontStack_ ||
        channel >= kChannelCount) return;

    // Validate against the bank currently selected on this channel before
    // committing the new program. Existing voices retain their stored region.
    const uint8_t oldProgram = channelCache->GetProgram(channel);
    channelCache->ProgramChange(channel, program);

    uint32_t presetIndex = 0;
    uint8_t soundFontIndex = 0u;
    if (ResolveChannelPreset(activeSoundFontStack_, *channelCache, channel,
                             &soundFontIndex, &presetIndex)) {
        channelSoundFontIndex_[channel] = soundFontIndex;
        channelCache->SetSelectedPreset(channel, static_cast<uint16_t>(presetIndex));
        ++channelLaunchRevision_[channel];
        return;
    }

    // Invalid selection: restore the prior program and leave the prior preset
    // active, matching TSF's failed preset-selection behavior.
    channelCache->ProgramChange(channel, oldProgram);
}

void Driver::HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb) {
    if (channelCache)
        channelCache->PitchBend(channel, static_cast<int16_t>((msb << 7) | lsb));

    if (!voiceManager || !channelCache || channel >= kChannelCount) return;
    ++channelLaunchRevision_[channel];

    const float bendSemitones = channelCache->GetPitchBendSemitones(channel) +
        sysexMasterFineTune_ + sysexMasterTranspose_;
    const float commonRatio = powf(2.0f, bendSemitones / 12.0f);
    channelPitchBendRatio_[channel] = commonRatio;
    channelCache->SetBendRatio(channel, commonRatio);
    // Rewrites phaseIncs inline — or, under the whole-voice pre-pass,
    // reports a channel op so the exact-frame application happens in the
    // owning worker (see VoiceManager::SetRowOpHooks).
    voiceManager->ApplyChannelBendRatio(channel, bendSemitones);
}

void Driver::RefreshAllPitchIncrements() {
    if (!voiceManager || !channelCache) return;
    for (uint8_t channel = 0u; channel < kChannelCount; ++channel) {
        ++channelLaunchRevision_[channel];
        const float semitones = channelCache->GetPitchBendSemitones(channel) +
            sysexMasterFineTune_ + sysexMasterTranspose_;
        const float commonRatio = powf(2.0f, semitones / 12.0f);
        channelPitchBendRatio_[channel] = commonRatio;
        channelCache->SetBendRatio(channel, commonRatio);
        voiceManager->ApplyChannelBendRatio(channel, semitones);
    }
}

} // namespace svms
