#include "VirtuallySuperEngine.h"
#include "VirtuallySuperDensity.h"
#include "VirtuallySuperGrouped.h"
#include "VirtuallySuperOverload.h"
#include "VirtuallySuperSamplerEngine.h"
#include "VirtuallySuperScene.h"
#include "VirtuallySuperSoundFontRuntime.h"
#include "VirtuallySuperTelemetry.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <windows.h>

using namespace virtuallysuper;

static void AppendU16(std::vector<uint8_t> *data, uint16_t value) {
  data->push_back((uint8_t)(value & 0xFFu));
  data->push_back((uint8_t)((value >> 8) & 0xFFu));
}

static void AppendS16(std::vector<uint8_t> *data, int16_t value) {
  AppendU16(data, (uint16_t)value);
}

static void AppendU32(std::vector<uint8_t> *data, uint32_t value) {
  data->push_back((uint8_t)(value & 0xFFu));
  data->push_back((uint8_t)((value >> 8) & 0xFFu));
  data->push_back((uint8_t)((value >> 16) & 0xFFu));
  data->push_back((uint8_t)((value >> 24) & 0xFFu));
}

static size_t BeginChunk(std::vector<uint8_t> *data, const char *id) {
  data->insert(data->end(), id, id + 4);
  const size_t sizeOffset = data->size();
  AppendU32(data, 0);
  return sizeOffset;
}

static void EndChunk(std::vector<uint8_t> *data, size_t sizeOffset) {
  uint32_t chunkSize = (uint32_t)(data->size() - sizeOffset - 4u);
  (*data)[sizeOffset + 0] = (uint8_t)(chunkSize & 0xFFu);
  (*data)[sizeOffset + 1] = (uint8_t)((chunkSize >> 8) & 0xFFu);
  (*data)[sizeOffset + 2] = (uint8_t)((chunkSize >> 16) & 0xFFu);
  (*data)[sizeOffset + 3] = (uint8_t)((chunkSize >> 24) & 0xFFu);
  if (chunkSize & 1u)
    data->push_back(0);
}

static size_t BeginList(std::vector<uint8_t> *data, const char *listType) {
  const size_t sizeOffset = BeginChunk(data, "LIST");
  data->insert(data->end(), listType, listType + 4);
  return sizeOffset;
}

static void AppendName20(std::vector<uint8_t> *data, const char *name) {
  char buffer[20] = {};
  if (name)
    strncpy_s(buffer, name, _TRUNCATE);
  data->insert(data->end(), buffer, buffer + 20);
}

static std::string MakeTempPath(const char *suffix) {
  char tempDir[MAX_PATH] = {};
  char tempFile[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, tempDir);
  GetTempFileNameA(tempDir, "vss", 0, tempFile);
  std::string path = tempFile;
  if (suffix)
    path += suffix;
  return path;
}

static bool WriteAllBytes(const std::string &path,
                          const std::vector<uint8_t> &bytes) {
  FILE *file = 0;
  if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
    return false;
  const size_t written = fwrite(bytes.data(), 1, bytes.size(), file);
  fclose(file);
  return written == bytes.size();
}

static bool CreateMinimalSf2File(std::string *outPath) {
  if (!outPath)
    return false;

  const uint32_t sampleFrames = 96;
  const uint32_t guardFrames = 46;
  std::vector<uint8_t> fileData;

  const size_t riffSize = BeginChunk(&fileData, "RIFF");
  fileData.insert(fileData.end(), {'s','f','b','k'});

  const size_t sdtaSize = BeginList(&fileData, "sdta");
  const size_t smplSize = BeginChunk(&fileData, "smpl");
  for (uint32_t i = 0; i < sampleFrames; ++i) {
    const float phase = (float)i / (float)sampleFrames;
    const int16_t value = (int16_t)((phase * 2.0f - 1.0f) * 28000.0f);
    AppendS16(&fileData, value);
  }
  for (uint32_t i = 0; i < guardFrames; ++i)
    AppendS16(&fileData, 0);
  EndChunk(&fileData, smplSize);
  EndChunk(&fileData, sdtaSize);

  const size_t pdtaSize = BeginList(&fileData, "pdta");

  const size_t phdrSize = BeginChunk(&fileData, "phdr");
  AppendName20(&fileData, "Preset");
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 0);
  AppendU32(&fileData, 0);
  AppendU32(&fileData, 0);
  AppendU32(&fileData, 0);
  AppendName20(&fileData, "EOP");
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 1);
  AppendU32(&fileData, 0);
  AppendU32(&fileData, 0);
  AppendU32(&fileData, 0);
  EndChunk(&fileData, phdrSize);

  const size_t pbagSize = BeginChunk(&fileData, "pbag");
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 1);
  AppendU16(&fileData, 0);
  EndChunk(&fileData, pbagSize);

  const size_t pgenSize = BeginChunk(&fileData, "pgen");
  AppendU16(&fileData, 41);
  AppendU16(&fileData, 0);
  EndChunk(&fileData, pgenSize);

  const size_t instSize = BeginChunk(&fileData, "inst");
  AppendName20(&fileData, "Instrument");
  AppendU16(&fileData, 0);
  AppendName20(&fileData, "EOI");
  AppendU16(&fileData, 1);
  EndChunk(&fileData, instSize);

  const size_t ibagSize = BeginChunk(&fileData, "ibag");
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 1);
  AppendU16(&fileData, 0);
  EndChunk(&fileData, ibagSize);

  const size_t igenSize = BeginChunk(&fileData, "igen");
  AppendU16(&fileData, 53);
  AppendU16(&fileData, 0);
  EndChunk(&fileData, igenSize);

  const size_t shdrSize = BeginChunk(&fileData, "shdr");
  AppendName20(&fileData, "Sample");
  AppendU32(&fileData, 0);
  AppendU32(&fileData, sampleFrames);
  AppendU32(&fileData, 8);
  AppendU32(&fileData, sampleFrames - 8);
  AppendU32(&fileData, 44100);
  fileData.push_back(60);
  fileData.push_back(0);
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 1);
  AppendName20(&fileData, "EOS");
  AppendU32(&fileData, sampleFrames);
  AppendU32(&fileData, sampleFrames);
  AppendU32(&fileData, sampleFrames);
  AppendU32(&fileData, sampleFrames);
  AppendU32(&fileData, 44100);
  fileData.push_back(60);
  fileData.push_back(0);
  AppendU16(&fileData, 0);
  AppendU16(&fileData, 1);
  EndChunk(&fileData, shdrSize);

  EndChunk(&fileData, pdtaSize);
  EndChunk(&fileData, riffSize);

  *outPath = MakeTempPath(".sf2");
  return WriteAllBytes(*outPath, fileData);
}

static bool CreateInvalidSf2File(std::string *outPath) {
  if (!outPath)
    return false;
  *outPath = MakeTempPath(".sf2");
  const std::vector<uint8_t> bytes = {'n','o','p','e'};
  return WriteAllBytes(*outPath, bytes);
}

static NormalizedEvent MakeEvent(EventKind kind, uint8_t channel, uint8_t note,
                                 uint8_t velocity, int64_t targetSample,
                                 uint32_t sequence,
                                 uint8_t applyPriority = 0) {
  NormalizedEvent event;
  event.kind = kind;
  event.channel = channel;
  event.note = note;
  event.velocity = velocity;
  event.targetSample = targetSample;
  event.sequence = sequence;
  event.applyPriority = applyPriority;
  return event;
}

static bool TestSchedulerOrderingAndCoalescing() {
  Scheduler scheduler;
  SchedulerConfig config;
  config.ingressCapacity = 16;
  config.scheduledCapacity = 16;
  if (!scheduler.Initialize(config))
    return false;

  if (scheduler.ScheduleDirect(
          MakeEvent(EventKind::NoteOn, 0, 60, 100, 20, 2)) !=
      ScheduleDecision::Accepted)
    return false;
  if (scheduler.ScheduleDirect(
          MakeEvent(EventKind::NoteOn, 0, 60, 110, 20, 3)) !=
      ScheduleDecision::Coalesced)
    return false;
  if (scheduler.ScheduleDirect(
          MakeEvent(EventKind::NoteOff, 0, 60, 0, 10, 1)) !=
      ScheduleDecision::Accepted)
    return false;

  NormalizedEvent drained[4];
  int64_t renderUntil = 0;
  const size_t count =
      scheduler.DrainScheduledWindow(0, 100, 100, drained, 4, &renderUntil);
  if (count != 2)
    return false;
  if (drained[0].kind != EventKind::NoteOff || drained[0].targetSample != 10)
    return false;
  if (drained[1].kind != EventKind::NoteOn || drained[1].targetSample != 20)
    return false;
  if (scheduler.GetStats().coalescedEvents != 1)
    return false;
  return true;
}

static bool TestEngineApplyWindowAndNoteOff() {
  EnginePrototype engine;
  EngineConfig config;
  config.scheduler.ingressCapacity = 16;
  config.scheduler.scheduledCapacity = 16;
  config.exact.maxVoices = 4;
  if (!engine.Initialize(config))
    return false;

  if (engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 60, 100, 10, 1)) ==
      ScheduleDecision::Dropped)
    return false;
  if (engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 61, 90, 12, 2)) ==
      ScheduleDecision::Dropped)
    return false;
  engine.FlushPendingIngress(16);

  int64_t renderUntil = 0;
  if (engine.ApplyScheduledWindow(0, 100, 100, &renderUntil) != 2)
    return false;
  if (engine.GetExactSystem().GetActiveVoiceCount() != 2)
    return false;

  engine.SubmitEvent(MakeEvent(EventKind::NoteOff, 0, 60, 0, 20, 3));
  engine.FlushPendingIngress(16);
  if (engine.ApplyScheduledWindow(12, 100, 100, &renderUntil) != 1)
    return false;
  if (engine.GetExactSystem().GetActiveVoiceCount() != 1)
    return false;
  if (engine.GetExactSystem().GetReleasedVoiceCount() != 1)
    return false;
  if (engine.GetExactSystem().GetStats().noteOffsApplied != 1)
    return false;
  return true;
}

static bool TestQuietReleaseSteal() {
  EnginePrototype engine;
  EngineConfig config;
  config.scheduler.ingressCapacity = 16;
  config.scheduler.scheduledCapacity = 16;
  config.exact.maxVoices = 2;
  config.exact.quietVelocityThreshold = 60;
  if (!engine.Initialize(config))
    return false;

  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 60, 100, 0, 1));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 61, 20, 1, 2));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOff, 0, 61, 0, 2, 3));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 62, 120, 3, 4));
  engine.FlushPendingIngress(16);

  int64_t renderUntil = 0;
  if (engine.ApplyScheduledWindow(0, 100, 100, &renderUntil) != 4)
    return false;
  if (engine.GetExactSystem().GetActiveVoiceCount() != 2)
    return false;
  if (engine.GetExactSystem().GetReleasedVoiceCount() != 0)
    return false;
  if (engine.GetExactSystem().GetStats().steals != 1)
    return false;
  if (engine.GetExactSystem().GetStats().quietSteals != 1)
    return false;
  if (engine.GetExactSystem().GetStats().releaseSteals != 1)
    return false;
  if (engine.GetExactSystem().GetKeyHead(0, 61) != kInvalidVoiceHandle)
    return false;
  return true;
}

static bool TestGroupedPrototypeBuckets() {
  GroupedSystem grouped;
  GroupedConfig config;
  config.maxGroups = 8;
  config.pitchBandSemitones = 12;
  config.timingBucketSamples = 16;
  if (!grouped.Initialize(config))
    return false;

  grouped.BeginWindow();
  if (!grouped.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 60, 90, 0, 1)))
    return false;
  if (!grouped.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 61, 80, 4, 2)))
    return false;
  if (!grouped.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 72, 70, 20, 3)))
    return false;

  if (grouped.GetActiveGroupCount() != 2)
    return false;
  if (grouped.GetStats().noteOnsAccumulated != 3)
    return false;
  return true;
}

static bool TestDensityPrototypeClouds() {
  DensitySystem density;
  DensityConfig config;
  config.maxObjects = 8;
  config.pitchBandSemitones = 12;
  config.timingBucketSamples = 16;
  config.activationThreshold = 3;
  config.saturationK = 4;
  if (!density.Initialize(config))
    return false;

  density.BeginWindow();
  if (!density.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 60, 90, 0, 1)))
    return false;
  if (!density.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 61, 80, 4, 2)))
    return false;
  if (!density.AccumulateEvent(MakeEvent(EventKind::NoteOn, 0, 62, 70, 8, 3)))
    return false;

  if (density.GetActiveObjectCount() != 1)
    return false;
  const DensityObject *object = density.GetDensityObject(0);
  if (!object || object->representedNoteCount != 3)
    return false;
  if (object->saturatedGain <= 0.0f || object->saturatedGain >= 1.0f)
    return false;
  if (object->grainJitterSeed == 0)
    return false;
  if (density.GetStats().promotedClouds != 1)
    return false;
  return true;
}

static bool TestEngineDensityAccumulation() {
  EnginePrototype engine;
  EngineConfig config;
  config.scheduler.ingressCapacity = 16;
  config.scheduler.scheduledCapacity = 16;
  config.exact.maxVoices = 8;
  config.grouped.maxGroups = 8;
  config.density.maxObjects = 8;
  config.density.pitchBandSemitones = 12;
  config.density.timingBucketSamples = 16;
  config.density.activationThreshold = 2;
  config.overload.softExactVoiceThreshold = 1;
  config.overload.hardExactVoiceThreshold = 1;
  config.overload.panicExactVoiceThreshold = 1;
  config.overload.softSchedulerQueueThreshold = 1;
  config.overload.hardSchedulerQueueThreshold = 1;
  config.overload.panicSchedulerQueueThreshold = 1;
  if (!engine.Initialize(config))
    return false;

  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 60, 18, 0, 1));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 61, 20, 2, 2));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 62, 22, 4, 3));
  engine.FlushPendingIngress(16);

  int64_t renderUntil = 0;
  if (engine.ApplyScheduledWindow(0, 100, 100, &renderUntil) != 3)
    return false;
  if (engine.GetDensitySystem().GetActiveObjectCount() != 1)
    return false;
  if (engine.GetDensitySystem().GetStats().promotedClouds != 1)
    return false;
  return true;
}

static bool TestOverloadPressureReducesExactWork() {
  EnginePrototype engine;
  EngineConfig config;
  config.scheduler.ingressCapacity = 32;
  config.scheduler.scheduledCapacity = 32;
  config.exact.maxVoices = 16;
  config.grouped.maxGroups = 16;
  config.density.maxObjects = 16;
  config.overload.softExactVoiceThreshold = 2;
  config.overload.hardExactVoiceThreshold = 3;
  config.overload.panicExactVoiceThreshold = 4;
  config.overload.softSchedulerQueueThreshold = 4;
  config.overload.hardSchedulerQueueThreshold = 6;
  config.overload.panicSchedulerQueueThreshold = 8;
  if (!engine.Initialize(config))
    return false;

  for (uint32_t i = 0; i < 10; ++i) {
    if (engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, (uint8_t)(60 + i),
                                     (uint8_t)(32 + i), (int64_t)i,
                                     i + 1)) == ScheduleDecision::Dropped) {
      return false;
    }
  }
  engine.FlushPendingIngress(32);

  int64_t renderUntil = 0;
  if (engine.ApplyScheduledWindow(0, 128, 128, &renderUntil) != 10)
    return false;

  if (engine.GetExactSystem().GetActiveVoiceCount() >= 10)
    return false;
  if (engine.GetGroupedSystem().GetStats().noteOnsAccumulated == 0)
    return false;
  if (engine.GetDensitySystem().GetStats().noteOnsAccumulated == 0)
    return false;
  if (engine.GetLatestTelemetrySnapshot().overloadPressureLevel <
      (uint32_t)PressureLevel::Hard) {
    return false;
  }
  return true;
}

static bool TestEngineRenderProducesAudioAndRetiresRelease() {
  EnginePrototype engine;
  EngineConfig config;
  config.scheduler.ingressCapacity = 16;
  config.scheduler.scheduledCapacity = 16;
  config.exact.maxVoices = 4;
  config.grouped.maxGroups = 8;
  config.density.maxObjects = 8;
  config.density.activationThreshold = 2;
  if (!engine.Initialize(config))
    return false;

  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 60, 100, 0, 1));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 61, 90, 2, 2));
  engine.FlushPendingIngress(16);

  int64_t renderUntil = 0;
  if (engine.ApplyScheduledWindow(0, 256, 256, &renderUntil) != 2)
    return false;

  float buffer[512] = {};
  engine.RenderBlock(buffer, 256, 44100);

  bool hasAudio = false;
  for (size_t i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i) {
    if (buffer[i] != 0.0f) {
      hasAudio = true;
      break;
    }
  }
  if (!hasAudio)
    return false;

  engine.SubmitEvent(MakeEvent(EventKind::NoteOff, 0, 60, 0, 260, 3));
  engine.SubmitEvent(MakeEvent(EventKind::NoteOff, 0, 61, 0, 262, 4));
  engine.FlushPendingIngress(16);
  if (engine.ApplyScheduledWindow(256, 4096, 4096, &renderUntil) != 2)
    return false;

  for (int i = 0; i < 32; ++i)
    engine.RenderBlock(buffer, 256, 44100);

  if (engine.GetExactSystem().GetReleasedVoiceCount() != 0)
    return false;
  return true;
}

static bool TestSamplerEngineShell() {
  VirtuallySuperSamplerEngine engine;
  SamplerInitParams params;
  params.sourcePath = "prototype.vs";
  params.sampleRate = 44100;
  params.maxVoices = 8;
  params.runtimeSettings.velocityCurve = 2.4f;
  params.runtimeSettings.velocityFloor = 0.0f;
  params.runtimeSettings.velocityIgnoreBelow = 0;
  params.runtimeSettings.asyncNoteStarts = true;
  params.runtimeSettings.eventTimingMode = EventTimingMode::ACCURATE;

  if (!engine.Initialize(params))
    return false;

  engine.SetRenderWindow(1024, 32, 44100, 0, 0, false);
  engine.BeginRenderBlock();

  MidiEvent on = {};
  on.type = MidiEvent::NOTE_ON;
  on.channel = 0;
  on.data1 = 60;
  on.data2 = 100;
  on.sequence = 1;
  on.targetSample = 1024;
  engine.ProcessMidiEvent(on);

  float buffer[64] = {};
  engine.Render(buffer, 32);

  DWORD channels[16] = {};
  const DWORD active = engine.GetActiveVoiceStats(channels, 16);
  if (active != 1 || channels[0] != 1)
    return false;

  SamplerDiagnostics diagnostics = engine.GetDiagnostics();
  if (diagnostics.noteOnEventsThisBlock != 1)
    return false;
  if (diagnostics.lastWarning.empty())
    return false;
  if (diagnostics.samplerErrorCode !=
      (unsigned int)SamplerErrorCode::NONE)
    return false;

  bool hasAudio = false;
  for (size_t i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i) {
    if (buffer[i] != 0.0f) {
      hasAudio = true;
      break;
    }
  }
  if (!hasAudio)
    return false;

  return true;
}

static bool TestSamplerEngineIdleFastPath() {
  VirtuallySuperSamplerEngine engine;
  SamplerInitParams params;
  params.sourcePath = "prototype.vs";
  params.sampleRate = 44100;
  params.maxVoices = 8;
  params.runtimeSettings.velocityCurve = 2.4f;
  params.runtimeSettings.velocityFloor = 0.0f;
  params.runtimeSettings.velocityIgnoreBelow = 0;
  params.runtimeSettings.asyncNoteStarts = true;
  params.runtimeSettings.eventTimingMode = EventTimingMode::ACCURATE;

  if (!engine.Initialize(params))
    return false;

  engine.SetRenderWindow(2048, 32, 44100, 0, 0, false);
  engine.BeginRenderBlock();

  float buffer[64];
  for (size_t i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i)
    buffer[i] = 1.0f;

  engine.Render(buffer, 32);

  for (size_t i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i) {
    if (buffer[i] != 0.0f)
      return false;
  }

  SamplerDiagnostics diagnostics = engine.GetDiagnostics();
  if (diagnostics.sampleRenderMs != 0.0f)
    return false;
  if (diagnostics.virtuallySuperIdleFastPathHits == 0)
    return false;
  if (diagnostics.virtuallySuperVoiceEquivalent != 0)
    return false;
  return true;
}

static bool TestSoundFontRuntimeLoadsAndDispatches() {
  std::string path;
  if (!CreateMinimalSf2File(&path))
    return false;

  SoundFontRuntime runtime;
  std::string warning;
  const bool loaded = runtime.Load(path.c_str(), 44100, &warning);
  DeleteFileA(path.c_str());
  if (!loaded)
    return false;
  if (runtime.GetPresetCount() != 1 || runtime.GetRegionCount() != 1 ||
      runtime.GetSampleCount() != 1) {
    return false;
  }

  NormalizedEvent noteOn = MakeEvent(EventKind::NoteOn, 0, 60, 100, 0, 1);
  SoundFontNoteInfo info;
  if (!runtime.PrepareNoteOn(noteOn, &info) || !info.valid)
    return false;
  const float neutralStep = info.phaseStep;

  NormalizedEvent octave = MakeEvent(EventKind::NoteOn, 0, 72, 100, 0, 2);
  if (!runtime.PrepareNoteOn(octave, &info) || info.phaseStep <= neutralStep)
    return false;

  NormalizedEvent bend = MakeEvent(EventKind::PitchBend, 0, 0, 0, 0, 3);
  bend.value = 0x7F;
  bend.velocity = 0x7F;
  if (!runtime.HandleEvent(bend))
    return false;
  if (!runtime.PrepareNoteOn(noteOn, &info) || info.phaseStep <= neutralStep)
    return false;

  NormalizedEvent program = MakeEvent(EventKind::ProgramChange, 0, 5, 0, 0, 4);
  if (!runtime.HandleEvent(program))
    return false;
  if (runtime.PrepareNoteOn(noteOn, &info))
    return false;

  return true;
}

static bool TestExactSyntheticPitchBendRefresh() {
  ExactSystem exact;
  ExactConfig config;
  config.maxVoices = 4;
  config.sampleRate = 44100;
  if (!exact.Initialize(config))
    return false;

  NormalizedEvent noteOn = MakeEvent(EventKind::NoteOn, 0, 60, 100, 0, 1);
  noteOn.mappedVelocity = 100;
  if (!exact.ApplyEvent(noteOn))
    return false;

  const uint32_t head = exact.GetKeyHead(0, 60);
  const ExactVoice *voice = exact.GetVoice(head);
  if (!voice)
    return false;
  const float neutralStep = voice->phaseStep;

  NormalizedEvent bend = MakeEvent(EventKind::PitchBend, 0, 0, 0, 0, 2);
  bend.value = 0x7F;
  bend.velocity = 0x7F;
  if (!exact.ApplyEvent(bend))
    return false;

  voice = exact.GetVoice(head);
  if (!voice || voice->phaseStep <= neutralStep)
    return false;
  return true;
}

static bool TestSampleBackedEngineStaysExactOnlyUnderPressure() {
  std::string path;
  if (!CreateMinimalSf2File(&path))
    return false;

  SoundFontRuntime runtime;
  std::string warning;
  const bool loaded = runtime.Load(path.c_str(), 44100, &warning);
  DeleteFileA(path.c_str());
  if (!loaded)
    return false;

  EnginePrototype engine;
  EngineConfig config;
  config.scheduler.ingressCapacity = 16;
  config.scheduler.scheduledCapacity = 16;
  config.exact.maxVoices = 8;
  config.grouped.maxGroups = 8;
  config.density.maxObjects = 8;
  config.overload.softExactVoiceThreshold = 1;
  config.overload.hardExactVoiceThreshold = 1;
  config.overload.panicExactVoiceThreshold = 64;
  config.overload.softSchedulerQueueThreshold = 64;
  config.overload.hardSchedulerQueueThreshold = 64;
  config.overload.panicSchedulerQueueThreshold = 64;
  if (!engine.Initialize(config))
    return false;

  engine.GetExactSystem().SetSoundFontRuntime(&runtime);

  if (engine.SubmitEvent(MakeEvent(EventKind::ProgramChange, 0, 0, 0, 0, 1)) ==
      ScheduleDecision::Dropped) {
    return false;
  }
  if (engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 60, 100, 1, 2)) ==
      ScheduleDecision::Dropped) {
    return false;
  }
  if (engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 61, 40, 2, 3)) ==
      ScheduleDecision::Dropped) {
    return false;
  }
  if (engine.SubmitEvent(MakeEvent(EventKind::NoteOn, 0, 62, 24, 3, 4)) ==
      ScheduleDecision::Dropped) {
    return false;
  }
  engine.FlushPendingIngress(16);

  int64_t renderUntil = 0;
  if (engine.ApplyScheduledWindow(0, 128, 128, &renderUntil) != 4)
    return false;
  if (engine.GetExactSystem().GetActiveVoiceCount() != 3)
    return false;
  if (engine.GetGroupedSystem().GetStats().noteOnsAccumulated != 0)
    return false;
  if (engine.GetDensitySystem().GetStats().noteOnsAccumulated != 0)
    return false;

  return true;
}

static bool TestSoundFontInvalidFileFails() {
  std::string path;
  if (!CreateInvalidSf2File(&path))
    return false;

  VirtuallySuperSamplerEngine engine;
  SamplerInitParams params;
  params.sourcePath = path;
  params.sampleRate = 44100;
  params.maxVoices = 8;
  params.runtimeSettings.velocityCurve = 2.4f;
  params.runtimeSettings.velocityFloor = 0.0f;
  params.runtimeSettings.velocityIgnoreBelow = 0;
  params.runtimeSettings.asyncNoteStarts = true;
  params.runtimeSettings.eventTimingMode = EventTimingMode::ACCURATE;

  const bool initialized = engine.Initialize(params);
  DeleteFileA(path.c_str());
  if (initialized)
    return false;

  SamplerDiagnostics diagnostics = engine.GetDiagnostics();
  if (diagnostics.samplerStateCode !=
          (unsigned int)SamplerRuntimeStateCode::FAILED ||
      diagnostics.failedSampleCount == 0) {
    return false;
  }
  return true;
}

static bool TestSamplerEngineNativeSf2Playback() {
  std::string path;
  if (!CreateMinimalSf2File(&path))
    return false;

  VirtuallySuperSamplerEngine engine;
  SamplerInitParams params;
  params.sourcePath = path;
  params.sampleRate = 44100;
  params.maxVoices = 8;
  params.runtimeSettings.velocityCurve = 2.4f;
  params.runtimeSettings.velocityFloor = 0.0f;
  params.runtimeSettings.velocityIgnoreBelow = 0;
  params.runtimeSettings.asyncNoteStarts = true;
  params.runtimeSettings.eventTimingMode = EventTimingMode::ACCURATE;

  if (!engine.Initialize(params)) {
    DeleteFileA(path.c_str());
    return false;
  }

  engine.SetRenderWindow(0, 64, 44100, 0, 0, false);
  engine.BeginRenderBlock();

  MidiEvent program = {};
  program.type = MidiEvent::PROGRAM_CHANGE;
  program.channel = 0;
  program.data1 = 0;
  program.sequence = 1;
  program.targetSample = 0;
  engine.ProcessMidiEvent(program);

  MidiEvent on = {};
  on.type = MidiEvent::NOTE_ON;
  on.channel = 0;
  on.data1 = 60;
  on.data2 = 120;
  on.sequence = 2;
  on.targetSample = 0;
  engine.ProcessMidiEvent(on);

  float buffer[128] = {};
  engine.Render(buffer, 64);
  DeleteFileA(path.c_str());

  bool hasAudio = false;
  for (size_t i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i) {
    if (buffer[i] != 0.0f) {
      hasAudio = true;
      break;
    }
  }
  if (!hasAudio)
    return false;

  SamplerDiagnostics diagnostics = engine.GetDiagnostics();
  if (diagnostics.loadedSampleCount != 1 ||
      diagnostics.virtuallySuperLoadedPresets != 1 ||
      diagnostics.virtuallySuperLoadedRegions != 1 ||
      diagnostics.virtuallySuperExactMode != 1 ||
      diagnostics.virtuallySuperExactVoices == 0) {
    return false;
  }
  return true;
}

static bool TestSamplerEngineCenterPitchBendIsNeutral() {
  std::string path;
  if (!CreateMinimalSf2File(&path))
    return false;

  SamplerInitParams params;
  params.sourcePath = path;
  params.sampleRate = 44100;
  params.maxVoices = 8;
  params.runtimeSettings.velocityCurve = 2.4f;
  params.runtimeSettings.velocityFloor = 0.0f;
  params.runtimeSettings.velocityIgnoreBelow = 0;
  params.runtimeSettings.asyncNoteStarts = true;
  params.runtimeSettings.eventTimingMode = EventTimingMode::ACCURATE;

  VirtuallySuperSamplerEngine baseline;
  VirtuallySuperSamplerEngine centered;
  if (!baseline.Initialize(params) || !centered.Initialize(params)) {
    DeleteFileA(path.c_str());
    return false;
  }

  baseline.SetRenderWindow(0, 64, 44100, 0, 0, false);
  baseline.BeginRenderBlock();
  centered.SetRenderWindow(0, 64, 44100, 0, 0, false);
  centered.BeginRenderBlock();

  MidiEvent program = {};
  program.type = MidiEvent::PROGRAM_CHANGE;
  program.channel = 0;
  program.data1 = 0;
  program.sequence = 1;
  baseline.ProcessMidiEvent(program);
  centered.ProcessMidiEvent(program);

  MidiEvent bend = {};
  bend.type = MidiEvent::PITCH_BEND;
  bend.channel = 0;
  bend.data1 = 8192;
  bend.sequence = 2;
  centered.ProcessMidiEvent(bend);

  MidiEvent on = {};
  on.type = MidiEvent::NOTE_ON;
  on.channel = 0;
  on.data1 = 60;
  on.data2 = 120;
  on.sequence = 3;
  baseline.ProcessMidiEvent(on);
  centered.ProcessMidiEvent(on);

  float baselineBuffer[128] = {};
  float centeredBuffer[128] = {};
  baseline.Render(baselineBuffer, 64);
  centered.Render(centeredBuffer, 64);
  DeleteFileA(path.c_str());

  for (size_t i = 0; i < sizeof(baselineBuffer) / sizeof(baselineBuffer[0]); ++i) {
    const float delta = baselineBuffer[i] - centeredBuffer[i];
    if (delta < -0.00001f || delta > 0.00001f)
      return false;
  }
  return true;
}

static bool TestSamplerEngineVelocityIgnoreBelowSkipsQuietNotes() {
  VirtuallySuperSamplerEngine engine;
  SamplerInitParams params;
  params.sourcePath = "prototype.vs";
  params.sampleRate = 44100;
  params.maxVoices = 8;
  params.runtimeSettings.velocityCurve = 2.4f;
  params.runtimeSettings.velocityFloor = 0.0f;
  params.runtimeSettings.velocityIgnoreBelow = 64;
  params.runtimeSettings.asyncNoteStarts = true;
  params.runtimeSettings.eventTimingMode = EventTimingMode::ACCURATE;

  if (!engine.Initialize(params))
    return false;

  engine.SetRenderWindow(0, 64, 44100, 0, 0, false);
  engine.BeginRenderBlock();

  MidiEvent quiet = {};
  quiet.type = MidiEvent::NOTE_ON;
  quiet.channel = 0;
  quiet.data1 = 60;
  quiet.data2 = 32;
  quiet.sequence = 1;
  engine.ProcessMidiEvent(quiet);

  float quietBuffer[128] = {};
  engine.Render(quietBuffer, 64);
  for (size_t i = 0; i < sizeof(quietBuffer) / sizeof(quietBuffer[0]); ++i) {
    if (quietBuffer[i] != 0.0f)
      return false;
  }

  engine.SetRenderWindow(64, 64, 44100, 0, 0, false);
  engine.BeginRenderBlock();

  MidiEvent strong = quiet;
  strong.data2 = 100;
  strong.sequence = 2;
  engine.ProcessMidiEvent(strong);

  float strongBuffer[128] = {};
  engine.Render(strongBuffer, 64);
  bool hasAudio = false;
  for (size_t i = 0; i < sizeof(strongBuffer) / sizeof(strongBuffer[0]); ++i) {
    if (strongBuffer[i] != 0.0f) {
      hasAudio = true;
      break;
    }
  }
  return hasAudio;
}

static bool TestSceneCompilerActions() {
  SceneCompiler scene;
  ExactStats exactStats;
  scene.BeginWindow();

  SceneAction on = scene.CompileEvent(
      MakeEvent(EventKind::NoteOn, 0, 60, 100, 0, 1), exactStats,
      PressureLevel::Normal);
  if (on.kind != SceneActionKind::SpawnExactVoice)
    return false;
  if (on.observeGrouped != 0 || on.observeDensity != 0 ||
      on.protectedAttack == 0) {
    return false;
  }

  SceneAction off = scene.CompileEvent(
      MakeEvent(EventKind::NoteOff, 0, 60, 0, 8, 2), exactStats,
      PressureLevel::Normal);
  if (off.kind != SceneActionKind::ReleaseExactVoice)
    return false;

  SceneAction reset = scene.CompileEvent(
      MakeEvent(EventKind::Reset, 0, 0, 0, 16, 3), exactStats,
      PressureLevel::Normal);
  if (reset.kind != SceneActionKind::ResetScene)
    return false;

  SceneAction hard = scene.CompileEvent(
      MakeEvent(EventKind::NoteOn, 0, 48, 48, 24, 4), exactStats,
      PressureLevel::Hard);
  if (hard.kind != SceneActionKind::None || hard.observeGrouped == 0)
    return false;

  SceneAction panic = scene.CompileEvent(
      MakeEvent(EventKind::NoteOn, 0, 40, 12, 32, 5), exactStats,
      PressureLevel::Panic);
  if (panic.kind != SceneActionKind::None || panic.observeDensity == 0)
    return false;

  if (scene.GetStats().exactActions != 2)
    return false;
  if (scene.GetStats().resetActions != 1)
    return false;
  return true;
}

static bool TestTelemetryPublisher() {
  TelemetryPublisher telemetry;
  SchedulerStats scheduler;
  SceneStats scene;
  ExactStats exact;
  GroupedStats grouped;
  DensityStats density;

  scheduler.maxTransitionQueueDepth = 5;
  scheduler.coalescedEvents = 2;
  scene.exactActions = 3;
  scene.groupedObservations = 4;
  scene.densityObservations = 5;
  exact.activeVoices = 6;
  exact.releasedVoices = 1;
  exact.steals = 2;
  grouped.activeGroups = 7;
  grouped.noteOnsAccumulated = 8;
  density.activeObjects = 9;
  density.noteOnsAccumulated = 10;
  density.promotedClouds = 11;

  telemetry.Publish(scheduler, scene, exact, grouped, density, 12, 13,
                    PressureLevel::Hard);
  const TelemetrySnapshot &snapshot = telemetry.GetLatestSnapshot();
  if (snapshot.exactVoices != 6)
    return false;
  if (snapshot.groupedObjects != 7)
    return false;
  if (snapshot.densityObjects != 9)
    return false;
  if (snapshot.voiceEquivalent != 24)
    return false;
  if (snapshot.schedulerQueuedEvents != 12)
    return false;
  if (snapshot.lastAppliedEvents != 13)
    return false;
  if (snapshot.overloadPressureLevel != (uint32_t)PressureLevel::Hard)
    return false;
  if (telemetry.GetSharedState().sequence == 0)
    return false;
  return true;
}

int main() {
  const struct {
    const char *name;
    bool (*fn)();
  } tests[] = {
      {"scheduler ordering and coalescing", TestSchedulerOrderingAndCoalescing},
      {"engine apply window and note off", TestEngineApplyWindowAndNoteOff},
      {"quiet release steal", TestQuietReleaseSteal},
      {"grouped prototype buckets", TestGroupedPrototypeBuckets},
      {"density prototype clouds", TestDensityPrototypeClouds},
      {"engine density accumulation", TestEngineDensityAccumulation},
      {"overload pressure reduces exact work", TestOverloadPressureReducesExactWork},
      {"engine render produces audio and retires release",
       TestEngineRenderProducesAudioAndRetiresRelease},
      {"sampler engine shell", TestSamplerEngineShell},
      {"sampler engine idle fast path", TestSamplerEngineIdleFastPath},
      {"soundfont runtime loads and dispatches",
       TestSoundFontRuntimeLoadsAndDispatches},
      {"exact synthetic pitch bend refresh", TestExactSyntheticPitchBendRefresh},
      {"sample-backed engine stays exact only under pressure",
       TestSampleBackedEngineStaysExactOnlyUnderPressure},
      {"soundfont invalid file fails", TestSoundFontInvalidFileFails},
      {"sampler engine native sf2 playback", TestSamplerEngineNativeSf2Playback},
      {"sampler engine center pitch bend is neutral",
       TestSamplerEngineCenterPitchBendIsNeutral},
      {"sampler engine velocity ignore below skips quiet notes",
       TestSamplerEngineVelocityIgnoreBelowSkipsQuietNotes},
      {"scene compiler actions", TestSceneCompilerActions},
      {"telemetry publisher", TestTelemetryPublisher},
  };

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    if (!tests[i].fn()) {
      fprintf(stderr, "FAILED: %s\n", tests[i].name);
      return 1;
    }
  }

  printf("VirtuallySuper prototype tests passed.\n");
  return 0;
}
