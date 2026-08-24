#include "PageOffline.h"

#include "ConfigDocument.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <sstream>
#include <vector>

#include <commdlg.h>

namespace svms::cfg {
namespace {

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                              static_cast<int>(value.size()),
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), required,
                        nullptr, nullptr);
    return result;
}

std::wstring ParentDirectory(const std::wstring& path) {
    if (path.empty()) return {};
    try {
        return std::filesystem::path(path).parent_path().wstring();
    } catch (const std::filesystem::filesystem_error&) {
        return {};
    }
}

bool BrowseOpenFile(HWND owner, const wchar_t* title, const wchar_t* filter,
                    const std::wstring& initialDirectory,
                    std::wstring& selected) {
    std::vector<wchar_t> buffer(32768u, L'\0');
    if (!selected.empty()) {
        wcsncpy_s(buffer.data(), buffer.size(), selected.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrInitialDir = initialDirectory.empty()
        ? nullptr : initialDirectory.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;
    selected = buffer.data();
    return true;
}

bool BrowseSaveFile(HWND owner, const std::wstring& initialDirectory,
                    std::wstring& selected) {
    std::vector<wchar_t> buffer(32768u, L'\0');
    if (!selected.empty()) {
        wcsncpy_s(buffer.data(), buffer.size(), selected.c_str(), _TRUNCATE);
    }
    static const wchar_t filter[] =
        L"Wave audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = L"Select output WAV";
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrInitialDir = initialDirectory.empty()
        ? nullptr : initialDirectory.c_str();
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return false;
    selected = buffer.data();
    if (std::filesystem::path(selected).extension().empty())
        selected += L".wav";
    return true;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> fields;
    size_t begin = 0;
    for (;;) {
        const size_t end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end - begin));
        if (end == std::string::npos) break;
        begin = end + 1u;
    }
    return fields;
}

bool ParseDouble(const std::string& text, double& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

bool ParseU64(const std::string& text, uint64_t& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

std::string FormatDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) return "--:--:--";
    const uint64_t total = static_cast<uint64_t>(seconds + 0.5);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(total / 3600u),
                  static_cast<unsigned long long>((total / 60u) % 60u),
                  static_cast<unsigned long long>(total % 60u));
    return text;
}

const char* StateLabel(OfflineRendererPage::State state) {
    switch (state) {
    case OfflineRendererPage::State::Idle:       return "READY";
    case OfflineRendererPage::State::Starting:   return "STARTING";
    case OfflineRendererPage::State::Loading:    return "LOADING MIDI";
    case OfflineRendererPage::State::Preparing:  return "PREPARING";
    case OfflineRendererPage::State::Rendering:  return "RENDERING";
    case OfflineRendererPage::State::Tail:       return "RELEASE TAIL";
    case OfflineRendererPage::State::Cancelling: return "CANCELLING";
    case OfflineRendererPage::State::Complete:   return "COMPLETE";
    case OfflineRendererPage::State::Cancelled:  return "CANCELLED";
    case OfflineRendererPage::State::Error:      return "ERROR";
    }
    return "UNKNOWN";
}

bool IsRunningState(OfflineRendererPage::State state) {
    return state == OfflineRendererPage::State::Starting ||
           state == OfflineRendererPage::State::Loading ||
           state == OfflineRendererPage::State::Preparing ||
           state == OfflineRendererPage::State::Rendering ||
           state == OfflineRendererPage::State::Tail ||
           state == OfflineRendererPage::State::Cancelling;
}

void PathRow(const char* label, const std::wstring& path,
             const char* buttonLabel, const std::function<void()>& browse) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    const std::string utf8 = WideToUtf8(path);
    if (utf8.empty()) ImGui::TextDisabled("Not selected");
    else {
        ImGui::TextWrapped("%s", utf8.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(utf8.c_str());
            ImGui::EndTooltip();
        }
    }
    ImGui::TableNextColumn();
    if (ImGui::Button(buttonLabel, ImVec2(92.0f, 0.0f))) browse();
}

} // namespace

OfflineRendererPage::~OfflineRendererPage() { Shutdown(); }

std::wstring OfflineRendererPage::ExecutableDirectory() {
    std::vector<wchar_t> path(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0u || length >= path.size()) return L".";
    return std::filesystem::path(path.data()).parent_path().wstring();
}

std::wstring OfflineRendererPage::QuoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'\"') {
            result.append(slashes * 2u + 1u, L'\\');
            result.push_back(L'\"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(c);
        }
    }
    result.append(slashes * 2u, L'\\');
    result.push_back(L'\"');
    return result;
}

void OfflineRendererPage::EnsureInitialPaths(const ConfigDocument& document) {
    if (pathsInitialized_) return;
    pathsInitialized_ = true;
    const std::wstring configured = document.Working().soundFontPath;
    if (!configured.empty()) {
        std::filesystem::path path(configured);
        if (path.is_relative()) path = ExecutableDirectory() / path;
        soundFontPath_ = path.lexically_normal().wstring();
    } else {
        const std::filesystem::path fallback =
            std::filesystem::path(ExecutableDirectory()) / L"gm.sf2";
        if (GetFileAttributesW(fallback.c_str()) != INVALID_FILE_ATTRIBUTES)
            soundFontPath_ = fallback.wstring();
    }
    lastSoundFontDirectory_ = ParentDirectory(soundFontPath_);
}

void OfflineRendererPage::BrowseMidi(HWND owner) {
    static const wchar_t filter[] =
        L"MIDI files (*.mid;*.midi)\0*.mid;*.midi\0All files (*.*)\0*.*\0\0";
    std::wstring selected = midiPath_;
    if (!BrowseOpenFile(owner, L"Select MIDI file", filter,
                        lastMidiDirectory_, selected)) return;
    midiPath_ = selected;
    lastMidiDirectory_ = ParentDirectory(selected);
    if (outputPath_.empty() || outputPathAutomatic_) {
        std::filesystem::path output(selected);
        output.replace_extension(L".wav");
        outputPath_ = output.wstring();
        lastOutputDirectory_ = output.parent_path().wstring();
        outputPathAutomatic_ = true;
    }
}

void OfflineRendererPage::BrowseSoundFont(HWND owner) {
    static const wchar_t filter[] =
        L"SoundFont 2 files (*.sf2)\0*.sf2\0All files (*.*)\0*.*\0\0";
    std::wstring selected = soundFontPath_;
    if (!BrowseOpenFile(owner, L"Select SoundFont", filter,
                        lastSoundFontDirectory_, selected)) return;
    soundFontPath_ = selected;
    lastSoundFontDirectory_ = ParentDirectory(selected);
}

void OfflineRendererPage::BrowseOutput(HWND owner) {
    std::wstring selected = outputPath_;
    if (!BrowseSaveFile(owner, lastOutputDirectory_, selected)) return;
    outputPath_ = selected;
    lastOutputDirectory_ = ParentDirectory(selected);
    outputPathAutomatic_ = false;
}

void OfflineRendererPage::JoinFinishedWorker() {
    if (worker_.joinable() && !workerRunning_.load(std::memory_order_acquire))
        worker_.join();
}

void OfflineRendererPage::Start(const ConfigDocument& document) {
    JoinFinishedWorker();
    if (shutdown_ || workerRunning_.load(std::memory_order_acquire)) return;

    const std::filesystem::path renderer =
        std::filesystem::path(ExecutableDirectory()) / L"svms_v3_render.exe";
    if (GetFileAttributesW(renderer.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.state = State::Error;
        snapshot_.message = "Offline renderer executable was not found beside the Configurator.";
        snapshot_.detail = WideToUtf8(renderer.wstring());
        return;
    }

    static std::atomic<uint32_t> serial{0u};
    wchar_t eventName[128]{};
    swprintf_s(eventName, L"Local\\SVMSOfflineCancel_%lu_%u",
               GetCurrentProcessId(), serial.fetch_add(1u));
    HANDLE cancelEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (!cancelEvent) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.state = State::Error;
        snapshot_.message = "Could not create the render cancellation event.";
        return;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 64u * 1024u) ||
        !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        CloseHandle(cancelEvent);
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.state = State::Error;
        snapshot_.message = "Could not create the renderer progress pipe.";
        return;
    }

    const ConfigValues& values = document.Working();
    auto number = [](double value) {
        wchar_t text[64]{};
        swprintf_s(text, L"%.9g", value);
        return std::wstring(text);
    };
    std::wstring command = QuoteArgument(renderer.wstring());
    auto append = [&](const std::wstring& value) {
        command.push_back(L' ');
        command += QuoteArgument(value);
    };
    append(midiPath_);
    append(soundFontPath_);
    append(outputPath_);
    append(L"--sample-rate"); append(std::to_wstring(values.sampleRate));
    append(L"--max-voices"); append(std::to_wstring(values.maxVoices));
    append(L"--render-threads"); append(std::to_wstring(values.renderThreads));
    append(L"--master-volume"); append(number(values.masterVolume));
    append(L"--limiter"); append(values.limiterEnabled ? L"on" : L"off");
    append(L"--limiter-algorithm");
    append(values.limiterAlgorithm == 1u ? L"adaptive" : L"classic");
    append(L"--limiter-threshold"); append(number(values.limiterThreshold));
    append(L"--limiter-lookahead-ms"); append(number(values.limiterLookaheadMs));
    append(L"--limiter-attack-ms"); append(number(values.limiterAttackMs));
    append(L"--limiter-release-ms"); append(number(values.limiterReleaseMs));
    append(L"--backend"); append(L"auto");
    append(L"--machine-progress");
    append(L"--cancel-event"); append(eventName);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION processInfo{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL created = CreateProcessW(
        renderer.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, ExecutableDirectory().c_str(),
        &startup, &processInfo);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        CloseHandle(cancelEvent);
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.state = State::Error;
        snapshot_.message = "Could not start the offline renderer.";
        snapshot_.detail = "CreateProcess failed with Windows error " +
                           std::to_string(GetLastError());
        return;
    }
    CloseHandle(processInfo.hThread);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_ = Snapshot{};
        snapshot_.state = State::Starting;
        snapshot_.message = "Starting standalone renderer...";
        process_ = processInfo.hProcess;
        cancelEvent_ = cancelEvent;
    }
    workerRunning_.store(true, std::memory_order_release);
    worker_ = std::thread(&OfflineRendererPage::ReadProcessOutput, this,
                          readPipe, processInfo.hProcess);
}

void OfflineRendererPage::Cancel() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (cancelEvent_) SetEvent(cancelEvent_);
    if (IsRunningState(snapshot_.state)) {
        snapshot_.state = State::Cancelling;
        snapshot_.message = "Cancellation requested; finalizing the partial WAV...";
    }
}

void OfflineRendererPage::Shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    Cancel();
    HANDLE processCopy = nullptr;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (process_) {
            if (!DuplicateHandle(GetCurrentProcess(), process_,
                                 GetCurrentProcess(), &processCopy,
                                 SYNCHRONIZE | PROCESS_TERMINATE,
                                 FALSE, 0u)) {
                TerminateProcess(process_, 4u);
            }
        }
    }
    if (processCopy) {
        if (WaitForSingleObject(processCopy, 5000u) == WAIT_TIMEOUT)
            TerminateProcess(processCopy, 4u);
        CloseHandle(processCopy);
    }
    if (worker_.joinable()) worker_.join();
}

void OfflineRendererPage::ReadProcessOutput(HANDLE readPipe, HANDLE process) {
    std::string pending;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read) {
        pending.append(buffer, buffer + read);
        size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1u);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            HandleProtocolLine(line);
        }
    }
    if (!pending.empty()) HandleProtocolLine(pending);
    CloseHandle(readPipe);

    WaitForSingleObject(process, INFINITE);
    DWORD exitCode = 1u;
    GetExitCodeProcess(process, &exitCode);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (IsRunningState(snapshot_.state)) {
            if (exitCode == 0u) {
                snapshot_.state = State::Complete;
                snapshot_.message = "Render completed successfully.";
            } else if (exitCode == 3u) {
                snapshot_.state = State::Cancelled;
                snapshot_.message = "Render cancelled; partial WAV retained.";
            } else {
                snapshot_.state = State::Error;
                snapshot_.message = "Offline renderer exited unexpectedly.";
                snapshot_.detail = "Process exit code " +
                                   std::to_string(exitCode);
            }
        }
        if (process_) CloseHandle(process_);
        if (cancelEvent_) CloseHandle(cancelEvent_);
        process_ = nullptr;
        cancelEvent_ = nullptr;
    }
    workerRunning_.store(false, std::memory_order_release);
}

void OfflineRendererPage::HandleProtocolLine(const std::string& line) {
    const std::vector<std::string> fields = SplitTabs(line);
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (fields.size() < 2u || fields[0] != "SVMS3") {
        if (!line.empty()) snapshot_.detail = line;
        return;
    }

    if (fields[1] == "LOAD" && fields.size() >= 7u) {
        double progress = 0.0, elapsed = 0.0, eta = 0.0;
        if (!ParseDouble(fields[2], progress) ||
            !ParseDouble(fields[3], elapsed) ||
            !ParseDouble(fields[4], eta)) return;
        snapshot_.state = State::Loading;
        snapshot_.loadProgress = (std::max)(0.0, (std::min)(1.0, progress));
        snapshot_.loadElapsed = elapsed;
        snapshot_.loadEta = eta;
        snapshot_.message = "Scanning and validating the MIDI stream...";
        return;
    }

    if (fields[1] == "RENDER" && fields.size() >= 13u) {
        double progress = 0.0, elapsed = 0.0, rendered = 0.0;
        double total = 0.0, speed = 0.0, eta = 0.0;
        uint64_t active = 0, peak = 0, steals = 0, events = 0;
        if (!ParseDouble(fields[2], progress) ||
            !ParseDouble(fields[3], elapsed) ||
            !ParseDouble(fields[4], rendered) ||
            !ParseDouble(fields[5], total) ||
            !ParseDouble(fields[6], speed) ||
            !ParseDouble(fields[7], eta) ||
            !ParseU64(fields[8], active) || !ParseU64(fields[9], peak) ||
            !ParseU64(fields[10], steals) || !ParseU64(fields[11], events))
            return;
        if (snapshot_.state != State::Cancelling) {
            snapshot_.state = fields[12] == "tail"
                ? State::Tail : State::Rendering;
        }
        snapshot_.loadProgress = 1.0;
        snapshot_.renderProgress =
            (std::max)(0.0, (std::min)(1.0, progress));
        snapshot_.elapsed = elapsed;
        snapshot_.renderedSeconds = rendered;
        snapshot_.totalSeconds = total;
        snapshot_.speed = speed;
        snapshot_.eta = eta;
        snapshot_.activeVoices = static_cast<uint32_t>((std::min<uint64_t>)(
            active, UINT32_MAX));
        snapshot_.peakVoices = static_cast<uint32_t>((std::min<uint64_t>)(
            peak, UINT32_MAX));
        snapshot_.steals = steals;
        snapshot_.events = events;
        if (snapshot_.state != State::Cancelling) {
            snapshot_.message = snapshot_.state == State::Tail
                ? "Rendering natural SoundFont release tails..."
                : "Rendering audio...";
        }
        return;
    }

    if (fields[1] == "STATUS" && fields.size() >= 3u) {
        const std::string message = fields.size() >= 4u ? fields[3] : std::string{};
        const bool cancelling = snapshot_.state == State::Cancelling;
        if (fields[2] == "LOADING" && !cancelling) snapshot_.state = State::Loading;
        else if (fields[2] == "PREPARING" && !cancelling) snapshot_.state = State::Preparing;
        else if (fields[2] == "RENDERING" && !cancelling) snapshot_.state = State::Rendering;
        else if (fields[2] == "TAIL" && !cancelling) snapshot_.state = State::Tail;
        else if (fields[2] == "COMPLETE") snapshot_.state = State::Complete;
        else if (fields[2] == "CANCELLED") snapshot_.state = State::Cancelled;
        else if (fields[2] == "ERROR") snapshot_.state = State::Error;
        if (!message.empty() && (!cancelling || fields[2] == "COMPLETE" ||
                                 fields[2] == "CANCELLED" ||
                                 fields[2] == "ERROR"))
            snapshot_.message = message;
        if (snapshot_.state == State::Error) snapshot_.detail = message;
    }
}

void OfflineRendererPage::Draw(ConfigDocument& document, HWND owner) {
    EnsureInitialPaths(document);
    JoinFinishedWorker();

    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot = snapshot_;
    }
    const bool running = IsRunningState(snapshot.state);

    SectionHeader("OFFLINE RENDERER");
    ImGui::TextDisabled(
        "Runs svms_v3_render.exe separately using the current working engine settings. "
        "The same streaming MIDI parser and WAV/RF64 writer are used by the CLI.");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, GetPanelBg());
    ImGui::BeginChild("##offline_paths", ImVec2(0.0f, 168.0f),
                      ImGuiChildFlags_Borders |
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    if (ImGui::BeginTable("##offline_path_table", 3,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Browse", ImGuiTableColumnFlags_WidthFixed, 102.0f);
        ImGui::BeginDisabled(running);
        PathRow("MIDI file", midiPath_, "Browse...",
                [&] { BrowseMidi(owner); });
        PathRow("SoundFont", soundFontPath_, "Browse...",
                [&] { BrowseSoundFont(owner); });
        PathRow("Output WAV", outputPath_, "Browse...",
                [&] { BrowseOutput(owner); });
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    const bool pathsReady = !midiPath_.empty() && !soundFontPath_.empty() &&
                            !outputPath_.empty();
    ImGui::BeginDisabled(running || !pathsReady);
    ImVec4 startColor = GetAccent();
    startColor.w = 0.68f;
    ImGui::PushStyleColor(ImGuiCol_Button, startColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHover());
    if (ImGui::Button("Start Render", ImVec2(140.0f, 32.0f))) Start(document);
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running || snapshot.state == State::Cancelling);
    if (ImGui::Button("Cancel Render", ImVec2(140.0f, 32.0f))) Cancel();
    ImGui::EndDisabled();
    if (!pathsReady) {
        ImGui::SameLine();
        ImGui::TextDisabled("Select all three paths to begin.");
    }

    ImGui::Spacing();
    SectionHeader("PROGRESS");

    char loadOverlay[96]{};
    std::snprintf(loadOverlay, sizeof(loadOverlay), "MIDI loading  %.1f%%  |  ETA %s",
                  snapshot.loadProgress * 100.0,
                  FormatDuration(snapshot.loadEta).c_str());
    ImGui::ProgressBar(static_cast<float>(snapshot.loadProgress),
                       ImVec2(-1.0f, 24.0f), loadOverlay);

    char renderOverlay[128]{};
    std::snprintf(renderOverlay, sizeof(renderOverlay),
                  "Render  %.1f%%  |  %s / %s",
                  snapshot.renderProgress * 100.0,
                  FormatDuration(snapshot.renderedSeconds).c_str(),
                  FormatDuration(snapshot.totalSeconds).c_str());
    ImGui::ProgressBar(static_cast<float>(snapshot.renderProgress),
                       ImVec2(-1.0f, 24.0f), renderOverlay);

    ImGui::Spacing();
    if (ImGui::BeginTable("##offline_metrics", 4,
                          ImGuiTableFlags_SizingStretchSame |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_RowBg)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("ELAPSED");
        ImGui::TextUnformatted(FormatDuration(snapshot.elapsed).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("RENDER SPEED");
        if (snapshot.speed > 0.0) ImGui::Text("%.2fx realtime", snapshot.speed);
        else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("ETA");
        ImGui::TextUnformatted(FormatDuration(snapshot.eta).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("MIDI EVENTS");
        ImGui::Text("%llu", static_cast<unsigned long long>(snapshot.events));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("CURRENT VOICES");
        ImGui::Text("%u", snapshot.activeVoices);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("PEAK VOICES");
        ImGui::Text("%u", snapshot.peakVoices);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("VOICE STEALS");
        ImGui::Text("%llu", static_cast<unsigned long long>(snapshot.steals));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("LOADING ELAPSED");
        ImGui::TextUnformatted(FormatDuration(snapshot.loadElapsed).c_str());
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImVec4 stateColor = GetMutedText();
    if (snapshot.state == State::Complete) stateColor = GetSuccess();
    else if (snapshot.state == State::Error) stateColor = GetError();
    else if (snapshot.state == State::Cancelled ||
             snapshot.state == State::Cancelling) stateColor = GetWarning();
    else if (running) stateColor = GetAccent();
    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
    ImGui::Text("%s", StateLabel(snapshot.state));
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::TextWrapped("%s", snapshot.message.c_str());
    if (!snapshot.detail.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
        ImGui::TextWrapped("%s", snapshot.detail.c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace svms::cfg
