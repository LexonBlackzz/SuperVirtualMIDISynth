#include "VirtuallySuperSoundFontRuntime.h"

#include "VirtuallySuperSoundFontDispatch.h"
#include "VirtuallySuperSoundFontParser.h"

#include <math.h>
#include <stdio.h>

namespace {

static float DbToLinear(float db) {
  return powf(10.0f, db * (-1.0f / 20.0f));
}

static float ClampFloat(float value, float minValue, float maxValue) {
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

static uint16_t ClampPitchWheel(uint16_t value) {
  return value > 0x3FFFu ? 0x3FFFu : value;
}

static float ComputeChannelPitchShift(const virtuallysuper::SoundFontChannelState &channel) {
  if (channel.pitchWheel == 8192)
    return channel.tuning;
  return ((float)channel.pitchWheel / 16383.0f) * channel.pitchRange * 2.0f -
         channel.pitchRange + channel.tuning;
}

static void ApplyRpnState(virtuallysuper::SoundFontChannelState *channel,
                          uint8_t controller) {
  if (!channel || channel->rpnMsb != 0)
    return;

  if (channel->rpnLsb == 0) {
    const float range = (float)channel->dataEntryMsb +
                        (float)channel->dataEntryLsb * 0.01f;
    channel->pitchRange = ClampFloat(range, 0.0f, 96.0f);
  } else if (channel->rpnLsb == 1) {
    const uint16_t midiData =
        (uint16_t)(((uint16_t)channel->dataEntryMsb << 7) | channel->dataEntryLsb);
    channel->tuning =
        (float)((int)channel->tuning) + ((float)midiData - 8192.0f) / 8192.0f;
  } else if (channel->rpnLsb == 2 && controller == 6) {
    channel->tuning =
        ((float)channel->dataEntryMsb - 64.0f) +
        (channel->tuning - (float)((int)channel->tuning));
  }
}

static float MakeReleaseDecay(float releaseSeconds, uint32_t sampleRate) {
  const float clampedRelease = releaseSeconds <= 0.0005f ? 0.0005f : releaseSeconds;
  const float releaseSamples = clampedRelease * (float)(sampleRate > 0 ? sampleRate : 44100u);
  if (releaseSamples <= 1.0f)
    return 0.0f;
  return powf(0.0001f, 1.0f / releaseSamples);
}

static void ComputeStereoGains(float pan, float *left, float *right) {
  const float clamped = ClampFloat(pan, -1.0f, 1.0f);
  *left = 0.5f * (1.0f - clamped * 0.35f);
  *right = 0.5f * (1.0f + clamped * 0.35f);
}

} // namespace

namespace virtuallysuper {

SoundFontRuntime::SoundFontRuntime()
    : runtimeData_(), channels_(), outputSampleRate_(44100) {
  ResetChannels();
}

bool SoundFontRuntime::Load(const char *path, uint32_t outputSampleRate,
                            std::string *warningText) {
  Reset();
  outputSampleRate_ = outputSampleRate > 0 ? outputSampleRate : 44100u;

  SoundFontParser parser;
  std::string parseError;
  if (!parser.LoadFile(path, runtimeData_, parseError)) {
    if (warningText)
      *warningText = parseError;
    return false;
  }

  if (!BuildSoundFontDispatch(runtimeData_)) {
    if (warningText)
      *warningText = "VirtuallySuper failed to build SoundFont dispatch tables.";
    Reset();
    return false;
  }

  ResetChannels();

  if (warningText) {
    char buffer[192];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "VirtuallySuper native SF2 exact tier loaded: %u presets, %u regions, %u samples.",
                (unsigned int)runtimeData_.presets.size(),
                (unsigned int)runtimeData_.regions.size(),
                (unsigned int)runtimeData_.samples.size());
    *warningText = buffer;
  }
  return true;
}

void SoundFontRuntime::Reset() {
  runtimeData_.Reset();
  outputSampleRate_ = 44100u;
  ResetChannels();
}

void SoundFontRuntime::ResetChannels() {
  for (uint32_t channel = 0; channel < 16; ++channel)
    ResetChannel((uint8_t)channel);
}

bool SoundFontRuntime::IsLoaded() const {
  return !runtimeData_.presets.empty() && !runtimeData_.regions.empty() &&
         !runtimeData_.samples.empty() && !runtimeData_.sampleData.empty();
}

bool SoundFontRuntime::HandleEvent(const NormalizedEvent &event) {
  if (!IsLoaded() || event.channel >= 16)
    return false;

  SoundFontChannelState &channel = channels_[event.channel];
  switch (event.kind) {
  case EventKind::ProgramChange:
    channel.program = event.note;
    ResolveProgram(event.channel);
    return true;
  case EventKind::ControlChange: {
    const uint8_t controller = event.value;
    const uint8_t value = event.velocity;
    switch (controller) {
    case 0:
      channel.bankMsb = value;
      channel.resolvedBank = (uint16_t)((channel.bankMsb << 7) | channel.bankLsb);
      ResolveProgram(event.channel);
      return true;
    case 32:
      channel.bankLsb = value;
      channel.resolvedBank = (uint16_t)((channel.bankMsb << 7) | channel.bankLsb);
      ResolveProgram(event.channel);
      return true;
    case 7:
      channel.volume = (float)value / 127.0f;
      return true;
    case 10:
      channel.pan = (float)value / 127.0f;
      return true;
    case 11:
      channel.expression = (float)value / 127.0f;
      return true;
    case 101:
      channel.rpnMsb = value;
      return true;
    case 100:
      channel.rpnLsb = value;
      return true;
    case 99:
    case 98:
      channel.rpnMsb = 127;
      channel.rpnLsb = 127;
      return true;
    case 6:
      channel.dataEntryMsb = value;
      ApplyRpnState(&channel, controller);
      return true;
    case 38:
      channel.dataEntryLsb = value;
      ApplyRpnState(&channel, controller);
      return true;
    case 64:
      channel.sustain = value >= 64 ? 1 : 0;
      return true;
    case 121: {
      const uint16_t bankMsb = channel.bankMsb;
      const uint16_t bankLsb = channel.bankLsb;
      const uint16_t program = channel.program;
      ResetChannel(event.channel);
      channel.bankMsb = bankMsb;
      channel.bankLsb = bankLsb;
      channel.resolvedBank = (uint16_t)((bankMsb << 7) | bankLsb);
      channel.program = program;
      ResolveProgram(event.channel);
      return true;
    }
    case 120:
    case 123:
      return true;
    default:
      return false;
    }
  }
  case EventKind::PitchBend:
    channel.pitchWheel = ClampPitchWheel(
        (uint16_t)((event.value & 0x7Fu) |
                   ((uint16_t)(event.velocity & 0x7Fu) << 7)));
    return true;
  case EventKind::Reset:
    ResetChannels();
    return true;
  default:
    return false;
  }
}

bool SoundFontRuntime::PrepareNoteOn(const NormalizedEvent &event,
                                     SoundFontNoteInfo *info) const {
  if (!info)
    return false;

  *info = SoundFontNoteInfo();
  if (!IsLoaded() || event.channel >= 16)
    return false;

  const SoundFontChannelState &channel = channels_[event.channel];
  if (channel.presetIndex == kInvalidSoundFontIndex)
    return false;

  uint16_t regionIndex = kInvalidSoundFontIndex;
  const SoundFontRegion *region = ResolveSoundFontRegion(
      runtimeData_, channel.presetIndex, event.note, event.velocity, &regionIndex);
  if (!region)
    return false;

  return FillNoteInfo(event.channel, event.note, event.velocity,
                      event.mappedVelocity, *region, regionIndex, info);
}

bool SoundFontRuntime::RefreshVoiceInfo(uint8_t channel, uint8_t note,
                                        uint8_t velocity,
                                        uint8_t mappedVelocity,
                                        uint16_t regionIndex,
                                        SoundFontNoteInfo *info) const {
  if (!info) 
    return false;
  *info = SoundFontNoteInfo();

  if (!IsLoaded() || channel >= 16 || regionIndex >= runtimeData_.regions.size())
    return false;

  return FillNoteInfo(channel, note, velocity, mappedVelocity,
                      runtimeData_.regions[regionIndex], regionIndex, info);
}

uint32_t SoundFontRuntime::GetPresetCount() const {
  return (uint32_t)runtimeData_.presets.size();
}

uint32_t SoundFontRuntime::GetRegionCount() const {
  return (uint32_t)runtimeData_.regions.size();
}

uint32_t SoundFontRuntime::GetSampleCount() const {
  return (uint32_t)runtimeData_.samples.size();
}

bool SoundFontRuntime::IsSustainEnabled(uint8_t channel) const {
  return channel < 16 && channels_[channel].sustain != 0;
}

float SoundFontRuntime::GetPitchBendRange(uint8_t channel) const {
  return channel < 16 ? channels_[channel].pitchRange : 0.0f;
}

void SoundFontRuntime::ResetChannel(uint8_t channel) {
  if (channel >= 16)
    return;
  channels_[channel] = SoundFontChannelState();
  ResolveProgram(channel);
}

void SoundFontRuntime::ResolveProgram(uint8_t channel) {
  if (channel >= 16)
    return;

  SoundFontChannelState &state = channels_[channel];
  state.resolvedBank = (uint16_t)((state.bankMsb << 7) | state.bankLsb);
  const int presetIndex =
      ResolveSoundFontPresetIndex(runtimeData_, state.resolvedBank, state.program);
  state.presetIndex =
      presetIndex >= 0 ? (uint16_t)presetIndex : kInvalidSoundFontIndex;
}

bool SoundFontRuntime::FillNoteInfo(uint8_t channelIndex, uint8_t note,
                                    uint8_t velocity, uint8_t mappedVelocity,
                                    const SoundFontRegion &region,
                                    uint16_t regionIndex,
                                    SoundFontNoteInfo *info) const {
  if (!info || region.sampleIndex >= runtimeData_.samples.size() ||
      runtimeData_.sampleData.empty()) {
    return false;
  }

  const SoundFontChannelState &channel = channels_[channelIndex];
  const SoundFontSample &sample = runtimeData_.samples[region.sampleIndex];

  const float rootKey =
      (float)(region.rootKey >= 0 ? region.rootKey : (int)sample.originalPitch);
  const float pitchShiftSemitones = ComputeChannelPitchShift(channel);
  const float noteTuneSemitones =
      (float)region.coarseTune +
      ((float)region.fineTune + (float)sample.pitchCorrection) *
          (1.0f / 100.0f);
  const float adjustedPitch =
      rootKey +
      ((((float)note + noteTuneSemitones) - rootKey) *
       ((float)region.keyTrack / 100.0f)) +
      pitchShiftSemitones;
  const float semitoneOffset = adjustedPitch - rootKey;
  const float pitchRatio = powf(2.0f, semitoneOffset * (1.0f / 12.0f));
  const float sourceRate = (float)(region.sampleRate > 0 ? region.sampleRate : sample.sampleRate);
  const float outRate = (float)(outputSampleRate_ > 0 ? outputSampleRate_ : 44100u);
  const uint8_t gainVelocity = mappedVelocity > 0 ? mappedVelocity : velocity;
  const float velocityGain = (float)gainVelocity / 127.0f;
  const float gain = velocityGain * velocityGain * DbToLinear(region.attenuationDb) *
                     channel.volume * channel.expression;
  const float channelPan = channel.pan * 2.0f - 1.0f;
  const float combinedPan = ClampFloat(region.pan + channelPan, -1.0f, 1.0f);

  *info = SoundFontNoteInfo();
  info->valid = true;
  info->regionIndex = regionIndex;
  info->sampleIndex = region.sampleIndex;
  info->sampleData = runtimeData_.sampleData.data();
  info->sampleStart = region.sampleStart;
  info->sampleEnd = region.sampleEnd;
  info->loopStart = region.loopStart;
  info->loopEnd = region.loopEnd;
  info->loopMode = region.loopMode;
  info->phaseStep = (outRate > 0.0f && sourceRate > 0.0f)
                        ? (sourceRate / outRate) * pitchRatio
                        : pitchRatio;
  info->initialGain = gain;
  ComputeStereoGains(combinedPan, &info->leftGain, &info->rightGain);
  info->attackSeconds = region.attackSeconds;
  info->releaseDecay = MakeReleaseDecay(region.releaseSeconds, outputSampleRate_);
  return true;
}

} // namespace virtuallysuper
