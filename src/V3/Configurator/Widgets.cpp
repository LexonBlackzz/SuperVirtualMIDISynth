#include "Widgets.h"
#include "ConfiguratorApp.h"
#include "ConfigDocument.h"
#include "Theme.h"
#include "ModernWidgets.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "DriverLink.h"
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

void NoteLiveChange() {
    if (g_liveLink.app) g_liveLink.app->NoteLiveChange();
}

void SendLiveCommand(svms::RLCommandType type, uint32_t param) {
    if (!g_liveLink.connected || !g_liveLink.client) return;
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    g_liveLink.client->SendCommand(type, 0u, param, svms::RuntimeLiveStateV2{}, 100u, result);
    NoteLiveChange();
}


void PushLabel(float scale) { PushMono(scale); }

void PopLabel() { PopMono(); }

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

bool BeginRackPanel(const char* title) {
    const ThemeSettings& t = GetThemeSettings();
    ui::BeginEntry();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, GetPanelBg());
    ImGui::PushStyleColor(ImGuiCol_Border, GetPanelEdge());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.cornerRadius + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(18.0f * t.density, 10.0f * t.density));
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
    PushMono();
    const ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(wp.x + (width - ts.x) * 0.5f, wp.y + 8.0f),
                ImGui::GetColorU32(GetMutedText()), title);
    const float titleH = ImGui::GetTextLineHeight();
    PopMono();
    ImGui::SetCursorPosY(8.0f + titleH + 6.0f);
    return open;
}

void EndRackPanel() {
    {
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
    ui::EndEntry();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
}


bool KeyGroup(const char* id, int* current, const char* const* labels,
              int count) {
    ImGui::PushID(id);
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& th = GetThemeSettings();

    PushMono();
    const float lineH = ImGui::GetTextLineHeight();
    const float keyH = lineH + 22.0f;
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
        DrawLed(dl, ImVec2(x + keyW * 0.5f, a.y + 7.0f), 2.6f, on);
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(x + (keyW - ts.x) * 0.5f, a.y + 13.0f),
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
    const ImVec2 ts = ImGui::CalcTextSize(label);
    if (!enabled) {
        // Flat, recessed, dim: clearly not pressable.
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                          ImGui::GetColorU32(Mix(th.control, th.background, 0.55f)),
                          th.cornerRadius);
        dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y),
                    ImGui::GetColorU32(GetPanelEdge()), th.cornerRadius);
        dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(Alpha(th.mutedText, 0.45f)), label);
        ImGui::PopID();
        return false;
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
                ImGui::GetColorU32(ink), label);
    ImGui::PopID();
    return clicked;
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





void DrawRackLever(ImDrawList* dl, ImVec2 pos, ImVec2 size, bool on, bool hovered) {
    // Slot with a sliding lever; the lever lights up when on.
    const ThemeSettings& th = GetThemeSettings();
    const float w = size.x, h = size.y;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                      ImGui::GetColorU32(GetKeyEdge()), 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h),
                ImGui::GetColorU32(GetPanelEdge()), 4.0f);
    const float leverW = w * 0.5f - 3.0f;
    const float lx = on ? pos.x + w - leverW - 3.0f : pos.x + 3.0f;
    const ImVec4 lever = on ? GetAccent() : Mix(th.control, th.text, hovered ? 0.12f : 0.04f);
    dl->AddRectFilled(ImVec2(lx, pos.y + 3.0f), ImVec2(lx + leverW, pos.y + h - 3.0f),
                      ImGui::GetColorU32(lever), 3.0f);
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
    for (int i = 0; theme.style == kStyleRack && i <= 10; ++i) {
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
    for (char* c = labelBuf; *c; ++c)
        if (*c >= 'a' && *c <= 'z') *c = static_cast<char>(*c - 32);
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








} // namespace svms::cfg
