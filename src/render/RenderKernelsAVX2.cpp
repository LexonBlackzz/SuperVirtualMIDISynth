// AVX2 render kernels.
//
// Layout of this file (all kernels live in the anonymous namespace until the
// public entry points near the end):
//
//   1. Helpers            filter, scatter and horizontal-sum helpers
//   2. Sample gathers     int16 store gathers (see the padding contract below)
//   3. Steal tails        fading tail of a stolen voice (RenderStealTailsAVX2)
//   4. Looping voices     per-voice time-chunk kernels (8 frames per vector):
//                         sustained, release, transient; plus the
//                         voice-batched short-span kernels (1-7 frames)
//   5. Filtered groups    eight filtered voices per vector, via 8x8 transpose
//   6. Class entry points RenderSustainedLoopAVX2 / RenderReleaseLoopAVX2 /
//                         RenderTransientLoopAVX2: pick the kernel for a batch
//                         and return false (state untouched) to refuse it
//   7. Public functions   test switches and the steal-key builder
//   8. One-shot voices    non-looping voices: scalar spans, time-chunk
//                         kernels, class entry points
//
// Contracts every kernel here keeps:
//   - Envelope gains are simulated scalarly into gains[8] with the exact
//     sequential recurrence; the cursor is a 32.32 fixed-point integer.
//   - A class entry point returns false WITHOUT mutating state when it cannot
//     take the batch; the caller then renders each voice on the scalar path.
//   - Retirements go through RecordRetirement, class changes through the
//     context's classChangeHandles.
#include "render/RenderKernels.h"
#include "render/Envelope.h"
#include "render/VoiceFilter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <immintrin.h>
#include <vector>

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

// ════════════════════════════════════════════════════════════════════════
// 1. Helpers
// ════════════════════════════════════════════════════════════════════════

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

// Sums the eight lanes in a fixed pairwise order so results are deterministic.
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
// 32.32 fixed-point cursor helpers (see synth/PhaseFx.h).
//
// A voice cursor is one uint64 (index << 32 | fraction).  The interpolation
// weight is the top 24 fraction bits as a float, the same value the scalar
// path derives, so scalar and AVX2 read identical (index, weight) pairs and
// integer adds make a whole chunk's positions exact.
// ════════════════════════════════════════════════════════════════════════

// k * step for k = 0..7, ready for FxChunk, plus the scalar multiples the
// chunk guards use.
struct FxSteps8 {
    __m256i lo;      // k * step, k = 0..3 (one uint64 per lane)
    __m256i hi;      // k * step, k = 4..7
    uint64_t step;
    uint64_t step7;
    uint64_t step8;
};

inline FxSteps8 MakeFxSteps8(uint64_t step) {
    alignas(32) uint64_t multiples[8];
    for (uint32_t k = 0u; k < 8u; ++k) multiples[k] = step * k;
    FxSteps8 s;
    s.lo = _mm256_load_si256(reinterpret_cast<const __m256i*>(multiples));
    s.hi = _mm256_load_si256(reinterpret_cast<const __m256i*>(multiples + 4));
    s.step = step;
    s.step7 = multiples[7];
    s.step8 = step * 8u;
    return s;
}

// Weight (top 24 fraction bits) of eight 32-bit fractions.
inline __m256 FxWeight8(__m256i frac) {
    return _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_srli_epi32(frac, 8)),
                         _mm256_set1_ps(1.0f / 16777216.0f));
}

// Sample indices and interpolation weights of the eight frames starting at pos.
inline void FxChunk(uint64_t pos, const FxSteps8& s, __m256i& bases,
                    __m256& weight) {
    const __m256i p = _mm256_set1_epi64x(static_cast<long long>(pos));
    const __m256i a = _mm256_add_epi64(p, s.lo);
    const __m256i b = _mm256_add_epi64(p, s.hi);
    const __m256i odd = _mm256_setr_epi32(1, 3, 5, 7, 1, 3, 5, 7);
    const __m256i even = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6);
    bases = _mm256_permute2x128_si256(
        _mm256_permutevar8x32_epi32(a, odd),
        _mm256_permutevar8x32_epi32(b, odd), 0x20);
    weight = FxWeight8(_mm256_permute2x128_si256(
        _mm256_permutevar8x32_epi32(a, even),
        _mm256_permutevar8x32_epi32(b, even), 0x20));
}

// The eight scalar gains of a chunk as one vector.  They are produced by eight
// scalar stores, so a single 256-bit load of the array stalls on store
// forwarding; assembling the vector from the eight scalars does not.
SVMS_K_FORCEINLINE __m256 Gains8(const float* g) {
    return _mm256_setr_ps(g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
}

// Exact eight-step envelope ladder for a voice deep inside its attack or decay
// stage (at least nine counted steps left, so all eight steps stay in the
// stage).  It is the same sequential recurrence the general simulation runs,
// minus the stage and counter tests that cannot fire, so the gains are
// bit-identical.  Returns false (nothing written) when the counters are too
// small and the general simulation has to run.
SVMS_K_FORCEINLINE bool EnvelopeLadder8(
    uint8_t stage, float gain, uint32_t attackRemaining,
    uint32_t decayRemaining, float attackStep, float targetGain,
    float decaySlope, float sustainLevel, float* gains, float& futureGain,
    uint32_t& futureAttack, uint32_t& futureDecay) {
    if (stage == 1u && attackRemaining >= 9u) {
        float g = gain;
        for (uint32_t k = 0u; k < 8u; ++k) {
            g += attackStep;
            if (g > targetGain) g = targetGain;
            gains[k] = g;
        }
        futureGain = g;
        futureAttack = attackRemaining - 8u;
        return true;
    }
    if (stage == 2u && decayRemaining >= 9u) {
        float g = gain;
        for (uint32_t k = 0u; k < 8u; ++k) {
            g *= decaySlope;
            if (g < sustainLevel) g = sustainLevel;
            gains[k] = g;
        }
        futureGain = g;
        futureDecay = decayRemaining - 8u;
        return true;
    }
    return false;
}

// Eight independent cursors: a cursor past its loop end re-enters at the loop
// start with the same fraction (single wrap; callers keep step < loop length).
inline void FxWrap8(__m256i& idx, __m256i loopStart, __m256i loopEnd) {
    const __m256i past = _mm256_cmpgt_epi32(
        idx, _mm256_sub_epi32(loopEnd, _mm256_set1_epi32(1)));
    idx = _mm256_blendv_epi8(
        idx, _mm256_add_epi32(_mm256_sub_epi32(idx, loopEnd), loopStart), past);
}

// Eight independent cursors (voice-batched kernels): idx/frac += stepIdx/stepFrac
// with the carry out of the fraction.
inline void FxAdd8(__m256i& idx, __m256i& frac, __m256i stepIdx,
                   __m256i stepFrac) {
    frac = _mm256_add_epi32(frac, stepFrac);
    const __m256i noCarry = _mm256_cmpeq_epi32(
        _mm256_max_epu32(frac, stepFrac), frac);
    const __m256i carry = _mm256_andnot_si256(noCarry, _mm256_set1_epi32(1));
    idx = _mm256_add_epi32(_mm256_add_epi32(idx, stepIdx), carry);
}

// ════════════════════════════════════════════════════════════════════════
// 2. 16-bit sample-store gathers.
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
    const uint32_t lo = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm256_castsi256_si128(bases)));
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

// ════════════════════════════════════════════════════════════════════════
// 3. Steal tails
//
// When a voice is stolen its last samples keep playing as a short fading
// "tail" so the steal does not click.  RenderTailScalar renders one tail;
// RenderStealTailsAVX2 batches eight tails per vector when every lane is
// simple enough (looping, unfiltered, same frame count, no wrap in span) and
// falls back to the scalar tail otherwise.
// ════════════════════════════════════════════════════════════════════════

// Renders up to `frameCount` frames of one steal tail, advancing its stored
// phase and remaining-frame count.  Mixes into the context's output planes.
void RenderTailScalar(const RenderSpanContext& c, uint32_t h,
                      uint32_t frameCount) {
    VoiceSoA& v = *c.voices;
    uint32_t remaining = v.stealTailFramesRemaining[h];
    if (remaining == 0u || v.stealTailSampleBacked[h] == 0u ||
        c.sampleData == nullptr || frameCount == 0u) return;
    const uint32_t relEnd = v.stealTailRelEnd[h];
    if (relEnd < 2u) { v.stealTailFramesRemaining[h] = 0u; return; }
    uint64_t pos = phasefx::Pack(v.stealTailPhaseIdx[h], v.stealTailPhaseFrac[h]);
    const uint64_t step = phasefx::FromFloat(v.stealTailPhaseInc[h]);
    const uint32_t loopS = v.stealTailRelLoopS[h];
    const uint32_t loopE = v.stealTailRelLoopE[h];
    const uint64_t loopS64 = phasefx::Pack(loopS, 0u);
    const uint64_t loopE64 = phasefx::Pack(loopE, 0u);
    const bool loop = v.stealTailLoopEnabled[h] != 0u;
    const uint32_t total = v.stealTailFramesTotal[h];
    const uint32_t count = (std::min)(frameCount, remaining);
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t base = phasefx::Index(pos);
        if (base + 1u >= relEnd) {
            if (!loop) { remaining = 0u; break; }
            pos = loopS64;
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
        const float fraction = phasefx::Weight(phasefx::Frac(pos));
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
        pos += step;
        if (loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
        --remaining;
    }
    v.stealTailPhaseIdx[h] = phasefx::Index(pos);
    v.stealTailPhaseFrac[h] = phasefx::Frac(pos);
    v.stealTailFramesRemaining[h] = remaining;
}

// Renders the tails of `handles` for `c.frameCount` frames.  `frameCounts` is
// indexed by voice handle (not by position in `handles`).
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
        alignas(32) uint32_t stepIdxLane[8], stepFracLane[8];
        for (uint32_t lane = 0; lane < 8u; ++lane) {
            const uint32_t x = hs[lane];
            const uint64_t pos = phasefx::Pack(v.stealTailPhaseIdx[x],
                                               v.stealTailPhaseFrac[x]);
            const uint64_t step = phasefx::FromFloat(v.stealTailPhaseInc[x]);
            stepIdxLane[lane] = phasefx::Index(step);
            stepFracLane[lane] = phasefx::Frac(step);
            const uint64_t loopS64 = phasefx::Pack(v.stealTailRelLoopS[x], 0u);
            const uint64_t loopE64 = phasefx::Pack(v.stealTailRelLoopE[x], 0u);
            valid = valid && frameCounts[x] == c.frameCount &&
                v.stealTailFramesRemaining[x] >= c.frameCount &&
                v.stealTailSampleBacked[x] != 0u &&
                v.stealTailFilterEnabled[x] == 0u &&
                v.stealTailLoopEnabled[x] != 0u &&
                v.stealTailRelEnd[x] >= 2u &&
                v.stealTailRelLoopS[x] < v.stealTailRelLoopE[x] &&
                v.stealTailSampleStart[x] + v.stealTailRelEnd[x] <=
                    c.sampleDataFrames &&
                pos + step * (c.frameCount - 1u) < loopE64 &&
                step < loopE64 - loopS64;
        }
        if (!valid) {
            for (uint32_t lane = 0; lane < 8u; ++lane)
                RenderTailScalar(c, hs[lane], frameCounts[hs[lane]]);
            continue;
        }
        __m256i phaseIdx = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.stealTailPhaseIdx), h, 4);
        __m256i phaseFrac = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.stealTailPhaseFrac), h, 4);
        const __m256i stepIdxV = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(stepIdxLane));
        const __m256i stepFracV = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(stepFracLane));
        const __m256 gain = _mm256_i32gather_ps(v.stealTailGain, h, 4);
        const __m256 gainL = _mm256_mul_ps(
            gain, _mm256_i32gather_ps(v.stealTailMixGainL, h, 4));
        const __m256 gainR = _mm256_mul_ps(
            gain, _mm256_i32gather_ps(v.stealTailMixGainR, h, 4));
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
            const __m256i base = phaseIdx;
            const __m256i nextRaw = _mm256_add_epi32(base, _mm256_set1_epi32(1));
            const __m256i wraps = _mm256_cmpgt_epi32(
                nextRaw, _mm256_sub_epi32(loopE, _mm256_set1_epi32(1)));
            const __m256i next = _mm256_blendv_epi8(nextRaw, loopS, wraps);
            const __m256 first = GatherSampleAVX2(c.sampleData,
                _mm256_add_epi32(sampleStart, base));
            const __m256 second = GatherSampleAVX2(c.sampleData,
                _mm256_add_epi32(sampleStart, next));
            const __m256 fraction = FxWeight8(phaseFrac);
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
            FxAdd8(phaseIdx, phaseFrac, stepIdxV, stepFracV);
        }
        alignas(32) uint32_t idxOut[8], fracOut[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(idxOut), phaseIdx);
        _mm256_store_si256(reinterpret_cast<__m256i*>(fracOut), phaseFrac);
        for (uint32_t lane = 0; lane < 8u; ++lane) {
            const uint32_t x = hs[lane];
            const uint64_t wrapped = phasefx::Wrap(
                phasefx::Pack(idxOut[lane], fracOut[lane]),
                phasefx::Pack(v.stealTailRelLoopS[x], 0u),
                phasefx::Pack(v.stealTailRelLoopE[x], 0u));
            v.stealTailPhaseIdx[x] = phasefx::Index(wrapped);
            v.stealTailPhaseFrac[x] = phasefx::Frac(wrapped);
            v.stealTailFramesRemaining[x] -= c.frameCount;
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

// Reports that `handle` finished at `frameOffset` of this span.  Real voices
// append to the context's retirement list (applied later in exact order);
// ghost rows have no list and are simply marked Free.
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

// One sequential chunk fetch: lerp of the pair-word gather (the sample's
// neighbours are one word apart, so a single gather yields both).
SVMS_K_FORCEINLINE __m256 LerpGather8(const int16_t* region, uint32_t readable,
                                      __m256i bases, __m256 fraction) {
    __m256 first, second;
    GatherSamplePairSeqAVX2(region, readable, bases, first, second);
    return _mm256_add_ps(first,
        _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
}

// Warm the next chunk's sample lines: reads are sequential per voice, so one
// prefetch per chunk hides most of the gather's memory latency at ~0.1 cycles
// per frame.
SVMS_K_FORCEINLINE void PrefetchNext(const int16_t* region, uint64_t nextPos) {
    _mm_prefetch(reinterpret_cast<const char*>(
                     region + phasefx::Index(nextPos)),
                 _MM_HINT_T0);
}

// out += sample * gain for eight frames of both channels.
SVMS_K_FORCEINLINE void MixChunk8(float* outL, float* outR, __m256 sample,
                                  __m256 gainL, __m256 gainR) {
    _mm256_storeu_ps(outL, _mm256_add_ps(_mm256_loadu_ps(outL),
        _mm256_mul_ps(sample, gainL)));
    _mm256_storeu_ps(outR, _mm256_add_ps(_mm256_loadu_ps(outR),
        _mm256_mul_ps(sample, gainR)));
}
SVMS_K_FORCEINLINE void MixChunk8Masked(float* outL, float* outR,
                                        __m256i mask, __m256 sample,
                                        __m256 gainL, __m256 gainR) {
    _mm256_maskstore_ps(outL, mask, _mm256_add_ps(
        _mm256_maskload_ps(outL, mask), _mm256_mul_ps(sample, gainL)));
    _mm256_maskstore_ps(outR, mask, _mm256_add_ps(
        _mm256_maskload_ps(outR, mask), _mm256_mul_ps(sample, gainR)));
}

// One frame of eight looping voices, one per lane: wraps the cursors into
// their loops, fetches both lerp neighbours (the second wraps to the loop
// start) and interpolates.  The cursors are not advanced.
SVMS_K_FORCEINLINE __m256 LoopLerp8(const int16_t* data, __m256i& phaseIdx,
                                    __m256i phaseFrac, __m256i sampleStart,
                                    __m256i loopStart, __m256i loopEnd) {
    FxWrap8(phaseIdx, loopStart, loopEnd);
    const __m256i base = phaseIdx;
    const __m256i nextRaw = _mm256_add_epi32(base, _mm256_set1_epi32(1));
    const __m256i wrap = _mm256_cmpgt_epi32(
        nextRaw, _mm256_sub_epi32(loopEnd, _mm256_set1_epi32(1)));
    const __m256i next = _mm256_blendv_epi8(nextRaw, loopStart, wrap);
    const __m256 first = GatherSampleAVX2(
        data, _mm256_add_epi32(sampleStart, base));
    const __m256 second = GatherSampleAVX2(
        data, _mm256_add_epi32(sampleStart, next));
    const __m256 fraction = FxWeight8(phaseFrac);
    return _mm256_add_ps(first,
        _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
}

// Whole-voice vibrato: the 64-frame LFO control window is rebuilt whenever it
// runs out, and the fast-chunk gate keeps a chunk from straddling a window
// boundary.  remaining = UINT32_MAX means the LFO is off (scalar/SSE2 windows
// are handled by the renderer, not the kernel).
struct VibratoWindow {
    uint32_t remaining = UINT32_MAX;

    bool Begin(const RenderSpanContext& c, VoiceSoA& v, uint32_t handle,
               float& step, uint64_t& stepFx, FxSteps8& fs) {
        const bool on = c.lfoActive != 0u &&
            v.vibLfoSteps[handle] != 0.0f &&
            v.vibLfoToPitchCents[handle] > 0.0f && c.lfoDepth > 0.0f;
        if (on) {
            step = AdvanceVibratoLfoWindow(v, handle,
                (std::min)(64u, c.frameCount), c.lfoDepth, c.lfoBendRatio);
            stepFx = phasefx::FromFloat(step);
            fs = MakeFxSteps8(stepFx);
            remaining = (std::min)(64u, c.frameCount);
        }
        return on;
    }
    void Refill(const RenderSpanContext& c, VoiceSoA& v, uint32_t handle,
                uint32_t frame, float& step, uint64_t& stepFx, FxSteps8& fs) {
        if (remaining != 0u) return;
        const uint32_t win = (std::min)(64u, c.frameCount - frame);
        step = AdvanceVibratoLfoWindow(v, handle, win, c.lfoDepth,
                                       c.lfoBendRatio);
        stepFx = phasefx::FromFloat(step);
        fs = MakeFxSteps8(stepFx);
        remaining = win;
    }
};

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

// ════════════════════════════════════════════════════════════════════════
// 4. Looping voices
//
// Per-voice kernels vectorize across TIME (8 consecutive frames of one
// voice).  The *Short* kernels further down vectorize across VOICES for
// spans of 1-7 frames.
// ════════════════════════════════════════════════════════════════════════

// True when the voice's loop/sample bounds are sane enough for the vector
// kernels (loop inside the sample, sample inside the store, step below the
// loop length).  Kernels fall back to scalar when this is false.
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

// Sustained voice (constant gain), one voice for the whole span, 8 frames per
// chunk.  Chunks run only while 8 frames remain, the vibrato window covers
// them and the last read stays clear of the loop end (or of the sample end for
// a one-shot); everything else steps one frame at a time.
//   Loop:    wraps the cursor, rides rotation and the masked tail chunk, and
//            returns UINT32_MAX (sustained voices never retire here).
//   OneShot: retires at the first frame whose lerp partner would read past
//            the sample, returning that frame, with the phase not advanced.
template <bool Loop>
uint32_t RenderSustainedChunked(const RenderSpanContext& c, uint32_t handle) {
    VoiceSoA& v = *c.voices;
    ChunkRotation rot;
    if constexpr (Loop) {
        if (!ValidateLoopVoice(c, handle) ||
            !rot.Init(v, c.hilbertData, handle)) {
            return ScalarRenderSustainedLoop(
                v, handle, c.sampleData, c.hilbertData, c.sampleDataFrames,
                c.outputLeft, c.outputRight, c.frameStart, c.frameCount);
        }
    }

    const uint32_t relEnd = v.relEnd[handle];
    uint64_t pos = phasefx::Load(v, handle);
    float step = v.phaseIncs[handle];
    uint64_t stepFx = phasefx::FromFloat(step);
    const uint32_t relLoopS = Loop ? v.relLoopS[handle] : 0u;
    const uint32_t relLoopE = Loop ? v.relLoopE[handle] : 0u;
    const uint64_t loopS64 = phasefx::Pack(relLoopS, 0u);
    const uint64_t loopE64 = phasefx::Pack(relLoopE, 0u);
    // A chunk needs index(pos + 7*step) + 1 < bound for every lane, i.e.
    // pos + 7*step < (bound - 1).0
    const uint64_t guard64 = phasefx::Pack((Loop ? relLoopE : relEnd) - 1u, 0u);
    const float gainL = v.renderGainL[handle];
    const float gainR = v.renderGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    const uint32_t readable = c.sampleDataFrames - v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    FxSteps8 fs = MakeFxSteps8(stepFx);
    const __m256 gainLVector = _mm256_set1_ps(gainL);
    const __m256 gainRVector = _mm256_set1_ps(gainR);
    VibratoWindow vib;
    (void)vib.Begin(c, v, handle, step, stepFx, fs);
    uint32_t& windowRemaining = vib.remaining;
    uint32_t frame = 0u;

    while (frame < c.frameCount) {
        vib.Refill(c, v, handle, frame, step, stepFx, fs);
        if constexpr (Loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
        const uint64_t lastPos = pos + fs.step7;
        if (frame + 8u <= c.frameCount && windowRemaining >= 8u &&
            lastPos < guard64) {
            PrefetchNext(region, lastPos + stepFx);
            __m256i bases;
            __m256 fraction;
            FxChunk(pos, fs, bases, fraction);
            __m256 sample = LerpGather8(region, readable, bases, fraction);
            if (rot) sample = rot.Chunk(sample, bases, fraction);
            sample = FilterTime8AVX2(v, handle, sample);
            MixChunk8(outL + frame, outR + frame, sample, gainLVector,
                      gainRVector);
            pos += fs.step8;
            frame += 8u;
            windowRemaining -= 8u;
            continue;
        }

        if constexpr (Loop) {
            // Masked tail chunk (see RenderReleaseLoopFramesAVX2): plain
            // voices only, since filter and rotation state advance per lane.
            if (!rot && v.filterEnabled[handle] == 0u &&
                windowRemaining >= c.frameCount - frame &&
                c.frameCount - frame < 8u) {
                const uint32_t rem = c.frameCount - frame;
                const uint64_t lastLive = pos + stepFx * (rem - 1u);
                if (lastLive < guard64) {
                    const __m256i liveMask = _mm256_cmpgt_epi32(
                        _mm256_set1_epi32(static_cast<int>(rem)),
                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
                    __m256i bases;
                    __m256 fraction;
                    FxChunk(pos, fs, bases, fraction);
                    // Dead lanes may sit past the loop; keep their reads in
                    // range.
                    bases = _mm256_min_epi32(bases, _mm256_set1_epi32(
                        static_cast<int>(phasefx::Index(lastLive))));
                    const __m256 sample =
                        LerpGather8(region, readable, bases, fraction);
                    MixChunk8Masked(outL + frame, outR + frame, liveMask,
                                    sample, gainLVector, gainRVector);
                    pos += stepFx * rem;
                    frame += rem;
                    if (windowRemaining != UINT32_MAX) windowRemaining -= rem;
                    continue;
                }
            }
        }

        uint32_t base = phasefx::Index(pos);
        uint32_t next = base + 1u;
        if constexpr (Loop) {
            if (base + 1u >= relEnd) {
                pos = loopS64;
                base = relLoopS;
            }
            next = base + 1u;
            if (next >= relLoopE) next = relLoopS;
        } else if (base + 1u >= relEnd) {
            // Sample ended: the lerp partner would read past the region.
            // Retire at this frame; the phase is not advanced.
            phasefx::Store(v, handle, pos);
            return frame;
        }
        const float fraction = phasefx::Weight(phasefx::Frac(pos));
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[next]) *
            (1.0f / 32768.0f) - first) * fraction;
        if (rot) sample = rot.Scalar(sample, base, next, fraction);
        sample = ProcessVoiceFilterSample(v, handle, sample);
        outL[frame] += sample * gainL;
        outR[frame] += sample * gainR;
        pos += stepFx;
        --windowRemaining;
        ++frame;
    }
    if constexpr (Loop) {
        // The stored cursor is wrapped, like the scalar span's.
        phasefx::Store(v, handle, phasefx::Wrap(pos, loopS64, loopE64));
        rot.Store();
    } else {
        phasefx::Store(v, handle, pos);
    }
    return UINT32_MAX;
}

uint32_t RenderSustainedLoopFramesAVX2(const RenderSpanContext& c,
                                       uint32_t handle) {
    return RenderSustainedChunked<true>(c, handle);
}

// Frame-by-frame release of a looping voice (used by the short kernel when a
// lane fails validation).  Returns the retirement frame or UINT32_MAX; also
// returns 0 when the voice fails ValidateLoopVoice, without mutating it.
uint32_t RenderReleaseLoopScalar(const RenderSpanContext& c,
                                 uint32_t handle) {
    VoiceSoA& v = *c.voices;
    if (!ValidateLoopVoice(c, handle)) return 0u;
    uint64_t pos = phasefx::Load(v, handle);
    float gain = v.currentGain[handle];
    uint32_t remaining = v.releaseSamplesRemaining[handle];
    const float decay = v.releaseDecay[handle];
    const uint64_t step = phasefx::FromFloat(v.phaseIncs[handle]);
    const uint64_t loopS64 = phasefx::Pack(v.relLoopS[handle], 0u);
    const uint64_t loopE64 = phasefx::Pack(v.relLoopE[handle], 0u);
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    uint32_t retiredAt = UINT32_MAX;
    for (uint32_t frame = 0u; frame < c.frameCount; ++frame) {
        uint32_t base = phasefx::Index(pos);
        if (base + 1u >= v.relEnd[handle]) {
            pos = loopS64;
            base = v.relLoopS[handle];
        }
        uint32_t next = base + 1u;
        if (next >= v.relLoopE[handle]) next = v.relLoopS[handle];
        const float fraction = phasefx::Weight(phasefx::Frac(pos));
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
        pos = phasefx::Wrap(pos + step, loopS64, loopE64);
        if (finished ||
            (remaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
            retiredAt = frame;
            break;
        }
    }
    phasefx::Store(v, handle, pos);
    v.currentGain[handle] = gain;
    v.releaseSamplesRemaining[handle] = remaining;
    return retiredAt;
}

// Releasing voice, 8 frames per chunk.  The release gain ladder is simulated
// scalarly into gains[8]; only fetch/lerp/mix is vectorized.  Returns the
// frame the voice retired at, or UINT32_MAX if it is still alive.
//   Loop:    wraps the cursor, rides rotation, the masked tail chunk and the
//            optional closed-form gain ladder; returns 0 (untouched) when the
//            voice fails ValidateLoopVoice.
//   OneShot: retires at the first frame whose lerp partner would read past
//            the sample, before the envelope consumes that frame (mirrors the
//            generic state machine's sampleEnded branch).
//   gainTable: optional exact gain table of this voice (see
//            ReleaseGainTables8); entry f is the gain after f + 1 steps of the
//            release recurrence, so a chunk reads its eight gains instead of
//            running the serial multiply chain itself.
template <bool Loop>
uint32_t RenderReleaseChunked(const RenderSpanContext& c, uint32_t handle,
                              const float* gainTable = nullptr) {
    VoiceSoA& v = *c.voices;
    // Form-1 (diffuse) rotation never reaches here: the class kernel refuses
    // before any state is touched.
    ChunkRotation rot;
    if constexpr (Loop) {
        if (!ValidateLoopVoice(c, handle)) return 0u;
        rot.Init(v, c.hilbertData, handle);
    }
    const uint32_t relEnd = v.relEnd[handle];
    uint64_t pos = phasefx::Load(v, handle);
    float gain = v.currentGain[handle];
    uint32_t remaining = v.releaseSamplesRemaining[handle];
    const float decay = v.releaseDecay[handle];
    float step = v.phaseIncs[handle];
    uint64_t stepFx = phasefx::FromFloat(step);
    const uint32_t relLoopS = Loop ? v.relLoopS[handle] : 0u;
    const uint32_t relLoopE = Loop ? v.relLoopE[handle] : 0u;
    const uint64_t loopS64 = phasefx::Pack(relLoopS, 0u);
    const uint64_t loopE64 = phasefx::Pack(relLoopE, 0u);
    const uint64_t guard64 = phasefx::Pack((Loop ? relLoopE : relEnd) - 1u, 0u);
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    const uint32_t readable = c.sampleDataFrames - v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    FxSteps8 fs = MakeFxSteps8(stepFx);
    VibratoWindow vib;
    const bool lfoOn = vib.Begin(c, v, handle, step, stepFx, fs);
    uint32_t& windowRemaining = vib.remaining;
    // quality.fast_chunk_math: the gain ladder in closed form (loop voices).
    const bool fastMath = Loop && g_fastChunkMath && !lfoOn;
    __m256 decayLadder = _mm256_setzero_ps();
    float decay8 = 1.0f;
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
    }
    const bool tabled = gainTable != nullptr && !fastMath;
    uint32_t frame = 0u;
    uint32_t retiredAt = UINT32_MAX;
    while (frame < c.frameCount) {
        vib.Refill(c, v, handle, frame, step, stepFx, fs);
        if constexpr (Loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
        float futureGain = gain;
        alignas(32) float gains[8];
        __m256 tableGains = _mm256_setzero_ps();
        if (fastMath) {
            futureGain = gain * decay8;
        } else if (tabled) {
            tableGains = _mm256_loadu_ps(gainTable + frame);
            futureGain = gainTable[frame + 7u];
        } else {
            for (uint32_t laneIndex = 0u; laneIndex < 8u; ++laneIndex) {
                futureGain *= decay;
                gains[laneIndex] = futureGain;
            }
        }
        const bool countdownSafe = remaining == UINT32_MAX || remaining > 8u;
        const bool thresholdSafe = remaining != UINT32_MAX ||
            ((fastMath || tabled) ? futureGain : gains[7]) >=
                VoiceRetireThreshold();
        const uint64_t lastPos = pos + fs.step7;
        if (frame + 8u <= c.frameCount && windowRemaining >= 8u &&
            countdownSafe && thresholdSafe && lastPos < guard64) {
            PrefetchNext(region, lastPos + stepFx);
            __m256i bases;
            __m256 fraction;
            FxChunk(pos, fs, bases, fraction);
            __m256 sample = LerpGather8(region, readable, bases, fraction);
            sample = FilterTime8AVX2(v, handle, sample);
            if (rot) sample = rot.Chunk(sample, bases, fraction);
            const __m256 gainVector = fastMath
                ? _mm256_mul_ps(_mm256_set1_ps(gain), decayLadder)
                : (tabled ? tableGains : Gains8(gains));
            MixChunk8(outL + frame, outR + frame,
                      _mm256_mul_ps(sample, gainVector),
                      _mm256_set1_ps(mixL), _mm256_set1_ps(mixR));
            gain = futureGain;
            if (remaining != UINT32_MAX) remaining -= 8u;
            pos += fs.step8;
            frame += 8u;
            windowRemaining -= 8u;
            continue;
        }

        if constexpr (Loop) {
            // Masked tail chunk: the last 1..7 frames of a span ride the same
            // vector code (only the live lanes are committed) instead of the
            // slow per-frame path.  Spans are short when events split them,
            // so most chunk-path misses were exactly this remainder.  Same
            // guards as the full chunk, evaluated for the live lanes only.
            // Rotation and filter state advance per lane in order, so voices
            // that carry either keep the per-frame remainder.
            if (!lfoOn && !rot && v.filterEnabled[handle] == 0u &&
                c.frameCount - frame < 8u) {
                const uint32_t rem = c.frameCount - frame;
                const uint64_t lastLive = pos + stepFx * (rem - 1u);
                const float tailGain = fastMath ? gain * ladder[rem - 1u]
                    : (tabled ? gainTable[frame + rem - 1u] : gains[rem - 1u]);
                if ((remaining == UINT32_MAX || remaining > rem) &&
                    (remaining != UINT32_MAX ||
                     tailGain >= VoiceRetireThreshold()) &&
                    lastLive < guard64) {
                    const __m256i liveMask = _mm256_cmpgt_epi32(
                        _mm256_set1_epi32(static_cast<int>(rem)),
                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
                    __m256i bases;
                    __m256 fraction;
                    FxChunk(pos, fs, bases, fraction);
                    // Dead lanes may sit past the loop; keep their reads in
                    // range.
                    bases = _mm256_min_epi32(bases, _mm256_set1_epi32(
                        static_cast<int>(phasefx::Index(lastLive))));
                    __m256 sample =
                        LerpGather8(region, readable, bases, fraction);
                    sample = FilterTime8AVX2(v, handle, sample);
                    const __m256 gainVector = fastMath
                        ? _mm256_mul_ps(_mm256_set1_ps(gain), decayLadder)
                        : (tabled ? tableGains : Gains8(gains));
                    MixChunk8Masked(outL + frame, outR + frame, liveMask,
                                    _mm256_mul_ps(sample, gainVector),
                                    _mm256_set1_ps(mixL), _mm256_set1_ps(mixR));
                    gain = tailGain;
                    if (remaining != UINT32_MAX) remaining -= rem;
                    pos += stepFx * rem;
                    frame += rem;
                    continue;
                }
            }
        }

        uint32_t base = phasefx::Index(pos);
        uint32_t next = base + 1u;
        if constexpr (Loop) {
            if (base + 1u >= relEnd) {
                pos = loopS64;
                base = relLoopS;
            }
            next = base + 1u;
            if (next >= relLoopE) next = relLoopS;
        } else if (base + 1u >= relEnd) {
            // Sample ended: retire before the envelope consumes a frame.
            retiredAt = frame;
            break;
        }
        const float fraction = phasefx::Weight(phasefx::Frac(pos));
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[next]) *
            (1.0f / 32768.0f) - first) * fraction;
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
        if constexpr (Loop) pos = phasefx::Wrap(pos + stepFx, loopS64, loopE64);
        else pos += stepFx;
        --windowRemaining;
        if (finished ||
            (remaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
            retiredAt = frame;
            break;
        }
        ++frame;
    }
    phasefx::Store(v, handle, pos);
    v.currentGain[handle] = gain;
    v.releaseSamplesRemaining[handle] = remaining;
    rot.Store();
    return retiredAt;
}

uint32_t RenderReleaseLoopFramesAVX2(const RenderSpanContext& c,
                                     uint32_t handle,
                                     const float* gainTable = nullptr) {
    return RenderReleaseChunked<true>(c, handle, gainTable);
}

// Voice-batched release for spans of 1-4 frames: eight voices per vector,
// frame-outer.  A group with any invalid lane is rendered scalar instead.
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
        alignas(32) uint32_t stepIdxLane[8], stepFracLane[8];
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            const uint64_t stepFx = phasefx::FromFloat(v.phaseIncs[handle]);
            stepIdxLane[lane] = phasefx::Index(stepFx);
            stepFracLane[lane] = phasefx::Frac(stepFx);
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

        __m256i phaseIdx = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.phaseIdx), h, 4);
        __m256i phaseFrac = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.phaseFrac), h, 4);
        const __m256i stepIdxV = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(stepIdxLane));
        const __m256i stepFracV = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(stepFracLane));
        __m256 gain = _mm256_i32gather_ps(v.currentGain, h, 4);
        const __m256 decay = _mm256_i32gather_ps(v.releaseDecay, h, 4);
        const __m256 mixL = _mm256_i32gather_ps(v.mixGainL, h, 4);
        const __m256 mixR = _mm256_i32gather_ps(v.mixGainR, h, 4);
        const __m256i sampleStart = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.sampleStart), h, 4);
        const __m256i loopStart = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.relLoopS), h, 4);
        const __m256i loopEnd = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.relLoopE), h, 4);
        for (uint32_t frame = 0u; frame < c.frameCount; ++frame) {
            const __m256 sample = LoopLerp8(c.sampleData, phaseIdx, phaseFrac, sampleStart,
            loopStart, loopEnd);
            gain = _mm256_mul_ps(gain, decay);
            const __m256 scaled = _mm256_mul_ps(sample, gain);
            sumsL[frame] = _mm256_add_ps(
                sumsL[frame], _mm256_mul_ps(scaled, mixL));
            sumsR[frame] = _mm256_add_ps(
                sumsR[frame], _mm256_mul_ps(scaled, mixR));
            FxAdd8(phaseIdx, phaseFrac, stepIdxV, stepFracV);
        }
        FxWrap8(phaseIdx, loopStart, loopEnd);
        alignas(32) uint32_t idxOut[8], fracOut[8];
        alignas(32) float gains[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(idxOut), phaseIdx);
        _mm256_store_si256(reinterpret_cast<__m256i*>(fracOut), phaseFrac);
        _mm256_store_ps(gains, gain);
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            v.phaseIdx[handle] = idxOut[lane];
            v.phaseFrac[handle] = fracOut[lane];
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

// ── Class entry point: release-loop batch ───────────────────────────────
// Routes by span length: 1-4 frames voice-batched, 8+ frames grouped (filtered
// voices eight at a time), otherwise per-voice chunks.  Returns false before
// touching any state when the batch needs the scalar fallback.
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
// Transient kernel (attack/decay voices), loop and one-shot.
//
// Mirrors RenderReleaseChunked: the envelope is advanced scalarly
// eight steps ahead into gains[8] using the exact sequential scalar
// recurrence (bit-exact by construction — no decay^8 power tricks), and
// only the sample fetch / lerp / mix is vectorized across time.  Transient
// voices never retire mid-span; when attack+decay completes (stage reaches
// 3) the voice is reported through classChangeHandles instead of
// retirements, matching the scalar class kernel.  A one-shot voice instead
// retires at the first frame whose lerp partner would read past the sample
// (the frame is returned), and a loop voice that fails ValidateLoopVoice is
// left untouched.
// ════════════════════════════════════════════════════════════════════════
template <bool Loop>
uint32_t RenderTransientChunked(const RenderSpanContext& c, uint32_t handle) {
    VoiceSoA& v = *c.voices;
    ChunkRotation rot;
    if constexpr (Loop) {
        if (!ValidateLoopVoice(c, handle)) return UINT32_MAX;
        rot.Init(v, c.hilbertData, handle);
    }
    uint64_t pos = phasefx::Load(v, handle);
    float gain = v.currentGain[handle];
    uint8_t stage = v.envelopeStage[handle];
    const uint8_t initialStage = stage;
    uint32_t attackRemaining = v.attackSamplesRemaining[handle];
    uint32_t decayRemaining = v.decaySamplesRemaining[handle];
    float step = v.phaseIncs[handle];
    uint64_t stepFx = phasefx::FromFloat(step);
    const float targetGain = v.targetGain[handle];
    const float sustainLevel = v.sustainLevel[handle];
    const float attackStep = v.attackGainStep[handle];
    const float decaySlope = v.decaySlope[handle];
    const uint32_t relEnd = v.relEnd[handle];
    const uint32_t loopS = Loop ? v.relLoopS[handle] : 0u;
    const uint32_t loopE = Loop ? v.relLoopE[handle] : 0u;
    const uint64_t loopS64 = phasefx::Pack(loopS, 0u);
    const uint64_t loopE64 = phasefx::Pack(loopE, 0u);
    const uint64_t guard64 = phasefx::Pack((Loop ? loopE : relEnd) - 1u, 0u);
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const int16_t* region = c.sampleData + v.sampleStart[handle];
    const uint32_t readable = c.sampleDataFrames - v.sampleStart[handle];
    float* outL = c.outputLeft + c.frameStart;
    float* outR = c.outputRight + c.frameStart;
    FxSteps8 fs = MakeFxSteps8(stepFx);
    // Whole-voice vibrato windows (see RenderSustainedLoopFramesAVX2).
    VibratoWindow vib;
    const bool lfoOn = vib.Begin(c, v, handle, step, stepFx, fs);
    uint32_t& windowRemaining = vib.remaining;
    const bool fastMath = Loop && g_fastChunkMath && !lfoOn;
    uint32_t frame = 0u;
    uint32_t retiredAt = UINT32_MAX;
    while (frame < c.frameCount) {
        vib.Refill(c, v, handle, frame, step, stepFx, fs);
        if constexpr (Loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
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
        if (!closedForm)
            closedForm = EnvelopeLadder8(stage, gain, attackRemaining,
                decayRemaining, attackStep, targetGain, decaySlope,
                sustainLevel, gains, futureGain, futureAttack, futureDecay);
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
        // lastPos < guard proves every lane's base + 1 < relLoopE, so neither
        // the sample end (base + 1 >= relEnd) nor the loop point
        // (next >= relLoopE) can be crossed inside the chunk, and both
        // gather indices stay inside the validated region.
        const uint64_t lastPos = pos + fs.step7;
        if (frame + 8u <= c.frameCount && windowRemaining >= 8u &&
            futureStage == stage && lastPos < guard64) {
            // Warm the next chunk's sample lines: reads are sequential per
            // voice, so one prefetch per chunk hides most of the gather's
            // memory latency at ~0.1 cycles per frame.
            PrefetchNext(region, lastPos + stepFx);
            __m256i bases;
            __m256 fraction;
            FxChunk(pos, fs, bases, fraction);
            __m256 sample = LerpGather8(region, readable, bases, fraction);
            sample = FilterTime8AVX2(v, handle, sample);
            if (rot) sample = rot.Chunk(sample, bases, fraction);
            const __m256 scaled = _mm256_mul_ps(sample, Gains8(gains));
            MixChunk8(outL + frame, outR + frame, scaled, _mm256_set1_ps(mixL), _mm256_set1_ps(mixR));
            gain = futureGain;
            stage = futureStage;
            attackRemaining = futureAttack;
            decayRemaining = futureDecay;
            pos += fs.step8;
            frame += 8u;
            windowRemaining -= 8u;
            continue;
        }

        // Scalar tail: the exact per-frame sequence from the reference
        // path (RenderScalar.h full-quality looping attack/decay).
        uint32_t base = phasefx::Index(pos);
        uint32_t next = base + 1u;
        if constexpr (Loop) {
            if (base + 1u >= relEnd) {
                pos = loopS64;
                base = loopS;
            }
            next = base + 1u;
            if (next >= loopE) next = loopS;
        } else if (base + 1u >= relEnd) {
            // One-shot sample ended: retire.
            retiredAt = frame;
            break;
        }
        const float fraction = phasefx::Weight(phasefx::Frac(pos));
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
        if constexpr (Loop) pos = phasefx::Wrap(pos + stepFx, loopS64, loopE64);
        else pos += stepFx;
        --windowRemaining;
        ++frame;
    }
    phasefx::Store(v, handle, pos);
    v.currentGain[handle] = gain;
    v.envelopeStage[handle] = stage;
    v.attackSamplesRemaining[handle] = attackRemaining;
    v.decaySamplesRemaining[handle] = decayRemaining;
    rot.Store();
    if (retiredAt == UINT32_MAX && stage != initialStage &&
        c.classChangeHandles != nullptr && c.classChangeCount != nullptr) {
        c.classChangeHandles[(*c.classChangeCount)++] = handle;
    }
    return retiredAt;
}

void RenderTransientLoopFramesAVX2(const RenderSpanContext& c,
                                   uint32_t handle) {
    (void)RenderTransientChunked<true>(c, handle);
}


// Voice-batched short-span transient kernel.  Spans of 1-7 frames dominate
// event-fragmented (buzz) workloads, where per-voice time-vectorization
// cannot engage (the 8-frame chunk guard never fires).  Lanes carry eight
// voices; the per-frame envelope recurrence runs scalarly per lane
// (bit-exact by construction) and only the sample fetch / lerp / mix is
// vectorized.  Callers must have validated every handle with
// ValidateLoopVoice and the transient preconditions.
// Voice-batched transient kernel for spans of 1-7 frames (per-lane scalar
// envelope).  Completed attack/decay goes to classChangeHandles.
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
        __m256i phaseIdx = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.phaseIdx), h, 4);
        __m256i phaseFrac = _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(v.phaseFrac), h, 4);
        alignas(32) uint32_t stepIdxLane[8], stepFracLane[8];
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint64_t stepFx = phasefx::FromFloat(v.phaseIncs[hs[lane]]);
            stepIdxLane[lane] = phasefx::Index(stepFx);
            stepFracLane[lane] = phasefx::Frac(stepFx);
        }
        const __m256i stepIdxV = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(stepIdxLane));
        const __m256i stepFracV = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(stepFracLane));
        __m256 gain = _mm256_i32gather_ps(v.currentGain, h, 4);
        const __m256 mixL = _mm256_i32gather_ps(v.mixGainL, h, 4);
        const __m256 mixR = _mm256_i32gather_ps(v.mixGainR, h, 4);
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
            __m256 sample = LoopLerp8(c.sampleData, phaseIdx, phaseFrac, sampleStart,
            loopStart, loopEnd);
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
            gain = Gains8(gains);
            const __m256 scaled = _mm256_mul_ps(sample, gain);
            sumsL[frame] = _mm256_add_ps(sumsL[frame],
                _mm256_mul_ps(scaled, mixL));
            sumsR[frame] = _mm256_add_ps(sumsR[frame],
                _mm256_mul_ps(scaled, mixR));
            FxAdd8(phaseIdx, phaseFrac, stepIdxV, stepFracV);
        }
        FxWrap8(phaseIdx, loopStart, loopEnd);
        alignas(32) float finalGains[8];
        alignas(32) uint32_t idxOut[8], fracOut[8];
        _mm256_store_ps(finalGains, gain);
        _mm256_store_si256(reinterpret_cast<__m256i*>(idxOut), phaseIdx);
        _mm256_store_si256(reinterpret_cast<__m256i*>(fracOut), phaseFrac);
        for (uint32_t lane = 0u; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            const uint8_t finalStage = stageV[lane];
            v.phaseIdx[handle] = idxOut[lane];
            v.phaseFrac[handle] = fracOut[lane];
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

// ── Class entry point: transient-loop batch ─────────────────────────────
// Short spans take the voice-batched kernel, longer ones the per-voice chunk
// kernel.  Refuses (state untouched) for rotation cases it cannot handle.
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
// 5. Filtered sustained loops, eight voices per vector.
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
// Test switch (SetFilteredGroupKernelForTest): off forces the per-voice kernels.
bool g_filteredGroupsEnabled = true;

// In-place 8x8 float transpose: rows <-> columns.
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

// Exact release-gain tables for eight voices at once.  A voice's gain chain
// (gain *= decay, once per frame) is a latency-bound dependency, so one voice
// alone leaves the multiplier idle most of the time.  The chains of eight
// different voices are independent: lane k of the vector multiply is the same
// single-precision multiply the scalar recurrence does, in the same order, so
// every table entry is bit-identical to the scalar chain.  Entry f of voice k
// (table + k * stride) is the gain after f + 1 steps; stride must be at least
// ReleaseGainStride(frames).
inline uint32_t ReleaseGainStride(uint32_t frames) {
    // The chunk kernel reads eight gains ahead of the frame it is on, so the
    // table runs up to seven frames past the span.
    return ((frames + 15u) / 8u) * 8u;
}

void ReleaseGainTables8(const VoiceSoA& v, const uint32_t* hs,
                        uint32_t frames, float* table) {
    const uint32_t stride = ReleaseGainStride(frames);
    const __m256i h = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(hs));
    __m256 gain = _mm256_i32gather_ps(v.currentGain, h, 4);
    const __m256 decay = _mm256_i32gather_ps(v.releaseDecay, h, 4);
    for (uint32_t block = 0u; block < stride; block += 8u) {
        __m256 rows[8];
        for (uint32_t k = 0u; k < 8u; ++k) {
            gain = _mm256_mul_ps(gain, decay);
            rows[k] = gain;
        }
        Transpose8x8Ps(rows);  // rows[k] = eight consecutive gains of voice k
        for (uint32_t k = 0u; k < 8u; ++k)
            _mm256_storeu_ps(table + static_cast<size_t>(k) * stride + block,
                             rows[k]);
    }
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

// Renders eight eligible filtered looping voices (`h[0..7]`) together, one
// per lane, 8 frames per step.  Lanes mix in lane order, which is the handle
// order the sequential path uses.
//   Sustained: constant renderGain.
//   Release:   the envelope is the same exact sequential recurrence the
//              per-voice kernel simulates (gains[8] per lane, no powers); a
//              lane whose countdown or retire threshold is not safe for a
//              whole chunk ends the group, and every lane then finishes
//              through the per-voice kernel on the remaining sub-span (which
//              owns retirement).  retiredAt[i] is the span-relative frame
//              voice i retires on, or UINT32_MAX.
template <bool Release>
void RenderFilteredGroup(const RenderSpanContext& c, const uint32_t* h,
                         uint32_t* retiredAt) {
    VoiceSoA& v = *c.voices;
    RenderSpanContext dest[8];
    uint64_t pos[8], stepFx[8], loopS64[8], loopE64[8], guard64[8];
    FxSteps8 fsA[8];
    float gain[8], decay[8], mixL[8], mixR[8];
    uint32_t remaining[8];
    const int16_t* region[8];
    float* outL[8];
    float* outR[8];
    uint32_t frame[8];
    for (uint32_t i = 0u; i < 8u; ++i) {
        dest[i] = SelectVoiceDestination(c, v.channel[h[i]]);
        pos[i] = phasefx::Load(v, h[i]);
        stepFx[i] = phasefx::FromFloat(v.phaseIncs[h[i]]);
        fsA[i] = MakeFxSteps8(stepFx[i]);
        loopS64[i] = phasefx::Pack(v.relLoopS[h[i]], 0u);
        loopE64[i] = phasefx::Pack(v.relLoopE[h[i]], 0u);
        guard64[i] = phasefx::Pack(v.relLoopE[h[i]] - 1u, 0u);
        if constexpr (Release) {
            gain[i] = v.currentGain[h[i]];
            remaining[i] = v.releaseSamplesRemaining[h[i]];
            decay[i] = v.releaseDecay[h[i]];
            mixL[i] = v.mixGainL[h[i]];
            mixR[i] = v.mixGainR[h[i]];
            retiredAt[i] = UINT32_MAX;
        } else {
            // Sustained lanes scale by renderGain only; reuse the mix slots.
            mixL[i] = v.renderGainL[h[i]];
            mixR[i] = v.renderGainR[h[i]];
        }
        region[i] = c.sampleData + v.sampleStart[h[i]];
        outL[i] = dest[i].outputLeft + c.frameStart;
        outR[i] = dest[i].outputRight + c.frameStart;
        frame[i] = 0u;
    }
    // The eight lanes' biquads run in lockstep over a transposed chunk.  The
    // coefficients are fixed for the group; the state lives in registers
    // while the group streams and is spilled to the SoA whenever a lane has to
    // step a scalar frame (which advances the filter through the SoA) or the
    // group ends.
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
                pos[i] = phasefx::Wrap(pos[i], loopS64[i], loopE64[i]);
                if (frame[i] + 8u > c.frameCount) { done = true; break; }
                if constexpr (Release) {
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
                    if (!countdownSafe || !thresholdSafe) {
                        done = true;
                        break;
                    }
                }
                if (pos[i] + fsA[i].step7 < guard64[i]) break;
                // Straddling this lane's loop end: one scalar frame, exactly
                // as the per-voice kernel steps it (filter state via the SoA;
                // for a release the chunk was proven safe, so the voice
                // cannot retire here).
                if (!spilled) { spillZ(); spilled = true; }
                const uint32_t hh = h[i];
                uint32_t base = phasefx::Index(pos[i]);
                if (base + 1u >= v.relEnd[hh]) {
                    pos[i] = loopS64[i];
                    base = v.relLoopS[hh];
                }
                uint32_t next = base + 1u;
                if (next >= v.relLoopE[hh]) next = v.relLoopS[hh];
                const float fraction = phasefx::Weight(phasefx::Frac(pos[i]));
                const float first =
                    static_cast<float>(region[i][base]) * (1.0f / 32768.0f);
                float sample = first +
                    (static_cast<float>(region[i][next]) * (1.0f / 32768.0f) -
                     first) * fraction;
                if constexpr (Release) {
                    if (remaining[i] != 0u) {
                        gain[i] *= decay[i];
                        if (remaining[i] != UINT32_MAX) --remaining[i];
                    }
                }
                sample = ProcessVoiceFilterSample(v, hh, sample);
                if constexpr (Release) {
                    outL[i][frame[i]] += sample * gain[i] * mixL[i];
                    outR[i][frame[i]] += sample * gain[i] * mixR[i];
                } else {
                    outL[i][frame[i]] += sample * mixL[i];
                    outR[i][frame[i]] += sample * mixR[i];
                }
                pos[i] = phasefx::Wrap(pos[i] + stepFx[i], loopS64[i],
                                       loopE64[i]);
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
            PrefetchNext(region[i], pos[i] + fsA[i].step7 + stepFx[i]);
            __m256i bases;
            __m256 fraction;
            FxChunk(pos[i], fsA[i], bases, fraction);
            rows[i] = LerpGather8(region[i],
                c.sampleDataFrames - v.sampleStart[h[i]], bases, fraction);
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
            __m256 scaled = rows[i];
            if constexpr (Release)
                scaled = _mm256_mul_ps(rows[i], _mm256_load_ps(gains[i]));
            MixChunk8(outL[i] + frame[i], outR[i] + frame[i], scaled,
                      _mm256_set1_ps(mixL[i]), _mm256_set1_ps(mixR[i]));
            if constexpr (Release) {
                gain[i] = futureGain[i];
                if (remaining[i] != UINT32_MAX) remaining[i] -= 8u;
            }
            pos[i] += fsA[i].step8;
            frame[i] += 8u;
        }
    }

    for (uint32_t i = 0u; i < 8u; ++i) {
        phasefx::Store(v, h[i],
                       phasefx::Wrap(pos[i], loopS64[i], loopE64[i]));
        if constexpr (Release) {
            v.currentGain[h[i]] = gain[i];
            v.releaseSamplesRemaining[h[i]] = remaining[i];
        }
    }
    for (uint32_t i = 0u; i < 8u; ++i) {
        if (frame[i] >= c.frameCount) continue;
        RenderSpanContext rest = dest[i];
        rest.frameStart = c.frameStart + frame[i];
        rest.frameCount = c.frameCount - frame[i];
        if constexpr (Release) {
            const uint32_t at = RenderReleaseLoopFramesAVX2(rest, h[i]);
            if (at != UINT32_MAX) retiredAt[i] = frame[i] + at;
        } else {
            (void)RenderSustainedLoopFramesAVX2(rest, h[i]);
        }
    }
}

void RenderSustainedFilteredGroupAVX2(const RenderSpanContext& c,
                                      const uint32_t* h) {
    RenderFilteredGroup<false>(c, h, nullptr);
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
// Same eligibility as the sustained group; kept as a separate name so the
// release path can diverge without touching the sustained one.
inline bool FilteredReleaseGroupEligible(const RenderSpanContext& c,
                                         uint32_t h) {
    return FilteredGroupEligible(c, h);
}

// Renders eight eligible filtered releasing voices together; `retiredAt[k]`
// is the retirement frame of voice k or UINT32_MAX.
void RenderReleaseFilteredGroupAVX2(const RenderSpanContext& c,
                                    const uint32_t* h, uint32_t* retiredAt) {
    RenderFilteredGroup<true>(c, h, retiredAt);
}

// Release-loop spans of 8+ frames, no rotation: group kernel for runs of
// eight eligible filtered voices, per-voice kernel for the rest, handle order
// (and therefore retirement recording order) preserved.
void RenderReleaseFramesGrouped(const RenderSpanContext& c,
                                const uint32_t* handles, uint32_t count) {
    VoiceSoA& v = *c.voices;
    // Gain tables for the current and the previous block of eight handles.
    // A pending filtered run is shorter than eight, so it never reaches back
    // past the previous block.
    static thread_local std::vector<float> tableStore[2];
    uint32_t tableBlock[2] = {UINT32_MAX, UINT32_MAX};
    const uint32_t stride = ReleaseGainStride(c.frameCount);
    const bool tables = !g_fastChunkMath;
    uint32_t pending[8];
    uint32_t pendingPosition[8];
    uint32_t pendingCount = 0u;
    auto tableFor = [&](uint32_t position) -> const float* {
        const uint32_t block = position / 8u;
        if (tableBlock[block & 1u] != block) return nullptr;
        return tableStore[block & 1u].data() +
            static_cast<size_t>(position % 8u) * stride;
    };
    auto perVoice = [&](uint32_t handle, uint32_t position) {
        uint32_t retiredAt;
        const float* table = tableFor(position);
        if (c.channelBusLeft == nullptr) {
            retiredAt = RenderReleaseLoopFramesAVX2(c, handle, table);  // no copy
        } else {
            const RenderSpanContext vc = SelectVoiceDestination(
                c, v.channel[handle]);
            retiredAt = RenderReleaseLoopFramesAVX2(vc, handle, table);
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
            for (uint32_t k = 0u; k < pendingCount; ++k)
                perVoice(pending[k], pendingPosition[k]);
        }
        pendingCount = 0u;
    };
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t handle = handles[i];
        if (tables && i % 8u == 0u && i + 8u <= count) {
            std::vector<float>& store = tableStore[(i / 8u) & 1u];
            if (store.size() < static_cast<size_t>(8u) * stride)
                store.resize(static_cast<size_t>(8u) * stride);
            ReleaseGainTables8(v, handles + i, c.frameCount, store.data());
            tableBlock[(i / 8u) & 1u] = i / 8u;
        }
        if (g_filteredGroupsEnabled && c.frameCount >= 8u &&
            FilteredReleaseGroupEligible(c, handle)) {
            pendingPosition[pendingCount] = i;
            pending[pendingCount++] = handle;
            if (pendingCount == 8u) flush();
            continue;
        }
        flush();
        perVoice(handle, i);
    }
    flush();
}

// ════════════════════════════════════════════════════════════════════════
// 6. Sustained-loop class entry point, with the rotation-aware
//    voice-batched kernel (PhaseRotation.h).
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
        __m256i phaseIdx = denseHandles
            ? _mm256_load_si256(reinterpret_cast<const __m256i*>(v.phaseIdx + position))
            : _mm256_i32gather_epi32(reinterpret_cast<const int*>(v.phaseIdx), h, 4);
        __m256i phaseFrac = denseHandles
            ? _mm256_load_si256(reinterpret_cast<const __m256i*>(v.phaseFrac + position))
            : _mm256_i32gather_epi32(reinterpret_cast<const int*>(v.phaseFrac), h, 4);
        const __m256 step = denseHandles ? _mm256_load_ps(v.phaseIncs + position)
            : _mm256_i32gather_ps(v.phaseIncs, h, 4);
        const __m256 gainL = denseHandles ? _mm256_load_ps(v.renderGainL + position)
            : _mm256_i32gather_ps(v.renderGainL, h, 4);
        const __m256 gainR = denseHandles ? _mm256_load_ps(v.renderGainR + position)
            : _mm256_i32gather_ps(v.renderGainR, h, 4);

        alignas(32) uint32_t hs[8];
        alignas(32) float steps[8];
        alignas(32) uint32_t stepIdxLane[8], stepFracLane[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(hs), h);
        _mm256_store_ps(steps, step);
        bool valid = true;
        for (uint32_t lane = 0; lane < 8u; ++lane) {
            const uint32_t handle = hs[lane];
            const uint64_t stepFx = phasefx::FromFloat(steps[lane]);
            stepIdxLane[lane] = phasefx::Index(stepFx);
            stepFracLane[lane] = phasefx::Frac(stepFx);
            valid = valid && v.relEnd[handle] >= 2u &&
                v.relLoopS[handle] < v.relLoopE[handle] &&
                v.sampleStart[handle] < context.sampleDataFrames &&
                steps[lane] >= 0.0f &&
                steps[lane] < static_cast<float>(
                    v.relLoopE[handle] - v.relLoopS[handle]);
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
        const __m256i stepIdxV = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(stepIdxLane));
        const __m256i stepFracV = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(stepFracLane));

        for (uint32_t frame = 0; frame < context.frameCount; ++frame) {
            __m256 sample = LoopLerp8(context.sampleData, phaseIdx, phaseFrac, sampleStart,
            loopStart, loopEnd);
            sample = FilterVoices8AVX2(v, h, sample);
            accumulatedLeft[frame] = _mm256_add_ps(
                accumulatedLeft[frame], _mm256_mul_ps(sample, gainL));
            accumulatedRight[frame] = _mm256_add_ps(
                accumulatedRight[frame], _mm256_mul_ps(sample, gainR));
            FxAdd8(phaseIdx, phaseFrac, stepIdxV, stepFracV);
        }
        FxWrap8(phaseIdx, loopStart, loopEnd);
        if (denseHandles) {
            _mm256_store_si256(reinterpret_cast<__m256i*>(v.phaseIdx + position), phaseIdx);
            _mm256_store_si256(reinterpret_cast<__m256i*>(v.phaseFrac + position), phaseFrac);
        } else {
            alignas(32) uint32_t idxOut[8], fracOut[8];
            _mm256_store_si256(reinterpret_cast<__m256i*>(idxOut), phaseIdx);
            _mm256_store_si256(reinterpret_cast<__m256i*>(fracOut), phaseFrac);
            for (uint32_t lane = 0; lane < 8u; ++lane) {
                v.phaseIdx[hs[lane]] = idxOut[lane];
                v.phaseFrac[hs[lane]] = fracOut[lane];
            }
        }
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

// ════════════════════════════════════════════════════════════════════════
// 7. Public functions
// ════════════════════════════════════════════════════════════════════════

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
    uint64_t currentFrame, float gainScale, float ageScaleValue, uint64_t* outputKeys,
    uint32_t* outputHandles, uint32_t* inverseHeapPositions,
    uint32_t positionBase) {
    if (!handles || !birthFrames || !currentGains || !outputGains ||
        !activePositions || !outputKeys || !outputHandles ||
        !inverseHeapPositions || currentFrame >
            static_cast<uint64_t>(INT32_MAX)) {
        return false;
    }

    const __m256i current = _mm256_set1_epi32(
        static_cast<int32_t>(currentFrame));
    const __m256 ageScale = _mm256_set1_ps(ageScaleValue);
    const __m256 commonAge = _mm256_set1_ps(
        static_cast<float>(currentFrame) * ageScaleValue);
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
            inverseHeapPositions[handle] = positionBase + heapPosition;
        }
    }

    const float commonAgeScalar =
        static_cast<float>(currentFrame) * ageScaleValue;
    for (; position < handleCount; ++position) {
        const uint32_t handle = handles[position];
        const uint64_t rawAge = currentFrame > birthFrames[handle]
            ? currentFrame - birthFrames[handle] : 0u;
        const uint32_t age = rawAge > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(rawAge);
        const float ageUnits = static_cast<float>(age) * ageScaleValue;
        const float level = std::fabs(currentGains[handle]) *
            outputGains[handle];
        const float score = ageUnits - level * gainScale;
        outputKeys[position] = encode(
            score - commonAgeScalar, activePositions[handle]);
        outputHandles[position] = handle;
        inverseHeapPositions[handle] = positionBase + position;
    }
    _mm256_zeroupper();
    return true;
}

// ── Scalar one-shot span helpers ────────────────────────────────────────
// Exact mirrors of the generic envelope state machine's branches
// (RenderScalar.h RenderPrimaryVoiceSpan) for spans too short to
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
    uint64_t pos = phasefx::Load(v, idx);
    float gain = v.currentGain[idx];
    uint32_t remaining = v.releaseSamplesRemaining[idx];
    const float decay = v.releaseDecay[idx];
    const uint64_t step = phasefx::FromFloat(v.phaseIncs[idx]);
    const float mixL = v.mixGainL[idx];
    const float mixR = v.mixGainR[idx];
    const int16_t* region = sampleData + v.sampleStart[idx];
    float* outL = outputLeft + frameStart;
    float* outR = outputRight + frameStart;
    for (uint32_t frame = 0u; frame < frameCount; ++frame) {
        const uint32_t base = phasefx::Index(pos);
        if (base + 1u >= relEnd) {
            phasefx::Store(v, idx, pos);
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
        const float fraction = phasefx::Weight(phasefx::Frac(pos));
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[base + 1u]) *
            (1.0f / 32768.0f) - first) * fraction;
        sample = ProcessVoiceFilterSample(v, idx, sample);
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        pos += step;
        if (finished ||
            (remaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
            phasefx::Store(v, idx, pos);
            v.currentGain[idx] = gain;
            v.releaseSamplesRemaining[idx] = remaining;
            return frame;
        }
    }
    phasefx::Store(v, idx, pos);
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
    uint64_t pos = phasefx::Load(v, idx);
    float gain = v.currentGain[idx];
    uint8_t stage = v.envelopeStage[idx];
    uint32_t delayRemaining = v.delaySamplesRemaining[idx];
    uint32_t holdRemaining = v.holdSamplesRemaining[idx];
    uint32_t attackRemaining = v.attackSamplesRemaining[idx];
    uint32_t decayRemaining = v.decaySamplesRemaining[idx];
    const uint64_t step = phasefx::FromFloat(v.phaseIncs[idx]);
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
        const uint32_t base = phasefx::Index(pos);
        if (base + 1u >= relEnd) {
            // One-shot sample ended: retire (no loop to wrap to).
            phasefx::Store(v, idx, pos);
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
        const float fraction = phasefx::Weight(phasefx::Frac(pos));
        const float first = static_cast<float>(region[base]) * (1.0f / 32768.0f);
        float sample = first + (static_cast<float>(region[base + 1u]) *
            (1.0f / 32768.0f) - first) * fraction;
        sample = ProcessVoiceFilterSample(v, idx, sample);
        outL[frame] += sample * gain * mixL;
        outR[frame] += sample * gain * mixR;
        pos += step;
    }
    phasefx::Store(v, idx, pos);
    v.currentGain[idx] = gain;
    v.envelopeStage[idx] = stage;
    v.delaySamplesRemaining[idx] = delayRemaining;
    v.holdSamplesRemaining[idx] = holdRemaining;
    v.attackSamplesRemaining[idx] = attackRemaining;
    v.decaySamplesRemaining[idx] = decayRemaining;
    return UINT32_MAX;
}

// ════════════════════════════════════════════════════════════════════════
// 8. One-shot kernels (held and releasing non-looping voices).
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
    return RenderSustainedChunked<false>(c, handle);
}

uint32_t RenderReleaseOneShotFramesAVX2(const RenderSpanContext& c,
                                        uint32_t handle) {
    return RenderReleaseChunked<false>(c, handle);
}

// Transient one-shot: non-looping attack/decay voices (the decay stage of
// every piano/pluck note).
uint32_t RenderTransientOneShotFramesAVX2(const RenderSpanContext& c,
                                          uint32_t handle) {
    return RenderTransientChunked<false>(c, handle);
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
            v.phaseIncs[h] < 0.0f) {
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
            v.phaseIncs[h] < 0.0f) {
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
            v.phaseIncs[h] < 0.0f) {
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

// Steal-tail span, eight frames per step: the cursor advances in fixed point
// (exact, as in RenderStealTailSpan), the sample gather, lerp, fade
// and mix are vectors, and the voice filter (a recurrence) runs per frame on
// the vector's lanes.  A chunk is taken only when no frame of it wraps the
// loop, reaches the sample end or the loop edge, or leaves the store; anything
// else is left to the scalar span, so those frames are bit-identical to it.
uint32_t RenderStealTailSpanAVX2(VoiceSoA& v, uint32_t idx,
                                 const int16_t* sampleData,
                                 uint32_t sampleDataFrames,
                                 float* outputLeft, float* outputRight,
                                 uint32_t frameStart, uint32_t frameCount) {
    uint32_t remaining = v.stealTailFramesRemaining[idx];
    if (remaining < 8u || frameCount < 8u || sampleData == nullptr ||
        v.stealTailSampleBacked[idx] == 0u || v.rot != nullptr) {
        return 0u;
    }
    const uint32_t relEnd = v.stealTailRelEnd[idx];
    if (relEnd < 2u) return 0u;
    const float stepF = v.stealTailPhaseInc[idx];
    if (!(stepF >= 0.0f)) return 0u;
    const uint64_t stepFx = phasefx::FromFloat(stepF);
    const FxSteps8 fs = MakeFxSteps8(stepFx);
    const bool loop = v.stealTailLoopEnabled[idx] != 0u;
    const uint32_t sampleStart = v.stealTailSampleStart[idx];
    const uint64_t loopE64 = phasefx::Pack(v.stealTailRelLoopE[idx], 0u);
    const uint32_t limit = loop ? (std::min)(relEnd, v.stealTailRelLoopE[idx])
                                : relEnd;
    const uint32_t total = v.stealTailFramesTotal[idx];
    const bool filtered = v.stealTailFilterEnabled[idx] != 0u;
    uint64_t pos = phasefx::Pack(v.stealTailPhaseIdx[idx], v.stealTailPhaseFrac[idx]);
    const __m256 gain = _mm256_set1_ps(v.stealTailGain[idx]);
    const __m256 mixL = _mm256_set1_ps(v.stealTailMixGainL[idx]);
    const __m256 mixR = _mm256_set1_ps(v.stealTailMixGainR[idx]);
    const __m256 fadeDen = _mm256_set1_ps(
        total > 1u ? static_cast<float>(total - 1u) : 1.0f);
    const __m256 lane = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f,
                                       6.0f, 7.0f);
    uint32_t consumed = 0u;
    while (frameCount - consumed >= 8u && remaining >= 8u) {
        // The last lane is the largest; `after` is the cursor after the chunk.
        const uint32_t base7 = phasefx::Index(pos + fs.step7);
        const uint64_t after = pos + fs.step8;
        if (base7 + 1u >= limit || (loop && after >= loopE64) ||
            sampleStart + base7 + 1u >= sampleDataFrames) {
            break;
        }
        __m256i base;
        __m256 frac;
        FxChunk(pos, fs, base, frac);
        __m256 first, second;
        GatherSamplePairAVX2(sampleData,
            _mm256_add_epi32(base, _mm256_set1_epi32(
                static_cast<int>(sampleStart))), first, second);
        __m256 sample = _mm256_add_ps(first, _mm256_mul_ps(
            _mm256_sub_ps(second, first), frac));
        if (filtered) {
            alignas(32) float s[8];
            _mm256_store_ps(s, sample);
            for (uint32_t k = 0u; k < 8u; ++k)
                s[k] = ProcessStealTailFilterSample(v, idx, s[k]);
            sample = _mm256_load_ps(s);
        }
        const __m256 fade = total > 1u
            ? _mm256_div_ps(_mm256_sub_ps(_mm256_set1_ps(
                  static_cast<float>(remaining - 1u)), lane), fadeDen)
            : _mm256_setzero_ps();
        const __m256 scaled = _mm256_mul_ps(_mm256_mul_ps(sample, gain), fade);
        float* l = outputLeft + frameStart + consumed;
        float* r = outputRight + frameStart + consumed;
        _mm256_storeu_ps(l, _mm256_add_ps(_mm256_loadu_ps(l),
                                          _mm256_mul_ps(scaled, mixL)));
        _mm256_storeu_ps(r, _mm256_add_ps(_mm256_loadu_ps(r),
                                          _mm256_mul_ps(scaled, mixR)));
        pos = after;
        remaining -= 8u;
        consumed += 8u;
    }
    if (consumed != 0u) {
        v.stealTailPhaseIdx[idx] = phasefx::Index(pos);
        v.stealTailPhaseFrac[idx] = phasefx::Frac(pos);
        v.stealTailFramesRemaining[idx] = remaining;
    }
    _mm256_zeroupper();
    return consumed;
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
        result.stealTailSpan = RenderStealTailSpanAVX2;
        result.name = "avx2";
        return result;
    }();
    return set;
}

} // namespace svms
