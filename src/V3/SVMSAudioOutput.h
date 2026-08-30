#ifndef SVMS_AUDIO_OUTPUT_H
#define SVMS_AUDIO_OUTPUT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <audiopolicy.h>
#include <avrt.h>
#include "SVMSThreadAffinity.h"
#include <ksmedia.h>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <new>
#include <string>
#include <vector>
#include <xmmintrin.h>
#include <cstddef>

#if defined(__MINGW32__)
// MinGW's ksmedia.h only declares (never defines) the KS subtype GUIDs, and
// some MinGW distributions (w64devkit) do not ship ksuser.lib to link them.
// Provide the constant inline so both toolchains resolve the same value.
static const GUID kKsSubTypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
#else
static const GUID& kKsSubTypeIeeeFloat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
#endif

// WAVEFORMATEX is a packed wire structure. Including mmeapi.h before
// mmreg.h with some Windows SDKs defines it at the default packing (20 bytes),
// which shifts WAVEFORMATEXTENSIBLE::SubFormat by two bytes and makes a float
// endpoint look like integer PCM. Keep this guard beside the parsing code.
static_assert(sizeof(WAVEFORMATEX) == 18, "WAVEFORMATEX must use wire packing");
static_assert(offsetof(WAVEFORMATEXTENSIBLE, SubFormat) == 24,
              "WAVEFORMATEXTENSIBLE layout is invalid; include mmreg.h first");

namespace svms {

class AudioDeviceNotification final : public IMMNotificationClient {
public:
    explicit AudioDeviceNotification(HANDLE signal)
        : references_(1u), signal_(signal) {}

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            references_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (remaining == 0u) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        Signal(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        Signal(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        Signal(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR) override {
        if (flow == eRender && role == eConsole) Signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,
                                                     const PROPERTYKEY) override {
        return S_OK;
    }

private:
    void Signal() const { if (signal_) SetEvent(signal_); }
    std::atomic<ULONG> references_;
    HANDLE signal_;
};

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool Initialize(uint32_t sampleRate, uint32_t bufferFrames,
                    const std::wstring& deviceName = {});
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
    bool OpenEndpoint(bool preserveEngineRate, bool allowDefaultFallback);
    void CloseEndpoint();
    bool StartEndpoint();
    void RenderAvailable(BYTE* destination, uint32_t frames);
    bool DefaultEndpointChanged() const;
    static bool IsDefaultDeviceName(const std::wstring& name);

    IAudioClient* audioClient_;
    IAudioRenderClient* renderClient_;
    IMMDevice* device_;
    HANDLE threadHandle_;
    HANDLE stopEvent_;
    HANDLE audioEvent_;
    HANDLE deviceChangeEvent_;
    IMMDeviceEnumerator* notificationEnumerator_;
    AudioDeviceNotification* notificationClient_;
    std::atomic<bool> running_;
    std::atomic<bool> streamActive_;
    uint32_t sampleRate_;
    uint32_t bufferFrames_;
    uint32_t callbackFrameLimit_;
    uint32_t requestedBufferFrames_;
    RenderCallback renderCallback_;
    void* userData_;
    std::atomic<HRESULT> lastHResult_;
    std::atomic<uint32_t> recoveryCount_;
    bool formatIsFloat_;
    uint16_t formatBitsPerSample_;
    uint16_t formatChannels_;
    std::vector<float> renderScratch_;
    std::wstring configuredDeviceName_;
    std::wstring activeDeviceId_;
    bool comInitialized_;
};

inline AudioOutput::AudioOutput()
    : audioClient_(nullptr), renderClient_(nullptr), device_(nullptr),
      threadHandle_(nullptr), stopEvent_(nullptr), audioEvent_(nullptr),
      deviceChangeEvent_(nullptr), notificationEnumerator_(nullptr),
      notificationClient_(nullptr), running_(false),
      streamActive_(false), sampleRate_(0), bufferFrames_(0),
      callbackFrameLimit_(0), requestedBufferFrames_(0), renderCallback_(nullptr),
      userData_(nullptr), lastHResult_(S_OK), recoveryCount_(0),
      formatIsFloat_(true), formatBitsPerSample_(32), formatChannels_(2),
      renderScratch_(), configuredDeviceName_(), activeDeviceId_(),
      comInitialized_(false) {}

inline AudioOutput::~AudioOutput() {
    Shutdown();
}

inline bool AudioOutput::Initialize(uint32_t sampleRate, uint32_t bufferFrames,
                                    const std::wstring& deviceName) {
    Shutdown();
    sampleRate_ = sampleRate;
    requestedBufferFrames_ = bufferFrames;
    configuredDeviceName_ = deviceName;
    lastHResult_.store(S_OK, std::memory_order_release);
    recoveryCount_.store(0u, std::memory_order_release);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        lastHResult_.store(hr, std::memory_order_release);
        char dbg[256];
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: CoInitializeEx FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }
    comInitialized_ = SUCCEEDED(hr);

    if (!OpenEndpoint(false, false)) return false;
    callbackFrameLimit_ = bufferFrames_;
    renderScratch_.resize(static_cast<size_t>(callbackFrameLimit_) * 2u);

    deviceChangeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (deviceChangeEvent_ &&
        SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(
                                       &notificationEnumerator_)))) {
        notificationClient_ =
            new (std::nothrow) AudioDeviceNotification(deviceChangeEvent_);
        if (!notificationClient_ ||
            FAILED(notificationEnumerator_->RegisterEndpointNotificationCallback(
                notificationClient_))) {
            if (notificationClient_) {
                notificationClient_->Release();
                notificationClient_ = nullptr;
            }
            notificationEnumerator_->Release();
            notificationEnumerator_ = nullptr;
        }
    }
    return true;
}

inline bool AudioOutput::IsDefaultDeviceName(const std::wstring& name) {
    return name.empty() || _wcsicmp(name.c_str(), L"default") == 0;
}

inline void AudioOutput::CloseEndpoint() {
    streamActive_.store(false, std::memory_order_release);
    if (audioClient_) audioClient_->Stop();
    if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
    if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (audioEvent_) { CloseHandle(audioEvent_); audioEvent_ = nullptr; }
    activeDeviceId_.clear();
}

inline bool AudioOutput::OpenEndpoint(bool preserveEngineRate,
                                      bool allowDefaultFallback) {
    CloseEndpoint();
    char dbg[256];

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: MMDeviceEnumerator FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    // Empty is retained for configurations created by older V3 builds.
    // New configurations write the explicit "default" sentinel so first-run
    // behavior is visible and editable instead of looking unconfigured.
    if (IsDefaultDeviceName(configuredDeviceName_)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    } else {
        IMMDeviceCollection* collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr) && collection) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT index = 0; index < count && !device_; ++index) {
                IMMDevice* candidate = nullptr;
                IPropertyStore* properties = nullptr;
                PROPVARIANT friendlyName;
                PropVariantInit(&friendlyName);
                if (SUCCEEDED(collection->Item(index, &candidate)) && candidate &&
                    SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &properties)) &&
                    properties &&
                    SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,
                                                   &friendlyName)) &&
                    friendlyName.vt == VT_LPWSTR && friendlyName.pwszVal &&
                    _wcsicmp(friendlyName.pwszVal,
                             configuredDeviceName_.c_str()) == 0) {
                    device_ = candidate;
                    candidate = nullptr;
                }
                PropVariantClear(&friendlyName);
                if (properties) properties->Release();
                if (candidate) candidate->Release();
            }
            collection->Release();
            if (!device_ && allowDefaultFallback)
                hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                         &device_);
            else if (!device_)
                hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
    }
    enumerator->Release();
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: render endpoint selection FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    LPWSTR endpointId = nullptr;
    if (SUCCEEDED(device_->GetId(&endpointId)) && endpointId) {
        activeDeviceId_ = endpointId;
        CoTaskMemFree(endpointId);
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: Activate IAudioClient FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = audioClient_->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: GetMixFormat FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    WAVEFORMATEX recoveryFormat{};
    WAVEFORMATEX* clientFormat = pwfx;
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (!preserveEngineRate) {
        sampleRate_ = pwfx->nSamplesPerSec;
    } else if (pwfx->nSamplesPerSec != sampleRate_) {
        // Keep the established synth/output-frame clock after moving to an
        // endpoint with a different mix rate. Shared WASAPI performs the rate
        // conversion, so MIDI timing and live voice pitch remain unchanged.
        recoveryFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        recoveryFormat.nChannels = 2;
        recoveryFormat.nSamplesPerSec = sampleRate_;
        recoveryFormat.wBitsPerSample = 32;
        recoveryFormat.nBlockAlign = 8;
        recoveryFormat.nAvgBytesPerSec = sampleRate_ * 8u;
        recoveryFormat.cbSize = 0;
        clientFormat = &recoveryFormat;
        streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                       AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }
    formatIsFloat_ = (clientFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (clientFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(clientFormat);
        formatIsFloat_ = IsEqualGUID(ext->SubFormat, kKsSubTypeIeeeFloat) != FALSE;
    }
    formatBitsPerSample_ = clientFormat->wBitsPerSample;
    formatChannels_ = clientFormat->nChannels;

    sprintf(dbg, "[SVMS] AudioOutput::Initialize: mix format tag=0x%04X rate=%u bits=%u ch=%u cbSize=%u\n",
            clientFormat->wFormatTag, sampleRate_, clientFormat->wBitsPerSample,
            clientFormat->nChannels, clientFormat->cbSize);
    OutputDebugStringA(dbg);

    REFERENCE_TIME bufferDuration =
        static_cast<REFERENCE_TIME>(requestedBufferFrames_) * 10000000ULL /
        sampleRate_;
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent_) {
        lastHResult_.store(HRESULT_FROM_WIN32(GetLastError()),
                           std::memory_order_release);
        CoTaskMemFree(pwfx);
        return false;
    }

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   streamFlags, bufferDuration, 0,
                                   clientFormat, nullptr);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: IAudioClient::Initialize FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    hr = audioClient_->SetEventHandle(audioEvent_);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        return false;
    }

    uint32_t actualFrames = 0;
    hr = audioClient_->GetBufferSize(&actualFrames);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: GetBufferSize FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }
    bufferFrames_ = actualFrames;
    hr = audioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        sprintf(dbg, "[SVMS] AudioOutput::Initialize: GetService FAILED hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    sprintf(dbg, "[SVMS] AudioOutput::Initialize: SUCCESS rate=%u frames=%u\n",
            sampleRate_, bufferFrames_);
    OutputDebugStringA(dbg);
    lastHResult_.store(S_OK, std::memory_order_release);
    return true;
}

inline void AudioOutput::Shutdown() {
    Stop();
    if (notificationEnumerator_ && notificationClient_)
        notificationEnumerator_->UnregisterEndpointNotificationCallback(
            notificationClient_);
    if (notificationClient_) {
        notificationClient_->Release();
        notificationClient_ = nullptr;
    }
    if (notificationEnumerator_) {
        notificationEnumerator_->Release();
        notificationEnumerator_ = nullptr;
    }
    if (deviceChangeEvent_) {
        CloseHandle(deviceChangeEvent_);
        deviceChangeEvent_ = nullptr;
    }
    CloseEndpoint();
    callbackFrameLimit_ = 0u;
    bufferFrames_ = 0u;
    renderScratch_.clear();
    if (comInitialized_) { CoUninitialize(); comInitialized_ = false; }
}

inline bool AudioOutput::Start() {
    if (!audioClient_) return false;
    if (threadHandle_) return true;

    stopEvent_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;

    running_.store(true);
    threadHandle_ = CreateThread(nullptr, 0, AudioThreadProc, this, 0, nullptr);
    svms::PinThreadToPerformanceCores(threadHandle_);
    if (!threadHandle_) {
        running_.store(false);
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        return false;
    }

    return true;
}

inline void AudioOutput::Stop() {
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
    streamActive_.store(false, std::memory_order_release);
}

inline uint32_t AudioOutput::GetSampleRate() const { return sampleRate_; }
inline uint32_t AudioOutput::GetBufferFrames() const { return bufferFrames_; }
inline bool AudioOutput::IsRunning() const {
    // This reports ownership/liveness of the render thread, not whether a
    // device is momentarily active. Callers use it to decide whether direct
    // synth-state mutation is safe while endpoint recovery is in progress.
    return running_.load(std::memory_order_acquire);
}
inline HRESULT AudioOutput::GetLastError() const {
    return lastHResult_.load(std::memory_order_acquire);
}

inline void AudioOutput::SetRenderCallback(RenderCallback cb, void* userData) {
    renderCallback_ = cb;
    userData_ = userData;
}

inline DWORD WINAPI AudioOutput::AudioThreadProc(LPVOID param) {
    AudioOutput* self = static_cast<AudioOutput*>(param);
    self->AudioThreadLoop();
    return 0;
}

inline bool AudioOutput::StartEndpoint() {
    if (!audioClient_ || !renderClient_ || !audioEvent_) return false;

    BYTE* initialBuffer = nullptr;
    HRESULT hr = renderClient_->GetBuffer(bufferFrames_, &initialBuffer);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        return false;
    }
    hr = renderClient_->ReleaseBuffer(bufferFrames_,
                                      AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        return false;
    }
    hr = audioClient_->Start();
    if (FAILED(hr)) {
        lastHResult_.store(hr, std::memory_order_release);
        return false;
    }
    lastHResult_.store(S_OK, std::memory_order_release);
    streamActive_.store(true, std::memory_order_release);
    return true;
}

inline void AudioOutput::RenderAvailable(BYTE* destination, uint32_t frames) {
    const size_t bytesPerFrame = static_cast<size_t>(formatChannels_) *
                                 formatBitsPerSample_ / 8u;
    if (!renderCallback_) {
        std::memset(destination, 0, static_cast<size_t>(frames) * bytesPerFrame);
        return;
    }

    const uint32_t limit = callbackFrameLimit_ != 0u
        ? callbackFrameLimit_ : frames;
    for (uint32_t base = 0u; base < frames;) {
        const uint32_t count = (std::min)(limit, frames - base);
        if (formatIsFloat_ && formatBitsPerSample_ == 32 &&
            formatChannels_ == 2) {
            renderCallback_(reinterpret_cast<float*>(destination) +
                                static_cast<size_t>(base) * 2u,
                            count, userData_);
        } else {
            renderCallback_(renderScratch_.data(), count, userData_);
            auto clamp = [](float x) {
                return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
            };
            for (uint32_t frame = 0u; frame < count; ++frame) {
                const float left = renderScratch_[frame * 2u];
                const float right = renderScratch_[frame * 2u + 1u];
                for (uint16_t channel = 0u; channel < formatChannels_; ++channel) {
                    const float sample = clamp(channel == 0u ? left :
                        (channel == 1u ? right : (left + right) * 0.5f));
                    BYTE* output = destination +
                        (static_cast<size_t>(base + frame) * formatChannels_ +
                         channel) * formatBitsPerSample_ / 8u;
                    if (formatIsFloat_ && formatBitsPerSample_ == 32) {
                        std::memcpy(output, &sample, sizeof(sample));
                    } else if (formatBitsPerSample_ == 32) {
                        const int32_t value =
                            static_cast<int32_t>(sample * 2147483647.0f);
                        std::memcpy(output, &value, sizeof(value));
                    } else if (formatBitsPerSample_ == 24) {
                        const int32_t value =
                            static_cast<int32_t>(sample * 8388607.0f);
                        output[0] = static_cast<BYTE>(value & 0xff);
                        output[1] = static_cast<BYTE>((value >> 8) & 0xff);
                        output[2] = static_cast<BYTE>((value >> 16) & 0xff);
                    } else if (formatBitsPerSample_ == 16) {
                        const int16_t value =
                            static_cast<int16_t>(sample * 32767.0f);
                        std::memcpy(output, &value, sizeof(value));
                    } else if (formatBitsPerSample_ == 8) {
                        *output = static_cast<BYTE>((sample + 1.0f) * 127.5f);
                    }
                }
            }
        }
        base += count;
    }
}

inline bool AudioOutput::DefaultEndpointChanged() const {
    if (!IsDefaultDeviceName(configuredDeviceName_) || activeDeviceId_.empty())
        return false;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* current = nullptr;
    LPWSTR endpointId = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr))
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current);
    if (SUCCEEDED(hr)) hr = current->GetId(&endpointId);
    const bool changed = SUCCEEDED(hr) && endpointId &&
                         _wcsicmp(endpointId, activeDeviceId_.c_str()) != 0;
    if (endpointId) CoTaskMemFree(endpointId);
    if (current) current->Release();
    if (enumerator) enumerator->Release();
    return changed;
}

inline void AudioOutput::AudioThreadLoop() {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    bool recover = !StartEndpoint();
    DWORD retryDelayMs = 100u;
    while (running_.load()) {
        if (recover) {
            streamActive_.store(false, std::memory_order_release);
            CloseEndpoint();
            HANDLE retryHandles[2] = { stopEvent_, deviceChangeEvent_ };
            const DWORD retryHandleCount = deviceChangeEvent_ ? 2u : 1u;
            const DWORD retryWait = WaitForMultipleObjects(
                retryHandleCount, retryHandles, FALSE, retryDelayMs);
            if (retryWait == WAIT_OBJECT_0 || retryWait == WAIT_FAILED) break;
            if (OpenEndpoint(true, true) && StartEndpoint()) {
                recoveryCount_.fetch_add(1u, std::memory_order_relaxed);
                retryDelayMs = 100u;
                recover = false;
                continue;
            }
            retryDelayMs = (std::min<DWORD>)(retryDelayMs * 2u, 5000u);
            continue;
        }

        HANDLE waitHandles[3] = { stopEvent_, audioEvent_, deviceChangeEvent_ };
        const DWORD waitHandleCount = deviceChangeEvent_ ? 3u : 2u;
        const DWORD wait = WaitForMultipleObjects(waitHandleCount, waitHandles,
                                                  FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        if (deviceChangeEvent_ && wait == WAIT_OBJECT_0 + 2u) {
            if (!IsDefaultDeviceName(configuredDeviceName_) ||
                DefaultEndpointChanged()) {
                lastHResult_.store(AUDCLNT_E_DEVICE_INVALIDATED,
                                   std::memory_order_release);
                recover = true;
            }
            continue;
        }
        if (wait != WAIT_OBJECT_0 + 1u) continue;

        UINT32 padding = 0;
        HRESULT hr = audioClient_->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            lastHResult_.store(hr, std::memory_order_release);
            recover = true;
            continue;
        }
        if (padding >= bufferFrames_) continue;
        UINT32 availFrames = bufferFrames_ - padding;
        if (availFrames == 0) continue;

        BYTE* destBuffer = nullptr;
        hr = renderClient_->GetBuffer(availFrames, &destBuffer);
        if (FAILED(hr)) {
            lastHResult_.store(hr, std::memory_order_release);
            recover = true;
            continue;
        }
        RenderAvailable(destBuffer, availFrames);
        hr = renderClient_->ReleaseBuffer(availFrames, 0);
        if (FAILED(hr)) {
            lastHResult_.store(hr, std::memory_order_release);
            recover = true;
        }
    }

    streamActive_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    if (audioClient_) audioClient_->Stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(comHr)) CoUninitialize();
}

} // namespace svms

#endif
