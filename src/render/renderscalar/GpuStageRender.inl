// Part of RenderScalar.h; included from there inside namespace svms. Not standalone.
//
// GPU render stage glue (see render/GpuStage.h).  A block that qualifies has
// its eligible rows listed in the stage (sustained and envelope lists) and
// rendered by the GPU while the worker pool renders
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
    uint32_t jobCount;      // jobs the active list is split into
    uint32_t jobFirst;      // pass 2: the jobs [jobFirst, jobEnd) of one part
    uint32_t jobEnd;
    uint32_t partSBase;     // pass 3: the part's slice of the two lists
    uint32_t partEBase;
    uint32_t activeCount;
    uint32_t cpuCount;
    uint32_t sustainedCount;
    uint32_t envCount;
    uint32_t storeFrames;
    bool stagedPending;
    uint32_t cpuBase[GpuStage::kMaxJobs];
    uint32_t gpuBase[GpuStage::kMaxJobs];
    uint32_t envBase[GpuStage::kMaxJobs];
    // Sort passes 4..7 (SortGpuRows): the list being sorted and its scratch.
    uint32_t sortCount;
    uint32_t* sortRows;
    uint64_t* sortA;
    uint64_t* sortB;
    uint32_t* sortHist;     // sort jobs x GpuStage::kSortBuckets counters
    const uint32_t* sortSampleStart;
    const float* sortPhases;
};

// Which GPU kernel (if any) renders the row's whole block exactly like the
// CPU: GpuStage::kKindSustained for a plain sustained-loop segment (the fast
// branch of RenderSustainedLoopSpan), kKindEnvelope for any other
// sample-backed, unfaded, unrotated class (the generic span machine), 0 for
// the CPU.
inline bool RenderScalar::GpuRowBlockOk(const VoiceSoA& v, uint32_t h,
                                        bool stagedPending) const {
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
    return true;
}

inline uint8_t RenderScalar::GpuRowKind(const VoiceSoA& v, uint32_t h,
                                        bool stagedPending,
                                        uint32_t storeFrames) const {
    if (!GpuRowBlockOk(v, h, stagedPending)) return 0u;
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
    if (!(step >= 0.0f)) return 0u;
    return GpuStage::kKindEnvelope;
}

inline void RenderScalar::GpuPassThunk(uint32_t localJob, const IndexedJobMix&,
                                       uint32_t, void* userData) {
    GpuPassContext& c = *static_cast<GpuPassContext*>(userData);
    // Passes 1 and 2 split the active list into jobCount ranges (pass 2 runs
    // them part by part); pass 3 splits one part's list slice.
    if (c.pass >= 4u) {
        // Parallel radix sort (two 12-bit digits) of c.sortRows[0, c.sortCount):
        // each job owns one contiguous chunk of the input of every pass.
        const uint32_t jobs = c.jobEnd - c.jobFirst;
        const uint32_t begin = c.sortCount * localJob / jobs;
        const uint32_t end = c.sortCount * (localJob + 1u) / jobs;
        uint32_t* hist = c.sortHist + localJob * GpuStage::kSortBuckets;
        constexpr uint32_t kMask = GpuStage::kSortBuckets - 1u;
        if (c.pass == 4u) {
            std::memset(hist, 0, GpuStage::kSortBuckets * sizeof(uint32_t));
            for (uint32_t i = begin; i < end; ++i) {
                const uint32_t h = c.sortRows[i];
                const uint64_t e = GpuStage::SortEntry(
                    h, c.sortSampleStart[h], c.sortPhases[h]);
                c.sortA[i] = e;
                ++hist[(e >> 32) & kMask];
            }
        } else if (c.pass == 5u) {
            for (uint32_t i = begin; i < end; ++i)
                c.sortB[hist[(c.sortA[i] >> 32) & kMask]++] = c.sortA[i];
        } else if (c.pass == 6u) {
            std::memset(hist, 0, GpuStage::kSortBuckets * sizeof(uint32_t));
            for (uint32_t i = begin; i < end; ++i)
                ++hist[(c.sortB[i] >> 44) & kMask];
        } else {
            for (uint32_t i = begin; i < end; ++i) {
                c.sortRows[hist[(c.sortB[i] >> 44) & kMask]++] =
                    static_cast<uint32_t>(c.sortB[i]);
            }
        }
        return;
    }
    const uint32_t job = c.pass == 3u ? localJob : c.jobFirst + localJob;
    RenderScalar* renderer = c.renderer;
    VoiceSoA& v = c.voices->v;
    GpuStage& stage = *c.stage;
    if (c.pass == 3u) {
        // Scatter the read-back dynamic state into the SoA.
        {
            const uint32_t begin = c.partSBase + c.sustainedCount * job / c.jobCount;
            const uint32_t end = c.partSBase + c.sustainedCount * (job + 1u) / c.jobCount;
            const uint32_t* list = stage.List();
            const gpu::DynOut* dyn = stage.Dyn();
            for (uint32_t k = begin; k < end; ++k) {
                const uint32_t h = list[k];
                v.phases[h] = dyn[k].phase;
                if (v.filterEnabled[h] != 0u) {
                    v.filterZ1[h] = dyn[k].z1;
                    v.filterZ2[h] = dyn[k].z2;
                }
            }
        }
        const uint32_t begin = c.partEBase + c.envCount * job / c.jobCount;
        const uint32_t end = c.partEBase + c.envCount * (job + 1u) / c.jobCount;
        const uint32_t* list = stage.EnvList();
        const gpu::EnvDynOut* dyn = stage.EnvDyn();
        GpuStage::RetireRec* retire = stage.RetireScratch() + begin;
        uint32_t* cross = stage.CrossScratch() + begin;
        uint32_t retired = 0u;
        uint32_t crossed = 0u;
        for (uint32_t k = begin; k < end; ++k) {
            const uint32_t h = list[k];
            // The SoA still holds the pre-block lifecycle (it is rewritten
            // below), so the stage the kernel started from is read here.
            const uint32_t stageBefore =
                v.state[h] == static_cast<uint8_t>(VoiceState::Releasing)
                    ? 0u : v.envelopeStage[h];
            const gpu::EnvDynOut& d = dyn[k];
            // Only the fields the kernel can change for this lifecycle stage.
            v.phases[h] = d.phase;
            if (v.filterEnabled[h] != 0u) {
                // The kernel leaves an unfiltered row's delay line alone.
                v.filterZ1[h] = d.z1;
                v.filterZ2[h] = d.z2;
            }
            if (stageBefore == 0u) {
                v.currentGain[h] = d.gain;
                v.releaseSamplesRemaining[h] = d.releaseRem;
            } else if (stageBefore != 3u) {
                v.currentGain[h] = d.gain;
                const uint32_t stageAfter = (d.stageRetire >> 24) & 0xFu;
                v.envelopeStage[h] = static_cast<uint8_t>(stageAfter);
                v.attackSamplesRemaining[h] = d.attackRem;
                v.decaySamplesRemaining[h] = d.decayRem;
                if (stageAfter == 3u) {
                    // Transient -> sustained crossing: the sustained kernels
                    // read renderGain, which the launch left at its
                    // pre-envelope value (same bake as the CPU batch path).
                    // The render class moves with it, and the GPU copy of
                    // the row (envelope kind) no longer fits the new class.
                    v.renderGainL[h] = v.currentGain[h] * v.mixGainL[h];
                    v.renderGainR[h] = v.currentGain[h] * v.mixGainR[h];
                    stage.Resident()[h] = UINT32_MAX;
                    cross[crossed++] = h;
                }
            }
            const uint32_t frame = d.stageRetire & 0xFFFFFFu;
            if (frame != gpu::kEnvNoRetire)
                retire[retired++] = {h, frame};
        }
        stage.JobRetireCount()[job] = retired;
        stage.JobCrossCount()[job] = crossed;
        return;
    }
    const uint32_t begin = c.activeCount * job / c.jobCount;
    const uint32_t end = c.activeCount * (job + 1u) / c.jobCount;
    const uint32_t* active = c.voices->activeList_;
    uint8_t* flags = stage.Flags();
    if (c.pass == 1u) {
        uint32_t sustained = 0u;
        uint32_t envelope = 0u;
        uint32_t* resident = stage.Resident();
        const uint8_t* residentKind = stage.ResidentKind();
        for (uint32_t i = begin; i < end; ++i) {
            const uint32_t h = active[i];
            const uint32_t stamp = v.playIndex[h];
            uint8_t kind;
            uint8_t mark = 0u;
            // A resident row whose lifecycle still matches the kind it was
            // uploaded as keeps that kind (its statics, class and envelope
            // can only move through events that change the play index or the
            // lifecycle state, or through a render, which invalidates it).
            const uint8_t rk = residentKind[h];
            const uint8_t st = v.state[h];
            if (stamp != UINT32_MAX && resident[h] == stamp && rk != 0u &&
                ((rk == 3u && st == static_cast<uint8_t>(VoiceState::Releasing)) ||
                 (rk != 3u && st == static_cast<uint8_t>(VoiceState::Active)))) {
                kind = renderer->GpuRowBlockOk(v, h, c.stagedPending)
                    ? (rk == 1u ? GpuStage::kKindSustained
                                : GpuStage::kKindEnvelope)
                    : 0u;
            } else {
                kind = renderer->GpuRowKind(v, h, c.stagedPending, c.storeFrames);
                mark = GpuStage::kNeedsCheck;
            }
            if (kind == 0u && resident[h] != UINT32_MAX) resident[h] = UINT32_MAX;
            flags[i] = static_cast<uint8_t>(kind | mark);
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
    uint32_t* list = stage.List();
    uint32_t* envList = stage.EnvList();
    gpu::UploadRecord* scratch = stage.UploadScratch() + begin;
    gpu::EnvUploadRecord* envScratch = stage.EnvUploadScratch() + begin;
    uint32_t* cpuOut = stage.CpuRows() + c.cpuBase[job];
    uint32_t* listOut = list + c.gpuBase[job];
    uint32_t* envListOut = envList + c.envBase[job];
    uint32_t uploads = 0u;
    uint32_t envUploads = 0u;
    for (uint32_t i = begin; i < end; ++i) {
        const uint32_t h = active[i];
        const uint8_t kind = flags[i] & 3u;
        if (kind == 0u) {
            *cpuOut++ = h;
            continue;
        }
        if ((flags[i] & GpuStage::kNeedsCheck) == 0u) {
            // Resident and still valid: the list is all the GPU needs.
            if (kind == GpuStage::kKindSustained) {
                *listOut++ = h;
            } else {
                *envListOut++ = h;
            }
            continue;
        }
        // A row without a play index (UINT32_MAX) can't be told apart from its
        // successor on the same handle, so it re-uploads every block.  The
        // kind also keys residency: held and released envelope rows carry
        // different statics under one play index.
        const uint32_t stamp = v.playIndex[h];
        if (kind == GpuStage::kKindSustained) {
            *listOut++ = h;
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
            r.phase = v.phases[h];
            r.z1 = v.filterZ1[h];
            r.z2 = v.filterZ2[h];
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
        *envListOut++ = h;
        const uint8_t envKind = released ? 3u : 2u;
        if (stamp != UINT32_MAX && resident[h] == stamp &&
            residentKind[h] == envKind) {
            continue;
        }
        resident[h] = stamp;
        residentKind[h] = envKind;
        gpu::EnvUploadRecord& r = envScratch[envUploads++];
        r.handle = h;
        std::memcpy(&r.dyn[0], &v.phases[h], sizeof(float));
        std::memcpy(&r.dyn[1], &v.filterZ1[h], sizeof(float));
        std::memcpy(&r.dyn[2], &v.filterZ2[h], sizeof(float));
        std::memcpy(&r.dyn[3], &v.currentGain[h], sizeof(float));
        r.dyn[4] = stage0;
        r.dyn[5] = v.attackSamplesRemaining[h];
        r.dyn[6] = v.decaySamplesRemaining[h];
        r.dyn[7] = v.releaseSamplesRemaining[h];
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
    const uint32_t jobs = ctx.jobEnd - ctx.jobFirst;
    if (jobs >= 2u && workerPool_ != nullptr) {
        dispatched = workerPool_->ExecuteIndexed(
            jobs, 1u, outputLeft, outputRight, &GpuPassThunk, &ctx,
            nullptr, nullptr);
    }
    if (!dispatched) {
        for (uint32_t job = 0u; job < jobs; ++job)
            GpuPassThunk(job, IndexedJobMix{}, 1u, &ctx);
    }
}

// Orders rows[0, count) by the store position each row reads at block start
// (see GpuStage::SortEntry).  Big lists go through the worker pool: four short
// dispatches (keys + digit-0 histogram, scatter, digit-1 histogram, scatter)
// with a serial prefix over jobs x 4096 counters between them.  Small lists and
// a missing pool use the serial sort.  Stable, so equal keys keep list order.
inline void RenderScalar::SortGpuRows(GpuPassContext& ctx, uint32_t* rows,
                                      uint32_t count, float* outputLeft,
                                      float* outputRight) {
    GpuStage& stage = *ctx.stage;
    if (!stage.SortByRegion() || count < 64u || count > stage.SortCapacity())
        return;
    const VoiceSoA& v = ctx.voices->v;
    uint32_t jobs = 1u;
    if (workerPool_ != nullptr) {
        jobs = (std::min)((std::min)(workerPool_->GetThreadCount(),
                                     workerPool_->GetJobCapacity()),
                          GpuStage::kMaxJobs);
        jobs = (std::min)(jobs, count / 8192u);
    }
    if (jobs < 2u) {
        stage.SortRows(rows, count, v.sampleStart, v.phases);
        return;
    }
    const uint64_t start = GpuStage::NowMicros();
    ctx.jobFirst = 0u;
    ctx.jobEnd = jobs;
    ctx.sortCount = count;
    ctx.sortRows = rows;
    ctx.sortA = stage.SortA();
    ctx.sortB = stage.SortB();
    ctx.sortHist = stage.SortHist();
    ctx.sortSampleStart = v.sampleStart;
    ctx.sortPhases = v.phases;
    // Turns the per-job counts into start offsets: bucket-major, job-minor, so
    // earlier jobs' elements precede later jobs' within a bucket (stable).
    auto prefix = [&]() {
        uint32_t total = 0u;
        for (uint32_t d = 0u; d < GpuStage::kSortBuckets; ++d) {
            for (uint32_t j = 0u; j < jobs; ++j) {
                uint32_t& c = ctx.sortHist[j * GpuStage::kSortBuckets + d];
                const uint32_t n = c;
                c = total;
                total += n;
            }
        }
    };
    ctx.pass = 4u;
    RunGpuPass(ctx, outputLeft, outputRight);
    prefix();
    ctx.pass = 5u;
    RunGpuPass(ctx, outputLeft, outputRight);
    ctx.pass = 6u;
    RunGpuPass(ctx, outputLeft, outputRight);
    prefix();
    ctx.pass = 7u;
    RunGpuPass(ctx, outputLeft, outputRight);
    stage.AddSortMicros(GpuStage::NowMicros() - start);
}

// Lists the block's GPU rows (the CPU keeps the rest in wvItemHandles_) and
// starts the GPU.  Returns the number of rows the GPU owns this block (0 = CPU only).
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
    gpuSkipRefresh_ = 0u;
    gpuRefreshHandles_.clear();
    GpuStage& stage = *gpuStage_;
    if (!stage.Enabled()) return 0u;
    if (voices.StaticEpoch() != gpuVoiceEpoch_) {
        // Row statics were rewritten outside the plan's op log.
        gpuVoiceEpoch_ = voices.StaticEpoch();
        stage.BumpEpoch();
    }
    GpuStageStats& counters = stage.MutableStats();
    ++counters.considered;
    uint32_t declineReason = 8u;
    if (sampleData == nullptr || numFrames < 8u) declineReason = 0u;
    else if (channelBusLeft != nullptr) declineReason = 1u;
    else if (voices.v.rot != nullptr) declineReason = 2u;
    else if (wvDecimating_ || wvScalarVibrato_) declineReason = 3u;
    else if (activeCount < stage.MinimumVoices()) declineReason = 4u;
    else if (!stage.Prepare(sampleData, sampleDataFrames,
                            voices.v.GetCapacity(), numFrames)) declineReason = 5u;
    if (declineReason != 8u) {
        // CPU-only block: rows may change under a resident copy.
        ++counters.declined[declineReason];
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
    // A big block is classified, packed and submitted part by part: the GPU
    // starts on part 0 after a fraction of the CPU passes and the CPU keeps
    // packing the next part (and scattering the previous one) while it runs.
    // Part p owns the jobs [p * jobsPerPart, (p + 1) * jobsPerPart) of the
    // active list; the jobs' outputs are contiguous in every list, so a part
    // is a slice of each.
    const uint32_t parts = activeCount >= stage.SplitMinRows() ? stage.Parts() : 1u;
    uint32_t jobsPerPart = 1u;
    if (workerPool_ != nullptr) {
        jobsPerPart = (std::min)(workerPool_->GetThreadCount() * 2u,
                                 workerPool_->GetJobCapacity());
        jobsPerPart = (std::min)(jobsPerPart,
                                 (std::max)(1u, activeCount / parts / 2048u));
    }
    while (parts * jobsPerPart > GpuStage::kMaxJobs) --jobsPerPart;
    ctx.jobCount = parts * jobsPerPart;

    gpuPartCount_ = 0u;
    gpuSubmittedParts_ = 0u;
    uint32_t cpuRun = 0u;
    uint32_t gpuRun = 0u;
    uint32_t envRun = 0u;
    for (uint32_t part = 0u; part < parts; ++part) {
        const uint32_t j0 = part * jobsPerPart;
        const uint32_t j1 = j0 + jobsPerPart;
        const bool last = part + 1u == parts;
        const uint64_t classifyStart = GpuStage::NowMicros();
        ctx.pass = 1u;
        ctx.jobFirst = j0;
        ctx.jobEnd = j1;
        RunGpuPass(ctx, outputLeft, outputRight);
        stage.MutableStats().classifyMicros += GpuStage::NowMicros() - classifyStart;
        const uint32_t sStart = gpuRun;
        const uint32_t eStart = envRun;
        for (uint32_t job = j0; job < j1; ++job) {
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
        // The first part stands for the block: extrapolate its GPU share.
        const uint32_t partRows = activeCount * j1 / ctx.jobCount -
                                  activeCount * j0 / ctx.jobCount;
        if (part == 0u && partRows != 0u &&
            static_cast<uint64_t>(gpuRun + envRun) * activeCount / partRows <
                stage.MinimumVoices()) {
            // Too little for the GPU to pay off: everything stays on the CPU.
            ++counters.declined[6];
            stage.BumpEpoch();
            return 0u;
        }
        gpuPart_[part] = {sStart, gpuRun - sStart, eStart, envRun - eStart};

        const uint64_t packStart = GpuStage::NowMicros();
        ctx.pass = 2u;
        RunGpuPass(ctx, outputLeft, outputRight);
        stage.MutableStats().packMicros += GpuStage::NowMicros() - packStart;

        const uint64_t uploadStart = GpuStage::NowMicros();
        uint32_t uploads = 0u;
        uint32_t envUploads = 0u;
        bool ok = true;
        for (uint32_t job = j0; job < j1 && ok; ++job) {
            const uint32_t begin = activeCount * job / ctx.jobCount;
            const uint32_t count = stage.JobUploadCount()[job];
            const uint32_t envCount = stage.JobEnvUploadCount()[job];
            uploads += count;
            envUploads += envCount;
            ok = stage.UploadRows(stage.UploadScratch() + begin, count) &&
                 stage.UploadEnvRows(stage.EnvUploadScratch() + begin, envCount);
        }
        stage.MutableStats().uploadMicros += GpuStage::NowMicros() - uploadStart;
        if (ok) {
            SortGpuRows(ctx, stage.List() + sStart, gpuRun - sStart,
                        outputLeft, outputRight);
            SortGpuRows(ctx, stage.EnvList() + eStart, envRun - eStart,
                        outputLeft, outputRight);
        }
        ok = ok && stage.SubmitPart(sStart, gpuRun - sStart, eStart,
                                    envRun - eStart, numFrames, uploads,
                                    envUploads, VoiceRetireThreshold(),
                                    part == 0u, last);
        if (!ok) {
            // Nothing has been folded into the SoA yet, so the whole block
            // simply stays on the CPU.
            ++counters.declined[7];
            stage.Abandon();
            stage.BumpEpoch();
            return 0u;
        }
        ++gpuPartCount_;
        ++gpuSubmittedParts_;
    }
    submitted = true;
    gpuSustainedRows_ = gpuRun;
    gpuEnvRows_ = envRun;
    wvItemHandles_ = stage.CpuRows();
    return gpuRun + envRun;
}

// Waits for the GPU part by part and folds its rows into the block (scattering
// one part while the GPU still runs the next), or renders the rows of a late,
// lost or never-submitted part on the CPU.  `cpuCount` is where the GPU rows
// start in wvSliceHandles_: all sustained rows, then all envelope rows.
inline void RenderScalar::FinishGpuRows(VoiceManager& voices,
                                        const int16_t* sampleData,
                                        uint32_t sampleDataFrames,
                                        float* outputLeft, float* outputRight,
                                        uint32_t numFrames, uint32_t gpuCount,
                                        uint32_t cpuCount, bool submitted) {
    (void)submitted;
    (void)cpuCount;
    GpuStage& stage = *gpuStage_;
    VoiceSoA& v = voices.v;
    bool failed = false;
    for (uint32_t part = 0u; part < gpuPartCount_; ++part) {
        const GpuPart& info = gpuPart_[part];
        if (!failed && part < gpuSubmittedParts_ &&
            stage.Collect(numFrames, part) == gpu::Result::Ok) {
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
            ctx.sustainedCount = info.sCount;
            ctx.envCount = info.eCount;
            ctx.partSBase = info.sBase;
            ctx.partEBase = info.eBase;
            const uint32_t rows = info.sCount + info.eCount;
            uint32_t jobs = 1u;
            if (workerPool_ != nullptr) {
                jobs = (std::min)((std::min)(workerPool_->GetThreadCount() * 2u,
                                             workerPool_->GetJobCapacity()),
                                  GpuStage::kMaxJobs);
                jobs = (std::min)(jobs, (std::max)(1u, rows / 2048u));
            }
            ctx.jobCount = jobs < 2u ? 1u : jobs;
            ctx.jobFirst = 0u;
            ctx.jobEnd = ctx.jobCount;
            RunGpuPass(ctx, outputLeft, outputRight);
            // Retire frames the envelope kernel found join the block's
            // retirement list (job slot 0; the list is sorted by frame and
            // active position afterwards, so the slot does not matter).
            if (info.eCount != 0u) {
                SpanRetirement* retBuf = wvJobRetirements_;
                uint32_t& retCount = wvJobRetireCounts_[0];
                for (uint32_t job = 0u; job < ctx.jobCount; ++job) {
                    const uint32_t begin =
                        info.eBase + info.eCount * job / ctx.jobCount;
                    const GpuStage::RetireRec* recs =
                        stage.RetireScratch() + begin;
                    const uint32_t n = stage.JobRetireCount()[job];
                    for (uint32_t i = 0u; i < n && retCount < scratchCapacity_;
                         ++i) {
                        retBuf[retCount++] = {
                            recs[i].handle, recs[i].frame,
                            voices.activePosition_[recs[i].handle]};
                    }
                    const uint32_t* crossed = stage.CrossScratch() + begin;
                    gpuRefreshHandles_.insert(gpuRefreshHandles_.end(), crossed,
                                              crossed + stage.JobCrossCount()[job]);
                }
            }
            stage.MutableStats().scatterMicros +=
                GpuStage::NowMicros() - scatterStart;
            continue;
        }
        if (!failed) {
            // Late, lost or never submitted: parts already folded in stay (the
            // SoA holds their new state); this part and every later one still
            // hold the pre-block state, so the CPU kernels reproduce them
            // exactly.  The GPU copy of every row is now one block ahead of
            // (or unrelated to) the SoA, so nothing stays resident.
            failed = true;
            stage.Abandon();
            stage.BumpEpoch();
        }
        const IndexedJobMix mix{outputLeft, outputRight, nullptr, nullptr};
        const uint32_t* sRows = stage.List() + info.sBase;
        for (uint32_t offset = 0u; offset < info.sCount; offset += 256u) {
            const uint32_t n = (std::min)(256u, info.sCount - offset);
            if (RenderWholeVoiceBatch(v, voices, sRows + offset, n,
                                      VoiceRenderClass::SustainedLoop, sampleData,
                                      sampleDataFrames, mix, 0u, numFrames, 0u)) {
                continue;
            }
            for (uint32_t i = 0u; i < n; ++i) {
                RenderWholeVoiceSegment(v, &voices, sRows[offset + i], sampleData,
                                        sampleDataFrames, outputLeft, outputRight,
                                        0u, numFrames, true, 0u, 0.0f, 1.0f, false);
            }
        }
        const uint32_t* eRows = stage.EnvList() + info.eBase;
        for (uint32_t i = 0u; i < info.eCount; ++i) {
            RenderWholeVoiceSegment(v, &voices, eRows[i], sampleData,
                                    sampleDataFrames, outputLeft, outputRight,
                                    0u, numFrames, true, 0u, 0.0f, 1.0f, false);
        }
    }
    if (failed) {
        // Crossings of folded parts are covered by refreshing every row.
        gpuRefreshHandles_.clear();
        gpuSkipRefresh_ = 0u;
        wvItemHandles_ = wvSliceHandles_;
    } else {
        gpuSkipRefresh_ = gpuCount;
    }
}

#endif  // SVMS_GPU_STAGE
