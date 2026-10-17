#include "PageSettings.h"
#include "Anim.h"
#include "ConfigDocument.h"
#include "ConfiguratorApp.h"
#include "ModernWidgets.h"
#include "PlatformUtil.h"
#include "SVMSBuildInfo.h"
#include "Theme.h"
#include "UpdateService.h"
#include "Widgets.h"
#include "imgui.h"
#include "imgui_internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace svms::cfg {
namespace {

float S() { return ui::Scale(); }
ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

void Toast(const std::string& text) {
    if (ConfiguratorApp* app = GetLiveLinkContext().app) app->ShowToast(text);
}

void Muted(const char* text, float topGap = 0.0f) {
    if (topGap > 0.0f) ImGui::Dummy(ImVec2(0.0f, topGap * S() - ImGui::GetStyle().ItemSpacing.y));
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::PopFont();
}

// The theme file is rewritten after every change here: where it was loaded
// from (portable next to the exe, else AppData).
void PersistTheme() {
    std::string error;
    const bool portable = std::string(GetThemeSourceName()).rfind("Portable", 0) == 0;
    if (!SaveTheme(portable ? ThemeStorage::Portable : ThemeStorage::AppData, &error))
        Toast("Could not save the look: " + error);
}

void ThemeChanged(bool persist = true) {
    MarkThemeDirty();
    ApplyTheme();
    if (persist) PersistTheme();
}

// --- Look ------------------------------------------------------------------------------------

// One theme card: a small preview of the style, name and a line of text.
bool ThemeCard(const char* id, ImVec2 pos, float width, bool selected, bool rackPreview,
               const char* name, const char* desc) {
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    ui::PushText(2, 15.0f);
    const float nameH = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushText(0, 12.0f);
    const float descH = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float h = 16.0f * s * 2.0f + 84.0f * s + 10.0f * s + nameH + descH;
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##card", ImVec2(width, h));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const bool clicked = ImGui::IsItemClicked();
    const float lift = AnimSpring(ImGui::GetID("##lift"), hov ? 1.0f : 0.0f) * 3.0f * s;
    const float selT = AnimEase(ImGui::GetID("##sel"), selected ? 1.0f : 0.0f, 0.25f);
    const ImVec2 a(pos.x, pos.y - lift), b(pos.x + width, pos.y + h - lift);
    const float r = k.rack ? 3.0f * s : 12.0f * s;
    if (selT > 0.01f)
        dl->AddRect(ImVec2(a.x - 2.0f * s, a.y - 2.0f * s), ImVec2(b.x + 2.0f * s, b.y + 2.0f * s),
                    ui::WithAlpha(k.accSoft, selT), r + 2.0f * s, 0, 4.0f * s);
    dl->AddRectFilled(a, b, k.card2, r);
    dl->AddRect(a, b, ui::MixU32(k.line2, k.acc, selT), r);

    const ImVec2 pa(a.x + 16.0f * s, a.y + 16.0f * s), pb(b.x - 16.0f * s, pa.y + 84.0f * s);
    dl->AddRectFilled(pa, pb, rackPreview ? IM_COL32(0x1B, 0x1A, 0x18, 255) : IM_COL32(0x0E, 0x0E, 0x11, 255), 8.0f * s);
    const float inner = pb.x - pa.x - 16.0f * s;
    const float w1 = inner * 0.22f;
    const ImVec2 ia(pa.x + 8.0f * s, pa.y + 8.0f * s), ib(pb.x - 8.0f * s, pb.y - 8.0f * s);
    auto box = [&](ImVec2 p0, ImVec2 p1) {
        if (rackPreview) {
            dl->AddRectFilledMultiColor(p0, p1, IM_COL32(0x2E, 0x2D, 0x29, 255), IM_COL32(0x2E, 0x2D, 0x29, 255),
                                        IM_COL32(0x24, 0x23, 0x1F, 255), IM_COL32(0x24, 0x23, 0x1F, 255));
            dl->AddLine(ImVec2(p0.x, p0.y + 0.5f), ImVec2(p1.x, p0.y + 0.5f), IM_COL32(255, 255, 255, 20));
        } else {
            dl->AddRectFilled(p0, p1, IM_COL32(0x1C, 0x1C, 0x22, 255), 6.0f * s);
        }
    };
    box(ia, ImVec2(ia.x + w1, ib.y));
    box(ImVec2(ia.x + w1 + 6.0f * s, ia.y), ib);

    ui::PushText(2, 15.0f);
    dl->AddText(ImVec2(a.x + 16.0f * s, pb.y + 10.0f * s), k.text, ui::Caps(name));
    ui::PopFont();
    ui::PushText(0, 12.0f);
    dl->AddText(ImVec2(a.x + 16.0f * s, pb.y + 10.0f * s + nameH), k.muted, desc);
    ui::PopFont();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(width, 0.0f));
    return clicked;
}

struct Swatch {
    const char* name;
    unsigned rgb;
};
const Swatch kSwatches[5] = {{"Amber", 0xFFB23E}, {"Phosphor", 0x5CF08A}, {"Ice", 0x7DD3FC},
                             {"Magenta", 0xF472B6}, {"Mono", 0xD4D4D8}};

ImVec4 FromRgb(unsigned rgb) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f);
}

bool SameColor(const ImVec4& a, const ImVec4& b) {
    return std::fabs(a.x - b.x) < 0.004f && std::fabs(a.y - b.y) < 0.004f && std::fabs(a.z - b.z) < 0.004f;
}

// Round swatch with the selection ring from the prototype. Returns true on click.
bool SwatchButton(const char* id, ImVec2 center, ImU32 fill, bool selected, const char* tooltip,
                  bool rainbow = false) {
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(center.x - 17.0f * s, center.y - 17.0f * s));
    ImGui::InvisibleButton("##sw", ImVec2(34.0f * s, 34.0f * s));
    const bool hov = ImGui::IsItemHovered();
    if (hov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", tooltip);
    }
    const bool clicked = ImGui::IsItemClicked();
    const float scale = AnimSpring(ImGui::GetID("##scale"), hov ? 1.12f : 1.0f);
    const float r = 17.0f * s * scale;
    if (selected) {
        dl->AddCircle(center, r + 4.0f * s, k.text, 40, 2.0f * s);
        dl->AddCircle(center, r + 1.5f * s, k.bg, 40, 3.0f * s);
    }
    if (rainbow) {
        static const unsigned hues[6] = {0xFF4D4D, 0xFFD24D, 0x5CF08A, 0x4DDCFF, 0x6A5CFF, 0xF472B6};
        for (int i = 0; i < 6; ++i) {
            const float a0 = i * 3.14159265f / 3.0f - 1.5708f, a1 = (i + 1) * 3.14159265f / 3.0f - 1.5708f;
            dl->PathLineTo(center);
            dl->PathArcTo(center, r, a0, a1, 8);
            dl->PathFillConvex(U32(FromRgb(hues[i])));
        }
    }
    if (fill) dl->AddCircleFilled(center, r, fill, 40);
    ImGui::PopID();
    return clicked;
}

// --- Themed colour picker --------------------------------------------------------------------------
// Replaces the OS picker: a popover under the Custom swatch with a saturation/value square, a hue
// bar (ring handles that grow while dragged), a preview, a hex field and Done. Everything recolours
// live: each change rebuilds the theme palette.

// Paints the four corners outside a rounded rect with `bg`, so gradients (which have no rounding)
// can sit on a rounded surface.
void RoundCorners(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 bg) {
    // A thick stroke just outside the rect: its inner edge is the rounded rect, the rest hides the corners.
    const float t = r + 3.0f;
    dl->AddRect(ImVec2(a.x - t * 0.5f, a.y - t * 0.5f), ImVec2(b.x + t * 0.5f, b.y + t * 0.5f), bg, r + t * 0.5f, 0, t);
}

// White ring handle, 18 px, growing to 1.25x while dragged.
void RingHandle(ImDrawList* dl, ImVec2 c, ImU32 fill, bool active, ImGuiID id) {
    const float s = S();
    const float r = 9.0f * s * AnimSpring(id, active ? 1.25f : 1.0f);
    dl->AddCircleFilled(ImVec2(c.x, c.y + 3.0f * s), r + 1.0f * s, IM_COL32(0, 0, 0, 70), 24);
    dl->AddCircleFilled(c, r, fill, 28);
    dl->AddCircle(c, r - 1.5f * s, IM_COL32(255, 255, 255, 255), 28, 3.0f * s);
    dl->AddCircle(c, r + 0.5f, IM_COL32(0, 0, 0, 90), 28, 1.0f);
}

bool ParseHex6(const char* text, ImVec4& out) {
    if (*text == '#') ++text;
    if (std::strlen(text) != 6) return false;
    unsigned v = 0;
    for (int i = 0; i < 6; ++i) {
        const char ch = text[i];
        unsigned d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = 10 + ch - 'a';
        else if (ch >= 'A' && ch <= 'F') d = 10 + ch - 'A';
        else return false;
        v = v * 16 + d;
    }
    out = FromRgb(v);
    return true;
}

struct PickerState {
    float h = 0.0f, sat = 0.0f, val = 1.0f;
    char hex[16] = {};
    bool dirty = false;   // a change is waiting for the theme file save (on close)
};
PickerState g_picker;

void PickerApply(ThemeSettings& theme, bool final) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(g_picker.h, g_picker.sat, g_picker.val, r, g, b);
    const ImVec4 col(r, g, b, 1.0f);
    theme.customAccent = col;
    theme.hasCustomAccent = true;
    theme.accent = col;
    theme.colorStrength = 0.65f;
    RebuildPalette(theme);
    ThemeChanged(final);
    std::snprintf(g_picker.hex, sizeof(g_picker.hex), "#%02X%02X%02X", static_cast<int>(r * 255.0f + 0.5f),
                  static_cast<int>(g * 255.0f + 0.5f), static_cast<int>(b * 255.0f + 0.5f));
    g_picker.dirty = !final;
}

// Called once when the popover opens: start from the Custom colour (or the prototype's violet) and apply it.
void PickerOpen(ThemeSettings& theme) {
    const ImVec4 start = theme.hasCustomAccent ? theme.customAccent : FromRgb(0x9B7BFF);
    ImGui::ColorConvertRGBtoHSV(start.x, start.y, start.z, g_picker.h, g_picker.sat, g_picker.val);
    PickerApply(theme, false);
}

void DrawPicker(ThemeSettings& theme, ImVec2 popupPos) {
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ImGui::SetNextWindowPos(popupPos);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, k.card2);
    ImGui::PushStyleColor(ImGuiCol_Border, k.line2);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 16.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * s, 16.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    const bool open = ImGui::BeginPopup("##accentpicker");
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = 258.0f * s;
        float cr, cg, cb;
        bool changed = false;

        // Saturation / value square.
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + w, a.y + 160.0f * s);
        ImGui::InvisibleButton("##sv", ImVec2(w, 160.0f * s));
        const bool svActive = ImGui::IsItemActive();
        if (svActive) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            g_picker.sat = ImClamp((m.x - a.x) / w, 0.0f, 1.0f);
            g_picker.val = 1.0f - ImClamp((m.y - a.y) / (160.0f * s), 0.0f, 1.0f);
            changed = true;
        }
        ImGui::ColorConvertHSVtoRGB(g_picker.h, 1.0f, 1.0f, cr, cg, cb);
        const ImU32 hueCol = ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 1.0f));
        dl->AddRectFilledMultiColor(a, b, IM_COL32_WHITE, hueCol, hueCol, IM_COL32_WHITE);
        dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32_BLACK, IM_COL32_BLACK);
        RoundCorners(dl, a, b, 12.0f * s, k.card2);
        ImGui::ColorConvertHSVtoRGB(g_picker.h, g_picker.sat, g_picker.val, cr, cg, cb);
        const ImU32 cur = ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 1.0f));
        RingHandle(dl, ImVec2(a.x + g_picker.sat * w, a.y + (1.0f - g_picker.val) * 160.0f * s), cur, svActive,
                   ImGui::GetID("##svk"));

        // Hue bar.
        ImGui::Dummy(ImVec2(0.0f, 14.0f * s - ImGui::GetStyle().ItemSpacing.y));
        const ImVec2 ha = ImGui::GetCursorScreenPos();
        const float hh = 14.0f * s;
        const ImVec2 hb(ha.x + w, ha.y + hh);
        ImGui::InvisibleButton("##hue", ImVec2(w, hh));
        const bool hueActive = ImGui::IsItemActive();
        if (hueActive) {
            g_picker.h = ImClamp((ImGui::GetIO().MousePos.x - ha.x) / w, 0.0f, 0.9999f);
            changed = true;
        }
        for (int i = 0; i < 6; ++i) {
            float r0, g0, b0, r1, g1, b1;
            ImGui::ColorConvertHSVtoRGB(i / 6.0f, 1.0f, 1.0f, r0, g0, b0);
            ImGui::ColorConvertHSVtoRGB((i + 1) / 6.0f > 0.9999f ? 0.0f : (i + 1) / 6.0f, 1.0f, 1.0f, r1, g1, b1);
            const ImU32 c0 = ImGui::ColorConvertFloat4ToU32(ImVec4(r0, g0, b0, 1.0f));
            const ImU32 c1 = ImGui::ColorConvertFloat4ToU32(ImVec4(r1, g1, b1, 1.0f));
            dl->AddRectFilledMultiColor(ImVec2(ha.x + w * i / 6.0f, ha.y), ImVec2(ha.x + w * (i + 1) / 6.0f, hb.y),
                                        c0, c1, c1, c0);
        }
        RoundCorners(dl, ha, hb, hh * 0.5f, k.card2);
        RingHandle(dl, ImVec2(ha.x + g_picker.h * w, ha.y + hh * 0.5f), hueCol, hueActive, ImGui::GetID("##huek"));

        if (changed) PickerApply(theme, false);

        // Preview, hex field, Done.
        ImGui::Dummy(ImVec2(0.0f, 14.0f * s - ImGui::GetStyle().ItemSpacing.y));
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        const float bw = ui::ButtonSize("Done").x;
        dl->AddRectFilled(rp, ImVec2(rp.x + 38.0f * s, rp.y + 38.0f * s), cur, 10.0f * s);
        dl->AddRect(rp, ImVec2(rp.x + 38.0f * s, rp.y + 38.0f * s), IM_COL32(255, 255, 255, 38), 10.0f * s);
        ImGui::SetCursorScreenPos(ImVec2(rp.x + 48.0f * s, rp.y));
        if (ui::Field("##hex", g_picker.hex, sizeof(g_picker.hex), w - 48.0f * s - bw - 10.0f * s)) {
            ImVec4 parsed;
            if (ParseHex6(g_picker.hex, parsed)) {
                ImGui::ColorConvertRGBtoHSV(parsed.x, parsed.y, parsed.z, g_picker.h, g_picker.sat, g_picker.val);
                PickerApply(theme, false);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(rp.x + w - bw, rp.y));
        if (ui::Button("Done", ui::ButtonKind::Primary)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    } else if (g_picker.dirty) {
        g_picker.dirty = false;
        PersistTheme();
    }
}

void LookCard(ConfigDocument&) {
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ThemeSettings& theme = EditThemeSettings();
    ui::BeginCard("##look", "Look", nullptr, 0.0f, "theme");

    // Modern / Rack.
    {
        const float gap = 12.0f * s;
        const float width = ImGui::GetContentRegionAvail().x;
        const float cw = (width - gap) * 0.5f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        float bottom = origin.y;
        if (ThemeCard("modern", origin, cw, theme.style == kStyleModern, false, "Modern",
                      "Rounded, calm, readable. The new default.") && theme.style != kStyleModern) {
            theme.style = kStyleModern;
            RebuildPalette(theme);
            ThemeChanged();
        }
        bottom = (std::max)(bottom, ImGui::GetCursorScreenPos().y);
        if (ThemeCard("rack", ImVec2(origin.x + cw + gap, origin.y), cw, theme.style == kStyleRack, true, "Rack",
                      "Hardware-rack styling: bevels, screws, lamps.") && theme.style != kStyleRack) {
            theme.style = kStyleRack;
            RebuildPalette(theme);
            ThemeChanged();
        }
        bottom = (std::max)(bottom, ImGui::GetCursorScreenPos().y);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));
        ImGui::Dummy(ImVec2(width, 0.0f));
    }

    // Accent colour.
    ImGui::Dummy(ImVec2(0.0f, 16.0f * s - ImGui::GetStyle().ItemSpacing.y));
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::TextUnformatted("Accent colour");
    ImGui::PopStyleColor();
    ui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 8.0f * s - ImGui::GetStyle().ItemSpacing.y));

    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float cy = row.y + 20.0f * s;
    bool presetActive = false;
    for (int i = 0; i < 5; ++i) {
        const ImVec4 c = FromRgb(kSwatches[i].rgb);
        const bool sel = SameColor(theme.accent, c);
        presetActive |= sel;
        if (SwatchButton(kSwatches[i].name, ImVec2(row.x + 20.0f * s + i * 44.0f * s, cy), U32(c), sel, kSwatches[i].name)) {
            theme.accent = c;
            theme.colorStrength = i == 4 ? 0.0f : 0.65f;
            RebuildPalette(theme);
            ThemeChanged();
        }
    }
    const ImVec2 custom(row.x + 20.0f * s + 5 * 44.0f * s, cy);
    const bool customActive = !presetActive;
    if (SwatchButton("custom", custom, theme.hasCustomAccent ? U32(theme.customAccent) : 0u, customActive,
                     "Pick any colour", !theme.hasCustomAccent))
    {
        PickerOpen(theme);
        ImGui::OpenPopup("##accentpicker");
    }
    {
        char hex[16];
        const ImVec4& shown = customActive ? theme.accent : theme.customAccent;
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", static_cast<int>(shown.x * 255.0f + 0.5f),
                      static_cast<int>(shown.y * 255.0f + 0.5f), static_cast<int>(shown.z * 255.0f + 0.5f));
        ui::PushNum(13.0f);
        const char* label = customActive || theme.hasCustomAccent ? hex : "Custom";
        ImGui::GetWindowDrawList()->AddText(ImVec2(custom.x + 17.0f * s + 14.0f * s, cy - ImGui::GetTextLineHeight() * 0.5f),
                                            k.muted, label);
        ui::PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 40.0f * s));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    DrawPicker(theme, ImVec2(row.x, row.y + 50.0f * s));

    // Motion.
    {
        static const char* labels[3] = {"Full", "Calm", "Off"};
        int motion = (std::max)(0, (std::min)(2, theme.motion));
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        ui::BeginRow("motion", "Motion", "Full is bouncy. Calm keeps fades but drops the springs. Off removes all animation.",
                     ui::SegmentedSize(labels, 3));
        if (ui::Segmented("##motion", &motion, labels, 3)) {
            theme.motion = motion;
            ThemeChanged();
        }
        ui::EndRow();
    }
    // Fire.
    ui::BeginRow("fire", "Load meter catches fire past 500%",
                 "Purely for fun. Turn it off if the flames feel heavy on a slow PC.", ImVec2(46.0f * s, 28.0f * s));
    if (ui::Toggle("##fire", &theme.fire)) ThemeChanged();
    ui::EndRow();
    // Welcome tour.
    {
        const float bw = ui::ButtonSize("Replay now").x;
        ui::BeginRow("tour", "Welcome tour", "Show the short introduction when the configurator opens.",
                     ImVec2(bw + 10.0f * s + 46.0f * s, 38.0f * s));
        ui::Button("Replay now", ui::ButtonKind::Normal, ImVec2(0, 0), false);
        ImGui::SameLine(0.0f, 10.0f * s);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f * s);
        if (ui::Toggle("##tour", &theme.tourAtStart)) ThemeChanged();
        ui::EndRow();
    }
    ui::EndCard();
}

// --- Configuration file ------------------------------------------------------------------------

std::wstring g_lastProfileDir;

bool SelectProfileFile(bool save, std::wstring& path) {
#ifndef _WIN32
    std::string chosen;
    if (!util::PickFile(save, save ? "Export SVMS profile" : "Import SVMS profile",
                        util::WideToUtf8(g_lastProfileDir), "*.json", chosen))
        return false;
    path = util::Utf8ToWide(chosen);
#else
    std::vector<wchar_t> buffer(32768u, L'\0');
    if (save) wcsncpy_s(buffer.data(), buffer.size(), L"svms-profile.json", _TRUNCATE);
    static const wchar_t filter[] = L"SVMS JSON profiles (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ofn.hwndOwner = static_cast<HWND>(viewport && viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw
                                                                               : (viewport ? viewport->PlatformHandle : nullptr));
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrInitialDir = g_lastProfileDir.empty() ? nullptr : g_lastProfileDir.c_str();
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = save ? L"Export SVMS profile" : L"Import SVMS profile";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
    path = buffer.data();
#endif
    try {
        g_lastProfileDir = fs::path(path).parent_path().wstring();
    } catch (const fs::filesystem_error&) {
        g_lastProfileDir.clear();
    }
    return true;
}

void ConfigCard(ConfigDocument& doc) {
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ui::BeginCard("##config", "Configuration file", nullptr, 0.0f, "config");
    const std::wstring path = doc.GetActivePath();
    const std::string pathUtf8 = util::WideToUtf8(path);
    ui::PushNum(13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(pathUtf8.empty() ? "No config file yet" : pathUtf8.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));

    const float gap = 8.0f * s;
    const bool ro = doc.IsReadOnly();
    if (ui::Button("Open folder")) {
        std::wstring folder = path;
        const auto pos = folder.find_last_of(L"\\/");
        if (pos != std::wstring::npos) folder.resize(pos);
        util::OpenExternal(util::WideToUtf8(folder));
    }
    ImGui::SameLine(0.0f, gap);
    if (ui::Button("Import profile\xE2\x80\xA6", ui::ButtonKind::Normal, ImVec2(0, 0), !ro)) {
        std::wstring profilePath;
        if (SelectProfileFile(false, profilePath)) {
            std::string error;
            if (doc.ImportProfile(profilePath, &error)) Toast("Profile imported. Save to apply it");
            else Toast("Import failed: " + error);
        }
    }
    ImGui::SameLine(0.0f, gap);
    if (ui::Button("Export profile\xE2\x80\xA6", ui::ButtonKind::Normal, ImVec2(0, 0), !ro)) {
        std::wstring profilePath;
        if (SelectProfileFile(true, profilePath)) {
            std::string error;
            if (doc.ExportProfile(profilePath, &error)) Toast("Profile exported");
            else Toast("Export failed: " + error);
        }
    }
    // Reset sits at the far right and asks twice.
    {
        const char* label = "Reset all to defaults";
        const char* armed = "Click again to reset everything";
        const float rw = (std::max)(ui::ButtonSize(label).x, ui::ButtonSize(armed).x);
        const float contentRight = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
        ImGui::SameLine(0.0f, gap);
        ImGui::SetCursorScreenPos(ImVec2((std::max)(ImGui::GetCursorScreenPos().x, contentRight - rw),
                                         ImGui::GetCursorScreenPos().y));
        if (ui::DangerButton("##reset", label, armed)) {
            doc.LoadDefaults();
            doc.MarkDirty();
            Toast("Defaults loaded. Save to keep them");
        }
    }
    if (ro) Muted("This file is read-only in the configurator. Its unknown or malformed data will not be overwritten.", 12.0f);
    if (doc.HasParseError()) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.bad);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("Parse error: %s", doc.ParseError().c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    if (!doc.ConfigWarning().empty()) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.exp);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("Warning: %s", doc.ConfigWarning().c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    ui::EndCard();
}

// --- Driver logging ----------------------------------------------------------------------------

void LoggingCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = S();
    ui::BeginCard("##logging", "Driver logging", nullptr, 0.0f, "diagOn");
    struct Item { const char* id; const char* title; const char* desc; bool ConfigValues::*field; };
    const Item items[3] = {
        {"diagOn", "Diagnostics", "Collect the numbers on the Diagnostics page.", &ConfigValues::diagnosticsEnabled},
        {"diagWin", "Monitor window", "Opens a small live window next to the player.", &ConfigValues::diagnosticsWindow},
        {"debugOut", "Debug output", "Writes log lines for DebugView.", &ConfigValues::diagnosticsDebugOutput},
    };
    for (const Item& item : items) {
        ui::BeginRow(item.id, item.title, item.desc, ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle(item.id, &(w.*item.field))) doc.MarkDirty();
        ui::EndRow();
    }
    ui::EndCard();
}

// --- About ---------------------------------------------------------------------------------------

void AboutCard(UpdateService& updates) {
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    const UpdateSnapshot update = updates.GetSnapshot();
    const bool checking = update.status == UpdateStatus::Checking;
    const char* buttonLabel = "Check for updates";
    ui::BeginCard("##about", "About", nullptr, ui::ButtonSize(buttonLabel).x);
    if (ui::Button(buttonLabel, ui::ButtonKind::Normal, ImVec2(0, 0), !checking))
        updates.CheckAsync(update.status != UpdateStatus::Idle);
    ui::CardHeaderEnd();

    char version[96];
    std::snprintf(version, sizeof(version), "%s (build %u, %s)", svms::build::kProductVersion,
                  svms::build::kBuildNumber, svms::build::kReleaseChannel);
    const char* keys[3] = {"Version", "License", "Repository"};
    const char* link = "github.com/LexonBlackzz/SuperVirtualMIDISynth";
    const char* vals[3] = {version, "GPL-3.0", ""};
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ui::KeyValueGrid(keys, vals, 3);
    // The repository value is a link: overlay it on its row (third row).
    {
        ui::PushNum(13.0f);
        const float lh = ImGui::GetTextLineHeight();
        const float rowH = lh + 8.0f * s;
        const ImVec2 lp(start.x + 176.0f * s, start.y + 2.0f * rowH);
        const ImVec2 ls = ImGui::CalcTextSize(link);
        ui::PopFont();
        ImGui::SetCursorScreenPos(lp);
        ImGui::InvisibleButton("##repo", ls);
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(lp.x, lp.y + ls.y), ImVec2(lp.x + ls.x, lp.y + ls.y), k.acc);
        }
        if (ImGui::IsItemClicked()) util::OpenExternal("https://github.com/LexonBlackzz/SuperVirtualMIDISynth");
        ui::PushNum(13.0f);
        ImGui::GetWindowDrawList()->AddText(lp, k.acc, link);
        ui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + 3.0f * rowH));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
    }

    // Update result.
    if (update.status == UpdateStatus::Checking) {
        Muted("Checking GitHub releases\xE2\x80\xA6", 12.0f);
    } else if (update.status == UpdateStatus::Available) {
        char text[96];
        std::snprintf(text, sizeof(text), "%u.%u.%u is available%s", update.major, update.minor, update.patch,
                      update.fromCache ? " (cached)" : "");
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::PushText(1, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.ok);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        ui::PopFont();
        if (!update.releaseUrl.empty() && ui::Button("Open release page")) util::OpenExternal(update.releaseUrl);
    } else if (update.status == UpdateStatus::UpToDate) {
        Muted(update.fromCache ? "Up to date (cached)." : "Up to date.", 12.0f);
    } else if (update.status == UpdateStatus::Failed) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.exp);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(update.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    Muted("Built for MIDI files that make normal synthesizers reconsider their career choices.", 12.0f);
    ui::EndCard();
}

} // namespace

void DrawSettingsPage(ConfigDocument& doc, UpdateService& updates) {
    LookCard(doc);
    ConfigCard(doc);
    LoggingCard(doc);
    AboutCard(updates);
}

} // namespace svms::cfg
