#include "configurator/PageEffects.h"
#include "configurator/Anim.h"
#include "configurator/ConfigDocument.h"
#include "configurator/ConfiguratorApp.h"
#include "configurator/DriverLink.h"
#include "configurator/ModernWidgets.h"
#include "configurator/PlatformUtil.h"
#include "configurator/Widgets.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>

namespace svms::cfg {
namespace {

using C = LiveParam;
constexpr const char* kMinus = "\xE2\x88\x92";

float Clamp(float v, float lo, float hi) { return (std::max)(lo, (std::min)(hi, v)); }
float S() { return ui::Scale(); }

// Muted 13 px line (the prototype's .cs).
void NoteLine(const char* text, float topGap = 0.0f) {
    if (topGap > 0.0f) ImGui::Dummy(ImVec2(0.0f, topGap * S() - ImGui::GetStyle().ItemSpacing.y));
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::PopFont();
}

// Mono 11 px label placed at an absolute position.
void MonoAt(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, bool alignRight = false) {
    ui::PushNum(11.0f);
    if (alignRight) pos.x -= ImGui::CalcTextSize(text).x;
    dl->AddText(pos, col, text);
    ui::PopFont();
}

// Header right slot: "On"/"Off" word and the toggle. Call between BeginCard
// (rightSlotW = HeaderToggleWidth()) and CardHeaderEnd().
float HeaderToggleWidth() {
    ui::PushText(0, 13.0f);
    const float w = ImGui::CalcTextSize("Off").x;
    ui::PopFont();
    return w + 10.0f * S() + 46.0f * S();
}

bool HeaderToggle(const char* id, bool* on) {
    const ui::Tokens& k = ui::Tok();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ui::PushText(0, 13.0f);
    const char* word = *on ? "On" : "Off";
    const float offW = ImGui::CalcTextSize("Off").x;
    const ImVec2 ts = ImGui::CalcTextSize(word);
    dl->AddText(ImVec2(p.x + offW - ts.x, p.y + (38.0f * S() - ts.y) * 0.5f), k.muted, word);
    ui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + offW + 10.0f * S(), p.y + 5.0f * S()));
    const bool changed = ui::Toggle(id, on);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 38.0f * S()));
    return changed;
}

// Fades the content of a switched-off block to 40 %, like the prototype's
// .off-dim: ImGui has no group opacity, so a card-coloured veil is laid over
// the span drawn between Begin and End.
struct Dim {
    ImVec2 start;
    float width;
    ImGuiID id;
    void Begin(const char* key) {
        id = ImGui::GetID(key);
        start = ImGui::GetCursorScreenPos();
        width = ImGui::GetContentRegionAvail().x;
    }
    void End(bool on) {
        const float t = AnimEase(id, on ? 1.0f : 0.0f, 0.3f);
        if (t >= 0.999f) return;
        const ImVec2 end(start.x + width, ImGui::GetCursorScreenPos().y);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(start.x - 4.0f * S(), start.y - 4.0f * S()), ImVec2(end.x + 4.0f * S(), end.y),
            ui::WithAlpha(ui::Tok().card, 0.6f * (1.0f - t)));
    }
};

// --- Knobs ------------------------------------------------------------------------------

float Pct(float x) { return x * 100.0f; }
float KHz(float hz) { return hz / 1000.0f; }
float LinearToDb(float linear) {
    if (!std::isfinite(linear) || linear <= 0.000001f) return -60.0f;
    return (std::max)(-60.0f, 20.0f * std::log10(linear));
}

struct KnobDef {
    const char* id;     // registry id (search anchor)
    ui::KnobSpec spec;
    float ConfigValues::*field;
    C command;
};

const KnobDef kRevMain[] = {
    {"revMix", {"Mix", 0.0f, 1.0f, 0.25f, 0.01f, false, "%.0f%%", Pct}, &ConfigValues::reverbMix, C::SetReverbMix},
    {"revSize", {"Room size", 0.0f, 1.0f, 0.60f, 0.01f, false, "%.0f%%", Pct}, &ConfigValues::reverbRoomSize, C::SetReverbRoomSize},
    {"revDecay", {"Decay", 0.0f, 1.0f, 0.50f, 0.01f, false, "%.0f%%", Pct}, &ConfigValues::reverbDecay, C::SetReverbDecay},
    {"revPre", {"Pre-delay", 0.0f, 200.0f, 12.0f, 1.0f, false, "%.0f ms"}, &ConfigValues::reverbPreDelayMs, C::SetReverbPreDelayMs},
};
const KnobDef kRevMore[] = {
    {"revWidth", {"Width", 0.0f, 1.0f, 1.0f, 0.01f, false, "%.0f%%", Pct}, &ConfigValues::reverbWidth, C::SetReverbWidth},
    {"revEarly", {"Early", 0.0f, 1.5f, 0.35f, 0.01f, false, "%.2f"}, &ConfigValues::reverbEarlyLevel, C::SetReverbEarlyLevel},
    {"revLate", {"Late", 0.0f, 1.5f, 0.85f, 0.01f, false, "%.2f"}, &ConfigValues::reverbLateLevel, C::SetReverbLateLevel},
    {"revDiff", {"Diffusion", 0.0f, 1.0f, 0.70f, 0.01f, false, "%.0f%%", Pct}, &ConfigValues::reverbDiffusion, C::SetReverbDiffusion},
    {"revDamp", {"Damping", 0.0f, 1.0f, 0.35f, 0.01f, false, "%.0f%%", Pct}, &ConfigValues::reverbDamping, C::SetReverbDamping},
    {"revModD", {"Mod depth", 0.0f, 1.0f, 0.30f, 0.01f, false, "%.0f%%", Pct}, &ConfigValues::reverbModDepth, C::SetReverbModDepth},
    {"revModR", {"Mod rate", 0.0f, 1.0f, 0.35f, 0.01f, false, "%.2f"}, &ConfigValues::reverbModRate, C::SetReverbModRate},
    {"revLow", {"Low cut", 0.0f, 2000.0f, 70.0f, 1.0f, false, "%.0f Hz"}, &ConfigValues::reverbLowCutHz, C::SetReverbLowCutHz},
    {"revHigh", {"High cut", 1000.0f, 20000.0f, 16000.0f, 100.0f, false, "%.1f kHz", KHz}, &ConfigValues::reverbHighCutHz, C::SetReverbHighCutHz},
};

// A row of knobs spread over the card's width, wrapping when they do not fit.
template <class Fn>
void KnobRow(int count, Fn&& draw) {
    const ImVec2 ks = ui::KnobSize();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const int perRow = (std::max)(1, (std::min)(count, static_cast<int>(width / ks.x)));
    const float slot = width / perRow;
    const int rows = (count + perRow - 1) / perRow;
    for (int i = 0; i < count; ++i) {
        const int r = i / perRow, c = i % perRow;
        const ImVec2 pos(origin.x + c * slot + (slot - ks.x) * 0.5f, origin.y + r * (ks.y + 8.0f * S()));
        ImGui::SetCursorScreenPos(pos);
        draw(i, pos);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, rows * ks.y + (rows - 1) * 8.0f * S()));
}

void LiveKnob(ConfigDocument& doc, const KnobDef& def, ImVec2 pos) {
    ConfigValues& w = doc.Working();
    const ImVec2 ks = ui::KnobSize();
    ui::Anchor(def.id, pos, ImVec2(pos.x + ks.x, pos.y + ks.y));
    if (ui::Knob(def.id, &(w.*def.field), def.spec)) {
        doc.MarkDirty();
        PushLiveFloat(def.command, w.*def.field);
    }
}

void KnobGroup(ConfigDocument& doc, const KnobDef* defs, int count) {
    KnobRow(count, [&](int i, ImVec2 pos) { LiveKnob(doc, defs[i], pos); });
}

// --- Chain ------------------------------------------------------------------------------

struct ChainNode {
    const char* id;
    const char* name;
    std::string sub;
    bool on;
};

void DashedWire(ImDrawList* dl, float x1, float x2, float y, bool flow) {
    const ui::Tokens& k = ui::Tok();
    const float period = 10.0f * S(), dash = 5.0f * S();
    float phase = 0.0f;
    if (flow && GetMotion() != Motion::Off)
        phase = std::fmod(static_cast<float>(ImGui::GetTime()) / 0.6f, 1.0f) * period;
    const ImU32 col = flow ? k.acc : k.line2;
    for (float x = x1 - period + phase; x < x2; x += period) {
        const float a = (std::max)(x, x1), b = (std::min)(x + dash, x2);
        if (b > a) dl->AddRectFilled(ImVec2(a, y - S()), ImVec2(b, y + S()), col);
    }
}

void ChainCard(std::string& panel, const ConfigValues& w, bool connected) {
    const ui::Tokens& k = ui::Tok();
    ui::BeginCard("##chain");
    char buf[48];
    std::string synthSub = "No SoundFont";
    if (!w.soundFontPaths.empty()) {
        synthSub = util::WideToUtf8(fs::path(w.soundFontPaths.front()).filename().wstring());
    }
    auto dbText = [&](float linear) {
        const float db = LinearToDb(linear);
        std::snprintf(buf, sizeof(buf), "%s%.1f dB", db < 0.0f ? kMinus : "", std::fabs(db));
        return std::string(buf);
    };
    const bool asio = w.audioBackend.find(L"asio") != std::wstring::npos;
    std::snprintf(buf, sizeof(buf), "%s %gk", asio ? "ASIO" : "WASAPI", w.sampleRate / 1000.0);
    const std::string outSub = buf;
    std::snprintf(buf, sizeof(buf), "%.0f%% wet", w.reverbMix * 100.0f);
    const ChainNode nodes[5] = {
        {"synth", "Synth", synthSub, true},
        {"chlim", "Channel limiter", w.channelLimiterEnabled ? dbText(w.channelLimiterThreshold) : "bypassed", w.channelLimiterEnabled},
        {"reverb", "Reverb", w.enableReverb ? std::string(buf) : "bypassed", w.enableReverb},
        {"limiter", "Master limiter", w.limiterEnabled ? dbText(w.limiterThreshold) : "bypassed", w.limiterEnabled},
        {"out", "Output", outSub, true},
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = S();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float nodeH = 60.0f * s;
    const float pad = 6.0f * s;   // room for the hover lift and selection ring
    float nodeW[5];
    float total = 0.0f;
    for (int i = 0; i < 5; ++i) {
        ui::PushText(2, 13.0f);
        const float nw = ImGui::CalcTextSize(nodes[i].name).x + 14.0f * s + 32.0f * s;
        ui::PopFont();
        ui::PushNum(12.0f);
        const float sw = ImGui::CalcTextSize(nodes[i].sub.c_str()).x + 32.0f * s;
        ui::PopFont();
        nodeW[i] = (std::max)((std::max)(132.0f * s, nw), (std::min)(sw, 180.0f * s));
        total += nodeW[i];
    }
    // Too wide for the card (big UI sizes): scroll sideways instead of clipping.
    const float need = total + 4.0f * 18.0f * s;
    const bool scroll = need + 2.0f * pad > avail;
    if (scroll) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##chainscroll", ImVec2(avail, nodeH + 2.0f * pad + 14.0f * s), 0,
                          ImGuiWindowFlags_HorizontalScrollbar);
        dl = ImGui::GetWindowDrawList();
    }
    const float wireW = scroll ? 18.0f * s : (std::max)(18.0f * s, (avail - total) / 4.0f);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    if (scroll) { origin.x += pad; origin.y += pad; }
    float x = origin.x;
    for (int i = 0; i < 5; ++i) {
        if (i > 0) {
            DashedWire(dl, x + 6.0f * s, x + wireW - 6.0f * s, origin.y + nodeH * 0.5f, connected);
            x += wireW;
        }
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::InvisibleButton("##node", ImVec2(nodeW[i], nodeH));
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) {
            if (std::string(nodes[i].id) == "synth") GetLiveLinkContext().app->Navigate(NavPage::Sound);
            else if (std::string(nodes[i].id) == "out") GetLiveLinkContext().app->Navigate(NavPage::Output);
            else panel = nodes[i].id;
        }
        const bool sel = panel == nodes[i].id;
        const float lift = AnimSpring(ImGui::GetID("##lift"), hov ? 1.0f : 0.0f) * 2.0f * s;
        const ImVec2 a(x, origin.y - lift), b(x + nodeW[i], origin.y + nodeH - lift);
        const float r = k.rack ? 3.0f * s : 12.0f * s;
        const float selT = AnimEase(ImGui::GetID("##sel"), sel ? 1.0f : 0.0f, 0.25f);
        if (selT > 0.01f) dl->AddRect(ImVec2(a.x - 2.0f * s, a.y - 2.0f * s), ImVec2(b.x + 2.0f * s, b.y + 2.0f * s),
                                      ui::WithAlpha(k.accSoft, selT), r + 2.0f * s, 0, 4.0f * s);
        dl->AddRectFilled(a, b, k.card2, r);
        dl->AddRect(a, b, ui::MixU32(k.line2, k.acc, selT), r);
        const float onT = AnimEase(ImGui::GetID("##on"), nodes[i].on ? 1.0f : 0.0f, 0.3f);
        const ImVec2 lamp(a.x + 16.0f * s + 3.5f * s, a.y + 12.0f * s + 8.5f * s);
        if (onT > 0.01f) ui::Glow(dl, lamp, 3.5f * s, ui::WithAlpha(k.ok, onT), 8.0f * s, 5);
        dl->AddCircleFilled(lamp, 3.5f * s, ui::MixU32(IM_COL32(0x55, 0x55, 0x5F, 255), k.ok, onT));
        ui::PushText(2, 13.0f);
        dl->AddText(ImVec2(lamp.x + 3.5f * s + 7.0f * s, a.y + 12.0f * s), k.text, ui::Caps(nodes[i].name));
        ui::PopFont();
        ui::PushNum(12.0f);
        dl->AddText(ImVec2(a.x + 16.0f * s, a.y + 12.0f * s + 20.0f * s), k.muted, nodes[i].sub.c_str());
        ui::PopFont();
        if (!nodes[i].on) dl->AddRectFilled(a, b, ui::WithAlpha(k.card, 0.3f), r);
        ImGui::PopID();
        x += nodeW[i];
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(scroll ? need + pad : avail, nodeH));
    if (scroll) {
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ui::EndCard();
}

// --- Reverb ------------------------------------------------------------------------------

void DecayGraph(const ConfigValues& v) {
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 150.0f * s;
    const ImVec2 q(p.x + w, p.y + h);
    const float r = k.rack ? 3.0f * s : 12.0f * s;
    dl->AddRectFilled(p, q, k.bg, r);
    dl->PushClipRect(p, q, true);
    auto X = [&](float vx) { return p.x + vx / 300.0f * w; };
    auto Y = [&](float vy) { return p.y + vy / 150.0f * h; };
    dl->AddLine(ImVec2(p.x, Y(75)), ImVec2(q.x, Y(75)), k.line);
    dl->AddLine(ImVec2(X(100), p.y), ImVec2(X(100), q.y), k.line);
    dl->AddLine(ImVec2(X(200), p.y), ImVec2(X(200), q.y), k.line);

    const float t60 = 0.25f + v.reverbDecay * 3.0f + v.reverbRoomSize * 0.8f;
    const float pre = v.reverbPreDelayMs / 1000.0f;
    for (int e = 0; e < 8; ++e) {
        const float hh = (0.9f - e * 0.09f) * v.reverbEarlyLevel * 110.0f * (0.5f + v.reverbDiffusion * 0.5f);
        const float ex = pre * 120.0f + 4.0f + e * 5.0f + (e % 3) * 2.0f;
        dl->AddRectFilled(ImVec2(X(ex), Y(145.0f - hh)), ImVec2(X(ex) + (std::max)(2.0f, 2.0f / 300.0f * w), Y(145.0f)),
                          ui::WithAlpha(k.acc, 0.55f));
    }
    ImVec2 pts[51];
    for (int j = 0; j <= 50; ++j) {
        const float ts = j / 50.0f * 2.5f;
        float amp = ts < pre ? 0.0f
                             : std::exp(-6.9f * (ts - pre) / t60) * (0.35f + v.reverbMix * 0.65f) * v.reverbLateLevel;
        amp = (std::min)(amp, 1.3f);
        pts[j] = ImVec2(X(j * 6.0f), Y(145.0f - amp * 115.0f));
    }
    for (int j = 0; j < 50; ++j)
        dl->AddQuadFilled(pts[j], pts[j + 1], ImVec2(pts[j + 1].x, q.y), ImVec2(pts[j].x, q.y), k.accSoft);
    dl->AddPolyline(pts, 51, k.acc, ImDrawFlags_RoundCornersAll, 2.5f * s);
    dl->PopClipRect();
    dl->AddRect(p, q, k.line, r);

    // Legend and RT60.
    float lx = p.x + 12.0f * s;
    const float ly = p.y + 10.0f * s;
    ui::PushNum(11.0f);
    const float th = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(lx, ly + (th - 11.0f * s) * 0.5f), ImVec2(lx + 5.0f * s, ly + (th + 11.0f * s) * 0.5f),
                      ui::WithAlpha(k.acc, 0.55f), 1.0f);
    lx += 11.0f * s;
    dl->AddText(ImVec2(lx, ly), k.muted, "Early reflections");
    lx += ImGui::CalcTextSize("Early reflections").x + 12.0f * s;
    dl->AddRectFilled(ImVec2(lx, ly + th * 0.5f - s), ImVec2(lx + 16.0f * s, ly + th * 0.5f + s), k.acc, 1.0f);
    lx += 22.0f * s;
    dl->AddText(ImVec2(lx, ly), k.muted, "Reverb tail");
    char rt[24];
    std::snprintf(rt, sizeof(rt), "RT60 %.1f s", t60);
    dl->AddText(ImVec2(q.x - 12.0f * s - ImGui::CalcTextSize(rt).x, ly), k.acc, rt);
    ui::PopFont();
    ImGui::Dummy(ImVec2(w, h));

    // Time axis.
    const ImVec2 ap = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(ap.x, ap.y + 6.0f * s - ImGui::GetStyle().ItemSpacing.y));
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    MonoAt(dl, ImVec2(lp.x + 2.0f * s, lp.y), k.muted, "0 s");
    ui::PushNum(11.0f);
    const float mw = ImGui::CalcTextSize("1.25 s").x;
    ui::PopFont();
    MonoAt(dl, ImVec2(lp.x + (w - mw) * 0.5f, lp.y), k.muted, "1.25 s");
    MonoAt(dl, ImVec2(lp.x + w - 2.0f * s, lp.y), k.muted, "2.5 s", true);
    ui::PushNum(11.0f);
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
    ui::PopFont();
}

void ReverbCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    ui::BeginCard("##reverb", "Reverb", "Adds a room around the sound.", HeaderToggleWidth(), "revOn");
    if (HeaderToggle("##revon", &w.enableReverb)) {
        doc.MarkDirty();
        PushLiveBool(C::SetReverbEnabled, w.enableReverb);
    }
    ui::CardHeaderEnd();
    Dim dim;
    dim.Begin("##revdim");
    DecayGraph(w);
    ImGui::Dummy(ImVec2(0.0f, 16.0f * S() - ImGui::GetStyle().ItemSpacing.y));
    KnobGroup(doc, kRevMain, 4);
    dim.End(w.enableReverb);
    ui::EndCard();

    if (ui::Disclosure("rev", "Fine-tune the reverb", "Width, early/late balance, diffusion, damping, modulation, tone")) {
        ui::BeginReveal("rev");
        ui::BeginCard("##revmore");
        KnobGroup(doc, kRevMore, 9);
        ui::EndCard();
        ui::EndReveal();
    }
}

// --- Master limiter ------------------------------------------------------------------------

// Gain-reduction samples, one per frame with their time, so the trace scrolls
// at the display rate instead of in 30 Hz telemetry steps.
struct GrHistory {
    std::deque<std::pair<double, float>> samples;
    void Push(double now, float gr) {
        if (!samples.empty() && now - samples.back().first > 0.25) {
            samples.emplace_back(samples.back().first + 0.02, 0.0f);
            samples.emplace_back(now - 0.02, 0.0f);
        }
        samples.emplace_back(now, gr);
        while (!samples.empty() && now - samples.front().first > 10.5) samples.pop_front();
    }
    float Max() const {
        float m = 0.0f;
        for (const auto& s : samples) m = (std::max)(m, s.second);
        return m;
    }
};
GrHistory g_gr;

float GrDisplayMax(float observed) {
    if (!std::isfinite(observed) || observed <= 24.0f) return 24.0f;
    return (std::min)(240.0f, std::ceil(observed / 24.0f) * 24.0f);
}

void GrGraph(float dmax) {
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 120.0f * s;
    const ImVec2 q(p.x + w, p.y + h);
    const float r = k.rack ? 3.0f * s : 12.0f * s;
    dl->AddRectFilled(p, q, k.bg, r);
    dl->PushClipRect(p, q, true);
    for (int i = 1; i < 4; ++i) {
        const float gy = std::floor(p.y + h * i / 4.0f) + 0.5f;
        dl->AddLine(ImVec2(p.x, gy), ImVec2(q.x, gy), k.line);
    }
    const double now = ImGui::GetTime();
    const auto& hist = g_gr.samples;
    if (hist.size() >= 2) {
        auto px = [&](double t) { return q.x - static_cast<float>((now - t) / 10.0) * w; };
        auto py = [&](float g) { return p.y + (std::min)(dmax, g) / dmax * (h - 3.0f * s) + 1.5f * s; };
        std::deque<ImVec2> pts;
        for (const auto& smp : hist) pts.emplace_back(px(smp.first), py(smp.second));
        pts.emplace_back(q.x, pts.back().y);   // hold the newest value up to "now"
        for (size_t i = 1; i < pts.size(); ++i)
            dl->AddQuadFilled(pts[i - 1], pts[i], ImVec2(pts[i].x, p.y), ImVec2(pts[i - 1].x, p.y),
                              ui::WithAlpha(k.bad, 0.22f));
        for (const ImVec2& pt : pts) dl->PathLineTo(pt);
        dl->PathStroke(k.bad, 0, 2.0f * s);
    }
    dl->PopClipRect();
    dl->AddRect(p, q, k.line, r);
    const char* names[4] = {"0 dB", "", "", ""};
    char lab[4][16];
    std::snprintf(lab[0], 16, "0 dB");
    for (int i = 1; i < 4; ++i) std::snprintf(lab[i], 16, "%s%.0f", kMinus, dmax * i / 4.0f);
    (void)names;
    for (int i = 0; i < 4; ++i)
        MonoAt(dl, ImVec2(q.x - 12.0f * s, i == 0 ? p.y + 8.0f * s : p.y + h * i / 4.0f - 5.0f * s), k.muted, lab[i], true);
    ImGui::Dummy(ImVec2(w, h));

    const ImVec2 ap = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(ap.x, ap.y + 6.0f * s - ImGui::GetStyle().ItemSpacing.y));
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    MonoAt(dl, ImVec2(lp.x + 2.0f * s, lp.y), k.muted, "10 s ago");
    MonoAt(dl, ImVec2(lp.x + w - 2.0f * s, lp.y), k.muted, "now", true);
    ui::PushNum(11.0f);
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
    ui::PopFont();
}

void LimiterCard(ConfigDocument& doc, const SVMS_TelemetryV3* t) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ui::BeginCard("##limiter", "Master limiter", "Keeps the final output under the ceiling.", HeaderToggleWidth(), "limOn");
    if (HeaderToggle("##limon", &w.limiterEnabled)) {
        doc.MarkDirty();
        PushLiveBool(C::SetLimiterEnabled, w.limiterEnabled);
    }
    ui::CardHeaderEnd();
    Dim dim;
    dim.Begin("##limdim");

    static const char* modes[2] = {"Classic", "Adaptive"};
    int mode = static_cast<int>((std::min)(1u, w.limiterAlgorithm));
    ui::BeginRow("limMode", "Mode",
                 mode ? "Adaptive follows the material, so dense passages pump less. Recommended."
                      : "Classic is a fixed, predictable brickwall limiter.",
                 ui::SegmentedSize(modes, 2));
    if (ui::Segmented("##limmode", &mode, modes, 2)) {
        w.limiterAlgorithm = static_cast<uint32_t>(mode);
        doc.MarkDirty();
        PushLiveLimiterAlgorithm(w.limiterAlgorithm);
    }
    ui::EndRow();

    ImGui::Dummy(ImVec2(0.0f, 8.0f * s - ImGui::GetStyle().ItemSpacing.y));
    {
        static const ui::KnobSpec ceiling = {"Ceiling", -20.0f, 0.0f, -0.4f, 0.1f, false, "%.1f dB"};
        static const ui::KnobSpec look = {"Lookahead", 0.0f, 20.0f, 3.0f, 0.1f, false, "%.1f ms"};
        static const ui::KnobSpec attack = {"Attack", 0.01f, 100.0f, 0.5f, 0.01f, true, "%.1f ms"};
        static const ui::KnobSpec release = {"Release", 1.0f, 5000.0f, 100.0f, 1.0f, true, "%.0f ms"};
        KnobRow(4, [&](int i, ImVec2 pos) {
            const ImVec2 ks = ui::KnobSize();
            static const char* ids[4] = {"limThresh", "limLook", "limAtk", "limRel"};
            ui::Anchor(ids[i], pos, ImVec2(pos.x + ks.x, pos.y + ks.y));
            if (i == 0) {
                float db = LinearToDb(w.limiterThreshold);
                if (ui::Knob(ids[i], &db, ceiling)) {
                    w.limiterThreshold = std::pow(10.0f, db / 20.0f);
                    doc.MarkDirty();
                    PushLiveFloat(C::SetLimiterThreshold, w.limiterThreshold);
                }
            } else {
                float* val = i == 1 ? &w.limiterLookaheadMs : i == 2 ? &w.limiterAttackMs : &w.limiterReleaseMs;
                const C cmd = i == 1 ? C::SetLimiterLookahead : i == 2 ? C::SetLimiterAttack : C::SetLimiterRelease;
                if (ui::Knob(ids[i], val, i == 1 ? look : i == 2 ? attack : release)) {
                    doc.MarkDirty();
                    PushLiveFloat(cmd, *val);
                }
            }
        });
    }

    const float gr = t ? (std::max)(0.0f, std::isfinite(t->limiter_gain_reduction_db) ? t->limiter_gain_reduction_db : 0.0f) : 0.0f;
    const float dmax = GrDisplayMax(g_gr.Max());
    {
        ImGui::Dummy(ImVec2(0.0f, 16.0f * s - ImGui::GetStyle().ItemSpacing.y));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ui::PushText(0, 13.0f);
        const float lh = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddText(p, k.muted, "Gain reduction, last 10 seconds");
        ui::PopFont();
        char txt[24];
        if (gr > 0.05f) std::snprintf(txt, sizeof(txt), "%s%.1f dB", kMinus, gr);
        else std::snprintf(txt, sizeof(txt), "0.0 dB");
        ui::PushNum(13.0f);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + width - ImGui::CalcTextSize(txt).x, p.y), k.text, txt);
        ui::PopFont();
        ImGui::Dummy(ImVec2(width, lh + 8.0f * s));
    }
    GrGraph(dmax);
    {
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, h = 10.0f * s;
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), k.card2, h * 0.5f);
        const float fill = Clamp(gr / dmax, 0.0f, 1.0f) * width;
        if (fill > 0.5f)
            dl->AddRectFilled(ImVec2(p.x + width - fill, p.y), ImVec2(p.x + width, p.y + h), k.bad, h * 0.5f);
        ImGui::Dummy(ImVec2(width, h));
    }
    dim.End(w.limiterEnabled);
    ui::EndCard();
}

// --- Channel limiter -----------------------------------------------------------------------

void ChannelLimiterCard(ConfigDocument& doc, const SVMS_TelemetryV3* t) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    auto pushAll = [&]() {
        PushLiveChannelLimiter(w.channelLimiterEnabled, w.channelLimiterThreshold, w.channelLimiterReleaseMs);
    };
    ui::BeginCard("##chlim", "Channel limiter",
                  "Limits each of the 16 MIDI channels on its own, so one loud channel cannot drown the rest.",
                  HeaderToggleWidth(), "chOn");
    if (HeaderToggle("##chon", &w.channelLimiterEnabled)) {
        doc.MarkDirty();
        pushAll();
    }
    ui::CardHeaderEnd();
    Dim dim;
    dim.Begin("##chdim");

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float gap = 24.0f * s;
    const float half = (width - gap) * 0.5f;
    const ImVec2 ks = ui::KnobSize();
    ui::PushNum(11.0f);
    const float rowH = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float rowGap = 6.0f * s;
    const float gridH = 8 * rowH + 7 * rowGap;
    const float height = (std::max)(gridH, ks.y);

    // Left: two knobs, centred in the half.
    {
        static const ui::KnobSpec ceiling = {"Ceiling", -30.0f, 0.0f, -6.0f, 0.1f, false, "%.1f dB"};
        static const ui::KnobSpec release = {"Release", 20.0f, 1000.0f, 150.0f, 1.0f, true, "%.0f ms"};
        const float slot = half / 2.0f;
        const float y = origin.y + (height - ks.y) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + (slot - ks.x) * 0.5f, y));
        ui::Anchor("chThresh", ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + ks.x, y + ks.y));
        float db = LinearToDb(w.channelLimiterThreshold);
        if (ui::Knob("chThresh", &db, ceiling)) {
            w.channelLimiterThreshold = std::pow(10.0f, db / 20.0f);
            doc.MarkDirty();
            pushAll();
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x + slot + (slot - ks.x) * 0.5f, y));
        ui::Anchor("chRel", ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + ks.x, y + ks.y));
        if (ui::Knob("chRel", &w.channelLimiterReleaseMs, release)) {
            doc.MarkDirty();
            pushAll();
        }
    }

    // Right: 16 channel bars (peak level), two columns, row by row.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float colW = (half - 20.0f * s) * 0.5f;
        const float gx = origin.x + half + gap;
        const float gy = origin.y + (height - gridH) * 0.5f;
        const bool active = t && t->channel_limiter_enabled != 0u;
        for (int i = 0; i < 16; ++i) {
            const float cx = gx + (i % 2) * (colW + 20.0f * s);
            const float cy = gy + (i / 2) * (rowH + rowGap);
            char n[8];
            std::snprintf(n, sizeof(n), "%d", i + 1);
            ui::PushNum(11.0f);
            const float labelW = 16.0f * s;
            dl->AddText(ImVec2(cx + labelW - ImGui::CalcTextSize(n).x, cy), k.muted, n);
            ui::PopFont();
            const float bx = cx + labelW + 8.0f * s, bw = colW - labelW - 8.0f * s, bh = 8.0f * s;
            const float by = cy + (rowH - bh) * 0.5f;
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), k.card2, bh * 0.5f);
            if (t && active) {
                const float db = LinearToDb(t->channel_limiter_input_peak[i]);
                const float a = Clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
                if (a > 0.0f) dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * a, by + bh), k.acc, bh * 0.5f);
            }
        }
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    NoteLine("Costs a little CPU when on (about 5 to 13% on dense material). Off sounds exactly like before.", 12.0f);
    dim.End(w.channelLimiterEnabled);
    ui::EndCard();
}

} // namespace

void DrawEffectsPage(ConfigDocument& doc, std::string& panel) {
    const LiveLinkContext& live = GetLiveLinkContext();
    const SVMS_TelemetryV3* t = live.connected ? live.telemetry : nullptr;
    g_gr.Push(ImGui::GetTime(), t ? (std::max)(0.0f, std::isfinite(t->limiter_gain_reduction_db) ? t->limiter_gain_reduction_db : 0.0f) : 0.0f);

    ChainCard(panel, doc.Working(), live.connected);
    if (panel == "limiter") LimiterCard(doc, t);
    else if (panel == "chlim") ChannelLimiterCard(doc, t);
    else ReverbCard(doc);
}

} // namespace svms::cfg
