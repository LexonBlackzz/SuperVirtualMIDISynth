#include "midisynth/synth.h"
#include "midisynth/midi_file.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::vector<float> makeSample() {
    std::vector<float> result(1024);
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = std::sin(2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(result.size()));
    }
    return result;
}

struct Workload {
    std::string name;
    std::uint32_t capacity;
    std::uint32_t frames;
    std::vector<midisynth::Event> events;
};

Workload sustained(std::uint32_t voices, std::uint32_t frames = 4410) {
    Workload w{"sustained-" + std::to_string(voices), voices, frames, {}};
    w.events.reserve(voices);
    for (std::uint32_t i = 0; i < voices; ++i) {
        w.events.push_back(midisynth::Event::noteOn(0, i,
            static_cast<std::uint8_t>(i % 128), 0, 0.25F + (i % 17) * 0.01F,
            0.001F, 0.001F));
    }
    return w;
}

Workload chopped(std::uint32_t voices) {
    constexpr std::uint32_t cycles = 64;
    Workload w{"chopped-" + std::to_string(voices), voices, cycles * 8, {}};
    w.events.reserve(static_cast<std::size_t>(voices) * cycles * 2);
    std::uint32_t sequence = 0;
    for (std::uint32_t cycle = 0; cycle < cycles; ++cycle) {
        for (std::uint32_t i = 0; i < voices; ++i) {
            w.events.push_back(midisynth::Event::noteOn(cycle * 8, sequence++,
                static_cast<std::uint8_t>(i % 128), 0, 1.0F, 0.001F, 0.001F));
        }
        for (std::uint32_t i = 0; i < voices; ++i) {
            w.events.push_back(midisynth::Event::noteOff(cycle * 8 + 4, sequence++,
                static_cast<std::uint8_t>(i % 128)));
        }
    }
    return w;
}

Workload sameFrameNoteOns(std::uint32_t voices) {
    auto w = sustained(voices, 64);
    w.name = "same-frame-" + std::to_string(voices);
    return w;
}

Workload fullEnvelope(std::uint32_t voices, bool heterogeneous) {
    Workload w{heterogeneous ? "envelope-heterogeneous" : "envelope-full",
               voices, 4410, {}};
    w.events.reserve(voices);
    for (std::uint32_t i=0;i<voices;++i) {
        auto event=midisynth::Event::noteOn(0,i,static_cast<std::uint8_t>(i%128),0,
            0.25F+(i%17)*0.01F,0.001F,0.001F,heterogeneous?i%97:64,256);
        event.delayFrames=heterogeneous?i%89:32;
        event.holdFrames=heterogeneous?i%83:64;
        event.decayFrames=heterogeneous?64+i%211:256;
        event.sustainGain=0.2F+(i%7)*0.1F;
        event.logarithmicEnvelope=true;
        w.events.push_back(event);
    }
    return w;
}

Workload controllerActivity(std::uint32_t voices) {
    auto w=sustained(voices,512);
    w.name="channel-controllers-"+std::to_string(voices);
    std::uint32_t sequence=voices;
    for(std::uint32_t frame=1;frame<512;++frame) {
        const auto type=frame%3==0?midisynth::EventType::ChannelVolume:
            frame%3==1?midisynth::EventType::ChannelExpression:
                       midisynth::EventType::ChannelPan;
        w.events.push_back(midisynth::Event::channelControl(frame,sequence++,type,0,
            static_cast<std::uint16_t>(frame%128)));
    }
    std::sort(w.events.begin(),w.events.end(),[](const auto& a,const auto& b){
        return a.frame<b.frame||(a.frame==b.frame&&a.sequence<b.sequence);
    });
    return w;
}

Workload irrelevantEventBoundaries(std::uint32_t voices) {
    constexpr std::uint32_t frames = 512;
    Workload w{"irrelevant-" + std::to_string(voices), voices, frames, {}};
    w.events.reserve(voices + frames - 1);
    std::uint32_t sequence = 0;
    for (std::uint32_t i = 0; i < voices; ++i) {
        w.events.push_back(midisynth::Event::noteOn(0, sequence++, 0, 0,
            0.5F, 0.001F, 0.001F));
    }
    for (std::uint32_t frame = 1; frame < frames; ++frame) {
        w.events.push_back(midisynth::Event::noteOff(frame, sequence++, 1));
    }
    return w;
}

Workload uniqueEventFrames(std::uint32_t backgroundVoices) {
    constexpr std::uint32_t frames = 512;
    Workload w{"unique-events-" + std::to_string(backgroundVoices),
               backgroundVoices + 1, frames, {}};
    w.events.reserve(backgroundVoices + frames - 1);
    std::uint32_t sequence = 0;
    for (std::uint32_t i = 0; i < backgroundVoices; ++i) {
        w.events.push_back(midisynth::Event::noteOn(0, sequence++, 0, 0,
            0.5F, 0.001F, 0.001F));
    }
    for (std::uint32_t frame = 1; frame < frames; ++frame) {
        if ((frame & 1U) != 0) {
            w.events.push_back(midisynth::Event::noteOn(frame, sequence++, 1, 0,
                1.0F, 0.001F, 0.001F, 0, 0));
        } else {
            w.events.push_back(midisynth::Event::noteOff(frame, sequence++, 1));
        }
    }
    return w;
}

struct Options {
    std::uint32_t runs{5};
    std::uint32_t warmups{1};
    std::string suite{"core"};
    bool avx2FastAdvance{true};
    bool avx2ContiguousLoads{true};
    bool detailed{false};
    std::uint32_t workerMinimumFrames{8};
    std::string sf2;
    std::string midi;
};

std::uint32_t parsePositive(std::string_view text, const char* option) {
    const auto value = std::strtoul(std::string(text).c_str(), nullptr, 10);
    if (value == 0 || value > 1000) {
        throw std::invalid_argument(std::string(option) + " must be in [1, 1000]");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t parseNonnegative(std::string_view text, const char* option) {
    const auto value = std::strtoul(std::string(text).c_str(), nullptr, 10);
    if (value > 1000000) {
        throw std::invalid_argument(std::string(option) + " must be in [0, 1000000]");
    }
    return static_cast<std::uint32_t>(value);
}

Options parseOptions(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if ((arg == "--runs" || arg == "--warmups" || arg == "--suite" ||
             arg == "--worker-min-frames" || arg == "--sf2" || arg == "--midi") && i + 1 >= argc) {
            throw std::invalid_argument(std::string(arg) + " requires a value");
        }
        if (arg == "--runs") result.runs = parsePositive(argv[++i], "--runs");
        else if (arg == "--warmups") result.warmups = parsePositive(argv[++i], "--warmups");
        else if (arg == "--suite") result.suite = argv[++i];
        else if (arg == "--sf2") result.sf2 = argv[++i];
        else if (arg == "--midi") result.midi = argv[++i];
        else if (arg == "--disable-avx2-fast-advance") result.avx2FastAdvance = false;
        else if (arg == "--disable-avx2-contiguous-loads") result.avx2ContiguousLoads = false;
        else if (arg == "--detailed") result.detailed = true;
        else if (arg == "--worker-min-frames") {
            result.workerMinimumFrames = parseNonnegative(argv[++i], "--worker-min-frames");
        }
        else if (arg == "--help") {
            std::cout << "midisynth_bench [--runs N] [--warmups N] "
                         "[--suite core|workers|tiles|diagnostic|real|all] "
                         "[--disable-avx2-fast-advance] "
                         "[--disable-avx2-contiguous-loads] [--detailed] "
                         "[--worker-min-frames N] [--sf2 file.sf2 --midi file.mid]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    if (result.suite != "core" && result.suite != "workers" &&
        result.suite != "tiles" && result.suite != "diagnostic" &&
        result.suite != "all" && result.suite != "real") {
        throw std::invalid_argument("unknown suite: " + result.suite);
    }
    if (result.sf2.empty() != result.midi.empty()) {
        throw std::invalid_argument("--sf2 and --midi must be supplied together");
    }
    return result;
}

struct Measurement {
    double wallMs{};
    double eventMs{};
    double synthesisMs{};
    double tileMs{};
    double reductionMs{};
    std::uint64_t events{};
    std::uint64_t eventFrames{};
    std::uint32_t highWater{};
};

struct Distribution {
    double minimum{};
    double median{};
    double mean{};
    double maximum{};
};

Distribution summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    const double median = values.size() % 2 != 0
        ? values[middle] : (values[middle - 1] + values[middle]) * 0.5;
    return {values.front(), median,
            std::accumulate(values.begin(), values.end(), 0.0) / values.size(),
            values.back()};
}

Measurement measureOnce(const midisynth::SampleBank& bank, const Workload& workload,
                        midisynth::Backend backend, std::uint32_t workers,
                        std::uint32_t tileSize, const Options& options) {
    midisynth::SynthConfig config{.voiceCapacity = workload.capacity,
        .tileSize = tileSize, .maxBlockFrames = workload.frames,
        .workerThreads = workers, .backend = backend};
    config.enableAvx2FastAdvance = options.avx2FastAdvance;
    config.enableAvx2ContiguousLoads = options.avx2ContiguousLoads;
    config.collectDetailedTiming = options.detailed;
    config.workerDispatchMinimumFrames = options.workerMinimumFrames;
    midisynth::Synth synth(bank, config);
    std::vector<float> left(workload.frames), right(workload.frames);

    for (std::uint32_t i = 0; i < options.warmups; ++i) {
        synth.render(0, workload.events, left, right);
        synth.reset();
    }

    const auto wallStart = Clock::now();
    synth.render(0, workload.events, left, right);
    const auto wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - wallStart).count();
    const auto& stats = synth.stats();
    return {wallNs * 1.0e-6, stats.eventNanoseconds * 1.0e-6,
            stats.synthesisNanoseconds * 1.0e-6, stats.tileNanoseconds * 1.0e-6,
            stats.reductionNanoseconds * 1.0e-6, stats.eventsDispatched,
            stats.uniqueEventFrames, stats.activeVoiceHighWater};
}

void printDistribution(const Distribution& d) {
    std::cout << std::fixed << std::setprecision(3)
              << d.minimum << '/' << d.median << '/' << d.mean << '/' << d.maximum;
}

void run(const midisynth::SampleBank& bank, const Workload& workload,
         midisynth::Backend backend, std::uint32_t workers, std::uint32_t tileSize,
         const Options& options) {
    std::vector<Measurement> measurements;
    measurements.reserve(options.runs);
    for (std::uint32_t i = 0; i < options.runs; ++i) {
        measurements.push_back(measureOnce(bank, workload, backend, workers, tileSize, options));
    }
    std::vector<double> wall, event, synthesis, tile, reduction;
    wall.reserve(measurements.size());
    event.reserve(measurements.size());
    synthesis.reserve(measurements.size());
    tile.reserve(measurements.size());
    reduction.reserve(measurements.size());
    for (const auto& measurement : measurements) {
        wall.push_back(measurement.wallMs);
        event.push_back(measurement.eventMs);
        synthesis.push_back(measurement.synthesisMs);
        tile.push_back(measurement.tileMs);
        reduction.push_back(measurement.reductionMs);
    }
    const auto wallStats = summarize(wall);
    const auto eventStats = summarize(event);
    const auto synthesisStats = summarize(synthesis);
    const auto tileStats = summarize(tile);
    const auto reductionStats = summarize(reduction);
    const auto& representative = measurements.front();
    const double medianSeconds = wallStats.median * 1.0e-3;
    const double medianEventSeconds = eventStats.median * 1.0e-3;

    std::cout << std::left << std::setw(22) << workload.name
              << std::setw(8) << (backend == midisynth::Backend::Scalar ? "scalar" : "avx2")
              << std::right << std::setw(4) << workers
              << std::setw(6) << tileSize
              << std::setw(9) << representative.highWater
              << std::setw(12) << std::fixed << std::setprecision(1)
              << (medianEventSeconds > 0 ? representative.events / medianEventSeconds : 0.0)
              << std::setw(12)
              << (medianEventSeconds > 0 ? representative.eventFrames / medianEventSeconds : 0.0)
              << std::setw(12) << (workload.frames / medianSeconds)
              << std::setw(9) << (workload.frames / 44100.0 / medianSeconds) << "  ";
    printDistribution(wallStats);
    std::cout << "  ";
    printDistribution(eventStats);
    std::cout << "  ";
    printDistribution(synthesisStats);
    if (options.detailed) {
        std::cout << "  ";
        printDistribution(tileStats);
        std::cout << "  ";
        printDistribution(reductionStats);
    }
    std::cout << '\n';
}

void printHeader(const Options& options) {
    std::cout << "runs=" << options.runs << " warmups=" << options.warmups
              << " sample_rate=44100 times=min/median/mean/max ms\n"
              << std::left << std::setw(22) << "workload" << std::setw(8) << "backend"
              << std::right << std::setw(4) << "wrk" << std::setw(6) << "tile"
              << std::setw(9) << "hiwater" << std::setw(12) << "events/s"
              << std::setw(12) << "evtfrm/s" << std::setw(12) << "samples/s"
              << std::setw(9) << "RT" << "  wall min/median/mean/max ms"
              << "  EVT min/median/mean/max ms  SYN min/median/mean/max ms\n";
    if (options.detailed) {
        std::cout << "Detailed columns after SYN: tile execution/dispatch, deterministic reduction.\n";
    }
}

std::vector<midisynth::Backend> availableBackends() {
    std::vector result{midisynth::Backend::Scalar};
    if (midisynth::Synth::avx2Supported()) result.push_back(midisynth::Backend::Avx2);
    return result;
}

void coreSuite(const midisynth::SampleBank& bank, const Options& options) {
    for (const auto backend : availableBackends()) {
        for (const auto workers : {0U, 3U}) {
            for (const auto voices : {64U, 256U, 1000U, 10000U, 100000U})
                run(bank, sustained(voices), backend, workers, 512, options);
            for (const auto voices : {64U, 256U, 1000U})
                run(bank, chopped(voices), backend, workers, 512, options);
            run(bank, sameFrameNoteOns(10000), backend, workers, 512, options);
            run(bank, uniqueEventFrames(10000), backend, workers, 512, options);
            run(bank, irrelevantEventBoundaries(10000), backend, workers, 512, options);
            run(bank, sustained(10000, 44100), backend, workers, 512, options);
        }
    }
}

void workerSuite(const midisynth::SampleBank& bank, const Options& options) {
    for (const auto backend : availableBackends()) {
        for (const auto workers : {0U, 1U, 2U, 3U, 4U, 6U, 8U, 12U, 16U}) {
            run(bank, sustained(10000, 44100), backend, workers, 512, options);
            run(bank, chopped(1000), backend, workers, 512, options);
        }
    }
}

void tileSuite(const midisynth::SampleBank& bank, const Options& options) {
    for (const auto backend : availableBackends()) {
        for (const auto tile : {64U, 128U, 256U, 512U, 1024U, 2048U}) {
            run(bank, sustained(10000, 44100), backend, 3, tile, options);
            run(bank, chopped(1000), backend, 3, tile, options);
        }
    }
}

void diagnosticSuite(const midisynth::SampleBank& bank, const Options& options) {
    for (const auto backend : availableBackends()) {
        run(bank, sameFrameNoteOns(10000), backend, 0, 512, options);
        run(bank, uniqueEventFrames(10000), backend, 0, 512, options);
        run(bank, irrelevantEventBoundaries(10000), backend, 0, 512, options);
        run(bank, sustained(10000, 44100), backend, 0, 512, options);
        run(bank, fullEnvelope(10000, false), backend, 0, 512, options);
        run(bank, fullEnvelope(10000, true), backend, 0, 512, options);
        run(bank, controllerActivity(10000), backend, 0, 512, options);
    }
}

void realSuite(const Options& options) {
    constexpr std::uint32_t sampleRate = 44100, block = 4096;
    const auto font = midisynth::loadSoundFont(options.sf2);
    const auto midi = midisynth::loadMidiFile(options.midi, sampleRate);
    const auto performance = midisynth::preparePerformance(midi, font, sampleRate);
    const auto frames = performance.lastFrame + sampleRate * 5ULL + 1;
    for (const auto& warning : font.warnings()) {
        std::cerr << "SF2 warning: " << warning << '\n';
    }
    for (const auto& warning : performance.warnings) {
        std::cerr << "MIDI warning: " << warning << '\n';
    }
    for (const auto backend : availableBackends()) {
      for (const auto workers : {0U, 3U}) {
        std::vector<double> wall, evt, syn;
        std::uint32_t highWater = 0;
        std::uint64_t eventCount = 0, eventFrames = 0, dropped = 0;
        for (std::uint32_t runIndex = 0; runIndex < options.runs; ++runIndex) {
            midisynth::SynthConfig config{.voiceCapacity = 65536, .tileSize = 512,
                .maxBlockFrames = block, .workerThreads = workers, .backend = backend};
            config.workerDispatchMinimumFrames = options.workerMinimumFrames;
            midisynth::Synth synth(font.sampleBank(), config);
            std::vector<float> left(block), right(block);
            auto renderAll = [&] {
                std::size_t eventIndex = 0;
                for (std::uint64_t frame = 0; frame < frames; frame += block) {
                    const auto count = static_cast<std::size_t>(
                        std::min<std::uint64_t>(block, frames - frame));
                    const auto first = eventIndex;
                    while (eventIndex < performance.events.size() &&
                           performance.events[eventIndex].frame < frame + count) {
                        ++eventIndex;
                    }
                    synth.render(frame,
                        std::span(performance.events).subspan(first, eventIndex - first),
                        std::span(left).first(count), std::span(right).first(count));
                }
            };
            for (std::uint32_t warmup = 0; warmup < options.warmups; ++warmup) {
                renderAll();
                synth.reset();
            }
            const auto begin = Clock::now();
            renderAll();
            const auto elapsed = std::chrono::duration<double, std::milli>(
                Clock::now() - begin).count();
            const auto& stats = synth.stats();
            wall.push_back(elapsed);
            evt.push_back(stats.eventNanoseconds * 1e-6);
            syn.push_back(stats.synthesisNanoseconds * 1e-6);
            highWater = stats.activeVoiceHighWater;
            eventCount = stats.eventsDispatched;
            eventFrames = stats.uniqueEventFrames;
            dropped = stats.droppedNoteOns;
        }
        const auto wallStats = summarize(wall);
        const auto eventStats = summarize(evt);
        const auto synthesisStats = summarize(syn);
        const auto seconds = wallStats.median * 1e-3;
        std::cout << "real-sf2-midi backend="
                  << (backend == midisynth::Backend::Scalar ? "scalar" : "avx2")
                  << " workers=" << workers << " high-water=" << highWater
                  << " events=" << eventCount << " event-frames=" << eventFrames
                  << " source-noteons=" << performance.sourceNoteOns
                  << " dropped=" << dropped << " samples/s=" << frames / seconds
                  << " RT=" << (frames / static_cast<double>(sampleRate)) / seconds
                  << " wall ms ";
        printDistribution(wallStats);
        std::cout << " EVT ms ";
        printDistribution(eventStats);
        std::cout << " SYN ms ";
        printDistribution(synthesisStats);
        std::cout << '\n';
      }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        if (options.suite != "real") {
            midisynth::SampleBank bank;
            const auto sample = makeSample();
            bank.addLoop(sample, 0, static_cast<std::uint32_t>(sample.size()));
            printHeader(options);
            if (options.suite == "core" || options.suite == "all") coreSuite(bank, options);
            if (options.suite == "workers" || options.suite == "all") workerSuite(bank, options);
            if (options.suite == "tiles" || options.suite == "all") tileSuite(bank, options);
            if (options.suite == "diagnostic" || options.suite == "all") diagnosticSuite(bank, options);
        }
        if (!options.sf2.empty()) realSuite(options);
        else if (options.suite == "real") throw std::invalid_argument("--suite real requires --sf2 and --midi");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BENCHMARK ERROR: " << error.what() << '\n';
        return 1;
    }
}
