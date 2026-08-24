#include "PageLiveRecording.h"

#include "Theme.h"
#include "Widgets.h"
#include "SVMSBuildInfo.h"
#include "../SVMSRuntimeLink.h"
#include "imgui.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <commdlg.h>
#include <shlobj.h>

namespace svms::cfg {
namespace {

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            count, nullptr, nullptr) != count) return {};
    return result;
}

bool ParseU64(const char* begin, char** end, uint64_t& value) {
    errno = 0;
    const unsigned long long parsed = std::strtoull(begin, end, 10);
    if (errno != 0 || *end == begin) return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

std::string FormatDuration(uint64_t frames, uint32_t sampleRate) {
    const uint64_t seconds = sampleRate == 0u ? 0u : frames / sampleRate;
    char text[32]{};
    std::snprintf(text, sizeof(text), "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(seconds / 3600u),
                  static_cast<unsigned long long>((seconds / 60u) % 60u),
                  static_cast<unsigned long long>(seconds % 60u));
    return text;
}

std::string FormatBytes(uint64_t bytes) {
    char text[48]{};
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(text, sizeof(text), "%.2f GiB",
                      static_cast<double>(bytes) /
                          (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(text, sizeof(text), "%.1f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(text, sizeof(text), "%.1f KiB",
                      static_cast<double>(bytes) / 1024.0);
    }
    return text;
}

const char* StateLabel(uint32_t state) {
    switch (state) {
    case 1u: return "RECORDING";
    case 2u: return "STOPPING";
    case 3u: return "ERROR";
    case 4u: return "STARTING";
    default: return "STOPPED";
    }
}

} // namespace

void LiveRecordingPage::EnsureDefaultPath() {
    if (pathInitialized_) return;
    pathInitialized_ = true;
    wchar_t directory[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr,
                                SHGFP_TYPE_CURRENT, directory))) {
        GetCurrentDirectoryW(MAX_PATH, directory);
    }
    lastDirectory_ = directory;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[96]{};
    swprintf_s(name, L"SVMS Recording %04u-%02u-%02u %02u-%02u-%02u.wav",
               time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond);
    outputPath_ = (std::filesystem::path(lastDirectory_) / name).wstring();
}

void LiveRecordingPage::Browse(HWND owner) {
    std::vector<wchar_t> buffer(32768u, L'\0');
    if (!outputPath_.empty())
        wcsncpy_s(buffer.data(), buffer.size(), outputPath_.c_str(), _TRUNCATE);
    static const wchar_t filter[] =
        L"Wave audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = L"Select live recording WAV";
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrInitialDir = lastDirectory_.empty()
        ? nullptr : lastDirectory_.c_str();
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    outputPath_ = buffer.data();
    if (std::filesystem::path(outputPath_).extension().empty())
        outputPath_ += L".wav";
    lastDirectory_ = std::filesystem::path(outputPath_).parent_path().wstring();
}

void LiveRecordingPage::Start(svms::RuntimeLinkClient& client) {
    const std::string utf8 = WideToUtf8(outputPath_);
    if (utf8.empty()) {
        status_ = "The output path could not be encoded as UTF-8.";
        statusError_ = true;
        return;
    }
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    const svms::RLResult code = client.SendCommand(
        svms::RLCommandType::StartLiveRecording, 0u, 0u,
        svms::RuntimeLiveStateV2{}, 1000u, result, utf8.c_str());
    status_ = result[0] ? result : svms::RLV2_ResultToString(code);
    statusError_ = code != svms::RLResult::Ok;
    if (code == svms::RLResult::Ok) {
        state_ = 1u;
        framesWritten_ = 0u;
        droppedFrames_ = 0u;
        errorCode_ = 0u;
        Query(client, true);
    }
}

void LiveRecordingPage::Stop(svms::RuntimeLinkClient& client) {
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    const svms::RLResult code = client.SendCommand(
        svms::RLCommandType::StopLiveRecording, 0u, 0u,
        svms::RuntimeLiveStateV2{}, 3000u, result);
    status_ = result[0] ? result : svms::RLV2_ResultToString(code);
    statusError_ = code != svms::RLResult::Ok;
    Query(client, true);
}

void LiveRecordingPage::Query(svms::RuntimeLinkClient& client, bool force) {
    if (!force) {
        queryTimer_ += ImGui::GetIO().DeltaTime;
        if (queryTimer_ < 0.25f) return;
    }
    queryTimer_ = 0.0f;
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    if (client.SendCommand(svms::RLCommandType::QueryLiveRecording,
                           0u, 0u, svms::RuntimeLiveStateV2{}, 100u,
                           result) != svms::RLResult::Ok) return;

    char* cursor = result;
    char* end = nullptr;
    uint64_t values[5]{};
    for (uint32_t i = 0u; i < 5u; ++i) {
        if (!ParseU64(cursor, &end, values[i])) return;
        if (i != 4u) {
            if (*end != '\t') return;
            cursor = end + 1;
        } else if (*end != '\0') {
            return;
        }
    }
    state_ = static_cast<uint32_t>(values[0]);
    sampleRate_ = static_cast<uint32_t>(values[1]);
    framesWritten_ = values[2];
    droppedFrames_ = values[3];
    errorCode_ = static_cast<uint32_t>(values[4]);
    if (state_ == 3u) {
        status_ = "The recording writer stopped because of a file error.";
        statusError_ = true;
    } else if (state_ == 0u &&
               status_ == "Connect to a running V3 driver to record.") {
        status_ = "Ready to record the running driver's final output.";
        statusError_ = false;
    }
}

void LiveRecordingPage::Draw(svms::RuntimeLinkClient* client,
                             bool connected, HWND owner) {
    EnsureDefaultPath();
    const bool supported = connected && client && client->HasCapability(
        svms::build::CapabilityLiveRecording);
    if (supported) Query(*client, false);
    const bool recording = state_ == 1u || state_ == 2u || state_ == 4u;

    SectionHeader("LIVE WAV RECORDING");
    ImGui::TextDisabled(
        "Records the final post-reverb, post-limiter stereo output from the running driver. "
        "The audio callback copies into an eight-second lock-free buffer; disk writes happen on a worker thread.");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, GetPanelBg());
    ImGui::BeginChild("##live_record_path", ImVec2(0.0f, 92.0f),
                      ImGuiChildFlags_Borders |
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextDisabled("OUTPUT WAV");
    const std::string path = WideToUtf8(outputPath_);
    ImGui::TextWrapped("%s", path.empty() ? "No output selected" : path.c_str());
    ImGui::BeginDisabled(recording);
    if (ImGui::Button("Browse...", ImVec2(110.0f, 28.0f))) Browse(owner);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::BeginDisabled(!supported || recording || outputPath_.empty());
    ImGui::PushStyleColor(ImGuiCol_Button, GetAccentDim());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHover());
    if (ImGui::Button("Start Recording", ImVec2(150.0f, 32.0f)) && client)
        Start(*client);
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!supported || !recording);
    if (ImGui::Button("Stop && Finalize", ImVec2(150.0f, 32.0f)) && client)
        Stop(*client);
    ImGui::EndDisabled();

    if (!connected) {
        ImGui::SameLine();
        ImGui::TextDisabled("No running V3 driver is connected.");
    } else if (!supported) {
        ImGui::SameLine();
        ImGui::TextDisabled("The connected driver does not support live recording.");
    }

    ImGui::Spacing();
    SectionHeader("RECORDING STATUS");
    if (ImGui::BeginTable("##live_record_metrics", 4,
                          ImGuiTableFlags_SizingStretchSame |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_RowBg)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("STATE");
        ImGui::TextUnformatted(StateLabel(state_));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("DURATION");
        ImGui::TextUnformatted(FormatDuration(framesWritten_, sampleRate_).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("FILE AUDIO");
        ImGui::TextUnformatted(FormatBytes(framesWritten_ * 8u).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("DROPPED FRAMES");
        ImGui::Text("%llu", static_cast<unsigned long long>(droppedFrames_));
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImVec4 color = statusError_ ? GetError() :
        (recording ? GetAccent() : GetMutedText());
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", status_.c_str());
    ImGui::PopStyleColor();
    if (droppedFrames_ != 0u) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::TextWrapped(
            "The disk writer fell behind. Playback stayed real-time, but the WAV has missing frames.");
        ImGui::PopStyleColor();
    }
    if (errorCode_ != 0u) {
        ImGui::TextDisabled("Recorder error code: %u", errorCode_);
    }
}

} // namespace svms::cfg
