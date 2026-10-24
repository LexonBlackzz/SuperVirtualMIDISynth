// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.
//
// GPU render stage glue (see render/GpuStage.h).  A block that qualifies has
// its eligible sustained-loop rows moved to the tail of wvSliceHandles_ and
// rendered by the GPU while the worker pool renders everything else.  The
// rows are eligible only when the plan left them a plain whole-block segment,
// so event timing, release frames, ops and retirement never involve the GPU;
// a late or failed GPU falls back to the exact CPU kernels with the SoA still
// holding the pre-block state.

#if defined(SVMS_GPU_STAGE)

struct RenderScalar::GpuPassContext {
    RenderScalar* renderer;
    VoiceManager* voices;
    GpuStage* stage;
    uint32_t pass;          // 1 classify, 2 pack, 3 scatter results
    uint32_t jobCount;
    uint32_t activeCount;
    uint32_t cpuCount;
    uint32_t storeFrames;
    bool stagedPending;
    uint32_t cpuBase[GpuStage::kMaxJobs];
    uint32_t gpuBase[GpuStage::kMaxJobs];
};

// True when the row's whole block is one plain sustained-loop segment the GPU
// kernel reproduces exactly (the fast branch of RenderSustainedLoopSpan).
inline bool RenderScalar::GpuRowEligible(const VoiceSoA& v, uint32_t h,
                                         bool stagedPending,
                                         uint32_t storeFrames) const {
    if (wvStartFrame_[h] != UINT32_MAX || wvReleaseFrame_[h] != UINT32_MAX)
        return false;
    if (stagedPending && v.staged[h] != 0u) return false;
    const uint8_t channelRaw = v.channel[h];
    const uint32_t channel = channelRaw < kChannelCount ? channelRaw
                                                        : kChannelCount;
    if (wvChanOpStart_[channel] != wvChanOpStart_[channel + 1u] ||
        wvChanVibratoActive_[channel]) {
        return false;
    }
    if (wvExciteHead_ != nullptr && h < wvExciteCapacity_ &&
        wvExciteHead_[h] != UINT32_MAX) {
        return false;
    }
    if (ClassifyWholeVoiceRow(v, h) != VoiceRenderClass::SustainedLoop)
        return false;
    const uint32_t sampleStart = v.sampleStart[h];
    const uint32_t relEnd = v.relEnd[h];
    if (sampleStart >= storeFrames || relEnd > storeFrames - sampleStart ||
        sampleStart >= gpu::kFilterFlag) {
        return false;
    }
    const float step = v.phaseIncs[h];
    return step > 0.0f && step < v.relLoopEF[h] - v.relLoopSF[h];
}

inline void RenderScalar::GpuPassThunk(uint32_t job, const IndexedJobMix&,
                                       uint32_t, void* userData) {
    GpuPassContext& c = *static_cast<GpuPassContext*>(userData);
    RenderScalar* renderer = c.renderer;
    VoiceSoA& v = c.voices->v;
    GpuStage& stage = *c.stage;
    if (c.pass == 3u) {
        // Scatter the read-back dynamic state into the SoA.
        const uint32_t total = c.activeCount - c.cpuCount;
        const uint32_t begin = total * job / c.jobCount;
        const uint32_t end = total * (job + 1u) / c.jobCount;
        const gpu::ListEntry* list = stage.List();
        const gpu::DynOut* dyn = stage.Dyn();
        for (uint32_t k = begin; k < end; ++k) {
            const uint32_t h = list[k].handle;
            v.phases[h] = dyn[k].phase;
            v.filterZ1[h] = dyn[k].z1;
            v.filterZ2[h] = dyn[k].z2;
        }
        return;
    }
    const uint32_t begin = c.activeCount * job / c.jobCount;
    const uint32_t end = c.activeCount * (job + 1u) / c.jobCount;
    const uint32_t* active = c.voices->activeList_;
    uint8_t* flags = stage.Flags();
    if (c.pass == 1u) {
        uint32_t count = 0u;
        for (uint32_t i = begin; i < end; ++i) {
            const bool gpuRow = renderer->GpuRowEligible(
                v, active[i], c.stagedPending, c.storeFrames);
            flags[i] = gpuRow ? 1u : 0u;
            count += gpuRow ? 1u : 0u;
        }
        stage.JobGpuCount()[job] = count;
        return;
    }
    // Pass 2: compact the two lists and pack what the GPU is missing.
    uint32_t* resident = stage.Resident();
    gpu::ListEntry* list = stage.List();
    gpu::UploadRecord* scratch = stage.UploadScratch() + begin;
    uint32_t* cpuOut = renderer->wvSliceHandles_ + c.cpuBase[job];
    uint32_t* gpuOut = renderer->wvSliceHandles_ + c.cpuCount + c.gpuBase[job];
    gpu::ListEntry* listOut = list + c.gpuBase[job];
    uint32_t uploads = 0u;
    for (uint32_t i = begin; i < end; ++i) {
        const uint32_t h = active[i];
        if (flags[i] == 0u) {
            *cpuOut++ = h;
            if (resident[h] != UINT32_MAX) resident[h] = UINT32_MAX;
            continue;
        }
        *gpuOut++ = h;
        *listOut++ = {h, v.phases[h], v.filterZ1[h], v.filterZ2[h]};
        // A row without a play index (UINT32_MAX) can't be told apart from its
        // successor on the same handle, so it re-uploads every block.
        const uint32_t stamp = v.playIndex[h];
        if (stamp != UINT32_MAX && resident[h] == stamp) continue;
        resident[h] = stamp;
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
    }
    stage.JobUploadCount()[job] = uploads;
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
    uint32_t gpuTotal = 0u;
    for (uint32_t job = 0u; job < ctx.jobCount; ++job)
        gpuTotal += stage.JobGpuCount()[job];
    if (gpuTotal < stage.MinimumVoices()) {
        stage.BumpEpoch();
        return 0u;
    }
    ctx.cpuCount = activeCount - gpuTotal;
    uint32_t cpuRun = 0u;
    uint32_t gpuRun = 0u;
    for (uint32_t job = 0u; job < ctx.jobCount; ++job) {
        const uint32_t begin = activeCount * job / ctx.jobCount;
        const uint32_t end = activeCount * (job + 1u) / ctx.jobCount;
        ctx.cpuBase[job] = cpuRun;
        ctx.gpuBase[job] = gpuRun;
        cpuRun += (end - begin) - stage.JobGpuCount()[job];
        gpuRun += stage.JobGpuCount()[job];
    }
    const uint64_t packStart = GpuStage::NowMicros();
    ctx.pass = 2u;
    RunGpuPass(ctx, outputLeft, outputRight);
    stage.MutableStats().packMicros += GpuStage::NowMicros() - packStart;

    const uint64_t uploadStart = GpuStage::NowMicros();
    uint32_t uploads = 0u;
    bool ok = true;
    for (uint32_t job = 0u; job < ctx.jobCount && ok; ++job) {
        const uint32_t begin = activeCount * job / ctx.jobCount;
        const uint32_t count = stage.JobUploadCount()[job];
        uploads += count;
        ok = stage.UploadRows(stage.UploadScratch() + begin, count);
    }
    stage.MutableStats().uploadMicros += GpuStage::NowMicros() - uploadStart;
    submitted = ok && stage.Submit(gpuTotal, numFrames, uploads);
    if (!submitted) stage.BumpEpoch();
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
        uint32_t jobs = 1u;
        if (workerPool_ != nullptr) {
            jobs = (std::min)((std::min)(workerPool_->GetThreadCount() * 2u,
                                         workerPool_->GetJobCapacity()),
                              GpuStage::kMaxJobs);
            jobs = (std::min)(jobs, (std::max)(1u, gpuCount / 2048u));
        }
        ctx.jobCount = jobs < 2u ? 1u : jobs;
        RunGpuPass(ctx, outputLeft, outputRight);
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
    for (uint32_t offset = 0u; offset < gpuCount; offset += 256u) {
        const uint32_t n = (std::min)(256u, gpuCount - offset);
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
}

#endif  // SVMS_GPU_STAGE
