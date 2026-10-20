// Test cases split out of V3Tests.cpp; included inside its anonymous namespace.

void TestPostHighPass3Hz() {
    constexpr uint32_t kRate = 44100u;

    svms::PostHighPass3Hz dcFilter;
    dcFilter.Initialize(kRate);
    Check(dcFilter.GetPole() > 0.999f && dcFilter.GetPole() < 1.0f,
          "3 Hz post filter has a stable low-frequency pole");
    const double cutoffOmega = 6.28318530717958647692 * 3.0 / kRate;
    const double pole = dcFilter.GetPole();
    const double cutoffGain =
        (2.0 * std::sin(cutoffOmega * 0.5)) /
        std::sqrt(1.0 + pole * pole - 2.0 * pole * std::cos(cutoffOmega));
    Check(cutoffGain > 0.70 && cutoffGain < 0.72,
          "3 Hz post filter is approximately -3 dB at its cutoff");
    std::vector<float> dcLeft(kRate, 1.0f);
    std::vector<float> dcRight(kRate, -0.5f);
    dcFilter.ProcessPlanar(dcLeft.data(), dcRight.data(), kRate);
    Check(std::fabs(dcLeft.back()) < 1.0e-7f &&
              std::fabs(dcRight.back()) < 1.0e-7f,
          "3 Hz post filter rejects steady DC within one second");

    constexpr uint32_t kFrames = kRate * 3u;
    std::vector<float> passLeft(kFrames);
    std::vector<float> passRight(kFrames);
    double inputEnergy = 0.0;
    for (uint32_t i = 0; i < kFrames; ++i) {
        const float sample = std::sin(6.28318530717958647692f * 100.0f *
                                      static_cast<float>(i) /
                                      static_cast<float>(kRate));
        passLeft[i] = sample;
        passRight[i] = sample;
        if (i >= kRate) inputEnergy += static_cast<double>(sample) * sample;
    }
    svms::PostHighPass3Hz passFilter;
    passFilter.Initialize(kRate);
    passFilter.ProcessPlanar(passLeft.data(), passRight.data(), kFrames);
    double outputEnergy = 0.0;
    for (uint32_t i = kRate; i < kFrames; ++i)
        outputEnergy += static_cast<double>(passLeft[i]) * passLeft[i];
    const double passbandGain = std::sqrt(outputEnergy / inputEnergy);
    Check(passbandGain > 0.995 && passbandGain < 1.005,
          "3 Hz post filter preserves the 100 Hz passband");

    constexpr uint32_t kContinuityFrames = 4096u;
    std::vector<float> whole(kContinuityFrames * 2u);
    for (uint32_t i = 0; i < kContinuityFrames; ++i) {
        whole[i * 2u] = 0.2f + std::sin(static_cast<float>(i) * 0.017f);
        whole[i * 2u + 1u] = -0.1f + std::cos(static_cast<float>(i) * 0.013f);
    }
    std::vector<float> chunked = whole;
    std::vector<float> fused(kContinuityFrames * 2u);
    svms::PostHighPass3Hz wholeFilter;
    svms::PostHighPass3Hz chunkedFilter;
    svms::PostHighPass3Hz fusedFilter;
    wholeFilter.Initialize(kRate);
    chunkedFilter.Initialize(kRate);
    fusedFilter.Initialize(kRate);
    wholeFilter.ProcessInterleavedStereo(whole.data(), kContinuityFrames);
    for (uint32_t i = 0; i < kContinuityFrames; ++i) {
        float left = chunked[i * 2u];
        float right = chunked[i * 2u + 1u];
        fusedFilter.ProcessStereoSample(left, right);
        fused[i * 2u] = left;
        fused[i * 2u + 1u] = right;
    }
    fusedFilter.FinishBlock();
    const uint32_t chunks[] = {17u, 1u, 511u, 3u, 1024u, 29u, 2048u, 463u};
    uint32_t cursor = 0u;
    for (uint32_t chunk : chunks) {
        chunkedFilter.ProcessInterleavedStereo(chunked.data() + cursor * 2u,
                                               chunk);
        cursor += chunk;
    }
    Check(cursor == kContinuityFrames,
          "3 Hz post filter continuity test covers the complete buffer");
    bool identical = true;
    for (uint32_t i = 0; i < kContinuityFrames * 2u; ++i) {
        if (whole[i] != chunked[i]) {
            identical = false;
            break;
        }
    }
    Check(identical,
          "3 Hz post filter is exactly continuous across callback boundaries");
    Check(whole == fused,
          "per-sample post filter matches the block filter");
}

void TestBankProgramState() {
    svms::ChannelCache cache;

    cache.ControlChange(0, 0, 2);
    cache.ControlChange(0, 32, 7);
    Check(cache.GetBankMSB(0) == 2, "bank MSB is tracked");
    Check(cache.GetBankLSB(0) == 7, "bank LSB is tracked");
    Check(cache.GetBank(0) == 263, "combined bank is tracked");

    cache.ProgramChange(0, 41);
    cache.SetSelectedPreset(0, 19);
    Check(cache.GetProgram(0) == 41, "program is tracked");
    Check(cache.GetSelectedPreset(0) == 19, "selected preset is tracked");

    cache.ControlChange(0, 0, 3);
    Check(cache.GetSelectedPreset(0) == 19,
          "bank changes do not retarget existing selected preset");
}

void TestExactPresetLookup() {
    auto data = std::make_unique<svms::SF2Data>();
    std::memset(data.get(), 0, sizeof(svms::SF2Data));
    data->presetCount = 2;
    data->presets[0].bank = 0;
    data->presets[0].preset = 0;
    data->presets[1].bank = 1;
    data->presets[1].preset = 0;

    uint32_t index = UINT32_MAX;
    Check(svms::sf2_find_preset(data.get(), 1, 0, &index) && index == 1,
          "exact bank/program lookup selects the matching preset");
    Check(svms::sf2_find_preset(data.get(), 0, 0, &index) && index == 0,
          "bank zero/program zero lookup selects the matching preset");
    Check(!svms::sf2_find_preset(data.get(), 2, 0, &index),
          "missing bank does not select an arbitrary same-program preset");

    data->presets[1].bank = 128;
    data->presets[1].preset = 0;
    Check(svms::sf2_resolve_preset(data.get(), 0, 0, false, &index) && index == 0,
          "normal-channel resolver selects the exact melodic preset");
    Check(svms::sf2_resolve_preset(data.get(), 0, 0, true, &index) && index == 1,
          "percussion resolver selects bank 128 before melodic bank zero");
}

void TestRegionValidationAndLiveConfiguration() {
    auto data = std::make_unique<svms::SF2Data>();
    std::memset(data.get(), 0, sizeof(svms::SF2Data));
    int16_t rawSamples[64];
    int16_t renderSamples[72]{};
    for (uint32_t i = 0; i < 64; ++i) {
        rawSamples[i] = static_cast<int16_t>((static_cast<int>(i) - 32) * 700);
        renderSamples[i] = rawSamples[i];
    }
    data->sampleData = rawSamples;
    data->sampleDataFrames = 64;
    data->sampleCount = 1;
    data->samples[0].start = 0;
    data->samples[0].end = 63;
    data->samples[0].sampleRate = 44100;
    data->samples[0].originalPitch = 60;

    svms::SFSampleRegion region{};
    region.sampleIndex = 0;
    region.startOffset = 0;
    region.endOffset = 63;
    region.rootKey = 60;
    Check(svms::sf2_validate_region(data.get(), &region),
          "valid region passes live playback validation");
    Check(svms::sf2_region_initial_peak(data.get(), &region) > 0.1f,
          "valid region has a measurable initial sample peak");

    region.sampleIndex = 1;
    Check(!svms::sf2_validate_region(data.get(), &region),
          "invalid sample ID is rejected before voice allocation");
    region.sampleIndex = 0;
    region.endOffset = 0;
    Check(!svms::sf2_validate_region(data.get(), &region),
          "empty sample bounds are rejected before voice allocation");
    region.endOffset = 64;
    Check(!svms::sf2_validate_region(data.get(), &region),
          "out-of-buffer sample bounds are rejected before voice allocation");

    region.endOffset = 63;
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(1);
    const svms::VoiceHandle voice = voices->AllocateVoice(0, 60, 127);
    voices->SetVoiceSample(voice, 0, 63, 0, 0, 0, 1.0f, 1);
    voices->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f, 0.999f);
    voices->SetVoiceGain(voice, 1.0f, 1.0f);
    svms::ChannelCache channels;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    channels.RebuildCache(cfg, 44100.0f);
    voices->RefreshMixGain(voice, channels.GetParams()[0]);
    float left[32]{}, right[32]{};
    svms::RenderScalar renderer;
    renderer.RenderBlock(*voices, channels, renderSamples, 64, left, right, 32, cfg);
    float peak = 0.0f;
    for (float value : left) peak = (std::max)(peak, std::fabs(value));
    Check(peak > 0.1f, "a validated live region produces non-silent scalar output");
}

void TestShippedGmSoundFontSmoke() {
    const std::filesystem::path gmPath = std::filesystem::current_path() / "bin" / "gm.sf2";
    if (!std::filesystem::exists(gmPath)) {
        std::puts("SKIP: shipped gm.sf2 is not present beside the build output");
        return;
    }

    auto data = std::make_unique<svms::SF2Data>();
    Check(svms::sf2_load(gmPath.string().c_str(), data.get()),
          "shipped gm.sf2 loads for the piano smoke test");
    if (!data->loaded) return;
    svms::sf2_build_regions(data.get());

    uint32_t indexedRegionTotal = 0u;
    for (uint32_t preset = 0; preset < data->presetCount; ++preset) {
        const uint32_t begin = data->presetRegionStart[preset];
        const uint32_t end = begin + data->presetRegionCount[preset];
        Check(end <= data->regionCount,
              "shipped SF2 preset region index stays in bounds");
        for (uint32_t region = begin; region < end && region < data->regionCount;
             ++region) {
            Check(data->regions[region].presetIndex == preset,
                  "shipped SF2 preset region index contains only its preset");
        }
        indexedRegionTotal += data->presetRegionCount[preset];
    }
    Check(indexedRegionTotal == data->regionCount,
          "shipped SF2 preset region index covers every compiled region");

    uint32_t pianoPreset = UINT32_MAX;
    Check(svms::sf2_resolve_preset(data.get(), 0, 0, false, &pianoPreset),
          "shipped gm.sf2 resolves acoustic piano preset zero");
    if (pianoPreset != UINT32_MAX) {
        // The shipped piano's final raw instrument zone begins at key 98.
        // Full-key coverage specifically guards the pbag/ibag sentinel
        // boundary: the old compiler absorbed later instruments into the
        // final zone and made every key from 98 through 127 silent.
        for (uint32_t noteValue = 0; noteValue < 128; ++noteValue) {
            const uint8_t note = static_cast<uint8_t>(noteValue);
            for (uint8_t velocity : {uint8_t(32), uint8_t(64), uint8_t(127)}) {
                const svms::SFSampleRegion* matches[8]{};
                const uint32_t count = svms::sf2_find_regions(data.get(), pianoPreset, note,
                                                               velocity, matches, 8);
                Check(count > 0, "shipped piano resolves a region at representative key/velocity");
                if (count > 0) {
                    Check(svms::sf2_validate_region(data.get(), matches[0]),
                          "shipped piano region has valid sample bounds");
                    Check(svms::sf2_region_initial_peak(data.get(), matches[0]) > 1.0e-4f,
                          "shipped piano region has audible initial samples");
                }
            }
        }
    }
    svms::sf2_free(data.get());
}

void TestCompiledSF2ZonesAndExactResolver() {
    auto data = std::make_unique<svms::SF2Data>();
    std::memset(data.get(), 0, sizeof(svms::SF2Data));
    data->presetCount = 1;
    data->instrumentCount = 1;
    data->sampleCount = 2;
    data->sampleDataFrames = 1000;
    data->presets[0].preset = 0;
    data->presets[0].bank = 0;
    data->presets[0].zoneIndex = 0;
    data->instruments[0].zoneIndex = 0;
    data->samples[0].start = 0;
    data->samples[0].end = 900;
    data->samples[0].loopStart = 100;
    data->samples[0].loopEnd = 800;
    data->samples[0].sampleRate = 44100;
    data->samples[0].originalPitch = 60;
    data->samples[1] = data->samples[0];

    // Preset global: key 10..100 and +10cB attenuation.
    data->presetZones[0].generatorIndex = 0;
    data->presetZones[1].generatorIndex = 3;
    data->generators[0] = {svms::Gen_KeyRange, static_cast<uint16_t>(10 | (100 << 8))};
    data->generators[1] = {svms::Gen_InitialAttenuation, 10};
    data->generators[2] = {svms::Gen_FineTune, 5};
    // Preset local: instrument 0 and key 20..80.
    data->generators[3] = {svms::Gen_Instrument, 0};
    data->generators[4] = {svms::Gen_KeyRange, static_cast<uint16_t>(20 | (80 << 8))};
    data->pgenCount = 5;
    data->presetZoneCount = 2;

    // Instrument global: +20cB attenuation and +3 fine tune.
    data->instrumentZones[0].generatorIndex = 5;
    data->instrumentZones[1].generatorIndex = 7;
    data->instrumentZones[2].generatorIndex = 9;
    data->generators[5] = {svms::Gen_InitialAttenuation, 20};
    data->generators[6] = {svms::Gen_FineTune, 3};
    // Instrument local: sample 0 and key 30..70.
    data->generators[7] = {svms::Gen_SampleID, 0};
    data->generators[8] = {svms::Gen_KeyRange, static_cast<uint16_t>(30 | (70 << 8))};
    data->generators[9] = {svms::Gen_SampleID, 1};
    data->generators[10] = {svms::Gen_KeyRange, static_cast<uint16_t>(30 | (70 << 8))};
    data->generatorCount = 11;
    data->instrumentZoneCount = 3;

    svms::sf2_build_regions(data.get());
    Check(data->regionCount == 2, "compiled SF2 produces both layered regions");
    if (data->regionCount == 2) {
        const auto& r = data->regions[0];
        Check(r.keyLo == 30 && r.keyHi == 70, "preset/instrument key ranges intersect");
        Check(r.initialAttenuation == 30, "attenuation is additive across global zones");
        Check(r.fineTune == 8, "fine tuning is additive across zones");
        Check(data->regions[1].sampleIndex == 1,
              "layered regions preserve source order");
    }

    const svms::SFSampleRegion* matches[4]{};
    Check(svms::sf2_find_regions(data.get(), 0, 40, 100, matches, 4) == 2,
          "canonical resolver finds all exact matching layers");
    Check(svms::sf2_find_regions(data.get(), 0, 20, 100, matches, 4) == 0,
          "canonical resolver does not use nearest-key fallback");
}

void TestEnvelopeConversions() {
    Check(NearlyEqual(svms::SustainAttenuationToGain(0.0f), 1.0f),
          "zero sustain attenuation means full sustain");
    Check(NearlyEqual(svms::SustainAttenuationToGain(100.0f), 0.3162278f, 1.0e-5f),
          "100 centibels converts to linear sustain gain");
    Check(NearlyEqual(svms::SustainAttenuationToGain(600.0f), 0.001f, 1.0e-6f),
          "600 centibels converts to linear sustain gain");
    Check(NearlyEqual(svms::InitialAttenuationToGain(200.0f), 0.1f, 1.0e-6f),
          "initial attenuation converts from centibels");
    Check(NearlyEqual(svms::TimecentsToSeconds(0), 1.0f) &&
              NearlyEqual(svms::TimecentsToSeconds(1200), 2.0f) &&
              svms::TimecentsToSeconds(-12000) == 0.0f,
          "SF2 envelope timecents convert to seconds with the zero-time pin");
    Check(svms::MakeReleaseSamples(1.0f, 44100) == 44100 &&
              svms::MakeReleaseSamples(0.0f, 44100) == 441,
          "SF2 release duration uses parsed seconds and the TSF-compatible 10 ms fallback");

    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(1);
    const svms::VoiceHandle voice = voices->AllocateVoice(0, 60, 100);
    voices->SetVoiceEnvelope(voice, 0.75f, 0.25f,
                             0, 0, 0, 100,
                             0.0f, 0.99f, 0.999f);
    Check(voices->v.envelopeStage[voice] == 2,
          "zero-attack region enters the decay stage");
    Check(NearlyEqual(voices->v.currentGain[voice], 0.75f),
          "zero-attack decay starts at initial gain rather than silence");
}
