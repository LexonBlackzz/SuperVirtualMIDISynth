#ifdef _WIN32
#include "configurator/PageLiveRecording.h"
#include "configurator/PlatformUtil.h"

#include "configurator/Anim.h"
#include "configurator/ModernWidgets.h"
#include "BuildInfo.h"
#include "configurator/DriverLink.h"
#include "imgui.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <commdlg.h>
#include <shlobj.h>

namespace svms::cfg {
namespace {

std::string WideToUtf8(const std::wstring& value) {
    return svms::cfg::util::WideToUtf8(value);
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
    if (lastDirectory_.empty()) lastDirectory_ = directory;
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
    pathAutomatic_ = false;
    if (std::filesystem::path(outputPath_).extension().empty())
        outputPath_ += L".wav";
    lastDirectory_ = std::filesystem::path(outputPath_).parent_path().wstring();
}

void LiveRecordingPage::Start(svms::EngineLink& client) {
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

void LiveRecordingPage::Stop(svms::EngineLink& client) {
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    const svms::RLResult code = client.SendCommand(
        svms::RLCommandType::StopLiveRecording, 0u, 0u,
        svms::RuntimeLiveStateV2{}, 3000u, result);
    status_ = result[0] ? result : svms::RLV2_ResultToString(code);
    statusError_ = code != svms::RLResult::Ok;
    Query(client, true);
}

void LiveRecordingPage::Query(svms::EngineLink& client, bool force) {
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

void LiveRecordingPage::Draw(svms::EngineLink* client,
                             bool connected, HWND owner) {
    EnsureDefaultPath();
    const bool supported = connected && client && client->HasCapability(
        svms::build::CapabilityLiveRecording);
    if (supported) Query(*client, false);
    const bool recording = state_ == 1u || state_ == 2u || state_ == 4u;
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();

    ui::BeginCard("##record", nullptr, nullptr, 0.0f, "record");
    {
        // The big morphing button: a ring around a circle that becomes a square.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float d = 88.0f * s;
        ImGui::PushID("##recbtn");
        ImGui::InvisibleButton("##rec", ImVec2(d, d));
        const bool hov = ImGui::IsItemHovered();
        const bool down = ImGui::IsItemActive();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool enabled = supported && (recording || !outputPath_.empty());
        if (ImGui::IsItemClicked() && enabled && client) {
            if (recording) {
                Stop(*client);
            } else {
                if (pathAutomatic_ && framesWritten_ > 0u) {
                    pathInitialized_ = false;
                    EnsureDefaultPath();
                }
                Start(*client);
            }
        }
        const ImGuiID id = ImGui::GetID("##rec");
        const float press = AnimSpring(id, down ? 0.92f : (hov ? 1.05f : 1.0f));
        const float on = AnimSpring(ImGui::GetID("##recon"), recording ? 1.0f : 0.0f);
        const ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
        const float rr = d * 0.5f * press;
        const float onE = (std::max)(0.0f, (std::min)(1.0f, on));
        dl->AddCircle(c, rr - 1.5f * s * press, ui::MixU32(k.line2, k.bad, onE), 48, 3.0f * s * press);
        const float half = (18.0f - 4.0f * on) * s * press;
        const float round = (18.0f - 12.0f * on) * s * press;
        dl->AddRectFilled(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half),
                          enabled ? k.bad : ui::WithAlpha(k.bad, 0.4f), round);
        if (recording && GetMotion() != Motion::Off) {
            const float t = std::fmod(static_cast<float>(ImGui::GetTime()) / 1.6f, 1.0f);
            const float e = EaseOut(t);
            dl->AddCircle(c, rr + 8.0f * s + e * 14.0f * s, ui::WithAlpha(k.bad, 0.8f * (1.0f - e)), 48, 2.0f * s);
        }
        ImGui::PopID();

        // Timer and status to the right of the button.
        const float tx = p.x + d + 26.0f * s;
        const uint64_t secs = sampleRate_ == 0u ? 0u : framesWritten_ / sampleRate_;
        char timer[24];
        if (secs >= 3600u)
            std::snprintf(timer, sizeof(timer), "%02llu:%02llu:%02llu", static_cast<unsigned long long>(secs / 3600u),
                          static_cast<unsigned long long>((secs / 60u) % 60u), static_cast<unsigned long long>(secs % 60u));
        else
            std::snprintf(timer, sizeof(timer), "%02llu:%02llu", static_cast<unsigned long long>(secs / 60u),
                          static_cast<unsigned long long>(secs % 60u));
        ui::PushNum(44.0f);
        const float th = ImGui::GetTextLineHeight();
        ui::PopFont();
        ui::PushText(0, 13.0f);
        const float sh = ImGui::GetTextLineHeight();
        ui::PopFont();
        const float y0 = p.y + (d - th - sh - 4.0f * s) * 0.5f;
        ui::PushNum(44.0f);
        dl->AddText(ImVec2(tx, y0), k.text, timer);
        ui::PopFont();
        const char* status;
        ImU32 statusColor = k.muted;
        if (!connected) status = "Start playing something in a MIDI app first.";
        else if (!supported) status = "The connected driver does not support live recording.";
        else if (statusError_) { status = status_.c_str(); statusColor = k.bad; }
        else if (recording) { status = "Recording\xE2\x80\xA6"; statusColor = k.bad; }
        else if (framesWritten_ > 0u) status = "Saved. Press again for a new file.";
        else status = "Ready.";
        ui::PushText(0, 13.0f);
        dl->AddText(ImVec2(tx, y0 + th + 4.0f * s), statusColor, status);
        ui::PopFont();
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + d));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
    }

    ImGui::Dummy(ImVec2(0.0f, 14.0f * s));
    const std::string path = WideToUtf8(outputPath_);
    ui::BeginRow("recordPath", "Save to", path.empty() ? "No output selected" : path.c_str(),
                 ui::ButtonSize("Change\xE2\x80\xA6"));
    if (ui::Button("Change\xE2\x80\xA6", ui::ButtonKind::Normal, ImVec2(0, 0), !recording)) Browse(owner);
    ui::EndRow();

    char size[32], dropped[24], rate[24];
    std::snprintf(size, sizeof(size), "%s", FormatBytes(framesWritten_ * 8u).c_str());
    std::snprintf(dropped, sizeof(dropped), "%llu", static_cast<unsigned long long>(droppedFrames_));
    if (sampleRate_) std::snprintf(rate, sizeof(rate), "%g kHz", sampleRate_ / 1000.0);
    else std::snprintf(rate, sizeof(rate), "\xE2\x80\x94");
    const char* labels[4] = {"File size", "Dropped frames", "Format", "Rate"};
    const char* values[4] = {size, dropped, "32-bit float", rate};
    ui::StatGrid(labels, values, 4);

    if (droppedFrames_ != 0u) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.exp);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("The disk writer fell behind. Playback stayed real-time, but the WAV has missing frames.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    if (errorCode_ != 0u) {
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::Text("Recorder error code: %u", errorCode_);
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    ui::EndCard();
}

} // namespace svms::cfg

#else  // Linux
#include "configurator/PageLiveRecording.h"
#include "imgui.h"

namespace svms::cfg {

void LiveRecordingPage::Draw(svms::EngineLink*, bool, HWND) {
    ImGui::TextWrapped("Live recording is not available for the Linux driver yet.");
}

} // namespace svms::cfg

#endif  // _WIN32
