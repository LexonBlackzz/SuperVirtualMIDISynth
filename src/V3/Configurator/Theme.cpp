#include "Theme.h"
#include "imgui.h"

namespace svms::cfg {

static void ApplyBaseColors() {
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.WindowPadding = ImVec2(12, 12);
    s.ScrollbarSize = 14.0f;
    s.GrabMinSize = 8.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.Alpha = 1.0f;

    ImVec4* colors = s.Colors;

    colors[ImGuiCol_Text]                  = ImVec4(0.90f, 0.91f, 0.92f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.37f, 0.40f, 0.44f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.066f, 0.075f, 0.082f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.082f, 0.090f, 0.102f, 0.96f);
    colors[ImGuiCol_Border]                = ImVec4(0.176f, 0.196f, 0.227f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.125f, 0.141f, 0.165f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.145f, 0.165f, 0.192f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.157f, 0.180f, 0.216f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.059f, 0.067f, 0.075f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.059f, 0.067f, 0.075f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.059f, 0.067f, 0.075f, 0.50f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.098f, 0.106f, 0.118f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.059f, 0.067f, 0.075f, 0.54f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.235f, 0.255f, 0.282f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.294f, 0.318f, 0.353f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.333f, 0.361f, 0.400f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.447f, 0.533f, 0.855f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.447f, 0.533f, 0.855f, 0.78f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.510f, 0.596f, 0.922f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.145f, 0.165f, 0.196f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.196f, 0.224f, 0.267f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.255f, 0.286f, 0.333f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.145f, 0.165f, 0.196f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.196f, 0.224f, 0.267f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.255f, 0.286f, 0.333f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.176f, 0.196f, 0.227f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.447f, 0.533f, 0.855f, 0.50f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.447f, 0.533f, 0.855f, 0.80f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.447f, 0.533f, 0.855f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.447f, 0.533f, 0.855f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.447f, 0.533f, 0.855f, 0.95f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.098f, 0.110f, 0.125f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.196f, 0.224f, 0.267f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.145f, 0.165f, 0.196f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.098f, 0.110f, 0.125f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.125f, 0.141f, 0.165f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.125f, 0.141f, 0.165f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.176f, 0.196f, 0.227f, 1.00f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.125f, 0.141f, 0.165f, 1.00f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.447f, 0.533f, 0.855f, 0.35f);
    colors[ImGuiCol_DragDropTarget]        = ImVec4(0.447f, 0.533f, 0.855f, 0.90f);
    colors[ImGuiCol_NavHighlight]          = ImVec4(0.447f, 0.533f, 0.855f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.20f, 0.22f, 0.27f, 0.60f);
}

static const ImVec4 kAccent       = ImVec4(0.447f, 0.533f, 0.855f, 1.00f);
static const ImVec4 kAccentDim    = ImVec4(0.447f, 0.533f, 0.855f, 0.15f);
static const ImVec4 kAccentHover  = ImVec4(0.510f, 0.596f, 0.922f, 1.00f);
static const ImVec4 kWarning      = ImVec4(0.90f, 0.70f, 0.20f, 1.0f);
static const ImVec4 kError        = ImVec4(0.85f, 0.30f, 0.30f, 1.0f);
static const ImVec4 kSuccess      = ImVec4(0.30f, 0.75f, 0.40f, 1.0f);
static const ImVec4 kMutedText    = ImVec4(0.56f, 0.59f, 0.62f, 1.0f);
static const ImVec4 kDisabledText = ImVec4(0.37f, 0.40f, 0.44f, 1.0f);
static const ImVec4 kSidebarBg    = ImVec4(0.090f, 0.102f, 0.118f, 1.0f);
static const ImVec4 kPanelBg      = ImVec4(0.078f, 0.086f, 0.102f, 1.0f);
static const ImVec4 kInputBorder  = ImVec4(0.176f, 0.196f, 0.227f, 1.0f);

void ApplyTheme() {
    ApplyBaseColors();
}

void PushEffectPageStyle() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.075f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.110f, 0.125f, 0.153f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.137f, 0.157f, 0.192f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.153f, 0.176f, 0.216f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.125f, 0.141f, 0.176f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.176f, 0.196f, 0.243f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.224f, 0.247f, 0.302f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.447f, 0.533f, 0.855f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.510f, 0.596f, 0.922f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 7));
}

void PopEffectPageStyle() {
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(9);
}

const ImVec4& GetAccent()     { return kAccent; }
const ImVec4& GetAccentDim()  { return kAccentDim; }
const ImVec4& GetAccentHover(){ return kAccentHover; }
const ImVec4& GetWarning()    { return kWarning; }
const ImVec4& GetError()      { return kError; }
const ImVec4& GetSuccess()    { return kSuccess; }
const ImVec4& GetMutedText()  { return kMutedText; }
const ImVec4& GetDisabledText(){ return kDisabledText; }
const ImVec4& GetSidebarBg()  { return kSidebarBg; }
const ImVec4& GetPanelBg()    { return kPanelBg; }
const ImVec4& GetInputBorder(){ return kInputBorder; }

} // namespace svms::cfg
