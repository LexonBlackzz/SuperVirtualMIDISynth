#include "avx2_kernel.h"

#include "midisynth/synth.h"

#include <algorithm>
#include <cmath>
#include <immintrin.h>

namespace midisynth::detail {

namespace {

void retire(KernelStateView s, std::uint32_t slot,
            std::uint32_t* retired, std::size_t& retiredCount) {
    if (!s.alive[slot]) return;
    s.envelopeStage[slot] = EnvelopeStage::Inactive;
    s.alive[slot] = 0;
    retired[retiredCount++] = slot;
}

void enterDecayOrSustain(KernelStateView s, std::uint32_t slot) {
    if (s.decayFrames[slot] != 0 && s.sustainGain[slot] < 1.0F) {
        s.envelopeStage[slot] = EnvelopeStage::Decay;
        s.envelopeFramesRemaining[slot] = s.decayFrames[slot];
        s.envelopeStep[slot] = s.logarithmicEnvelope[slot]
            ? std::pow(std::max(s.sustainGain[slot], 1.0e-8F),
                1.0F / static_cast<float>(s.decayFrames[slot]))
            : (1.0F - s.sustainGain[slot]) /
                static_cast<float>(s.decayFrames[slot]);
    } else {
        s.envelope[slot] = s.sustainGain[slot];
        s.envelopeStep[slot] = 0.0F;
        s.envelopeFramesRemaining[slot] = 0;
        s.envelopeStage[slot] = EnvelopeStage::Sustain;
    }
}

void enterHoldOrLater(KernelStateView s, std::uint32_t slot) {
    s.envelope[slot] = 1.0F;
    s.envelopeStep[slot] = 0.0F;
    if (s.holdFrames[slot] != 0) {
        s.envelopeStage[slot] = EnvelopeStage::Hold;
        s.envelopeFramesRemaining[slot] = s.holdFrames[slot];
    } else {
        enterDecayOrSustain(s, slot);
    }
}

void enterAttackOrLater(KernelStateView s, std::uint32_t slot) {
    if (s.attackFrames[slot] != 0) {
        s.envelope[slot] = 0.0F;
        s.envelopeStep[slot] = 1.0F / static_cast<float>(s.attackFrames[slot]);
        s.envelopeFramesRemaining[slot] = s.attackFrames[slot];
        s.envelopeStage[slot] = EnvelopeStage::Attack;
    } else {
        enterHoldOrLater(s, slot);
    }
}

void advance(KernelStateView s, std::uint32_t slot,
             std::uint32_t* retired, std::size_t& retiredCount) {
    switch (s.envelopeStage[slot]) {
    case EnvelopeStage::Delay:
        if (--s.envelopeFramesRemaining[slot] == 0) enterAttackOrLater(s, slot);
        break;
    case EnvelopeStage::Attack:
        s.envelope[slot] += s.envelopeStep[slot];
        if (--s.envelopeFramesRemaining[slot] == 0) enterHoldOrLater(s, slot);
        break;
    case EnvelopeStage::Hold:
        if (--s.envelopeFramesRemaining[slot] == 0) enterDecayOrSustain(s, slot);
        break;
    case EnvelopeStage::Decay:
        if (s.logarithmicEnvelope[slot]) s.envelope[slot] *= s.envelopeStep[slot];
        else s.envelope[slot] -= s.envelopeStep[slot];
        if (--s.envelopeFramesRemaining[slot] == 0) {
            s.envelope[slot] = s.sustainGain[slot];
            s.envelopeStep[slot] = 0.0F;
            s.envelopeStage[slot] = EnvelopeStage::Sustain;
        }
        break;
    case EnvelopeStage::Release:
        if (s.logarithmicEnvelope[slot]) s.envelope[slot] *= s.envelopeStep[slot];
        else s.envelope[slot] -= s.envelopeStep[slot];
        --s.envelopeFramesRemaining[slot];
        if ((s.logarithmicEnvelope[slot] &&
             (s.envelopeFramesRemaining[slot] == 0 || s.envelope[slot] <= 1.0e-5F)) ||
            (!s.logarithmicEnvelope[slot] &&
             (s.envelopeFramesRemaining[slot] == 0 || s.envelope[slot] <= 0.0F))) {
            s.envelope[slot] = 0.0F;
            s.envelopeStep[slot] = 0.0F;
            retire(s, slot, retired, retiredCount);
        }
        break;
    default:
        break;
    }
    if (!s.alive[slot]) return;
    float phase = s.phase[slot] + s.increment[slot] * s.channelPitchRatio[s.channel[slot]];
    if (s.looping[slot]) {
        const float start = static_cast<float>(s.loopStart[slot]);
        const float end = static_cast<float>(s.loopEnd[slot]);
        if (phase >= end) phase = start + std::fmod(phase - start, end - start);
        s.phase[slot] = phase;
    } else if (phase >= static_cast<float>(s.sampleEnd[slot])) {
        s.phase[slot] = static_cast<float>(s.sampleEnd[slot]);
        retire(s, slot, retired, retiredCount);
    } else {
        s.phase[slot] = phase;
    }
}

float scalarInterpolate(KernelStateView s, std::uint32_t slot, const float* data) {
    const float phase = s.phase[slot];
    const auto i0 = static_cast<std::uint32_t>(phase);
    auto i1 = i0 + 1;
    if (s.looping[slot] && i1 >= s.loopEnd[slot]) i1 = s.loopStart[slot];
    if (!s.looping[slot] && i1 >= s.sampleEnd[slot]) i1 = s.sampleEnd[slot] - 1;
    const auto base = s.sampleOffset[slot];
    const float a = data[base + i0];
    return a + (data[base + i1] - a) * (phase - static_cast<float>(i0));
}

} // namespace

void renderAvx2Tile(KernelStateView s, const float* sampleData,
                    const std::uint32_t* slots, std::size_t slotCount,
                    std::size_t frameCount, float* left, float* right,
                    std::uint32_t* retired, std::size_t& retiredCount,
                    bool enableFastAdvance, bool enableContiguousLoads) {
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        float mixedLeft = 0.0F;
        float mixedRight = 0.0F;
        std::size_t voice = 0;
        alignas(32) float laneLeft[8];
        alignas(32) float laneRight[8];
        for (; voice + 8 <= slotCount; voice += 8) {
            const __m256i indices = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(slots + voice));
            const auto firstSlot = slots[voice];
            const __m256i expectedIndices = _mm256_add_epi32(
                _mm256_set1_epi32(static_cast<int>(firstSlot)),
                _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
            const bool contiguous = enableContiguousLoads &&
                _mm256_movemask_ps(_mm256_castsi256_ps(
                    _mm256_cmpeq_epi32(indices, expectedIndices))) == 0xff;
            const __m256 phase = contiguous
                ? _mm256_loadu_ps(s.phase + firstSlot)
                : _mm256_i32gather_ps(s.phase, indices, 4);
            const __m256i i0 = _mm256_cvttps_epi32(phase);
            const __m256i offsets = contiguous
                ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.sampleOffset + firstSlot))
                : _mm256_i32gather_epi32(reinterpret_cast<const int*>(s.sampleOffset), indices, 4);
            const __m256i absolute0 = _mm256_add_epi32(offsets, i0);

            alignas(32) std::uint32_t slotLane[8];
            _mm256_store_si256(reinterpret_cast<__m256i*>(slotLane), indices);
            const __m256i ones = _mm256_set1_epi32(1);
            const __m256i zero = _mm256_setzero_si256();
            const __m256i candidate = _mm256_add_epi32(i0, ones);
            const __m256i looping = contiguous
                ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.looping + firstSlot))
                : _mm256_i32gather_epi32(reinterpret_cast<const int*>(s.looping), indices, 4);
            const __m256i loopEnd = contiguous
                ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.loopEnd + firstSlot))
                : _mm256_i32gather_epi32(reinterpret_cast<const int*>(s.loopEnd), indices, 4);
            const __m256i sampleEnd = contiguous
                ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.sampleEnd + firstSlot))
                : _mm256_i32gather_epi32(reinterpret_cast<const int*>(s.sampleEnd), indices, 4);
            const __m256i loopStart = contiguous
                ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.loopStart + firstSlot))
                : _mm256_i32gather_epi32(reinterpret_cast<const int*>(s.loopStart), indices, 4);
            const __m256i isLooping = _mm256_cmpgt_epi32(looping, zero);
            const __m256i crossesLoop = _mm256_cmpgt_epi32(candidate,
                _mm256_sub_epi32(loopEnd, ones));
            const __m256i crossesEnd = _mm256_cmpgt_epi32(candidate,
                _mm256_sub_epi32(sampleEnd, ones));
            const __m256i loopNext = _mm256_blendv_epi8(candidate, loopStart, crossesLoop);
            const __m256i oneShotNext = _mm256_blendv_epi8(candidate,
                _mm256_sub_epi32(sampleEnd, ones), crossesEnd);
            const __m256i i1 = _mm256_blendv_epi8(oneShotNext, loopNext, isLooping);
            const __m256i absolute1 = _mm256_add_epi32(offsets, i1);
            const __m256i alive = contiguous
                ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.alive + firstSlot))
                : _mm256_i32gather_epi32(reinterpret_cast<const int*>(s.alive), indices, 4);
            const __m256i aliveMaskI = _mm256_cmpgt_epi32(alive, zero);
            const __m256i safe0 = _mm256_blendv_epi8(zero, absolute0, aliveMaskI);
            const __m256i safe1 = _mm256_blendv_epi8(zero, absolute1, aliveMaskI);
            const __m256 a = _mm256_i32gather_ps(sampleData, safe0, 4);
            const __m256 b = _mm256_i32gather_ps(sampleData, safe1, 4);
            const __m256 fraction = _mm256_sub_ps(phase, _mm256_cvtepi32_ps(i0));
            __m256 value = _mm256_add_ps(a, _mm256_mul_ps(_mm256_sub_ps(b, a), fraction));
            const __m256 envelope = contiguous ? _mm256_loadu_ps(s.envelope + firstSlot)
                                               : _mm256_i32gather_ps(s.envelope, indices, 4);
            const __m256 leftGain = contiguous ? _mm256_loadu_ps(s.leftGain + firstSlot)
                                               : _mm256_i32gather_ps(s.leftGain, indices, 4);
            const __m256 rightGain = contiguous ? _mm256_loadu_ps(s.rightGain + firstSlot)
                                                : _mm256_i32gather_ps(s.rightGain, indices, 4);
            alignas(32) float channelLeftLane[8], channelRightLane[8];
            alignas(32) float pitchRatioLane[8];
            for (int lane = 0; lane < 8; ++lane) {
                const auto channel = s.channel[slotLane[lane]];
                channelLeftLane[lane] = s.channelLeftScale[channel];
                channelRightLane[lane] = s.channelRightScale[channel];
                pitchRatioLane[lane] = s.channelPitchRatio[channel];
            }
            value = _mm256_mul_ps(value, envelope);
            __m256 outLeft = _mm256_mul_ps(value,
                _mm256_mul_ps(leftGain, _mm256_load_ps(channelLeftLane)));
            __m256 outRight = _mm256_mul_ps(value,
                _mm256_mul_ps(rightGain, _mm256_load_ps(channelRightLane)));
            const auto mask = _mm256_castsi256_ps(aliveMaskI);
            outLeft = _mm256_and_ps(outLeft, mask);
            outRight = _mm256_and_ps(outRight, mask);
            _mm256_store_ps(laneLeft, outLeft);
            _mm256_store_ps(laneRight, outRight);
            bool fastAdvance = enableFastAdvance &&
                _mm256_movemask_ps(mask) == 0xff &&
                _mm256_movemask_ps(_mm256_castsi256_ps(isLooping)) == 0xff;
            if (fastAdvance) {
                for (int lane = 0; lane < 8; ++lane) {
                    if (s.envelopeStage[slotLane[lane]] != EnvelopeStage::Sustain) {
                        fastAdvance = false;
                        break;
                    }
                }
            }
            alignas(32) float nextPhaseLane[8];
            if (fastAdvance) {
                const __m256 increment = contiguous
                    ? _mm256_loadu_ps(s.increment + firstSlot)
                    : _mm256_i32gather_ps(s.increment, indices, 4);
                const __m256 nextPhase = _mm256_add_ps(phase,
                    _mm256_mul_ps(increment, _mm256_load_ps(pitchRatioLane)));
                const __m256 loopEndFloat = _mm256_cvtepi32_ps(loopEnd);
                const __m256 beforeLoopEnd = _mm256_cmp_ps(
                    nextPhase, loopEndFloat, _CMP_LT_OQ);
                fastAdvance = _mm256_movemask_ps(beforeLoopEnd) == 0xff;
                if (fastAdvance) _mm256_store_ps(nextPhaseLane, nextPhase);
            }
            for (int lane = 0; lane < 8; ++lane) {
                mixedLeft += laneLeft[lane];
                mixedRight += laneRight[lane];
                if (fastAdvance) {
                    s.phase[slotLane[lane]] = nextPhaseLane[lane];
                } else if (s.alive[slotLane[lane]]) {
                    advance(s, slotLane[lane], retired, retiredCount);
                }
            }
        }
        for (; voice < slotCount; ++voice) {
            const auto slot = slots[voice];
            if (!s.alive[slot]) continue;
            const auto channel = s.channel[slot];
            const float value = scalarInterpolate(s, slot, sampleData) * s.envelope[slot];
            mixedLeft += value * s.leftGain[slot] * s.channelLeftScale[channel];
            mixedRight += value * s.rightGain[slot] * s.channelRightScale[channel];
            advance(s, slot, retired, retiredCount);
        }
        left[frame] = mixedLeft;
        right[frame] = mixedRight;
    }
}

} // namespace midisynth::detail
