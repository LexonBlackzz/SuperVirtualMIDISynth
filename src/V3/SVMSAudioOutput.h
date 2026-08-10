#ifndef SVMS_AUDIO_OUTPUT_H
#define SVMS_AUDIO_OUTPUT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <ksmedia.h>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <vector>
#include <xmmintrin.h>
#include <cstddef>

// WAVEFORMATEX is a packed wire structure. Including mmeapi.h before
// mmreg.h with some Windows SDKs defines it at the default packing (20 bytes),
// which shifts WAVEFORMATEXTENSIBLE::SubFormat by two bytes and makes a float
// endpoint look like integer PCM. Keep this guard beside the parsing code.
static_assert(sizeof(WAVEFORMATEX) == 18, "WAVEFORMATEX must use wire packing");
static_assert(offsetof(WAVEFORMATEXTENSIBLE, SubFormat) == 24,
              "WAVEFORMATEXTENSIBLE layout is invalid; include mmreg.h first");

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
    HANDLE audioEvent_;
    std::atomic<bool> running_;
    uint32_t sampleRate_;
    uint32_t bufferFrames_;
    RenderCallback renderCallback_;
    void* userData_;
    HRESULT lastHResult_;
    bool formatIsFloat_;
    uint16_t formatBitsPerSample_;
    uint16_t formatChannels_;
    std::vector<float> renderScratch_;
    bool comInitialized_;
};

inline AudioOutput::AudioOutput()
    : audioClient_(nullptr), renderClient_(nullptr), device_(nullptr),
      threadHandle_(nullptr), stopEvent_(nullptr), audioEvent_(nullptr), running_(false),
      sampleRate_(0), bufferFrames_(0), renderCallback_(nullptr), userData_(nullptr),
      lastHResult_(S_OK), formatIsFloat_(true), formatBitsPerSample_(32), formatChannels_(2),
      renderScratch_(), comInitialized_(false) {}

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
    comInitialized_ = SUCCEEDED(hr);

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
    formatIsFloat_ = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        formatIsFloat_ = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }
    formatBitsPerSample_ = pwfx->wBitsPerSample;
    formatChannels_ = pwfx->nChannels;

    sprintf(dbg, "[SVMS] AudioOutput::Initialize: mix format tag=0x%04X rate=%u bits=%u ch=%u cbSize=%u\n",
            pwfx->wFormatTag, sampleRate_, pwfx->wBitsPerSample, pwfx->nChannels, pwfx->cbSize);
    OutputDebugStringA(dbg);

    REFERENCE_TIME bufferDuration = (REFERENCE_TIME)bufferFrames_ * 10000000ULL / sampleRate_;
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent_) {
        lastHResult_ = HRESULT_FROM_WIN32(GetLastError());
        CoTaskMemFree(pwfx);
        return false;
    }

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                   bufferDuration, 0, pwfx, nullptr);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) {
        lastHResult_ = hr;
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: IAudioClient::Initialize FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    hr = audioClient_->SetEventHandle(audioEvent_);
    if (FAILED(hr)) {
        lastHResult_ = hr;
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
    renderScratch_.resize(static_cast<size_t>(bufferFrames_) * 2u);

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
    if (audioEvent_) { CloseHandle(audioEvent_); audioEvent_ = nullptr; }
    if (comInitialized_) { CoUninitialize(); comInitialized_ = false; }
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

    return true;
}

inline void AudioOutput::Stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (stopEvent_) SetEvent(stopEvent_);
    if (threadHandle_) {
        WaitForSingleObject(threadHandle_, INFINITE);
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
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    BYTE* initialBuffer = nullptr;
    if (SUCCEEDED(renderClient_->GetBuffer(bufferFrames_, &initialBuffer)))
        renderClient_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    audioClient_->Start();

    HANDLE waitHandles[2] = { stopEvent_, audioEvent_ };
    while (running_.load()) {
        const DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        if (wait != WAIT_OBJECT_0 + 1) continue;

        UINT32 padding = 0;
        HRESULT hr = audioClient_->GetCurrentPadding(&padding);
        if (FAILED(hr)) continue;

        if (padding >= bufferFrames_) continue;
        UINT32 availFrames = bufferFrames_ - padding;
        if (availFrames == 0) continue;

        BYTE* destBuffer = nullptr;
        hr = renderClient_->GetBuffer(availFrames, &destBuffer);
        if (SUCCEEDED(hr)) {
            const size_t destBytes = static_cast<size_t>(availFrames) * formatChannels_
                                   * formatBitsPerSample_ / 8u;
            if (!renderCallback_) {
                std::memset(destBuffer, 0, destBytes);
            } else if (formatIsFloat_ && formatBitsPerSample_ == 32 && formatChannels_ == 2) {
                renderCallback_(reinterpret_cast<float*>(destBuffer), availFrames, userData_);
            } else {
                renderCallback_(renderScratch_.data(), availFrames, userData_);

                auto clamp = [](float x) {
                    return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
                };
                for (UINT32 f = 0; f < availFrames; ++f) {
                    const float l = renderScratch_[f * 2u];
                    const float r = renderScratch_[f * 2u + 1u];
                    for (uint16_t ch = 0; ch < formatChannels_; ++ch) {
                        const float x = clamp(ch == 0 ? l : (ch == 1 ? r : (l + r) * 0.5f));
                        BYTE* p = destBuffer + (static_cast<size_t>(f) * formatChannels_ + ch)
                                             * formatBitsPerSample_ / 8u;
                        if (formatIsFloat_ && formatBitsPerSample_ == 32) {
                            std::memcpy(p, &x, sizeof(float));
                        } else if (formatBitsPerSample_ == 32) {
                            const int32_t sample = static_cast<int32_t>(x * 2147483647.0f);
                            std::memcpy(p, &sample, sizeof(sample));
                        } else if (formatBitsPerSample_ == 24) {
                            const int32_t sample = static_cast<int32_t>(x * 8388607.0f);
                            p[0] = static_cast<BYTE>(sample & 0xff);
                            p[1] = static_cast<BYTE>((sample >> 8) & 0xff);
                            p[2] = static_cast<BYTE>((sample >> 16) & 0xff);
                        } else if (formatBitsPerSample_ == 16) {
                            const int16_t sample = static_cast<int16_t>(x * 32767.0f);
                            std::memcpy(p, &sample, sizeof(sample));
                        } else if (formatBitsPerSample_ == 8) {
                            *p = static_cast<BYTE>((x + 1.0f) * 127.5f);
                        }
                    }
                }
            }
            renderClient_->ReleaseBuffer(availFrames, 0);
        }
    }

    audioClient_->Stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(comHr)) CoUninitialize();
}

} // namespace svms

#endif
