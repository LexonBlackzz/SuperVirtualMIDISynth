#include "CanonicalCore.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace c = svms::canonical;

struct Result {
    double milliseconds{};
    double millionVoiceSamplesPerSecond{};
};

Result run(std::uint32_t voices, c::Backend backend,
           std::uint32_t workers, bool denseEvents) {
    constexpr std::uint32_t frames = 2048;
    c::SampleBank bank;
    std::vector<float> waveform(2048);
    for (std::size_t i = 0; i < waveform.size(); ++i)
        waveform[i] = 0.25F * std::sin(static_cast<float>(i) * 0.017F);
    const auto sample = bank.addLoop(waveform, 128, 2048);
    c::SynthConfig config{};
    config.voiceCapacity = voices;
    config.tileSize = 256;
    config.maxBlockFrames = frames;
    config.workerThreads = workers;
    config.backend = backend;
    config.workerDispatchMinimumFrames = 0;
    c::Synth synth(bank, config);

    std::vector<c::Event> launch;
    launch.reserve(voices);
    for (std::uint32_t i = 0; i < voices; ++i) {
        launch.push_back(c::Event::noteOn(0, i,
            static_cast<std::uint8_t>(i % 128), sample,
            0.3F + 0.01F * static_cast<float>(i % 53),
            0.0002F * static_cast<float>(1 + i % 11),
            0.0002F * static_cast<float>(1 + i % 13)));
    }
    std::vector<float> left(frames), right(frames);
    synth.render(0, launch, left, right);

    std::vector<c::Event> events;
    if (denseEvents) {
        events.reserve(frames);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            events.push_back(c::Event::channelControl(frame, frame,
                frame & 1 ? c::EventType::ChannelVolume
                          : c::EventType::ChannelExpression,
                static_cast<std::uint8_t>(frame % 16),
                static_cast<std::uint16_t>(32 + frame % 96)));
        }
    }
    const std::uint32_t iterations = voices <= 64 ? 50 : voices <= 1000 ? 12 : 3;
    std::uint64_t absoluteFrame = frames;
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
        if (denseEvents) {
            for (std::uint32_t frame = 0; frame < frames; ++frame)
                events[frame].frame = absoluteFrame + frame;
        }
        synth.render(absoluteFrame, events, left, right);
        absoluteFrame += frames;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    const double voiceSamples = static_cast<double>(voices) * frames * iterations;
    return {elapsed * 1000.0, voiceSamples / elapsed / 1.0e6};
}

int main() {
    std::cout << "backend,workers,voices,workload,ms,Mvoice-samples/s\n";
    for (const auto backend : {c::Backend::Scalar, c::Backend::Avx2}) {
        if (backend == c::Backend::Avx2 && !c::Synth::avx2Supported()) continue;
        for (const std::uint32_t workers : {0u, 3u}) {
            for (const std::uint32_t voices : {64u, 1000u, 10000u}) {
                const auto result = run(voices, backend, workers, false);
                std::cout << (backend == c::Backend::Scalar ? "scalar" : "avx2")
                          << ',' << workers << ',' << voices << ",sustained,"
                          << std::fixed << std::setprecision(3) << result.milliseconds
                          << ',' << result.millionVoiceSamplesPerSecond << '\n';
            }
            const auto dense = run(10000, backend, workers, true);
            std::cout << (backend == c::Backend::Scalar ? "scalar" : "avx2")
                      << ',' << workers << ",10000,dense-events,"
                      << std::fixed << std::setprecision(3) << dense.milliseconds
                      << ',' << dense.millionVoiceSamplesPerSecond << '\n';
        }
    }
}
