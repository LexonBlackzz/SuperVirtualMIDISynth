#include "configurator/Platform.h"
#include "configurator/Theme.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace svms::cfg {

namespace {

const wchar_t* kWindowClass = L"SVMS_V3_Configurator";
const wchar_t* kWindowTitle = L"SuperVirtualMIDISynth V3";

void LogStartupFailure(const wchar_t* what) {
    wchar_t buf[1024];
    swprintf(buf, 1024, L"SVMS V3 Configurator failed to start: %s\n", what);
    OutputDebugStringW(buf);
}

void LogStartupFailure(const wchar_t* what, HRESULT hr) {
    wchar_t buf[1024];
    swprintf(buf, 1024,
             L"SVMS V3 Configurator failed to start: %s (HRESULT 0x%08lX)\n",
             what, static_cast<unsigned long>(hr));
    OutputDebugStringW(buf);
}

std::wstring StartupFailureText(const wchar_t* what) {
    wchar_t buf[1024];
    swprintf(buf, 1024, L"SVMS V3 Configurator failed to start: %s", what);
    return std::wstring(buf);
}

std::wstring StartupFailureText(const wchar_t* what, HRESULT hr) {
    wchar_t buf[1024];
    swprintf(buf, 1024,
             L"SVMS V3 Configurator failed to start: %s (HRESULT 0x%08lX)",
             what, static_cast<unsigned long>(hr));
    return std::wstring(buf);
}

bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (size > 0) {
        out.resize(static_cast<size_t>(size));
        fseek(f, 0, SEEK_SET);
        fread(out.data(), 1, out.size(), f);
    }
    fclose(f);
    return !out.empty();
}

class Win32Platform final : public Platform {
public:
    bool CreateWindowAndDevice(const Hooks& hooks, std::wstring& error) override {
        hooks_ = hooks;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return CreateMainWindow(error) && CreateD3D11(error);
    }

    void DestroyWindowAndDevice() override {
        if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
        if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
        if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
        if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    bool InitBackends(std::wstring& error) override {
        if (!ImGui_ImplWin32_Init(hwnd_)) {
            error = StartupFailureText(L"ImGui_ImplWin32_Init failed");
            LogStartupFailure(L"ImGui_ImplWin32_Init failed");
            return false;
        }
        if (!ImGui_ImplDX11_Init(d3dDevice_, d3dContext_)) {
            error = StartupFailureText(L"ImGui_ImplDX11_Init failed");
            LogStartupFailure(L"ImGui_ImplDX11_Init failed");
            return false;
        }
        return true;
    }

    void ShutdownBackends() override {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
    }

    void InvalidateFontObjects() override { ImGui_ImplDX11_InvalidateDeviceObjects(); }

    bool PumpEvents() override {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) return false;
        }
        return true;
    }

    bool IsMinimized() const override {
        return hwnd_ != nullptr && IsIconic(hwnd_) != FALSE;
    }
    void WaitForEvent() override { WaitMessage(); }

    void SetTitle(const wchar_t* title) override {
        if (hwnd_ != nullptr && title != nullptr && title[0] != L'\0')
            SetWindowTextW(hwnd_, title);
    }

    void ApplyPendingResize() override {
        if (!resizePending_) return;
        resizePending_ = false;
        ResizeSwapChain(static_cast<int>(pendingWidth_),
                        static_cast<int>(pendingHeight_));
    }

    bool TakeDpiChange(float& scale) override {
        if (!dpiRebuildPending_) return false;
        dpiRebuildPending_ = false;
        scale = pendingDpiScale_;
        return true;
    }

    bool CanRender() const override { return renderTarget_ != nullptr; }

    void NewFrame() override {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
    }

    void Present(const float clear[4]) override {
        d3dContext_->OMSetRenderTargets(1, &renderTarget_, nullptr);
        d3dContext_->ClearRenderTargetView(renderTarget_, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swapChain_->Present(1, 0);
    }

    int Width() const override { return windowWidth_; }
    int Height() const override { return windowHeight_; }
    float DpiScale() const override { return dpiScale_; }
    void* NativeWindow() const override { return hwnd_; }

    bool LoadSystemFont(bool mono, std::vector<unsigned char>& out) override {
        out.clear();
        wchar_t windowsDir[MAX_PATH] = {};
        const UINT len = GetWindowsDirectoryW(windowsDir, MAX_PATH);
        if (len == 0u || len >= MAX_PATH) return false;
        const std::wstring dir(windowsDir);
        if (!mono) return ReadWholeFile(dir + L"\\Fonts\\segoeui.ttf", out);
        for (const wchar_t* file : {L"\\Fonts\\consola.ttf", L"\\Fonts\\cour.ttf"}) {
            if (ReadWholeFile(dir + file, out)) return true;
        }
        return false;
    }

    std::wstring DefaultConfigPath() override {
        wchar_t appData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                       SHGFP_TYPE_CURRENT, appData)))
            return std::wstring(appData) + L"\\SuperVirtualMIDISynth\\config.json";
        return std::wstring();
    }

    bool FileExists(const std::wstring& path) override {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    std::wstring Utf8ToWide(const std::string& utf8) override {
        wchar_t buf[MAX_PATH] = {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf, MAX_PATH);
        return n > 0 ? std::wstring(buf) : std::wstring();
    }

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto self = [](HWND h) {
            return reinterpret_cast<Win32Platform*>(GetWindowLongPtrW(h, GWLP_USERDATA));
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
            Win32Platform* p = self(hWnd);
            if (p) {
                RECT rc = {0, 0,
                           static_cast<LONG>(kMinWindowWidth * p->dpiScale_),
                           static_cast<LONG>(kMinWindowHeight * p->dpiScale_)};
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
            Win32Platform* p = self(hWnd);
            if (p && wParam != SIZE_MINIMIZED) {
                p->pendingWidth_ = static_cast<UINT>(LOWORD(lParam));
                p->pendingHeight_ = static_cast<UINT>(HIWORD(lParam));
                p->resizePending_ = true;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            Win32Platform* p = self(hWnd);
            if (p) {
                p->HandleDpiChange(static_cast<float>(HIWORD(wParam)) / 96.0f,
                                   reinterpret_cast<const RECT*>(lParam));
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH * 2] = {};
                if (!DragQueryFileW(drop, i, path, MAX_PATH * 2)) continue;
                char utf8[MAX_PATH * 4] = {};
                if (WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr) > 0)
                    DroppedFileQueue().push_back(utf8);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_CLOSE: {
            Win32Platform* p = self(hWnd);
            if (p && p->hooks_.hasUnsavedChanges && p->hooks_.hasUnsavedChanges()) {
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
            if (self(hWnd)) self(hWnd)->hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void HandleDpiChange(float scale, const RECT* suggestedRect) {
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

    bool CreateMainWindow(std::wstring& error) {
        HINSTANCE hInstance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc)) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            error = StartupFailureText(L"RegisterClassExW failed", hr);
            LogStartupFailure(L"RegisterClassExW failed", hr);
            return false;
        }

        dpiScale_ = static_cast<float>(GetDpiForSystem()) / 96.0f;

        RECT rc = { 0, 0,
                    static_cast<LONG>(kDefaultWindowWidth * dpiScale_),
                    static_cast<LONG>(kDefaultWindowHeight * dpiScale_) };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        const int x = (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2;

        hwnd_ = CreateWindowExW(
            0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
            x, y, rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, hInstance, this);
        if (!hwnd_) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            error = StartupFailureText(L"CreateWindowExW failed", hr);
            LogStartupFailure(L"CreateWindowExW failed", hr);
            return false;
        }

        DragAcceptFiles(hwnd_, TRUE);
        dpiScale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);
        return true;
    }

    bool CreateD3D11(std::wstring& error) {
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
            error = StartupFailureText(L"D3D11CreateDeviceAndSwapChain failed", hr);
            LogStartupFailure(L"D3D11CreateDeviceAndSwapChain failed", hr);
            return false;
        }

        ID3D11Texture2D* backBuffer = nullptr;
        hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) {
            error = StartupFailureText(L"swap chain GetBuffer failed", hr);
            LogStartupFailure(L"swap chain GetBuffer failed", hr);
            return false;
        }

        hr = d3dDevice_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
        backBuffer->Release();
        if (FAILED(hr)) {
            error = StartupFailureText(L"CreateRenderTargetView failed", hr);
            LogStartupFailure(L"CreateRenderTargetView failed", hr);
            return false;
        }
        return true;
    }

    void ResizeSwapChain(int width, int height) {
        if (!swapChain_ || !d3dDevice_) return;
        if (width <= 0 || height <= 0) return;
        if (width == windowWidth_ && height == windowHeight_ && renderTarget_)
            return;

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

    Hooks hooks_;
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
};

} // namespace

std::unique_ptr<Platform> CreatePlatform() {
    return std::make_unique<Win32Platform>();
}

} // namespace svms::cfg
