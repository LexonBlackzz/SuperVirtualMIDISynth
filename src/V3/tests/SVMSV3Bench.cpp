#include <cmath>
#include "SVMSRenderScalar.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

enum class Workload { Sustained, Envelope, Release, Steal, Dense };

const char* WorkloadName(Workload workload) {
    switch (workload) {
        case Workload::Sustained: return "sustained";
        case Workload::Envelope: return "envelope";
        case Workload::Release: return "release";
        case Workload::Steal: return "steal";
        case Workload::Dense: return "dense";
    }
    return "unknown";
}

bool ParseWorkload(const char* value, Workload& result) {
    if (std::strcmp(value, "sustained") == 0) result = Workload::Sustained;
    else if (std::strcmp(value, "envelope") == 0) result = Workload::Envelope;
    else if (std::strcmp(value, "release") == 0) result = Workload::Release;
    else if (std::strcmp(value, "steal") == 0) result = Workload::Steal;
    else if (std::strcmp(value, "dense") == 0) result = Workload::Dense;
    else return false;
    return true;
}

void NoopDispatch(const svms::RenderEvent&, uint32_t, void*) {}

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
};

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
        } else if (std::strcmp(argv[i], "--enforce") == 0) {
            options.enforce = true;
        } else if (std::strcmp(argv[i], "--reference") == 0) {
            options.reference = true;
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

void ConfigureVoices(svms::VoiceManager& voices, svms::ChannelCache& channels,
                     const svms::RuntimeConfigSnapshot& cfg,
                     uint32_t voiceCount, Workload workload,
                     uint32_t sampleFrames) {
    voices.Initialize(voiceCount, 44100);
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
}

void PerformSteals(svms::VoiceManager& voices, const svms::ChannelCache& channels,
                   uint32_t& sequence, uint32_t count, uint32_t sampleFrames) {
    constexpr uint32_t regionFrames = 2048;
    const uint32_t regionCount = sampleFrames / regionFrames;
    for (uint32_t i = 0; i < count; ++i, ++sequence) {
        const uint8_t channel = static_cast<uint8_t>(sequence & 15u);
        const uint8_t note = static_cast<uint8_t>(24u + sequence % 88u);
        const uint8_t velocity = static_cast<uint8_t>(64u + sequence % 64u);
        const svms::VoiceHandle handle = voices.AllocateVoiceOrSteal(
            channel, note, velocity);
        if (handle == svms::kInvalidVoice) continue;
        const uint32_t start = (sequence % regionCount) * regionFrames;
        voices.SetVoiceSample(handle, start, start + regionFrames,
                              start + 16u, start + regionFrames - 16u, 1u,
                              0.5f + static_cast<float>(sequence % 97u) / 64.0f, 1u);
        voices.SetVoiceEnvelope(handle, 1.0f, 0.7f, 0u, 0u, 0u, 0u,
                                0.0f, 1.0f, 0.9999999f);
        voices.SetVoiceGain(handle, 0.001f, 0.001f);
        voices.RefreshMixGain(handle, channels.GetParams()[channel]);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::fprintf(stderr,
            "usage: svms_v3_bench [--voices 1..4096] [--frames 16..8192] "
            "[--seconds N] [--warmup N] [--workload sustained|envelope|release|steal|dense] "
            "[--quick] [--reference] [--enforce]\n");
        return 1;
    }

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
    ConfigureVoices(*voices, channels, cfg, options.voices, options.workload, sampleFrames);
    auto renderer = std::make_unique<svms::RenderScalar>();

    std::vector<svms::RenderEvent> events;
    if (options.workload == Workload::Dense) {
        events.resize(options.frames);
        for (uint32_t frame = 0; frame < options.frames; ++frame) {
            events[frame].type = svms::RenderEventType::ControlChange;
            events[frame].frameOffset = frame;
            events[frame].ingressSequence = frame;
        }
        renderer->SetEventDispatcher(NoopDispatch, nullptr);
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
            PerformSteals(*voices, channels, stealSequence, 16u, sampleFrames);
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
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
        absoluteFrame += options.frames;
    };

    for (uint32_t i = 0; i < warmupCallbacks; ++i) renderOne();

    std::vector<double> callbackPercent;
    callbackPercent.reserve(measuredCallbacks);
    double elapsedTotal = 0.0;
    const double budgetSeconds = static_cast<double>(options.frames) / 44100.0;
    for (uint32_t i = 0; i < measuredCallbacks; ++i) {
        LARGE_INTEGER begin{}, end{};
        QueryPerformanceCounter(&begin);
        renderOne();
        QueryPerformanceCounter(&end);
        const double elapsed = static_cast<double>(end.QuadPart - begin.QuadPart) /
                               static_cast<double>(frequency.QuadPart);
        elapsedTotal += elapsed;
        callbackPercent.push_back(elapsed / budgetSeconds * 100.0);
    }

    const double p50 = Percentile(callbackPercent, 0.50);
    const double p95 = Percentile(callbackPercent, 0.95);
    const double p99 = Percentile(callbackPercent, 0.99);
    const double p999 = Percentile(callbackPercent, 0.999);
    const double maximum = *std::max_element(callbackPercent.begin(), callbackPercent.end());
    const double voiceSamples = static_cast<double>(options.voices) * options.frames *
                                measuredCallbacks;
    const double voiceSamplesPerSecond = voiceSamples / elapsedTotal;

    std::printf(
        "{\"renderer\":\"%s\",\"workload\":\"%s\",\"voices\":%u,\"frames\":%u,"
        "\"callbacks\":%u,\"voice_samples_per_second\":%.0f,"
        "\"callback_percent\":{\"p50\":%.2f,\"p95\":%.2f,"
        "\"p99\":%.2f,\"p99_9\":%.2f,\"max\":%.2f}}\n",
        options.reference ? "reference" : "span", WorkloadName(options.workload),
        options.voices, options.frames,
        measuredCallbacks, voiceSamplesPerSecond, p50, p95, p99, p999, maximum);

    if (options.enforce && options.voices == 4096u) {
        const double limit = options.workload == Workload::Sustained ? 60.0 : 70.0;
        if (p99 >= limit) return 2;
    }
    return 0;
}
