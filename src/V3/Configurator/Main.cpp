#include "ConfiguratorApp.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

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
        MessageBoxW(nullptr, L"Failed to initialize configurator.",
                    L"SVMS V3 Configurator", MB_OK | MB_ICONERROR);
        return 1;
    }

    while (app.PumpMessages()) {
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
