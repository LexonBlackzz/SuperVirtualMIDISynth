#ifndef SVMS_CONFIGURATOR_CONFIGURATORAPP_H
#define SVMS_CONFIGURATOR_CONFIGURATORAPP_H

#include "ConfigDocument.h"
#include "PageAbout.h"
#include "WasapiDevices.h"
#include "EasterEggs.h"
#include "../SVMSRuntimeLink.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

struct ImGuiContext;

namespace svms::cfg {

enum class Page {
    Overview,
    Audio,
    Performance,
    Midi,
    Reverb,
    Limiter,
    Diagnostics,
    Advanced,
    About,
    PageCount
};

class ConfiguratorApp {
public:
    ConfiguratorApp() = default;
    ~ConfiguratorApp() = default;

    bool Initialize(HINSTANCE hInstance, int argc, char** argv);
    void Shutdown();
    bool PumpMessages();
    void RenderFrame();

    bool IsRunning() const { return running_; }

    const wchar_t* LastInitError() const { return lastInitError_.c_str(); }

    // Live-parameter entry points for widgets (see FlushLiveChanges):
    // mutate the coalescing working state and mark the group dirty.
    void SetLiveFloat(svms::RLCommandType type, float value);
    void SetLiveBool(svms::RLCommandType type, bool value);

    // DPI change hook called from WndProc (WM_DPICHANGED).  Applies the
    // suggested window rectangle immediately, but only records the font
    // rebuild as pending: the actual atlas rebuild happens in the normal
    // frame loop, never re-entrantly inside WndProc.
    void HandleDpiChange(float scale, const RECT* suggestedRect);

private:
    friend LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam);

    bool CreateMainWindow(HINSTANCE hInstance);
    bool CreateD3D11();
    bool CreateImGui();
    void RecreateFonts(float scale, bool rendererBackendInitialized);
    void DestroyD3D11();
    void DestroyMainWindow();
    void ResizeSwapChain(int width, int height);

    void DrawHeader();
    void DrawSidebar();
    void DrawFooter();
    void DrawPageContent();
    void DrawToastOverlay();
    void HandleKeyboardShortcuts();

    bool running_ = false;
    bool dirty_ = false;

    HWND hwnd_ = nullptr;
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* renderTarget_ = nullptr;

    int windowWidth_ = 1180;
    int windowHeight_ = 760;
    float dpiScale_ = 1.0f;

    // Window lifecycle state written by WndProc, consumed by the main
    // render loop.  Rendering and swap-chain work never happen inside
    // WndProc itself.
    bool resizePending_ = false;
    UINT pendingWidth_ = 0;
    UINT pendingHeight_ = 0;
    float pendingDpiScale_ = 1.0f;
    bool dpiRebuildPending_ = false;
    bool shutdownDone_ = false;
    std::wstring lastInitError_;
    std::vector<unsigned char> fontData_;

    Page currentPage_ = Page::Overview;

    ConfigDocument config_;
    EasterEggState easterEggs_;
    bool showMegaFuckerPopup_ = false;

    // RuntimeLink V2 — live telemetry + grouped live commands
    svms::RuntimeLinkClientV2 rlClient_;
    svms::RuntimeLinkTelemetryV2 rlTelemetry_{};
    bool rlConnected_ = false;
    float rlPollTimer_ = 0.0f;
    static constexpr float kRlPollInterval = 1.0f / 30.0f; // 30 Hz

    // Coalesced live-parameter sending: widgets mutate workingLive_ and
    // set pendingLiveMask_; FlushLiveChanges() sends ONE grouped
    // ApplyLiveConfig command per flush interval.
    svms::RuntimeLiveStateV2 workingLive_{};
    uint32_t pendingLiveMask_ = 0;
    float rlFlushTimer_ = 0.0f;
    static constexpr float kRlFlushInterval = 0.25f; // 4 Hz
    uint32_t rlFailedFlushes_ = 0;

    // Auto-discovery and reconnection
    float rlReconnectTimer_ = 0.0f;
    static constexpr float kRlReconnectInterval = 3.0f; // scan every 3s when disconnected
    bool rlAutoReconnect_ = true;
    uint32_t rlLastKnownPid_ = 0;

    bool TryAutoDiscoverDriver();
    void OnConnected();

    void PollRuntimeLink();
    void FlushLiveChanges();
    void SeedWorkingLive();
    void PushAllLiveParams();

    // Part G: adopt the engine's APPLIED live state (telemetry echo) into
    // the working copy — the reverse of PushAllLiveParams.  Lets the user
    // pull out-of-band engine changes (e.g. from a second configurator)
    // into the document, then save them to disk.
    void AdoptEngineLiveState();

    // Part Q-T: display sync + frame pacing.  vsync_ drives the swap
    // chain's present interval; the frame-time ring feeds the About page
    // histogram grid.
    bool vsync_ = true;
    void RecordFrameTime(float ms);
    FramePacingStats GetFramePacingStats() const;
    float frameTimeMs_[256] = {};
    int frameTimePos_ = 0;
    int frameTimeCount_ = 0;

    std::string statusMessage_;
    float toastTimer_ = 0.0f;
    std::string toastMessage_;
};

} // namespace svms::cfg

#endif
