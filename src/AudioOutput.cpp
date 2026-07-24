#include "AudioOutput.h"
#include "Config.h"
#include "CpuFeatures.h"
#include "LiveRuntime.h"
#include "Synth.h"
#include "SystemWinmm.h"
#include <algorithm>
#include <cmath>
#define DIRECTSOUND_VERSION 0x0800
#include <dsound.h>
#ifndef SVMS_LEGACY_XP
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#endif
#include <cstring>     // memset
#include <emmintrin.h> // SSE2
#include <float.h>
#include <immintrin.h> // AVX
#include <stdlib.h>
#include <vector>
#include <windows.h>

#if defined(_MSC_VER) || defined(__AVX__)
#define SVMS_ENABLE_AVX_INTRINSICS 1
#else
#define SVMS_ENABLE_AVX_INTRINSICS 0
#endif

#define BLOCK_SIZE 512
#define BLOCK_COUNT 16

static float ClampFloat(float value, float minValue, float maxValue) {
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

static float SoftLimit(float value) {
  return value / (1.0f + std::fabs(value));
}

static const char *WaveOutErrorToString(MMRESULT result) {
  switch (result) {
  case MMSYSERR_NOERROR:
    return "MMSYSERR_NOERROR";
  case MMSYSERR_ERROR:
    return "MMSYSERR_ERROR";
  case MMSYSERR_BADDEVICEID:
    return "MMSYSERR_BADDEVICEID";
  case MMSYSERR_NOTENABLED:
    return "MMSYSERR_NOTENABLED";
  case MMSYSERR_ALLOCATED:
    return "MMSYSERR_ALLOCATED";
  case MMSYSERR_INVALHANDLE:
    return "MMSYSERR_INVALHANDLE";
  case MMSYSERR_NODRIVER:
    return "MMSYSERR_NODRIVER";
  case MMSYSERR_NOMEM:
    return "MMSYSERR_NOMEM";
  case MMSYSERR_NOTSUPPORTED:
    return "MMSYSERR_NOTSUPPORTED";
  case WAVERR_BADFORMAT:
    return "WAVERR_BADFORMAT";
  case WAVERR_SYNC:
    return "WAVERR_SYNC";
  case WAVERR_UNPREPARED:
    return "WAVERR_UNPREPARED";
  default:
    return "UNKNOWN_MMRESULT";
  }
}

static AudioOutput::AudioBackendId ParseAudioBackend(const std::string &value) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 ::tolower);
  if (normalized == "waveout")
    return AudioOutput::AudioBackendId::WAVEOUT;
  if (normalized == "dsound" || normalized == "directsound")
    return AudioOutput::AudioBackendId::DSOUND;
  if (normalized == "wasapi-shared" || normalized == "wasapi")
    return AudioOutput::AudioBackendId::WASAPI_SHARED;
  return AudioOutput::AudioBackendId::AUTO;
}

static const char *AudioBackendToConfigString(AudioOutput::AudioBackendId backend) {
  switch (backend) {
  case AudioOutput::AudioBackendId::WAVEOUT:
    return "waveout";
  case AudioOutput::AudioBackendId::DSOUND:
    return "dsound";
  case AudioOutput::AudioBackendId::WASAPI_SHARED:
    return "wasapi-shared";
  default:
    return "auto";
  }
}

static const char *AudioBackendToDisplayString(
    AudioOutput::AudioBackendId backend) {
  switch (backend) {
  case AudioOutput::AudioBackendId::WAVEOUT:
    return "waveOut";
  case AudioOutput::AudioBackendId::DSOUND:
    return "DirectSound";
  case AudioOutput::AudioBackendId::WASAPI_SHARED:
    return "WASAPI Shared";
  default:
    return "auto";
  }
}

static bool IsAudioBackendSupportedInBuild(AudioOutput::AudioBackendId backend) {
#ifdef SVMS_LEGACY_XP
  return backend != AudioOutput::AudioBackendId::WASAPI_SHARED;
#else
  (void)backend;
  return true;
#endif
}

static bool IsQuantizedTimingModeEnabled() {
  std::string mode = Config::Instance().GetString("event_timing_mode", std::string());
  if (mode.empty()) {
    return !Config::Instance().GetBool("async_note_starts", true);
  }
  std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
  return mode == "quantized";
}

static bool IsAccurateTimingModeEnabled() {
  std::string mode = Config::Instance().GetString("event_timing_mode", std::string());
  if (mode.empty())
    return Config::Instance().GetBool("async_note_starts", true);
  std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
  return mode == "accurate";
}

static const char *DirectSoundErrorToString(HRESULT hr) {
  switch (hr) {
  case DS_OK:
    return "DS_OK";
  case DSERR_ALLOCATED:
    return "DSERR_ALLOCATED";
  case DSERR_BADFORMAT:
    return "DSERR_BADFORMAT";
  case DSERR_BUFFERLOST:
    return "DSERR_BUFFERLOST";
  case DSERR_CONTROLUNAVAIL:
    return "DSERR_CONTROLUNAVAIL";
  case DSERR_GENERIC:
    return "DSERR_GENERIC";
  case DSERR_INVALIDCALL:
    return "DSERR_INVALIDCALL";
  case DSERR_INVALIDPARAM:
    return "DSERR_INVALIDPARAM";
  case DSERR_NOAGGREGATION:
    return "DSERR_NOAGGREGATION";
  case DSERR_NODRIVER:
    return "DSERR_NODRIVER";
  case DSERR_OUTOFMEMORY:
    return "DSERR_OUTOFMEMORY";
  case DSERR_PRIOLEVELNEEDED:
    return "DSERR_PRIOLEVELNEEDED";
  case DSERR_UNSUPPORTED:
    return "DSERR_UNSUPPORTED";
  default:
    return "UNKNOWN_DSERR";
  }
}

#ifndef SVMS_LEGACY_XP
static const char *WasapiErrorToString(HRESULT hr) {
  switch (hr) {
  case AUDCLNT_E_UNSUPPORTED_FORMAT:
    return "AUDCLNT_E_UNSUPPORTED_FORMAT";
  case AUDCLNT_E_DEVICE_INVALIDATED:
    return "AUDCLNT_E_DEVICE_INVALIDATED";
  case AUDCLNT_E_NOT_INITIALIZED:
    return "AUDCLNT_E_NOT_INITIALIZED";
  case AUDCLNT_E_ALREADY_INITIALIZED:
    return "AUDCLNT_E_ALREADY_INITIALIZED";
  case AUDCLNT_E_SERVICE_NOT_RUNNING:
    return "AUDCLNT_E_SERVICE_NOT_RUNNING";
  case AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED:
    return "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED";
  case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:
    return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
  case AUDCLNT_E_BUFFER_OPERATION_PENDING:
    return "AUDCLNT_E_BUFFER_OPERATION_PENDING";
  case AUDCLNT_E_OUT_OF_ORDER:
    return "AUDCLNT_E_OUT_OF_ORDER";
  case E_POINTER:
    return "E_POINTER";
  case E_INVALIDARG:
    return "E_INVALIDARG";
  case CO_E_NOTINITIALIZED:
    return "CO_E_NOTINITIALIZED";
  default:
    return "UNKNOWN_WASAPI_ERROR";
  }
}
#endif

static void LogWaveOutDevices(SystemWinmm &sys) {
  if (!sys.Real_waveOutGetNumDevs) {
    OutputDebugStringA("SVMS: waveOutGetNumDevs unavailable\n");
    return;
  }

  UINT numDevs = sys.Real_waveOutGetNumDevs();
  char buf[256];
  sprintf(buf, "SVMS: waveOut devices reported: %u\n", numDevs);
  OutputDebugStringA(buf);

  if (!sys.Real_waveOutGetDevCapsA) {
    OutputDebugStringA("SVMS: waveOutGetDevCapsA unavailable\n");
    return;
  }

  for (UINT i = 0; i < numDevs; ++i) {
    WAVEOUTCAPSA caps;
    memset(&caps, 0, sizeof(caps));
    MMRESULT capsRes =
        sys.Real_waveOutGetDevCapsA(i, &caps, sizeof(WAVEOUTCAPSA));
    if (capsRes == MMSYSERR_NOERROR) {
      sprintf(buf,
              "SVMS: waveOut device %u: name=\"%s\" formats=0x%08lX channels=%u "
              "support=0x%08lX\n",
              i, caps.szPname, (unsigned long)caps.dwFormats, caps.wChannels,
              (unsigned long)caps.dwSupport);
    } else {
      sprintf(buf,
              "SVMS: waveOut device %u: GetDevCaps failed: %u (%s)\n", i,
              (unsigned int)capsRes, WaveOutErrorToString(capsRes));
    }
    OutputDebugStringA(buf);
  }
}

// ============================================================================
// SIMD Soft Clipper + Conversion Functions
// ============================================================================

// SSE2 version: processes 4 samples at a time
static void ProcessSamplesSSE2(const float *input, int16_t *output,
                               int totalSamples, float masterVolume) {
  __m128 vVolume = _mm_set1_ps(masterVolume);
  __m128 v27 = _mm_set1_ps(27.0f);
  __m128 v9 = _mm_set1_ps(9.0f);
  __m128 vOne = _mm_set1_ps(1.0f);
  __m128 vNegOne = _mm_set1_ps(-1.0f);
  __m128 vScale = _mm_set1_ps(32767.0f);

  int simdCount = totalSamples & ~3; // Round down to multiple of 4

  for (int i = 0; i < simdCount; i += 4) {
    __m128 s = _mm_loadu_ps(&input[i]);
    s = _mm_mul_ps(s, vVolume);

    // Soft Clipper: s * (27 + s^2) / (27 + 9*s^2)
    __m128 s2 = _mm_mul_ps(s, s);
    __m128 num = _mm_mul_ps(s, _mm_add_ps(v27, s2));
    __m128 den = _mm_add_ps(v27, _mm_mul_ps(v9, s2));
    s = _mm_div_ps(num, den);

    // Clamp to [-1, 1]
    s = _mm_min_ps(s, vOne);
    s = _mm_max_ps(s, vNegOne);

    // Convert to int16
    s = _mm_mul_ps(s, vScale);
    __m128i si = _mm_cvtps_epi32(s);
    si = _mm_packs_epi32(si, si);
    _mm_storel_epi64((__m128i *)&output[i], si);
  }

  // Scalar tail
  for (int i = simdCount; i < totalSamples; ++i) {
    float s = input[i] * masterVolume;
    float s2 = s * s;
    s = s * (27.0f + s2) / (27.0f + 9.0f * s2);
    if (s > 1.0f)
      s = 1.0f;
    else if (s < -1.0f)
      s = -1.0f;
    output[i] = (int16_t)(s * 32767.0f);
  }
}

// AVX version: processes 8 samples at a time
#if SVMS_ENABLE_AVX_INTRINSICS
static void ProcessSamplesAVX(const float *input, int16_t *output,
                              int totalSamples, float masterVolume) {
  __m256 vVolume = _mm256_set1_ps(masterVolume);
  __m256 v27 = _mm256_set1_ps(27.0f);
  __m256 v9 = _mm256_set1_ps(9.0f);
  __m256 vOne = _mm256_set1_ps(1.0f);
  __m256 vNegOne = _mm256_set1_ps(-1.0f);
  __m256 vScale = _mm256_set1_ps(32767.0f);

  int avxCount = totalSamples & ~7; // Round down to multiple of 8

  for (int i = 0; i < avxCount; i += 8) {
    __m256 s = _mm256_loadu_ps(&input[i]);
    s = _mm256_mul_ps(s, vVolume);

    // Soft Clipper: s * (27 + s^2) / (27 + 9*s^2)
    __m256 s2 = _mm256_mul_ps(s, s);
    __m256 num = _mm256_mul_ps(s, _mm256_add_ps(v27, s2));
    __m256 den = _mm256_add_ps(v27, _mm256_mul_ps(v9, s2));
    s = _mm256_div_ps(num, den);

    // Clamp to [-1, 1]
    s = _mm256_min_ps(s, vOne);
    s = _mm256_max_ps(s, vNegOne);

    // Convert to int16: float -> int32 -> int16
    s = _mm256_mul_ps(s, vScale);
    __m256i si = _mm256_cvtps_epi32(s);

    // Pack 8 int32s to 8 int16s (with saturation)
    // AVX2: _mm256_packs_epi32 packs within lanes, need to permute
    __m128i lo = _mm256_castsi256_si128(si);      // Lower 4 int32s
    __m128i hi = _mm256_extracti128_si256(si, 1); // Upper 4 int32s
    __m128i packed = _mm_packs_epi32(lo, hi);     // 8 int16s
    _mm_storeu_si128((__m128i *)&output[i], packed);
  }

  // Handle remaining with SSE2 or scalar
  int remaining = totalSamples - avxCount;
  if (remaining > 0) {
    ProcessSamplesSSE2(input + avxCount, output + avxCount, remaining,
                       masterVolume);
  }

  // Clear upper YMM state to avoid SSE transition penalties
  _mm256_zeroupper();
}
#endif

AudioOutput &AudioOutput::Instance() {
  static AudioOutput instance;
  return instance;
}

#ifdef SVMS_LEGACY_XP
AudioOutput::AudioOutput()
    : audioThread(NULL), running(false), refCount(0), mixBuffer(BLOCK_SIZE * 2),
      dsBlockBuffer(BLOCK_SIZE * 2), reverbWriteIndex(0), reverbSampleRate(0),
      reverbFilterL(0.0f), reverbFilterR(0.0f), reverbBlurL1(0.0f),
      reverbBlurR1(0.0f), reverbBlurL2(0.0f), reverbBlurR2(0.0f),
      reverbStateActive(false),
      limiterGain(1.0f), resolvedBackendName("idle"),
      synthRuntimeSettingsSeeded(false), cachedVelocityCurve(2.4f),
      cachedVelocityFloor(0.0f), cachedVelocityIgnoreBelow(0),
      cachedAsyncNoteStarts(true), renderSampleCursor(0),
      runtimeConfigRefreshRequested(false) {}
#else
AudioOutput::AudioOutput()
    : running(false), refCount(0), mixBuffer(BLOCK_SIZE * 2),
      dsBlockBuffer(BLOCK_SIZE * 2), reverbWriteIndex(0), reverbSampleRate(0),
      reverbFilterL(0.0f), reverbFilterR(0.0f), reverbBlurL1(0.0f),
      reverbBlurR1(0.0f), reverbBlurL2(0.0f), reverbBlurR2(0.0f),
      reverbStateActive(false),
      limiterGain(1.0f), resolvedBackendName("idle"),
      synthRuntimeSettingsSeeded(false), cachedVelocityCurve(2.4f),
      cachedVelocityFloor(0.0f), cachedVelocityIgnoreBelow(0),
      cachedAsyncNoteStarts(true), renderSampleCursor(0),
      runtimeConfigRefreshRequested(false) {}
#endif

AudioOutput::~AudioOutput() { Stop(); }

int AudioOutput::GetRefCount() {
  compat::LockGuard<compat::Mutex> lock(mutex);
  return refCount;
}

std::string AudioOutput::GetResolvedBackendName() {
  compat::LockGuard<compat::Mutex> lock(backendStateMutex);
  return resolvedBackendName;
}

bool AudioOutput::IsWasapiAsyncFeedActive() {
  compat::LockGuard<compat::Mutex> lock(backendStateMutex);
  return resolvedBackendName == "WASAPI Shared" && audioSettings.wasapiAsyncFeed &&
         IsAccurateTimingModeEnabled();
}

void AudioOutput::RequestRuntimeConfigRefresh() {
  runtimeConfigRefreshRequested.store(true, std::memory_order_relaxed);
}

void AudioOutput::SetResolvedBackendName(const char *backendName) {
  compat::LockGuard<compat::Mutex> lock(backendStateMutex);
  resolvedBackendName = backendName ? backendName : "";
}

bool AudioOutput::RefreshSynthRuntimeSettingsFromConfig() {
  float velocityCurve = Config::Instance().GetFloat("velocity_curve", 2.4f);
  float velocityFloor = Config::Instance().GetFloat("velocity_floor", 0.0f);
  int velocityIgnoreBelow =
      Config::Instance().GetInt("velocity_ignore_below", 0);
  bool asyncNoteStarts =
      Config::Instance().GetBool("async_note_starts", true);
  std::string timingMode =
      Config::Instance().GetString("event_timing_mode", std::string());
  if (timingMode.empty())
    timingMode = asyncNoteStarts ? "accurate" : "legacy-sync";

  const bool changed =
      !synthRuntimeSettingsSeeded || cachedVelocityCurve != velocityCurve ||
      cachedVelocityFloor != velocityFloor ||
      cachedVelocityIgnoreBelow != velocityIgnoreBelow ||
      cachedAsyncNoteStarts != asyncNoteStarts ||
      cachedTimingMode != timingMode;

  cachedVelocityCurve = velocityCurve;
  cachedVelocityFloor = velocityFloor;
  cachedVelocityIgnoreBelow = velocityIgnoreBelow;
  cachedAsyncNoteStarts = asyncNoteStarts;
  cachedTimingMode = timingMode;
  synthRuntimeSettingsSeeded = true;

  if (changed) {
    OutputDebugStringA(
        "SVMS: Audio thread observed synth runtime config change, reloading synth timing/settings\n");
    Synth::Instance().ReloadRuntimeSettings();
  }
  return changed;
}

void AudioOutput::LoadAudioSettings() {
  audioSettings.pollingRate = Config::Instance().GetInt("polling_rate", 0);
  audioSettings.masterVolume = Config::Instance().GetFloat("master_volume", 1.0f);
  audioSettings.reverbEnabled =
      Config::Instance().GetBool("reverb_enable", false);
  audioSettings.reverbMix = Config::Instance().GetFloat("reverb_mix", 0.18f);
  audioSettings.reverbFeedback =
      Config::Instance().GetFloat("reverb_feedback", 0.72f);
  audioSettings.reverbTone = Config::Instance().GetFloat("reverb_tone", 0.28f);
  audioSettings.reverbWidth =
      Config::Instance().GetFloat("reverb_width", 0.35f);
  audioSettings.reverbBlur = Config::Instance().GetFloat("reverb_blur", 0.45f);
  audioSettings.limiterEnabled =
      Config::Instance().GetBool("limiter_enable", true);
  audioSettings.limiterThreshold =
      Config::Instance().GetFloat("limiter_threshold", 0.98f);
  audioSettings.limiterReleaseMs =
      Config::Instance().GetFloat("limiter_release_ms", 80.0f);
  audioSettings.wasapiAsyncFeed =
      Config::Instance().GetBool("wasapi_async_feed", true);
  RefreshSynthRuntimeSettingsFromConfig();
}

void AudioOutput::ResetDynamicsState() { limiterGain = 1.0f; }

void AudioOutput::ResetReverbState() {
  reverbWriteIndex = 0;
  reverbFilterL = 0.0f;
  reverbFilterR = 0.0f;
  reverbBlurL1 = 0.0f;
  reverbBlurR1 = 0.0f;
  reverbBlurL2 = 0.0f;
  reverbBlurR2 = 0.0f;
  if (!reverbBufferL.empty())
    std::fill(reverbBufferL.begin(), reverbBufferL.end(), 0.0f);
  if (!reverbBufferR.empty())
    std::fill(reverbBufferR.begin(), reverbBufferR.end(), 0.0f);
  reverbStateActive = false;
}

void AudioOutput::EnsureReverbState(int sampleRate) {
  if (reverbSampleRate == sampleRate && !reverbBufferL.empty())
    return;

  int bufferFrames = sampleRate * 3 / 2;
  if (bufferFrames < BLOCK_SIZE)
    bufferFrames = BLOCK_SIZE;

  reverbBufferL.assign(bufferFrames, 0.0f);
  reverbBufferR.assign(bufferFrames, 0.0f);
  reverbWriteIndex = 0;
  reverbSampleRate = sampleRate;
  reverbFilterL = 0.0f;
  reverbFilterR = 0.0f;
  reverbBlurL1 = 0.0f;
  reverbBlurR1 = 0.0f;
  reverbBlurL2 = 0.0f;
  reverbBlurR2 = 0.0f;
  reverbStateActive = false;
}

void AudioOutput::ApplyLoFiReverb(float *buffer, int numFrames, int sampleRate,
                                  bool enabled, float mix, float feedback,
                                  float tone, float width, float blur) {
  if (!enabled || mix <= 0.001f) {
    if (reverbStateActive)
      ResetReverbState();
    return;
  }

  EnsureReverbState(sampleRate);
  reverbStateActive = true;

  mix = ClampFloat(mix, 0.0f, 1.0f);
  feedback = ClampFloat(feedback, 0.0f, 0.97f);
  tone = ClampFloat(tone, 0.02f, 0.95f);
  width = ClampFloat(width, 0.0f, 1.0f);
  blur = ClampFloat(blur, 0.0f, 1.0f);

  float blurA = 0.18f + blur * 0.42f;
  float blurB = 0.10f + blur * 0.30f;
  float blurGain = 1.0f + blur * 0.45f;

  const int delayA = (sampleRate * 37) / 1000;
  const int delayB = (sampleRate * 61) / 1000;
  const int delayC = (sampleRate * 89) / 1000;
  const int delayD = (sampleRate * 127) / 1000;
  const int bufferFrames = static_cast<int>(reverbBufferL.size());

  for (int i = 0; i < numFrames; ++i) {
    int readA = reverbWriteIndex - delayA;
    int readB = reverbWriteIndex - delayB;
    int readC = reverbWriteIndex - delayC;
    int readD = reverbWriteIndex - delayD;
    if (readA < 0)
      readA += bufferFrames;
    if (readB < 0)
      readB += bufferFrames;
    if (readC < 0)
      readC += bufferFrames;
    if (readD < 0)
      readD += bufferFrames;

    float dryL = buffer[i * 2];
    float dryR = buffer[i * 2 + 1];
    float monoIn = (dryL + dryR) * 0.5f;

    float tapL = reverbBufferL[readA] * 0.58f + reverbBufferL[readC] * 0.42f +
                 reverbBufferR[readB] * 0.26f;
    float tapR = reverbBufferR[readB] * 0.58f + reverbBufferR[readD] * 0.42f +
                 reverbBufferL[readC] * 0.26f;

    reverbFilterL += (tapL - reverbFilterL) * tone;
    reverbFilterR += (tapR - reverbFilterR) * tone;

    float wetL = reverbFilterL * (0.75f + width * 0.25f) +
                 reverbFilterR * (0.25f * (1.0f - width));
    float wetR = reverbFilterR * (0.75f + width * 0.25f) +
                 reverbFilterL * (0.25f * (1.0f - width));

    // Diffusion blur: two short smoothing stages to smear the wet path and
    // reduce the obvious retro tap character.
    reverbBlurL1 += (wetL - reverbBlurL1) * blurA;
    reverbBlurR1 += (wetR - reverbBlurR1) * blurA;
    reverbBlurL2 += (reverbBlurL1 - reverbBlurL2) * blurB;
    reverbBlurR2 += (reverbBlurR1 - reverbBlurR2) * blurB;
    wetL = reverbBlurL2 * blurGain;
    wetR = reverbBlurR2 * blurGain;

    buffer[i * 2] = dryL + wetL * mix;
    buffer[i * 2 + 1] = dryR + wetR * mix;

    float feedbackL = SoftLimit(monoIn + reverbFilterR * feedback);
    float feedbackR = SoftLimit(monoIn + reverbFilterL * feedback);
    reverbBufferL[reverbWriteIndex] = feedbackL;
    reverbBufferR[reverbWriteIndex] = feedbackR;

    ++reverbWriteIndex;
    if (reverbWriteIndex >= bufferFrames)
      reverbWriteIndex = 0;
  }
}

void AudioOutput::ApplyLimiter(float *buffer, int numFrames, int sampleRate,
                               bool enabled, float threshold,
                               float releaseMs) {
  if (!enabled)
    return;

  threshold = ClampFloat(threshold, 0.1f, 1.0f);
  releaseMs = ClampFloat(releaseMs, 5.0f, 2500.0f);

  float releaseSamples = (releaseMs * 0.001f) * sampleRate;
  if (releaseSamples < 1.0f)
    releaseSamples = 1.0f;
  float releaseCoeff = expf(-1.0f / releaseSamples);

  for (int i = 0; i < numFrames; ++i) {
    float *frame = &buffer[i * 2];
    float peak = (std::max)(fabsf(frame[0]), fabsf(frame[1]));
    float desiredGain =
        (peak > threshold && peak > FLT_MIN) ? (threshold / peak) : 1.0f;

    if (desiredGain < limiterGain) {
      limiterGain = desiredGain;
    } else {
      limiterGain =
          desiredGain + (limiterGain - desiredGain) * releaseCoeff;
    }

    frame[0] *= limiterGain;
    frame[1] *= limiterGain;
  }
}

bool AudioOutput::Start() {
  OutputDebugStringA("SVMS: AudioOutput::Start() called\n");
  compat::LockGuard<compat::Mutex> lock(mutex);
  if (refCount > 0) {
    refCount++;
    OutputDebugStringA(
        "SVMS: AudioOutput already running, incremented refCount\n");
    return true;
  }

  running = true;
  renderSampleCursor = 0;
  synthRuntimeSettingsSeeded = false;
  cachedTimingMode.clear();
  runtimeConfigRefreshRequested.store(false, std::memory_order_relaxed);
  SetResolvedBackendName("starting");
  OutputDebugStringA("SVMS: Creating audio thread...\n");
#ifdef SVMS_LEGACY_XP
  audioThread = CreateThread(NULL, 0, &AudioOutput::AudioThreadProc, this, 0, NULL);
  if (!audioThread) {
    OutputDebugStringA("SVMS: Failed to create audio thread\n");
    running = false;
    return false;
  }
#else
  audioThread = std::thread(&AudioOutput::AudioThreadFunc, this);
#endif
  OutputDebugStringA("SVMS: Audio thread created\n");
  refCount = 1;
  return true;
}

void AudioOutput::Stop(bool waitForThread) {
  compat::LockGuard<compat::Mutex> lock(mutex);
  if (refCount > 0) {
    refCount--;
    if (refCount > 0) {
      OutputDebugStringA(
          "SVMS: AudioOutput::Stop() - refCount > 0, continuing\n");
      return;
    }
  }

  if (!running.load())
    return;
  running = false;
#ifdef SVMS_LEGACY_XP
  if (audioThread) {
    if (waitForThread)
      WaitForSingleObject(audioThread, INFINITE);
    CloseHandle(audioThread);
    audioThread = NULL;
  }
#else
  if (audioThread.joinable()) {
    if (waitForThread)
      audioThread.join();
    else
      audioThread.detach();
  }
#endif
  renderSampleCursor = 0;
  synthRuntimeSettingsSeeded = false;
  cachedTimingMode.clear();
  runtimeConfigRefreshRequested.store(false, std::memory_order_relaxed);
  SetResolvedBackendName("stopped");
}

void AudioOutput::ForceStop(bool waitForThread) {
  compat::LockGuard<compat::Mutex> lock(mutex);
  refCount = 0;

  if (!running.load()) {
    SetResolvedBackendName("stopped");
    return;
  }

  running = false;
#ifdef SVMS_LEGACY_XP
  if (audioThread) {
    if (waitForThread)
      WaitForSingleObject(audioThread, INFINITE);
    CloseHandle(audioThread);
    audioThread = NULL;
  }
#else
  if (audioThread.joinable()) {
    if (waitForThread)
      audioThread.join();
    else
      audioThread.detach();
  }
#endif
  renderSampleCursor = 0;
  synthRuntimeSettingsSeeded = false;
  cachedTimingMode.clear();
  runtimeConfigRefreshRequested.store(false, std::memory_order_relaxed);
  SetResolvedBackendName("killed");
}

#ifdef SVMS_LEGACY_XP
DWORD WINAPI AudioOutput::AudioThreadProc(LPVOID param) {
  static_cast<AudioOutput *>(param)->AudioThreadFunc();
  return 0;
}
#endif

bool AudioOutput::RunBackend(AudioBackendId requestedBackend, SystemWinmm &sys,
                             int sampleRate, LARGE_INTEGER &freq,
                             LARGE_INTEGER &lastChunkTime) {
  AudioBackendId candidates[3];
  int candidateCount = 0;

  auto addCandidate = [&](AudioBackendId backend) {
    for (int i = 0; i < candidateCount; ++i) {
      if (candidates[i] == backend)
        return;
    }
    candidates[candidateCount++] = backend;
  };

#ifdef SVMS_LEGACY_XP
  if (requestedBackend == AudioBackendId::AUTO) {
    addCandidate(AudioBackendId::WAVEOUT);
    addCandidate(AudioBackendId::DSOUND);
  } else {
    addCandidate(requestedBackend);
    addCandidate(AudioBackendId::WAVEOUT);
    addCandidate(AudioBackendId::DSOUND);
  }
#else
  if (requestedBackend == AudioBackendId::AUTO) {
    addCandidate(AudioBackendId::WASAPI_SHARED);
    addCandidate(AudioBackendId::WAVEOUT);
    addCandidate(AudioBackendId::DSOUND);
  } else {
    addCandidate(requestedBackend);
    addCandidate(AudioBackendId::WASAPI_SHARED);
    addCandidate(AudioBackendId::WAVEOUT);
    addCandidate(AudioBackendId::DSOUND);
  }
#endif

  for (int i = 0; i < candidateCount && running.load(); ++i) {
    AudioBackendId candidate = candidates[i];
    if (!IsAudioBackendSupportedInBuild(candidate)) {
      char buf[192];
      sprintf(buf, "SVMS: Requested backend %s is unavailable in this build\n",
              AudioBackendToDisplayString(candidate));
      OutputDebugStringA(buf);
      continue;
    }

    char buf[192];
    sprintf(buf, "SVMS: Trying backend candidate %s\n",
            AudioBackendToDisplayString(candidate));
    OutputDebugStringA(buf);

    SetResolvedBackendName(AudioBackendToDisplayString(candidate));

    bool success = false;
    switch (candidate) {
    case AudioBackendId::WAVEOUT:
      success = RunWaveOutBackend(sys, sampleRate, freq, lastChunkTime);
      break;
    case AudioBackendId::DSOUND:
      success = RunDirectSoundBackend(sampleRate, freq, lastChunkTime);
      break;
    case AudioBackendId::WASAPI_SHARED:
      success = RunWasapiSharedBackend(sampleRate, freq, lastChunkTime);
      break;
    default:
      break;
    }

    if (!success)
      continue;

    char status[SVMS_MAX_STATUS_TEXT];
    if (requestedBackend == AudioBackendId::AUTO) {
      sprintf(status, "Auto selected %s backend",
              AudioBackendToDisplayString(candidate));
    } else if (candidate == requestedBackend) {
      sprintf(status, "Using %s backend",
              AudioBackendToDisplayString(candidate));
    } else {
      sprintf(status, "Requested %s backend failed, using %s",
              AudioBackendToDisplayString(requestedBackend),
              AudioBackendToDisplayString(candidate));
    }
    LiveRuntime::Instance().PublishStatus(status);
    return true;
  }

  SetResolvedBackendName("unavailable");
  return false;
}

void AudioOutput::AudioThreadFunc() {
  OutputDebugStringA("SVMS: ===== Audio thread STARTING =====\n");
  OutputDebugStringA("SVMS: Audio engine by LexonBlackzz\n");
  SystemWinmm &sys = SystemWinmm::Instance();
  SetResolvedBackendName("starting");

  // Request high resolution timer for low latency Sleep(1)
  if (sys.Real_timeBeginPeriod)
    sys.Real_timeBeginPeriod(1);

  // Timing for Synchronous Mode
  LARGE_INTEGER freq, lastChunkTime;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&lastChunkTime);

  int sampleRate = Config::Instance().GetInt("sample_rate", 44100);
  LoadAudioSettings();
  ResetDynamicsState();
  std::string backendName =
      Config::Instance().GetString("audio_backend", "auto");
  AudioBackendId backend = ParseAudioBackend(backendName);
  char buf[160];
  sprintf(buf, "SVMS: Audio backend requested: %s\n",
          AudioBackendToDisplayString(backend));
  OutputDebugStringA(buf);

  bool success = RunBackend(backend, sys, sampleRate, freq, lastChunkTime);

  if (!success && running.load()) {
    OutputDebugStringA("SVMS: ERROR - No audio backend could be initialized\n");
    LiveRuntime::Instance().PublishError(
        "No audio backend could be initialized");
  }

  if (sys.Real_timeEndPeriod)
    sys.Real_timeEndPeriod(1);
  if (!running.load())
    SetResolvedBackendName("stopped");
  OutputDebugStringA("SVMS: Audio thread stopped\n");
}

int AudioOutput::FillOutputBuffer(int16_t *destPtr, int numFrames, int sampleRate,
                                  int loopCount, LARGE_INTEGER &freq,
                                  LARGE_INTEGER &lastChunkTime,
                                  int &configReloadCounter) {
  bool requestedRuntimeRefresh =
      runtimeConfigRefreshRequested.exchange(false, std::memory_order_relaxed);
  if (requestedRuntimeRefresh || ++configReloadCounter >= 50) {
    if (requestedRuntimeRefresh) {
      OutputDebugStringA(
          "SVMS: Audio thread processing requested runtime config refresh\n");
    }
    Config::Instance().Reload();
    LoadAudioSettings();
    configReloadCounter = 0;
  }

  const bool usePollingRate = IsQuantizedTimingModeEnabled();
  const bool useWasapiAsyncFeed =
      !usePollingRate && IsAccurateTimingModeEnabled() &&
      audioSettings.wasapiAsyncFeed &&
      GetResolvedBackendName() == "WASAPI Shared";

  int renderBlockSize = numFrames;
  if (renderBlockSize < 1)
    renderBlockSize = 1;
  if (usePollingRate && audioSettings.pollingRate > 0) {
    renderBlockSize = sampleRate / audioSettings.pollingRate;
    if (renderBlockSize < 1)
      renderBlockSize = 1;
    if (renderBlockSize > BLOCK_SIZE)
      renderBlockSize = BLOCK_SIZE;
  }

  if ((int)mixBuffer.size() < renderBlockSize * 2)
    mixBuffer.resize(renderBlockSize * 2);

  {
    static bool s_lastLoggedUsePollingRate = false;
    static bool s_lastLoggedUseWasapiAsyncFeed = false;
    static int s_lastLoggedRenderBlockSize = -1;
    static int s_lastLoggedNumFrames = -1;
    static std::string s_lastLoggedResolvedBackend;

    const std::string resolvedBackend = GetResolvedBackendName();
    if (s_lastLoggedUsePollingRate != usePollingRate ||
        s_lastLoggedUseWasapiAsyncFeed != useWasapiAsyncFeed ||
        s_lastLoggedRenderBlockSize != renderBlockSize ||
        s_lastLoggedNumFrames != numFrames ||
        s_lastLoggedResolvedBackend != resolvedBackend) {
      char chunkBuf[256];
      sprintf(chunkBuf,
              "SVMS: FillOutputBuffer mode backend=%s polling=%u accurate=%u wasapiAsync=%u deviceFrames=%d renderBlock=%d\n",
              resolvedBackend.c_str(), usePollingRate ? 1u : 0u,
              IsAccurateTimingModeEnabled() ? 1u : 0u,
              useWasapiAsyncFeed ? 1u : 0u, numFrames, renderBlockSize);
      OutputDebugStringA(chunkBuf);
      s_lastLoggedUsePollingRate = usePollingRate;
      s_lastLoggedUseWasapiAsyncFeed = useWasapiAsyncFeed;
      s_lastLoggedRenderBlockSize = renderBlockSize;
      s_lastLoggedNumFrames = numFrames;
      s_lastLoggedResolvedBackend = resolvedBackend;
    }
  }

  int samplesFilled = 0;
  int16_t maxVal = 0;
  float reportedSynthRenderMs = 0.0f;
  float reportedAudioBlockMs = 0.0f;
  float reportedBudgetMs = 0.0f;

  if (usePollingRate && audioSettings.pollingRate > 0) {
    reportedBudgetMs =
        (float)((double)renderBlockSize * 1000.0 / (double)sampleRate);
  } else {
    reportedBudgetMs =
        (float)((double)numFrames * 1000.0 / (double)sampleRate);
  }

  while (samplesFilled < numFrames) {
    int toRender = numFrames - samplesFilled;
    if (toRender > renderBlockSize)
      toRender = renderBlockSize;

    LARGE_INTEGER renderStart;
    LARGE_INTEGER renderEnd;
    LARGE_INTEGER blockStart;
    LARGE_INTEGER blockEnd;
    LARGE_INTEGER renderBlockEndQpc;

    if (usePollingRate && audioSettings.pollingRate > 0) {
      double targetSeconds = (double)toRender / sampleRate;
      LARGE_INTEGER targetTicks;
      targetTicks.QuadPart =
          lastChunkTime.QuadPart + (LONGLONG)(targetSeconds * freq.QuadPart);

      LARGE_INTEGER now;
      while (true) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= targetTicks.QuadPart)
          break;

        double waitS =
            (double)(targetTicks.QuadPart - now.QuadPart) / freq.QuadPart;
        if (waitS > 0.002)
          Sleep(1);
        else if (waitS > 0.0005)
          Sleep(0);
        else
          YieldProcessor();

        if (!running.load())
          break;
      }

      if (now.QuadPart - targetTicks.QuadPart > freq.QuadPart)
        lastChunkTime = now;
      else
        lastChunkTime = targetTicks;
    } else {
      QueryPerformanceCounter(&lastChunkTime);
    }

    QueryPerformanceCounter(&blockStart);
    QueryPerformanceCounter(&renderStart);
    renderBlockEndQpc = renderStart;
    if (sampleRate > 0 && freq.QuadPart > 0) {
      renderBlockEndQpc.QuadPart +=
          (LONGLONG)(((double)toRender / (double)sampleRate) * freq.QuadPart);
    }
    Synth::Instance().SetRealtimeBudgetMs(
        (float)((double)toRender * 1000.0 / (double)sampleRate));
    Synth::Instance().SetRenderBlockContext(
        renderSampleCursor, toRender, sampleRate, renderStart.QuadPart,
        renderBlockEndQpc.QuadPart, usePollingRate && audioSettings.pollingRate > 0);
    Synth::Instance().Render(mixBuffer.data(), toRender);
    QueryPerformanceCounter(&renderEnd);
    ApplyLoFiReverb(mixBuffer.data(), toRender, sampleRate,
                    audioSettings.reverbEnabled, audioSettings.reverbMix,
                    audioSettings.reverbFeedback, audioSettings.reverbTone,
                    audioSettings.reverbWidth, audioSettings.reverbBlur);
    ApplyLimiter(mixBuffer.data(), toRender, sampleRate,
                 audioSettings.limiterEnabled,
                 audioSettings.limiterThreshold,
                 audioSettings.limiterReleaseMs);
    int totalSamples = toRender * 2;

#if SVMS_ENABLE_AVX_INTRINSICS
    if (CpuFeatures::HasAVX()) {
      ProcessSamplesAVX(mixBuffer.data(), destPtr, totalSamples,
                        audioSettings.masterVolume);
    } else {
      ProcessSamplesSSE2(mixBuffer.data(), destPtr, totalSamples,
                         audioSettings.masterVolume);
    }
#else
    ProcessSamplesSSE2(mixBuffer.data(), destPtr, totalSamples,
                       audioSettings.masterVolume);
#endif

    QueryPerformanceCounter(&blockEnd);
    float synthRenderMs =
        (float)((double)(renderEnd.QuadPart - renderStart.QuadPart) * 1000.0 /
                (double)freq.QuadPart);
    float audioBlockMs =
        (float)((double)(blockEnd.QuadPart - blockStart.QuadPart) * 1000.0 /
                (double)freq.QuadPart);
    if (usePollingRate && audioSettings.pollingRate > 0) {
      if (synthRenderMs > reportedSynthRenderMs)
        reportedSynthRenderMs = synthRenderMs;
      if (audioBlockMs > reportedAudioBlockMs)
        reportedAudioBlockMs = audioBlockMs;
    } else {
      reportedSynthRenderMs += synthRenderMs;
      reportedAudioBlockMs += audioBlockMs;
    }

    if (loopCount < 10) {
      for (int i = 0; i < totalSamples; i += 16) {
        if (abs(destPtr[i]) > maxVal)
          maxVal = abs(destPtr[i]);
      }
    }

    destPtr += totalSamples;
    samplesFilled += toRender;
    renderSampleCursor += (unsigned long long)toRender;
  }

  LiveRuntime::Instance().UpdateAudioTimings(
      reportedSynthRenderMs, reportedAudioBlockMs, reportedBudgetMs);

  return maxVal;
}

#ifndef SVMS_LEGACY_XP
bool AudioOutput::RunWasapiSharedBackend(int sampleRate, LARGE_INTEGER &freq,
                                         LARGE_INTEGER &lastChunkTime) {
  OutputDebugStringA("SVMS: Trying WASAPI shared backend\n");

  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool shouldCoUninitialize = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - CoInitializeEx failed for WASAPI: 0x%08lX\n",
            (unsigned long)hr);
    OutputDebugStringA(buf);
    return false;
  }

  IMMDeviceEnumerator *enumerator = NULL;
  IMMDevice *device = NULL;
  IAudioClient *audioClient = NULL;
  IAudioRenderClient *renderClient = NULL;
  WAVEFORMATEX *closestFormat = NULL;
  HANDLE sampleReadyEvent = NULL;
  HANDLE mmcssHandle = NULL;
  DWORD mmcssTaskIndex = 0;
  bool started = false;

  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                        __uuidof(IMMDeviceEnumerator),
                        reinterpret_cast<void **>(&enumerator));
  if (FAILED(hr) || !enumerator) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - CoCreateInstance(MMDeviceEnumerator) failed: "
                 "0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (FAILED(hr) || !device) {
    char buf[160];
    sprintf(buf,
            "SVMS: ERROR - GetDefaultAudioEndpoint failed: 0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                        reinterpret_cast<void **>(&audioClient));
  if (FAILED(hr) || !audioClient) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - Activate(IAudioClient) failed: 0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  WAVEFORMATEX desiredFormat;
  memset(&desiredFormat, 0, sizeof(desiredFormat));
  desiredFormat.wFormatTag = WAVE_FORMAT_PCM;
  desiredFormat.nChannels = 2;
  desiredFormat.nSamplesPerSec = sampleRate;
  desiredFormat.wBitsPerSample = 16;
  desiredFormat.nBlockAlign =
      (desiredFormat.wBitsPerSample * desiredFormat.nChannels) >> 3;
  desiredFormat.nAvgBytesPerSec =
      desiredFormat.nSamplesPerSec * desiredFormat.nBlockAlign;

  hr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &desiredFormat,
                                      &closestFormat);
  if (hr == S_FALSE && closestFormat) {
    char buf[256];
    sprintf(buf,
            "SVMS: WASAPI shared suggested fallback format: %lu Hz, %u bits, "
            "%u channels\n",
            (unsigned long)closestFormat->nSamplesPerSec,
            closestFormat->wBitsPerSample, closestFormat->nChannels);
    OutputDebugStringA(buf);
  }
  if (closestFormat) {
    CoTaskMemFree(closestFormat);
    closestFormat = NULL;
  }

  REFERENCE_TIME bufferDuration =
      static_cast<REFERENCE_TIME>((10000000.0 * BLOCK_SIZE * 4) / sampleRate);
  if (bufferDuration < 100000)
    bufferDuration = 100000;

  hr = audioClient->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
          AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
      bufferDuration, 0, &desiredFormat, NULL);
  if (FAILED(hr)) {
    char buf[192];
    sprintf(buf, "SVMS: ERROR - WASAPI shared Initialize failed: 0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  sampleReadyEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (!sampleReadyEvent) {
    OutputDebugStringA("SVMS: ERROR - Failed to create WASAPI event handle\n");
    goto cleanup;
  }

  hr = audioClient->SetEventHandle(sampleReadyEvent);
  if (FAILED(hr)) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - WASAPI SetEventHandle failed: 0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  hr = audioClient->GetService(__uuidof(IAudioRenderClient),
                               reinterpret_cast<void **>(&renderClient));
  if (FAILED(hr) || !renderClient) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - WASAPI GetService(IAudioRenderClient) failed: "
                 "0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  UINT32 bufferFrameCount = 0;
  hr = audioClient->GetBufferSize(&bufferFrameCount);
  if (FAILED(hr) || bufferFrameCount == 0) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - WASAPI GetBufferSize failed: 0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  BYTE *initialBuffer = NULL;
  hr = renderClient->GetBuffer(bufferFrameCount, &initialBuffer);
  if (FAILED(hr) || !initialBuffer) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - WASAPI initial GetBuffer failed: 0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  int loopCount = 0;
  int configReloadCounter = 0;
  int maxVal = FillOutputBuffer(reinterpret_cast<int16_t *>(initialBuffer),
                                static_cast<int>(bufferFrameCount), sampleRate,
                                loopCount, freq, lastChunkTime,
                                configReloadCounter);
  hr = renderClient->ReleaseBuffer(bufferFrameCount, 0);
  if (FAILED(hr)) {
    char buf[160];
    sprintf(buf,
            "SVMS: ERROR - WASAPI initial ReleaseBuffer failed: 0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  char primeBuf[160];
  sprintf(primeBuf, "SVMS: WASAPI shared primed %lu frames, max audio=%d\n",
          (unsigned long)bufferFrameCount, maxVal);
  OutputDebugStringA(primeBuf);

  mmcssHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &mmcssTaskIndex);

  hr = audioClient->Start();
  if (FAILED(hr)) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - WASAPI Start failed: 0x%08lX (%s)\n",
            (unsigned long)hr, WasapiErrorToString(hr));
    OutputDebugStringA(buf);
    goto cleanup;
  }

  started = true;
  OutputDebugStringA("SVMS: WASAPI shared playback started\n");

  while (running.load()) {
    DWORD waitResult = WaitForSingleObject(sampleReadyEvent, 200);
    if (waitResult == WAIT_TIMEOUT)
      continue;
    if (waitResult != WAIT_OBJECT_0) {
      OutputDebugStringA("SVMS: WASAPI wait failed, leaving backend loop\n");
      break;
    }

    UINT32 padding = 0;
    hr = audioClient->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
      char buf[160];
      sprintf(buf, "SVMS: ERROR - WASAPI GetCurrentPadding failed: 0x%08lX "
                   "(%s)\n",
              (unsigned long)hr, WasapiErrorToString(hr));
      OutputDebugStringA(buf);
      break;
    }

    UINT32 availableFrames = bufferFrameCount - padding;
    if (availableFrames == 0)
      continue;

    BYTE *bufferData = NULL;
    hr = renderClient->GetBuffer(availableFrames, &bufferData);
    if (FAILED(hr) || !bufferData) {
      char buf[160];
      sprintf(buf, "SVMS: ERROR - WASAPI GetBuffer failed: 0x%08lX (%s)\n",
              (unsigned long)hr, WasapiErrorToString(hr));
      OutputDebugStringA(buf);
      break;
    }

    maxVal = FillOutputBuffer(reinterpret_cast<int16_t *>(bufferData),
                              static_cast<int>(availableFrames), sampleRate,
                              loopCount, freq, lastChunkTime,
                              configReloadCounter);
    hr = renderClient->ReleaseBuffer(availableFrames, 0);
    if (FAILED(hr)) {
      char buf[160];
      sprintf(buf,
              "SVMS: ERROR - WASAPI ReleaseBuffer failed: 0x%08lX (%s)\n",
              (unsigned long)hr, WasapiErrorToString(hr));
      OutputDebugStringA(buf);
      break;
    }

    if (++loopCount <= 10) {
      char buf[192];
      sprintf(buf,
              "SVMS: Wrote WASAPI block %d, frames=%lu padding=%lu max=%d\n",
              loopCount, (unsigned long)availableFrames,
              (unsigned long)padding, maxVal);
      OutputDebugStringA(buf);
    }
  }

cleanup:
  if (started && audioClient)
    audioClient->Stop();
  if (mmcssHandle)
    AvRevertMmThreadCharacteristics(mmcssHandle);
  if (renderClient)
    renderClient->Release();
  if (audioClient)
    audioClient->Release();
  if (device)
    device->Release();
  if (enumerator)
    enumerator->Release();
  if (closestFormat)
    CoTaskMemFree(closestFormat);
  if (sampleReadyEvent)
    CloseHandle(sampleReadyEvent);
  if (shouldCoUninitialize)
    CoUninitialize();

  return started;
}
#else
bool AudioOutput::RunWasapiSharedBackend(int, LARGE_INTEGER &, LARGE_INTEGER &) {
  OutputDebugStringA("SVMS: WASAPI shared backend unavailable in legacy XP "
                     "build\n");
  return false;
}
#endif

bool AudioOutput::RunWaveOutBackend(SystemWinmm &sys, int sampleRate,
                                    LARGE_INTEGER &freq,
                                    LARGE_INTEGER &lastChunkTime) {
  OutputDebugStringA("SVMS: Trying waveOut backend\n");

  // Ensure we have the functions
  OutputDebugStringA("SVMS: Checking SystemWinmm functions...\n");
  if (!sys.Real_waveOutOpen) {
    OutputDebugStringA("SVMS: ERROR - Real_waveOutOpen is NULL!\n");
    return false;
  }
  if (!sys.Real_waveOutWrite) {
    OutputDebugStringA("SVMS: ERROR - Real_waveOutWrite is NULL!\n");
    return false;
  }
  if (!sys.Real_waveOutPrepareHeader) {
    OutputDebugStringA("SVMS: ERROR - Real_waveOutPrepareHeader is NULL!\n");
    return false;
  }
  OutputDebugStringA("SVMS: SystemWinmm functions OK\n");
  LogWaveOutDevices(sys);

  if (sys.Real_waveOutGetNumDevs && sys.Real_waveOutGetNumDevs() == 0) {
    OutputDebugStringA(
        "SVMS: waveOut backend unavailable because no devices were reported\n");
    return false;
  }

  HWAVEOUT hWaveOut = NULL;
  WAVEFORMATEX wfx;
  OutputDebugStringA("SVMS: Got sample rate from config\n");

  memset(&wfx, 0, sizeof(wfx));
  wfx.nSamplesPerSec = sampleRate;
  wfx.wBitsPerSample = 16;
  wfx.nChannels = 2;
  wfx.cbSize = 0;
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nBlockAlign = (wfx.wBitsPerSample * wfx.nChannels) >> 3;
  wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;

  OutputDebugStringA("SVMS: Calling waveOutOpen...\n");
  MMRESULT res =
      sys.Real_waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
  if (res != MMSYSERR_NOERROR) {
    char buf[128];
    sprintf(buf,
            "SVMS: ERROR - Failed to open WAVE_MAPPER. Error code: %u (%s)\n",
            (unsigned int)res, WaveOutErrorToString(res));
    OutputDebugStringA(buf);
    return false;
  }

  OutputDebugStringA("SVMS: waveOut device opened successfully\n");

  OutputDebugStringA("SVMS: Preparing waveOut buffers...\n");
  std::vector<WAVEHDR> headers(BLOCK_COUNT);
  std::vector<std::vector<int16_t>> buffers(BLOCK_COUNT);

  for (int i = 0; i < BLOCK_COUNT; ++i) {
    buffers[i].resize(BLOCK_SIZE * 2);
    memset(&headers[i], 0, sizeof(WAVEHDR));
    headers[i].lpData = (LPSTR)buffers[i].data();
    headers[i].dwBufferLength = BLOCK_SIZE * 2 * sizeof(int16_t);
    headers[i].dwFlags = 0;
    MMRESULT prepRes =
        sys.Real_waveOutPrepareHeader(hWaveOut, &headers[i], sizeof(WAVEHDR));
    if (prepRes != MMSYSERR_NOERROR) {
      char buf[128];
      sprintf(buf, "SVMS: ERROR - waveOutPrepareHeader failed: %u (%s)\n",
              (unsigned int)prepRes, WaveOutErrorToString(prepRes));
      OutputDebugStringA(buf);
      sys.Real_waveOutClose(hWaveOut);
      return false;
    }
    // We mark them as DONE initially so the loop fills them immediately
    headers[i].dwFlags |= WHDR_DONE;
  }
  OutputDebugStringA("SVMS: Buffers prepared OK\n");

  int currentBlock = 0;
  int loopCount = 0;

  OutputDebugStringA("SVMS: Audio loop starting...\n");
  int configReloadCounter = 0;
  while (running.load()) {
    WAVEHDR &hdr = headers[currentBlock];

    // Wait for the buffer to be returned by the driver
    int waitCycles = 0;
    while (!(hdr.dwFlags & WHDR_DONE)) {
      if (!running.load())
        break;
      if (++waitCycles > 1000)
        Sleep(1);
      else
        Sleep(0);
    }
    if (!running.load())
      break;

    int16_t maxVal =
        FillOutputBuffer(buffers[currentBlock].data(), BLOCK_SIZE, sampleRate,
                         loopCount, freq, lastChunkTime, configReloadCounter);

    if (++loopCount <= 10) {
      char buf[128];
      sprintf(buf, "SVMS: Wrote block %d, max audio=%d\n", loopCount, maxVal);
      OutputDebugStringA(buf);
    }

    hdr.dwFlags &= ~WHDR_DONE;
    MMRESULT writeRes = sys.Real_waveOutWrite(hWaveOut, &hdr, sizeof(WAVEHDR));
    if (writeRes != MMSYSERR_NOERROR) {
      char buf[128];
      sprintf(buf, "SVMS: ERROR - waveOutWrite failed: %u (%s)\n",
              (unsigned int)writeRes, WaveOutErrorToString(writeRes));
      OutputDebugStringA(buf);
    }

    currentBlock = (currentBlock + 1) % BLOCK_COUNT;
  }

  OutputDebugStringA("SVMS: Cleaning up audio output\n");
  sys.Real_waveOutReset(hWaveOut);
  for (int i = 0; i < BLOCK_COUNT; ++i) {
    sys.Real_waveOutUnprepareHeader(hWaveOut, &headers[i], sizeof(WAVEHDR));
  }
  sys.Real_waveOutClose(hWaveOut);
  return true;
}

bool AudioOutput::RunDirectSoundBackend(int sampleRate, LARGE_INTEGER &freq,
                                        LARGE_INTEGER &lastChunkTime) {
  OutputDebugStringA("SVMS: Trying DirectSound backend\n");

  HRESULT hr;
  IDirectSound *directSound = NULL;
  IDirectSoundBuffer *primaryBuffer = NULL;
  IDirectSoundBuffer *secondaryBuffer = NULL;

  hr = DirectSoundCreate(NULL, &directSound, NULL);
  if (FAILED(hr) || !directSound) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - DirectSoundCreate failed: 0x%08lX (%s)\n",
            (unsigned long)hr, DirectSoundErrorToString(hr));
    OutputDebugStringA(buf);
    return false;
  }

  HWND focusWindow = GetDesktopWindow();
  hr = directSound->SetCooperativeLevel(focusWindow, DSSCL_PRIORITY);
  if (FAILED(hr)) {
    hr = directSound->SetCooperativeLevel(focusWindow, DSSCL_NORMAL);
  }
  if (FAILED(hr)) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - SetCooperativeLevel failed: 0x%08lX (%s)\n",
            (unsigned long)hr, DirectSoundErrorToString(hr));
    OutputDebugStringA(buf);
    directSound->Release();
    return false;
  }

  WAVEFORMATEX wfx;
  memset(&wfx, 0, sizeof(wfx));
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = 2;
  wfx.nSamplesPerSec = sampleRate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = (wfx.wBitsPerSample * wfx.nChannels) >> 3;
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  DSBUFFERDESC primaryDesc;
  memset(&primaryDesc, 0, sizeof(primaryDesc));
  primaryDesc.dwSize = sizeof(primaryDesc);
  primaryDesc.dwFlags = DSBCAPS_PRIMARYBUFFER;

  hr = directSound->CreateSoundBuffer(&primaryDesc, &primaryBuffer, NULL);
  if (SUCCEEDED(hr) && primaryBuffer) {
    primaryBuffer->SetFormat(&wfx);
  }

  const DWORD blockBytes = BLOCK_SIZE * wfx.nBlockAlign;
  const DWORD bufferBytes = blockBytes * BLOCK_COUNT;

  DSBUFFERDESC secondaryDesc;
  memset(&secondaryDesc, 0, sizeof(secondaryDesc));
  secondaryDesc.dwSize = sizeof(secondaryDesc);
  secondaryDesc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
  secondaryDesc.dwBufferBytes = bufferBytes;
  secondaryDesc.lpwfxFormat = &wfx;

  hr = directSound->CreateSoundBuffer(&secondaryDesc, &secondaryBuffer, NULL);
  if (FAILED(hr) || !secondaryBuffer) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - CreateSoundBuffer failed: 0x%08lX (%s)\n",
            (unsigned long)hr, DirectSoundErrorToString(hr));
    OutputDebugStringA(buf);
    if (primaryBuffer)
      primaryBuffer->Release();
    directSound->Release();
    return false;
  }

  void *lockPtr1 = NULL;
  void *lockPtr2 = NULL;
  DWORD lockBytes1 = 0;
  DWORD lockBytes2 = 0;
  hr = secondaryBuffer->Lock(0, bufferBytes, &lockPtr1, &lockBytes1, &lockPtr2,
                             &lockBytes2, 0);
  if (FAILED(hr)) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - DirectSound initial lock failed: 0x%08lX (%s)\n",
            (unsigned long)hr, DirectSoundErrorToString(hr));
    OutputDebugStringA(buf);
    secondaryBuffer->Release();
    if (primaryBuffer)
      primaryBuffer->Release();
    directSound->Release();
    return false;
  }
  if (lockPtr1 && lockBytes1)
    memset(lockPtr1, 0, lockBytes1);
  if (lockPtr2 && lockBytes2)
    memset(lockPtr2, 0, lockBytes2);
  secondaryBuffer->Unlock(lockPtr1, lockBytes1, lockPtr2, lockBytes2);

  DWORD writeOffset = 0;
  int configReloadCounter = 0;
  int loopCount = 0;

  // Prime a few blocks before starting playback to avoid an immediate underrun.
  for (int i = 0; i < 4 && running.load(); ++i) {
    int maxVal = FillOutputBuffer(dsBlockBuffer.data(), BLOCK_SIZE, sampleRate,
                                  loopCount, freq, lastChunkTime,
                                  configReloadCounter);
    hr = secondaryBuffer->Lock(writeOffset, blockBytes, &lockPtr1, &lockBytes1,
                               &lockPtr2, &lockBytes2, 0);
    if (FAILED(hr)) {
      char buf[160];
      sprintf(buf, "SVMS: ERROR - DirectSound prime lock failed: 0x%08lX (%s)\n",
              (unsigned long)hr, DirectSoundErrorToString(hr));
      OutputDebugStringA(buf);
      secondaryBuffer->Release();
      if (primaryBuffer)
        primaryBuffer->Release();
      directSound->Release();
      return false;
    }
    if (lockPtr1 && lockBytes1)
      memcpy(lockPtr1, dsBlockBuffer.data(), lockBytes1);
    if (lockPtr2 && lockBytes2)
      memcpy(lockPtr2, (BYTE *)dsBlockBuffer.data() + lockBytes1, lockBytes2);
    secondaryBuffer->Unlock(lockPtr1, lockBytes1, lockPtr2, lockBytes2);

    if (++loopCount <= 10) {
      char buf[128];
      sprintf(buf, "SVMS: Primed DS block %d, max audio=%d\n", loopCount,
              maxVal);
      OutputDebugStringA(buf);
    }
    writeOffset = (writeOffset + blockBytes) % bufferBytes;
  }

  hr = secondaryBuffer->Play(0, 0, DSBPLAY_LOOPING);
  if (FAILED(hr)) {
    char buf[160];
    sprintf(buf, "SVMS: ERROR - DirectSound play failed: 0x%08lX (%s)\n",
            (unsigned long)hr, DirectSoundErrorToString(hr));
    OutputDebugStringA(buf);
    secondaryBuffer->Release();
    if (primaryBuffer)
      primaryBuffer->Release();
    directSound->Release();
    return false;
  }

  OutputDebugStringA("SVMS: DirectSound playback started\n");

  const DWORD targetAheadBytes = blockBytes * 6;
  const DWORD minSafeDistanceBytes = blockBytes * 2;
  while (running.load()) {
    DWORD playCursor = 0;
    DWORD writeCursor = 0;
    hr = secondaryBuffer->GetCurrentPosition(&playCursor, &writeCursor);
    if (FAILED(hr)) {
      char buf[160];
      sprintf(buf,
              "SVMS: ERROR - DirectSound GetCurrentPosition failed: 0x%08lX (%s)\n",
              (unsigned long)hr, DirectSoundErrorToString(hr));
      OutputDebugStringA(buf);
      break;
    }

    DWORD aheadBytes = (writeOffset + bufferBytes - playCursor) % bufferBytes;
    DWORD safeDistanceFromPlay =
        (playCursor + bufferBytes - writeOffset) % bufferBytes;
    if (aheadBytes >= targetAheadBytes ||
        safeDistanceFromPlay < minSafeDistanceBytes) {
      Sleep(1);
      continue;
    }

    int maxVal = FillOutputBuffer(dsBlockBuffer.data(), BLOCK_SIZE, sampleRate,
                                  loopCount, freq, lastChunkTime,
                                  configReloadCounter);
    DWORD safeWriteOffset = writeOffset;
    DWORD distanceToWriteCursor =
        (safeWriteOffset + bufferBytes - writeCursor) % bufferBytes;
    if (distanceToWriteCursor > targetAheadBytes) {
      safeWriteOffset = writeCursor;
    }

    hr = secondaryBuffer->Lock(safeWriteOffset, blockBytes, &lockPtr1, &lockBytes1,
                               &lockPtr2, &lockBytes2, 0);
    if (FAILED(hr)) {
      if (hr == DSERR_BUFFERLOST) {
        secondaryBuffer->Restore();
        continue;
      }
      char buf[160];
      sprintf(buf, "SVMS: ERROR - DirectSound lock failed: 0x%08lX (%s)\n",
              (unsigned long)hr, DirectSoundErrorToString(hr));
      OutputDebugStringA(buf);
      break;
    }

    if (lockPtr1 && lockBytes1)
      memcpy(lockPtr1, dsBlockBuffer.data(), lockBytes1);
    if (lockPtr2 && lockBytes2)
      memcpy(lockPtr2, (BYTE *)dsBlockBuffer.data() + lockBytes1, lockBytes2);
    secondaryBuffer->Unlock(lockPtr1, lockBytes1, lockPtr2, lockBytes2);

    if (++loopCount <= 10) {
      char buf[160];
      sprintf(buf,
              "SVMS: Wrote DS block %d, max audio=%d play=%lu write=%lu next=%lu\n",
              loopCount, maxVal, (unsigned long)playCursor,
              (unsigned long)writeCursor, (unsigned long)safeWriteOffset);
      OutputDebugStringA(buf);
    }

    writeOffset = (safeWriteOffset + blockBytes) % bufferBytes;
  }

  OutputDebugStringA("SVMS: Cleaning up DirectSound output\n");
  secondaryBuffer->Stop();
  secondaryBuffer->Release();
  if (primaryBuffer)
    primaryBuffer->Release();
  directSound->Release();
  return true;
}
