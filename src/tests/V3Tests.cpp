#include "synth/Tuning.h"
#include "tests/VibratoGolden.h"
#include "synth/ChannelCache.h"
#include "render/RenderScalar.h"
#include "synth/SoundFont.h"
#include "synth/VoiceManager.h"
#include "render/HilbertPair.h"
#include "render/Envelope.h"
#include "events/PSCQueue.h"
#include "events/MPSCQueue.h"
#include "events/EventScheduler.h"
#include "events/EventPages.h"
#include "events/EventCompile.h"
#include "events/SysEx.h"
#include "events/MidiByteStream.h"
#include "synth/Config.h"
#include "synth/PresetResolve.h"
#include "events/FrameClock.h"
#include "render/PostFilter.h"
#include "render/ChannelLimiter.h"
#include "events/NoteOnCollapse.h"
#include "events/NoteMerge.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <crtdbg.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

static std::atomic<uint64_t> g_realtimeAllocationCount{0};
static thread_local bool g_trackRealtimeAllocations = false;

void* operator new(std::size_t size) {
    if (g_trackRealtimeAllocations)
        g_realtimeAllocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (g_trackRealtimeAllocations)
        g_realtimeAllocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = _aligned_malloc(size, static_cast<std::size_t>(alignment)))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    _aligned_free(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    _aligned_free(memory);
}

namespace {

int g_failures = 0;
// Whole-voice differential tolerances.  Exact mode keeps the oracle numbers;
// the quality.fast_chunk_math test widens both (closed-form phase/gain are
// tolerance-equal and, being one rounding per chunk instead of eight, drift
// from the sequentially-rounded oracle by a fraction of a frame).
float g_waveTol = 2.0e-5f;
float g_phaseTol = 2.0e-3f;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool NearlyEqual(float a, float b, float epsilon = 1.0e-5f) {
    return std::fabs(a - b) <= epsilon;
}

#include "tests/cases/Basics.inl"
#include "tests/cases/VoiceLifecycle.inl"
#include "tests/cases/Scheduler.inl"
#include "tests/cases/SoundFontStack.inl"
#include "tests/cases/RenderDifferentials.inl"
#include "tests/cases/WholeVoice.inl"
#include "tests/cases/GpuStage.inl"

} // namespace

#include "tests/cases/Features.inl"

int main() {
    // Run the whole suite with launch staging on: the same differentials, the
    // same tolerances (SVMS_TEST_LAUNCH_STAGING=1).
    const bool launchStaging = std::getenv("SVMS_TEST_LAUNCH_STAGING") != nullptr;
    svms::g_launchStagingEnabled.store(launchStaging);
    if (std::getenv("SVMS_TEST_GPU_ONLY") != nullptr) {
        TestGpuStageDifferential();
        std::puts(g_failures == 0 ? "GPU stage tests passed" : "GPU stage tests FAILED");
        return g_failures == 0 ? 0 : 1;
    }
    TestMidiByteStream();
    TestReExciteKey();
    TestReExciteRenderDifferential(128u, 0);
    TestReExciteRenderDifferential(2u, 1);
    TestReExciteRenderDifferential(64u, 2);
    TestMergedNoteExactness();
    TestNoteMergerSemantics();
    TestMergedForkInFullPool();
    TestExtendedKeysAndTuning();
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_CHECK_ALWAYS_DF);
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF);
#endif
    TestWholeVoiceStealDifferential();
    TestWholeVoiceRotationStealDifferential();
    TestWholeVoiceLaunchDifferential();
    TestLaunchStagingIdentity();
    TestLazyLaunchIdentity();
    TestWholeVoiceRotationLaunchDifferential();
    TestWholeVoiceResetBarrierDifferential();
    TestFloorLimitedReleaseSamples();
    TestWholeVoiceDecimation();
    TestWholeVoiceDecimationByLevel();
    TestMutedTailCull();
    TestFilteredGroupKernel();
    TestWholeVoiceCCDifferential();
    TestFastChunkMathDifferential();
    TestWholeVoiceOpBudgetSplitDifferential();
    TestWholeVoiceVibratoDifferential();
    TestWholeVoiceScalarVibratoDifferential();
    TestHilbertPairDifferential();
    TestHilbertPairLoopAware();
    TestPerChannelLimiterDifferential();
    TestWholeVoiceProductionGateParity();
    TestGpuStageDifferential();
    TestPerKeyVoiceCap();
    TestStealReject();
    TestFastProducerRing();
    TestExclusiveClassLifecycle();
    TestSfzParserAndPlayback();
    TestSoundFontStackResolution();

    if (launchStaging) {
        std::printf("launch staging: %llu staged launches, %llu leaked blocks\n",
                    static_cast<unsigned long long>(
                        svms::g_launchStagingStaged.load()),
                    static_cast<unsigned long long>(
                        svms::g_launchStagingLeaks.load()));
        Check(svms::g_launchStagingStaged.load() > 0u,
              "launch staging: no launch was staged (feature not exercised)");
        Check(svms::g_launchStagingLeaks.load() == 0u,
              "launch staging: a block ended with a staged row");
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }

    std::puts("SVMS V3 correctness tests passed");
    return 0;
}
