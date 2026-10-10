#include "Widgets.h"
#include "ConfiguratorApp.h"
#include "ConfigDocument.h"
#include "Theme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../SVMSRuntimeLink.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace svms::cfg {

static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
static ImVec4 Alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

static LiveLinkContext g_liveLink = {};

void SetLiveLinkContext(const LiveLinkContext& ctx) { g_liveLink = ctx; }
const LiveLinkContext& GetLiveLinkContext() { return g_liveLink; }

// Live changes are routed through the ConfiguratorApp so widgets never
// talk to the driver directly: every knob/edit marks its group dirty on
// the app's coalescing working live state, and the app sends one grouped
// ApplyLiveConfig command as soon as the current UI frame finishes.
void PushLiveFloat(svms::RLCommandType type, float value) {
    if (g_liveLink.app) g_liveLink.app->SetLiveFloat(type, value);
}

void PushLiveBool(svms::RLCommandType type, bool value) {
    if (g_liveLink.app) g_liveLink.app->SetLiveBool(type, value);
}

void PushLiveMaxVoices(uint32_t value) {
    if (g_liveLink.app) g_liveLink.app->SetLiveMaxVoices(value);
}

void PushLiveLimiterAlgorithm(uint32_t value) {
    if (g_liveLink.app) g_liveLink.app->SetLiveLimiterAlgorithm(value);
}

void PushLiveChannelLimiter(bool enabled, float threshold, float releaseMs) {
    if (g_liveLink.app) g_liveLink.app->SetLiveChannelLimiter(
        enabled, threshold, releaseMs);
}

static float g_toastTimer = 0.0f;
static char g_toastText[512] = {};
static bool g_toastActive = false;

bool IsRefinedStyle() { return GetThemeSettings().style == 1; }

std::string StyleText(const char* text) {
    if (!text) return {};
    if (!IsRefinedStyle()) return text;
    static const char* kKeep[] = {"MIDI", "SVMS", "ASIO", "WASAPI", "CPU", "RT60",
                                  "PID", "KDMAPI", "WINMM", "SF2", "API", "DLL",
                                  "GB", "MIB", "RL", "EDO", "LCD", "CC"};
    std::string out;
    const size_t n = std::strlen(text);
    size_t i = 0;
    bool first = true;
    while (i < n) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (std::isalnum(ch)) {
            size_t j = i;
            bool hasDigit = false;
            while (j < n && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '.')) {
                if (std::isdigit(static_cast<unsigned char>(text[j]))) hasDigit = true;
                ++j;
            }
            std::string tok(text + i, j - i);
            std::string upper = tok;
            for (char& u : upper) u = static_cast<char>(std::toupper(static_cast<unsigned char>(u)));
            bool keep = hasDigit;
            for (const char* k : kKeep) if (upper == k) keep = true;
            if (upper == "OMNIMIDI") {
                tok = "OmniMIDI";
            } else if (!keep) {
                for (char& l : tok) l = static_cast<char>(std::tolower(static_cast<unsigned char>(l)));
                if (first) tok[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tok[0])));
            }
            out += tok;
            first = false;
            i = j;
        } else {
            out += text[i];
            ++i;
        }
    }
    return out;
}

void PushLabel(float scale) {
    if (IsRefinedStyle()) {
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * scale * 1.08f);
    } else {
        PushMono(scale);
    }
}

void PopLabel() {
    if (IsRefinedStyle()) ImGui::PopFont();
    else PopMono();
}

static int g_monoDepth = 0;
static float g_monoBaseSize = 0.0f;

void PushMono(float scale) {
    ImFont* f = GetMonoFont();
    if (!f) return;
    // Size is taken from the surrounding UI font so nesting never compounds.
    if (g_monoDepth == 0) g_monoBaseSize = ImGui::GetFontSize();
    ++g_monoDepth;
    ImGui::PushFont(f, g_monoBaseSize * scale);
}

void PopMono() {
    if (!GetMonoFont()) return;
    --g_monoDepth;
    ImGui::PopFont();
}

void DrawLed(ImDrawList* dl, ImVec2 c, float r, bool on, const ImVec4* color) {
    const ImVec4 lit = color ? *color : GetAccent();
    if (on) {
        dl->AddCircleFilled(c, r + 2.5f, ImGui::GetColorU32(Alpha(lit, 0.16f)), 16);
    }
    dl->AddCircleFilled(c, r, ImGui::GetColorU32(on ? lit : GetLedOff()), 16);
    dl->AddCircle(c, r, ImGui::GetColorU32(GetKeyEdge()), 16, 1.0f);
}

static void PlainSectionHeader(const char* label) {
    if (IsRefinedStyle()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(StyleText(label).c_str());
        ImGui::Spacing();
        return;
    }
    ImGui::Spacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    PushMono();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    DrawLed(dl, ImVec2(p.x + 5.0f, p.y + h * 0.5f), 3.0f, true);
    dl->AddText(ImVec2(p.x + 16.0f, p.y),
                ImGui::GetColorU32(GetMutedText()), label);
    const float tw = ImGui::CalcTextSize(label).x;
    const float x0 = p.x + 16.0f + tw + 10.0f;
    const float x1 = p.x + ImGui::GetContentRegionAvail().x;
    if (x1 > x0) {
        dl->AddLine(ImVec2(x0, p.y + h * 0.5f), ImVec2(x1, p.y + h * 0.5f),
                    ImGui::GetColorU32(GetPanelEdge()), 1.0f);
    }
    ImGui::Dummy(ImVec2(0.0f, h));
    PopMono();
    ImGui::Spacing();
}

bool BeginRackPanel(const char* title) {
    const ThemeSettings& t = GetThemeSettings();
    const bool refined = t.style == 1;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, GetPanelBg());
    ImGui::PushStyleColor(ImGuiCol_Border, GetPanelEdge());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.cornerRadius + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, refined ? 0.0f : 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        refined ? ImVec2(16.0f * t.density, 12.0f * t.density)
                                : ImVec2(18.0f * t.density, 10.0f * t.density));
    const bool open = ImGui::BeginChild(
        title, ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
            ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();
    float titleH = 0.0f;
    if (refined) {
        const std::string text = StyleText(title);
        titleH = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(wp.x + 16.0f, wp.y + 12.0f), ImGui::GetColorU32(t.text), text.c_str());
        ImGui::SetCursorPosY(12.0f + titleH + 10.0f);
    } else {
        PushMono();
        const ImVec2 ts = ImGui::CalcTextSize(title);
        dl->AddText(ImVec2(wp.x + (width - ts.x) * 0.5f, wp.y + 8.0f),
                    ImGui::GetColorU32(GetMutedText()), title);
        titleH = ImGui::GetTextLineHeight();
        PopMono();
        ImGui::SetCursorPosY(8.0f + titleH + 6.0f);
    }
    return open;
}

void EndRackPanel() {
    if (!IsRefinedStyle()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const ImU32 edge = ImGui::GetColorU32(GetKeyEdge());
        const ImU32 rim = ImGui::GetColorU32(GetPanelEdge());
        const float in = 8.0f;
        const ImVec2 screws[4] = {
            ImVec2(wp.x + in, wp.y + in), ImVec2(wp.x + ws.x - in, wp.y + in),
            ImVec2(wp.x + in, wp.y + ws.y - in),
            ImVec2(wp.x + ws.x - in, wp.y + ws.y - in)};
        for (const ImVec2& s : screws) {
            dl->AddCircleFilled(s, 2.6f, edge, 10);
            dl->AddCircle(s, 2.6f, rim, 10, 1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
}

static ImGuiWindow* g_pageWindow = nullptr;
static ImGuiWindow* g_autoPanelWindow = nullptr;

void BeginAutoPanels() {
    g_pageWindow = ImGui::GetCurrentWindow();
    g_autoPanelWindow = nullptr;
}

void EndAutoPanels() {
    if (g_autoPanelWindow && ImGui::GetCurrentWindow() == g_autoPanelWindow) {
        EndRackPanel();
    }
    g_autoPanelWindow = nullptr;
    g_pageWindow = nullptr;
}

void SectionHeader(const char* label) {
    ImGuiWindow* cur = ImGui::GetCurrentWindow();
    if (g_autoPanelWindow && cur == g_autoPanelWindow) {
        EndRackPanel();
        g_autoPanelWindow = nullptr;
        cur = ImGui::GetCurrentWindow();
    }
    if (g_pageWindow && cur == g_pageWindow && ImGui::GetCurrentTable() == nullptr) {
        BeginRackPanel(label);
        g_autoPanelWindow = ImGui::GetCurrentWindow();
        return;
    }
    PlainSectionHeader(label);
}

void SettingLabel(const char* label, const char* help) {
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help && *help) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.0f);
        char sentence[160];
        size_t n = 0;
        for (; help[n] && n < sizeof(sentence) - 4; ++n) {
            sentence[n] = help[n];
            if (help[n] == '.' && (help[n + 1] == ' ' || help[n + 1] == '\0')) {
                ++n;
                break;
            }
        }
        sentence[n] = '\0';
        const float avail = ImGui::GetContentRegionAvail().x - 6.0f;
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.84f);
        bool cut = false;
        while (n > 4 && ImGui::CalcTextSize(sentence).x > avail) {
            sentence[--n] = '\0';
            cut = true;
        }
        if (cut) {
            while (n > 0 && sentence[n - 1] == ' ') sentence[--n] = '\0';
            sentence[n++] = '.';
            sentence[n++] = '.';
            sentence[n++] = '.';
            sentence[n] = '\0';
        }
        ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
        ImGui::TextUnformatted(sentence);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::EndGroup();
    if (help && *help && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool KeyGroup(const char* id, int* current, const char* const* labels,
              int count) {
    ImGui::PushID(id);
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& th = GetThemeSettings();

    if (th.style == 1) {
        // Refined: one rounded track, the chosen segment lifted.
        PushLabel(0.95f);
        const float segH = ImGui::GetTextLineHeight() + 12.0f;
        const float pad = 3.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        float total = pad;
        std::string texts[32];
        float widths[32];
        const int n = (std::min)(count, 32);
        for (int i = 0; i < n; ++i) {
            texts[i] = StyleText(labels[i]);
            widths[i] = ImGui::CalcTextSize(texts[i].c_str()).x + 26.0f;
            total += widths[i] + 2.0f;
        }
        total += pad - 2.0f;
        dl->AddRectFilled(origin, ImVec2(origin.x + total, origin.y + segH + pad * 2.0f),
                          ImGui::GetColorU32(GetLcdBg()), 9.0f);
        float x = origin.x + pad;
        for (int i = 0; i < n; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(x, origin.y + pad));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("seg", ImVec2(widths[i], segH)) && *current != i) {
                *current = i;
                changed = true;
            }
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = *current == i;
            if (on || hov) {
                dl->AddRectFilled(ImVec2(x, origin.y + pad),
                                  ImVec2(x + widths[i], origin.y + pad + segH),
                                  ImGui::GetColorU32(Mix(th.control, th.text, on ? 0.10f : 0.03f)),
                                  7.0f);
            }
            const ImVec2 ts = ImGui::CalcTextSize(texts[i].c_str());
            dl->AddText(ImVec2(x + (widths[i] - ts.x) * 0.5f,
                               origin.y + pad + (segH - ts.y) * 0.5f),
                        ImGui::GetColorU32(on ? th.text : th.mutedText), texts[i].c_str());
            x += widths[i] + 2.0f;
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(total, segH + pad * 2.0f));
        PopLabel();
        ImGui::PopID();
        return changed;
    }

    PushMono();
    const float lineH = ImGui::GetTextLineHeight();
    const float keyH = lineH + 20.0f;
    const float gap = 5.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float x = origin.x;
    for (int i = 0; i < count; ++i) {
        const float keyW = ImGui::CalcTextSize(labels[i]).x + 34.0f;
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("key", ImVec2(keyW, keyH)) && *current != i) {
            *current = i;
            changed = true;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = (*current == i);
        const float drop = on ? 2.0f : 0.0f;
        const float edgeH = on ? 1.0f : 3.0f;
        const ImVec4 face = Mix(th.control, th.text, on ? 0.07f : (hov ? 0.04f : 0.0f));
        const ImVec2 a(x, origin.y + drop);
        const float bottom = origin.y + keyH - (on ? 2.0f : 0.0f);
        dl->AddRectFilled(a, ImVec2(x + keyW, bottom),
                          ImGui::GetColorU32(GetKeyEdge()), th.cornerRadius - 1.0f);
        dl->AddRectFilled(a, ImVec2(x + keyW, bottom - edgeH),
                          ImGui::GetColorU32(face), th.cornerRadius - 1.0f);
        DrawLed(dl, ImVec2(x + keyW * 0.5f, a.y + 8.0f), 2.6f, on);
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(x + (keyW - ts.x) * 0.5f, a.y + 14.0f),
                    ImGui::GetColorU32(on ? th.text : th.mutedText), labels[i]);
        x += keyW + gap;
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(x - origin.x, keyH));
    PopMono();
    ImGui::PopID();
    return changed;
}

bool KeyButton(const char* label, const ImVec2& size, bool primary, bool enabled) {
    const ThemeSettings& th = GetThemeSettings();
    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##key", size) && enabled;
    const bool hov = ImGui::IsItemHovered() && enabled;
    const bool down = ImGui::IsItemActive() && enabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string shown = StyleText(label);
    const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    if (!enabled) {
        // Flat, recessed, dim: clearly not pressable.
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                          ImGui::GetColorU32(Mix(th.control, th.background, 0.55f)),
                          th.cornerRadius);
        dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y),
                    ImGui::GetColorU32(GetPanelEdge()), th.cornerRadius);
        dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(Alpha(th.mutedText, 0.45f)), shown.c_str());
        ImGui::PopID();
        return false;
    }
    if (th.style == 1) {
        const ImVec4 face = primary
            ? Mix(th.accent, th.background, down ? 0.25f : (hov ? 0.0f : 0.06f))
            : Mix(th.control, th.text, down ? 0.12f : (hov ? 0.07f : 0.0f));
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                          ImGui::GetColorU32(face), th.cornerRadius + 1.0f);
        const ImVec4 ink = primary ? ImVec4(0.07f, 0.06f, 0.04f, 1.0f) : th.text;
        dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(ink), shown.c_str());
        ImGui::PopID();
        return clicked;
    }
    const float edgeH = down ? 1.0f : 3.0f;
    const float drop = down ? 2.0f : 0.0f;
    const ImVec4 face = primary
        ? Mix(th.accent, th.background, down ? 0.25f : (hov ? 0.0f : 0.08f))
        : Mix(th.control, th.text, hov ? 0.06f : 0.0f);
    const float bottom = p.y + size.y - (down ? 2.0f : 0.0f);
    dl->AddRectFilled(ImVec2(p.x, p.y + drop), ImVec2(p.x + size.x, bottom),
                      ImGui::GetColorU32(GetKeyEdge()), th.cornerRadius);
    dl->AddRectFilled(ImVec2(p.x, p.y + drop),
                      ImVec2(p.x + size.x, bottom - edgeH),
                      ImGui::GetColorU32(face), th.cornerRadius);
    const ImVec4 ink = primary ? ImVec4(0.07f, 0.06f, 0.04f, 1.0f) : th.text;
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f,
                       p.y + drop + (size.y - edgeH - ts.y) * 0.5f),
                ImGui::GetColorU32(ink), shown.c_str());
    ImGui::PopID();
    return clicked;
}

void DrawLcdFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, int cols, int rows) {
    const ThemeSettings& th = GetThemeSettings();
    dl->AddRectFilled(min, max, ImGui::GetColorU32(GetLcdBg()), th.cornerRadius);
    const ImU32 grid = ImGui::GetColorU32(Alpha(th.accent, 0.10f));
    for (int i = 1; i < cols; ++i) {
        const float x = min.x + (max.x - min.x) * static_cast<float>(i) / cols;
        dl->AddLine(ImVec2(x, min.y + 2.0f), ImVec2(x, max.y - 2.0f), grid, 1.0f);
    }
    for (int i = 1; i < rows; ++i) {
        const float y = min.y + (max.y - min.y) * static_cast<float>(i) / rows;
        dl->AddLine(ImVec2(min.x + 2.0f, y), ImVec2(max.x - 2.0f, y), grid, 1.0f);
    }
    dl->AddRect(min, max, ImGui::GetColorU32(GetKeyEdge()), th.cornerRadius, 0, 1.5f);
}

void PanelCaption(const char* caption, const char* help, bool restart) {
    PushLabel(0.85f);
    ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
    ImGui::TextUnformatted(StyleText(caption).c_str());
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsItemHovered();
    PopLabel();
    if (restart) {
        ImGui::SameLine(0.0f, 8.0f);
        RestartPill();
    }
    if (hovered && help && *help) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool PanelKeys(const char* caption, int* current, const char* const* labels,
               int count, const char* help, bool restart) {
    ImGui::PushID(caption);
    PanelCaption(caption, help, restart);
    const bool changed = KeyGroup("##keys", current, labels, count);
    ImGui::PopID();
    return changed;
}

bool PanelLever(const char* caption, bool* value, const char* help, bool restart) {
    const bool changed = ToggleSwitch(caption, value, help);
    if (restart) {
        ImGui::SameLine(0.0f, 10.0f);
        RestartPill();
    }
    return changed;
}

bool PanelKnob(const char* label, float* value, float minValue, float maxValue,
               float defaultValue, const char* format, float size,
               const char* help, bool* committed, float (*displayFn)(float),
               float (*inverseFn)(float)) {
    const float startX = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (avail - size) * 0.5f));
    KnobState ks = {*value, minValue, maxValue, defaultValue,
                    label, nullptr, size, 1.0f, displayFn, inverseFn};
    const bool changed = RotaryKnob(ks, format);
    if (changed) *value = ks.value;
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    if (changed) st->SetInt(id, 1);
    bool released = false;
    if (!active && st->GetInt(id, 0) != 0) {
        released = true;
        st->SetInt(id, 0);
    }
    if (committed) *committed = released;
    if (hovered && !active) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        if (help && *help) {
            ImGui::TextUnformatted(help);
            ImGui::Spacing();
        }
        ImGui::TextDisabled("Ctrl+click to type a value  |  double-click to reset");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return changed;
}

bool PanelLcdInt(const char* caption, int* value, int minValue, int maxValue,
                 const char* help, bool restart, bool* committed,
                 const char* zeroText) {
    ImGui::PushID(caption);
    PanelCaption(caption, help, restart);
    ImGui::SetNextItemWidth(130.0f);
    PushMono();
    ImGui::PushStyleColor(ImGuiCol_Text, GetAccent());
    const bool edited = ImGui::InputInt("##lcdint", value, 0, 0);
    const bool done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleColor();
    PopMono();
    if (edited) *value = (std::max)(minValue, (std::min)(maxValue, *value));
    if (zeroText && *value == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", zeroText);
    }
    if (committed) *committed = done;
    ImGui::PopID();
    return edited;
}

void RestartPill() {
    PushLabel(0.78f);
    const bool refined = IsRefinedStyle();
    const std::string label = StyleText("RESTART");
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    const ImVec2 pad(refined ? 8.0f : 6.0f, 1.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f);
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (refined) {
        // A quiet chip: information, not a warning.
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                          ImGui::GetColorU32(Alpha(GetThemeSettings().text, 0.07f)),
                          size.y * 0.5f);
        dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y),
                    ImGui::GetColorU32(GetMutedText()), label.c_str());
    } else {
        // A tag, not a warning: follows the theme accent so it never clashes.
        const ImVec4 c = GetAccent();
        dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y),
                    ImGui::GetColorU32(Alpha(c, 0.55f)), 3.0f);
        dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(c), label.c_str());
    }
    PopLabel();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Requires driver restart to take effect.");
        ImGui::EndTooltip();
    }
}

void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool ToggleSwitch(const char* label, bool* value, const char* tooltip) {
    ImGui::PushID(label);
    bool changed = false;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = 42.0f;
    float h = 22.0f;
    float radius = h * 0.5f;

    ImGui::InvisibleButton("toggle", ImVec2(w, h));
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& th = GetThemeSettings();
    const bool hov = ImGui::IsItemHovered();
    if (IsRefinedStyle()) {
        // Flat pill toggle.
        const float tw = 38.0f;
        const float th2 = 20.0f;
        const ImVec2 q(pos.x, pos.y + (h - th2) * 0.5f);
        dl->AddRectFilled(q, ImVec2(q.x + tw, q.y + th2),
                          ImGui::GetColorU32(*value ? GetAccent() : Mix(th.control, th.text, hov ? 0.10f : 0.04f)),
                          th2 * 0.5f);
        const float cx = *value ? q.x + tw - th2 * 0.5f : q.x + th2 * 0.5f;
        dl->AddCircleFilled(ImVec2(cx, q.y + th2 * 0.5f), th2 * 0.5f - 3.0f,
                            ImGui::GetColorU32(*value ? ImVec4(0.07f, 0.06f, 0.04f, 1.0f) : th.mutedText));
    } else {
    // Slot with a sliding lever; the lever lights up when on.
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                      ImGui::GetColorU32(GetKeyEdge()), 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h),
                ImGui::GetColorU32(GetPanelEdge()), 4.0f);
    const float leverW = w * 0.5f - 3.0f;
    const float lx = *value ? pos.x + w - leverW - 3.0f : pos.x + 3.0f;
    const ImVec4 lever = *value
        ? GetAccent()
        : Mix(th.control, th.text, hov ? 0.12f : 0.04f);
    dl->AddRectFilled(ImVec2(lx, pos.y + 3.0f),
                      ImVec2(lx + leverW, pos.y + h - 3.0f),
                      ImGui::GetColorU32(lever), 3.0f);
    }
    (void)radius;

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
    ImGui::TextUnformatted(StyleText(label).c_str());

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool LabeledFloat(const char* label, float* value, float min, float max,
                  const char* format, const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float inputWidth = ImMin(avail - labelWidth - 8.0f, 160.0f);

    ImGui::PushItemWidth(inputWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - inputWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + inputWidth);

    if (ImGui::InputFloat("##val", value, 0.0f, 0.0f, format)) {
        if (*value < min) *value = min;
        if (*value > max) *value = max;
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool LabeledInt(const char* label, int* value, int min, int max,
                const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float inputWidth = ImMin(avail - labelWidth - 8.0f, 160.0f);

    ImGui::PushItemWidth(inputWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - inputWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + inputWidth);

    if (ImGui::InputInt("##val", value, 0, 0)) {
        if (*value < min) *value = min;
        if (*value > max) *value = max;
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool LabeledUInt(const char* label, unsigned int* value, unsigned int min,
                 unsigned int max, const char* tooltip) {
    int signedVal = static_cast<int>(*value);
    bool changed = LabeledInt(label, &signedVal, static_cast<int>(min),
                              static_cast<int>(max), tooltip);
    if (changed) *value = static_cast<unsigned int>(signedVal);
    return changed;
}

bool LabeledCombo(const char* label, int* current, const char* const* items,
                  int itemCount, const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float comboWidth = ImMin(avail - labelWidth - 8.0f, 240.0f);

    ImGui::PushItemWidth(comboWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - comboWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + comboWidth);

    if (ImGui::Combo("##val", current, items, itemCount)) {
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool SliderFloat(const char* label, float* value, float min, float max,
                 const char* format, const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float sliderWidth = ImMin(avail - labelWidth - 8.0f, 300.0f);

    ImGui::PushItemWidth(sliderWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - sliderWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + sliderWidth);

    if (ImGui::SliderFloat("##val", value, min, max, format)) {
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

bool SliderInt(const char* label, int* value, int min, int max,
               const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    float labelWidth = ImGui::CalcTextSize(label).x + 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float sliderWidth = ImMin(avail - labelWidth - 8.0f, 300.0f);

    ImGui::PushItemWidth(sliderWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelWidth - sliderWidth);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - avail + labelWidth + sliderWidth);

    if (ImGui::SliderInt("##val", value, min, max)) {
        changed = true;
    }
    ImGui::PopItemWidth();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }

    ImGui::PopID();
    return changed;
}

void StatusBar(const char* text, bool modified) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

    if (modified) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::Text("Configuration modified");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
}

void ToastNotification(const char* message, float durationSeconds) {
    snprintf(g_toastText, sizeof(g_toastText), "%s", message);
    g_toastTimer = durationSeconds;
    g_toastActive = true;
}

void PushToastStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        (std::max)(6.0f, GetThemeSettings().cornerRadius));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));
    ImVec4 bg = GetPanelBg();
    bg.w = 0.97f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, GetAccentDim());
}

void PopToastStyle() {
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

bool BeginToast(const char* id) {
    if (!g_toastActive) return false;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos(io.DisplaySize.x - 20.0f, io.DisplaySize.y - 20.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);

    PushToastStyle();
    bool visible = ImGui::Begin(id, nullptr,
                                ImGuiWindowFlags_NoDecoration |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoFocusOnAppearing);
    return visible;
}

void EndToast() {
    ImGui::End();
    PopToastStyle();
}

bool BeginToastOverlay() {
    if (!g_toastActive) return false;

    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    g_toastTimer -= dt;
    if (g_toastTimer <= 0.0f) {
        g_toastActive = false;
        return false;
    }

    ImVec2 pos(io.DisplaySize.x - 20.0f, io.DisplaySize.y - 20.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);

    PushToastStyle();
    bool visible = ImGui::Begin("##toast", nullptr,
                                ImGuiWindowFlags_NoDecoration |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoFocusOnAppearing);
    return visible;
}

void EndToastOverlay() {
    ImGui::End();
    PopToastStyle();
}

bool RotaryKnob(KnobState& state, const char* format) {
    ImGui::PushID(state.label);

    bool changed = false;
    float size = state.size > 0 ? state.size : 58.0f;
    float radius = size * 0.42f;

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 center(cursor.x + size * 0.5f, cursor.y + radius + 8.0f);
    ImVec2 totalSize(size, size + 24.0f);

    ImGui::InvisibleButton("knob", totalSize);

    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    // Ctrl+click opens a typed-value field over the readout.
    ImGuiStorage* knobStore = ImGui::GetStateStorage();
    const ImGuiID editId = ImGui::GetID("##knob_edit");
    const ImGuiID initId = ImGui::GetID("##knob_edit_init");
    bool editing = knobStore->GetInt(editId, 0) != 0;
    if (!editing && ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
        ImGui::GetIO().KeyCtrl) {
        editing = true;
        knobStore->SetInt(editId, 1);
        knobStore->SetInt(initId, 0);
    }

    float t = (state.value - state.minValue) / (state.maxValue - state.minValue);
    t = ImClamp(t, 0.0f, 1.0f);

    const float startAngle = 0.75f * 3.14159265f;
    const float sweep = 1.5f * 3.14159265f;
    const float angle = startAngle + t * sweep;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& theme = GetThemeSettings();

    // Scale ticks around the knob (Rack style only).
    const ImU32 tickCol = ImGui::GetColorU32(Alpha(theme.mutedText, 0.55f));
    for (int i = 0; theme.style == 0 && i <= 10; ++i) {
        const float a = startAngle + sweep * (static_cast<float>(i) / 10.0f);
        dl->AddLine(ImVec2(center.x + std::cos(a) * (radius + 3.0f),
                           center.y + std::sin(a) * (radius + 3.0f)),
                    ImVec2(center.x + std::cos(a) * (radius + 6.0f),
                           center.y + std::sin(a) * (radius + 6.0f)),
                    tickCol, 1.0f);
    }

    // Value arc.
    dl->PathArcTo(center, radius - 1.0f, startAngle, startAngle + sweep, 32);
    dl->PathStroke(ImGui::GetColorU32(GetKeyEdge()), 0, 3.0f);
    if (t > 0.005f) {
        dl->PathArcTo(center, radius - 1.0f, startAngle, angle, 32);
        dl->PathStroke(ImGui::GetColorU32(theme.accent), 0, 3.0f);
    }

    // Cap.
    const float capR = radius - 7.0f;
    dl->AddCircleFilled(center, capR + 2.0f, ImGui::GetColorU32(GetKeyEdge()), 32);
    const ImVec4 cap = Mix(theme.control, theme.text,
                           active ? 0.10f : (hovered ? 0.05f : 0.0f));
    dl->AddCircleFilled(center, capR, ImGui::GetColorU32(cap), 32);
    dl->AddLine(ImVec2(center.x + std::cos(angle) * capR * 0.30f,
                       center.y + std::sin(angle) * capR * 0.30f),
                ImVec2(center.x + std::cos(angle) * capR * 0.92f,
                       center.y + std::sin(angle) * capR * 0.92f),
                ImGui::GetColorU32(theme.text), 2.2f);

    PushLabel();
    char valueBuf[64];
    float displayVal = state.displayFn ? state.displayFn(state.value)
                                       : state.value * state.displayScale;
    snprintf(valueBuf, sizeof(valueBuf), format, displayVal);
    ImVec2 textSize = ImGui::CalcTextSize(valueBuf);
    if (!editing) {
        dl->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 7.0f),
                    ImGui::GetColorU32(theme.accent), valueBuf);
    }

    char labelBuf[128];
    snprintf(labelBuf, sizeof(labelBuf), "%s", state.label);
    if (theme.style == 0) {
        for (char* c = labelBuf; *c; ++c)
            if (*c >= 'a' && *c <= 'z') *c = static_cast<char>(*c - 32);
    } else {
        const std::string styled = StyleText(labelBuf);
        std::snprintf(labelBuf, sizeof(labelBuf), "%s", styled.c_str());
    }
    ImVec2 labelText = ImGui::CalcTextSize(labelBuf);
    dl->AddText(ImVec2(center.x - labelText.x * 0.5f, center.y + radius + 7.0f + textSize.y + 1.0f),
                ImGui::GetColorU32(theme.mutedText), labelBuf);
    PopLabel();

    if (editing) {
        static char editBuf[48];
        static char editInit[48];
        const bool first = knobStore->GetInt(initId, 0) == 0;
        if (first) {
            std::snprintf(editBuf, sizeof(editBuf), "%.4g", displayVal);
            std::memcpy(editInit, editBuf, sizeof(editInit));
        }
        const ImVec2 saved = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(center.x - size * 0.5f - 8.0f,
                                         center.y + radius + 4.0f));
        ImGui::SetNextItemWidth(size + 16.0f);
        if (first) {
            ImGui::SetKeyboardFocusHere();
            knobStore->SetInt(initId, 1);
        }
        PushMono();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.accent);
        ImGui::InputText("##knob_edit_text", editBuf, sizeof(editBuf),
                         ImGuiInputTextFlags_AutoSelectAll |
                         ImGuiInputTextFlags_EnterReturnsTrue |
                         ImGuiInputTextFlags_CharsDecimal);
        const bool done = ImGui::IsItemDeactivated();
        ImGui::PopStyleColor();
        PopMono();
        ImGui::SetCursorScreenPos(saved);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        if (done) {
            knobStore->SetInt(editId, 0);
            knobStore->SetInt(initId, 0);
            if (std::strcmp(editBuf, editInit) != 0) {
                const float typed = static_cast<float>(std::atof(editBuf));
                float v = state.inverseFn
                    ? state.inverseFn(typed)
                    : (state.displayFn ? typed
                                       : typed / (state.displayScale != 0.0f
                                                      ? state.displayScale : 1.0f));
                v = ImClamp(v, state.minValue, state.maxValue);
                if (v != state.value) {
                    state.value = v;
                    changed = true;
                }
            }
        }
    }

    if (active) {
        float delta = ImGui::GetIO().MouseDelta.y;
        float range = state.maxValue - state.minValue;
        float step = range * 0.002f;
        if (ImGui::GetIO().KeyShift) step *= 0.1f;
        float newVal = state.value - delta * step;
        newVal = ImClamp(newVal, state.minValue, state.maxValue);
        if (newVal != state.value) {
            state.value = newVal;
            changed = true;
        }
    } else if (hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float range = state.maxValue - state.minValue;
            float step = range * 0.02f;
            if (ImGui::GetIO().KeyShift) step *= 0.1f;
            float newVal = state.value + wheel * step;
            newVal = ImClamp(newVal, state.minValue, state.maxValue);
            if (newVal != state.value) {
                state.value = newVal;
                changed = true;
            }
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle) ||
        (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        state.value = state.defaultValue;
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

// LED ladder: lit segments green -> amber -> red, with a peak-hold segment.
void DrawLedLadder(ImDrawList* dl, ImVec2 pos, ImVec2 size, float value,
                   float peak, bool fromTop, const ImVec4* single) {
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      ImGui::GetColorU32(GetLcdBg()), 3.0f);
    const float segH = 4.0f;
    const float pitch = 6.0f;
    const int n = static_cast<int>((size.y - 4.0f) / pitch);
    const float used = n * pitch - (pitch - segH);
    const float pad = (size.y - used) * 0.5f;
    const int lit = static_cast<int>(ImClamp(value, 0.0f, 1.0f) * n + 0.5f);
    const int pk = peak > 0.01f ? static_cast<int>(ImClamp(peak, 0.0f, 1.0f) * n) : -1;
    for (int i = 0; i < n; ++i) {
        const float frac = static_cast<float>(i) / static_cast<float>(n);
        ImVec4 col = single ? *single
                            : (frac < 0.62f ? GetSuccess()
                                            : (frac < 0.82f ? GetWarning() : GetError()));
        const bool on = i < lit || i == pk - 0;
        const ImVec4 c = on ? col : Alpha(col, single ? 0.07f : 0.16f);
        const float y = fromTop ? pos.y + pad + i * pitch
                                : pos.y + size.y - pad - segH - i * pitch;
        dl->AddRectFilled(ImVec2(pos.x + 3.0f, y),
                          ImVec2(pos.x + size.x - 3.0f, y + segH),
                          ImGui::GetColorU32(c), 1.0f);
    }
}

void DrawVerticalMeter(const char* /*id*/, float value, float peak,
                       const ImVec2& size, bool showScale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    DrawLedLadder(dl, pos, size, value, peak, false, nullptr);

    if (showScale) {
        PushMono();
        auto drawTick = [&](float db, const char* text) {
            float frac = (db + 48.0f) / 48.0f;
            float y = pos.y + size.y - size.y * frac;
            dl->AddText(ImVec2(pos.x + size.x + 4.0f, y - 6.0f),
                        ImGui::GetColorU32(Alpha(GetMutedText(), 0.85f)), text);
        };
        drawTick(0.0f, "0");
        drawTick(-3.0f, "-3");
        drawTick(-6.0f, "-6");
        drawTick(-12.0f, "-12");
        drawTick(-24.0f, "-24");
        drawTick(-48.0f, "-48");
        PopMono();
    }

    ImGui::Dummy(ImVec2(size.x + (showScale ? 34.0f : 0.0f), size.y));
}

void DrawGainReductionMeter(const char* /*id*/, float gr,
                            const ImVec2& size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec4 amber = GetAccent();
    DrawLedLadder(dl, pos, size, ImClamp(fabsf(gr) / 24.0f, 0.0f, 1.0f), 0.0f,
                  true, &amber);

    PushMono();
    auto drawTick = [&](float db, const char* text) {
        float y = pos.y + size.y * (db / 24.0f);
        dl->AddText(ImVec2(pos.x + size.x + 4.0f, y - 6.0f),
                    ImGui::GetColorU32(Alpha(GetMutedText(), 0.85f)), text);
    };
    drawTick(0.0f, "0");
    drawTick(-3.0f, "-3");
    drawTick(-6.0f, "-6");
    drawTick(-12.0f, "-12");
    drawTick(-24.0f, "-24");

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f dB", gr);
    dl->AddText(ImVec2(pos.x + 2.0f, pos.y + size.y + 3.0f),
                ImGui::GetColorU32(GetAccent()), buf);
    PopMono();

    ImGui::Dummy(ImVec2(size.x + 34.0f, size.y + 18.0f));
}

void DrawReverbVisualizer(ImDrawList* dl, ImVec2 center, float radius,
                           float roomSize, float decay, float diffusion,
                           float width, float modDepth, float time) {
    int ringCount = 6 + static_cast<int>(roomSize * 10.0f);
    if (ringCount > 18) ringCount = 18;

    for (int i = 0; i < ringCount; ++i) {
        float t = static_cast<float>(i) / ringCount;
        float r = radius * (0.15f + t * 0.85f) * roomSize;
        float alpha = (1.0f - t * 0.7f) * decay;
        float wobble = std::sin(time * 1.5f + i * 0.7f) * modDepth * 4.0f * t;

        ImVec4 ring = GetAccent();
        ring.w = alpha * 0.35f;
        ImU32 col = ImGui::GetColorU32(ring);

        int segments = 32;
        for (int j = 0; j < segments; ++j) {
            float a0 = (static_cast<float>(j) / segments) * 6.28318f;
            float a1 = (static_cast<float>(j + 1) / segments) * 6.28318f;

            float spread = 1.0f + std::sin(a0 * 2.0f) * (1.0f - diffusion) * 0.3f;
            float spread2 = 1.0f + std::sin(a1 * 2.0f) * (1.0f - diffusion) * 0.3f;

            float xw = width;
            ImVec2 p0(center.x + std::cos(a0) * r * spread * xw + wobble,
                      center.y + std::sin(a0) * r * spread + wobble * 0.5f);
            ImVec2 p1(center.x + std::cos(a1) * r * spread2 * xw + wobble * 0.8f,
                      center.y + std::sin(a1) * r * spread2 + wobble * 0.3f);
            dl->AddLine(p0, p1, col, 1.0f);
        }
    }

    int cloudPoints = static_cast<int>(diffusion * 60.0f) + 10;
    for (int i = 0; i < cloudPoints; ++i) {
        float angle = static_cast<float>(i) / cloudPoints * 6.28318f;
        float dist = radius * (0.1f + fmodf(static_cast<float>(i * 7 + 3),
                                             17.0f) / 17.0f * 0.6f * roomSize);
        float modOffset = std::sin(time * 0.8f + angle * 3.0f) * modDepth * dist * 0.15f;
        ImVec2 pt(center.x + std::cos(angle) * (dist + modOffset) * width,
                  center.y + std::sin(angle) * (dist + modOffset));
        ImVec4 dot = GetAccent();
        dot.w = 0.15f + decay * 0.15f;
        ImU32 dotCol = ImGui::GetColorU32(dot);
        dl->AddCircleFilled(pt, 1.5f, dotCol, 6);
    }
}

bool LiveAppliedMatches(const svms::RuntimeLinkTelemetryV2& telemetry,
                        const ConfigValues& working) {
    const svms::RuntimeLiveStateV2& e = telemetry.live;
    const auto closeEnough = [](float a, float b) {
        const float d = a - b;
        return d > -1e-4f && d < 1e-4f;
    };
    if (e.correctnessMode != (working.correctnessMode ? 1u : 0u)) return false;
    if (e.reverbEnabled != (working.enableReverb ? 1u : 0u)) return false;
    if (e.limiterEnabled != (working.limiterEnabled ? 1u : 0u)) return false;
    if (e.limiterAlgorithm != working.limiterAlgorithm) return false;
    if (e.maxVoices != 0u && e.maxVoices != working.maxVoices) return false;
    if (!closeEnough(e.masterVolume, working.masterVolume)) return false;
    if (!closeEnough(e.reverbMix, working.reverbMix)) return false;
    if (!closeEnough(e.reverbRoomSize, working.reverbRoomSize)) return false;
    if (!closeEnough(e.reverbDecay, working.reverbDecay)) return false;
    if (!closeEnough(e.reverbDamping, working.reverbDamping)) return false;
    if (!closeEnough(e.reverbWidth, working.reverbWidth)) return false;
    if (!closeEnough(e.reverbDiffusion, working.reverbDiffusion)) return false;
    if (!closeEnough(e.reverbPreDelayMs, working.reverbPreDelayMs)) return false;
    if (!closeEnough(e.reverbEarlyLevel, working.reverbEarlyLevel)) return false;
    if (!closeEnough(e.reverbLateLevel, working.reverbLateLevel)) return false;
    if (!closeEnough(e.reverbModDepth, working.reverbModDepth)) return false;
    if (!closeEnough(e.reverbModRate, working.reverbModRate)) return false;
    if (!closeEnough(e.reverbLowCutHz, working.reverbLowCutHz)) return false;
    if (!closeEnough(e.reverbHighCutHz, working.reverbHighCutHz)) return false;
    if (!closeEnough(e.limiterThreshold, working.limiterThreshold)) return false;
    if (!closeEnough(e.limiterLookaheadMs, working.limiterLookaheadMs)) return false;
    if (!closeEnough(e.limiterAttackMs, working.limiterAttackMs)) return false;
    if (!closeEnough(e.limiterReleaseMs, working.limiterReleaseMs)) return false;
    return true;
}

void AppliedStateBadge(bool connected,
                       const svms::RuntimeLinkTelemetryV2* telemetry,
                       const ConfigValues& working, const char* scopeTooltip) {
    ImGui::SameLine();
    bool synced = connected && telemetry && LiveAppliedMatches(*telemetry, working);
    if (connected && telemetry && !synced) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::TextDisabled("PENDING");
    } else if (connected && telemetry) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetSuccess());
        ImGui::TextDisabled("APPLIED");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, GetMutedText());
        ImGui::TextDisabled("OFFLINE");
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(scopeTooltip ? scopeTooltip
            : "Engine applied state vs working copy");
        if (connected && telemetry && !synced) {
            ImGui::TextUnformatted("Working values differ from the engine's");
            ImGui::TextUnformatted("applied echo — awaiting the next live flush.");
        }
        ImGui::EndTooltip();
    }
}

void LiveBadge(const char* tooltip) {
    const char* label = "LIVE";

    ImGui::PushStyleColor(ImGuiCol_Text, GetSuccess());
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
}

void RestartRequiredBadge() {
    ImGui::SameLine();
    RestartPill();
}

} // namespace svms::cfg
