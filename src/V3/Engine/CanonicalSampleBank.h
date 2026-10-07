#pragma once

#include "CanonicalCompat.h"

#include <cstdint>
#include <vector>

namespace svms::canonical {

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
    std::uint32_t appendSamples(Span<const float> samples);
    std::uint32_t appendPcm16(Span<const std::int16_t> samples);
    SampleId addOneShotView(std::uint32_t offset, std::uint32_t length);
    SampleId addLoopView(std::uint32_t offset, std::uint32_t length,
                         std::uint32_t loopStart, std::uint32_t loopEnd);
    SampleId addOneShot(Span<const float> samples);
    SampleId addLoop(Span<const float> samples,
                     std::uint32_t loopStart,
                     std::uint32_t loopEnd);

    [[nodiscard]] const SampleDescriptor& descriptor(SampleId id) const;
    [[nodiscard]] const std::vector<std::int16_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return descriptors_.size(); }

private:
    std::vector<std::int16_t> data_;
    std::vector<SampleDescriptor> descriptors_;
};

} // namespace svms::canonical

