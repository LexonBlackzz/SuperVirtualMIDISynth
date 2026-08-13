#include <cmath>
#include "SVMSRenderScalar.h"
#include "SVMSSoundFont.h"

#include <windows.h>
#include <avrt.h>
#include <immintrin.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

enum class Workload {
    Sustained, Envelope, Release, Steal, Dense, MixedEvents, NoteBurst
};

const char* WorkloadName(Workload workload) {
    switch (workload) {
        case Workload::Sustained: return "sustained";
        case Workload::Envelope: return "envelope";
        case Workload::Release: return "release";
        case Workload::Steal: return "steal";
        case Workload::Dense: return "dense";
        case Workload::MixedEvents: return "mixed-events";
        case Workload::NoteBurst: return "note-burst";
    }
    return "unknown";
}

bool ParseWorkload(const char* value, Workload& result) {
    if (std::strcmp(value, "sustained") == 0) result = Workload::Sustained;
    else if (std::strcmp(value, "envelope") == 0) result = Workload::Envelope;
    else if (std::strcmp(value, "release") == 0) result = Workload::Release;
    else if (std::strcmp(value, "steal") == 0) result = Workload::Steal;
    else if (std::strcmp(value, "dense") == 0) result = Workload::Dense;
    else if (std::strcmp(value, "mixed-events") == 0) result = Workload::MixedEvents;
    else if (std::strcmp(value, "note-burst") == 0) result = Workload::NoteBurst;
    else return false;
    return true;
}

void NoopBatchDispatch(const svms::RenderEvent*, uint32_t, uint32_t, void*) {}

bool gCollectBreakdown = false;
uint64_t gRenderCycles = 0u;
uint64_t gDispatchCycles = 0u;
uint64_t gStealCycles = 0u;
uint64_t gRegionResolveCycles = 0u;
uint64_t gLaunchPrepareCycles = 0u;
uint64_t gMatchedRegions = 0u;
uint64_t gBreakdownSampleCounter = 0u;
constexpr uint32_t kBreakdownSampleInterval = 1024u;
static_assert((kBreakdownSampleInterval &
               (kBreakdownSampleInterval - 1u)) == 0u,
              "breakdown sample interval must be a power of two");

double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::ceil(percentile * values.size())) - 1u;
    return values[(std::min)(index, values.size() - 1u)];
}

struct Options {
    uint32_t voices = 4096;
    uint32_t frames = 2048;
    uint32_t seconds = 60;
    uint32_t warmupSeconds = 2;
    Workload workload = Workload::Sustained;
    bool enforce = false;
    bool reference = false;
    bool breakdown = false;
    bool transactionalLaunch = true;
    bool copiedLaunchPlan = false;
    uint32_t eventStride = 1;
    uint32_t noteRate = 64000;
    uint32_t keyCount = 128;
    uint32_t attackFrames = 0;
    std::string soundFontPath;
    uint32_t pinCore = UINT32_MAX;
    bool automaticBackend = true;
    svms::RenderBackend backend = svms::RenderBackend::Scalar;
};

bool ParseBackend(const char* value, Options& options) {
    if (std::strcmp(value, "auto") == 0) {
        options.automaticBackend = true;
        return true;
    }
    options.automaticBackend = false;
    if (std::strcmp(value, "scalar") == 0)
        options.backend = svms::RenderBackend::Scalar;
    else if (std::strcmp(value, "sse2") == 0)
        options.backend = svms::RenderBackend::SSE2;
    else if (std::strcmp(value, "avx2") == 0)
        options.backend = svms::RenderBackend::AVX2;
    else return false;
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        auto nextNumber = [&](uint32_t& destination) {
            if (i + 1 >= argc) return false;
            destination = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            return true;
        };
        if (std::strcmp(argv[i], "--voices") == 0) {
            if (!nextNumber(options.voices)) return false;
        } else if (std::strcmp(argv[i], "--frames") == 0) {
            if (!nextNumber(options.frames)) return false;
        } else if (std::strcmp(argv[i], "--seconds") == 0) {
            if (!nextNumber(options.seconds)) return false;
        } else if (std::strcmp(argv[i], "--warmup") == 0) {
            if (!nextNumber(options.warmupSeconds)) return false;
        } else if (std::strcmp(argv[i], "--workload") == 0) {
            if (i + 1 >= argc || !ParseWorkload(argv[++i], options.workload)) return false;
        } else if (std::strcmp(argv[i], "--event-stride") == 0) {
            if (!nextNumber(options.eventStride) || options.eventStride == 0u)
                return false;
        } else if (std::strcmp(argv[i], "--note-rate") == 0) {
            if (!nextNumber(options.noteRate) || options.noteRate == 0u)
                return false;
        } else if (std::strcmp(argv[i], "--key-count") == 0) {
            if (!nextNumber(options.keyCount) || options.keyCount == 0u ||
                options.keyCount > 128u) return false;
        } else if (std::strcmp(argv[i], "--attack-frames") == 0) {
            if (!nextNumber(options.attackFrames)) return false;
        } else if (std::strcmp(argv[i], "--soundfont") == 0) {
            if (i + 1 >= argc) return false;
            options.soundFontPath = argv[++i];
        } else if (std::strcmp(argv[i], "--pin-core") == 0) {
            if (!nextNumber(options.pinCore) || options.pinCore >= 64u)
                return false;
        } else if (std::strcmp(argv[i], "--backend") == 0) {
            if (i + 1 >= argc || !ParseBackend(argv[++i], options)) return false;
        } else if (std::strcmp(argv[i], "--enforce") == 0) {
            options.enforce = true;
        } else if (std::strcmp(argv[i], "--reference") == 0) {
            options.reference = true;
        } else if (std::strcmp(argv[i], "--breakdown") == 0) {
            options.breakdown = true;
        } else if (std::strcmp(argv[i], "--launch-path") == 0) {
            if (i + 1 >= argc) return false;
            const char* path = argv[++i];
            if (std::strcmp(path, "transactional") == 0)
                options.transactionalLaunch = true;
            else if (std::strcmp(path, "legacy") == 0)
                options.transactionalLaunch = false;
            else
                return false;
        } else if (std::strcmp(argv[i], "--launch-plan") == 0) {
            if (i + 1 >= argc) return false;
            const char* plan = argv[++i];
            if (std::strcmp(plan, "direct") == 0)
                options.copiedLaunchPlan = false;
            else if (std::strcmp(plan, "copy") == 0)
                options.copiedLaunchPlan = true;
            else
                return false;
        } else if (std::strcmp(argv[i], "--quick") == 0) {
            options.seconds = 1;
            options.warmupSeconds = 1;
        } else {
            return false;
        }
    }
    return options.voices >= 1u && options.voices <= svms::kMaxPolyphony &&
           options.frames >= 16u && options.frames <= 8192u && options.seconds > 0u;
}

bool ConfigureVoices(svms::VoiceManager& voices, svms::ChannelCache& channels,
                     const svms::RuntimeConfigSnapshot& cfg,
                     uint32_t voiceCount, Workload workload,
                     uint32_t sampleFrames) {
    if (!voices.Initialize(voiceCount, 44100)) return false;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    constexpr uint32_t regionFrames = 2048;
    const uint32_t regionCount = sampleFrames / regionFrames;

    for (uint32_t i = 0; i < voiceCount; ++i) {
        const svms::VoiceHandle handle = voices.AllocateVoice(
            static_cast<uint8_t>(i & 15u), static_cast<uint8_t>(24u + i % 88u),
            static_cast<uint8_t>(64u + i % 64u));
        const uint32_t start = (i % regionCount) * regionFrames;
        const float phaseStep = 0.5f + static_cast<float>(i % 97u) / 64.0f;
        voices.SetVoiceSample(handle, start, start + regionFrames,
                              start + 16u, start + regionFrames - 16u,
                              1u, phaseStep, 1u);

        if (workload == Workload::Envelope) {
            if ((i & 1u) == 0u) {
                voices.SetVoiceEnvelope(handle, 1.0f, 0.4f, 0u, 0u,
                                        UINT32_MAX, 0u, 1.0e-9f, 1.0f, 0.99999f);
            } else {
                voices.SetVoiceEnvelope(handle, 1.0f, 0.4f, 0u, 0u, 0u,
                                        UINT32_MAX, 0.0f, 0.9999999f, 0.99999f);
            }
        } else {
            voices.SetVoiceEnvelope(handle, 1.0f, 0.7f, 0u, 0u, 0u, 0u,
                                    0.0f, 1.0f, 0.9999999f);
        }
        voices.SetVoiceGain(handle, 0.001f, 0.001f);
        voices.RefreshMixGain(handle, channels.GetParams()[i & 15u]);

        if (workload == Workload::Release) voices.StartRelease(handle);
    }
    return true;
}

void PerformSteals(svms::VoiceManager& voices, const svms::ChannelCache& channels,
                   uint32_t& sequence, uint32_t count, uint32_t sampleFrames,
                   const svms::RenderEvent* sourceEvent = nullptr,
                   uint32_t attackFrames = 0u,
                   bool transactional = false,
                   bool copiedLaunchPlan = false,
                   uint32_t profileScale = 0u) {
    constexpr uint32_t regionFrames = 2048;
    const uint32_t regionCount = sampleFrames / regionFrames;
    // Every physical SF2 region produced by one MIDI note belongs to one
    // playIndex. This makes the synthetic layered benchmark exercise the
    // same atomic stereo-stealing path as the production driver.
    const uint32_t playIndex = sequence + 1u;
    if (transactional && count <= 8u) {
        const uint64_t prepareBegin = profileScale != 0u ? __rdtsc() : 0u;
        const uint8_t channel = sourceEvent ? sourceEvent->channel
            : static_cast<uint8_t>(sequence & 15u);
        const uint8_t note = sourceEvent ? sourceEvent->data1
            : static_cast<uint8_t>(24u + sequence % 88u);
        const uint8_t velocity = sourceEvent ? sourceEvent->data2
            : static_cast<uint8_t>(64u + sequence % 64u);
        svms::VoiceConfiguration setups[8]{};
        svms::VoiceHandle handles[8]{};
        for (uint32_t layer = 0u; layer < count; ++layer) {
            const uint32_t layerSequence = sequence + layer;
            const uint32_t start = (layerSequence % regionCount) * regionFrames;
            auto& setup = setups[layer];
            setup.sampleStart = start;
            setup.sampleEnd = start + regionFrames;
            setup.loopStart = start + 16u;
            setup.loopEnd = start + regionFrames - 16u;
            setup.loopMode = 1u;
            setup.phaseStep = 0.5f +
                static_cast<float>(layerSequence % 97u) / 64.0f;
            setup.basePhaseStep = setup.phaseStep;
            setup.initialGain = 1.0f;
            setup.attackSamples = attackFrames;
            setup.attackGainStep = attackFrames > 0u
                ? setup.initialGain / static_cast<float>(attackFrames) : 0.0f;
            setup.sustainLevel = 0.7f;
            setup.releaseDecay = 0.9999999f;
            setup.gainLeft = setup.gainRight = 0.001f;
        }
        if (profileScale != 0u)
            gLaunchPrepareCycles +=
                (__rdtsc() - prepareBegin) * profileScale;
        sequence += count;
        if (copiedLaunchPlan) {
            svms::VoiceConfiguration copied[8]{};
            for (uint32_t layer = 0u; layer < count; ++layer) {
                copied[layer] = setups[layer];
                copied[layer].playIndex = playIndex;
            }
            voices.LaunchVoiceGroup(channel, note, velocity, copied, count,
                                    channels.GetParams()[channel], handles);
        } else {
            voices.LaunchVoiceGroup(channel, note, velocity, setups, count,
                                    playIndex, channels.GetParams()[channel],
                                    handles);
        }
        return;
    }
    for (uint32_t i = 0; i < count; ++i, ++sequence) {
        const uint8_t channel = sourceEvent ? sourceEvent->channel
            : static_cast<uint8_t>(sequence & 15u);
        const uint8_t note = sourceEvent ? sourceEvent->data1
            : static_cast<uint8_t>(24u + sequence % 88u);
        const uint8_t velocity = sourceEvent ? sourceEvent->data2
            : static_cast<uint8_t>(64u + sequence % 64u);
        const svms::VoiceHandle handle = voices.AllocateVoiceOrSteal(
            channel, note, velocity, nullptr, true);
        if (handle == svms::kInvalidVoice) continue;
        const uint32_t start = (sequence % regionCount) * regionFrames;
        svms::VoiceConfiguration setup{};
        setup.sampleStart = start;
        setup.sampleEnd = start + regionFrames;
        setup.loopStart = start + 16u;
        setup.loopEnd = start + regionFrames - 16u;
        setup.loopMode = 1u;
        setup.playIndex = playIndex;
        setup.phaseStep = 0.5f +
            static_cast<float>(sequence % 97u) / 64.0f;
        setup.basePhaseStep = setup.phaseStep;
        setup.initialGain = 1.0f;
        setup.attackSamples = attackFrames;
        setup.attackGainStep = attackFrames > 0u
            ? setup.initialGain / static_cast<float>(attackFrames) : 0.0f;
        setup.sustainLevel = 0.7f;
        setup.releaseDecay = 0.9999999f;
        setup.gainLeft = 0.001f;
        setup.gainRight = 0.001f;
        voices.ConfigureVoice(handle, setup, channels.GetParams()[channel], true);
    }
}

struct MixedDispatchContext {
    svms::VoiceManager* voices;
    svms::ChannelCache* channels;
    const svms::RuntimeConfigSnapshot* config;
    uint32_t sampleFrames;
    uint32_t sequence;
    const svms::SF2Data* soundFont;
    uint32_t presetIndex;
    uint32_t attackFrames;
    bool transactionalLaunch;
    bool copiedLaunchPlan;
    uint16_t regionCacheCount[128];
    uint32_t regionCacheIndices[128][8];
};

void MixedDispatch(const svms::RenderEvent& event, uint32_t, void* userData) {
    const uint32_t profileScale = gCollectBreakdown &&
        ((gBreakdownSampleCounter++ &
          (kBreakdownSampleInterval - 1u)) == 0u)
        ? kBreakdownSampleInterval : 0u;
    const uint64_t dispatchBegin = profileScale != 0u ? __rdtsc() : 0u;
    auto* context = static_cast<MixedDispatchContext*>(userData);
    auto& voices = *context->voices;
    auto& channels = *context->channels;
    switch (event.type) {
        case svms::RenderEventType::NoteOn: {
            const uint64_t resolveBegin =
                profileScale != 0u ? __rdtsc() : 0u;
            uint32_t layerCount = 1u;
            if (context->soundFont) {
                const svms::SFSampleRegion* regions[512]{};
                const uint16_t cachedCount =
                    context->regionCacheCount[event.data1];
                if (cachedCount != UINT16_MAX) {
                    layerCount = cachedCount;
                    for (uint32_t i = 0; i < layerCount; ++i)
                        regions[i] = &context->soundFont->regions[
                            context->regionCacheIndices[event.data1][i]];
                } else {
                    layerCount = svms::sf2_find_regions(
                        context->soundFont, context->presetIndex, event.data1,
                        event.data2, regions, 512u);
                    if (layerCount <= 8u) {
                        context->regionCacheCount[event.data1] =
                            static_cast<uint16_t>(layerCount);
                        for (uint32_t i = 0; i < layerCount; ++i)
                            context->regionCacheIndices[event.data1][i] =
                                static_cast<uint32_t>(regions[i] -
                                    context->soundFont->regions);
                    }
                }
                gMatchedRegions += layerCount;
                if (layerCount == 0u) break;
            }
            if (profileScale != 0u)
                gRegionResolveCycles +=
                    (__rdtsc() - resolveBegin) * profileScale;
            if (profileScale != 0u) {
                const uint64_t begin = __rdtsc();
                PerformSteals(voices, channels, context->sequence, layerCount,
                              context->sampleFrames, &event,
                              context->attackFrames,
                              context->transactionalLaunch,
                              context->copiedLaunchPlan, profileScale);
                const uint64_t end = __rdtsc();
                gStealCycles += (end - begin) * profileScale;
                break;
            }
            PerformSteals(voices, channels, context->sequence, layerCount,
                          context->sampleFrames, &event,
                          context->attackFrames,
                          context->transactionalLaunch,
                          context->copiedLaunchPlan);
            break;
        }
        case svms::RenderEventType::NoteOff: {
            svms::VoiceHandle last = svms::kInvalidVoice;
            voices.ForEachChannelActive(event.channel,
                [&](svms::VoiceHandle handle) { last = handle; });
            if (last != svms::kInvalidVoice) voices.StartRelease(last);
            break;
        }
        case svms::RenderEventType::ControlChange:
            channels.ControlChange(event.channel, event.data1, event.data2);
            channels.RebuildCache(*context->config, 44100.0f);
            if (event.data1 == 7u || event.data1 == 10u || event.data1 == 11u)
                voices.RefreshMixGainsForChannel(
                    event.channel, channels.GetParams()[event.channel]);
            break;
        case svms::RenderEventType::PitchBend: {
            const int32_t wheel = (static_cast<int32_t>(event.data2) << 7) |
                                  event.data1;
            const float semitones = static_cast<float>(wheel - 8192) / 4096.0f;
            const float ratio = std::pow(2.0f, semitones / 12.0f);
            voices.ForEachChannelActive(event.channel,
                [&](svms::VoiceHandle handle) {
                voices.v.phaseIncs[handle] =
                    voices.v.basePhaseIncs[handle] * ratio;
            });
            break;
        }
        default:
            break;
    }
    if (profileScale != 0u) {
        const uint64_t dispatchEnd = __rdtsc();
        gDispatchCycles += (dispatchEnd - dispatchBegin) * profileScale;
    }
}

void MixedBatchDispatch(const svms::RenderEvent* events, uint32_t eventCount,
                        uint32_t blockCursor, void* userData) {
    for (uint32_t i = 0; i < eventCount; ++i)
        MixedDispatch(events[i], blockCursor, userData);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::fprintf(stderr,
            "usage: svms_v3_bench [--voices 1..4096] [--frames 16..8192] "
            "[--seconds N] [--warmup N] [--workload sustained|envelope|release|steal|dense|mixed-events|note-burst] "
            "[--event-stride N] [--note-rate N] [--key-count 1..128] [--attack-frames N] [--soundfont PATH] "
            "[--backend auto|scalar|sse2|avx2] "
            "[--launch-path legacy|transactional] "
            "[--launch-plan direct|copy] "
            "[--breakdown] "
            "[--pin-core 0..63] "
            "[--quick] [--reference] [--enforce]\n");
        return 1;
    }

    if (options.pinCore != UINT32_MAX)
        SetThreadAffinityMask(GetCurrentThread(), 1ull << options.pinCore);
    DWORD mmcssTaskIndex = 0u;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    constexpr uint32_t sampleFrames = 64u * 2048u;
    std::vector<float> samples(sampleFrames);
    for (uint32_t i = 0; i < sampleFrames; ++i) {
        samples[i] = 0.45f * std::sin(static_cast<float>(i) * 0.017f) +
                     0.2f * std::sin(static_cast<float>(i) * 0.071f);
    }

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.velocityCurve = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.interpolation = svms::InterpolationMode::Linear;
    cfg.correctnessMode = true;

    auto voices = std::make_unique<svms::VoiceManager>();
    svms::ChannelCache channels;
    if (!ConfigureVoices(*voices, channels, cfg, options.voices,
                         options.workload, sampleFrames)) {
        std::fprintf(stderr, "cannot allocate voice storage\n");
        return 3;
    }
    auto renderer = std::make_unique<svms::RenderScalar>();
    if (!renderer->ReserveVoiceCapacity(options.voices)) {
        std::fprintf(stderr, "cannot allocate renderer scratch\n");
        return 3;
    }
    if (!options.automaticBackend && !renderer->SetRenderBackend(options.backend)) {
        std::fprintf(stderr, "requested render backend is not supported by this CPU/build\n");
        return 3;
    }

    std::vector<svms::RenderEvent> events;
    std::unique_ptr<svms::SF2Data> soundFont;
    uint32_t soundFontPreset = UINT32_MAX;
    if (!options.soundFontPath.empty()) {
        soundFont = std::make_unique<svms::SF2Data>();
        if (!svms::sf2_load(options.soundFontPath.c_str(), soundFont.get())) {
            std::fprintf(stderr, "failed to load benchmark SoundFont: %s\n",
                         options.soundFontPath.c_str());
            return 4;
        }
        svms::sf2_build_regions(soundFont.get());
        if (!svms::sf2_resolve_preset(soundFont.get(), 0u, 0u, false,
                                      &soundFontPreset)) {
            std::fprintf(stderr, "benchmark SoundFont has no bank 0 program 0\n");
            return 4;
        }
    }
    MixedDispatchContext mixedContext{
        voices.get(), &channels, &cfg, sampleFrames, options.voices,
        soundFont.get(), soundFontPreset, options.attackFrames,
        options.transactionalLaunch, options.copiedLaunchPlan};
    std::fill(std::begin(mixedContext.regionCacheCount),
              std::end(mixedContext.regionCacheCount), UINT16_MAX);
    if (options.workload == Workload::Dense ||
        options.workload == Workload::MixedEvents ||
        options.workload == Workload::NoteBurst) {
        const uint32_t eventCount = options.workload == Workload::NoteBurst
            ? static_cast<uint32_t>((static_cast<uint64_t>(options.noteRate) *
                                     options.frames + 44099u) / 44100u)
            : (options.frames + options.eventStride - 1u) / options.eventStride;
        events.resize(eventCount);
        for (uint32_t index = 0; index < eventCount; ++index) {
            const uint32_t frame = options.workload == Workload::NoteBurst
                ? static_cast<uint32_t>(static_cast<uint64_t>(index) *
                                        options.frames / eventCount)
                : index * options.eventStride;
            auto& event = events[index];
            event.frameOffset = frame;
            event.ingressSequence = index;
            event.channel = static_cast<uint8_t>(index & 15u);
            if (options.workload == Workload::Dense) {
                event.type = svms::RenderEventType::ControlChange;
                continue;
            }
            if (options.workload == Workload::NoteBurst) {
                event.type = svms::RenderEventType::NoteOn;
                event.data1 = static_cast<uint8_t>(index % options.keyCount);
                event.data2 = 127u;
                continue;
            }
            switch (index % 6u) {
                case 0u:
                    event.type = svms::RenderEventType::NoteOn;
                    event.data1 = static_cast<uint8_t>(48u + index % 48u);
                    event.data2 = static_cast<uint8_t>(80u + index % 47u);
                    break;
                case 1u:
                    event.type = svms::RenderEventType::NoteOff;
                    break;
                case 2u:
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 11u;
                    event.data2 = static_cast<uint8_t>(48u + index % 80u);
                    break;
                case 3u:
                    event.type = svms::RenderEventType::PitchBend;
                    event.data1 = static_cast<uint8_t>((index * 37u) & 0x7fu);
                    event.data2 = static_cast<uint8_t>(48u + index % 32u);
                    break;
                case 4u:
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 7u;
                    event.data2 = static_cast<uint8_t>(72u + index % 56u);
                    break;
                default:
                    event.type = svms::RenderEventType::NoteOn;
                    event.data1 = static_cast<uint8_t>(36u + index % 60u);
                    event.data2 = static_cast<uint8_t>(96u + index % 31u);
                    break;
            }
        }
        renderer->SetEventBatchDispatcher(
            options.workload == Workload::Dense
                ? NoopBatchDispatch : MixedBatchDispatch,
            options.workload == Workload::Dense ? nullptr : &mixedContext);
    }

    std::vector<float> left(options.frames);
    std::vector<float> right(options.frames);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const uint32_t callbacksPerSecond =
        (44100u + options.frames - 1u) / options.frames;
    const uint32_t warmupCallbacks = options.warmupSeconds * callbacksPerSecond;
    const uint32_t measuredCallbacks = options.seconds * callbacksPerSecond;
    uint64_t absoluteFrame = 0u;
    uint32_t stealSequence = options.voices;

    auto renderOne = [&] {
        if (options.workload == Workload::Steal)
            PerformSteals(*voices, channels, stealSequence, 16u, sampleFrames,
                          nullptr, 0u, options.transactionalLaunch, false,
                          gCollectBreakdown ? 1u : 0u);
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        const uint64_t renderBegin = gCollectBreakdown ? __rdtsc() : 0u;
        if (options.reference) {
            renderer->RenderBlockReference(
                *voices, channels, samples.data(), sampleFrames,
                left.data(), right.data(), options.frames, cfg,
                events.empty() ? nullptr : events.data(),
                static_cast<uint32_t>(events.size()), true, absoluteFrame);
        } else {
            renderer->RenderBlock(*voices, channels, samples.data(), sampleFrames,
                                  left.data(), right.data(), options.frames, cfg,
                                  events.empty() ? nullptr : events.data(),
                                  static_cast<uint32_t>(events.size()), true, absoluteFrame);
        }
        if (gCollectBreakdown) {
            const uint64_t renderEnd = __rdtsc();
            gRenderCycles += renderEnd - renderBegin;
        }
        absoluteFrame += options.frames;
    };

    for (uint32_t i = 0; i < warmupCallbacks; ++i) renderOne();

    gRenderCycles = gDispatchCycles = gStealCycles = gMatchedRegions = 0u;
    gRegionResolveCycles = gLaunchPrepareCycles = 0u;
    gBreakdownSampleCounter = 0u;
    voices->ResetGroupReuseCountersForTest();
    gCollectBreakdown = options.breakdown;

    std::vector<double> callbackPercent;
    callbackPercent.reserve(measuredCallbacks);
    double elapsedTotal = 0.0;
    uint64_t cycleTotal = 0u;
    uint32_t consecutiveOverruns = 0u;
    uint32_t maximumConsecutiveOverruns = 0u;
    const uint32_t initialSteals = voices->stealCount_;
    const double budgetSeconds = static_cast<double>(options.frames) / 44100.0;
    for (uint32_t i = 0; i < measuredCallbacks; ++i) {
        LARGE_INTEGER begin{}, end{};
        ULONG64 cycleBegin = 0u, cycleEnd = 0u;
        QueryThreadCycleTime(GetCurrentThread(), &cycleBegin);
        QueryPerformanceCounter(&begin);
        renderOne();
        QueryPerformanceCounter(&end);
        QueryThreadCycleTime(GetCurrentThread(), &cycleEnd);
        const double elapsed = static_cast<double>(end.QuadPart - begin.QuadPart) /
                               static_cast<double>(frequency.QuadPart);
        elapsedTotal += elapsed;
        callbackPercent.push_back(elapsed / budgetSeconds * 100.0);
        cycleTotal += cycleEnd - cycleBegin;
        if (elapsed > budgetSeconds) {
            ++consecutiveOverruns;
            maximumConsecutiveOverruns =
                (std::max)(maximumConsecutiveOverruns, consecutiveOverruns);
        } else {
            consecutiveOverruns = 0u;
        }
    }

    const double p50 = Percentile(callbackPercent, 0.50);
    const double p95 = Percentile(callbackPercent, 0.95);
    const double p99 = Percentile(callbackPercent, 0.99);
    const double p999 = Percentile(callbackPercent, 0.999);
    const double maximum = *std::max_element(callbackPercent.begin(), callbackPercent.end());
    const double voiceSamples = static_cast<double>(options.voices) * options.frames *
                                measuredCallbacks;
    const double voiceSamplesPerSecond = voiceSamples / elapsedTotal;
    const double cyclesPerVoiceSample = voiceSamples > 0.0
        ? static_cast<double>(cycleTotal) / voiceSamples : 0.0;
    const double eventsPerSecond = elapsedTotal > 0.0
        ? static_cast<double>(events.size()) * measuredCallbacks / elapsedTotal : 0.0;
    const uint32_t measuredSteals = voices->stealCount_ - initialSteals;
    const double stealsPerSecond = elapsedTotal > 0.0
        ? static_cast<double>(measuredSteals) / elapsedTotal : 0.0;
    uint32_t classCounts[svms::kVoiceRenderClassCount]{};
    for (uint32_t classIndex = 0; classIndex < svms::kVoiceRenderClassCount;
         ++classIndex) {
        classCounts[classIndex] = voices->GetRenderClassCount(
            static_cast<svms::VoiceRenderClass>(classIndex));
    }
    uint32_t groupedVoices = 0u;
    for (uint32_t position = 0u; position < voices->GetActiveCount(); ++position)
        groupedVoices += voices->GetPlayGroupSizeForTest(
            static_cast<svms::VoiceHandle>(voices->activeList_[position])) > 1u;

    std::printf(
        "{\"renderer\":\"%s\",\"backend\":\"%s\",\"workload\":\"%s\",\"launch_path\":\"%s\",\"launch_plan\":\"%s\",\"voices\":%u,\"frames\":%u,"
        "\"callbacks\":%u,\"event_stride\":%u,\"note_rate\":%u,\"key_count\":%u,\"attack_frames\":%u,"
        "\"soundfont_regions\":%u,\"preset_regions\":%u,\"pinned_core\":%d,"
        "\"voice_soa_bytes\":%zu,\"voice_manager_bytes\":%zu,\"renderer_bytes\":%zu,"
        "\"voice_samples_per_second\":%.0f,"
        "\"cycles_per_voice_sample\":%.3f,\"events_per_second\":%.0f,"
        "\"steals_per_second\":%.0f,\"matched_regions\":%llu,\"max_consecutive_overruns\":%u,"
        "\"cycle_breakdown\":{\"total\":%llu,\"synthesis\":%llu,\"event_dispatch\":%llu,"
        "\"region_resolution\":%llu,\"launch_preparation\":%llu,\"index_and_steal\":%llu},"
        "\"group_reuse\":{\"attempts\":%llu,\"matches\":%llu,\"reserved\":%llu,"
        "\"smaller\":%llu,\"larger\":%llu,\"grouped_voices\":%u},"
        "\"render_classes\":{\"sustained_loop\":%u,\"sustained_one_shot\":%u,"
        "\"transient_loop\":%u,\"release_loop\":%u,\"release_one_shot\":%u,"
        "\"generic\":%u,\"steal_tails\":%u},"
        "\"callback_percent\":{\"p50\":%.2f,\"p95\":%.2f,"
        "\"p99\":%.2f,\"p99_9\":%.2f,\"max\":%.2f}}\n",
        options.reference ? "reference" : "span", renderer->GetRenderBackendName(),
        WorkloadName(options.workload),
        options.transactionalLaunch ? "transactional" : "legacy",
        options.copiedLaunchPlan ? "copy" : "direct",
        options.voices, options.frames,
        measuredCallbacks, options.eventStride, options.noteRate, options.keyCount,
        options.attackFrames,
        soundFont ? soundFont->regionCount : 0u,
        soundFont ? soundFont->presetRegionCount[soundFontPreset] : 0u,
        options.pinCore == UINT32_MAX ? -1 : static_cast<int>(options.pinCore),
        voices->v.GetAllocatedBytes(), voices->GetAllocatedBytes(),
        renderer->GetAllocatedBytes(),
        voiceSamplesPerSecond,
        cyclesPerVoiceSample, eventsPerSecond, stealsPerSecond,
        static_cast<unsigned long long>(gMatchedRegions), maximumConsecutiveOverruns,
        static_cast<unsigned long long>(gRenderCycles),
        static_cast<unsigned long long>(gRenderCycles -
            (std::min)(gRenderCycles, gDispatchCycles)),
        static_cast<unsigned long long>(gDispatchCycles -
            (std::min)(gDispatchCycles, gStealCycles + gRegionResolveCycles)),
        static_cast<unsigned long long>(gRegionResolveCycles),
        static_cast<unsigned long long>(gLaunchPrepareCycles),
        static_cast<unsigned long long>(gStealCycles -
            (std::min)(gStealCycles, gLaunchPrepareCycles)),
        static_cast<unsigned long long>(voices->GetGroupReuseAttemptCountForTest()),
        static_cast<unsigned long long>(voices->GetGroupReuseMatchCountForTest()),
        static_cast<unsigned long long>(voices->GetGroupReuseReservedCountForTest()),
        static_cast<unsigned long long>(voices->GetGroupReuseSmallerCountForTest()),
        static_cast<unsigned long long>(voices->GetGroupReuseLargerCountForTest()),
        groupedVoices,
        classCounts[static_cast<uint32_t>(svms::VoiceRenderClass::SustainedLoop)],
        classCounts[static_cast<uint32_t>(svms::VoiceRenderClass::SustainedOneShot)],
        classCounts[static_cast<uint32_t>(svms::VoiceRenderClass::TransientLoop)],
        classCounts[static_cast<uint32_t>(svms::VoiceRenderClass::ReleaseLoop)],
        classCounts[static_cast<uint32_t>(svms::VoiceRenderClass::ReleaseOneShot)],
        classCounts[static_cast<uint32_t>(svms::VoiceRenderClass::Generic)],
        voices->GetStealTailCount(), p50, p95, p99, p999, maximum);

    int result = 0;
    if (options.enforce && options.voices == 2000u &&
        options.workload == Workload::NoteBurst &&
        options.noteRate >= 943000u) {
        if (p99 >= 40.0 || maximumConsecutiveOverruns != 0u) result = 2;
    } else if (options.enforce && options.voices == 4096u) {
        const double limit = options.workload == Workload::MixedEvents ? 35.0
            : options.workload == Workload::Dense ? 25.0
            : options.workload == Workload::Sustained ? 60.0 : 70.0;
        if (p99 >= limit) result = 2;
    }
    if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
    if (soundFont) svms::sf2_free(soundFont.get());
    return result;
}
