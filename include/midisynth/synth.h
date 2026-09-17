#pragma once

#include "midisynth/sample_bank.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace midisynth {

enum class EventType : std::uint8_t {
    NoteOff,
    NoteOn,
    ChannelVolume,
    ChannelPan,
    ChannelExpression,
    PitchBend,
    PitchBendRange,
    AllNotesOff,
    AllSoundOff,
    ResetControllers
};

struct Event {
    std::uint64_t frame{};
    std::uint32_t sequence{};
    EventType type{};
    std::uint8_t note{};
    SampleId sample{};
    float phaseIncrement{1.0F};
    float leftGain{1.0F};
    float rightGain{1.0F};
    std::uint32_t attackFrames{};
    std::uint32_t releaseFrames{};
    std::uint8_t channel{};
    std::uint32_t delayFrames{};
    std::uint32_t holdFrames{};
    std::uint32_t decayFrames{};
    float sustainGain{1.0F};
    std::uint32_t noteInstance{}; // zero means every matching instance
    std::uint16_t value{};
    bool logarithmicEnvelope{};

    static Event noteOn(std::uint64_t frame, std::uint32_t sequence,
                        std::uint8_t note, SampleId sample,
                        float phaseIncrement = 1.0F,
                        float leftGain = 1.0F, float rightGain = 1.0F,
                        std::uint32_t attackFrames = 0,
                        std::uint32_t releaseFrames = 0,
                        std::uint8_t channel = 0);
    static Event noteOff(std::uint64_t frame, std::uint32_t sequence,
                         std::uint8_t note, std::uint8_t channel = 0,
                         std::uint32_t noteInstance = 0);
    static Event channelControl(std::uint64_t frame, std::uint32_t sequence,
                                EventType type, std::uint8_t channel,
                                std::uint16_t value);
};

enum class Backend : std::uint8_t { Scalar, Avx2 };
enum class EnvelopeStage : std::uint8_t {
    Inactive, Delay, Attack, Hold, Decay, Sustain, Release
};

struct SynthConfig {
    std::uint32_t voiceCapacity{1024};
    std::uint32_t tileSize{512};
    std::uint32_t maxBlockFrames{4096};
    std::uint32_t workerThreads{0};
    Backend backend{Backend::Scalar};
    bool enableAvx2FastAdvance{true};
    bool enableAvx2ContiguousLoads{true};
    bool collectDetailedTiming{false};
    std::uint32_t workerDispatchMinimumFrames{8};
};

struct RenderStats {
    std::uint64_t eventNanoseconds{};
    std::uint64_t synthesisNanoseconds{};
    std::uint64_t tileNanoseconds{};
    std::uint64_t reductionNanoseconds{};
    std::uint64_t eventsDispatched{};
    std::uint64_t uniqueEventFrames{};
    std::uint64_t renderedFrames{};
    std::uint64_t droppedNoteOns{};
    std::uint32_t activeVoiceHighWater{};
};

struct VoiceSnapshot {
    std::uint32_t slot{};
    std::uint8_t note{};
    std::uint8_t channel{};
    float phase{};
    float phaseIncrement{};
    float envelopeGain{};
    float envelopeStep{};
    float leftGain{};
    float rightGain{};
    float sustainGain{};
    std::uint32_t envelopeFramesRemaining{};
    std::uint32_t noteInstance{};
    bool logarithmicEnvelope{};
    EnvelopeStage envelopeStage{};
    bool looping{};
};

class Synth {
public:
    Synth(const SampleBank& samples, SynthConfig config = {});
    ~Synth();
    Synth(Synth&&) noexcept;
    Synth& operator=(Synth&&) noexcept;
    Synth(const Synth&) = delete;
    Synth& operator=(const Synth&) = delete;

    void render(std::uint64_t startFrame,
                std::span<const Event> events,
                std::span<float> outputLeft,
                std::span<float> outputRight);

    void reset();
    [[nodiscard]] const RenderStats& stats() const noexcept;
    [[nodiscard]] std::vector<VoiceSnapshot> activeVoices() const;
    [[nodiscard]] std::uint32_t activeVoiceCount() const noexcept;
    [[nodiscard]] const SynthConfig& config() const noexcept;
    [[nodiscard]] static bool avx2Supported() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace midisynth
