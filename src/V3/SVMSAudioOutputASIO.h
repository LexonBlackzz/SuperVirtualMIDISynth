// ASIO playback backend for SVMS V3 (non-XP builds).
//
// Wraps the Steinberg ASIO SDK host facade. The SDK sources
// (asio-main/common + asio-main/host) are compiled into svmsynth; the
// actual ASIO drivers are vendor-provided COM DLLs on the machine and are
// enumerated/opened through the SDK at Initialize time.
//
// Mirrors the AudioOutput (WASAPI) surface used by SVMSDriver:
//   Initialize / Start / Stop / Shutdown / IsRunning
//   GetSampleRate / GetBufferFrames / GetLastError / SetRenderCallback
// The render callback is the same backend-agnostic float stereo filler.
#ifndef SVMS_AUDIO_OUTPUT_ASIO_H
#define SVMS_AUDIO_OUTPUT_ASIO_H

#include <windows.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "SVMSAudioOutput.h"
#include "asiosys.h"     // defines IEEE754_64FLOAT on Windows — must precede asio.h
#include "asio.h"
#include "asiodrivers.h"

// The SDK declares these nowhere public: asiodrivers.cpp defines the global
// and the loader free function (this SDK version has no header for them).
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace svms {

class AudioOutputASIO : public AudioOutputBase {
public:
    using FormatChangedCallback = void (*)(uint32_t sampleRate,
                                           uint32_t bufferFrames, void* userData);

    AudioOutputASIO() = default;
    ~AudioOutputASIO() { Shutdown(); }

    // driverName: empty or "default" -> first enumerated ASIO driver.
    bool Initialize(uint32_t sampleRate, uint32_t bufferFrames,
                    const std::wstring& driverName = {}) {
        Shutdown();
        requestedRate_ = sampleRate;
        requestedFrames_ = bufferFrames;
        configuredDriver_ = driverName;

        char names[kMaxDrivers_][32] = {};
        char* namePtrs[kMaxDrivers_];
        for (int i = 0; i < kMaxDrivers_; ++i) namePtrs[i] = names[i];
        static AsioDrivers drivers;
        ::asioDrivers = &drivers;   // the SDK's loader consumes this global
        const long found = drivers.getDriverNames(namePtrs, kMaxDrivers_);
        if (found <= 0) {
            lastError_ = E_FAIL;
            errorText_ = "no installed ASIO drivers found";
            OutputDebugStringA("[SVMS] ASIO: no installed ASIO drivers found\n");
            return false;
        }
        int pick = 0;
        if (!driverName.empty() && _wcsicmp(driverName.c_str(), L"default") != 0) {
            wchar_t wide[32];
            for (long i = 0; i < found; ++i) {
                MultiByteToWideChar(CP_UTF8, 0, names[i], -1, wide, 32);
                if (!_wcsicmp(wide, driverName.c_str())) {
                    pick = static_cast<int>(i);
                    break;
                }
            }
        }
        strncpy(activeDriverName_, names[pick], sizeof(activeDriverName_) - 1);
        if (!::loadAsioDriver(activeDriverName_)) {
            lastError_ = E_FAIL;
            errorText_ = "ASIO init failed";
            OutputDebugStringA("[SVMS] ASIO: loadAsioDriver failed\n");
            return false;
        }

        ASIODriverInfo info;
        info.asioVersion = 2;
        info.sysRef = nullptr;
        if (ASIOInit(&info) != ASE_OK) {
            lastError_ = E_FAIL;
            errorText_ = "ASIO init failed";
            OutputDebugStringA("[SVMS] ASIO: ASIOInit failed\n");
            return false;
        }
        initialized_ = true;

        long inCh = 0, outCh = 0;
        if (ASIOGetChannels(&inCh, &outCh) != ASE_OK || outCh < 2) {
            lastError_ = E_FAIL;
            errorText_ = "ASIO init failed";
            OutputDebugStringA("[SVMS] ASIO: fewer than 2 output channels\n");
            return false;
        }
        return OpenStream(sampleRate, bufferFrames, /*preferDriverSize=*/true);
    }

    // TEST HOOK: exercises the exact park → teardown → driver reload →
    // restart path the watcher thread uses for driver-initiated resets and
    // silent buffer-size drift. `frames == 0` behaves identically to the
    // drift path (driver's preferred size wins); nonzero requests that
    // exact size (still subject to the driver's min/max window).
    bool TestReopenWithBufferSize(uint32_t frames = 0) {
        char driver[32];
        strncpy(driver, activeDriverName_, sizeof(driver) - 1);
        driver[sizeof(driver) - 1] = 0;
        const bool wasRunning = running_.load(std::memory_order_acquire);
        OutputDebugStringA("[SVMS] ASIO: test hook reopening with size hint\n");
        if (Reopen_(driver, frames) && wasRunning)
            return Start();
        return false;
    }

private:
    // preferDriverSize: the driver's preferred buffer size (what its control
    // panel is set to) wins — used for both the initial open and reset
    // reopens, so the driver is always the latency authority. The caller's
    // hint is only consulted when the driver refuses the preferred size.
    bool OpenStream(uint32_t sampleRate, uint32_t requestedFrames,
                    bool preferDriverSize = false) {
        // Buffer size: honor the request when it fits the driver's window,
        // otherwise take the driver's preferred size.
        long minSize = 0, maxSize = 0, prefSize = 0, granularity = 0;
        if (ASIOGetBufferSize(&minSize, &maxSize, &prefSize, &granularity) != ASE_OK) {
            lastError_ = E_FAIL;
            return false;
        }
        long chosen = prefSize;
        if (!preferDriverSize &&
            requestedFrames >= static_cast<uint32_t>(minSize) &&
            requestedFrames <= static_cast<uint32_t>(maxSize))
            chosen = static_cast<long>(requestedFrames);
        bufferSize_ = static_cast<uint32_t>(chosen);

        // Sample rate: set ours; if the driver refuses, adopt its rate.
        ASIOSampleRate rate = static_cast<ASIOSampleRate>(sampleRate);
        if (ASIOSetSampleRate(rate) != ASE_OK &&
            ASIOGetSampleRate(&rate) != ASE_OK) {
            lastError_ = E_FAIL;
            return false;
        }
        sampleRate_ = static_cast<uint32_t>(rate);

        // Stereo output pair, channels 0/1.
        memset(bufferInfos_, 0, sizeof(bufferInfos_));
        bufferInfos_[0].isInput = ASIOFalse;
        bufferInfos_[0].channelNum = 0;
        bufferInfos_[1].isInput = ASIOFalse;
        bufferInfos_[1].channelNum = 1;

        callbacks_.bufferSwitch = &AudioOutputASIO::BufferSwitchStatic;
        callbacks_.sampleRateDidChange = &AudioOutputASIO::SampleRateDidChangeStatic;
        callbacks_.asioMessage = &AudioOutputASIO::AsioMessageStatic;
        callbacks_.bufferSwitchTimeInfo = &AudioOutputASIO::BufferSwitchTimeInfoStatic;

        if (ASIOCreateBuffers(bufferInfos_, 2, chosen, &callbacks_) != ASE_OK) {
            lastError_ = E_FAIL;
            errorText_ = "ASIO init failed";
            OutputDebugStringA("[SVMS] ASIO: ASIOCreateBuffers failed\n");
            return false;
        }
        buffersCreated_ = true;
        // The driver may have adjusted the requested size (spec allows it);
        // whatever it actually runs with is what the engine must see.
        long actualMin = 0, actualMax = 0, actualPref = 0, actualGran = 0;
        if (ASIOGetBufferSize(&actualMin, &actualMax, &actualPref,
                              &actualGran) == ASE_OK &&
            actualPref > 0) {
            chosen = actualPref;
        }
        bufferSize_ = static_cast<uint32_t>(chosen);

        for (int i = 0; i < 2; ++i) {
            ASIOChannelInfo ci;
            ci.channel = bufferInfos_[i].channelNum;
            ci.isInput = ASIOFalse;
            sampleType_[i] = ASIOGetChannelInfo(&ci) == ASE_OK
                                 ? ci.type : ASIOSTFloat32LSB;
        }
        scratch_.assign(static_cast<size_t>(bufferSize_) * 2u, 0.0f);
        g_instance = this;   // callbacks route to the active output
        return true;
    }

    void RenderInto(long bufferIndex) {
        float* out = scratch_.data();
        const uint32_t frames = bufferSize_;
        if (renderCallback_) {
            renderCallback_(out, frames, userData_);
        } else {
            memset(out, 0, static_cast<size_t>(frames) * 2u * sizeof(float));
        }
        for (int ch = 0; ch < 2; ++ch) {
            ConvertChannel(bufferInfos_[ch].buffers[bufferIndex],
                           out, frames, sampleType_[ch], ch);
        }
        if (switchCount_ == 0) {
            char msg[128];
            wsprintfA(msg, "[SVMS] ASIO: stream live, %u Hz / %u frames, fmt=%s/%s\n",
                      sampleRate_, bufferSize_,
                      SampleTypeName(sampleType_[0]), SampleTypeName(sampleType_[1]));
            OutputDebugStringA(msg);
        }
        // Post-output handshake: drivers we told we'd call this use it to
        // latch the buffers; skipping it leaves them consuming stale/silent
        // data. Cheap call; latch off after the first rejection.
        if (outputReadyEnabled_.load(std::memory_order_relaxed) &&
            ASIOOutputReady() != ASE_OK)
            outputReadyEnabled_.store(false, std::memory_order_relaxed);
        // Heartbeat: proves callbacks fire when diagnosing silent output.
        if ((++switchCount_ % 200) == 0)
            OutputDebugStringA("[SVMS] ASIO: bufferSwitch alive (x200)\n");
    }

    static void ConvertChannel(void* dst, const float* src, uint32_t frames,
                               long type, int ch) {
        // src is the interleaved stereo render buffer (frame i -> src[2i+ch]).
        // All samples are clamped to [-1, 1] before int conversion.
        switch (type) {
            case ASIOSTFloat32LSB: {
                float* out = static_cast<float*>(dst);
                for (uint32_t i = 0; i < frames; ++i)
                    out[i] = src[i * 2u + static_cast<unsigned>(ch)];
                DebugCapture(out, frames * sizeof(float));
                break;
            }
            case ASIOSTFloat64LSB: {
                // FlexASIO and several interfaces negotiate 64-bit floats;
                // previously unhandled -> memset(0) = silent output.
                double* out = static_cast<double*>(dst);
                for (uint32_t i = 0; i < frames; ++i)
                    out[i] = static_cast<double>(
                        src[i * 2u + static_cast<unsigned>(ch)]);
                DebugCapture(out, frames * sizeof(double));
                break;
            }
            case ASIOSTInt32LSB: {
                int32_t* out = static_cast<int32_t*>(dst);
                for (uint32_t i = 0; i < frames; ++i) {
                    float s = src[i * 2u + static_cast<unsigned>(ch)];
                    if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
                    out[i] = static_cast<int32_t>(s * 2147483647.0f);
                }
                DebugCapture(out, frames * sizeof(int32_t));
                break;
            }
            case ASIOSTInt24LSB: {
                uint8_t* out = static_cast<uint8_t*>(dst);
                for (uint32_t i = 0; i < frames; ++i) {
                    float s = src[i * 2u + static_cast<unsigned>(ch)];
                    if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
                    const int32_t v = static_cast<int32_t>(s * 8388607.0f);
                    out[i * 3u + 0u] = static_cast<uint8_t>(v & 0xFF);
                    out[i * 3u + 1u] = static_cast<uint8_t>((v >> 8) & 0xFF);
                    out[i * 3u + 2u] = static_cast<uint8_t>((v >> 16) & 0xFF);
                }
                DebugCapture(out, frames * 3u);
                break;
            }
            case ASIOSTInt16LSB: {
                int16_t* out = static_cast<int16_t*>(dst);
                for (uint32_t i = 0; i < frames; ++i) {
                    float s = src[i * 2u + static_cast<unsigned>(ch)];
                    if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
                    out[i] = static_cast<int16_t>(s * 32767.0f);
                }
                DebugCapture(out, frames * sizeof(int16_t));
                break;
            }
            default:
                memset(dst, 0, static_cast<size_t>(frames) * 4u);
                break;
        }
    }

    static const char* SampleTypeName(long type) {
        switch (type) {
            case ASIOSTFloat32LSB: return "float32";
            case ASIOSTFloat64LSB: return "float64";
            case ASIOSTInt32LSB:   return "int32";
            case ASIOSTInt24LSB:   return "int24";
            case ASIOSTInt16LSB:   return "int16";
            default:               return "unsupported";
        }
    }

public:
    const char* ChannelTypeName(int ch) const {
        return SampleTypeName(ch == 0 ? sampleType_[0] : sampleType_[1]);
    }

    // Debug: when non-null, ConvertChannel appends the raw bytes it hands
    // to the driver for channel 0 (probe/diagnostics only).
    static std::FILE* debugCaptureFile_;

private:

    static void DebugCapture(const void* data, size_t bytes) {
        std::FILE* f = debugCaptureFile_;
        if (f) std::fwrite(data, 1, bytes, f);
    }

    static void BufferSwitchStatic(long bufferIndex,
                                                   ASIOBool directProcess) {
        (void)directProcess;
        // Single acquire-load: park-to-null during a reopen makes this a
        // no-op; a torn-down instance can never be dereferenced here.
        AudioOutputASIO* inst = g_instance.load(std::memory_order_acquire);
        if (inst) inst->RenderInto(bufferIndex);
    }

    static ASIOTime* BufferSwitchTimeInfoStatic(
        ASIOTime* params, long bufferIndex, ASIOBool directProcess) {
        BufferSwitchStatic(bufferIndex, directProcess);
        return params;
    }

    static void SampleRateDidChangeStatic(ASIOSampleRate rate) {
        AudioOutputASIO* inst = g_instance.load(std::memory_order_acquire);
        if (inst) {
            inst->sampleRate_ = static_cast<uint32_t>(rate);
            inst->resetRequested_.store(true, std::memory_order_release);
        }
    }

    static long AsioMessageStatic(long selector, long value,
                                                  void* message, double* opt) {
        (void)message; (void)opt;
        switch (selector) {
            case kAsioSelectorSupported:
                // Note: ASIOOutputReady support is signaled by answering
                // kAsioEngineVersion >= 2 (below), not by a selector here.
                return (value == kAsioEngineVersion ||
                        value == kAsioResetRequest ||
                        value == kAsioBufferSizeChange) ? 1 : 0;
            case kAsioEngineVersion:
                return 2;
            case kAsioResetRequest:
            case kAsioBufferSizeChange: {
                // Driver wants a reopen (control-panel buffer/rate change).
                // Deferred to the watcher thread — never re-enter the driver
                // from inside its own callback.
                AudioOutputASIO* inst = g_instance.load(std::memory_order_acquire);
                if (inst)
                    inst->resetRequested_.store(true, std::memory_order_release);
                return 1;
            }
            case kAsioResyncRequest: {
                return 0;   // nothing to resync; we render fresh every switch
            }
            default:
                return 0;
        }
    }

private:
    void WatcherLoop() {
        while (!watcherQuit_.load(std::memory_order_acquire)) {
            if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
                const bool wasRunning =
                    running_.load(std::memory_order_acquire);
                char driver[32];
                strncpy(driver, activeDriverName_, sizeof(driver) - 1);
                driver[sizeof(driver) - 1] = 0;
                OutputDebugStringA("[SVMS] ASIO: driver requested reset, reopening\n");
                if (Reopen_(driver) && wasRunning)
                    Start();
            } else if (running_.load(std::memory_order_acquire)) {
                // Some drivers (notably hardware interfaces) change their
                // buffer size silently — no reset message, the next
                // bufferSwitch simply arrives at the new size. Poll the
                // driver's size off the audio thread and reopen on drift.
                long mn = 0, mx = 0, pref = 0, gran = 0;
                if (ASIOGetBufferSize(&mn, &mx, &pref, &gran) == ASE_OK &&
                    pref > 0 && static_cast<uint32_t>(pref) != bufferSize_) {
                    OutputDebugStringA(
                        "[SVMS] ASIO: buffer size drifted, reopening\n");
                    char driver[32];
                    strncpy(driver, activeDriverName_, sizeof(driver) - 1);
                    driver[sizeof(driver) - 1] = 0;
                    if (Reopen_(driver))
                        Start();
                }
            }
            Sleep(100);
        }
    }

    // Full driver tear-down + reopen. Call only from the watcher thread
    // with the stream stopped. `frames == 0` adopts the driver's current
    // preferred size (the normal reset/drift path); nonzero requests that
    // exact size, still subject to the driver's min/max window.
    bool Reopen_(const char* driverName, uint32_t frames = 0) {
        Stop();
        // Park: any bufferSwitch that still races in becomes a no-op until
        // OpenStream republishes the (new) stream at its end.
        g_instance.store(nullptr, std::memory_order_release);
        if (buffersCreated_) { ASIODisposeBuffers(); buffersCreated_ = false; }
        if (initialized_) { ASIOExit(); initialized_ = false; }
        if (asioDrivers) asioDrivers->removeCurrentDriver();

        if (!::loadAsioDriver(const_cast<char*>(driverName))) {
            errorText_ = "ASIO reopen failed (driver load)";
            OutputDebugStringA("[SVMS] ASIO: reopen loadAsioDriver failed\n");
            return false;
        }
        ASIODriverInfo info;
        info.asioVersion = 2;
        info.sysRef = nullptr;
        if (ASIOInit(&info) != ASE_OK) {
            errorText_ = "ASIO reopen failed (init)";
            return false;
        }
        initialized_ = true;
        long inCh = 0, outCh = 0;
        if (ASIOGetChannels(&inCh, &outCh) != ASE_OK || outCh < 2) {
            errorText_ = "ASIO reopen failed (channels)";
            return false;
        }
        const uint32_t oldRate = sampleRate_;
        const uint32_t oldFrames = bufferSize_;
        // frames==0 -> the driver's control panel changed the format — its
        // new preferred buffer size (and possibly rate) is the authority,
        // not our stale config hint. Nonzero -> caller's exact request.
        if (!OpenStream(requestedRate_, frames, /*preferDriverSize=*/frames == 0))
            return false;
        if (formatChanged_ &&
            (sampleRate_ != oldRate || bufferSize_ != oldFrames)) {
            OutputDebugStringA(
                "[SVMS] ASIO: format changed on reset, notifying engine\n");
            formatChanged_(sampleRate_, bufferSize_, formatChangedUser_);
        }
        return true;
    }

    static constexpr int kMaxDrivers_ = 8;

    static std::atomic<AudioOutputASIO*> g_instance;

    ASIOBufferInfo bufferInfos_[2];
    ASIOCallbacks callbacks_{};
    long sampleType_[2] = {ASIOSTFloat32LSB, ASIOSTFloat32LSB};
    RenderCallback renderCallback_ = nullptr;
    void* userData_ = nullptr;
    std::vector<float> scratch_;
    std::atomic<bool> running_{false};
    std::atomic<HRESULT> lastError_{S_OK};
    uint32_t requestedRate_ = 0;
    uint32_t requestedFrames_ = 0;
    uint32_t sampleRate_ = 0;
    uint32_t bufferSize_ = 0;
    bool initialized_ = false;
    bool buffersCreated_ = false;
    char activeDriverName_[32] = {};
    const char* errorText_ = "no ASIO driver";
    std::wstring configuredDriver_;
    std::atomic<bool> resetRequested_{false};
    std::atomic<bool> watcherQuit_{false};
    std::thread watcher_;
    FormatChangedCallback formatChanged_ = nullptr;
    void* formatChangedUser_ = nullptr;
    std::atomic<bool> outputReadyEnabled_{true};
    uint32_t switchCount_ = 0;

public:
    bool Start() {
        if (!initialized_ || !buffersCreated_)
            return false;
        if (!watcher_.joinable()) {
            watcherQuit_.store(false, std::memory_order_release);
            watcher_ = std::thread(&AudioOutputASIO::WatcherLoop, this);
        }
        if (running_.load(std::memory_order_acquire))
            return true;
        if (ASIOStart() != ASE_OK) {
            lastError_ = E_FAIL;
            return false;
        }
        running_.store(true, std::memory_order_release);
        return true;
    }

    void Stop() {
        if (running_.exchange(false, std::memory_order_acq_rel))
            ASIOStop();
    }

    void Shutdown() {
        if (watcher_.joinable()) {
            watcherQuit_.store(true, std::memory_order_release);
            watcher_.join();
        }
        g_instance.store(nullptr, std::memory_order_release);
        Stop();
        if (buffersCreated_) { ASIODisposeBuffers(); buffersCreated_ = false; }
        if (initialized_) { ASIOExit(); initialized_ = false; }
        std::vector<float>().swap(scratch_);
    }

    // Invoked (from the watcher thread, stream stopped) when the driver
    // forced a reopen and the format (rate or buffer size) changed, so the
    // engine can reallocate its mix buffers and reconfigure DSP.
    void SetFormatChangedCallback(FormatChangedCallback cb, void* userData) {
        formatChanged_ = cb;
        formatChangedUser_ = userData;
    }

    uint32_t GetSampleRate() const { return sampleRate_; }
    uint32_t GetBufferFrames() const { return bufferSize_; }
    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    HRESULT GetLastError() const { return lastError_; }
    const char* GetLastErrorText() const override { return errorText_; }

    using RenderCallback = void (*)(float* output, uint32_t numFrames, void* userData);
    void SetRenderCallback(RenderCallback cb, void* userData) {
        renderCallback_ = cb;
        userData_ = userData;
    }
};

// The SDK callbacks are plain C function pointers: single active instance.
// Atomic so the watcher thread can park it to null across a driver
// reopen — bufferSwitch then observes either the old or the new stream,
// never a torn-down one (crackle-then-wedge bug).
inline std::atomic<AudioOutputASIO*> AudioOutputASIO::g_instance = nullptr;
inline std::FILE* AudioOutputASIO::debugCaptureFile_ = nullptr;

} // namespace svms

#endif // SVMS_AUDIO_OUTPUT_ASIO_H
