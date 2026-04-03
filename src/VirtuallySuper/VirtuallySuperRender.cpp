#include "VirtuallySuperRender.h"
#include "VirtuallySuperRenderSIMD.h"

#include <algorithm>
#include <cstring>
#include <immintrin.h>

// XP compatibility: __cpuid may not be available in older headers
#ifdef SVMS_LEGACY_XP
#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(__cpuid)
#endif
#endif

namespace {

// Fast linear interpolation for sample playback
inline float InterpolateSample(const float *data, uint32_t baseIndex,
                               uint32_t nextIndex, float frac) {
  const float s0 = data[baseIndex];
  const float s1 = data[nextIndex];
  return s0 + (s1 - s0) * frac;
}

// Optimized sawtooth generator
inline float MakeSaw(float phase) {
  return phase * 2.0f - 1.0f;
}

// Fast phase advance with wrap
inline float AdvancePhase(float phase, float phaseStep) {
  phase += phaseStep;
  phase -= (phase >= 1.0f) ? 1.0f : 0.0f;
  return phase;
}

// Check if voice should loop
inline bool ShouldLoop(const virtuallysuper::ExactVoice &voice) {
  if (!voice.sampleBacked) return false;
  if (voice.loopMode == virtuallysuper::SoundFontLoopNone) return false;
  if (voice.state == virtuallysuper::ExactLifecycleState::Released &&
      voice.loopMode == virtuallysuper::SoundFontLoopSustain) {
    return false;
  }
  return voice.loopEnd > voice.loopStart + 1u;
}

// Hash function for density noise
inline uint32_t HashNoise(uint32_t seed, uint32_t index) {
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

RenderSystem::RenderSystem() : simdEnabled_(false) {
  // Enable SIMD only if supported
  // x64 always has SSE2, x86 needs runtime check
  // AVX2 is disabled for x86 and XP builds
#ifdef _M_AMD64
  simdEnabled_ = simd::HasSSE2();
  // AVX2 only for non-XP x64 builds
#ifndef SVMS_LEGACY_XP
  if (simdEnabled_ && simd::HasAVX2()) {
    // AVX2 available - still use SSE2 path for compatibility
    // Full AVX2 optimization would require separate code path
  }
#endif
#else
  // x86 build - check for SSE2 at runtime
  simdEnabled_ = simd::HasSSE2();
#endif
}

void RenderSystem::Reset() {
  stats_ = RenderStats();
  voiceBuffer_.Reset();
}

void RenderSystem::ResetBlockStats() {
  stats_ = RenderStats();
}

const RenderStats &RenderSystem::GetStats() const {
  return stats_;
}

void RenderSystem::RenderBlock(ExactSystem &exact, const GroupedSystem &grouped,
                               const DensitySystem &density, float *output,
                               int numFrames, int sampleRate) {
  (void)sampleRate;
  if (!output || numFrames <= 0) return;

  ResetBlockStats();

  // Clear output buffer once
  std::memset(output, 0, static_cast<size_t>(numFrames) * 2u * sizeof(float));

  // Process in tiles for better cache utilization
  uint32_t tileStart = 0;
  while (tileStart < static_cast<uint32_t>(numFrames)) {
    const uint32_t remaining = static_cast<uint32_t>(numFrames) - tileStart;
    const uint32_t frames = remaining > kDefaultRenderTileFrames
                                ? kDefaultRenderTileFrames
                                : remaining;
    float *tileOutput = output + tileStart * 2u;

    if (exact.GetActiveVoiceCount() != 0 || exact.GetReleasedVoiceCount() != 0) {
      if (simdEnabled_) {
        RenderExactTileSIMD(exact, tileOutput, frames);
      } else {
        RenderExactTile(exact, tileOutput, frames);
      }
    }
    
    if (grouped.GetActiveHandleCount() != 0) {
      RenderGroupedTile(grouped, tileOutput, frames);
    }
    
    if (density.GetActiveHandleCount() != 0) {
      RenderDensityTile(density, tileOutput, frames, tileStart);
    }

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
      bool retireVoice = false;

      // Pre-fetch frequently accessed data
      const bool isSampleBacked = voice->sampleBacked != 0 && voice->sampleData != nullptr;
      const bool hasAttack = voice->attackSamplesRemaining > 0;
      const bool isReleased = voice->state == ExactLifecycleState::Released;
      const float phaseStep = voice->phaseStep;
      const float leftGain = voice->leftGain;
      const float rightGain = voice->rightGain;
      const float releaseDecay = voice->releaseDecay;
      const float attackGainStep = voice->attackGainStep;
      const uint16_t attackRemaining = voice->attackSamplesRemaining;
      const float targetGain = voice->targetGain;

      // Sample playback pre-compute (only for sample-backed voices)
      uint32_t relativeSampleEnd = 0, relativeLoopStart = 0, relativeLoopEnd = 0;
      if (isSampleBacked) {
        relativeSampleEnd = voice->sampleEnd - voice->sampleStart;
        relativeLoopStart = voice->loopStart > voice->sampleStart
            ? voice->loopStart - voice->sampleStart : 0u;
        relativeLoopEnd = voice->loopEnd > voice->sampleStart
            ? voice->loopEnd - voice->sampleStart : 0u;
      }
      const bool shouldLoop = isSampleBacked && ShouldLoop(*voice);

      // Inner render loop - ADSR envelope applied per sample
      // Based on TSF's envelope implementation: linear attack/decay, exponential release
      for (uint32_t frame = 0; frame < frames; ++frame) {
        float sample = 0.0f;

        if (isSampleBacked && voice->sampleData != nullptr) {
          // Sample playback with interpolation
          if (phase < 0.0f) phase = 0.0f;

          uint32_t baseOffset = static_cast<uint32_t>(phase);
          if (baseOffset + 1u >= relativeSampleEnd) {
            if (!shouldLoop) {
              retireVoice = true;
              break;
            }
            phase = static_cast<float>(relativeLoopStart);
            baseOffset = relativeLoopStart;
          }

          uint32_t baseIndex = voice->sampleStart + baseOffset;
          uint32_t nextIndex = baseIndex + 1u;

          if (shouldLoop && nextIndex >= voice->loopEnd)
            nextIndex = voice->loopStart;
          if (nextIndex >= voice->sampleEnd)
            nextIndex = voice->sampleEnd - 1u;

          const float frac = phase - static_cast<float>(baseOffset);
          sample = InterpolateSample(voice->sampleData, baseIndex, nextIndex, frac);

          // Apply ADSR envelope (clean state machine, no fall-through)
          if (!isReleased) {
            if (voice->envelopeStage == 0) {
              // Delay stage - hold at zero
              if (voice->holdSamplesRemaining > 0) {
                --voice->holdSamplesRemaining;
                gain = 0.0f;
              } else {
                voice->envelopeStage = 1;  // Move to attack
              }
            }
            
            if (voice->envelopeStage == 1) {
              // Attack stage - linear ramp to targetGain
              if (voice->attackSamplesRemaining > 0) {
                gain += voice->attackGainStep;
                --voice->attackSamplesRemaining;
                if (gain > voice->targetGain) gain = voice->targetGain;
              } else {
                gain = voice->targetGain;  // Ensure we're at target
              }
              
              if (voice->attackSamplesRemaining == 0) {
                if (voice->decaySamplesRemaining > 0) {
                  voice->envelopeStage = 2;  // Move to decay
                } else {
                  voice->envelopeStage = 3;  // Move to sustain
                }
              }
            }
            
            if (voice->envelopeStage == 2) {
              // Decay stage - linear ramp to sustainLevel
              if (voice->decaySamplesRemaining > 0) {
                gain -= voice->decayGainStep;
                --voice->decaySamplesRemaining;
                if (gain < voice->sustainLevel) gain = voice->sustainLevel;
              } else {
                gain = voice->sustainLevel;  // Ensure we're at sustain
              }
              
              if (voice->decaySamplesRemaining == 0) {
                voice->envelopeStage = 3;  // Move to sustain
              }
            }
            // Stage 3 (Sustain) - gain stays at sustainLevel until note off
          } else {
            // Release stage - exponential decay
            gain *= voice->releaseDecay;
          }

          sample *= gain;
          phase += phaseStep;

          // Handle loop wrap
          if (shouldLoop && phase >= static_cast<float>(relativeLoopEnd)) {
            phase = static_cast<float>(relativeLoopStart) +
                    (phase - static_cast<float>(relativeLoopEnd));
          }
        } else {
          // Synthetic oscillator with ADSR
          sample = MakeSaw(phase) * gain;
          phase = AdvancePhase(phase, phaseStep);
          
          if (!isReleased) {
            if (voice->envelopeStage == 0) {
              if (voice->holdSamplesRemaining > 0) {
                --voice->holdSamplesRemaining;
                gain = 0.0f;
              } else {
                voice->envelopeStage = 1;
              }
            }
            
            if (voice->envelopeStage == 1) {
              if (voice->attackSamplesRemaining > 0) {
                gain += voice->attackGainStep;
                --voice->attackSamplesRemaining;
                if (gain > voice->targetGain) gain = voice->targetGain;
              } else {
                gain = voice->targetGain;
              }
              
              if (voice->attackSamplesRemaining == 0) {
                voice->envelopeStage = (voice->decaySamplesRemaining > 0) ? 2 : 3;
              }
            }
            
            if (voice->envelopeStage == 2) {
              if (voice->decaySamplesRemaining > 0) {
                gain -= voice->decayGainStep;
                --voice->decaySamplesRemaining;
                if (gain < voice->sustainLevel) gain = voice->sustainLevel;
              } else {
                gain = voice->sustainLevel;
              }
              
              if (voice->decaySamplesRemaining == 0) {
                voice->envelopeStage = 3;
              }
            }
          } else {
            gain *= voice->releaseDecay;
          }
        }

        // Apply stereo panning and mix to output
        tileOutput[frame * 2u] += sample * leftGain;
        tileOutput[frame * 2u + 1u] += sample * rightGain;
      }

      voice->phase = phase;
      voice->currentGain = gain;

      // Check for voice retirement
      if (retireVoice || (isReleased && gain < 0.00015f)) {
        exact.RetireVoice(handle);
      }

      handle = nextHandle;
    }
  }
}

void RenderSystem::RenderExactTileSIMD(ExactSystem &exact, float *tileOutput,
                                       uint32_t frames) {
  if (frames == 0) return;
  
  VoiceSoABuffer &voices = exact.GetVoiceSoA();
  const uint32_t voiceCapacity = voices.capacity;
  
  // Process voices in batches of 8 (AVX2) or 4 (SSE2)
  constexpr uint32_t kBatchSize = 8;
  
  // Temporary buffers for batch processing
  alignas(32) float batchPhases[kBatchSize];
  alignas(32) float batchPhaseSteps[kBatchSize];
  alignas(32) float batchGains[kBatchSize];
  alignas(32) float batchLeftGains[kBatchSize];
  alignas(32) float batchRightGains[kBatchSize];
  alignas(32) float batchReleaseDecays[kBatchSize];
  alignas(32) uint8_t batchStates[kBatchSize];
  alignas(32) uint8_t batchSampleBacked[kBatchSize];
  
  // Collect active voices for batch processing
  std::vector<uint32_t> activeHandles;
  activeHandles.reserve(voiceCapacity);
  
  static const ExactQueueClass kOrder[] = {
      ExactQueueClass::QuietActive, ExactQueueClass::LoudActive,
      ExactQueueClass::QuietRelease, ExactQueueClass::LoudRelease};

  for (size_t classIndex = 0; classIndex < sizeof(kOrder) / sizeof(kOrder[0]);
       ++classIndex) {
    uint32_t handle = exact.GetQueueHead(kOrder[classIndex]);
    while (handle != kInvalidVoiceHandle) {
      const ExactVoice *voice = exact.GetVoice(handle);
      if (voice && voice->state != ExactLifecycleState::Free) {
        activeHandles.push_back(handle);
      }
      handle = voice ? voice->nextQueue : kInvalidVoiceHandle;
    }
  }
  
  const uint32_t totalVoices = (uint32_t)activeHandles.size();
  if (totalVoices == 0) return;
  
  // Process in batches
  for (uint32_t batchStart = 0; batchStart < totalVoices; batchStart += kBatchSize) {
    const uint32_t batchSize = std::min(kBatchSize, totalVoices - batchStart);
    
    // Load voice data into temporary arrays
    for (uint32_t i = 0; i < batchSize; ++i) {
      const uint32_t handle = activeHandles[batchStart + i];
      batchPhases[i] = voices.phases[handle];
      batchPhaseSteps[i] = voices.phaseSteps[handle];
      batchGains[i] = voices.currentGains[handle];
      batchLeftGains[i] = voices.leftGains[handle];
      batchRightGains[i] = voices.rightGains[handle];
      batchReleaseDecays[i] = voices.releaseDecays[handle];
      batchStates[i] = voices.states[handle];
      batchSampleBacked[i] = voices.sampleBacked[handle];
    }
    
    // Process this batch with SIMD
    for (uint32_t frame = 0; frame < frames; ++frame) {
      float frameSamplesL[kBatchSize];
      float frameSamplesR[kBatchSize];
      
      // Generate samples for all voices in batch
      for (uint32_t i = 0; i < batchSize; ++i) {
        float sample = 0.0f;
        const bool isReleased = batchStates[i] == (uint8_t)ExactLifecycleState::Released;
        const bool isSampleBacked = batchSampleBacked[i] != 0;
        
        if (isSampleBacked) {
          // Sample-backed voice - scalar path (complex interpolation)
          const uint32_t handle = activeHandles[batchStart + i];
          const ExactVoice *voice = exact.GetVoice(handle);
          if (voice && voice->sampleData != nullptr) {
            float phase = batchPhases[i];
            if (phase < 0.0f) phase = 0.0f;
            
            const uint32_t relativeSampleEnd = voice->sampleEnd - voice->sampleStart;
            const uint32_t relativeLoopStart = voice->loopStart > voice->sampleStart
                ? voice->loopStart - voice->sampleStart : 0u;
            const uint32_t relativeLoopEnd = voice->loopEnd > voice->sampleStart
                ? voice->loopEnd - voice->sampleStart : 0u;
            const bool shouldLoop = ShouldLoop(*voice);
            
            uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset + 1u >= relativeSampleEnd) {
              if (!shouldLoop) {
                batchGains[i] = 0.0f;
                batchPhases[i] = 0.0f;
                frameSamplesL[i] = 0.0f;
                frameSamplesR[i] = 0.0f;
                continue;
              }
              phase = static_cast<float>(relativeLoopStart);
              baseOffset = relativeLoopStart;
            }
            
            uint32_t baseIndex = voice->sampleStart + baseOffset;
            uint32_t nextIndex = baseIndex + 1u;
            if (shouldLoop && nextIndex >= voice->loopEnd)
              nextIndex = voice->loopStart;
            if (nextIndex >= voice->sampleEnd)
              nextIndex = voice->sampleEnd - 1u;
            
            const float frac = phase - static_cast<float>(baseOffset);
            sample = InterpolateSample(voice->sampleData, baseIndex, nextIndex, frac);
            
            // Apply envelope
            if (!isReleased) {
              if (voice->envelopeStage == 1 && voice->attackSamplesRemaining > 0) {
                batchGains[i] += voice->attackGainStep;
                if (batchGains[i] > voice->targetGain) batchGains[i] = voice->targetGain;
              } else if (voice->envelopeStage == 2 && voice->decaySamplesRemaining > 0) {
                batchGains[i] -= voice->decayGainStep;
                if (batchGains[i] < voice->sustainLevel) batchGains[i] = voice->sustainLevel;
              }
            } else {
              batchGains[i] *= voice->releaseDecay;
            }
            
            sample *= batchGains[i];
            phase += batchPhaseSteps[i];
            
            if (shouldLoop && phase >= static_cast<float>(relativeLoopEnd)) {
              phase = static_cast<float>(relativeLoopStart) + (phase - static_cast<float>(relativeLoopEnd));
            }
            batchPhases[i] = phase;
          }
        } else {
          // Synthetic oscillator - can use SIMD optimization
          sample = MakeSaw(batchPhases[i]) * batchGains[i];
          batchPhases[i] = AdvancePhase(batchPhases[i], batchPhaseSteps[i]);
          
          // Apply release decay
          if (isReleased) {
            batchGains[i] *= batchReleaseDecays[i];
          }
        }
        
        frameSamplesL[i] = sample * batchLeftGains[i];
        frameSamplesR[i] = sample * batchRightGains[i];
      }
      
      // Mix batch results into output using SIMD
      #ifdef _M_AMD64
      // x64 - use SSE2 for mixing
      for (uint32_t i = 0; i < batchSize; i += 4) {
        const uint32_t count = std::min(4u, batchSize - i);
        if (count == 4) {
          __m128 samplesL = _mm_loadu_ps(&frameSamplesL[i]);
          __m128 samplesR = _mm_loadu_ps(&frameSamplesR[i]);
          __m128 outL = _mm_loadu_ps(&tileOutput[frame * 2 + 0]);
          __m128 outR = _mm_loadu_ps(&tileOutput[frame * 2 + 1]);
          outL = _mm_add_ps(outL, samplesL);
          outR = _mm_add_ps(outR, samplesR);
          _mm_storeu_ps(&tileOutput[frame * 2 + 0], outL);
          _mm_storeu_ps(&tileOutput[frame * 2 + 1], outR);
        } else {
          for (uint32_t j = 0; j < count; ++j) {
            tileOutput[frame * 2 + 0] += frameSamplesL[i + j];
            tileOutput[frame * 2 + 1] += frameSamplesR[i + j];
          }
        }
      }
      #else
      // x86 - scalar mixing
      for (uint32_t i = 0; i < batchSize; ++i) {
        tileOutput[frame * 2 + 0] += frameSamplesL[i];
        tileOutput[frame * 2 + 1] += frameSamplesR[i];
      }
      #endif
    }
    
    // Write back updated state
    for (uint32_t i = 0; i < batchSize; ++i) {
      const uint32_t handle = activeHandles[batchStart + i];
      voices.phases[handle] = batchPhases[i];
      voices.currentGains[handle] = batchGains[i];
      
      // Check for retirement
      if (batchStates[i] == (uint8_t)ExactLifecycleState::Released && 
          batchGains[i] < 0.00015f) {
        exact.RetireVoice(handle);
      }
    }
  }
  
  stats_.exactVoicesVisited += totalVoices;
}

void RenderSystem::RenderGroupedTile(const GroupedSystem &grouped,
                                     float *tileOutput, uint32_t frames) {
  const uint32_t activeCount = grouped.GetActiveHandleCount();

  for (uint32_t i = 0; i < activeCount; ++i) {
    const GroupedObject *group = grouped.GetGroup(grouped.GetActiveHandle(i));
    if (!group || group->active == 0 || group->representedNoteCount == 0) continue;

    ++stats_.groupedObjectsVisited;

    float phase = group->phase;
    const float gain = group->gain;
    const float leftGain = group->leftGain;
    const float rightGain = group->rightGain;
    const float phaseStep = group->phaseStep;

    for (uint32_t frame = 0; frame < frames; ++frame) {
      const float sample = MakeSaw(phase) * gain;
      tileOutput[frame * 2u] += sample * leftGain;
      tileOutput[frame * 2u + 1u] += sample * rightGain;
      phase = AdvancePhase(phase, phaseStep);
    }

    const_cast<GroupedObject *>(group)->phase = phase;
  }
}

void RenderSystem::RenderDensityTile(const DensitySystem &density,
                                     float *tileOutput, uint32_t frames,
                                     uint32_t tileStart) {
  const uint32_t activeCount = density.GetActiveHandleCount();

  for (uint32_t i = 0; i < activeCount; ++i) {
    const DensityObject *object = density.GetDensityObject(density.GetActiveHandle(i));
    if (!object || object->active == 0 || object->representedNoteCount == 0 ||
        object->representedNoteCount < object->activationThreshold) {
      continue;
    }

    ++stats_.densityObjectsVisited;

    const float gain = object->saturatedGain * 0.012f;
    const float leftGain = object->leftGain;
    const float rightGain = object->rightGain;
    const uint32_t seed = object->grainJitterSeed;

    for (uint32_t frame = 0; frame < frames; ++frame) {
      const uint32_t hash = HashNoise(seed, tileStart + frame);
      const float sample = (((hash >> 8) & 0xFFFFu) / 32767.5f - 1.0f) * gain;
      tileOutput[frame * 2u] += sample * leftGain;
      tileOutput[frame * 2u + 1u] += sample * rightGain;
    }
  }
}

} // namespace virtuallysuper
