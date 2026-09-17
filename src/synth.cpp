#include "midisynth/synth.h"

#if MIDISYNTH_HAS_AVX2
#include "avx2_kernel.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace midisynth {

#if MIDISYNTH_HAS_AVX2
namespace detail {
bool avx2CpuSupported() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#elif defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 1);
    constexpr int osxsave = 1 << 27;
    constexpr int avx = 1 << 28;
    if ((registers[2] & (osxsave | avx)) != (osxsave | avx) ||
        (_xgetbv(0) & 0x6) != 0x6) return false;
    __cpuidex(registers, 7, 0);
    return (registers[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}
} // namespace detail
#endif

Event Event::noteOn(std::uint64_t frame, std::uint32_t sequence,
                    std::uint8_t note, SampleId sample, float phaseIncrement,
                    float leftGain, float rightGain,
                    std::uint32_t attackFrames, std::uint32_t releaseFrames,
                    std::uint8_t channel) {
    Event result{};
    result.frame = frame;
    result.sequence = sequence;
    result.type = EventType::NoteOn;
    result.note = note;
    result.sample = sample;
    result.phaseIncrement = phaseIncrement;
    result.leftGain = leftGain;
    result.rightGain = rightGain;
    result.attackFrames = attackFrames;
    result.releaseFrames = releaseFrames;
    result.channel = channel;
    return result;
}

Event Event::noteOff(std::uint64_t frame, std::uint32_t sequence,
                     std::uint8_t note, std::uint8_t channel,
                     std::uint32_t noteInstance) {
    Event result{};
    result.frame = frame;
    result.sequence = sequence;
    result.type = EventType::NoteOff;
    result.note = note;
    result.channel = channel;
    result.noteInstance = noteInstance;
    return result;
}

Event Event::channelControl(std::uint64_t frame, std::uint32_t sequence,
                            EventType type, std::uint8_t channel,
                            std::uint16_t value) {
    Event result{};
    result.frame = frame;
    result.sequence = sequence;
    result.type = type;
    result.channel = channel;
    result.value = value;
    return result;
}

namespace {
using Clock = std::chrono::steady_clock;

struct VoiceState {
    std::vector<float> phase;
    std::vector<float> increment;
    std::vector<float> envelope;
    std::vector<float> envelopeStep;
    std::vector<float> sustainGain;
    std::vector<float> leftGain;
    std::vector<float> rightGain;
    std::vector<std::uint32_t> sampleOffset;
    std::vector<std::uint32_t> sampleEnd;
    std::vector<std::uint32_t> loopStart;
    std::vector<std::uint32_t> loopEnd;
    std::vector<std::uint32_t> releaseFrames;
    std::vector<std::uint32_t> attackFrames;
    std::vector<std::uint32_t> holdFrames;
    std::vector<std::uint32_t> decayFrames;
    std::vector<std::uint32_t> envelopeFramesRemaining;
    std::vector<std::uint32_t> noteInstance;
    std::vector<std::uint32_t> logarithmicEnvelope;
    std::vector<std::uint8_t> note;
    std::vector<std::uint8_t> channel;
    std::vector<EnvelopeStage> envelopeStage;
    std::vector<std::uint32_t> looping;
    std::vector<std::uint32_t> alive;
    std::vector<std::int32_t> noteNext;
    std::vector<std::int32_t> notePrevious;
    std::vector<std::uint32_t> activePosition;

    explicit VoiceState(std::size_t n)
        : phase(n), increment(n), envelope(n), envelopeStep(n), sustainGain(n), leftGain(n),
          rightGain(n), sampleOffset(n), sampleEnd(n), loopStart(n), loopEnd(n),
          releaseFrames(n), attackFrames(n), holdFrames(n), decayFrames(n), envelopeFramesRemaining(n),
          noteInstance(n), logarithmicEnvelope(n), note(n), channel(n),
          envelopeStage(n, EnvelopeStage::Inactive),
          looping(n), alive(n), noteNext(n, -1), notePrevious(n, -1),
          activePosition(n, std::numeric_limits<std::uint32_t>::max()) {}
};
} // namespace

class Synth::Impl {
public:
    Impl(const SampleBank& sampleBank, SynthConfig synthConfig)
        : samples(sampleBank), cfg(synthConfig), voices(cfg.voiceCapacity) {
        if (cfg.voiceCapacity == 0 || cfg.tileSize == 0 || cfg.maxBlockFrames == 0) {
            throw std::invalid_argument("capacity, tile size, and block size must be non-zero");
        }
        if (cfg.backend == Backend::Avx2) {
#if MIDISYNTH_HAS_AVX2
            if (!detail::avx2CpuSupported()) throw std::runtime_error("AVX2 is not supported by this CPU");
#else
            throw std::runtime_error("AVX2 backend was disabled at build time");
#endif
        }
        active.reserve(cfg.voiceCapacity);
        freeSlots.reserve(cfg.voiceCapacity);
        retiredSlots.resize(cfg.voiceCapacity);
        maxTiles = (cfg.voiceCapacity + cfg.tileSize - 1) / cfg.tileSize;
        tileMix.resize(static_cast<std::size_t>(maxTiles) * cfg.maxBlockFrames * 2);
        tileRetired.resize(cfg.voiceCapacity);
        tileRetiredCounts.resize(maxTiles);
        pendingRetiredTiles.resize(maxTiles);
        reset();
        workers.reserve(cfg.workerThreads);
        for (std::uint32_t i = 0; i < cfg.workerThreads; ++i) {
            workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        jobEpoch.fetch_add(1, std::memory_order_release);
        jobEpoch.notify_all();
        for (auto& worker : workers) worker.join();
    }

    void reset() {
        active.clear();
        retiredCount = 0;
        pendingRetiredTileCount = 0;
        std::fill(tileRetiredCounts.begin(), tileRetiredCounts.end(), 0);
        freeSlots.clear();
        for (std::uint32_t i = cfg.voiceCapacity; i > 0; --i) {
            freeSlots.push_back(i - 1);
        }
        std::fill(voices.alive.begin(), voices.alive.end(), 0);
        std::fill(voices.envelopeStage.begin(), voices.envelopeStage.end(),
                  EnvelopeStage::Inactive);
        noteHeads.fill(-1);
        std::fill(voices.activePosition.begin(), voices.activePosition.end(),
                  std::numeric_limits<std::uint32_t>::max());
        channelVolume.fill(1.0F);
        channelExpression.fill(1.0F);
        channelPanLeft.fill(1.0F);
        channelPanRight.fill(1.0F);
        channelLeftScale.fill(1.0F);
        channelRightScale.fill(1.0F);
        channelPitchRatio.fill(1.0F);
        channelPitchBend.fill(8192);
        channelPitchBendRange.fill(2.0F);
        renderStats = {};
    }

    void updateChannelScales(std::uint8_t channel) {
        const float level = channelVolume[channel] * channelExpression[channel];
        channelLeftScale[channel] = level * channelPanLeft[channel];
        channelRightScale[channel] = level * channelPanRight[channel];
    }

    void enterDecayOrSustain(std::uint32_t slot) {
        if (voices.decayFrames[slot] != 0 && voices.sustainGain[slot] < 1.0F) {
            voices.envelopeStage[slot] = EnvelopeStage::Decay;
            voices.envelopeFramesRemaining[slot] = voices.decayFrames[slot];
            voices.envelopeStep[slot] = voices.logarithmicEnvelope[slot]
                ? std::pow(std::max(voices.sustainGain[slot], 1.0e-8F),
                    1.0F / static_cast<float>(voices.decayFrames[slot]))
                : (1.0F - voices.sustainGain[slot]) /
                    static_cast<float>(voices.decayFrames[slot]);
        } else {
            voices.envelope[slot] = voices.sustainGain[slot];
            voices.envelopeStep[slot] = 0.0F;
            voices.envelopeFramesRemaining[slot] = 0;
            voices.envelopeStage[slot] = EnvelopeStage::Sustain;
        }
    }

    void enterHoldOrLater(std::uint32_t slot) {
        voices.envelope[slot] = 1.0F;
        voices.envelopeStep[slot] = 0.0F;
        if (voices.holdFrames[slot] != 0) {
            voices.envelopeStage[slot] = EnvelopeStage::Hold;
            voices.envelopeFramesRemaining[slot] = voices.holdFrames[slot];
        } else {
            enterDecayOrSustain(slot);
        }
    }

    void enterAttackOrLater(std::uint32_t slot) {
        if (voices.attackFrames[slot] != 0) {
            voices.envelope[slot] = 0.0F;
            voices.envelopeStep[slot] = 1.0F /
                static_cast<float>(voices.attackFrames[slot]);
            voices.envelopeFramesRemaining[slot] = voices.attackFrames[slot];
            voices.envelopeStage[slot] = EnvelopeStage::Attack;
        } else {
            enterHoldOrLater(slot);
        }
    }

    void validate(std::uint64_t start, std::uint64_t frameCount,
                  std::span<const Event> events) const {
        if (frameCount > cfg.maxBlockFrames) {
            throw std::invalid_argument("render block exceeds configured maximum");
        }
        const auto end = start + frameCount;
        if (end < start) {
            throw std::overflow_error("render timeline overflow");
        }
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].frame < start || events[i].frame >= end) {
                throw std::invalid_argument("event lies outside render block");
            }
            if (i != 0) {
                const auto& a = events[i - 1];
                const auto& b = events[i];
                if (a.frame > b.frame || (a.frame == b.frame && a.sequence > b.sequence)) {
                    throw std::invalid_argument("events are not sorted by frame and sequence");
                }
            }
        }
    }

    void startVoice(const Event& event) {
        if (event.channel >= 16) throw std::invalid_argument("MIDI channel must be in [0, 15]");
        if (freeSlots.empty()) {
            ++renderStats.droppedNoteOns;
            return;
        }
        if (!(event.phaseIncrement > 0.0F) || !std::isfinite(event.phaseIncrement)) {
            throw std::invalid_argument("phase increment must be finite and positive");
        }
        const auto& sample = samples.descriptor(event.sample);
        const auto slot = freeSlots.back();
        freeSlots.pop_back();
        voices.phase[slot] = 0.0F;
        voices.increment[slot] = event.phaseIncrement;
        voices.envelope[slot] = 0.0F;
        voices.envelopeStep[slot] = 0.0F;
        voices.sustainGain[slot] = std::clamp(event.sustainGain, 0.0F, 1.0F);
        voices.leftGain[slot] = event.leftGain;
        voices.rightGain[slot] = event.rightGain;
        voices.sampleOffset[slot] = sample.offset;
        voices.sampleEnd[slot] = sample.length;
        voices.loopStart[slot] = sample.loopStart;
        voices.loopEnd[slot] = sample.loopEnd;
        voices.releaseFrames[slot] = event.releaseFrames;
        voices.attackFrames[slot] = event.attackFrames;
        voices.holdFrames[slot] = event.holdFrames;
        voices.decayFrames[slot] = event.decayFrames;
        voices.envelopeFramesRemaining[slot] = event.delayFrames;
        voices.noteInstance[slot] = event.noteInstance;
        voices.logarithmicEnvelope[slot] = event.logarithmicEnvelope ? 1U : 0U;
        voices.note[slot] = event.note;
        voices.channel[slot] = event.channel;
        if (event.delayFrames != 0) {
            voices.envelopeStage[slot] = EnvelopeStage::Delay;
        } else {
            enterAttackOrLater(slot);
        }
        voices.looping[slot] = sample.looping ? 1 : 0;
        voices.alive[slot] = 1;
        voices.notePrevious[slot] = -1;
        const auto key = static_cast<std::size_t>(event.channel) * 128 + event.note;
        voices.noteNext[slot] = noteHeads[key];
        if (noteHeads[key] >= 0) {
            voices.notePrevious[static_cast<std::uint32_t>(noteHeads[key])] =
                static_cast<std::int32_t>(slot);
        }
        noteHeads[key] = static_cast<std::int32_t>(slot);
        voices.activePosition[slot] = static_cast<std::uint32_t>(active.size());
        active.push_back(slot);
        renderStats.activeVoiceHighWater = std::max(
            renderStats.activeVoiceHighWater, static_cast<std::uint32_t>(active.size()));
    }

    void releaseNote(std::uint8_t note, std::uint8_t channel,
                     std::uint32_t noteInstance = 0) {
        if (channel >= 16) throw std::invalid_argument("MIDI channel must be in [0, 15]");
        auto current = noteHeads[static_cast<std::size_t>(channel) * 128 + note];
        while (current >= 0) {
            const auto slot = static_cast<std::uint32_t>(current);
            current = voices.noteNext[slot];
            if (!voices.alive[slot] || voices.envelopeStage[slot] == EnvelopeStage::Release ||
                (noteInstance != 0 && voices.noteInstance[slot] != noteInstance)) continue;
            const auto frames = voices.releaseFrames[slot];
            if (frames == 0 || voices.envelope[slot] <= 0.0F) {
                voices.envelope[slot] = 0.0F;
                retireEventVoice(slot);
            } else {
                voices.envelopeStep[slot] = voices.logarithmicEnvelope[slot]
                    ? std::pow(1.0e-5F, 1.0F / static_cast<float>(frames))
                    : voices.envelope[slot] / static_cast<float>(frames);
                voices.envelopeFramesRemaining[slot] = frames;
                voices.envelopeStage[slot] = EnvelopeStage::Release;
            }
        }
    }

    void retireTo(std::uint32_t slot, std::uint32_t* queue, std::size_t& count) {
        if (!voices.alive[slot]) return;
        voices.envelopeStage[slot] = EnvelopeStage::Inactive;
        voices.alive[slot] = 0;
        queue[count++] = slot;
    }

    void retireEventVoice(std::uint32_t slot) {
        retireTo(slot, retiredSlots.data(), retiredCount);
    }

    void unlinkNote(std::uint32_t slot) {
        const auto previous = voices.notePrevious[slot];
        const auto next = voices.noteNext[slot];
        if (previous >= 0) {
            voices.noteNext[static_cast<std::uint32_t>(previous)] = next;
        } else {
            noteHeads[static_cast<std::size_t>(voices.channel[slot]) * 128 +
                      voices.note[slot]] = next;
        }
        if (next >= 0) {
            voices.notePrevious[static_cast<std::uint32_t>(next)] = previous;
        }
        voices.noteNext[slot] = -1;
        voices.notePrevious[slot] = -1;
    }

    void dispatch(const Event& event) {
        if (event.channel >= 16) throw std::invalid_argument("MIDI channel must be in [0, 15]");
        if (event.type == EventType::NoteOn) {
            startVoice(event);
        } else if (event.type == EventType::NoteOff) {
            releaseNote(event.note, event.channel, event.noteInstance);
        } else if (event.type == EventType::ChannelVolume) {
            channelVolume[event.channel] = std::min<std::uint16_t>(event.value, 127) / 127.0F;
            updateChannelScales(event.channel);
        } else if (event.type == EventType::ChannelExpression) {
            channelExpression[event.channel] = std::min<std::uint16_t>(event.value, 127) / 127.0F;
            updateChannelScales(event.channel);
        } else if (event.type == EventType::ChannelPan) {
            const auto value = std::min<std::uint16_t>(event.value, 127);
            const float pan = value <= 64 ? (static_cast<float>(value) - 64.0F) / 64.0F
                                          : (static_cast<float>(value) - 64.0F) / 63.0F;
            // MIDI pan is applied as a constant-power balance around an exact
            // unity center so it composes with the region's prepared pan law.
            channelPanLeft[event.channel] = pan <= 0.0F ? 1.0F : std::sqrt(1.0F - pan);
            channelPanRight[event.channel] = pan >= 0.0F ? 1.0F : std::sqrt(1.0F + pan);
            updateChannelScales(event.channel);
        } else if (event.type == EventType::PitchBend) {
            channelPitchBend[event.channel] = std::min<std::uint16_t>(event.value, 16383);
            updatePitchRatio(event.channel);
        } else if (event.type == EventType::PitchBendRange) {
            channelPitchBendRange[event.channel] = event.value / 100.0F;
            updatePitchRatio(event.channel);
        } else if (event.type == EventType::AllNotesOff) {
            for (std::uint16_t note = 0; note < 128; ++note) {
                releaseNote(static_cast<std::uint8_t>(note), event.channel);
            }
        } else if (event.type == EventType::AllSoundOff) {
            for (std::uint16_t note = 0; note < 128; ++note) {
                auto current = noteHeads[static_cast<std::size_t>(event.channel) * 128 + note];
                while (current >= 0) {
                    const auto slot = static_cast<std::uint32_t>(current);
                    current = voices.noteNext[slot];
                    retireEventVoice(slot);
                }
            }
        } else if (event.type == EventType::ResetControllers) {
            channelExpression[event.channel] = 1.0F;
            channelPanLeft[event.channel] = 1.0F;
            channelPanRight[event.channel] = 1.0F;
            channelPitchBend[event.channel] = 8192;
            channelPitchBendRange[event.channel] = 2.0F;
            updateChannelScales(event.channel);
            updatePitchRatio(event.channel);
        }
    }

    void updatePitchRatio(std::uint8_t channel) {
        const float bend = (static_cast<int>(channelPitchBend[channel]) - 8192) / 8192.0F;
        channelPitchRatio[channel] = std::exp2(
            bend * channelPitchBendRange[channel] / 12.0F);
    }

    float interpolate(std::uint32_t slot) const {
        const float phase = voices.phase[slot];
        const auto i0 = static_cast<std::uint32_t>(phase);
        std::uint32_t i1 = i0 + 1;
        if (voices.looping[slot]) {
            if (i1 >= voices.loopEnd[slot]) {
                i1 = voices.loopStart[slot];
            }
        } else if (i1 >= voices.sampleEnd[slot]) {
            i1 = voices.sampleEnd[slot] - 1;
        }
        const auto base = voices.sampleOffset[slot];
        const float a = samples.data()[base + i0];
        const float b = samples.data()[base + i1];
        return a + (b - a) * (phase - static_cast<float>(i0));
    }

    void advance(std::uint32_t slot, std::uint32_t* retired, std::size_t& count) {
        switch (voices.envelopeStage[slot]) {
        case EnvelopeStage::Delay:
            if (--voices.envelopeFramesRemaining[slot] == 0) enterAttackOrLater(slot);
            break;
        case EnvelopeStage::Attack:
            voices.envelope[slot] += voices.envelopeStep[slot];
            if (--voices.envelopeFramesRemaining[slot] == 0) enterHoldOrLater(slot);
            break;
        case EnvelopeStage::Hold:
            if (--voices.envelopeFramesRemaining[slot] == 0) enterDecayOrSustain(slot);
            break;
        case EnvelopeStage::Decay:
            if (voices.logarithmicEnvelope[slot]) voices.envelope[slot] *= voices.envelopeStep[slot];
            else voices.envelope[slot] -= voices.envelopeStep[slot];
            if (--voices.envelopeFramesRemaining[slot] == 0) {
                voices.envelope[slot] = voices.sustainGain[slot];
                voices.envelopeStep[slot] = 0.0F;
                voices.envelopeStage[slot] = EnvelopeStage::Sustain;
            }
            break;
        case EnvelopeStage::Release:
            if (voices.logarithmicEnvelope[slot]) voices.envelope[slot] *= voices.envelopeStep[slot];
            else voices.envelope[slot] -= voices.envelopeStep[slot];
            --voices.envelopeFramesRemaining[slot];
            if ((voices.logarithmicEnvelope[slot] &&
                 (voices.envelopeFramesRemaining[slot] == 0 || voices.envelope[slot] <= 1.0e-5F)) ||
                (!voices.logarithmicEnvelope[slot] &&
                 (voices.envelopeFramesRemaining[slot] == 0 || voices.envelope[slot] <= 0.0F))) {
                voices.envelope[slot] = 0.0F;
                voices.envelopeStep[slot] = 0.0F;
                retireTo(slot, retired, count);
            }
            break;
        default:
            break;
        }

        if (!voices.alive[slot]) {
            return;
        }
        float phase = voices.phase[slot] + voices.increment[slot] *
            channelPitchRatio[voices.channel[slot]];
        if (voices.looping[slot]) {
            const float loopStart = static_cast<float>(voices.loopStart[slot]);
            const float loopEnd = static_cast<float>(voices.loopEnd[slot]);
            const float loopLength = loopEnd - loopStart;
            if (phase >= loopEnd) {
                phase = loopStart + std::fmod(phase - loopStart, loopLength);
            }
            voices.phase[slot] = phase;
        } else if (phase >= static_cast<float>(voices.sampleEnd[slot])) {
            voices.phase[slot] = static_cast<float>(voices.sampleEnd[slot]);
            retireTo(slot, retired, count);
        } else {
            voices.phase[slot] = phase;
        }
    }

    void renderScalarTile(std::size_t activeBegin, std::size_t activeCount,
                          std::size_t frameCount, float* left, float* right,
                          std::uint32_t* retired, std::size_t& retireCount) {
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            float mixedLeft = 0.0F;
            float mixedRight = 0.0F;
            for (std::size_t i = activeBegin; i < activeBegin + activeCount; ++i) {
                const auto slot = active[i];
                if (!voices.alive[slot]) {
                    continue;
                }
                const auto channel = voices.channel[slot];
                const float value = interpolate(slot) * voices.envelope[slot];
                mixedLeft += value * voices.leftGain[slot] * channelLeftScale[channel];
                mixedRight += value * voices.rightGain[slot] * channelRightScale[channel];
                advance(slot, retired, retireCount);
            }
            left[frame] = mixedLeft;
            right[frame] = mixedRight;
        }
    }

#if MIDISYNTH_HAS_AVX2
    detail::KernelStateView kernelStateView() {
        return {voices.phase.data(), voices.increment.data(), voices.envelope.data(),
                voices.envelopeStep.data(), voices.sustainGain.data(),
                voices.leftGain.data(), voices.rightGain.data(),
                voices.sampleOffset.data(), voices.sampleEnd.data(), voices.loopStart.data(),
                voices.loopEnd.data(), voices.attackFrames.data(), voices.holdFrames.data(),
                voices.decayFrames.data(), voices.envelopeFramesRemaining.data(),
                voices.logarithmicEnvelope.data(), voices.channel.data(),
                voices.envelopeStage.data(), voices.looping.data(),
                voices.alive.data(), channelLeftScale.data(), channelRightScale.data(),
                channelPitchRatio.data()};
    }
#endif

    void renderTile(std::size_t tile, std::size_t frameCount) {
        const auto begin = tile * cfg.tileSize;
        const auto count = std::min<std::size_t>(cfg.tileSize, active.size() - begin);
        const auto tileStride = static_cast<std::size_t>(cfg.maxBlockFrames) * 2;
        float* left = tileMix.data() + tile * tileStride;
        float* right = left + cfg.maxBlockFrames;
        auto* retired = tileRetired.data() + begin;
        auto& retireCount = tileRetiredCounts[tile];
        retireCount = 0;
        if (cfg.backend == Backend::Scalar) {
            renderScalarTile(begin, count, frameCount, left, right, retired, retireCount);
        } else {
#if MIDISYNTH_HAS_AVX2
            detail::renderAvx2Tile(kernelStateView(), samples.data().data(),
                active.data() + begin, count, frameCount, left, right,
                retired, retireCount, cfg.enableAvx2FastAdvance,
                cfg.enableAvx2ContiguousLoads);
#endif
        }
    }

    void executeAvailableTiles() {
        while (true) {
            const auto tile = nextTile.fetch_add(1, std::memory_order_relaxed);
            if (tile >= jobTileCount) return;
            renderTile(tile, jobFrameCount);
            if (remainingTiles.fetch_sub(1, std::memory_order_release) == 1) {
                remainingTiles.notify_one();
            }
        }
    }

    void workerLoop() {
        auto observed = jobEpoch.load(std::memory_order_acquire);
        while (!stopping.load(std::memory_order_acquire)) {
            jobEpoch.wait(observed, std::memory_order_acquire);
            observed = jobEpoch.load(std::memory_order_acquire);
            if (stopping.load(std::memory_order_acquire)) return;
            executeAvailableTiles();
        }
    }

    void renderSpan(std::size_t outputOffset, std::size_t frameCount,
                    std::span<float> left, std::span<float> right) {
        const auto tileCount = (active.size() + cfg.tileSize - 1) / cfg.tileSize;
        if (tileCount == 0) return;
        const auto tileStart = cfg.collectDetailedTiming ? Clock::now() : Clock::time_point{};
        jobFrameCount = frameCount;
        jobTileCount = tileCount;
        nextTile.store(0, std::memory_order_relaxed);
        remainingTiles.store(tileCount, std::memory_order_relaxed);
        if (!workers.empty() && tileCount > 1 &&
            frameCount >= cfg.workerDispatchMinimumFrames) {
            jobEpoch.fetch_add(1, std::memory_order_release);
            jobEpoch.notify_all();
        }
        executeAvailableTiles();
        auto remaining = remainingTiles.load(std::memory_order_acquire);
        while (remaining != 0) {
            remainingTiles.wait(remaining, std::memory_order_acquire);
            remaining = remainingTiles.load(std::memory_order_acquire);
        }
        if (cfg.collectDetailedTiming) {
            renderStats.tileNanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - tileStart).count());
        }
        pendingRetiredTileCount = 0;
        for (std::size_t tile = 0; tile < tileCount; ++tile) {
            if (tileRetiredCounts[tile] != 0) {
                pendingRetiredTiles[pendingRetiredTileCount++] =
                    static_cast<std::uint32_t>(tile);
            }
        }
        const auto tileStride = static_cast<std::size_t>(cfg.maxBlockFrames) * 2;
        const auto reductionStart = cfg.collectDetailedTiming ? Clock::now() : Clock::time_point{};
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            for (std::size_t tile = 0; tile < tileCount; ++tile) {
                const float* tileLeft = tileMix.data() + tile * tileStride;
                const float* tileRight = tileLeft + cfg.maxBlockFrames;
                left[outputOffset + frame] += tileLeft[frame];
                right[outputOffset + frame] += tileRight[frame];
            }
        }
        if (cfg.collectDetailedTiming) {
            renderStats.reductionNanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - reductionStart).count());
        }
    }

    void recoverOne(std::uint32_t slot) {
        constexpr auto invalid = std::numeric_limits<std::uint32_t>::max();
        const auto position = voices.activePosition[slot];
        if (position == invalid) return;
        const auto movedSlot = active.back();
        active[position] = movedSlot;
        voices.activePosition[movedSlot] = position;
        active.pop_back();
        voices.activePosition[slot] = invalid;
        unlinkNote(slot);
        freeSlots.push_back(slot);
    }

    void recoverRetired() {
        for (std::size_t i = 0; i < retiredCount; ++i) {
            recoverOne(retiredSlots[i]);
        }
        retiredCount = 0;
        for (std::size_t pending = 0; pending < pendingRetiredTileCount; ++pending) {
            const auto tile = pendingRetiredTiles[pending];
            const auto begin = tile * cfg.tileSize;
            for (std::size_t i = 0; i < tileRetiredCounts[tile]; ++i) {
                recoverOne(tileRetired[begin + i]);
            }
            tileRetiredCounts[tile] = 0;
        }
        pendingRetiredTileCount = 0;
    }

    void render(std::uint64_t start, std::span<const Event> events,
                std::span<float> left, std::span<float> right) {
        if (left.size() != right.size()) {
            throw std::invalid_argument("stereo output spans must have equal length");
        }
        validate(start, left.size(), events);
        std::fill(left.begin(), left.end(), 0.0F);
        std::fill(right.begin(), right.end(), 0.0F);

        std::uint64_t cursor = start;
        std::size_t eventIndex = 0;
        const auto end = start + left.size();
        while (cursor < end) {
            const auto nextEventFrame = eventIndex < events.size()
                ? events[eventIndex].frame : end;
            if (nextEventFrame > cursor) {
                const auto begin = Clock::now();
                renderSpan(static_cast<std::size_t>(cursor - start),
                           static_cast<std::size_t>(nextEventFrame - cursor), left, right);
                recoverRetired();
                renderStats.synthesisNanoseconds += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
                cursor = nextEventFrame;
            }
            if (cursor == end) {
                break;
            }

            const auto begin = Clock::now();
            ++renderStats.uniqueEventFrames;
            while (eventIndex < events.size() && events[eventIndex].frame == cursor) {
                dispatch(events[eventIndex]);
                // Preserve sequence semantics, including capacity made available
                // by a NoteOff for a later same-frame NoteOn.
                recoverRetired();
                ++eventIndex;
                ++renderStats.eventsDispatched;
            }
            renderStats.eventNanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
        }
        renderStats.renderedFrames += left.size();
    }

    const SampleBank& samples;
    SynthConfig cfg;
    VoiceState voices;
    std::vector<std::uint32_t> active;
    std::vector<std::uint32_t> freeSlots;
    std::vector<std::uint32_t> retiredSlots;
    std::size_t retiredCount{};
    std::array<std::int32_t, 16 * 128> noteHeads{};
    std::array<float, 16> channelVolume{};
    std::array<float, 16> channelExpression{};
    std::array<float, 16> channelPanLeft{};
    std::array<float, 16> channelPanRight{};
    std::array<float, 16> channelLeftScale{};
    std::array<float, 16> channelRightScale{};
    std::array<float, 16> channelPitchRatio{};
    std::array<std::uint16_t, 16> channelPitchBend{};
    std::array<float, 16> channelPitchBendRange{};
    std::uint32_t maxTiles{};
    std::vector<float> tileMix;
    std::vector<std::uint32_t> tileRetired;
    std::vector<std::size_t> tileRetiredCounts;
    std::vector<std::uint32_t> pendingRetiredTiles;
    std::size_t pendingRetiredTileCount{};
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> jobEpoch{0};
    std::atomic<std::size_t> nextTile{0};
    std::atomic<std::size_t> remainingTiles{0};
    std::size_t jobTileCount{};
    std::size_t jobFrameCount{};
    RenderStats renderStats{};
};

Synth::Synth(const SampleBank& samples, SynthConfig config)
    : impl_(std::make_unique<Impl>(samples, config)) {}
Synth::~Synth() = default;
Synth::Synth(Synth&&) noexcept = default;
Synth& Synth::operator=(Synth&&) noexcept = default;

void Synth::render(std::uint64_t startFrame, std::span<const Event> events,
                   std::span<float> outputLeft, std::span<float> outputRight) {
    impl_->render(startFrame, events, outputLeft, outputRight);
}

void Synth::reset() { impl_->reset(); }
const RenderStats& Synth::stats() const noexcept { return impl_->renderStats; }
std::uint32_t Synth::activeVoiceCount() const noexcept {
    return static_cast<std::uint32_t>(impl_->active.size());
}
const SynthConfig& Synth::config() const noexcept { return impl_->cfg; }

std::vector<VoiceSnapshot> Synth::activeVoices() const {
    std::vector<VoiceSnapshot> result;
    result.reserve(impl_->active.size());
    for (const auto slot : impl_->active) {
        result.push_back({slot, impl_->voices.note[slot], impl_->voices.channel[slot],
                          impl_->voices.phase[slot],
                          impl_->voices.increment[slot], impl_->voices.envelope[slot],
                          impl_->voices.envelopeStep[slot], impl_->voices.leftGain[slot],
                          impl_->voices.rightGain[slot], impl_->voices.sustainGain[slot],
                          impl_->voices.envelopeFramesRemaining[slot],
                          impl_->voices.noteInstance[slot],
                          impl_->voices.logarithmicEnvelope[slot] != 0,
                          impl_->voices.envelopeStage[slot],
                          impl_->voices.looping[slot] != 0});
    }
    return result;
}

bool Synth::avx2Supported() noexcept {
#if MIDISYNTH_HAS_AVX2
    return detail::avx2CpuSupported();
#else
    return false;
#endif
}

} // namespace midisynth
