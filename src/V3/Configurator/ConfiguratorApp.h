#ifndef SVMS_CONFIGURATOR_CONFIGURATORAPP_H
#define SVMS_CONFIGURATOR_CONFIGURATORAPP_H

#include "ConfigDocument.h"
#include "WasapiDevices.h"
#include "EasterEggs.h"
#include "UpdateService.h"
#include "PageOffline.h"
#include "PageLiveRecording.h"
#include "Platform.h"
#include "../SVMSRuntimeLink.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>


struct ImGuiContext;

namespace svms::cfg {

enum class Page {
    Overview,
    Audio,
    Performance,
    Midi,
    OfflineRenderer,
    LiveRecording,
    Reverb,
    Limiter,
    ChannelLimiter,
    Diagnostics,
    Advanced,
    About,
    Synth,
    PageCount
};

class ConfiguratorApp {
public:
    ConfiguratorApp() = default;
    ~ConfiguratorApp() = default;

    bool Initialize(int argc, char** argv);
    void Shutdown();
    bool PumpMessages();
    bool IsMinimized() const;
    void WaitForEvent();
    void SetWindowTitle(const wchar_t* title);
    void RenderFrame();

    bool IsRunning() const { return running_; }

    const wchar_t* LastInitError() const { return lastInitError_.c_str(); }

    void SetLiveFloat(svms::RLCommandType type, float value);
    void SetLiveBool(svms::RLCommandType type, bool value);
    void SetLiveChannelLimiter(bool enabled, float threshold, float releaseMs);
    void SendPendingChannelLimiter();
    void SetLiveMaxVoices(uint32_t value);
    void SetLiveLimiterAlgorithm(uint32_t value);

    // Jump to a stage/page (used by the Home overview's OPEN keys).
    void NavigateTo(Page page) { currentPage_ = page; }

private:

    bool CreateImGui();
    void RecreateFonts(float scale, bool rendererBackendInitialized);

    void DrawHeader();
    void DrawMasterStrip(float y);
    void DrawChainStrip(float y);
    void DrawFooter();
    void DrawPageContent();
    void DrawToastOverlay();
    void HandleKeyboardShortcuts();

    std::unique_ptr<Platform> platform_;
    bool running_ = false;
    bool dirty_ = false;


    int windowWidth_ = 1180;
    int windowHeight_ = 760;
    float dpiScale_ = 1.0f;

    bool shutdownDone_ = false;
    std::wstring lastInitError_;
    std::vector<unsigned char> fontData_;
    std::vector<unsigned char> monoFontData_;

    Page currentPage_ = Page::Overview;

    ConfigDocument config_;
    EasterEggState easterEggs_;
    UpdateService updateService_;
    OfflineRendererPage offlineRendererPage_;
    LiveRecordingPage liveRecordingPage_;
    bool showMegaFuckerPopup_ = false;

    svms::RuntimeLinkClient rlClient_;
    svms::RuntimeLinkTelemetryV2 rlTelemetry_{};
    bool rlConnected_ = false;
    float rlPollTimer_ = 0.0f;
    static constexpr float kRlPollInterval = 1.0f / 30.0f;

    svms::RuntimeLiveStateV2 workingLive_{};
    uint32_t pendingLiveMask_ = 0;
    // Coalesced SetChannelLimiter command (dedicated wire command because
    // RuntimeLiveStateV2 is ABI-pinned without channel limiter fields).
    bool pendingChannelLimiter_ = false;
    float pendingChannelLimiterValues_[3] = {};
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
