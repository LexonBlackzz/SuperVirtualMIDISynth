#ifndef SVMS_LIVE_RECORDER_H
#define SVMS_LIVE_RECORDER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace svms {

class LiveWaveRecorder {
public:
    enum class State : uint32_t {
        Stopped = 0u,
        Recording = 1u,
        Stopping = 2u,
        Error = 3u,
        Starting = 4u,
    };

    struct Status {
        State state = State::Stopped;
        uint32_t sampleRate = 0u;
        uint64_t framesWritten = 0u;
        uint64_t droppedFrames = 0u;
        uint32_t errorCode = 0u;
    };

    LiveWaveRecorder() = default;
    ~LiveWaveRecorder();

    LiveWaveRecorder(const LiveWaveRecorder&) = delete;
    LiveWaveRecorder& operator=(const LiveWaveRecorder&) = delete;

    bool Start(const wchar_t* path, uint32_t sampleRate, std::string& error);
    void Stop();

    // Audio-thread producer. This function allocates nothing and never waits.
    void Capture(const float* interleavedStereo, uint32_t frames) noexcept;

    Status GetStatus() const noexcept;

private:
    static constexpr uint32_t kChannels = 2u;
    static constexpr uint32_t kBytesPerFrame = sizeof(float) * kChannels;
    static constexpr uint32_t kRingSeconds = 8u;

    bool WriteInitialHeader();
    bool FinalizeHeader();
    bool WriteBytes(const void* data, size_t bytes);
    bool Seek(uint64_t offset);
    uint64_t Tell() const;
    void WriterLoop();

    FILE* file_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    std::thread writer_;
    std::vector<float> ring_;
    uint64_t ringCapacityFrames_ = 0u;

    std::atomic<uint64_t> writeFrame_{0u};
    std::atomic<uint64_t> readFrame_{0u};
    std::atomic<uint64_t> framesWritten_{0u};
    std::atomic<uint64_t> droppedFrames_{0u};
    std::atomic<uint32_t> activeCaptures_{0u};
    std::atomic<uint32_t> state_{static_cast<uint32_t>(State::Stopped)};
    std::atomic<uint32_t> errorCode_{0u};
    uint32_t sampleRate_ = 0u;
    uint64_t dataSizeOffset_ = 0u;
    uint64_t ds64Offset_ = 0u;
};

} // namespace svms

#endif // SVMS_LIVE_RECORDER_H
