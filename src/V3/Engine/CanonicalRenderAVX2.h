#pragma once

#include <cstddef>
#include <cstdint>

#include "CanonicalCore.h"

namespace svms::canonical::detail {

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
    float* filterA0;
    float* filterB1;
    float* filterB2;
    float* filterZ1;
    float* filterZ2;
    std::uint32_t* filterEnabled;
    float* baseFilterCutoffCents;
    float* filterResonanceCentibels;
    float* modulationPitchRatio;
    float* modulationGain;
    float* modLfoPhase;
    float* vibLfoPhase;
    float* modLfoPhaseStep;
    float* vibLfoPhaseStep;
    float* modLfoToPitchCents;
    float* vibLfoToPitchCents;
    float* modWheelToVibPitchCents;
    float* modLfoToFilterCents;
    float* modLfoToVolumeCentibels;
    float* modEnv;
    float* modEnvStep;
    float* modEnvSustain;
    float* modEnvToPitchCents;
    float* modEnvToFilterCents;
    std::uint32_t* modulationActive;
    std::uint32_t* modLfoDelayRemaining;
    std::uint32_t* vibLfoDelayRemaining;
    std::uint32_t* modEnvAttackFrames;
    std::uint32_t* modEnvHoldFrames;
    std::uint32_t* modEnvDecayFrames;
    std::uint32_t* modEnvFramesRemaining;
    EnvelopeStage* modEnvStage;
    float sampleRate;
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
    const float* channelModulation;
};

void renderAvx2Tile(KernelStateView state, const std::int16_t* sampleData,
                    const std::uint32_t* slots, std::size_t slotCount,
                    std::size_t frameCount, float* left, float* right,
                    std::uint32_t* retired, std::size_t& retiredCount,
                    bool enableFastAdvance, bool enableContiguousLoads);

bool avx2CpuSupported() noexcept;

} // namespace svms::canonical::detail
