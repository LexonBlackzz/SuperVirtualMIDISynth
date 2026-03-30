#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <windows.h>
#include "Compat.h"
#include <atomic>
#ifndef SVMS_LEGACY_XP
#include <thread>
#endif
#include <string>
#include <vector>

struct tagDSBUFFERDESC;
struct IDirectSound;
struct IDirectSoundBuffer;
class SystemWinmm;

class AudioOutput {
public:
  enum class AudioBackendId { AUTO, WAVEOUT, DSOUND, WASAPI_SHARED };

  static AudioOutput &Instance();

  bool Start();
  void Stop(bool waitForThread = true);
  void ForceStop(bool waitForThread = true);
  int GetRefCount();
  std::string GetResolvedBackendName();
  bool IsWasapiAsyncFeedActive();
  void RequestRuntimeConfigRefresh();

private:
  struct AudioSettings {
    int pollingRate = 0;
    float masterVolume = 1.0f;
    bool reverbEnabled = false;
    float reverbMix = 0.18f;
    float reverbFeedback = 0.72f;
    float reverbTone = 0.28f;
    float reverbWidth = 0.35f;
    float reverbBlur = 0.45f;
    bool limiterEnabled = true;
    float limiterThreshold = 0.98f;
    float limiterReleaseMs = 80.0f;
    bool wasapiAsyncFeed = true;
  };

  AudioOutput();
  ~AudioOutput();

  void AudioThreadFunc();
  int FillOutputBuffer(int16_t *destPtr, int numFrames, int sampleRate,
                       int loopCount,
                       LARGE_INTEGER &freq, LARGE_INTEGER &lastChunkTime,
                       int &configReloadCounter);
  void LoadAudioSettings();
  void ResetDynamicsState();
  void ResetReverbState();
  void EnsureReverbState(int sampleRate);
  void ApplyLoFiReverb(float *buffer, int numFrames, int sampleRate,
                       bool enabled, float mix, float feedback, float tone,
                       float width, float blur);
  void ApplyLimiter(float *buffer, int numFrames, int sampleRate, bool enabled,
                    float threshold, float releaseMs);
  bool RunBackend(AudioBackendId requestedBackend, SystemWinmm &sys,
                  int sampleRate, LARGE_INTEGER &freq,
                  LARGE_INTEGER &lastChunkTime);
  bool RunWaveOutBackend(SystemWinmm &sys, int sampleRate, LARGE_INTEGER &freq,
                         LARGE_INTEGER &lastChunkTime);
  bool RunDirectSoundBackend(int sampleRate, LARGE_INTEGER &freq,
                             LARGE_INTEGER &lastChunkTime);
  bool RunWasapiSharedBackend(int sampleRate, LARGE_INTEGER &freq,
                              LARGE_INTEGER &lastChunkTime);
  void SetResolvedBackendName(const char *backendName);
  bool RefreshSynthRuntimeSettingsFromConfig();
#ifdef SVMS_LEGACY_XP
  static DWORD WINAPI AudioThreadProc(LPVOID param);
#endif

#ifdef SVMS_LEGACY_XP
  HANDLE audioThread;
#else
  std::thread audioThread;
#endif
  std::atomic<bool> running;

  compat::Mutex mutex;
  compat::Mutex backendStateMutex;
  int refCount;
  std::vector<float> mixBuffer;
  std::vector<int16_t> dsBlockBuffer;
  std::vector<float> reverbBufferL;
  std::vector<float> reverbBufferR;
  int reverbWriteIndex;
  int reverbSampleRate;
  float reverbFilterL;
  float reverbFilterR;
  float reverbBlurL1;
  float reverbBlurR1;
  float reverbBlurL2;
  float reverbBlurR2;
  bool reverbStateActive;
  AudioSettings audioSettings;
  float limiterGain;
  std::string resolvedBackendName;
  bool synthRuntimeSettingsSeeded;
  float cachedVelocityCurve;
  float cachedVelocityFloor;
  int cachedVelocityIgnoreBelow;
  bool cachedAsyncNoteStarts;
  std::string cachedTimingMode;
  unsigned long long renderSampleCursor;
  std::atomic<bool> runtimeConfigRefreshRequested;
};

#endif
