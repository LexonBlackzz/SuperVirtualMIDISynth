#ifndef SVMS_CONFIGURATOR_CONFIGURATORAPP_H
#define SVMS_CONFIGURATOR_CONFIGURATORAPP_H

#include "ConfigDocument.h"
#include "WasapiDevices.h"
#include "EasterEggs.h"
#include "UpdateService.h"
#include "PageOffline.h"
#include "PageLiveRecording.h"
#include "Platform.h"
#include "DriverLink.h"
#include "SettingsRegistry.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>


struct ImGuiContext;


namespace svms::cfg {

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

    // Starts the top bar's "Applied live" chip (any live send).
    void NoteLiveChange();
    void Navigate(NavPage page);
    // Name of the process using SVMS ("PID n" when it cannot be resolved).
    std::string ConnectedAppName();
    void ShowToast(const std::string& message, float seconds = 2.2f);

private:

    bool CreateImGui();
    void RecreateFonts(float scale, bool rendererBackendInitialized);

    void DrawTopBar();
    void DrawSidebar();
    void DrawSearchResults();
    void DrawSaveBar(ImVec2 areaMin, ImVec2 areaMax);
    void DrawPageContent();
    void DrawToastOverlay(ImVec2 areaMin, ImVec2 areaMax);
    void HandleKeyboardShortcuts();
    void JumpTo(const SettingDef& def);
    void SaveConfig();

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

    NavPage page_ = NavPage::Home;
    bool pageEntered_ = true;
    char search_[128] = {};
    bool searchActive_ = false;
    bool resultsHovered_ = false;
    bool searchEscaped_ = false;
    ImVec2 searchMin_{}, searchMax_{};
    double liveChipTime_ = -10.0;
    double toastStart_ = -10.0;
    std::string toastShown_;
    float outL_ = 0.0f, outR_ = 0.0f;
    uint32_t appNamePid_ = 0;
    std::string appName_;
    std::string effectsPanel_ = "reverb";   // chain selection (Phase 3)

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
