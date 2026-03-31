#include "VirtuallySuperSamplerEngine.h"

#include <algorithm>

namespace {

static uint8_t ClampMidiData(int value) {
  if (value < 0)
    return 0;
  if (value > 127)
    return 127;
  return (uint8_t)value;
}

} // namespace

VirtuallySuperSamplerEngine::VirtuallySuperSamplerEngine()
    : prototype_(), initialized_(false), sampleRate_(44100),
      resolvedSourcePath_(), resolvedSourceFormat_(), runtimeSettings_(),
      diagnostics_(), noteOnEventsThisBlock_(0), noteOffEventsThisBlock_(0) {
  runtimeSettings_.velocityCurve = 2.4f;
  runtimeSettings_.velocityFloor = 0.0f;
  runtimeSettings_.velocityIgnoreBelow = 0;
  runtimeSettings_.asyncNoteStarts = true;
  runtimeSettings_.eventTimingMode = EventTimingMode::ACCURATE;
}

VirtuallySuperSamplerEngine::~VirtuallySuperSamplerEngine() { Shutdown(true); }

bool VirtuallySuperSamplerEngine::Initialize(const SamplerInitParams &params) {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  Shutdown(false);

  runtimeSettings_ = params.runtimeSettings;
  sampleRate_ = params.sampleRate;
  resolvedSourcePath_ = params.sourcePath;
  resolvedSourceFormat_ = DetectSourceFormat(params.sourcePath);

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
  config.grouped.maxGroups =
      std::max<uint32_t>(virtuallysuper::kDefaultGroupedCapacity,
                         config.exact.maxVoices / 2u);
  config.density.maxObjects =
      std::max<uint32_t>(virtuallysuper::kDefaultDensityCapacity,
                         config.exact.maxVoices / 4u);

  initialized_ = prototype_.Initialize(config);
  ResetPerBlockStatsLocked();
  return initialized_;
}

void VirtuallySuperSamplerEngine::Shutdown(bool waitForThreads) {
  (void)waitForThreads;
  initialized_ = false;
  prototype_.Reset();
  resolvedSourcePath_.clear();
  resolvedSourceFormat_.clear();
  ResetPerBlockStatsLocked();
}

void VirtuallySuperSamplerEngine::Reset() {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (!initialized_)
    return;
  prototype_.Reset();
  ResetPerBlockStatsLocked();
}

void VirtuallySuperSamplerEngine::ReloadRuntimeSettings(
    const RuntimeSettings &settings) {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  runtimeSettings_ = settings;
}

void VirtuallySuperSamplerEngine::BeginRenderBlock() {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  ResetPerBlockStatsLocked();
}

void VirtuallySuperSamplerEngine::ProcessMidiEvent(const MidiEvent &event) {
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (!initialized_)
    return;

  const virtuallysuper::NormalizedEvent normalized = ConvertMidiEvent(event);
  const virtuallysuper::ScheduleDecision decision =
      prototype_.SubmitEvent(normalized);

  if (normalized.kind == virtuallysuper::EventKind::NoteOn)
    ++noteOnEventsThisBlock_;
  else if (normalized.kind == virtuallysuper::EventKind::NoteOff)
    ++noteOffEventsThisBlock_;

  if (decision == virtuallysuper::ScheduleDecision::Dropped &&
      normalized.kind == virtuallysuper::EventKind::NoteOn) {
    ++diagnostics_.droppedNoteOnEvents;
  }
}

void VirtuallySuperSamplerEngine::Render(float *output, int numFrames) {
  if (!output || numFrames <= 0)
    return;

  std::fill(output, output + numFrames * 2, 0.0f);

  compat::LockGuard<compat::Mutex> lock(engineMutex);
  if (!initialized_)
    return;

  LARGE_INTEGER freq = {};
  LARGE_INTEGER start = {};
  LARGE_INTEGER end = {};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);

  prototype_.FlushPendingIngress(virtuallysuper::kDefaultIngressCapacity);

  int64_t renderUntilSample = numFrames;
  const size_t applied =
      prototype_.ApplyScheduledWindow(0, numFrames, numFrames, &renderUntilSample);

  prototype_.RenderBlock(output, numFrames, sampleRate_);

  QueryPerformanceCounter(&end);
  diagnostics_.sampleRenderMs =
      freq.QuadPart > 0
          ? (float)((double)(end.QuadPart - start.QuadPart) * 1000.0 /
                    (double)freq.QuadPart)
          : 0.0f;

  diagnostics_.eventsProcessedThisBlock = (unsigned int)applied;
  diagnostics_.noteOnEventsThisBlock = noteOnEventsThisBlock_;
  diagnostics_.noteOffEventsThisBlock = noteOffEventsThisBlock_;
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
  diagnostics_.loadedSampleCount = snapshot.groupedObjects;
  diagnostics_.failedSampleCount = snapshot.densityObjects;
  diagnostics_.schedulerPendingSameKeyTransitions =
      snapshot.schedulerQueuedEvents;
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
  compat::LockGuard<compat::Mutex> lock(engineMutex);
  SamplerDiagnostics copy = diagnostics_;
  copy.lastWarning =
      "VirtuallySuper prototype shell active. Deterministic prototype audio "
      "rendering is enabled, but real SoundFont/sample playback is not "
      "implemented yet.";
  return copy;
}

virtuallysuper::NormalizedEvent
VirtuallySuperSamplerEngine::ConvertMidiEvent(const MidiEvent &event) const {
  virtuallysuper::NormalizedEvent normalized;
  normalized.channel = (uint8_t)event.channel;
  normalized.note = ClampMidiData(event.data1);
  normalized.value = ClampMidiData(event.data1);
  normalized.velocity = ClampMidiData(event.data2);
  normalized.sequence = event.sequence;
  normalized.targetSample = event.targetSample;

  switch (event.type) {
  case MidiEvent::NOTE_ON:
    normalized.kind = virtuallysuper::EventKind::NoteOn;
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
    break;
  case MidiEvent::RESET:
    normalized.kind = virtuallysuper::EventKind::Reset;
    break;
  }

  return normalized;
}

void VirtuallySuperSamplerEngine::ResetPerBlockStatsLocked() {
  diagnostics_ = SamplerDiagnostics();
  noteOnEventsThisBlock_ = 0;
  noteOffEventsThisBlock_ = 0;
}
