#include "BassMidiEngine.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

typedef DWORD HSTREAM;
typedef DWORD HSOUNDFONT;
typedef unsigned long long QWORD;

static const DWORD BASSVERSION = 0x204;
static const DWORD BASS_UNICODE = 0x80000000u;
static const DWORD BASS_SAMPLE_FLOAT = 0x100u;
static const DWORD BASS_STREAM_DECODE = 0x200000u;
static const DWORD BASS_ATTRIB_MIDI_VOICES_ACTIVE = 0x12004u;
static const DWORD BASS_CONFIG_MIDI_VOICES = 0x10401u;
static const DWORD MIDI_EVENT_PITCHRANGE = 5u;
static const DWORD MIDI_EVENT_RESET = 17u;
static const DWORD kDefaultPitchBendRangeSemitones = 2u;

struct BASS_MIDI_FONT {
  HSOUNDFONT font;
  int preset;
  int bank;
};

struct BASS_MIDI_FONTINFO {
  const char *name;
  const char *copyright;
  const char *comment;
  DWORD presets;
  DWORD samsize;
  DWORD samload;
  DWORD samtype;
};

typedef DWORD(WINAPI *BASS_GetVersionFunc)(void);
typedef int(WINAPI *BASS_ErrorGetCodeFunc)(void);
typedef BOOL(WINAPI *BASS_InitFunc)(int, DWORD, DWORD, HWND, const void *);
typedef BOOL(WINAPI *BASS_FreeFunc)(void);
typedef BOOL(WINAPI *BASS_SetConfigFunc)(DWORD, DWORD);
typedef DWORD(WINAPI *BASS_ChannelGetDataFunc)(DWORD, void *, DWORD);
typedef BOOL(WINAPI *BASS_ChannelGetAttributeFunc)(DWORD, DWORD, float *);
typedef BOOL(WINAPI *BASS_StreamFreeFunc)(HSTREAM);

typedef DWORD(WINAPI *BASS_MIDI_GetVersionFunc)(void);
typedef HSTREAM(WINAPI *BASS_MIDI_StreamCreateFunc)(DWORD, DWORD, DWORD);
typedef BOOL(WINAPI *BASS_MIDI_StreamSetFontsFunc)(HSTREAM, const void *,
                                                   DWORD);
typedef BOOL(WINAPI *BASS_MIDI_StreamLoadSamplesFunc)(HSTREAM);
typedef BOOL(WINAPI *BASS_MIDI_StreamEventFunc)(HSTREAM, DWORD, DWORD, DWORD);
typedef DWORD(WINAPI *BASS_MIDI_StreamEventsFunc)(HSTREAM, DWORD, const void *,
                                                  DWORD);
typedef HSOUNDFONT(WINAPI *BASS_MIDI_FontInitFunc)(const void *, DWORD);
typedef BOOL(WINAPI *BASS_MIDI_FontFreeFunc)(HSOUNDFONT);
typedef BOOL(WINAPI *BASS_MIDI_FontGetInfoFunc)(HSOUNDFONT,
                                                BASS_MIDI_FONTINFO *);

static std::string NormalizePathSeparators(const std::string &path) {
  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '/', '\\');
  return normalized;
}

static std::wstring Utf8ToWide(const std::string &text) {
  if (text.empty())
    return std::wstring();
  int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
  if (size <= 0) {
    size = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, NULL, 0);
    if (size <= 0)
      return std::wstring(text.begin(), text.end());
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, &out[0], size);
    if (!out.empty() && out.back() == L'\0')
      out.pop_back();
    return out;
  }
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], size);
  if (!out.empty() && out.back() == L'\0')
    out.pop_back();
  return out;
}

static std::string GetModuleDirectory() {
  char path[MAX_PATH];
  DWORD length = GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase),
                                    path, MAX_PATH);
  if (!length || length >= MAX_PATH)
    return std::string();
  char *slash = strrchr(path, '\\');
  if (!slash)
    return std::string();
  *slash = '\0';
  return std::string(path);
}

static bool FileExists(const std::string &path) {
  return !path.empty() &&
         GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::string JoinPath(const std::string &left, const std::string &right) {
  if (left.empty())
    return right;
  if (right.empty())
    return left;
  if (left.back() == '\\')
    return left + right;
  return left + "\\" + right;
}

static std::string GetBassErrorString(int errorCode) {
  switch (errorCode) {
  case 0:
    return "ok";
  case 2:
    return "BASS error: file open";
  case 3:
    return "BASS error: driver";
  case 4:
    return "BASS error: buffer lost";
  case 5:
    return "BASS error: handle";
  case 6:
    return "BASS error: format";
  case 14:
    return "BASS error: already initialized";
  case 32:
    return "BASS error: unsupported";
  case 41:
    return "BASS error: file form";
  case 7000:
    return "BASSMIDI error: SFZ include missing";
  default: {
    char buffer[64];
    sprintf(buffer, "BASS error code %d", errorCode);
    return std::string(buffer);
  }
  }
}

struct BassApi {
  HMODULE bassModule = NULL;
  HMODULE bassMidiModule = NULL;
  BASS_GetVersionFunc getVersion = NULL;
  BASS_ErrorGetCodeFunc errorGetCode = NULL;
  BASS_InitFunc init = NULL;
  BASS_FreeFunc freeBass = NULL;
  BASS_SetConfigFunc setConfig = NULL;
  BASS_ChannelGetDataFunc channelGetData = NULL;
  BASS_ChannelGetAttributeFunc channelGetAttribute = NULL;
  BASS_StreamFreeFunc streamFree = NULL;
  BASS_MIDI_GetVersionFunc midiGetVersion = NULL;
  BASS_MIDI_StreamCreateFunc midiStreamCreate = NULL;
  BASS_MIDI_StreamSetFontsFunc midiStreamSetFonts = NULL;
  BASS_MIDI_StreamLoadSamplesFunc midiStreamLoadSamples = NULL;
  BASS_MIDI_StreamEventFunc midiStreamEvent = NULL;
  BASS_MIDI_StreamEventsFunc midiStreamEvents = NULL;
  BASS_MIDI_FontInitFunc midiFontInit = NULL;
  BASS_MIDI_FontFreeFunc midiFontFree = NULL;
  BASS_MIDI_FontGetInfoFunc midiFontGetInfo = NULL;

  void Unload() {
    if (bassMidiModule) {
      FreeLibrary(bassMidiModule);
      bassMidiModule = NULL;
    }
    if (bassModule) {
      FreeLibrary(bassModule);
      bassModule = NULL;
    }
    getVersion = NULL;
    errorGetCode = NULL;
    init = NULL;
    freeBass = NULL;
    setConfig = NULL;
    channelGetData = NULL;
    channelGetAttribute = NULL;
    streamFree = NULL;
    midiGetVersion = NULL;
    midiStreamCreate = NULL;
    midiStreamSetFonts = NULL;
    midiStreamLoadSamples = NULL;
    midiStreamEvent = NULL;
    midiStreamEvents = NULL;
    midiFontInit = NULL;
    midiFontFree = NULL;
    midiFontGetInfo = NULL;
  }
};

static FARPROC LoadRequiredProc(HMODULE module, const char *name) {
  return module ? GetProcAddress(module, name) : NULL;
}

} // namespace

struct BassMidiEngine::Impl {
  BassApi api;
  HSTREAM stream = 0;
  HSOUNDFONT font = 0;
  std::string resolvedSourcePath;
  std::string resolvedSourceFormat;
  SamplerDiagnostics diagnostics;
  int sampleRate = 44100;
  int configuredMaxVoices = 500;
  int currentVoiceCap = 500;
  unsigned int lastPressureOverload = 0;
  unsigned int lastPressureLagState = 0;
  unsigned int lastPressureCadence = 0;
  unsigned int lastPressureScheduledPending = 0;

  bool LoadApi() {
    std::vector<std::string> searchDirs;
    const std::string moduleDir = GetModuleDirectory();
    if (!moduleDir.empty()) {
      searchDirs.push_back(moduleDir);
      searchDirs.push_back(JoinPath(moduleDir, "BASSMIDI DLL"));
      searchDirs.push_back(JoinPath(moduleDir, "BassMIDI"));
    }
    searchDirs.push_back(".");
    searchDirs.push_back(".\\BASSMIDI DLL");
    searchDirs.push_back(".\\BassMIDI");

    for (size_t i = 0; i < searchDirs.size() && !api.bassModule; ++i) {
      const std::string bassPath = JoinPath(searchDirs[i], "bass.dll");
      const std::string bassMidiPath = JoinPath(searchDirs[i], "bassmidi.dll");
      if (!FileExists(bassPath) || !FileExists(bassMidiPath))
        continue;
      api.bassModule = LoadLibraryA(bassPath.c_str());
      if (!api.bassModule)
        continue;
      api.bassMidiModule = LoadLibraryA(bassMidiPath.c_str());
      if (!api.bassMidiModule) {
        FreeLibrary(api.bassModule);
        api.bassModule = NULL;
        continue;
      }
    }

    if (!api.bassModule || !api.bassMidiModule) {
      diagnostics.lastWarning =
          "BASSMIDI DLLs were not found. Put bass.dll and bassmidi.dll in "
          "BASSMIDI DLL beside the SVMS DLL.";
      return false;
    }

    api.getVersion =
        reinterpret_cast<BASS_GetVersionFunc>(LoadRequiredProc(api.bassModule,
                                                               "BASS_GetVersion"));
    api.errorGetCode = reinterpret_cast<BASS_ErrorGetCodeFunc>(
        LoadRequiredProc(api.bassModule, "BASS_ErrorGetCode"));
    api.init =
        reinterpret_cast<BASS_InitFunc>(LoadRequiredProc(api.bassModule,
                                                         "BASS_Init"));
    api.freeBass =
        reinterpret_cast<BASS_FreeFunc>(LoadRequiredProc(api.bassModule,
                                                         "BASS_Free"));
    api.setConfig = reinterpret_cast<BASS_SetConfigFunc>(
        LoadRequiredProc(api.bassModule, "BASS_SetConfig"));
    api.channelGetData = reinterpret_cast<BASS_ChannelGetDataFunc>(
        LoadRequiredProc(api.bassModule, "BASS_ChannelGetData"));
    api.channelGetAttribute = reinterpret_cast<BASS_ChannelGetAttributeFunc>(
        LoadRequiredProc(api.bassModule, "BASS_ChannelGetAttribute"));
    api.streamFree = reinterpret_cast<BASS_StreamFreeFunc>(
        LoadRequiredProc(api.bassModule, "BASS_StreamFree"));
    api.midiGetVersion = reinterpret_cast<BASS_MIDI_GetVersionFunc>(
        LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_GetVersion"));
    api.midiStreamCreate = reinterpret_cast<BASS_MIDI_StreamCreateFunc>(
        LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_StreamCreate"));
    api.midiStreamSetFonts = reinterpret_cast<BASS_MIDI_StreamSetFontsFunc>(
        LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_StreamSetFonts"));
    api.midiStreamLoadSamples =
        reinterpret_cast<BASS_MIDI_StreamLoadSamplesFunc>(
            LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_StreamLoadSamples"));
    api.midiStreamEvent = reinterpret_cast<BASS_MIDI_StreamEventFunc>(
        LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_StreamEvent"));
    api.midiStreamEvents = reinterpret_cast<BASS_MIDI_StreamEventsFunc>(
        LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_StreamEvents"));
    api.midiFontInit = reinterpret_cast<BASS_MIDI_FontInitFunc>(
        LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_FontInit"));
    api.midiFontFree = reinterpret_cast<BASS_MIDI_FontFreeFunc>(
        LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_FontFree"));
    api.midiFontGetInfo = reinterpret_cast<BASS_MIDI_FontGetInfoFunc>(
        LoadRequiredProc(api.bassMidiModule, "BASS_MIDI_FontGetInfo"));

    if (!api.getVersion || !api.errorGetCode || !api.init || !api.freeBass ||
        !api.setConfig || !api.channelGetData || !api.channelGetAttribute ||
        !api.streamFree || !api.midiGetVersion || !api.midiStreamCreate ||
        !api.midiStreamSetFonts || !api.midiStreamLoadSamples ||
        !api.midiStreamEvent || !api.midiStreamEvents || !api.midiFontInit ||
        !api.midiFontFree || !api.midiFontGetInfo) {
      diagnostics.lastWarning = "BASSMIDI DLL exports are incomplete.";
      api.Unload();
      return false;
    }

    DWORD bassVersion = api.getVersion();
    DWORD bassMidiVersion = api.midiGetVersion();
    if (HIWORD(bassVersion) != BASSVERSION ||
        HIWORD(bassMidiVersion) != BASSVERSION) {
      char buffer[160];
      sprintf(buffer,
              "BASS/BASSMIDI version mismatch (bass=%08lX bassmidi=%08lX, "
              "expected API %04X.x).",
              static_cast<unsigned long>(bassVersion),
              static_cast<unsigned long>(bassMidiVersion),
              static_cast<unsigned int>(BASSVERSION));
      diagnostics.lastWarning = buffer;
      api.Unload();
      return false;
    }

    return true;
  }

  void ApplyDefaultPitchBendRange() {
    if (!stream || !api.midiStreamEvent)
      return;
    for (DWORD channel = 0; channel < 16; ++channel) {
      api.midiStreamEvent(stream, channel, MIDI_EVENT_PITCHRANGE,
                          kDefaultPitchBendRangeSemitones);
      diagnostics.pitchBendRange[channel] =
          (float)kDefaultPitchBendRangeSemitones;
    }
  }

  void CloseHandles() {
    if (stream && api.streamFree) {
      api.streamFree(stream);
      stream = 0;
    }
    if (font && api.midiFontFree) {
      api.midiFontFree(font);
      font = 0;
    }
    if (api.freeBass)
      api.freeBass();
    api.Unload();
  }

  void SendRaw(const BYTE *data, DWORD length) {
    if (!stream || !api.midiStreamEvents || !data || !length)
      return;
    api.midiStreamEvents(stream, 0x10000u, data, length);
  }

  void SendControlChange(int channel, int control, int value) {
    BYTE raw[3];
    raw[0] = static_cast<BYTE>(0xB0 | (channel & 0x0F));
    raw[1] = static_cast<BYTE>(control & 0x7F);
    raw[2] = static_cast<BYTE>(value & 0x7F);
    SendRaw(raw, 3);
  }

  void ResetStreamState() {
    if (!stream)
      return;
    for (int channel = 0; channel < 16; ++channel) {
      SendControlChange(channel, 64, 0);
      SendControlChange(channel, 121, 0);
      SendControlChange(channel, 120, 0);
      SendControlChange(channel, 123, 0);
      if (api.midiStreamEvent)
        api.midiStreamEvent(stream, static_cast<DWORD>(channel), MIDI_EVENT_RESET,
                            0);
    }
    ApplyDefaultPitchBendRange();
  }

  void ApplyVoiceCap(int targetVoices) {
    if (!api.setConfig)
      return;

    if (targetVoices < 128)
      targetVoices = 128;
    if (configuredMaxVoices > 0 && targetVoices > configuredMaxVoices)
      targetVoices = configuredMaxVoices;
    if (targetVoices == currentVoiceCap)
      return;

    api.setConfig(BASS_CONFIG_MIDI_VOICES, static_cast<DWORD>(targetVoices));
    currentVoiceCap = targetVoices;

    char buffer[192];
    sprintf(buffer,
            "SVMS: BASSMIDI voice cap=%d configured=%d overload=%u lag=%u cadence=%u pending=%u\n",
            currentVoiceCap, configuredMaxVoices, lastPressureOverload,
            lastPressureLagState, lastPressureCadence,
            lastPressureScheduledPending);
    OutputDebugStringA(buffer);
  }

  void UpdateVoiceCapFromPressure() {
    int configured = configuredMaxVoices > 0 ? configuredMaxVoices : 500;
    int target = configured;

    if (lastPressureOverload >= 2 || lastPressureLagState >= 2 ||
        lastPressureCadence >= 16 || lastPressureScheduledPending >= 4096) {
      target = configured / 4;
      if (target < 384)
        target = 384;
      if (target > 768)
        target = 768;
    } else if (lastPressureOverload >= 1 || lastPressureLagState >= 1 ||
               lastPressureCadence >= 6 || lastPressureScheduledPending >= 1024) {
      target = configured / 2;
      if (target < 512)
        target = 512;
      if (target > 1280)
        target = 1280;
    } else if (lastPressureCadence >= 3 || lastPressureScheduledPending >= 256) {
      target = (configured * 3) / 4;
      if (target < 768)
        target = 768;
    }

    if (target < currentVoiceCap) {
      ApplyVoiceCap(target);
      return;
    }

    if (target > currentVoiceCap) {
      int step = currentVoiceCap + 128;
      if (step > target)
        step = target;
      ApplyVoiceCap(step);
    }
  }
};

BassMidiEngine::BassMidiEngine() : impl(new Impl()) {}

BassMidiEngine::~BassMidiEngine() { Shutdown(true); }

bool BassMidiEngine::Initialize(const SamplerInitParams &params) {
  impl->diagnostics = SamplerDiagnostics();
  impl->resolvedSourcePath = NormalizePathSeparators(params.sourcePath);
  impl->resolvedSourceFormat = DetectSourceFormat(params.sourcePath);
  impl->sampleRate = params.sampleRate > 0 ? params.sampleRate : 44100;
  impl->configuredMaxVoices = params.maxVoices > 0 ? params.maxVoices : 500;
  impl->currentVoiceCap = impl->configuredMaxVoices;

  if (!impl->LoadApi())
    return false;

  if (!impl->api.init(-1, static_cast<DWORD>(impl->sampleRate), 0, NULL, NULL)) {
    impl->diagnostics.lastWarning =
        "BASS init failed: " +
        GetBassErrorString(impl->api.errorGetCode ? impl->api.errorGetCode() : -1);
    impl->CloseHandles();
    return false;
  }

  impl->api.setConfig(BASS_CONFIG_MIDI_VOICES,
                      static_cast<DWORD>(impl->configuredMaxVoices));

  std::wstring widePath = Utf8ToWide(impl->resolvedSourcePath);
  impl->font = impl->api.midiFontInit(widePath.c_str(), BASS_UNICODE);
  if (!impl->font) {
    impl->diagnostics.lastWarning =
        "BASSMIDI font load failed: " +
        GetBassErrorString(impl->api.errorGetCode ? impl->api.errorGetCode() : -1);
    impl->CloseHandles();
    return false;
  }

  impl->stream = impl->api.midiStreamCreate(
      16, BASS_SAMPLE_FLOAT | BASS_STREAM_DECODE,
      static_cast<DWORD>(impl->sampleRate));
  if (!impl->stream) {
    impl->diagnostics.lastWarning =
        "BASSMIDI stream create failed: " +
        GetBassErrorString(impl->api.errorGetCode ? impl->api.errorGetCode() : -1);
    impl->CloseHandles();
    return false;
  }

  BASS_MIDI_FONT fontMapping;
  fontMapping.font = impl->font;
  fontMapping.preset = -1;
  fontMapping.bank = 0;
  if (!impl->api.midiStreamSetFonts(impl->stream, &fontMapping, 1)) {
    impl->diagnostics.lastWarning =
        "BASSMIDI font attach failed: " +
        GetBassErrorString(impl->api.errorGetCode ? impl->api.errorGetCode() : -1);
    impl->CloseHandles();
    return false;
  }

  impl->api.midiStreamLoadSamples(impl->stream);
  impl->ApplyDefaultPitchBendRange();

  BASS_MIDI_FONTINFO info;
  memset(&info, 0, sizeof(info));
  if (impl->api.midiFontGetInfo(impl->font, &info)) {
    impl->diagnostics.loadedSampleCount = info.samload ? 1u : 0u;
  }
  impl->diagnostics.lastWarning.clear();
  return true;
}

void BassMidiEngine::Shutdown(bool) {
  if (!impl)
    return;
  impl->CloseHandles();
}

void BassMidiEngine::Reset() {
  if (!impl)
    return;
  impl->currentVoiceCap = impl->configuredMaxVoices > 0
                              ? impl->configuredMaxVoices
                              : impl->currentVoiceCap;
  impl->ApplyVoiceCap(impl->currentVoiceCap);
  impl->ResetStreamState();
}

void BassMidiEngine::ReloadRuntimeSettings(const RuntimeSettings &) {}

void BassMidiEngine::SetRealtimePressure(unsigned int overloadState,
                                         unsigned int schedulerLagState,
                                         unsigned int cadenceStreak,
                                         unsigned int scheduledPendingEvents) {
  if (!impl)
    return;
  impl->lastPressureOverload = overloadState;
  impl->lastPressureLagState = schedulerLagState;
  impl->lastPressureCadence = cadenceStreak;
  impl->lastPressureScheduledPending = scheduledPendingEvents;
  impl->UpdateVoiceCapFromPressure();
}

void BassMidiEngine::BeginRenderBlock() {}

void BassMidiEngine::ProcessMidiEvent(const MidiEvent &event) {
  if (!impl || !impl->stream)
    return;

  BYTE raw[3];
  switch (event.type) {
  case MidiEvent::NOTE_ON:
    raw[0] = static_cast<BYTE>((event.data2 > 0 ? 0x90 : 0x80) |
                               (event.channel & 0x0F));
    raw[1] = static_cast<BYTE>(event.data1 & 0x7F);
    raw[2] = static_cast<BYTE>(event.data2 > 0 ? (event.data2 & 0x7F) : 0);
    impl->SendRaw(raw, 3);
    break;
  case MidiEvent::NOTE_OFF:
    raw[0] = static_cast<BYTE>(0x80 | (event.channel & 0x0F));
    raw[1] = static_cast<BYTE>(event.data1 & 0x7F);
    raw[2] = 0;
    impl->SendRaw(raw, 3);
    break;
  case MidiEvent::PROGRAM_CHANGE:
    raw[0] = static_cast<BYTE>(0xC0 | (event.channel & 0x0F));
    raw[1] = static_cast<BYTE>(event.data1 & 0x7F);
    impl->SendRaw(raw, 2);
    break;
  case MidiEvent::CONTROL_CHANGE:
    impl->SendControlChange(event.channel, event.data1, event.data2);
    break;
  case MidiEvent::PITCH_BEND: {
    int bend = event.data1;
    if (bend < 0)
      bend = 0;
    if (bend > 0x3FFF)
      bend = 0x3FFF;
    raw[0] = static_cast<BYTE>(0xE0 | (event.channel & 0x0F));
    raw[1] = static_cast<BYTE>(bend & 0x7F);
    raw[2] = static_cast<BYTE>((bend >> 7) & 0x7F);
    impl->SendRaw(raw, 3);
    break;
  }
  case MidiEvent::RESET:
    impl->ResetStreamState();
    break;
  }
}

void BassMidiEngine::Render(float *output, int numFrames) {
  if (!output || numFrames <= 0)
    return;
  memset(output, 0, static_cast<size_t>(numFrames) * 2u * sizeof(float));
  if (!impl || !impl->stream || !impl->api.channelGetData)
    return;

  DWORD requestedBytes = static_cast<DWORD>(numFrames * 2 * sizeof(float));
  DWORD readBytes = impl->api.channelGetData(impl->stream, output, requestedBytes);
  if (readBytes == static_cast<DWORD>(-1)) {
    impl->diagnostics.lastWarning =
        "BASS render failed: " +
        GetBassErrorString(impl->api.errorGetCode ? impl->api.errorGetCode() : -1);
    return;
  }
  if (readBytes < requestedBytes) {
    BYTE *tail = reinterpret_cast<BYTE *>(output) + readBytes;
    memset(tail, 0, requestedBytes - readBytes);
  }
}

std::string BassMidiEngine::GetResolvedSourcePath() const {
  return impl ? impl->resolvedSourcePath : std::string();
}

std::string BassMidiEngine::GetResolvedSourceFormat() const {
  return impl ? impl->resolvedSourceFormat : std::string();
}

std::string BassMidiEngine::GetEngineName() const { return "bassmidi"; }

DWORD BassMidiEngine::GetActiveVoiceStats(DWORD *channelCounts, int count) const {
  if (channelCounts && count > 0)
    memset(channelCounts, 0, static_cast<size_t>(count) * sizeof(DWORD));
  if (!impl || !impl->stream || !impl->api.channelGetAttribute)
    return 0;
  float activeVoices = 0.0f;
  if (!impl->api.channelGetAttribute(impl->stream, BASS_ATTRIB_MIDI_VOICES_ACTIVE,
                                     &activeVoices))
    return 0;
  return static_cast<DWORD>(activeVoices > 0.0f ? activeVoices : 0.0f);
}

SamplerDiagnostics BassMidiEngine::GetDiagnostics() const {
  SamplerDiagnostics diagnostics = impl ? impl->diagnostics : SamplerDiagnostics();
  if (impl && impl->stream && impl->api.channelGetAttribute) {
    float activeVoices = 0.0f;
    if (impl->api.channelGetAttribute(impl->stream,
                                      BASS_ATTRIB_MIDI_VOICES_ACTIVE,
                                      &activeVoices)) {
      diagnostics.voiceStartMs = 0.0f;
    }
  }
  return diagnostics;
}
