#include "ConfiguratorApp.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <string>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    svms::cfg::ConfiguratorApp app;

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char** argv = nullptr;

    if (wargv && argc > 0) {
        argv = new char*[static_cast<size_t>(argc)];
        for (int i = 0; i < argc; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                          nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                argv[i] = new char[static_cast<size_t>(len)];
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                    argv[i], len, nullptr, nullptr);
            } else {
                argv[i] = new char[1]{};
            }
        }
    }

    if (!app.Initialize(hInstance, argc, argv)) {
        const wchar_t* detail = app.LastInitError();
        std::wstring message = (detail && detail[0] != L'\0')
            ? std::wstring(detail)
            : std::wstring(L"Failed to initialize the configurator.");
        MessageBoxW(nullptr, message.c_str(), L"SVMS V3 Configurator",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    while (app.PumpMessages()) {
        // A minimized DXGI window is not guaranteed to block in Present().
        // Continuing to build ImGui/D3D11 frames while it is iconified can
        // therefore spin extremely fast and make the driver accumulate
        // transient WRITE_DISCARD backing allocations. Sleep on the Win32
        // message queue instead; the restore message wakes us immediately.
        if (app.IsMinimized()) {
            WaitMessage();
            continue;
        }
        app.RenderFrame();
    }

    app.Shutdown();

    if (argv) {
        for (int i = 0; i < argc; ++i) delete[] argv[i];
        delete[] argv;
    }
    if (wargv) LocalFree(wargv);

    return 0;
}
