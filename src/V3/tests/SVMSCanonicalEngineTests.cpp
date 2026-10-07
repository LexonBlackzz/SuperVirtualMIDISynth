#include "CanonicalChannelInterpreter.h"
#include "CanonicalCore.h"
#include "CanonicalSoundFont.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic<std::uint64_t> allocationCount{};
double differentialMaximum{};
double differentialRms{};
}

void* operator new(std::size_t size) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
namespace c = svms::canonical;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void near(float actual, float expected, const std::string& message,
          float tolerance = 4.0e-5F) {
    if (std::fabs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

c::SampleBank basicBank() {
    c::SampleBank bank;
    const std::vector<float> sample{1.0F, 0.5F, -0.5F, -1.0F};
    bank.addOneShot(sample);
    bank.addLoop(sample, 1, 4);
    return bank;
}

c::PreparedSoundFont preparedFixture() {
    c::PreparedSoundFont font;
    const std::vector<float> sample{1.0F, 0.5F, -0.5F, -1.0F};
    auto& samples = font.preparationSamples();
    const auto oneShot = samples.addOneShot(sample);
    const auto loop = samples.addLoop(sample, 1, 4);
    auto& preset = font.addPreset(0, 5, "layers");
    preset.regions.push_back({oneShot, 60, 60, 0, 63, 60, 44100.0F});
    preset.regions.push_back({loop, 60, 72, 64, 127, 60, 44100.0F});
    preset.regions.push_back({oneShot, 60, 72, 64, 127, 72, 44100.0F,
                              100.0F, 0.0F, 0.5F});
    font.finalize();
    return font;
}

void exactFramesAndStableOrder() {
    auto bank = basicBank();
    c::Synth synth(bank, {.voiceCapacity=8, .maxBlockFrames=16});
    const std::vector events{c::Event::noteOn(3, 0, 60, 0, 0.5F)};
    std::vector<float> left(7), right(7);
    synth.render(0, events, left, right);
    near(left[2], 0.0F, "note-on leaked before its frame");
    near(left[3], 1.0F, "note-on missed exact frame");
    near(left[4], 0.75F, "phase/interpolation mismatch");

    c::Synth ordered(bank, {.voiceCapacity=8, .maxBlockFrames=8});
    const std::vector orderedEvents{
        c::Event::noteOff(0, 1, 60), c::Event::noteOn(0, 2, 60, 1)};
    std::vector<float> ol(2), ort(2);
    ordered.render(0, orderedEvents, ol, ort);
    near(ol[0], 1.0F, "same-frame NoteOff/NoteOn order changed");
}

void deterministicCapacitySteal() {
    auto bank = basicBank();
    c::Synth synth(bank, {.voiceCapacity=1, .maxBlockFrames=8});
    const std::vector events{
        c::Event::noteOn(0, 0, 60, 1),
        c::Event::noteOn(1, 1, 61, 1)};
    std::vector<float> left(3), right(3);
    synth.render(0, events, left, right);
    require(synth.stats().stolenVoices == 1 &&
            synth.stats().droppedNoteOns == 0,
            "capacity pressure did not use canonical deterministic stealing");
    const auto active = synth.activeVoices();
    require(active.size() == 1 && active[0].note == 61,
            "capacity steal retained the wrong logical voice");
}

void preparationAndSustainOwnership() {
    auto font = preparedFixture();
    const auto* preset = font.findPreset(0, 5);
    require(preset && font.matchingRegions(*preset, 60, 100).size() == 2,
            "layered prepared lookup failed");
    std::vector<c::Event> prepared;
    std::uint32_t sequence{};
    c::appendPreparedNoteOn(prepared, *preset, font, 0, sequence,
                            0, 60, 127, 44100.0);
    require(prepared.size() == 2, "layered note did not expand");
    near(prepared[0].leftGain, std::sqrt(0.5F),
         "velocity/pan preparation changed", 2.0e-7F);

    c::detail::ChannelInterpreter interpreter(font, 44100.0, 16, false);
    std::vector<c::Event> events;
    events.reserve(32);
    auto process = [&](std::uint64_t frame, c::MidiMessageType type,
                       std::uint8_t a, std::uint8_t b) {
        require(interpreter.process({frame, type, 0, a, b}, sequence, events),
                "channel interpreter rejected event");
    };
    process(0, c::MidiMessageType::ProgramChange, 5, 0);
    process(0, c::MidiMessageType::NoteOn, 60, 100);
    process(1, c::MidiMessageType::ControlChange, 64, 127);
    process(2, c::MidiMessageType::NoteOff, 60, 0);
    const auto beforePedalUp = events.size();
    process(3, c::MidiMessageType::ControlChange, 64, 0);
    require(events.size() == beforePedalUp + 1 &&
            events.back().type == c::EventType::NoteOff &&
            events.back().frame == 3,
            "sustain did not retain/release logical note ownership");

    c::detail::ChannelInterpreter tailInterpreter(font, 44100.0, 16, false);
    std::vector<c::Event> tailEvents;
    tailEvents.reserve(16);
    std::uint32_t tailSequence{};
    require(tailInterpreter.process({0, c::MidiMessageType::ProgramChange,
                0, 5, 0}, tailSequence, tailEvents) &&
            tailInterpreter.process({0, c::MidiMessageType::NoteOn,
                0, 60, 100}, tailSequence, tailEvents) &&
            tailInterpreter.process({1, c::MidiMessageType::ControlChange,
                0, 64, 127}, tailSequence, tailEvents) &&
            tailInterpreter.process({2, c::MidiMessageType::NoteOff,
                0, 60, 0}, tailSequence, tailEvents),
        "tail setup was rejected");
    require(tailInterpreter.requiredReleaseAllCapacity() == 1,
        "held note missing from forced end-of-song release capacity");
    tailInterpreter.appendReleaseAll(3, tailSequence, tailEvents);
    require(tailEvents.back().type == c::EventType::NoteOff &&
            tailEvents.back().frame == 3 &&
            tailInterpreter.requiredReleaseAllCapacity() == 0,
        "end-of-song release left a sustain-held logical note active");
}

void filterPreparationAndLifecycleFeatures() {
    c::PreparedSoundFont font;
    const std::vector<float> sample{1.0F, 0.5F, -0.5F, -1.0F};
    const auto loop = font.preparationSamples().addLoop(sample, 1, 4);
    auto& preset = font.addPreset(0, 0, "feature fixture");
    c::PreparedRegion region{};
    region.sample = loop;
    region.keyLow = 0;
    region.keyHigh = 127;
    region.velocityLow = 0;
    region.velocityHigh = 127;
    region.rootKey = 60;
    region.sampleRate = 44100.0F;
    region.scaleTuning = 100.0F;
    region.gain = 1.0F;
    region.sustainGain = 1.0F;
    region.filterCutoffCents = 6000;
    region.filterResonanceCentibels = 120;
    region.sampleMode = 3;
    region.exclusiveClass = 5;
    region.forcedKey = 72;
    region.forcedVelocity = 64;
    region.modLfoDelaySeconds = 2.0F / 44100.0F;
    region.modLfoFrequencyHz = 110.25F;
    region.modLfoToPitchCents = 25.0F;
    region.vibLfoToPitchCents = 10.0F;
    region.modLfoToFilterCents = 100.0F;
    region.modLfoToVolumeCentibels = 20.0F;
    region.modEnvDelaySeconds = 1.0F / 44100.0F;
    region.modEnvAttackSeconds = 4.0F / 44100.0F;
    region.modEnvHoldSeconds = 2.0F / 44100.0F;
    region.modEnvDecaySeconds = 4.0F / 44100.0F;
    region.modEnvReleaseSeconds = 3.0F / 44100.0F;
    region.modEnvSustain = 0.25F;
    region.modEnvKeynumToHoldTimecents = 100.0F;
    region.modEnvKeynumToDecayTimecents = 100.0F;
    region.modEnvToPitchCents = 40.0F;
    region.modEnvToFilterCents = 300.0F;
    region.modulators.push_back({0x0002, 17, 100, 0, 0});
    region.modulators.push_back({0x0003, 8, 1200, 0, 0});
    preset.regions.push_back(region);
    font.finalize();

    std::vector<c::Event> prepared;
    std::uint32_t sequence{};
    c::appendPreparedNoteOn(prepared, *font.findPreset(0, 0), font, 0,
        sequence, 0, 60, 127, 44100.0, 41);
    require(prepared.size() == 1 && prepared[0].filterEnabled,
        "low SF2 cutoff did not prepare a canonical filter");
    require(prepared[0].releaseFrames == 441,
        "SF2 release lost the V3 10 ms minimum");
    require(prepared[0].filterA0 > 0.0F && prepared[0].filterA0 < 1.0F,
        "prepared filter coefficient is invalid");
    near(prepared[0].phaseIncrement, 2.0F,
        "forced key did not affect synthesis pitch", 1.0e-6F);
    near(std::hypot(prepared[0].leftGain, prepared[0].rightGain),
        (64.0F / 127.0F) * (64.0F / 127.0F),
        "forced velocity did not feed square curve", 1.0e-6F);
    require(prepared[0].note == 60 && prepared[0].noteInstance == 41,
        "forced key changed logical NoteOff ownership");
    require(prepared[0].modLfoPhaseStep > 0.0F &&
            prepared[0].modEnvDelayFrames == 1 &&
            prepared[0].modEnvAttackFrames == 4 &&
            prepared[0].modEnvHoldFrames == 1 &&
            prepared[0].modEnvDecayFrames == 2 &&
            prepared[0].modEnvReleaseFrames == 3 &&
            prepared[0].modEnvSustain == 0.25F &&
            prepared[0].modEnvToFilterCents == 300.0F,
        "LFO/modulation-envelope preparation lost generator state");
    near(prepared[0].baseFilterCutoffCents, 5493.75F,
        "explicit key-to-filter modulation or velocity default changed", 1.0e-4F);
    require(prepared[0].leftGain != prepared[0].rightGain,
        "explicit velocity-to-pan modulator was not evaluated");

    c::PreparedSoundFont openFont;
    const auto openSample = openFont.preparationSamples().addLoop(sample, 1, 4);
    auto& openPreset = openFont.addPreset(0, 0);
    region.sample = openSample;
    region.filterCutoffCents = 13500;
    region.filterResonanceCentibels = 0;
    region.forcedVelocity = 127;
    openPreset.regions.push_back(region);
    openFont.finalize();
    prepared.clear();
    sequence = 0;
    c::appendPreparedNoteOn(prepared, *openFont.findPreset(0, 0), openFont,
        0, sequence, 0, 60, 100, 32000.0);
    require(!prepared[0].filterEnabled,
        "effectively open SF2 filter should not retain recursive state");

    auto bank = basicBank();
    auto oldA = c::Event::noteOn(0, 0, 60, 1);
    oldA.noteInstance = 1;
    oldA.exclusiveClass = 5;
    oldA.sampleMode = 3;
    auto oldLayer = c::Event::noteOn(0, 1, 60, 1);
    oldLayer.noteInstance = 1;
    oldLayer.sampleMode = 3;
    auto otherChannel = c::Event::noteOn(0, 2, 60, 1, 1.0F, 1.0F,
        1.0F, 0, 100, 1);
    otherChannel.noteInstance = 2;
    otherChannel.exclusiveClass = 5;
    auto replacement = c::Event::noteOn(0, 3, 61, 1);
    replacement.noteInstance = 3;
    replacement.exclusiveClass = 5;
    replacement.exclusiveMaskLow = std::uint64_t{1} << 5;
    replacement.exclusiveReleaseFrames = 32;
    c::Synth chokeSynth(bank, {.voiceCapacity=8, .maxBlockFrames=8});
    std::vector<float> left(2), right(2);
    const std::vector<c::Event> chokeEvents{oldA, oldLayer, otherChannel, replacement};
    chokeSynth.render(0, chokeEvents, left, right);
    const auto voices = chokeSynth.activeVoices();
    std::size_t releasedVictimLayers = 0;
    bool independentChannelAlive = false;
    for (const auto& voice : voices) {
        if (voice.noteInstance == 1 &&
            voice.envelopeStage == c::EnvelopeStage::Release && !voice.looping)
            ++releasedVictimLayers;
        if (voice.noteInstance == 2 && voice.envelopeStage != c::EnvelopeStage::Release)
            independentChannelAlive = true;
    }
    require(releasedVictimLayers == 2 && independentChannelAlive,
        "exclusiveClass did not choke the complete channel-local logical note");

    auto releaseLoop = c::Event::noteOn(0, 0, 60, 1, 1.0F, 1.0F, 1.0F, 0, 100);
    releaseLoop.sampleMode = 3;
    c::Synth releaseLoopSynth(bank, {.voiceCapacity=4, .maxBlockFrames=16});
    std::vector<c::Event> releaseEvents{releaseLoop, c::Event::noteOff(2, 1, 60)};
    left.assign(8, 0.0F); right.assign(8, 0.0F);
    releaseLoopSynth.render(0, releaseEvents, left, right);
    require(releaseLoopSynth.activeVoiceCount() == 0,
        "sampleModes=3 did not leave its loop on release");

    c::PreparedSoundFont sustainFont;
    const auto sustainSample = sustainFont.preparationSamples().addLoop(sample, 1, 4);
    auto& sustainPreset = sustainFont.addPreset(0, 0);
    region.sample = sustainSample;
    region.sampleMode = 3;
    region.forcedKey = -1;
    region.forcedVelocity = -1;
    sustainPreset.regions.push_back(region);
    sustainFont.finalize();
    c::detail::ChannelInterpreter sustainInterpreter(sustainFont, 44100.0, 8, false);
    std::vector<c::Event> sustainEvents;
    sustainEvents.reserve(8);
    std::uint32_t sustainSequence{};
    const auto sustainProcess = [&](std::uint64_t frame, c::MidiMessageType type,
                                    std::uint8_t a, std::uint8_t b) {
        require(sustainInterpreter.process({frame, type, 0, a, b},
            sustainSequence, sustainEvents), "sustain-loop interpreter rejected event");
    };
    sustainProcess(0, c::MidiMessageType::NoteOn, 60, 100);
    sustainProcess(1, c::MidiMessageType::ControlChange, 64, 127);
    sustainProcess(2, c::MidiMessageType::NoteOff, 60, 0);
    sustainProcess(4, c::MidiMessageType::ControlChange, 64, 0);
    c::Synth sustainLoopSynth(sustainFont.sampleBank(),
        {.voiceCapacity=4, .maxBlockFrames=16});
    left.assign(12, 0.0F); right.assign(12, 0.0F);
    sustainLoopSynth.render(0, sustainEvents, left, right);
    require(sustainLoopSynth.activeVoiceCount() == 0,
        "sampleModes=3 did not leave its loop on sustain release");
}

std::vector<c::Event> denseEvents() {
    std::vector<c::Event> events;
    events.reserve(400);
    std::uint32_t sequence{};
    for (std::uint32_t i = 0; i < 300; ++i) {
        auto event = c::Event::noteOn(i % 3, sequence++,
            static_cast<std::uint8_t>(i % 96), i & 1,
            0.25F + (i % 9) * 0.125F,
            0.01F * (1 + i % 7), 0.02F * (1 + i % 5),
            i % 4, 4 + i % 5);
        event.delayFrames = i % 3;
        event.holdFrames = i % 4;
        event.decayFrames = i % 6;
        event.sustainGain = 0.2F * (i % 5);
        event.logarithmicEnvelope = (i & 1) != 0;
        if ((i % 3) == 0) {
            event.filterEnabled = true;
            event.filterA0 = 0.15F;
            event.filterB1 = -0.8F;
            event.filterB2 = 0.3F;
        }
        if ((i % 11) == 0) {
            event.modLfoPhaseStep = 0.125F;
            event.vibLfoPhaseStep = 0.2F;
            event.modLfoToPitchCents = 80.0F;
            event.vibLfoToPitchCents = 50.0F;
            event.modLfoToVolumeCentibels = 30.0F;
            event.modLfoToFilterCents = 200.0F;
            event.baseFilterCutoffCents = 6000.0F;
            event.filterResonanceCentibels = 60.0F;
            event.modEnvAttackFrames = 3;
            event.modEnvHoldFrames = 2;
            event.modEnvDecayFrames = 5;
            event.modEnvReleaseFrames = 4;
            event.modEnvSustain = 0.3F;
            event.modEnvToPitchCents = 70.0F;
            event.modEnvToFilterCents = 400.0F;
        }
        events.push_back(event);
    }
    for (std::uint32_t note = 0; note < 48; ++note)
        events.push_back(c::Event::noteOff(19, sequence++,
                                           static_cast<std::uint8_t>(note)));
    events.push_back(c::Event::channelControl(4, sequence++,
        c::EventType::ModulationWheel, 0, 96));
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.frame < b.frame;
    });
    return events;
}

std::pair<std::vector<float>, std::vector<float>> run(
    const c::SampleBank& bank, c::Backend backend, std::uint32_t workers,
    const std::vector<c::Event>& events) {
    c::SynthConfig config{.voiceCapacity=384, .tileSize=37,
        .maxBlockFrames=64, .workerThreads=workers, .backend=backend};
    config.workerDispatchMinimumFrames = 0;
    c::Synth synth(bank, config);
    std::vector<float> left(64), right(64);
    synth.render(0, events, left, right);
    return {left, right};
}

void scalarAvxAndThreadDifferentials() {
    auto bank = basicBank();
    const auto events = denseEvents();
    const auto scalar = run(bank, c::Backend::Scalar, 0, events);
    const auto threadedScalar = run(bank, c::Backend::Scalar, 3, events);
    require(scalar == threadedScalar,
            "threading changed scalar output or reduction order");
    if (!c::Synth::avx2Supported()) return;
    const auto avx = run(bank, c::Backend::Avx2, 0, events);
    const auto threadedAvx = run(bank, c::Backend::Avx2, 3, events);
    require(avx == threadedAvx,
            "threading changed AVX2 output or reduction order");
    double squared = 0.0;
    std::size_t samples = 0;
    for (std::size_t i = 0; i < scalar.first.size(); ++i) {
        near(avx.first[i], scalar.first[i], "AVX2 left differs", 2.0e-5F);
        near(avx.second[i], scalar.second[i], "AVX2 right differs", 2.0e-5F);
        for (const double difference : {
                 static_cast<double>(avx.first[i] - scalar.first[i]),
                 static_cast<double>(avx.second[i] - scalar.second[i])}) {
            differentialMaximum = std::max(differentialMaximum, std::fabs(difference));
            squared += difference * difference;
            ++samples;
        }
    }
    differentialRms = std::sqrt(squared / samples);
}

void renderDoesNotAllocate() {
    auto bank = basicBank();
    const auto events = denseEvents();
    c::Synth synth(bank, {.voiceCapacity=384, .tileSize=37,
        .maxBlockFrames=64, .workerThreads=3, .backend=c::Backend::Scalar});
    std::vector<float> left(64), right(64);
    synth.render(0, events, left, right); // warm workers and caches
    synth.reset();
    const auto before = allocationCount.load(std::memory_order_acquire);
    synth.render(0, events, left, right);
    const auto after = allocationCount.load(std::memory_order_acquire);
    require(after == before, "canonical render allocated from the heap");
}
} // namespace

int main() {
    try {
        exactFramesAndStableOrder();
        deterministicCapacitySteal();
        preparationAndSustainOwnership();
        filterPreparationAndLifecycleFeatures();
        scalarAvxAndThreadDifferentials();
        renderDoesNotAllocate();
        std::cout << "Canonical scalar/AVX2/thread/allocation tests passed"
                  << " (max=" << differentialMaximum
                  << ", diff-rms=" << differentialRms << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
