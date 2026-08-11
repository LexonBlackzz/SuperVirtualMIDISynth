#include "SVMSRenderKernels.h"

#include <algorithm>
#include <cmath>
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

void RenderSustainedLoopAVX2(const RenderSpanContext& context,
                             const uint32_t* handles,
                             uint32_t handleCount) {
    VoiceSoA& v = *context.voices;
    if (context.frameCount == 0u || context.sampleData == nullptr) return;
    if (context.frameCount > 4u) {
        for (uint32_t i = 0; i < handleCount; ++i) {
            ScalarRenderSustainedLoop(v, handles[i], context.sampleData,
                context.sampleDataFrames, context.outputLeft,
                context.outputRight, context.frameStart, context.frameCount);
        }
        _mm256_zeroupper();
        return;
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
}

} // namespace

const RenderKernelSet& GetAVX2RenderKernelSet() {
    static const RenderKernelSet set = [] {
        RenderKernelSet result{};
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::SustainedLoop)] =
            RenderSustainedLoopAVX2;
        result.backend = RenderBackend::AVX2;
        result.name = "avx2";
        return result;
    }();
    return set;
}

} // namespace svms
