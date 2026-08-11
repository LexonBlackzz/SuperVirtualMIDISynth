#ifndef SVMS_AUDIO_OUTPUT_DIRECTSOUND_H
#define SVMS_AUDIO_OUTPUT_DIRECTSOUND_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include <xmmintrin.h>

namespace svms {

inline void TraceXPAudioFailure(const char* stage, unsigned long code) {
    char message[192] = {};
    std::snprintf(message, sizeof(message),
                  "[SVMS XP] %s FAILED: 0x%08lX\r\n", stage, code);
    OutputDebugStringA(message);
}

inline void TraceXPModulePath(const char* label, HMODULE module) {
    wchar_t widePath[MAX_PATH] = {};
    char path[MAX_PATH * 3] = {};
    if (module) GetModuleFileNameW(module, widePath, MAX_PATH);
    if (widePath[0]) {
        WideCharToMultiByte(CP_ACP, 0, widePath, -1, path,
                            static_cast<int>(sizeof(path)), nullptr, nullptr);
    }
    char message[1200] = {};
    std::snprintf(message, sizeof(message),
                  "[SVMS XP] %s handle=%p path='%s'\r\n", label,
                  static_cast<void*>(module), path[0] ? path : "<none>");
    OutputDebugStringA(message);
}

struct XPDirectSoundProbe {
    GUID firstPhysicalGuid;
    bool hasPhysicalGuid;
};

inline BOOL CALLBACK TraceXPDirectSoundDevice(LPGUID guid, LPCSTR description,
                                               LPCSTR module, LPVOID context) {
    XPDirectSoundProbe* probe = static_cast<XPDirectSoundProbe*>(context);
    if (guid && probe && !probe->hasPhysicalGuid) {
        probe->firstPhysicalGuid = *guid;
        probe->hasPhysicalGuid = true;
    }
    char guidText[96] = "default";
    if (guid) {
        std::snprintf(guidText, sizeof(guidText),
                      "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                      static_cast<unsigned long>(guid->Data1), guid->Data2,
                      guid->Data3, guid->Data4[0], guid->Data4[1],
                      guid->Data4[2], guid->Data4[3], guid->Data4[4],
                      guid->Data4[5], guid->Data4[6], guid->Data4[7]);
    }
    char message[768] = {};
    std::snprintf(message, sizeof(message),
                  "[SVMS XP] DirectSoundEnumerate device guid=%s description='%s' module='%s'\r\n",
                  guidText, description ? description : "<null>",
                  module ? module : "<null>");
    OutputDebugStringA(message);
    return TRUE;
}

// XP output keeps the same floating-point render callback and scheduler used by
// the modern build. DirectSound is attempted first; the real system winmm.dll
// supplies an event-driven waveOut fallback for machines where DirectSound
// cannot initialize (a behavior inherited from V1's XP path).
class AudioOutput {
public:
    using RenderCallback = void(*)(float* output, uint32_t numFrames,
                                   void* userData);

    AudioOutput();
    ~AudioOutput();

    bool Initialize(uint32_t sampleRate, uint32_t bufferFrames);
    void Shutdown();
    bool Start();
    void Stop();

    uint32_t GetSampleRate() const { return sampleRate_; }
    uint32_t GetBufferFrames() const { return segmentFrames_; }
    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    bool IsWaveOutFallback() const { return backend_ == Backend::WaveOut; }
    HRESULT GetLastError() const { return lastHResult_; }
    void SetRenderCallback(RenderCallback callback, void* userData) {
        renderCallback_ = callback;
        userData_ = userData;
    }

private:
    static constexpr uint32_t kSegmentCount = 4;

    enum class Backend : uint8_t { None, DirectSound, WaveOut };
    using WaveOutGetNumDevsProc = UINT (WINAPI*)();
    using WaveOutOpenProc = MMRESULT (WINAPI*)(LPHWAVEOUT, UINT,
                                               LPCWAVEFORMATEX, DWORD_PTR,
                                               DWORD_PTR, DWORD);
    using WaveOutCloseProc = MMRESULT (WINAPI*)(HWAVEOUT);
    using WaveOutPrepareProc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    using WaveOutUnprepareProc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    using WaveOutWriteProc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    using WaveOutResetProc = MMRESULT (WINAPI*)(HWAVEOUT);

    static DWORD WINAPI AudioThreadProc(LPVOID parameter);
    void AudioThreadLoop();
    void DirectSoundThreadLoop();
    void WaveOutThreadLoop();
    bool InitializeDirectSound();
    bool InitializeWaveOut();
    bool FillDirectSoundSegment(uint32_t segment);
    bool FillWaveOutSegment(uint32_t segment);
    bool ClearDirectSoundBuffer();
    void ReleaseDirectSound();
    void ReleaseWaveOut();
    void RenderAndConvert(int16_t* destination);
    static HRESULT MmResultToHResult(MMRESULT result);

    Backend backend_;
    IDirectSound* directSound_;
    IDirectSoundBuffer* streamBuffer_;
    IDirectSoundNotify* notify_;
    HANDLE threadHandle_;
    HANDLE stopEvent_;
    HANDLE segmentEvents_[kSegmentCount];

    HMODULE systemWinmm_;
    HWAVEOUT waveOut_;
    HANDLE waveEvent_;
    WAVEHDR waveHeaders_[kSegmentCount];
    std::vector<int16_t> waveBuffers_[kSegmentCount];
    uint32_t waveRefillCursor_;
    WaveOutGetNumDevsProc realWaveOutGetNumDevs_;
    WaveOutOpenProc realWaveOutOpen_;
    WaveOutCloseProc realWaveOutClose_;
    WaveOutPrepareProc realWaveOutPrepare_;
    WaveOutUnprepareProc realWaveOutUnprepare_;
    WaveOutWriteProc realWaveOutWrite_;
    WaveOutResetProc realWaveOutReset_;

    std::atomic<bool> running_;
    uint32_t sampleRate_;
    uint32_t segmentFrames_;
    uint32_t segmentBytes_;
    RenderCallback renderCallback_;
    void* userData_;
    HRESULT lastHResult_;
    std::vector<float> renderScratch_;
};

inline AudioOutput::AudioOutput()
    : backend_(Backend::None), directSound_(nullptr), streamBuffer_(nullptr),
      notify_(nullptr), threadHandle_(nullptr), stopEvent_(nullptr),
      segmentEvents_{nullptr, nullptr, nullptr, nullptr},
      systemWinmm_(nullptr), waveOut_(nullptr),
      waveEvent_(nullptr),
      waveHeaders_{}, waveBuffers_{}, waveRefillCursor_(0),
      realWaveOutGetNumDevs_(nullptr), realWaveOutOpen_(nullptr),
      realWaveOutClose_(nullptr), realWaveOutPrepare_(nullptr),
      realWaveOutUnprepare_(nullptr), realWaveOutWrite_(nullptr),
      realWaveOutReset_(nullptr), running_(false), sampleRate_(0),
      segmentFrames_(0), segmentBytes_(0), renderCallback_(nullptr),
      userData_(nullptr), lastHResult_(S_OK), renderScratch_() {}

inline AudioOutput::~AudioOutput() {
    Shutdown();
}

inline HRESULT AudioOutput::MmResultToHResult(MMRESULT result) {
    return result == MMSYSERR_NOERROR
               ? S_OK
               : HRESULT_FROM_WIN32(static_cast<DWORD>(result));
}

inline bool AudioOutput::Initialize(uint32_t sampleRate,
                                    uint32_t bufferFrames) {
    Shutdown();
    sampleRate_ = sampleRate;
    // buffer_frames is one complete render callback. Four backend segments
    // provide underrun tolerance without changing scheduling granularity.
    segmentFrames_ = std::max<uint32_t>(16u, bufferFrames);
    segmentBytes_ = segmentFrames_ * 2u * sizeof(int16_t);
    renderScratch_.resize(static_cast<size_t>(segmentFrames_) * 2u);

    wchar_t forceValue[2] = {};
    const bool forceWaveOut =
        GetEnvironmentVariableW(L"SVMS_XP_FORCE_WAVEOUT", forceValue, 2) > 0 &&
        forceValue[0] != L'0';
    if (!forceWaveOut && InitializeDirectSound()) return true;

    const HRESULT directSoundError = lastHResult_;
    ReleaseDirectSound();
    OutputDebugStringA(forceWaveOut
        ? "[SVMS XP] waveOut forced by SVMS_XP_FORCE_WAVEOUT\r\n"
        : "[SVMS XP] DirectSound unavailable; trying system waveOut\r\n");
    if (InitializeWaveOut()) {
        OutputDebugStringA("[SVMS XP] system waveOut initialized\r\n");
        return true;
    }
    if (SUCCEEDED(lastHResult_)) lastHResult_ = directSoundError;
    OutputDebugStringA("[SVMS XP] system waveOut initialization FAILED\r\n");
    return false;
}

inline bool AudioOutput::InitializeDirectSound() {
    // Match V1's baseline interface. Some XP-era drivers expose functional
    // DirectSound but reject DirectSoundCreate8/IDirectSoundBuffer8.
    TraceXPModulePath("loaded winmm.dll", GetModuleHandleW(L"winmm.dll"));
    TraceXPModulePath("loaded dsound.dll", GetModuleHandleW(L"dsound.dll"));
    XPDirectSoundProbe probe{};
    const HRESULT enumerateResult =
        DirectSoundEnumerateA(&TraceXPDirectSoundDevice, &probe);
    {
        char message[160] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] DirectSoundEnumerateA result: 0x%08lX\r\n",
                      static_cast<unsigned long>(enumerateResult));
        OutputDebugStringA(message);
    }
    lastHResult_ = DirectSoundCreate(nullptr, &directSound_, nullptr);
    if (FAILED(lastHResult_) && probe.hasPhysicalGuid) {
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] default DirectSoundCreate failed 0x%08lX; retrying enumerated physical device\r\n",
                      static_cast<unsigned long>(lastHResult_));
        OutputDebugStringA(message);
        lastHResult_ = DirectSoundCreate(&probe.firstPhysicalGuid,
                                         &directSound_, nullptr);
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] physical-device DirectSoundCreate result: 0x%08lX\r\n",
                      static_cast<unsigned long>(lastHResult_));
        OutputDebugStringA(message);
    }
    if (FAILED(lastHResult_)) {
        TraceXPAudioFailure("DirectSoundCreate",
                            static_cast<unsigned long>(lastHResult_));
        return false;
    }

    lastHResult_ = directSound_->SetCooperativeLevel(GetDesktopWindow(),
                                                     DSSCL_NORMAL);
    if (FAILED(lastHResult_)) {
        TraceXPAudioFailure("IDirectSound::SetCooperativeLevel",
                            static_cast<unsigned long>(lastHResult_));
        return false;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = sampleRate_;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels *
                                           format.wBitsPerSample / 8u);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    DSBUFFERDESC description{};
    description.dwSize = sizeof(description);
    description.dwFlags = DSBCAPS_CTRLPOSITIONNOTIFY |
                          DSBCAPS_GETCURRENTPOSITION2 |
                          DSBCAPS_GLOBALFOCUS;
    description.dwBufferBytes = segmentBytes_ * kSegmentCount;
    description.lpwfxFormat = &format;
    lastHResult_ = directSound_->CreateSoundBuffer(&description,
                                                   &streamBuffer_, nullptr);
    if (FAILED(lastHResult_)) {
        TraceXPAudioFailure("notification DirectSound buffer; retrying polling",
                            static_cast<unsigned long>(lastHResult_));
        description.dwFlags &= ~DSBCAPS_CTRLPOSITIONNOTIFY;
        lastHResult_ = directSound_->CreateSoundBuffer(&description,
                                                       &streamBuffer_, nullptr);
        if (FAILED(lastHResult_)) {
            TraceXPAudioFailure("IDirectSound::CreateSoundBuffer",
                                static_cast<unsigned long>(lastHResult_));
            return false;
        }
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        lastHResult_ = HRESULT_FROM_WIN32(GetLastError());
        TraceXPAudioFailure("DirectSound stop event",
                            static_cast<unsigned long>(lastHResult_));
        return false;
    }

    // Notifications are preferred, but old XP WDM drivers sometimes reject
    // IDirectSoundNotify. In that case the same four-segment renderer is fed
    // by a lightweight play-cursor loop, as V1 did.
    HRESULT notifyResult = streamBuffer_->QueryInterface(
        IID_IDirectSoundNotify, reinterpret_cast<void**>(&notify_));
    if (SUCCEEDED(notifyResult)) {
        DSBPOSITIONNOTIFY positions[kSegmentCount]{};
        for (uint32_t i = 0; i < kSegmentCount; ++i) {
            segmentEvents_[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!segmentEvents_[i]) {
                notifyResult = HRESULT_FROM_WIN32(GetLastError());
                break;
            }
            positions[i].dwOffset = (i + 1u) * segmentBytes_ - 1u;
            positions[i].hEventNotify = segmentEvents_[i];
        }
        if (SUCCEEDED(notifyResult))
            notifyResult = notify_->SetNotificationPositions(kSegmentCount,
                                                              positions);
    }
    if (FAILED(notifyResult)) {
        TraceXPAudioFailure("DirectSound notifications; using cursor polling",
                            static_cast<unsigned long>(notifyResult));
        if (notify_) { notify_->Release(); notify_ = nullptr; }
        for (HANDLE& eventHandle : segmentEvents_) {
            if (eventHandle) { CloseHandle(eventHandle); eventHandle = nullptr; }
        }
    }
    if (!ClearDirectSoundBuffer()) return false;
    backend_ = Backend::DirectSound;
    lastHResult_ = S_OK;
    return true;
}

inline bool AudioOutput::InitializeWaveOut() {
    wchar_t systemPath[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemPath, MAX_PATH);
    static const wchar_t suffix[] = L"\\winmm.dll";
    if (!length || length + (sizeof(suffix) / sizeof(suffix[0])) > MAX_PATH) {
        lastHResult_ = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        TraceXPAudioFailure("GetSystemDirectory for waveOut",
                            static_cast<unsigned long>(lastHResult_));
        return false;
    }
    std::wcscat(systemPath, suffix);
    OutputDebugStringA(
        "[SVMS XP] loading system WinMM by absolute path for waveOut\r\n");
    systemWinmm_ = LoadLibraryW(systemPath);
    if (!systemWinmm_) {
        lastHResult_ = HRESULT_FROM_WIN32(GetLastError());
        TraceXPAudioFailure("loading absolute system WinMM",
                            static_cast<unsigned long>(lastHResult_));
        return false;
    }
    TraceXPModulePath("waveOut absolute WinMM result", systemWinmm_);

    realWaveOutGetNumDevs_ = reinterpret_cast<WaveOutGetNumDevsProc>(
        GetProcAddress(systemWinmm_, "waveOutGetNumDevs"));
    realWaveOutOpen_ = reinterpret_cast<WaveOutOpenProc>(
        GetProcAddress(systemWinmm_, "waveOutOpen"));
    realWaveOutClose_ = reinterpret_cast<WaveOutCloseProc>(
        GetProcAddress(systemWinmm_, "waveOutClose"));
    realWaveOutPrepare_ = reinterpret_cast<WaveOutPrepareProc>(
        GetProcAddress(systemWinmm_, "waveOutPrepareHeader"));
    realWaveOutUnprepare_ = reinterpret_cast<WaveOutUnprepareProc>(
        GetProcAddress(systemWinmm_, "waveOutUnprepareHeader"));
    realWaveOutWrite_ = reinterpret_cast<WaveOutWriteProc>(
        GetProcAddress(systemWinmm_, "waveOutWrite"));
    realWaveOutReset_ = reinterpret_cast<WaveOutResetProc>(
        GetProcAddress(systemWinmm_, "waveOutReset"));
    if (!realWaveOutGetNumDevs_ || !realWaveOutOpen_ || !realWaveOutClose_ || !realWaveOutPrepare_ ||
        !realWaveOutUnprepare_ || !realWaveOutWrite_ || !realWaveOutReset_) {
        lastHResult_ = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        TraceXPAudioFailure("GetProcAddress(system waveOut exports)",
                            static_cast<unsigned long>(lastHResult_));
        return false;
    }
    {
        char message[384] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] waveOut exports getNum=%p open=%p close=%p prepare=%p write=%p reset=%p\r\n",
                      reinterpret_cast<void*>(realWaveOutGetNumDevs_),
                      reinterpret_cast<void*>(realWaveOutOpen_),
                      reinterpret_cast<void*>(realWaveOutClose_),
                      reinterpret_cast<void*>(realWaveOutPrepare_),
                      reinterpret_cast<void*>(realWaveOutWrite_),
                      reinterpret_cast<void*>(realWaveOutReset_));
        OutputDebugStringA(message);
    }

    waveEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!waveEvent_) {
        lastHResult_ = HRESULT_FROM_WIN32(GetLastError());
        TraceXPAudioFailure("waveOut completion event",
                            static_cast<unsigned long>(lastHResult_));
        return false;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        lastHResult_ = HRESULT_FROM_WIN32(GetLastError());
        TraceXPAudioFailure("waveOut stop event",
                            static_cast<unsigned long>(lastHResult_));
        return false;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = sampleRate_;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels *
                                           format.wBitsPerSample / 8u);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    const UINT deviceCount = realWaveOutGetNumDevs_();
    {
        char message[128] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] system waveOut devices: %u\r\n",
                      static_cast<unsigned>(deviceCount));
        OutputDebugStringA(message);
    }

    MMRESULT result = realWaveOutOpen_(&waveOut_, WAVE_MAPPER, &format,
                                      reinterpret_cast<DWORD_PTR>(waveEvent_),
                                      0, CALLBACK_EVENT);
    if (result == MMSYSERR_BADDEVICEID) {
        // Some XP mapper configurations reject WAVE_MAPPER even though one or
        // more physical waveOut devices are present. Try each reported device
        // explicitly, preserving the configured format and event callback.
        for (UINT device = 0; device < deviceCount; ++device) {
            result = realWaveOutOpen_(&waveOut_, device, &format,
                                      reinterpret_cast<DWORD_PTR>(waveEvent_),
                                      0, CALLBACK_EVENT);
            char message[144] = {};
            std::snprintf(message, sizeof(message),
                          "[SVMS XP] system waveOutOpen device %u result: 0x%08lX\r\n",
                          static_cast<unsigned>(device),
                          static_cast<unsigned long>(result));
            OutputDebugStringA(message);
            if (result == MMSYSERR_NOERROR) break;
        }
    }
    if (result != MMSYSERR_NOERROR) {
        lastHResult_ = MmResultToHResult(result);
        TraceXPAudioFailure("system waveOutOpen MMRESULT",
                            static_cast<unsigned long>(result));
        return false;
    }

    for (uint32_t i = 0; i < kSegmentCount; ++i) {
        waveBuffers_[i].resize(static_cast<size_t>(segmentFrames_) * 2u);
        std::memset(&waveHeaders_[i], 0, sizeof(WAVEHDR));
        waveHeaders_[i].lpData =
            reinterpret_cast<LPSTR>(waveBuffers_[i].data());
        waveHeaders_[i].dwBufferLength = segmentBytes_;
        result = realWaveOutPrepare_(waveOut_, &waveHeaders_[i],
                                     sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            lastHResult_ = MmResultToHResult(result);
            TraceXPAudioFailure("system waveOutPrepareHeader MMRESULT",
                                static_cast<unsigned long>(result));
            return false;
        }
    }
    backend_ = Backend::WaveOut;
    waveRefillCursor_ = 0;
    lastHResult_ = S_OK;
    return true;
}

inline bool AudioOutput::ClearDirectSoundBuffer() {
    if (!streamBuffer_) return false;
    void* first = nullptr;
    void* second = nullptr;
    DWORD firstBytes = 0;
    DWORD secondBytes = 0;
    HRESULT result = streamBuffer_->Lock(0, segmentBytes_ * kSegmentCount,
                                         &first, &firstBytes, &second,
                                         &secondBytes, DSBLOCK_ENTIREBUFFER);
    if (result == DSERR_BUFFERLOST) {
        streamBuffer_->Restore();
        result = streamBuffer_->Lock(0, segmentBytes_ * kSegmentCount,
                                     &first, &firstBytes, &second,
                                     &secondBytes, DSBLOCK_ENTIREBUFFER);
    }
    if (FAILED(result)) {
        lastHResult_ = result;
        return false;
    }
    std::memset(first, 0, firstBytes);
    if (second) std::memset(second, 0, secondBytes);
    streamBuffer_->Unlock(first, firstBytes, second, secondBytes);
    return true;
}

inline bool AudioOutput::Start() {
    if (backend_ == Backend::None || !stopEvent_) return false;
    if (running_.exchange(true, std::memory_order_acq_rel)) return true;
    ResetEvent(stopEvent_);

    if (backend_ == Backend::DirectSound) {
        ClearDirectSoundBuffer();
        streamBuffer_->SetCurrentPosition(0);
        lastHResult_ = streamBuffer_->Play(0, 0, DSBPLAY_LOOPING);
        if (FAILED(lastHResult_)) {
            running_.store(false, std::memory_order_release);
            return false;
        }
    } else {
        ResetEvent(waveEvent_);
    }

    threadHandle_ = CreateThread(nullptr, 0, AudioThreadProc, this, 0, nullptr);
    if (!threadHandle_) {
        lastHResult_ = HRESULT_FROM_WIN32(GetLastError());
        if (streamBuffer_) streamBuffer_->Stop();
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

inline void AudioOutput::Stop() {
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    if (wasRunning && stopEvent_) SetEvent(stopEvent_);
    if (threadHandle_) {
        if (stopEvent_) SetEvent(stopEvent_);
        WaitForSingleObject(threadHandle_, INFINITE);
        CloseHandle(threadHandle_);
        threadHandle_ = nullptr;
    }
    if (streamBuffer_) streamBuffer_->Stop();
    if (waveOut_ && realWaveOutReset_) realWaveOutReset_(waveOut_);
}

inline void AudioOutput::ReleaseDirectSound() {
    if (notify_) { notify_->Release(); notify_ = nullptr; }
    if (streamBuffer_) { streamBuffer_->Release(); streamBuffer_ = nullptr; }
    if (directSound_) { directSound_->Release(); directSound_ = nullptr; }
    for (HANDLE& eventHandle : segmentEvents_) {
        if (eventHandle) { CloseHandle(eventHandle); eventHandle = nullptr; }
    }
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    if (backend_ == Backend::DirectSound) backend_ = Backend::None;
}

inline void AudioOutput::ReleaseWaveOut() {
    if (waveOut_ && realWaveOutReset_) realWaveOutReset_(waveOut_);
    if (waveOut_ && realWaveOutUnprepare_) {
        for (uint32_t i = 0; i < kSegmentCount; ++i) {
            if (waveHeaders_[i].dwFlags & WHDR_PREPARED)
                realWaveOutUnprepare_(waveOut_, &waveHeaders_[i],
                                      sizeof(WAVEHDR));
        }
    }
    if (waveOut_ && realWaveOutClose_) realWaveOutClose_(waveOut_);
    waveOut_ = nullptr;
    if (waveEvent_) { CloseHandle(waveEvent_); waveEvent_ = nullptr; }
    for (uint32_t i = 0; i < kSegmentCount; ++i) {
        std::memset(&waveHeaders_[i], 0, sizeof(WAVEHDR));
        waveBuffers_[i].clear();
    }
    realWaveOutGetNumDevs_ = nullptr;
    realWaveOutOpen_ = nullptr;
    realWaveOutClose_ = nullptr;
    realWaveOutPrepare_ = nullptr;
    realWaveOutUnprepare_ = nullptr;
    realWaveOutWrite_ = nullptr;
    realWaveOutReset_ = nullptr;
    if (systemWinmm_) { FreeLibrary(systemWinmm_); systemWinmm_ = nullptr; }
    if (backend_ == Backend::WaveOut) backend_ = Backend::None;
}

inline void AudioOutput::Shutdown() {
    Stop();
    ReleaseDirectSound();
    ReleaseWaveOut();
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    backend_ = Backend::None;
    renderScratch_.clear();
}

inline DWORD WINAPI AudioOutput::AudioThreadProc(LPVOID parameter) {
    static_cast<AudioOutput*>(parameter)->AudioThreadLoop();
    return 0;
}

inline void AudioOutput::RenderAndConvert(int16_t* destination) {
    if (renderCallback_)
        renderCallback_(renderScratch_.data(), segmentFrames_, userData_);
    else
        std::memset(renderScratch_.data(), 0,
                    renderScratch_.size() * sizeof(float));
    for (size_t i = 0; i < renderScratch_.size(); ++i) {
        const float value = std::max(-1.0f, std::min(1.0f, renderScratch_[i]));
        destination[i] = static_cast<int16_t>(value * 32767.0f);
    }
}

inline bool AudioOutput::FillDirectSoundSegment(uint32_t segment) {
    if (!streamBuffer_ || segment >= kSegmentCount) return false;
    if (renderCallback_)
        renderCallback_(renderScratch_.data(), segmentFrames_, userData_);
    else
        std::memset(renderScratch_.data(), 0,
                    renderScratch_.size() * sizeof(float));

    void* first = nullptr;
    void* second = nullptr;
    DWORD firstBytes = 0;
    DWORD secondBytes = 0;
    HRESULT result = streamBuffer_->Lock(segment * segmentBytes_, segmentBytes_,
                                         &first, &firstBytes, &second,
                                         &secondBytes, 0);
    if (result == DSERR_BUFFERLOST) {
        streamBuffer_->Restore();
        ClearDirectSoundBuffer();
        result = streamBuffer_->Lock(segment * segmentBytes_, segmentBytes_,
                                     &first, &firstBytes, &second,
                                     &secondBytes, 0);
    }
    if (FAILED(result)) {
        lastHResult_ = result;
        return false;
    }

    size_t sourceSample = 0;
    auto convert = [&](void* destination, DWORD bytes) {
        int16_t* pcm = static_cast<int16_t*>(destination);
        const size_t samples = bytes / sizeof(int16_t);
        for (size_t i = 0; i < samples; ++i) {
            const float value = std::max(
                -1.0f, std::min(1.0f, renderScratch_[sourceSample++]));
            pcm[i] = static_cast<int16_t>(value * 32767.0f);
        }
    };
    convert(first, firstBytes);
    if (second) convert(second, secondBytes);
    streamBuffer_->Unlock(first, firstBytes, second, secondBytes);
    return true;
}

inline bool AudioOutput::FillWaveOutSegment(uint32_t segment) {
    if (!waveOut_ || !realWaveOutWrite_ || segment >= kSegmentCount)
        return false;
    RenderAndConvert(waveBuffers_[segment].data());
    WAVEHDR& header = waveHeaders_[segment];
    header.dwBufferLength = segmentBytes_;
    const MMRESULT result = realWaveOutWrite_(waveOut_, &header,
                                              sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        lastHResult_ = MmResultToHResult(result);
        return false;
    }
    return true;
}

inline void AudioOutput::AudioThreadLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#if defined(_MM_DENORMALS_ZERO_ON)
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    if (backend_ == Backend::WaveOut)
        WaveOutThreadLoop();
    else
        DirectSoundThreadLoop();
}

inline void AudioOutput::DirectSoundThreadLoop() {
    if (!notify_) {
        uint32_t consumedSegment = 0;
        while (running_.load(std::memory_order_acquire)) {
            if (WaitForSingleObject(stopEvent_, 1) == WAIT_OBJECT_0) break;
            DWORD playCursor = 0;
            const HRESULT result = streamBuffer_->GetCurrentPosition(
                &playCursor, nullptr);
            if (FAILED(result)) {
                lastHResult_ = result;
                TraceXPAudioFailure("DirectSound play cursor",
                                    static_cast<unsigned long>(result));
                break;
            }
            const uint32_t playingSegment =
                (playCursor / segmentBytes_) % kSegmentCount;
            while (consumedSegment != playingSegment) {
                if (!FillDirectSoundSegment(consumedSegment)) return;
                consumedSegment = (consumedSegment + 1u) % kSegmentCount;
            }
        }
        return;
    }

    HANDLE waits[kSegmentCount + 1] = {
        stopEvent_, segmentEvents_[0], segmentEvents_[1],
        segmentEvents_[2], segmentEvents_[3]
    };
    while (running_.load(std::memory_order_acquire)) {
        const DWORD result = WaitForMultipleObjects(kSegmentCount + 1, waits,
                                                    FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
        if (result > WAIT_OBJECT_0 &&
            result <= WAIT_OBJECT_0 + kSegmentCount) {
            if (!FillDirectSoundSegment(result - WAIT_OBJECT_0 - 1u)) break;
        }
    }
}

inline void AudioOutput::WaveOutThreadLoop() {
    for (uint32_t i = 0; i < kSegmentCount; ++i) {
        if (!running_.load(std::memory_order_acquire) ||
            !FillWaveOutSegment(i)) {
            running_.store(false, std::memory_order_release);
            return;
        }
    }

    HANDLE waits[2] = {stopEvent_, waveEvent_};
    while (running_.load(std::memory_order_acquire)) {
        const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
        if (result == WAIT_OBJECT_0 + 1u) {
            // Requeue strictly in completion/submission order. Scanning from
            // slot zero can reorder audio after a delayed wake at ring wrap.
            for (uint32_t completed = 0; completed < kSegmentCount;
                 ++completed) {
                const uint32_t i = waveRefillCursor_;
                if (!(waveHeaders_[i].dwFlags & WHDR_DONE) ||
                    (waveHeaders_[i].dwFlags & WHDR_INQUEUE))
                    break;
                if (!FillWaveOutSegment(i)) {
                    running_.store(false, std::memory_order_release);
                    return;
                }
                waveRefillCursor_ = (waveRefillCursor_ + 1u) % kSegmentCount;
            }
        }
    }
}

} // namespace svms

#endif
