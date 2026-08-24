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
#include <commdlg.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

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

ImVec4 HsvColor(float h, float s, float v, float a = 1.0f) {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImVec4(r, g, b, a);
}

void RebuildPaletteFromThemeColor(ThemeSettings& theme, const ImVec4& picked,
                                  float strength) {
    float h = 0.0f;
    float s = 0.0f;
    float v = 0.0f;
    ImGui::ColorConvertRGBtoHSV(picked.x, picked.y, picked.z, h, s, v);

    strength = std::clamp(strength, 0.0f, 1.0f);
    const float chroma = std::clamp(s, 0.0f, 1.0f);

    // Strength controls both saturation and luminance of the generated dark
    // surfaces. 0% is almost-neutral SVMS dark; 100% is intentionally bold,
    // but still dark enough to preserve contrast and avoid paint-bucket mode.
    const float surfaceSat = chroma * (0.06f + strength * 0.64f);

    theme.accent = ImVec4(picked.x, picked.y, picked.z, 1.0f);
    theme.background = HsvColor(h, surfaceSat * 0.78f,
                                0.070f + strength * 0.072f);
    theme.sidebar    = HsvColor(h, surfaceSat * 0.88f,
                                0.095f + strength * 0.090f);
    theme.panel      = HsvColor(h, surfaceSat * 0.84f,
                                0.086f + strength * 0.082f);
    theme.control    = HsvColor(h, surfaceSat,
                                0.150f + strength * 0.125f);

    // Keep text highly readable while letting stronger themes tint it just
    // enough that the whole UI belongs to the same colour family.
    theme.text      = HsvColor(h, chroma * (0.015f + strength * 0.040f), 0.925f);
    theme.mutedText = HsvColor(h, 0.025f + chroma * strength * 0.115f,
                               0.615f + strength * 0.025f);

    // These remain semantic instead of changing meaning with the theme hue.
    theme.warning = ImVec4(0.90f, 0.70f, 0.20f, 1.0f);
    theme.error   = ImVec4(0.85f, 0.30f, 0.30f, 1.0f);
    theme.success = ImVec4(0.30f, 0.75f, 0.40f, 1.0f);
}

bool confirmReset = false;
std::string themeStatus;
bool themeStatusError = false;
std::wstring lastProfileDirectory;
std::string profileStatus;
bool profileStatusError = false;

bool SelectProfileFile(HWND owner, bool save, std::wstring& path) {
    std::vector<wchar_t> buffer(32768u, L'\0');
    if (save) {
        const wchar_t* initialName = L"svms-profile.json";
        wcsncpy_s(buffer.data(), buffer.size(), initialName, _TRUNCATE);
    }
    static const wchar_t filter[] =
        L"SVMS JSON profiles (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrInitialDir = lastProfileDirectory.empty()
        ? nullptr : lastProfileDirectory.c_str();
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = save ? L"Export SVMS profile" : L"Import SVMS profile";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL selected = save ? GetSaveFileNameW(&ofn)
                               : GetOpenFileNameW(&ofn);
    if (!selected) return false;
    path = buffer.data();
    try {
        lastProfileDirectory =
            std::filesystem::path(path).parent_path().wstring();
    } catch (const std::filesystem::filesystem_error&) {
        lastProfileDirectory.clear();
    }
    return true;
}

HWND MainViewportWindow() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return nullptr;
    void* handle = viewport->PlatformHandleRaw
        ? viewport->PlatformHandleRaw : viewport->PlatformHandle;
    return static_cast<HWND>(handle);
}

} // namespace

void DrawAdvancedPage(ConfigDocument& doc) {
    auto& w = doc.Working();

    SectionHeader("APPEARANCE");
    ImGui::TextDisabled(
        "Pick one colour and the background, panels, controls and accents follow it automatically.");

    ThemeSettings& theme = EditThemeSettings();
    bool themeChanged = false;
    bool themeColorChanged = false;
    bool themeStrengthChanged = false;

    ImGui::BeginGroup();
    ImGui::TextUnformatted("THEME COLOR");
    ImGui::SetNextItemWidth(220.0f);
    themeColorChanged |= ImGui::ColorPicker3(
        "##theme_accent_wheel", &theme.accent.x,
        ImGuiColorEditFlags_PickerHueWheel |
        ImGuiColorEditFlags_NoSidePreview |
        ImGuiColorEditFlags_NoSmallPreview |
        ImGuiColorEditFlags_NoInputs);
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0.0f, 24.0f));
    ImGui::TextDisabled("Selected colour");
    ImGui::ColorButton("##theme_accent_preview", theme.accent,
                       ImGuiColorEditFlags_NoTooltip,
                       ImVec2(52.0f, 28.0f));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(128.0f);
    themeColorChanged |= ImGui::ColorEdit3(
        "##theme_accent_hex", &theme.accent.x,
        ImGuiColorEditFlags_DisplayHex |
        ImGuiColorEditFlags_NoPicker |
        ImGuiColorEditFlags_NoSmallPreview);

    ImGui::Spacing();
    ImGui::TextDisabled("Colour strength");
    ImGui::SetNextItemWidth(220.0f);
    float strengthPercent = theme.colorStrength * 100.0f;
    if (ImGui::SliderFloat("##theme_colour_strength", &strengthPercent,
                           0.0f, 100.0f, "%.0f%%")) {
        theme.colorStrength = strengthPercent / 100.0f;
        themeStrengthChanged = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "Controls how strongly the selected hue tints and brightens the generated UI surfaces.");
        ImGui::EndTooltip();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Advanced overrides remain available below.");
    ImGui::EndGroup();

    if (themeColorChanged || themeStrengthChanged) {
        const ImVec4 picked = theme.accent;
        RebuildPaletteFromThemeColor(theme, picked, theme.colorStrength);
        themeChanged = true;
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced palette / layout")) {
        ImGui::TextDisabled(
            "Override any generated colour here. Moving the wheel or strength slider again regenerates this palette.");
        if (ImGui::BeginTable("##theme_advanced_settings", 2,
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_RowBg,
                              ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);

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
    ImGui::TextDisabled(
        "Profiles are complete JSON working copies. Import does not overwrite config.json until you save.");
    ImGui::BeginDisabled(doc.IsReadOnly());
    if (ImGui::Button("Import Profile...")) {
        std::wstring profilePath;
        if (SelectProfileFile(MainViewportWindow(), false, profilePath)) {
            std::string error;
            if (doc.ImportProfile(profilePath, &error)) {
                profileStatus =
                    "Profile imported into the working copy. Save Configuration to apply it.";
                profileStatusError = false;
            } else {
                profileStatus = "Profile import failed: " + error;
                profileStatusError = true;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Export Profile...")) {
        std::wstring profilePath;
        if (SelectProfileFile(MainViewportWindow(), true, profilePath)) {
            std::string error;
            if (doc.ExportProfile(profilePath, &error)) {
                profileStatus = "Profile exported successfully.";
                profileStatusError = false;
            } else {
                profileStatus = "Profile export failed: " + error;
                profileStatusError = true;
            }
        }
    }
    ImGui::EndDisabled();
    if (!profileStatus.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              profileStatusError ? GetError() : GetMutedText());
        ImGui::TextWrapped("%s", profileStatus.c_str());
        ImGui::PopStyleColor();
    }
    if (doc.IsReadOnly()) {
        ImGui::TextDisabled(
            "Profile import/export is disabled while the active configuration is read-only.");
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
    if (doc.IsReadOnly()) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::TextWrapped(
            "This file is read-only in the configurator. Its unknown or "
            "malformed data will not be overwritten.");
        ImGui::PopStyleColor();
    }
}

} // namespace svms::cfg
