#include "midisynth/sample_bank.h"

#include <limits>
#include <stdexcept>

namespace midisynth {

namespace {
void validateSize(std::size_t size, std::size_t existing) {
    if (size == 0) {
        throw std::invalid_argument("a sample must contain at least one frame");
    }
    if (size > std::numeric_limits<std::uint32_t>::max() - existing) {
        throw std::length_error("sample bank exceeds 32-bit addressable storage");
    }
}
} // namespace

std::uint32_t SampleBank::appendSamples(std::span<const float> samples) {
    validateSize(samples.size(), data_.size());
    const auto offset = static_cast<std::uint32_t>(data_.size());
    data_.insert(data_.end(), samples.begin(), samples.end());
    return offset;
}

SampleId SampleBank::addOneShotView(std::uint32_t offset, std::uint32_t length) {
    if (length == 0 || offset > data_.size() || length > data_.size() - offset) {
        throw std::invalid_argument("invalid one-shot sample view");
    }
    const auto id = static_cast<SampleId>(descriptors_.size());
    descriptors_.push_back({offset, length, 0, length, false});
    return id;
}

SampleId SampleBank::addLoopView(std::uint32_t offset, std::uint32_t length,
                                 std::uint32_t loopStart, std::uint32_t loopEnd) {
    if (length == 0 || offset > data_.size() || length > data_.size() - offset ||
        loopStart >= loopEnd || loopEnd > length) {
        throw std::invalid_argument("invalid looping sample view");
    }
    const auto id = static_cast<SampleId>(descriptors_.size());
    descriptors_.push_back({offset, length, loopStart, loopEnd, true});
    return id;
}

SampleId SampleBank::addOneShot(std::span<const float> samples) {
    const auto offset = appendSamples(samples);
    return addOneShotView(offset, static_cast<std::uint32_t>(samples.size()));
}

SampleId SampleBank::addLoop(std::span<const float> samples,
                             std::uint32_t loopStart,
                             std::uint32_t loopEnd) {
    if (loopStart >= loopEnd || loopEnd > samples.size()) {
        throw std::invalid_argument("loop must be a non-empty half-open sample range");
    }
    const auto offset = appendSamples(samples);
    return addLoopView(offset, static_cast<std::uint32_t>(samples.size()),
                       loopStart, loopEnd);
}

const SampleDescriptor& SampleBank::descriptor(SampleId id) const {
    if (id >= descriptors_.size()) {
        throw std::out_of_range("invalid sample id");
    }
    return descriptors_[id];
}

} // namespace midisynth
