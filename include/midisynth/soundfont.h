#pragma once

#include "midisynth/sample_bank.h"
#include "midisynth/synth.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace midisynth {

struct PreparedRegion {
    SampleId sample{};
    std::uint8_t keyLow{};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{};
    std::uint8_t velocityHigh{127};
    std::uint8_t rootKey{60};
    float sampleRate{44100.0F};
    float scaleTuning{100.0F};
    float tuningCents{};
    float gain{1.0F};
    float pan{}; // -1 left, +1 right
    float delaySeconds{};
    float attackSeconds{};
    float holdSeconds{};
    float decaySeconds{};
    float sustainGain{1.0F};
    float releaseSeconds{};
    float keynumToHoldTimecents{};
    float keynumToDecayTimecents{};
    bool logarithmicEnvelope{};
};

struct PreparedPreset {
    std::uint16_t bank{};
    std::uint8_t program{};
    std::string name;
    std::vector<PreparedRegion> regions;
    std::array<std::uint32_t, 128 * 128 + 1> lookupOffsets{};
    std::vector<std::uint32_t> lookupRegionIndices;
};

class PreparedSoundFont {
public:
    SampleBank& preparationSamples();
    PreparedPreset& addPreset(std::uint16_t bank, std::uint8_t program,
                              std::string name = {});
    void finalize();
    [[nodiscard]] const SampleBank& sampleBank() const noexcept { return samples_; }
    [[nodiscard]] std::span<const PreparedPreset> presets() const noexcept { return presets_; }
    [[nodiscard]] std::span<const std::string> warnings() const noexcept { return warnings_; }
    [[nodiscard]] const PreparedPreset* findPreset(std::uint16_t bank,
                                                   std::uint8_t program) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> matchingRegions(
        const PreparedPreset& preset, std::uint8_t key, std::uint8_t velocity) const;

private:
    SampleBank samples_;
    std::vector<PreparedPreset> presets_;
    std::vector<std::string> warnings_;
    bool finalized_{};

    friend PreparedSoundFont loadSoundFont(const std::string& path);
};

PreparedSoundFont loadSoundFont(const std::string& path);

// Expands one already-selected musical note into ordinary format-agnostic core
// events. No SF2 structures are consulted by Synth or its kernels.
void appendPreparedNoteOn(std::vector<Event>& output, const PreparedPreset& preset,
                          const PreparedSoundFont& font, std::uint64_t frame,
                          std::uint32_t& sequence, std::uint8_t channel,
                          std::uint8_t key, std::uint8_t velocity,
                          double outputSampleRate,
                          std::uint32_t noteInstance = 0);

} // namespace midisynth
