#include "VirtuallySuperRender.h"

#include <string.h>

namespace {

static float AdvancePhase(float phase, float phaseStep) {
  phase += phaseStep;
  if (phase >= 1.0f)
    phase -= 1.0f;
  return phase;
}

static float MakeSaw(float phase) { return phase * 2.0f - 1.0f; }

static uint32_t HashNoise(uint32_t seed, uint32_t index) {
  uint32_t x = seed ^ (index * 747796405u + 2891336453u);
  x ^= x >> 16;
  x *= 2246822519u;
  x ^= x >> 13;
  x *= 3266489917u;
  x ^= x >> 16;
  return x;
}

} // namespace

namespace virtuallysuper {

RenderSystem::RenderSystem() : stats_() {}

void RenderSystem::Reset() { stats_ = RenderStats(); }

void RenderSystem::ResetBlockStats() { stats_ = RenderStats(); }

const RenderStats &RenderSystem::GetStats() const { return stats_; }

void RenderSystem::RenderBlock(ExactSystem &exact, const GroupedSystem &grouped,
                               const DensitySystem &density, float *output,
                               int numFrames, int sampleRate) {
  (void)sampleRate;
  if (!output || numFrames <= 0)
    return;

  ResetBlockStats();

  uint32_t tileStart = 0;
  while (tileStart < (uint32_t)numFrames) {
    const uint32_t remaining = (uint32_t)numFrames - tileStart;
    const uint32_t frames =
        remaining > kDefaultRenderTileFrames ? kDefaultRenderTileFrames
                                             : remaining;
    float *tileOutput = output + tileStart * 2u;

    if (exact.GetActiveVoiceCount() != 0 || exact.GetReleasedVoiceCount() != 0)
      RenderExactTile(exact, tileOutput, frames);
    if (grouped.GetActiveHandleCount() != 0)
      RenderGroupedTile(grouped, tileOutput, frames);
    if (density.GetActiveHandleCount() != 0)
      RenderDensityTile(density, tileOutput, frames, tileStart);

    tileStart += frames;
  }
}

void RenderSystem::RenderExactTile(ExactSystem &exact, float *tileOutput,
                                   uint32_t frames) {
  static const ExactQueueClass kOrder[] = {
      ExactQueueClass::QuietActive, ExactQueueClass::LoudActive,
      ExactQueueClass::QuietRelease, ExactQueueClass::LoudRelease};

  for (size_t classIndex = 0; classIndex < sizeof(kOrder) / sizeof(kOrder[0]);
       ++classIndex) {
    uint32_t handle = exact.GetQueueHead(kOrder[classIndex]);
    if (handle == kInvalidVoiceHandle)
      continue;

    while (handle != kInvalidVoiceHandle) {
      ExactVoice *voice = exact.GetMutableVoice(handle);
      if (!voice)
        break;

      const uint32_t nextHandle = voice->nextQueue;
      if (voice->state == ExactLifecycleState::Free) {
        handle = nextHandle;
        continue;
      }

      ++stats_.exactVoicesVisited;
      float phase = voice->phase;
      float gain = voice->currentGain;

      for (uint32_t frame = 0; frame < frames; ++frame) {
        const float sample = MakeSaw(phase) * gain;
        tileOutput[frame * 2u] += sample * voice->leftGain;
        tileOutput[frame * 2u + 1u] += sample * voice->rightGain;
        phase = AdvancePhase(phase, voice->phaseStep);
        if (voice->state == ExactLifecycleState::Released)
          gain *= voice->releaseDecay;
      }

      voice->phase = phase;
      voice->currentGain = gain;

      if (voice->state == ExactLifecycleState::Released && gain < 0.00015f)
        exact.RetireVoice(handle);

      handle = nextHandle;
    }
  }
}

void RenderSystem::RenderGroupedTile(const GroupedSystem &grouped,
                                     float *tileOutput, uint32_t frames) {
  const uint32_t activeCount = grouped.GetActiveHandleCount();
  for (uint32_t i = 0; i < activeCount; ++i) {
    const GroupedObject *group = grouped.GetGroup(grouped.GetActiveHandle(i));
    if (!group || group->active == 0 || group->representedNoteCount == 0)
      continue;

    ++stats_.groupedObjectsVisited;
    float phase = group->phase;
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const float sample = MakeSaw(phase) * group->gain;
      tileOutput[frame * 2u] += sample * group->leftGain;
      tileOutput[frame * 2u + 1u] += sample * group->rightGain;
      phase = AdvancePhase(phase, group->phaseStep);
    }

    const_cast<GroupedObject *>(group)->phase = phase;
  }
}

void RenderSystem::RenderDensityTile(const DensitySystem &density,
                                     float *tileOutput, uint32_t frames,
                                     uint32_t tileStart) {
  const uint32_t activeCount = density.GetActiveHandleCount();
  for (uint32_t i = 0; i < activeCount; ++i) {
    const DensityObject *object =
        density.GetDensityObject(density.GetActiveHandle(i));
    if (!object || object->active == 0 || object->representedNoteCount == 0 ||
        object->representedNoteCount < object->activationThreshold) {
      continue;
    }

    ++stats_.densityObjectsVisited;
    const float gain = object->saturatedGain * 0.012f;
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const uint32_t hash = HashNoise(object->grainJitterSeed, tileStart + frame);
      const float sample = (((hash >> 8) & 0xFFFFu) / 32767.5f - 1.0f) * gain;
      tileOutput[frame * 2u] += sample * object->leftGain;
      tileOutput[frame * 2u + 1u] += sample * object->rightGain;
    }
  }
}

} // namespace virtuallysuper
