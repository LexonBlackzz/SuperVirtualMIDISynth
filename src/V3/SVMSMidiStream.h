#ifndef SVMS_MIDI_STREAM_H
#define SVMS_MIDI_STREAM_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace svms {

struct PackedMidiEvent {
    uint64_t outputFrame;
    uint32_t sequence;
    uint32_t message;
};
static_assert(sizeof(PackedMidiEvent) == 16);

struct MidiStreamInfo {
    uint16_t format = 0;
    uint16_t division = 0;
    uint16_t tracks = 0;
    uint64_t eventCount = 0;
    uint64_t noteOnCount = 0;
    uint64_t totalFrames = 0;
    uint64_t peakEventsPerSecond = 0;
    uint64_t peakNoteOnsPerSecond = 0;
    uint64_t peakEventsAtFrame = 0;
    uint64_t peakNoteOnsAtFrame = 0;
    uint64_t peakExactDuplicateNoteOnsAtFrame = 0;
    uint64_t peakKeyDuplicateNoteOnsAtFrame = 0;
    uint64_t exactDuplicateNoteOnCount = 0;
    uint64_t keyDuplicateNoteOnCount = 0;
    uint64_t noteRunExactDuplicateCount = 0;
    uint64_t peakNoteRunExactDuplicatesAtFrame = 0;
    uint64_t adjacentExactDuplicateNoteOnCount = 0;
    uint64_t noteOnFrameCount = 0;
    uint64_t peakEventSecond = 0;
    uint64_t peakNoteOnSecond = 0;
    uint64_t peakFrame = 0;
};

class MappedMidiFile {
public:
    MappedMidiFile();
    ~MappedMidiFile();
    MappedMidiFile(const MappedMidiFile&) = delete;
    MappedMidiFile& operator=(const MappedMidiFile&) = delete;
    bool Open(const wchar_t* path, std::string& error);
    void Close();
    const uint8_t* Data() const { return data_; }
    uint64_t Size() const { return size_; }
private:
    void* file_;
    void* mapping_;
    const uint8_t* data_;
    uint64_t size_;
};

// A reusable decoder. Scan() validates/counts without storing events; Decode()
// invokes the sink in deterministic (tick, track, in-track) order.
class MidiStreamDecoder {
public:
    using Sink = bool (*)(const PackedMidiEvent&, void*);
    bool Scan(const MappedMidiFile& file, uint32_t sampleRate,
              MidiStreamInfo& info, std::string& error) const;
    bool Decode(const MappedMidiFile& file, uint32_t sampleRate,
                Sink sink, void* user, std::atomic<bool>* cancel,
                MidiStreamInfo* info, std::string& error) const;
};

class ParsedEventRing {
public:
    explicit ParsedEventRing(uint32_t megabytes);
    ~ParsedEventRing();
    ParsedEventRing(const ParsedEventRing&) = delete;
    ParsedEventRing& operator=(const ParsedEventRing&) = delete;
    bool IsValid() const { return events_ != nullptr; }
    uint64_t Capacity() const { return capacity_; }
    bool Push(const PackedMidiEvent& event, const std::atomic<bool>& cancel);
    bool Peek(PackedMidiEvent& event) const;
    bool Pop(PackedMidiEvent& event);
    uint64_t Size() const;
private:
    PackedMidiEvent* events_;
    uint64_t capacity_;
    uint64_t mask_;
    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> tail_;
};

} // namespace svms
#endif
