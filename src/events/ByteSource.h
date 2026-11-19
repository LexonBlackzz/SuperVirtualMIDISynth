#ifndef SVMS_BYTE_SOURCE_H
#define SVMS_BYTE_SOURCE_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace svms {

// A pull stream of bytes: a file read front to back, or a decompressor.
// Read() returns false with `error` set on failure; got == 0 with true means
// the stream has ended.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual bool Read(uint8_t* dst, size_t want, size_t& got, std::string& error) = 0;
};

// A file read sequentially (no mapping).
class FileSource : public ByteSource {
public:
    FileSource() = default;
    ~FileSource() override;
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;
    bool Open(const wchar_t* path, std::string& error);
    bool Read(uint8_t* dst, size_t want, size_t& got, std::string& error) override;
private:
#if defined(_WIN32)
    void* file_ = nullptr;
#else
    int file_ = -1;
#endif
};

class MemorySource : public ByteSource {
public:
    MemorySource(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool Read(uint8_t* dst, size_t want, size_t& got, std::string& error) override;
private:
    const uint8_t* p_;
    size_t n_;
};

} // namespace svms
#endif
