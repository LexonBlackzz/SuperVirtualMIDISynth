// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.

// Render an independent stolen-voice continuation across an event-free span.
// All state is held in locals and committed once.
inline void RenderStealTailSpan(VoiceSoA& v, uint32_t idx,
                                const int16_t* sampleData,
                                const int16_t* hilbertData,
                                uint32_t sampleDataFrames,
                                float* outputLeft, float* outputRight,
                                uint32_t frameStart, uint32_t frameCount) {
    uint32_t remaining = v.stealTailFramesRemaining[idx];
    if (remaining == 0u || v.stealTailSampleBacked[idx] == 0u ||
        sampleData == nullptr || frameCount == 0u) {
        return;
    }

    const uint32_t relEnd = v.stealTailRelEnd[idx];
    if (relEnd < 2u) {
        v.stealTailFramesRemaining[idx] = 0u;
        return;
    }

    uint64_t pos = phasefx::Pack(v.stealTailPhaseIdx[idx], v.stealTailPhaseFrac[idx]);
    const uint64_t step = phasefx::FromFloat(v.stealTailPhaseInc[idx]);
    const float gain = v.stealTailGain[idx];
    const float mixL = v.stealTailMixGainL[idx];
    const float mixR = v.stealTailMixGainR[idx];
    const uint32_t sampleStart = v.stealTailSampleStart[idx];
    const uint32_t relLoopS = v.stealTailRelLoopS[idx];
    const uint32_t relLoopE = v.stealTailRelLoopE[idx];
    const uint64_t loopS64 = phasefx::Pack(relLoopS, 0u);
    const uint64_t loopE64 = phasefx::Pack(relLoopE, 0u);
    const bool loop = v.stealTailLoopEnabled[idx] != 0u;
    const uint32_t total = v.stealTailFramesTotal[idx];
    const uint32_t count = (std::min)(frameCount, remaining);

    for (uint32_t n = 0; n < count; ++n) {
        uint32_t baseOffset = phasefx::Index(pos);
        if (baseOffset + 1u >= relEnd) {
            if (!loop) {
                remaining = 0u;
                break;
            }
            pos = loopS64;
            baseOffset = relLoopS;
        }

        uint32_t nextRel = baseOffset + 1u;
        if (loop && nextRel >= relLoopE) nextRel = relLoopS;
        if (nextRel >= relEnd) nextRel = relEnd - 1u;
        const uint32_t baseIndex = sampleStart + baseOffset;
        const uint32_t nextIndex = sampleStart + nextRel;
        if (baseIndex >= sampleDataFrames || nextIndex >= sampleDataFrames) {
            remaining = 0u;
            break;
        }

        const float frac = phasefx::Weight(phasefx::Frac(pos));
        float sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
        if (v.rot)
            sample = RotateVoiceSample(v.stealTailRot[idx], sample,
                                       hilbertData, baseIndex, nextIndex,
                                       frac);
        sample = ProcessStealTailFilterSample(v, idx, sample);
        const float fade = total > 1u
            ? static_cast<float>(remaining - 1u) / static_cast<float>(total - 1u)
            : 0.0f;
        const float scaled = sample * gain * fade;
        outputLeft[frameStart + n] += scaled * mixL;
        outputRight[frameStart + n] += scaled * mixR;

        pos += step;
        if (loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
        --remaining;
    }

    v.stealTailPhaseIdx[idx] = phasefx::Index(pos);
    v.stealTailPhaseFrac[idx] = phasefx::Frac(pos);
    v.stealTailFramesRemaining[idx] = remaining;
}

// Render one primary voice across a span.  The return value is the zero-based
// frame inside the span on which the voice retires, or UINT32_MAX when it
// remains active.  Event dispatch cannot mutate voice state inside a span.
// Closed-form release advance for a MUTED (decimated) releasing voice: nothing
// is mixed, so the per-frame gain recurrence is replaced by one powf and the
// retire frame is found arithmetically. Returns the 0-based frame the voice
// retires at inside `frames` (UINT32_MAX = survives) and writes the number of
// frames consumed. Differs from the sequential loop by float rounding of the
// gain (~1e-6 relative) and, for threshold-governed tails, by at most one
// frame of retire timing; counted tails retire on the exact frame. Callers
// only use it when decay is in (0, 1) and exactSilentAdvance is off.
inline uint32_t SilentReleaseAdvance(float& gain, uint32_t& remaining,
                                     float decay, uint32_t frames,
                                     uint32_t& consumed) {
    if (remaining == 0u) {
        consumed = 1u;
        return 0u;
    }
    if (remaining != UINT32_MAX) {
        if (remaining <= frames) {
            consumed = remaining;
            gain *= powf(decay, static_cast<float>(consumed));
            remaining = 0u;
            return consumed - 1u;
        }
        consumed = frames;
        gain *= powf(decay, static_cast<float>(frames));
        remaining -= frames;
        return UINT32_MAX;
    }
    const float threshold = VoiceRetireThreshold();
    // Survivors (the overwhelming case) cost one logf + one expf: the tail
    // outlives the block exactly when gain * decay^frames is still at or
    // above the floor; only a retiring tail needs the quotient.
    const float logDecay = logf(decay);
    const float survived = gain * expf(static_cast<float>(frames) * logDecay);
    if (survived >= threshold) {
        consumed = frames;
        gain = survived;
        return UINT32_MAX;
    }
    uint32_t needed = 1u;
    if (gain * decay >= threshold) {
        const float quotient = logf(threshold / gain) / logDecay;
        needed = quotient >= 4.0e9f ? UINT32_MAX
                                    : static_cast<uint32_t>(quotient) + 1u;
    }
    if (needed > frames) needed = frames;
    consumed = needed;
    gain *= expf(static_cast<float>(needed) * logDecay);
    return needed - 1u;
}

inline uint32_t RenderPrimaryVoiceSpan(VoiceSoA& v, uint32_t idx,
                                       const int16_t* sampleData,
                                       const int16_t* hilbertData,
                                       uint32_t sampleDataFrames,
                                       float* outputLeft, float* outputRight,
                                       uint32_t frameStart, uint32_t frameCount,
                                       uint32_t mixedFrameCount,
                                       bool exactSilentAdvance = false) {
    {
        auto& shard = g_primarySpans.Mine();
        g_primarySpans.Add(shard, 0u, 1u);
        g_primarySpans.Add(shard, 1u, frameCount);
    }

    if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free) || frameCount == 0u)
        return UINT32_MAX;

    const bool sampleBacked = v.sampleBacked[idx] != 0u && sampleData != nullptr;
    uint32_t fadeRemaining = v.stealFadeInFramesRemaining[idx];
    const uint32_t fadeTotal = v.stealFadeInFramesTotal[idx];

    // Preserve the legacy behavior for synthetic voices without sample data:
    // they are silent and stationary, but an outstanding replacement fade
    // still advances.
    if (!sampleBacked) {
        const uint32_t consumed = (std::min)(fadeRemaining, frameCount);
        v.stealFadeInFramesRemaining[idx] -= consumed;
        return UINT32_MAX;
    }

    const uint32_t sampleStart = v.sampleStart[idx];
    const uint32_t relEnd = v.relEnd[idx];
    const uint32_t relLoopS = v.relLoopS[idx];
    const uint32_t relLoopE = v.relLoopE[idx];
    const uint64_t loopS64 = phasefx::Pack(relLoopS, 0u);
    const uint64_t loopE64 = phasefx::Pack(relLoopE, 0u);
    const bool loop = v.loopEnabled[idx] != 0u;
    const float phaseStep = v.phaseIncs[idx];
    const uint64_t step = phasefx::FromFloat(phaseStep);
    const float mixL = v.mixGainL[idx];
    const float mixR = v.mixGainR[idx];
    uint64_t pos = phasefx::Load(v, idx);
    float gain = v.currentGain[idx];
    const bool released = v.state[idx] == static_cast<uint8_t>(VoiceState::Releasing);
    uint8_t stage = v.envelopeStage[idx];

    if (relEnd < 2u || sampleStart >= sampleDataFrames ||
        relEnd > sampleDataFrames - sampleStart ||
        (loop && (relLoopS >= relLoopE || relLoopE > relEnd))) {
        return 0u;
    }

    // The overwhelmingly common path: a held, sustained, looping SF2 voice.
    // Only phase/fade state is written back after the span.
    if (!released && stage == 3u && loop && v.rot == nullptr) {
        // Full-quality steady state: no decimation, no replacement fade, and
        // bounds already validated above.  This is the 4K acceptance kernel.
        if (mixedFrameCount == frameCount && fadeRemaining == 0u) {
            const float gainL = gain * mixL;
            const float gainR = gain * mixR;
            float* outL = outputLeft + frameStart;
            float* outR = outputRight + frameStart;
            // Wrap-free fast chunks: the cursor is exact, so a run of frames
            // that provably stays short of the last interior position needs no
            // wrap checks.  Chunks are capped at 64 frames.
            const uint64_t guard64 = phasefx::Pack(relLoopE - 1u, 0u);
            uint32_t n = 0u;
            while (n < frameCount) {
                uint32_t run = 0u;
                if (step > 0u && pos < guard64) {
                    const uint64_t frames = (guard64 - pos + step - 1u) / step;
                    run = static_cast<uint32_t>((std::min<uint64_t>)(frames, 64u));
                    if (run > frameCount - n) run = frameCount - n;
                }
                if (run == 0u) {
                    // a long span may end with the wrap pending: carry the overflow
                    pos = phasefx::Wrap(pos, loopS64, loopE64);
                    uint32_t baseOffset = phasefx::Index(pos);
                    if (baseOffset + 1u >= relEnd) {
                        pos = loopS64;
                        baseOffset = relLoopS;
                    }
                    uint32_t nextRel = baseOffset + 1u;
                    if (nextRel >= relLoopE) nextRel = relLoopS;
                    const float frac = phasefx::Weight(phasefx::Frac(pos));
                    float sample = InterpolateSample(
                        sampleData, sampleStart + baseOffset,
                        sampleStart + nextRel, frac);
                    sample = ProcessVoiceFilterSample(v, idx, sample);
                    outL[n] += sample * gainL;
                    outR[n] += sample * gainR;
                    pos = phasefx::Wrap(pos + step, loopS64, loopE64);
                    ++n;
                } else {
                    const uint32_t endIndex = n + run;
                    for (; n < endIndex; ++n) {
                        const uint32_t baseOffset = phasefx::Index(pos);
                        const float frac = phasefx::Weight(phasefx::Frac(pos));
                        float sample = InterpolateSample(
                            sampleData, sampleStart + baseOffset,
                            sampleStart + baseOffset + 1u, frac);
                        sample = ProcessVoiceFilterSample(v, idx, sample);
                        outL[n] += sample * gainL;
                        outR[n] += sample * gainR;
                        pos += step;
                    }
                }
            }
            // Stored wrapped, like every other looping path.
            phasefx::Store(v, idx, phasefx::Wrap(pos, loopS64, loopE64));
            return UINT32_MAX;
        }

        // Decimated steady voices still advance phase without fetching sample
        // memory or touching the output buffers.
        if (mixedFrameCount == 0u && fadeRemaining == 0u) {
            // A muted/silent advance moves authoritative state without
            // fetching samples; the cursor is exact, so one add covers it.
            pos = phasefx::Wrap(pos + step * frameCount, loopS64, loopE64);
            phasefx::Store(v, idx, pos);
            return UINT32_MAX;
        }

        for (uint32_t n = 0; n < frameCount; ++n) {
            uint32_t baseOffset = phasefx::Index(pos);
            if (baseOffset + 1u >= relEnd) {
                pos = loopS64;
                baseOffset = relLoopS;
            }

            float fade = 1.0f;
            if (fadeRemaining > 0u) {
                fade = fadeTotal > 0u
                    ? static_cast<float>(fadeTotal - fadeRemaining + 1u) /
                      static_cast<float>(fadeTotal)
                    : 1.0f;
                --fadeRemaining;
            }

            if (n < mixedFrameCount) {
                uint32_t nextRel = baseOffset + 1u;
                if (nextRel >= relLoopE) nextRel = relLoopS;
                if (nextRel >= relEnd) nextRel = relEnd - 1u;
                const uint32_t baseIndex = sampleStart + baseOffset;
                const uint32_t nextIndex = sampleStart + nextRel;
                const float frac = phasefx::Weight(phasefx::Frac(pos));
                float sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);
                sample = ProcessVoiceFilterSample(v, idx, sample);
                if (v.rot)
                    sample = RotateVoiceSample(v.rot[idx], sample, hilbertData,
                                               baseIndex, nextIndex, frac);
                const float scaled = sample * gain * fade;
                outputLeft[frameStart + n] += scaled * mixL;
                outputRight[frameStart + n] += scaled * mixR;
            }

            pos = phasefx::Wrap(pos + step, loopS64, loopE64);
        }
        phasefx::Store(v, idx, pos);
        v.stealFadeInFramesRemaining[idx] = fadeRemaining;
        return UINT32_MAX;
    }

    // A silent advance moves the authoritative steal/envelope state without
    // producing samples. Releasing loops dominate chopped-note workloads, so
    // keep them out of the generic sample/envelope machine: only the phase,
    // release gain/countdown and exact retirement frame can change here.
    if (released && loop && fadeRemaining == 0u &&
        mixedFrameCount == 0u) {
        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
        const float releaseDecay = v.releaseDecay[idx];
        uint32_t retiredAt = UINT32_MAX;

        if (!exactSilentAdvance && releaseDecay > 0.0f &&
            releaseDecay < 1.0f) {
            if (phasefx::Index(pos) + 1u >= relEnd)
                pos = loopS64;
            uint32_t consumed = 0u;
            retiredAt = SilentReleaseAdvance(gain, releaseRemaining,
                                             releaseDecay, frameCount,
                                             consumed);
            pos = phasefx::Wrap(pos + step * consumed, loopS64, loopE64);
            phasefx::Store(v, idx, pos);
            v.currentGain[idx] = gain;
            v.releaseSamplesRemaining[idx] = releaseRemaining;
            return retiredAt;
        }

        for (uint32_t n = 0u; n < frameCount; ++n) {
            const uint32_t baseOffset = phasefx::Index(pos);
            if (baseOffset + 1u >= relEnd)
                pos = loopS64;

            bool releaseFinished = false;
            if (releaseRemaining == 0u) {
                releaseFinished = true;
            } else {
                gain *= releaseDecay;
                if (releaseRemaining != UINT32_MAX) {
                    --releaseRemaining;
                    releaseFinished = releaseRemaining == 0u;
                }
            }

            pos = phasefx::Wrap(pos + step, loopS64, loopE64);

            if (releaseFinished ||
                (releaseRemaining == UINT32_MAX &&
                 gain < VoiceRetireThreshold())) {
                retiredAt = n;
                break;
            }
        }

        phasefx::Store(v, idx, pos);
        v.currentGain[idx] = gain;
        v.releaseSamplesRemaining[idx] = releaseRemaining;
        return retiredAt;
    }

    if (released && !loop && fadeRemaining == 0u &&
        mixedFrameCount == 0u) {
        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
        const float releaseDecay = v.releaseDecay[idx];
        uint32_t retiredAt = UINT32_MAX;

        if (!exactSilentAdvance && releaseDecay > 0.0f &&
            releaseDecay < 1.0f && step > 0u) {
            // The sample runs out (base + 1 >= relEnd) at the first frame
            // whose phase reaches relEnd - 1; the release retires on its own
            // schedule. The earlier of the two wins.
            const uint64_t lastPos = phasefx::Pack(relEnd - 1u, 0u);
            uint32_t endFrame = UINT32_MAX;
            if (pos >= lastPos) {
                endFrame = 0u;
            } else {
                const uint64_t frames = (lastPos - pos + step - 1u) / step;
                if (frames < 4000000000ull) endFrame = static_cast<uint32_t>(frames);
            }
            uint32_t consumed = 0u;
            const uint32_t releaseAt = SilentReleaseAdvance(
                gain, releaseRemaining, releaseDecay, frameCount, consumed);
            if (endFrame < frameCount &&
                (releaseAt == UINT32_MAX || endFrame <= releaseAt)) {
                retiredAt = endFrame;
                consumed = endFrame;
            } else {
                retiredAt = releaseAt;
            }
            pos += step * consumed;
            phasefx::Store(v, idx, pos);
            v.currentGain[idx] = gain;
            v.releaseSamplesRemaining[idx] = releaseRemaining;
            return retiredAt;
        }

        for (uint32_t n = 0u; n < frameCount; ++n) {
            const uint32_t baseOffset = phasefx::Index(pos);
            if (baseOffset + 1u >= relEnd) {
                retiredAt = n;
                break;
            }

            bool releaseFinished = false;
            if (releaseRemaining == 0u) {
                releaseFinished = true;
            } else {
                gain *= releaseDecay;
                if (releaseRemaining != UINT32_MAX) {
                    --releaseRemaining;
                    releaseFinished = releaseRemaining == 0u;
                }
            }
            pos += step;

            if (releaseFinished ||
                (releaseRemaining == UINT32_MAX &&
                 gain < VoiceRetireThreshold())) {
                retiredAt = n;
                break;
            }
        }

        phasefx::Store(v, idx, pos);
        v.currentGain[idx] = gain;
        v.releaseSamplesRemaining[idx] = releaseRemaining;
        return retiredAt;
    }

    // Full-quality looping attack/decay voices.  Envelope state remains local
    // while the sample cursor follows the same validated loop as steady state.
    if (!released && loop && fadeRemaining == 0u &&
        mixedFrameCount == frameCount && (stage == 1u || stage == 2u) &&
        v.rot == nullptr) {
        uint32_t attackRemaining = v.attackSamplesRemaining[idx];
        uint32_t decayRemaining = v.decaySamplesRemaining[idx];
        const float targetGain = v.targetGain[idx];
        const float sustainLevel = v.sustainLevel[idx];
        const float attackStep = v.attackGainStep[idx];
        const float decaySlope = v.decaySlope[idx];
        float* outL = outputLeft + frameStart;
        float* outR = outputRight + frameStart;

        for (uint32_t n = 0; n < frameCount; ++n) {
            uint32_t baseOffset = phasefx::Index(pos);
            if (baseOffset + 1u >= relEnd) {
                pos = loopS64;
                baseOffset = relLoopS;
            }
            uint32_t nextRel = baseOffset + 1u;
            if (nextRel >= relLoopE) nextRel = relLoopS;
            const float frac = phasefx::Weight(phasefx::Frac(pos));
            float sample = InterpolateSample(
                sampleData, sampleStart + baseOffset, sampleStart + nextRel, frac);
            sample = ProcessVoiceFilterSample(v, idx, sample);

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

            float sampleR = sample;
            if (v.rot)
                sampleR = RotateVoiceSample(v.rot[idx], sampleR, hilbertData,
                                            sampleStart + baseOffset,
                                            sampleStart + nextRel, frac);
            const float scaled = sampleR * gain;
            outL[n] += scaled * mixL;
            outR[n] += scaled * mixR;
            pos = phasefx::Wrap(pos + step, loopS64, loopE64);
        }

        phasefx::Store(v, idx, pos);
        v.currentGain[idx] = gain;
        v.envelopeStage[idx] = stage;
        v.attackSamplesRemaining[idx] = attackRemaining;
        v.decaySamplesRemaining[idx] = decayRemaining;
        return UINT32_MAX;
    }

    // Full-quality continuous-loop release. Mode-3 key-held loops are already
    // disabled by StartRelease and therefore remain on the generic path.
    if (released && loop && fadeRemaining == 0u &&
        mixedFrameCount == frameCount && v.rot == nullptr) {
        uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
        const float releaseDecay = v.releaseDecay[idx];
        float* outL = outputLeft + frameStart;
        float* outR = outputRight + frameStart;
        uint32_t retiredAt = UINT32_MAX;

        for (uint32_t n = 0; n < frameCount; ++n) {
            uint32_t baseOffset = phasefx::Index(pos);
            if (baseOffset + 1u >= relEnd) {
                pos = loopS64;
                baseOffset = relLoopS;
            }
            uint32_t nextRel = baseOffset + 1u;
            if (nextRel >= relLoopE) nextRel = relLoopS;
            const float frac = phasefx::Weight(phasefx::Frac(pos));
            float sample = InterpolateSample(
                sampleData, sampleStart + baseOffset, sampleStart + nextRel, frac);
            sample = ProcessVoiceFilterSample(v, idx, sample);

            bool releaseFinished = false;
            if (releaseRemaining == 0u) {
                releaseFinished = true;
            } else {
                gain *= releaseDecay;
                if (releaseRemaining != UINT32_MAX) {
                    --releaseRemaining;
                    releaseFinished = releaseRemaining == 0u;
                }
            }
            float sampleR = sample;
            if (v.rot)
                sampleR = RotateVoiceSample(v.rot[idx], sampleR, hilbertData,
                                            sampleStart + baseOffset,
                                            sampleStart + nextRel, frac);
            const float scaled = sampleR * gain;
            outL[n] += scaled * mixL;
            outR[n] += scaled * mixR;

            pos = phasefx::Wrap(pos + step, loopS64, loopE64);
            if (releaseFinished ||
                (releaseRemaining == UINT32_MAX && gain < VoiceRetireThreshold())) {
                retiredAt = n;
                break;
            }
        }

        phasefx::Store(v, idx, pos);
        v.currentGain[idx] = gain;
        v.releaseSamplesRemaining[idx] = releaseRemaining;
        return retiredAt;
    }

    uint32_t delayRemaining = v.delaySamplesRemaining[idx];
    uint32_t holdRemaining = v.holdSamplesRemaining[idx];
    uint32_t attackRemaining = v.attackSamplesRemaining[idx];
    uint32_t decayRemaining = v.decaySamplesRemaining[idx];
    uint32_t releaseRemaining = v.releaseSamplesRemaining[idx];
    const float targetGain = v.targetGain[idx];
    const float sustainLevel = v.sustainLevel[idx];
    const float attackStep = v.attackGainStep[idx];
    const float decaySlope = v.decaySlope[idx];
    const float releaseDecay = v.releaseDecay[idx];
    uint32_t retiredAt = UINT32_MAX;

    for (uint32_t n = 0; n < frameCount; ++n) {
        float sample = 0.0f;
        // Fetch coordinates for the Hilbert companion (the mix block below
        // runs after the phase advance, so the fetch locals are out of scope).
        uint32_t fetchBaseIndex = 0u;
        uint32_t fetchNextIndex = 0u;
        float fetchFrac = 0.0f;
        bool sampleEnded = false;
        uint32_t baseOffset = phasefx::Index(pos);
        if (baseOffset + 1u >= relEnd) {
            if (!loop) {
                sampleEnded = true;
            } else {
                pos = loopS64;
                baseOffset = relLoopS;
            }
        }

        if (!sampleEnded && n < mixedFrameCount) {
            uint32_t nextRel = baseOffset + 1u;
            if (loop && nextRel >= relLoopE) nextRel = relLoopS;
            if (nextRel >= relEnd) nextRel = relEnd - 1u;
            const uint32_t baseIndex = sampleStart + baseOffset;
            const uint32_t nextIndex = sampleStart + nextRel;
            const float frac = phasefx::Weight(phasefx::Frac(pos));
            sample = InterpolateSample(
                sampleData, baseIndex, nextIndex, frac);
            fetchBaseIndex = baseIndex;
            fetchNextIndex = nextIndex;
            fetchFrac = frac;
        }

        bool releaseFinished = false;
        const bool sustained = !released && stage == 3u;
        if (!sampleEnded) {
            if (released) {
                if (releaseRemaining == 0u) {
                    releaseFinished = true;
                } else {
                    gain *= releaseDecay;
                    if (releaseRemaining != UINT32_MAX) {
                        --releaseRemaining;
                        releaseFinished = releaseRemaining == 0u;
                    }
                }
            } else if (!sustained) {
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
            }

            pos += step;
            if (loop) pos = phasefx::Wrap(pos, loopS64, loopE64);
        }

        float fade = 1.0f;
        if (fadeRemaining > 0u) {
            fade = fadeTotal > 0u
                ? static_cast<float>(fadeTotal - fadeRemaining + 1u) /
                  static_cast<float>(fadeTotal)
                : 1.0f;
            --fadeRemaining;
        }

        if (!sampleEnded && n < mixedFrameCount) {
            float sampleR = sample;
            if (v.rot)
                sampleR = RotateVoiceSample(v.rot[idx], sampleR, hilbertData,
                                            fetchBaseIndex, fetchNextIndex,
                                            fetchFrac);
            sampleR = ProcessVoiceFilterSample(v, idx, sampleR);
            const float scaled = sampleR * gain * fade;
            outputLeft[frameStart + n] += scaled * mixL;
            outputRight[frameStart + n] += scaled * mixR;
        }

        const bool thresholdFinished = released && releaseRemaining == UINT32_MAX &&
                                       gain < VoiceRetireThreshold();
        if (sampleEnded || releaseFinished || thresholdFinished) {
            retiredAt = n;
            break;
        }
    }

    phasefx::Store(v, idx, pos);
    v.currentGain[idx] = gain;
    v.envelopeStage[idx] = stage;
    v.delaySamplesRemaining[idx] = delayRemaining;
    v.holdSamplesRemaining[idx] = holdRemaining;
    v.attackSamplesRemaining[idx] = attackRemaining;
    v.decaySamplesRemaining[idx] = decayRemaining;
    v.releaseSamplesRemaining[idx] = releaseRemaining;
    v.stealFadeInFramesRemaining[idx] = fadeRemaining;
    return retiredAt;
}
