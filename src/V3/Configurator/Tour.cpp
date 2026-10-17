#include "Tour.h"
#include "Anim.h"
#include "ConfiguratorApp.h"
#include "ModernWidgets.h"
#include "SettingsRegistry.h"
#include "Theme.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace svms::cfg::tour {
namespace {

struct Step {
    const char* key;
    int page;   // NavPage to open first, -1 = stay
    const char* title;
    const char* text;
};

// Copy is the prototype's TOUR table, verbatim.
const Step kSteps[] = {
    {"t-search", -1, "Find any setting",
     "Type what you are after, like \xE2\x80\x9C" "latency\xE2\x80\x9D" " or \xE2\x80\x9C" "reverb\xE2\x80\x9D" ". Names from the previous version work too, such as "
     "\xE2\x80\x9C" "Buffer frames\xE2\x80\x9D" " or \xE2\x80\x9C" "Hybrid thread affinity\xE2\x80\x9D" "."},
    {"t-nav", -1, "Sections by goal",
     "Sound is what plays. Effects is the chain after the synth. Output is your speakers and latency. Performance is how "
     "much SVMS can carry. MIDI is keyboards and overload handling."},
    {"t-meters", -1, "Always-on status",
     "Voices, render load and output level stay visible on every page. Master changes the volume of everything."},
    {"profile", static_cast<int>(NavPage::Performance), "Start with a profile",
     "Accurate, Balanced or Extreme sets all the CPU shortcuts at once. You can still flip any single one further down."},
    {"buf", static_cast<int>(NavPage::Output), "Live, or after a restart",
     "Most changes are heard instantly while music plays. Anything marked \xE2\x86\xBB applies the next time an app opens SVMS. "
     "Unsaved changes collect in a bar at the bottom until you Save."},
    {"t-tools", -1, "Tools",
     "Render a MIDI to WAV, record what you hear, and copy a diagnostics report for bug reports."},
    {"t-settings", -1, "Make it yours",
     "Like the look of hardware gear? Switch to the Rack theme in Settings. You can also pick any accent colour, tone the "
     "animations down, or replay this tour."},
};
constexpr int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

int g_step = -1;      // -1 closed, 0 welcome, 1..kStepCount spotlight
float g_age = 0.0f;   // seconds since the tour started (drives the entry animations)
int g_run = 0;
ImVec2 g_min[kStepCount], g_max[kStepCount];
bool g_have[kStepCount] = {};

float Scl() { return ui::Scale(); }

void Go(ConfiguratorApp& app, int n) {
    if (n < 0 || n > kStepCount) {
        const bool finished = n > kStepCount;
        g_step = -1;
        if (finished) {
            app.Navigate(NavPage::Home);
            app.ShowToast("Replay the tour any time from Settings");
        }
        return;
    }
    if (n >= 1 && kSteps[n - 1].page >= 0) app.Navigate(static_cast<NavPage>(kSteps[n - 1].page));
    g_step = n;
}

// Dims everything outside a rounded hole: four strips plus four corner pieces, so no pixel is dimmed twice.
void DimAround(ImDrawList* dl, float W, float H, float hx, float hy, float hw, float hh, float r, ImU32 col) {
    hx = (std::max)(0.0f, hx);
    hy = (std::max)(0.0f, hy);
    hw = (std::min)(hw, W - hx);
    hh = (std::min)(hh, H - hy);
    r = (std::min)(r, (std::min)(hw, hh) * 0.5f);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, hy), col);
    dl->AddRectFilled(ImVec2(0, hy + hh), ImVec2(W, H), col);
    dl->AddRectFilled(ImVec2(0, hy), ImVec2(hx, hy + hh), col);
    dl->AddRectFilled(ImVec2(hx + hw, hy), ImVec2(W, hy + hh), col);
    const float cx[4] = {hx, hx + hw, hx, hx + hw}, cy[4] = {hy, hy, hy + hh, hy + hh};
    const float dx[4] = {1, -1, 1, -1}, dy[4] = {1, 1, -1, -1};
    for (int i = 0; i < 4; ++i) {
        const ImVec2 centre(cx[i] + dx[i] * r, cy[i] + dy[i] * r);
        const ImVec2 va(-dx[i], 0.0f), vb(0.0f, -dy[i]);   // centre -> the two edge points
        dl->PathLineTo(ImVec2(cx[i], cy[i]));
        dl->PathLineTo(ImVec2(centre.x + va.x * r, centre.y));
        for (int j = 1; j < 10; ++j) {
            const float t = j / 10.0f;
            ImVec2 d(va.x + (vb.x - va.x) * t, va.y + (vb.y - va.y) * t);
            const float len = std::sqrt(d.x * d.x + d.y * d.y);
            dl->PathLineTo(ImVec2(centre.x + d.x / len * r, centre.y + d.y / len * r));
        }
        dl->PathLineTo(ImVec2(centre.x, centre.y + vb.y * r));
        dl->PathFillConcave(col);
    }
}

float WrapH(const char* text, float wrapW) { return ImGui::CalcTextSize(text, nullptr, false, wrapW).y; }

ImU32 Fade(ImU32 col, float a) { return ui::WithAlpha(col, a); }

void PersistShowAtStart() {
    std::string error;
    const bool portable = std::string(GetThemeSourceName()).rfind("Portable", 0) == 0;
    SaveTheme(portable ? ThemeStorage::Portable : ThemeStorage::AppData, &error);
}

// --- Welcome card ----------------------------------------------------------------------------------

void DrawWelcome(ConfiguratorApp& app, ImDrawList* dl, float W, float H) {
    const ui::Tokens& k = ui::Tok();
    const float s = Scl();
    const double now = ImGui::GetTime();
    const float scrim = AnimEase(ImGui::GetID("##scrim"), g_age > 0.01f ? 1.0f : 0.0f, 0.4f);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(6, 6, 9, static_cast<int>(0.72f * scrim * 255.0f)));

    const float e = AnimSpring(ImGui::GetID("##welIn"), g_age > 0.01f ? 1.0f : 0.0f);
    const float a = ImClamp(e, 0.0f, 1.0f);
    const float cw = 560.0f * s, padX = 38.0f * s;
    const float inner = cw - 2.0f * padX;

    static const char* kIcons[3] = {
        "M18 11a7 7 0 1 0-14 0 7 7 0 0 0 14 0zM20 20l-3.5-3.5",
        "M13 2L3 14h9l-1 8 10-12h-9l1-8z",
        "M3 12a9 9 0 1 0 3-6.7L3 8M3 3v5h5"};
    static const char* kTipTitle[3] = {"Lost? Search.", "Most changes are instant.", "Like hardware gear?"};
    static const char* kTipText[3] = {"Names from the previous version work too, like \xE2\x80\x9C" "Buffer frames\xE2\x80\x9D" ".",
                                      "You hear them while the music plays.", "Try the Rack theme in Settings."};
    const char* intro = "Same SVMS, same settings. Everything from the old configurator is still here, just grouped by what you "
                        "want to do instead of how the engine works inside.";

    // Measure first so the card can be drawn behind its content.
    ui::PushText(0, 14.0f);
    const float introH = WrapH(intro, inner);
    ui::PopFont();
    ui::PushText(2, 13.0f);
    const float tipTitleH = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushText(0, 13.0f);
    const float tipTextH = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float tipH = 12.0f * s * 2 + tipTitleH + 2.0f * s + tipTextH;
    ui::PushText(2, 24.0f);
    const float h2H = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float btnH = 38.0f * s;
    const float ch = 36.0f * s + 34.0f * s + 20.0f * s + h2H + 8.0f * s + introH + 20.0f * s + 3 * tipH + 2 * 12.0f * s +
                     26.0f * s + btnH + 28.0f * s;
    const ImVec2 p0(std::floor((W - cw) * 0.5f), std::floor((H - ch) * 0.5f + (1.0f - e) * 24.0f * s));
    const ImVec2 p1(p0.x + cw, p0.y + ch);
    ui::SoftShadow(dl, p0, p1, 20.0f * s, Fade(IM_COL32(0, 0, 0, 255), 0.45f * a), 30.0f * s, 8);
    dl->AddRectFilled(p0, p1, Fade(k.card, a), 20.0f * s);
    dl->AddRect(p0, p1, Fade(k.line2, a), 20.0f * s);

    float y = p0.y + 36.0f * s;
    const float x = p0.x + padX;
    // Animated EQ logo.
    for (int i = 0; i < 4; ++i) {
        static const float kPh[4] = {0.0f, -0.3f, -0.6f, -0.15f};
        float wave = 0.5f;
        if (GetMotion() != Motion::Off) {
            const float ph = static_cast<float>(std::fmod(now - kPh[i] + 10.0, 1.1) / 1.1);
            wave = 0.3f + 0.7f * (0.5f - 0.5f * std::cos(6.2831853f * ph));
        }
        const float h = wave * 34.0f * s;
        const float bx = x + i * 11.0f * s;
        dl->AddRectFilled(ImVec2(bx, y + 34.0f * s - h), ImVec2(bx + 7.0f * s, y + 34.0f * s), Fade(k.acc, a), 3.0f * s);
    }
    y += 34.0f * s + 20.0f * s;
    ui::PushText(2, 24.0f);
    dl->AddText(ImVec2(x, y), Fade(k.text, a), "Welcome to the new configurator");
    ui::PopFont();
    y += h2H + 8.0f * s;
    ui::PushText(0, 14.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), Fade(k.muted, a), intro, nullptr, inner);
    ui::PopFont();
    y += introH + 20.0f * s;
    for (int i = 0; i < 3; ++i) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + inner, y + tipH), Fade(k.card2, a), 12.0f * s);
        ui::StrokeIcon(dl, kIcons[i], ImVec2(x + 14.0f * s, y + (tipH - 18.0f * s) * 0.5f), 18.0f * s, Fade(k.acc, a),
                       2.0f * s);
        ui::PushText(2, 13.0f);
        dl->AddText(ImVec2(x + 50.0f * s, y + 12.0f * s), Fade(k.text, a), kTipTitle[i]);
        ui::PopFont();
        ui::PushText(0, 13.0f);
        dl->AddText(ImVec2(x + 50.0f * s, y + 12.0f * s + tipTitleH + 2.0f * s), Fade(k.muted, a), kTipText[i]);
        ui::PopFont();
        y += tipH + 12.0f * s;
    }
    y += 26.0f * s - 12.0f * s;

    // Buttons: checkbox on the left, Skip and Show me around on the right.
    ThemeSettings& theme = EditThemeSettings();
    {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ui::PushText(0, 12.0f);
        const float lw = ImGui::CalcTextSize("Show this at start").x;
        const float lh = ImGui::GetTextLineHeight();
        ui::PopFont();
        ImGui::InvisibleButton("##tchk", ImVec2(16.0f * s + 8.0f * s + lw, btnH));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) {
            theme.tourAtStart = !theme.tourAtStart;
            PersistShowAtStart();
        }
        const ImVec2 b0(x, y + (btnH - 16.0f * s) * 0.5f);
        const ImVec2 b1(b0.x + 16.0f * s, b0.y + 16.0f * s);
        if (theme.tourAtStart) {
            dl->AddRectFilled(b0, b1, Fade(k.acc, a), 4.0f * s);
            ui::StrokeIcon(dl, "M5 12.5l4.5 4.5L19 7", ImVec2(b0.x + 1.0f * s, b0.y + 1.0f * s), 14.0f * s, Fade(k.onAcc, a),
                           2.4f * s);
        } else {
            dl->AddRect(b0, b1, Fade(k.line2, a), 4.0f * s, 0, 1.5f * s);
        }
        ui::PushText(0, 12.0f);
        dl->AddText(ImVec2(b1.x + 8.0f * s, y + (btnH - lh) * 0.5f), Fade(k.muted, a), "Show this at start");
        ui::PopFont();
    }
    const float goW = ui::ButtonSize("Show me around").x, skipW = ui::ButtonSize("Skip").x;
    ImGui::SetCursorScreenPos(ImVec2(p1.x - padX - goW, y));
    if (ui::Button("Show me around", ui::ButtonKind::Primary)) Go(app, 1);
    ImGui::SetCursorScreenPos(ImVec2(p1.x - padX - goW - 10.0f * s - skipW, y));
    if (ui::Button("Skip")) Go(app, -1);
}

// --- Spotlight -------------------------------------------------------------------------------------

void DrawSpot(ConfiguratorApp& app, ImDrawList* dl, float W, float H) {
    const ui::Tokens& k = ui::Tok();
    const float s = Scl();
    const int n = g_step;
    const Step& st = kSteps[n - 1];
    const float scrim = AnimEase(ImGui::GetID("##scrim"), 1.0f, 0.4f);

    // Target rect (the last known one while the page behind is still switching).
    static ImVec2 last[2] = {ImVec2(560.0f, 380.0f), ImVec2(720.0f, 440.0f)};
    if (g_have[n - 1]) {
        last[0] = g_min[n - 1];
        last[1] = g_max[n - 1];
    }
    const float pad = 6.0f * s;
    const float tx = last[0].x - pad, ty = last[0].y - pad;
    const float tw = last[1].x - last[0].x + 2.0f * pad, th = last[1].y - last[0].y + 2.0f * pad;
    const float hx = AnimSpring(ImGui::GetID("##hx"), tx), hy = AnimSpring(ImGui::GetID("##hy"), ty);
    const float hw = AnimSpring(ImGui::GetID("##hw"), tw), hh = AnimSpring(ImGui::GetID("##hh"), th);
    DimAround(dl, W, H, hx, hy, hw, hh, 14.0f * s, IM_COL32(6, 6, 9, static_cast<int>(0.72f * scrim * 255.0f)));
    // Pulsing outline.
    {
        float u = 0.0f, alpha = 1.0f;
        if (GetMotion() != Motion::Off) {
            u = EaseOut(static_cast<float>(std::fmod(ImGui::GetTime(), 1.6) / 1.6));
            alpha = 1.0f - u;
        }
        const float grow = 5.0f * s + u * 0.025f * (std::max)(hw, hh);
        dl->AddRect(ImVec2(hx - grow, hy - grow), ImVec2(hx + hw + grow, hy + hh + grow), Fade(k.acc, alpha),
                    18.0f * s + u * 4.0f * s, 0, 2.0f * s);
    }

    // Tip card: right of the target when it fits, else below it.
    const float cw = 340.0f * s, padX = 24.0f * s;
    const float inner = cw - 2.0f * padX;
    ui::PushText(2, 17.0f);
    const float titleH = WrapH(st.title, inner);
    ui::PopFont();
    ui::PushText(0, 13.0f);
    const float textH = WrapH(st.text, inner);
    ui::PopFont();
    ui::PushText(0, 11.0f);
    const float stepH = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float btnH = 38.0f * s;
    const float ch = 22.0f * s + stepH + 6.0f * s + titleH + 6.0f * s + textH + 18.0f * s + btnH + 20.0f * s;
    float cx, cy;
    if (tx + tw + 18.0f * s + cw <= W - 16.0f * s) {
        cx = tx + tw + 18.0f * s;
        cy = ImClamp(ty, 16.0f * s, (std::max)(16.0f * s, H - ch - 82.0f * s));
    } else {
        cx = ImClamp(tx + tw - cw, 16.0f * s, (std::max)(16.0f * s, W - cw - 16.0f * s));
        cy = ImClamp(ty + th + 16.0f * s, 16.0f * s, (std::max)(16.0f * s, H - ch - 82.0f * s));
    }
    const float px = AnimSpring(ImGui::GetID("##tipx"), cx), py = AnimSpring(ImGui::GetID("##tipy"), cy);
    const float a = AnimEase(ImGui::GetID("##tipa"), 1.0f, 0.35f);
    const ImVec2 p0(px, py), p1(px + cw, py + ch);
    ui::SoftShadow(dl, p0, p1, 16.0f * s, Fade(IM_COL32(0, 0, 0, 255), 0.45f * a), 24.0f * s, 8);
    dl->AddRectFilled(p0, p1, Fade(k.card2, a), 16.0f * s);
    dl->AddRect(p0, p1, Fade(k.line2, a), 16.0f * s);

    float y = py + 22.0f * s;
    const float x = px + padX;
    char label[32];
    std::snprintf(label, sizeof(label), "STEP %d OF %d", n, kStepCount);
    ui::PushText(0, 11.0f);
    dl->AddText(ImVec2(x, y), Fade(k.acc, a), label);
    ui::PopFont();
    y += stepH + 6.0f * s;
    ui::PushText(2, 17.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), Fade(k.text, a), st.title, nullptr, inner);
    ui::PopFont();
    y += titleH + 6.0f * s;
    ui::PushText(0, 13.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), Fade(k.muted, a), st.text, nullptr, inner);
    ui::PopFont();
    y += textH + 18.0f * s;

    // Progress dots (the active one stretches), Back, Next / Finish.
    float dx = x;
    for (int i = 0; i < kStepCount; ++i) {
        ImGui::PushID(i);
        const float w = AnimSpring(ImGui::GetID("##dot"), i == n - 1 ? 18.0f : 6.0f) * s;
        ImGui::PopID();
        dl->AddRectFilled(ImVec2(dx, y + (btnH - 6.0f * s) * 0.5f), ImVec2(dx + (std::max)(w, 2.0f * s), y + (btnH + 6.0f * s) * 0.5f),
                          Fade(i == n - 1 ? k.acc : k.line2, a), 3.0f * s);
        dx += w + 5.0f * s;
    }
    const char* next = n == kStepCount ? "Finish" : "Next";
    const float nextW = ui::ButtonSize(next).x, backW = ui::ButtonSize("Back").x;
    ImGui::SetCursorScreenPos(ImVec2(p1.x - padX - nextW, y));
    const bool goNext = ui::Button(next, ui::ButtonKind::Primary);
    ImGui::SetCursorScreenPos(ImVec2(p1.x - padX - nextW - 10.0f * s - backW, y));
    const bool goBack = ui::Button("Back");
    // Skip tour, bottom right.
    const float skipW = ui::ButtonSize("Skip tour").x;
    ImGui::SetCursorScreenPos(ImVec2(W - 24.0f * s - skipW, H - 20.0f * s - btnH));
    const bool skip = ui::Button("Skip tour");

    if (goNext) Go(app, n + 1);
    else if (goBack) Go(app, n - 1);
    else if (skip) Go(app, -1);
}

} // namespace

void Start() {
    g_step = 0;
    g_age = 0.0f;
    ++g_run;
}

bool Active() { return g_step >= 0; }

void Register(const char* key, ImVec2 min, ImVec2 max) {
    if (g_step < 0 || !key) return;
    for (int i = 0; i < kStepCount; ++i) {
        if (std::strcmp(key, kSteps[i].key) == 0) {
            g_min[i] = min;
            g_max[i] = max;
            g_have[i] = true;
            return;
        }
    }
}

void Draw(ConfiguratorApp& app) {
    if (g_step < 0) return;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    g_age += ImGui::GetIO().DeltaTime;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##tour", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PushID(g_run);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_step == 0) DrawWelcome(app, dl, disp.x, disp.y);
    else DrawSpot(app, dl, disp.x, disp.y);

    if (g_step >= 0 && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) Go(app, -1);
        else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) Go(app, g_step + 1);
        else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && g_step > 0) Go(app, g_step - 1);
    }
    ImGui::PopID();
    ImGui::End();
}

} // namespace svms::cfg::tour
