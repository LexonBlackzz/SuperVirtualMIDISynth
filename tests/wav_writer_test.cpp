#include "midisynth/wav_writer.h"

#include <bit>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::uint16_t little16(const std::vector<std::uint8_t>& bytes, std::size_t position) {
    require(position <= bytes.size() && bytes.size() - position >= 2, "truncated test WAV field");
    return static_cast<std::uint16_t>(bytes[position] | (bytes[position + 1] << 8));
}
std::uint32_t little32(const std::vector<std::uint8_t>& bytes, std::size_t position) {
    require(position <= bytes.size() && bytes.size() - position >= 4, "truncated test WAV field");
    return static_cast<std::uint32_t>(bytes[position]) |
        (static_cast<std::uint32_t>(bytes[position + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[position + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[position + 3]) << 24);
}
std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(input), "could not reopen test WAV");
    const auto size = input.tellg(); input.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), size)),
            "could not read test WAV");
    return bytes;
}
bool id(const std::vector<std::uint8_t>& bytes, std::size_t position, const char* value) {
    return position <= bytes.size() && bytes.size() - position >= 4 &&
        bytes[position] == static_cast<std::uint8_t>(value[0]) &&
        bytes[position + 1] == static_cast<std::uint8_t>(value[1]) &&
        bytes[position + 2] == static_cast<std::uint8_t>(value[2]) &&
        bytes[position + 3] == static_cast<std::uint8_t>(value[3]);
}

void float32RoundTripIsExact() {
    const std::string path = "midisynth_float32_test.wav";
    const std::vector<float> left{-2.5F, -1.0F, -0.0F, 0.1F, 1.5F};
    const std::vector<float> right{3.25F, 1.0F, 0.0F, -0.2F, -4.0F};
    {
        midisynth::WavWriter writer(path, 48000); // Float32 is the default.
        require(writer.format() == midisynth::WavSampleFormat::Float32,
                "default WAV format is not float32");
        writer.write(left, right);
        writer.close();
    }
    const auto bytes = readFile(path); std::remove(path.c_str());
    require(bytes.size() == 56 + left.size() * 8 && id(bytes, 0, "RIFF") &&
            id(bytes, 8, "WAVE") && id(bytes, 12, "fmt "),
            "float WAV RIFF structure is invalid");
    require(little32(bytes, 4) == bytes.size() - 8 && little32(bytes, 16) == 16 &&
            little16(bytes, 20) == 3 && little16(bytes, 22) == 2 &&
            little32(bytes, 24) == 48000 && little32(bytes, 28) == 48000 * 8 &&
            little16(bytes, 32) == 8 && little16(bytes, 34) == 32,
            "float WAV format fields are invalid");
    require(id(bytes, 36, "fact") && little32(bytes, 40) == 4 &&
            little32(bytes, 44) == left.size() && id(bytes, 48, "data") &&
            little32(bytes, 52) == left.size() * 8,
            "float WAV fact/data chunks are invalid");
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto storedLeft = little32(bytes, 56 + i * 8);
        const auto storedRight = little32(bytes, 60 + i * 8);
        require(storedLeft == std::bit_cast<std::uint32_t>(left[i]) &&
                storedRight == std::bit_cast<std::uint32_t>(right[i]),
                "float WAV sample did not survive exact round-trip");
    }
    require(little32(bytes, 56) == std::bit_cast<std::uint32_t>(-2.5F) &&
            little32(bytes, 60) == std::bit_cast<std::uint32_t>(3.25F),
            "float WAV unexpectedly clamped out-of-range samples");
}

void pcm16CompatibilityIsExplicit() {
    const std::string path = "midisynth_pcm16_test.wav";
    const std::vector<float> left{-2.0F, 0.5F}, right{2.0F, -0.5F};
    {
        midisynth::WavWriter writer(path, 44100, midisynth::WavSampleFormat::Pcm16);
        writer.write(left, right);
        writer.close();
    }
    const auto bytes = readFile(path); std::remove(path.c_str());
    require(bytes.size() == 44 + left.size() * 4 && little16(bytes, 20) == 1 &&
            little16(bytes, 22) == 2 && little16(bytes, 32) == 4 &&
            little16(bytes, 34) == 16 && id(bytes, 36, "data"),
            "PCM16 compatibility WAV header is invalid");
    require(static_cast<std::int16_t>(little16(bytes, 44)) == -32767 &&
            static_cast<std::int16_t>(little16(bytes, 46)) == 32767,
            "PCM16 compatibility conversion did not clamp explicitly");
}

} // namespace

int main() {
    try {
        float32RoundTripIsExact();
        pcm16CompatibilityIsExplicit();
        std::cout << "All WAV writer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WAV TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
