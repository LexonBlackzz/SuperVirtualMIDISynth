// Test cases split out of V3Tests.cpp; included inside its anonymous namespace.

void RenderDeterministic(float* left, float* right, uint32_t frames) {
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(4);
    svms::VoiceHandle voice = voices->AllocateVoice(0, 60, 127);
    voices->SetVoiceSample(voice, 0, 256, 0, 0, 0, 1.0f, 1);
    voices->v.phases[voice] = 0.0f;
    voices->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0, 0, 0, 0,
                            0.0f, 1.0f, 0.999f);
    voices->SetVoiceGain(voice, 1.0f, 1.0f);
    voices->SetVoiceSoundFontIdentity(voice, 0, 0);

    svms::ChannelCache channels;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    channels.RebuildCache(cfg, 44100.0f);

    int16_t samples[264]{};
    for (uint32_t i = 0; i < 256; ++i)
        samples[i] = static_cast<int16_t>((static_cast<int>(i % 17) - 8) * 4096);

    std::memset(left, 0, frames * sizeof(float));
    std::memset(right, 0, frames * sizeof(float));
    svms::RenderScalar renderer;
    renderer.RenderBlock(*voices, channels, samples, 256, left, right,
                         frames, cfg);
}

void TestPitchAndDeterministicRender() {
    svms::ChannelCache cache;
    cache.PitchBend(0, 0);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), -2.0f),
          "minimum pitch wheel uses the MIDI default -2 semitones");
    cache.PitchBend(0, 8192);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 0.0f),
          "center pitch wheel is unpitched");
    cache.PitchBend(0, 16383);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 2.0f, 2.0e-3f),
          "maximum pitch wheel uses the MIDI default +2 semitones");
    cache.ControlChange(0, 101, 0);
    cache.ControlChange(0, 100, 0);
    cache.ControlChange(0, 6, 12);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 12.0f, 2.0e-3f),
          "RPN pitch-bend sensitivity changes the range to +12 semitones");
    cache.PitchBend(0, 8192);
    cache.ControlChange(0, 100, 1);
    cache.ControlChange(0, 6, 65);
    cache.ControlChange(0, 38, 0);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 0.015625f),
          "RPN channel fine tuning is applied around the 8192 center");
    cache.ControlChange(0, 100, 2);
    cache.ControlChange(0, 6, 88);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 24.015625f),
          "RPN channel coarse tuning is combined with fine tuning");
    cache.ControlChange(0, 100, 0);
    cache.ControlChange(0, 6, 12);
    cache.PitchBend(0, 0);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 12.015625f),
          "coarse and fine tuning compensate an exact full-down wheel");
    cache.ResetControllers(0);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 0.0f),
          "reset controllers restores channel tuning and pitch wheel");

    float leftA[32], rightA[32], leftB[32], rightB[32];
    RenderDeterministic(leftA, rightA, 32);
    RenderDeterministic(leftB, rightB, 32);
    Check(std::memcmp(leftA, leftB, sizeof(leftA)) == 0,
          "scalar left output is deterministic");
    Check(std::memcmp(rightA, rightB, sizeof(rightA)) == 0,
          "scalar right output is deterministic");
}

void TestConfiguredVelocityMapping() {
    svms::ChannelCache cache;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.velocityCurve = 1.0f;
    cfg.velocityFloor = 0.0f;
    cfg.velocityIgnoreBelow = 64;

    Check(cache.ComputeVelocity(1, cfg) == 0.0f &&
              cache.ComputeVelocity(63, cfg) == 0.0f,
          "velocity_ignore_below suppresses only notes below the threshold");
    Check(NearlyEqual(cache.ComputeVelocity(64, cfg), 64.0f / 127.0f) &&
              cache.ComputeVelocity(65, cfg) > 0.0f &&
              NearlyEqual(cache.ComputeVelocity(127, cfg), 1.0f),
          "the threshold velocity and higher retain their original mapping");

    const float gatedVelocity64 = cache.ComputeVelocity(64, cfg);
    cfg.velocityIgnoreBelow = 0;
    Check(NearlyEqual(gatedVelocity64, cache.ComputeVelocity(64, cfg)),
          "velocity gating does not make admitted velocities quieter");

    cfg.velocityCurve = 2.0f;
    cfg.velocityFloor = 0.2f;
    const float expected = 0.2f + 0.8f * ::powf(64.0f / 127.0f, 2.0f);
    Check(NearlyEqual(cache.ComputeVelocity(64, cfg), expected, 1.0e-5f),
          "velocity curve and floor transform admitted note velocity");

    cfg.velocityIgnoreBelow = 32;
    cfg.ignoreVelocity = true;
    Check(cache.ComputeVelocity(31, cfg) == 0.0f &&
              cache.ComputeVelocity(32, cfg) == 1.0f,
          "ignore-velocity mode still honors the configured note threshold");

    float panLeft = 0.0f, panRight = 0.0f;
    cache.ComputeSoundFontPan(0, panLeft, panRight);
    Check(NearlyEqual(panLeft, 1.0f) && NearlyEqual(panRight, 1.0f),
          "centered SF2 pan preserves channel gain");
    cache.ComputeSoundFontPan(-500, panLeft, panRight);
    Check(NearlyEqual(panLeft, ::sqrtf(2.0f)) && NearlyEqual(panRight, 0.0f),
          "hard-left SF2 pan preserves power and stereo placement");

    Check(svms::ComputeDecimationStep(10000u) == 1,
          "pools up to 10000 voices render at full quality");
    Check(svms::ComputeDecimationStep(10001u) == 2 &&
              svms::ComputeDecimationStep(25000u) == 2,
          "10001-25000 active voices decimate every other voice");
    Check(svms::ComputeDecimationStep(25001u) == 4 &&
              svms::ComputeDecimationStep(50000u) == 4,
          "25001-50000 active voices decimate to every fourth voice");
    Check(svms::ComputeDecimationStep(50001u) == 8 &&
              svms::ComputeDecimationStep(150000u) == 8,
          "50001-150000 active voices decimate to every eighth voice");
    Check(svms::ComputeDecimationStep(150001u) == 16,
          "beyond 150000 active voices decimate to every sixteenth voice");
}

void TestEventRingWrapAndCapacity() {
    constexpr uint32_t capacity = svms::kDefaultEventRingCapacity;
    svms::SPSCQueue<svms::TimestampedMidiEvent, capacity> queue;
    svms::TimestampedMidiEvent event{};
    for (uint32_t i = 0; i < capacity; ++i) {
        event.sequence = i;
        Check(queue.Push(event), "event ring accepts every slot");
    }
    event.sequence = capacity;
    Check(!queue.Push(event), "event ring detects full state");
    for (uint32_t i = 0; i < capacity; ++i) {
        svms::TimestampedMidiEvent out{};
        Check(queue.TryPop(out) && out.sequence == i, "event ring preserves sequence");
        event.sequence = capacity + i;
        Check(queue.Push(event), "event ring wraps after consuming");
    }
    for (uint32_t i = 0; i < capacity; ++i) {
        svms::TimestampedMidiEvent out{};
        Check(queue.TryPop(out) && out.sequence == capacity + i,
              "wrapped event ring preserves order");
    }
    Check(queue.IsEmpty(), "event ring is empty after full wrap test");

    // Configuration-controlled handoff queues are not constrained by the
    // old 393,216-slot compile-time array.
    constexpr uint32_t dynamicCapacity = 400003u;
    svms::DynamicSPSCQueue<svms::TimestampedMidiEvent> dynamicQueue;
    Check(dynamicQueue.ConfigureCapacity(dynamicCapacity) &&
              dynamicQueue.CapacityValue() == dynamicCapacity,
          "runtime event ring accepts a capacity above the legacy ceiling");
    for (uint32_t i = 0u; i < dynamicCapacity; ++i) {
        event.sequence = i;
        Check(dynamicQueue.Push(event),
              "runtime event ring accepts every configured slot");
    }
    Check(!dynamicQueue.Push(event),
          "runtime event ring detects its configured full state");
    for (uint32_t i = 0u; i < dynamicCapacity; ++i) {
        svms::TimestampedMidiEvent out{};
        Check(dynamicQueue.TryPop(out) && out.sequence == i,
              "runtime event ring preserves sequence above legacy capacity");
    }
    Check(dynamicQueue.IsEmpty(),
          "runtime event ring drains after a capacity-sized burst");
}

void TestWindowedSchedulerOrdering() {
    svms::EventScheduler scheduler(8);
    for (uint32_t i = 0; i < 4; ++i) {
        svms::ScheduledRenderEvent e;
        e.targetFrame = (i & 1) ? 4 : 2;
        e.sequence = i;
        Check(scheduler.Enqueue(e), "scheduler accepts event");
    }
    svms::ScheduledRenderEvent out;
    Check(scheduler.PopBefore(3, out) && out.sequence == 0,
          "scheduler orders first sample by sequence");
    Check(scheduler.PopBefore(3, out) && out.sequence == 2,
          "scheduler preserves equal-time ordering");
    Check(scheduler.PopBefore(5, out) && out.sequence == 1,
          "scheduler advances to next sample window");
    Check(scheduler.PopBefore(5, out) && out.sequence == 3,
          "scheduler drains final equal-time event");
    Check(scheduler.Empty(), "scheduler drains completely");

    svms::EventScheduler batched(256);
    for (uint32_t i = 0; i < 200; ++i) {
        svms::ScheduledRenderEvent event;
        event.targetFrame = static_cast<int64_t>((i * 37u) % 11u);
        event.sequence = 1000u - i;
        Check(batched.EnqueueBatched(event), "batched scheduler accepts event");
    }
    batched.FinalizeBatch();
    int64_t previousFrame = -1;
    uint32_t previousSequence = 0;
    uint32_t popped = 0;
    while (batched.PopBefore(12, out)) {
        const bool ordered = out.targetFrame > previousFrame ||
            (out.targetFrame == previousFrame && out.sequence >= previousSequence);
        Check(ordered, "linear heap rebuild preserves frame/sequence ordering");
        previousFrame = out.targetFrame;
        previousSequence = out.sequence;
        ++popped;
    }
    Check(popped == 200 && batched.Empty(),
          "batched scheduler drains every rebuilt-heap event");

    // Production receives concatenated compiler chunks. Each chunk is
    // ordered internally, but adjacent chunks overlap in absolute time.
    svms::EventScheduler merged(4096);
    std::vector<svms::ScheduledRenderEvent> expected;
    for (uint32_t chunk = 0u; chunk < 12u; ++chunk) {
        for (uint32_t i = 0u; i < 128u; ++i) {
            svms::ScheduledRenderEvent event;
            event.targetFrame = static_cast<int64_t>(i) - 32;
            event.sequence = chunk * 128u + i;
            Check(merged.EnqueueBatched(event),
                  "scheduler accepts a sorted compiler chunk");
            expected.push_back(event);
        }
    }
    std::stable_sort(expected.begin(), expected.end(),
        [](const auto& a, const auto& b) {
            return a.targetFrame < b.targetFrame ||
                (a.targetFrame == b.targetFrame && a.sequence < b.sequence);
        });
    merged.FinalizeBatch();
    for (const auto& event : expected) {
        Check(merged.PopBefore(INT64_MAX, out) &&
                  out.targetFrame == event.targetFrame &&
                  out.sequence == event.sequence,
              "natural-run merge preserves exact frame/sequence order");
    }
    Check(merged.Empty(), "natural-run scheduler drains completely");

    merged.ConfigureCapacity(32u);
    Check(merged.Capacity() == 32u && merged.Empty(),
          "scheduler capacity can be configured before real-time use");
    merged.ConfigureCapacity(400003u);
    Check(merged.Capacity() == 400003u && merged.Empty(),
          "scheduler allocation also exceeds the former fixed ceiling");
}

void TestPagedSchedulerOrderingAndRecycling() {
    constexpr uint32_t pageEvents = 257u;
    constexpr uint32_t pageCount = 3u;
    constexpr uint32_t totalEvents = pageEvents * pageCount;
    svms::CompiledEventPagePool pool;
    Check(pool.ConfigureCapacity(totalEvents),
          "compiled page pool allocates configured event storage");
    svms::PagedEventScheduler scheduler;
    Check(scheduler.Configure(&pool, totalEvents),
          "paged scheduler allocates only page cursors and winner tree");

    std::vector<svms::ScheduledRenderEvent> expected;
    expected.reserve(totalEvents);
    constexpr uint32_t sequenceBase = UINT32_MAX - 300u;
    for (uint32_t pageNumber = 0u; pageNumber < pageCount; ++pageNumber) {
        uint32_t pageIndex = svms::kInvalidEventPage;
        Check(pool.AcquireForCompiler(pageIndex),
              "compiler obtains a free immutable page");
        auto& page = pool.Page(pageIndex);
        for (uint32_t i = 0u; i < pageEvents; ++i) {
            svms::ScheduledRenderEvent event{};
            const uint32_t logicalSequence = pageNumber * pageEvents + i;
            event.sequence = sequenceBase + logicalSequence;
            event.targetFrame = static_cast<int64_t>(
                (logicalSequence * 97u + pageNumber * 13u) % 41u) - 9;
            event.channel = static_cast<uint8_t>(logicalSequence & 15u);
            event.data1 = static_cast<uint8_t>(logicalSequence & 127u);
            page.events[i] = event;
            expected.push_back(event);
        }
        svms::SortCompiledEventPage(page.events, pool.SortScratch(), pageEvents);
        Check(pool.PublishFromCompiler(pageIndex, pageEvents),
              "compiler publishes one sorted immutable page");
    }

    std::stable_sort(expected.begin(), expected.end(), [](const auto& a,
                                                          const auto& b) {
        if (a.targetFrame != b.targetFrame)
            return a.targetFrame < b.targetFrame;
        return static_cast<int32_t>(a.sequence - b.sequence) < 0;
    });
    Check(scheduler.ImportAllReady() == pageCount &&
              scheduler.Size() == totalEvents &&
              scheduler.ActivePages() == pageCount,
          "audio imports page descriptors without copying payloads");

    g_realtimeAllocationCount.store(0u, std::memory_order_relaxed);
    g_trackRealtimeAllocations = true;
    svms::ScheduledRenderEvent actual{};
    for (const auto& wanted : expected) {
        const bool popped = scheduler.PopBefore(INT64_MAX, actual);
        if (!popped || actual.targetFrame != wanted.targetFrame ||
            actual.sequence != wanted.sequence) {
            g_trackRealtimeAllocations = false;
            Check(false,
                  "page-head winner tree preserves frame and wrapped sequence order");
            g_trackRealtimeAllocations = true;
            break;
        }
    }
    g_trackRealtimeAllocations = false;
    Check(g_realtimeAllocationCount.load(std::memory_order_relaxed) == 0u,
          "page import and winner advancement allocate nothing on audio thread");
    Check(scheduler.Size() == 0u && scheduler.ActivePages() == 0u,
          "exhausted page cursors return every payload page to the compiler");

    uint32_t recycledIndex = svms::kInvalidEventPage;
    Check(pool.AcquireForCompiler(recycledIndex),
          "compiler reuses a page recycled by the audio thread");
}

void TestFairPriorityLaneDrain() {
    svms::PriorityEventIngress<svms::TimestampedMidiEvent> ingress;
    Check(ingress.ConfigureCapacity(400003u) &&
              ingress.TotalCapacity() == 400003u &&
              ingress.LaneCapacity(svms::EventLane::State) == 133334u &&
              ingress.LaneCapacity(svms::EventLane::Quiet) == 33335u,
          "priority ingress derives exact runtime lane capacities");
    svms::TimestampedMidiEvent batch[17]{};
    for (uint32_t i = 0u; i < 17u; ++i) {
        batch[i].sequence = 1000u + i;
        batch[i].message = 0x90u | ((40u + i) << 8u) | (127u << 16u);
        batch[i].qpcTimestamp = 5000u + i;
    }
    Check(ingress.TryPushBatch(svms::EventLane::Medium, batch, 17u),
          "MPSC lane reserves a contiguous producer batch");
    for (uint32_t i = 0u; i < 17u; ++i) {
        svms::TimestampedMidiEvent event{};
        Check(ingress.TryPop(svms::EventLane::Medium, event) &&
                  event.sequence == batch[i].sequence &&
                  event.message == batch[i].message &&
                  event.qpcTimestamp == batch[i].qpcTimestamp,
              "MPSC batch publication preserves FIFO payload order");
    }
    for (uint32_t i = 0u; i < 64u; ++i) {
        svms::TimestampedMidiEvent state{};
        state.sequence = i * 2u;
        svms::TimestampedMidiEvent loud{};
        loud.sequence = i * 2u + 1u;
        Check(ingress.TryPush(svms::EventLane::State, state),
              "state lane accepts fairness test event");
        Check(ingress.TryPush(svms::EventLane::Loud, loud),
              "loud lane accepts fairness test event");
    }
    uint32_t cursor = 0u;
    uint32_t stateCount = 0u;
    uint32_t loudCount = 0u;
    for (uint32_t i = 0u; i < 128u; ++i) {
        svms::TimestampedMidiEvent event{};
        Check(ingress.TryPopFair(event, cursor),
              "fair drain returns every queued event");
        if ((event.sequence & 1u) == 0u) ++stateCount;
        else ++loudCount;
        Check(stateCount <= loudCount + 1u && loudCount <= stateCount + 1u,
              "saturated state and loud lanes cannot starve each other");
    }
    Check(stateCount == 64u && loudCount == 64u,
          "fair drain consumes both saturated priority lanes");

    constexpr uint32_t eventsPerLane = 257u;
    constexpr uint32_t totalRunEvents = eventsPerLane * 5u;
    constexpr uint64_t epoch = 50'000'000u;
    constexpr uint64_t frequency = 10'000'000u;
    constexpr uint32_t sampleRate = 44'100u;
    svms::PriorityEventIngress<svms::TimestampedMidiEvent> runIngress;
    std::vector<svms::ScheduledRenderEvent> expected;
    expected.reserve(totalRunEvents);
    for (uint32_t laneIndex = 0u; laneIndex < 5u; ++laneIndex) {
        const auto lane = static_cast<svms::EventLane>(laneIndex);
        for (uint32_t i = 0u; i < eventsPerLane; ++i) {
            svms::TimestampedMidiEvent event{};
            event.sequence = i * 5u + laneIndex;
            event.qpcTimestamp = epoch +
                static_cast<uint64_t>((event.sequence * 7'919u) % 100'003u);
            event.message = 0x90u | ((event.sequence & 0x7fu) << 8u) |
                (127u << 16u);
            Check(runIngress.TryPush(lane, event),
                  "run drain accepts every lane event");
            svms::ScheduledRenderEvent scheduled{};
            Check(svms::CompileTimestampedEvent(
                      event, epoch, frequency, sampleRate, 2048u, scheduled),
                  "run drain fixture compiles");
            expected.push_back(scheduled);
        }
    }
    svms::EventScheduler runScheduler(totalRunEvents);
    std::vector<uint8_t> runSeen(totalRunEvents, 0u);
    uint32_t runCursor = 0u;
    const uint32_t runDrained = runIngress.DrainFairRuns(
        totalRunEvents, 37u, runCursor,
        [&](const svms::TimestampedMidiEvent& event) {
            Check(event.sequence < totalRunEvents,
                  "run drain sequence remains in range");
            if (event.sequence < totalRunEvents) {
                Check(runSeen[event.sequence] == 0u,
                      "run drain never duplicates an event");
                runSeen[event.sequence] = 1u;
            }
            svms::ScheduledRenderEvent scheduled{};
            Check(svms::CompileTimestampedEvent(
                      event, epoch, frequency, sampleRate, 2048u, scheduled) &&
                      runScheduler.EnqueueBatched(scheduled),
                  "run drain compiles every event into the scheduler");
        });
    Check(runDrained == totalRunEvents && runIngress.TotalSize() == 0u,
          "run drain consumes every lane without starvation");
    Check(std::all_of(runSeen.begin(), runSeen.end(),
                      [](uint8_t value) { return value == 1u; }),
          "run drain preserves every event exactly once");

    std::sort(expected.begin(), expected.end(), [](const auto& left,
                                                    const auto& right) {
        return left.targetFrame < right.targetFrame ||
            (left.targetFrame == right.targetFrame &&
             left.sequence < right.sequence);
    });
    runScheduler.FinalizeBatch();
    svms::ScheduledRenderEvent actual{};
    for (const auto& wanted : expected) {
        Check(runScheduler.PopBefore(INT64_MAX, actual) &&
                  actual.targetFrame == wanted.targetFrame &&
                  actual.sequence == wanted.sequence,
              "bounded lane runs restore exact frame/sequence order");
    }
    Check(runScheduler.Empty(),
          "bounded lane-run scheduler drains without extra events");
}

void TestFourProducerMPSCIntegrity() {
    constexpr uint32_t producerCount = 4;
    constexpr uint32_t eventsPerProducer = 4000;
    constexpr uint32_t totalEvents = producerCount * eventsPerProducer;
    svms::MPSCQueue<svms::TimestampedMidiEvent, 4096> queue;
    std::atomic<uint32_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;

    for (uint32_t producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (uint32_t i = 0; i < eventsPerProducer;) {
                constexpr uint32_t batchCount = 8u;
                svms::TimestampedMidiEvent batch[batchCount]{};
                const uint32_t count = (producer & 1u) != 0u
                    ? (std::min)(batchCount, eventsPerProducer - i) : 1u;
                for (uint32_t j = 0u; j < count; ++j) {
                    auto& event = batch[j];
                    event.sequence = producer * eventsPerProducer + i + j;
                    event.message = 0x90u | ((60u + producer) << 8) |
                        (127u << 16);
                    event.qpcTimestamp = event.sequence * 17ull;
                }
                if (count > 1u) {
                    while (!queue.TryPushBatch(batch, count))
                        std::this_thread::yield();
                } else {
                    while (!queue.TryPush(batch[0])) std::this_thread::yield();
                }
                i += count;
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != producerCount)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);

    std::vector<uint8_t> seen(totalEvents, 0);
    uint32_t consumed = 0;
    while (consumed != totalEvents) {
        svms::TimestampedMidiEvent event{};
        if (!queue.TryPop(event)) {
            std::this_thread::yield();
            continue;
        }
        Check(event.sequence < totalEvents, "MPSC event sequence is in range");
        if (event.sequence < totalEvents) {
            Check(seen[event.sequence] == 0, "MPSC does not duplicate events");
            seen[event.sequence] = 1;
            Check(event.qpcTimestamp == event.sequence * 17ull,
                  "MPSC event payload is not torn or corrupted");
        }
        ++consumed;
    }
    for (auto& producer : producers) producer.join();
    Check(queue.Size() == 0, "MPSC drains after four concurrent producers");
    Check(std::all_of(seen.begin(), seen.end(), [](uint8_t value) { return value == 1; }),
          "MPSC preserves every producer event");
}

void TestJsonConfigurationLifecycle() {
    namespace fs = std::filesystem;
    wchar_t tempRoot[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempRoot);
    const fs::path directory = fs::path(tempRoot) /
        (L"SVMS V3 config unicode \u03a9 " + std::to_wstring(GetCurrentProcessId()));
    const fs::path configPath = directory / L"config.json";
    std::error_code ec;
    fs::remove_all(directory, ec);
    const fs::path soundFontDirectory = directory / L"soundfonts";
    fs::create_directories(soundFontDirectory, ec);
    const fs::path alphaSoundFont = soundFontDirectory / L"Alpha Piano.SF2";
    const fs::path betaSoundFont = soundFontDirectory / L"beta.sf2";
    {
        std::ofstream alpha(alphaSoundFont, std::ios::binary);
        std::ofstream beta(betaSoundFont, std::ios::binary);
        alpha << "test";
        beta << "test";
    }
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", configPath.c_str());
    SetEnvironmentVariableW(L"SVMS_TEST_SOUNDFONT_DIRECTORY",
                            soundFontDirectory.c_str());

    svms::EngineConfig created = svms::EngineConfig::Load();
    Check(fs::exists(configPath), "first run creates config.json");
    Check(created.sampleRate == 44100 && created.bufferFrames == 2048,
          "first-run JSON uses compiled audio defaults");
    Check(created.audioDevice == L"default",
          "first-run JSON selects the current Windows default audio output");
    Check(!created.midiInputEnabled && created.midiInputDevice.empty(),
          "first-run JSON keeps direct physical MIDI routing opt-in");
    Check(created.eventRingCapacity == 393216 && created.highPriorityVelocity == 96,
          "first-run JSON uses priority ingress defaults");
#if defined(SVMS_XP_COMPAT)
    Check(created.diagnosticsEnabled && created.diagnosticsWindow,
          "first-run XP JSON opens the diagnostic window by default");
#else
    Check(!created.diagnosticsEnabled && !created.diagnosticsWindow,
          "first-run modern JSON keeps diagnostics opt-in");
#endif
    Check(created.soundFontPath == L"Alpha Piano.SF2",
          "first-run JSON records a discovered DLL-local SoundFont name");
    {
        std::ifstream input(configPath, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), {});
        Check(text.find("\"schema_version\": 1") != std::string::npos,
              "created JSON carries schema version");
        Check(text.find("\"correctness_mode\": true") != std::string::npos,
              "created JSON enables scalar correctness mode");
#if defined(SVMS_XP_COMPAT)
        Check(text.find("\"render_threads\": 1") != std::string::npos,
              "created XP JSON records the single-thread compatibility default");
#else
        Check(text.find("\"render_threads\": 0") != std::string::npos,
              "created modern JSON enables topology-aware rendering");
#endif
        Check(text.find("\"device\": \"default\"") != std::string::npos,
              "created JSON immediately records the default audio output");
        Check(text.find("\"input_enabled\": false") != std::string::npos,
              "created JSON records disabled physical MIDI input routing");
        Check(text.find("Alpha Piano.SF2") != std::string::npos,
              "created JSON explicitly stores the discovered SoundFont");
    }

    std::string queriedConfig;
    std::string configEditWarning;
    Check(svms::ReadV3ConfigJson(queriedConfig, &configEditWarning) &&
              queriedConfig.find("\"schema_version\": 1") !=
                  std::string::npos,
          "native config reader returns the complete selected JSON document");
    const std::string validPatch =
        R"json({"synth":{"max_voices":321},"future_extension":{"kept":true}})json";
    const bool validPatchApplied = svms::PatchV3ConfigJson(
        validPatch.data(), validPatch.size(), &configEditWarning);
    if (!validPatchApplied)
        std::printf("INFO: config patch rejection: %s\n",
                    configEditWarning.c_str());
    Check(validPatchApplied,
          "native config merge patch accepts valid known and unknown fields");
    svms::EngineConfig patchedConfig = svms::EngineConfig::Load();
    Check(patchedConfig.maxVoices == 321u,
          "native config merge patch persists recognized values");
    Check(svms::ReadV3ConfigJson(queriedConfig, &configEditWarning) &&
              queriedConfig.find("future_extension") != std::string::npos &&
              queriedConfig.find("Alpha Piano.SF2") != std::string::npos,
          "native config merge patch preserves untouched and unknown fields");
    const std::string beforeRejectedPatch = queriedConfig;
    const std::string invalidPatch =
        R"json({"audio":{"sample_rate":1}})json";
    Check(!svms::PatchV3ConfigJson(invalidPatch.data(), invalidPatch.size(),
                                   &configEditWarning) &&
              !configEditWarning.empty(),
          "native config merge patch rejects invalid recognized values");
    Check(svms::ReadV3ConfigJson(queriedConfig, nullptr) &&
              queriedConfig == beforeRejectedPatch,
          "rejected native config patch leaves the document byte-unchanged");

    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << R"json({"schema_version":1,"synth":{"soundfont":"Alpha Piano.SF2","soundfonts":["Alpha Piano.SF2","beta.sf2"],"soundfont_routes":[{"soundfont":1,"bank":4,"preset":-1,"source_bank":0,"source_preset":-1,"percussion":false}]}})json";
    }
    svms::EngineConfig stacked = svms::EngineConfig::Load();
    const std::vector<std::wstring> resolvedStack =
        svms::ResolveV3SoundFontPaths(stacked);
    Check(stacked.soundFontPaths.size() == 2u &&
              stacked.soundFontRoutes.size() == 1u &&
              stacked.soundFontRoutes[0].soundFontIndex == 1u &&
              stacked.soundFontRoutes[0].targetBank == 4u,
          "JSON loads priority SoundFont stack and explicit route");
    Check(resolvedStack.size() == 2u &&
              fs::path(resolvedStack[0]) == alphaSoundFont &&
              fs::path(resolvedStack[1]) == betaSoundFont,
          "SoundFont stack paths resolve in stable priority order");

    svms::EngineConfig explicitAbsolute = svms::EngineConfig::Default();
    explicitAbsolute.soundFontPath = betaSoundFont.wstring();
    Check(fs::path(svms::ResolveV3SoundFontPath(explicitAbsolute)) == betaSoundFont,
          "absolute configured SoundFont paths take precedence");
    svms::EngineConfig explicitRelative = svms::EngineConfig::Default();
    explicitRelative.soundFontPath = L"Alpha Piano.SF2";
    Check(fs::path(svms::ResolveV3SoundFontPath(explicitRelative)) == alphaSoundFont,
          "relative configured SoundFont paths resolve beside winmm.dll");
    svms::EngineConfig missingConfigured = svms::EngineConfig::Default();
    missingConfigured.soundFontPath = L"missing.sf2";
    std::string discoveryWarning;
    Check(fs::path(svms::ResolveV3SoundFontPath(
              missingConfigured, &discoveryWarning)) == alphaSoundFont &&
              !discoveryWarning.empty(),
          "missing configured SoundFont falls back to deterministic local discovery");

    fs::remove(configPath, ec);
    svms::EngineConfig concurrentA{};
    svms::EngineConfig concurrentB{};
    std::thread creatorA([&] { concurrentA = svms::EngineConfig::Load(); });
    std::thread creatorB([&] { concurrentB = svms::EngineConfig::Load(); });
    creatorA.join();
    creatorB.join();
    Check(fs::exists(configPath) && concurrentA.Validate() && concurrentB.Validate(),
          "named mutex serializes concurrent first-run creation");

    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << R"json({"schema_version":1,"audio":{"device":"Playback 1/2 (E4x4 Pre)","sample_rate":1,"buffer_frames":256},"synth":{"max_voices":64},"unknown":{"preserve_me":true}})json";
    }
    svms::EngineConfig invalidField = svms::EngineConfig::Load();
    Check(invalidField.sampleRate == 44100,
          "invalid JSON field retains its compiled default");
    Check(invalidField.bufferFrames == 256 && invalidField.maxVoices == 64,
          "valid JSON fields still apply beside an invalid field");
    Check(invalidField.audioDevice == L"Playback 1/2 (E4x4 Pre)",
          "configured WASAPI endpoint friendly name is preserved");
    Check(!invalidField.configWarning.empty(), "invalid JSON field publishes warning");

    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << R"json({"schema_version":1,"events":{"ring_capacity":1048576,"max_events_per_block":700000}})json";
    }
    svms::EngineConfig largeEventStorage = svms::EngineConfig::Load();
    Check(largeEventStorage.eventRingCapacity == 1048576u &&
              largeEventStorage.maxEventsPerBlock == 700000u &&
              largeEventStorage.Validate(),
          "JSON accepts event storage above the former fixed ceiling");

    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << R"json({"schema_version":1,"events":{"ring_capacity":4096,"max_events_per_block":65536}})json";
    }
    svms::EngineConfig independentEventStorage = svms::EngineConfig::Load();
    Check(independentEventStorage.eventRingCapacity == 4096u &&
              independentEventStorage.maxEventsPerBlock == 65536u &&
              independentEventStorage.Validate(),
          "queue capacity and callback event budget remain independent");

    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << R"json({"schema_version":1,"synth":{"render_threads":8},"midi":{"input_enabled":true,"input_device":"Unicode MIDI Ω"}})json";
    }
    svms::EngineConfig workerConfiguration = svms::EngineConfig::Load();
    Check(workerConfiguration.renderThreads == 8u &&
              workerConfiguration.Validate(),
          "JSON selects a fixed voice-render thread count");
    Check(workerConfiguration.midiInputEnabled &&
              workerConfiguration.midiInputDevice == L"Unicode MIDI Ω",
          "JSON selects a named physical MIDI input");

    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << R"json({"schema_version":1,"events":{"ring_capacity":4294967295,"max_events_per_block":4294967295}})json";
    }
    svms::EngineConfig maximumEventStorage = svms::EngineConfig::Load();
    Check(maximumEventStorage.eventRingCapacity == UINT32_MAX &&
              maximumEventStorage.maxEventsPerBlock == UINT32_MAX &&
              maximumEventStorage.Validate(),
          "JSON event storage has no cache-size-derived ceiling");

    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << "{ malformed";
    }
    svms::EngineConfig malformed = svms::EngineConfig::Load();
    Check(malformed.sampleRate == 44100 && !malformed.configWarning.empty(),
          "malformed JSON runs with defaults and warning");
    {
        std::ifstream input(configPath, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), {});
        Check(text == "{ malformed", "malformed JSON is never overwritten");
    }

    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << R"({"schema_version":99,"audio":{"sample_rate":96000}})";
    }
    svms::EngineConfig newer = svms::EngineConfig::Load();
    Check(newer.sampleRate == 44100 && !newer.configWarning.empty(),
          "newer schema is rejected without applying values");

    const fs::path legacyPath = directory / L"legacy config.ini";
    {
        std::ofstream output(legacyPath, std::ios::binary | std::ios::trunc);
        output << "sample_rate=48000\n"
                  "max_voices=777\n"
                  "master_volume=0.37\n"
                  "soundfont=Unicode SoundFont.sf2\n"
                  "limiter_enable=false\n"
                  "limiter_threshold=0.95\n"
                  "reverb_enable=true\n";
    }
    fs::remove(configPath, ec);
    SetEnvironmentVariableW(L"SVMS_TEST_LEGACY_INI", legacyPath.c_str());
    svms::EngineConfig migrated = svms::EngineConfig::Load();
    Check(migrated.sampleRate == 48000 && migrated.maxVoices == 777 &&
          NearlyEqual(migrated.masterVolume, 0.37f),
          "first creation imports recognized legacy INI engine values");
    Check(migrated.soundFontPath == L"Unicode SoundFont.sf2",
          "legacy SoundFont path migrates into JSON");
    {
        std::ifstream input(configPath, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), {});
        Check(text.find("legacy_import") != std::string::npos &&
              text.find("limiter_threshold") != std::string::npos,
              "legacy effect and limiter values are preserved in migration JSON");
    }
    SetEnvironmentVariableW(L"SVMS_TEST_LEGACY_INI", nullptr);

    SetEnvironmentVariableW(L"SVMS_NO_DROP_EVENTS", L"1");
    SetEnvironmentVariableW(L"SVMS_CORRECTNESS_MODE", L"0");
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS", L"1");
    SetEnvironmentVariableW(L"SVMS_AUDIO_DEVICE", L"Test Render Endpoint");
    svms::EngineConfig environment = svms::EngineConfig::Load();
    Check(environment.eventOverflowMode == svms::EventOverflowMode::LosslessBackpressure,
          "lossless compatibility environment override has final precedence");
    Check(!environment.correctnessMode && environment.diagnosticsEnabled,
          "correctness and diagnostics environment overrides have final precedence");
    Check(environment.audioDevice == L"Test Render Endpoint",
          "audio endpoint environment override has final precedence");

    const fs::path portableDirectory = directory / L"portable demo";
    const fs::path portableConfig = portableDirectory / L"config.json";
    fs::create_directories(portableDirectory, ec);
    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << R"({"schema_version":1,"audio":{"sample_rate":48000}})";
    }
    {
        std::ofstream output(portableConfig, std::ios::binary | std::ios::trunc);
        output << R"({"schema_version":1,"audio":{"sample_rate":96000}})";
    }
    SetEnvironmentVariableW(L"SVMS_TEST_LOCAL_CONFIG_PATH", portableConfig.c_str());
    svms::EngineConfig portable = svms::EngineConfig::Load();
    Check(portable.sampleRate == 96000 &&
              fs::path(portable.configPath) == portableConfig,
          "local config.json takes precedence over the AppData config");

    fs::remove(portableConfig, ec);
    svms::EngineConfig appDataFallback = svms::EngineConfig::Load();
    Check(appDataFallback.sampleRate == 48000 &&
              fs::path(appDataFallback.configPath) == configPath,
          "missing local config.json falls back to AppData");

    const fs::path blockedParent = directory / L"blocked AppData";
    {
        std::ofstream output(blockedParent, std::ios::binary | std::ios::trunc);
        output << "not a directory";
    }
    const fs::path unavailableAppData = blockedParent / L"config.json";
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", unavailableAppData.c_str());
    svms::EngineConfig portableCreation = svms::EngineConfig::Load();
    Check(fs::exists(portableConfig) &&
              fs::path(portableCreation.configPath) == portableConfig,
          "unavailable AppData creates a portable config beside the DLL");

    fs::remove(alphaSoundFont, ec);
    fs::remove(betaSoundFont, ec);
    svms::EngineConfig noConfiguredSoundFont = svms::EngineConfig::Default();
    std::string noSoundFontWarning;
    Check(svms::ResolveV3SoundFontPath(
              noConfiguredSoundFont, &noSoundFontWarning).empty() &&
              !noSoundFontWarning.empty(),
          "missing configuration and local SF2 produce an explicit warning");

    SetEnvironmentVariableW(L"SVMS_NO_DROP_EVENTS", nullptr);
    SetEnvironmentVariableW(L"SVMS_CORRECTNESS_MODE", nullptr);
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS", nullptr);
    SetEnvironmentVariableW(L"SVMS_AUDIO_DEVICE", nullptr);
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", nullptr);
    SetEnvironmentVariableW(L"SVMS_TEST_LOCAL_CONFIG_PATH", nullptr);
    SetEnvironmentVariableW(L"SVMS_TEST_SOUNDFONT_DIRECTORY", nullptr);
    fs::remove_all(directory, ec);
}

void TestSixHourFrameClockDrift() {
    constexpr int64_t qpcFrequency = 10'000'000;
    constexpr uint32_t sampleRate = 44'100;
    constexpr int64_t durationSeconds = 6 * 60 * 60;
    const int64_t endQpc = durationSeconds * qpcFrequency;
    const int64_t expectedFrames = durationSeconds * static_cast<int64_t>(sampleRate);
    Check(svms::QpcDeltaToFrames(endQpc, qpcFrequency, sampleRate) == expectedFrames,
          "fixed-epoch QPC conversion has no six-hour accumulated drift");

    svms::TimestampedMidiEvent frameTimed{};
    frameTimed.qpcTimestamp = svms::kAbsoluteFrameTimestampTag | 1234567u;
    frameTimed.message = 0x00643c90u;
    frameTimed.sequence = 77u;
    svms::ScheduledRenderEvent frameScheduled{};
    Check(svms::CompileTimestampedEvent(
              frameTimed, 999999u, qpcFrequency, sampleRate, 2048u,
              frameScheduled) && frameScheduled.targetFrame == 1234567 &&
              frameScheduled.sequence == 77u,
          "absolute-frame API timestamps bypass QPC conversion exactly");

    // Exercise non-integral endpoints and every required WASAPI block size.
    static constexpr uint32_t blockSizes[] = {16, 32, 64, 128, 256, 512,
                                               1024, 2048, 4096, 8192};
    for (uint32_t block : blockSizes) {
        const int64_t nearEndFrame = expectedFrames - block;
        const int64_t nearEndQpc = nearEndFrame * qpcFrequency / sampleRate;
        const int64_t recovered = svms::QpcDeltaToFrames(
            nearEndQpc, qpcFrequency, sampleRate);
        Check(std::llabs(recovered - nearEndFrame) <= 1,
              "fixed-epoch frame conversion remains within one frame");
    }
}

void TestMidiByteStream() {
    svms::MidiByteStreamParser parser;
    std::vector<uint32_t> shorts;
    std::vector<std::vector<uint8_t>> sysex;
    auto feed = [&](std::initializer_list<uint8_t> bytes) {
        std::vector<uint8_t> v(bytes);
        parser.Feed(v.data(), v.size(),
                    [&](uint32_t m) { shorts.push_back(m); },
                    [&](const uint8_t* d, uint32_t n) {
                        sysex.emplace_back(d, d + n);
                    });
    };
    // Packed events + running status + realtime byte inside a message.
    feed({0x90, 0x3c, 0x64, 0x3e, 0xf8, 0x65, 0xc1, 0x05, 0x07, 0xe0, 0x01, 0x02});
    Check(shorts.size() == 5u && shorts[0] == 0x00643c90u &&
              shorts[1] == 0x00653e90u && shorts[2] == 0x000005c1u &&
              shorts[3] == 0x000007c1u && shorts[4] == 0x000201e0u,
          "stream: packed events, running status, 1-byte programs, realtime skip");
    // Running status survives a buffer boundary.
    shorts.clear();
    feed({0x91, 0x40}); feed({0x5a, 0x41, 0x5b});
    Check(shorts.size() == 2u && shorts[0] == 0x005a4091u &&
              shorts[1] == 0x005b4191u,
          "stream: message and running status span buffers");
    // SysEx split over three buffers keeps order; F7 clears running status.
    shorts.clear();
    feed({0xb0, 0x07, 0x64, 0xf0, 0x7e}); feed({0x7f, 0x09}); feed({0x01, 0xf7, 0x40, 0x01});
    Check(sysex.size() == 1u && sysex[0].size() == 6u &&
              sysex[0].front() == 0xf0u && sysex[0].back() == 0xf7u &&
              shorts.size() == 1u && shorts[0] == 0x006407b0u,
          "stream: fragmented SysEx reassembles; running status cleared by F7");
}

void TestXGSystemExclusiveTranslation() {
    std::vector<uint32_t> messages;
    auto emit = [&](uint32_t message) { messages.push_back(message); };

    const uint8_t masterTune[] = {
        0xf0u, 0x43u, 0x10u, 0x4cu, 0x00u, 0x00u, 0x00u,
        0x00u, 0x04u, 0x00u, 0x00u, 0xf7u};
    Check(svms::TranslateXGSystemExclusive(
              masterTune, static_cast<uint32_t>(sizeof(masterTune)), emit) &&
              messages.size() == 1u && messages[0] ==
                  svms::MakeInternalMasterFineTuneMessage(8192u),
          "XG centered four-nibble master tuning remains unshifted");

    messages.clear();
    const uint8_t transpose[] = {
        0xf0u, 0x43u, 0x10u, 0x4cu, 0x00u, 0x00u, 0x06u,
        0x58u, 0xf7u};
    Check(svms::TranslateXGSystemExclusive(
              transpose, static_cast<uint32_t>(sizeof(transpose)), emit) &&
              messages.size() == 1u && messages[0] ==
                  svms::MakeInternalMasterTransposeMessage(88u),
          "XG master transpose preserves its centered semitone value");

    messages.clear();
    const uint8_t partBankProgram[] = {
        0xf0u, 0x43u, 0x10u, 0x4cu, 0x08u, 0x02u, 0x01u,
        0x7fu, 0x05u, 0x28u, 0xf7u};
    Check(svms::TranslateXGSystemExclusive(
              partBankProgram,
              static_cast<uint32_t>(sizeof(partBankProgram)), emit) &&
              messages.size() == 3u &&
              messages[0] == svms::MakeMidiControlChange(2u, 0u, 127u) &&
              messages[1] == svms::MakeMidiControlChange(2u, 32u, 5u) &&
              messages[2] == svms::MakeMidiProgramChange(2u, 40u),
          "XG bulk part bank/program bytes retain parameter order");

    messages.clear();
    const uint8_t partMode[] = {
        0xf0u, 0x43u, 0x10u, 0x4cu, 0x08u, 0x04u, 0x07u,
        0x02u, 0xf7u};
    Check(svms::TranslateXGSystemExclusive(
              partMode, static_cast<uint32_t>(sizeof(partMode)), emit) &&
              messages.size() == 1u && messages[0] ==
                  svms::MakeInternalRhythmPartMessage(4u, 2u),
          "XG part mode maps percussion state onto the exact-frame command");

    messages.clear();
    const uint8_t noteShift[] = {
        0xf0u, 0x43u, 0x10u, 0x4cu, 0x08u, 0x03u, 0x08u,
        0x46u, 0xf7u};
    Check(svms::TranslateXGSystemExclusive(
              noteShift, static_cast<uint32_t>(sizeof(noteShift)), emit) &&
              messages.size() == 5u &&
              messages[0] == svms::MakeMidiControlChange(3u, 101u, 0u) &&
              messages[1] == svms::MakeMidiControlChange(3u, 100u, 2u) &&
              messages[2] == svms::MakeMidiControlChange(3u, 6u, 70u) &&
              messages[3] == svms::MakeMidiControlChange(3u, 101u, 127u) &&
              messages[4] == svms::MakeMidiControlChange(3u, 100u, 127u),
          "XG note shift translates to a bounded channel RPN transaction");

    svms::TimestampedMidiEvent timed{};
    timed.qpcTimestamp = svms::kAbsoluteFrameTimestampTag | 91u;
    timed.sequence = 123u;
    timed.message = svms::MakeInternalMasterTransposeMessage(52u);
    svms::ScheduledRenderEvent scheduled{};
    Check(svms::CompileTimestampedEvent(
              timed, 0u, 10'000'000u, 44'100u, 0u, scheduled) &&
              scheduled.type == svms::RenderEventType::MasterTranspose &&
              scheduled.data1 == 52u && scheduled.targetFrame == 91,
          "internal XG tuning commands retain exact output-frame placement");
}
