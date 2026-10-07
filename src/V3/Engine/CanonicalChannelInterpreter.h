#pragma once

#include "CanonicalEngineTypes.h"
#include "CanonicalSoundFont.h"

#include <array>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace svms::canonical::detail {

// Stateful MIDI-channel interpretation shared by file preparation and native
// offline sessions. It owns musical/controller state only and emits ordinary
// V4 Events; it is not renderer or voice state.
class ChannelInterpreter {
public:
    ChannelInterpreter(const PreparedSoundFont& font, double sampleRate,
                       std::size_t logicalNoteCapacity = 0,
                       bool collectWarnings = true);

    void reset();
    [[nodiscard]] bool process(const MidiMessage& message,
                               std::uint32_t& sequence,
                               std::vector<Event>& output);
    [[nodiscard]] std::size_t requiredEventCapacity(
        const MidiMessage& message) const noexcept;
    [[nodiscard]] std::size_t requiredReleaseAllCapacity() const noexcept;
    void appendReleaseAll(std::uint64_t frame, std::uint32_t& sequence,
                          std::vector<Event>& output);
    void appendWarnings(std::vector<std::string>& output) const;
    [[nodiscard]] std::uint64_t sourceNoteOns() const noexcept {
        return sourceNoteOns_;
    }

private:
    static constexpr std::uint32_t noNode = UINT32_MAX;
    struct KeyInstance {
        std::uint32_t id{}, next{noNode};
        bool held{};
    };
    struct Channel {
        std::uint8_t msb{}, lsb{}, program{};
        std::uint8_t rpnMsb{127}, rpnLsb{127}, dataMsb{2}, dataLsb{};
        bool sustain{};
        std::array<std::uint32_t, 128> keyHead{};
        std::array<std::uint32_t, 128> keyTail{};
    };

    const PreparedPreset* findPreset(std::uint8_t channel,
                                     const Channel& state) const;
    void releaseDeferred(std::uint8_t channel, Channel& state,
                         std::uint64_t frame, std::uint32_t& sequence,
                         std::vector<Event>& output);
    void noteOff(std::uint8_t channel, Channel& state, std::uint8_t note,
                 std::uint64_t frame, std::uint32_t& sequence,
                 std::vector<Event>& output);
    bool appendInstance(Channel& state, std::uint8_t note, std::uint32_t id);
    void removeInstance(Channel& state, std::uint8_t note,
                        std::uint32_t previous, std::uint32_t index);
    void clearInstances(Channel& state) noexcept;
    void initializeChannels() noexcept;

    const PreparedSoundFont& font_;
    double sampleRate_{};
    std::array<Channel, 16> channels_{};
    std::vector<KeyInstance> instances_;
    std::uint32_t freeInstance_{noNode};
    std::size_t logicalNoteCapacity_{};
    bool collectWarnings_{true};
    std::uint32_t nextNoteInstance_{1};
    std::uint64_t sourceNoteOns_{};
    std::set<std::pair<std::uint16_t, std::uint8_t>> missing_;
};

} // namespace svms::canonical::detail
