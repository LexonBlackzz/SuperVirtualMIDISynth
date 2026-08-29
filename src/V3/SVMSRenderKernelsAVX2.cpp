#include "SVMSRenderKernels.h"
#include "SVMSEnvelope.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

// ════════════════════════════════════════════════════════════════════════
// 16-bit sample-store gathers.
//
// The sample store holds raw SF2 int16 values (the previous float store
// held exact float(int16) values, so converting on load is identical
// math).  AVX2 gathers are 4-byte granular, so each 32-bit word covering
// elements [2k, 2k+1] is gathered and the wanted half is extracted by
// index parity.  Gathering the word for element e+1 can touch one int16
// beyond the last validated frame; every store is therefore allocated
// with eight zero elements of trailing padding.
// ════════════════════════════════════════════════════════════════════════
inline __m256 GatherSampleAVX2(const int16_t* data, __m256i elem) {
    const __m256i one = _mm256_set1_epi32(1);
    const __m256i word = _mm256_srai_epi32(elem, 1);
    const __m256i parity = _mm256_and_si256(elem, one);
    const __m256i oddMask = _mm256_cmpeq_epi32(parity, one);
    const __m256i w0 = _mm256_i32gather_epi32(
        reinterpret_cast<const int*>(data), word, 4);
    const __m256i w1 = _mm256_i32gather_epi32(
        reinterpret_cast<const int*>(data), _mm256_add_epi32(word, one), 4);
    // Little-endian words: low half = element 2k, high half = 2k + 1.
    // 1/32768 is a power of two, so the scale is exact and bit-identical to
    // the previous float store's int16/32768 values.
    const __m256 int16Scale = _mm256_set1_ps(1.0f / 32768.0f);
    const __m256i low = _mm256_srai_epi32(_mm256_slli_epi32(w0, 16), 16);
    const __m256i high = _mm256_srai_epi32(w0, 16);
    return _mm256_mul_ps(_mm256_cvtepi32_ps(
        _mm256_blendv_epi8(low, high, oddMask)), int16Scale);
}

// Pair gather for the time-lane kernels where the right neighbour is
// provably elem + 1 (chunk guards exclude loop wraps), sharing the two
// word gathers between both lanes of the lerp.
inline void GatherSamplePairAVX2(const int16_t* data, __m256i elem,
                                 __m256& first, __m256& second) {
    const __m256i one = _mm256_set1_epi32(1);
    const __m256i word = _mm256_srai_epi32(elem, 1);
    const __m256i parity = _mm256_and_si256(elem, one);
    const __m256i oddMask = _mm256_cmpeq_epi32(parity, one);
    const __m256i w0 = _mm256_i32gather_epi32(
        reinterpret_cast<const int*>(data), word, 4);
    const __m256i w1 = _mm256_i32gather_epi32(
        reinterpret_cast<const int*>(data), _mm256_add_epi32(word, one), 4);
    const __m256i low = _mm256_srai_epi32(_mm256_slli_epi32(w0, 16), 16);
    const __m256i high = _mm256_srai_epi32(w0, 16);
    const __m256i lowNext = _mm256_srai_epi32(_mm256_slli_epi32(w1, 16), 16);
    const __m256 int16Scale = _mm256_set1_ps(1.0f / 32768.0f);
    first = _mm256_mul_ps(_mm256_cvtepi32_ps(
        _mm256_blendv_epi8(low, high, oddMask)), int16Scale);
    second = _mm256_mul_ps(_mm256_cvtepi32_ps(
        _mm256_blendv_epi8(high, lowNext, oddMask)), int16Scale);
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
        const float first = static_cast<float>(c.sampleData[firstIndex]) * (1.0f / 32768.0f);
        const float sample = first + (static_cast<float>(c.sampleData[nextIndex]) * (1.0f / 32768.0f) - first) *
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
            const __m256 first = GatherSampleAVX2(c.sampleData,
                _mm256_add_epi32(sampleStart, base));
            const __m256 second = GatherSampleAVX2(c.sampleData,
                _mm256_add_epi32(sampleStart, next));
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
    const int16_t* region = c.sampleData + v.sampleStart[handle];
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
            __m256 first, second;
            GatherSamplePairAVX2(region, bases, first, second);
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
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        const float sample = first + (static_cast<float>(region[next]) * (1.0f / 32768.0f) - first) * fraction;
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
    const int16_t* region = c.sampleData + v.sampleStart[handle];
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
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        const float sample = first + (static_cast<float>(region[next]) * (1.0f / 32768.0f) - first) * fraction;
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
    const int16_t* region = c.sampleData + v.sampleStart[handle];
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
            __m256 first, second;
            GatherSamplePairAVX2(region, bases, first, second);
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
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        const float sample = first + (static_cast<float>(region[next]) * (1.0f / 32768.0f) - first) * fraction;
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
            const __m256 first = GatherSampleAVX2(c.sampleData,
                _mm256_add_epi32(sampleStart, base));
            const __m256 second = GatherSampleAVX2(c.sampleData,
                _mm256_add_epi32(sampleStart, next));
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
    // Per-voice phase rotation: release spans are a minority of the voice
    // budget, so refuse and let the (rotation-hooked) scalar span kernels
    // take over rather than carrying filter state through every release
    // sub-path.
    if (context.voices->rot != nullptr) return false;
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
// ════════════════════════════════════════════════════════════════════════
// Transient-loop kernel (looping attack/decay voices).
//
// Mirrors RenderReleaseLoopFramesAVX2: the envelope is advanced scalarly
// eight steps ahead into gains[8] using the exact sequential scalar
// recurrence (bit-exact by construction — no decay^8 power tricks), and
// only the sample fetch / lerp / mix is vectorized across time.  Transient
// voices never retire mid-span; when attack+decay completes (stage reaches
// 3) the voice is reported through classChangeHandles instead of
// retirements, matching the scalar class kernel.
// ════════════════════════════════════════════════════════════════════════
void RenderTransientLoopFramesAVX2(const RenderSpanContext& c,
                                   uint32_t handle) {
    VoiceSoA& v = *c.voices;
    if (!ValidateLoopVoice(c, handle)) return;
    float phase = (std::max)(0.0f, v.phases[handle]);
    float gain = v.currentGain[handle];
    uint8_t stage = v.envelopeStage[handle];
    const uint8_t initialStage = stage;
    uint32_t attackRemaining = v.attackSamplesRemaining[handle];
    uint32_t decayRemaining = v.decaySamplesRemaining[handle];
    const float step = v.phaseIncs[handle];
    const float targetGain = v.targetGain[handle];
    const float sustainLevel = v.sustainLevel[handle];
    const float attackStep = v.attackGainStep[handle];
    const float decaySlope = v.decaySlope[handle];
    const uint32_t relEnd = v.relEnd[handle];
    const uint32_t loopS = v.relLoopS[handle];
    const uint32_t loopE = v.relLoopE[handle];
    const float loopStart = v.relLoopSF[handle];
    const float loopEnd = v.relLoopEF[handle];
    const float loopLength = loopEnd - loopStart;
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    const __m256 stepVector = _mm256_set1_ps(step);
    const __m256i one = _mm256_set1_epi32(1);
    uint32_t frame = 0u;
    while (frame < c.frameCount) {
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (overflow >= loopLength)
                overflow -= std::floor(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        // Simulate the exact scalar envelope recurrence eight steps ahead.
        // The clamps are part of the simulated sequence, so gains[] is
        // bit-exact regardless.  The chunk is only taken when the stage is
        // still the same after all eight steps — no class change can land
        // inside the vector body.
        float futureGain = gain;
        uint8_t futureStage = stage;
        uint32_t futureAttack = attackRemaining;
        uint32_t futureDecay = decayRemaining;
        alignas(32) float gains[8];
        for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex) {
            if (futureStage == 1u) {
                if (futureAttack > 0u) {
                    futureGain += attackStep;
                    --futureAttack;
                    if (futureGain > targetGain) futureGain = targetGain;
                } else {
                    futureGain = targetGain;
                }
                if (futureAttack == 0u)
                    futureStage = futureDecay > 0u ? 2u : 3u;
            }
            if (futureStage == 2u) {
                if (futureDecay > 0u) {
                    futureGain *= decaySlope;
                    --futureDecay;
                    if (futureGain < sustainLevel) futureGain = sustainLevel;
                } else {
                    futureGain = sustainLevel;
                }
                if (futureDecay == 0u) futureStage = 3u;
            }
            gains[laneIndex] = futureGain;
        }
        // lastPhase < loopEnd - 1 proves every lane's base + 1 < relLoopE,
        // so neither the sample end (base + 1 >= relEnd) nor the loop point
        // (next >= relLoopE) can be crossed inside the chunk, and both
        // gather indices stay inside the validated region.
        const float lastPhase = phase + step * 7.0f;
        if (frame + 8u <= c.frameCount && futureStage == stage &&
            lastPhase < loopEnd - 1.0f) {
            const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairAVX2(region, bases, first, second);
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
            stage = futureStage;
            attackRemaining = futureAttack;
            decayRemaining = futureDecay;
            for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex)
                phase += step;
            frame += 8u;
            continue;
        }

        // Scalar tail: the exact per-frame sequence from the reference
        // path (SVMSRenderScalar.h full-quality looping attack/decay).
        uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= relEnd) {
            phase = loopStart;
            base = loopS;
        }
        uint32_t next = base + 1u;
        if (next >= loopE) next = loopS;
        const float fraction = phase - static_cast<float>(base);
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        const float sample = first + (static_cast<float>(region[next]) * (1.0f / 32768.0f) - first) * fraction;
        if (stage == 1u) {
            if (attackRemaining > 0u) {
                gain += attackStep;
                --attackRemaining;
                if (gain > targetGain) gain = targetGain;
            } else {
                gain = targetGain;
            }
            if (attackRemaining == 0u)
                stage = decayRemaining > 0u ? 2u : 3u;
        }
        if (stage == 2u) {
            if (decayRemaining > 0u) {
                gain *= decaySlope;
                --decayRemaining;
                if (gain < sustainLevel) gain = sustainLevel;
            } else {
                gain = sustainLevel;
            }
            if (decayRemaining == 0u) stage = 3u;
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
        ++frame;
    }
    v.phases[handle] = phase;
    v.currentGain[handle] = gain;
    v.envelopeStage[handle] = stage;
    v.attackSamplesRemaining[handle] = attackRemaining;
    v.decaySamplesRemaining[handle] = decayRemaining;
    if (stage != initialStage && c.classChangeHandles != nullptr &&
        c.classChangeCount != nullptr) {
        c.classChangeHandles[(*c.classChangeCount)++] = handle;
    }
}

bool RenderTransientLoopAVX2(const RenderSpanContext& context,
                             const uint32_t* handles,
                             uint32_t handleCount) {
    if (handleCount == 0u) return true;
    if (context.frameCount == 0u || context.sampleData == nullptr) return false;
    // Per-voice phase rotation: refuse and let the (rotation-hooked) scalar
    // span kernels take over, same policy as the release kernel.
    if (context.voices->rot != nullptr) return false;
    VoiceSoA& v = *context.voices;
    // Whole-batch eligibility: any ineligible voice falls back to the
    // scalar path for the entire class.
    for (uint32_t position = 0u; position < handleCount; ++position) {
        const uint32_t handle = handles[position];
        if (!ValidateLoopVoice(context, handle) ||
            v.loopEnabled[handle] == 0u ||
            v.stealFadeInFramesRemaining[handle] != 0u ||
            (v.envelopeStage[handle] != 1u &&
             v.envelopeStage[handle] != 2u) ||
            v.releaseSamplesRemaining[handle] != UINT32_MAX) {
            return false;
        }
    }
    for (uint32_t position = 0u; position < handleCount; ++position)
        RenderTransientLoopFramesAVX2(context, handles[position]);
    _mm256_zeroupper();
    return true;
}



// ════════════════════════════════════════════════════════════════════════
// Rotation-aware sustained-loop kernel (SVMSPhaseRotation.h).
//
// The regular AVX2 path vectorizes across TIME for one voice, but the
// per-voice phase-rotation filter is an IIR with a sequential time
// dependency — it cannot ride those time lanes.  This kernel instead
// vectorizes across VOICES: eight voices per batch, each lane carrying its
// own rotation state (gather/scatter on the 48-byte VoiceRotationState
// records; vgather's scale is capped at 8, so lane indices are pre-scaled
// by 12 floats and gathered with scale 4).
//
// Bookkeeping (phase advance, loop wrap, interpolation, gain) is the same
// arithmetic as the short-span batch above; the rotation filter is inserted
// between interpolation and accumulation.
// ════════════════════════════════════════════════════════════════════════
void RenderSustainedLoopRotationAVX2(const RenderSpanContext& c,
                                     const uint32_t* handles,
                                     uint32_t handleCount) {
    VoiceSoA& v = *c.voices;
    VoiceRotationState* const rot = v.rot;
    constexpr uint32_t kChunk = 32u;

    alignas(32) __m256 sumsL[kChunk];
    alignas(32) __m256 sumsR[kChunk];
    const __m256 zero = _mm256_setzero_ps();

    for (uint32_t chunkStart = 0u; chunkStart < c.frameCount;
         chunkStart += kChunk) {
        const uint32_t frames =
            (std::min)(kChunk, c.frameCount - chunkStart);
        for (uint32_t f = 0u; f < frames; ++f) {
            sumsL[f] = zero;
            sumsR[f] = zero;
        }
        float* const outL = c.outputLeft + c.frameStart + chunkStart;
        float* const outR = c.outputRight + c.frameStart + chunkStart;

        uint32_t position = 0u;
        for (; position + 8u <= handleCount; position += 8u) {
            const __m256i h = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(handles + position));
            alignas(32) uint32_t hs[8];
            alignas(32) float steps[8], lengths[8];
            _mm256_store_si256(reinterpret_cast<__m256i*>(hs), h);
            const __m256 step = _mm256_i32gather_ps(v.phaseIncs, h, 4);
            const __m256 loopStartF =
                _mm256_i32gather_ps(v.relLoopSF, h, 4);
            const __m256 loopEndF = _mm256_i32gather_ps(v.relLoopEF, h, 4);
            const __m256 loopLength = _mm256_sub_ps(loopEndF, loopStartF);
            _mm256_store_ps(steps, step);
            _mm256_store_ps(lengths, loopLength);
            bool valid = true;
            for (uint32_t lane = 0; lane < 8u; ++lane) {
                const uint32_t handle = hs[lane];
                valid = valid && v.relEnd[handle] >= 2u &&
                    v.relLoopS[handle] < v.relLoopE[handle] &&
                    v.sampleStart[handle] < c.sampleDataFrames &&
                    steps[lane] >= 0.0f && steps[lane] < lengths[lane];
            }
            if (!valid) {
                for (uint32_t lane = 0; lane < 8u; ++lane) {
                    ScalarRenderSustainedLoop(v, hs[lane], c.sampleData,
                        c.sampleDataFrames, c.outputLeft, c.outputRight,
                        c.frameStart + chunkStart, frames);
                }
                continue;
            }
            // Rotation form must be uniform across the batch (it always is —
            // the mode is engine-global — but mixed lanes fall back to the
            // scalar kernel rather than mixing filters).
            const __m256i h12 = _mm256_add_epi32(
                _mm256_slli_epi32(h, 3), _mm256_slli_epi32(h, 2));
            const int* const formBase = reinterpret_cast<const int*>(
                reinterpret_cast<const uint8_t*>(rot) +
                offsetof(VoiceRotationState, form));
            const __m256i formV = _mm256_i32gather_epi32(formBase, h12, 4);
            const __m256i formEq = _mm256_cmpeq_epi32(
                formV, _mm256_shuffle_epi32(formV, 0x00));
            if (_mm256_movemask_ps(_mm256_castsi256_ps(formEq)) != 0xFF) {
                for (uint32_t lane = 0; lane < 8u; ++lane) {
                    ScalarRenderSustainedLoop(v, hs[lane], c.sampleData,
                        c.sampleDataFrames, c.outputLeft, c.outputRight,
                        c.frameStart + chunkStart, frames);
                }
                continue;
            }
            const bool quad = _mm_cvtsi128_si32(
                _mm256_castsi256_si128(formV)) == 0;

            float* const stateBase = reinterpret_cast<float*>(rot);
#define SVMS_ROT_GATHER(field) _mm256_i32gather_ps(                        \
            stateBase + (offsetof(VoiceRotationState, field) /             \
                         sizeof(float)), h12, 4)
            const __m256 a0 = SVMS_ROT_GATHER(a0);
            const __m256 a1 = SVMS_ROT_GATHER(a1);
            const __m256 a2 = SVMS_ROT_GATHER(a2);
            const __m256 a3 = SVMS_ROT_GATHER(a3);
            const __m256 dc = SVMS_ROT_GATHER(dc);
            const __m256 ds = SVMS_ROT_GATHER(ds);
            __m256 cs = SVMS_ROT_GATHER(c);
            __m256 sn = SVMS_ROT_GATHER(s);
            __m256 z0 = SVMS_ROT_GATHER(z0);
            __m256 z1 = SVMS_ROT_GATHER(z1);
            __m256 z2 = SVMS_ROT_GATHER(z2);
            __m256 z3 = SVMS_ROT_GATHER(z3);
#undef SVMS_ROT_GATHER

            const __m256 gainL = _mm256_i32gather_ps(v.renderGainL, h, 4);
            const __m256 gainR = _mm256_i32gather_ps(v.renderGainR, h, 4);
            const __m256i sampleStart = _mm256_i32gather_epi32(
                reinterpret_cast<const int*>(v.sampleStart), h, 4);
            const __m256i loopStart = _mm256_i32gather_epi32(
                reinterpret_cast<const int*>(v.relLoopS), h, 4);
            const __m256i loopEnd = _mm256_i32gather_epi32(
                reinterpret_cast<const int*>(v.relLoopE), h, 4);
            __m256 phase = _mm256_max_ps(
                _mm256_i32gather_ps(v.phases, h, 4), zero);
            for (uint32_t f = 0u; f < frames; ++f) {
                const __m256 pastLoop =
                    _mm256_cmp_ps(phase, loopEndF, _CMP_GE_OQ);
                phase = _mm256_blendv_ps(phase,
                    _mm256_add_ps(loopStartF,
                        _mm256_sub_ps(phase, loopEndF)),
                    pastLoop);
                const __m256i base = _mm256_cvttps_epi32(phase);
                const __m256i nextRaw =
                    _mm256_add_epi32(base, _mm256_set1_epi32(1));
                const __m256i wrap = _mm256_cmpgt_epi32(nextRaw,
                    _mm256_sub_epi32(loopEnd, _mm256_set1_epi32(1)));
                const __m256i next =
                    _mm256_blendv_epi8(nextRaw, loopStart, wrap);
                const __m256i baseIndex = _mm256_add_epi32(sampleStart, base);
                const __m256i nextIndex = _mm256_add_epi32(sampleStart, next);
                const __m256 first = GatherSampleAVX2(c.sampleData, baseIndex);
                const __m256 second = GatherSampleAVX2(c.sampleData, nextIndex);
                const __m256 fraction =
                    _mm256_sub_ps(phase, _mm256_cvtepi32_ps(base));
                __m256 sample = _mm256_add_ps(
                    first, _mm256_mul_ps(_mm256_sub_ps(second, first),
                                         fraction));

                if (quad) {
                    // Quadrature rotation, lane-parallel: advance θ, run
                    // both allpass branches, then y = I·cosθ + Q·sinθ.
                    const __m256 nc = _mm256_sub_ps(
                        _mm256_mul_ps(cs, dc), _mm256_mul_ps(sn, ds));
                    const __m256 ns = _mm256_add_ps(
                        _mm256_mul_ps(sn, dc), _mm256_mul_ps(cs, ds));
                    cs = nc;
                    sn = ns;
                    const __m256 tA = _mm256_add_ps(
                        _mm256_mul_ps(a0, sample), z0);
                    z0 = _mm256_sub_ps(sample, _mm256_mul_ps(a0, tA));
                    const __m256 A = _mm256_add_ps(
                        _mm256_mul_ps(a1, tA), z1);
                    z1 = _mm256_sub_ps(tA, _mm256_mul_ps(a1, A));
                    const __m256 tB = _mm256_add_ps(
                        _mm256_mul_ps(a2, sample), z2);
                    z2 = _mm256_sub_ps(sample, _mm256_mul_ps(a2, tB));
                    const __m256 B = _mm256_add_ps(
                        _mm256_mul_ps(a3, tB), z3);
                    z3 = _mm256_sub_ps(tB, _mm256_mul_ps(a3, B));
                    const __m256 I = _mm256_mul_ps(
                        _mm256_add_ps(A, B), _mm256_set1_ps(0.5f));
                    const __m256 Q = _mm256_mul_ps(
                        _mm256_sub_ps(A, B), _mm256_set1_ps(0.5f));
                    sample = _mm256_add_ps(_mm256_mul_ps(I, cs),
                                           _mm256_mul_ps(Q, sn));
                } else {
                    // Cascade rotation: per-voice random unity-gain allpass.
                    const __m256 t = _mm256_add_ps(
                        _mm256_mul_ps(a0, sample), z0);
                    z0 = _mm256_sub_ps(sample, _mm256_mul_ps(a0, t));
                    __m256 y = _mm256_add_ps(_mm256_mul_ps(a1, t), z1);
                    z1 = _mm256_sub_ps(t, _mm256_mul_ps(a1, y));
                    const __m256 t2 = _mm256_add_ps(
                        _mm256_mul_ps(a2, y), z2);
                    z2 = _mm256_sub_ps(y, _mm256_mul_ps(a2, t2));
                    y = _mm256_add_ps(_mm256_mul_ps(a3, t2), z3);
                    z3 = _mm256_sub_ps(t2, _mm256_mul_ps(a3, y));
                    sample = y;
                }

                sumsL[f] = _mm256_add_ps(sumsL[f],
                    _mm256_mul_ps(sample, gainL));
                sumsR[f] = _mm256_add_ps(sumsR[f],
                    _mm256_mul_ps(sample, gainR));
                phase = _mm256_add_ps(phase, step);
            }

#define SVMS_ROT_SCATTER(field, value) _mm256_i32scatter_ps(               \
            stateBase + (offsetof(VoiceRotationState, field) /             \
                         sizeof(float)), h12, value, 4)
            SVMS_ROT_SCATTER(c, cs);
            SVMS_ROT_SCATTER(s, sn);
            SVMS_ROT_SCATTER(z0, z0);
            SVMS_ROT_SCATTER(z1, z1);
            SVMS_ROT_SCATTER(z2, z2);
            SVMS_ROT_SCATTER(z3, z3);
#undef SVMS_ROT_SCATTER
            alignas(32) float phases[8];
            _mm256_store_ps(phases, phase);
            for (uint32_t lane = 0; lane < 8u; ++lane)
                v.phases[hs[lane]] = phases[lane];
        }
        for (; position < handleCount; ++position) {
            ScalarRenderSustainedLoop(v, handles[position], c.sampleData,
                c.sampleDataFrames, c.outputLeft, c.outputRight,
                c.frameStart + chunkStart, frames);
        }
        for (uint32_t f = 0u; f < frames; ++f) {
            outL[f] += HorizontalSum(sumsL[f]);
            outR[f] += HorizontalSum(sumsR[f]);
        }
    }
    _mm256_zeroupper();
}

bool RenderSustainedLoopAVX2(const RenderSpanContext& context,
                             const uint32_t* handles,
                             uint32_t handleCount) {
    VoiceSoA& v = *context.voices;
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    // Per-voice phase rotation: the IIR filter is sequential across time, so
    // route through the voice-parallel kernel that applies it across lanes.
    if (v.rot != nullptr) {
        RenderSustainedLoopRotationAVX2(context, handles, handleCount);
        _mm256_zeroupper();
        return true;
    }
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
            const __m256 first = GatherSampleAVX2(context.sampleData, baseIndex);
            const __m256 second = GatherSampleAVX2(context.sampleData, nextIndex);
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
            RenderTransientLoopAVX2;
        result.backend = RenderBackend::AVX2;
        result.name = "avx2";
        return result;
    }();
    return set;
}

} // namespace svms
