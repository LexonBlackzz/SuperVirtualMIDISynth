// ASIO playback backend for SVMS V3 (non-XP builds).
//
// A thin client of AsioHost, which owns every driver on its own thread (see
// AsioHost.h). Mirrors the AudioOutput (WASAPI) surface used by the driver:
//   Initialize / Start / Stop / Shutdown / IsRunning
//   GetSampleRate / GetBufferFrames / GetLastError / SetRenderCallback
#ifndef SVMS_AUDIO_OUTPUT_ASIO_H
#define SVMS_AUDIO_OUTPUT_ASIO_H

#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include "driver/AudioOutput.h"
#include "driver/AsioHost.h"

namespace svms {

class AudioOutputASIO : public AudioOutputBase {
public:
    // The host reopened the stream by itself (driver reset or rate change).
    // Called before the reopened stream's first callback.
    using FormatChangedCallback = void (*)(uint32_t sampleRate, uint32_t bufferFrames,
                                           void* userData);
    // The host gave up on the stream (the driver could not be reopened).
    using DeadCallback = void (*)(void* userData);

    AudioOutputASIO() = default;
    ~AudioOutputASIO() override { Shutdown(); }

    // driverName: empty or "default" -> the first installed ASIO driver.
    // firstChannel: 0-based first output of the stereo pair.
    bool Initialize(uint32_t sampleRate, uint32_t bufferFrames,
                    const std::wstring& driverName = {}, uint32_t firstChannel = 0) {
        Shutdown();
        AsioHost& host = AsioHost::Instance();
        std::string name = Narrow(driverName);
        if (name.empty() || _stricmp(name.c_str(), "default") == 0) {
            const std::vector<AsioDriverEntry> drivers = host.ListDrivers();
            if (drivers.empty()) return Fail("no installed ASIO drivers found");
            name = drivers.front().name;
        }
        AsioStreamInfo info;
        std::string error;
        if (!host.Open(name, sampleRate, bufferFrames, firstChannel, &RenderThunk, this,
                       &EventThunk, info, error))
            return Fail(error);
        owner_ = this;
        sampleRate_ = info.rate;
        bufferFrames_ = info.frames;
        {
            std::lock_guard<std::mutex> lock(infoMutex_);
            info_ = info;
        }
        if (!info.note.empty())
            Log("[SVMS] ASIO: " + info.driver + ": " + info.note + "\n");
        return true;
    }

    bool Start() override {
        if (owner_.load() != this) return false;
        std::string error;
        if (!AsioHost::Instance().Start(error)) return Fail(error);
        running_ = true;
        return true;
    }

    void Stop() override {
        if (running_.exchange(false) && owner_.load() == this) AsioHost::Instance().Stop();
    }

    void Shutdown() override {
        running_ = false;
        AudioOutputASIO* self = this;
        if (owner_.compare_exchange_strong(self, nullptr)) AsioHost::Instance().Close();
    }

    uint32_t GetSampleRate() const override { return sampleRate_.load(); }
    uint32_t GetBufferFrames() const override { return bufferFrames_.load(); }
    bool IsRunning() const override { return running_.load(); }
    HRESULT GetLastError() const override { return lastError_.load(); }
    const char* GetLastErrorText() const override { return errorText_.c_str(); }

    void SetRenderCallback(RenderCallback cb, void* userData) override {
        renderCallback_ = cb;
        userData_ = userData;
    }
    void SetFormatChangedCallback(FormatChangedCallback cb, void* userData) {
        formatChanged_ = cb;
        formatChangedUser_ = userData;
    }
    void SetDeadCallback(DeadCallback cb, void* userData) {
        dead_ = cb;
        deadUser_ = userData;
    }

    AsioStreamInfo Info() const {
        std::lock_guard<std::mutex> lock(infoMutex_);
        return info_;
    }

private:
    static std::string Narrow(const std::wstring& w) {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                          nullptr);
        if (n <= 1) return {};
        std::string s(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    static void Log(const std::string& text) { OutputDebugStringA(text.c_str()); }

    bool Fail(const std::string& error) {
        lastError_ = E_FAIL;
        errorText_ = error.empty() ? std::string("ASIO error") : error;
        Log("[SVMS] ASIO: " + errorText_ + "\n");
        return false;
    }

    static void RenderThunk(float* lr, uint32_t frames, void* user) {
        AudioOutputASIO* self = static_cast<AudioOutputASIO*>(user);
        if (RenderCallback cb = self->renderCallback_)
            cb(lr, frames, self->userData_);
        else
            std::memset(lr, 0, static_cast<size_t>(frames) * 2u * sizeof(float));
    }

    static void EventThunk(const AsioStreamInfo& info, bool streamDead, void* user) {
        AudioOutputASIO* self = static_cast<AudioOutputASIO*>(user);
        if (streamDead) {
            self->running_ = false;
            Log("[SVMS] ASIO: the driver could not be reopened\n");
            if (self->dead_) self->dead_(self->deadUser_);
            return;
        }
        const bool changed =
            info.rate != self->sampleRate_.load() || info.frames != self->bufferFrames_.load();
        self->sampleRate_ = info.rate;
        self->bufferFrames_ = info.frames;
        {
            std::lock_guard<std::mutex> lock(self->infoMutex_);
            self->info_ = info;
        }
        if (changed && self->formatChanged_)
            self->formatChanged_(info.rate, info.frames, self->formatChangedUser_);
    }

    // The host carries one stream; the output that opened it owns it.
    static inline std::atomic<AudioOutputASIO*> owner_{nullptr};

    RenderCallback renderCallback_ = nullptr;
    void* userData_ = nullptr;
    FormatChangedCallback formatChanged_ = nullptr;
    void* formatChangedUser_ = nullptr;
    DeadCallback dead_ = nullptr;
    void* deadUser_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<HRESULT> lastError_{S_OK};
    std::atomic<uint32_t> sampleRate_{0};
    std::atomic<uint32_t> bufferFrames_{0};
    std::string errorText_;
    mutable std::mutex infoMutex_;
    AsioStreamInfo info_;
};

} // namespace svms

#endif // SVMS_AUDIO_OUTPUT_ASIO_H
