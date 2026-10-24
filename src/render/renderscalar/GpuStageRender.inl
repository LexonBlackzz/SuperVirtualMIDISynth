// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.
//
// GPU render stage glue (see render/GpuStage.h).  A block that qualifies has
// its eligible rows moved to the tail of wvSliceHandles_ -- [cpu][sustained]
// [envelope] -- and rendered by the GPU while the worker pool renders
// everything else.  Sustained-loop rows run the kernel of the scalar fast
// branch; every other plain whole-block row (release tails, attack/decay,
// one-shots) runs the scalar generic span machine, retire frames included.
// Rows are eligible only when the plan left them a plain whole-block segment,
// so event timing, release frames, ops and the retirement order never involve
// the GPU; a late or failed GPU falls back to the exact CPU kernels with the
// SoA still holding the pre-block state.

#if defined(SVMS_GPU_STAGE)

struct RenderScalar::GpuPassContext {
    RenderScalar* renderer;
    VoiceManager* voices;
    GpuStage* stage;
    uint32_t pass;          // 1 classify, 2 pack, 3 scatter results
    uint32_t jobCount;
    uint32_t activeCount;
    uint32_t cpuCount;
    uint32_t sustainedCount;
    uint32_t envCount;
    uint32_t storeFrames;
    bool stagedPending;
    uint32_t cpuBase[GpuStage::kMaxJobs];
    uint32_t gpuBase[GpuStage::kMaxJobs];
    uint32_t envBase[GpuStage::kMaxJobs];
};

// Which GPU kernel (if any) renders the row's whole block exactly like the
// CPU: GpuStage::kKindSustained for a plain sustained-loop segment (the fast
// branch of RenderSustainedLoopSpan), kKindEnvelope for any other
// sample-backed, unfaded, unrotated class (the generic span machine), 0 for
// the CPU.
inline uint8_t RenderScalar::GpuRowKind(const VoiceSoA& v, uint32_t h,
                                        bool stagedPending,
                                        uint32_t storeFrames) const {
    if (wvStartFrame_[h] != UINT32_MAX || wvReleaseFrame_[h] != UINT32_MAX)
        return 0u;
    if (stagedPending && v.staged[h] != 0u) return 0u;
    const uint8_t channelRaw = v.channel[h];
    const uint32_t channel = channelRaw < kChannelCount ? channelRaw
                                                        : kChannelCount;
    if (wvChanOpStart_[channel] != wvChanOpStart_[channel + 1u] ||
        wvChanVibratoActive_[channel]) {
        return 0u;
    }
    if (wvExciteHead_ != nullptr && h < wvExciteCapacity_ &&
        wvExciteHead_[h] != UINT32_MAX) {
        return 0u;
    }
    const VoiceRenderClass renderClass = ClassifyWholeVoiceRow(v, h);
    if (renderClass == VoiceRenderClass::Generic) return 0u;
    const uint32_t sampleStart = v.sampleStart[h];
    const uint32_t relEnd = v.relEnd[h];
    if (sampleStart >= storeFrames || relEnd > storeFrames - sampleStart)
        return 0u;
    const float step = v.phaseIncs[h];
    if (renderClass == VoiceRenderClass::SustainedLoop) {
        if (sampleStart >= gpu::kFilterFlag) return 0u;
        return step > 0.0f && step < v.relLoopEF[h] - v.relLoopSF[h]
            ? GpuStage::kKindSustained : 0u;
    }
    if (h > gpu::kEnvHandleMask || !(step >= 0.0f)) return 0u;
    return GpuStage::kKindEnvelope;
}

inline void RenderScalar::GpuPassThunk(uint32_t job, const IndexedJobMix&,
                                       uint32_t, void* userData) {
    GpuPassContext& c = *static_cast<GpuPassContext*>(userData);
    RenderScalar* renderer = c.renderer;
    VoiceSoA& v = c.voices->v;
    GpuStage& stage = *c.stage;
    if (c.pass == 3u) {
        // Scatter the read-back dynamic state into the SoA.
        {
            const uint32_t begin = c.sustainedCount * job / c.jobCount;
            const uint32_t end = c.sustainedCount * (job + 1u) / c.jobCount;
            const gpu::ListEntry* list = stage.List();
            const gpu::DynOut* dyn = stage.Dyn();
            for (uint32_t k = begin; k < end; ++k) {
                const uint32_t h = list[k].handle;
                v.phases[h] = dyn[k].phase;
                v.filterZ1[h] = dyn[k].z1;
                v.filterZ2[h] = dyn[k].z2;
            }
        }
        const uint32_t begin = c.envCount * job / c.jobCount;
        const uint32_t end = c.envCount * (job + 1u) / c.jobCount;
        const gpu::EnvListEntry* list = stage.EnvList();
        const gpu::EnvDynOut* dyn = stage.EnvDyn();
        GpuStage::RetireRec* retire = stage.RetireScratch() + begin;
        uint32_t retired = 0u;
        for (uint32_t k = begin; k < end; ++k) {
            const uint32_t h = list[k].handleStage & gpu::kEnvHandleMask;
            const uint32_t stageBefore = list[k].handleStage >>
                                         gpu::kEnvStageShift;
            const gpu::EnvDynOut& d = dyn[k];
            v.phases[h] = d.phase;
            v.filterZ1[h] = d.z1;
            v.filterZ2[h] = d.z2;
            v.currentGain[h] = d.gain;
            if (stageBefore == 0u) {
                v.releaseSamplesRemaining[h] = d.releaseRem;
            } else {
                const uint32_t stageAfter = (d.stageRetire >> 24) & 0xFu;
                v.envelopeStage[h] = static_cast<uint8_t>(stageAfter);
                v.attackSamplesRemaining[h] = d.attackRem;
                v.decaySamplesRemaining[h] = d.decayRem;
                if (stageBefore != 3u && stageAfter == 3u) {
                    // Transient -> sustained crossing: the sustained kernels
                    // read renderGain, which the launch left at its
                    // pre-envelope value (same bake as the CPU batch path).
                    v.renderGainL[h] = v.currentGain[h] * v.mixGainL[h];
                    v.renderGainR[h] = v.currentGain[h] * v.mixGainR[h];
                }
            }
            const uint32_t frame = d.stageRetire & 0xFFFFFFu;
            if (frame != gpu::kEnvNoRetire)
                retire[retired++] = {h, frame};
        }
        stage.JobRetireCount()[job] = retired;
        return;
    }
    const uint32_t begin = c.activeCount * job / c.jobCount;
    const uint32_t end = c.activeCount * (job + 1u) / c.jobCount;
    const uint32_t* active = c.voices->activeList_;
    uint8_t* flags = stage.Flags();
    if (c.pass == 1u) {
        uint32_t sustained = 0u;
        uint32_t envelope = 0u;
        for (uint32_t i = begin; i < end; ++i) {
            const uint8_t kind = renderer->GpuRowKind(
                v, active[i], c.stagedPending, c.storeFrames);
            flags[i] = kind;
            sustained += kind == GpuStage::kKindSustained ? 1u : 0u;
            envelope += kind == GpuStage::kKindEnvelope ? 1u : 0u;
        }
        stage.JobGpuCount()[job] = sustained;
        stage.JobEnvCount()[job] = envelope;
        return;
    }
    // Pass 2: compact the three lists and pack what the GPU is missing.
    uint32_t* resident = stage.Resident();
    uint8_t* residentKind = stage.ResidentKind();
    gpu::ListEntry* list = stage.List();
    gpu::EnvListEntry* envList = stage.EnvList();
    gpu::UploadRecord* scratch = stage.UploadScratch() + begin;
    gpu::EnvUploadRecord* envScratch = stage.EnvUploadScratch() + begin;
    uint32_t* cpuOut = renderer->wvSliceHandles_ + c.cpuBase[job];
    uint32_t* gpuOut = renderer->wvSliceHandles_ + c.cpuCount + c.gpuBase[job];
    uint32_t* envOut = renderer->wvSliceHandles_ + c.cpuCount +
                       c.sustainedCount + c.envBase[job];
    gpu::ListEntry* listOut = list + c.gpuBase[job];
    gpu::EnvListEntry* envListOut = envList + c.envBase[job];
    uint32_t uploads = 0u;
    uint32_t envUploads = 0u;
    for (uint32_t i = begin; i < end; ++i) {
        const uint32_t h = active[i];
        const uint8_t kind = flags[i];
        if (kind == 0u) {
            *cpuOut++ = h;
            if (resident[h] != UINT32_MAX) resident[h] = UINT32_MAX;
            continue;
        }
        // A row without a play index (UINT32_MAX) can't be told apart from its
        // successor on the same handle, so it re-uploads every block.  The
        // kind also keys residency: held and released envelope rows carry
        // different statics under one play index.
        const uint32_t stamp = v.playIndex[h];
        if (kind == GpuStage::kKindSustained) {
            *gpuOut++ = h;
            *listOut++ = {h, v.phases[h], v.filterZ1[h], v.filterZ2[h]};
            if (stamp != UINT32_MAX && resident[h] == stamp &&
                residentKind[h] == kind) {
                continue;
            }
            resident[h] = stamp;
            residentKind[h] = kind;
            gpu::UploadRecord& r = scratch[uploads++];
            r.handle = h;
            r.words[0] = v.sampleStart[h] |
                         (v.filterEnabled[h] != 0u ? gpu::kFilterFlag : 0u);
            r.words[1] = v.relLoopS[h];
            r.words[2] = v.relLoopE[h];
            std::memcpy(&r.words[3], &v.relLoopSF[h], sizeof(float));
            std::memcpy(&r.words[4], &v.relLoopEF[h], sizeof(float));
            r.words[5] = 0u;
            std::memcpy(&r.words[6], &v.phaseIncs[h], sizeof(float));
            std::memcpy(&r.words[7], &v.renderGainL[h], sizeof(float));
            std::memcpy(&r.words[8], &v.renderGainR[h], sizeof(float));
            std::memcpy(&r.words[9], &v.filterA0[h], sizeof(float));
            std::memcpy(&r.words[10], &v.filterB1[h], sizeof(float));
            std::memcpy(&r.words[11], &v.filterB2[h], sizeof(float));
            continue;
        }
        const bool released =
            v.state[h] == static_cast<uint8_t>(VoiceState::Releasing);
        const uint32_t stage0 = released ? 0u : v.envelopeStage[h];
        *envOut++ = h;
        *envListOut++ = {h | (stage0 << gpu::kEnvStageShift), v.phases[h],
                         v.filterZ1[h], v.filterZ2[h], v.currentGain[h],
                         v.releaseSamplesRemaining[h],
                         v.attackSamplesRemaining[h],
                         v.decaySamplesRemaining[h]};
        const uint8_t envKind = released ? 3u : 2u;
        if (stamp != UINT32_MAX && resident[h] == stamp &&
            residentKind[h] == envKind) {
            continue;
        }
        resident[h] = stamp;
        residentKind[h] = envKind;
        gpu::EnvUploadRecord& r = envScratch[envUploads++];
        r.handle = h;
        r.words[0] = v.sampleStart[h];
        r.words[1] = (v.filterEnabled[h] != 0u ? gpu::kEnvFlagFilter : 0u) |
                     (v.loopEnabled[h] != 0u ? gpu::kEnvFlagLoop : 0u);
        r.words[2] = v.relEnd[h];
        r.words[3] = v.relLoopS[h];
        r.words[4] = v.relLoopE[h];
        std::memcpy(&r.words[5], &v.relLoopSF[h], sizeof(float));
        std::memcpy(&r.words[6], &v.relLoopEF[h], sizeof(float));
        std::memcpy(&r.words[7], &v.phaseIncs[h], sizeof(float));
        std::memcpy(&r.words[8], &v.mixGainL[h], sizeof(float));
        std::memcpy(&r.words[9], &v.mixGainR[h], sizeof(float));
        std::memcpy(&r.words[10], &v.targetGain[h], sizeof(float));
        std::memcpy(&r.words[11], &v.sustainLevel[h], sizeof(float));
        std::memcpy(&r.words[12], &v.attackGainStep[h], sizeof(float));
        std::memcpy(&r.words[13], &v.decaySlope[h], sizeof(float));
        std::memcpy(&r.words[14], &v.releaseDecay[h], sizeof(float));
        std::memcpy(&r.words[15], &v.filterA0[h], sizeof(float));
        std::memcpy(&r.words[16], &v.filterB1[h], sizeof(float));
        std::memcpy(&r.words[17], &v.filterB2[h], sizeof(float));
        r.words[18] = 0u;
        r.words[19] = 0u;
    }
    stage.JobUploadCount()[job] = uploads;
    stage.JobEnvUploadCount()[job] = envUploads;
}

// Runs one pass over the pool (or inline when the pool refuses).
inline void RenderScalar::RunGpuPass(GpuPassContext& ctx, float* outputLeft,
                                     float* outputRight) {
    bool dispatched = false;
    if (ctx.jobCount >= 2u && workerPool_ != nullptr) {
        dispatched = workerPool_->ExecuteIndexed(
            ctx.jobCount, 1u, outputLeft, outputRight, &GpuPassThunk, &ctx,
            nullptr, nullptr);
    }
    if (!dispatched) {
        for (uint32_t job = 0u; job < ctx.jobCount; ++job)
            GpuPassThunk(job, IndexedJobMix{}, 1u, &ctx);
    }
}

// Moves the block's GPU rows to the tail of wvSliceHandles_ and starts the
// GPU.  Returns the number of rows the GPU owns this block (0 = CPU only).
// `submitted` is false when rows were partitioned but the submit failed; the
// caller then renders them on the CPU like a late block.
inline uint32_t RenderScalar::TryGpuSubmit(VoiceManager& voices,
                                           const int16_t* sampleData,
                                           uint32_t sampleDataFrames,
                                           float* outputLeft,
                                           float* outputRight,
                                           uint32_t numFrames,
                                           uint32_t activeCount,
                                           float* const* channelBusLeft,
                                           bool& submitted) {
    submitted = false;
    gpuSustainedRows_ = 0u;
    gpuEnvRows_ = 0u;
    GpuStage& stage = *gpuStage_;
    if (!stage.Enabled()) return 0u;
    if (voices.StaticEpoch() != gpuVoiceEpoch_) {
        // Row statics were rewritten outside the plan's op log.
        gpuVoiceEpoch_ = voices.StaticEpoch();
        stage.BumpEpoch();
    }
    if (sampleData == nullptr || numFrames < 8u || channelBusLeft != nullptr ||
        voices.v.rot != nullptr || wvDecimating_ || wvScalarVibrato_ ||
        activeCount < stage.MinimumVoices() ||
        !stage.Prepare(sampleData, sampleDataFrames, voices.v.GetCapacity(),
                       numFrames)) {
        // CPU-only block: rows may change under a resident copy.
        stage.BumpEpoch();
        return 0u;
    }
    GpuPassContext ctx{};
    ctx.renderer = this;
    ctx.voices = &voices;
    ctx.stage = &stage;
    ctx.activeCount = activeCount;
    ctx.storeFrames = sampleDataFrames;
    ctx.stagedPending = voices.StagedPending();
    uint32_t jobs = 1u;
    if (workerPool_ != nullptr) {
        jobs = (std::min)((std::min)(workerPool_->GetThreadCount() * 2u,
                                     workerPool_->GetJobCapacity()),
                          GpuStage::kMaxJobs);
        jobs = (std::min)(jobs, (std::max)(1u, activeCount / 2048u));
    }
    ctx.jobCount = jobs < 2u ? 1u : jobs;

    const uint64_t classifyStart = GpuStage::NowMicros();
    ctx.pass = 1u;
    RunGpuPass(ctx, outputLeft, outputRight);
    stage.MutableStats().classifyMicros += GpuStage::NowMicros() - classifyStart;
    uint32_t sustainedTotal = 0u;
    uint32_t envTotal = 0u;
    for (uint32_t job = 0u; job < ctx.jobCount; ++job) {
        sustainedTotal += stage.JobGpuCount()[job];
        envTotal += stage.JobEnvCount()[job];
    }
    const uint32_t gpuTotal = sustainedTotal + envTotal;
    if (gpuTotal < stage.MinimumVoices()) {
        stage.BumpEpoch();
        return 0u;
    }
    ctx.cpuCount = activeCount - gpuTotal;
    ctx.sustainedCount = sustainedTotal;
    ctx.envCount = envTotal;
    uint32_t cpuRun = 0u;
    uint32_t gpuRun = 0u;
    uint32_t envRun = 0u;
    for (uint32_t job = 0u; job < ctx.jobCount; ++job) {
        const uint32_t begin = activeCount * job / ctx.jobCount;
        const uint32_t end = activeCount * (job + 1u) / ctx.jobCount;
        ctx.cpuBase[job] = cpuRun;
        ctx.gpuBase[job] = gpuRun;
        ctx.envBase[job] = envRun;
        cpuRun += (end - begin) - stage.JobGpuCount()[job] -
                  stage.JobEnvCount()[job];
        gpuRun += stage.JobGpuCount()[job];
        envRun += stage.JobEnvCount()[job];
    }
    const uint64_t packStart = GpuStage::NowMicros();
    ctx.pass = 2u;
    RunGpuPass(ctx, outputLeft, outputRight);
    stage.MutableStats().packMicros += GpuStage::NowMicros() - packStart;

    const uint64_t uploadStart = GpuStage::NowMicros();
    uint32_t uploads = 0u;
    uint32_t envUploads = 0u;
    bool ok = true;
    for (uint32_t job = 0u; job < ctx.jobCount && ok; ++job) {
        const uint32_t begin = activeCount * job / ctx.jobCount;
        const uint32_t count = stage.JobUploadCount()[job];
        const uint32_t envCount = stage.JobEnvUploadCount()[job];
        uploads += count;
        envUploads += envCount;
        ok = stage.UploadRows(stage.UploadScratch() + begin, count) &&
             stage.UploadEnvRows(stage.EnvUploadScratch() + begin, envCount);
    }
    stage.MutableStats().uploadMicros += GpuStage::NowMicros() - uploadStart;
    submitted = ok && stage.Submit(sustainedTotal, envTotal, numFrames,
                                   uploads, envUploads,
                                   VoiceRetireThreshold());
    if (!submitted) stage.BumpEpoch();
    gpuSustainedRows_ = sustainedTotal;
    gpuEnvRows_ = envTotal;
    return gpuTotal;
}

// Waits for the GPU and folds its rows into the block, or renders them on the
// CPU when it was late or failed.  `cpuCount` is where the GPU rows start in
// wvSliceHandles_.
inline void RenderScalar::FinishGpuRows(VoiceManager& voices,
                                        const int16_t* sampleData,
                                        uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames, uint32_t gpuCount,
                                        uint32_t cpuCount, bool submitted) {
    GpuStage& stage = *gpuStage_;
    const uint32_t sustainedCount = gpuSustainedRows_;
    const uint32_t envCount = gpuEnvRows_;
    if (submitted && stage.Collect(numFrames) == gpu::Result::Ok) {
        const float* mix = stage.Mix();
        for (uint32_t frame = 0u; frame < numFrames; ++frame) {
            outputLeft[frame] += mix[frame * 2u];
            outputRight[frame] += mix[frame * 2u + 1u];
        }
        const uint64_t scatterStart = GpuStage::NowMicros();
        GpuPassContext ctx{};
        ctx.renderer = this;
        ctx.voices = &voices;
        ctx.stage = &stage;
        ctx.pass = 3u;
        ctx.activeCount = cpuCount + gpuCount;
        ctx.cpuCount = cpuCount;
        ctx.sustainedCount = sustainedCount;
        ctx.envCount = envCount;
        uint32_t jobs = 1u;
        if (workerPool_ != nullptr) {
            jobs = (std::min)((std::min)(workerPool_->GetThreadCount() * 2u,
                                         workerPool_->GetJobCapacity()),
                              GpuStage::kMaxJobs);
            jobs = (std::min)(jobs, (std::max)(1u, gpuCount / 2048u));
        }
        ctx.jobCount = jobs < 2u ? 1u : jobs;
        RunGpuPass(ctx, outputLeft, outputRight);
        // Retire frames the envelope kernel found join the block's retirement
        // list (job slot 0; the list is sorted by frame and active position
        // afterwards, so the slot does not matter).
        if (envCount != 0u) {
            SpanRetirement* retBuf = wvJobRetirements_;
            uint32_t& retCount = wvJobRetireCounts_[0];
            for (uint32_t job = 0u; job < ctx.jobCount; ++job) {
                const uint32_t begin = envCount * job / ctx.jobCount;
                const GpuStage::RetireRec* recs =
                    stage.RetireScratch() + begin;
                const uint32_t n = stage.JobRetireCount()[job];
                for (uint32_t i = 0u; i < n && retCount < scratchCapacity_;
                     ++i) {
                    retBuf[retCount++] = {recs[i].handle, recs[i].frame,
                                          voices.activePosition_[recs[i].handle]};
                }
            }
        }
        stage.MutableStats().scatterMicros +=
            GpuStage::NowMicros() - scatterStart;
        return;
    }
    // Late, lost or never submitted: the SoA still holds the pre-block state,
    // so the CPU kernels reproduce the block exactly (statics are unchanged,
    // residency stays valid).
    VoiceSoA& v = voices.v;
    const IndexedJobMix mix{outputLeft, outputRight, nullptr, nullptr};
    const uint32_t* gpuRows = wvSliceHandles_ + cpuCount;
    for (uint32_t offset = 0u; offset < sustainedCount; offset += 256u) {
        const uint32_t n = (std::min)(256u, sustainedCount - offset);
        if (RenderWholeVoiceBatch(v, voices, gpuRows + offset, n,
                                  VoiceRenderClass::SustainedLoop, sampleData,
                                  sampleDataFrames, mix, 0u, numFrames, 0u)) {
            continue;
        }
        for (uint32_t i = 0u; i < n; ++i) {
            RenderWholeVoiceSegment(v, &voices, gpuRows[offset + i], sampleData,
                                    sampleDataFrames, outputLeft, outputRight,
                                    0u, numFrames, true, 0u, 0.0f, 1.0f, false);
        }
    }
    for (uint32_t i = 0u; i < envCount; ++i) {
        RenderWholeVoiceSegment(v, &voices, gpuRows[sustainedCount + i],
                                sampleData, sampleDataFrames, outputLeft,
                                outputRight, 0u, numFrames, true, 0u, 0.0f,
                                1.0f, false);
    }
}

#endif  // SVMS_GPU_STAGE
