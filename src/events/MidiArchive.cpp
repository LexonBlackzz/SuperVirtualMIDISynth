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
#include "miniz/miniz.h"
#include "zstd/zstd.h"

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

// ---- buffered input shared by the gzip and zstd sources ------------------------

namespace {
struct InputBuffer {
    std::vector<uint8_t> data = std::vector<uint8_t>(1u << 16);
    size_t pos = 0, len = 0;
    bool end = false;
    bool Refill(ByteSource& in, std::string& error) {
        if (pos < len || end) return true;
        size_t n = 0;
        if (!in.Read(data.data(), data.size(), n, error)) return false;
        pos = 0; len = n; end = n == 0;
        return true;
    }
    // One byte; false at the end of the stream (or on a read error: see error).
    bool Byte(ByteSource& in, uint8_t& b, std::string& error) {
        if (!Refill(in, error) || pos == len) return false;
        b = data[pos++];
        return true;
    }
};
} // namespace

// ---- GzipSource --------------------------------------------------------------

struct GzipSource::State {
    InputBuffer in;
    mz_stream z{};
    bool zOpen = false, finished = false, inMember = false;
    uint32_t crc = 0, size = 0;
    unsigned members = 0;
};

GzipSource::GzipSource(std::unique_ptr<ByteSource> in) : in_(std::move(in)), state_(new State()) {}
GzipSource::~GzipSource() { if (state_->zOpen) mz_inflateEnd(&state_->z); }

bool GzipSource::Read(uint8_t* dst, size_t want, size_t& got, std::string& error) {
    State& s = *state_;
    got = 0;
    auto corrupt = [&] {
        if (error.empty()) error = "gzip data is truncated or corrupt";
        return false;
    };
    while (got == 0 && want && !s.finished) {
        if (!s.inMember) {
            // Member header (RFC 1952). After the first member, the end of the
            // stream or bytes that are not another member end the data.
            uint8_t h[10];
            size_t n = 0;
            while (n < 10 && s.in.Byte(*in_, h[n], error)) ++n;
            if (!error.empty()) return false;
            if (s.members > 0 && (n == 0 || (n >= 2 && (h[0] != 0x1f || h[1] != 0x8b)))) {
                s.finished = true;
                break;
            }
            if (n < 10 || h[0] != 0x1f || h[1] != 0x8b || h[2] != 8) return corrupt();
            const uint8_t flags = h[3];
            uint8_t b = 0;
            if (flags & 4) {   // FEXTRA
                uint8_t lo = 0, hi = 0;
                if (!s.in.Byte(*in_, lo, error) || !s.in.Byte(*in_, hi, error)) return corrupt();
                for (unsigned k = 0, x = lo | (hi << 8); k < x; ++k)
                    if (!s.in.Byte(*in_, b, error)) return corrupt();
            }
            for (unsigned bit : {8u, 16u}) {   // FNAME, FCOMMENT: zero-terminated
                if (!(flags & bit)) continue;
                do { if (!s.in.Byte(*in_, b, error)) return corrupt(); } while (b != 0);
            }
            if (flags & 2) {   // FHCRC
                if (!s.in.Byte(*in_, b, error) || !s.in.Byte(*in_, b, error)) return corrupt();
            }
            s.z = mz_stream{};
            if (mz_inflateInit2(&s.z, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return corrupt();
            s.zOpen = true;
            s.inMember = true;
            s.crc = 0; s.size = 0;
            ++s.members;
        }
        if (!s.in.Refill(*in_, error)) return false;
        s.z.next_in = s.in.data.data() + s.in.pos;
        s.z.avail_in = static_cast<unsigned>(s.in.len - s.in.pos);
        s.z.next_out = dst;
        s.z.avail_out = static_cast<unsigned>((std::min<size_t>)(want, 1u << 30));
        const int r = mz_inflate(&s.z, MZ_NO_FLUSH);
        const size_t used = (s.in.len - s.in.pos) - s.z.avail_in;
        s.in.pos += used;
        got = static_cast<size_t>(s.z.next_out - dst);
        s.crc = static_cast<uint32_t>(mz_crc32(s.crc, dst, got));
        s.size += static_cast<uint32_t>(got);
        if (r == MZ_STREAM_END) {
            mz_inflateEnd(&s.z);
            s.zOpen = false;
            uint8_t t[8];
            for (size_t k = 0; k < 8; ++k)
                if (!s.in.Byte(*in_, t[k], error)) return corrupt();
            const uint32_t crc = t[0] | (t[1] << 8) | (t[2] << 16) | (uint32_t(t[3]) << 24);
            const uint32_t size = t[4] | (t[5] << 8) | (t[6] << 16) | (uint32_t(t[7]) << 24);
            if (crc != s.crc || size != s.size) return corrupt();
            s.inMember = false;
        } else if (r != MZ_OK && r != MZ_BUF_ERROR) {
            return corrupt();
        } else if (got == 0 && used == 0 && s.in.end) {
            return corrupt();   // the stream ended inside a member
        }
    }
    return true;
}

// ---- ZstdSource --------------------------------------------------------------

struct ZstdSource::State {
    InputBuffer in;
    ZSTD_DCtx* ctx = nullptr;
    size_t last = 0;   // 0 = the last frame is complete
    bool finished = false, any = false;
};

ZstdSource::ZstdSource(std::unique_ptr<ByteSource> in) : in_(std::move(in)), state_(new State()) {
    state_->ctx = ZSTD_createDCtx();
    if (state_->ctx)
        ZSTD_DCtx_setParameter(state_->ctx, ZSTD_d_windowLogMax, sizeof(void*) == 4 ? 30 : 31);
}
ZstdSource::~ZstdSource() { ZSTD_freeDCtx(state_->ctx); }

bool ZstdSource::Read(uint8_t* dst, size_t want, size_t& got, std::string& error) {
    State& s = *state_;
    got = 0;
    if (!s.ctx) { error = "out of memory while decoding zstd data"; return false; }
    while (got == 0 && want && !s.finished) {
        if (!s.in.Refill(*in_, error)) return false;
        ZSTD_inBuffer input = {s.in.data.data(), s.in.len, s.in.pos};
        ZSTD_outBuffer output = {dst, want, 0};
        const size_t r = ZSTD_decompressStream(s.ctx, &output, &input);
        if (ZSTD_isError(r)) {
            error = std::string("zstd data is corrupt: ") + ZSTD_getErrorName(r);
            return false;
        }
        const bool progress = input.pos != s.in.pos || output.pos != 0;
        s.in.pos = input.pos;
        got = output.pos;
        s.last = r;
        s.any = true;
        if (!progress && s.in.end) {
            if (s.last != 0) { error = "zstd data is truncated"; return false; }
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
        case MidiLayer::Gzip: next.reset(new GzipSource(std::move(top))); name = "gzip"; break;
        case MidiLayer::Zstd: next.reset(new ZstdSource(std::move(top))); name = "zstd"; break;
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
