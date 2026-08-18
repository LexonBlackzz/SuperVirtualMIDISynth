#include "PageAdvanced.h"
#include "ConfigDocument.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <string>

namespace svms::cfg {
namespace {

std::string WideToUtf8Adv(const std::wstring& ws) {
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

bool ThemeColorRow(const char* label, const char* id, ImVec4& color) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth((std::min)(260.0f, ImGui::GetContentRegionAvail().x));
    return ImGui::ColorEdit4(id, &color.x,
                             ImGuiColorEditFlags_AlphaBar |
                             ImGuiColorEditFlags_DisplayHex);
}

bool ThemeFloatRow(const char* label, const char* id, float& value,
                   float minValue, float maxValue, const char* format) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth((std::min)(260.0f, ImGui::GetContentRegionAvail().x));
    return ImGui::SliderFloat(id, &value, minValue, maxValue, format);
}

bool confirmReset = false;
std::string themeStatus;
bool themeStatusError = false;

} // namespace

void DrawAdvancedPage(ConfigDocument& doc) {
    auto& w = doc.Working();

    SectionHeader("APPEARANCE");
    ImGui::TextDisabled(
        "Theme changes are previewed immediately. No theme JSON is created until you save one.");

    ThemeSettings& theme = EditThemeSettings();
    bool themeChanged = false;

    if (ImGui::BeginTable("##theme_settings", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_RowBg,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);

        themeChanged |= ThemeColorRow("Accent", "##theme_accent", theme.accent);
        themeChanged |= ThemeColorRow("Background", "##theme_background", theme.background);
        themeChanged |= ThemeColorRow("Sidebar", "##theme_sidebar", theme.sidebar);
        themeChanged |= ThemeColorRow("Panel", "##theme_panel", theme.panel);
        themeChanged |= ThemeColorRow("Controls", "##theme_control", theme.control);
        themeChanged |= ThemeColorRow("Primary text", "##theme_text", theme.text);
        themeChanged |= ThemeColorRow("Muted text", "##theme_muted", theme.mutedText);
        themeChanged |= ThemeColorRow("Warning", "##theme_warning", theme.warning);
        themeChanged |= ThemeColorRow("Error", "##theme_error", theme.error);
        themeChanged |= ThemeColorRow("Success", "##theme_success", theme.success);
        themeChanged |= ThemeFloatRow("Corner radius", "##theme_rounding",
                                      theme.cornerRadius, 0.0f, 12.0f, "%.0f px");
        themeChanged |= ThemeFloatRow("UI density", "##theme_density",
                                      theme.density, 0.75f, 1.35f, "%.2fx");

        ImGui::EndTable();
    }

    if (themeChanged) {
        MarkThemeDirty();
        ApplyTheme();
        themeStatus.clear();
    }

    ImGui::Spacing();
    ImGui::Text("Startup source: %s", GetThemeSourceName());
    const std::wstring sourcePath = GetThemeSourcePath();
    if (!sourcePath.empty()) {
        const std::string sourceUtf8 = WideToUtf8Adv(sourcePath);
        ImGui::TextDisabled("%s", sourceUtf8.c_str());
    } else {
        ImGui::TextDisabled("No theme file loaded — using the built-in theme.");
    }
    if (IsThemeDirty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::TextUnformatted("MODIFIED PREVIEW");
        ImGui::PopStyleColor();
    }

    if (ImGui::Button("Save to AppData")) {
        std::string error;
        if (SaveTheme(ThemeStorage::AppData, &error)) {
            themeStatus = "Theme saved to AppData.";
            themeStatusError = false;
        } else {
            themeStatus = "Theme save failed: " + error;
            themeStatusError = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save portable")) {
        std::string error;
        if (SaveTheme(ThemeStorage::Portable, &error)) {
            themeStatus = "Portable theme saved next to the configurator.";
            themeStatusError = false;
        } else {
            themeStatus = "Portable theme save failed: " + error;
            themeStatusError = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset preview")) {
        ResetThemePreview();
        themeStatus = "Built-in theme restored in memory. Save it if you want to replace a saved theme.";
        themeStatusError = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) {
        std::string error;
        if (ReloadThemeFromDisk(&error)) {
            themeStatus = "Theme reloaded. Portable overrides AppData when both exist.";
            themeStatusError = false;
        } else {
            themeStatus = "Theme reload warning: " + error;
            themeStatusError = true;
        }
    }

    if (!themeStatus.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              themeStatusError ? GetError() : GetMutedText());
        ImGui::TextWrapped("%s", themeStatus.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::TextDisabled(
        "Load order: configurator_theme.json beside the EXE, then AppData, then built-in defaults.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    SectionHeader("DEBUG / DIAGNOSTICS");
    ImGui::TextDisabled(
        "These switches configure the driver's diagnostic subsystem; the Diagnostics page itself stays read-only and lightweight.");

    bool diagEnabled = w.diagnosticsEnabled;
    if (ToggleSwitch("Diagnostics Enabled", &diagEnabled,
                     "Enable V3's runtime diagnostic subsystem.")) {
        w.diagnosticsEnabled = diagEnabled;
        doc.MarkDirty();
    }
    RestartRequiredBadge();

    bool diagWindow = w.diagnosticsWindow;
    if (ToggleSwitch("Diagnostics Window", &diagWindow,
                     "Shows V3's built-in runtime diagnostic window when enabled.")) {
        w.diagnosticsWindow = diagWindow;
        doc.MarkDirty();
    }
    RestartRequiredBadge();

    bool debugOutput = w.diagnosticsDebugOutput;
    if (ToggleSwitch("Debug Output", &debugOutput,
                     "Emits additional diagnostic messages to debugger output. Use DebugView or similar to view.")) {
        w.diagnosticsDebugOutput = debugOutput;
        doc.MarkDirty();
    }
    RestartRequiredBadge();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    SectionHeader("CONFIGURATION");

    const auto path = doc.GetActivePath();
    const std::string pathStr = WideToUtf8Adv(path);
    ImGui::Text("Config path:");
    ImGui::TextDisabled("%s", pathStr.c_str());

    ImGui::Spacing();

    if (ImGui::Button("Open Config Folder")) {
        std::wstring folder = path;
        const auto pos = folder.find_last_of(L"\\/");
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
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::Text("Reset ALL synth settings to defaults?");
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
        ImGui::PushStyleColor(ImGuiCol_Text, GetError());
        ImGui::TextWrapped("Parse error: %s", doc.ParseError().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, GetSuccess());
        ImGui::Text("No parse errors");
        ImGui::PopStyleColor();
    }

    if (!doc.ConfigWarning().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::TextWrapped("Warning: %s", doc.ConfigWarning().c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace svms::cfg
