// Test cases split out of V3Tests.cpp; included at file scope (outside the anonymous namespace).

void TestFastProducerRing() {
    // One thread feeds the single-producer ring while another feeds the MPSC
    // lanes; the merged stream must contain every event exactly once and keep
    // the fast thread's own order strictly.
    auto ingress = std::make_unique<
        svms::PriorityEventIngress<svms::TimestampedMidiEvent>>();
    constexpr uint32_t kFastEvents = 300000u;  // > ring capacity: exercises wrap + full
    constexpr uint32_t kLaneEvents = 50000u;
    std::atomic<uint32_t> laneSequence{0u};
    std::thread fastProducer([&] {
        for (uint32_t i = 0u; i < kFastEvents; ++i) {
            svms::TimestampedMidiEvent e{};
            e.message = 0x90u | (i & 0x7fu) << 8u;
            e.qpcTimestamp = i;
            e.sequence = i * 2u;  // even sequences = fast stream
            while (!ingress->TryPushFast(e)) std::this_thread::yield();
        }
    });
    std::thread laneProducer([&] {
        for (uint32_t i = 0u; i < kLaneEvents; ++i) {
            svms::TimestampedMidiEvent e{};
            e.message = 0x80u;
            e.qpcTimestamp = i;
            e.sequence = i * 2u + 1u;  // odd sequences = lane stream
            while (!ingress->TryPush(svms::EventLane::State, e))
                std::this_thread::yield();
            laneSequence.store(i, std::memory_order_relaxed);
        }
    });
    svms::PriorityEventIngress<svms::TimestampedMidiEvent>::OrderedMergeState
        merge{};
    uint32_t fastSeen = 0u, laneSeen = 0u;
    bool fastInOrder = true, laneInOrder = true;
    svms::TimestampedMidiEvent e{};
    while (fastSeen < kFastEvents || laneSeen < kLaneEvents) {
        if (!ingress->TryPopSequenceOrdered(e, merge)) {
            std::this_thread::yield();
            continue;
        }
        if ((e.sequence & 1u) == 0u) {
            fastInOrder = fastInOrder && e.qpcTimestamp == fastSeen;
            ++fastSeen;
        } else {
            laneInOrder = laneInOrder && e.qpcTimestamp == laneSeen;
            ++laneSeen;
        }
    }
    fastProducer.join();
    laneProducer.join();
    Check(fastSeen == kFastEvents && laneSeen == kLaneEvents,
          "fast ring + lanes deliver every event exactly once");
    Check(fastInOrder && laneInOrder,
          "each producer's stream stays in its own order through the merge");
    Check(ingress->TotalSize() == 0u && !ingress->TryPop(e),
          "the merged ingress is empty afterwards");
}

void TestStealReject() {
    svms::ChannelParamsSnapshot channel{};
    channel.volume = channel.expression = 1.0f;
    channel.panLeft = channel.panRight = 0.70710678f;
    channel.mixScaleLeft = channel.mixScaleRight = 0.70710678f;
    auto makeSetup = [](float gain, uint32_t playIndex) {
        svms::VoiceConfiguration setup{};
        setup.sampleStart = 0u;
        setup.sampleEnd = 128u;
        setup.loopStart = 8u;
        setup.loopEnd = 120u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep = 1.0f;
        setup.initialGain = setup.sustainLevel = gain;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.1f;
        setup.playIndex = playIndex;
        return setup;
    };
    for (const bool reject : {false, true}) {
        auto voices = std::make_unique<svms::VoiceManager>();
        Check(voices->Initialize(2u, 44100u), "steal reject fixture allocates two voices");
        voices->SetStealReject(reject);
        svms::VoiceHandle handles[1] = {svms::kInvalidVoice};
        for (uint32_t i = 0u; i < 2u; ++i) {
            const svms::VoiceConfiguration loud = makeSetup(1.0f, i + 1u);
            Check(voices->LaunchVoiceGroup(0u, static_cast<uint8_t>(60u + i), 127u,
                                           &loud, 1u, i + 1u, channel, handles),
                  "steal reject fixture fills the pool with loud voices");
        }
        voices->SetCurrentFrame(64u);
        const svms::VoiceConfiguration quiet = makeSetup(0.001f, 10u);
        const bool quietLaunched = voices->LaunchVoiceGroup(
            0u, 70u, 20u, &quiet, 1u, 10u, channel, handles);
        Check(quietLaunched == !reject,
              "a note quieter than every live voice is dropped only when the reject is on");
        Check(voices->LastLaunchRejected() == reject &&
                  voices->StealRejectCount() == (reject ? 1u : 0u),
              "the reject is reported separately from an allocation failure");
        Check(voices->GetActiveCount() == 2u &&
                  voices->GetChannelKeyVoiceCount(0u, 60u) +
                          voices->GetChannelKeyVoiceCount(0u, 61u) ==
                      (reject ? 2u : 1u),
              "a rejected note leaves the pool untouched; an accepted one steals");
        const svms::VoiceConfiguration loudNew = makeSetup(1.0f, 11u);
        Check(voices->LaunchVoiceGroup(0u, 72u, 127u, &loudNew, 1u, 11u,
                                       channel, handles) &&
                  !voices->LastLaunchRejected(),
              "a loud note still steals with the reject on");
    }
}

void TestPerKeyVoiceCap() {
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

    {
        // Cap disabled (default): the same key piles up without limit.
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(8u, 44100u);
        svms::VoiceHandle handle = svms::kInvalidVoice;
        for (uint32_t i = 0u; i < 4u; ++i)
            Check(voices->LaunchVoiceGroup(0u, 60u, 100u, &mono, 1u,
                                           channel, &handle),
                  "uncapped launch admits the note");
        Check(voices->GetChannelKeyVoiceCount(0u, 60u) == 4u,
              "cap 0 (off) leaves per-key polyphony unbounded");
    }

    {
        // Cap = 2: the third note-on replaces the oldest key member in
        // place instead of growing the pileup, and it never touches other
        // keys even when the whole pool is full.
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(4u, 44100u);
        voices->SetPerKeyVoiceCap(2u);
        svms::VoiceHandle key60[2]{};
        Check(voices->LaunchVoiceGroup(0u, 60u, 100u, &mono, 1u,
                                       channel, &key60[0]) &&
                  voices->LaunchVoiceGroup(0u, 60u, 100u, &mono, 1u,
                                           channel, &key60[1]),
              "launches under the cap take free slots");
        svms::VoiceHandle key64[2]{};
        Check(voices->LaunchVoiceGroup(0u, 64u, 100u, &mono, 1u,
                                       channel, &key64[0]) &&
                  voices->LaunchVoiceGroup(0u, 64u, 100u, &mono, 1u,
                                           channel, &key64[1]),
              "the pool fills with the other key");
        Check(voices->GetActiveCount() == 4u, "pool is full");
        svms::VoiceHandle replacement = svms::kInvalidVoice;
        Check(voices->LaunchVoiceGroup(0u, 60u, 100u, &mono, 1u,
                                       channel, &replacement),
              "a capped key still accepts note-ons");
        Check(voices->GetActiveCount() == 4u,
              "capped retrigger replaces in place instead of growing");
        Check(replacement == key60[0],
              "the oldest key member is the victim");
        Check(voices->IsActive(key64[0]) && voices->IsActive(key64[1]),
              "the keyed steal never touches other keys");
        Check(voices->GetChannelKeyVoiceCount(0u, 60u) == 2u,
              "the cap bounds the key chain");
    }

    {
        // Stereo note-on at cap 1: layer 0 replaces the whole play group,
        // layer 1 lands on the freed sibling slot — the launch never steals
        // its own newborn, and repeated retriggers stay stable.
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(8u, 44100u);
        voices->SetPerKeyVoiceCap(1u);
        svms::VoiceConfiguration stereo[2] = {mono, mono};
        // The short overload takes the group identity from setups[0];
        // UINT32_MAX means "no play group". Give each note-on its own.
        uint32_t groupPlayIndex = 200u;
        stereo[0].playIndex = stereo[1].playIndex = groupPlayIndex;
        svms::VoiceHandle group[2]{};
        Check(voices->LaunchVoiceGroup(0u, 60u, 100u, stereo, 2u,
                                       channel, group),
              "stereo launch under the cap succeeds");
        Check(voices->GetChannelKeyVoiceCount(0u, 60u) == 2u,
              "a stereo pair links two chain members");
        for (uint32_t retrigger = 0u; retrigger < 3u; ++retrigger) {
            svms::VoiceHandle replacement[2]{};
            ++groupPlayIndex;
            stereo[0].playIndex = stereo[1].playIndex = groupPlayIndex;
            Check(voices->LaunchVoiceGroup(0u, 60u, 100u, stereo, 2u,
                                           channel, replacement),
                  "capped stereo retrigger succeeds");
            Check(replacement[0] != replacement[1] &&
                      voices->GetActiveCount() == 2u &&
                      voices->GetChannelKeyVoiceCount(0u, 60u) == 2u,
                  "retrigger replaces the group without self-stealing");
            Check(voices->GetPlayGroupSizeForTest(replacement[0]) == 2u &&
                      voices->GetPlayGroupSizeForTest(replacement[1]) == 2u,
                  "the replacement layers form one play group");
        }
    }

    {
        // Note-offs unlink the key chain, so released keys admit fresh
        // notes without consuming the cap.
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(8u, 44100u);
        voices->SetPerKeyVoiceCap(2u);
        svms::VoiceHandle first = svms::kInvalidVoice;
        svms::VoiceHandle second = svms::kInvalidVoice;
        Check(voices->LaunchVoiceGroup(0u, 60u, 100u, &mono, 1u,
                                       channel, &first) &&
                  voices->LaunchVoiceGroup(0u, 60u, 100u, &mono, 1u,
                                           channel, &second),
              "two key launches fill the cap");
        voices->StartRelease(first);
        voices->StartRelease(second);
        Check(voices->GetChannelKeyVoiceCount(0u, 60u) == 0u,
              "releasing the key unlinks its chain members");
        svms::VoiceHandle relaunched = svms::kInvalidVoice;
        Check(voices->LaunchVoiceGroup(0u, 60u, 100u, &mono, 1u,
                                       channel, &relaunched),
              "a released key admits fresh notes");
        Check(voices->GetChannelKeyVoiceCount(0u, 60u) == 1u,
              "fresh notes count against the cap again");
    }
}

void TestExclusiveClassLifecycle() {
    svms::ChannelParamsSnapshot channel{};
    channel.volume = channel.expression = 1.0f;
    channel.panLeft = channel.panRight = 0.70710678f;
    channel.mixScaleLeft = channel.mixScaleRight = 0.70710678f;

    svms::VoiceConfiguration base{};
    base.sampleStart = 0u;
    base.sampleEnd = 128u;
    base.loopStart = 8u;
    base.loopEnd = 120u;
    base.loopMode = 1u;
    base.phaseStep = base.basePhaseStep = 1.0f;
    base.initialGain = base.sustainLevel = 1.0f;
    base.gainLeft = base.gainRight = 0.1f;

    auto voices = std::make_unique<svms::VoiceManager>();
    Check(voices->Initialize(16u, 44100u),
          "exclusive-class pool initializes");

    svms::VoiceConfiguration oldLayers[2] = {base, base};
    oldLayers[0].exclusiveClass = oldLayers[1].exclusiveClass = 7u;
    svms::VoiceHandle oldGroup[2]{};
    Check(voices->LaunchVoiceGroup(0u, 42u, 100u, oldLayers, 2u, 100u,
                                   channel, oldGroup),
          "layered exclusive-class note launches");

    svms::VoiceConfiguration other = base;
    other.exclusiveClass = 9u;
    svms::VoiceHandle otherClass = svms::kInvalidVoice;
    Check(voices->LaunchVoiceGroup(0u, 44u, 100u, &other, 1u, 101u,
                                   channel, &otherClass),
          "different exclusive class launches");
    other.exclusiveClass = 7u;
    svms::VoiceHandle otherChannel = svms::kInvalidVoice;
    Check(voices->LaunchVoiceGroup(1u, 46u, 100u, &other, 1u, 102u,
                                   channel, &otherChannel),
          "same exclusive class on another channel launches");
    other.exclusiveClass = 0u;
    svms::VoiceHandle unclassified = svms::kInvalidVoice;
    Check(voices->LaunchVoiceGroup(0u, 48u, 100u, &other, 1u, 103u,
                                   channel, &unclassified),
          "class zero note launches");
    Check(voices->GrowCapacity(24u),
          "exclusive-class index survives voice-pool growth");

    svms::VoiceConfiguration replacement[2] = {base, base};
    replacement[0].exclusiveClass = replacement[1].exclusiveClass = 7u;
    svms::VoiceHandle newGroup[2]{};
    constexpr uint32_t kChokeFrame = 37u;
    Check(voices->LaunchVoiceGroup(0u, 50u, 100u, replacement, 2u, 200u,
                                   channel, newGroup, kChokeFrame),
          "replacement exclusive-class note launches");

    const uint32_t fastRelease = svms::MakeReleaseSamples(
        svms::kMinReleaseSeconds, 44100u);
    Check(voices->v.state[oldGroup[0]] ==
              static_cast<uint8_t>(svms::VoiceState::Releasing) &&
          voices->v.state[oldGroup[1]] ==
              static_cast<uint8_t>(svms::VoiceState::Releasing),
          "exclusive class chokes the complete previous play group");
    Check(voices->v.releaseStartInBlock[oldGroup[0]] == kChokeFrame &&
          voices->v.releaseStartInBlock[oldGroup[1]] == kChokeFrame &&
          voices->v.releaseSamplesRemaining[oldGroup[0]] == fastRelease &&
          voices->v.releaseSamplesRemaining[oldGroup[1]] == fastRelease,
          "exclusive choke starts an exact-frame minimum release");
    Check(voices->v.state[newGroup[0]] ==
              static_cast<uint8_t>(svms::VoiceState::Active) &&
          voices->v.state[newGroup[1]] ==
              static_cast<uint8_t>(svms::VoiceState::Active) &&
          voices->v.playIndex[newGroup[0]] == 200u &&
          voices->v.playIndex[newGroup[1]] == 200u,
          "layers born by one note do not choke each other");
    Check(voices->v.state[otherClass] ==
              static_cast<uint8_t>(svms::VoiceState::Active) &&
          voices->v.state[otherChannel] ==
              static_cast<uint8_t>(svms::VoiceState::Active) &&
          voices->v.state[unclassified] ==
              static_cast<uint8_t>(svms::VoiceState::Active),
          "exclusive choke is isolated by class and MIDI channel");
}

void WriteTestWave(const std::filesystem::path& path) {
    constexpr uint32_t kFrames = 96u;
    constexpr uint32_t kDataBytes = kFrames * sizeof(int16_t);
    auto put16 = [](std::ofstream& stream, uint16_t value) {
        const char bytes[2] = {char(value), char(value >> 8u)};
        stream.write(bytes, sizeof(bytes));
    };
    auto put32 = [](std::ofstream& stream, uint32_t value) {
        const char bytes[4] = {char(value), char(value >> 8u),
            char(value >> 16u), char(value >> 24u)};
        stream.write(bytes, sizeof(bytes));
    };
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4); put32(output, 36u + kDataBytes);
    output.write("WAVEfmt ", 8); put32(output, 16u);
    put16(output, 1u); put16(output, 1u); put32(output, 44100u);
    put32(output, 88200u); put16(output, 2u); put16(output, 16u);
    output.write("data", 4); put32(output, kDataBytes);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const int16_t value = static_cast<int16_t>(
            std::sin(static_cast<double>(frame) * 0.31) * 24000.0);
        put16(output, static_cast<uint16_t>(value));
    }
}

void TestSfzParserAndPlayback() {
    namespace fs = std::filesystem;
    wchar_t tempRoot[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempRoot);
    const fs::path directory = fs::path(tempRoot) /
        (L"svms_sfz_test_" + std::to_wstring(GetCurrentProcessId()));
    const fs::path samples = directory / L"Samples";
    const fs::path instruments = directory / L"Instruments";
    std::error_code ec;
    fs::remove_all(directory, ec);
    fs::create_directories(samples, ec);
    fs::create_directories(instruments, ec);
    const fs::path wavePath = samples / L"My Tone.wav";
    const fs::path sfzPath = instruments / L"Mapped Instrument.sfz";
    WriteTestWave(wavePath);
    {
        std::ofstream sfz(sfzPath, std::ios::binary);
        sfz << "<control> default_path=../Samples/\n"
               "<group> sample=My Tone.wav lovel=10 hivel=100 "
               "pitch_keycenter=C3 transpose=2 tune=-7 pitch_keytrack=50 "
               "volume=-6 pan=-20 offset=2 end=80 loop_mode=loop_sustain "
               "loop_start=8 loop_end=40 ampeg_attack=0.01 "
               "ampeg_decay=0.2 ampeg_sustain=50 ampeg_release=0.3 group=11\n"
               "<region> lokey=C4 hikey=C4 cutoff=1200\n"
               "<region> lokey=61 hikey=61 group=12 off_by=11 volume=3\n";
    }

    auto data = std::make_unique<svms::SF2Data>();
    Check(svms::soundfont_load(sfzPath.c_str(), data.get()),
          "SFZ frontend loads an external WAV with a relative spaced path");
    if (!data->loaded) {
        fs::remove_all(directory, ec);
        return;
    }
    const uint32_t compiledCount = data->regionCount;
    svms::sf2_build_regions(data.get());
    Check(data->isSfz && data->regionCount == 2u &&
              data->regionCount == compiledCount && data->sampleCount == 1u,
          "SFZ compiles directly into stable shared sample/region tables");

    SetEnvironmentVariableW(L"SVMS_TEST_SOUNDFONT_DIRECTORY",
                            directory.c_str());
    svms::EngineConfig discovered = svms::EngineConfig::Default();
    discovered.soundFontPath.clear();
    std::string discoveryWarning;
    Check(fs::path(svms::ResolveV3SoundFontPath(
              discovered, &discoveryWarning)).lexically_normal() ==
              sfzPath.lexically_normal(),
          "recursive local discovery finds SFZ instruments in subfolders");
    discovered.soundFontPath = L"Instruments\\Mapped Instrument.sfz";
    Check(fs::path(svms::ResolveV3SoundFontPath(discovered)).lexically_normal() ==
              sfzPath.lexically_normal(),
          "configured SFZ selection retains and resolves its relative subfolder path");
    SetEnvironmentVariableW(L"SVMS_TEST_SOUNDFONT_DIRECTORY", nullptr);
    Check(data->sampleDataFrames >= 97u &&
              data->sampleData[data->sampleDataFrames + 7u] == 0,
          "SFZ sample store carries the required eight-sample AVX2 padding");

    uint32_t preset = UINT32_MAX;
    Check(svms::sf2_resolve_preset(data.get(), 37u, 99u, false, &preset) &&
              preset == 0u,
          "single SFZ instrument remains selected across MIDI bank/program changes");
    const svms::SFSampleRegion* matches[4]{};
    Check(svms::sf2_find_regions(data.get(), preset, 60u, 9u,
                                 matches, 4u) == 0u &&
              svms::sf2_find_regions(data.get(), preset, 60u, 64u,
                                     matches, 4u) == 1u,
          "SFZ inherited key and velocity mapping selects exact regions");
    const svms::SFSampleRegion& first = data->regions[0];
    const svms::SFSampleRegion& second = data->regions[1];
    Check(first.rootKey == 48 && first.coarseTune == 2 &&
              first.fineTune == -7 && first.scaleTuning == 50,
          "SFZ pitch center, transpose, tune, and key tracking compile");
    Check(first.initialAttenuation == 60 && first.pan == -100 &&
              first.startOffset == 2 && first.endOffset == 81,
          "SFZ gain, pan, and sample bounds compile");
    Check(first.loopMode == 3u && first.loopStartOffset == 8 &&
              first.loopEndOffset == 41 && first.attackVolEnv < -7900 &&
              first.sustainVolEnv == 60 && first.releaseVolEnv < -2000,
          "SFZ loops and amp envelope compile into shared region units");
    Check(first.exclusiveClass != 0 && first.offByClass == -1 &&
              second.exclusiveClass != first.exclusiveClass &&
              second.offByClass == first.exclusiveClass &&
              second.initialAttenuation == -30,
          "SFZ group/off_by compile as separate membership and choke classes");

    auto voices = std::make_unique<svms::VoiceManager>();
    Check(voices->Initialize(4u, 44100u), "SFZ playback voice pool initializes");
    svms::ChannelCache channels;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    channels.RebuildCache(cfg, 44100.0f);
    svms::VoiceConfiguration setup{};
    setup.sampleStart = static_cast<uint32_t>(first.startOffset);
    setup.sampleEnd = static_cast<uint32_t>(first.endOffset);
    setup.loopStart = static_cast<uint32_t>(first.loopStartOffset);
    setup.loopEnd = static_cast<uint32_t>(first.loopEndOffset);
    setup.loopMode = first.loopMode;
    setup.phaseStep = setup.basePhaseStep = 1.0f;
    setup.initialGain = svms::InitialAttenuationToGain(
        static_cast<float>(first.initialAttenuation));
    setup.sustainLevel = svms::SustainAttenuationToGain(
        static_cast<float>(first.sustainVolEnv));
    setup.releaseDecay = 0.999f;
    setup.releaseSamples = 4410u;
    channels.ComputeSoundFontPan(first.pan, setup.gainLeft, setup.gainRight);
    setup.exclusiveClass = static_cast<uint16_t>(first.exclusiveClass);
    setup.offByClass = first.offByClass < 0
        ? UINT16_MAX : static_cast<uint16_t>(first.offByClass);
    svms::VoiceHandle voice = svms::kInvalidVoice;
    Check(voices->LaunchVoiceGroup(0u, 60u, 64u, &setup, 1u, 1u,
                                   channels.GetParams()[0], &voice),
          "compiled SFZ region launches through the normal voice transaction");
    float left[48]{}, right[48]{};
    svms::RenderScalar renderer;
    renderer.RenderBlock(*voices, channels, data->sampleData,
                         data->sampleDataFrames, left, right, 48u, cfg);
    float peak = 0.0f;
    for (float value : left) peak = (std::max)(peak, std::fabs(value));
    Check(peak > 0.05f,
          "compiled SFZ WAV renders through the existing scalar sample path");

    svms::VoiceHandle sameGroupVoice = svms::kInvalidVoice;
    Check(voices->LaunchVoiceGroup(0u, 60u, 64u, &setup, 1u, 3u,
                                   channels.GetParams()[0], &sameGroupVoice,
                                   19u) &&
              voices->v.state[voice] ==
                  static_cast<uint8_t>(svms::VoiceState::Active),
          "SFZ group membership alone does not self-choke");

    svms::VoiceConfiguration choker = setup;
    choker.exclusiveClass = static_cast<uint16_t>(second.exclusiveClass);
    choker.offByClass = static_cast<uint16_t>(second.offByClass);
    svms::VoiceHandle chokerVoice = svms::kInvalidVoice;
    Check(voices->LaunchVoiceGroup(0u, 61u, 64u, &choker, 1u, 2u,
                                   channels.GetParams()[0], &chokerVoice, 23u) &&
              voices->v.state[voice] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing) &&
              voices->v.releaseStartInBlock[voice] == 23u &&
              voices->v.state[sameGroupVoice] ==
                  static_cast<uint8_t>(svms::VoiceState::Releasing) &&
              voices->v.releaseStartInBlock[sameGroupVoice] == 23u &&
              voices->v.state[chokerVoice] ==
                  static_cast<uint8_t>(svms::VoiceState::Active),
          "SFZ off_by chokes its target group at the exact launch frame");

    svms::sf2_free(data.get());
    fs::remove_all(directory, ec);
}

// 16 stereo bus planes for one renderer; planes are exactly kFrames long so
// a per-block zero-fill leaves no stale audio.
struct ChannelBusSet {
    std::vector<float> planes;
    float* left[16];
    float* right[16];

    void Init(uint32_t frames) {
        planes.assign(static_cast<size_t>(16u) * 2u * frames, 0.0f);
        for (uint32_t c = 0; c < 16u; ++c) {
            left[c] = planes.data() + static_cast<size_t>(c) * 2u * frames;
            right[c] = left[c] + frames;
        }
    }
    void Zero() { std::fill(planes.begin(), planes.end(), 0.0f); }
};

void TestPerChannelLimiterDifferential() {
    // ── Unit: the classic per-channel limiter math (shared by driver and
    // oracle paths) ──
    {
        svms::EngineConfig config = svms::EngineConfig::Default();
        config.channelLimiterEnabled = true;
        config.channelLimiterThreshold = 0.5f;  // -6 dBFS
        config.channelLimiterReleaseMs = 150.0f;
        svms::ChannelLimiterState limiter;
        limiter.Configure(44100u, config);

        // Driver buses use the allocation capacity as their plane stride,
        // while a callback may contain fewer frames. Clearing must follow
        // the plane tables rather than treating the active prefixes as one
        // packed allocation, or old audio reappears in later callbacks.
        {
            constexpr uint32_t kCapacity = 32u;
            constexpr uint32_t kActiveFrames = 11u;
            ChannelBusSet stridedBuses;
            stridedBuses.Init(kCapacity);
            std::fill(stridedBuses.planes.begin(),
                      stridedBuses.planes.end(), 0.75f);
            svms::ChannelLimiterState::ClearBuses(
                stridedBuses.left, stridedBuses.right, kActiveFrames);
            bool activePrefixesClear = true;
            bool inactiveTailsUntouched = true;
            for (uint32_t c = 0u; c < 16u; ++c) {
                for (uint32_t f = 0u; f < kActiveFrames; ++f) {
                    activePrefixesClear = activePrefixesClear &&
                        stridedBuses.left[c][f] == 0.0f &&
                        stridedBuses.right[c][f] == 0.0f;
                }
                inactiveTailsUntouched = inactiveTailsUntouched &&
                    stridedBuses.left[c][kActiveFrames] == 0.75f &&
                    stridedBuses.right[c][kActiveFrames] == 0.75f;
            }
            Check(activePrefixesClear && inactiveTailsUntouched,
                  "variable-size callbacks clear every strided channel "
                  "bus prefix without touching its inactive tail");
        }

        // A live threshold is shared by all buses. Identical simultaneous
        // input must see the same threshold while it glides.
        {
            limiter.Reset();
            limiter.SetLiveTargets(true, 1.0f, 150.0f, 44100u);
            ChannelBusSet glideBuses;
            glideBuses.Init(1u);
            for (uint32_t c = 0u; c < 16u; ++c) {
                glideBuses.left[c][0] = 0.7f;
                glideBuses.right[c][0] = 0.7f;
            }
            float glideOutL = 0.0f;
            float glideOutR = 0.0f;
            limiter.ProcessAndSum(glideBuses.left, glideBuses.right,
                                  &glideOutL, &glideOutR, 1u);
            bool channelThresholdsMatch = true;
            for (uint32_t c = 1u; c < 16u; ++c) {
                channelThresholdsMatch = channelThresholdsMatch &&
                    limiter.channel[c].gainReductionDb ==
                        limiter.channel[0].gainReductionDb;
            }
            Check(channelThresholdsMatch,
                  "live threshold glide advances once per frame and is "
                  "identical across channels");
            limiter.Configure(44100u, config);
        }

        constexpr uint32_t kFrames = 4096u;
        ChannelBusSet buses;
        buses.Init(kFrames);
        for (uint32_t f = 0; f < kFrames; ++f) {
            buses.left[0][f] =
                0.9f * std::sin(static_cast<float>(f) * 0.021f);
            buses.right[0][f] =
                0.7f * std::cos(static_cast<float>(f) * 0.017f);
            // Quiet channel below the threshold must pass through exactly.
            buses.left[1][f] = 0.1f * std::sin(static_cast<float>(f) * 0.033f);
            buses.right[1][f] = 0.1f * std::cos(static_cast<float>(f) * 0.029f);
        }
        std::vector<float> masterL(kFrames, 0.0f), masterR(kFrames, 0.0f);
        limiter.ProcessAndSum(buses.left, buses.right, masterL.data(),
                              masterR.data(), kFrames);

        // The channel stage uses a short gain ramp to avoid a discontinuity;
        // after that attack settles, the loud bus stays within the soft-knee
        // ceiling. The downstream master limiter owns the brief overshoot.
        float loudPeak = 0.0f;
        constexpr uint32_t kAttackSettleFrames = 256u;
        for (uint32_t f = kAttackSettleFrames; f < kFrames; ++f) {
            // Subtract the exact quiet-channel passthrough.
            const float l = masterL[f] - buses.left[1][f];
            const float r = masterR[f] - buses.right[1][f];
            loudPeak = (std::max)(loudPeak, (std::max)(std::fabs(l),
                                                       std::fabs(r)));
        }
        Check(loudPeak <= 0.5f *
                  svms::ChannelLimiterState::kKneeRatio * (1.0f + 1.0e-4f),
              "per-channel limiter settles within the soft-knee ceiling "
              "after its click-free attack ramp");

        // Step response: reduction begins immediately but ramps rather than
        // jumping straight to the target gain in one sample.
        {
            ChannelBusSet stepBuses;
            constexpr uint32_t kStepFrames = 128u;
            stepBuses.Init(kStepFrames);
            for (uint32_t f = 0u; f < kStepFrames; ++f) {
                stepBuses.left[0][f] = 0.9f;   // rho = 1.8 > knee
                stepBuses.right[0][f] = 0.9f;
            }
            std::vector<float> stepL(kStepFrames, 0.0f),
                stepR(kStepFrames, 0.0f);
            limiter.Reset();
            limiter.ProcessAndSum(stepBuses.left, stepBuses.right,
                                  stepL.data(), stepR.data(), kStepFrames);
            bool monotonicAttack = stepL[0] < 0.9f && stepL[0] > 0.5f;
            for (uint32_t f = 1u; f < kStepFrames; ++f)
                monotonicAttack = monotonicAttack &&
                    stepL[f] <= stepL[f - 1u];
            Check(monotonicAttack && stepL[kStepFrames - 1u] < 0.51f &&
                      stepR[kStepFrames - 1u] < 0.51f,
                  "above-knee transients engage with a smooth monotonic "
                  "gain ramp and settle at the threshold");
        }
        Check(limiter.channel[0].gainReductionDb > 1.0f,
              "per-channel limiter reports gain reduction on the loud bus");
        Check(limiter.channel[1].gainReductionDb == 0.0f,
              "per-channel limiter leaves the quiet bus untouched");

        // Quiet-channel-only input is an exact passthrough (gain == 1).
        ChannelBusSet quietBuses;
        quietBuses.Init(kFrames);
        for (uint32_t f = 0; f < kFrames; ++f) {
            quietBuses.left[3][f] =
                0.1f * std::sin(static_cast<float>(f) * 0.019f);
            quietBuses.right[3][f] =
                0.1f * std::cos(static_cast<float>(f) * 0.023f);
        }
        std::fill(masterL.begin(), masterL.end(), 0.0f);
        std::fill(masterR.begin(), masterR.end(), 0.0f);
        limiter.ProcessAndSum(quietBuses.left, quietBuses.right,
                              masterL.data(), masterR.data(), kFrames);
        bool quietExact = true;
        for (uint32_t f = 0; f < kFrames && quietExact; ++f) {
            quietExact = masterL[f] == quietBuses.left[3][f] &&
                         masterR[f] == quietBuses.right[3][f];
        }
        Check(quietExact,
              "below-threshold channels pass through bit-exactly");

        // Release: silence decays the envelope toward zero (150 ms at
        // 44.1 kHz needs ~10 blocks of 4096 frames to settle).
        for (uint32_t f = 0; f < kFrames; ++f) {
            buses.left[0][f] = 0.0f;
            buses.right[0][f] = 0.0f;
            buses.left[1][f] = 0.0f;
            buses.right[1][f] = 0.0f;
        }
        for (int pass = 0; pass < 12; ++pass) {
            limiter.ProcessAndSum(buses.left, buses.right, masterL.data(),
                                  masterR.data(), kFrames);
        }
        Check(limiter.channel[0].envelope < 1.0e-3f,
              "per-channel limiter envelope releases during silence");
    }

    // ── Differential: engine vs frame-major oracle through the channel
    // buses, plus the same shared limiter applied to both ──
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 6u;
    constexpr uint32_t kPool = 48u;
    constexpr uint32_t kSeedCount = 32u;
    constexpr uint32_t kEventsPerBlock = 10u;
    constexpr float kThreshold = 0.15f;
    // Full-pool note churn forces steal storms: a tolerance-equal state
    // drift can flip a steal victim selection, and the displaced ghost then
    // renders different (bounded) content — the documented drift class the
    // Hilbert storm differential carries at 3.0e-2 for the same regime.
    // 1.0e-2 leaves generous margin under that while staying far above the
    // observed ~2.3e-3 worst case; the scalar pass proves the bus math is
    // exact (~4e-9).
    constexpr float kMaxDiff = 1.0e-2f;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.4f * std::sin(static_cast<float>(i) * 0.031f) +
             0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    svms::EngineConfig limiterConfig = svms::EngineConfig::Default();
    limiterConfig.channelLimiterEnabled = true;
    limiterConfig.channelLimiterThreshold = kThreshold;
    limiterConfig.channelLimiterReleaseMs = 150.0f;

    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        const char* backendName = avx2Pass ? "avx2" : "scalar";

        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool,
                                    kSeedCount);
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto engineVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache engineChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar engine;
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  engine.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  engine.ConfigureRenderThreads(4u, kFrames),
              "per-channel limiter differential starts serial and parallel "
              "renderers");
        Check(engine.SetRenderBackend(
                  avx2Pass ? svms::RenderBackend::AVX2
                           : svms::RenderBackend::Scalar),
              "per-channel limiter differential selects the requested "
              "backend");

        WholeVoiceCCContext oracleContext{oracleVoices.get(), &oracleChannels,
                                          oracleChannels.GetParams(), &cfg};
        WholeVoiceCCContext engineContext{engineVoices.get(), &engineChannels,
                                          engineChannels.GetParams(), &cfg};
        oracleContext.ResetMasterState();
        engineContext.ResetMasterState();
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent,
                                       &oracleContext);
        engine.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent,
                                       &engineContext);

        svms::ChannelLimiterState oracleLimiter;
        svms::ChannelLimiterState engineLimiter;
        oracleLimiter.Configure(44100u, limiterConfig);
        engineLimiter.Configure(44100u, limiterConfig);

        WholeVoiceRng rng;
        ChannelBusSet oracleBuses;
        ChannelBusSet engineBuses;
        oracleBuses.Init(kFrames);
        engineBuses.Init(kFrames);
        std::vector<float> oracleMasterL(kFrames, 0.0f),
            oracleMasterR(kFrames, 0.0f);
        std::vector<float> engineMasterL(kFrames, 0.0f),
            engineMasterR(kFrames, 0.0f);
        float passMaxDiff = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 40000u + block * kFrames;
            std::vector<svms::RenderEvent> events;
            events.reserve(kEventsPerBlock);
            // Odd blocks get a dedicated AllSoundOff event first (a barrier
            // event the renderer splits around); even blocks stay note-only.
            // Both ride the whole-voice bus path.
            if ((block & 1u) != 0u) {
                svms::RenderEvent event{};
                event.channel = 0u;
                event.frameOffset = 0u;
                event.ingressSequence = block * kEventsPerBlock;
                event.type = svms::RenderEventType::AllSoundOff;
                events.push_back(event);
            }
            for (uint32_t j = 0; j < kEventsPerBlock; ++j) {
                svms::RenderEvent event{};
                // Channel 0 carries most of the load (the hot bus); the
                // rest spread over channels 1-3.
                const uint32_t channelRoll = rng.Next() % 10u;
                event.channel = channelRoll < 6u
                    ? 0u
                    : static_cast<uint8_t>(1u + rng.Next() % 3u);
                event.frameOffset = rng.Next() % kFrames;
                event.ingressSequence = block * kEventsPerBlock + j;
                const uint32_t roll = rng.Next() % 3u;
                if (roll == 0u) {
                    event.type = svms::RenderEventType::NoteOn;
                    event.data1 =
                        static_cast<uint8_t>(36u + rng.Next() % 72u);
                    event.data2 =
                        static_cast<uint8_t>(64u + rng.Next() % 64u);
                } else if (roll == 1u) {
                    event.type = svms::RenderEventType::NoteOff;
                    event.data1 =
                        static_cast<uint8_t>(36u + rng.Next() % 72u);
                } else {
                    // Mix-fold controller: keeps the row-op machinery in
                    // the scenario (whole-voice blocks fold it as an
                    // exact-frame mix op; dense/sparse blocks fold it at
                    // span boundaries).
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 7u;
                    event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
                }
                events.push_back(event);
            }
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });

            oracleBuses.Zero();
            engineBuses.Zero();
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), sampleCount,
                oracleMasterL.data(), oracleMasterR.data(), kFrames, cfg,
                events.data(), static_cast<uint32_t>(events.size()), true,
                blockStartFrame, oracleBuses.left, oracleBuses.right);
            engine.RenderBlock(
                *engineVoices, engineChannels, samples.data(), sampleCount,
                engineMasterL.data(), engineMasterR.data(), kFrames, cfg,
                events.data(), static_cast<uint32_t>(events.size()), true,
                blockStartFrame, engineBuses.left, engineBuses.right);

            // Buses must match before limiting.
            float blockDiff = 0.0f;
            for (uint32_t c = 0; c < 16u; ++c) {
                for (uint32_t f = 0; f < kFrames; ++f) {
                    blockDiff = (std::max)(blockDiff,
                        std::fabs(oracleBuses.left[c][f] -
                                  engineBuses.left[c][f]));
                    blockDiff = (std::max)(blockDiff,
                        std::fabs(oracleBuses.right[c][f] -
                                  engineBuses.right[c][f]));
                }
            }
            // The masters are empty here (buses mode bypasses the direct
            // mix); apply the shared limiter to both bus sets and compare
            // the summed outputs too.
            std::fill(oracleMasterL.begin(), oracleMasterL.end(), 0.0f);
            std::fill(oracleMasterR.begin(), oracleMasterR.end(), 0.0f);
            std::fill(engineMasterL.begin(), engineMasterL.end(), 0.0f);
            std::fill(engineMasterR.begin(), engineMasterR.end(), 0.0f);
            oracleLimiter.ProcessAndSum(oracleBuses.left, oracleBuses.right,
                                        oracleMasterL.data(),
                                        oracleMasterR.data(), kFrames);
            engineLimiter.ProcessAndSum(engineBuses.left, engineBuses.right,
                                        engineMasterL.data(),
                                        engineMasterR.data(), kFrames);
            for (uint32_t f = 0; f < kFrames; ++f) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleMasterL[f] - engineMasterL[f]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleMasterR[f] - engineMasterR[f]));
            }
            if (blockDiff > kMaxDiff) {
                std::fprintf(stderr,
                    "[PCLDIFF] backend=%s block=%u max=%g\n",
                    backendName, block, blockDiff);
            }
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        std::fprintf(stderr, "[PCLDIFF] backend=%s max=%g\n",
                     backendName, passMaxDiff);
        Check(passMaxDiff <= kMaxDiff,
              "per-channel bus blocks match the oracle per sample");
        Check(engine.GetWholeVoiceBlocksForTest() == kBlocks,
              "scenario renders every bus block (barrier or not) on the "
              "whole-voice path");

        // Toggle continuity: a direct-mix block (buses off) between bus
        // blocks must stay in parity too.
        oracleBuses.Zero();
        engineBuses.Zero();
        std::fill(oracleMasterL.begin(), oracleMasterL.end(), 0.0f);
        std::fill(oracleMasterR.begin(), oracleMasterR.end(), 0.0f);
        std::fill(engineMasterL.begin(), engineMasterL.end(), 0.0f);
        std::fill(engineMasterR.begin(), engineMasterR.end(), 0.0f);
        oracle.RenderBlockReference(
            *oracleVoices, oracleChannels, samples.data(), sampleCount,
            oracleMasterL.data(), oracleMasterR.data(), kFrames, cfg,
            nullptr, 0u, true, 40000u + kBlocks * kFrames);
        engine.RenderBlock(
            *engineVoices, engineChannels, samples.data(), sampleCount,
            engineMasterL.data(), engineMasterR.data(), kFrames, cfg,
            nullptr, 0u, true, 40000u + kBlocks * kFrames);
        float offDiff = 0.0f;
        for (uint32_t f = 0; f < kFrames; ++f) {
            offDiff = (std::max)(offDiff,
                std::fabs(oracleMasterL[f] - engineMasterL[f]));
            offDiff = (std::max)(offDiff,
                std::fabs(oracleMasterR[f] - engineMasterR[f]));
        }
        Check(offDiff <= kMaxDiff,
              "direct-mix blocks stay in parity after bus-mode blocks");
    }
}


void TestExtendedKeysAndTuning() {
    for (uint32_t key = 0; key < 256; ++key) {
        svms::TimestampedMidiEvent input{};
        input.message = 0x90u | (key << 8u) | (100u << 16u);
        input.sequence = 9;
        input.qpcTimestamp = svms::kAbsoluteFrameTimestampTag | 17u;
        svms::ScheduledRenderEvent event{};
        Check(svms::CompileTimestampedEvent(input, 0, 1, 44100, 0, event),
              "all 256 note-ons compile");
        Check(event.data1 == key && event.targetFrame == 17 && event.sequence == 9,
              "extended keys preserve identity and timing");
        input.message = 0x80u | (key << 8u);
        Check(svms::CompileTimestampedEvent(input, 0, 1, 44100, 0, event) &&
              event.data1 == key && event.type == svms::RenderEventType::NoteOff,
              "all 256 note-offs compile without wrapping");
        Check(svms::RetunePhaseStep(0.1234567f, uint8_t(key), 1.0f, false) == 0.1234567f,
              "normal tuning is an exact bypass");
    }
    Check(svms::MidiKeyPitch(155, true) == 60.0f &&
          svms::MidiRegionKey(155, true) == 60,
          "31EDO key 155 selects middle C");
    const float low = svms::RetunePhaseStep(powf(2.0f, (155.0f-60)/12),155,1,true);
    const float high = svms::RetunePhaseStep(powf(2.0f, (186.0f-60)/12),186,1,true);
    Check(std::fabs(low - 1.0f) < 1e-6f && std::fabs(high / low - 2.0f) < 1e-6f,
          "31 steps render one octave apart");
    auto data = std::make_unique<svms::SF2Data>();
    data->presetCount = 1; data->regionCount = 2;
    data->presetRegionStart[0] = 0; data->presetRegionCount[0] = 2;
    data->regions[0].keyLo = 0; data->regions[0].keyHi = 63;
    data->regions[1].keyLo = 64; data->regions[1].keyHi = 127;
    for (int i=0;i<2;++i) { data->regions[i].velLo=0; data->regions[i].velHi=127; }
    const svms::SFSampleRegion* regions[2]{};
    Check(svms::sf2_find_regions(data.get(),0,255,100,regions,2)==1 &&
          regions[0]==&data->regions[1], "extended keys use the highest SoundFont zone");
    auto voices = std::make_unique<svms::VoiceManager>();
    Check(voices->Initialize(8,44100), "extended lifecycle pool initializes");
    const auto lowVoice = voices->AllocateVoice(0,127,100);
    const auto highVoice = voices->AllocateVoice(0,255,100);
    const auto nextChannel = voices->AllocateVoice(1,127,100);
    Check(lowVoice != svms::kInvalidVoice && highVoice != svms::kInvalidVoice &&
          nextChannel != svms::kInvalidVoice, "extended voices allocate");
    Check(voices->GetChannelKeyVoiceCount(0,127)==1 &&
          voices->GetChannelKeyVoiceCount(0,255)==1 &&
          voices->GetChannelKeyVoiceCount(1,127)==1, "extended key chains do not alias");
    voices->StartRelease(highVoice);
    Check(voices->GetChannelKeyVoiceCount(0,255)==0 &&
          voices->GetChannelKeyVoiceCount(0,127)==1 &&
          voices->GetChannelKeyVoiceCount(1,127)==1, "extended release leaves other keys intact");
}

// ═══════════════════════════════════════════════════════════════════════
// Exact note-on merging (NoteMerge.h).
//
// A merged note-on (count n) launches ONE voice at n times the gain and each
// note-off releases one unit, forking a releasing twin when fewer than n units
// are released. The contract is: it sounds exactly like n separate note-ons
// released one by one. Three renders of one randomized script prove it:
//   ref     n separate launches                    (frame-major oracle)
//   mOracle one merged launch                      (frame-major oracle)
//   mWhole  one merged launch                      (whole-voice renderer)
// ═══════════════════════════════════════════════════════════════════════

struct MergeScenarioContext {
    svms::VoiceManager* voices;
    svms::ChannelCache* channels;
    const svms::ChannelParamsSnapshot* channelParams;
    bool merged = false;
    uint32_t playIndex = 20000u;
    uint32_t launchedUnits = 0u;
    // Re-excite scenarios: per-key launch gain, so steal victims are the
    // quietest keys in both renderers whatever the envelope stage.
    bool keyScaledGain = false;
};

void DispatchMergeScenarioEvent(const svms::RenderEvent* events,
                                uint32_t eventCount, uint32_t,
                                void* userData) {
    auto& context = *static_cast<MergeScenarioContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        const uint8_t channel = event.channel;
        switch (event.type) {
            case svms::RenderEventType::NoteReExcite:
                // Driver behaviour: restart what sounds on the key, launch
                // normally when nothing does.
                if (context.voices->ReExciteKey(channel, event.data1) != 0u)
                    break;
                [[fallthrough]];
            case svms::RenderEventType::NoteOn: {
                const uint32_t units = event.count > 1u ? event.count : 1u;
                // Looping and one-shot regions, transient envelope, capped
                // release: the same mix the other whole-voice scenarios use.
                const bool loop = ((event.data1 + channel) % 3u) != 0u;
                svms::VoiceConfiguration setup{};
                setup.sampleStart = 0u;
                setup.sampleEnd = 4096u;
                setup.loopStart = 64u;
                setup.loopEnd = 4032u;
                setup.loopMode = loop ? 1u : 0u;
                // One-shot regions crawl so none reaches its sample end
                // inside the scenario: whole-voice retirements land at block
                // end, so a mid-block natural retirement would leave the dead
                // generation in the key chain for later same-block note-offs
                // (the documented whole-voice drift, independent of merging).
                setup.phaseStep = setup.basePhaseStep = loop
                    ? 0.637f + event.data1 * 0.011f
                    : 0.06f + event.data1 * 0.004f;
                setup.initialGain = context.keyScaledGain
                    ? 0.05f + 0.01f * static_cast<float>(event.data1 - 60u)
                    : 0.6f;
                setup.sustainLevel = 0.5f;
                setup.attackSamples = 17u;
                setup.attackGainStep = 0.6f / 17.0f;
                setup.decaySamples = 53u;
                setup.decaySlope = 0.99f;
                setup.releaseDecay = 0.9995f;
                setup.releaseSamples = 400u;
                setup.gainLeft = setup.gainRight = 0.02f;
                setup.sampleBacked = 1u;
                const uint32_t launches = context.merged ? 1u : units;
                for (uint32_t launch = 0u; launch < launches; ++launch) {
                    setup.playIndex = context.playIndex++;
                    svms::VoiceHandle handle = svms::kInvalidVoice;
                    context.voices->LaunchVoiceGroup(
                        channel, event.data1, event.data2, &setup, 1u,
                        setup.playIndex, context.channelParams[channel],
                        &handle, 0u, context.merged ? units : 1u);
                }
                context.launchedUnits += units;
                break;
            }
            case svms::RenderEventType::NoteOff: {
                const bool sustain = context.channels->IsSustainActive(channel);
                context.channels->NoteOff(channel, event.data1);
                context.voices->NoteOffOldestPlayIndices(
                    channel, event.data1, 1u, sustain, event.frameOffset);
                break;
            }
            case svms::RenderEventType::ControlChange: {
                if (event.data1 != 64u) break;
                const bool was = context.channels->IsSustainActive(channel);
                context.channels->ControlChange(channel, 64u, event.data2);
                if (was && !context.channels->IsSustainActive(channel)) {
                    context.voices->ReleaseSustain(channel, event.frameOffset);
                }
                break;
            }
            default:
                break;
        }
    }
}

void TestMergedNoteExactness() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 14u;
    constexpr uint32_t kPool = 512u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f) +
             0.12f * std::cos(static_cast<float>(i) * 0.19f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        const char* backendName = avx2Pass ? "avx2" : "scalar";

        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto refVoices = std::make_unique<svms::VoiceManager>();
        Check(refVoices->Initialize(kPool, 44100u), "merge test pool");
        auto mOracleVoices = std::make_unique<svms::VoiceManager>(*refVoices);
        auto mWholeVoices = std::make_unique<svms::VoiceManager>(*refVoices);
        svms::ChannelCache refChannels = seedChannels;
        svms::ChannelCache mOracleChannels = seedChannels;
        svms::ChannelCache mWholeChannels = seedChannels;

        svms::RenderScalar refRenderer;
        svms::RenderScalar oracleRenderer;
        svms::RenderScalar wholeRenderer;
        Check(refRenderer.ReserveVoiceCapacity(kPool) &&
                  oracleRenderer.ReserveVoiceCapacity(kPool) &&
                  wholeRenderer.ReserveVoiceCapacity(kPool) &&
                  refRenderer.ConfigureRenderThreads(1u, kFrames) &&
                  oracleRenderer.ConfigureRenderThreads(1u, kFrames) &&
                  wholeRenderer.ConfigureRenderThreads(4u, kFrames),
              "merge test starts its renderers");
        Check(wholeRenderer.SetRenderBackend(
                  avx2Pass ? svms::RenderBackend::AVX2
                           : svms::RenderBackend::Scalar),
              "merge test selects the requested backend");

        MergeScenarioContext refContext{refVoices.get(), &refChannels,
                                        refChannels.GetParams(), false};
        MergeScenarioContext oracleContext{mOracleVoices.get(),
                                           &mOracleChannels,
                                           mOracleChannels.GetParams(), true};
        MergeScenarioContext wholeContext{mWholeVoices.get(), &mWholeChannels,
                                          mWholeChannels.GetParams(), true};
        refRenderer.SetEventBatchDispatcher(DispatchMergeScenarioEvent,
                                            &refContext);
        oracleRenderer.SetEventBatchDispatcher(DispatchMergeScenarioEvent,
                                               &oracleContext);
        wholeRenderer.SetEventBatchDispatcher(DispatchMergeScenarioEvent,
                                              &wholeContext);

        // Outstanding (unreleased) units per key bound the note-offs, exactly
        // like a well-formed MIDI stream. Fresh PRNG per pass.
        WholeVoiceRng rng;
        uint32_t outstanding[2][6] = {};
        bool pedalDown[2] = {false, false};
        float refVsMerged = 0.0f;
        float refVsWhole = 0.0f;
        float oracleVsWhole = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 10000u + block * kFrames;
            std::vector<svms::RenderEvent> events;
            auto push = [&](svms::RenderEventType type, uint8_t channel,
                            uint8_t key, uint8_t value, uint32_t frame,
                            uint32_t count) {
                svms::RenderEvent event{};
                event.type = type;
                event.channel = channel;
                event.data1 = key;
                event.data2 = value;
                event.frameOffset = frame;
                event.count = count;
                events.push_back(event);
            };
            // Launch phase: the first blocks add merged notes; same-frame
            // multi-key bursts and re-attacks of keys still sounding.
            if (block < 8u) {
                const uint32_t launches = 2u + rng.Next() % 3u;
                for (uint32_t j = 0; j < launches; ++j) {
                    const uint8_t channel = static_cast<uint8_t>(rng.Next() % 2u);
                    const uint32_t k = rng.Next() % 6u;
                    const uint32_t units = 1u + rng.Next() % 6u;
                    // Offs for this key's earlier units must not be issued in
                    // the same block as the launch ahead of it, so launches
                    // are early in the block and offs late.
                    push(svms::RenderEventType::NoteOn, channel,
                         static_cast<uint8_t>(60u + k * 2u),
                         static_cast<uint8_t>(70u + rng.Next() % 50u),
                         (rng.Next() % 3u) * 8u, units);
                    outstanding[channel][k] += units;
                }
            }
            // Pedal toggles (held twins must release on pedal-up).
            if (block == 3u || block == 9u) {
                push(svms::RenderEventType::ControlChange, 0u, 64u, 127u, 150u, 1u);
                pedalDown[0] = true;
            }
            if (block == 6u || block == 11u) {
                push(svms::RenderEventType::ControlChange, 0u, 64u, 0u, 420u, 1u);
                pedalDown[0] = false;
            }
            // Release phase: drain every key's units in random partial steps
            // scattered over the block (partial releases fork twins).
            for (uint32_t channel = 0; channel < 2u; ++channel) {
                for (uint32_t k = 0; k < 6u; ++k) {
                    if (block < 2u || outstanding[channel][k] == 0u) continue;
                    if (rng.Next() % 3u == 0u) continue;
                    uint32_t offs = 1u + rng.Next() % 3u;
                    if (offs > outstanding[channel][k])
                        offs = outstanding[channel][k];
                    // Offs are distinct events: some share a frame (as a
                    // batched burst would), some are spread out.
                    const uint32_t burstFrame = 60u + rng.Next() % 380u;
                    for (uint32_t o = 0; o < offs; ++o) {
                        const uint32_t frame = rng.Next() % 2u
                            ? burstFrame : 60u + rng.Next() % 380u;
                        push(svms::RenderEventType::NoteOff,
                             static_cast<uint8_t>(channel),
                             static_cast<uint8_t>(60u + k * 2u), 0u, frame, 1u);
                    }
                    outstanding[channel][k] -= offs;
                }
            }
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });
            for (uint32_t i = 0; i < events.size(); ++i)
                events[i].ingressSequence = i;

            std::vector<float> refL(kFrames, 0.0f), refR(kFrames, 0.0f);
            std::vector<float> oraL(kFrames, 0.0f), oraR(kFrames, 0.0f);
            std::vector<float> whoL(kFrames, 0.0f), whoR(kFrames, 0.0f);
            refRenderer.RenderBlockReference(
                *refVoices, refChannels, samples.data(), sampleCount,
                refL.data(), refR.data(), kFrames, cfg, events.data(),
                static_cast<uint32_t>(events.size()), true, blockStartFrame);
            oracleRenderer.RenderBlockReference(
                *mOracleVoices, mOracleChannels, samples.data(), sampleCount,
                oraL.data(), oraR.data(), kFrames, cfg, events.data(),
                static_cast<uint32_t>(events.size()), true, blockStartFrame);
            wholeRenderer.RenderBlock(
                *mWholeVoices, mWholeChannels, samples.data(), sampleCount,
                whoL.data(), whoR.data(), kFrames, cfg, events.data(),
                static_cast<uint32_t>(events.size()), true, blockStartFrame);

            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                refVsMerged = (std::max)(refVsMerged, std::fabs(refL[frame] - oraL[frame]));
                refVsMerged = (std::max)(refVsMerged, std::fabs(refR[frame] - oraR[frame]));
                refVsWhole = (std::max)(refVsWhole, std::fabs(refL[frame] - whoL[frame]));
                refVsWhole = (std::max)(refVsWhole, std::fabs(refR[frame] - whoR[frame]));
                oracleVsWhole = (std::max)(oracleVsWhole, std::fabs(oraL[frame] - whoL[frame]));
                oracleVsWhole = (std::max)(oracleVsWhole, std::fabs(oraR[frame] - whoR[frame]));
            }
            if (std::getenv("SVMS_MERGE_DEBUG")) {
                float d = 0.0f;
                uint32_t worstFrame = 0u;
                for (uint32_t frame = 0; frame < kFrames; ++frame) {
                    const float e = std::fabs(oraL[frame] - whoL[frame]);
                    if (e > d) { d = e; worstFrame = frame; }
                }
                if (d > 1.0e-6f) {
                    CheckWholeVoiceStateParity(*mOracleVoices, *mWholeVoices,
                                               "debug-block");
                    auto dump = [](const char* label,
                                   const svms::VoiceManager& vm) {
                        std::vector<std::array<uint32_t, 4>> rows;
                        for (uint32_t i = 0; i < vm.activeCount_; ++i) {
                            const uint32_t h = vm.activeList_[i];
                            rows.push_back({vm.v.playIndex[h], vm.v.mult[h],
                                            vm.v.state[h],
                                            vm.v.releaseSamplesRemaining[h]});
                        }
                        std::sort(rows.begin(), rows.end());
                        std::fprintf(stderr, "[MERGEDBG] %s:", label);
                        for (auto& r : rows)
                            std::fprintf(stderr, " (p%u m%u s%u r%u)", r[0],
                                         r[1], r[2], r[3]);
                        std::fprintf(stderr, "\n");
                    };
                    dump("oracle", *mOracleVoices);
                    dump("whole ", *mWholeVoices);
                    std::fprintf(stderr, "[MERGEDBG] worst frame=%u events:", worstFrame);
                    for (const auto& e : events)
                        std::fprintf(stderr, " [t%u ch%u k%u v%u f%u n%u]",
                                     (unsigned)e.type, e.channel, e.data1,
                                     e.data2, e.frameOffset, e.count);
                    std::fprintf(stderr, "\n");
                }
                std::fprintf(stderr,
                    "[MERGEDBG] %s block=%u events=%zu oracle-vs-whole=%g "
                    "effective ref=%u oracle=%llu whole=%llu active whole=%u "
                    "forkFail oracle=%u whole=%u\n",
                    backendName, block, events.size(), d,
                    refVoices->activeCount_,
                    (unsigned long long)mOracleVoices->CountEffectiveVoices(),
                    (unsigned long long)mWholeVoices->CountEffectiveVoices(),
                    mWholeVoices->activeCount_,
                    mOracleVoices->GetForkFailureCount(),
                    mWholeVoices->GetForkFailureCount());
            }
            // The merged pool must hold exactly as many units as the
            // unmerged pool holds voices, block after block.
            Check(mOracleVoices->CountEffectiveVoices() == refVoices->activeCount_,
                  "merged oracle pool accounts for every unmerged voice");
            Check(mWholeVoices->CountEffectiveVoices() == refVoices->activeCount_,
                  "merged whole-voice pool accounts for every unmerged voice");
        }
        std::fprintf(stderr,
                     "[MERGEDIFF] backend=%s ref-vs-mergedOracle=%g "
                     "ref-vs-mergedWhole=%g oracle-vs-whole=%g forks=%u "
                     "active=%u/%u\n",
                     backendName, refVsMerged, refVsWhole, oracleVsWhole,
                     mOracleVoices->GetForkCount(),
                     mOracleVoices->activeCount_, refVoices->activeCount_);
        Check(refVsMerged <= 2.0e-5f,
              "merged note-ons sound exactly like the same notes launched one by one");
        Check(refVsWhole <= 2.0e-5f && oracleVsWhole <= 2.0e-5f,
              "merged note-ons render identically in the whole-voice renderer");
        Check(mOracleVoices->GetForkCount() != 0u,
              "merge scenario exercises partial-release forks");
        Check(mOracleVoices->stealCount_ == 0u && refVoices->stealCount_ == 0u,
              "merge scenario never exhausts the pool");
        CheckWholeVoiceStateParity(*mOracleVoices, *mWholeVoices, backendName);
        Check(wholeRenderer.GetWholeVoiceBlocksForTest() == kBlocks,
              "merge scenario routes every block through the whole-voice renderer");
        (void)pedalDown;
    }
}

svms::ScheduledRenderEvent MakeMergeEvent(int64_t frame, uint32_t sequence,
                                          svms::RenderEventType type,
                                          uint8_t channel, uint8_t key,
                                          uint8_t value) {
    svms::ScheduledRenderEvent event{};
    event.targetFrame = frame;
    event.sequence = sequence;
    event.type = type;
    event.channel = channel;
    event.data1 = key;
    event.data2 = value;
    return event;
}

void TestNoteMergerSemantics() {
    using svms::RenderEventType;
    const auto on = [](int64_t frame, uint32_t seq, uint8_t ch, uint8_t key,
                       uint8_t vel) {
        return MakeMergeEvent(frame, seq, RenderEventType::NoteOn, ch, key, vel);
    };
    const auto off = [](int64_t frame, uint32_t seq, uint8_t ch, uint8_t key) {
        return MakeMergeEvent(frame, seq, RenderEventType::NoteOff, ch, key, 0u);
    };
    const auto cc = [](int64_t frame, uint32_t seq, uint8_t ch) {
        return MakeMergeEvent(frame, seq, RenderEventType::ControlChange, ch,
                              7u, 100u);
    };
    svms::NoteMerger merger;
    uint64_t removed = 0u;
    const auto run = [&](std::vector<svms::ScheduledRenderEvent>& events,
                         bool hoist) {
        removed = 0u;
        events.resize(merger.Merge(events.data(),
                                   static_cast<uint32_t>(events.size()),
                                   hoist, removed));
    };

    {   // Plain run of identical note-ons folds into the first.
        std::vector<svms::ScheduledRenderEvent> e = {
            on(100, 1, 0, 60, 100), on(100, 2, 0, 60, 100),
            on(100, 3, 0, 60, 100)};
        run(e, true);
        Check(e.size() == 1u && removed == 2u && e[0].sequence == 1u &&
                  e[0].IsMergedNoteOn() && e[0].NoteCount() == 3u &&
                  e[0].BaseType() == RenderEventType::NoteOn,
              "identical same-frame note-ons fold into the first one");
        const svms::RenderEvent expanded = e[0].ToRenderEvent(7u);
        Check(expanded.type == RenderEventType::NoteOn && expanded.count == 3u &&
                  expanded.channel == 0u && expanded.data1 == 60u &&
                  expanded.data2 == 100u && expanded.frameOffset == 7u &&
                  expanded.ingressSequence == 1u,
              "a merged event expands to a NoteOn carrying its count");
        svms::ScheduledRenderEvent back{};
        back.SetRenderEvent(expanded);
        Check(back.NoteCount() == 3u && back.sequence == 1u,
              "a merged event survives the RenderEvent round trip");
    }
    {   // An unrelated key between the duplicates: hoisting merges across it,
        // strict adjacency does not.
        std::vector<svms::ScheduledRenderEvent> e = {
            on(100, 1, 0, 60, 100), on(100, 2, 0, 62, 100),
            on(100, 3, 0, 60, 100)};
        auto strict = e;
        run(e, true);
        Check(e.size() == 2u && e[0].NoteCount() == 2u && e[1].data1 == 62u,
              "hoisting merges duplicates across a different key");
        run(strict, false);
        Check(strict.size() == 3u && removed == 0u,
              "strict adjacency refuses to merge across a different key");
    }
    {   // Only exact duplicates: velocity, channel and frame must all match.
        std::vector<svms::ScheduledRenderEvent> e = {
            on(100, 1, 0, 60, 100), on(100, 2, 0, 60, 101),
            on(100, 3, 1, 60, 100), on(101, 4, 0, 60, 100)};
        run(e, true);
        Check(e.size() == 4u && removed == 0u,
              "differing velocity, channel or frame never merges");
    }
    {   // Barriers: anything that could change what a launch sees.
        std::vector<svms::ScheduledRenderEvent> offBetween = {
            on(100, 1, 0, 60, 100), off(100, 2, 0, 60), on(100, 3, 0, 60, 100)};
        run(offBetween, true);
        Check(offBetween.size() == 3u, "a note-off ends the merge window");
        std::vector<svms::ScheduledRenderEvent> ccSame = {
            on(100, 1, 0, 60, 100), cc(100, 2, 0), on(100, 3, 0, 60, 100)};
        run(ccSame, true);
        Check(ccSame.size() == 3u,
              "a controller on the same channel ends the merge window");
        std::vector<svms::ScheduledRenderEvent> ccOther = {
            on(100, 1, 0, 60, 100), cc(100, 2, 1), on(100, 3, 0, 60, 100)};
        run(ccOther, true);
        Check(ccOther.size() == 2u && ccOther[0].NoteCount() == 2u,
              "a controller on another channel leaves the window open");
        std::vector<svms::ScheduledRenderEvent> reset = {
            on(100, 1, 0, 60, 100),
            MakeMergeEvent(100, 2, RenderEventType::Reset, 0u, 0u, 0u),
            on(100, 3, 0, 60, 100)};
        run(reset, true);
        Check(reset.size() == 3u, "a reset ends every merge window");
    }
    {   // The offline/prerender entry point merges RenderEvent blocks the same
        // way, with the count in RenderEvent::count.
        std::vector<svms::RenderEvent> block;
        const auto renderOn = [](uint32_t frame, uint8_t key, uint8_t vel) {
            svms::RenderEvent event{};
            event.type = RenderEventType::NoteOn;
            event.channel = 3u;
            event.data1 = key;
            event.data2 = vel;
            event.frameOffset = frame;
            return event;
        };
        block = {renderOn(10, 60, 90), renderOn(10, 62, 90),
                 renderOn(10, 60, 90), renderOn(10, 60, 90),
                 renderOn(11, 60, 90)};
        uint64_t folded = 0u;
        const uint32_t kept = merger.MergeEvents(
            block.data(), static_cast<uint32_t>(block.size()), true, folded);
        Check(kept == 3u && folded == 2u && block[0].count == 3u &&
                  block[1].data1 == 62u && block[1].count == 1u &&
                  block[2].frameOffset == 11u && block[2].count == 1u,
              "RenderEvent blocks merge like compiled pages");
        const uint32_t again = merger.MergeEvents(block.data(), kept, true,
                                                  folded);
        Check(again == kept && folded == 2u,
              "an already merged block is left alone");
    }
    {   // The count rides in one byte: long runs split into several events.
        std::vector<svms::ScheduledRenderEvent> e;
        for (uint32_t i = 0; i < 600u; ++i) e.push_back(on(50, i, 2, 64, 90));
        run(e, true);
        uint32_t total = 0u;
        bool withinCap = true;
        for (const auto& event : e) {
            total += event.NoteCount();
            withinCap &= event.NoteCount() <= svms::kMergedNoteOnMaxCount;
        }
        Check(total == 600u && withinCap && e.size() == 3u && removed == 597u,
              "an oversized run splits into events within the count cap");
    }
}

// A partial release in a FULL pool must fork its twin by stealing some other
// voice, never the voice being split.
void TestMergedForkInFullPool() {
    constexpr uint32_t kPool = 4u;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    svms::VoiceManager voices;
    Check(voices.Initialize(kPool, 44100u), "full-pool fork test allocates");

    const auto launch = [&](uint8_t key, uint32_t playIndex, float gain,
                            uint32_t units) {
        svms::VoiceConfiguration setup{};
        setup.sampleEnd = 4096u;
        setup.loopStart = 64u;
        setup.loopEnd = 4032u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep = 1.0f;
        setup.initialGain = gain;
        setup.sustainLevel = 1.0f;
        setup.decaySlope = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.02f;
        setup.sampleBacked = 1u;
        setup.playIndex = playIndex;
        svms::VoiceHandle handle = svms::kInvalidVoice;
        voices.LaunchVoiceGroup(0u, key, 100u, &setup, 1u, playIndex,
                                channels.GetParams()[0], &handle, 0u, units);
        return handle;
    };
    // The merged voice is the quietest candidate, i.e. exactly the voice a
    // naive allocation would steal.
    const svms::VoiceHandle merged = launch(60u, 1u, 0.01f, 3u);
    launch(62u, 2u, 0.9f, 1u);
    launch(64u, 3u, 0.8f, 1u);
    launch(65u, 4u, 0.7f, 1u);
    Check(voices.activeCount_ == kPool && voices.freeTop_ == 0u,
          "full-pool fork test fills the pool");
    Check(voices.GetVoiceMultiplicity(merged) == 3u,
          "a merged launch records its multiplicity");

    const uint64_t effectiveBefore = voices.CountEffectiveVoices();
    const uint32_t released = voices.NoteOffOldestPlayIndices(0u, 60u, 1u,
                                                              false, 0u);
    Check(released == 1u && voices.GetForkFailureCount() == 0u &&
              voices.GetForkCount() == 1u,
          "a partial release forks even when the pool is full");
    Check(voices.v.state[merged] == static_cast<uint8_t>(svms::VoiceState::Active) &&
              voices.GetVoiceMultiplicity(merged) == 2u &&
              voices.v.playIndex[merged] == 1u,
          "the split voice survives with the remaining units");
    uint32_t releasingTwins = 0u;
    for (uint32_t i = 0; i < voices.activeCount_; ++i) {
        const uint32_t h = voices.activeList_[i];
        if (voices.v.state[h] == static_cast<uint8_t>(svms::VoiceState::Releasing) &&
            voices.v.note[h] == 60u && voices.GetVoiceMultiplicity(h) == 1u) {
            ++releasingTwins;
        }
    }
    Check(releasingTwins == 1u && voices.stealCount_ == 1u &&
              voices.activeCount_ == kPool,
          "the twin releases one unit and exactly one other voice was stolen");
    // 3 units + 3 singles became 2 + 1 (twin) + 2 surviving singles.
    Check(voices.CountEffectiveVoices() == effectiveBefore - 1u,
          "only the stolen voice's unit left the pool");
    // The remaining two units release together.
    Check(voices.NoteOffOldestPlayIndices(0u, 60u, 5u, false, 0u) == 2u,
          "surplus note-offs beyond the outstanding units are ignored");
}

// ═══════════════════════════════════════════════════════════════════════
// Note-on re-excite (VoiceManager::ReExciteKey + RenderEventType::NoteReExcite).
// ═══════════════════════════════════════════════════════════════════════

void TestReExciteKey() {
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    svms::VoiceManager voices;
    Check(voices.Initialize(16u, 44100u), "re-excite test allocates");

    const auto launch = [&](uint8_t key, uint32_t playIndex) {
        svms::VoiceConfiguration setup{};
        setup.sampleEnd = 4096u;
        setup.loopStart = 64u;
        setup.loopEnd = 4032u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep = 1.0f;
        setup.initialGain = 1.0f;
        setup.sustainLevel = 0.25f;
        setup.decaySamples = 100000u;
        setup.decaySlope = 0.9995f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.02f;
        setup.sampleBacked = 1u;
        setup.playIndex = playIndex;
        svms::VoiceHandle handle = svms::kInvalidVoice;
        voices.LaunchVoiceGroup(0u, key, 100u, &setup, 1u, playIndex,
                                channels.GetParams()[0], &handle, 0u, 1u);
        return handle;
    };

    const svms::VoiceHandle decaying = launch(60u, 1u);
    Check(voices.v.envelopeStage[decaying] == 2u,
          "the test voice launches in its decay stage");
    voices.v.currentGain[decaying] = 0.5f;
    Check(voices.ReExciteKey(0u, 60u) == 1u,
          "re-excite finds the live voice on the key");
    const uint32_t ramp = voices.ReExciteAttackFrames();
    Check(ramp == 44100u / 250u, "the re-excite ramp is 4 ms of frames");
    Check(voices.v.envelopeStage[decaying] == 1u &&
              voices.v.attackSamplesRemaining[decaying] == ramp,
          "a decayed voice re-enters a short attack");
    Check(std::fabs(voices.v.attackGainStep[decaying] *
                        static_cast<float>(ramp) - 0.5f) < 1.0e-5f,
          "the attack ramps from the current level back to the peak (no step)");
    // ln(0.25) / ln(0.9995) frames for the decay to fall to sustain.
    const float expectedDecay = std::log(0.25f) / std::log(0.9995f);
    Check(std::fabs(static_cast<float>(voices.v.decaySamplesRemaining[decaying]) -
                    expectedDecay) <= 2.0f,
          "the decay restarts from the peak with its full length");

    // A voice sitting at its sustain level (class SustainedLoop) moves to the
    // transient class when excited, and back is the renderer's business.
    const svms::VoiceHandle sustained = launch(62u, 2u);
    voices.v.envelopeStage[sustained] = 3u;
    voices.v.currentGain[sustained] = 0.25f;
    voices.RefreshRenderClass(sustained);
    Check(voices.v.renderClass[sustained] ==
              static_cast<uint8_t>(svms::VoiceRenderClass::SustainedLoop),
          "a sustained voice starts in the sustained class");
    Check(voices.ReExciteKey(0u, 62u) == 1u &&
              voices.v.envelopeStage[sustained] == 1u &&
              voices.v.renderClass[sustained] ==
                  static_cast<uint8_t>(svms::VoiceRenderClass::TransientLoop),
          "re-exciting a sustained voice relinks it to the transient class");

    // Already attacking: nothing to do, still counted as sounding.
    const uint32_t before = voices.v.attackSamplesRemaining[sustained];
    Check(voices.ReExciteKey(0u, 62u) == 1u &&
              voices.v.attackSamplesRemaining[sustained] == before,
          "a voice that is still attacking is left as it is");

    // Releasing voices are not on the key chain: no excite, caller launches.
    const svms::VoiceHandle releasing = launch(64u, 3u);
    voices.NoteOffOldestPlayIndices(0u, 64u, 1u, false, 0u);
    Check(voices.v.state[releasing] ==
              static_cast<uint8_t>(svms::VoiceState::Releasing) &&
              voices.ReExciteKey(0u, 64u) == 0u,
          "a released voice is never re-excited");
    Check(voices.ReExciteKey(0u, 90u) == 0u && voices.ReExciteKey(3u, 60u) == 0u,
          "keys with no live voice report nothing to excite");

    // Ingress thinning: one kept hit per attack ramp per key; a spawn counts
    // as a kept hit too, and keys are independent.
    {
        svms::NoteOnCollapseGate gate;
        gate.SetReExciteSpacing(100u);
        Check(gate.AllowReExcite(5u, 1000u),
              "the first hit on a key is kept");
        Check(!gate.AllowReExcite(5u, 1099u),
              "a hit inside the ramp of the last kept hit is dropped");
        Check(gate.AllowReExcite(5u, 1100u),
              "a hit one ramp after the last kept hit is kept");
        Check(gate.AllowReExcite(6u, 1101u),
              "another key is independent");
        gate.SetWindowTicks(20000u);
        gate.SetThreshold(32u);
        uint32_t stack = 0u;
        Check(gate.OnNoteOn(7u, 5000u, stack),
              "the gate spawns the first note-on of a key");
        Check(!gate.AllowReExcite(7u, 5050u) && gate.AllowReExcite(7u, 5100u),
              "a spawn counts as a kept hit for the thinning");
        gate.ResetKey(5u);
        Check(gate.AllowReExcite(5u, 1101u),
              "resetting a key forgets its last kept hit");
    }

    // The ingress tag compiles to the dedicated event type, and only for a
    // real note-on (a tagged velocity-0 message is still a note-off).
    {
        svms::TimestampedMidiEvent timed{};
        svms::ScheduledRenderEvent scheduled{};
        const uint32_t base = 0x90u | (60u << 8) | (100u << 16);
        timed.message = base | svms::kReExciteMessageTag;
        Check(svms::CompileTimestampedEvent(timed, 0u, 10000000u, 44100u, 0u,
                                            scheduled) &&
                  scheduled.type == svms::RenderEventType::NoteReExcite &&
                  scheduled.data1 == 60u && scheduled.data2 == 100u,
              "a tagged note-on compiles to NoteReExcite");
        timed.message = base;
        svms::CompileTimestampedEvent(timed, 0u, 10000000u, 44100u, 0u, scheduled);
        Check(scheduled.type == svms::RenderEventType::NoteOn,
              "an untagged note-on still compiles to NoteOn");
        timed.message = (0x90u | (60u << 8)) | svms::kReExciteMessageTag;
        svms::CompileTimestampedEvent(timed, 0u, 10000000u, 44100u, 0u, scheduled);
        Check(scheduled.type == svms::RenderEventType::NoteOff,
              "a tagged velocity-0 message is still a note-off");
    }
}

// A renderer fed NoteReExcite events must match the frame-major oracle
// exactly: the span renderer splits at each excite frame, the whole-voice
// and dense planners refuse the block, and the audio is the same.
// scenario 0: random hits on a roomy pool; 1: scripted two-voice pool where
// excited voices are stolen (ghosts) and hits land on stolen/released keys;
// 2: merged note-ons (forked twins must hear the hits their source took).
void TestReExciteRenderDifferential(uint32_t kPool, int scenario) {
    const bool churn = scenario == 1;
    const bool mergeScenario = scenario == 2;
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 10u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f) +
             0.12f * std::cos(static_cast<float>(i) * 0.19f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        const char* backendName = avx2Pass ? "avx2" : "scalar";
        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto refVoices = std::make_unique<svms::VoiceManager>();
        Check(refVoices->Initialize(kPool, 44100u), "re-excite diff pool");
        auto prodVoices = std::make_unique<svms::VoiceManager>(*refVoices);
        svms::ChannelCache refChannels = seedChannels;
        svms::ChannelCache prodChannels = seedChannels;
        svms::RenderScalar refRenderer;
        svms::RenderScalar prodRenderer;
        Check(refRenderer.ReserveVoiceCapacity(kPool) &&
                  prodRenderer.ReserveVoiceCapacity(kPool) &&
                  refRenderer.ConfigureRenderThreads(1u, kFrames) &&
                  prodRenderer.ConfigureRenderThreads(4u, kFrames),
              "re-excite diff starts its renderers");
        Check(prodRenderer.SetRenderBackend(
                  avx2Pass ? svms::RenderBackend::AVX2
                           : svms::RenderBackend::Scalar),
              "re-excite diff selects the backend");
        MergeScenarioContext refContext{refVoices.get(), &refChannels,
                                        refChannels.GetParams(), false};
        MergeScenarioContext prodContext{prodVoices.get(), &prodChannels,
                                         prodChannels.GetParams(),
                                         mergeScenario};
        refContext.keyScaledGain = prodContext.keyScaledGain = churn;
        refRenderer.SetEventBatchDispatcher(DispatchMergeScenarioEvent,
                                            &refContext);
        prodRenderer.SetEventBatchDispatcher(DispatchMergeScenarioEvent,
                                             &prodContext);

        WholeVoiceRng rng;
        float worst = 0.0f;
        uint32_t exciteBlocks = 0u;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 10000u + block * kFrames;
            std::vector<svms::RenderEvent> events;
            auto push = [&](svms::RenderEventType type, uint8_t key,
                            uint8_t value, uint32_t frame,
                            uint32_t count = 1u) {
                svms::RenderEvent event{};
                event.type = type;
                event.channel = 0u;
                event.data1 = key;
                event.data2 = value;
                event.frameOffset = frame;
                event.count = count;
                events.push_back(event);
            };
            if (mergeScenario) {
                using T = svms::RenderEventType;
                if (block == 0u) {
                    push(T::NoteOn, 60u, 100u, 0u, 4u);
                    push(T::NoteOn, 62u, 90u, 3u, 3u);
                } else if (block == 1u) {
                    push(T::NoteReExcite, 60u, 90u, 100u);
                    push(T::NoteOff, 60u, 0u, 200u);        // forks a twin
                    push(T::NoteReExcite, 60u, 90u, 250u);  // remaining units only
                    push(T::NoteReExcite, 62u, 90u, 300u);
                    push(T::NoteOff, 62u, 0u, 400u);
                    push(T::NoteOff, 62u, 0u, 400u);
                } else if (block == 2u) {
                    push(T::NoteReExcite, 60u, 90u, 40u);
                    push(T::NoteReExcite, 62u, 90u, 60u);
                    push(T::NoteOff, 60u, 0u, 200u);
                    push(T::NoteReExcite, 62u, 90u, 260u);
                } else if (block == 3u) {
                    push(T::NoteOff, 60u, 0u, 100u);
                    push(T::NoteOff, 60u, 0u, 100u);
                }
                if (block != 0u) ++exciteBlocks;
            } else if (block == 0u) {
                const uint32_t launchKeys = churn ? 2u : 5u;
                for (uint32_t k = 0; k < launchKeys; ++k)
                    push(svms::RenderEventType::NoteOn,
                         static_cast<uint8_t>(60u + k * 2u),
                         static_cast<uint8_t>(80u + k * 5u), k * 3u);
            } else if (churn) {
                // Scripted two-voice pool: excited voices get stolen (ghosts
                // that already took hits), hits land on stolen keys (launch
                // fallback) and on releasing keys. Per-key gains make the
                // victim the quietest key in both renderers.
                using T = svms::RenderEventType;
                if (block == 1u) {
                    push(T::NoteReExcite, 60u, 90u, 100u);
                    push(T::NoteOn, 64u, 100u, 200u);    // steals key 60 (ghost)
                    push(T::NoteReExcite, 60u, 90u, 300u);  // nothing live: launches, steals 62
                    push(T::NoteReExcite, 64u, 90u, 350u);
                    push(T::NoteOff, 60u, 0u, 450u);
                } else if (block == 2u) {
                    push(T::NoteReExcite, 64u, 90u, 40u);
                    push(T::NoteReExcite, 60u, 90u, 60u);   // 60 is releasing: launches
                    push(T::NoteOn, 66u, 100u, 300u);
                    push(T::NoteReExcite, 66u, 90u, 320u);
                    push(T::NoteReExcite, 66u, 90u, 330u);
                }
                ++exciteBlocks;
            } else {
                // Repeat hits on sounding keys and a release in the middle.
                const uint32_t hits = 4u + rng.Next() % 8u;
                for (uint32_t h = 0; h < hits; ++h) {
                    const uint32_t k = rng.Next() % 6u;
                    push(svms::RenderEventType::NoteReExcite,
                         static_cast<uint8_t>(60u + k * 2u),
                         static_cast<uint8_t>(90u), 5u + rng.Next() % 500u);
                }
                if (block == 4u)
                    push(svms::RenderEventType::NoteOff, 62u, 0u, 200u);
                ++exciteBlocks;
            }
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });
            for (uint32_t i = 0; i < events.size(); ++i)
                events[i].ingressSequence = i;

            std::vector<float> refL(kFrames, 0.0f), refR(kFrames, 0.0f);
            std::vector<float> prodL(kFrames, 0.0f), prodR(kFrames, 0.0f);
            refRenderer.RenderBlockReference(
                *refVoices, refChannels, samples.data(), sampleCount,
                refL.data(), refR.data(), kFrames, cfg, events.data(),
                static_cast<uint32_t>(events.size()), true, blockStartFrame);
            prodRenderer.RenderBlock(
                *prodVoices, prodChannels, samples.data(), sampleCount,
                prodL.data(), prodR.data(), kFrames, cfg, events.data(),
                static_cast<uint32_t>(events.size()), true, blockStartFrame);
            if (block != 0u) {
                Check((prodRenderer.GetLastRenderPaths() & 0x1u) != 0u,
                      "a block carrying re-excite events renders on the whole-voice path");
            }
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                worst = (std::max)(worst, std::fabs(refL[frame] - prodL[frame]));
                worst = (std::max)(worst, std::fabs(refR[frame] - prodR[frame]));
            }
        }
        std::fprintf(stderr, "[REEXCITEDIFF] pool=%u backend=%s "
                     "oracle-vs-wholevoice=%g excite-blocks=%u active=%u/%u "
                     "steals=%u/%u\n", kPool, backendName, worst,
                     exciteBlocks, prodVoices->activeCount_,
                     refVoices->activeCount_, prodVoices->stealCount_,
                     refVoices->stealCount_);
        Check(worst <= 2.0e-5f,
              "re-excited voices render exactly like the frame-major oracle");
        Check(mergeScenario ||
                  prodVoices->activeCount_ == refVoices->activeCount_,
              "re-excite leaves the same voices alive in both renderers");
        Check(exciteBlocks == kBlocks - 1u,
              "re-excite scenario sends excite events in every block after the launch");
        if (mergeScenario) {
            Check(prodVoices->GetForkCount() != 0u,
                  "the merge scenario forks twins under re-excite");
        }
        if (churn) {
            Check(refVoices->stealCount_ != 0u,
                  "the churn scenario forces steals (ghosts) under re-excite");
        }
        if (!mergeScenario)
            CheckWholeVoiceStateParity(*refVoices, *prodVoices, backendName);
    }
}
