#include "events/MidiArchive.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#include <fcntl.h>
#include <locale>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
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

// ---- archives ----------------------------------------------------------------

namespace {
// A file read at offsets (archives keep their directory at the end).
class RandomFile {
public:
    ~RandomFile() {
#if defined(_WIN32)
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (file_ >= 0) ::close(file_);
#endif
    }
    bool Open(const wchar_t* path) {
#if defined(_WIN32)
        file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        if (file_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(file_, &size)) return false;
        size_ = static_cast<uint64_t>(size.QuadPart);
#else
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        file_ = ::open(converter.to_bytes(path).c_str(), O_RDONLY);
        struct stat st{};
        if (file_ < 0 || fstat(file_, &st) != 0) return false;
        size_ = static_cast<uint64_t>(st.st_size);
#endif
        return true;
    }
    uint64_t Size() const { return size_; }
    size_t ReadAt(uint64_t offset, void* dst, size_t n) const {
        size_t done = 0;
        while (done < n) {
            const size_t ask = (std::min<size_t>)(n - done, 1u << 30);
#if defined(_WIN32)
            OVERLAPPED at{};
            at.Offset = static_cast<DWORD>(offset + done);
            at.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
            DWORD got = 0;
            if (!ReadFile(file_, static_cast<uint8_t*>(dst) + done, static_cast<DWORD>(ask), &got, &at) ||
                got == 0) break;
#else
            const ssize_t got = ::pread(file_, static_cast<uint8_t*>(dst) + done, ask,
                                        static_cast<off_t>(offset + done));
            if (got <= 0) break;
#endif
            done += static_cast<size_t>(got);
        }
        return done;
    }
private:
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
#else
    int file_ = -1;
#endif
    uint64_t size_ = 0;
};

bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != tail[i]) return false;
    return true;
}
bool IsMidiName(std::string name) {
    for (const char* stream : {".xz", ".gz", ".zst"})
        if (EndsWith(name, stream)) { name.resize(name.size() - std::strlen(stream)); break; }
    return EndsWith(name, ".mid") || EndsWith(name, ".midi");
}
// Entry indices, largest first: named MIDIs first (if any), else all.
std::vector<size_t> CandidateOrder(const std::vector<std::pair<std::string, uint64_t>>& entries) {
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return entries[a].second > entries[b].second; });
    return order;
}
} // namespace

int PickMidiEntry(const std::vector<std::pair<std::string, uint64_t>>& entries) {
    int best = -1;
    for (size_t i = 0; i < entries.size(); ++i)
        if (IsMidiName(entries[i].first) &&
            (best < 0 || entries[i].second > entries[static_cast<size_t>(best)].second))
            best = static_cast<int>(i);
    return best;
}

namespace {
// Chooses the entry to load and opens it: the named pick, else the largest
// entry whose first bytes are a MIDI or a stream layer.
template <class OpenEntry>
std::unique_ptr<ByteSource> PickAndOpen(const std::vector<std::pair<std::string, uint64_t>>& entries,
                                        std::string& entryName, std::string& error,
                                        OpenEntry open) {
    const int picked = PickMidiEntry(entries);
    if (picked >= 0) {
        entryName = entries[static_cast<size_t>(picked)].first;
        return open(static_cast<size_t>(picked), error);
    }
    for (size_t i : CandidateOrder(entries)) {
        std::string ignored;
        std::unique_ptr<ByteSource> src = open(i, ignored);
        if (!src) continue;
        std::unique_ptr<PeekSource> peek(new PeekSource(std::move(src)));
        if (!peek->Peek(16, ignored)) continue;
        const MidiLayer layer = SniffLayer(peek->Head(), peek->HeadBytes());
        if (layer == MidiLayer::Midi || layer == MidiLayer::Xz || layer == MidiLayer::Gzip ||
            layer == MidiLayer::Zstd) {
            entryName = entries[i].first;
            return std::unique_ptr<ByteSource>(std::move(peek));
        }
    }
    error = "no MIDI file found in the archive";
    return nullptr;
}

size_t ZipRead(void* opaque, mz_uint64 offset, void* dst, size_t n) {
    return static_cast<RandomFile*>(opaque)->ReadAt(offset, dst, n);
}

// One zip entry, inflated as it is read; CRC and size checked at its end.
class ZipEntrySource : public ByteSource {
public:
    ~ZipEntrySource() override {
        if (iter_) mz_zip_reader_extract_iter_free(iter_);
        mz_zip_reader_end(&zip_);
    }
    bool Open(const wchar_t* path, std::string& error) {
        if (!file_.Open(path)) { error = "cannot open MIDI file"; return false; }
        zip_.m_pRead = ZipRead;
        zip_.m_pIO_opaque = &file_;
        if (!mz_zip_reader_init(&zip_, file_.Size(), 0)) return Fail(error);
        return true;
    }
    std::vector<std::pair<std::string, uint64_t>> Entries() {
        std::vector<std::pair<std::string, uint64_t>> out;
        const mz_uint n = mz_zip_reader_get_num_files(&zip_);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip_, i, &st) || st.m_is_directory) {
                out.push_back({std::string(), 0});   // never picked: empty name, no size
                continue;
            }
            out.push_back({st.m_filename, st.m_uncomp_size});
        }
        return out;
    }
    bool Select(mz_uint index, std::string& error) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip_, index, &st)) return Fail(error);
        if (st.m_is_encrypted) { error = "the zip entry is encrypted"; return false; }
        if (st.m_method != 0 && st.m_method != MZ_DEFLATED) {
            char text[96];
            std::snprintf(text, sizeof(text), "zip method %u is not supported (Stored and Deflate are)",
                          unsigned(st.m_method));
            error = text;
            return false;
        }
        iter_ = mz_zip_reader_extract_iter_new(&zip_, index, 0);
        if (!iter_) return Fail(error);
        return true;
    }
    bool Read(uint8_t* dst, size_t want, size_t& got, std::string& error) override {
        got = 0;
        if (!iter_ || !want) return true;
        got = mz_zip_reader_extract_iter_read(iter_, dst, want);
        if (got == 0) {
            const bool ok = mz_zip_reader_extract_iter_free(iter_) != 0;
            iter_ = nullptr;
            if (!ok) return Fail(error);
        }
        return true;
    }
private:
    bool Fail(std::string& error) {
        error = std::string("zip archive is truncated or corrupt (") +
                mz_zip_get_error_string(mz_zip_get_last_error(&zip_)) + ")";
        return false;
    }
    RandomFile file_;
    mz_zip_archive zip_{};
    mz_zip_reader_extract_iter_state* iter_ = nullptr;
};

std::unique_ptr<ByteSource> OpenZipEntry(const wchar_t* path, std::string& entryName,
                                         std::string& error) {
    std::unique_ptr<ZipEntrySource> probe(new ZipEntrySource());
    if (!probe->Open(path, error)) return nullptr;
    const auto entries = probe->Entries();
    probe.reset();
    return PickAndOpen(entries, entryName, error, [&](size_t i, std::string& err) {
        std::unique_ptr<ZipEntrySource> src(new ZipEntrySource());
        if (!src->Open(path, err) || !src->Select(static_cast<mz_uint>(i), err)) return std::unique_ptr<ByteSource>();
        return std::unique_ptr<ByteSource>(std::move(src));
    });
}
} // namespace

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
            if (layers > 0) {
                error = "an archive inside a compressed file is not supported";
                return nullptr;
            }
            if (layer == MidiLayer::SevenZip) return nullptr;   // not handled yet: mapped as before
            std::string entry;
            std::unique_ptr<ByteSource> src = OpenZipEntry(path, entry, error);
            if (!src) return nullptr;
            ++layers;
            format = "zip";
            top.reset(new PeekSource(std::move(src)));
            if (!top->Peek(16, error)) return nullptr;
            continue;
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
