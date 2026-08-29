#include "SVMSRenderKernels.h"

#include <algorithm>
#include <cmath>
#include <xmmintrin.h>

namespace svms {
namespace {

float HorizontalSum(__m128 value) {
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, value);
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
}

bool RenderSustainedLoopSSE2(const RenderSpanContext& context,
                             const uint32_t* handles,
                             uint32_t handleCount) {
    VoiceSoA& v = *context.voices;
    if (context.frameCount == 0u || context.sampleData == nullptr) return true;
    // SSE2 has no gather instruction.  For 1-4 frame event spans, packing
    // four unrelated SoundFont cursors costs more than the tuned scalar
    // batch, particularly on the legacy CPUs this backend serves.  Keep the
    // backend boundary but deliberately select the faster scalar subkernel.
    if (context.frameCount <= 4u) {
        ScalarRenderSustainedLoopShortBatch(v, handles, handleCount,
            context.sampleData, context.sampleDataFrames, context.outputLeft,
            context.outputRight, context.frameStart, context.frameCount);
        return true;
    }
    if (context.frameCount > 4u) {
        for (uint32_t i = 0; i < handleCount; ++i) {
            ScalarRenderSustainedLoop(v, handles[i], context.sampleData,
                context.sampleDataFrames, context.outputLeft,
                context.outputRight, context.frameStart, context.frameCount);
        }
        return true;
    }

    __m128 accumulatedLeft[4] = {
        _mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps()};
    __m128 accumulatedRight[4] = {
        _mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps()};
    uint32_t position = 0u;
    for (; position + 4u <= handleCount; position += 4u) {
        alignas(16) float phase[4], step[4], gainL[4], gainR[4];
        alignas(16) float first[4], second[4], fraction[4];
        bool valid = true;
        for (uint32_t lane = 0; lane < 4u; ++lane) {
            const uint32_t h = handles[position + lane];
            phase[lane] = (std::max)(0.0f, v.phases[h]);
            step[lane] = v.phaseIncs[h];
            gainL[lane] = v.renderGainL[h];
            gainR[lane] = v.renderGainR[h];
            const float loopLength = v.relLoopEF[h] - v.relLoopSF[h];
            valid = valid && v.relEnd[h] >= 2u && v.relLoopS[h] < v.relLoopE[h] &&
                v.sampleStart[h] < context.sampleDataFrames && step[lane] >= 0.0f &&
                step[lane] < loopLength;
        }
        if (!valid) {
            for (uint32_t lane = 0; lane < 4u; ++lane) {
                ScalarRenderSustainedLoop(v, handles[position + lane],
                    context.sampleData, context.sampleDataFrames,
                    context.outputLeft, context.outputRight,
                    context.frameStart, context.frameCount);
            }
            continue;
        }

        for (uint32_t frame = 0; frame < context.frameCount; ++frame) {
            for (uint32_t lane = 0; lane < 4u; ++lane) {
                const uint32_t h = handles[position + lane];
                if (phase[lane] >= v.relLoopEF[h])
                    phase[lane] = v.relLoopSF[h] + (phase[lane] - v.relLoopEF[h]);
                const uint32_t base = static_cast<uint32_t>(phase[lane]);
                uint32_t next = base + 1u;
                if (next >= v.relLoopE[h]) next = v.relLoopS[h];
                const uint32_t start = v.sampleStart[h];
                first[lane] = static_cast<float>(context.sampleData[start + base]) * (1.0f / 32768.0f);
                second[lane] = static_cast<float>(context.sampleData[start + next]) * (1.0f / 32768.0f);
                fraction[lane] = phase[lane] - static_cast<float>(base);
            }
            const __m128 a = _mm_load_ps(first);
            const __m128 sample = _mm_add_ps(
                a, _mm_mul_ps(_mm_sub_ps(_mm_load_ps(second), a),
                              _mm_load_ps(fraction)));
            accumulatedLeft[frame] = _mm_add_ps(accumulatedLeft[frame],
                _mm_mul_ps(sample, _mm_load_ps(gainL)));
            accumulatedRight[frame] = _mm_add_ps(accumulatedRight[frame],
                _mm_mul_ps(sample, _mm_load_ps(gainR)));
            for (uint32_t lane = 0; lane < 4u; ++lane) phase[lane] += step[lane];
        }
        for (uint32_t lane = 0; lane < 4u; ++lane)
            v.phases[handles[position + lane]] = phase[lane];
    }
    for (; position < handleCount; ++position) {
        ScalarRenderSustainedLoop(v, handles[position], context.sampleData,
            context.sampleDataFrames, context.outputLeft, context.outputRight,
            context.frameStart, context.frameCount);
    }
    for (uint32_t frame = 0; frame < context.frameCount; ++frame) {
        context.outputLeft[context.frameStart + frame] +=
            HorizontalSum(accumulatedLeft[frame]);
        context.outputRight[context.frameStart + frame] +=
            HorizontalSum(accumulatedRight[frame]);
    }
    return true;
}

} // namespace

const RenderKernelSet& GetSSE2RenderKernelSet() {
    static const RenderKernelSet set = [] {
        RenderKernelSet result{};
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::SustainedLoop)] =
            RenderSustainedLoopSSE2;
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::TransientLoop)] =
            ScalarRenderTransientLoopClass;
        result.backend = RenderBackend::SSE2;
        result.name = "sse2";
        return result;
    }();
    return set;
}

} // namespace svms
