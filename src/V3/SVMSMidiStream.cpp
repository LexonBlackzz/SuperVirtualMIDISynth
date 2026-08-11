#include "SVMSMidiStream.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <queue>

namespace svms {
namespace {
uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t BE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
bool VLen(const uint8_t*& p, const uint8_t* end, uint32_t& value) {
    value = 0;
    for (unsigned i = 0; i != 4; ++i) {
        if (p == end) return false;
        const uint8_t c = *p++;
        value = (value << 7) | (c & 0x7f);
        if ((c & 0x80) == 0) return true;
    }
    return false;
}
struct RawEvent {
    uint64_t tick = 0;
    uint32_t order = 0;
    uint32_t message = 0;
    uint32_t tempo = 0;
    bool midi = false;
    bool valid = false;
};
struct Track {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint64_t tick = 0;
    uint32_t order = 0;
    uint8_t running = 0;
    bool done = false;
};
bool Next(Track& t, RawEvent& out, std::string& error) {
    out = {};
    while (!t.done && t.p < t.end) {
        uint32_t delta = 0;
        if (!VLen(t.p, t.end, delta)) { error = "invalid MIDI delta VLQ"; return false; }
        t.tick += delta;
        if (t.p == t.end) { error = "truncated MIDI event"; return false; }
        uint8_t status = *t.p;
        if (status < 0x80) {
            if (t.running < 0x80 || t.running >= 0xf0) {
                error = "invalid MIDI running status"; return false;
            }
            status = t.running;
        } else {
            ++t.p;
            if (status < 0xf0) t.running = status;
            else t.running = 0;
        }
        out.tick = t.tick;
        out.order = t.order++;
        if (status == 0xff) {
            if (t.p == t.end) { error = "truncated MIDI meta event"; return false; }
            const uint8_t type = *t.p++;
            uint32_t length = 0;
            if (!VLen(t.p, t.end, length) || uint64_t(t.end - t.p) < length) {
                error = "truncated MIDI meta payload"; return false;
            }
            if (type == 0x2f) t.done = true;
            if (type == 0x51 && length == 3) {
                out.tempo = (uint32_t(t.p[0]) << 16) |
                            (uint32_t(t.p[1]) << 8) | t.p[2];
                out.valid = out.tempo != 0;
            }
            t.p += length;
            // Preserve every delta-bearing record in the merged stream even
            // when it produces no synth event. This bounds exact conversion
            // to one legal VLQ delta and includes end-of-track silence.
            out.valid = true;
            return true;
        }
        if (status == 0xf0 || status == 0xf7) {
            uint32_t length = 0;
            if (!VLen(t.p, t.end, length) || uint64_t(t.end - t.p) < length) {
                error = "truncated MIDI SysEx"; return false;
            }
            t.p += length;
            out.valid = true;
            return true;
        }
        if (status >= 0xf0) {
            static const uint8_t lengths[16] = {0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0};
            const uint8_t n = lengths[status & 15];
            if (uint64_t(t.end - t.p) < n) { error = "truncated system event"; return false; }
            t.p += n;
            out.valid = true;
            return true;
        }
        const uint8_t kind = status & 0xf0;
        const uint8_t bytes = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
        if (uint64_t(t.end - t.p) < bytes) { error = "truncated channel event"; return false; }
        const uint8_t d1 = *t.p++;
        const uint8_t d2 = bytes == 2 ? *t.p++ : 0;
        if ((d1 | d2) & 0x80) { error = "invalid MIDI data byte"; return false; }
        out.message = uint32_t(status) | (uint32_t(d1) << 8) | (uint32_t(d2) << 16);
        out.midi = true;
        out.valid = true;
        return true;
    }
    t.done = true;
    return true;
}
bool Tracks(const MappedMidiFile& file, MidiStreamInfo& info,
            std::vector<Track>& tracks, std::string& error) {
    const uint8_t* p = file.Data();
    const uint8_t* end = p + file.Size();
    if (!p || file.Size() < 14 || std::memcmp(p, "MThd", 4) != 0) {
        error = "not a Standard MIDI File"; return false;
    }
    const uint32_t headerLength = BE32(p + 4);
    if (headerLength < 6 || uint64_t(end - p) < uint64_t(8) + headerLength) {
        error = "invalid MIDI header"; return false;
    }
    info.format = BE16(p + 8); info.tracks = BE16(p + 10); info.division = BE16(p + 12);
    if (info.format > 1) { error = "SMF format 2 is not supported"; return false; }
    if (info.division == 0 || (info.division & 0x8000)) {
        error = "SMPTE MIDI time division is not supported"; return false;
    }
    p += 8 + headerLength;
    tracks.clear(); tracks.reserve(info.tracks);
    while (p < end && tracks.size() < info.tracks) {
        if (end - p < 8) { error = "truncated MIDI chunk"; return false; }
        const uint32_t length = BE32(p + 4);
        if (uint64_t(end - (p + 8)) < length) { error = "truncated MIDI track"; return false; }
        if (std::memcmp(p, "MTrk", 4) == 0)
            tracks.push_back({p + 8, p + 8 + length});
        p += 8 + length;
    }
    if (tracks.size() != info.tracks || tracks.empty()) {
        error = "missing MIDI track chunks"; return false;
    }
    return true;
}
struct HeapItem { RawEvent event; uint32_t track; };
struct Later {
    bool operator()(const HeapItem& a, const HeapItem& b) const {
        if (a.event.tick != b.event.tick) return a.event.tick > b.event.tick;
        if (a.track != b.track) return a.track > b.track;
        return a.event.order > b.event.order;
    }
};
bool Run(const MappedMidiFile& file, uint32_t rate, MidiStreamDecoder::Sink sink,
         void* user, std::atomic<bool>* cancel, MidiStreamInfo& info, std::string& error) {
    std::vector<Track> tracks;
    if (!Tracks(file, info, tracks, error)) return false;
    std::priority_queue<HeapItem, std::vector<HeapItem>, Later> heap;
    for (uint32_t i = 0; i < tracks.size(); ++i) {
        RawEvent event;
        if (!Next(tracks[i], event, error)) return false;
        if (event.valid) heap.push({event, i});
    }
    uint64_t tick = 0, frame = 0, remainder = 0, count = 0, noteOns = 0;
    uint32_t tempo = 500000, sequence = 0;
    const uint64_t denom = uint64_t(info.division) * 1000000ull;
    while (!heap.empty()) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        const HeapItem item = heap.top(); heap.pop();
        const uint64_t delta = item.event.tick - tick;
        const uint64_t numerator = uint64_t(tempo) * rate;
        const uint64_t quotient = numerator / denom;
        const uint64_t rest = numerator % denom;
        // SMF VLQs cap each inter-event delta at 0x0fffffff, keeping this
        // exact remainder product inside uint64_t for every legal division.
        frame += delta * quotient;
        const uint64_t fraction = remainder + delta * rest;
        frame += fraction / denom;
        remainder = fraction % denom;
        tick = item.event.tick;
        if (item.event.tempo) tempo = item.event.tempo;
        else if (item.event.midi) {
            PackedMidiEvent packed{frame, sequence++, item.event.message};
            if (sink && !sink(packed, user)) return false;
            ++count;
            if ((item.event.message & 0xf0u) == 0x90u &&
                ((item.event.message >> 16) & 0x7fu) != 0u) ++noteOns;
        }
        RawEvent next;
        if (!Next(tracks[item.track], next, error)) return false;
        if (next.valid) heap.push({next, item.track});
    }
    info.eventCount = count;
    info.noteOnCount = noteOns;
    info.totalFrames = frame;
    return true;
}
} // namespace

MappedMidiFile::MappedMidiFile() : file_(INVALID_HANDLE_VALUE), mapping_(nullptr), data_(nullptr), size_(0) {}
MappedMidiFile::~MappedMidiFile() { Close(); }
bool MappedMidiFile::Open(const wchar_t* path, std::string& error) {
    Close();
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = "cannot open MIDI file"; return false; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0) { CloseHandle(f); error = "empty MIDI file"; return false; }
#if defined(_WIN32) && !defined(_WIN64)
    if (size.QuadPart > 0x7fffffffull) { CloseHandle(f); error = "MIDI file is too large for the x86 renderer"; return false; }
#endif
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) { CloseHandle(f); error = "cannot map MIDI file"; return false; }
    const void* data = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!data) { CloseHandle(m); CloseHandle(f); error = "cannot map MIDI view"; return false; }
    file_ = f; mapping_ = m; data_ = static_cast<const uint8_t*>(data); size_ = uint64_t(size.QuadPart);
    return true;
}
void MappedMidiFile::Close() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(file_));
    file_ = INVALID_HANDLE_VALUE; mapping_ = nullptr; data_ = nullptr; size_ = 0;
}
bool MidiStreamDecoder::Scan(const MappedMidiFile& file, uint32_t rate,
                             MidiStreamInfo& info, std::string& error) const {
    info = {}; return Run(file, rate, nullptr, nullptr, nullptr, info, error);
}
bool MidiStreamDecoder::Decode(const MappedMidiFile& file, uint32_t rate, Sink sink,
                               void* user, std::atomic<bool>* cancel,
                               MidiStreamInfo* info, std::string& error) const {
    MidiStreamInfo local{};
    const bool ok = Run(file, rate, sink, user, cancel, local, error);
    if (info) *info = local;
    return ok;
}
ParsedEventRing::ParsedEventRing(uint32_t mb) : events_(nullptr), capacity_(0), mask_(0), head_(0), tail_(0) {
    uint64_t slots = (uint64_t(mb ? mb : 1) << 20) / sizeof(PackedMidiEvent);
    uint64_t power = 1; while ((power << 1) <= slots) power <<= 1;
    capacity_ = power; mask_ = power - 1;
    events_ = static_cast<PackedMidiEvent*>(VirtualAlloc(nullptr, size_t(power * sizeof(PackedMidiEvent)),
                                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
ParsedEventRing::~ParsedEventRing() { if (events_) VirtualFree(events_, 0, MEM_RELEASE); }
bool ParsedEventRing::Push(const PackedMidiEvent& event, const std::atomic<bool>& cancel) {
    while (!cancel.load(std::memory_order_relaxed)) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) < capacity_) {
            events_[h & mask_] = event;
            head_.store(h + 1, std::memory_order_release); return true;
        }
        Sleep(0);
    }
    return false;
}
bool ParsedEventRing::Peek(PackedMidiEvent& event) const {
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    event = events_[t & mask_]; return true;
}
bool ParsedEventRing::Pop(PackedMidiEvent& event) {
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    event = events_[t & mask_]; tail_.store(t + 1, std::memory_order_release); return true;
}
uint64_t ParsedEventRing::Size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}
} // namespace svms
