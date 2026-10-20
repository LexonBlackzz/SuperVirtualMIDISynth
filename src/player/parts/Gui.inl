// Part of Player.cpp; included inside its anonymous namespace. Not standalone.

void PrintUsage() {
    std::puts(
        "svms_player — console MIDI player for SuperVirtualMIDISynth\n"
        "\n"
        "usage: svms_player [options] [file.mid]\n"
        "  (launching with no file — or no arguments at all — opens the\n"
        "   GUI window; pick the song and the synth inside it)\n"
        "  --backend <name>    svms (default) | kdapi | winmm\n"
        "  --dll <path>        synth module for the kdapi/winmm backends\n"
        "                      (default: OmniMIDI.dll / winmm.dll, app dir\n"
        "                      first — drop a synth DLL next to the player)\n"
        "  --sf2 <path>        load this soundfont into the engine (svms)\n"
        "  --ring-mb <n>       event ring budget in MB (default 64)\n"
        "  --lookahead-ms <n>  submit-ahead horizon (default 150, svms)\n"
        "  --loop              restart when the song ends\n"
        "  --auto <seconds>    quit automatically after N seconds of play\n"
        "  --quit-at-end       exit when the song finishes\n"
        "  --gui               ImGui window instead of the console UI\n"
        "                      (may be launched with no MIDI file — pick\n"
        "                      one, and a synth, inside the window)\n"
        "\n"
        "discovery (no MIDI file needed):\n"
        "  --scan [dir]        classify DLLs in dir (default: the player's\n"
        "                      own directory) — static export sniffing, no\n"
        "                      module is loaded\n"
        "  --probe <dll>       classify + handshake one module explicitly\n"
        "\n"
        "keys: SPACE pause/resume  S stop  LEFT/RIGHT seek 5s  "
        "UP/DOWN volume  L loop  Q quit");
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                            static_cast<int>(wide.size()), result.data(),
                            bytes, nullptr, nullptr);
    }
    return result;
}

// ── discovery modes (no MIDI file required) ─────────────────────────────

std::wstring ExeDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring dir(buffer, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

void PrintScanResults(const std::vector<svmscan::Entry>& entries,
                      const std::wstring& dir) {
    std::printf("scan: %s (%zu dll%s, none loaded)\n",
                WideToUtf8(dir).c_str(), entries.size(),
                entries.size() == 1 ? "" : "s");
    for (const svmscan::Entry& e : entries) {
        char sizeKb[32];
        sprintf(sizeKb, "%5llu KB",
                static_cast<unsigned long long>(e.fileSize / 1024));
        std::printf("  %-7s %-5s %s  %-28ls %ls\n",
                    e.loadable ? e.kind.c_str()
                               : (e.kind == "runtime" ? "runtime" : "-"),
                    e.arch.c_str(), sizeKb,
                    e.displayName.c_str(), e.filename.c_str());
    }
    const int loadable = static_cast<int>(std::count_if(
        entries.begin(), entries.end(),
        [](const svmscan::Entry& e) { return e.loadable; }));
    std::printf("  %d usable synth%s; probe one with: svms_player --probe <dll>\n",
                loadable, loadable == 1 ? "" : "s");
}

int RunScanMode(const std::wstring& dir) {
    PrintScanResults(svmscan::ScanDirectory(dir), dir);
    return 0;
}

int RunProbeMode(const std::wstring& dllPath) {
    // Build a synthetic entry so classification drives the probe choice.
    svmscan::Entry entry;
    entry.path = dllPath;
    const size_t slash = dllPath.find_last_of(L"\\/");
    entry.filename = (slash != std::wstring::npos)
                         ? dllPath.substr(slash + 1)
                         : dllPath;
    entry.displayName = svmscan::ResolveDisplayName(entry.path,
                                                    entry.filename);
    if (svmscan::IsDenylisted(entry.filename)) {
        entry.kind = "runtime";
    } else {
        const svmscan::PeExports pe = svmscan::ReadExports(entry.path);
        entry.kind = svmscan::ClassifyKind(pe);
        entry.arch = svmscan::ArchName(pe.machine);
    }
    std::printf("probe: %s\n  class=%s arch=%s name=\"%ls\"\n",
                WideToUtf8(dllPath).c_str(), entry.kind.c_str(),
                entry.arch.c_str(), entry.displayName.c_str());
    if (entry.kind != "svms" && entry.kind != "kdapi" &&
        entry.kind != "winmm") {
        std::printf("  refusing to load: not a synth\n");
        return 1;
    }
    const svmscan::ProbeResult r = svmscan::Probe(entry);
    std::printf("  %s%s\n", r.ok ? "OK  " : "FAIL", r.detail.c_str());
    return r.ok ? 0 : 1;
}

// ── GUI host (Win32 + DX11 + ImGui) ─────────────────────────────────────
// Same proven bootstrap the configurator uses: Win32 window -> D3D11 device
// + swap chain (WARP fallback) -> ImGui with the win32/dx11 backends.

class PlayerGuiHost {
public:
    bool Create(const wchar_t* title, int width, int height) {
        self_ = this;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.lpszClassName = L"SVMSPlayerGui";
        RegisterClassExW(&wc);

        RECT rc{0, 0, width, height};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(
            0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
            rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd_) return Fail("CreateWindowExW failed");

        if (!CreateD3D()) { Destroy(); return false; }
        if (!CreateImGui()) { Destroy(); return false; }
        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);
        return true;
    }

    void Destroy() {
        DestroyImGui();
        DestroyD3D();
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (self_ == this) self_ = nullptr;
        UnregisterClassW(L"SVMSPlayerGui", GetModuleHandleW(nullptr));
    }

    bool Pump() {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running_ = false;
        }
        return running_;
    }

    void FrameBegin() {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void FrameEnd() {
        ImGui::Render();
        if (target_) {
            context_->OMSetRenderTargets(1, &target_, nullptr);
            const float clear[4] = {0.06f, 0.06f, 0.08f, 1.0f};
            context_->ClearRenderTargetView(target_, clear);
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (swapChain_) swapChain_->Present(1, 0);
    }

    void Close() { running_ = false; }
    bool IsRunning() const { return running_; }
    HWND Hwnd() const { return hwnd_; }

private:
    bool Fail(const char* what) {
        std::fprintf(stderr, "error: %s (GetLastError=%lu)\n", what,
                     static_cast<unsigned long>(GetLastError()));
        return false;
    }

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;
        switch (msg) {
        case WM_SIZE:
            if (self_ && wParam != SIZE_MINIMIZED)
                self_->Resize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }
    }

    bool CreateD3D() {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate = {60, 1};
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd_;
        sd.SampleDesc = {1, 0};
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0,
                                             D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &swapChain_, &device_, nullptr,
            &context_);
        if (FAILED(hr))
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                D3D11_SDK_VERSION, &sd, &swapChain_, &device_, nullptr,
                &context_);
        if (FAILED(hr)) {
            std::fprintf(stderr,
                         "error: D3D11CreateDeviceAndSwapChain failed "
                         "(hr=0x%08lx)\n",
                         static_cast<unsigned long>(hr));
            return false;
        }
        return RecreateTarget();
    }

    bool RecreateTarget() {
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
            return false;
        HRESULT hr = device_->CreateRenderTargetView(backBuffer, nullptr,
                                                     &target_);
        backBuffer->Release();
        return SUCCEEDED(hr);
    }

    void Resize(int width, int height) {
        if (!swapChain_ || width <= 0 || height <= 0) return;
        if (target_) { target_->Release(); target_ = nullptr; }
        swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        RecreateTarget();
    }

    bool CreateImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        if (!ImGui_ImplWin32_Init(hwnd_)) return false;
        if (!ImGui_ImplDX11_Init(device_, context_)) return false;
        return true;
    }

    void DestroyImGui() {
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
    }

    void DestroyD3D() {
        if (target_) { target_->Release(); target_ = nullptr; }
        if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
        if (context_) { context_->Release(); context_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
    }

    HWND hwnd_ = nullptr;
    bool running_ = true;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* target_ = nullptr;
    static PlayerGuiHost* self_;
};

PlayerGuiHost* PlayerGuiHost::self_ = nullptr;

// ── GUI playback loop ───────────────────────────────────────────────────
// Same shared ticks as the console loop (SampleTelemetryTick /
// PlaybackEventsTick / FeedRateTick); only input and rendering differ.
// The library panel switches synth DLLs at runtime; songs can be opened
// in-window, so the player is launchable with no CLI file argument.

void RunGuiMode(PlayerCore& core, const EngineSpec& startupEngine,
                uint32_t lookaheadMs, double autoSeconds, bool& loop,
                bool quitAtEnd, UiShared& ui) {
    PlayerGuiHost host;
    if (!host.Create(L"SVMS Player", 960, 640)) {
        ui.running = false;
        return;
    }

    std::vector<svmscan::Entry> library;
    bool libraryScanned = false;
    std::string guiStatus;
    EngineSpec activeSpec = startupEngine;
    std::wstring activePath;   // empty = default engine from the CLI

    while (ui.running && !core.quit.load(std::memory_order_relaxed) &&
           host.Pump()) {
        host.FrameBegin();

        const int64_t playhead =
            core.playhead.load(std::memory_order_relaxed);
        const char* stateName = PlayerStateName(core, ui.songEnded);
        const uint32_t state = core.state.load(std::memory_order_relaxed);
        const bool svmsBackend = core.backend == kBackendSvms;
        // Derived from the live engine state every frame — never a cached
        // flag — so the UI can never call into an engine that is not loaded.
        const bool engineUp =
            core.backend == kBackendSvms
                ? (core.session != 0 &&
                   core.api.fn.get_telemetry != nullptr)
                : core.legacy.started;
        const bool haveSong = engineUp && core.totalFrames > 0;

        char versionLabel[96];
        if (svmsBackend) {
            snprintf(versionLabel, sizeof(versionLabel),
                     "SVMSAPI %u.%u.%u (API %u)",
                     core.api.fn.product_major, core.api.fn.product_minor,
                     core.api.fn.product_patch, core.api.fn.abi_version);
        } else {
            char moduleAscii[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, core.legacy.moduleName, -1,
                                moduleAscii, sizeof(moduleAscii), nullptr,
                                nullptr);
            snprintf(versionLabel, sizeof(versionLabel), "%s — %.60s",
                     core.backend == kBackendKdapi ? "KDMAPI" : "WinMM",
                     moduleAscii);
        }
        if (!engineUp) {
            snprintf(versionLabel, sizeof(versionLabel),
                     "<no synth loaded — pick one in the library>");
        }
        const std::string backendLabel(versionLabel);
        const std::string fileUtf8 = WideToUtf8(
            core.filePath.empty() ? std::wstring(L"<no song loaded>")
                                  : core.filePath);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##player", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse);

        ImGui::Text("SVMS Player  [%s]  %s", stateName,
                    backendLabel.c_str());
        ImGui::TextDisabled("%s", fileUtf8.c_str());
        ImGui::Separator();

        // ── Transport (needs a loaded song) ─────────────────────────────
        if (haveSong) {
        if (ImGui::Button(state == kStatePlaying ? "Pause" : "Play")) {
            if (state == kStatePlaying) {
                core.state.store(kStatePausing, std::memory_order_release);
            } else if (state == kStatePaused) {
                StartPlayback(&core);
            } else if (state == kStateIdle) {
                SeekTo(&core, 0);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            SeekTo(&core, 0);
            core.state.store(kStateIdle, std::memory_order_release);
        }
        ImGui::SameLine();
        if (ImGui::Button("-5s"))
            SeekTo(&core, static_cast<uint64_t>(playhead -
                                                5i64 * core.sampleRate));
        ImGui::SameLine();
        if (ImGui::Button("+5s"))
            SeekTo(&core, static_cast<uint64_t>(playhead +
                                                5i64 * core.sampleRate));
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &loop);
        ImGui::SameLine();
        if (ImGui::Button("Quit")) ui.running = false;

        // Seek slider commits on release (avoid seek storms while dragging).
        float progress = core.totalFrames
            ? static_cast<float>(static_cast<double>(playhead) /
                                 static_cast<double>(core.totalFrames) *
                                 100.0)
            : 0.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 100.0f) progress = 100.0f;
        float seekSlider = progress;
        if (ImGui::SliderFloat("##seek", &seekSlider, 0.0f, 100.0f, "%.1f%%") &&
            ImGui::IsItemDeactivatedAfterEdit() && core.totalFrames) {
            SeekTo(&core, static_cast<uint64_t>(
                              seekSlider / 100.0f *
                              static_cast<double>(core.totalFrames)));
        }
        } else {
            ImGui::TextDisabled("no song loaded — open a MIDI file below");
            ImGui::SameLine();
            if (ImGui::Button("Quit")) ui.running = false;
        }

        // ── Volume ──────────────────────────────────────────────────────
        // Slider works in 0..100% for display; ui.volume stays 0..1.
        float volumePercent = ui.volume * 100.0f;
        if (engineUp &&
            ImGui::SliderFloat("Volume", &volumePercent, 0.0f, 100.0f,
                               "%.0f%%") &&
            ImGui::IsItemDeactivatedAfterEdit() &&
            volumePercent != ui.volume * 100.0f) {
            ui.volume = volumePercent / 100.0f;
            if (svmsBackend) PatchMasterVolume(core, ui.volume);
            else core.legacy.SendVolume(ui.volume);
        }

        // ── Song open (needs a synth — load one in the library first) ───
        if (!engineUp) {
            ImGui::TextDisabled("open a synth in the library panel first, "
                                "then pick a song");
        } else if (ImGui::Button("Open MIDI...")) {
            wchar_t midiPath[MAX_PATH] = {};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = host.Hwnd();
            ofn.lpstrFilter =
                L"MIDI files (*.mid;*.midi)\0*.mid;*.midi\0"
                L"All files (*.*)\0*.*\0";
            ofn.lpstrFile = midiPath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                // Swap the persistent mapping: stop the decoder first (it
                // reads g_songFile), then re-open, scan, and restart.
                core.state.store(kStateSeeking, std::memory_order_release);
                StopDecoder();
                g_songFile.Close();
                svms::MidiStreamInfo info{};
                if (!g_songFile.Open(ofn.lpstrFile, guiStatus)) {
                    core.totalFrames = 0;
                    core.state.store(kStateIdle, std::memory_order_release);
                } else if (!svms::MidiStreamDecoder{}.Scan(
                               g_songFile, core.sampleRate, info, guiStatus)) {
                    core.totalFrames = 0;
                    core.state.store(kStateIdle, std::memory_order_release);
                } else {
                    core.totalFrames = info.totalFrames;
                    if (LoadSong(core, ofn.lpstrFile)) {
                        ui.songEnded = false;
                        char summary[128];
                        snprintf(summary, sizeof(summary),
                                 "loaded: %llu events, %.1f s",
                                 static_cast<unsigned long long>(
                                     info.eventCount),
                                 static_cast<double>(info.totalFrames) /
                                     core.sampleRate);
                        guiStatus = summary;
                    } else {
                        guiStatus = "load failed: no synth is loaded";
                    }
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", guiStatus.c_str());
        // ── Library: synth DLLs next to the player ──────────────────────
        if (ImGui::CollapsingHeader("Synth library")) {
            if (!libraryScanned) {
                if (ImGui::Button("Scan")) {
                    library = svmscan::ScanDirectory(ExeDirectory());
                    libraryScanned = true;
                    guiStatus = library.empty()
                                    ? std::string("no DLLs found")
                                    : std::to_string(library.size()) +
                                          " DLLs found";
                }
            } else {
                if (ImGui::Button("Rescan"))
                    library = svmscan::ScanDirectory(ExeDirectory());
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "static export scan — nothing loads until you click Load");
                if (ImGui::BeginTable("synths", 5,
                                      ImGuiTableFlags_Borders |
                                          ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("name");
                    ImGui::TableSetupColumn("file");
                    ImGui::TableSetupColumn("type");
                    ImGui::TableSetupColumn("arch");
                    ImGui::TableSetupColumn("action");
                    ImGui::TableHeadersRow();
                    for (const svmscan::Entry& entry : library) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            WideToUtf8(entry.displayName).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%ls", entry.filename.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(entry.kind.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(entry.arch.c_str());
                        ImGui::TableNextColumn();
                        const bool active =
                            !activePath.empty() &&
                            _wcsicmp(activePath.c_str(),
                                     entry.path.c_str()) == 0;
                        if (active) {
                            ImGui::TextColored(
                                ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "<active>");
                        } else if (!entry.loadable) {
                            ImGui::TextDisabled("-");
                        } else {
                            ImGui::PushID(entry.filename.c_str());
                            const bool clicked = ImGui::Button("Load");
                            ImGui::PopID();
                            if (clicked) {
                                EngineSpec spec;
                                if (entry.kind == "svms") {
                                    spec.svms = true;
                                    spec.apiPath = entry.path;
                                } else if (entry.kind == "kdapi") {
                                    spec.svms = false;
                                    spec.legacyKind =
                                        LegacySink::kKindKdapi;
                                    spec.legacyPath = entry.path;
                                } else {
                                    spec.svms = false;
                                    spec.legacyKind =
                                        LegacySink::kKindWinmm;
                                    spec.legacyPath = entry.path;
                                }
                                spec.soundfont = activeSpec.soundfont;
                                const EngineSpec previous = activeSpec;
                                std::string switchError;
                                if (SwitchEngine(core, spec, lookaheadMs,
                                                 switchError)) {
                                    activeSpec = spec;
                                    activePath = entry.path;
                                    ui.songEnded = false;
                                    guiStatus = "loaded " +
                                                WideToUtf8(entry.filename);
                                    if (core.backend == kBackendSvms)
                                        ui.volume = ReadMasterVolume(core);
                                    else
                                        core.legacy.SendVolume(ui.volume);
                                } else {
                                    guiStatus = "load failed: " +
                                                switchError +
                                                " — restoring previous";
                                    std::string restoreError;
                                    if (SwitchEngine(core, previous,
                                                     lookaheadMs,
                                                     restoreError))
                                        guiStatus += " (restored)";
                                    else
                                        guiStatus += " (RESTORE FAILED: " +
                                                     restoreError + ")";
                                }
                            }
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
        // ── Telemetry graphs ────────────────────────────────────────────
        // 2×2 grid: MIDI polyphony (player-side), synth voices, NPS, and
        // render CPU load (percent of the engine's frame budget).
        float voiceMax = 1.0f, renderMax = 1.0f, npsMax = 1.0f, polyMax = 1.0f;
        for (uint32_t i = 0; i < ui.historyCount; ++i) {
            if (ui.voiceHistory[i] > voiceMax) voiceMax = ui.voiceHistory[i];
            if (ui.renderHistory[i] > renderMax)
                renderMax = ui.renderHistory[i];
            if (ui.npsHistory[i] > npsMax) npsMax = ui.npsHistory[i];
            if (ui.polyHistory[i] > polyMax) polyMax = ui.polyHistory[i];
        }
        const ImVec4 polyColor(1.0f, 0.7f, 0.25f, 1.0f);   // amber
        const ImVec4 voiceColor(0.35f, 0.75f, 1.0f, 1.0f); // blue
        const ImVec4 npsColor(0.45f, 1.0f, 0.55f, 1.0f);   // green
        const float colGap = 16.0f;
        const float colWidth =
            (ImGui::GetContentRegionAvail().x - colGap) * 0.5f;

        ImGui::TextColored(polyColor, "MIDI POLYPHONY");
        ImGui::SameLine(colWidth + colGap);
        ImGui::TextColored(voiceColor, "VOICES (synth)");
        ImGui::PushStyleColor(ImGuiCol_PlotLines, polyColor);
        ImGui::PlotLines("##midipoly", ui.polyHistory, 128, ui.historyIndex,
                         nullptr, 0.0f, polyMax, ImVec2(colWidth, 58));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotLines, voiceColor);
        ImGui::PlotLines("##voices", ui.voiceHistory, 128, ui.historyIndex,
                         nullptr, 0.0f, voiceMax, ImVec2(-1, 58));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextColored(polyColor, "now %u", ui.currentMidiPoly);
        ImGui::SameLine();
        ImGui::TextDisabled("peak %.0f", static_cast<double>(polyMax));
        ImGui::SameLine(colWidth + colGap);
        ImGui::TextColored(voiceColor, "now %u", ui.telemetry.active_voices);
        ImGui::SameLine();
        ImGui::TextDisabled("peak %.0f", static_cast<double>(voiceMax));

        ImGui::Spacing();

        ImGui::TextColored(npsColor, "NPS — NOTES / SECOND");
        ImGui::SameLine(colWidth + colGap);
        // Frame budget: buffer_frames / sample_rate. Render time as a
        // percentage of that budget is the engine's real-time CPU load.
        const float budgetMs = (ui.telemetry.buffer_frames &&
                                ui.telemetry.sample_rate)
            ? static_cast<float>(ui.telemetry.buffer_frames) * 1000.0f /
                  static_cast<float>(ui.telemetry.sample_rate)
            : 0.0f;
        const float cpuNow =
            budgetMs ? ui.telemetry.render_time_ms / budgetMs * 100.0f : 0.0f;
        const float cpuPeak = budgetMs ? renderMax / budgetMs * 100.0f : 0.0f;
        const ImVec4 renderColor =
            cpuNow < 50.0f ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f)
            : cpuNow < 80.0f ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
                             : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(renderColor, "RENDER — CPU of frame budget");

        ImGui::PushStyleColor(ImGuiCol_PlotLines, npsColor);
        ImGui::PlotLines("##nps", ui.npsHistory, 128, ui.historyIndex,
                         nullptr, 0.0f, npsMax, ImVec2(colWidth, 58));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (svmsBackend) {
            // Scale the history to percent so the plot IS the CPU load.
            float cpuHistory[128];
            const float scale = budgetMs > 0.0f ? 100.0f / budgetMs : 0.0f;
            for (uint32_t i = 0; i < 128; ++i)
                cpuHistory[i] = ui.renderHistory[i] * scale;
            ImGui::PushStyleColor(ImGuiCol_PlotLines, renderColor);
            ImGui::PlotLines("##render", cpuHistory, 128, ui.historyIndex,
                             nullptr, 0.0f, cpuPeak > 0.0f ? cpuPeak : 1.0f,
                             ImVec2(-1, 58));
            ImGui::PopStyleColor();
        } else {
            ImGui::Dummy(ImVec2(-1, 58));
        }
        ImGui::Spacing();
        ImGui::TextColored(npsColor, "now %.0f",
                           static_cast<double>(ui.currentNps));
        ImGui::SameLine();
        ImGui::TextDisabled("peak %.0f", static_cast<double>(npsMax));
        ImGui::SameLine(colWidth + colGap);
        if (svmsBackend) {
            ImGui::TextColored(renderColor, "now %.0f%% (%.2f ms)",
                               static_cast<double>(cpuNow),
                               ui.telemetry.render_time_ms);
            ImGui::SameLine();
            ImGui::TextDisabled("peak %.0f%%",
                                static_cast<double>(cpuPeak));
        } else {
            ImGui::TextDisabled("SVMS telemetry only");
        }
        ImGui::Spacing();

        ImGui::Separator();
        const uint64_t ringCapacity = core.ring->Capacity();
        const uint32_t ringPercent = static_cast<uint32_t>(
            ringCapacity ? core.ring->Size() * 100u / ringCapacity : 0u);
        ImGui::Text("events %llu   feed %llu evt/s   ring %u%%   steals %llu",
                    static_cast<unsigned long long>(
                        core.submittedEvents.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(ui.feedRatePerSecond),
                    ringPercent,
                    static_cast<unsigned long long>(
                        ui.telemetry.voice_steals));
        ImGui::Text("audio %s   sf2 %s   rate %u   volume %.0f%%",
                    ui.telemetry.audio_running ? "running" : "NOT RUNNING",
                    ui.telemetry.soundfont_loaded ? "loaded" : "none",
                    ui.telemetry.sample_rate, ui.volume * 100.0f);
        if (!ui.telemetry.audio_running) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "WARNING: engine audio is not running");
        }

        // ── Keyboard (mirrors the console keys) ─────────────────────────
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (haveSong && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                if (state == kStatePlaying)
                    core.state.store(kStatePausing,
                                     std::memory_order_release);
                else if (state == kStatePaused)
                    StartPlayback(&core);
                else if (state == kStateIdle)
                    SeekTo(&core, 0);
            }
            if (haveSong && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                SeekTo(&core, 0);
                core.state.store(kStateIdle, std::memory_order_release);
            }
            if (haveSong && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
                SeekTo(&core, static_cast<uint64_t>(
                                  playhead - 5i64 * core.sampleRate));
            if (haveSong && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
                SeekTo(&core, static_cast<uint64_t>(
                                  playhead + 5i64 * core.sampleRate));
            if (engineUp && ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                ui.volume += 0.1f;
                if (ui.volume > 1.0f) ui.volume = 1.0f;
                if (svmsBackend) PatchMasterVolume(core, ui.volume);
                else core.legacy.SendVolume(ui.volume);
            }
            if (engineUp && ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                ui.volume -= 0.1f;
                if (ui.volume < 0.0f) ui.volume = 0.0f;
                if (svmsBackend) PatchMasterVolume(core, ui.volume);
                else core.legacy.SendVolume(ui.volume);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_L, false)) loop = !loop;
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ui.running = false;
        }

        ImGui::End();

        host.FrameEnd();
        if (engineUp) SampleTelemetryTick(core, svmsBackend, ui);
        PlaybackEventsTick(core, svmsBackend, autoSeconds, loop, quitAtEnd,
                           ui);
        FeedRateTick(core, ui);
    }
    host.Destroy();
}
