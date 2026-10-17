#include "ConfiguratorApp.h"
#include "Platform.h"
#include "Theme.h"
#include "Widgets.h"
#include "ModernWidgets.h"
#include "Tour.h"
#include "Anim.h"
#include "GeistFonts.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "PlatformUtil.h"  // HWND (opaque on Linux) for the page draw calls

#include "PageHome.h"
#include "PageSound.h"
#include "PageOutput.h"
#include "PagePerformance.h"
#include "PageMidi.h"
#include "PageOffline.h"
#include "PageLiveRecording.h"
#include "PageEffects.h"
#include "PageDiagnostics.h"
#include "PageSettings.h"
#ifndef _WIN32
#include "PageDriverFeatures.h"
#endif

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <vector>

namespace svms::cfg {

enum class VersionRelation { Unknown, Same, DriverOlder, DriverNewer };

static VersionRelation CompareDriverVersion(
    const svms::RuntimeLinkClient::HostInfo& peer) {
    if (!peer.hasVersionIdentity) return VersionRelation::Unknown;
    const uint32_t driver[3] = {
        peer.productMajor, peer.productMinor, peer.productPatch };
    const uint32_t tool[3] = {
        svms::build::kProductMajor, svms::build::kProductMinor,
        svms::build::kProductPatch };
    for (uint32_t i = 0u; i < 3u; ++i) {
        if (driver[i] < tool[i]) return VersionRelation::DriverOlder;
        if (driver[i] > tool[i]) return VersionRelation::DriverNewer;
    }
    // Build zero denotes a local/developer build and is deliberately not
    // ordered relative to a numbered release of the same product version.
    if (peer.buildNumber != 0u && svms::build::kBuildNumber != 0u) {
        if (peer.buildNumber < svms::build::kBuildNumber)
            return VersionRelation::DriverOlder;
        if (peer.buildNumber > svms::build::kBuildNumber)
            return VersionRelation::DriverNewer;
    }
    return VersionRelation::Same;
}

bool ConfiguratorApp::Initialize(int argc, char** argv) {
    platform_ = CreatePlatform();

    easterEggs_ = RollEasterEggs(argc, argv);
    showMegaFuckerPopup_ = easterEggs_.megaFuckerDac;

    std::wstring configPath;
    bool configPathSpecified = false;
    uint32_t runtimeLinkPid = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            configPath = platform_->Utf8ToWide(argv[i + 1]);
            if (!configPath.empty()) configPathSpecified = true;
        }
        if (std::string(argv[i]) == "--runtime-link" && i + 1 < argc) {
            runtimeLinkPid = static_cast<uint32_t>(std::atoi(argv[i + 1]));
        }
    }

    if (!configPathSpecified) {
        config_.LoadDefaults();
        const std::wstring autoPath = platform_->DefaultConfigPath();
        if (!autoPath.empty()) {
            if (platform_->FileExists(autoPath)) {
                config_.Load(autoPath);
            } else {
                config_.LoadDefaults();
                config_.SetActivePath(autoPath);
            }
        }
    } else {
        config_.Load(configPath);
    }
    SeedWorkingLive();

    Platform::Hooks hooks;
    hooks.hasUnsavedChanges = [this] { return config_.IsDirty(); };
    if (!platform_->CreateWindowAndDevice(hooks, lastInitError_)) return false;
    dpiScale_ = platform_->DpiScale();
    if (!CreateImGui()) return false;
    ApplyTheme();

    if (runtimeLinkPid > 0) {
        rlConnected_ = rlClient_.Open(runtimeLinkPid);
        if (rlConnected_) {
            rlLastKnownPid_ = runtimeLinkPid;
            OnConnected();
        } else {
            statusMessage_ = "Failed to connect to driver PID " +
                             std::to_string(runtimeLinkPid);
        }
    } else {
        if (TryAutoDiscoverDriver()) {
            OnConnected();
        }
    }

    running_ = true;
    if (statusMessage_.empty())
        statusMessage_ = "Configuration loaded";

    return true;
}

void ConfiguratorApp::Shutdown() {
    if (shutdownDone_) return;
    shutdownDone_ = true;

    rlClient_.Close();
    rlConnected_ = false;
    offlineRendererPage_.Shutdown();

    if (platform_) {
        platform_->ShutdownBackends();
        ImGui::DestroyContext();
        platform_->DestroyWindowAndDevice();
    }
    running_ = false;
}

bool ConfiguratorApp::CreateImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ApplyTheme();

    RecreateFonts(dpiScale_, false);

    return platform_->InitBackends(lastInitError_);
}

void ConfiguratorApp::RecreateFonts(float scale, bool rendererBackendInitialized) {
    if (rendererBackendInitialized) {
        platform_->InvalidateFontObjects();
    }

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.PixelSnapH = true;
    cfg.FontDataOwnedByAtlas = false;   // embedded arrays / member buffers

    const float fontSize = 14.0f * scale;
    auto embedded = [&](const unsigned char* data, size_t size) {
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data),
                                              static_cast<int>(size), fontSize, &cfg);
    };
    UiFonts f;
    f.ui = embedded(kGeist_Regular, sizeof(kGeist_Regular));
    f.medium = embedded(kGeist_Medium, sizeof(kGeist_Medium));
    f.semibold = embedded(kGeist_SemiBold, sizeof(kGeist_SemiBold));
    f.bold = embedded(kGeist_Bold, sizeof(kGeist_Bold));
    f.mono = embedded(kGeistMono_Regular, sizeof(kGeistMono_Regular));
    f.monoMedium = embedded(kGeistMono_Medium, sizeof(kGeistMono_Medium));

    // Fallback: the system UI / mono fonts the configurator used before.
    if (!f.ui && platform_->LoadSystemFont(false, fontData_)) {
        f.ui = io.Fonts->AddFontFromMemoryTTF(
            fontData_.data(), static_cast<int>(fontData_.size()), fontSize, &cfg);
    }
    if (!f.ui) f.ui = io.Fonts->AddFontDefault();
    if (!f.mono && platform_->LoadSystemFont(true, monoFontData_)) {
        f.mono = io.Fonts->AddFontFromMemoryTTF(
            monoFontData_.data(), static_cast<int>(monoFontData_.size()), fontSize, &cfg);
    }
    if (!f.medium) f.medium = f.ui;
    if (!f.semibold) f.semibold = f.medium;
    if (!f.bold) f.bold = f.semibold;
    if (!f.monoMedium) f.monoMedium = f.mono;
    Fonts() = f;
    io.FontDefault = f.ui;
    SetMonoFont(f.mono);
    // No Fonts->Build(): imgui 1.92+ (RendererHasTextures) builds the atlas on demand.
}

bool ConfiguratorApp::PumpMessages() {
    if (!platform_->PumpEvents()) {
        running_ = false;
        return false;
    }
    return running_;
}

bool ConfiguratorApp::IsMinimized() const {
    return platform_ && platform_->IsMinimized();
}

void ConfiguratorApp::WaitForEvent() { platform_->WaitForEvent(); }

void ConfiguratorApp::SetWindowTitle(const wchar_t* title) {
    if (platform_) platform_->SetTitle(title);
}

namespace {

constexpr float kTopBarH = 60.0f;
constexpr float kSidebarW = 232.0f;
constexpr float kPi = 3.14159265358979f;

struct NavItem {
    NavPage page;
    const char* label;
    const char* icon;
};

// Labels and stroke icons from the prototype's NAV / TOOLS / GEAR tables.
const NavItem kNavMain[] = {
    {NavPage::Home, "Home", "M3 11l9-7 9 7v9a1 1 0 0 1-1 1h-5v-6H9v6H4a1 1 0 0 1-1-1z"},
    {NavPage::Sound, "Sound", "M9 18V5l12-2v13M9 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0zM21 16a3 3 0 1 1-6 0 3 3 0 0 1 6 0z"},
    {NavPage::Effects, "Effects", "M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"},
    {NavPage::Output, "Output", "M11 5L6 9H2v6h4l5 4V5zM15.5 8.5a5 5 0 0 1 0 7M19 5a10 10 0 0 1 0 14"},
    {NavPage::Performance, "Performance", "M13 2L3 14h9l-1 8 10-12h-9l1-8z"},
    {NavPage::Midi, "MIDI", "M2 6h20v12H2zM7 6v7M12 6v7M17 6v7"},
};
const NavItem kNavTools[] = {
    {NavPage::Render, "Render to file", "M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9zM14 3v6h6M12 12v6M9 15l3 3 3-3"},
    {NavPage::Record, "Record", "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z"},
    {NavPage::Diagnostics, "Diagnostics", "M22 12h-4l-3 9L9 3l-3 9H2"},
#ifndef _WIN32
    {NavPage::DriverFeatures, "Driver features", "M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"},
#endif
};
const NavItem kNavSettings = {NavPage::Settings, "Settings",
    "M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"};

struct PageHeader {
    NavPage page;
    const char* title;
    const char* subtitle;
};
const PageHeader kPageHeaders[] = {
    {NavPage::Home, "Home", "What SVMS is doing right now, and the switches you will touch most."},
    {NavPage::Sound, "Sound", "Which instruments play, and how hard notes hit."},
    {NavPage::Effects, "Effects", "Everything after the synth, in the order the sound flows. Click a block to edit it."},
    {NavPage::Output, "Output", "Where the sound comes out, and how much delay you accept for stability."},
    {NavPage::Performance, "Performance", "How many notes SVMS can carry, and which shortcuts it may take when the CPU runs out."},
    {NavPage::Midi, "MIDI", "A hardware keyboard, and how SVMS copes when a file sends more events than it can take."},
    {NavPage::Render, "Render to file", "Turn a MIDI file into a WAV, faster than realtime, with the settings you have now."},
    {NavPage::Record, "Record", "Capture exactly what you hear (after reverb and limiter) to a WAV file."},
    {NavPage::Diagnostics, "Diagnostics", "For bug reports. Copy the report and paste it into an issue."},
    {NavPage::Settings, "Settings", "How this window looks, where the config lives, and about SVMS."},
    {NavPage::DriverFeatures, "Driver features", nullptr},
};

ImU32 HexU32(unsigned rgb) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

// Render-load colour tiers: 0-100 accent, 100-200 to red, 200-350 to
// orange, 350-500 to hot cyan, 500+ cyan (VISION §8).
ImU32 LoadColor(float c, ImU32 acc) { return ui::LoadColor(c, acc); }

std::string WithCommas(uint32_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

float EaseOutBack(float t) {
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// A no-input window right below an overlay window that only carries its soft
// shadow, so the shadow can spill outside the overlay's own clip rect.
void OverlayShadow(const char* id, ImVec2 min, ImVec2 max, float rounding,
                   ImU32 color, float spread, float alpha) {
    const ImVec2 pad(spread, spread);
    ImGui::SetNextWindowPos(ImVec2(min.x - pad.x, min.y - pad.y));
    ImGui::SetNextWindowSize(ImVec2(max.x - min.x + 2.0f * pad.x, max.y - min.y + 2.0f * pad.y));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ui::SoftShadow(ImGui::GetWindowDrawList(), min, max, rounding,
                   ui::WithAlpha(color, alpha), spread * 0.8f, 10);
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace

void ConfiguratorApp::NoteLiveChange() {
    if (rlConnected_) liveChipTime_ = ImGui::GetTime();
}

void ConfiguratorApp::ShowToast(const std::string& message, float seconds) {
    toastMessage_ = message;
    toastTimer_ = seconds;
}

void ConfiguratorApp::Navigate(NavPage page) {
    if (page == page_) return;
    page_ = page;
    pageEntered_ = true;
}

void ConfiguratorApp::JumpTo(const SettingDef& def) {
    Navigate(def.page);
    pageEntered_ = true;
    if (def.disclosure) ui::DisclosureState(def.disclosure) = true;
    if (def.fxPanel) effectsPanel_ = def.fxPanel;
    ui::FlashAnchor(def.id);
    search_[0] = '\0';
    searchActive_ = false;
    resultsHovered_ = false;
    ImGui::ClearActiveID();
}

std::string ConfiguratorApp::ConnectedAppName() {
    const uint32_t pid = rlClient_.GetPID();
    if (pid == appNamePid_ && !appName_.empty()) return appName_;
    appNamePid_ = pid;
    appName_ = "PID " + std::to_string(pid);
#ifdef _WIN32
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
            const wchar_t* name = std::wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            char utf8[MAX_PATH * 3] = {};
            if (WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr) > 0)
                appName_ = utf8;
        }
        CloseHandle(h);
    }
#endif
    return appName_;
}

void ConfiguratorApp::RenderFrame() {
    platform_->ApplyPendingResize();

    float newScale = 0.0f;
    if (platform_->TakeDpiChange(newScale)) {
        RecreateFonts(newScale, true);
    }

    if (!platform_->CanRender()) return;
    windowWidth_ = platform_->Width();
    windowHeight_ = platform_->Height();
    dpiScale_ = platform_->DpiScale();

    HandleKeyboardShortcuts();
    PollRuntimeLink();

    // Rack uses the mono face for its whole UI.
    ImGui::GetIO().FontDefault = IsModernStyle() || !Fonts().mono ? Fonts().ui : Fonts().mono;

    platform_->NewFrame();
    ImGui::NewFrame();
    ui::BeginFrame(dpiScale_);
    const ui::Tokens& k = ui::Tok();
    const float s = dpiScale_;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowWidth_),
                                     static_cast<float>(windowHeight_)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, k.bg);

    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    DrawTopBar();
    DrawSidebar();

    const ImVec2 areaMin(kSidebarW * s, kTopBarH * s);
    const ImVec2 areaMax(static_cast<float>(windowWidth_), static_cast<float>(windowHeight_));
    if (pageEntered_) {
        ui::PageEntered();
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::SetCursorScreenPos(areaMin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(34.0f * s, 30.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0f * s);
    ImGui::BeginChild("##page", ImVec2(areaMax.x - areaMin.x, areaMax.y - areaMin.y),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    ui::SetScrollHost();
    for (const PageHeader& h : kPageHeaders) {
        if (h.page != page_) continue;
        ui::PushText(2, k.rack ? 22.0f : 26.0f);
        ImGui::TextUnformatted(ui::Caps(h.title));
        ui::PopFont();
        if (h.subtitle) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f * s - ImGui::GetStyle().ItemSpacing.y);
            ui::PushText(0, 14.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
            ImGui::TextWrapped("%s", h.subtitle);
            ImGui::PopStyleColor();
            ui::PopFont();
        }
        ImGui::Dummy(ImVec2(0.0f, 16.0f * s - 2.0f * ImGui::GetStyle().ItemSpacing.y));
    }
    const float bodyW = (std::min)(980.0f * s, ImGui::GetContentRegionAvail().x);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##body", ImVec2(bodyW, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    DrawPageContent();
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.0f, 120.0f * s));   // room above the floating save bar
    ImGui::EndChild();
    pageEntered_ = false;

    // Esc is "back to Home" unless something is being edited or a menu is open.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !searchEscaped_ && !ImGui::IsAnyItemActive() &&
        !ImGui::GetIO().WantTextInput && !tour::Active() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        Navigate(NavPage::Home);
    }
    searchEscaped_ = false;

    ImGui::End();
    ImGui::PopStyleVar(3);

    DrawSearchResults();
    DrawSaveBar(areaMin, areaMax);
    ShowMegaFuckerNotification(easterEggs_, &showMegaFuckerPopup_);
    DrawToastOverlay(areaMin, areaMax);
    if (!tourChecked_) {
        tourChecked_ = true;
        if (GetThemeSettings().tourAtStart) tour::Start();
    }
    tour::Draw(*this);

    ImGui::Render();
    const ImVec4 clear = GetThemeSettings().background;
    const float clearColor[4] = { clear.x, clear.y, clear.z, clear.w };
    platform_->Present(clearColor);

    // Present the click/drag first, then perform the cross-process update.
    // This prevents a slow or temporarily busy driver from making the button
    // itself feel like it ignored the user. Normally the ACK arrives almost
    // immediately; the 50 ms timeout is only a safety ceiling.
    FlushLiveChanges();
}

static float MasterVolumeDb(float v) {
    return 20.0f * std::log10((std::max)(v, 0.001f));
}

static float MasterVolumeFromDb(float db) {
    return std::pow(10.0f, db / 20.0f);
}

void ConfiguratorApp::DrawTopBar() {
    const ui::Tokens& k = ui::Tok();
    const float s = dpiScale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = static_cast<float>(windowWidth_);
    const float H = kTopBarH * s;
    const float cy = H * 0.5f;
    const double now = ImGui::GetTime();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), k.side);
    dl->AddLine(ImVec2(0, H - 0.5f), ImVec2(W, H - 0.5f), k.line);

    // Brand: four EQ bars (moving only while connected), name, version.
    float x = 18.0f * s;
    const float amp = AnimEase(ImGui::GetID("##eqamp"), rlConnected_ ? 1.0f : 0.0f, 0.3f);
    static const float kPhase[4] = {0.0f, -0.3f, -0.6f, -0.15f};
    for (int i = 0; i < 4; ++i) {
        float wave = 0.4f;
        if (GetMotion() != Motion::Off) {
            const float ph = static_cast<float>(std::fmod(now - kPhase[i] + 10.0, 1.1) / 1.1);
            wave = 0.3f + 0.7f * (0.5f - 0.5f * std::cos(2.0f * kPi * ph));
        }
        const float h = (0.4f + amp * (wave - 0.4f)) * 18.0f * s;
        const float bx = x + i * 6.0f * s;
        dl->AddRectFilled(ImVec2(bx, cy + 9.0f * s - h), ImVec2(bx + 4.0f * s, cy + 9.0f * s),
                          k.acc, 2.0f * s);
    }
    ui::PushText(3, 16.0f);
    float tx = x + 32.0f * s;
    const char* brand = "SVMS";
    ImVec2 ts = ImGui::CalcTextSize(brand);
    dl->AddText(ImVec2(tx, cy - ts.y * 0.5f), k.text, brand);
    tx += ts.x + 10.0f * s;
    ui::PopFont();
    char version[64];
    std::snprintf(version, sizeof(version), "V3 \xC2\xB7 %s", svms::build::kProductVersion);
    ui::PushNum(12.0f);
    ts = ImGui::CalcTextSize(version);
    dl->AddText(ImVec2(tx, cy - ts.y * 0.5f), k.muted, version);
    ui::PopFont();
    x += 196.0f * s + 18.0f * s;

    // Connection pill: click to connect / disconnect.
    std::string connText = "No app connected";
    std::string connTip;
    if (rlConnected_) {
        connText = "Connected \xC2\xB7 " + ConnectedAppName();
        const VersionRelation relation = CompareDriverVersion(rlClient_.GetPeerInfo());
        if (relation == VersionRelation::DriverOlder)
            connTip = "The driver is older than this configurator. Some newer settings are not available.";
        else if (relation == VersionRelation::DriverNewer)
            connTip = "The driver is newer than this configurator. Running in compatibility mode.";
    }
    ui::PushText(0, 13.0f);
    ts = ImGui::CalcTextSize(connText.c_str());
    const ImVec2 pillSize(14.0f * s + 8.0f * s + 8.0f * s + ts.x + 14.0f * s, 32.0f * s);
    const ImVec2 pillPos(x, cy - pillSize.y * 0.5f);
    ImGui::SetCursorScreenPos(pillPos);
    if (ImGui::InvisibleButton("##conn", pillSize)) {
        if (rlConnected_) {
            rlClient_.Close();
            rlConnected_ = false;
            statusMessage_ = "Disconnected from driver";
            ShowToast(statusMessage_);
        } else if (TryAutoDiscoverDriver()) {
            OnConnected();
        } else {
            ShowToast("No running SVMS driver found");
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(rlConnected_ ? "Click to disconnect." : "Click to look for a running SVMS.");
        if (!connTip.empty()) ImGui::TextUnformatted(connTip.c_str());
        ImGui::EndTooltip();
    }
    {
        const float sc = AnimSpring(ImGui::GetID("##connpress"), ImGui::IsItemActive() ? 0.95f : 1.0f);
        const ImVec2 c(pillPos.x + pillSize.x * 0.5f, cy);
        const ImVec2 a(c.x - pillSize.x * 0.5f * sc, c.y - pillSize.y * 0.5f * sc);
        const ImVec2 b(c.x + pillSize.x * 0.5f * sc, c.y + pillSize.y * 0.5f * sc);
        const float on = AnimEase(ImGui::GetID("##connon"), rlConnected_ ? 1.0f : 0.0f, 0.3f);
        const ImU32 warn = connTip.empty() ? k.ok : HexU32(0xFFB23E);
        dl->AddRect(a, b, ui::MixU32(k.line2, ui::WithAlpha(warn, 0.4f), on),
                    k.rack ? 3.0f * s : pillSize.y * 0.5f);
        const ImVec2 dot(a.x + 14.0f * s * sc + 4.0f * s, cy);
        if (rlConnected_ && GetMotion() != Motion::Off) {
            const float ph = static_cast<float>(std::fmod(now, 1.8) / 1.8);
            const float e = EaseOut(ph);
            dl->AddCircle(dot, 8.0f * s * (0.5f + 1.3f * e), ui::WithAlpha(warn, 0.9f * (1.0f - e)),
                          24, 2.0f * s);
        }
        dl->AddCircleFilled(dot, 4.0f * s, ui::MixU32(HexU32(0x6E6E7A), warn, on), 16);
        dl->AddText(ImVec2(dot.x + 12.0f * s, cy - ts.y * 0.5f),
                    ui::MixU32(k.muted, k.text, on), connText.c_str());
    }
    ui::PopFont();
    x += pillSize.x + 18.0f * s;

    // "Applied live" chip, ~0.9 s after any live change.
    const bool liveShow = now - liveChipTime_ < 0.9;
    const float liveA = AnimEase(ImGui::GetID("##livea"), liveShow ? 1.0f : 0.0f, 0.25f);
    const float liveY = AnimSpring(ImGui::GetID("##livey"), liveShow ? 0.0f : -4.0f);
    ui::PushText(0, 12.0f);
    const char* liveText = "Applied live";
    ts = ImGui::CalcTextSize(liveText);
    if (liveA > 0.01f) {
        const ImU32 col = ui::WithAlpha(k.ok, liveA);
        ui::StrokeIcon(dl, "M20 6L9 17l-5-5", ImVec2(x, cy - 7.0f * s + liveY * s), 14.0f * s, col, 2.2f * s);
        dl->AddText(ImVec2(x + 20.0f * s, cy - ts.y * 0.5f + liveY * s), col, liveText);
    }
    const float liveW = 20.0f * s + ts.x;
    ui::PopFont();
    x += liveW + 18.0f * s;

    // Master volume (right edge).
    auto& w = config_.Working();
    ui::PushText(0, 12.0f);
    const float labelW = ImGui::CalcTextSize("Master").x;
    ui::PopFont();
    const float sliderW = 110.0f * s;
    const float readW = 58.0f * s;
    const float masterW = 18.0f * s + labelW + 10.0f * s + sliderW + 10.0f * s + readW;
    const float masterX = W - 18.0f * s - masterW;
    dl->AddLine(ImVec2(masterX, cy - 16.0f * s), ImVec2(masterX, cy + 16.0f * s), k.line);
    ui::PushText(0, 12.0f);
    ts = ImGui::CalcTextSize("Master");
    dl->AddText(ImVec2(masterX + 18.0f * s, cy - ts.y * 0.5f), k.muted, "Master");
    ui::PopFont();
    float db = (std::max)(-24.0f, (std::min)(6.0f, MasterVolumeDb(w.masterVolume)));
    ImGui::SetCursorScreenPos(ImVec2(masterX + 18.0f * s + labelW + 10.0f * s, cy - 11.0f * s));
    if (ui::RangeSlider("##master", &db, -24.0f, 6.0f, sliderW)) {
        db = std::round(db * 2.0f) * 0.5f;
        const float lin = MasterVolumeFromDb(db);
        if (!AlmostEquals(lin, w.masterVolume)) {
            w.masterVolume = lin;
            config_.MarkDirty();
            SetLiveFloat(svms::RLCommandType::SetMasterVolume, lin);
        }
    }
    char dbText[32];
    const float shownDb = MasterVolumeDb(w.masterVolume);
    std::snprintf(dbText, sizeof(dbText), "%s%.1f dB", shownDb > 0.05f ? "+" : "", shownDb);
    ui::PushNum(12.0f, true);
    ts = ImGui::CalcTextSize(dbText);
    dl->AddText(ImVec2(W - 18.0f * s - ts.x, cy - ts.y * 0.5f), k.text, dbText);
    ui::PopFont();

    // Meters: Voices, CPU (laps past 100%), Output (stereo).
    const float meterGap = 18.0f * s;
    const float meterAvail = masterX - meterGap - x;
    const float meterW = (std::min)(150.0f * s, (meterAvail - 2.0f * meterGap) / 3.0f);
    if (meterW < 60.0f * s) return;
    float mx = masterX - meterGap - (3.0f * meterW + 2.0f * meterGap);
    tour::Register("t-meters", ImVec2(mx - 10.0f * s, 8.0f * s), ImVec2(mx + 3.0f * meterW + 2.0f * meterGap + 10.0f * s, H - 8.0f * s));
    const bool tel = rlConnected_;
    const float dt = ImGui::GetIO().DeltaTime;
    auto follow = [&](float& v, float target) {
        v += (target - v) * (1.0f - std::exp(-(target > v ? 30.0f : 6.0f) * dt));
    };
    follow(outL_, tel ? rlTelemetry_.limiterOutputPeakL : 0.0f);
    follow(outR_, tel ? rlTelemetry_.limiterOutputPeakR : 0.0f);

    auto meterHead = [&](float mx0, const char* label, const char* value) {
        ui::PushText(0, 12.0f);
        const float lh = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(mx0, cy - (lh + 11.0f * s) * 0.5f), k.muted, label);
        ui::PopFont();
        ui::PushNum(12.0f, true);
        const ImVec2 vs = ImGui::CalcTextSize(value);
        dl->AddText(ImVec2(mx0 + meterW - vs.x, cy - (lh + 11.0f * s) * 0.5f), k.text, value);
        ui::PopFont();
        return cy - (lh + 11.0f * s) * 0.5f + lh + 5.0f * s;
    };
    const char* dash = "\xE2\x80\x94";
    // Voices
    const uint32_t cap = tel && rlTelemetry_.maxVoices ? rlTelemetry_.maxVoices : (std::max)(1u, w.maxVoices);
    std::string voices = tel ? WithCommas(rlTelemetry_.activeVoices) : dash;
    float by = meterHead(mx, "Voices", voices.c_str());
    dl->AddRectFilled(ImVec2(mx, by), ImVec2(mx + meterW, by + 6.0f * s), k.card2, 3.0f * s);
    const float vf = tel ? (std::min)(1.0f, static_cast<float>(rlTelemetry_.activeVoices) / cap) : 0.0f;
    if (vf > 0.0f) dl->AddRectFilled(ImVec2(mx, by), ImVec2(mx + meterW * vf, by + 6.0f * s), k.acc, 3.0f * s);
    mx += meterW + meterGap;
    // CPU
    char cpuText[32];
    const float load = AnimEase(ImGui::GetID("##topload"), tel ? (std::max)(0.0f, rlTelemetry_.cpuLoadPercent) : 0.0f, 0.3f);
    if (tel) std::snprintf(cpuText, sizeof(cpuText), "%.0f%%", load);
    else std::snprintf(cpuText, sizeof(cpuText), "%s", dash);
    by = meterHead(mx, "CPU", cpuText);
    const bool over = load >= 500.0f;
    const int lap = static_cast<int>((std::min)(load, 499.99f) / 100.0f);
    const float frac = over ? 1.0f : (load - lap * 100.0f) / 100.0f;
    dl->AddRectFilled(ImVec2(mx, by), ImVec2(mx + meterW, by + 6.0f * s),
                      lap > 0 ? LoadColor(lap * 100.0f, k.acc) : k.card2, 3.0f * s);
    if (frac > 0.0f)
        dl->AddRectFilled(ImVec2(mx, by), ImVec2(mx + meterW * frac, by + 6.0f * s), LoadColor(load, k.acc), 3.0f * s);
    mx += meterW + meterGap;
    // Output
    char outText[32];
    const float peak = (std::max)(outL_, outR_);
    if (!tel) std::snprintf(outText, sizeof(outText), "%s", dash);
    else if (peak <= 0.0001f) std::snprintf(outText, sizeof(outText), "\xE2\x88\x92\xE2\x88\x9E dB");
    else std::snprintf(outText, sizeof(outText), "%.1f dB", 20.0f * std::log10(peak));
    by = meterHead(mx, "Output", outText);
    dl->AddRectFilled(ImVec2(mx, by), ImVec2(mx + meterW, by + 6.0f * s), k.card2, 3.0f * s);
    auto stereoBar = [&](float y0, float v) {
        const float fill = meterW * (std::min)(1.0f, v / 1.3f);
        const float segs[3][2] = {{0.0f, 0.7f}, {0.7f, 0.9f}, {0.9f, 1.0f}};
        const ImU32 cols[3] = {k.ok, k.acc, k.bad};
        for (int i = 0; i < 3; ++i) {
            const float a = meterW * segs[i][0], b = (std::min)(fill, meterW * segs[i][1]);
            if (b > a) dl->AddRectFilled(ImVec2(mx + a, y0), ImVec2(mx + b, y0 + 2.5f * s), cols[i]);
        }
    };
    stereoBar(by, tel ? outL_ : 0.0f);
    stereoBar(by + 3.5f * s, tel ? outR_ : 0.0f);
}

void ConfiguratorApp::DrawSidebar() {
    const ui::Tokens& k = ui::Tok();
    const float s = dpiScale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float top = kTopBarH * s;
    const float W = kSidebarW * s;
    const float H = static_cast<float>(windowHeight_);
    dl->AddRectFilled(ImVec2(0, top), ImVec2(W, H), k.side);
    dl->AddLine(ImVec2(W - 0.5f, top), ImVec2(W - 0.5f, H), k.line);

    // Search field.
    const float x0 = 12.0f * s;
    const float innerW = W - 24.0f * s;
    float y = top + 14.0f * s;
    {
        ui::PushText(0, 13.0f);
        const float fieldH = 36.0f * s;
        const float fontH = ImGui::GetFontSize();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(34.0f * s, (fieldH - fontH) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, k.rack ? 3.0f * s : 10.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, k.card);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, k.card);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, k.card);
        ImGui::PushStyleColor(ImGuiCol_Border, k.line2);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, k.muted);
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::SetNextItemWidth(innerW);
        ImGui::InputTextWithHint("##search", "Find a setting\xE2\x80\xA6", search_, sizeof(search_));
        searchActive_ = ImGui::IsItemActive();
        searchMin_ = ImGui::GetItemRectMin();
        searchMax_ = ImGui::GetItemRectMax();
        tour::Register("t-search", searchMin_, searchMax_);
        if ((searchActive_ || ImGui::IsItemDeactivated()) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            search_[0] = '\0';
            searchEscaped_ = true;   // Esc here clears the search, not the page
        }
        if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && search_[0]) {
            const SettingDef* hits[1];
            if (SearchSettings(search_, hits, 1) == 1) JumpTo(*hits[0]);
        }
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);
        ui::PopFont();
        const float focus = AnimEase(ImGui::GetID("##searchfocus"), searchActive_ ? 1.0f : 0.0f, 0.25f);
        if (focus > 0.01f) {
            const float r = k.rack ? 3.0f * s : 10.0f * s;
            dl->AddRect(ImVec2(searchMin_.x - 2.0f * s, searchMin_.y - 2.0f * s),
                        ImVec2(searchMax_.x + 2.0f * s, searchMax_.y + 2.0f * s),
                        ui::WithAlpha(k.accSoft, focus), r + 2.0f * s, 0, 4.0f * s);
            dl->AddRect(searchMin_, searchMax_, ui::WithAlpha(k.acc, focus), r);
        }
        ui::StrokeIcon(dl, "M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14zM20 20l-3.5-3.5",
                       ImVec2(searchMin_.x + 11.0f * s, searchMin_.y + 10.0f * s), 16.0f * s,
                       k.muted, 2.0f * s);
        y += 36.0f * s + 14.0f * s;
    }

    const ChangeSummary changes = SummarizeChanges(config_.Working(), config_.Loaded());
    const float itemH = 38.0f * s;
    const float step = 40.0f * s;
    const float groupH = 30.0f * s;

    auto drawItem = [&](const NavItem& item, float iy) {
        ImGui::PushID(static_cast<int>(item.page));
        ImGui::SetCursorScreenPos(ImVec2(x0, iy));
        if (ImGui::InvisibleButton("##nav", ImVec2(innerW, itemH))) Navigate(item.page);
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool act = page_ == item.page;
        if (hov && !act)
            dl->AddRectFilled(ImVec2(x0, iy), ImVec2(x0 + innerW, iy + itemH),
                              IM_COL32(255, 255, 255, 8), k.rack ? 3.0f * s : 10.0f * s);
        const ImU32 ink = act || hov ? k.text : k.muted;
        const float rot = AnimSpring(ImGui::GetID("##wr"), hov ? -8.0f : 0.0f) * kPi / 180.0f;
        const float sc = AnimSpring(ImGui::GetID("##ws"), hov ? 1.12f : 1.0f);
        ui::StrokeIcon(dl, item.icon, ImVec2(x0 + 14.0f * s, iy + (itemH - 18.0f * s) * 0.5f),
                       18.0f * s, act ? k.acc : ink, 1.8f * s, rot, sc);
        ui::PushText(act ? 1 : 0, 14.0f);
        const char* label = ui::Caps(item.label);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(x0 + 43.0f * s, iy + (itemH - ts.y) * 0.5f), ink, label);
        ui::PopFont();
        // Unsaved-change badge, pops in when it appears.
        const int count = changes.perPage[static_cast<int>(item.page)];
        ImGuiStorage* st = ImGui::GetStateStorage();
        const ImGuiID prevId = ImGui::GetID("##badgeprev");
        if (count > 0 && st->GetInt(prevId, 0) == 0) AnimTrigger(ImGui::GetID("##badgepop"));
        st->SetInt(prevId, count);
        if (count > 0) {
            char num[16];
            std::snprintf(num, sizeof(num), "%d", count);
            ui::PushNum(11.0f);
            const ImVec2 ns = ImGui::CalcTextSize(num);
            const float bw = (std::max)(18.0f * s, ns.x + 10.0f * s);
            const float pop = EaseOutBack(AnimPulse(ImGui::GetID("##badgepop"), 0.4f));
            const ImVec2 c(x0 + innerW - 14.0f * s - bw * 0.5f, iy + itemH * 0.5f);
            const float hw = bw * 0.5f * pop, hh = 9.0f * s * pop;
            if (pop > 0.01f) {
                dl->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), k.acc, hh);
                if (pop > 0.6f)
                    dl->AddText(ImVec2(c.x - ns.x * 0.5f, c.y - ns.y * 0.5f), k.onAcc, num);
            }
            ui::PopFont();
        }
        ImGui::PopID();
    };

    // Spring indicator behind the active item (main list and tools).
    const float listTop = y;
    float indY = -1.0f;
    for (size_t i = 0; i < std::size(kNavMain); ++i)
        if (kNavMain[i].page == page_) indY = listTop + step * static_cast<float>(i);
    for (size_t i = 0; i < std::size(kNavTools); ++i)
        if (kNavTools[i].page == page_)
            indY = listTop + step * static_cast<float>(std::size(kNavMain) + i) + groupH;
    const float indAlpha = AnimEase(ImGui::GetID("##indA"), indY >= 0.0f ? 1.0f : 0.0f, 0.2f);
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID lastIndId = ImGui::GetID("##indLast");
    if (indY < 0.0f) indY = st->GetFloat(lastIndId, listTop);
    st->SetFloat(lastIndId, indY);
    const float springY = listTop + AnimSpring(ImGui::GetID("##indY"), indY - listTop);
    if (indAlpha > 0.01f) {
        const float r = k.rack ? 3.0f * s : 10.0f * s;
        dl->AddRectFilled(ImVec2(x0, springY), ImVec2(x0 + innerW, springY + itemH),
                          ui::WithAlpha(k.accSoft, indAlpha), r);
        dl->AddRect(ImVec2(x0, springY), ImVec2(x0 + innerW, springY + itemH),
                    ui::WithAlpha(k.accLine, indAlpha), r);
    }

    tour::Register("t-nav", ImVec2(x0, y), ImVec2(x0 + innerW, y + step * static_cast<float>(std::size(kNavMain)) - (step - itemH)));
    for (const NavItem& item : kNavMain) {
        drawItem(item, y);
        y += step;
    }
    const float toolsTop = y;
    ui::PushText(0, 11.0f);
    const char* tools = "TOOLS";
    dl->AddText(ImVec2(x0 + 12.0f * s, y + groupH - 6.0f * s - ImGui::GetTextLineHeight()), k.muted, tools);
    ui::PopFont();
    y += groupH;
    for (const NavItem& item : kNavTools) {
        drawItem(item, y);
        y += step;
    }
    tour::Register("t-tools", ImVec2(x0, toolsTop), ImVec2(x0 + innerW, y - (step - itemH)));
    // Settings, pinned to the bottom.
    const float settingsY = (std::max)(y + 8.0f * s, H - 14.0f * s - itemH);
    if (page_ == NavPage::Settings) {
        const float r = k.rack ? 3.0f * s : 10.0f * s;
        dl->AddRectFilled(ImVec2(x0, settingsY), ImVec2(x0 + innerW, settingsY + itemH), k.accSoft, r);
        dl->AddRect(ImVec2(x0, settingsY), ImVec2(x0 + innerW, settingsY + itemH), k.accLine, r);
    }
    drawItem(kNavSettings, settingsY);
    tour::Register("t-settings", ImVec2(x0, settingsY), ImVec2(x0 + innerW, settingsY + itemH));
}

void ConfiguratorApp::DrawSearchResults() {
    const bool show = (searchActive_ || resultsHovered_) && search_[0] != '\0';
    if (!show) {
        resultsHovered_ = false;
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("##resultsOpen"), 0);
        return;
    }
    const ui::Tokens& k = ui::Tok();
    const float s = dpiScale_;
    const SettingDef* hits[7];
    const int n = SearchSettings(search_, hits, 7);
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID openId = ImGui::GetID("##resultsOpen");
    if (st->GetInt(openId, 0) == 0) AnimTrigger(ImGui::GetID("##resultsDrop"));
    st->SetInt(openId, 1);
    const float drop = EaseOut(AnimPulse(ImGui::GetID("##resultsDrop"), 0.22f));

    ui::PushText(1, 13.0f);
    const float l1 = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushText(0, 12.0f);
    const float l2 = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float rowH = 20.0f * s + l1 + l2;
    const float width = 340.0f * s;
    const float height = 12.0f * s + (n > 0 ? rowH * n : l1 + 20.0f * s);
    const ImVec2 pos(searchMin_.x, searchMax_.y + 6.0f * s - 6.0f * s * (1.0f - drop));
    OverlayShadow("##results_shadow", pos, ImVec2(pos.x + width, pos.y + height), 12.0f * s,
                  IM_COL32(0, 0, 0, 140), 30.0f * s, drop);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * s, 6.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, k.rack ? 3.0f * s : 12.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (std::max)(0.01f, drop));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, k.card2);
    ImGui::PushStyleColor(ImGuiCol_Border, k.line2);
    ImGui::Begin("##search_results", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (n == 0) {
        ui::PushText(0, 13.0f);
        ImGui::SetCursorPos(ImVec2(16.0f * s, 16.0f * s));
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::TextUnformatted("Nothing matches that. Try \xE2\x80\x9Creverb\xE2\x80\x9D, \xE2\x80\x9Clatency\xE2\x80\x9D or \xE2\x80\x9Cvoices\xE2\x80\x9D.");
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    const SettingDef* picked = nullptr;
    for (int i = 0; i < n; ++i) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float rw = width - 12.0f * s;
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##hit", ImVec2(rw, rowH))) picked = hits[i];
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rowH), k.accSoft, k.rack ? 2.0f * s : 8.0f * s);
        }
        ImGui::PopID();
        ui::PushText(1, 13.0f);
        dl->AddText(ImVec2(p.x + 12.0f * s, p.y + 10.0f * s), k.text, hits[i]->name);
        ui::PopFont();
        ui::PushText(0, 12.0f);
        dl->AddText(ImVec2(p.x + 12.0f * s, p.y + 10.0f * s + l1), k.muted, hits[i]->where);
        ui::PopFont();
    }
    // Stays open while a hit is being pressed (the input lost focus on the
    // mouse-down, and plain IsWindowHovered is false while an item is held).
    resultsHovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                      (ImGui::GetActiveID() != 0 && ImGui::IsWindowFocused());
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(5);
    if (picked) JumpTo(*picked);
}

void ConfiguratorApp::SaveConfig() {
    if (config_.IsReadOnly()) {
        ShowToast("This config was opened read-only to preserve its data", 4.0f);
        return;
    }
    const ChangeSummary changes = SummarizeChanges(config_.Working(), config_.Loaded());
    ConfigValidation v = config_.Validate();
    if (!v.valid) {
        ShowToast("Validation failed: " + v.warnings, 5.0f);
        return;
    }
    if (!config_.Save(config_.GetActivePath())) {
        ShowToast("Could not save configuration", 4.0f);
        return;
    }
    PushAllLiveParams();
    statusMessage_ = "Saved";
    if (changes.restart > 0)
        ShowToast("Saved. Restart the MIDI app for " + std::to_string(changes.restart) + " of them.");
    else
        ShowToast("Saved");
}

void ConfiguratorApp::DrawSaveBar(ImVec2 areaMin, ImVec2 areaMax) {
    const ui::Tokens& k = ui::Tok();
    const float s = dpiScale_;
    const ChangeSummary changes = SummarizeChanges(config_.Working(), config_.Loaded());
    const bool show = changes.total > 0;
    const float slide = AnimSpring(ImGui::GetID("##saveslide"), show ? 0.0f : 1.0f);
    const float alpha = AnimEase(ImGui::GetID("##savealpha"), show ? 1.0f : 0.0f, 0.25f);
    if (alpha <= 0.01f && !show) return;

    const float barH = 66.0f * s;
    const float left = areaMin.x + 34.0f * s;
    const float width = (std::min)(980.0f * s, areaMax.x - areaMin.x - 68.0f * s);
    const float y = areaMax.y - 18.0f * s - barH + slide * barH * 1.4f;
    const ImVec2 pos(left, y);
    OverlayShadow("##save_shadow", pos, ImVec2(left + width, y + barH), 14.0f * s,
                  IM_COL32(0, 0, 0, 150), 40.0f * s, alpha);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(width, barH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * s, 14.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, k.rack ? 4.0f * s : 14.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (std::max)(0.01f, alpha));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, k.card2);
    ImGui::PushStyleColor(ImGuiCol_Border, k.line2);
    ImGui::Begin("##savebar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = y + barH * 0.5f;
    ui::Glow(dl, ImVec2(left + 26.5f * s, cy), 4.5f * s, k.acc, 8.0f * s, 6);
    dl->AddCircleFilled(ImVec2(left + 26.5f * s, cy), 4.5f * s, k.acc, 16);
    float x = left + 43.0f * s;
    const int total = (std::max)(1, changes.total);
    const std::string t1 = total == 1 ? "1 unsaved change" : std::to_string(total) + " unsaved changes";
    ui::PushText(1, 14.0f);
    ImVec2 ts = ImGui::CalcTextSize(t1.c_str());
    dl->AddText(ImVec2(x, cy - ts.y * 0.5f), k.text, t1.c_str());
    ui::PopFont();
    x += ts.x + 12.0f * s;
    std::string t2;
    if (changes.restart > 0) t2 = "\xC2\xB7 " + std::to_string(changes.restart) + " of them apply after the app restarts";
    else if (rlConnected_) t2 = "\xC2\xB7 already playing live";
    if (config_.IsReadOnly()) t2 = "\xC2\xB7 this config is read-only";
    if (!t2.empty()) {
        ui::PushText(0, 13.0f);
        ts = ImGui::CalcTextSize(t2.c_str());
        dl->AddText(ImVec2(x, cy - ts.y * 0.5f), k.muted, t2.c_str());
        ui::PopFont();
    }
    const ImVec2 saveSize = ui::ButtonSize("Save");
    const ImVec2 discardSize = ui::ButtonSize("Discard");
    ImGui::SetCursorScreenPos(ImVec2(left + width - 16.0f * s - saveSize.x - 8.0f * s - discardSize.x,
                                     cy - saveSize.y * 0.5f));
    if (ui::Button("Discard")) {
        config_.Revert();
        PushAllLiveParams();
        statusMessage_ = "Configuration reverted";
        ShowToast("Changes discarded");
    }
    ImGui::SameLine(0.0f, 8.0f * s);
    if (ui::Button("Save", ui::ButtonKind::Primary, ImVec2(0, 0), !config_.IsReadOnly())) SaveConfig();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void ConfiguratorApp::DrawPageContent() {
    LiveLinkContext lc;
    lc.app = this;
    const bool liveSupported = rlConnected_ && rlClient_.HasCapability(
        svms::build::CapabilityLiveConfiguration);
    lc.client = liveSupported ? &rlClient_ : nullptr;
    lc.telemetry = rlConnected_ ? &rlTelemetry_ : nullptr;
    lc.connected = liveSupported;
    SetLiveLinkContext(lc);

    if (const int added = HandleDroppedSoundFonts(config_)) {
        ShowToast(added == 1 ? "Added 1 SoundFont" : "Added " + std::to_string(added) + " SoundFonts");
    }
    switch (page_) {
    case NavPage::Home:        DrawHomePage(config_); break;
    case NavPage::Sound:       DrawSoundPage(config_); break;
    case NavPage::Effects:
        DrawEffectsPage(config_, effectsPanel_);
        break;
    case NavPage::Output:      DrawOutputPage(config_, easterEggs_); break;
    case NavPage::Performance: DrawPerformancePage(config_); break;
    case NavPage::Midi:        DrawMidiPage(config_); break;
    case NavPage::Render:
        offlineRendererPage_.Draw(config_, static_cast<HWND>(platform_->NativeWindow()));
        break;
    case NavPage::Record:
        liveRecordingPage_.Draw(rlConnected_ ? &rlClient_ : nullptr,
                                rlConnected_, static_cast<HWND>(platform_->NativeWindow()));
        break;
    case NavPage::Diagnostics: DrawDiagnosticsPage(config_); break;
    case NavPage::Settings:
        DrawSettingsPage(config_, updateService_);
        break;
    case NavPage::DriverFeatures:
#ifndef _WIN32
        DrawDriverFeaturesPage(rlConnected_ ? &rlClient_ : nullptr);
#endif
        break;
    default:
        break;
    }
}

void ConfiguratorApp::DrawToastOverlay(ImVec2 areaMin, ImVec2 areaMax) {
    const ui::Tokens& k = ui::Tok();
    const float s = dpiScale_;
    if (toastTimer_ > 0.0f) {
        if (toastMessage_ != toastShown_) {
            toastShown_ = toastMessage_;
            toastStart_ = ImGui::GetTime();
        }
        toastTimer_ -= ImGui::GetIO().DeltaTime;
    }
    const bool show = toastTimer_ > 0.0f && !toastMessage_.empty();
    if (!show) toastShown_.clear();
    const float y = AnimSpring(ImGui::GetID("##toasty"), show ? 0.0f : -30.0f);
    const float alpha = AnimEase(ImGui::GetID("##toasta"), show ? 1.0f : 0.0f, 0.25f);
    if (alpha <= 0.01f || toastMessage_.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ui::PushText(1, 13.0f);
    const ImVec2 ts = ImGui::CalcTextSize(toastMessage_.c_str());
    const ImVec2 size(ts.x + 32.0f * s, ts.y + 20.0f * s);
    const float cx = (areaMin.x + areaMax.x) * 0.5f;
    const ImVec2 a(cx - size.x * 0.5f, areaMin.y + 16.0f * s + y * s);
    const ImVec2 b(a.x + size.x, a.y + size.y);
    const bool error = toastMessage_.find("Could not") != std::string::npos ||
                       toastMessage_.find("failed") != std::string::npos ||
                       toastMessage_.find("Validation") != std::string::npos;
    ui::SoftShadow(dl, a, ImVec2(b.x, b.y + 6.0f * s), 12.0f * s, IM_COL32(0, 0, 0, static_cast<int>(120 * alpha)), 18.0f * s, 8);
    dl->AddRectFilled(a, b, ui::WithAlpha(error ? k.bad : k.text, alpha), k.rack ? 3.0f * s : 12.0f * s);
    dl->AddText(ImVec2(a.x + 16.0f * s, a.y + 10.0f * s), IM_COL32(17, 17, 17, static_cast<int>(255 * alpha)),
                toastMessage_.c_str());
    ui::PopFont();
}

void ConfiguratorApp::HandleKeyboardShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !config_.IsReadOnly()) {
        SaveConfig();
    }
}

void ConfiguratorApp::PollRuntimeLink() {
    ImGuiIO& io = ImGui::GetIO();

    if (!rlConnected_) {
        if (!rlAutoReconnect_) return;
        rlReconnectTimer_ += io.DeltaTime;
        if (rlReconnectTimer_ < kRlReconnectInterval) return;
        rlReconnectTimer_ = 0.0f;

        if (TryAutoDiscoverDriver()) {
            OnConnected();
        }
        return;
    }

    rlPollTimer_ += io.DeltaTime;
    if (rlPollTimer_ < kRlPollInterval) return;
    rlPollTimer_ = 0.0f;

    if (!rlClient_.IsHostAlive(svms::kRuntimeHostTimeoutMs)) {
        rlConnected_ = false;
        rlReconnectTimer_ = 0.0f;
        statusMessage_ = "Driver disconnected — will attempt reconnect";
        toastTimer_ = 3.0f;
        toastMessage_ = statusMessage_;
        return;
    }

    rlClient_.ReadTelemetry(rlTelemetry_);
}

void ConfiguratorApp::FlushLiveChanges() {
    if (!rlConnected_ || !rlClient_.HasCapability(
            svms::build::CapabilityLiveConfiguration) ||
        (pendingLiveMask_ == 0u && !pendingChannelLimiter_)) return;

    ImGuiIO& io = ImGui::GetIO();
    if (rlRetryBackoff_ > 0.0f) {
        rlRetryBackoff_ -= io.DeltaTime;
        return;
    }

    rlFlushTimer_ += io.DeltaTime;
    if (rlFlushTimer_ < kRlFlushInterval) return;
    rlFlushTimer_ = 0.0f;

    const uint32_t submittedMask = pendingLiveMask_;
    char err[svms::kRuntimeLinkResultTextCapacity] = {};
    // ApplyLiveConfig rejects an empty group mask, so it is only sent when
    // a grouped change is pending; channel-limiter-only changes skip
    // straight to their dedicated command below.
    svms::RLResult result = svms::RLResult::Ok;
    if (submittedMask != 0u) {
        result = rlClient_.SendCommand(
            svms::RLCommandType::ApplyLiveConfig, submittedMask, 0u,
            workingLive_, kRlLiveCommandTimeoutMs, err);
    }
    if (result == svms::RLResult::Ok) {
        pendingLiveMask_ &= ~submittedMask;
        rlFailedFlushes_ = 0u;
        rlRetryBackoff_ = 0.0f;
        SendPendingChannelLimiter();
        return;
    }

    if (result == svms::RLResult::RestartRequired &&
        (submittedMask & svms::RLGroupVoices) != 0u) {
        pendingLiveMask_ &= ~svms::RLGroupVoices;
        SendPendingChannelLimiter();
        statusMessage_ = "Voice cap exceeds the startup pool — restart required to grow it";
        if (err[0] != '\0') statusMessage_ += std::string(" — ") + err;
        toastTimer_ = 4.0f;
        toastMessage_ = statusMessage_;
        rlFailedFlushes_ = 0u;
        return;
    }

    if (result == svms::RLResult::Unsupported) {
        // Retrying cannot help; drop the change and say so once.
        pendingLiveMask_ &= ~submittedMask;
        SendPendingChannelLimiter();
        statusMessage_ = err[0] != '\0' ? std::string(err) : "Not available live on this driver";
        toastTimer_ = 4.0f;
        toastMessage_ = statusMessage_;
        rlFailedFlushes_ = 0u;
        return;
    }

    rlRetryBackoff_ = 0.15f;
    if (++rlFailedFlushes_ >= 3u) {
        rlFailedFlushes_ = 0u;
        statusMessage_ = "Live update delayed: " +
            std::string(svms::RLV2_ResultToString(result));
        if (err[0] != '\0') statusMessage_ += std::string(" — ") + err;
        toastTimer_ = 3.0f;
        toastMessage_ = statusMessage_;
    }
}

void ConfiguratorApp::SetLiveFloat(svms::RLCommandType type, float value) {
    NoteLiveChange();
    switch (type) {
    case svms::RLCommandType::SetMasterVolume:
        workingLive_.masterVolume = value; break;
    case svms::RLCommandType::SetReverbMix:
        workingLive_.reverbMix = value; break;
    case svms::RLCommandType::SetReverbRoomSize:
        workingLive_.reverbRoomSize = value; break;
    case svms::RLCommandType::SetReverbDecay:
        workingLive_.reverbDecay = value; break;
    case svms::RLCommandType::SetReverbDamping:
        workingLive_.reverbDamping = value; break;
    case svms::RLCommandType::SetReverbWidth:
        workingLive_.reverbWidth = value; break;
    case svms::RLCommandType::SetReverbDiffusion:
        workingLive_.reverbDiffusion = value; break;
    case svms::RLCommandType::SetReverbPreDelayMs:
        workingLive_.reverbPreDelayMs = value; break;
    case svms::RLCommandType::SetReverbEarlyLevel:
        workingLive_.reverbEarlyLevel = value; break;
    case svms::RLCommandType::SetReverbLateLevel:
        workingLive_.reverbLateLevel = value; break;
    case svms::RLCommandType::SetReverbModDepth:
        workingLive_.reverbModDepth = value; break;
    case svms::RLCommandType::SetReverbModRate:
        workingLive_.reverbModRate = value; break;
    case svms::RLCommandType::SetReverbLowCutHz:
        workingLive_.reverbLowCutHz = value; break;
    case svms::RLCommandType::SetReverbHighCutHz:
        workingLive_.reverbHighCutHz = value; break;
    case svms::RLCommandType::SetLimiterThreshold:
        workingLive_.limiterThreshold = value; break;
    case svms::RLCommandType::SetLimiterLookahead:
        workingLive_.limiterLookaheadMs = value; break;
    case svms::RLCommandType::SetLimiterAttack:
        workingLive_.limiterAttackMs = value; break;
    case svms::RLCommandType::SetLimiterRelease:
        workingLive_.limiterReleaseMs = value; break;
    default:
        return;
    }
    pendingLiveMask_ |= svms::RLV2_GroupForType(type);
    rlFlushTimer_ = kRlFlushInterval;
}

void ConfiguratorApp::SetLiveBool(svms::RLCommandType type, bool value) {
    NoteLiveChange();
    switch (type) {
    case svms::RLCommandType::SetReverbEnabled:
        workingLive_.reverbEnabled = value ? 1u : 0u; break;
    case svms::RLCommandType::SetLimiterEnabled:
        workingLive_.limiterEnabled = value ? 1u : 0u; break;
    case svms::RLCommandType::SetCorrectnessMode:
        workingLive_.correctnessMode = value ? 1u : 0u; break;
    default:
        return;
    }
    pendingLiveMask_ |= svms::RLV2_GroupForType(type);
    rlFlushTimer_ = kRlFlushInterval;
}

void ConfiguratorApp::SetLiveChannelLimiter(bool enabled, float threshold,
                                            float releaseMs) {
    if (!rlConnected_ || !rlClient_.HasCapability(
            svms::build::CapabilityLiveConfiguration)) return;
    NoteLiveChange();
    pendingChannelLimiter_ = true;
    pendingChannelLimiterValues_[0] = enabled ? 1.0f : 0.0f;
    pendingChannelLimiterValues_[1] = threshold;
    pendingChannelLimiterValues_[2] = releaseMs;
    rlFlushTimer_ = kRlFlushInterval;
}

// The channel limiter rides a dedicated wire command because
// RuntimeLiveStateV2 (ABI-pinned, echoed inside the 512-byte legacy
// telemetry prefix) has no spare words for it. Payload travels in the
// command text area: "enabled;threshold;releaseMs".
void ConfiguratorApp::SendPendingChannelLimiter() {
    if (!pendingChannelLimiter_) return;
    char payload[64];
    std::snprintf(payload, sizeof(payload), "%d;%.6f;%.1f",
                  pendingChannelLimiterValues_[0] > 0.5f ? 1 : 0,
                  pendingChannelLimiterValues_[1],
                  pendingChannelLimiterValues_[2]);
    char err[svms::kRuntimeLinkResultTextCapacity] = {};
    const svms::RLResult result = rlClient_.SendCommand(
        svms::RLCommandType::SetChannelLimiter,
        svms::RLGroupChannelLimiter, 0u, workingLive_,
        kRlLiveCommandTimeoutMs, err, payload);
    if (result == svms::RLResult::Ok) {
        pendingChannelLimiter_ = false;
    } else if (result != svms::RLResult::Busy) {
        pendingChannelLimiter_ = false;
        statusMessage_ = "Per-channel limiter update failed: " +
            std::string(svms::RLV2_ResultToString(result));
        if (err[0] != 0) statusMessage_ += std::string(" - ") + err;
        toastTimer_ = 3.0f;
        toastMessage_ = statusMessage_;
    }
}

void ConfiguratorApp::SetLiveMaxVoices(uint32_t value) {
    NoteLiveChange();
    if (value == 0u) value = 1u;
    workingLive_.maxVoices = value;
    pendingLiveMask_ |= svms::RLGroupVoices;
    rlFlushTimer_ = kRlFlushInterval;
}

void ConfiguratorApp::SetLiveLimiterAlgorithm(uint32_t value) {
    NoteLiveChange();
    workingLive_.limiterAlgorithm = (std::min)(1u, value);
    pendingLiveMask_ |= svms::RLGroupLimiter;
    rlFlushTimer_ = kRlFlushInterval;
}

static svms::RuntimeLiveStateV2 LiveStateFromConfig(const ConfigValues& w) {
    svms::RuntimeLiveStateV2 l{};
    l.masterVolume = w.masterVolume;
    l.correctnessMode = w.correctnessMode ? 1u : 0u;
    l.maxVoices = w.maxVoices;
    l.reverbEnabled = w.enableReverb ? 1u : 0u;
    l.reverbMix = w.reverbMix;
    l.reverbRoomSize = w.reverbRoomSize;
    l.reverbDecay = w.reverbDecay;
    l.reverbDamping = w.reverbDamping;
    l.reverbWidth = w.reverbWidth;
    l.reverbDiffusion = w.reverbDiffusion;
    l.reverbPreDelayMs = w.reverbPreDelayMs;
    l.reverbEarlyLevel = w.reverbEarlyLevel;
    l.reverbLateLevel = w.reverbLateLevel;
    l.reverbModDepth = w.reverbModDepth;
    l.reverbModRate = w.reverbModRate;
    l.reverbLowCutHz = w.reverbLowCutHz;
    l.reverbHighCutHz = w.reverbHighCutHz;
    l.limiterEnabled = w.limiterEnabled ? 1u : 0u;
    l.limiterAlgorithm = (std::min)(1u, w.limiterAlgorithm);
    l.limiterThreshold = w.limiterThreshold;
    l.limiterLookaheadMs = w.limiterLookaheadMs;
    l.limiterAttackMs = w.limiterAttackMs;
    l.limiterReleaseMs = w.limiterReleaseMs;
    return l;
}

void ConfiguratorApp::SeedWorkingLive() {
    workingLive_ = LiveStateFromConfig(config_.Working());
}

void ConfiguratorApp::PushAllLiveParams() {
    if (!rlConnected_ || !rlClient_.HasCapability(
            svms::build::CapabilityLiveConfiguration)) return;

    workingLive_ = LiveStateFromConfig(config_.Working());
    pendingLiveMask_ |= svms::RLGroupAll;
    rlFlushTimer_ = kRlFlushInterval;
    rlRetryBackoff_ = 0.0f;
}

void ConfiguratorApp::AdoptEngineLiveState() {
    if (!rlConnected_) return;

    const svms::RuntimeLiveStateV2& e = rlTelemetry_.live;
    ConfigValues& w = config_.Working();
    const ConfigValues before = w;
    w.masterVolume = e.masterVolume;
    w.correctnessMode = e.correctnessMode != 0u;
    if (e.maxVoices != 0u) w.maxVoices = e.maxVoices;
    w.enableReverb = e.reverbEnabled != 0u;
    w.reverbMix = e.reverbMix;
    w.reverbRoomSize = e.reverbRoomSize;
    w.reverbDecay = e.reverbDecay;
    w.reverbDamping = e.reverbDamping;
    w.reverbWidth = e.reverbWidth;
    w.reverbDiffusion = e.reverbDiffusion;
    w.reverbPreDelayMs = e.reverbPreDelayMs;
    w.reverbEarlyLevel = e.reverbEarlyLevel;
    w.reverbLateLevel = e.reverbLateLevel;
    w.reverbModDepth = e.reverbModDepth;
    w.reverbModRate = e.reverbModRate;
    w.reverbLowCutHz = e.reverbLowCutHz;
    w.reverbHighCutHz = e.reverbHighCutHz;
    w.limiterEnabled = e.limiterEnabled != 0u;
    w.limiterAlgorithm = (std::min)(1u, e.limiterAlgorithm);
    w.limiterThreshold = e.limiterThreshold;
    w.limiterLookaheadMs = e.limiterLookaheadMs;
    w.limiterAttackMs = e.limiterAttackMs;
    w.limiterReleaseMs = e.limiterReleaseMs;
    SeedWorkingLive();
    if (ConfigValuesEqual(before, w)) return;
    config_.MarkDirty();
    statusMessage_ = "Adopted engine state — review and save";
    ShowToast("Picked up the live settings of the running SVMS. Save to keep them.", 3.5f);
}

void ConfiguratorApp::OnConnected() {
    rlConnected_ = true;
    rlLastKnownPid_ = rlClient_.GetPID();
    rlReconnectTimer_ = 0.0f;
    rlPollTimer_ = 0.0f;
    rlFlushTimer_ = 0.0f;
    rlRetryBackoff_ = 0.0f;
    rlFailedFlushes_ = 0u;
    statusMessage_ = "Connected to driver (PID " +
                     std::to_string(rlLastKnownPid_) + ", RuntimeLink " +
                     std::to_string(static_cast<uint32_t>(
                         rlClient_.GetProtocol())) + ")";
    toastTimer_ = 3.0f;
    toastMessage_ = statusMessage_;

    // Adopt Engine is gone: with no unsaved edits, the working copy follows
    // whatever the running engine is actually playing with.
    if (!config_.IsDirty() &&
        rlClient_.HasCapability(svms::build::CapabilityLiveConfiguration) &&
        rlClient_.ReadTelemetry(rlTelemetry_) && rlTelemetry_.live.maxVoices != 0u) {
        AdoptEngineLiveState();
    }
}

bool ConfiguratorApp::TryAutoDiscoverDriver() {
    svms::RuntimeLinkClient::HostInfo hosts[svms::kRuntimeHostMaxCount];
    const uint32_t count = svms::RuntimeLinkClient::EnumerateHosts(
        hosts, svms::kRuntimeHostMaxCount);
    if (count == 0u) return false;

    for (uint32_t pass = 0; pass < 2u; ++pass) {
        for (uint32_t i = 0; i < count; ++i) {
            if (pass == 0u && hosts[i].pid != rlLastKnownPid_) continue;
            if (pass == 1u && hosts[i].pid == rlLastKnownPid_) continue;
            if (!hosts[i].fresh) continue;
            if (rlClient_.Open(hosts[i].pid)) return true;
        }
    }
    return false;
}

} // namespace svms::cfg
