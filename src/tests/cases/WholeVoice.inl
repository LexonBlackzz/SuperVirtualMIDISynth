// Test cases split out of V3Tests.cpp; included inside its anonymous namespace.

void TestLaunchChurnInstrumentation() {
    auto voices = std::make_unique<svms::VoiceManager>();
    Check(voices->Initialize(1u, 44100u),
          "launch churn fixture allocates one voice");
    voices->SetLaunchChurnProfilingEnabledForTest(true);
    svms::ChannelParamsSnapshot channel{};
    channel.volume = channel.expression = 1.0f;
    channel.panLeft = channel.panRight = 0.70710678f;
    channel.mixScaleLeft = channel.mixScaleRight = 0.70710678f;
    svms::VoiceConfiguration setup{};
    setup.sampleStart = 0u;
    setup.sampleEnd = 128u;
    setup.loopStart = 8u;
    setup.loopEnd = 120u;
    setup.loopMode = 1u;
    setup.phaseStep = setup.basePhaseStep = 1.0f;
    setup.initialGain = setup.sustainLevel = 1.0f;
    setup.releaseDecay = 0.999f;
    setup.gainLeft = setup.gainRight = 0.1f;
    setup.presetIndex = 0u;
    setup.regionIndex = 7u;
    svms::VoiceHandle handle = svms::kInvalidVoice;
    Check(voices->LaunchVoiceGroup(0u, 60u, 127u, &setup, 1u, 1u,
                                   channel, &handle),
          "launch churn fixture fills its free slot");

    voices->SetCurrentFrame(64u);
    voices->ResetGroupReuseCountersForTest();
    Check(voices->LaunchVoiceGroup(0u, 60u, 127u, &setup, 1u, 2u,
                                   channel, &handle) &&
              voices->LaunchVoiceGroup(0u, 60u, 127u, &setup, 1u, 3u,
                                       channel, &handle),
          "launch churn fixture performs previous-frame and same-frame steals");
    voices->SetCurrentFrame(65u);
    const svms::LaunchChurnStats& stats =
        voices->GetLaunchChurnStatsForTest();
    Check(stats.logicalLaunches == 2u && stats.successfulLaunches == 2u &&
              stats.physicalVoicesRequested == 2u &&
              stats.physicalVoicesConfigured == 2u,
          "launch churn separates logical and physical launch denominators");
    Check(stats.stealTransactions == 2u && stats.victimGroups == 2u &&
              stats.physicalVictims == 2u &&
              stats.sameFrameVictimGroups == 1u &&
              stats.sameFramePhysicalVictims == 1u,
          "launch churn identifies exact zero-sample victim replacement");
    Check(stats.monoVictimGroups == 2u &&
              stats.stableVictimGroups == 2u &&
              stats.matchingSizeVictimGroups == 2u &&
              stats.matchingPlanVictimGroups == 2u &&
              stats.singleInPlaceVictimGroups == 2u,
          "launch churn classifies stable matching in-place victims");
    Check(stats.nextFrameSurvivingGroups == 1u &&
              stats.nextFrameSurvivingPhysicalVoices == 1u,
          "launch churn counts only the final same-frame survivor");
    Check(stats.tailCaptureAttempts == 2u &&
              stats.tailCaptureAccepted + stats.tailCaptureRejected +
                      stats.tailCaptureIneligible ==
                  stats.tailCaptureAttempts,
          "launch churn accounts for every outgoing-tail decision");
}

void TestVibratoModulationAudible() {
    {
        // Regions compiled without any vibrato generator must still carry
        // the SF2 built-in default modulator depth (50 cents) and the
        // default 1 Hz LFO rate, or stock SoundFonts stay static.
        auto data = std::make_unique<svms::SF2Data>();
        std::memset(data.get(), 0, sizeof(svms::SF2Data));
        data->presetCount = 1;
        data->instrumentCount = 1;
        data->sampleCount = 1;
        data->sampleDataFrames = 256;
        data->presets[0].zoneIndex = 0;
        data->instruments[0].zoneIndex = 0;
        data->samples[0].start = 0;
        data->samples[0].end = 255;
        data->samples[0].sampleRate = 44100;
        data->samples[0].originalPitch = 60;
        data->presetZones[0].generatorIndex = 0;
        data->presetZones[1].generatorIndex = 1;
        data->presetZoneCount = 2;
        data->generators[0] = {svms::Gen_Instrument, 0};
        data->pgenCount = 2;
        data->instrumentZones[0].generatorIndex = 2;
        data->instrumentZones[1].generatorIndex = 3;
        data->instrumentZoneCount = 2;
        data->generators[2] = {svms::Gen_SampleID, 0};
        data->generatorCount = 4;
        svms::sf2_build_regions(data.get());
        Check(data->regionCount == 1 &&
                  data->regions[0].vibLfoToPitch == 50 &&
                  data->regions[0].freqVibLfo == 0,
              "regions without vibrato generators use SF2 default modulator values");
    }

    constexpr uint32_t kSampleFrames = 64u;
    int16_t sampleBuf[kSampleFrames + 8u]{};
    for (uint32_t i = 0; i < kSampleFrames; ++i) {
        sampleBuf[i] = static_cast<int16_t>(std::sin(static_cast<float>(i) *
            (6.283185307179586f / 64.0f)) * 0.5f * 32767.0f);
    }

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;

    auto renderPass = [&](bool modulated, std::vector<float>& out) {
        auto voices = std::make_unique<svms::VoiceManager>();
        voices->Initialize(4, 44100);
        const svms::VoiceHandle handle = voices->AllocateVoice(0, 60, 100);
        voices->SetVoiceSample(handle, 0, kSampleFrames - 1u, 0,
                               kSampleFrames - 1u, 1, 1.0f, 1);
        voices->SetVoiceEnvelope(handle, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->SetVoiceGain(handle, 1.0f, 1.0f);
        // The driver seeds these from the prepared region at note-on; the
        // direct-launch fixture mirrors that with the resolved defaults.
        voices->v.vibLfoToPitchCents[handle] = 50.0f;
        voices->v.vibLfoSteps[handle] = 5.0f / 44100.0f;

        svms::ChannelCache channels;
        channels.SetMasterVolume(1.0f);
        if (modulated)
            channels.ControlChange(0, 1, 127);
        channels.RebuildCache(cfg, 44100.0f);
        voices->RefreshMixGain(handle, channels.GetParams()[0]);
        if (modulated) {
            Check(channels.GetParams()[0].modDepth > 0.99f,
                  "diagnostic: CC1 reaches the channel modulation depth");
        }

        out.clear();
        out.reserve(4096u);
        svms::RenderScalar renderer;
        float blockLeft[kSampleFrames]{}, blockRight[kSampleFrames]{};
        for (uint32_t block = 0; block < 64u; ++block) {
            renderer.RenderBlock(*voices, channels, sampleBuf, kSampleFrames,
                                 blockLeft, blockRight, kSampleFrames, cfg);
            if (modulated && block == 32u) {
                Check(std::fabs(voices->v.phaseIncs[handle] -
                                voices->v.basePhaseIncs[handle]) > 1.0e-9f,
                      "diagnostic: modulated span rewrites the voice phase increment");
            }
            out.insert(out.end(), blockLeft, blockLeft + kSampleFrames);
        }
    };

    std::vector<float> plain, plainAgain, modulated;
    renderPass(false, plain);
    renderPass(false, plainAgain);
    Check(plain == plainAgain,
          "unmodulated vibrato gating leaves output byte-identical");

    renderPass(true, modulated);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < plain.size(); ++i) {
        maxDiff = (std::max)(maxDiff,
                             std::fabs(plain[i] - modulated[i]));
    }
    Check(maxDiff > 0.01f,
          "full-scale CC1 produces audible vibrato deviation");
}

void TestNoteOnCollapseGate() {
    svms::NoteOnCollapseGate gate;
    // DEFAULT STATE: coalescing is OFF. Every hit spawns — retrigger
    // timing/buzz precision is untouched unless explicitly enabled.
    Check(!gate.Enabled(), "coalescing defaults to off");
    {
        uint32_t stack = 0;
        bool allSpawn = true;
        for (uint32_t i = 0; i < 1000u; ++i) {
            allSpawn = allSpawn && gate.OnNoteOn(7u, 1000u + i, stack);
        }
        Check(allSpawn, "default gate spawns every note-on");
    }

    // Threshold setter: power-of-two accept, round-down, disable.
    gate.SetThreshold(128u);
    Check(gate.Threshold() == 128u,
          "threshold setter accepts a power of two");
    gate.SetThreshold(100u);
    Check(gate.Threshold() == 64u, "threshold rounds down to a power of two");
    gate.SetThreshold(1u);
    Check(gate.Threshold() == 1u, "threshold 1 disables coalescing");

    // Enable with a fixed QPC window (10 ms at 1 tick = 1 microsecond in
    // this test's synthetic clock, i.e. windowTicks = 10000).
    gate.SetThreshold(128u);
    const uint64_t kWin = 10000u;
    gate.SetWindowTicks(kWin);
    Check(gate.Enabled(), "gate enables with threshold and window set");

    // First strike on a key always spawns (latency must be preserved).
    Check(gate.OnNoteOn(0u, 1000u), "first note-on on a key spawns a voice");

    // Hits inside the same window: exactly one more spawn at the
    // threshold crossing (hit 128); everything else collapses.
    uint32_t spawns = 0;
    for (uint32_t i = 1u; i < 200u; ++i) {
        // Timestamps inside the first window (anchor 1000, window 10000).
        if (gate.OnNoteOn(0u, 1000u + i)) ++spawns;
    }
    Check(spawns == 1u, "a key inside one window spawns once per threshold repeats");

    // Frame-size independence: the window is pure time. The crossing
    // spawn at hit 128 re-anchored the window at qpc 1128, so expiry is
    // 1128 + window. A hit past that spawns immediately, no matter how
    // "blocks" would fall.
    Check(gate.OnNoteOn(0u, 1128u + kWin + 1u),
          "a hit after the window expires spawns immediately");

    // A different key is independent and still spawns on its first hit.
    Check(gate.OnNoteOn(1u, 11000u), "a different key spawns on its first hit");

    // Note-off reset clears the key: the next hit spawns immediately.
    gate.ResetKey(0u);
    Check(gate.OnNoteOn(0u, 11500u), "note-off reset makes the next hit spawn");

    // Channel reset clears the whole channel row.
    gate.OnNoteOn(2u * svms::kNoteCount + 60u, 12000u);
    gate.OnNoteOn(2u * svms::kNoteCount + 61u, 12001u);
    gate.ResetChannel(2u);
    Check(gate.OnNoteOn(2u * svms::kNoteCount + 60u, 12002u) &&
              gate.OnNoteOn(2u * svms::kNoteCount + 61u, 12003u),
          "channel reset makes every key on the channel spawn");

    // Reset-all clears even the last key index.
    gate.OnNoteOn(svms::NoteOnCollapseGate::kKeyCount - 1u, 13000u);
    gate.ResetAll();
    Check(gate.OnNoteOn(svms::NoteOnCollapseGate::kKeyCount - 1u, 13001u),
          "reset-all makes any key spawn");

    // Velocity stacking: hits between spawns accumulate per key and are
    // reported to the caller so density can become loudness.
    gate.ResetAll();
    gate.SetThreshold(4u);
    uint32_t stack = 0;
    Check(gate.OnNoteOn(9u, 20000u, stack) && stack == 1u,
          "first hit spawns with stack 1");
    Check(!gate.OnNoteOn(9u, 20001u, stack) && stack == 1u,
          "second hit collapses with running stack 1");
    Check(!gate.OnNoteOn(9u, 20002u, stack) && stack == 2u,
          "third hit collapses with running stack 2");
    Check(!gate.OnNoteOn(9u, 20003u, stack) && stack == 3u,
          "fourth hit collapses with running stack 3");
    Check(gate.OnNoteOn(9u, 20004u, stack) && stack == 4u,
          "threshold crossing spawns with the accumulated stack");
    Check(!gate.OnNoteOn(9u, 20005u, stack) && stack == 1u,
          "a new cycle starts right after a spawn");
    // A different key's stack is untouched by the hammering above.
    Check(gate.OnNoteOn(10u, 20006u, stack) && stack == 1u,
          "stacks are tracked per key");
    // Reset clears the stack too.
    gate.OnNoteOn(11u, 20007u);
    gate.OnNoteOn(11u, 20008u, stack);
    gate.ResetKey(11u);
    Check(gate.OnNoteOn(11u, 20009u, stack) && stack == 1u,
          "note-off reset clears the accumulated stack");

    // Sustained hammering within a continuously re-anchored window keeps
    // the 1-in-N ratio exact (each spawn re-anchors the window, so the
    // rate is deterministic regardless of buffer size).
    gate.ResetAll();
    gate.SetThreshold(128u);
    const uint32_t threshold = 128u;
    uint64_t totalSpawns = 0;
    const uint32_t kHammerHits = threshold * 10u + 5u;
    for (uint32_t i = 0; i < kHammerHits; ++i) {
        if (gate.OnNoteOn(42u, 30000u + i)) ++totalSpawns;
    }
    // Hit 0 spawns (new window), plus every threshold crossing while the
    // re-anchored window still covers the stream (10 crossings for 1285
    // hits at threshold 128; the tail re-anchors keep counting).
    Check(totalSpawns == 1u + (kHammerHits - 1u) / threshold,
          "spawn ratio over sustained hammering is 1 per threshold");

    // Window 0 disables collapsing even with a threshold set.
    gate.SetWindowTicks(0u);
    Check(gate.OnNoteOn(50u, 40000u) && gate.OnNoteOn(50u, 40001u),
          "window 0 forces every hit to spawn");
}

// ═══════════════════════════════════════════════════════════════════════
// Whole-voice whole-block renderer differentials
//
// The whole-voice renderer dispatches every event in ingress order during
// a pre-pass (with the render clock at each event's exact frame) and then
// renders each voice once for the whole block.  The oracle renders
// frame-major, dispatching at the same exact frames mid-render.  Steal
// victim choice therefore sees block-start gains in the pre-pass where
// the oracle sees already-attacked gains after an in-block launch — an
// accepted, documented semantic change (see the RenderScalar.h
// header comment).  Both scenarios below keep victim choice drift-proof:
// no candidate changes gain mid-block, so with one steal per block both
// sides choose identical victims.
//
// Per-sample audio is compared at the standard 2e-5 oracle tolerance (the
// whole-voice renderer accumulates per voice in worker-count-dependent
// order; the accelerated backends are tolerance-equal, never
// bit-replacing, the scalar path).
// ═══════════════════════════════════════════════════════════════════════

struct WholeVoiceRng {
    uint32_t state = 0x9E3779B9u;
    uint32_t Next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
};

void ConfigureWholeVoiceSeedPool(svms::VoiceManager& voices,
                                 const svms::ChannelCache& channels,
                                 uint32_t poolSize, uint32_t seedCount,
                                 bool transientSeeds = true,
                                 bool vibratoSeeds = false) {
    // Modeled on ConfigureDifferentialSeed but every seeded voice is Active
    // and immortal: the whole-voice scenarios need a candidate pool whose
    // state is identical at block start and mid-block.  ConfigureDifferentialSeed's
    // releasing branch cannot be overridden back to Active (StartRelease has
    // no public inverse), so the seed builds its own pool.  The gain ladder
    // values are pairwise distinct (2x steps within each i%9 residue class,
    // monotone across periods), so the level-based steal score always has
    // an unambiguous quietest candidate.
    Check(voices.Initialize(poolSize, 44100u),
          "whole-voice seed pool allocates voice storage");
    for (uint32_t i = 0; i < seedCount; ++i) {
        const uint8_t channel = static_cast<uint8_t>(i & 3u);
        const uint8_t note = static_cast<uint8_t>(48u + i % 24u);
        const svms::VoiceHandle voice = voices.AllocateVoice(
            channel, note, static_cast<uint8_t>(48u + i));
        const bool loop = (i % 4u) != 1u;
        // Every 8th seed (i % 8 == 5, always non-loop) runs a real
        // attack/decay envelope: the transient one-shot state machine and
        // its AVX2 kernel need stage-1/2 coverage against the oracle, and
        // the decay settles at the sustain level well inside the scenario
        // so the pool stays shrink-free afterwards.
        const bool transientSeed = transientSeeds && (i % 8u) == 5u;
        // Non-loop (one-shot) seed voices get a crawl-speed phase step so
        // none of them can reach relEnd and retire inside the scenario: a
        // mid-block retirement would change the active set between the
        // whole-voice pre-pass and the oracle's mid-block dispatch, which
        // is the documented drift the scenarios must avoid.
        voices.SetVoiceSample(voice, 0, 4096, 64, 4032, loop ? 1u : 0u,
                              loop ? 0.4f + static_cast<float>(i % 17u) * 0.11f
                                   : 0.05f,
                              1);
        voices.v.phases[voice] = static_cast<float>((i * 37u) % 1700u) + 0.375f;
        const float gain =
            0.004f * std::ldexp(1.0f, static_cast<int>(i % 9u)) *
            (1.0f + 0.01f * static_cast<float>(i));
        // Immortal envelope pinned to the ladder gain (sustain is stored as
        // a fraction of the initial level, hence the 1.0f): stage 3 holds
        // forever and releaseSamplesRemaining stays UINT32_MAX, so the pool
        // never shrinks and no candidate ever retires mid-scenario.
        if (transientSeed) {
            voices.SetVoiceEnvelope(voice, gain, 0.35f, 0, 0, 64, 3000,
                                    gain / 64.0f, 0.999f, 0.999f);
        } else {
            voices.SetVoiceEnvelope(voice, gain, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f,
                                    0.999f);
        }
        voices.SetVoicePlayIndex(voice, i + 1u);
        voices.SetVoiceGain(voice, 0.02f, 0.02f);
        voices.RefreshMixGain(voice, channels.GetParams()[channel]);
        if (vibratoSeeds) {
            // SF2 default vibrato modulator (5 Hz triangle, 50 cents):
            // gives the whole-voice LFO windows and the sparse vibrato
            // reference real per-voice modulation to disagree about.
            voices.v.vibLfoToPitchCents[voice] = 50.0f;
            voices.v.vibLfoSteps[voice] = 5.0f / 44100.0f;
        }
    }
}

void CheckWholeVoiceStateParity(const svms::VoiceManager& oracleVoices,
                                const svms::VoiceManager& wholeVoices,
                                const char* label) {
    // Slot handles are internal bookkeeping: the whole-voice renderer applies
    // its deferred retirements at block end (sorted by exact frame), so freed
    // slots are recycled in a different order than the frame-major oracle,
    // which frees them inline mid-block.  Voice identity is therefore the
    // playIndex (unique per launch); the per-sample audio parity pins the
    // audible behavior.
    std::vector<uint32_t> oracleActive(
        oracleVoices.activeList_,
        oracleVoices.activeList_ + oracleVoices.activeCount_);
    std::vector<uint32_t> wholeActive(
        wholeVoices.activeList_,
        wholeVoices.activeList_ + wholeVoices.activeCount_);
    auto activePlayIndices = [](const svms::VoiceManager& vm,
                                const std::vector<uint32_t>& active) {
        std::vector<uint32_t> ids;
        ids.reserve(active.size());
        for (uint32_t voice : active) ids.push_back(vm.v.playIndex[voice]);
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const std::vector<uint32_t> oracleIds =
        activePlayIndices(oracleVoices, oracleActive);
    const std::vector<uint32_t> wholeIds =
        activePlayIndices(wholeVoices, wholeActive);
    if (oracleIds != wholeIds) {
        std::fprintf(stderr,
                     "[WVDIFFV] %s active mismatch: oracle=%zu whole=%zu\n",
                     label, oracleIds.size(), wholeIds.size());
        const size_t n = (std::min)(oracleIds.size(), wholeIds.size());
        for (size_t i = 0; i < n; ++i) {
            if (oracleIds[i] != wholeIds[i]) {
                std::fprintf(stderr,
                    "[WVDIFFV] %s first playIndex divergence at %zu: oracle=%u whole=%u\n",
                    label, i, oracleIds[i], wholeIds[i]);
                break;
            }
        }
    }
    Check(oracleIds == wholeIds,
          "whole-voice preserves active voice identity and retirement");
    if (oracleIds == wholeIds) {
        uint32_t dumped = 0u;
        for (uint32_t voice : oracleActive) {
            const uint32_t playIndex = oracleVoices.v.playIndex[voice];
            uint32_t wholeVoice = UINT32_MAX;
            for (uint32_t candidate : wholeActive) {
                if (wholeVoices.v.playIndex[candidate] == playIndex) {
                    wholeVoice = candidate;
                    break;
                }
            }
            if (wholeVoice == UINT32_MAX) continue;
            const bool stateMatches =
                oracleVoices.v.state[voice] == wholeVoices.v.state[wholeVoice] &&
                NearlyEqual(oracleVoices.v.phases[voice],
                            wholeVoices.v.phases[wholeVoice], g_phaseTol) &&
                NearlyEqual(oracleVoices.v.currentGain[voice],
                            wholeVoices.v.currentGain[wholeVoice], 2.0e-5f) &&
                oracleVoices.v.releaseSamplesRemaining[voice] ==
                    wholeVoices.v.releaseSamplesRemaining[wholeVoice];
            if (!stateMatches) {
                if (dumped++ < 8u) {
                    std::fprintf(stderr,
                        "[WVDIFFV] %s playIndex=%u state=%u/%u phase=%g/%g gain=%g/%g release=%u/%u\n",
                        label, playIndex,
                        oracleVoices.v.state[voice],
                        wholeVoices.v.state[wholeVoice],
                        oracleVoices.v.phases[voice],
                        wholeVoices.v.phases[wholeVoice],
                        oracleVoices.v.currentGain[voice],
                        wholeVoices.v.currentGain[wholeVoice],
                        oracleVoices.v.releaseSamplesRemaining[voice],
                        wholeVoices.v.releaseSamplesRemaining[wholeVoice]);
                }
                Check(false, "whole-voice preserves active voice state");
            }
        }
    }
    for (uint32_t renderClass = 0u;
         renderClass < svms::kVoiceRenderClassCount; ++renderClass) {
        const auto klass = static_cast<svms::VoiceRenderClass>(renderClass);
        const uint32_t oracleCount = oracleVoices.GetRenderClassCount(klass);
        const uint32_t wholeCount = wholeVoices.GetRenderClassCount(klass);
        if (oracleCount != wholeCount) {
            std::fprintf(stderr,
                         "[WVDIFFV] %s class %u: oracle=%u whole=%u\n",
                         label, renderClass, oracleCount, wholeCount);
        }
        Check(oracleCount == wholeCount,
              "whole-voice preserves per-class render lists");
    }
    Check(oracleVoices.stealCount_ == wholeVoices.stealCount_,
          "whole-voice preserves the steal counter");
    Check(oracleVoices.GetStealTailCount() == wholeVoices.GetStealTailCount(),
          "whole-voice preserves the steal-tail census");
}

struct WholeVoiceStealContext {
    svms::VoiceManager* voices;
    const svms::ChannelParamsSnapshot* channelParams;
    uint32_t playIndex = 5000u;
    uint32_t launches = 0u;
};

void DispatchWholeVoiceStealEvent(const svms::RenderEvent* events,
                                  uint32_t eventCount, uint32_t,
                                  void* userData) {
    auto& context = *static_cast<WholeVoiceStealContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        if (event.type != svms::RenderEventType::NoteOn) continue;
        // Production launch path (LaunchVoiceGroup -> ConfigureVoice): the
        // whole-voice pre-pass derives each newborn's exact startFrame from
        // the voice-configured hook, which only the configuration
        // transaction fires.
        svms::VoiceConfiguration setup{};
        setup.sampleStart = 0u;
        setup.sampleEnd = 4096u;
        setup.loopStart = 64u;
        setup.loopEnd = 4032u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep =
            0.637f + event.data1 * 0.011f;
        // Launched voices sustain far above every ladder gain so the
        // level-based victim score can never select one of them.  Sustain
        // is stored as a fraction of the initial level, hence the 1.0f.
        setup.initialGain =
            2.0f + 0.05f * static_cast<float>(context.launches + 1u);
        setup.sustainLevel = 1.0f;
        setup.attackGainStep = 0.0f;
        setup.decaySlope = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.02f;
        setup.sampleBacked = 1u;
        setup.playIndex = context.playIndex++;
        svms::VoiceHandle handle = svms::kInvalidVoice;
        context.voices->LaunchVoiceGroup(
            event.channel, event.data1, event.data2, &setup, 1u,
            setup.playIndex,
            context.channelParams[event.channel], &handle);
        if (handle != svms::kInvalidVoice) ++context.launches;
    }
}

void RunWholeVoiceStealDifferential(uint32_t rotationMode) {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 6u;
    constexpr uint32_t kPool = 48u;
    // No transient seeds here: mid-block envelope decay changes steal
    // scores between the whole-voice pre-pass (which scores launches
    // against block-start state) and the oracle's per-frame dispatch —
    // the documented whole-voice drift, amplified by evolving gains into
    // observable victim divergence.  Transient rendering is
    // oracle-verified by the launch/CC differentials, whose launches do
    // not straddle mid-block score evolution.
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.4f * std::sin(static_cast<float>(i) * 0.031f) +
             0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);
    // Rotation modes need the exact analytic companion store.
    std::vector<int16_t> hilbert(samples.size(), 0);
    svms::BuildHilbertSlice(samples.data(), hilbert.data(),
                            {0u, sampleCount, 0u, sampleCount});
    const int16_t* hilbertPtr = rotationMode != 0u ? hilbert.data() : nullptr;

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        const char* backendName = avx2Pass ? "avx2" : "scalar";

        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool, kPool,
                                      false);
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        if (rotationMode != 0u) {
            Check(oracleVoices->SetPhaseRotationMode(rotationMode) &&
                      wholeVoices->SetPhaseRotationMode(rotationMode),
                  "steal differential seeds rotation states");
        }
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache wholeChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames),
              "whole-voice steal differential starts serial and parallel "
              "renderers");
        Check(whole.SetRenderBackend(avx2Pass ? svms::RenderBackend::AVX2
                                              : svms::RenderBackend::Scalar),
              "whole-voice steal differential selects the requested backend");

        WholeVoiceStealContext oracleContext{oracleVoices.get(),
                                             oracleChannels.GetParams()};
        WholeVoiceStealContext wholeContext{wholeVoices.get(),
                                            wholeChannels.GetParams()};
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceStealEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceStealEvent,
                                      &wholeContext);

        // Fresh PRNG per pass so both backends walk identical scenarios.
        WholeVoiceRng rng;
        float passMaxDiff = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 10000u + block * kFrames;
            svms::RenderEvent event{};
            event.type = svms::RenderEventType::NoteOn;
            event.channel = static_cast<uint8_t>(rng.Next() % 4u);
            event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
            event.data2 = static_cast<uint8_t>(64u + rng.Next() % 64u);
            // Mid-block steal frame.  Late frames push the 64-frame tail
            // fade past the block boundary, exercising ghost-tail adoption
            // into real steal-tail slots on the following block.
            event.frameOffset = 32u + rng.Next() % 469u;
            event.ingressSequence = block;

            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f);
            std::vector<float> wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), hilbertPtr,
                sampleCount,
                oracleLeft.data(), oracleRight.data(), kFrames, cfg, &event,
                1u, true, blockStartFrame);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              hilbertPtr, sampleCount, wholeLeft.data(),
                              wholeRight.data(), kFrames, cfg, &event, 1u,
                              true, blockStartFrame);

            float blockDiff = 0.0f;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleLeft[frame] - wholeLeft[frame]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleRight[frame] - wholeRight[frame]));
            }
            std::fprintf(stderr,
                         "[WVDIFF] backend=%s rot=%u steal-block=%u max=%g\n",
                         backendName, rotationMode, block, blockDiff);
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        Check(passMaxDiff <= g_waveTol,
              "whole-voice in-block stealing matches the oracle per sample");
        CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, backendName);
        if (rotationMode != 0u) {
            bool pairForm = wholeVoices->v.rot != nullptr;
            for (uint32_t k = 0; pairForm && k < wholeVoices->activeCount_; ++k)
                pairForm = wholeVoices->v.rot[wholeVoices->activeList_[k]].form == 2u;
            Check(pairForm, "rotated whole-voice pool keeps the exact pair form");
        }
        Check(whole.GetWholeVoiceBlocksForTest() == kBlocks,
              "steal scenario routes every block through the whole-voice "
              "renderer");
        Check(oracleVoices->stealCount_ == wholeVoices->stealCount_ &&
                  wholeVoices->stealCount_ == kBlocks,
              "every saturated-pool launch steals exactly one victim");
    }
}

void TestWholeVoiceStealDifferential() { RunWholeVoiceStealDifferential(0u); }

// The AVX2 steal-tail span takes whole 8-frame chunks and leaves the rest to
// the scalar span: the pair must match the scalar span alone on output, phase,
// remaining frames and filter state, across loop wraps, the sample end, odd
// remaining counts and a span longer than the fade.
void TestStealTailSpanAvx2() {
    if (!svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) return;
    const svms::RenderKernelSet* kernels =
        svms::SelectRenderKernelSet(svms::RenderBackend::AVX2);
    Check(kernels != nullptr && kernels->stealTailSpan != nullptr,
          "the AVX2 kernel set provides a steal-tail span");
    if (kernels == nullptr || kernels->stealTailSpan == nullptr) return;
    constexpr uint32_t kStoreFrames = 4096u;
    std::vector<int16_t> samples(kStoreFrames + 8u, 0);
    for (uint32_t i = 0; i < kStoreFrames; ++i)
        samples[i] = static_cast<int16_t>(
            (0.4f * std::sin(static_cast<float>(i) * 0.031f) +
             0.2f * std::cos(static_cast<float>(i) * 0.113f)) * 32767.0f);
    struct TailCase {
        float phase, step;
        bool loop, filter;
        uint32_t remaining, frames;
    };
    const TailCase cases[] = {
        {100.3f, 0.77f, true, false, 64u, 64u},
        {100.3f, 1.90f, true, true, 64u, 64u},
        {3990.0f, 1.30f, true, false, 64u, 64u},   // wraps the loop
        {4010.0f, 0.90f, false, false, 64u, 64u},  // runs off the sample end
        {10.0f, 0.50f, true, true, 37u, 64u},      // odd remaining count
        {250.0f, 1.10f, true, true, 64u, 100u},    // span longer than the fade
        {250.0f, 1.10f, true, false, 64u, 13u},    // one chunk plus a remainder
        {7.0f, 0.00f, true, true, 64u, 64u},       // zero step
    };
    uint32_t totalDone = 0u;
    for (const TailCase& c : cases) {
        svms::VoiceSoA scalarRow, kernelRow;
        Check(scalarRow.Reserve(1u) && kernelRow.Reserve(1u),
              "steal-tail test rows allocate");
        for (svms::VoiceSoA* row : {&scalarRow, &kernelRow}) {
            row->stealTailPhase[0] = c.phase;
            row->stealTailPhaseInc[0] = c.step;
            row->stealTailGain[0] = 0.8f;
            row->stealTailMixGainL[0] = 0.6f;
            row->stealTailMixGainR[0] = 0.4f;
            row->stealTailSampleStart[0] = 0u;
            row->stealTailRelEnd[0] = kStoreFrames;
            row->stealTailRelLoopS[0] = 64u;
            row->stealTailRelLoopE[0] = 4032u;
            row->stealTailRelLoopSF[0] = 64.0f;
            row->stealTailRelLoopEF[0] = 4032.0f;
            row->stealTailSampleBacked[0] = 1u;
            row->stealTailLoopEnabled[0] = c.loop ? 1u : 0u;
            row->stealTailFramesRemaining[0] = c.remaining;
            row->stealTailFramesTotal[0] = 64u;
            row->stealTailFilterEnabled[0] = c.filter ? 1u : 0u;
            row->stealTailFilterA0[0] = 0.12f;
            row->stealTailFilterB1[0] = -0.7f;
            row->stealTailFilterB2[0] = 0.25f;
            row->stealTailFilterZ1[0] = 0.01f;
            row->stealTailFilterZ2[0] = -0.02f;
        }
        const uint32_t offset = 5u;
        std::vector<float> scalarL(offset + c.frames, 0.25f),
            scalarR(offset + c.frames, -0.25f);
        std::vector<float> kernelL = scalarL, kernelR = scalarR;
        svms::RenderStealTailSpan(scalarRow, 0u, samples.data(), nullptr,
                                  kStoreFrames, scalarL.data(), scalarR.data(),
                                  offset, c.frames);
        const uint32_t done = kernels->stealTailSpan(
            kernelRow, 0u, samples.data(), kStoreFrames, kernelL.data(),
            kernelR.data(), offset, c.frames);
        Check(done % 8u == 0u && done <= c.frames,
              "steal-tail kernel consumes whole 8-frame chunks");
        totalDone += done;
        if (done < c.frames) {
            svms::RenderStealTailSpan(kernelRow, 0u, samples.data(), nullptr,
                                      kStoreFrames, kernelL.data(),
                                      kernelR.data(), offset + done,
                                      c.frames - done);
        }
        float diff = 0.0f;
        for (size_t i = 0; i < scalarL.size(); ++i) {
            diff = (std::max)(diff, std::fabs(scalarL[i] - kernelL[i]));
            diff = (std::max)(diff, std::fabs(scalarR[i] - kernelR[i]));
        }
        Check(diff <= 1.0e-6f, "steal-tail kernel matches the scalar span");
        Check(scalarRow.stealTailFramesRemaining[0] ==
                  kernelRow.stealTailFramesRemaining[0],
              "steal-tail kernel leaves the same frames remaining");
        Check(std::fabs(scalarRow.stealTailPhase[0] -
                        kernelRow.stealTailPhase[0]) <= 1.0e-4f,
              "steal-tail kernel leaves the same phase");
        Check(std::fabs(scalarRow.stealTailFilterZ1[0] -
                        kernelRow.stealTailFilterZ1[0]) <= 1.0e-6f &&
                  std::fabs(scalarRow.stealTailFilterZ2[0] -
                            kernelRow.stealTailFilterZ2[0]) <= 1.0e-6f,
              "steal-tail kernel leaves the same filter state");
    }
    Check(totalDone >= 256u, "the AVX2 steal-tail kernel takes chunks in the cases");
}

struct WholeVoiceLoudStealContext {
    svms::VoiceManager* voices;
    const svms::ChannelParamsSnapshot* channelParams;
    uint32_t playIndex = 5000u;
    uint32_t launches = 0u;
};

void DispatchWholeVoiceLoudStealEvent(const svms::RenderEvent* events,
                                      uint32_t eventCount, uint32_t,
                                      void* userData) {
    auto& context = *static_cast<WholeVoiceLoudStealContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        if (event.type != svms::RenderEventType::NoteOn) continue;
        svms::VoiceConfiguration setup{};
        setup.sampleStart = 0u;
        setup.sampleEnd = 4096u;
        setup.loopStart = 64u;
        setup.loopEnd = 4032u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep = 0.637f + event.data1 * 0.011f;
        setup.initialGain =
            2.0f + 0.05f * static_cast<float>(context.launches + 1u);
        setup.sustainLevel = 1.0f;
        setup.attackGainStep = 0.0f;
        setup.decaySlope = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.5f;
        setup.sampleBacked = 1u;
        setup.playIndex = context.playIndex++;
        svms::VoiceHandle handle = svms::kInvalidVoice;
        context.voices->LaunchVoiceGroup(
            event.channel, event.data1, event.data2, &setup, 1u,
            setup.playIndex, context.channelParams[event.channel], &handle);
        if (handle != svms::kInvalidVoice) ++context.launches;
    }
}

// Steals whose victims are loud enough to leave a fade tail (the differentials
// above use a gain ladder so quiet that no tail is ever captured).  The
// whole-voice renderer's in-block tail fade is not sample-identical to the
// frame-major oracle's: the difference is a ~64-frame burst at each steal.
// After listening to it (amplified 20x it is a faint swish) a peak difference
// of 1% of the signal peak (-40 dB) is accepted; the tail census is not
// compared here.  `lateSteals` puts every steal in the last 60 frames of the
// block so the fade straddles the block end and is adopted into a tail slot;
// `filtered` gives every victim a low-pass.
void RunWholeVoiceLoudStealParity(bool lateSteals, bool filtered) {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kPool = 192u;
    constexpr uint32_t kBlocks = kPool - 2u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.4f * std::sin(static_cast<float>(i) * 0.031f) +
             0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        auto seed = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seed, channels, kPool, kPool, false);
        for (uint32_t k = 0; k < seed->activeCount_; ++k) {
            const uint32_t h = seed->activeList_[k];
            seed->SetVoiceGain(h, 0.5f, 0.5f);
            seed->RefreshMixGain(h, channels.GetParams()[seed->v.channel[h]]);
            if (filtered) {
                // Low-pass near 140 Hz: the test tones sit at 100-600 Hz.
                const svms::PreparedVoiceFilter f = svms::PrepareVoiceLowPass(
                    static_cast<uint8_t>(svms::FilterType::LowPass2Pole), 7500,
                    0, 44100u);
                seed->v.filterA0[h] = f.a0;
                seed->v.filterB1[h] = f.b1;
                seed->v.filterB2[h] = f.b2;
                seed->v.filterEnabled[h] = f.enabled;
            }
        }
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seed);
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seed);
        svms::ChannelCache oracleChannels = channels;
        svms::ChannelCache wholeChannels = channels;
        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames) &&
                  whole.SetRenderBackend(avx2Pass
                                             ? svms::RenderBackend::AVX2
                                             : svms::RenderBackend::Scalar),
              "loud steal parity starts both renderers");
        WholeVoiceLoudStealContext oracleContext{oracleVoices.get(),
                                                 oracleChannels.GetParams()};
        WholeVoiceLoudStealContext wholeContext{wholeVoices.get(),
                                                wholeChannels.GetParams()};
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceLoudStealEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceLoudStealEvent,
                                      &wholeContext);
        WholeVoiceRng rng;
        float peak = 0.0f;
        float diffPeak = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            svms::RenderEvent event{};
            event.type = svms::RenderEventType::NoteOn;
            event.channel = static_cast<uint8_t>(rng.Next() % 4u);
            event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
            event.data2 = static_cast<uint8_t>(64u + rng.Next() % 64u);
            event.frameOffset = lateSteals ? kFrames - 60u + rng.Next() % 40u
                                           : 32u + rng.Next() % 469u;
            event.ingressSequence = block;
            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f), wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            const uint64_t blockStart = 10000u + block * kFrames;
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), nullptr,
                sampleCount, oracleLeft.data(), oracleRight.data(), kFrames,
                cfg, &event, 1u, true, blockStart);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              nullptr, sampleCount, wholeLeft.data(),
                              wholeRight.data(), kFrames, cfg, &event, 1u,
                              true, blockStart);
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                peak = (std::max)(peak, (std::max)(std::fabs(oracleLeft[frame]),
                                                   std::fabs(oracleRight[frame])));
                diffPeak = (std::max)(diffPeak,
                    (std::max)(std::fabs(oracleLeft[frame] - wholeLeft[frame]),
                               std::fabs(oracleRight[frame] - wholeRight[frame])));
            }
        }
        Check(oracleContext.launches == kBlocks &&
                  wholeContext.launches == kBlocks,
              "loud steal parity steals once per block");
        Check(peak > 1.0f && diffPeak <= 0.01f * peak,
              "loud-victim steal tails stay within -40 dB of the oracle");
    }
}

void TestWholeVoiceLoudStealParity() {
    RunWholeVoiceLoudStealParity(false, false);
    RunWholeVoiceLoudStealParity(true, false);
    RunWholeVoiceLoudStealParity(false, true);
    RunWholeVoiceLoudStealParity(true, true);
}

// Rotated pools (Analytic/Sweep/Random pair form) ride the whole-voice path:
// ghosts carry their rotation state into the in-band tail, and adopted tails
// continue it.  Same scenario and oracle as the Coherent steal differential.
void TestWholeVoiceRotationStealDifferential() {
    for (uint32_t mode : {1u, 2u, 4u}) RunWholeVoiceStealDifferential(mode);
}

struct WholeVoiceLaunchContext {
    svms::VoiceManager* voices;
    const svms::ChannelParamsSnapshot* channelParams;
    uint32_t playIndex = 9000u;
    uint32_t launches = 0u;
    uint32_t releaseSamples = 400u;
    bool allLoop = false;
    uint32_t layers = 1u;   // voices per note-on (layered SoundFont regions)
};

void DispatchWholeVoiceLaunchEvent(const svms::RenderEvent* events,
                                   uint32_t eventCount, uint32_t,
                                   void* userData) {
    auto& context = *static_cast<WholeVoiceLaunchContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        switch (event.type) {
            case svms::RenderEventType::NoteOn: {
                // Production launch path (LaunchVoiceGroup -> ConfigureVoice):
                // the whole-voice pre-pass derives each newborn's exact
                // startFrame from the voice-configured hook.  The pool is
                // kept unsaturated so launches never steal.
                const bool loop = context.allLoop || (context.playIndex % 3u) != 0u;
                svms::VoiceConfiguration setup{};
                setup.sampleStart = 0u;
                setup.sampleEnd = 4096u;
                setup.loopStart = 64u;
                setup.loopEnd = 4032u;
                setup.loopMode = loop ? 1u : 0u;
                setup.phaseStep = setup.basePhaseStep =
                    0.637f + event.data1 * 0.011f;
                // Attack+decay transient (~70 frames) into sustain, then a
                // release capped at 400 samples: covers transient->sustained
                // class changes, exact-frame deferred releases and
                // counter-governed retirement.
                setup.initialGain = 0.6f;
                setup.sustainLevel = 0.5f;
                setup.attackSamples = 17u;
                setup.attackGainStep = 0.6f / 17.0f;
                setup.decaySamples = 53u;
                setup.decaySlope = 0.99f;
                setup.releaseDecay = 0.9995f;
                setup.releaseSamples = context.releaseSamples;
                setup.gainLeft = setup.gainRight = 0.02f;
                setup.sampleBacked = 1u;
                setup.playIndex = context.playIndex++;
                svms::VoiceConfiguration layerSetups[4];
                svms::VoiceHandle handles[4] = {
                    svms::kInvalidVoice, svms::kInvalidVoice,
                    svms::kInvalidVoice, svms::kInvalidVoice};
                const uint32_t layers = (std::min)(context.layers, 4u);
                for (uint32_t layer = 0u; layer < layers; ++layer) {
                    layerSetups[layer] = setup;
                    // Distinct layers: detuned, panned differently.
                    layerSetups[layer].phaseStep =
                        layerSetups[layer].basePhaseStep =
                            setup.phaseStep * (1.0f + 0.013f * layer);
                    layerSetups[layer].gainLeft = 0.02f * (1.0f + 0.3f * layer);
                    layerSetups[layer].gainRight = 0.02f * (1.0f - 0.2f * layer);
                }
                context.voices->LaunchVoiceGroup(
                    event.channel, event.data1, event.data2, layerSetups,
                    layers, setup.playIndex,
                    context.channelParams[event.channel], handles);
                if (handles[0] != svms::kInvalidVoice) ++context.launches;
                break;
            }
            case svms::RenderEventType::NoteOff:
                for (uint32_t i = 0; i < context.voices->activeCount_; ++i) {
                    const uint32_t voice = context.voices->activeList_[i];
                    if (context.voices->v.channel[voice] == event.channel &&
                        context.voices->v.note[voice] == event.data1) {
                        context.voices->StartRelease(voice);
                    }
                }
                break;
            case svms::RenderEventType::Reset:
                context.voices->Reset();
                break;
            default:
                break;
        }
    }
}

// Barrier events (Reset, AllSoundOff) split the block: the stretches before
// and after render on the whole-voice path and the barrier dispatches at its
// exact frame.  The oracle dispatches the same events in the same order.
// Voice decimation (opt-in).  A static pool with distinct velocities: the
// whole-voice renderer must mute exactly the voices the frame-major oracle
// leaves out (everything below the loudest activeCount/step by velocity),
// keep their state advancing, and leave the audio untouched when disabled.
// Retire floor on counted release tails: default floor leaves the count
// alone; a raised floor cuts it to the frame the tail reaches the floor.
void TestFloorLimitedReleaseSamples() {
    const float decay = svms::MakeReleaseDecay(1.5f, 44100u);
    const uint32_t samples = svms::MakeReleaseSamples(1.5f, 44100u);
    const float saved = svms::VoiceRetireThreshold();
    svms::g_voiceRetireThreshold.store(svms::kVoiceRetireThreshold);
    Check(svms::FloorLimitedReleaseSamples(samples, decay, 1.0f) == samples,
          "default floor changes the release count");
    const float floorGain = std::pow(10.0f, -50.0f / 20.0f);
    svms::g_voiceRetireThreshold.store(floorGain);
    const uint32_t cut = svms::FloorLimitedReleaseSamples(samples, decay, 0.8f);
    Check(cut < samples && cut > samples / 2u, "raised floor did not shorten the tail");
    Check(0.8f * std::pow(decay, static_cast<float>(cut)) <= floorGain * 1.001f &&
          0.8f * std::pow(decay, static_cast<float>(cut - 1u)) > floorGain * 0.999f,
          "cut count is not the floor crossing frame");
    Check(svms::FloorLimitedReleaseSamples(samples, decay, floorGain * 0.5f) == 1u,
          "voice below the floor does not retire at once");
    svms::g_voiceRetireThreshold.store(saved);
}

void TestWholeVoiceDecimation() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 3u;
    constexpr uint32_t kPool = 40u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        const char* backendName = avx2Pass ? "avx2" : "scalar";
        for (int enabled = 0; enabled < 2; ++enabled) {
            svms::ChannelCache seedChannels;
            seedChannels.SetMasterVolume(1.0f);
            seedChannels.RebuildCache(cfg, 44100.0f);
            auto seedVoices = std::make_unique<svms::VoiceManager>();
            ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool,
                                        kPool, false);
            auto oracleVoices =
                std::make_unique<svms::VoiceManager>(*seedVoices);
            auto wholeVoices =
                std::make_unique<svms::VoiceManager>(*seedVoices);
            auto plainVoices =
                std::make_unique<svms::VoiceManager>(*seedVoices);
            svms::ChannelCache oracleChannels = seedChannels;
            svms::ChannelCache wholeChannels = seedChannels;
            svms::ChannelCache plainChannels = seedChannels;

            svms::RenderScalar oracle, whole, plain;
            Check(oracle.ReserveVoiceCapacity(kPool) &&
                      whole.ReserveVoiceCapacity(kPool) &&
                      plain.ReserveVoiceCapacity(kPool) &&
                      oracle.ConfigureRenderThreads(1u, kFrames) &&
                      whole.ConfigureRenderThreads(4u, kFrames) &&
                      plain.ConfigureRenderThreads(4u, kFrames),
                  "decimation test starts its renderers");
            Check(whole.SetRenderBackend(
                      avx2Pass ? svms::RenderBackend::AVX2
                               : svms::RenderBackend::Scalar) &&
                      plain.SetRenderBackend(
                          avx2Pass ? svms::RenderBackend::AVX2
                                   : svms::RenderBackend::Scalar),
                  "decimation test selects the backend");
            // Tiers shrunk so a 40-voice pool lands on step 4 (keep 10).
            for (svms::RenderScalar* r : {&oracle, &whole})
                r->SetDecimationTiersForTest(16u, 32u, 48u, 64u);
            oracle.SetDecimationEnabled(enabled != 0);
            whole.SetDecimationEnabled(enabled != 0);

            float maxDiff = 0.0f;
            float maxVsPlain = 0.0f;
            for (uint32_t block = 0; block < kBlocks; ++block) {
                const uint64_t start = 10000u + block * kFrames;
                std::vector<float> oL(kFrames, 0.0f), oR(kFrames, 0.0f);
                std::vector<float> wL(kFrames, 0.0f), wR(kFrames, 0.0f);
                std::vector<float> pL(kFrames, 0.0f), pR(kFrames, 0.0f);
                oracle.RenderBlockReference(*oracleVoices, oracleChannels,
                    samples.data(), sampleCount, oL.data(), oR.data(),
                    kFrames, cfg, nullptr, 0u, true, start);
                whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                    sampleCount, wL.data(), wR.data(), kFrames, cfg, nullptr,
                    0u, true, start);
                plain.RenderBlock(*plainVoices, plainChannels, samples.data(),
                    sampleCount, pL.data(), pR.data(), kFrames, cfg, nullptr,
                    0u, true, start);
                for (uint32_t f = 0; f < kFrames; ++f) {
                    maxDiff = (std::max)(maxDiff,
                        std::fabs(oL[f] - wL[f]));
                    maxDiff = (std::max)(maxDiff,
                        std::fabs(oR[f] - wR[f]));
                    maxVsPlain = (std::max)(maxVsPlain,
                        std::fabs(pL[f] - wL[f]));
                }
            }
            std::fprintf(stderr,
                "[DECIM] backend=%s enabled=%d oracle-vs-whole=%g whole-vs-plain=%g\n",
                backendName, enabled, maxDiff, maxVsPlain);
            Check(maxDiff <= 2.0e-5f,
                  "decimated whole-voice blocks match the frame-major oracle");
            if (enabled == 0) {
                Check(maxVsPlain == 0.0f,
                      "disabled decimation leaves the audio untouched");
            } else {
                Check(maxVsPlain > 1.0e-4f,
                      "enabled decimation actually silences voices");
                // Muted rows still advance: per-handle state matches the
                // oracle (no events, so handles are the stable identity;
                // the seed pool shares one playIndex).
                bool stateMatches = oracleVoices->activeCount_ ==
                                    wholeVoices->activeCount_;
                for (uint32_t h = 0; h < kPool && stateMatches; ++h) {
                    stateMatches =
                        // A muted span adds the phase in one step instead of
                        // per frame, so its float rounding drifts by a
                        // fraction of a sample over a few blocks; envelopes
                        // are exact.  Inaudible while muted.
                        NearlyEqual(oracleVoices->v.phases[h],
                                    wholeVoices->v.phases[h], 0.5f) &&
                        NearlyEqual(oracleVoices->v.currentGain[h],
                                    wholeVoices->v.currentGain[h], 2.0e-5f);
                }
                Check(stateMatches,
                      "muted voices keep advancing phase and gain");
            }
        }
    }
}

// Decimation ranked by level: velocity falls with the handle while the
// envelope level rises with it, so the two rankings keep opposite halves of
// the pool.  Keeping the loud-by-level voices must leave more energy than
// keeping the high-velocity (quiet) ones.
void TestWholeVoiceDecimationByLevel() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kPool = 40u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    double energy[3] = {0.0, 0.0, 0.0};  // plain, by velocity, by level
    for (int mode = 0; mode < 3; ++mode) {
        svms::ChannelCache channels;
        channels.SetMasterVolume(1.0f);
        channels.RebuildCache(cfg, 44100.0f);
        auto voices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*voices, channels, kPool, kPool, false);
        for (uint32_t h = 0; h < kPool; ++h) {
            voices->v.velocity[h] = static_cast<uint8_t>(127u - h);
            voices->v.stealOutputGain[h] = 1.0f;
            voices->v.currentGain[h] = 0.02f + 0.02f * static_cast<float>(h);
            voices->v.renderGainL[h] =
                voices->v.currentGain[h] * voices->v.mixGainL[h];
            voices->v.renderGainR[h] =
                voices->v.currentGain[h] * voices->v.mixGainR[h];
        }
        svms::RenderScalar r;
        Check(r.ReserveVoiceCapacity(kPool) &&
                  r.ConfigureRenderThreads(2u, kFrames),
              "by-level decimation test starts its renderer");
        r.SetDecimationTiersForTest(16u, 32u, 48u, 64u);
        r.SetDecimationEnabled(mode != 0);
        r.SetDecimationByLevel(mode == 2);
        std::vector<float> L(kFrames, 0.0f), R(kFrames, 0.0f);
        r.RenderBlock(*voices, channels, samples.data(), sampleCount,
                      L.data(), R.data(), kFrames, cfg, nullptr, 0u, true,
                      10000u);
        for (uint32_t f = 0; f < kFrames; ++f)
            energy[mode] += double(L[f]) * L[f] + double(R[f]) * R[f];
    }
    std::fprintf(stderr, "[DECIM-LEVEL] plain=%g velocity=%g level=%g\n",
                 energy[0], energy[1], energy[2]);
    Check(energy[1] < energy[0] && energy[2] < energy[0],
          "decimation by level mutes voices");
    Check(energy[2] > 2.0 * energy[1],
          "ranking by level keeps the louder voices than ranking by velocity");
}

// Muted-tail cull: a releasing pool above the (shrunk) decimation tier keeps
// only its loudest 1/step mixing; with the cull on, the muted rest is retired
// after the configured run of muted blocks, with it off nothing is.
void TestMutedTailCull() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kPool = 40u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    for (uint32_t cull : {0u, 2u}) {
        svms::ChannelCache channels;
        channels.SetMasterVolume(1.0f);
        channels.RebuildCache(cfg, 44100.0f);
        auto voices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*voices, channels, kPool, kPool, false);
        for (uint32_t h = 0; h < kPool; ++h) {
            voices->v.velocity[h] = static_cast<uint8_t>(127u - h);
            voices->StartRelease(static_cast<svms::VoiceHandle>(h));
        }
        svms::RenderScalar r;
        Check(r.ReserveVoiceCapacity(kPool) &&
                  r.ConfigureRenderThreads(2u, kFrames),
              "cull test starts its renderer");
        r.SetDecimationTiersForTest(16u, 32u, 48u, 64u);
        r.SetDecimationEnabled(true);
        r.SetMutedTailCullBlocks(cull);
        std::vector<float> L(kFrames, 0.0f), R(kFrames, 0.0f);
        uint32_t activeAfter[6] = {};
        for (uint32_t block = 0; block < 6u; ++block) {
            std::fill(L.begin(), L.end(), 0.0f);
            std::fill(R.begin(), R.end(), 0.0f);
            r.RenderBlock(*voices, channels, samples.data(), sampleCount,
                          L.data(), R.data(), kFrames, cfg, nullptr, 0u, true,
                          10000u + block * kFrames);
            activeAfter[block] = voices->activeCount_;
        }
        std::fprintf(stderr, "[CULL] cull=%u active after blocks: %u %u %u %u %u %u\n",
                     cull, activeAfter[0], activeAfter[1], activeAfter[2],
                     activeAfter[3], activeAfter[4], activeAfter[5]);
        if (cull == 0u) {
            Check(activeAfter[5] == kPool, "no cull: muted tails stay in the pool");
        } else {
            Check(activeAfter[0] == kPool && activeAfter[1] == kPool,
                  "cull waits for the muted run");
            Check(activeAfter[5] == kPool / 4u,
                  "cull retires the muted tails and keeps the loudest 1/step");
        }
    }
}

// Filtered sustained loops through the 8-voices-per-vector biquad group
// kernel must reproduce the per-voice kernel exactly: with each voice on its
// own channel plane (bus mode) the planes and every voice's end state are
// compared bitwise; on a single shared plane only the mix order of lanes that
// drifted apart could differ, so that comparison is a tight tolerance.
void RunFilteredGroupKernelTest(bool release) {
    if (!svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) return;
    constexpr uint32_t kVoices = 27u;  // three groups of eight + a remainder
    constexpr uint32_t kRegion = 4096u;
    std::vector<int16_t> samples(kRegion + 8u, 0);
    uint32_t rng = 12345u;
    for (uint32_t i = 0; i < kRegion; ++i) {
        rng = rng * 1664525u + 1013904223u;
        samples[i] = static_cast<int16_t>((rng >> 16) & 0x7FFF) - 16384;
    }
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto seed = std::make_unique<svms::VoiceManager>();
    Check(seed->Initialize(64u, 44100u), "filter group pool allocates");
    const uint32_t loopLengths[] = {40u, 130u, 700u, 3000u};
    std::vector<uint32_t> handles;
    for (uint32_t i = 0; i < kVoices; ++i) {
        const uint8_t channel = static_cast<uint8_t>(i % 16u);
        const svms::VoiceHandle voice = seed->AllocateVoice(
            channel, static_cast<uint8_t>(40u + i), 100);
        const uint32_t loopLen = loopLengths[i % 4u];
        seed->SetVoiceSample(voice, 0, kRegion, 16, 16 + loopLen, 1u,
                             0.3f + 0.07f * static_cast<float>(i % 23u), 1);
        seed->v.phases[voice] = static_cast<float>((i * 53u) % 900u) + 0.25f;
        seed->SetVoiceEnvelope(voice, 0.5f, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f, 0.999f);
        seed->SetVoicePlayIndex(voice, i + 1u);
        seed->SetVoiceGain(voice, 0.3f + 0.01f * static_cast<float>(i),
                           0.2f + 0.01f * static_cast<float>(i));
        seed->RefreshMixGain(voice, channels.GetParams()[channel]);
        seed->v.renderGainL[voice] = seed->v.currentGain[voice] * seed->v.mixGainL[voice];
        seed->v.renderGainR[voice] = seed->v.currentGain[voice] * seed->v.mixGainR[voice];
        const svms::PreparedVoiceFilter f = svms::PrepareVoiceLowPass(
            static_cast<uint8_t>(svms::FilterType::LowPass2Pole),
            static_cast<int16_t>(6000 + 250 * static_cast<int>(i % 28u)),
            static_cast<int16_t>((i % 5u) * 60), 44100u);
        seed->v.filterA0[voice] = f.a0;
        seed->v.filterB1[voice] = f.b1;
        seed->v.filterB2[voice] = f.b2;
        seed->v.filterEnabled[voice] = f.enabled;
        seed->v.filterZ1[voice] = 0.01f * static_cast<float>(i % 7u);
        seed->v.filterZ2[voice] = -0.005f * static_cast<float>(i % 5u);
        if (release) {
            // Releasing tails: a mix of threshold-governed (UINT32_MAX) and
            // counted ones, several of which retire inside the longer span.
            seed->StartRelease(voice);
            seed->v.currentGain[voice] = 0.4f + 0.01f * static_cast<float>(i);
            seed->v.releaseDecay[voice] = 0.9990f - 0.0004f * static_cast<float>(i % 9u);
            seed->v.releaseSamplesRemaining[voice] =
                (i % 3u == 0u) ? UINT32_MAX : 200u + 150u * (i % 11u);
        }
        handles.push_back(voice);
    }
    // One unfiltered voice mid-list splits the pending group, as in a real mix.
    seed->v.filterEnabled[handles[13]] = 0u;

    const svms::RenderKernelSet& kernels = svms::GetAVX2RenderKernelSet();
    const svms::RenderClassKernel kernel =
        kernels.kernels[static_cast<uint32_t>(release
            ? svms::VoiceRenderClass::ReleaseLoop
            : svms::VoiceRenderClass::SustainedLoop)];
    struct Span { uint32_t start, count; };
    const Span spans[] = {{0u, 2048u}, {5u, 203u}, {0u, 64u}};
    for (const Span& span : spans) {
        for (int bus = 0; bus < 2; ++bus) {
            std::vector<float> planes[2][2][16];  // [grouped][L/R][channel]
            float* planePtr[2][2][16];
            std::vector<float> mix[2][2];
            std::unique_ptr<svms::VoiceManager> run[2];
            std::vector<svms::SpanRetirement> retired[2];
            std::vector<uint32_t> positions(64u);
            for (uint32_t p = 0; p < 64u; ++p) positions[p] = p;
            for (int grouped = 0; grouped < 2; ++grouped) {
                run[grouped] = std::make_unique<svms::VoiceManager>(*seed);
                for (int side = 0; side < 2; ++side) {
                    for (uint32_t ch = 0; ch < 16u; ++ch) {
                        planes[grouped][side][ch].assign(span.start + span.count + 16u, 0.0f);
                        planePtr[grouped][side][ch] = planes[grouped][side][ch].data();
                    }
                    mix[grouped][side].assign(span.start + span.count + 16u, 0.0f);
                }
                svms::SetFilteredGroupKernelForTest(grouped != 0);
                retired[grouped].assign(64u, svms::SpanRetirement{});
                uint32_t retiredCount = 0u;
                svms::RenderSpanContext ctx{
                    &run[grouped]->v, samples.data(), nullptr, kRegion + 8u,
                    mix[grouped][0].data(), mix[grouped][1].data(),
                    span.start, span.count, 64u, nullptr, nullptr,
                    positions.data(), retired[grouped].data(), &retiredCount,
                    0.0f, 1.0f, 0u,
                    bus ? planePtr[grouped][0] : nullptr,
                    bus ? planePtr[grouped][1] : nullptr};
                Check(kernel(ctx, handles.data(),
                             static_cast<uint32_t>(handles.size())),
                      "filter group kernel accepts the batch");
                retired[grouped].resize(retiredCount);
            }
            const bool retiredSame =
                retired[0].size() == retired[1].size() &&
                (retired[0].empty() ||
                 std::memcmp(retired[0].data(), retired[1].data(),
                             retired[0].size() *
                                 sizeof(svms::SpanRetirement)) == 0);
            svms::SetFilteredGroupKernelForTest(true);
            float maxDiff = 0.0f;
            bool bitExact = true;
            if (bus) {
                for (int side = 0; side < 2; ++side)
                    for (uint32_t ch = 0; ch < 16u; ++ch)
                        bitExact = bitExact &&
                            std::memcmp(planes[0][side][ch].data(), planes[1][side][ch].data(),
                                        planes[0][side][ch].size() * sizeof(float)) == 0;
            } else {
                for (int side = 0; side < 2; ++side)
                    for (size_t f = 0; f < mix[0][side].size(); ++f)
                        maxDiff = (std::max)(maxDiff,
                            std::fabs(mix[0][side][f] - mix[1][side][f]));
            }
            bool stateExact = true;
            for (uint32_t handle : handles) {
                stateExact = stateExact &&
                    run[0]->v.phases[handle] == run[1]->v.phases[handle] &&
                    run[0]->v.filterZ1[handle] == run[1]->v.filterZ1[handle] &&
                    run[0]->v.filterZ2[handle] == run[1]->v.filterZ2[handle] &&
                    run[0]->v.currentGain[handle] == run[1]->v.currentGain[handle] &&
                    run[0]->v.releaseSamplesRemaining[handle] ==
                        run[1]->v.releaseSamplesRemaining[handle];
            }
            std::fprintf(stderr,
                "[FILTGRP] %s start=%u count=%u bus=%d planes-bit-exact=%d state-bit-exact=%d retire-same=%d(%zu) mix-maxdiff=%g\n",
                release ? "release" : "sustained", span.start, span.count, bus,
                bus ? (bitExact ? 1 : 0) : -1, stateExact ? 1 : 0,
                retiredSame ? 1 : 0, retired[0].size(), maxDiff);
            Check(retiredSame, "filter group retires the same voices on the same frames");
            Check(stateExact, "filter group keeps per-voice phase and filter state bit-identical");
            if (bus) Check(bitExact, "filter group planes are bit-identical to the per-voice kernel");
            else Check(maxDiff <= 1.0e-6f, "filter group shared-plane mix matches within summation order");
        }
    }
}

void TestFilteredGroupKernel() {
    RunFilteredGroupKernelTest(false);
    RunFilteredGroupKernelTest(true);
}

void TestWholeVoiceResetBarrierDifferential() {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 8u;
    constexpr uint32_t kPool = 64u;
    constexpr uint32_t kSeed = 21u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        const char* backendName = avx2Pass ? "avx2" : "scalar";

        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool, kSeed);
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache wholeChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames),
              "reset barrier differential starts serial and parallel "
              "renderers");
        Check(whole.SetRenderBackend(avx2Pass ? svms::RenderBackend::AVX2
                                              : svms::RenderBackend::Scalar),
              "reset barrier differential selects the requested backend");

        WholeVoiceLaunchContext oracleContext{oracleVoices.get(),
                                              oracleChannels.GetParams()};
        WholeVoiceLaunchContext wholeContext{wholeVoices.get(),
                                             wholeChannels.GetParams()};
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                      &wholeContext);

        WholeVoiceRng rng;
        std::vector<std::pair<uint8_t, uint8_t>> launchedPairs;
        float passMaxDiff = 0.0f;
        uint32_t barrierBlocks = 0u;
        uint32_t nextKey = 0u;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 20000u + block * kFrames;
            std::vector<svms::RenderEvent> events;
            uint32_t sequence = 0u;
            const auto push = [&](svms::RenderEventType type, uint8_t channel,
                                  uint8_t data1, uint8_t data2,
                                  uint32_t frame) {
                svms::RenderEvent event{};
                event.type = type;
                event.channel = channel;
                event.data1 = data1;
                event.data2 = data2;
                event.frameOffset = frame;
                event.ingressSequence = sequence++;
                events.push_back(event);
            };
            for (uint32_t j = 0; j < 5u; ++j, ++nextKey) {
                push(svms::RenderEventType::NoteOn,
                     static_cast<uint8_t>(nextKey % 4u),
                     static_cast<uint8_t>(40u + nextKey % 80u),
                     static_cast<uint8_t>(64u + rng.Next() % 64u),
                     rng.Next() % kFrames);
            }
            for (uint32_t j = 0; j < 3u && !launchedPairs.empty(); ++j) {
                const auto pair =
                    launchedPairs[rng.Next() % launchedPairs.size()];
                push(svms::RenderEventType::NoteOff, pair.first, pair.second,
                     0u, rng.Next() % kFrames);
            }
            for (uint32_t j = 0; j < 5u; ++j)
                launchedPairs.emplace_back(events[j].channel, events[j].data1);
            // Barrier placement: mid-block, frame 0 twice then a note, last
            // frame, no-op + reset at one frame, a lone no-op.
            switch (block) {
                case 2u:
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 200u);
                    break;
                case 3u:
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 0u);
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 0u);
                    push(svms::RenderEventType::NoteOn, 1u, 77u, 100u, 0u);
                    break;
                case 4u:
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u,
                         kFrames - 1u);
                    break;
                case 5u:
                    push(svms::RenderEventType::AllSoundOff, 0u, 0u, 0u, 100u);
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 100u);
                    push(svms::RenderEventType::Reset, 0u, 0u, 0u, 300u);
                    push(svms::RenderEventType::NoteOn, 2u, 90u, 110u, 301u);
                    break;
                case 6u:
                    push(svms::RenderEventType::AllNotesOff, 0u, 0u, 0u, 50u);
                    break;
                default:
                    break;
            }
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });
            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f);
            std::vector<float> wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), sampleCount,
                oracleLeft.data(), oracleRight.data(), kFrames, cfg,
                events.data(), static_cast<uint32_t>(events.size()), true,
                blockStartFrame);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              sampleCount, wholeLeft.data(),
                              wholeRight.data(), kFrames, cfg, events.data(),
                              static_cast<uint32_t>(events.size()), true,
                              blockStartFrame);
            const uint32_t paths = whole.GetLastRenderPaths();
            Check((paths & 0x6u) == 0u,
                  "barrier blocks never reach the dense or sparse renderer");
            if ((paths & 0x200u) != 0u) ++barrierBlocks;

            float blockDiff = 0.0f;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleLeft[frame] - wholeLeft[frame]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleRight[frame] - wholeRight[frame]));
            }
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        std::fprintf(stderr, "[WVDIFF] backend=%s reset-barrier max=%g\n",
                     backendName, passMaxDiff);
        Check(passMaxDiff <= g_waveTol,
              "barrier-split blocks match the oracle per sample");
        Check(barrierBlocks == 5u, "five blocks were barrier-split");
        Check(whole.GetPathBlocks(1) == 0u && whole.GetPathBlocks(2) == 0u,
              "no block fell back to dense or sparse");
        CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, backendName);
        Check(oracleContext.launches == wholeContext.launches,
              "both sides launched the same notes");
    }
}

void RunWholeVoiceLaunchDifferential(uint32_t rotationMode) {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 6u;
    constexpr uint32_t kPool = 64u;
    constexpr uint32_t kSeed = 16u;
    constexpr uint32_t kLaunchesPerBlock = 5u;
    constexpr uint32_t kNoteOffsPerBlock = 3u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);
    std::vector<int16_t> hilbert(samples.size(), 0);
    svms::BuildHilbertSlice(samples.data(), hilbert.data(),
                            {0u, sampleCount, 0u, sampleCount});
    const int16_t* hilbertPtr = rotationMode != 0u ? hilbert.data() : nullptr;

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        const char* backendName = avx2Pass ? "avx2" : "scalar";

        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool, kSeed);
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        if (rotationMode != 0u) {
            Check(oracleVoices->SetPhaseRotationMode(rotationMode) &&
                      wholeVoices->SetPhaseRotationMode(rotationMode),
                  "launch differential seeds rotation states");
        }
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache wholeChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames),
              "whole-voice launch differential starts serial and parallel "
              "renderers");
        Check(whole.SetRenderBackend(avx2Pass ? svms::RenderBackend::AVX2
                                              : svms::RenderBackend::Scalar),
              "whole-voice launch differential selects the requested backend");

        WholeVoiceLaunchContext oracleContext{oracleVoices.get(),
                                              oracleChannels.GetParams()};
        WholeVoiceLaunchContext wholeContext{wholeVoices.get(),
                                             wholeChannels.GetParams()};
        if (rotationMode != 0u) {
            // Rotation seeds depend on the slot handle, and whole-voice
            // retirements land at block end (slot recycling differs from
            // the oracle by design): keep releases from retiring inside the
            // scenario so both sides allocate identical handles.
            oracleContext.releaseSamples = wholeContext.releaseSamples = 30000u;
            oracleContext.allLoop = wholeContext.allLoop = true;
        }
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                      &wholeContext);

        // Fresh PRNG per pass so both backends walk identical scenarios.
        // (channel, note) pairs are never reused, so a block never re-attacks
        // a key released earlier in the same block.
        WholeVoiceRng rng;
        std::vector<std::pair<uint8_t, uint8_t>> launchedPairs;
        float passMaxDiff = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 10000u + block * kFrames;
            const uint32_t noteOffCount =
                launchedPairs.empty() ? 0u : kNoteOffsPerBlock;
            std::vector<svms::RenderEvent> events(kLaunchesPerBlock +
                                                  noteOffCount);
            for (uint32_t j = 0; j < kLaunchesPerBlock; ++j) {
                svms::RenderEvent& event = events[j];
                event.type = svms::RenderEventType::NoteOn;
                const uint32_t launch = block * kLaunchesPerBlock + j;
                event.channel = static_cast<uint8_t>(launch % 4u);
                event.data1 = static_cast<uint8_t>(120u + launch);
                event.data2 = static_cast<uint8_t>(64u + rng.Next() % 64u);
                event.frameOffset = rng.Next() % kFrames;
                event.ingressSequence = j;
            }
            for (uint32_t j = 0; j < noteOffCount; ++j) {
                svms::RenderEvent& event = events[kLaunchesPerBlock + j];
                event.type = svms::RenderEventType::NoteOff;
                // Release keys launched in earlier blocks only: the note-off
                // scan is not frame-scoped inside the whole-voice pre-pass,
                // so releasing a key that is re-attacked later in the same
                // block would observe pre-pass state the oracle never sees.
                const uint32_t pick = rng.Next() % launchedPairs.size();
                event.channel = launchedPairs[pick].first;
                event.data1 = launchedPairs[pick].second;
                event.data2 = 0u;
                event.frameOffset = rng.Next() % kFrames;
                event.ingressSequence = kLaunchesPerBlock + j;
            }
            for (uint32_t j = 0; j < kLaunchesPerBlock; ++j)
                launchedPairs.emplace_back(events[j].channel,
                                           events[j].data1);
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });

            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f);
            std::vector<float> wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), hilbertPtr,
                sampleCount,
                oracleLeft.data(), oracleRight.data(), kFrames, cfg,
                events.data(), static_cast<uint32_t>(events.size()), true,
                blockStartFrame);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              hilbertPtr, sampleCount, wholeLeft.data(),
                              wholeRight.data(), kFrames, cfg, events.data(),
                              static_cast<uint32_t>(events.size()), true,
                              blockStartFrame);

            {
                // The releasing counter must equal the number of rows in the
                // Releasing state (a voice released and retired inside one
                // block once underflowed it).
                uint32_t releasingRows = 0u;
                for (uint32_t i = 0u; i < wholeVoices->activeCount_; ++i)
                    releasingRows += wholeVoices->v.state[
                        wholeVoices->activeList_[i]] ==
                        static_cast<uint8_t>(svms::VoiceState::Releasing);
                Check(wholeVoices->GetReleasingCount() == releasingRows,
                      "releasing-voice counter matches the Releasing rows");
            }
            float blockDiff = 0.0f;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleLeft[frame] - wholeLeft[frame]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleRight[frame] - wholeRight[frame]));
            }
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        std::fprintf(stderr, "[WVDIFF] backend=%s rot=%u launch max=%g\n",
                     backendName, rotationMode, passMaxDiff);
        Check(passMaxDiff <= g_waveTol,
              "whole-voice launches, deferred releases and retirements "
              "match the oracle per sample");
        CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, backendName);
        Check(whole.GetWholeVoiceBlocksForTest() == kBlocks,
              "launch scenario routes every block through the whole-voice "
              "renderer");
        Check(oracleContext.launches == wholeContext.launches &&
                  wholeContext.launches == kBlocks * kLaunchesPerBlock,
              "launch scenario dispatches every note-on on both sides");
        Check(oracleVoices->stealCount_ == 0u && wholeVoices->stealCount_ == 0u,
              "launch scenario never exhausts the pool");
    }
}

void TestWholeVoiceLaunchDifferential() { RunWholeVoiceLaunchDifferential(0u); }


// Rotated pools: free-slot launches seed their own state; deferred releases
// and natural retirements keep rotating voices exact against the oracle.
void TestWholeVoiceRotationLaunchDifferential() {
    for (uint32_t mode : {1u, 2u, 4u}) RunWholeVoiceLaunchDifferential(mode);
}

struct WholeVoiceCCContext {
    svms::VoiceManager* voices;
    svms::ChannelCache* channels;
    const svms::ChannelParamsSnapshot* channelParams;
    const svms::RuntimeConfigSnapshot* config = nullptr;
    uint32_t playIndex = 12000u;
    uint32_t launches = 0u;
    // Vibrato scenario: launched voices carry the SF2 default vibrato
    // modulator like production note-ons do.
    bool vibratoLaunches = false;
    // SysEx master state, mirroring the driver's sysex terms.
    float masterFineTune = 0.0f;
    float masterTranspose = 0.0f;
    float channelBendRatio[svms::kChannelCount] = {};
    void ResetMasterState() {
        masterFineTune = 0.0f;
        masterTranspose = 0.0f;
        for (float& ratio : channelBendRatio) ratio = 1.0f;
    }
};

// Mirrors the production dispatch semantics (Driver::HandleControlChange and
// NoteOffPlayIndex) for exactly the event set the whole-voice renderer
// accepts: notes plus the lifecycle controllers 64/66/120/123.  Both the
// oracle and the whole-voice renderer share this dispatcher, so any
// divergence between the two sides is pure render-path divergence.
void DispatchWholeVoiceCCEvent(const svms::RenderEvent* events,
                               uint32_t eventCount, uint32_t blockCursor,
                               void* userData) {
    auto& context = *static_cast<WholeVoiceCCContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        const uint8_t channel = event.channel;
        // Mirrors Driver::HandlePitchBend: the sysex master terms ride on
        // top of the channel wheel.
        const auto applyBend = [&context, channel](uint16_t wheel) {
            context.channels->PitchBend(
                channel, static_cast<int16_t>(wheel));
            const float bendSemitones =
                context.channels->GetPitchBendSemitones(channel) +
                context.masterFineTune + context.masterTranspose;
            const float commonRatio = std::pow(2.0f, bendSemitones / 12.0f);
            context.channelBendRatio[channel] = commonRatio;
            context.channels->SetBendRatio(channel, commonRatio);
            context.voices->ApplyChannelBendRatio(channel, bendSemitones);
        };
        switch (event.type) {
            case svms::RenderEventType::NoteOn: {
                // Production launch path (LaunchVoiceGroup -> ConfigureVoice),
                // with a transient envelope and a release cap so voices also
                // die naturally mid-scenario.
                const bool loop = (context.playIndex % 3u) != 0u;
                svms::VoiceConfiguration setup{};
                setup.sampleStart = 0u;
                setup.sampleEnd = 4096u;
                setup.loopStart = 64u;
                setup.loopEnd = 4032u;
                setup.loopMode = loop ? 1u : 0u;
                setup.phaseStep = setup.basePhaseStep =
                    0.637f + event.data1 * 0.011f;
                setup.initialGain = 0.6f;
                setup.sustainLevel = 0.5f;
                setup.attackSamples = 17u;
                setup.attackGainStep = 0.6f / 17.0f;
                setup.decaySamples = 53u;
                setup.decaySlope = 0.99f;
                setup.releaseDecay = 0.9995f;
                setup.releaseSamples = 400u;
                setup.gainLeft = setup.gainRight = 0.02f;
                setup.sampleBacked = 1u;
                setup.playIndex = context.playIndex++;
                svms::VoiceHandle handle = svms::kInvalidVoice;
                context.voices->LaunchVoiceGroup(
                    channel, event.data1, event.data2, &setup, 1u,
                    setup.playIndex,
                    context.channelParams[channel], &handle);
                if (handle != svms::kInvalidVoice) {
                    ++context.launches;
                    if (context.vibratoLaunches) {
                        context.voices->v.vibLfoToPitchCents[handle] = 50.0f;
                        context.voices->v.vibLfoSteps[handle] =
                            5.0f / 44100.0f;
                    }
                }
                break;
            }
            case svms::RenderEventType::NoteOff:
            case svms::RenderEventType::StaleNoteOffBatch: {
                // NoteOffPlayIndex semantics: pedal-held keys take the held
                // flag (the key chain is managed inside StartRelease for the
                // free case); free keys release at this exact frame.
                const bool sustain =
                    context.channels->IsSustainActive(channel);
                for (uint32_t i = 0; i < context.voices->activeCount_; ++i) {
                    const uint32_t voice = context.voices->activeList_[i];
                    if (context.voices->v.channel[voice] != channel ||
                        context.voices->v.note[voice] != event.data1) {
                        continue;
                    }
                    context.voices->v.releaseStartInBlock[voice] =
                        blockCursor;
                    if (sustain ||
                        context.voices->v.heldBySostenuto[voice]) {
                        if (sustain)
                            context.voices->v.heldBySustain[voice] = 1u;
                    } else {
                        context.voices->StartRelease(
                            static_cast<svms::VoiceHandle>(voice));
                    }
                }
                break;
            }
            case svms::RenderEventType::ControlChange:
                switch (event.data1) {
                    case 64u:
                        context.channels->ControlChange(channel, 64u,
                                                        event.data2);
                        if (event.data2 < 64u)
                            context.voices->ReleaseSustain(channel,
                                                           blockCursor);
                        break;
                    case 66u: {
                        const bool wasActive =
                            context.channels->IsSostenutoActive(channel);
                        context.channels->ControlChange(channel, 66u,
                                                        event.data2);
                        const bool nowActive = event.data2 >= 64u;
                        if (!wasActive && nowActive)
                            context.voices->CaptureSostenuto(channel);
                        else if (wasActive && !nowActive)
                            context.voices->ReleaseSostenuto(channel,
                                                             blockCursor);
                        break;
                    }
                    case 120u:
                        context.voices->SilenceChannelImmediate(channel);
                        break;
                    case 123u:
                        context.voices->ReleaseChannel(channel, blockCursor);
                        break;
                    case 6u:
                    case 38u:
                        // RPN data entry mirrors HandleControlChange's wheel
                        // re-application.
                        context.channels->ControlChange(channel, event.data1,
                                                        event.data2);
                        applyBend(
                            context.channels->GetPitchBendValue(channel));
                        break;
                    case 1u:
                        // Mod wheel mirrors HandleControlChange: rebuild,
                        // restore the sysex-aware bend ratio, report the
                        // post-rebuild modulation depth as a vibrato op.
                        context.channels->ControlChange(channel, 1u,
                                                        event.data2);
                        context.channels->RebuildChannel(
                            channel, *context.config, 44100.0f);
                        context.channels->SetBendRatio(
                            channel, context.channelBendRatio[channel]);
                        context.voices->MarkChannelVibratoDepth(
                            channel,
                            context.channels->GetParams()[channel].modDepth);
                        break;
                    case 7u:
                    case 10u:
                    case 11u:
                        context.channels->ControlChange(channel, event.data1,
                                                        event.data2);
                        context.channels->RebuildChannel(
                            channel, *context.config, 44100.0f);
                        context.voices->MarkChannelMixStale(
                            channel,
                            context.channels->GetParams()[channel]);
                        break;
                    case 121u: {
                        const bool sustainWasActive =
                            context.channels->IsSustainActive(channel);
                        const bool sostenutoWasActive =
                            context.channels->IsSostenutoActive(channel);
                        context.channels->ControlChange(channel, 121u,
                                                        event.data2);
                        if (sustainWasActive)
                            context.voices->ReleaseSustain(channel,
                                                           blockCursor);
                        if (sostenutoWasActive)
                            context.voices->ReleaseSostenuto(channel,
                                                             blockCursor);
                        // Reset All Controllers also centers the wheel.
                        applyBend(8192u);
                        context.channels->RebuildChannel(
                            channel, *context.config, 44100.0f);
                        context.channels->SetBendRatio(
                            channel, context.channelBendRatio[channel]);
                        context.voices->MarkChannelMixStale(
                            channel,
                            context.channels->GetParams()[channel]);
                        context.voices->MarkChannelVibratoDepth(
                            channel,
                            context.channels->GetParams()[channel].modDepth);
                        break;
                    }
                    default:
                        break;
                }
                break;
            case svms::RenderEventType::ProgramChange:
                // No soundfont stack in the harness: the driver handler
                // early-outs before touching any state, so this is a
                // dispatch-only event here (still exercises the whitelisting
                // and the ingress ordering against same-frame launches).
                break;
            case svms::RenderEventType::RhythmPart:
                if (channel < svms::kChannelCount)
                    context.channels->SetRhythmPart(channel, event.data1);
                break;
            case svms::RenderEventType::MasterVolume: {
                // Mirrors Driver::DispatchRenderEvent's MasterVolume handler:
                // cache-wide rebuild, then a mix mark per channel.
                const uint16_t value = static_cast<uint16_t>(
                    event.data1 | (static_cast<uint16_t>(event.data2) << 7u));
                const float effective = context.config->masterVolume *
                    (static_cast<float>(value) / 16383.0f);
                context.channels->SetMasterVolume(effective);
                context.channels->RebuildCache(*context.config, 44100.0f);
                for (uint8_t ch = 0; ch < svms::kChannelCount; ++ch) {
                    context.voices->MarkChannelMixStale(
                        ch, context.channels->GetParams()[ch]);
                    context.channels->SetBendRatio(
                        ch, context.channelBendRatio[ch]);
                }
                break;
            }
            case svms::RenderEventType::MasterFineTune:
            case svms::RenderEventType::MasterTranspose:
                // Mirrors Driver::RefreshAllPitchIncrements (minus the
                // launch-revision bookkeeping the harness does not model).
                if (event.type == svms::RenderEventType::MasterFineTune) {
                    const uint16_t value = static_cast<uint16_t>(
                        event.data1 |
                        (static_cast<uint16_t>(event.data2) << 7u));
                    context.masterFineTune = static_cast<float>(
                        static_cast<int32_t>(value) - 8192) / 8192.0f;
                } else {
                    context.masterTranspose = static_cast<float>(
                        static_cast<int32_t>(event.data1) - 64);
                }
                for (uint8_t ch = 0; ch < svms::kChannelCount; ++ch) {
                    const float semitones =
                        context.channels->GetPitchBendSemitones(ch) +
                        context.masterFineTune + context.masterTranspose;
                    const float commonRatio =
                        std::pow(2.0f, semitones / 12.0f);
                    context.channelBendRatio[ch] = commonRatio;
                    context.channels->SetBendRatio(ch, commonRatio);
                    context.voices->ApplyChannelBendRatio(ch, semitones);
                }
                break;
            case svms::RenderEventType::PitchBend:
                applyBend(static_cast<uint16_t>((event.data2 << 7) |
                                                event.data1));
                break;
            case svms::RenderEventType::ChannelPressure:
                // Mirrors Driver::HandleChannelPressure: cache update,
                // rebuild, sysex-aware ratio restore, vibrato depth report.
                context.channels->ChannelPressure(channel, event.data1);
                context.channels->RebuildChannel(channel, *context.config,
                                                 44100.0f);
                context.channels->SetBendRatio(
                    channel, context.channelBendRatio[channel]);
                context.voices->MarkChannelVibratoDepth(
                    channel,
                    context.channels->GetParams()[channel].modDepth);
                break;
            default:
                break;
        }
    }
}

void RunWholeVoiceCCDifferential(uint32_t rowOpBudget) {
    // Notes interleaved with lifecycle CCs: pedal capture/release chains,
    // all-notes-off sweeps and mid-block all-sound-off kills (which free
    // slots that later same-block launches reuse, and which kill pre-existing
    // steal tails).  The whole-voice renderer must reproduce the frame-major
    // oracle per sample — through ghosts for CC120 victims and exact-frame
    // deferred releases for CC64/66/123.
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 8u;
    constexpr uint32_t kPool = 48u;
    constexpr uint32_t kSeedCount = 32u;
    constexpr uint32_t kEventsPerBlock = 12u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.4f * std::sin(static_cast<float>(i) * 0.031f) +
             0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (int backendPass = 0; backendPass < 2; ++backendPass) {
        const bool avx2Pass = backendPass == 1;
        if (avx2Pass &&
            !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) {
            break;
        }
        const char* backendName = avx2Pass ? "avx2" : "scalar";

        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool,
                                    kSeedCount);
        auto oracleVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        svms::ChannelCache oracleChannels = seedChannels;
        svms::ChannelCache wholeChannels = seedChannels;

        svms::RenderScalar oracle;
        svms::RenderScalar whole;
        if (rowOpBudget != 0u) whole.SetRowOpBudgetForTest(rowOpBudget);
        Check(oracle.ReserveVoiceCapacity(kPool) &&
                  whole.ReserveVoiceCapacity(kPool) &&
                  oracle.ConfigureRenderThreads(1u, kFrames) &&
                  whole.ConfigureRenderThreads(4u, kFrames),
              "whole-voice CC differential starts serial and parallel "
              "renderers");
        Check(whole.SetRenderBackend(avx2Pass ? svms::RenderBackend::AVX2
                                              : svms::RenderBackend::Scalar),
              "whole-voice CC differential selects the requested backend");

        WholeVoiceCCContext oracleContext{oracleVoices.get(), &oracleChannels,
                                          oracleChannels.GetParams(), &cfg};
        WholeVoiceCCContext wholeContext{wholeVoices.get(), &wholeChannels,
                                         wholeChannels.GetParams(), &cfg};
        oracleContext.ResetMasterState();
        wholeContext.ResetMasterState();
        oracle.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent,
                                       &oracleContext);
        whole.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent,
                                      &wholeContext);

        WholeVoiceRng rng;
        float passMaxDiff = 0.0f;
        for (uint32_t block = 0; block < kBlocks; ++block) {
            const uint64_t blockStartFrame = 20000u + block * kFrames;
            std::vector<svms::RenderEvent> events;
            events.reserve(kEventsPerBlock);
            for (uint32_t j = 0; j < kEventsPerBlock; ++j) {
                svms::RenderEvent event{};
                event.channel = static_cast<uint8_t>(rng.Next() % 4u);
                event.frameOffset = rng.Next() % kFrames;
                event.ingressSequence = block * kEventsPerBlock + j;
                const uint32_t roll = rng.Next() % 12u;
                if (roll < 4u) {
                    event.type = svms::RenderEventType::NoteOn;
                    event.data1 =
                        static_cast<uint8_t>(36u + rng.Next() % 72u);
                    event.data2 =
                        static_cast<uint8_t>(64u + rng.Next() % 64u);
                } else if (roll < 6u) {
                    event.type = svms::RenderEventType::NoteOff;
                    event.data1 =
                        static_cast<uint8_t>(36u + rng.Next() % 72u);
                } else if (roll == 6u) {
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 64u;
                    event.data2 = (rng.Next() % 2u) != 0u ? 127u : 0u;
                } else if (roll == 7u) {
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 66u;
                    event.data2 = (rng.Next() % 2u) != 0u ? 127u : 0u;
                } else if (roll == 8u) {
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 120u;
                    event.data2 = 0u;
                } else if (roll == 9u) {
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = 123u;
                    event.data2 = 0u;
                } else if (roll == 10u) {
                    // Mix-fold controllers: CC7/10/11 land as gain ops.
                    static const uint8_t kMixControllers[3] = {7u, 10u, 11u};
                    event.type = svms::RenderEventType::ControlChange;
                    event.data1 = kMixControllers[rng.Next() % 3u];
                    event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
                } else {
                    // Row-mutator variety: pitch bend, reset-all, RPN data,
                    // SysEx master events, program/rhythm launch-time state.
                    const uint32_t pick = rng.Next() % 9u;
                    if (pick == 0u) {
                        event.type = svms::RenderEventType::PitchBend;
                        event.data1 =
                            static_cast<uint8_t>(rng.Next() % 128u);
                        event.data2 =
                            static_cast<uint8_t>(rng.Next() % 128u);
                    } else if (pick <= 3u) {
                        static const uint8_t kRowControllers[3] = {
                            121u, 6u, 38u};
                        event.type = svms::RenderEventType::ControlChange;
                        event.data1 = kRowControllers[pick - 1u];
                        event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
                    } else if (pick == 4u) {
                        event.type = svms::RenderEventType::MasterVolume;
                        const uint32_t value = rng.Next() % 16384u;
                        event.data1 = static_cast<uint8_t>(value & 0x7Fu);
                        event.data2 = static_cast<uint8_t>(value >> 7u);
                    } else if (pick == 5u) {
                        event.type = svms::RenderEventType::MasterFineTune;
                        const uint32_t value = rng.Next() % 16384u;
                        event.data1 = static_cast<uint8_t>(value & 0x7Fu);
                        event.data2 = static_cast<uint8_t>(value >> 7u);
                    } else if (pick == 6u) {
                        event.type = svms::RenderEventType::MasterTranspose;
                        event.data1 =
                            static_cast<uint8_t>(rng.Next() % 128u);
                    } else if (pick == 7u) {
                        event.type = svms::RenderEventType::ProgramChange;
                        event.data1 =
                            static_cast<uint8_t>(rng.Next() % 128u);
                    } else {
                        event.type = svms::RenderEventType::RhythmPart;
                        event.data1 =
                            static_cast<uint8_t>(rng.Next() % 2u);
                    }
                }
                events.push_back(event);
            }
            std::stable_sort(events.begin(), events.end(),
                [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                    return a.frameOffset < b.frameOffset;
                });

            std::vector<float> oracleLeft(kFrames, 0.0f),
                oracleRight(kFrames, 0.0f);
            std::vector<float> wholeLeft(kFrames, 0.0f),
                wholeRight(kFrames, 0.0f);
            oracle.RenderBlockReference(
                *oracleVoices, oracleChannels, samples.data(), sampleCount,
                oracleLeft.data(), oracleRight.data(), kFrames, cfg,
                events.data(), static_cast<uint32_t>(events.size()), true,
                blockStartFrame);
            whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                              sampleCount, wholeLeft.data(), wholeRight.data(),
                              kFrames, cfg, events.data(),
                              static_cast<uint32_t>(events.size()), true,
                              blockStartFrame);

            float blockDiff = 0.0f;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleLeft[frame] - wholeLeft[frame]));
                blockDiff = (std::max)(blockDiff,
                    std::fabs(oracleRight[frame] - wholeRight[frame]));
            }
            if (blockDiff > g_waveTol) {
                for (uint32_t frame = 0; frame < kFrames; ++frame) {
                    if (std::fabs(oracleLeft[frame] - wholeLeft[frame]) >
                            g_waveTol ||
                        std::fabs(oracleRight[frame] - wholeRight[frame]) >
                            g_waveTol) {
                        std::fprintf(stderr,
                            "[WVDIFF] backend=%s cc-block=%u first-diff "
                            "frame=%u\n",
                            backendName, block, frame);
                        break;
                    }
                }
                for (const svms::RenderEvent& e : events) {
                    std::fprintf(stderr,
                        "[WVDIFFEV] blk=%u f=%u t=%d ch=%u d1=%u d2=%u\n",
                        block, e.frameOffset, static_cast<int>(e.type),
                        e.channel, e.data1, e.data2);
                }
            }
            std::fprintf(stderr,
                         "[WVDIFF] backend=%s cc-block=%u max=%g\n",
                         backendName, block, blockDiff);
            passMaxDiff = (std::max)(passMaxDiff, blockDiff);
        }
        std::fprintf(stderr, "[WVDIFF] backend=%s cc max=%g launches=%u\n",
                     backendName, passMaxDiff, wholeContext.launches);
        Check(passMaxDiff <= g_waveTol,
              "whole-voice lifecycle-CC blocks match the oracle per sample");
        CheckWholeVoiceStateParity(*oracleVoices, *wholeVoices, backendName);
        if (rowOpBudget != 0u) {
            Check(whole.GetPathBlocks(0) > kBlocks && whole.GetPathBlocks(1) == 0u &&
                      whole.GetPathBlocks(2) == 0u,
                  "op budget cut blocks into more whole-voice stretches, none fell back");
        }
        Check(rowOpBudget != 0u || whole.GetWholeVoiceBlocksForTest() == kBlocks,
              "CC scenario routes every block through the whole-voice "
              "renderer");
        Check(wholeContext.launches >= kBlocks,
              "CC scenario exercised in-block launches alongside the "
              "controllers");
    }
}

void TestWholeVoiceCCDifferential() { RunWholeVoiceCCDifferential(0u); }

// quality.fast_chunk_math: closed-form gain/phase per chunk is tolerance-equal
// (not bit-equal) to the frame-major oracle.  Re-run the steal, launch and CC
// differentials (their AVX2 passes carry release tails and note-off churn)
// with the toggle on, at the same tolerances, then restore the exact default.
void TestFastChunkMathDifferential() {
    svms::SetFastChunkMath(true);
    // The oracle advances phase with one float add per frame, which at large
    // phases drifts whole frames over a block (8 adds round in one direction);
    // the closed-form chunk advance is the closer one to the true phase
    // (.perf/phasedrift.py: 0.1-0.7 frames vs 0.9-3.5 after 2048 frames).  The
    // wave tolerance therefore has to cover the oracle's own drift on a
    // steep waveform, not just float-ulp differences.
    g_waveTol = 4.0e-3f;
    g_phaseTol = 0.5f;     // frames, as for muted rows
    RunWholeVoiceStealDifferential(0u);
    RunWholeVoiceLaunchDifferential(0u);
    RunWholeVoiceCCDifferential(0u);
    g_waveTol = 2.0e-5f;
    g_phaseTol = 2.0e-3f;
    svms::SetFastChunkMath(false);
}


// A tiny op-log budget forces the block splitter to cut stretches between
// event groups (no dispatch, same frames and order); the result must still
// match the oracle, and no block may fall back to dense or sparse.
void TestWholeVoiceOpBudgetSplitDifferential() {
    RunWholeVoiceCCDifferential(3u);
}

// scalarExact: the whole-voice side runs the SCALAR backend (sparse-identical
// LFO windows) and must match the forced-sparse reference to float rounding;
// otherwise the whole-voice side is AVX2 with the documented control-rate drift.
void RunWholeVoiceVibratoDifferential(bool scalarExact) {
    // CC1/channel-pressure vibrato rides the whole-voice path on every
    // backend.  AVX2 rebuilds the 64-frame LFO windows inside its kernels
    // (documented control-rate drift vs the reference); scalar/SSE2 render
    // in the exact windows the span renderer used.  The reference is the
    // frozen output of that span renderer (VibratoGolden.h), captured
    // before it was removed.  Channel 0 stays modulated from block 0,
    // mid-block CC1 sweeps depth on/mid/off, and channels 1-3 exercise the
    // per-channel gating with their own CC1/pressure events.
    if (!scalarExact &&
        !svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) return;

    constexpr uint32_t kFrames = 512u;
    // The exact-window scenario stops before the saturated pool starts to
    // steal and retire: those paths diverge from the reference by design
    // (whole-voice scores victims at block start, retires at block end) and
    // are covered by the steal differentials.
    const uint32_t kBlocks = scalarExact ? 6u : 8u;
    constexpr uint32_t kPool = 48u;
    constexpr uint32_t kSeedCount = 32u;
    constexpr uint32_t kEventsPerBlock = 10u;
    const float kMaxWaveDiff = scalarExact ? 2.0e-5f : 3.0e-2f;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.4f * std::sin(static_cast<float>(i) * 0.031f) +
             0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    svms::ChannelCache seedChannels;
    seedChannels.SetMasterVolume(1.0f);
    seedChannels.ControlChange(0, 1, 127);
    seedChannels.RebuildCache(cfg, 44100.0f);

    auto seedVoices = std::make_unique<svms::VoiceManager>();
    ConfigureWholeVoiceSeedPool(*seedVoices, seedChannels, kPool, kSeedCount,
                                true, true);
    auto wholeVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
    svms::ChannelCache wholeChannels = seedChannels;

    svms::RenderScalar whole;
    Check(whole.ReserveVoiceCapacity(kPool) &&
              whole.ConfigureRenderThreads(4u, kFrames),
          "vibrato differential starts the parallel renderer");
    Check(whole.SetRenderBackend(scalarExact ? svms::RenderBackend::Scalar
                                             : svms::RenderBackend::AVX2),
          "vibrato differential selects the backend");

    WholeVoiceCCContext wholeContext{wholeVoices.get(), &wholeChannels,
                                     wholeChannels.GetParams(), &cfg};
    wholeContext.ResetMasterState();
    wholeContext.vibratoLaunches = true;
    whole.SetEventBatchDispatcher(DispatchWholeVoiceCCEvent, &wholeContext);

    WholeVoiceRng rng;
    float passMaxDiff = 0.0f;
    for (uint32_t block = 0; block < kBlocks; ++block) {
        const uint64_t blockStartFrame = 30000u + block * kFrames;
        std::vector<svms::RenderEvent> events;
        events.reserve(kEventsPerBlock);
        for (uint32_t j = 0; j < kEventsPerBlock; ++j) {
            svms::RenderEvent event{};
            event.channel = (rng.Next() % 2u) != 0u
                ? 0u : static_cast<uint8_t>(1u + rng.Next() % 3u);
            event.frameOffset = rng.Next() % kFrames;
            event.ingressSequence = block * kEventsPerBlock + j;
            const uint32_t roll = rng.Next() % 6u;
            if (roll < 2u) {
                event.type = svms::RenderEventType::NoteOn;
                event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
                event.data2 = static_cast<uint8_t>(64u + rng.Next() % 64u);
            } else if (roll == 2u) {
                event.type = svms::RenderEventType::NoteOff;
                event.data1 = static_cast<uint8_t>(36u + rng.Next() % 72u);
            } else if (roll == 3u) {
                event.type = svms::RenderEventType::ControlChange;
                event.data1 = 1u;
                static const uint8_t kModValues[3] = {127u, 64u, 0u};
                event.data2 = kModValues[rng.Next() % 3u];
            } else if (roll == 4u) {
                event.type = svms::RenderEventType::PitchBend;
                event.data1 = static_cast<uint8_t>(rng.Next() % 128u);
                event.data2 = static_cast<uint8_t>(rng.Next() % 128u);
            } else {
                event.type = svms::RenderEventType::ChannelPressure;
                event.data1 = static_cast<uint8_t>(rng.Next() % 128u);
            }
            events.push_back(event);
        }
        std::stable_sort(events.begin(), events.end(),
            [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                return a.frameOffset < b.frameOffset;
            });

        std::vector<float> wholeLeft(kFrames, 0.0f),
            wholeRight(kFrames, 0.0f);
        whole.RenderBlock(*wholeVoices, wholeChannels, samples.data(),
                          sampleCount, wholeLeft.data(), wholeRight.data(),
                          kFrames, cfg, events.data(),
                          static_cast<uint32_t>(events.size()), true,
                          blockStartFrame);
        float blockDiff = 0.0f;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            blockDiff = (std::max)(blockDiff,
                std::fabs(kVibGoldenL[block * kFrames + frame] -
                          wholeLeft[frame]));
            blockDiff = (std::max)(blockDiff,
                std::fabs(kVibGoldenR[block * kFrames + frame] -
                          wholeRight[frame]));
        }
        std::fprintf(stderr, "[WVDIFFV] blk=%u max=%g\n", block, blockDiff);
        passMaxDiff = (std::max)(passMaxDiff, blockDiff);
    }
    std::fprintf(stderr, "[WVDIFFV] %s vibrato whole-voice vs golden max=%g\n",
                 scalarExact ? "scalar" : "avx2", passMaxDiff);
    Check(passMaxDiff <= kMaxWaveDiff,
          "whole-voice vibrato matches the frozen reference within the "
          "control-rate drift budget");
    Check(whole.GetWholeVoiceBlocksForTest() == kBlocks,
          "vibrato scenario routes every block through the whole-voice "
          "renderer");
    Check(wholeContext.launches >= kBlocks,
          "vibrato scenario exercised in-block launches");
    // Active identity (8-block run only: the 6-block exact run ends before
    // the golden pool state).
    if (!scalarExact) {
        std::vector<uint32_t> wholeIds;
        for (uint32_t i = 0; i < wholeVoices->activeCount_; ++i)
            wholeIds.push_back(
                wholeVoices->v.playIndex[wholeVoices->activeList_[i]]);
        std::sort(wholeIds.begin(), wholeIds.end());
        const std::vector<uint32_t> goldenIds(
            std::begin(kVibGoldenActive), std::end(kVibGoldenActive));
        Check(goldenIds == wholeIds,
              "vibrato scenario preserves active voice identity");
    }
}

void TestWholeVoiceVibratoDifferential() {
    RunWholeVoiceVibratoDifferential(false);
}

// Scalar/SSE2 vibrato renders on the whole-voice path in the sparse loop's
// exact LFO windows (global event + 64-frame boundaries, powf ratio).
void TestWholeVoiceScalarVibratoDifferential() {
    RunWholeVoiceVibratoDifferential(true);
}

// Launch staging (voices.launch_staging) is a pure layout change: with the
// same event stream a staged pool and a direct pool must produce BIT-identical
// audio and identical dense arrays after every block.  The stream saturates a
// small pool so in-place steals dominate, and note-offs target keys launched
// earlier in the same block (release and steal of staged rows).
void RunLaunchStagingIdentity(uint32_t renderThreads, bool avx2Pass,
                              uint32_t layers = 1u) {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 10u;
    constexpr uint32_t kPool = 32u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    const bool savedFlag = svms::g_launchStagingEnabled.load();
    const uint64_t deferredBefore = svms::g_ghostCopiesDeferred.load();
    svms::ChannelCache seedChannels;
    seedChannels.SetMasterVolume(1.0f);
    seedChannels.RebuildCache(cfg, 44100.0f);

    svms::g_launchStagingEnabled.store(false);
    auto directVoices = std::make_unique<svms::VoiceManager>();
    ConfigureWholeVoiceSeedPool(*directVoices, seedChannels, kPool, kPool);
    svms::g_launchStagingEnabled.store(true);
    auto stagedVoices = std::make_unique<svms::VoiceManager>();
    ConfigureWholeVoiceSeedPool(*stagedVoices, seedChannels, kPool, kPool);
    svms::g_launchStagingEnabled.store(savedFlag);
    // Make every seed louder than any newborn (steal score is level based) so
    // the quietest victims are voices launched earlier in the same block:
    // staged rows get released, stolen and ghost-copied before any render.
    for (svms::VoiceManager* pool : {directVoices.get(), stagedVoices.get()}) {
        for (uint32_t h = 0; h < kPool; ++h) {
            pool->SetVoiceEnvelope(h, 40.0f, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f,
                                   0.999f);
            pool->SetVoiceGain(h, 0.2f, 0.2f);
            pool->RefreshMixGain(h, seedChannels.GetParams()[pool->v.channel[h]]);
        }
    }
    Check(directVoices->v.launch == nullptr && stagedVoices->v.launch != nullptr,
          "launch staging identity: only the staged pool owns records");

    svms::ChannelCache directChannels = seedChannels;
    svms::ChannelCache stagedChannels = seedChannels;
    svms::RenderScalar direct;
    svms::RenderScalar staged;
    Check(direct.ReserveVoiceCapacity(kPool) &&
              staged.ReserveVoiceCapacity(kPool) &&
              direct.ConfigureRenderThreads(renderThreads, kFrames) &&
              staged.ConfigureRenderThreads(renderThreads, kFrames),
          "launch staging identity starts both renderers");
    const svms::RenderBackend backend = avx2Pass
        ? svms::RenderBackend::AVX2 : svms::RenderBackend::Scalar;
    Check(direct.SetRenderBackend(backend) && staged.SetRenderBackend(backend),
          "launch staging identity selects the backend");

    WholeVoiceLaunchContext directContext{directVoices.get(),
                                          directChannels.GetParams()};
    WholeVoiceLaunchContext stagedContext{stagedVoices.get(),
                                          stagedChannels.GetParams()};
    directContext.layers = stagedContext.layers = layers;
    direct.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                   &directContext);
    staged.SetEventBatchDispatcher(DispatchWholeVoiceLaunchEvent,
                                   &stagedContext);

    WholeVoiceRng rng;
    uint32_t mismatchedBlocks = 0u;
    for (uint32_t block = 0; block < kBlocks; ++block) {
        const uint64_t blockStartFrame = 20000u + block * kFrames;
        std::vector<svms::RenderEvent> events;
        uint32_t sequence = 0u;
        const auto push = [&](svms::RenderEventType type, uint8_t channel,
                              uint8_t data1, uint8_t data2, uint32_t frame) {
            svms::RenderEvent event{};
            event.type = type;
            event.channel = channel;
            event.data1 = data1;
            event.data2 = data2;
            event.frameOffset = frame;
            event.ingressSequence = sequence++;
            events.push_back(event);
        };
        // 24 note-ons (each steals), a note-off for most of them a few frames
        // after launch, and a few repeated keys so same-key chains form.
        for (uint32_t j = 0; j < 24u; ++j) {
            const uint32_t frame = rng.Next() % (kFrames - 40u);
            const uint8_t channel = static_cast<uint8_t>(rng.Next() % 4u);
            const uint8_t key = static_cast<uint8_t>(40u + rng.Next() % 24u);
            push(svms::RenderEventType::NoteOn, channel, key,
                 static_cast<uint8_t>(40u + rng.Next() % 80u), frame);
            if ((rng.Next() % 4u) != 0u)
                push(svms::RenderEventType::NoteOff, channel, key, 0u,
                     frame + 1u + rng.Next() % 38u);
        }
        std::stable_sort(events.begin(), events.end(),
            [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                return a.frameOffset < b.frameOffset;
            });
        std::vector<float> directLeft(kFrames, 0.0f), directRight(kFrames, 0.0f);
        std::vector<float> stagedLeft(kFrames, 0.0f), stagedRight(kFrames, 0.0f);
        direct.RenderBlock(*directVoices, directChannels, samples.data(),
                           sampleCount, directLeft.data(), directRight.data(),
                           kFrames, cfg, events.data(),
                           static_cast<uint32_t>(events.size()), true,
                           blockStartFrame);
        staged.RenderBlock(*stagedVoices, stagedChannels, samples.data(),
                           sampleCount, stagedLeft.data(), stagedRight.data(),
                           kFrames, cfg, events.data(),
                           static_cast<uint32_t>(events.size()), true,
                           blockStartFrame);
        bool same =
            std::memcmp(directLeft.data(), stagedLeft.data(),
                        kFrames * sizeof(float)) == 0 &&
            std::memcmp(directRight.data(), stagedRight.data(),
                        kFrames * sizeof(float)) == 0;
        same = same && directVoices->GetActiveCount() ==
                           stagedVoices->GetActiveCount();
        const uint32_t cap = (std::min)(directVoices->GetMaxVoices(),
                                        stagedVoices->GetMaxVoices());
#define SVMS_CMP_FIELD(type, name) \
        same = same && std::memcmp(directVoices->v.name, stagedVoices->v.name, \
                                   cap * sizeof(type)) == 0;
        SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_CMP_FIELD)
#undef SVMS_CMP_FIELD
        if (!same) ++mismatchedBlocks;
        Check(stagedVoices->CountStagedRows() == 0u,
              "launch staging identity: no row stays staged after a block");
    }
    Check(mismatchedBlocks == 0u,
          "launch staging identity: staged and direct pools render and "
          "store bit-identical blocks");
    Check(stagedContext.launches > 100u,
          "launch staging identity exercised the launch path");
    Check(svms::g_ghostCopiesDeferred.load() > deferredBefore,
          "launch staging identity: no ghost copy was deferred");
}

void TestLaunchStagingIdentity() {
    for (uint32_t threads : {1u, 4u}) {
        for (uint32_t layers : {1u, 2u}) {
            RunLaunchStagingIdentity(threads, false, layers);
            if (svms::IsRenderBackendSupported(svms::RenderBackend::AVX2))
                RunLaunchStagingIdentity(threads, true, layers);
        }
    }
}

// Lazy same-frame launch (DropZeroLifeVictimsEnabled): a launch whose victim
// group was born on the same frame is parked as pending and committed through
// the normal launch code later.  With zero-life tails dropped on BOTH sides, a
// lazy pool and a normally-launching pool must stay BIT-identical: audio and
// every dense/row array after every block.  The stream uses two-layer notes
// (the shape that takes the group-reuse path), many notes per frame so chains
// of same-frame steals form, and note-offs a few frames after the note-ons.
struct LazyLaunchContext {
    svms::VoiceManager* voices;
    const svms::ChannelParamsSnapshot* channelParams;
    bool lazy = false;
    bool graded = false;   // level follows velocity (steal-reject runs)
    uint32_t playIndex = 7000u;
    uint32_t launches = 0u;
};

void DispatchLazyLaunchEvents(const svms::RenderEvent* events,
                              uint32_t eventCount, uint32_t, void* userData) {
    auto& c = *static_cast<LazyLaunchContext*>(userData);
    uint32_t i = 0u;
    while (i < eventCount) {
        if (events[i].type == svms::RenderEventType::NoteOn) {
            const bool lazyRun = c.lazy && c.voices->BeginLazyLaunchRun();
            while (i < eventCount &&
                   events[i].type == svms::RenderEventType::NoteOn) {
                const svms::RenderEvent& e = events[i++];
                svms::VoiceConfiguration setups[2];
                for (uint32_t layer = 0u; layer < 2u; ++layer) {
                    svms::VoiceConfiguration& s = setups[layer];
                    s.sampleStart = 0u;
                    s.sampleEnd = 4096u;
                    s.loopStart = 64u;
                    s.loopEnd = 4032u;
                    s.loopMode = 1u;
                    s.phaseStep = s.basePhaseStep =
                        (0.5f + 0.2f * static_cast<float>(layer)) +
                        e.data1 * 0.009f;
                    s.initialGain = c.graded ? 0.05f * static_cast<float>(e.data2)
                        : (e.data2 >= 100u ? 8.0f : 0.6f);
                    s.sustainLevel = 0.5f;
                    s.attackSamples = 17u;
                    s.attackGainStep = s.initialGain / 17.0f;
                    s.decaySamples = 53u;
                    s.decaySlope = 0.99f;
                    s.releaseDecay = 0.9995f;
                    s.releaseSamples = 400u;
                    s.gainLeft = 0.02f + 0.004f * static_cast<float>(layer);
                    s.gainRight = 0.02f - 0.004f * static_cast<float>(layer);
                    s.sampleBacked = 1u;
                }
                svms::VoiceHandle handles[2] = {svms::kInvalidVoice,
                                                svms::kInvalidVoice};
                const uint32_t playIndex = c.playIndex++;
                setups[0].playIndex = setups[1].playIndex = playIndex;
                if (c.voices->LaunchVoiceGroup(
                        e.channel, e.data1, e.data2, setups, 2u, playIndex,
                        c.channelParams[e.channel], handles))
                    ++c.launches;
            }
            if (lazyRun) c.voices->EndLazyLaunchRun();
            continue;
        }
        const svms::RenderEvent& e = events[i++];
        if (e.type == svms::RenderEventType::NoteOff) {
            for (uint32_t a = 0; a < c.voices->activeCount_; ++a) {
                const uint32_t voice = c.voices->activeList_[a];
                if (c.voices->v.channel[voice] == e.channel &&
                    c.voices->v.note[voice] == e.data1)
                    c.voices->StartRelease(voice);
            }
        }
    }
}

void RunLazyLaunchIdentity(uint32_t renderThreads, bool avx2Pass,
                           bool stealReject) {
    constexpr uint32_t kFrames = 512u;
    constexpr uint32_t kBlocks = 12u;
    constexpr uint32_t kPool = 32u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            (0.35f * std::sin(static_cast<float>(i) * 0.071f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    const bool savedDrop = svms::g_dropZeroLifeVictims.load();
    svms::g_dropZeroLifeVictims.store(true);

    svms::ChannelCache seedChannels;
    seedChannels.SetMasterVolume(1.0f);
    seedChannels.RebuildCache(cfg, 44100.0f);
    auto normalVoices = std::make_unique<svms::VoiceManager>();
    auto lazyVoices = std::make_unique<svms::VoiceManager>();
    Check(normalVoices->Initialize(kPool, 44100u) &&
              lazyVoices->Initialize(kPool, 44100u),
          "lazy launch identity allocates both pools");
    normalVoices->SetStealReject(stealReject);
    lazyVoices->SetStealReject(stealReject);
    svms::ChannelCache normalChannels = seedChannels;
    svms::ChannelCache lazyChannels = seedChannels;
    svms::RenderScalar normal;
    svms::RenderScalar lazy;
    Check(normal.ReserveVoiceCapacity(kPool) &&
              lazy.ReserveVoiceCapacity(kPool) &&
              normal.ConfigureRenderThreads(renderThreads, kFrames) &&
              lazy.ConfigureRenderThreads(renderThreads, kFrames),
          "lazy launch identity starts both renderers");
    const svms::RenderBackend backend = avx2Pass
        ? svms::RenderBackend::AVX2 : svms::RenderBackend::Scalar;
    Check(normal.SetRenderBackend(backend) && lazy.SetRenderBackend(backend),
          "lazy launch identity selects the backend");

    LazyLaunchContext normalContext{normalVoices.get(),
                                    normalChannels.GetParams()};
    LazyLaunchContext lazyContext{lazyVoices.get(), lazyChannels.GetParams()};
    lazyContext.lazy = true;
    normalContext.graded = lazyContext.graded = stealReject;
    normal.SetEventBatchDispatcher(DispatchLazyLaunchEvents, &normalContext);
    lazy.SetEventBatchDispatcher(DispatchLazyLaunchEvents, &lazyContext);

    WholeVoiceRng rng;
    uint32_t mismatchedBlocks = 0u;
    for (uint32_t block = 0; block < kBlocks; ++block) {
        const uint64_t blockStartFrame = 30000u + block * kFrames;
        std::vector<svms::RenderEvent> events;
        uint32_t sequence = 0u;
        const auto push = [&](svms::RenderEventType type, uint8_t channel,
                              uint8_t data1, uint8_t data2, uint32_t frame) {
            svms::RenderEvent event{};
            event.type = type;
            event.channel = channel;
            event.data1 = data1;
            event.data2 = data2;
            event.frameOffset = frame;
            event.ingressSequence = sequence++;
            events.push_back(event);
        };
        if (block == 0u) {
            // Fill the pool with loud two-layer groups (never the quietest).
            for (uint32_t j = 0; j < kPool / 2u; ++j)
                push(svms::RenderEventType::NoteOn,
                     static_cast<uint8_t>(j & 3u),
                     static_cast<uint8_t>(30u + j),
                     stealReject && j == 0u ? 20u : 110u, 2u + j);
        } else {
            // 6 frames per block, 5 quiet note-ons each (same-frame chains),
            // note-offs a few frames later.
            for (uint32_t f = 0; f < 6u; ++f) {
                const uint32_t frame = 20u + f * 70u + rng.Next() % 5u;
                for (uint32_t j = 0; j < 5u; ++j) {
                    const uint8_t channel = static_cast<uint8_t>(rng.Next() % 4u);
                    const uint8_t key = static_cast<uint8_t>(50u + rng.Next() % 24u);
                    // Rising velocities inside a frame keep same-frame chains
                    // alive under steal-reject (each newcomer beats the
                    // pending note it replaces). A dip lands between the
                    // pending note and the one before it: a stale-row score
                    // admits it, the pending score rejects it.
                    const uint32_t velocity = stealReject
                        ? 20u + (block * 6u + f) + 6u * j -
                              (j != 0u && rng.Next() % 4u == 0u ? 9u : 0u)
                        : 40u + rng.Next() % 50u;
                    push(svms::RenderEventType::NoteOn, channel, key,
                         static_cast<uint8_t>(velocity), frame);
                    if ((rng.Next() % 3u) != 0u)
                        push(svms::RenderEventType::NoteOff, channel, key, 0u,
                             frame + 1u + rng.Next() % 30u);
                }
            }
        }
        std::stable_sort(events.begin(), events.end(),
            [](const svms::RenderEvent& a, const svms::RenderEvent& b) {
                return a.frameOffset < b.frameOffset;
            });
        std::vector<float> normalLeft(kFrames, 0.0f), normalRight(kFrames, 0.0f);
        std::vector<float> lazyLeft(kFrames, 0.0f), lazyRight(kFrames, 0.0f);
        normal.RenderBlock(*normalVoices, normalChannels, samples.data(),
                           sampleCount, normalLeft.data(), normalRight.data(),
                           kFrames, cfg, events.data(),
                           static_cast<uint32_t>(events.size()), true,
                           blockStartFrame);
        lazy.RenderBlock(*lazyVoices, lazyChannels, samples.data(),
                         sampleCount, lazyLeft.data(), lazyRight.data(),
                         kFrames, cfg, events.data(),
                         static_cast<uint32_t>(events.size()), true,
                         blockStartFrame);
        bool same =
            std::memcmp(normalLeft.data(), lazyLeft.data(),
                        kFrames * sizeof(float)) == 0 &&
            std::memcmp(normalRight.data(), lazyRight.data(),
                        kFrames * sizeof(float)) == 0 &&
            normalVoices->GetActiveCount() == lazyVoices->GetActiveCount() &&
            std::memcmp(normalVoices->activeList_, lazyVoices->activeList_,
                        normalVoices->GetActiveCount() * sizeof(uint32_t)) == 0;
#define SVMS_CMP_FIELD(type, name) \
        same = same && std::memcmp(normalVoices->v.name, lazyVoices->v.name, \
                                   kPool * sizeof(type)) == 0;
        SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_CMP_FIELD)
#undef SVMS_CMP_FIELD
        same = same && std::memcmp(normalVoices->v.rows, lazyVoices->v.rows,
                                   kPool * sizeof(svms::VoiceRow)) == 0;
        // The steal index too: chained lazy launches defer its repair to the
        // commit and must leave exactly the tree a normal launch leaves.
        same = same && normalVoices->SameStealIndexAs(*lazyVoices);
        if (!same) ++mismatchedBlocks;
    }
    svms::g_dropZeroLifeVictims.store(savedDrop);
    Check(mismatchedBlocks == 0u,
          "lazy launch identity: lazy and normal pools render and store "
          "bit-identical blocks");
    if (std::getenv("SVMS_LAZYDBG")) std::fprintf(stderr, "LAZYDBG reject=%d chain=%llu starts=%llu rej=%llu launches n=%u l=%u\n", int(stealReject), (unsigned long long)lazyVoices->GetLazyChainSteps(), (unsigned long long)lazyVoices->GetLazyStarts(), (unsigned long long)lazyVoices->StealRejectCount(), normalContext.launches, lazyContext.launches);
    Check(lazyVoices->GetLazyChainSteps() > (stealReject ? 0u : 20u),
          "lazy launch identity exercised chained lazy launches");
    Check(lazyContext.launches == normalContext.launches &&
              lazyContext.launches > 100u,
          "lazy launch identity launches the same notes");
    Check(svms::g_lazyKeyMismatches.load() == 0u,
          "lazy launch identity: lazily written steal keys match the "
          "committed keys");
}

void TestLazyLaunchIdentity() {
    for (uint32_t threads : {1u, 4u}) {
        for (bool reject : {false, true}) {
            RunLazyLaunchIdentity(threads, false, reject);
            if (svms::IsRenderBackendSupported(svms::RenderBackend::AVX2))
                RunLazyLaunchIdentity(threads, true, reject);
        }
    }
}
