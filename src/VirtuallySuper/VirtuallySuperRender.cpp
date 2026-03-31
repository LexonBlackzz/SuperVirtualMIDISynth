#include "VirtuallySuperRender.h"

#include <math.h>

namespace {

static float WrapPhase(float phase) {
  while (phase >= 1.0f)
    phase -= 1.0f;
  while (phase < 0.0f)
    phase += 1.0f;
  return phase;
}

static float MakeSaw(float phase) { return phase * 2.0f - 1.0f; }

static float NoteToFrequencyHz(float note) {
  return 440.0f * powf(2.0f, (note - 69.0f) * (1.0f / 12.0f));
}

static float MakePan(uint32_t channel, uint32_t note) {
  const uint32_t index = (channel * 17u + note * 13u) & 15u;
  return ((float)index / 7.5f) - 1.0f;
}

static void MakeStereoGains(float pan, float *left, float *right) {
  const float clamped = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
  *left = 0.5f * (1.0f - clamped * 0.35f);
  *right = 0.5f * (1.0f + clamped * 0.35f);
}

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

RenderSystem::RenderSystem() : tileBuffer_() {}

void RenderSystem::Reset() {}

void RenderSystem::RenderBlock(ExactSystem &exact, const GroupedSystem &grouped,
                               const DensitySystem &density, float *output,
                               int numFrames, int sampleRate) {
  if (!output || numFrames <= 0 || sampleRate <= 0)
    return;

  uint32_t tileStart = 0;
  while (tileStart < (uint32_t)numFrames) {
    const uint32_t remaining = (uint32_t)numFrames - tileStart;
    const uint32_t frames =
        remaining > kDefaultRenderTileFrames ? kDefaultRenderTileFrames
                                             : remaining;

    ClearTileBuffer(frames);
    RenderExactTile(exact, tileStart, frames, sampleRate);
    RenderGroupedTile(grouped, tileStart, frames, sampleRate);
    RenderDensityTile(density, tileStart, frames);
    CopyTileToOutput(output, tileStart, frames);
    tileStart += frames;
  }
}

void RenderSystem::RenderExactTile(ExactSystem &exact, uint32_t tileStart,
                                   uint32_t frames, int sampleRate) {
  (void)tileStart;
  static const ExactQueueClass kOrder[] = {
      ExactQueueClass::QuietActive, ExactQueueClass::LoudActive,
      ExactQueueClass::QuietRelease, ExactQueueClass::LoudRelease};

  for (size_t classIndex = 0; classIndex < sizeof(kOrder) / sizeof(kOrder[0]);
       ++classIndex) {
    uint32_t handle = exact.GetQueueHead(kOrder[classIndex]);
    while (handle != kInvalidVoiceHandle) {
      ExactVoice *voice = exact.GetMutableVoice(handle);
      if (!voice)
        break;

      const uint32_t nextHandle = voice->nextQueue;
      if (voice->state == ExactLifecycleState::Free) {
        handle = nextHandle;
        continue;
      }

      const float phaseStep = voice->frequencyHz / (float)sampleRate;
      float phase = voice->phase;
      float gain = voice->currentGain;
      float leftGain = 0.0f;
      float rightGain = 0.0f;
      MakeStereoGains(MakePan(voice->channel, voice->note), &leftGain,
                      &rightGain);

      for (uint32_t frame = 0; frame < frames; ++frame) {
        const float sample = MakeSaw(phase) * gain;
        tileBuffer_[frame * 2u] += sample * leftGain;
        tileBuffer_[frame * 2u + 1u] += sample * rightGain;
        phase = WrapPhase(phase + phaseStep);
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
                                     uint32_t tileStart, uint32_t frames,
                                     int sampleRate) {
  const GroupedConfig &config = grouped.GetConfig();
  const uint32_t capacity = grouped.GetGroupCapacity();

  for (uint32_t i = 0; i < capacity; ++i) {
    const GroupedObject *group = grouped.GetGroup(i);
    if (!group || group->active == 0 || group->representedNoteCount == 0)
      continue;

    const float centerNote =
        (float)(group->pitchBandId * config.pitchBandSemitones) +
        (float)config.pitchBandSemitones * 0.5f;
    const float phaseOffset =
        (float)((group->groupId * 1103515245u + 12345u) & 1023u) / 1024.0f;
    const float phaseStep = NoteToFrequencyHz(centerNote) / (float)sampleRate;
    const float gain =
        ((float)group->representedNoteCount * 0.0012f) > 0.02f
            ? 0.02f
            : (float)group->representedNoteCount * 0.0012f;
    float leftGain = 0.0f;
    float rightGain = 0.0f;
    MakeStereoGains(MakePan(group->channel, (uint32_t)centerNote), &leftGain,
                    &rightGain);

    float phase = WrapPhase(phaseOffset + (float)tileStart * phaseStep);
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const float sample = MakeSaw(phase) * gain;
      tileBuffer_[frame * 2u] += sample * leftGain;
      tileBuffer_[frame * 2u + 1u] += sample * rightGain;
      phase = WrapPhase(phase + phaseStep);
    }
  }
}

void RenderSystem::RenderDensityTile(const DensitySystem &density,
                                     uint32_t tileStart, uint32_t frames) {
  const uint32_t capacity = density.GetObjectCapacity();

  for (uint32_t i = 0; i < capacity; ++i) {
    const DensityObject *object = density.GetDensityObject(i);
    if (!object || object->active == 0 || object->representedNoteCount == 0 ||
        object->representedNoteCount < object->activationThreshold) {
      continue;
    }

    const float gain = object->saturatedGain * 0.012f;
    float leftGain = 0.0f;
    float rightGain = 0.0f;
    MakeStereoGains(MakePan(object->channel, object->pitchBandId * 12u),
                    &leftGain, &rightGain);

    for (uint32_t frame = 0; frame < frames; ++frame) {
      const uint32_t hash = HashNoise(object->grainJitterSeed, tileStart + frame);
      const float sample = (((hash >> 8) & 0xFFFFu) / 32767.5f - 1.0f) * gain;
      tileBuffer_[frame * 2u] += sample * leftGain;
      tileBuffer_[frame * 2u + 1u] += sample * rightGain;
    }
  }
}

void RenderSystem::ClearTileBuffer(uint32_t frames) {
  for (uint32_t i = 0; i < frames * 2u; ++i)
    tileBuffer_[i] = 0.0f;
}

void RenderSystem::CopyTileToOutput(float *output, uint32_t tileStart,
                                    uint32_t frames) {
  float *dest = output + tileStart * 2u;
  for (uint32_t i = 0; i < frames * 2u; ++i)
    dest[i] += tileBuffer_[i];
}

} // namespace virtuallysuper
