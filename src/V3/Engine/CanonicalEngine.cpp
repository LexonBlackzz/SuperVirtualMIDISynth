#include "CanonicalEngine.h"

#include "CanonicalChannelInterpreter.h"
#include "CanonicalCore.h"
#include "CanonicalSoundFont.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace svms::canonical {

namespace {
MidiMessage translate(const CanonicalEvent& event) noexcept {
    MidiMessage result{};
    result.frame = event.absoluteFrame;
    result.channel = event.channel;
    result.data1 = event.data1;
    result.data2 = event.data2;
    switch (event.type) {
    case CanonicalEventType::NoteOn:
        result.type = event.data2 ? MidiMessageType::NoteOn
                                  : MidiMessageType::NoteOff;
        break;
    case CanonicalEventType::NoteOff:
        result.type = MidiMessageType::NoteOff;
        break;
    case CanonicalEventType::ControlChange:
        result.type = MidiMessageType::ControlChange;
        break;
    case CanonicalEventType::ProgramChange:
        result.type = MidiMessageType::ProgramChange;
        break;
    case CanonicalEventType::PitchBend:
        result.type = MidiMessageType::PitchBend;
        break;
    }
    return result;
}
} // namespace

class CanonicalEngine::Impl {
public:
    Impl(const std::filesystem::path& path,
         const CanonicalEngineConfig& requested)
        : config(requested), font(loadSoundFont(path)),
          interpreter(font, requested.sampleRate,
                      requested.eventScratchCapacity, false),
          synth(font.sampleBank(), makeCoreConfig(requested)) {
        if (!(config.sampleRate > 0.0) || config.voiceCapacity == 0 ||
            config.maxBlockFrames == 0 || config.eventScratchCapacity == 0) {
            throw std::invalid_argument("invalid canonical engine configuration");
        }
        expanded.reserve(config.eventScratchCapacity);
    }

    static SynthConfig makeCoreConfig(const CanonicalEngineConfig& config) {
        SynthConfig result{};
        result.sampleRate = config.sampleRate;
        result.voiceCapacity = config.voiceCapacity;
        result.tileSize = (std::max)(1u, config.tileSize);
        result.maxBlockFrames = config.maxBlockFrames;
        result.workerThreads = config.workerThreads;
        result.backend = config.backend == CanonicalBackend::Avx2
            ? Backend::Avx2 : Backend::Scalar;
        return result;
    }

    bool dispatch(const CanonicalEvent& event) noexcept {
        ++sourceEvents;
        if (event.channel >= 16 || event.data1 >= 128 || event.data2 >= 128) {
            ++rejectedEvents;
            ++rejectedInvalidData;
            return false;
        }
        if (hasPendingSource &&
            (event.absoluteFrame < lastSourceFrame ||
             (event.absoluteFrame == lastSourceFrame &&
              event.sequence < lastSourceSequence))) {
            ++rejectedEvents;
            ++rejectedOrder;
            return false;
        }
        const auto message = translate(event);
        const auto needed = interpreter.requiredEventCapacity(message);
        if (needed > expanded.capacity() - expanded.size()) {
            ++rejectedEvents;
            ++rejectedCapacity;
            return false;
        }
        try {
            if (!interpreter.process(message, coreSequence, expanded)) {
                ++rejectedEvents;
                ++rejectedInterpreter;
                return false;
            }
        } catch (...) {
            ++rejectedEvents;
            ++rejectedInterpreter;
            return false;
        }
        lastSourceSequence = event.sequence;
        lastSourceFrame = event.absoluteFrame;
        hasPendingSource = true;
        return true;
    }

    void render(std::uint64_t frame, std::span<float> left,
                std::span<float> right) {
        synth.render(frame, expanded, left, right);
        expandedEventCount += expanded.size();
        expanded.clear();
        lastSourceSequence = 0;
        lastSourceFrame = 0;
        hasPendingSource = false;
    }

    void reset() {
        synth.reset();
        interpreter.reset();
        expanded.clear();
        coreSequence = 0;
        lastSourceSequence = 0;
        lastSourceFrame = 0;
        hasPendingSource = false;
    }

    void releaseAll(std::uint64_t frame) noexcept {
        try {
            const auto needed = interpreter.requiredReleaseAllCapacity();
            if (needed > expanded.capacity() - expanded.size()) {
                ++rejectedEvents;
                ++rejectedCapacity;
                return;
            }
            interpreter.appendReleaseAll(frame, coreSequence, expanded);
        } catch (...) {
            ++rejectedEvents;
            ++rejectedInterpreter;
        }
    }

    CanonicalTelemetry telemetry() const noexcept {
        const auto& stats = synth.stats();
        CanonicalTelemetry result{};
        result.sourceEvents = sourceEvents;
        result.sourceNoteOns = interpreter.sourceNoteOns();
        result.expandedEvents = expandedEventCount + expanded.size();
        result.rejectedEvents = rejectedEvents;
        result.rejectedInvalidData = rejectedInvalidData;
        result.rejectedOrder = rejectedOrder;
        result.rejectedCapacity = rejectedCapacity;
        result.rejectedInterpreter = rejectedInterpreter;
        result.launchedVoices = stats.launchedVoices;
        result.retiredVoices = stats.retiredVoices;
        result.droppedNoteOns = stats.droppedNoteOns;
        result.stolenVoices = stats.stolenVoices;
        result.renderedFrames = stats.renderedFrames;
        result.activeVoices = synth.activeVoiceCount();
        result.peakVoices = stats.activeVoiceHighWater;
        return result;
    }

    CanonicalEngineConfig config;
    PreparedSoundFont font;
    detail::ChannelInterpreter interpreter;
    Synth synth;
    std::vector<Event> expanded;
    std::uint32_t coreSequence{};
    std::uint32_t lastSourceSequence{};
    std::uint64_t lastSourceFrame{};
    bool hasPendingSource{};
    std::uint64_t sourceEvents{};
    std::uint64_t expandedEventCount{};
    std::uint64_t rejectedEvents{};
    std::uint64_t rejectedInvalidData{};
    std::uint64_t rejectedOrder{};
    std::uint64_t rejectedCapacity{};
    std::uint64_t rejectedInterpreter{};
};

CanonicalEngine::CanonicalEngine(const std::filesystem::path& path,
                                 const CanonicalEngineConfig& config)
    : impl_(std::make_unique<Impl>(path, config)) {}
CanonicalEngine::~CanonicalEngine() = default;
CanonicalEngine::CanonicalEngine(CanonicalEngine&&) noexcept = default;
CanonicalEngine& CanonicalEngine::operator=(CanonicalEngine&&) noexcept = default;

void CanonicalEngine::reset() { impl_->reset(); }
bool CanonicalEngine::dispatch(const CanonicalEvent& event) noexcept {
    return impl_->dispatch(event);
}
void CanonicalEngine::renderBlock(std::uint64_t frame,
                                  std::span<float> left,
                                  std::span<float> right) {
    impl_->render(frame, left, right);
}
void CanonicalEngine::releaseAll(std::uint64_t frame) noexcept {
    impl_->releaseAll(frame);
}
CanonicalTelemetry CanonicalEngine::telemetry() const noexcept {
    return impl_->telemetry();
}
const char* CanonicalEngine::backendName() const noexcept {
    return impl_->config.backend == CanonicalBackend::Avx2
        ? "canonical-avx2" : "canonical-scalar";
}
bool CanonicalEngine::avx2Supported() noexcept { return Synth::avx2Supported(); }

} // namespace svms::canonical
