#include "SVMSChannelCache.h"
#include "SVMSRenderScalar.h"
#include "SVMSSoundFont.h"
#include "SVMSVoiceManager.h"
#include "SVMSEnvelope.h"
#include "SVMSPSCQueue.h"
#include "SVMSMPSCQueue.h"
#include "SVMSEventScheduler.h"
#include "SVMSEventPages.h"
#include "SVMSEventCompile.h"
#include "SVMSConfig.h"
#include "SVMSFrameClock.h"
#include "SVMSPostFilter.h"

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
    Check(aligned64(voices->v.channel) && aligned64(voices->v.phases) &&
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

        float tailSamples[256];
        std::fill(std::begin(tailSamples), std::end(tailSamples), 0.25f);
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

    // Exercise the release-heavy equal-frame shape used by chopped Black
    // MIDI: many stable voices become volatile, then many exact replacements
    // consume that heap without advancing the output frame.
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
            const svms::VoiceHandle expected =
                denseVoices->FindStealVictimExhaustiveForTest();
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
        uint32_t classPosition = 0u;
        voices->ForEachRenderClassBlock(
            static_cast<svms::VoiceRenderClass>(classIndex),
            [&](const uint32_t* handles, uint32_t count) {
                for (uint32_t offset = 0u; offset < count; ++offset) {
                    Check(classPosition < expectedRender[classIndex].size() &&
                              handles[offset] ==
                                  expectedRender[classIndex][classPosition],
                          "paged render-class traversal preserves flat order");
                    ++classPosition;
                    ++renderIndexed;
                }
            });
        Check(classPosition == voices->GetRenderClassCount(
                  static_cast<svms::VoiceRenderClass>(classIndex)),
              "paged render-class count matches block traversal");
    }
    Check(renderIndexed == voices->GetActiveCount(),
          "paged render classes contain every active voice exactly once");
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

    Check(svms::ComputeDecimationStep(svms::kMaxPolyphony) == 1,
          "the complete configured voice pool remains full quality");
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
    Check(created.audioDevice == L"default",
          "first-run JSON selects the current Windows default audio output");
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
        output << R"json({"schema_version":1,"synth":{"render_threads":8}})json";
    }
    svms::EngineConfig workerConfiguration = svms::EngineConfig::Load();
    Check(workerConfiguration.renderThreads == 8u &&
              workerConfiguration.Validate(),
          "JSON selects a fixed voice-render thread count");

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

void TestTransientClassKernelDifferential() {
    constexpr uint32_t voiceCount = 32u;
    std::vector<float> samples(4096);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = std::sin(static_cast<float>(i) * 0.017f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    struct Scenario {
        uint32_t attack;
        uint32_t decay;
        float phase;
        float phaseStep;
    };
    const Scenario scenarios[] = {
        {64u, 64u, 20.25f, 0.75f},
        {0u, 64u, 20.25f, 0.75f},
        {2u, 2u, 20.25f, 0.75f},
        {64u, 64u, 2030.75f, 3.25f},
    };

    for (uint32_t frames = 1u; frames <= 4u; ++frames) {
        for (const Scenario& scenario : scenarios) {
            auto seed = std::make_unique<svms::VoiceManager>();
            seed->Initialize(voiceCount, 44100);
            for (uint32_t i = 0; i < voiceCount; ++i) {
                const svms::VoiceHandle h = seed->AllocateVoice(0, 60, 100);
                seed->SetVoiceSample(h, 0, 4096, 16, 2032, 1,
                                     scenario.phaseStep, 1);
                seed->SetVoiceEnvelope(h, 1.0f, 0.4f, 0, 0,
                    scenario.attack, scenario.decay,
                    scenario.attack != 0u
                        ? 1.0f / static_cast<float>(scenario.attack)
                        : 0.0f,
                    0.97f, 0.9999f);
                seed->SetVoiceGain(h, 0.01f, 0.012f);
                seed->RefreshMixGain(h, channels.GetParams()[0]);
                seed->v.phases[h] = scenario.phase +
                    static_cast<float>(i & 3u) * 0.03125f;
            }

            auto referenceVoices = std::make_unique<svms::VoiceManager>(*seed);
            auto spanVoices = std::make_unique<svms::VoiceManager>(*seed);
            svms::RenderScalar reference;
            svms::RenderScalar span;
            span.SetRenderBackend(svms::RenderBackend::Scalar);
            float referenceLeft[4]{}, referenceRight[4]{};
            float spanLeft[4]{}, spanRight[4]{};
            reference.RenderBlockReference(*referenceVoices, channels,
                samples.data(), static_cast<uint32_t>(samples.size()),
                referenceLeft, referenceRight, frames, cfg, nullptr, 0, true,
                4000u);
            span.RenderBlock(*spanVoices, channels, samples.data(),
                static_cast<uint32_t>(samples.size()), spanLeft, spanRight,
                frames, cfg, nullptr, 0, true, 4000u);

            for (uint32_t frame = 0; frame < frames; ++frame) {
                Check(NearlyEqual(referenceLeft[frame], spanLeft[frame],
                                  2.0e-5f) &&
                          NearlyEqual(referenceRight[frame], spanRight[frame],
                                      2.0e-5f),
                      "short transient class kernel matches oracle audio");
            }
            for (uint32_t h = 0; h < voiceCount; ++h) {
                Check(NearlyEqual(referenceVoices->v.phases[h],
                                  spanVoices->v.phases[h], 1.0e-5f) &&
                          NearlyEqual(referenceVoices->v.currentGain[h],
                                      spanVoices->v.currentGain[h], 1.0e-6f) &&
                          referenceVoices->v.envelopeStage[h] ==
                              spanVoices->v.envelopeStage[h] &&
                          referenceVoices->v.attackSamplesRemaining[h] ==
                              spanVoices->v.attackSamplesRemaining[h] &&
                          referenceVoices->v.decaySamplesRemaining[h] ==
                              spanVoices->v.decaySamplesRemaining[h],
                      "short transient class kernel preserves envelope state");
            }
            for (uint32_t classIndex = 0;
                 classIndex < svms::kVoiceRenderClassCount; ++classIndex) {
                const auto renderClass =
                    static_cast<svms::VoiceRenderClass>(classIndex);
                Check(referenceVoices->GetRenderClassCount(renderClass) ==
                          spanVoices->GetRenderClassCount(renderClass),
                      "short transient class kernel preserves class transitions");
            }
        }
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

void TestParallelSustainedRenderDifferential() {
    constexpr uint32_t voiceCount = 1024u;
    constexpr uint32_t frames = 2048u;
    std::vector<float> samples(8192u);
    for (uint32_t index = 0u; index < samples.size(); ++index)
        samples[index] = 0.4f * std::sin(static_cast<float>(index) * 0.037f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.interpolation = svms::InterpolationMode::Linear;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    auto makeVoices = [&]() {
        auto result = std::make_unique<svms::VoiceManager>();
        Check(result->Initialize(voiceCount, 44100u),
              "parallel differential allocates voice storage");
        for (uint32_t index = 0u; index < voiceCount; ++index) {
            const svms::VoiceHandle voice = result->AllocateVoice(
                static_cast<uint8_t>(index & 15u),
                static_cast<uint8_t>(24u + index % 88u), 127u);
            result->SetVoiceSample(voice, 0u, 8192u, 64u, 8128u, 1u,
                0.5f + static_cast<float>(index % 31u) * 0.03125f, 1u);
            result->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0u, 0u, 0u, 0u,
                                     0.0f, 1.0f, 0.999f);
            result->SetVoiceGain(voice, 0.0001f, 0.0001f);
            result->RefreshMixGain(voice,
                channels.GetParams()[index & 15u]);
        }
        return result;
    };

    auto serialVoices = makeVoices();
    auto parallelVoices = makeVoices();
    svms::RenderScalar serial;
    svms::RenderScalar parallel;
    Check(serial.ReserveVoiceCapacity(voiceCount) &&
              parallel.ReserveVoiceCapacity(voiceCount) &&
              serial.ConfigureRenderThreads(1u, frames) &&
              parallel.ConfigureRenderThreads(4u, frames) &&
              parallel.GetRenderThreadCount() == 4u,
          "parallel differential starts four deterministic render lanes");

    std::vector<float> serialLeft(frames, 0.0f), serialRight(frames, 0.0f);
    std::vector<float> parallelLeft(frames, 0.0f), parallelRight(frames, 0.0f);
    serial.RenderBlock(*serialVoices, channels, samples.data(),
        static_cast<uint32_t>(samples.size()), serialLeft.data(),
        serialRight.data(), frames, cfg, nullptr, 0u, true, 1000u);
    parallel.RenderBlock(*parallelVoices, channels, samples.data(),
        static_cast<uint32_t>(samples.size()), parallelLeft.data(),
        parallelRight.data(), frames, cfg, nullptr, 0u, true, 1000u);

    for (uint32_t frame = 0u; frame < frames; ++frame) {
        Check(NearlyEqual(serialLeft[frame], parallelLeft[frame], 5.0e-5f) &&
                  NearlyEqual(serialRight[frame], parallelRight[frame],
                              5.0e-5f),
              "parallel sustained renderer preserves waveform tolerance");
    }
    for (uint32_t voice = 0u; voice < voiceCount; ++voice) {
        Check(serialVoices->v.phases[voice] ==
                  parallelVoices->v.phases[voice] &&
                  serialVoices->v.state[voice] ==
                  parallelVoices->v.state[voice],
              "parallel sustained renderer preserves exact voice state");
    }
}

struct DensePlannerDispatchContext {
    svms::VoiceManager* voices = nullptr;
    const svms::ChannelCache* channels = nullptr;
    uint32_t playIndex = 10000u;
};

void DensePlannerDispatch(const svms::RenderEvent* events,
                          uint32_t eventCount, uint32_t,
                          void* userData) {
    auto& context = *static_cast<DensePlannerDispatchContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        if (event.type != svms::RenderEventType::NoteOn) continue;
        svms::VoiceConfiguration setup{};
        setup.sampleStart = 0u;
        setup.sampleEnd = 128u;
        setup.loopStart = 16u;
        setup.loopEnd = 96u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep =
            0.637123f +
            static_cast<float>(event.data1 & 31u) * 0.030731f;
        setup.initialGain = setup.sustainLevel = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.001f;
        setup.sampleBacked = 1u;
        setup.playIndex = context.playIndex++;
        svms::VoiceHandle handle = svms::kInvalidVoice;
        context.voices->LaunchVoiceGroup(
            event.channel, event.data1, event.data2, &setup, 1u,
            setup.playIndex,
            context.channels->GetParams()[event.channel], &handle);
    }
}

void TestDensePlannerOracleDifferential() {
    constexpr uint32_t voiceCount = 512u;
    constexpr uint32_t frames = 256u;
    std::vector<float> samples(4096u);
    for (uint32_t index = 0u; index < samples.size(); ++index)
        samples[index] = 0.35f * std::sin(static_cast<float>(index) * 0.071f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.interpolation = svms::InterpolationMode::Linear;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    auto makeVoices = [&]() {
        auto result = std::make_unique<svms::VoiceManager>();
        Check(result->Initialize(voiceCount, 44100u),
              "dense planner differential allocates voice storage");
        for (uint32_t index = 0u; index < voiceCount; ++index) {
            const svms::VoiceHandle voice = result->AllocateVoice(
                static_cast<uint8_t>(index & 15u),
                static_cast<uint8_t>(24u + index % 88u), 127u);
            result->SetVoiceSample(voice, 0u, 128u, 16u, 96u, 1u,
                0.637123f +
                    static_cast<float>(index & 31u) * 0.030731f,
                1u);
            result->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0u, 0u, 0u, 0u,
                                     0.0f, 1.0f, 0.999f);
            result->SetVoiceGain(voice, 0.001f, 0.001f);
            result->SetVoicePlayIndex(voice, index + 1u);
            result->RefreshMixGain(voice,
                channels.GetParams()[index & 15u]);
        }
        return result;
    };

    constexpr uint32_t eventsPerFrame = 32u;
    constexpr uint8_t notes[4] = {39u, 51u, 63u, 75u};
    std::vector<svms::RenderEvent> events(frames * eventsPerFrame);
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        for (uint32_t lane = 0u; lane < eventsPerFrame; ++lane) {
            const uint32_t index = frame * eventsPerFrame + lane;
            events[index].type = svms::RenderEventType::NoteOn;
            events[index].channel = 0u;
            events[index].data1 = notes[lane & 3u];
            events[index].data2 = 127u;
            events[index].frameOffset = frame;
            events[index].ingressSequence = index;
        }
    }

    auto oracleVoices = makeVoices();
    auto plannedVoices = makeVoices();
    svms::RenderScalar oracle;
    svms::RenderScalar planned;
    Check(oracle.ReserveVoiceCapacity(voiceCount) &&
              planned.ReserveVoiceCapacity(voiceCount) &&
              oracle.ConfigureRenderThreads(1u, frames) &&
              planned.ConfigureRenderThreads(4u, frames),
          "dense planner differential starts serial and parallel renderers");
    DensePlannerDispatchContext oracleContext{oracleVoices.get(), &channels};
    DensePlannerDispatchContext plannedContext{plannedVoices.get(), &channels};
    oracle.SetEventBatchDispatcher(DensePlannerDispatch, &oracleContext);
    planned.SetEventBatchDispatcher(DensePlannerDispatch, &plannedContext);
    planned.SetCoverageProfilingEnabledForTest(true);
    std::vector<float> oracleLeft(frames, 0.0f), oracleRight(frames, 0.0f);
    std::vector<float> plannedLeft(frames, 0.0f), plannedRight(frames, 0.0f);
    oracle.RenderBlock(*oracleVoices, channels, samples.data(),
        static_cast<uint32_t>(samples.size()), oracleLeft.data(),
        oracleRight.data(), frames, cfg, events.data(),
        static_cast<uint32_t>(events.size()), true, 5000u);
    g_realtimeAllocationCount.store(0u, std::memory_order_relaxed);
    g_trackRealtimeAllocations = true;
    planned.RenderBlock(*plannedVoices, channels, samples.data(),
        static_cast<uint32_t>(samples.size()), plannedLeft.data(),
        plannedRight.data(), frames, cfg, events.data(),
        static_cast<uint32_t>(events.size()), true, 5000u);
    g_trackRealtimeAllocations = false;
    Check(g_realtimeAllocationCount.load(std::memory_order_relaxed) == 0u,
          "dense planner performs no coordinator-side callback allocation");
    Check(planned.GetCoverageStatsForTest().denseRendered == 1u,
          "dense planner differential exercises the planned worker path");

    float maximumDifference = 0.0f;
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        maximumDifference = (std::max)(maximumDifference,
            std::fabs(oracleLeft[frame] - plannedLeft[frame]));
        maximumDifference = (std::max)(maximumDifference,
            std::fabs(oracleRight[frame] - plannedRight[frame]));
    }
    Check(maximumDifference <= 1.0e-6f,
          "dense planner preserves loop-crossing steal tails at every frame");
    Check(oracleVoices->stealCount_ == plannedVoices->stealCount_ &&
              oracleVoices->GetActiveCount() == plannedVoices->GetActiveCount(),
          "dense planner preserves exact stealing and active voice counts");
    bool equivalent = true;
    for (uint32_t handle = 0u; handle < voiceCount && equivalent; ++handle) {
        equivalent = oracleVoices->v.state[handle] ==
                plannedVoices->v.state[handle] &&
            oracleVoices->v.playIndex[handle] ==
                plannedVoices->v.playIndex[handle] &&
            oracleVoices->v.channel[handle] ==
                plannedVoices->v.channel[handle] &&
            oracleVoices->v.note[handle] == plannedVoices->v.note[handle] &&
            std::fabs(oracleVoices->v.phases[handle] -
                      plannedVoices->v.phases[handle]) <= 1.0e-4f;
    }
    Check(equivalent,
          "dense planner preserves exact voice identities and phase state");

    for (uint32_t threadCount : {2u, 8u}) {
        auto repeatVoices = makeVoices();
        svms::RenderScalar repeat;
        Check(repeat.ReserveVoiceCapacity(voiceCount) &&
                  repeat.ConfigureRenderThreads(threadCount, frames),
              "dense planner starts each deterministic worker count");
        DensePlannerDispatchContext repeatContext{repeatVoices.get(),
                                                   &channels};
        repeat.SetEventBatchDispatcher(DensePlannerDispatch, &repeatContext);
        std::vector<float> repeatLeft(frames, 0.0f);
        std::vector<float> repeatRight(frames, 0.0f);
        repeat.RenderBlock(*repeatVoices, channels, samples.data(),
            static_cast<uint32_t>(samples.size()), repeatLeft.data(),
            repeatRight.data(), frames, cfg, events.data(),
            static_cast<uint32_t>(events.size()), true, 5000u);
        Check(repeatLeft == plannedLeft && repeatRight == plannedRight,
              "dense planner output is deterministic across worker counts");
    }
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

void TestLaunchChurnInstrumentation() {
    auto voices = std::make_unique<svms::VoiceManager>();
    Check(voices->Initialize(1u, 44100u),
          "launch churn fixture allocates one voice");
    voices->SetLaunchChurnProfilingEnabledForTest(true);
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
    setup.presetIndex = 0u;
    setup.regionIndex = 7u;
    svms::VoiceHandle handle = svms::kInvalidVoice;
    Check(voices->LaunchVoiceGroup(0u, 60u, 127u, &setup, 1u, 1u,
                                   channel, &handle),
          "launch churn fixture fills its free slot");

    voices->SetCurrentFrame(64u);
    voices->ResetGroupReuseCountersForTest();
    Check(voices->LaunchVoiceGroup(0u, 60u, 127u, &setup, 1u, 2u,
                                   channel, &handle) &&
              voices->LaunchVoiceGroup(0u, 60u, 127u, &setup, 1u, 3u,
                                       channel, &handle),
          "launch churn fixture performs previous-frame and same-frame steals");
    voices->SetCurrentFrame(65u);
    const svms::LaunchChurnStats& stats =
        voices->GetLaunchChurnStatsForTest();
    Check(stats.logicalLaunches == 2u && stats.successfulLaunches == 2u &&
              stats.physicalVoicesRequested == 2u &&
              stats.physicalVoicesConfigured == 2u,
          "launch churn separates logical and physical launch denominators");
    Check(stats.stealTransactions == 2u && stats.victimGroups == 2u &&
              stats.physicalVictims == 2u &&
              stats.sameFrameVictimGroups == 1u &&
              stats.sameFramePhysicalVictims == 1u,
          "launch churn identifies exact zero-sample victim replacement");
    Check(stats.monoVictimGroups == 2u &&
              stats.stableVictimGroups == 2u &&
              stats.matchingSizeVictimGroups == 2u &&
              stats.matchingPlanVictimGroups == 2u &&
              stats.singleInPlaceVictimGroups == 2u,
          "launch churn classifies stable matching in-place victims");
    Check(stats.nextFrameSurvivingGroups == 1u &&
              stats.nextFrameSurvivingPhysicalVoices == 1u,
          "launch churn counts only the final same-frame survivor");
    Check(stats.tailCaptureAttempts == 2u &&
              stats.tailCaptureAccepted + stats.tailCaptureRejected +
                      stats.tailCaptureIneligible ==
                  stats.tailCaptureAttempts,
          "launch churn accounts for every outgoing-tail decision");
}

} // namespace

int main() {
    TestPostHighPass3Hz();
    TestBankProgramState();
    TestExactPresetLookup();
    TestRegionValidationAndLiveConfiguration();
    TestCompiledSF2ZonesAndExactResolver();
    TestShippedGmSoundFontSmoke();
    TestEnvelopeConversions();
    TestExactFrameBatchDispatch();
    TestExactReleaseDurationAcrossBlocks();
    TestReleaseGeneratorMerging();
    TestCapacitySizedVoiceStorage();
    TestCapacitySizedRendererScratch();
    TestVoiceIdentityAndStealing();
    TestPriorityAwareStealingAndFadeTail();
    TestExactStealHeapAndVoiceIndices();
    TestPersistentStealIndexAgainstOracle();
    TestPagedChannelIndexFragmentation();
    TestChannelTerminationControllers();
    TestOverlappingRetriggerGenerations();
    TestPitchAndDeterministicRender();
    TestConfiguredVelocityMapping();
    TestEventRingWrapAndCapacity();
    TestWindowedSchedulerOrdering();
    TestPagedSchedulerOrderingAndRecycling();
    TestFairPriorityLaneDrain();
    TestFourProducerMPSCIntegrity();
    TestJsonConfigurationLifecycle();
    TestSixHourFrameClockDrift();
    TestOverloadTimelineRecovery();
    TestExpressionAgeRetirementAndLoopWrap();
    TestSpanRendererDifferential();
    TestRenderBackendSelectionAndDenseEquivalence();
    TestTransientClassKernelDifferential();
    TestParallelSustainedRenderDifferential();
    TestDensePlannerOracleDifferential();
    TestLaunchChurnInstrumentation();
    TestRenderCallbackPurity();
    TestCallbackSourcePurity();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }

    std::puts("SVMS V3 correctness tests passed");
    return 0;
}
