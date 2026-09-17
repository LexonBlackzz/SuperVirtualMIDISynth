#include "midisynth/midi_file.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

void be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8)); out.push_back(static_cast<std::uint8_t>(value));
}
void be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24)); out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8)); out.push_back(static_cast<std::uint8_t>(value));
}
void vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t bytes[4]; unsigned count = 0;
    bytes[count++] = static_cast<std::uint8_t>(value & 0x7fU);
    while ((value >>= 7) != 0) bytes[count++] = static_cast<std::uint8_t>(0x80U | (value & 0x7fU));
    while (count != 0) out.push_back(bytes[--count]);
}
void event(std::vector<std::uint8_t>& track, std::uint32_t delta, std::uint8_t status,
           std::uint8_t a, std::uint8_t b, bool oneByte = false) {
    vlq(track, delta); track.push_back(status); track.push_back(a); if (!oneByte) track.push_back(b);
}
struct Workload { std::string name; std::vector<std::vector<std::uint8_t>> tracks; };

std::vector<std::uint8_t> fileFor(const Workload& workload) {
    std::vector<std::uint8_t> file{'M','T','h','d'};
    be32(file, 6); be16(file, workload.tracks.size() == 1 ? 0 : 1);
    be16(file, static_cast<std::uint16_t>(workload.tracks.size())); be16(file, 480);
    for (auto track : workload.tracks) {
        track.insert(track.end(), {0, 0xff, 0x2f, 0});
        file.insert(file.end(), {'M','T','r','k'}); be32(file, static_cast<std::uint32_t>(track.size()));
        file.insert(file.end(), track.begin(), track.end());
    }
    return file;
}
Workload notes(std::string name, std::uint32_t pairs, std::uint32_t gap,
               std::uint32_t perFrame, unsigned tracks = 1) {
    Workload result{std::move(name), std::vector<std::vector<std::uint8_t>>(tracks)};
    for (std::uint32_t i = 0; i < pairs; ++i) {
        auto& track = result.tracks[i % tracks];
        const auto delta = i < tracks ? 0U : (i % perFrame == 0 ? gap : 0U);
        event(track, delta, static_cast<std::uint8_t>(0x90U | (i & 15U)), i & 127U, 100);
        event(track, 0, static_cast<std::uint8_t>(0x80U | (i & 15U)), i & 127U, 0);
    }
    return result;
}
Workload noteOns(std::string name, std::uint32_t count) {
    Workload result{std::move(name), {}}; auto& track = result.tracks.emplace_back();
    for (std::uint32_t i = 0; i < count; ++i) {
        event(track, 0, static_cast<std::uint8_t>(0x90U | (i & 15U)), i & 127U, 100);
    }
    return result;
}
Workload ordinary(std::uint32_t bars) {
    Workload result{"ordinary-musical", {}}; auto& t = result.tracks.emplace_back();
    for (std::uint32_t i = 0; i < bars; ++i) {
        if (i % 64 == 0) event(t, i == 0 ? 0 : 120, 0xc0, i & 7U, 0, true);
        event(t, i % 64 == 0 ? 0 : 120, 0x90, 48 + i % 24, 70 + i % 40);
        event(t, 60, 0x80, 48 + i % 24, 0);
        if (i % 16 == 0) event(t, 0, 0xb0, 64, (i / 16) & 1U ? 127 : 0);
    }
    return result;
}
Workload controllers(std::uint32_t count) {
    Workload result{"controller-program-heavy", {}}; auto& t = result.tracks.emplace_back();
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((i & 3U) == 0) event(t, 1, 0xc0 | (i & 15U), i & 127U, 0, true);
        else event(t, 1, 0xb0 | (i & 15U), (i & 1U) ? 64 : 32, i & 127U);
    }
    return result;
}

void run(const Workload& workload, const midisynth::PreparedSoundFont& font) {
    const auto bytes = fileFor(workload);
    const auto path = "midisynth_midi_benchmark.mid";
    { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    std::vector<double> loadTimes, decodeTimes, prepareTimes;
    midisynth::MidiFile midi;
    std::uint64_t decoded = 0;
    for (unsigned run = 0; run < 3; ++run) {
        const auto loadStart = Clock::now();
        midi = midisynth::loadMidiFile(path, 44100.0);
        loadTimes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - loadStart).count());
        midisynth::MidiMessage message; decoded = 0;
        const auto decodeStart = Clock::now();
        auto reader = midi.reader(); while (reader.next(message)) ++decoded;
        decodeTimes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - decodeStart).count());
        const auto prepareStart = Clock::now();
        const auto performance = midisynth::preparePerformance(midi, font, 44100.0);
        prepareTimes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - prepareStart).count());
    }
    std::remove(path);
    std::sort(loadTimes.begin(), loadTimes.end());
    std::sort(decodeTimes.begin(), decodeTimes.end());
    std::sort(prepareTimes.begin(), prepareTimes.end());
    const auto loadMs = loadTimes[1], decodeMs = decodeTimes[1], prepareMs = prepareTimes[1];
    const auto& s = midi.storage;
    std::cout << std::left << std::setw(29) << workload.name << std::right
              << std::setw(11) << s.logicalMessages << std::setw(12) << s.compactBytes
              << std::setw(9) << std::fixed << std::setprecision(3)
              << (s.logicalMessages ? static_cast<double>(s.compactBytes) / s.logicalMessages : 0)
              << std::setw(11) << s.timingAndGroupBytes << std::setw(12) << s.messagePayloadBytes
              << std::setw(12) << s.peakParserWorkingBytes << std::setw(12) << s.legacyEstimatedPeakBytes
              << std::setw(10) << std::setprecision(2) << loadMs
              << std::setw(12) << (decodeMs > 0 ? decoded / (decodeMs * 1e-3) / 1e6 : 0)
              << std::setw(11) << prepareMs << '\n';
}
} // namespace

int main() {
    try {
        midisynth::PreparedSoundFont font;
        std::vector<float> sample{1.0F, 0.0F};
        const auto handle = font.preparationSamples().addLoop(sample, 0, 2);
        for (std::uint16_t bank : {std::uint16_t{0}, std::uint16_t{128}}) {
            for (std::uint8_t program = 0; program < 8; ++program) {
                auto& preset = font.addPreset(bank, program);
                preset.regions.push_back({handle, 0, 127, 0, 127, 60, 44100.0F});
            }
        }
        font.finalize();
        std::cout << "workload                       messages     compact    B/msg    headers    payload     newPeak     oldPeak   load_ms  decode_M/s prepare_ms\n";
        run(notes("isolated-large-gaps", 50000, 0x0fffffffU, 1), font);
        run(ordinary(100000), font);
        run(noteOns("many-same-frame-noteons", 1000000), font);
        run(notes("same-frame-on-off", 500000, 0, 1000000), font);
        run(notes("tiny-timing-deltas", 250000, 1, 1), font);
        run(notes("very-large-timing-deltas", 50000, 0x0fffffffU, 1), font);
        run(notes("black-midi-multichannel", 500000, 1, 4096, 8), font);
        run(controllers(500000), font);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MIDI BENCHMARK ERROR: " << error.what() << '\n';
        return 1;
    }
}
