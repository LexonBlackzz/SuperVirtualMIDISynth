#include "events/MidiStream.h"
#include "events/SysEx.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#include <fcntl.h>
#include <locale>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>

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
    const uint8_t* sysexData = nullptr;
    uint32_t sysexSize = 0;
    bool midi = false;
    bool sysex = false;
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
            out.sysexData = t.p;
            out.sysexSize = length;
            out.sysex = true;
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
        // Extended-key SMFs carry 0..255 keys after an explicit note status.
        // Running status with a high key is inherently ambiguous.
        if ((d2 & 0x80) || ((d1 & 0x80) && kind != 0x80 && kind != 0x90 && kind != 0xa0)) { error = "invalid MIDI data byte"; return false; }
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
         void* user, std::atomic<bool>* cancel, MidiStreamInfo& info,
         std::string& error,
         MidiStreamDecoder::ScanProgress scanProgress = nullptr,
         void* scanProgressUser = nullptr) {
    std::vector<Track> tracks;
    if (!Tracks(file, info, tracks, error)) return false;
    uint64_t totalTrackBytes = 0;
    for (const Track& track : tracks)
        totalTrackBytes += static_cast<uint64_t>(track.end - track.p);
    uint64_t processedTrackBytes = 0;
    uint64_t processedRecords = 0;
    auto nextEvent = [&](uint32_t trackIndex, RawEvent& event) {
        Track& track = tracks[trackIndex];
        if (!scanProgress) return Next(track, event, error);
        const uint8_t* before = track.p;
        const bool ok = Next(track, event, error);
        processedTrackBytes += static_cast<uint64_t>(track.p - before);
        return ok;
    };
    auto reportScanProgress = [&](bool force) {
        if (!scanProgress) return true;
        if (!force && (processedRecords & 0x3fffu) != 0u) return true;
        return scanProgress(processedTrackBytes, totalTrackBytes,
                            processedRecords, scanProgressUser);
    };
    if (!reportScanProgress(true)) return false;
    std::priority_queue<HeapItem, std::vector<HeapItem>, Later> heap;
    for (uint32_t i = 0; i < tracks.size(); ++i) {
        RawEvent event;
        if (!nextEvent(i, event)) return false;
        if (event.valid) heap.push({event, i});
    }
    uint64_t tick = 0, frame = 0, remainder = 0, count = 0, noteOns = 0;
    uint64_t bucketSecond = UINT64_MAX, bucketEvents = 0, bucketNotes = 0;
    uint64_t groupFrame = UINT64_MAX, groupEvents = 0, groupNotes = 0;
    uint64_t groupExactDuplicates = 0, groupKeyDuplicates = 0;
    uint64_t groupRunExactDuplicates = 0;
    uint32_t groupGeneration = 0u;
    uint32_t noteRunGeneration = 0u;
    uint32_t previousNoteMessage = UINT32_MAX;
    std::vector<uint32_t> exactSeen;
    std::vector<uint32_t> keySeen;
    std::vector<uint32_t> noteRunExactSeen;
    const bool collectFrameRepetition = sink == nullptr;
    if (collectFrameRepetition) {
        exactSeen.resize(1u << 19u, 0u);
        keySeen.resize(1u << 12u, 0u);
        noteRunExactSeen.resize(1u << 19u, 0u);
    }
    auto finishFrame = [&]() {
        if (groupFrame == UINT64_MAX) return;
        if (groupEvents > info.peakEventsAtFrame) {
            info.peakEventsAtFrame = groupEvents;
            info.peakFrame = groupFrame;
        }
        info.peakNoteOnsAtFrame =
            (std::max)(info.peakNoteOnsAtFrame, groupNotes);
        info.peakExactDuplicateNoteOnsAtFrame = (std::max)(
            info.peakExactDuplicateNoteOnsAtFrame, groupExactDuplicates);
        info.peakKeyDuplicateNoteOnsAtFrame = (std::max)(
            info.peakKeyDuplicateNoteOnsAtFrame, groupKeyDuplicates);
        info.peakNoteRunExactDuplicatesAtFrame = (std::max)(
            info.peakNoteRunExactDuplicatesAtFrame,
            groupRunExactDuplicates);
        if (groupNotes != 0u) ++info.noteOnFrameCount;
    };
    uint32_t tempo = 500000, sequence = 0;
    const uint64_t denom = uint64_t(info.division) * 1000000ull;
    auto processMessage = [&](uint32_t message) {
        const uint64_t second = rate ? frame / rate : 0;
        if (second != bucketSecond) {
            if (bucketEvents > info.peakEventsPerSecond) {
                info.peakEventsPerSecond = bucketEvents;
                info.peakEventSecond = bucketSecond;
            }
            if (bucketNotes > info.peakNoteOnsPerSecond) {
                info.peakNoteOnsPerSecond = bucketNotes;
                info.peakNoteOnSecond = bucketSecond;
            }
            bucketSecond = second; bucketEvents = 0; bucketNotes = 0;
        }
        if (frame != groupFrame) {
            finishFrame();
            groupFrame = frame;
            groupEvents = 0;
            groupNotes = 0;
            groupExactDuplicates = 0;
            groupKeyDuplicates = 0;
            groupRunExactDuplicates = 0;
            previousNoteMessage = UINT32_MAX;
            if (collectFrameRepetition && ++groupGeneration == 0u) {
                std::fill(exactSeen.begin(), exactSeen.end(), 0u);
                std::fill(keySeen.begin(), keySeen.end(), 0u);
                groupGeneration = 1u;
            }
            if (collectFrameRepetition && ++noteRunGeneration == 0u) {
                std::fill(noteRunExactSeen.begin(),
                          noteRunExactSeen.end(), 0u);
                noteRunGeneration = 1u;
            }
        }
        PackedMidiEvent packed{frame, sequence++, message};
        if (sink && !sink(packed, user)) return false;
        ++count;
        ++bucketEvents; ++groupEvents;
        if ((message & 0xf0u) == 0x90u &&
            ((message >> 16) & 0x7fu) != 0u) {
            ++noteOns; ++bucketNotes;
            ++groupNotes;
            if (collectFrameRepetition) {
                const uint32_t channel = message & 0x0fu;
                const uint32_t note = (message >> 8u) & 0xffu;
                const uint32_t velocity = (message >> 16u) & 0x7fu;
                const uint32_t keyIdentity = (channel << 8u) | note;
                const uint32_t exactIdentity =
                    (keyIdentity << 7u) | velocity;
                if (keySeen[keyIdentity] == groupGeneration) {
                    ++groupKeyDuplicates;
                    ++info.keyDuplicateNoteOnCount;
                } else {
                    keySeen[keyIdentity] = groupGeneration;
                }
                if (exactSeen[exactIdentity] == groupGeneration) {
                    ++groupExactDuplicates;
                    ++info.exactDuplicateNoteOnCount;
                } else {
                    exactSeen[exactIdentity] = groupGeneration;
                }
                if (noteRunExactSeen[exactIdentity] == noteRunGeneration) {
                    ++groupRunExactDuplicates;
                    ++info.noteRunExactDuplicateCount;
                } else {
                    noteRunExactSeen[exactIdentity] = noteRunGeneration;
                }
                if (previousNoteMessage == message)
                    ++info.adjacentExactDuplicateNoteOnCount;
                previousNoteMessage = message;
            }
        } else if (collectFrameRepetition) {
            previousNoteMessage = UINT32_MAX;
            if (++noteRunGeneration == 0u) {
                std::fill(noteRunExactSeen.begin(),
                          noteRunExactSeen.end(), 0u);
                noteRunGeneration = 1u;
            }
        }
        return true;
    };
    while (!heap.empty()) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        const HeapItem item = heap.top(); heap.pop();
        if (scanProgress) {
            ++processedRecords;
            if (!reportScanProgress(false)) return false;
        }
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
            if (!processMessage(item.event.message)) return false;
        } else if (item.event.sysex) {
            bool accepted = true;
            (void)TranslateXGSystemExclusivePayload(
                item.event.sysexData, item.event.sysexSize,
                [&](uint32_t message) {
                    if (accepted) accepted = processMessage(message);
                });
            if (!accepted) return false;
        }
        RawEvent next;
        if (!nextEvent(item.track, next)) return false;
        if (next.valid) heap.push({next, item.track});
    }
    if (bucketEvents > info.peakEventsPerSecond) {
        info.peakEventsPerSecond = bucketEvents; info.peakEventSecond = bucketSecond;
    }
    if (bucketNotes > info.peakNoteOnsPerSecond) {
        info.peakNoteOnsPerSecond = bucketNotes; info.peakNoteOnSecond = bucketSecond;
    }
    finishFrame();
    info.eventCount = count;
    info.noteOnCount = noteOns;
    info.totalFrames = frame;
    if (!reportScanProgress(true)) return false;
    return true;
}
} // namespace

#if defined(_WIN32)
MappedMidiFile::MappedMidiFile() : file_(INVALID_HANDLE_VALUE), mapping_(nullptr), data_(nullptr), size_(0) {}
#else
MappedMidiFile::MappedMidiFile() : file_(-1), data_(nullptr), size_(0) {}
#endif
MappedMidiFile::~MappedMidiFile() { Close(); }
bool MappedMidiFile::Open(const wchar_t* path, std::string& error) {
    Close();
#if defined(_WIN32)
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
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    const std::string utf8 = converter.to_bytes(path);
    const int file = ::open(utf8.c_str(), O_RDONLY);
    if (file < 0) { error = "cannot open MIDI file"; return false; }
    struct stat status{};
    if (fstat(file, &status) != 0 || status.st_size <= 0) {
        ::close(file); error = "empty MIDI file"; return false;
    }
    void* mapping = mmap(nullptr, static_cast<size_t>(status.st_size),
                         PROT_READ, MAP_PRIVATE, file, 0);
    if (mapping == MAP_FAILED) {
        ::close(file); error = "cannot map MIDI file"; return false;
    }
    file_ = file;
    data_ = static_cast<const uint8_t*>(mapping);
    size_ = static_cast<uint64_t>(status.st_size);
#endif
    return true;
}
void MappedMidiFile::Close() {
#if defined(_WIN32)
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(file_));
    file_ = INVALID_HANDLE_VALUE; mapping_ = nullptr; data_ = nullptr; size_ = 0;
#else
    if (data_) munmap(const_cast<uint8_t*>(data_), static_cast<size_t>(size_));
    if (file_ >= 0) ::close(file_);
    file_ = -1; data_ = nullptr; size_ = 0;
#endif
}
bool MidiStreamDecoder::Scan(const MappedMidiFile& file, uint32_t rate,
                             MidiStreamInfo& info, std::string& error,
                             ScanProgress progress, void* progressUser,
                             std::atomic<bool>* cancel) const {
    info = {};
    return Run(file, rate, nullptr, nullptr, cancel, info, error,
               progress, progressUser);
}
bool MidiStreamDecoder::Decode(const MappedMidiFile& file, uint32_t rate, Sink sink,
                               void* user, std::atomic<bool>* cancel,
                               MidiStreamInfo* info, std::string& error) const {
    MidiStreamInfo local{};
    const bool ok = Run(file, rate, sink, user, cancel, local, error);
    if (info) *info = local;
    return ok;
}
namespace {
struct PTrack {
    Track t;
    const uint8_t* begin = nullptr;
    RawEvent pending;
    bool has = false;
    bool dropNotes = false;
};
struct WinEv { uint32_t off; uint32_t message; };
struct TempoEv { uint64_t tick; uint32_t tempo; };
struct WinWorker {
    size_t first = 0, last = 0;
    std::vector<WinEv> events;
    std::vector<TempoEv> tempos;
    uint64_t maxTick = 0;
    bool sawRecord = false, allDone = true, ok = true;
    std::string error;
};

// Consume every record with tick < b from the worker's tracks. Records are
// not interpreted beyond tempo / channel messages / SysEx translation.
void ParseWindow(std::vector<PTrack>& tracks, WinWorker& w, uint64_t a,
                 uint64_t b) {
    w.events.clear(); w.tempos.clear();
    w.sawRecord = false; w.allDone = true; w.ok = true; w.maxTick = 0;
    for (size_t i = w.first; i < w.last; ++i) {
        PTrack& pt = tracks[i];
        for (;;) {
            if (!pt.has) {
                if (pt.t.done) break;
                if (!Next(pt.t, pt.pending, w.error)) { w.ok = false; return; }
                if (!pt.pending.valid) break;  // track ended (done is set)
                pt.has = true;
            }
            const RawEvent& e = pt.pending;
            if (e.tick >= b) break;
            pt.has = false;
            w.sawRecord = true;
            if (e.tick > w.maxTick) w.maxTick = e.tick;
            const uint32_t off = static_cast<uint32_t>(e.tick - a);
            if (e.tempo) w.tempos.push_back({e.tick, e.tempo});
            else if (e.midi) {
                const uint32_t type = e.message & 0xf0u;
                if (!(pt.dropNotes && (type == 0x80u || type == 0x90u)))
                    w.events.push_back({off, e.message});
            }
            else if (e.sysex) {
                (void)TranslateXGSystemExclusivePayload(
                    e.sysexData, e.sysexSize,
                    [&](uint32_t m) { w.events.push_back({off, m}); });
            }
        }
        if (pt.has || !pt.t.done) w.allDone = false;
    }
}
} // namespace

namespace {
struct TrackMeasure {
    uint64_t endTick = 0;
    uint64_t noteOns = 0;
    std::vector<TempoEv> tempos;
    bool ok = true;
};

// Same record grammar as Next(), minus event construction and data-byte
// validation (Decode() reports those).
bool MeasureTrack(const Track& tr, TrackMeasure& out) {
    const uint8_t* p = tr.p;
    const uint8_t* end = tr.end;
    uint64_t tick = 0;
    uint8_t running = 0;
    while (p < end) {
        uint32_t delta = 0;
        if (!VLen(p, end, delta)) return false;
        tick += delta;
        if (p == end) return false;
        uint8_t status = *p;
        if (status < 0x80) {
            if (running < 0x80 || running >= 0xf0) return false;
            status = running;
        } else {
            ++p;
            running = status < 0xf0 ? status : 0;
        }
        if (status == 0xff) {
            if (p == end) return false;
            const uint8_t type = *p++;
            uint32_t length = 0;
            if (!VLen(p, end, length) || uint64_t(end - p) < length) return false;
            if (type == 0x51 && length == 3) {
                const uint32_t tempo = (uint32_t(p[0]) << 16) |
                                       (uint32_t(p[1]) << 8) | p[2];
                if (tempo) out.tempos.push_back({tick, tempo});
            }
            p += length;
            if (type == 0x2f) break;
        } else if (status == 0xf0 || status == 0xf7) {
            uint32_t length = 0;
            if (!VLen(p, end, length) || uint64_t(end - p) < length) return false;
            p += length;
        } else if (status >= 0xf0) {
            static const uint8_t lengths[16] = {0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0};
            const uint8_t n = lengths[status & 15];
            if (uint64_t(end - p) < n) return false;
            p += n;
        } else {
            const uint8_t kind = status & 0xf0;
            const uint8_t bytes = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
            if (uint64_t(end - p) < bytes) return false;
            if (kind == 0x90 && p[1] != 0) ++out.noteOns;
            p += bytes;
        }
    }
    out.endTick = tick;
    return true;
}
} // namespace

bool ParallelMidiDecoder::MeasureDuration(const MappedMidiFile& file,
                                          uint32_t rate, uint64_t& totalFrames,
                                          std::string& error,
                                          std::atomic<bool>* cancel,
                                          uint32_t threads,
                                          std::vector<uint64_t>* noteOnsPerTrack) {
    MidiStreamInfo header{};
    std::vector<Track> tracks;
    if (!Tracks(file, header, tracks, error)) return false;
    std::vector<TrackMeasure> measures(tracks.size());
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    size_t n = threads ? threads
                       : (std::min)(32u, (std::max)(1u, std::thread::hardware_concurrency()));
    n = (std::max<size_t>)(1, (std::min)(n, tracks.size()));
    auto work = [&] {
        for (;;) {
            if (failed.load(std::memory_order_relaxed) ||
                (cancel && cancel->load(std::memory_order_relaxed))) return;
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= tracks.size()) return;
            if (!MeasureTrack(tracks[i], measures[i]))
                failed.store(true, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> pool;
    for (size_t k = 1; k < n; ++k) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    if (cancel && cancel->load(std::memory_order_relaxed)) return false;
    if (failed.load()) { error = "cannot measure MIDI duration"; return false; }

    if (noteOnsPerTrack) {
        noteOnsPerTrack->clear();
        for (const TrackMeasure& m : measures) noteOnsPerTrack->push_back(m.noteOns);
    }
    uint64_t maxTick = 0;
    std::vector<TempoEv> tempos;   // track order, then in-track order
    for (const TrackMeasure& m : measures) {
        maxTick = (std::max)(maxTick, m.endTick);
        tempos.insert(tempos.end(), m.tempos.begin(), m.tempos.end());
    }
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const TempoEv& x, const TempoEv& y) { return x.tick < y.tick; });

    // Piecewise exact floor accumulation, in <=2^28-tick steps so every
    // product stays inside uint64_t (same bound the VLQ cap gives Run()).
    const uint64_t denom = uint64_t(header.division) * 1000000ull;
    uint64_t tempo = 500000u, frame = 0, rem = 0, tick = 0;
    auto advanceTo = [&](uint64_t target) {
        const uint64_t num = tempo * rate;
        const uint64_t q = num / denom, rest = num % denom;
        while (tick < target) {
            const uint64_t d = (std::min<uint64_t>)(target - tick, 0x0fffffffull);
            frame += d * q;
            const uint64_t fraction = rem + d * rest;
            frame += fraction / denom;
            rem = fraction % denom;
            tick += d;
        }
    };
    for (const TempoEv& t : tempos) {
        if (t.tick > maxTick) break;
        advanceTo(t.tick);
        tempo = t.tempo;
    }
    advanceTo(maxTick);
    totalFrames = frame;
    return true;
}

bool ParallelMidiDecoder::NoteTracks(const MappedMidiFile& file,
                                     std::vector<uint32_t>& out,
                                     std::string& error) {
    MidiStreamInfo header{};
    std::vector<Track> tracks;
    if (!Tracks(file, header, tracks, error)) return false;
    out.clear();
    for (uint32_t i = 0; i < tracks.size(); ++i) {
        RawEvent e;
        for (;;) {
            if (!Next(tracks[i], e, error)) return false;
            if (!e.valid) break;
            if (e.midi && (e.message & 0xf0u) == 0x90u && ((e.message >> 16) & 0x7fu)) {
                out.push_back(i);
                break;
            }
        }
    }
    return true;
}

bool ParallelMidiDecoder::Decode(const MappedMidiFile& file, uint32_t rate,
                                 MidiStreamDecoder::Sink sink, void* user,
                                 std::atomic<bool>* cancel,
                                 MidiStreamInfo* info, std::string& error,
                                 std::atomic<uint64_t>* bytesDone,
                                 std::atomic<uint64_t>* totalBytes,
                                 uint32_t threads) const {
    MidiStreamInfo local{};
    std::vector<Track> raw;
    if (!Tracks(file, local, raw, error)) return false;
    std::vector<PTrack> tracks(raw.size());
    uint64_t total = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        tracks[i].t = raw[i];
        tracks[i].begin = raw[i].p;
        total += static_cast<uint64_t>(raw[i].end - raw[i].p);
    }
    if (!keepNoteTracks.empty()) {
        for (PTrack& pt : tracks) pt.dropNotes = true;
        for (uint32_t k : keepNoteTracks)
            if (k < tracks.size()) tracks[k].dropNotes = false;
    }
    if (totalBytes) totalBytes->store(total, std::memory_order_relaxed);

    size_t n = threads ? threads
                       : (std::min)(8u, (std::max)(1u, std::thread::hardware_concurrency() / 2u));
    n = (std::max<size_t>)(1, (std::min)(n, tracks.size()));
    std::vector<WinWorker> workers(n);
    {   // contiguous track ranges balanced by bytes keep (track, order) order
        size_t idx = 0; uint64_t acc = 0;
        for (size_t k = 0; k < n; ++k) {
            workers[k].first = idx;
            const uint64_t target = total / n * (k + 1);
            while (idx < tracks.size() && (acc < target || k + 1 == n)) {
                acc += static_cast<uint64_t>(tracks[idx].t.end - tracks[idx].t.p);
                ++idx;
            }
            workers[k].last = idx;
        }
    }

    std::mutex m;
    std::condition_variable cvStart, cvDone;
    uint64_t generation = 0, winA = 0, winB = 0;
    size_t outstanding = 0;
    bool quit = false;
    std::vector<std::thread> pool;
    struct Join {
        std::mutex& m; std::condition_variable& cv; bool& quit;
        std::vector<std::thread>& pool;
        ~Join() {
            { std::lock_guard<std::mutex> lk(m); quit = true; }
            cv.notify_all();
            for (auto& t : pool) t.join();
        }
    } join{m, cvStart, quit, pool};
    for (size_t k = 1; k < n; ++k) {
        pool.emplace_back([&, k] {
            uint64_t seen = 0;
            for (;;) {
                uint64_t a, b;
                {
                    std::unique_lock<std::mutex> lk(m);
                    cvStart.wait(lk, [&] { return quit || generation != seen; });
                    if (quit) return;
                    seen = generation; a = winA; b = winB;
                }
                ParseWindow(tracks, workers[k], a, b);
                std::lock_guard<std::mutex> lk(m);
                if (--outstanding == 0) cvDone.notify_one();
            }
        });
    }
    auto runWindow = [&](uint64_t a, uint64_t b) {
        { std::lock_guard<std::mutex> lk(m);
          winA = a; winB = b; outstanding = n - 1; ++generation; }
        cvStart.notify_all();
        ParseWindow(tracks, workers[0], a, b);
        std::unique_lock<std::mutex> lk(m);
        cvDone.wait(lk, [&] { return outstanding == 0; });
    };

    // Exact tick->frame recurrence of Run(): per-tick stepping is the same
    // floor accumulation as the per-delta form.
    const uint64_t denom = uint64_t(local.division) * 1000000ull;
    uint64_t q = 0, rest = 0;
    auto setTempo = [&](uint32_t t) {
        const uint64_t num = uint64_t(t) * rate;
        q = num / denom; rest = num % denom;
    };
    setTempo(500000u);
    uint64_t frame = 0, rem = 0, count = 0, noteOns = 0, totalFrames = 0;
    uint32_t sequence = 0;
    const uint64_t kMaxWindow = 65536u;
    uint64_t span = (std::min)(kMaxWindow,
                               (std::max<uint64_t>)(64u, local.division / 2u));
    std::vector<uint64_t> frames;
    std::vector<uint32_t> cnt, sorted;
    std::vector<TempoEv> tempos;
    for (uint64_t a = 0, b = 0, windowIndex = 0;; a = b, ++windowIndex) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        b = a + span;
        runWindow(a, b);
        bool allDone = true, sawAny = false;
        uint64_t wmax = 0, nev = 0;
        tempos.clear();
        for (WinWorker& w : workers) {
            if (!w.ok) { error = w.error; return false; }
            allDone = allDone && w.allDone;
            nev += w.events.size();
            if (w.sawRecord) { sawAny = true; wmax = (std::max)(wmax, w.maxTick); }
            tempos.insert(tempos.end(), w.tempos.begin(), w.tempos.end());
        }
        std::stable_sort(tempos.begin(), tempos.end(),
                         [](const TempoEv& x, const TempoEv& y) { return x.tick < y.tick; });

        if (!sawAny) {
            const uint64_t fraction = rem + span * rest;
            frame += span * q + fraction / denom;
            rem = fraction % denom;
        } else {
            frames.resize(span);
            size_t tp = 0;
            for (uint64_t i = 0; i < span; ++i) {
                frames[i] = frame;
                while (tp < tempos.size() && tempos[tp].tick == a + i)
                    setTempo(tempos[tp++].tempo);
                rem += rest; frame += q;
                if (rem >= denom) { rem -= denom; ++frame; }
            }
            totalFrames = (std::max)(totalFrames, frames[wmax - a]);

            cnt.assign(span + 1, 0u);
            for (const WinWorker& w : workers)
                for (const WinEv& e : w.events) ++cnt[e.off + 1];
            for (uint64_t i = 0; i < span; ++i) cnt[i + 1] += cnt[i];
            sorted.resize(nev);
            for (const WinWorker& w : workers)
                for (const WinEv& e : w.events) sorted[cnt[e.off]++] = e.message;
            size_t pos = 0;
            for (uint64_t off = 0; off < span; ++off) {
                const size_t end = cnt[off];
                for (; pos < end; ++pos) {
                    const uint32_t message = sorted[pos];
                    PackedMidiEvent packed{frames[off], sequence++, message};
                    if (sink && !sink(packed, user)) return false;
                    ++count;
                    if ((message & 0xf0u) == 0x90u && ((message >> 16) & 0x7fu))
                        ++noteOns;
                }
            }
        }
        // Keep windows around 2M events: big enough to amortise the barrier,
        // small enough that the sort buffers stay modest.
        if (nev > (4u << 20) && span > 64u) span >>= 1;
        else if (nev < (1u << 19) && span < kMaxWindow) span <<= 1;
        if (bytesDone && (windowIndex & 7u) == 0u) {
            uint64_t done = 0;
            for (const PTrack& pt : tracks)
                done += static_cast<uint64_t>(pt.t.p - pt.begin);
            bytesDone->store(done, std::memory_order_relaxed);
        }
        if (allDone) break;
    }
    if (bytesDone) bytesDone->store(total, std::memory_order_relaxed);
    local.eventCount = count;
    local.noteOnCount = noteOns;
    local.totalFrames = totalFrames;
    if (info) *info = local;
    return true;
}

ParsedEventRing::ParsedEventRing(uint32_t mb) : events_(nullptr), capacity_(0), mask_(0), head_(0), tail_(0) {
    uint64_t slots = (uint64_t(mb ? mb : 1) << 20) / sizeof(PackedMidiEvent);
    uint64_t power = 1; while ((power << 1) <= slots) power <<= 1;
    capacity_ = power; mask_ = power - 1;
    events_ = static_cast<PackedMidiEvent*>(_aligned_malloc(
        size_t(power * sizeof(PackedMidiEvent)), 64u));
}
ParsedEventRing::~ParsedEventRing() { _aligned_free(events_); }
bool ParsedEventRing::Push(const PackedMidiEvent& event, const std::atomic<bool>& cancel) {
    while (!cancel.load(std::memory_order_relaxed)) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) < capacity_) {
            events_[h & mask_] = event;
            head_.store(h + 1, std::memory_order_release); return true;
        }
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
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
