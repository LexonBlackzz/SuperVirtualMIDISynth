#include <algorithm>
#include <cmath>
#include "SVMSRenderKernels.h"

namespace svms {

namespace {

uint32_t RenderSustainedLoopSpan(
    VoiceSoA& v, uint32_t idx, const float* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount) {
    const uint32_t sampleStart = v.sampleStart[idx];
    const uint32_t relEnd = v.relEnd[idx];
    const uint32_t relLoopS = v.relLoopS[idx];
    const uint32_t relLoopE = v.relLoopE[idx];
    const float relLoopSF = v.relLoopSF[idx];
    const float relLoopEF = v.relLoopEF[idx];
    if (sampleData == nullptr || relEnd < 2u || sampleStart >= sampleDataFrames ||
        relEnd > sampleDataFrames - sampleStart || relLoopS >= relLoopE ||
        relLoopE > relEnd) {
        return 0u;
    }

    float phase = (std::max)(0.0f, v.phases[idx]);
    const float phaseStep = v.phaseIncs[idx];
    const float gainL = v.renderGainL[idx];
    const float gainR = v.renderGainR[idx];
    const float loopLength = relLoopEF - relLoopSF;
    const float* region = sampleData + sampleStart;
    float* outL = outputLeft + frameStart;
    float* outR = outputRight + frameStart;

    uint32_t n = 0u;
    if (frameCount >= 8u && phaseStep > 0.0f && phaseStep < loopLength) {
        const float lastInteriorPhase = relLoopEF - 1.0f;
        while (n < frameCount) {
            if (phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                if (overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }
            if (phase < lastInteriorPhase) {
                const float distance = lastInteriorPhase - phase;
                uint32_t run = static_cast<uint32_t>(ceilf(distance / phaseStep));
                if (run == 0u) run = 1u;
                run = (std::min)(run, frameCount - n);
                const uint32_t runEnd = n + run;
                for (; n < runEnd; ++n) {
                    const uint32_t baseOffset = static_cast<uint32_t>(phase);
                    const float fraction = phase - static_cast<float>(baseOffset);
                    const float first = region[baseOffset];
                    const float sample =
                        first + (region[baseOffset + 1u] - first) * fraction;
                    outL[n] += sample * gainL;
                    outR[n] += sample * gainR;
                    phase += phaseStep;
                }
                continue;
            }

            uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset >= relLoopE) baseOffset = relLoopE - 1u;
            const float fraction = phase - static_cast<float>(baseOffset);
            const float first = region[baseOffset];
            const float sample = first + (region[relLoopS] - first) * fraction;
            outL[n] += sample * gainL;
            outR[n] += sample * gainR;
            ++n;
            phase += phaseStep;
        }
    } else {
        for (; n < frameCount; ++n) {
            uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset + 1u >= relEnd) {
                phase = relLoopSF;
                baseOffset = relLoopS;
            }
            uint32_t nextOffset = baseOffset + 1u;
            if (nextOffset >= relLoopE) nextOffset = relLoopS;
            const float fraction = phase - static_cast<float>(baseOffset);
            const float first = region[baseOffset];
            const float sample = first + (region[nextOffset] - first) * fraction;
            outL[n] += sample * gainL;
            outR[n] += sample * gainR;
            phase += phaseStep;
            if (phase >= relLoopEF) {
                float overflow = phase - relLoopEF;
                if (overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = relLoopSF + overflow;
            }
        }
    }
    v.phases[idx] = phase;
    return UINT32_MAX;
}

uint32_t RenderSustainedOneShotSpan(
    VoiceSoA& v, uint32_t idx, const float* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount) {
    const uint32_t sampleStart = v.sampleStart[idx];
    const uint32_t relEnd = v.relEnd[idx];
    if (sampleData == nullptr || relEnd < 2u || sampleStart >= sampleDataFrames ||
        relEnd > sampleDataFrames - sampleStart) {
        return 0u;
    }
    float phase = (std::max)(0.0f, v.phases[idx]);
    const float phaseStep = v.phaseIncs[idx];
    const float gainL = v.renderGainL[idx];
    const float gainR = v.renderGainR[idx];
    const float* region = sampleData + sampleStart;
    float* outL = outputLeft + frameStart;
    float* outR = outputRight + frameStart;
    uint32_t retiredAt = UINT32_MAX;

    for (uint32_t n = 0; n < frameCount; ++n) {
        const uint32_t baseOffset = static_cast<uint32_t>(phase);
        if (baseOffset + 1u >= relEnd) {
            retiredAt = n;
            break;
        }
        const float fraction = phase - static_cast<float>(baseOffset);
        const float first = region[baseOffset];
        const float sample = first + (region[baseOffset + 1u] - first) * fraction;
        outL[n] += sample * gainL;
        outR[n] += sample * gainR;
        phase += phaseStep;
    }
    v.phases[idx] = phase;
    return retiredAt;
}

template <uint32_t FrameCount>
void RenderSustainedLoopShortBatchFixed(
    VoiceSoA& v, const uint32_t* handles, uint32_t handleCount,
    const float* sampleData, float* outputLeft, float* outputRight,
    uint32_t frameStart) {
    float sumsLeft[FrameCount]{};
    float sumsRight[FrameCount]{};

    for (uint32_t position = 0; position < handleCount; ++position) {
        const uint32_t idx = handles[position];
        float phase = (std::max)(0.0f, v.phases[idx]);
        const float phaseStep = v.phaseIncs[idx];
        const uint32_t loopStartOffset = v.relLoopS[idx];
        const uint32_t loopEndOffset = v.relLoopE[idx];
        const float loopStart = v.relLoopSF[idx];
        const float loopEnd = v.relLoopEF[idx];
        const float loopLength = loopEnd - loopStart;
        const float gainLeft = v.renderGainL[idx];
        const float gainRight = v.renderGainR[idx];
        const float* region = sampleData + v.sampleStart[idx];

        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            const uint32_t baseOffset = static_cast<uint32_t>(phase);
            uint32_t nextOffset = baseOffset + 1u;
            if (nextOffset >= loopEndOffset) nextOffset = loopStartOffset;
            const float fraction = phase - static_cast<float>(baseOffset);
            const float first = region[baseOffset];
            const float sample = first + (region[nextOffset] - first) * fraction;
            sumsLeft[frame] += sample * gainLeft;
            sumsRight[frame] += sample * gainRight;
            phase += phaseStep;
            if (phase >= loopEnd) {
                float overflow = phase - loopEnd;
                if (overflow >= loopLength)
                    overflow -= floorf(overflow / loopLength) * loopLength;
                phase = loopStart + overflow;
            }
        }
        v.phases[idx] = phase;
    }

    for (uint32_t frame = 0; frame < FrameCount; ++frame) {
        outputLeft[frameStart + frame] += sumsLeft[frame];
        outputRight[frameStart + frame] += sumsRight[frame];
    }
}

void RenderSustainedLoopShortBatch(
    VoiceSoA& v, const uint32_t* handles, uint32_t handleCount,
    const float* sampleData, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount) {
    switch (frameCount) {
        case 1u:
            RenderSustainedLoopShortBatchFixed<1u>(
                v, handles, handleCount, sampleData, outputLeft, outputRight,
                frameStart);
            break;
        case 2u:
            RenderSustainedLoopShortBatchFixed<2u>(
                v, handles, handleCount, sampleData, outputLeft, outputRight,
                frameStart);
            break;
        case 3u:
            RenderSustainedLoopShortBatchFixed<3u>(
                v, handles, handleCount, sampleData, outputLeft, outputRight,
                frameStart);
            break;
        case 4u:
            RenderSustainedLoopShortBatchFixed<4u>(
                v, handles, handleCount, sampleData, outputLeft, outputRight,
                frameStart);
            break;
        default:
            break;
    }
}

void RenderSustainedLoopClassKernel(const RenderSpanContext& context,
                                    const uint32_t* handles,
                                    uint32_t handleCount) {
    VoiceSoA& voices = *context.voices;
    if (context.frameCount <= 4u) {
        RenderSustainedLoopShortBatch(
            voices, handles, handleCount, context.sampleData,
            context.outputLeft, context.outputRight, context.frameStart,
            context.frameCount);
        return;
    }
    for (uint32_t position = 0; position < handleCount; ++position) {
        RenderSustainedLoopSpan(
            voices, handles[position], context.sampleData,
            context.sampleDataFrames, context.outputLeft, context.outputRight,
            context.frameStart, context.frameCount);
    }
}

} // namespace

const RenderKernelSet& GetScalarRenderKernelSet() {
    static const RenderKernelSet kernelSet = [] {
        RenderKernelSet result{};
        result.kernels[static_cast<uint32_t>(VoiceRenderClass::SustainedLoop)] =
            RenderSustainedLoopClassKernel;
        result.backend = RenderBackend::Scalar;
        result.name = "scalar";
        return result;
    }();
    return kernelSet;
}

uint32_t ScalarRenderSustainedLoop(
    VoiceSoA& voices, uint32_t handle, const float* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount) {
    return RenderSustainedLoopSpan(
        voices, handle, sampleData, sampleDataFrames, outputLeft, outputRight,
        frameStart, frameCount);
}

uint32_t ScalarRenderSustainedOneShot(
    VoiceSoA& voices, uint32_t handle, const float* sampleData,
    uint32_t sampleDataFrames, float* outputLeft, float* outputRight,
    uint32_t frameStart, uint32_t frameCount) {
    return RenderSustainedOneShotSpan(
        voices, handle, sampleData, sampleDataFrames, outputLeft, outputRight,
        frameStart, frameCount);
}

void ScalarRenderSustainedLoopShortBatch(
    VoiceSoA& voices, const uint32_t* handles, uint32_t handleCount,
    const float* sampleData, uint32_t sampleDataFrames, float* outputLeft,
    float* outputRight, uint32_t frameStart, uint32_t frameCount) {
    (void)sampleDataFrames;
    RenderSustainedLoopShortBatch(
        voices, handles, handleCount, sampleData, outputLeft,
        outputRight, frameStart, frameCount);
}

} // namespace svms
