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
        // Attach the owning ConfiguratorApp; CreateWindowExW passes `this`
        // as lpCreateParams.
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_SIZE: {
        // Record the new client size only; the swap chain is resized in
        // the normal render loop (never re-entrantly from WndProc).
        ConfiguratorApp* app = appFromHwnd(hWnd);
        if (app && wParam != SIZE_MINIMIZED) {
            app->pendingWidth_ = static_cast<UINT>(LOWORD(lParam));
            app->pendingHeight_ = static_cast<UINT>(HIWORD(lParam));
            app->resizePending_ = true;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        // Apply the suggested rect; the font rebuild is deferred to the
        // render loop via pending flags.
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
        // Ask before discarding unsaved configuration edits.
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
        // The window is already gone; forget the handle so Shutdown()
        // (which runs once in WinMain after the message loop) cannot call
        // DestroyWindow on a stale HWND, then end the message loop.
        if (appFromHwnd(hWnd)) {
            appFromHwnd(hWnd)->hwnd_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool ConfiguratorApp::Initialize(HINSTANCE hInstance, int argc, char** argv) {
    // Per-monitor V2 DPI awareness: the window and its ImGui font follow
    // the monitor the window actually lives on.  Must be set before any
    // window is created.  (Configurator builds target modern Windows.)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    easterEggs_ = RollEasterEggs(argc, argv);
    // The notification popup must be opened by default: ShowMegaFuckerNotification
    // early-returns unless *open is already true.
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
    // Idempotent: runs exactly once (from WinMain after the message loop
    // exits).  Never invoked from WndProc/WM_DESTROY.
    if (shutdownDone_) return;
    shutdownDone_ = true;

    rlClient_.Close();
    rlConnected_ = false;

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

    // lpParam = this: WM_NCCREATE stores the pointer in GWLP_USERDATA so
    // every later message can reach the owning app instance.
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

    // With per-monitor V2 awareness the real scale is the window's,
    // not the system's.
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

    // Initial font build: the DX11 backend does not exist yet, so the
    // atlas is only prepared here (no backend invalidation call).  The
    // backend uploads the atlas during its first NewFrame/CreateDeviceObjects.
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
    // Runtime (post-init) rebuilds must invalidate the DX11 backend's
    // references to the CURRENT atlas before the atlas is rebuilt; the
    // backend recreates its device/font objects from the new atlas during
    // the next NewFrame.  Before ImGui_ImplDX11_Init() there is no backend
    // data yet, and calling InvalidateDeviceObjects() would dereference it
    // (startup crash), so it is skipped when rendererBackendInitialized
    // is false.
    if (rendererBackendInitialized) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // Since 1.92 the font data referenced by an ImFontConfig must persist
    // for the whole atlas lifetime (the atlas no longer makes its own copy
    // and keeps the caller's pointer for on-demand glyph rasterization).
    // Keep our copy in a member, and keep ownership so the atlas does not
    // free() a C++-allocated buffer on shutdown (heap corruption).
    ImFontConfig cfg;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.FontDataOwnedByAtlas = false;

    // Windows is not necessarily installed on C:; resolve the real
    // Windows directory for the Segoe UI font file and load it through
    // the wide-character file API (AddFontFromFileTTF only accepts
    // narrow paths).
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
        // Fall back to the built-in font when Segoe UI is unavailable.
        font = io.Fonts->AddFontDefault();
    }
    io.FontDefault = font;

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

    // The font atlas rebuild is deferred to the start of the next frame;
    // doing it here would render re-entrantly inside WndProc.
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

    // Resize only when something actually changed; keeping the current
    // render target means the swap chain and RTV stay in sync.
    if (width == windowWidth_ && height == windowHeight_ && renderTarget_) {
        return;
    }

    if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }

    HRESULT hr = swapChain_->ResizeBuffers(
        0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LogStartupFailure(L"ResizeBuffers failed (retrying with the "
                          L"current back buffer)", hr);
    }

    // Even when ResizeBuffers fails, the previous back buffer may still
    // be usable — recreate the RTV from whatever the swap chain holds now.
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
        // No valid render target: re-arm the pending resize so the next
        // frame retries instead of rendering into a null resource.
        resizePending_ = true;
        if (FAILED(hr)) {
            LogStartupFailure(L"GetBuffer/CreateRenderTargetView after "
                              L"resize failed", hr);
        }
    }
}

void ConfiguratorApp::RenderFrame() {
    // Deferred window-size handling: the swap chain is resized exactly
    // once per actual WM_SIZE, in the render loop (never in WndProc).
    if (resizePending_) {
        resizePending_ = false;
        ResizeSwapChain(static_cast<int>(pendingWidth_),
                        static_cast<int>(pendingHeight_));
    }

    // Deferred DPI font rebuild (WM_DPICHANGED only records it).
    if (dpiRebuildPending_) {
        dpiRebuildPending_ = false;
        RecreateFonts(pendingDpiScale_, true);
    }

    // A failed resize may have left no render target; drop the frame
    // instead of dereferencing invalid resources (ResizeSwapChain re-arms
    // resizePending_ so the next frame retries).
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
    lc.app = this;
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

    // Heartbeat-based death detection (no process scanning).
    if (!rlClient_.IsHostAlive(svms::kRuntimeHostTimeoutMs)) {
        rlConnected_ = false;
        rlReconnectTimer_ = 0.0f;
        statusMessage_ = "Driver disconnected — will attempt reconnect";
        toastTimer_ = 3.0f;
        toastMessage_ = statusMessage_;
        return;
    }

    // Skip-if-busy telemetry read: mid-publish snapshots are skipped and
    // the last good snapshot is kept.
    rlClient_.ReadTelemetry(rlTelemetry_);

    FlushLiveChanges();
}

void ConfiguratorApp::FlushLiveChanges() {
    if (!rlConnected_ || pendingLiveMask_ == 0u) return;

    ImGuiIO& io = ImGui::GetIO();
    rlFlushTimer_ += io.DeltaTime;
    if (rlFlushTimer_ < kRlFlushInterval) return;
    rlFlushTimer_ = 0.0f;

    char err[svms::kRuntimeLinkResultTextCapacity] = {};
    const svms::RLResult result = rlClient_.SendCommand(
        svms::RLCommandType::ApplyLiveConfig, pendingLiveMask_, 0u,
        workingLive_, svms::kRuntimeLinkDefaultCommandTimeoutMs, err);
    if (result == svms::RLResult::Ok) {
        pendingLiveMask_ = 0u;
        rlFailedFlushes_ = 0u;
        return;
    }
    // Keep the mask so the change is re-sent on the next flush; surface
    // persistent failures to the user (every 5th failed attempt).
    if (++rlFailedFlushes_ >= 5u) {
        rlFailedFlushes_ = 0u;
        statusMessage_ = "Live update failed: " +
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
        return; // unknown field — do not mark dirty
    }
    pendingLiveMask_ |= svms::RLV2_GroupForType(type);
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
}

// Maps the (saved or working) config values onto the live payload used
// for grouped ApplyLiveConfig commands.
static svms::RuntimeLiveStateV2 LiveStateFromConfig(const ConfigValues& w) {
    svms::RuntimeLiveStateV2 l{};
    l.masterVolume = w.masterVolume;
    l.correctnessMode = w.correctnessMode ? 1u : 0u;
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
    if (!rlConnected_) return;

    workingLive_ = LiveStateFromConfig(config_.Working());
    char err[svms::kRuntimeLinkResultTextCapacity] = {};
    const svms::RLResult result = rlClient_.SendCommand(
        svms::RLCommandType::ApplyLiveConfig, svms::RLGroupAll, 0u,
        workingLive_, 1500u, err);
    if (result == svms::RLResult::Ok) {
        pendingLiveMask_ = 0u;
    } else {
        statusMessage_ = "Live push failed: " +
            std::string(svms::RLV2_ResultToString(result));
        if (err[0] != '\0') statusMessage_ += std::string(" — ") + err;
        toastTimer_ = 4.0f;
        toastMessage_ = statusMessage_;
    }
}

void ConfiguratorApp::OnConnected() {
    rlConnected_ = true;
    rlLastKnownPid_ = rlClient_.GetPID();
    rlReconnectTimer_ = 0.0f;
    rlPollTimer_ = 0.0f;
    rlFlushTimer_ = 0.0f;
    rlFailedFlushes_ = 0u;
    statusMessage_ = "Connected to driver (PID " +
                     std::to_string(rlLastKnownPid_) + ")";
    toastTimer_ = 3.0f;
    toastMessage_ = statusMessage_;

    // Connecting is READ-ONLY: no live parameters are pushed on connect.
    // The RUNTIME state is displayed from telemetry and only diverges
    // from the config after the user changes something.
}

bool ConfiguratorApp::TryAutoDiscoverDriver() {
    // Discovery goes through the hosts registry (well-known mapping),
    // preferring the most recently used driver PID.
    svms::RuntimeLinkClientV2::HostInfo hosts[svms::kRuntimeHostMaxCount];
    const uint32_t count = svms::RuntimeLinkClientV2::EnumerateHosts(
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
