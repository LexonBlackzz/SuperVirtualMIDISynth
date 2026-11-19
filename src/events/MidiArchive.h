#ifndef SVMS_MIDI_ARCHIVE_H
#define SVMS_MIDI_ARCHIVE_H

#include "events/ByteSource.h"
#include <memory>
#include <string>

namespace svms {

enum class MidiLayer { Midi, Xz, Gzip, Zstd, Zip, SevenZip, Rar, Unknown };

// What the first bytes of a file or stream say it is.
MidiLayer SniffLayer(const uint8_t* head, size_t n);

// Opens a compressed or archived MIDI as a stream positioned at its MThd.
// Returns null with an empty error when the file is a plain MIDI (or not
// recognised), so the caller maps it as before; null with an error when the
// file cannot be read as a MIDI. `format` names the layers, outermost first
// ("xz", "zip+xz").
std::unique_ptr<ByteSource> OpenMidiLayers(const wchar_t* path, std::string& format,
                                           std::string& error);

// Reads a few bytes ahead to look at the next layer, then replays them.
class PeekSource : public ByteSource {
public:
    explicit PeekSource(std::unique_ptr<ByteSource> in) : in_(std::move(in)) {}
    bool Peek(size_t n, std::string& error);   // fills Head() with up to n bytes
    const uint8_t* Head() const { return head_; }
    size_t HeadBytes() const { return headLen_; }
    bool Read(uint8_t* dst, size_t want, size_t& got, std::string& error) override;
private:
    std::unique_ptr<ByteSource> in_;
    uint8_t head_[16] = {};
    size_t headLen_ = 0, headPos_ = 0;
};

// .xz: one or more concatenated streams (stream padding included).
class XzSource : public ByteSource {
public:
    explicit XzSource(std::unique_ptr<ByteSource> in);
    ~XzSource() override;
    bool Read(uint8_t* dst, size_t want, size_t& got, std::string& error) override;
private:
    struct State;
    std::unique_ptr<ByteSource> in_;
    std::unique_ptr<State> state_;
};

// .gz: one or more members; CRC32 and length of each are checked.
class GzipSource : public ByteSource {
public:
    explicit GzipSource(std::unique_ptr<ByteSource> in);
    ~GzipSource() override;
    bool Read(uint8_t* dst, size_t want, size_t& got, std::string& error) override;
private:
    struct State;
    std::unique_ptr<ByteSource> in_;
    std::unique_ptr<State> state_;
};

// .zst: one or more frames, long-distance windows included.
class ZstdSource : public ByteSource {
public:
    explicit ZstdSource(std::unique_ptr<ByteSource> in);
    ~ZstdSource() override;
    bool Read(uint8_t* dst, size_t want, size_t& got, std::string& error) override;
private:
    struct State;
    std::unique_ptr<ByteSource> in_;
    std::unique_ptr<State> state_;
};

} // namespace svms
#endif
