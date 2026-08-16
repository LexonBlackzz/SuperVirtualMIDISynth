#include "ConfiguratorApp.h"
#include "Theme.h"
#include "Widgets.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include "PageOverview.h"
#include "PageAudio.h"
#include "PagePerformance.h"
#include "PageMidi.h"
#include "PageReverb.h"
#include "PageLimiter.h"
#include "PageDiagnostics.h"
#include "PageAdvanced.h"
#include "PageAbout.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace svms::cfg {

static ConfiguratorApp* g_app = nullptr;

static const wchar_t* kWindowClass = L"SVMS_V3_Configurator";
static const wchar_t* kWindowTitle = L"SuperVirtualMIDISynth V3";

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 1;

    switch (msg) {
    case WM_SIZE:
        if (g_app && wParam != SIZE_MINIMIZED) {
            g_app->RenderFrame();
        }
        return 0;
    case WM_DESTROY:
        if (g_app) g_app->Shutdown();
        return 0;
    case WM_DPICHANGED:
        if (g_app) {
            g_app->RenderFrame();
        }
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool ConfiguratorApp::Initialize(HINSTANCE hInstance, int argc, char** argv) {
    easterEggs_ = RollEasterEggs(argc, argv);

    wchar_t configPath[MAX_PATH] = {};
    bool configPathSpecified = false;
    uint32_t runtimeLinkPid = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            int wLen = MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1,
                                           configPath, MAX_PATH);
            if (wLen > 0) configPathSpecified = true;
        }
        if (std::string(argv[i]) == "--runtime-link" && i + 1 < argc) {
            runtimeLinkPid = static_cast<uint32_t>(std::atoi(argv[i + 1]));
        }
    }

    if (!configPathSpecified) {
        config_.LoadDefaults();
        std::wstring autoPath;
        wchar_t appData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                       SHGFP_TYPE_CURRENT, appData))) {
            autoPath = std::wstring(appData) +
                       L"\\SuperVirtualMIDISynth\\config.json";
        }
        if (!autoPath.empty()) {
            DWORD attr = GetFileAttributesW(autoPath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                config_.Load(autoPath);
            } else {
                config_.LoadDefaults();
                config_.SetActivePath(autoPath);
            }
        }
    } else {
        config_.Load(configPath);
    }

    if (!CreateMainWindow(hInstance)) return false;
    if (!CreateD3D11()) return false;
    CreateImGui();
    ApplyTheme();

    // Connect to RuntimeLink if PID was specified, otherwise try auto-discovery
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
    rlClient_.Close();
    rlConnected_ = false;

    if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    DestroyMainWindow();
    running_ = false;
}

bool ConfiguratorApp::CreateMainWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    dpiScale_ = static_cast<float>(dpi) / 96.0f;

    RECT rc = { 0, 0,
                static_cast<LONG>(kDefaultWindowWidth * dpiScale_),
                static_cast<LONG>(kDefaultWindowHeight * dpiScale_) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int x = (scrW - (rc.right - rc.left)) / 2;
    int y = (scrH - (rc.bottom - rc.top)) / 2;

    hwnd_ = CreateWindowExW(
        0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        x, y, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd_) return false;

    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);
    return true;
}

void ConfiguratorApp::DestroyMainWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool ConfiguratorApp::CreateD3D11() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0,
                                          D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION,
        &sd, &swapChain_, &d3dDevice_, &featureLevel, &d3dContext_);

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &swapChain_, &d3dDevice_, &featureLevel, &d3dContext_);
    }

    if (FAILED(hr)) return false;

    ID3D11Texture2D* backBuffer = nullptr;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = d3dDevice_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
    backBuffer->Release();
    if (FAILED(hr)) return false;

    return true;
}

void ConfiguratorApp::DestroyD3D11() {
    if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }
}

void ConfiguratorApp::CreateImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ApplyTheme();

    const wchar_t* fontPath = L"C:\\Windows\\Fonts\\segoeui.ttf";
    DWORD attr = GetFileAttributesW(fontPath);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        ImFontConfig cfg;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, &cfg);
    }

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(d3dDevice_, d3dContext_);
}

bool ConfiguratorApp::PumpMessages() {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) {
            running_ = false;
            return false;
        }
    }
    return running_;
}

void ConfiguratorApp::HandleWindowSize() {
    if (!swapChain_ || !d3dDevice_) return;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
    swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* backBuffer = nullptr;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        d3dDevice_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
        backBuffer->Release();
    }

    windowWidth_ = w;
    windowHeight_ = h;
}

void ConfiguratorApp::RenderFrame() {
    HandleWindowSize();
    HandleKeyboardShortcuts();
    PollRuntimeLink();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowWidth_),
                                     static_cast<float>(windowHeight_)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);

    DrawHeader();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
    DrawSidebar();
    ImGui::SameLine();

    float sidebarWidth = kSidebarWidth * dpiScale_;
    ImGui::SetCursorPosX(sidebarWidth);
    ImGui::SetCursorPosY(kHeaderHeight * dpiScale_ + 2.0f);

    float pageW = static_cast<float>(windowWidth_) - sidebarWidth;
    float footerH = kFooterHeight * dpiScale_;
    float pageH = static_cast<float>(windowHeight_) -
                  kHeaderHeight * dpiScale_ - footerH - 4.0f;

    ImGui::BeginChild("##page", ImVec2(pageW, pageH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    DrawPageContent();
    ImGui::EndChild();

    float footerY = static_cast<float>(windowHeight_) - footerH;
    ImGui::SetCursorPos(ImVec2(sidebarWidth + 12.0f, footerY + 6.0f));
    DrawFooter();

    ImGui::End();
    ImGui::PopStyleVar(3);

    ShowMegaFuckerNotification(easterEggs_, &showMegaFuckerPopup_);
    DrawToastOverlay();

    ImGui::Render();
    const float clearColor[4] = { 0.066f, 0.075f, 0.082f, 1.0f };
    d3dContext_->OMSetRenderTargets(1, &renderTarget_, nullptr);
    d3dContext_->ClearRenderTargetView(renderTarget_, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);
}

void ConfiguratorApp::DrawHeader() {
    ImGui::SetCursorPosY(6.0f);
    ImGui::SetCursorPosX(14.0f);

    ImGui::PushFont(nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
    ImGui::Text("SuperVirtualMIDISynth V3");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::SameLine(300.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.48f, 0.52f, 1.0f));
    ImGui::Text("\"Semi-Professional*\" Audio Software "
                "tailored for Black MIDI");
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("*Professionalism may decrease as NPS increases.");
        ImGui::EndTooltip();
    }

    float lineY = kHeaderHeight * dpiScale_ - 1.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(0, lineY),
                ImVec2(static_cast<float>(windowWidth_), lineY),
                ImGui::GetColorU32(ImVec4(0.176f, 0.196f, 0.227f, 1.0f)),
                1.0f);
}

void ConfiguratorApp::DrawSidebar() {
    float sidebarW = kSidebarWidth * dpiScale_;
    float headerH = kHeaderHeight * dpiScale_ + 2.0f;
    float footerH = kFooterHeight * dpiScale_;
    float sidebarH = static_cast<float>(windowHeight_) - headerH - footerH;

    ImVec2 sidebarPos(0, headerH);
    ImGui::SetCursorPos(sidebarPos);

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(0.090f, 0.102f, 0.118f, 1.0f));

    ImGui::BeginChild("##sidebar", ImVec2(sidebarW, sidebarH),
                      ImGuiChildFlags_None);

    ImGui::SetCursorPosY(8.0f);

    auto drawNavItem = [&](const char* label, Page page, const char* category) {
        if (category) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.37f, 0.40f, 0.44f, 1.0f));
            ImGui::PushFont(nullptr);
            ImGui::SetCursorPosX(14.0f);
            ImGui::TextUnformatted(category);
            ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
        }

        bool selected = (currentPage_ == page);
        ImVec4 textColor = selected
            ? ImVec4(0.85f, 0.88f, 0.92f, 1.0f)
            : ImVec4(0.56f, 0.59f, 0.62f, 1.0f);

        if (selected) {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos,
                              ImVec2(pos.x + sidebarW, pos.y + 28.0f),
                              ImGui::GetColorU32(ImVec4(0.447f, 0.533f,
                                                       0.855f, 0.08f)));
            dl->AddRectFilled(ImVec2(pos.x, pos.y + 4.0f),
                              ImVec2(pos.x + 3.0f, pos.y + 24.0f),
                              ImGui::GetColorU32(ImVec4(0.447f, 0.533f,
                                                       0.855f, 0.9f)),
                              1.5f);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, textColor);
        ImGui::SetCursorPosX(14.0f);
        if (ImGui::Selectable(label, selected,
                              ImGuiSelectableFlags_None,
                              ImVec2(sidebarW - 14.0f, 28.0f))) {
            currentPage_ = page;
        }
        ImGui::PopStyleColor();
    };

    drawNavItem("Overview", Page::Overview, "GENERAL");
    drawNavItem("Audio", Page::Audio, nullptr);
    drawNavItem("Performance", Page::Performance, nullptr);
    drawNavItem("MIDI / Events", Page::Midi, nullptr);

    drawNavItem("Reverb", Page::Reverb, "EFFECTS");
    drawNavItem("Limiter", Page::Limiter, nullptr);

    drawNavItem("Diagnostics", Page::Diagnostics, "SYSTEM");
    drawNavItem("Advanced", Page::Advanced, nullptr);
    drawNavItem("About", Page::About, nullptr);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ConfiguratorApp::DrawFooter() {
    float avail = ImGui::GetContentRegionAvail().x;

    if (config_.IsDirty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.90f, 0.70f, 0.20f, 1.0f));
        ImGui::Text("Configuration modified");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
        ImGui::Text("Configuration saved");
        ImGui::PopStyleColor();
    }

    // RuntimeLink status + Connect/Disconnect button
    ImGui::SameLine(200.0f);
    if (rlConnected_) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.30f, 0.80f, 0.45f, 1.0f));
        ImGui::Text("Driver PID %u", rlClient_.GetPID());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Disconnect")) {
            rlClient_.Close();
            rlConnected_ = false;
            statusMessage_ = "Disconnected from driver";
            toastTimer_ = 2.0f;
            toastMessage_ = statusMessage_;
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.45f, 0.48f, 0.52f, 1.0f));
        ImGui::TextDisabled("No driver");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Connect")) {
            if (TryAutoDiscoverDriver()) {
                OnConnected();
            } else {
                statusMessage_ = "No running SVMS driver found";
                toastTimer_ = 3.0f;
                toastMessage_ = statusMessage_;
            }
        }
    }

    ImGui::SameLine(avail - 370.0f);

    if (ImGui::Button("Revert", ImVec2(70, 28))) {
        config_.Revert();
        PushAllLiveParams();
        statusMessage_ = "Configuration reverted";
        toastTimer_ = 3.0f;
        toastMessage_ = "Configuration reverted";
    }

    ImGui::SameLine();

    // Discard: push loaded (saved) values back to driver, clear working changes
    bool canDiscard = config_.IsDirty();
    if (!canDiscard) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    }
    if (ImGui::Button("Discard", ImVec2(70, 28)) && canDiscard) {
        config_.Revert();  // restore working = loaded
        PushAllLiveParams(); // push loaded values to driver
        statusMessage_ = "Changes discarded — driver restored";
        toastTimer_ = 3.0f;
        toastMessage_ = "Changes discarded";
    }
    if (!canDiscard) {
        ImGui::PopStyleVar();
    }

    ImGui::SameLine();

    bool canSave = config_.IsDirty();
    if (!canSave) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    }

    ImGui::PushStyleColor(ImGuiCol_Button,
                          canSave
                              ? ImVec4(0.447f, 0.533f, 0.855f, 0.6f)
                              : ImVec4(0.15f, 0.17f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.510f, 0.596f, 0.922f, 0.8f));

    if (ImGui::Button("Save Configuration", ImVec2(160, 28)) && canSave) {
        auto path = config_.GetActivePath();
        ConfigValidation v = config_.Validate();
        if (v.valid) {
            if (config_.Save(path)) {
                // Also push all live params so driver matches saved config
                PushAllLiveParams();
                toastTimer_ = 3.0f;
                toastMessage_ = "Configuration saved & pushed to driver";
                statusMessage_ = "Saved";
            } else {
                toastTimer_ = 4.0f;
                toastMessage_ = "Could not save configuration";
            }
        } else {
            toastTimer_ = 5.0f;
            toastMessage_ = "Validation failed: " + v.warnings;
        }
    }

    ImGui::PopStyleColor(2);
    if (!canSave) {
        ImGui::PopStyleVar();
    }

    if (toastTimer_ > 0.0f) {
        ImGuiIO& io = ImGui::GetIO();
        toastTimer_ -= io.DeltaTime;
    }
}

void ConfiguratorApp::DrawPageContent() {
    // Make live telemetry available to all pages
    LiveLinkContext lc;
    lc.client = rlConnected_ ? &rlClient_ : nullptr;
    lc.telemetry = rlConnected_ ? &rlTelemetry_ : nullptr;
    lc.connected = rlConnected_;
    SetLiveLinkContext(lc);

    switch (currentPage_) {
    case Page::Overview:    DrawOverviewPage(config_); break;
    case Page::Audio:       DrawAudioPage(config_, easterEggs_); break;
    case Page::Performance: DrawPerformancePage(config_); break;
    case Page::Midi:        DrawMidiPage(config_); break;
    case Page::Reverb:      DrawReverbPage(config_); break;
    case Page::Limiter:     DrawLimiterPage(config_); break;
    case Page::Diagnostics: DrawDiagnosticsPage(config_); break;
    case Page::Advanced:    DrawAdvancedPage(config_); break;
    case Page::About:       DrawAboutPage(config_); break;
    }
}

void ConfiguratorApp::DrawToastOverlay() {
    if (toastTimer_ <= 0.0f || toastMessage_.empty()) return;

    float alpha = ImClamp(toastTimer_, 0.0f, 1.0f);

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos(io.DisplaySize.x - 20.0f, io.DisplaySize.y - 60.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f * alpha);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 8));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0.10f, 0.12f, 0.16f, 0.92f * alpha));
    ImGui::PushStyleColor(ImGuiCol_Border,
                          ImVec4(0.447f, 0.533f, 0.855f, 0.4f * alpha));

    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    bool isError = toastMessage_.find("Could not") != std::string::npos ||
                   toastMessage_.find("Validation") != std::string::npos;

    ImVec4 textColor = isError
        ? ImVec4(0.90f, 0.45f, 0.35f, alpha)
        : ImVec4(0.30f, 0.80f, 0.45f, alpha);

    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::TextWrapped("%s", toastMessage_.c_str());
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void ConfiguratorApp::HandleKeyboardShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        auto path = config_.GetActivePath();
        ConfigValidation v = config_.Validate();
        if (v.valid) {
            if (config_.Save(path)) {
                toastTimer_ = 3.0f;
                toastMessage_ = "Configuration saved";
            } else {
                toastTimer_ = 4.0f;
                toastMessage_ = "Could not save configuration";
            }
        } else {
            toastTimer_ = 5.0f;
            toastMessage_ = "Validation failed: " + v.warnings;
        }
    }
}

void ConfiguratorApp::PollRuntimeLink() {
    ImGuiIO& io = ImGui::GetIO();

    if (!rlConnected_) {
        // Auto-reconnect: periodically scan for a running driver
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

    // Check if driver is still alive
    if (!rlClient_.IsOpen()) {
        rlConnected_ = false;
        rlReconnectTimer_ = 0.0f;
        statusMessage_ = "Driver disconnected — will attempt reconnect";
        toastTimer_ = 3.0f;
        toastMessage_ = statusMessage_;
        return;
    }

    rlTelemetry_ = rlClient_.ReadTelemetry();
}

void ConfiguratorApp::SendLiveCommand(svms::RLCommandType type, float value) {
    if (!rlConnected_) return;
    rlClient_.PushFloatCommand(type, value);
}

void ConfiguratorApp::SendLiveBoolCommand(svms::RLCommandType type, bool value) {
    if (!rlConnected_) return;
    rlClient_.PushBoolCommand(type, value);
}

void ConfiguratorApp::PushAllLiveParams() {
    if (!rlConnected_) return;
    auto& w = config_.Working();

    rlClient_.PushFloatCommand(svms::RLCommandType::SetMasterVolume, w.masterVolume);
    rlClient_.PushBoolCommand(svms::RLCommandType::SetCorrectnessMode, w.correctnessMode);

    rlClient_.PushBoolCommand(svms::RLCommandType::SetReverbEnabled, w.enableReverb);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbMix, w.reverbMix);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbRoomSize, w.reverbRoomSize);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbDecay, w.reverbDecay);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbDamping, w.reverbDamping);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbWidth, w.reverbWidth);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbDiffusion, w.reverbDiffusion);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbPreDelayMs, w.reverbPreDelayMs);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbEarlyLevel, w.reverbEarlyLevel);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbLateLevel, w.reverbLateLevel);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbModDepth, w.reverbModDepth);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbModRate, w.reverbModRate);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbLowCutHz, w.reverbLowCutHz);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetReverbHighCutHz, w.reverbHighCutHz);

    rlClient_.PushBoolCommand(svms::RLCommandType::SetLimiterEnabled, w.limiterEnabled);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetLimiterThreshold, w.limiterThreshold);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetLimiterLookahead, w.limiterLookaheadMs);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetLimiterAttack, w.limiterAttackMs);
    rlClient_.PushFloatCommand(svms::RLCommandType::SetLimiterRelease, w.limiterReleaseMs);
}

void ConfiguratorApp::OnConnected() {
    rlConnected_ = true;
    rlLastKnownPid_ = rlClient_.GetPID();
    rlReconnectTimer_ = 0.0f;
    statusMessage_ = "Connected to driver (PID " +
                     std::to_string(rlLastKnownPid_) + ")";
    toastTimer_ = 3.0f;
    toastMessage_ = statusMessage_;

    // Push all config values to the driver so it matches the loaded config
    PushAllLiveParams();
}

bool ConfiguratorApp::TryAutoDiscoverDriver() {
    wchar_t memName[128];

    // Enumerate running processes looking for one with our shared memory
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;

    if (Process32FirstW(snap, &pe)) {
        do {
            // Try opening the shared memory mapping for this PID
            svms::RL_SharedMemName(pe.th32ProcessID, memName, 128);
            HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, memName);
            if (hMap) {
                CloseHandle(hMap);
                // Found a process with our shared memory — try connecting
                if (rlClient_.Open(pe.th32ProcessID)) {
                    found = true;
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return found;
}

} // namespace svms::cfg
