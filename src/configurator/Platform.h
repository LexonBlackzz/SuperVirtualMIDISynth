#ifndef SVMS_CONFIGURATOR_PLATFORM_H
#define SVMS_CONFIGURATOR_PLATFORM_H

// Everything the configurator needs from the host OS: window, graphics
// device, ImGui backends, event pump, system fonts and the default config
// location. ConfiguratorApp talks only to this interface so the app code
// contains no windowing or graphics API. Platform_Win32.cpp (Win32 + D3D11)
// is the only implementation today; an SDL2 one slots in beside it.

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace svms::cfg {

// Files dropped on the window (UTF-8 paths), queued by the platform layer and
// drained by the page that wants them.
inline std::vector<std::string>& DroppedFileQueue() {
    static std::vector<std::string> queue;
    return queue;
}
inline std::vector<std::string> TakeDroppedFiles() {
    std::vector<std::string> out;
    out.swap(DroppedFileQueue());
    return out;
}

class Platform {
public:
    struct Hooks {
        // True while the config has unsaved edits (asked when the window is closed).
        std::function<bool()> hasUnsavedChanges;
    };

    virtual ~Platform() = default;

    // Window + graphics device. On failure `error` holds user-facing text.
    virtual bool CreateWindowAndDevice(const Hooks& hooks, std::wstring& error) = 0;
    virtual void DestroyWindowAndDevice() = 0;
    // ImGui platform/renderer backends; needs a live ImGui context.
    virtual bool InitBackends(std::wstring& error) = 0;
    virtual void ShutdownBackends() = 0;
    // Drop GPU font objects before the atlas is rebuilt (DPI change).
    virtual void InvalidateFontObjects() = 0;

    // Returns false once the user has asked to quit.
    virtual bool PumpEvents() = 0;
    virtual bool IsMinimized() const = 0;
    virtual void WaitForEvent() = 0;
    virtual void SetTitle(const wchar_t* title) = 0;

    // Applies a deferred window resize to the swap chain / framebuffer.
    virtual void ApplyPendingResize() = 0;
    // True once after the monitor scale changed; `scale` is the new factor.
    virtual bool TakeDpiChange(float& scale) = 0;
    virtual bool CanRender() const = 0;
    virtual void NewFrame() = 0;
    // Renders ImGui's draw data over `clear` (RGBA) and presents.
    virtual void Present(const float clear[4]) = 0;

    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual float DpiScale() const = 0;
    // Native window handle (HWND on Windows) for OS file dialogs.
    virtual void* NativeWindow() const = 0;

    // Raw TTF bytes of a system UI font (mono = LCD/engraving face).
    virtual bool LoadSystemFont(bool mono, std::vector<unsigned char>& out) = 0;
    virtual std::wstring DefaultConfigPath() = 0;
    virtual bool FileExists(const std::wstring& path) = 0;
    virtual std::wstring Utf8ToWide(const std::string& utf8) = 0;
};

std::unique_ptr<Platform> CreatePlatform();

} // namespace svms::cfg

#endif
