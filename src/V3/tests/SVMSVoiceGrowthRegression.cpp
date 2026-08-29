#include "SVMSChannelCache.h"
#include "SVMSLiveControl.h"
#include "SVMSRenderScalar.h"
#include "SVMSVoiceManager.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool NearlyEqual(float a, float b, float epsilon = 1.0e-6f) {
    return std::fabs(a - b) <= epsilon;
}

} // namespace

// ── Same-frame note-on burst: per-note vs batch-prefetch equivalence ─────
// Mirrors the two DispatchRenderEventBatch paths from identical pool state:
//   * "correctness mode ON"  — one LaunchVoiceGroup per note, no prefetch.
//   * "correctness mode OFF" — the run's victims popped once up front via
//     PopStealCandidates (under the same StealPoolSaturated /
//     StealBatchPrefetchAllowed gates the driver uses), one victim handed to
//     each launch, unconsumed victims re-armed via RearmBatchPoppedVictim.
// Asserts every note lands on the identical physical voice slot(s) and both
// managers end in identical state.
void RunVoiceStealBatchLaunchEquivalenceChecks() {
    constexpr uint32_t kPoolCapacity = 48u;
    constexpr uint32_t kGroupCount = kPoolCapacity / 2u;
    constexpr uint32_t kBurstNotes = 24u;

    svms::ChannelParamsSnapshot channel{};
    channel.volume = channel.expression = 1.0f;
    channel.panLeft = channel.panRight = 0.70710678f;
    channel.mixScaleLeft = channel.mixScaleRight = 0.70710678f;

    svms::VoiceConfiguration setups[2]{};
    for (auto& setup : setups) {
        setup.sampleStart = 0u;
        setup.sampleEnd = 128u;
        setup.loopStart = 8u;
        setup.loopEnd = 120u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep = 1.0f;
        setup.initialGain = setup.sustainLevel = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.sampleBacked = 1u;
    }

    svms::VoiceManager baseSingleton;
    Check(baseSingleton.Initialize(kPoolCapacity, 44100u),
          "steal-batch singleton pool initializes");
    for (uint32_t group = 0u; group < kPoolCapacity; ++group) {
        setups[0].playIndex = group + 1u;
        setups[0].gainLeft = setups[0].gainRight =
            0.02f + 0.01f * static_cast<float>(group % 7u);
        svms::VoiceHandle handles[1]{};
        Check(baseSingleton.LaunchVoiceGroup(0u, 60u, 100u, setups, 1u,
                                             channel, handles),
              "steal-batch singleton fixture group launches");
    }
    Check(baseSingleton.GetActiveCount() == kPoolCapacity &&
              baseSingleton.freeTop_ == 0u,
          "steal-batch singleton fixture pool is saturated");

    svms::VoiceManager baseStereo;
    Check(baseStereo.Initialize(kPoolCapacity, 44100u),
          "steal-batch stereo pool initializes");
    for (uint32_t group = 0u; group < kGroupCount; ++group) {
        setups[0].playIndex = setups[1].playIndex = group + 1u;
        setups[0].gainLeft = setups[0].gainRight =
            0.02f + 0.01f * static_cast<float>(group % 7u);
        setups[1].gainLeft = setups[1].gainRight =
            0.03f + 0.008f * static_cast<float>(group % 5u);
        svms::VoiceHandle handles[2]{};
        Check(baseStereo.LaunchVoiceGroup(0u, 60u, 100u, setups, 2u, channel,
                                          handles),
              "steal-batch stereo fixture group launches");
    }
    Check(baseStereo.GetActiveCount() == kPoolCapacity &&
              baseStereo.freeTop_ == 0u,
          "steal-batch stereo fixture pool is saturated");

    const auto compareManagers = [&](svms::VoiceManager& seq,
                                     svms::VoiceManager& batch,
                                     const char* label) {
        Check(seq.GetActiveCount() == batch.GetActiveCount() &&
                  seq.freeTop_ == batch.freeTop_ &&
                  seq.stealCount_ == batch.stealCount_,
              label);
        bool identical = true;
        for (uint32_t position = 0u;
             identical && position < seq.GetActiveCount(); ++position) {
            const svms::VoiceHandle a = seq.activeList_[position];
            const svms::VoiceHandle b = batch.activeList_[position];
            identical = a == b &&
                seq.v.state[a] == batch.v.state[b] &&
                seq.v.note[a] == batch.v.note[b] &&
                seq.v.channel[a] == batch.v.channel[b] &&
                seq.v.playIndex[a] == batch.v.playIndex[b] &&
                seq.v.renderClass[a] == batch.v.renderClass[b];
        }
        Check(identical, label);
    };

    const auto runBurst = [&](const svms::VoiceManager& basePool,
                              uint32_t layers, bool releaseTailFirst) {
        svms::VoiceManager sequential(basePool);
        svms::VoiceManager batched(basePool);
        if (releaseTailFirst) {
            // Force releasing tails into the pool: the releasing-ring fast
            // path becomes eligible, so the launch-internal gate falls back
            // to the per-layer pop path — both managers must still agree
            // byte for byte.
            for (uint32_t handle = 0u; handle < kPoolCapacity; handle += 5u)
                sequential.StartRelease(static_cast<svms::VoiceHandle>(handle));
            for (uint32_t handle = 0u; handle < kPoolCapacity; handle += 5u)
                batched.StartRelease(static_cast<svms::VoiceHandle>(handle));
        }

        // Mode A (correctness mode ON): batching disabled (default state).
        // Mode B (correctness mode OFF): the driver mirrors the toggle, so
        // each LaunchVoiceGroup batches its layer-victim selection.
        batched.SetStealBatchingEnabled(true);

        svms::VoiceHandle seqHandles[kBurstNotes][3]{};
        svms::VoiceHandle batchHandles[kBurstNotes][3]{};
        for (uint32_t noteIndex = 0u; noteIndex < kBurstNotes; ++noteIndex) {
            setups[0].playIndex = setups[1].playIndex = 9000u + noteIndex;
            Check(sequential.LaunchVoiceGroup(
                      0u, 60u, 100u, setups, layers, channel,
                      seqHandles[noteIndex]),
                  "sequential burst launch succeeds");
            Check(batched.LaunchVoiceGroup(
                      0u, 60u, 100u, setups, layers, channel,
                      batchHandles[noteIndex]),
                  "batched burst launch succeeds");
        }

        bool identical = true;
        for (uint32_t noteIndex = 0u;
             identical && noteIndex < kBurstNotes; ++noteIndex) {
            for (uint32_t layer = 0u; layer < layers; ++layer)
                identical = seqHandles[noteIndex][layer] ==
                            batchHandles[noteIndex][layer];
        }
        Check(identical,
              "burst victims land on identical physical voice slots");
        compareManagers(sequential, batched,
                        "post-burst manager states are identical");
    };

    runBurst(baseSingleton, 1u, false); // mono control (Try path, no batch)
    runBurst(baseSingleton, 2u, false); // stereo on singletons: batched loop
    runBurst(baseSingleton, 3u, false); // 3 layers on singletons: batched loop
    runBurst(baseStereo, 2u, false);    // stereo on stereo groups: reuse path
    runBurst(baseSingleton, 3u, true);  // releasing tails: gated fallback
    runBurst(baseStereo, 2u, true);     // releasing tails: gated fallback
}

// Test hook granted friend access to VoiceManager::PopStealCandidate so the
// batch wrapper can be compared against genuine single-victim pops.
namespace svms {
struct VoiceStealBatchTestAccess {
    static VoiceHandle PopOne(VoiceManager& vm, uint32_t& position) {
        return vm.PopStealCandidate(position, false);
    }
};
} // namespace svms

namespace {

// Builds a saturated pool (all slots active) with heterogeneous steal
// priorities and a mix of sustained and Releasing states, then proves that
// PopStealCandidates(count, ...) picks exactly the same handles, in exactly
// the same order, as `count` sequential PopStealCandidate(pos, false) calls
// on an identical clone of the pool state.
void RunVoiceStealBatchEquivalenceChecks() {
    constexpr uint32_t kPoolCapacity = 256u;

    svms::VoiceManager voices;
    Check(voices.Initialize(kPoolCapacity, 44100u),
          "steal-batch pool initializes");
    for (uint32_t index = 0u; index < kPoolCapacity; ++index) {
        const svms::VoiceHandle voice = voices.AllocateVoice(
            static_cast<uint8_t>(index & 15u),
            static_cast<uint8_t>(24u + index % 88u),
            static_cast<uint8_t>(1u + (index * 37u) % 127u));
        Check(voice != svms::kInvalidVoice,
              "steal-batch pool fills without stealing");
        if (voice == svms::kInvalidVoice) continue;
        voices.SetVoiceSample(voice, 0u, 4096u, 64u, 4096u - 64u, 1u,
                              0.01f + static_cast<float>(index % 29u) * 0.01f,
                              1u);
        voices.SetVoiceEnvelope(voice, 0.8f, 0.7f, 0u, 0u, 0u, 0u,
                                0.0f, 1.0f, 0.9995f);
        voices.SetVoiceGain(voice, 0.0005f, 0.0005f);
        if (index % 7u == 3u) voices.StartRelease(voice);
    }
    Check(voices.GetActiveCount() == kPoolCapacity,
          "steal-batch pool is fully saturated");

    // Case 1: batch fits comfortably within available candidates.
    {
        svms::VoiceManager sequential(voices);
        svms::VoiceManager batched(voices);

        constexpr uint32_t kBatch = 16u;
        svms::VoiceHandle seqHandles[kBatch];
        uint32_t seqPositions[kBatch];
        uint32_t seqFound = 0u;
        for (; seqFound < kBatch; ++seqFound) {
            const svms::VoiceHandle victim =
                svms::VoiceStealBatchTestAccess::PopOne(
                    sequential, seqPositions[seqFound]);
            if (victim == svms::kInvalidVoice) break;
            seqHandles[seqFound] = victim;
        }

        svms::VoiceHandle batchHandles[kBatch];
        uint32_t batchPositions[kBatch];
        const uint32_t batchFound =
            batched.PopStealCandidates(kBatch, batchHandles, batchPositions);

        Check(batchFound == seqFound,
              "batch pop finds the same victim count as sequential pops");
        bool identical = batchFound == seqFound;
        for (uint32_t i = 0u; identical && i < batchFound; ++i) {
            identical = batchHandles[i] == seqHandles[i] &&
                        batchPositions[i] == seqPositions[i];
        }
        Check(identical,
              "batch victims match sequential pop victims exactly, in order");

        // After the batch, both managers must still agree: one more pop from
        // each must return the same victim (structures repaired identically).
        uint32_t seqNextPos = 0u, batchNextPos = 0u;
        const svms::VoiceHandle seqNext =
            svms::VoiceStealBatchTestAccess::PopOne(sequential, seqNextPos);
        const svms::VoiceHandle batchNext =
            svms::VoiceStealBatchTestAccess::PopOne(batched, batchNextPos);
        Check(seqNext == batchNext && seqNextPos == batchNextPos,
              "post-batch structures agree with post-sequential structures");
    }

    // Case 2: batch larger than the available candidate population must
    // return fewer victims and still match sequential pops one-for-one.
    {
        svms::VoiceManager sequential(voices);
        svms::VoiceManager batched(voices);

        constexpr uint32_t kOversized = kPoolCapacity * 4u;
        std::vector<svms::VoiceHandle> seqHandles;
        std::vector<uint32_t> seqPositions;
        for (;;) {
            uint32_t position = 0u;
            const svms::VoiceHandle victim =
                svms::VoiceStealBatchTestAccess::PopOne(sequential, position);
            if (victim == svms::kInvalidVoice) break;
            seqHandles.push_back(victim);
            seqPositions.push_back(position);
        }
        Check(!seqHandles.empty(),
              "oversized case has a non-empty sequential baseline");

        std::vector<svms::VoiceHandle> batchHandles(kOversized,
                                                    svms::kInvalidVoice);
        std::vector<uint32_t> batchPositions(kOversized, 0u);
        const uint32_t batchFound = batched.PopStealCandidates(
            kOversized, batchHandles.data(), batchPositions.data());
        Check(batchFound == seqHandles.size(),
              "oversized batch drains exactly the same candidate population");
        bool identical = batchFound == seqHandles.size();
        for (uint32_t i = 0u; identical && i < batchFound; ++i) {
            identical = batchHandles[i] == seqHandles[i] &&
                        batchPositions[i] == seqPositions[i];
        }
        Check(identical,
              "drained batch victims match sequential victims exactly");

        // Both fully drained: one further pop must fail identically.
        uint32_t dryPosSeq = 0u, dryPosBatch = 0u;
        Check(svms::VoiceStealBatchTestAccess::PopOne(sequential, dryPosSeq) ==
                  svms::kInvalidVoice &&
                  svms::VoiceStealBatchTestAccess::PopOne(batched,
                                                          dryPosBatch) ==
                      svms::kInvalidVoice,
              "both managers report no candidates after full drain");
    }
}

} // namespace

int main() {
    constexpr uint32_t kOldCapacity = 1024u;
    constexpr uint32_t kGrownCapacity = 2048u;
    constexpr uint32_t kLoweredLimit = 512u;
    constexpr uint32_t kFrames = 16u;
    constexpr uint32_t kSampleFrames = 4096u;

    svms::PublishRuntimeVoicePoolCapacity(kOldCapacity);
    svms::RequestRuntimeVoiceLimit(kOldCapacity);
    svms::PublishAppliedRuntimeVoiceLimit(kOldCapacity);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.interpolation = svms::InterpolationMode::Linear;
    cfg.correctnessMode = true;

    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    std::vector<float> samples(kSampleFrames);
    for (uint32_t i = 0u; i < kSampleFrames; ++i)
        samples[i] = 0.25f + 0.20f * std::sin(static_cast<float>(i) * 0.031f);

    svms::VoiceManager voices;
    Check(voices.Initialize(kOldCapacity, 44100u),
          "initial 1024-voice pool allocates");
    Check(voices.GetAllocatedBytes() ==
              svms::VoiceManager::EstimateAllocatedBytes(kOldCapacity),
          "voice-manager memory estimate matches the reserved layout");

    for (uint32_t index = 0u; index < kOldCapacity; ++index) {
        const svms::VoiceHandle voice = voices.AllocateVoice(
            static_cast<uint8_t>(index & 15u),
            static_cast<uint8_t>(24u + index % 88u),
            static_cast<uint8_t>(64u + index % 64u));
        Check(voice != svms::kInvalidVoice,
              "all initial voices allocate without stealing");
        if (voice == svms::kInvalidVoice) continue;

        voices.SetVoicePlayIndex(voice, index + 1u);
        voices.SetVoiceSample(voice, 0u, kSampleFrames, 64u,
                              kSampleFrames - 64u, 1u,
                              0.25f + static_cast<float>(index % 31u) * 0.01f,
                              1u);
        voices.SetVoiceEnvelope(voice, 0.8f, 0.7f, 0u, 0u, 0u, 0u,
                                0.0f, 1.0f, 0.9995f);
        voices.SetVoiceGain(voice, 0.0005f, 0.0005f);
        voices.RefreshMixGain(voice, channels.GetParams()[index & 15u]);
        voices.v.phases[voice] = static_cast<float>(index % 97u) + 0.375f;
    }

    Check(voices.GetActiveCount() == kOldCapacity,
          "initial pool is fully active before growth");

    // A configured memory ceiling must reject an oversized live request at a
    // render boundary without mutating the existing pool or voice limit.
    svms::ConfigureRuntimeVoiceGrowthCeiling(1536u);
    svms::RequestRuntimeVoiceLimit(kGrownCapacity);
    voices.ApplyRuntimeVoiceLimit(99u);
    Check(voices.GetMaxVoices() == kOldCapacity,
          "memory ceiling prevents oversized physical growth");
    Check(voices.GetVoiceLimit() == kOldCapacity,
          "memory ceiling preserves the current logical voice limit");
    Check(svms::RequestedRuntimeVoiceLimit() == kOldCapacity,
          "rejected live growth snaps the requested limit back");
    svms::ConfigureRuntimeVoiceGrowthCeiling(
        svms::kRuntimeVoiceGrowthCeiling);

    struct SavedVoice {
        uint8_t state;
        uint8_t channel;
        uint8_t note;
        uint8_t renderClass;
        uint32_t playIndex;
        float phase;
        float currentGain;
        float mixGainL;
        float mixGainR;
        uint32_t sampleStart;
        uint32_t activePosition;
    };
    std::vector<SavedVoice> saved(kOldCapacity);
    for (uint32_t handle = 0u; handle < kOldCapacity; ++handle) {
        saved[handle] = {
            voices.v.state[handle],
            voices.v.channel[handle],
            voices.v.note[handle],
            voices.v.renderClass[handle],
            voices.v.playIndex[handle],
            voices.v.phases[handle],
            voices.v.currentGain[handle],
            voices.v.mixGainL[handle],
            voices.v.mixGainR[handle],
            voices.v.sampleStart[handle],
            voices.activePosition_[handle]
        };
    }

    // Exercise the exact production request path: the render-boundary command
    // sees a logical limit above the physical pool and grows the pool without
    // resetting active voices.
    svms::RequestRuntimeVoiceLimit(kGrownCapacity);
    voices.ApplyRuntimeVoiceLimit(100u);
    Check(voices.GetMaxVoices() == kGrownCapacity,
          "1024 -> 2048 grows the physical pool live");
    Check(voices.GetVoiceLimit() == kGrownCapacity,
          "1024 -> 2048 applies the requested logical limit");
    Check(svms::AppliedRuntimeVoiceLimit() == kGrownCapacity,
          "growth publishes the applied logical limit");
    Check(svms::RuntimeAllocatedVoicePoolCapacity() == kGrownCapacity,
          "growth publishes the physical pool capacity");
    Check(voices.GetActiveCount() == kOldCapacity,
          "growth preserves the complete active voice set");

    bool identical = true;
    for (uint32_t handle = 0u; handle < kOldCapacity; ++handle) {
        const SavedVoice& before = saved[handle];
        identical = identical &&
            voices.v.state[handle] == before.state &&
            voices.v.channel[handle] == before.channel &&
            voices.v.note[handle] == before.note &&
            voices.v.renderClass[handle] == before.renderClass &&
            voices.v.playIndex[handle] == before.playIndex &&
            NearlyEqual(voices.v.phases[handle], before.phase) &&
            NearlyEqual(voices.v.currentGain[handle], before.currentGain) &&
            NearlyEqual(voices.v.mixGainL[handle], before.mixGainL) &&
            NearlyEqual(voices.v.mixGainR[handle], before.mixGainR) &&
            voices.v.sampleStart[handle] == before.sampleStart &&
            voices.activePosition_[handle] == before.activePosition &&
            voices.activeList_[voices.activePosition_[handle]] == handle;
        if (!identical) break;
    }
    Check(identical,
          "all pre-growth voices keep identical render/lifecycle state");

    // Deliberately leave renderer scratch at the original 1024 capacity.
    // Before the fix, RenderBlock returned immediately solely because the
    // physical VoiceManager capacity had become 2048, producing the exact
    // 'notes stop, reverb tail continues' regression.
    svms::RenderScalar renderer;
    Check(renderer.ReserveVoiceCapacity(kOldCapacity),
          "renderer starts with old-capacity scratch");
    Check(renderer.GetAllocatedBytes() ==
              svms::RenderScalar::EstimateAllocatedBytes(
                  kOldCapacity, 1u, kFrames),
          "serial-renderer memory estimate matches the reserved layout");
    std::vector<float> left(kFrames, 0.0f);
    std::vector<float> right(kFrames, 0.0f);
    const float phaseBeforeRender = voices.v.phases[0u];
    renderer.RenderBlock(voices, channels, samples.data(), kSampleFrames,
                         left.data(), right.data(), kFrames, cfg,
                         nullptr, 0u, true, 100u);

    bool nonzeroOutput = false;
    for (uint32_t frame = 0u; frame < kFrames; ++frame) {
        if (left[frame] != 0.0f || right[frame] != 0.0f) {
            nonzeroOutput = true;
            break;
        }
    }
    Check(nonzeroOutput,
          "old voices still render after physical pool growth");
    Check(!NearlyEqual(voices.v.phases[0u], phaseBeforeRender),
          "old voice phase advances after physical pool growth");

    // A new allocation must be able to use the grown handle range. The grown
    // free stack is rebuilt from voice state and includes handles 1024..2047.
    const svms::VoiceHandle grownHandle = voices.AllocateVoice(0u, 100u, 127u);
    Check(grownHandle != svms::kInvalidVoice && grownHandle >= kOldCapacity,
          "new voice allocation can use a handle above the old capacity");
    if (grownHandle != svms::kInvalidVoice) {
        voices.SetVoicePlayIndex(grownHandle, 5000u);
        voices.SetVoiceSample(grownHandle, 0u, kSampleFrames, 64u,
                              kSampleFrames - 64u, 1u, 0.5f, 1u);
        voices.SetVoiceEnvelope(grownHandle, 1.0f, 1.0f, 0u, 0u, 0u, 0u,
                                0.0f, 1.0f, 0.9995f);
        voices.SetVoiceGain(grownHandle, 0.001f, 0.001f);
        voices.RefreshMixGain(grownHandle, channels.GetParams()[0u]);
    }

    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    renderer.RenderBlock(voices, channels, samples.data(), kSampleFrames,
                         left.data(), right.data(), kFrames, cfg,
                         nullptr, 0u, true, 116u);
    Check(renderer.GetScratchCapacity() >= voices.GetActiveCount(),
          "renderer scratch follows active polyphony rather than physical capacity");

    // Lowering the logical cap must continue to be observed after a grow.
    // With 1025 active voices this also exercises the existing gradual
    // forced-release path instead of shrinking the physical allocation.
    svms::RequestRuntimeVoiceLimit(kLoweredLimit);
    voices.ApplyRuntimeVoiceLimit(132u);
    Check(voices.GetVoiceLimit() == kLoweredLimit,
          "2048 -> 512 applies after growth");
    Check(svms::AppliedRuntimeVoiceLimit() == kLoweredLimit,
          "lowered cap is published after growth");
    Check(voices.GetMaxVoices() == kGrownCapacity,
          "lowering the logical cap does not shrink the physical pool");
    Check(voices.GetReleasingCount() != 0u,
          "lowering below active polyphony starts forced release");

    // Raising back to an already allocated size is purely logical: no second
    // VoiceSoA replacement and no physical shrink/grow cycle is required.
    svms::RequestRuntimeVoiceLimit(kGrownCapacity);
    voices.ApplyRuntimeVoiceLimit(133u);
    Check(voices.GetVoiceLimit() == kGrownCapacity,
          "512 -> 2048 reapplies the logical cap");
    Check(svms::AppliedRuntimeVoiceLimit() == kGrownCapacity,
          "re-raised logical cap is published");
    Check(voices.GetMaxVoices() == kGrownCapacity,
          "512 -> 2048 reuses the existing 2048 physical pool");

    RunVoiceStealBatchEquivalenceChecks();
    RunVoiceStealBatchLaunchEquivalenceChecks();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d live growth regression test(s) failed\n",
                     g_failures);
        return 1;
    }
    std::puts("SVMS V3 live voice growth regression passed");
    return 0;
}
