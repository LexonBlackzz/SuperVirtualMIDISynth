#include "SVMSRenderKernels.h"
#include "SVMSEnvelope.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <immintrin.h>

namespace svms {
namespace {

float HorizontalSum(__m256 value) {
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, value);
    return ((lanes[0] + lanes[1]) + (lanes[2] + lanes[3])) +
           ((lanes[4] + lanes[5]) + (lanes[6] + lanes[7]));
}

void RenderTailScalar(const RenderSpanContext& c, uint32_t h,
                      uint32_t frameCount) {
    VoiceSoA& v = *c.voices;
    uint32_t remaining = v.stealTailFramesRemaining[h];
    if (remaining == 0u || v.stealTailSampleBacked[h] == 0u ||
        c.sampleData == nullptr || frameCount == 0u) return;
    const uint32_t relEnd = v.stealTailRelEnd[h];
    if (relEnd < 2u) { v.stealTailFramesRemaining[h] = 0u; return; }
    float phase = (std::max)(0.0f, v.stealTailPhase[h]);
    const float step = v.stealTailPhaseInc[h];
    const uint32_t loopS = v.stealTailRelLoopS[h];
    const uint32_t loopE = v.stealTailRelLoopE[h];
    const float loopSF = v.stealTailRelLoopSF[h];
    const float loopEF = v.stealTailRelLoopEF[h];
    const bool loop = v.stealTailLoopEnabled[h] != 0u;
    const uint32_t total = v.stealTailFramesTotal[h];
    const uint32_t count = (std::min)(frameCount, remaining);
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= relEnd) {
            if (!loop) { remaining = 0u; break; }
            phase = loopSF;
            base = loopS;
        }
        uint32_t next = base + 1u;
        if (loop && next >= loopE) next = loopS;
        if (next >= relEnd) next = relEnd - 1u;
        const uint32_t firstIndex = v.stealTailSampleStart[h] + base;
        const uint32_t nextIndex = v.stealTailSampleStart[h] + next;
        if (firstIndex >= c.sampleDataFrames || nextIndex >= c.sampleDataFrames) {
            remaining = 0u;
            break;
        }
        const float first = c.sampleData[firstIndex];
        const float sample = first + (c.sampleData[nextIndex] - first) *
            (phase - static_cast<float>(base));
        const float fade = total > 1u
            ? static_cast<float>(remaining - 1u) / static_cast<float>(total - 1u)
            : 0.0f;
        const float scaled = sample * v.stealTailGain[h] * fade;
        c.outputLeft[c.frameStart + n] += scaled * v.stealTailMixGainL[h];
        c.outputRight[c.frameStart + n] += scaled * v.stealTailMixGainR[h];
        phase += step;
        if (loop && phase >= loopEF) {
            float overflow = phase - loopEF;
            const float length = loopEF - loopSF;
            if (length > 0.0f && overflow >= length)
                overflow -= std::floor(overflow / length) * length;
            phase = loopSF + overflow;
        }
        --remaining;
    }
    v.stealTailPhase[h] = phase;
    v.stealTailFramesRemaining[h] = remaining;
}

void RenderStealTailsAVX2(const RenderSpanContext& c,
                          const uint32_t* handles, uint32_t handleCount,
                          const uint32_t* frameCounts) {
    if (c.frameCount == 0u || c.sampleData == nullptr) return;
    if (c.frameCount > 4u) {
        for (uint32_t i = 0; i < handleCount; ++i)
            RenderTailScalar(c, handles[i], frameCounts[handles[i]]);
        _mm256_zeroupper();
        return;
    }
    VoiceSoA& v = *c.voices;
    __m256 sumL[4] = {_mm256_setzero_ps(), _mm256_setzero_ps(),
                      _mm256_setzero_ps(), _mm256_setzero_ps()};
    __m256 sumR[4] = {_mm256_setzero_ps(), _mm256_setzero_ps(),
                      _mm256_setzero_ps(), _mm256_setzero_ps()};
    uint32_t position = 0u;
    for (; position + 8u <= handleCount; position += 8u) {
        const __m256i h = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(handles + position));
        alignas(32) uint32_t hs[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(hs), h);
        bool valid = true;
        for (uint32_t lane = 0; lane < 8u; ++lane) {
            const uint32_t x = hs[lane];
            const float phase = v.stealTailPhase[x];
            const float step = v.stealTailPhaseInc[x];
            const float loopEnd = v.stealTailRelLoopEF[x];
            valid = valid && frameCounts[x] == c.frameCount &&
                v.stealTailFramesRemaining[x] >= c.frameCount &&
                v.stealTailSampleBacked[x] != 0u &&
                v.stealTailLoopEnabled[x] != 0u &&
                v.stealTailRelEnd[x] >= 2u &&
                v.stealTailRelLoopS[x] < v.stealTailRelLoopE[x] &&
                v.stealTailSampleStart[x] + v.stealTailRelEnd[x] <=
                    c.sampleDataFrames && phase >= 0.0f && step >= 0.0f &&
                phase + step * static_cast<float>(c.frameCount - 1u) < loopEnd &&
                step < loopEnd - v.stealTailRelLoopSF[x];
        }
        if (!valid) {
            for (uint32_t lane = 0; lane < 8u; ++lane)
                RenderTailScalar(c, hs[lane], frameCounts[hs[lane]]);
            continue;
        }
        __m256 phase = _mm256_i32gather_ps(v.stealTailPhase, h, 4);
        const __m256 step = _mm256_i32gather_ps(v.stealTailPhaseInc, h, 4);
        const __m256 gain = _mm256_i32gather_ps(v.stealTailGain, h, 4);
        const __m256 gainL = _mm256_mul_ps(
            gain, _mm256_i32gather_ps(v.stealTailMixGainL, h, 4));
        const __m256 gainR = _mm256_mul_ps(
            gain, _mm256_i32gather_ps(v.stealTailMixGainR, h, 4));
        const __m256 loopSF = _mm256_i32gather_ps(v.stealTailRelLoopSF, h, 4);
        const __m256 loopEF = _mm256_i32gather_ps(v.stealTailRelLoopEF, h, 4);
        const __m256i sampleStart = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.stealTailSampleStart), h, 4);
        const __m256i loopS = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.stealTailRelLoopS), h, 4);
        const __m256i loopE = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.stealTailRelLoopE), h, 4);
        __m256 remaining = _mm256_cvtepi32_ps(_mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.stealTailFramesRemaining), h, 4));
        const __m256 total = _mm256_cvtepi32_ps(_mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.stealTailFramesTotal), h, 4));
        for (uint32_t frame = 0; frame < c.frameCount; ++frame) {
            const __m256i base = _mm256_cvttps_epi32(phase);
            const __m256i nextRaw = _mm256_add_epi32(base, _mm256_set1_epi32(1));
            const __m256i wraps = _mm256_cmpgt_epi32(
                nextRaw, _mm256_sub_epi32(loopE, _mm256_set1_epi32(1)));
            const __m256i next = _mm256_blendv_epi8(nextRaw, loopS, wraps);
            const __m256 first = _mm256_i32gather_ps(c.sampleData,
                _mm256_add_epi32(sampleStart, base), 4);
            const __m256 second = _mm256_i32gather_ps(c.sampleData,
                _mm256_add_epi32(sampleStart, next), 4);
            const __m256 fraction = _mm256_sub_ps(
                phase, _mm256_cvtepi32_ps(base));
            const __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            const __m256 fade = _mm256_div_ps(
                _mm256_sub_ps(remaining, _mm256_set1_ps(1.0f)),
                _mm256_sub_ps(total, _mm256_set1_ps(1.0f)));
            sumL[frame] = _mm256_add_ps(sumL[frame],
                _mm256_mul_ps(_mm256_mul_ps(sample, fade), gainL));
            sumR[frame] = _mm256_add_ps(sumR[frame],
                _mm256_mul_ps(_mm256_mul_ps(sample, fade), gainR));
            remaining = _mm256_sub_ps(remaining, _mm256_set1_ps(1.0f));
            phase = _mm256_add_ps(phase, step);
        }
        const __m256 wrapped = _mm256_add_ps(loopSF,
            _mm256_sub_ps(phase, loopEF));
        phase = _mm256_blendv_ps(phase, wrapped,
            _mm256_cmp_ps(phase, loopEF, _CMP_GE_OQ));
        alignas(32) float phases[8];
        _mm256_store_ps(phases, phase);
        for (uint32_t lane = 0; lane < 8u; ++lane) {
            v.stealTailPhase[hs[lane]] = phases[lane];
            v.stealTailFramesRemaining[hs[lane]] -= c.frameCount;
        }
    }
    for (; position < handleCount; ++position)
        RenderTailScalar(c, handles[position], frameCounts[handles[position]]);
    for (uint32_t frame = 0; frame < c.frameCount; ++frame) {
        c.outputLeft[c.frameStart + frame] += HorizontalSum(sumL[frame]);
        c.outputRight[c.frameStart + frame] += HorizontalSum(sumR[frame]);
    }
    _mm256_zeroupper();
}

void RecordRetirement(const RenderSpanContext& c, uint32_t handle,
                      uint32_t frameOffset) {
    if (c.retirements != nullptr && c.retirementCount != nullptr &&
        c.activePositions != nullptr) {
        c.retirements[(*c.retirementCount)++] = {
            handle, frameOffset, c.activePositions[handle]};
    } else {
        // Dense tile state is a disposable render snapshot. Authoritative
        // lifecycle was already committed by the exact-frame planner.
        c.voices->state[handle] = static_cast<uint8_t>(VoiceState::Free);
    }
}

bool ValidateLoopVoice(const RenderSpanContext& c, uint32_t handle) {
    const VoiceSoA& v = *c.voices;
    return v.relEnd[handle] >= 2u &&
        v.relLoopS[handle] < v.relLoopE[handle] &&
        v.relLoopE[handle] <= v.relEnd[handle] &&
        v.sampleStart[handle] < c.sampleDataFrames &&
        v.relEnd[handle] <= c.sampleDataFrames - v.sampleStart[handle] &&
        v.phaseIncs[handle] >= 0.0f &&
        v.phaseIncs[handle] < v.relLoopEF[handle] - v.relLoopSF[handle];
}

uint32_t RenderSustainedLoopFramesAVX2(const RenderSpanContext& c,
                                       uint32_t handle) {
    VoiceSoA& v = *c.voices;
    if (!ValidateLoopVoice(c, handle)) {
        return ScalarRenderSustainedLoop(
            v, handle, c.sampleData, c.sampleDataFrames, c.outputLeft,
            c.outputRight, c.frameStart, c.frameCount);
    }

    float phase = (std::max)(0.0f, v.phases[handle]);
    const float step = v.phaseIncs[handle];
    const float loopStart = v.relLoopSF[handle];
    const float loopEnd = v.relLoopEF[handle];
    const float loopLength = loopEnd - loopStart;
    const float gainL = v.renderGainL[handle];
    const float gainR = v.renderGainR[handle];
    const float* region = c.sampleData + v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    const __m256 stepVector = _mm256_set1_ps(step);
    const __m256 gainLVector = _mm256_set1_ps(gainL);
    const __m256 gainRVector = _mm256_set1_ps(gainR);
    const __m256i one = _mm256_set1_epi32(1);
    uint32_t frame = 0u;

    while (frame < c.frameCount) {
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (overflow >= loopLength)
                overflow -= std::floor(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        const float lastPhase = phase + step * 7.0f;
        if (frame + 8u <= c.frameCount && lastPhase < loopEnd - 1.0f) {
            const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            const __m256 first = _mm256_i32gather_ps(region, bases, 4);
            const __m256 second = _mm256_i32gather_ps(
                region, _mm256_add_epi32(bases, one), 4);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            const __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            _mm256_storeu_ps(outL + frame, _mm256_add_ps(
                _mm256_loadu_ps(outL + frame),
                _mm256_mul_ps(sample, gainLVector)));
            _mm256_storeu_ps(outR + frame, _mm256_add_ps(
                _mm256_loadu_ps(outR + frame),
                _mm256_mul_ps(sample, gainRVector)));
            // Preserve the scalar cursor's repeated-add rounding.
            for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex)
                phase += step;
            frame += 8u;
            continue;
        }

        uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= v.relEnd[handle]) {
            phase = loopStart;
            base = v.relLoopS[handle];
        }
        uint32_t next = base + 1u;
        if (next >= v.relLoopE[handle]) next = v.relLoopS[handle];
        const float fraction = phase - static_cast<float>(base);
        const float first = region[base];
        const float sample = first + (region[next] - first) * fraction;
        outL[frame] += sample * gainL;
        outR[frame] += sample * gainR;
        phase += step;
        ++frame;
    }
    // Match the existing AVX2 short-span cursor contract: wrapping happens
    // before the next rendered sample, so the final increment may remain
    // just past loopEnd in the stored snapshot.
    v.phases[handle] = phase;
    return UINT32_MAX;
}

uint32_t RenderReleaseLoopScalar(const RenderSpanContext& c,
                                 uint32_t handle) {
    VoiceSoA& v = *c.voices;
    if (!ValidateLoopVoice(c, handle)) return 0u;
    float phase = (std::max)(0.0f, v.phases[handle]);
    float gain = v.currentGain[handle];
    uint32_t remaining = v.releaseSamplesRemaining[handle];
    const float decay = v.releaseDecay[handle];
    const float step = v.phaseIncs[handle];
    const float loopStart = v.relLoopSF[handle];
    const float loopEnd = v.relLoopEF[handle];
    const float loopLength = loopEnd - loopStart;
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const float* region = c.sampleData + v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    uint32_t retiredAt = UINT32_MAX;
    for (uint32_t frame = 0u; frame < c.frameCount; ++frame) {
        uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= v.relEnd[handle]) {
            phase = loopStart;
            base = v.relLoopS[handle];
        }
        uint32_t next = base + 1u;
        if (next >= v.relLoopE[handle]) next = v.relLoopS[handle];
        const float fraction = phase - static_cast<float>(base);
        const float first = region[base];
        const float sample = first + (region[next] - first) * fraction;
        bool finished = remaining == 0u;
        if (!finished) {
            gain *= decay;
            if (remaining != UINT32_MAX) {
                --remaining;
                finished = remaining == 0u;
            }
        }
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        phase += step;
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (overflow >= loopLength)
                overflow -= std::floor(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        if (finished ||
            (remaining == UINT32_MAX && gain < kVoiceRetireThreshold)) {
            retiredAt = frame;
            break;
        }
    }
    v.phases[handle] = phase;
    v.currentGain[handle] = gain;
    v.releaseSamplesRemaining[handle] = remaining;
    return retiredAt;
}

uint32_t RenderReleaseLoopFramesAVX2(const RenderSpanContext& c,
                                     uint32_t handle) {
    VoiceSoA& v = *c.voices;
    if (!ValidateLoopVoice(c, handle)) return 0u;
    float phase = (std::max)(0.0f, v.phases[handle]);
    float gain = v.currentGain[handle];
    uint32_t remaining = v.releaseSamplesRemaining[handle];
    const float decay = v.releaseDecay[handle];
    const float step = v.phaseIncs[handle];
    const float loopStart = v.relLoopSF[handle];
    const float loopEnd = v.relLoopEF[handle];
    const float loopLength = loopEnd - loopStart;
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const float* region = c.sampleData + v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    const __m256 stepVector = _mm256_set1_ps(step);
    const __m256i one = _mm256_set1_epi32(1);
    uint32_t frame = 0u;
    uint32_t retiredAt = UINT32_MAX;
    while (frame < c.frameCount) {
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (overflow >= loopLength)
                overflow -= std::floor(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        float futureGain = gain;
        alignas(32) float gains[8];
        for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex) {
            futureGain *= decay;
            gains[laneIndex] = futureGain;
        }
        const bool countdownSafe = remaining == UINT32_MAX || remaining > 8u;
        const bool thresholdSafe = remaining != UINT32_MAX ||
            gains[7] >= kVoiceRetireThreshold;
        const float lastPhase = phase + step * 7.0f;
        if (frame + 8u <= c.frameCount && countdownSafe && thresholdSafe &&
            lastPhase < loopEnd - 1.0f) {
            const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            const __m256 first = _mm256_i32gather_ps(region, bases, 4);
            const __m256 second = _mm256_i32gather_ps(
                region, _mm256_add_epi32(bases, one), 4);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            const __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            const __m256 scaled = _mm256_mul_ps(sample, _mm256_load_ps(gains));
            _mm256_storeu_ps(outL + frame, _mm256_add_ps(
                _mm256_loadu_ps(outL + frame),
                _mm256_mul_ps(scaled, _mm256_set1_ps(mixL))));
            _mm256_storeu_ps(outR + frame, _mm256_add_ps(
                _mm256_loadu_ps(outR + frame),
                _mm256_mul_ps(scaled, _mm256_set1_ps(mixR))));
            gain = futureGain;
            if (remaining != UINT32_MAX) remaining -= 8u;
            for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex)
                phase += step;
            frame += 8u;
            continue;
        }

        uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= v.relEnd[handle]) {
            phase = loopStart;
            base = v.relLoopS[handle];
        }
        uint32_t next = base + 1u;
        if (next >= v.relLoopE[handle]) next = v.relLoopS[handle];
        const float fraction = phase - static_cast<float>(base);
        const float first = region[base];
        const float sample = first + (region[next] - first) * fraction;
        bool finished = remaining == 0u;
        if (!finished) {
            gain *= decay;
            if (remaining != UINT32_MAX) {
                --remaining;
                finished = remaining == 0u;
            }
        }
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        phase += step;
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (overflow >= loopLength)
                overflow -= std::floor(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        if (finished ||
            (remaining == UINT32_MAX && gain < kVoiceRetireThreshold)) {
            retiredAt = frame;
            break;
        }
        ++frame;
    }
    v.phases[handle] = phase;
    v.currentGain[handle] = gain;
    v.releaseSamplesRemaining[handle] = remaining;
    return retiredAt;
}

void RenderReleaseLoopShortAVX2(const RenderSpanContext& c,
                                const uint32_t* handles,
                                uint32_t handleCount) {
    VoiceSoA& v = *c.voices;
    __m256 sumsL[4] = {_mm256_setzero_ps(), _mm256_setzero_ps(),
                       _mm256_setzero_ps(), _mm256_setzero_ps()};
    __m256 sumsR[4] = {_mm256_setzero_ps(), _mm256_setzero_ps(),
                       _mm256_setzero_ps(), _mm256_setzero_ps()};
    uint32_t position = 0u;
    for (; position + 8u <= handleCount; position += 8u) {
        const __m256i h = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(handles + position));
        alignas(32) uint32_t hs[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(hs), h);
        bool valid = true;
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            float finalGain = v.currentGain[handle];
            for (uint32_t frame = 0u; frame < c.frameCount; ++frame)
                finalGain *= v.releaseDecay[handle];
            const uint32_t remaining = v.releaseSamplesRemaining[handle];
            valid = valid && ValidateLoopVoice(c, handle) &&
                (remaining == UINT32_MAX || remaining > c.frameCount) &&
                (remaining != UINT32_MAX ||
                 finalGain >= kVoiceRetireThreshold);
        }
        if (!valid) {
            for (uint32_t lane = 0u; lane < 8u; ++lane) {
                const uint32_t retiredAt = RenderReleaseLoopScalar(c, hs[lane]);
                if (retiredAt != UINT32_MAX)
                    RecordRetirement(c, hs[lane], retiredAt);
            }
            continue;
        }

        __m256 phase = _mm256_i32gather_ps(v.phases, h, 4);
        phase = _mm256_max_ps(phase, _mm256_setzero_ps());
        const __m256 step = _mm256_i32gather_ps(v.phaseIncs, h, 4);
        __m256 gain = _mm256_i32gather_ps(v.currentGain, h, 4);
        const __m256 decay = _mm256_i32gather_ps(v.releaseDecay, h, 4);
        const __m256 mixL = _mm256_i32gather_ps(v.mixGainL, h, 4);
        const __m256 mixR = _mm256_i32gather_ps(v.mixGainR, h, 4);
        const __m256 loopStartF = _mm256_i32gather_ps(v.relLoopSF, h, 4);
        const __m256 loopEndF = _mm256_i32gather_ps(v.relLoopEF, h, 4);
        const __m256i sampleStart = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.sampleStart), h, 4);
        const __m256i loopStart = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.relLoopS), h, 4);
        const __m256i loopEnd = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.relLoopE), h, 4);
        for (uint32_t frame = 0u; frame < c.frameCount; ++frame) {
            const __m256 pastLoop = _mm256_cmp_ps(
                phase, loopEndF, _CMP_GE_OQ);
            phase = _mm256_blendv_ps(phase,
                _mm256_add_ps(loopStartF, _mm256_sub_ps(phase, loopEndF)),
                pastLoop);
            const __m256i base = _mm256_cvttps_epi32(phase);
            const __m256i nextRaw = _mm256_add_epi32(
                base, _mm256_set1_epi32(1));
            const __m256i wraps = _mm256_cmpgt_epi32(
                nextRaw, _mm256_sub_epi32(loopEnd, _mm256_set1_epi32(1)));
            const __m256i next = _mm256_blendv_epi8(nextRaw, loopStart, wraps);
            const __m256 first = _mm256_i32gather_ps(c.sampleData,
                _mm256_add_epi32(sampleStart, base), 4);
            const __m256 second = _mm256_i32gather_ps(c.sampleData,
                _mm256_add_epi32(sampleStart, next), 4);
            const __m256 fraction = _mm256_sub_ps(
                phase, _mm256_cvtepi32_ps(base));
            const __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            gain = _mm256_mul_ps(gain, decay);
            const __m256 scaled = _mm256_mul_ps(sample, gain);
            sumsL[frame] = _mm256_add_ps(
                sumsL[frame], _mm256_mul_ps(scaled, mixL));
            sumsR[frame] = _mm256_add_ps(
                sumsR[frame], _mm256_mul_ps(scaled, mixR));
            phase = _mm256_add_ps(phase, step);
        }
        const __m256 wrapped = _mm256_add_ps(
            loopStartF, _mm256_sub_ps(phase, loopEndF));
        phase = _mm256_blendv_ps(phase, wrapped,
            _mm256_cmp_ps(phase, loopEndF, _CMP_GE_OQ));
        alignas(32) float phases[8], gains[8];
        _mm256_store_ps(phases, phase);
        _mm256_store_ps(gains, gain);
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            v.phases[handle] = phases[lane];
            v.currentGain[handle] = gains[lane];
            if (v.releaseSamplesRemaining[handle] != UINT32_MAX)
                v.releaseSamplesRemaining[handle] -= c.frameCount;
        }
    }
    for (; position < handleCount; ++position) {
        const uint32_t handle = handles[position];
        const uint32_t retiredAt = RenderReleaseLoopScalar(c, handle);
        if (retiredAt != UINT32_MAX)
            RecordRetirement(c, handle, retiredAt);
    }
    for (uint32_t frame = 0u; frame < c.frameCount; ++frame) {
        c.outputLeft[c.frameStart + frame] += HorizontalSum(sumsL[frame]);
        c.outputRight[c.frameStart + frame] += HorizontalSum(sumsR[frame]);
    }
}

bool RenderReleaseLoopAVX2(const RenderSpanContext& context,
                           const uint32_t* handles,
                           uint32_t handleCount) {
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    if (context.frameCount <= 4u) {
        RenderReleaseLoopShortAVX2(context, handles, handleCount);
        _mm256_zeroupper();
        return true;
    }
    for (uint32_t position = 0u; position < handleCount; ++position) {
        const uint32_t handle = handles[position];
        const uint32_t retiredAt = RenderReleaseLoopFramesAVX2(
            context, handle);
        if (retiredAt != UINT32_MAX)
            RecordRetirement(context, handle, retiredAt);
    }
    _mm256_zeroupper();
    return true;
}

bool RenderSustainedLoopAVX2(const RenderSpanContext& context,
                             const uint32_t* handles,
                             uint32_t handleCount) {
    VoiceSoA& v = *context.voices;
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    if (context.frameCount > 4u) {
        for (uint32_t i = 0; i < handleCount; ++i) {
            RenderSustainedLoopFramesAVX2(context, handles[i]);
        }
        _mm256_zeroupper();
        return true;
    }

    const bool denseHandles = context.voiceCapacity >= 8u &&
        handleCount * 4u >= context.voiceCapacity * 3u;
    const uint32_t iterationCount = denseHandles ? context.voiceCapacity : handleCount;
    __m256 accumulatedLeft[4] = {
        _mm256_setzero_ps(), _mm256_setzero_ps(),
        _mm256_setzero_ps(), _mm256_setzero_ps()};
    __m256 accumulatedRight[4] = {
        _mm256_setzero_ps(), _mm256_setzero_ps(),
        _mm256_setzero_ps(), _mm256_setzero_ps()};
    uint32_t position = 0u;
    for (; position + 8u <= iterationCount; position += 8u) {
        const __m256i h = denseHandles
            ? _mm256_setr_epi32(static_cast<int>(position + 0u),
                static_cast<int>(position + 1u), static_cast<int>(position + 2u),
                static_cast<int>(position + 3u), static_cast<int>(position + 4u),
                static_cast<int>(position + 5u), static_cast<int>(position + 6u),
                static_cast<int>(position + 7u))
            : _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(handles + position));
        if (denseHandles) {
            bool allSustained = true;
            for (uint32_t lane = 0; lane < 8u; ++lane) {
                allSustained = allSustained &&
                    v.renderClass[position + lane] ==
                        static_cast<uint8_t>(VoiceRenderClass::SustainedLoop);
            }
            if (!allSustained) {
                for (uint32_t lane = 0; lane < 8u; ++lane) {
                    const uint32_t handle = position + lane;
                    if (v.renderClass[handle] ==
                        static_cast<uint8_t>(VoiceRenderClass::SustainedLoop)) {
                        ScalarRenderSustainedLoop(v, handle, context.sampleData,
                            context.sampleDataFrames, context.outputLeft,
                            context.outputRight, context.frameStart,
                            context.frameCount);
                    }
                }
                continue;
            }
        }
        __m256 phase = denseHandles ? _mm256_load_ps(v.phases + position)
            : _mm256_i32gather_ps(v.phases, h, 4);
        const __m256 step = denseHandles ? _mm256_load_ps(v.phaseIncs + position)
            : _mm256_i32gather_ps(v.phaseIncs, h, 4);
        const __m256 gainL = denseHandles ? _mm256_load_ps(v.renderGainL + position)
            : _mm256_i32gather_ps(v.renderGainL, h, 4);
        const __m256 gainR = denseHandles ? _mm256_load_ps(v.renderGainR + position)
            : _mm256_i32gather_ps(v.renderGainR, h, 4);
        const __m256 loopStartF = denseHandles ? _mm256_load_ps(v.relLoopSF + position)
            : _mm256_i32gather_ps(v.relLoopSF, h, 4);
        const __m256 loopEndF = denseHandles ? _mm256_load_ps(v.relLoopEF + position)
            : _mm256_i32gather_ps(v.relLoopEF, h, 4);
        const __m256 loopLength = _mm256_sub_ps(loopEndF, loopStartF);

        alignas(32) uint32_t hs[8];
        alignas(32) float steps[8], lengths[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(hs), h);
        _mm256_store_ps(steps, step);
        _mm256_store_ps(lengths, loopLength);
        bool valid = true;
        for (uint32_t lane = 0; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            valid = valid && v.relEnd[handle] >= 2u &&
                v.relLoopS[handle] < v.relLoopE[handle] &&
                v.sampleStart[handle] < context.sampleDataFrames &&
                steps[lane] >= 0.0f && steps[lane] < lengths[lane];
        }
        if (!valid) {
            for (uint32_t lane = 0; lane < 8u; ++lane) {
                ScalarRenderSustainedLoop(v, hs[lane], context.sampleData,
                    context.sampleDataFrames, context.outputLeft,
                    context.outputRight, context.frameStart, context.frameCount);
            }
            continue;
        }

        const __m256i sampleStart = denseHandles
            ? _mm256_load_si256(reinterpret_cast<const __m256i*>(v.sampleStart + position))
            : _mm256_i32gather_epi32(reinterpret_cast<const int*>(v.sampleStart), h, 4);
        const __m256i loopStart = denseHandles
            ? _mm256_load_si256(reinterpret_cast<const __m256i*>(v.relLoopS + position))
            : _mm256_i32gather_epi32(reinterpret_cast<const int*>(v.relLoopS), h, 4);
        const __m256i loopEnd = denseHandles
            ? _mm256_load_si256(reinterpret_cast<const __m256i*>(v.relLoopE + position))
            : _mm256_i32gather_epi32(reinterpret_cast<const int*>(v.relLoopE), h, 4);
        const __m256 zero = _mm256_setzero_ps();
        phase = _mm256_max_ps(phase, zero);

        for (uint32_t frame = 0; frame < context.frameCount; ++frame) {
            const __m256 pastLoop = _mm256_cmp_ps(phase, loopEndF, _CMP_GE_OQ);
            phase = _mm256_blendv_ps(
                phase, _mm256_add_ps(loopStartF, _mm256_sub_ps(phase, loopEndF)),
                pastLoop);
            const __m256i base = _mm256_cvttps_epi32(phase);
            const __m256i nextRaw = _mm256_add_epi32(base, _mm256_set1_epi32(1));
            const __m256i wrap = _mm256_cmpgt_epi32(nextRaw,
                _mm256_sub_epi32(loopEnd, _mm256_set1_epi32(1)));
            const __m256i next = _mm256_blendv_epi8(nextRaw, loopStart, wrap);
            const __m256i baseIndex = _mm256_add_epi32(sampleStart, base);
            const __m256i nextIndex = _mm256_add_epi32(sampleStart, next);
            const __m256 first = _mm256_i32gather_ps(context.sampleData, baseIndex, 4);
            const __m256 second = _mm256_i32gather_ps(context.sampleData, nextIndex, 4);
            const __m256 fraction = _mm256_sub_ps(phase, _mm256_cvtepi32_ps(base));
            const __m256 sample = _mm256_add_ps(
                first, _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            accumulatedLeft[frame] = _mm256_add_ps(
                accumulatedLeft[frame], _mm256_mul_ps(sample, gainL));
            accumulatedRight[frame] = _mm256_add_ps(
                accumulatedRight[frame], _mm256_mul_ps(sample, gainR));
            phase = _mm256_add_ps(phase, step);
        }
        alignas(32) float phases[8];
        _mm256_store_ps(phases, phase);
        if (denseHandles) _mm256_store_ps(v.phases + position, phase);
        else for (uint32_t lane = 0; lane < 8u; ++lane) v.phases[hs[lane]] = phases[lane];
    }
    for (; position < iterationCount; ++position) {
        const uint32_t handle = denseHandles ? position : handles[position];
        if (!denseHandles || v.renderClass[handle] ==
            static_cast<uint8_t>(VoiceRenderClass::SustainedLoop)) {
            ScalarRenderSustainedLoop(v, handle, context.sampleData,
                context.sampleDataFrames, context.outputLeft, context.outputRight,
                context.frameStart, context.frameCount);
        }
    }
    for (uint32_t frame = 0; frame < context.frameCount; ++frame) {
        context.outputLeft[context.frameStart + frame] +=
            HorizontalSum(accumulatedLeft[frame]);
        context.outputRight[context.frameStart + frame] +=
            HorizontalSum(accumulatedRight[frame]);
    }
    _mm256_zeroupper();
    return true;
}

} // namespace

bool BuildVolatileStealKeysAVX2(
    const uint32_t* handles, uint32_t handleCount,
    const uint64_t* birthFrames, const float* currentGains,
    const float* outputGains, const uint32_t* activePositions,
    uint64_t currentFrame, float gainScale, uint64_t* outputKeys,
    uint32_t* outputHandles, uint32_t* inverseHeapPositions) {
    if (!handles || !birthFrames || !currentGains || !outputGains ||
        !activePositions || !outputKeys || !outputHandles ||
        !inverseHeapPositions || currentFrame >
            static_cast<uint64_t>(INT32_MAX)) {
        return false;
    }

    const __m256i current = _mm256_set1_epi32(
        static_cast<int32_t>(currentFrame));
    const __m256 ageScale = _mm256_set1_ps(1.0f / 256.0f);
    const __m256 commonAge = _mm256_set1_ps(
        static_cast<float>(currentFrame) * (1.0f / 256.0f));
    const __m256 gainScaleVector = _mm256_set1_ps(gainScale);
    const __m256 signMask = _mm256_set1_ps(-0.0f);

    auto encode = [](float score, uint32_t activePosition) {
        if (score == 0.0f) score = 0.0f;
        uint32_t bits = 0u;
        std::memcpy(&bits, &score, sizeof(bits));
        const uint32_t ordered = (bits & 0x80000000u) != 0u
            ? ~bits : bits ^ 0x80000000u;
        return (static_cast<uint64_t>(ordered) << 32u) |
            (UINT32_MAX - activePosition);
    };

    uint32_t position = 0u;
    for (; position + 8u <= handleCount; position += 8u) {
        const __m256i h = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(handles + position));
        // currentFrame <= INT32_MAX and birthFrame <= currentFrame for every
        // live voice, so gathering the low dword is an exact unsigned age in
        // the range where cvtepi32_ps matches scalar uint32_t conversion.
        const __m256i births = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(birthFrames), h, 8);
        const __m256 ages = _mm256_cvtepi32_ps(
            _mm256_sub_epi32(current, births));
        const __m256 gains = _mm256_andnot_ps(
            signMask, _mm256_i32gather_ps(currentGains, h, 4));
        const __m256 levels = _mm256_mul_ps(
            gains, _mm256_i32gather_ps(outputGains, h, 4));
        const __m256 score = _mm256_sub_ps(
            _mm256_sub_ps(_mm256_mul_ps(ages, ageScale),
                          _mm256_mul_ps(levels, gainScaleVector)),
            commonAge);
        const __m256i active = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(activePositions), h, 4);

        // Encode eight float priorities and active-position ties into the
        // same monotonic uint64_t ordering used by the scalar tournament.
        // Normalizing signed zero retains EncodeStableWinnerKey exactly.
        const __m256 zeroMask = _mm256_cmp_ps(
            score, _mm256_setzero_ps(), _CMP_EQ_OQ);
        const __m256 normalizedScore = _mm256_andnot_ps(zeroMask, score);
        const __m256i scoreBits = _mm256_castps_si256(normalizedScore);
        const __m256i sign = _mm256_srai_epi32(scoreBits, 31);
        const __m256i ordered = _mm256_xor_si256(
            scoreBits, _mm256_or_si256(
                sign, _mm256_set1_epi32(static_cast<int32_t>(0x80000000u))));
        const __m256i inverseActive = _mm256_xor_si256(
            active, _mm256_set1_epi32(-1));
        const __m256i lowPairs = _mm256_unpacklo_epi32(
            inverseActive, ordered);
        const __m256i highPairs = _mm256_unpackhi_epi32(
            inverseActive, ordered);
        const __m256i keys0123 = _mm256_permute2x128_si256(
            lowPairs, highPairs, 0x20);
        const __m256i keys4567 = _mm256_permute2x128_si256(
            lowPairs, highPairs, 0x31);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(outputKeys + position), keys0123);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(outputKeys + position + 4u), keys4567);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(outputHandles + position), h);
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint32_t heapPosition = position + lane;
            const uint32_t handle = handles[heapPosition];
            inverseHeapPositions[handle] = heapPosition;
        }
    }

    const float commonAgeScalar =
        static_cast<float>(currentFrame) * (1.0f / 256.0f);
    for (; position < handleCount; ++position) {
        const uint32_t handle = handles[position];
        const uint64_t rawAge = currentFrame > birthFrames[handle]
            ? currentFrame - birthFrames[handle] : 0u;
        const uint32_t age = rawAge > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(rawAge);
        const float ageUnits = static_cast<float>(age) * (1.0f / 256.0f);
        const float level = std::fabs(currentGains[handle]) *
            outputGains[handle];
        const float score = ageUnits - level * gainScale;
        outputKeys[position] = encode(
            score - commonAgeScalar, activePositions[handle]);
        outputHandles[position] = handle;
        inverseHeapPositions[handle] = position;
    }
    _mm256_zeroupper();
    return true;
}

const RenderKernelSet& GetAVX2RenderKernelSet() {
    static const RenderKernelSet set = [] {
        RenderKernelSet result{};
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::SustainedLoop)] =
            RenderSustainedLoopAVX2;
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::ReleaseLoop)] =
            RenderReleaseLoopAVX2;
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::TransientLoop)] =
            ScalarRenderTransientLoopClass;
        result.backend = RenderBackend::AVX2;
        result.name = "avx2";
        return result;
    }();
    return set;
}

} // namespace svms
