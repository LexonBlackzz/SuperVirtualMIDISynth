#include "PageOverview.h"
#include "ConfigDocument.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace svms::cfg {
namespace {

std::string WideToUtf8Overview(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
                                        static_cast<int>(ws.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        result.data(), len, nullptr, nullptr);
    return result;
}

float LinearToDb(float linear) {
    if (linear <= 0.000001f) return -60.0f;
    return (std::max)(-60.0f, (std::min)(0.0f, 20.0f * std::log10(linear)));
}

void DrawOverviewOutputMeter(const svms::RuntimeLinkTelemetryV2* telemetry,
                             bool connected,
                             float height) {
    constexpr float kMeterWidth = 26.0f;
    constexpr float kFloorDb = -60.0f;

    static float visualDb = kFloorDb;
    float targetDb = kFloorDb;
    if (connected && telemetry) {
        const float linear = (std::max)(telemetry->limiterOutputPeakL,
                                        telemetry->limiterOutputPeakR);
        targetDb = LinearToDb(linear);
    }

    const float dt = (std::max)(0.0f, ImGui::GetIO().DeltaTime);
    const float speed = targetDb > visualDb ? 22.0f : 9.0f;
    const float alpha = 1.0f - std::exp(-speed * dt);
    visualDb += (targetDb - visualDb) * alpha;

    height = (std::max)(80.0f, height);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(kMeterWidth, height);
    ImGui::InvisibleButton("##overview_limiter_output", size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 maxP(p.x + size.x, p.y + size.y);
    const float rounding = (std::min)(GetThemeSettings().cornerRadius, 5.0f);
    dl->AddRectFilled(p, maxP, ImGui::GetColorU32(GetPanelBg()), rounding);
    dl->AddRect(p, maxP, ImGui::GetColorU32(GetInputBorder()), rounding);

    const float t = (visualDb - kFloorDb) / -kFloorDb;
    const float clamped = (std::max)(0.0f, (std::min)(1.0f, t));
    const float inset = 3.0f;
    const float innerTop = p.y + inset;
    const float innerBottom = maxP.y - inset;
    const float fillTop = innerBottom - (innerBottom - innerTop) * clamped;

    ImVec4 fill = GetAccent();
    if (visualDb > -3.0f) fill = GetError();
    else if (visualDb > -9.0f) fill = GetWarning();

    if (clamped > 0.0f) {
        dl->AddRectFilled(ImVec2(p.x + inset, fillTop),
                          ImVec2(maxP.x - inset, innerBottom),
                          ImGui::GetColorU32(fill),
                          (std::max)(0.0f, rounding - 2.0f));
    }

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (connected && telemetry)
            ImGui::Text("Limiter output: %.1f dBFS", targetDb);
        else
            ImGui::TextUnformatted("Limiter output: RuntimeLink offline");
        ImGui::EndTooltip();
    }
}

} // namespace

void DrawOverviewPage(ConfigDocument& doc) {
    auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();

    if (!ImGui::BeginTable("##overview_layout", 2,
                           ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
        return;
    }
    ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthFixed, 26.0f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    SectionHeader("SOUNDFONT");

    if (w.soundFontPath.empty()) {
        ImGui::TextUnformatted("Automatic / local fallback");
        ImGui::TextDisabled("Choose a SoundFont to make it the configured source.");
    } else {
        const std::filesystem::path sfPath(w.soundFontPath);
        const std::string filename = WideToUtf8Overview(sfPath.filename().wstring());
        const std::string fullPath = WideToUtf8Overview(sfPath.wstring());
        ImGui::Text("%s", filename.c_str());
        ImGui::TextDisabled("%s", fullPath.c_str());
    }

    ImGui::Spacing();

    static std::wstring lastSoundFontDir;
    if (lastSoundFontDir.empty() && !w.soundFontPath.empty()) {
        lastSoundFontDir = std::filesystem::path(w.soundFontPath).parent_path().wstring();
    }

    if (ImGui::Button("Choose SoundFont...", ImVec2(150.0f, 0.0f))) {
        wchar_t fileBuf[1024] = {};
        const std::wstring initialDir = lastSoundFontDir;
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter =
            L"SoundFont files (*.sf2;*.dls)\0*.sf2;*.dls\0"
            L"All files (*.*)\0*.*\0";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = static_cast<DWORD>(std::size(fileBuf));
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.lpstrTitle = L"Select SoundFont";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            w.soundFontPath = fileBuf;
            lastSoundFontDir = std::filesystem::path(fileBuf).parent_path().wstring();
            doc.MarkDirty();
        }
    }
    if (!w.soundFontPath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(64.0f, 0.0f))) {
            w.soundFontPath.clear();
            doc.MarkDirty();
        }
    }
    ImGui::SameLine();
    RestartRequiredBadge();

    ImGui::Spacing();
    ImGui::TextDisabled(
        "SoundFont changes are saved to the synth configuration and currently take effect after restart.");

    ImGui::TableNextColumn();
    const float meterHeight = ImGui::GetContentRegionAvail().y;
    DrawOverviewOutputMeter(lc.telemetry, lc.connected, meterHeight);

    ImGui::EndTable();
}

} // namespace svms::cfg
