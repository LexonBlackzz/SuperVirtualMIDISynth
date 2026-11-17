// A device-free audio output for tests (selected with SVMS_TEST_FAKE_AUDIO=1
// in place of WASAPI). It calls the render callback once per block period
// from its own thread, like a real device, and throws the audio away.
#ifndef SVMS_AUDIO_OUTPUT_FAKE_H
#define SVMS_AUDIO_OUTPUT_FAKE_H

#include <windows.h>
#include <atomic>
#include <thread>
#include <vector>
#include "driver/AudioOutput.h"

namespace svms {

class AudioOutputFake : public AudioOutputBase {
public:
    ~AudioOutputFake() override { Shutdown(); }

    bool Initialize(uint32_t sampleRate, uint32_t bufferFrames) {
        rate_ = sampleRate ? sampleRate : 44100u;
        frames_ = bufferFrames ? bufferFrames : 441u;
        return true;
    }
    bool Start() override {
        if (running_) return true;
        quit_ = false;
        running_ = true;
        thread_ = std::thread([this] { Run(); });
        return true;
    }
    void Stop() override {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
        running_ = false;
    }
    void Shutdown() override { Stop(); }
    uint32_t GetSampleRate() const override { return rate_; }
    uint32_t GetBufferFrames() const override { return frames_; }
    bool IsRunning() const override { return running_; }
    void SetRenderCallback(RenderCallback cb, void* userData) override {
        callback_ = cb;
        user_ = userData;
    }

private:
    void Run() {
        std::vector<float> block(static_cast<size_t>(frames_) * 2u);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        const LONGLONG periodTicks = static_cast<LONGLONG>(frames_) * 10000000LL / rate_;
        LARGE_INTEGER due;
        while (!quit_) {
            if (callback_) callback_(block.data(), frames_, user_);
            due.QuadPart = -(periodTicks > 0 ? periodTicks : 1);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
        }
        CloseHandle(timer);
    }

    uint32_t rate_ = 44100u;
    uint32_t frames_ = 441u;
    RenderCallback callback_ = nullptr;
    void* user_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::thread thread_;
};

} // namespace svms

#endif // SVMS_AUDIO_OUTPUT_FAKE_H
