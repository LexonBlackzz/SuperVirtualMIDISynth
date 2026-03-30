#include "SfzEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

namespace {
static const float kDefaultPitchBendRangeSemitones = 2.0f;

static std::string ToLower(const std::string &value) {
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) { return (char)std::tolower(ch); });
  return lowered;
}

static std::string Trim(const std::string &value) {
  size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return std::string();
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

static bool FileExists(const std::string &path) {
  return !path.empty() &&
         GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool IsAbsolutePath(const std::string &path) {
  return path.size() > 2 && path[1] == ':' &&
         (path[2] == '\\' || path[2] == '/');
}

static std::string NormalizeSlashes(const std::string &path) {
  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '/', '\\');
  return normalized;
}

static std::string GetDirectoryName(const std::string &path) {
  size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos)
    return std::string();
  return path.substr(0, slash);
}

static std::string JoinPath(const std::string &baseDir,
                            const std::string &relativePath) {
  if (relativePath.empty())
    return baseDir;
  if (IsAbsolutePath(relativePath))
    return NormalizeSlashes(relativePath);
  if (baseDir.empty())
    return NormalizeSlashes(relativePath);
  return NormalizeSlashes(baseDir + "\\" + relativePath);
}

static std::string CanonicalizePath(const std::string &path) {
  char fullPath[MAX_PATH];
  DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, fullPath, NULL);
  if (length == 0 || length >= MAX_PATH)
    return NormalizeSlashes(path);
  return NormalizeSlashes(fullPath);
}

static float ClampFloat(float value, float minValue, float maxValue) {
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

static int ClampInt(int value, int minValue, int maxValue) {
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

static float DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

static bool ParseInteger(const std::string &text, int &value) {
  char *end = NULL;
  long parsed = strtol(text.c_str(), &end, 10);
  if (!end || *end != '\0')
    return false;
  value = static_cast<int>(parsed);
  return true;
}

static bool ParseFloat(const std::string &text, float &value) {
  char *end = NULL;
  float parsed = static_cast<float>(strtod(text.c_str(), &end));
  if (!end || *end != '\0')
    return false;
  value = parsed;
  return true;
}

static std::string Unquote(const std::string &value) {
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

static bool ParseMidiNoteText(const std::string &text, int &value) {
  if (text.empty())
    return false;

  int numeric = 0;
  if (ParseInteger(text, numeric)) {
    value = ClampInt(numeric, 0, 127);
    return true;
  }

  std::string lowered = ToLower(text);
  if (lowered.size() < 2)
    return false;

  int semitone = -1;
  switch (lowered[0]) {
  case 'c':
    semitone = 0;
    break;
  case 'd':
    semitone = 2;
    break;
  case 'e':
    semitone = 4;
    break;
  case 'f':
    semitone = 5;
    break;
  case 'g':
    semitone = 7;
    break;
  case 'a':
    semitone = 9;
    break;
  case 'b':
    semitone = 11;
    break;
  default:
    return false;
  }

  size_t index = 1;
  if (lowered[index] == '#') {
    semitone += 1;
    ++index;
  } else if (lowered[index] == 'b') {
    semitone -= 1;
    ++index;
  }

  int octave = 0;
  if (!ParseInteger(lowered.substr(index), octave))
    return false;

  value = ClampInt((octave + 1) * 12 + semitone, 0, 127);
  return true;
}

static std::string StripComments(const std::string &line) {
  bool inQuotes = false;
  for (size_t i = 0; i + 1 < line.size(); ++i) {
    char ch = line[i];
    if (ch == '"')
      inQuotes = !inQuotes;
    if (!inQuotes && ch == '/' && line[i + 1] == '/')
      return line.substr(0, i);
  }
  return line;
}

static std::vector<std::string> TokenizeLine(const std::string &line) {
  std::vector<std::string> tokens;
  std::string current;
  bool inQuotes = false;

  for (size_t i = 0; i < line.size(); ++i) {
    char ch = line[i];
    if (ch == '"') {
      current.push_back(ch);
      inQuotes = !inQuotes;
      continue;
    }

    if (!inQuotes && std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }

    if (!inQuotes && ch == '<') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      size_t end = line.find('>', i);
      if (end == std::string::npos) {
        current.push_back(ch);
      } else {
        tokens.push_back(line.substr(i, end - i + 1));
        i = end;
      }
      continue;
    }

    current.push_back(ch);
  }

  if (!current.empty())
    tokens.push_back(current);
  return tokens;
}

} // namespace

struct SfzEngine::Impl {
  enum TriggerMode { TRIGGER_ATTACK, TRIGGER_RELEASE };
  enum LoopMode { LOOP_NONE, LOOP_ONE_SHOT, LOOP_CONTINUOUS, LOOP_SUSTAIN };
  enum EnvPhase {
    ENV_DELAY,
    ENV_ATTACK,
    ENV_HOLD,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE,
    ENV_FINISHED
  };
  enum ParseSection {
    SECTION_NONE,
    SECTION_CONTROL,
    SECTION_GLOBAL,
    SECTION_MASTER,
    SECTION_GROUP,
    SECTION_REGION
  };

  struct Region {
    std::string samplePath;
    std::string sampleBaseDir;
    std::string defaultPath;
    std::string defaultPathBaseDir;
    int lokey;
    int hikey;
    int lovel;
    int hivel;
    int key;
    int pitchKeycenter;
    int transpose;
    int tuneCents;
    float volumeDb;
    float pan;
    float cutoffHz;
    int offset;
    int endFrame;
    int loopStart;
    int loopEnd;
    LoopMode loopMode;
    float ampegDelay;
    float ampegAttack;
    float ampegHold;
    float ampegDecay;
    float ampegSustain;
    float ampegRelease;
    int group;
    int offBy;
    TriggerMode trigger;
    int swLokey;
    int swHikey;
    int swLast;
    int locc[128];
    int hicc[128];
    int sampleIndex;
    std::string resolvedSamplePath;

    Region()
        : lokey(0), hikey(127), lovel(1), hivel(127), key(-1),
          pitchKeycenter(-1), transpose(0), tuneCents(0), volumeDb(0.0f),
          pan(0.0f), cutoffHz(0.0f), offset(0), endFrame(-1), loopStart(-1), loopEnd(-1),
          loopMode(LOOP_NONE), ampegDelay(0.0f), ampegAttack(0.0f),
          ampegHold(0.0f), ampegDecay(0.0f), ampegSustain(100.0f),
          ampegRelease(0.02f), group(0), offBy(0), trigger(TRIGGER_ATTACK),
          swLokey(-1), swHikey(-1), swLast(-1), sampleIndex(-1) {
      for (int i = 0; i < 128; ++i) {
        locc[i] = -1;
        hicc[i] = -1;
      }
    }
  };

  struct LoadedSample {
    std::string path;
    AudioSample audio;
  };

  struct ChannelState {
    int cc[128];
    int lastSwitchKey;
    bool sustainPedal;
    int pitchBend;
    float pitchBendRange;
    int rpnMsb;
    int rpnLsb;
    int dataEntryMsb;
    int dataEntryLsb;
    int lastVelocity[128];

    ChannelState()
        : lastSwitchKey(-1), sustainPedal(false), pitchBend(8192),
          pitchBendRange(kDefaultPitchBendRangeSemitones), rpnMsb(127),
          rpnLsb(127), dataEntryMsb(12), dataEntryLsb(0) {
      for (int i = 0; i < 128; ++i) {
        cc[i] = 0;
        lastVelocity[i] = 100;
      }
      cc[7] = 100;
      cc[10] = 64;
      cc[11] = 127;
    }
  };

  struct Voice {
    bool active;
    bool released;
    bool sustained;
    bool oneShot;
    int channel;
    int note;
    int group;
    int rawVelocity;
    int sampleIndex;
    int regionIndex;
    double position;
    double step;
    int startFrame;
    int endFrame;
    int loopStart;
    int loopEnd;
    LoopMode loopMode;
    float gainLeft;
    float gainRight;
    float filterAlpha;
    float filterStateLeft;
    float filterStateRight;
    float envLevel;
    float sustainLevel;
    float releaseStartLevel;
    float delaySeconds;
    float attackSeconds;
    float holdSeconds;
    float decaySeconds;
    float releaseSeconds;
    EnvPhase phase;
    int phaseSamples;
    int phasePosition;
    unsigned long long serial;

    Voice()
        : active(false), released(false), sustained(false), oneShot(false),
          channel(0), note(0), group(0), rawVelocity(0), sampleIndex(-1),
          regionIndex(-1), position(0.0), step(1.0), startFrame(0), endFrame(0),
          loopStart(-1), loopEnd(-1), loopMode(LOOP_NONE), gainLeft(0.0f),
          gainRight(0.0f), filterAlpha(1.0f), filterStateLeft(0.0f),
          filterStateRight(0.0f), envLevel(0.0f), sustainLevel(1.0f),
          releaseStartLevel(1.0f), delaySeconds(0.0f), attackSeconds(0.0f),
          holdSeconds(0.0f), decaySeconds(0.0f), releaseSeconds(0.02f),
          phase(ENV_FINISHED), phaseSamples(0), phasePosition(0), serial(0) {}
  };

  struct ParserContext {
    Region controlRegion;
    Region globalRegion;
    Region masterRegion;
    Region groupRegion;
    Region currentRegion;
    ParseSection currentSection;
    bool hasGlobal;
    bool hasMaster;
    bool hasGroup;
    bool regionOpen;
    std::vector<Region> parsedRegions;

    ParserContext()
        : currentSection(SECTION_NONE), hasGlobal(false), hasMaster(false),
          hasGroup(false), regionOpen(false) {}
  };

  compat::Mutex engineMutex;
  RuntimeSettings runtimeSettings;
  std::string resolvedSourcePath;
  std::string resolvedSourceFormat;
  SamplerDiagnostics diagnostics;
  int outputSampleRate;
  int maxVoices;
  std::vector<Region> regions;
  std::vector<LoadedSample> loadedSamples;
  std::map<std::string, int> sampleCache;
  std::vector<Voice> voices;
  std::vector<int> activeVoices;
  std::vector<int> freeVoices;
  ChannelState channels[16];
  unsigned long long voiceSerialCounter;
  std::set<std::string> uniqueWarnings;

  Impl()
      : outputSampleRate(44100), maxVoices(256), voiceSerialCounter(1) {
    runtimeSettings.velocityCurve = 2.4f;
    runtimeSettings.velocityFloor = 0.0f;
    runtimeSettings.velocityIgnoreBelow = 0;
    runtimeSettings.asyncNoteStarts = true;
  }

  void ResetChannels();
  void ResetVoices();
  void ClearLoadedData();
  void Warn(const std::string &message);
  float MapMidiVelocity(int velocity) const;
  static bool IsTriggerSupported(const std::string &value, TriggerMode &mode);
  static bool ParseLoopModeValue(const std::string &value, LoopMode &mode);
  bool ApplyOpcode(Region &region, const std::string &opcode,
                   const std::string &rawValue, const std::string &currentDir);
  Region BuildRegionBase(const ParserContext &ctx) const;
  void FinalizeCurrentRegion(ParserContext &ctx);
  bool ParseFile(const std::string &path, ParserContext &ctx, int depth);
  std::string ResolveRegionSamplePath(const Region &region) const;
  bool LoadRegionsAndSamples(const std::string &path);
  bool IsSwitchNote(int note) const;
  bool RegionMatches(const Region &region, int channel, int note, int velocity,
                     TriggerMode trigger) const;
  int AcquireVoice();
  void KillVoicesByGroup(int group);
  void BeginRelease(Voice &voice);
  double ComputeVoiceStep(const Region &region, int channel, int note,
                         int sampleRate) const;
  void UpdateVoicePitch(Voice &voice);
  void UpdateChannelPitch(int channel);
  float GetPitchBendSemitones(int channel) const;
  void ResetChannelControllers(int channel);
  void ApplyPitchBendRangeFromData(int channel);
  void StartVoice(const Region &region, int regionIndex, int channel, int note,
                  int velocity);
  void StartMatchedVoices(int channel, int note, int velocity,
                          TriggerMode trigger);
  void ReleaseNoteVoices(int channel, int note);
  void HandleNoteOn(int channel, int note, int velocity);
  void HandleNoteOff(int channel, int note);
  void HandleControlChange(int channel, int controller, int value);
  float AdvanceEnvelope(Voice &voice);
  void RenderVoice(Voice &voice, float *output, int numFrames);
  void CompactVoices();
};

void SfzEngine::Impl::ResetChannels() {
  for (int i = 0; i < 16; ++i)
    channels[i] = ChannelState();
}

void SfzEngine::Impl::ResetVoices() {
  activeVoices.clear();
  freeVoices.clear();
  voices.assign(static_cast<size_t>(maxVoices), Voice());
  for (int i = 0; i < maxVoices; ++i)
    freeVoices.push_back(maxVoices - 1 - i);
}

void SfzEngine::Impl::ClearLoadedData() {
  regions.clear();
  loadedSamples.clear();
  sampleCache.clear();
  diagnostics = SamplerDiagnostics();
  uniqueWarnings.clear();
  resolvedSourcePath.clear();
  resolvedSourceFormat.clear();
  ResetChannels();
  ResetVoices();
}

void SfzEngine::Impl::Warn(const std::string &message) {
  diagnostics.lastWarning = message;
  if (uniqueWarnings.insert(message).second)
    diagnostics.warningCount++;
  OutputDebugStringA(("SVMS SFZ: " + message + "\n").c_str());
}

float SfzEngine::Impl::MapMidiVelocity(int velocity) const {
  if (velocity <= 0)
    return 0.0f;
  if (velocity >= 127)
    return 1.0f;

  int ignoreBelow = ClampInt(runtimeSettings.velocityIgnoreBelow, 0, 126);
  if (velocity <= ignoreBelow)
    return 0.0f;

  float normalized =
      static_cast<float>(velocity - ignoreBelow) / (127.0f - ignoreBelow);
  float curve = ClampFloat(runtimeSettings.velocityCurve, 0.25f, 6.0f);
  float floor = ClampFloat(runtimeSettings.velocityFloor, 0.0f, 0.5f);
  return floor + (1.0f - floor) * std::pow(normalized, curve);
}

bool SfzEngine::Impl::IsTriggerSupported(const std::string &value,
                                         TriggerMode &mode) {
  std::string lowered = ToLower(value);
  if (lowered == "attack") {
    mode = TRIGGER_ATTACK;
    return true;
  }
  if (lowered == "release") {
    mode = TRIGGER_RELEASE;
    return true;
  }
  return false;
}

bool SfzEngine::Impl::ParseLoopModeValue(const std::string &value,
                                         LoopMode &mode) {
  std::string lowered = ToLower(value);
  if (lowered == "no_loop") {
    mode = LOOP_NONE;
    return true;
  }
  if (lowered == "one_shot") {
    mode = LOOP_ONE_SHOT;
    return true;
  }
  if (lowered == "loop_continuous") {
    mode = LOOP_CONTINUOUS;
    return true;
  }
  if (lowered == "loop_sustain") {
    mode = LOOP_SUSTAIN;
    return true;
  }
  return false;
}

bool SfzEngine::Impl::ApplyOpcode(Region &region, const std::string &opcode,
                                  const std::string &rawValue,
                                  const std::string &currentDir) {
  std::string key = ToLower(opcode);
  std::string value = Unquote(Trim(rawValue));
  int intValue = 0;
  float floatValue = 0.0f;

  if (key == "sample") {
    region.samplePath = NormalizeSlashes(value);
    region.sampleBaseDir = currentDir;
    return true;
  }
  if (key == "default_path") {
    region.defaultPath = NormalizeSlashes(value);
    region.defaultPathBaseDir = currentDir;
    return true;
  }
  if (key == "key") {
    if (!ParseMidiNoteText(value, intValue))
      return false;
    region.key = intValue;
    region.lokey = intValue;
    region.hikey = intValue;
    if (region.pitchKeycenter < 0)
      region.pitchKeycenter = intValue;
    return true;
  }
  if (key == "lokey") {
    if (!ParseMidiNoteText(value, intValue))
      return false;
    region.lokey = ClampInt(intValue, 0, 127);
    return true;
  }
  if (key == "hikey") {
    if (!ParseMidiNoteText(value, intValue))
      return false;
    region.hikey = ClampInt(intValue, 0, 127);
    return true;
  }
  if (key == "lovel") {
    if (!ParseInteger(value, intValue))
      return false;
    region.lovel = ClampInt(intValue, 1, 127);
    return true;
  }
  if (key == "hivel") {
    if (!ParseInteger(value, intValue))
      return false;
    region.hivel = ClampInt(intValue, 1, 127);
    return true;
  }
  if (key == "pitch_keycenter") {
    if (!ParseMidiNoteText(value, intValue))
      return false;
    region.pitchKeycenter = ClampInt(intValue, 0, 127);
    return true;
  }
  if (key == "transpose") {
    if (!ParseInteger(value, intValue))
      return false;
    region.transpose = intValue;
    return true;
  }
  if (key == "tune") {
    if (!ParseInteger(value, intValue))
      return false;
    region.tuneCents = intValue;
    return true;
  }
  if (key == "volume") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.volumeDb = floatValue;
    return true;
  }
  if (key == "pan") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.pan = ClampFloat(floatValue, -100.0f, 100.0f);
    return true;
  }
  if (key == "cutoff") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.cutoffHz = (std::max)(0.0f, floatValue);
    return true;
  }
  if (key == "offset") {
    if (!ParseInteger(value, intValue))
      return false;
    region.offset = (std::max)(0, intValue);
    return true;
  }
  if (key == "end") {
    if (!ParseInteger(value, intValue))
      return false;
    region.endFrame = intValue;
    return true;
  }
  if (key == "loop_start") {
    if (!ParseInteger(value, intValue))
      return false;
    region.loopStart = intValue;
    return true;
  }
  if (key == "loop_end") {
    if (!ParseInteger(value, intValue))
      return false;
    region.loopEnd = intValue;
    return true;
  }
  if (key == "loop_mode") {
    LoopMode loopMode = LOOP_NONE;
    if (!ParseLoopModeValue(value, loopMode)) {
      Warn("Unsupported loop_mode value: " + value);
      return true;
    }
    region.loopMode = loopMode;
    return true;
  }
  if (key == "ampeg_delay") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.ampegDelay = (std::max)(0.0f, floatValue);
    return true;
  }
  if (key == "ampeg_attack") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.ampegAttack = (std::max)(0.0f, floatValue);
    return true;
  }
  if (key == "ampeg_hold") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.ampegHold = (std::max)(0.0f, floatValue);
    return true;
  }
  if (key == "ampeg_decay") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.ampegDecay = (std::max)(0.0f, floatValue);
    return true;
  }
  if (key == "ampeg_sustain") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.ampegSustain = ClampFloat(floatValue, 0.0f, 100.0f);
    return true;
  }
  if (key == "ampeg_release") {
    if (!ParseFloat(value, floatValue))
      return false;
    region.ampegRelease = (std::max)(0.0f, floatValue);
    return true;
  }
  if (key == "group") {
    if (!ParseInteger(value, intValue))
      return false;
    region.group = intValue;
    return true;
  }
  if (key == "off_by") {
    if (!ParseInteger(value, intValue))
      return false;
    region.offBy = intValue;
    return true;
  }
  if (key == "trigger") {
    TriggerMode triggerMode = TRIGGER_ATTACK;
    if (!IsTriggerSupported(value, triggerMode)) {
      Warn("Unsupported trigger mode: " + value);
      return true;
    }
    region.trigger = triggerMode;
    return true;
  }
  if (key == "sw_lokey") {
    if (!ParseMidiNoteText(value, intValue))
      return false;
    region.swLokey = ClampInt(intValue, 0, 127);
    return true;
  }
  if (key == "sw_hikey") {
    if (!ParseMidiNoteText(value, intValue))
      return false;
    region.swHikey = ClampInt(intValue, 0, 127);
    return true;
  }
  if (key == "sw_last") {
    if (!ParseMidiNoteText(value, intValue))
      return false;
    region.swLast = ClampInt(intValue, 0, 127);
    return true;
  }
  if (key.size() > 4 && key.compare(0, 4, "locc") == 0) {
    int cc = atoi(key.c_str() + 4);
    if (cc < 0 || cc > 127 || !ParseInteger(value, intValue))
      return false;
    region.locc[cc] = ClampInt(intValue, 0, 127);
    return true;
  }
  if (key.size() > 4 && key.compare(0, 4, "hicc") == 0) {
    int cc = atoi(key.c_str() + 4);
    if (cc < 0 || cc > 127 || !ParseInteger(value, intValue))
      return false;
    region.hicc[cc] = ClampInt(intValue, 0, 127);
    return true;
  }

  Warn("Unsupported opcode: " + key);
  return true;
}

SfzEngine::Impl::Region
SfzEngine::Impl::BuildRegionBase(const ParserContext &ctx) const {
  if (ctx.hasGroup)
    return ctx.groupRegion;
  if (ctx.hasMaster)
    return ctx.masterRegion;
  if (ctx.hasGlobal)
    return ctx.globalRegion;
  return ctx.controlRegion;
}

void SfzEngine::Impl::FinalizeCurrentRegion(ParserContext &ctx) {
  if (!ctx.regionOpen)
    return;
  if (ctx.currentRegion.samplePath.empty()) {
    Warn("Skipping region without sample opcode");
  } else {
    ctx.currentRegion.lokey = ClampInt(ctx.currentRegion.lokey, 0, 127);
    ctx.currentRegion.hikey = ClampInt(ctx.currentRegion.hikey, 0, 127);
    if (ctx.currentRegion.hikey < ctx.currentRegion.lokey)
      std::swap(ctx.currentRegion.hikey, ctx.currentRegion.lokey);
    if (ctx.currentRegion.hivel < ctx.currentRegion.lovel)
      std::swap(ctx.currentRegion.hivel, ctx.currentRegion.lovel);
    ctx.parsedRegions.push_back(ctx.currentRegion);
  }
  ctx.regionOpen = false;
}

bool SfzEngine::Impl::ParseFile(const std::string &path, ParserContext &ctx,
                                int depth) {
  if (depth > 16) {
    Warn("Include depth exceeded while parsing " + path);
    return false;
  }

  FILE *file = fopen(path.c_str(), "rb");
  if (!file) {
    Warn("Could not open SFZ file: " + path);
    return false;
  }

  char lineBuffer[4096];
  std::string currentDir = GetDirectoryName(path);

  while (fgets(lineBuffer, sizeof(lineBuffer), file)) {
    std::string stripped = Trim(StripComments(lineBuffer));
    if (stripped.empty())
      continue;

    if (stripped.compare(0, 8, "#include") == 0) {
      size_t quoteStart = stripped.find('"');
      size_t quoteEnd = stripped.find_last_of('"');
      if (quoteStart == std::string::npos || quoteEnd <= quoteStart) {
        Warn("Malformed #include in " + path);
        continue;
      }
      std::string includePath =
          stripped.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
      std::string resolvedInclude =
          CanonicalizePath(JoinPath(currentDir, includePath));
      if (!FileExists(resolvedInclude)) {
        Warn("Included SFZ not found: " + resolvedInclude);
        continue;
      }
      ParseFile(resolvedInclude, ctx, depth + 1);
      continue;
    }

    std::vector<std::string> tokens = TokenizeLine(stripped);
    for (size_t i = 0; i < tokens.size(); ++i) {
      const std::string &token = tokens[i];
      if (token.size() > 2 && token.front() == '<' && token.back() == '>') {
        std::string sectionName = ToLower(token.substr(1, token.size() - 2));
        if (sectionName != "region")
          FinalizeCurrentRegion(ctx);

        if (sectionName == "control") {
          ctx.currentSection = SECTION_CONTROL;
        } else if (sectionName == "global") {
          ctx.globalRegion = ctx.controlRegion;
          ctx.currentSection = SECTION_GLOBAL;
          ctx.hasGlobal = true;
          ctx.hasMaster = false;
          ctx.hasGroup = false;
        } else if (sectionName == "master") {
          ctx.masterRegion = ctx.hasGlobal ? ctx.globalRegion : ctx.controlRegion;
          ctx.currentSection = SECTION_MASTER;
          ctx.hasMaster = true;
          ctx.hasGroup = false;
        } else if (sectionName == "group") {
          ctx.groupRegion = BuildRegionBase(ctx);
          ctx.currentSection = SECTION_GROUP;
          ctx.hasGroup = true;
        } else if (sectionName == "region") {
          FinalizeCurrentRegion(ctx);
          ctx.currentRegion = BuildRegionBase(ctx);
          ctx.currentSection = SECTION_REGION;
          ctx.regionOpen = true;
        } else {
          Warn("Unsupported section <" + sectionName + ">");
          ctx.currentSection = SECTION_NONE;
        }
        continue;
      }

      size_t eq = token.find('=');
      if (eq == std::string::npos) {
        Warn("Ignoring malformed token: " + token);
        continue;
      }

      std::string opcode = Trim(token.substr(0, eq));
      std::string value = Trim(token.substr(eq + 1));
      Region *target = NULL;
      switch (ctx.currentSection) {
      case SECTION_CONTROL:
        target = &ctx.controlRegion;
        break;
      case SECTION_GLOBAL:
        target = &ctx.globalRegion;
        break;
      case SECTION_MASTER:
        target = &ctx.masterRegion;
        break;
      case SECTION_GROUP:
        target = &ctx.groupRegion;
        break;
      case SECTION_REGION:
        target = &ctx.currentRegion;
        break;
      default:
        ctx.currentSection = SECTION_CONTROL;
        target = &ctx.controlRegion;
        break;
      }

      if (!ApplyOpcode(*target, opcode, value, currentDir))
        Warn("Invalid opcode value: " + opcode + "=" + value);
    }
  }

  fclose(file);
  return true;
}

std::string SfzEngine::Impl::ResolveRegionSamplePath(const Region &region) const {
  if (region.samplePath.empty())
    return std::string();

  if (IsAbsolutePath(region.samplePath))
    return CanonicalizePath(region.samplePath);

  std::string baseDir = region.sampleBaseDir;
  if (!region.defaultPath.empty()) {
    if (IsAbsolutePath(region.defaultPath)) {
      baseDir = region.defaultPath;
    } else {
      std::string defaultBase = region.defaultPathBaseDir.empty()
                                    ? region.sampleBaseDir
                                    : region.defaultPathBaseDir;
      baseDir = JoinPath(defaultBase, region.defaultPath);
    }
  }

  return CanonicalizePath(JoinPath(baseDir, region.samplePath));
}

bool SfzEngine::Impl::LoadRegionsAndSamples(const std::string &path) {
  ParserContext ctx;
  if (!ParseFile(path, ctx, 0)) {
    if (ctx.parsedRegions.empty())
      return false;
  }
  FinalizeCurrentRegion(ctx);
  regions.swap(ctx.parsedRegions);

  if (regions.empty()) {
    Warn("No playable <region> entries were found");
    return false;
  }

  for (size_t i = 0; i < regions.size(); ++i) {
    Region &region = regions[i];
    region.resolvedSamplePath = ResolveRegionSamplePath(region);
    if (region.resolvedSamplePath.empty() || !FileExists(region.resolvedSamplePath)) {
      diagnostics.failedSampleCount++;
      Warn("Sample not found: " + region.resolvedSamplePath);
      region.sampleIndex = -1;
      continue;
    }

    std::map<std::string, int>::const_iterator cached =
        sampleCache.find(region.resolvedSamplePath);
    if (cached != sampleCache.end()) {
      region.sampleIndex = cached->second;
      continue;
    }

    AudioSample sample;
    std::string errorMessage;
    if (!WavLoader::LoadAudioFile(region.resolvedSamplePath, sample,
                                  &errorMessage)) {
      diagnostics.failedSampleCount++;
      Warn("Sample decode failed: " + region.resolvedSamplePath + " (" +
           errorMessage + ")");
      region.sampleIndex = -1;
      continue;
    }

    LoadedSample loaded;
    loaded.path = region.resolvedSamplePath;
    loaded.audio = sample;
    int sampleIndex = static_cast<int>(loadedSamples.size());
    loadedSamples.push_back(loaded);
    sampleCache[region.resolvedSamplePath] = sampleIndex;
    diagnostics.loadedSampleCount =
        static_cast<unsigned int>(loadedSamples.size());
    region.sampleIndex = sampleIndex;
  }

  size_t playable = 0;
  for (size_t i = 0; i < regions.size(); ++i) {
    if (regions[i].sampleIndex >= 0)
      playable++;
  }
  if (playable == 0) {
    Warn("No playable regions were loaded");
    return false;
  }

  return true;
}

bool SfzEngine::Impl::IsSwitchNote(int note) const {
  for (size_t i = 0; i < regions.size(); ++i) {
    const Region &region = regions[i];
    if ((region.swLokey >= 0 && region.swHikey >= 0 &&
         note >= region.swLokey && note <= region.swHikey) ||
        region.swLast == note) {
      return true;
    }
  }
  return false;
}

bool SfzEngine::Impl::RegionMatches(const Region &region, int channel, int note,
                                    int velocity, TriggerMode trigger) const {
  if (region.sampleIndex < 0 || region.trigger != trigger)
    return false;
  if (note < region.lokey || note > region.hikey)
    return false;
  if (velocity < region.lovel || velocity > region.hivel)
    return false;

  const ChannelState &channelState = channels[channel];
  if (region.swLast >= 0 && channelState.lastSwitchKey != region.swLast)
    return false;
  if (region.swLokey >= 0 && region.swHikey >= 0) {
    if (channelState.lastSwitchKey < region.swLokey ||
        channelState.lastSwitchKey > region.swHikey)
      return false;
  }

  for (int cc = 0; cc < 128; ++cc) {
    if (region.locc[cc] >= 0 && channelState.cc[cc] < region.locc[cc])
      return false;
    if (region.hicc[cc] >= 0 && channelState.cc[cc] > region.hicc[cc])
      return false;
  }

  return true;
}

int SfzEngine::Impl::AcquireVoice() {
  if (!freeVoices.empty()) {
    int index = freeVoices.back();
    freeVoices.pop_back();
    return index;
  }

  if (activeVoices.empty())
    return -1;

  size_t oldestPos = 0;
  unsigned long long oldestSerial = voices[activeVoices[0]].serial;
  for (size_t i = 1; i < activeVoices.size(); ++i) {
    unsigned long long serial = voices[activeVoices[i]].serial;
    if (serial < oldestSerial) {
      oldestSerial = serial;
      oldestPos = i;
    }
  }

  int index = activeVoices[oldestPos];
  activeVoices.erase(activeVoices.begin() + static_cast<std::ptrdiff_t>(oldestPos));
  voices[index] = Voice();
  return index;
}

double SfzEngine::Impl::ComputeVoiceStep(const Region &region, int channel,
                                         int note, int sampleRate) const {
  int rootKey = region.pitchKeycenter >= 0
                    ? region.pitchKeycenter
                    : (region.key >= 0 ? region.key : 60);
  float noteSemitones =
      static_cast<float>(note - rootKey + region.transpose) +
      region.tuneCents / 100.0f + GetPitchBendSemitones(channel);
  return std::pow(2.0, noteSemitones / 12.0f) *
         (static_cast<double>(sampleRate) / outputSampleRate);
}

void SfzEngine::Impl::UpdateVoicePitch(Voice &voice) {
  if (!voice.active || voice.regionIndex < 0 ||
      voice.regionIndex >= static_cast<int>(regions.size()) ||
      voice.sampleIndex < 0 ||
      voice.sampleIndex >= static_cast<int>(loadedSamples.size()))
    return;
  const Region &region = regions[voice.regionIndex];
  const LoadedSample &sample = loadedSamples[voice.sampleIndex];
  voice.step =
      ComputeVoiceStep(region, voice.channel, voice.note, sample.audio.sampleRate);
}

void SfzEngine::Impl::UpdateChannelPitch(int channel) {
  for (size_t i = 0; i < activeVoices.size(); ++i) {
    Voice &voice = voices[activeVoices[i]];
    if (voice.active && voice.channel == channel)
      UpdateVoicePitch(voice);
  }
}

void SfzEngine::Impl::KillVoicesByGroup(int group) {
  if (group <= 0)
    return;
  for (size_t i = 0; i < activeVoices.size(); ++i) {
    Voice &voice = voices[activeVoices[i]];
    if (voice.group == group)
      voice.active = false;
  }
}

void SfzEngine::Impl::BeginRelease(Voice &voice) {
  if (!voice.active || voice.oneShot)
    return;
  voice.released = true;
  voice.sustained = false;
  voice.releaseStartLevel = voice.envLevel;
  voice.phase = ENV_RELEASE;
  voice.phaseSamples =
      (std::max)(1, static_cast<int>(voice.releaseSeconds * outputSampleRate));
  voice.phasePosition = 0;
}

float SfzEngine::Impl::GetPitchBendSemitones(int channel) const {
  float normalized =
      static_cast<float>(channels[channel].pitchBend - 8192) / 8192.0f;
  return normalized * channels[channel].pitchBendRange;
}

void SfzEngine::Impl::ResetChannelControllers(int channel) {
  bool hadSustain = channels[channel].sustainPedal;
  channels[channel] = ChannelState();
  if (hadSustain) {
    for (size_t i = 0; i < activeVoices.size(); ++i) {
      Voice &voice = voices[activeVoices[i]];
      if (voice.active && voice.channel == channel && voice.sustained)
        BeginRelease(voice);
    }
  }
  UpdateChannelPitch(channel);
}

void SfzEngine::Impl::ApplyPitchBendRangeFromData(int channel) {
  ChannelState &state = channels[channel];
  if (state.rpnMsb != 0 || state.rpnLsb != 0)
    return;
  float pitchRange = state.dataEntryMsb + 0.01f * state.dataEntryLsb;
  state.pitchBendRange = ClampFloat(pitchRange, 0.0f, 96.0f);
  UpdateChannelPitch(channel);
}

void SfzEngine::Impl::StartVoice(const Region &region, int regionIndex,
                                 int channel, int note, int velocity) {
  float mappedVelocity = MapMidiVelocity(velocity);
  if (mappedVelocity <= 0.0f)
    return;

  if (region.offBy > 0)
    KillVoicesByGroup(region.offBy);

  int voiceIndex = AcquireVoice();
  if (voiceIndex < 0)
    return;

  const LoadedSample &sample = loadedSamples[region.sampleIndex];
  Voice &voice = voices[voiceIndex];
  voice = Voice();
  voice.active = true;
  voice.channel = channel;
  voice.note = note;
  voice.group = region.group;
  voice.rawVelocity = velocity;
  voice.sampleIndex = region.sampleIndex;
  voice.regionIndex = regionIndex;
  voice.position = static_cast<double>(region.offset);
  voice.startFrame = ClampInt(region.offset, 0, sample.audio.frameCount);
  voice.endFrame =
      region.endFrame >= 0 ? ClampInt(region.endFrame + 1, 0, sample.audio.frameCount)
                           : sample.audio.frameCount;
  voice.loopStart =
      region.loopStart >= 0 ? ClampInt(region.loopStart, 0, sample.audio.frameCount)
                            : voice.startFrame;
  voice.loopEnd =
      region.loopEnd >= 0 ? ClampInt(region.loopEnd + 1, 0, sample.audio.frameCount)
                          : voice.endFrame;
  if (voice.endFrame <= voice.startFrame)
    voice.endFrame = sample.audio.frameCount;
  voice.loopMode = region.loopMode;
  if (voice.loopEnd <= voice.loopStart)
    voice.loopMode = LOOP_NONE;
  voice.oneShot = (voice.loopMode == LOOP_ONE_SHOT);

  voice.step =
      ComputeVoiceStep(region, channel, note, sample.audio.sampleRate);

  float gain = mappedVelocity * DbToLinear(region.volumeDb);
  float pan = ClampFloat(region.pan, -100.0f, 100.0f) / 100.0f;
  float panNorm = (pan + 1.0f) * 0.5f;
  voice.gainLeft = gain * std::sqrt(1.0f - panNorm);
  voice.gainRight = gain * std::sqrt(panNorm);
  if (region.cutoffHz > 0.0f) {
    float nyquist = outputSampleRate * 0.5f;
    float cutoff = ClampFloat(region.cutoffHz, 5.0f, nyquist * 0.95f);
    voice.filterAlpha =
        1.0f - std::exp(-2.0f * 3.1415926535f * cutoff / outputSampleRate);
  } else {
    voice.filterAlpha = 1.0f;
  }

  voice.sustainLevel = ClampFloat(region.ampegSustain / 100.0f, 0.0f, 1.0f);
  voice.delaySeconds = region.ampegDelay;
  voice.attackSeconds = region.ampegAttack;
  voice.holdSeconds = region.ampegHold;
  voice.decaySeconds = region.ampegDecay;
  voice.releaseSeconds = (std::max)(0.005f, region.ampegRelease);
  voice.serial = voiceSerialCounter++;

  if (voice.delaySeconds > 0.0f) {
    voice.phase = ENV_DELAY;
    voice.phaseSamples =
        (std::max)(1, static_cast<int>(voice.delaySeconds * outputSampleRate));
  } else if (voice.attackSeconds > 0.0f) {
    voice.phase = ENV_ATTACK;
    voice.phaseSamples =
        (std::max)(1, static_cast<int>(voice.attackSeconds * outputSampleRate));
  } else if (voice.holdSeconds > 0.0f) {
    voice.envLevel = 1.0f;
    voice.phase = ENV_HOLD;
    voice.phaseSamples =
        (std::max)(1, static_cast<int>(voice.holdSeconds * outputSampleRate));
  } else if (voice.decaySeconds > 0.0f) {
    voice.envLevel = 1.0f;
    voice.phase = ENV_DECAY;
    voice.phaseSamples =
        (std::max)(1, static_cast<int>(voice.decaySeconds * outputSampleRate));
  } else {
    voice.envLevel = voice.sustainLevel;
    voice.phase = ENV_SUSTAIN;
    voice.phaseSamples = 0;
  }

  activeVoices.push_back(voiceIndex);
}

void SfzEngine::Impl::StartMatchedVoices(int channel, int note, int velocity,
                                         TriggerMode trigger) {
  for (size_t i = 0; i < regions.size(); ++i) {
    const Region &region = regions[i];
    if (RegionMatches(region, channel, note, velocity, trigger))
      StartVoice(region, static_cast<int>(i), channel, note, velocity);
  }
}

void SfzEngine::Impl::ReleaseNoteVoices(int channel, int note) {
  for (size_t i = 0; i < activeVoices.size(); ++i) {
    Voice &voice = voices[activeVoices[i]];
    if (!voice.active || voice.channel != channel || voice.note != note)
      continue;
    if (channels[channel].sustainPedal)
      voice.sustained = true;
    else
      BeginRelease(voice);
  }
}

void SfzEngine::Impl::HandleNoteOn(int channel, int note, int velocity) {
  if (velocity <= 0) {
    HandleNoteOff(channel, note);
    return;
  }

  channels[channel].lastVelocity[note] = velocity;
  if (IsSwitchNote(note))
    channels[channel].lastSwitchKey = note;
  StartMatchedVoices(channel, note, velocity, TRIGGER_ATTACK);
}

void SfzEngine::Impl::HandleNoteOff(int channel, int note) {
  int lastVelocity = channels[channel].lastVelocity[note];
  ReleaseNoteVoices(channel, note);
  StartMatchedVoices(channel, note, lastVelocity, TRIGGER_RELEASE);
}

void SfzEngine::Impl::HandleControlChange(int channel, int controller,
                                          int value) {
  channels[channel].cc[controller] = value;
  if (controller == 64) {
    bool sustainNow = value >= 64;
    if (channels[channel].sustainPedal && !sustainNow) {
      for (size_t i = 0; i < activeVoices.size(); ++i) {
        Voice &voice = voices[activeVoices[i]];
        if (voice.active && voice.channel == channel && voice.sustained)
          BeginRelease(voice);
      }
    }
    channels[channel].sustainPedal = sustainNow;
    return;
  }

  if (controller == 100) {
    channels[channel].rpnLsb = value;
    return;
  }
  if (controller == 101) {
    channels[channel].rpnMsb = value;
    return;
  }
  if (controller == 6) {
    channels[channel].dataEntryMsb = value;
    ApplyPitchBendRangeFromData(channel);
    return;
  }
  if (controller == 38) {
    channels[channel].dataEntryLsb = value;
    ApplyPitchBendRangeFromData(channel);
    return;
  }
  if (controller == 121) {
    ResetChannelControllers(channel);
  }
}

float SfzEngine::Impl::AdvanceEnvelope(Voice &voice) {
  switch (voice.phase) {
  case ENV_DELAY:
    voice.envLevel = 0.0f;
    if (++voice.phasePosition >= voice.phaseSamples) {
      voice.phasePosition = 0;
      if (voice.attackSeconds > 0.0f) {
        voice.phase = ENV_ATTACK;
        voice.phaseSamples =
            (std::max)(1, static_cast<int>(voice.attackSeconds * outputSampleRate));
      } else if (voice.holdSeconds > 0.0f) {
        voice.phase = ENV_HOLD;
        voice.phaseSamples =
            (std::max)(1, static_cast<int>(voice.holdSeconds * outputSampleRate));
        voice.envLevel = 1.0f;
      } else if (voice.decaySeconds > 0.0f) {
        voice.phase = ENV_DECAY;
        voice.phaseSamples =
            (std::max)(1, static_cast<int>(voice.decaySeconds * outputSampleRate));
        voice.envLevel = 1.0f;
      } else {
        voice.phase = ENV_SUSTAIN;
        voice.envLevel = voice.sustainLevel;
      }
    }
    break;
  case ENV_ATTACK:
    voice.envLevel =
        static_cast<float>(voice.phasePosition + 1) / voice.phaseSamples;
    if (++voice.phasePosition >= voice.phaseSamples) {
      voice.phasePosition = 0;
      voice.envLevel = 1.0f;
      if (voice.holdSeconds > 0.0f) {
        voice.phase = ENV_HOLD;
        voice.phaseSamples =
            (std::max)(1, static_cast<int>(voice.holdSeconds * outputSampleRate));
      } else if (voice.decaySeconds > 0.0f) {
        voice.phase = ENV_DECAY;
        voice.phaseSamples =
            (std::max)(1, static_cast<int>(voice.decaySeconds * outputSampleRate));
      } else {
        voice.phase = ENV_SUSTAIN;
        voice.envLevel = voice.sustainLevel;
      }
    }
    break;
  case ENV_HOLD:
    voice.envLevel = 1.0f;
    if (++voice.phasePosition >= voice.phaseSamples) {
      voice.phasePosition = 0;
      if (voice.decaySeconds > 0.0f) {
        voice.phase = ENV_DECAY;
        voice.phaseSamples =
            (std::max)(1, static_cast<int>(voice.decaySeconds * outputSampleRate));
      } else {
        voice.phase = ENV_SUSTAIN;
        voice.envLevel = voice.sustainLevel;
      }
    }
    break;
  case ENV_DECAY: {
    float t = static_cast<float>(voice.phasePosition + 1) / voice.phaseSamples;
    voice.envLevel = 1.0f + (voice.sustainLevel - 1.0f) * t;
    if (++voice.phasePosition >= voice.phaseSamples) {
      voice.phase = ENV_SUSTAIN;
      voice.envLevel = voice.sustainLevel;
    }
    break;
  }
  case ENV_SUSTAIN:
    voice.envLevel = voice.sustainLevel;
    break;
  case ENV_RELEASE: {
    float t = static_cast<float>(voice.phasePosition + 1) / voice.phaseSamples;
    voice.envLevel = voice.releaseStartLevel * (1.0f - t);
    if (++voice.phasePosition >= voice.phaseSamples || voice.envLevel <= 0.0f) {
      voice.phase = ENV_FINISHED;
      voice.envLevel = 0.0f;
    }
    break;
  }
  case ENV_FINISHED:
    voice.envLevel = 0.0f;
    break;
  }
  return voice.envLevel;
}

void SfzEngine::Impl::RenderVoice(Voice &voice, float *output, int numFrames) {
  if (!voice.active)
    return;

  const LoadedSample &sample = loadedSamples[voice.sampleIndex];
  const int channelsInSample = sample.audio.channels;
  const int totalFrames = sample.audio.frameCount;
  const float *data = sample.audio.data.data();

  if (voice.position < voice.startFrame)
    voice.position = static_cast<double>(voice.startFrame);

  for (int frame = 0; frame < numFrames; ++frame) {
    if (!voice.active)
      break;

    if (voice.phase == ENV_FINISHED) {
      voice.active = false;
      break;
    }

    if (voice.position >= voice.endFrame) {
      bool shouldLoop =
          (voice.loopMode == LOOP_CONTINUOUS) ||
          (voice.loopMode == LOOP_SUSTAIN && !voice.released && !voice.oneShot);
      if (shouldLoop && voice.loopEnd > voice.loopStart) {
        double loopLength = static_cast<double>(voice.loopEnd - voice.loopStart);
        if (loopLength > 1.0) {
          voice.position = voice.loopStart +
                           std::fmod(voice.position - voice.loopStart, loopLength);
        }
      } else {
        voice.active = false;
        break;
      }
    }

    int frameIndex = static_cast<int>(voice.position);
    int nextFrame = frameIndex + 1;
    if (nextFrame >= totalFrames)
      nextFrame = totalFrames - 1;
    float frac = static_cast<float>(voice.position - frameIndex);

    float sampleLeft = 0.0f;
    float sampleRight = 0.0f;
    if (channelsInSample == 1) {
      float s0 = data[frameIndex];
      float s1 = data[nextFrame];
      float sampleValue = s0 + (s1 - s0) * frac;
      sampleLeft = sampleValue;
      sampleRight = sampleValue;
    } else {
      const float *src0 = &data[static_cast<size_t>(frameIndex) * 2];
      const float *src1 = &data[static_cast<size_t>(nextFrame) * 2];
      sampleLeft = src0[0] + (src1[0] - src0[0]) * frac;
      sampleRight = src0[1] + (src1[1] - src0[1]) * frac;
    }

    if (voice.filterAlpha < 0.9999f) {
      voice.filterStateLeft +=
          voice.filterAlpha * (sampleLeft - voice.filterStateLeft);
      voice.filterStateRight +=
          voice.filterAlpha * (sampleRight - voice.filterStateRight);
      sampleLeft = voice.filterStateLeft;
      sampleRight = voice.filterStateRight;
    }

    float env = AdvanceEnvelope(voice);
    output[frame * 2 + 0] += sampleLeft * voice.gainLeft * env;
    output[frame * 2 + 1] += sampleRight * voice.gainRight * env;
    voice.position += voice.step;
  }
}

void SfzEngine::Impl::CompactVoices() {
  std::vector<int> stillActive;
  stillActive.reserve(activeVoices.size());
  for (size_t i = 0; i < activeVoices.size(); ++i) {
    int voiceIndex = activeVoices[i];
    if (voices[voiceIndex].active)
      stillActive.push_back(voiceIndex);
    else
      freeVoices.push_back(voiceIndex);
  }
  activeVoices.swap(stillActive);
}

SfzEngine::SfzEngine() : impl(new Impl()) {}

SfzEngine::~SfzEngine() {
  Shutdown(true);
  delete impl;
  impl = 0;
}

bool SfzEngine::Initialize(const SamplerInitParams &params) {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  impl->ClearLoadedData();
  impl->runtimeSettings = params.runtimeSettings;
  impl->outputSampleRate = params.sampleRate > 0 ? params.sampleRate : 44100;
  impl->maxVoices = params.maxVoices > 0 ? params.maxVoices : 256;
  impl->resolvedSourcePath = CanonicalizePath(params.sourcePath);
  impl->resolvedSourceFormat = "sfz";
  impl->ResetChannels();
  impl->ResetVoices();
  if (!FileExists(impl->resolvedSourcePath)) {
    impl->Warn("SFZ source file not found: " + impl->resolvedSourcePath);
    return false;
  }
  return impl->LoadRegionsAndSamples(impl->resolvedSourcePath);
}

void SfzEngine::Shutdown(bool) {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  impl->ClearLoadedData();
}

void SfzEngine::Reset() {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  impl->ResetChannels();
  impl->ResetVoices();
}

void SfzEngine::ReloadRuntimeSettings(const RuntimeSettings &settings) {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  impl->runtimeSettings = settings;
}

void SfzEngine::BeginRenderBlock() {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  impl->diagnostics.voiceStartMs = 0.0f;
  impl->diagnostics.sampleRenderMs = 0.0f;
  impl->diagnostics.noteOnEventsThisBlock = 0;
  impl->diagnostics.noteOffEventsThisBlock = 0;
}

void SfzEngine::ProcessMidiEvent(const MidiEvent &event) {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  if (impl->regions.empty())
    return;

  int channel = ClampInt(event.channel, 0, 15);
  switch (event.type) {
  case MidiEvent::NOTE_ON:
    impl->diagnostics.noteOnEventsThisBlock++;
    impl->HandleNoteOn(channel, ClampInt(event.data1, 0, 127),
                       ClampInt(event.data2, 0, 127));
    break;
  case MidiEvent::NOTE_OFF:
    impl->diagnostics.noteOffEventsThisBlock++;
    impl->HandleNoteOff(channel, ClampInt(event.data1, 0, 127));
    break;
  case MidiEvent::PROGRAM_CHANGE:
    break;
  case MidiEvent::CONTROL_CHANGE:
    impl->HandleControlChange(channel, ClampInt(event.data1, 0, 127),
                              ClampInt(event.data2, 0, 127));
    break;
  case MidiEvent::PITCH_BEND:
    impl->channels[channel].pitchBend = ClampInt(event.data1, 0, 16383);
    impl->UpdateChannelPitch(channel);
    break;
  case MidiEvent::RESET:
    impl->ResetChannels();
    impl->ResetVoices();
    break;
  }
}

void SfzEngine::Render(float *output, int numFrames) {
  if (numFrames <= 0)
    return;
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  LARGE_INTEGER freq = {};
  LARGE_INTEGER start = {};
  LARGE_INTEGER end = {};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  std::fill(output, output + numFrames * 2, 0.0f);
  for (size_t i = 0; i < impl->activeVoices.size(); ++i)
    impl->RenderVoice(impl->voices[impl->activeVoices[i]], output, numFrames);
  impl->CompactVoices();
  QueryPerformanceCounter(&end);
  if (freq.QuadPart > 0) {
    impl->diagnostics.sampleRenderMs =
        (float)((double)(end.QuadPart - start.QuadPart) * 1000.0 /
                (double)freq.QuadPart);
  }
}

std::string SfzEngine::GetResolvedSourcePath() const {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  return impl->resolvedSourcePath;
}

std::string SfzEngine::GetResolvedSourceFormat() const {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  return impl->resolvedSourceFormat;
}

std::string SfzEngine::GetEngineName() const { return "sfz"; }

DWORD SfzEngine::GetActiveVoiceStats(DWORD *channelCounts, int count) const {
  if (channelCounts && count > 0) {
    for (int i = 0; i < count; ++i)
      channelCounts[i] = 0;
  }

  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  if (channelCounts && count > 0) {
    for (size_t i = 0; i < impl->activeVoices.size(); ++i) {
      const Impl::Voice &voice = impl->voices[impl->activeVoices[i]];
      if (voice.active && voice.channel >= 0 && voice.channel < count)
        channelCounts[voice.channel]++;
    }
  }
  return static_cast<DWORD>(impl->activeVoices.size());
}

SamplerDiagnostics SfzEngine::GetDiagnostics() const {
  compat::LockGuard<compat::Mutex> lock(impl->engineMutex);
  SamplerDiagnostics diagnostics = impl->diagnostics;
  for (int i = 0; i < 16; ++i)
    diagnostics.pitchBendRange[i] = impl->channels[i].pitchBendRange;
  return diagnostics;
}
