#include "Synth.h"
#include "BassMidiEngine.h"
#include "Config.h"
#ifndef SVMS_LEGACY_XP
#include "SfzEngine.h"
#endif
#include "TsfEngine.h"
#include "VirtuallySuper/VirtuallySuperSamplerEngine.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <memory>
#include <vector>
#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
#ifdef SVMS_LEGACY_XP
static const size_t kMaxPendingMidiEvents = 65536;
static const size_t kMaxScheduledNoteTransitions = 32768;
static const unsigned int kMaxMidiEventsPerRender = 2048;
static const unsigned int kSoftDeferredThreshold = 4096;
static const unsigned int kHardDeferredThreshold = 16384;
static const unsigned int kHardNoteOnStartsPerBlock = 192;
static const unsigned int kAccurateWorkerBatchLimit = 128;
static const unsigned int kAccurateFallbackBatchLimit = 192;
#else
static const size_t kMaxPendingMidiEvents = 262144;
static const size_t kMaxScheduledNoteTransitions = 131072;
static const unsigned int kMaxMidiEventsPerRender = 4096;
static const unsigned int kSoftDeferredThreshold = 8192;
static const unsigned int kHardDeferredThreshold = 32768;
static const unsigned int kHardNoteOnStartsPerBlock = 512;
static const unsigned int kAccurateWorkerBatchLimit = 256;
static const unsigned int kAccurateFallbackBatchLimit = 384;
#endif
static const int kHardOverloadVelocityFloor = 25;
static const float kSafeAudioBudgetFactor = 0.95f;
static const float kMidiBudgetFraction = 0.35f;
static const float kMaxMidiBudgetMs = 1.5f;
static const float kMinMidiBudgetMs = 0.10f;
static const unsigned int kQueueTrimTarget = kSoftDeferredThreshold / 2;
static const unsigned int kExtremePendingNoteOnThreshold =
    kHardDeferredThreshold / 2;
static const unsigned int kHardDueApplyEventLimit = 96;
static const unsigned int kSoftDueApplyEventLimit = 192;
static const unsigned int kReleaseLaneMaxEvents = 512;
static const unsigned int kReleaseLaneApplyLimit = 64;
static const unsigned int kTotalIngressHardCap = kHardDeferredThreshold;
static const unsigned int kFallbackScheduledBatchCap = 512;
static const unsigned int kAccurateSoftWorksetCap = 256;
static const unsigned int kAccurateHardWorksetCap = 128;
static const unsigned int kScheduledSoftPendingCap = kQueueTrimTarget;
static const unsigned int kScheduledHardPendingCap = kSoftDeferredThreshold;
static const unsigned int kScheduledTrimHeadroom = 256;
static const unsigned int kAccurateWorkerSoftBatchLimit =
    kAccurateWorkerBatchLimit / 2;
static const unsigned int kAccurateWorkerHardBatchLimit =
    kAccurateWorkerBatchLimit / 4;
static const unsigned int kAccurateBlockedRetrySoftLimit = 128;
static const unsigned int kAccurateBlockedRetryHardLimit = 64;
static const unsigned int kHardDueOnTimeBudget = 24;
static const unsigned int kSoftDueOnTimeBudget = 64;
static const unsigned int kHardDueOverdueBudget = 8;
static const unsigned int kSoftDueOverdueBudget = 24;
static const unsigned int kIngressRingCapacity = 1u << 20;
static const unsigned int kRenderPollSliceFrames = 32;
static const unsigned int kAccurateCoalesceWindowSamples = 8;
static const unsigned int kAccurateDrainBatchLimit = 1024;
static const int kSoftScheduledTrimVelocityFloor = 60;
static const unsigned int kSchedulerWarmupBlockCount = 8;

#ifndef SVMS_PERF_DEBUG
#define SVMS_PERF_DEBUG 0
#endif

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

static unsigned int GetConfiguredVoiceCountForOverload(int configuredMaxVoices) {
  return configuredMaxVoices > 0 ? (unsigned int)configuredMaxVoices : 500u;
}

static unsigned int ClampAdaptiveThreshold(unsigned long long value,
                                           unsigned int minValue,
                                           unsigned int maxValue) {
  if (value < (unsigned long long)minValue)
    return minValue;
  if (value > (unsigned long long)maxValue)
    return maxValue;
  return (unsigned int)value;
}

static unsigned int GetAdaptiveSoftDeferredThreshold(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices * 2u,
                                kSoftDeferredThreshold,
                                (unsigned int)kMaxPendingMidiEvents / 2u);
}

static unsigned int GetAdaptiveHardDeferredThreshold(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  const unsigned int softThreshold =
      GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
  const unsigned int minThreshold = std::max<unsigned int>(
      kHardDeferredThreshold, softThreshold + kScheduledTrimHeadroom);
  return ClampAdaptiveThreshold((unsigned long long)voices * 4u, minThreshold,
                                (unsigned int)kMaxPendingMidiEvents - 1024u);
}

static unsigned int GetAdaptiveScheduledSoftPendingCap(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices * 2u,
                                kScheduledSoftPendingCap,
                                (unsigned int)kMaxScheduledNoteTransitions / 2u);
}

static unsigned int GetAdaptiveScheduledHardPendingCap(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  const unsigned int softCap =
      GetAdaptiveScheduledSoftPendingCap(configuredMaxVoices);
  const unsigned int minCap = std::max<unsigned int>(
      kScheduledHardPendingCap, softCap + kScheduledTrimHeadroom);
  return ClampAdaptiveThreshold((unsigned long long)voices * 3u, minCap,
                                (unsigned int)kMaxScheduledNoteTransitions -
                                    1024u);
}

static unsigned int GetAdaptiveExtremePendingThreshold(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices * 3u / 2u,
                                kExtremePendingNoteOnThreshold,
                                GetAdaptiveHardDeferredThreshold(
                                    configuredMaxVoices));
}

static unsigned int GetAdaptiveHardNoteOnStartsPerBlock(
    int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices / 4u,
                                kHardNoteOnStartsPerBlock,
                                kHardNoteOnStartsPerBlock * 4u);
}

static unsigned int GetAdaptiveSoftDueApplyLimit(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices / 8u,
                                kSoftDueApplyEventLimit, 768u);
}

static unsigned int GetAdaptiveHardDueApplyLimit(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices / 16u,
                                kHardDueApplyEventLimit, 384u);
}

static unsigned int GetAdaptiveSoftDueOnTimeBudget(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices / 12u,
                                kSoftDueOnTimeBudget, 384u);
}

static unsigned int GetAdaptiveHardDueOnTimeBudget(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices / 24u,
                                kHardDueOnTimeBudget, 192u);
}

static unsigned int GetAdaptiveSoftDueOverdueBudget(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices / 32u,
                                kSoftDueOverdueBudget, 96u);
}

static unsigned int GetAdaptiveHardDueOverdueBudget(int configuredMaxVoices) {
  const unsigned int voices =
      GetConfiguredVoiceCountForOverload(configuredMaxVoices);
  return ClampAdaptiveThreshold((unsigned long long)voices / 48u,
                                kHardDueOverdueBudget, 48u);
}

static long long GetElapsedSamples(long long deltaQpc, int sampleRate,
                                   const LARGE_INTEGER &freq) {
  if (deltaQpc <= 0 || sampleRate <= 0 || freq.QuadPart <= 0)
    return 0;
  return (long long)((double)deltaQpc * (double)sampleRate /
                     (double)freq.QuadPart);
}

template <typename QueueType>
static unsigned int GetDeferredEventCount(const QueueType &critical,
                                          const QueueType &realtime,
                                          const QueueType &noteOn) {
  return static_cast<unsigned int>(critical.size() + realtime.size() +
                                   noteOn.size());
}

static unsigned int UpdateObservedMax(std::atomic<unsigned int> &target,
                                      unsigned int depth) {
  unsigned int observedMax = target.load(std::memory_order_relaxed);
  while (depth > observedMax &&
         !target.compare_exchange_weak(observedMax, depth,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
  }
  return target.load(std::memory_order_relaxed);
}

static unsigned int NextPowerOfTwo(unsigned int value) {
  if (value <= 2u)
    return 2u;
  --value;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  return value + 1u;
}

static void InsertMidiEventBySequence(std::vector<MidiEvent> &events,
                                      const MidiEvent &event) {
  std::vector<MidiEvent>::iterator pos = events.end();
  while (pos != events.begin() && (pos - 1)->sequence > event.sequence)
    --pos;
  events.insert(pos, event);
}

static void MergeMidiEventVectorsBySequence(std::vector<MidiEvent> &dst,
                                            const std::vector<MidiEvent> &src) {
  if (src.empty())
    return;
  if (dst.empty()) {
    dst = src;
    return;
  }

  std::vector<MidiEvent> merged;
  merged.reserve(dst.size() + src.size());
  size_t dstIndex = 0;
  size_t srcIndex = 0;
  while (dstIndex < dst.size() && srcIndex < src.size()) {
    if (dst[dstIndex].sequence <= src[srcIndex].sequence)
      merged.push_back(dst[dstIndex++]);
    else
      merged.push_back(src[srcIndex++]);
  }
  while (dstIndex < dst.size())
    merged.push_back(dst[dstIndex++]);
  while (srcIndex < src.size())
    merged.push_back(src[srcIndex++]);
  dst.swap(merged);
}

static std::string GetConfiguredSoundSource() {
  std::string source =
      Config::Instance().GetString("sound_source", std::string());
  if (source.empty())
    source = Config::Instance().GetString("soundfont", "gm.sf2");
  return source;
}

static std::string GetConfiguredSamplerEngine() {
  return Config::Instance().GetString("sampler_engine", "auto");
}

static SamplerEngineId ResolveImplementedEngine(SamplerEngineId requestedEngine,
                                                const std::string &sourcePath) {
  SamplerEngineId resolved =
      ResolveSamplerEngineId(requestedEngine, sourcePath);
  if (resolved == SamplerEngineId::SFZ) {
    std::string format = DetectSourceFormat(sourcePath);
    if (format != "sfz") {
      OutputDebugStringA("SVMS: SFZ engine requested for non-SFZ source, "
                         "falling back to TSF\n");
      return SamplerEngineId::TSF;
    }
  }
  return resolved;
}

static bool FileExists(const std::string &path) {
  return !path.empty() &&
         GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool IsAbsolutePath(const std::string &path) {
  return path.size() > 2 && path[1] == ':' &&
         (path[2] == '\\' || path[2] == '/');
}

static std::string GetModuleDirectory() {
  char path[MAX_PATH];
  DWORD length = GetModuleFileNameA(
      reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
  if (!length || length >= MAX_PATH)
    return std::string();
  char *slash = strrchr(path, '\\');
  if (!slash)
    return std::string();
  *slash = '\0';
  return std::string(path);
}

static void AddUniqueCandidate(std::vector<std::string> &candidates,
                               const std::string &candidate) {
  if (candidate.empty())
    return;
  if (std::find(candidates.begin(), candidates.end(), candidate) !=
      candidates.end())
    return;
  candidates.push_back(candidate);
}

static std::vector<std::string> BuildSourceCandidates(
    const std::string &configuredSource, SamplerEngineId requestedEngine) {
  std::vector<std::string> candidates;
  std::string legacySource = Config::Instance().GetString("soundfont", "gm.sf2");
  std::string moduleDir = GetModuleDirectory();
  std::string configuredFormat = DetectSourceFormat(configuredSource);

  AddUniqueCandidate(candidates, configuredSource);
  if (!configuredSource.empty() && !IsAbsolutePath(configuredSource) &&
      !moduleDir.empty()) {
    AddUniqueCandidate(candidates, moduleDir + "\\" + configuredSource);
  }

  if (configuredFormat == "sfz" || requestedEngine == SamplerEngineId::SFZ)
    return candidates;

  AddUniqueCandidate(candidates, legacySource);

  const char *fallbackNames[] = {"gm.sf2", "gm.sf3", "default.sf2"};
  for (size_t i = 0; i < sizeof(fallbackNames) / sizeof(fallbackNames[0]); ++i) {
    AddUniqueCandidate(candidates, fallbackNames[i]);
    if (!moduleDir.empty()) {
      AddUniqueCandidate(candidates,
                         moduleDir + "\\" + std::string(fallbackNames[i]));
    }
  }

  return candidates;
}

static bool IsResetLikeEvent(const MidiEvent &event) {
  if (event.type == MidiEvent::RESET)
    return true;
  if (event.type != MidiEvent::CONTROL_CHANGE)
    return false;
  return event.data1 == 120 || event.data1 == 121 || event.data1 == 123;
}

static void LogTimingDebug(const char *format, ...) {
#if SVMS_PERF_DEBUG
  char buffer[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  OutputDebugStringA(buffer);
#else
  (void)format;
#endif
}

} // namespace

Synth &Synth::Instance() {
  static Synth instance;
  return instance;
}

Synth::Synth() {
  runtimeSettings.velocityCurve = 2.4f;
  runtimeSettings.velocityFloor = 0.0f;
  runtimeSettings.velocityIgnoreBelow = 0;
  runtimeSettings.asyncNoteStarts = true;
  runtimeSettings.eventTimingMode = EventTimingMode::ACCURATE;
  eventTimingMode = EventTimingMode::ACCURATE;
  eventTimingModeFast.store((int)EventTimingMode::ACCURATE,
                            std::memory_order_relaxed);
  lastInitStatus = "Synth not initialized";
  InitializeEventRing(realtimeIngressRing, kIngressRingCapacity);
  InitializeEventRing(accurateIngressRing, kIngressRingCapacity);
  InitializeFlatMidiQueue(deferredCriticalEvents, kSoftDeferredThreshold);
  InitializeFlatMidiQueue(deferredRealtimeEvents, kSoftDeferredThreshold);
  InitializeFlatMidiQueue(deferredNoteOnEvents, kHardDeferredThreshold);
  InitializeFlatMidiQueue(overloadReleaseEvents, kReleaseLaneMaxEvents);
  InitializeFlatMidiQueue(accuratePendingEvents, kHardDeferredThreshold);
  pendingCriticalEvents.reserve(kAccurateDrainBatchLimit);
  pendingRealtimeEvents.reserve(kAccurateDrainBatchLimit);
  pendingNoteOnEvents.reserve(kAccurateDrainBatchLimit);
  incomingCriticalEvents.reserve(kAccurateDrainBatchLimit);
  incomingRealtimeEvents.reserve(kAccurateDrainBatchLimit);
  incomingNoteOnEvents.reserve(kAccurateDrainBatchLimit);
  dueEventsScratch.reserve(kAccurateDrainBatchLimit);
  releaseEventsScratch.reserve(kReleaseLaneApplyLimit);
  scheduledIngressScratch.reserve(kAccurateDrainBatchLimit);
  accurateWorksetScratch.reserve(kAccurateDrainBatchLimit);
  heapIndexByEventId.reserve(kHardDeferredThreshold + 1u);
  scheduledHeap.reserve(kHardDeferredThreshold);
  scheduledNoteOnTrimHeap.reserve(kHardDeferredThreshold);
  accurateEventWakeEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
  accurateEventStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
  acceptingEvents.store(0, std::memory_order_relaxed);
  realtimeIngressDepth.store(0, std::memory_order_relaxed);
  accurateIngressDepthAtomic.store(0, std::memory_order_relaxed);
  ResetScheduledStateLocked();
}

Synth::~Synth() {
  Shutdown();
  if (accurateEventWakeEvent) {
    CloseHandle(accurateEventWakeEvent);
    accurateEventWakeEvent = NULL;
  }
  if (accurateEventStopEvent) {
    CloseHandle(accurateEventStopEvent);
    accurateEventStopEvent = NULL;
  }
}

void Synth::InitializeEventRing(EventRingBuffer &ring, unsigned int capacity) {
  const unsigned int normalizedCapacity = NextPowerOfTwo(capacity);
  ring.slots.clear();
  ring.slots.resize(normalizedCapacity);
  ring.capacity = (LONG)normalizedCapacity;
  ring.mask = (LONG)(normalizedCapacity - 1u);
  ring.readIndex = 0;
  ring.writeIndex = 0;
}

void Synth::ResetEventRing(EventRingBuffer &ring) {
  ring.readIndex = 0;
  ring.writeIndex = 0;
}

void Synth::InitializeFlatMidiQueue(FlatMidiQueue &queue, unsigned int capacity) {
  const unsigned int normalizedCapacity = NextPowerOfTwo(capacity > 0 ? capacity : 2u);
  queue.slots.clear();
  queue.slots.resize(normalizedCapacity);
  queue.mask = normalizedCapacity - 1u;
  queue.clear();
}

void Synth::ResetFlatMidiQueue(FlatMidiQueue &queue) { queue.clear(); }

bool Synth::TryPushEventRing(EventRingBuffer &ring, const MidiEvent &event,
                             std::atomic<unsigned int> &depthCounter) {
  if (ring.capacity <= 1)
    return false;

  const LONG writeIndex = ring.writeIndex;
  const LONG readIndex = ring.readIndex;
  const LONG nextWrite = (writeIndex + 1) & ring.mask;
  if (nextWrite == readIndex)
    return false;

  ring.slots[(size_t)writeIndex] = event;
  MemoryBarrier();
  InterlockedExchange(const_cast<LONG *>(&ring.writeIndex), nextWrite);

  unsigned int depth = (writeIndex >= readIndex)
                           ? (unsigned int)(writeIndex - readIndex + 1)
                           : (unsigned int)(ring.capacity - (readIndex - writeIndex) +
                                            1);
  depthCounter.store(depth, std::memory_order_relaxed);
  return true;
}

unsigned int
Synth::DrainEventRingToPendingLocked(EventRingBuffer &ring,
                                     std::atomic<unsigned int> &depthCounter,
                                     bool accurateRoute, unsigned int maxCount) {
  if (maxCount == 0 || ring.capacity <= 1)
    return 0;

  LONG readIndex = ring.readIndex;
  const LONG writeIndex = ring.writeIndex;
  unsigned int drained = 0;
  while (readIndex != writeIndex && drained < maxCount) {
    const MidiEvent &event = ring.slots[(size_t)readIndex];
    if (accurateRoute)
      accuratePendingEvents.push_back(event);
    else
      EnqueuePendingEventLocked(event);
    readIndex = (readIndex + 1) & ring.mask;
    ++drained;
  }

  if (drained != 0) {
    MemoryBarrier();
    InterlockedExchange(const_cast<LONG *>(&ring.readIndex), readIndex);
  }

  const LONG publishedWrite = ring.writeIndex;
  unsigned int depth = 0;
  if (publishedWrite != readIndex) {
    depth = (publishedWrite > readIndex)
                ? (unsigned int)(publishedWrite - readIndex)
                : (unsigned int)(ring.capacity - (readIndex - publishedWrite));
  }
  depthCounter.store(depth, std::memory_order_relaxed);
  return drained;
}

RuntimeSettings Synth::LoadRuntimeSettingsLocked() const {
  RuntimeSettings settings;
  settings.velocityCurve = Config::Instance().GetFloat("velocity_curve", 2.4f);
  settings.velocityFloor = Config::Instance().GetFloat("velocity_floor", 0.0f);
  settings.velocityIgnoreBelow =
      Config::Instance().GetInt("velocity_ignore_below", 0);
  settings.asyncNoteStarts =
      Config::Instance().GetBool("async_note_starts", true);
  std::string configuredMode =
      Config::Instance().GetString("event_timing_mode", std::string());
  if (!configuredMode.empty()) {
    settings.eventTimingMode = ParseEventTimingMode(configuredMode);
  } else {
    settings.eventTimingMode =
        settings.asyncNoteStarts ? EventTimingMode::ACCURATE
                                 : EventTimingMode::LEGACY_SYNC;
  }
  return settings;
}

void Synth::ReloadRuntimeSettings() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  EventTimingMode previousTimingMode = eventTimingMode;
  RuntimeSettings previousSettings = runtimeSettings;
  runtimeSettings = LoadRuntimeSettingsLocked();
  eventTimingMode = runtimeSettings.eventTimingMode;
  eventTimingModeFast.store((int)eventTimingMode, std::memory_order_relaxed);
  ++runtimeReloadCount;
  LogTimingDebug("SVMS: Timing reload %s -> %s\n",
                 EventTimingModeToConfigString(previousTimingMode),
                 EventTimingModeToConfigString(eventTimingMode));
  if (engine)
    engine->ReloadRuntimeSettings(runtimeSettings);
  bool preserveSchedulerState =
      previousTimingMode == eventTimingMode &&
      previousSettings.eventTimingMode == runtimeSettings.eventTimingMode;
  {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    if (preserveSchedulerState) {
      unsigned int deferredCount = GetDeferredEventCount(
          deferredCriticalEvents, deferredRealtimeEvents, deferredNoteOnEvents);
      unsigned int ingressCount = GetIngressDepthLocked();
      unsigned int scheduledCount = ComputeScheduledPendingCountLocked();
      bool schedulerHealthy =
          overloadState == 0 && lastAsyncLagState == 0 &&
          lastSchedulerLagSamples == 0 &&
          consecutiveAccurateBlockStartApplies < 2 &&
          deferredCount == 0 && ingressCount == 0 && scheduledCount == 0 &&
          accuratePendingEvents.empty() && overloadReleaseEvents.empty() &&
          realtimeIngressDepth.load(std::memory_order_relaxed) == 0 &&
          accurateIngressDepthAtomic.load(std::memory_order_relaxed) == 0;
      if (!schedulerHealthy)
        preserveSchedulerState = false;
    }

    if (!preserveSchedulerState) {
      FlushScheduledEventsLocked();
      accuratePendingEvents.clear();
      ResetEventRing(realtimeIngressRing);
      ResetEventRing(accurateIngressRing);
      realtimeIngressDepth.store(0, std::memory_order_relaxed);
      accurateIngressDepthAtomic.store(0, std::memory_order_relaxed);
      accurateEventClockBaseSample.store(0, std::memory_order_relaxed);
      accurateEventClockBaseQpc.store(0, std::memory_order_relaxed);
      accurateEventClockSampleRate.store(0, std::memory_order_relaxed);
      consecutiveAccurateBlockStartApplies = 0;
      overloadState = 0;
      lastAsyncLagState = 0;
      lastSchedulerLagSamples = 0;
      lastSchedulerLateEvents = 0;
      accurateReleaseLaneDepth = 0;
      accurateIngressDepth = 0;
      schedulerWarmupBlocks = kSchedulerWarmupBlockCount;
      ++accurateClockResetCount;
      LogTimingDebug(
          "SVMS: Runtime reload reset accurate scheduler state mode=%s\n",
          EventTimingModeToConfigString(eventTimingMode));
    } else {
      ++schedulerStatePreservedCount;
      LogTimingDebug(
          "SVMS: Runtime reload preserved accurate scheduler state mode=%s\n",
          EventTimingModeToConfigString(eventTimingMode));
    }
  }
  if (IsAccurateEventThreadEnabledLocked())
    EnsureAccurateEventThreadLocked();
  else
    StopAccurateEventThreadLocked(true);
}

bool Synth::IsSchedulerEnabledLocked() const {
  return eventTimingMode != EventTimingMode::LEGACY_SYNC;
}

bool Synth::IsStrictAccurateModeLocked() const {
  return eventTimingMode == EventTimingMode::ACCURATE;
}

EventTimingMode Synth::GetEventTimingModeFast() const {
  return (EventTimingMode)eventTimingModeFast.load(std::memory_order_relaxed);
}

bool Synth::IsAccurateEventThreadEnabledLocked() const {
  return eventTimingMode == EventTimingMode::ACCURATE;
}

bool Synth::IsPositiveNoteOn(const MidiEvent &event) {
  return event.type == MidiEvent::NOTE_ON && event.data2 > 0;
}

bool Synth::IsNoteTransitionEvent(const MidiEvent &event) {
  return event.type == MidiEvent::NOTE_OFF ||
         (event.type == MidiEvent::NOTE_ON);
}

bool Synth::IsScheduledTimingEvent(const MidiEvent &event) {
  return event.type == MidiEvent::NOTE_ON || event.type == MidiEvent::NOTE_OFF ||
         event.type == MidiEvent::PROGRAM_CHANGE ||
         event.type == MidiEvent::CONTROL_CHANGE ||
         event.type == MidiEvent::PITCH_BEND || event.type == MidiEvent::RESET;
}

bool Synth::IsReleaseAffectingControlEvent(const MidiEvent &event) {
  if (event.type == MidiEvent::RESET)
    return true;
  if (event.type != MidiEvent::CONTROL_CHANGE)
    return false;
  if (event.data1 == 64)
    return event.data2 < 64;
  return event.data1 == 120 || event.data1 == 121 || event.data1 == 123;
}

bool Synth::IsReleaseLikeEvent(const MidiEvent &event) {
  return event.type == MidiEvent::NOTE_OFF ||
         (event.type == MidiEvent::NOTE_ON && event.data2 <= 0) ||
         IsReleaseAffectingControlEvent(event);
}

int Synth::GetScheduledEventApplyPriority(const MidiEvent &event) {
  if (IsResetLikeEvent(event))
    return 0;
  if (event.type == MidiEvent::CONTROL_CHANGE && event.data1 == 64 &&
      event.data2 < 64)
    return 1;
  if (event.type == MidiEvent::NOTE_OFF ||
      (event.type == MidiEvent::NOTE_ON && event.data2 <= 0))
    return 1;
  if (event.type == MidiEvent::CONTROL_CHANGE || event.type == MidiEvent::PITCH_BEND ||
      event.type == MidiEvent::PROGRAM_CHANGE)
    return 2;
  return 3;
}

void Synth::EnsureScheduledEventIndexCapacityLocked(uint32_t eventId) {
  if (eventId >= heapIndexByEventId.size())
    heapIndexByEventId.resize((size_t)eventId + 1u, 0u);
}

void Synth::RefreshScheduledCacheLocked() const {
  if (!scheduledCacheDirty)
    return;

  bool found = false;
  ULONGLONG oldestTick = 0;
  long long earliestSample = 0;
  for (size_t i = 0; i < scheduledHeap.size(); ++i) {
    const ScheduledTimedEvent &event = scheduledHeap[i];
    if (event.cancelled)
      continue;
    if (!found || event.enqueueTick < oldestTick)
      oldestTick = event.enqueueTick;
    if (!found || event.targetSample < earliestSample)
      earliestSample = event.targetSample;
    found = true;
  }

  scheduledCacheHasValue = found;
  scheduledOldestEnqueueTick = found ? oldestTick : 0;
  scheduledEarliestTargetSample = found ? earliestSample : 0;
  scheduledCacheDirty = false;
#if SVMS_PERF_DEBUG
  ++const_cast<Synth *>(this)->perfSchedulerCacheRebuilds;
#endif
}

void Synth::NoteScheduledEventInsertedLocked(const ScheduledTimedEvent &event) {
  if (event.cancelled)
    return;
  if (!scheduledCacheHasValue) {
    scheduledOldestEnqueueTick = event.enqueueTick;
    scheduledEarliestTargetSample = event.targetSample;
    scheduledCacheHasValue = true;
    return;
  }
  if (scheduledCacheDirty)
    return;
  if (event.enqueueTick < scheduledOldestEnqueueTick)
    scheduledOldestEnqueueTick = event.enqueueTick;
  if (event.targetSample < scheduledEarliestTargetSample)
    scheduledEarliestTargetSample = event.targetSample;
}

void Synth::NoteScheduledEventRemovedLocked(const ScheduledTimedEvent &event) {
  if (event.cancelled || !scheduledCacheHasValue)
    return;
  if (event.enqueueTick == scheduledOldestEnqueueTick ||
      event.targetSample == scheduledEarliestTargetSample)
    scheduledCacheDirty = true;
}

void Synth::NoteScheduledEventMutatedLocked(const ScheduledTimedEvent &oldEvent,
                                            const ScheduledTimedEvent &newEvent) {
  NoteScheduledEventRemovedLocked(oldEvent);
  NoteScheduledEventInsertedLocked(newEvent);
}

void Synth::SwapScheduledHeapNodesLocked(uint32_t lhs, uint32_t rhs) {
  if (lhs == rhs)
    return;
  ScheduledTimedEvent tmp = scheduledHeap[lhs];
  scheduledHeap[lhs] = scheduledHeap[rhs];
  scheduledHeap[rhs] = tmp;
  if (scheduledHeap[lhs].eventId < heapIndexByEventId.size())
    heapIndexByEventId[scheduledHeap[lhs].eventId] = lhs + 1u;
  if (scheduledHeap[rhs].eventId < heapIndexByEventId.size())
    heapIndexByEventId[scheduledHeap[rhs].eventId] = rhs + 1u;
}

bool Synth::ScheduledEventLess(const ScheduledTimedEvent &lhs,
                               const ScheduledTimedEvent &rhs) {
  if (lhs.targetSample != rhs.targetSample)
    return lhs.targetSample < rhs.targetSample;
  if (lhs.applyPriority != rhs.applyPriority)
    return lhs.applyPriority < rhs.applyPriority;
  return lhs.event.sequence < rhs.event.sequence;
}

bool Synth::ScheduledNoteOnTrimLess(const ScheduledNoteOnTrimEntry &lhs,
                                    const ScheduledNoteOnTrimEntry &rhs) {
  if (lhs.velocity != rhs.velocity)
    return lhs.velocity > rhs.velocity;
  if (lhs.targetSample != rhs.targetSample)
    return lhs.targetSample < rhs.targetSample;
  if (lhs.enqueueTick != rhs.enqueueTick)
    return lhs.enqueueTick > rhs.enqueueTick;
  if (lhs.sequence != rhs.sequence)
    return lhs.sequence > rhs.sequence;
  return lhs.eventId > rhs.eventId;
}

void Synth::PushScheduledNoteOnTrimLocked(uint32_t eventId, ULONGLONG enqueueTick,
                                          long long targetSample,
                                          unsigned int sequence,
                                          unsigned int velocity) {
  ScheduledNoteOnTrimEntry entry;
  entry.enqueueTick = enqueueTick;
  entry.targetSample = targetSample;
  entry.sequence = sequence;
  entry.velocity = (unsigned char)(velocity > 127u ? 127u : velocity);
  entry.eventId = eventId;
  scheduledNoteOnTrimHeap.push_back(entry);
  std::push_heap(scheduledNoteOnTrimHeap.begin(), scheduledNoteOnTrimHeap.end(),
                 ScheduledNoteOnTrimLess);
}

bool Synth::PopScheduledNoteOnTrimLocked(uint32_t *eventId) {
  while (!scheduledNoteOnTrimHeap.empty()) {
    std::pop_heap(scheduledNoteOnTrimHeap.begin(), scheduledNoteOnTrimHeap.end(),
                  ScheduledNoteOnTrimLess);
    ScheduledNoteOnTrimEntry entry = scheduledNoteOnTrimHeap.back();
    scheduledNoteOnTrimHeap.pop_back();
    const ScheduledTimedEvent *event = GetScheduledEventLocked(entry.eventId);
    if (event && !event->cancelled && IsPositiveNoteOn(event->event)) {
      if (eventId)
        *eventId = entry.eventId;
      return true;
    }
#if SVMS_PERF_DEBUG
    ++perfSchedulerTrimHeapTombstonePrunes;
#endif
  }
  if (eventId)
    *eventId = 0u;
  return false;
}

void Synth::SiftScheduledHeapUpLocked(uint32_t index) {
  while (index > 0u) {
    uint32_t parent = (index - 1u) >> 1u;
    if (!ScheduledEventLess(scheduledHeap[index], scheduledHeap[parent]))
      break;
    SwapScheduledHeapNodesLocked(index, parent);
    index = parent;
  }
}

void Synth::SiftScheduledHeapDownLocked(uint32_t index) {
  uint32_t size = (uint32_t)scheduledHeap.size();
  while (true) {
    uint32_t left = index * 2u + 1u;
    uint32_t right = left + 1u;
    uint32_t smallest = index;
    if (left < size &&
        ScheduledEventLess(scheduledHeap[left], scheduledHeap[smallest]))
      smallest = left;
    if (right < size &&
        ScheduledEventLess(scheduledHeap[right], scheduledHeap[smallest]))
      smallest = right;
    if (smallest == index)
      break;
    SwapScheduledHeapNodesLocked(index, smallest);
    index = smallest;
  }
}

Synth::ScheduledTimedEvent *Synth::GetScheduledEventLocked(uint32_t eventId) {
  if (eventId == 0u || eventId >= heapIndexByEventId.size())
    return NULL;
  uint32_t encodedIndex = heapIndexByEventId[eventId];
  if (encodedIndex == 0u)
    return NULL;
  uint32_t index = encodedIndex - 1u;
  if (index >= scheduledHeap.size())
    return NULL;
  if (scheduledHeap[index].eventId != eventId)
    return NULL;
  return &scheduledHeap[index];
}

const Synth::ScheduledTimedEvent *
Synth::GetScheduledEventLocked(uint32_t eventId) const {
  if (eventId == 0u || eventId >= heapIndexByEventId.size())
    return NULL;
  uint32_t encodedIndex = heapIndexByEventId[eventId];
  if (encodedIndex == 0u)
    return NULL;
  uint32_t index = encodedIndex - 1u;
  if (index >= scheduledHeap.size())
    return NULL;
  if (scheduledHeap[index].eventId != eventId)
    return NULL;
  return &scheduledHeap[index];
}

uint32_t Synth::PushScheduledEventLocked(const Synth::ScheduledTimedEvent &event) {
  uint32_t index = (uint32_t)scheduledHeap.size();
  EnsureScheduledEventIndexCapacityLocked(event.eventId);
  scheduledHeap.push_back(event);
  heapIndexByEventId[event.eventId] = index + 1u;
  SiftScheduledHeapUpLocked(index);
  if (!event.cancelled) {
    ++scheduledPendingCount;
    NoteScheduledEventInsertedLocked(event);
    if (IsPositiveNoteOn(event.event))
      PushScheduledNoteOnTrimLocked(event.eventId, event.enqueueTick,
                                    event.targetSample, event.event.sequence,
                                    (unsigned int)event.event.data2);
  }
  return event.eventId;
}

void Synth::CancelScheduledEventLocked(uint32_t eventId) {
  ScheduledTimedEvent *event = GetScheduledEventLocked(eventId);
  if (!event || event->cancelled)
    return;
  NoteScheduledEventRemovedLocked(*event);
  event->cancelled = true;
  if (scheduledPendingCount > 0)
    --scheduledPendingCount;
  if (scheduledPendingCount == 0) {
    scheduledCacheHasValue = false;
    scheduledCacheDirty = false;
    scheduledOldestEnqueueTick = 0;
    scheduledEarliestTargetSample = 0;
  }
}

bool Synth::PopScheduledEventLocked(Synth::ScheduledTimedEvent &event) {
  PruneScheduledHeapTopLocked();
  if (scheduledHeap.empty())
    return false;

  event = scheduledHeap.front();
  if (event.eventId < heapIndexByEventId.size())
    heapIndexByEventId[event.eventId] = 0u;

  if (scheduledHeap.size() == 1u) {
    scheduledHeap.pop_back();
  } else {
    scheduledHeap[0] = scheduledHeap.back();
    scheduledHeap.pop_back();
    if (scheduledHeap[0].eventId < heapIndexByEventId.size())
      heapIndexByEventId[scheduledHeap[0].eventId] = 1u;
    SiftScheduledHeapDownLocked(0u);
  }

  if (!event.cancelled && scheduledPendingCount > 0) {
    --scheduledPendingCount;
    NoteScheduledEventRemovedLocked(event);
    if (scheduledPendingCount == 0) {
      scheduledCacheHasValue = false;
      scheduledCacheDirty = false;
      scheduledOldestEnqueueTick = 0;
      scheduledEarliestTargetSample = 0;
    }
  }
  return true;
}

void Synth::PruneScheduledHeapTopLocked() {
  while (!scheduledHeap.empty() && scheduledHeap.front().cancelled) {
    uint32_t tombstoneId = scheduledHeap.front().eventId;
    if (tombstoneId < heapIndexByEventId.size())
      heapIndexByEventId[tombstoneId] = 0u;
    if (scheduledHeap.size() == 1u) {
      scheduledHeap.pop_back();
    } else {
      scheduledHeap[0] = scheduledHeap.back();
      scheduledHeap.pop_back();
      if (scheduledHeap[0].eventId < heapIndexByEventId.size())
        heapIndexByEventId[scheduledHeap[0].eventId] = 1u;
      SiftScheduledHeapDownLocked(0u);
    }
  }
}

void Synth::CompactPendingTransitionQueueLocked(int channel, int note) {
  if (channel < 0 || channel >= 16 || note < 0 || note >= 128)
    return;

  PendingTransitionQueue &queue = pendingTransition[channel][note];
  unsigned int writeIndex = 0;
  for (unsigned int i = 0; i < queue.count; ++i) {
    const uint32_t eventId = queue.eventIds[i];
    const ScheduledTimedEvent *event = GetScheduledEventLocked(eventId);
    if (!event || event->cancelled)
      continue;
    queue.eventIds[writeIndex++] = eventId;
  }
  const unsigned int liveCount = writeIndex;
  while (writeIndex < kPendingTransitionsPerKey)
    queue.eventIds[writeIndex++] = 0u;
  queue.count = (unsigned char)((liveCount > kPendingTransitionsPerKey)
                                    ? kPendingTransitionsPerKey
                                    : liveCount);
}

bool Synth::RemovePendingTransitionEventLocked(int channel, int note,
                                               uint32_t eventId) {
  if (channel < 0 || channel >= 16 || note < 0 || note >= 128 || eventId == 0u)
    return false;

  PendingTransitionQueue &queue = pendingTransition[channel][note];
  for (unsigned int i = 0; i < queue.count; ++i) {
    if (queue.eventIds[i] != eventId)
      continue;
    for (unsigned int j = i + 1; j < queue.count; ++j)
      queue.eventIds[j - 1] = queue.eventIds[j];
    if (queue.count > 0)
      --queue.count;
    queue.eventIds[queue.count] = 0u;
    return true;
  }
  return false;
}

int Synth::FindQueuedSameStateTransitionLocked(int channel, int note,
                                               int stateIndex,
                                               long long targetSample) const {
  if (channel < 0 || channel >= 16 || note < 0 || note >= 128 ||
      stateIndex < 0 || stateIndex > 1)
    return -1;

  const PendingTransitionQueue &queue = pendingTransition[channel][note];
  for (int i = (int)queue.count - 1; i >= 0; --i) {
    const ScheduledTimedEvent *event = GetScheduledEventLocked(queue.eventIds[i]);
    if (!event || event->cancelled)
      continue;
    const int queuedStateIndex =
        (event->kind == (uint8_t)MidiEvent::NOTE_ON && event->event.data2 > 0)
            ? 1
            : 0;
    if (queuedStateIndex == stateIndex && event->targetSample == targetSample)
      return i;
  }
  return -1;
}

int Synth::FindQueuedOppositeStateSameSampleLocked(int channel, int note,
                                                   int stateIndex,
                                                   long long targetSample) const {
  if (channel < 0 || channel >= 16 || note < 0 || note >= 128 ||
      stateIndex < 0 || stateIndex > 1)
    return -1;

  const PendingTransitionQueue &queue = pendingTransition[channel][note];
  const int oppositeStateIndex = stateIndex ? 0 : 1;
  for (int i = (int)queue.count - 1; i >= 0; --i) {
    const ScheduledTimedEvent *event = GetScheduledEventLocked(queue.eventIds[i]);
    if (!event || event->cancelled)
      continue;
    const int queuedStateIndex =
        (event->kind == (uint8_t)MidiEvent::NOTE_ON && event->event.data2 > 0)
            ? 1
            : 0;
    if (queuedStateIndex == oppositeStateIndex &&
        event->targetSample == targetSample)
      return i;
  }
  return -1;
}

bool Synth::AppendPendingTransitionEventLocked(int channel, int note,
                                               const ScheduledTimedEvent &event,
                                               bool hardOverload,
                                               bool &droppedNewEvent) {
  droppedNewEvent = false;
  if (channel < 0 || channel >= 16 || note < 0 || note >= 128)
    return false;

  CompactPendingTransitionQueueLocked(channel, note);
  PendingTransitionQueue &queue = pendingTransition[channel][note];
  if (queue.count < kPendingTransitionsPerKey) {
    queue.eventIds[queue.count++] = event.eventId;
    return true;
  }

  const bool newIsPositiveNoteOn = IsPositiveNoteOn(event.event);
  int evictionIndex = -1;
  const ScheduledTimedEvent *evictionEvent = NULL;
  for (unsigned int i = 0; i < queue.count; ++i) {
    const ScheduledTimedEvent *queued = GetScheduledEventLocked(queue.eventIds[i]);
    if (!queued || queued->cancelled || !IsPositiveNoteOn(queued->event))
      continue;
    if (!evictionEvent ||
        queued->event.data2 < evictionEvent->event.data2 ||
        (queued->event.data2 == evictionEvent->event.data2 &&
         queued->targetSample > evictionEvent->targetSample) ||
        (queued->event.data2 == evictionEvent->event.data2 &&
         queued->targetSample == evictionEvent->targetSample &&
         queued->event.sequence < evictionEvent->event.sequence)) {
      evictionIndex = (int)i;
      evictionEvent = queued;
    }
  }

  if (evictionIndex == -1) {
    for (unsigned int i = 0; i < queue.count; ++i) {
      const ScheduledTimedEvent *queued = GetScheduledEventLocked(queue.eventIds[i]);
      if (!queued || queued->cancelled)
        continue;
      if (!evictionEvent ||
          queued->targetSample > evictionEvent->targetSample ||
          (queued->targetSample == evictionEvent->targetSample &&
           queued->event.sequence < evictionEvent->event.sequence)) {
        evictionIndex = (int)i;
        evictionEvent = queued;
      }
    }
  }

  if (evictionIndex == -1) {
    droppedNewEvent = true;
    return false;
  }

  if (newIsPositiveNoteOn && !hardOverload && evictionEvent &&
      IsPositiveNoteOn(evictionEvent->event)) {
    const bool newEventIsWeaker =
        event.event.data2 < evictionEvent->event.data2 ||
        (event.event.data2 == evictionEvent->event.data2 &&
         event.targetSample > evictionEvent->targetSample) ||
        (event.event.data2 == evictionEvent->event.data2 &&
         event.targetSample == evictionEvent->targetSample &&
         event.event.sequence >= evictionEvent->event.sequence);
    if (newEventIsWeaker) {
      droppedNewEvent = true;
      return false;
    }
  }

  if (evictionEvent) {
    CancelScheduledEventLocked(evictionEvent->eventId);
    RemovePendingTransitionEventLocked(channel, note, evictionEvent->eventId);
  }
  if (queue.count < kPendingTransitionsPerKey)
    queue.eventIds[queue.count++] = event.eventId;
  return true;
}

void Synth::UpdateScheduledKeyStateFromRefsLocked(int channel, int note) {
  if (channel < 0 || channel >= 16 || note < 0 || note >= 128)
    return;

  CompactPendingTransitionQueueLocked(channel, note);
  ScheduledKeyState &state = scheduledKeyStates[channel][note];
  state.pendingNoteOnCount = 0;
  state.pendingNoteOffCount = 0;
  state.lastScheduledState = state.lastAppliedState;

  const PendingTransitionQueue &queue = pendingTransition[channel][note];
  for (unsigned int i = 0; i < queue.count; ++i) {
    const ScheduledTimedEvent *event = GetScheduledEventLocked(queue.eventIds[i]);
    if (!event || event->cancelled)
      continue;
    if (event->kind == (uint8_t)MidiEvent::NOTE_ON && event->event.data2 > 0) {
      ++state.pendingNoteOnCount;
      state.lastScheduledState = true;
    } else {
      ++state.pendingNoteOffCount;
      state.lastScheduledState = false;
    }
  }
}

void Synth::ResetScheduledAppliedStateLocked() {
  for (int channel = 0; channel < 16; ++channel) {
    for (int note = 0; note < 128; ++note) {
      scheduledKeyStates[channel][note].soundingGenerations = 0;
      scheduledKeyStates[channel][note].lastAppliedState = false;
      UpdateScheduledKeyStateFromRefsLocked(channel, note);
    }
  }
}

void Synth::ResetScheduledStateLocked() {
  nextEventSequence.store(1, std::memory_order_relaxed);
  for (int channel = 0; channel < 16; ++channel) {
    for (int note = 0; note < 128; ++note) {
      scheduledKeyStates[channel][note] = ScheduledKeyState();
      pendingTransition[channel][note] = PendingTransitionQueue();
    }
  }
  scheduledHeap.clear();
  scheduledNoteOnTrimHeap.clear();
  scheduledPendingCount = 0;
  heapIndexByEventId.clear();
  scheduledCacheDirty = false;
  scheduledCacheHasValue = false;
  scheduledOldestEnqueueTick = 0;
  scheduledEarliestTargetSample = 0;
  schedulerAnchorSample.store(0, std::memory_order_relaxed);
  schedulerAnchorQpc.store(0, std::memory_order_relaxed);
  schedulerAnchorSampleRate.store(0, std::memory_order_relaxed);
  currentRenderBlockStartSample = 0;
  currentRenderBlockStartQpc = 0;
  currentRenderBlockEndQpc = 0;
  currentRenderSampleRate = 0;
  currentBlockQuantized = false;
  lastSchedulerBlockFrames = 0;
  lastSchedulerBlockMs = 0.0f;
  accurateEventClockBaseSample.store(0, std::memory_order_relaxed);
  accurateEventClockBaseQpc.store(0, std::memory_order_relaxed);
  accurateEventClockSampleRate.store(0, std::memory_order_relaxed);
  ++accurateClockResetCount;
  ResetEventRing(realtimeIngressRing);
  ResetEventRing(accurateIngressRing);
  ResetFlatMidiQueue(deferredCriticalEvents);
  ResetFlatMidiQueue(deferredRealtimeEvents);
  ResetFlatMidiQueue(deferredNoteOnEvents);
  ResetFlatMidiQueue(overloadReleaseEvents);
  ResetFlatMidiQueue(accuratePendingEvents);
  realtimeIngressDepth.store(0, std::memory_order_relaxed);
  accurateIngressDepthAtomic.store(0, std::memory_order_relaxed);
  renderProgressSample.store(0, std::memory_order_relaxed);
  renderProgressEndSample.store(0, std::memory_order_relaxed);
  consecutiveAccurateBlockStartApplies = 0;
  schedulerWarmupBlocks = kSchedulerWarmupBlockCount;
}

void Synth::FlushScheduledEventsLocked() {
  for (int channel = 0; channel < 16; ++channel) {
    for (int note = 0; note < 128; ++note) {
      scheduledKeyStates[channel][note].pendingNoteOnCount = 0;
      scheduledKeyStates[channel][note].pendingNoteOffCount = 0;
      scheduledKeyStates[channel][note].lastScheduledState =
          scheduledKeyStates[channel][note].lastAppliedState;
      pendingTransition[channel][note] = PendingTransitionQueue();
    }
  }
  scheduledHeap.clear();
  scheduledNoteOnTrimHeap.clear();
  scheduledPendingCount = 0;
  heapIndexByEventId.clear();
  scheduledCacheDirty = false;
  scheduledCacheHasValue = false;
  scheduledOldestEnqueueTick = 0;
  scheduledEarliestTargetSample = 0;
}

void Synth::CollectPendingScheduledEventsLocked(std::vector<MidiEvent> &events) {
  events.clear();

  auto collectFromVector = [&](std::vector<MidiEvent> &source) {
    std::vector<MidiEvent> retained;
    retained.reserve(source.size());
    for (std::vector<MidiEvent>::const_iterator it = source.begin();
         it != source.end(); ++it) {
      if (IsScheduledTimingEvent(*it))
        InsertMidiEventBySequence(events, *it);
      else
        retained.push_back(*it);
    }
    source.swap(retained);
  };

  auto collectFromFlatQueue = [&](FlatMidiQueue &source) {
    uint32_t originalCount = source.size();
    for (uint32_t i = 0; i < originalCount; ++i) {
      MidiEvent event = source.front();
      source.pop_front();
      if (IsScheduledTimingEvent(event))
        InsertMidiEventBySequence(events, event);
      else
        source.push_back(event);
    }
  };

  collectFromFlatQueue(deferredCriticalEvents);
  collectFromFlatQueue(deferredRealtimeEvents);
  collectFromFlatQueue(deferredNoteOnEvents);
  collectFromVector(incomingCriticalEvents);
  collectFromVector(incomingRealtimeEvents);
  collectFromVector(incomingNoteOnEvents);
}

void Synth::EnqueuePendingEventLocked(const MidiEvent &event) {
  int priority = GetEventPriority(event);
  if (priority == 0 || IsCriticalControlEvent(event))
    pendingCriticalEvents.push_back(event);
  else if (priority == 1)
    pendingRealtimeEvents.push_back(event);
  else
    pendingNoteOnEvents.push_back(event);
}

unsigned int Synth::GetIngressDepthLocked() const {
  return static_cast<unsigned int>(
      pendingCriticalEvents.size() + pendingRealtimeEvents.size() +
      pendingNoteOnEvents.size() + deferredCriticalEvents.size() +
      deferredRealtimeEvents.size() + deferredNoteOnEvents.size() +
      accuratePendingEvents.size() + overloadReleaseEvents.size()) +
         realtimeIngressDepth.load(std::memory_order_relaxed) +
         accurateIngressDepthAtomic.load(std::memory_order_relaxed);
}

void Synth::EnqueueReleaseLaneEventLocked(const MidiEvent &event) {
  if (!overloadReleaseEvents.empty()) {
    MidiEvent &tail = overloadReleaseEvents.back();
    bool sameController =
        event.type == MidiEvent::CONTROL_CHANGE &&
        tail.type == MidiEvent::CONTROL_CHANGE && tail.channel == event.channel &&
        tail.data1 == event.data1;
    bool samePitch = event.type == MidiEvent::PITCH_BEND &&
                     tail.type == MidiEvent::PITCH_BEND &&
                     tail.channel == event.channel;
    bool sameProgram = event.type == MidiEvent::PROGRAM_CHANGE &&
                       tail.type == MidiEvent::PROGRAM_CHANGE &&
                       tail.channel == event.channel;
    if (sameController || samePitch || sameProgram) {
      tail = event;
      ++accurateControlsCoalesced;
      return;
    }
  }
  if (overloadReleaseEvents.size() >= kReleaseLaneMaxEvents)
    overloadReleaseEvents.pop_front();
  overloadReleaseEvents.push_back(event);
}

unsigned int Synth::PopReleaseLaneEventsLocked(std::vector<MidiEvent> &events,
                                               unsigned int maxCount) {
  events.clear();
  while (!overloadReleaseEvents.empty() && events.size() < maxCount) {
    events.push_back(overloadReleaseEvents.front());
    overloadReleaseEvents.pop_front();
  }
  return static_cast<unsigned int>(events.size());
}

unsigned int Synth::ComputeSameKeyPendingTransitionCountLocked() const {
  unsigned int total = 0;
  for (int channel = 0; channel < 16; ++channel) {
    for (int note = 0; note < 128; ++note) {
      total += pendingTransition[channel][note].count;
    }
  }
  return total;
}

unsigned int Synth::ComputeMaxSameKeyQueueDepthLocked() const {
  unsigned int maxDepth = 0;
  for (int channel = 0; channel < 16; ++channel) {
    for (int note = 0; note < 128; ++note) {
      unsigned int depth = pendingTransition[channel][note].count;
      if (depth > maxDepth)
        maxDepth = depth;
    }
  }
  return maxDepth;
}

int Synth::GetQuantizeGridFramesLocked() const {
  if (lastSchedulerBlockFrames > 0)
    return static_cast<int>(lastSchedulerBlockFrames);
  return 512;
}

long long Synth::QuantizeTargetSampleLocked(long long sample) const {
  if (eventTimingMode != EventTimingMode::QUANTIZED)
    return sample;
  const int grid = GetQuantizeGridFramesLocked();
  if (grid <= 1)
    return sample;
  if (sample < 0)
    sample = 0;
  long long remainder = sample % grid;
  if (remainder == 0)
    return sample;
  return sample + (grid - remainder);
}

void Synth::InsertScheduledTimedEventLocked(const MidiEvent &event,
                                            bool hardOverload) {
  const bool noteOn = IsPositiveNoteOn(event);
  const bool noteTransition = IsNoteTransitionEvent(event);
  const bool strictAccurate = IsStrictAccurateModeLocked();
  long long earliestRenderableSample = strictAccurate
                                           ? renderProgressSample.load(
                                                 std::memory_order_relaxed)
                                           : (long long)currentRenderBlockStartSample;
  if (earliestRenderableSample < 0)
    earliestRenderableSample = 0;
  long long targetSample = event.targetSample;
  if (targetSample <= 0) {
    if (!strictAccurate && event.arrivalQpc != 0 && currentRenderBlockStartQpc != 0 &&
        currentRenderSampleRate > 0) {
      LARGE_INTEGER freq = GetPerfFrequency();
      targetSample =
          (long long)currentRenderBlockStartSample +
          GetElapsedSamples(event.arrivalQpc - currentRenderBlockStartQpc,
                            currentRenderSampleRate, freq);
    } else {
      targetSample = earliestRenderableSample;
    }
  }
  if (eventTimingMode == EventTimingMode::QUANTIZED)
    targetSample = QuantizeTargetSampleLocked(targetSample);

  if (hardOverload && consecutiveAccurateBlockStartApplies >= 16 &&
      !IsReleaseLikeEvent(event) && !IsCriticalControlEvent(event)) {
    long long nextBlockSample =
        earliestRenderableSample +
        (lastSchedulerBlockFrames > 0 ? (long long)lastSchedulerBlockFrames : 64ll);
    if (targetSample < nextBlockSample) {
      targetSample = nextBlockSample;
      ++lastCatchupPrevented;
    }
  }

  if (targetSample < earliestRenderableSample) {
    targetSample = earliestRenderableSample;
    ++lastSchedulerLateEvents;
  }

  if (noteOn)
    ++lastNoteOnsAttempted;

  if (hardOverload && noteOn &&
      event.data2 <=
          (runtimeSettings.velocityIgnoreBelow > kHardOverloadVelocityFloor
               ? runtimeSettings.velocityIgnoreBelow
               : kHardOverloadVelocityFloor)) {
    ++lastAsyncDropped;
    ++lastNoteOnsDropped;
    droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  ScheduledTimedEvent timedEvent;
  timedEvent.event = event;
  timedEvent.targetSample = targetSample;
  timedEvent.enqueueTick = compat::GetTickCount64Compat();
  timedEvent.eventId = event.sequence;
  timedEvent.applyPriority = (uint8_t)GetScheduledEventApplyPriority(event);
  timedEvent.kind = (uint8_t)event.type;
  timedEvent.channel =
      (uint8_t)((event.channel >= 0 && event.channel < 16) ? event.channel : 0xFF);
  timedEvent.note =
      (uint8_t)((event.data1 >= 0 && event.data1 < 128) ? event.data1 : 0xFF);
  timedEvent.cancelled = false;

  if (noteOn) {
    const unsigned int scheduledSoftPendingCap =
        GetAdaptiveScheduledSoftPendingCap(configuredMaxVoices);
    const unsigned int scheduledHardPendingCap =
        GetAdaptiveScheduledHardPendingCap(configuredMaxVoices);
    unsigned int scheduledPending = ComputeScheduledPendingCountLocked();
    if (scheduledPending >= scheduledSoftPendingCap) {
      unsigned int trimTarget = scheduledSoftPendingCap;
      if (trimTarget > kScheduledTrimHeadroom)
        trimTarget -= kScheduledTrimHeadroom;
      if (scheduledPending >= scheduledHardPendingCap &&
          trimTarget > kScheduledTrimHeadroom)
        trimTarget -= kScheduledTrimHeadroom;
      unsigned int trimCount =
          scheduledPending >= trimTarget ? (scheduledPending - trimTarget + 1) : 0;
      if (trimCount > 0) {
        int trimVelocityFloor =
            (hardOverload || scheduledPending >= scheduledHardPendingCap)
                ? -1
                : kSoftScheduledTrimVelocityFloor;
        unsigned int removed =
            DropScheduledPendingNoteOnsLocked(trimCount, trimVelocityFloor);
        if (removed > 0) {
          droppedNoteOnEvents.fetch_add(removed, std::memory_order_relaxed);
          lastAsyncDropped += removed;
          lastNoteOnsDropped += removed;
          lastOverloadNoteOnsDropped += removed;
          lastPostScheduleDrops += removed;
          lastCatchupPrevented += removed;
          scheduledPending -= removed;
          if (removed >= 16 || scheduledPending >= scheduledHardPendingCap) {
            LogTimingDebug(
                "SVMS: Scheduled shedding removed %u note-ons pending=%u floor=%d hard=%u\n",
                removed, scheduledPending, trimVelocityFloor,
                hardOverload ? 1u : 0u);
          }
        }
      }
      if (scheduledPending >= scheduledHardPendingCap ||
          (hardOverload && scheduledPending >= scheduledSoftPendingCap)) {
        ++lastAsyncDropped;
        ++lastNoteOnsDropped;
        ++lastOverloadNoteOnsDropped;
        ++lastPostScheduleDrops;
        ++lastCatchupPrevented;
        droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }

  if (noteTransition) {
    const int channel = event.channel;
    const int note = event.data1;
    if (channel < 0 || channel >= 16 || note < 0 || note >= 128)
      return;
    ScheduledKeyState &state = scheduledKeyStates[channel][note];
    const int stateIndex = noteOn ? 1 : 0;
    CompactPendingTransitionQueueLocked(channel, note);

    const int sameQueueIndex =
        FindQueuedSameStateTransitionLocked(channel, note, stateIndex,
                                           targetSample);
    if (sameQueueIndex >= 0) {
      PendingTransitionQueue &queue = pendingTransition[channel][note];
      ScheduledTimedEvent *sameEvent =
          GetScheduledEventLocked(queue.eventIds[sameQueueIndex]);
      if (sameEvent && !sameEvent->cancelled) {
        ScheduledTimedEvent oldEvent = *sameEvent;
        uint32_t heapIndex = heapIndexByEventId[sameEvent->eventId] - 1u;
        sameEvent->event = event;
        sameEvent->targetSample = targetSample;
        sameEvent->enqueueTick = timedEvent.enqueueTick;
        sameEvent->applyPriority = timedEvent.applyPriority;
        sameEvent->kind = timedEvent.kind;
        sameEvent->channel = timedEvent.channel;
        sameEvent->note = timedEvent.note;
        NoteScheduledEventMutatedLocked(oldEvent, *sameEvent);
        if (IsPositiveNoteOn(sameEvent->event))
          PushScheduledNoteOnTrimLocked(sameEvent->eventId, sameEvent->enqueueTick,
                                        sameEvent->targetSample,
                                        sameEvent->event.sequence,
                                        (unsigned int)sameEvent->event.data2);
        SiftScheduledHeapUpLocked(heapIndex);
        heapIndex = heapIndexByEventId[sameEvent->eventId] - 1u;
        SiftScheduledHeapDownLocked(heapIndex);
        ++lastAsyncCoalesced;
        if (noteOn)
          ++lastSchedulerNoteOnsCoalesced;
        else
          ++lastSchedulerNoteOffsCoalesced;
        UpdateScheduledKeyStateFromRefsLocked(channel, note);
        return;
      }
    }

    if (state.soundingGenerations == 0) {
      const int oppositeSameSampleIndex =
          FindQueuedOppositeStateSameSampleLocked(channel, note, stateIndex,
                                                 targetSample);
      if (oppositeSameSampleIndex >= 0) {
        PendingTransitionQueue &queue = pendingTransition[channel][note];
        ScheduledTimedEvent *oppositeEvent =
            GetScheduledEventLocked(queue.eventIds[oppositeSameSampleIndex]);
        if (oppositeEvent && !oppositeEvent->cancelled) {
          CancelScheduledEventLocked(oppositeEvent->eventId);
          RemovePendingTransitionEventLocked(channel, note,
                                            oppositeEvent->eventId);
          ++lastAsyncCoalesced;
          if (noteOn)
            ++lastSchedulerNoteOnsCoalesced;
          else
            ++lastSchedulerNoteOffsCanceled;
          if (noteOn) {
            UpdateScheduledKeyStateFromRefsLocked(channel, note);
          } else {
            UpdateScheduledKeyStateFromRefsLocked(channel, note);
            return;
          }
        }
      }
    }

    if (ComputeScheduledPendingCountLocked() >= kMaxScheduledNoteTransitions &&
        noteOn) {
      ++lastAsyncDropped;
      ++lastNoteOnsDropped;
      droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    {
      bool droppedNewEvent = false;
      if (!AppendPendingTransitionEventLocked(channel, note, timedEvent,
                                              hardOverload, droppedNewEvent)) {
        if (droppedNewEvent && noteOn) {
          ++lastAsyncDropped;
          ++lastNoteOnsDropped;
          ++lastOverloadNoteOnsDropped;
          ++lastPostScheduleDrops;
          ++lastCatchupPrevented;
          droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
        }
        UpdateScheduledKeyStateFromRefsLocked(channel, note);
        return;
      }
    }
    PushScheduledEventLocked(timedEvent);
    UpdateScheduledKeyStateFromRefsLocked(channel, note);
  } else {
    PushScheduledEventLocked(timedEvent);
  }

  UpdateObservedMax(maxAsyncQueueDepth, ComputeScheduledPendingCountLocked());
  unsigned int sameKeyQueueDepth = ComputeMaxSameKeyQueueDepthLocked();
  if (sameKeyQueueDepth > lastSchedulerMaxSameKeyQueueDepth)
    lastSchedulerMaxSameKeyQueueDepth = sameKeyQueueDepth;
}

void Synth::ScheduleEventsLocked(const std::vector<MidiEvent> &events,
                                 bool hardOverload) {
  for (std::vector<MidiEvent>::const_iterator it = events.begin();
       it != events.end(); ++it) {
    InsertScheduledTimedEventLocked(*it, hardOverload);
  }
}

unsigned int Synth::ComputeScheduledQueueAgeMsLocked() const {
  RefreshScheduledCacheLocked();
  if (!scheduledCacheHasValue)
    return 0;
  return static_cast<unsigned int>(compat::GetTickCount64Compat() -
                                   scheduledOldestEnqueueTick);
}

unsigned int Synth::ComputeScheduledLagSamplesLocked() const {
  RefreshScheduledCacheLocked();
  if (!scheduledCacheHasValue)
    return 0;
  if (scheduledEarliestTargetSample >= (long long)currentRenderBlockStartSample)
    return 0;
  return static_cast<unsigned int>((long long)currentRenderBlockStartSample -
                                   scheduledEarliestTargetSample);
}

unsigned int Synth::ComputeScheduledLagStateLocked() const {
  const unsigned int softDeferredThreshold =
      GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
  const unsigned int hardDeferredThreshold =
      GetAdaptiveHardDeferredThreshold(configuredMaxVoices);
  unsigned int pending = ComputeScheduledPendingCountLocked();
  unsigned int ageMs = ComputeScheduledQueueAgeMsLocked();
  unsigned int lagSamples = ComputeScheduledLagSamplesLocked();
  const bool cadencePressureActive =
      lastSchedulerLateEvents > 0 ||
      lagSamples > (unsigned int)(kAccurateCoalesceWindowSamples * 2u);
  if (cadencePressureActive && consecutiveAccurateBlockStartApplies >= 12)
    return 2;
  if (cadencePressureActive && consecutiveAccurateBlockStartApplies >= 4)
    return 1;
  if (pending >= hardDeferredThreshold || ageMs >= 250 ||
      lagSamples >= (unsigned int)(GetQuantizeGridFramesLocked() * 2))
    return 2;
  if (pending >= softDeferredThreshold || ageMs >= 100 ||
      lagSamples >= (unsigned int)GetQuantizeGridFramesLocked())
    return 1;
  return 0;
}

unsigned int Synth::ComputeScheduledPendingCountLocked() const {
  return scheduledPendingCount;
}

bool Synth::DrainScheduledWindowLocked(long long cursorSample,
                                       long long blockEndSample,
                                       long long windowEndSample,
                                       std::vector<MidiEvent> &dueEvents,
                                       long long &renderUntilSample) {
  dueEvents.clear();
  renderUntilSample = blockEndSample;
  PruneScheduledHeapTopLocked();
  if (scheduledHeap.empty())
    return false;

  if (scheduledHeap.front().targetSample >= blockEndSample) {
    renderUntilSample = blockEndSample;
    return false;
  }

  if (scheduledHeap.front().targetSample >= windowEndSample) {
    renderUntilSample = scheduledHeap.front().targetSample;
    return false;
  }

  while (true) {
    ScheduledTimedEvent event;
    if (!PopScheduledEventLocked(event))
      break;
    if (event.cancelled)
      continue;
    if (event.targetSample >= windowEndSample || event.targetSample >= blockEndSample) {
      PushScheduledEventLocked(event);
      break;
    }
    dueEvents.push_back(event.event);
    if (event.kind == (uint8_t)MidiEvent::NOTE_OFF ||
        event.kind == (uint8_t)MidiEvent::NOTE_ON) {
      const int channel = event.channel;
      const int note = event.note;
      if (channel >= 0 && channel < 16 && note >= 0 && note < 128) {
        RemovePendingTransitionEventLocked(channel, note, event.eventId);
        UpdateScheduledKeyStateFromRefsLocked(channel, note);
      }
    }
    renderUntilSample = event.targetSample;
    PruneScheduledHeapTopLocked();
    if (scheduledHeap.empty())
      break;
    if (scheduledHeap.front().targetSample >= windowEndSample ||
        scheduledHeap.front().targetSample >= blockEndSample)
      break;
  }
  return !dueEvents.empty();
}

void Synth::FilterDueEventsForOverloadLocked(std::vector<MidiEvent> &events,
                                             long long currentSample,
                                             unsigned int &droppedCount) {
  droppedCount = 0;
  unsigned int effectiveOverloadState = overloadState;
  const unsigned int pending = ComputeScheduledPendingCountLocked();
  const unsigned int queueAgeMs = ComputeScheduledQueueAgeMsLocked();
  const unsigned int softDeferredThreshold =
      GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
  const unsigned int hardDeferredThreshold =
      GetAdaptiveHardDeferredThreshold(configuredMaxVoices);
  const bool cadencePressureActive =
      lastSchedulerLateEvents > 0 ||
      lastSchedulerLagSamples >
          (unsigned int)(kAccurateCoalesceWindowSamples * 2u);
  const bool trueLatePressure =
      lastSchedulerLateEvents > 0 ||
      lastSchedulerLagSamples >
          (unsigned int)(kAccurateCoalesceWindowSamples * 2u) ||
      queueAgeMs >= 100u || pending >= hardDeferredThreshold;
  const bool warmupGraceActive =
      schedulerWarmupBlocks > 0 && !cadencePressureActive &&
      lastSchedulerLagSamples == 0 && lastAsyncQueueAgeMs < 8u;
  if (warmupGraceActive || !trueLatePressure)
    return;
  if (cadencePressureActive && consecutiveAccurateBlockStartApplies >= 12 &&
      effectiveOverloadState < 2) {
    effectiveOverloadState = 2;
  } else if (cadencePressureActive &&
             consecutiveAccurateBlockStartApplies >= 4 &&
             effectiveOverloadState < 1) {
    effectiveOverloadState = 1;
  }
  if (effectiveOverloadState == 1 && pending < softDeferredThreshold &&
      queueAgeMs < 100u &&
      lastSchedulerLagSamples <=
          (unsigned int)(kAccurateCoalesceWindowSamples * 2u)) {
    return;
  }
  if (effectiveOverloadState == 0)
    return;

  const unsigned int applyLimit =
      effectiveOverloadState >= 2
          ? GetAdaptiveHardDueApplyLimit(configuredMaxVoices)
          : GetAdaptiveSoftDueApplyLimit(configuredMaxVoices);
  std::vector<MidiEvent> kept;
  std::vector<unsigned char> keepFlags(events.size(), 0u);
  std::vector<size_t> onTimeStrong;
  std::vector<size_t> onTimeQuiet;
  std::vector<size_t> overdueStrong;
  std::vector<size_t> overdueQuiet;
  kept.reserve(events.size());
  for (size_t i = 0; i < events.size(); ++i) {
    const MidiEvent &event = events[i];
    if (!IsPositiveNoteOn(event)) {
      keepFlags[i] = 1u;
      kept.push_back(event);
    } else if (event.targetSample < currentSample) {
      if (event.data2 <= kSoftScheduledTrimVelocityFloor)
        overdueQuiet.push_back(i);
      else
        overdueStrong.push_back(i);
    } else {
      if (event.data2 <= kSoftScheduledTrimVelocityFloor)
        onTimeQuiet.push_back(i);
      else
        onTimeStrong.push_back(i);
    }
  }

  unsigned int noteOnBudget =
      kept.size() >= applyLimit ? 0u : (applyLimit - (unsigned int)kept.size());
  if (effectiveOverloadState >= 2) {
    const unsigned int onTimeBudget =
        GetAdaptiveHardDueOnTimeBudget(configuredMaxVoices);
    if (noteOnBudget > onTimeBudget)
      noteOnBudget = onTimeBudget;
  } else {
    const unsigned int onTimeBudget =
        GetAdaptiveSoftDueOnTimeBudget(configuredMaxVoices);
    if (noteOnBudget > onTimeBudget)
      noteOnBudget = onTimeBudget;
  }

  unsigned int overdueBudget =
      effectiveOverloadState >= 2
          ? GetAdaptiveHardDueOverdueBudget(configuredMaxVoices)
          : GetAdaptiveSoftDueOverdueBudget(configuredMaxVoices);
  if (lastSchedulerLagSamples == 0)
    overdueBudget = noteOnBudget;
  else if (overdueBudget > noteOnBudget)
    overdueBudget = noteOnBudget;

  if (cadencePressureActive &&
      currentSample == (long long)currentRenderBlockStartSample &&
      consecutiveAccurateBlockStartApplies >= 12) {
    if (noteOnBudget > 8u)
      noteOnBudget = 8u;
    overdueBudget = 0u;
  } else if (cadencePressureActive &&
             currentSample == (long long)currentRenderBlockStartSample &&
             consecutiveAccurateBlockStartApplies >= 4) {
    if (noteOnBudget > 16u)
      noteOnBudget = 16u;
    if (overdueBudget > 4u)
      overdueBudget = 4u;
  }

  const std::vector<size_t> *orderedBuckets[] = {
      &onTimeStrong, &onTimeQuiet, &overdueStrong, &overdueQuiet};
  for (size_t bucketIndex = 0;
       bucketIndex < sizeof(orderedBuckets) / sizeof(orderedBuckets[0]);
       ++bucketIndex) {
    const bool overdueBucket = bucketIndex >= 2;
    const std::vector<size_t> &bucket = *orderedBuckets[bucketIndex];
    for (size_t i = 0; i < bucket.size(); ++i) {
      const size_t eventIndex = bucket[i];
      if (noteOnBudget == 0u) {
        ++droppedCount;
        continue;
      }
      if (overdueBucket && overdueBudget == 0u) {
        ++droppedCount;
        continue;
      }
      keepFlags[eventIndex] = 1u;
      --noteOnBudget;
      if (overdueBucket && overdueBudget > 0u)
        --overdueBudget;
    }
  }

  if (droppedCount == 0)
    return;

  kept.clear();
  for (size_t i = 0; i < events.size(); ++i) {
    if (keepFlags[i] != 0u)
      kept.push_back(events[i]);
  }
  events.swap(kept);
  droppedNoteOnEvents.fetch_add(droppedCount, std::memory_order_relaxed);
  lastNoteOnsDropped += droppedCount;
  lastOverloadNoteOnsDropped += droppedCount;
  lastPostScheduleDrops += droppedCount;
  lastCatchupPrevented += droppedCount;
}

void Synth::ApplyScheduledEventsLocked(const std::vector<MidiEvent> &events,
                                       unsigned int &appliedCount) {
  for (std::vector<MidiEvent>::const_iterator it = events.begin();
       it != events.end(); ++it) {
    const MidiEvent &event = *it;
    if (IsResetLikeEvent(event)) {
      ResetScheduledAppliedStateLocked();
      consecutiveAccurateBlockStartApplies = 0;
      lastSchedulerLagSamples = 0;
      lastSchedulerLateEvents = 0;
      schedulerWarmupBlocks = kSchedulerWarmupBlockCount;
      accurateEventClockBaseSample.store(0, std::memory_order_relaxed);
      accurateEventClockBaseQpc.store(0, std::memory_order_relaxed);
    }
    engine->ProcessMidiEvent(event);
    if (IsPositiveNoteOn(event)) {
      ScheduledKeyState &state = scheduledKeyStates[event.channel][event.data1];
      ++state.soundingGenerations;
      state.lastAppliedState = true;
      UpdateScheduledKeyStateFromRefsLocked(event.channel, event.data1);
      ++lastNoteOnsStarted;
    } else if (event.type == MidiEvent::NOTE_OFF ||
               (event.type == MidiEvent::NOTE_ON && event.data2 <= 0)) {
      ScheduledKeyState &state = scheduledKeyStates[event.channel][event.data1];
      if (state.soundingGenerations > 0)
        --state.soundingGenerations;
      state.lastAppliedState = (state.soundingGenerations > 0);
      UpdateScheduledKeyStateFromRefsLocked(event.channel, event.data1);
      ++lastNoteOffsProcessed;
      ++lastSchedulerNoteOffsApplied;
    } else if (IsReleaseAffectingControlEvent(event)) {
      ++lastSchedulerReleaseControlsApplied;
    }
    ++appliedCount;
  }
}

void Synth::SetRealtimeBudgetMs(float audioBudgetMs) {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  currentAudioBudgetMs = audioBudgetMs;
}

void Synth::SetRenderBlockContext(unsigned long long blockStartSample,
                                  int blockFrames, int sampleRate,
                                  long long blockStartQpc, long long blockEndQpc,
                                  bool quantizedByPollingRate) {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  const unsigned long long previousBlockStartSample = currentRenderBlockStartSample;
  const long long previousBlockStartQpc = currentRenderBlockStartQpc;
  const long long previousRenderEndSample =
      renderProgressEndSample.load(std::memory_order_relaxed);
  const bool timelineRewound =
      (previousRenderEndSample > 0 &&
       (blockStartSample == 0 ||
        (long long)blockStartSample + (blockFrames > 0 ? blockFrames : 0) <
            previousRenderEndSample));
  const bool qpcRewound =
      previousBlockStartQpc != 0 && blockStartQpc != 0 &&
      blockStartQpc < previousBlockStartQpc;

  if (timelineRewound || qpcRewound) {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    ResetScheduledStateLocked();
    overloadState = 0;
    consecutiveOverloadBlocks = 0;
    lastAsyncLagState = 0;
    lastSchedulerLagSamples = 0;
    lastSchedulerLateEvents = 0;
    lastSchedulerDueEvents = 0;
    accurateReleaseLaneDepth = 0;
    accurateIngressDepth = 0;
    accurateControlsCoalesced = 0;
    accurateCatchupPrevented = 0;
    LogTimingDebug(
        "SVMS: Render timeline rewind detected sample=%llu prev=%llu qpc=%lld prevQpc=%lld, resetting accurate session state\n",
        blockStartSample, previousBlockStartSample, blockStartQpc,
        previousBlockStartQpc);
  }

  currentRenderBlockStartSample = blockStartSample;
  currentRenderBlockStartQpc = blockStartQpc;
  currentRenderBlockEndQpc = blockEndQpc;
  currentRenderSampleRate = sampleRate;
  currentBlockQuantized = quantizedByPollingRate;
  lastSchedulerBlockFrames = blockFrames > 0 ? (unsigned int)blockFrames : 0;
  lastSchedulerBlockMs =
      (blockFrames > 0 && sampleRate > 0)
          ? (float)((double)blockFrames * 1000.0 / (double)sampleRate)
          : 0.0f;
  schedulerAnchorSample.store((long long)blockStartSample,
                              std::memory_order_relaxed);
  schedulerAnchorQpc.store(blockStartQpc, std::memory_order_relaxed);
  schedulerAnchorSampleRate.store(sampleRate, std::memory_order_relaxed);
  renderProgressSample.store((long long)blockStartSample, std::memory_order_relaxed);
  renderProgressEndSample.store((long long)blockStartSample + blockFrames,
                                std::memory_order_relaxed);
  if (schedulerWarmupBlocks > 0)
    --schedulerWarmupBlocks;
  if (sampleRate > 0) {
    int existingRate =
        accurateEventClockSampleRate.load(std::memory_order_relaxed);
    if (existingRate != sampleRate) {
      accurateEventClockSampleRate.store(sampleRate, std::memory_order_relaxed);
      accurateEventClockBaseSample.store(0, std::memory_order_relaxed);
      accurateEventClockBaseQpc.store(0, std::memory_order_relaxed);
      ++accurateClockResetCount;
      if (IsAccurateEventThreadEnabledLocked()) {
        LogTimingDebug(
            "SVMS: Accurate clock rate reset for decoupled mode rate=%d block=%d\n",
            sampleRate, blockFrames);
        SignalAccurateEventThread();
      }
    } else if (IsAccurateEventThreadEnabledLocked() && blockStartQpc != 0) {
      SignalAccurateEventThread();
    }
  }
  if (engine) {
    engine->SetRenderWindow(blockStartSample, blockFrames, sampleRate,
                            blockStartQpc, blockEndQpc, quantizedByPollingRate);
  }
}

int Synth::GetEventPriority(const MidiEvent &event) {
  if (event.type == MidiEvent::NOTE_ON && event.data2 > 0)
    return 2;
  if (event.type == MidiEvent::NOTE_OFF || event.type == MidiEvent::RESET)
    return 0;
  return 1;
}

bool Synth::IsCriticalControlEvent(const MidiEvent &event) const {
  if (event.type != MidiEvent::CONTROL_CHANGE)
    return false;
  return event.data1 == 64 ? event.data2 < 64
                           : (event.data1 == 120 || event.data1 == 121 ||
                              event.data1 == 123);
}

void Synth::EnqueueDeferredEvent(const MidiEvent &event) {
  int priority = GetEventPriority(event);
  if (priority == 0 || IsCriticalControlEvent(event))
    deferredCriticalEvents.push_back(event);
  else if (priority == 1)
    deferredRealtimeEvents.push_back(event);
  else
    deferredNoteOnEvents.push_back(event);
}

unsigned int Synth::DropDeferredNoteOnsForKey(int channel, int note) {
  unsigned int removed = 0;
  uint32_t originalCount = deferredNoteOnEvents.size();
  for (uint32_t i = 0; i < originalCount; ++i) {
    MidiEvent event = deferredNoteOnEvents.front();
    deferredNoteOnEvents.pop_front();
    if (event.channel == channel && event.data1 == note) {
      ++removed;
    } else {
      deferredNoteOnEvents.push_back(event);
    }
  }
  return removed;
}

unsigned int Synth::DropOldestDeferredNoteOns(unsigned int count) {
  unsigned int removed = 0;
  while (removed < count && !deferredNoteOnEvents.empty()) {
    deferredNoteOnEvents.pop_front();
    ++removed;
  }
  return removed;
}

unsigned int Synth::DropOldestPendingVectorNoteOnsLocked(
    std::vector<MidiEvent> &queue, unsigned int count, int velocityFloor) {
  if (count == 0 || queue.empty())
    return 0;

  unsigned int removed = 0;
  std::vector<MidiEvent> kept;
  kept.reserve(queue.size());
  for (std::vector<MidiEvent>::const_iterator it = queue.begin();
       it != queue.end(); ++it) {
    bool removable = IsPositiveNoteOn(*it) &&
                     (velocityFloor < 0 || it->data2 <= velocityFloor);
    if (removed < count && removable) {
      ++removed;
      continue;
    }
    kept.push_back(*it);
  }
  if (removed > 0)
    queue.swap(kept);
  return removed;
}

unsigned int Synth::DropOldestPendingNoteOnsLocked(FlatMidiQueue &queue,
                                                   unsigned int count,
                                                   int velocityFloor) {
  if (count == 0 || queue.empty())
    return 0;

  unsigned int removed = 0;
  uint32_t originalCount = queue.size();
  for (uint32_t i = 0; i < originalCount; ++i) {
    MidiEvent event = queue.front();
    queue.pop_front();
    bool removable = IsPositiveNoteOn(event) &&
                     (velocityFloor < 0 || event.data2 <= velocityFloor);
    if (removed < count && removable) {
      ++removed;
      continue;
    }
    queue.push_back(event);
  }
  return removed;
}

unsigned int Synth::DropScheduledPendingNoteOnsLocked(unsigned int count,
                                                      int velocityFloor) {
  if (count == 0)
    return 0;

  unsigned int removed = 0;
  while (removed < count) {
    std::vector<ScheduledNoteOnTrimEntry> skipped;
    uint32_t bestEventId = 0u;
    bool foundCandidate = false;
    while (PopScheduledNoteOnTrimLocked(&bestEventId) && bestEventId != 0u) {
      ScheduledTimedEvent *bestEvent = GetScheduledEventLocked(bestEventId);
      if (!bestEvent || bestEvent->cancelled || !IsPositiveNoteOn(bestEvent->event))
        continue;
      if (velocityFloor >= 0 && bestEvent->event.data2 > velocityFloor) {
        ScheduledNoteOnTrimEntry entry;
        entry.enqueueTick = bestEvent->enqueueTick;
        entry.targetSample = bestEvent->targetSample;
        entry.sequence = bestEvent->event.sequence;
        entry.velocity = (unsigned char)(bestEvent->event.data2 < 0
                                             ? 0
                                             : (bestEvent->event.data2 > 127
                                                    ? 127
                                                    : bestEvent->event.data2));
        entry.eventId = bestEventId;
        skipped.push_back(entry);
        continue;
      }
      foundCandidate = true;
      break;
    }
    for (size_t i = 0; i < skipped.size(); ++i) {
      scheduledNoteOnTrimHeap.push_back(skipped[i]);
      std::push_heap(scheduledNoteOnTrimHeap.begin(),
                     scheduledNoteOnTrimHeap.end(), ScheduledNoteOnTrimLess);
    }
    if (!foundCandidate)
      break;
    {
      ScheduledTimedEvent *bestEvent = GetScheduledEventLocked(bestEventId);
      if (bestEvent && !bestEvent->cancelled) {
      const int channel = bestEvent->channel;
      const int note = bestEvent->note;
      CancelScheduledEventLocked(bestEventId);
      if (channel >= 0 && channel < 16 && note >= 0 && note < 128) {
        RemovePendingTransitionEventLocked(channel, note, bestEventId);
        UpdateScheduledKeyStateFromRefsLocked(channel, note);
      }
      ++removed;
      }
    }
  }

  return removed;
}

unsigned int Synth::DropOldestIngressNoteOnsLocked(unsigned int count,
                                                   int velocityFloor) {
  unsigned int removed = 0;
  removed += DropOldestPendingVectorNoteOnsLocked(
      pendingNoteOnEvents, count - removed, velocityFloor);
  if (removed < count)
    removed += DropOldestPendingNoteOnsLocked(accuratePendingEvents,
                                              count - removed, velocityFloor);
  if (removed < count)
    removed += DropOldestDeferredNoteOns(count - removed);
  if (removed < count)
    removed +=
        DropOldestPendingVectorNoteOnsLocked(incomingNoteOnEvents, count - removed,
                                             velocityFloor);
  return removed;
}

unsigned int Synth::ExtractAccurateWorksetLocked(std::vector<MidiEvent> &events,
                                                 unsigned int maxCount,
                                                 unsigned int overloadState) {
  events.clear();
  if (accuratePendingEvents.empty())
    return 0;

  bool seenController[16][128] = {};
  bool seenProgram[16] = {};
  bool seenPitch[16] = {};
  bool seenNoteTransition[16][128] = {};
  unsigned int removedNoteOns = 0;

  while (!accuratePendingEvents.empty()) {
    MidiEvent event = accuratePendingEvents.back();
    accuratePendingEvents.pop_back();

    if (overloadState > 0) {
      if (event.channel >= 0 && event.channel < 16) {
        if (event.type == MidiEvent::CONTROL_CHANGE && event.data1 >= 0 &&
            event.data1 < 128) {
          if (seenController[event.channel][event.data1]) {
            ++accurateControlsCoalesced;
            ++lastAsyncCoalesced;
            continue;
          }
          seenController[event.channel][event.data1] = true;
        } else if (event.type == MidiEvent::PROGRAM_CHANGE) {
          if (seenProgram[event.channel]) {
            ++accurateControlsCoalesced;
            ++lastAsyncCoalesced;
            continue;
          }
          seenProgram[event.channel] = true;
        } else if (event.type == MidiEvent::PITCH_BEND) {
          if (seenPitch[event.channel]) {
            ++accurateControlsCoalesced;
            ++lastAsyncCoalesced;
            continue;
          }
          seenPitch[event.channel] = true;
        }
      }

      if (IsNoteTransitionEvent(event) && event.channel >= 0 &&
          event.channel < 16 && event.data1 >= 0 && event.data1 < 128) {
        if (IsPositiveNoteOn(event) &&
            seenNoteTransition[event.channel][event.data1]) {
          ++removedNoteOns;
          ++lastStaleNoteOnsDropped;
          ++lastPreScheduleDrops;
          ++lastCatchupPrevented;
          continue;
        }
        seenNoteTransition[event.channel][event.data1] = true;
      }
    }

    if (events.size() < maxCount) {
      events.push_back(event);
      continue;
    }

    if (IsPositiveNoteOn(event)) {
      ++removedNoteOns;
      ++lastOverloadNoteOnsDropped;
      ++lastPreScheduleDrops;
      ++lastCatchupPrevented;
      continue;
    }

    if (IsReleaseLikeEvent(event) || IsCriticalControlEvent(event) ||
        event.type == MidiEvent::PROGRAM_CHANGE ||
        event.type == MidiEvent::PITCH_BEND ||
        event.type == MidiEvent::CONTROL_CHANGE || event.type == MidiEvent::RESET) {
      EnqueueReleaseLaneEventLocked(event);
      continue;
    }

    ++lastPreScheduleDrops;
    ++lastCatchupPrevented;
    droppedNonNoteEvents.fetch_add(1, std::memory_order_relaxed);
  }

  if (removedNoteOns > 0)
    droppedNoteOnEvents.fetch_add(removedNoteOns, std::memory_order_relaxed);
  if (!events.empty())
    std::reverse(events.begin(), events.end());
  return removedNoteOns;
}

unsigned int Synth::TrimAccurateOverloadQueuesLocked(unsigned int overloadState) {
  if (overloadState == 0)
    return 0;

  const unsigned int softDeferredThreshold =
      GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
  const unsigned int hardDeferredThreshold =
      GetAdaptiveHardDeferredThreshold(configuredMaxVoices);
  const unsigned int trimTarget = softDeferredThreshold > kScheduledTrimHeadroom
                                      ? (softDeferredThreshold -
                                         kScheduledTrimHeadroom)
                                      : softDeferredThreshold;
  unsigned int removed = 0;
  if (overloadState >= 2) {
    if (accuratePendingEvents.size() > trimTarget) {
      removed += DropOldestPendingNoteOnsLocked(
          accuratePendingEvents,
          static_cast<unsigned int>(accuratePendingEvents.size()) -
              trimTarget,
          -1);
    }
    if (deferredNoteOnEvents.size() > trimTarget) {
      removed += DropOldestDeferredNoteOns(
          static_cast<unsigned int>(deferredNoteOnEvents.size()) -
          trimTarget);
    }
  } else {
    if (accuratePendingEvents.size() > hardDeferredThreshold) {
      removed += DropOldestPendingNoteOnsLocked(
          accuratePendingEvents,
          static_cast<unsigned int>(accuratePendingEvents.size()) -
              trimTarget,
          kSoftScheduledTrimVelocityFloor);
    }
    if (deferredNoteOnEvents.size() > hardDeferredThreshold) {
      removed += DropOldestDeferredNoteOns(
          static_cast<unsigned int>(deferredNoteOnEvents.size()) -
          trimTarget);
    }
  }

  if (removed > 0) {
    droppedNoteOnEvents.fetch_add(removed, std::memory_order_relaxed);
    lastAsyncDropped += removed;
    lastNoteOnsDropped += removed;
  }
  return removed;
}

bool Synth::PopNextDeferredEvent(MidiEvent &event, int priorityClass) {
  FlatMidiQueue *queue = priorityClass == 0
                             ? &deferredCriticalEvents
                             : (priorityClass == 1 ? &deferredRealtimeEvents
                                                   : &deferredNoteOnEvents);
  if (queue->empty())
    return false;
  event = queue->front();
  queue->pop_front();
  return true;
}

bool Synth::PopNextIncomingEvent(MidiEvent &event, int priorityClass) {
  std::vector<MidiEvent> *queue = priorityClass == 0
                                      ? &incomingCriticalEvents
                                      : (priorityClass == 1 ? &incomingRealtimeEvents
                                                            : &incomingNoteOnEvents);
  size_t *index = priorityClass == 0
                      ? &incomingCriticalIndex
                      : (priorityClass == 1 ? &incomingRealtimeIndex
                                            : &incomingNoteOnIndex);
  if (*index >= queue->size())
    return false;
  event = (*queue)[(*index)++];
  return true;
}

float Synth::GetMidiBudgetMsLocked() const {
  float audioBudgetMs = currentAudioBudgetMs;
  if (audioBudgetMs <= 0.0f)
    return kMaxMidiBudgetMs;

  float safeBudgetMs = audioBudgetMs * kSafeAudioBudgetFactor;
  if (safeBudgetMs <= 0.0f)
    safeBudgetMs = audioBudgetMs;

  float baseBudgetMs = audioBudgetMs * kMidiBudgetFraction;
  if (baseBudgetMs > kMaxMidiBudgetMs)
    baseBudgetMs = kMaxMidiBudgetMs;

  float remainingBudgetMs = safeBudgetMs - lastSampleRenderEstimateMs;
  if (remainingBudgetMs < kMinMidiBudgetMs)
    remainingBudgetMs = kMinMidiBudgetMs;

  return baseBudgetMs < remainingBudgetMs ? baseBudgetMs : remainingBudgetMs;
}

long long Synth::GetRenderBlockEndSampleLocked() const {
  return (long long)currentRenderBlockStartSample +
         (long long)lastSchedulerBlockFrames;
}

void Synth::Shutdown(bool waitForThreads) {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  acceptingEvents.store(0, std::memory_order_relaxed);
  if (refCount > 0) {
    refCount--;
    if (refCount > 0)
      return;
  }

  if (engine) {
    engine->Shutdown(waitForThreads);
    engine.reset();
  }

  StopAccurateEventThreadLocked(waitForThreads);

  requestedSamplerEngineName.clear();
  lastInitStatus = "Synth stopped";
  eventTimingModeFast.store((int)eventTimingMode, std::memory_order_relaxed);
  {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    pendingCriticalEvents.clear();
    pendingRealtimeEvents.clear();
    pendingNoteOnEvents.clear();
    deferredCriticalEvents.clear();
    deferredRealtimeEvents.clear();
    deferredNoteOnEvents.clear();
    overloadReleaseEvents.clear();
    incomingCriticalEvents.clear();
    incomingRealtimeEvents.clear();
    incomingNoteOnEvents.clear();
    accuratePendingEvents.clear();
    ResetEventRing(realtimeIngressRing);
    ResetEventRing(accurateIngressRing);
    ResetScheduledStateLocked();
  }
  realtimeIngressDepth.store(0, std::memory_order_relaxed);
  accurateIngressDepthAtomic.store(0, std::memory_order_relaxed);
  droppedNoteOnEvents.store(0, std::memory_order_relaxed);
  droppedNonNoteEvents.store(0, std::memory_order_relaxed);
  maxObservedQueueDepth.store(0, std::memory_order_relaxed);
  maxAsyncQueueDepth.store(0, std::memory_order_relaxed);
  lastPendingQueueDepth = 0;
  lastDeferredQueueDepth = 0;
  lastEventsProcessed = 0;
  lastNoteOnsAttempted = 0;
  lastNoteOnsStarted = 0;
  lastNoteOnsDropped = 0;
  lastNoteOffsProcessed = 0;
  lastAsyncPendingNoteOns = 0;
  lastAsyncStarted = 0;
  lastAsyncDropped = 0;
  lastAsyncCoalesced = 0;
  lastOverloadNoteOnsDropped = 0;
  lastStaleNoteOnsDropped = 0;
  lastPreScheduleDrops = 0;
  lastPostScheduleDrops = 0;
  lastCatchupPrevented = 0;
  accurateControlsCoalesced = 0;
  lastAsyncQueueAgeMs = 0;
  lastAsyncLagState = 0;
  lastSchedulerDueEvents = 0;
  lastSchedulerLateEvents = 0;
  lastSchedulerLagSamples = 0;
  perfSchedulerCacheRebuilds = 0;
  perfSchedulerTrimHeapTombstonePrunes = 0;
  lastSchedulerPendingSameKeyTransitions = 0;
  lastSchedulerMaxSameKeyQueueDepth = 0;
  lastSchedulerNoteOnsCoalesced = 0;
  lastSchedulerNoteOffsApplied = 0;
  lastSchedulerNoteOffsCoalesced = 0;
  lastSchedulerNoteOffsCanceled = 0;
  lastSchedulerReleaseControlsApplied = 0;
  lastSchedulerRenderSplits = 0;
  overloadState = 0;
  consecutiveOverloadBlocks = 0;
  lastMidiProcessMs = 0.0f;
  currentAudioBudgetMs = 0.0f;
  lastSampleRenderEstimateMs = 0.0f;
  accurateReleaseLaneDepth = 0;
  accurateIngressDepth = 0;
  accurateControlsCoalesced = 0;
}

void Synth::ForceShutdown(bool waitForThreads) {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  acceptingEvents.store(0, std::memory_order_relaxed);
  refCount = 0;

  if (engine) {
    engine->Shutdown(waitForThreads);
    engine.reset();
  }

  StopAccurateEventThreadLocked(waitForThreads);

  requestedSamplerEngineName.clear();
  lastInitStatus = "Synth force-stopped";
  eventTimingModeFast.store((int)eventTimingMode, std::memory_order_relaxed);
  {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    pendingCriticalEvents.clear();
    pendingRealtimeEvents.clear();
    pendingNoteOnEvents.clear();
    deferredCriticalEvents.clear();
    deferredRealtimeEvents.clear();
    deferredNoteOnEvents.clear();
    overloadReleaseEvents.clear();
    incomingCriticalEvents.clear();
    incomingRealtimeEvents.clear();
    incomingNoteOnEvents.clear();
    accuratePendingEvents.clear();
    ResetEventRing(realtimeIngressRing);
    ResetEventRing(accurateIngressRing);
    ResetScheduledStateLocked();
  }
  realtimeIngressDepth.store(0, std::memory_order_relaxed);
  accurateIngressDepthAtomic.store(0, std::memory_order_relaxed);
  droppedNoteOnEvents.store(0, std::memory_order_relaxed);
  droppedNonNoteEvents.store(0, std::memory_order_relaxed);
  maxObservedQueueDepth.store(0, std::memory_order_relaxed);
  maxAsyncQueueDepth.store(0, std::memory_order_relaxed);
  lastPendingQueueDepth = 0;
  lastDeferredQueueDepth = 0;
  lastEventsProcessed = 0;
  lastNoteOnsAttempted = 0;
  lastNoteOnsStarted = 0;
  lastNoteOnsDropped = 0;
  lastNoteOffsProcessed = 0;
  lastAsyncPendingNoteOns = 0;
  lastAsyncStarted = 0;
  lastAsyncDropped = 0;
  lastAsyncCoalesced = 0;
  lastOverloadNoteOnsDropped = 0;
  lastStaleNoteOnsDropped = 0;
  lastPreScheduleDrops = 0;
  lastPostScheduleDrops = 0;
  lastCatchupPrevented = 0;
  lastAsyncQueueAgeMs = 0;
  lastAsyncLagState = 0;
  lastSchedulerDueEvents = 0;
  lastSchedulerLateEvents = 0;
  lastSchedulerLagSamples = 0;
  lastSchedulerPendingSameKeyTransitions = 0;
  lastSchedulerMaxSameKeyQueueDepth = 0;
  lastSchedulerNoteOnsCoalesced = 0;
  lastSchedulerNoteOffsApplied = 0;
  lastSchedulerNoteOffsCoalesced = 0;
  lastSchedulerNoteOffsCanceled = 0;
  lastSchedulerReleaseControlsApplied = 0;
  lastSchedulerRenderSplits = 0;
  overloadState = 0;
  consecutiveOverloadBlocks = 0;
  lastMidiProcessMs = 0.0f;
  currentAudioBudgetMs = 0.0f;
  lastSampleRenderEstimateMs = 0.0f;
  accurateReleaseLaneDepth = 0;
  accurateIngressDepth = 0;
  accurateControlsCoalesced = 0;
  accurateCatchupPrevented = 0;
}

void Synth::Initialize() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  if (refCount > 0) {
    refCount++;
    acceptingEvents.store(1, std::memory_order_relaxed);
    return;
  }

  acceptingEvents.store(0, std::memory_order_relaxed);

  runtimeSettings = LoadRuntimeSettingsLocked();
  eventTimingMode = runtimeSettings.eventTimingMode;
  eventTimingModeFast.store((int)eventTimingMode, std::memory_order_relaxed);
  if (IsAccurateEventThreadEnabledLocked())
    EnsureAccurateEventThreadLocked();
  else
    StopAccurateEventThreadLocked(true);

  SamplerInitParams initParams;
  initParams.sampleRate = Config::Instance().GetInt("sample_rate", 44100);
  initParams.maxVoices = Config::Instance().GetInt("max_voices", 500);
  configuredMaxVoices = initParams.maxVoices > 0 ? initParams.maxVoices : 500;
  initParams.runtimeSettings = runtimeSettings;
  lastInitStatus = "Initializing synth...";

  SamplerEngineId requestedEngine =
      ParseSamplerEngineId(GetConfiguredSamplerEngine());
  requestedSamplerEngineName = SamplerEngineIdToConfigString(requestedEngine);
  std::string configuredSource = GetConfiguredSoundSource();

  std::vector<std::string> candidates =
      BuildSourceCandidates(configuredSource, requestedEngine);
  for (size_t i = 0; i < candidates.size(); ++i) {
    initParams.sourcePath = candidates[i];
    if (!FileExists(initParams.sourcePath))
      continue;

    SamplerEngineId resolvedEngine =
        ResolveImplementedEngine(requestedEngine, initParams.sourcePath);
    if (resolvedEngine == SamplerEngineId::TSF) {
      engine.reset(new TsfEngine());
    } else if (resolvedEngine == SamplerEngineId::BASSMIDI) {
      engine.reset(new BassMidiEngine());
    } else if (resolvedEngine == SamplerEngineId::VIRTUALLYSUPER) {
      engine.reset(new VirtuallySuperSamplerEngine());
#ifndef SVMS_LEGACY_XP
    } else if (resolvedEngine == SamplerEngineId::SFZ) {
      engine.reset(new SfzEngine());
#endif
    } else {
      engine.reset();
    }

    if (engine && engine->Initialize(initParams)) {
      {
        compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
        pendingCriticalEvents.clear();
        pendingRealtimeEvents.clear();
        pendingNoteOnEvents.clear();
        deferredCriticalEvents.clear();
        deferredRealtimeEvents.clear();
        deferredNoteOnEvents.clear();
        overloadReleaseEvents.clear();
        incomingCriticalEvents.clear();
        incomingRealtimeEvents.clear();
        incomingNoteOnEvents.clear();
        accuratePendingEvents.clear();
        ResetEventRing(realtimeIngressRing);
        ResetEventRing(accurateIngressRing);
        ResetScheduledStateLocked();
      }
      realtimeIngressDepth.store(0, std::memory_order_relaxed);
      accurateIngressDepthAtomic.store(0, std::memory_order_relaxed);
      droppedNoteOnEvents.store(0, std::memory_order_relaxed);
      droppedNonNoteEvents.store(0, std::memory_order_relaxed);
      maxObservedQueueDepth.store(0, std::memory_order_relaxed);
      maxAsyncQueueDepth.store(0, std::memory_order_relaxed);
      lastPendingQueueDepth = 0;
      lastDeferredQueueDepth = 0;
      lastEventsProcessed = 0;
      lastNoteOnsAttempted = 0;
      lastNoteOnsStarted = 0;
      lastNoteOnsDropped = 0;
      lastNoteOffsProcessed = 0;
      lastAsyncPendingNoteOns = 0;
      lastAsyncStarted = 0;
      lastAsyncDropped = 0;
      lastAsyncCoalesced = 0;
      lastOverloadNoteOnsDropped = 0;
      lastStaleNoteOnsDropped = 0;
      lastPreScheduleDrops = 0;
      lastPostScheduleDrops = 0;
      lastCatchupPrevented = 0;
      lastAsyncQueueAgeMs = 0;
      lastAsyncLagState = 0;
      lastSchedulerDueEvents = 0;
      lastSchedulerLateEvents = 0;
      lastSchedulerLagSamples = 0;
      lastSchedulerPendingSameKeyTransitions = 0;
      lastSchedulerMaxSameKeyQueueDepth = 0;
      lastSchedulerNoteOnsCoalesced = 0;
      lastSchedulerNoteOffsApplied = 0;
      lastSchedulerNoteOffsCoalesced = 0;
      lastSchedulerNoteOffsCanceled = 0;
      lastSchedulerReleaseControlsApplied = 0;
      lastSchedulerRenderSplits = 0;
      overloadState = 0;
      consecutiveOverloadBlocks = 0;
      lastMidiProcessMs = 0.0f;
      currentAudioBudgetMs = 0.0f;
      lastSampleRenderEstimateMs = 0.0f;
      accurateReleaseLaneDepth = 0;
      accurateIngressDepth = 0;
      accurateControlsCoalesced = 0;
      accurateCatchupPrevented = 0;
      refCount = 1;
      acceptingEvents.store(1, std::memory_order_relaxed);
      char status[MAX_PATH + 64];
      sprintf(status, "Loaded %s via %s", engine->GetResolvedSourcePath().c_str(),
              engine->GetEngineName().c_str());
      lastInitStatus = status;
      return;
    }

    if (engine) {
      SamplerDiagnostics diagnostics = engine->GetDiagnostics();
      if (!diagnostics.lastWarning.empty())
        lastInitStatus = diagnostics.lastWarning;
    }
    engine.reset();
  }

#ifdef SVMS_LEGACY_XP
  if (DetectSourceFormat(configuredSource) == "sfz" ||
      requestedEngine == SamplerEngineId::SFZ) {
    lastInitStatus =
        "SFZ is not supported in the legacy XP build. Use TSF/SF2 on XP-safe "
        "targets.";
  } else {
    lastInitStatus =
        "Failed to load any sampler source. Check sound_source/soundfont path.";
  }
#else
  if (lastInitStatus == "Initializing synth..." || lastInitStatus.empty()) {
    lastInitStatus =
        "Failed to load sampler source. Check status details for parser/sample "
        "load errors.";
  }
#endif
  OutputDebugStringA("SVMS: ERROR - Failed to initialize sampler engine\n");
}

void Synth::PushEvent(MidiEvent ev) {
  if (acceptingEvents.load(std::memory_order_relaxed) == 0)
    return;

  ev.sequence = nextEventSequence.fetch_add(1, std::memory_order_relaxed);
  const EventTimingMode timingMode = GetEventTimingModeFast();
  const bool prefersAccurateThread =
      timingMode == EventTimingMode::ACCURATE && IsScheduledTimingEvent(ev);
  if (prefersAccurateThread) {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    ev.arrivalQpc = now.QuadPart;
  } else {
    ev.arrivalQpc = 0;
  }
  ev.targetSample = 0;

  int anchorRate = schedulerAnchorSampleRate.load(std::memory_order_relaxed);
  unsigned int realtimeDepth = realtimeIngressDepth.load(std::memory_order_relaxed);
  unsigned int accurateDepth =
      accurateIngressDepthAtomic.load(std::memory_order_relaxed);
  bool routeToAccurateThread = prefersAccurateThread;
  unsigned int internalDepth = 0;
  unsigned int totalDepth = 0;
  bool softIngressOverload = false;
  bool hardIngressOverload = false;

  {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    const unsigned int softDeferredThreshold =
        GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
    const unsigned int hardDeferredThreshold =
        GetAdaptiveHardDeferredThreshold(configuredMaxVoices);
    const unsigned int extremePendingThreshold =
        GetAdaptiveExtremePendingThreshold(configuredMaxVoices);
    internalDepth = static_cast<unsigned int>(
        pendingCriticalEvents.size() + pendingRealtimeEvents.size() +
        pendingNoteOnEvents.size() + accuratePendingEvents.size() +
        deferredCriticalEvents.size() + deferredRealtimeEvents.size() +
        deferredNoteOnEvents.size() + overloadReleaseEvents.size());
    totalDepth = internalDepth + realtimeDepth + accurateDepth;
    softIngressOverload = totalDepth >= softDeferredThreshold;
    hardIngressOverload = totalDepth >= hardDeferredThreshold;

    if (totalDepth >= kMaxPendingMidiEvents || hardIngressOverload) {
      if (IsPositiveNoteOn(ev)) {
        droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      if (IsReleaseLikeEvent(ev) || IsCriticalControlEvent(ev)) {
        EnqueueReleaseLaneEventLocked(ev);
        accurateReleaseLaneDepth =
            static_cast<unsigned int>(overloadReleaseEvents.size());
        accurateIngressDepth = internalDepth + realtimeDepth + accurateDepth;
        UpdateObservedMax(maxObservedQueueDepth, accurateIngressDepth);
        return;
      }

      droppedNonNoteEvents.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (softIngressOverload &&
        (IsReleaseLikeEvent(ev) || IsCriticalControlEvent(ev))) {
      EnqueueReleaseLaneEventLocked(ev);
      accurateReleaseLaneDepth =
          static_cast<unsigned int>(overloadReleaseEvents.size());
      accurateIngressDepth = internalDepth + realtimeDepth + accurateDepth;
      UpdateObservedMax(maxObservedQueueDepth, accurateIngressDepth);
      return;
    }
  }

  if (routeToAccurateThread && !IsPositiveNoteOn(ev) &&
      totalDepth >= GetAdaptiveSoftDeferredThreshold(configuredMaxVoices)) {
    routeToAccurateThread = false;
  }

  if (routeToAccurateThread && IsPositiveNoteOn(ev) &&
      totalDepth >= GetAdaptiveExtremePendingThreshold(configuredMaxVoices)) {
    droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  EventRingBuffer &targetRing =
      routeToAccurateThread ? accurateIngressRing : realtimeIngressRing;
  std::atomic<unsigned int> &targetDepth =
      routeToAccurateThread ? accurateIngressDepthAtomic : realtimeIngressDepth;
  if (!TryPushEventRing(targetRing, ev, targetDepth)) {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    if (IsReleaseLikeEvent(ev) || IsCriticalControlEvent(ev)) {
      EnqueueReleaseLaneEventLocked(ev);
      accurateReleaseLaneDepth =
          static_cast<unsigned int>(overloadReleaseEvents.size());
    } else if (IsPositiveNoteOn(ev)) {
      droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
      return;
    } else {
      droppedNonNoteEvents.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }

  const unsigned int depth = internalDepth +
                             realtimeIngressDepth.load(std::memory_order_relaxed) +
                             accurateIngressDepthAtomic.load(std::memory_order_relaxed);
  accurateIngressDepth = depth;
  if (depth > accuratePeakPendingEvents)
    accuratePeakPendingEvents = depth;
  if (depth >= GetAdaptiveHardDeferredThreshold(configuredMaxVoices) &&
      anchorRate > 0 &&
      timingMode != EventTimingMode::LEGACY_SYNC &&
      (ev.sequence <= 16 || (ev.sequence % 8192u) == 0u)) {
    LogTimingDebug(
        "SVMS: Enqueue pressure depth=%u accurateRing=%u realtimeRing=%u mode=%s\n",
        depth, accurateIngressDepthAtomic.load(std::memory_order_relaxed),
        realtimeIngressDepth.load(std::memory_order_relaxed),
        EventTimingModeToConfigString(timingMode));
  }
  UpdateObservedMax(maxObservedQueueDepth, depth);
  if (routeToAccurateThread)
    SignalAccurateEventThread();
}

void Synth::ProcessEventsLocked() {
  LARGE_INTEGER freq = GetPerfFrequency();
  LARGE_INTEGER start = {};
  LARGE_INTEGER end = {};
  QueryPerformanceCounter(&start);

  {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    DrainEventRingToPendingLocked(realtimeIngressRing, realtimeIngressDepth, false,
                                  kAccurateDrainBatchLimit);
    incomingCriticalEvents.swap(pendingCriticalEvents);
    incomingRealtimeEvents.swap(pendingRealtimeEvents);
    incomingNoteOnEvents.swap(pendingNoteOnEvents);
    lastPendingQueueDepth = GetIngressDepthLocked();
  }
  incomingCriticalIndex = 0;
  incomingRealtimeIndex = 0;
  incomingNoteOnIndex = 0;

  lastEventsProcessed = 0;
  lastNoteOnsAttempted = 0;
  lastNoteOnsStarted = 0;
  lastNoteOnsDropped = 0;
  lastNoteOffsProcessed = 0;
  lastAsyncPendingNoteOns = 0;
  lastAsyncStarted = 0;
  lastAsyncDropped = 0;
  lastAsyncCoalesced = 0;
  lastAsyncQueueAgeMs = 0;
  lastAsyncLagState = 0;
  lastSchedulerDueEvents = 0;
  lastSchedulerLateEvents = 0;
  lastSchedulerLagSamples = 0;

  const bool schedulerEnabled = IsSchedulerEnabledLocked();
  const unsigned int deferredBefore = GetDeferredEventCount(
      deferredCriticalEvents, deferredRealtimeEvents, deferredNoteOnEvents);
  const unsigned int scheduledBefore = ComputeScheduledPendingCountLocked();
  const unsigned int totalBacklogBefore =
      scheduledBefore + deferredBefore + lastPendingQueueDepth;
  if (lastPendingQueueDepth > accuratePeakPendingEvents)
    accuratePeakPendingEvents = lastPendingQueueDepth;
  if (deferredBefore > accuratePeakDeferredEvents)
    accuratePeakDeferredEvents = deferredBefore;
  if (scheduledBefore > accuratePeakScheduledEvents)
    accuratePeakScheduledEvents = scheduledBefore;

  const unsigned int softDeferredThreshold =
      GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
  const unsigned int hardDeferredThreshold =
      GetAdaptiveHardDeferredThreshold(configuredMaxVoices);
  const unsigned int previousOverloadState = overloadState;
  const bool warmupGraceActive =
      schedulerWarmupBlocks > 0 && lastSchedulerLateEvents == 0 &&
      lastSchedulerLagSamples == 0 && lastAsyncQueueAgeMs < 8u;
  unsigned int blockOverloadState =
      totalBacklogBefore >= hardDeferredThreshold
          ? 2u
          : (totalBacklogBefore >= softDeferredThreshold ? 1u : 0u);

  float safeBudgetMs = currentAudioBudgetMs * kSafeAudioBudgetFactor;
  if (safeBudgetMs > 0.0f && !warmupGraceActive) {
    float estimatedBlockMs = lastMidiProcessMs + lastSampleRenderEstimateMs;
    const bool budgetPressureEligible =
        previousOverloadState > 0u ||
        totalBacklogBefore >= (softDeferredThreshold / 2u) ||
        lastAsyncQueueAgeMs >= 8u || lastSchedulerLateEvents > 0 ||
        lastSchedulerLagSamples >
            (unsigned int)(kAccurateCoalesceWindowSamples * 2u);
    if (budgetPressureEligible) {
      if (estimatedBlockMs >= safeBudgetMs * 1.10f)
        blockOverloadState = 2u;
      else if (estimatedBlockMs >= safeBudgetMs * 0.98f &&
               blockOverloadState < 1u)
        blockOverloadState = 1u;
    }
  }

  float midiBudgetMs = GetMidiBudgetMsLocked();
  unsigned int scheduledLagState = ComputeScheduledLagStateLocked();
  if (scheduledLagState > blockOverloadState)
    blockOverloadState = scheduledLagState;
  const bool cadencePressureActive =
      lastSchedulerLateEvents > 0 ||
      lastSchedulerLagSamples >
          (unsigned int)(kAccurateCoalesceWindowSamples * 2u);
  if (blockOverloadState >= 2u && totalBacklogBefore >= hardDeferredThreshold &&
      previousOverloadState == 0u && lastAsyncQueueAgeMs < 4u &&
      lastSchedulerLagSamples < (kAccurateCoalesceWindowSamples * 4u) &&
      lastPendingQueueDepth < (softDeferredThreshold / 2u)) {
    blockOverloadState = 1u;
  }
  if (!warmupGraceActive && cadencePressureActive &&
      consecutiveAccurateBlockStartApplies >= 12 &&
      blockOverloadState < 2u)
    blockOverloadState = 2u;
  else if (!warmupGraceActive && cadencePressureActive &&
           consecutiveAccurateBlockStartApplies >= 4 &&
           blockOverloadState < 1u)
    blockOverloadState = 1u;

  const bool hardSheddingActive =
      blockOverloadState >= 2u &&
      (scheduledLagState >= 2u || totalBacklogBefore >= hardDeferredThreshold ||
       lastAsyncQueueAgeMs >= 100u || lastSchedulerLateEvents > 0 ||
       lastSchedulerLagSamples >
           (unsigned int)(kAccurateCoalesceWindowSamples * 2u));
  overloadState = blockOverloadState;

  if (engine) {
    engine->SetRealtimePressure(blockOverloadState, scheduledLagState,
                                consecutiveAccurateBlockStartApplies,
                                scheduledBefore);
    engine->BeginRenderBlock();
  }

  if (schedulerEnabled) {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    TrimAccurateOverloadQueuesLocked(blockOverloadState);
  }

  if (schedulerEnabled && IsStrictAccurateModeLocked()) {
    std::vector<MidiEvent> &accurateEvents = accurateWorksetScratch;
    std::vector<MidiEvent> &releaseLaneEvents = releaseEventsScratch;
    accurateEvents.clear();
    releaseLaneEvents.clear();
    {
      compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
      if (!accuratePendingEvents.empty() && blockOverloadState > 0) {
        unsigned int worksetCap = blockOverloadState >= 2
                                      ? kAccurateHardWorksetCap
                                      : kAccurateSoftWorksetCap;
        ExtractAccurateWorksetLocked(accurateEvents, worksetCap,
                                     blockOverloadState);
      } else if (!accuratePendingEvents.empty()) {
        unsigned int batchCount =
            static_cast<unsigned int>(accuratePendingEvents.size());
        if (batchCount > kAccurateFallbackBatchLimit)
          batchCount = kAccurateFallbackBatchLimit;
        accurateEvents.reserve(batchCount);
        for (unsigned int i = 0; i < batchCount; ++i) {
          accurateEvents.push_back(accuratePendingEvents.front());
          accuratePendingEvents.pop_front();
        }
        if (accuratePendingEvents.size() >
            GetAdaptiveHardDeferredThreshold(configuredMaxVoices)) {
          unsigned int trimCount =
              static_cast<unsigned int>(accuratePendingEvents.size()) -
              GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
          if (trimCount > 0) {
            unsigned int removed = DropOldestPendingNoteOnsLocked(
                accuratePendingEvents, trimCount,
                kSoftScheduledTrimVelocityFloor);
            if (removed > 0)
              droppedNoteOnEvents.fetch_add(removed, std::memory_order_relaxed);
          }
        }
      }
    }
    std::vector<MidiEvent> &bypassEvents = scheduledIngressScratch;
    bypassEvents.clear();
    CollectPendingScheduledEventsLocked(bypassEvents);
    if (!bypassEvents.empty()) {
      if (blockOverloadState > 0) {
        const unsigned int worksetCap = blockOverloadState >= 2
                                            ? kAccurateHardWorksetCap
                                            : kAccurateSoftWorksetCap;
        for (std::vector<MidiEvent>::const_iterator it = bypassEvents.begin();
             it != bypassEvents.end(); ++it) {
          if (accurateEvents.size() < worksetCap) {
            InsertMidiEventBySequence(accurateEvents, *it);
          } else if (IsPositiveNoteOn(*it)) {
            ++lastNoteOnsDropped;
            ++lastAsyncDropped;
            ++lastOverloadNoteOnsDropped;
            ++lastPreScheduleDrops;
            ++lastCatchupPrevented;
            droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
          } else {
            releaseLaneEvents.push_back(*it);
          }
        }
      } else {
        MergeMidiEventVectorsBySequence(accurateEvents, bypassEvents);
      }
    }
    if (!accurateEvents.empty()) {
      if (accurateEvents.size() > kFallbackScheduledBatchCap) {
        std::vector<MidiEvent> overflow(accurateEvents.begin() +
                                            kFallbackScheduledBatchCap,
                                        accurateEvents.end());
        accurateEvents.resize(kFallbackScheduledBatchCap);
        for (std::vector<MidiEvent>::const_iterator it = overflow.begin();
             it != overflow.end(); ++it) {
          if (IsPositiveNoteOn(*it)) {
            ++lastNoteOnsDropped;
            ++lastAsyncDropped;
            ++lastOverloadNoteOnsDropped;
            ++lastPostScheduleDrops;
            ++lastCatchupPrevented;
            droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
          } else if (IsReleaseLikeEvent(*it) || IsCriticalControlEvent(*it)) {
            releaseLaneEvents.push_back(*it);
          } else {
            ++lastPostScheduleDrops;
            ++lastCatchupPrevented;
            EnqueueDeferredEvent(*it);
          }
        }
      }
      if (!releaseLaneEvents.empty()) {
        compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
        for (std::vector<MidiEvent>::const_iterator it = releaseLaneEvents.begin();
             it != releaseLaneEvents.end(); ++it) {
          EnqueueReleaseLaneEventLocked(*it);
        }
      }
      PrepareAccurateTimedEvents(accurateEvents);
      if ((unsigned int)accurateEvents.size() >= kAccurateFallbackBatchLimit ||
          blockOverloadState >= 1u) {
        LogTimingDebug(
            "SVMS: Accurate fallback scheduling %u queued events pending=%u overload=%u\n",
            (unsigned int)accurateEvents.size(),
            lastPendingQueueDepth + lastDeferredQueueDepth, blockOverloadState);
      }
      ScheduleEventsLocked(accurateEvents, blockOverloadState >= 2);
    }
  } else if (schedulerEnabled && !IsStrictAccurateModeLocked()) {
    std::vector<MidiEvent> &scheduledEvents = scheduledIngressScratch;
    scheduledEvents.clear();
    CollectPendingScheduledEventsLocked(scheduledEvents);
    if (!scheduledEvents.empty())
      ScheduleEventsLocked(scheduledEvents, blockOverloadState >= 2);
  } else {
    if (blockOverloadState >= 2 &&
        deferredNoteOnEvents.size() >
            GetAdaptiveHardDeferredThreshold(configuredMaxVoices)) {
      unsigned int trimCount =
          static_cast<unsigned int>(deferredNoteOnEvents.size()) -
          GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
      if (trimCount > 0) {
        unsigned int removed = DropOldestDeferredNoteOns(trimCount);
        if (removed > 0)
          droppedNoteOnEvents.fetch_add(removed, std::memory_order_relaxed);
      }
    }
    MidiEvent event = {};
    while (engine && lastEventsProcessed < kMaxMidiEventsPerRender) {
      QueryPerformanceCounter(&end);
      if (GetElapsedMs(start, end, freq) >= midiBudgetMs)
        break;

      bool found = false;
      for (int priorityClass = 0; priorityClass < 3; ++priorityClass) {
        bool popped = PopNextDeferredEvent(event, priorityClass) ||
                      PopNextIncomingEvent(event, priorityClass);
        if (popped) {
          found = true;
          break;
        }
      }
      if (!found)
        break;

      if (IsPositiveNoteOn(event)) {
        ++lastNoteOnsAttempted;
        if (hardSheddingActive &&
            lastNoteOnsStarted >=
                GetAdaptiveHardNoteOnStartsPerBlock(configuredMaxVoices)) {
          ++lastNoteOnsDropped;
          droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (hardSheddingActive &&
            event.data2 <=
                (runtimeSettings.velocityIgnoreBelow > kHardOverloadVelocityFloor
                     ? runtimeSettings.velocityIgnoreBelow
                     : kHardOverloadVelocityFloor)) {
          ++lastNoteOnsDropped;
          droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        engine->ProcessMidiEvent(event);
        ++lastNoteOnsStarted;
      } else {
        engine->ProcessMidiEvent(event);
        if (event.type == MidiEvent::NOTE_OFF ||
            (event.type == MidiEvent::NOTE_ON && event.data2 <= 0))
          ++lastNoteOffsProcessed;
      }
      ++lastEventsProcessed;
    }

    for (; incomingCriticalIndex < incomingCriticalEvents.size();
         ++incomingCriticalIndex)
      EnqueueDeferredEvent(incomingCriticalEvents[incomingCriticalIndex]);
    for (; incomingRealtimeIndex < incomingRealtimeEvents.size();
         ++incomingRealtimeIndex)
      EnqueueDeferredEvent(incomingRealtimeEvents[incomingRealtimeIndex]);
    for (; incomingNoteOnIndex < incomingNoteOnEvents.size();
         ++incomingNoteOnIndex)
      EnqueueDeferredEvent(incomingNoteOnEvents[incomingNoteOnIndex]);
  }

  incomingCriticalEvents.clear();
  incomingRealtimeEvents.clear();
  incomingNoteOnEvents.clear();

  unsigned int releaseLaneDepthSnapshot = 0;
  unsigned int accuratePendingSnapshot = 0;
  unsigned int ingressDepthSnapshot = 0;
  {
    compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
    releaseLaneDepthSnapshot =
        static_cast<unsigned int>(overloadReleaseEvents.size());
    accuratePendingSnapshot =
        static_cast<unsigned int>(accuratePendingEvents.size());
    ingressDepthSnapshot = GetIngressDepthLocked();
  }
  lastDeferredQueueDepth = GetDeferredEventCount(
      deferredCriticalEvents, deferredRealtimeEvents, deferredNoteOnEvents) +
                           releaseLaneDepthSnapshot;
  lastAsyncPendingNoteOns =
      ComputeScheduledPendingCountLocked() + accuratePendingSnapshot;
  lastAsyncQueueAgeMs = ComputeScheduledQueueAgeMsLocked();
  lastSchedulerLagSamples = ComputeScheduledLagSamplesLocked();
  lastSchedulerPendingSameKeyTransitions = ComputeSameKeyPendingTransitionCountLocked();
  lastSchedulerMaxSameKeyQueueDepth = ComputeMaxSameKeyQueueDepthLocked();
  lastAsyncLagState = ComputeScheduledLagStateLocked();
  lastAsyncDropped += lastPreScheduleDrops + lastPostScheduleDrops;
  accurateCatchupPrevented += lastCatchupPrevented;

  if ((lastDeferredQueueDepth + lastAsyncPendingNoteOns) >=
      GetAdaptiveHardDeferredThreshold(configuredMaxVoices)) {
    overloadState = 2;
  } else if ((lastDeferredQueueDepth + lastAsyncPendingNoteOns) >=
             GetAdaptiveSoftDeferredThreshold(configuredMaxVoices)) {
    overloadState = 1;
  } else {
    overloadState = 0;
  }
  if (lastPendingQueueDepth > accuratePeakPendingEvents)
    accuratePeakPendingEvents = lastPendingQueueDepth;
  if (lastDeferredQueueDepth > accuratePeakDeferredEvents)
    accuratePeakDeferredEvents = lastDeferredQueueDepth;
  if (lastAsyncPendingNoteOns > accuratePeakScheduledEvents)
    accuratePeakScheduledEvents = lastAsyncPendingNoteOns;
  accurateReleaseLaneDepth = releaseLaneDepthSnapshot;
  accurateIngressDepth = ingressDepthSnapshot;

  if (overloadState != 0)
    consecutiveOverloadBlocks++;
  else
    consecutiveOverloadBlocks = 0;

  if (overloadState == 2 && lastLoggedOverloadState != 2)
    ++accurateHardOverloadEntries;
  else if (overloadState < 2 && lastLoggedOverloadState == 2)
    ++accurateHardOverloadRecoveries;

  if (overloadState != lastLoggedOverloadState) {
    LogTimingDebug(
        "SVMS: Overload state %u pending=%u deferred=%u scheduled=%u started=%u dropped=%u midi=%.3f render=%.3f\n",
        overloadState, lastPendingQueueDepth, lastDeferredQueueDepth,
        lastAsyncPendingNoteOns, lastNoteOnsStarted, lastNoteOnsDropped,
        lastMidiProcessMs, lastSampleRenderEstimateMs);
    lastLoggedOverloadState = overloadState;
  }

  QueryPerformanceCounter(&end);
  lastMidiProcessMs = GetElapsedMs(start, end, freq);
}

void Synth::EnsureAccurateEventThreadLocked() {
  if (!accurateEventWakeEvent || !accurateEventStopEvent || accurateEventThread)
    return;
  ResetEvent(accurateEventStopEvent);
  accurateEventThread =
      CreateThread(NULL, 0, &Synth::AccurateEventThreadProc, this, 0, NULL);
  LogTimingDebug("SVMS: Accurate event thread %s\n",
                 accurateEventThread ? "started" : "failed");
}

void Synth::StopAccurateEventThreadLocked(bool waitForThread) {
  if (!accurateEventThread)
    return;
  eventProcessorThreadActive.store(0, std::memory_order_relaxed);
  if (accurateEventStopEvent)
    SetEvent(accurateEventStopEvent);
  if (accurateEventWakeEvent)
    SetEvent(accurateEventWakeEvent);
  if (waitForThread)
    WaitForSingleObject(accurateEventThread, 5000);
  CloseHandle(accurateEventThread);
  accurateEventThread = NULL;
  if (accurateEventStopEvent)
    ResetEvent(accurateEventStopEvent);
  LogTimingDebug("SVMS: Accurate event thread stopped\n");
}

void Synth::SignalAccurateEventThread() {
  if (accurateEventWakeEvent)
    SetEvent(accurateEventWakeEvent);
}

DWORD WINAPI Synth::AccurateEventThreadProc(LPVOID param) {
  Synth *synth = reinterpret_cast<Synth *>(param);
  if (!synth)
    return 0;
  synth->ProcessAccuratePendingEvents();
  return 0;
}

void Synth::PrepareAccurateTimedEvents(std::vector<MidiEvent> &events) {
  if (events.empty())
    return;

  const LARGE_INTEGER freq = GetPerfFrequency();
  int sampleRate = accurateEventClockSampleRate.load(std::memory_order_relaxed);
  if (sampleRate <= 0) {
    const int publishedRate =
        schedulerAnchorSampleRate.load(std::memory_order_relaxed);
    if (publishedRate > 0) {
      accurateEventClockSampleRate.store(publishedRate,
                                         std::memory_order_relaxed);
      sampleRate = publishedRate;
    }
  }

  if (sampleRate <= 0 || freq.QuadPart <= 0)
    return;

  long long clockQpc = accurateEventClockBaseQpc.load(std::memory_order_relaxed);
  long long clockSample =
      accurateEventClockBaseSample.load(std::memory_order_relaxed);

  if (clockQpc == 0) {
    long long seedQpc = 0;
    for (std::vector<MidiEvent>::const_iterator it = events.begin();
         it != events.end(); ++it) {
      if (it->arrivalQpc != 0) {
        seedQpc = it->arrivalQpc;
        break;
      }
    }
    if (seedQpc == 0) {
      LARGE_INTEGER now = {};
      QueryPerformanceCounter(&now);
      seedQpc = now.QuadPart;
    }

    long long seedSample = renderProgressSample.load(std::memory_order_relaxed);
    const long long publishedQpc =
        schedulerAnchorQpc.load(std::memory_order_relaxed);
    const long long publishedSample =
        schedulerAnchorSample.load(std::memory_order_relaxed);
    if (publishedQpc != 0 && seedQpc >= publishedQpc) {
      seedSample = publishedSample +
                   GetElapsedSamples(seedQpc - publishedQpc, sampleRate, freq);
    } else if (seedSample <= 0) {
      seedSample = publishedSample;
    }
    if (seedSample < 0)
      seedSample = 0;

    long long expectedZero = 0;
    if (accurateEventClockBaseQpc.compare_exchange_strong(
            expectedZero, seedQpc, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      accurateEventClockBaseSample.store(seedSample,
                                         std::memory_order_relaxed);
      clockQpc = seedQpc;
      clockSample = seedSample;
      LogTimingDebug(
          "SVMS: Accurate clock seeded decoupled sample=%lld rate=%d qpc=%lld\n",
          seedSample, sampleRate, seedQpc);
    } else {
      clockQpc = accurateEventClockBaseQpc.load(std::memory_order_relaxed);
      clockSample = accurateEventClockBaseSample.load(std::memory_order_relaxed);
    }
  }

  const long long earliestSample =
      renderProgressSample.load(std::memory_order_relaxed);
  const long long liveAnchorQpc =
      schedulerAnchorQpc.load(std::memory_order_relaxed);
  const long long liveAnchorSample =
      schedulerAnchorSample.load(std::memory_order_relaxed);
  const long long liveEndSample =
      renderProgressEndSample.load(std::memory_order_relaxed);

  for (std::vector<MidiEvent>::iterator it = events.begin(); it != events.end();
       ++it) {
    long long eventQpc = it->arrivalQpc;
    if (eventQpc == 0)
      eventQpc = clockQpc;
    long long mappedSample =
        clockSample + GetElapsedSamples(eventQpc - clockQpc, sampleRate, freq);
    if (liveAnchorQpc != 0 && eventQpc >= liveAnchorQpc && liveEndSample > 0) {
      const long long liveMapped =
          liveAnchorSample +
          GetElapsedSamples(eventQpc - liveAnchorQpc, sampleRate, freq);
      if (liveMapped >= earliestSample - (long long)kRenderPollSliceFrames &&
          liveMapped <= liveEndSample) {
        mappedSample = liveMapped;
      }
    }
    if (mappedSample < earliestSample)
      mappedSample = earliestSample;
    it->targetSample = mappedSample;
  }
}

void Synth::ProcessAccuratePendingEvents() {
  HANDLE handles[2] = {accurateEventStopEvent, accurateEventWakeEvent};
  std::vector<MidiEvent> localEvents;
  localEvents.reserve(512);
  ULONGLONG lastClockWaitLog = 0;
  ULONGLONG lastBusyLog = 0;
  ULONGLONG lastScheduleLog = 0;

  while (true) {
    if (WaitForMultipleObjects(2, handles, FALSE, 10) == WAIT_OBJECT_0)
      break;

    if (localEvents.empty()) {
      compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
      DrainEventRingToPendingLocked(accurateIngressRing, accurateIngressDepthAtomic,
                                    true, kAccurateDrainBatchLimit);
      const unsigned int softDeferredThreshold =
          GetAdaptiveSoftDeferredThreshold(configuredMaxVoices);
      const unsigned int hardDeferredThreshold =
          GetAdaptiveHardDeferredThreshold(configuredMaxVoices);
      if (!accuratePendingEvents.empty() &&
          accuratePendingEvents.size() >= softDeferredThreshold) {
        unsigned int workerOverloadState =
            accuratePendingEvents.size() >= hardDeferredThreshold ? 2u : 1u;
        unsigned int workerCap = workerOverloadState >= 2
                                     ? kAccurateHardWorksetCap
                                     : kAccurateSoftWorksetCap;
        ExtractAccurateWorksetLocked(localEvents, workerCap, workerOverloadState);
      } else if (!accuratePendingEvents.empty()) {
        unsigned int batchCount =
            static_cast<unsigned int>(accuratePendingEvents.size());
        unsigned int workerBatchLimit = kAccurateWorkerBatchLimit;
        if (accuratePendingEvents.size() >= hardDeferredThreshold)
          workerBatchLimit = kAccurateWorkerHardBatchLimit;
        else if (accuratePendingEvents.size() >= softDeferredThreshold)
          workerBatchLimit = kAccurateWorkerSoftBatchLimit;
        if (batchCount > workerBatchLimit)
          batchCount = workerBatchLimit;
        localEvents.reserve(batchCount);
        for (unsigned int i = 0; i < batchCount; ++i) {
          localEvents.push_back(accuratePendingEvents.front());
          accuratePendingEvents.pop_front();
        }
        if (accuratePendingEvents.size() > hardDeferredThreshold) {
          unsigned int trimCount =
              static_cast<unsigned int>(accuratePendingEvents.size()) -
              softDeferredThreshold;
          if (trimCount > 0) {
            unsigned int removed = DropOldestPendingNoteOnsLocked(
                accuratePendingEvents, trimCount,
                kSoftScheduledTrimVelocityFloor);
            if (removed > 0)
              droppedNoteOnEvents.fetch_add(removed,
                                            std::memory_order_relaxed);
          }
        }
      }
    }
    if (localEvents.empty())
      continue;

    PrepareAccurateTimedEvents(localEvents);
    if (accurateEventClockSampleRate.load(std::memory_order_relaxed) <= 0 ||
        accurateEventClockBaseQpc.load(std::memory_order_relaxed) == 0) {
      ULONGLONG nowTick = compat::GetTickCount64Compat();
      if (nowTick - lastClockWaitLog >= 250) {
        LogTimingDebug("SVMS: Accurate worker waiting for clock, queued=%u\n",
                       (unsigned int)localEvents.size());
        lastClockWaitLog = nowTick;
      }
      compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
      unsigned int queuedDepth =
          static_cast<unsigned int>(accuratePendingEvents.size());
      unsigned int retryBudget =
          queuedDepth >= GetAdaptiveHardDeferredThreshold(configuredMaxVoices)
              ? kAccurateBlockedRetryHardLimit
              : kAccurateBlockedRetrySoftLimit;
      unsigned int requeuedNoteOns = 0;
      for (std::vector<MidiEvent>::reverse_iterator it = localEvents.rbegin();
           it != localEvents.rend(); ++it) {
        if (!IsPositiveNoteOn(*it)) {
          EnqueueReleaseLaneEventLocked(*it);
          continue;
        }
        if (requeuedNoteOns < retryBudget &&
            accuratePendingEvents.size() < kQueueTrimTarget) {
          accuratePendingEvents.push_front(*it);
          ++requeuedNoteOns;
        } else {
          droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
          ++lastAsyncDropped;
          ++lastNoteOnsDropped;
          ++lastOverloadNoteOnsDropped;
          ++lastPostScheduleDrops;
          ++lastCatchupPrevented;
        }
      }
      accurateReleaseLaneDepth =
          static_cast<unsigned int>(overloadReleaseEvents.size());
      accurateIngressDepth = GetIngressDepthLocked();
      localEvents.clear();
      Sleep(1);
      continue;
    }

    if (!synthMutex.try_lock()) {
      ++accurateWorkerBlockedCount;
      ULONGLONG nowTick = compat::GetTickCount64Compat();
      if (nowTick - lastBusyLog >= 250) {
        LogTimingDebug("SVMS: Accurate worker blocked by synth mutex, queued=%u\n",
                       (unsigned int)localEvents.size());
        lastBusyLog = nowTick;
      }
      compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
      unsigned int scheduledDepth = ComputeScheduledPendingCountLocked();
      unsigned int queuedDepth =
          static_cast<unsigned int>(accuratePendingEvents.size()) + scheduledDepth;
      unsigned int retryBudget =
          queuedDepth >= GetAdaptiveHardDeferredThreshold(configuredMaxVoices)
              ? kAccurateBlockedRetryHardLimit
              : kAccurateBlockedRetrySoftLimit;
      unsigned int requeuedNoteOns = 0;
      for (std::vector<MidiEvent>::reverse_iterator it = localEvents.rbegin();
           it != localEvents.rend(); ++it) {
        if (IsPositiveNoteOn(*it)) {
          if (requeuedNoteOns < retryBudget &&
              accuratePendingEvents.size() < kQueueTrimTarget) {
            accuratePendingEvents.push_front(*it);
            ++requeuedNoteOns;
          } else {
            droppedNoteOnEvents.fetch_add(1, std::memory_order_relaxed);
            ++lastAsyncDropped;
            ++lastNoteOnsDropped;
            ++lastOverloadNoteOnsDropped;
            ++lastPostScheduleDrops;
            ++lastCatchupPrevented;
          }
        } else {
          EnqueueReleaseLaneEventLocked(*it);
        }
      }
      accurateReleaseLaneDepth =
          static_cast<unsigned int>(overloadReleaseEvents.size());
      accurateIngressDepth = GetIngressDepthLocked();
      localEvents.clear();
      Sleep(0);
      continue;
    }

    bool keepRunning = IsAccurateEventThreadEnabledLocked();
    if (keepRunning) {
      eventProcessorThreadActive.store(1, std::memory_order_relaxed);
      unsigned int totalBacklogBefore =
          GetDeferredEventCount(deferredCriticalEvents, deferredRealtimeEvents,
                                deferredNoteOnEvents) +
          ComputeScheduledPendingCountLocked() +
          static_cast<unsigned int>(localEvents.size());
      unsigned int lagState = ComputeScheduledLagStateLocked();
      bool hardOverload =
          totalBacklogBefore >=
              GetAdaptiveHardDeferredThreshold(configuredMaxVoices) ||
                          lagState >= 2 ||
                          consecutiveAccurateBlockStartApplies >= 12;
      if (!localEvents.empty()) {
        ULONGLONG nowTick = compat::GetTickCount64Compat();
        bool shouldLog = localEvents.size() >= 16 ||
                         totalBacklogBefore >=
                             GetAdaptiveSoftDeferredThreshold(
                                 configuredMaxVoices) ||
                         localEvents.front().sequence <= 32u ||
                         nowTick - lastScheduleLog >= 250;
        if (shouldLog) {
        LogTimingDebug(
            "SVMS: Accurate worker scheduling %u events backlog=%u hard=%u firstSeq=%u firstTarget=%lld\n",
            (unsigned int)localEvents.size(), totalBacklogBefore,
            hardOverload ? 1u : 0u, localEvents.front().sequence,
            localEvents.front().targetSample);
          lastScheduleLog = nowTick;
        }
      }
      for (std::vector<MidiEvent>::const_iterator it = localEvents.begin();
           it != localEvents.end(); ++it) {
        InsertScheduledTimedEventLocked(*it, hardOverload);
      }
    }
    synthMutex.unlock();

    if (!keepRunning) {
      compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
      for (std::vector<MidiEvent>::const_iterator it = localEvents.begin();
           it != localEvents.end(); ++it)
        EnqueuePendingEventLocked(*it);
    }

    localEvents.clear();
  }

  eventProcessorThreadActive.store(0, std::memory_order_relaxed);
}

void Synth::Render(float *output, int numFrames) {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  if (!engine) {
    std::fill(output, output + numFrames * 2, 0.0f);
    return;
  }

  LARGE_INTEGER freq = GetPerfFrequency();
  LARGE_INTEGER renderStart = {};
  LARGE_INTEGER renderEnd = {};
  QueryPerformanceCounter(&renderStart);

  ProcessEventsLocked();

  std::vector<MidiEvent> &releaseEvents = releaseEventsScratch;
  std::vector<MidiEvent> &dueEvents = dueEventsScratch;
  releaseEvents.clear();
  dueEvents.clear();

  if (!IsSchedulerEnabledLocked()) {
    engine->Render(output, numFrames);
  } else {
    long long blockStartSample = (long long)currentRenderBlockStartSample;
    long long blockEndSample = blockStartSample + numFrames;
    long long cursorSample = blockStartSample;
    int renderedFrames = 0;
    bool blockStartAppliedOnly = false;
    bool appliedInsideBlock = false;
    static ULONGLONG s_lastBlockStartApplyLogTick = 0;

    unsigned int releaseAppliedCount = 0;
    {
      compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
      PopReleaseLaneEventsLocked(releaseEvents, kReleaseLaneApplyLimit);
    }
    if (!releaseEvents.empty()) {
      ApplyScheduledEventsLocked(releaseEvents, releaseAppliedCount);
      lastSchedulerDueEvents += releaseAppliedCount;
      lastEventsProcessed += releaseAppliedCount;
    }

    unsigned int overdueAppliedCount = 0;
    long long renderUntilSample = blockEndSample;
    if (DrainScheduledWindowLocked(blockStartSample, blockEndSample,
                                   blockStartSample + 1, dueEvents,
                                   renderUntilSample)) {
      unsigned int overdueDroppedCount = 0;
      FilterDueEventsForOverloadLocked(dueEvents, blockStartSample,
                                       overdueDroppedCount);
      ApplyScheduledEventsLocked(dueEvents, overdueAppliedCount);
      if (overdueAppliedCount > 0) {
        ULONGLONG nowTick = compat::GetTickCount64Compat();
        if (overdueAppliedCount >= 16 ||
            nowTick - s_lastBlockStartApplyLogTick >= 250) {
          LogTimingDebug(
              "SVMS: Accurate render applied %u overdue/due events at block start=%lld\n",
              overdueAppliedCount, blockStartSample);
          s_lastBlockStartApplyLogTick = nowTick;
        }
        blockStartAppliedOnly = true;
      }
      lastSchedulerDueEvents += overdueAppliedCount;
      lastEventsProcessed += overdueAppliedCount;
    }

    while (renderedFrames < numFrames) {
      renderProgressSample.store(cursorSample, std::memory_order_relaxed);
      unsigned int releaseChunkApplied = 0;
      {
        compat::LockGuard<compat::Mutex> queueLock(eventQueueMutex);
        PopReleaseLaneEventsLocked(releaseEvents, kReleaseLaneApplyLimit);
      }
      if (!releaseEvents.empty()) {
        ApplyScheduledEventsLocked(releaseEvents, releaseChunkApplied);
        lastSchedulerDueEvents += releaseChunkApplied;
        lastEventsProcessed += releaseChunkApplied;
        if (cursorSample != blockStartSample)
          appliedInsideBlock = true;
      }
      const long long pollSliceEnd =
          cursorSample +
          (long long)((kRenderPollSliceFrames < (unsigned int)(numFrames - renderedFrames))
                          ? kRenderPollSliceFrames
                          : (unsigned int)(numFrames - renderedFrames));
      const long long coalesceWindowEnd =
          cursorSample +
          (long long)((kAccurateCoalesceWindowSamples <
                       (unsigned int)(numFrames - renderedFrames))
                          ? kAccurateCoalesceWindowSamples
                          : (unsigned int)(numFrames - renderedFrames));
      long long renderUntilSample = blockEndSample;
      bool drainedWindow = DrainScheduledWindowLocked(
          cursorSample, blockEndSample, coalesceWindowEnd, dueEvents,
          renderUntilSample);
      if (!drainedWindow && !dueEvents.empty())
        dueEvents.clear();
      if (!drainedWindow && IsStrictAccurateModeLocked() &&
          pollSliceEnd < blockEndSample) {
        renderUntilSample = pollSliceEnd;
      }

      if (renderUntilSample > cursorSample) {
        int chunkFrames = (int)(renderUntilSample - cursorSample);
        engine->Render(output + renderedFrames * 2, chunkFrames);
        renderedFrames += chunkFrames;
        cursorSample = renderUntilSample;
        if (renderedFrames < numFrames)
          ++lastSchedulerRenderSplits;
      }

      unsigned int appliedCount = 0;
      if (!dueEvents.empty()) {
        unsigned int droppedCount = 0;
        FilterDueEventsForOverloadLocked(dueEvents, cursorSample, droppedCount);
        ApplyScheduledEventsLocked(dueEvents, appliedCount);
        if (appliedCount > 0 &&
            (appliedCount >= 32 || cursorSample == blockStartSample)) {
          LogTimingDebug(
              "SVMS: Accurate render applied %u due events at sample=%lld blockStart=%lld rendered=%d/%d\n",
              appliedCount, cursorSample, blockStartSample, renderedFrames,
              numFrames);
        }
        if (cursorSample != blockStartSample)
          appliedInsideBlock = true;
        lastSchedulerDueEvents += appliedCount;
        lastEventsProcessed += appliedCount;
        dueEvents.clear();
      }
    }
    renderProgressSample.store(blockEndSample, std::memory_order_relaxed);
    const bool cadencePressureActive =
        lastSchedulerLateEvents > 0 ||
        lastSchedulerLagSamples >
            (unsigned int)(kAccurateCoalesceWindowSamples * 2u);
    if (blockStartAppliedOnly && !appliedInsideBlock && cadencePressureActive) {
      ++consecutiveAccurateBlockStartApplies;
      if (consecutiveAccurateBlockStartApplies == 8 ||
          (consecutiveAccurateBlockStartApplies % 32u) == 0u) {
        LogTimingDebug(
            "SVMS: Accurate cadence leak suspected, block-start-only applies streak=%u blockFrames=%u due=%u late=%u\n",
            consecutiveAccurateBlockStartApplies, lastSchedulerBlockFrames,
            lastSchedulerDueEvents, lastSchedulerLateEvents);
      }
    } else if (lastSchedulerDueEvents > 0 || consecutiveAccurateBlockStartApplies > 0) {
      consecutiveAccurateBlockStartApplies = 0;
    }
  }

  engine->EndRenderBlock();

  QueryPerformanceCounter(&renderEnd);
  lastSampleRenderEstimateMs = GetElapsedMs(renderStart, renderEnd, freq);
}

void Synth::NoteOn(int channel, int note, int velocity) {
  PushEvent({MidiEvent::NOTE_ON, channel, note, velocity});
}

void Synth::NoteOff(int channel, int note) {
  PushEvent({MidiEvent::NOTE_OFF, channel, note, 0});
}

void Synth::ProgramChange(int channel, int program) {
  PushEvent({MidiEvent::PROGRAM_CHANGE, channel, program, 0});
}

void Synth::ControlChange(int channel, int control, int value) {
  PushEvent({MidiEvent::CONTROL_CHANGE, channel, control, value});
}

void Synth::PitchBend(int channel, int value) {
  PushEvent({MidiEvent::PITCH_BEND, channel, value, 0});
}

void Synth::Reset() { PushEvent({MidiEvent::RESET, 0, 0, 0}); }

void Synth::SetRestartReason(unsigned int reasonCode) {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  lastRestartReason = reasonCode;
}

int Synth::GetRefCount() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  return refCount;
}

std::string Synth::GetResolvedSoundfontPath() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  return engine ? engine->GetResolvedSourcePath() : std::string();
}

std::string Synth::GetResolvedSamplerEngineName() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  return engine ? engine->GetEngineName() : std::string();
}

std::string Synth::GetRequestedSamplerEngineName() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  return requestedSamplerEngineName;
}

std::string Synth::GetResolvedSourceFormat() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  return engine ? engine->GetResolvedSourceFormat() : std::string();
}

std::string Synth::GetLastInitStatus() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  return lastInitStatus;
}

DWORD Synth::GetActiveVoiceStats(DWORD *channelCounts, int count) {
  if (channelCounts && count > 0) {
    for (int i = 0; i < count; ++i)
      channelCounts[i] = 0;
  }

  compat::LockGuard<compat::Mutex> lock(synthMutex);
  if (!engine)
    return 0;
  return engine->GetActiveVoiceStats(channelCounts, count);
}

SamplerDiagnostics Synth::GetSamplerDiagnostics() {
  compat::LockGuard<compat::Mutex> lock(synthMutex);
  SamplerDiagnostics diagnostics;
  if (engine)
    diagnostics = engine->GetDiagnostics();

  diagnostics.queuedMidiEvents = lastPendingQueueDepth;
  diagnostics.deferredMidiEvents = lastDeferredQueueDepth;
  diagnostics.maxQueuedMidiEvents =
      maxObservedQueueDepth.load(std::memory_order_relaxed);
  diagnostics.droppedNoteOnEvents =
      droppedNoteOnEvents.load(std::memory_order_relaxed);
  diagnostics.droppedNonNoteEvents =
      droppedNonNoteEvents.load(std::memory_order_relaxed);
  diagnostics.eventsProcessedThisBlock = lastEventsProcessed;
  diagnostics.noteOnEventsThisBlock = lastNoteOnsAttempted;
  diagnostics.noteOnStartedThisBlock = lastNoteOnsStarted;
  diagnostics.noteOnDroppedThisBlock = lastNoteOnsDropped;
  diagnostics.noteOffEventsThisBlock = lastNoteOffsProcessed;
  diagnostics.asyncPendingNoteOns = lastAsyncPendingNoteOns;
  diagnostics.asyncStartedThisBlock = lastSchedulerDueEvents;
  diagnostics.asyncDroppedThisBlock = lastAsyncDropped;
  diagnostics.asyncCoalescedThisBlock = lastAsyncCoalesced;
  diagnostics.overloadNoteOnsDroppedThisBlock = lastOverloadNoteOnsDropped;
  diagnostics.staleNoteOnsDroppedThisBlock = lastStaleNoteOnsDropped;
  diagnostics.preScheduleDropsThisBlock = lastPreScheduleDrops;
  diagnostics.postScheduleDropsThisBlock = lastPostScheduleDrops;
  diagnostics.catchupPreventedThisBlock = lastCatchupPrevented;
  diagnostics.asyncMaxQueuedNoteOns =
      maxAsyncQueueDepth.load(std::memory_order_relaxed);
  diagnostics.asyncQueueAgeMs = lastAsyncQueueAgeMs;
  diagnostics.asyncLagState = lastAsyncLagState;
  diagnostics.asyncNoteStartsEnabled = IsSchedulerEnabledLocked() ? 1u : 0u;
  diagnostics.eventProcessorThreadActive =
      eventProcessorThreadActive.load(std::memory_order_relaxed);
  diagnostics.schedulerSliceFrames = lastSchedulerBlockFrames;
  diagnostics.schedulerDueEventsThisBlock = lastSchedulerDueEvents;
  diagnostics.schedulerLateEventsThisBlock = lastSchedulerLateEvents;
  diagnostics.schedulerLagSamples = lastSchedulerLagSamples;
  diagnostics.schedulerPendingSameKeyTransitions =
      lastSchedulerPendingSameKeyTransitions;
  diagnostics.schedulerMaxSameKeyQueueDepth = lastSchedulerMaxSameKeyQueueDepth;
  diagnostics.schedulerNoteOnsCoalescedThisBlock =
      lastSchedulerNoteOnsCoalesced;
  diagnostics.schedulerNoteOffsAppliedThisBlock =
      lastSchedulerNoteOffsApplied;
  diagnostics.schedulerNoteOffsCoalescedThisBlock =
      lastSchedulerNoteOffsCoalesced;
  diagnostics.schedulerNoteOffsCanceledThisBlock =
      lastSchedulerNoteOffsCanceled;
  diagnostics.schedulerReleaseControlsAppliedThisBlock =
      lastSchedulerReleaseControlsApplied;
  diagnostics.schedulerRenderSplitsThisBlock = lastSchedulerRenderSplits;
  diagnostics.schedulerSliceMs = lastSchedulerBlockMs;
  diagnostics.schedulerLagMs =
      (currentRenderSampleRate > 0)
          ? (float)((double)lastSchedulerLagSamples * 1000.0 /
                    (double)currentRenderSampleRate)
          : 0.0f;
  diagnostics.schedulerBlockStartSample = currentRenderBlockStartSample;
  diagnostics.midiProcessMs = lastMidiProcessMs;
  diagnostics.sampleRenderMs = lastSampleRenderEstimateMs;
  diagnostics.overloadState = overloadState;
  diagnostics.consecutiveOverloadBlocks = consecutiveOverloadBlocks;
  diagnostics.runtimeReloadCount = runtimeReloadCount;
  diagnostics.accurateClockResetCount = accurateClockResetCount;
  diagnostics.schedulerStatePreservedCount = schedulerStatePreservedCount;
  diagnostics.lastRestartReason = lastRestartReason;
  diagnostics.accurateHardOverloadEntries = accurateHardOverloadEntries;
  diagnostics.accurateHardOverloadRecoveries = accurateHardOverloadRecoveries;
  diagnostics.accurateWorkerBlockedCount = accurateWorkerBlockedCount;
  diagnostics.accuratePeakPendingEvents = accuratePeakPendingEvents;
  diagnostics.accuratePeakDeferredEvents = accuratePeakDeferredEvents;
  diagnostics.accuratePeakScheduledEvents = accuratePeakScheduledEvents;
  diagnostics.perfCountersEnabled = SVMS_PERF_DEBUG ? 1u : 0u;
  diagnostics.schedulerCacheRebuilds = perfSchedulerCacheRebuilds;
  diagnostics.schedulerTrimHeapTombstonePrunes =
      perfSchedulerTrimHeapTombstonePrunes;
  diagnostics.asyncCoalescedThisBlock += accurateControlsCoalesced;
  return diagnostics;
}
