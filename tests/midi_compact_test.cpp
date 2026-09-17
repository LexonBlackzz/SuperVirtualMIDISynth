#include "midisynth/midi_file.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}
void be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}
void id(std::vector<std::uint8_t>& out, const char* text) { out.insert(out.end(), text, text + 4); }
void vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t bytes[4];
    unsigned count = 0;
    bytes[count++] = static_cast<std::uint8_t>(value & 0x7fU);
    while ((value >>= 7) != 0) bytes[count++] = static_cast<std::uint8_t>(0x80U | (value & 0x7fU));
    while (count != 0) out.push_back(bytes[--count]);
}
std::vector<std::uint8_t> smf(std::uint16_t format,
                              const std::vector<std::vector<std::uint8_t>>& tracks,
                              std::uint16_t division = 480) {
    std::vector<std::uint8_t> file;
    id(file, "MThd"); be32(file, 6); be16(file, format);
    be16(file, static_cast<std::uint16_t>(tracks.size())); be16(file, division);
    for (const auto& track : tracks) {
        id(file, "MTrk"); be32(file, static_cast<std::uint32_t>(track.size()));
        file.insert(file.end(), track.begin(), track.end());
    }
    return file;
}
midisynth::MidiFile load(const std::vector<std::uint8_t>& bytes, const std::string& suffix,
                         double sampleRate = 44100.0) {
    const auto path = "midisynth_compact_" + suffix + ".mid";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    try {
        auto midi = midisynth::loadMidiFile(path, sampleRate);
        std::remove(path.c_str());
        return midi;
    } catch (...) {
        std::remove(path.c_str());
        throw;
    }
}
std::vector<midisynth::MidiMessage> decode(const midisynth::MidiFile& midi) {
    std::vector<midisynth::MidiMessage> messages;
    auto reader = midi.reader();
    midisynth::MidiMessage message;
    while (reader.next(message)) messages.push_back(message);
    return messages;
}
void endTrack(std::vector<std::uint8_t>& track) {
    track.insert(track.end(), {0, 0xff, 0x2f, 0});
}

void formatZeroRunningStatusAndNormalization() {
    std::vector<std::uint8_t> track{0, 0xc2, 5, 0, 0xb2, 0, 1, 0, 32, 2,
        0, 64, 127, 0, 0x92, 60, 100, 0, 61, 0, 0, 0x82, 60, 4};
    endTrack(track);
    const auto midi = load(smf(0, {track}), "format0");
    const auto messages = decode(midi);
    require(messages.size() == 7, "format-0/running-status message count differs");
    require(messages[0].type == midisynth::MidiMessageType::ProgramChange &&
            messages[1].type == midisynth::MidiMessageType::ControlChange &&
            messages[4].type == midisynth::MidiMessageType::NoteOn &&
            messages[5].type == midisynth::MidiMessageType::NoteOff &&
            messages[6].type == midisynth::MidiMessageType::NoteOff,
            "running status or velocity-zero normalization differs");
    require(messages[5].data2 == 0 && messages[5].channel == 2,
            "velocity-zero NoteOn was not normalized exactly");
    require(midi.storage.compactBytes == midi.compactData.size() &&
            midi.storage.compactBytes == midi.storage.timingAndGroupBytes +
                                         midi.storage.messagePayloadBytes,
            "compact accounting does not sum to retained bytes");
}

void formatOneTempoAndCrossTrackOrder() {
    std::vector<std::uint8_t> track0{
        0, 0xc0, 5,
        0, 0x90, 60, 100,
        0x81, 0x70, 0xff, 0x51, 3, 0x0f, 0x42, 0x40,
        0x81, 0x70, 0x80, 60, 0};
    endTrack(track0);
    std::vector<std::uint8_t> track1{
        0, 0xb1, 0, 1,
        0, 0xb1, 32, 2,
        0, 0xb1, 64, 127,
        0, 0x91, 61, 90,
        0x81, 0x70, 0x81, 61, 0,
        0x81, 0x70, 0xb1, 64, 0};
    endTrack(track1);
    const auto midi = load(smf(1, {track0, track1}), "format1");
    const auto messages = decode(midi);
    // This is the complete logical stream produced by the former wide
    // Raw/stable-sort decoder for this fixture.
    const std::vector<midisynth::MidiMessage> reference{
        {0,0,midisynth::MidiMessageType::ProgramChange,0,5,0},
        {0,1,midisynth::MidiMessageType::NoteOn,0,60,100},
        {0,2,midisynth::MidiMessageType::ControlChange,1,0,1},
        {0,3,midisynth::MidiMessageType::ControlChange,1,32,2},
        {0,4,midisynth::MidiMessageType::ControlChange,1,64,127},
        {0,5,midisynth::MidiMessageType::NoteOn,1,61,90},
        {11025,6,midisynth::MidiMessageType::NoteOff,1,61,0},
        {33075,7,midisynth::MidiMessageType::NoteOff,0,60,0},
        {33075,8,midisynth::MidiMessageType::ControlChange,1,64,0}};
    require(messages.size() == reference.size(), "format-1 message count differs");
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const auto& actual = messages[i]; const auto& expected = reference[i];
        require(actual.frame == expected.frame && actual.order == expected.order &&
                actual.type == expected.type && actual.channel == expected.channel &&
                actual.data1 == expected.data1 && actual.data2 == expected.data2,
                "compact decode differs from reference at message " + std::to_string(i));
    }

    auto font = midisynth::PreparedSoundFont{};
    const std::vector<float> sample{1.0F, 0.0F};
    const auto handle = font.preparationSamples().addLoop(sample, 0, 2);
    auto& preset = font.addPreset(0, 5);
    preset.regions.push_back({handle, 0, 127, 0, 127, 60, 44100.0F});
    auto& banked = font.addPreset(130, 0);
    banked.regions.push_back({handle, 0, 127, 0, 127, 60, 44100.0F});
    font.finalize();
    const auto performance = midisynth::preparePerformance(midi, font, 44100.0);
    require(performance.sourceNoteOns == 2, "preparePerformance did not stream every NoteOn");
    require(performance.events.size() == 4 && performance.events[0].frame == 0 &&
            performance.events[1].frame == 0 && performance.events[2].frame == 33075 &&
            performance.events[3].frame == 33075,
            "program/bank/sustain preparation order differs");
}

void maximumVlqAndLargeFrameDelta() {
    std::vector<std::uint8_t> track{0, 0x90, 60, 1};
    vlq(track, 0x0fffffffU); track.insert(track.end(), {0x80, 60, 0});
    endTrack(track);
    const auto midi = load(smf(0, {track}, 1), "maxvlq", 1000000.0);
    const auto messages = decode(midi);
    require(messages.size() == 2 && messages[1].frame == 134217727500000ULL,
            "maximum legal MIDI VLQ or large frame delta changed");
    require(midi.storage.timingAndGroupBytes > 4,
            "large frame delta did not use a variable-length extension");
}

void pitchBendAndCommonControllers() {
    std::vector<std::uint8_t> track{
        0,0xe0,0,64,
        0,0xb0,7,100,
        0,0xb0,10,0,
        0,0xb0,11,80,
        0,0xb0,101,0,
        0,0xb0,100,0,
        0,0xb0,6,12};
    endTrack(track);
    const auto midi=load(smf(0,{track}),"controllers");
    const auto messages=decode(midi);
    require(messages.size()==7&&messages[0].type==midisynth::MidiMessageType::PitchBend,
            "pitch bend was not retained by compact MIDI");
    require(midi.warnings.empty(),"supported common controllers produced warnings");

    midisynth::PreparedSoundFont font;
    const std::vector<float> sample{1,1};
    const auto handle=font.preparationSamples().addLoop(sample,0,2);
    auto& preset=font.addPreset(0,0); preset.regions.push_back({handle}); font.finalize();
    const auto performance=midisynth::preparePerformance(midi,font,44100);
    const std::vector expected{midisynth::EventType::PitchBend,
        midisynth::EventType::ChannelVolume,midisynth::EventType::ChannelPan,
        midisynth::EventType::ChannelExpression,midisynth::EventType::PitchBendRange};
    require(performance.events.size()==expected.size(),"common controller expansion count differs");
    for(std::size_t i=0;i<expected.size();++i) require(performance.events[i].type==expected[i],
        "common controller expansion order differs");
    require(performance.events.back().value==1200,"RPN 0,0 bend range was not decoded");
}

void compactCorruptionIsRejected() {
    const std::vector<std::vector<std::uint8_t>> corrupt{
        {0x80},                         // truncated frame delta
        {0, 0},                         // empty group
        {0, 2, 0x10, 60, 100},         // count exceeds remaining payload
        {0, 1, 0xf0, 60},              // unknown opcode
        {0, 1, 0x10, 60},              // truncated 3-byte message
        {0, 1, 0x00, 0x80},            // invalid data byte
        {0, 1, 0x00, 60, 0, 1, 0x00, 61}, // duplicate frame group
        {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,2,1,0,60},
        {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,1,1,0,60,
         1,1,0,61} // valid UINT64_MAX frame followed by overflowing delta
    };
    for (const auto& bytes : corrupt) {
        bool rejected = false;
        try {
            midisynth::CompactMidiReader reader(bytes);
            midisynth::MidiMessage message;
            while (reader.next(message)) {}
        } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "compact-stream corruption was accepted");
    }
}

void malformedSmfIsRejected() {
    std::vector<std::vector<std::uint8_t>> tracks;
    tracks.push_back({0x81}); // truncated VLQ
    bool rejected = false;
    try { (void)load(smf(0, tracks), "truncated_vlq"); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "truncated SMF VLQ was accepted");

    tracks[0] = {0, 60, 100}; // running data without status
    rejected = false;
    try { (void)load(smf(0, tracks), "bad_running"); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "invalid running status was accepted");

    auto invalidChunk = smf(0, {{0, 0xff, 0x2f, 0}});
    invalidChunk[14] = 'X';
    rejected = false;
    try { (void)load(invalidChunk, "invalid_chunk"); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "invalid track chunk was accepted");

    auto overflowingLength = smf(0, {{0, 0xff, 0x2f, 0}});
    overflowingLength[18] = overflowingLength[19] = overflowingLength[20] =
        overflowingLength[21] = 0xff;
    rejected = false;
    try { (void)load(overflowingLength, "track_overflow"); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "overflowing track length was accepted");
}

void millionEventDenseGroup() {
    constexpr std::uint32_t count = 1000000;
    std::vector<std::uint8_t> track;
    track.reserve(3 + (count - 1) * 2 + 4);
    track.insert(track.end(), {0, 0x90, 0, 100});
    for (std::uint32_t i = 1; i < count; ++i) {
        track.push_back(0);
        track.push_back(static_cast<std::uint8_t>(i & 0x7fU));
        track.push_back(100);
    }
    endTrack(track);
    const auto midi = load(smf(0, {track}), "million");
    require(midi.storage.logicalMessages == count, "million-event stream lost messages");
    require(midi.storage.compactBytes <= count * 3ULL + 5,
            "dense NoteOn stream missed the three-byte target");
    std::uint64_t decoded = 0;
    auto reader = midi.reader();
    midisynth::MidiMessage message;
    while (reader.next(message)) {
        require(message.frame == 0 && message.type == midisynth::MidiMessageType::NoteOn,
                "million-event decode changed content");
        ++decoded;
    }
    require(decoded == count, "million-event sequential decode count differs");
    require(midi.storage.peakParserWorkingBytes < midi.storage.legacyEstimatedPeakBytes,
            "compact parser peak estimate did not improve on wide legacy pipeline");
}

} // namespace

int main() {
    try {
        formatZeroRunningStatusAndNormalization();
        formatOneTempoAndCrossTrackOrder();
        maximumVlqAndLargeFrameDelta();
        pitchBendAndCommonControllers();
        compactCorruptionIsRejected();
        malformedSmfIsRejected();
        millionEventDenseGroup();
        std::cout << "All compact MIDI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MIDI TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
