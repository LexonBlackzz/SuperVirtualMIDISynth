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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

static std::atomic<uint64_t> g_realtimeAllocationCount{0};
static thread_local bool g_trackRealtimeAllocations = false;

void* operator new(std::size_t size) {
    if (g_trackRealtimeAllocations)
        g_realtimeAllocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (g_trackRealtimeAllocations)
        g_realtimeAllocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = _aligned_malloc(size, static_cast<std::size_t>(alignment)))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    _aligned_free(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    _aligned_free(memory);
}

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
    float samples[2]{0.0f, 0.0f};
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
        Check(stolen && fadeFrames == svms::kStealFadeFrames,
              "stealing captures the fixed 64-frame outgoing tail");
        Check(voices->v.stealFadeInFramesTotal[replacement] == 0,
              "replacement attack is not blurred by the outgoing tail fade");

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
        Check(std::fabs(left.back()) <= 1.0e-7f,
              "stolen tail reaches silence at the end of its fade");
        Check(voices->v.stealTailFramesRemaining[replacement] == 0,
              "stolen tail retires without consuming a voice slot");
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

    uint64_t frame = 9000u;
    for (uint32_t iteration = 0; iteration < 512u; ++iteration) {
        frame += (iteration == 256u) ? 441000u : 37u;
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
        const svms::VoiceHandle expected =
            voices->FindStealVictimExhaustiveForTest();
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

    // Dense piano attacks used to live in the exact fallback list and force
    // a complete pool scan for every equal-frame layer. Verify that the
    // per-frame volatile heap retains the exhaustive oracle's victim and tie
    // decisions both within a frame and after envelope time advances.
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
              "per-frame transient steal heap matches exhaustive victim selection");
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

    Check(svms::ComputeDecimationStep(svms::kMaxPolyphony) == 1,
          "the complete 4096-voice pool remains full quality");
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
        Check(text.find("Alpha Piano.SF2") != std::string::npos,
              "created JSON explicitly stores the discovered SoundFont");
    }

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

void TestOverloadTimelineRecovery() {
    Check(svms::RecoverRealtimeRenderFrame(4096, 4120, 512) == 4096,
          "small QPC jitter does not move the render timeline");
    Check(svms::RecoverRealtimeRenderFrame(4096, 5000, 512) == 5000,
          "missed callback frames fast-forward the render timeline");
    Check(svms::RecoverRealtimeRenderFrame(5000, 4096, 512) == 5000,
          "wall-clock recovery never moves the render timeline backward");
    Check(!svms::IsObsoleteNoteOn(3000, 4096, 2048),
          "a note-on within one buffer is clamped instead of skipped");
    Check(svms::IsObsoleteNoteOn(1000, 4096, 2048),
          "a note-on from skipped audio time is discarded");
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

struct DifferentialDispatchContext {
    svms::VoiceManager* voices;
    svms::ChannelCache* channels;
    const svms::RuntimeConfigSnapshot* cfg;
    uint32_t order[256]{};
    uint32_t orderCount = 0;
    uint32_t playIndex = 1000;
};

void DispatchDifferentialEvent(const svms::RenderEvent& event, uint32_t,
                               void* userData) {
    auto* context = static_cast<DifferentialDispatchContext*>(userData);
    if (context->orderCount < std::size(context->order))
        context->order[context->orderCount++] = event.ingressSequence;

    switch (event.type) {
        case svms::RenderEventType::NoteOn: {
            const svms::VoiceHandle voice = context->voices->AllocateVoiceOrSteal(
                event.channel, event.data1, event.data2);
            if (voice == svms::kInvalidVoice) break;
            context->voices->SetVoicePlayIndex(voice, context->playIndex++);
            context->voices->SetVoiceSample(voice, 0, 4096, 64, 4032, 1,
                                             0.75f + event.data1 * 0.005f, 1);
            context->voices->SetVoiceEnvelope(voice, 0.6f, 0.5f, 0, 0, 17, 53,
                                               0.6f / 17.0f, 0.99f, 0.9995f,
                                               400);
            context->voices->SetVoiceGain(voice, 0.02f, 0.02f);
            context->voices->RefreshMixGain(
                voice, context->channels->GetParams()[event.channel]);
            break;
        }
        case svms::RenderEventType::NoteOff:
            for (uint32_t i = 0; i < context->voices->activeCount_; ++i) {
                const uint32_t voice = context->voices->activeList_[i];
                if (context->voices->v.channel[voice] == event.channel &&
                    context->voices->v.note[voice] == event.data1) {
                    context->voices->StartRelease(voice);
                }
            }
            break;
        case svms::RenderEventType::ControlChange:
            context->channels->ControlChange(event.channel, event.data1, event.data2);
            context->channels->RebuildCache(*context->cfg, 44100.0f);
            context->voices->RefreshMixGains(context->channels->GetParams());
            break;
        case svms::RenderEventType::Reset:
            context->voices->Reset();
            context->channels->Reset();
            context->channels->RebuildCache(*context->cfg, 44100.0f);
            break;
        default:
            break;
    }
}

void ConfigureDifferentialSeed(svms::VoiceManager& voices,
                               const svms::ChannelCache& channels) {
    // Start full so randomized note-ons exercise exact stealing and the
    // independent steal-tail batch in every differential buffer-size case.
    voices.Initialize(48, 44100);
    for (uint32_t i = 0; i < 48; ++i) {
        const uint8_t channel = static_cast<uint8_t>(i & 3u);
        const uint8_t note = static_cast<uint8_t>(48u + i % 24u);
        const svms::VoiceHandle voice = voices.AllocateVoice(
            channel, note, static_cast<uint8_t>(48u + i));
        const bool loop = (i % 4u) != 1u;
        voices.SetVoiceSample(voice, 0, 4096, 64, 4032, loop ? 1u : 0u,
                              0.4f + static_cast<float>(i % 17u) * 0.11f, 1);
        voices.v.phases[voice] = static_cast<float>((i * 37u) % 1700u) + 0.375f;
        if ((i % 4u) == 2u) {
            voices.SetVoiceEnvelope(voice, 0.8f, 0.35f, 0, 0, 101 + i, 0,
                                    0.8f / static_cast<float>(101 + i),
                                    1.0f, 0.9997f, 700 + i);
        } else if ((i % 4u) == 3u) {
            voices.SetVoiceEnvelope(voice, 0.8f, 0.35f, 0, 0, 0, 307 + i,
                                    0.0f, 0.997f, 0.9997f, 700 + i);
            voices.StartRelease(voice);
        } else {
            voices.SetVoiceEnvelope(voice, 0.8f, 0.7f, 0, 0, 0, 0,
                                    0.0f, 1.0f, 0.9997f, 700 + i);
        }
        voices.SetVoicePlayIndex(voice, i + 1u);
        voices.SetVoiceGain(voice, 0.02f, 0.02f);
        voices.RefreshMixGain(voice, channels.GetParams()[channel]);
    }
}

void TestSpanRendererDifferential() {
    static constexpr uint32_t bufferSizes[] = {16, 64, 257, 2048, 8192};
    std::vector<float> samples(4096);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.4f * std::sin(static_cast<float>(i) * 0.031f) +
                     0.15f * std::cos(static_cast<float>(i) * 0.079f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.velocityCurve = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (const uint32_t frames : bufferSizes) {
        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureDifferentialSeed(*seedVoices, seedChannels);
        auto referenceVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto spanVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        svms::ChannelCache referenceChannels = seedChannels;
        svms::ChannelCache spanChannels = seedChannels;

        std::vector<svms::RenderEvent> events;
        uint32_t random = 0x6d2b79f5u ^ frames;
        for (uint32_t sequence = 0; sequence < 48; ++sequence) {
            random = random * 1664525u + 1013904223u;
            svms::RenderEvent event{};
            event.frameOffset = random % frames;
            event.ingressSequence = sequence;
            event.channel = static_cast<uint8_t>((random >> 8) & 3u);
            if ((sequence % 3u) == 0u) {
                event.type = svms::RenderEventType::NoteOn;
                event.data1 = static_cast<uint8_t>(72u + sequence % 24u);
                event.data2 = static_cast<uint8_t>(80u + sequence % 40u);
            } else if ((sequence % 3u) == 1u) {
                event.type = svms::RenderEventType::NoteOff;
                event.data1 = static_cast<uint8_t>(48u + sequence % 24u);
            } else {
                event.type = svms::RenderEventType::ControlChange;
                event.data1 = static_cast<uint8_t>((sequence & 1u) ? 11u : 7u);
                event.data2 = static_cast<uint8_t>(40u + sequence);
            }
            events.push_back(event);
        }
        std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
            if (a.frameOffset != b.frameOffset) return a.frameOffset < b.frameOffset;
            return a.ingressSequence < b.ingressSequence;
        });

        std::vector<float> referenceLeft(frames, 0.0f), referenceRight(frames, 0.0f);
        std::vector<float> spanLeft(frames, 0.0f), spanRight(frames, 0.0f);
        DifferentialDispatchContext referenceContext{
            referenceVoices.get(), &referenceChannels, &cfg};
        DifferentialDispatchContext spanContext{spanVoices.get(), &spanChannels, &cfg};
        auto referenceRenderer = std::make_unique<svms::RenderScalar>();
        auto spanRenderer = std::make_unique<svms::RenderScalar>();
        referenceRenderer->SetEventDispatcher(DispatchDifferentialEvent, &referenceContext);
        spanRenderer->SetEventDispatcher(DispatchDifferentialEvent, &spanContext);

        referenceRenderer->RenderBlockReference(
            *referenceVoices, referenceChannels, samples.data(),
            static_cast<uint32_t>(samples.size()), referenceLeft.data(),
            referenceRight.data(), frames, cfg, events.data(),
            static_cast<uint32_t>(events.size()), true, 10000);
        spanRenderer->RenderBlock(
            *spanVoices, spanChannels, samples.data(),
            static_cast<uint32_t>(samples.size()), spanLeft.data(), spanRight.data(),
            frames, cfg, events.data(), static_cast<uint32_t>(events.size()), true, 10000);

        Check(referenceContext.orderCount == spanContext.orderCount &&
                  std::memcmp(referenceContext.order, spanContext.order,
                              referenceContext.orderCount * sizeof(uint32_t)) == 0,
              "span renderer preserves exact event dispatch order");

        float maximumDifference = 0.0f;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            maximumDifference = (std::max)(maximumDifference,
                std::fabs(referenceLeft[frame] - spanLeft[frame]));
            maximumDifference = (std::max)(maximumDifference,
                std::fabs(referenceRight[frame] - spanRight[frame]));
        }
        Check(maximumDifference <= 2.0e-4f,
              "span waveform matches the frame-major scalar reference");

        std::vector<uint32_t> referenceActive(
            referenceVoices->activeList_,
            referenceVoices->activeList_ + referenceVoices->activeCount_);
        std::vector<uint32_t> spanActive(
            spanVoices->activeList_, spanVoices->activeList_ + spanVoices->activeCount_);
        std::sort(referenceActive.begin(), referenceActive.end());
        std::sort(spanActive.begin(), spanActive.end());
        if (referenceActive != spanActive) {
            std::fprintf(stderr, "differential active mismatch at %u frames: ref=%zu span=%zu\n",
                         frames, referenceActive.size(), spanActive.size());
        }
        Check(referenceActive == spanActive,
              "span renderer preserves active voice identity and retirement");
        for (uint32_t voice : referenceActive) {
            const bool stateMatches =
                referenceVoices->v.state[voice] == spanVoices->v.state[voice] &&
                NearlyEqual(referenceVoices->v.phases[voice],
                            spanVoices->v.phases[voice], 2.0e-3f) &&
                NearlyEqual(referenceVoices->v.currentGain[voice],
                            spanVoices->v.currentGain[voice], 2.0e-5f) &&
                referenceVoices->v.releaseSamplesRemaining[voice] ==
                    spanVoices->v.releaseSamplesRemaining[voice];
            if (!stateMatches) {
                std::fprintf(stderr,
                    "differential state mismatch frames=%u voice=%u state=%u/%u phase=%g/%g gain=%g/%g release=%u/%u\n",
                    frames, voice, referenceVoices->v.state[voice], spanVoices->v.state[voice],
                    referenceVoices->v.phases[voice], spanVoices->v.phases[voice],
                    referenceVoices->v.currentGain[voice], spanVoices->v.currentGain[voice],
                    referenceVoices->v.releaseSamplesRemaining[voice],
                    spanVoices->v.releaseSamplesRemaining[voice]);
            }
            Check(stateMatches, "span renderer preserves active voice state");
        }
    }
}

void TestRenderBackendSelectionAndDenseEquivalence() {
    constexpr uint32_t voiceCount = 32u;
    constexpr uint32_t frames = 4u;
    std::vector<float> samples(2048);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = std::sin(static_cast<float>(i) * 0.021f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto seed = std::make_unique<svms::VoiceManager>();
    seed->Initialize(voiceCount, 44100);
    for (uint32_t i = 0; i < voiceCount; ++i) {
        const svms::VoiceHandle h = seed->AllocateVoice(0, 60, 100);
        seed->SetVoiceSample(h, 0, 2048, 16, 2032, 1,
                             0.5f + static_cast<float>(i) * 0.01f, 1);
        seed->SetVoiceEnvelope(h, 1.0f, 1.0f, 0, 0, 0, 0,
                               0.0f, 1.0f, 0.9999f);
        seed->SetVoiceGain(h, 0.01f, 0.01f);
        seed->RefreshMixGain(h, channels.GetParams()[0]);
    }

    const svms::RenderBackend backends[] = {
        svms::RenderBackend::SSE2, svms::RenderBackend::AVX2};
    for (svms::RenderBackend backend : backends) {
        if (!svms::IsRenderBackendSupported(backend)) continue;
        auto scalarVoices = std::make_unique<svms::VoiceManager>(*seed);
        auto acceleratedVoices = std::make_unique<svms::VoiceManager>(*seed);
        svms::RenderScalar scalar;
        svms::RenderScalar accelerated;
        Check(scalar.SetRenderBackend(svms::RenderBackend::Scalar) &&
                  accelerated.SetRenderBackend(backend),
              "supported render backend can be selected explicitly");
        float scalarLeft[frames]{}, scalarRight[frames]{};
        float acceleratedLeft[frames]{}, acceleratedRight[frames]{};
        scalar.RenderBlock(*scalarVoices, channels, samples.data(),
            static_cast<uint32_t>(samples.size()), scalarLeft, scalarRight,
            frames, cfg, nullptr, 0, true, 1000u);
        accelerated.RenderBlock(*acceleratedVoices, channels, samples.data(),
            static_cast<uint32_t>(samples.size()), acceleratedLeft,
            acceleratedRight, frames, cfg, nullptr, 0, true, 1000u);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            Check(NearlyEqual(scalarLeft[frame], acceleratedLeft[frame], 2.0e-5f) &&
                      NearlyEqual(scalarRight[frame], acceleratedRight[frame], 2.0e-5f),
                  "accelerated dense kernel matches scalar audio");
        }
        for (uint32_t h = 0; h < voiceCount; ++h)
            Check(NearlyEqual(scalarVoices->v.phases[h],
                              acceleratedVoices->v.phases[h], 1.0e-5f),
                  "accelerated dense kernel preserves phase state");
    }
}

void TestRenderCallbackPurity() {
    constexpr uint32_t frames = 512;
    constexpr uint32_t voiceCount = 256;
    std::vector<float> samples(4096, 0.25f);
    std::vector<float> left(frames, 0.0f), right(frames, 0.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(voiceCount, 44100);
    for (uint32_t i = 0; i < voiceCount; ++i) {
        const svms::VoiceHandle voice = voices->AllocateVoice(0, 60, 100);
        voices->SetVoiceSample(voice, 0, 4096, 64, 4032, 1, 1.0f, 1);
        voices->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->SetVoiceGain(voice, 0.01f, 0.01f);
        voices->RefreshMixGain(voice, channels.GetParams()[0]);
    }
    auto renderer = std::make_unique<svms::RenderScalar>();

    g_realtimeAllocationCount.store(0, std::memory_order_relaxed);
    g_trackRealtimeAllocations = true;
    renderer->RenderBlock(*voices, channels, samples.data(),
                          static_cast<uint32_t>(samples.size()), left.data(),
                          right.data(), frames, cfg, nullptr, 0, true, 0);
    g_trackRealtimeAllocations = false;
    Check(g_realtimeAllocationCount.load(std::memory_order_relaxed) == 0,
          "production scalar render performs no heap allocation");
}

void TestCallbackSourcePurity() {
    const std::filesystem::path sourcePath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "SVMSDriver.cpp";
    std::ifstream input(sourcePath, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    const size_t callbackBegin = source.find("void Driver::RenderCallback");
    const size_t callbackEnd = source.find("} // namespace svms", callbackBegin);
    Check(callbackBegin != std::string::npos && callbackEnd != std::string::npos,
          "callback purity audit locates the production real-time section");
    if (callbackBegin == std::string::npos || callbackEnd == std::string::npos) return;
    const std::string realtimeSection =
        source.substr(callbackBegin, callbackEnd - callbackBegin);
    Check(realtimeSection.find("EnterCriticalSection") == std::string::npos &&
              realtimeSection.find("OutputDebugString") == std::string::npos &&
              realtimeSection.find("LOG(") == std::string::npos &&
              realtimeSection.find("DiagWindow_Create") == std::string::npos &&
              realtimeSection.find("CreateWindow") == std::string::npos,
          "audio callback and MIDI dispatch contain no lock, debug, or UI calls");
}

} // namespace

int main() {
    TestBankProgramState();
    TestExactPresetLookup();
    TestRegionValidationAndLiveConfiguration();
    TestCompiledSF2ZonesAndExactResolver();
    TestShippedGmSoundFontSmoke();
    TestEnvelopeConversions();
    TestExactFrameBatchDispatch();
    TestExactReleaseDurationAcrossBlocks();
    TestReleaseGeneratorMerging();
    TestVoiceIdentityAndStealing();
    TestPriorityAwareStealingAndFadeTail();
    TestExactStealHeapAndVoiceIndices();
    TestPersistentStealIndexAgainstOracle();
    TestChannelTerminationControllers();
    TestOverlappingRetriggerGenerations();
    TestPitchAndDeterministicRender();
    TestConfiguredVelocityMapping();
    TestEventRingWrapAndCapacity();
    TestWindowedSchedulerOrdering();
    TestFourProducerMPSCIntegrity();
    TestJsonConfigurationLifecycle();
    TestSixHourFrameClockDrift();
    TestOverloadTimelineRecovery();
    TestExpressionAgeRetirementAndLoopWrap();
    TestSpanRendererDifferential();
    TestRenderBackendSelectionAndDenseEquivalence();
    TestRenderCallbackPurity();
    TestCallbackSourcePurity();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }

    std::puts("SVMS V3 correctness tests passed");
    return 0;
}
