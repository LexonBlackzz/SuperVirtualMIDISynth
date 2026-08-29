#include <windows.h>
#include <mmreg.h>
#include <mmeapi.h>
#include <timeapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <algorithm>
#include <iterator>
#include <intrin.h>
#include <limits>
#include <thread>

#if defined(SVMS_XP_COMPAT)
#include "SVMSAudioOutputDirectSound.h"
#else
#include "SVMSAudioOutput.h"
#endif
#include "SVMSVoiceManager.h"
#include "SVMSChannelCache.h"
#include "SVMSRenderScalar.h"
#include "SVMSSoundFont.h"
#include "SVMSConfig.h"
#include "SVMSMPSCQueue.h"
#include "SVMSPSCQueue.h"
#include "SVMSNoteOnCollapse.h"

// Fixed QPC-time collapse window for same-key note-on coalescing.
// Frame-size independent by construction: never derived from the WASAPI
// buffer size. With the default threshold of 32 the sustained spawn rate
// when enabled is 32 / 20 ms = 1600 Hz per key.
constexpr uint32_t kNoteOnCollapseWindowMs = 20u;
constexpr uint32_t kNoteOnCollapseDefaultThreshold = 32u;

#include "SVMSEventScheduler.h"
#include "SVMSEventPages.h"
#include "SVMSEventCompile.h"
#include "SVMSSysEx.h"
#include "SVMSFrameClock.h"
#include "SVMSDiagWindow.h"
#include "SVMSPostFilter.h"
#include "SVMSLimiter.h"
#include "SVMSRuntimeLink.h"
#include "SVMSBuildInfo.h"
#include "SVMSNativeOffline.h"
#include "include/svmsapi.h"
#if !defined(SVMS_XP_COMPAT)
#include "SVMSLiveRecorder.h"
#endif

// ── Logging ────────────────────────────────────────────────────────────

#define SVMS_VERBOSE_LOG 0

static FILE* g_logFile = nullptr;
static CRITICAL_SECTION g_logLock;
static bool g_logInit = false;

static void LogInit() {
    if (g_logInit) return;
    InitializeCriticalSection(&g_logLock);
    g_logInit = true;

#if SVMS_VERBOSE_LOG
    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* slash = strrchr(logPath, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(logPath, MAX_PATH, "svms_v3.log");
    g_logFile = fopen(logPath, "w");
    if (g_logFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_logFile, "=== SVMS V3 Log [%04d-%02d-%02d %02d:%02d:%02d] ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(g_logFile);
    }
#endif
}

static void Log(const char* fmt, ...) {
#if !SVMS_VERBOSE_LOG
    (void)fmt;
#endif
#if SVMS_VERBOSE_LOG
    if (!g_logInit) LogInit();
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    OutputDebugStringA(buf);
    EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        fputs(buf, g_logFile);
        fflush(g_logFile);
    }
    LeaveCriticalSection(&g_logLock);
#endif
}

#define LOG(fmt, ...) Log("[SVMS] " fmt "\n", ##__VA_ARGS__)

#if defined(SVMS_XP_COMPAT)
static void XPBootstrapTrace(const char* message) {
    OutputDebugStringA(message);
}

// A drop-in winmm.dll must continue forwarding the non-MIDI multimedia API.
// DirectSound and XP audio drivers import these exports by module name, so
// returning placeholder values here makes a perfectly healthy audio device
// disappear inside the host process. Match V1: load the genuine DLL by its
// absolute System32 path and forward into that module.
static volatile LONG g_xpSystemWinmmState = 0;
static HMODULE g_xpSystemWinmm = nullptr;

static void TraceXPModuleA(const char* label, HMODULE module) {
    wchar_t widePath[MAX_PATH] = {};
    char path[MAX_PATH * 3] = {};
    if (module) GetModuleFileNameW(module, widePath, MAX_PATH);
    if (widePath[0]) {
        WideCharToMultiByte(CP_ACP, 0, widePath, -1, path,
                            static_cast<int>(sizeof(path)), nullptr, nullptr);
    }
    char message[1200] = {};
    std::snprintf(message, sizeof(message),
                  "[SVMS XP] %s handle=%p path='%s' lastError=0x%08lX\r\n",
                  label, static_cast<void*>(module), path[0] ? path : "<none>",
                  static_cast<unsigned long>(GetLastError()));
    OutputDebugStringA(message);
}

static HMODULE GetXPSystemWinmm() {
    LONG state = InterlockedCompareExchange(&g_xpSystemWinmmState, 1, 0);
    if (state == 0) {
        wchar_t systemPath[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(systemPath, MAX_PATH);
        static const wchar_t suffix[] = L"\\winmm.dll";
        bool success = length != 0 &&
            length + (sizeof(suffix) / sizeof(suffix[0])) <= MAX_PATH;
        if (success) std::wcscat(systemPath, suffix);
        if (success) {
            g_xpSystemWinmm = LoadLibraryW(systemPath);
            success = g_xpSystemWinmm != nullptr;
        }
        TraceXPModuleA("proxy GetModuleHandle(winmm.dll)",
                       GetModuleHandleW(L"winmm.dll"));
        TraceXPModuleA("absolute system WinMM result", g_xpSystemWinmm);
        OutputDebugStringA(success
            ? "[SVMS XP] system WinMM forwarding bridge initialized\r\n"
            : "[SVMS XP] system WinMM forwarding bridge FAILED\r\n");
        InterlockedExchange(&g_xpSystemWinmmState, success ? 2 : 3);
        return g_xpSystemWinmm;
    }
    while (state == 1) {
        Sleep(0);
        state = InterlockedCompareExchange(&g_xpSystemWinmmState, 0, 0);
    }
    return state == 2 ? g_xpSystemWinmm : nullptr;
}

static FARPROC GetXPSystemWinmmProc(const char* name) {
    HMODULE module = GetXPSystemWinmm();
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    if (!proc) {
        char message[256] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] system WinMM export '%s' missing, error=0x%08lX\r\n",
                      name, static_cast<unsigned long>(GetLastError()));
        OutputDebugStringA(message);
    }
    return proc;
}

static FARPROC GetSystemWinmmProc(const char* name) {
    return GetXPSystemWinmmProc(name);
}
#else
static void XPBootstrapTrace(const char*) {}

// Hosts may use the local winmm.dll for more than MIDI. Resolve those
// compatibility exports from the genuine system DLL by absolute path so the
// loader cannot hand us this proxy again and recurse back into it.
static volatile LONG g_systemWinmmState = 0;
static HMODULE g_systemWinmm = nullptr;

static HMODULE GetSystemWinmm() {
    LONG state = InterlockedCompareExchange(&g_systemWinmmState, 1, 0);
    if (state == 0) {
        wchar_t systemPath[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(systemPath, MAX_PATH);
        static const wchar_t suffix[] = L"\\winmm.dll";
        bool success = length != 0 &&
            length + (sizeof(suffix) / sizeof(suffix[0])) <= MAX_PATH;
        if (success) std::wcscat(systemPath, suffix);
        if (success) {
            g_systemWinmm = LoadLibraryW(systemPath);
            success = g_systemWinmm != nullptr;
        }
        InterlockedExchange(&g_systemWinmmState, success ? 2 : 3);
        return g_systemWinmm;
    }
    while (state == 1) {
        Sleep(0);
        state = InterlockedCompareExchange(&g_systemWinmmState, 0, 0);
    }
    return state == 2 ? g_systemWinmm : nullptr;
}

static FARPROC GetSystemWinmmProc(const char* name) {
    HMODULE module = GetSystemWinmm();
    return module ? GetProcAddress(module, name) : nullptr;
}
#endif

namespace svms {

static uint32_t SelectRenderLanesForPhysicalCores(uint32_t cores) {
    // Dense tile rendering streams sample data through the shared memory
    // hierarchy from every lane; past roughly cores-2 lanes, SMT siblings
    // and per-chunk join overhead cost more than they add (measured on an
    // 8C/16T part: throughput peaks at 6 lanes and falls monotonically to
    // 16). Reserving two physical lanes also leaves headroom for the
    // audio/WASAPI thread and OS. SVMS_RENDER_THREADS overrides this.
    return cores > 2u ? (std::min)(16u, cores - 2u) : 1u;
}

static uint32_t SelectAutomaticRenderThreadCount() {
#if defined(SVMS_XP_COMPAT)
    return 1u;
#else
    // Prefer one renderer lane per physical core. On hybrid processors the
    // CPU-set efficiency class lets us select only the fastest core class;
    // older systems fall back to the physical-core topology, then to a
    // conservative logical-processor count.
    using GetCpuSetsProc = BOOL (WINAPI*)(
        PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    GetCpuSetsProc getCpuSets = kernel32
        ? reinterpret_cast<GetCpuSetsProc>(
              GetProcAddress(kernel32, "GetSystemCpuSetInformation"))
        : nullptr;
    if (getCpuSets) {
        ULONG bytes = 0u;
        getCpuSets(nullptr, 0u, &bytes, nullptr, 0u);
        unsigned char* storage = bytes != 0u
            ? static_cast<unsigned char*>(std::malloc(bytes)) : nullptr;
        if (storage && getCpuSets(
                reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage), bytes,
                &bytes, nullptr, 0u)) {
            BYTE fastestClass = 0u;
            for (ULONG offset = 0u; offset < bytes;) {
                const auto* info = reinterpret_cast<
                    const SYSTEM_CPU_SET_INFORMATION*>(storage + offset);
                if (info->Size == 0u || offset + info->Size > bytes) break;
                if (info->Type == CpuSetInformation)
                    fastestClass = (std::max)(
                        fastestClass, info->CpuSet.EfficiencyClass);
                offset += info->Size;
            }
            uint16_t cores[64]{};
            uint32_t coreCount = 0u;
            for (ULONG offset = 0u; offset < bytes && coreCount < 16u;) {
                const auto* info = reinterpret_cast<
                    const SYSTEM_CPU_SET_INFORMATION*>(storage + offset);
                if (info->Size == 0u || offset + info->Size > bytes) break;
                if (info->Type == CpuSetInformation &&
                    info->CpuSet.EfficiencyClass == fastestClass) {
                    const uint16_t key = static_cast<uint16_t>(
                        (static_cast<uint16_t>(info->CpuSet.Group) << 8u) |
                        info->CpuSet.CoreIndex);
                    bool found = false;
                    for (uint32_t index = 0u; index < coreCount; ++index)
                        found |= cores[index] == key;
                    if (!found) cores[coreCount++] = key;
                }
                offset += info->Size;
            }
        std::free(storage);
        if (coreCount != 0u)
            return SelectRenderLanesForPhysicalCores(coreCount);
        } else {
            std::free(storage);
        }
    }

    DWORD bytes = 0u;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    auto* topology = bytes != 0u
        ? static_cast<unsigned char*>(std::malloc(bytes)) : nullptr;
    if (topology && GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                topology), &bytes)) {
        uint32_t cores = 0u;
        for (DWORD offset = 0u; offset < bytes;) {
            const auto* info = reinterpret_cast<
                const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                    topology + offset);
            if (info->Size == 0u || offset + info->Size > bytes) break;
            if (info->Relationship == RelationProcessorCore) ++cores;
            offset += info->Size;
        }
        std::free(topology);
        if (cores != 0u)
            return SelectRenderLanesForPhysicalCores(
                (std::min)(16u, cores));
    } else {
        std::free(topology);
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    return (std::max)(1u, (std::min)(8u,
        static_cast<uint32_t>(systemInfo.dwNumberOfProcessors)));
#endif
}

static size_t EstimateRuntimeVoiceMemoryBytes(
    uint32_t voiceCapacity, uint32_t renderThreads,
    uint32_t maximumBlockFrames) noexcept {
    const size_t manager = VoiceManager::EstimateAllocatedBytes(voiceCapacity);
    const size_t renderer = RenderScalar::EstimateAllocatedBytes(
        voiceCapacity, renderThreads, maximumBlockFrames);
    if (manager == (std::numeric_limits<size_t>::max)() ||
        renderer > (std::numeric_limits<size_t>::max)() - manager)
        return (std::numeric_limits<size_t>::max)();
    return manager + renderer;
}

static uint32_t LargestVoiceCapacityInRange(
    uint64_t budgetBytes, uint32_t renderThreads, uint32_t blockFrames,
    uint32_t first, uint32_t last) noexcept {
    if (first > last ||
        EstimateRuntimeVoiceMemoryBytes(first, renderThreads, blockFrames) >
            budgetBytes)
        return 0u;
    uint32_t low = first;
    uint32_t high = last;
    while (low < high) {
        const uint32_t middle = low + (high - low + 1u) / 2u;
        if (EstimateRuntimeVoiceMemoryBytes(
                middle, renderThreads, blockFrames) <= budgetBytes)
            low = middle;
        else
            high = middle - 1u;
    }
    return low;
}

static uint32_t LargestInitialVoiceCapacityForBudget(
    uint64_t budgetBytes, uint32_t renderThreads,
    uint32_t blockFrames) noexcept {
    // Dense-planner storage exists only through 8,192 voices, so the estimate
    // has one intentional downward step at 8,193. Search both monotonic ranges
    // instead of assuming a globally monotonic function.
    const uint32_t highRange = LargestVoiceCapacityInRange(
        budgetBytes, renderThreads, blockFrames,
        kDenseRenderMaximumVoices + 1u, kMaxPolyphony);
    if (highRange != 0u) return highRange;
    return LargestVoiceCapacityInRange(
        budgetBytes, renderThreads, blockFrames, 1u,
        kDenseRenderMaximumVoices);
}

static size_t EstimateRuntimeVoiceMemoryAfterGrowth(
    uint32_t initialCapacity, uint32_t grownCapacity,
    uint32_t renderThreads, uint32_t maximumBlockFrames) noexcept {
    if (grownCapacity < initialCapacity) grownCapacity = initialCapacity;
    const size_t manager = VoiceManager::EstimateAllocatedBytes(grownCapacity);
    const size_t initialRenderer = RenderScalar::EstimateAllocatedBytes(
        initialCapacity, renderThreads, maximumBlockFrames);
    const size_t initialSerialRenderer = RenderScalar::EstimateAllocatedBytes(
        initialCapacity, 1u, maximumBlockFrames);
    const size_t grownSerialRenderer = RenderScalar::EstimateAllocatedBytes(
        grownCapacity, 1u, maximumBlockFrames);
    const size_t scratchGrowth = grownSerialRenderer >= initialSerialRenderer
        ? grownSerialRenderer - initialSerialRenderer : 0u;
    if (initialRenderer > (std::numeric_limits<size_t>::max)() - manager ||
        scratchGrowth > (std::numeric_limits<size_t>::max)() -
                            manager - initialRenderer)
        return (std::numeric_limits<size_t>::max)();
    return manager + initialRenderer + scratchGrowth;
}

static uint32_t LargestGrowthCapacityForBudget(
    uint64_t budgetBytes, uint32_t initialCapacity,
    uint32_t renderThreads, uint32_t blockFrames) noexcept {
    if (EstimateRuntimeVoiceMemoryAfterGrowth(
            initialCapacity, initialCapacity, renderThreads, blockFrames) >
        budgetBytes)
        return 0u;
    uint32_t low = initialCapacity;
    uint32_t high = kMaxPolyphony;
    while (low < high) {
        const uint32_t middle = low + (high - low + 1u) / 2u;
        if (EstimateRuntimeVoiceMemoryAfterGrowth(
                initialCapacity, middle, renderThreads, blockFrames) <=
            budgetBytes)
            low = middle;
        else
            high = middle - 1u;
    }
    return low;
}

static bool UsesXPWaveOut(const AudioOutput* output) {
#if defined(SVMS_XP_COMPAT)
    return output && output->IsWaveOutFallback();
#else
    (void)output;
    return false;
#endif
}

// WaitOnAddress is available on Windows 8 and newer, but some older Windows
// SDK import libraries do not expose the API-set forwarding symbols. Resolve
// it once during engine initialization so the audio callback never invokes
// the loader. The compatibility fallback merely yields; cancellation is still
// observed on every retry.
using WaitOnAddressProc = BOOL (WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressAllProc = VOID (WINAPI*)(PVOID);
static WaitOnAddressProc g_waitOnAddress = nullptr;
static WakeByAddressAllProc g_wakeByAddressAll = nullptr;

static void ResolveAddressWaitApi() noexcept {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return;
    g_waitOnAddress = reinterpret_cast<WaitOnAddressProc>(
        GetProcAddress(kernel32, "WaitOnAddress"));
    g_wakeByAddressAll = reinterpret_cast<WakeByAddressAllProc>(
        GetProcAddress(kernel32, "WakeByAddressAll"));
}

static void WaitForAddressChange(std::atomic<uint32_t>& address,
                                 uint32_t observed) noexcept {
    if (g_waitOnAddress) {
        g_waitOnAddress(reinterpret_cast<volatile VOID*>(&address),
                        &observed, sizeof(observed), 50);
    } else {
        // Windows 7 and some Wine configurations do not expose
        // WaitOnAddress. Avoid turning an idle compiler/backpressure waiter
        // into a full-core spin loop on those systems.
        Sleep(1);
    }
}

static void WakeAddressWaiters(std::atomic<uint32_t>& address) noexcept {
    if (g_wakeByAddressAll) {
        g_wakeByAddressAll(reinterpret_cast<PVOID>(&address));
    }
}

static void PublishTerminationFence(std::atomic<uint64_t>& destination,
                                    uint32_t sequence) noexcept {
    const uint64_t encoded = static_cast<uint64_t>(sequence) + 1u;
    uint64_t observed = destination.load(std::memory_order_relaxed);
    for (;;) {
        if (observed != 0u) {
            const uint32_t current = static_cast<uint32_t>(observed - 1u);
            if (!SequenceAtOrBefore(current, sequence)) return;
        }
        if (destination.compare_exchange_weak(observed, encoded,
                std::memory_order_release, std::memory_order_relaxed)) {
            return;
        }
    }
}

static bool FenceSuppresses(uint32_t sequence, uint64_t encodedFence) noexcept {
    return encodedFence != 0u &&
           SequenceAtOrBefore(sequence, static_cast<uint32_t>(encodedFence - 1u));
}

// ── Velocity→gain LUT (SnappySynth pattern) ────────────────────────────
// Maps MIDI velocity 0-127 to squared gain for natural loudness perception.
// Linear mapping (vel/127) sounds thin; squared gives proper acoustic feel.
static const float g_velGainLUT[128] = {
    0.000000f, 0.000062f, 0.000248f, 0.000558f, 0.000992f, 0.001550f, 0.002232f, 0.003038f,
    0.003968f, 0.005022f, 0.006200f, 0.007501f, 0.008927f, 0.010476f, 0.012150f, 0.013947f,
    0.015868f, 0.017913f, 0.020082f, 0.022374f, 0.024791f, 0.027331f, 0.029996f, 0.032784f,
    0.035696f, 0.038732f, 0.041892f, 0.045175f, 0.048583f, 0.052114f, 0.055770f, 0.059549f,
    0.063452f, 0.067478f, 0.071629f, 0.075903f, 0.080302f, 0.084824f, 0.089470f, 0.094240f,
    0.099133f, 0.104151f, 0.109292f, 0.114558f, 0.119947f, 0.125460f, 0.131097f, 0.136857f,
    0.142742f, 0.148750f, 0.154883f, 0.161139f, 0.167519f, 0.174023f, 0.180650f, 0.187402f,
    0.194277f, 0.201277f, 0.208400f, 0.215647f, 0.223018f, 0.230512f, 0.238131f, 0.245873f,
    0.253740f, 0.261730f, 0.269844f, 0.278082f, 0.286444f, 0.294929f, 0.303539f, 0.312272f,
    0.321130f, 0.330111f, 0.339216f, 0.348445f, 0.357798f, 0.367275f, 0.376875f, 0.386599f,
    0.396448f, 0.406419f, 0.416515f, 0.426735f, 0.437078f, 0.447546f, 0.458137f, 0.468852f,
    0.479691f, 0.490654f, 0.501740f, 0.512951f, 0.524285f, 0.535743f, 0.547325f, 0.559030f,
    0.570860f, 0.582813f, 0.594890f, 0.607091f, 0.619416f, 0.631864f, 0.644437f, 0.657133f,
    0.669953f, 0.682897f, 0.695965f, 0.709156f, 0.722471f, 0.735910f, 0.749473f, 0.763160f,
    0.776970f, 0.790904f, 0.804962f, 0.819144f, 0.833449f, 0.847879f, 0.862432f, 0.877109f,
    0.891909f, 0.906834f, 0.921882f, 0.937054f, 0.952350f, 0.967770f, 0.983313f, 1.000000f,
};

struct ReverbState {
    static constexpr uint32_t kFdnLines = 8u;
    static constexpr uint32_t kInputDiffusers = 2u;
    static constexpr uint32_t kEarlyTapCount = 6u;

    // Power-of-two ring buffers.
    //
    // 65536 frames gives:
    //   1.486 s @ 44.1 kHz
    //   341 ms @ 192 kHz
    //   171 ms @ 384 kHz
    //
    // Plenty for the FDN delays used here.
    static constexpr uint32_t kFdnBufferFrames = 65536u;
    static constexpr uint32_t kFdnBufferMask = kFdnBufferFrames - 1u;

    static constexpr uint32_t kEarlyBufferFrames = 65536u;
    static constexpr uint32_t kEarlyBufferMask = kEarlyBufferFrames - 1u;

    // Input diffusers only need short delays.
    static constexpr uint32_t kAllpassBufferFrames = 8192u;

    // 200 ms at 384 kHz = 76800 samples.
    // Use power-of-two buffer for cheap wrapping.
    static constexpr uint32_t kPreDelayBufferFrames = 131072u;
    static constexpr uint32_t kPreDelayBufferMask =
        kPreDelayBufferFrames - 1u;

    static constexpr float kPi =
        3.14159265358979323846f;

    static constexpr float kInvSqrt2 =
        0.7071067811865475244f;

    static constexpr float kInvSqrt8 =
        0.3535533905932737622f;

    // Prevent the reverb from becoming a rumour again.
    static constexpr float kFdnInputGain = 0.42f;
    static constexpr float kEarlyOutputGain = 0.60f;

    static_assert(
        (kFdnBufferFrames & (kFdnBufferFrames - 1u)) == 0u);

    static_assert(
        (kEarlyBufferFrames & (kEarlyBufferFrames - 1u)) == 0u);

    static_assert(
        (kPreDelayBufferFrames & (kPreDelayBufferFrames - 1u)) == 0u);

    // =====================================================================
    // FDN
    // =====================================================================

    alignas(64)
    float fdnDelay[kFdnLines][kFdnBufferFrames]{};

    uint32_t fdnWritePosition = 0u;

    float fdnBaseDelayFrames[kFdnLines]{};
    float fdnModDepthFrames[kFdnLines]{};
    float fdnFeedback[kFdnLines]{};

    // One-pole damping state for each feedback path.
    float fdnDampingState[kFdnLines]{};

    // =====================================================================
    // FDN modulation
    //
    // Recursive sine oscillator:
    //
    // sin(a+b) = sin(a)cos(b) + cos(a)sin(b)
    // cos(a+b) = cos(a)cos(b) - sin(a)sin(b)
    //
    // No sin() calls in the sample loop.
    // =====================================================================

    float lfoSin[kFdnLines]{};
    float lfoCos[kFdnLines]{};

    float lfoSinIncrement[kFdnLines]{};
    float lfoCosIncrement[kFdnLines]{};

    // =====================================================================
    // Input diffusion
    // =====================================================================

    alignas(64)
    float allpassDelay[2][kInputDiffusers][kAllpassBufferFrames]{};

    uint32_t allpassPosition[2][kInputDiffusers]{};
    uint32_t allpassLength[2][kInputDiffusers]{
        {1u, 1u},
        {1u, 1u}
    };

    float allpassFeedback = 0.60f;

    // =====================================================================
    // Predelay
    // =====================================================================

    alignas(64)
    float preDelay[2][kPreDelayBufferFrames]{};

    uint32_t preDelayPosition = 0u;
    uint32_t preDelayLength = 0u;

    // =====================================================================
    // Early reflections
    // =====================================================================

    alignas(64)
    float earlyDelay[2][kEarlyBufferFrames]{};

    uint32_t earlyPosition = 0u;

    uint32_t earlyTapFrames[kEarlyTapCount][2]{};

    // =====================================================================
    // Wet output filtering
    // =====================================================================

    float outputHighpassState[2]{};
    float outputLowpassState[2]{};

    float outputHighpassAlpha = 0.0f;
    float outputLowpassAlpha = 1.0f;

    // Feedback damping coefficient.
    float dampingAlpha = 1.0f;

    // =====================================================================
    // User parameters
    // =====================================================================

    bool enabled = false;

    float mix = 0.25f;

    float roomSize = 0.60f;
    float decay = 0.50f;
    float damping = 0.35f;
    float width = 1.0f;

    float diffusion = 0.70f;
    float preDelayMs = 12.0f;

    float earlyLevel = 0.35f;
    float lateLevel = 0.85f;

    float modDepth = 0.30f;
    float modRate = 0.35f;

    float lowCutHz = 70.0f;
    float highCutHz = 16000.0f;

    float decaySeconds = 4.6875f;

    uint32_t configuredSampleRate = 44100u;

    // =====================================================================
    // Live-change morph targets
    //
    // Delay lengths never jump: UpdateDerived() writes the *target* values
    // here, and Process() glides the current values toward them by a small
    // per-sample step.  A length step on a ring buffer is otherwise a
    // splice (read-position jump = click); the glide turns it into a brief
    // pitch/behaviour wobble instead.  Coefficients (feedback, damping,
    // filter alphas, gains) apply instantly — a coefficient step is a
    // smooth amplitude/tone transition on material already in the lines.
    // =====================================================================

    uint32_t preDelayLengthTarget = 0u;
    uint32_t allpassLengthTarget[2][kInputDiffusers]{
        {1u, 1u},
        {1u, 1u}
    };
    uint32_t earlyTapFramesTarget[kEarlyTapCount][2]{};
    float fdnBaseDelayFramesTarget[kFdnLines]{};

    // True while any delay-length current differs from its target.
    // UpdateDerived() arms it (it runs only when live reverb parameters
    // actually changed); Process() clears it once every glide lands.
    bool gliding_ = false;

    static uint32_t GlideU32(uint32_t current, uint32_t target,
                             uint32_t step) noexcept {
        if (current < target) {
            const uint32_t next = current + step;
            return next > target ? target : next;
        }
        if (current > target) {
            const uint32_t next = current - step;
            return next < target ? target : next;
        }
        return current;
    }

    static float GlideF32(float current, float target, float step) noexcept {
        if (current < target) return (std::min)(target, current + step);
        if (current > target) return (std::max)(target, current - step);
        return current;
    }

    // =====================================================================
    // Helpers
    // =====================================================================

    static float Clamp(
        float value,
        float minimum,
        float maximum) noexcept
    {
        return (std::max)(
            minimum,
            (std::min)(maximum, value));
    }

    static float Clamp01(float value) noexcept {
        return Clamp(value, 0.0f, 1.0f);
    }

    static uint32_t MsToFrames(
        float milliseconds,
        uint32_t sampleRate,
        uint32_t maxFrames) noexcept
    {
        const float frames =
            milliseconds *
            0.001f *
            static_cast<float>(sampleRate);

        return (std::min)(
            maxFrames,
            (std::max)(
                1u,
                static_cast<uint32_t>(frames + 0.5f)));
    }

    static float OnePoleAlpha(
        float cutoffHz,
        uint32_t sampleRate) noexcept
    {
        const float sr =
            static_cast<float>(sampleRate);

        cutoffHz =
            Clamp(
                cutoffHz,
                1.0f,
                sr * 0.45f);

        return
            1.0f -
            std::exp(
                -2.0f *
                kPi *
                cutoffHz /
                sr);
    }

    static float ComputeFeedback(
        float delayFrames,
        uint32_t sampleRate,
        float rt60Seconds) noexcept
    {
        const float delaySeconds =
            delayFrames /
            static_cast<float>(sampleRate);

        // -60 dB after RT60.
        const float feedback =
            std::pow(
                10.0f,
                -3.0f *
                delaySeconds /
                rt60Seconds);

        return Clamp(
            feedback,
            0.0f,
            0.9997f);
    }

    // ---------------------------------------------------------------------
    // Fractional FDN read with linear interpolation.
    // ---------------------------------------------------------------------

    float ReadFdnDelay(
        uint32_t line,
        float delayFrames) const noexcept
    {
        delayFrames =
            Clamp(
                delayFrames,
                1.0f,
                static_cast<float>(
                    kFdnBufferFrames - 3u));

        const uint32_t whole =
            static_cast<uint32_t>(delayFrames);

        const float fraction =
            delayFrames -
            static_cast<float>(whole);

        const uint32_t index0 =
            (fdnWritePosition - whole) &
            kFdnBufferMask;

        const uint32_t index1 =
            (index0 - 1u) &
            kFdnBufferMask;

        const float a =
            fdnDelay[line][index0];

        const float b =
            fdnDelay[line][index1];

        return
            a +
            (b - a) *
            fraction;
    }

    // ---------------------------------------------------------------------
    // Proper Schroeder allpass diffuser.
    // ---------------------------------------------------------------------

    float ProcessAllpass(
        float input,
        uint32_t channel,
        uint32_t stage) noexcept
    {
        const uint32_t p =
            allpassPosition[channel][stage];

        const float delayed =
            allpassDelay[channel][stage][p];

        const float output =
            delayed -
            allpassFeedback *
            input;

        allpassDelay[channel][stage][p] =
            input +
            allpassFeedback *
            output;

        allpassPosition[channel][stage] =
            (p + 1u ==
             allpassLength[channel][stage])
                ? 0u
                : p + 1u;

        // Safety wrap: if the length ever lands below the advancing
        // position (shrinking lengths are glided, but a stale position
        // from a reparameterization would otherwise walk past the wrap
        // condition and read out of the ring until the buffer edge).
        if (allpassPosition[channel][stage] >= kAllpassBufferFrames)
            allpassPosition[channel][stage] = 0u;

        return output;
    }

    // =====================================================================
    // Reset
    // =====================================================================

    void Reset() noexcept {
        std::memset(
            fdnDelay,
            0,
            sizeof(fdnDelay));

        std::memset(
            fdnDampingState,
            0,
            sizeof(fdnDampingState));

        fdnWritePosition = 0u;

        // Seed the FDN LFOs with their spread initial phases.  Live
        // reparameterizations preserve the phase (UpdateDerived only
        // rewrites the increments); Reset is the init/reload point where
        // a fresh phase is wanted.
        static constexpr float kInitialPhase[kFdnLines] =
        {
            0.03f,
            0.17f,
            0.31f,
            0.46f,
            0.59f,
            0.71f,
            0.83f,
            0.94f
        };
        for (uint32_t i = 0u; i < kFdnLines; ++i) {
            const float angle = 2.0f * kPi * kInitialPhase[i];
            lfoSin[i] = std::sin(angle);
            lfoCos[i] = std::cos(angle);
        }

        std::memset(
            allpassDelay,
            0,
            sizeof(allpassDelay));

        std::memset(
            allpassPosition,
            0,
            sizeof(allpassPosition));

        std::memset(
            preDelay,
            0,
            sizeof(preDelay));

        preDelayPosition = 0u;

        std::memset(
            earlyDelay,
            0,
            sizeof(earlyDelay));

        earlyPosition = 0u;

        std::memset(
            outputHighpassState,
            0,
            sizeof(outputHighpassState));

        std::memset(
            outputLowpassState,
            0,
            sizeof(outputLowpassState));
    }

    // =====================================================================
    // Configure
    // =====================================================================

    void Configure(
        uint32_t sampleRate,
        const EngineConfig& cfg) noexcept
    {
        configuredSampleRate =
            (std::max)(1u, sampleRate);

        enabled =
            cfg.enableReverb;

        mix =
            Clamp01(cfg.reverbMix);

        roomSize =
            Clamp01(cfg.reverbRoomSize);

        decay =
            Clamp01(cfg.reverbDecay);

        damping =
            Clamp01(cfg.reverbDamping);

        width =
            Clamp01(cfg.reverbWidth);

        diffusion =
            Clamp01(cfg.reverbDiffusion);

        preDelayMs =
            Clamp(
                cfg.reverbPreDelayMs,
                0.0f,
                200.0f);

        earlyLevel =
            Clamp(
                cfg.reverbEarlyLevel,
                0.0f,
                1.5f);

        lateLevel =
            Clamp(
                cfg.reverbLateLevel,
                0.0f,
                1.5f);

        modDepth =
            Clamp01(cfg.reverbModDepth);

        modRate =
            Clamp01(cfg.reverbModRate);

        lowCutHz =
            Clamp(
                cfg.reverbLowCutHz,
                0.0f,
                2000.0f);

        highCutHz =
            Clamp(
                cfg.reverbHighCutHz,
                1000.0f,
                static_cast<float>(
                    configuredSampleRate) *
                    0.45f);

        // Derived coefficients (FDN delays, feedback, taps, filters,
        // LFOs) from the raw fields.  UpdateDerived never clears the
        // delay lines, so live parameter changes morph the tail in
        // place instead of cutting it dead.  Reset() here zeroes
        // everything for the init/reload path only.
        UpdateDerived();
        Reset();
    }

    // =====================================================================
    // UpdateDerived
    // =====================================================================
    //
    // Recomputes all derived coefficients (FDN delay lengths, feedback,
    // damping, diffuser/allpass lengths, predelay/early taps, wet EQ and
    // LFO increments) from the current raw fields.  Deliberately does NOT
    // clear any delay line or state: the audio thread calls this once per
    // block after folding live reverb parameters from the mailbox, so
    // knob moves morph the existing tail instead of cutting it.
    void UpdateDerived() noexcept
    {

        // =============================================================
        // RT60
        //
        // 0.0 -> 0.25 sec
        // 0.5 -> 4.69 sec
        // 0.7 -> 8.95 sec
        // 0.8 -> 11.61 sec
        // 1.0 -> 18 sec
        //
        // This is intentionally capable of being stupidly huge.
        // =============================================================

        decaySeconds =
            0.25f +
            decay *
            decay *
            17.75f;

        // =============================================================
        // FDN delay lengths
        // =============================================================

        static constexpr float
            kBaseDelayMs[kFdnLines] =
        {
            29.7f,
            37.1f,
            41.1f,
            43.7f,
            53.3f,
            59.9f,
            67.7f,
            73.9f
        };

        // Non-linear mapping:
        //
        // low room_size still gives useful small/medium rooms,
        // upper end expands aggressively into hall/cathedral territory.
        const float roomScale =
            0.60f +
            std::pow(
                roomSize,
                1.35f) *
            1.50f;

        // Maximum modulation is only a few milliseconds.
        //
        // Enough to break stationary resonances without becoming chorus.
        const float modulationMs =
            modDepth *
            modDepth *
            2.8f;

        static constexpr float
            kModDepthMultiplier[kFdnLines] =
        {
            0.73f,
            0.89f,
            1.07f,
            0.81f,
            1.17f,
            0.94f,
            1.11f,
            0.77f
        };

        for (uint32_t i = 0u;
             i < kFdnLines;
             ++i)
        {
            float baseFrames =
                kBaseDelayMs[i] *
                roomScale *
                0.001f *
                static_cast<float>(
                    configuredSampleRate);

            const float modFrames =
                modulationMs *
                kModDepthMultiplier[i] *
                0.001f *
                static_cast<float>(
                    configuredSampleRate);

            // Keep enough safety margin for interpolation +
            // modulation.
            const float maximumBase =
                static_cast<float>(
                    kFdnBufferFrames - 4u) -
                modFrames;

            baseFrames =
                Clamp(
                    baseFrames,
                    2.0f,
                    maximumBase);

            fdnBaseDelayFramesTarget[i] =
                baseFrames;

            // Snap the current length on the first update (and on
            // Configure/Reset, where the lines are empty anyway); later
            // live changes are glided by Process().
            if (fdnBaseDelayFrames[i] == 0.0f)
                fdnBaseDelayFrames[i] = baseFrames;

            fdnModDepthFrames[i] =
                modFrames;

            fdnFeedback[i] =
                ComputeFeedback(
                    baseFrames,
                    configuredSampleRate,
                    decaySeconds);
        }

        // =============================================================
        // Feedback damping
        //
        // damping = 0 -> bright tail
        // damping = 1 -> heavily damped tail
        //
        // Logarithmic cutoff mapping.
        // =============================================================

        constexpr float brightCutoff =
            18000.0f;

        constexpr float darkCutoff =
            1400.0f;

        const float dampingCutoff =
            brightCutoff *
            std::pow(
                darkCutoff /
                brightCutoff,
                damping);

        dampingAlpha =
            OnePoleAlpha(
                dampingCutoff,
                configuredSampleRate);

        // =============================================================
        // Input diffusion
        // =============================================================

        static constexpr float
            kDiffuserMs[kInputDiffusers] =
        {
            3.1f,
            7.7f
        };

        static constexpr float
            kRightOffsetMs[kInputDiffusers] =
        {
            0.43f,
            0.71f
        };

        const float diffuserScale =
            0.85f +
            roomSize *
            0.55f;

        for (uint32_t i = 0u;
             i < kInputDiffusers;
             ++i)
        {
            const uint32_t targetL =
                MsToFrames(
                    kDiffuserMs[i] *
                    diffuserScale,
                    configuredSampleRate,
                    kAllpassBufferFrames);

            const uint32_t targetR =
                MsToFrames(
                    (kDiffuserMs[i] +
                     kRightOffsetMs[i]) *
                    diffuserScale,
                    configuredSampleRate,
                    kAllpassBufferFrames);

            allpassLengthTarget[0][i] =
                targetL;
            allpassLengthTarget[1][i] =
                targetR;

            // First-time snap; live changes are glided in Process().
            if (allpassLength[0][i] == 0u)
                allpassLength[0][i] = targetL;
            if (allpassLength[1][i] == 0u)
                allpassLength[1][i] = targetR;

            // Ring-cursor normalization: a position past the (possibly
            // shrunken) length would never hit the wrap condition and
            // would read stale or out-of-wrap content until it wrapped
            // at the buffer edge; ProcessAllpass now also wraps at the
            // buffer edge, so clamp here for the warm-up case only.
            for (uint32_t ch = 0u; ch < 2u; ++ch) {
                const uint32_t len = allpassLength[ch][i];
                allpassPosition[ch][i] =
                    (std::min)(allpassPosition[ch][i], len != 0u ? len - 1u : 0u);
            }
        }

        allpassFeedback =
            0.22f +
            diffusion *
            0.53f;

        // =============================================================
        // Predelay
        // =============================================================

        if (preDelayMs <= 0.0f) {
            preDelayLengthTarget = 0u;
        }
        else {
            preDelayLengthTarget =
                MsToFrames(
                    preDelayMs,
                    configuredSampleRate,
                    kPreDelayBufferFrames - 1u);
        }

        // First-time snap (glided on live changes by Process()).
        preDelayLength =
            preDelayLengthTarget == 0u
                ? 0u
                : (preDelayLength != 0u
                       ? preDelayLength
                       : preDelayLengthTarget);

        // =============================================================
        // Early reflections
        // =============================================================

        static constexpr float
            kEarlyMs[kEarlyTapCount] =
        {
             7.1f,
            11.9f,
            18.3f,
            27.1f,
            39.7f,
            56.9f
        };

        static constexpr float
            kEarlyStereoOffsetMs[kEarlyTapCount] =
        {
             0.71f,
            -0.93f,
             1.27f,
            -1.53f,
             2.11f,
            -2.37f
        };

        const float earlyScale =
            0.70f +
            roomSize *
            0.90f;

        for (uint32_t i = 0u;
             i < kEarlyTapCount;
             ++i)
        {
            const uint32_t target0 =
                MsToFrames(
                    kEarlyMs[i] *
                    earlyScale,
                    configuredSampleRate,
                    kEarlyBufferFrames - 1u);

            const uint32_t target1 =
                MsToFrames(
                    (kEarlyMs[i] +
                     kEarlyStereoOffsetMs[i]) *
                    earlyScale,
                    configuredSampleRate,
                    kEarlyBufferFrames - 1u);

            earlyTapFramesTarget[i][0] =
                target0;
            earlyTapFramesTarget[i][1] =
                target1;

            // First-time snap; live changes are glided in Process().
            if (earlyTapFrames[i][0] == 0u)
                earlyTapFrames[i][0] = target0;
            if (earlyTapFrames[i][1] == 0u)
                earlyTapFrames[i][1] = target1;
        }

        // =============================================================
        // Wet EQ
        // =============================================================

        if (lowCutHz <= 0.0f) {
            outputHighpassAlpha =
                0.0f;
        }
        else {
            outputHighpassAlpha =
                OnePoleAlpha(
                    lowCutHz,
                    configuredSampleRate);
        }

        outputLowpassAlpha =
            OnePoleAlpha(
                highCutHz,
                configuredSampleRate);

        // =============================================================
        // LFO setup
        // =============================================================

        static constexpr float
            kRateMultiplier[kFdnLines] =
        {
            0.79f,
            0.93f,
            1.07f,
            1.19f,
            0.71f,
            1.31f,
            0.87f,
            1.13f
        };

        // 0.07 .. ~1.0 Hz.
        const float baseRateHz =
            0.07f +
            modRate *
            modRate *
            0.93f;

        for (uint32_t i = 0u;
             i < kFdnLines;
             ++i)
        {
            // LFO PHASE IS PRESERVED across live reparameterizations:
            // only the per-sample increment is recomputed, so a modRate
            // change sweeps the existing oscillator rather than resetting
            // it (the old code re-seeded sin/cos to the initial phase on
            // every UpdateDerived call, making each knob move restart the
            // modulation pattern).
            const float rateHz =
                baseRateHz *
                kRateMultiplier[i];

            const float increment =
                2.0f *
                kPi *
                rateHz /
                static_cast<float>(
                    configuredSampleRate);

            lfoSinIncrement[i] =
                std::sin(increment);

            lfoCosIncrement[i] =
                std::cos(increment);
        }

        // Arm the per-sample glides.  UpdateDerived only runs when live
        // reverb parameters changed (or on init/reload), so this flag is
        // not a per-block cost.
        gliding_ = true;
    }

    // =====================================================================
    // Process
    // =====================================================================

    void Process(
        float* audio,
        uint32_t frames,
        uint32_t channels) noexcept
    {
        if (!audio ||
            frames == 0u ||
            channels == 0u ||
            !enabled ||
            mix <= 0.0f)
        {
            return;
        }

        const float dryGain =
            1.0f - mix;

        // =============================================================
        // Delay-length glides (click-free live parameter changes)
        //
        // UpdateDerived() writes targets; here the current lengths creep
        // toward them by one frame per sample (FDN base delays by 1/4
        // frame — the read is interpolated, so a slower creep sounds
        // smoother).  A stepped ring length would be a splice/click.
        // =============================================================
        if (gliding_) {
            gliding_ = false;
            preDelayLength = GlideU32(preDelayLength,
                                      preDelayLengthTarget, 1u);
            if (preDelayLength != preDelayLengthTarget) gliding_ = true;
            for (uint32_t ch = 0u; ch < 2u; ++ch) {
                for (uint32_t i = 0u; i < kInputDiffusers; ++i) {
                    allpassLength[ch][i] = GlideU32(
                        allpassLength[ch][i],
                        allpassLengthTarget[ch][i], 1u);
                    if (allpassLength[ch][i] != allpassLengthTarget[ch][i])
                        gliding_ = true;
                }
            }
            for (uint32_t i = 0u; i < kEarlyTapCount; ++i) {
                earlyTapFrames[i][0] = GlideU32(
                    earlyTapFrames[i][0], earlyTapFramesTarget[i][0], 1u);
                if (earlyTapFrames[i][0] != earlyTapFramesTarget[i][0])
                    gliding_ = true;
                earlyTapFrames[i][1] = GlideU32(
                    earlyTapFrames[i][1], earlyTapFramesTarget[i][1], 1u);
                if (earlyTapFrames[i][1] != earlyTapFramesTarget[i][1])
                    gliding_ = true;
            }
            for (uint32_t i = 0u; i < kFdnLines; ++i) {
                fdnBaseDelayFrames[i] = GlideF32(
                    fdnBaseDelayFrames[i],
                    fdnBaseDelayFramesTarget[i], 0.25f);
                if (fdnBaseDelayFrames[i] != fdnBaseDelayFramesTarget[i])
                    gliding_ = true;
            }
        }

        // Fixed early reflection gains.
        static constexpr float
            kEarlyGain[kEarlyTapCount] =
        {
             0.52f,
            -0.39f,
             0.31f,
            -0.25f,
             0.20f,
            -0.16f
        };

        float* frame =
            audio;

        for (uint32_t f = 0u;
             f < frames;
             ++f,
             frame += channels)
        {
            const float dryL =
                frame[0];

            const float dryR =
                channels > 1u
                    ? frame[1]
                    : dryL;

            // =========================================================
            // Predelay
            // =========================================================

            float inputL =
                dryL;

            float inputR =
                dryR;

            if (preDelayLength > 0u) {
                const uint32_t readPosition =
                    (preDelayPosition -
                     preDelayLength) &
                    kPreDelayBufferMask;

                inputL =
                    preDelay[0][readPosition];

                inputR =
                    preDelay[1][readPosition];

                preDelay[0][preDelayPosition] =
                    dryL;

                preDelay[1][preDelayPosition] =
                    dryR;

                preDelayPosition =
                    (preDelayPosition + 1u) &
                    kPreDelayBufferMask;
            }

            // =========================================================
            // Early reflections
            // =========================================================

            earlyDelay[0][earlyPosition] =
                inputL;

            earlyDelay[1][earlyPosition] =
                inputR;

            float earlyL = 0.0f;
            float earlyR = 0.0f;

            for (uint32_t i = 0u;
                 i < kEarlyTapCount;
                 ++i)
            {
                const uint32_t readL =
                    (earlyPosition -
                     earlyTapFrames[i][0]) &
                    kEarlyBufferMask;

                const uint32_t readR =
                    (earlyPosition -
                     earlyTapFrames[i][1]) &
                    kEarlyBufferMask;

                // Alternate direct/cross-channel taps.
                //
                // Cheap way of making the early field much wider and
                // less "six obvious echoes".
                if ((i & 1u) == 0u) {
                    earlyL +=
                        earlyDelay[0][readL] *
                        kEarlyGain[i];

                    earlyR +=
                        earlyDelay[1][readR] *
                        kEarlyGain[i];
                }
                else {
                    earlyL +=
                        earlyDelay[1][readL] *
                        kEarlyGain[i];

                    earlyR +=
                        earlyDelay[0][readR] *
                        kEarlyGain[i];
                }
            }

            earlyPosition =
                (earlyPosition + 1u) &
                kEarlyBufferMask;

            earlyL *=
                kEarlyOutputGain;

            earlyR *=
                kEarlyOutputGain;

            // =========================================================
            // Input diffusion
            // =========================================================

            float diffusedL =
                inputL;

            float diffusedR =
                inputR;

            for (uint32_t i = 0u;
                 i < kInputDiffusers;
                 ++i)
            {
                diffusedL =
                    ProcessAllpass(
                        diffusedL,
                        0u,
                        i);

                diffusedR =
                    ProcessAllpass(
                        diffusedR,
                        1u,
                        i);
            }

            // =========================================================
            // Read all 8 modulated FDN lines
            // =========================================================

            float delayed[kFdnLines];
            float damped[kFdnLines];

            for (uint32_t i = 0u;
                 i < kFdnLines;
                 ++i)
            {
                // Recursive oscillator update.
                const float sinValue =
                    lfoSin[i];

                const float cosValue =
                    lfoCos[i];

                const float newSin =
                    sinValue *
                        lfoCosIncrement[i] +
                    cosValue *
                        lfoSinIncrement[i];

                const float newCos =
                    cosValue *
                        lfoCosIncrement[i] -
                    sinValue *
                        lfoSinIncrement[i];

                lfoSin[i] =
                    newSin;

                lfoCos[i] =
                    newCos;

                const float modulatedDelay =
                    fdnBaseDelayFrames[i] +
                    newSin *
                    fdnModDepthFrames[i];

                delayed[i] =
                    ReadFdnDelay(
                        i,
                        modulatedDelay);

                // Frequency-dependent decay.
                fdnDampingState[i] +=
                    dampingAlpha *
                    (delayed[i] -
                     fdnDampingState[i]);

                damped[i] =
                    fdnDampingState[i];
            }

            // =========================================================
            // Late stereo decode
            //
            // Different +/- patterns produce stereo decorrelation.
            // 1/sqrt(8) keeps the level sensible.
            // =========================================================

            float lateL =
                (
                    delayed[0] +
                    delayed[1] +
                    delayed[2] -
                    delayed[3] +
                    delayed[4] -
                    delayed[5] -
                    delayed[6] +
                    delayed[7]
                ) *
                kInvSqrt8;

            float lateR =
                (
                    delayed[0] -
                    delayed[1] +
                    delayed[2] +
                    delayed[3] -
                    delayed[4] +
                    delayed[5] -
                    delayed[6] -
                    delayed[7]
                ) *
                kInvSqrt8;

            // =========================================================
            // Householder feedback matrix
            //
            // H = I - (2/N) * 11^T
            //
            // N=8 -> 2/N = 0.25.
            //
            // This is the heart of the FDN.
            //
            // Instead of an 8x8 matrix multiply:
            //
            //     sum all lines once
            //     subtract 0.25 * sum from each line
            //
            // Very cheap and energy-preserving.
            // =========================================================

            const float sum =
                damped[0] +
                damped[1] +
                damped[2] +
                damped[3] +
                damped[4] +
                damped[5] +
                damped[6] +
                damped[7];

            const float matrixCommon =
                sum *
                0.25f;

            // =========================================================
            // Stereo input injection
            // =========================================================

            const float sumInput =
                (diffusedL + diffusedR) *
                kInvSqrt2;

            const float differenceInput =
                (diffusedL - diffusedR) *
                kInvSqrt2;

            const float injection[kFdnLines] =
            {
                 diffusedL,
                 diffusedR,
                 sumInput,
                 differenceInput,
                -diffusedL,
                -diffusedR,
                -sumInput,
                -differenceInput
            };

            // =========================================================
            // Write feedback network
            // =========================================================

            for (uint32_t i = 0u;
                 i < kFdnLines;
                 ++i)
            {
                const float mixed =
                    damped[i] -
                    matrixCommon;

                fdnDelay[i][fdnWritePosition] =
                    injection[i] *
                        kFdnInputGain +
                    mixed *
                        fdnFeedback[i];
            }

            fdnWritePosition =
                (fdnWritePosition + 1u) &
                kFdnBufferMask;

            // =========================================================
            // Early + late
            // =========================================================

            float wetL =
                earlyL *
                    earlyLevel +
                lateL *
                    lateLevel;

            float wetR =
                earlyR *
                    earlyLevel +
                lateR *
                    lateLevel;

            // =========================================================
            // Wet low-cut
            // =========================================================

            if (outputHighpassAlpha > 0.0f) {
                outputHighpassState[0] +=
                    outputHighpassAlpha *
                    (wetL -
                     outputHighpassState[0]);

                outputHighpassState[1] +=
                    outputHighpassAlpha *
                    (wetR -
                     outputHighpassState[1]);

                wetL -=
                    outputHighpassState[0];

                wetR -=
                    outputHighpassState[1];
            }

            // =========================================================
            // Wet high-cut
            // =========================================================

            outputLowpassState[0] +=
                outputLowpassAlpha *
                (wetL -
                 outputLowpassState[0]);

            outputLowpassState[1] +=
                outputLowpassAlpha *
                (wetR -
                 outputLowpassState[1]);

            wetL =
                outputLowpassState[0];

            wetR =
                outputLowpassState[1];

            // =========================================================
            // Stereo width
            // =========================================================

            const float mid =
                0.5f *
                (wetL + wetR);

            const float side =
                0.5f *
                (wetL - wetR) *
                width;

            wetL =
                mid + side;

            wetR =
                mid - side;

            // =========================================================
            // Dry / wet
            // =========================================================

            frame[0] =
                dryL *
                    dryGain +
                wetL *
                    mix;

            if (channels > 1u) {
                frame[1] =
                    dryR *
                        dryGain +
                    wetR *
                        mix;
            }
        }

        // =================================================================
        // LFO drift cleanup.
        //
        // Only 8 sqrt() calls per PROCESS BLOCK, not per sample.
        // =================================================================

        for (uint32_t i = 0u;
             i < kFdnLines;
             ++i)
        {
            const float magnitudeSquared =
                lfoSin[i] *
                    lfoSin[i] +
                lfoCos[i] *
                    lfoCos[i];

            if (magnitudeSquared > 0.0f) {
                const float inverseMagnitude =
                    1.0f /
                    std::sqrt(
                        magnitudeSquared);

                lfoSin[i] *=
                    inverseMagnitude;

                lfoCos[i] *=
                    inverseMagnitude;
            }
        }

        // =================================================================
        // Denormal cleanup.
        //
        // Still preferably enable FTZ/DAZ for the audio thread globally.
        // =================================================================

        for (uint32_t i = 0u;
             i < kFdnLines;
             ++i)
        {
            if (std::fabs(
                    fdnDampingState[i]) <
                1.0e-20f)
            {
                fdnDampingState[i] =
                    0.0f;
            }
        }

        for (uint32_t c = 0u;
             c < 2u;
             ++c)
        {
            if (std::fabs(
                    outputHighpassState[c]) <
                1.0e-20f)
            {
                outputHighpassState[c] =
                    0.0f;
            }

            if (std::fabs(
                    outputLowpassState[c]) <
                1.0e-20f)
            {
                outputLowpassState[c] =
                    0.0f;
            }
        }
    }
};

using LimiterState = LimiterRouterState;

// Allocation-free rolling callback histogram. One-percent bins are precise
// enough for the diagnostic/acceptance thresholds and make percentile reads a
// bounded 201-bin scan instead of sorting on the audio thread.
struct CallbackTimingWindow {
    static constexpr uint32_t kWindowSize = 1024;
    static constexpr uint32_t kBinCount = 201; // 0..199%, 200 = 200%+

    uint16_t samples[kWindowSize]{};
    uint16_t bins[kBinCount]{};
    uint32_t cursor = 0;
    uint32_t count = 0;
    uint64_t overBudgetCallbacks = 0;
    uint32_t consecutiveOverBudget = 0;
    uint32_t maxConsecutiveOverBudget = 0;

    void Reset() noexcept { *this = CallbackTimingWindow{}; }

    void Observe(float percent) noexcept {
        uint32_t bin = percent > 0.0f ? static_cast<uint32_t>(percent + 0.5f) : 0u;
        if (bin >= kBinCount) bin = kBinCount - 1u;
        if (count == kWindowSize) {
            --bins[samples[cursor]];
        } else {
            ++count;
        }
        samples[cursor] = static_cast<uint16_t>(bin);
        ++bins[bin];
        cursor = (cursor + 1u) & (kWindowSize - 1u);

        if (percent > 100.0f) {
            ++overBudgetCallbacks;
            ++consecutiveOverBudget;
            maxConsecutiveOverBudget = (std::max)(maxConsecutiveOverBudget,
                                                   consecutiveOverBudget);
        } else {
            consecutiveOverBudget = 0;
        }
    }

    float Percentile(uint32_t numerator, uint32_t denominator) const noexcept {
        if (count == 0u) return 0.0f;
        const uint32_t rank = (count * numerator + denominator - 1u) / denominator;
        uint32_t accumulated = 0u;
        for (uint32_t bin = 0; bin < kBinCount; ++bin) {
            accumulated += bins[bin];
            if (accumulated >= rank) return static_cast<float>(bin);
        }
        return static_cast<float>(kBinCount - 1u);
    }
};

struct PreparedSF2Region;
struct SoundFontBundle;

static constexpr uint32_t kNoteRegionCacheSize = 4096u;
static constexpr uint32_t kNoteRegionCacheLayers = 8u;
static constexpr uint32_t kMaxMatchingRegions = 512u;
static_assert((kNoteRegionCacheSize & (kNoteRegionCacheSize - 1u)) == 0u,
              "note-region cache size must be a power of two");

struct alignas(64) NoteRegionCacheEntry {
    uint32_t tag;
    uint16_t count;
    uint16_t reserved;
    uint32_t regionIndices[kNoteRegionCacheLayers];
};

struct alignas(64) NoteLaunchPlanCacheEntry {
    uint32_t soundFontGeneration;
    uint32_t channelRevision;
    uint16_t presetIndex;
    uint8_t soundFontIndex;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint8_t count;
    uint8_t reserved;
    VoiceConfiguration setup[kNoteRegionCacheLayers];
};

class Driver {
public:
    static Driver& Instance();

    bool Initialize();
    void Shutdown();
    bool LoadSoundFont(const wchar_t* path);
    bool LoadConfiguredSoundFont();
    bool StartAudio();
    void ResetAllVoices();
    bool IsInitialized() const;
    void CopyDebugInfo(DriverDebugInfo& out) const;
    void CopyVoiceStatistics(SnappyVoiceStatistics& out) const;
    float GetRenderingTimeMilliseconds() const;
    const LegacyDriverDebugInfo* GetLegacyDebugInfo() const;

    void SubmitShortMsg(uint32_t msg);
    void SubmitShortMsgAtQpc(uint32_t msg, uint64_t qpcTimestamp);
    void SubmitShortMsgAtFrame(uint32_t msg, uint64_t outputFrame);
    bool SubmitShortMsgAtQpcCancellable(
        uint32_t msg, uint64_t qpcTimestamp,
        const std::atomic<uint64_t>* externalCancellation,
        uint64_t cancellationToken);
    bool SubmitShortBatchAtQpcCancellable(
        const SVMS_ShortEvent* events, uint32_t eventCount,
        uint64_t immediateQpc,
        const std::atomic<uint64_t>* externalCancellation,
        uint64_t cancellationToken);
    bool SubmitShortMsgAtFrameCancellable(
        uint32_t msg, uint64_t outputFrame,
        const std::atomic<uint64_t>* externalCancellation,
        uint64_t cancellationToken);
    void WakeBlockedProducers();
    void SubmitSystemExclusive(const uint8_t* data, uint32_t size);
    bool SubmitSystemExclusiveCancellable(
        const uint8_t* data, uint32_t size,
        const std::atomic<uint64_t>* externalCancellation,
        uint64_t cancellationToken);
    void SetIngressMode(EventOverflowMode mode);
    // Same-key note-on coalescing spawn interval (power-of-two rounded;
    // values below 2 disable coalescing, which is the default state).
    // Runtime-tunable. The collapse window itself is a fixed QPC time
    // span (kNoteOnCollapseWindowMs), never a render block, so collapsing
    // behavior is identical at any WASAPI buffer size.
    void SetNoteOnCollapseThreshold(uint32_t threshold);
    // Enables coalescing with the default threshold (32) or disables it
    // entirely (default state: every note-on spawns).
    void EnableNoteOnCollapse(bool enable);
    void CopyNativeQueueInfo(SVMS_QueueInfo& out) const;
    uint64_t GetNextOutputFrame() const;

    bool initialized;
    uint32_t sampleRate;
    uint32_t bufferFrames;

private:
    Driver();
    ~Driver();

    static void RenderCallback(float* output, uint32_t numFrames, void* userData);

    // EventDispatcher callback — invoked by RenderScalar at each event's
    // exact integer output frame during RenderBlock.
    static void DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                     void* userData);
    static void DispatchRenderEventBatch(const RenderEvent* events,
                                         uint32_t eventCount,
                                         uint32_t blockCursor, void* userData);

    uint64_t HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity,
                          bool deferLifetimeCounters = false,
                          const NoteLaunchPlanCacheEntry* exactFramePlan = nullptr);
    void HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset);
    void HandleStaleNoteOffBatch(uint8_t channel, uint8_t note, uint8_t count,
                                 uint32_t blockOffset);
    void HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value,
                             uint32_t blockOffset);
    void HandleProgramChange(uint8_t channel, uint8_t program);
    void HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb);
    void HandleChannelPressure(uint8_t channel, uint8_t value);
    void RefreshAllPitchIncrements();
    uint32_t RefreshVelocityCutoff(EventLane lane) noexcept;
    void EventCompilerLoop();
    SoundFontBundle* BuildSoundFontBundle(const wchar_t* path,
                                          uint64_t requestId,
                                          std::string& error);
    SoundFontBundle* BuildSoundFontStackBundle(
        const std::vector<std::wstring>& paths,
        const std::vector<SoundFontRoute>& routes,
        uint64_t requestId, std::string& error);
    void PublishSoundFontBundle(SoundFontBundle* bundle) noexcept;
    void ActivatePendingSoundFontAtBlockBoundary() noexcept;
    void RetireSoundFontBundle(SoundFontBundle* bundle) noexcept;
    void ReclaimRetiredSoundFonts() noexcept;
    void DestroyAllSoundFontBundles() noexcept;
    std::wstring CopyActiveSoundFontPath() const;
    bool StartConfiguredMidiInput();
    void StopConfiguredMidiInput() noexcept;
    static void CALLBACK MidiInputCallback(HMIDIIN input, UINT message,
                                           DWORD_PTR instance,
                                           DWORD_PTR parameter1,
                                           DWORD_PTR parameter2);
#if !defined(SVMS_XP_COMPAT)
    bool QueueSoundFontLoad(const std::wstring& path, uint64_t& requestId);
    void SoundFontLoaderLoop();
#endif
    uint32_t ResolveNoteRegions(const SoundFontBundle* bank,
                                uint8_t soundFontIndex,
                                uint32_t presetIndex, uint8_t note,
                                uint8_t velocity,
                                const SFSampleRegion** outRegions,
                                uint32_t outCapacity);
    void RefreshSelectedPresets();

    PriorityEventIngress<TimestampedMidiEvent> midiIngress_;
    CompiledEventPagePool compiledPages_;
    PagedEventScheduler pagedScheduler_;
    EventScheduler eventScheduler_;
    std::atomic<EventOverflowMode> overflowMode_;
    bool correctnessMode_;
    uint32_t highPriorityVelocity_;
    uint32_t shedStartPercent_;
    uint32_t maxEventsPerBlock_;
    bool diagnosticsEnabled_;
    bool diagnosticsWindow_;
    bool diagnosticsDebugOutput_;
    std::atomic<uint32_t> nextEventSequence_;
    // A state event can overtake older note-ons held in another priority
    // lane.  These producer-published fences prevent those stale note-ons
    // from sounding after a reset or per-channel termination controller.
    std::atomic<uint64_t> globalTerminationFence_;
    std::atomic<uint64_t> channelTerminationFence_[kChannelCount];
    std::atomic<bool> cancelProducers_;
    std::atomic<uint32_t> producerWakeEpoch_;
    std::atomic<uint32_t> scheduledSizePublished_;
    std::atomic<uint64_t> submittedAtomic_;
    std::atomic<uint64_t> acceptedAtomic_;
    std::atomic<uint64_t> shedAtomic_;
    std::atomic<uint64_t> cancelledAtomic_;
    // Same-key note-on coalescing (see SVMSNoteOnCollapse.h). Collapsed
    // duplicates never reach the ingress queues, lanes, or audio thread.
    NoteOnCollapseGate noteOnCollapse_;
    std::atomic<uint64_t> coalescedAtomic_{0};
    std::atomic<uint32_t> currentVelocityCutoffAtomic_;
    std::atomic<uint64_t> compilerEpochQPC_;
    std::atomic<uint32_t> compilerWakeEpoch_;
    std::atomic<bool> compilerSleeping_;
    std::thread eventCompilerThread_;
    bool useEventCompiler_;
    std::atomic<uint64_t> shedByVelocityAtomic_[128];
    EventTelemetry telemetry_;
    LiveSF2Telemetry sf2Telemetry_;
    DriverDebugInfo debugSnapshots_[2];
    SnappyVoiceStatistics voiceStatisticsSnapshots_[2];
    LegacyDriverDebugInfo legacyDebugSnapshots_[2];
    float renderingTimeSnapshots_[2]{};
    std::atomic<uint32_t> debugSnapshotIndex_;
    uint64_t callbackCount_;
    CallbackTimingWindow callbackTiming_;
    uint64_t dispatchCyclesCurrent_ = 0u;
    bool captureSf2Detail_ = false;

    AudioOutput* audioOutput;
    VoiceManager* voiceManager;
    ChannelCache* channelCache;
    RenderScalar* renderScalar;
    SF2Data* soundFontData;
    RuntimeConfigSnapshot* configSnapshot;
    int16_t* sampleDataStore;
    SF2Sample* samplesStore;
    float* regionInitialPeaks;
    uint32_t regionInitialPeakCount;
    PreparedSF2Region* preparedRegions;
    uint32_t preparedRegionCount;
    uint32_t soundFontGeneration_;
    SoundFontBundle* activeSoundFontStack_ = nullptr;
    std::atomic<SoundFontBundle*> activeSoundFontBundle_{nullptr};
    std::atomic<SoundFontBundle*> pendingSoundFontBundle_{nullptr};
    std::atomic<SoundFontBundle*> retiredSoundFontBundles_{nullptr};
    std::atomic<uint64_t> soundFontRequestId_{0u};
    std::atomic<uint64_t> soundFontActivatedId_{0u};
#if !defined(SVMS_XP_COMPAT)
    std::thread soundFontLoaderThread_;
    HANDLE soundFontLoadEvent_ = nullptr;
    std::atomic<bool> soundFontLoaderStop_{false};
    std::wstring requestedSoundFontPath_;
    uint64_t requestedSoundFontId_ = 0u;
    std::atomic<uint32_t> soundFontLoadState_{0u};
    std::string soundFontLoadError_;
#endif
    uint32_t channelLaunchRevision_[kChannelCount];
    uint8_t channelSoundFontIndex_[kChannelCount]{};
    NoteRegionCacheEntry noteRegionCache_[kNoteRegionCacheSize];
    NoteLaunchPlanCacheEntry noteLaunchPlanCache_[kNoteRegionCacheSize];
    NoteLaunchPlanCacheEntry*
        noteLaunchHotCache_[kChannelCount][kNoteCount];
    const SFSampleRegion* noteRegionScratch_[kMaxMatchingRegions];
    VoiceConfiguration noteLaunchScratch_[kMaxMatchingRegions];
    VoiceHandle noteLaunchHandles_[kMaxMatchingRegions];
    float configuredVelocityGain_[128];
    float channelPitchBendRatio_[kChannelCount];
    uint32_t sampleStoreCount;
    uint32_t sampleDataFrames;
    uint64_t qpcFreq;

    static constexpr uint32_t kMidiInputBufferCount = 4u;
    static constexpr uint32_t kMidiInputBufferBytes = 4096u;
    HMIDIIN midiInput_ = nullptr;
    MIDIHDR midiInputHeaders_[kMidiInputBufferCount]{};
    alignas(64) char midiInputData_[kMidiInputBufferCount]
                                  [kMidiInputBufferBytes]{};
    std::atomic<bool> midiInputRunning_{false};

    float* leftBuffer;
    float* rightBuffer;
    uint32_t bufferCapacity;
    PostHighPass3Hz postHighPass;
    ReverbState reverb;
    LimiterState limiter;
#if !defined(SVMS_XP_COMPAT)
    svms::LiveWaveRecorder liveRecorder_;
#endif

    // ── Atomic live-config mailbox (seqlock) ─────────────────────────
    // The control thread is the ONLY writer.  It bumps liveMailboxSeq_
    // to ODD, stores the atomic fields, then bumps to EVEN (release).
    // The audio thread reads once per render block: if the sequence is
    // even and unchanged after the copy, the copy is torn-free; otherwise
    // it falls back to appliedMailbox_ (the last state it applied).  No
    // locks, no torn reads, no ABA (single writer, monotonically even
    // sequence values 2, 4, 6, ...).  DSP application is skipped entirely
    // when the sequence equals lastAppliedLiveSeq_, so derived
    // recomputation (reverb.UpdateDerived, limiter targets) happens only
    // when live values actually changed.
    svms::LiveConfigMailbox liveMailbox_;
    std::atomic<uint32_t> liveMailboxSeq_{2u};

    // Last master volume the audio thread actually folded into playing
    // voices' mix gains.  Audio-thread only.
    float appliedMasterVolume_ = 1.0f;
    float sysexMasterVolume_ = 1.0f;
    float sysexMasterFineTune_ = 0.0f;
    float sysexMasterTranspose_ = 0.0f;

    // Per-block dispatch queue. Allocated once during initialization from the
    // smaller of max_events_per_block and the configured scheduler capacity.
    svms::RenderEvent* eventBuffer;
    uint32_t eventBufferCapacity_;

    // When the renderer falls behind by more than one device buffer, old
    // note-ons no longer have a meaningful historical frame at which they
    // can be rendered. Keep only the newest still-on note for each MIDI
    // channel/key while draining that obsolete window. This bounded catch-up
    // set prevents overload from converging to permanent silence.
    static constexpr uint32_t kStaleRecoveryKeys = kChannelCount * kNoteCount;
    RenderEvent staleRecoveryEvents_[kStaleRecoveryKeys];
    uint8_t staleRecoveryValid_[kStaleRecoveryKeys];
    uint32_t staleRecoveryNoteOffSequence_[kStaleRecoveryKeys];
    int64_t staleRecoveryNoteOffFrame_[kStaleRecoveryKeys];
    uint32_t staleRecoveryNoteOffCount_[kStaleRecoveryKeys];
    uint8_t staleRecoveryNoteOffValid_[kStaleRecoveryKeys];

    // Exact-frame note-off transaction scratch. A late recovery callback can
    // collapse a skipped interval onto frame zero, producing hundreds of
    // thousands of interleaved note-offs. Their per-key multiplicity matters,
    // but repeated channel/key lookups between state-event boundaries do not.
    // Generation stamps avoid clearing all 2,048 entries for every run.
    uint32_t noteOffBatchStamp_[kStaleRecoveryKeys]{};
    uint32_t noteOffBatchCount_[kStaleRecoveryKeys]{};
    uint16_t noteOffBatchKeys_[kStaleRecoveryKeys]{};
    uint32_t noteOffBatchGeneration_ = 0u;

    // Persistent audio-thread-only overflow queue.  Events whose
    // sampleOffset lands beyond the current block (sampleOffset >=
    // numFrames) are kept here — NOT pushed back into the lock-free SPSC
    // queue — with their sampleOffset re-based to the next block's frame
    // of reference at block completion.  This is what lets a future event
    // roll over smoothly across block boundaries instead of snapping to
    // sample 0 of the next callback (BUG1: 100Hz buffer-grid buzzing).
    // Absolute QPC/output-frame epoch. Conversion is always made from this
    // fixed epoch, so callback rounding cannot accumulate clock drift.
    uint64_t virtualRenderClockQPC;
    int64_t virtualRenderSample_;
    std::atomic<uint64_t> outputFramePublished_{0u};
    bool clockInitialized;
    uint32_t nextPlayIndex_;
    EngineConfig engineConfig_;

    #if !defined(SVMS_XP_COMPAT)
    svms::RLResult HandleRuntimeLinkCommand(const svms::RuntimeLinkCommandV2& cmd,
                                  char* resultText);
    svms::RuntimeLinkTelemetryV2 BuildRuntimeLinkTelemetry();
#endif

    // Audio-thread record of the LAST live state it actually applied
    // (from the mailbox seqlock), plus the mailbox sequence that
    // produced it.  The control thread reads both for the telemetry
    // "applied live" echo; the release/acquire pair on appliedSeq_
    // makes the plain appliedMailbox_ copy coherent on the reader side.
    svms::NonAtomicLiveConfigMailbox appliedMailbox_;
    std::atomic<uint32_t> appliedSeq_{2u};

    // Audio-thread-only: the most recent mailbox sequence folded into
    // the render path.  The DSP apply (incl. reverb.UpdateDerived) is
    // skipped entirely when it equals the latest publish, so derived
    // recomputation happens only when live values actually changed.
    uint32_t lastAppliedLiveSeq_ = 0u;

    // Control-thread-owned telemetry echo bookkeeping: the last applied
    // sequence the control thread echoed, the sequence of the last
    // ApplyLiveConfig publish, and the cached RuntimeLiveStateV2 echoed
    // to telemetry.  Control-thread-only.
#if !defined(SVMS_XP_COMPAT)
    uint32_t lastEchoedAppliedSeq_ = 0u;
    uint32_t lastPublishedMailboxSeq_ = 2u;
    svms::RuntimeLiveStateV2 appliedLiveEcho_{};
#endif

    CRITICAL_SECTION cs;
    CRITICAL_SECTION soundFontBuildCs_;
};

static Driver* s_instance = nullptr;

// ── RuntimeLink V2 IPC (driver side) ───────────────────────────────────────
// The control thread polls the command mailbox every ~33 ms, applies
// live config changes (grouped ApplyLiveConfig), runs the reload/reset
// commands, and publishes telemetry at ~30 Hz.  The audio thread never
// touches IPC; it only updates the process-local g_audioSnapshot, which
// the control thread reads at publish time.
#if !defined(SVMS_XP_COMPAT)
static svms::RuntimeLinkDriverV2 g_rlDriver;

// Process-local audio→control snapshot.  Audio thread writes (odd/even
// sequence), control thread reads.  Never mapped into shared memory.
static svms::RuntimeAudioSnapshot g_audioSnapshot;
#endif

// ── RuntimeLink command handler (runs on control thread) ────────────────
// V2 command handling.  Live parameters arrive as ONE grouped
// ApplyLiveConfig command carrying the full RuntimeLiveStateV2 payload
// and a groupMask; the handler validates the payload, writes only the
// masked groups into the seqlock mailbox (odd sequence = in-progress,
// even = published; the audio thread reads it once per block — no locks,
// no torn reads). ReloadSoundFont only queues the dedicated loader thread;
// ResetVoices routes through the SPSC ingress exactly
// like midiOutReset so the audio thread performs the release work.
static uint32_t FloatToU32Bits(float value) noexcept {
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float U32BitsToFloat(uint32_t bits) noexcept {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

#if !defined(SVMS_XP_COMPAT)
static svms::RuntimeLiveStateV2 LiveStateFromMailbox(
    const NonAtomicLiveConfigMailbox& mb, uint32_t sampleRate) {
    svms::RuntimeLiveStateV2 l{};
    l.masterVolume = mb.masterVolume;
    l.correctnessMode = mb.correctnessMode ? 1u : 0u;
    l.reverbEnabled = mb.reverbEnabled ? 1u : 0u;
    l.reverbMix = mb.reverbMix;
    l.reverbRoomSize = mb.reverbRoomSize;
    l.reverbDecay = mb.reverbDecay;
    l.reverbDamping = mb.reverbDamping;
    l.reverbWidth = mb.reverbWidth;
    l.reverbDiffusion = mb.reverbDiffusion;
    l.reverbPreDelayMs = mb.reverbPreDelayMs;
    l.reverbEarlyLevel = mb.reverbEarlyLevel;
    l.reverbLateLevel = mb.reverbLateLevel;
    l.reverbModDepth = mb.reverbModDepth;
    l.reverbModRate = mb.reverbModRate;
    l.reverbLowCutHz = mb.reverbLowCutHz;
    l.reverbHighCutHz = mb.reverbHighCutHz;
    l.limiterEnabled = mb.limiterEnabled ? 1u : 0u;
    l.limiterAlgorithm = mb.limiterAlgorithm;
    l.limiterThreshold = mb.limiterThreshold;
    l.limiterLookaheadMs = static_cast<float>(mb.limiterDelayFrames)
                         / sampleRate * 1000.0f;
    const float attackCoeff = mb.limiterAttackCoeff;
    const float releaseCoeff = mb.limiterReleaseCoeff;
    l.limiterAttackMs = attackCoeff > 0.0f
        ? -1000.0f / (sampleRate * std::log(1.0f - attackCoeff))
        : 0.01f;
    l.limiterReleaseMs = releaseCoeff > 0.0f
        ? -1000.0f / (sampleRate * std::log(1.0f - releaseCoeff))
        : 100.0f;
    return l;
}

svms::RLResult Driver::HandleRuntimeLinkCommand(
    const svms::RuntimeLinkCommandV2& cmd, char* resultText) {
    using RT = svms::RLCommandType;
    constexpr uint32_t kText = svms::kRuntimeLinkResultTextCapacity;

    switch (static_cast<RT>(cmd.type)) {
    case RT::Ping:
        return svms::RLResult::Ok;

    case RT::ApplyLiveConfig: {
        if (cmd.groupMask == 0u) {
            strncpy_s(resultText, kText, "empty group mask", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        const svms::RuntimeLiveStateV2& l = cmd.live;
        // Reject non-finite payloads before touching the mailbox.
        if (!svms::RLV2_IsFinite(l.masterVolume) ||
            !svms::RLV2_IsFinite(l.reverbMix) ||
            !svms::RLV2_IsFinite(l.reverbRoomSize) ||
            !svms::RLV2_IsFinite(l.reverbDecay) ||
            !svms::RLV2_IsFinite(l.reverbDamping) ||
            !svms::RLV2_IsFinite(l.reverbWidth) ||
            !svms::RLV2_IsFinite(l.reverbDiffusion) ||
            !svms::RLV2_IsFinite(l.reverbPreDelayMs) ||
            !svms::RLV2_IsFinite(l.reverbEarlyLevel) ||
            !svms::RLV2_IsFinite(l.reverbLateLevel) ||
            !svms::RLV2_IsFinite(l.reverbModDepth) ||
            !svms::RLV2_IsFinite(l.reverbModRate) ||
            !svms::RLV2_IsFinite(l.reverbLowCutHz) ||
            !svms::RLV2_IsFinite(l.reverbHighCutHz) ||
            !svms::RLV2_IsFinite(l.limiterThreshold) ||
            !svms::RLV2_IsFinite(l.limiterLookaheadMs) ||
            !svms::RLV2_IsFinite(l.limiterAttackMs) ||
            !svms::RLV2_IsFinite(l.limiterReleaseMs)) {
            strncpy_s(resultText, kText, "non-finite parameter", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        if (l.correctnessMode > 1u || l.reverbEnabled > 1u ||
            l.limiterEnabled > 1u) {
            strncpy_s(resultText, kText, "boolean flags must be 0 or 1",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        if (l.limiterAlgorithm > 1u) {
            strncpy_s(resultText, kText,
                      "limiter algorithm must be 0 or 1", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }

        // Seqlock write: odd sequence marks the mailbox as in-progress
        // for the audio thread, even publishes the completed copy.
        LiveConfigMailbox* mb = &liveMailbox_;
        const uint32_t even = liveMailboxSeq_.load(std::memory_order_relaxed);
        liveMailboxSeq_.store(even | 1u, std::memory_order_relaxed);
        RLV2_MemBarrier();

        if (cmd.groupMask & svms::RLGroupMaster) {
            mb->masterVolume = (std::max)(0.0f, (std::min)(4.0f, l.masterVolume));
        }
        if (cmd.groupMask & svms::RLGroupCorrectness) {
            mb->correctnessMode = l.correctnessMode != 0u;
        }
        if (cmd.groupMask & svms::RLGroupReverb) {
            mb->reverbEnabled = l.reverbEnabled != 0u;
            mb->reverbMix = (std::max)(0.0f, (std::min)(1.0f, l.reverbMix));
            mb->reverbRoomSize = (std::max)(0.0f, (std::min)(1.0f, l.reverbRoomSize));
            mb->reverbDecay = (std::max)(0.0f, (std::min)(1.0f, l.reverbDecay));
            mb->reverbDamping = (std::max)(0.0f, (std::min)(1.0f, l.reverbDamping));
            mb->reverbWidth = (std::max)(0.0f, (std::min)(1.0f, l.reverbWidth));
            mb->reverbDiffusion = (std::max)(0.0f, (std::min)(1.0f, l.reverbDiffusion));
            mb->reverbPreDelayMs = (std::max)(0.0f, (std::min)(200.0f, l.reverbPreDelayMs));
            mb->reverbEarlyLevel = (std::max)(0.0f, (std::min)(1.5f, l.reverbEarlyLevel));
            mb->reverbLateLevel = (std::max)(0.0f, (std::min)(1.5f, l.reverbLateLevel));
            mb->reverbModDepth = (std::max)(0.0f, (std::min)(1.0f, l.reverbModDepth));
            mb->reverbModRate = (std::max)(0.0f, (std::min)(1.0f, l.reverbModRate));
            mb->reverbLowCutHz = (std::max)(0.0f, (std::min)(2000.0f, l.reverbLowCutHz));
            mb->reverbHighCutHz = (std::max)(1000.0f,
                (std::min)(static_cast<float>(sampleRate) * 0.45f, l.reverbHighCutHz));
        }
        if (cmd.groupMask & svms::RLGroupLimiter) {
            mb->limiterEnabled = l.limiterEnabled != 0u;
            mb->limiterAlgorithm = l.limiterAlgorithm;
            mb->limiterThreshold = (std::max)(0.1f, (std::min)(1.0f, l.limiterThreshold));
            uint32_t frames = static_cast<uint32_t>(
                (std::max)(0.0f, (std::min)(20.0f, l.limiterLookaheadMs))
                * sampleRate * 0.001f + 0.5f);
            mb->limiterDelayFrames =
                (std::min)(svms::LimiterState::kMaxDelayFrames, frames);
            float attackSamples = (std::max)(0.01f, (std::min)(100.0f, l.limiterAttackMs))
                                * sampleRate * 0.001f;
            attackSamples = (std::max)(1.0f, attackSamples);
            mb->limiterAttackCoeff = 1.0f - std::exp(-1.0f / attackSamples);
            float releaseSamples = (std::max)(1.0f, (std::min)(5000.0f, l.limiterReleaseMs))
                                 * sampleRate * 0.001f;
            releaseSamples = (std::max)(1.0f, releaseSamples);
            mb->limiterReleaseCoeff = 1.0f - std::exp(-1.0f / releaseSamples);
        }

        // Publish the completed copy (even sequence, release store).
        RLV2_MemBarrier();
        liveMailboxSeq_.store(even + 2u, std::memory_order_release);
        lastPublishedMailboxSeq_ = even + 2u;
        return svms::RLResult::Ok;
    }

    case RT::ReloadSoundFont: {
        // Queue a transactional immutable-bundle load. The loader thread does
        // all file, conversion, and preparation work; callback entry performs
        // only the completed bundle activation.
        std::wstring widePath;
        const size_t length = strnlen_s(
            cmd.resultText, svms::kRuntimeLinkCommandTextCapacity);
        if (length != 0u && length < svms::kRuntimeLinkCommandTextCapacity) {
            const int wideLength = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, cmd.resultText,
                static_cast<int>(length), nullptr, 0);
            if (wideLength <= 0) {
                strncpy_s(resultText, kText,
                          "SoundFont path is not valid UTF-8", _TRUNCATE);
                return svms::RLResult::InvalidArgument;
            }
            widePath.resize(static_cast<size_t>(wideLength));
            if (MultiByteToWideChar(
                    CP_UTF8, MB_ERR_INVALID_CHARS, cmd.resultText,
                    static_cast<int>(length), widePath.data(), wideLength) !=
                wideLength) {
                strncpy_s(resultText, kText, "could not decode SoundFont path",
                          _TRUNCATE);
                return svms::RLResult::InvalidArgument;
            }
        } else {
            std::string resolutionWarning;
            widePath = ResolveV3SoundFontPath(engineConfig_, &resolutionWarning);
            if (widePath.empty() && !resolutionWarning.empty())
                strncpy_s(resultText, kText, resolutionWarning.c_str(), _TRUNCATE);
        }
        if (widePath.empty()) {
            strncpy_s(resultText, kText, "no SoundFont configured", _TRUNCATE);
            return svms::RLResult::LoadFailed;
        }
        uint64_t requestId = 0u;
        if (!QueueSoundFontLoad(widePath, requestId)) {
            strncpy_s(resultText, kText, "SoundFont loader is unavailable",
                      _TRUNCATE);
            return svms::RLResult::LoadFailed;
        }
        snprintf(resultText, kText, "loading request %llu",
                 static_cast<unsigned long long>(requestId));
        return svms::RLResult::Ok;
    }

    case RT::QuerySoundFontLoad: {
        ReclaimRetiredSoundFonts();
        const uint32_t state = soundFontLoadState_.load(
            std::memory_order_acquire);
        const uint64_t requested = soundFontRequestId_.load(
            std::memory_order_acquire);
        const uint64_t activated = soundFontActivatedId_.load(
            std::memory_order_acquire);
        std::string error;
        EnterCriticalSection(&cs);
        error = soundFontLoadError_;
        LeaveCriticalSection(&cs);
        snprintf(resultText, kText, "%u\t%llu\t%llu\t%s", state,
                 static_cast<unsigned long long>(requested),
                 static_cast<unsigned long long>(activated), error.c_str());
        return svms::RLResult::Ok;
    }

    case RT::ResetVoices:
        // Routes through the SPSC ingress when audio is running, so the
        // release work happens on the audio thread exactly like
        // midiOutReset.
        ResetAllVoices();
        return svms::RLResult::Ok;

    case RT::SetPhaseRotation: {
        const uint32_t mode = cmd.param;
        if (mode > 4u) {
            strncpy_s(resultText, kText,
                      "phase rotation mode must be 0..4", _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        LiveConfigMailbox* mb = &liveMailbox_;
        const uint32_t even = liveMailboxSeq_.load(std::memory_order_relaxed);
        liveMailboxSeq_.store(even | 1u, std::memory_order_relaxed);
        RLV2_MemBarrier();
        mb->phaseRotationMode.store(mode, std::memory_order_relaxed);
        RLV2_MemBarrier();
        liveMailboxSeq_.store(even + 2u, std::memory_order_release);
        lastPublishedMailboxSeq_ = even + 2u;
        strncpy_s(resultText, kText, "phase rotation mode set", _TRUNCATE);
        return svms::RLResult::Ok;
    }

    case RT::SetNoteOnCollapse: {
        // param = spawn interval; 0/1 disables coalescing (default state:
        // every note-on spawns at its exact QPC timestamp). The gate
        // rounds the value down to a power of two internally.
        const uint32_t threshold = cmd.param;
        if (threshold > 65536u) {
            strncpy_s(resultText, kText,
                      "note-on collapse threshold must be 0..65536",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        noteOnCollapse_.SetThreshold(threshold);
        if (threshold <= 1u) {
            strncpy_s(resultText, kText,
                      "note-on coalescing disabled (every note-on spawns)",
                      _TRUNCATE);
        } else {
            snprintf(resultText, kText,
                     "note-on coalescing enabled, 1 voice per %u hits",
                     noteOnCollapse_.Threshold());
        }
        return svms::RLResult::Ok;
    }

    case RT::StartLiveRecording: {
        const size_t length = strnlen_s(
            cmd.resultText, svms::kRuntimeLinkCommandTextCapacity);
        if (length == 0u ||
            length >= svms::kRuntimeLinkCommandTextCapacity) {
            strncpy_s(resultText, kText, "missing or invalid WAV path",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        const int wideLength = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, cmd.resultText,
            static_cast<int>(length), nullptr, 0);
        if (wideLength <= 0) {
            strncpy_s(resultText, kText, "WAV path is not valid UTF-8",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        std::wstring path(static_cast<size_t>(wideLength), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                cmd.resultText, static_cast<int>(length),
                                path.data(), wideLength) != wideLength) {
            strncpy_s(resultText, kText, "could not decode WAV path",
                      _TRUNCATE);
            return svms::RLResult::InvalidArgument;
        }
        const auto status = liveRecorder_.GetStatus();
        if (status.state == svms::LiveWaveRecorder::State::Recording ||
            status.state == svms::LiveWaveRecorder::State::Starting ||
            status.state == svms::LiveWaveRecorder::State::Stopping) {
            strncpy_s(resultText, kText, "a live recording is already active",
                      _TRUNCATE);
            return svms::RLResult::Busy;
        }
        std::string error;
        if (!liveRecorder_.Start(path.c_str(), sampleRate, error)) {
            strncpy_s(resultText, kText, error.c_str(), _TRUNCATE);
            return svms::RLResult::LoadFailed;
        }
        strncpy_s(resultText, kText, "recording started", _TRUNCATE);
        return svms::RLResult::Ok;
    }

    case RT::StopLiveRecording:
        liveRecorder_.Stop();
        strncpy_s(resultText, kText, "recording stopped and WAV finalized",
                  _TRUNCATE);
        return svms::RLResult::Ok;

    case RT::QueryLiveRecording: {
        const auto status = liveRecorder_.GetStatus();
        std::snprintf(resultText, kText, "%u\t%u\t%llu\t%llu\t%u",
                      static_cast<unsigned>(status.state), status.sampleRate,
                      static_cast<unsigned long long>(status.framesWritten),
                      static_cast<unsigned long long>(status.droppedFrames),
                      status.errorCode);
        return svms::RLResult::Ok;
    }

    case RT::RequestRestart:
        strncpy_s(resultText, kText,
                  "restart is a manual operation: change restart-only "
                  "fields and restart the driver",
                  _TRUNCATE);
        return svms::RLResult::Unsupported;

    default:
        strncpy_s(resultText, kText, "unknown command type", _TRUNCATE);
        return svms::RLResult::InvalidArgument;
    }
}
#endif // !defined(SVMS_XP_COMPAT)

// ── RuntimeLink telemetry builder (runs on the control thread) ──────────
// Reads the process-local audio snapshot (written by the audio thread),
// the immutable engine parameters, and the applied-live echo, then hands
// the result to the driver for publication.  No audio-thread structures
// are touched here: every per-block counter travels through the snapshot.
#if !defined(SVMS_XP_COMPAT)
svms::RuntimeLinkTelemetryV2 Driver::BuildRuntimeLinkTelemetry() {
    // This control thread is the sole reclaimer. Draining before reading the
    // active pointer ensures a bundle retired concurrently after the load
    // remains alive until the next telemetry pass.
    ReclaimRetiredSoundFonts();
    // Wide→UTF-8 for the SoundFont name broadcast (local helper; the
    // SVMSConfig.cpp copy of WideToUtf8 is not exported via the header).
    auto wideToUtf8 = [](const std::wstring& value) -> std::string {
        if (value.empty()) return std::string();
        const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                              static_cast<int>(value.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (count <= 0) return std::string();
        std::string out(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(),
                            static_cast<int>(value.size()), out.data(), count,
                            nullptr, nullptr);
        return out;
    };

    svms::RuntimeLinkTelemetryV2 snap{};
    static svms::RuntimeLinkTelemetryV2 s_lastBuilt;  // last stable publish

    // Monotonic seqlock read: take the even sequence, copy the relaxed
    // payload, then confirm nothing moved (odd, or the sequence advanced
    // mid-copy = the frame may be torn).  Because the sequence only ever
    // grows by 2, an equality check is exact (no ABA).
    const svms::RuntimeAudioSnapshot& as = g_audioSnapshot;
    const uint32_t seq = as.sequence.load(std::memory_order_acquire);
    if ((seq & 1u) == 0u) {
        const uint32_t tick = as.tickMs.load(std::memory_order_relaxed);
        snap.activeVoices = as.activeVoices.load(std::memory_order_relaxed);
        snap.releasingVoices = as.releasingVoices.load(std::memory_order_relaxed);
        snap.freeTop = as.freeTop.load(std::memory_order_relaxed);
        snap.voiceSteals = as.voiceSteals.load(std::memory_order_relaxed);
        snap.retiredCount = as.retiredCount.load(std::memory_order_relaxed);
        snap.retiredImmediateCount =
            as.retiredImmediateCount.load(std::memory_order_relaxed);
        snap.decimationStep = as.decimationStep.load(std::memory_order_relaxed);
        snap.renderPeak = U32BitsToFloat(
            as.renderPeakBits.load(std::memory_order_relaxed));
        snap.audioRunning = as.audioRunning.load(std::memory_order_relaxed);
        snap.soundFontLoaded = as.soundFontLoaded.load(std::memory_order_relaxed);
        snap.audioHResult = as.audioHResult.load(std::memory_order_relaxed);
        snap.cpuLoadPercent = U32BitsToFloat(
            as.cpuLoadPercentBits.load(std::memory_order_relaxed));
        snap.callbackP95Percent = U32BitsToFloat(
            as.callbackP95PercentBits.load(std::memory_order_relaxed));
        snap.callbackP99Percent = U32BitsToFloat(
            as.callbackP99PercentBits.load(std::memory_order_relaxed));
        snap.callbackP999Percent = U32BitsToFloat(
            as.callbackP999PercentBits.load(std::memory_order_relaxed));
        snap.maxConsecutiveOverBudget =
            as.maxConsecutiveOverBudget.load(std::memory_order_relaxed);
        snap.overBudgetCallbacks =
            as.overBudgetCallbacks.load(std::memory_order_relaxed);
        snap.eventsSubmitted = as.eventsSubmitted.load(std::memory_order_relaxed);
        snap.eventsAccepted = as.eventsAccepted.load(std::memory_order_relaxed);
        snap.eventsDropped = as.eventsDropped.load(std::memory_order_relaxed);
        snap.eventsDispatched = as.eventsDispatched.load(std::memory_order_relaxed);
        snap.limiterInputPeakL = U32BitsToFloat(
            as.limiterInputPeakLBits.load(std::memory_order_relaxed));
        snap.limiterInputPeakR = U32BitsToFloat(
            as.limiterInputPeakRBits.load(std::memory_order_relaxed));
        snap.limiterOutputPeakL = U32BitsToFloat(
            as.limiterOutputPeakLBits.load(std::memory_order_relaxed));
        snap.limiterOutputPeakR = U32BitsToFloat(
            as.limiterOutputPeakRBits.load(std::memory_order_relaxed));
        snap.limiterGainReductionDb = U32BitsToFloat(
            as.limiterGainReductionDbBits.load(std::memory_order_relaxed));
        snap.schedulerPercent = U32BitsToFloat(
            as.schedulerPercentBits.load(std::memory_order_relaxed));
        snap.eventDispatchPercent = U32BitsToFloat(
            as.eventDispatchPercentBits.load(std::memory_order_relaxed));
        snap.rawIngressCount =
            as.rawIngressCount.load(std::memory_order_relaxed);
        snap.compiledPagedCount =
            as.compiledPagedCount.load(std::memory_order_relaxed);
        snap.scheduledBacklogCount =
            as.scheduledBacklogCount.load(std::memory_order_relaxed);

        // Re-verify the settlement: a writer that started mid-copy means
        // this frame may be torn — reuse the last stable publish instead
        // (same skip-if-busy pattern as the client side).
        RLV2_MemBarrier();
        if (as.sequence.load(std::memory_order_acquire) != seq ||
            as.tickMs.load(std::memory_order_relaxed) != tick) {
            return s_lastBuilt;
        }
        s_lastBuilt = snap;
    } else {
        return s_lastBuilt;
    }

    snap.maxVoices = voiceManager ? voiceManager->GetMaxVoices() : 0u;
    snap.sampleRate = sampleRate;
    snap.bufferFrames = bufferFrames;

    // Live-state echo: what the audio thread last applied from the
    // mailbox (appliedMailbox_ + appliedSeq_ — release/acquire ordered),
    // forwarded to the client as the "applied" live config.
    const uint32_t appliedSeq = appliedSeq_.load(std::memory_order_acquire);
    if (appliedSeq != lastEchoedAppliedSeq_) {
        lastEchoedAppliedSeq_ = appliedSeq;
        appliedLiveEcho_ = LiveStateFromMailbox(appliedMailbox_, sampleRate);
    }
    snap.live = appliedLiveEcho_;

    const std::wstring activePath = CopyActiveSoundFontPath();
    if (!activePath.empty()) {
        std::string narrow = wideToUtf8(activePath);
        strncpy_s(snap.soundFontName, sizeof(snap.soundFontName),
                  narrow.c_str(), _TRUNCATE);
    }

    return snap;
}
#endif // !defined(SVMS_XP_COMPAT)

struct PreparedSF2Region {
    float basePhaseStep[kNoteCount];
    float bendScale;
    // SF2 vibrato LFO constants resolved once at load time.
    float vibLfoToPitchCents;
    float vibLfoPhaseStep;
    uint32_t vibLfoDelaySamples;
    float attenuationGain;
    float sustainLevel;
    float decaySlope;
    float releaseDecay;
    float panLeft;
    float panRight;
    uint32_t delaySamples;
    uint32_t holdSamples;
    uint32_t attackSamples;
    uint32_t decaySamples;
    uint32_t releaseSamples;
    uint8_t valid;
};

// One SoundFont load produces one self-contained immutable bundle.  The
// loader owns and fills every field before publication; after that only the
// audio thread reads the payload.  retiredNext is lifecycle metadata used by
// the lock-free audio->control retirement stack and is never render data.
struct SoundFontBundle {
    SF2Data* data = nullptr;
    int16_t* sampleData = nullptr;
    SF2Sample* samples = nullptr;
    float* regionInitialPeaks = nullptr;
    PreparedSF2Region* preparedRegions = nullptr;
    uint32_t sampleCount = 0u;
    uint32_t sampleDataFrames = 0u;
    uint32_t regionInitialPeakCount = 0u;
    uint32_t preparedRegionCount = 0u;
    uint32_t sampleBase = 0u;
    uint64_t requestId = 0u;
    std::wstring path;
    SoundFontBundle* banks[kMaxSoundFontStackEntries]{};
    uint32_t bankCount = 0u;
    SoundFontRoute routes[kMaxSoundFontRoutes]{};
    uint32_t routeCount = 0u;
    SoundFontBundle* retiredNext = nullptr;
};

static uint32_t SoundFontBankCount(const SoundFontBundle* bundle) noexcept {
    if (!bundle) return 0u;
    return bundle->bankCount != 0u ? bundle->bankCount : 1u;
}

static SoundFontBundle* SoundFontBankAt(const SoundFontBundle* bundle,
                                        uint32_t index) noexcept {
    if (!bundle) return nullptr;
    if (bundle->bankCount == 0u)
        return index == 0u ? const_cast<SoundFontBundle*>(bundle) : nullptr;
    return index < bundle->bankCount ? bundle->banks[index] : nullptr;
}

// Resolve explicit routes first, then walk the immutable stack in priority
// order. CC0 remains the SF2 bank selector; CC32 stays tracked MIDI state.
static bool ResolveChannelPreset(const SoundFontBundle* bundle,
                                 const ChannelCache& cache, uint8_t channel,
                                 uint8_t* outSoundFontIndex,
                                 uint32_t* outPresetIndex) {
    if (!bundle || !outSoundFontIndex || !outPresetIndex ||
        channel >= kChannelCount) return false;
    const uint16_t bank = cache.GetBankMSB(channel);
    const uint8_t program = cache.GetProgram(channel);
    const bool percussion = cache.IsPercussion(channel);
    for (uint32_t i = 0u; i < bundle->routeCount; ++i) {
        const SoundFontRoute& route = bundle->routes[i];
        if (route.targetBank != bank || route.percussion != percussion ||
            (route.targetPreset >= 0 && route.targetPreset != program))
            continue;
        SoundFontBundle* selected = SoundFontBankAt(bundle,
                                                     route.soundFontIndex);
        if (!selected || !selected->data) continue;
        const uint16_t sourcePreset = route.sourcePreset >= 0
            ? static_cast<uint16_t>(route.sourcePreset) : program;
        if (sf2_find_preset(selected->data, route.sourceBank, sourcePreset,
                            outPresetIndex)) {
            *outSoundFontIndex = static_cast<uint8_t>(route.soundFontIndex);
            return true;
        }
    }
    const uint32_t count = SoundFontBankCount(bundle);
    for (uint32_t i = 0u; i < count; ++i) {
        SoundFontBundle* selected = SoundFontBankAt(bundle, i);
        if (selected && selected->data &&
            sf2_resolve_preset(selected->data, bank, program, percussion,
                               outPresetIndex)) {
            *outSoundFontIndex = static_cast<uint8_t>(i);
            return true;
        }
    }
    return false;
}

static void DestroySoundFontBundle(SoundFontBundle* bundle) noexcept {
    if (!bundle) return;
    if (bundle->bankCount != 0u) {
        for (uint32_t i = 0u; i < bundle->bankCount; ++i)
            DestroySoundFontBundle(bundle->banks[i]);
        free(bundle->sampleData);
        delete bundle;
        return;
    }
    free(bundle->regionInitialPeaks);
    free(bundle->preparedRegions);
    free(bundle->sampleData);
    free(bundle->samples);
    if (bundle->data) {
        sf2_free(bundle->data);
        delete bundle->data;
    }
    delete bundle;
}

static void PrepareSF2Region(const SF2Data* data, const SFSampleRegion& region,
                             uint32_t outputRate, ChannelCache* channelCache,
                             PreparedSF2Region& out) {
    std::memset(&out, 0, sizeof(out));
    if (!data || region.sampleIndex >= data->sampleCount ||
        !sf2_validate_region(data, &region)) return;

    const SF2Sample& sample = data->samples[region.sampleIndex];
    const int rootKey = region.rootKey >= 0
        ? static_cast<int>(region.rootKey)
        : static_cast<int>(sample.originalPitch);
    const float tune = static_cast<float>(region.coarseTune) +
        static_cast<float>(region.fineTune) / 100.0f;
    out.bendScale = static_cast<float>(
        region.scaleTuning != 0 ? region.scaleTuning : 100) / 100.0f;
    const float sourceRate = static_cast<float>(
        sample.sampleRate > 0u ? sample.sampleRate : 44100u);
    const float targetRate = static_cast<float>(
        outputRate > 0u ? outputRate : 44100u);
    const float rateRatio = sourceRate / targetRate;
    for (uint32_t note = 0; note < kNoteCount; ++note) {
        const float semitones =
            (static_cast<float>(note) + tune - static_cast<float>(rootKey)) *
            out.bendScale;
        out.basePhaseStep[note] =
            rateRatio * powf(2.0f, semitones / 12.0f);
    }

    out.attenuationGain = region.initialAttenuation > 0
        ? InitialAttenuationToGain(static_cast<float>(region.initialAttenuation))
        : 1.0f;
    out.sustainLevel = SustainAttenuationToGain((std::max)(
        0.0f, static_cast<float>(region.sustainVolEnv)));
    if (out.sustainLevel > 1.0f) out.sustainLevel = 1.0f;

    const float rate = static_cast<float>(outputRate > 0u ? outputRate : 44100u);
    const float delaySeconds = TimecentsToSeconds(region.delayVolEnv);
    const float holdSeconds = TimecentsToSeconds(region.holdVolEnv);
    const float attackSeconds = TimecentsToSeconds(region.attackVolEnv);
    const float decaySeconds = TimecentsToSeconds(region.decayVolEnv);
    const float releaseSeconds = TimecentsToSeconds(region.releaseVolEnv);
    out.delaySamples = delaySeconds > 0.0f
        ? static_cast<uint32_t>(delaySeconds * rate) : 0u;
    out.holdSamples = holdSeconds > 0.0f
        ? static_cast<uint32_t>(holdSeconds * rate) : 0u;
    out.attackSamples = attackSeconds > 0.0001f
        ? static_cast<uint32_t>(attackSeconds * rate) : 0u;
    out.decaySamples = decaySeconds > 0.0001f
        ? static_cast<uint32_t>(decaySeconds * rate) : 0u;
    out.decaySlope = 1.0f;
    if (out.decaySamples > 0u) {
        const float slope = -9.226f / static_cast<float>(out.decaySamples);
        out.decaySlope = expf(slope);
        if (out.sustainLevel > 0.0f && out.sustainLevel < 1.0f)
            out.decaySamples = static_cast<uint32_t>(logf(out.sustainLevel) / slope);
    }
    out.releaseDecay = MakeReleaseDecay(releaseSeconds, outputRate);
    out.releaseSamples = MakeReleaseSamples(releaseSeconds, outputRate);
    out.vibLfoToPitchCents = static_cast<float>(region.vibLfoToPitch);
    out.vibLfoPhaseStep =
        powf(2.0f, static_cast<float>(region.freqVibLfo) / 1200.0f) / rate;
    const float vibDelaySeconds = TimecentsToSeconds(region.delayVibLfo);
    out.vibLfoDelaySamples = vibDelaySeconds > 0.0f
        ? static_cast<uint32_t>(vibDelaySeconds * rate) : 0u;
    out.panLeft = 1.0f;
    out.panRight = 1.0f;
    if (channelCache)
        channelCache->ComputeSoundFontPan(region.pan, out.panLeft, out.panRight);
    out.valid = 1u;
}

Driver& Driver::Instance() {
    if (!s_instance) {
        s_instance = new Driver();
    }
    return *s_instance;
}

Driver::Driver()
    : initialized(false), sampleRate(44100), bufferFrames(512),
      audioOutput(nullptr), voiceManager(nullptr), channelCache(nullptr),
      renderScalar(nullptr), soundFontData(nullptr), configSnapshot(nullptr),
      sampleDataStore(nullptr), samplesStore(nullptr), regionInitialPeaks(nullptr),
      regionInitialPeakCount(0), preparedRegions(nullptr), preparedRegionCount(0),
      soundFontGeneration_(1u),
      sampleStoreCount(0), sampleDataFrames(0),
      qpcFreq(1),
      leftBuffer(nullptr), rightBuffer(nullptr), bufferCapacity(0),
      eventBuffer(nullptr), eventBufferCapacity_(0u),
      eventScheduler_(1u),
      overflowMode_(EventOverflowMode::PriorityVelocity), correctnessMode_(false),
      highPriorityVelocity_(96), shedStartPercent_(70), maxEventsPerBlock_(65536),
      diagnosticsEnabled_(false), diagnosticsWindow_(false), diagnosticsDebugOutput_(false),
      nextEventSequence_(0), globalTerminationFence_(0), cancelProducers_(false),
      producerWakeEpoch_(0),
      scheduledSizePublished_(0), submittedAtomic_(0), acceptedAtomic_(0), shedAtomic_(0),
      cancelledAtomic_(0), currentVelocityCutoffAtomic_(1),
      compilerEpochQPC_(0), compilerWakeEpoch_(0), compilerSleeping_(false),
      useEventCompiler_(false),
      telemetry_{}, debugSnapshotIndex_(0), callbackCount_(0),
      virtualRenderClockQPC(0),
      virtualRenderSample_(0), clockInitialized(false), nextPlayIndex_(1) {
    std::memset(noteRegionCache_, 0xff, sizeof(noteRegionCache_));
    std::memset(noteLaunchPlanCache_, 0, sizeof(noteLaunchPlanCache_));
    std::memset(noteLaunchHotCache_, 0, sizeof(noteLaunchHotCache_));
    std::fill(std::begin(channelLaunchRevision_),
              std::end(channelLaunchRevision_), 1u);
    std::memset(configuredVelocityGain_, 0, sizeof(configuredVelocityGain_));
    std::fill(std::begin(channelPitchBendRatio_),
              std::end(channelPitchBendRatio_), 1.0f);
    for (auto& counter : shedByVelocityAtomic_) counter.store(0, std::memory_order_relaxed);
    for (auto& fence : channelTerminationFence_) fence.store(0, std::memory_order_relaxed);
    reverb.Reset();
    limiter.Reset();
    InitializeCriticalSection(&cs);
    InitializeCriticalSection(&soundFontBuildCs_);
}

Driver::~Driver() {
    Shutdown();
    if (eventBuffer) { _aligned_free(eventBuffer); eventBuffer = nullptr; }
    DeleteCriticalSection(&soundFontBuildCs_);
    DeleteCriticalSection(&cs);
}

bool Driver::Initialize() {
    if (initialized) return true;

    ResolveAddressWaitApi();
    midiIngress_.DrainAvailable();
    eventScheduler_.Reset();
    compilerEpochQPC_.store(0u, std::memory_order_relaxed);
    globalTerminationFence_.store(0, std::memory_order_relaxed);
    for (auto& fence : channelTerminationFence_) fence.store(0, std::memory_order_relaxed);
    virtualRenderClockQPC = 0;
    virtualRenderSample_ = 0;
    outputFramePublished_.store(0u, std::memory_order_relaxed);
    clockInitialized = false;
    callbackTiming_.Reset();

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFreq = freq.QuadPart;

    // Note-on collapse window in QPC ticks (fixed 20 ms, frame-size
    // independent). Coalescing itself defaults OFF; this only defines the
    // window used once it is enabled at runtime.
    noteOnCollapse_.SetWindowTicks(
        freq.QuadPart * kNoteOnCollapseWindowMs / 1000u);

    EngineConfig cfg = EngineConfig::Load();
    if (!cfg.configWarning.empty()) {
        std::string warning = "[SVMS] configuration warning: " +
                              cfg.configWarning + "\n";
        OutputDebugStringA(warning.c_str());
    }
    if (!cfg.Validate()) { LOG("EngineConfig validation failed"); return false; }

    // Threshold comes from config (default 1 = disabled). Applied before
    // the audio thread starts, so no torn first-block state.
    noteOnCollapse_.SetThreshold(cfg.noteOnCollapseThreshold);

    overflowMode_ = cfg.eventOverflowMode;
    correctnessMode_ = cfg.correctnessMode;
    highPriorityVelocity_ = cfg.highPriorityVelocity;
    shedStartPercent_ = cfg.shedStartPercent;
    diagnosticsEnabled_ = cfg.diagnosticsEnabled;
    diagnosticsWindow_ = cfg.diagnosticsEnabled && cfg.diagnosticsWindow;
    diagnosticsDebugOutput_ = cfg.diagnosticsEnabled && cfg.diagnosticsDebugOutput;
    cancelProducers_.store(false, std::memory_order_release);
    compilerSleeping_.store(false, std::memory_order_relaxed);

    auto configureEventStorage = [this](uint32_t ringCapacity,
                                        uint32_t blockCapacity) -> bool {
        if (!midiIngress_.ConfigureCapacity(ringCapacity)) return false;
        if (!compiledPages_.ConfigureCapacity(ringCapacity)) return false;
        if (!pagedScheduler_.Configure(&compiledPages_, ringCapacity))
            return false;
        try {
            eventScheduler_.ConfigureCapacity(ringCapacity);
        } catch (...) {
            return false;
        }
        const uint32_t actualBlockCapacity = blockCapacity;
        if (static_cast<size_t>(actualBlockCapacity) >
            (std::numeric_limits<size_t>::max)() / sizeof(svms::RenderEvent)) {
            return false;
        }
        svms::RenderEvent* replacement =
            static_cast<svms::RenderEvent*>(_aligned_malloc(
                sizeof(svms::RenderEvent) *
                    static_cast<size_t>(actualBlockCapacity),
                64));
        if (!replacement) return false;
        if (eventBuffer) _aligned_free(eventBuffer);
        eventBuffer = replacement;
        eventBufferCapacity_ = actualBlockCapacity;
        return true;
    };

    if (!configureEventStorage(cfg.eventRingCapacity,
                               cfg.maxEventsPerBlock)) {
        char warning[256]{};
        std::snprintf(
            warning, sizeof(warning),
            "[SVMS] configuration warning: event capacity %u / block %u "
            "could not be allocated; using %u / %u\n",
            cfg.eventRingCapacity, cfg.maxEventsPerBlock,
            kDefaultEventRingCapacity, 65536u);
        OutputDebugStringA(warning);
        LOG("Configuration warning: event capacity %u / block %u could not "
            "be allocated; falling back to %u / %u",
            cfg.eventRingCapacity, cfg.maxEventsPerBlock,
            kDefaultEventRingCapacity, 65536u);
        cfg.eventRingCapacity = kDefaultEventRingCapacity;
        cfg.maxEventsPerBlock = 65536u;
        if (!configureEventStorage(cfg.eventRingCapacity,
                                   cfg.maxEventsPerBlock)) {
            LOG("FAILED: Could not allocate default event storage");
            return false;
        }
    }
    maxEventsPerBlock_ = cfg.maxEventsPerBlock;
    engineConfig_ = cfg;

    sampleRate = cfg.sampleRate;
    bufferFrames = cfg.bufferFrames;
    LOG("Initialize: sampleRate=%u bufferFrames=%u maxVoices=%u", sampleRate, bufferFrames, cfg.maxVoices);

    // Start diagnostics before the backend so an XP DirectSound failure is
    // visible rather than returning from midiOutOpen with no evidence.
    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_)) {
        DiagWindow_Create(diagnosticsWindow_, diagnosticsDebugOutput_);
        DiagWindow_UpdateStartup(false, 0, false, sampleRate, bufferFrames,
                                 cfg.masterVolume);
    }

    audioOutput = new AudioOutput();
#if defined(SVMS_XP_COMPAT)
    if (!audioOutput->Initialize(sampleRate, bufferFrames)) {
#else
    if (!audioOutput->Initialize(sampleRate, bufferFrames, cfg.audioDevice)) {
#endif
        HRESULT hr = audioOutput->GetLastError();
        LOG("FAILED: AudioOutput::Initialize hr=0x%08X", (unsigned)hr);
        XPBootstrapTrace("[SVMS XP] DirectSound initialization FAILED\r\n");
        DiagWindow_UpdateStartup(false, static_cast<int32_t>(hr), false,
                                 sampleRate, bufferFrames, cfg.masterVolume,
                                 UsesXPWaveOut(audioOutput));
#if !defined(SVMS_XP_COMPAT)
        delete audioOutput;
        audioOutput = nullptr;
#endif
        return false;
    }
    bufferFrames = audioOutput->GetBufferFrames();
    sampleRate = audioOutput->GetSampleRate();
    postHighPass.Initialize(sampleRate);
    reverb.Configure(sampleRate, cfg);
    limiter.Configure(sampleRate, cfg);
    LOG("AudioOutput initialized, rate=%u bufferFrames=%u", sampleRate, bufferFrames);

    bufferCapacity = bufferFrames;
    leftBuffer = static_cast<float*>(_aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
    rightBuffer = static_cast<float*>(_aligned_malloc(bufferCapacity * sizeof(float), kMixBufferAlign));
    if (!leftBuffer || !rightBuffer) {
        LOG("FAILED: Could not allocate render buffers");
        return false;
    }

    uint32_t renderThreads = cfg.renderThreads;
    if (renderThreads == 0u)
        renderThreads = SelectAutomaticRenderThreadCount();
    uint32_t voiceGrowthCeiling = kMaxPolyphony;
    if (cfg.voiceMemoryBudgetMB != 0u) {
        const uint64_t budgetBytes =
            static_cast<uint64_t>(cfg.voiceMemoryBudgetMB) << 20u;
        if (renderThreads > 1u &&
            cfg.maxVoices <= kDenseRenderMaximumVoices &&
            EstimateRuntimeVoiceMemoryBytes(
                cfg.maxVoices, renderThreads, bufferCapacity) > budgetBytes) {
            LOG("Voice memory budget %u MiB cannot fit the dense %u-thread "
                "renderer at %u voices; falling back to one render thread",
                cfg.voiceMemoryBudgetMB, renderThreads, cfg.maxVoices);
            renderThreads = 1u;
        }
        voiceGrowthCeiling = LargestInitialVoiceCapacityForBudget(
            budgetBytes, renderThreads, bufferCapacity);
        if (voiceGrowthCeiling == 0u && renderThreads > 1u) {
            LOG("Voice memory budget %u MiB cannot fit %u-thread renderer; "
                "falling back to one render thread",
                cfg.voiceMemoryBudgetMB, renderThreads);
            renderThreads = 1u;
            voiceGrowthCeiling = LargestInitialVoiceCapacityForBudget(
                budgetBytes, renderThreads, bufferCapacity);
        }
        if (voiceGrowthCeiling == 0u) {
            LOG("FAILED: voice memory budget %u MiB is below minimum runtime "
                "storage", cfg.voiceMemoryBudgetMB);
            return false;
        }
        if (cfg.maxVoices > voiceGrowthCeiling) {
            char warning[256]{};
            std::snprintf(warning, sizeof(warning),
                "[SVMS] configuration warning: synth.max_voices %u exceeds "
                "the %u MiB voice-memory budget; using %u voices\n",
                cfg.maxVoices, cfg.voiceMemoryBudgetMB, voiceGrowthCeiling);
            OutputDebugStringA(warning);
            LOG("Voice memory budget clamped maxVoices %u -> %u",
                cfg.maxVoices, voiceGrowthCeiling);
            cfg.maxVoices = voiceGrowthCeiling;
        }
        voiceGrowthCeiling = LargestGrowthCapacityForBudget(
            budgetBytes, cfg.maxVoices, renderThreads, bufferCapacity);
        if (voiceGrowthCeiling == 0u) {
            LOG("FAILED: voice memory budget %u MiB cannot represent the "
                "selected runtime layout", cfg.voiceMemoryBudgetMB);
            return false;
        }
        const size_t startupBytes = EstimateRuntimeVoiceMemoryBytes(
            cfg.maxVoices, renderThreads, bufferCapacity);
        LOG("Voice memory budget %u MiB: startup %.2f MiB, live-growth "
            "ceiling %u voices", cfg.voiceMemoryBudgetMB,
            static_cast<double>(startupBytes) / (1024.0 * 1024.0),
            voiceGrowthCeiling);
    }
    ConfigureRuntimeVoiceGrowthCeiling(voiceGrowthCeiling);
    engineConfig_ = cfg;

    voiceManager = new VoiceManager();
    if (!voiceManager->Initialize(cfg.maxVoices, sampleRate)) {
        LOG("FAILED: Could not allocate voice storage maxVoices=%u",
            cfg.maxVoices);
        return false;
    }
    // Per-voice phase rotation lives in the VoiceManager (SVMSPhaseRotation.h);
    // mode 0 (Coherent) keeps the render path bit-exact.
    if (!voiceManager->SetPhaseRotationMode(cfg.phaseRotationMode)) {
        LOG("WARNING: Could not allocate phase rotation state; running Coherent");
    }

    for (uint32_t index = 0; index < 2u; ++index) {
        voiceStatisticsSnapshots_[index] = SnappyVoiceStatistics{};
        voiceStatisticsSnapshots_[index].freeVoices = cfg.maxVoices;
        legacyDebugSnapshots_[index] = LegacyDriverDebugInfo{};
        legacyDebugSnapshots_[index].audioLatency =
            static_cast<double>(bufferFrames) * 1000.0 /
            static_cast<double>(sampleRate);
        legacyDebugSnapshots_[index].audioBufferSize = bufferFrames;
        renderingTimeSnapshots_[index] = 0.0f;
    }
    debugSnapshotIndex_.store(0u, std::memory_order_release);
    LOG("VoiceManager initialized, maxVoices=%u", cfg.maxVoices);

    channelCache = new ChannelCache();
    channelCache->SetMasterVolume(cfg.masterVolume);
    renderScalar = new RenderScalar();
    // Honor the configured render backend. The constructor already selects
    // the best set (Auto behavior); an explicit non-Auto request overrides
    // it, falling back to the best available set when unsupported.
    if (cfg.renderBackend != RenderBackend::Auto) {
        if (!renderScalar->SetRenderBackend(cfg.renderBackend)) {
            LOG("Configuration warning: render backend %u not supported on "
                "this CPU; using best available",
                static_cast<uint32_t>(cfg.renderBackend));
        }
    }
    if (!renderScalar->ReserveVoiceCapacity(cfg.maxVoices)) {
        LOG("FAILED: Could not allocate renderer scratch maxVoices=%u",
            cfg.maxVoices);
        return false;
    }
    if (!renderScalar->ConfigureRenderThreads(renderThreads, bufferCapacity)) {
        LOG("Configuration warning: could not start %u render threads; "
            "using the audio thread only", renderThreads);
        renderScalar->ConfigureRenderThreads(1u, bufferCapacity);
    }
    LOG("Voice renderer initialized: backend=%s threads=%u",
        renderScalar->GetRenderBackendName(),
        renderScalar->GetRenderThreadCount());

    // Register the EventDispatcher callback so RenderScalar can dispatch
    // MIDI events at their exact sub-sample positions during RenderBlock.
    renderScalar->SetEventDispatcher(DispatchRenderEvent, this);
    renderScalar->SetEventBatchDispatcher(DispatchRenderEventBatch, this);

    configSnapshot = new RuntimeConfigSnapshot();
    std::memset(configSnapshot, 0, sizeof(RuntimeConfigSnapshot));
    configSnapshot->masterVolume = cfg.masterVolume;
    configSnapshot->velocityCurve = cfg.velocityCurve;
    configSnapshot->velocityFloor = cfg.velocityFloor;
    configSnapshot->velocityIgnoreBelow = cfg.velocityIgnoreBelow;
    configSnapshot->ignoreVelocity = cfg.ignoreVelocity;
    configSnapshot->monoOutput = cfg.monoOutput;
    configSnapshot->enableReverb = cfg.enableReverb;
    configSnapshot->enableChorus = cfg.enableChorus;
    configSnapshot->enableFilter = cfg.enableFilter;
    configSnapshot->enableModulators = cfg.enableModulators;
    configSnapshot->interpolation = cfg.interpolation;
    configSnapshot->filterType = cfg.filterType;
    configSnapshot->panLaw = cfg.panLaw;
    configSnapshot->correctnessMode = cfg.correctnessMode;

    // Initialize both mailbox buffers with the same starting state.
    liveMailbox_.InitFromEngineConfig(cfg, sampleRate);
    liveMailbox_.StoreToNonAtomic(appliedMailbox_);
    liveMailboxSeq_.store(2u, std::memory_order_release);
    appliedSeq_.store(2u, std::memory_order_release);
    lastAppliedLiveSeq_ = 0u;
    appliedMasterVolume_ = cfg.masterVolume;

    // Velocity curve/floor are restart-only configuration.  Preserve the
    // exact historical quantization into g_velGainLUT, but pay powf once at
    // initialization instead of once per note-on.
    for (uint32_t velocity = 0; velocity < 128u; ++velocity) {
        const float mapped = channelCache->ComputeVelocity(
            static_cast<uint8_t>(velocity), *configSnapshot);
        if (mapped <= 0.0f) {
            configuredVelocityGain_[velocity] = 0.0f;
            continue;
        }
        uint32_t mappedIndex = static_cast<uint32_t>(mapped * 127.0f + 0.5f);
        mappedIndex = (std::max)(1u, (std::min)(127u, mappedIndex));
        configuredVelocityGain_[velocity] = g_velGainLUT[mappedIndex];
    }

    audioOutput->SetRenderCallback(RenderCallback, this);

#if !defined(SVMS_XP_COMPAT)
    useEventCompiler_ = std::thread::hardware_concurrency() >= 2u;
    if (useEventCompiler_) {
        try {
            eventCompilerThread_ = std::thread(&Driver::EventCompilerLoop, this);
        } catch (...) {
            useEventCompiler_ = false;
        }
    }

    soundFontLoaderStop_.store(false, std::memory_order_release);
    soundFontLoadState_.store(0u, std::memory_order_relaxed);
    soundFontLoadEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (soundFontLoadEvent_) {
        try {
            soundFontLoaderThread_ =
                std::thread(&Driver::SoundFontLoaderLoop, this);
        } catch (...) {
            CloseHandle(soundFontLoadEvent_);
            soundFontLoadEvent_ = nullptr;
        }
    }
#endif

    initialized = true;
    LOG("Initialize SUCCESS");

    // Start RuntimeLink V2 IPC so the V3 Configurator can connect.
    // Optional: failure must never break midiOutOpen/KDMAPI/audio.
#if !defined(SVMS_XP_COMPAT)
    if (g_rlDriver.Initialize()) {
        g_rlDriver.StartControlThread(
            [this]() { return BuildRuntimeLinkTelemetry(); },
            [this](const svms::RuntimeLinkCommandV2& cmd, char* resultText) {
                return HandleRuntimeLinkCommand(cmd, resultText);
            });
        LOG("RuntimeLink V2%s initialized: PID=%u session=%016llX",
            g_rlDriver.IsV3Initialized() ? "/V3" : "",
            g_rlDriver.GetPID(),
            static_cast<unsigned long long>(g_rlDriver.GetSessionId()));
    } else {
        LOG("RuntimeLink V2 initialization skipped (non-fatal)");
    }
#endif

    return true;
}

void Driver::Shutdown() {
#if !defined(SVMS_XP_COMPAT)
    g_rlDriver.Shutdown();
    liveRecorder_.Stop();
    soundFontLoaderStop_.store(true, std::memory_order_release);
    if (soundFontLoadEvent_) SetEvent(soundFontLoadEvent_);
    if (soundFontLoaderThread_.joinable()) soundFontLoaderThread_.join();
    if (soundFontLoadEvent_) {
        CloseHandle(soundFontLoadEvent_);
        soundFontLoadEvent_ = nullptr;
    }
#endif

    cancelProducers_.store(true, std::memory_order_release);
    producerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(producerWakeEpoch_);
    compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(compilerWakeEpoch_);
    compilerSleeping_.store(false, std::memory_order_release);
    StopConfiguredMidiInput();
    if (audioOutput) {
        audioOutput->Stop();
        audioOutput->Shutdown();
        delete audioOutput;
        audioOutput = nullptr;
    }
    if (eventCompilerThread_.joinable()) eventCompilerThread_.join();
    useEventCompiler_ = false;
    midiIngress_.DrainAvailable();
    pagedScheduler_.Reset();
    eventScheduler_.Reset();
    scheduledSizePublished_.store(0, std::memory_order_release);
    if (eventBuffer) {
        _aligned_free(eventBuffer);
        eventBuffer = nullptr;
        eventBufferCapacity_ = 0u;
    }
    delete voiceManager; voiceManager = nullptr;
    delete channelCache; channelCache = nullptr;
    delete renderScalar; renderScalar = nullptr;
    ConfigureRuntimeVoiceGrowthCeiling(kRuntimeVoiceGrowthCeiling);
    delete configSnapshot; configSnapshot = nullptr;
    _aligned_free(leftBuffer); leftBuffer = nullptr;
    _aligned_free(rightBuffer); rightBuffer = nullptr;
    bufferCapacity = 0;

    DestroyAllSoundFontBundles();

    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_))
        DiagWindow_Destroy();

    initialized = false;
}

bool Driver::LoadConfiguredSoundFont() {
    std::string resolutionWarning;
    const std::vector<std::wstring> paths =
        ResolveV3SoundFontPaths(engineConfig_, &resolutionWarning);
    if (!resolutionWarning.empty()) {
        const std::string message =
            "[SVMS] SoundFont configuration warning: " +
            resolutionWarning + "\n";
        OutputDebugStringA(message.c_str());
    }
    bool loaded = false;
    if (!paths.empty()) {
        const uint64_t requestId = soundFontRequestId_.fetch_add(
            1u, std::memory_order_acq_rel) + 1u;
        std::string error;
        EnterCriticalSection(&soundFontBuildCs_);
        SoundFontBundle* bundle = BuildSoundFontStackBundle(
            paths, engineConfig_.soundFontRoutes, requestId, error);
        LeaveCriticalSection(&soundFontBuildCs_);
        if (bundle) {
            PublishSoundFontBundle(bundle);
            if (!audioOutput || !audioOutput->IsRunning()) {
                ActivatePendingSoundFontAtBlockBoundary();
                ReclaimRetiredSoundFonts();
            }
            loaded = true;
        } else if (!error.empty()) {
            OutputDebugStringA(("[SVMS] SoundFont stack load failed: " +
                                error + "\n").c_str());
        }
    }
    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_)) {
        DiagWindow_UpdateStartup(audioOutput && audioOutput->IsRunning(),
                                 audioOutput
                                     ? static_cast<int32_t>(audioOutput->GetLastError())
                                     : 0,
                                 loaded, sampleRate, bufferFrames,
                                 engineConfig_.masterVolume,
                                 UsesXPWaveOut(audioOutput));
    }
    return loaded;
}

SoundFontBundle* Driver::BuildSoundFontBundle(const wchar_t* path,
                                               uint64_t requestId,
                                               std::string& error) {
    error.clear();
    if (!path || !*path) {
        error = "no SoundFont path supplied";
        return nullptr;
    }
    SoundFontBundle* bundle = new (std::nothrow) SoundFontBundle();
    SF2Data* sf2 = new (std::nothrow) SF2Data();
    if (!bundle || !sf2) {
        delete bundle;
        delete sf2;
        error = "not enough memory to load SoundFont";
        return nullptr;
    }
    bundle->data = sf2;
    bundle->requestId = requestId;
    bundle->path = path;
    if (!sf2_load(path, sf2)) {
        LOG("  sf2_load FAILED: presets=%u inst=%u samples=%u sampleData=%d frames=%u",
            sf2->presetCount, sf2->instrumentCount, sf2->sampleCount,
            sf2->sampleData ? 1 : 0, sf2->sampleDataFrames);
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(h, nullptr);
            LOG("  File exists and readable, size=%u bytes", sz);
            CloseHandle(h);
        } else {
            LOG("  File access error: %u", (unsigned)GetLastError());
        }
        error = "SoundFont parse failed";
        DestroySoundFontBundle(bundle);
        return nullptr;
    }
    LOG("  Parsed: %u presets, %u instruments, %u samples, %u frames",
        sf2->presetCount, sf2->instrumentCount, sf2->sampleCount, sf2->sampleDataFrames);

    sf2_build_regions(sf2);
    LOG("  Built %u regions", sf2->regionCount);
    if (sf2->regionOverflow) {
        LOG("  FAILED: SoundFont compiled region capacity exceeded");
        error = "SoundFont compiled region capacity exceeded";
        DestroySoundFontBundle(bundle);
        return nullptr;
    }
    // Diagnostic peak inspection walks up to 512 source samples.  Doing
    // that for every configured voice made diagnostics catastrophically
    // expensive in dense MIDI.  Compile the immutable values once while
    // loading off the audio thread; note-on becomes a single cached read.
    if (sf2->regionCount != 0u) {
        bundle->regionInitialPeaks = static_cast<float*>(
            malloc(static_cast<size_t>(sf2->regionCount) * sizeof(float)));
        if (bundle->regionInitialPeaks) {
            bundle->regionInitialPeakCount = sf2->regionCount;
            for (uint32_t region = 0; region < sf2->regionCount; ++region) {
                bundle->regionInitialPeaks[region] =
                    sf2_region_initial_peak(sf2, &sf2->regions[region]);
            }
        }
        bundle->preparedRegions = static_cast<PreparedSF2Region*>(malloc(
            static_cast<size_t>(sf2->regionCount) * sizeof(PreparedSF2Region)));
        if (bundle->preparedRegions) {
            bundle->preparedRegionCount = sf2->regionCount;
            for (uint32_t region = 0; region < sf2->regionCount; ++region) {
                PrepareSF2Region(sf2, sf2->regions[region], sampleRate,
                                 channelCache, bundle->preparedRegions[region]);
            }
        }
        if (!bundle->preparedRegions) {
            error = "not enough memory to prepare SoundFont regions";
            DestroySoundFontBundle(bundle);
            return nullptr;
        }
    }

    if (sf2->sampleData) {
        const uint32_t frames = sf2->sampleDataFrames;
        // 16-bit sample store.  The previous float store held exact
        // float(int16) values, so rendering converts on load with identical
        // math at half the cache footprint.  Eight zero elements of
        // trailing padding keep the AVX2 pair-word gather in bounds.
        int16_t* sbuf = static_cast<int16_t*>(malloc(
            (static_cast<size_t>(frames) + 8u) * sizeof(int16_t)));
        if (!sbuf) {
            error = "not enough memory to convert SoundFont samples";
            DestroySoundFontBundle(bundle);
            return nullptr;
        }
        std::memcpy(sbuf, sf2->sampleData,
                    static_cast<size_t>(frames) * sizeof(int16_t));
        std::memset(sbuf + frames, 0, 8u * sizeof(int16_t));
        bundle->sampleData = sbuf;
        bundle->sampleDataFrames = frames;
    }

    const uint32_t sampCount = sf2->sampleCount;
    if (sampCount != 0u) {
        bundle->samples = static_cast<SF2Sample*>(
            malloc(static_cast<size_t>(sampCount) * sizeof(SF2Sample)));
        if (!bundle->samples) {
            error = "not enough memory to cache SoundFont sample headers";
            DestroySoundFontBundle(bundle);
            return nullptr;
        }
        std::memcpy(bundle->samples, sf2->samples,
                    static_cast<size_t>(sampCount) * sizeof(SF2Sample));
    }
    bundle->sampleCount = sampCount;
    if (!bundle->sampleData || bundle->sampleDataFrames == 0u ||
        !bundle->samples || bundle->sampleCount == 0u) {
        error = "SoundFont contains no usable sample data";
        DestroySoundFontBundle(bundle);
        return nullptr;
    }
    // Region compilation and diagnostic peaks are complete. Rendering uses
    // the immutable 16-bit store, so retaining the original 16-bit RIFF blob
    // would only duplicate every loaded bank for its entire lifetime.
    free(sf2->sampleData);
    sf2->sampleData = nullptr;
    sf2->sampleDataSize = 0u;
    LOG("  SoundFont bundle ready: %u samples cached", sampCount);
    return bundle;
}

SoundFontBundle* Driver::BuildSoundFontStackBundle(
    const std::vector<std::wstring>& paths,
    const std::vector<SoundFontRoute>& routes,
    uint64_t requestId, std::string& error) {
    if (paths.empty()) {
        error = "no SoundFont path supplied";
        return nullptr;
    }
    if (paths.size() == 1u && routes.empty())
        return BuildSoundFontBundle(paths.front().c_str(), requestId, error);

    SoundFontBundle* stack = new (std::nothrow) SoundFontBundle();
    if (!stack) {
        error = "not enough memory to create SoundFont stack";
        return nullptr;
    }
    stack->requestId = requestId;
    stack->path = paths.front();
    uint64_t totalFrames = 0u;
    const uint32_t requestedCount = static_cast<uint32_t>((std::min)(
        paths.size(), static_cast<size_t>(kMaxSoundFontStackEntries)));
    for (uint32_t i = 0u; i < requestedCount; ++i) {
        std::string bankError;
        SoundFontBundle* bank = BuildSoundFontBundle(
            paths[i].c_str(), requestId, bankError);
        if (!bank) {
            error = "SoundFont " + std::to_string(i + 1u) + " failed: " +
                    (bankError.empty() ? "load failed" : bankError);
            DestroySoundFontBundle(stack);
            return nullptr;
        }
        if (totalFrames + bank->sampleDataFrames > UINT32_MAX) {
            error = "combined SoundFont sample data exceeds 32-bit offsets";
            DestroySoundFontBundle(bank);
            DestroySoundFontBundle(stack);
            return nullptr;
        }
        bank->sampleBase = static_cast<uint32_t>(totalFrames);
        const uint64_t nextFrames = totalFrames + bank->sampleDataFrames;
        int16_t* grown = static_cast<int16_t*>(realloc(
            stack->sampleData,
            (static_cast<size_t>(nextFrames) + 8u) * sizeof(int16_t)));
        if (!grown) {
            error = "not enough memory to combine SoundFont samples";
            DestroySoundFontBundle(bank);
            DestroySoundFontBundle(stack);
            return nullptr;
        }
        stack->sampleData = grown;
        std::memcpy(stack->sampleData + bank->sampleBase, bank->sampleData,
                    static_cast<size_t>(bank->sampleDataFrames) * sizeof(int16_t));
        std::memset(stack->sampleData + nextFrames, 0, 8u * sizeof(int16_t));
        free(bank->sampleData);
        bank->sampleData = nullptr;
        totalFrames = nextFrames;
        stack->sampleDataFrames = static_cast<uint32_t>(totalFrames);
        stack->banks[stack->bankCount++] = bank;
    }
    for (const SoundFontRoute& route : routes) {
        if (stack->routeCount >= kMaxSoundFontRoutes) break;
        if (route.soundFontIndex >= stack->bankCount) continue;
        stack->routes[stack->routeCount++] = route;
    }
    LOG("  SoundFont stack ready: %u banks, %u routes, %u frames",
        stack->bankCount, stack->routeCount, stack->sampleDataFrames);
    return stack;
}

void Driver::RetireSoundFontBundle(SoundFontBundle* bundle) noexcept {
    if (!bundle) return;
    SoundFontBundle* head = retiredSoundFontBundles_.load(
        std::memory_order_relaxed);
    do {
        bundle->retiredNext = head;
    } while (!retiredSoundFontBundles_.compare_exchange_weak(
        head, bundle, std::memory_order_release, std::memory_order_relaxed));
}

void Driver::ReclaimRetiredSoundFonts() noexcept {
    SoundFontBundle* bundle = retiredSoundFontBundles_.exchange(
        nullptr, std::memory_order_acquire);
    while (bundle) {
        SoundFontBundle* next = bundle->retiredNext;
        DestroySoundFontBundle(bundle);
        bundle = next;
    }
}

std::wstring Driver::CopyActiveSoundFontPath() const {
    SoundFontBundle* bundle = activeSoundFontBundle_.load(
        std::memory_order_acquire);
    return bundle ? bundle->path : std::wstring();
}

void Driver::PublishSoundFontBundle(SoundFontBundle* bundle) noexcept {
    SoundFontBundle* superseded = pendingSoundFontBundle_.exchange(
        bundle, std::memory_order_acq_rel);
    // Publication runs off the callback. A pending bundle that lost the race
    // was never visible to rendering and can be reclaimed immediately here.
    DestroySoundFontBundle(superseded);
}

void Driver::ActivatePendingSoundFontAtBlockBoundary() noexcept {
    SoundFontBundle* next = pendingSoundFontBundle_.exchange(
        nullptr, std::memory_order_acquire);
    if (!next) return;

    // Voice sample locations are offsets into one bundle. Old voices must not
    // survive into the new sample bank; MIDI channel/program/controller state
    // remains intact and is remapped below.
    if (voiceManager) voiceManager->Reset();

    activeSoundFontStack_ = next;
    SoundFontBundle* primary = SoundFontBankAt(next, 0u);
    soundFontData = primary ? primary->data : nullptr;
    sampleDataStore = next->sampleData;
    samplesStore = primary ? primary->samples : nullptr;
    regionInitialPeaks = primary ? primary->regionInitialPeaks : nullptr;
    preparedRegions = primary ? primary->preparedRegions : nullptr;
    sampleStoreCount = primary ? primary->sampleCount : 0u;
    sampleDataFrames = next->sampleDataFrames;
    regionInitialPeakCount = primary ? primary->regionInitialPeakCount : 0u;
    preparedRegionCount = primary ? primary->preparedRegionCount : 0u;

    if (++soundFontGeneration_ == 0u) {
        soundFontGeneration_ = 1u;
        std::memset(noteLaunchPlanCache_, 0, sizeof(noteLaunchPlanCache_));
    }
    std::memset(noteRegionCache_, 0xff, sizeof(noteRegionCache_));
    RefreshSelectedPresets();
    for (uint32_t channel = 0; channel < kChannelCount; ++channel)
        ++channelLaunchRevision_[channel];
    nextPlayIndex_ = 1u;

    SoundFontBundle* old = activeSoundFontBundle_.exchange(
        next, std::memory_order_release);
    soundFontActivatedId_.store(next->requestId, std::memory_order_release);
#if !defined(SVMS_XP_COMPAT)
    if (soundFontRequestId_.load(std::memory_order_acquire) == next->requestId)
        soundFontLoadState_.store(3u, std::memory_order_release);
#endif
    RetireSoundFontBundle(old);
}

void Driver::DestroyAllSoundFontBundles() noexcept {
    DestroySoundFontBundle(pendingSoundFontBundle_.exchange(
        nullptr, std::memory_order_acq_rel));
    DestroySoundFontBundle(activeSoundFontBundle_.exchange(
        nullptr, std::memory_order_acq_rel));
    ReclaimRetiredSoundFonts();
    soundFontData = nullptr;
    activeSoundFontStack_ = nullptr;
    sampleDataStore = nullptr;
    samplesStore = nullptr;
    regionInitialPeaks = nullptr;
    preparedRegions = nullptr;
    sampleStoreCount = sampleDataFrames = 0u;
    regionInitialPeakCount = preparedRegionCount = 0u;
}

bool Driver::LoadSoundFont(const wchar_t* path) {
    if (!path || !*path) return false;
    const uint64_t requestId = soundFontRequestId_.fetch_add(
        1u, std::memory_order_acq_rel) + 1u;
    std::string error;
    EnterCriticalSection(&soundFontBuildCs_);
    SoundFontBundle* bundle = BuildSoundFontBundle(path, requestId, error);
    LeaveCriticalSection(&soundFontBuildCs_);
    if (!bundle) return false;
#if !defined(SVMS_XP_COMPAT)
    soundFontLoadState_.store(2u, std::memory_order_release);
#endif
    PublishSoundFontBundle(bundle);
    if (!audioOutput || !audioOutput->IsRunning()) {
        ActivatePendingSoundFontAtBlockBoundary();
#if defined(SVMS_XP_COMPAT)
        ReclaimRetiredSoundFonts();
#endif
    }

    return true;
}

#if !defined(SVMS_XP_COMPAT)
bool Driver::QueueSoundFontLoad(const std::wstring& path,
                                uint64_t& requestId) {
    requestId = 0u;
    if (path.empty() || !soundFontLoadEvent_ ||
        soundFontLoaderStop_.load(std::memory_order_acquire)) return false;
    requestId = soundFontRequestId_.fetch_add(
        1u, std::memory_order_acq_rel) + 1u;
    EnterCriticalSection(&cs);
    requestedSoundFontPath_ = path;
    requestedSoundFontId_ = requestId;
    soundFontLoadError_.clear();
    soundFontLoadState_.store(1u, std::memory_order_release);
    LeaveCriticalSection(&cs);
    SetEvent(soundFontLoadEvent_);
    return true;
}

void Driver::SoundFontLoaderLoop() {
    uint64_t handledId = 0u;
    for (;;) {
        WaitForSingleObject(soundFontLoadEvent_, INFINITE);
        if (soundFontLoaderStop_.load(std::memory_order_acquire)) break;

        for (;;) {
            std::wstring path;
            uint64_t requestId = 0u;
            EnterCriticalSection(&cs);
            requestId = requestedSoundFontId_;
            path = requestedSoundFontPath_;
            LeaveCriticalSection(&cs);
            if (requestId == 0u || requestId == handledId) break;
            handledId = requestId;

            std::string error;
            EnterCriticalSection(&soundFontBuildCs_);
            SoundFontBundle* bundle = BuildSoundFontBundle(
                path.c_str(), requestId, error);
            LeaveCriticalSection(&soundFontBuildCs_);

            if (soundFontLoaderStop_.load(std::memory_order_acquire)) {
                DestroySoundFontBundle(bundle);
                return;
            }
            if (soundFontRequestId_.load(std::memory_order_acquire) !=
                    requestId) {
                DestroySoundFontBundle(bundle);
                continue;
            }
            if (!bundle) {
                EnterCriticalSection(&cs);
                soundFontLoadError_ = error.empty()
                    ? "SoundFont load failed" : error;
                LeaveCriticalSection(&cs);
                soundFontLoadState_.store(4u, std::memory_order_release);
                continue;
            }

            soundFontLoadState_.store(2u, std::memory_order_release);
            PublishSoundFontBundle(bundle);
            if (!audioOutput || !audioOutput->IsRunning()) {
                ActivatePendingSoundFontAtBlockBoundary();
            }
        }
    }
}
#endif

bool Driver::IsInitialized() const {
    return initialized;
}

void Driver::CopyDebugInfo(DriverDebugInfo& out) const {
    const uint32_t index = debugSnapshotIndex_.load(std::memory_order_acquire) & 1u;
    out = debugSnapshots_[index];
    // These fields are useful even before the first render callback publishes
    // a snapshot (for example when WASAPI failed to start its event loop).
    out.soundFontLoaded = soundFontData && sampleDataStore ? 1u : 0u;
    out.sampleDataFrames = sampleDataFrames;
    out.sampleCount = sampleStoreCount;
    out.audioRunning = audioOutput && audioOutput->IsRunning() ? 1u : 0u;
    out.audioHResult = audioOutput ? static_cast<int32_t>(audioOutput->GetLastError()) : 0;
}

void Driver::CopyVoiceStatistics(SnappyVoiceStatistics& out) const {
    const uint32_t index = debugSnapshotIndex_.load(std::memory_order_acquire) & 1u;
    out = voiceStatisticsSnapshots_[index];
}

float Driver::GetRenderingTimeMilliseconds() const {
    const uint32_t index = debugSnapshotIndex_.load(std::memory_order_acquire) & 1u;
    return renderingTimeSnapshots_[index];
}

const LegacyDriverDebugInfo* Driver::GetLegacyDebugInfo() const {
    const uint32_t index = debugSnapshotIndex_.load(std::memory_order_acquire) & 1u;
    return &legacyDebugSnapshots_[index];
}

bool Driver::StartAudio() {
    if (audioOutput && !audioOutput->IsRunning()) {
        LOG("StartAudio: starting audio stream...");
        const bool ok = audioOutput->Start();
        LOG("StartAudio: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) {
            XPBootstrapTrace("[SVMS XP] audio stream start FAILED\r\n");
            DiagWindow_UpdateStartup(false,
                                     static_cast<int32_t>(audioOutput->GetLastError()),
                                     soundFontData && sampleDataStore,
                                     sampleRate, bufferFrames,
                                     engineConfig_.masterVolume,
                                     UsesXPWaveOut(audioOutput));
        }
        if (ok && engineConfig_.midiInputEnabled && !midiInput_)
            (void)StartConfiguredMidiInput();
        return ok;
    }
    const bool running = audioOutput && audioOutput->IsRunning();
    if (running && engineConfig_.midiInputEnabled && !midiInput_)
        (void)StartConfiguredMidiInput();
    return running;
}

bool Driver::StartConfiguredMidiInput() {
    StopConfiguredMidiInput();
    if (!engineConfig_.midiInputEnabled) return true;

    using GetNumProc = UINT (WINAPI*)(void);
    using GetCapsProc = MMRESULT (WINAPI*)(UINT_PTR, LPMIDIINCAPSW, UINT);
    using OpenProc = MMRESULT (WINAPI*)(LPHMIDIIN, UINT, DWORD_PTR,
                                       DWORD_PTR, DWORD);
    using HeaderProc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    using StartProc = MMRESULT (WINAPI*)(HMIDIIN);
    GetNumProc getNum = reinterpret_cast<GetNumProc>(
        GetSystemWinmmProc("midiInGetNumDevs"));
    GetCapsProc getCaps = reinterpret_cast<GetCapsProc>(
        GetSystemWinmmProc("midiInGetDevCapsW"));
    OpenProc open = reinterpret_cast<OpenProc>(
        GetSystemWinmmProc("midiInOpen"));
    HeaderProc prepare = reinterpret_cast<HeaderProc>(
        GetSystemWinmmProc("midiInPrepareHeader"));
    HeaderProc addBuffer = reinterpret_cast<HeaderProc>(
        GetSystemWinmmProc("midiInAddBuffer"));
    StartProc start = reinterpret_cast<StartProc>(
        GetSystemWinmmProc("midiInStart"));
    if (!getNum || !getCaps || !open || !prepare || !addBuffer || !start) {
        OutputDebugStringA("[SVMS] MIDI input routing unavailable: system WinMM exports missing\n");
        return false;
    }

    const UINT deviceCount = getNum();
    if (deviceCount == 0u) {
        OutputDebugStringA("[SVMS] MIDI input routing enabled but no physical input devices were found\n");
        return false;
    }

    UINT selected = 0u;
    const std::wstring& requested = engineConfig_.midiInputDevice;
    if (!requested.empty() && _wcsicmp(requested.c_str(), L"default") != 0) {
        bool found = false;
        for (UINT index = 0u; index < deviceCount; ++index) {
            MIDIINCAPSW caps{};
            if (getCaps(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
                _wcsicmp(caps.szPname, requested.c_str()) == 0) {
                selected = index;
                found = true;
                break;
            }
        }
        if (!found) {
            const std::string requestedUtf8 = [&]() {
                if (requested.empty()) return std::string{};
                const int bytes = WideCharToMultiByte(
                    CP_UTF8, 0, requested.data(), static_cast<int>(requested.size()),
                    nullptr, 0, nullptr, nullptr);
                std::string value(bytes > 0 ? static_cast<size_t>(bytes) : 0u, '\0');
                if (bytes > 0) WideCharToMultiByte(
                    CP_UTF8, 0, requested.data(), static_cast<int>(requested.size()),
                    value.data(), bytes, nullptr, nullptr);
                return value;
            }();
            std::string warning = "[SVMS] configured MIDI input was not found: " +
                                  requestedUtf8 + "\n";
            OutputDebugStringA(warning.c_str());
            return false;
        }
    }

    MMRESULT result = open(&midiInput_, selected,
        reinterpret_cast<DWORD_PTR>(&Driver::MidiInputCallback),
        reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR || !midiInput_) {
        midiInput_ = nullptr;
        OutputDebugStringA("[SVMS] configured MIDI input could not be opened\n");
        return false;
    }

    uint32_t prepared = 0u;
    for (; prepared < kMidiInputBufferCount; ++prepared) {
        MIDIHDR& header = midiInputHeaders_[prepared];
        std::memset(&header, 0, sizeof(header));
        header.lpData = midiInputData_[prepared];
        header.dwBufferLength = kMidiInputBufferBytes;
        if (prepare(midiInput_, &header, sizeof(header)) != MMSYSERR_NOERROR ||
            addBuffer(midiInput_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            break;
        }
    }
    midiInputRunning_.store(true, std::memory_order_release);
    if (prepared != kMidiInputBufferCount ||
        start(midiInput_) != MMSYSERR_NOERROR) {
        StopConfiguredMidiInput();
        OutputDebugStringA("[SVMS] configured MIDI input buffers could not be started\n");
        return false;
    }
    OutputDebugStringA("[SVMS] configured physical MIDI input routing started\n");
    return true;
}

void Driver::StopConfiguredMidiInput() noexcept {
    midiInputRunning_.store(false, std::memory_order_release);
    HMIDIIN input = midiInput_;
    if (!input) return;

    using SimpleProc = MMRESULT (WINAPI*)(HMIDIIN);
    using HeaderProc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    SimpleProc stop = reinterpret_cast<SimpleProc>(
        GetSystemWinmmProc("midiInStop"));
    SimpleProc reset = reinterpret_cast<SimpleProc>(
        GetSystemWinmmProc("midiInReset"));
    SimpleProc close = reinterpret_cast<SimpleProc>(
        GetSystemWinmmProc("midiInClose"));
    HeaderProc unprepare = reinterpret_cast<HeaderProc>(
        GetSystemWinmmProc("midiInUnprepareHeader"));
    if (stop) (void)stop(input);
    if (reset) (void)reset(input);
    if (unprepare) {
        for (MIDIHDR& header : midiInputHeaders_) {
            if ((header.dwFlags & MHDR_PREPARED) != 0u)
                (void)unprepare(input, &header, sizeof(header));
        }
    }
    if (close) (void)close(input);
    midiInput_ = nullptr;
    std::memset(midiInputHeaders_, 0, sizeof(midiInputHeaders_));
}

void CALLBACK Driver::MidiInputCallback(HMIDIIN input, UINT message,
                                        DWORD_PTR instance,
                                        DWORD_PTR parameter1,
                                        DWORD_PTR) {
    Driver* self = reinterpret_cast<Driver*>(instance);
    if (!self || !self->midiInputRunning_.load(std::memory_order_acquire))
        return;
    if (message == MIM_DATA) {
        LARGE_INTEGER timestamp{};
        QueryPerformanceCounter(&timestamp);
        self->SubmitShortMsgAtQpc(
            static_cast<uint32_t>(parameter1),
            static_cast<uint64_t>(timestamp.QuadPart));
        return;
    }
    if (message != MIM_LONGDATA) return;

    MIDIHDR* header = reinterpret_cast<MIDIHDR*>(parameter1);
    if (!header) return;
    if (header->dwBytesRecorded != 0u && header->lpData)
        self->SubmitSystemExclusive(
            reinterpret_cast<const uint8_t*>(header->lpData),
            header->dwBytesRecorded);
    header->dwBytesRecorded = 0u;
    if (!self->midiInputRunning_.load(std::memory_order_acquire)) return;
    using HeaderProc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    HeaderProc addBuffer = reinterpret_cast<HeaderProc>(
        GetSystemWinmmProc("midiInAddBuffer"));
    if (addBuffer) (void)addBuffer(input, header, sizeof(*header));
}

void Driver::ResetAllVoices() {
    if (audioOutput && audioOutput->IsRunning()) {
        SubmitShortMsg(kInternalResetMessage);
        return;
    }
    if (voiceManager) voiceManager->Reset();
    if (channelCache) channelCache->Reset();
    sysexMasterVolume_ = 1.0f;
    sysexMasterFineTune_ = 0.0f;
    sysexMasterTranspose_ = 0.0f;
    if (channelCache && configSnapshot)
        channelCache->SetMasterVolume(configSnapshot->masterVolume);
    RefreshSelectedPresets();
    std::fill(std::begin(channelPitchBendRatio_),
              std::end(channelPitchBendRatio_), 1.0f);
    for (uint32_t channel = 0; channel < kChannelCount; ++channel)
        ++channelLaunchRevision_[channel];
    nextPlayIndex_ = 1;
    eventScheduler_.Reset();
    postHighPass.Reset();
    reverb.Reset();
    limiter.Reset();
}

void Driver::SubmitShortMsg(uint32_t msg) {
    LARGE_INTEGER timestamp{};
    QueryPerformanceCounter(&timestamp);
    SubmitShortMsgAtQpc(msg, static_cast<uint64_t>(timestamp.QuadPart));
}

void Driver::SubmitShortMsgAtFrame(uint32_t msg, uint64_t outputFrame) {
    (void)SubmitShortMsgAtFrameCancellable(msg, outputFrame, nullptr, 0u);
}

bool Driver::SubmitShortMsgAtFrameCancellable(
    uint32_t msg, uint64_t outputFrame,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    return SubmitShortMsgAtQpcCancellable(
        msg, kAbsoluteFrameTimestampTag |
                 (outputFrame & kAbsoluteFrameTimestampMask),
        externalCancellation, cancellationToken);
}

void Driver::WakeBlockedProducers() {
    producerWakeEpoch_.fetch_add(1u, std::memory_order_release);
    WakeAddressWaiters(producerWakeEpoch_);
}

void Driver::SetIngressMode(EventOverflowMode mode) {
    overflowMode_.store(mode, std::memory_order_release);
}

void Driver::SetNoteOnCollapseThreshold(uint32_t threshold) {
    noteOnCollapse_.SetThreshold(threshold);
}

void Driver::EnableNoteOnCollapse(bool enable) {
    noteOnCollapse_.SetThreshold(enable ? kNoteOnCollapseDefaultThreshold : 1u);
}

void Driver::CopyNativeQueueInfo(SVMS_QueueInfo& out) const {
    out = {};
    out.struct_size = sizeof(out);
    out.struct_version = SVMS_STRUCT_VERSION_1;
    out.ingress_mode = overflowMode_.load(std::memory_order_acquire) ==
            EventOverflowMode::LosslessBackpressure
        ? SVMS_INGRESS_LOSSLESS : SVMS_INGRESS_PRIORITY;
    out.current_velocity_cutoff = currentVelocityCutoffAtomic_.load(
        std::memory_order_relaxed);
    out.queue_capacity = midiIngress_.TotalCapacity();
    out.raw_ingress_count = midiIngress_.TotalSize();
    out.compiled_count = compiledPages_.ReadyEventCount();
    out.scheduled_count = scheduledSizePublished_.load(
        std::memory_order_acquire);
    out.max_events_per_callback = maxEventsPerBlock_;
    out.submitted_events = submittedAtomic_.load(std::memory_order_relaxed);
    out.accepted_events = acceptedAtomic_.load(std::memory_order_relaxed);
    out.intentionally_shed_events = shedAtomic_.load(
        std::memory_order_relaxed);
    out.cancelled_submissions = cancelledAtomic_.load(
        std::memory_order_relaxed);
}

uint64_t Driver::GetNextOutputFrame() const {
    return outputFramePublished_.load(std::memory_order_acquire);
}

void Driver::SubmitShortMsgAtQpc(uint32_t msg, uint64_t qpcTimestamp) {
    (void)SubmitShortMsgAtQpcCancellable(msg, qpcTimestamp, nullptr, 0u);
}

uint32_t Driver::RefreshVelocityCutoff(EventLane lane) noexcept {
    const uint32_t rawIngress = midiIngress_.TotalSize();
    const uint32_t compiledIngress = compiledPages_.ReadyEventCount();
    const uint32_t scheduled =
        scheduledSizePublished_.load(std::memory_order_acquire);
    const uint32_t rawIngressPressure = static_cast<uint32_t>(
        static_cast<uint64_t>(rawIngress) * 100u /
        midiIngress_.TotalCapacity());
    const uint32_t compiledCapacity = pagedScheduler_.Capacity();
    const uint32_t compiledIngressPressure = compiledCapacity != 0u
        ? static_cast<uint32_t>(
              static_cast<uint64_t>(compiledIngress) * 100u /
              compiledCapacity)
        : 100u;
    const uint32_t laneCapacity = midiIngress_.LaneCapacity(lane);
    const uint32_t lanePressure = static_cast<uint32_t>(
        static_cast<uint64_t>(midiIngress_.LaneSize(lane)) * 100u /
        laneCapacity);
    const uint32_t scheduledCapacity = useEventCompiler_
        ? pagedScheduler_.Capacity() : eventScheduler_.Capacity();
    const uint32_t scheduledPressure = scheduledCapacity != 0u
        ? static_cast<uint32_t>(
              static_cast<uint64_t>(scheduled) * 100u /
              scheduledCapacity)
        : 100u;
    const uint32_t pressure = (std::max)(
        lanePressure,
        (std::max)(rawIngressPressure,
            (std::max)(compiledIngressPressure, scheduledPressure)));
    uint32_t cutoff = 1u;
    if (pressure > shedStartPercent_) {
        const uint32_t range = 100u - shedStartPercent_;
        cutoff = 1u +
            ((std::min)(pressure - shedStartPercent_, range) * 94u) / range;
    }
    currentVelocityCutoffAtomic_.store(cutoff, std::memory_order_relaxed);
    return cutoff;
}

bool Driver::SubmitShortMsgAtQpcCancellable(
    uint32_t msg, uint64_t qpcTimestamp,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    submittedAtomic_.fetch_add(1, std::memory_order_relaxed);
    TimestampedMidiEvent evt{};
    evt.message = msg;
    evt.sequence = nextEventSequence_.fetch_add(1, std::memory_order_relaxed);
    evt.qpcTimestamp = qpcTimestamp;

    if (externalCancellation && externalCancellation->load(
            std::memory_order_acquire) == cancellationToken) {
        cancelledAtomic_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint8_t status = static_cast<uint8_t>(msg & 0xffu);
    const uint8_t data1 = static_cast<uint8_t>((msg >> 8) & 0x7fu);
    uint8_t velocity = static_cast<uint8_t>((msg >> 16) & 0x7fu);
    const bool noteOn = !IsInternalEngineMessage(msg) &&
                        (status & 0xf0u) == 0x90u && velocity != 0;

    // Publish the cutoff before waiting on the lossless state lane.  If that
    // lane overtakes a backlog of older note-ons, those notes are rejected at
    // dispatch instead of resurrecting sound after pause/mute.
    if (msg == kInternalResetMessage) {
        PublishTerminationFence(globalTerminationFence_, evt.sequence);
    } else if ((status & 0xf0u) == 0xb0u &&
               (data1 == 120u || data1 == 123u)) {
        PublishTerminationFence(channelTerminationFence_[status & 0x0fu], evt.sequence);
    }

    // ── Same-key note-on coalescing (DEFAULT OFF) ────────────────────────
    // Runs after cancellation/fence bookkeeping and before lane selection:
    // collapsed duplicates never touch the ingress queues, lanes, or the
    // audio thread at all. See SVMSNoteOnCollapse.h for the rationale.
    // When disabled (the default) every note-on spawns at its exact QPC
    // timestamp — retrigger/buzz precision is untouched.
    if (noteOn) {
        const uint32_t keyIndex =
            static_cast<uint32_t>(status & 0x0fu) * kNoteCount + data1;
        uint32_t stack = 0u;
        if (!noteOnCollapse_.OnNoteOn(keyIndex, qpcTimestamp, stack)) {
            coalescedAtomic_.fetch_add(1u, std::memory_order_relaxed);
            return true;
        }
        // ── Velocity stacking ───────────────────────────────────────────
        // The collapsed hits between two spawns are density, not silence.
        // Feed the accumulated stack into the spawned event's velocity on
        // a log2 curve (2 velocity units per doubling) so hammered keys
        // read as louder, not quieter, matching SnappySynth's stack counter.
        // Boosting here also lifts the event into a higher priority lane
        // and above the velocity shed cutoff.
        if (stack > 1u && velocity < 127u) {
            uint32_t boost = 0u;
            for (uint32_t s = stack; s >>= 1u;) ++boost;
            boost *= 2u;
            const uint32_t boosted =
                velocity + boost > 127u ? 127u : velocity + boost;
            if (boosted != velocity) {
                velocity = static_cast<uint8_t>(boosted);
                evt.message = (evt.message & ~0x007f0000u) |
                              (static_cast<uint32_t>(velocity) << 16);
            }
        }
    } else if (!IsInternalEngineMessage(msg)) {
        const uint8_t statusType = status & 0xf0u;
        // Note-offs intentionally do NOT reset the per-key window/stack:
        // clearing on every note-off would let interleaved on/off floods
        // spawn a voice per event again.  Panic-style state messages
        // still clear, matching their "start over" semantics.
        if (statusType == 0xb0u &&
            (data1 == 120u || data1 == 123u)) {
            noteOnCollapse_.ResetChannel(status & 0x0fu);
        }
    } else if (msg == kInternalResetMessage) {
        noteOnCollapse_.ResetAll();
    }

    EventLane lane = EventLane::State;
    if (noteOn) {
        if (velocity >= highPriorityVelocity_) lane = EventLane::Loud;
        else if (velocity >= 64) lane = EventLane::UpperMedium;
        else if (velocity >= 32) lane = EventLane::Medium;
        else lane = EventLane::Quiet;
    }

    // Queue pressure changes much more slowly than MIDI arrives. Sampling it
    // once per 256 global events removes twelve shared atomic loads from the
    // ordinary producer path while still reacting within a fraction of one
    // audio block at extreme rates.
    uint32_t cutoff = currentVelocityCutoffAtomic_.load(
        std::memory_order_relaxed);
    if ((evt.sequence & 0xffu) == 0u) {
        cutoff = RefreshVelocityCutoff(lane);
    }

    const EventOverflowMode overflowMode =
        overflowMode_.load(std::memory_order_relaxed);
    if (noteOn && velocity < cutoff &&
        overflowMode == EventOverflowMode::PriorityVelocity) {
        shedAtomic_.fetch_add(1, std::memory_order_relaxed);
        shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const bool lossless = !noteOn || velocity >= highPriorityVelocity_ ||
                          overflowMode == EventOverflowMode::LosslessBackpressure;
    for (;;) {
        if (midiIngress_.TryPush(lane, evt)) {
            acceptedAtomic_.fetch_add(1, std::memory_order_relaxed);
            // A running compiler will observe the queue without help. Only
            // cross the kernel/API boundary when it explicitly published
            // that it is asleep.
            if (compilerSleeping_.exchange(false,
                    std::memory_order_acq_rel)) {
                compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
                WakeAddressWaiters(compilerWakeEpoch_);
            }
#if defined(SVMS_XP_COMPAT)
            static LONG acceptedTraceCount = 0;
            const LONG acceptedIndex = InterlockedIncrement(&acceptedTraceCount);
            if (acceptedIndex <= 32) {
                char message[224] = {};
                std::snprintf(message, sizeof(message),
                              "[SVMS XP] ingress accepted #%ld seq=%lu lane=%u queued=%lu\r\n",
                              static_cast<long>(acceptedIndex),
                              static_cast<unsigned long>(evt.sequence),
                              static_cast<unsigned>(lane),
                              static_cast<unsigned long>(midiIngress_.TotalSize()));
                OutputDebugStringA(message);
            }
#endif
            return true;
        }
        if (!lossless) {
            shedAtomic_.fetch_add(1, std::memory_order_relaxed);
            shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (cancelProducers_.load(std::memory_order_acquire) ||
            (externalCancellation && externalCancellation->load(
                std::memory_order_acquire) == cancellationToken)) {
            cancelledAtomic_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        uint32_t observed = producerWakeEpoch_.load(std::memory_order_acquire);
        WaitForAddressChange(producerWakeEpoch_, observed);
    }
}

bool Driver::SubmitShortBatchAtQpcCancellable(
    const SVMS_ShortEvent* events, uint32_t eventCount,
    uint64_t immediateQpc,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (!events && eventCount != 0u) return false;

    // Full-velocity Black MIDI batches overwhelmingly occupy the loud lane.
    // Keep mixed/state/priority-shedding batches on the established per-event
    // path; homogeneous protected note-ons can reserve 256 lane cells with
    // one producer CAS while retaining one global sequence per event.
    bool homogeneousLoudNotes = eventCount != 0u;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        const uint32_t message = events[i].packed_message;
        const uint8_t status = static_cast<uint8_t>(message & 0xffu);
        const uint8_t velocity = static_cast<uint8_t>(
            (message >> 16u) & 0x7fu);
        if ((status & 0xf0u) != 0x90u || velocity == 0u ||
            velocity < highPriorityVelocity_) {
            homogeneousLoudNotes = false;
            break;
        }
    }
    if (!homogeneousLoudNotes) {
        for (uint32_t i = 0u; i < eventCount; ++i) {
            const uint64_t timestamp = events[i].timestamp_qpc != 0u
                ? events[i].timestamp_qpc : immediateQpc;
            if (!SubmitShortMsgAtQpcCancellable(
                    events[i].packed_message, timestamp,
                    externalCancellation, cancellationToken)) {
                return false;
            }
        }
        return true;
    }

    constexpr uint32_t kBatchReservation = 256u;
    TimestampedMidiEvent prepared[kBatchReservation]{};
    uint32_t cursor = 0u;
    while (cursor < eventCount) {
        if (cancelProducers_.load(std::memory_order_acquire) ||
            (externalCancellation && externalCancellation->load(
                std::memory_order_acquire) == cancellationToken)) {
            cancelledAtomic_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        const uint32_t laneCapacity = midiIngress_.LaneCapacity(EventLane::Loud);
        const uint32_t count = (std::min)(
            eventCount - cursor,
            (std::min)(kBatchReservation, laneCapacity));
        submittedAtomic_.fetch_add(count, std::memory_order_relaxed);
        const uint32_t firstSequence = nextEventSequence_.fetch_add(
            count, std::memory_order_relaxed);
        for (uint32_t i = 0u; i < count; ++i) {
            const SVMS_ShortEvent& source = events[cursor + i];
            prepared[i].message = source.packed_message;
            prepared[i].sequence = firstSequence + i;
            prepared[i].qpcTimestamp = source.timestamp_qpc != 0u
                ? source.timestamp_qpc : immediateQpc;
        }
        // Maintain the same pressure-sampling cadence as individual pushes.
        RefreshVelocityCutoff(EventLane::Loud);
        for (;;) {
            if (midiIngress_.TryPushBatch(
                    EventLane::Loud, prepared, count)) {
                acceptedAtomic_.fetch_add(count, std::memory_order_relaxed);
                if (compilerSleeping_.exchange(
                        false, std::memory_order_acq_rel)) {
                    compilerWakeEpoch_.fetch_add(1u,
                                                  std::memory_order_release);
                    WakeAddressWaiters(compilerWakeEpoch_);
                }
                cursor += count;
                break;
            }
            if (cancelProducers_.load(std::memory_order_acquire) ||
                (externalCancellation && externalCancellation->load(
                    std::memory_order_acquire) == cancellationToken)) {
                cancelledAtomic_.fetch_add(1u, std::memory_order_relaxed);
                return false;
            }
            const uint32_t observed = producerWakeEpoch_.load(
                std::memory_order_acquire);
            WaitForAddressChange(producerWakeEpoch_, observed);
        }
    }
    return true;
}

void Driver::SubmitSystemExclusive(const uint8_t* data, uint32_t size) {
    (void)SubmitSystemExclusiveCancellable(data, size, nullptr, 0u);
}

bool Driver::SubmitSystemExclusiveCancellable(
    const uint8_t* data, uint32_t size,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (!data || size < 2u || data[0] != 0xf0u ||
        data[size - 1u] != 0xf7u) return true;

    LARGE_INTEGER timestamp{};
    QueryPerformanceCounter(&timestamp);
    const uint64_t qpc = static_cast<uint64_t>(timestamp.QuadPart);
    bool accepted = true;
    auto emit = [&](uint32_t message) {
        if (accepted) {
            accepted = SubmitShortMsgAtQpcCancellable(
                message, qpc, externalCancellation, cancellationToken);
        }
    };
    auto emitCC = [&](uint8_t channel, uint8_t controller, uint8_t value) {
        emit(static_cast<uint32_t>(0xb0u | (channel & 0x0fu)) |
             (static_cast<uint32_t>(controller & 0x7fu) << 8u) |
             (static_cast<uint32_t>(value & 0x7fu) << 16u));
    };
    auto emitProgram = [&](uint8_t channel, uint8_t program) {
        emit(static_cast<uint32_t>(0xc0u | (channel & 0x0fu)) |
             (static_cast<uint32_t>(program & 0x7fu) << 8u));
    };

    // Universal non-realtime mode messages: GM1 on/off and GM2 on.  MSGS
    // returns to its basic GM/GS state for these, just as it does for GS
    // Reset. Device ID is intentionally accepted as either broadcast or a
    // concrete 7-bit device number.
    if (size >= 6u && data[1] == 0x7eu && data[3] == 0x09u &&
        (data[4] == 0x01u || data[4] == 0x02u || data[4] == 0x03u)) {
        emit(kInternalResetMessage);
        return accepted;
    }

    // Universal realtime Master Volume (14-bit, LSB then MSB).
    if (size >= 8u && data[1] == 0x7fu && data[3] == 0x04u &&
        data[4] == 0x01u) {
        const uint16_t value = static_cast<uint16_t>(
            (data[5] & 0x7fu) | ((data[6] & 0x7fu) << 7u));
        emit(MakeInternalMasterVolumeMessage(value));
        return accepted;
    }
    // Universal realtime Master Fine/Coarse Tuning. Fine tuning shares the
    // MIDI Tuning Standard's centered 14-bit representation; coarse tuning
    // shares XG's centered 7-bit semitone representation.
    if (size >= 8u && data[1] == 0x7fu && data[3] == 0x04u &&
        data[4] == 0x03u) {
        const uint16_t value = static_cast<uint16_t>(
            (data[5] & 0x7fu) | ((data[6] & 0x7fu) << 7u));
        emit(MakeInternalMasterFineTuneMessage(value));
        return accepted;
    }
    if (size >= 7u && data[1] == 0x7fu && data[3] == 0x04u &&
        data[4] == 0x04u) {
        emit(MakeInternalMasterTransposeMessage(data[5] & 0x7fu));
        return accepted;
    }

    // Roland GS Data Set 1 (DT1).  Verify the Roland checksum, then map the
    // MSGS-relevant system and part parameters onto exact-frame engine/MIDI
    // events. Bulk packets work too because the 7-bit GS address advances
    // for every data byte.
    if (size >= 11u && data[1] == 0x41u && data[3] == 0x42u &&
        data[4] == 0x12u) {
        uint32_t checksumSum = 0u;
        for (uint32_t i = 5u; i + 1u < size; ++i)
            checksumSum += data[i] & 0x7fu;
        if ((checksumSum & 0x7fu) != 0u) return true;

        uint8_t address0 = data[5] & 0x7fu;
        uint8_t address1 = data[6] & 0x7fu;
        uint8_t address2 = data[7] & 0x7fu;
        const uint32_t dataEnd = size - 2u; // checksum, F7
        for (uint32_t i = 8u; i < dataEnd; ++i) {
            const uint8_t value = data[i] & 0x7fu;
            if (address0 == 0x40u && address1 == 0x00u) {
                if (address2 == 0x7fu && value == 0u) {
                    emit(kInternalResetMessage);
                } else if (address2 == 0x04u) {
                    emit(MakeInternalMasterVolumeMessage(
                        static_cast<uint16_t>(value) * 129u));
                }
            } else if (address0 == 0x40u &&
                       (address1 & 0x70u) == 0x10u) {
                const uint8_t part = address1 & 0x0fu;
                const uint8_t channel = part == 0u ? 9u
                    : (part <= 9u ? static_cast<uint8_t>(part - 1u) : part);
                switch (address2) {
                    case 0x00u: emitCC(channel, 0u, value); break;
                    case 0x01u: emitProgram(channel, value); break;
                    case 0x15u:
                        emit(MakeInternalRhythmPartMessage(channel, value));
                        break;
                    case 0x19u: emitCC(channel, 7u, value); break;
                    case 0x1cu: emitCC(channel, 10u,
                                      value == 0u ? 64u : value); break;
                    default: break;
                }
            }

            if (++address2 == 0x80u) {
                address2 = 0u;
                if (++address1 == 0x80u) {
                    address1 = 0u;
                    address0 = static_cast<uint8_t>((address0 + 1u) & 0x7fu);
                }
            }
        }
        return accepted;
    }

    // Translate the useful XG system and multi-part subset into the same
    // timestamped command stream as ordinary MIDI. No parameter is applied
    // on this producer thread, and bulk parameter packets preserve byte order.
    (void)TranslateXGSystemExclusive(data, size, emit);
    return accepted;
}

void Driver::EventCompilerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // Small enough to stay cache-friendly, large enough that one 2048-frame
    // Black-MIDI callback arrives as only a handful of ordered runs.
    static constexpr uint32_t kCompilerChunkCapacity = 8192u;
    PriorityEventIngress<TimestampedMidiEvent>::OrderedMergeState mergeState{};

    auto publishProducerSpace = [this]() {
        producerWakeEpoch_.fetch_add(1, std::memory_order_release);
        WakeAddressWaiters(producerWakeEpoch_);
    };

    while (!cancelProducers_.load(std::memory_order_acquire)) {
        const uint64_t epoch = compilerEpochQPC_.load(std::memory_order_acquire);
        if (epoch == 0u) {
            compilerSleeping_.store(true, std::memory_order_release);
            const uint32_t observed = compilerWakeEpoch_.load(std::memory_order_acquire);
            if (compilerEpochQPC_.load(std::memory_order_acquire) == 0u)
                WaitForAddressChange(compilerWakeEpoch_, observed);
            compilerSleeping_.store(false, std::memory_order_release);
            continue;
        }

        TimestampedMidiEvent timed{};
        if (!midiIngress_.TryPopSequenceOrdered(timed, mergeState)) {
            compilerSleeping_.store(true, std::memory_order_release);
            const uint32_t observed = compilerWakeEpoch_.load(std::memory_order_acquire);
            // Close the store-to-sleep race: a producer that arrived before
            // the epoch load either wakes us or is observed by this retry.
            if (!midiIngress_.TryPopSequenceOrdered(timed, mergeState)) {
                WaitForAddressChange(compilerWakeEpoch_, observed);
                compilerSleeping_.store(false, std::memory_order_release);
                continue;
            }
            compilerSleeping_.store(false, std::memory_order_release);
        }

        uint32_t pageIndex = kInvalidEventPage;
        while (!compiledPages_.AcquireForCompiler(pageIndex)) {
            if (cancelProducers_.load(std::memory_order_acquire)) return;
            const uint32_t observed =
                compilerWakeEpoch_.load(std::memory_order_acquire);
            if (compiledPages_.AcquireForCompiler(pageIndex)) break;
            WaitForAddressChange(compilerWakeEpoch_, observed);
        }
        CompiledEventPage& page = compiledPages_.Page(pageIndex);
        uint32_t compiledCount = 0u;
        uint32_t drained = 0u;
        auto compileOne = [&](const TimestampedMidiEvent& source) {
            ScheduledRenderEvent scheduled{};
            if (CompileTimestampedEvent(source, epoch, qpcFreq, sampleRate,
                                        bufferFrames, scheduled)) {
                page.events[compiledCount++] = scheduled;
            }
        };
        compileOne(timed);
        ++drained;

        while (drained < kCompilerChunkCapacity &&
               midiIngress_.TryPopSequenceOrdered(timed, mergeState)) {
            compileOne(timed);
            ++drained;
        }

        // Ordering is paid once, outside the callback, directly in the final
        // immutable payload page. Publication transfers only its index.
        if (compiledCount != 0u) {
            SortCompiledEventPage(page.events, compiledPages_.SortScratch(),
                                  compiledCount);
            while (!compiledPages_.PublishFromCompiler(pageIndex,
                                                        compiledCount)) {
                if (cancelProducers_.load(std::memory_order_acquire)) {
                    compiledPages_.ReturnUnusedFromCompiler(pageIndex);
                    return;
                }
                const uint32_t observed =
                    compilerWakeEpoch_.load(std::memory_order_acquire);
                WaitForAddressChange(compilerWakeEpoch_, observed);
            }
        } else {
            compiledPages_.ReturnUnusedFromCompiler(pageIndex);
        }
        if (drained != 0u) {
            publishProducerSpace();
        }
        if (cancelProducers_.load(std::memory_order_acquire))
            continue;
    }
}

void Driver::RenderCallback(float* output, uint32_t numFrames, void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self || !self->initialized) return;
    ++self->callbackCount_;

    // The immutable bundle was fully parsed and prepared by a non-audio
    // thread.  Activation is one pointer handoff at the callback boundary;
    // no allocation, file access, lock, or device restart occurs here.
    self->ActivatePendingSoundFontAtBlockBoundary();

    VoiceManager* vm = self->voiceManager;
    ChannelCache* cc = self->channelCache;
    RenderScalar* render = self->renderScalar;
    RuntimeConfigSnapshot* snap = self->configSnapshot;
    const int16_t* sd = self->sampleDataStore;

    if (!vm || !cc || !render || !snap) return;

    // ── Mailbox sync: dirty-gated monotonic seqlock read ───────────
    // The control thread publishes only when the user actually moves a
    // knob; the mailbox is skipped entirely when the sequence matches
    // lastAppliedLiveSeq_ (the common per-block case).  On a torn (odd
    // or mid-flight) read we fall back to appliedMailbox_, the last
    // state this audio thread applied.  The DSP apply (incl.
    // reverb.UpdateDerived and the limiter glide targets) runs only on
    // change, so the per-block overhead of the live path is one atomic
    // load and one compare.
    const uint32_t seq = self->liveMailboxSeq_.load(std::memory_order_acquire);
    if (seq != self->lastAppliedLiveSeq_) {
        NonAtomicLiveConfigMailbox mb;
        uint32_t appliedSeq = seq;
        if ((seq & 1u) == 0u) {
            self->liveMailbox_.StoreToNonAtomic(mb);
            if (self->liveMailboxSeq_.load(std::memory_order_acquire) != seq) {
                mb = self->appliedMailbox_;
                appliedSeq = self->appliedSeq_.load(std::memory_order_acquire);
            }
        } else {
            mb = self->appliedMailbox_;
            appliedSeq = self->appliedSeq_.load(std::memory_order_acquire);
        }

        snap->masterVolume   = mb.masterVolume;
        snap->correctnessMode = mb.correctnessMode;
        snap->enableReverb   = mb.reverbEnabled;
        self->correctnessMode_ = mb.correctnessMode;

        self->reverb.enabled     = mb.reverbEnabled;
        self->reverb.mix         = mb.reverbMix;
        self->reverb.roomSize    = mb.reverbRoomSize;
        self->reverb.decay       = mb.reverbDecay;
        self->reverb.damping     = mb.reverbDamping;
        self->reverb.width       = mb.reverbWidth;
        self->reverb.diffusion   = mb.reverbDiffusion;
        self->reverb.preDelayMs  = mb.reverbPreDelayMs;
        self->reverb.earlyLevel  = mb.reverbEarlyLevel;
        self->reverb.lateLevel   = mb.reverbLateLevel;
        self->reverb.modDepth    = mb.reverbModDepth;
        self->reverb.modRate     = mb.reverbModRate;
        self->reverb.lowCutHz    = mb.reverbLowCutHz;
        self->reverb.highCutHz   = mb.reverbHighCutHz;

        // Derived parameters (FDN feedback, tap gains, lengths, LFO
        // increments) recompute WITHOUT clearing the delay lines, so a
        // live change morphs the tail instead of cutting it dead.
        self->reverb.UpdateDerived();

        self->limiter.enabled           = mb.limiterEnabled;
        self->limiter.algorithmTarget   = mb.limiterAlgorithm;
        self->limiter.thresholdTarget   = mb.limiterThreshold;
        self->limiter.delayFramesTarget = mb.limiterDelayFrames;
        self->limiter.attackCoeff       = mb.limiterAttackCoeff;
        self->limiter.releaseCoeff      = mb.limiterReleaseCoeff;

        // Per-voice phase rotation mode (0 = Coherent bit-exact bypass).
        self->voiceManager->SetPhaseRotationMode(mb.phaseRotationMode);

        self->channelCache->SetMasterVolume(
            mb.masterVolume * self->sysexMasterVolume_);

        // Echo the applied state to the control thread (telemetry audit).
        // The echo must be coherent BEFORE lastAppliedLiveSeq_ advances
        // so no reader can ever observe a newer appliedSeq_ with an
        // older mailbox.
        self->appliedMailbox_ = mb;
        self->appliedSeq_.store(appliedSeq, std::memory_order_release);
        self->lastAppliedLiveSeq_ = appliedSeq;
    }

    LARGE_INTEGER renderStartQPC;
    QueryPerformanceCounter(&renderStartQPC);
    const bool profileCallback = self->diagnosticsEnabled_;
    const uint64_t profileCycleStart = profileCallback ? __rdtsc() : 0u;

    cc->RebuildCache(*snap, static_cast<float>(self->sampleRate));

    // Fold a live master-volume change into ALL playing voices (their
    // mixGainL/R are only refreshed at note-on otherwise).  Per-channel
    // refresh over the active list — done only when the value actually
    // changed, never per block at 500K voices.
    const float effectiveMasterVolume =
        snap->masterVolume * self->sysexMasterVolume_;
    if (self->appliedMasterVolume_ != effectiveMasterVolume) {
        self->appliedMasterVolume_ = effectiveMasterVolume;
        for (uint32_t ch = 0u; ch < kChannelCount; ++ch) {
            vm->RefreshMixGainsForChannel(
                static_cast<uint8_t>(ch), cc->GetParams()[ch]);
        }
    }

    if (!self->leftBuffer || !self->rightBuffer) {
        std::memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }
    float* leftBuf = self->leftBuffer;
    float* rightBuf = self->rightBuffer;
    std::memset(leftBuf, 0, numFrames * sizeof(float));
    std::memset(rightBuf, 0, numFrames * sizeof(float));

    // ── Diagnostic: voice retire stats ──────────────────────────────

    // ── Persistent Pending Queue — Unified Event Pipeline ──────────────
    // The SPSC queue carries TimestampedMidiEvent structs from the MIDI
    // host thread.  We drain ALL of them into a persistent, audio-thread-
    // only `pendingEventBuffer`.  Events are converted to RenderEvent with
    // a fractional sampleOffset computed ONCE against this block's start
    // QPC.  From the pending queue we budget-extract at most
    // kMaxEventsPerBlock events that fall within [0, numFrames) into
    // evtBuf for dispatch.  Remaining events stay in the pending queue
    // and have their offsets decremented by numFrames at block end (smooth
    // rollover).  This guarantees:
    //
    //   a) Zero event loss — events beyond the budget simply queue up and
    //      fire in subsequent blocks (natural time-stretching).
    //   b) No frame-0 clumping — pending offsets are decremented uniformly,
    //      never re-computed from QPC.
    //   c) Bounded per-block work — the DSP thread processes at most
    //      kMaxEventsPerBlock events regardless of input density.
    uint64_t blockStartQPC;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&blockStartQPC));

    // ── Virtual render clock ─────────────────────────────────────────
    // Monotonically advances by (numFrames * qpcFreq / sr) each callback
    // so event offsets are relative to the audio timeline, not the
    // wall-clock callback time.  Without this, every SPSC event pushed
    // during the previous block has a negative deltaQPC and snaps to
    // sample 0 — producing the 100 Hz buffer-grid buzz.
    if (!self->clockInitialized) {
        self->virtualRenderClockQPC = blockStartQPC;
        self->clockInitialized = true;
        self->compilerEpochQPC_.store(blockStartQPC, std::memory_order_release);
        self->compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
        WakeAddressWaiters(self->compilerWakeEpoch_);
    }

    const int64_t wallRenderSample = QpcDeltaToFrames(
        static_cast<int64_t>(blockStartQPC) -
            static_cast<int64_t>(self->virtualRenderClockQPC),
        static_cast<int64_t>(self->qpcFreq), self->sampleRate);
    const int64_t recoveredRenderSample = RecoverRealtimeRenderFrame(
        self->virtualRenderSample_, wallRenderSample, numFrames);
    if (recoveredRenderSample > self->virtualRenderSample_) {
        self->telemetry_.skippedOutputFrames += static_cast<uint64_t>(
            recoveredRenderSample - self->virtualRenderSample_);
        self->virtualRenderSample_ = recoveredRenderSample;
    }

    // ── Drift recovery ──────────────────────────────────────────────
    // If the render takes >100% CPU, the virtual clock falls behind
    // wall time.  Left unchecked this causes permanent desync: event
    // offsets inflate forever, the pending queue never drains, and the
    // audio is delayed until reset.  Fast-forward the clock to catch
    // up and rescale queued event offsets so they land in this block.
    // ── Step 1: Drain ALL SPSC events → append to pending queue ─────────
    uint32_t scannedIngress = 0;
    uint32_t admittedEvents = 0;
    std::memset(self->staleRecoveryValid_, 0,
                sizeof(self->staleRecoveryValid_));
    std::memset(self->staleRecoveryNoteOffValid_, 0,
                sizeof(self->staleRecoveryNoteOffValid_));
    std::memset(self->staleRecoveryNoteOffCount_, 0,
                sizeof(self->staleRecoveryNoteOffCount_));
    const uint32_t importedPages = self->useEventCompiler_
        ? self->pagedScheduler_.ImportAllReady()
        : 0u;
    if (self->useEventCompiler_) {
        self->telemetry_.scheduledHighWater = (std::max)(
            self->telemetry_.scheduledHighWater,
            static_cast<uint64_t>(self->pagedScheduler_.HighWater()));
    }
    const uint32_t ingressScanBudget = self->eventScheduler_.Capacity();
    while (!self->useEventCompiler_ &&
           scannedIngress < ingressScanBudget &&
           admittedEvents < self->maxEventsPerBlock_ &&
           self->eventScheduler_.Size() < self->eventScheduler_.Capacity()) {
        ScheduledRenderEvent scheduled{};
        TimestampedMidiEvent timed{};
        if (!self->midiIngress_.TryPop(timed)) break;
        if (!CompileTimestampedEvent(
                timed, self->virtualRenderClockQPC, self->qpcFreq,
                self->sampleRate, self->bufferFrames, scheduled)) {
            ++scannedIngress;
            continue;
        }
        ++scannedIngress;
        if (self->eventScheduler_.Size() >= self->eventScheduler_.Capacity()) {
            break;
        }

        RenderEvent ev = scheduled.ToRenderEvent();
        const RenderEventType etype = ev.type;
        const uint8_t ch = ev.channel;
        const uint8_t d1 = ev.data1;
        const uint32_t sequence = scheduled.sequence;
#if defined(SVMS_XP_COMPAT)
        // DirectSound's notification cursor and the QPC playback position can
        // differ by several ring segments on XP. A live event behind the next
        // writable frame is late, not obsolete: dispatch it at that frame.
        if (scheduled.targetFrame < self->virtualRenderSample_) {
            ++self->telemetry_.late;
            scheduled.targetFrame = self->virtualRenderSample_;
        }
#endif
        const uint32_t recoveryKey =
            static_cast<uint32_t>(ch) * kNoteCount + d1;
        if (etype == RenderEventType::NoteOff && d1 < kNoteCount &&
            scheduled.targetFrame < self->virtualRenderSample_) {
            if (self->staleRecoveryNoteOffCount_[recoveryKey] <
                kMaxPolyphony) {
                ++self->staleRecoveryNoteOffCount_[recoveryKey];
            }
            if (!self->staleRecoveryNoteOffValid_[recoveryKey] ||
                !SequenceAtOrBefore(
                    sequence,
                    self->staleRecoveryNoteOffSequence_[recoveryKey])) {
                self->staleRecoveryNoteOffSequence_[recoveryKey] = sequence;
                self->staleRecoveryNoteOffFrame_[recoveryKey] =
                    scheduled.targetFrame;
                self->staleRecoveryNoteOffValid_[recoveryKey] = 1u;
            }
            if (self->staleRecoveryValid_[recoveryKey] &&
                SequenceAtOrBefore(
                    self->staleRecoveryEvents_[recoveryKey].ingressSequence,
                    sequence)) {
                self->staleRecoveryValid_[recoveryKey] = 0u;
                ++self->telemetry_.staleNoteOnsSkipped;
            }
            ++self->telemetry_.staleNoteOffsCompacted;
            continue;
        }
        if (etype == RenderEventType::NoteOn &&
            IsObsoleteNoteOn(scheduled.targetFrame, self->virtualRenderSample_,
                             self->bufferFrames)) {
            ++self->telemetry_.late;
            if (d1 >= kNoteCount ||
                (self->staleRecoveryNoteOffValid_[recoveryKey] &&
                 SequenceAtOrBefore(
                     sequence,
                     self->staleRecoveryNoteOffSequence_[recoveryKey]))) {
                ++self->telemetry_.staleNoteOnsSkipped;
                continue;
            }
            if (self->staleRecoveryValid_[recoveryKey]) {
                if (SequenceAtOrBefore(
                        sequence,
                        self->staleRecoveryEvents_[recoveryKey].ingressSequence)) {
                    ++self->telemetry_.staleNoteOnsSkipped;
                    continue;
                }
                ++self->telemetry_.staleNoteOnsSkipped;
            }
            ev.frameOffset = 0u;
            self->staleRecoveryEvents_[recoveryKey] = ev;
            self->staleRecoveryValid_[recoveryKey] = 1u;
            continue;
        }
        if (!self->eventScheduler_.EnqueueBatched(scheduled)) {
            ++self->telemetry_.dropped;
            break;
        }
        ++admittedEvents;
        self->telemetry_.scheduledHighWater =
            (std::max)(self->telemetry_.scheduledHighWater,
                       static_cast<uint64_t>(self->eventScheduler_.Size()));
    }
    // Recovered notes all become writable at this block's first frame. Late
    // note-offs are replayed in batches of at most 255, capped by the current
    // maximum possible voice generations. This retains note-off multiplicity
    // without allowing millions of dead historical messages to consume the
    // callback quota. The scheduler restores frame/sequence order even though
    // the compact set is walked by channel/key here.
    for (uint32_t key = 0;
         !self->useEventCompiler_ &&
         key < Driver::kStaleRecoveryKeys &&
         admittedEvents < self->maxEventsPerBlock_ &&
         self->eventScheduler_.Size() < self->eventScheduler_.Capacity();
         ++key) {
        uint32_t remainingNoteOffs =
            self->staleRecoveryNoteOffValid_[key]
                ? self->staleRecoveryNoteOffCount_[key]
                : 0u;
        while (remainingNoteOffs != 0u &&
               admittedEvents < self->maxEventsPerBlock_ &&
               self->eventScheduler_.Size() <
                   self->eventScheduler_.Capacity()) {
            const uint32_t batch = (std::min)(remainingNoteOffs, 255u);
            ScheduledRenderEvent recoveredOff{};
            recoveredOff.targetFrame =
                self->staleRecoveryNoteOffFrame_[key];
            recoveredOff.sequence =
                self->staleRecoveryNoteOffSequence_[key];
            recoveredOff.type = RenderEventType::StaleNoteOffBatch;
            recoveredOff.channel = static_cast<uint8_t>(key / kNoteCount);
            recoveredOff.data1 = static_cast<uint8_t>(key % kNoteCount);
            recoveredOff.data2 = static_cast<uint8_t>(batch);
            if (!self->eventScheduler_.EnqueueBatched(recoveredOff)) break;
            remainingNoteOffs -= batch;
            ++admittedEvents;
        }
        if (!self->staleRecoveryValid_[key]) continue;
        ScheduledRenderEvent recovered;
        recovered.SetRenderEvent(self->staleRecoveryEvents_[key]);
        recovered.targetFrame = self->virtualRenderSample_;
        recovered.sequence = self->staleRecoveryEvents_[key].ingressSequence;
        if (!self->eventScheduler_.EnqueueBatched(recovered)) {
            ++self->telemetry_.staleNoteOnsSkipped;
            break;
        }
        ++admittedEvents;
    }

    if (!self->useEventCompiler_) self->eventScheduler_.FinalizeBatch();

    self->producerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(self->producerWakeEpoch_);
    const uint32_t scheduledBeforeDispatch = self->useEventCompiler_
        ? self->pagedScheduler_.Size()
        : self->eventScheduler_.Size();
    self->scheduledSizePublished_.store(scheduledBeforeDispatch,
                                        std::memory_order_release);

    // Extract only this render window.  Future events remain in the heap.
    RenderEvent* evtBuf = self->eventBuffer;
    uint32_t evCount = 0;
    uint32_t examinedCount = 0;
    const uint32_t eventBudget =
        self->eventBufferCapacity_;
    auto admitScheduled = [&](const ScheduledRenderEvent& scheduledOut) {
        ++examinedCount;
        if (self->overflowMode_.load(std::memory_order_relaxed) ==
                EventOverflowMode::PriorityVelocity &&
            scheduledOut.type == RenderEventType::NoteOn &&
            IsObsoleteNoteOn(scheduledOut.targetFrame,
                             self->virtualRenderSample_, self->bufferFrames)) {
            ++self->telemetry_.late;
            ++self->telemetry_.staleNoteOnsSkipped;
            return;
        }
        int64_t offset = scheduledOut.targetFrame - self->virtualRenderSample_;
        if (offset < 0) { ++self->telemetry_.late; offset = 0; }
        evtBuf[evCount++] = scheduledOut.ToRenderEvent(
            static_cast<uint32_t>(offset));
        ++self->telemetry_.dispatched;
    };
    if (self->useEventCompiler_) {
        for (;;) {
            const ScheduledRenderEvent* run = nullptr;
            const uint32_t runCount = self->pagedScheduler_.PeekRunBefore(
                self->virtualRenderSample_ + numFrames,
                eventBudget - examinedCount, run);
            if (runCount == 0u) break;
            for (uint32_t i = 0u; i < runCount; ++i)
                admitScheduled(run[i]);
            self->pagedScheduler_.ConsumeRun(runCount);
            if (examinedCount == eventBudget) break;
        }
    } else {
        ScheduledRenderEvent scheduledOut{};
        while (examinedCount < eventBudget &&
               self->eventScheduler_.PopBefore(
                   self->virtualRenderSample_ + numFrames, scheduledOut)) {
            admitScheduled(scheduledOut);
        }
    }
    const uint32_t scheduledAfterDispatch = self->useEventCompiler_
        ? self->pagedScheduler_.Size()
        : self->eventScheduler_.Size();
    self->scheduledSizePublished_.store(scheduledAfterDispatch,
                                        std::memory_order_release);
    if (importedPages != 0u || examinedCount != 0u) {
        self->compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
        WakeAddressWaiters(self->compilerWakeEpoch_);
    }

    // ── Step 2: Sort pending queue by sampleOffset ─────────────────────
    // Insertion sort.  The carry-forward portion (from previous blocks) is
    // already sorted (offset decrement preserves relative order).  Newly
    // appended SPSC events at the tail are the only unsorted elements.
    // For mostly-sorted data this is O(N) with a small constant.

    // ── Step 3: Extract all in-block events for this block ──────────────
    // Walk the sorted pending queue from offset 0.  Take every event with
    // sampleOffset < numFrames into evtBuf.  Stop at the first future
    // event (offset >= numFrames).  With in-place voice recycling keeping
    // active polyphony bounded to ~128-256 voices, the engine can process
    // all incoming events in real-time without batch-chunking artifacts.
    // ── Step 4: Decrement remaining pending offsets by numFrames ────────
    // Smooth rollover: future events advance at the true audio clock rate
    // toward their firing time without frame-boundary re-quantization.
    // ── Step 5: Sort evtBuf by sampleOffset (defensive) ─────────────────

    // ── Step 6: Render with sub-sample event slicing ───────────────────
    // RenderBlock will invoke DispatchRenderEvent at each event's exact
    // fractional sample offset.  All event handling (voice allocation,
    // release, CC updates) happens inside the render loop via the callback.
    const uint64_t profileScheduleEnd = profileCallback ? __rdtsc() : 0u;
    self->dispatchCyclesCurrent_ = 0u;
    // The diagnostic UI publishes once per callback. Capture one successful
    // voice launch for its detailed SF2 probe instead of rewriting ~20 fields
    // for every dense note-on; lifetime counters remain exact below.
    self->captureSf2Detail_ = self->diagnosticsEnabled_;
    render->RenderBlock(*vm, *cc, sd, self->sampleDataFrames,
                        leftBuf, rightBuf, numFrames, *snap,
                        evtBuf, evCount, self->correctnessMode_,
                        static_cast<uint64_t>(self->virtualRenderSample_));
    const uint64_t profileRenderEnd = profileCallback ? __rdtsc() : 0u;

    // ── Advance virtual render clock for the next callback ──────────
    self->virtualRenderSample_ += static_cast<int64_t>(numFrames);
    self->outputFramePublished_.store(
        static_cast<uint64_t>(self->virtualRenderSample_),
        std::memory_order_release);

    // masterVolume is already included in ChannelCache's per-channel mix
    // gains. Applying it here again would attenuate the output twice.
    float renderPeak = 0.0f;
    const bool collectRenderPeak = self->diagnosticsEnabled_;
    for (uint32_t i = 0; i < numFrames; ++i) {
        const float left = leftBuf[i];
        const float right = rightBuf[i];
        output[i * 2u] = left;
        output[i * 2u + 1u] = right;
        if (collectRenderPeak) {
            renderPeak = (std::max)(renderPeak, std::fabs(left));
            renderPeak = (std::max)(renderPeak, std::fabs(right));
        }
    }
    if (collectRenderPeak) {
        self->sf2Telemetry_.renderPeak = renderPeak;
        if (self->sf2Telemetry_.lastVoiceHandle < vm->GetMaxVoices()) {
            const uint32_t h = self->sf2Telemetry_.lastVoiceHandle;
            self->sf2Telemetry_.lastPhase = vm->v.phases[h];
        }
    }
    // Per-voice phase rotation is applied inside RenderBlock (per-voice, at
    // each mix site), so Coherent mode (no state allocated) stays bit-exact
    // and non-Coherent modes never touch loudness or gain-reduction inputs.
    // Filter the final limited samples in the same loop so the 3 Hz cutoff
    // neither changes gain detection nor requires another memory pass.
    self->reverb.Process(output, numFrames, 2);
    self->limiter.Process(output, numFrames, 2, self->postHighPass);
#if !defined(SVMS_XP_COMPAT)
    self->liveRecorder_.Capture(output, numFrames);
#endif

    const uint64_t profilePostEnd = profileCallback ? __rdtsc() : 0u;

    LARGE_INTEGER renderEndQPC;
    QueryPerformanceCounter(&renderEndQPC);

    double elapsedUs = (double)(renderEndQPC.QuadPart - renderStartQPC.QuadPart)
                     / (double)self->qpcFreq * 1e6;
    double budgetUs = (double)numFrames / (double)self->sampleRate * 1e6;
    float cpuPct = (budgetUs > 0.0) ? (float)(elapsedUs / budgetUs * 100.0) : 0.0f;
    float schedulerPercent = 0.0f;
    float dispatchPercent = 0.0f;
    float synthesisPercent = 0.0f;
    float postPercent = 0.0f;
    if (profileCallback && profilePostEnd > profileCycleStart) {
        const uint64_t totalCycles = profilePostEnd - profileCycleStart;
        const uint64_t schedulerCycles = profileScheduleEnd - profileCycleStart;
        const uint64_t renderCycles = profileRenderEnd - profileScheduleEnd;
        const uint64_t dispatchCycles = (std::min)(
            self->dispatchCyclesCurrent_, renderCycles);
        const float scale = cpuPct / static_cast<float>(totalCycles);
        schedulerPercent = static_cast<float>(schedulerCycles) * scale;
        dispatchPercent = static_cast<float>(dispatchCycles) * scale;
        synthesisPercent = static_cast<float>(renderCycles - dispatchCycles) * scale;
        postPercent = static_cast<float>(profilePostEnd - profileRenderEnd) * scale;
    }
    self->callbackTiming_.Observe(cpuPct);
    self->telemetry_.maxCallbackQPC = (std::max)(self->telemetry_.maxCallbackQPC,
        static_cast<uint64_t>(renderEndQPC.QuadPart - renderStartQPC.QuadPart));
    self->telemetry_.callbackP95Percent = self->callbackTiming_.Percentile(95u, 100u);
    self->telemetry_.callbackP99Percent = self->callbackTiming_.Percentile(99u, 100u);
    self->telemetry_.callbackP999Percent = self->callbackTiming_.Percentile(999u, 1000u);
    self->telemetry_.overBudgetCallbacks = self->callbackTiming_.overBudgetCallbacks;
    self->telemetry_.maxConsecutiveOverBudget =
        self->callbackTiming_.maxConsecutiveOverBudget;
    self->telemetry_.immediateRetirements = vm->retireImmediateCount_;
    self->telemetry_.voiceSteals = vm->stealCount_;
    self->telemetry_.submitted = self->submittedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.accepted = self->acceptedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.dropped = self->shedAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.shedNoteOns = self->telemetry_.dropped;
    self->telemetry_.cancelledSubmissions =
        self->cancelledAtomic_.load(std::memory_order_relaxed);
    self->telemetry_.currentVelocityCutoff =
        self->currentVelocityCutoffAtomic_.load(std::memory_order_relaxed);

    const uint32_t nextDebugIndex =
        (self->debugSnapshotIndex_.load(std::memory_order_relaxed) + 1u) & 1u;
    DriverDebugInfo& debug = self->debugSnapshots_[nextDebugIndex];
    debug = DriverDebugInfo{};
    debug.callbackCount = self->callbackCount_;
    debug.submitted = self->telemetry_.submitted;
    debug.accepted = self->telemetry_.accepted;
    debug.dispatched = self->telemetry_.dispatched;
    debug.noteOns = self->sf2Telemetry_.noteOns;
    debug.matchedRegions = self->sf2Telemetry_.exactRegionMatches;
    debug.configuredVoices = self->sf2Telemetry_.configuredVoices;
    debug.activeVoices = vm->activeCount_;
    debug.sampleDataFrames = self->sampleDataFrames;
    debug.sampleCount = self->sampleStoreCount;
    debug.soundFontLoaded = self->soundFontData && self->sampleDataStore ? 1u : 0u;
    debug.audioRunning = self->audioOutput && self->audioOutput->IsRunning() ? 1u : 0u;
    debug.audioHResult = self->audioOutput
        ? static_cast<int32_t>(self->audioOutput->GetLastError()) : 0;
    debug.renderPeak = self->sf2Telemetry_.renderPeak;

    SnappyVoiceStatistics& voiceStats =
        self->voiceStatisticsSnapshots_[nextDebugIndex];
    voiceStats.activeVoices = vm->activeCount_;
    voiceStats.freeVoices = vm->GetMaxVoices() - vm->activeCount_;
    voiceStats.voiceSteals = vm->stealCount_;

    LegacyDriverDebugInfo& legacy =
        self->legacyDebugSnapshots_[nextDebugIndex];
    legacy = LegacyDriverDebugInfo{};
    legacy.renderingTime = static_cast<float>(elapsedUs / 1000.0);
    for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
        legacy.activeVoices[channel] = vm->GetChannelActiveCount(channel);
    }
    legacy.audioLatency = budgetUs / 1000.0;
    legacy.audioBufferSize = numFrames;
    self->renderingTimeSnapshots_[nextDebugIndex] = legacy.renderingTime;
    self->debugSnapshotIndex_.store(nextDebugIndex, std::memory_order_release);
    if (self->diagnosticsEnabled_) {
        for (uint32_t velocity = 0; velocity < 128; ++velocity) {
            self->telemetry_.shedByVelocity[velocity] =
                self->shedByVelocityAtomic_[velocity].load(std::memory_order_relaxed);
        }
    }

    // Smooth CPU reading with a simple low-pass
    static float s_cpuSmoothed = 0.0f;
    s_cpuSmoothed += 0.1f * (cpuPct - s_cpuSmoothed);
    static float s_schedulerSmoothed = 0.0f;
    static float s_dispatchSmoothed = 0.0f;
    static float s_synthesisSmoothed = 0.0f;
    static float s_postSmoothed = 0.0f;
    s_schedulerSmoothed += 0.1f * (schedulerPercent - s_schedulerSmoothed);
    s_dispatchSmoothed += 0.1f * (dispatchPercent - s_dispatchSmoothed);
    s_synthesisSmoothed += 0.1f * (synthesisPercent - s_synthesisSmoothed);
    s_postSmoothed += 0.1f * (postPercent - s_postSmoothed);

    if (self->diagnosticsEnabled_) {
        static int diagTick = 0;
        if (++diagTick >= 1) {
            diagTick = 0;
            if (self->diagnosticsWindow_ || self->diagnosticsDebugOutput_) {
                // Extreme-polyphony rule: never scan activeList for
                // diagnostics; the releasing count comes from the exact
                // transition counter (VoiceManager::releasingCount_), so
                // only the sustain-held tally still needs a walk.
                const uint32_t releasingVoices = vm->GetReleasingCount();
                uint32_t sustainHeldVoices = 0;
                for (uint32_t position = 0; position < vm->activeCount_; ++position) {
                    const uint32_t voice = vm->activeList_[position];
                    sustainHeldVoices += vm->v.heldBySustain[voice] != 0;
                }
                DiagWindow_Update(vm->activeCount_, vm->GetMaxVoices(),
                                  releasingVoices, sustainHeldVoices,
                                  vm->stealCount_,
                                  self->coalescedAtomic_.load(
                                      std::memory_order_relaxed),
                                  self->noteOnCollapse_.Threshold(),
                                  s_cpuSmoothed, self->correctnessMode_ ? 1u
                                      : ComputeDecimationStep(vm->activeCount_),
                                  self->telemetry_.callbackP95Percent,
                                  self->telemetry_.callbackP99Percent,
                                  self->telemetry_.callbackP999Percent,
                                  self->telemetry_.overBudgetCallbacks,
                                  self->telemetry_.maxConsecutiveOverBudget,
                                  vm->retireCount_, vm->retireImmediateCount_,
                                  self->audioOutput && self->audioOutput->IsRunning(),
                                  self->audioOutput
                                      ? static_cast<int32_t>(self->audioOutput->GetLastError())
                                      : 0,
                                  self->soundFontData && self->sampleDataStore,
                                   self->sampleRate, self->bufferFrames,
                                   snap->masterVolume,
                                  UsesXPWaveOut(self->audioOutput),
                                  render->GetRenderBackend(),
                                  render->GetRenderThreadCount(),
                                  render->GetMulticoreEffectiveness(),
                                  s_schedulerSmoothed, s_dispatchSmoothed,
                                  s_synthesisSmoothed, s_postSmoothed,
                                  evCount, scheduledAfterDispatch,
                                  self->sf2Telemetry_);
            }
        }
    }

    // ── Audio→control snapshot (RuntimeLink V2) ────────────────────
    // The control thread publishes at ~30 Hz from this process-local
    // snapshot; the audio thread never touches shared memory.  The
    // releasing-voice count comes from the exact transition counter
    // (VoiceManager::releasingCount_), so no O(activeN) scan is needed;
    // the only remaining walk is the diag window's sustain tally.
    // Writes are relaxed atomics wrapped in a monotonic odd/even
    // sequence (2, 4, 6, ...): odd = writer inside, even = settled.
#if !defined(SVMS_XP_COMPAT)
    {
        svms::RuntimeAudioSnapshot& as = g_audioSnapshot;
        const uint32_t odd = as.sequence.load(std::memory_order_relaxed) | 1u;
        as.sequence.store(odd, std::memory_order_relaxed);
        RLV2_MemBarrier();
        as.tickMs.store(static_cast<uint32_t>(GetTickCount()),
                        std::memory_order_relaxed);
        as.activeVoices.store(vm->activeCount_, std::memory_order_relaxed);
        as.releasingVoices.store(vm->GetReleasingCount(), std::memory_order_relaxed);
        as.freeTop.store(vm->freeTop_, std::memory_order_relaxed);
        as.voiceSteals.store(vm->stealCount_, std::memory_order_relaxed);
        as.retiredCount.store(vm->retireCount_, std::memory_order_relaxed);
        as.retiredImmediateCount.store(vm->retireImmediateCount_, std::memory_order_relaxed);
        as.decimationStep.store(self->correctnessMode_ ? 1u
            : svms::ComputeDecimationStep(vm->activeCount_),
            std::memory_order_relaxed);
        as.renderPeakBits.store(FloatToU32Bits(self->sf2Telemetry_.renderPeak),
                                std::memory_order_relaxed);
        as.audioRunning.store(self->audioOutput && self->audioOutput->IsRunning()
            ? 1u : 0u, std::memory_order_relaxed);
        as.soundFontLoaded.store(self->soundFontData && self->sampleDataStore
            ? 1u : 0u, std::memory_order_relaxed);
        as.audioHResult.store(self->audioOutput
            ? static_cast<int32_t>(self->audioOutput->GetLastError()) : 0,
            std::memory_order_relaxed);
        as.cpuLoadPercentBits.store(FloatToU32Bits(s_cpuSmoothed),
                                    std::memory_order_relaxed);
        as.callbackP95PercentBits.store(
            FloatToU32Bits(self->telemetry_.callbackP95Percent),
            std::memory_order_relaxed);
        as.callbackP99PercentBits.store(
            FloatToU32Bits(self->telemetry_.callbackP99Percent),
            std::memory_order_relaxed);
        as.callbackP999PercentBits.store(
            FloatToU32Bits(self->telemetry_.callbackP999Percent),
            std::memory_order_relaxed);
        as.maxConsecutiveOverBudget.store(
            self->telemetry_.maxConsecutiveOverBudget,
            std::memory_order_relaxed);
        as.overBudgetCallbacks.store(self->telemetry_.overBudgetCallbacks,
                                     std::memory_order_relaxed);
        as.eventsSubmitted.store(self->telemetry_.submitted,
                                 std::memory_order_relaxed);
        as.eventsAccepted.store(self->telemetry_.accepted,
                                std::memory_order_relaxed);
        as.eventsDropped.store(self->telemetry_.dropped,
                               std::memory_order_relaxed);
        as.eventsDispatched.store(self->telemetry_.dispatched,
                                  std::memory_order_relaxed);
        as.limiterInputPeakLBits.store(FloatToU32Bits(self->limiter.inputPeakL),
                                       std::memory_order_relaxed);
        as.limiterInputPeakRBits.store(FloatToU32Bits(self->limiter.inputPeakR),
                                       std::memory_order_relaxed);
        as.limiterOutputPeakLBits.store(FloatToU32Bits(self->limiter.outputPeakL),
                                        std::memory_order_relaxed);
        as.limiterOutputPeakRBits.store(FloatToU32Bits(self->limiter.outputPeakR),
                                        std::memory_order_relaxed);
        as.limiterGainReductionDbBits.store(
            FloatToU32Bits(self->limiter.gainReductionDb),
            std::memory_order_relaxed);
        as.schedulerPercentBits.store(FloatToU32Bits(s_schedulerSmoothed),
                                      std::memory_order_relaxed);
        as.eventDispatchPercentBits.store(FloatToU32Bits(s_dispatchSmoothed),
                                          std::memory_order_relaxed);
        as.rawIngressCount.store(self->midiIngress_.TotalSize(),
                                 std::memory_order_relaxed);
        as.compiledPagedCount.store(self->compiledPages_.ReadyEventCount(),
                                    std::memory_order_relaxed);
        as.scheduledBacklogCount.store(scheduledAfterDispatch,
                                       std::memory_order_relaxed);
        RLV2_MemBarrier();
        as.sequence.store(odd + 1u, std::memory_order_release);
    }
#endif
}

// ── EventDispatcher callback ─────────────────────────────────────────────
// Called by RenderScalar::RenderBlock at each event's exact sub-sample
// position.  Voice allocation, release, CC updates all happen here so
// that the sub-sample phase offset and releaseStartInBlock are set at the precise frame.
void Driver::DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                  void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self) return;

    switch (event.type) {
        case RenderEventType::NoteOn: {
            const uint64_t globalFence =
                self->globalTerminationFence_.load(std::memory_order_acquire);
            const uint64_t channelFence = event.channel < kChannelCount
                ? self->channelTerminationFence_[event.channel].load(std::memory_order_acquire)
                : 0u;
            if (FenceSuppresses(event.ingressSequence, globalFence) ||
                FenceSuppresses(event.ingressSequence, channelFence)) {
                break;
            }
            self->HandleNoteOn(event.channel, event.data1, event.data2);
            break;
        }
        case RenderEventType::NoteOff:
            self->HandleNoteOff(event.channel, event.data1, blockCursor);
            break;
        case RenderEventType::StaleNoteOffBatch:
            self->HandleStaleNoteOffBatch(event.channel, event.data1,
                                          event.data2, blockCursor);
            break;
        case RenderEventType::ControlChange:
            self->HandleControlChange(event.channel, event.data1, event.data2,
                                      blockCursor);
            break;
        case RenderEventType::ProgramChange:
            self->HandleProgramChange(event.channel, event.data1);
            break;
        case RenderEventType::PitchBend:
            self->HandlePitchBend(event.channel, event.data1, event.data2);
            break;
        case RenderEventType::ChannelPressure:
            self->HandleChannelPressure(event.channel, event.data1);
            break;
        case RenderEventType::AllNotesOff:
        case RenderEventType::AllSoundOff:
            // [HOOK] Overload ladder: these can trigger hard/panic release.
            break;
        case RenderEventType::Reset:
            if (self->voiceManager) self->voiceManager->Reset();
            if (self->channelCache) self->channelCache->Reset();
            self->sysexMasterVolume_ = 1.0f;
            self->sysexMasterFineTune_ = 0.0f;
            self->sysexMasterTranspose_ = 0.0f;
            if (self->channelCache && self->configSnapshot) {
                self->channelCache->SetMasterVolume(
                    self->configSnapshot->masterVolume);
                self->channelCache->RebuildCache(
                    *self->configSnapshot,
                    static_cast<float>(self->sampleRate));
            }
            self->RefreshSelectedPresets();
            std::fill(std::begin(self->channelPitchBendRatio_),
                      std::end(self->channelPitchBendRatio_), 1.0f);
            for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
                self->channelCache->SetBendRatio(
                    static_cast<uint8_t>(channel), 1.0f);
                ++self->channelLaunchRevision_[channel];
            }
            self->nextPlayIndex_ = 1;
            self->postHighPass.Reset();
            self->reverb.Reset();
            self->limiter.Reset();
            break;
        case RenderEventType::MasterVolume: {
            const uint16_t value = static_cast<uint16_t>(
                event.data1 | (static_cast<uint16_t>(event.data2) << 7u));
            self->sysexMasterVolume_ =
                static_cast<float>(value) / 16383.0f;
            if (self->channelCache && self->configSnapshot) {
                const float effective = self->configSnapshot->masterVolume *
                    self->sysexMasterVolume_;
                self->channelCache->SetMasterVolume(effective);
                self->channelCache->RebuildCache(
                    *self->configSnapshot,
                    static_cast<float>(self->sampleRate));
                if (self->voiceManager) {
                    for (uint8_t channel = 0u;
                         channel < kChannelCount; ++channel) {
                        self->voiceManager->RefreshMixGainsForChannel(
                            channel,
                            self->channelCache->GetParams()[channel]);
                        self->channelCache->SetBendRatio(
                            channel, self->channelPitchBendRatio_[channel]);
                    }
                }
                self->appliedMasterVolume_ = effective;
            }
            break;
        }
        case RenderEventType::MasterFineTune: {
            const uint16_t value = static_cast<uint16_t>(
                event.data1 | (static_cast<uint16_t>(event.data2) << 7u));
            self->sysexMasterFineTune_ =
                static_cast<float>(static_cast<int32_t>(value) - 8192) /
                8192.0f;
            self->RefreshAllPitchIncrements();
            break;
        }
        case RenderEventType::MasterTranspose:
            self->sysexMasterTranspose_ = static_cast<float>(
                static_cast<int32_t>(event.data1) - 64);
            self->RefreshAllPitchIncrements();
            break;
        case RenderEventType::RhythmPart:
            if (self->channelCache && event.channel < kChannelCount) {
                self->channelCache->SetRhythmPart(event.channel, event.data1);
                uint32_t presetIndex = 0u;
                uint8_t soundFontIndex = 0u;
                if (ResolveChannelPreset(self->activeSoundFontStack_,
                        *self->channelCache, event.channel,
                        &soundFontIndex, &presetIndex)) {
                    self->channelSoundFontIndex_[event.channel] =
                        soundFontIndex;
                    self->channelCache->SetSelectedPreset(
                        event.channel, static_cast<uint16_t>(presetIndex));
                } else {
                    self->channelCache->SetSelectedPreset(
                        event.channel, UINT16_MAX);
                }
                ++self->channelLaunchRevision_[event.channel];
            }
            break;
    }
}

void Driver::DispatchRenderEventBatch(const RenderEvent* events,
                                      uint32_t eventCount,
                                      uint32_t blockCursor, void* userData) {
    Driver* self = static_cast<Driver*>(userData);
    if (!self || !events) return;
    const uint64_t profileBegin = self->diagnosticsEnabled_ ? __rdtsc() : 0u;

    // The renderer has already grouped this range by exact output frame and
    // ingress sequence. Process maximal note-on runs directly; any state or
    // termination event breaks the run and goes through the full dispatcher.
    uint32_t index = 0u;
    uint64_t deferredNoteOns = 0u;
    uint64_t deferredMatches = 0u;
    uint64_t deferredConfigured = 0u;
    const bool allowStateCoalescing =
        self->overflowMode_.load(std::memory_order_relaxed) ==
        EventOverflowMode::PriorityVelocity;
    const auto isCoalescibleStateWrite = [](const RenderEvent& event) {
        switch (event.type) {
            case RenderEventType::ControlChange:
                // These controllers only replace channel state. Sustain,
                // reset and termination controllers have lifecycle side
                // effects and must always remain literal events.
                return event.data1 == 0u || event.data1 == 7u ||
                       event.data1 == 10u || event.data1 == 11u ||
                       event.data1 == 32u;
            case RenderEventType::ProgramChange:
            case RenderEventType::PitchBend:
            case RenderEventType::MasterVolume:
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
            case RenderEventType::RhythmPart:
                return true;
            default:
                return false;
        }
    };
    const auto hasSameStateTarget = [](const RenderEvent& left,
                                       const RenderEvent& right) {
        if (left.type != right.type) return false;
        switch (left.type) {
            case RenderEventType::ControlChange:
                return left.channel == right.channel &&
                       left.data1 == right.data1;
            case RenderEventType::ProgramChange:
            case RenderEventType::PitchBend:
            case RenderEventType::RhythmPart:
                return left.channel == right.channel;
            case RenderEventType::MasterVolume:
            case RenderEventType::MasterFineTune:
            case RenderEventType::MasterTranspose:
                return true;
            default:
                return false;
        }
    };
    while (index < eventCount) {
        if (events[index].type == RenderEventType::NoteOff) {
            // Strict lossless mode retains literal ingress ordering. It may
            // still combine an adjacent identical channel/key run into one
            // counted operation because no event can observe an intermediate
            // state, but it never reorders interleaved keys.
            if (self->overflowMode_.load(std::memory_order_relaxed) !=
                EventOverflowMode::PriorityVelocity) {
                const uint8_t channel = events[index].channel;
                const uint8_t note = events[index].data1;
                uint32_t runEnd = index + 1u;
                while (runEnd < eventCount &&
                       events[runEnd].type == RenderEventType::NoteOff &&
                       events[runEnd].channel == channel &&
                       events[runEnd].data1 == note) {
                    ++runEnd;
                }
                uint32_t remaining = runEnd - index;
                while (remaining != 0u) {
                    const uint8_t batch = static_cast<uint8_t>(
                        (std::min)(remaining, 255u));
                    self->HandleStaleNoteOffBatch(channel, note, batch,
                                                  blockCursor);
                    remaining -= batch;
                }
                index = runEnd;
                continue;
            }
            // All events in this callback invocation share an exact output
            // frame. Aggregate a maximal note-off-only run by channel/key;
            // controllers, note-ons and termination events remain hard
            // boundaries. Releasing A,B,A is observably identical to A,A,B
            // before the next boundary, while avoiding three oldest-
            // generation traversals when one counted operation is enough.
            uint32_t generation = ++self->noteOffBatchGeneration_;
            if (generation == 0u) {
                std::memset(self->noteOffBatchStamp_, 0,
                            sizeof(self->noteOffBatchStamp_));
                generation = ++self->noteOffBatchGeneration_;
            }
            uint32_t keyCount = 0u;
            uint32_t runEnd = index;
            const uint32_t multiplicityLimit = self->voiceManager
                ? self->voiceManager->GetMaxVoices() : kMaxPolyphony;
            while (runEnd < eventCount &&
                   events[runEnd].type == RenderEventType::NoteOff) {
                const RenderEvent& event = events[runEnd++];
                if (event.channel >= kChannelCount || event.data1 >= kNoteCount)
                    continue;
                const uint32_t key =
                    static_cast<uint32_t>(event.channel) * kNoteCount +
                    event.data1;
                if (self->noteOffBatchStamp_[key] != generation) {
                    self->noteOffBatchStamp_[key] = generation;
                    self->noteOffBatchCount_[key] = 0u;
                    self->noteOffBatchKeys_[keyCount++] =
                        static_cast<uint16_t>(key);
                }
                if (self->noteOffBatchCount_[key] < multiplicityLimit)
                    ++self->noteOffBatchCount_[key];
            }
            for (uint32_t keyIndex = 0u; keyIndex < keyCount; ++keyIndex) {
                const uint32_t key = self->noteOffBatchKeys_[keyIndex];
                uint32_t remaining = self->noteOffBatchCount_[key];
                const uint8_t channel = static_cast<uint8_t>(key / kNoteCount);
                const uint8_t note = static_cast<uint8_t>(key % kNoteCount);
                while (remaining != 0u) {
                    const uint8_t batch = static_cast<uint8_t>(
                        (std::min)(remaining, 255u));
                    self->HandleStaleNoteOffBatch(channel, note, batch,
                                                  blockCursor);
                    remaining -= batch;
                }
            }
            index = runEnd;
            continue;
        }
        if (events[index].type != RenderEventType::NoteOn) {
            // This callback contains one exact output frame in established
            // ingress order. Adjacent writes to the same stateless target
            // have no observable intermediate sample, so priority mode can
            // apply only the final value. Never cross another event, and keep
            // strict-lossless mode completely literal.
            if (allowStateCoalescing &&
                isCoalescibleStateWrite(events[index])) {
                uint32_t runEnd = index + 1u;
                while (runEnd < eventCount &&
                       hasSameStateTarget(events[index], events[runEnd])) {
                    ++runEnd;
                }
                DispatchRenderEvent(events[runEnd - 1u], blockCursor, self);
                index = runEnd;
                continue;
            }
            DispatchRenderEvent(events[index++], blockCursor, self);
            continue;
        }
        const uint64_t globalFence =
            self->globalTerminationFence_.load(std::memory_order_acquire);
        while (index < eventCount &&
               events[index].type == RenderEventType::NoteOn) {
            const uint8_t runChannel = events[index].channel;
            const uint8_t runNote = events[index].data1;
            const uint8_t runVelocity = events[index].data2;
            uint32_t runEnd = index + 1u;
            while (runEnd < eventCount &&
                   events[runEnd].type == RenderEventType::NoteOn &&
                   events[runEnd].channel == runChannel &&
                   events[runEnd].data1 == runNote &&
                   events[runEnd].data2 == runVelocity) {
                ++runEnd;
            }

            // A repeated chopped note on one exact frame has immutable SF2,
            // preset, pitch and channel state. Resolve its prepared launch
            // plan once, then reuse it for the rest of this run. No event is
            // moved and a state event above remains a hard batch boundary.
            const NoteLaunchPlanCacheEntry* exactFramePlan = nullptr;

            // ── Batched steal-candidate selection (hot-toggleable) ─────────
            // correctnessMode_ is the established optimized-vs-exact toggle
            // (plain audio-thread bool, read exactly like the render callback
            // and telemetry paths do). When it is OFF, each note launch in
            // this run is allowed to batch its steal-victim selection through
            // VoiceManager::PopStealCandidates inside LaunchVoiceGroup —
            // batched per launch transaction, where the victim order is
            // provably identical to the sequential per-layer pops (no
            // candidate insertions happen inside one launch; commits land
            // only after the allocation loop). See the FLAG note in
            // LaunchVoiceGroup for why the batching is deliberately scoped
            // per launch instead of per run. With correctness mode ON the
            // flag below is cleared and every selection takes the unchanged
            // per-layer PopStealCandidate path — zero behavioral or
            // performance difference.
            if (self->voiceManager)
                self->voiceManager->SetStealBatchingEnabled(
                    !self->correctnessMode_);
            for (; index < runEnd; ++index) {
                const RenderEvent& event = events[index];
                const uint64_t channelFence = event.channel < kChannelCount
                    ? self->channelTerminationFence_[event.channel].load(
                        std::memory_order_acquire)
                    : 0u;
                if (FenceSuppresses(event.ingressSequence, globalFence) ||
                    FenceSuppresses(event.ingressSequence, channelFence) ||
                    event.channel >= kChannelCount || event.data1 >= kNoteCount) {
                    continue;
                }
                ++deferredNoteOns;
                const uint64_t delta = self->HandleNoteOn(
                    event.channel, event.data1, event.data2, true,
                    exactFramePlan);
                deferredMatches += static_cast<uint32_t>(delta);
                deferredConfigured += static_cast<uint32_t>(delta >> 32u);

                if (!exactFramePlan && self->activeSoundFontStack_ &&
                    self->channelCache) {
                    const NoteLaunchPlanCacheEntry* candidate =
                        self->noteLaunchHotCache_[runChannel][runNote];
                    const uint32_t preset =
                        self->channelCache->GetSelectedPreset(runChannel);
                    const uint8_t soundFontIndex =
                        self->channelSoundFontIndex_[runChannel];
                    if (candidate && candidate->soundFontGeneration ==
                            self->soundFontGeneration_ &&
                        candidate->channelRevision ==
                            self->channelLaunchRevision_[runChannel] &&
                        candidate->presetIndex == preset &&
                        candidate->soundFontIndex == soundFontIndex &&
                        candidate->channel == runChannel &&
                        candidate->note == runNote &&
                        candidate->velocity == (runVelocity & 0x7fu) &&
                        candidate->count != 0u &&
                        candidate->count <= kNoteRegionCacheLayers) {
                        exactFramePlan = candidate;
                    }
                }
            }
        }
    }
    self->sf2Telemetry_.noteOns += deferredNoteOns;
    self->sf2Telemetry_.exactRegionMatches += deferredMatches;
    self->sf2Telemetry_.configuredVoices += deferredConfigured;
    if (self->diagnosticsEnabled_)
        self->dispatchCyclesCurrent_ += __rdtsc() - profileBegin;
}

uint32_t Driver::ResolveNoteRegions(const SoundFontBundle* bank,
                                    uint8_t soundFontIndex,
                                    uint32_t presetIndex, uint8_t note,
                                    uint8_t velocity,
                                    const SFSampleRegion** outRegions,
                                    uint32_t outCapacity) {
    const SF2Data* data = bank ? bank->data : nullptr;
    if (!data || !outRegions || presetIndex >= data->presetCount)
        return 0u;

    const uint32_t tag = (static_cast<uint32_t>(soundFontIndex) << 23u) |
        (presetIndex << 14u) |
        (static_cast<uint32_t>(note) << 7u) | velocity;
    uint32_t hash = tag;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    const uint32_t slot = hash & (kNoteRegionCacheSize - 1u);
    NoteRegionCacheEntry& cached = noteRegionCache_[slot];
    if (cached.tag == tag && cached.count <= kNoteRegionCacheLayers) {
        ++telemetry_.noteRegionCacheHits;
        const uint32_t count = cached.count;
        const uint32_t copied = (std::min)(count, outCapacity);
        for (uint32_t i = 0; i < copied; ++i)
            outRegions[i] = &data->regions[cached.regionIndices[i]];
        return count;
    }

    ++telemetry_.noteRegionCacheMisses;
    const uint32_t count = sf2_find_regions(data, presetIndex, note,
                                            velocity, outRegions, outCapacity);
    if (count <= kNoteRegionCacheLayers && count <= outCapacity) {
        cached.tag = tag;
        cached.count = static_cast<uint16_t>(count);
        cached.reserved = 0u;
        for (uint32_t i = 0; i < count; ++i) {
            cached.regionIndices[i] = static_cast<uint32_t>(
                outRegions[i] - data->regions);
        }
    }
    return count;
}

void Driver::RefreshSelectedPresets() {
    if (!channelCache || !activeSoundFontStack_) return;
    for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
        uint32_t presetIndex = 0u;
        uint8_t soundFontIndex = 0u;
        if (ResolveChannelPreset(activeSoundFontStack_, *channelCache, channel,
                                 &soundFontIndex, &presetIndex)) {
            channelSoundFontIndex_[channel] = soundFontIndex;
            channelCache->SetSelectedPreset(channel,
                static_cast<uint16_t>(presetIndex));
        } else {
            channelSoundFontIndex_[channel] = 0u;
            channelCache->SetSelectedPreset(channel, UINT16_MAX);
        }
    }
}

uint64_t Driver::HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity,
                              bool deferLifetimeCounters,
                              const NoteLaunchPlanCacheEntry* exactFramePlan) {
    if (!channelCache || !voiceManager) return 0u;
    if (channel >= kChannelCount || note >= kNoteCount) return 0u;
    velocity &= 0x7fu;

    if (!deferLifetimeCounters) ++sf2Telemetry_.noteOns;
    const bool captureDetail = captureSf2Detail_;

    const float velGain = configuredVelocityGain_[velocity];
    if (velGain <= 0.0f) return 0u;

    channelCache->NoteOn(channel, note, velocity);

    if (!activeSoundFontStack_ || !sampleDataStore) return 0u;

    // Bank/program handlers commit this cache at their exact event frame.
    // The fallback covers reset and SoundFont-swap boundaries only; the
    // multi-million-NPS path therefore avoids scanning the preset table for
    // every repeated note.
    uint32_t presetIndex = channelCache->GetSelectedPreset(channel);
    uint8_t soundFontIndex = channelSoundFontIndex_[channel];
    SoundFontBundle* bank = SoundFontBankAt(activeSoundFontStack_,
                                            soundFontIndex);
    if (!bank || !bank->data || !bank->samples ||
        presetIndex >= bank->data->presetCount) {
        if (!ResolveChannelPreset(activeSoundFontStack_, *channelCache, channel,
                                  &soundFontIndex, &presetIndex)) {
            ++sf2Telemetry_.invalidPresets;
            return 0u;
        }
        channelSoundFontIndex_[channel] = soundFontIndex;
        channelCache->SetSelectedPreset(channel,
                                        static_cast<uint16_t>(presetIndex));
        bank = SoundFontBankAt(activeSoundFontStack_, soundFontIndex);
        if (!bank || !bank->data || !bank->samples) return 0u;
    }
    SF2Data* data = bank->data;
    // Probe the complete launch-plan cache before doing even the cached
    // region lookup.  The former ordering resolved/copied regions and
    // revalidated every layer before discovering that the fully prepared
    // plan was already present.  At Black-MIDI rates that redundant work is
    // paid close to a million times per second.
    auto planMatches = [&](const NoteLaunchPlanCacheEntry* entry) {
        return entry &&
            entry->soundFontGeneration == soundFontGeneration_ &&
            entry->channelRevision == channelLaunchRevision_[channel] &&
            entry->presetIndex == presetIndex &&
            entry->soundFontIndex == soundFontIndex &&
            entry->channel == channel &&
            entry->note == note && entry->velocity == velocity &&
            entry->count != 0u &&
            entry->count <= kNoteRegionCacheLayers;
    };
    NoteLaunchPlanCacheEntry* launchCache = const_cast<
        NoteLaunchPlanCacheEntry*>(exactFramePlan);
    bool launchCacheHit = exactFramePlan != nullptr;
    if (!launchCacheHit)
        launchCache = noteLaunchHotCache_[channel][note];
    if (!launchCacheHit) launchCacheHit = planMatches(launchCache);
    if (!launchCacheHit) {
        uint32_t launchHash = presetIndex * 0x9e3779b9u;
        launchHash ^= static_cast<uint32_t>(note) * 0x85ebca6bu;
        launchHash ^= static_cast<uint32_t>(velocity) * 0xc2b2ae35u;
        launchHash ^= static_cast<uint32_t>(channel) * 0x27d4eb2fu;
        launchHash ^= static_cast<uint32_t>(soundFontIndex) * 0xd3a2646cu;
        launchHash ^= channelLaunchRevision_[channel] * 0x165667b1u;
        launchHash ^= launchHash >> 16u;
        launchCache = &noteLaunchPlanCache_[
            launchHash & (kNoteRegionCacheSize - 1u)];
        launchCacheHit = planMatches(launchCache);
        noteLaunchHotCache_[channel][note] = launchCache;
    }

    uint32_t matchCount = launchCacheHit ? launchCache->count
        : ResolveNoteRegions(bank, soundFontIndex, presetIndex, note, velocity,
                             noteRegionScratch_, kMaxMatchingRegions);
    if (matchCount > kMaxMatchingRegions) {
        ++telemetry_.allocationFailures;
        return 0u;
    }

    if (matchCount == 0) {
        // Region fallback: some SoundFont presets cover only part of the
        // keyboard (or lack velocity layers at this spot). Resolve against
        // the bank's widest-coverage preset so incomplete instruments stay
        // audible instead of silently dropping notes. The launch-plan cache
        // still stores the result under the original preset tag, so repeat
        // note-ons pay no repeated lookup.
        const uint16_t fallbackPreset = data->fallbackPresetIndex;
        if (fallbackPreset < data->presetCount &&
            fallbackPreset != presetIndex) {
            matchCount = ResolveNoteRegions(bank, soundFontIndex,
                fallbackPreset, note, velocity, noteRegionScratch_,
                kMaxMatchingRegions);
        }
        if (matchCount == 0) {
            ++telemetry_.zeroMatchedRegions;
            ++sf2Telemetry_.zeroMatchedRegions;
            return 0u;
        }
        if (!deferLifetimeCounters)
            ++sf2Telemetry_.fallbackRegionMatches;
    }

    // Validate every layer before mutating the voice pool. A malformed
    // SoundFont region must reject the complete generation, never leave a
    // partial instrument whose remaining layers become audible artifacts.
    if (!launchCacheHit) {
        for (uint32_t mi = 0; mi < matchCount; ++mi) {
            const SFSampleRegion* region = noteRegionScratch_[mi];
            if (!region || region->sampleIndex >= bank->sampleCount) {
                ++sf2Telemetry_.invalidRegions;
                return 0u;
            }
            const uint32_t regionIndex = static_cast<uint32_t>(
                region - data->regions);
            const bool valid = bank->preparedRegions &&
                    regionIndex < bank->preparedRegionCount
                ? bank->preparedRegions[regionIndex].valid != 0u
                : sf2_validate_region(data, region);
            if (!valid) {
                ++sf2Telemetry_.invalidSampleRanges;
                return 0u;
            }
        }
    }
    if (!deferLifetimeCounters)
        sf2Telemetry_.exactRegionMatches += matchCount;

    // All regions layered by this one MIDI note-on share a generation.  A
    // later note-off must release only this generation's oldest outstanding
    // retrigger, not every voice with the same channel/key.
    if (nextPlayIndex_ == 0 || nextPlayIndex_ >= UINT32_MAX - 1)
        nextPlayIndex_ = 1;
    const uint32_t playIndex = nextPlayIndex_++;

    const float sr = static_cast<float>(
        sampleRate > 0 ? sampleRate : 44100u);
    const float pitchBendSemitones =
        channelCache->GetPitchBendSemitones(channel) +
        sysexMasterFineTune_ + sysexMasterTranspose_;
    const float commonBendRatio = channelPitchBendRatio_[channel];
    const VoiceConfiguration* launchSetups = noteLaunchScratch_;
    if (launchCacheHit) {
        // The cached setup is immutable. playIndex is the only per-note field;
        // pass it separately instead of copying every layer into scratch just
        // to patch four bytes at multi-million-note rates.
        launchSetups = launchCache->setup;
    } else for (uint32_t mi = 0; mi < matchCount; ++mi) {
        const SFSampleRegion* matchedRegion = noteRegionScratch_[mi];
        const uint32_t matchedRegionIndex = static_cast<uint32_t>(
            matchedRegion - data->regions);
        const PreparedSF2Region* prepared =
            bank->preparedRegions &&
                    matchedRegionIndex < bank->preparedRegionCount
                ? &bank->preparedRegions[matchedRegionIndex] : nullptr;
        uint32_t sampleIndex = matchedRegion->sampleIndex;
        const SF2Sample& samp = bank->samples[sampleIndex];

        const float bendScale = prepared ? prepared->bendScale
            : static_cast<float>(matchedRegion->scaleTuning != 0
                ? matchedRegion->scaleTuning : 100) / 100.0f;
        float basePhaseStep;
        if (prepared) {
            basePhaseStep = prepared->basePhaseStep[note];
        } else {
            const int rootKey = matchedRegion->rootKey >= 0
                ? matchedRegion->rootKey : static_cast<int>(samp.originalPitch);
            const float tune = static_cast<float>(matchedRegion->coarseTune) +
                static_cast<float>(matchedRegion->fineTune) / 100.0f;
            const float semitones =
                (static_cast<float>(note) + tune - static_cast<float>(rootKey)) *
                bendScale;
            const float sourceRate = static_cast<float>(
                samp.sampleRate > 0 ? samp.sampleRate : 44100u);
            const float outputRate = static_cast<float>(
                sampleRate > 0 ? sampleRate : 44100u);
            basePhaseStep = sourceRate / outputRate *
                powf(2.0f, semitones / 12.0f);
        }
        const float bendRatio = bendScale == 1.0f || pitchBendSemitones == 0.0f
            ? commonBendRatio
            : powf(2.0f, pitchBendSemitones * bendScale / 12.0f);
        float phaseStep = basePhaseStep * bendRatio;
        // A corrupt SF2 pitch generator must not poison the scalar loop with
        // NaN/Inf phase values: float-to-integer conversion then pins sample
        // lookup unpredictably and silently poisons the mixed output.
        if (!std::isfinite(phaseStep) || phaseStep <= 0.0f) {
            phaseStep = 1.0f;
        }

        uint32_t sStart = bank->sampleBase +
            static_cast<uint32_t>(matchedRegion->startOffset);
        uint32_t sEnd = bank->sampleBase +
            static_cast<uint32_t>(matchedRegion->endOffset);
        uint32_t sLoopStart = bank->sampleBase +
            static_cast<uint32_t>(matchedRegion->loopStartOffset);
        uint32_t sLoopEnd = bank->sampleBase +
            static_cast<uint32_t>(matchedRegion->loopEndOffset);
        uint8_t loopMode = matchedRegion->loopMode;

        float initialGain;
        float sustainLevel;
        uint32_t delaySamples;
        uint32_t holdSamples;
        uint32_t attackSamples;
        uint32_t decaySamples;
        float decaySlope;
        float releaseDecay;
        uint32_t releaseSamples;
        float regionPanLeft;
        float regionPanRight;
        if (prepared) {
            initialGain = velGain * prepared->attenuationGain;
            sustainLevel = prepared->sustainLevel;
            delaySamples = prepared->delaySamples;
            holdSamples = prepared->holdSamples;
            attackSamples = prepared->attackSamples;
            decaySamples = prepared->decaySamples;
            decaySlope = prepared->decaySlope;
            releaseDecay = prepared->releaseDecay;
            releaseSamples = prepared->releaseSamples;
            regionPanLeft = prepared->panLeft;
            regionPanRight = prepared->panRight;
        } else {
            initialGain = velGain;
            if (matchedRegion->initialAttenuation > 0)
                initialGain *= InitialAttenuationToGain(
                    static_cast<float>(matchedRegion->initialAttenuation));
            sustainLevel = SustainAttenuationToGain((std::max)(
                0.0f, static_cast<float>(matchedRegion->sustainVolEnv)));
            if (sustainLevel > 1.0f) sustainLevel = 1.0f;
            const float delaySeconds = TimecentsToSeconds(matchedRegion->delayVolEnv);
            const float holdSeconds = TimecentsToSeconds(matchedRegion->holdVolEnv);
            const float attackSeconds = TimecentsToSeconds(matchedRegion->attackVolEnv);
            const float decaySeconds = TimecentsToSeconds(matchedRegion->decayVolEnv);
            const float releaseSeconds = TimecentsToSeconds(matchedRegion->releaseVolEnv);
            delaySamples = delaySeconds > 0.0f
                ? static_cast<uint32_t>(delaySeconds * sr) : 0u;
            holdSamples = holdSeconds > 0.0f
                ? static_cast<uint32_t>(holdSeconds * sr) : 0u;
            attackSamples = attackSeconds > 0.0001f
                ? static_cast<uint32_t>(attackSeconds * sr) : 0u;
            decaySamples = decaySeconds > 0.0001f
                ? static_cast<uint32_t>(decaySeconds * sr) : 0u;
            decaySlope = 1.0f;
            if (decaySamples > 0u) {
                const float slope = -9.226f / static_cast<float>(decaySamples);
                decaySlope = expf(slope);
                if (sustainLevel > 0.0f && sustainLevel < 1.0f)
                    decaySamples = static_cast<uint32_t>(logf(sustainLevel) / slope);
            }
            releaseDecay = MakeReleaseDecay(releaseSeconds, sampleRate);
            releaseSamples = MakeReleaseSamples(releaseSeconds, sampleRate);
            regionPanLeft = 1.0f;
            regionPanRight = 1.0f;
            channelCache->ComputeSoundFontPan(matchedRegion->pan,
                                              regionPanLeft, regionPanRight);
        }
        const float attackGainStep = attackSamples > 0u
            ? initialGain / static_cast<float>(attackSamples) : 0.0f;

        VoiceConfiguration setup{};
        setup.sampleStart = sStart;
        setup.sampleEnd = sEnd;
        setup.loopStart = sLoopStart;
        setup.loopEnd = sLoopEnd;
        setup.delaySamples = delaySamples;
        setup.holdSamples = holdSamples;
        setup.attackSamples = attackSamples;
        setup.decaySamples = decaySamples;
        setup.releaseSamples = releaseSamples;
        setup.phaseStep = phaseStep;
        setup.basePhaseStep = basePhaseStep;
        setup.pitchBendScale = bendScale;
        setup.initialGain = initialGain;
        setup.sustainLevel = sustainLevel;
        setup.attackGainStep = attackGainStep;
        setup.decaySlope = decaySlope;
        setup.releaseDecay = releaseDecay;
        setup.gainLeft = regionPanLeft;
        setup.gainRight = regionPanRight;
        setup.presetIndex = static_cast<uint16_t>(presetIndex);
        setup.regionIndex = static_cast<uint16_t>(matchedRegionIndex);
        setup.loopMode = loopMode;
        setup.sampleBacked = 1u;
        if (prepared) {
            setup.vibLfoToPitchCents = prepared->vibLfoToPitchCents;
            setup.vibLfoPhaseStep = prepared->vibLfoPhaseStep;
            setup.vibLfoDelaySamples = prepared->vibLfoDelaySamples;
        } else {
            const float vibDelaySeconds =
                TimecentsToSeconds(matchedRegion->delayVibLfo);
            setup.vibLfoToPitchCents =
                static_cast<float>(matchedRegion->vibLfoToPitch);
            setup.vibLfoPhaseStep =
                powf(2.0f, static_cast<float>(
                           matchedRegion->freqVibLfo) / 1200.0f) / sr;
            setup.vibLfoDelaySamples = vibDelaySeconds > 0.0f
                ? static_cast<uint32_t>(vibDelaySeconds * sr) : 0u;
        }
        noteLaunchScratch_[mi] = setup;
    }

    if (!launchCacheHit && matchCount <= kNoteRegionCacheLayers) {
        launchCache->soundFontGeneration = soundFontGeneration_;
        launchCache->channelRevision = channelLaunchRevision_[channel];
        launchCache->presetIndex = static_cast<uint16_t>(presetIndex);
        launchCache->soundFontIndex = soundFontIndex;
        launchCache->channel = channel;
        launchCache->note = note;
        launchCache->velocity = velocity;
        launchCache->count = static_cast<uint8_t>(matchCount);
        for (uint32_t layer = 0u; layer < matchCount; ++layer)
            launchCache->setup[layer] = noteLaunchScratch_[layer];
    }

    if (!voiceManager->LaunchVoiceGroup(
            channel, note, velocity, launchSetups, matchCount, playIndex,
            channelCache->GetParams()[channel], noteLaunchHandles_)) {
        ++telemetry_.allocationFailures;
        return matchCount;
    }

    if (!deferLifetimeCounters) sf2Telemetry_.configuredVoices += matchCount;
    const uint64_t lifetimeDelta = static_cast<uint64_t>(matchCount) |
        (static_cast<uint64_t>(matchCount) << 32u);
    if (!captureDetail) return lifetimeDelta;
    captureSf2Detail_ = false;
    sf2Telemetry_.lastChannel = channel;
    sf2Telemetry_.lastNote = note;
    sf2Telemetry_.lastVelocity = velocity;
    sf2Telemetry_.lastPreset = static_cast<uint16_t>(presetIndex);
    const uint32_t last = matchCount - 1u;
    const VoiceConfiguration& lastSetup = launchSetups[last];
    const VoiceHandle lastVoice = noteLaunchHandles_[last];
    const SFSampleRegion* lastRegion =
        &data->regions[lastSetup.regionIndex];
    sf2Telemetry_.lastRegion = lastSetup.regionIndex;
    sf2Telemetry_.lastSample = static_cast<uint16_t>(lastRegion->sampleIndex);
    sf2Telemetry_.lastSampleStart = lastSetup.sampleStart;
    sf2Telemetry_.lastSampleEnd = lastSetup.sampleEnd;
    sf2Telemetry_.lastInitialPeak =
        bank->regionInitialPeaks &&
                lastSetup.regionIndex < bank->regionInitialPeakCount
            ? bank->regionInitialPeaks[lastSetup.regionIndex] : 0.0f;
    sf2Telemetry_.lastVoiceGain = lastSetup.initialGain;
    sf2Telemetry_.lastMixGainL = voiceManager->v.mixGainL[lastVoice];
    sf2Telemetry_.lastMixGainR = voiceManager->v.mixGainR[lastVoice];
    sf2Telemetry_.lastDelaySamples = lastSetup.delaySamples;
    sf2Telemetry_.lastAttackSamples = lastSetup.attackSamples;
    sf2Telemetry_.lastFloatSample = static_cast<float>(sampleDataStore[lastSetup.sampleStart]);
    sf2Telemetry_.lastPhaseStep = lastSetup.phaseStep;
    sf2Telemetry_.lastPhase = voiceManager->v.phases[lastVoice];
    sf2Telemetry_.lastRelativeEnd = voiceManager->v.relEnd[lastVoice];
    sf2Telemetry_.lastSampleBacked = voiceManager->v.sampleBacked[lastVoice];
    sf2Telemetry_.lastVoiceHandle = lastVoice;
    return lifetimeDelta;
}

void Driver::HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset) {
    if (!channelCache || !voiceManager) return;

    bool sustain = channelCache->IsSustainActive(channel);
    channelCache->NoteOff(channel, note);

    // Use the voice's SF2-defined release — no adaptive release based on
    // pool pressure.  BASSMIDI and SnappySynth both use SF2-specified
    // release times; adaptive release causes audible inconsistency under
    // varying polyphony loads.
    const uint32_t playIndex = voiceManager->FindOldestPlayIndex(channel, note);
    if (playIndex == UINT32_MAX) return;
    voiceManager->NoteOffPlayIndex(channel, note, playIndex, sustain, blockOffset);
}

void Driver::HandleStaleNoteOffBatch(uint8_t channel, uint8_t note,
                                     uint8_t count,
                                     uint32_t blockOffset) {
    if (!channelCache || !voiceManager || count == 0u) return;
    const bool sustain = channelCache->IsSustainActive(channel);
    channelCache->NoteOff(channel, note);
    voiceManager->NoteOffOldestPlayIndices(channel, note, count, sustain,
                                           blockOffset);
}

void Driver::HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value,
                                 uint32_t blockOffset) {
    const bool sustainWasActive = channelCache && channelCache->IsSustainActive(channel);
    const bool sostenutoWasActive =
        channelCache && channelCache->IsSostenutoActive(channel);
    if (channelCache) channelCache->ControlChange(channel, controller, value);
    if (channel < kChannelCount &&
        (controller == 0u || controller == 32u || controller == 6u ||
         controller == 38u || controller == 96u || controller == 97u ||
         controller == 100u || controller == 101u)) {
        ++channelLaunchRevision_[channel];
    }

    if ((controller == 0 || controller == 32) && channelCache &&
        activeSoundFontStack_ &&
        channel < kChannelCount) {
        uint32_t presetIndex = 0u;
        uint8_t soundFontIndex = 0u;
        if (ResolveChannelPreset(activeSoundFontStack_, *channelCache, channel,
                                 &soundFontIndex, &presetIndex)) {
            channelSoundFontIndex_[channel] = soundFontIndex;
            channelCache->SetSelectedPreset(channel,
                                            static_cast<uint16_t>(presetIndex));
        } else {
            channelCache->SetSelectedPreset(channel, UINT16_MAX);
        }
    }

    if (controller == 64) {
        if (value < 64) voiceManager->ReleaseSustain(channel, blockOffset);
    } else if (controller == 66) {
        const bool sostenutoIsActive = value >= 64;
        if (!sostenutoWasActive && sostenutoIsActive)
            voiceManager->CaptureSostenuto(channel);
        else if (sostenutoWasActive && !sostenutoIsActive)
            voiceManager->ReleaseSostenuto(channel, blockOffset);
    }

    if (controller == 120) {
        voiceManager->SilenceChannelImmediate(channel);
    } else if (controller == 123) {
        voiceManager->ReleaseChannel(channel, blockOffset);
    } else if (controller == 121) {
        if (sustainWasActive)
            voiceManager->ReleaseSustain(channel, blockOffset);
        if (sostenutoWasActive)
            voiceManager->ReleaseSostenuto(channel, blockOffset);
    }

    if (controller == 121) {
        // Reset All Controllers also centers the wheel. Recompute the phase
        // increment of already sounding voices at this exact event frame.
        HandlePitchBend(channel, 0, 64);
    }

    if (channelCache && (controller == 6u || controller == 38u ||
                         controller == 96u || controller == 97u)) {
        const uint16_t wheel = channelCache->GetPitchBendValue(channel);
        HandlePitchBend(channel, static_cast<uint8_t>(wheel & 0x7fu),
                        static_cast<uint8_t>(wheel >> 7u));
    }

    // Controller events are dispatched before the sample at their target
    // frame. Rebuild now so existing voices and same-frame note-ons observe
    // the new channel state rather than waiting for the next callback.
    if (channelCache && configSnapshot &&
        (controller == 7 || controller == 10 || controller == 11 ||
         controller == 64 || controller == 121 || controller == 6 ||
         controller == 38 || controller == 96 || controller == 97 ||
         controller == 1)) {
        channelCache->RebuildChannel(channel, *configSnapshot,
                                     static_cast<float>(sampleRate));
        // Keep the vibrato pass on the exact sysex-aware common ratio even
        // when this rebuild recomputed the snapshot from channel cents.
        channelCache->SetBendRatio(channel, channelPitchBendRatio_[channel]);
        if (controller == 7 || controller == 10 || controller == 11 ||
            controller == 121) {
            voiceManager->RefreshMixGainsForChannel(
                channel, channelCache->GetParams()[channel]);
        }
    }
}

void Driver::HandleChannelPressure(uint8_t channel, uint8_t value) {
    if (!channelCache || channel >= kChannelCount) return;
    channelCache->ChannelPressure(channel, value);
    if (!configSnapshot) return;
    channelCache->RebuildChannel(channel, *configSnapshot,
                                 static_cast<float>(sampleRate));
    channelCache->SetBendRatio(channel, channelPitchBendRatio_[channel]);
}

void Driver::HandleProgramChange(uint8_t channel, uint8_t program) {
    if (!channelCache || !activeSoundFontStack_ ||
        channel >= kChannelCount) return;

    // Validate against the bank currently selected on this channel before
    // committing the new program. Existing voices retain their stored region.
    const uint8_t oldProgram = channelCache->GetProgram(channel);
    channelCache->ProgramChange(channel, program);

    uint32_t presetIndex = 0;
    uint8_t soundFontIndex = 0u;
    if (ResolveChannelPreset(activeSoundFontStack_, *channelCache, channel,
                             &soundFontIndex, &presetIndex)) {
        channelSoundFontIndex_[channel] = soundFontIndex;
        channelCache->SetSelectedPreset(channel, static_cast<uint16_t>(presetIndex));
        ++channelLaunchRevision_[channel];
        return;
    }

    // Invalid selection: restore the prior program and leave the prior preset
    // active, matching TSF's failed preset-selection behavior.
    channelCache->ProgramChange(channel, oldProgram);
}

void Driver::HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb) {
    if (channelCache)
        channelCache->PitchBend(channel, static_cast<int16_t>((msb << 7) | lsb));

    if (!voiceManager || !channelCache || channel >= kChannelCount) return;
    ++channelLaunchRevision_[channel];

    const float bendSemitones = channelCache->GetPitchBendSemitones(channel) +
        sysexMasterFineTune_ + sysexMasterTranspose_;
    const float commonRatio = powf(2.0f, bendSemitones / 12.0f);
    channelPitchBendRatio_[channel] = commonRatio;
    channelCache->SetBendRatio(channel, commonRatio);
    voiceManager->ForEachChannelActive(channel, [&](VoiceHandle voice) {
        const uint32_t i = voice;
        const float scale = voiceManager->v.pitchBendScales[i];
        const float ratio = scale == 1.0f
            ? commonRatio : powf(2.0f, bendSemitones * scale / 12.0f);
        voiceManager->v.phaseIncs[i] = voiceManager->v.basePhaseIncs[i] * ratio;
    });
}

void Driver::RefreshAllPitchIncrements() {
    if (!voiceManager || !channelCache) return;
    for (uint8_t channel = 0u; channel < kChannelCount; ++channel) {
        ++channelLaunchRevision_[channel];
        const float semitones = channelCache->GetPitchBendSemitones(channel) +
            sysexMasterFineTune_ + sysexMasterTranspose_;
        const float commonRatio = powf(2.0f, semitones / 12.0f);
        channelPitchBendRatio_[channel] = commonRatio;
        channelCache->SetBendRatio(channel, commonRatio);
        voiceManager->ForEachChannelActive(channel, [&](VoiceHandle voice) {
            const uint32_t handle = voice;
            const float scale = voiceManager->v.pitchBendScales[handle];
            const float ratio = scale == 1.0f
                ? commonRatio : powf(2.0f, semitones * scale / 12.0f);
            voiceManager->v.phaseIncs[handle] =
                voiceManager->v.basePhaseIncs[handle] * ratio;
        });
    }
}

} // namespace svms

static svms::Driver* g_driver = nullptr;
static CRITICAL_SECTION g_frontendLock;
static std::atomic<uint32_t> g_winmmOwners{0u};
static std::atomic<uint32_t> g_nativeOwners{0u};
static std::atomic<bool> g_kdmapiInitialized{false};
static const HMIDIOUT kSVMSMidiOutHandle = reinterpret_cast<HMIDIOUT>(0x1234);
static DWORD_PTR g_midiOutCallback = 0u;
static DWORD_PTR g_midiOutInstance = 0u;
static DWORD g_midiOutCallbackFlags = CALLBACK_NULL;

// MIDIHDR gained trailing fields in WinMM 4.0 and callers can use a
// different structure packing than the proxy.  Submission only needs the
// fields through dwFlags, so accept every ABI that supplies those fields.
static bool HasMidiOutHeaderFields(const MIDIHDR* header, UINT byteCount) {
    const UINT required = static_cast<UINT>(FIELD_OFFSET(MIDIHDR, dwFlags) +
                                             sizeof(header->dwFlags));
    return header != nullptr && byteCount >= required;
}

static void NotifyMidiOutClient(UINT message, DWORD_PTR param1 = 0u,
                                DWORD_PTR param2 = 0u) {
    switch (g_midiOutCallbackFlags & CALLBACK_TYPEMASK) {
        case CALLBACK_FUNCTION:
            if (g_midiOutCallback) {
                using CallbackProc = void (CALLBACK*)(
                    HMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
                reinterpret_cast<CallbackProc>(g_midiOutCallback)(
                    kSVMSMidiOutHandle, message, g_midiOutInstance,
                    param1, param2);
            }
            break;
        case CALLBACK_WINDOW:
            if (g_midiOutCallback) {
                PostMessageW(reinterpret_cast<HWND>(g_midiOutCallback),
                             message,
                             reinterpret_cast<WPARAM>(kSVMSMidiOutHandle),
                             static_cast<LPARAM>(param1));
            }
            break;
        case CALLBACK_THREAD:
            if (g_midiOutCallback) {
                PostThreadMessageW(static_cast<DWORD>(g_midiOutCallback),
                                   message,
                                   reinterpret_cast<WPARAM>(kSVMSMidiOutHandle),
                                   static_cast<LPARAM>(param1));
            }
            break;
        case CALLBACK_EVENT:
            if (g_midiOutCallback)
                SetEvent(reinterpret_cast<HANDLE>(g_midiOutCallback));
            break;
        default:
            break;
    }
}

static bool IsSupportedMidiOutputDevice(UINT_PTR deviceId) {
    return deviceId == 0u || deviceId == static_cast<UINT_PTR>(MIDI_MAPPER);
}

extern "C" {

static bool EnsureDriverInitialized();
static void MaybeShutdownDriver();

BOOL WINAPI PlaySoundA(LPCSTR pszSound, HMODULE hmod, DWORD fdwSound) {
    using Proc = BOOL (WINAPI*)(LPCSTR, HMODULE, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("PlaySoundA"));
    return proc ? proc(pszSound, hmod, fdwSound) : FALSE;
}

BOOL WINAPI PlaySoundW(LPCWSTR pszSound, HMODULE hmod, DWORD fdwSound) {
    using Proc = BOOL (WINAPI*)(LPCWSTR, HMODULE, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("PlaySoundW"));
    return proc ? proc(pszSound, hmod, fdwSound) : FALSE;
}

UINT WINAPI midiOutGetNumDevs(void) {
    XPBootstrapTrace("[SVMS XP] midiOutGetNumDevs reached\r\n");
    LOG("midiOutGetNumDevs -> 1");
    return 1;
}

MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR uDeviceID, LPMIDIOUTCAPSA lpCaps, UINT cbCaps) {
    if (!IsSupportedMidiOutputDevice(uDeviceID) || !lpCaps || cbCaps < sizeof(MIDIOUTCAPSA))
        return MMSYSERR_BADDEVICEID;
    std::memset(lpCaps, 0, cbCaps);
    lpCaps->wMid = 1;
    lpCaps->wPid = 1;
    lpCaps->vDriverVersion = 0x0300;
    std::memcpy(lpCaps->szPname, "SuperVirtualMIDISynth V3", 25);
    lpCaps->wTechnology = MOD_SWSYNTH;
    lpCaps->wVoices = 64;
    lpCaps->wNotes = 64;
    lpCaps->wChannelMask = 0xFFFF;
    lpCaps->dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR uDeviceID, LPMIDIOUTCAPSW lpCaps, UINT cbCaps) {
    if (!IsSupportedMidiOutputDevice(uDeviceID) || !lpCaps || cbCaps < sizeof(MIDIOUTCAPSW))
        return MMSYSERR_BADDEVICEID;
    std::memset(lpCaps, 0, cbCaps);
    lpCaps->wMid = 1;
    lpCaps->wPid = 1;
    lpCaps->vDriverVersion = 0x0300;
    const wchar_t name[] = L"SuperVirtualMIDISynth V3";
    std::memcpy(lpCaps->szPname, name, sizeof(name));
    lpCaps->wTechnology = MOD_SWSYNTH;
    lpCaps->wVoices = 64;
    lpCaps->wNotes = 64;
    lpCaps->wChannelMask = 0xFFFF;
    lpCaps->dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutOpen(LPHMIDIOUT phmo, UINT uDeviceID,
    DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
    XPBootstrapTrace("[SVMS XP] midiOutOpen reached\r\n");
    LOG("midiOutOpen: uDeviceID=%u", uDeviceID);
    if (!IsSupportedMidiOutputDevice(uDeviceID)) {
        XPBootstrapTrace("[SVMS XP] midiOutOpen rejected unsupported device ID\r\n");
        return MMSYSERR_BADDEVICEID;
    }
    if (!phmo) return MMSYSERR_INVALPARAM;

    g_winmmOwners.fetch_add(1u, std::memory_order_acq_rel);
    if (!EnsureDriverInitialized()) {
        g_winmmOwners.fetch_sub(1u, std::memory_order_acq_rel);
        LOG("midiOutOpen: engine start FAILED");
        XPBootstrapTrace("[SVMS XP] engine initialization FAILED\r\n");
        return MMSYSERR_NOMEM;
    }

    LOG("midiOutOpen: SUCCESS, returning handle");
    *phmo = kSVMSMidiOutHandle;
    g_midiOutCallback = dwCallback;
    g_midiOutInstance = dwInstance;
    g_midiOutCallbackFlags = fdwOpen;
    NotifyMidiOutClient(MOM_OPEN);
    XPBootstrapTrace("[SVMS XP] midiOutOpen SUCCESS handle=0x00001234\r\n");
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutClose(HMIDIOUT hmo) {
    if (hmo != kSVMSMidiOutHandle) return MMSYSERR_INVALHANDLE;
    XPBootstrapTrace("[SVMS XP] midiOutClose reached\r\n");
    uint32_t owners = g_winmmOwners.load(std::memory_order_acquire);
    while (owners != 0u && !g_winmmOwners.compare_exchange_weak(
        owners, owners - 1u, std::memory_order_acq_rel,
        std::memory_order_acquire)) {}
    MaybeShutdownDriver();
    NotifyMidiOutClient(MOM_CLOSE);
    g_midiOutCallback = 0u;
    g_midiOutInstance = 0u;
    g_midiOutCallbackFlags = CALLBACK_NULL;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutShortMsg(HMIDIOUT hmo, DWORD dwMsg) {
    static LONG traceCount = 0;
    const LONG traceIndex = InterlockedIncrement(&traceCount);
    if (traceIndex <= 32) {
        char message[256] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] midiOutShortMsg #%ld handle=%p raw=0x%08lX status=0x%02lX data1=%lu data2=%lu driver=%s\r\n",
                      static_cast<long>(traceIndex), static_cast<void*>(hmo),
                      static_cast<unsigned long>(dwMsg),
                      static_cast<unsigned long>(dwMsg & 0xFFu),
                      static_cast<unsigned long>((dwMsg >> 8) & 0x7Fu),
                      static_cast<unsigned long>((dwMsg >> 16) & 0x7Fu),
                      g_driver ? "ready" : "null");
        OutputDebugStringA(message);
    }
    if (g_driver) {
        static int msgCount = 0;
        if (msgCount < 15) {
            LOG("midiOutShortMsg #%d: 0x%08X", msgCount, dwMsg);
        }
        msgCount++;
        g_driver->SubmitShortMsg(dwMsg);
    }
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutLongMsg(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    XPBootstrapTrace("[SVMS XP] midiOutLongMsg reached\r\n");
    if (hmo != kSVMSMidiOutHandle) return MMSYSERR_INVALHANDLE;
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr) ||
        (!lpMidiHdr->lpData && lpMidiHdr->dwBufferLength != 0u))
        return MMSYSERR_INVALPARAM;
    if ((lpMidiHdr->dwFlags & MHDR_PREPARED) == 0u)
        return MIDIERR_UNPREPARED;
    lpMidiHdr->dwFlags &= ~MHDR_DONE;
    lpMidiHdr->dwFlags |= MHDR_INQUEUE;
    if (g_driver && lpMidiHdr->dwBufferLength != 0u) {
        g_driver->SubmitSystemExclusive(
            reinterpret_cast<const uint8_t*>(lpMidiHdr->lpData),
            lpMidiHdr->dwBufferLength);
    }
    lpMidiHdr->dwFlags &= ~MHDR_INQUEUE;
    lpMidiHdr->dwFlags |= MHDR_DONE;
    NotifyMidiOutClient(MOM_DONE,
                        reinterpret_cast<DWORD_PTR>(lpMidiHdr), 0u);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutReset(HMIDIOUT hmo) {
    (void)hmo;
    if (g_driver) g_driver->ResetAllVoices();
    LOG("midiOutReset: all voices released");
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (hmo != kSVMSMidiOutHandle) return MMSYSERR_INVALHANDLE;
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr))
        return MMSYSERR_INVALPARAM;
    lpMidiHdr->dwFlags |= MHDR_PREPARED;
    lpMidiHdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (hmo != kSVMSMidiOutHandle) return MMSYSERR_INVALHANDLE;
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr))
        return MMSYSERR_INVALPARAM;
    if ((lpMidiHdr->dwFlags & MHDR_INQUEUE) != 0u)
        return MIDIERR_STILLPLAYING;
    lpMidiHdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetVolume(HMIDIOUT hmo, LPDWORD pdwVolume) {
    (void)hmo;
    if (!pdwVolume) return MMSYSERR_INVALPARAM;
    *pdwVolume = 0xFFFFFFFF;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutSetVolume(HMIDIOUT hmo, DWORD dwVolume) {
    (void)hmo; (void)dwVolume;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT mmrError, LPSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const char* msg = "Unknown error";
    switch (mmrError) {
        case MMSYSERR_NOERROR: msg = "No error"; break;
        case MMSYSERR_BADDEVICEID: msg = "Bad device ID"; break;
        case MMSYSERR_INVALHANDLE: msg = "Invalid handle"; break;
        case MMSYSERR_INVALPARAM: msg = "Invalid parameter"; break;
        case MMSYSERR_NOMEM: msg = "Out of memory"; break;
    }
    size_t len = std::strlen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len);
    lpText[len] = '\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutGetErrorTextW(MMRESULT mmrError, LPWSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const wchar_t* msg = L"Unknown error";
    switch (mmrError) {
        case MMSYSERR_NOERROR: msg = L"No error"; break;
        case MMSYSERR_BADDEVICEID: msg = L"Bad device ID"; break;
        case MMSYSERR_INVALHANDLE: msg = L"Invalid handle"; break;
        case MMSYSERR_INVALPARAM: msg = L"Invalid parameter"; break;
        case MMSYSERR_NOMEM: msg = L"Out of memory"; break;
    }
    size_t len = wcslen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len * sizeof(wchar_t));
    lpText[len] = L'\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutMessage(HMIDIOUT hmo, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hmo; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_NOERROR;
}

// ── KDMAPI / OmniMIDI extensions ──────────────────────────────────────

static bool EnsureDriverInitialized() {
    EnterCriticalSection(&g_frontendLock);
    if (g_driver && g_driver->initialized) {
        LeaveCriticalSection(&g_frontendLock);
        return true;
    }
    if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
    g_driver = &svms::Driver::Instance();
    if (!g_driver->Initialize()) {
        LOG("KDMAPI: Initialize FAILED");
        XPBootstrapTrace("[SVMS XP] KDMAPI engine initialization FAILED\r\n");
#if !defined(SVMS_XP_COMPAT)
        g_driver->Shutdown();
        g_driver = nullptr;
#endif
        LeaveCriticalSection(&g_frontendLock);
        return false;
    }
    g_driver->LoadConfiguredSoundFont();
    const bool started = g_driver->StartAudio();
    LeaveCriticalSection(&g_frontendLock);
    return started;
}

static void MaybeShutdownDriver() {
    if (g_winmmOwners.load(std::memory_order_acquire) != 0u ||
        g_nativeOwners.load(std::memory_order_acquire) != 0u ||
        g_kdmapiInitialized.load(std::memory_order_acquire))
        return;
    EnterCriticalSection(&g_frontendLock);
    if (g_winmmOwners.load(std::memory_order_acquire) == 0u &&
        g_nativeOwners.load(std::memory_order_acquire) == 0u &&
        !g_kdmapiInitialized.load(std::memory_order_acquire) && g_driver) {
        g_driver->Shutdown();
        g_driver = nullptr;
    }
    LeaveCriticalSection(&g_frontendLock);
}

// â”€â”€ Native SVMS API â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static constexpr uint32_t kNativeSessionCapacity = 64u;
static std::atomic<uint64_t> g_nativeSessions[kNativeSessionCapacity]{};
static std::atomic<uint64_t>
    g_nativeSessionCancellation[kNativeSessionCapacity]{};
static std::atomic<uint32_t> g_nativeSessionGeneration{1u};
static svms::NativeOfflineSessions g_nativeOfflineSessions;

static bool NativeSessionIsValid(SVMS_Session session) {
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    if (encodedIndex == 0u || encodedIndex > kNativeSessionCapacity)
        return false;
    return g_nativeSessions[encodedIndex - 1u].load(
        std::memory_order_acquire) == session;
}

static std::atomic<uint64_t>* NativeSessionCancellation(
    SVMS_Session session) {
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    if (encodedIndex == 0u || encodedIndex > kNativeSessionCapacity ||
        g_nativeSessions[encodedIndex - 1u].load(std::memory_order_acquire) !=
            session)
        return nullptr;
    return &g_nativeSessionCancellation[encodedIndex - 1u];
}

static SVMS_Result SVMS_CALL NativeCreateSession(
    const SVMS_SessionConfig* config, SVMS_Session* outSession) {
    if (!outSession) return SVMS_RESULT_INVALID_ARGUMENT;
    *outSession = 0u;
    if (config) {
        if (config->struct_size < 16u ||
            config->struct_version != SVMS_STRUCT_VERSION_1 ||
            config->flags != 0u)
            return SVMS_RESULT_INVALID_ARGUMENT;
    }

    // Reserve engine ownership before initialization so another frontend
    // cannot shut the shared runtime down between StartAudio and slot publish.
    g_nativeOwners.fetch_add(1u, std::memory_order_acq_rel);
    if (!EnsureDriverInitialized()) {
        g_nativeOwners.fetch_sub(1u, std::memory_order_acq_rel);
        return SVMS_RESULT_INTERNAL_ERROR;
    }

    uint32_t generation = g_nativeSessionGeneration.fetch_add(
        1u, std::memory_order_relaxed) + 1u;
    if (generation == 0u)
        generation = g_nativeSessionGeneration.fetch_add(
            1u, std::memory_order_relaxed) + 1u;
    for (uint32_t i = 0u; i < kNativeSessionCapacity; ++i) {
        const uint64_t token = (static_cast<uint64_t>(generation) << 32u) |
                               static_cast<uint64_t>(i + 1u);
        uint64_t empty = 0u;
        if (g_nativeSessions[i].compare_exchange_strong(
                empty, token, std::memory_order_release,
                std::memory_order_relaxed)) {
            *outSession = token;
            return SVMS_RESULT_OK;
        }
    }
    g_nativeOwners.fetch_sub(1u, std::memory_order_acq_rel);
    MaybeShutdownDriver();
    return SVMS_RESULT_NO_RESOURCES;
}

static SVMS_Result SVMS_CALL NativeDestroySession(SVMS_Session session) {
    if (g_nativeOfflineSessions.IsToken(session))
        return g_nativeOfflineSessions.Destroy(session);
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    if (encodedIndex == 0u || encodedIndex > kNativeSessionCapacity)
        return SVMS_RESULT_INVALID_ARGUMENT;
    if (g_nativeSessions[encodedIndex - 1u].load(std::memory_order_acquire) !=
        session)
        return SVMS_RESULT_INVALID_ARGUMENT;
    g_nativeSessionCancellation[encodedIndex - 1u].store(
        session, std::memory_order_release);
    if (g_driver) g_driver->WakeBlockedProducers();
    uint64_t expected = session;
    if (!g_nativeSessions[encodedIndex - 1u].compare_exchange_strong(
            expected, 0u, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return SVMS_RESULT_INVALID_ARGUMENT;
    g_nativeOwners.fetch_sub(1u, std::memory_order_acq_rel);
    MaybeShutdownDriver();
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeSendShort(SVMS_Session session,
                                              uint32_t message) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    LARGE_INTEGER timestamp{};
    QueryPerformanceCounter(&timestamp);
    return g_driver->SubmitShortMsgAtQpcCancellable(
               message, static_cast<uint64_t>(timestamp.QuadPart),
               cancellation, session)
        ? SVMS_RESULT_OK : SVMS_RESULT_CANCELLED;
}

static SVMS_Result SVMS_CALL NativeSendShortAtQpc(
    SVMS_Session session, uint32_t message, uint64_t timestampQpc) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (timestampQpc == 0u) {
        LARGE_INTEGER timestamp{};
        QueryPerformanceCounter(&timestamp);
        timestampQpc = static_cast<uint64_t>(timestamp.QuadPart);
    }
    return g_driver->SubmitShortMsgAtQpcCancellable(
               message, timestampQpc, cancellation, session)
        ? SVMS_RESULT_OK : SVMS_RESULT_CANCELLED;
}

static SVMS_Result SVMS_CALL NativeSendShortBatch(
    SVMS_Session session, const SVMS_ShortEvent* events,
    uint32_t eventCount) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!events && eventCount != 0u) return SVMS_RESULT_INVALID_ARGUMENT;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        if (events[i].reserved != 0u) return SVMS_RESULT_INVALID_ARGUMENT;
    }
    // A batch is one submission boundary. Timestamp its immediate records
    // once, just like NativeSendTimedShortBatch, rather than crossing into
    // QueryPerformanceCounter for every packed MIDI message. Explicit QPC
    // timestamps remain untouched and equal-time records retain array order.
    LARGE_INTEGER immediate{};
    if (eventCount != 0u && !QueryPerformanceCounter(&immediate))
        return SVMS_RESULT_INTERNAL_ERROR;
    const uint64_t immediateQpc = static_cast<uint64_t>(immediate.QuadPart);
    return g_driver->SubmitShortBatchAtQpcCancellable(
               events, eventCount, immediateQpc, cancellation, session)
        ? SVMS_RESULT_OK : SVMS_RESULT_CANCELLED;
}

static SVMS_Result SVMS_CALL NativeSendSystemExclusive(
    SVMS_Session session, const uint8_t* data, uint32_t size) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!data || size < 2u || data[0] != 0xf0u || data[size - 1u] != 0xf7u)
        return SVMS_RESULT_INVALID_ARGUMENT;
    return g_driver->SubmitSystemExclusiveCancellable(
               data, size, cancellation, session)
        ? SVMS_RESULT_OK : SVMS_RESULT_CANCELLED;
}

static SVMS_Result SVMS_CALL NativeReset(SVMS_Session session) {
    if (g_nativeOfflineSessions.IsToken(session))
        return g_nativeOfflineSessions.Reset(session);
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    g_driver->ResetAllVoices();
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetTelemetry(
    SVMS_Session session, SVMS_TelemetryV1* telemetry) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!telemetry || telemetry->struct_size < sizeof(SVMS_TelemetryV1) ||
        telemetry->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    svms::DriverDebugInfo debug{};
    svms::SnappyVoiceStatistics voices{};
    g_driver->CopyDebugInfo(debug);
    g_driver->CopyVoiceStatistics(voices);
    SVMS_TelemetryV1 result{};
    result.struct_size = sizeof(result);
    result.struct_version = SVMS_STRUCT_VERSION_1;
    result.callback_count = debug.callbackCount;
    result.submitted_events = debug.submitted;
    result.accepted_events = debug.accepted;
    result.dispatched_events = debug.dispatched;
    result.note_ons = debug.noteOns;
    result.matched_regions = debug.matchedRegions;
    result.configured_voices = debug.configuredVoices;
    result.voice_steals = voices.voiceSteals;
    result.active_voices = voices.activeVoices;
    result.free_voices = voices.freeVoices;
    result.sample_rate = g_driver->sampleRate;
    result.buffer_frames = g_driver->bufferFrames;
    result.soundfont_loaded = debug.soundFontLoaded;
    result.audio_running = debug.audioRunning;
    result.render_time_ms = g_driver->GetRenderingTimeMilliseconds();
    result.render_peak = debug.renderPeak;
    *telemetry = result;
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetRuntimeClock(
    uint64_t* qpcNow, uint64_t* qpcFrequency) {
    if (!qpcNow || !qpcFrequency) return SVMS_RESULT_INVALID_ARGUMENT;
    LARGE_INTEGER now{}, frequency{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency))
        return SVMS_RESULT_INTERNAL_ERROR;
    *qpcNow = static_cast<uint64_t>(now.QuadPart);
    *qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
    return SVMS_RESULT_OK;
}

static uint64_t QpcTicksToMonotonicNanoseconds(uint64_t ticks,
                                               uint64_t frequency) {
    if (!frequency) return 0u;
    const uint64_t seconds = ticks / frequency;
    const uint64_t remainder = ticks % frequency;
    return seconds * 1000000000ull +
        (remainder * 1000000000ull) / frequency;
}

static uint64_t MonotonicNanosecondsToQpcTicks(uint64_t nanoseconds,
                                               uint64_t frequency) {
    if (!frequency) return 0u;
    const uint64_t seconds = nanoseconds / 1000000000ull;
    const uint64_t remainder = nanoseconds % 1000000000ull;
    return seconds * frequency + (remainder * frequency) / 1000000000ull;
}

static SVMS_Result SVMS_CALL NativeGetMonotonicClock(uint64_t* nanoseconds) {
    if (!nanoseconds) return SVMS_RESULT_INVALID_ARGUMENT;
    LARGE_INTEGER now{}, frequency{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency))
        return SVMS_RESULT_INTERNAL_ERROR;
    *nanoseconds = QpcTicksToMonotonicNanoseconds(
        static_cast<uint64_t>(now.QuadPart),
        static_cast<uint64_t>(frequency.QuadPart));
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeSendTimedShortBatch(
    SVMS_Session session, const SVMS_TimedShortEvent* events,
    uint32_t eventCount) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!events && eventCount != 0u) return SVMS_RESULT_INVALID_ARGUMENT;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        if (events[i].reserved != 0u ||
            events[i].timestamp_domain > SVMS_TIMESTAMP_MONOTONIC_NS ||
            (events[i].timestamp_domain == SVMS_TIMESTAMP_OUTPUT_FRAME &&
             events[i].timestamp > svms::kAbsoluteFrameTimestampMask) ||
            (events[i].timestamp_domain == SVMS_TIMESTAMP_QPC &&
             (events[i].timestamp & svms::kAbsoluteFrameTimestampTag) != 0u))
            return SVMS_RESULT_INVALID_ARGUMENT;
    }
    LARGE_INTEGER immediate{}, frequency{};
    if (!QueryPerformanceCounter(&immediate) ||
        !QueryPerformanceFrequency(&frequency))
        return SVMS_RESULT_INTERNAL_ERROR;
    const uint64_t immediateQpc = static_cast<uint64_t>(immediate.QuadPart);
    const uint64_t qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
    for (uint32_t i = 0u; i < eventCount; ++i) {
        const SVMS_TimedShortEvent& event = events[i];
        switch (event.timestamp_domain) {
        case SVMS_TIMESTAMP_IMMEDIATE:
            if (!g_driver->SubmitShortMsgAtQpcCancellable(
                    event.packed_message, immediateQpc, cancellation, session))
                return SVMS_RESULT_CANCELLED;
            break;
        case SVMS_TIMESTAMP_OUTPUT_FRAME:
            if (!g_driver->SubmitShortMsgAtFrameCancellable(
                    event.packed_message, event.timestamp, cancellation,
                    session))
                return SVMS_RESULT_CANCELLED;
            break;
        case SVMS_TIMESTAMP_QPC:
            if (!g_driver->SubmitShortMsgAtQpcCancellable(
                    event.packed_message, event.timestamp, cancellation,
                    session))
                return SVMS_RESULT_CANCELLED;
            break;
        case SVMS_TIMESTAMP_MONOTONIC_NS:
            if (!g_driver->SubmitShortMsgAtQpcCancellable(
                    event.packed_message,
                    MonotonicNanosecondsToQpcTicks(event.timestamp,
                                                   qpcFrequency),
                    cancellation, session))
                return SVMS_RESULT_CANCELLED;
            break;
        }
    }
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetOutputClock(
    SVMS_Session session, uint64_t* nextOutputFrame, uint32_t* sampleRate) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!nextOutputFrame || !sampleRate) return SVMS_RESULT_INVALID_ARGUMENT;
    *nextOutputFrame = g_driver->GetNextOutputFrame();
    *sampleRate = g_driver->sampleRate;
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeSetIngressMode(
    SVMS_Session session, uint32_t ingressMode) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (ingressMode > SVMS_INGRESS_LOSSLESS)
        return SVMS_RESULT_INVALID_ARGUMENT;
    g_driver->SetIngressMode(ingressMode == SVMS_INGRESS_LOSSLESS
        ? svms::EventOverflowMode::LosslessBackpressure
        : svms::EventOverflowMode::PriorityVelocity);
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetQueueInfo(
    SVMS_Session session, SVMS_QueueInfo* queueInfo) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!queueInfo || queueInfo->struct_size < 16u ||
        queueInfo->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    const uint32_t callerSize = queueInfo->struct_size;
    SVMS_QueueInfo result{};
    g_driver->CopyNativeQueueInfo(result);
    std::memcpy(queueInfo, &result,
                (std::min)(callerSize,
                           static_cast<uint32_t>(sizeof(result))));
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeLoadSoundFontUtf8(
    SVMS_Session session, const char* pathUtf8) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!pathUtf8 || !*pathUtf8) return SVMS_RESULT_INVALID_ARGUMENT;
    const int required = MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1,
                                              nullptr, 0);
    if (required <= 1) return SVMS_RESULT_INVALID_ARGUMENT;
    std::wstring path(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1, path.data(), required) ==
        0)
        return SVMS_RESULT_INVALID_ARGUMENT;
    path.resize(static_cast<size_t>(required - 1));
    return g_driver->LoadSoundFont(path.c_str()) ? SVMS_RESULT_OK
                                                 : SVMS_RESULT_INTERNAL_ERROR;
}

static SVMS_Result SVMS_CALL NativePanic(SVMS_Session session) {
    return NativeReset(session);
}

static bool NativeUtf8ToWide(const char* pathUtf8, std::wstring& path) {
    if (!pathUtf8 || !*pathUtf8) return false;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              pathUtf8, -1, nullptr, 0);
    if (required <= 1) return false;
    path.assign(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pathUtf8, -1,
                            path.data(), required) == 0) {
        path.clear();
        return false;
    }
    path.resize(static_cast<size_t>(required - 1));
    return true;
}

static SVMS_Result SVMS_CALL NativeCreateOfflineSession(
    const SVMS_OfflineSessionConfig* config, const char* soundfontPathUtf8,
    SVMS_Session* outSession) {
    std::wstring soundfont;
    if (!NativeUtf8ToWide(soundfontPathUtf8, soundfont))
        return SVMS_RESULT_INVALID_ARGUMENT;
    return g_nativeOfflineSessions.Create(config, soundfont, outSession);
}

static SVMS_Result SVMS_CALL NativeRenderOffline(
    SVMS_Session session, const SVMS_OfflineEvent* events,
    uint32_t eventCount, float* outputLeft, float* outputRight,
    uint32_t frameCount) {
    return g_nativeOfflineSessions.Render(session, events, eventCount,
                                           outputLeft, outputRight,
                                           frameCount);
}

static SVMS_Result SVMS_CALL NativeGetOfflineTelemetry(
    SVMS_Session session, SVMS_OfflineTelemetry* telemetry) {
    return g_nativeOfflineSessions.GetTelemetry(session, telemetry);
}

static SVMS_Result NativeCopyUtf8(const std::string& value, char* buffer,
                                  uint32_t* inoutBytes) {
    if (!inoutBytes || value.size() >= UINT32_MAX)
        return SVMS_RESULT_INVALID_ARGUMENT;
    const uint32_t required = static_cast<uint32_t>(value.size() + 1u);
    const uint32_t supplied = *inoutBytes;
    *inoutBytes = required;
    if (!buffer || supplied < required) return SVMS_RESULT_BUFFER_TOO_SMALL;
    std::memcpy(buffer, value.c_str(), required);
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetConfigJson(
    SVMS_Session session, char* bufferUtf8, uint32_t* inoutBufferBytes) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    std::string document;
    if (!svms::ReadV3ConfigJson(document)) return SVMS_RESULT_INTERNAL_ERROR;
    return NativeCopyUtf8(document, bufferUtf8, inoutBufferBytes);
}

static SVMS_Result SVMS_CALL NativePatchConfigJson(
    SVMS_Session session, const char* mergePatchUtf8,
    uint32_t mergePatchBytes) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    std::string warning;
    if (svms::PatchV3ConfigJson(mergePatchUtf8, mergePatchBytes, &warning))
        return SVMS_RESULT_OK;
    return warning.find("invalid") != std::string::npos ||
           warning.find("schema") != std::string::npos ||
           warning.find("must be") != std::string::npos
        ? SVMS_RESULT_INVALID_ARGUMENT : SVMS_RESULT_INTERNAL_ERROR;
}

static SVMS_Result SVMS_CALL NativeGetConfigPathUtf8(
    SVMS_Session session, char* bufferUtf8, uint32_t* inoutBufferBytes) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    const std::wstring path = svms::GetV3ConfigPath();
    if (path.empty()) return SVMS_RESULT_INTERNAL_ERROR;
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return SVMS_RESULT_INTERNAL_ERROR;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, path.data(),
                            static_cast<int>(path.size()), utf8.data(), bytes,
                            nullptr, nullptr) == 0)
        return SVMS_RESULT_INTERNAL_ERROR;
    return NativeCopyUtf8(utf8, bufferUtf8, inoutBufferBytes);
}

static SVMS_Result SVMS_CALL NativeCancelSessionSubmissions(
    SVMS_Session session) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    cancellation->store(session, std::memory_order_release);
    if (g_driver) g_driver->WakeBlockedProducers();
    return SVMS_RESULT_OK;
}

SVMS_Result SVMS_CALL SVMS_GetInterface(
    uint32_t requestedAbi, uint32_t callerTableSize,
    SVMS_Interface* outInterface) {
    constexpr uint32_t minimumSize = static_cast<uint32_t>(
        offsetof(SVMS_Interface, get_runtime_clock) +
        sizeof(SVMS_GetRuntimeClockFn));
    if (!outInterface || callerTableSize < minimumSize)
        return SVMS_RESULT_INVALID_ARGUMENT;
    if (requestedAbi != SVMS_ABI_VERSION_1)
        return SVMS_RESULT_UNSUPPORTED_ABI;

    SVMS_Interface table{};
    table.struct_size = sizeof(table);
    table.struct_version = SVMS_STRUCT_VERSION_1;
    table.abi_version = SVMS_ABI_VERSION_1;
    table.capabilities = SVMS_CAP_EXACT_QPC_TIMESTAMPS |
        SVMS_CAP_SHORT_EVENT_BATCH | SVMS_CAP_SYSTEM_EXCLUSIVE |
        SVMS_CAP_TELEMETRY_V1 | SVMS_CAP_KDMAPI_FACADE |
        SVMS_CAP_EXACT_MONOTONIC_NS | SVMS_CAP_EXACT_OUTPUT_FRAMES |
        SVMS_CAP_QUEUE_CONTROL | SVMS_CAP_SOUNDFONT_RELOAD |
        SVMS_CAP_MIXED_TIMESTAMP_BATCH |
        SVMS_CAP_ISOLATED_OFFLINE_SESSIONS | SVMS_CAP_CONFIG_JSON |
        SVMS_CAP_CANCELLABLE_SUBMISSION;
    table.product_major = svms::build::kProductMajor;
    table.product_minor = svms::build::kProductMinor;
    table.product_patch = svms::build::kProductPatch;
    table.build_number = svms::build::kBuildNumber;
    table.create_session = NativeCreateSession;
    table.destroy_session = NativeDestroySession;
    table.send_short = NativeSendShort;
    table.send_short_at_qpc = NativeSendShortAtQpc;
    table.send_short_batch = NativeSendShortBatch;
    table.send_system_exclusive = NativeSendSystemExclusive;
    table.reset = NativeReset;
    table.get_telemetry = NativeGetTelemetry;
    table.get_runtime_clock = NativeGetRuntimeClock;
    table.send_timed_short_batch = NativeSendTimedShortBatch;
    table.get_output_clock = NativeGetOutputClock;
    table.get_monotonic_clock = NativeGetMonotonicClock;
    table.set_ingress_mode = NativeSetIngressMode;
    table.get_queue_info = NativeGetQueueInfo;
    table.load_soundfont_utf8 = NativeLoadSoundFontUtf8;
    table.panic = NativePanic;
    table.create_offline_session = NativeCreateOfflineSession;
    table.render_offline = NativeRenderOffline;
    table.get_offline_telemetry = NativeGetOfflineTelemetry;
    table.get_config_json = NativeGetConfigJson;
    table.patch_config_json = NativePatchConfigJson;
    table.get_config_path_utf8 = NativeGetConfigPathUtf8;
    table.cancel_session_submissions = NativeCancelSessionSubmissions;
    std::memcpy(outInterface, &table,
                (std::min)(callerTableSize,
                           static_cast<uint32_t>(sizeof(table))));
    return SVMS_RESULT_OK;
}

BOOL WINAPI IsKDMAPIAvailable(void) {
    XPBootstrapTrace("[SVMS XP] IsKDMAPIAvailable reached\r\n");
    return TRUE;
}

LPVOID WINAPI InitializeKDMAPIStream(void) {
    const bool wasInitialized = g_kdmapiInitialized.exchange(
        true, std::memory_order_acq_rel);
    if (!EnsureDriverInitialized()) {
        if (!wasInitialized)
            g_kdmapiInitialized.store(false, std::memory_order_release);
        return nullptr;
    }
    return reinterpret_cast<LPVOID>(1);
}

void WINAPI TerminateKDMAPIStream(void) {
    g_kdmapiInitialized.store(false, std::memory_order_release);
    MaybeShutdownDriver();
}

void WINAPI ResetKDMAPIStream(void) {
    if (g_driver) g_driver->ResetAllVoices();
}

UINT WINAPI ReturnKDMAPIVer(LPDWORD pdwMajor, LPDWORD pdwMinor, LPDWORD pdwBuild, LPDWORD pdwRevision) {
    if (pdwMajor) *pdwMajor = 4;
    if (pdwMinor) *pdwMinor = 1;
    if (pdwBuild) *pdwBuild = 0;
    if (pdwRevision) *pdwRevision = 0;
    return TRUE;
}

void WINAPI SendDirectData(DWORD dwMsg) {
    if (!g_kdmapiInitialized.load(std::memory_order_acquire)) return;
    if (g_driver) g_driver->SubmitShortMsg(dwMsg);
}

void WINAPI SendDirectDataNoBuf(DWORD dwMsg) {
    SendDirectData(dwMsg);
}

UINT WINAPI SendCustomEvent(DWORD dwEvent, LPVOID pData, DWORD dwSize) {
    (void)pData; (void)dwSize;
    if (!g_kdmapiInitialized.load(std::memory_order_acquire)) return 0;
    if (g_driver) g_driver->SubmitShortMsg(static_cast<DWORD>(dwEvent));
    return 1;
}

UINT WINAPI SendDirectLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (!g_kdmapiInitialized.load(std::memory_order_acquire) || !g_driver ||
        !HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr) ||
        (!lpMidiHdr->lpData && lpMidiHdr->dwBufferLength != 0u))
        return MMSYSERR_INVALPARAM;
    g_driver->SubmitSystemExclusive(
        reinterpret_cast<const uint8_t*>(lpMidiHdr->lpData),
        lpMidiHdr->dwBufferLength);
    lpMidiHdr->dwFlags |= MHDR_DONE;
    lpMidiHdr->dwFlags &= ~MHDR_INQUEUE;
    return MMSYSERR_NOERROR;
}

UINT WINAPI SendDirectLongDataNoBuf(LPSTR data, DWORD size) {
    if (!g_kdmapiInitialized.load(std::memory_order_acquire) || !g_driver ||
        (!data && size != 0u))
        return MMSYSERR_INVALPARAM;
    g_driver->SubmitSystemExclusive(
        reinterpret_cast<const uint8_t*>(data), size);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI PrepareLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr))
        return MMSYSERR_INVALPARAM;
    lpMidiHdr->dwFlags |= MHDR_PREPARED;
    lpMidiHdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI UnprepareLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    if (!HasMidiOutHeaderFields(lpMidiHdr, cbMidiHdr))
        return MMSYSERR_INVALPARAM;
    if ((lpMidiHdr->dwFlags & MHDR_INQUEUE) != 0u)
        return MIDIERR_STILLPLAYING;
    lpMidiHdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

UINT WINAPI DriverSettings(DWORD dwSetting, LPVOID pValue, DWORD dwSize, LPVOID pOut) {
    (void)dwSetting; (void)pValue; (void)dwSize; (void)pOut;
    return 1;
}

svms::LegacyDriverDebugInfo* WINAPI GetDriverDebugInfo(void) {
    if (!g_driver) return nullptr;
    return const_cast<svms::LegacyDriverDebugInfo*>(
        g_driver->GetLegacyDebugInfo());
}

FLOAT WINAPI GetRenderingTime(void) {
    return g_driver ? g_driver->GetRenderingTimeMilliseconds() : 0.0f;
}

DWORD WINAPI GetVoiceCount(void) {
    if (!g_driver) return 0u;
    svms::SnappyVoiceStatistics statistics;
    g_driver->CopyVoiceStatistics(statistics);
    return statistics.activeVoices;
}

svms::SnappyVoiceStatistics* WINAPI GetVoiceStatistics(
        svms::SnappyVoiceStatistics* statistics) {
    if (!statistics) return nullptr;
    if (g_driver) {
        g_driver->CopyVoiceStatistics(*statistics);
    } else {
        *statistics = svms::SnappyVoiceStatistics{};
    }
    return statistics;
}

DWORD WINAPI SVMSGetDriverDebugInfoV1(LPVOID pMem, DWORD cbMem) {
    if (!pMem || cbMem < sizeof(svms::DriverDebugInfo) || !g_driver) return 0;
    svms::DriverDebugInfo info;
    g_driver->CopyDebugInfo(info);
    std::memcpy(pMem, &info, sizeof(info));
    return static_cast<DWORD>(sizeof(info));
}

BOOL WINAPI LoadCustomSoundFontsList(LPVOID pList) {
    (void)pList;
    return TRUE;
}

ULONGLONG WINAPI timeGetTime64(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (ULONGLONG)(cnt.QuadPart * 1000000ULL / freq.QuadPart);
}

// ── MIDI Input ────────────────────────────────────────────────────────

UINT WINAPI midiInGetNumDevs(void) {
    using Proc = UINT (WINAPI*)(void);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInGetNumDevs"));
    return proc ? proc() : 0u;
}

MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR uDeviceID, LPMIDIINCAPSA lpCaps, UINT cbCaps) {
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPMIDIINCAPSA, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInGetDevCapsA"));
    return proc ? proc(uDeviceID, lpCaps, cbCaps) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR uDeviceID, LPMIDIINCAPSW lpCaps, UINT cbCaps) {
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPMIDIINCAPSW, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInGetDevCapsW"));
    return proc ? proc(uDeviceID, lpCaps, cbCaps) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInOpen(LPHMIDIIN phmi, UINT uDeviceID, DWORD_PTR dwCallback,
                           DWORD_PTR dwInstance, DWORD fdwOpen) {
    using Proc = MMRESULT (WINAPI*)(LPHMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInOpen"));
    return proc ? proc(phmi, uDeviceID, dwCallback, dwInstance, fdwOpen)
                : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInClose(HMIDIIN hmi) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInClose"));
    return proc ? proc(hmi) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInPrepareHeader(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInPrepareHeader"));
    return proc ? proc(hmi, lpMidiHdr, cbMidiHdr) : MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInUnprepareHeader(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInUnprepareHeader"));
    return proc ? proc(hmi, lpMidiHdr, cbMidiHdr) : MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInAddBuffer(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN, LPMIDIHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInAddBuffer"));
    return proc ? proc(hmi, lpMidiHdr, cbMidiHdr) : MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInStart(HMIDIIN hmi) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInStart"));
    return proc ? proc(hmi) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInStop(HMIDIIN hmi) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInStop"));
    return proc ? proc(hmi) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInReset(HMIDIIN hmi) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInReset"));
    return proc ? proc(hmi) : MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInMessage(HMIDIIN hmi, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    using Proc = MMRESULT (WINAPI*)(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR);
    Proc proc = reinterpret_cast<Proc>(GetSystemWinmmProc("midiInMessage"));
    return proc ? proc(hmi, uMsg, dw1, dw2) : MMSYSERR_ERROR;
}

// ── Wave Input ─────────────────────────────────────────────────────────

UINT WINAPI waveInGetNumDevs(void) { return 0; }

MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR uDeviceID, LPWAVEINCAPSA lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR uDeviceID, LPWAVEINCAPSW lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInOpen(LPHWAVEIN phwi, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                           DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)phwi; (void)uDeviceID; (void)pwfx; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInClose(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInPrepareHeader(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInAddBuffer(HWAVEIN hwi, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
    (void)hwi; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInStart(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInStop(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInReset(HWAVEIN hwi) {
    (void)hwi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveInMessage(HWAVEIN hwi, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hwi; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveInGetPosition(HWAVEIN hwi, LPMMTIME pmmt, UINT cbmmt) {
    (void)hwi; (void)pmmt; (void)cbmmt;
    return MMSYSERR_ERROR;
}

// ── Mixer ──────────────────────────────────────────────────────────────

UINT WINAPI mixerGetNumDevs(void) {
#if defined(SVMS_XP_COMPAT)
    using Proc = UINT (WINAPI*)(void);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetNumDevs"));
    if (proc) return proc();
#endif
    return 0;
}

MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR uMxId, LPMIXERCAPSA lpCaps, UINT cbCaps) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPMIXERCAPSA, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetDevCapsA"));
    if (proc) return proc(uMxId, lpCaps, cbCaps);
#endif
    (void)uMxId; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR uMxId, LPMIXERCAPSW lpCaps, UINT cbCaps) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPMIXERCAPSW, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetDevCapsW"));
    if (proc) return proc(uMxId, lpCaps, cbCaps);
#endif
    (void)uMxId; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerOpen(LPHMIXER phmx, UINT uMxId, DWORD_PTR dwCallback,
                          DWORD_PTR dwInstance, DWORD fdwOpen) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(LPHMIXER, UINT, DWORD_PTR, DWORD_PTR, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerOpen"));
    if (proc) return proc(phmx, uMxId, dwCallback, dwInstance, fdwOpen);
#endif
    (void)phmx; (void)uMxId; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI mixerClose(HMIXER hmx) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXER);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerClose"));
    if (proc) return proc(hmx);
#endif
    (void)hmx;
    return MMSYSERR_BADDEVICEID;
}

DWORD WINAPI mixerMessage(HMIXER hmx, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
#if defined(SVMS_XP_COMPAT)
    using Proc = DWORD (WINAPI*)(HMIXER, UINT, DWORD_PTR, DWORD_PTR);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerMessage"));
    if (proc) return proc(hmx, uMsg, dw1, dw2);
#endif
    (void)hmx; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineInfoA(HMIXEROBJ hmxobj, LPMIXERLINEA pmxl, DWORD fdwInfo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERLINEA, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetLineInfoA"));
    if (proc) return proc(hmxobj, pmxl, fdwInfo);
#endif
    (void)hmxobj; (void)pmxl; (void)fdwInfo;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineInfoW(HMIXEROBJ hmxobj, LPMIXERLINEW pmxl, DWORD fdwInfo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERLINEW, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetLineInfoW"));
    if (proc) return proc(hmxobj, pmxl, fdwInfo);
#endif
    (void)hmxobj; (void)pmxl; (void)fdwInfo;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetID(HMIXEROBJ hmxobj, LPUINT puMxId, DWORD fdwId) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPUINT, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetID"));
    if (proc) return proc(hmxobj, puMxId, fdwId);
#endif
    (void)hmxobj; (void)puMxId; (void)fdwId;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineControlsA(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSA pmxlc, DWORD fdwControls) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERLINECONTROLSA, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetLineControlsA"));
    if (proc) return proc(hmxobj, pmxlc, fdwControls);
#endif
    (void)hmxobj; (void)pmxlc; (void)fdwControls;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetLineControlsW(HMIXEROBJ hmxobj, LPMIXERLINECONTROLSW pmxlc, DWORD fdwControls) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERLINECONTROLSW, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetLineControlsW"));
    if (proc) return proc(hmxobj, pmxlc, fdwControls);
#endif
    (void)hmxobj; (void)pmxlc; (void)fdwControls;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetControlDetailsA(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetControlDetailsA"));
    if (proc) return proc(hmxobj, pmxcd, fdwDetails);
#endif
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerGetControlDetailsW(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerGetControlDetailsW"));
    if (proc) return proc(hmxobj, pmxcd, fdwDetails);
#endif
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI mixerSetControlDetails(HMIXEROBJ hmxobj, LPMIXERCONTROLDETAILS pmxcd, DWORD fdwDetails) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("mixerSetControlDetails"));
    if (proc) return proc(hmxobj, pmxcd, fdwDetails);
#endif
    (void)hmxobj; (void)pmxcd; (void)fdwDetails;
    return MMSYSERR_ERROR;
}

// ── Wave Output ────────────────────────────────────────────────────────

UINT WINAPI waveOutGetNumDevs(void) {
#if defined(SVMS_XP_COMPAT)
    using Proc = UINT (WINAPI*)(void);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetNumDevs"));
    if (proc) {
        const UINT count = proc();
        char message[256] = {};
        std::snprintf(message, sizeof(message),
                      "[SVMS XP] forwarded waveOutGetNumDevs proc=%p result=%u\r\n",
                      reinterpret_cast<void*>(proc),
                      static_cast<unsigned>(count));
        OutputDebugStringA(message);
        return count;
    }
#endif
    return 0;
}

MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR uDeviceID, LPWAVEOUTCAPSA lpCaps, UINT cbCaps) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPWAVEOUTCAPSA, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetDevCapsA"));
    if (proc) return proc(uDeviceID, lpCaps, cbCaps);
#endif
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR uDeviceID, LPWAVEOUTCAPSW lpCaps, UINT cbCaps) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(UINT_PTR, LPWAVEOUTCAPSW, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetDevCapsW"));
    if (proc) return proc(uDeviceID, lpCaps, cbCaps);
#endif
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutOpen(LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                            DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX,
                                     DWORD_PTR, DWORD_PTR, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutOpen"));
    if (proc) return proc(phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen);
#endif
    (void)phwo; (void)uDeviceID; (void)pwfx; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutClose(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutClose"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutPrepareHeader"));
    if (proc) return proc(hwo, lpWaveHdr, cbWaveHdr);
#endif
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutUnprepareHeader"));
    if (proc) return proc(hwo, lpWaveHdr, cbWaveHdr);
#endif
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutWrite(HWAVEOUT hwo, LPWAVEHDR lpWaveHdr, UINT cbWaveHdr) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutWrite"));
    if (proc) return proc(hwo, lpWaveHdr, cbWaveHdr);
#endif
    (void)hwo; (void)lpWaveHdr; (void)cbWaveHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutReset(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutReset"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutRestart(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutRestart"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutPause(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutPause"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT hwo) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutBreakLoop"));
    if (proc) return proc(hwo);
#endif
    (void)hwo;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI waveOutGetPosition(HWAVEOUT hwo, LPMMTIME pmmt, UINT cbmmt) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPMMTIME, UINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetPosition"));
    if (proc) return proc(hwo, pmmt, cbmmt);
#endif
    (void)hwo; (void)pmmt; (void)cbmmt;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI waveOutGetVolume(HWAVEOUT hwo, LPDWORD pdwVolume) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPDWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetVolume"));
    if (proc) return proc(hwo, pdwVolume);
#endif
    (void)hwo;
    if (pdwVolume) *pdwVolume = 0xFFFFFFFF;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetVolume(HWAVEOUT hwo, DWORD dwVolume) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, DWORD);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutSetVolume"));
    if (proc) return proc(hwo, dwVolume);
#endif
    (void)hwo; (void)dwVolume;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetPitch(HWAVEOUT hwo, LPDWORD pdwPitch) {
    (void)hwo;
    if (pdwPitch) *pdwPitch = 0x00010000;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetPitch(HWAVEOUT hwo, DWORD dwPitch) {
    (void)hwo; (void)dwPitch;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetPlaybackRate(HWAVEOUT hwo, LPDWORD pdwRate) {
    (void)hwo;
    if (pdwRate) *pdwRate = 0x00010000;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetPlaybackRate(HWAVEOUT hwo, DWORD dwRate) {
    (void)hwo; (void)dwRate;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetID(HWAVEOUT hwo, LPUINT puDeviceID) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, LPUINT);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutGetID"));
    if (proc) return proc(hwo, puDeviceID);
#endif
    (void)hwo;
    if (puDeviceID) *puDeviceID = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT mmrError, LPSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const char* msg = "Unknown error";
    size_t len = std::strlen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len);
    lpText[len] = '\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT mmrError, LPWSTR lpText, UINT cchText) {
    if (!lpText || cchText == 0) return MMSYSERR_INVALPARAM;
    const wchar_t* msg = L"Unknown error";
    size_t len = wcslen(msg);
    if (len >= cchText) len = cchText - 1;
    std::memcpy(lpText, msg, len * sizeof(wchar_t));
    lpText[len] = L'\0';
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutMessage(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
#if defined(SVMS_XP_COMPAT)
    using Proc = MMRESULT (WINAPI*)(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR);
    Proc proc = reinterpret_cast<Proc>(GetXPSystemWinmmProc("waveOutMessage"));
    if (proc) return proc(hwo, uMsg, dw1, dw2);
#endif
    (void)hwo; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
}

// ── Multimedia Timer ───────────────────────────────────────────────────

static LARGE_INTEGER g_timeFreq;
static BOOL g_timeInitialized = FALSE;

DWORD WINAPI timeGetTime(void) {
    if (!g_timeInitialized) {
        QueryPerformanceFrequency(&g_timeFreq);
        g_timeInitialized = TRUE;
    }
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return (DWORD)(cnt.QuadPart * 1000ULL / g_timeFreq.QuadPart);
}

MMRESULT WINAPI timeBeginPeriod(UINT uPeriod) {
    (void)uPeriod;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeEndPeriod(UINT uPeriod) {
    (void)uPeriod;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc) {
    if (!ptc || cbtc < sizeof(TIMECAPS))
        return TIMERR_NOCANDO;
    ptc->wPeriodMin = 1;
    ptc->wPeriodMax = 65535;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeSetEvent(UINT uDelay, UINT uResolution,
                             void (CALLBACK *fptc)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR),
                             DWORD_PTR dwUser, UINT fuEvent) {
    (void)uDelay; (void)uResolution; (void)fptc; (void)dwUser; (void)fuEvent;
    return 0;
}

MMRESULT WINAPI timeKillEvent(UINT uTimerID) {
    (void)uTimerID;
    return TIMERR_NOERROR;
}

// ── DLL Entry ──────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        InitializeCriticalSection(&g_frontendLock);
        XPBootstrapTrace("[SVMS XP] V3 winmm.dll loaded\r\n");
        LogInit();
        LOG("DLL_PROCESS_ATTACH");
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        LOG("DLL_PROCESS_DETACH");
    }
    return TRUE;
}

} // extern "C"
