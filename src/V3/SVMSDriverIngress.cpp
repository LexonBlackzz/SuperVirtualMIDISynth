// Event ingress: MIDI input, Submit*, SysEx, event compiler thread.

#include "SVMSDriverInternal.h"

namespace svms {

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
}

void Driver::SubmitShortMsg(uint32_t msg) {
    uint64_t timestamp = 0u;
    if (!tscClock_.Now(timestamp)) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        timestamp = static_cast<uint64_t>(qpc.QuadPart);
    }
    SubmitShortMsgAtQpc(msg, timestamp);
}

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
    out.submitted_events = submittedAtomic_.load(std::memory_order_relaxed);
    out.accepted_events = acceptedAtomic_.load(std::memory_order_relaxed);
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
    const uint32_t lanePressure = static_cast<uint32_t>(
        static_cast<uint64_t>(midiIngress_.LaneSize(lane)) * 100u /
        laneCapacity);
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

bool Driver::SubmitShortMsgAtQpcCancellable(
    uint32_t msg, uint64_t qpcTimestamp,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    submittedAtomic_.fetch_add(1, std::memory_order_relaxed);
    TimestampedMidiEvent evt{};
    evt.message = msg;
    evt.sequence = nextEventSequence_.fetch_add(1, std::memory_order_relaxed);
    evt.qpcTimestamp = qpcTimestamp;

    if (externalCancellation && externalCancellation->load(
            std::memory_order_acquire) == cancellationToken) {
        cancelledAtomic_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint8_t status = static_cast<uint8_t>(msg & 0xffu);
    const uint8_t data1 = static_cast<uint8_t>((msg >> 8) & (((status & 0xe0u) == 0x80u) ? 0xffu : 0x7fu));
    uint8_t velocity = static_cast<uint8_t>((msg >> 16) & 0x7fu);
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
    // audio thread at all. See SVMSNoteOnCollapse.h for the rationale.
    // When disabled (the default) every note-on spawns at its exact QPC
    // timestamp — retrigger/buzz precision is untouched.
    if (noteOn) {
        const uint32_t keyIndex =
            static_cast<uint32_t>(status & 0x0fu) * kNoteCount + data1;
        uint32_t stack = 0u;
        if (!noteOnCollapse_.OnNoteOn(keyIndex, qpcTimestamp, stack)) {
            coalescedAtomic_.fetch_add(1u, std::memory_order_relaxed);
            return true;
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

    EventLane lane = EventLane::State;
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
    if ((evt.sequence & 0xffu) == 0u) {
        cutoff = RefreshVelocityCutoff(lane);
    }

    const EventOverflowMode overflowMode =
        overflowMode_.load(std::memory_order_relaxed);
    if (noteOn && velocity < cutoff &&
        overflowMode == EventOverflowMode::PriorityVelocity) {
        shedAtomic_.fetch_add(1, std::memory_order_relaxed);
        shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const bool lossless = !noteOn || velocity >= highPriorityVelocity_ ||
                          overflowMode == EventOverflowMode::LosslessBackpressure;
    for (;;) {
            if (midiIngress_.TryPush(lane, evt)) {
                acceptedAtomic_.fetch_add(1, std::memory_order_relaxed);
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
            return true;
        }
        if (cancelProducers_.load(std::memory_order_acquire) ||
            (externalCancellation && externalCancellation->load(
                std::memory_order_acquire) == cancellationToken)) {
            cancelledAtomic_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        uint32_t observed = producerWakeEpoch_.load(std::memory_order_acquire);
        WaitForAddressChange(producerWakeEpoch_, observed);
    }
}

bool Driver::SubmitShortBatchAtQpcCancellable(
    const SVMS_ShortEvent* events, uint32_t eventCount,
    uint64_t immediateQpc,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (!events && eventCount != 0u) return false;

    // Full-velocity Black MIDI batches overwhelmingly occupy the loud lane.
    // Keep mixed/state/priority-shedding batches on the established per-event
    // path; homogeneous protected note-ons can reserve 256 lane cells with
    // one producer CAS while retaining one global sequence per event.
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
        for (uint32_t i = 0u; i < eventCount; ++i) {
            const uint64_t timestamp = events[i].timestamp_qpc != 0u
                ? events[i].timestamp_qpc : immediateQpc;
            if (!SubmitShortMsgAtQpcCancellable(
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
        const uint32_t laneCapacity = midiIngress_.LaneCapacity(EventLane::Loud);
        const uint32_t count = (std::min)(
            eventCount - cursor,
            (std::min)(kBatchReservation, laneCapacity));
        submittedAtomic_.fetch_add(count, std::memory_order_relaxed);
        const uint32_t firstSequence = nextEventSequence_.fetch_add(
            count, std::memory_order_relaxed);
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
            if (midiIngress_.TryPushBatch(
                    EventLane::Loud, prepared, count)) {
                acceptedAtomic_.fetch_add(count, std::memory_order_relaxed);
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
                return false;
            }
            const uint32_t observed = producerWakeEpoch_.load(
                std::memory_order_acquire);
            WaitForAddressChange(producerWakeEpoch_, observed);
        }
    }
    return true;
}

void Driver::SubmitSystemExclusive(const uint8_t* data, uint32_t size) {
    (void)SubmitSystemExclusiveCancellable(data, size, nullptr, 0u);
}

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
        while (!compiledPages_.AcquireForCompiler(pageIndex)) {
            if (cancelProducers_.load(std::memory_order_acquire)) return;
            const uint32_t observed =
                compilerWakeEpoch_.load(std::memory_order_acquire);
            if (compiledPages_.AcquireForCompiler(pageIndex)) break;
            WaitForAddressChange(compilerWakeEpoch_, observed);
        }
        CompiledEventPage& page = compiledPages_.Page(pageIndex);
        uint32_t compiledCount = 0u;
        uint32_t drained = 0u;
        const bool collapseCc = ccCollapseEnabled_.load(std::memory_order_relaxed);
        auto compileOne = [&](const TimestampedMidiEvent& source) {
            ScheduledRenderEvent scheduled{};
            if (!CompileTimestampedEvent(source, epoch, qpcFreq, sampleRate,
                                        bufferFrames, scheduled)) {
                return;
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
            while (!compiledPages_.PublishFromCompiler(pageIndex,
                                                        compiledCount)) {
                if (cancelProducers_.load(std::memory_order_acquire)) {
                    compiledPages_.ReturnUnusedFromCompiler(pageIndex);
                    return;
                }
                const uint32_t observed =
                    compilerWakeEpoch_.load(std::memory_order_acquire);
                WaitForAddressChange(compilerWakeEpoch_, observed);
            }
        } else {
            compiledPages_.ReturnUnusedFromCompiler(pageIndex);
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
