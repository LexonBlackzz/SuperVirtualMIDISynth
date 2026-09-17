#include "midisynth/wav_writer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace midisynth {
namespace {

void writeLittle16(std::ostream& stream, std::uint16_t value) {
    const char bytes[2]{static_cast<char>(value), static_cast<char>(value >> 8)};
    stream.write(bytes, 2);
}

void writeLittle32(std::ostream& stream, std::uint32_t value) {
    const char bytes[4]{static_cast<char>(value), static_cast<char>(value >> 8),
                        static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    stream.write(bytes, 4);
}

} // namespace

const char* wavSampleFormatName(WavSampleFormat format) noexcept {
    return format == WavSampleFormat::Float32 ? "float32 IEEE" : "PCM16";
}

WavWriter::WavWriter(const std::string& path, std::uint32_t sampleRate,
                     WavSampleFormat format)
    : file_(path, std::ios::binary), format_(format) {
    if (!file_) throw std::runtime_error("cannot create WAV: " + path);
    const std::uint32_t bytesPerSample = format == WavSampleFormat::Float32 ? 4 : 2;
    const std::uint32_t blockAlign = bytesPerSample * 2;
    if (sampleRate > std::numeric_limits<std::uint32_t>::max() / blockAlign) {
        throw std::invalid_argument("WAV sample rate overflows byte rate");
    }

    file_.write("RIFF", 4);
    riffSizePosition_ = file_.tellp();
    writeLittle32(file_, 0);
    file_.write("WAVEfmt ", 8);
    writeLittle32(file_, 16);
    writeLittle16(file_, format == WavSampleFormat::Float32 ? 3 : 1);
    writeLittle16(file_, 2);
    writeLittle32(file_, sampleRate);
    writeLittle32(file_, sampleRate * blockAlign);
    writeLittle16(file_, static_cast<std::uint16_t>(blockAlign));
    writeLittle16(file_, static_cast<std::uint16_t>(bytesPerSample * 8));
    if (format == WavSampleFormat::Float32) {
        file_.write("fact", 4);
        writeLittle32(file_, 4);
        factFramesPosition_ = file_.tellp();
        writeLittle32(file_, 0);
    }
    file_.write("data", 4);
    dataSizePosition_ = file_.tellp();
    writeLittle32(file_, 0);
    if (!file_) throw std::runtime_error("failed writing WAV header");
}

WavWriter::~WavWriter() {
    try { finalize(); } catch (...) {}
}

void WavWriter::write(std::span<const float> left, std::span<const float> right) {
    if (closed_) throw std::logic_error("cannot write to a closed WAV");
    if (left.size() != right.size()) throw std::invalid_argument("WAV channel sizes differ");
    const std::uint64_t bytesPerFrame = format_ == WavSampleFormat::Float32 ? 8 : 4;
    const std::uint64_t fixedRiffBytes = format_ == WavSampleFormat::Float32 ? 48 : 36;
    const auto maximumFrames = (std::numeric_limits<std::uint32_t>::max() - fixedRiffBytes) /
                               bytesPerFrame;
    if (left.size() > maximumFrames - frames_) throw std::length_error("WAV exceeds RIFF 4 GiB limit");

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (format_ == WavSampleFormat::Float32) {
            // Interleave only at the output boundary. bit_cast preserves every
            // finite value, signed zero, infinity, and NaN payload bit-for-bit.
            writeLittle32(file_, std::bit_cast<std::uint32_t>(left[i]));
            writeLittle32(file_, std::bit_cast<std::uint32_t>(right[i]));
        } else {
            const auto convert = [](float value) {
                return static_cast<std::int16_t>(
                    std::lrint(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
            };
            writeLittle16(file_, static_cast<std::uint16_t>(convert(left[i])));
            writeLittle16(file_, static_cast<std::uint16_t>(convert(right[i])));
        }
    }
    if (!file_) throw std::runtime_error("failed writing WAV samples");
    frames_ += left.size();
}

void WavWriter::finalize() {
    if (closed_) return;
    const auto bytesPerFrame = format_ == WavSampleFormat::Float32 ? 8ULL : 4ULL;
    const auto fixedRiffBytes = format_ == WavSampleFormat::Float32 ? 48ULL : 36ULL;
    const auto dataBytes = static_cast<std::uint32_t>(frames_ * bytesPerFrame);
    file_.seekp(riffSizePosition_);
    writeLittle32(file_, static_cast<std::uint32_t>(fixedRiffBytes + dataBytes));
    if (format_ == WavSampleFormat::Float32) {
        file_.seekp(factFramesPosition_);
        writeLittle32(file_, static_cast<std::uint32_t>(frames_));
    }
    file_.seekp(dataSizePosition_);
    writeLittle32(file_, dataBytes);
    file_.flush();
    if (!file_) throw std::runtime_error("failed finalizing WAV");
    file_.close();
    closed_ = true;
}

void WavWriter::close() { finalize(); }

} // namespace midisynth
