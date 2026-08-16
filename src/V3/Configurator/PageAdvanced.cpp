#include "PageAdvanced.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace svms::cfg {

static std::string WideToUtf8Adv(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
                                  static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        s.data(), len, nullptr, nullptr);
    return s;
}

static bool confirmReset = false;

void DrawAdvancedPage(ConfigDocument& doc) {
    SectionHeader("CONFIGURATION");

    auto path = doc.GetActivePath();
    std::string pathStr = WideToUtf8Adv(path);
    ImGui::Text("Config path:");
    ImGui::TextDisabled("%s", pathStr.c_str());

    ImGui::Spacing();

    if (ImGui::Button("Open Config Folder")) {
        std::wstring folder = path;
        auto pos = folder.find_last_of(L"\\/");
        if (pos != std::wstring::npos) folder.resize(pos);
        ShellExecuteW(nullptr, L"open", folder.c_str(),
                      nullptr, nullptr, SW_SHOWDEFAULT);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open config.json")) {
        ShellExecuteW(nullptr, L"open", path.c_str(),
                      nullptr, nullptr, SW_SHOWDEFAULT);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    SectionHeader("RESET");

    if (!confirmReset) {
        if (ImGui::Button("Reset All To Defaults")) {
            confirmReset = true;
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.20f, 1.0f));
        ImGui::Text("Reset ALL settings to defaults?");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Yes, reset")) {
            doc.LoadDefaults();
            doc.MarkDirty();
            confirmReset = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            confirmReset = false;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    SectionHeader("PARSE STATUS");

    if (doc.HasParseError()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.30f, 0.30f, 1.0f));
        ImGui::TextWrapped("Parse error: %s", doc.ParseError().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.75f, 0.40f, 1.0f));
        ImGui::Text("No parse errors");
        ImGui::PopStyleColor();
    }

    if (!doc.ConfigWarning().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.20f, 1.0f));
        ImGui::TextWrapped("Warning: %s", doc.ConfigWarning().c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace svms::cfg
