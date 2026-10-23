#ifdef _WIN32
#include "configurator/PageOffline.h"

#include "configurator/ConfigDocument.h"
#include "configurator/ConfiguratorApp.h"
#include "configurator/DriverLink.h"
#include "configurator/ModernWidgets.h"
#include "configurator/Widgets.h"
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

// Cuts the front of a long path so it fits `maxWidth` (mono 12 px).
std::string Elide(const std::string& text, float maxWidth) {
    if (text.empty() || ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
    size_t cut = 1;
    while (cut < text.size() &&
           ImGui::CalcTextSize(("\xE2\x80\xA6" + text.substr(cut)).c_str()).x > maxWidth) ++cut;
    return "\xE2\x80\xA6" + text.substr(cut);
}

// One numbered step of the render card. Returns true when its button is clicked.
bool Step(int number, bool done, const char* title, const std::string& sub,
          const char* button, bool enabled) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(number);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ui::PushText(1, 14.0f);
    const float titleH = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushNum(12.0f);
    const float subH = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float h = (std::max)(28.0f * s, titleH + subH) + 28.0f * s;
    const float r = k.rack ? 3.0f * s : 12.0f * s;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), k.card2, r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), k.line2, r);
    const ImVec2 c(p.x + 18.0f * s + 14.0f * s, p.y + h * 0.5f);
    dl->AddCircleFilled(c, 14.0f * s, done ? k.acc : k.card3);
    char num[8];
    std::snprintf(num, sizeof(num), "%d", number);
    ui::PushNum(13.0f);
    const ImVec2 ns = ImGui::CalcTextSize(num);
    dl->AddText(ImVec2(c.x - ns.x * 0.5f, c.y - ns.y * 0.5f), done ? k.onAcc : k.text, num);
    ui::PopFont();
    const float textX = p.x + 18.0f * s + 28.0f * s + 14.0f * s;
    const float bw = ui::ButtonSize(button).x;
    const float textY = p.y + (h - titleH - subH) * 0.5f;
    ui::PushText(1, 14.0f);
    dl->AddText(ImVec2(textX, textY), k.text, title);
    ui::PopFont();
    ui::PushNum(12.0f);
    const std::string shown = Elide(sub, p.x + w - 18.0f * s - bw - 14.0f * s - textX);
    dl->AddText(ImVec2(textX, textY + titleH), k.muted, shown.c_str());
    ui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - 18.0f * s - bw, p.y + (h - 38.0f * s) * 0.5f));
    const bool clicked = ui::Button(button, ui::ButtonKind::Normal, ImVec2(0, 0), enabled);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(w, 10.0f * s - ImGui::GetStyle().ItemSpacing.y));
    ImGui::PopID();
    return clicked;
}

std::string FileName(const std::wstring& path) {
    if (path.empty()) return {};
    return WideToUtf8(std::filesystem::path(path).filename().wstring());
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

void OfflineRendererPage::BrowseOutput(HWND owner) {
    std::wstring selected = outputPath_;
    if (!BrowseSaveFile(owner, lastOutputDirectory_, selected)) return;
    outputPath_ = selected;
    lastOutputDirectory_ = ParentDirectory(selected);
    outputPathAutomatic_ = false;
}

void OfflineRendererPage::JoinFinishedWorker() {
    if (worker_.joinable() && !workerRunning_.load(std::memory_order_acquire)) {
        worker_.join();
        if (!configTemp_.empty()) {
            DeleteFileW(configTemp_.c_str());
            configTemp_.clear();
        }
    }
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
    if (stemMode_ > 0) {
        static const wchar_t* const kModes[] = {L"", L"separate", L"merge", L"both"};
        append(L"--stems"); append(kModes[stemMode_]);
    }
    if (fullVelocity_) append(L"--full-velocity");
    if (renderMode_ == 1) append(L"--bassmidi-mode");
    if (shards_ > 1) { append(L"--shards"); append(std::to_wstring(shards_)); }
    if (stemTracks_[0]) { append(L"--tracks"); append(std::wstring(stemTracks_, stemTracks_ + std::strlen(stemTracks_))); }
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
    // Hand over the *working* settings (saved or not) so every engine option
    // - decimation, reverb, velocity, collapse... - matches the live synth.
    {
        wchar_t tempDirectory[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, tempDirectory);
        if (length > 0 && length < MAX_PATH) {
            wchar_t name[96]{};
            swprintf_s(name, L"svms_offline_%lu_%u.json",
                       GetCurrentProcessId(), serial.load());
            const std::wstring path = std::wstring(tempDirectory) + name;
            std::string exportError;
            if (document.ExportProfile(path, &exportError)) {
                configTemp_ = path;
                append(L"--config"); append(path);
            }
        }
    }
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
    if (!configTemp_.empty()) {
        DeleteFileW(configTemp_.c_str());
        configTemp_.clear();
    }
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

    if (fields[1] == "PHASE" && fields.size() >= 5u) {
        uint64_t step = 0, steps = 0;
        if (ParseU64(fields[2], step) && ParseU64(fields[3], steps)) {
            snapshot_.phaseStep = static_cast<uint32_t>(step);
            snapshot_.phaseSteps = static_cast<uint32_t>(steps);
            snapshot_.phaseName = fields[4];
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
    const ConfigValues& v = document.Working();
    const float s = ui::Scale();
    const ui::Tokens& k = ui::Tok();

    // Step 2 is the Sound page's first SoundFont.
    if (!running && !v.soundFontPath.empty()) {
        std::filesystem::path path(v.soundFontPath);
        if (path.is_relative()) path = std::filesystem::path(ExecutableDirectory()) / path;
        soundFontPath_ = path.lexically_normal().wstring();
    }

    ui::BeginCard("##render", nullptr, nullptr, 0.0f, "render");
    if (Step(1, !midiPath_.empty(), "MIDI file", midiPath_.empty() ? "Not chosen yet" : FileName(midiPath_),
             "Choose\xE2\x80\xA6", !running))
        BrowseMidi(owner);
    if (Step(2, !soundFontPath_.empty(), "SoundFont",
             soundFontPath_.empty() ? "None. Pick one on the Sound page." : FileName(soundFontPath_) + " (same as Sound page)",
             "Change", !running)) {
        if (ConfiguratorApp* app = GetLiveLinkContext().app) app->Navigate(NavPage::Sound);
    }
    if (Step(3, !outputPath_.empty(), "Save as", outputPath_.empty() ? "Not chosen yet" : WideToUtf8(outputPath_),
             "Choose\xE2\x80\xA6", !running))
        BrowseOutput(owner);

    const bool pathsReady = !midiPath_.empty() && !soundFontPath_.empty() && !outputPath_.empty();
    ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(220.0f * s);
    ImGui::Combo("Per-track render##stems", &stemMode_,
                 "Off\0Separate WAV per track\0Merge into one WAV\0Both\0");
    ImGui::SetNextItemWidth(220.0f * s);
    ImGui::InputTextWithHint("Only tracks##stem", "all (or e.g. 1,3,5)", stemTracks_,
                             sizeof(stemTracks_), ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SetNextItemWidth(220.0f * s);
    ImGui::Combo("Sound model##rmode", &renderMode_, "Exact engine\0BASSMIDI-style (KMC)\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Exact engine: every note as the synth plays it.\n"
                          "BASSMIDI-style: imitates KMC/BASSMIDI on huge Black MIDI. Notes are held at least 4 ms, a full pool drops notes that are not louder than its quietest voice, DC offset is kept, and a program change to a preset the SoundFont lacks stays silent.\n"
                          "Uses the voice limit from the Performance page. Renders the whole file plus a 3 s tail; a MIDI with too many notes for memory stops with a message.");
    ImGui::Checkbox("Full velocity##fullvel", &fullVelocity_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every note plays at velocity 127.");
    ImGui::SetNextItemWidth(220.0f * s);
    ImGui::SliderInt("Parallel shards##shards", &shards_, 1, 32);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Splits notes by key across this many synth cores that plan in parallel.\nFor huge Black MIDI. The voice limit is divided between them; each core loads its own SoundFont copy.");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Per-track render runs one synth per track, as many at once as your render threads.\n"
                          "Only tracks: limit to these 0-based MIDI tracks (one combined render if Per-track is Off).\n"
                          "Same length as the full render, so stems line up in your DAW.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
    const bool cancelling = snapshot.state == State::Cancelling;
    if (ui::Button(running ? "Cancel render" : "Start render", ui::ButtonKind::Primary, ImVec2(0, 0),
                   running ? !cancelling : pathsReady)) {
        if (running) Cancel();
        else Start(document);
    }
    ImGui::SameLine(0.0f, 10.0f * s);
    {
        const char* hint = snapshot.state == State::Complete ? "Done. The WAV is ready."
            : running ? StateLabel(snapshot.state)
            : pathsReady ? "Ready."
            : "Pick the MIDI file and where to save.";
        ui::PushText(0, 13.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (38.0f * s - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    {
        // What the renderer cannot apply yet (everything else rides --config).
        std::string unapplied;
        auto add = [&](bool on, const char* name) {
            if (!on) return;
            if (!unapplied.empty()) unapplied += ", ";
            unapplied += name;
        };
        add(v.channelLimiterEnabled, "channel limiter");
        add(v.ccCollapse, "skipping overwritten controller changes");
        add(v.bendCollapse, "thinning pitch bends");
        add(v.minNoteMs > 0u && renderMode_ == 0, "minimum note length");
        if (!unapplied.empty()) {
            const std::string note = "Uses your current settings, even unsaved ones. Live-only for now, so not in the file: " + unapplied + ".";
            ImGui::Dummy(ImVec2(0.0f, 8.0f * s - ImGui::GetStyle().ItemSpacing.y));
            ui::PushText(0, 13.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(note.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ui::PopFont();
        }
    }
    ui::EndCard();

    const bool loadingPhase = snapshot.state == State::Starting || snapshot.state == State::Loading ||
                              snapshot.state == State::Preparing;
    const double progress = loadingPhase ? snapshot.loadProgress : snapshot.renderProgress;
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%.0f%%", progress * 100.0);
    ui::PushNum(13.0f);
    const float pctW = ImGui::CalcTextSize(pct).x;
    ui::PopFont();
    ui::BeginCard("##renderprogress", "Progress", nullptr, pctW);
    ui::PushNum(13.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (38.0f * s - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextUnformatted(pct);
    ui::PopFont();
    ui::CardHeaderEnd();
    ui::StripedProgress(running || snapshot.state == State::Complete ? static_cast<float>(progress) : -1.0f);
    char speed[24], eta[24], voices[24], peak[24], elapsed[24], done[48];
    if (snapshot.speed > 0.0) std::snprintf(speed, sizeof(speed), "%.1fx realtime", snapshot.speed);
    else std::snprintf(speed, sizeof(speed), "\xE2\x80\x94");
    const double etaShown = loadingPhase && snapshot.loadEta > 0.0 ? snapshot.loadEta : snapshot.eta;
    std::snprintf(eta, sizeof(eta), "%s", running && etaShown > 0.0 ? FormatDuration(etaShown).c_str() : "\xE2\x80\x94");
    std::snprintf(elapsed, sizeof(elapsed), "%s", running || snapshot.state == State::Complete ? FormatDuration(snapshot.elapsed).c_str() : "\xE2\x80\x94");
    if (snapshot.totalSeconds > 0.0)
        std::snprintf(done, sizeof(done), "%s / %s", FormatDuration(snapshot.renderedSeconds).c_str(), FormatDuration(snapshot.totalSeconds).c_str());
    else std::snprintf(done, sizeof(done), "\xE2\x80\x94");
    std::snprintf(voices, sizeof(voices), "%u", snapshot.activeVoices);
    std::snprintf(peak, sizeof(peak), "%u", snapshot.peakVoices);
    if (renderMode_ == 1) {
        const char* labels[4] = {"Speed", "Time left", "Elapsed", "Audio"};
        const char* values[4] = {speed, eta, elapsed, done};
        ui::StatGrid(labels, values, 4);
    } else {
        const char* labels[6] = {"Speed", "Time left", "Elapsed", "Audio", "Voices now", "Peak voices"};
        const char* values[6] = {speed, eta, elapsed, done, voices, peak};
        ui::StatGrid(labels, values, 6);
    }

    ImGui::Dummy(ImVec2(0.0f, 14.0f * s - ImGui::GetStyle().ItemSpacing.y));
    ImU32 stateColor = k.muted;
    if (snapshot.state == State::Complete) stateColor = k.ok;
    else if (snapshot.state == State::Error) stateColor = k.bad;
    else if (snapshot.state == State::Cancelled || snapshot.state == State::Cancelling) stateColor = k.exp;
    else if (running) stateColor = k.acc;
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
    ImGui::PushTextWrapPos(0.0f);
    if (running && snapshot.phaseSteps > 0u)
        ImGui::Text("%s  Step %u of %u: %s", StateLabel(snapshot.state), snapshot.phaseStep, snapshot.phaseSteps, snapshot.phaseName.c_str());
    else
        ImGui::Text("%s  %s", StateLabel(snapshot.state), snapshot.message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (!snapshot.detail.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(snapshot.detail.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ui::PopFont();
    ui::EndCard();
}

} // namespace svms::cfg

#else  // Linux
#include "configurator/PageOffline.h"
#include "configurator/ConfigDocument.h"
#include "imgui.h"

namespace svms::cfg {

OfflineRendererPage::~OfflineRendererPage() = default;

void OfflineRendererPage::Shutdown() {}

void OfflineRendererPage::Draw(ConfigDocument&, HWND) {
    ImGui::TextWrapped(
        "Offline rendering on Linux uses the svms_v3_render command line tool "
        "shipped beside this program:");
    ImGui::Spacing();
    ImGui::TextDisabled("svms_v3_render song.mid --soundfont gm.sf2 --output song.wav");
}

} // namespace svms::cfg

#endif  // _WIN32
