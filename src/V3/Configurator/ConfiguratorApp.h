#ifndef SVMS_CONFIGURATOR_CONFIGURATORAPP_H
#define SVMS_CONFIGURATOR_CONFIGURATORAPP_H

#include "ConfigDocument.h"
#include "WasapiDevices.h"
#include "EasterEggs.h"
#include "../SVMSRuntimeLink.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
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

    void SetLiveFloat(svms::RLCommandType type, float value);
    void SetLiveBool(svms::RLCommandType type, bool value);
    void SetLiveMaxVoices(uint32_t value);

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

    svms::RuntimeLinkClientV2 rlClient_;
    svms::RuntimeLinkTelemetryV2 rlTelemetry_{};
    bool rlConnected_ = false;
    float rlPollTimer_ = 0.0f;
    static constexpr float kRlPollInterval = 1.0f / 30.0f;

    svms::RuntimeLiveStateV2 workingLive_{};
    uint32_t pendingLiveMask_ = 0;
    float rlFlushTimer_ = 0.0f;
    // Live controls are coalesced only across extremely fast UI frames. At
    // 60/120/144 Hz this means a change is eligible immediately on the frame
    // that produced it, rather than sitting in the old quarter-second queue.
    static constexpr float kRlFlushInterval = 1.0f / 240.0f;
    static constexpr uint32_t kRlLiveCommandTimeoutMs = 50u;
    uint32_t rlFailedFlushes_ = 0;
    float rlRetryBackoff_ = 0.0f;

    float rlReconnectTimer_ = 0.0f;
    static constexpr float kRlReconnectInterval = 3.0f;
    bool rlAutoReconnect_ = true;
    uint32_t rlLastKnownPid_ = 0;

    bool TryAutoDiscoverDriver();
    void OnConnected();

    void PollRuntimeLink();
    void FlushLiveChanges();
    void SeedWorkingLive();
    void PushAllLiveParams();
    void AdoptEngineLiveState();

    std::string statusMessage_;
    float toastTimer_ = 0.0f;
    std::string toastMessage_;
};

} // namespace svms::cfg

#endif
