#include "events/MidiArchive.h"

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include "lzma/7zCrc.h"
#include "lzma/Alloc.h"
#include "lzma/Xz.h"
#include "lzma/XzCrc64.h"
}

namespace svms {

namespace {
constexpr unsigned kMaxLayers = 4;

void InitCrcTables() {
    static const bool done = [] {
        CrcGenerateTable();
        Crc64GenerateTable();
        return true;
    }();
    (void)done;
}
} // namespace

MidiLayer SniffLayer(const uint8_t* h, size_t n) {
    auto is = [&](const char* magic, size_t len) {
        return n >= len && std::memcmp(h, magic, len) == 0;
    };
    if (is("MThd", 4)) return MidiLayer::Midi;
    if (is("\xFD" "7zXZ\0", 6)) return MidiLayer::Xz;
    if (is("7z\xBC\xAF\x27\x1C", 6)) return MidiLayer::SevenZip;
    if (is("PK\x03\x04", 4) || is("PK\x05\x06", 4)) return MidiLayer::Zip;
    if (is("\x1F\x8B", 2)) return MidiLayer::Gzip;
    if (is("\x28\xB5\x2F\xFD", 4)) return MidiLayer::Zstd;
    if (is("Rar!\x1A\x07", 6)) return MidiLayer::Rar;
    return MidiLayer::Unknown;
}

// ---- PeekSource --------------------------------------------------------------

bool PeekSource::Peek(size_t n, std::string& error) {
    n = (std::min)(n, sizeof(head_));
    while (headLen_ < n) {
        size_t got = 0;
        if (!in_->Read(head_ + headLen_, n - headLen_, got, error)) return false;
        if (got == 0) break;
        headLen_ += got;
    }
    return true;
}
bool PeekSource::Read(uint8_t* dst, size_t want, size_t& got, std::string& error) {
    got = 0;
    if (headPos_ < headLen_) {
        got = (std::min)(want, headLen_ - headPos_);
        std::memcpy(dst, head_ + headPos_, got);
        headPos_ += got;
        return true;
    }
    return in_->Read(dst, want, got, error);
}

// ---- XzSource ----------------------------------------------------------------

struct XzSource::State {
    CXzUnpacker unpacker;
    std::vector<uint8_t> in = std::vector<uint8_t>(1u << 16);
    size_t inPos = 0, inLen = 0;
    bool inEnd = false, finished = false;
};

XzSource::XzSource(std::unique_ptr<ByteSource> in) : in_(std::move(in)), state_(new State()) {
    InitCrcTables();
    XzUnpacker_Construct(&state_->unpacker, &g_Alloc);
    XzUnpacker_Init(&state_->unpacker);
}
XzSource::~XzSource() { XzUnpacker_Free(&state_->unpacker); }

bool XzSource::Read(uint8_t* dst, size_t want, size_t& got, std::string& error) {
    State& s = *state_;
    got = 0;
    while (got == 0 && want && !s.finished) {
        if (s.inPos == s.inLen && !s.inEnd) {
            size_t n = 0;
            if (!in_->Read(s.in.data(), s.in.size(), n, error)) return false;
            s.inPos = 0; s.inLen = n;
            s.inEnd = n == 0;
        }
        SizeT outLen = want;
        SizeT srcLen = s.inLen - s.inPos;
        ECoderStatus status = CODER_STATUS_NOT_SPECIFIED;
        const SRes res = XzUnpacker_Code(&s.unpacker, dst, &outLen, s.in.data() + s.inPos, &srcLen,
                                         s.inEnd ? 1 : 0, CODER_FINISH_ANY, &status);
        s.inPos += srcLen;
        got = outLen;
        if (res != SZ_OK) {
            error = res == SZ_ERROR_MEM ? "out of memory while decoding xz data"
                  : res == SZ_ERROR_UNSUPPORTED ? "xz data uses an unsupported filter"
                  : "xz data is truncated or corrupt";
            return false;
        }
        if (got == 0 && s.inEnd && s.inPos == s.inLen) {
            if (!XzUnpacker_IsStreamWasFinished(&s.unpacker)) {
                error = "xz data is truncated or corrupt";
                return false;
            }
            s.finished = true;
        }
    }
    return true;
}

// ---- layers ------------------------------------------------------------------

std::unique_ptr<ByteSource> OpenMidiLayers(const wchar_t* path, std::string& format,
                                           std::string& error) {
    format.clear();
    std::unique_ptr<FileSource> file(new FileSource());
    std::string ignored;
    if (!file->Open(path, ignored)) return nullptr;   // the mapping path reports it
    std::unique_ptr<PeekSource> top(new PeekSource(std::move(file)));
    if (!top->Peek(16, error)) return nullptr;
    unsigned layers = 0;
    for (;;) {
        const MidiLayer layer = SniffLayer(top->Head(), top->HeadBytes());
        if (layer == MidiLayer::Rar) {
            error = "RAR archives are not supported yet; extract the MIDI first";
            return nullptr;
        }
        if (layers == 0 && (layer == MidiLayer::Midi || layer == MidiLayer::Unknown))
            return nullptr;   // plain file: mapped as before
        if (layer == MidiLayer::Midi || layer == MidiLayer::Unknown) return top;
        if (layer == MidiLayer::Zip || layer == MidiLayer::SevenZip) {
            if (layers == 0) return nullptr;
            error = "an archive inside a compressed file is not supported";
            return nullptr;
        }
        if (++layers > kMaxLayers) { error = "too many nested compression layers"; return nullptr; }
        std::unique_ptr<ByteSource> next;
        const char* name = "";
        switch (layer) {
        case MidiLayer::Xz: next.reset(new XzSource(std::move(top))); name = "xz"; break;
        default:
            error = "unsupported compression format";
            return nullptr;
        }
        if (!format.empty()) format += "+";
        format += name;
        top.reset(new PeekSource(std::move(next)));
        if (!top->Peek(16, error)) return nullptr;
    }
}

} // namespace svms
