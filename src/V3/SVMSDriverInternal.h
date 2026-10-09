#pragma once

// Shared declarations for the driver translation units (SVMSDriver*.cpp,
// SVMSFront*.cpp, SVMSDllMain.cpp). Internal to the engine DLLs; not a
// public header.

#include "SVMSTuning.h"
#include <windows.h>

// Audio-callback census lines ([SVMS] sched/flow/pool/planRefuse): compile-
// time default OFF. They were the only recurring debug-stream traffic and
// destabilize DebugView in OmniMIDI's presence; the shim's file channel
// (%TEMP%\svms_bass.log) carries prerender diagnostics instead. Define
// SVMS_AUDIO_CENSUS=1 for a diagnostics build.
#ifndef SVMS_AUDIO_CENSUS
#define SVMS_AUDIO_CENSUS 0
#endif
#if !defined(SVMS_XP_COMPAT)
#include <dbghelp.h>
#endif
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
#include <mutex>
#include <unordered_map>
#include <vector>
#include <intrin.h>
#include <limits>
#include <thread>

#if defined(SVMS_XP_COMPAT)
#include "SVMSAudioOutputDirectSound.h"
#else
#include "SVMSAudioOutput.h"
#if defined(SVMS_WITH_ASIO)
#include "SVMSAudioOutputASIO.h"
#endif
#endif
#include "SVMSVoiceManager.h"
#include "SVMSVoiceFilter.h"
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
#define SVMSAPI_NO_CLIENT_BINDERS 1
#include "SVMSAPI.h"
#include "SVMSEventPages.h"
#include "SVMSEventCompile.h"
#include "SVMSSysEx.h"
#include "SVMSFrameClock.h"
#include "SVMSDiagWindow.h"
#include "SVMSPostFilter.h"
#include "SVMSLimiter.h"
#include "SVMSReverb.h"
#include "SVMSChannelLimiter.h"
#include "SVMSRuntimeLink.h"
#include "SVMSBuildInfo.h"
#include "include/svmsapi.h"
#if !defined(SVMS_XP_COMPAT)
#include "SVMSLiveRecorder.h"
#endif

// ── Logging ────────────────────────────────────────────────────────────

#define SVMS_VERBOSE_LOG 0

inline FILE* g_logFile = nullptr;
inline CRITICAL_SECTION g_logLock;
inline bool g_logInit = false;

inline void LogInit() {
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

inline void Log(const char* fmt, ...) {
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
inline void XPBootstrapTrace(const char* message) {
    OutputDebugStringA(message);
}

// A drop-in winmm.dll must continue forwarding the non-MIDI multimedia API.
// DirectSound and XP audio drivers import these exports by module name, so
// returning placeholder values here makes a perfectly healthy audio device
// disappear inside the host process. Match V1: load the genuine DLL by its
// absolute System32 path and forward into that module.
inline volatile LONG g_xpSystemWinmmState = 0;
inline HMODULE g_xpSystemWinmm = nullptr;

inline void TraceXPModuleA(const char* label, HMODULE module) {
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

inline HMODULE GetXPSystemWinmm() {
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

inline FARPROC GetXPSystemWinmmProc(const char* name) {
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

inline FARPROC GetSystemWinmmProc(const char* name) {
    return GetXPSystemWinmmProc(name);
}
#else
inline void XPBootstrapTrace(const char*) {}

// Hosts may use the local winmm.dll for more than MIDI. Resolve those
// compatibility exports from the genuine system DLL by absolute path so the
// loader cannot hand us this proxy again and recurse back into it.
inline volatile LONG g_systemWinmmState = 0;
inline HMODULE g_systemWinmm = nullptr;

inline HMODULE GetSystemWinmm() {
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

inline FARPROC GetSystemWinmmProc(const char* name) {
    HMODULE module = GetSystemWinmm();
    return module ? GetProcAddress(module, name) : nullptr;
}
#endif

namespace svms {

#if !defined(SVMS_XP_COMPAT)
inline bool UsesXPWaveOut(const AudioOutputBase* output) {
    (void)output;
    return false;
}
#else
inline bool UsesXPWaveOut(const void* output) {
    return output && static_cast<const AudioOutput*>(output)->IsWaveOutFallback();
}
#endif

// WaitOnAddress is available on Windows 8 and newer, but some older Windows
// SDK import libraries do not expose the API-set forwarding symbols. Resolve
// it once during engine initialization so the audio callback never invokes
// the loader. The compatibility fallback merely yields; cancellation is still
// observed on every retry.
using WaitOnAddressProc = BOOL (WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressAllProc = VOID (WINAPI*)(PVOID);
inline WaitOnAddressProc g_waitOnAddress = nullptr;
inline WakeByAddressAllProc g_wakeByAddressAll = nullptr;

inline void ResolveAddressWaitApi() noexcept {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return;
    g_waitOnAddress = reinterpret_cast<WaitOnAddressProc>(
        GetProcAddress(kernel32, "WaitOnAddress"));
    g_wakeByAddressAll = reinterpret_cast<WakeByAddressAllProc>(
        GetProcAddress(kernel32, "WakeByAddressAll"));
}

inline void WaitForAddressChange(std::atomic<uint32_t>& address,
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

inline void WakeAddressWaiters(std::atomic<uint32_t>& address) noexcept {
    if (g_wakeByAddressAll) {
        g_wakeByAddressAll(reinterpret_cast<PVOID>(&address));
    }
}

// ── TSC-anchored QPC clock ─────────────────────────────────────────────────
// SendDirectData / MIDI-input stamp every event with a QPC timestamp, and
// the QueryPerformanceCounter call is a measurable share of producer CPU at
// multi-million-event rates (profiled ~7% of process total). On CPUs with
// an invariant TSC, __rdtsc is a user-mode read of the SAME underlying
// clock: qpc(t) ≈ qpcBase + (tsc - tscBase) * slope. The slope is
// calibrated at startup over a real interval and continuously refreshed by
// the audio callback, which already reads a true QPC once per block; the
// base triple is published under a seqlock so producers read a consistent
// snapshot. Non-invariant CPUs fall back to plain QPC forever.
class TscQpcClock {
public:
    void Initialize() {
        int regs[4]{};
        __cpuid(regs, 0x80000000u);
        if (static_cast<unsigned>(regs[0]) < 0x80000007u) return;
        __cpuid(regs, 0x80000007u);
        if (!(regs[3] & (1u << 8))) return;  // EDX[8]: invariant TSC

        LARGE_INTEGER q0{}, q1{};
        QueryPerformanceCounter(&q0);
        const uint64_t t0 = __rdtsc();
        Sleep(60);
        QueryPerformanceCounter(&q1);
        const uint64_t t1 = __rdtsc();
        if (q1.QuadPart <= q0.QuadPart || t1 <= t0) return;
        const double measured =
            static_cast<double>(q1.QuadPart - q0.QuadPart) /
            static_cast<double>(t1 - t0);
        if (!(measured > 0.0)) return;
        // Publish the first base pair with the measured slope. The sequence
        // starts even, marking the snapshot valid for readers.
        fields_.tscBase = t1;
        fields_.qpcBase = static_cast<uint64_t>(q1.QuadPart);
        fields_.slope = measured;
        seq_.store(2u, std::memory_order_release);
        available_.store(true, std::memory_order_release);
    }

    // Audio thread only: a true QPC was just read for this block; refresh
    // the base pair and smooth the slope so session-long drift stays
    // far below one audio frame.
    void Refresh(uint64_t trueQpc, uint64_t tsc) noexcept {
        if (!available_.load(std::memory_order_relaxed)) return;
        const uint32_t seq = seq_.load(std::memory_order_relaxed);
        const int64_t tscDelta =
            static_cast<int64_t>(tsc) -
            static_cast<int64_t>(fields_.tscBase);
        const int64_t qpcDelta =
            static_cast<int64_t>(trueQpc) -
            static_cast<int64_t>(fields_.qpcBase);
        double slope = fields_.slope;
        if (tscDelta > 0 && qpcDelta > 0) {
            const double instant =
                static_cast<double>(qpcDelta) /
                static_cast<double>(tscDelta);
            if (instant > 0.0 && instant < 1.0) {
                slope = slope * 0.9 + instant * 0.1;
            }
        }
        seq_.store(seq + 1u, std::memory_order_relaxed);  // odd
        fields_.tscBase = tsc;
        fields_.qpcBase = trueQpc;
        fields_.slope = slope;
        seq_.store(seq + 2u, std::memory_order_release);  // even
    }

    // Producer threads. Returns false when unavailable (callers fall back
    // to QueryPerformanceCounter).
    bool Now(uint64_t& outQpc) const noexcept {
        if (!available_.load(std::memory_order_acquire)) return false;
        uint64_t tscBase, qpcBase;
        double slope;
        uint32_t before, after;
        for (;;) {
            before = seq_.load(std::memory_order_acquire);
            if (before & 1u) continue;  // writer in progress
            tscBase = fields_.tscBase;
            qpcBase = fields_.qpcBase;
            slope = fields_.slope;
            after = seq_.load(std::memory_order_acquire);
            if (after == before) break;
        }
        const uint64_t now = __rdtsc();
        if (now <= tscBase) {
            outQpc = qpcBase;
            return true;
        }
        const double delta = static_cast<double>(now - tscBase);
        const int64_t offset = static_cast<int64_t>(delta * slope);
        outQpc = qpcBase + static_cast<uint64_t>(offset);
        return true;
    }

private:
    struct Fields {
        uint64_t tscBase = 0u;
        uint64_t qpcBase = 0u;
        double slope = 0.0;
    };
    Fields fields_{};
    std::atomic<uint32_t> seq_{1u};  // odd = never initialized
    std::atomic<bool> available_{false};
};

inline void PublishTerminationFence(std::atomic<uint64_t>& destination,
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

inline bool FenceSuppresses(uint32_t sequence, uint64_t encodedFence) noexcept {
    return encodedFence != 0u &&
           SequenceAtOrBefore(sequence, static_cast<uint32_t>(encodedFence - 1u));
}

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
#if !defined(SVMS_XP_COMPAT)
    // Public pass-through to the runtime-link command handler so the native
    // API can drive the engine live-control surface (SVMS_CAP_RUNTIME_COMMANDS).
    svms::RLResult ExecuteRuntimeCommand(const svms::RuntimeLinkCommandV2& cmd,
                                         char* resultText);
#endif
    // Fills SVMS_TelemetryV2 from the live engine census (SVMS_CAP_TELEMETRY_V2).
    void CopyTelemetryCensus(SVMS_TelemetryV2* out) const;
    // Per-callback trace (SVMS_CAP_CALLBACK_TRACE). The ring is allocated on
    // the first enable and lives as long as the driver; the audio thread
    // writes one record per callback only while tracing is enabled.
    struct CallbackTraceRing {
        static constexpr uint32_t kCapacity = 8192u;  // power of two
        std::atomic<uint64_t> head{0u};  // records written so far
        SVMS_CallbackTrace records[kCapacity];
    };
    bool EnableCallbackTrace(bool enable);
    uint32_t ReadCallbackTrace(uint64_t& nextIndex, SVMS_CallbackTrace* out,
                               uint32_t capacity) const;
    std::atomic<CallbackTraceRing*> callbackTrace_{nullptr};
    std::atomic<bool> callbackTraceEnabled_{false};
    // Lossless-backpressure waits by submitting threads (slow path only).
    std::atomic<uint64_t> producerWaits_{0u};
    std::atomic<uint64_t> producerWaitQpc_{0u};
    void WaitForProducerSlot(uint32_t observed) noexcept;
    // External synth backend (SVMS-API / KDMAPI / WinMM): load once at init,
    // forward admitted events from the render callback, tear down on
    // shutdown. A failure falls back to the in-process SVMS engine.
    bool InitializeExternalBackend();
    void ShutdownExternalBackend();
    bool OpenSvmsApiBackend(HMODULE module);
    bool OpenKdmapiBackend(HMODULE module);
    bool OpenWinmmBackend(HMODULE module);
    void ExternalBackendReset();
    void ForwardBlockToBackend(const RenderEvent* events, uint32_t count);
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

    // ASIO format changes (driver-initiated buffer size / sample rate
    // changes) are parked here by the notification callback and applied at
    // the top of the next render callback — same thread that owns the mix
    // buffers, so no reallocation races.
    std::atomic<uint32_t> pendingFormatRate_{0};
    std::atomic<uint32_t> pendingFormatFrames_{0};

private:
    Driver();
    ~Driver();

    static void RenderCallback(float* output, uint32_t numFrames, void* userData);
    static void OnAsioFormatChanged(uint32_t sampleRate, uint32_t bufferFrames,
                                    void* userData);
    void ApplyPendingAudioFormat();
#if !defined(SVMS_XP_COMPAT) && defined(SVMS_WITH_ASIO)
    // Object-level ASIO recovery: after repeated reopen failures the output
    // object is retired (never deleted in-place — the parked instance may
    // still have in-flight bufferSwitch) and a brand-new one is constructed
    // on a dedicated thread: fresh COM load, fresh buffers, full reinit.
    // Also the groundwork for live output device switching.
    static void OnAsioRebuildRequested(void* userData);
    void RebuildAudioOutput();
#endif

    // EventDispatcher callback — invoked by RenderScalar at each event's
    // exact integer output frame during RenderBlock.
    static void DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                     void* userData);
    static void DispatchRenderEventBatch(const RenderEvent* events,
                                         uint32_t eventCount,
                                         uint32_t blockCursor, void* userData);

    uint64_t HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity,
                          bool deferLifetimeCounters = false,
                          const NoteLaunchPlanCacheEntry* exactFramePlan = nullptr,
                          uint32_t blockOffset = 0u);
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
    // Producer-side timestamp clock (invariant-TSC hosts; plain QPC
    // otherwise). Refreshed by the audio callback each block.
    TscQpcClock tscClock_;
    // A state event can overtake older note-ons held in another priority
    // lane.  These producer-published fences prevent those stale note-ons
    // from sounding after a reset or per-channel termination controller.
    std::atomic<uint64_t> globalTerminationFence_;
    std::atomic<uint64_t> channelTerminationFence_[kChannelCount];
    // Audio-thread-only: note-ons rejected at dispatch by a termination
    // fence. Never counted before, which made fence-suppressed silence
    // indistinguishable from a dead event stream in telemetry.
    uint64_t fenceSuppressedNoteOns_ = 0u;
    // Opt-in compiler-thread CC collapse: superseded same-(channel,controller)
    // state events are dropped by overwriting the earlier one's slot in the
    // page still being compiled. A published page can never be patched, so
    // each record is tagged with the page-fill epoch it was created in and a
    // record only survives while its page is the one in flight.
    struct CcCollapseRecord {
        uint32_t page = 0u;
        uint32_t offset = 0u;
        uint64_t pageEpoch = 0u;
        bool valid = false;
    };
    std::atomic<bool> ccCollapseEnabled_{false};
    // Opt-in block-granular dispatch: admitted events fire at block start
    // instead of their exact intra-block sample offset.
    std::atomic<bool> blockTimingEnabled_{false};
    // Pending steal-policy switch (UINT32_MAX = none). The policy is
    // structural to the steal index, so the audio thread applies it at a
    // block boundary instead of the config thread doing it mid-render,
    // where it tore launch transactions apart (observed as a wild write in
    // VolatileHeapSiftDown when a reserved-candidate commit raced the
    // index teardown).
    std::atomic<uint32_t> pendingStealPolicy_{UINT32_MAX};
    // Opt-in unbounded render: no wall-time recovery jump, no per-block
    // admission soft cap. The schedule is rendered in exact order at
    // whatever speed the engine manages; WASAPI glitches are accepted.
    std::atomic<bool> unboundedRenderEnabled_{false};
    // External synth backend state (api.backend). externalBackendKind_ is
    // read by the audio thread every callback; the rest is only touched by
    // init/shutdown before audio starts.
    std::atomic<uint32_t> externalBackendKind_{0u};
    SVMSBackendInterface externalBackend_{};
    HMODULE externalBackendModule_ = nullptr;
    bool externalBackendOpen_ = false;
    struct KdmapiBackendFns {
        int (WINAPI* initialize)(void);
        int (WINAPI* terminate)(void);
        void (WINAPI* reset)(void);
        unsigned int (WINAPI* sendNoBuf)(unsigned int);
        unsigned int (WINAPI* send)(unsigned int);
    };
    KdmapiBackendFns kdmapiBackend_{};
    HMODULE kdmapiBackendModule_ = nullptr;
    bool kdmapiBackendOpen_ = false;
    HMIDIOUT winmmBackendOut_ = nullptr;
    HMODULE winmmBackendModule_ = nullptr;  // set by auto-detected winmm dlls
    // System-winmm function pointers cached at backend load (kind 3).
    unsigned int (WINAPI* systemWinmmShortMsg_)(HMIDIOUT, DWORD) = nullptr;
    unsigned int (WINAPI* systemWinmmResetProc_)(HMIDIOUT) = nullptr;
    CcCollapseRecord ccCollapseRecords_[kChannelCount][128]{};
    uint64_t ccPageFillEpoch_ = 0u;   // compiler-thread owned
    uint64_t ccCollapsedCount_ = 0u;  // compiler-thread owned, census only
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

#if defined(SVMS_XP_COMPAT)
    class AudioOutput* audioOutput;   // XP: concrete DirectSound/waveOut class
#else
    AudioOutputBase* audioOutput;
    // Object-level audio output rebuild (ASIO recovery / live switching).
    // audioOutputMutex_ serializes rebuild vs shutdown; the rebuild itself
    // happens on audioRebuildThread_ because the watcher thread that
    // escalated must keep spinning (parked on rebuildRequested_) until the
    // retired object is finally torn down in Shutdown().
    std::mutex audioOutputMutex_;
    std::vector<AudioOutputBase*> retiredOutputs_;
    std::thread audioRebuildThread_;
    std::atomic<bool> audioRebuildActive_{false};
    std::atomic<bool> audioRebuildShutdown_{false};
#endif
    VoiceManager* voiceManager;
    ChannelCache* channelCache;
    RenderScalar* renderScalar;
    SF2Data* soundFontData;
    RuntimeConfigSnapshot* configSnapshot;
    int16_t* sampleDataStore;
    int16_t* hilbertDataStore;
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
    // Per-MIDI-channel limiter (opt-in, default off): 16 stereo bus planes
    // RenderBlock mixes into when enabled, then ChannelLimiterState limits
    // each bus and sums them into leftBuffer/rightBuffer.  Allocated with
    // the mix buffers so the audio thread never allocates.
    float* channelBusPlanes;
    float* channelBusLeftTable[kChannelCount];
    float* channelBusRightTable[kChannelCount];
    uint32_t channelBusCapacity;
    ChannelLimiterState channelLimiter;
    // Bus planes for the per-MIDI-channel limiter; sized to the mix-buffer
    // capacity, allocated outside the audio callback alongside the mix
    // buffers, and rebuilt wherever those are rebuilt.
    bool AllocateChannelBuses(uint32_t capacity);
    void FreeChannelBuses();
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

#if !defined(SVMS_XP_COMPAT)
// Process-local audio→control snapshot.  Audio thread writes (odd/even
// sequence), control thread reads.  Never mapped into shared memory.
inline svms::RuntimeAudioSnapshot g_audioSnapshot;
#endif

inline uint32_t FloatToU32Bits(float value) noexcept {
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float U32BitsToFloat(uint32_t bits) noexcept {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct PreparedSF2Region {
    float basePhaseStep[kNoteCount];
    float bendScale;
    // SF2 vibrato LFO constants resolved once at load time.
    float vibLfoToPitchCents;
    float vibLfoPhaseStep;
    uint32_t vibLfoDelaySamples;
    PreparedVoiceFilter filter;
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
    // Optional analytic companion (Hilbert pair) over sampleData, same
    // layout + 8-element trailing padding. Built at load when the phase-
    // rotation mode can use it; null otherwise.
    int16_t* hilbertData = nullptr;
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

inline uint32_t SoundFontBankCount(const SoundFontBundle* bundle) noexcept {
    if (!bundle) return 0u;
    return bundle->bankCount != 0u ? bundle->bankCount : 1u;
}

inline SoundFontBundle* SoundFontBankAt(const SoundFontBundle* bundle,
                                        uint32_t index) noexcept {
    if (!bundle) return nullptr;
    if (bundle->bankCount == 0u)
        return index == 0u ? const_cast<SoundFontBundle*>(bundle) : nullptr;
    return index < bundle->bankCount ? bundle->banks[index] : nullptr;
}

// Resolve explicit routes first, then walk the immutable stack in priority
// order. CC0 remains the SF2 bank selector; CC32 stays tracked MIDI state.
inline bool ResolveChannelPreset(const SoundFontBundle* bundle,
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

} // namespace svms

// ── Front-end shared state ─────────────────────────────────────────────
inline svms::Driver* g_driver = nullptr;
inline CRITICAL_SECTION g_frontendLock;
inline std::atomic<uint32_t> g_winmmOwners{0u};
inline std::atomic<uint32_t> g_nativeOwners{0u};
inline std::atomic<bool> g_kdmapiInitialized{false};

// MIDIHDR gained trailing fields in WinMM 4.0 and callers can use a
// different structure packing than the proxy.  Submission only needs the
// fields through dwFlags, so accept every ABI that supplies those fields.
inline bool HasMidiOutHeaderFields(const MIDIHDR* header, UINT byteCount) {
    const UINT required = static_cast<UINT>(FIELD_OFFSET(MIDIHDR, dwFlags) +
                                             sizeof(header->dwFlags));
    return header != nullptr && byteCount >= required;
}

// Engine lifetime shared by the WinMM, KDMAPI and native front ends
// (SVMSDllMain.cpp).
bool EnsureDriverInitialized();
void MaybeShutdownDriver();

// Offline-session entry points of the native API (SVMSFrontNative.cpp),
// also driven directly by the BASS shim (SVMSFrontBass.cpp).
extern "C" {
SVMS_Result SVMS_CALL NativeCreateOfflineSession(
    const SVMS_OfflineSessionConfig* config, const char* soundfontPathUtf8,
    SVMS_Session* outSession);
SVMS_Result SVMS_CALL NativeRenderOffline(
    SVMS_Session session, const SVMS_OfflineEvent* events,
    uint32_t eventCount, float* outputLeft, float* outputRight,
    uint32_t frameCount);
SVMS_Result SVMS_CALL NativeGetOfflineTelemetry(
    SVMS_Session session, SVMS_OfflineTelemetry* telemetry);
SVMS_Result SVMS_CALL NativeDestroySession(SVMS_Session session);
} // extern "C"
