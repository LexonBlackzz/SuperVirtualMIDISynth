#include "PageHome.h"
#include "Anim.h"
#include "ConfigDocument.h"
#include "ConfiguratorApp.h"
#include "DriverLink.h"
#include "ModernWidgets.h"
#include "PagePerformance.h"
#include "PlatformUtil.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace svms::cfg {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTileContent = 113.0f;   // header 19 + gap 10 + gauge 84

std::string WithCommas(uint32_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

// A rounded rect filled with a left-to-right colour ramp (alpha included).
void GradientRoundRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 left, ImU32 right) {
    const int first = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding);
    const float w = (std::max)(1.0f, b.x - a.x);
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = ImClamp((v.pos.x - a.x) / w, 0.0f, 1.0f);
        const ImU32 c = ui::MixU32(left, right, t);
        const ImU32 alpha = (((c >> IM_COL32_A_SHIFT) & 0xFF) * ((v.col >> IM_COL32_A_SHIFT) & 0xFF)) / 255u;
        v.col = (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
    }
}

// Tile header: muted label on the left, mono value on the right.
void TileHeader(const char* label, const char* value) {
    const ui::Tokens& k = ui::Tok();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ui::PushText(0, 13.0f);
    const float h = ImGui::GetTextLineHeight();
    const float lw = ImGui::CalcTextSize(ui::Caps(label)).x;
    dl->AddText(p, k.muted, ui::Caps(label));
    ui::PopFont();
    ui::PushNum(12.0f);
    const ImVec2 vs = ImGui::CalcTextSize(value);
    if (lw + vs.x + 12.0f < w) dl->AddText(ImVec2(p.x + w - vs.x, p.y + (h - vs.y) * 0.5f), k.muted, value);
    ui::PopFont();
    ImGui::Dummy(ImVec2(w, h));
}

void HeroCard(const ConfigValues& w, const svms::RuntimeLinkTelemetryV2* t, ConfiguratorApp* app) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    const bool on = t != nullptr;
    char desc[256];
    if (on) {
        static const char* synths[] = {"SVMS engine", "SVMS-API DLL", "KDMAPI DLL", "WinMM device", "Auto"};
        const bool asio = w.audioBackend == L"asio";
        const uint32_t sr = t->sampleRate ? t->sampleRate : w.sampleRate;
        const float ms = 1000.0f * static_cast<float>(t->bufferFrames ? t->bufferFrames : w.bufferFrames) /
                         static_cast<float>((std::max)(1u, sr));
        std::snprintf(desc, sizeof(desc), "%s \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 %g kHz \xC2\xB7 %.1f ms latency",
                      app ? app->ConnectedAppName().c_str() : "App",
                      synths[(std::min)(4u, w.apiBackend)], asio ? "ASIO" : "WASAPI", sr / 1000.0, ms);
    } else {
        std::snprintf(desc, sizeof(desc), "%s",
                      "Nothing is using SVMS right now. You can still change and save settings; they apply when an app opens MIDI.");
    }

    ui::BeginCard("##hero");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float ring = 52.0f * s;
    const ImVec2 btn = ui::ButtonSize("Output settings");
    const float gap = 18.0f * s;
    const float textW = (std::max)(120.0f * s, width - ring - gap * 2.0f - btn.x);

    ui::PushText(2, 18.0f);
    const float titleH = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushText(0, 13.0f);
    const ImVec2 dsz = ImGui::CalcTextSize(desc, nullptr, false, textW);
    ui::PopFont();
    const float block = titleH + 2.0f * s + dsz.y;
    const float height = (std::max)(ring, block);

    if (!k.rack) {
        // Soft accent wash from the top-left corner, clipped to the card shape.
        const ImVec2 a(p.x - 24.0f * s + 1.0f, p.y - 22.0f * s + 1.0f);
        const ImVec2 b(p.x + width + 24.0f * s - 1.0f, p.y + height + 22.0f * s - 1.0f);
        GradientRoundRect(dl, a, b, 13.0f * s, k.accSoft, ui::WithAlpha(k.acc, 0.0f));
    }

    const ImVec2 c(p.x + ring * 0.5f, p.y + height * 0.5f);
    dl->AddCircleFilled(c, ring * 0.5f, k.card2, 40);
    dl->AddCircle(c, ring * 0.5f, k.line2, 40);
    if (on && (GetMotion() == Motion::Full)) {
        const float ph = static_cast<float>(std::fmod(ImGui::GetTime(), 2.4) / 2.4);
        const float e = EaseOut(ph);
        dl->AddCircle(c, (ring * 0.5f + 6.0f * s) * (0.5f + 1.3f * e), ui::WithAlpha(k.ok, 0.36f * (1.0f - e)), 40, 2.0f * s);
    }
    ui::StrokeIcon(dl, on ? "M20 6L9 17l-5-5" : "M12 8v4M12 16h.01M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z",
                   ImVec2(c.x - 12.0f * s, c.y - 12.0f * s), 24.0f * s, on ? k.ok : k.muted, 2.0f * s);

    const float tx = p.x + ring + gap;
    const float ty = p.y + (height - block) * 0.5f;
    ui::PushText(2, 18.0f);
    dl->AddText(ImVec2(tx, ty), k.text, on ? "Playing through SVMS" : "Waiting for an app");
    ui::PopFont();
    ui::PushText(0, 13.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, ty + titleH + 2.0f * s), k.muted, desc,
                nullptr, textW);
    ui::PopFont();

    ImGui::Dummy(ImVec2(width, height));
    ImGui::SetCursorScreenPos(ImVec2(p.x + width - btn.x, p.y + (height - btn.y) * 0.5f));
    if (ui::Button("Output settings") && app) app->Navigate(NavPage::Output);
    ui::EndCard();
}

void VoicesTile(const svms::RuntimeLinkTelemetryV2* t, uint32_t cap) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ui::BeginCard("##t_voices");
    char capText[48];
    std::snprintf(capText, sizeof(capText), "of %s", WithCommas(cap).c_str());
    TileHeader("Voices playing", capText);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ui::PushNum(34.0f, true);
    dl->AddText(ImVec2(p.x, p.y + 10.0f * s), k.text, t ? WithCommas(t->activeVoices).c_str() : "\xE2\x80\x94");
    ui::PopFont();
    const float bh = 10.0f * s;
    const float by = p.y + (kTileContent - 19.0f) * s - bh;
    dl->AddRectFilled(ImVec2(p.x, by), ImVec2(p.x + w, by + bh), k.card2, bh * 0.5f);
    const float f = t ? ImClamp(static_cast<float>(t->activeVoices) / static_cast<float>((std::max)(1u, cap)), 0.0f, 1.0f) : 0.0f;
    const float shown = AnimEase(ImGui::GetID("##vf"), f, 0.15f);
    if (shown > 0.001f) dl->AddRectFilled(ImVec2(p.x, by), ImVec2(p.x + (std::max)(bh, w * shown), by + bh), k.acc, bh * 0.5f);
    ImGui::Dummy(ImVec2(w, (kTileContent - 19.0f) * s));
    ui::EndCard();
}

// --- Campfire (past 500% load) -------------------------------------------------------------------
// A particle sim in the foreground draw list, over the ring. Layout numbers are the prototype's
// FIRE/mkP table. Ignition is staged: smoke 0 s, base glow 0.6 s, flames grow from the bottom
// 0.9-1.35 s per layer, embers 1.9 s; 1.5 s fade-out when the load drops.
struct FireLayer {
    int n;
    float size, spread, rise, period, seed;
};

float Frac(double v) { return static_cast<float>(v - std::floor(v)); }

// Deterministic per-particle randoms, the prototype's sin hash.
float Rnd(int i, float seed) { return Frac(std::sin((i + 1) * static_cast<double>(seed)) * 10000.0); }

struct FireState {
    float t = 0.0f;       // seconds since ignition
    float alpha = 0.0f;   // 1 while burning, 1 -> 0 over 1.5 s afterwards
};

ImU32 FireCol(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, static_cast<int>(ImClamp(a, 0.0f, 1.0f) * 255.0f));
}

void DrawCampfire(const ImVec2& c, float s, bool burning, bool still) {
    static FireState st;
    const float dt = ImGui::GetIO().DeltaTime;
    if (still) {                       // Motion Off: no animation, so no fade either; one calm frame
        st.alpha = burning ? 1.0f : 0.0f;
        st.t = 10.0f;
    } else if (burning) {
        st.alpha = (std::min)(1.0f, st.alpha + dt / 0.15f);
        st.t += dt;
    } else if (st.alpha > 0.0f) {
        st.alpha -= dt / 1.5f;
        st.t += dt;
        if (st.alpha <= 0.0f) st = FireState();
    }
    if (st.alpha <= 0.0f) return;

    const float master = st.alpha * (2.0f - st.alpha);   // ease-in fade-out
    const double time = still ? 0.35 : ImGui::GetTime();
    const float t = st.t;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 o(c.x, c.y + (42.0f + 4.0f) * s);       // bottom centre of the prototype's fire box
    auto col = [&](int r, int g, int b, float a) { return FireCol(r, g, b, a * master); };

    // Smoke: puffs rise, grow and fade. Starts at once.
    {
        static const FireLayer sm{6, 0, 22, 0, 3.2f, 9.1f};
        const float in = ImClamp(t / 1.2f, 0.0f, 1.0f);
        for (int i = 0; i < sm.n; ++i) {
            const float r = Rnd(i, sm.seed);
            const float x = ((i / float(sm.n - 1)) - 0.5f) * 2.0f * sm.spread * (0.7f + 0.3f * r);
            const float dx = (r - 0.5f) * 60.0f;
            const float u = Frac(time / sm.period + ((i * 7 % sm.n) * sm.period / sm.n + r * 0.15f) / sm.period);
            const float a = (u < 0.25f ? u / 0.25f * 0.6f : 0.6f * (1.0f - (u - 0.25f) / 0.75f)) * in;
            const float rad = 19.0f * (0.5f + 2.3f * u) * s;
            const ImVec2 p(o.x + (x + dx * u) * s, o.y - (129.0f + 150.0f * u) * s);
            dl->AddCircleFilled(p, rad, col(110, 106, 116, a * 0.22f), 24);
            dl->AddCircleFilled(p, rad * 0.75f, col(110, 106, 116, a * 0.3f), 24);
            dl->AddCircleFilled(p, rad * 0.5f, col(110, 106, 116, a * 0.4f), 20);
        }
    }
    // Base glow under the flames, pulsing once it is lit.
    {
        const float in = ImClamp((t - 0.6f) / 0.8f, 0.0f, 1.0f);
        const float pulse = t > 1.4f ? 0.5f + 0.5f * std::sin((t - 1.4f) * 2.85f) : 0.0f;
        const float a = in * (1.0f - 0.45f * pulse);
        const float sc = 1.0f + 0.1f * pulse;
        const ImVec2 gc(o.x, o.y - 35.0f * s);
        for (int i = 0; i < 7; ++i) {
            const float f = (7 - i) / 7.0f;
            dl->AddEllipseFilled(gc, ImVec2(95.0f * 0.75f * f * sc * s, 45.0f * 0.75f * f * sc * s),
                                 col(255, 120, 30, a * 0.07f), 0.0f, 32);
        }
    }
    // Flame tongues: red, orange, yellow, white core. Each layer grows from the bottom of the box.
    {
        static const FireLayer layers[4] = {{22, 46, 44, -125, 1.15f, 12.9f},
                                            {18, 34, 32, -105, 1.0f, 7.3f},
                                            {14, 24, 20, -78, 0.85f, 3.1f},
                                            {8, 14, 9, -46, 0.7f, 5.7f}};
        static const int rgb[4][3] = {{229, 54, 27}, {255, 122, 26}, {255, 201, 60}, {255, 244, 204}};
        static const float layerAlpha[4] = {0.7f, 0.7f, 0.8f, 0.9f};
        for (int li = 0; li < 4; ++li) {
            const FireLayer& L = layers[li];
            const float g = ImClamp((t - (0.9f + 0.15f * li)) / 1.5f, 0.0f, 1.0f);
            if (g <= 0.0f) continue;
            const float e = EaseOut(g);
            const float sx = 0.25f + 0.75f * e, sy = 0.04f + 0.96f * e;
            const float la = ImClamp(e * 2.0f, 0.0f, 1.0f) * layerAlpha[li];
            const ImU32 cc = col(rgb[li][0], rgb[li][1], rgb[li][2], la);
            for (int i = 0; i < L.n; ++i) {
                const float r = Rnd(i, L.seed);
                const float x = (L.n > 1 ? (i / float(L.n - 1) - 0.5f) * 2.0f : 0.0f) * L.spread * (0.7f + 0.3f * r);
                const float dx = -x * 0.5f, h = L.rise * (0.8f + 0.35f * r), size = L.size * (0.8f + 0.4f * r);
                const float u = Frac(time / L.period + ((i * 7 % L.n) * L.period / L.n + r * 0.15f) / L.period);
                const float k = std::pow(u, 1.25f);
                // Position relative to the box bottom, scaled by the ignition transform.
                const float px = (x + dx * k) * sx, py = (-14.0f + h * k) * sy;
                const float rad = size * 0.5f * (1.0f - 0.95f * k) * sx * s;
                const ImVec2 p(o.x + px * s, o.y + py * s);
                if (li == 0) dl->AddCircleFilled(p, rad * 1.5f, col(255, 80, 20, la * 0.1f), 24);   // red rim glow
                dl->AddCircleFilled(p, rad, cc, 24);
            }
        }
    }
    // Embers drifting up.
    {
        static const FireLayer em{12, 0, 50, 0, 1.9f, 4.4f};
        const float in = ImClamp((t - 1.9f) / 1.0f, 0.0f, 1.0f);
        if (in > 0.0f) {
            for (int i = 0; i < em.n; ++i) {
                const float r = Rnd(i, em.seed);
                const float x = ((i / float(em.n - 1)) - 0.5f) * 2.0f * em.spread * (0.7f + 0.3f * r);
                const float dx = (r - 0.5f) * 60.0f;
                const float u = Frac(time / em.period + ((i * 7 % em.n) * em.period / em.n + r * 0.15f) / em.period);
                const float a = (u < 0.12f ? u / 0.12f : 1.0f - (u - 0.12f) / 0.88f) * in;
                const ImVec2 p(o.x + (x + dx * u) * s, o.y - (52.0f + 180.0f * u) * s);
                dl->AddCircleFilled(p, 5.0f * s, col(255, 138, 31, a * 0.35f), 12);
                dl->AddCircleFilled(p, 2.0f * s, col(255, 224, 154, a), 10);
            }
        }
    }
}

void LoadTile(const svms::RuntimeLinkTelemetryV2* t, const ConfigValues& cfg) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    // Warm tile glow while it burns, faded in and out over ~1.4 s.
    const bool burning = GetThemeSettings().fire && t && t->cpuLoadPercent >= 500.0f;
    static ImVec2 cardMin, cardMax;
    const float heat = AnimEase(ImGui::GetID("##heat"), burning ? 1.0f : 0.0f, 1.4f);
    if (heat > 0.01f && !k.rack && cardMax.x > cardMin.x) {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        wdl->PushClipRectFullScreen();
        ui::SoftShadow(wdl, cardMin, cardMax, k.rCard * s, ui::WithAlpha(IM_COL32(255, 106, 26, 255), 0.5f * heat),
                       26.0f * s, 8);
        wdl->PopClipRect();
        ui::SetNextCardBorder(ui::WithAlpha(IM_COL32(255, 106, 26, 255), 0.33f * heat));
    }
    ui::BeginCard("##t_load");
    cardMin = ImGui::GetWindowPos();
    cardMax = ImVec2(cardMin.x + ImGui::GetWindowSize().x, cardMin.y + ImGui::GetWindowSize().y);
    const uint32_t sr = t && t->sampleRate ? t->sampleRate : cfg.sampleRate;
    const uint32_t frames = t && t->bufferFrames ? t->bufferFrames : cfg.bufferFrames;
    char budget[48];
    std::snprintf(budget, sizeof(budget), "%.1f ms per block", 1000.0f * frames / static_cast<float>((std::max)(1u, sr)));
    TileHeader("Render load", budget);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 10.0f * s));
    const ImVec2 q = ImGui::GetCursorScreenPos();

    const float load = AnimEase(ImGui::GetID("##load"), t ? (std::max)(0.0f, t->cpuLoadPercent) : 0.0f, 0.3f);
    const bool over = load >= 500.0f;
    const int lap = static_cast<int>((std::min)(load, 499.99f) / 100.0f);
    const float frac = over ? 1.0f : (load - lap * 100.0f) / 100.0f;
    const ImU32 col = ui::LoadColor(load, k.acc);
    const float gd = 84.0f * s;
    const ImVec2 c(q.x + gd * 0.5f, q.y + gd * 0.5f);
    const float r = 34.0f * s, sw = 8.0f * s;
    const float glow = AnimEase(ImGui::GetID("##loadglow"), ui::LoadGlow(load), 0.8f) * s;   // fades in, never snaps
    for (int i = 4; i >= 1 && glow > 0.1f; --i) {   // ring-shaped glow, strongest at the stroke
        const float f = static_cast<float>(i) / 4.0f;
        dl->AddCircle(c, r, ui::WithAlpha(col, 0.16f * (1.0f - f) + 0.02f), 64, sw + 2.0f * glow * f);
    }
    dl->AddCircle(c, r, lap > 0 ? ui::WithAlpha(ui::LoadColor(lap * 100.0f, k.acc), 0.4f) : k.card3, 64, sw);
    if (frac >= 0.999f) {
        dl->AddCircle(c, r, col, 64, sw);
    } else if (frac > 0.0f) {
        dl->PathArcTo(c, r, -kPi * 0.5f, -kPi * 0.5f + 2.0f * kPi * frac, 64);
        dl->PathStroke(col, 0, sw);
    }

    const float tx = q.x + gd + 16.0f * s;
    char big[32];
    if (t) std::snprintf(big, sizeof(big), "%.0f%%", load);
    else std::snprintf(big, sizeof(big), "%s", "\xE2\x80\x94");
    const char* sub = load >= 500.0f ? "way past budget. It is on fire."
                      : load > 100.0f ? "over budget: playback slows or skips"
                                      : "of the time each audio block has";
    ui::PushNum(34.0f, true);
    const float bigH = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushText(0, 13.0f);
    const float subW = (std::max)(60.0f * s, w - gd - 16.0f * s);
    const ImVec2 ssz = ImGui::CalcTextSize(sub, nullptr, false, subW);
    ui::PopFont();
    const float block = bigH + 10.0f * s + ssz.y;
    const float ty = q.y + (gd - block) * 0.5f;
    ui::PushNum(34.0f, true);
    dl->AddText(ImVec2(tx, ty), ui::MixU32(k.text, col, ImClamp((load - 100.0f) / 15.0f, 0.0f, 1.0f)), big);
    ui::PopFont();
    ui::PushText(0, 13.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, ty + bigH + 10.0f * s), k.muted, sub, nullptr, subW);
    ui::PopFont();
    DrawCampfire(c, s, burning, GetMotion() == Motion::Off);
    ImGui::SetCursorScreenPos(q);
    ImGui::Dummy(ImVec2(w, gd));
    ui::EndCard();
}

struct Peak {
    float level = 0.0f, hold = 0.0f;
    double heldAt = 0.0;
};

void Follow(float& v, float target, float dt) {
    v += (target - v) * (1.0f - std::exp(-(target > v ? 30.0f : 6.0f) * dt));
}

void OutputTile(const svms::RuntimeLinkTelemetryV2* t) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    static Peak ch[2];
    const float dt = ImGui::GetIO().DeltaTime;
    const double now = ImGui::GetTime();
    const float target[2] = {t ? t->limiterOutputPeakL : 0.0f, t ? t->limiterOutputPeakR : 0.0f};
    for (int i = 0; i < 2; ++i) {
        Follow(ch[i].level, target[i], dt);
        if (ch[i].level >= ch[i].hold) {
            ch[i].hold = ch[i].level;
            ch[i].heldAt = now;
        } else if (now - ch[i].heldAt > 1.0) {
            ch[i].hold = (std::max)(ch[i].level, ch[i].hold - 0.8f * dt);
        }
    }
    ui::BeginCard("##t_out");
    const float peak = (std::max)(ch[0].level, ch[1].level);
    char level[32];
    if (!t) std::snprintf(level, sizeof(level), "%s", "\xE2\x80\x94");
    else if (peak <= 0.0001f) std::snprintf(level, sizeof(level), "\xE2\x88\x92\xE2\x88\x9E dB");
    else std::snprintf(level, sizeof(level), "%.1f dB", 20.0f * std::log10(peak));
    TileHeader("Output level", level);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float bh = 10.0f * s, rowGap = 8.0f * s;
    const float total = (kTileContent - 19.0f) * s;
    float y = p.y + total - 2.0f * bh - rowGap;
    static const char* names[2] = {"L", "R"};
    for (int i = 0; i < 2; ++i) {
        ui::PushNum(11.0f);
        const ImVec2 ls = ImGui::CalcTextSize(names[i]);
        dl->AddText(ImVec2(p.x, y + (bh - ls.y) * 0.5f), k.muted, names[i]);
        ui::PopFont();
        const float x0 = p.x + 8.0f * s + 8.0f * s + 8.0f * s, x1 = p.x + w;
        const float bw = x1 - x0;
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + bh), k.card2, bh * 0.5f);
        const float fill = bw * ImClamp(ch[i].level / 1.3f, 0.0f, 1.0f);
        const float segs[3][2] = {{0.0f, 0.7f}, {0.7f, 0.9f}, {0.9f, 1.0f}};
        const ImU32 cols[3] = {k.ok, k.acc, k.bad};
        dl->PushClipRect(ImVec2(x0, y), ImVec2(x0 + fill, y + bh), true);
        for (int sIdx = 0; sIdx < 3; ++sIdx)
            dl->AddRectFilled(ImVec2(x0 + bw * segs[sIdx][0], y), ImVec2(x0 + bw * segs[sIdx][1] + 1.0f, y + bh), cols[sIdx], bh * 0.5f);
        dl->PopClipRect();
        if (ch[i].hold > 0.002f) {
            const float hx = x0 + (std::min)(bw - 2.0f * s, bw * ImClamp(ch[i].hold / 1.3f, 0.0f, 0.99f));
            dl->AddRectFilled(ImVec2(hx, y), ImVec2(hx + 2.0f * s, y + bh), k.text);
        }
        y += bh + rowGap;
    }
    ImGui::Dummy(ImVec2(w, total));
    ui::EndCard();
}

void SoundFontCard(const ConfigValues& w, ConfiguratorApp* app) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ui::BeginCard("##sfcard", "SoundFont", nullptr, ui::ButtonSize("Change").x);
    if (ui::Button("Change") && app) app->Navigate(NavPage::Sound);
    ui::CardHeaderEnd();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float box = 44.0f * s;
    std::string name = "None chosen", dir;
    if (!w.soundFontPath.empty()) {
        const fs::path sf(w.soundFontPath);
        name = util::WideToUtf8(sf.filename().wstring());
        dir = util::WideToUtf8(sf.parent_path().wstring());
    }
    dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), k.accSoft, 12.0f * s);
    ui::StrokeIcon(dl, "M9 18V5l12-2v13M9 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0zM21 16a3 3 0 1 1-6 0 3 3 0 0 1 6 0z",
                   ImVec2(p.x + (box - 22.0f * s) * 0.5f, p.y + (box - 22.0f * s) * 0.5f), 22.0f * s, k.acc, 1.8f * s);
    const float tx = p.x + box + 14.0f * s;
    dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + width, p.y + box), true);
    ui::PushText(2, 16.0f);
    const float nh = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushNum(12.0f);
    const float dh = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float ty = p.y + (box - nh - dh) * 0.5f;
    ui::PushText(2, 16.0f);
    dl->AddText(ImVec2(tx, ty), k.text, name.c_str());
    ui::PopFont();
    ui::PushNum(12.0f);
    dl->AddText(ImVec2(tx, ty + nh), k.muted, dir.c_str());
    ui::PopFont();
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(width, box));
    ui::EndCard();
}

void ProfileCard(ConfigDocument& doc) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ui::BeginCard("##profcard", "Performance profile", nullptr, ui::ButtonSize("Details").x);
    if (ui::Button("Details")) {
        if (ConfiguratorApp* app = GetLiveLinkContext().app) app->Navigate(NavPage::Performance);
    }
    ui::CardHeaderEnd();
    static const char* labels[3] = {"Accurate", "Balanced", "Extreme"};
    static const char* descs[3] = {
        "Every note exactly as written. For normal MIDI and for recording.",
        "Only the shortcuts that sound the same. A safe pick for dense files.",
        "Every shortcut on. For million-note Black MIDI in realtime."};
    int cur = MatchPerformanceProfile(doc.Working());
    if (ui::Segmented("##prof", &cur, labels, 3, -1.0f) && cur >= 0) ApplyPerformanceProfile(doc, cur);
    ImGui::Dummy(ImVec2(0.0f, 10.0f * s - ImGui::GetStyle().ItemSpacing.y));
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(MatchPerformanceProfile(doc.Working()) < 0
                               ? "Custom: you changed individual switches on the Performance page."
                               : descs[MatchPerformanceProfile(doc.Working())]);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::PopFont();
    ui::EndCard();
}

} // namespace

// Home: what SVMS is doing right now, and the switches touched most.
void DrawHomePage(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const LiveLinkContext& lc = GetLiveLinkContext();
    const svms::RuntimeLinkTelemetryV2* t = lc.telemetry;
    const float s = ui::Scale();

    HeroCard(w, t, lc.app);

    const uint32_t cap = t && t->maxVoices ? t->maxVoices : (std::max)(1u, w.maxVoices);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f * s, 0.0f));
    if (ImGui::BeginTable("##home_tiles", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        VoicesTile(t, cap);
        ImGui::TableNextColumn();
        LoadTile(t, w);
        ImGui::TableNextColumn();
        OutputTile(t);
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##home_cards", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        ui::BeginCardRow("##home_cards");
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        SoundFontCard(w, lc.app);
        ImGui::TableNextColumn();
        ProfileCard(doc);
        ui::EndCardRow();
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ui::BeginCard("##quick", "Quick switches");
    {
        bool v = w.enableReverb;
        ui::BeginRow("revOnQ", "Reverb", "Room sound added after the synth. Tune it on the Effects page.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##rev", &v)) {
            w.enableReverb = v;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetReverbEnabled, v);
        }
        ui::EndRow();
    }
    {
        bool v = w.limiterEnabled;
        ui::BeginRow("limOnQ", "Limiter", "Stops the output from clipping when thousands of notes pile up.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##lim", &v)) {
            w.limiterEnabled = v;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetLimiterEnabled, v);
        }
        ui::EndRow();
    }
    {
        static const char* labels[5] = {"Off", "Analytic", "Sweep", "Diffuse", "Random"};
        int phase = static_cast<int>((std::min)(4u, w.phaseRotationMode));
        const ui::ChipDef chip = {"About 2\xC3\x97 CPU when on", ui::ChipKind::Cpu};
        ui::BeginRow("phaseQ", "Voice character", "Gives every voice its own phase so the Black MIDI hum stops stacking up.",
                     ui::SegmentedSize(labels, 5), &chip, 1);
        if (ui::Segmented("##phase", &phase, labels, 5)) {
            w.phaseRotationMode = static_cast<uint32_t>(phase);
            doc.MarkDirty();
            SendLiveCommand(svms::RLCommandType::SetPhaseRotation, w.phaseRotationMode);
        }
        ui::EndRow();
    }
    ui::EndCard();
}

} // namespace svms::cfg
