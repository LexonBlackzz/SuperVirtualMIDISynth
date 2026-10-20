#include "synth/SVMSTuning.h"
#include "tests/SVMSVibratoGolden.h"
#include "synth/SVMSChannelCache.h"
#include "render/SVMSRenderScalar.h"
#include "synth/SVMSSoundFont.h"
#include "synth/SVMSVoiceManager.h"
#include "render/SVMSHilbertPair.h"
#include "render/SVMSEnvelope.h"
#include "events/SVMSPSCQueue.h"
#include "events/SVMSMPSCQueue.h"
#include "events/SVMSEventScheduler.h"
#include "events/SVMSEventPages.h"
#include "events/SVMSEventCompile.h"
#include "events/SVMSSysEx.h"
#include "events/SVMSMidiByteStream.h"
#include "synth/SVMSConfig.h"
#include "events/SVMSFrameClock.h"
#include "render/SVMSPostFilter.h"
#include "render/SVMSChannelLimiter.h"
#include "events/SVMSNoteOnCollapse.h"
#include "events/SVMSNoteMerge.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <crtdbg.h>
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
// Whole-voice differential tolerances.  Exact mode keeps the oracle numbers;
// the quality.fast_chunk_math test widens both (closed-form phase/gain are
// tolerance-equal and, being one rounding per chunk instead of eight, drift
// from the sequentially-rounded oracle by a fraction of a frame).
float g_waveTol = 2.0e-5f;
float g_phaseTol = 2.0e-3f;

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

    int16_t samples[18]{};
    for (uint32_t i = 0; i < 16; ++i) samples[i] = 8192;
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
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>((0.4f * std::sin(static_cast<float>(i) * 0.031f) +
                      0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);

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
            sampleCount, referenceLeft.data(),
            referenceRight.data(), frames, cfg, events.data(),
            static_cast<uint32_t>(events.size()), true, 10000);
        spanRenderer->RenderBlock(
            *spanVoices, spanChannels, samples.data(),
            sampleCount, spanLeft.data(), spanRight.data(),
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


// ── Hilbert-pair rotation (form 2) ───────────────────────────────────────
// The exact analytic companion x̂ is built by the production builder from
// the sample store; modes 1/2/4 rotate with y = x·cosθ − x̂·sinθ.  The
// frame-major oracle and the production span renderer must agree
// sample-for-sample, and the seeded states must take form 2 (modes 1/2/4
// always rotate with the exact pair; there is no approximate fallback).
void TestHilbertPairDifferential() {
    const uint32_t sampleCount = 4096u;
    // Integer cycle counts so the signal is exactly periodic over the
    // (power-of-two) slice: the FFT-based pair is then the EXACT analytic
    // twin everywhere, edges included, and the only error left is i16
    // quantization.
    const float omega1 = 6.2831853f * 20.0f / static_cast<float>(sampleCount);
    const float omega2 = 6.2831853f * 51.0f / static_cast<float>(sampleCount);
    std::vector<int16_t> samples(4096 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>((0.4f * std::sin(static_cast<float>(i) * omega1) +
                      0.15f * std::cos(static_cast<float>(i) * omega2)) * 32767.0f);

    // The whole slice is played as one loop, so its companion is the
    // periodic transform: the exact analytic twin everywhere, edges included.
    std::vector<int16_t> hilbert(samples.size(), 0);
    svms::BuildHilbertSlice(samples.data(), hilbert.data(),
                            {0u, sampleCount, 0u, sampleCount});

    // Builder sanity: for x = a·sin + b·cos the analytic twin is
    // x̂ = −a·cos + b·sin.
    for (uint32_t i = 0u; i < sampleCount; ++i) {
        const float expected = -0.4f * std::cos(static_cast<float>(i) * omega1) +
                               0.15f * std::sin(static_cast<float>(i) * omega2);
        const float built = static_cast<float>(hilbert[i]) * (1.0f / 32768.0f);
        Check(NearlyEqual(expected, built, 2.0e-4f),
              "periodic companion is the analytic twin");
    }

    // The waveform storm and the steal-free differential that used to live
    // here drove a raw SetVoice* dispatcher through the span renderer, which
    // was the only path that took rotation.  Rotation now rides the
    // whole-voice renderer, and TestWholeVoiceRotation{Steal,Launch}
    // Differential cover modes 1/2/4 (steals, launches, ghost tails) against
    // the frame-major oracle at ~1e-8.

    // Without a companion store there is nothing exact to rotate with: the
    // voice stays unrotated instead of falling back to an approximation.
    svms::VoiceRotationState unpaired{};
    svms::SeedVoiceRotation(unpaired, 1u, 12345u, 44100.0f);
    Check(unpaired.form == 2u, "analytic mode always seeds the exact-pair form");
    Check(svms::RotateVoiceSample(unpaired, 0.37f, nullptr, 0u, 1u, 0.5f) == 0.37f,
          "without a companion store the voice stays unrotated");
}

// Loop-aware analytic companion. A rotated voice is a pure phase shift only
// when x-hat is the Hilbert transform of what the voice actually plays: over
// a loop that is the PERIODIC transform of one loop period. The old builder
// transformed each sample slice as a whole, so x-hat stepped at every loop
// wrap and a rotated sustained voice clicked at the loop rate (buzz) while
// its level depended on the angle.
void TestHilbertPairLoopAware() {
    const double twoPi = 6.283185307179586;
    // Odd (prime) loop length; the even case is checked right below.
    const uint32_t attack = 1500u, loopLength = 347u, tail = 600u;
    const uint32_t loopStart = attack, loopEnd = attack + loopLength;
    const uint32_t frames = loopEnd + tail;
    auto loopWave = [&](uint32_t n) {
        const double p = twoPi * static_cast<double>(n % loopLength) / loopLength;
        return 0.35 * std::sin(3.0 * p) + 0.2 * std::cos(10.0 * p);
    };
    auto loopTwin = [&](uint32_t n) {
        const double p = twoPi * static_cast<double>(n % loopLength) / loopLength;
        return -0.35 * std::cos(3.0 * p) + 0.2 * std::sin(10.0 * p);
    };
    std::vector<int16_t> samples(frames + 8u, 0);
    for (uint32_t i = 0; i < frames; ++i) {
        double x;
        if (i < loopStart)
            x = 0.25 * std::sin(i * 0.4113) + 0.15 * std::sin(i * 0.0731);
        else if (i < loopEnd)
            x = loopWave(i - loopStart);
        else
            x = loopWave(i - loopStart) * std::exp(-static_cast<double>(i - loopEnd) / 200.0);
        samples[i] = static_cast<int16_t>(std::lrint(x * 32767.0));
    }
    std::vector<int16_t> companion(samples.size(), 0);
    svms::BuildHilbertSlice(samples.data(), companion.data(),
                            {0u, frames, loopStart, loopEnd});
    auto at = [&](uint32_t i) { return companion[i] / 32768.0; };

    double loopError = 0.0;
    for (uint32_t n = 0; n < loopLength; ++n)
        loopError = (std::max)(loopError, std::fabs(at(loopStart + n) - loopTwin(n)));
    Check(loopError <= 2.0e-4, "loop companion is the exact periodic analytic twin");
    Check(std::fabs(at(loopEnd) - at(loopStart)) <= 2.0e-4,
          "loop companion wraps seamlessly at the loop end");
    Check(std::fabs(at(loopStart - 1u) - loopTwin(loopLength - 1u)) <= 5.0e-4,
          "attack companion meets the loop companion at loop start");

    // Even loop length (the periodic kernel drops the Nyquist bin).
    {
        const uint32_t evenLength = 348u;
        std::vector<int16_t> evenLoop(evenLength + 8u, 0), evenHat(evenLoop.size(), 0);
        auto phase = [&](uint32_t n) { return twoPi * n / evenLength; };
        for (uint32_t n = 0; n < evenLength; ++n)
            evenLoop[n] = static_cast<int16_t>(std::lrint(
                (0.3 * std::sin(5.0 * phase(n)) + 0.2 * std::cos(17.0 * phase(n))) * 32767.0));
        svms::BuildHilbertSlice(evenLoop.data(), evenHat.data(),
                                {0u, evenLength, 0u, evenLength});
        double evenError = 0.0;
        for (uint32_t n = 0; n < evenLength; ++n) {
            const double twin = -0.3 * std::cos(5.0 * phase(n)) + 0.2 * std::sin(17.0 * phase(n));
            evenError = (std::max)(evenError, std::fabs(evenHat[n] / 32768.0 - twin));
        }
        Check(evenError <= 2.0e-4, "even-length loop companion is the exact analytic twin");
    }

    // One-shot: a burst at the very end of a power-of-two slice must not
    // leak into the slice start (a plain circular FFT puts them side by side).
    std::vector<int16_t> burst(4096u + 8u, 0), burstHat(burst.size(), 0);
    for (uint32_t i = 4032u; i < 4096u; ++i)
        burst[i] = static_cast<int16_t>(0.8 * 32767.0 * std::sin((i - 4032u) * 0.39));
    svms::BuildHilbertSlice(burst.data(), burstHat.data(), {0u, 4096u, 0u, 0u});
    double headLeak = 0.0;
    for (uint32_t i = 0; i < 16u; ++i)
        headLeak = (std::max)(headLeak, std::fabs(burstHat[i] / 32768.0));
    Check(headLeak <= 1.0e-3,
          "one-shot companion does not wrap the slice end into its start");

    // Render-level, production span path: a rotated sustained looping voice
    // keeps the coherent voice's energy at every angle and has no step at
    // the loop wraps.
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.velocityCurve = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    const uint32_t renderFrames = loopStart + 24u * loopLength;
    const uint32_t windowStart = renderFrames - 8u * loopLength;
    auto render = [&](uint32_t mode, float theta) {
        svms::VoiceManager voices;
        voices.Initialize(4, 44100);
        const svms::VoiceHandle h = voices.AllocateVoice(0, 60, 100);
        voices.SetVoiceSample(h, 0, frames, loopStart, loopEnd, 1u, 1.0f, 1);
        voices.SetVoiceEnvelope(h, 1.0f, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f, 0.9997f);
        voices.SetVoiceGain(h, 0.5f, 0.5f);
        voices.RefreshMixGain(h, channels.GetParams()[0]);
        if (mode != 0u) {
            Check(voices.SetPhaseRotationMode(mode), "loop render seeds rotation");
            voices.v.rot[h].c = std::cos(theta);
            voices.v.rot[h].s = std::sin(theta);
        }
        svms::RenderScalar renderer;
        std::vector<float> left(renderFrames, 0.0f), right(renderFrames, 0.0f);
        for (uint32_t done = 0; done < renderFrames;) {
            const uint32_t n = (std::min)(1024u, renderFrames - done);
            renderer.RenderBlock(voices, channels, samples.data(), companion.data(),
                                 frames, left.data() + done, right.data() + done,
                                 n, cfg, nullptr, 0u, true, 10000);
            done += n;
        }
        return left;
    };
    auto energy = [&](const std::vector<float>& signal) {
        double sum = 0.0;
        for (uint32_t n = windowStart; n < renderFrames; ++n)
            sum += static_cast<double>(signal[n]) * signal[n];
        return sum;
    };
    const std::vector<float> coherent = render(0u, 0.0f);
    const double coherentEnergy = energy(coherent);
    Check(coherentEnergy > 1.0e-3, "coherent loop voice is audible");
    for (const float theta : {0.4f, 1.2f, 2.0f, 2.9f, 3.9f, 5.1f}) {
        const std::vector<float> rotated = render(1u, theta);
        const double ratio = energy(rotated) / coherentEnergy;
        if (std::fabs(ratio - 1.0) > 1.0e-3)
            std::fprintf(stderr, "loop rotation energy ratio %.6f at theta %.2f\n", ratio, theta);
        Check(std::fabs(ratio - 1.0) <= 1.0e-3,
              "rotated looping voice keeps its energy at every angle");
        float wrapStep = 0.0f, innerStep = 0.0f;
        for (uint32_t n = windowStart; n < renderFrames; ++n) {
            const float step = std::fabs(rotated[n] - rotated[n - 1u]);
            if ((n - loopStart) % loopLength == 0u)
                wrapStep = (std::max)(wrapStep, step);
            else
                innerStep = (std::max)(innerStep, step);
        }
        Check(wrapStep <= innerStep * 1.05f,
              "rotated looping voice has no step at the loop wrap");
    }

    // Mode 4 (Random) is the exact pair too; it differs from Sweep by a
    // per-voice sweep rate and direction.
    bool sawUp = false, sawDown = false;
    float slowest = 1.0e9f, fastest = 0.0f;
    for (uint64_t seed = 1u; seed <= 64u; ++seed) {
        svms::VoiceRotationState st{};
        svms::SeedVoiceRotation(st, 4u, seed * 7919u, 44100.0f);
        Check(st.form == 2u, "random mode seeds the exact-pair form");
        const float hz = std::atan2(st.ds, st.dc) * 44100.0f / 6.2831853f;
        sawUp = sawUp || hz > 0.0f;
        sawDown = sawDown || hz < 0.0f;
        slowest = (std::min)(slowest, std::fabs(hz));
        fastest = (std::max)(fastest, std::fabs(hz));
    }
    Check(sawUp && sawDown && slowest >= 0.124f && fastest <= 0.376f &&
              fastest - slowest > 0.1f,
          "random mode sweeps each voice at its own rate and direction");
    svms::VoiceRotationState sweep{};
    svms::SeedVoiceRotation(sweep, 2u, 99u, 44100.0f);
    Check(sweep.form == 2u &&
              std::fabs(std::atan2(sweep.ds, sweep.dc) * 44100.0f / 6.2831853f - 0.25f) < 1.0e-4f,
          "sweep mode keeps its fixed 0.25 Hz sweep");
}

void TestRenderBackendSelectionAndDenseEquivalence() {
    constexpr uint32_t voiceCount = 32u;
    constexpr uint32_t frames = 4u;
    const uint32_t sampleCount = 2048u;
    std::vector<int16_t> samples(2048 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(std::sin(static_cast<float>(i) * 0.021f) * 32767.0f);
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
            sampleCount, scalarLeft, scalarRight,
            frames, cfg, nullptr, 0, true, 1000u);
        accelerated.RenderBlock(*acceleratedVoices, channels, samples.data(),
            sampleCount, acceleratedLeft,
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
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(std::sin(static_cast<float>(i) * 0.017f) * 32767.0f);
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
                samples.data(), sampleCount,
                referenceLeft, referenceRight, frames, cfg, nullptr, 0, true,
                4000u);
            span.RenderBlock(*spanVoices, channels, samples.data(),
                sampleCount, spanLeft, spanRight,
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

void TestTransientAVX2LongSpanDifferential() {
    // The AVX2 transient-loop kernel's 8-frame vector chunks only engage for
    // spans longer than 4 frames; this differential exercises them (including
    // mid-span stage transitions and loop wraps) against the per-sample
    // reference.
    constexpr uint32_t voiceCount = 32u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(std::sin(static_cast<float>(i) * 0.017f) * 32767.0f);
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
        {200u, 100u, 2028.5f, 0.4375f},
    };
    const uint32_t spanSizes[] = {7u, 8u, 9u, 64u, 257u};

    if (!svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) return;
    for (const uint32_t frames : spanSizes) {
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
            svms::RenderScalar accelerated;
            Check(accelerated.SetRenderBackend(svms::RenderBackend::AVX2),
                  "AVX2 backend selectable for transient long-span test");
            std::vector<float> referenceLeft(frames, 0.0f),
                referenceRight(frames, 0.0f);
            std::vector<float> spanLeft(frames, 0.0f),
                spanRight(frames, 0.0f);
            reference.RenderBlockReference(*referenceVoices, channels,
                samples.data(), sampleCount,
                referenceLeft.data(), referenceRight.data(), frames, cfg,
                nullptr, 0, true, 4000u);
            accelerated.RenderBlock(*spanVoices, channels, samples.data(),
                sampleCount, spanLeft.data(),
                spanRight.data(), frames, cfg, nullptr, 0, true, 4000u);

            for (uint32_t frame = 0; frame < frames; ++frame) {
                Check(NearlyEqual(referenceLeft[frame], spanLeft[frame],
                                  2.0e-5f) &&
                          NearlyEqual(referenceRight[frame], spanRight[frame],
                                      2.0e-5f),
                      "AVX2 transient long-span kernel matches oracle audio");
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
                      "AVX2 transient long-span kernel preserves envelope state");
            }
            for (uint32_t classIndex = 0;
                 classIndex < svms::kVoiceRenderClassCount; ++classIndex) {
                const auto renderClass =
                    static_cast<svms::VoiceRenderClass>(classIndex);
                Check(referenceVoices->GetRenderClassCount(renderClass) ==
                          spanVoices->GetRenderClassCount(renderClass),
                      "AVX2 transient long-span kernel preserves class transitions");
            }
        }
    }
}



void TestRenderCallbackPurity() {
    constexpr uint32_t frames = 512;
    constexpr uint32_t voiceCount = 256;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096 + 8u, 8192);
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
                          sampleCount, left.data(),
                          right.data(), frames, cfg, nullptr, 0, true, 0);
    g_trackRealtimeAllocations = false;
    Check(g_realtimeAllocationCount.load(std::memory_order_relaxed) == 0,
          "production scalar render performs no heap allocation");
}

void TestParallelSustainedRenderDifferential() {
    constexpr uint32_t voiceCount = 1024u;
    constexpr uint32_t frames = 2048u;
    const uint32_t sampleCount = 8192u;
    std::vector<int16_t> samples(8192u + 8u, 0);
    for (uint32_t index = 0u; index < samples.size(); ++index)
        samples[index] = static_cast<int16_t>(0.4f * std::sin(static_cast<float>(index) * 0.037f) * 32767.0f);

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
    std::fprintf(stderr, "CKPT pre-serial\\n"); std::fflush(stderr);
    std::fprintf(stderr, "M3"); std::fflush(stderr); serial.RenderBlock(*serialVoices, channels, samples.data(),
        sampleCount, serialLeft.data(),
        serialRight.data(), frames, cfg, nullptr, 0u, true, 1000u);
    parallel.RenderBlock(*parallelVoices, channels, samples.data(),
        sampleCount, parallelLeft.data(),
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

void TestWholeVoiceProductionGateParity() {
    // The whole-voice whole-block renderer is the primary production
    // renderer for note material: it must produce audio within the oracle
    // tolerance with correctnessMode disabled (the live configuration) and
    // exercise pools above the historical 8192-voice cap of the removed dense planner.  Audio is
    // compared at the standard 2e-5 tolerance because per-voice whole-block
    // jobs accumulate in worker-count-dependent order (and the accelerated
    // backend is tolerance-equal, never bit-replacing, the scalar path).
    constexpr uint32_t voiceCount = 3000u;
    constexpr uint32_t frames = 256u;
    constexpr uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t index = 0u; index < samples.size(); ++index)
        samples[index] = static_cast<int16_t>(
            0.35f * std::sin(static_cast<float>(index) * 0.071f) * 32767.0f);

    svms::ChannelCache channels;
    auto makeVoices = [&]() {
        auto result = std::make_unique<svms::VoiceManager>();
        Check(result->Initialize(voiceCount, 44100u),
              "production gate parity allocates voice storage");
        for (uint32_t index = 0u; index < voiceCount; ++index) {
            const svms::VoiceHandle voice = result->AllocateVoice(
                static_cast<uint8_t>(index & 15u),
                static_cast<uint8_t>(24u + index % 88u), 127u);
            result->SetVoiceSample(voice, 0u, 128u, 16u, 96u, 1u,
                0.637123f + static_cast<float>(index & 31u) * 0.030731f, 1u);
            result->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0u, 0u, 0u, 0u,
                                     0.0f, 1.0f, 0.999f);
            result->SetVoiceGain(voice, 0.001f, 0.001f);
            result->SetVoicePlayIndex(voice, index + 1u);
            result->RefreshMixGain(voice,
                channels.GetParams()[index & 15u]);
        }
        return result;
    };

    constexpr uint32_t eventsPerFrame = 8u;
    constexpr uint8_t notes[4] = {39u, 51u, 63u, 75u};
    std::vector<svms::RenderEvent> events(frames * eventsPerFrame);
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        for (uint32_t lane = 0u; lane < eventsPerFrame; ++lane) {
            const uint32_t index = frame * eventsPerFrame + lane;
            events[index].type = svms::RenderEventType::NoteOn;
            events[index].channel = static_cast<uint8_t>(lane & 15u);
            events[index].data1 = notes[lane & 3u];
            events[index].data2 = 127u;
            events[index].frameOffset = frame;
            events[index].ingressSequence = index;
        }
    }

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.interpolation = svms::InterpolationMode::Linear;
    cfg.correctnessMode = false;

    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    auto productionVoices = makeVoices();
    auto referenceVoices = makeVoices();
    auto serialVoices = makeVoices();
    svms::RenderScalar production;
    svms::RenderScalar reference;
    svms::RenderScalar serial;
    Check(production.ReserveVoiceCapacity(voiceCount) &&
              reference.ReserveVoiceCapacity(voiceCount) &&
              production.ConfigureRenderThreads(6u, frames) &&
              reference.ConfigureRenderThreads(4u, frames) &&
              serial.ConfigureRenderThreads(1u, frames),
          "production gate parity starts parallel and serial renderers");
    DensePlannerDispatchContext productionContext{productionVoices.get(),
                                                   &channels};
    DensePlannerDispatchContext referenceContext{referenceVoices.get(),
                                                  &channels};
    DensePlannerDispatchContext serialContext{serialVoices.get(), &channels};
    production.SetEventBatchDispatcher(DensePlannerDispatch,
                                        &productionContext);
    reference.SetEventBatchDispatcher(DensePlannerDispatch, &referenceContext);
    serial.SetEventBatchDispatcher(DensePlannerDispatch, &serialContext);
    std::vector<float> productionLeft(frames, 0.0f),
        productionRight(frames, 0.0f);
    std::vector<float> referenceLeft(frames, 0.0f),
        referenceRight(frames, 0.0f);
    production.RenderBlock(*productionVoices, channels, samples.data(),
        sampleCount, productionLeft.data(), productionRight.data(), frames,
        cfg, events.data(), static_cast<uint32_t>(events.size()), true,
        5000u);
    reference.RenderBlock(*referenceVoices, channels, samples.data(),
        sampleCount, referenceLeft.data(), referenceRight.data(), frames,
        cfg, events.data(), static_cast<uint32_t>(events.size()), true,
        5000u);
    std::vector<float> serialLeft(frames, 0.0f), serialRight(frames, 0.0f);
    serial.RenderBlock(*serialVoices, channels, samples.data(),
        sampleCount, serialLeft.data(), serialRight.data(), frames,
        cfg, events.data(), static_cast<uint32_t>(events.size()), false,
        5000u);
    Check(production.GetWholeVoiceBlocksForTest() == 1u,
          "production configuration routes the callback through the whole-voice renderer");
    {
        float maxDiff = 0.0f; uint32_t firstFrame = UINT32_MAX;
        for (uint32_t frame = 0u; frame < frames; ++frame) {
            const float dl = std::fabs(productionLeft[frame] - referenceLeft[frame]);
            const float dr = std::fabs(productionRight[frame] - referenceRight[frame]);
            if (dl > maxDiff) maxDiff = dl;
            if (dr > maxDiff) maxDiff = dr;
            if ((dl != 0.0f || dr != 0.0f) && firstFrame == UINT32_MAX) firstFrame = frame;
        }
        fprintf(stderr, "PARITYDIFF max=%g first=%u\\n", maxDiff, firstFrame);
    }
    float maxParityDifference = 0.0f;
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        maxParityDifference = (std::max)(maxParityDifference,
            std::fabs(productionLeft[frame] - referenceLeft[frame]));
        maxParityDifference = (std::max)(maxParityDifference,
            std::fabs(productionRight[frame] - referenceRight[frame]));
    }
    Check(maxParityDifference <= 2.0e-5f,
          "whole-voice renderer matches the reference within oracle tolerance");
}

void TestCallbackSourcePurity() {
    const std::filesystem::path sourcePath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "driver" / "SVMSDriver.cpp";
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

void TestVibratoModulationAudible() {
    {
        // Regions compiled without any vibrato generator must still carry
        // the SF2 built-in default modulator depth (50 cents) and the
        // default 1 Hz LFO rate, or stock SoundFonts stay static.
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
        data->generators[0] = {svms::Gen_Instrument, 0};
        data->pgenCount = 2;
        data->instrumentZones[0].generatorIndex = 2;
        data->instrumentZones[1].generatorIndex = 3;
        data->instrumentZoneCount = 2;
        data->generators[2] = {svms::Gen_SampleID, 0};
        data->generatorCount = 4;
        svms::sf2_build_regions(data.get());
        Check(data->regionCount == 1 &&
                  data->regions[0].vibLfoToPitch == 50 &&
                  data->regions[0].freqVibLfo == 0,
              "regions without vibrato generators use SF2 default modulator values");
    }

    constexpr uint32_t kSampleFrames = 64u;
    int16_t sampleBuf[kSampleFrames + 8u]{};
    for (uint32_t i = 0; i < kSampleFrames; ++i) {
        sampleBuf[i] = static_cast<int16_t>(std::sin(static_cast<float>(i) *
            (6.283185307179586f / 64.0f)) * 0.5f * 32767.0f);
    }

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;

    auto renderPass = [&](bool modulated, std::vector<float>& out) {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(4, 44100);
        const svms::VoiceHandle handle = voices->AllocateVoice(0, 60, 100);
        voices->SetVoiceSample(handle, 0, kSampleFrames - 1u, 0,
                               kSampleFrames - 1u, 1, 1.0f, 1);
        voices->SetVoiceEnvelope(handle, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->SetVoiceGain(handle, 1.0f, 1.0f);
        // The driver seeds these from the prepared region at note-on; the
        // direct-launch fixture mirrors that with the resolved defaults.
        voices->v.vibLfoToPitchCents[handle] = 50.0f;
        voices->v.vibLfoSteps[handle] = 5.0f / 44100.0f;

        svms::ChannelCache channels;
        channels.SetMasterVolume(1.0f);
        if (modulated)
            channels.ControlChange(0, 1, 127);
        channels.RebuildCache(cfg, 44100.0f);
        voices->RefreshMixGain(handle, channels.GetParams()[0]);
        if (modulated) {
            Check(channels.GetParams()[0].modDepth > 0.99f,
                  "diagnostic: CC1 reaches the channel modulation depth");
        }

        out.clear();
        out.reserve(4096u);
        svms::RenderScalar renderer;
        float blockLeft[kSampleFrames]{}, blockRight[kSampleFrames]{};
        for (uint32_t block = 0; block < 64u; ++block) {
            renderer.RenderBlock(*voices, channels, sampleBuf, kSampleFrames,
                                 blockLeft, blockRight, kSampleFrames, cfg);
            if (modulated && block == 32u) {
                Check(std::fabs(voices->v.phaseIncs[handle] -
                                voices->v.basePhaseIncs[handle]) > 1.0e-9f,
                      "diagnostic: modulated span rewrites the voice phase increment");
            }
            out.insert(out.end(), blockLeft, blockLeft + kSampleFrames);
        }
    };

    std::vector<float> plain, plainAgain, modulated;
    renderPass(false, plain);
    renderPass(false, plainAgain);
    Check(plain == plainAgain,
          "unmodulated vibrato gating leaves output byte-identical");

    renderPass(true, modulated);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < plain.size(); ++i) {
        maxDiff = (std::max)(maxDiff,
                             std::fabs(plain[i] - modulated[i]));
    }
    Check(maxDiff > 0.01f,
          "full-scale CC1 produces audible vibrato deviation");
}

void TestNoteOnCollapseGate() {
    svms::NoteOnCollapseGate gate;
    // DEFAULT STATE: coalescing is OFF. Every hit spawns — retrigger
    // timing/buzz precision is untouched unless explicitly enabled.
    Check(!gate.Enabled(), "coalescing defaults to off");
    {
        uint32_t stack = 0;
        bool allSpawn = true;
        for (uint32_t i = 0; i < 1000u; ++i) {
            allSpawn = allSpawn && gate.OnNoteOn(7u, 1000u + i, stack);
        }
        Check(allSpawn, "default gate spawns every note-on");
    }

    // Threshold setter: power-of-two accept, round-down, disable.
    gate.SetThreshold(128u);
    Check(gate.Threshold() == 128u,
          "threshold setter accepts a power of two");
    gate.SetThreshold(100u);
    Check(gate.Threshold() == 64u, "threshold rounds down to a power of two");
    gate.SetThreshold(1u);
    Check(gate.Threshold() == 1u, "threshold 1 disables coalescing");

    // Enable with a fixed QPC window (10 ms at 1 tick = 1 microsecond in
    // this test's synthetic clock, i.e. windowTicks = 10000).
    gate.SetThreshold(128u);
    const uint64_t kWin = 10000u;
    gate.SetWindowTicks(kWin);
    Check(gate.Enabled(), "gate enables with threshold and window set");

    // First strike on a key always spawns (latency must be preserved).
    Check(gate.OnNoteOn(0u, 1000u), "first note-on on a key spawns a voice");

    // Hits inside the same window: exactly one more spawn at the
    // threshold crossing (hit 128); everything else collapses.
    uint32_t spawns = 0;
    for (uint32_t i = 1u; i < 200u; ++i) {
        // Timestamps inside the first window (anchor 1000, window 10000).
        if (gate.OnNoteOn(0u, 1000u + i)) ++spawns;
    }
    Check(spawns == 1u, "a key inside one window spawns once per threshold repeats");

    // Frame-size independence: the window is pure time. The crossing
    // spawn at hit 128 re-anchored the window at qpc 1128, so expiry is
    // 1128 + window. A hit past that spawns immediately, no matter how
    // "blocks" would fall.
    Check(gate.OnNoteOn(0u, 1128u + kWin + 1u),
          "a hit after the window expires spawns immediately");

    // A different key is independent and still spawns on its first hit.
    Check(gate.OnNoteOn(1u, 11000u), "a different key spawns on its first hit");

    // Note-off reset clears the key: the next hit spawns immediately.
    gate.ResetKey(0u);
    Check(gate.OnNoteOn(0u, 11500u), "note-off reset makes the next hit spawn");

    // Channel reset clears the whole channel row.
    gate.OnNoteOn(2u * svms::kNoteCount + 60u, 12000u);
    gate.OnNoteOn(2u * svms::kNoteCount + 61u, 12001u);
    gate.ResetChannel(2u);
    Check(gate.OnNoteOn(2u * svms::kNoteCount + 60u, 12002u) &&
              gate.OnNoteOn(2u * svms::kNoteCount + 61u, 12003u),
          "channel reset makes every key on the channel spawn");

    // Reset-all clears even the last key index.
    gate.OnNoteOn(svms::NoteOnCollapseGate::kKeyCount - 1u, 13000u);
    gate.ResetAll();
    Check(gate.OnNoteOn(svms::NoteOnCollapseGate::kKeyCount - 1u, 13001u),
          "reset-all makes any key spawn");

    // Velocity stacking: hits between spawns accumulate per key and are
    // reported to the caller so density can become loudness.
    gate.ResetAll();
    gate.SetThreshold(4u);
    uint32_t stack = 0;
    Check(gate.OnNoteOn(9u, 20000u, stack) && stack == 1u,
          "first hit spawns with stack 1");
    Check(!gate.OnNoteOn(9u, 20001u, stack) && stack == 1u,
          "second hit collapses with running stack 1");
    Check(!gate.OnNoteOn(9u, 20002u, stack) && stack == 2u,
          "third hit collapses with running stack 2");
    Check(!gate.OnNoteOn(9u, 20003u, stack) && stack == 3u,
          "fourth hit collapses with running stack 3");
    Check(gate.OnNoteOn(9u, 20004u, stack) && stack == 4u,
          "threshold crossing spawns with the accumulated stack");
    Check(!gate.OnNoteOn(9u, 20005u, stack) && stack == 1u,
          "a new cycle starts right after a spawn");
    // A different key's stack is untouched by the hammering above.
    Check(gate.OnNoteOn(10u, 20006u, stack) && stack == 1u,
          "stacks are tracked per key");
    // Reset clears the stack too.
    gate.OnNoteOn(11u, 20007u);
    gate.OnNoteOn(11u, 20008u, stack);
    gate.ResetKey(11u);
    Check(gate.OnNoteOn(11u, 20009u, stack) && stack == 1u,
          "note-off reset clears the accumulated stack");

    // Sustained hammering within a continuously re-anchored window keeps
    // the 1-in-N ratio exact (each spawn re-anchors the window, so the
    // rate is deterministic regardless of buffer size).
    gate.ResetAll();
    gate.SetThreshold(128u);
    const uint32_t threshold = 128u;
    uint64_t totalSpawns = 0;
    const uint32_t kHammerHits = threshold * 10u + 5u;
    for (uint32_t i = 0; i < kHammerHits; ++i) {
        if (gate.OnNoteOn(42u, 30000u + i)) ++totalSpawns;
    }
    // Hit 0 spawns (new window), plus every threshold crossing while the
    // re-anchored window still covers the stream (10 crossings for 1285
    // hits at threshold 128; the tail re-anchors keep counting).
    Check(totalSpawns == 1u + (kHammerHits - 1u) / threshold,
          "spawn ratio over sustained hammering is 1 per threshold");

    // Window 0 disables collapsing even with a threshold set.
    gate.SetWindowTicks(0u);
    Check(gate.OnNoteOn(50u, 40000u) && gate.OnNoteOn(50u, 40001u),
          "window 0 forces every hit to spawn");
}

// ═══════════════════════════════════════════════════════════════════════
// Whole-voice whole-block renderer differentials
//
// The whole-voice renderer dispatches every event in ingress order during
// a pre-pass (with the render clock at each event's exact frame) and then
// renders each voice once for the whole block.  The oracle renders
// frame-major, dispatching at the same exact frames mid-render.  Steal
// victim choice therefore sees block-start gains in the pre-pass where
// the oracle sees already-attacked gains after an in-block launch — an
// accepted, documented semantic change (see the SVMSRenderScalar.h
// header comment).  Both scenarios below keep victim choice drift-proof:
// no candidate changes gain mid-block, so with one steal per block both
// sides choose identical victims.
//
// Per-sample audio is compared at the standard 2e-5 oracle tolerance (the
// whole-voice renderer accumulates per voice in worker-count-dependent
// order; the accelerated backends are tolerance-equal, never
// bit-replacing, the scalar path).
// ═══════════════════════════════════════════════════════════════════════

struct WholeVoiceRng {
    uint32_t state = 0x9E3779B9u;
    uint32_t Next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
};

void ConfigureWholeVoiceSeedPool(svms::VoiceManager& voices,
                                 const svms::ChannelCache& channels,
                                 uint32_t poolSize, uint32_t seedCount,
                                 bool transientSeeds = true,
                                 bool vibratoSeeds = false) {
    // Modeled on ConfigureDifferentialSeed but every seeded voice is Active
    // and immortal: the whole-voice scenarios need a candidate pool whose
    // state is identical at block start and mid-block.  ConfigureDifferentialSeed's
    // releasing branch cannot be overridden back to Active (StartRelease has
    // no public inverse), so the seed builds its own pool.  The gain ladder
    // values are pairwise distinct (2x steps within each i%9 residue class,
    // monotone across periods), so the level-based steal score always has
    // an unambiguous quietest candidate.
    Check(voices.Initialize(poolSize, 44100u),
          "whole-voice seed pool allocates voice storage");
    for (uint32_t i = 0; i < seedCount; ++i) {
        const uint8_t channel = static_cast<uint8_t>(i & 3u);
        const uint8_t note = static_cast<uint8_t>(48u + i % 24u);
        const svms::VoiceHandle voice = voices.AllocateVoice(
            channel, note, static_cast<uint8_t>(48u + i));
        const bool loop = (i % 4u) != 1u;
        // Every 8th seed (i % 8 == 5, always non-loop) runs a real
        // attack/decay envelope: the transient one-shot state machine and
        // its AVX2 kernel need stage-1/2 coverage against the oracle, and
        // the decay settles at the sustain level well inside the scenario
        // so the pool stays shrink-free afterwards.
        const bool transientSeed = transientSeeds && (i % 8u) == 5u;
        // Non-loop (one-shot) seed voices get a crawl-speed phase step so
        // none of them can reach relEnd and retire inside the scenario: a
        // mid-block retirement would change the active set between the
        // whole-voice pre-pass and the oracle's mid-block dispatch, which
        // is the documented drift the scenarios must avoid.
        voices.SetVoiceSample(voice, 0, 4096, 64, 4032, loop ? 1u : 0u,
                              loop ? 0.4f + static_cast<float>(i % 17u) * 0.11f
                                   : 0.05f,
                              1);
        voices.v.phases[voice] = static_cast<float>((i * 37u) % 1700u) + 0.375f;
        const float gain =
            0.004f * std::ldexp(1.0f, static_cast<int>(i % 9u)) *
            (1.0f + 0.01f * static_cast<float>(i));
        // Immortal envelope pinned to the ladder gain (sustain is stored as
        // a fraction of the initial level, hence the 1.0f): stage 3 holds
        // forever and releaseSamplesRemaining stays UINT32_MAX, so the pool
        // never shrinks and no candidate ever retires mid-scenario.
        if (transientSeed) {
            voices.SetVoiceEnvelope(voice, gain, 0.35f, 0, 0, 64, 3000,
                                    gain / 64.0f, 0.999f, 0.999f);
        } else {
            voices.SetVoiceEnvelope(voice, gain, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f,
                                    0.999f);
        }
        voices.SetVoicePlayIndex(voice, i + 1u);
        voices.SetVoiceGain(voice, 0.02f, 0.02f);
        voices.RefreshMixGain(voice, channels.GetParams()[channel]);
        if (vibratoSeeds) {
            // SF2 default vibrato modulator (5 Hz triangle, 50 cents):
            // gives the whole-voice LFO windows and the sparse vibrato
            // reference real per-voice modulation to disagree about.
            voices.v.vibLfoToPitchCents[voice] = 50.0f;
            voices.v.vibLfoSteps[voice] = 5.0f / 44100.0f;
        }
    }
}

void CheckWholeVoiceStateParity(const svms::VoiceManager& oracleVoices,
                                const svms::VoiceManager& wholeVoices,
                                const char* label) {
    // Slot handles are internal bookkeeping: the whole-voice renderer applies
    // its deferred retirements at block end (sorted by exact frame), so freed
    // slots are recycled in a different order than the frame-major oracle,
    // which frees them inline mid-block.  Voice identity is therefore the
    // playIndex (unique per launch); the per-sample audio parity pins the
    // audible behavior.
    std::vector<uint32_t> oracleActive(
        oracleVoices.activeList_,
        oracleVoices.activeList_ + oracleVoices.activeCount_);
    std::vector<uint32_t> wholeActive(
        wholeVoices.activeList_,
        wholeVoices.activeList_ + wholeVoices.activeCount_);
    auto activePlayIndices = [](const svms::VoiceManager& vm,
                                const std::vector<uint32_t>& active) {
        std::vector<uint32_t> ids;
        ids.reserve(active.size());
        for (uint32_t voice : active) ids.push_back(vm.v.playIndex[voice]);
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const std::vector<uint32_t> oracleIds =
        activePlayIndices(oracleVoices, oracleActive);
    const std::vector<uint32_t> wholeIds =
        activePlayIndices(wholeVoices, wholeActive);
    if (oracleIds != wholeIds) {
        std::fprintf(stderr,
                     "[WVDIFFV] %s active mismatch: oracle=%zu whole=%zu\n",
                     label, oracleIds.size(), wholeIds.size());
        const size_t n = (std::min)(oracleIds.size(), wholeIds.size());
        for (size_t i = 0; i < n; ++i) {
            if (oracleIds[i] != wholeIds[i]) {
                std::fprintf(stderr,
                    "[WVDIFFV] %s first playIndex divergence at %zu: oracle=%u whole=%u\n",
                    label, i, oracleIds[i], wholeIds[i]);
                break;
            }
        }
    }
    Check(oracleIds == wholeIds,
          "whole-voice preserves active voice identity and retirement");
    if (oracleIds == wholeIds) {
        uint32_t dumped = 0u;
        for (uint32_t voice : oracleActive) {
            const uint32_t playIndex = oracleVoices.v.playIndex[voice];
            uint32_t wholeVoice = UINT32_MAX;
            for (uint32_t candidate : wholeActive) {
                if (wholeVoices.v.playIndex[candidate] == playIndex) {
                    wholeVoice = candidate;
                    break;
                }
            }
            if (wholeVoice == UINT32_MAX) continue;
            const bool stateMatches =
                oracleVoices.v.state[voice] == wholeVoices.v.state[wholeVoice] &&
                NearlyEqual(oracleVoices.v.phases[voice],
                            wholeVoices.v.phases[wholeVoice], g_phaseTol) &&
                NearlyEqual(oracleVoices.v.currentGain[voice],
                            wholeVoices.v.currentGain[wholeVoice], 2.0e-5f) &&
                oracleVoices.v.releaseSamplesRemaining[voice] ==
                    wholeVoices.v.releaseSamplesRemaining[wholeVoice];
            if (!stateMatches) {
                if (dumped++ < 8u) {
                    std::fprintf(stderr,
                        "[WVDIFFV] %s playIndex=%u state=%u/%u phase=%g/%g gain=%g/%g release=%u/%u\n",
                        label, playIndex,
                        oracleVoices.v.state[voice],
                        wholeVoices.v.state[wholeVoice],
                        oracleVoices.v.phases[voice],
                        wholeVoices.v.phases[wholeVoice],
                        oracleVoices.v.currentGain[voice],
                        wholeVoices.v.currentGain[wholeVoice],
                        oracleVoices.v.releaseSamplesRemaining[voice],
                        wholeVoices.v.releaseSamplesRemaining[wholeVoice]);
                }
                Check(false, "whole-voice preserves active voice state");
            }
        }
    }
    for (uint32_t renderClass = 0u;
         renderClass < svms::kVoiceRenderClassCount; ++renderClass) {
        const auto klass = static_cast<svms::VoiceRenderClass>(renderClass);
        const uint32_t oracleCount = oracleVoices.GetRenderClassCount(klass);
        const uint32_t wholeCount = wholeVoices.GetRenderClassCount(klass);
        if (oracleCount != wholeCount) {
            std::fprintf(stderr,
                         "[WVDIFFV] %s class %u: oracle=%u whole=%u\n",
                         label, renderClass, oracleCount, wholeCount);
        }
        Check(oracleCount == wholeCount,
              "whole-voice preserves per-class render lists");
    }
    Check(oracleVoices.stealCount_ == wholeVoices.stealCount_,
          "whole-voice preserves the steal counter");
    Check(oracleVoices.GetStealTailCount() == wholeVoices.GetStealTailCount(),
          "whole-voice preserves the steal-tail census");
}

struct WholeVoiceStealContext {
    svms::VoiceManager* voices;
    const svms::ChannelParamsSnapshot* channelParams;
    uint32_t playIndex = 5000u;
    uint32_t launches = 0u;
};

void DispatchWholeVoiceStealEvent(const svms::RenderEvent* events,
                                  uint32_t eventCount, uint32_t,
                                  void* userData) {
    auto& context = *static_cast<WholeVoiceStealContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        if (event.type != svms::RenderEventType::NoteOn) continue;
        // Production launch path (LaunchVoiceGroup -> ConfigureVoice): the
        // whole-voice pre-pass derives each newborn's exact startFrame from
        // the voice-configured hook, which only the configuration
        // transaction fires.
        svms::VoiceConfiguration setup{};
        setup.sampleStart = 0u;
        setup.sampleEnd = 4096u;
        setup.loopStart = 64u;
        setup.loopEnd = 4032u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep =
            0.637f + event.data1 * 0.011f;
        // Launched voices sustain far above every ladder gain so the
        // level-based victim score can never select one of them.  Sustain
        // is stored as a fraction of the initial level, hence the 1.0f.
        setup.initialGain =
            2.0f + 0.05f * static_cast<float>(context.launches + 1u);
        setup.sustainLevel = 1.0f;
        setup.attackGainStep = 0.0f;
        setup.decaySlope = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.02f;
        setup.sampleBacked = 1u;
        setup.playIndex = context.playIndex++;
        svms::VoiceHandle handle = svms::kInvalidVoice;
        context.voices->LaunchVoiceGroup(
            event.channel, event.data1, event.data2, &setup, 1u,
            setup.playIndex,
            context.channelParams[event.channel], &handle);
        if (handle != svms::kInvalidVoice) ++context.launches;
    }
}

void RunWholeVoiceStealDifferential(uint32_t rotationMode) {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 6u;
    constexpr uint32_t kPool = 48u;
    // No transient seeds here: mid-block envelope decay changes steal
    // scores between the whole-voice pre-pass (which scores launches
    // against block-start state) and the oracle's per-frame dispatch —
    // the documented whole-voice drift, amplified by evolving gains into
    // observable victim divergence.  Transient rendering is
    // oracle-verified by the launch/CC differentials, whose launches do
    // not straddle mid-block score evolution.
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.4f * std::sin(static_cast<float>(i) * 0.031f) +
             0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);
    // Rotation modes need the exact analytic companion store.
    std::vector<int16_t> hilbert(samples.size(), 0);
    svms::BuildHilbertSlice(samples.data(), hilbert.data(),
                            {0u, sampleCount, 0u, sampleCount});
    const int16_t* hilbertPtr = rotationMode != 0u ? hilbert.data() : nullptr;

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
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool, kPool,
                                      false);
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        if (rotationMode != 0u) {
            Check(oracleVoices->SetPhaseRotationMode(rotationMode) &&
                      wholeVoices->SetPhaseRotationMode(rotationMode),
                  "steal differential seeds rotation states");
        }
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache wholeChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames),
              "whole-voice steal differential starts serial and parallel "
              "renderers");
        Check(whole.SetRenderBackend(avx2Pass ? svms::RenderBackend::AVX2
                                              : svms::RenderBackend::Scalar),
              "whole-voice steal differential selects the requested backend");

        WholeVoiceStealContext oracleContext{oracleVoices.get(),
                                             oracleChannels.GetParams()};
        WholeVoiceStealContext wholeContext{wholeVoices.get(),
                                            wholeChannels.GetParams()};
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceStealEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceStealEvent,
                                      &wholeContext);

        // Fresh PRNG per pass so both backends walk identical scenarios.
        WholeVoiceRng rng;
        float passMaxDiff = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 10000u + block * kFrames;
            svms::RenderEvent event{};
            event.type = svms::RenderEventType::NoteOn;
            event.channel = static_cast<uint8_t>(rng.Next() % 4u);
            event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
            event.data2 = static_cast<uint8_t>(64u + rng.Next() % 64u);
            // Mid-block steal frame.  Late frames push the 64-frame tail
            // fade past the block boundary, exercising ghost-tail adoption
            // into real steal-tail slots on the following block.
            event.frameOffset = 32u + rng.Next() % 469u;
            event.ingressSequence = block;

            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f);
            std::vector<float> wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), hilbertPtr,
                sampleCount,
                oracleLeft.data(), oracleRight.data(), kFrames, cfg, &event,
                1u, true, blockStartFrame);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              hilbertPtr, sampleCount, wholeLeft.data(),
                              wholeRight.data(), kFrames, cfg, &event, 1u,
                              true, blockStartFrame);

            float blockDiff = 0.0f;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleLeft[frame] - wholeLeft[frame]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleRight[frame] - wholeRight[frame]));
            }
            std::fprintf(stderr,
                         "[WVDIFF] backend=%s rot=%u steal-block=%u max=%g\n",
                         backendName, rotationMode, block, blockDiff);
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        Check(passMaxDiff <= g_waveTol,
              "whole-voice in-block stealing matches the oracle per sample");
        CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, backendName);
        if (rotationMode != 0u) {
            bool pairForm = wholeVoices->v.rot != nullptr;
            for (uint32_t k = 0; pairForm && k < wholeVoices->activeCount_; ++k)
                pairForm = wholeVoices->v.rot[wholeVoices->activeList_[k]].form == 2u;
            Check(pairForm, "rotated whole-voice pool keeps the exact pair form");
        }
        Check(whole.GetWholeVoiceBlocksForTest() == kBlocks,
              "steal scenario routes every block through the whole-voice "
              "renderer");
        Check(oracleVoices->stealCount_ == wholeVoices->stealCount_ &&
                  wholeVoices->stealCount_ == kBlocks,
              "every saturated-pool launch steals exactly one victim");
    }
}

void TestWholeVoiceStealDifferential() { RunWholeVoiceStealDifferential(0u); }

// Rotated pools (Analytic/Sweep/Random pair form) ride the whole-voice path:
// ghosts carry their rotation state into the in-band tail, and adopted tails
// continue it.  Same scenario and oracle as the Coherent steal differential.
void TestWholeVoiceRotationStealDifferential() {
    for (uint32_t mode : {1u, 2u, 4u}) RunWholeVoiceStealDifferential(mode);
}

struct WholeVoiceLaunchContext {
    svms::VoiceManager* voices;
    const svms::ChannelParamsSnapshot* channelParams;
    uint32_t playIndex = 9000u;
    uint32_t launches = 0u;
    uint32_t releaseSamples = 400u;
    bool allLoop = false;
};

void DispatchWholeVoiceLaunchEvent(const svms::RenderEvent* events,
                                   uint32_t eventCount, uint32_t,
                                   void* userData) {
    auto& context = *static_cast<WholeVoiceLaunchContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        switch (event.type) {
            case svms::RenderEventType::NoteOn: {
                // Production launch path (LaunchVoiceGroup -> ConfigureVoice):
                // the whole-voice pre-pass derives each newborn's exact
                // startFrame from the voice-configured hook.  The pool is
                // kept unsaturated so launches never steal.
                const bool loop = context.allLoop || (context.playIndex % 3u) != 0u;
                svms::VoiceConfiguration setup{};
                setup.sampleStart = 0u;
                setup.sampleEnd = 4096u;
                setup.loopStart = 64u;
                setup.loopEnd = 4032u;
                setup.loopMode = loop ? 1u : 0u;
                setup.phaseStep = setup.basePhaseStep =
                    0.637f + event.data1 * 0.011f;
                // Attack+decay transient (~70 frames) into sustain, then a
                // release capped at 400 samples: covers transient->sustained
                // class changes, exact-frame deferred releases and
                // counter-governed retirement.
                setup.initialGain = 0.6f;
                setup.sustainLevel = 0.5f;
                setup.attackSamples = 17u;
                setup.attackGainStep = 0.6f / 17.0f;
                setup.decaySamples = 53u;
                setup.decaySlope = 0.99f;
                setup.releaseDecay = 0.9995f;
                setup.releaseSamples = context.releaseSamples;
                setup.gainLeft = setup.gainRight = 0.02f;
                setup.sampleBacked = 1u;
                setup.playIndex = context.playIndex++;
                svms::VoiceHandle handle = svms::kInvalidVoice;
                context.voices->LaunchVoiceGroup(
                    event.channel, event.data1, event.data2, &setup, 1u,
                    setup.playIndex,
                    context.channelParams[event.channel], &handle);
                if (handle != svms::kInvalidVoice) ++context.launches;
                break;
            }
            case svms::RenderEventType::NoteOff:
                for (uint32_t i = 0; i < context.voices->activeCount_; ++i) {
                    const uint32_t voice = context.voices->activeList_[i];
                    if (context.voices->v.channel[voice] == event.channel &&
                        context.voices->v.note[voice] == event.data1) {
                        context.voices->StartRelease(voice);
                    }
                }
                break;
            case svms::RenderEventType::Reset:
                context.voices->Reset();
                break;
            default:
                break;
        }
    }
}

// Barrier events (Reset, AllSoundOff) split the block: the stretches before
// and after render on the whole-voice path and the barrier dispatches at its
// exact frame.  The oracle dispatches the same events in the same order.
// Voice decimation (opt-in).  A static pool with distinct velocities: the
// whole-voice renderer must mute exactly the voices the frame-major oracle
// leaves out (everything below the loudest activeCount/step by velocity),
// keep their state advancing, and leave the audio untouched when disabled.
// Retire floor on counted release tails: default floor leaves the count
// alone; a raised floor cuts it to the frame the tail reaches the floor.
void TestFloorLimitedReleaseSamples() {
    const float decay = svms::MakeReleaseDecay(1.5f, 44100u);
    const uint32_t samples = svms::MakeReleaseSamples(1.5f, 44100u);
    const float saved = svms::VoiceRetireThreshold();
    svms::g_voiceRetireThreshold.store(svms::kVoiceRetireThreshold);
    Check(svms::FloorLimitedReleaseSamples(samples, decay, 1.0f) == samples,
          "default floor changes the release count");
    const float floorGain = std::pow(10.0f, -50.0f / 20.0f);
    svms::g_voiceRetireThreshold.store(floorGain);
    const uint32_t cut = svms::FloorLimitedReleaseSamples(samples, decay, 0.8f);
    Check(cut < samples && cut > samples / 2u, "raised floor did not shorten the tail");
    Check(0.8f * std::pow(decay, static_cast<float>(cut)) <= floorGain * 1.001f &&
          0.8f * std::pow(decay, static_cast<float>(cut - 1u)) > floorGain * 0.999f,
          "cut count is not the floor crossing frame");
    Check(svms::FloorLimitedReleaseSamples(samples, decay, floorGain * 0.5f) == 1u,
          "voice below the floor does not retire at once");
    svms::g_voiceRetireThreshold.store(saved);
}

void TestWholeVoiceDecimation() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 3u;
    constexpr uint32_t kPool = 40u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
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
        for (int enabled = 0; enabled < 2; ++enabled) {
            svms::ChannelCache seedChannels;
            seedChannels.SetMasterVolume(1.0f);
            seedChannels.RebuildCache(cfg, 44100.0f);
            auto seedVoices = std::make_unique<svms::VoiceManager>();
            ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool,
                                        kPool, false);
            auto oracleVoices =
                std::make_unique<svms::VoiceManager>(*seedVoices);
            auto wholeVoices =
                std::make_unique<svms::VoiceManager>(*seedVoices);
            auto plainVoices =
                std::make_unique<svms::VoiceManager>(*seedVoices);
            svms::ChannelCache oracleChannels = seedChannels;
            svms::ChannelCache wholeChannels = seedChannels;
            svms::ChannelCache plainChannels = seedChannels;

            svms::RenderScalar oracle, whole, plain;
            Check(oracle.ReserveVoiceCapacity(kPool) &&
                      whole.ReserveVoiceCapacity(kPool) &&
                      plain.ReserveVoiceCapacity(kPool) &&
                      oracle.ConfigureRenderThreads(1u, kFrames) &&
                      whole.ConfigureRenderThreads(4u, kFrames) &&
                      plain.ConfigureRenderThreads(4u, kFrames),
                  "decimation test starts its renderers");
            Check(whole.SetRenderBackend(
                      avx2Pass ? svms::RenderBackend::AVX2
                               : svms::RenderBackend::Scalar) &&
                      plain.SetRenderBackend(
                          avx2Pass ? svms::RenderBackend::AVX2
                                   : svms::RenderBackend::Scalar),
                  "decimation test selects the backend");
            // Tiers shrunk so a 40-voice pool lands on step 4 (keep 10).
            for (svms::RenderScalar* r : {&oracle, &whole})
                r->SetDecimationTiersForTest(16u, 32u, 48u, 64u);
            oracle.SetDecimationEnabled(enabled != 0);
            whole.SetDecimationEnabled(enabled != 0);

            float maxDiff = 0.0f;
            float maxVsPlain = 0.0f;
            for (uint32_t block = 0; block < kBlocks; ++block) {
                const uint64_t start = 10000u + block * kFrames;
                std::vector<float> oL(kFrames, 0.0f), oR(kFrames, 0.0f);
                std::vector<float> wL(kFrames, 0.0f), wR(kFrames, 0.0f);
                std::vector<float> pL(kFrames, 0.0f), pR(kFrames, 0.0f);
                oracle.RenderBlockReference(*oracleVoices, oracleChannels,
                    samples.data(), sampleCount, oL.data(), oR.data(),
                    kFrames, cfg, nullptr, 0u, true, start);
                whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                    sampleCount, wL.data(), wR.data(), kFrames, cfg, nullptr,
                    0u, true, start);
                plain.RenderBlock(*plainVoices, plainChannels, samples.data(),
                    sampleCount, pL.data(), pR.data(), kFrames, cfg, nullptr,
                    0u, true, start);
                for (uint32_t f = 0; f < kFrames; ++f) {
                    maxDiff = (std::max)(maxDiff,
                        std::fabs(oL[f] - wL[f]));
                    maxDiff = (std::max)(maxDiff,
                        std::fabs(oR[f] - wR[f]));
                    maxVsPlain = (std::max)(maxVsPlain,
                        std::fabs(pL[f] - wL[f]));
                }
            }
            std::fprintf(stderr,
                "[DECIM] backend=%s enabled=%d oracle-vs-whole=%g whole-vs-plain=%g\n",
                backendName, enabled, maxDiff, maxVsPlain);
            Check(maxDiff <= 2.0e-5f,
                  "decimated whole-voice blocks match the frame-major oracle");
            if (enabled == 0) {
                Check(maxVsPlain == 0.0f,
                      "disabled decimation leaves the audio untouched");
            } else {
                Check(maxVsPlain > 1.0e-4f,
                      "enabled decimation actually silences voices");
                // Muted rows still advance: per-handle state matches the
                // oracle (no events, so handles are the stable identity;
                // the seed pool shares one playIndex).
                bool stateMatches = oracleVoices->activeCount_ ==
                                    wholeVoices->activeCount_;
                for (uint32_t h = 0; h < kPool && stateMatches; ++h) {
                    stateMatches =
                        // A muted span adds the phase in one step instead of
                        // per frame, so its float rounding drifts by a
                        // fraction of a sample over a few blocks; envelopes
                        // are exact.  Inaudible while muted.
                        NearlyEqual(oracleVoices->v.phases[h],
                                    wholeVoices->v.phases[h], 0.5f) &&
                        NearlyEqual(oracleVoices->v.currentGain[h],
                                    wholeVoices->v.currentGain[h], 2.0e-5f);
                }
                Check(stateMatches,
                      "muted voices keep advancing phase and gain");
            }
        }
    }
}

// Decimation ranked by level: velocity falls with the handle while the
// envelope level rises with it, so the two rankings keep opposite halves of
// the pool.  Keeping the loud-by-level voices must leave more energy than
// keeping the high-velocity (quiet) ones.
void TestWholeVoiceDecimationByLevel() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kPool = 40u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    double energy[3] = {0.0, 0.0, 0.0};  // plain, by velocity, by level
    for (int mode = 0; mode < 3; ++mode) {
        svms::ChannelCache channels;
        channels.SetMasterVolume(1.0f);
        channels.RebuildCache(cfg, 44100.0f);
        auto voices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*voices, channels, kPool, kPool, false);
        for (uint32_t h = 0; h < kPool; ++h) {
            voices->v.velocity[h] = static_cast<uint8_t>(127u - h);
            voices->v.stealOutputGain[h] = 1.0f;
            voices->v.currentGain[h] = 0.02f + 0.02f * static_cast<float>(h);
            voices->v.renderGainL[h] =
                voices->v.currentGain[h] * voices->v.mixGainL[h];
            voices->v.renderGainR[h] =
                voices->v.currentGain[h] * voices->v.mixGainR[h];
        }
        svms::RenderScalar r;
        Check(r.ReserveVoiceCapacity(kPool) &&
                  r.ConfigureRenderThreads(2u, kFrames),
              "by-level decimation test starts its renderer");
        r.SetDecimationTiersForTest(16u, 32u, 48u, 64u);
        r.SetDecimationEnabled(mode != 0);
        r.SetDecimationByLevel(mode == 2);
        std::vector<float> L(kFrames, 0.0f), R(kFrames, 0.0f);
        r.RenderBlock(*voices, channels, samples.data(), sampleCount,
                      L.data(), R.data(), kFrames, cfg, nullptr, 0u, true,
                      10000u);
        for (uint32_t f = 0; f < kFrames; ++f)
            energy[mode] += double(L[f]) * L[f] + double(R[f]) * R[f];
    }
    std::fprintf(stderr, "[DECIM-LEVEL] plain=%g velocity=%g level=%g\n",
                 energy[0], energy[1], energy[2]);
    Check(energy[1] < energy[0] && energy[2] < energy[0],
          "decimation by level mutes voices");
    Check(energy[2] > 2.0 * energy[1],
          "ranking by level keeps the louder voices than ranking by velocity");
}

// Muted-tail cull: a releasing pool above the (shrunk) decimation tier keeps
// only its loudest 1/step mixing; with the cull on, the muted rest is retired
// after the configured run of muted blocks, with it off nothing is.
void TestMutedTailCull() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kPool = 40u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    for (uint32_t cull : {0u, 2u}) {
        svms::ChannelCache channels;
        channels.SetMasterVolume(1.0f);
        channels.RebuildCache(cfg, 44100.0f);
        auto voices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*voices, channels, kPool, kPool, false);
        for (uint32_t h = 0; h < kPool; ++h) {
            voices->v.velocity[h] = static_cast<uint8_t>(127u - h);
            voices->StartRelease(static_cast<svms::VoiceHandle>(h));
        }
        svms::RenderScalar r;
        Check(r.ReserveVoiceCapacity(kPool) &&
                  r.ConfigureRenderThreads(2u, kFrames),
              "cull test starts its renderer");
        r.SetDecimationTiersForTest(16u, 32u, 48u, 64u);
        r.SetDecimationEnabled(true);
        r.SetMutedTailCullBlocks(cull);
        std::vector<float> L(kFrames, 0.0f), R(kFrames, 0.0f);
        uint32_t activeAfter[6] = {};
        for (uint32_t block = 0; block < 6u; ++block) {
            std::fill(L.begin(), L.end(), 0.0f);
            std::fill(R.begin(), R.end(), 0.0f);
            r.RenderBlock(*voices, channels, samples.data(), sampleCount,
                          L.data(), R.data(), kFrames, cfg, nullptr, 0u, true,
                          10000u + block * kFrames);
            activeAfter[block] = voices->activeCount_;
        }
        std::fprintf(stderr, "[CULL] cull=%u active after blocks: %u %u %u %u %u %u\n",
                     cull, activeAfter[0], activeAfter[1], activeAfter[2],
                     activeAfter[3], activeAfter[4], activeAfter[5]);
        if (cull == 0u) {
            Check(activeAfter[5] == kPool, "no cull: muted tails stay in the pool");
        } else {
            Check(activeAfter[0] == kPool && activeAfter[1] == kPool,
                  "cull waits for the muted run");
            Check(activeAfter[5] == kPool / 4u,
                  "cull retires the muted tails and keeps the loudest 1/step");
        }
    }
}

// Filtered sustained loops through the 8-voices-per-vector biquad group
// kernel must reproduce the per-voice kernel exactly: with each voice on its
// own channel plane (bus mode) the planes and every voice's end state are
// compared bitwise; on a single shared plane only the mix order of lanes that
// drifted apart could differ, so that comparison is a tight tolerance.
void RunFilteredGroupKernelTest(bool release) {
    if (!svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) return;
    constexpr uint32_t kVoices = 27u;  // three groups of eight + a remainder
    constexpr uint32_t kRegion = 4096u;
    std::vector<int16_t> samples(kRegion + 8u, 0);
    uint32_t rng = 12345u;
    for (uint32_t i = 0; i < kRegion; ++i) {
        rng = rng * 1664525u + 1013904223u;
        samples[i] = static_cast<int16_t>((rng >> 16) & 0x7FFF) - 16384;
    }
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto seed = std::make_unique<svms::VoiceManager>();
    Check(seed->Initialize(64u, 44100u), "filter group pool allocates");
    const uint32_t loopLengths[] = {40u, 130u, 700u, 3000u};
    std::vector<uint32_t> handles;
    for (uint32_t i = 0; i < kVoices; ++i) {
        const uint8_t channel = static_cast<uint8_t>(i % 16u);
        const svms::VoiceHandle voice = seed->AllocateVoice(
            channel, static_cast<uint8_t>(40u + i), 100);
        const uint32_t loopLen = loopLengths[i % 4u];
        seed->SetVoiceSample(voice, 0, kRegion, 16, 16 + loopLen, 1u,
                             0.3f + 0.07f * static_cast<float>(i % 23u), 1);
        seed->v.phases[voice] = static_cast<float>((i * 53u) % 900u) + 0.25f;
        seed->SetVoiceEnvelope(voice, 0.5f, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f, 0.999f);
        seed->SetVoicePlayIndex(voice, i + 1u);
        seed->SetVoiceGain(voice, 0.3f + 0.01f * static_cast<float>(i),
                           0.2f + 0.01f * static_cast<float>(i));
        seed->RefreshMixGain(voice, channels.GetParams()[channel]);
        seed->v.renderGainL[voice] = seed->v.currentGain[voice] * seed->v.mixGainL[voice];
        seed->v.renderGainR[voice] = seed->v.currentGain[voice] * seed->v.mixGainR[voice];
        const svms::PreparedVoiceFilter f = svms::PrepareVoiceLowPass(
            static_cast<uint8_t>(svms::FilterType::LowPass2Pole),
            static_cast<int16_t>(6000 + 250 * static_cast<int>(i % 28u)),
            static_cast<int16_t>((i % 5u) * 60), 44100u);
        seed->v.filterA0[voice] = f.a0;
        seed->v.filterB1[voice] = f.b1;
        seed->v.filterB2[voice] = f.b2;
        seed->v.filterEnabled[voice] = f.enabled;
        seed->v.filterZ1[voice] = 0.01f * static_cast<float>(i % 7u);
        seed->v.filterZ2[voice] = -0.005f * static_cast<float>(i % 5u);
        if (release) {
            // Releasing tails: a mix of threshold-governed (UINT32_MAX) and
            // counted ones, several of which retire inside the longer span.
            seed->StartRelease(voice);
            seed->v.currentGain[voice] = 0.4f + 0.01f * static_cast<float>(i);
            seed->v.releaseDecay[voice] = 0.9990f - 0.0004f * static_cast<float>(i % 9u);
            seed->v.releaseSamplesRemaining[voice] =
                (i % 3u == 0u) ? UINT32_MAX : 200u + 150u * (i % 11u);
        }
        handles.push_back(voice);
    }
    // One unfiltered voice mid-list splits the pending group, as in a real mix.
    seed->v.filterEnabled[handles[13]] = 0u;

    const svms::RenderKernelSet& kernels = svms::GetAVX2RenderKernelSet();
    const svms::RenderClassKernel kernel =
        kernels.kernels[static_cast<uint32_t>(release
            ? svms::VoiceRenderClass::ReleaseLoop
            : svms::VoiceRenderClass::SustainedLoop)];
    struct Span { uint32_t start, count; };
    const Span spans[] = {{0u, 2048u}, {5u, 203u}, {0u, 64u}};
    for (const Span& span : spans) {
        for (int bus = 0; bus < 2; ++bus) {
            std::vector<float> planes[2][2][16];  // [grouped][L/R][channel]
            float* planePtr[2][2][16];
            std::vector<float> mix[2][2];
            std::unique_ptr<svms::VoiceManager> run[2];
            std::vector<svms::SpanRetirement> retired[2];
            std::vector<uint32_t> positions(64u);
            for (uint32_t p = 0; p < 64u; ++p) positions[p] = p;
            for (int grouped = 0; grouped < 2; ++grouped) {
                run[grouped] = std::make_unique<svms::VoiceManager>(*seed);
                for (int side = 0; side < 2; ++side) {
                    for (uint32_t ch = 0; ch < 16u; ++ch) {
                        planes[grouped][side][ch].assign(span.start + span.count + 16u, 0.0f);
                        planePtr[grouped][side][ch] = planes[grouped][side][ch].data();
                    }
                    mix[grouped][side].assign(span.start + span.count + 16u, 0.0f);
                }
                svms::SetFilteredGroupKernelForTest(grouped != 0);
                retired[grouped].assign(64u, svms::SpanRetirement{});
                uint32_t retiredCount = 0u;
                svms::RenderSpanContext ctx{
                    &run[grouped]->v, samples.data(), nullptr, kRegion + 8u,
                    mix[grouped][0].data(), mix[grouped][1].data(),
                    span.start, span.count, 64u, nullptr, nullptr,
                    positions.data(), retired[grouped].data(), &retiredCount,
                    0.0f, 1.0f, 0u,
                    bus ? planePtr[grouped][0] : nullptr,
                    bus ? planePtr[grouped][1] : nullptr};
                Check(kernel(ctx, handles.data(),
                             static_cast<uint32_t>(handles.size())),
                      "filter group kernel accepts the batch");
                retired[grouped].resize(retiredCount);
            }
            const bool retiredSame =
                retired[0].size() == retired[1].size() &&
                (retired[0].empty() ||
                 std::memcmp(retired[0].data(), retired[1].data(),
                             retired[0].size() *
                                 sizeof(svms::SpanRetirement)) == 0);
            svms::SetFilteredGroupKernelForTest(true);
            float maxDiff = 0.0f;
            bool bitExact = true;
            if (bus) {
                for (int side = 0; side < 2; ++side)
                    for (uint32_t ch = 0; ch < 16u; ++ch)
                        bitExact = bitExact &&
                            std::memcmp(planes[0][side][ch].data(), planes[1][side][ch].data(),
                                        planes[0][side][ch].size() * sizeof(float)) == 0;
            } else {
                for (int side = 0; side < 2; ++side)
                    for (size_t f = 0; f < mix[0][side].size(); ++f)
                        maxDiff = (std::max)(maxDiff,
                            std::fabs(mix[0][side][f] - mix[1][side][f]));
            }
            bool stateExact = true;
            for (uint32_t handle : handles) {
                stateExact = stateExact &&
                    run[0]->v.phases[handle] == run[1]->v.phases[handle] &&
                    run[0]->v.filterZ1[handle] == run[1]->v.filterZ1[handle] &&
                    run[0]->v.filterZ2[handle] == run[1]->v.filterZ2[handle] &&
                    run[0]->v.currentGain[handle] == run[1]->v.currentGain[handle] &&
                    run[0]->v.releaseSamplesRemaining[handle] ==
                        run[1]->v.releaseSamplesRemaining[handle];
            }
            std::fprintf(stderr,
                "[FILTGRP] %s start=%u count=%u bus=%d planes-bit-exact=%d state-bit-exact=%d retire-same=%d(%zu) mix-maxdiff=%g\n",
                release ? "release" : "sustained", span.start, span.count, bus,
                bus ? (bitExact ? 1 : 0) : -1, stateExact ? 1 : 0,
                retiredSame ? 1 : 0, retired[0].size(), maxDiff);
            Check(retiredSame, "filter group retires the same voices on the same frames");
            Check(stateExact, "filter group keeps per-voice phase and filter state bit-identical");
            if (bus) Check(bitExact, "filter group planes are bit-identical to the per-voice kernel");
            else Check(maxDiff <= 1.0e-6f, "filter group shared-plane mix matches within summation order");
        }
    }
}

void TestFilteredGroupKernel() {
    RunFilteredGroupKernelTest(false);
    RunFilteredGroupKernelTest(true);
}

void TestWholeVoiceResetBarrierDifferential() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 8u;
    constexpr uint32_t kPool = 64u;
    constexpr uint32_t kSeed = 21u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);

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
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool, kSeed);
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache wholeChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames),
              "reset barrier differential starts serial and parallel "
              "renderers");
        Check(whole.SetRenderBackend(avx2Pass ? svms::RenderBackend::AVX2
                                              : svms::RenderBackend::Scalar),
              "reset barrier differential selects the requested backend");

        WholeVoiceLaunchContext oracleContext{oracleVoices.get(),
                                              oracleChannels.GetParams()};
        WholeVoiceLaunchContext wholeContext{wholeVoices.get(),
                                             wholeChannels.GetParams()};
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                      &wholeContext);

        WholeVoiceRng rng;
        std::vector<std::pair<uint8_t, uint8_t>> launchedPairs;
        float passMaxDiff = 0.0f;
        uint32_t barrierBlocks = 0u;
        uint32_t nextKey = 0u;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 20000u + block * kFrames;
            std::vector<svms::RenderEvent> events;
            uint32_t sequence = 0u;
            const auto push = [&](svms::RenderEventType type, uint8_t channel,
                                  uint8_t data1, uint8_t data2,
                                  uint32_t frame) {
                svms::RenderEvent event{};
                event.type = type;
                event.channel = channel;
                event.data1 = data1;
                event.data2 = data2;
                event.frameOffset = frame;
                event.ingressSequence = sequence++;
                events.push_back(event);
            };
            for (uint32_t j = 0; j < 5u; ++j, ++nextKey) {
                push(svms::RenderEventType::NoteOn,
                     static_cast<uint8_t>(nextKey % 4u),
                     static_cast<uint8_t>(40u + nextKey % 80u),
                     static_cast<uint8_t>(64u + rng.Next() % 64u),
                     rng.Next() % kFrames);
            }
            for (uint32_t j = 0; j < 3u && !launchedPairs.empty(); ++j) {
                const auto pair =
                    launchedPairs[rng.Next() % launchedPairs.size()];
                push(svms::RenderEventType::NoteOff, pair.first, pair.second,
                     0u, rng.Next() % kFrames);
            }
            for (uint32_t j = 0; j < 5u; ++j)
                launchedPairs.emplace_back(events[j].channel, events[j].data1);
            // Barrier placement: mid-block, frame 0 twice then a note, last
            // frame, no-op + reset at one frame, a lone no-op.
            switch (block) {
                case 2u:
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 200u);
                    break;
                case 3u:
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 0u);
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 0u);
                    push(svms::RenderEventType::NoteOn, 1u, 77u, 100u, 0u);
                    break;
                case 4u:
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u,
                         kFrames - 1u);
                    break;
                case 5u:
                    push(svms::RenderEventType::AllSoundOff, 0u, 0u, 0u, 100u);
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 100u);
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 300u);
                    push(svms::RenderEventType::NoteOn, 2u, 90u, 110u, 301u);
                    break;
                case 6u:
                    push(svms::RenderEventType::AllNotesOff, 0u, 0u, 0u, 50u);
                    break;
                default:
                    break;
            }
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });
            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f);
            std::vector<float> wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), sampleCount,
                oracleLeft.data(), oracleRight.data(), kFrames, cfg,
                events.data(), static_cast<uint32_t>(events.size()), true,
                blockStartFrame);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              sampleCount, wholeLeft.data(),
                              wholeRight.data(), kFrames, cfg, events.data(),
                              static_cast<uint32_t>(events.size()), true,
                              blockStartFrame);
            const uint32_t paths = whole.GetLastRenderPaths();
            Check((paths & 0x6u) == 0u,
                  "barrier blocks never reach the dense or sparse renderer");
            if ((paths & 0x200u) != 0u) ++barrierBlocks;

            float blockDiff = 0.0f;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleLeft[frame] - wholeLeft[frame]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleRight[frame] - wholeRight[frame]));
            }
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        std::fprintf(stderr, "[WVDIFF] backend=%s reset-barrier max=%g\n",
                     backendName, passMaxDiff);
        Check(passMaxDiff <= g_waveTol,
              "barrier-split blocks match the oracle per sample");
        Check(barrierBlocks == 5u, "five blocks were barrier-split");
        Check(whole.GetPathBlocks(1) == 0u && whole.GetPathBlocks(2) == 0u,
              "no block fell back to dense or sparse");
        CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, backendName);
        Check(oracleContext.launches == wholeContext.launches,
              "both sides launched the same notes");
    }
}

void RunWholeVoiceLaunchDifferential(uint32_t rotationMode) {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 6u;
    constexpr uint32_t kPool = 64u;
    constexpr uint32_t kSeed = 16u;
    constexpr uint32_t kLaunchesPerBlock = 5u;
    constexpr uint32_t kNoteOffsPerBlock = 3u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
    std::vector<int16_t> hilbert(samples.size(), 0);
    svms::BuildHilbertSlice(samples.data(), hilbert.data(),
                            {0u, sampleCount, 0u, sampleCount});
    const int16_t* hilbertPtr = rotationMode != 0u ? hilbert.data() : nullptr;

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
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool, kSeed);
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        if (rotationMode != 0u) {
            Check(oracleVoices->SetPhaseRotationMode(rotationMode) &&
                      wholeVoices->SetPhaseRotationMode(rotationMode),
                  "launch differential seeds rotation states");
        }
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache wholeChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames),
              "whole-voice launch differential starts serial and parallel "
              "renderers");
        Check(whole.SetRenderBackend(avx2Pass ? svms::RenderBackend::AVX2
                                              : svms::RenderBackend::Scalar),
              "whole-voice launch differential selects the requested backend");

        WholeVoiceLaunchContext oracleContext{oracleVoices.get(),
                                              oracleChannels.GetParams()};
        WholeVoiceLaunchContext wholeContext{wholeVoices.get(),
                                             wholeChannels.GetParams()};
        if (rotationMode != 0u) {
            // Rotation seeds depend on the slot handle, and whole-voice
            // retirements land at block end (slot recycling differs from
            // the oracle by design): keep releases from retiring inside the
            // scenario so both sides allocate identical handles.
            oracleContext.releaseSamples = wholeContext.releaseSamples = 30000u;
            oracleContext.allLoop = wholeContext.allLoop = true;
        }
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                      &wholeContext);

        // Fresh PRNG per pass so both backends walk identical scenarios.
        // (channel, note) pairs are never reused, so a block never re-attacks
        // a key released earlier in the same block.
        WholeVoiceRng rng;
        std::vector<std::pair<uint8_t, uint8_t>> launchedPairs;
        float passMaxDiff = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 10000u + block * kFrames;
            const uint32_t noteOffCount =
                launchedPairs.empty() ? 0u : kNoteOffsPerBlock;
            std::vector<svms::RenderEvent> events(kLaunchesPerBlock +
                                                  noteOffCount);
            for (uint32_t j = 0; j < kLaunchesPerBlock; ++j) {
                svms::RenderEvent& event = events[j];
                event.type = svms::RenderEventType::NoteOn;
                const uint32_t launch = block * kLaunchesPerBlock + j;
                event.channel = static_cast<uint8_t>(launch % 4u);
                event.data1 = static_cast<uint8_t>(120u + launch);
                event.data2 = static_cast<uint8_t>(64u + rng.Next() % 64u);
                event.frameOffset = rng.Next() % kFrames;
                event.ingressSequence = j;
            }
            for (uint32_t j = 0; j < noteOffCount; ++j) {
                svms::RenderEvent& event = events[kLaunchesPerBlock + j];
                event.type = svms::RenderEventType::NoteOff;
                // Release keys launched in earlier blocks only: the note-off
                // scan is not frame-scoped inside the whole-voice pre-pass,
                // so releasing a key that is re-attacked later in the same
                // block would observe pre-pass state the oracle never sees.
                const uint32_t pick = rng.Next() % launchedPairs.size();
                event.channel = launchedPairs[pick].first;
                event.data1 = launchedPairs[pick].second;
                event.data2 = 0u;
                event.frameOffset = rng.Next() % kFrames;
                event.ingressSequence = kLaunchesPerBlock + j;
            }
            for (uint32_t j = 0; j < kLaunchesPerBlock; ++j)
                launchedPairs.emplace_back(events[j].channel,
                                           events[j].data1);
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });

            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f);
            std::vector<float> wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), hilbertPtr,
                sampleCount,
                oracleLeft.data(), oracleRight.data(), kFrames, cfg,
                events.data(), static_cast<uint32_t>(events.size()), true,
                blockStartFrame);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              hilbertPtr, sampleCount, wholeLeft.data(),
                              wholeRight.data(), kFrames, cfg, events.data(),
                              static_cast<uint32_t>(events.size()), true,
                              blockStartFrame);

            {
                // The releasing counter must equal the number of rows in the
                // Releasing state (a voice released and retired inside one
                // block once underflowed it).
                uint32_t releasingRows = 0u;
                for (uint32_t i = 0u; i < wholeVoices->activeCount_; ++i)
                    releasingRows += wholeVoices->v.state[
                        wholeVoices->activeList_[i]] ==
                        static_cast<uint8_t>(svms::VoiceState::Releasing);
                Check(wholeVoices->GetReleasingCount() == releasingRows,
                      "releasing-voice counter matches the Releasing rows");
            }
            float blockDiff = 0.0f;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleLeft[frame] - wholeLeft[frame]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleRight[frame] - wholeRight[frame]));
            }
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        std::fprintf(stderr, "[WVDIFF] backend=%s rot=%u launch max=%g\n",
                     backendName, rotationMode, passMaxDiff);
        Check(passMaxDiff <= g_waveTol,
              "whole-voice launches, deferred releases and retirements "
              "match the oracle per sample");
        CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, backendName);
        Check(whole.GetWholeVoiceBlocksForTest() == kBlocks,
              "launch scenario routes every block through the whole-voice "
              "renderer");
        Check(oracleContext.launches == wholeContext.launches &&
                  wholeContext.launches == kBlocks * kLaunchesPerBlock,
              "launch scenario dispatches every note-on on both sides");
        Check(oracleVoices->stealCount_ == 0u && wholeVoices->stealCount_ == 0u,
              "launch scenario never exhausts the pool");
    }
}

void TestWholeVoiceLaunchDifferential() { RunWholeVoiceLaunchDifferential(0u); }


// Rotated pools: free-slot launches seed their own state; deferred releases
// and natural retirements keep rotating voices exact against the oracle.
void TestWholeVoiceRotationLaunchDifferential() {
    for (uint32_t mode : {1u, 2u, 4u}) RunWholeVoiceLaunchDifferential(mode);
}

struct WholeVoiceCCContext {
    svms::VoiceManager* voices;
    svms::ChannelCache* channels;
    const svms::ChannelParamsSnapshot* channelParams;
    const svms::RuntimeConfigSnapshot* config = nullptr;
    uint32_t playIndex = 12000u;
    uint32_t launches = 0u;
    // Vibrato scenario: launched voices carry the SF2 default vibrato
    // modulator like production note-ons do.
    bool vibratoLaunches = false;
    // SysEx master state, mirroring the driver's sysex terms.
    float masterFineTune = 0.0f;
    float masterTranspose = 0.0f;
    float channelBendRatio[svms::kChannelCount] = {};
    void ResetMasterState() {
        masterFineTune = 0.0f;
        masterTranspose = 0.0f;
        for (float& ratio : channelBendRatio) ratio = 1.0f;
    }
};

// Mirrors the production dispatch semantics (Driver::HandleControlChange and
// NoteOffPlayIndex) for exactly the event set the whole-voice renderer
// accepts: notes plus the lifecycle controllers 64/66/120/123.  Both the
// oracle and the whole-voice renderer share this dispatcher, so any
// divergence between the two sides is pure render-path divergence.
void DispatchWholeVoiceCCEvent(const svms::RenderEvent* events,
                               uint32_t eventCount, uint32_t blockCursor,
                               void* userData) {
    auto& context = *static_cast<WholeVoiceCCContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        const uint8_t channel = event.channel;
        // Mirrors Driver::HandlePitchBend: the sysex master terms ride on
        // top of the channel wheel.
        const auto applyBend = [&context, channel](uint16_t wheel) {
            context.channels->PitchBend(
                channel, static_cast<int16_t>(wheel));
            const float bendSemitones =
                context.channels->GetPitchBendSemitones(channel) +
                context.masterFineTune + context.masterTranspose;
            const float commonRatio = std::pow(2.0f, bendSemitones / 12.0f);
            context.channelBendRatio[channel] = commonRatio;
            context.channels->SetBendRatio(channel, commonRatio);
            context.voices->ApplyChannelBendRatio(channel, bendSemitones);
        };
        switch (event.type) {
            case svms::RenderEventType::NoteOn: {
                // Production launch path (LaunchVoiceGroup -> ConfigureVoice),
                // with a transient envelope and a release cap so voices also
                // die naturally mid-scenario.
                const bool loop = (context.playIndex % 3u) != 0u;
                svms::VoiceConfiguration setup{};
                setup.sampleStart = 0u;
                setup.sampleEnd = 4096u;
                setup.loopStart = 64u;
                setup.loopEnd = 4032u;
                setup.loopMode = loop ? 1u : 0u;
                setup.phaseStep = setup.basePhaseStep =
                    0.637f + event.data1 * 0.011f;
                setup.initialGain = 0.6f;
                setup.sustainLevel = 0.5f;
                setup.attackSamples = 17u;
                setup.attackGainStep = 0.6f / 17.0f;
                setup.decaySamples = 53u;
                setup.decaySlope = 0.99f;
                setup.releaseDecay = 0.9995f;
                setup.releaseSamples = 400u;
                setup.gainLeft = setup.gainRight = 0.02f;
                setup.sampleBacked = 1u;
                setup.playIndex = context.playIndex++;
                svms::VoiceHandle handle = svms::kInvalidVoice;
                context.voices->LaunchVoiceGroup(
                    channel, event.data1, event.data2, &setup, 1u,
                    setup.playIndex,
                    context.channelParams[channel], &handle);
                if (handle != svms::kInvalidVoice) {
                    ++context.launches;
                    if (context.vibratoLaunches) {
                        context.voices->v.vibLfoToPitchCents[handle] = 50.0f;
                        context.voices->v.vibLfoSteps[handle] =
                            5.0f / 44100.0f;
                    }
                }
                break;
            }
            case svms::RenderEventType::NoteOff:
            case svms::RenderEventType::StaleNoteOffBatch: {
                // NoteOffPlayIndex semantics: pedal-held keys take the held
                // flag (the key chain is managed inside StartRelease for the
                // free case); free keys release at this exact frame.
                const bool sustain =
                    context.channels->IsSustainActive(channel);
                for (uint32_t i = 0; i < context.voices->activeCount_; ++i) {
                    const uint32_t voice = context.voices->activeList_[i];
                    if (context.voices->v.channel[voice] != channel ||
                        context.voices->v.note[voice] != event.data1) {
                        continue;
                    }
                    context.voices->v.releaseStartInBlock[voice] =
                        blockCursor;
                    if (sustain ||
                        context.voices->v.heldBySostenuto[voice]) {
                        if (sustain)
                            context.voices->v.heldBySustain[voice] = 1u;
                    } else {
                        context.voices->StartRelease(
                            static_cast<svms::VoiceHandle>(voice));
                    }
                }
                break;
            }
            case svms::RenderEventType::ControlChange:
                switch (event.data1) {
                    case 64u:
                        context.channels->ControlChange(channel, 64u,
                                                        event.data2);
                        if (event.data2 < 64u)
                            context.voices->ReleaseSustain(channel,
                                                           blockCursor);
                        break;
                    case 66u: {
                        const bool wasActive =
                            context.channels->IsSostenutoActive(channel);
                        context.channels->ControlChange(channel, 66u,
                                                        event.data2);
                        const bool nowActive = event.data2 >= 64u;
                        if (!wasActive && nowActive)
                            context.voices->CaptureSostenuto(channel);
                        else if (wasActive && !nowActive)
                            context.voices->ReleaseSostenuto(channel,
                                                             blockCursor);
                        break;
                    }
                    case 120u:
                        context.voices->SilenceChannelImmediate(channel);
                        break;
                    case 123u:
                        context.voices->ReleaseChannel(channel, blockCursor);
                        break;
                    case 6u:
                    case 38u:
                        // RPN data entry mirrors HandleControlChange's wheel
                        // re-application.
                        context.channels->ControlChange(channel, event.data1,
                                                        event.data2);
                        applyBend(
                            context.channels->GetPitchBendValue(channel));
                        break;
                    case 1u:
                        // Mod wheel mirrors HandleControlChange: rebuild,
                        // restore the sysex-aware bend ratio, report the
                        // post-rebuild modulation depth as a vibrato op.
                        context.channels->ControlChange(channel, 1u,
                                                        event.data2);
                        context.channels->RebuildChannel(
                            channel, *context.config, 44100.0f);
                        context.channels->SetBendRatio(
                            channel, context.channelBendRatio[channel]);
                        context.voices->MarkChannelVibratoDepth(
                            channel,
                            context.channels->GetParams()[channel].modDepth);
                        break;
                    case 7u:
                    case 10u:
                    case 11u:
                        context.channels->ControlChange(channel, event.data1,
                                                        event.data2);
                        context.channels->RebuildChannel(
                            channel, *context.config, 44100.0f);
                        context.voices->MarkChannelMixStale(
                            channel,
                            context.channels->GetParams()[channel]);
                        break;
                    case 121u: {
                        const bool sustainWasActive =
                            context.channels->IsSustainActive(channel);
                        const bool sostenutoWasActive =
                            context.channels->IsSostenutoActive(channel);
                        context.channels->ControlChange(channel, 121u,
                                                        event.data2);
                        if (sustainWasActive)
                            context.voices->ReleaseSustain(channel,
                                                           blockCursor);
                        if (sostenutoWasActive)
                            context.voices->ReleaseSostenuto(channel,
                                                             blockCursor);
                        // Reset All Controllers also centers the wheel.
                        applyBend(8192u);
                        context.channels->RebuildChannel(
                            channel, *context.config, 44100.0f);
                        context.channels->SetBendRatio(
                            channel, context.channelBendRatio[channel]);
                        context.voices->MarkChannelMixStale(
                            channel,
                            context.channels->GetParams()[channel]);
                        context.voices->MarkChannelVibratoDepth(
                            channel,
                            context.channels->GetParams()[channel].modDepth);
                        break;
                    }
                    default:
                        break;
                }
                break;
            case svms::RenderEventType::ProgramChange:
                // No soundfont stack in the harness: the driver handler
                // early-outs before touching any state, so this is a
                // dispatch-only event here (still exercises the whitelisting
                // and the ingress ordering against same-frame launches).
                break;
            case svms::RenderEventType::RhythmPart:
                if (channel < svms::kChannelCount)
                    context.channels->SetRhythmPart(channel, event.data1);
                break;
            case svms::RenderEventType::MasterVolume: {
                // Mirrors Driver::DispatchRenderEvent's MasterVolume handler:
                // cache-wide rebuild, then a mix mark per channel.
                const uint16_t value = static_cast<uint16_t>(
                    event.data1 | (static_cast<uint16_t>(event.data2) << 7u));
                const float effective = context.config->masterVolume *
                    (static_cast<float>(value) / 16383.0f);
                context.channels->SetMasterVolume(effective);
                context.channels->RebuildCache(*context.config, 44100.0f);
                for (uint8_t ch = 0; ch < svms::kChannelCount; ++ch) {
                    context.voices->MarkChannelMixStale(
                        ch, context.channels->GetParams()[ch]);
                    context.channels->SetBendRatio(
                        ch, context.channelBendRatio[ch]);
                }
                break;
            }
            case svms::RenderEventType::MasterFineTune:
            case svms::RenderEventType::MasterTranspose:
                // Mirrors Driver::RefreshAllPitchIncrements (minus the
                // launch-revision bookkeeping the harness does not model).
                if (event.type == svms::RenderEventType::MasterFineTune) {
                    const uint16_t value = static_cast<uint16_t>(
                        event.data1 |
                        (static_cast<uint16_t>(event.data2) << 7u));
                    context.masterFineTune = static_cast<float>(
                        static_cast<int32_t>(value) - 8192) / 8192.0f;
                } else {
                    context.masterTranspose = static_cast<float>(
                        static_cast<int32_t>(event.data1) - 64);
                }
                for (uint8_t ch = 0; ch < svms::kChannelCount; ++ch) {
                    const float semitones =
                        context.channels->GetPitchBendSemitones(ch) +
                        context.masterFineTune + context.masterTranspose;
                    const float commonRatio =
                        std::pow(2.0f, semitones / 12.0f);
                    context.channelBendRatio[ch] = commonRatio;
                    context.channels->SetBendRatio(ch, commonRatio);
                    context.voices->ApplyChannelBendRatio(ch, semitones);
                }
                break;
            case svms::RenderEventType::PitchBend:
                applyBend(static_cast<uint16_t>((event.data2 << 7) |
                                                event.data1));
                break;
            case svms::RenderEventType::ChannelPressure:
                // Mirrors Driver::HandleChannelPressure: cache update,
                // rebuild, sysex-aware ratio restore, vibrato depth report.
                context.channels->ChannelPressure(channel, event.data1);
                context.channels->RebuildChannel(channel, *context.config,
                                                 44100.0f);
                context.channels->SetBendRatio(
                    channel, context.channelBendRatio[channel]);
                context.voices->MarkChannelVibratoDepth(
                    channel,
                    context.channels->GetParams()[channel].modDepth);
                break;
            default:
                break;
        }
    }
}

void RunWholeVoiceCCDifferential(uint32_t rowOpBudget) {
    // Notes interleaved with lifecycle CCs: pedal capture/release chains,
    // all-notes-off sweeps and mid-block all-sound-off kills (which free
    // slots that later same-block launches reuse, and which kill pre-existing
    // steal tails).  The whole-voice renderer must reproduce the frame-major
    // oracle per sample — through ghosts for CC120 victims and exact-frame
    // deferred releases for CC64/66/123.
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 8u;
    constexpr uint32_t kPool = 48u;
    constexpr uint32_t kSeedCount = 32u;
    constexpr uint32_t kEventsPerBlock = 12u;
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
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache wholeChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        if (rowOpBudget != 0u) whole.SetRowOpBudgetForTest(rowOpBudget);
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames),
              "whole-voice CC differential starts serial and parallel "
              "renderers");
        Check(whole.SetRenderBackend(avx2Pass ? svms::RenderBackend::AVX2
                                              : svms::RenderBackend::Scalar),
              "whole-voice CC differential selects the requested backend");

        WholeVoiceCCContext oracleContext{oracleVoices.get(), &oracleChannels,
                                          oracleChannels.GetParams(), &cfg};
        WholeVoiceCCContext wholeContext{wholeVoices.get(), &wholeChannels,
                                         wholeChannels.GetParams(), &cfg};
        oracleContext.ResetMasterState();
        wholeContext.ResetMasterState();
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent,
                                      &wholeContext);

        WholeVoiceRng rng;
        float passMaxDiff = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 20000u + block * kFrames;
            std::vector<svms::RenderEvent> events;
            events.reserve(kEventsPerBlock);
            for (uint32_t j = 0; j < kEventsPerBlock; ++j) {
                svms::RenderEvent event{};
                event.channel = static_cast<uint8_t>(rng.Next() % 4u);
                event.frameOffset = rng.Next() % kFrames;
                event.ingressSequence = block * kEventsPerBlock + j;
                const uint32_t roll = rng.Next() % 12u;
                if (roll < 4u) {
                    event.type = svms::RenderEventType::NoteOn;
                    event.data1 =
                        static_cast<uint8_t>(36u + rng.Next() % 72u);
                    event.data2 =
                        static_cast<uint8_t>(64u + rng.Next() % 64u);
                } else if (roll < 6u) {
                    event.type = svms::RenderEventType::NoteOff;
                    event.data1 =
                        static_cast<uint8_t>(36u + rng.Next() % 72u);
                } else if (roll == 6u) {
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 64u;
                    event.data2 = (rng.Next() % 2u) != 0u ? 127u : 0u;
                } else if (roll == 7u) {
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 66u;
                    event.data2 = (rng.Next() % 2u) != 0u ? 127u : 0u;
                } else if (roll == 8u) {
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 120u;
                    event.data2 = 0u;
                } else if (roll == 9u) {
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 123u;
                    event.data2 = 0u;
                } else if (roll == 10u) {
                    // Mix-fold controllers: CC7/10/11 land as gain ops.
                    static const uint8_t kMixControllers[3] = {7u, 10u, 11u};
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = kMixControllers[rng.Next() % 3u];
                    event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
                } else {
                    // Row-mutator variety: pitch bend, reset-all, RPN data,
                    // SysEx master events, program/rhythm launch-time state.
                    const uint32_t pick = rng.Next() % 9u;
                    if (pick == 0u) {
                        event.type = svms::RenderEventType::PitchBend;
                        event.data1 =
                            static_cast<uint8_t>(rng.Next() % 128u);
                        event.data2 =
                            static_cast<uint8_t>(rng.Next() % 128u);
                    } else if (pick <= 3u) {
                        static const uint8_t kRowControllers[3] = {
                            121u, 6u, 38u};
                        event.type = svms::RenderEventType::ControlChange;
                        event.data1 = kRowControllers[pick - 1u];
                        event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
                    } else if (pick == 4u) {
                        event.type = svms::RenderEventType::MasterVolume;
                        const uint32_t value = rng.Next() % 16384u;
                        event.data1 = static_cast<uint8_t>(value & 0x7Fu);
                        event.data2 = static_cast<uint8_t>(value >> 7u);
                    } else if (pick == 5u) {
                        event.type = svms::RenderEventType::MasterFineTune;
                        const uint32_t value = rng.Next() % 16384u;
                        event.data1 = static_cast<uint8_t>(value & 0x7Fu);
                        event.data2 = static_cast<uint8_t>(value >> 7u);
                    } else if (pick == 6u) {
                        event.type = svms::RenderEventType::MasterTranspose;
                        event.data1 =
                            static_cast<uint8_t>(rng.Next() % 128u);
                    } else if (pick == 7u) {
                        event.type = svms::RenderEventType::ProgramChange;
                        event.data1 =
                            static_cast<uint8_t>(rng.Next() % 128u);
                    } else {
                        event.type = svms::RenderEventType::RhythmPart;
                        event.data1 =
                            static_cast<uint8_t>(rng.Next() % 2u);
                    }
                }
                events.push_back(event);
            }
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });

            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f);
            std::vector<float> wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), sampleCount,
                oracleLeft.data(), oracleRight.data(), kFrames, cfg,
                events.data(), static_cast<uint32_t>(events.size()), true,
                blockStartFrame);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              sampleCount, wholeLeft.data(), wholeRight.data(),
                              kFrames, cfg, events.data(),
                              static_cast<uint32_t>(events.size()), true,
                              blockStartFrame);

            float blockDiff = 0.0f;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleLeft[frame] - wholeLeft[frame]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleRight[frame] - wholeRight[frame]));
            }
            if (blockDiff > g_waveTol) {
                for (uint32_t frame = 0; frame < kFrames; ++frame) {
                    if (std::fabs(oracleLeft[frame] - wholeLeft[frame]) >
                            g_waveTol ||
                        std::fabs(oracleRight[frame] - wholeRight[frame]) >
                            g_waveTol) {
                        std::fprintf(stderr,
                            "[WVDIFF] backend=%s cc-block=%u first-diff "
                            "frame=%u\n",
                            backendName, block, frame);
                        break;
                    }
                }
                for (const svms::RenderEvent& e : events) {
                    std::fprintf(stderr,
                        "[WVDIFFEV] blk=%u f=%u t=%d ch=%u d1=%u d2=%u\n",
                        block, e.frameOffset, static_cast<int>(e.type),
                        e.channel, e.data1, e.data2);
                }
            }
            std::fprintf(stderr,
                         "[WVDIFF] backend=%s cc-block=%u max=%g\n",
                         backendName, block, blockDiff);
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        std::fprintf(stderr, "[WVDIFF] backend=%s cc max=%g launches=%u\n",
                     backendName, passMaxDiff, wholeContext.launches);
        Check(passMaxDiff <= g_waveTol,
              "whole-voice lifecycle-CC blocks match the oracle per sample");
        CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, backendName);
        if (rowOpBudget != 0u) {
            Check(whole.GetPathBlocks(0) > kBlocks && whole.GetPathBlocks(1) == 0u &&
                      whole.GetPathBlocks(2) == 0u,
                  "op budget cut blocks into more whole-voice stretches, none fell back");
        }
        Check(rowOpBudget != 0u || whole.GetWholeVoiceBlocksForTest() == kBlocks,
              "CC scenario routes every block through the whole-voice "
              "renderer");
        Check(wholeContext.launches >= kBlocks,
              "CC scenario exercised in-block launches alongside the "
              "controllers");
    }
}

void TestWholeVoiceCCDifferential() { RunWholeVoiceCCDifferential(0u); }

// quality.fast_chunk_math: closed-form gain/phase per chunk is tolerance-equal
// (not bit-equal) to the frame-major oracle.  Re-run the steal, launch and CC
// differentials (their AVX2 passes carry release tails and note-off churn)
// with the toggle on, at the same tolerances, then restore the exact default.
void TestFastChunkMathDifferential() {
    svms::SetFastChunkMath(true);
    // The oracle advances phase with one float add per frame, which at large
    // phases drifts whole frames over a block (8 adds round in one direction);
    // the closed-form chunk advance is the closer one to the true phase
    // (.perf/phasedrift.py: 0.1-0.7 frames vs 0.9-3.5 after 2048 frames).  The
    // wave tolerance therefore has to cover the oracle's own drift on a
    // steep waveform, not just float-ulp differences.
    g_waveTol = 4.0e-3f;
    g_phaseTol = 0.5f;     // frames, as for muted rows
    RunWholeVoiceStealDifferential(0u);
    RunWholeVoiceLaunchDifferential(0u);
    RunWholeVoiceCCDifferential(0u);
    g_waveTol = 2.0e-5f;
    g_phaseTol = 2.0e-3f;
    svms::SetFastChunkMath(false);
}


// A tiny op-log budget forces the block splitter to cut stretches between
// event groups (no dispatch, same frames and order); the result must still
// match the oracle, and no block may fall back to dense or sparse.
void TestWholeVoiceOpBudgetSplitDifferential() {
    RunWholeVoiceCCDifferential(3u);
}

// scalarExact: the whole-voice side runs the SCALAR backend (sparse-identical
// LFO windows) and must match the forced-sparse reference to float rounding;
// otherwise the whole-voice side is AVX2 with the documented control-rate drift.
void RunWholeVoiceVibratoDifferential(bool scalarExact) {
    // CC1/channel-pressure vibrato rides the whole-voice path on every
    // backend.  AVX2 rebuilds the 64-frame LFO windows inside its kernels
    // (documented control-rate drift vs the reference); scalar/SSE2 render
    // in the exact windows the span renderer used.  The reference is the
    // frozen output of that span renderer (SVMSVibratoGolden.h), captured
    // before it was removed.  Channel 0 stays modulated from block 0,
    // mid-block CC1 sweeps depth on/mid/off, and channels 1-3 exercise the
    // per-channel gating with their own CC1/pressure events.
    if (!scalarExact &&
        !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) return;

    constexpr uint32_t kFrames = 512u;
    // The exact-window scenario stops before the saturated pool starts to
    // steal and retire: those paths diverge from the reference by design
    // (whole-voice scores victims at block start, retires at block end) and
    // are covered by the steal differentials.
    const uint32_t kBlocks = scalarExact ? 6u : 8u;
    constexpr uint32_t kPool = 48u;
    constexpr uint32_t kSeedCount = 32u;
    constexpr uint32_t kEventsPerBlock = 10u;
    const float kMaxWaveDiff = scalarExact ? 2.0e-5f : 3.0e-2f;
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

    svms::ChannelCache seedChannels;
    seedChannels.SetMasterVolume(1.0f);
    seedChannels.ControlChange(0, 1, 127);
    seedChannels.RebuildCache(cfg, 44100.0f);

    auto seedVoices = std::make_unique<svms::VoiceManager>();
    ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool, kSeedCount,
                                true, true);
    auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
    svms::ChannelCache wholeChannels = seedChannels;

    svms::RenderScalar whole;
    Check(whole.ReserveVoiceCapacity(kPool) &&
              whole.ConfigureRenderThreads(4u, kFrames),
          "vibrato differential starts the parallel renderer");
    Check(whole.SetRenderBackend(scalarExact ? svms::RenderBackend::Scalar
                                             : svms::RenderBackend::AVX2),
          "vibrato differential selects the backend");

    WholeVoiceCCContext wholeContext{wholeVoices.get(), &wholeChannels,
                                     wholeChannels.GetParams(), &cfg};
    wholeContext.ResetMasterState();
    wholeContext.vibratoLaunches = true;
    whole.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent, &wholeContext);

    WholeVoiceRng rng;
    float passMaxDiff = 0.0f;
    for (uint32_t block = 0; block < kBlocks; ++block) {
        const uint64_t blockStartFrame = 30000u + block * kFrames;
        std::vector<svms::RenderEvent> events;
        events.reserve(kEventsPerBlock);
        for (uint32_t j = 0; j < kEventsPerBlock; ++j) {
            svms::RenderEvent event{};
            event.channel = (rng.Next() % 2u) != 0u
                ? 0u : static_cast<uint8_t>(1u + rng.Next() % 3u);
            event.frameOffset = rng.Next() % kFrames;
            event.ingressSequence = block * kEventsPerBlock + j;
            const uint32_t roll = rng.Next() % 6u;
            if (roll < 2u) {
                event.type = svms::RenderEventType::NoteOn;
                event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
                event.data2 = static_cast<uint8_t>(64u + rng.Next() % 64u);
            } else if (roll == 2u) {
                event.type = svms::RenderEventType::NoteOff;
                event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
            } else if (roll == 3u) {
                event.type = svms::RenderEventType::ControlChange;
                event.data1 = 1u;
                static const uint8_t kModValues[3] = {127u, 64u, 0u};
                event.data2 = kModValues[rng.Next() % 3u];
            } else if (roll == 4u) {
                event.type = svms::RenderEventType::PitchBend;
                event.data1 = static_cast<uint8_t>(rng.Next() % 128u);
                event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
            } else {
                event.type = svms::RenderEventType::ChannelPressure;
                event.data1 = static_cast<uint8_t>(rng.Next() % 128u);
            }
            events.push_back(event);
        }
        std::stable_sort(events.begin(), events.end(),
            [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                return a.frameOffset < b.frameOffset;
            });

        std::vector<float> wholeLeft(kFrames, 0.0f),
            wholeRight(kFrames, 0.0f);
        whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                          sampleCount, wholeLeft.data(), wholeRight.data(),
                          kFrames, cfg, events.data(),
                          static_cast<uint32_t>(events.size()), true,
                          blockStartFrame);
        float blockDiff = 0.0f;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            blockDiff = (std::max)(blockDiff,
                std::fabs(kVibGoldenL[block * kFrames + frame] -
                          wholeLeft[frame]));
            blockDiff = (std::max)(blockDiff,
                std::fabs(kVibGoldenR[block * kFrames + frame] -
                          wholeRight[frame]));
        }
        std::fprintf(stderr, "[WVDIFFV] blk=%u max=%g\n", block, blockDiff);
        passMaxDiff = (std::max)(passMaxDiff, blockDiff);
    }
    std::fprintf(stderr, "[WVDIFFV] %s vibrato whole-voice vs golden max=%g\n",
                 scalarExact ? "scalar" : "avx2", passMaxDiff);
    Check(passMaxDiff <= kMaxWaveDiff,
          "whole-voice vibrato matches the frozen reference within the "
          "control-rate drift budget");
    Check(whole.GetWholeVoiceBlocksForTest() == kBlocks,
          "vibrato scenario routes every block through the whole-voice "
          "renderer");
    Check(wholeContext.launches >= kBlocks,
          "vibrato scenario exercised in-block launches");
    // Active identity (8-block run only: the 6-block exact run ends before
    // the golden pool state).
    if (!scalarExact) {
        std::vector<uint32_t> wholeIds;
        for (uint32_t i = 0; i < wholeVoices->activeCount_; ++i)
            wholeIds.push_back(
                wholeVoices->v.playIndex[wholeVoices->activeList_[i]]);
        std::sort(wholeIds.begin(), wholeIds.end());
        const std::vector<uint32_t> goldenIds(
            std::begin(kVibGoldenActive), std::end(kVibGoldenActive));
        Check(goldenIds == wholeIds,
              "vibrato scenario preserves active voice identity");
    }
}

void TestWholeVoiceVibratoDifferential() {
    RunWholeVoiceVibratoDifferential(false);
}

// Scalar/SSE2 vibrato renders on the whole-voice path in the sparse loop's
// exact LFO windows (global event + 64-frame boundaries, powf ratio).
void TestWholeVoiceScalarVibratoDifferential() {
    RunWholeVoiceVibratoDifferential(true);
}

} // namespace

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
// Exact note-on merging (SVMSNoteMerge.h).
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

int main() {
    TestMidiByteStream();
    TestReExciteKey();
    TestReExciteRenderDifferential(128u, 0);
    TestReExciteRenderDifferential(2u, 1);
    TestReExciteRenderDifferential(64u, 2);
    TestMergedNoteExactness();
    TestNoteMergerSemantics();
    TestMergedForkInFullPool();
    TestExtendedKeysAndTuning();
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_CHECK_ALWAYS_DF);
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF);
#endif
    TestWholeVoiceStealDifferential();
    TestWholeVoiceRotationStealDifferential();
    TestWholeVoiceLaunchDifferential();
    TestWholeVoiceRotationLaunchDifferential();
    TestWholeVoiceResetBarrierDifferential();
    TestFloorLimitedReleaseSamples();
    TestWholeVoiceDecimation();
    TestWholeVoiceDecimationByLevel();
    TestMutedTailCull();
    TestFilteredGroupKernel();
    TestWholeVoiceCCDifferential();
    TestFastChunkMathDifferential();
    TestWholeVoiceOpBudgetSplitDifferential();
    TestWholeVoiceVibratoDifferential();
    TestWholeVoiceScalarVibratoDifferential();
    TestHilbertPairDifferential();
    TestHilbertPairLoopAware();
    TestPerChannelLimiterDifferential();
    TestWholeVoiceProductionGateParity();
    TestPerKeyVoiceCap();
    TestStealReject();
    TestFastProducerRing();
    TestExclusiveClassLifecycle();
    TestSfzParserAndPlayback();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }

    std::puts("SVMS V3 correctness tests passed");
    return 0;
}
