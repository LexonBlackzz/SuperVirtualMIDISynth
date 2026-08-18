#ifndef SVMS_CONFIGURATOR_THEME_H
#define SVMS_CONFIGURATOR_THEME_H

#include "imgui.h"

#include <string>

namespace svms::cfg {

struct ThemeColors {
    ImGuiCol_ idx;
    unsigned int col;
};

struct ThemeSettings {
    ImVec4 accent;
    ImVec4 background;
    ImVec4 sidebar;
    ImVec4 panel;
    ImVec4 control;
    ImVec4 text;
    ImVec4 mutedText;
    ImVec4 warning;
    ImVec4 error;
    ImVec4 success;
    float cornerRadius = 4.0f;
    float density = 1.0f;
};

enum class ThemeStorage {
    AppData,
    Portable,
};

const ThemeSettings& GetThemeSettings();
ThemeSettings& EditThemeSettings();
ThemeSettings BuiltInTheme();

void ApplyTheme();
void MarkThemeDirty();
bool IsThemeDirty();
const char* GetThemeSourceName();
std::wstring GetThemeSourcePath();
std::wstring GetAppDataThemePath();
std::wstring GetPortableThemePath();

bool SaveTheme(ThemeStorage storage, std::string* error = nullptr);
void ResetThemePreview();
bool ReloadThemeFromDisk(std::string* error = nullptr);

void PushEffectPageStyle();
void PopEffectPageStyle();

const ImVec4& GetAccent();
ImVec4 GetAccentDim();
ImVec4 GetAccentHover();
const ImVec4& GetWarning();
const ImVec4& GetError();
const ImVec4& GetSuccess();
const ImVec4& GetMutedText();
ImVec4 GetDisabledText();
const ImVec4& GetSidebarBg();
const ImVec4& GetPanelBg();
ImVec4 GetInputBorder();

inline constexpr float kSidebarWidth = 200.0f;
inline constexpr float kFooterHeight = 48.0f;
inline constexpr float kHeaderHeight = 34.0f;
inline constexpr float kMinWindowWidth = 960.0f;
inline constexpr float kMinWindowHeight = 620.0f;
inline constexpr float kDefaultWindowWidth = 1180.0f;
inline constexpr float kDefaultWindowHeight = 760.0f;

} // namespace svms::cfg

#endif
