#ifndef SVMS_AUDIO_OUTPUT_H
#define SVMS_AUDIO_OUTPUT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <atomic>
#include <cstring>
#include <algorithm>

namespace svms {

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool Initialize(uint32_t sampleRate, uint32_t bufferFrames);
    void Shutdown();
    bool Start();
    void Stop();

    uint32_t GetSampleRate() const;
    uint32_t GetBufferFrames() const;
    bool IsRunning() const;
    HRESULT GetLastError() const;

    using RenderCallback = void(*)(float* output, uint32_t numFrames, void* userData);
    void SetRenderCallback(RenderCallback cb, void* userData);

private:
    static DWORD WINAPI AudioThreadProc(LPVOID param);
    void AudioThreadLoop();

    IAudioClient* audioClient_;
    IAudioRenderClient* renderClient_;
    IMMDevice* device_;
    HANDLE threadHandle_;
    HANDLE stopEvent_;
    std::atomic<bool> running_;
    uint32_t sampleRate_;
    uint32_t bufferFrames_;
    RenderCallback renderCallback_;
    void* userData_;
    HRESULT lastHResult_;
    bool formatIsFloat_;
    uint16_t formatBitsPerSample_;
    uint16_t formatChannels_;
};

inline AudioOutput::AudioOutput()
    : audioClient_(nullptr), renderClient_(nullptr), device_(nullptr),
      threadHandle_(nullptr), stopEvent_(nullptr), running_(false),
      sampleRate_(0), bufferFrames_(0), renderCallback_(nullptr), userData_(nullptr),
      lastHResult_(S_OK), formatIsFloat_(true), formatBitsPerSample_(32), formatChannels_(2) {}

inline AudioOutput::~AudioOutput() {
    Shutdown();
}

inline bool AudioOutput::Initialize(uint32_t sampleRate, uint32_t bufferFrames) {
    sampleRate_ = sampleRate;
    bufferFrames_ = bufferFrames;
    lastHResult_ = S_OK;

    char dbg[256];

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: CoInitializeEx FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: MMDeviceEnumerator FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    enumerator->Release();
    if (FAILED(hr)) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: GetDefaultAudioEndpoint FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
    if (FAILED(hr)) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: Activate IAudioClient FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = audioClient_->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: GetMixFormat FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    sampleRate_ = pwfx->nSamplesPerSec;
    formatIsFloat_ = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                      (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->wBitsPerSample == 32));
    formatBitsPerSample_ = pwfx->wBitsPerSample;
    formatChannels_ = pwfx->nChannels;

    sprintf(dbg, "[SVMS] AudioOutput::Initialize: mix format tag=0x%04X rate=%u bits=%u ch=%u cbSize=%u\n",
            pwfx->wFormatTag, sampleRate_, pwfx->wBitsPerSample, pwfx->nChannels, pwfx->cbSize);
    OutputDebugStringA(dbg);

    REFERENCE_TIME bufferDuration = (REFERENCE_TIME)bufferFrames_ * 10000000ULL / sampleRate_;
    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                   bufferDuration, 0, pwfx, nullptr);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: IAudioClient::Initialize FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    uint32_t actualFrames = 0;
    hr = audioClient_->GetBufferSize(&actualFrames);
    if (FAILED(hr)) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: GetBufferSize FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }
    bufferFrames_ = actualFrames;

    hr = audioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
    if (FAILED(hr)) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: GetService FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    sprintf(dbg, "[SVMS] AudioOutput::Initialize: SUCCESS rate=%u frames=%u\n",
            sampleRate_, bufferFrames_);
    OutputDebugStringA(dbg);
    return true;
}

inline void AudioOutput::Shutdown() {
    Stop();
    if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
    if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}

inline bool AudioOutput::Start() {
    if (!audioClient_) return false;
    if (running_.load()) return true;

    stopEvent_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;

    running_.store(true);
    threadHandle_ = CreateThread(nullptr, 0, AudioThreadProc, this, 0, nullptr);
    if (!threadHandle_) {
        running_.store(false);
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        return false;
    }

    SetThreadPriority(threadHandle_, THREAD_PRIORITY_TIME_CRITICAL);
    OutputDebugStringA("[SVMS] AudioOutput::Start: thread created\n");
    return true;
}

inline void AudioOutput::Stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (stopEvent_) SetEvent(stopEvent_);
    if (threadHandle_) {
        WaitForSingleObject(threadHandle_, 2000);
        CloseHandle(threadHandle_);
        threadHandle_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (audioClient_) audioClient_->Stop();
}

inline uint32_t AudioOutput::GetSampleRate() const { return sampleRate_; }
inline uint32_t AudioOutput::GetBufferFrames() const { return bufferFrames_; }
inline bool AudioOutput::IsRunning() const { return running_.load(); }
inline HRESULT AudioOutput::GetLastError() const { return lastHResult_; }

inline void AudioOutput::SetRenderCallback(RenderCallback cb, void* userData) {
    renderCallback_ = cb;
    userData_ = userData;
}

inline DWORD WINAPI AudioOutput::AudioThreadProc(LPVOID param) {
    AudioOutput* self = static_cast<AudioOutput*>(param);
    self->AudioThreadLoop();
    return 0;
}

inline void AudioOutput::AudioThreadLoop() {
    char dbg[256];
    sprintf(dbg, "[SVMS] AudioThreadLoop: START bufferFrames=%u\n", bufferFrames_);
    OutputDebugStringA(dbg);

    audioClient_->Start();

    int loopCount = 0;
    while (running_.load()) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;

        UINT32 padding = 0;
        HRESULT hr = audioClient_->GetCurrentPadding(&padding);
        if (FAILED(hr)) continue;

        UINT32 availFrames = bufferFrames_ - padding;
        if (availFrames == 0) {
            Sleep(1);
            continue;
        }

        BYTE* destBuffer = nullptr;
        hr = renderClient_->GetBuffer(availFrames, &destBuffer);
        if (SUCCEEDED(hr)) {
            if (renderCallback_) {
                renderCallback_(reinterpret_cast<float*>(destBuffer), availFrames, userData_);
            } else {
                std::memset(destBuffer, 0, availFrames * formatChannels_ * formatBitsPerSample_ / 8);
            }
            renderClient_->ReleaseBuffer(availFrames, 0);
        }

        loopCount++;
    }

    audioClient_->Stop();
    OutputDebugStringA("[SVMS] AudioThreadLoop: STOP\n");
}

} // namespace svms

#endif
