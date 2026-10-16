#include "PageAdvanced.h"
#include "PlatformUtil.h"
#include "ConfigDocument.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"

#ifdef _WIN32
#include <commdlg.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

namespace svms::cfg {
namespace {

std::string WideToUtf8Adv(const std::wstring& ws) {
    return svms::cfg::util::WideToUtf8(ws);
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

void RebuildPaletteFromThemeColor(ThemeSettings& theme, const ImVec4& picked,
                                  float strength) {
    theme.accent = picked;
    theme.colorStrength = strength;
    RebuildPalette(theme);
}

bool confirmReset = false;
std::string themeStatus;
bool themeStatusError = false;
std::wstring lastProfileDirectory;
std::string profileStatus;
bool profileStatusError = false;

bool SelectProfileFile(void* owner, bool save, std::wstring& path) {
#ifndef _WIN32
    (void)owner;
    std::string chosen;
    if (!util::PickFile(save, save ? "Export SVMS profile" : "Import SVMS profile",
                        util::WideToUtf8(lastProfileDirectory), "*.json", chosen))
        return false;
    path = util::Utf8ToWide(chosen);
    try {
        lastProfileDirectory = fs::path(path).parent_path().wstring();
    } catch (const fs::filesystem_error&) {
        lastProfileDirectory.clear();
    }
    return true;
#else
    std::vector<wchar_t> buffer(32768u, L'\0');
    if (save) {
        const wchar_t* initialName = L"svms-profile.json";
        wcsncpy_s(buffer.data(), buffer.size(), initialName, _TRUNCATE);
    }
    static const wchar_t filter[] =
        L"SVMS JSON profiles (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(owner);
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
            fs::path(path).parent_path().wstring();
    } catch (const fs::filesystem_error&) {
        lastProfileDirectory.clear();
    }
    return true;
#endif
}

void* MainViewportWindow() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return nullptr;
    void* handle = viewport->PlatformHandleRaw
        ? viewport->PlatformHandleRaw : viewport->PlatformHandle;
    return handle;
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
    ImGui::TextDisabled("Style");
    {
        static const char* styleLabels[] = {"Rack", "Modern"};
        int style = theme.style;
        if (KeyGroup("##theme_style", &style, styleLabels, 2)) {
            theme.style = style;
            RebuildPalette(theme);
            themeChanged = true;
        }
        const float keysH = ImGui::GetItemRectSize().y;
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (keysH - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextDisabled("Modern: Rounded, calm, readable. The new default. Rack: Hardware-rack styling: bevels, screws, lamps.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Presets");
    {
        struct Preset { const char* name; ImVec4 accent; float strength; };
        static const Preset presets[] = {
            {"Amber",    ImVec4(1.000f, 0.698f, 0.243f, 1.0f), 0.65f},
            {"Phosphor", ImVec4(0.435f, 0.863f, 0.549f, 1.0f), 0.65f},
            {"Ice",      ImVec4(0.369f, 0.784f, 1.000f, 1.0f), 0.65f},
            {"Magenta",  ImVec4(1.000f, 0.373f, 0.690f, 1.0f), 0.65f},
            {"Mono",     ImVec4(0.910f, 0.910f, 0.910f, 1.0f), 0.00f},
        };
        for (size_t i = 0; i < std::size(presets); ++i) {
            if (i) ImGui::SameLine();
            if (KeyButton(presets[i].name, ImVec2(84.0f, 28.0f))) {
                theme.accent = presets[i].accent;
                theme.colorStrength = presets[i].strength;
                themeColorChanged = true;
            }
        }
    }

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
        util::OpenExternal(WideToUtf8Adv(folder));
    }
    ImGui::SameLine();
    if (ImGui::Button("Open config.json")) {
        util::OpenExternal(WideToUtf8Adv(path));
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
