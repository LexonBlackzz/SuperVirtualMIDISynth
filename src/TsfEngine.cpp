#define TSF_IMPLEMENTATION
#include "TsfEngine.h"
#include "CpuFeatures.h"
#ifndef SVMS_PERF_DEBUG
#define SVMS_PERF_DEBUG 0
#endif
#include "tsf.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <immintrin.h>
#include <windows.h>

#if defined(_MSC_VER) || defined(__AVX__)
#define SVMS_ENABLE_AVX_INTRINSICS 1
#else
#define SVMS_ENABLE_AVX_INTRINSICS 0
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
static const float kDefaultPitchBendRangeSemitones = 2.0f;

static int ClampMidiVelocity(int velocity) {
  if (velocity < 0)
    return 0;
  if (velocity > 127)
    return 127;
  return velocity;
}

static std::string ResolveSourcePath(const std::string &configuredPath) {
  if (configuredPath.empty())
    return configuredPath;

  if (GetFileAttributesA(configuredPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    return configuredPath;

  char dllPath[MAX_PATH];
  HMODULE hModule = reinterpret_cast<HMODULE>(&__ImageBase);
  if (hModule && GetModuleFileNameA(hModule, dllPath, MAX_PATH)) {
    char *lastSlash = strrchr(dllPath, '\\');
    if (lastSlash) {
      *(lastSlash + 1) = '\0';
      std::string fullPath = std::string(dllPath) + configuredPath;
      if (GetFileAttributesA(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return fullPath;
    }
  }

  return configuredPath;
}

static LARGE_INTEGER GetPerfFrequency() {
  LARGE_INTEGER freq = {};
  QueryPerformanceFrequency(&freq);
  return freq;
}

static float GetElapsedMs(const LARGE_INTEGER &start, const LARGE_INTEGER &end,
                         const LARGE_INTEGER &freq) {
  if (freq.QuadPart <= 0)
    return 0.0f;
  return (float)((double)(end.QuadPart - start.QuadPart) * 1000.0 /
                 (double)freq.QuadPart);
}

static uint32_t GetVoiceSampleClusterKey(const tsf *handle, int voiceIndex) {
  const struct tsf_voice *voice = &handle->voices[voiceIndex];
  const struct tsf_region *region = voice->region;
  unsigned int sampleIndex;
  unsigned int sampleEnd;
  if (!region || !handle->fontSamples)
    return 0;

  sampleEnd = (region->end > region->offset ? region->end - 1u : region->offset);
  if (voice->sourceSamplePosition <= (double)region->offset)
    sampleIndex = region->offset;
  else if (voice->sourceSamplePosition >= (double)sampleEnd)
    sampleIndex = sampleEnd;
  else
    sampleIndex = (unsigned int)voice->sourceSamplePosition;

  return (uint32_t)(sampleIndex >> 4);
}

static unsigned char GetVoiceRenderHelperClass(const tsf *handle, int voiceIndex,
                                               int numFrames) {
  if (!handle || voiceIndex < 0 || numFrames <= 0)
    return (unsigned char)TSF_RENDER_HELPER_COMPLEX;
  return (unsigned char)tsf_voice_render_helper_class(
      handle, &handle->voices[voiceIndex], numFrames);
}

#if SVMS_ENABLE_AVX_INTRINSICS
static void MergeWorkerBufferAVX2(float *dst, const float *src,
                                  int totalSamples,
                                  bool allowStreamingStore) {
  int sample = 0;
  const bool useStreamingStore =
      allowStreamingStore && totalSamples >= 2048 &&
      (((uintptr_t)dst & 31u) == 0);
  int avxCount = totalSamples & ~7;
  for (; sample < avxCount; sample += 8) {
    __m256 out = _mm256_loadu_ps(&dst[sample]);
    __m256 in = _mm256_loadu_ps(&src[sample]);
    __m256 sum = _mm256_add_ps(out, in);
    if (useStreamingStore)
      _mm256_stream_ps(&dst[sample], sum);
    else
      _mm256_storeu_ps(&dst[sample], sum);
  }
  for (; sample < totalSamples; ++sample)
    dst[sample] += src[sample];
  if (useStreamingStore)
    _mm_sfence();
}
#endif

static void MergeWorkerBufferSSE2(float *dst, const float *src,
                                  int totalSamples,
                                  bool allowStreamingStore) {
  int sample = 0;
  const bool useStreamingStore =
      allowStreamingStore && totalSamples >= 1024 &&
      (((uintptr_t)dst & 15u) == 0);
  int simdCount = totalSamples & ~3;
  for (; sample < simdCount; sample += 4) {
    __m128 out = _mm_loadu_ps(&dst[sample]);
    __m128 in = _mm_loadu_ps(&src[sample]);
    __m128 sum = _mm_add_ps(out, in);
    if (useStreamingStore)
      _mm_stream_ps(&dst[sample], sum);
    else
      _mm_storeu_ps(&dst[sample], sum);
  }
  for (; sample < totalSamples; ++sample)
    dst[sample] += src[sample];
  if (useStreamingStore)
    _mm_sfence();
}

} // namespace

void TsfEngine::ResetPerfCountersLocked() {
  perfHelperContiguousBlocks = 0;
  perfHelperGatherBlocks = 0;
  perfHelperComplexBlocks = 0;
  perfClusteredVoicesContiguous = 0;
  perfClusteredVoicesGather = 0;
  perfClusteredVoicesComplex = 0;
  perfSingleThreadFragments = 0;
  perfThreadedFragments = 0;
#if SVMS_PERF_DEBUG
  if (handle)
    tsf_reset_perf_counters(handle);
#endif
}

#ifndef SVMS_LEGACY_XP
void TsfEngine::EnsureWorkerBuffersCapacityLocked(int numFrames) {
  int requiredSamples = numFrames * 2;
  if (requiredSamples <= 0)
    return;

  for (int i = 0; i < numThreads; ++i) {
    if ((int)workerData[i].buffer.size() < requiredSamples)
      workerData[i].buffer.resize(requiredSamples);
  }
}
#endif

TsfEngine::TsfEngine()
    : handle(nullptr), sampleRate(44100), activeVoiceCount(0),
      lastVoiceStartMs(0.0f), lastSampleRenderMs(0.0f), lastNoteOnEvents(0),
      lastNoteOffEvents(0), realtimeOverloadState(0),
      realtimeSchedulerLagState(0), realtimeCadenceStreak(0),
      realtimeScheduledPending(0) {
  runtimeSettings.velocityCurve = 2.4f;
  runtimeSettings.velocityFloor = 0.0f;
  runtimeSettings.velocityIgnoreBelow = 0;
  runtimeSettings.asyncNoteStarts = true;
  for (int i = 0; i < 16; ++i)
    activeVoiceChannels[i] = 0;
#ifndef SVMS_LEGACY_XP
  numThreads = 0;
  currentGeneration.store(0);
  stopThreads.store(false);
  shutdownRequested.store(false);
  currentWorkerCount = 0;
  currentNumFrames = 0;
  currentRenderVoiceCount = 0;
#else
  stopThreads.store(false);
  shutdownRequested.store(false);
#endif
}

TsfEngine::~TsfEngine() { Shutdown(true); }

float TsfEngine::MapMidiVelocity(int velocity) const {
  if (velocity <= 0)
    return 0.0f;
  if (velocity >= 127)
    return 1.0f;

  int ignoreBelow = runtimeSettings.velocityIgnoreBelow;
  if (ignoreBelow < 0)
    ignoreBelow = 0;
  if (ignoreBelow > 126)
    ignoreBelow = 126;
  if (velocity <= ignoreBelow)
    return 0.0f;

  float normalized =
      static_cast<float>(velocity - ignoreBelow) / (127.0f - ignoreBelow);
  float curve = runtimeSettings.velocityCurve;
  if (curve < 0.25f)
    curve = 0.25f;
  if (curve > 6.0f)
    curve = 6.0f;

  float floor = runtimeSettings.velocityFloor;
  if (floor < 0.0f)
    floor = 0.0f;
  if (floor > 0.5f)
    floor = 0.5f;

  return floor + (1.0f - floor) * std::pow(normalized, curve);
}

bool TsfEngine::Initialize(const SamplerInitParams &params) {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  Shutdown(false);

  stopThreads.store(false);
  shutdownRequested.store(false);
  sampleRate = params.sampleRate;
  runtimeSettings = params.runtimeSettings;
  resolvedSourcePath = ResolveSourcePath(params.sourcePath);
  resolvedSourceFormat = DetectSourceFormat(resolvedSourcePath);

  handle = tsf_load_filename(resolvedSourcePath.c_str());
  if (!handle) {
    resolvedSourcePath.clear();
    resolvedSourceFormat.clear();
    OutputDebugStringA("SVMS: ERROR - Failed to load TSF source\n");
    return false;
  }

  tsf_set_output(handle, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
  if (!tsf_set_max_voices(handle, params.maxVoices)) {
    tsf_close(handle);
    handle = nullptr;
    resolvedSourcePath.clear();
    resolvedSourceFormat.clear();
    OutputDebugStringA("SVMS: ERROR - Failed to reserve TSF voices\n");
    return false;
  }

  ResetChannelsLocked();
  EnsureVoiceScratchCapacityLocked(params.maxVoices > 0 ? params.maxVoices
                                                        : kHardVoiceRenderLimit);

#ifndef SVMS_LEGACY_XP
  int desiredThreads = (int)std::thread::hardware_concurrency();
  if (desiredThreads < 1)
    desiredThreads = 1;
  if (desiredThreads > 1)
    desiredThreads -= 1;
  if (desiredThreads > 6)
    desiredThreads = 6;
  if (desiredThreads < 1)
    desiredThreads = 1;
  numThreads = desiredThreads;
  currentGeneration.store(0);
  currentWorkerCount = 0;
  currentNumFrames = 0;
  currentRenderVoiceCount = 0;
  workerData.clear();
  workerFinishedGen.clear();
  threads.clear();
  workerData.resize(numThreads);
  workerFinishedGen.assign(numThreads, 0);
  EnsureWorkerBuffersCapacityLocked(kThreadRenderChunkFrames);
  for (int i = 0; i < numThreads; ++i) {
    threads.emplace_back(&TsfEngine::WorkerThread, this, i);
  }
#endif

  UpdateVoiceStatsLocked();
  return true;
}

void TsfEngine::Shutdown(bool waitForThreads) {
  (void)waitForThreads;
  tsf *localHandle = nullptr;

#ifndef SVMS_LEGACY_XP
  stopThreads.store(true);
  shutdownRequested.store(true);
  {
    compat::LockGuard<compat::Mutex> lock(workerMutex);
      int targetGen = currentGeneration.load();
      for (size_t i = 0; i < workerFinishedGen.size(); ++i)
        workerFinishedGen[i] = targetGen;
  }
  workerCV.notify_all();
  masterCV.notify_all();

  for (size_t i = 0; i < threads.size(); ++i) {
    if (threads[i].joinable())
      threads[i].join();
  }
  threads.clear();
  workerData.clear();
  workerFinishedGen.clear();
#else
  stopThreads.store(true);
  shutdownRequested.store(true);
#endif

  renderVoiceIndices.clear();
  renderVoiceKeys.clear();
  renderVoiceTempIndices.clear();
  renderVoiceTempKeys.clear();
  cleanupPending = false;

  localHandle = handle;
  handle = nullptr;
  activeVoiceCount = 0;
  for (int i = 0; i < 16; ++i)
    activeVoiceChannels[i] = 0;
  lastVoiceStartMs = 0.0f;
  lastSampleRenderMs = 0.0f;
  lastNoteOnEvents = 0;
  lastNoteOffEvents = 0;
  realtimeOverloadState = 0;
  realtimeSchedulerLagState = 0;
  realtimeCadenceStreak = 0;
  realtimeScheduledPending = 0;

  if (localHandle)
    tsf_close(localHandle);

  resolvedSourcePath.clear();
  resolvedSourceFormat.clear();
}

void TsfEngine::ApplyDefaultChannelStateLocked(int channel) {
  if (!handle)
    return;
  tsf_channel_set_presetnumber(handle, channel, 0, (channel == 9));
  tsf_channel_set_volume(handle, channel, 1.0f);
  tsf_channel_set_pan(handle, channel, 0.5f);
  tsf_channel_set_pitchrange(handle, channel, kDefaultPitchBendRangeSemitones);
  tsf_channel_set_pitchwheel(handle, channel, 8192);
}

void TsfEngine::ResetChannelsLocked() {
  if (!handle)
    return;
  for (int i = 0; i < 16; ++i)
    ApplyDefaultChannelStateLocked(i);
}

void TsfEngine::Reset() {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (!handle)
    return;
  tsf_reset(handle);
  ResetChannelsLocked();
  UpdateVoiceStatsLocked();
}

void TsfEngine::ReloadRuntimeSettings(const RuntimeSettings &settings) {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  runtimeSettings = settings;
}

void TsfEngine::SetRealtimePressure(unsigned int overloadState,
                                    unsigned int schedulerLagState,
                                    unsigned int cadenceStreak,
                                    unsigned int scheduledPendingEvents) {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  realtimeOverloadState = overloadState;
  realtimeSchedulerLagState = schedulerLagState;
  realtimeCadenceStreak = cadenceStreak;
  realtimeScheduledPending = scheduledPendingEvents;
}

void TsfEngine::BeginRenderBlock() {
  lastVoiceStartMs = 0.0f;
  lastSampleRenderMs = 0.0f;
  lastNoteOnEvents = 0;
  lastNoteOffEvents = 0;
  cleanupPending = false;
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  ResetPerfCountersLocked();
}

void TsfEngine::EndRenderBlock() {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (handle && cleanupPending)
    tsf_cleanup_inactive_voices(handle);
  cleanupPending = false;
  UpdateVoiceStatsLocked();
}

void TsfEngine::ClusterRenderVoicesLocked(int voiceCount, int numFrames) {
  int pass;
  if (!handle || voiceCount < 2 || numFrames <= 0)
    return;
  if (voiceCount < kMinClusterVoices ||
      (long long)voiceCount * (long long)numFrames < (long long)kMinClusterWork)
    return;

  renderVoiceKeys.resize(voiceCount);
  renderVoiceTempIndices.resize(voiceCount);
  renderVoiceTempKeys.resize(voiceCount);

  for (int i = 0; i < voiceCount; ++i)
    renderVoiceKeys[i] = GetVoiceSampleClusterKey(handle, renderVoiceIndices[i]);

  {
    std::vector<int> *srcIndices = &renderVoiceIndices;
    std::vector<int> *dstIndices = &renderVoiceTempIndices;
    std::vector<uint32_t> *srcKeys = &renderVoiceKeys;
    std::vector<uint32_t> *dstKeys = &renderVoiceTempKeys;

    for (pass = 0; pass < 4; ++pass) {
      unsigned int counts[256] = {0};
      unsigned int offsets[256];
      unsigned int shift = (unsigned int)(pass * 8);

      for (int i = 0; i < voiceCount; ++i)
        counts[((*srcKeys)[i] >> shift) & 0xFFu]++;

      offsets[0] = 0;
      for (int bucket = 1; bucket < 256; ++bucket)
        offsets[bucket] = offsets[bucket - 1] + counts[bucket - 1];

      for (int i = 0; i < voiceCount; ++i) {
        unsigned int key = (*srcKeys)[i];
        unsigned int bucket = (key >> shift) & 0xFFu;
        unsigned int dst = offsets[bucket]++;
        (*dstIndices)[dst] = (*srcIndices)[i];
        (*dstKeys)[dst] = key;
      }

      {
        std::vector<int> *tmpIndices = srcIndices;
        std::vector<uint32_t> *tmpKeys = srcKeys;
        srcIndices = dstIndices;
        dstIndices = tmpIndices;
        srcKeys = dstKeys;
        dstKeys = tmpKeys;
      }
    }

    if (srcIndices != &renderVoiceIndices) {
      renderVoiceIndices.swap(*srcIndices);
      renderVoiceKeys.swap(*srcKeys);
    }
  }

  perfClusteredVoicesContiguous = 0;
  perfClusteredVoicesGather = 0;
  perfClusteredVoicesComplex = 0;
  for (int i = 0; i < voiceCount; ++i) {
    switch (GetVoiceRenderHelperClass(handle, renderVoiceIndices[i], numFrames)) {
    case TSF_RENDER_HELPER_CONTIGUOUS:
      ++perfClusteredVoicesContiguous;
      break;
    case TSF_RENDER_HELPER_GATHER:
      ++perfClusteredVoicesGather;
      break;
    default:
      ++perfClusteredVoicesComplex;
      break;
    }
  }
}

void TsfEngine::EnsureVoiceScratchCapacityLocked(int voiceCapacity) {
  if (voiceCapacity <= 0)
    return;
  if ((int)renderVoiceIndices.capacity() < voiceCapacity)
    renderVoiceIndices.reserve(voiceCapacity);
  if ((int)renderVoiceKeys.capacity() < voiceCapacity)
    renderVoiceKeys.reserve(voiceCapacity);
  if ((int)renderVoiceTempIndices.capacity() < voiceCapacity)
    renderVoiceTempIndices.reserve(voiceCapacity);
  if ((int)renderVoiceTempKeys.capacity() < voiceCapacity)
    renderVoiceTempKeys.reserve(voiceCapacity);
  if ((int)renderVoiceHelperClass.capacity() < voiceCapacity)
    renderVoiceHelperClass.reserve(voiceCapacity);
  if ((int)renderVoiceTempHelperClass.capacity() < voiceCapacity)
    renderVoiceTempHelperClass.reserve(voiceCapacity);
}

int TsfEngine::PrepareRenderVoicesLocked(int voiceCount) {
  if (!handle || voiceCount <= 0)
    return 0;

  EnsureVoiceScratchCapacityLocked(voiceCount);
  renderVoiceIndices.resize(voiceCount);

  const int *activeIndices = tsf_get_active_voice_indices(handle);
  int preparedCount = 0;
  for (int i = 0; i < voiceCount; ++i) {
    const int voiceIndex = activeIndices[i];
    const struct tsf_voice *voice = &handle->voices[voiceIndex];
    if (voice->playingPreset == -1)
      continue;
    renderVoiceIndices[preparedCount] = voiceIndex;
    ++preparedCount;
  }

  renderVoiceIndices.resize(preparedCount);
  if (preparedCount <= kHardVoiceRenderLimit)
    return preparedCount;

  std::nth_element(
      renderVoiceIndices.begin(),
      renderVoiceIndices.begin() + kHardVoiceRenderLimit, renderVoiceIndices.end(),
      [this](int lhs, int rhs) {
        const struct tsf_voice *left = &handle->voices[lhs];
        const struct tsf_voice *right = &handle->voices[rhs];
        const float leftScore =
            tsf_voice_estimated_gain(left) * (float)left->midiVelocity;
        const float rightScore =
            tsf_voice_estimated_gain(right) * (float)right->midiVelocity;
        if (leftScore != rightScore)
          return leftScore > rightScore;
        return left->playIndex > right->playIndex;
      });

  for (int i = kHardVoiceRenderLimit; i < preparedCount; ++i) {
    struct tsf_voice *voice = &handle->voices[renderVoiceIndices[i]];
    tsf_voice_endquick(handle, voice);
  }

  renderVoiceIndices.resize(kHardVoiceRenderLimit);
  return kHardVoiceRenderLimit;
}

void TsfEngine::ProcessMidiEvent(const MidiEvent &event) {
  if (!handle)
    return;

  switch (event.type) {
  case MidiEvent::NOTE_ON: {
    float mappedVelocity = MapMidiVelocity(event.data2);
    if (mappedVelocity > 0.0f) {
      tsf_channel_note_on_ex(handle, event.channel, event.data1, mappedVelocity,
                             ClampMidiVelocity(event.data2));
      lastNoteOnEvents++;
    }
    break;
  }
  case MidiEvent::NOTE_OFF:
    tsf_channel_note_off(handle, event.channel, event.data1);
    lastNoteOffEvents++;
    break;
  case MidiEvent::PROGRAM_CHANGE:
    tsf_channel_set_presetnumber(handle, event.channel, event.data1,
                                 (event.channel == 9));
    break;
  case MidiEvent::CONTROL_CHANGE:
    tsf_channel_midi_control(handle, event.channel, event.data1, event.data2);
    if (event.data1 == 121)
      tsf_channel_set_pitchrange(handle, event.channel,
                                 kDefaultPitchBendRangeSemitones);
    break;
  case MidiEvent::PITCH_BEND:
    tsf_channel_set_pitchwheel(handle, event.channel, event.data1);
    break;
  case MidiEvent::RESET:
    tsf_reset(handle);
    ResetChannelsLocked();
    break;
  }
}

void TsfEngine::UpdateVoiceStatsLocked() {
  activeVoiceCount = 0;
  for (int i = 0; i < 16; ++i)
    activeVoiceChannels[i] = 0;

  if (!handle)
    return;

  activeVoiceCount = (DWORD)tsf_active_voice_count(handle);
  int channelCounts[16] = {0};
  tsf_get_active_voice_channel_counts(handle, channelCounts, 16);
  for (int i = 0; i < 16; ++i)
    activeVoiceChannels[i] = (DWORD)channelCounts[i];
}

#ifndef SVMS_LEGACY_XP
void TsfEngine::WorkerThread(int id) {
  while (!stopThreads.load()) {
    int targetGen = 0;
    {
      std::unique_lock<compat::Mutex> lock(workerMutex);
      workerCV.wait(lock, [this, id] {
        return shutdownRequested.load() || stopThreads.load() ||
               currentGeneration.load() > workerFinishedGen[id];
      });
      targetGen = currentGeneration.load();
    }

    if (stopThreads.load())
      break;

    int bufferSamples = currentNumFrames * 2;

    if (id < currentWorkerCount && handle && currentRenderVoiceCount > 0) {
      int totalPartitions = currentWorkerCount + 1;
      int voicesPerPartition =
          (currentRenderVoiceCount + totalPartitions - 1) / totalPartitions;
      int startIdx = (id + 1) * voicesPerPartition;
      int endIdx =
          (std::min)((id + 2) * voicesPerPartition, currentRenderVoiceCount);
      std::memset(workerData[id].buffer.data(), 0, bufferSamples * sizeof(float));
      if (startIdx < endIdx) {
        tsf_render_float_indexed(handle, workerData[id].buffer.data(),
                                 currentNumFrames, 1,
                                 renderVoiceIndices.data() + startIdx,
                                 endIdx - startIdx);
      }
    } else if (id < (int)workerData.size()) {
      std::memset(workerData[id].buffer.data(), 0, bufferSamples * sizeof(float));
    }

    {
      compat::LockGuard<compat::Mutex> lock(workerMutex);
      workerFinishedGen[id] = targetGen;
      bool allDone = true;
      for (int i = 0; i < currentWorkerCount; ++i) {
        if (workerFinishedGen[i] != targetGen) {
          allDone = false;
          break;
        }
      }
      if (allDone)
        masterCV.notify_one();
    }
  }
}
#endif

void TsfEngine::Render(float *output, int numFrames) {
  if (numFrames <= 0) {
    return;
  }

  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (!handle || shutdownRequested.load() || stopThreads.load()) {
    std::fill(output, output + numFrames * 2, 0.0f);
    return;
  }

  LARGE_INTEGER freq = GetPerfFrequency();
  LARGE_INTEGER renderStart = {};
  LARGE_INTEGER renderEnd = {};
  QueryPerformanceCounter(&renderStart);
  int voiceCount = tsf_active_voice_count(handle);
  int preparedVoiceCount = PrepareRenderVoicesLocked(voiceCount);
  long long renderWork = (long long)preparedVoiceCount * (long long)numFrames;

  if (preparedVoiceCount <= 0) {
    std::fill(output, output + numFrames * 2, 0.0f);
    QueryPerformanceCounter(&renderEnd);
    lastSampleRenderMs += GetElapsedMs(renderStart, renderEnd, freq);
    return;
  }

  ClusterRenderVoicesLocked(preparedVoiceCount, numFrames);

#ifdef SVMS_LEGACY_XP
  ++perfSingleThreadFragments;
  tsf_render_float_indexed(handle, output, numFrames, 0, renderVoiceIndices.data(),
                           preparedVoiceCount);
  cleanupPending = true;
  QueryPerformanceCounter(&renderEnd);
  lastSampleRenderMs += GetElapsedMs(renderStart, renderEnd, freq);
  return;
#else
  int desiredPartitions =
      (std::min)(numThreads + 1,
                 (std::max)(1, (preparedVoiceCount + kVoicesPerWorker - 1) /
                                   kVoicesPerWorker));
  int targetWorkers = desiredPartitions - 1;
  if (numFrames < kMinThreadedFrames ||
      renderWork < (long long)kMinThreadedWork || targetWorkers < 2) {
    ++perfSingleThreadFragments;
    tsf_render_float_indexed(handle, output, numFrames, 0,
                             renderVoiceIndices.data(), preparedVoiceCount);
    cleanupPending = true;
    QueryPerformanceCounter(&renderEnd);
    lastSampleRenderMs += GetElapsedMs(renderStart, renderEnd, freq);
    return;
  }

  EnsureWorkerBuffersCapacityLocked((std::min)(numFrames, kThreadRenderChunkFrames));
  currentRenderVoiceCount = preparedVoiceCount;
  currentWorkerCount = targetWorkers;
  ++perfThreadedFragments;
  {
    int voicesPerPartition =
        (preparedVoiceCount + desiredPartitions - 1) / desiredPartitions;
    int mainEndIdx = (std::min)(voicesPerPartition, preparedVoiceCount);
    int chunkOffset = 0;

    while (chunkOffset < numFrames) {
      int chunkFrames =
          (std::min)(kThreadRenderChunkFrames, numFrames - chunkOffset);
      float *chunkOutput = output + chunkOffset * 2;
      int generation = 0;

      currentNumFrames = chunkFrames;
      generation = currentGeneration.fetch_add(1) + 1;
      {
        compat::LockGuard<compat::Mutex> workerLock(workerMutex);
        for (int i = 0; i < currentWorkerCount; ++i)
          workerFinishedGen[i] = generation - 1;
      }

      workerCV.notify_all();

      if (mainEndIdx > 0)
        tsf_render_float_indexed(handle, chunkOutput, chunkFrames, 0,
                                 renderVoiceIndices.data(), mainEndIdx);
      else
        std::fill(chunkOutput, chunkOutput + chunkFrames * 2, 0.0f);

      {
        std::unique_lock<compat::Mutex> workerLock(workerMutex);
        masterCV.wait(workerLock, [this, generation] {
          if (stopThreads.load())
            return true;
          for (int i = 0; i < currentWorkerCount; ++i) {
            if (workerFinishedGen[i] != generation)
              return false;
          }
          return true;
        });
      }

      if (stopThreads.load() || shutdownRequested.load()) {
        std::fill(output, output + numFrames * 2, 0.0f);
        return;
      }

      {
        int totalSamples = chunkFrames * 2;
#if SVMS_ENABLE_AVX_INTRINSICS
        if (CpuFeatures::HasAVX2()) {
          for (int workerIndex = 0; workerIndex < currentWorkerCount;
               ++workerIndex) {
            bool isLastMerge = (workerIndex + 1 == currentWorkerCount);
            MergeWorkerBufferAVX2(chunkOutput, workerData[workerIndex].buffer.data(),
                                  totalSamples, isLastMerge);
          }
          _mm256_zeroupper();
        } else {
#endif
          for (int workerIndex = 0; workerIndex < currentWorkerCount;
               ++workerIndex) {
            bool isLastMerge = (workerIndex + 1 == currentWorkerCount);
            MergeWorkerBufferSSE2(chunkOutput, workerData[workerIndex].buffer.data(),
                                  totalSamples, isLastMerge);
          }
#if SVMS_ENABLE_AVX_INTRINSICS
        }
#endif
      }

      chunkOffset += chunkFrames;
    }
  }
#endif

  cleanupPending = true;
  QueryPerformanceCounter(&renderEnd);
  lastSampleRenderMs += GetElapsedMs(renderStart, renderEnd, freq);
}

std::string TsfEngine::GetResolvedSourcePath() const {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  return resolvedSourcePath;
}

std::string TsfEngine::GetResolvedSourceFormat() const {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  return resolvedSourceFormat;
}

std::string TsfEngine::GetEngineName() const { return "tsf"; }

DWORD TsfEngine::GetActiveVoiceStats(DWORD *channelCounts, int count) const {
  if (channelCounts && count > 0) {
    for (int i = 0; i < count; ++i)
      channelCounts[i] = 0;
  }

  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (channelCounts && count > 0) {
    int limit = count < 16 ? count : 16;
    for (int i = 0; i < limit; ++i)
      channelCounts[i] = activeVoiceChannels[i];
  }
  return activeVoiceCount;
}

SamplerDiagnostics TsfEngine::GetDiagnostics() const {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  SamplerDiagnostics diagnostics;
  diagnostics.noteOnEventsThisBlock = lastNoteOnEvents;
  diagnostics.noteOffEventsThisBlock = lastNoteOffEvents;
  diagnostics.voiceStartMs = lastVoiceStartMs;
  diagnostics.sampleRenderMs = lastSampleRenderMs;
  diagnostics.perfCountersEnabled = SVMS_PERF_DEBUG ? 1u : 0u;
  diagnostics.tsfSingleThreadFragments = perfSingleThreadFragments;
  diagnostics.tsfThreadedFragments = perfThreadedFragments;
  diagnostics.tsfClusteredVoicesContiguous = perfClusteredVoicesContiguous;
  diagnostics.tsfClusteredVoicesGather = perfClusteredVoicesGather;
  diagnostics.tsfClusteredVoicesComplex = perfClusteredVoicesComplex;
  if (handle) {
#if SVMS_PERF_DEBUG
    diagnostics.tsfHelperContiguousBlocks = handle->perfHelperContiguousBlocks;
    diagnostics.tsfHelperGatherBlocks = handle->perfHelperGatherBlocks;
    diagnostics.tsfHelperComplexBlocks = handle->perfHelperComplexBlocks;
#endif
    for (int i = 0; i < 16; ++i)
      diagnostics.pitchBendRange[i] = tsf_channel_get_pitchrange(handle, i);
  }
  return diagnostics;
}
