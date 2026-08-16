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
#include "SVMSEventScheduler.h"
#include "SVMSEventCompile.h"
#include "SVMSFrameClock.h"
#include "SVMSDiagWindow.h"
#include "SVMSPostFilter.h"
#include "SVMSRuntimeLink.h"

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
#else
static void XPBootstrapTrace(const char*) {}
#endif

namespace svms {

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

            fdnBaseDelayFrames[i] =
                baseFrames;

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
            allpassLength[0][i] =
                MsToFrames(
                    kDiffuserMs[i] *
                    diffuserScale,
                    configuredSampleRate,
                    kAllpassBufferFrames);

            allpassLength[1][i] =
                MsToFrames(
                    (kDiffuserMs[i] +
                     kRightOffsetMs[i]) *
                    diffuserScale,
                    configuredSampleRate,
                    kAllpassBufferFrames);
        }

        allpassFeedback =
            0.22f +
            diffusion *
            0.53f;

        // =============================================================
        // Predelay
        // =============================================================

        if (preDelayMs <= 0.0f) {
            preDelayLength = 0u;
        }
        else {
            preDelayLength =
                MsToFrames(
                    preDelayMs,
                    configuredSampleRate,
                    kPreDelayBufferFrames - 1u);
        }

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
            earlyTapFrames[i][0] =
                MsToFrames(
                    kEarlyMs[i] *
                    earlyScale,
                    configuredSampleRate,
                    kEarlyBufferFrames - 1u);

            earlyTapFrames[i][1] =
                MsToFrames(
                    (kEarlyMs[i] +
                     kEarlyStereoOffsetMs[i]) *
                    earlyScale,
                    configuredSampleRate,
                    kEarlyBufferFrames - 1u);
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
            kInitialPhase[kFdnLines] =
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
            const float initialAngle =
                2.0f *
                kPi *
                kInitialPhase[i];

            lfoSin[i] =
                std::sin(initialAngle);

            lfoCos[i] =
                std::cos(initialAngle);

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

struct LimiterState {
    static constexpr uint32_t kMaxDelayFrames = 8192;

    float delayBuffer[kMaxDelayFrames * 2];
    uint32_t delayWritePos = 0;
    uint32_t delayFrames = 128;
    float envelope = 0.0f;
    float threshold = 0.95f;
    float attackCoeff = 0.25f;
    float releaseCoeff = 0.001f;
    bool enabled = true;

    // Per-block meters, written by Process() on the audio thread and
    // published into g_audioSnapshot by RenderCallback.  gainReductionDb
    // is positive (amount of reduction); peaks are linear sample scale.
    float inputPeakL = 0.0f;
    float inputPeakR = 0.0f;
    float outputPeakL = 0.0f;
    float outputPeakR = 0.0f;
    float gainReductionDb = 0.0f;

    void Reset() {
        std::memset(delayBuffer, 0, sizeof(delayBuffer));
        delayWritePos = 0;
        envelope = 0.0f;
        inputPeakL = inputPeakR = outputPeakL = outputPeakR = 0.0f;
        gainReductionDb = 0.0f;
    }

    void Configure(uint32_t sampleRate, const EngineConfig& cfg) {
        Reset();
        enabled = cfg.limiterEnabled;
        threshold = cfg.limiterThreshold;
        delayFrames = (std::min)(kMaxDelayFrames,
            (std::max)(1u, static_cast<uint32_t>(
                cfg.limiterLookaheadMs * sampleRate * 0.001f + 0.5f)));
        const float attackSamples = (std::max)(1.0f,
            cfg.limiterAttackMs * sampleRate * 0.001f);
        const float releaseSamples = (std::max)(1.0f,
            cfg.limiterReleaseMs * sampleRate * 0.001f);
        attackCoeff = 1.0f - std::exp(-1.0f / attackSamples);
        releaseCoeff = 1.0f - std::exp(-1.0f / releaseSamples);
    }

    void Process(float* interleaved, uint32_t numFrames, uint32_t channels,
                 PostHighPass3Hz& highPass) {
        inputPeakL = inputPeakR = outputPeakL = outputPeakR = 0.0f;
        gainReductionDb = 0.0f;

        if (!enabled) {
            // Bypass: no limiting, so the meters just mirror the
            // passthrough signal (input == output).
            for (uint32_t f = 0; f < numFrames; ++f) {
                const float inL = interleaved[f * channels];
                const float inR = (channels > 1) ? interleaved[f * channels + 1] : inL;
                const float aL = inL > 0.0f ? inL : -inL;
                const float aR = inR > 0.0f ? inR : -inR;
                if (aL > inputPeakL) inputPeakL = aL;
                if (aR > inputPeakR) inputPeakR = aR;
            }
            outputPeakL = inputPeakL;
            outputPeakR = inputPeakR;
            highPass.ProcessInterleavedStereo(interleaved, numFrames);
            return;
        }

        for (uint32_t f = 0; f < numFrames; ++f) {
            float inL = interleaved[f * channels];
            float inR = (channels > 1) ? interleaved[f * channels + 1] : inL;

            float absL = inL > 0 ? inL : -inL;
            float absR = inR > 0 ? inR : -inR;
            float peak = absL > absR ? absL : absR;
            if (absL > inputPeakL) inputPeakL = absL;
            if (absR > inputPeakR) inputPeakR = absR;

            if (peak > envelope) {
                envelope += attackCoeff * (peak - envelope);
            } else {
                envelope += releaseCoeff * (peak - envelope);
            }

            float gain = 1.0f;
            if (envelope > threshold) {
                gain = threshold / envelope;
                const float reduction =
                    -20.0f * std::log10(gain);
                if (reduction > gainReductionDb) gainReductionDb = reduction;
            }

            uint32_t dw = delayWritePos * channels;
            float dL = delayBuffer[dw];
            float dR = delayBuffer[dw + 1];

            delayBuffer[dw] = inL;
            delayBuffer[dw + 1] = inR;

            dL *= gain;
            dR *= gain;

            // Soft-knee saturation is only used above the limiter knee; it
            // avoids the former hard clip that made dense reference renders
            // visibly flatten while still guaranteeing bounded output.
            const float limitThreshold = threshold;
            auto softLimit = [limitThreshold](float x) {
                const float ax = std::fabs(x);
                if (ax <= limitThreshold) return x;
                const float headroom = 1.0f - limitThreshold;
                const float compressed = limitThreshold + headroom *
                    std::tanh((ax - limitThreshold) /
                              (headroom > 0.0001f ? headroom : 0.0001f));
                return x < 0.0f ? -compressed : compressed;
            };
            dL = softLimit(dL);
            dR = softLimit(dR);
            const float oL = dL > 0.0f ? dL : -dL;
            const float oR = dR > 0.0f ? dR : -dR;
            if (oL > outputPeakL) outputPeakL = oL;
            if (oR > outputPeakR) outputPeakR = oR;
            highPass.ProcessStereoSample(dL, dR);

            interleaved[f * channels] = dL;
            if (channels > 1) interleaved[f * channels + 1] = dR;

            delayWritePos = (delayWritePos + 1) % delayFrames;
        }
        highPass.FinishBlock();
    }
};

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
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint8_t count;
    uint16_t reserved;
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

    void HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset);
    void HandleStaleNoteOffBatch(uint8_t channel, uint8_t note, uint8_t count,
                                 uint32_t blockOffset);
    void HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value,
                             uint32_t blockOffset);
    void HandleProgramChange(uint8_t channel, uint8_t program);
    void HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb);
    void EventCompilerLoop();
    uint32_t ResolveNoteRegions(uint32_t presetIndex, uint8_t note,
                                uint8_t velocity,
                                const SFSampleRegion** outRegions,
                                uint32_t outCapacity);
    void RefreshSelectedPresets();

    PriorityEventIngress<TimestampedMidiEvent> midiIngress_;
    DynamicSPSCQueue<ScheduledRenderEvent> compiledIngress_;
    EventScheduler eventScheduler_;
    EventOverflowMode overflowMode_;
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
    float* sampleDataStore;
    SF2Sample* samplesStore;
    float* regionInitialPeaks;
    uint32_t regionInitialPeakCount;
    PreparedSF2Region* preparedRegions;
    uint32_t preparedRegionCount;
    uint32_t soundFontGeneration_;
    uint32_t channelLaunchRevision_[kChannelCount];
    NoteRegionCacheEntry noteRegionCache_[kNoteRegionCacheSize];
    NoteLaunchPlanCacheEntry noteLaunchPlanCache_[kNoteRegionCacheSize];
    const SFSampleRegion* noteRegionScratch_[kMaxMatchingRegions];
    VoiceConfiguration noteLaunchScratch_[kMaxMatchingRegions];
    VoiceHandle noteLaunchHandles_[kMaxMatchingRegions];
    float configuredVelocityGain_[128];
    float channelPitchBendRatio_[kChannelCount];
    uint32_t sampleStoreCount;
    uint32_t sampleDataFrames;
    uint64_t qpcFreq;

    float* leftBuffer;
    float* rightBuffer;
    uint32_t bufferCapacity;
    PostHighPass3Hz postHighPass;
    ReverbState reverb;
    LimiterState limiter;

    // ── Atomic live-config mailbox (seqlock) ─────────────────────────
    // The control thread is the ONLY writer.  It bumps liveMailboxSeq_
    // to ODD, writes the mailbox fields, then bumps to EVEN (release).
    // The audio thread reads once per render block: if the sequence is
    // even and unchanged after the copy, the copy is torn-free; otherwise
    // it falls back to liveMailboxApplied_ (the last stable read).  No
    // locks, no torn reads, no ABA (single writer, monotonically even
    // sequence values 2, 4, 6, ...).
    svms::LiveConfigMailbox liveMailbox_;
    std::atomic<uint32_t> liveMailboxSeq_{2u};
    svms::LiveConfigMailbox liveMailboxApplied_;

    // Last master volume the audio thread actually folded into playing
    // voices' mix gains.  Audio-thread only.
    float appliedMasterVolume_ = 1.0f;

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
    bool clockInitialized;
    uint32_t nextPlayIndex_;
    EngineConfig engineConfig_;

    #if !defined(SVMS_XP_COMPAT)
    svms::RLResult HandleRuntimeLinkCommand(const svms::RuntimeLinkCommandV2& cmd,
                                  char* resultText);
    svms::RuntimeLinkTelemetryV2 BuildRuntimeLinkTelemetry();
#endif

    // Path of the last successfully loaded SoundFont.  Written by the
    // host/control thread under LoadSoundFont(); read by the control
    // thread for telemetry.  Never touched by the audio thread.
    wchar_t loadedSoundFontPath_[MAX_PATH] = {};

    uint32_t lastReleasingVoices_ = 0;
    uint32_t releasingScanTick_ = 0;

    // Control-thread-owned echo of the live parameters the audio thread
    // is applying (via the mailbox).  Published in RuntimeLink telemetry.
#if !defined(SVMS_XP_COMPAT)
    svms::RuntimeLiveStateV2 appliedLive_;
#endif

    CRITICAL_SECTION cs;
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
// no torn reads).  Heavy commands (ReloadSoundFont) run here, never on
// the audio thread; ResetVoices routes through the SPSC ingress exactly
// like midiOutReset so the audio thread performs the release work.
#if !defined(SVMS_XP_COMPAT)
static svms::RuntimeLiveStateV2 LiveStateFromMailbox(
    const LiveConfigMailbox& mb, uint32_t sampleRate) {
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
    l.limiterThreshold = mb.limiterThreshold;
    l.limiterLookaheadMs = static_cast<float>(mb.limiterDelayFrames)
                         / sampleRate * 1000.0f;
    l.limiterAttackMs = mb.limiterAttackCoeff > 0.0f
        ? -1000.0f / (sampleRate * std::log(1.0f - mb.limiterAttackCoeff))
        : 0.01f;
    l.limiterReleaseMs = mb.limiterReleaseCoeff > 0.0f
        ? -1000.0f / (sampleRate * std::log(1.0f - mb.limiterReleaseCoeff))
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
            mb->limiterThreshold = (std::max)(0.1f, (std::min)(1.0f, l.limiterThreshold));
            uint32_t frames = static_cast<uint32_t>(
                (std::max)(0.0f, (std::min)(20.0f, l.limiterLookaheadMs))
                * sampleRate * 0.001f + 0.5f);
            mb->limiterDelayFrames = (std::max)(1u,
                (std::min)(svms::LimiterState::kMaxDelayFrames, frames));
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
        appliedLive_ = LiveStateFromMailbox(*mb, sampleRate);
        return svms::RLResult::Ok;
    }

    case RT::ReloadSoundFont: {
        // Transactional reload: resolve the configured SoundFont, parse
        // it (control thread — never the audio thread), and swap while
        // the audio device is stopped.  A failed parse leaves the
        // existing runtime untouched.
        std::string resolutionWarning;
        const std::wstring widePath =
            ResolveV3SoundFontPath(engineConfig_, &resolutionWarning);
        if (widePath.empty()) {
            strncpy_s(resultText, kText, "no SoundFont configured", _TRUNCATE);
            if (!resolutionWarning.empty()) {
                strncpy_s(resultText, kText, resolutionWarning.c_str(), _TRUNCATE);
            }
            return svms::RLResult::LoadFailed;
        }
        if (!LoadSoundFont(widePath.c_str())) {
            strncpy_s(resultText, kText,
                      "SoundFont parse failed; previous SoundFont kept",
                      _TRUNCATE);
            return svms::RLResult::LoadFailed;
        }
        return svms::RLResult::Ok;
    }

    case RT::ResetVoices:
        // Routes through the SPSC ingress when audio is running, so the
        // release work happens on the audio thread exactly like
        // midiOutReset.
        ResetAllVoices();
        return svms::RLResult::Ok;

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

    constexpr uint32_t kStamp = 1u;
    const svms::RuntimeAudioSnapshot& as = g_audioSnapshot;
    const bool settled = as.sequence == svms::RLV2_SnapshotSettledSequence;
    if (settled) {
        float f = 0.0f;
        if (as.activeVoices.TryLoad(f, kStamp)) snap.activeVoices = static_cast<uint32_t>(f);
        if (as.releasingVoices.TryLoad(f, kStamp)) snap.releasingVoices = static_cast<uint32_t>(f);
        if (as.freeTop.TryLoad(f, kStamp)) snap.freeTop = static_cast<uint32_t>(f);
        if (as.voiceSteals.TryLoad(f, kStamp)) snap.voiceSteals = static_cast<uint32_t>(f);
        if (as.retiredCount.TryLoad(f, kStamp)) snap.retiredCount = static_cast<uint32_t>(f);
        if (as.retiredImmediateCount.TryLoad(f, kStamp)) snap.retiredImmediateCount = static_cast<uint32_t>(f);
        if (as.decimationStep.TryLoad(f, kStamp)) snap.decimationStep = static_cast<uint32_t>(f);
        if (as.renderPeak.TryLoad(f, kStamp)) snap.renderPeak = f;
        if (as.audioRunning.TryLoad(f, kStamp)) snap.audioRunning = static_cast<uint32_t>(f);
        if (as.soundFontLoaded.TryLoad(f, kStamp)) snap.soundFontLoaded = static_cast<uint32_t>(f);
        if (as.audioHResult.TryLoad(f, kStamp)) snap.audioHResult = static_cast<int32_t>(f);
        if (as.cpuLoadPercent.TryLoad(f, kStamp)) snap.cpuLoadPercent = f;
        if (as.callbackP95Percent.TryLoad(f, kStamp)) snap.callbackP95Percent = f;
        if (as.callbackP99Percent.TryLoad(f, kStamp)) snap.callbackP99Percent = f;
        if (as.callbackP999Percent.TryLoad(f, kStamp)) snap.callbackP999Percent = f;
        if (as.maxConsecutiveOverBudget.TryLoad(f, kStamp)) snap.maxConsecutiveOverBudget = static_cast<uint32_t>(f);
        if (as.limiterInputPeakL.TryLoad(f, kStamp)) snap.limiterInputPeakL = f;
        if (as.limiterInputPeakR.TryLoad(f, kStamp)) snap.limiterInputPeakR = f;
        if (as.limiterOutputPeakL.TryLoad(f, kStamp)) snap.limiterOutputPeakL = f;
        if (as.limiterOutputPeakR.TryLoad(f, kStamp)) snap.limiterOutputPeakR = f;
        if (as.limiterGainReductionDb.TryLoad(f, kStamp)) snap.limiterGainReductionDb = f;
        snap.overBudgetCallbacks = as.overBudgetCallbacks;
        snap.eventsSubmitted = as.eventsSubmitted;
        snap.eventsAccepted = as.eventsAccepted;
        snap.eventsDropped = as.eventsDropped;
        snap.eventsDispatched = as.eventsDispatched;

        // Re-verify the settle marker: a writer that started mid-copy
        // (sequence flipped back to 1) means this frame may be torn —
        // reuse the last stable publish instead (same skip-if-busy
        // pattern as the client side).
        RLV2_MemBarrier();
        if (as.sequence != svms::RLV2_SnapshotSettledSequence) return s_lastBuilt;
        s_lastBuilt = snap;
    } else {
        return s_lastBuilt;
    }

    snap.maxVoices = voiceManager ? voiceManager->GetMaxVoices() : 0u;
    snap.sampleRate = sampleRate;
    snap.bufferFrames = bufferFrames;

    // Live-state echo: what the control thread last published to the
    // mailbox (i.e. what the audio thread is applying).
    snap.live = appliedLive_;

    if (loadedSoundFontPath_[0] != L'\0') {
        std::string narrow = wideToUtf8(loadedSoundFontPath_);
        strncpy_s(snap.soundFontName, sizeof(snap.soundFontName),
                  narrow.c_str(), _TRUNCATE);
    }

    return snap;
}
#endif // !defined(SVMS_XP_COMPAT)

// Resolve a channel's active preset using the same bank/program rules as TSF:
// normal channels use their selected bank and program; MIDI channel 10 first
// searches the percussion bank (128 | bank), then the standard percussion
// fallbacks.  The caller only commits a successful result, so an invalid
// program change leaves the previously selected preset untouched.
static bool ResolveChannelPreset(const SF2Data* data, const ChannelCache& cache,
                                 uint8_t channel, uint32_t* outPresetIndex) {
    if (!data || !outPresetIndex || channel >= kChannelCount) return false;
    return sf2_resolve_preset(data, cache.GetBank(channel), cache.GetProgram(channel),
                              channel == 9, outPresetIndex);
}

struct PreparedSF2Region {
    float basePhaseStep[kNoteCount];
    float bendScale;
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
}

Driver::~Driver() {
    Shutdown();
    if (eventBuffer) { _aligned_free(eventBuffer); eventBuffer = nullptr; }
    DeleteCriticalSection(&cs);
}

bool Driver::Initialize() {
    if (initialized) return true;

    ResolveAddressWaitApi();
    midiIngress_.DrainAvailable();
    compiledIngress_.Reset();
    eventScheduler_.Reset();
    compilerEpochQPC_.store(0u, std::memory_order_relaxed);
    globalTerminationFence_.store(0, std::memory_order_relaxed);
    for (auto& fence : channelTerminationFence_) fence.store(0, std::memory_order_relaxed);
    virtualRenderClockQPC = 0;
    virtualRenderSample_ = 0;
    clockInitialized = false;
    callbackTiming_.Reset();

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFreq = freq.QuadPart;

    EngineConfig cfg = EngineConfig::Load();
    if (!cfg.configWarning.empty()) {
        std::string warning = "[SVMS] configuration warning: " +
                              cfg.configWarning + "\n";
        OutputDebugStringA(warning.c_str());
    }
    if (!cfg.Validate()) { LOG("EngineConfig validation failed"); return false; }

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
        if (!compiledIngress_.ConfigureCapacity(ringCapacity)) return false;
        try {
            eventScheduler_.ConfigureCapacity(ringCapacity);
        } catch (...) {
            return false;
        }
        const uint32_t actualBlockCapacity =
            (std::min)(blockCapacity, ringCapacity);
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

    voiceManager = new VoiceManager();
    if (!voiceManager->Initialize(cfg.maxVoices, sampleRate)) {
        LOG("FAILED: Could not allocate voice storage maxVoices=%u",
            cfg.maxVoices);
        return false;
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
    if (!renderScalar->ReserveVoiceCapacity(cfg.maxVoices)) {
        LOG("FAILED: Could not allocate renderer scratch maxVoices=%u",
            cfg.maxVoices);
        return false;
    }
    uint32_t renderThreads = cfg.renderThreads;
    if (renderThreads == 0u) {
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        renderThreads = (std::max)(1u, (std::min)(8u,
            static_cast<uint32_t>(systemInfo.dwNumberOfProcessors)));
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
    liveMailboxApplied_ = liveMailbox_;
    liveMailboxSeq_.store(2u, std::memory_order_release);
    appliedMasterVolume_ = cfg.masterVolume;
#if !defined(SVMS_XP_COMPAT)
    appliedLive_ = LiveStateFromMailbox(liveMailbox_, sampleRate);
#endif

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
        LOG("RuntimeLink V2 initialized: PID=%u session=%016llX",
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
#endif

    cancelProducers_.store(true, std::memory_order_release);
    producerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(producerWakeEpoch_);
    compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(compilerWakeEpoch_);
    compilerSleeping_.store(false, std::memory_order_release);
    if (audioOutput) {
        audioOutput->Stop();
        audioOutput->Shutdown();
        delete audioOutput;
        audioOutput = nullptr;
    }
    if (eventCompilerThread_.joinable()) eventCompilerThread_.join();
    useEventCompiler_ = false;
    midiIngress_.DrainAvailable();
    compiledIngress_.Reset();
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
    delete configSnapshot; configSnapshot = nullptr;
    free(regionInitialPeaks); regionInitialPeaks = nullptr;
    regionInitialPeakCount = 0;
    free(preparedRegions); preparedRegions = nullptr;
    preparedRegionCount = 0;
    _aligned_free(leftBuffer); leftBuffer = nullptr;
    _aligned_free(rightBuffer); rightBuffer = nullptr;
    bufferCapacity = 0;

    if (soundFontData) {
        bool ownsResampled = (sampleDataStore == soundFontData->resampledData);
        bool ownsRawSamples = (sampleDataStore && !ownsResampled);
        if (ownsRawSamples && sampleDataStore != (void*)(soundFontData->sampleData)) {
            free(sampleDataStore);
        }
        sampleDataStore = nullptr;
        soundFontData->resampledData = nullptr;
        free(samplesStore); samplesStore = nullptr;
        sf2_free(soundFontData);
        delete soundFontData;
        soundFontData = nullptr;
    } else {
        free(sampleDataStore); sampleDataStore = nullptr;
        free(samplesStore); samplesStore = nullptr;
    }

    if (diagnosticsEnabled_ && (diagnosticsWindow_ || diagnosticsDebugOutput_))
        DiagWindow_Destroy();

    initialized = false;
}

bool Driver::LoadConfiguredSoundFont() {
    std::string resolutionWarning;
    const std::wstring widePath =
        ResolveV3SoundFontPath(engineConfig_, &resolutionWarning);
    if (!resolutionWarning.empty()) {
        const std::string message =
            "[SVMS] SoundFont configuration warning: " +
            resolutionWarning + "\n";
        OutputDebugStringA(message.c_str());
    }
    const bool loaded = !widePath.empty() && LoadSoundFont(widePath.c_str());
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

bool Driver::LoadSoundFont(const wchar_t* path) {
    if (!path) return false;

    // Parsing and sample conversion happen on the calling thread. If a host
    // requests a reload while audio is active, stop and join first so the
    // callback can never observe partially replaced SF2/sample storage.
    const bool restartAudio = audioOutput && audioOutput->IsRunning();
    if (restartAudio) audioOutput->Stop();
    struct RestartAudioGuard {
        AudioOutput* output;
        bool restart;
        ~RestartAudioGuard() { if (restart && output) output->Start(); }
    } restartGuard{audioOutput, restartAudio};

    EnterCriticalSection(&cs);

    SF2Data* sf2 = new SF2Data();
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
        delete sf2;
        LeaveCriticalSection(&cs);
        return false;
    }
    LOG("  Parsed: %u presets, %u instruments, %u samples, %u frames",
        sf2->presetCount, sf2->instrumentCount, sf2->sampleCount, sf2->sampleDataFrames);

    sf2_build_regions(sf2);
    LOG("  Built %u regions", sf2->regionCount);
    if (sf2->regionOverflow) {
        LOG("  FAILED: SoundFont compiled region capacity exceeded");
        sf2_free(sf2);
        delete sf2;
        LeaveCriticalSection(&cs);
        return false;
    }

    if (soundFontData) {
        free(regionInitialPeaks); regionInitialPeaks = nullptr;
        regionInitialPeakCount = 0;
        free(preparedRegions); preparedRegions = nullptr;
        preparedRegionCount = 0;
        free(sampleDataStore); sampleDataStore = nullptr;
        free(samplesStore); samplesStore = nullptr;
        sf2_free(soundFontData);
        delete soundFontData;
    }
    soundFontData = sf2;
    if (++soundFontGeneration_ == 0u) {
        soundFontGeneration_ = 1u;
        std::memset(noteLaunchPlanCache_, 0,
                    sizeof(noteLaunchPlanCache_));
    }
    // Region pointers are immutable for one SoundFont bundle.  Reset the
    // direct-mapped note lookup cache at the swap boundary so callback-side
    // note-ons can never observe indices from the retired bundle.
    std::memset(noteRegionCache_, 0xff, sizeof(noteRegionCache_));

    // Diagnostic peak inspection walks up to 512 source samples.  Doing
    // that for every configured voice made diagnostics catastrophically
    // expensive in dense MIDI.  Compile the immutable values once while
    // loading off the audio thread; note-on becomes a single cached read.
    if (sf2->regionCount != 0u) {
        regionInitialPeaks = static_cast<float*>(
            malloc(static_cast<size_t>(sf2->regionCount) * sizeof(float)));
        if (regionInitialPeaks) {
            regionInitialPeakCount = sf2->regionCount;
            for (uint32_t region = 0; region < sf2->regionCount; ++region) {
                regionInitialPeaks[region] =
                    sf2_region_initial_peak(sf2, &sf2->regions[region]);
            }
        }
        preparedRegions = static_cast<PreparedSF2Region*>(malloc(
            static_cast<size_t>(sf2->regionCount) * sizeof(PreparedSF2Region)));
        if (preparedRegions) {
            preparedRegionCount = sf2->regionCount;
            for (uint32_t region = 0; region < sf2->regionCount; ++region) {
                PrepareSF2Region(sf2, sf2->regions[region], sampleRate,
                                 channelCache, preparedRegions[region]);
            }
        }
    }

    // Establish the initial selected preset for every channel.  Future
    // invalid program changes do not overwrite this committed selection.
    if (channelCache) {
        for (uint8_t ch = 0; ch < kChannelCount; ++ch) {
            uint32_t presetIndex = 0;
            if (ResolveChannelPreset(soundFontData, *channelCache, ch, &presetIndex)) {
                channelCache->SetSelectedPreset(ch, static_cast<uint16_t>(presetIndex));
            } else {
                channelCache->SetSelectedPreset(ch, UINT16_MAX);
            }
        }
    }

    if (sf2->sampleData) {
        uint32_t frames = sf2->sampleDataFrames;
        float* fbuf = static_cast<float*>(malloc(frames * sizeof(float)));
        if (fbuf) {
            for (uint32_t i = 0; i < frames; ++i) {
                fbuf[i] = sf2->sampleData[i] / 32768.0f;
            }
            sampleDataStore = fbuf;
        }
        sampleDataFrames = frames;
    }

    uint32_t sampCount = sf2->sampleCount;
    SF2Sample* sampBuf = static_cast<SF2Sample*>(malloc(sampCount * sizeof(SF2Sample)));
    if (sampBuf) {
        std::memcpy(sampBuf, sf2->samples, sampCount * sizeof(SF2Sample));
    }
    samplesStore = sampBuf;
    sampleStoreCount = sampCount;

    LOG("  LoadSoundFont SUCCESS: %u samples cached", sampCount);
    wcsncpy_s(loadedSoundFontPath_, MAX_PATH, path, _TRUNCATE);
    LeaveCriticalSection(&cs);
    return true;
}

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
        return ok;
    }
    return audioOutput && audioOutput->IsRunning();
}

void Driver::ResetAllVoices() {
    if (audioOutput && audioOutput->IsRunning()) {
        SubmitShortMsg(kInternalResetMessage);
        return;
    }
    if (voiceManager) voiceManager->Reset();
    if (channelCache) channelCache->Reset();
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
    submittedAtomic_.fetch_add(1, std::memory_order_relaxed);
    TimestampedMidiEvent evt{};
    evt.message = msg;
    evt.sequence = nextEventSequence_.fetch_add(1, std::memory_order_relaxed);
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&evt.qpcTimestamp));

    const uint8_t status = static_cast<uint8_t>(msg & 0xffu);
    const uint8_t data1 = static_cast<uint8_t>((msg >> 8) & 0x7fu);
    const uint8_t velocity = static_cast<uint8_t>((msg >> 16) & 0x7fu);
    const bool noteOn = msg != kInternalResetMessage &&
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
        const uint32_t rawIngress = midiIngress_.TotalSize();
        const uint32_t compiledIngress = compiledIngress_.Size();
        const uint32_t scheduled =
            scheduledSizePublished_.load(std::memory_order_acquire);
        const uint32_t rawIngressPressure = static_cast<uint32_t>(
            static_cast<uint64_t>(rawIngress) * 100u /
            PriorityEventIngress<TimestampedMidiEvent>::TotalCapacity());
        const uint32_t compiledCapacity = compiledIngress_.CapacityValue();
        const uint32_t compiledIngressPressure = compiledCapacity != 0u
            ? static_cast<uint32_t>(
                  static_cast<uint64_t>(compiledIngress) * 100u /
                  compiledCapacity)
            : 100u;
        const uint32_t laneCapacity =
            PriorityEventIngress<TimestampedMidiEvent>::LaneCapacity(lane);
        const uint32_t lanePressure = static_cast<uint32_t>(
            static_cast<uint64_t>(midiIngress_.LaneSize(lane)) * 100u /
            laneCapacity);
        const uint32_t scheduledCapacity = eventScheduler_.Capacity();
        const uint32_t scheduledPressure = scheduledCapacity != 0u
            ? static_cast<uint32_t>(
                  static_cast<uint64_t>(scheduled) * 100u /
                  scheduledCapacity)
            : 100u;
        const uint32_t pressure = (std::max)(
            lanePressure,
            (std::max)(rawIngressPressure,
                (std::max)(compiledIngressPressure, scheduledPressure)));
        cutoff = 1u;
        if (pressure > shedStartPercent_) {
            const uint32_t range = 100u - shedStartPercent_;
            cutoff = 1u +
                ((std::min)(pressure - shedStartPercent_, range) * 94u) /
                    range;
        }
        currentVelocityCutoffAtomic_.store(cutoff,
                                            std::memory_order_relaxed);
    }

    if (noteOn && velocity < cutoff &&
        overflowMode_ == EventOverflowMode::PriorityVelocity) {
        shedAtomic_.fetch_add(1, std::memory_order_relaxed);
        shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const bool lossless = !noteOn || velocity >= highPriorityVelocity_ ||
                          overflowMode_ == EventOverflowMode::LosslessBackpressure;
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
            return;
        }
        if (!lossless) {
            shedAtomic_.fetch_add(1, std::memory_order_relaxed);
            shedByVelocityAtomic_[velocity].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (cancelProducers_.load(std::memory_order_acquire)) {
            cancelledAtomic_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        uint32_t observed = producerWakeEpoch_.load(std::memory_order_acquire);
        WaitForAddressChange(producerWakeEpoch_, observed);
    }
}

void Driver::EventCompilerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // Small enough to stay cache-friendly, large enough that one 2048-frame
    // Black-MIDI callback arrives as only a handful of ordered runs.
    static constexpr uint32_t kCompilerChunkCapacity = 8192u;
    static constexpr uint32_t kCompilerLaneRunQuota =
        (kCompilerChunkCapacity + 4u) / 5u;
    EventScheduler chunkScheduler(kCompilerChunkCapacity);
    uint32_t fairLaneCursor = 0u;

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
        if (!midiIngress_.TryPopFair(timed, fairLaneCursor)) {
            compilerSleeping_.store(true, std::memory_order_release);
            const uint32_t observed = compilerWakeEpoch_.load(std::memory_order_acquire);
            // Close the store-to-sleep race: a producer that arrived before
            // the epoch load either wakes us or is observed by this retry.
            if (!midiIngress_.TryPopFair(timed, fairLaneCursor)) {
                WaitForAddressChange(compilerWakeEpoch_, observed);
                compilerSleeping_.store(false, std::memory_order_release);
                continue;
            }
            compilerSleeping_.store(false, std::memory_order_release);
        }

        chunkScheduler.Reset();
        uint32_t drained = 0u;
        auto compileOne = [&](const TimestampedMidiEvent& source) {
            ScheduledRenderEvent scheduled{};
            if (CompileTimestampedEvent(source, epoch, qpcFreq, sampleRate,
                                        bufferFrames, scheduled)) {
                const bool admitted = chunkScheduler.EnqueueBatched(scheduled);
                assert(admitted);
                (void)admitted;
            }
        };
        compileOne(timed);
        ++drained;

        // Priority lanes are FIFO runs in normal host traffic. Round-robin
        // popping used to alternate those runs event-by-event, manufacturing
        // thousands of inversions that the chunk sorter immediately had to
        // undo. Drain a bounded run per lane instead: fairness is retained,
        // but a normal chunk reaches the sorter as only a handful of runs.
        drained += midiIngress_.DrainFairRuns(
            kCompilerChunkCapacity - drained, kCompilerLaneRunQuota,
            fairLaneCursor, compileOne);

        // Ordering is deliberately paid here, outside the callback. Chunks
        // remain individually sorted in compiledIngress_; the audio thread
        // only merges their natural runs with any carried future events.
        chunkScheduler.FinalizeBatch();
        ScheduledRenderEvent scheduled{};
        while (chunkScheduler.PopBefore(INT64_MAX, scheduled)) {
            for (;;) {
                if (compiledIngress_.Push(scheduled)) break;
                if (cancelProducers_.load(std::memory_order_acquire)) return;
                const uint32_t observed =
                    compilerWakeEpoch_.load(std::memory_order_acquire);
                // Recheck after observing the wake epoch so a just-freed slot
                // cannot be missed between the failed push and the wait.
                if (compiledIngress_.Push(scheduled)) break;
                WaitForAddressChange(compilerWakeEpoch_, observed);
            }
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

    VoiceManager* vm = self->voiceManager;
    ChannelCache* cc = self->channelCache;
    RenderScalar* render = self->renderScalar;
    RuntimeConfigSnapshot* snap = self->configSnapshot;
    const float* sd = self->sampleDataStore;

    if (!vm || !cc || !render || !snap) return;

    // ── Mailbox sync: seqlock read of the live params ──────────────
    // The control thread publishes via odd→even sequence; we copy once
    // per block, retrying on a torn (odd / stale) sequence by falling
    // back to the last stable copy.  All live params land in the local
    // structs the render path reads, eliminating every data race between
    // the control thread (writer) and this audio thread (reader).
    LiveConfigMailbox mb;
    const uint32_t seq =
        self->liveMailboxSeq_.load(std::memory_order_acquire);
    if ((seq & 1u) == 0u) {
        mb = self->liveMailbox_;
        if (self->liveMailboxSeq_.load(std::memory_order_acquire) == seq) {
            self->liveMailboxApplied_ = mb;
        } else {
            mb = self->liveMailboxApplied_;
        }
    } else {
        mb = self->liveMailboxApplied_;
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

    // FDN delays, feedback, taps, filters and LFOs derive from the raw
    // fields.  UpdateDerived recomputes them WITHOUT clearing the delay
    // lines, so live parameter changes morph the tail instead of cutting
    // it dead.  (~50 scalar ops once per block: negligible.)
    self->reverb.UpdateDerived();

    self->limiter.enabled       = mb.limiterEnabled;
    self->limiter.threshold     = mb.limiterThreshold;
    self->limiter.delayFrames   = mb.limiterDelayFrames;
    self->limiter.attackCoeff   = mb.limiterAttackCoeff;
    self->limiter.releaseCoeff  = mb.limiterReleaseCoeff;

    self->channelCache->SetMasterVolume(mb.masterVolume);

    LARGE_INTEGER renderStartQPC;
    QueryPerformanceCounter(&renderStartQPC);
    const bool profileCallback = self->diagnosticsEnabled_;
    const uint64_t profileCycleStart = profileCallback ? __rdtsc() : 0u;

    cc->RebuildCache(*snap, static_cast<float>(self->sampleRate));

    // Fold a live master-volume change into ALL playing voices (their
    // mixGainL/R are only refreshed at note-on otherwise).  Per-channel
    // refresh over the active list — done only when the value actually
    // changed, never per block at 500K voices.
    if (self->appliedMasterVolume_ != snap->masterVolume) {
        self->appliedMasterVolume_ = snap->masterVolume;
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
    const uint32_t ingressScanBudget =
        self->eventScheduler_.Capacity();
    while (scannedIngress < ingressScanBudget &&
           admittedEvents < self->maxEventsPerBlock_ &&
           self->eventScheduler_.Size() < self->eventScheduler_.Capacity()) {
        ScheduledRenderEvent scheduled{};
        if (self->useEventCompiler_) {
            if (!self->compiledIngress_.TryPop(scheduled)) break;
        } else {
            TimestampedMidiEvent timed{};
            if (!self->midiIngress_.TryPop(timed)) break;
            if (!CompileTimestampedEvent(
                    timed, self->virtualRenderClockQPC, self->qpcFreq,
                    self->sampleRate, self->bufferFrames, scheduled)) {
                ++scannedIngress;
                continue;
            }
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
    if (self->useEventCompiler_ && scannedIngress != 0u) {
        self->compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
        WakeAddressWaiters(self->compilerWakeEpoch_);
    }

    // Recovered notes all become writable at this block's first frame. Late
    // note-offs are replayed in batches of at most 255, capped by the current
    // maximum possible voice generations. This retains note-off multiplicity
    // without allowing millions of dead historical messages to consume the
    // callback quota. The scheduler restores frame/sequence order even though
    // the compact set is walked by channel/key here.
    for (uint32_t key = 0;
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

    self->eventScheduler_.FinalizeBatch();

    self->producerWakeEpoch_.fetch_add(1, std::memory_order_release);
    WakeAddressWaiters(self->producerWakeEpoch_);
    self->scheduledSizePublished_.store(self->eventScheduler_.Size(),
                                        std::memory_order_release);

    // Extract only this render window.  Future events remain in the heap.
    RenderEvent* evtBuf = self->eventBuffer;
    uint32_t evCount = 0;
    uint32_t examinedCount = 0;
    const uint32_t eventBudget =
        self->eventBufferCapacity_;
    ScheduledRenderEvent scheduledOut;
    while (examinedCount < eventBudget &&
           self->eventScheduler_.PopBefore(self->virtualRenderSample_ + numFrames,
                                           scheduledOut)) {
        ++examinedCount;
        if (scheduledOut.type == RenderEventType::NoteOn &&
            IsObsoleteNoteOn(scheduledOut.targetFrame,
                             self->virtualRenderSample_, self->bufferFrames)) {
            ++self->telemetry_.late;
            ++self->telemetry_.staleNoteOnsSkipped;
            continue;
        }
        int64_t offset = scheduledOut.targetFrame - self->virtualRenderSample_;
        if (offset < 0) { ++self->telemetry_.late; offset = 0; }
        evtBuf[evCount++] = scheduledOut.ToRenderEvent(
            static_cast<uint32_t>(offset));
        ++self->telemetry_.dispatched;
    }
    self->scheduledSizePublished_.store(self->eventScheduler_.Size(),
                                        std::memory_order_release);

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
    // Filter the final limited samples in the same loop so the 3 Hz cutoff
    // neither changes gain detection nor requires another memory pass.
    self->reverb.Process(output, numFrames, 2);
    self->limiter.Process(output, numFrames, 2, self->postHighPass);

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
                uint32_t releasingVoices = 0;
                uint32_t sustainHeldVoices = 0;
                for (uint32_t position = 0; position < vm->activeCount_; ++position) {
                    const uint32_t voice = vm->activeList_[position];
                    releasingVoices += vm->v.state[voice] ==
                        static_cast<uint8_t>(VoiceState::Releasing);
                    sustainHeldVoices += vm->v.heldBySustain[voice] != 0;
                }
                DiagWindow_Update(vm->activeCount_, vm->GetMaxVoices(),
                                  releasingVoices, sustainHeldVoices,
                                  vm->stealCount_,
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
                                  s_schedulerSmoothed, s_dispatchSmoothed,
                                  s_synthesisSmoothed, s_postSmoothed,
                                  evCount, self->eventScheduler_.Size(),
                                  self->sf2Telemetry_);
            }
        }
    }

    // ── Audio→control snapshot (RuntimeLink V2) ────────────────────
    // The control thread publishes at ~30 Hz from this process-local
    // snapshot; the audio thread never touches shared memory.  The
    // O(activeN) releasing-voice scan is throttled to every 3rd block.
    // Snapshot stores are ~30 aligned 32-bit writes per block.
#if !defined(SVMS_XP_COMPAT)
    {
        constexpr uint32_t kStamp = 1u;
        if (++self->releasingScanTick_ % 3u == 0u) {
            uint32_t releasing = 0;
            for (uint32_t i = 0; i < vm->activeCount_; ++i) {
                const uint32_t v = vm->activeList_[i];
                releasing += vm->v.state[v] ==
                    static_cast<uint8_t>(svms::VoiceState::Releasing);
            }
            self->lastReleasingVoices_ = releasing;
        }
        svms::RuntimeAudioSnapshot& as = g_audioSnapshot;
        as.sequence = 1;
        RLV2_MemBarrier();
        as.activeVoices.Store(static_cast<float>(vm->activeCount_), kStamp);
        as.releasingVoices.Store(static_cast<float>(self->lastReleasingVoices_), kStamp);
        as.freeTop.Store(static_cast<float>(vm->freeTop_), kStamp);
        as.voiceSteals.Store(static_cast<float>(vm->stealCount_), kStamp);
        as.retiredCount.Store(static_cast<float>(vm->retireCount_), kStamp);
        as.retiredImmediateCount.Store(static_cast<float>(vm->retireImmediateCount_), kStamp);
        as.decimationStep.Store(static_cast<float>(self->correctnessMode_ ? 1u
            : svms::ComputeDecimationStep(vm->activeCount_)), kStamp);
        as.renderPeak.Store(self->sf2Telemetry_.renderPeak, kStamp);
        as.audioRunning.Store(self->audioOutput && self->audioOutput->IsRunning()
            ? 1.0f : 0.0f, kStamp);
        as.soundFontLoaded.Store(self->soundFontData && self->sampleDataStore
            ? 1.0f : 0.0f, kStamp);
        as.audioHResult.Store(static_cast<float>(self->audioOutput
            ? static_cast<int32_t>(self->audioOutput->GetLastError()) : 0), kStamp);
        as.cpuLoadPercent.Store(s_cpuSmoothed, kStamp);
        as.callbackP95Percent.Store(self->telemetry_.callbackP95Percent, kStamp);
        as.callbackP99Percent.Store(self->telemetry_.callbackP99Percent, kStamp);
        as.callbackP999Percent.Store(self->telemetry_.callbackP999Percent, kStamp);
        as.maxConsecutiveOverBudget.Store(
            static_cast<float>(self->telemetry_.maxConsecutiveOverBudget), kStamp);
        as.overBudgetCallbacks = self->telemetry_.overBudgetCallbacks;
        as.eventsSubmitted = self->telemetry_.submitted;
        as.eventsAccepted = self->telemetry_.accepted;
        as.eventsDropped = self->telemetry_.dropped;
        as.eventsDispatched = self->telemetry_.dispatched;
        as.limiterInputPeakL.Store(self->limiter.inputPeakL, kStamp);
        as.limiterInputPeakR.Store(self->limiter.inputPeakR, kStamp);
        as.limiterOutputPeakL.Store(self->limiter.outputPeakL, kStamp);
        as.limiterOutputPeakR.Store(self->limiter.outputPeakR, kStamp);
        as.limiterGainReductionDb.Store(self->limiter.gainReductionDb, kStamp);
        as.tickMs = static_cast<uint32_t>(GetTickCount());
        RLV2_MemBarrier();
        as.sequence = RLV2_SnapshotSettledSequence;
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
        case RenderEventType::AllNotesOff:
        case RenderEventType::AllSoundOff:
            // [HOOK] Overload ladder: these can trigger hard/panic release.
            break;
        case RenderEventType::Reset:
            if (self->voiceManager) self->voiceManager->Reset();
            if (self->channelCache) self->channelCache->Reset();
            self->RefreshSelectedPresets();
            std::fill(std::begin(self->channelPitchBendRatio_),
                      std::end(self->channelPitchBendRatio_), 1.0f);
            for (uint32_t channel = 0; channel < kChannelCount; ++channel)
                ++self->channelLaunchRevision_[channel];
            self->nextPlayIndex_ = 1;
            self->postHighPass.Reset();
            self->reverb.Reset();
            self->limiter.Reset();
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
    while (index < eventCount) {
        if (events[index].type != RenderEventType::NoteOn) {
            DispatchRenderEvent(events[index++], blockCursor, self);
            continue;
        }
        const uint64_t globalFence =
            self->globalTerminationFence_.load(std::memory_order_acquire);
        do {
            const RenderEvent& event = events[index++];
            const uint64_t channelFence = event.channel < kChannelCount
                ? self->channelTerminationFence_[event.channel].load(
                    std::memory_order_acquire)
                : 0u;
            if (!FenceSuppresses(event.ingressSequence, globalFence) &&
                !FenceSuppresses(event.ingressSequence, channelFence)) {
                self->HandleNoteOn(event.channel, event.data1, event.data2);
            }
        } while (index < eventCount &&
                 events[index].type == RenderEventType::NoteOn);
    }
    if (self->diagnosticsEnabled_)
        self->dispatchCyclesCurrent_ += __rdtsc() - profileBegin;
}

uint32_t Driver::ResolveNoteRegions(uint32_t presetIndex, uint8_t note,
                                    uint8_t velocity,
                                    const SFSampleRegion** outRegions,
                                    uint32_t outCapacity) {
    if (!soundFontData || !outRegions || presetIndex >= soundFontData->presetCount)
        return 0u;

    const uint32_t tag = (presetIndex << 14u) |
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
            outRegions[i] = &soundFontData->regions[cached.regionIndices[i]];
        return count;
    }

    ++telemetry_.noteRegionCacheMisses;
    const uint32_t count = sf2_find_regions(soundFontData, presetIndex, note,
                                            velocity, outRegions, outCapacity);
    if (count <= kNoteRegionCacheLayers && count <= outCapacity) {
        cached.tag = tag;
        cached.count = static_cast<uint16_t>(count);
        cached.reserved = 0u;
        for (uint32_t i = 0; i < count; ++i) {
            cached.regionIndices[i] = static_cast<uint32_t>(
                outRegions[i] - soundFontData->regions);
        }
    }
    return count;
}

void Driver::RefreshSelectedPresets() {
    if (!channelCache || !soundFontData) return;
    for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
        uint32_t presetIndex = 0u;
        if (ResolveChannelPreset(soundFontData, *channelCache, channel,
                                 &presetIndex)) {
            channelCache->SetSelectedPreset(channel,
                static_cast<uint16_t>(presetIndex));
        } else {
            channelCache->SetSelectedPreset(channel, UINT16_MAX);
        }
    }
}

void Driver::HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!channelCache || !voiceManager) return;
    if (channel >= kChannelCount || note >= kNoteCount) return;
    velocity &= 0x7fu;

    ++sf2Telemetry_.noteOns;
    const bool captureDetail = captureSf2Detail_;

    const float velGain = configuredVelocityGain_[velocity];
    if (velGain <= 0.0f) return;

    channelCache->NoteOn(channel, note, velocity);

    if (!soundFontData || !samplesStore || !sampleDataStore) return;

    // Bank/program handlers commit this cache at their exact event frame.
    // The fallback covers reset and SoundFont-swap boundaries only; the
    // multi-million-NPS path therefore avoids scanning the preset table for
    // every repeated note.
    uint32_t presetIndex = channelCache->GetSelectedPreset(channel);
    if (presetIndex >= soundFontData->presetCount) {
        if (!ResolveChannelPreset(soundFontData, *channelCache, channel,
                                  &presetIndex)) {
            ++sf2Telemetry_.invalidPresets;
            return;
        }
        channelCache->SetSelectedPreset(channel,
                                        static_cast<uint16_t>(presetIndex));
    }
    // Probe the complete launch-plan cache before doing even the cached
    // region lookup.  The former ordering resolved/copied regions and
    // revalidated every layer before discovering that the fully prepared
    // plan was already present.  At Black-MIDI rates that redundant work is
    // paid close to a million times per second.
    uint32_t launchHash = presetIndex * 0x9e3779b9u;
    launchHash ^= static_cast<uint32_t>(note) * 0x85ebca6bu;
    launchHash ^= static_cast<uint32_t>(velocity) * 0xc2b2ae35u;
    launchHash ^= static_cast<uint32_t>(channel) * 0x27d4eb2fu;
    launchHash ^= channelLaunchRevision_[channel] * 0x165667b1u;
    launchHash ^= launchHash >> 16u;
    NoteLaunchPlanCacheEntry& launchCache =
        noteLaunchPlanCache_[launchHash & (kNoteRegionCacheSize - 1u)];
    const bool launchCacheHit =
        launchCache.soundFontGeneration == soundFontGeneration_ &&
        launchCache.channelRevision == channelLaunchRevision_[channel] &&
        launchCache.presetIndex == presetIndex &&
        launchCache.channel == channel && launchCache.note == note &&
        launchCache.velocity == velocity && launchCache.count != 0u &&
        launchCache.count <= kNoteRegionCacheLayers;

    uint32_t matchCount = launchCacheHit ? launchCache.count
        : ResolveNoteRegions(presetIndex, note, velocity,
                             noteRegionScratch_, kMaxMatchingRegions);
    if (matchCount > kMaxMatchingRegions) {
        ++telemetry_.allocationFailures;
        return;
    }

    if (matchCount == 0) {
        ++telemetry_.zeroMatchedRegions;
        ++sf2Telemetry_.zeroMatchedRegions;
        return;
    }

    // Validate every layer before mutating the voice pool. A malformed
    // SoundFont region must reject the complete generation, never leave a
    // partial instrument whose remaining layers become audible artifacts.
    if (!launchCacheHit) {
        for (uint32_t mi = 0; mi < matchCount; ++mi) {
            const SFSampleRegion* region = noteRegionScratch_[mi];
            if (!region || region->sampleIndex >= sampleStoreCount) {
                ++sf2Telemetry_.invalidRegions;
                return;
            }
            const uint32_t regionIndex = static_cast<uint32_t>(
                region - soundFontData->regions);
            const bool valid = preparedRegions && regionIndex < preparedRegionCount
                ? preparedRegions[regionIndex].valid != 0u
                : sf2_validate_region(soundFontData, region);
            if (!valid) {
                ++sf2Telemetry_.invalidSampleRanges;
                return;
            }
        }
    }
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
        channelCache->GetPitchBendSemitones(channel);
    const float commonBendRatio = channelPitchBendRatio_[channel];
    const VoiceConfiguration* launchSetups = noteLaunchScratch_;
    if (launchCacheHit) {
        // The cached setup is immutable. playIndex is the only per-note field;
        // pass it separately instead of copying every layer into scratch just
        // to patch four bytes at multi-million-note rates.
        launchSetups = launchCache.setup;
    } else for (uint32_t mi = 0; mi < matchCount; ++mi) {
        const SFSampleRegion* matchedRegion = noteRegionScratch_[mi];
        const uint32_t matchedRegionIndex = static_cast<uint32_t>(
            matchedRegion - soundFontData->regions);
        const PreparedSF2Region* prepared =
            preparedRegions && matchedRegionIndex < preparedRegionCount
                ? &preparedRegions[matchedRegionIndex] : nullptr;
        uint32_t sampleIndex = matchedRegion->sampleIndex;
        const SF2Sample& samp = samplesStore[sampleIndex];

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

        uint32_t sStart = static_cast<uint32_t>(matchedRegion->startOffset);
        uint32_t sEnd = static_cast<uint32_t>(matchedRegion->endOffset);
        uint32_t sLoopStart = static_cast<uint32_t>(matchedRegion->loopStartOffset);
        uint32_t sLoopEnd = static_cast<uint32_t>(matchedRegion->loopEndOffset);
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
        noteLaunchScratch_[mi] = setup;
    }

    if (!launchCacheHit && matchCount <= kNoteRegionCacheLayers) {
        launchCache.soundFontGeneration = soundFontGeneration_;
        launchCache.channelRevision = channelLaunchRevision_[channel];
        launchCache.presetIndex = static_cast<uint16_t>(presetIndex);
        launchCache.channel = channel;
        launchCache.note = note;
        launchCache.velocity = velocity;
        launchCache.count = static_cast<uint8_t>(matchCount);
        for (uint32_t layer = 0u; layer < matchCount; ++layer)
            launchCache.setup[layer] = noteLaunchScratch_[layer];
    }

    if (!voiceManager->LaunchVoiceGroup(
            channel, note, velocity, launchSetups, matchCount, playIndex,
            channelCache->GetParams()[channel], noteLaunchHandles_)) {
        ++telemetry_.allocationFailures;
        return;
    }

    sf2Telemetry_.configuredVoices += matchCount;
    if (!captureDetail) return;
    captureSf2Detail_ = false;
    sf2Telemetry_.lastChannel = channel;
    sf2Telemetry_.lastNote = note;
    sf2Telemetry_.lastVelocity = velocity;
    sf2Telemetry_.lastPreset = static_cast<uint16_t>(presetIndex);
    const uint32_t last = matchCount - 1u;
    const VoiceConfiguration& lastSetup = launchSetups[last];
    const VoiceHandle lastVoice = noteLaunchHandles_[last];
    const SFSampleRegion* lastRegion =
        &soundFontData->regions[lastSetup.regionIndex];
    sf2Telemetry_.lastRegion = lastSetup.regionIndex;
    sf2Telemetry_.lastSample = static_cast<uint16_t>(lastRegion->sampleIndex);
    sf2Telemetry_.lastSampleStart = lastSetup.sampleStart;
    sf2Telemetry_.lastSampleEnd = lastSetup.sampleEnd;
    sf2Telemetry_.lastInitialPeak =
        regionInitialPeaks && lastSetup.regionIndex < regionInitialPeakCount
            ? regionInitialPeaks[lastSetup.regionIndex] : 0.0f;
    sf2Telemetry_.lastVoiceGain = lastSetup.initialGain;
    sf2Telemetry_.lastMixGainL = voiceManager->v.mixGainL[lastVoice];
    sf2Telemetry_.lastMixGainR = voiceManager->v.mixGainR[lastVoice];
    sf2Telemetry_.lastDelaySamples = lastSetup.delaySamples;
    sf2Telemetry_.lastAttackSamples = lastSetup.attackSamples;
    sf2Telemetry_.lastFloatSample = sampleDataStore[lastSetup.sampleStart];
    sf2Telemetry_.lastPhaseStep = lastSetup.phaseStep;
    sf2Telemetry_.lastPhase = voiceManager->v.phases[lastVoice];
    sf2Telemetry_.lastRelativeEnd = voiceManager->v.relEnd[lastVoice];
    sf2Telemetry_.lastSampleBacked = voiceManager->v.sampleBacked[lastVoice];
    sf2Telemetry_.lastVoiceHandle = lastVoice;
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
    if (channelCache) channelCache->ControlChange(channel, controller, value);
    if (channel < kChannelCount &&
        (controller == 0u || controller == 32u || controller == 6u ||
         controller == 38u || controller == 96u || controller == 97u ||
         controller == 100u || controller == 101u)) {
        ++channelLaunchRevision_[channel];
    }

    if ((controller == 0 || controller == 32) && channelCache && soundFontData &&
        channel < kChannelCount) {
        uint32_t presetIndex = 0u;
        if (ResolveChannelPreset(soundFontData, *channelCache, channel,
                                 &presetIndex)) {
            channelCache->SetSelectedPreset(channel,
                                            static_cast<uint16_t>(presetIndex));
        } else {
            channelCache->SetSelectedPreset(channel, UINT16_MAX);
        }
    }

    if (controller == 64) {
        if (value < 64) {
            voiceManager->ForEachChannelActive(channel, [&](VoiceHandle voice) {
                const uint32_t i = voice;
                if (voiceManager->v.heldBySustain[i]) {
                    voiceManager->v.heldBySustain[i] = 0;
                    voiceManager->StartRelease(i);
                }
            });
        }
    }

    if (controller == 120) {
        voiceManager->SilenceChannelImmediate(channel);
    } else if (controller == 123) {
        voiceManager->ReleaseChannel(channel, blockOffset);
    } else if (controller == 121 && sustainWasActive) {
        voiceManager->ForEachChannelActive(channel, [&](VoiceHandle voice) {
            const uint32_t i = voice;
            if (voiceManager->v.heldBySustain[i]) {
                voiceManager->v.heldBySustain[i] = 0;
                voiceManager->StartRelease(i);
            }
        });
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
         controller == 38 || controller == 96 || controller == 97)) {
        channelCache->RebuildChannel(channel, *configSnapshot,
                                     static_cast<float>(sampleRate));
        if (controller == 7 || controller == 10 || controller == 11 ||
            controller == 121) {
            voiceManager->RefreshMixGainsForChannel(
                channel, channelCache->GetParams()[channel]);
        }
    }
}

void Driver::HandleProgramChange(uint8_t channel, uint8_t program) {
    if (!channelCache || !soundFontData || channel >= kChannelCount) return;

    // Validate against the bank currently selected on this channel before
    // committing the new program. Existing voices retain their stored region.
    const uint8_t oldProgram = channelCache->GetProgram(channel);
    channelCache->ProgramChange(channel, program);

    uint32_t presetIndex = 0;
    if (ResolveChannelPreset(soundFontData, *channelCache, channel, &presetIndex)) {
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

    const float bendSemitones = channelCache->GetPitchBendSemitones(channel);
    const float commonRatio = powf(2.0f, bendSemitones / 12.0f);
    channelPitchBendRatio_[channel] = commonRatio;
    voiceManager->ForEachChannelActive(channel, [&](VoiceHandle voice) {
        const uint32_t i = voice;
        const float scale = voiceManager->v.pitchBendScales[i];
        const float ratio = scale == 1.0f
            ? commonRatio : powf(2.0f, bendSemitones * scale / 12.0f);
        voiceManager->v.phaseIncs[i] = voiceManager->v.basePhaseIncs[i] * ratio;
    });
}

} // namespace svms

static svms::Driver* g_driver = nullptr;
static const HMIDIOUT kSVMSMidiOutHandle = reinterpret_cast<HMIDIOUT>(0x1234);

static bool IsSupportedMidiOutputDevice(UINT_PTR deviceId) {
    return deviceId == 0u || deviceId == static_cast<UINT_PTR>(MIDI_MAPPER);
}

extern "C" {

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
    (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    XPBootstrapTrace("[SVMS XP] midiOutOpen reached\r\n");
    LOG("midiOutOpen: uDeviceID=%u", uDeviceID);
    if (!IsSupportedMidiOutputDevice(uDeviceID)) {
        XPBootstrapTrace("[SVMS XP] midiOutOpen rejected unsupported device ID\r\n");
        return MMSYSERR_BADDEVICEID;
    }
    if (!phmo) return MMSYSERR_INVALPARAM;

    if (!g_driver || !g_driver->initialized) {
        if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
        g_driver = &svms::Driver::Instance();
        if (!g_driver->Initialize()) {
            LOG("midiOutOpen: Initialize FAILED");
            XPBootstrapTrace("[SVMS XP] engine initialization FAILED\r\n");
#if !defined(SVMS_XP_COMPAT)
            g_driver->Shutdown();
            g_driver = nullptr;
#endif
            return MMSYSERR_NOMEM;
        }
    }

    const bool loaded = g_driver->LoadConfiguredSoundFont();

    LOG("midiOutOpen: SF loaded=%d", loaded);
    if (!g_driver->StartAudio()) {
        LOG("midiOutOpen: audio start FAILED");
#if !defined(SVMS_XP_COMPAT)
        g_driver->Shutdown();
        g_driver = nullptr;
#endif
        return MMSYSERR_ERROR;
    }

    LOG("midiOutOpen: SUCCESS, returning handle");
    *phmo = kSVMSMidiOutHandle;
    XPBootstrapTrace("[SVMS XP] midiOutOpen SUCCESS handle=0x00001234\r\n");
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutClose(HMIDIOUT hmo) {
    (void)hmo;
    XPBootstrapTrace("[SVMS XP] midiOutClose reached\r\n");
    if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
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
    (void)hmo; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutReset(HMIDIOUT hmo) {
    (void)hmo;
    if (g_driver) g_driver->ResetAllVoices();
    LOG("midiOutReset: all voices released");
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmo; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT hmo, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmo; (void)lpMidiHdr; (void)cbMidiHdr;
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

static bool g_kdmapiInitialized = false;

static bool EnsureDriverInitialized() {
    if (g_driver && g_driver->initialized) return true;
    if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
    g_driver = &svms::Driver::Instance();
    if (!g_driver->Initialize()) {
        LOG("KDMAPI: Initialize FAILED");
        XPBootstrapTrace("[SVMS XP] KDMAPI engine initialization FAILED\r\n");
#if !defined(SVMS_XP_COMPAT)
        g_driver->Shutdown();
        g_driver = nullptr;
#endif
        return false;
    }
    g_driver->LoadConfiguredSoundFont();
    return g_driver->StartAudio();
}

BOOL WINAPI IsKDMAPIAvailable(void) {
    XPBootstrapTrace("[SVMS XP] IsKDMAPIAvailable reached\r\n");
    return TRUE;
}

LPVOID WINAPI InitializeKDMAPIStream(void) {
    if (!EnsureDriverInitialized()) return nullptr;
    g_kdmapiInitialized = true;
    return reinterpret_cast<LPVOID>(1);
}

void WINAPI TerminateKDMAPIStream(void) {
    if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
    g_kdmapiInitialized = false;
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
    if (!g_kdmapiInitialized) return;
    if (g_driver) g_driver->SubmitShortMsg(dwMsg);
}

void WINAPI SendDirectDataNoBuf(DWORD dwMsg) {
    SendDirectData(dwMsg);
}

UINT WINAPI SendCustomEvent(DWORD dwEvent, LPVOID pData, DWORD dwSize) {
    (void)pData; (void)dwSize;
    if (!g_kdmapiInitialized) return 0;
    if (g_driver) g_driver->SubmitShortMsg(static_cast<DWORD>(dwEvent));
    return 1;
}

void WINAPI SendDirectLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)lpMidiHdr; (void)cbMidiHdr;
}

void WINAPI SendDirectLongDataNoBuf(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)lpMidiHdr; (void)cbMidiHdr;
}

MMRESULT WINAPI PrepareLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI UnprepareLongData(LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)lpMidiHdr; (void)cbMidiHdr;
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

UINT WINAPI midiInGetNumDevs(void) { return 0; }

MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR uDeviceID, LPMIDIINCAPSA lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR uDeviceID, LPMIDIINCAPSW lpCaps, UINT cbCaps) {
    (void)uDeviceID; (void)lpCaps; (void)cbCaps;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInOpen(LPHMIDIIN phmi, UINT uDeviceID, DWORD_PTR dwCallback,
                           DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)phmi; (void)uDeviceID; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInClose(HMIDIIN hmi) {
    (void)hmi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInPrepareHeader(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmi; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInUnprepareHeader(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmi; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInAddBuffer(HMIDIIN hmi, LPMIDIHDR lpMidiHdr, UINT cbMidiHdr) {
    (void)hmi; (void)lpMidiHdr; (void)cbMidiHdr;
    return MMSYSERR_ERROR;
}

MMRESULT WINAPI midiInStart(HMIDIIN hmi) {
    (void)hmi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInStop(HMIDIIN hmi) {
    (void)hmi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInReset(HMIDIIN hmi) {
    (void)hmi;
    return MMSYSERR_BADDEVICEID;
}

MMRESULT WINAPI midiInMessage(HMIDIIN hmi, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hmi; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_ERROR;
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
        XPBootstrapTrace("[SVMS XP] V3 winmm.dll loaded\r\n");
        LogInit();
        LOG("DLL_PROCESS_ATTACH");
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        LOG("DLL_PROCESS_DETACH");
    }
    return TRUE;
}

} // extern "C"
