#ifndef SVMS_MIDI_CHUNK_STORE_H
#define SVMS_MIDI_CHUNK_STORE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace svms {

class ByteSource;

struct MidiChunk {
    const uint8_t* data;
    uint32_t size;
};

struct MidiChunkParams {
    uint32_t minChunk = 64;
    uint32_t maxChunk = 1024;
    uint32_t mask = 0xFF;
    uint32_t pooledTrackMax = 64u << 20;
    uint64_t inFlightMax = 256ull << 20;
    uint32_t threads = 0;   // 0 = hardware threads, capped at 16
};

// The track bytes of a MIDI read from a stream, kept in chunks. Each track is
// cut after complete events at content-defined points (a gear rolling hash),
// so a passage that repeats is cut the same way wherever it appears, and each
// distinct chunk is stored once. A track is the list of its chunk ids; the
// decoders walk those chunks and see exactly the bytes of the original track.
class MidiChunkStore {
public:
    // How the outer chunk sequence ended, so the decoders can report the same
    // error a mapped file gives.
    enum class End { Clean, TruncatedChunk, TruncatedTrack };

    MidiChunkStore();
    ~MidiChunkStore();
    MidiChunkStore(const MidiChunkStore&) = delete;
    MidiChunkStore& operator=(const MidiChunkStore&) = delete;

    // Fails only on a read error of the source. A stream that is not a MIDI
    // builds an empty store; the decoders then report it as they do today.
    bool Build(ByteSource& src, const MidiChunkParams& params, std::string& error);

    // The first min(14, stream size) bytes of the stream.
    const uint8_t* Header() const { return head_; }
    size_t HeaderBytes() const { return headBytes_; }
    // True when the stream held the whole MThd chunk its length field names.
    bool HeaderComplete() const { return headerComplete_; }
    End EndStatus() const { return end_; }

    uint32_t TrackCount() const { return static_cast<uint32_t>(refs_.size()); }
    const uint32_t* TrackRefs(uint32_t track, uint32_t& count) const {
        count = static_cast<uint32_t>(refs_[track].size());
        return refs_[track].data();
    }
    uint64_t TrackSize(uint32_t track) const { return trackSize_[track]; }
    MidiChunk Chunk(uint32_t id) const {
        const Shard& s = *shards_[id >> kIndexBits];
        return s.chunks[id & kIndexMask];
    }

    uint64_t StoredBytes() const;     // distinct chunk bytes
    uint64_t MidiBytes() const { return midiBytes_; }   // bytes read from the stream
    uint32_t WorkersUsed() const { return workersUsed_; }

private:
    static constexpr unsigned kShardBits = 6;
    static constexpr unsigned kIndexBits = 32 - kShardBits;
    static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1u;
    struct Shard {
        std::mutex lock;
        std::unordered_multimap<uint64_t, uint32_t> index;
        std::vector<MidiChunk> chunks;
        std::vector<std::unique_ptr<uint8_t[]>> blocks;
        uint8_t* smallBlock = nullptr;    // current block for chunks up to 1 MB
        size_t smallUsed = 0, smallSize = 0;
        uint64_t bytes = 0;
    };
    friend class MidiTrackChunker;
    bool Intern(const uint8_t* p, uint32_t n, uint32_t& id, std::string& error);
    bool ReadTracks(ByteSource& src, const MidiChunkParams& params, std::string& error);

    uint8_t head_[14] = {};
    size_t headBytes_ = 0;
    bool headerComplete_ = false;
    End end_ = End::Clean;
    std::vector<std::vector<uint32_t>> refs_;
    std::vector<uint64_t> trackSize_;
    std::unique_ptr<Shard> shards_[1u << kShardBits];
    uint64_t midiBytes_ = 0;
    uint32_t workersUsed_ = 1;
};

} // namespace svms
#endif
