#include "SVMSChannelCache.h"
#include "SVMSRenderScalar.h"
#include "SVMSSoundFont.h"
#include "SVMSVoiceManager.h"
#include "SVMSEnvelope.h"
#include "SVMSPSCQueue.h"
#include "SVMSMPSCQueue.h"
#include "SVMSEventScheduler.h"
#include "SVMSConfig.h"
#include "SVMSFrameClock.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <atomic>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool NearlyEqual(float a, float b, float epsilon = 1.0e-5f) {
    return std::fabs(a - b) <= epsilon;
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
    float renderSamples[64];
    for (uint32_t i = 0; i < 64; ++i) {
        rawSamples[i] = static_cast<int16_t>((static_cast<int>(i) - 32) * 700);
        renderSamples[i] = rawSamples[i] / 32768.0f;
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
    float samples[16];
    std::fill(std::begin(samples), std::end(samples), 1.0f);
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

void TestPriorityAwareStealingAndFadeTail() {
    {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(2, 44100);
        const svms::VoiceHandle quiet = voices->AllocateVoice(0, 60, 24);
        const svms::VoiceHandle loud = voices->AllocateVoice(0, 64, 120);
        voices->SetVoiceEnvelope(quiet, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->SetVoiceEnvelope(loud, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->v.mixGainL[quiet] = voices->v.mixGainR[quiet] = 1.0f;
        voices->v.mixGainL[loud] = voices->v.mixGainR[loud] = 1.0f;

        bool stolen = false;
        const svms::VoiceHandle replacement =
            voices->AllocateVoiceOrSteal(0, 67, 100, &stolen);
        Check(stolen && replacement == quiet,
              "voice pressure steals low velocity before velocity >= 96");
        Check(voices->v.velocity[loud] == 120,
              "high-priority loud voice survives pool stealing");
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
        const uint32_t fadeFrames = voices->v.stealTailFramesTotal[replacement];
        Check(stolen && fadeFrames == 441,
              "44.1 kHz stealing captures a 10 ms outgoing tail");
        Check(voices->v.stealFadeInFramesTotal[replacement] == fadeFrames,
              "replacement uses an independent matching fade-in");

        float samples[64];
        std::fill(std::begin(samples), std::end(samples), 1.0f);
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
        Check(left.back() > 0.0f && left.back() < 0.01f,
              "stolen tail reaches silence at the end of its fade");
        Check(voices->v.stealTailFramesRemaining[replacement] == 0,
              "stolen tail retires without consuming a voice slot");
    }
}

void TestChannelTerminationControllers() {
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
    voices->StartReleaseForPlayIndex(0, 60, 10);
    Check(voices->v.state[firstLayer] == static_cast<uint8_t>(svms::VoiceState::Releasing) &&
              voices->v.state[firstLayer2] == static_cast<uint8_t>(svms::VoiceState::Releasing),
          "all layered regions from one retrigger are released together");
    Check(voices->v.state[secondLayer] == static_cast<uint8_t>(svms::VoiceState::Active),
          "a later same-key retrigger survives the earlier note-off");
}

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

    float samples[256];
    for (uint32_t i = 0; i < 256; ++i)
        samples[i] = static_cast<float>(static_cast<int>(i % 17) - 8) / 8.0f;

    std::memset(left, 0, frames * sizeof(float));
    std::memset(right, 0, frames * sizeof(float));
    svms::RenderScalar renderer;
    renderer.RenderBlock(*voices, channels, samples, 256, left, right,
                         frames, cfg);
}

void TestPitchAndDeterministicRender() {
    svms::ChannelCache cache;
    cache.PitchBend(0, 0);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), -12.0f),
          "minimum pitch wheel is -12 semitones");
    cache.PitchBend(0, 8192);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 0.0f),
          "center pitch wheel is unpitched");
    cache.PitchBend(0, 16383);
    Check(NearlyEqual(cache.GetPitchBendSemitones(0), 12.0f, 2.0e-3f),
          "maximum pitch wheel is +12 semitones");

    float leftA[32], rightA[32], leftB[32], rightB[32];
    RenderDeterministic(leftA, rightA, 32);
    RenderDeterministic(leftB, rightB, 32);
    Check(std::memcmp(leftA, leftB, sizeof(leftA)) == 0,
          "scalar left output is deterministic");
    Check(std::memcmp(rightA, rightB, sizeof(rightA)) == 0,
          "scalar right output is deterministic");
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
}

void TestWindowedSchedulerOrdering() {
    svms::EventScheduler scheduler(8);
    for (uint32_t i = 0; i < 4; ++i) {
        svms::ScheduledRenderEvent e;
        e.targetFrame = (i & 1) ? 4 : 2;
        e.sequence = i;
        e.event.frameOffset = 0;
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
            for (uint32_t i = 0; i < eventsPerProducer; ++i) {
                svms::TimestampedMidiEvent event{};
                event.sequence = producer * eventsPerProducer + i;
                event.message = 0x90u | ((60u + producer) << 8) | (127u << 16);
                event.qpcTimestamp = event.sequence * 17ull;
                while (!queue.TryPush(event)) std::this_thread::yield();
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
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", configPath.c_str());

    svms::EngineConfig created = svms::EngineConfig::Load();
    Check(fs::exists(configPath), "first run creates config.json");
    Check(created.sampleRate == 44100 && created.bufferFrames == 2048,
          "first-run JSON uses compiled audio defaults");
    Check(created.eventRingCapacity == 393216 && created.highPriorityVelocity == 96,
          "first-run JSON uses priority ingress defaults");
    {
        std::ifstream input(configPath, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), {});
        Check(text.find("\"schema_version\": 1") != std::string::npos,
              "created JSON carries schema version");
        Check(text.find("\"correctness_mode\": true") != std::string::npos,
              "created JSON enables scalar correctness mode");
    }

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
        output << R"({"schema_version":1,"audio":{"sample_rate":1,"buffer_frames":256},"synth":{"max_voices":64},"unknown":{"preserve_me":true}})";
    }
    svms::EngineConfig invalidField = svms::EngineConfig::Load();
    Check(invalidField.sampleRate == 44100,
          "invalid JSON field retains its compiled default");
    Check(invalidField.bufferFrames == 256 && invalidField.maxVoices == 64,
          "valid JSON fields still apply beside an invalid field");
    Check(!invalidField.configWarning.empty(), "invalid JSON field publishes warning");

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
    svms::EngineConfig environment = svms::EngineConfig::Load();
    Check(environment.eventOverflowMode == svms::EventOverflowMode::LosslessBackpressure,
          "lossless compatibility environment override has final precedence");
    Check(!environment.correctnessMode && environment.diagnosticsEnabled,
          "correctness and diagnostics environment overrides have final precedence");

    SetEnvironmentVariableW(L"SVMS_NO_DROP_EVENTS", nullptr);
    SetEnvironmentVariableW(L"SVMS_CORRECTNESS_MODE", nullptr);
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS", nullptr);
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", nullptr);
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

void TestExpressionAgeRetirementAndLoopWrap() {
    svms::ChannelCache channels;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(8, 44100);
    voices->SetCurrentFrame(1000);
    const svms::VoiceHandle first = voices->AllocateVoice(0, 60, 127);
    const svms::VoiceHandle middle = voices->AllocateVoice(0, 61, 127);
    const svms::VoiceHandle last = voices->AllocateVoice(0, 62, 127);
    voices->SetVoiceGain(first, 1.0f, 1.0f);
    voices->RefreshMixGain(first, channels.GetParams()[0]);
    const float fullExpressionGain = voices->v.mixGainL[first];
    channels.ControlChange(0, 11, 64);
    channels.RebuildCache(cfg, 44100.0f);
    voices->RefreshMixGain(first, channels.GetParams()[0]);
    Check(NearlyEqual(voices->v.mixGainL[first] / fullExpressionGain, 64.0f / 127.0f),
          "CC11 expression participates in active voice gain");

    voices->RetireVoice(middle);
    Check(voices->GetActiveCount() == 2 &&
          voices->activePosition_[last] < voices->GetActiveCount() &&
          voices->activeList_[voices->activePosition_[last]] == last,
          "inverse active positions make middle retirement O(1) and coherent");
    voices->SetCurrentFrame(1245);
    Check(voices->GetVoiceAge(first) == 245,
          "voice age is derived from absolute birth frame");

    float samples[16]{};
    for (uint32_t i = 0; i < 16; ++i) samples[i] = 0.25f;
    voices->SetVoiceSample(first, 0, 10, 2, 6, 1, 12.25f, 1);
    voices->v.phases[first] = 3.5f;
    voices->SetVoiceEnvelope(first, 1.0f, 1.0f, 0, 0, 0, 0,
                             0.0f, 1.0f, 0.999f);
    float left[1]{};
    float right[1]{};
    svms::RenderScalar renderer;
    renderer.RenderBlock(*voices, channels, samples, 16, left, right, 1, cfg,
                         nullptr, 0, true, 1245);
    Check(NearlyEqual(voices->v.phases[first], 3.75f),
          "loop wrapping preserves overshoot across multiple loop lengths");
}

} // namespace

int main() {
    TestBankProgramState();
    TestExactPresetLookup();
    TestRegionValidationAndLiveConfiguration();
    TestCompiledSF2ZonesAndExactResolver();
    TestShippedGmSoundFontSmoke();
    TestEnvelopeConversions();
    TestExactReleaseDurationAcrossBlocks();
    TestReleaseGeneratorMerging();
    TestVoiceIdentityAndStealing();
    TestPriorityAwareStealingAndFadeTail();
    TestChannelTerminationControllers();
    TestOverlappingRetriggerGenerations();
    TestPitchAndDeterministicRender();
    TestEventRingWrapAndCapacity();
    TestWindowedSchedulerOrdering();
    TestFourProducerMPSCIntegrity();
    TestJsonConfigurationLifecycle();
    TestSixHourFrameClockDrift();
    TestExpressionAgeRetirementAndLoopWrap();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }

    std::puts("SVMS V3 correctness tests passed");
    return 0;
}
