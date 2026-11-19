#include "events/MidiChunkStore.h"
#include "events/ByteSource.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#include <fcntl.h>
#include <locale>
#include <unistd.h>
#endif
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <new>
#include <thread>

namespace svms {

// ---- byte sources ------------------------------------------------------------

FileSource::~FileSource() {
#if defined(_WIN32)
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
#else
    if (file_ >= 0) ::close(file_);
#endif
}
bool FileSource::Open(const wchar_t* path, std::string& error) {
#if defined(_WIN32)
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = "cannot open MIDI file"; return false; }
    file_ = f;
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    file_ = ::open(converter.to_bytes(path).c_str(), O_RDONLY);
    if (file_ < 0) { error = "cannot open MIDI file"; return false; }
#endif
    return true;
}
bool FileSource::Read(uint8_t* dst, size_t want, size_t& got, std::string& error) {
    got = 0;
#if defined(_WIN32)
    while (got < want) {
        const DWORD ask = static_cast<DWORD>((std::min<size_t>)(want - got, 1u << 30));
        DWORD n = 0;
        if (!ReadFile(static_cast<HANDLE>(file_), dst + got, ask, &n, nullptr)) {
            error = "cannot read MIDI file"; return false;
        }
        if (n == 0) break;
        got += n;
    }
#else
    while (got < want) {
        const ssize_t n = ::read(file_, dst + got, (std::min<size_t>)(want - got, 1u << 30));
        if (n < 0) { error = "cannot read MIDI file"; return false; }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
#endif
    return true;
}
bool MemorySource::Read(uint8_t* dst, size_t want, size_t& got, std::string&) {
    got = (std::min)(want, n_);
    if (got) std::memcpy(dst, p_, got);
    p_ += got; n_ -= got;
    return true;
}

// ---- chunking ----------------------------------------------------------------

namespace {
uint64_t Mix(uint64_t h, uint64_t w) {
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 32);
}
uint64_t HashBytes(const uint8_t* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h = Mix(h, w); }
    uint64_t t = 0;
    for (; i < n; ++i) t = (t << 8) | p[i];
    return Mix(h, t);
}
struct GearTable {
    uint64_t v[256];
    GearTable() {
        uint64_t s = 0x1234567;
        for (uint64_t& g : v) {
            s += 0x9E3779B97F4A7C15ull;
            uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            g = z ^ (z >> 31);
        }
    }
};
const GearTable& Gear() { static const GearTable table; return table; }
uint32_t BE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
bool ReadExact(ByteSource& src, uint8_t* dst, size_t n, size_t& got, std::string& error) {
    got = 0;
    while (got < n) {
        size_t step = 0;
        if (!src.Read(dst + got, n - got, step, error)) return false;
        if (step == 0) break;
        got += step;
    }
    return true;
}
// Reads and drops n bytes; got says how many the stream had.
bool Skip(ByteSource& src, uint64_t n, uint64_t& got, std::string& error) {
    uint8_t scratch[1 << 14];
    got = 0;
    while (got < n) {
        size_t step = 0;
        const size_t want = static_cast<size_t>((std::min<uint64_t>)(n - got, sizeof(scratch)));
        if (!src.Read(scratch, want, step, error)) return false;
        if (step == 0) break;
        got += step;
    }
    return true;
}
} // namespace

// The bytes of one track, in a window that slides over the stream. The
// window always holds the current chunk from its start, so a chunk is
// contiguous wherever it is cut.
struct TrackFeed {
    ByteSource* src = nullptr;       // null: the whole track is in `buf`
    uint8_t* buf = nullptr;
    size_t cap = 0;
    size_t start = 0, pos = 0, end = 0;
    uint64_t left = 0;               // track bytes not yet read into the window
    uint64_t* counter = nullptr;     // bytes read from the stream
    std::vector<uint8_t>* own = nullptr;
    bool truncated = false, failed = false;
    std::string* error = nullptr;

    // Makes `need` bytes from `pos` readable; false when the track has fewer.
    bool More(size_t need) {
        if (end - pos >= need) return true;
        if (!src || left == 0) return false;
        if (start > 0) {
            std::memmove(buf, buf + start, end - start);
            pos -= start; end -= start; start = 0;
        }
        if (cap - pos < need) {
            own->resize((std::max)(need + pos, own->size() * 2));
            buf = own->data(); cap = own->size();
        }
        while (end - pos < need && left) {
            const size_t want = static_cast<size_t>((std::min<uint64_t>)(left, cap - end));
            size_t got = 0;
            if (!src->Read(buf + end, want, got, *error)) { failed = true; return false; }
            if (got == 0) { truncated = true; return false; }
            end += got; left -= got; *counter += got;
        }
        return end - pos >= need;
    }
    // Reads the rest of the track into the window.
    bool All() { return More(end - pos + static_cast<size_t>(left)) || left == 0; }
    // Reads and drops the rest of the track.
    bool Drain() {
        if (!src || left == 0) return true;
        uint64_t got = 0;
        if (!Skip(*src, left, got, *error)) { failed = true; return false; }
        *counter += got;
        if (got != left) { truncated = true; return false; }
        left = 0;
        return true;
    }
};

class MidiTrackChunker {
public:
    // Cuts one track into chunks and appends their ids to refs. False on a
    // read error (feed.failed), a short stream (feed.truncated) or when the
    // store is full (error set).
    static bool Run(MidiChunkStore& store, TrackFeed& f, const MidiChunkParams& p,
                    std::vector<uint32_t>& refs, std::string& error);
};

bool MidiChunkStore::Intern(const uint8_t* p, uint32_t n, uint32_t& id, std::string& error) {
    const uint64_t h = HashBytes(p, n);
    const uint32_t shardIndex = static_cast<uint32_t>(h >> (64 - kShardBits));
    Shard& s = *shards_[shardIndex];
    std::lock_guard<std::mutex> lock(s.lock);
    auto range = s.index.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) {
        const MidiChunk& c = s.chunks[it->second];
        if (c.size == n && std::memcmp(c.data, p, n) == 0) {
            id = (shardIndex << kIndexBits) | it->second;
            return true;
        }
    }
    if (s.chunks.size() > kIndexMask) { error = "MIDI has too many distinct chunks"; return false; }
    uint8_t* dst;
    // A failed allocation must leave the shard as it was: another worker may
    // still intern into it before the build stops.
    if (n > (1u << 20)) {
        std::unique_ptr<uint8_t[]> block(new uint8_t[n]);
        s.blocks.push_back(std::move(block));
        s.arena += n;
        dst = s.blocks.back().get();
    } else {
        if (s.smallUsed + n > s.smallSize) {
            // Blocks grow 64 KB -> 4 MB per shard, so a small MIDI does not
            // pay for 64 big blocks and the unused tail of a full one stays small.
            const size_t size = std::max<size_t>(n, s.smallSize ? std::min<size_t>(s.smallSize * 2, 4u << 20)
                                                                : size_t(64u << 10));
            std::unique_ptr<uint8_t[]> block(new uint8_t[size]);
            s.blocks.push_back(std::move(block));
            s.arena += size;
            s.smallSize = size;
            s.smallBlock = s.blocks.back().get();
            s.smallUsed = 0;
        }
        dst = s.smallBlock + s.smallUsed;
        s.smallUsed += n;
    }
    std::memcpy(dst, p, n);
    const uint32_t local = static_cast<uint32_t>(s.chunks.size());
    s.chunks.push_back({dst, n});
    s.index.emplace(h, local);
    s.bytes += n;
    id = (shardIndex << kIndexBits) | local;
    return true;
}

namespace {
// Length of the event at f.pos, parsed exactly as the decoders parse it, or 0
// when the decoders would stop with an error inside it (or the track ends).
size_t EventLength(TrackFeed& f, uint8_t& running, bool& endOfTrack) {
    size_t k = 0;
    auto vlq = [&](uint32_t& value) {
        value = 0;
        for (unsigned i = 0; i != 4; ++i) {
            if (!f.More(k + 1)) return false;
            const uint8_t c = f.buf[f.pos + k++];
            value = (value << 7) | (c & 0x7f);
            if ((c & 0x80) == 0) return true;
        }
        return false;
    };
    uint32_t delta = 0;
    if (!vlq(delta)) return 0;
    if (!f.More(k + 1)) return 0;
    uint8_t status = f.buf[f.pos + k];
    if (status < 0x80) {
        if (running < 0x80 || running >= 0xf0) return 0;
        status = running;
    } else {
        ++k;
        running = status < 0xf0 ? status : 0;
    }
    if (status == 0xff) {
        if (!f.More(k + 1)) return 0;
        const uint8_t type = f.buf[f.pos + k++];
        uint32_t length = 0;
        if (!vlq(length) || !f.More(k + length)) return 0;
        k += length;
        endOfTrack = type == 0x2f;
        return k;
    }
    if (status == 0xf0 || status == 0xf7) {
        uint32_t length = 0;
        if (!vlq(length) || !f.More(k + length)) return 0;
        return k + length;
    }
    if (status >= 0xf0) {
        static const uint8_t lengths[16] = {0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0};
        const uint8_t n = lengths[status & 15];
        return f.More(k + n) ? k + n : 0;
    }
    const uint8_t kind = status & 0xf0;
    const size_t n = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
    return f.More(k + n) ? k + n : 0;
}
} // namespace

bool MidiTrackChunker::Run(MidiChunkStore& store, TrackFeed& f, const MidiChunkParams& p,
                           std::vector<uint32_t>& refs, std::string& error) {
    const GearTable& gear = Gear();
    uint8_t running = 0;
    uint64_t h = 0;
    auto emit = [&](size_t to) {
        if (to == f.start) return true;
        uint32_t id = 0;
        if (!store.Intern(f.buf + f.start, static_cast<uint32_t>(to - f.start), id, error))
            return false;
        refs.push_back(id);
        f.start = to;
        return true;
    };
    for (;;) {
        if (!f.More(1)) {   // end of track (or of the stream)
            if (f.failed || f.truncated) return false;
            return emit(f.pos);
        }
        bool endOfTrack = false;
        const size_t length = EventLength(f, running, endOfTrack);
        if (f.failed || f.truncated) return false;
        if (length == 0) {
            // The decoders stop with an error in here: keep every byte left.
            if (!f.All()) return false;
            f.pos = f.end;
            return emit(f.pos);
        }
        for (size_t i = 0; i < length; ++i) h = (h << 1) + gear.v[f.buf[f.pos + i]];
        f.pos += length;
        const size_t size = f.pos - f.start;
        if (endOfTrack) {
            // Nothing after End-of-Track is ever read: drop it.
            if (!emit(f.pos)) return false;
            return f.Drain();
        }
        if ((size >= p.minChunk && (h & p.mask) == 0) || size >= p.maxChunk) {
            if (!emit(f.pos)) return false;
            h = 0;
        }
    }
}

// ---- the store ---------------------------------------------------------------

MidiChunkStore::MidiChunkStore() {
    for (auto& s : shards_) s.reset(new Shard());
}
MidiChunkStore::~MidiChunkStore() = default;

uint64_t MidiChunkStore::ArenaBytes() const {
    uint64_t n = 0;
    for (const auto& s : shards_) n += s->arena;
    return n;
}

uint64_t MidiChunkStore::StoredBytes() const {
    uint64_t n = 0;
    for (const auto& s : shards_) n += s->bytes;
    return n;
}

bool MidiChunkStore::Build(ByteSource& src, const MidiChunkParams& params, std::string& error) {
    try {
        size_t got = 0;
        if (!ReadExact(src, head_, sizeof(head_), got, error)) return false;
        headBytes_ = got; midiBytes_ = got;
        if (got < sizeof(head_) || std::memcmp(head_, "MThd", 4) != 0) return true;
        const uint32_t headerLength = BE32(head_ + 4);
        if (headerLength < 6) return true;
        uint64_t skipped = 0;
        if (!Skip(src, headerLength - 6u, skipped, error)) return false;
        midiBytes_ += skipped;
        if (skipped != headerLength - 6u) return true;
        headerComplete_ = true;

        return ReadTracks(src, params, error);
    } catch (const std::bad_alloc&) {
        error = "out of memory while loading the MIDI";
        return false;
    }
}

// The calling thread reads the stream. A track up to pooledTrackMax is copied
// out and chunked by a worker; a bigger one is chunked here as it streams.
// Each track's chunk list goes to its own slot, so track order never depends
// on which worker finishes first.
bool MidiChunkStore::ReadTracks(ByteSource& src, const MidiChunkParams& params,
                                std::string& error) {
    const uint32_t declared = (uint32_t(head_[10]) << 8) | head_[11];
    const uint32_t threads = params.threads
        ? params.threads
        : (std::min)(16u, (std::max)(1u, std::thread::hardware_concurrency()));
    const uint32_t workerCount = threads > 1 ? threads - 1 : 0;

    struct Job { size_t slot; std::vector<uint8_t> bytes; };
    std::mutex m;
    std::condition_variable wake, room;
    std::deque<Job> queue;
    std::deque<std::vector<uint32_t>> slots;   // stable under push_back
    uint64_t inFlight = 0;
    bool closing = false, failed = false;
    std::string workerError;

    auto worker = [&] {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m);
                wake.wait(lock, [&] { return closing || !queue.empty(); });
                if (queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            std::vector<uint32_t> refs;
            std::string jobError;
            bool ok = true;
            try {
                TrackFeed f;
                uint64_t unused = 0;
                f.buf = job.bytes.data(); f.cap = f.end = job.bytes.size();
                f.counter = &unused; f.error = &jobError;
                ok = MidiTrackChunker::Run(*this, f, params, refs, jobError);
                refs.shrink_to_fit();
            } catch (const std::bad_alloc&) {
                ok = false; jobError = "out of memory while loading the MIDI";
            }
            std::lock_guard<std::mutex> lock(m);
            inFlight -= job.bytes.size();
            if (ok) slots[job.slot] = std::move(refs);
            else if (!failed) { failed = true; workerError = jobError; }
            room.notify_one();
        }
    };
    std::vector<std::thread> pool;
    struct Joiner {
        std::mutex& m; std::condition_variable& wake; bool& closing;
        std::vector<std::thread>& pool;
        ~Joiner() {
            { std::lock_guard<std::mutex> lock(m); closing = true; }
            wake.notify_all();
            for (auto& t : pool) t.join();
        }
    } joiner{m, wake, closing, pool};
    for (uint32_t i = 0; i < workerCount; ++i) pool.emplace_back(worker);
    workersUsed_ = workerCount ? workerCount : 1;

    std::vector<uint8_t> window(4u << 20);
    bool readOk = true;
    while (slots.size() < declared) {
        {
            std::lock_guard<std::mutex> lock(m);
            if (failed) break;
        }
        uint8_t chunk[8];
        size_t got = 0;
        if (!ReadExact(src, chunk, 8, got, error)) { readOk = false; break; }
        midiBytes_ += got;
        if (got == 0) break;
        if (got < 8) { end_ = End::TruncatedChunk; break; }
        const uint32_t length = BE32(chunk + 4);
        if (std::memcmp(chunk, "MTrk", 4) != 0) {
            uint64_t skipped = 0;
            if (!Skip(src, length, skipped, error)) { readOk = false; break; }
            midiBytes_ += skipped;
            if (skipped != length) { end_ = End::TruncatedTrack; break; }
            continue;
        }
        if (workerCount && length <= params.pooledTrackMax) {
            std::vector<uint8_t> bytes(length);
            if (!ReadExact(src, bytes.data(), length, got, error)) { readOk = false; break; }
            midiBytes_ += got;
            if (got != length) { end_ = End::TruncatedTrack; break; }
            std::unique_lock<std::mutex> lock(m);
            room.wait(lock, [&] {
                return failed || inFlight == 0 || inFlight + length <= params.inFlightMax;
            });
            if (failed) break;
            inFlight += length;
            slots.emplace_back();
            trackSize_.push_back(length);
            queue.push_back({slots.size() - 1, std::move(bytes)});
            wake.notify_one();
            continue;
        }
        TrackFeed f;
        f.src = &src; f.own = &window; f.buf = window.data(); f.cap = window.size();
        f.left = length; f.counter = &midiBytes_; f.error = &error;
        std::vector<uint32_t> refs;
        if (!MidiTrackChunker::Run(*this, f, params, refs, error)) {
            if (f.failed || !error.empty()) { readOk = false; break; }
            end_ = End::TruncatedTrack;
            break;
        }
        refs.shrink_to_fit();
        std::lock_guard<std::mutex> lock(m);
        slots.push_back(std::move(refs));
        trackSize_.push_back(length);
    }
    {   // let the workers finish what is queued
        std::unique_lock<std::mutex> lock(m);
        closing = true;
        wake.notify_all();
    }
    for (auto& t : pool) t.join();
    pool.clear();
    if (!readOk) return false;
    if (failed) { error = workerError; return false; }
    // Read the stream to its end even past the last track: the xz index and
    // footer, the zip entry CRC and the zstd frame end are only checked there.
    uint64_t rest = 0;
    if (!Skip(src, UINT64_MAX, rest, error)) return false;
    midiBytes_ += rest;
    refs_.assign(std::make_move_iterator(slots.begin()), std::make_move_iterator(slots.end()));
    return true;
}

} // namespace svms
