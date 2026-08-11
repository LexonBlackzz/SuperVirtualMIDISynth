#include "SVMSAudioOutputDirectSound.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

void RenderSilence(float* output, uint32_t frames, void* userData) {
    auto* callbacks = static_cast<std::atomic<uint32_t>*>(userData);
    std::memset(output, 0, static_cast<size_t>(frames) * 2u * sizeof(float));
    callbacks->fetch_add(1, std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    const bool forceWaveOut = argc == 2 &&
                              std::strcmp(argv[1], "--waveout") == 0;
    if (forceWaveOut)
        SetEnvironmentVariableW(L"SVMS_XP_FORCE_WAVEOUT", L"1");
    else
        SetEnvironmentVariableW(L"SVMS_XP_FORCE_WAVEOUT", nullptr);
    SetEnvironmentVariableW(L"SVMS_XP_FORCE_WINMM_COPY",
                            forceWaveOut ? L"1" : nullptr);

    std::atomic<uint32_t> callbacks{0};
    svms::AudioOutput output;
    output.SetRenderCallback(RenderSilence, &callbacks);
    if (!output.Initialize(44100, 512)) {
        std::printf("SKIP: XP audio initialization failed (0x%08lx)\n",
                    static_cast<unsigned long>(output.GetLastError()));
        return 77;
    }
    if (output.IsWaveOutFallback() != forceWaveOut) {
        std::puts("FAIL: XP audio selected the unexpected backend");
        return 1;
    }
    if (!output.Start()) {
        std::printf("FAIL: DirectSound start failed (0x%08lx)\n",
                    static_cast<unsigned long>(output.GetLastError()));
        return 1;
    }
    Sleep(250);
    output.Stop();
    if (callbacks.load(std::memory_order_relaxed) == 0u) {
        std::puts("FAIL: DirectSound produced no notification callbacks");
        return 1;
    }
    if (output.GetBufferFrames() != 512u) {
        std::printf("FAIL: configured 512 frames became %lu frames\n",
                    static_cast<unsigned long>(output.GetBufferFrames()));
        return 1;
    }
    std::printf("PASS: %s callbacks=%lu, segment_frames=%lu\n",
                forceWaveOut ? "waveOut" : "DirectSound",
                static_cast<unsigned long>(callbacks.load(std::memory_order_relaxed)),
                static_cast<unsigned long>(output.GetBufferFrames()));
    SetEnvironmentVariableW(L"SVMS_XP_FORCE_WAVEOUT", nullptr);
    SetEnvironmentVariableW(L"SVMS_XP_FORCE_WINMM_COPY", nullptr);
    return 0;
}
