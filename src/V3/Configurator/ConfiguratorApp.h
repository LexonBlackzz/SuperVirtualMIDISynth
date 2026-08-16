#ifndef SVMS_CONFIGURATOR_CONFIGURATORAPP_H
#define SVMS_CONFIGURATOR_CONFIGURATORAPP_H

#include "ConfigDocument.h"
#include "WasapiDevices.h"
#include "EasterEggs.h"
#include "../SVMSRuntimeLink.h"

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

private:
    bool CreateMainWindow(HINSTANCE hInstance);
    bool CreateD3D11();
    void CreateImGui();
    void DestroyD3D11();
    void DestroyMainWindow();

    void DrawHeader();
    void DrawSidebar();
    void DrawFooter();
    void DrawPageContent();
    void DrawToastOverlay();
    void HandleKeyboardShortcuts();

    void HandleWindowSize();

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

    Page currentPage_ = Page::Overview;

    ConfigDocument config_;
    EasterEggState easterEggs_;
    bool showMegaFuckerPopup_ = false;

    // RuntimeLink — live telemetry from the driver
    svms::RuntimeLinkClient rlClient_;
    svms::RLTelemetry rlTelemetry_{};
    bool rlConnected_ = false;
    float rlPollTimer_ = 0.0f;
    static constexpr float kRlPollInterval = 1.0f / 30.0f; // 30 Hz

    // Auto-discovery and reconnection
    float rlReconnectTimer_ = 0.0f;
    static constexpr float kRlReconnectInterval = 3.0f; // scan every 3s when disconnected
    bool rlAutoReconnect_ = true;
    uint32_t rlLastKnownPid_ = 0;

    bool TryAutoDiscoverDriver();
    void OnConnected();

    void PollRuntimeLink();
    void SendLiveCommand(svms::RLCommandType type, float value);
    void SendLiveBoolCommand(svms::RLCommandType type, bool value);
    void PushAllLiveParams();

    std::string statusMessage_;
    float toastTimer_ = 0.0f;
    std::string toastMessage_;
};

} // namespace svms::cfg

#endif
