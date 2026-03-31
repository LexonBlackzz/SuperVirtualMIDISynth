#include "VirtuallySuperExact.h"

#include <math.h>

namespace {

static float MidiNoteToFrequencyHz(uint8_t note) {
  return 440.0f * powf(2.0f, ((float)note - 69.0f) * (1.0f / 12.0f));
}

static float VelocityToGain(uint8_t velocity) {
  return ((float)velocity / 127.0f) * 0.045f;
}

static float ExactPan(uint32_t channel, uint32_t note) {
  const uint32_t index = (channel * 17u + note * 13u) & 15u;
  return ((float)index / 7.5f) - 1.0f;
}

static void ExactStereoGains(float pan, float *left, float *right) {
  const float clamped = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
  *left = 0.5f * (1.0f - clamped * 0.35f);
  *right = 0.5f * (1.0f + clamped * 0.35f);
}

} // namespace

namespace virtuallysuper {

ExactSystem::ExactSystem()
    : config_(), initialized_(false), nextVoiceId_(1), soundFont_(0),
      voices_(), freeList_(), freeCount_(0), stats_() {}

bool ExactSystem::Initialize(const ExactConfig &config) {
  if (config.maxVoices == 0)
    return false;

  config_ = config;
  voices_.assign(config_.maxVoices, ExactVoice());
  freeList_.assign(config_.maxVoices, 0);
  initialized_ = true;
  Reset();
  return true;
}

void ExactSystem::SetSoundFontRuntime(SoundFontRuntime *soundFont) {
  soundFont_ = soundFont;
}

void ExactSystem::Reset() {
  nextVoiceId_ = 1;
  freeCount_ = (uint32_t)voices_.size();
  stats_ = ExactStats();

  for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
    for (uint32_t note = 0; note < kNoteCount; ++note) {
      generationCounters_[channel][note] = 0;
      keyHeads_[channel][note] = kInvalidVoiceHandle;
    }
  }

  for (uint32_t i = 0; i < 5; ++i)
    queues_[i] = QueueState();

  for (uint32_t i = 0; i < freeCount_; ++i) {
    voices_[i] = ExactVoice();
    freeList_[i] = freeCount_ - 1U - i;
  }
}

bool ExactSystem::ApplyEvent(const NormalizedEvent &event) {
  if (!initialized_ || event.channel >= kChannelCount)
    return false;

  switch (event.kind) {
  case EventKind::NoteOn:
    return NoteOn(event);
  case EventKind::NoteOff:
    return NoteOff(event.channel, event.note) > 0;
  case EventKind::ProgramChange:
    return soundFont_ ? soundFont_->HandleEvent(event) : true;
  case EventKind::PitchBend:
    if (soundFont_ && soundFont_->HandleEvent(event))
      RefreshChannelVoices(event.channel);
    return true;
  case EventKind::ControlChange:
    {
      const uint8_t controller = event.value;
      const bool handled = soundFont_ ? soundFont_->HandleEvent(event) : false;
      if (controller == 64 && soundFont_ &&
          !soundFont_->IsSustainEnabled(event.channel))
        ReleaseSustainedVoices(event.channel);
      else if (controller == 120)
        ReleaseAllChannelVoices(event.channel, true);
      else if (controller == 123)
        ReleaseAllChannelVoices(event.channel, false);
      else if (controller == 7 || controller == 10 || controller == 11 ||
               controller == 121) {
        RefreshChannelVoices(event.channel);
      }
      return handled || controller == 64 || controller == 120 ||
             controller == 123 || controller == 7 || controller == 10 ||
             controller == 11 || controller == 121;
    }
  case EventKind::Reset:
    if (soundFont_)
      soundFont_->HandleEvent(event);
    Reset();
    return true;
  default:
    return false;
  }
}

bool ExactSystem::NoteOn(const NormalizedEvent &event) {
  if (!initialized_ || event.channel >= kChannelCount || event.note >= kNoteCount)
    return false;

  uint32_t handle = kInvalidVoiceHandle;
  bool stolen = false;
  if (!AllocateVoiceHandle(&handle, &stolen))
    return false;

  ActivateVoice(handle, event);
  if (voices_[handle].state == ExactLifecycleState::Free)
    return false;

  ++stats_.noteOnsApplied;
  if (stolen)
    ++stats_.steals;
  return true;
}

uint32_t ExactSystem::NoteOff(uint8_t channel, uint8_t note) {
  if (!initialized_ || channel >= kChannelCount || note >= kNoteCount)
    return 0;

  uint32_t applied = 0;
  uint32_t handle = keyHeads_[channel][note];
  while (handle != kInvalidVoiceHandle) {
    ExactVoice &voice = voices_[handle];
    const uint32_t next = voice.nextSameKey;
    if (voice.state == ExactLifecycleState::Active) {
      if (soundFont_ && soundFont_->IsLoaded() &&
          soundFont_->IsSustainEnabled(channel)) {
        voice.heldBySustain = 1;
        ++applied;
      } else {
        TransitionVoiceToReleased(handle);
        ++applied;
      }
    }
    handle = next;
  }

  if (applied > 0)
    ++stats_.noteOffsApplied;
  return applied;
}

bool ExactSystem::IsSampleBackedMode() const {
  return soundFont_ != 0 && soundFont_->IsLoaded();
}

uint32_t ExactSystem::GetActiveVoiceCount() const { return stats_.activeVoices; }

uint32_t ExactSystem::GetReleasedVoiceCount() const {
  return stats_.releasedVoices;
}

const ExactStats &ExactSystem::GetStats() const { return stats_; }

const ExactConfig &ExactSystem::GetConfig() const { return config_; }

const ExactVoice *ExactSystem::GetVoice(uint32_t handle) const {
  if (handle >= voices_.size())
    return 0;
  return &voices_[handle];
}

ExactVoice *ExactSystem::GetMutableVoice(uint32_t handle) {
  if (handle >= voices_.size())
    return 0;
  return &voices_[handle];
}

uint32_t ExactSystem::GetKeyHead(uint32_t channel, uint32_t note) const {
  return keyHeads_[channel][note];
}

uint32_t ExactSystem::GetQueueHead(ExactQueueClass queueClass) const {
  return queues_[(uint32_t)queueClass].head;
}

void ExactSystem::RetireVoice(uint32_t handle) {
  if (handle >= voices_.size())
    return;

  ExactVoice &voice = voices_[handle];
  if (voice.state == ExactLifecycleState::Free)
    return;

  RemoveVoiceState(voice.state);
  UnlinkQueue(handle);
  RemoveKeyVoice(handle);
  ReleaseVoiceHandle(handle);
}

bool ExactSystem::AllocateVoiceHandle(uint32_t *handle, bool *stolen) {
  *stolen = false;

  if (freeCount_ > 0) {
    *handle = freeList_[--freeCount_];
    return true;
  }

  bool quiet = false;
  bool released = false;
  uint32_t candidate = kInvalidVoiceHandle;
  if (!SelectStealCandidate(&candidate, &quiet, &released))
    return false;

  ExactVoice &voice = voices_[candidate];
  RemoveVoiceState(voice.state);
  UnlinkQueue(candidate);
  RemoveKeyVoice(candidate);
  voice = ExactVoice();

  *handle = candidate;
  *stolen = true;
  if (quiet)
    ++stats_.quietSteals;
  if (released)
    ++stats_.releaseSteals;
  return true;
}

bool ExactSystem::SelectStealCandidate(uint32_t *handle, bool *quiet,
                                       bool *released) const {
  const ExactQueueClass order[] = {
      ExactQueueClass::QuietRelease, ExactQueueClass::QuietActive,
      ExactQueueClass::LoudRelease, ExactQueueClass::LoudActive};

  for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
    const QueueState &queue = queues_[(uint32_t)order[i]];
    if (queue.head == kInvalidVoiceHandle)
      continue;
    *handle = queue.head;
    *quiet = (order[i] == ExactQueueClass::QuietRelease ||
              order[i] == ExactQueueClass::QuietActive);
    *released = (order[i] == ExactQueueClass::QuietRelease ||
                 order[i] == ExactQueueClass::LoudRelease);
    return true;
  }

  return false;
}

void ExactSystem::ReleaseVoiceHandle(uint32_t handle) {
  voices_[handle] = ExactVoice();
  freeList_[freeCount_++] = handle;
}

void ExactSystem::ActivateVoice(uint32_t handle, const NormalizedEvent &event) {
  ExactVoice &voice = voices_[handle];
  voice = ExactVoice();
  voice.voiceId = nextVoiceId_++;
  voice.channel = event.channel;
  voice.note = event.note;
  voice.velocity = event.velocity;
  voice.startSequence = event.sequence;
  voice.generation = ++generationCounters_[event.channel][event.note];
  voice.state = ExactLifecycleState::Active;
  voice.queueClass = ExactQueueClass::None;
  voice.phase = 0.0f;
  voice.heldBySustain = 0;
  voice.releaseDecay = 0.9985f;

  bool initializedVoice = false;
  if (soundFont_ && soundFont_->IsLoaded()) {
    SoundFontNoteInfo info;
    if (soundFont_->PrepareNoteOn(event, &info) && info.valid) {
      voice.sampleBacked = 1;
      voice.regionIndex = info.regionIndex;
      voice.sampleIndex = info.sampleIndex;
      voice.sampleData = info.sampleData;
      voice.sampleStart = info.sampleStart;
      voice.sampleEnd = info.sampleEnd;
      voice.loopStart = info.loopStart;
      voice.loopEnd = info.loopEnd;
      voice.loopMode = info.loopMode;
      voice.phase = (float)info.sampleStart;
      voice.phaseStep = info.phaseStep;
      voice.targetGain = info.initialGain;
      voice.leftGain = info.leftGain;
      voice.rightGain = info.rightGain;
      voice.releaseDecay = info.releaseDecay;
      if (info.attackSeconds > 0.0001f) {
        const float attackSamples =
            info.attackSeconds * (float)(config_.sampleRate > 0 ? config_.sampleRate : 44100u);
        voice.attackSamplesRemaining =
            attackSamples > 1.0f ? (uint16_t)(attackSamples > 65535.0f ? 65535u : (uint32_t)attackSamples)
                                 : 0u;
        voice.currentGain = 0.0f;
        voice.attackGainStep =
            voice.attackSamplesRemaining > 0
                ? (info.initialGain / (float)voice.attackSamplesRemaining)
                : info.initialGain;
      } else {
        voice.currentGain = info.initialGain;
        voice.attackSamplesRemaining = 0;
        voice.attackGainStep = info.initialGain;
      }
      initializedVoice = true;
    }
  }

  if (!initializedVoice) {
    if (soundFont_ && soundFont_->IsLoaded()) {
      ReleaseVoiceHandle(handle);
      return;
    }

    voice.frequencyHz = MidiNoteToFrequencyHz(event.note);
    voice.phaseStep =
        voice.frequencyHz /
        (float)(config_.sampleRate > 0 ? config_.sampleRate : 44100u);
    voice.currentGain = VelocityToGain(event.velocity);
    voice.targetGain = voice.currentGain;
    voice.releaseDecay = 0.9985f;
    ExactStereoGains(ExactPan(event.channel, event.note), &voice.leftGain,
                     &voice.rightGain);
  }

  InsertKeyVoice(handle);
  ReclassifyVoiceQueue(handle);
  AddVoiceState(voice.state);
}

void ExactSystem::TransitionVoiceToReleased(uint32_t handle) {
  ExactVoice &voice = voices_[handle];
  if (voice.state != ExactLifecycleState::Active)
    return;

  voice.state = ExactLifecycleState::Released;
  voice.heldBySustain = 0;
  ReclassifyVoiceQueue(handle);
  RemoveVoiceState(ExactLifecycleState::Active);
  AddVoiceState(ExactLifecycleState::Released);
}

void ExactSystem::RefreshVoiceFromSoundFont(uint32_t handle) {
  if (!soundFont_ || !soundFont_->IsLoaded() || handle >= voices_.size())
    return;

  ExactVoice &voice = voices_[handle];
  if (!voice.sampleBacked || voice.regionIndex == kInvalidSoundFontIndex)
    return;

  SoundFontNoteInfo info;
  if (!soundFont_->RefreshVoiceInfo(voice.channel, voice.note, voice.velocity,
                                    voice.regionIndex, &info) ||
      !info.valid) {
    return;
  }

  voice.phaseStep = info.phaseStep;
  voice.targetGain = info.initialGain;
  voice.leftGain = info.leftGain;
  voice.rightGain = info.rightGain;
  voice.releaseDecay = info.releaseDecay;
  if (voice.attackSamplesRemaining == 0 &&
      voice.state == ExactLifecycleState::Active) {
    voice.currentGain = info.initialGain;
  }
}

void ExactSystem::RefreshChannelVoices(uint8_t channel) {
  if (!soundFont_ || !soundFont_->IsLoaded() || channel >= kChannelCount)
    return;

  for (uint32_t note = 0; note < kNoteCount; ++note) {
    uint32_t handle = keyHeads_[channel][note];
    while (handle != kInvalidVoiceHandle) {
      const uint32_t next = voices_[handle].nextSameKey;
      RefreshVoiceFromSoundFont(handle);
      handle = next;
    }
  }
}

void ExactSystem::ReleaseSustainedVoices(uint8_t channel) {
  if (channel >= kChannelCount)
    return;

  for (uint32_t note = 0; note < kNoteCount; ++note) {
    uint32_t handle = keyHeads_[channel][note];
    while (handle != kInvalidVoiceHandle) {
      const uint32_t next = voices_[handle].nextSameKey;
      ExactVoice &voice = voices_[handle];
      if (voice.state == ExactLifecycleState::Active && voice.heldBySustain) {
        TransitionVoiceToReleased(handle);
      }
      handle = next;
    }
  }
}

void ExactSystem::ReleaseAllChannelVoices(uint8_t channel, bool hardKill) {
  if (channel >= kChannelCount)
    return;

  for (uint32_t note = 0; note < kNoteCount; ++note) {
    uint32_t handle = keyHeads_[channel][note];
    while (handle != kInvalidVoiceHandle) {
      const uint32_t next = voices_[handle].nextSameKey;
      if (hardKill)
        RetireVoice(handle);
      else if (voices_[handle].state == ExactLifecycleState::Active)
        TransitionVoiceToReleased(handle);
      handle = next;
    }
  }
}

void ExactSystem::InsertKeyVoice(uint32_t handle) {
  ExactVoice &voice = voices_[handle];
  uint32_t &head = keyHeads_[voice.channel][voice.note];
  voice.prevSameKey = kInvalidVoiceHandle;
  voice.nextSameKey = head;
  if (head != kInvalidVoiceHandle)
    voices_[head].prevSameKey = handle;
  head = handle;
}

void ExactSystem::RemoveKeyVoice(uint32_t handle) {
  ExactVoice &voice = voices_[handle];
  if (voice.channel >= kChannelCount || voice.note >= kNoteCount)
    return;

  uint32_t &head = keyHeads_[voice.channel][voice.note];
  if (voice.prevSameKey != kInvalidVoiceHandle)
    voices_[voice.prevSameKey].nextSameKey = voice.nextSameKey;
  else if (head == handle)
    head = voice.nextSameKey;

  if (voice.nextSameKey != kInvalidVoiceHandle)
    voices_[voice.nextSameKey].prevSameKey = voice.prevSameKey;

  voice.prevSameKey = kInvalidVoiceHandle;
  voice.nextSameKey = kInvalidVoiceHandle;
}

void ExactSystem::LinkQueueTail(ExactQueueClass queueClass, uint32_t handle) {
  QueueState &queue = queues_[(uint32_t)queueClass];
  ExactVoice &voice = voices_[handle];
  voice.queueClass = queueClass;
  voice.prevQueue = queue.tail;
  voice.nextQueue = kInvalidVoiceHandle;

  if (queue.tail != kInvalidVoiceHandle)
    voices_[queue.tail].nextQueue = handle;
  else
    queue.head = handle;
  queue.tail = handle;
}

void ExactSystem::UnlinkQueue(uint32_t handle) {
  ExactVoice &voice = voices_[handle];
  if (voice.queueClass == ExactQueueClass::None)
    return;

  QueueState &queue = queues_[(uint32_t)voice.queueClass];
  if (voice.prevQueue != kInvalidVoiceHandle)
    voices_[voice.prevQueue].nextQueue = voice.nextQueue;
  else
    queue.head = voice.nextQueue;

  if (voice.nextQueue != kInvalidVoiceHandle)
    voices_[voice.nextQueue].prevQueue = voice.prevQueue;
  else
    queue.tail = voice.prevQueue;

  voice.queueClass = ExactQueueClass::None;
  voice.prevQueue = kInvalidVoiceHandle;
  voice.nextQueue = kInvalidVoiceHandle;
}

ExactQueueClass ExactSystem::ClassifyQueue(const ExactVoice &voice) const {
  const bool quiet = voice.velocity <= config_.quietVelocityThreshold;
  if (voice.state == ExactLifecycleState::Released)
    return quiet ? ExactQueueClass::QuietRelease : ExactQueueClass::LoudRelease;
  if (voice.state == ExactLifecycleState::Active)
    return quiet ? ExactQueueClass::QuietActive : ExactQueueClass::LoudActive;
  return ExactQueueClass::None;
}

void ExactSystem::ReclassifyVoiceQueue(uint32_t handle) {
  ExactVoice &voice = voices_[handle];
  UnlinkQueue(handle);
  const ExactQueueClass queueClass = ClassifyQueue(voice);
  if (queueClass != ExactQueueClass::None)
    LinkQueueTail(queueClass, handle);
}

void ExactSystem::AddVoiceState(ExactLifecycleState state) {
  if (state == ExactLifecycleState::Active) {
    ++stats_.activeVoices;
    if (stats_.activeVoices > stats_.peakActiveVoices)
      stats_.peakActiveVoices = stats_.activeVoices;
  } else if (state == ExactLifecycleState::Released) {
    ++stats_.releasedVoices;
  }
}

void ExactSystem::RemoveVoiceState(ExactLifecycleState state) {
  if (state == ExactLifecycleState::Active) {
    if (stats_.activeVoices > 0)
      --stats_.activeVoices;
  } else if (state == ExactLifecycleState::Released) {
    if (stats_.releasedVoices > 0)
      --stats_.releasedVoices;
  }
}

} // namespace virtuallysuper
