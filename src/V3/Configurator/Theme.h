#ifndef SVMS_CONFIGURATOR_THEME_H
#define SVMS_CONFIGURATOR_THEME_H

#include "imgui.h"

namespace svms::cfg {

struct ThemeColors {
    ImGuiCol_ idx;
    unsigned int col;
};

void ApplyTheme();
void PushEffectPageStyle();
void PopEffectPageStyle();

inline constexpr float kSidebarWidth = 200.0f;
inline constexpr float kFooterHeight = 48.0f;
inline constexpr float kHeaderHeight = 34.0f;
inline constexpr float kMinWindowWidth = 960.0f;
inline constexpr float kMinWindowHeight = 620.0f;
inline constexpr float kDefaultWindowWidth = 1180.0f;
inline constexpr float kDefaultWindowHeight = 760.0f;

} // namespace svms::cfg

#endif
