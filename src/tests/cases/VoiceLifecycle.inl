// Test cases split out of V3Tests.cpp; included inside its anonymous namespace.

struct ReleaseDispatchContext {
    svms::VoiceManager* voices;
    svms::VoiceHandle first;
    svms::VoiceHandle second;
};

void DispatchReleaseForTest(const svms::RenderEvent&, uint32_t blockCursor,
                            void* userData) {
    auto* context = static_cast<ReleaseDispatchContext*>(userData);
    context->voices->v.releaseStartInBlock[context->first] = blockCursor;
    context->voices->v.releaseStartInBlock[context->second] = blockCursor;
    context->voices->StartRelease(context->first);
    context->voices->StartRelease(context->second);
}

struct BatchDispatchContext {
    uint32_t calls = 0;
    uint32_t counts[4]{};
    uint32_t cursors[4]{};
    uint32_t sequences[8]{};
    uint32_t sequenceCount = 0;
};

void DispatchBatchForTest(const svms::RenderEvent* events, uint32_t eventCount,
                          uint32_t blockCursor, void* userData) {
    auto* context = static_cast<BatchDispatchContext*>(userData);
    context->counts[context->calls] = eventCount;
    context->cursors[context->calls] = blockCursor;
    ++context->calls;
    for (uint32_t i = 0; i < eventCount; ++i)
        context->sequences[context->sequenceCount++] = events[i].ingressSequence;
}

void TestExactFrameBatchDispatch() {
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(8, 44100);
    svms::ChannelCache channels;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    const svms::RenderEvent events[] = {
        {svms::RenderEventType::NoteOn, 0, 60, 100, 1, 10},
        {svms::RenderEventType::ControlChange, 0, 11, 90, 1, 11},
        {svms::RenderEventType::NoteOn, 0, 61, 100, 1, 12},
        {svms::RenderEventType::NoteOff, 0, 60, 0, 3, 13},
    };
    int16_t samples[2]{0, 0};
    float left[4]{}, right[4]{};
    BatchDispatchContext context{};
    svms::RenderScalar renderer;
    renderer.SetEventBatchDispatcher(DispatchBatchForTest, &context);
    renderer.RenderBlock(*voices, channels, samples, 2, left, right, 4, cfg,
                         events, 4, true, 100);

    Check(context.calls == 2 && context.counts[0] == 3 &&
              context.counts[1] == 1 && context.cursors[0] == 1 &&
              context.cursors[1] == 3,
          "equal-frame events are delivered through one exact-frame batch");
    Check(context.sequenceCount == 4 && context.sequences[0] == 10 &&
              context.sequences[1] == 11 && context.sequences[2] == 12 &&
              context.sequences[3] == 13,
          "batch dispatch preserves global ingress ordering exactly");
}

void TestExactReleaseDurationAcrossBlocks() {
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(2, 1000);
    const svms::VoiceHandle loud = voices->AllocateVoice(0, 60, 127);
    const svms::VoiceHandle quiet = voices->AllocateVoice(0, 64, 20);
    const float releaseDecay = std::exp(-9.226f / 20.0f);
    for (const svms::VoiceHandle voice : {loud, quiet}) {
        voices->SetVoiceSample(voice, 0, 16, 0, 16, 1, 1.0f, 1);
        voices->SetVoiceGain(voice, 1.0f, 1.0f);
    }
    voices->SetVoiceEnvelope(loud, 1.0f, 1.0f, 0, 0, 0, 0,
                             0.0f, 1.0f, releaseDecay, 20);
    voices->SetVoiceEnvelope(quiet, 0.01f, 1.0f, 0, 0, 0, 0,
                             0.0f, 1.0f, releaseDecay, 20);

    svms::ChannelCache channels;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 1000.0f);
    int16_t samples[18]{};
    std::fill(std::begin(samples), std::begin(samples) + 16,
              static_cast<int16_t>(32767));
    svms::RenderScalar renderer;
    ReleaseDispatchContext context{voices.get(), loud, quiet};
    renderer.SetEventDispatcher(DispatchReleaseForTest, &context);
    const svms::RenderEvent releaseEvent{
        svms::RenderEventType::NoteOff, 0, 60, 0, 5, 1};

    float leftA[8]{}, rightA[8]{};
    renderer.RenderBlock(*voices, channels, samples, 16, leftA, rightA, 8, cfg,
                         &releaseEvent, 1, true, 0);
    Check(voices->v.releaseSamplesRemaining[loud] == 17 &&
              voices->v.releaseSamplesRemaining[quiet] == 17,
          "mid-block release consumes exactly the frames after its event");

    float leftB[16]{}, rightB[16]{};
    renderer.RenderBlock(*voices, channels, samples, 16, leftB, rightB, 16, cfg,
                         nullptr, 0, true, 8);
    Check(voices->IsActive(loud) && voices->IsActive(quiet) &&
              voices->v.releaseSamplesRemaining[loud] == 1 &&
              voices->v.releaseSamplesRemaining[quiet] == 1,
          "release countdown advances continuously across callback boundaries");

    float leftC[1]{}, rightC[1]{};
    renderer.RenderBlock(*voices, channels, samples, 16, leftC, rightC, 1, cfg,
                         nullptr, 0, true, 24);
    Check(voices->GetActiveCount() == 0,
          "release finishes on its parsed frame independent of starting gain");
}

void TestReleaseGeneratorMerging() {
    auto data = std::make_unique<svms::SF2Data>();
    std::memset(data.get(), 0, sizeof(svms::SF2Data));
    data->presetCount = 1;
    data->instrumentCount = 1;
    data->sampleCount = 1;
    data->sampleDataFrames = 256;
    data->presets[0].zoneIndex = 0;
    data->instruments[0].zoneIndex = 0;
    data->samples[0].start = 0;
    data->samples[0].end = 255;
    data->samples[0].sampleRate = 44100;
    data->samples[0].originalPitch = 60;

    data->presetZones[0].generatorIndex = 0;
    data->presetZones[1].generatorIndex = 1;
    data->presetZoneCount = 2;
    data->generators[0] = {svms::Gen_ReleaseVolEnv, 1000};
    data->generators[1] = {svms::Gen_Instrument, 0};
    data->pgenCount = 2;

    data->instrumentZones[0].generatorIndex = 2;
    data->instrumentZones[1].generatorIndex = 3;
    data->instrumentZoneCount = 2;
    data->generators[2] = {svms::Gen_ReleaseVolEnv, 200};
    data->generators[3] = {svms::Gen_ReleaseVolEnv,
                           static_cast<uint16_t>(static_cast<int16_t>(-100))};
    data->generators[4] = {svms::Gen_SampleID, 0};
    data->generatorCount = 5;

    svms::sf2_build_regions(data.get());
    Check(data->regionCount == 1 && data->regions[0].releaseVolEnv == 1100,
          "releaseVolEnv is parsed and added across preset/instrument zones");
}

void TestVoiceIdentityAndStealing() {
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(2);

    svms::VoiceHandle first = voices->AllocateVoice(0, 60, 100);
    svms::VoiceHandle second = voices->AllocateVoice(0, 64, 100);
    voices->SetVoiceSoundFontIdentity(first, 3, 7);
    voices->SetVoiceSoundFontIdentity(second, 4, 8);

    Check(voices->v.presetIndex[first] == 3 && voices->v.regionIndex[first] == 7,
          "voice stores its preset and region identity");
    voices->StartRelease(first);
    Check(voices->v.presetIndex[first] == 3 && voices->v.regionIndex[first] == 7,
          "release does not change voice SoundFont identity");

    bool stolen = false;
    svms::VoiceHandle replacement =
        voices->AllocateVoiceOrSteal(0, 67, 100, &stolen);
    Check(replacement != svms::kInvalidVoice && stolen,
          "full pool allocates by stealing a voice");
    if (replacement != svms::kInvalidVoice) {
        voices->SetVoiceSoundFontIdentity(replacement, 9, 11);
    }
    Check(replacement != svms::kInvalidVoice &&
              voices->v.presetIndex[replacement] == 9 &&
              voices->v.regionIndex[replacement] == 11,
          "stolen slot receives the replacement voice identity");
}

void TestCapacitySizedVoiceStorage() {
    auto voices = std::make_unique<svms::VoiceManager>();
    Check(voices->Initialize(1000u, 44100u),
          "capacity-sized voice storage allocates the configured pool");
    Check(voices->v.GetCapacity() == 1000u &&
              voices->GetMaxVoices() == 1000u,
          "voice storage capacity matches max_voices");
    Check(voices->GetStealTreeLeafBaseForTest() == 1024u,
          "steal tournament uses the next power of two above capacity");

    const auto aligned64 = [](const void* pointer) {
        return (reinterpret_cast<uintptr_t>(pointer) & 63u) == 0u;
    };
    Check(aligned64(voices->v.rows) && aligned64(voices->v.phases) &&
              aligned64(voices->v.currentGain) &&
              aligned64(voices->v.sampleStart) &&
              aligned64(voices->v.birthFrame),
          "render-hot voice arrays retain 64-byte alignment");

    const size_t bytesAt1000 = voices->GetAllocatedBytes();
    const svms::VoiceHandle handle = voices->AllocateVoice(3u, 67u, 111u);
    Check(handle != svms::kInvalidVoice,
          "capacity-sized storage supports voice allocation");
    if (handle != svms::kInvalidVoice) {
        voices->v.phases[handle] = 123.25f;
        voices->v.currentGain[handle] = 0.625f;
        voices->v.birthFrame[handle] = 987654321ull;
    }

    auto copied = std::make_unique<svms::VoiceManager>(*voices);
    Check(copied->v.phases != voices->v.phases &&
              copied->v.currentGain != voices->v.currentGain &&
              copied->v.GetCapacity() == voices->v.GetCapacity(),
          "VoiceManager copies own independent voice storage");
    if (handle != svms::kInvalidVoice) {
        Check(copied->v.phases[handle] == 123.25f &&
                  copied->v.currentGain[handle] == 0.625f &&
                  copied->v.birthFrame[handle] == 987654321ull,
              "VoiceManager deep copy preserves voice state");
        voices->v.phases[handle] = 7.0f;
        Check(copied->v.phases[handle] == 123.25f,
              "VoiceManager deep copy is isolated from source writes");
    }

    Check(voices->Initialize(svms::kMaxPolyphony, 48000u),
          "voice storage can be reinitialized at maximum capacity");
    Check(voices->v.GetCapacity() == svms::kMaxPolyphony &&
              voices->GetAllocatedBytes() > bytesAt1000 &&
              voices->GetActiveCount() == 0u,
          "voice reinitialization grows storage and resets lifecycle state");
    Check(voices->GetStealTreeLeafBaseForTest() == svms::kMaxPolyphony,
          "maximum voice pool retains the complete steal tournament");
}

void TestCapacitySizedRendererScratch() {
    auto renderer = std::make_unique<svms::RenderScalar>();
    Check(renderer->GetScratchCapacity() == svms::kMaxVoicesDefault,
          "renderer scratch defaults to configured default polyphony");
    const size_t defaultBytes = renderer->GetAllocatedBytes();
    Check(renderer->ReserveVoiceCapacity(svms::kMaxPolyphony) &&
              renderer->GetScratchCapacity() == svms::kMaxPolyphony &&
              renderer->GetAllocatedBytes() > defaultBytes,
          "renderer scratch grows explicitly with voice capacity");
    Check(renderer->ReserveVoiceCapacity(1000u) &&
              renderer->GetScratchCapacity() == svms::kMaxPolyphony,
          "renderer scratch never reallocates or shrinks in the callback path");
}

void TestPriorityAwareStealingAndFadeTail() {
    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(2, 44100);
        const svms::VoiceHandle quiet = voices->AllocateVoice(0, 60, 127);
        const svms::VoiceHandle loud = voices->AllocateVoice(0, 64, 1);
        voices->SetVoiceEnvelope(quiet, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->SetVoiceEnvelope(loud, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->v.mixGainL[quiet] = voices->v.mixGainR[quiet] = 0.05f;
        voices->v.mixGainL[loud] = voices->v.mixGainR[loud] = 1.0f;

        bool stolen = false;
        const svms::VoiceHandle replacement =
            voices->AllocateVoiceOrSteal(0, 67, 100, &stolen);
        Check(stolen && replacement == quiet,
              "BASS-like stealing chooses effective quietness over velocity");
        Check(voices->v.velocity[loud] == 1,
              "a low-velocity but louder voice survives pool stealing");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(3, 44100);
        const svms::VoiceHandle left = voices->AllocateVoice(0, 60, 100);
        const svms::VoiceHandle right = voices->AllocateVoice(0, 60, 100);
        const svms::VoiceHandle mono = voices->AllocateVoice(0, 64, 100);
        for (const svms::VoiceHandle handle : {left, right, mono}) {
            voices->SetVoiceSample(handle, 0, 128, 8, 120, 1, 1.0f, 1);
            voices->SetVoiceEnvelope(handle, 1.0f, 1.0f, 0, 0, 0, 0,
                                     0.0f, 1.0f, 0.999f);
        }
        voices->SetVoicePlayIndex(left, 10u);
        voices->SetVoicePlayIndex(right, 10u);
        voices->SetVoicePlayIndex(mono, 11u);
        voices->v.mixGainL[left] = 0.1f;
        voices->v.mixGainR[left] = 0.0f;
        voices->v.mixGainL[right] = 0.0f;
        voices->v.mixGainR[right] = 0.1f;
        voices->v.mixGainL[mono] = voices->v.mixGainR[mono] = 1.0f;

        bool stolen = false;
        const svms::VoiceHandle replacement =
            voices->AllocateVoiceOrSteal(0, 67, 100, &stolen);
        const svms::VoiceHandle sibling = replacement == left ? right : left;
        Check(stolen && !voices->IsActive(sibling) && voices->IsActive(mono),
              "stealing one stereo region retires its complete playIndex group");
        Check(voices->GetActiveCount() == 2u && voices->freeTop_ == 1u &&
                  voices->stealCount_ == 2u,
              "group stealing counts and frees both physical stereo voices");
        const svms::VoiceHandle secondLayer = voices->AllocateVoice(0, 67, 100);
        Check(secondLayer == sibling && voices->GetActiveCount() == 3u,
              "the following replacement layer reuses the freed stereo sibling");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(2, 44100);
        const svms::VoiceHandle quiet = voices->AllocateVoice(0, 60, 100);
        const svms::VoiceHandle loud = voices->AllocateVoice(0, 64, 100);
        voices->SetVoicePlayIndex(quiet, 20u);
        voices->SetVoicePlayIndex(loud, 21u);
        voices->SetVoiceEnvelope(quiet, 0.1f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->SetVoiceEnvelope(loud, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->v.mixGainL[quiet] = voices->v.mixGainR[quiet] = 0.1f;
        voices->v.mixGainL[loud] = voices->v.mixGainR[loud] = 1.0f;
        voices->AllocateVoiceOrSteal(0, 67, 100);
        Check(voices->GetActiveCount() == 2u && voices->freeTop_ == 0u &&
                  voices->stealCount_ == 1u,
              "a mono playIndex still steals exactly one physical voice");
    }

    {
        constexpr uint32_t kVoiceCount = 64u;
        constexpr uint32_t kIterations = 256u;
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(kVoiceCount, 44100);
        svms::ChannelParamsSnapshot channel{};
        channel.volume = channel.expression = 1.0f;
        channel.panLeft = channel.panRight = 0.70710678f;
        channel.mixScaleLeft = channel.mixScaleRight = 0.70710678f;
        svms::VoiceConfiguration setup{};
        setup.sampleStart = 0u;
        setup.sampleEnd = 128u;
        setup.loopStart = 8u;
        setup.loopEnd = 120u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep = 1.0f;
        setup.initialGain = setup.sustainLevel = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.1f;
        setup.sampleBacked = 1u;

        for (uint32_t group = 0; group < kVoiceCount / 2u; ++group) {
            setup.playIndex = group + 1u;
            for (uint32_t layer = 0; layer < 2u; ++layer) {
                const svms::VoiceHandle voice =
                    voices->AllocateVoice(0, 60, 100);
                voices->ConfigureVoice(voice, setup, channel, false);
            }
        }
        for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
            const svms::VoiceHandle first =
                voices->AllocateVoiceOrSteal(0, 60, 100);
            const svms::VoiceHandle second =
                voices->AllocateVoiceOrSteal(0, 60, 100);
            setup.playIndex = 1000u + iteration;
            voices->ConfigureVoice(first, setup, channel, false);
            voices->ConfigureVoice(second, setup, channel, false);
        }
        Check(voices->GetStealHeapBuildCountForTest() == 1u,
              "dense stereo group stealing retains its incremental heap");
        Check(voices->GetActiveCount() == kVoiceCount &&
                  voices->freeTop_ == 0u,
              "dense stereo group replacement keeps the pool full");
    }

    {
        // A layered transactional launch starting from singleton victims must
        // reserve two distinct slots. Keeping the first winner-tree leaf live
        // allowed the second layer to steal the same deferred handle again,
        // overwriting one stereo side and collapsing its play group.
        auto voices = std::make_unique<svms::VoiceManager>();
        auto oracle = std::make_unique<svms::VoiceManager>();
        voices->Initialize(4u, 44100u);
        oracle->Initialize(4u, 44100u);
        svms::ChannelParamsSnapshot channel{};
        channel.volume = channel.expression = 1.0f;
        channel.panLeft = channel.panRight = 0.70710678f;
        channel.mixScaleLeft = channel.mixScaleRight = 0.70710678f;
        svms::VoiceConfiguration mono{};
        mono.sampleStart = 0u;
        mono.sampleEnd = 128u;
        mono.loopStart = 8u;
        mono.loopEnd = 120u;
        mono.loopMode = 1u;
        mono.phaseStep = mono.basePhaseStep = 1.0f;
        mono.initialGain = mono.sustainLevel = 1.0f;
        mono.gainLeft = mono.gainRight = 0.1f;
        for (uint32_t i = 0u; i < 4u; ++i) {
            mono.playIndex = i + 1u;
            const auto handle = voices->AllocateVoice(0u, 60u, 100u);
            voices->ConfigureVoice(handle, mono, channel, false);
            const auto oracleHandle = oracle->AllocateVoice(0u, 60u, 100u);
            oracle->ConfigureVoice(oracleHandle, mono, channel, false);
        }
        svms::VoiceConfiguration stereo[2] = {mono, mono};
        stereo[0].playIndex = stereo[1].playIndex = 100u;
        svms::VoiceHandle handles[2]{};
        Check(voices->LaunchVoiceGroup(0u, 67u, 127u, stereo, 2u,
                                      channel, handles),
              "layered launch succeeds from singleton victims");
        svms::VoiceHandle oracleHandles[2]{};
        for (uint32_t layer = 0u; layer < 2u; ++layer) {
            oracleHandles[layer] = oracle->AllocateVoiceOrSteal(
                0u, 67u, 127u, nullptr, true, false);
        }
        for (uint32_t layer = 0u; layer < 2u; ++layer) {
            oracle->ConfigureVoice(oracleHandles[layer], stereo[layer],
                                   channel, true);
        }
        Check(handles[0] != handles[1] &&
                  voices->GetPlayGroupSizeForTest(handles[0]) == 2u &&
                  voices->GetPlayGroupSizeForTest(handles[1]) == 2u,
              "layered fallback reserves distinct slots and links both sides");
        bool matchesOracle = handles[0] == oracleHandles[0] &&
            handles[1] == oracleHandles[1] &&
            voices->FindStealVictimExhaustiveForTest() ==
                oracle->FindStealVictimExhaustiveForTest();
        for (uint32_t handle = 0u; handle < 4u && matchesOracle; ++handle) {
            matchesOracle = voices->v.state[handle] == oracle->v.state[handle] &&
                voices->v.playIndex[handle] == oracle->v.playIndex[handle] &&
                voices->v.note[handle] == oracle->v.note[handle] &&
                voices->v.renderClass[handle] == oracle->v.renderClass[handle];
        }
        Check(matchesOracle,
              "layered fallback matches explicit distinct-slot steal oracle");
    }

    {
        auto legacy = std::make_unique<svms::VoiceManager>();
        auto transactional = std::make_unique<svms::VoiceManager>();
        legacy->Initialize(8u, 44100u);
        transactional->Initialize(8u, 44100u);
        svms::ChannelParamsSnapshot channel{};
        channel.volume = channel.expression = 1.0f;
        channel.panLeft = channel.panRight = 0.70710678f;
        channel.mixScaleLeft = channel.mixScaleRight = 0.70710678f;
        svms::VoiceConfiguration setup{};
        setup.sampleStart = 0u;
        setup.sampleEnd = 128u;
        setup.loopStart = 8u;
        setup.loopEnd = 120u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep = 1.0f;
        setup.initialGain = setup.sustainLevel = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.1f;
        setup.sampleBacked = 1u;
        for (uint32_t i = 0u; i < 8u; ++i) {
            setup.playIndex = i + 1u;
            const auto first = legacy->AllocateVoice(0u, 60u, 100u);
            const auto second = transactional->AllocateVoice(0u, 60u, 100u);
            legacy->ConfigureVoice(first, setup, channel, false);
            transactional->ConfigureVoice(second, setup, channel, false);
        }
        bool launchOk = true;
        bool sameVictims = true;
        for (uint32_t iteration = 0u; iteration < 512u; ++iteration) {
            const uint8_t note = static_cast<uint8_t>(36u + iteration % 72u);
            const uint8_t velocity = static_cast<uint8_t>(64u + iteration % 64u);
            const uint32_t playIndex = 100u + iteration;
            setup.initialGain = setup.sustainLevel =
                0.25f + static_cast<float>(iteration % 17u) * 0.03125f;
            svms::VoiceConfiguration copiedSetup = setup;
            copiedSetup.playIndex = playIndex;
            const auto replacement = legacy->AllocateVoiceOrSteal(
                0u, note, velocity, nullptr, true);
            legacy->ConfigureVoice(replacement, copiedSetup, channel, true);
            svms::VoiceHandle launched = svms::kInvalidVoice;
            launchOk = transactional->LaunchVoiceGroup(
                0u, note, velocity, &setup, 1u, playIndex, channel, &launched);
            sameVictims = sameVictims && launchOk && launched == replacement &&
                legacy->FindStealVictimExhaustiveForTest() ==
                    transactional->FindStealVictimExhaustiveForTest();
            if (!launchOk || !sameVictims) break;
        }
        Check(launchOk && sameVictims,
              "transactional mono replacement preserves exact victim sequence");
        bool sameState = launchOk && sameVictims &&
            legacy->GetActiveCount() == transactional->GetActiveCount() &&
            legacy->GetStealTailCount() == transactional->GetStealTailCount();
        for (uint32_t i = 0u; sameState && i < 8u; ++i) {
            sameState = legacy->v.state[i] == transactional->v.state[i] &&
                legacy->v.playIndex[i] == transactional->v.playIndex[i] &&
                legacy->v.envelopeStage[i] ==
                    transactional->v.envelopeStage[i] &&
                std::fabs(legacy->v.phases[i] -
                          transactional->v.phases[i]) < 1.0e-7f &&
                std::fabs(legacy->v.currentGain[i] -
                          transactional->v.currentGain[i]) < 1.0e-7f;
        }
        Check(sameState,
              "repeated transactional mono replacement preserves voice state");
    }

    {
        // Exercise the saturated common stereo path repeatedly. The in-place
        // transaction must remain equivalent to selecting the exhaustive
        // victim, retiring its complete play group, and configuring the two
        // replacement slots sequentially.
        auto legacy = std::make_unique<svms::VoiceManager>();
        auto transactional = std::make_unique<svms::VoiceManager>();
        legacy->Initialize(8u, 44100u);
        transactional->Initialize(8u, 44100u);
        svms::ChannelParamsSnapshot channel{};
        channel.volume = channel.expression = 1.0f;
        channel.panLeft = channel.panRight = 0.70710678f;
        channel.mixScaleLeft = channel.mixScaleRight = 0.70710678f;
        svms::VoiceConfiguration setups[2]{};
        for (auto& setup : setups) {
            setup.sampleStart = 0u;
            setup.sampleEnd = 128u;
            setup.loopStart = 8u;
            setup.loopEnd = 120u;
            setup.loopMode = 1u;
            setup.phaseStep = setup.basePhaseStep = 1.0f;
            setup.initialGain = setup.sustainLevel = 1.0f;
            setup.releaseDecay = 0.999f;
            setup.gainLeft = setup.gainRight = 0.1f;
            setup.sampleBacked = 1u;
        }
        for (uint32_t group = 0u; group < 4u; ++group) {
            setups[0].playIndex = setups[1].playIndex = group + 1u;
            for (uint32_t layer = 0u; layer < 2u; ++layer) {
                const auto a = legacy->AllocateVoice(0u, 60u, 100u);
                const auto b = transactional->AllocateVoice(0u, 60u, 100u);
                legacy->ConfigureVoice(a, setups[layer], channel, false);
                transactional->ConfigureVoice(b, setups[layer], channel, false);
            }
        }
        Check(transactional->GetPlayGroupSizeForTest(6u) == 2u &&
                  transactional->GetPlayGroupSizeForTest(7u) == 2u,
              "transactional fixture starts with intact stereo play groups");

        bool equivalent = true;
        for (uint32_t iteration = 0u; iteration < 64u && equivalent;
             ++iteration) {
            const uint8_t note = static_cast<uint8_t>(36u + iteration % 72u);
            const uint32_t playIndex = 100u + iteration;
            setups[0].playIndex = setups[1].playIndex = playIndex;
            setups[0].phaseStep = setups[0].basePhaseStep =
                0.75f + static_cast<float>(iteration & 3u) * 0.125f;
            setups[1].phaseStep = setups[1].basePhaseStep =
                setups[0].phaseStep + 0.03125f;

            const auto expected = legacy->FindStealVictimExhaustiveForTest();
            svms::VoiceHandle legacyHandles[2]{};
            legacyHandles[0] = legacy->AllocateVoiceOrSteal(
                0u, note, 127u, nullptr, true);
            legacyHandles[1] = legacy->AllocateVoiceOrSteal(
                0u, note, 127u, nullptr, true);
            for (uint32_t layer = 0u; layer < 2u; ++layer)
                legacy->ConfigureVoice(legacyHandles[layer], setups[layer],
                                       channel, true);

            svms::VoiceHandle transactionHandles[2]{};
            const bool launched = transactional->LaunchVoiceGroup(
                0u, note, 127u, setups, 2u, channel, transactionHandles);
            if (launched) {
                Check(transactional->GetPlayGroupSizeForTest(
                          transactionHandles[0]) == 2u &&
                          transactional->GetPlayGroupSizeForTest(
                          transactionHandles[1]) == 2u,
                      "transactional launch keeps replacement stereo group linked");
            }
            equivalent = launched && legacyHandles[0] == expected &&
                transactionHandles[0] == expected &&
                legacy->GetActiveCount() == transactional->GetActiveCount() &&
                legacy->GetStealTailCount() ==
                    transactional->GetStealTailCount() &&
                legacy->FindStealVictimExhaustiveForTest() ==
                    transactional->FindStealVictimExhaustiveForTest();
            for (uint32_t handle = 0u; handle < 8u && equivalent; ++handle) {
                equivalent = legacy->v.state[handle] ==
                        transactional->v.state[handle] &&
                    legacy->v.channel[handle] ==
                        transactional->v.channel[handle] &&
                    legacy->v.note[handle] == transactional->v.note[handle] &&
                    legacy->v.playIndex[handle] ==
                        transactional->v.playIndex[handle] &&
                    legacy->v.renderClass[handle] ==
                        transactional->v.renderClass[handle] &&
                    std::fabs(legacy->v.phaseIncs[handle] -
                              transactional->v.phaseIncs[handle]) < 1.0e-7f;
            }
        }
        Check(equivalent,
              "in-place stereo transactions preserve exhaustive victims and voice state");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(2, 44100);
        const svms::VoiceHandle mature = voices->AllocateVoice(0, 60, 100);
        voices->SetVoiceEnvelope(mature, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->v.mixGainL[mature] = voices->v.mixGainR[mature] = 1.0f;
        voices->SetCurrentFrame(1000);
        const svms::VoiceHandle attacking = voices->AllocateVoice(0, 61, 40);
        voices->SetVoiceEnvelope(attacking, 1.0f, 1.0f, 0, 0, 256, 0,
                                 1.0f / 256.0f, 1.0f, 0.999f);
        voices->v.mixGainL[attacking] = voices->v.mixGainR[attacking] = 1.0f;

        bool stolen = false;
        const svms::VoiceHandle replacement =
            voices->AllocateVoiceOrSteal(0, 62, 80, &stolen);
        Check(stolen && replacement == mature,
              "new attack is protected from stealing when a mature voice exists");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(1, 44100);
        const svms::VoiceHandle victim = voices->AllocateVoice(0, 60, 80);
        voices->SetVoiceSample(victim, 0, 64, 0, 64, 1, 1.0f, 1);
        voices->SetVoiceEnvelope(victim, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->v.mixGainL[victim] = voices->v.mixGainR[victim] = 1.0f;

        bool stolen = false;
        const svms::VoiceHandle replacement =
            voices->AllocateVoiceOrSteal(0, 67, 100, &stolen);
        const uint32_t tailSlot = voices->GetStealTailList()[0];
        const uint32_t fadeFrames = voices->v.stealTailFramesTotal[tailSlot];
        Check(stolen && fadeFrames == svms::kStealFadeFrames,
              "stealing captures the fixed 64-frame outgoing tail");
        Check(voices->v.stealFadeInFramesTotal[replacement] == 0,
              "replacement attack is not blurred by the outgoing tail fade");

        int16_t samples[72]{};
        std::fill(std::begin(samples), std::begin(samples) + 64,
                  static_cast<int16_t>(32767));
        std::vector<float> left(fadeFrames, 0.0f);
        std::vector<float> right(fadeFrames, 0.0f);
        svms::ChannelCache channels;
        svms::RuntimeConfigSnapshot cfg{};
        cfg.masterVolume = 1.0f;
        cfg.panLaw = svms::PanLaw::ConstantPower;
        channels.RebuildCache(cfg, 44100.0f);
        svms::RenderScalar renderer;
        renderer.RenderBlock(*voices, channels, samples, 64, left.data(), right.data(),
                             fadeFrames, cfg, nullptr, 0, true, 0);

        Check(left.front() > 0.99f,
              "stolen tail begins continuously at the victim's current amplitude");
        Check(left[fadeFrames / 2] > 0.45f && left[fadeFrames / 2] < 0.55f,
              "stolen tail ramps linearly instead of being hard-cut");
        Check(std::fabs(left.back()) <= 1.0e-7f,
              "stolen tail reaches silence at the end of its fade");
        Check(voices->v.stealTailFramesRemaining[tailSlot] == 0,
              "stolen tail retires without consuming a voice slot");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(64, 44100);
        for (uint32_t i = 0; i < 64u; ++i) {
            const svms::VoiceHandle handle = voices->AllocateVoice(
                0, static_cast<uint8_t>(i + 24u), 100);
            voices->SetVoiceSample(handle, 0, 256, 8, 248, 1, 1.0f, 1);
            voices->SetVoiceEnvelope(handle, 1.0f, 1.0f, 0, 0, 0, 0,
                                     0.0f, 1.0f, 0.999f);
            voices->v.mixGainL[handle] = voices->v.mixGainR[handle] = 1.0f;
        }
        for (uint32_t i = 0; i < 60u; ++i) {
            const svms::VoiceHandle replacement = voices->AllocateVoiceOrSteal(
                0, static_cast<uint8_t>(36u + i % 72u), 100);
            voices->SetVoiceSample(replacement, 0, 256, 8, 248, 1, 1.0f, 1);
            voices->SetVoiceEnvelope(replacement, 1.0f, 1.0f, 0, 0, 0, 0,
                                     0.0f, 1.0f, 0.999f);
            voices->v.mixGainL[replacement] =
                voices->v.mixGainR[replacement] = 1.0f;
        }
        Check(voices->GetStealTailCount() == svms::kStealTailReserve,
              "BASS-like outgoing tail reserve is capped at 50 voices");

        const svms::VoiceHandle quietVictim =
            static_cast<svms::VoiceHandle>(voices->activeList_[0]);
        voices->SetVoiceEnvelope(quietVictim, 0.1f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->InvalidateStealCandidates();
        voices->AllocateVoiceOrSteal(0, 96, 100);
        bool retainedOnlyLouderTails = true;
        for (uint32_t i = 0; i < voices->GetStealTailCount(); ++i) {
            const uint32_t slot = voices->GetStealTailList()[i];
            retainedOnlyLouderTails &= voices->v.stealTailGain[slot] > 0.9f;
        }
        Check(retainedOnlyLouderTails,
              "a quieter outgoing victim cannot evict a louder reserve tail");

        for (uint32_t i = 0; i < voices->activeCount_; ++i) {
            const svms::VoiceHandle handle = static_cast<svms::VoiceHandle>(
                voices->activeList_[i]);
            voices->SetVoiceSample(handle, 0, 256, 8, 248, 1, 1.0f, 1);
            voices->SetVoiceEnvelope(handle, i == 0u ? 2.0f : 3.0f, 1.0f,
                                     0, 0, 0, 0, 0.0f, 1.0f, 0.999f);
            voices->v.mixGainL[handle] = voices->v.mixGainR[handle] = 1.0f;
        }
        voices->InvalidateStealCandidates();
        voices->AllocateVoiceOrSteal(0, 97, 100);
        bool admittedLouderTail = false;
        for (uint32_t i = 0; i < voices->GetStealTailCount(); ++i) {
            const uint32_t slot = voices->GetStealTailList()[i];
            admittedLouderTail |= voices->v.stealTailGain[slot] > 1.5f;
        }
        Check(admittedLouderTail,
              "a louder outgoing victim replaces the quietest reserve tail");

        int16_t tailSamples[264]{};
        std::fill(std::begin(tailSamples), std::begin(tailSamples) + 256,
                  static_cast<int16_t>(8192));
        float tailLeft[8]{};
        float tailRight[8]{};
        svms::ChannelCache tailChannels;
        svms::RuntimeConfigSnapshot tailConfig{};
        tailConfig.masterVolume = 1.0f;
        tailChannels.RebuildCache(tailConfig, 44100.0f);
        svms::RenderScalar tailRenderer;
        tailRenderer.RenderBlock(*voices, tailChannels, tailSamples, 256u,
                                 tailLeft, tailRight, 8u, tailConfig,
                                 nullptr, 0u, true, 0u);
        Check(voices->GetStealTailCount() <= svms::kStealTailReserve,
              "dense fixed tail storage renders without exceeding its reserve");
    }
}

void TestExactStealHeapAndVoiceIndices() {
    svms::ChannelCache channels;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    channels.RebuildCache(cfg, 44100.0f);

    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(4, 44100);
    svms::VoiceHandle handles[4]{};
    const uint8_t velocities[4] = {10, 20, 30, 40};
    for (uint32_t i = 0; i < 4; ++i) {
        handles[i] = voices->AllocateVoice(
            static_cast<uint8_t>(i & 1u), static_cast<uint8_t>(60u + i),
            velocities[i]);
        voices->SetVoiceSample(handles[i], 0, 128, 8, 120, 1, 1.0f, 1);
        voices->SetVoiceEnvelope(handles[i], 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->SetVoiceGain(handles[i], 1.0f, 1.0f);
        voices->RefreshMixGain(handles[i], channels.GetParams()[i & 1u]);
    }

    Check(voices->GetChannelActiveCount(0) == 2u &&
              voices->GetChannelActiveCount(1) == 2u,
          "per-channel active indices track allocation");
    Check(voices->GetRenderClassCount(svms::VoiceRenderClass::SustainedLoop) == 4u,
          "render-class index tracks configured sustained loops");

    voices->SetCurrentFrame(1024);
    bool stolen = false;
    const svms::VoiceHandle firstVictim =
        voices->AllocateVoiceOrSteal(1, 80, 127, &stolen);
    Check(stolen && firstVictim == handles[0],
          "exact steal heap chooses the same lowest-velocity victim as a scan");
    voices->SetVoiceSample(firstVictim, 0, 128, 8, 120, 1, 1.0f, 1);
    voices->SetVoiceEnvelope(firstVictim, 1.0f, 1.0f, 0, 0, 0, 0,
                             0.0f, 1.0f, 0.999f);
    voices->SetVoiceGain(firstVictim, 1.0f, 1.0f);
    voices->RefreshMixGain(firstVictim, channels.GetParams()[1]);

    const svms::VoiceHandle secondVictim =
        voices->AllocateVoiceOrSteal(1, 81, 127, &stolen);
    Check(stolen && secondVictim == handles[1],
          "same-frame heap pop preserves exact next-victim ordering");
    voices->SetVoiceSample(secondVictim, 0, 128, 8, 120, 1, 1.0f, 1);
    voices->SetVoiceEnvelope(secondVictim, 1.0f, 1.0f, 0, 0, 0, 0,
                             0.0f, 1.0f, 0.999f);
    voices->SetVoiceGain(secondVictim, 1.0f, 1.0f);
    voices->RefreshMixGain(secondVictim, channels.GetParams()[1]);

    voices->v.currentGain[handles[3]] = 0.01f;
    voices->StartRelease(handles[3]);
    const svms::VoiceHandle releasedVictim =
        voices->AllocateVoiceOrSteal(0, 82, 127, &stolen);
    Check(stolen && releasedVictim == handles[3],
          "intervening release updates the exact same-frame steal ranking");

    uint32_t indexedVoices = 0u;
    for (uint32_t channel = 0; channel < svms::kChannelCount; ++channel)
        indexedVoices += voices->GetChannelActiveCount(static_cast<uint8_t>(channel));
    Check(indexedVoices == voices->activeCount_,
          "channel indices remain complete after in-place steals");

    voices->RetireVoice(secondVictim);
    indexedVoices = 0u;
    for (uint32_t channel = 0; channel < svms::kChannelCount; ++channel)
        indexedVoices += voices->GetChannelActiveCount(static_cast<uint8_t>(channel));
    Check(indexedVoices == voices->activeCount_,
          "channel indices remain complete after retirement");
}

void TestPersistentStealIndexAgainstOracle() {
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(128, 44100);
    for (uint32_t i = 0; i < 128u; ++i) {
        voices->SetCurrentFrame(i * 53u);
        const svms::VoiceHandle h = voices->AllocateVoice(
            static_cast<uint8_t>(i & 15u), static_cast<uint8_t>(24u + i % 88u),
            static_cast<uint8_t>(1u + i % 127u));
        voices->SetVoiceSample(h, 0, 512, 8, 504, 1, 0.75f, 1);
        voices->SetVoiceEnvelope(h, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.9999f);
        voices->SetVoiceGain(h, 0.5f, 0.5f);
        voices->RefreshMixGain(h, channels.GetParams()[i & 15u]);
    }

    auto denseVoices = std::make_unique<svms::VoiceManager>(*voices);
#if !defined(SVMS_XP_COMPAT)
    if (svms::IsRenderBackendSupported(svms::RenderBackend::AVX2))
        denseVoices->SetStealKeyBackend(svms::RenderBackend::AVX2);
#endif
    uint64_t frame = 9000u;
    for (uint32_t iteration = 0; iteration < 512u; ++iteration) {
        frame += (iteration == 256u)
            ? 44100ull * 6ull * 60ull * 60ull : 37u;
        voices->SetCurrentFrame(frame);
        if ((iteration % 7u) == 0u) {
            const uint32_t h = voices->activeList_[(iteration * 17u) % voices->activeCount_];
            voices->StartRelease(static_cast<svms::VoiceHandle>(h));
        }
        if ((iteration % 11u) == 0u) {
            const uint8_t channel = static_cast<uint8_t>(iteration & 15u);
            channels.ControlChange(channel, 11u,
                static_cast<uint8_t>(32u + iteration % 96u));
            channels.RebuildCache(cfg, 44100.0f);
            voices->RefreshMixGainsForChannel(channel,
                channels.GetParams()[channel]);
        }
        uint32_t bestPos = 0u;
        const svms::VoiceHandle expected =
            voices->PredictStealVictimForTest(bestPos);
        bool stolen = false;
        const svms::VoiceHandle actual = voices->AllocateVoiceOrSteal(
            static_cast<uint8_t>(iteration & 15u),
            static_cast<uint8_t>(36u + iteration % 72u),
            static_cast<uint8_t>(64u + iteration % 64u), &stolen);
        Check(stolen && actual == expected,
              "persistent steal index matches exhaustive victim selection");
        voices->SetVoiceSample(actual, 0, 512, 8, 504, 1, 0.75f, 1);
        voices->SetVoiceEnvelope(actual, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.9999f);
        voices->SetVoiceGain(actual, 0.5f, 0.5f);
        voices->RefreshMixGain(actual,
            channels.GetParams()[iteration & 15u]);
    }

    // Exercise the release-heavy equal-frame shape used by chopped Black
    // MIDI: many stable voices become volatile, then many exact replacements
    // consume that heap without advancing the output frame.
    //
    // The matching-group reuse route keeps exact reserved-tier semantics
    // (its probe must be able to restore the selected candidate verbatim),
    // so the Releasing-ring fast path is opted out for this segment.
    denseVoices->SetReleasingRingEnabled(false);
    svms::VoiceConfiguration choppedSetup{};
    choppedSetup.sampleEnd = 512u;
    choppedSetup.loopStart = 8u;
    choppedSetup.loopEnd = 504u;
    choppedSetup.loopMode = 1u;
    choppedSetup.initialGain = choppedSetup.sustainLevel = 1.0f;
    choppedSetup.releaseDecay = 0.9999f;
    choppedSetup.gainLeft = choppedSetup.gainRight = 0.5f;
    for (uint32_t denseFrame = 0u; denseFrame < 64u; ++denseFrame) {
        denseVoices->SetCurrentFrame(9000u + denseFrame + 1u);
        uint32_t released = 0u;
        for (uint32_t position = 0u;
             position < denseVoices->activeCount_ && released < 32u;
             ++position) {
            const svms::VoiceHandle handle = static_cast<svms::VoiceHandle>(
                denseVoices->activeList_[(position * 17u + denseFrame) %
                                         denseVoices->activeCount_]);
            if (denseVoices->v.state[handle] !=
                static_cast<uint8_t>(svms::VoiceState::Active)) continue;
            denseVoices->StartRelease(handle);
            ++released;
        }
        for (uint32_t launch = 0u; launch < released; ++launch) {
            uint32_t predictedPos = 0u;
            const svms::VoiceHandle expected =
                denseVoices->PredictStealVictimForTest(predictedPos);
            svms::VoiceHandle actual = svms::kInvalidVoice;
            choppedSetup.playIndex = denseFrame * 32u + launch + 1u;
            const uint8_t channel = static_cast<uint8_t>(launch & 15u);
            const bool launched = denseVoices->LaunchVoiceGroup(
                channel, static_cast<uint8_t>(24u + launch % 96u), 127u,
                &choppedSetup, 1u, choppedSetup.playIndex,
                channels.GetParams()[channel], &actual);
            Check(launched && actual == expected,
                  "release-heavy same-frame heap matches exhaustive victims");
        }
    }

    // Delay/hold/attack use fixed target gain for stealing and therefore live
    // in the persistent exact tree. Verify the tree retains the exhaustive
    // oracle's victim and tie decisions within and across output frames.
    auto attacks = std::make_unique<svms::VoiceManager>();
    attacks->Initialize(64u, 44100u);
    for (uint32_t i = 0; i < 64u; ++i) {
        const svms::VoiceHandle handle = attacks->AllocateVoice(
            static_cast<uint8_t>(i & 15u), static_cast<uint8_t>(36u + i),
            static_cast<uint8_t>(96u + i % 32u));
        svms::VoiceConfiguration setup{};
        setup.sampleEnd = 512u;
        setup.loopStart = 8u;
        setup.loopEnd = 504u;
        setup.loopMode = 1u;
        setup.initialGain = 0.8f;
        setup.attackSamples = 2048u;
        setup.attackGainStep = setup.initialGain / 2048.0f;
        setup.gainLeft = setup.gainRight = 0.5f;
        attacks->ConfigureVoice(handle, setup,
            channels.GetParams()[i & 15u], false);
    }
    uint64_t attackFrame = 12000u;
    for (uint32_t iteration = 0; iteration < 512u; ++iteration) {
        if ((iteration & 31u) == 0u) ++attackFrame;
        attacks->SetCurrentFrame(attackFrame);
        const svms::VoiceHandle expected =
            attacks->FindStealVictimExhaustiveForTest();
        const uint8_t channel = static_cast<uint8_t>(iteration & 15u);
        const svms::VoiceHandle actual = attacks->AllocateVoiceOrSteal(
            channel, static_cast<uint8_t>(24u + iteration % 96u),
            static_cast<uint8_t>(96u + iteration % 32u), nullptr, true);
        Check(actual == expected,
              "persistent attack steal tree matches exhaustive victim selection");
        svms::VoiceConfiguration setup{};
        setup.sampleEnd = 512u;
        setup.loopStart = 8u;
        setup.loopEnd = 504u;
        setup.loopMode = 1u;
        setup.initialGain = 0.8f;
        setup.attackSamples = 2048u;
        setup.attackGainStep = setup.initialGain / 2048.0f;
        setup.gainLeft = setup.gainRight = 0.5f;
        attacks->ConfigureVoice(actual, setup,
            channels.GetParams()[channel], true);
    }

    {
        auto transitions = std::make_unique<svms::VoiceManager>();
        transitions->Initialize(2u, 44100u);
        for (uint32_t i = 0u; i < 2u; ++i) {
            const auto handle = transitions->AllocateVoice(
                0u, static_cast<uint8_t>(60u + i), 100u);
            transitions->SetVoiceSample(handle, 0u, 512u, 8u, 504u,
                                        1u, 0.75f, 1u);
            transitions->SetVoiceEnvelope(handle, 1.0f, 0.4f, 0u, 0u,
                                           8u, 64u, 0.125f, 0.98f,
                                           0.9999f);
            transitions->SetVoiceGain(handle, 0.5f, 0.5f);
            transitions->RefreshMixGain(handle, channels.GetParams()[0]);
        }
        // Materialize the persistent attack index, configure its replacement
        // as another attack, then emulate the exact attack-to-decay boundary
        // without changing render-class membership.
        const auto indexed = transitions->AllocateVoiceOrSteal(
            0u, 71u, 100u, nullptr, true);
        svms::VoiceConfiguration setup{};
        setup.sampleEnd = 512u;
        setup.loopStart = 8u;
        setup.loopEnd = 504u;
        setup.loopMode = 1u;
        setup.initialGain = 1.0f;
        setup.sustainLevel = 0.4f;
        setup.attackSamples = 8u;
        setup.decaySamples = 64u;
        setup.attackGainStep = 0.125f;
        setup.decaySlope = 0.98f;
        setup.gainLeft = setup.gainRight = 0.5f;
        transitions->ConfigureVoice(indexed, setup,
                                    channels.GetParams()[0], true);
        transitions->v.envelopeStage[indexed] = 2u;
        transitions->v.currentGain[indexed] = 0.01f;
        transitions->RefreshRenderClass(indexed);
        const auto expected = transitions->FindStealVictimExhaustiveForTest();
        const auto actual = transitions->AllocateVoiceOrSteal(
            0u, 72u, 127u, nullptr, true);
        Check(actual == expected,
              "attack-to-decay transition migrates to the volatile steal index");
    }
}

void TestPagedChannelIndexFragmentation() {
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(svms::kMaxPolyphony, 44100u);
    std::vector<uint8_t> expectedActive(svms::kMaxPolyphony, 0u);
    std::vector<uint8_t> expectedChannel(svms::kMaxPolyphony, 0u);
    std::vector<svms::VoiceHandle> expectedLists[svms::kChannelCount];
    std::vector<svms::VoiceHandle>
        expectedRender[svms::kVoiceRenderClassCount];
    const uint32_t genericClass =
        static_cast<uint32_t>(svms::VoiceRenderClass::Generic);

    for (uint32_t i = 0u; i < svms::kMaxPolyphony; ++i) {
        const uint8_t channel = static_cast<uint8_t>((i * 7u + i / 97u) & 15u);
        const svms::VoiceHandle handle = voices->AllocateVoice(
            channel, static_cast<uint8_t>(i & 127u), 100u);
        Check(handle != svms::kInvalidVoice,
              "paged channel index fills complete voice capacity");
        if (handle != svms::kInvalidVoice) {
            expectedActive[handle] = 1u;
            expectedChannel[handle] = channel;
            expectedLists[channel].push_back(handle);
            expectedRender[genericClass].push_back(handle);
        }
    }

    for (uint32_t i = 0u; i < svms::kMaxPolyphony; ++i) {
        const uint32_t handle = (i * 4051u) % svms::kMaxPolyphony;
        if ((i % 3u) != 0u) continue;
        const uint8_t channel = expectedChannel[handle];
        auto& expected = expectedLists[channel];
        const auto where = std::find(expected.begin(), expected.end(), handle);
        Check(where != expected.end(),
              "paged channel oracle finds retired handle");
        if (where != expected.end()) {
            *where = expected.back();
            expected.pop_back();
        }
        auto& expectedGeneric = expectedRender[genericClass];
        const auto genericWhere = std::find(
            expectedGeneric.begin(), expectedGeneric.end(), handle);
        Check(genericWhere != expectedGeneric.end(),
              "paged render-class oracle finds retired handle");
        if (genericWhere != expectedGeneric.end()) {
            *genericWhere = expectedGeneric.back();
            expectedGeneric.pop_back();
        }
        voices->RetireVoice(static_cast<svms::VoiceHandle>(handle));
        expectedActive[handle] = 0u;
    }

    for (uint32_t i = 0u; i < 1000u; ++i) {
        const uint8_t channel = static_cast<uint8_t>((i * 11u + 3u) & 15u);
        const svms::VoiceHandle handle = voices->AllocateVoice(
            channel, static_cast<uint8_t>((i * 13u) & 127u), 110u);
        Check(handle != svms::kInvalidVoice,
              "paged channel index reuses fragmented capacity");
        if (handle != svms::kInvalidVoice) {
            expectedActive[handle] = 1u;
            expectedChannel[handle] = channel;
            expectedLists[channel].push_back(handle);
            expectedRender[genericClass].push_back(handle);
        }
    }

    const auto moveExpectedRender = [&](svms::VoiceHandle handle,
                                        svms::VoiceRenderClass from,
                                        svms::VoiceRenderClass to) {
        auto& source = expectedRender[static_cast<uint32_t>(from)];
        const auto where = std::find(source.begin(), source.end(), handle);
        Check(where != source.end(),
              "paged render-class oracle finds transitioned handle");
        if (where != source.end()) {
            *where = source.back();
            source.pop_back();
        }
        expectedRender[static_cast<uint32_t>(to)].push_back(handle);
    };
    for (uint32_t position = 0u; position < voices->GetActiveCount();
         position += 2u) {
        const svms::VoiceHandle handle = static_cast<svms::VoiceHandle>(
            voices->activeList_[position]);
        voices->v.sampleBacked[handle] = 1u;
        voices->v.relEnd[handle] = 256u;
        voices->v.relLoopS[handle] = 16u;
        voices->v.relLoopE[handle] = 240u;
        voices->v.loopEnabled[handle] = 1u;
        voices->v.envelopeStage[handle] = 3u;
        voices->v.stealFadeInFramesRemaining[handle] = 0u;
        voices->RefreshRenderClass(handle);
        moveExpectedRender(handle, svms::VoiceRenderClass::Generic,
                           svms::VoiceRenderClass::SustainedLoop);
    }
    const auto sustainedBeforeRelease = expectedRender[
        static_cast<uint32_t>(svms::VoiceRenderClass::SustainedLoop)];
    for (uint32_t position = 0u; position < sustainedBeforeRelease.size();
         position += 3u) {
        const svms::VoiceHandle handle = sustainedBeforeRelease[position];
        voices->v.state[handle] =
            static_cast<uint8_t>(svms::VoiceState::Releasing);
        voices->RefreshRenderClass(handle);
        moveExpectedRender(handle, svms::VoiceRenderClass::SustainedLoop,
                           svms::VoiceRenderClass::ReleaseLoop);
    }

    std::vector<uint8_t> seen(svms::kMaxPolyphony, 0u);
    uint32_t indexed = 0u;
    for (uint32_t channel = 0u; channel < svms::kChannelCount; ++channel) {
        uint32_t channelCount = 0u;
        voices->ForEachChannelActive(static_cast<uint8_t>(channel),
            [&](svms::VoiceHandle handle) {
                Check(handle < svms::kMaxPolyphony,
                      "paged channel handle remains in range");
                if (handle >= svms::kMaxPolyphony) return;
                Check(expectedActive[handle] != 0u &&
                          expectedChannel[handle] == channel,
                      "paged channel handle remains in its owning channel");
                Check(seen[handle] == 0u,
                      "paged channel index contains no duplicate handle");
                Check(channelCount < expectedLists[channel].size() &&
                          handle == expectedLists[channel][channelCount],
                      "paged channel traversal preserves flat swap-remove order");
                seen[handle] = 1u;
                ++channelCount;
                ++indexed;
            });
        Check(channelCount == voices->GetChannelActiveCount(
                  static_cast<uint8_t>(channel)),
              "paged channel count matches block traversal");
    }
    Check(indexed == voices->GetActiveCount(),
          "paged channel blocks contain every active voice exactly once");
    for (uint32_t handle = 0u; handle < svms::kMaxPolyphony; ++handle)
        Check(seen[handle] == expectedActive[handle],
              "paged channel membership matches lifecycle oracle");

    uint32_t renderIndexed = 0u;
    for (uint32_t classIndex = 0u;
         classIndex < svms::kVoiceRenderClassCount; ++classIndex) {
        const uint32_t classCount = voices->GetRenderClassCount(
            static_cast<svms::VoiceRenderClass>(classIndex));
        Check(classCount == expectedRender[classIndex].size(),
              "paged render-class count matches the lifecycle oracle");
        renderIndexed += classCount;
    }
    Check(renderIndexed == voices->GetActiveCount(),
          "paged render classes count every active voice exactly once");
}

void TestChannelTerminationControllers() {
    {
        svms::ChannelCache channels;
        channels.ControlChange(3, 66, 127);
        Check(channels.IsSostenutoActive(3),
              "CC66 enables channel sostenuto state");
        channels.ResetControllers(3);
        Check(!channels.IsSostenutoActive(3),
              "CC121 resets channel sostenuto state");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(4, 44100);
        const auto captured = voices->AllocateVoice(0, 60, 100);
        voices->SetVoicePlayIndex(captured, 10u);
        voices->CaptureSostenuto(0);

        const auto launchedAfterPedal = voices->AllocateVoice(0, 64, 100);
        voices->SetVoicePlayIndex(launchedAfterPedal, 11u);
        voices->NoteOffPlayIndex(0, 60, 10u, false, 7u);
        voices->NoteOffPlayIndex(0, 64, 11u, false, 8u);
        Check(voices->v.state[captured] ==
                  static_cast<uint8_t>(svms::VoiceState::Active) &&
                  voices->v.heldBySostenuto[captured] == 1u,
              "sostenuto holds only notes captured at pedal-down");
        Check(voices->v.state[launchedAfterPedal] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing),
              "sostenuto does not capture notes launched after pedal-down");

        voices->ReleaseSostenuto(0, 19u);
        Check(voices->v.state[captured] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing) &&
                  voices->v.releaseStartInBlock[captured] == 19u,
              "sostenuto pedal-up releases a captured key-up voice exactly");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(3, 44100);
        const auto keyStillDown = voices->AllocateVoice(2, 67, 100);
        voices->SetVoicePlayIndex(keyStillDown, 20u);
        voices->CaptureSostenuto(2);
        voices->ReleaseSostenuto(2, 5u);
        Check(voices->v.state[keyStillDown] ==
                  static_cast<uint8_t>(svms::VoiceState::Active) &&
                  voices->v.heldBySostenuto[keyStillDown] == 0u,
              "sostenuto pedal-up preserves a key that is still depressed");
        voices->NoteOffPlayIndex(2, 67, 20u, false, 6u);
        Check(voices->v.state[keyStillDown] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing),
              "a later key-up releases a no-longer-captured voice");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(2, 44100);
        const auto heldByBoth = voices->AllocateVoice(4, 72, 100);
        voices->SetVoicePlayIndex(heldByBoth, 30u);
        voices->CaptureSostenuto(4);
        voices->NoteOffPlayIndex(4, 72, 30u, true, 3u);
        voices->ReleaseSustain(4, 4u);
        Check(voices->v.state[heldByBoth] ==
                  static_cast<uint8_t>(svms::VoiceState::Active),
              "releasing sustain preserves a voice still held by sostenuto");
        voices->ReleaseSostenuto(4, 9u);
        Check(voices->v.state[heldByBoth] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing) &&
                  voices->v.releaseStartInBlock[heldByBoth] == 9u,
              "the final released pedal starts the SF2 release");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(4, 44100);
        const svms::VoiceHandle first = voices->AllocateVoice(0, 60, 100);
        const svms::VoiceHandle layered = voices->AllocateVoice(0, 60, 100);
        const svms::VoiceHandle otherChannel = voices->AllocateVoice(1, 64, 100);
        voices->v.heldBySustain[first] = 1;
        voices->v.heldBySustain[layered] = 1;
        voices->SetVoiceSample(first, 0, 64, 8, 56, 3, 1.0f, 1);
        voices->SetVoiceSample(layered, 0, 64, 8, 56, 1, 1.0f, 1);

        voices->ReleaseChannel(0, 17);
        Check(voices->v.state[first] == static_cast<uint8_t>(svms::VoiceState::Releasing) &&
                  voices->v.state[layered] == static_cast<uint8_t>(svms::VoiceState::Releasing),
              "CC123 releases every layer on only its target channel");
        Check(voices->v.heldBySustain[first] == 0 &&
                  voices->v.heldBySustain[layered] == 0,
              "CC123 releases voices previously held by sustain");
        Check(voices->v.releaseStartInBlock[first] == 17 &&
                  voices->v.releaseStartInBlock[layered] == 17,
              "CC123 release begins at its exact event frame");
        Check(voices->v.state[otherChannel] == static_cast<uint8_t>(svms::VoiceState::Active),
              "CC123 does not release another MIDI channel");
        Check(voices->v.loopEnabled[first] == 0 && voices->v.loopEnabled[layered] == 1,
              "CC123 stops SF2 mode-3 key-held loops but preserves continuous loops");
    }

    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(3, 44100);
        const svms::VoiceHandle first = voices->AllocateVoice(0, 60, 100);
        const svms::VoiceHandle second = voices->AllocateVoice(0, 64, 100);
        const svms::VoiceHandle otherChannel = voices->AllocateVoice(1, 67, 100);
        voices->v.stealTailFramesRemaining[first] = 128;
        voices->v.stealFadeInFramesRemaining[first] = 128;
        voices->v.stealTailFramesRemaining[otherChannel] = 128;
        voices->v.stealTailChannel[otherChannel] = 0;
        voices->RefreshStealTail(first);
        voices->RefreshStealTail(otherChannel);

        voices->SilenceChannelImmediate(0);
        Check(!voices->IsActive(first) && !voices->IsActive(second),
              "CC120 retires all voices on its target channel immediately");
        Check(voices->IsActive(otherChannel) && voices->GetActiveCount() == 1,
              "CC120 preserves voices on other MIDI channels");
        Check(voices->v.stealTailFramesRemaining[first] == 0 &&
                  voices->v.stealFadeInFramesRemaining[first] == 0 &&
                  voices->v.stealTailFramesRemaining[otherChannel] == 0,
              "CC120 also cancels steal crossfades on the silenced channel");
    }

    Check(svms::SequenceAtOrBefore(100, 101) &&
              svms::SequenceAtOrBefore(101, 101) &&
              !svms::SequenceAtOrBefore(102, 101),
          "termination fence rejects only note-ons at or before the controller");
    Check(svms::SequenceAtOrBefore(UINT32_MAX, 0) &&
              !svms::SequenceAtOrBefore(1, 0),
          "termination fence comparison remains ordered across sequence wrap");
}

void TestOverlappingRetriggerGenerations() {
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(8);

    svms::VoiceHandle firstLayer = voices->AllocateVoice(0, 60, 100);
    svms::VoiceHandle firstLayer2 = voices->AllocateVoice(0, 60, 100);
    svms::VoiceHandle secondLayer = voices->AllocateVoice(0, 60, 100);
    voices->SetVoicePlayIndex(firstLayer, 10);
    voices->SetVoicePlayIndex(firstLayer2, 10);
    voices->SetVoicePlayIndex(secondLayer, 11);

    Check(voices->FindOldestPlayIndex(0, 60) == 10,
          "note-off selects the oldest retrigger generation");
    voices->NoteOffPlayIndex(0, 60, 10, false, 37);
    Check(voices->v.state[firstLayer] == static_cast<uint8_t>(svms::VoiceState::Releasing) &&
              voices->v.state[firstLayer2] == static_cast<uint8_t>(svms::VoiceState::Releasing),
          "all layered regions from one retrigger are released together");
    Check(voices->v.state[secondLayer] == static_cast<uint8_t>(svms::VoiceState::Active),
          "a later same-key retrigger survives the earlier note-off");
    Check(voices->FindOldestPlayIndex(0, 60) == 11,
          "O(1) oldest-generation tail advances after layered release");
    voices->NoteOffPlayIndex(0, 60, 11, true, 0);
    Check(voices->v.heldBySustain[secondLayer] == 1 &&
              voices->FindOldestPlayIndex(0, 60) == UINT32_MAX,
          "sustain-held generation leaves the pending note-off chain");

    auto batched = std::make_unique<svms::VoiceManager>();
    batched->Initialize(8);
    const auto oldestLeft = batched->AllocateVoice(2, 72, 100);
    const auto oldestRight = batched->AllocateVoice(2, 72, 100);
    const auto newest = batched->AllocateVoice(2, 72, 100);
    const auto otherKey = batched->AllocateVoice(2, 73, 100);
    batched->SetVoicePlayIndex(oldestLeft, 20u);
    batched->SetVoicePlayIndex(oldestRight, 20u);
    batched->SetVoicePlayIndex(newest, 21u);
    batched->SetVoicePlayIndex(otherKey, 22u);
    Check(batched->NoteOffOldestPlayIndices(2, 72, 1u, false, 19u) == 1u &&
              batched->v.state[oldestLeft] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing) &&
              batched->v.state[oldestRight] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing) &&
              batched->v.state[newest] ==
                  static_cast<uint8_t>(svms::VoiceState::Active),
          "batched stale note-off preserves stereo grouping and exact count");
    Check(batched->NoteOffOldestPlayIndices(2, 72, 255u, false, 23u) == 1u &&
              batched->v.state[newest] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing) &&
              batched->v.state[otherKey] ==
                  static_cast<uint8_t>(svms::VoiceState::Active),
          "batched stale note-off stops at an empty key without touching others");
}
