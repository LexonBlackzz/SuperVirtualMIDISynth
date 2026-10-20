#include "render/SVMSRenderKernels.h"
#include "render/SVMSEnvelope.h"
#include "render/SVMSVoiceFilter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <immintrin.h>

#if defined(_MSC_VER)
#define SVMS_K_FORCEINLINE __forceinline
#define SVMS_K_NOINLINE __declspec(noinline)
#else
#define SVMS_K_FORCEINLINE inline __attribute__((always_inline))
#define SVMS_K_NOINLINE __attribute__((noinline))
#endif

namespace svms {
namespace {
// Opt-in (quality.fast_chunk_math): sustained, transient and release-loop
// 8-frame chunks take their gain ladder and phase advance in closed form
// instead of 8 dependent multiplies and adds.  Per-sample results move by a few float ulps; sample reads and
// interpolation are untouched.  Default off keeps the exact sequential math.
bool g_fastChunkMath = false;


// GCC-target-compatible replacement for _mm256_i32scatter_ps (which is an
// AVX512VL intrinsic). Performs the same lane-wise store using only AVX2:
// dst[lane] = value[lane] where dst = base + index[lane] (unit stride, float).
inline void SVMS_ScatterPs(float* base, __m256i index, __m256 value) {
    alignas(32) float val[8];
    alignas(32) int32_t idx[8];
    _mm256_store_ps(val, value);
    _mm256_store_si256(reinterpret_cast<__m256i*>(idx), index);
    for (uint32_t lane = 0; lane < 8u; ++lane)
        base[idx[lane]] = val[lane];
}

float HorizontalSum(__m256 value) {
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, value);
    return ((lanes[0] + lanes[1]) + (lanes[2] + lanes[3])) +
           ((lanes[4] + lanes[5]) + (lanes[6] + lanes[7]));
}

// Eight consecutive samples of one filtered voice. Interpolation stays
// vectorized; recursive filter state is consumed in scalar-equivalent order.
SVMS_K_NOINLINE __m256 FilterTime8EnabledAVX2(VoiceSoA& v, uint32_t handle,
                                              __m256 input) {
    // Recursive IIR state must preserve scalar ordering exactly.
    // Keep interpolation vectorized, then consume these 8 samples in-order.
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, input);
    for (uint32_t lane = 0u; lane < 8u; ++lane)
        lanes[lane] = ProcessVoiceFilterSample(v, handle, lanes[lane]);
    return _mm256_load_ps(lanes);
}

// The unfiltered test stays inline: out of line, every 8-frame chunk of an
// unfiltered voice paid a vzeroupper + call for nothing (profile, 2026-10-16).
SVMS_K_FORCEINLINE __m256 FilterTime8AVX2(VoiceSoA& v, uint32_t handle,
                                          __m256 input) {
    if (v.filterEnabled[handle] == 0u) return input;
    return FilterTime8EnabledAVX2(v, handle, input);
}

// Eight independent voices for the short-span / event-fragmented kernels.
inline __m256 FilterVoices8AVX2(VoiceSoA& v, __m256i handles,
                                __m256 input) {
    const __m256i h = handles;
    const __m256i enabled = _mm256_i32gather_epi32(
        reinterpret_cast<const int*>(v.filterEnabled), h, 4);
    const __m256 mask = _mm256_castsi256_ps(_mm256_cmpgt_epi32(
        enabled, _mm256_setzero_si256()));
    if (_mm256_movemask_ps(mask) == 0) return input;

    const __m256 a0 = _mm256_i32gather_ps(v.filterA0, h, 4);
    const __m256 b1 = _mm256_i32gather_ps(v.filterB1, h, 4);
    const __m256 b2 = _mm256_i32gather_ps(v.filterB2, h, 4);
    const __m256 oldZ1 = _mm256_i32gather_ps(v.filterZ1, h, 4);
    const __m256 oldZ2 = _mm256_i32gather_ps(v.filterZ2, h, 4);
    const __m256 output = _mm256_add_ps(_mm256_mul_ps(input, a0), oldZ1);
    const __m256 newZ1 = _mm256_sub_ps(
        _mm256_add_ps(_mm256_mul_ps(input, _mm256_add_ps(a0, a0)), oldZ2),
        _mm256_mul_ps(b1, output));
    const __m256 newZ2 = _mm256_sub_ps(
        _mm256_mul_ps(input, a0), _mm256_mul_ps(b2, output));
    SVMS_ScatterPs(v.filterZ1, h, _mm256_blendv_ps(oldZ1, newZ1, mask));
    SVMS_ScatterPs(v.filterZ2, h, _mm256_blendv_ps(oldZ2, newZ2, mask));
    return _mm256_blendv_ps(input, output, mask);
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
    // One word gather suffices: element e always lives in word e >> 1
    // (low half when e is even, high half when e is odd).  The pair helper
    // below needs the neighbouring word for its e+1 lane; this one does not.
    const __m256i w0 = _mm256_i32gather_epi32(
        reinterpret_cast<const int*>(data), word, 4);
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

// Contiguous variant of the pair gather for the time-lane kernels.  When the
// eight lane positions span under 16 samples (pitch ratio below ~2) the same
// elements are loaded as three contiguous vectors and picked per lane with
// permutes: no gather (slow, latency-bound, microcoded on E-cores), identical
// values and scale, so results are bit-exact vs GatherSamplePairAVX2.
// `readable` = int16 elements readable from `data`; anything else falls back.
inline void GatherSamplePairSeqAVX2(const int16_t* data, uint32_t readable,
                                    __m256i bases, __m256& first,
                                    __m256& second) {
    const uint32_t lo = static_cast<uint32_t>(_mm256_cvtsi256_si32(bases));
    const uint32_t hi = static_cast<uint32_t>(_mm256_extract_epi32(bases, 7));
    if (hi - lo > 15u || lo + 24u > readable) {
        GatherSamplePairAVX2(data, bases, first, second);
        return;
    }
    const int16_t* p = data + lo;
    const __m256i a = _mm256_cvtepi16_epi32(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
    const __m256i b = _mm256_cvtepi16_epi32(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 8)));
    const __m256i c = _mm256_cvtepi16_epi32(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16)));
    const __m256i rel = _mm256_sub_epi32(bases, _mm256_set1_epi32(
        static_cast<int>(lo)));
    const __m256i rel1 = _mm256_add_epi32(rel, _mm256_set1_epi32(1));
    const __m256i seven = _mm256_set1_epi32(7);
    const __m256i fifteen = _mm256_set1_epi32(15);
    const __m256i f = _mm256_blendv_epi8(
        _mm256_permutevar8x32_epi32(a, rel),
        _mm256_permutevar8x32_epi32(b, rel),
        _mm256_cmpgt_epi32(rel, seven));
    const __m256i sec = _mm256_blendv_epi8(
        _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(a, rel1),
                           _mm256_permutevar8x32_epi32(b, rel1),
                           _mm256_cmpgt_epi32(rel1, seven)),
        _mm256_permutevar8x32_epi32(c, rel1),
        _mm256_cmpgt_epi32(rel1, fifteen));
    const __m256 int16Scale = _mm256_set1_ps(1.0f / 32768.0f);
    first = _mm256_mul_ps(_mm256_cvtepi32_ps(f), int16Scale);
    second = _mm256_mul_ps(_mm256_cvtepi32_ps(sec), int16Scale);
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
        const float fraction = phase - static_cast<float>(base);
        const float first = static_cast<float>(c.sampleData[firstIndex]) *
                            (1.0f / 32768.0f);
        float sample =
            first + (static_cast<float>(c.sampleData[nextIndex]) *
                     (1.0f / 32768.0f) - first) * fraction;
        if (v.rot)
            sample = RotateVoiceSample(v.stealTailRot[h], sample,
                                       c.hilbertData, firstIndex, nextIndex,
                                       fraction);
        sample = ProcessStealTailFilterSample(v, h, sample);
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

    // Phase-rotated tails own recursive per-tail state. Keep them on the
    // exact per-tail Hilbert path instead of the non-rotation vector kernel.
    if (c.voices->rot != nullptr) {
        for (uint32_t i = 0u; i < handleCount; ++i) {
            const uint32_t h = handles[i];
            const RenderSpanContext vc = SelectVoiceDestination(
                c, c.voices->stealTailChannel[h]);
            RenderTailScalar(vc, h, frameCounts[h]);
        }
        _mm256_zeroupper();
        return;
    }

    // Channel buses: the voice-batched path below mixes eight lanes into
    // one frame position and cannot scatter into per-channel planes; bus
    // batches therefore take the per-voice scalar tail with per-voice
    // plane selection.
    if (c.channelBusLeft != nullptr) {
        for (uint32_t i = 0; i < handleCount; ++i) {
            const RenderSpanContext vc = SelectVoiceDestination(
                c, c.voices->channel[handles[i]]);
            RenderTailScalar(vc, handles[i], frameCounts[handles[i]]);
        }
        _mm256_zeroupper();
        return;
    }
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
                v.stealTailFilterEnabled[x] == 0u &&
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
        // Ghost rows are disposable render snapshots (no retirement
        // buffer). The authoritative lifecycle was already committed by the
        // whole-voice pre-pass.
        c.voices->state[handle] = static_cast<uint8_t>(VoiceState::Free);
    }
}

// Hilbert-pair (form 2) rotation for the time-chunked kernels. Init binds a
// voice; Chunk rotates eight consecutive frames, Scalar rotates one (the
// scalar tails), Store writes the angle back. Static angles (dc=1, ds=0) are
// exact; sweeping angles use a closed-form (dc + i*ds)^k ladder per chunk
// instead of eight serially dependent steps (tolerance-equal to the scalar
// recurrence). Form 2 without a companion store is a pass-through that does
// not advance the angle, exactly like RotateVoiceSample.
struct ChunkRotation {
    VoiceRotationState* rs = nullptr;
    const int16_t* hregion = nullptr;
    bool sweeping = false;
    float curC = 1.0f, curS = 0.0f, chunkDc = 1.0f, chunkDs = 0.0f;
    alignas(32) float ladderC[8];
    alignas(32) float ladderS[8];

    // False for the recursive diffuse allpass (form 1): the caller must use
    // the scalar span for that voice.
    bool Init(VoiceSoA& v, const int16_t* hilbertData, uint32_t handle) {
        if (v.rot == nullptr) return true;
        VoiceRotationState& st = v.rot[handle];
        if (st.form == 1u) return false;
        if (st.form != 2u || hilbertData == nullptr) return true;
        rs = &st;
        hregion = hilbertData + v.sampleStart[handle];
        curC = st.c;
        curS = st.s;
        sweeping = st.dc != 1.0f || st.ds != 0.0f;
        if (sweeping) {
            ladderC[0] = 1.0f;
            ladderS[0] = 0.0f;
            for (uint32_t k = 1u; k < 8u; ++k) {
                ladderC[k] = ladderC[k - 1u] * st.dc -
                             ladderS[k - 1u] * st.ds;
                ladderS[k] = ladderS[k - 1u] * st.dc +
                             ladderC[k - 1u] * st.ds;
            }
            chunkDc = ladderC[7] * st.dc - ladderS[7] * st.ds;
            chunkDs = ladderS[7] * st.dc + ladderC[7] * st.ds;
        }
        return true;
    }
    explicit operator bool() const { return rs != nullptr; }

    __m256 Chunk(__m256 sample, __m256i bases, __m256 fraction) {
        __m256 hFirst, hSecond;
        GatherSamplePairAVX2(hregion, bases, hFirst, hSecond);
        const __m256 xhat = _mm256_add_ps(hFirst,
            _mm256_mul_ps(_mm256_sub_ps(hSecond, hFirst), fraction));
        __m256 rotC, rotS;
        if (sweeping) {
            const __m256 c0 = _mm256_set1_ps(curC);
            const __m256 s0 = _mm256_set1_ps(curS);
            const __m256 lc = _mm256_load_ps(ladderC);
            const __m256 ls = _mm256_load_ps(ladderS);
            rotC = _mm256_sub_ps(_mm256_mul_ps(c0, lc),
                                 _mm256_mul_ps(s0, ls));
            rotS = _mm256_add_ps(_mm256_mul_ps(s0, lc),
                                 _mm256_mul_ps(c0, ls));
            const float nc = curC * chunkDc - curS * chunkDs;
            const float ns = curS * chunkDc + curC * chunkDs;
            curC = nc;
            curS = ns;
        } else {
            rotC = _mm256_set1_ps(curC);
            rotS = _mm256_set1_ps(curS);
        }
        return _mm256_sub_ps(_mm256_mul_ps(sample, rotC),
                             _mm256_mul_ps(xhat, rotS));
    }
    float Scalar(float sample, uint32_t base, uint32_t next, float fraction) {
        rs->c = curC;
        rs->s = curS;
        sample = RotateVoiceSample(*rs, sample, hregion, base, next, fraction);
        curC = rs->c;
        curS = rs->s;
        return sample;
    }
    void Store() {
        if (rs == nullptr) return;
        rs->c = curC;
        rs->s = curS;
    }
};

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
            v, handle, c.sampleData, c.hilbertData, c.sampleDataFrames,
            c.outputLeft,
            c.outputRight, c.frameStart, c.frameCount);
    }

    ChunkRotation rot;
    if (!rot.Init(v, c.hilbertData, handle)) {
        return ScalarRenderSustainedLoop(
            v, handle, c.sampleData, c.hilbertData, c.sampleDataFrames,
            c.outputLeft, c.outputRight, c.frameStart, c.frameCount);
    }

    float phase = (std::max)(0.0f, v.phases[handle]);
    float step = v.phaseIncs[handle];
    const float loopStart = v.relLoopSF[handle];
    const float loopEnd = v.relLoopEF[handle];
    const float loopLength = loopEnd - loopStart;
    const float gainL = v.renderGainL[handle];
    const float gainR = v.renderGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    const uint32_t readable = c.sampleDataFrames - v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    __m256 stepVector = _mm256_set1_ps(step);
    const __m256 gainLVector = _mm256_set1_ps(gainL);
    const __m256 gainRVector = _mm256_set1_ps(gainR);
    const __m256i one = _mm256_set1_epi32(1);
    // Whole-voice vibrato: the 64-frame LFO control window is rebuilt at
    // the loop top; the gate keeps fast chunks from straddling a window
    // boundary.  UINT32_MAX = LFO off (scalar/SSE2 windows are handled by
    // the renderer, not the kernel).
    const bool lfoOn = c.lfoActive != 0u &&
        v.vibLfoSteps[handle] != 0.0f &&
        v.vibLfoToPitchCents[handle] > 0.0f && c.lfoDepth > 0.0f;
    uint32_t windowRemaining = UINT32_MAX;
    if (lfoOn) {
        step = AdvanceVibratoLfoWindow(v, handle,
            (std::min)(64u, c.frameCount), c.lfoDepth, c.lfoBendRatio);
        stepVector = _mm256_set1_ps(step);
        windowRemaining = (std::min)(64u, c.frameCount);
    }
    // quality.fast_chunk_math: phase advances 8 frames in one multiply.
    const bool fastMath = g_fastChunkMath && !lfoOn;
    const float step8 = step * 8.0f;
    uint32_t frame = 0u;

    while (frame < c.frameCount) {
        if (windowRemaining == 0u) {
            const uint32_t win = (std::min)(64u, c.frameCount - frame);
            step = AdvanceVibratoLfoWindow(v, handle, win, c.lfoDepth,
                                           c.lfoBendRatio);
            stepVector = _mm256_set1_ps(step);
            windowRemaining = win;
        }
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (overflow >= loopLength)
                overflow -= std::floor(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        const float lastPhase = phase + step * 7.0f;
        if (frame + 8u <= c.frameCount && windowRemaining >= 8u &&
            lastPhase < loopEnd - 1.0f) {
                        // Warm the next chunk's sample lines: reads are sequential per
            // voice, so one prefetch per chunk hides most of the gather's
            // memory latency at ~0.1 cycles per frame.
            _mm_prefetch(reinterpret_cast<const char*>(
                             region +
                             static_cast<uint32_t>(lastPhase + step)),
                         _MM_HINT_T0);
const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairSeqAVX2(region, readable, bases, first, second);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            if (rot) sample = rot.Chunk(sample, bases, fraction);
            sample = FilterTime8AVX2(v, handle, sample);
            _mm256_storeu_ps(outL + frame, _mm256_add_ps(
                _mm256_loadu_ps(outL + frame),
                _mm256_mul_ps(sample, gainLVector)));
            _mm256_storeu_ps(outR + frame, _mm256_add_ps(
                _mm256_loadu_ps(outR + frame),
                _mm256_mul_ps(sample, gainRVector)));
            // Preserve the scalar cursor's repeated-add rounding.
            if (fastMath) {
                phase += step8;
            } else {
                for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex)
                    phase += step;
            }
            frame += 8u;
            windowRemaining -= 8u;
            continue;
        }

        // Masked tail chunk (see RenderReleaseLoopFramesAVX2): plain voices
        // only, since filter and rotation state advance per lane.
        if (fastMath && !rot && v.filterEnabled[handle] == 0u &&
            c.frameCount - frame < 8u) {
            const uint32_t rem = c.frameCount - frame;
            const float lastLive = phase + step * static_cast<float>(rem - 1u);
            if (lastLive < loopEnd - 1.0f) {
                const __m256i liveMask = _mm256_cmpgt_epi32(
                    _mm256_set1_epi32(static_cast<int>(rem)),
                    _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
                const __m256 phases = _mm256_min_ps(
                    _mm256_add_ps(_mm256_set1_ps(phase),
                                  _mm256_mul_ps(stepVector, lane)),
                    _mm256_set1_ps(lastLive));
                const __m256i bases = _mm256_cvttps_epi32(phases);
                __m256 first, second;
                GatherSamplePairSeqAVX2(region, readable, bases, first,
                                        second);
                const __m256 fraction = _mm256_sub_ps(
                    phases, _mm256_cvtepi32_ps(bases));
                const __m256 sample = _mm256_add_ps(first,
                    _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
                _mm256_maskstore_ps(outL + frame, liveMask, _mm256_add_ps(
                    _mm256_maskload_ps(outL + frame, liveMask),
                    _mm256_mul_ps(sample, gainLVector)));
                _mm256_maskstore_ps(outR + frame, liveMask, _mm256_add_ps(
                    _mm256_maskload_ps(outR + frame, liveMask),
                    _mm256_mul_ps(sample, gainRVector)));
                phase += step * static_cast<float>(rem);
                frame += rem;
                continue;
            }
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
        float sample = first + (static_cast<float>(region[next]) * (1.0f / 32768.0f) - first) * fraction;
        if (rot) sample = rot.Scalar(sample, base, next, fraction);
        sample = ProcessVoiceFilterSample(v, handle, sample);
        outL[frame] += sample * gainL;
        outR[frame] += sample * gainR;
        phase += step;
        --windowRemaining;
        ++frame;
    }
    // Match the existing AVX2 short-span cursor contract: wrapping happens
    // before the next rendered sample, so the final increment may remain
    // just past loopEnd in the stored snapshot.
    v.phases[handle] = phase;
    rot.Store();
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
            (remaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
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
    // Form-1 (diffuse) rotation never reaches here: the class kernel refuses
    // before any state is touched.
    ChunkRotation rot;
    rot.Init(v, c.hilbertData, handle);
    float phase = (std::max)(0.0f, v.phases[handle]);
    float gain = v.currentGain[handle];
    uint32_t remaining = v.releaseSamplesRemaining[handle];
    const float decay = v.releaseDecay[handle];
    float step = v.phaseIncs[handle];
    const float loopStart = v.relLoopSF[handle];
    const float loopEnd = v.relLoopEF[handle];
    const float loopLength = loopEnd - loopStart;
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    const uint32_t readable = c.sampleDataFrames - v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    __m256 stepVector = _mm256_set1_ps(step);
    const __m256i one = _mm256_set1_epi32(1);
    // Whole-voice vibrato windows (see RenderSustainedLoopFramesAVX2).
    const bool lfoOn = c.lfoActive != 0u &&
        v.vibLfoSteps[handle] != 0.0f &&
        v.vibLfoToPitchCents[handle] > 0.0f && c.lfoDepth > 0.0f;
    uint32_t windowRemaining = UINT32_MAX;
    if (lfoOn) {
        step = AdvanceVibratoLfoWindow(v, handle,
            (std::min)(64u, c.frameCount), c.lfoDepth, c.lfoBendRatio);
        stepVector = _mm256_set1_ps(step);
        windowRemaining = (std::min)(64u, c.frameCount);
    }
    // quality.fast_chunk_math: gain ladder and phase advance in closed form.
    const bool fastMath = g_fastChunkMath && !lfoOn;
    __m256 decayLadder = _mm256_setzero_ps();
    float decay8 = 1.0f, step8 = 0.0f;
    alignas(32) float ladder[8] = {};
    if (fastMath) {
        // Powers by squaring in double: three dependent steps, and each float
        // entry is correctly rounded (a float decay^8 carried a systematic
        // relative bias that compounds once per chunk).
        const double dd = decay, d2 = dd * dd, d4 = d2 * d2;
        ladder[0] = decay; ladder[1] = static_cast<float>(d2);
        ladder[2] = static_cast<float>(d2 * dd);
        ladder[3] = static_cast<float>(d4);
        ladder[4] = static_cast<float>(d4 * dd);
        ladder[5] = static_cast<float>(d4 * d2);
        ladder[6] = static_cast<float>(d4 * d2 * dd);
        ladder[7] = static_cast<float>(d4 * d4);
        decayLadder = _mm256_load_ps(ladder);
        decay8 = ladder[7];
        step8 = step * 8.0f;
    }
    uint32_t frame = 0u;
    uint32_t retiredAt = UINT32_MAX;
    while (frame < c.frameCount) {
        if (windowRemaining == 0u) {
            const uint32_t win = (std::min)(64u, c.frameCount - frame);
            step = AdvanceVibratoLfoWindow(v, handle, win, c.lfoDepth,
                                           c.lfoBendRatio);
            stepVector = _mm256_set1_ps(step);
            windowRemaining = win;
        }
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (overflow >= loopLength)
                overflow -= std::floor(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        float futureGain = gain;
        alignas(32) float gains[8];
        if (fastMath) {
            futureGain = gain * decay8;
        } else {
            for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex) {
                futureGain *= decay;
                gains[laneIndex] = futureGain;
            }
        }
        const bool countdownSafe = remaining == UINT32_MAX || remaining > 8u;
        const bool thresholdSafe = remaining != UINT32_MAX ||
            (fastMath ? futureGain : gains[7]) >= VoiceRetireThreshold();
        const float lastPhase = phase + step * 7.0f;
        if (frame + 8u <= c.frameCount && windowRemaining >= 8u &&
            countdownSafe && thresholdSafe &&
            lastPhase < loopEnd - 1.0f) {
                        // Warm the next chunk's sample lines: reads are sequential per
            // voice, so one prefetch per chunk hides most of the gather's
            // memory latency at ~0.1 cycles per frame.
            _mm_prefetch(reinterpret_cast<const char*>(
                             region +
                             static_cast<uint32_t>(lastPhase + step)),
                         _MM_HINT_T0);
const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairSeqAVX2(region, readable, bases, first, second);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            sample = FilterTime8AVX2(v, handle, sample);
            if (rot) sample = rot.Chunk(sample, bases, fraction);
            const __m256 gainVector = fastMath
                ? _mm256_mul_ps(_mm256_set1_ps(gain), decayLadder)
                : _mm256_load_ps(gains);
            const __m256 scaled = _mm256_mul_ps(sample, gainVector);
            _mm256_storeu_ps(outL + frame, _mm256_add_ps(
                _mm256_loadu_ps(outL + frame),
                _mm256_mul_ps(scaled, _mm256_set1_ps(mixL))));
            _mm256_storeu_ps(outR + frame, _mm256_add_ps(
                _mm256_loadu_ps(outR + frame),
                _mm256_mul_ps(scaled, _mm256_set1_ps(mixR))));
            gain = futureGain;
            if (remaining != UINT32_MAX) remaining -= 8u;
            if (fastMath) {
                phase += step8;
            } else {
                for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex)
                    phase += step;
            }
            frame += 8u;
            windowRemaining -= 8u;
            continue;
        }

        // Masked tail chunk: the last 1..7 frames of a span ride the same
        // vector code (only the live lanes are committed) instead of the slow
        // per-frame path.  Spans are short when events split them, so most
        // chunk-path misses were exactly this remainder.  Same guards as the
        // full chunk, evaluated for the live lanes only.
        // Rotation and filter state advance per lane in order, so voices that
        // carry either keep the per-frame remainder.
        if (!lfoOn && !rot && v.filterEnabled[handle] == 0u &&
            c.frameCount - frame < 8u) {
            const uint32_t rem = c.frameCount - frame;
            const float lastLive = phase + step * static_cast<float>(rem - 1u);
            const float tailGain = fastMath ? gain * ladder[rem - 1u]
                                            : gains[rem - 1u];
            if ((remaining == UINT32_MAX || remaining > rem) &&
                (remaining != UINT32_MAX ||
                 tailGain >= VoiceRetireThreshold()) &&
                lastLive < loopEnd - 1.0f) {
                const __m256i liveMask = _mm256_cmpgt_epi32(
                    _mm256_set1_epi32(static_cast<int>(rem)),
                    _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
                const __m256 phases = _mm256_min_ps(
                    _mm256_add_ps(_mm256_set1_ps(phase),
                                  _mm256_mul_ps(stepVector, lane)),
                    _mm256_set1_ps(lastLive));
                const __m256i bases = _mm256_cvttps_epi32(phases);
                __m256 first, second;
                GatherSamplePairSeqAVX2(region, readable, bases, first,
                                        second);
                const __m256 fraction = _mm256_sub_ps(
                    phases, _mm256_cvtepi32_ps(bases));
                __m256 sample = _mm256_add_ps(first,
                    _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
                sample = FilterTime8AVX2(v, handle, sample);
                if (rot) sample = rot.Chunk(sample, bases, fraction);
                const __m256 gainVector = fastMath
                    ? _mm256_mul_ps(_mm256_set1_ps(gain), decayLadder)
                    : _mm256_load_ps(gains);
                const __m256 scaled = _mm256_mul_ps(sample, gainVector);
                const __m256 maskPs = _mm256_castsi256_ps(liveMask);
                _mm256_maskstore_ps(outL + frame, liveMask, _mm256_add_ps(
                    _mm256_maskload_ps(outL + frame, liveMask),
                    _mm256_mul_ps(scaled, _mm256_set1_ps(mixL))));
                _mm256_maskstore_ps(outR + frame, liveMask, _mm256_add_ps(
                    _mm256_maskload_ps(outR + frame, liveMask),
                    _mm256_mul_ps(scaled, _mm256_set1_ps(mixR))));
                (void)maskPs;
                gain = tailGain;
                if (remaining != UINT32_MAX) remaining -= rem;
                for (uint32_t k = 0u; k < rem; ++k) phase += step;
                frame += rem;
                continue;
            }
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
        float sample = first + (static_cast<float>(region[next]) * (1.0f / 32768.0f) - first) * fraction;
        bool finished = remaining == 0u;
        if (!finished) {
            gain *= decay;
            if (remaining != UINT32_MAX) {
                --remaining;
                finished = remaining == 0u;
            }
        }
        sample = ProcessVoiceFilterSample(v, handle, sample);
        if (rot) sample = rot.Scalar(sample, base, next, fraction);
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        phase += step;
        --windowRemaining;
        if (phase >= loopEnd) {
            float overflow = phase - loopEnd;
            if (overflow >= loopLength)
                overflow -= std::floor(overflow / loopLength) * loopLength;
            phase = loopStart + overflow;
        }
        if (finished ||
            (remaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
            retiredAt = frame;
            break;
        }
        ++frame;
    }
    v.phases[handle] = phase;
    v.currentGain[handle] = gain;
    v.releaseSamplesRemaining[handle] = remaining;
    rot.Store();
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
                 finalGain >= VoiceRetireThreshold());
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

void RenderReleaseFramesGrouped(const RenderSpanContext& c,
                                const uint32_t* handles, uint32_t count);

bool RenderReleaseLoopAVX2(const RenderSpanContext& context,
                           const uint32_t* handles,
                           uint32_t handleCount) {
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    // Per-voice phase rotation: Hilbert-pair (form 2) voices ride the
    // time-chunked kernel below. Refuse (nothing mutated yet) for the
    // voice-batched short kernel and for any diffuse (form 1) voice, and
    // let the scalar span kernels take the whole batch.
    if (context.voices->rot != nullptr) {
        if (context.frameCount <= 4u) return false;
        for (uint32_t position = 0u; position < handleCount; ++position)
            if (context.voices->rot[handles[position]].form == 1u)
                return false;
    }
    // Channel buses: the voice-batched short kernel mixes eight voices (up
    // to eight different MIDI channels) into the same frame position, which
    // cannot scatter into per-channel planes. Refuse (no state mutated —
    // validation has not run) and let the caller's per-voice scalar fallback
    // select planes.
    if (context.channelBusLeft != nullptr && context.frameCount <= 7u)
        return false;
    if (context.frameCount <= 4u) {
        RenderReleaseLoopShortAVX2(context, handles, handleCount);
        _mm256_zeroupper();
        return true;
    }
    if (context.frameCount >= 8u && context.voices->rot == nullptr) {
        RenderReleaseFramesGrouped(context, handles, handleCount);
        _mm256_zeroupper();
        return true;
    }
    for (uint32_t position = 0u; position < handleCount; ++position) {
        const uint32_t handle = handles[position];
        const RenderSpanContext vc = SelectVoiceDestination(
            context, context.voices->channel[handle]);
        const uint32_t retiredAt = RenderReleaseLoopFramesAVX2(
            vc, handle);
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
    ChunkRotation rot;
    rot.Init(v, c.hilbertData, handle);
    float phase = (std::max)(0.0f, v.phases[handle]);
    float gain = v.currentGain[handle];
    uint8_t stage = v.envelopeStage[handle];
    const uint8_t initialStage = stage;
    uint32_t attackRemaining = v.attackSamplesRemaining[handle];
    uint32_t decayRemaining = v.decaySamplesRemaining[handle];
    float step = v.phaseIncs[handle];
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
    const uint32_t readable = c.sampleDataFrames - v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    __m256 stepVector = _mm256_set1_ps(step);
    const __m256i one = _mm256_set1_epi32(1);
    // Whole-voice vibrato windows (see RenderSustainedLoopFramesAVX2).
    const bool lfoOn = c.lfoActive != 0u &&
        v.vibLfoSteps[handle] != 0.0f &&
        v.vibLfoToPitchCents[handle] > 0.0f && c.lfoDepth > 0.0f;
    uint32_t windowRemaining = UINT32_MAX;
    if (lfoOn) {
        step = AdvanceVibratoLfoWindow(v, handle,
            (std::min)(64u, c.frameCount), c.lfoDepth, c.lfoBendRatio);
        stepVector = _mm256_set1_ps(step);
        windowRemaining = (std::min)(64u, c.frameCount);
    }
    const bool fastMath = g_fastChunkMath && !lfoOn;
    uint32_t frame = 0u;
    while (frame < c.frameCount) {
        if (windowRemaining == 0u) {
            const uint32_t win = (std::min)(64u, c.frameCount - frame);
            step = AdvanceVibratoLfoWindow(v, handle, win, c.lfoDepth,
                                           c.lfoBendRatio);
            stepVector = _mm256_set1_ps(step);
            windowRemaining = win;
        }
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
        // quality.fast_chunk_math: a chunk that stays in one stage and hits
        // no clamp has a closed-form gain ladder (linear ramp in attack,
        // geometric in decay); anything else runs the exact simulation.
        bool closedForm = false;
        if (fastMath) {
            if (stage == 1u && attackRemaining >= 9u && attackStep >= 0.0f) {
                for (uint32_t k = 0u; k < 8u; ++k)
                    gains[k] = gain + attackStep * static_cast<float>(k + 1u);
                if (gains[7] <= targetGain) {
                    closedForm = true;
                    futureGain = gains[7];
                    futureAttack -= 8u;
                }
            } else if (stage == 2u && decayRemaining >= 9u && decaySlope > 0.0f) {
                const double s1 = decaySlope, s2 = s1 * s1, s4 = s2 * s2;
                const double g = gain;
                gains[0] = static_cast<float>(g * s1);
                gains[1] = static_cast<float>(g * s2);
                gains[2] = static_cast<float>(g * s2 * s1);
                gains[3] = static_cast<float>(g * s4);
                gains[4] = static_cast<float>(g * s4 * s1);
                gains[5] = static_cast<float>(g * s4 * s2);
                gains[6] = static_cast<float>(g * s4 * s2 * s1);
                gains[7] = static_cast<float>(g * s4 * s4);
                if (gains[0] >= sustainLevel && gains[7] >= sustainLevel) {
                    closedForm = true;
                    futureGain = gains[7];
                    futureDecay -= 8u;
                }
            }
        }
        for (uint32_t laneIndex = 0u; !closedForm && laneIndex < 8u;
             ++laneIndex) {
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
        if (frame + 8u <= c.frameCount && windowRemaining >= 8u &&
            futureStage == stage &&
            lastPhase < loopEnd - 1.0f) {
                        // Warm the next chunk's sample lines: reads are sequential per
            // voice, so one prefetch per chunk hides most of the gather's
            // memory latency at ~0.1 cycles per frame.
            _mm_prefetch(reinterpret_cast<const char*>(
                             region +
                             static_cast<uint32_t>(lastPhase + step)),
                         _MM_HINT_T0);
const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairSeqAVX2(region, readable, bases, first, second);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            sample = FilterTime8AVX2(v, handle, sample);
            if (rot) sample = rot.Chunk(sample, bases, fraction);
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
            if (fastMath) {
                phase += step * 8.0f;
            } else {
                for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex)
                    phase += step;
            }
            frame += 8u;
            windowRemaining -= 8u;
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
        float sample = first + (static_cast<float>(region[next]) * (1.0f / 32768.0f) - first) * fraction;
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
        sample = ProcessVoiceFilterSample(v, handle, sample);
        if (rot) sample = rot.Scalar(sample, base, next, fraction);
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        phase += step;
        --windowRemaining;
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
    rot.Store();
    if (stage != initialStage && c.classChangeHandles != nullptr &&
        c.classChangeCount != nullptr) {
        c.classChangeHandles[(*c.classChangeCount)++] = handle;
    }
}

// Voice-batched short-span transient kernel.  Spans of 1-7 frames dominate
// event-fragmented (buzz) workloads, where per-voice time-vectorization
// cannot engage (the 8-frame chunk guard never fires).  Lanes carry eight
// voices; the per-frame envelope recurrence runs scalarly per lane
// (bit-exact by construction) and only the sample fetch / lerp / mix is
// vectorized.  Callers must have validated every handle with
// ValidateLoopVoice and the transient preconditions.
void RenderTransientLoopShortAVX2(const RenderSpanContext& c,
                                  const uint32_t* handles,
                                  uint32_t handleCount) {
    VoiceSoA& v = *c.voices;
    const uint32_t frames = c.frameCount;
    const __m256 zero = _mm256_setzero_ps();
    __m256 sumsL[8], sumsR[8];
    for (uint32_t f = 0u; f < frames; ++f) {
        sumsL[f] = zero;
        sumsR[f] = zero;
    }
    uint32_t position = 0u;
    for (; position + 8u <= handleCount; position += 8u) {
        const __m256i h = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(handles + position));
        alignas(32) uint32_t hs[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(hs), h);
        __m256 phase = _mm256_max_ps(
            _mm256_i32gather_ps(v.phases, h, 4), zero);
        const __m256 step = _mm256_i32gather_ps(v.phaseIncs, h, 4);
        __m256 gain = _mm256_i32gather_ps(v.currentGain, h, 4);
        const __m256 mixL = _mm256_i32gather_ps(v.mixGainL, h, 4);
        const __m256 mixR = _mm256_i32gather_ps(v.mixGainR, h, 4);
        const __m256 loopStartF = _mm256_i32gather_ps(v.relLoopSF, h, 4);
        const __m256 loopEndF = _mm256_i32gather_ps(v.relLoopEF, h, 4);
        const __m256 targetGain = _mm256_i32gather_ps(v.targetGain, h, 4);
        const __m256 sustainLevel = _mm256_i32gather_ps(v.sustainLevel, h, 4);
        const __m256 attackStep = _mm256_i32gather_ps(v.attackGainStep, h, 4);
        const __m256 decaySlope = _mm256_i32gather_ps(v.decaySlope, h, 4);
        const __m256i sampleStart = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.sampleStart), h, 4);
        const __m256i loopStart = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.relLoopS), h, 4);
        const __m256i loopEnd = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.relLoopE), h, 4);
        alignas(32) uint32_t attackR[8], decayR[8];
        alignas(32) uint8_t stageV[8], initialStage[8];
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            attackR[lane] = v.attackSamplesRemaining[handle];
            decayR[lane] = v.decaySamplesRemaining[handle];
            stageV[lane] = v.envelopeStage[handle];
            initialStage[lane] = stageV[lane];
        }

        for (uint32_t frame = 0u; frame < frames; ++frame) {
            const __m256 pastLoop = _mm256_cmp_ps(
                phase, loopEndF, _CMP_GE_OQ);
            phase = _mm256_blendv_ps(phase,
                _mm256_add_ps(loopStartF, _mm256_sub_ps(phase, loopEndF)),
                pastLoop);
            const __m256i base = _mm256_cvttps_epi32(phase);
            const __m256i nextRaw = _mm256_add_epi32(
                base, _mm256_set1_epi32(1));
            const __m256i wrap = _mm256_cmpgt_epi32(
                nextRaw, _mm256_sub_epi32(loopEnd, _mm256_set1_epi32(1)));
            const __m256i next = _mm256_blendv_epi8(nextRaw, loopStart, wrap);
            const __m256 first = GatherSampleAVX2(c.sampleData,
                _mm256_add_epi32(sampleStart, base));
            const __m256 second = GatherSampleAVX2(c.sampleData,
                _mm256_add_epi32(sampleStart, next));
            const __m256 fraction = _mm256_sub_ps(
                phase, _mm256_cvtepi32_ps(base));
            __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            sample = FilterVoices8AVX2(v, h, sample);
            // Exact scalar envelope recurrence per lane: gains are
            // bit-identical to the per-voice scalar sequence.
            alignas(32) float gains[8], targets[8], sustains[8];
            alignas(32) float aSteps[8], dSlopes[8];
            _mm256_store_ps(gains, gain);
            _mm256_store_ps(targets, targetGain);
            _mm256_store_ps(sustains, sustainLevel);
            _mm256_store_ps(aSteps, attackStep);
            _mm256_store_ps(dSlopes, decaySlope);
            for (uint32_t lane = 0u; lane < 8u; ++lane) {
                if (stageV[lane] == 1u) {
                    if (attackR[lane] > 0u) {
                        gains[lane] += aSteps[lane];
                        --attackR[lane];
                        if (gains[lane] > targets[lane])
                            gains[lane] = targets[lane];
                    } else {
                        gains[lane] = targets[lane];
                    }
                    if (attackR[lane] == 0u)
                        stageV[lane] = decayR[lane] > 0u ? 2u : 3u;
                }
                if (stageV[lane] == 2u) {
                    if (decayR[lane] > 0u) {
                        gains[lane] *= dSlopes[lane];
                        --decayR[lane];
                        if (gains[lane] < sustains[lane])
                            gains[lane] = sustains[lane];
                    } else {
                        gains[lane] = sustains[lane];
                    }
                    if (decayR[lane] == 0u) stageV[lane] = 3u;
                }
            }
            gain = _mm256_load_ps(gains);
            const __m256 scaled = _mm256_mul_ps(sample, gain);
            sumsL[frame] = _mm256_add_ps(sumsL[frame],
                _mm256_mul_ps(scaled, mixL));
            sumsR[frame] = _mm256_add_ps(sumsR[frame],
                _mm256_mul_ps(scaled, mixR));
            phase = _mm256_add_ps(phase, step);
        }
        alignas(32) float finalGains[8], finalPhases[8];
        _mm256_store_ps(finalGains, gain);
        _mm256_store_ps(finalPhases, phase);
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            const uint8_t finalStage = stageV[lane];
            v.phases[handle] = finalPhases[lane];
            v.currentGain[handle] = finalGains[lane];
            v.envelopeStage[handle] = finalStage;
            v.attackSamplesRemaining[handle] = attackR[lane];
            v.decaySamplesRemaining[handle] = decayR[lane];
            if (finalStage != initialStage[lane] &&
                c.classChangeHandles != nullptr &&
                c.classChangeCount != nullptr) {
                c.classChangeHandles[(*c.classChangeCount)++] = handle;
            }
        }
    }
    for (; position < handleCount; ++position) {
        RenderTransientLoopFramesAVX2(c, handles[position]);
    }
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        c.outputLeft[c.frameStart + frame] += HorizontalSum(sumsL[frame]);
        c.outputRight[c.frameStart + frame] += HorizontalSum(sumsR[frame]);
    }
}

bool RenderTransientLoopAVX2(const RenderSpanContext& context,

                             const uint32_t* handles,
                             uint32_t handleCount) {
    if (handleCount == 0u) return true;
    if (context.frameCount == 0u || context.sampleData == nullptr) return false;
    // Per-voice phase rotation: form-2 voices take the time-chunked kernel;
    // short spans and diffuse (form 1) voices refuse like the release kernel.
    if (context.voices->rot != nullptr) {
        if (context.frameCount <= 7u) return false;
        for (uint32_t position = 0u; position < handleCount; ++position)
            if (context.voices->rot[handles[position]].form == 1u)
                return false;
    }
    // Channel buses: the voice-batched short kernel cannot scatter into
    // per-channel planes (see RenderReleaseLoopAVX2). Refuse before any
    // validation side effect.
    if (context.channelBusLeft != nullptr && context.frameCount <= 7u)
        return false;
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
    // Short event-fragmented spans: vectorize across voices (8 lanes) since
    // the per-voice time-chunk guard (frame + 8 <= frameCount) cannot fire.
    if (context.frameCount <= 7u) {
        RenderTransientLoopShortAVX2(context, handles, handleCount);
        _mm256_zeroupper();
        return true;
    }
    for (uint32_t position = 0u; position < handleCount; ++position) {
        const RenderSpanContext vc = SelectVoiceDestination(
            context, v.channel[handles[position]]);
        RenderTransientLoopFramesAVX2(vc, handles[position]);
    }
    _mm256_zeroupper();
    return true;
}



// ════════════════════════════════════════════════════════════════════════
// Filtered sustained loops, eight voices per vector.
//
// The per-voice time-chunk kernel interpolates eight frames at once but must
// run the voice's recursive 2-pole low-pass one sample at a time (scalar
// order, SoA load/store of z1/z2 each sample): about 62 of its 80 cycles per
// chunk.  Eight DIFFERENT voices have independent filter states, so this
// group kernel interpolates eight voices' chunks as before, transposes the
// 8x8 block so that one vector holds the same time step of eight voices, runs
// the biquad on all of them at once (the identical operations in the
// identical order, hence bit-identical per-voice results), and transposes
// back to mix.
//
// Each lane keeps its own frame cursor: a lane that straddles its loop end
// steps scalar frames (exactly what the per-voice kernel does) while the
// others wait on their next chunk.  When any lane has fewer than 8 frames
// left, the group stops and every lane finishes through the per-voice kernel
// on the remaining sub-span (all state lives in the SoA, so the hand-off is
// exact).  Lanes mix in lane order, which is the handle order the sequential
// path uses, so even the mix order is unchanged while lanes stay aligned.
// ════════════════════════════════════════════════════════════════════════
bool g_filteredGroupsEnabled = true;

inline void Transpose8x8Ps(__m256* r) {
    const __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]);
    const __m256 t1 = _mm256_unpackhi_ps(r[0], r[1]);
    const __m256 t2 = _mm256_unpacklo_ps(r[2], r[3]);
    const __m256 t3 = _mm256_unpackhi_ps(r[2], r[3]);
    const __m256 t4 = _mm256_unpacklo_ps(r[4], r[5]);
    const __m256 t5 = _mm256_unpackhi_ps(r[4], r[5]);
    const __m256 t6 = _mm256_unpacklo_ps(r[6], r[7]);
    const __m256 t7 = _mm256_unpackhi_ps(r[6], r[7]);
    const __m256 u0 = _mm256_shuffle_ps(t0, t2, 0x44);
    const __m256 u1 = _mm256_shuffle_ps(t0, t2, 0xEE);
    const __m256 u2 = _mm256_shuffle_ps(t1, t3, 0x44);
    const __m256 u3 = _mm256_shuffle_ps(t1, t3, 0xEE);
    const __m256 u4 = _mm256_shuffle_ps(t4, t6, 0x44);
    const __m256 u5 = _mm256_shuffle_ps(t4, t6, 0xEE);
    const __m256 u6 = _mm256_shuffle_ps(t5, t7, 0x44);
    const __m256 u7 = _mm256_shuffle_ps(t5, t7, 0xEE);
    r[0] = _mm256_permute2f128_ps(u0, u4, 0x20);
    r[1] = _mm256_permute2f128_ps(u1, u5, 0x20);
    r[2] = _mm256_permute2f128_ps(u2, u6, 0x20);
    r[3] = _mm256_permute2f128_ps(u3, u7, 0x20);
    r[4] = _mm256_permute2f128_ps(u0, u4, 0x31);
    r[5] = _mm256_permute2f128_ps(u1, u5, 0x31);
    r[6] = _mm256_permute2f128_ps(u2, u6, 0x31);
    r[7] = _mm256_permute2f128_ps(u3, u7, 0x31);
}

// True when RenderSustainedLoopFramesAVX2 would run this voice through its
// filtered time-chunk path (no rotation: the caller excludes v.rot).
inline bool FilteredGroupEligible(const RenderSpanContext& c, uint32_t h) {
    const VoiceSoA& v = *c.voices;
    if (v.filterEnabled[h] == 0u || !ValidateLoopVoice(c, h)) return false;
    const bool lfoOn = c.lfoActive != 0u &&
        v.vibLfoSteps[h] != 0.0f && v.vibLfoToPitchCents[h] > 0.0f &&
        c.lfoDepth > 0.0f;
    return !lfoOn;
}

void RenderSustainedFilteredGroupAVX2(const RenderSpanContext& c,
                                      const uint32_t* h) {
    VoiceSoA& v = *c.voices;
    RenderSpanContext dest[8];
    float phase[8], step[8], loopStart[8], loopEnd[8], loopLength[8];
    float gainL[8], gainR[8];
    const int16_t* region[8];
    float* outL[8];
    float* outR[8];
    uint32_t frame[8];
    for (uint32_t i = 0u; i < 8u; ++i) {
        dest[i] = SelectVoiceDestination(c, v.channel[h[i]]);
        phase[i] = (std::max)(0.0f, v.phases[h[i]]);
        step[i] = v.phaseIncs[h[i]];
        loopStart[i] = v.relLoopSF[h[i]];
        loopEnd[i] = v.relLoopEF[h[i]];
        loopLength[i] = loopEnd[i] - loopStart[i];
        gainL[i] = v.renderGainL[h[i]];
        gainR[i] = v.renderGainR[h[i]];
        region[i] = c.sampleData + v.sampleStart[h[i]];
        outL[i] = dest[i].outputLeft + c.frameStart;
        outR[i] = dest[i].outputRight + c.frameStart;
        frame[i] = 0u;
    }
    alignas(32) float a0a[8], b1a[8], b2a[8], z1a[8], z2a[8];
    for (uint32_t i = 0u; i < 8u; ++i) {
        a0a[i] = v.filterA0[h[i]];
        b1a[i] = v.filterB1[h[i]];
        b2a[i] = v.filterB2[h[i]];
    }
    const __m256 a0v = _mm256_load_ps(a0a);
    const __m256 a0x2v = _mm256_add_ps(a0v, a0v);  // == 2.0f * a0, exactly
    const __m256 b1v = _mm256_load_ps(b1a);
    const __m256 b2v = _mm256_load_ps(b2a);
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    __m256 z1, z2;
    auto loadZ = [&]() {
        for (uint32_t i = 0u; i < 8u; ++i) {
            z1a[i] = v.filterZ1[h[i]];
            z2a[i] = v.filterZ2[h[i]];
        }
        z1 = _mm256_load_ps(z1a);
        z2 = _mm256_load_ps(z2a);
    };
    auto spillZ = [&]() {
        _mm256_store_ps(z1a, z1);
        _mm256_store_ps(z2a, z2);
        for (uint32_t i = 0u; i < 8u; ++i) {
            v.filterZ1[h[i]] = z1a[i];
            v.filterZ2[h[i]] = z2a[i];
        }
    };
    loadZ();

    for (;;) {
        bool done = false;
        bool spilled = false;
        for (uint32_t i = 0u; i < 8u && !done; ++i) {
            for (;;) {
                if (phase[i] >= loopEnd[i]) {
                    float overflow = phase[i] - loopEnd[i];
                    if (overflow >= loopLength[i])
                        overflow -= std::floor(overflow / loopLength[i]) *
                                    loopLength[i];
                    phase[i] = loopStart[i] + overflow;
                }
                if (frame[i] + 8u > c.frameCount) { done = true; break; }
                if (phase[i] + step[i] * 7.0f < loopEnd[i] - 1.0f) break;
                // Straddling this lane's loop end: one scalar frame, exactly
                // as the per-voice kernel steps it (filter state via the SoA).
                if (!spilled) { spillZ(); spilled = true; }
                const uint32_t hh = h[i];
                uint32_t base = static_cast<uint32_t>(phase[i]);
                if (base + 1u >= v.relEnd[hh]) {
                    phase[i] = loopStart[i];
                    base = v.relLoopS[hh];
                }
                uint32_t next = base + 1u;
                if (next >= v.relLoopE[hh]) next = v.relLoopS[hh];
                const float fraction = phase[i] - static_cast<float>(base);
                const float first =
                    static_cast<float>(region[i][base]) * (1.0f / 32768.0f);
                float sample = first +
                    (static_cast<float>(region[i][next]) * (1.0f / 32768.0f) -
                     first) * fraction;
                sample = ProcessVoiceFilterSample(v, hh, sample);
                outL[i][frame[i]] += sample * gainL[i];
                outR[i][frame[i]] += sample * gainR[i];
                phase[i] += step[i];
                ++frame[i];
            }
        }
        if (done) {
            if (!spilled) spillZ();
            break;
        }
        if (spilled) loadZ();

        __m256 rows[8];
        for (uint32_t i = 0u; i < 8u; ++i) {
            const float lastPhase = phase[i] + step[i] * 7.0f;
            _mm_prefetch(reinterpret_cast<const char*>(
                             region[i] +
                             static_cast<uint32_t>(lastPhase + step[i])),
                         _MM_HINT_T0);
            const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase[i]),
                _mm256_mul_ps(_mm256_set1_ps(step[i]), lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairAVX2(region[i], bases, first, second);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            rows[i] = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
        }
        Transpose8x8Ps(rows);  // rows[k] = time step k of all eight voices
        for (uint32_t k = 0u; k < 8u; ++k) {
            const __m256 input = rows[k];
            const __m256 output = _mm256_add_ps(
                _mm256_mul_ps(input, a0v), z1);
            z1 = _mm256_sub_ps(
                _mm256_add_ps(_mm256_mul_ps(input, a0x2v), z2),
                _mm256_mul_ps(b1v, output));
            z2 = _mm256_sub_ps(
                _mm256_mul_ps(input, a0v), _mm256_mul_ps(b2v, output));
            rows[k] = output;
        }
        Transpose8x8Ps(rows);  // back to one voice per vector
        for (uint32_t i = 0u; i < 8u; ++i) {
            _mm256_storeu_ps(outL[i] + frame[i], _mm256_add_ps(
                _mm256_loadu_ps(outL[i] + frame[i]),
                _mm256_mul_ps(rows[i], _mm256_set1_ps(gainL[i]))));
            _mm256_storeu_ps(outR[i] + frame[i], _mm256_add_ps(
                _mm256_loadu_ps(outR[i] + frame[i]),
                _mm256_mul_ps(rows[i], _mm256_set1_ps(gainR[i]))));
            for (uint32_t k = 0u; k < 8u; ++k) phase[i] += step[i];
            frame[i] += 8u;
        }
    }

    for (uint32_t i = 0u; i < 8u; ++i) v.phases[h[i]] = phase[i];
    for (uint32_t i = 0u; i < 8u; ++i) {
        if (frame[i] >= c.frameCount) continue;
        RenderSpanContext rest = dest[i];
        rest.frameStart = c.frameStart + frame[i];
        rest.frameCount = c.frameCount - frame[i];
        RenderSustainedLoopFramesAVX2(rest, h[i]);
    }
}

// Sustained-loop spans of 8+ frames, no rotation: eight consecutive eligible
// filtered voices go through the group kernel, everything else through the
// per-voice kernel, preserving the handle order.
void RenderSustainedFramesGrouped(const RenderSpanContext& c,
                                  const uint32_t* handles, uint32_t count) {
    VoiceSoA& v = *c.voices;
    uint32_t pending[8];
    uint32_t pendingCount = 0u;
    auto flush = [&]() {
        if (pendingCount == 8u) {
            RenderSustainedFilteredGroupAVX2(c, pending);
        } else {
            for (uint32_t k = 0u; k < pendingCount; ++k) {
                const RenderSpanContext vc = SelectVoiceDestination(
                    c, v.channel[pending[k]]);
                RenderSustainedLoopFramesAVX2(vc, pending[k]);
            }
        }
        pendingCount = 0u;
    };
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t handle = handles[i];
        if (g_filteredGroupsEnabled && FilteredGroupEligible(c, handle)) {
            pending[pendingCount++] = handle;
            if (pendingCount == 8u) flush();
            continue;
        }
        flush();
        const RenderSpanContext vc = SelectVoiceDestination(
            c, v.channel[handle]);
        RenderSustainedLoopFramesAVX2(vc, handle);
    }
    flush();
}

// Release-loop twin of the sustained group kernel above: eight filtered
// releasing voices in lockstep.  The envelope is the same exact sequential
// recurrence the per-voice kernel simulates (gains[8] per lane, no powers);
// a lane whose countdown or retire threshold is not safe for a whole chunk
// ends the group, and every lane then finishes through the per-voice kernel
// on the remaining sub-span (which owns retirement).  retiredAt[i] is the
// span-relative frame the voice retires on, or UINT32_MAX.
inline bool FilteredReleaseGroupEligible(const RenderSpanContext& c,
                                         uint32_t h) {
    return FilteredGroupEligible(c, h);
}

void RenderReleaseFilteredGroupAVX2(const RenderSpanContext& c,
                                    const uint32_t* h, uint32_t* retiredAt) {
    VoiceSoA& v = *c.voices;
    RenderSpanContext dest[8];
    float phase[8], step[8], loopStart[8], loopEnd[8], loopLength[8];
    float gain[8], decay[8], mixL[8], mixR[8];
    uint32_t remaining[8];
    const int16_t* region[8];
    float* outL[8];
    float* outR[8];
    uint32_t frame[8];
    for (uint32_t i = 0u; i < 8u; ++i) {
        dest[i] = SelectVoiceDestination(c, v.channel[h[i]]);
        phase[i] = (std::max)(0.0f, v.phases[h[i]]);
        step[i] = v.phaseIncs[h[i]];
        loopStart[i] = v.relLoopSF[h[i]];
        loopEnd[i] = v.relLoopEF[h[i]];
        loopLength[i] = loopEnd[i] - loopStart[i];
        gain[i] = v.currentGain[h[i]];
        remaining[i] = v.releaseSamplesRemaining[h[i]];
        decay[i] = v.releaseDecay[h[i]];
        mixL[i] = v.mixGainL[h[i]];
        mixR[i] = v.mixGainR[h[i]];
        region[i] = c.sampleData + v.sampleStart[h[i]];
        outL[i] = dest[i].outputLeft + c.frameStart;
        outR[i] = dest[i].outputRight + c.frameStart;
        frame[i] = 0u;
        retiredAt[i] = UINT32_MAX;
    }
    alignas(32) float a0a[8], b1a[8], b2a[8], z1a[8], z2a[8];
    for (uint32_t i = 0u; i < 8u; ++i) {
        a0a[i] = v.filterA0[h[i]];
        b1a[i] = v.filterB1[h[i]];
        b2a[i] = v.filterB2[h[i]];
    }
    const __m256 a0v = _mm256_load_ps(a0a);
    const __m256 a0x2v = _mm256_add_ps(a0v, a0v);
    const __m256 b1v = _mm256_load_ps(b1a);
    const __m256 b2v = _mm256_load_ps(b2a);
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    __m256 z1, z2;
    auto loadZ = [&]() {
        for (uint32_t i = 0u; i < 8u; ++i) {
            z1a[i] = v.filterZ1[h[i]];
            z2a[i] = v.filterZ2[h[i]];
        }
        z1 = _mm256_load_ps(z1a);
        z2 = _mm256_load_ps(z2a);
    };
    auto spillZ = [&]() {
        _mm256_store_ps(z1a, z1);
        _mm256_store_ps(z2a, z2);
        for (uint32_t i = 0u; i < 8u; ++i) {
            v.filterZ1[h[i]] = z1a[i];
            v.filterZ2[h[i]] = z2a[i];
        }
    };
    loadZ();

    alignas(32) float gains[8][8];
    float futureGain[8];
    for (;;) {
        bool done = false;
        bool spilled = false;
        for (uint32_t i = 0u; i < 8u && !done; ++i) {
            for (;;) {
                if (phase[i] >= loopEnd[i]) {
                    float overflow = phase[i] - loopEnd[i];
                    if (overflow >= loopLength[i])
                        overflow -= std::floor(overflow / loopLength[i]) *
                                    loopLength[i];
                    phase[i] = loopStart[i] + overflow;
                }
                if (frame[i] + 8u > c.frameCount) { done = true; break; }
                float fg = gain[i];
                for (uint32_t k = 0u; k < 8u; ++k) {
                    fg *= decay[i];
                    gains[i][k] = fg;
                }
                futureGain[i] = fg;
                const bool countdownSafe =
                    remaining[i] == UINT32_MAX || remaining[i] > 8u;
                const bool thresholdSafe = remaining[i] != UINT32_MAX ||
                    gains[i][7] >= VoiceRetireThreshold();
                if (!countdownSafe || !thresholdSafe) { done = true; break; }
                if (phase[i] + step[i] * 7.0f < loopEnd[i] - 1.0f) break;
                // Loop-end straddle: one scalar frame exactly as the
                // per-voice kernel steps it (safe: no retirement possible).
                if (!spilled) { spillZ(); spilled = true; }
                const uint32_t hh = h[i];
                uint32_t base = static_cast<uint32_t>(phase[i]);
                if (base + 1u >= v.relEnd[hh]) {
                    phase[i] = loopStart[i];
                    base = v.relLoopS[hh];
                }
                uint32_t next = base + 1u;
                if (next >= v.relLoopE[hh]) next = v.relLoopS[hh];
                const float fraction = phase[i] - static_cast<float>(base);
                const float first =
                    static_cast<float>(region[i][base]) * (1.0f / 32768.0f);
                float sample = first +
                    (static_cast<float>(region[i][next]) * (1.0f / 32768.0f) -
                     first) * fraction;
                bool finished = remaining[i] == 0u;
                if (!finished) {
                    gain[i] *= decay[i];
                    if (remaining[i] != UINT32_MAX) {
                        --remaining[i];
                        finished = remaining[i] == 0u;
                    }
                }
                sample = ProcessVoiceFilterSample(v, hh, sample);
                outL[i][frame[i]] += sample * gain[i] * mixL[i];
                outR[i][frame[i]] += sample * gain[i] * mixR[i];
                phase[i] += step[i];
                if (phase[i] >= loopEnd[i]) {
                    float overflow = phase[i] - loopEnd[i];
                    if (overflow >= loopLength[i])
                        overflow -= std::floor(overflow / loopLength[i]) *
                                    loopLength[i];
                    phase[i] = loopStart[i] + overflow;
                }
                (void)finished;  // cannot be set: the chunk was proven safe
                ++frame[i];
            }
        }
        if (done) {
            if (!spilled) spillZ();
            break;
        }
        if (spilled) loadZ();

        __m256 rows[8];
        for (uint32_t i = 0u; i < 8u; ++i) {
            const float lastPhase = phase[i] + step[i] * 7.0f;
            _mm_prefetch(reinterpret_cast<const char*>(
                             region[i] +
                             static_cast<uint32_t>(lastPhase + step[i])),
                         _MM_HINT_T0);
            const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase[i]),
                _mm256_mul_ps(_mm256_set1_ps(step[i]), lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairAVX2(region[i], bases, first, second);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            rows[i] = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
        }
        Transpose8x8Ps(rows);
        for (uint32_t k = 0u; k < 8u; ++k) {
            const __m256 input = rows[k];
            const __m256 output = _mm256_add_ps(
                _mm256_mul_ps(input, a0v), z1);
            z1 = _mm256_sub_ps(
                _mm256_add_ps(_mm256_mul_ps(input, a0x2v), z2),
                _mm256_mul_ps(b1v, output));
            z2 = _mm256_sub_ps(
                _mm256_mul_ps(input, a0v), _mm256_mul_ps(b2v, output));
            rows[k] = output;
        }
        Transpose8x8Ps(rows);
        for (uint32_t i = 0u; i < 8u; ++i) {
            const __m256 scaled = _mm256_mul_ps(
                rows[i], _mm256_load_ps(gains[i]));
            _mm256_storeu_ps(outL[i] + frame[i], _mm256_add_ps(
                _mm256_loadu_ps(outL[i] + frame[i]),
                _mm256_mul_ps(scaled, _mm256_set1_ps(mixL[i]))));
            _mm256_storeu_ps(outR[i] + frame[i], _mm256_add_ps(
                _mm256_loadu_ps(outR[i] + frame[i]),
                _mm256_mul_ps(scaled, _mm256_set1_ps(mixR[i]))));
            gain[i] = futureGain[i];
            if (remaining[i] != UINT32_MAX) remaining[i] -= 8u;
            for (uint32_t k = 0u; k < 8u; ++k) phase[i] += step[i];
            frame[i] += 8u;
        }
    }

    for (uint32_t i = 0u; i < 8u; ++i) {
        v.phases[h[i]] = phase[i];
        v.currentGain[h[i]] = gain[i];
        v.releaseSamplesRemaining[h[i]] = remaining[i];
    }
    for (uint32_t i = 0u; i < 8u; ++i) {
        if (frame[i] >= c.frameCount) continue;
        RenderSpanContext rest = dest[i];
        rest.frameStart = c.frameStart + frame[i];
        rest.frameCount = c.frameCount - frame[i];
        const uint32_t at = RenderReleaseLoopFramesAVX2(rest, h[i]);
        if (at != UINT32_MAX) retiredAt[i] = frame[i] + at;
    }
}

// Release-loop spans of 8+ frames, no rotation: group kernel for runs of
// eight eligible filtered voices, per-voice kernel for the rest, handle order
// (and therefore retirement recording order) preserved.
void RenderReleaseFramesGrouped(const RenderSpanContext& c,
                                const uint32_t* handles, uint32_t count) {
    VoiceSoA& v = *c.voices;
    uint32_t pending[8];
    uint32_t pendingCount = 0u;
    auto perVoice = [&](uint32_t handle) {
        uint32_t retiredAt;
        if (c.channelBusLeft == nullptr) {
            retiredAt = RenderReleaseLoopFramesAVX2(c, handle);  // no copy
        } else {
            const RenderSpanContext vc = SelectVoiceDestination(
                c, v.channel[handle]);
            retiredAt = RenderReleaseLoopFramesAVX2(vc, handle);
        }
        if (retiredAt != UINT32_MAX) RecordRetirement(c, handle, retiredAt);
    };
    auto flush = [&]() {
        if (pendingCount == 8u) {
            uint32_t retiredAt[8];
            RenderReleaseFilteredGroupAVX2(c, pending, retiredAt);
            for (uint32_t k = 0u; k < 8u; ++k)
                if (retiredAt[k] != UINT32_MAX)
                    RecordRetirement(c, pending[k], retiredAt[k]);
        } else {
            for (uint32_t k = 0u; k < pendingCount; ++k) perVoice(pending[k]);
        }
        pendingCount = 0u;
    };
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t handle = handles[i];
        if (g_filteredGroupsEnabled && c.frameCount >= 8u &&
            FilteredReleaseGroupEligible(c, handle)) {
            pending[pendingCount++] = handle;
            if (pendingCount == 8u) flush();
            continue;
        }
        flush();
        perVoice(handle);
    }
    flush();
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
bool RenderSustainedLoopAVX2(const RenderSpanContext& context,
                             const uint32_t* handles,
                             uint32_t handleCount) {
    VoiceSoA& v = *context.voices;
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    // Per-voice phase rotation: spans of 8+ frames take the time-chunked
    // kernel, which rotates form-2 (Hilbert-pair) voices in the lanes and
    // hands any other form back to the scalar span (the diffuse allpass is
    // recursive). Short spans stay scalar. Sustained gain is constant, so
    // there is no envelope to simulate; the former voice-parallel rotation
    // kernel that skipped the envelope for non-sustained classes is gone.
    if (v.rot != nullptr) {
        for (uint32_t i = 0u; i < handleCount; ++i) {
            const RenderSpanContext vc = SelectVoiceDestination(
                context, v.channel[handles[i]]);
            if (vc.frameCount > 7u) {
                RenderSustainedLoopFramesAVX2(vc, handles[i]);
                continue;
            }
            ScalarRenderSustainedLoop(v, handles[i], vc.sampleData,
                vc.hilbertData, vc.sampleDataFrames,
                vc.outputLeft,
                vc.outputRight, vc.frameStart, vc.frameCount);
        }
        _mm256_zeroupper();
        return true;
    }
    // Channel buses: short spans run per-voice scalar spans with per-voice
    // plane selection (the voice-batched path below mixes eight channels
    // into one frame position). Longer spans keep the time-chunked AVX2
    // kernels — each processes one voice, so the per-voice context copy
    // redirects the mix to the voice's channel plane.
    if (context.channelBusLeft != nullptr) {
        if (context.frameCount > 7u) {
            RenderSustainedFramesGrouped(context, handles, handleCount);
            _mm256_zeroupper();
            return true;
        }
        for (uint32_t i = 0u; i < handleCount; ++i) {
            const RenderSpanContext vc = SelectVoiceDestination(
                context, v.channel[handles[i]]);
            ScalarRenderSustainedLoop(v, handles[i], vc.sampleData,
                vc.hilbertData, vc.sampleDataFrames,
                vc.outputLeft,
                vc.outputRight, vc.frameStart, vc.frameCount);
        }
        _mm256_zeroupper();
        return true;
    }
    if (context.frameCount > 7u) {
        RenderSustainedFramesGrouped(context, handles, handleCount);
        _mm256_zeroupper();
        return true;
    }

    const bool denseHandles = context.voiceCapacity >= 8u &&
        handleCount * 4u >= context.voiceCapacity * 3u;
    const uint32_t iterationCount =
        denseHandles ? context.voiceCapacity : handleCount;
    __m256 accumulatedLeft[8] = {
        _mm256_setzero_ps(), _mm256_setzero_ps(),
        _mm256_setzero_ps(), _mm256_setzero_ps(),
        _mm256_setzero_ps(), _mm256_setzero_ps(),
        _mm256_setzero_ps(), _mm256_setzero_ps()};
    __m256 accumulatedRight[8] = {
        _mm256_setzero_ps(), _mm256_setzero_ps(),
        _mm256_setzero_ps(), _mm256_setzero_ps(),
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
                            context.hilbertData, context.sampleDataFrames,
                            context.outputLeft,
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
                    context.hilbertData, context.sampleDataFrames,
                    context.outputLeft,
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
            __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            sample = FilterVoices8AVX2(v, h, sample);
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
                context.hilbertData, context.sampleDataFrames,
                context.outputLeft, context.outputRight,
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

void SetFilteredGroupKernelForTest(bool enabled) {
    g_filteredGroupsEnabled = enabled;
}

void SetFastChunkMath(bool enabled) {
    g_fastChunkMath = enabled;
}

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

// ── Scalar one-shot span helpers ────────────────────────────────────────
// Exact mirrors of the generic envelope state machine's branches
// (SVMSRenderScalar.h RenderPrimaryVoiceSpan) for spans too short to
// vectorize and for states the batch validation refuses.  Used by the
// batch entries below; no AVX2 is executed on these paths, so calling
// them costs no AVX↔SSE transition.

uint32_t RenderReleaseOneShotSpanScalar(
    VoiceSoA& v, uint32_t idx, const int16_t* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount) {
    const uint32_t relEnd = v.relEnd[idx];
    if (sampleData == nullptr || relEnd < 2u ||
        v.sampleStart[idx] >= sampleDataFrames ||
        relEnd > sampleDataFrames - v.sampleStart[idx]) {
        return 0u;
    }
    float phase = (std::max)(0.0f, v.phases[idx]);
    float gain = v.currentGain[idx];
    uint32_t remaining = v.releaseSamplesRemaining[idx];
    const float decay = v.releaseDecay[idx];
    const float step = v.phaseIncs[idx];
    const float mixL = v.mixGainL[idx];
    const float mixR = v.mixGainR[idx];
    const int16_t* region = sampleData + v.sampleStart[idx];
    float* outL = outputLeft + frameStart;
    float* outR = outputRight + frameStart;
    for (uint32_t frame = 0u; frame < frameCount; ++frame) {
        const uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= relEnd) {
            v.phases[idx] = phase;
            v.currentGain[idx] = gain;
            v.releaseSamplesRemaining[idx] = remaining;
            return frame;
        }
        bool finished = remaining == 0u;
        if (!finished) {
            gain *= decay;
            if (remaining != UINT32_MAX) {
                --remaining;
                finished = remaining == 0u;
            }
        }
        const float fraction = phase - static_cast<float>(base);
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[base + 1u]) *
            (1.0f / 32768.0f) - first) * fraction;
        sample = ProcessVoiceFilterSample(v, idx, sample);
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        phase += step;
        if (finished ||
            (remaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
            v.phases[idx] = phase;
            v.currentGain[idx] = gain;
            v.releaseSamplesRemaining[idx] = remaining;
            return frame;
        }
    }
    v.phases[idx] = phase;
    v.currentGain[idx] = gain;
    v.releaseSamplesRemaining[idx] = remaining;
    return UINT32_MAX;
}

uint32_t RenderTransientOneShotSpanScalar(
    VoiceSoA& v, uint32_t idx, const int16_t* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount) {
    const uint32_t relEnd = v.relEnd[idx];
    if (sampleData == nullptr || relEnd < 2u ||
        v.sampleStart[idx] >= sampleDataFrames ||
        relEnd > sampleDataFrames - v.sampleStart[idx]) {
        return UINT32_MAX;
    }
    float phase = (std::max)(0.0f, v.phases[idx]);
    float gain = v.currentGain[idx];
    uint8_t stage = v.envelopeStage[idx];
    uint32_t delayRemaining = v.delaySamplesRemaining[idx];
    uint32_t holdRemaining = v.holdSamplesRemaining[idx];
    uint32_t attackRemaining = v.attackSamplesRemaining[idx];
    uint32_t decayRemaining = v.decaySamplesRemaining[idx];
    const float step = v.phaseIncs[idx];
    const float targetGain = v.targetGain[idx];
    const float sustainLevel = v.sustainLevel[idx];
    const float attackStep = v.attackGainStep[idx];
    const float decaySlope = v.decaySlope[idx];
    const float mixL = v.mixGainL[idx];
    const float mixR = v.mixGainR[idx];
    const int16_t* region = sampleData + v.sampleStart[idx];
    float* outL = outputLeft + frameStart;
    float* outR = outputRight + frameStart;
    for (uint32_t frame = 0u; frame < frameCount; ++frame) {
        const uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= relEnd) {
            // One-shot sample ended: retire (no loop to wrap to).
            v.phases[idx] = phase;
            v.currentGain[idx] = gain;
            v.envelopeStage[idx] = stage;
            v.delaySamplesRemaining[idx] = delayRemaining;
            v.holdSamplesRemaining[idx] = holdRemaining;
            v.attackSamplesRemaining[idx] = attackRemaining;
            v.decaySamplesRemaining[idx] = decayRemaining;
            return frame;
        }
        // The exact envelope sequence from the generic state machine
        // (unreleased branch).
        if (stage == 4u) {
            if (delayRemaining > 0u) {
                --delayRemaining;
                gain = 0.0f;
            } else {
                stage = 0u;
            }
        }
        if (stage == 0u) {
            if (holdRemaining > 0u) {
                --holdRemaining;
                gain = targetGain;
            } else {
                stage = 1u;
            }
        }
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
        const float fraction = phase - static_cast<float>(base);
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[base + 1u]) *
            (1.0f / 32768.0f) - first) * fraction;
        sample = ProcessVoiceFilterSample(v, idx, sample);
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        phase += step;
    }
    v.phases[idx] = phase;
    v.currentGain[idx] = gain;
    v.envelopeStage[idx] = stage;
    v.delaySamplesRemaining[idx] = delayRemaining;
    v.holdSamplesRemaining[idx] = holdRemaining;
    v.attackSamplesRemaining[idx] = attackRemaining;
    v.decaySamplesRemaining[idx] = decayRemaining;
    return UINT32_MAX;
}

// ════════════════════════════════════════════════════════════════════════
// One-shot kernels (held and releasing non-looping voices).
//
// Every one-shot instrument (pianos, plucks, mallets — any SF2 region
// without a loop) used to classify into SustainedOneShot/ReleaseOneShot
// slots that had no AVX2 kernel, so their whole sustain and release
// rendered on the scalar per-voice fallback at ~19 cycles per frame —
// ~43x the vectorized rate, on both the realtime and prerender pipelines.
// These kernels mirror RenderSustainedLoopFramesAVX2 /
// RenderReleaseLoopFramesAVX2 with the loop-wrap math replaced by
// end-of-sample retirement:
//   - sustain: constant renderGain, phase walks toward relEnd, the voice
//     retires at the first frame whose base+1 >= relEnd (the lerp partner
//     would read past the sample);
//   - release: the release envelope (gain *= decay, countdown, threshold)
//     advances with the exact scalar recurrence into gains[8], the sample
//     end retires before the envelope consumes a frame, and the retire
//     conditions match the generic envelope state machine exactly.
// Samples longer than 2^24 frames fall back to the scalar path: the
// fast-chunk guard compares against a float-converted relEnd bound that
// is only exact below that point.
// ════════════════════════════════════════════════════════════════════════

uint32_t RenderSustainedOneShotFramesAVX2(const RenderSpanContext& c,
                                          uint32_t handle) {
    VoiceSoA& v = *c.voices;
    const uint32_t relEnd = v.relEnd[handle];
    float phase = (std::max)(0.0f, v.phases[handle]);
    float step = v.phaseIncs[handle];
    const float gainL = v.renderGainL[handle];
    const float gainR = v.renderGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    __m256 stepVector = _mm256_set1_ps(step);
    const __m256 gainLVector = _mm256_set1_ps(gainL);
    const __m256 gainRVector = _mm256_set1_ps(gainR);
    // The 8-frame fast chunk needs floor(phase + 7*step) + 1 < relEnd for
    // every lane; comparing against the float-converted bound is exact
    // while the sample fits in 24 bits of mantissa.
    const bool fastGuardExact = relEnd <= 0x1000000u && step >= 0.0f;
    const float lastSafePhase = fastGuardExact
        ? static_cast<float>(relEnd - 1u) : -1.0f;
    // Whole-voice vibrato windows (see RenderSustainedLoopFramesAVX2).
    const bool lfoOn = c.lfoActive != 0u &&
        v.vibLfoSteps[handle] != 0.0f &&
        v.vibLfoToPitchCents[handle] > 0.0f && c.lfoDepth > 0.0f;
    uint32_t windowRemaining = UINT32_MAX;
    if (lfoOn) {
        step = AdvanceVibratoLfoWindow(v, handle,
            (std::min)(64u, c.frameCount), c.lfoDepth, c.lfoBendRatio);
        stepVector = _mm256_set1_ps(step);
        windowRemaining = (std::min)(64u, c.frameCount);
    }
    uint32_t frame = 0u;
    while (frame < c.frameCount) {
        if (windowRemaining == 0u) {
            const uint32_t win = (std::min)(64u, c.frameCount - frame);
            step = AdvanceVibratoLfoWindow(v, handle, win, c.lfoDepth,
                                           c.lfoBendRatio);
            stepVector = _mm256_set1_ps(step);
            windowRemaining = win;
        }
        const float lastPhase = phase + step * 7.0f;
        if (fastGuardExact && frame + 8u <= c.frameCount &&
            windowRemaining >= 8u && lastPhase < lastSafePhase) {
            // Warm the next chunk's sample lines (sequential per voice).
            _mm_prefetch(reinterpret_cast<const char*>(
                             region +
                             static_cast<uint32_t>(lastPhase + step)),
                         _MM_HINT_T0);
            const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairAVX2(region, bases, first, second);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            sample = FilterTime8AVX2(v, handle, sample);
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
            windowRemaining -= 8u;
            continue;
        }
        const uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= relEnd) {
            // Sample ended: the lerp partner would read past the region.
            // Retire at this frame; the phase is not advanced.
            v.phases[handle] = phase;
            return frame;
        }
        const float fraction = phase - static_cast<float>(base);
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[base + 1u]) *
            (1.0f / 32768.0f) - first) * fraction;
        sample = ProcessVoiceFilterSample(v, handle, sample);
        outL[frame] += sample * gainL;
        outR[frame] += sample * gainR;
        phase += step;
        --windowRemaining;
        ++frame;
    }
    v.phases[handle] = phase;
    return UINT32_MAX;
}

uint32_t RenderReleaseOneShotFramesAVX2(const RenderSpanContext& c,
                                        uint32_t handle) {
    VoiceSoA& v = *c.voices;
    const uint32_t relEnd = v.relEnd[handle];
    float phase = (std::max)(0.0f, v.phases[handle]);
    float gain = v.currentGain[handle];
    uint32_t remaining = v.releaseSamplesRemaining[handle];
    const float decay = v.releaseDecay[handle];
    float step = v.phaseIncs[handle];
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    __m256 stepVector = _mm256_set1_ps(step);
    const bool fastGuardExact = relEnd <= 0x1000000u && step >= 0.0f;
    const float lastSafePhase = fastGuardExact
        ? static_cast<float>(relEnd - 1u) : -1.0f;
    // Whole-voice vibrato windows (see RenderSustainedLoopFramesAVX2).
    const bool lfoOn = c.lfoActive != 0u &&
        v.vibLfoSteps[handle] != 0.0f &&
        v.vibLfoToPitchCents[handle] > 0.0f && c.lfoDepth > 0.0f;
    uint32_t windowRemaining = UINT32_MAX;
    if (lfoOn) {
        step = AdvanceVibratoLfoWindow(v, handle,
            (std::min)(64u, c.frameCount), c.lfoDepth, c.lfoBendRatio);
        stepVector = _mm256_set1_ps(step);
        windowRemaining = (std::min)(64u, c.frameCount);
    }
    uint32_t frame = 0u;
    while (frame < c.frameCount) {
        if (windowRemaining == 0u) {
            const uint32_t win = (std::min)(64u, c.frameCount - frame);
            step = AdvanceVibratoLfoWindow(v, handle, win, c.lfoDepth,
                                           c.lfoBendRatio);
            stepVector = _mm256_set1_ps(step);
            windowRemaining = win;
        }
        float futureGain = gain;
        alignas(32) float gains[8];
        for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex) {
            futureGain *= decay;
            gains[laneIndex] = futureGain;
        }
        const bool countdownSafe = remaining == UINT32_MAX || remaining > 8u;
        const bool thresholdSafe = remaining != UINT32_MAX ||
            gains[7] >= VoiceRetireThreshold();
        const float lastPhase = phase + step * 7.0f;
        if (fastGuardExact && frame + 8u <= c.frameCount &&
            windowRemaining >= 8u && countdownSafe &&
            thresholdSafe && lastPhase < lastSafePhase) {
            _mm_prefetch(reinterpret_cast<const char*>(
                             region +
                             static_cast<uint32_t>(lastPhase + step)),
                         _MM_HINT_T0);
            const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairAVX2(region, bases, first, second);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            sample = FilterTime8AVX2(v, handle, sample);
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
            windowRemaining -= 8u;
            continue;
        }
        const uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= relEnd) {
            // Sample ended: retire before the envelope consumes a frame
            // (mirrors the generic state machine's sampleEnded branch).
            v.phases[handle] = phase;
            v.currentGain[handle] = gain;
            v.releaseSamplesRemaining[handle] = remaining;
            return frame;
        }
        bool finished = remaining == 0u;
        if (!finished) {
            gain *= decay;
            if (remaining != UINT32_MAX) {
                --remaining;
                finished = remaining == 0u;
            }
        }
        const float fraction = phase - static_cast<float>(base);
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[base + 1u]) *
            (1.0f / 32768.0f) - first) * fraction;
        sample = ProcessVoiceFilterSample(v, handle, sample);
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        phase += step;
        --windowRemaining;
        ++frame;
        if (finished ||
            (remaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
            v.phases[handle] = phase;
            v.currentGain[handle] = gain;
            v.releaseSamplesRemaining[handle] = remaining;
            return frame - 1u;
        }
    }
    v.phases[handle] = phase;
    v.currentGain[handle] = gain;
    v.releaseSamplesRemaining[handle] = remaining;
    return UINT32_MAX;
}

// Transient one-shot: non-looping attack/decay voices (the decay stage of
// every piano/pluck note).  Mirrors RenderTransientLoopFramesAVX2 with the
// loop-wrap math replaced by end-of-sample retirement; the envelope runs
// with the exact scalar recurrence into gains[8], stage transitions fall to
// the scalar tail so the class change is reported once.
uint32_t RenderTransientOneShotFramesAVX2(const RenderSpanContext& c,
                                          uint32_t handle) {
    VoiceSoA& v = *c.voices;
    const uint32_t relEnd = v.relEnd[handle];
    float phase = (std::max)(0.0f, v.phases[handle]);
    float gain = v.currentGain[handle];
    uint8_t stage = v.envelopeStage[handle];
    const uint8_t initialStage = stage;
    uint32_t attackRemaining = v.attackSamplesRemaining[handle];
    uint32_t decayRemaining = v.decaySamplesRemaining[handle];
    float step = v.phaseIncs[handle];
    const float targetGain = v.targetGain[handle];
    const float sustainLevel = v.sustainLevel[handle];
    const float attackStep = v.attackGainStep[handle];
    const float decaySlope = v.decaySlope[handle];
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f,
                                       4.0f, 5.0f, 6.0f, 7.0f);
    __m256 stepVector = _mm256_set1_ps(step);
    const bool fastGuardExact = relEnd <= 0x1000000u && step >= 0.0f;
    const float lastSafePhase = fastGuardExact
        ? static_cast<float>(relEnd - 1u) : -1.0f;
    // Whole-voice vibrato windows (see RenderSustainedLoopFramesAVX2).
    const bool lfoOn = c.lfoActive != 0u &&
        v.vibLfoSteps[handle] != 0.0f &&
        v.vibLfoToPitchCents[handle] > 0.0f && c.lfoDepth > 0.0f;
    uint32_t windowRemaining = UINT32_MAX;
    if (lfoOn) {
        step = AdvanceVibratoLfoWindow(v, handle,
            (std::min)(64u, c.frameCount), c.lfoDepth, c.lfoBendRatio);
        stepVector = _mm256_set1_ps(step);
        windowRemaining = (std::min)(64u, c.frameCount);
    }
    uint32_t frame = 0u;
    while (frame < c.frameCount) {
        if (windowRemaining == 0u) {
            const uint32_t win = (std::min)(64u, c.frameCount - frame);
            step = AdvanceVibratoLfoWindow(v, handle, win, c.lfoDepth,
                                           c.lfoBendRatio);
            stepVector = _mm256_set1_ps(step);
            windowRemaining = win;
        }
        // Simulate the exact scalar envelope recurrence eight steps ahead;
        // the chunk is only taken when the stage is unchanged after all
        // eight steps and no lane can reach the sample end.
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
        const float lastPhase = phase + step * 7.0f;
        if (fastGuardExact && frame + 8u <= c.frameCount &&
            windowRemaining >= 8u &&
            futureStage == stage && lastPhase < lastSafePhase) {
            _mm_prefetch(reinterpret_cast<const char*>(
                             region +
                             static_cast<uint32_t>(lastPhase + step)),
                         _MM_HINT_T0);
            const __m256 phases = _mm256_add_ps(
                _mm256_set1_ps(phase), _mm256_mul_ps(stepVector, lane));
            const __m256i bases = _mm256_cvttps_epi32(phases);
            __m256 first, second;
            GatherSamplePairAVX2(region, bases, first, second);
            const __m256 fraction = _mm256_sub_ps(
                phases, _mm256_cvtepi32_ps(bases));
            __m256 sample = _mm256_add_ps(first,
                _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
            sample = FilterTime8AVX2(v, handle, sample);
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
            windowRemaining -= 8u;
            continue;
        }
        const uint32_t base = static_cast<uint32_t>(phase);
        if (base + 1u >= relEnd) {
            // One-shot sample ended: retire.
            v.phases[handle] = phase;
            v.currentGain[handle] = gain;
            v.envelopeStage[handle] = stage;
            v.attackSamplesRemaining[handle] = attackRemaining;
            v.decaySamplesRemaining[handle] = decayRemaining;
            return frame;
        }
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
        const float fraction = phase - static_cast<float>(base);
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[base + 1u]) *
            (1.0f / 32768.0f) - first) * fraction;
        sample = ProcessVoiceFilterSample(v, handle, sample);
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        phase += step;
        --windowRemaining;
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
    return UINT32_MAX;
}

bool RenderTransientOneShotAVX2(const RenderSpanContext& context,
                                const uint32_t* handles,
                                uint32_t handleCount) {
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    if (context.voices->rot != nullptr) return false;
    VoiceSoA& v = *context.voices;
    for (uint32_t i = 0u; i < handleCount; ++i) {
        const uint32_t h = handles[i];
        if (v.state[h] == static_cast<uint8_t>(VoiceState::Releasing) ||
            (v.envelopeStage[h] != 1u && v.envelopeStage[h] != 2u) ||
            v.releaseSamplesRemaining[h] != UINT32_MAX ||
            v.relEnd[h] < 2u ||
            v.sampleStart[h] >= context.sampleDataFrames ||
            v.relEnd[h] > context.sampleDataFrames - v.sampleStart[h] ||
            v.phaseIncs[h] < 0.0f || v.relEnd[h] > 0x1000000u) {
            return false;
        }
    }
    if (context.frameCount < 8u) {
        // Short event-fragmented spans: the 8-frame time chunk cannot fire,
        // so run the exact scalar sequence per row — no AVX2 setup, no
        // AVX↔SSE transitions.
        for (uint32_t i = 0u; i < handleCount; ++i) {
            const RenderSpanContext vc = SelectVoiceDestination(
                context, v.channel[handles[i]]);
            RenderTransientOneShotSpanScalar(
                v, handles[i], vc.sampleData, vc.sampleDataFrames,
                vc.outputLeft, vc.outputRight, vc.frameStart,
                vc.frameCount);
        }
        return true;
    }
    for (uint32_t i = 0u; i < handleCount; ++i) {
        const RenderSpanContext vc = SelectVoiceDestination(
            context, v.channel[handles[i]]);
        RenderTransientOneShotFramesAVX2(vc, handles[i]);
    }
    _mm256_zeroupper();
    return true;
}

bool RenderSustainedOneShotAVX2(const RenderSpanContext& context,
                                const uint32_t* handles,
                                uint32_t handleCount) {
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    // Rotation voices render through the rotation-hooked scalar spans.
    if (context.voices->rot != nullptr) return false;
    VoiceSoA& v = *context.voices;
    // Validate the whole batch before mutating anything: a false return
    // must leave zero state changes (kernel fallback contract).
    for (uint32_t i = 0u; i < handleCount; ++i) {
        const uint32_t h = handles[i];
        if (v.relEnd[h] < 2u ||
            v.sampleStart[h] >= context.sampleDataFrames ||
            v.relEnd[h] > context.sampleDataFrames - v.sampleStart[h] ||
            v.phaseIncs[h] < 0.0f || v.relEnd[h] > 0x1000000u) {
            return false;
        }
    }
    if (context.frameCount < 8u) {
        // Short event-fragmented spans: exact scalar per row — no AVX2
        // setup, no AVX↔SSE transitions (the per-row entry overhead would
        // exceed the render work at these lengths).
        for (uint32_t i = 0u; i < handleCount; ++i) {
            const RenderSpanContext vc = SelectVoiceDestination(
                context, v.channel[handles[i]]);
            const uint32_t retiredAt = RenderSustainedOneShotSpan(
                v, handles[i], vc.sampleData, vc.hilbertData,
                vc.sampleDataFrames, vc.outputLeft,
                vc.outputRight, vc.frameStart, vc.frameCount);
            if (retiredAt != UINT32_MAX)
                RecordRetirement(context, handles[i], retiredAt);
        }
        return true;
    }
    for (uint32_t i = 0u; i < handleCount; ++i) {
        const RenderSpanContext vc = SelectVoiceDestination(
            context, v.channel[handles[i]]);
        const uint32_t retiredAt = RenderSustainedOneShotFramesAVX2(
            vc, handles[i]);
        if (retiredAt != UINT32_MAX)
            RecordRetirement(context, handles[i], retiredAt);
    }
    _mm256_zeroupper();
    return true;
}

bool RenderReleaseOneShotAVX2(const RenderSpanContext& context,
                              const uint32_t* handles,
                              uint32_t handleCount) {
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    if (context.voices->rot != nullptr) return false;
    VoiceSoA& v = *context.voices;
    for (uint32_t i = 0u; i < handleCount; ++i) {
        const uint32_t h = handles[i];
        if (v.relEnd[h] < 2u ||
            v.sampleStart[h] >= context.sampleDataFrames ||
            v.relEnd[h] > context.sampleDataFrames - v.sampleStart[h] ||
            v.phaseIncs[h] < 0.0f || v.relEnd[h] > 0x1000000u) {
            return false;
        }
    }
    if (context.frameCount < 8u) {
        for (uint32_t i = 0u; i < handleCount; ++i) {
            const RenderSpanContext vc = SelectVoiceDestination(
                context, v.channel[handles[i]]);
            const uint32_t retiredAt = RenderReleaseOneShotSpanScalar(
                v, handles[i], vc.sampleData, vc.sampleDataFrames,
                vc.outputLeft, vc.outputRight, vc.frameStart,
                vc.frameCount);
            if (retiredAt != UINT32_MAX)
                RecordRetirement(context, handles[i], retiredAt);
        }
        return true;
    }
    for (uint32_t i = 0u; i < handleCount; ++i) {
        const RenderSpanContext vc = SelectVoiceDestination(
            context, v.channel[handles[i]]);
        const uint32_t retiredAt = RenderReleaseOneShotFramesAVX2(
            vc, handles[i]);
        if (retiredAt != UINT32_MAX)
            RecordRetirement(context, handles[i], retiredAt);
    }
    _mm256_zeroupper();
    return true;
}

const RenderKernelSet& GetAVX2RenderKernelSet() {
    static const RenderKernelSet set = [] {
        RenderKernelSet result{};
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::SustainedLoop)] =
            RenderSustainedLoopAVX2;
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::SustainedOneShot)] =
            RenderSustainedOneShotAVX2;
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::ReleaseLoop)] =
            RenderReleaseLoopAVX2;
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::ReleaseOneShot)] =
            RenderReleaseOneShotAVX2;
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::TransientLoop)] =
            RenderTransientLoopAVX2;
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::TransientOneShot)] =
            RenderTransientOneShotAVX2;
        result.backend = RenderBackend::AVX2;
        result.name = "avx2";
        return result;
    }();
    return set;
}

} // namespace svms
