#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace midisynth {

using SampleId = std::uint32_t;

struct SampleDescriptor {
    std::uint32_t offset{};
    std::uint32_t length{};
    std::uint32_t loopStart{};
    std::uint32_t loopEnd{};
    bool looping{};
};

class SampleBank {
public:
    std::uint32_t appendSamples(std::span<const float> samples);
    SampleId addOneShotView(std::uint32_t offset, std::uint32_t length);
    SampleId addLoopView(std::uint32_t offset, std::uint32_t length,
                         std::uint32_t loopStart, std::uint32_t loopEnd);
    SampleId addOneShot(std::span<const float> samples);
    SampleId addLoop(std::span<const float> samples,
                     std::uint32_t loopStart,
                     std::uint32_t loopEnd);

    [[nodiscard]] const SampleDescriptor& descriptor(SampleId id) const;
    [[nodiscard]] const std::vector<float>& data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return descriptors_.size(); }

private:
    std::vector<float> data_;
    std::vector<SampleDescriptor> descriptors_;
};

} // namespace midisynth
