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
    Sustained, Envelope, Release, Steal, Dense, MixedEvents, NoteBurst,
    ChoppedNotes
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
        case Workload::ChoppedNotes: return "chopped-notes";
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
    else if (std::strcmp(value, "chopped-notes") == 0) result = Workload::ChoppedNotes;
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
uint64_t gRdtscPairOverhead = 0u;
constexpr uint32_t kBreakdownSampleInterval = 4096u;
static_assert((kBreakdownSampleInterval &
               (kBreakdownSampleInterval - 1u)) == 0u,
              "breakdown sample interval must be a power of two");

uint64_t ScaledProfileDelta(uint64_t begin, uint64_t end, uint32_t scale,
                            uint32_t timestampPairs = 1u) {
    const uint64_t elapsed = end - begin;
    const uint64_t overhead = gRdtscPairOverhead * timestampPairs;
    return (elapsed > overhead ? elapsed - overhead : 0u) * scale;
}

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
    bool coverageProfile = false;
    bool transactionalLaunch = true;
    bool copiedLaunchPlan = false;
    bool batchNoteOffIndex = false;
    bool launchChurnProfile = false;
    bool volatileFallbackScan = false;
    uint32_t genericVoices = 0;
    uint32_t eventStride = 1;
    uint32_t noteRate = 64000;
    uint32_t keyCount = 128;
    uint32_t baseNote = 0;
    uint32_t keyStride = 1;
    uint32_t attackFrames = 0;
    uint32_t noteLengthFrames = 1;
    uint32_t renderThreads = 1;
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
        } else if (std::strcmp(argv[i], "--generic-voices") == 0) {
            if (!nextNumber(options.genericVoices)) return false;
        } else if (std::strcmp(argv[i], "--event-stride") == 0) {
            if (!nextNumber(options.eventStride) || options.eventStride == 0u)
                return false;
        } else if (std::strcmp(argv[i], "--note-rate") == 0) {
            if (!nextNumber(options.noteRate) || options.noteRate == 0u)
                return false;
        } else if (std::strcmp(argv[i], "--key-count") == 0) {
            if (!nextNumber(options.keyCount) || options.keyCount == 0u ||
                options.keyCount > 128u) return false;
        } else if (std::strcmp(argv[i], "--base-note") == 0) {
            if (!nextNumber(options.baseNote) || options.baseNote > 127u)
                return false;
        } else if (std::strcmp(argv[i], "--key-stride") == 0) {
            if (!nextNumber(options.keyStride) || options.keyStride == 0u ||
                options.keyStride > 127u) return false;
        } else if (std::strcmp(argv[i], "--attack-frames") == 0) {
            if (!nextNumber(options.attackFrames)) return false;
        } else if (std::strcmp(argv[i], "--note-length-frames") == 0) {
            if (!nextNumber(options.noteLengthFrames) ||
                options.noteLengthFrames > options.frames) return false;
        } else if (std::strcmp(argv[i], "--render-threads") == 0) {
            if (!nextNumber(options.renderThreads) ||
                options.renderThreads < 1u || options.renderThreads > 64u)
                return false;
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
        } else if (std::strcmp(argv[i], "--coverage") == 0) {
            options.coverageProfile = true;
        } else if (std::strcmp(argv[i], "--launch-churn") == 0) {
            options.launchChurnProfile = true;
        } else if (std::strcmp(argv[i], "--volatile-selection") == 0) {
            if (i + 1 >= argc) return false;
            const char* selection = argv[++i];
            if (std::strcmp(selection, "heap") == 0)
                options.volatileFallbackScan = false;
            else if (std::strcmp(selection, "scan") == 0)
                options.volatileFallbackScan = true;
            else
                return false;
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
        } else if (std::strcmp(argv[i], "--noteoff-index") == 0) {
            if (i + 1 >= argc) return false;
            const char* mode = argv[++i];
            if (std::strcmp(mode, "batch") == 0)
                options.batchNoteOffIndex = true;
            else if (std::strcmp(mode, "immediate") == 0)
                options.batchNoteOffIndex = false;
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
           options.frames >= 16u && options.frames <= 8192u &&
           options.noteLengthFrames <= options.frames && options.seconds > 0u;
}

bool ConfigureVoices(svms::VoiceManager& voices, svms::ChannelCache& channels,
                     const svms::RuntimeConfigSnapshot& cfg,
                     uint32_t voiceCount, Workload workload,
                     uint32_t sampleFrames, uint32_t genericVoices) {
    if (!voices.Initialize(voiceCount, 44100)) return false;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    constexpr uint32_t regionFrames = 2048;
    const uint32_t regionCount = sampleFrames / regionFrames;

    for (uint32_t i = 0; i < voiceCount; ++i) {
        const bool genericVoice = genericVoices != 0u &&
                                  (workload == Workload::Release ||
                                   workload == Workload::ChoppedNotes) &&
                                  i < genericVoices;
        const svms::VoiceHandle handle = voices.AllocateVoice(
            static_cast<uint8_t>(i & 15u),
            static_cast<uint8_t>(genericVoice ? 127u : 24u + i % 88u),
            static_cast<uint8_t>(64u + i % 64u));
        const uint32_t start = (i % regionCount) * regionFrames;
        const float phaseStep = 0.5f + static_cast<float>(i % 97u) / 64.0f;
        // Generic-class mimic: a sustaining, non-looping voice (like a bass
        // hum whose SF2 region has no loop points).  Used to reproduce the
        // live "one Generic voice correlates with a CPU jump" signature.
        // Note 127 keeps it outside the chopped-notes key range so stray
        // note-offs cannot release it.
        if (genericVoice) {
            voices.SetVoiceSample(handle, start, start + regionFrames,
                                  start + 16u, start + regionFrames - 16u,
                                  0u, 0.0005f, 1u);
        } else {
            voices.SetVoiceSample(handle, start, start + regionFrames,
                                  start + 16u, start + regionFrames - 16u,
                                  1u, phaseStep, 1u);
        }

        if (genericVoice) {
            // Non-loop active voice in the decay stage -> ClassifyVoice
            // returns Generic.  Slow decay so it survives the whole run.
            voices.SetVoiceEnvelope(handle, 1.0f, 0.4f, 0u, 0u, 0u,
                                    UINT32_MAX, 0.0f, 0.99999999f, 0.99999f);
        } else if (workload == Workload::Envelope) {
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
        voices.SetVoiceGain(handle, genericVoice ? 1.0f : 0.001f,
                            genericVoice ? 1.0f : 0.001f);
        voices.SetVoiceSoundFontIdentity(
            handle, 0u, static_cast<uint16_t>(i & 0xffffu));
        voices.RefreshMixGain(handle, channels.GetParams()[i & 15u]);

        if (workload == Workload::Release && !genericVoice)
            voices.StartRelease(handle);
    }
    return true;
}

void PrepareSyntheticLaunchPlan(svms::VoiceConfiguration* setups,
                                uint32_t count, uint32_t sampleFrames,
                                uint8_t note, uint32_t attackFrames) {
    constexpr uint32_t regionFrames = 2048u;
    const uint32_t regionCount = sampleFrames / regionFrames;
    for (uint32_t layer = 0u; layer < count; ++layer) {
        const uint32_t identity = static_cast<uint32_t>(note) * 8u + layer;
        const uint32_t start = (identity % regionCount) * regionFrames;
        auto& setup = setups[layer];
        setup = svms::VoiceConfiguration{};
        setup.sampleStart = start;
        setup.sampleEnd = start + regionFrames;
        setup.loopStart = start + 16u;
        setup.loopEnd = start + regionFrames - 16u;
        setup.loopMode = 1u;
        setup.phaseStep = 0.5f +
            static_cast<float>(identity % 97u) / 64.0f;
        setup.basePhaseStep = setup.phaseStep;
        setup.initialGain = 1.0f;
        setup.attackSamples = attackFrames;
        setup.attackGainStep = attackFrames > 0u
            ? setup.initialGain / static_cast<float>(attackFrames) : 0.0f;
        setup.sustainLevel = 0.7f;
        setup.releaseDecay = 0.9999999f;
        setup.gainLeft = setup.gainRight = 0.001f;
        setup.presetIndex = 0u;
        setup.regionIndex = static_cast<uint16_t>(identity & 0xffffu);
    }
}

void PerformSteals(svms::VoiceManager& voices, const svms::ChannelCache& channels,
                   uint32_t& sequence, uint32_t count, uint32_t sampleFrames,
                   const svms::RenderEvent* sourceEvent = nullptr,
                   uint32_t attackFrames = 0u,
                   bool transactional = false,
                   bool copiedLaunchPlan = false,
                   uint32_t profileScale = 0u,
                   const svms::VoiceConfiguration* preparedSetups = nullptr) {
    constexpr uint32_t regionFrames = 2048;
    const uint32_t regionCount = sampleFrames / regionFrames;
    // Every physical SF2 region produced by one MIDI note belongs to one
    // playIndex. This makes the synthetic layered benchmark exercise the
    // same atomic stereo-stealing path as the production driver.
    const uint32_t playIndex = sequence + 1u;
    if (transactional && count <= 8u) {
        const uint64_t prepareBegin =
            profileScale != 0u && !preparedSetups ? __rdtsc() : 0u;
        const uint8_t channel = sourceEvent ? sourceEvent->channel
            : static_cast<uint8_t>(sequence & 15u);
        const uint8_t note = sourceEvent ? sourceEvent->data1
            : static_cast<uint8_t>(24u + sequence % 88u);
        const uint8_t velocity = sourceEvent ? sourceEvent->data2
            : static_cast<uint8_t>(64u + sequence % 64u);
        svms::VoiceConfiguration localSetups[8]{};
        svms::VoiceHandle handles[8]{};
        const svms::VoiceConfiguration* setups = preparedSetups;
        if (!setups) {
            PrepareSyntheticLaunchPlan(localSetups, count, sampleFrames, note,
                                       attackFrames);
            setups = localSetups;
        }
        if (prepareBegin != 0u)
            gLaunchPrepareCycles += ScaledProfileDelta(
                prepareBegin, __rdtsc(), profileScale);
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

struct BenchLaunchPlanEntry {
    uint64_t tag = UINT64_MAX;
    uint8_t count = 0u;
    svms::VoiceConfiguration setups[8]{};
};

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
    bool batchNoteOffIndex;
    BenchLaunchPlanEntry* launchPlanCache;
};

void MixedDispatch(const svms::RenderEvent& event, uint32_t blockCursor,
                   void* userData) {
    const uint32_t sampleSlot = static_cast<uint32_t>(
        gBreakdownSampleCounter++ & (kBreakdownSampleInterval - 1u));
    const uint32_t dispatchScale = gCollectBreakdown && sampleSlot == 0u
        ? kBreakdownSampleInterval : 0u;
    const uint32_t regionScale = gCollectBreakdown && sampleSlot == 1u
        ? kBreakdownSampleInterval : 0u;
    const uint32_t stealScale = gCollectBreakdown && sampleSlot == 2u
        ? kBreakdownSampleInterval : 0u;
    const uint64_t dispatchBegin =
        dispatchScale != 0u ? __rdtsc() : 0u;
    auto* context = static_cast<MixedDispatchContext*>(userData);
    auto& voices = *context->voices;
    auto& channels = *context->channels;
    switch (event.type) {
        case svms::RenderEventType::NoteOn: {
            const uint64_t resolveBegin =
                regionScale != 0u ? __rdtsc() : 0u;
            uint32_t layerCount = 1u;
            uint32_t planHash = context->presetIndex * 0x9e3779b9u;
            planHash ^= static_cast<uint32_t>(event.data1) * 0x85ebca6bu;
            planHash ^= static_cast<uint32_t>(event.data2) * 0xc2b2ae35u;
            planHash ^= static_cast<uint32_t>(event.channel) * 0x27d4eb2fu;
            planHash ^= planHash >> 16u;
            const uint64_t planTag =
                (static_cast<uint64_t>(context->presetIndex) << 19u) |
                (static_cast<uint64_t>(event.channel) << 15u) |
                (static_cast<uint64_t>(event.data1) << 8u) | event.data2;
            auto& plan = context->launchPlanCache[planHash & 4095u];
            const bool planHit = plan.tag == planTag && plan.count != 0u;
            if (planHit) {
                layerCount = plan.count;
            } else {
                if (context->soundFont) {
                    const svms::SFSampleRegion* regions[512];
                    layerCount = svms::sf2_find_regions(
                        context->soundFont, context->presetIndex, event.data1,
                        event.data2, regions, 512u);
                }
                if (layerCount == 0u || layerCount > 8u) break;
                const uint64_t prepareBegin =
                    regionScale != 0u ? __rdtsc() : 0u;
                PrepareSyntheticLaunchPlan(plan.setups, layerCount,
                                           context->sampleFrames, event.data1,
                                           context->attackFrames);
                if (regionScale != 0u)
                    gLaunchPrepareCycles += ScaledProfileDelta(
                        prepareBegin, __rdtsc(), regionScale);
                plan.tag = planTag;
                plan.count = static_cast<uint8_t>(layerCount);
            }
            gMatchedRegions += layerCount;
            if (regionScale != 0u)
                gRegionResolveCycles += ScaledProfileDelta(
                    resolveBegin, __rdtsc(), regionScale);
            if (stealScale != 0u) {
                const uint64_t begin = __rdtsc();
                PerformSteals(voices, channels, context->sequence, layerCount,
                              context->sampleFrames, &event,
                              context->attackFrames,
                              context->transactionalLaunch,
                              context->copiedLaunchPlan, 0u,
                              plan.setups);
                const uint64_t end = __rdtsc();
                gStealCycles += ScaledProfileDelta(
                    begin, end, stealScale);
                break;
            }
            PerformSteals(voices, channels, context->sequence, layerCount,
                          context->sampleFrames, &event,
                          context->attackFrames,
                          context->transactionalLaunch,
                          context->copiedLaunchPlan, 0u, plan.setups);
            break;
        }
        case svms::RenderEventType::NoteOff: {
            voices.NoteOffOldestPlayIndices(
                event.channel, event.data1, 1u, false, blockCursor);
            break;
        }
        case svms::RenderEventType::ControlChange:
            channels.ControlChange(event.channel, event.data1, event.data2);
            channels.RebuildCache(*context->config, 44100.0f);
            if (event.data1 == 7u || event.data1 == 10u || event.data1 == 11u)
                voices.MarkChannelMixStale(
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
    if (dispatchScale != 0u) {
        const uint64_t dispatchEnd = __rdtsc();
        gDispatchCycles += ScaledProfileDelta(
            dispatchBegin, dispatchEnd, dispatchScale);
    }
}

void MixedBatchDispatch(const svms::RenderEvent* events, uint32_t eventCount,
                        uint32_t blockCursor, void* userData) {
    auto* context = static_cast<MixedDispatchContext*>(userData);
    uint32_t index = 0u;
    while (index < eventCount) {
        if (events[index].type == svms::RenderEventType::NoteOff ||
            events[index].type == svms::RenderEventType::StaleNoteOffBatch) {
            if (context->batchNoteOffIndex)
                context->voices->InvalidateStealCandidates();
            do {
                MixedDispatch(events[index++], blockCursor, userData);
            } while (index < eventCount &&
                     (events[index].type == svms::RenderEventType::NoteOff ||
                      events[index].type ==
                          svms::RenderEventType::StaleNoteOffBatch));
            continue;
        }
        MixedDispatch(events[index++], blockCursor, userData);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::fprintf(stderr,
            "usage: svms_v3_bench [--voices 1..524288] [--frames 16..8192] "
            "[--seconds N] [--warmup N] [--workload sustained|envelope|release|steal|dense|mixed-events|note-burst|chopped-notes] "
            "[--event-stride N] [--note-rate N] [--key-count 1..128] [--base-note 0..127] [--key-stride 1..127] [--attack-frames N] [--note-length-frames N] [--generic-voices N] [--soundfont PATH] "
            "[--render-threads 1..64] "
            "[--backend auto|scalar|sse2|avx2] "
            "[--launch-path legacy|transactional] "
            "[--launch-plan direct|copy] "
            "[--noteoff-index immediate|batch] "
            "[--breakdown] [--coverage] [--launch-churn] [--volatile-selection heap|scan] "
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
    gRdtscPairOverhead = UINT64_MAX;
    for (uint32_t sample = 0u; sample < 4096u; ++sample) {
        const uint64_t begin = __rdtsc();
        const uint64_t end = __rdtsc();
        gRdtscPairOverhead = (std::min)(gRdtscPairOverhead, end - begin);
    }

    constexpr uint32_t sampleFrames = 64u * 2048u;
    std::vector<int16_t> samples(sampleFrames + 8u, 0);
    for (uint32_t i = 0; i < sampleFrames; ++i) {
        samples[i] = static_cast<int16_t>((0.45f * std::sin(static_cast<float>(i) * 0.017f) +
                      0.2f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
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
                         options.workload, sampleFrames,
                         options.genericVoices)) {
        std::fprintf(stderr, "cannot allocate voice storage\n");
        return 3;
    }
    if (options.genericVoices != 0u) {
        std::fprintf(stderr, "[diag] after configure: active=%u generic=%u rloop=%u\n",
                     voices->GetActiveCount(),
                     voices->GetRenderClassCount(svms::VoiceRenderClass::Generic),
                     voices->GetRenderClassCount(svms::VoiceRenderClass::ReleaseLoop));
    }
    voices->SetLaunchChurnProfilingEnabledForTest(
        options.launchChurnProfile);
    voices->SetVolatileFallbackScanForTest(options.volatileFallbackScan);
    auto renderer = std::make_unique<svms::RenderScalar>();
    if (!renderer->ReserveVoiceCapacity(options.voices)) {
        std::fprintf(stderr, "cannot allocate renderer scratch\n");
        return 3;
    }
    if (!renderer->ConfigureRenderThreads(options.renderThreads,
                                          options.frames)) {
        std::fprintf(stderr, "cannot initialize render workers\n");
        return 3;
    }
    renderer->SetCoverageProfilingEnabledForTest(options.coverageProfile);
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
    auto launchPlanCache =
        std::make_unique<BenchLaunchPlanEntry[]>(4096u);
    MixedDispatchContext mixedContext{
        voices.get(), &channels, &cfg, sampleFrames, options.voices,
        soundFont.get(), soundFontPreset, options.attackFrames,
        options.transactionalLaunch, options.copiedLaunchPlan,
        options.batchNoteOffIndex,
        launchPlanCache.get()};
    if (options.workload == Workload::Dense ||
        options.workload == Workload::MixedEvents ||
        options.workload == Workload::NoteBurst ||
        options.workload == Workload::ChoppedNotes) {
        const bool noteStorm = options.workload == Workload::NoteBurst ||
            options.workload == Workload::ChoppedNotes;
        const uint32_t noteOnCount = noteStorm
            ? static_cast<uint32_t>((static_cast<uint64_t>(options.noteRate) *
                                     options.frames + 44099u) / 44100u)
            : (options.frames + options.eventStride - 1u) / options.eventStride;
        const uint32_t eventCount = options.workload == Workload::ChoppedNotes
            ? noteOnCount * 2u : noteOnCount;
        events.resize(eventCount);
        if (options.workload == Workload::ChoppedNotes) {
            for (uint32_t noteIndex = 0u; noteIndex < noteOnCount;
                 ++noteIndex) {
                const uint32_t frame = static_cast<uint32_t>(
                    static_cast<uint64_t>(noteIndex) * options.frames /
                    noteOnCount);
                const uint8_t channel = static_cast<uint8_t>(noteIndex & 15u);
                const uint8_t note = static_cast<uint8_t>(
                    (options.baseNote +
                     (noteIndex % options.keyCount) * options.keyStride) &
                    127u);
                svms::RenderEvent& on = events[noteIndex * 2u];
                on.frameOffset = frame;
                on.ingressSequence = noteIndex * 2u + 1u;
                on.type = svms::RenderEventType::NoteOn;
                on.channel = channel;
                on.data1 = note;
                on.data2 = 127u;
                svms::RenderEvent& off = events[noteIndex * 2u + 1u];
                off.frameOffset = (frame + options.noteLengthFrames) %
                    options.frames;
                off.ingressSequence = noteIndex * 2u;
                off.type = svms::RenderEventType::NoteOff;
                off.channel = channel;
                off.data1 = note;
            }
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    if (a.frameOffset != b.frameOffset)
                        return a.frameOffset < b.frameOffset;
                    if (a.type != b.type) {
                        return a.type == svms::RenderEventType::NoteOff;
                    }
                    return a.ingressSequence < b.ingressSequence;
                });
            for (uint32_t index = 0u; index < eventCount; ++index)
                events[index].ingressSequence = index;
        } else for (uint32_t index = 0; index < eventCount; ++index) {
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
                event.data1 = static_cast<uint8_t>(
                    (options.baseNote +
                     (index % options.keyCount) * options.keyStride) & 127u);
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
            gRenderCycles += ScaledProfileDelta(
                renderBegin, renderEnd, 1u);
        }
        absoluteFrame += options.frames;
    };

    for (uint32_t i = 0; i < warmupCallbacks; ++i) renderOne();

    gRenderCycles = gDispatchCycles = gStealCycles = gMatchedRegions = 0u;
    gRegionResolveCycles = gLaunchPrepareCycles = 0u;
    gBreakdownSampleCounter = 0u;
    voices->ResetGroupReuseCountersForTest();
    renderer->ResetCoverageStatsForTest();
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

    const svms::LaunchChurnStats& churn =
        voices->GetLaunchChurnStatsForTest();
    auto ratio = [](uint64_t numerator, uint64_t denominator) {
        return denominator != 0u
            ? 100.0 * static_cast<double>(numerator) /
                static_cast<double>(denominator)
            : 0.0;
    };
    std::string churnBuckets = "[";
    bool firstBucket = true;
    for (uint32_t bucket = 0u;
         bucket < svms::LaunchChurnStats::kClassificationBuckets;
         ++bucket) {
        const svms::LaunchChurnBucketStats& sample = churn.buckets[bucket];
        if (sample.samples == 0u) continue;
        char encoded[640]{};
        const double divisor = static_cast<double>(sample.samples);
        std::snprintf(encoded, sizeof(encoded),
            "%s{\"class\":%u,\"same_frame\":%s,\"layered\":%s,"
            "\"volatile\":%s,\"general_or_unreserved\":%s,\"no_steal\":%s,"
            "\"samples\":%llu,\"cycles_per_sample\":{\"total\":%.1f,"
            "\"victim_selection\":%.1f,\"tail_capture\":%.1f,"
            "\"lifecycle\":%.1f,\"configuration\":%.1f,"
            "\"tree_maintenance\":%.1f}}",
            firstBucket ? "" : ",", bucket,
            bucket < 16u && (bucket & 1u) != 0u ? "true" : "false",
            bucket < 16u && (bucket & 2u) != 0u ? "true" : "false",
            bucket < 16u && (bucket & 4u) != 0u ? "true" : "false",
            bucket < 16u && (bucket & 8u) != 0u ? "true" : "false",
            bucket == 16u ? "true" : "false",
            static_cast<unsigned long long>(sample.samples),
            static_cast<double>(sample.totalCycles) / divisor,
            static_cast<double>(sample.stageCycles[static_cast<uint32_t>(
                svms::LaunchProfileStage::VictimSelection)]) / divisor,
            static_cast<double>(sample.stageCycles[static_cast<uint32_t>(
                svms::LaunchProfileStage::TailCapture)]) / divisor,
            static_cast<double>(sample.stageCycles[static_cast<uint32_t>(
                svms::LaunchProfileStage::Lifecycle)]) / divisor,
            static_cast<double>(sample.stageCycles[static_cast<uint32_t>(
                svms::LaunchProfileStage::Configuration)]) / divisor,
            static_cast<double>(sample.stageCycles[static_cast<uint32_t>(
                svms::LaunchProfileStage::TreeMaintenance)]) / divisor);
        churnBuckets += encoded;
        firstBucket = false;
    }
    churnBuckets += "]";

    const svms::RenderCoverageStats& coverage =
        renderer->GetCoverageStatsForTest();
    char coverageHead[1200]{};
    std::snprintf(coverageHead, sizeof(coverageHead),
        "{\"enabled\":%s,\"callbacks\":%llu,\"dense_rendered\":%llu,"
        "\"dense_execution_fallbacks\":%llu,\"dense_rejected\":{"
        "\"correctness\":%llu,\"missing_events\":%llu,"
        "\"event_density\":%llu,\"workers\":%llu,\"storage\":%llu,"
        "\"voice_capacity\":%llu,\"shadow_capacity\":%llu,"
        "\"mutation_capacity\":%llu},\"spans\":%llu,"
        "\"sparse_voice_samples\":%llu,"
        "\"sustained_parallel_voice_samples\":%llu,"
        "\"sustained_rejected_voice_samples\":{"
        "\"unavailable\":%llu,\"frames\":%llu,\"voices\":%llu,"
        "\"product\":%llu},\"span_counts\":[",
        options.coverageProfile ? "true" : "false",
        static_cast<unsigned long long>(coverage.callbacks),
        static_cast<unsigned long long>(coverage.denseRendered),
        static_cast<unsigned long long>(coverage.denseExecutionFallbacks),
        static_cast<unsigned long long>(coverage.denseRejected[0]),
        static_cast<unsigned long long>(coverage.denseRejected[1]),
        static_cast<unsigned long long>(coverage.denseRejected[2]),
        static_cast<unsigned long long>(coverage.denseRejected[3]),
        static_cast<unsigned long long>(coverage.denseRejected[4]),
        static_cast<unsigned long long>(coverage.denseRejected[5]),
        static_cast<unsigned long long>(coverage.denseRejected[6]),
        static_cast<unsigned long long>(coverage.denseRejected[7]),
        static_cast<unsigned long long>(coverage.spans),
        static_cast<unsigned long long>(coverage.sparseVoiceSamples),
        static_cast<unsigned long long>(coverage.sustainedParallelVoiceSamples),
        static_cast<unsigned long long>(coverage.sustainedRejectedVoiceSamples[1]),
        static_cast<unsigned long long>(coverage.sustainedRejectedVoiceSamples[2]),
        static_cast<unsigned long long>(coverage.sustainedRejectedVoiceSamples[3]),
        static_cast<unsigned long long>(coverage.sustainedRejectedVoiceSamples[4]));
    std::string coverageJson = coverageHead;
    auto appendCoverageArray = [&](const uint64_t* values, uint32_t count) {
        char number[48]{};
        for (uint32_t index = 0u; index < count; ++index) {
            std::snprintf(number, sizeof(number), "%s%llu",
                index == 0u ? "" : ",",
                static_cast<unsigned long long>(values[index]));
            coverageJson += number;
        }
    };
    appendCoverageArray(coverage.spanCounts,
                        svms::RenderCoverageStats::kSpanBuckets);
    coverageJson += "],\"span_voice_samples\":[";
    appendCoverageArray(coverage.spanVoiceSamples,
                        svms::RenderCoverageStats::kSpanBuckets);
    coverageJson += "]}";

    std::printf(
        "{\"renderer\":\"%s\",\"backend\":\"%s\",\"render_threads\":%u,\"workload\":\"%s\",\"launch_path\":\"%s\",\"launch_plan\":\"%s\",\"launch_churn_profile\":%s,\"volatile_selection\":\"%s\",\"voices\":%u,\"frames\":%u,"
        "\"callbacks\":%u,\"event_stride\":%u,\"note_rate\":%u,\"key_count\":%u,\"base_note\":%u,\"key_stride\":%u,\"attack_frames\":%u,\"note_length_frames\":%u,\"noteoff_index\":\"%s\","
        "\"soundfont_regions\":%u,\"preset_regions\":%u,\"pinned_core\":%d,"
        "\"voice_soa_bytes\":%zu,\"voice_manager_bytes\":%zu,\"renderer_bytes\":%zu,"
        "\"estimated_voice_manager_bytes\":%zu,\"estimated_renderer_bytes\":%zu,"
        "\"voice_samples_per_second\":%.0f,"
        "\"cycles_per_voice_sample\":%.3f,\"events_per_second\":%.0f,"
        "\"steals_per_second\":%.0f,\"matched_regions\":%llu,\"max_consecutive_overruns\":%u,"
        "\"cycle_breakdown\":{\"total\":%llu,\"synthesis\":%llu,\"event_dispatch\":%llu,"
        "\"region_resolution\":%llu,\"launch_preparation\":%llu,\"index_and_steal\":%llu},"
        "\"group_reuse\":{\"attempts\":%llu,\"matches\":%llu,\"reserved\":%llu,"
        "\"smaller\":%llu,\"larger\":%llu,\"grouped_voices\":%u},"
        "\"launch_profile\":{\"samples\":%llu,\"pop\":%llu,\"tail\":%llu,"
        "\"volatile_heap_profile\":{\"builds\":%llu,\"samples\":%llu,"
        "\"cycles\":%llu,\"candidates\":%llu},"
        "\"lifecycle\":%llu,\"configure\":%llu,\"tree\":%llu},"
        "\"launch_churn\":{\"logical_launches\":%llu,\"successful_launches\":%llu,"
        "\"failed_launches\":%llu,\"physical_requested\":%llu,"
        "\"physical_configured\":%llu,\"free_slot_allocations\":%llu,"
        "\"steal_transactions\":%llu,\"victim_groups\":%llu,"
        "\"physical_victims\":%llu,\"same_frame_victim_groups\":%llu,"
        "\"same_frame_physical_victims\":%llu,\"mono_groups\":%llu,"
        "\"layered_groups\":%llu,\"matching_size_groups\":%llu,"
        "\"mismatched_size_groups\":%llu,\"stable_groups\":%llu,"
        "\"volatile_groups\":%llu,\"same_channel_key_groups\":%llu,"
        "\"matching_plan_groups\":%llu,\"single_in_place_groups\":%llu,"
        "\"matching_reuse_groups\":%llu,\"reserved_reuse_groups\":%llu,"
        "\"general_groups\":%llu,\"next_frame_surviving_groups\":%llu,"
        "\"next_frame_surviving_physical\":%llu,\"tail_attempts\":%llu,"
        "\"tail_accepted\":%llu,\"tail_replaced\":%llu,"
        "\"tail_rejected\":%llu,\"tail_ineligible\":%llu,"
        "\"ratios_percent\":{\"steal_transactions_per_launch\":%.3f,"
        "\"same_frame_groups_per_launch\":%.3f,"
        "\"same_frame_groups_per_victim_group\":%.3f,"
        "\"same_frame_physical_per_configured\":%.3f,"
        "\"next_frame_physical_per_configured\":%.3f},"
        "\"cycle_classes\":%s},"
        "\"coverage\":%s,"
        "\"render_classes\":{\"sustained_loop\":%u,\"sustained_one_shot\":%u,"
        "\"transient_loop\":%u,\"release_loop\":%u,\"release_one_shot\":%u,"
        "\"generic\":%u,\"steal_tails\":%u},"
        "\"callback_percent\":{\"p50\":%.2f,\"p95\":%.2f,"
        "\"p99\":%.2f,\"p99_9\":%.2f,\"max\":%.2f}}\n",
        options.reference ? "reference" : "span", renderer->GetRenderBackendName(),
        renderer->GetRenderThreadCount(),
        WorkloadName(options.workload),
        options.transactionalLaunch ? "transactional" : "legacy",
        options.copiedLaunchPlan ? "copy" : "direct",
        options.launchChurnProfile ? "true" : "false",
        options.volatileFallbackScan ? "scan" : "heap",
        options.voices, options.frames,
        measuredCallbacks, options.eventStride, options.noteRate, options.keyCount,
        options.baseNote, options.keyStride,
        options.attackFrames, options.noteLengthFrames,
        options.batchNoteOffIndex ? "batch" : "immediate",
        soundFont ? soundFont->regionCount : 0u,
        soundFont ? soundFont->presetRegionCount[soundFontPreset] : 0u,
        options.pinCore == UINT32_MAX ? -1 : static_cast<int>(options.pinCore),
        voices->v.GetAllocatedBytes(), voices->GetAllocatedBytes(),
        renderer->GetAllocatedBytes(),
        svms::VoiceManager::EstimateAllocatedBytes(options.voices),
        svms::RenderScalar::EstimateAllocatedBytes(
            options.voices, renderer->GetRenderThreadCount(), options.frames),
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
        static_cast<unsigned long long>(voices->GetLaunchProfileSamplesForTest()),
        static_cast<unsigned long long>(voices->GetLaunchProfilePopCyclesForTest()),
        static_cast<unsigned long long>(voices->GetLaunchProfileTailCyclesForTest()),
        static_cast<unsigned long long>(voices->GetVolatileHeapProfileBuildsForTest()),
        static_cast<unsigned long long>(voices->GetVolatileHeapProfileSamplesForTest()),
        static_cast<unsigned long long>(voices->GetVolatileHeapProfileCyclesForTest()),
        static_cast<unsigned long long>(voices->GetVolatileHeapProfileCandidatesForTest()),
        static_cast<unsigned long long>(voices->GetLaunchProfileLifecycleCyclesForTest()),
        static_cast<unsigned long long>(voices->GetLaunchProfileConfigureCyclesForTest()),
        static_cast<unsigned long long>(voices->GetLaunchProfileTreeCyclesForTest()),
        static_cast<unsigned long long>(churn.logicalLaunches),
        static_cast<unsigned long long>(churn.successfulLaunches),
        static_cast<unsigned long long>(churn.failedLaunches),
        static_cast<unsigned long long>(churn.physicalVoicesRequested),
        static_cast<unsigned long long>(churn.physicalVoicesConfigured),
        static_cast<unsigned long long>(churn.freeSlotAllocations),
        static_cast<unsigned long long>(churn.stealTransactions),
        static_cast<unsigned long long>(churn.victimGroups),
        static_cast<unsigned long long>(churn.physicalVictims),
        static_cast<unsigned long long>(churn.sameFrameVictimGroups),
        static_cast<unsigned long long>(churn.sameFramePhysicalVictims),
        static_cast<unsigned long long>(churn.monoVictimGroups),
        static_cast<unsigned long long>(churn.layeredVictimGroups),
        static_cast<unsigned long long>(churn.matchingSizeVictimGroups),
        static_cast<unsigned long long>(churn.mismatchedSizeVictimGroups),
        static_cast<unsigned long long>(churn.stableVictimGroups),
        static_cast<unsigned long long>(churn.volatileVictimGroups),
        static_cast<unsigned long long>(churn.sameChannelKeyVictimGroups),
        static_cast<unsigned long long>(churn.matchingPlanVictimGroups),
        static_cast<unsigned long long>(churn.singleInPlaceVictimGroups),
        static_cast<unsigned long long>(churn.matchingReuseVictimGroups),
        static_cast<unsigned long long>(churn.reservedReuseVictimGroups),
        static_cast<unsigned long long>(churn.generalVictimGroups),
        static_cast<unsigned long long>(churn.nextFrameSurvivingGroups),
        static_cast<unsigned long long>(churn.nextFrameSurvivingPhysicalVoices),
        static_cast<unsigned long long>(churn.tailCaptureAttempts),
        static_cast<unsigned long long>(churn.tailCaptureAccepted),
        static_cast<unsigned long long>(churn.tailCaptureReplaced),
        static_cast<unsigned long long>(churn.tailCaptureRejected),
        static_cast<unsigned long long>(churn.tailCaptureIneligible),
        ratio(churn.stealTransactions, churn.logicalLaunches),
        ratio(churn.sameFrameVictimGroups, churn.logicalLaunches),
        ratio(churn.sameFrameVictimGroups, churn.victimGroups),
        ratio(churn.sameFramePhysicalVictims,
              churn.physicalVoicesConfigured),
        ratio(churn.nextFrameSurvivingPhysicalVoices,
              churn.physicalVoicesConfigured),
        churnBuckets.c_str(),
        coverageJson.c_str(),
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
