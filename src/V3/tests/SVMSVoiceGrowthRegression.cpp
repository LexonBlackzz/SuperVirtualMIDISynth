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

    if (g_failures != 0) {
        std::fprintf(stderr, "%d live growth regression test(s) failed\n",
                     g_failures);
        return 1;
    }
    std::puts("SVMS V3 live voice growth regression passed");
    return 0;
}
