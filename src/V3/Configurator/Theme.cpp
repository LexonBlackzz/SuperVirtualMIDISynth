#include "Theme.h"
#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace svms::cfg {
namespace {

enum class ThemeSource {
    BuiltIn,
    AppData,
    Portable,
};

ThemeSettings g_theme{};
ThemeSource g_source = ThemeSource::BuiltIn;
std::wstring g_sourcePath;
bool g_loaded = false;
bool g_dirty = false;

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

ImVec4 Alpha(const ImVec4& c, float a) {
    return ImVec4(c.x, c.y, c.z, a);
}

std::filesystem::path PortableThemePathImpl() {
    wchar_t exePath[32768] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath,
                                         static_cast<DWORD>(std::size(exePath)));
    if (len == 0 || len >= std::size(exePath)) {
        return std::filesystem::path(L"configurator_theme.json");
    }
    return std::filesystem::path(exePath).parent_path() /
           L"configurator_theme.json";
}

std::filesystem::path AppDataThemePathImpl() {
    wchar_t appData[32768] = {};
    const DWORD len = GetEnvironmentVariableW(
        L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
    if (len == 0 || len >= std::size(appData)) return {};
    return std::filesystem::path(appData) /
           L"SuperVirtualMIDISynth" / L"configurator_theme.json";
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseHexByte(const std::string& s, size_t pos, unsigned& out) {
    if (pos + 1 >= s.size()) return false;
    const int hi = HexNibble(s[pos]);
    const int lo = HexNibble(s[pos + 1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<unsigned>((hi << 4) | lo);
    return true;
}

bool ParseColor(const nlohmann::json& value, ImVec4& out) {
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
        unsigned r = 0, g = 0, b = 0, a = 255;
        if (!ParseHexByte(s, 1, r) || !ParseHexByte(s, 3, g) ||
            !ParseHexByte(s, 5, b)) return false;
        if (s.size() == 9 && !ParseHexByte(s, 7, a)) return false;
        out = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
        return true;
    }

    if (value.is_array() && (value.size() == 3 || value.size() == 4)) {
        try {
            out.x = value.at(0).get<float>();
            out.y = value.at(1).get<float>();
            out.z = value.at(2).get<float>();
            out.w = value.size() == 4 ? value.at(3).get<float>() : 1.0f;
            out.x = (std::max)(0.0f, (std::min)(1.0f, out.x));
            out.y = (std::max)(0.0f, (std::min)(1.0f, out.y));
            out.z = (std::max)(0.0f, (std::min)(1.0f, out.z));
            out.w = (std::max)(0.0f, (std::min)(1.0f, out.w));
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

std::string ColorToHex(const ImVec4& c) {
    auto byte = [](float v) -> unsigned {
        v = (std::max)(0.0f, (std::min)(1.0f, v));
        return static_cast<unsigned>(std::lround(v * 255.0f));
    };
    char buf[16] = {};
    const unsigned a = byte(c.w);
    if (a == 255u) {
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                      byte(c.x), byte(c.y), byte(c.z));
    } else {
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X",
                      byte(c.x), byte(c.y), byte(c.z), a);
    }
    return std::string(buf);
}

void ReadColor(const nlohmann::json& root, const char* key, ImVec4& value) {
    const auto it = root.find(key);
    if (it != root.end()) {
        ImVec4 parsed = value;
        if (ParseColor(*it, parsed)) value = parsed;
    }
}

bool LoadThemeFile(const std::filesystem::path& path,
                   ThemeSettings& out,
                   std::string* error) {
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error) *error = "Could not open theme file.";
            return false;
        }
        nlohmann::json root;
        in >> root;
        if (!root.is_object()) {
            if (error) *error = "Theme JSON root must be an object.";
            return false;
        }

        ThemeSettings t = BuiltInTheme();
        ReadColor(root, "accent", t.accent);
        ReadColor(root, "background", t.background);
        ReadColor(root, "sidebar", t.sidebar);
        ReadColor(root, "panel", t.panel);
        ReadColor(root, "control", t.control);
        ReadColor(root, "text", t.text);
        ReadColor(root, "mutedText", t.mutedText);
        ReadColor(root, "warning", t.warning);
        ReadColor(root, "error", t.error);
        ReadColor(root, "success", t.success);

        if (root.contains("cornerRadius") && root["cornerRadius"].is_number())
            t.cornerRadius = root["cornerRadius"].get<float>();
        if (root.contains("density") && root["density"].is_number())
            t.density = root["density"].get<float>();

        t.cornerRadius = (std::max)(0.0f, (std::min)(12.0f, t.cornerRadius));
        t.density = (std::max)(0.75f, (std::min)(1.35f, t.density));
        out = t;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

nlohmann::json ThemeToJson(const ThemeSettings& t) {
    return {
        {"schema", 1},
        {"accent", ColorToHex(t.accent)},
        {"background", ColorToHex(t.background)},
        {"sidebar", ColorToHex(t.sidebar)},
        {"panel", ColorToHex(t.panel)},
        {"control", ColorToHex(t.control)},
        {"text", ColorToHex(t.text)},
        {"mutedText", ColorToHex(t.mutedText)},
        {"warning", ColorToHex(t.warning)},
        {"error", ColorToHex(t.error)},
        {"success", ColorToHex(t.success)},
        {"cornerRadius", t.cornerRadius},
        {"density", t.density},
    };
}

bool WriteThemeFile(const std::filesystem::path& path,
                    const ThemeSettings& theme,
                    std::string* error) {
    try {
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());

        const std::filesystem::path temp = path.wstring() + L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                if (error) *error = "Could not create theme file.";
                return false;
            }
            out << ThemeToJson(theme).dump(2) << '\n';
            if (!out.good()) {
                if (error) *error = "Could not finish writing theme file.";
                return false;
            }
        }

        if (!MoveFileExW(temp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            if (error) *error = "Could not replace the saved theme file.";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

void EnsureThemeLoaded(std::string* error = nullptr) {
    if (g_loaded) return;

    g_theme = BuiltInTheme();
    g_source = ThemeSource::BuiltIn;
    g_sourcePath.clear();
    g_dirty = false;

    const auto portable = PortableThemePathImpl();
    const auto appData = AppDataThemePathImpl();
    std::error_code ec;
    std::string localError;

    if (!portable.empty() && std::filesystem::exists(portable, ec)) {
        ThemeSettings loaded;
        if (LoadThemeFile(portable, loaded, &localError)) {
            g_theme = loaded;
            g_source = ThemeSource::Portable;
            g_sourcePath = portable.wstring();
            g_loaded = true;
            return;
        }
    }

    ec.clear();
    if (!appData.empty() && std::filesystem::exists(appData, ec)) {
        ThemeSettings loaded;
        std::string appError;
        if (LoadThemeFile(appData, loaded, &appError)) {
            g_theme = loaded;
            g_source = ThemeSource::AppData;
            g_sourcePath = appData.wstring();
            g_loaded = true;
            return;
        }
        if (error) *error = appError;
    } else if (error && !localError.empty()) {
        *error = localError;
    }

    g_loaded = true;
}

void ApplyBaseColors() {
    auto& s = ImGui::GetStyle();
    const ThemeSettings& t = g_theme;
    const float d = t.density;

    s.WindowRounding = t.cornerRadius;
    s.ChildRounding = t.cornerRadius;
    s.FrameRounding = (std::min)(t.cornerRadius, 6.0f);
    s.GrabRounding = (std::min)(t.cornerRadius, 6.0f);
    s.PopupRounding = t.cornerRadius;
    s.ScrollbarRounding = t.cornerRadius;
    s.TabRounding = (std::min)(t.cornerRadius, 6.0f);
    s.FramePadding = ImVec2(8.0f * d, 4.0f * d);
    s.ItemSpacing = ImVec2(8.0f * d, 6.0f * d);
    s.ItemInnerSpacing = ImVec2(6.0f * d, 4.0f * d);
    s.WindowPadding = ImVec2(12.0f * d, 12.0f * d);
    s.ScrollbarSize = 14.0f * d;
    s.GrabMinSize = 8.0f * d;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.Alpha = 1.0f;

    ImVec4* colors = s.Colors;
    const ImVec4 border = Mix(t.panel, t.text, 0.14f);
    const ImVec4 controlHover = Mix(t.control, t.accent, 0.14f);
    const ImVec4 controlActive = Mix(t.control, t.accent, 0.25f);
    const ImVec4 accentHover = Mix(t.accent, t.text, 0.12f);
    const ImVec4 disabled = Mix(t.mutedText, t.background, 0.38f);

    colors[ImGuiCol_Text]                  = t.text;
    colors[ImGuiCol_TextDisabled]          = disabled;
    colors[ImGuiCol_WindowBg]              = t.background;
    colors[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg]               = Alpha(Mix(t.background, t.panel, 0.55f), 0.98f);
    colors[ImGuiCol_Border]                = border;
    colors[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg]               = t.control;
    colors[ImGuiCol_FrameBgHovered]        = controlHover;
    colors[ImGuiCol_FrameBgActive]         = controlActive;
    colors[ImGuiCol_TitleBg]               = Mix(t.background, t.sidebar, 0.35f);
    colors[ImGuiCol_TitleBgActive]         = Mix(t.background, t.sidebar, 0.35f);
    colors[ImGuiCol_TitleBgCollapsed]      = Alpha(Mix(t.background, t.sidebar, 0.35f), 0.55f);
    colors[ImGuiCol_MenuBarBg]             = t.sidebar;
    colors[ImGuiCol_ScrollbarBg]           = Alpha(t.background, 0.65f);
    colors[ImGuiCol_ScrollbarGrab]         = Mix(t.control, t.text, 0.13f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = Mix(t.control, t.text, 0.23f);
    colors[ImGuiCol_ScrollbarGrabActive]   = Mix(t.control, t.text, 0.32f);
    colors[ImGuiCol_CheckMark]             = t.accent;
    colors[ImGuiCol_SliderGrab]            = Alpha(t.accent, 0.82f);
    colors[ImGuiCol_SliderGrabActive]      = accentHover;
    colors[ImGuiCol_Button]                = controlHover;
    colors[ImGuiCol_ButtonHovered]         = Mix(t.control, t.accent, 0.27f);
    colors[ImGuiCol_ButtonActive]          = Mix(t.control, t.accent, 0.40f);
    colors[ImGuiCol_Header]                = controlHover;
    colors[ImGuiCol_HeaderHovered]         = Mix(t.control, t.accent, 0.27f);
    colors[ImGuiCol_HeaderActive]          = Mix(t.control, t.accent, 0.40f);
    colors[ImGuiCol_Separator]             = border;
    colors[ImGuiCol_SeparatorHovered]      = Alpha(t.accent, 0.55f);
    colors[ImGuiCol_SeparatorActive]       = Alpha(t.accent, 0.85f);
    colors[ImGuiCol_ResizeGrip]            = Alpha(t.accent, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]     = Alpha(t.accent, 0.67f);
    colors[ImGuiCol_ResizeGripActive]      = Alpha(t.accent, 0.95f);
    colors[ImGuiCol_Tab]                   = Mix(t.background, t.control, 0.48f);
    colors[ImGuiCol_TabHovered]            = Mix(t.control, t.accent, 0.27f);
    colors[ImGuiCol_TabActive]             = controlHover;
    colors[ImGuiCol_TabUnfocused]          = Mix(t.background, t.control, 0.48f);
    colors[ImGuiCol_TabUnfocusedActive]    = t.control;
    colors[ImGuiCol_TableHeaderBg]         = t.control;
    colors[ImGuiCol_TableBorderStrong]     = border;
    colors[ImGuiCol_TableBorderLight]      = Mix(t.panel, t.text, 0.08f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt]         = Alpha(t.text, 0.025f);
    colors[ImGuiCol_TextSelectedBg]        = Alpha(t.accent, 0.35f);
    colors[ImGuiCol_DragDropTarget]        = Alpha(t.accent, 0.90f);
    colors[ImGuiCol_NavHighlight]          = t.accent;
    colors[ImGuiCol_NavWindowingHighlight] = Alpha(t.text, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]     = Alpha(t.text, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]      = Alpha(t.background, 0.70f);
}

} // namespace

ThemeSettings BuiltInTheme() {
    ThemeSettings t;
    t.accent     = ImVec4(0.447f, 0.533f, 0.855f, 1.00f);
    t.background = ImVec4(0.066f, 0.075f, 0.082f, 1.00f);
    t.sidebar    = ImVec4(0.090f, 0.102f, 0.118f, 1.00f);
    t.panel      = ImVec4(0.078f, 0.086f, 0.102f, 1.00f);
    t.control    = ImVec4(0.125f, 0.141f, 0.165f, 1.00f);
    t.text       = ImVec4(0.90f, 0.91f, 0.92f, 1.00f);
    t.mutedText  = ImVec4(0.56f, 0.59f, 0.62f, 1.00f);
    t.warning    = ImVec4(0.90f, 0.70f, 0.20f, 1.00f);
    t.error      = ImVec4(0.85f, 0.30f, 0.30f, 1.00f);
    t.success    = ImVec4(0.30f, 0.75f, 0.40f, 1.00f);
    t.cornerRadius = 4.0f;
    t.density = 1.0f;
    return t;
}

const ThemeSettings& GetThemeSettings() {
    EnsureThemeLoaded();
    return g_theme;
}

ThemeSettings& EditThemeSettings() {
    EnsureThemeLoaded();
    return g_theme;
}

void ApplyTheme() {
    EnsureThemeLoaded();
    ApplyBaseColors();
}

void MarkThemeDirty() {
    EnsureThemeLoaded();
    g_dirty = true;
}

bool IsThemeDirty() {
    EnsureThemeLoaded();
    return g_dirty;
}

const char* GetThemeSourceName() {
    EnsureThemeLoaded();
    switch (g_source) {
        case ThemeSource::Portable: return "Portable override";
        case ThemeSource::AppData:  return "AppData";
        default:                    return "Built-in";
    }
}

std::wstring GetThemeSourcePath() {
    EnsureThemeLoaded();
    return g_sourcePath;
}

std::wstring GetAppDataThemePath() {
    return AppDataThemePathImpl().wstring();
}

std::wstring GetPortableThemePath() {
    return PortableThemePathImpl().wstring();
}

bool SaveTheme(ThemeStorage storage, std::string* error) {
    EnsureThemeLoaded();
    const auto path = storage == ThemeStorage::Portable
        ? PortableThemePathImpl() : AppDataThemePathImpl();
    if (path.empty()) {
        if (error) *error = "Theme path is unavailable.";
        return false;
    }
    if (!WriteThemeFile(path, g_theme, error)) return false;

    g_source = storage == ThemeStorage::Portable
        ? ThemeSource::Portable : ThemeSource::AppData;
    g_sourcePath = path.wstring();
    g_dirty = false;
    return true;
}

void ResetThemePreview() {
    EnsureThemeLoaded();
    g_theme = BuiltInTheme();
    g_dirty = true;
    ApplyBaseColors();
}

bool ReloadThemeFromDisk(std::string* error) {
    g_loaded = false;
    EnsureThemeLoaded(error);
    ApplyBaseColors();
    return error == nullptr || error->empty();
}

void PushEffectPageStyle() {
    EnsureThemeLoaded();
    const ThemeSettings& t = g_theme;
    const ImVec4 effectBg = Mix(t.background, t.panel, 0.24f);
    const ImVec4 frame = Mix(t.control, t.background, 0.16f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, effectBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, frame);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Mix(frame, t.accent, 0.13f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Mix(frame, t.accent, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_Button, Mix(t.control, t.background, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Mix(t.control, t.accent, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(t.control, t.accent, 0.34f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Alpha(t.accent, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Mix(t.accent, t.text, 0.12f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, (std::min)(t.cornerRadius, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, (std::min)(t.cornerRadius, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * t.density, 5.0f * t.density));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * t.density, 7.0f * t.density));
}

void PopEffectPageStyle() {
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(9);
}

const ImVec4& GetAccent()      { EnsureThemeLoaded(); return g_theme.accent; }
ImVec4 GetAccentDim()          { EnsureThemeLoaded(); return Alpha(g_theme.accent, 0.15f); }
ImVec4 GetAccentHover()        { EnsureThemeLoaded(); return Mix(g_theme.accent, g_theme.text, 0.12f); }
const ImVec4& GetWarning()     { EnsureThemeLoaded(); return g_theme.warning; }
const ImVec4& GetError()       { EnsureThemeLoaded(); return g_theme.error; }
const ImVec4& GetSuccess()     { EnsureThemeLoaded(); return g_theme.success; }
const ImVec4& GetMutedText()   { EnsureThemeLoaded(); return g_theme.mutedText; }
ImVec4 GetDisabledText()       { EnsureThemeLoaded(); return Mix(g_theme.mutedText, g_theme.background, 0.38f); }
const ImVec4& GetSidebarBg()   { EnsureThemeLoaded(); return g_theme.sidebar; }
const ImVec4& GetPanelBg()     { EnsureThemeLoaded(); return g_theme.panel; }
ImVec4 GetInputBorder()        { EnsureThemeLoaded(); return Mix(g_theme.panel, g_theme.text, 0.14f); }

} // namespace svms::cfg
