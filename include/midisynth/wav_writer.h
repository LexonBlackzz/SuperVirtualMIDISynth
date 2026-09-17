#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>

namespace midisynth {

enum class WavSampleFormat : std::uint8_t {
    Float32,
    Pcm16
};

const char* wavSampleFormatName(WavSampleFormat format) noexcept;

class WavWriter {
public:
    WavWriter(const std::string& path, std::uint32_t sampleRate,
              WavSampleFormat format = WavSampleFormat::Float32);
    ~WavWriter();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    void write(std::span<const float> left, std::span<const float> right);
    void close();

    [[nodiscard]] WavSampleFormat format() const noexcept { return format_; }
    [[nodiscard]] std::uint64_t framesWritten() const noexcept { return frames_; }

private:
    void finalize();

    std::ofstream file_;
    WavSampleFormat format_{};
    std::uint64_t frames_{};
    std::streampos riffSizePosition_{};
    std::streampos factFramesPosition_{};
    std::streampos dataSizePosition_{};
    bool closed_{};
};

} // namespace midisynth
