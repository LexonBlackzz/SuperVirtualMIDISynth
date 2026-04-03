#include "VirtuallySuperSamplerEngine.h"

#include <algorithm>
#include <atomic>
#include <math.h>
#include <string.h>

namespace {

static uint8_t ClampMidiData(int value) {
  if (value < 0)
    return 0;
  if (value > 127)
    return 127;
  return (uint8_t)value;
}

static uint16_t ClampPitchBend14(int value) {
  if (value < 0)
    return 0;
  if (value > 0x3FFF)
    return 0x3FFFu;
  return (uint16_t)value;
}

} // namespace

VirtuallySuperSamplerEngine::VirtuallySuperSamplerEngine()
    : stateCode_(SamplerRuntimeStateCode::UNINITIALIZED),
      errorCode_(SamplerErrorCode::NONE),
      idleFastPathHits_(0),
      noteOnEventsThisBlock_(0),
      noteOffEventsThisBlock_(0),
      renderCursorSample_(0),
      prototype_(), 
      soundFontRuntime_(), 
      initialized_(false), 
      sampleRate_(44100),
      resolvedSourcePath_(), 
      resolvedSourceFormat_(), 
      runtimeSettings_(),
      diagnostics_(),
      errorCodeUI_(SamplerErrorCode::NONE) {
  runtimeSettings_.velocityCurve = 2.4f;
  runtimeSettings_.velocityFloor = 0.0f;
  runtimeSettings_.velocityIgnoreBelow = 0;
  runtimeSettings_.asyncNoteStarts = true;
  runtimeSettings_.eventTimingMode = EventTimingMode::ACCURATE;
}

VirtuallySuperSamplerEngine::~VirtuallySuperSamplerEngine() { 
  Shutdown(true); 
}

bool VirtuallySuperSamplerEngine::Initialize(const SamplerInitParams &params) {
  // Note: Initialize is called from UI thread, so mutex is safe
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  Shutdown(false);

  runtimeSettings_ = params.runtimeSettings;
  sampleRate_ = params.sampleRate;
  resolvedSourcePath_ = params.sourcePath;
  resolvedSourceFormat_ = DetectSourceFormat(params.sourcePath);

  const bool wantsSf2 = resolvedSourceFormat_ == "sf2";
  std::string initWarning;
  if (wantsSf2) {
    if (!soundFontRuntime_.Load(params.sourcePath.c_str(),
                                params.sampleRate > 0 ? (uint32_t)params.sampleRate
                                                      : 44100u,
                                &initWarning)) {
      initialized_ = false;
      SetState(SamplerRuntimeStateCode::FAILED,
               SamplerErrorCode::INIT_FAILED,
               initWarning.empty()
                   ? "VirtuallySuper failed to load the requested SoundFont."
                   : initWarning.c_str());
      diagnostics_.failedSampleCount = 1;
      return false;
    }
  } else {
    soundFontRuntime_.Reset();
  }

  virtuallysuper::EngineConfig config;
  config.scheduler.scheduledCapacity =
      params.maxVoices > 0 ? (uint32_t)(params.maxVoices * 8) : 4096u;
  if (config.scheduler.scheduledCapacity <
      virtuallysuper::kDefaultScheduledCapacity)
    config.scheduler.scheduledCapacity =
        virtuallysuper::kDefaultScheduledCapacity;
  config.exact.maxVoices =
      params.maxVoices > 0 ? (uint32_t)params.maxVoices
                           : virtuallysuper::kDefaultExactVoiceCapacity;
  config.exact.sampleRate =
      params.sampleRate > 0 ? (uint32_t)params.sampleRate : 44100u;
  config.grouped.maxGroups =
      std::max<uint32_t>(virtuallysuper::kDefaultGroupedCapacity,
                         config.exact.maxVoices / 2u);
  config.grouped.sampleRate = config.exact.sampleRate;
  config.density.maxObjects =
      std::max<uint32_t>(virtuallysuper::kDefaultDensityCapacity,
                         config.exact.maxVoices / 4u);

  initialized_ = prototype_.Initialize(config);
  if (initialized_) {
    prototype_.GetExactSystem().SetSoundFontRuntime(
        soundFontRuntime_.IsLoaded() ? &soundFontRuntime_ : 0);
    SetState(SamplerRuntimeStateCode::READY, SamplerErrorCode::NONE,
             soundFontRuntime_.IsLoaded()
                 ? initWarning.c_str()
                 : "VirtuallySuper host prototype active. Synthetic exact/grouped/density rendering is enabled.");
  } else {
    SetState(SamplerRuntimeStateCode::FAILED,
             SamplerErrorCode::INIT_FAILED,
             "VirtuallySuper failed to initialize its prototype runtime.");
  }
  ResetPerBlockStats();
  return initialized_;
}

void VirtuallySuperSamplerEngine::Shutdown(bool waitForThreads) {
  (void)waitForThreads;
  
  // Atomic shutdown - no mutex needed
  initialized_ = false;
  prototype_.Reset();
  prototype_.GetExactSystem().SetSoundFontRuntime(0);
  soundFontRuntime_.Reset();
  resolvedSourcePath_.clear();
  resolvedSourceFormat_.clear();
  
  // Reset atomic render window
  renderWindow_.blockStartSample.store(0, std::memory_order_release);
  renderWindow_.blockFrames.store(0, std::memory_order_release);
  renderWindow_.sampleRate.store(0, std::memory_order_release);
  renderWindow_.blockStartQpc.store(0, std::memory_order_release);
  renderWindow_.blockEndQpc.store(0, std::memory_order_release);
  renderWindow_.quantized.store(false, std::memory_order_release);
  renderWindow_.valid.store(false, std::memory_order_release);
  
  renderCursorSample_.store(0, std::memory_order_release);
  idleFastPathHits_.store(0, std::memory_order_release);
  
  SetState(SamplerRuntimeStateCode::UNINITIALIZED, SamplerErrorCode::NONE,
           "VirtuallySuper is not initialized.");
  ResetPerBlockStats();
}

void VirtuallySuperSamplerEngine::Reset() {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (!initialized_)
    return;
  soundFontRuntime_.ResetChannels();
  prototype_.Reset();
  renderCursorSample_.store(0, std::memory_order_release);
  SetState(SamplerRuntimeStateCode::READY, SamplerErrorCode::NONE,
           "VirtuallySuper reset cleanly.");
  ResetPerBlockStats();
}

void VirtuallySuperSamplerEngine::ReloadRuntimeSettings(
    const RuntimeSettings &settings) {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  runtimeSettings_ = settings;
}

void VirtuallySuperSamplerEngine::SetRenderWindow(
    unsigned long long blockStartSample, int blockFrames, int sampleRate,
    long long blockStartQpc, long long blockEndQpc,
    bool quantizedByPollingRate) {
  // Lock-free update using atomics - safe from audio thread
  renderWindow_.blockStartSample.store(blockStartSample, std::memory_order_release);
  renderWindow_.blockFrames.store(blockFrames, std::memory_order_release);
  renderWindow_.sampleRate.store(sampleRate, std::memory_order_release);
  renderWindow_.blockStartQpc.store(blockStartQpc, std::memory_order_release);
  renderWindow_.blockEndQpc.store(blockEndQpc, std::memory_order_release);
  renderWindow_.quantized.store(quantizedByPollingRate, std::memory_order_release);
  renderWindow_.valid.store(true, std::memory_order_release);
}

void VirtuallySuperSamplerEngine::BeginRenderBlock() {
  // Lock-free reset of per-block stats
  noteOnEventsThisBlock_.store(0, std::memory_order_relaxed);
  noteOffEventsThisBlock_.store(0, std::memory_order_relaxed);
  renderCursorSample_.store((long long)renderWindow_.blockStartSample.load(std::memory_order_acquire), 
                            std::memory_order_release);
  
  if (initialized_) {
    SetState(SamplerRuntimeStateCode::ACTIVE, SamplerErrorCode::NONE,
             diagnostics_.lastWarning.c_str());
  }
}

void VirtuallySuperSamplerEngine::ProcessMidiEvent(const MidiEvent &event) {
  // Lock-free event processing for audio thread
  if (!initialized_)
    return;

  const virtuallysuper::NormalizedEvent normalized = ConvertMidiEvent(event);
  if (normalized.kind == virtuallysuper::EventKind::NoteOn &&
      normalized.mappedVelocity == 0) {
    return;
  }
  
  // Submit event to scheduler's lock-free ingress queue
  const virtuallysuper::ScheduleDecision decision =
      prototype_.SubmitEvent(normalized);

  if (normalized.kind == virtuallysuper::EventKind::NoteOn) {
    noteOnEventsThisBlock_.fetch_add(1, std::memory_order_relaxed);
  } else if (normalized.kind == virtuallysuper::EventKind::NoteOff) {
    noteOffEventsThisBlock_.fetch_add(1, std::memory_order_relaxed);
  }

  if (decision == virtuallysuper::ScheduleDecision::Dropped &&
      normalized.kind == virtuallysuper::EventKind::NoteOn) {
    // Atomic increment of dropped events
    diagnostics_.droppedNoteOnEvents++;  // Note: diagnostics read from UI thread
  }
}

void VirtuallySuperSamplerEngine::Render(float *output, int numFrames) {
  if (!output || numFrames <= 0)
    return;

  // Fast path - check atomics without mutex
  if (!initialized_) {
    memset(output, 0, (size_t)numFrames * 2u * sizeof(float));
    return;
  }

  // Check render window validity (atomic read)
  if (!renderWindow_.valid.load(std::memory_order_acquire)) {
    memset(output, 0, (size_t)numFrames * 2u * sizeof(float));
    SetState(
        SamplerRuntimeStateCode::FAILED,
        SamplerErrorCode::MISSING_RENDER_CONTEXT,
        "VirtuallySuper did not receive a render timeline context from the "
        "host.");
    diagnostics_.sampleRenderMs = 0.0f;
    diagnostics_.virtuallySuperIdleFastPathHits = idleFastPathHits_.load(std::memory_order_relaxed);
    return;
  }

  // Check for idle fast path (no active voices)
  if (prototype_.CanIdleFastPath()) {
    memset(output, 0, (size_t)numFrames * 2u * sizeof(float));
    idleFastPathHits_.fetch_add(1, std::memory_order_relaxed);
    
    // Update diagnostics atomically
    diagnostics_.sampleRenderMs = 0.0f;
    diagnostics_.eventsProcessedThisBlock = 0;
    diagnostics_.noteOnEventsThisBlock = noteOnEventsThisBlock_.load(std::memory_order_relaxed);
    diagnostics_.noteOffEventsThisBlock = noteOffEventsThisBlock_.load(std::memory_order_relaxed);
    diagnostics_.queuedMidiEvents = 0;
    diagnostics_.schedulerDueEventsThisBlock = 0;
    diagnostics_.schedulerPendingSameKeyTransitions = 0;
    diagnostics_.schedulerMaxSameKeyQueueDepth =
        prototype_.GetScheduler().GetStats().maxTransitionQueueDepth;
    diagnostics_.schedulerNoteOnsCoalescedThisBlock =
        prototype_.GetScheduler().GetStats().coalescedEvents;
    diagnostics_.accuratePeakScheduledEvents =
        prototype_.GetScheduler().GetStats().maxScheduledDepth;
    UpdateSoundFontDiagnostics();
    diagnostics_.overloadState = 0;
    diagnostics_.virtuallySuperExactVoices = 0;
    diagnostics_.virtuallySuperReleasedExactVoices = 0;
    diagnostics_.virtuallySuperGroupedObjects = 0;
    diagnostics_.virtuallySuperDensityObjects = 0;
    diagnostics_.virtuallySuperVoiceEquivalent = 0;
    diagnostics_.virtuallySuperPressureLevel = 0;
    diagnostics_.virtuallySuperIdleFastPathHits = idleFastPathHits_.load(std::memory_order_relaxed);
    diagnostics_.virtuallySuperExactVisitedThisBlock = 0;
    diagnostics_.virtuallySuperGroupedVisitedThisBlock = 0;
    diagnostics_.virtuallySuperDensityVisitedThisBlock = 0;
    diagnostics_.samplerStateCode = (unsigned int)stateCode_.load(std::memory_order_relaxed);
    diagnostics_.samplerErrorCode = (unsigned int)errorCode_.load(std::memory_order_relaxed);
    return;
  }

  // Full render path
  memset(output, 0, (size_t)numFrames * 2u * sizeof(float));

  LARGE_INTEGER freq = {};
  LARGE_INTEGER start = {};
  LARGE_INTEGER end = {};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);

  const long long blockStartSample = renderCursorSample_.load(std::memory_order_acquire);
  const long long blockEndSample = blockStartSample + numFrames;

  // Flush ingress queue (lock-free)
  prototype_.FlushPendingIngress(virtuallysuper::kDefaultIngressCapacity);

  int64_t renderUntilSample = blockEndSample;
  const size_t applied = prototype_.ApplyScheduledWindow(
      blockStartSample, blockEndSample, blockEndSample, &renderUntilSample);

  // Render block using SIMD
  prototype_.RenderBlock(output, numFrames, sampleRate_);
  renderCursorSample_.store(blockEndSample, std::memory_order_release);

  QueryPerformanceCounter(&end);
  diagnostics_.sampleRenderMs =
      freq.QuadPart > 0
          ? (float)((double)(end.QuadPart - start.QuadPart) * 1000.0 /
                    (double)freq.QuadPart)
          : 0.0f;

  // Update diagnostics with atomic reads
  diagnostics_.eventsProcessedThisBlock = (unsigned int)applied;
  diagnostics_.noteOnEventsThisBlock = noteOnEventsThisBlock_.load(std::memory_order_relaxed);
  diagnostics_.noteOffEventsThisBlock = noteOffEventsThisBlock_.load(std::memory_order_relaxed);
  diagnostics_.queuedMidiEvents = prototype_.GetScheduler().GetIngressCount();
  diagnostics_.schedulerDueEventsThisBlock = (unsigned int)applied;

  const virtuallysuper::SchedulerStats &schedulerStats =
      prototype_.GetScheduler().GetStats();
  diagnostics_.schedulerMaxSameKeyQueueDepth =
      schedulerStats.maxTransitionQueueDepth;
  diagnostics_.schedulerNoteOnsCoalescedThisBlock =
      schedulerStats.coalescedEvents;
  diagnostics_.accuratePeakScheduledEvents = schedulerStats.maxScheduledDepth;

  const virtuallysuper::ExactStats &exactStats =
      prototype_.GetExactSystem().GetStats();
  diagnostics_.noteOnStartedThisBlock = exactStats.noteOnsApplied;
  diagnostics_.overloadNoteOnsDroppedThisBlock = exactStats.steals;

  const virtuallysuper::TelemetrySnapshot &snapshot =
      prototype_.GetLatestTelemetrySnapshot();
  UpdateSoundFontDiagnostics();
  diagnostics_.schedulerPendingSameKeyTransitions =
      snapshot.schedulerQueuedEvents;
  diagnostics_.overloadState = snapshot.overloadPressureLevel;
  diagnostics_.virtuallySuperExactVoices = snapshot.exactVoices;
  diagnostics_.virtuallySuperReleasedExactVoices =
      snapshot.releasedExactVoices;
  diagnostics_.virtuallySuperGroupedObjects = snapshot.groupedObjects;
  diagnostics_.virtuallySuperDensityObjects = snapshot.densityObjects;
  diagnostics_.virtuallySuperVoiceEquivalent = snapshot.voiceEquivalent;
  diagnostics_.virtuallySuperPressureLevel = snapshot.overloadPressureLevel;
  diagnostics_.virtuallySuperIdleFastPathHits = idleFastPathHits_.load(std::memory_order_relaxed);
  diagnostics_.virtuallySuperExactVisitedThisBlock =
      prototype_.GetLatestRenderStats().exactVoicesVisited;
  diagnostics_.virtuallySuperGroupedVisitedThisBlock =
      prototype_.GetLatestRenderStats().groupedObjectsVisited;
  diagnostics_.virtuallySuperDensityVisitedThisBlock =
      prototype_.GetLatestRenderStats().densityObjectsVisited;
  diagnostics_.samplerStateCode = (unsigned int)stateCode_.load(std::memory_order_relaxed);
  diagnostics_.samplerErrorCode = (unsigned int)errorCode_.load(std::memory_order_relaxed);
  diagnostics_.schedulerBlockStartSample = renderWindow_.blockStartSample.load(std::memory_order_acquire);
  
  if (stateCode_.load(std::memory_order_relaxed) != SamplerRuntimeStateCode::FAILED) {
    SetState(SamplerRuntimeStateCode::ACTIVE, SamplerErrorCode::NONE,
             soundFontRuntime_.IsLoaded()
                 ? "VirtuallySuper native SF2 exact tier active."
                 : "VirtuallySuper host prototype active. Synthetic exact/grouped/density rendering is enabled.");
  }
}

std::string VirtuallySuperSamplerEngine::GetResolvedSourcePath() const {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  return resolvedSourcePath_;
}

std::string VirtuallySuperSamplerEngine::GetResolvedSourceFormat() const {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  return resolvedSourceFormat_;
}

std::string VirtuallySuperSamplerEngine::GetEngineName() const {
  return "virtuallysuper";
}

DWORD VirtuallySuperSamplerEngine::GetActiveVoiceStats(DWORD *channelCounts,
                                                       int count) const {
  if (channelCounts && count > 0)
    std::fill(channelCounts, channelCounts + count, 0u);

  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (!initialized_)
    return 0;

  DWORD total = 0;
  for (uint32_t channel = 0; channel < virtuallysuper::kChannelCount; ++channel) {
    DWORD channelActive = 0;
    for (uint32_t note = 0; note < virtuallysuper::kNoteCount; ++note) {
      uint32_t handle = prototype_.GetExactSystem().GetKeyHead(channel, note);
      while (handle != virtuallysuper::kInvalidVoiceHandle) {
        const virtuallysuper::ExactVoice *voice =
            prototype_.GetExactSystem().GetVoice(handle);
        if (!voice)
          break;
        if (voice->state == virtuallysuper::ExactLifecycleState::Active)
          ++channelActive;
        handle = voice->nextSameKey;
      }
    }
    if (channelCounts && (int)channel < count)
      channelCounts[channel] = channelActive;
    total += channelActive;
  }
  return total;
}

SamplerDiagnostics VirtuallySuperSamplerEngine::GetDiagnostics() const {
  // Diagnostics are mostly atomic now, but we still need mutex for some fields
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  SamplerDiagnostics copy = diagnostics_;
  for (uint32_t channel = 0; channel < 16; ++channel)
    copy.pitchBendRange[channel] = soundFontRuntime_.GetPitchBendRange((uint8_t)channel);
  copy.samplerStateCode = (unsigned int)stateCode_.load(std::memory_order_relaxed);
  copy.samplerErrorCode = (unsigned int)errorCode_.load(std::memory_order_relaxed);
  return copy;
}

uint8_t VirtuallySuperSamplerEngine::MapMidiVelocity(int velocity) const {
  if (velocity <= 0)
    return 0;
  if (velocity >= 127)
    return 127;

  int ignoreBelow = runtimeSettings_.velocityIgnoreBelow;
  if (ignoreBelow < 0)
    ignoreBelow = 0;
  if (ignoreBelow > 126)
    ignoreBelow = 126;
  if (velocity <= ignoreBelow)
    return 0;

  float normalized =
      (float)(velocity - ignoreBelow) / (127.0f - (float)ignoreBelow);
  float curve = runtimeSettings_.velocityCurve;
  if (curve < 0.25f)
    curve = 0.25f;
  if (curve > 6.0f)
    curve = 6.0f;

  float floor = runtimeSettings_.velocityFloor;
  if (floor < 0.0f)
    floor = 0.0f;
  if (floor > 0.5f)
    floor = 0.5f;

  const float mapped = floor + (1.0f - floor) * powf(normalized, curve);
  int midi = (int)(mapped * 127.0f + 0.5f);
  if (midi < 1)
    midi = 1;
  if (midi > 127)
    midi = 127;
  return (uint8_t)midi;
}

virtuallysuper::NormalizedEvent
VirtuallySuperSamplerEngine::ConvertMidiEvent(const MidiEvent &event) const {
  virtuallysuper::NormalizedEvent normalized;
  normalized.channel = (uint8_t)event.channel;
  normalized.note = ClampMidiData(event.data1);
  normalized.value = ClampMidiData(event.data1);
  normalized.velocity = ClampMidiData(event.data2);
  normalized.mappedVelocity = normalized.velocity;
  normalized.sequence = event.sequence;
  normalized.targetSample = event.targetSample;

  switch (event.type) {
  case MidiEvent::NOTE_ON:
    normalized.kind = virtuallysuper::EventKind::NoteOn;
    normalized.mappedVelocity = MapMidiVelocity(event.data2);
    break;
  case MidiEvent::NOTE_OFF:
    normalized.kind = virtuallysuper::EventKind::NoteOff;
    break;
  case MidiEvent::PROGRAM_CHANGE:
    normalized.kind = virtuallysuper::EventKind::ProgramChange;
    break;
  case MidiEvent::CONTROL_CHANGE:
    normalized.kind = virtuallysuper::EventKind::ControlChange;
    break;
  case MidiEvent::PITCH_BEND:
    normalized.kind = virtuallysuper::EventKind::PitchBend;
    {
      const uint16_t bend = ClampPitchBend14(event.data1);
      normalized.note = 0;
      normalized.value = (uint8_t)(bend & 0x7Fu);
      normalized.velocity = (uint8_t)((bend >> 7) & 0x7Fu);
      normalized.mappedVelocity = 0;
    }
    break;
  case MidiEvent::RESET:
    normalized.kind = virtuallysuper::EventKind::Reset;
    break;
  }

  return normalized;
}

void VirtuallySuperSamplerEngine::ResetPerBlockStats() {
  // Called from UI thread or with mutex held
  std::string lastWarning = diagnostics_.lastWarning;
  diagnostics_ = SamplerDiagnostics();
  diagnostics_.lastWarning = lastWarning;
  UpdateSoundFontDiagnostics();
  diagnostics_.virtuallySuperIdleFastPathHits = idleFastPathHits_.load(std::memory_order_relaxed);
  noteOnEventsThisBlock_.store(0, std::memory_order_relaxed);
  noteOffEventsThisBlock_.store(0, std::memory_order_relaxed);
  diagnostics_.samplerStateCode = (unsigned int)stateCode_.load(std::memory_order_relaxed);
  diagnostics_.samplerErrorCode = (unsigned int)errorCode_.load(std::memory_order_relaxed);
}

void VirtuallySuperSamplerEngine::SetState(SamplerRuntimeStateCode stateCode,
                                           SamplerErrorCode errorCode,
                                           const char *warningText) {
  // Atomic state update - safe from any thread
  stateCode_.store(stateCode, std::memory_order_release);
  errorCode_.store(errorCode, std::memory_order_release);
  diagnostics_.samplerStateCode = (unsigned int)stateCode;
  diagnostics_.samplerErrorCode = (unsigned int)errorCode;
  if (warningText)
    diagnostics_.lastWarning = warningText;
}

void VirtuallySuperSamplerEngine::UpdateSoundFontDiagnostics() {
  // Called from render thread - use atomic updates
  diagnostics_.loadedSampleCount = soundFontRuntime_.GetSampleCount();
  diagnostics_.virtuallySuperLoadedPresets = soundFontRuntime_.GetPresetCount();
  diagnostics_.virtuallySuperLoadedRegions = soundFontRuntime_.GetRegionCount();
  diagnostics_.virtuallySuperExactMode = soundFontRuntime_.IsLoaded() ? 1u : 0u;
}
