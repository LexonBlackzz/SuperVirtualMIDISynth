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
#include "PageOffline.h"
#include "PageLiveRecording.h"
#include "PageReverb.h"
#include "PageLimiter.h"
#include "PageChannelLimiter.h"
#include "PageDiagnostics.h"
#include "PageAdvanced.h"
#include "PageAbout.h"

#include <cstdio>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace svms::cfg {

static const wchar_t* kWindowClass = L"SVMS_V3_Configurator";
static const wchar_t* kWindowTitle = L"SuperVirtualMIDISynth V3";

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

static void LogStartupFailure(const wchar_t* what) {
    wchar_t buf[1024];
    swprintf(buf, 1024, L"SVMS V3 Configurator failed to start: %s\n", what);
    OutputDebugStringW(buf);
}

static void LogStartupFailure(const wchar_t* what, HRESULT hr) {
    wchar_t buf[1024];
    swprintf(buf, 1024,
             L"SVMS V3 Configurator failed to start: %s (HRESULT 0x%08lX)\n",
             what, static_cast<unsigned long>(hr));
    OutputDebugStringW(buf);
}

static std::wstring StartupFailureText(const wchar_t* what) {
    wchar_t buf[1024];
    swprintf(buf, 1024, L"SVMS V3 Configurator failed to start: %s", what);
    return std::wstring(buf);
}

static std::wstring StartupFailureText(const wchar_t* what, HRESULT hr) {
    wchar_t buf[1024];
    swprintf(buf, 1024,
             L"SVMS V3 Configurator failed to start: %s (HRESULT 0x%08lX)",
             what, static_cast<unsigned long>(hr));
    return std::wstring(buf);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto appFromHwnd = [](HWND hWnd) -> ConfiguratorApp* {
        return reinterpret_cast<ConfiguratorApp*>(
            GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    };

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 1;

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        ConfiguratorApp* app = appFromHwnd(hWnd);
        if (app) {
            RECT rc = {0, 0,
                       static_cast<LONG>(kMinWindowWidth * app->dpiScale_),
                       static_cast<LONG>(kMinWindowHeight * app->dpiScale_)};
            AdjustWindowRectEx(
                &rc, static_cast<DWORD>(GetWindowLongPtrW(hWnd, GWL_STYLE)), FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(hWnd, GWL_EXSTYLE)));
            MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = rc.right - rc.left;
            info->ptMinTrackSize.y = rc.bottom - rc.top;
        }
        return 0;
    }
    case WM_SIZE: {
        ConfiguratorApp* app = appFromHwnd(hWnd);
        if (app && wParam != SIZE_MINIMIZED) {
            app->pendingWidth_ = static_cast<UINT>(LOWORD(lParam));
            app->pendingHeight_ = static_cast<UINT>(HIWORD(lParam));
            app->resizePending_ = true;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        ConfiguratorApp* app = appFromHwnd(hWnd);
        if (app) {
            const float newScale =
                static_cast<float>(HIWORD(wParam)) / 96.0f;
            app->HandleDpiChange(newScale,
                                 reinterpret_cast<const RECT*>(lParam));
        }
        return 0;
    }
    case WM_CLOSE: {
        ConfiguratorApp* app = appFromHwnd(hWnd);
        if (app && app->config_.IsDirty()) {
            const int choice = MessageBoxW(
                hWnd,
                L"Configuration has unsaved changes.\nClose anyway?",
                L"SuperVirtualMIDISynth V3",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (choice != IDYES) return 0;
        }
        DestroyWindow(hWnd);
        return 0;
    }
    case WM_DESTROY:
        if (appFromHwnd(hWnd)) {
            appFromHwnd(hWnd)->hwnd_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool ConfiguratorApp::Initialize(HINSTANCE hInstance, int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    easterEggs_ = RollEasterEggs(argc, argv);
    showMegaFuckerPopup_ = easterEggs_.megaFuckerDac;

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
    SeedWorkingLive();

    if (!CreateMainWindow(hInstance)) return false;
    if (!CreateD3D11()) return false;
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

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    DestroyD3D11();
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
    if (!RegisterClassExW(&wc)) {
        lastInitError_ = StartupFailureText(
            L"RegisterClassExW failed",
            HRESULT_FROM_WIN32(GetLastError()));
        LogStartupFailure(L"RegisterClassExW failed",
                          HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

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
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        lastInitError_ = StartupFailureText(
            L"CreateWindowExW failed",
            HRESULT_FROM_WIN32(GetLastError()));
        LogStartupFailure(L"CreateWindowExW failed",
                          HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    dpiScale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;

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

    if (FAILED(hr)) {
        lastInitError_ = StartupFailureText(L"D3D11CreateDeviceAndSwapChain failed", hr);
        LogStartupFailure(L"D3D11CreateDeviceAndSwapChain failed", hr);
        return false;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        lastInitError_ = StartupFailureText(L"swap chain GetBuffer failed", hr);
        LogStartupFailure(L"swap chain GetBuffer failed", hr);
        return false;
    }

    hr = d3dDevice_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
    backBuffer->Release();
    if (FAILED(hr)) {
        lastInitError_ = StartupFailureText(L"CreateRenderTargetView failed", hr);
        LogStartupFailure(L"CreateRenderTargetView failed", hr);
        return false;
    }

    return true;
}

void ConfiguratorApp::DestroyD3D11() {
    if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }
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

    if (!ImGui_ImplWin32_Init(hwnd_)) {
        lastInitError_ = StartupFailureText(L"ImGui_ImplWin32_Init failed");
        LogStartupFailure(L"ImGui_ImplWin32_Init failed");
        return false;
    }
    if (!ImGui_ImplDX11_Init(d3dDevice_, d3dContext_)) {
        lastInitError_ = StartupFailureText(L"ImGui_ImplDX11_Init failed");
        LogStartupFailure(L"ImGui_ImplDX11_Init failed");
        return false;
    }
    return true;
}

void ConfiguratorApp::RecreateFonts(float scale, bool rendererBackendInitialized) {
    if (rendererBackendInitialized) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.FontDataOwnedByAtlas = false;

    wchar_t windowsDir[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(windowsDir, MAX_PATH);
    fontData_.clear();
    if (len > 0u && len < MAX_PATH) {
        FILE* f = _wfopen((std::wstring(windowsDir) + L"\\Fonts\\segoeui.ttf").c_str(),
                          L"rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long size = ftell(f);
            if (size > 0) {
                fontData_.resize(static_cast<size_t>(size));
                fseek(f, 0, SEEK_SET);
                fread(fontData_.data(), 1, fontData_.size(), f);
            }
            fclose(f);
        }
    }

    const float fontSize = 16.0f * scale;
    ImFont* font = nullptr;
    if (!fontData_.empty()) {
        font = io.Fonts->AddFontFromMemoryTTF(
            fontData_.data(), static_cast<int>(fontData_.size()), fontSize, &cfg);
    }
    if (!font) {
        font = io.Fonts->AddFontDefault();
    }
    io.FontDefault = font;

    // Monospace face for LCD readouts, key caps and panel engravings.
    ImFont* mono = nullptr;
    monoFontData_.clear();
    if (len > 0u && len < MAX_PATH) {
        static const wchar_t* kMonoFiles[] = {L"\\Fonts\\consola.ttf",
                                              L"\\Fonts\\cour.ttf"};
        for (const wchar_t* file : kMonoFiles) {
            FILE* f = _wfopen((std::wstring(windowsDir) + file).c_str(), L"rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            const long size = ftell(f);
            if (size > 0) {
                monoFontData_.resize(static_cast<size_t>(size));
                fseek(f, 0, SEEK_SET);
                fread(monoFontData_.data(), 1, monoFontData_.size(), f);
            }
            fclose(f);
            if (!monoFontData_.empty()) break;
        }
    }
    if (!monoFontData_.empty()) {
        mono = io.Fonts->AddFontFromMemoryTTF(
            monoFontData_.data(), static_cast<int>(monoFontData_.size()),
            fontSize * 0.88f, &cfg);
    }
    SetMonoFont(mono);

    io.Fonts->Build();
}

void ConfiguratorApp::HandleDpiChange(float scale, const RECT* suggestedRect) {
    if (!hwnd_) return;
    dpiScale_ = scale;

    if (suggestedRect) {
        SetWindowPos(hwnd_, nullptr, suggestedRect->left, suggestedRect->top,
                     suggestedRect->right - suggestedRect->left,
                     suggestedRect->bottom - suggestedRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    pendingDpiScale_ = scale;
    dpiRebuildPending_ = true;
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

void ConfiguratorApp::ResizeSwapChain(int width, int height) {
    if (!swapChain_ || !d3dDevice_) return;
    if (width <= 0 || height <= 0) return;

    if (width == windowWidth_ && height == windowHeight_ && renderTarget_) {
        return;
    }

    if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }

    HRESULT hr = swapChain_->ResizeBuffers(
        0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LogStartupFailure(L"ResizeBuffers failed (retrying with the current back buffer)", hr);
    }

    ID3D11Texture2D* backBuffer = nullptr;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr) && backBuffer) {
        hr = d3dDevice_->CreateRenderTargetView(backBuffer, nullptr,
                                                &renderTarget_);
        backBuffer->Release();
    } else if (backBuffer) {
        backBuffer->Release();
    }

    windowWidth_ = width;
    windowHeight_ = height;

    if (FAILED(hr) || !renderTarget_) {
        resizePending_ = true;
        if (FAILED(hr)) {
            LogStartupFailure(L"GetBuffer/CreateRenderTargetView after resize failed", hr);
        }
    }
}

void ConfiguratorApp::RenderFrame() {
    if (resizePending_) {
        resizePending_ = false;
        ResizeSwapChain(static_cast<int>(pendingWidth_),
                        static_cast<int>(pendingHeight_));
    }

    if (dpiRebuildPending_) {
        dpiRebuildPending_ = false;
        RecreateFonts(pendingDpiScale_, true);
    }

    if (!renderTarget_) return;

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
    const float headerH = kHeaderHeight * dpiScale_;
    DrawMasterStrip(headerH);
    DrawChainStrip(headerH + kMasterHeight * dpiScale_);

    const float pageTop = headerH + (kMasterHeight + kChainHeight) * dpiScale_ + 2.0f;
    const float footerH = kFooterHeight * dpiScale_;
    const float pageW = static_cast<float>(windowWidth_);
    const float pageH = static_cast<float>(windowHeight_) - pageTop - footerH - 4.0f;

    ImGui::SetCursorPos(ImVec2(0.0f, pageTop));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 6.0f));
    ImGui::BeginChild("##page", ImVec2(pageW, pageH),
                      ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground);
    DrawPageContent();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Esc is "back to Home" unless something is being edited or a menu is open.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
        !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        currentPage_ = Page::Overview;
    }

    float footerY = static_cast<float>(windowHeight_) - footerH;
    ImGui::SetCursorPos(ImVec2(16.0f, footerY + 6.0f));
    DrawFooter();

    ImGui::End();
    ImGui::PopStyleVar(3);

    ShowMegaFuckerNotification(easterEggs_, &showMegaFuckerPopup_);
    DrawToastOverlay();

    ImGui::Render();
    const ImVec4 clear = GetThemeSettings().background;
    const float clearColor[4] = { clear.x, clear.y, clear.z, clear.w };
    d3dContext_->OMSetRenderTargets(1, &renderTarget_, nullptr);
    d3dContext_->ClearRenderTargetView(renderTarget_, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);

    // Present the click/drag first, then perform the cross-process update.
    // This prevents a slow or temporarily busy driver from making the button
    // itself feel like it ignored the user. Normally the ACK arrives almost
    // immediately; the 50 ms timeout is only a safety ceiling.
    FlushLiveChanges();
}

static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
static ImVec4 Alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

void ConfiguratorApp::DrawHeader() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& th = GetThemeSettings();
    const float W = static_cast<float>(windowWidth_);
    const float H = kHeaderHeight * dpiScale_;
    const ImVec2 origin = ImGui::GetWindowPos();

    // Faceplate strip.
    dl->AddRectFilled(origin, ImVec2(origin.x + W, origin.y + H),
                      ImGui::GetColorU32(th.panel));
    dl->AddRectFilled(ImVec2(origin.x, origin.y + H - 2.0f),
                      ImVec2(origin.x + W, origin.y + H),
                      ImGui::GetColorU32(GetKeyEdge()));

    PushMono();
    const float textH = ImGui::GetTextLineHeight();
    const float cy = origin.y + (H - 2.0f) * 0.5f;

    // Wordmark.
    float x = origin.x + 14.0f;
    dl->AddText(ImVec2(x, cy - textH * 0.5f), ImGui::GetColorU32(th.text), "SVMS");
    x += ImGui::CalcTextSize("SVMS").x;
    dl->AddText(ImVec2(x, cy - textH * 0.5f), ImGui::GetColorU32(th.accent), "/");
    x += ImGui::CalcTextSize("/").x;
    dl->AddText(ImVec2(x, cy - textH * 0.5f), ImGui::GetColorU32(th.text), "V3");
    x += ImGui::CalcTextSize("V3").x + 8.0f;
    dl->AddText(ImVec2(x, cy - textH * 0.5f), ImGui::GetColorU32(th.mutedText),
                svms::build::kProductVersion);

    // Link lamp on the right.
    const char* linkLabel = "LINK";
    const float linkW = ImGui::CalcTextSize(linkLabel).x;
    const float rightEdge = origin.x + W - 14.0f;
    DrawLed(dl, ImVec2(rightEdge - 4.0f, cy), 4.0f, rlConnected_, &th.success);
    dl->AddText(ImVec2(rightEdge - 14.0f - linkW, cy - textH * 0.5f),
                ImGui::GetColorU32(th.mutedText), linkLabel);

    // LCD readout.
    const float lcdX0 = origin.x + 170.0f * dpiScale_;
    const float lcdX1 = rightEdge - 14.0f - linkW - 14.0f;
    if (lcdX1 - lcdX0 > 120.0f) {
        dl->AddRectFilled(ImVec2(lcdX0, cy - 11.0f), ImVec2(lcdX1, cy + 11.0f),
                          ImGui::GetColorU32(GetLcdBg()), th.cornerRadius);
        dl->AddRect(ImVec2(lcdX0, cy - 11.0f), ImVec2(lcdX1, cy + 11.0f),
                    ImGui::GetColorU32(GetKeyEdge()), th.cornerRadius);
        char line[256];
        ImVec4 color = th.accent;
        if (rlConnected_) {
            const VersionRelation relation =
                CompareDriverVersion(rlClient_.GetPeerInfo());
            if (relation == VersionRelation::DriverOlder) {
                std::snprintf(line, sizeof(line),
                              "DRIVER OLDER - SOME NEWER FEATURES UNAVAILABLE");
                color = th.warning;
            } else if (relation == VersionRelation::DriverNewer) {
                std::snprintf(line, sizeof(line),
                              "CONFIGURATOR OLDER - COMPATIBILITY MODE");
                color = th.warning;
            } else {
                std::snprintf(line, sizeof(line),
                              "VOICES %-6u PID %-6u RL%u%s",
                              rlTelemetry_.activeVoices, rlClient_.GetPID(),
                              static_cast<uint32_t>(rlClient_.GetProtocol()),
                              rlClient_.GetProtocol() ==
                                      svms::RuntimeLinkClient::Protocol::V2
                                  ? " (LEGACY)" : "");
            }
        } else if (config_.IsReadOnly()) {
            std::snprintf(line, sizeof(line),
                          "CONFIG OPENED READ-ONLY TO PRESERVE ITS DATA");
            color = th.warning;
        } else {
            std::snprintf(line, sizeof(line), "OFFLINE - NO DRIVER CONNECTED");
            color = Mix(th.accent, th.background, 0.45f);
        }
        dl->PushClipRect(ImVec2(lcdX0, cy - 11.0f), ImVec2(lcdX1, cy + 11.0f), true);
        dl->AddText(ImVec2(lcdX0 + 10.0f, cy - textH * 0.5f),
                    ImGui::GetColorU32(color), line);
        dl->PopClipRect();
    }
    PopMono();
}

static float MasterVolumeDb(float v) {
    return 20.0f * std::log10((std::max)(v, 0.001f));
}

static float MasterVolumeFromDb(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// Segmented horizontal bar (outputs, voices, CPU).
static void DrawHSegBar(ImDrawList* dl, ImVec2 p, float width, float frac,
                        bool accentOnly) {
    const ThemeSettings& th = GetThemeSettings();
    const float h = 14.0f;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h),
                      ImGui::GetColorU32(GetLcdBg()), 3.0f);
    const float pitch = 6.0f;
    const int n = (std::max)(1, static_cast<int>((width - 6.0f) / pitch));
    const int lit = static_cast<int>(ImClamp(frac, 0.0f, 1.0f) * n + 0.5f);
    for (int i = 0; i < n; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(n);
        const ImVec4 col = accentOnly ? th.accent
            : (f < 0.65f ? th.success : (f < 0.85f ? th.warning : th.error));
        const ImVec4 c = i < lit ? col : Alpha(col, 0.20f);
        dl->AddRectFilled(ImVec2(p.x + 3.0f + i * pitch, p.y + 3.0f),
                          ImVec2(p.x + 3.0f + i * pitch + 4.0f, p.y + h - 3.0f),
                          ImGui::GetColorU32(c), 1.0f);
    }
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), ImGui::GetColorU32(GetKeyEdge()), 3.0f);
}

void ConfiguratorApp::DrawMasterStrip(float y) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& th = GetThemeSettings();
    const float W = static_cast<float>(windowWidth_);
    const float H = kMasterHeight * dpiScale_;
    dl->AddRectFilled(ImVec2(0.0f, y), ImVec2(W, y + H), ImGui::GetColorU32(th.sidebar));
    dl->AddRectFilled(ImVec2(0.0f, y + H - 2.0f), ImVec2(W, y + H),
                      ImGui::GetColorU32(GetKeyEdge()));

    auto& w = config_.Working();
    ImGui::SetCursorPos(ImVec2(22.0f, y + 4.0f));
    KnobState knob = {w.masterVolume, 0.0f, 4.0f, 1.0f, "MASTER", nullptr, 44.0f,
                      1.0f, MasterVolumeDb, MasterVolumeFromDb};
    if (RotaryKnob(knob, "%.1f dB")) {
        w.masterVolume = knob.value;
        config_.MarkDirty();
        SetLiveFloat(svms::RLCommandType::SetMasterVolume, knob.value);
    }

    const bool tel = rlConnected_;
    static float outVis = 0.0f;
    float outTarget = 0.0f;
    if (tel) {
        const float peak = (std::max)(rlTelemetry_.limiterOutputPeakL,
                                      rlTelemetry_.limiterOutputPeakR);
        if (peak > 0.000001f)
            outTarget = ImClamp((20.0f * std::log10(peak) + 60.0f) / 60.0f, 0.0f, 1.0f);
    }
    const float dt = (std::max)(0.0f, ImGui::GetIO().DeltaTime);
    outVis += (outTarget - outVis) * (1.0f - std::exp(-(outTarget > outVis ? 22.0f : 8.0f) * dt));

    const float x0 = 130.0f;
    const float gap = 30.0f;
    const float blockW = (std::max)(120.0f, (W - x0 - 24.0f - gap * 2.0f) / 3.0f);
    PushLabel(0.85f);
    char buf[64];
    auto block = [&](int i, const char* label, const char* value, float frac, bool accent) {
        const float x = x0 + i * (blockW + gap);
        dl->AddText(ImVec2(x, y + 20.0f), ImGui::GetColorU32(th.mutedText), StyleText(label).c_str());
        const ImVec2 vs = ImGui::CalcTextSize(value);
        dl->AddText(ImVec2(x + blockW - vs.x, y + 20.0f), ImGui::GetColorU32(th.accent), value);
        DrawHSegBar(dl, ImVec2(x, y + 44.0f), blockW, frac, accent);
    };
    if (tel) {
        const float peak = (std::max)(rlTelemetry_.limiterOutputPeakL,
                                      rlTelemetry_.limiterOutputPeakR);
        std::snprintf(buf, sizeof(buf), "%.1f dB",
                      peak > 0.000001f ? 20.0f * std::log10(peak) : -60.0f);
    } else {
        std::snprintf(buf, sizeof(buf), "--");
    }
    block(0, "OUTPUT", buf, outVis, false);
    if (tel) {
        std::snprintf(buf, sizeof(buf), "%u / %u", rlTelemetry_.activeVoices, w.maxVoices);
    } else {
        std::snprintf(buf, sizeof(buf), "-- / %u", w.maxVoices);
    }
    block(1, "VOICES", buf,
          tel ? static_cast<float>(rlTelemetry_.activeVoices) /
                    static_cast<float>((std::max)(1u, w.maxVoices))
              : 0.0f, true);
    if (tel) {
        std::snprintf(buf, sizeof(buf), "%.0f%%", rlTelemetry_.cpuLoadPercent);
    } else {
        std::snprintf(buf, sizeof(buf), "--");
    }
    block(2, "CPU", buf, tel ? rlTelemetry_.cpuLoadPercent / 100.0f : 0.0f, false);
    PopLabel();
}

void ConfiguratorApp::DrawChainStrip(float y) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& th = GetThemeSettings();
    const float W = static_cast<float>(windowWidth_);
    const float H = kChainHeight * dpiScale_;
    dl->AddRectFilled(ImVec2(0.0f, y), ImVec2(W, y + H), ImGui::GetColorU32(th.panel));
    dl->AddRectFilled(ImVec2(0.0f, y + H - 2.0f), ImVec2(W, y + H),
                      ImGui::GetColorU32(GetKeyEdge()));

    struct Stage { const char* label; Page page; };
    static const Stage stages[] = {
        {"HOME", Page::Overview}, {"MIDI", Page::Midi}, {"SYNTH", Page::Synth},
        {"REVERB", Page::Reverb}, {"LIMITER", Page::Limiter}, {"OUT", Page::Audio}};
    const auto& w = config_.Working();
    auto lamp = [&](Page p) {
        switch (p) {
            case Page::Reverb:  return w.enableReverb;
            case Page::Limiter: return w.limiterEnabled;
            default:            return true;
        }
    };

    auto drawKey = [&](const char* label, ImVec2 pos, float keyW, bool pressed,
                       bool lampOn, bool hovered) {
        const float keyH = 28.0f;
        if (th.style == 1) {
            // Refined: flat pill; the current stage is filled.
            const ImVec4 face = pressed ? Mix(th.control, th.text, 0.12f)
                                        : Mix(th.panel, th.text, hovered ? 0.08f : 0.04f);
            dl->AddRectFilled(pos, ImVec2(pos.x + keyW, pos.y + keyH),
                              ImGui::GetColorU32(face), th.cornerRadius + 2.0f);
            if (lampOn) {
                dl->AddCircleFilled(ImVec2(pos.x + 13.0f, pos.y + keyH * 0.5f), 2.8f,
                                    ImGui::GetColorU32(th.accent), 12);
            }
            dl->AddText(ImVec2(pos.x + 26.0f, pos.y + (keyH - ImGui::GetTextLineHeight()) * 0.5f),
                        ImGui::GetColorU32(pressed ? th.text : th.mutedText), label);
            return;
        }
        const float drop = pressed ? 2.0f : 0.0f;
        const float edgeH = pressed ? 1.0f : 3.0f;
        const float bottom = pos.y + keyH - (pressed ? 2.0f : 0.0f);
        const ImVec4 face = Mix(th.control, th.text, pressed ? 0.07f : (hovered ? 0.04f : 0.0f));
        dl->AddRectFilled(ImVec2(pos.x, pos.y + drop), ImVec2(pos.x + keyW, bottom),
                          ImGui::GetColorU32(GetKeyEdge()), th.cornerRadius);
        dl->AddRectFilled(ImVec2(pos.x, pos.y + drop), ImVec2(pos.x + keyW, bottom - edgeH),
                          ImGui::GetColorU32(face), th.cornerRadius);
        DrawLed(dl, ImVec2(pos.x + 13.0f, pos.y + drop + (keyH - edgeH) * 0.5f), 3.0f, lampOn);
        dl->AddText(ImVec2(pos.x + 26.0f,
                           pos.y + drop + (keyH - edgeH - ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(pressed ? th.text : th.mutedText), label);
    };

    PushLabel(0.95f);
    const float keyY = y + (H - 2.0f - 28.0f) * 0.5f;
    float x = 16.0f;
    const int count = static_cast<int>(sizeof(stages) / sizeof(stages[0]));
    for (int i = 0; i < count; ++i) {
        const Stage& s = stages[i];
        const std::string stageLabel = StyleText(s.label);
        const float keyW = ImGui::CalcTextSize(stageLabel.c_str()).x + 40.0f;
        ImGui::SetCursorPos(ImVec2(x, keyY));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##stage", ImVec2(keyW, 28.0f))) currentPage_ = s.page;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool selected = currentPage_ == s.page;
        drawKey(stageLabel.c_str(), ImVec2(x, keyY), keyW, selected,
                i == 0 ? selected : lamp(s.page), hov);
        x += keyW;
        if (i + 1 < count) {
            const float cy = keyY + 14.0f;
            if (i == 0) {
                // Home is the hub, not a stage: a divider instead of a signal line.
                dl->AddLine(ImVec2(x + 14.0f, cy - 10.0f), ImVec2(x + 14.0f, cy + 10.0f),
                            ImGui::GetColorU32(GetPanelEdge()), 1.0f);
                x += 28.0f;
            } else {
                dl->AddLine(ImVec2(x + 4.0f, cy), ImVec2(x + 26.0f, cy),
                            ImGui::GetColorU32(GetPanelEdge()), 1.5f);
                dl->AddTriangleFilled(ImVec2(x + 26.0f, cy - 4.0f), ImVec2(x + 26.0f, cy + 4.0f),
                                      ImVec2(x + 32.0f, cy),
                                      ImGui::GetColorU32(GetPanelEdge()));
                x += 36.0f;
            }
        }
    }

    // Tools: the pages you open occasionally.
    struct Tool { const char* label; Page page; };
    static const Tool tools[] = {
        {"Per-channel limiter", Page::ChannelLimiter},
        {"Offline renderer", Page::OfflineRenderer},
        {"Live recording", Page::LiveRecording},
        {"Diagnostics", Page::Diagnostics},
        {"Advanced / theme", Page::Advanced},
        {"About", Page::About}};
    const char* toolLabel = "TOOLS";
    bool toolActive = false;
    for (const Tool& t : tools)
        if (currentPage_ == t.page) { toolLabel = t.label; toolActive = true; }
    const std::string toolStr = StyleText(toolLabel);
    const float toolW = ImGui::CalcTextSize(toolStr.c_str()).x + 52.0f;
    const float toolX = W - toolW - 16.0f;
    ImGui::SetCursorPos(ImVec2(toolX, keyY));
    if (ImGui::InvisibleButton("##tools", ImVec2(toolW, 28.0f))) ImGui::OpenPopup("##tools_menu");
    drawKey(toolStr.c_str(), ImVec2(toolX, keyY), toolW, toolActive, toolActive, ImGui::IsItemHovered());
    dl->AddTriangleFilled(ImVec2(toolX + toolW - 20.0f, keyY + 11.0f),
                          ImVec2(toolX + toolW - 10.0f, keyY + 11.0f),
                          ImVec2(toolX + toolW - 15.0f, keyY + 17.0f),
                          ImGui::GetColorU32(th.mutedText));
    PopLabel();

    // Menu: same rack look as the rest of the app, right-aligned under the key.
    const float menuW = 240.0f;
    ImGui::SetNextWindowPos(ImVec2(W - menuW - 16.0f, keyY + 34.0f));
    ImGui::SetNextWindowSize(ImVec2(menuW, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, th.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, GetPanelEdge());
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, th.cornerRadius + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 4.0f));
    if (ImGui::BeginPopup("##tools_menu")) {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        PushLabel(0.95f);
        for (const Tool& t : tools) {
            const bool selected = currentPage_ == t.page;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float itemW = ImGui::GetContentRegionAvail().x;
            ImGui::PushID(t.label);
            if (ImGui::InvisibleButton("##tool", ImVec2(itemW, 28.0f))) {
                currentPage_ = t.page;
                ImGui::CloseCurrentPopup();
            }
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (selected || hov) {
                pdl->AddRectFilled(p, ImVec2(p.x + itemW, p.y + 28.0f),
                                   ImGui::GetColorU32(Mix(th.control, th.text, selected ? 0.07f : 0.03f)),
                                   th.cornerRadius);
            }
            DrawLed(pdl, ImVec2(p.x + 14.0f, p.y + 14.0f), 3.0f, selected);
            pdl->AddText(ImVec2(p.x + 30.0f, p.y + (28.0f - ImGui::GetTextLineHeight()) * 0.5f),
                         ImGui::GetColorU32(selected ? th.text : th.mutedText), t.label);
        }
        PopLabel();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

void ConfiguratorApp::DrawFooter() {
    const ThemeSettings& th = GetThemeSettings();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = static_cast<float>(windowWidth_);
    const float footerH = kFooterHeight * dpiScale_;
    const float footerTop = static_cast<float>(windowHeight_) - footerH;
    dl->AddRectFilled(ImVec2(0.0f, footerTop), ImVec2(W, static_cast<float>(windowHeight_)),
                      ImGui::GetColorU32(th.panel));
    dl->AddRectFilled(ImVec2(0.0f, footerTop), ImVec2(W, footerTop + 2.0f),
                      ImGui::GetColorU32(GetKeyEdge()));

    // One row: every element is centred on the same line.
    constexpr float kKeyH = 30.0f;
    const float rowY = footerTop + (footerH - kKeyH) * 0.5f + 1.0f;
    const float textY = rowY + (kKeyH - ImGui::GetTextLineHeight()) * 0.5f;

    constexpr float kRevertW = 80.0f;
    constexpr float kAdoptW = 116.0f;
    constexpr float kSaveW = 170.0f;
    constexpr float kGap = 8.0f;
    constexpr float kButtonGroupW = kRevertW + kGap + kAdoptW + kGap + kSaveW;
    const float buttonX = (std::max)(16.0f, W - 16.0f - kButtonGroupW);

    float x = 16.0f;
    DrawLed(dl, ImVec2(x + 4.0f, rowY + kKeyH * 0.5f), 4.0f, config_.IsDirty());
    x += 20.0f;
    const float leftBudget = (std::max)(0.0f, buttonX - x - 12.0f);
    const bool compact = leftBudget < 330.0f;
    const bool veryCompact = leftBudget < 230.0f;

    auto text = [&](const char* s, const ImVec4& color) {
        ImGui::SetCursorPos(ImVec2(x, textY));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(s);
        ImGui::PopStyleColor();
        x += ImGui::CalcTextSize(s).x + 14.0f;
    };

    if (!veryCompact) {
        if (config_.IsDirty()) text(compact ? "Modified" : "Configuration modified", GetWarning());
        else text(compact ? "Saved" : "Configuration saved", GetMutedText());
    }

    constexpr float kSmallKeyH = 26.0f;
    if (rlConnected_) {
        char buf[96];
        if (compact || veryCompact) {
            std::snprintf(buf, sizeof(buf), "PID %u / RL%u", rlClient_.GetPID(),
                          static_cast<uint32_t>(rlClient_.GetProtocol()));
        } else {
            std::snprintf(buf, sizeof(buf), "Driver PID %u / RuntimeLink %u",
                          rlClient_.GetPID(),
                          static_cast<uint32_t>(rlClient_.GetProtocol()));
        }
        text(buf, GetSuccess());
        ImGui::SetCursorPos(ImVec2(x, rowY + (kKeyH - kSmallKeyH) * 0.5f));
        if (KeyButton("Disconnect", ImVec2(96.0f, kSmallKeyH))) {
            rlClient_.Close();
            rlConnected_ = false;
            statusMessage_ = "Disconnected from driver";
            toastTimer_ = 2.0f;
            toastMessage_ = statusMessage_;
        }
    } else {
        text(veryCompact ? "Offline" : "No driver", GetMutedText());
        ImGui::SetCursorPos(ImVec2(x, rowY + (kKeyH - kSmallKeyH) * 0.5f));
        if (KeyButton("Connect", ImVec2(84.0f, kSmallKeyH))) {
            if (TryAutoDiscoverDriver()) {
                OnConnected();
            } else {
                statusMessage_ = "No running SVMS driver found";
                toastTimer_ = 3.0f;
                toastMessage_ = statusMessage_;
            }
        }
    }

    float bx = buttonX;
    ImGui::SetCursorPos(ImVec2(bx, rowY));
    if (KeyButton("Revert", ImVec2(kRevertW, kKeyH), false, config_.IsDirty())) {
        config_.Revert();
        PushAllLiveParams();
        statusMessage_ = "Configuration reverted";
        toastTimer_ = 3.0f;
        toastMessage_ = "Configuration reverted";
    }

    bx += kRevertW + kGap;
    const bool liveSupported = rlConnected_ && rlClient_.HasCapability(
        svms::build::CapabilityLiveConfiguration);
    ImGui::SetCursorPos(ImVec2(bx, rowY));
    if (KeyButton("Adopt Engine", ImVec2(kAdoptW, kKeyH), false, liveSupported)) {
        AdoptEngineLiveState();
    }

    bx += kAdoptW + kGap;
    const bool canSave = config_.IsDirty() && !config_.IsReadOnly();
    ImGui::SetCursorPos(ImVec2(bx, rowY));
    if (KeyButton("Save Configuration", ImVec2(kSaveW, kKeyH), canSave, canSave)) {
        auto path = config_.GetActivePath();
        ConfigValidation v = config_.Validate();
        if (v.valid) {
            if (config_.Save(path)) {
                PushAllLiveParams();
                toastTimer_ = 3.0f;
                toastMessage_ = "Configuration saved & queued for driver";
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

    if (toastTimer_ > 0.0f) {
        ImGuiIO& io = ImGui::GetIO();
        toastTimer_ -= io.DeltaTime;
    }
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

    BeginAutoPanels();
    switch (currentPage_) {
    case Page::Overview:    DrawOverviewPage(config_); break;
    case Page::Audio:       DrawAudioPage(config_, easterEggs_, AudioView::Output); break;
    case Page::Synth:
        DrawPerformancePage(config_);
        DrawAudioPage(config_, easterEggs_, AudioView::SoundFont);
        break;
    case Page::Performance: DrawPerformancePage(config_); break;
    case Page::Midi:        DrawMidiPage(config_); break;
    case Page::OfflineRenderer:
        offlineRendererPage_.Draw(config_, hwnd_);
        break;
    case Page::LiveRecording:
        liveRecordingPage_.Draw(rlConnected_ ? &rlClient_ : nullptr,
                                rlConnected_, hwnd_);
        break;
    case Page::Reverb:      DrawReverbPage(config_); break;
    case Page::Limiter:     DrawLimiterPage(config_); break;
    case Page::ChannelLimiter: DrawChannelLimiterPage(config_); break;
    case Page::Diagnostics: DrawDiagnosticsPage(config_); break;
    case Page::Advanced:    DrawAdvancedPage(config_); break;
    case Page::About:       DrawAboutPage(config_, updateService_); break;
    }
    EndAutoPanels();
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
    ImVec4 toastBg = GetPanelBg();
    toastBg.w = 0.92f * alpha;
    ImVec4 toastBorder = GetAccent();
    toastBorder.w = 0.40f * alpha;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toastBg);
    ImGui::PushStyleColor(ImGuiCol_Border, toastBorder);

    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    bool isError = toastMessage_.find("Could not") != std::string::npos ||
                   toastMessage_.find("Validation") != std::string::npos ||
                   toastMessage_.find("failed") != std::string::npos;

    ImVec4 textColor = isError ? GetError() : GetSuccess();
    textColor.w = alpha;

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

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S) && !config_.IsReadOnly()) {
        auto path = config_.GetActivePath();
        ConfigValidation v = config_.Validate();
        if (v.valid) {
            if (config_.Save(path)) {
                PushAllLiveParams();
                toastTimer_ = 3.0f;
                toastMessage_ = "Configuration saved & queued for driver";
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
    if (value == 0u) value = 1u;
    workingLive_.maxVoices = value;
    pendingLiveMask_ |= svms::RLGroupVoices;
    rlFlushTimer_ = kRlFlushInterval;
}

void ConfiguratorApp::SetLiveLimiterAlgorithm(uint32_t value) {
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
    config_.MarkDirty();
    SeedWorkingLive();
    statusMessage_ = "Adopted engine state — review and save";
    toastTimer_ = 3.0f;
    toastMessage_ = statusMessage_;
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
