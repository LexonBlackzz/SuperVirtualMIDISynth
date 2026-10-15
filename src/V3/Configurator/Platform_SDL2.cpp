// Linux (and any SDL2 host) implementation of Platform: SDL2 window, OpenGL 3
// context, ImGui SDL2/OpenGL3 backends, fontconfig-resolved system fonts.
// Not built on Windows; enabled by SVMS_BUILD_CONFIGURATOR_SDL2 in CMake.

#include "Platform.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <sys/stat.h>

namespace svms::cfg {

namespace {

constexpr int kDefaultWidth = 1180;
constexpr int kDefaultHeight = 760;
constexpr int kMinWidth = 1100;
constexpr int kMinHeight = 700;

bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    if (size > 0) {
        out.resize(static_cast<size_t>(size));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return !out.empty();
}

// Asks fontconfig for the file behind a family name ("sans", "monospace").
std::string FontconfigFile(const char* family) {
    const std::string command = std::string("fc-match -f '%{file}' ") + family + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    char buffer[1024] = {};
    const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, pipe);
    pclose(pipe);
    return std::string(buffer, n);
}

std::string ToNarrow(const std::wstring& wide) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    try { return converter.to_bytes(wide); } catch (...) { return {}; }
}

class Sdl2Platform final : public Platform {
public:
    bool CreateWindowAndDevice(const Hooks& hooks, std::wstring& error) override {
        hooks_ = hooks;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
            error = L"SDL_Init failed";
            std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return false;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        float ddpi = 96.0f;
        SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr);
        dpiScale_ = std::clamp(ddpi / 96.0f, 1.0f, 4.0f);

        window_ = SDL_CreateWindow(
            "SuperVirtualMIDISynth V3", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            int(kDefaultWidth * dpiScale_), int(kDefaultHeight * dpiScale_),
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window_) {
            error = L"SDL_CreateWindow failed";
            std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetWindowMinimumSize(window_, int(kMinWidth * dpiScale_), int(kMinHeight * dpiScale_));
        glContext_ = SDL_GL_CreateContext(window_);
        if (!glContext_) {
            error = L"cannot create an OpenGL 3.0 context";
            std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
            return false;
        }
        SDL_GL_MakeCurrent(window_, glContext_);
        SDL_GL_SetSwapInterval(1);
        SDL_GetWindowSize(window_, &width_, &height_);
        return true;
    }

    void DestroyWindowAndDevice() override {
        if (glContext_) { SDL_GL_DeleteContext(glContext_); glContext_ = nullptr; }
        if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
        SDL_Quit();
    }

    bool InitBackends(std::wstring& error) override {
        if (!ImGui_ImplSDL2_InitForOpenGL(window_, glContext_) ||
            !ImGui_ImplOpenGL3_Init("#version 130")) {
            error = L"ImGui SDL2/OpenGL3 backend init failed";
            return false;
        }
        return true;
    }

    void ShutdownBackends() override {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
    }

    // The atlas texture is recreated lazily on the next NewFrame.
    void InvalidateFontObjects() override { ImGui_ImplOpenGL3_DestroyDeviceObjects(); }

    bool PumpEvents() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            const bool closeRequested =
                event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT &&
                 event.window.event == SDL_WINDOWEVENT_CLOSE &&
                 event.window.windowID == SDL_GetWindowID(window_));
            if (closeRequested && ConfirmClose()) return false;
        }
        return true;
    }

    bool IsMinimized() const override {
        return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
    }
    void WaitForEvent() override { SDL_Delay(50); }

    void SetTitle(const wchar_t* title) override {
        if (window_ && title && title[0]) SDL_SetWindowTitle(window_, ToNarrow(title).c_str());
    }

    void ApplyPendingResize() override {}  // size is polled in CanRender()
    bool TakeDpiChange(float&) override { return false; }

    bool CanRender() const override {
        SDL_GetWindowSize(window_, &width_, &height_);
        return width_ > 0 && height_ > 0;
    }

    void NewFrame() override {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
    }

    void Present(const float clear[4]) override {
        const ImGuiIO& io = ImGui::GetIO();
        glViewport(0, 0, int(io.DisplaySize.x * io.DisplayFramebufferScale.x),
                   int(io.DisplaySize.y * io.DisplayFramebufferScale.y));
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window_);
    }

    int Width() const override { return width_; }
    int Height() const override { return height_; }
    float DpiScale() const override { return dpiScale_; }
    void* NativeWindow() const override { return nullptr; }

    bool LoadSystemFont(bool mono, std::vector<unsigned char>& out) override {
        out.clear();
        const std::string fromFontconfig = FontconfigFile(mono ? "monospace" : "sans");
        if (!fromFontconfig.empty() && ReadWholeFile(fromFontconfig, out)) return true;
        static const char* kUi[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"};
        static const char* kMono[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"};
        for (const char* path : mono ? kMono : kUi)
            if (ReadWholeFile(path, out)) return true;
        return false;
    }

    // Same location the Linux driver searches ($XDG_CONFIG_HOME or ~/.config).
    std::wstring DefaultConfigPath() override {
        std::string base;
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) base = xdg;
        else if (const char* home = std::getenv("HOME")) base = std::string(home) + "/.config";
        if (base.empty()) return {};
        return Utf8ToWide(base + "/SuperVirtualMIDISynth/config.json");
    }

    bool FileExists(const std::wstring& path) override {
        struct stat st{};
        const std::string narrow = ToNarrow(path);
        return !narrow.empty() && stat(narrow.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }

    std::wstring Utf8ToWide(const std::string& utf8) override {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        try { return converter.from_bytes(utf8); } catch (...) { return {}; }
    }

private:
    // True if the window may close (no unsaved edits, or the user said so).
    bool ConfirmClose() {
        if (!hooks_.hasUnsavedChanges || !hooks_.hasUnsavedChanges()) return true;
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Keep open"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Close anyway"}};
        SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, window_,
                               "SuperVirtualMIDISynth V3",
                               "Configuration has unsaved changes.\nClose anyway?",
                               2, buttons, nullptr};
        int choice = 0;
        return SDL_ShowMessageBox(&box, &choice) == 0 && choice == 1;
    }

    Hooks hooks_;
    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    mutable int width_ = kDefaultWidth;
    mutable int height_ = kDefaultHeight;
    float dpiScale_ = 1.0f;
};

} // namespace

std::unique_ptr<Platform> CreatePlatform() {
    return std::make_unique<Sdl2Platform>();
}

} // namespace svms::cfg
