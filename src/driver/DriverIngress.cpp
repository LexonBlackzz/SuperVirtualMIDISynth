// Event ingress: MIDI input, Submit*, SysEx, event compiler thread.
//
// Everything a MIDI event goes through before the audio thread sees it:
//
//   1. Physical MIDI input     StartConfiguredMidiInput / MidiInputCallback
//   2. Single-event submit     SubmitShortMsg*, PrepareShortEvent
//                              (fences, coalescing, lane choice, shedding)
//   3. Batch submit            SubmitShortBatch*, SubmitMixedBatchFast,
//                              SubmitShardedBatch, SubmitMidiStreamCancellable
//   4. SysEx                   SubmitSystemExclusiveCancellable (GM/GS/XG ->
//                              internal engine messages)
//   5. Direct page feed        SubmitShortBatchDirect, FlushDirectPage:
//                              already-ordered batches skip the ring
//   6. Compiler thread         EventCompilerLoop: ring -> sorted event pages
//
// Producers (any thread) push TimestampedMidiEvent into the priority ingress
// ring; the compiler thread turns them into ScheduledRenderEvent pages; the
// audio callback imports the pages into the scheduler.  Sequence numbers are
// assigned at submit time and define the ingress order the renderer keeps.

#include "driver/DriverInternal.h"

namespace svms {

// ── 1. Physical MIDI input ───────────────────────────────────────────────

// Opens the configured physical input device through the system WinMM (never
// our own shim) and starts it with a ring of SysEx buffers.  Returns false and
// leaves routing off if the device is missing or cannot be started.
bool Driver::StartConfiguredMidiInput() {
    StopConfiguredMidiInput();
    if (!engineConfig_.midiInputEnabled) return true;

    using GetNumProc = UINT (WINAPI*)(void);
    using GetCapsProc = MMRESULT (WINAPI*)(UINT_PTR, LPMIDIINCAPSW, UINT);
    using OpenProc = MMRESULT (WINAPI*)(LPHMIDIIN, UINT, DWORD_PTR,
                                       DWORD_PTR, DWORD);
    using HeaderProc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    using StartProc = MMRESULT (WINAPI*)(HMIDIIN);
    GetNumProc getNum = reinterpret_cast<GetNumProc>(
        GetSystemWinmmProc("midiInGetNumDevs"));
    GetCapsProc getCaps = reinterpret_cast<GetCapsProc>(
        GetSystemWinmmProc("midiInGetDevCapsW"));
    OpenProc open = reinterpret_cast<OpenProc>(
        GetSystemWinmmProc("midiInOpen"));
    HeaderProc prepare = reinterpret_cast<HeaderProc>(
        GetSystemWinmmProc("midiInPrepareHeader"));
    HeaderProc addBuffer = reinterpret_cast<HeaderProc>(
        GetSystemWinmmProc("midiInAddBuffer"));
    StartProc start = reinterpret_cast<StartProc>(
        GetSystemWinmmProc("midiInStart"));
    if (!getNum || !getCaps || !open || !prepare || !addBuffer || !start) {
        OutputDebugStringA("[SVMS] MIDI input routing unavailable: system WinMM exports missing\n");
        return false;
    }

    const UINT deviceCount = getNum();
    if (deviceCount == 0u) {
        OutputDebugStringA("[SVMS] MIDI input routing enabled but no physical input devices were found\n");
        return false;
    }

    UINT selected = 0u;
    const std::wstring& requested = engineConfig_.midiInputDevice;
    if (!requested.empty() && _wcsicmp(requested.c_str(), L"default") != 0) {
        bool found = false;
        for (UINT index = 0u; index < deviceCount; ++index) {
            MIDIINCAPSW caps{};
            if (getCaps(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
                _wcsicmp(caps.szPname, requested.c_str()) == 0) {
                selected = index;
                found = true;
                break;
            }
        }
        if (!found) {
            const std::string requestedUtf8 = [&]() {
                if (requested.empty()) return std::string{};
                const int bytes = WideCharToMultiByte(
                    CP_UTF8, 0, requested.data(), static_cast<int>(requested.size()),
                    nullptr, 0, nullptr, nullptr);
                std::string value(bytes > 0 ? static_cast<size_t>(bytes) : 0u, '\0');
                if (bytes > 0) WideCharToMultiByte(
                    CP_UTF8, 0, requested.data(), static_cast<int>(requested.size()),
                    value.data(), bytes, nullptr, nullptr);
                return value;
            }();
            std::string warning = "[SVMS] configured MIDI input was not found: " +
                                  requestedUtf8 + "\n";
            OutputDebugStringA(warning.c_str());
            return false;
        }
    }

    MMRESULT result = open(&midiInput_, selected,
        reinterpret_cast<DWORD_PTR>(&Driver::MidiInputCallback),
        reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR || !midiInput_) {
        midiInput_ = nullptr;
        OutputDebugStringA("[SVMS] configured MIDI input could not be opened\n");
        return false;
    }

    uint32_t prepared = 0u;
    for (; prepared < kMidiInputBufferCount; ++prepared) {
        MIDIHDR& header = midiInputHeaders_[prepared];
        std::memset(&header, 0, sizeof(header));
        header.lpData = midiInputData_[prepared];
        header.dwBufferLength = kMidiInputBufferBytes;
        if (prepare(midiInput_, &header, sizeof(header)) != MMSYSERR_NOERROR ||
            addBuffer(midiInput_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            break;
        }
    }
    midiInputRunning_.store(true, std::memory_order_release);
    if (prepared != kMidiInputBufferCount ||
        start(midiInput_) != MMSYSERR_NOERROR) {
        StopConfiguredMidiInput();
        OutputDebugStringA("[SVMS] configured MIDI input buffers could not be started\n");
        return false;
    }
    OutputDebugStringA("[SVMS] configured physical MIDI input routing started\n");
    return true;
}

// Stops and closes the physical input and releases its SysEx buffers.
// Safe to call when nothing is open.
void Driver::StopConfiguredMidiInput() noexcept {
    midiInputRunning_.store(false, std::memory_order_release);
    HMIDIIN input = midiInput_;
    if (!input) return;

    using SimpleProc = MMRESULT (WINAPI*)(HMIDIIN);
    using HeaderProc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    SimpleProc stop = reinterpret_cast<SimpleProc>(
        GetSystemWinmmProc("midiInStop"));
    SimpleProc reset = reinterpret_cast<SimpleProc>(
        GetSystemWinmmProc("midiInReset"));
    SimpleProc close = reinterpret_cast<SimpleProc>(
        GetSystemWinmmProc("midiInClose"));
    HeaderProc unprepare = reinterpret_cast<HeaderProc>(
        GetSystemWinmmProc("midiInUnprepareHeader"));
    if (stop) (void)stop(input);
    if (reset) (void)reset(input);
    if (unprepare) {
        for (MIDIHDR& header : midiInputHeaders_) {
            if ((header.dwFlags & MHDR_PREPARED) != 0u)
                (void)unprepare(input, &header, sizeof(header));
        }
    }
    if (close) (void)close(input);
    midiInput_ = nullptr;
    std::memset(midiInputHeaders_, 0, sizeof(midiInputHeaders_));
}

// WinMM input callback.  Short messages are timestamped and submitted as they
// arrive; completed SysEx buffers are submitted and handed back to the device.
void CALLBACK Driver::MidiInputCallback(HMIDIIN input, UINT message,
                                        DWORD_PTR instance,
                                        DWORD_PTR parameter1,
                                        DWORD_PTR) {
    Driver* self = reinterpret_cast<Driver*>(instance);
    if (!self || !self->midiInputRunning_.load(std::memory_order_acquire))
        return;
    if (message == MIM_DATA) {
        uint64_t timestamp = 0u;
        if (!self->tscClock_.Now(timestamp)) {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            timestamp = static_cast<uint64_t>(qpc.QuadPart);
        }
        self->SubmitShortMsgAtQpc(
            static_cast<uint32_t>(parameter1), timestamp);
        return;
    }
    if (message != MIM_LONGDATA) return;

    MIDIHDR* header = reinterpret_cast<MIDIHDR*>(parameter1);
    if (!header) return;
    if (header->dwBytesRecorded != 0u && header->lpData)
        self->SubmitSystemExclusive(
            reinterpret_cast<const uint8_t*>(header->lpData),
            header->dwBytesRecorded);
    header->dwBytesRecorded = 0u;
    if (!self->midiInputRunning_.load(std::memory_order_acquire)) return;
    using HeaderProc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    HeaderProc addBuffer = reinterpret_cast<HeaderProc>(
        GetSystemWinmmProc("midiInAddBuffer"));
    if (addBuffer) (void)addBuffer(input, header, sizeof(*header));
}

// ── 2. Single-event submit ───────────────────────────────────────────────

// Silences everything.  While audio is running this goes through the event
// pipeline as an internal reset message, so it lands at an exact frame in
// order; otherwise the state is reset directly.
void Driver::ResetAllVoices() {
    if (audioOutput && audioOutput->IsRunning()) {
        SubmitShortMsg(kInternalResetMessage);
        return;
    }
    if (voiceManager) voiceManager->Reset();
    if (channelCache) channelCache->Reset();
    sysexMasterVolume_ = 1.0f;
    sysexMasterFineTune_ = 0.0f;
    sysexMasterTranspose_ = 0.0f;
    if (channelCache && configSnapshot)
        channelCache->SetMasterVolume(configSnapshot->masterVolume);
    RefreshSelectedPresets();
    std::fill(std::begin(channelPitchBendRatio_),
              std::end(channelPitchBendRatio_), 1.0f);
    for (uint32_t channel = 0; channel < kChannelCount; ++channel)
        ++channelLaunchRevision_[channel];
    nextPlayIndex_ = 1;
    eventScheduler_.Reset();
    postHighPass.Reset();
    reverb.Reset();
    limiter.Reset();
    for (Driver* shard : shards_) shard->ResetAllVoices();
}

// Blocks a lossless producer until the audio thread frees ingress space
// (the wake epoch changes from `observed`).  Time spent here is counted.
void Driver::WaitForProducerSlot(uint32_t observed) noexcept {
    LARGE_INTEGER begin{}, end{};
    QueryPerformanceCounter(&begin);
    WaitForAddressChange(producerWakeEpoch_, observed);
    QueryPerformanceCounter(&end);
    producerWaits_.fetch_add(1u, std::memory_order_relaxed);
    producerWaitQpc_.fetch_add(
        static_cast<uint64_t>(end.QuadPart - begin.QuadPart),
        std::memory_order_relaxed);
}

// Submits a packed short message (0x00vvddss) timestamped "now".
void Driver::SubmitShortMsg(uint32_t msg) {
    uint64_t timestamp = 0u;
    if (!tscClock_.Now(timestamp)) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        timestamp = static_cast<uint64_t>(qpc.QuadPart);
    }
    SubmitShortMsgAtQpc(msg, timestamp);
}

// Submits a message for an absolute output frame.  The frame rides in the
// timestamp field behind kAbsoluteFrameTimestampTag.
void Driver::SubmitShortMsgAtFrame(uint32_t msg, uint64_t outputFrame) {
    (void)SubmitShortMsgAtFrameCancellable(msg, outputFrame, nullptr, 0u);
}

bool Driver::SubmitShortMsgAtFrameCancellable(
    uint32_t msg, uint64_t outputFrame,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    return SubmitShortMsgAtQpcCancellable(
        msg, kAbsoluteFrameTimestampTag |
                 (outputFrame & kAbsoluteFrameTimestampMask),
        externalCancellation, cancellationToken);
}

// Releases every producer waiting in WaitForProducerSlot.
void Driver::WakeBlockedProducers() {
    producerWakeEpoch_.fetch_add(1u, std::memory_order_release);
    WakeAddressWaiters(producerWakeEpoch_);
}

void Driver::SetIngressMode(EventOverflowMode mode) {
    overflowMode_.store(mode, std::memory_order_release);
}

void Driver::SetNoteOnCollapseThreshold(uint32_t threshold) {
    noteOnCollapse_.SetThreshold(threshold);
}

void Driver::EnableNoteOnCollapse(bool enable) {
    noteOnCollapse_.SetThreshold(enable ? kNoteOnCollapseDefaultThreshold : 1u);
}

// Fills the native-API queue report (backlogs, shed and cancel counts).
void Driver::CopyNativeQueueInfo(SVMS_QueueInfo& out) const {
    out = {};
    out.struct_size = sizeof(out);
    out.struct_version = SVMS_STRUCT_VERSION_1;
    out.ingress_mode = overflowMode_.load(std::memory_order_acquire) ==
            EventOverflowMode::LosslessBackpressure
        ? SVMS_INGRESS_LOSSLESS : SVMS_INGRESS_PRIORITY;
    out.current_velocity_cutoff = currentVelocityCutoffAtomic_.load(
        std::memory_order_relaxed);
    out.queue_capacity = midiIngress_.TotalCapacity();
    out.raw_ingress_count = midiIngress_.TotalSize();
    out.compiled_count = compiledPages_.ReadyEventCount();
    out.scheduled_count = scheduledSizePublished_.load(
        std::memory_order_acquire);
    out.max_events_per_callback = maxEventsPerBlock_;
    out.submitted_events = SubmittedEventCount();
    out.accepted_events = out.submitted_events -
        (std::min)(out.submitted_events,
                   notAcceptedAtomic_.load(std::memory_order_relaxed));
    out.intentionally_shed_events = shedAtomic_.load(
        std::memory_order_relaxed);
    out.cancelled_submissions = cancelledAtomic_.load(
        std::memory_order_relaxed);
}

uint64_t Driver::GetNextOutputFrame() const {
    return outputFramePublished_.load(std::memory_order_acquire);
}

void Driver::SubmitShortMsgAtQpc(uint32_t msg, uint64_t qpcTimestamp) {
    (void)SubmitShortMsgAtQpcCancellable(msg, qpcTimestamp, nullptr, 0u);
}

// Recomputes the velocity shed cutoff from the fullest queue (the lane, the
// raw ring, compiled pages, or the scheduler).  Below `shedStartPercent_` full
// the cutoff is 1 (nothing shed); above it, it rises linearly to 95.
uint32_t Driver::RefreshVelocityCutoff(EventLane lane) noexcept {
    const uint32_t rawIngress = midiIngress_.TotalSize();
    const uint32_t compiledIngress = compiledPages_.ReadyEventCount();
    const uint32_t scheduled =
        scheduledSizePublished_.load(std::memory_order_acquire);
    const uint32_t rawIngressPressure = static_cast<uint32_t>(
        static_cast<uint64_t>(rawIngress) * 100u /
        midiIngress_.TotalCapacity());
    const uint32_t compiledCapacity = pagedScheduler_.Capacity();
    const uint32_t compiledIngressPressure = compiledCapacity != 0u
        ? static_cast<uint32_t>(
              static_cast<uint64_t>(compiledIngress) * 100u /
              compiledCapacity)
        : 100u;
    const uint32_t laneCapacity = midiIngress_.LaneCapacity(lane);
    const uint32_t fastCapacity = midiIngress_.FastCapacity();
    const uint32_t lanePressure = (std::max)(
        static_cast<uint32_t>(
            static_cast<uint64_t>(midiIngress_.LaneSize(lane)) * 100u /
            laneCapacity),
        fastCapacity != 0u
            ? static_cast<uint32_t>(
                  static_cast<uint64_t>(midiIngress_.FastSize()) * 100u /
                  fastCapacity)
            : 0u);
    const uint32_t scheduledCapacity = useEventCompiler_
        ? pagedScheduler_.Capacity() : eventScheduler_.Capacity();
    const uint32_t scheduledPressure = scheduledCapacity != 0u
        ? static_cast<uint32_t>(
              static_cast<uint64_t>(scheduled) * 100u /
              scheduledCapacity)
        : 100u;
    const uint32_t pressure = (std::max)(
        lanePressure,
        (std::max)(rawIngressPressure,
            (std::max)(compiledIngressPressure, scheduledPressure)));
    uint32_t cutoff = 1u;
    if (pressure > shedStartPercent_) {
        const uint32_t range = 100u - shedStartPercent_;
        cutoff = 1u +
            ((std::min)(pressure - shedStartPercent_, range) * 94u) / range;
    }
    currentVelocityCutoffAtomic_.store(cutoff, std::memory_order_relaxed);
    return cutoff;
}

// True if the calling thread owns the single-producer fast ring.  The first
// submitting thread claims it when `events.fast_producer` is on.
bool Driver::IsFastProducerThread() noexcept {
    const uint32_t thread = GetCurrentThreadId();
    const uint32_t owner = fastOwnerThreadId_.load(std::memory_order_relaxed);
    if (owner == thread) {
        if (fastProducerEnabled_.load(std::memory_order_relaxed)) return true;
        // Disabled: this thread now uses the global sequence, so its leased
        // block is stale; drop it so a later re-enable cannot reuse numbers
        // below sequences already handed out.
        fastSequenceNext_ = fastSequenceEnd_;
        return false;
    }
    if (owner == 0u && fastProducerEnabled_.load(std::memory_order_relaxed)) {
        uint32_t expected = 0u;
        if (fastOwnerThreadId_.compare_exchange_strong(
                expected, thread, std::memory_order_acq_rel)) {
            fastSequenceNext_ = fastSequenceEnd_ = 0u;
            return true;
        }
    }
    return false;
}

// Statistics counter bump.  The fast-producer thread is the only writer of
// its core's counters in practice, so it skips the locked RMW (a lost update
// under a rare second submitting thread only skews a census number).
template <typename Counter>
static inline void BumpStat(Counter& counter, bool owner) {
    if (owner) {
        counter.store(counter.load(std::memory_order_relaxed) + 1,
                      std::memory_order_relaxed);
    } else {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
}

// Everything between sequence assignment and the ring push: termination
// fences, same-key coalescing, lane choice and the velocity shed.  Returns
// false when the event is consumed here (coalesced or shed).  Shared by the
// single-event path and the batch paths so they cannot drift apart.
bool Driver::PrepareShortEvent(uint32_t msg, uint64_t qpcTimestamp,
                               uint64_t cadenceCounter,
                               TimestampedMidiEvent& evt, EventLane& lane,
                               bool& lossless, uint8_t& velocity,
                               bool fastOwner) {
    const uint8_t status = static_cast<uint8_t>(msg & 0xffu);
    const uint8_t data1 = static_cast<uint8_t>((msg >> 8) & (((status & 0xe0u) == 0x80u) ? 0xffu : 0x7fu));
    velocity = static_cast<uint8_t>((msg >> 16) & 0x7fu);
    const bool noteOn = !IsInternalEngineMessage(msg) &&
                        (status & 0xf0u) == 0x90u && velocity != 0;

    // Publish the cutoff before waiting on the lossless state lane.  If that
    // lane overtakes a backlog of older note-ons, those notes are rejected at
    // dispatch instead of resurrecting sound after pause/mute.
    if (msg == kInternalResetMessage) {
        PublishTerminationFence(globalTerminationFence_, evt.sequence);
    } else if ((status & 0xf0u) == 0xb0u &&
               (data1 == 120u || data1 == 123u)) {
        PublishTerminationFence(channelTerminationFence_[status & 0x0fu], evt.sequence);
    }

    // ── Same-key note-on coalescing (DEFAULT OFF) ────────────────────────
    // Runs after cancellation/fence bookkeeping and before lane selection:
    // collapsed duplicates never touch the ingress queues, lanes, or the
    // audio thread at all. See NoteOnCollapse.h for the rationale.
    // When disabled (the default) every note-on spawns at its exact QPC
    // timestamp — retrigger/buzz precision is untouched.
    if (noteOn) {
        const uint32_t keyIndex =
            static_cast<uint32_t>(status & 0x0fu) * kNoteCount + data1;
        uint32_t stack = 0u;
        if (!noteOnCollapse_.OnNoteOn(keyIndex, qpcTimestamp, stack,
                                      fastOwner)) {
            BumpStat(coalescedAtomic_, fastOwner);
            if (!noteOnReexcite_.load(std::memory_order_relaxed)) {
                BumpStat(notAcceptedAtomic_, fastOwner);
                return false;
            }
            // Hits inside the previous kept hit's attack ramp change nothing.
            if (!noteOnCollapse_.AllowReExcite(keyIndex, qpcTimestamp)) {
                BumpStat(notAcceptedAtomic_, fastOwner);
                return false;
            }
            // Re-excite: keep the hit at its exact timestamp, tagged so the
            // audio thread restarts the key's live voices instead of
            // launching one. (No stack/velocity boost: nothing is dropped.)
            evt.message |= kReExciteMessageTag;
        }
        // ── Velocity stacking ───────────────────────────────────────────
        // The collapsed hits between two spawns are density, not silence.
        // Feed the accumulated stack into the spawned event's velocity on
        // a log2 curve (2 velocity units per doubling) so hammered keys
        // read as louder, not quieter, matching SnappySynth's stack counter.
        // Boosting here also lifts the event into a higher priority lane
        // and above the velocity shed cutoff.
        if (stack > 1u && velocity < 127u) {
            uint32_t boost = 0u;
            for (uint32_t s = stack; s >>= 1u;) ++boost;
            boost *= 2u;
            const uint32_t boosted =
                velocity + boost > 127u ? 127u : velocity + boost;
            if (boosted != velocity) {
                velocity = static_cast<uint8_t>(boosted);
                evt.message = (evt.message & ~0x007f0000u) |
                              (static_cast<uint32_t>(velocity) << 16);
            }
        }
    } else if (!IsInternalEngineMessage(msg)) {
        const uint8_t statusType = status & 0xf0u;
        // Note-offs intentionally do NOT reset the per-key window/stack:
        // clearing on every note-off would let interleaved on/off floods
        // spawn a voice per event again.  Panic-style state messages
        // still clear, matching their "start over" semantics.
        if (statusType == 0xb0u &&
            (data1 == 120u || data1 == 123u)) {
            noteOnCollapse_.ResetChannel(status & 0x0fu);
        }
    } else if (msg == kInternalResetMessage) {
        noteOnCollapse_.ResetAll();
    }

    lane = EventLane::State;
    if (noteOn) {
        if (velocity >= highPriorityVelocity_) lane = EventLane::Loud;
        else if (velocity >= 64) lane = EventLane::UpperMedium;
        else if (velocity >= 32) lane = EventLane::Medium;
        else lane = EventLane::Quiet;
    }

    // Queue pressure changes much more slowly than MIDI arrives. Sampling it
    // once per 256 global events removes twelve shared atomic loads from the
    // ordinary producer path while still reacting within a fraction of one
    // audio block at extreme rates.
    uint32_t cutoff = currentVelocityCutoffAtomic_.load(
        std::memory_order_relaxed);
    if ((cadenceCounter & 0xffu) == 0u) {
        cutoff = RefreshVelocityCutoff(lane);
    }

    const EventOverflowMode overflowMode =
        overflowMode_.load(std::memory_order_relaxed);
    if (noteOn && velocity < cutoff &&
        overflowMode == EventOverflowMode::PriorityVelocity) {
        BumpStat(shedAtomic_, fastOwner);
        BumpStat(shedByVelocityAtomic_[velocity], fastOwner);
        BumpStat(notAcceptedAtomic_, fastOwner);
        return false;
    }

    lossless = !noteOn || velocity >= highPriorityVelocity_ ||
                          overflowMode == EventOverflowMode::LosslessBackpressure;
    return true;
}

// Core single-event submit: assigns the sequence number, applies
// PrepareShortEvent, then pushes into the ring.  A lossless event waits for
// space; a sheddable one is dropped and counted.  Returns false only when
// cancelled.
bool Driver::SubmitShortMsgAtQpcCancellableCore(
    uint32_t msg, uint64_t qpcTimestamp,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    TimestampedMidiEvent evt{};
    evt.message = msg;
    const bool fastOwner = IsFastProducerThread();
    uint64_t cadenceCounter;
    if (fastOwner) {
        // Owner thread: sequences come from a leased block, the submitted
        // count is a plain store, and the push below is one ring store. The
        // sequence stays strictly increasing within this thread's stream.
        if (fastSequenceNext_ == fastSequenceEnd_) {
            fastSequenceNext_ = nextEventSequence_.fetch_add(
                kFastSequenceLease, std::memory_order_relaxed);
            fastSequenceEnd_ = fastSequenceNext_ + kFastSequenceLease;
            fastLeased_.store(fastLeased_.load(std::memory_order_relaxed) +
                                  kFastSequenceLease,
                              std::memory_order_relaxed);
        }
        evt.sequence = static_cast<uint32_t>(fastSequenceNext_++);
        cadenceCounter = fastSubmitted_.load(std::memory_order_relaxed) + 1u;
        fastSubmitted_.store(cadenceCounter, std::memory_order_relaxed);
    } else {
        evt.sequence = static_cast<uint32_t>(
            nextEventSequence_.fetch_add(1, std::memory_order_relaxed));
        cadenceCounter = evt.sequence;
    }
    evt.qpcTimestamp = qpcTimestamp;

    if (externalCancellation && externalCancellation->load(
            std::memory_order_acquire) == cancellationToken) {
        cancelledAtomic_.fetch_add(1, std::memory_order_relaxed);
        notAcceptedAtomic_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    EventLane lane;
    bool lossless;
    uint8_t velocity;
    if (!PrepareShortEvent(msg, qpcTimestamp, cadenceCounter, evt, lane,
                           lossless, velocity, fastOwner))
        return true;
    for (;;) {
        if (fastOwner ? midiIngress_.TryPushFast(evt)
                      : midiIngress_.TryPush(lane, evt)) {
            // A running compiler will observe the queue without help. Only
            // cross the kernel/API boundary when it explicitly published
            // that it is asleep. The load keeps the ordinary (awake) case
            // a plain read; the exchange only runs when a wake is real.
            // No lost wakeup is possible: the compiler re-pops the queue
            // after publishing sleeping=true, and this event's push
            // already completed before the load below observed anything.
            if (compilerSleeping_.load(std::memory_order_acquire) &&
                compilerSleeping_.exchange(false,
                    std::memory_order_acq_rel)) {
                compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
                WakeAddressWaiters(compilerWakeEpoch_);
            }
#if defined(SVMS_XP_COMPAT)
            static LONG acceptedTraceCount = 0;
            const LONG acceptedIndex = InterlockedIncrement(&acceptedTraceCount);
            if (acceptedIndex <= 32) {
                char message[224] = {};
                std::snprintf(message, sizeof(message),
                              "[SVMS XP] ingress accepted #%ld seq=%lu lane=%u queued=%lu\r\n",
                              static_cast<long>(acceptedIndex),
                              static_cast<unsigned long>(evt.sequence),
                              static_cast<unsigned>(lane),
                              static_cast<unsigned long>(midiIngress_.TotalSize()));
                OutputDebugStringA(message);
            }
#endif
            return true;
        }
        if (!lossless) {
            shedAtomic_.fetch_add(1, std::memory_order_relaxed);
            shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
            notAcceptedAtomic_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (cancelProducers_.load(std::memory_order_acquire) ||
            (externalCancellation && externalCancellation->load(
                std::memory_order_acquire) == cancellationToken)) {
            cancelledAtomic_.fetch_add(1, std::memory_order_relaxed);
            notAcceptedAtomic_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        uint32_t observed = producerWakeEpoch_.load(std::memory_order_acquire);
        WaitForProducerSlot(observed);
    }
}

// ── 3. Batch submit ──────────────────────────────────────────────────────

// Public entry: on a sharded master the batch is split per core (notes to
// their shard, everything else to every core, each core's subsequence in the
// original order); otherwise it goes straight to the core path.
bool Driver::SubmitShortBatchAtQpcCancellable(
    const SVMS_ShortEvent* events, uint32_t eventCount,
    uint64_t immediateQpc,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (!events && eventCount != 0u) return false;
    if (shardCount_ > 1u && !shardMaster_)
        return SubmitShardedBatch(events, eventCount, immediateQpc,
                                  externalCancellation, cancellationToken);
    return SubmitShortBatchCore(events, eventCount, immediateQpc,
                                externalCancellation, cancellationToken);
}

// Splits a batch across key-sharded cores in chunks: notes go to the core that
// owns their (channel, key), everything else goes to every core.  Each core
// sees its own subsequence in the original order.
bool Driver::SubmitShardedBatch(
    const SVMS_ShortEvent* events, uint32_t eventCount,
    uint64_t immediateQpc,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    constexpr uint32_t kChunk = 4096u;
    constexpr uint32_t kMaxCores = 32u;
    static thread_local std::vector<SVMS_ShortEvent> perCore[kMaxCores];
    const uint32_t cores = (std::min)(shardCount_, kMaxCores);
    for (uint32_t base = 0u; base < eventCount; base += kChunk) {
        const uint32_t count = (std::min)(kChunk, eventCount - base);
        for (uint32_t core = 0u; core < cores; ++core) perCore[core].clear();
        for (uint32_t i = 0u; i < count; ++i) {
            const SVMS_ShortEvent& event = events[base + i];
            const uint32_t msg = event.packed_message;
            const uint8_t status = static_cast<uint8_t>(msg & 0xffu);
            const uint8_t type = status & 0xf0u;
            if (!IsInternalEngineMessage(msg) &&
                (type == 0x80u || type == 0x90u)) {
                perCore[ShardFor(status & 0x0fu,
                                 static_cast<uint8_t>((msg >> 8u) & 0x7fu))]
                    .push_back(event);
            } else {
                for (uint32_t core = 0u; core < cores; ++core)
                    perCore[core].push_back(event);
            }
        }
        for (uint32_t core = 0u; core < cores; ++core) {
            if (perCore[core].empty()) continue;
            Driver* target = core == 0u ? this : shards_[core - 1u];
            if (!target->SubmitShortBatchCore(
                    perCore[core].data(),
                    static_cast<uint32_t>(perCore[core].size()),
                    immediateQpc, externalCancellation, cancellationToken))
                return false;
        }
    }
    return true;
}

// Fast-owner batch of arbitrary events (note-offs, controllers, mixed
// velocities).  Each event still goes through PrepareShortEvent, so fences,
// coalescing and shedding are exactly the single-event behaviour; only the
// ring push is amortised.  A chunk the ring cannot take whole falls back to
// the single-event push/shed/wait rules.
bool Driver::SubmitMixedBatchFast(
    const SVMS_ShortEvent* events, uint32_t eventCount,
    uint64_t immediateQpc,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    constexpr uint32_t kChunk = 256u;
    TimestampedMidiEvent prepared[kChunk];
    uint8_t lossless[kChunk];
    uint8_t velocities[kChunk];
    uint32_t cursor = 0u;
    while (cursor < eventCount) {
        if (cancelProducers_.load(std::memory_order_acquire) ||
            (externalCancellation && externalCancellation->load(
                std::memory_order_acquire) == cancellationToken)) {
            cancelledAtomic_.fetch_add(1u, std::memory_order_relaxed);
            notAcceptedAtomic_.fetch_add(eventCount - cursor,
                                         std::memory_order_relaxed);
            return false;
        }
        uint32_t prepCount = 0u;
        for (; cursor < eventCount && prepCount < kChunk; ++cursor) {
            const SVMS_ShortEvent& source = events[cursor];
            TimestampedMidiEvent evt{};
            evt.message = source.packed_message;
            if (fastSequenceNext_ == fastSequenceEnd_) {
                fastSequenceNext_ = nextEventSequence_.fetch_add(
                    kFastSequenceLease, std::memory_order_relaxed);
                fastSequenceEnd_ = fastSequenceNext_ + kFastSequenceLease;
                fastLeased_.store(fastLeased_.load(std::memory_order_relaxed) +
                                      kFastSequenceLease,
                                  std::memory_order_relaxed);
            }
            evt.sequence = static_cast<uint32_t>(fastSequenceNext_++);
            const uint64_t cadenceCounter =
                fastSubmitted_.load(std::memory_order_relaxed) + 1u;
            fastSubmitted_.store(cadenceCounter, std::memory_order_relaxed);
            evt.qpcTimestamp = source.timestamp_qpc != 0u
                ? source.timestamp_qpc : immediateQpc;
            EventLane lane;
            bool isLossless;
            uint8_t velocity;
            if (!PrepareShortEvent(source.packed_message, evt.qpcTimestamp,
                                   cadenceCounter, evt, lane, isLossless,
                                   velocity, true))
                continue;
            prepared[prepCount] = evt;
            lossless[prepCount] = isLossless ? 1u : 0u;
            velocities[prepCount] = velocity;
            ++prepCount;
        }
        if (prepCount == 0u) continue;
        if (!midiIngress_.TryPushFastBatch(prepared, prepCount)) {
            for (uint32_t i = 0u; i < prepCount; ++i) {
                for (;;) {
                    if (midiIngress_.TryPushFast(prepared[i])) break;
                    if (lossless[i] == 0u) {
                        shedAtomic_.fetch_add(1, std::memory_order_relaxed);
                        shedByVelocityAtomic_[velocities[i]].fetch_add(
                            1, std::memory_order_relaxed);
                        notAcceptedAtomic_.fetch_add(
                            1, std::memory_order_relaxed);
                        break;
                    }
                    if (cancelProducers_.load(std::memory_order_acquire) ||
                        (externalCancellation && externalCancellation->load(
                            std::memory_order_acquire) == cancellationToken)) {
                        cancelledAtomic_.fetch_add(
                            1, std::memory_order_relaxed);
                        notAcceptedAtomic_.fetch_add(
                            1, std::memory_order_relaxed);
                        return false;
                    }
                    const uint32_t observed = producerWakeEpoch_.load(
                        std::memory_order_acquire);
                    WaitForProducerSlot(observed);
                }
            }
        }
        if (compilerSleeping_.load(std::memory_order_acquire) &&
            compilerSleeping_.exchange(false, std::memory_order_acq_rel)) {
            compilerWakeEpoch_.fetch_add(1u, std::memory_order_release);
            WakeAddressWaiters(compilerWakeEpoch_);
        }
    }
    return true;
}

// Batch submit for one core.  A batch of only loud note-ons reserves ring
// cells and sequence numbers in blocks of 256 (one CAS per block); anything
// else takes the mixed fast path (fast owner) or the per-event path.
bool Driver::SubmitShortBatchCore(
    const SVMS_ShortEvent* events, uint32_t eventCount,
    uint64_t immediateQpc,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (!events && eventCount != 0u) return false;

    // Full-velocity Black MIDI batches overwhelmingly occupy the loud lane.
    // Keep mixed/state/priority-shedding batches on the established per-event
    // path; homogeneous protected note-ons can reserve 256 lane cells with
    // one producer CAS while retaining one global sequence per event.
    const bool fastOwner = IsFastProducerThread();
    bool homogeneousLoudNotes = eventCount != 0u;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        const uint32_t message = events[i].packed_message;
        const uint8_t status = static_cast<uint8_t>(message & 0xffu);
        const uint8_t velocity = static_cast<uint8_t>(
            (message >> 16u) & 0x7fu);
        if ((status & 0xf0u) != 0x90u || velocity == 0u ||
            velocity < highPriorityVelocity_) {
            homogeneousLoudNotes = false;
            break;
        }
    }
    if (!homogeneousLoudNotes) {
        if (fastOwner)
            return SubmitMixedBatchFast(events, eventCount, immediateQpc,
                                        externalCancellation,
                                        cancellationToken);
        for (uint32_t i = 0u; i < eventCount; ++i) {
            const uint64_t timestamp = events[i].timestamp_qpc != 0u
                ? events[i].timestamp_qpc : immediateQpc;
            if (!SubmitShortMsgAtQpcCancellableCore(
                    events[i].packed_message, timestamp,
                    externalCancellation, cancellationToken)) {
                return false;
            }
        }
        return true;
    }

    constexpr uint32_t kBatchReservation = 256u;
    TimestampedMidiEvent prepared[kBatchReservation]{};
    uint32_t cursor = 0u;
    while (cursor < eventCount) {
        if (cancelProducers_.load(std::memory_order_acquire) ||
            (externalCancellation && externalCancellation->load(
                std::memory_order_acquire) == cancellationToken)) {
            cancelledAtomic_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        const uint32_t laneCapacity = fastOwner
            ? midiIngress_.FastCapacity()
            : midiIngress_.LaneCapacity(EventLane::Loud);
        const uint32_t count = (std::min)(
            eventCount - cursor,
            (std::min)(kBatchReservation, laneCapacity));
        uint32_t firstSequence;
        if (fastOwner) {
            // Same leased sequence space as the per-event owner path.
            if (fastSequenceEnd_ - fastSequenceNext_ < count) {
                fastSequenceNext_ = nextEventSequence_.fetch_add(
                    kFastSequenceLease, std::memory_order_relaxed);
                fastSequenceEnd_ = fastSequenceNext_ + kFastSequenceLease;
                fastLeased_.store(fastLeased_.load(std::memory_order_relaxed) +
                                      kFastSequenceLease,
                                  std::memory_order_relaxed);
            }
            firstSequence = static_cast<uint32_t>(fastSequenceNext_);
            fastSequenceNext_ += count;
            fastSubmitted_.store(
                fastSubmitted_.load(std::memory_order_relaxed) + count,
                std::memory_order_relaxed);
        } else {
            firstSequence = static_cast<uint32_t>(
                nextEventSequence_.fetch_add(count, std::memory_order_relaxed));
        }
        for (uint32_t i = 0u; i < count; ++i) {
            const SVMS_ShortEvent& source = events[cursor + i];
            prepared[i].message = source.packed_message;
            prepared[i].sequence = firstSequence + i;
            prepared[i].qpcTimestamp = source.timestamp_qpc != 0u
                ? source.timestamp_qpc : immediateQpc;
        }
        // Maintain the same pressure-sampling cadence as individual pushes.
        RefreshVelocityCutoff(EventLane::Loud);
        for (;;) {
            if (fastOwner ? midiIngress_.TryPushFastBatch(prepared, count)
                          : midiIngress_.TryPushBatch(
                                EventLane::Loud, prepared, count)) {
                // Same guarded wake as the single-event path: the batch is
                // already visible in the lane, so only a compiler that
                // published sleeping=true after this push needs a wake.
                if (compilerSleeping_.load(std::memory_order_acquire) &&
                    compilerSleeping_.exchange(
                        false, std::memory_order_acq_rel)) {
                    compilerWakeEpoch_.fetch_add(1u,
                                                  std::memory_order_release);
                    WakeAddressWaiters(compilerWakeEpoch_);
                }
                cursor += count;
                break;
            }
            if (cancelProducers_.load(std::memory_order_acquire) ||
                (externalCancellation && externalCancellation->load(
                    std::memory_order_acquire) == cancellationToken)) {
                cancelledAtomic_.fetch_add(1u, std::memory_order_relaxed);
                notAcceptedAtomic_.fetch_add(count, std::memory_order_relaxed);
                return false;
            }
            const uint32_t observed = producerWakeEpoch_.load(
                std::memory_order_acquire);
            WaitForProducerSlot(observed);
        }
    }
    return true;
}

// Submits a raw MIDI byte stream: parses it into short messages (in chunks of
// 256) and SysEx messages, keeping them in stream order.  The caller's buffer
// is copied once through ReadProcessMemory so a bad pointer fails cleanly.
bool Driver::SubmitMidiStreamCancellable(
    const uint8_t* data, uint32_t size,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (size == 0u) return true;
    if (!data) return false;
    // One guarded copy: the caller's buffer may be unmapped under us.
    std::vector<uint8_t> owned;
    try { owned.resize(size); } catch (...) { return false; }
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), data, owned.data(), size,
                           &copied) || copied != size) return false;

    LARGE_INTEGER timestamp{};
    QueryPerformanceCounter(&timestamp);
    const uint64_t qpc = static_cast<uint64_t>(timestamp.QuadPart);
    constexpr uint32_t kChunk = 256u;
    SVMS_ShortEvent batch[kChunk];
    uint32_t batched = 0u;
    bool accepted = true;
    auto flush = [&]() {
        if (batched != 0u && accepted)
            accepted = SubmitShortBatchAtQpcCancellable(
                batch, batched, qpc, externalCancellation, cancellationToken);
        batched = 0u;
    };

    std::lock_guard<std::mutex> lock(midiStreamMutex_);
    midiStream_.Feed(owned.data(), size,
        [&](uint32_t message) {
            batch[batched++] = SVMS_ShortEvent{0u, message, 0u};
            if (batched == kChunk) flush();
        },
        [&](const uint8_t* sysex, uint32_t sysexSize) {
            flush();   // keep shorts ahead of the SysEx in stream order
            if (accepted)
                accepted = SubmitSystemExclusiveCancellable(
                    sysex, sysexSize, externalCancellation, cancellationToken);
        });
    flush();
    return accepted;
}

// ── 4. SysEx ─────────────────────────────────────────────────────────────

void Driver::SubmitSystemExclusive(const uint8_t* data, uint32_t size) {
    (void)SubmitSystemExclusiveCancellable(data, size, nullptr, 0u);
}

// Translates a SysEx message into ordinary timestamped engine events:
// GM1/GM2 mode messages and GS reset become a reset, master volume and
// tuning become internal messages, and GS (and a subset of XG) part
// parameters become CC and program changes.  Nothing is applied on this thread, so the effects land at
// an exact frame in ingress order.  Unknown messages are ignored.
bool Driver::SubmitSystemExclusiveCancellable(
    const uint8_t* data, uint32_t size,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (!data || size < 2u) return false;
    uint8_t endpoint = 0;
    SIZE_T checked = 0;
    const uintptr_t address = reinterpret_cast<uintptr_t>(data);
    if (address > UINTPTR_MAX - (size - 1u) ||
        !ReadProcessMemory(GetCurrentProcess(), data, &endpoint, 1u, &checked) ||
        !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + size - 1u), &endpoint, 1u, &checked)) return false;
    std::vector<uint8_t> owned;
    try { owned.resize(size); } catch (...) { return false; }
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), data, owned.data(), size, &copied) ||
        copied != size) return false;
    data = owned.data();
    if (data[0] != 0xf0u || data[size - 1u] != 0xf7u) return true;

    LARGE_INTEGER timestamp{};
    QueryPerformanceCounter(&timestamp);
    const uint64_t qpc = static_cast<uint64_t>(timestamp.QuadPart);
    bool accepted = true;
    auto emit = [&](uint32_t message) {
        if (accepted) {
            accepted = SubmitShortMsgAtQpcCancellable(
                message, qpc, externalCancellation, cancellationToken);
        }
    };
    auto emitCC = [&](uint8_t channel, uint8_t controller, uint8_t value) {
        emit(static_cast<uint32_t>(0xb0u | (channel & 0x0fu)) |
             (static_cast<uint32_t>(controller & 0x7fu) << 8u) |
             (static_cast<uint32_t>(value & 0x7fu) << 16u));
    };
    auto emitProgram = [&](uint8_t channel, uint8_t program) {
        emit(static_cast<uint32_t>(0xc0u | (channel & 0x0fu)) |
             (static_cast<uint32_t>(program & 0x7fu) << 8u));
    };

    // Universal non-realtime mode messages: GM1 on/off and GM2 on.  MSGS
    // returns to its basic GM/GS state for these, just as it does for GS
    // Reset. Device ID is intentionally accepted as either broadcast or a
    // concrete 7-bit device number.
    if (size >= 6u && data[1] == 0x7eu && data[3] == 0x09u &&
        (data[4] == 0x01u || data[4] == 0x02u || data[4] == 0x03u)) {
        emit(kInternalResetMessage);
        return accepted;
    }

    // Universal realtime Master Volume (14-bit, LSB then MSB).
    if (size >= 8u && data[1] == 0x7fu && data[3] == 0x04u &&
        data[4] == 0x01u) {
        const uint16_t value = static_cast<uint16_t>(
            (data[5] & 0x7fu) | ((data[6] & 0x7fu) << 7u));
        emit(MakeInternalMasterVolumeMessage(value));
        return accepted;
    }
    // Universal realtime Master Fine/Coarse Tuning. Fine tuning shares the
    // MIDI Tuning Standard's centered 14-bit representation; coarse tuning
    // shares XG's centered 7-bit semitone representation.
    if (size >= 8u && data[1] == 0x7fu && data[3] == 0x04u &&
        data[4] == 0x03u) {
        const uint16_t value = static_cast<uint16_t>(
            (data[5] & 0x7fu) | ((data[6] & 0x7fu) << 7u));
        emit(MakeInternalMasterFineTuneMessage(value));
        return accepted;
    }
    if (size >= 7u && data[1] == 0x7fu && data[3] == 0x04u &&
        data[4] == 0x04u) {
        emit(MakeInternalMasterTransposeMessage(data[5] & 0x7fu));
        return accepted;
    }

    // Roland GS Data Set 1 (DT1).  Verify the Roland checksum, then map the
    // MSGS-relevant system and part parameters onto exact-frame engine/MIDI
    // events. Bulk packets work too because the 7-bit GS address advances
    // for every data byte.
    if (size >= 11u && data[1] == 0x41u && data[3] == 0x42u &&
        data[4] == 0x12u) {
        uint32_t checksumSum = 0u;
        for (uint32_t i = 5u; i + 1u < size; ++i)
            checksumSum += data[i] & 0x7fu;
        if ((checksumSum & 0x7fu) != 0u) return true;

        uint8_t address0 = data[5] & 0x7fu;
        uint8_t address1 = data[6] & 0x7fu;
        uint8_t address2 = data[7] & 0x7fu;
        const uint32_t dataEnd = size - 2u; // checksum, F7
        for (uint32_t i = 8u; i < dataEnd; ++i) {
            const uint8_t value = data[i] & 0x7fu;
            if (address0 == 0x40u && address1 == 0x00u) {
                if (address2 == 0x7fu && value == 0u) {
                    emit(kInternalResetMessage);
                } else if (address2 == 0x04u) {
                    emit(MakeInternalMasterVolumeMessage(
                        static_cast<uint16_t>(value) * 129u));
                }
            } else if (address0 == 0x40u &&
                       (address1 & 0x70u) == 0x10u) {
                const uint8_t part = address1 & 0x0fu;
                const uint8_t channel = part == 0u ? 9u
                    : (part <= 9u ? static_cast<uint8_t>(part - 1u) : part);
                switch (address2) {
                    case 0x00u: emitCC(channel, 0u, value); break;
                    case 0x01u: emitProgram(channel, value); break;
                    case 0x15u:
                        emit(MakeInternalRhythmPartMessage(channel, value));
                        break;
                    case 0x19u: emitCC(channel, 7u, value); break;
                    case 0x1cu: emitCC(channel, 10u,
                                      value == 0u ? 64u : value); break;
                    default: break;
                }
            }

            if (++address2 == 0x80u) {
                address2 = 0u;
                if (++address1 == 0x80u) {
                    address1 = 0u;
                    address0 = static_cast<uint8_t>((address0 + 1u) & 0x7fu);
                }
            }
        }
        return accepted;
    }

    // Translate the useful XG system and multi-part subset into the same
    // timestamped command stream as ordinary MIDI. No parameter is applied
    // on this producer thread, and bulk parameter packets preserve byte order.
    (void)TranslateXGSystemExclusive(data, size, emit);
    return accepted;
}

// ── 5. Direct page feed ──────────────────────────────────────────────────

namespace {
// Spin lock over an atomic_flag, for the short critical sections around the
// compiled-page pool and the direct producer slot.
struct PoolGuard {
    std::atomic_flag& flag;
    explicit PoolGuard(std::atomic_flag& f) noexcept : flag(f) {
        while (flag.test_and_set(std::memory_order_acquire)) YieldProcessor();
    }
    ~PoolGuard() { flag.clear(std::memory_order_release); }
};
} // namespace

bool Driver::AcquireCompilerPage(uint32_t& pageIndex) noexcept {
    PoolGuard guard(compiledPoolLock_);
    return compiledPages_.AcquireForCompiler(pageIndex);
}

bool Driver::PublishCompilerPage(uint32_t pageIndex, uint32_t count) noexcept {
    PoolGuard guard(compiledPoolLock_);
    return compiledPages_.PublishFromCompiler(pageIndex, count);
}

void Driver::ReturnCompilerPage(uint32_t pageIndex) noexcept {
    PoolGuard guard(compiledPoolLock_);
    compiledPages_.ReturnUnusedFromCompiler(pageIndex);
}

// Direct page feed. A batch that is already in time order gains nothing from
// the ring, per-event sequencing and gating, or the compiler thread: compile it
// here and append it to an open page. Players send small batches every
// millisecond, so a page per batch would starve the pool; instead one page stays
// open (directState_), is published when full, and the audio callback takes
// whatever is in it right before it imports (FlushDirectPage), so no event waits
// longer than it would behind the compiler thread.
//
// The handoff is lock-free so the audio thread never waits on the producer
// (which runs at a lower priority and can be preempted): the producer writes
// events past the published count, then CASes the count up; the audio thread
// detaches the page with a CAS to 0. If that CAS beats the producer's, the
// producer's write was not published and it simply appends to a fresh page.
// CC / pitch-bend collapse works within a batch; priority-velocity shedding as
// on the ring path. Minimum note length, note-on coalescing and a sharded master
// make this decline, nothing consumed.
Driver::DirectSubmit Driver::SubmitShortBatchDirect(
    const SVMS_ShortEvent* events, uint32_t eventCount, uint64_t immediateQpc,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (!events && eventCount != 0u) return DirectSubmit::Unsupported;
    const uint64_t epoch = compilerEpochQPC_.load(std::memory_order_acquire);
    if (!useEventCompiler_ || epoch == 0u || shardCount_ > 1u ||
        minNoteMs_.load(std::memory_order_relaxed) != 0u ||
        noteOnCollapse_.Enabled()) {
        return DirectSubmit::Unsupported;
    }
    if (eventCount == 0u) return DirectSubmit::Done;

    // One direct producer at a time (the stage buffer is theirs alone).
    PoolGuard producer(directProducerLock_);
    if (!directReady_.load(std::memory_order_relaxed)) {
        directStage_.resize(kCompiledEventPageCapacity);
        directScratch_.resize(kCompiledEventPageCapacity);
        audioScratch_.resize(kCompiledEventPageCapacity);
        directMerger_.reset(new NoteMerger());
        audioMerger_.reset(new NoteMerger());
        directReady_.store(true, std::memory_order_release);
    }

    const bool collapseCc = ccCollapseEnabled_.load(std::memory_order_relaxed);
    const bool collapseBend = bendCollapseEnabled_.load(std::memory_order_relaxed);
    const DirectCompileContext ctx{epoch, static_cast<int64_t>(qpcFreq),
                                   sampleRate, bufferFrames};
    const uint32_t seqBase = static_cast<uint32_t>(
        nextEventSequence_.fetch_add(eventCount, std::memory_order_relaxed));
    auto terminate = [this](bool global, uint8_t channel, uint32_t sequence) {
        PublishTerminationFence(
            global ? globalTerminationFence_ : channelTerminationFence_[channel],
            sequence);
    };
    auto shed = [this](uint8_t velocity) {
        shedAtomic_.fetch_add(1, std::memory_order_relaxed);
        shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
        notAcceptedAtomic_.fetch_add(1, std::memory_order_relaxed);
    };
    auto cancelled = [&]() {
        return cancelProducers_.load(std::memory_order_acquire) ||
               (externalCancellation && externalCancellation->load(
                    std::memory_order_acquire) == cancellationToken);
    };
    // Queue pressure is sampled once per batch (the ring path: per 256 events).
    const uint32_t shedBelow =
        overflowMode_.load(std::memory_order_relaxed) ==
                EventOverflowMode::PriorityVelocity
            ? RefreshVelocityCutoff(EventLane::Loud) : 0u;

    for (uint32_t cursor = 0u; cursor < eventCount;) {
        const uint32_t take = (std::min)(eventCount - cursor,
                                         kCompiledEventPageCapacity);
        uint32_t staged = CompileDirectRun(
            events + cursor, take, immediateQpc, seqBase + cursor, ctx,
            directStage_.data(), terminate, shedBelow, shed);
        cursor += take;
        if (collapseCc || collapseBend)
            staged = CollapseDirectStage(staged, collapseCc, collapseBend);

        for (uint32_t done = 0u; done < staged;) {
            uint64_t state = directState_.load(std::memory_order_acquire);
            if (state == 0u) {
                uint32_t got = kInvalidEventPage;
                if (!AcquireCompilerPage(got)) {
                    if (cancelled()) {
                        cancelledAtomic_.fetch_add(1u, std::memory_order_relaxed);
                        notAcceptedAtomic_.fetch_add(
                            (eventCount - cursor) + (staged - done),
                            std::memory_order_relaxed);
                        return DirectSubmit::Cancelled;
                    }
                    // The audio thread bumps this epoch when it recycles pages.
                    const uint32_t observed =
                        compilerWakeEpoch_.load(std::memory_order_acquire);
                    if (!AcquireCompilerPage(got)) {
                        WaitForAddressChange(compilerWakeEpoch_, observed);
                        continue;
                    }
                }
                state = static_cast<uint64_t>(got + 1u) << 32;
                directState_.store(state, std::memory_order_release);
            }
            const uint32_t pageIndex = static_cast<uint32_t>(state >> 32) - 1u;
            const uint32_t count = static_cast<uint32_t>(state);
            const uint32_t n = (std::min)(staged - done,
                                          kCompiledEventPageCapacity - count);
            std::memcpy(compiledPages_.Page(pageIndex).events + count,
                        directStage_.data() + done,
                        static_cast<size_t>(n) * sizeof(ScheduledRenderEvent));
            const uint64_t next = (state & ~uint64_t{0xffffffffu}) | (count + n);
            uint64_t expected = state;
            if (!directState_.compare_exchange_strong(
                    expected, next, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                continue; // the audio thread took the page first: redo on a new one
            }
            done += n;
            if (count + n == kCompiledEventPageCapacity) {
                expected = next;
                if (directState_.compare_exchange_strong(
                        expected, 0u, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    PublishDirectPage(pageIndex, count + n);
                }
            }
        }
    }
    return DirectSubmit::Done;
}

// The compiler thread's CC / bend collapse on a staged batch: a collapsible
// controller (or a pitch bend) replaces the still-pending one of the same
// channel. Compacts in place and returns the new count.
uint32_t Driver::CollapseDirectStage(uint32_t count, bool collapseCc,
                                     bool collapseBend) noexcept {
    if (++directStageGen_ == 0u) {
        std::memset(directCc_, 0, sizeof(directCc_));
        std::memset(directBend_, 0, sizeof(directBend_));
        directStageGen_ = 1u;
    }
    ScheduledRenderEvent* events = directStage_.data();
    uint32_t w = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        const ScheduledRenderEvent e = events[i];
        if (collapseCc) {
            if (e.type == RenderEventType::ControlChange) {
                if (IsCollapsibleController(e.data1)) {
                    DirectCollapseRecord& r = directCc_[e.channel][e.data1];
                    if (r.gen == directStageGen_ && r.offset < w) {
                        events[r.offset] = e;
                        ++ccCollapsedCount_;
                        continue;
                    }
                    r = {directStageGen_, w};
                } else if (IsControllerStateReset(e.data1)) {
                    std::memset(directCc_[e.channel], 0, sizeof(directCc_[e.channel]));
                }
            } else if (e.type == RenderEventType::Reset) {
                std::memset(directCc_, 0, sizeof(directCc_));
            }
        }
        if (collapseBend) {
            if (e.type == RenderEventType::PitchBend && e.channel < kChannelCount) {
                DirectCollapseRecord& r = directBend_[e.channel];
                if (r.gen == directStageGen_ && r.offset < w) {
                    events[r.offset] = e;
                    ++ccCollapsedCount_;
                    continue;
                }
                r = {directStageGen_, w};
            } else if (e.type == RenderEventType::Reset) {
                std::memset(directBend_, 0, sizeof(directBend_));
            }
        }
        events[w++] = e;
    }
    return w;
}

// Producer: sort, merge and publish a full page through the ready queue.
void Driver::PublishDirectPage(uint32_t pageIndex, uint32_t count) noexcept {
    CompiledEventPage& page = compiledPages_.Page(pageIndex);
    SortCompiledEventPage(page.events, directScratch_.data(), count);
    if (noteMergeEnabled_.load(std::memory_order_relaxed)) {
        uint64_t folded = 0u;
        count = directMerger_->Merge(
            page.events, count,
            noteMergeHoist_.load(std::memory_order_relaxed), folded);
        if (folded != 0u)
            noteMergedAtomic_.fetch_add(folded, std::memory_order_relaxed);
    }
    // The ready queue holds as many pages as the pool owns, so this cannot
    // fail; if it somehow does, the page goes back rather than blocking.
    if (!PublishCompilerPage(pageIndex, count)) ReturnCompilerPage(pageIndex);
}

// Audio thread, once per callback right before the scheduler imports pages:
// take the open page if it holds events, order it, and hand it straight to the
// scheduler. Never waits for the producer. Returns pages imported (0 or 1).
uint32_t Driver::FlushDirectPage() noexcept {
    if (!directReady_.load(std::memory_order_acquire)) return 0u;
    uint64_t state = directState_.load(std::memory_order_acquire);
    for (;;) {
        if (static_cast<uint32_t>(state) == 0u) return 0u; // none open, or empty
        if (directState_.compare_exchange_weak(state, 0u,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
    const uint32_t pageIndex = static_cast<uint32_t>(state >> 32) - 1u;
    uint32_t count = static_cast<uint32_t>(state);
    CompiledEventPage& page = compiledPages_.Page(pageIndex);
    SortCompiledEventPage(page.events, audioScratch_.data(), count);
    if (noteMergeEnabled_.load(std::memory_order_relaxed)) {
        uint64_t folded = 0u;
        count = audioMerger_->Merge(
            page.events, count,
            noteMergeHoist_.load(std::memory_order_relaxed), folded);
        if (folded != 0u)
            noteMergedAtomic_.fetch_add(folded, std::memory_order_relaxed);
    }
    return pagedScheduler_.ImportPage(pageIndex, count) ? 1u : 0u;
}

// ── 6. Compiler thread ───────────────────────────────────────────────────

// The compiler thread: pops ingress events in sequence order, compiles each
// to a ScheduledRenderEvent (exact target frame), applies the opt-in
// minimum-note-length, CC-collapse and bend-collapse rules, sorts the page,
// optionally merges identical note-ons, and publishes it to the audio thread.
// Sleeps (with a wake epoch) when the ring is empty or no clock epoch exists.
void Driver::EventCompilerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // Small enough to stay cache-friendly, large enough that one 2048-frame
    // Black-MIDI callback arrives as only a handful of ordered runs.
    static constexpr uint32_t kCompilerChunkCapacity = 8192u;
    PriorityEventIngress<TimestampedMidiEvent>::OrderedMergeState mergeState{};

    auto publishProducerSpace = [this]() {
        producerWakeEpoch_.fetch_add(1, std::memory_order_release);
        WakeAddressWaiters(producerWakeEpoch_);
    };

    while (!cancelProducers_.load(std::memory_order_acquire)) {
        const uint64_t epoch = compilerEpochQPC_.load(std::memory_order_acquire);
        if (epoch == 0u) {
            compilerSleeping_.store(true, std::memory_order_release);
            const uint32_t observed = compilerWakeEpoch_.load(std::memory_order_acquire);
            if (compilerEpochQPC_.load(std::memory_order_acquire) == 0u)
                WaitForAddressChange(compilerWakeEpoch_, observed);
            compilerSleeping_.store(false, std::memory_order_release);
            continue;
        }

        TimestampedMidiEvent timed{};
        if (!midiIngress_.TryPopSequenceOrdered(timed, mergeState)) {
            compilerSleeping_.store(true, std::memory_order_release);
            const uint32_t observed = compilerWakeEpoch_.load(std::memory_order_acquire);
            // Close the store-to-sleep race: a producer that arrived before
            // the epoch load either wakes us or is observed by this retry.
            if (!midiIngress_.TryPopSequenceOrdered(timed, mergeState)) {
                WaitForAddressChange(compilerWakeEpoch_, observed);
                compilerSleeping_.store(false, std::memory_order_release);
                continue;
            }
            compilerSleeping_.store(false, std::memory_order_release);
        }

        uint32_t pageIndex = kInvalidEventPage;
        while (!AcquireCompilerPage(pageIndex)) {
            if (cancelProducers_.load(std::memory_order_acquire)) return;
            const uint32_t observed =
                compilerWakeEpoch_.load(std::memory_order_acquire);
            if (AcquireCompilerPage(pageIndex)) break;
            WaitForAddressChange(compilerWakeEpoch_, observed);
        }
        CompiledEventPage& page = compiledPages_.Page(pageIndex);
        uint32_t compiledCount = 0u;
        uint32_t drained = 0u;
        const bool collapseCc = ccCollapseEnabled_.load(std::memory_order_relaxed);
        const bool collapseBend =
            bendCollapseEnabled_.load(std::memory_order_relaxed);
        const int64_t minNoteFrames = static_cast<int64_t>(
            minNoteMs_.load(std::memory_order_relaxed)) * sampleRate / 1000;
        auto compileOne = [&](const TimestampedMidiEvent& source) {
            ScheduledRenderEvent scheduled{};
            if (!CompileTimestampedEvent(source, epoch, qpcFreq, sampleRate,
                                        bufferFrames, scheduled)) {
                return;
            }
            // Opt-in minimum note length: a note-off closer than the floor to
            // its key's last note-on is moved out to note-on + floor.  The
            // page is sorted after compilation, so the new target frame
            // lands in order.
            if (minNoteFrames != 0 && scheduled.data1 < kNoteCount &&
                scheduled.channel < kChannelCount) {
                int64_t& lastOn = minNoteLastOnFrame_[
                    scheduled.channel * kNoteCount + scheduled.data1];
                if (scheduled.BaseType() == RenderEventType::NoteOn) {
                    lastOn = static_cast<int64_t>(scheduled.targetFrame);
                } else if (scheduled.type == RenderEventType::NoteOff &&
                           static_cast<int64_t>(scheduled.targetFrame) <
                               lastOn + minNoteFrames) {
                    scheduled.targetFrame =
                        static_cast<decltype(scheduled.targetFrame)>(
                            lastOn + minNoteFrames);
                }
            }
            // Opt-in CC collapse: a collapsible controller supersedes the
            // still-pending same-(channel,controller) event in this page by
            // overwriting its slot (the page is sorted after compilation, so
            // slot position is meaningless until publish). Channel-wide
            // state resets re-target controller meaning and invalidate the
            // channel's records instead.
            if (collapseCc) {
                if (scheduled.type == RenderEventType::ControlChange) {
                    if (IsCollapsibleController(scheduled.data1)) {
                        CcCollapseRecord& record =
                            ccCollapseRecords_[scheduled.channel][scheduled.data1];
                        if (record.valid && record.pageEpoch == ccPageFillEpoch_ &&
                            record.page == pageIndex &&
                            record.offset < compiledCount) {
                            page.events[record.offset] = scheduled;
                            ++ccCollapsedCount_;
                            return;
                        }
                        record.page = pageIndex;
                        record.offset = compiledCount;
                        record.pageEpoch = ccPageFillEpoch_;
                        record.valid = true;
                    } else if (IsControllerStateReset(scheduled.data1)) {
                        for (auto& controllerRecords :
                             ccCollapseRecords_[scheduled.channel]) {
                            controllerRecords.valid = false;
                        }
                    }
                } else if (scheduled.type == RenderEventType::Reset) {
                    for (auto& channelRecords : ccCollapseRecords_) {
                        for (auto& record : channelRecords) record.valid = false;
                    }
                }
            }
            if (collapseBend) {
                if (scheduled.type == RenderEventType::PitchBend &&
                    scheduled.channel < kChannelCount) {
                    CcCollapseRecord& record =
                        bendCollapseRecords_[scheduled.channel];
                    if (record.valid && record.pageEpoch == ccPageFillEpoch_ &&
                        record.page == pageIndex &&
                        record.offset < compiledCount) {
                        page.events[record.offset] = scheduled;
                        ++ccCollapsedCount_;
                        return;
                    }
                    record.page = pageIndex;
                    record.offset = compiledCount;
                    record.pageEpoch = ccPageFillEpoch_;
                    record.valid = true;
                } else if (scheduled.type == RenderEventType::Reset) {
                    for (auto& record : bendCollapseRecords_)
                        record.valid = false;
                }
            }
            page.events[compiledCount++] = scheduled;
        };
        compileOne(timed);
        ++drained;

        while (drained < kCompilerChunkCapacity &&
               midiIngress_.TryPopSequenceOrdered(timed, mergeState)) {
            compileOne(timed);
            ++drained;
        }

        // Ordering is paid once, outside the callback, directly in the final
        // immutable payload page. Publication transfers only its index.
        if (compiledCount != 0u) {
            SortCompiledEventPage(page.events, compiledPages_.SortScratch(),
                                  compiledCount);
            // Opt-in exact note-on merging: runs on the sorted page, so the
            // fold only ever sees events in their final dispatch order.
            if (noteMergeEnabled_.load(std::memory_order_relaxed)) {
                uint64_t folded = 0u;
                compiledCount = noteMerger_.Merge(
                    page.events, compiledCount,
                    noteMergeHoist_.load(std::memory_order_relaxed), folded);
                if (folded != 0u) {
                    noteMergedAtomic_.fetch_add(folded,
                                                std::memory_order_relaxed);
                }
            }
            while (!PublishCompilerPage(pageIndex, compiledCount)) {
                if (cancelProducers_.load(std::memory_order_acquire)) {
                    ReturnCompilerPage(pageIndex);
                    return;
                }
                const uint32_t observed =
                    compilerWakeEpoch_.load(std::memory_order_acquire);
                WaitForAddressChange(compilerWakeEpoch_, observed);
            }
        } else {
            ReturnCompilerPage(pageIndex);
        }
        // The page is no longer patchable — collapse records pointing into
        // it must never match a later page fill (indexes recycle).
        ++ccPageFillEpoch_;
        if (drained != 0u) {
            publishProducerSpace();
        }
        if (cancelProducers_.load(std::memory_order_acquire))
            continue;
    }
}

} // namespace svms
