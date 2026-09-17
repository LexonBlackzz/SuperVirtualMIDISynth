#pragma once

#include <cstddef>
#include <cstdint>

#include "midisynth/synth.h"

namespace midisynth::detail {

// This is a non-owning view of the one authoritative SoA. It is deliberately a
// list of numeric arrays rather than a second state representation.
struct KernelStateView {
    float* phase;
    float* increment;
    float* envelope;
    float* envelopeStep;
    float* sustainGain;
    float* leftGain;
    float* rightGain;
    std::uint32_t* sampleOffset;
    std::uint32_t* sampleEnd;
    std::uint32_t* loopStart;
    std::uint32_t* loopEnd;
    std::uint32_t* attackFrames;
    std::uint32_t* holdFrames;
    std::uint32_t* decayFrames;
    std::uint32_t* envelopeFramesRemaining;
    std::uint32_t* logarithmicEnvelope;
    std::uint8_t* channel;
    EnvelopeStage* envelopeStage;
    std::uint32_t* looping;
    std::uint32_t* alive;
    const float* channelLeftScale;
    const float* channelRightScale;
    const float* channelPitchRatio;
};

void renderAvx2Tile(KernelStateView state, const float* sampleData,
                    const std::uint32_t* slots, std::size_t slotCount,
                    std::size_t frameCount, float* left, float* right,
                    std::uint32_t* retired, std::size_t& retiredCount,
                    bool enableFastAdvance, bool enableContiguousLoads);

bool avx2CpuSupported() noexcept;

} // namespace midisynth::detail
