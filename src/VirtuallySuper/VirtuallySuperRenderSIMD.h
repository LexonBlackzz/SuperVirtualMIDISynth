#ifndef VIRTUALLYSUPER_RENDER_SIMD_H
#define VIRTUALLYSUPER_RENDER_SIMD_H

#include "VirtuallySuperTypes.h"
#include <vector>

// Compatibility with XP and x86 builds
// SSE2 is guaranteed on x64, but optional on x86
// AVX2 is NOT available on XP-era CPUs generally
#ifndef SVMS_LEGACY_XP
#include <emmintrin.h>  // SSE2
#if defined(_MSC_VER) && defined(_M_AMD64)
#include <intrin.h>
#endif
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__) && defined(_M_AMD64))
#include <immintrin.h>  // AVX2
#define SVMS_HAS_AVX2_INTRINSICS 1
#else
#define SVMS_HAS_AVX2_INTRINSICS 0
#endif
#else
// XP/x86 legacy build - SSE2 may or may not be available
// We'll use runtime detection and fallback to scalar
#ifdef _MSC_VER
#include <emmintrin.h>
#if defined(_M_AMD64) || defined(_M_IX86)
#define SVMS_CAN_USE_SSE2 1
#endif
#endif
#define SVMS_HAS_AVX2_INTRINSICS 0
#endif

namespace virtuallysuper {

// Structure of Arrays layout for high-performance voice rendering
// This allows SIMD processing and better cache utilization
struct VoiceSoABuffer {
  // Core voice state - always accessed
  std::vector<float> phases;
  std::vector<float> phaseSteps;
  std::vector<float> currentGains;
  std::vector<float> targetGains;
  std::vector<float> leftGains;
  std::vector<float> rightGains;
  std::vector<float> releaseDecays;
  
  // Attack envelope - only used during attack phase
  std::vector<float> attackGainSteps;
  std::vector<uint16_t> attackSamplesRemaining;
  
  // Sample playback - only for sample-backed voices
  std::vector<const float*> sampleData;
  std::vector<uint32_t> sampleStarts;
  std::vector<uint32_t> sampleEnds;
  std::vector<uint32_t> loopStarts;
  std::vector<uint32_t> loopEnds;
  std::vector<uint8_t> loopModes;
  
  // Voice lifecycle
  std::vector<uint8_t> states;        // ExactLifecycleState
  std::vector<uint8_t> queueClasses;  // ExactQueueClass
  std::vector<uint8_t> sampleBacked;
  std::vector<uint8_t> heldBySustain;
  
  // Linked list pointers for voice management
  std::vector<uint32_t> nextSameKeys;
  std::vector<uint32_t> prevSameKeys;
  std::vector<uint32_t> nextQueues;
  std::vector<uint32_t> prevQueues;
  
  uint32_t capacity;
  uint32_t activeCount;
  
  VoiceSoABuffer() : capacity(0), activeCount(0) {}
  
  void Reserve(uint32_t maxVoices) {
    if (capacity >= maxVoices) return;
    
    capacity = maxVoices;
    phases.resize(maxVoices);
    phaseSteps.resize(maxVoices);
    currentGains.resize(maxVoices);
    targetGains.resize(maxVoices);
    leftGains.resize(maxVoices);
    rightGains.resize(maxVoices);
    releaseDecays.resize(maxVoices);
    attackGainSteps.resize(maxVoices);
    attackSamplesRemaining.resize(maxVoices);
    sampleData.resize(maxVoices);
    sampleStarts.resize(maxVoices);
    sampleEnds.resize(maxVoices);
    loopStarts.resize(maxVoices);
    loopEnds.resize(maxVoices);
    loopModes.resize(maxVoices);
    states.resize(maxVoices);
    queueClasses.resize(maxVoices);
    sampleBacked.resize(maxVoices);
    heldBySustain.resize(maxVoices);
    nextSameKeys.resize(maxVoices);
    prevSameKeys.resize(maxVoices);
    nextQueues.resize(maxVoices);
    prevQueues.resize(maxVoices);
  }
  
  void Reset() {
    activeCount = 0;
  }
};

// SIMD-optimized render functions
namespace simd {

// SSE2 stereo mix - process 4 samples at once
inline void MixStereo4(float* __restrict output, 
                       const float* __restrict samples,
                       float leftGain, float rightGain,
                       uint32_t frames) {
#ifdef SVMS_LEGACY_XP
#ifdef _MSC_VER
  const __m128 lg = _mm_set1_ps(leftGain);
  const __m128 rg = _mm_set1_ps(rightGain);
  
  for (uint32_t frame = 0; frame < frames; frame += 4) {
    __m128 s = _mm_loadu_ps(&samples[frame]);
    __m128 outL = _mm_loadu_ps(&output[frame * 2]);
    __m128 outR = _mm_loadu_ps(&output[frame * 2 + 1]);
    
    // Deinterleave: L R L R -> L L L L and R R R R
    __m128 even = _mm_shuffle_ps(s, s, _MM_SHUFFLE(2, 0, 2, 0));
    __m128 odd = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 1, 3, 1));
    
    even = _mm_mul_ps(even, lg);
    odd = _mm_mul_ps(odd, rg);
    
    outL = _mm_add_ps(outL, even);
    outR = _mm_add_ps(outR, odd);
    
    _mm_storeu_ps(&output[frame * 2], outL);
    _mm_storeu_ps(&output[frame * 2 + 1], outR);
  }
#else
  // Non-MSVC x86 fallback - scalar
  for (uint32_t frame = 0; frame < frames; ++frame) {
    output[frame * 2] += samples[frame] * leftGain;
    output[frame * 2 + 1] += samples[frame] * rightGain;
  }
#endif
#else
  const __m128 lg = _mm_set1_ps(leftGain);
  const __m128 rg = _mm_set1_ps(rightGain);
  
  for (uint32_t frame = 0; frame < frames; frame += 4) {
    __m128 s = _mm_loadu_ps(&samples[frame]);
    __m128 outL = _mm_loadu_ps(&output[frame * 2]);
    __m128 outR = _mm_loadu_ps(&output[frame * 2 + 1]);
    
    // Deinterleave: L R L R -> L L L L and R R R R
    __m128 even = _mm_shuffle_ps(s, s, _MM_SHUFFLE(2, 0, 2, 0));
    __m128 odd = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 1, 3, 1));
    
    even = _mm_mul_ps(even, lg);
    odd = _mm_mul_ps(odd, rg);
    
    outL = _mm_add_ps(outL, even);
    outR = _mm_add_ps(outR, odd);
    
    _mm_storeu_ps(&output[frame * 2], outL);
    _mm_storeu_ps(&output[frame * 2 + 1], outR);
  }
#endif
}

// SSE2 mono mix with gain - process 4 samples at once
inline void MixMono4(float* __restrict output,
                     const float* __restrict samples,
                     float gain,
                     uint32_t frames) {
#ifdef SVMS_LEGACY_XP
#ifdef _MSC_VER
  const __m128 g = _mm_set1_ps(gain);
  
  for (uint32_t i = 0; i < frames; i += 4) {
    __m128 s = _mm_loadu_ps(&samples[i]);
    __m128 out = _mm_loadu_ps(&output[i]);
    s = _mm_mul_ps(s, g);
    out = _mm_add_ps(out, s);
    _mm_storeu_ps(&output[i], out);
  }
#else
  for (uint32_t i = 0; i < frames; ++i) {
    output[i] += samples[i] * gain;
  }
#endif
#else
  const __m128 g = _mm_set1_ps(gain);
  
  for (uint32_t i = 0; i < frames; i += 4) {
    __m128 s = _mm_loadu_ps(&samples[i]);
    __m128 out = _mm_loadu_ps(&output[i]);
    s = _mm_mul_ps(s, g);
    out = _mm_add_ps(out, s);
    _mm_storeu_ps(&output[i], out);
  }
#endif
}

// SSE2 apply release decay - process 4 gains at once
inline void ApplyReleaseDecay4(float* __restrict gains,
                               const float* __restrict decays,
                               uint32_t count) {
#ifdef SVMS_LEGACY_XP
#ifdef _MSC_VER
  for (uint32_t i = 0; i < count; i += 4) {
    __m128 g = _mm_loadu_ps(&gains[i]);
    __m128 d = _mm_loadu_ps(&decays[i]);
    g = _mm_mul_ps(g, d);
    _mm_storeu_ps(&gains[i], g);
  }
#else
  for (uint32_t i = 0; i < count; ++i) {
    gains[i] *= decays[i];
  }
#endif
#else
  for (uint32_t i = 0; i < count; i += 4) {
    __m128 g = _mm_loadu_ps(&gains[i]);
    __m128 d = _mm_loadu_ps(&decays[i]);
    g = _mm_mul_ps(g, d);
    _mm_storeu_ps(&gains[i], g);
  }
#endif
}

// SSE2 sawtooth oscillator - process 4 phases at once
inline void GenerateSaw4(float* __restrict output,
                         float* __restrict phases,
                         const float* __restrict phaseSteps,
                         uint32_t frames) {
#ifdef SVMS_LEGACY_XP
#ifdef _MSC_VER
  const __m128 two = _mm_set1_ps(2.0f);
  const __m128 one = _mm_set1_ps(1.0f);
  
  for (uint32_t i = 0; i < frames; i += 4) {
    __m128 p = _mm_loadu_ps(&phases[i]);
    __m128 step = _mm_loadu_ps(&phaseSteps[i]);
    
    // Generate saw: phase * 2 - 1
    __m128 saw = _mm_sub_ps(_mm_mul_ps(p, two), one);
    _mm_storeu_ps(&output[i], saw);
    
    // Advance phase with wrap
    p = _mm_add_ps(p, step);
    __m128 mask = _mm_cmpge_ps(p, one);
    p = _mm_sub_ps(p, _mm_and_ps(mask, one));
    _mm_storeu_ps(&phases[i], p);
  }
#else
  for (uint32_t i = 0; i < frames; ++i) {
    output[i] = phases[i] * 2.0f - 1.0f;
    phases[i] += phaseSteps[i];
    if (phases[i] >= 1.0f) phases[i] -= 1.0f;
  }
#endif
#else
  const __m128 two = _mm_set1_ps(2.0f);
  const __m128 one = _mm_set1_ps(1.0f);
  
  for (uint32_t i = 0; i < frames; i += 4) {
    __m128 p = _mm_loadu_ps(&phases[i]);
    __m128 step = _mm_loadu_ps(&phaseSteps[i]);
    
    // Generate saw: phase * 2 - 1
    __m128 saw = _mm_sub_ps(_mm_mul_ps(p, two), one);
    _mm_storeu_ps(&output[i], saw);
    
    // Advance phase with wrap
    p = _mm_add_ps(p, step);
    __m128 mask = _mm_cmpge_ps(p, one);
    p = _mm_sub_ps(p, _mm_and_ps(mask, one));
    _mm_storeu_ps(&phases[i], p);
  }
#endif
}

#if SVMS_HAS_AVX2_INTRINSICS
// AVX2 stereo mix - process 8 samples at once
inline void MixStereo8(float* __restrict output,
                       const float* __restrict samples,
                       float leftGain, float rightGain,
                       uint32_t frames) {
  const __m256 lg = _mm256_set1_ps(leftGain);
  const __m256 rg = _mm256_set1_ps(rightGain);
  
  for (uint32_t frame = 0; frame < frames; frame += 8) {
    __m256 s = _mm256_loadu_ps(&samples[frame]);
    __m256 outL = _mm256_loadu_ps(&output[frame * 2]);
    __m256 outR = _mm256_loadu_ps(&output[frame * 2 + 1]);
    
    // Deinterleave
    __m256 even = _mm256_permute_ps(s, _MM_SHUFFLE(2, 0, 2, 0));
    __m256 odd = _mm256_permute_ps(s, _MM_SHUFFLE(3, 1, 3, 1));
    
    even = _mm256_mul_ps(even, lg);
    odd = _mm256_mul_ps(odd, rg);
    
    outL = _mm256_add_ps(outL, even);
    outR = _mm256_add_ps(outR, odd);
    
    _mm256_storeu_ps(&output[frame * 2], outL);
    _mm256_storeu_ps(&output[frame * 2 + 1], outR);
  }
  _mm256_zeroupper();
}

// AVX2 apply release decay - process 8 gains at once
inline void ApplyReleaseDecay8(float* __restrict gains,
                               const float* __restrict decays,
                               uint32_t count) {
  for (uint32_t i = 0; i < count; i += 8) {
    __m256 g = _mm256_loadu_ps(&gains[i]);
    __m256 d = _mm256_loadu_ps(&decays[i]);
    g = _mm256_mul_ps(g, d);
    _mm256_storeu_ps(&gains[i], g);
  }
  _mm256_zeroupper();
}
#endif // SVMS_HAS_AVX2_INTRINSICS

} // namespace simd

// Check CPU feature support (at virtuallysuper namespace level)
inline bool HasSSE2() {
#ifdef SVMS_LEGACY_XP
#ifdef _MSC_VER
  // Runtime CPUID check for XP
  int cpuInfo[4];
  __cpuid(cpuInfo, 1);
  return (cpuInfo[3] & (1 << 26)) != 0; // EDX bit 26 = SSE2
#else
  // Non-MSVC: assume available or use scalar fallback
  return true;
#endif
#else
#ifdef _M_AMD64
  return true;  // x64 always has SSE2
#else
  int cpuInfo[4];
  __cpuid(cpuInfo, 1);
  return (cpuInfo[3] & (1 << 26)) != 0;
#endif
#endif
}

inline bool HasAVX2() {
#if SVMS_HAS_AVX2_INTRINSICS
#ifdef _M_AMD64
  int cpuInfo[4];
  __cpuid(cpuInfo, 7);
  return (cpuInfo[1] & (1 << 5)) != 0; // EBX bit 5 = AVX2
#else
  return false;  // x86 builds don't use AVX2
#endif
#else
  return false;
#endif
}

// Also expose in simd namespace for convenience
namespace simd {
  inline bool HasSSE2() { return virtuallysuper::HasSSE2(); }
  inline bool HasAVX2() { return virtuallysuper::HasAVX2(); }
}

} // namespace virtuallysuper

#endif // VIRTUALLYSUPER_RENDER_SIMD_H
