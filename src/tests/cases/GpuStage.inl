// ═══════════════════════════════════════════════════════════════════════
// GPU render stage (render/GpuStage.h): sustained-loop rows rendered by the
// GPU must match the CPU renderers.  Phase and filter state are bit-equal to
// the scalar renderer (the shader repeats phase += step per frame with
// `precise` math and the scalar kernel's interior-run bookkeeping); audio is
// tolerance-equal (only the cross-voice sum order differs).  When no GPU API
// initialises on this machine the test prints a skip note.
// ═══════════════════════════════════════════════════════════════════════

#if defined(SVMS_GPU_STAGE)

void FillGpuTestSamples(std::vector<int16_t>& samples) {
    samples.assign(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.4f * std::sin(static_cast<float>(i) * 0.031f) +
             0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);
}

// A pool of sustained loops, a third of them low-pass filtered.  With
// wrapStress, every 29th row steps nearly a whole loop per frame (a wrap on
// almost every frame).
void ConfigureGpuSeedPool(svms::VoiceManager& voices,
                          const svms::ChannelCache& channels,
                          uint32_t poolSize, uint32_t seedCount,
                          bool wrapStress = true) {
    ConfigureWholeVoiceSeedPool(voices, channels, poolSize, seedCount,
                                /*transientSeeds=*/false);
    const svms::PreparedVoiceFilter lowPass = svms::PrepareVoiceLowPass(
        static_cast<uint8_t>(svms::FilterType::LowPass2Pole), 9000, 120, 44100u);
    for (uint32_t i = 0; i < seedCount; ++i) {
        const svms::VoiceHandle h = static_cast<svms::VoiceHandle>(i);
        if (voices.v.state[h] == static_cast<uint8_t>(svms::VoiceState::Free))
            continue;
        if (wrapStress && (i % 29u) == 7u)
            voices.v.phaseIncs[h] = voices.v.basePhaseIncs[h] = 3900.0f;
        if ((i % 3u) != 0u) continue;
        voices.v.filterEnabled[h] = lowPass.enabled;
        voices.v.filterA0[h] = lowPass.a0;
        voices.v.filterB1[h] = lowPass.b1;
        voices.v.filterB2[h] = lowPass.b2;
    }
}

bool CreateGpuStageForTest(std::unique_ptr<svms::GpuStage>& stage,
                           svms::gpu::Api api) {
    std::string error;
    stage = svms::GpuStage::Create(api, 0u, error);
    if (!stage) {
        std::printf("GPU stage differential skipped: %s\n", error.c_str());
        return false;
    }
    stage->SetMinimumVoices(1u);
    stage->SetSplitMinRows(1u);     // any block with two jobs goes out in two parts
    stage->SetDeadlinePercent(0u);
    std::printf("GPU stage differential on: %s\n", stage->Name());
    return true;
}

// Steady sustained pool, no events: GPU rows vs the scalar CPU renderer.
void RunGpuStageSteadyDifferential(svms::GpuStage& stage, uint32_t poolSize,
                                   uint32_t frames, uint32_t blocks,
                                   uint32_t renderThreads) {
    std::vector<int16_t> samples;
    FillGpuTestSamples(samples);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto seed = std::make_unique<svms::VoiceManager>();
    ConfigureGpuSeedPool(*seed, channels, poolSize, poolSize);
    // Float sums grow with the pool; keep the mix amplitude (and with it the
    // accumulation-order error) where the 2e-5 oracle gate is meaningful.
    if (poolSize > 400u) {
        const float k = 400.0f / static_cast<float>(poolSize);
        for (uint32_t i = 0; i < poolSize; ++i) {
            seed->v.mixGainL[i] *= k;
            seed->v.mixGainR[i] *= k;
            seed->v.renderGainL[i] *= k;
            seed->v.renderGainR[i] *= k;
        }
    }
    stage.BumpEpoch();   // a new pool reuses handles and play indices
    auto cpuVoices = std::make_unique<svms::VoiceManager>(*seed);
    auto gpuVoices = std::make_unique<svms::VoiceManager>(*seed);
    svms::ChannelCache cpuChannels = channels;
    svms::ChannelCache gpuChannels = channels;
    svms::RenderScalar cpu;
    svms::RenderScalar gpu;
    // Both sides render their CPU rows with the scalar kernels, so every
    // difference below comes from the GPU rows.
    Check(cpu.ReserveVoiceCapacity(poolSize) && gpu.ReserveVoiceCapacity(poolSize) &&
              cpu.ConfigureRenderThreads(1u, frames) &&
              gpu.ConfigureRenderThreads(renderThreads, frames) &&
              cpu.SetRenderBackend(svms::RenderBackend::Scalar) &&
              gpu.SetRenderBackend(svms::RenderBackend::Scalar),
          "gpu steady differential starts its renderers");
    gpu.SetGpuStage(&stage);
    const uint64_t rowsBefore = stage.Stats().rows;
    float maxDiff = 0.0f;
    for (uint32_t block = 0; block < blocks; ++block) {
        std::vector<float> cl(frames, 0.0f), cr(frames, 0.0f);
        std::vector<float> gl(frames, 0.0f), gr(frames, 0.0f);
        const uint64_t start = 20000u + static_cast<uint64_t>(block) * frames;
        cpu.RenderBlock(*cpuVoices, cpuChannels, samples.data(), 4096u,
                        cl.data(), cr.data(), frames, cfg, nullptr, 0u, true, start);
        gpu.RenderBlock(*gpuVoices, gpuChannels, samples.data(), 4096u,
                        gl.data(), gr.data(), frames, cfg, nullptr, 0u, true, start);
        for (uint32_t f = 0; f < frames; ++f) {
            maxDiff = (std::max)(maxDiff, std::fabs(cl[f] - gl[f]));
            maxDiff = (std::max)(maxDiff, std::fabs(cr[f] - gr[f]));
        }
    }
    uint32_t phaseMismatch = 0u;
    uint32_t filterMismatch = 0u;
    for (uint32_t i = 0; i < gpuVoices->activeCount_; ++i) {
        const uint32_t h = gpuVoices->activeList_[i];
        if (std::memcmp(&gpuVoices->v.phases[h], &cpuVoices->v.phases[h],
                        sizeof(float)) != 0) ++phaseMismatch;
        if (std::memcmp(&gpuVoices->v.filterZ1[h], &cpuVoices->v.filterZ1[h],
                        sizeof(float)) != 0 ||
            std::memcmp(&gpuVoices->v.filterZ2[h], &cpuVoices->v.filterZ2[h],
                        sizeof(float)) != 0) ++filterMismatch;
    }
    const uint64_t gpuRows = stage.Stats().rows - rowsBefore;
    std::fprintf(stderr,
        "[GPUDIFF] pool=%u frames=%u blocks=%u threads=%u audio max=%g "
        "phase-mismatch=%u filter-mismatch=%u gpu-rows=%llu\n",
        poolSize, frames, blocks, renderThreads, maxDiff, phaseMismatch,
        filterMismatch, static_cast<unsigned long long>(gpuRows));
    Check(gpuRows > 0u, "gpu steady differential rendered rows on the GPU");
    Check(maxDiff <= g_waveTol, "gpu stage audio matches the scalar renderer");
    Check(phaseMismatch == 0u, "gpu stage phases are bit-equal to scalar");
    Check(filterMismatch == 0u, "gpu stage filter state is bit-equal to scalar");
}

// One row at a time with its phase placed just before the loop end, big and
// small steps, with and without the filter: the wrap bookkeeping must pick the
// same sample as the scalar kernel on every frame.
void RunGpuStageWrapSweep(svms::GpuStage& stage) {
    std::vector<int16_t> samples;
    FillGpuTestSamples(samples);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    const svms::PreparedVoiceFilter lowPass = svms::PrepareVoiceLowPass(
        static_cast<uint8_t>(svms::FilterType::LowPass2Pole), 9000, 120, 44100u);
    WholeVoiceRng rng;
    uint32_t bad = 0u;
    constexpr uint32_t kTrials = 300u;
    for (uint32_t trial = 0; trial < kTrials; ++trial) {
        stage.BumpEpoch();
        auto seed = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seed, channels, 1u, 1u, false);
        const bool bigStep = (trial % 3u) == 2u;
        const float step = bigStep ? 3000.0f + static_cast<float>(rng.Next() % 960u)
                                   : 0.3f + static_cast<float>(rng.Next() % 2000u) * 0.001f;
        seed->v.phases[0] = 4032.0f - static_cast<float>(rng.Next() % 4000u) * 0.01f;
        seed->v.phaseIncs[0] = seed->v.basePhaseIncs[0] = step;
        if ((trial % 2u) == 1u) {
            seed->v.filterEnabled[0] = lowPass.enabled;
            seed->v.filterA0[0] = lowPass.a0;
            seed->v.filterB1[0] = lowPass.b1;
            seed->v.filterB2[0] = lowPass.b2;
        }
        auto cpuVoices = std::make_unique<svms::VoiceManager>(*seed);
        auto gpuVoices = std::make_unique<svms::VoiceManager>(*seed);
        svms::ChannelCache cpuChannels = channels;
        svms::ChannelCache gpuChannels = channels;
        svms::RenderScalar cpu;
        svms::RenderScalar gpu;
        cpu.ReserveVoiceCapacity(1u);
        gpu.ReserveVoiceCapacity(1u);
        cpu.ConfigureRenderThreads(1u, 64u);
        gpu.ConfigureRenderThreads(1u, 64u);
        cpu.SetRenderBackend(svms::RenderBackend::Scalar);
        gpu.SetGpuStage(&stage);
        std::vector<float> cl(64, 0.0f), cr(64, 0.0f), gl(64, 0.0f), gr(64, 0.0f);
        cpu.RenderBlock(*cpuVoices, cpuChannels, samples.data(), 4096u,
                        cl.data(), cr.data(), 64u, cfg, nullptr, 0u, true, 0u);
        gpu.RenderBlock(*gpuVoices, gpuChannels, samples.data(), 4096u,
                        gl.data(), gr.data(), 64u, cfg, nullptr, 0u, true, 0u);
        float worst = 0.0f;
        for (uint32_t f = 0; f < 64u; ++f)
            worst = (std::max)(worst, (std::max)(std::fabs(cl[f] - gl[f]),
                                                 std::fabs(cr[f] - gr[f])));
        const bool stateEqual =
            std::memcmp(&cpuVoices->v.phases[0], &gpuVoices->v.phases[0],
                        sizeof(float)) == 0 &&
            std::memcmp(&cpuVoices->v.filterZ1[0], &gpuVoices->v.filterZ1[0],
                        sizeof(float)) == 0 &&
            std::memcmp(&cpuVoices->v.filterZ2[0], &gpuVoices->v.filterZ2[0],
                        sizeof(float)) == 0;
        if (worst > 1.0e-7f || !stateEqual) {
            if (bad++ < 8u)
                std::fprintf(stderr,
                    "[GPUDIFF] wrap sweep trial %u diff=%g state-equal=%d\n",
                    trial, worst, stateEqual ? 1 : 0);
        }
    }
    std::fprintf(stderr, "[GPUDIFF] wrap sweep bad=%u of %u\n", bad, kTrials);
    Check(bad == 0u, "gpu stage wrap handling matches the scalar kernel");
}

// Events, ops, steals and releases around GPU rows: every block mixes rows
// that enter and leave the GPU set; one block is forced late (CPU fallback)
// and one bumps the epoch (full re-upload).
void RunGpuStageEventDifferential(svms::GpuStage& stage) {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 10u;
    constexpr uint32_t kPool = 64u;
    constexpr uint32_t kEventsPerBlock = 10u;
    std::vector<int16_t> samples;
    FillGpuTestSamples(samples);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache seedChannels;
    seedChannels.SetMasterVolume(1.0f);
    seedChannels.RebuildCache(cfg, 44100.0f);
    auto seedVoices = std::make_unique<svms::VoiceManager>();
    // 40 seeds in a pool of 64: room for launches to grow into.  (No
    // wrap-stress rows: their phase sits unwrapped at a block end in the CPU
    // renderers and wrapped in the oracle, which the parity check flags.)
    ConfigureGpuSeedPool(*seedVoices, seedChannels, kPool, 40u, false);
    stage.BumpEpoch();
    auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
    auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
    svms::ChannelCache oracleChannels = seedChannels;
    svms::ChannelCache wholeChannels = seedChannels;
    svms::RenderScalar oracle;
    svms::RenderScalar whole;
    Check(oracle.ReserveVoiceCapacity(kPool) && whole.ReserveVoiceCapacity(kPool) &&
              oracle.ConfigureRenderThreads(1u, kFrames) &&
              whole.ConfigureRenderThreads(4u, kFrames),
          "gpu event differential starts its renderers");
    whole.SetGpuStage(&stage);
    WholeVoiceCCContext oracleContext{oracleVoices.get(), &oracleChannels,
                                      oracleChannels.GetParams(), &cfg};
    WholeVoiceCCContext wholeContext{wholeVoices.get(), &wholeChannels,
                                     wholeChannels.GetParams(), &cfg};
    oracleContext.ResetMasterState();
    wholeContext.ResetMasterState();
    oracle.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent, &oracleContext);
    whole.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent, &wholeContext);
    const uint64_t rowsBefore = stage.Stats().rows;
    const uint64_t lateBefore = stage.Stats().late;
    const uint64_t uploadsBefore = stage.Stats().uploads;

    WholeVoiceRng rng;
    float maxDiff = 0.0f;
    for (uint32_t block = 0; block < kBlocks; ++block) {
        if (block == 3u) stage.ForceLateForTest(1u);
        if (block == 6u) stage.BumpEpoch();
        std::vector<svms::RenderEvent> events;
        for (uint32_t j = 0; j < kEventsPerBlock; ++j) {
            svms::RenderEvent event{};
            event.channel = static_cast<uint8_t>(rng.Next() % 4u);
            event.frameOffset = rng.Next() % kFrames;
            event.ingressSequence = block * kEventsPerBlock + j;
            // Blocks 1 and 4 have no events at all: pure GPU blocks after a
            // block of ops, which re-uploads the rows those ops touched.
            if (block == 1u || block == 4u) continue;
            const uint32_t roll = rng.Next() % 8u;
            if (roll < 3u) {
                event.type = svms::RenderEventType::NoteOn;
                event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
                event.data2 = static_cast<uint8_t>(64u + rng.Next() % 64u);
            } else if (roll < 5u) {
                event.type = svms::RenderEventType::NoteOff;
                event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
            } else if (roll == 5u) {
                event.type = svms::RenderEventType::PitchBend;
                event.data1 = static_cast<uint8_t>(rng.Next() % 128u);
                event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
            } else if (roll == 6u) {
                static const uint8_t kMix[3] = {7u, 10u, 11u};
                event.type = svms::RenderEventType::ControlChange;
                event.data1 = kMix[rng.Next() % 3u];
                event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
            } else {
                event.type = svms::RenderEventType::ControlChange;
                event.data1 = 123u;
            }
            events.push_back(event);
        }
        std::stable_sort(events.begin(), events.end(),
            [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                return a.frameOffset < b.frameOffset;
            });
        std::vector<float> ol(kFrames, 0.0f), orr(kFrames, 0.0f);
        std::vector<float> wl(kFrames, 0.0f), wr(kFrames, 0.0f);
        const uint64_t start = 20000u + static_cast<uint64_t>(block) * kFrames;
        oracle.RenderBlockReference(*oracleVoices, oracleChannels,
            samples.data(), 4096u, ol.data(), orr.data(), kFrames, cfg,
            events.data(), static_cast<uint32_t>(events.size()), true, start);
        whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(), 4096u,
            wl.data(), wr.data(), kFrames, cfg, events.data(),
            static_cast<uint32_t>(events.size()), true, start);
        for (uint32_t f = 0; f < kFrames; ++f) {
            maxDiff = (std::max)(maxDiff, std::fabs(ol[f] - wl[f]));
            maxDiff = (std::max)(maxDiff, std::fabs(orr[f] - wr[f]));
        }
    }
    const svms::GpuStageStats& stats = stage.Stats();
    std::fprintf(stderr,
        "[GPUDIFF] events max=%g gpu-rows=%llu uploads=%llu late=%llu\n",
        maxDiff, static_cast<unsigned long long>(stats.rows - rowsBefore),
        static_cast<unsigned long long>(stats.uploads - uploadsBefore),
        static_cast<unsigned long long>(stats.late - lateBefore));
    Check(stats.rows > rowsBefore, "gpu event differential used the GPU");
    Check(stats.late - lateBefore == 1u, "forced-late block fell back to the CPU");
    Check(maxDiff <= g_waveTol, "gpu stage blocks with events match the oracle");
    CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, "gpu-stage");
}

// Envelope rows: release tails (counted and threshold-governed, loop and
// one-shot), attack/decay stages that cross into sustain mid-block, and
// one-shots that run off the end of their sample.  The GPU rows must match
// the scalar renderer's generic span machine: every piece of state bit-equal
// (phase, gain, stage and counters, filter), the same voices retired, audio
// within the oracle tolerance.  One block is forced late (CPU fallback) and
// one bumps the epoch (full re-upload).
void ConfigureGpuEnvelopePool(svms::VoiceManager& voices,
                              const svms::ChannelCache& channels,
                              uint32_t poolSize) {
    Check(voices.Initialize(poolSize, 44100u),
          "gpu envelope pool allocates voice storage");
    const svms::PreparedVoiceFilter lowPass = svms::PrepareVoiceLowPass(
        static_cast<uint8_t>(svms::FilterType::LowPass2Pole), 9000, 120, 44100u);
    for (uint32_t i = 0; i < poolSize; ++i) {
        const uint8_t channel = static_cast<uint8_t>(i & 3u);
        const svms::VoiceHandle voice = voices.AllocateVoice(
            channel, static_cast<uint8_t>(48u + i % 24u),
            static_cast<uint8_t>(48u + i % 64u));
        const uint32_t type = i % 8u;
        const bool loop = type != 3u && type != 5u && type != 6u;
        float step = 0.4f + static_cast<float>(i % 17u) * 0.11f;
        float phase = static_cast<float>((i * 37u) % 1700u) + 0.375f;
        if (!loop) {
            // One-shots: some run off the end of the sample inside the
            // blocks, some do not.
            step = 0.6f + static_cast<float>(i % 5u) * 0.2f;
            phase = 4096.0f - 1.0f - static_cast<float>(300u + (i % 9u) * 90u);
        }
        voices.SetVoiceSample(voice, 0, 4096, 64, 4032, loop ? 1u : 0u, step, 1);
        voices.v.phases[voice] = phase;
        const float gain = 0.02f * (1.0f + 0.01f * static_cast<float>(i % 50u));
        switch (type) {
        case 1u:   // release loop, threshold-governed tail
            voices.SetVoiceEnvelope(voice, gain, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f,
                                    0.985f);
            break;
        case 2u:   // release loop, counted tail (some end inside a block)
            voices.SetVoiceEnvelope(voice, gain, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f,
                                    0.9995f, 150u + (i % 7u) * 400u);
            break;
        case 3u:   // release one-shot
            voices.SetVoiceEnvelope(voice, gain, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f,
                                    0.9999f);
            break;
        case 4u:   // attack then decay, crossing into sustain mid-run
            voices.SetVoiceEnvelope(voice, gain, 0.35f, 0, 0,
                                    200u + (i % 5u) * 150u, 500u + (i % 3u) * 400u,
                                    gain / 300.0f, 0.9993f, 0.999f);
            break;
        case 5u:   // decay-only one-shot
            voices.SetVoiceEnvelope(voice, gain, 0.4f, 0, 0, 0,
                                    300u + (i % 4u) * 300u, 0.0f, 0.9994f,
                                    0.999f);
            break;
        default:
            voices.SetVoiceEnvelope(voice, gain, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f,
                                    0.999f);
            break;
        }
        voices.SetVoicePlayIndex(voice, i + 1u);
        voices.SetVoiceGain(voice, 0.02f, 0.02f);
        voices.RefreshMixGain(voice, channels.GetParams()[channel]);
        if (type >= 1u && type <= 3u) voices.StartRelease(voice);
        if ((i % 3u) == 0u) {
            voices.v.filterEnabled[voice] = lowPass.enabled;
            voices.v.filterA0[voice] = lowPass.a0;
            voices.v.filterB1[voice] = lowPass.b1;
            voices.v.filterB2[voice] = lowPass.b2;
        }
    }
}

void RunGpuStageEnvelopeDifferential(svms::GpuStage& stage, uint32_t poolSize,
                                     uint32_t frames, uint32_t blocks,
                                     uint32_t renderThreads) {
    std::vector<int16_t> samples;
    FillGpuTestSamples(samples);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto seed = std::make_unique<svms::VoiceManager>();
    ConfigureGpuEnvelopePool(*seed, channels, poolSize);
    stage.BumpEpoch();
    auto cpuVoices = std::make_unique<svms::VoiceManager>(*seed);
    auto gpuVoices = std::make_unique<svms::VoiceManager>(*seed);
    svms::ChannelCache cpuChannels = channels;
    svms::ChannelCache gpuChannels = channels;
    svms::RenderScalar cpu;
    svms::RenderScalar gpu;
    Check(cpu.ReserveVoiceCapacity(poolSize) && gpu.ReserveVoiceCapacity(poolSize) &&
              cpu.ConfigureRenderThreads(1u, frames) &&
              gpu.ConfigureRenderThreads(renderThreads, frames) &&
              cpu.SetRenderBackend(svms::RenderBackend::Scalar) &&
              gpu.SetRenderBackend(svms::RenderBackend::Scalar),
          "gpu envelope differential starts its renderers");
    gpu.SetGpuStage(&stage);
    const uint64_t envBefore = stage.Stats().envRows;
    const uint64_t lateBefore = stage.Stats().late;
    const uint32_t startActive = cpuVoices->activeCount_;
    float maxDiff = 0.0f;
    uint32_t stateMismatch = 0u;
    uint32_t listMismatch = 0u;
    for (uint32_t block = 0; block < blocks; ++block) {
        if (block == 3u) stage.ForceLateForTest(1u);
        // Second part late (the first part was folded in, only the second is
        // rendered by the CPU) - only blocks that really go out in two parts.
        if (block == 4u && poolSize >= 6000u) stage.ForceLateSecondForTest(1u);
        if (block == 5u) stage.BumpEpoch();
        if (block == 7u) {
            // A 4-frame stretch (too short for the stage) renders every row on
            // the CPU between two GPU blocks: residency must not survive it.
            std::vector<float> sl(4, 0.0f), sr(4, 0.0f), tl(4, 0.0f), tr(4, 0.0f);
            cpu.RenderBlock(*cpuVoices, cpuChannels, samples.data(), 4096u,
                            sl.data(), sr.data(), 4u, cfg, nullptr, 0u, true, 19000u);
            gpu.RenderBlock(*gpuVoices, gpuChannels, samples.data(), 4096u,
                            tl.data(), tr.data(), 4u, cfg, nullptr, 0u, true, 19000u);
        }
        std::vector<float> cl(frames, 0.0f), cr(frames, 0.0f);
        std::vector<float> gl(frames, 0.0f), gr(frames, 0.0f);
        const uint64_t start = 20000u + static_cast<uint64_t>(block) * frames;
        cpu.RenderBlock(*cpuVoices, cpuChannels, samples.data(), 4096u,
                        cl.data(), cr.data(), frames, cfg, nullptr, 0u, true, start);
        gpu.RenderBlock(*gpuVoices, gpuChannels, samples.data(), 4096u,
                        gl.data(), gr.data(), frames, cfg, nullptr, 0u, true, start);
        for (uint32_t f = 0; f < frames; ++f) {
            maxDiff = (std::max)(maxDiff, std::fabs(cl[f] - gl[f]));
            maxDiff = (std::max)(maxDiff, std::fabs(cr[f] - gr[f]));
        }
        if (cpuVoices->activeCount_ != gpuVoices->activeCount_) {
            ++listMismatch;
            continue;
        }
        for (uint32_t i = 0; i < cpuVoices->activeCount_; ++i) {
            const uint32_t h = cpuVoices->activeList_[i];
            if (gpuVoices->activeList_[i] != h) { ++listMismatch; continue; }
            const svms::VoiceSoA& a = cpuVoices->v;
            const svms::VoiceSoA& b = gpuVoices->v;
            const bool same =
                std::memcmp(&a.phases[h], &b.phases[h], sizeof(float)) == 0 &&
                std::memcmp(&a.currentGain[h], &b.currentGain[h], sizeof(float)) == 0 &&
                std::memcmp(&a.filterZ1[h], &b.filterZ1[h], sizeof(float)) == 0 &&
                std::memcmp(&a.filterZ2[h], &b.filterZ2[h], sizeof(float)) == 0 &&
                std::memcmp(&a.renderGainL[h], &b.renderGainL[h], sizeof(float)) == 0 &&
                std::memcmp(&a.renderGainR[h], &b.renderGainR[h], sizeof(float)) == 0 &&
                a.state[h] == b.state[h] &&
                a.envelopeStage[h] == b.envelopeStage[h] &&
                a.attackSamplesRemaining[h] == b.attackSamplesRemaining[h] &&
                a.decaySamplesRemaining[h] == b.decaySamplesRemaining[h] &&
                a.releaseSamplesRemaining[h] == b.releaseSamplesRemaining[h];
            if (!same && stateMismatch++ < 6u) {
                std::fprintf(stderr,
                    "[GPUDIFF] env block %u handle %u: phase %g/%g gain %g/%g "
                    "stage %u/%u atk %u/%u dec %u/%u rel %u/%u\n",
                    block, h, a.phases[h], b.phases[h], a.currentGain[h],
                    b.currentGain[h], a.envelopeStage[h], b.envelopeStage[h],
                    a.attackSamplesRemaining[h], b.attackSamplesRemaining[h],
                    a.decaySamplesRemaining[h], b.decaySamplesRemaining[h],
                    a.releaseSamplesRemaining[h], b.releaseSamplesRemaining[h]);
            }
        }
    }
    const uint64_t envRows = stage.Stats().envRows - envBefore;
    std::fprintf(stderr,
        "[GPUDIFF] envelope pool=%u frames=%u blocks=%u threads=%u audio max=%g "
        "state-mismatch=%u list-mismatch=%u env-rows=%llu retired=%u late=%llu\n",
        poolSize, frames, blocks, renderThreads, maxDiff, stateMismatch,
        listMismatch, static_cast<unsigned long long>(envRows),
        startActive - cpuVoices->activeCount_,
        static_cast<unsigned long long>(stage.Stats().late - lateBefore));
    Check(envRows > 0u, "gpu envelope differential rendered rows on the GPU");
    Check(startActive > cpuVoices->activeCount_,
          "gpu envelope differential retired voices");
    stage.ForceLateSecondForTest(0u);
    if (poolSize >= 6000u) {
        Check(stage.Stats().late - lateBefore == 2u,
              "both a first-part and a second-part late block fell back");
    }
    Check(maxDiff <= g_waveTol, "gpu envelope audio matches the scalar renderer");
    Check(stateMismatch == 0u, "gpu envelope state is bit-equal to scalar");
    Check(listMismatch == 0u, "gpu envelope retirements match the scalar renderer");
}

void RunGpuStageDifferentialSuite(svms::gpu::Api api) {
    std::unique_ptr<svms::GpuStage> stage;
    if (!CreateGpuStageForTest(stage, api)) return;
    RunGpuStageSteadyDifferential(*stage, 200u, 512u, 12u, 1u);
    RunGpuStageSteadyDifferential(*stage, 6000u, 2048u, 6u, 4u);
    RunGpuStageSteadyDifferential(*stage, 3000u, 480u, 6u, 4u);
    RunGpuStageEnvelopeDifferential(*stage, 400u, 256u, 16u, 1u);
    RunGpuStageEnvelopeDifferential(*stage, 20000u, 512u, 8u, 4u);
    RunGpuStageWrapSweep(*stage);
    RunGpuStageEventDifferential(*stage);
    Check(!stage->Stats().lost && stage->Stats().failed == 0u,
          "gpu stage never lost the device or failed a submit");
}

// Every backend runs the same gates against the scalar renderer.
void TestGpuStageDifferential() {
    RunGpuStageDifferentialSuite(svms::gpu::Api::OpenGL);
    RunGpuStageDifferentialSuite(svms::gpu::Api::CUDA);
}

#else
void TestGpuStageDifferential() {}
#endif
