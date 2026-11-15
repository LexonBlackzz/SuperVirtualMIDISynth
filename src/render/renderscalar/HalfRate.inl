// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.
// Half-rate voice bus, see render/HalfRateBus.h.

inline bool RenderScalar::EnsureHalfRateScratch(uint32_t jobCount,
                                                uint32_t numFrames,
                                                uint32_t itemCount) {
    if (jobCount == 0u) jobCount = 1u;
    const uint32_t stride =
        (halfrate::BusFrames(numFrames) + 16u + 15u) & ~15u;
    // One job's slice of the item list is at most this long.
    const uint32_t queueStride = itemCount / jobCount + 2u;
    if (wvHalfStride_ >= stride && wvHalfJobs_ >= jobCount &&
        wvHalfQueueStride_ >= queueStride)
        return true;
    const uint32_t newStride = stride > wvHalfStride_ ? stride : wvHalfStride_;
    const uint32_t newJobs = jobCount > wvHalfJobs_ ? jobCount : wvHalfJobs_;
    const uint32_t newQueue =
        queueStride > wvHalfQueueStride_ ? queueStride : wvHalfQueueStride_;
    try {
        wvHalfBus_.assign((static_cast<size_t>(newJobs) * 2u + 2u) * newStride + 16u,
                          0.0f);
        wvHalfUsed_.assign(newJobs, uint8_t(0));
        wvHalfHandles_.assign(static_cast<size_t>(newJobs) * newQueue, 0u);
        wvHalfPos_.assign(static_cast<size_t>(newJobs) * newQueue, 0u);
        wvHalfStep_.assign(static_cast<size_t>(newJobs) * newQueue, 0.0f);
        wvHalfClass_.assign(static_cast<size_t>(newJobs) * newQueue, uint8_t(0));
        wvHalfG0_.assign(static_cast<size_t>(newJobs) * newQueue, 0.0f);
        wvHalfK_.assign(static_cast<size_t>(newJobs) * newQueue, 0.0f);
        wvHalfRem_.assign(static_cast<size_t>(newJobs) * newQueue, 0u);
    } catch (...) {
        wvHalfStride_ = 0u;
        wvHalfJobs_ = 0u;
        wvHalfQueueStride_ = 0u;
        return false;
    }
    wvHalfStride_ = newStride;
    wvHalfJobs_ = newJobs;
    wvHalfQueueStride_ = newQueue;
    return true;
}

inline float* RenderScalar::HalfRatePlane(uint32_t jobIndex, bool right) {
    uintptr_t base = reinterpret_cast<uintptr_t>(wvHalfBus_.data());
    base = (base + 63u) & ~static_cast<uintptr_t>(63u);
    return reinterpret_cast<float*>(base) +
           (static_cast<size_t>(jobIndex) * 2u + (right ? 1u : 0u)) *
               wvHalfStride_;
}

// The conditions that do not depend on where the cursor sits: a carried-over
// sustained looping row with nothing scheduled this block, no filter or
// rotation, and little enough high-frequency content at its pitch.
inline bool RenderScalar::HalfRateCandidate(const VoiceSoA& v, uint32_t handle,
                                            uint32_t frameCount) const {
    if (frameCount < halfrate::kMinFrames || wvScalarVibrato_) return false;
    if (wvStartFrame_[handle] != UINT32_MAX ||
        wvReleaseFrame_[handle] != UINT32_MAX || WholeVoiceRowMuted(handle))
        return false;
    if (wvExciteHead_ != nullptr && handle < wvExciteCapacity_ &&
        wvExciteHead_[handle] != UINT32_MAX)
        return false;
    const uint8_t channelRaw = v.channel[handle];
    const uint32_t channel = channelRaw < kChannelCount ? channelRaw
                                                        : kChannelCount;
    if (wvChanOpStart_[channel] != wvChanOpStart_[channel + 1u] ||
        wvChanVibratoActive_[channel])
        return false;
    if (v.rot != nullptr || v.filterEnabled[handle] != 0u ||
        ClassifyWholeVoiceRow(v, handle) == VoiceRenderClass::Generic)
        return false;
    // The region's measured bandwidth; a voice without one uses the default
    // (0 = not eligible).
    const uint8_t code = v.halfBand[handle];
    const float band = code != 0u
        ? static_cast<float>(code) * (1.0f / 255.0f)
        : halfRateBand_.load(std::memory_order_relaxed);
    return band > 0.0f && band * v.phaseIncs[handle] <= halfrate::kBandLimit;
}

inline bool RenderScalar::TryRenderHalfRate(
    VoiceManager& voices, uint32_t handle, const int16_t* sampleData,
    uint32_t sampleDataFrames, uint32_t frameCount, uint32_t jobIndex,
    uint32_t& queued) {
    if (sampleData == nullptr || jobIndex >= wvHalfJobs_ ||
        wvHalfStride_ < halfrate::BusFrames(frameCount) ||
        queued >= wvHalfQueueStride_)
        return false;
    VoiceSoA& v = voices.v;
    if (!HalfRateCandidate(v, handle, frameCount)) return false;
    const float stepFloat = v.phaseIncs[handle];
    const uint32_t sampleStart = v.sampleStart[handle];
    const uint32_t relEnd = v.relEnd[handle];
    const uint32_t relLoopS = v.relLoopS[handle];
    const uint32_t relLoopE = v.relLoopE[handle];
    if (relEnd < 2u || sampleStart >= sampleDataFrames ||
        relEnd > sampleDataFrames - sampleStart || relLoopS >= relLoopE ||
        relLoopE > relEnd)
        return false;
    const uint64_t step = phasefx::FromFloat(stepFloat);
    if (step == 0u) return false;
    const VoiceRenderClass cls = ClassifyWholeVoiceRow(v, handle);
    const bool loop = cls == VoiceRenderClass::SustainedLoop ||
                      cls == VoiceRenderClass::TransientLoop ||
                      cls == VoiceRenderClass::ReleaseLoop;
    if (loop && relLoopS >= relLoopE) return false;
    if (kernelSet_->kernels[static_cast<uint32_t>(cls)] == nullptr) return false;
    const uint64_t loopS64 = phasefx::Pack(relLoopS, 0u);
    const uint64_t loopE64 = phasefx::Pack(relLoopE, 0u);
    const uint64_t back = step * (2u * halfrate::kLead);
    uint64_t backedUp = back;   // how far the cursor is moved back
    // The bus runs from 2*kLead frames before the block to about 2*kLead
    // after it; the voice must live through all of that unchanged.
    const uint32_t need = frameCount + 40u;
    uint64_t pos0;
    if (loop) {
        // The backed-up cursor must stay inside the loop (no earlier wrap or
        // attack region to guess) and the loop must hold the whole back-up.
        pos0 = phasefx::Wrap(phasefx::Load(v, handle), loopS64, loopE64);
        const uint64_t loopLen = loopE64 - loopS64;
        if (pos0 < loopS64 + back || back >= loopLen) {
            // A short loop, or a cursor close to the loop start: the last
            // 2*kLead frames lay inside the loop whenever the voice has been
            // playing since long before the block (loop entry plus the
            // back-up, with a margin), and the loop repeats exactly, so the
            // cursor backs up modulo the loop length.
            const uint64_t birth = v.birthFrame[handle];
            if (wvHalfBlockStart_ <= birth) return false;
            const uint64_t travelled = (wvHalfBlockStart_ - birth) * step;
            if (travelled < loopS64 + 2u * back + step * 64u) return false;
            const uint64_t shift = back % loopLen;
            backedUp = shift;
            if (pos0 < loopS64 + shift) backedUp = shift - loopLen;  // wraps below
        }
    } else {
        // One-shot: no wrap; the whole bus run must stay inside the sample.
        pos0 = phasefx::Load(v, handle);
        if (pos0 < back ||
            pos0 + step * need + phasefx::Pack(2u, 0u) >=
                phasefx::Pack(relEnd, 0u))
            return false;
    }

    // Envelope.  The kernel steps once per bus frame (two output frames), so
    // it gets the two-frame step, started from the gain 2*kLead+1 frames
    // before the block; the exact end-of-block state is written afterwards.
    const float g0 = v.currentGain[handle];
    float param = 0.0f;
    float gStart = g0;
    float paramRun = 0.0f;
    uint32_t rem = 0u;
    const double lead = 2.0 * halfrate::kLead + 1.0;
    switch (cls) {
    case VoiceRenderClass::ReleaseLoop:
    case VoiceRenderClass::ReleaseOneShot: {
        const double decay = v.releaseDecay[handle];
        rem = v.releaseSamplesRemaining[handle];
        if (!(decay > 0.0 && decay <= 1.0)) return false;
        if (rem != UINT32_MAX ? rem <= need
                              : g0 * std::pow(decay, need) <
                                    VoiceRetireThreshold() * 1.5)
            return false;
        param = static_cast<float>(decay);
        paramRun = static_cast<float>(decay * decay);
        gStart = static_cast<float>(g0 * std::pow(decay, -lead));
        break;
    }
    case VoiceRenderClass::TransientLoop:
    case VoiceRenderClass::TransientOneShot:
        if (v.envelopeStage[handle] == 1u) {
            const double stepGain = v.attackGainStep[handle];
            rem = v.attackSamplesRemaining[handle];
            if (rem <= need || g0 - stepGain * lead < 0.0 ||
                g0 + stepGain * need > v.targetGain[handle])
                return false;
            param = static_cast<float>(stepGain);
            paramRun = static_cast<float>(stepGain * 2.0);
            gStart = static_cast<float>(g0 - stepGain * lead);
        } else {
            const double slope = v.decaySlope[handle];
            rem = v.decaySamplesRemaining[handle];
            if (rem <= need || !(slope > 0.0 && slope <= 1.0)) return false;
            param = static_cast<float>(slope);
            if (g0 <= v.sustainLevel[handle] * 1.0001f) {
                paramRun = 1.0f;     // already on the floor
            } else {
                paramRun = static_cast<float>(slope * slope);
                gStart = static_cast<float>(g0 * std::pow(slope, -lead));
            }
        }
        break;
    default:
        break;
    }

    // Back the cursor up and double the step now; the kernels run once per
    // class over the whole queue at the end of the job (FlushHalfRate), which
    // lets them prefetch the next rows' state like the batched full-rate path.
    const size_t slot =
        static_cast<size_t>(jobIndex) * wvHalfQueueStride_ + queued;
    wvHalfHandles_[slot] = handle;
    wvHalfPos_[slot] = pos0;
    wvHalfStep_[slot] = stepFloat;
    wvHalfClass_[slot] = static_cast<uint8_t>(cls);
    wvHalfG0_[slot] = g0;
    wvHalfK_[slot] = param;
    wvHalfRem_[slot] = rem;
    ++queued;
    phasefx::Store(v, handle, pos0 - backedUp);   // wraps below in 2^64
    v.phaseIncs[handle] = stepFloat * 2.0f;
    SetHalfRateEnvelope(v, handle, cls, gStart, paramRun);
    return true;
}

// Writes the bus run's envelope inputs: the gain it starts from and the
// per-bus-frame step (attack), slope (decay) or decay (release).
inline void RenderScalar::SetHalfRateEnvelope(VoiceSoA& v, uint32_t handle,
                                              VoiceRenderClass cls,
                                              float gain, float param) {
    switch (cls) {
    case VoiceRenderClass::ReleaseLoop:
    case VoiceRenderClass::ReleaseOneShot:
        v.currentGain[handle] = gain;
        v.releaseDecay[handle] = param;
        break;
    case VoiceRenderClass::TransientLoop:
    case VoiceRenderClass::TransientOneShot:
        v.currentGain[handle] = gain;
        if (v.envelopeStage[handle] == 1u) v.attackGainStep[handle] = param;
        else v.decaySlope[handle] = param;
        break;
    default:
        break;
    }
}

inline void RenderScalar::FlushHalfRate(VoiceManager& voices,
                                        const int16_t* sampleData,
                                        uint32_t sampleDataFrames,
                                        uint32_t frameCount, uint32_t jobIndex,
                                        uint32_t queued,
                                        const IndexedJobMix& mix) {
    if (queued == 0u) return;
    VoiceSoA& v = voices.v;
    float* const busLeft = HalfRatePlane(jobIndex, false);
    float* const busRight = HalfRatePlane(jobIndex, true);
    const uint32_t busFrames = halfrate::BusFrames(frameCount);
    std::memset(busLeft, 0, static_cast<size_t>(wvHalfStride_) * sizeof(float));
    std::memset(busRight, 0, static_cast<size_t>(wvHalfStride_) * sizeof(float));
    SpanRetirement* retBuf = wvJobRetirements_ +
        static_cast<size_t>(jobIndex) * scratchCapacity_;
    const size_t base = static_cast<size_t>(jobIndex) * wvHalfQueueStride_;
    bool anyOnBus = false;
    uint32_t handles[256];
    uint32_t entries[256];
    for (uint32_t cls = 0u;
         cls < static_cast<uint32_t>(VoiceRenderClass::Generic); ++cls) {
        const VoiceRenderClass klass = static_cast<VoiceRenderClass>(cls);
        const bool loop = klass == VoiceRenderClass::SustainedLoop ||
                          klass == VoiceRenderClass::TransientLoop ||
                          klass == VoiceRenderClass::ReleaseLoop;
        RenderClassKernel kernel = kernelSet_->kernels[cls];
        uint32_t count = 0u;
        for (uint32_t i = 0u; i < queued; ++i) {
            if (wvHalfClass_[base + i] != cls) continue;
            entries[count] = i;
            handles[count++] = wvHalfHandles_[base + i];
            if (count < 256u && i + 1u < queued) continue;
            // `count` rows of this class are ready (chunk full, or the queue
            // is about to end; a short chunk before that waits for more).
            bool rendered = false;
            if (kernel != nullptr) {
                RenderSpanContext context{
                    &v, sampleData, nullptr, sampleDataFrames, busLeft,
                    busRight, 0u, busFrames, v.GetCapacity(), nullptr, nullptr,
                    voices.activePosition_, retBuf,
                    &wvJobRetireCounts_[jobIndex * kWvRetireCountStride],
                    0.0f, 1.0f, 0u};
                rendered = kernel(context, handles, count);
            }
            FinishHalfRateChunk(voices, klass, loop, rendered, handles, entries,
                                count, base, sampleData, sampleDataFrames,
                                frameCount, jobIndex, mix);
            if (rendered) {
                anyOnBus = true;
                halfRateRows_.fetch_add(count, std::memory_order_relaxed);
                halfRateClassRows_[cls].fetch_add(count, std::memory_order_relaxed);
                halfRateFrames_.fetch_add(
                    static_cast<uint64_t>(count) * frameCount,
                    std::memory_order_relaxed);
            }
            count = 0u;
        }
        if (count != 0u) {   // the queue ended on a row of another class
            bool rendered = false;
            if (kernel != nullptr) {
                RenderSpanContext context{
                    &v, sampleData, nullptr, sampleDataFrames, busLeft,
                    busRight, 0u, busFrames, v.GetCapacity(), nullptr, nullptr,
                    voices.activePosition_, retBuf,
                    &wvJobRetireCounts_[jobIndex * kWvRetireCountStride],
                    0.0f, 1.0f, 0u};
                rendered = kernel(context, handles, count);
            }
            FinishHalfRateChunk(voices, klass, loop, rendered, handles, entries,
                                count, base, sampleData, sampleDataFrames,
                                frameCount, jobIndex, mix);
            if (rendered) {
                anyOnBus = true;
                halfRateRows_.fetch_add(count, std::memory_order_relaxed);
                halfRateClassRows_[cls].fetch_add(count, std::memory_order_relaxed);
                halfRateFrames_.fetch_add(
                    static_cast<uint64_t>(count) * frameCount,
                    std::memory_order_relaxed);
            }
        }
    }
    if (anyOnBus) wvHalfUsed_[jobIndex] = 1u;
}

// After a bus kernel call over `count` rows of one class: write each row's
// exact end-of-block cursor and envelope (closed form), or, when the kernel
// refused the chunk without touching state, put the row back and render it at
// full rate.
inline void RenderScalar::FinishHalfRateChunk(
    VoiceManager& voices, VoiceRenderClass klass, bool loop, bool rendered,
    const uint32_t* handles, const uint32_t* entries, uint32_t count,
    size_t base, const int16_t* sampleData, uint32_t sampleDataFrames,
    uint32_t frameCount, uint32_t jobIndex, const IndexedJobMix& mix) {
    VoiceSoA& v = voices.v;
    const double n = frameCount;
    for (uint32_t e = 0u; e < count; ++e) {
        const size_t slot = base + entries[e];
        const uint32_t handle = handles[e];
        const uint64_t pos0 = wvHalfPos_[slot];
        const float stepFloat = wvHalfStep_[slot];
        const float g0 = wvHalfG0_[slot];
        const float param = wvHalfK_[slot];
        const uint32_t rem = wvHalfRem_[slot];
        v.phaseIncs[handle] = stepFloat;
        const bool release = klass == VoiceRenderClass::ReleaseLoop ||
                             klass == VoiceRenderClass::ReleaseOneShot;
        const bool transient = klass == VoiceRenderClass::TransientLoop ||
                               klass == VoiceRenderClass::TransientOneShot;
        const bool attack = transient && v.envelopeStage[handle] == 1u;
        if (rendered) {
            uint64_t end = pos0 + phasefx::FromFloat(stepFloat) *
                                      static_cast<uint64_t>(frameCount);
            if (loop)
                end = phasefx::Wrap(end,
                                    phasefx::Pack(v.relLoopS[handle], 0u),
                                    phasefx::Pack(v.relLoopE[handle], 0u));
            phasefx::Store(v, handle, end);
            if (release) {
                v.currentGain[handle] = static_cast<float>(
                    g0 * std::pow(static_cast<double>(param), n));
                v.releaseDecay[handle] = param;
                if (rem != UINT32_MAX)
                    v.releaseSamplesRemaining[handle] = rem - frameCount;
            } else if (attack) {
                v.attackGainStep[handle] = param;
                v.currentGain[handle] = static_cast<float>(
                    g0 + static_cast<double>(param) * n);
                v.attackSamplesRemaining[handle] = rem - frameCount;
            } else if (transient) {
                v.decaySlope[handle] = param;
                const float floorGain = v.sustainLevel[handle];
                const float g = static_cast<float>(
                    g0 * std::pow(static_cast<double>(param), n));
                v.currentGain[handle] = g < floorGain ? floorGain : g;
                v.decaySamplesRemaining[handle] = rem - frameCount;
            }
        } else {
            phasefx::Store(v, handle, pos0);
            if (release || transient) v.currentGain[handle] = g0;
            if (release) v.releaseDecay[handle] = param;
            else if (attack) v.attackGainStep[handle] = param;
            else if (transient) v.decaySlope[handle] = param;
            RenderWholeVoiceRow(voices, handle, sampleData, sampleDataFrames,
                                mix.outputLeft, mix.outputRight, frameCount,
                                jobIndex);
        }
    }
}

// After the jobs: sum the jobs' half-rate buses and reconstruct once.
inline void RenderScalar::FinishHalfRate(uint32_t jobCount, uint32_t frameCount,
                                         float* outL, float* outR) {
    uint32_t first = UINT32_MAX;
    const uint32_t busFrames = halfrate::BusFrames(frameCount);
    for (uint32_t job = 0u; job < jobCount && job < wvHalfJobs_; ++job) {
        if (wvHalfUsed_[job] == 0u) continue;
        if (first == UINT32_MAX) {
            first = job;
            continue;
        }
        float* dstL = HalfRatePlane(first, false);
        float* dstR = HalfRatePlane(first, true);
        const float* srcL = HalfRatePlane(job, false);
        const float* srcR = HalfRatePlane(job, true);
        for (uint32_t i = 0u; i < busFrames; ++i) {
            dstL[i] += srcL[i];
            dstR[i] += srcR[i];
        }
    }
    if (first == UINT32_MAX) return;
    halfrate::UpsampleAdd(HalfRatePlane(first, false),
                          HalfRatePlane(first, true), outL, outR, frameCount,
                          HalfRatePlane(wvHalfJobs_, false),
                          HalfRatePlane(wvHalfJobs_, true));
}
