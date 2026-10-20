#include "configurator/ConfiguratorApp.h"
#include "SVMSBuildInfo.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    svms::cfg::ConfiguratorApp app;

    if (!app.Initialize(argc, argv)) {
        const wchar_t* detail = app.LastInitError();
        std::fprintf(stderr, "%ls\n",
                     (detail && detail[0] != L'\0') ? detail
                                                    : L"Failed to initialize the configurator.");
        return 1;
    }

    std::wstring windowTitle = L"SuperVirtualMIDISynth V3 Configurator - ";
    windowTitle += svms::build::kProductVersionWide;
    if (svms::build::kBuildNumber != 0u) {
        windowTitle += L" (build ";
        windowTitle += std::to_wstring(svms::build::kBuildNumber);
        windowTitle += L")";
    }
    app.SetWindowTitle(windowTitle.c_str());

    while (app.PumpMessages()) {
        // Minimised windows must not spin the render loop.
        if (app.IsMinimized()) {
            app.WaitForEvent();
            continue;
        }
        app.RenderFrame();
    }

    app.Shutdown();
    return 0;
}
