#include "SVMSLiveRecorder.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace svms {
namespace {

template <typename T>
bool WriteValue(FILE* file, const T& value) {
    return std::fwrite(&value, sizeof(value), 1u, file) == 1u;
}

} // namespace

LiveWaveRecorder::~LiveWaveRecorder() {
    Stop();
}

bool LiveWaveRecorder::Start(const wchar_t* path, uint32_t sampleRate,
                             std::string& error) {
    Stop();
    error.clear();
    if (!path || path[0] == L'\0' || sampleRate < 8000u ||
        sampleRate > 384000u) {
        error = "invalid recording path or sample rate";
        return false;
    }

    const uint64_t capacity = static_cast<uint64_t>(sampleRate) * kRingSeconds;
    if (capacity > std::numeric_limits<size_t>::max() / kChannels) {
        error = "recording buffer is too large";
        return false;
    }
    try {
        ring_.assign(static_cast<size_t>(capacity) * kChannels, 0.0f);
    } catch (...) {
        error = "could not allocate the recording buffer";
        return false;
    }

    file_ = _wfopen(path, L"wb+");
    if (!file_) {
        ring_.clear();
        error = "could not create the output WAV";
        return false;
    }
    sampleRate_ = sampleRate;
    ringCapacityFrames_ = capacity;
    writeFrame_.store(0u, std::memory_order_relaxed);
    readFrame_.store(0u, std::memory_order_relaxed);
    framesWritten_.store(0u, std::memory_order_relaxed);
    droppedFrames_.store(0u, std::memory_order_relaxed);
    activeCaptures_.store(0u, std::memory_order_relaxed);
    errorCode_.store(0u, std::memory_order_relaxed);

    if (!WriteInitialHeader()) {
        std::fclose(file_);
        file_ = nullptr;
        ring_.clear();
        error = "could not write the WAV header";
        return false;
    }

    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent_) {
        std::fclose(file_);
        file_ = nullptr;
        ring_.clear();
        error = "could not create the recording worker event";
        return false;
    }

    state_.store(static_cast<uint32_t>(State::Starting),
                 std::memory_order_release);
    try {
        writer_ = std::thread(&LiveWaveRecorder::WriterLoop, this);
    } catch (...) {
        state_.store(static_cast<uint32_t>(State::Stopped),
                     std::memory_order_release);
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
        std::fclose(file_);
        file_ = nullptr;
        ring_.clear();
        error = "could not start the recording writer thread";
        return false;
    }
    state_.store(static_cast<uint32_t>(State::Recording),
                 std::memory_order_release);
    SetEvent(wakeEvent_);
    return true;
}

void LiveWaveRecorder::Stop() {
    const uint32_t oldState = state_.exchange(
        static_cast<uint32_t>(State::Stopping), std::memory_order_acq_rel);
    if (oldState == static_cast<uint32_t>(State::Stopped) &&
        !writer_.joinable()) {
        state_.store(static_cast<uint32_t>(State::Stopped),
                     std::memory_order_release);
        return;
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
    if (writer_.joinable()) writer_.join();
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
    ring_.clear();
    ring_.shrink_to_fit();
    ringCapacityFrames_ = 0u;
    if (oldState == static_cast<uint32_t>(State::Error)) {
        state_.store(static_cast<uint32_t>(State::Error),
                     std::memory_order_release);
    } else {
        state_.store(static_cast<uint32_t>(State::Stopped),
                     std::memory_order_release);
    }
}

void LiveWaveRecorder::Capture(const float* samples, uint32_t frames) noexcept {
    if (!samples || frames == 0u ||
        state_.load(std::memory_order_acquire) !=
            static_cast<uint32_t>(State::Recording)) {
        return;
    }

    activeCaptures_.fetch_add(1u, std::memory_order_acq_rel);
    if (state_.load(std::memory_order_acquire) !=
        static_cast<uint32_t>(State::Recording)) {
        activeCaptures_.fetch_sub(1u, std::memory_order_release);
        return;
    }

    const uint64_t write = writeFrame_.load(std::memory_order_relaxed);
    const uint64_t read = readFrame_.load(std::memory_order_acquire);
    const uint64_t used = write - read;
    if (used > ringCapacityFrames_ ||
        static_cast<uint64_t>(frames) > ringCapacityFrames_ - used) {
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        activeCaptures_.fetch_sub(1u, std::memory_order_release);
        return;
    }

    const uint64_t start = write % ringCapacityFrames_;
    const uint32_t first = static_cast<uint32_t>((std::min<uint64_t>)(
        frames, ringCapacityFrames_ - start));
    std::memcpy(ring_.data() + static_cast<size_t>(start) * kChannels,
                samples, static_cast<size_t>(first) * kBytesPerFrame);
    if (first != frames) {
        std::memcpy(ring_.data(),
                    samples + static_cast<size_t>(first) * kChannels,
                    static_cast<size_t>(frames - first) * kBytesPerFrame);
    }
    writeFrame_.store(write + frames, std::memory_order_release);
    if (wakeEvent_) SetEvent(wakeEvent_);
    activeCaptures_.fetch_sub(1u, std::memory_order_release);
}

LiveWaveRecorder::Status LiveWaveRecorder::GetStatus() const noexcept {
    Status status{};
    status.state = static_cast<State>(state_.load(std::memory_order_acquire));
    status.sampleRate = sampleRate_;
    status.framesWritten = framesWritten_.load(std::memory_order_acquire);
    status.droppedFrames = droppedFrames_.load(std::memory_order_acquire);
    status.errorCode = errorCode_.load(std::memory_order_acquire);
    return status;
}

bool LiveWaveRecorder::WriteInitialHeader() {
    if (!file_) return false;
    const uint32_t zero32 = 0u;
    const uint32_t reserveSize = 28u;
    const uint32_t fmtSize = 16u;
    const uint16_t formatFloat = 3u;
    const uint16_t channels = 2u;
    const uint32_t byteRate = sampleRate_ * kBytesPerFrame;
    const uint16_t blockAlign = static_cast<uint16_t>(kBytesPerFrame);
    const uint16_t bits = 32u;

    if (!WriteBytes("RIFF", 4u) || !WriteValue(file_, zero32) ||
        !WriteBytes("WAVE", 4u) || !WriteBytes("JUNK", 4u) ||
        !WriteValue(file_, reserveSize)) return false;
    ds64Offset_ = Tell();
    uint8_t reserved[28]{};
    if (!WriteBytes(reserved, sizeof(reserved)) ||
        !WriteBytes("fmt ", 4u) || !WriteValue(file_, fmtSize) ||
        !WriteValue(file_, formatFloat) || !WriteValue(file_, channels) ||
        !WriteValue(file_, sampleRate_) || !WriteValue(file_, byteRate) ||
        !WriteValue(file_, blockAlign) || !WriteValue(file_, bits) ||
        !WriteBytes("data", 4u)) return false;
    dataSizeOffset_ = Tell();
    return WriteValue(file_, zero32);
}

bool LiveWaveRecorder::FinalizeHeader() {
    if (!file_) return false;
    const uint64_t frames = framesWritten_.load(std::memory_order_relaxed);
    const uint64_t dataBytes = frames * kBytesPerFrame;
    const uint64_t end = Tell();
    bool ok = true;
    if (dataBytes <= 0xffffffffull && end >= 8u && end - 8u <= 0xffffffffull) {
        ok &= Seek(4u);
        const uint32_t riffSize = static_cast<uint32_t>(end - 8u);
        ok &= WriteValue(file_, riffSize);
        ok &= Seek(dataSizeOffset_);
        const uint32_t dataSize = static_cast<uint32_t>(dataBytes);
        ok &= WriteValue(file_, dataSize);
    } else {
        ok &= Seek(0u);
        ok &= WriteBytes("RF64", 4u);
        const uint32_t sentinel = 0xffffffffu;
        ok &= WriteValue(file_, sentinel);
        ok &= Seek(12u);
        ok &= WriteBytes("ds64", 4u);
        const uint32_t ds64Size = 28u;
        ok &= WriteValue(file_, ds64Size);
        ok &= WriteValue(file_, end - 8u);
        ok &= WriteValue(file_, dataBytes);
        ok &= WriteValue(file_, frames);
        const uint32_t tableLength = 0u;
        ok &= WriteValue(file_, tableLength);
        ok &= Seek(dataSizeOffset_);
        ok &= WriteValue(file_, sentinel);
    }
    return ok && std::fflush(file_) == 0;
}

bool LiveWaveRecorder::WriteBytes(const void* data, size_t bytes) {
    return file_ && std::fwrite(data, 1u, bytes, file_) == bytes;
}

bool LiveWaveRecorder::Seek(uint64_t offset) {
    return file_ && _fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) == 0;
}

uint64_t LiveWaveRecorder::Tell() const {
    if (!file_) return 0u;
    const __int64 position = _ftelli64(file_);
    return position < 0 ? 0u : static_cast<uint64_t>(position);
}

void LiveWaveRecorder::WriterLoop() {
    for (;;) {
        const uint64_t read = readFrame_.load(std::memory_order_relaxed);
        const uint64_t write = writeFrame_.load(std::memory_order_acquire);
        if (read != write) {
            const uint64_t start = read % ringCapacityFrames_;
            const uint64_t available = write - read;
            const uint32_t frames = static_cast<uint32_t>((std::min<uint64_t>)(
                available, ringCapacityFrames_ - start));
            const size_t written = std::fwrite(
                ring_.data() + static_cast<size_t>(start) * kChannels,
                kBytesPerFrame, frames, file_);
            if (written != frames) {
                if (written != 0u) {
                    readFrame_.store(read + written, std::memory_order_release);
                    framesWritten_.fetch_add(written, std::memory_order_release);
                }
                errorCode_.store(1u, std::memory_order_release);
                state_.store(static_cast<uint32_t>(State::Error),
                             std::memory_order_release);
                break;
            }
            readFrame_.store(read + frames, std::memory_order_release);
            framesWritten_.fetch_add(frames, std::memory_order_release);
            continue;
        }

        const State state = static_cast<State>(
            state_.load(std::memory_order_acquire));
        if (state != State::Recording && state != State::Starting &&
            activeCaptures_.load(std::memory_order_acquire) == 0u) {
            const uint64_t finalWrite = writeFrame_.load(std::memory_order_acquire);
            if (readFrame_.load(std::memory_order_relaxed) == finalWrite) break;
        }
        if (wakeEvent_) WaitForSingleObject(wakeEvent_, 100u);
    }

    if (!FinalizeHeader()) {
        errorCode_.store(2u, std::memory_order_release);
        state_.store(static_cast<uint32_t>(State::Error),
                     std::memory_order_release);
    }
    if (file_) {
        if (std::fclose(file_) != 0) {
            errorCode_.store(3u, std::memory_order_release);
            state_.store(static_cast<uint32_t>(State::Error),
                         std::memory_order_release);
        }
        file_ = nullptr;
    }
}

} // namespace svms
