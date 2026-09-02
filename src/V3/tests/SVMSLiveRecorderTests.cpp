#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../SVMSLiveRecorder.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

uint32_t U32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value = 0u;
    if (offset + sizeof(value) <= bytes.size())
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

} // namespace

int main() {
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) return 1;
    const std::filesystem::path path = std::filesystem::path(temporary) /
        (L"SVMS_live_recording_\x0442\x0435\x0441\x0442_" +
         std::to_wstring(GetCurrentProcessId()) + L".wav");

    constexpr uint32_t sampleRate = 44100u;
    constexpr uint32_t frames = 1000u;
    std::vector<float> samples(static_cast<size_t>(frames) * 2u);
    for (uint32_t i = 0u; i < frames; ++i) {
        samples[i * 2u] = static_cast<float>(i) / frames;
        samples[i * 2u + 1u] = -samples[i * 2u];
    }

    bool ok = true;
    svms::LiveWaveRecorder recorder;
    std::string error;
    ok &= Check(recorder.Start(path.c_str(), sampleRate, error),
                "start live recorder");
    recorder.Capture(samples.data(), frames);
    recorder.Stop();
    recorder.Stop();

    const auto status = recorder.GetStatus();
    ok &= Check(status.state == svms::LiveWaveRecorder::State::Stopped,
                "recorder stopped");
    ok &= Check(status.framesWritten == frames, "all frames written");
    ok &= Check(status.droppedFrames == 0u, "no recording frames dropped");

    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    ok &= Check(bytes.size() == 80u + static_cast<size_t>(frames) * 8u,
                "expected WAV size");
    ok &= Check(bytes.size() >= 80u &&
                std::memcmp(bytes.data(), "RIFF", 4u) == 0 &&
                std::memcmp(bytes.data() + 8u, "WAVE", 4u) == 0 &&
                std::memcmp(bytes.data() + 12u, "JUNK", 4u) == 0 &&
                std::memcmp(bytes.data() + 48u, "fmt ", 4u) == 0 &&
                std::memcmp(bytes.data() + 72u, "data", 4u) == 0,
                "WAV chunks present");
    ok &= Check(U32(bytes, 4u) == bytes.size() - 8u,
                "RIFF size finalized");
    ok &= Check(U32(bytes, 76u) == frames * 8u,
                "data size finalized");
    ok &= Check(U32(bytes, 60u) == sampleRate,
                "sample rate stored");
    if (bytes.size() >= 80u + sizeof(float) * 2u) {
        float firstLeft = 1.0f;
        float lastRight = 0.0f;
        std::memcpy(&firstLeft, bytes.data() + 80u, sizeof(float));
        std::memcpy(&lastRight,
                    bytes.data() + 80u + (frames * 2u - 1u) * sizeof(float),
                    sizeof(float));
        ok &= Check(firstLeft == samples[0] &&
                    std::fabs(lastRight - samples.back()) < 1.0e-7f,
                    "interleaved float samples preserved");
    }

    input.close();          // release the sharing handle before unlinking
    DeleteFileW(path.c_str());
    if (!ok) return 1;
    std::cout << "Live recorder tests passed\n";
    return 0;
}
