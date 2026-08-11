#include "SVMSDiagWindow.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <atomic>

namespace svms {
namespace {

struct DiagStats {
    bool audioRunning;
    bool soundFontLoaded;
    int32_t audioError;
    uint32_t sampleRate;
    uint32_t bufferFrames;
    float masterVolume;
    bool waveOutFallback;
    uint32_t activeVoices;
    uint32_t maxVoices;
    uint32_t releasingVoices;
    uint32_t sustainHeldVoices;
    uint32_t voiceSteals;
    float cpuPercent;
    float callbackP95;
    float callbackP99;
    float callbackP999;
    uint64_t overBudgetCallbacks;
    uint32_t maxConsecutiveOverBudget;
    uint32_t decimationStep;
    uint32_t retired;
    uint32_t retiredImmediate;
    LiveSF2Telemetry sf2;
};

static DiagStats g_stats[2] = {};
static std::atomic<uint32_t> g_publishedStats{0};
static std::atomic<uint32_t> g_statsReader{UINT32_MAX};

static HWND g_hwnd = nullptr;
static HFONT g_font = nullptr;
static HFONT g_fontTitle = nullptr;
static HANDLE g_thread = nullptr;
static DWORD g_threadId = 0;
static HINSTANCE g_hinst = nullptr;
static std::atomic<bool> g_running{false};
static bool g_showWindow = false;
static bool g_debugOutput = false;

static HMODULE GetCurrentModule() {
    HMODULE h = nullptr;
    static int marker = 0;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&marker, &h);
    return h;
}

static const wchar_t* kWindowClass = L"SVMS V3 Diag";
static const int kWindowWidth = 540;
static const int kWindowHeight = 460;
static const int kTimerId = 1;
// Diagnostic-only refresh. Windows timers are scheduler-limited, but 1 ms
// gives the monitor the fastest practical readout without touching audio.
static const int kTimerIntervalMs = 16;
static const int kPadX = 12;
static const int kPadY = 8;
static const int kLineH = 20;

static COLORREF kBgColor = RGB(16, 16, 16);
static COLORREF kTextLabel = RGB(180, 180, 180);
static COLORREF kTextValue = RGB(100, 255, 100);
static COLORREF kTextTitle = RGB(120, 180, 255);

static void DrawStat(HDC hdc, int x, int y, const wchar_t* label, const wchar_t* value) {
    SetTextColor(hdc, kTextLabel);
    TextOutW(hdc, x, y, label, (int)wcslen(label));
    SetTextColor(hdc, kTextValue);
    int lw = 0;
    SIZE sz;
    if (GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &sz)) lw = sz.cx;
    TextOutW(hdc, x + lw, y, value, (int)wcslen(value));
}

static DiagStats ReadPublishedStats() {
    DiagStats result{};
    for (;;) {
        const uint32_t index = g_publishedStats.load(std::memory_order_acquire);
        g_statsReader.store(index, std::memory_order_release);
        if (g_publishedStats.load(std::memory_order_acquire) != index) continue;
        result = g_stats[index];
        g_statsReader.store(UINT32_MAX, std::memory_order_release);
        return result;
    }
}

static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    HBRUSH bgBrush = CreateSolidBrush(kBgColor);
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    const DiagStats s = ReadPublishedStats();

    SelectObject(memDC, g_fontTitle);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, kTextTitle);
    TextOutW(memDC, kPadX, kPadY, L"SuperVirtualMIDISynth V3", 20);

    SelectObject(memDC, g_font);
    int y = kPadY + 28;

    wchar_t buf[160];

#if defined(SVMS_XP_COMPAT)
    const wchar_t* backend = s.waveOutFallback
                                 ? L"waveOut fallback (XP x86)"
                                 : L"DirectSound (XP x86)";
#else
    const wchar_t* backend = L"WASAPI shared";
#endif
    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                  L"%ls: %ls, %u Hz / %u frames, hr=0x%08X", backend,
                  s.audioRunning ? L"running" : L"stopped", s.sampleRate,
                  s.bufferFrames, static_cast<unsigned int>(s.audioError));
    DrawStat(memDC, kPadX, y, L"Audio:            ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                  L"%ls, master=%.3f, render peak=%.5f",
                  s.soundFontLoaded ? L"loaded" : L"MISSING / FAILED TO LOAD",
                  s.masterVolume, s.sf2.renderPeak);
    DrawStat(memDC, kPadX, y, L"SoundFont:        ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%u / %u", s.activeVoices, s.maxVoices);
    DrawStat(memDC, kPadX, y, L"Active Voices:    ", buf);
    y += kLineH;

    if (s.maxVoices > 0) {
        float pct = (float)s.activeVoices / (float)s.maxVoices * 100.0f;
        std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.0f%%", pct);
    } else {
        std::wcscpy(buf, L"-");
    }
    DrawStat(memDC, kPadX, y, L"Pool Usage:       ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"release=%u sustain=%u steals=%u",
               s.releasingVoices, s.sustainHeldVoices, s.voiceSteals);
    DrawStat(memDC, kPadX, y, L"Voice states:     ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.1f%%", s.cpuPercent);
    DrawStat(memDC, kPadX, y, L"CPU Render:       ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.0f / %.0f / %.0f%%", s.callbackP95,
               s.callbackP99, s.callbackP999);
    DrawStat(memDC, kPadX, y, L"CPU p95/99/99.9:  ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%llu (max run %u)",
               static_cast<unsigned long long>(s.overBudgetCallbacks),
               s.maxConsecutiveOverBudget);
    DrawStat(memDC, kPadX, y, L"Over budget:      ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%u", s.decimationStep);
    DrawStat(memDC, kPadX, y, L"Decimation Step:  ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%u (%u immediate)", s.retired, s.retiredImmediate);
    DrawStat(memDC, kPadX, y, L"Retired:          ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%llu / %llu / %llu",
               static_cast<unsigned long long>(s.sf2.noteOns),
               static_cast<unsigned long long>(s.sf2.exactRegionMatches),
               static_cast<unsigned long long>(s.sf2.configuredVoices));
    DrawStat(memDC, kPadX, y, L"SF2 notes/match/voice: ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"zero=%llu preset=%llu region=%llu range=%llu",
               static_cast<unsigned long long>(s.sf2.zeroMatchedRegions),
               static_cast<unsigned long long>(s.sf2.invalidPresets),
               static_cast<unsigned long long>(s.sf2.invalidRegions),
               static_cast<unsigned long long>(s.sf2.invalidSampleRanges));
    DrawStat(memDC, kPadX, y, L"SF2 rejects:       ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"ch=%u n=%u v=%u p=%u r=%u s=%u raw=%.3f mix=%.3f",
               s.sf2.lastChannel, s.sf2.lastNote, s.sf2.lastVelocity,
               s.sf2.lastPreset, s.sf2.lastRegion, s.sf2.lastSample,
               s.sf2.lastInitialPeak, s.sf2.renderPeak);
    DrawStat(memDC, kPadX, y, L"Last SF2 voice:    ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.4f  %.4f / %.4f  %u / %u", s.sf2.lastVoiceGain,
               s.sf2.lastMixGainL, s.sf2.lastMixGainR, s.sf2.lastDelaySamples,
               s.sf2.lastAttackSamples);
    DrawStat(memDC, kPadX, y, L"Gain/mix/del/atk:  ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.5f / %.5f  relEnd=%u backed=%u", s.sf2.lastPhase,
               s.sf2.lastPhaseStep, s.sf2.lastRelativeEnd, s.sf2.lastSampleBacked);
    DrawStat(memDC, kPadX, y, L"Phase/step/region: ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.6f", s.sf2.lastFloatSample);
    DrawStat(memDC, kPadX, y, L"Cached sample[0]:  ", buf);
    y += kLineH;

    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%u .. %u", s.sf2.lastSampleStart, s.sf2.lastSampleEnd);
    DrawStat(memDC, kPadX, y, L"Sample bounds:     ", buf);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            g_fontTitle = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            SetTimer(hwnd, kTimerId, kTimerIntervalMs, nullptr);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            if (g_font) { DeleteObject(g_font); g_font = nullptr; }
            if (g_fontTitle) { DeleteObject(g_fontTitle); g_fontTitle = nullptr; }
            PostQuitMessage(0);
            return 0;
        case WM_TIMER:
            if (wp == kTimerId) {
                if (g_showWindow) InvalidateRect(hwnd, nullptr, FALSE);
                if (g_debugOutput) {
                    const DiagStats stats = ReadPublishedStats();
                    char text[192];
                    std::snprintf(text, sizeof(text),
                        "[SVMS] voices=%u/%u retire=%u immediate=%u step=%u cpu=%.1f%% p99=%.0f%% over=%llu\n",
                        stats.activeVoices, stats.maxVoices, stats.retired,
                        stats.retiredImmediate, stats.decimationStep,
                        static_cast<double>(stats.cpuPercent),
                        static_cast<double>(stats.callbackP99),
                        static_cast<unsigned long long>(stats.overBudgetCallbacks));
                    OutputDebugStringA(text);
                }
            }
            return 0;
        case WM_PAINT:
            OnPaint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI WindowThreadProc(LPVOID) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    if (!RegisterClassExW(&wc)) return 1;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int x = screenW - kWindowWidth - 32;
    int y = 32;

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    if (g_showWindow) style |= WS_VISIBLE;

    g_hwnd = CreateWindowExW(exStyle, kWindowClass, L"SVMS V3 Monitor",
                              style, x, y, kWindowWidth, kWindowHeight,
                              nullptr, nullptr, g_hinst, nullptr);
    if (!g_hwnd) return 1;

    SetLayeredWindowAttributes(g_hwnd, 0, 235, LWA_ALPHA);
    if (g_showWindow) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }

    // Drain the initial WM_PAINT before signaling ready
    MSG initMsg;
    while (PeekMessageW(&initMsg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&initMsg);
        DispatchMessageW(&initMsg);
    }
    g_running.store(true, std::memory_order_release);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running.store(false, std::memory_order_release);
    g_hwnd = nullptr;
    UnregisterClassW(kWindowClass, g_hinst);
    return 0;
}

} // namespace

void DiagWindow_Create(bool showWindow, bool debugOutput) {
    if (g_hwnd || g_thread) return;
    g_showWindow = showWindow;
    g_debugOutput = debugOutput;
    g_hinst = GetCurrentModule();
    g_thread = CreateThread(nullptr, 0, WindowThreadProc, nullptr, 0, &g_threadId);
}

void DiagWindow_Destroy() {
    if (g_thread) {
        if (g_hwnd) {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        } else if (g_threadId) {
            PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        }
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_threadId = 0;
    }
}

void DiagWindow_UpdateStartup(bool audioRunning, int32_t audioError,
                              bool soundFontLoaded, uint32_t sampleRate,
                              uint32_t bufferFrames, float masterVolume,
                              bool waveOutFallback) {
    const uint32_t target = 1u - g_publishedStats.load(std::memory_order_relaxed);
    if (g_statsReader.load(std::memory_order_acquire) == target) return;
    DiagStats& stats = g_stats[target];
    stats = g_stats[g_publishedStats.load(std::memory_order_acquire)];
    stats.audioRunning = audioRunning;
    stats.audioError = audioError;
    stats.soundFontLoaded = soundFontLoaded;
    stats.sampleRate = sampleRate;
    stats.bufferFrames = bufferFrames;
    stats.masterVolume = masterVolume;
    stats.waveOutFallback = waveOutFallback;
    g_publishedStats.store(target, std::memory_order_release);
}

void DiagWindow_Update(uint32_t activeVoices, uint32_t maxVoices,
                        uint32_t releasingVoices, uint32_t sustainHeldVoices,
                        uint32_t voiceSteals,
                        float cpuPercent, uint32_t decimationStep,
                        float callbackP95, float callbackP99,
                        float callbackP999, uint64_t overBudgetCallbacks,
                        uint32_t maxConsecutiveOverBudget,
                        uint32_t retired, uint32_t retiredImmediate,
                        bool audioRunning, int32_t audioError,
                        bool soundFontLoaded, uint32_t sampleRate,
                        uint32_t bufferFrames, float masterVolume,
                        bool waveOutFallback,
                        const LiveSF2Telemetry& sf2) {
    if (!g_running.load(std::memory_order_acquire)) return;
    const uint32_t target = 1u - g_publishedStats.load(std::memory_order_relaxed);
    if (g_statsReader.load(std::memory_order_acquire) == target) return;

    DiagStats& stats = g_stats[target];
    stats.audioRunning = audioRunning;
    stats.audioError = audioError;
    stats.soundFontLoaded = soundFontLoaded;
    stats.sampleRate = sampleRate;
    stats.bufferFrames = bufferFrames;
    stats.masterVolume = masterVolume;
    stats.waveOutFallback = waveOutFallback;
    stats.activeVoices = activeVoices;
    stats.maxVoices = maxVoices;
    stats.releasingVoices = releasingVoices;
    stats.sustainHeldVoices = sustainHeldVoices;
    stats.voiceSteals = voiceSteals;
    stats.cpuPercent = cpuPercent;
    stats.callbackP95 = callbackP95;
    stats.callbackP99 = callbackP99;
    stats.callbackP999 = callbackP999;
    stats.overBudgetCallbacks = overBudgetCallbacks;
    stats.maxConsecutiveOverBudget = maxConsecutiveOverBudget;
    stats.decimationStep = decimationStep;
    stats.retired = retired;
    stats.retiredImmediate = retiredImmediate;
    stats.sf2 = sf2;
    g_publishedStats.store(target, std::memory_order_release);
}

} // namespace svms
