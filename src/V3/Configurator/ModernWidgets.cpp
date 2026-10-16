#include "ModernWidgets.h"

#include "Anim.h"
#include "SettingsRegistry.h"
#include "Theme.h"
#include "Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace svms::cfg::ui {
namespace {

constexpr float kPi = 3.14159265358979f;

Tokens g_tok{};
float g_scale = 1.0f;

float S(float px) { return px * g_scale; }
double Now() { return ImGui::GetTime(); }

ImU32 Hex(unsigned rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                    static_cast<int>(a * 255.0f + 0.5f));
}
ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// Text helpers ----------------------------------------------------------------

ImFont* FontFor(int weight, bool mono) {
    const UiFonts& f = Fonts();
    ImFont* pick = nullptr;
    if (mono || g_tok.rack) {
        pick = weight >= 1 && f.monoMedium ? f.monoMedium : f.mono;
    } else {
        switch (weight) {
            case 0: pick = f.ui; break;
            case 1: pick = f.medium; break;
            case 2: pick = f.semibold; break;
            default: pick = f.bold ? f.bold : f.semibold; break;
        }
    }
    return pick ? pick : ImGui::GetIO().FontDefault;
}

void Text(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text) {
    dl->AddText(pos, col, text);
}

void HandCursorIfHovered() {
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

// Entry / reveal stack ------------------------------------------------------------

struct OffsetFrame {
    float offset;
    bool pushedAlpha;
};
std::vector<OffsetFrame> g_offsets;
double g_pageStart = -10.0;
int g_entryIndex = 0;
int g_entryDepth = 0;

void PushOffset(float offset, float alpha) {
    const bool push = alpha < 0.999f;
    if (offset != 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
    if (push) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (std::max)(alpha, 0.0f));
    g_offsets.push_back({offset, push});
}

void PopOffset() {
    if (g_offsets.empty()) return;
    const OffsetFrame f = g_offsets.back();
    g_offsets.pop_back();
    if (f.pushedAlpha) ImGui::PopStyleVar();
    if (f.offset != 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - f.offset);
}

// Flash ------------------------------------------------------------------------

std::string g_flashAnchor;
std::string g_flashLegacy;
double g_flashStart = -10.0;
bool g_flashPending = false;   // not yet scrolled into view
int g_flashMatchFrame = -1;
ImGuiWindow* g_scrollHost = nullptr;
constexpr float kFlashSeconds = 1.8f;

void DrawFlash(ImVec2 min, ImVec2 max) {
    const float t = static_cast<float>((Now() - g_flashStart) / kFlashSeconds);
    if (t >= 1.0f || GetMotion() == Motion::Off) return;
    const float k = t < 0.4f ? 1.0f : 1.0f - EaseOut((t - 0.4f) / 0.6f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = S(10.0f);
    min.x -= S(8.0f); max.x += S(8.0f);
    SoftShadow(dl, min, max, r, WithAlpha(g_tok.accSoft, k), S(24.0f), 8);
    dl->AddRectFilled(min, max, WithAlpha(g_tok.accSoft, k), r);
    dl->AddRect(min, max, WithAlpha(g_tok.acc, k), r, 0, S(2.0f));
}

void MatchFlash(ImVec2 min, ImVec2 max) {
    if (g_flashMatchFrame == ImGui::GetFrameCount()) return;  // first match wins
    g_flashMatchFrame = ImGui::GetFrameCount();
    if (g_flashPending) {
        g_flashPending = false;
        g_flashStart = Now();
        ImGuiWindow* host = g_scrollHost ? g_scrollHost : ImGui::GetCurrentWindow();
        ImGui::ScrollToRectEx(host, ImRect(min, max), ImGuiScrollFlags_AlwaysCenterY);
    }
    DrawFlash(min, max);
}

bool FlashLive() {
    return g_flashPending || Now() - g_flashStart < kFlashSeconds;
}

// Card / row state ------------------------------------------------------------------

struct CardFrame {
    const char* anchor;
    bool headerPending;
    int rows;
};
std::vector<CardFrame> g_cards;

struct RowFrame {
    const char* anchor;
    float x0, x1, top, leftBottom, ctlTop;
    ImVec2 ctlSize;
};
RowFrame g_row{};

// Disclosures / reveals -------------------------------------------------------------

std::unordered_map<std::string, bool> g_disclosures;

// SVG path ---------------------------------------------------------------------------

struct PathParser {
    const char* p;
    bool Number(float& out) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') ++p;
        if (!*p || !(std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '.' || *p == '+'))
            return false;
        char* end = nullptr;
        out = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
        return true;
    }
};

void ArcPoints(std::vector<ImVec2>& pts, ImVec2 a, float rx, float ry, float phiDeg,
               bool large, bool sweep, ImVec2 b) {
    if (rx == 0.0f || ry == 0.0f) { pts.push_back(b); return; }
    const float phi = phiDeg * kPi / 180.0f;
    const float cp = std::cos(phi), sp = std::sin(phi);
    const float dx = (a.x - b.x) * 0.5f, dy = (a.y - b.y) * 0.5f;
    const float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    rx = std::fabs(rx); ry = std::fabs(ry);
    float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lam > 1.0f) { const float s = std::sqrt(lam); rx *= s; ry *= s; }
    const float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float co = den > 0.0f ? std::sqrt((std::max)(0.0f, num / den)) : 0.0f;
    if (large == sweep) co = -co;
    const float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    const float cx = cp * cxp - sp * cyp + (a.x + b.x) * 0.5f;
    const float cy = sp * cxp + cp * cyp + (a.y + b.y) * 0.5f;
    auto ang = [](float ux, float uy, float vx, float vy) {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const float t1 = ang(1.0f, 0.0f, (x1 - cxp) / rx, (y1 - cyp) / ry);
    float dt = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
    if (!sweep && dt > 0.0f) dt -= 2.0f * kPi;
    if (sweep && dt < 0.0f) dt += 2.0f * kPi;
    const int n = (std::max)(4, static_cast<int>(std::fabs(dt) / (kPi / 12.0f)));
    for (int i = 1; i <= n; ++i) {
        const float t = t1 + dt * static_cast<float>(i) / static_cast<float>(n);
        const float ex = rx * std::cos(t), ey = ry * std::sin(t);
        pts.push_back(ImVec2(cp * ex - sp * ey + cx, sp * ex + cp * ey + cy));
    }
}

} // namespace

// -----------------------------------------------------------------------------
const Tokens& Tok() { return g_tok; }
float Scale() { return g_scale; }

void BeginFrame(float uiScale) {
    g_scale = uiScale > 0.0f ? uiScale : 1.0f;
    const ThemeSettings& t = GetThemeSettings();
    SetMotion(static_cast<Motion>((std::max)(0, (std::min)(2, t.motion))));
    Tokens k{};
    k.rack = t.style == kStyleRack;
    k.accV = t.accent;
    k.acc = U32(ImVec4(t.accent.x, t.accent.y, t.accent.z, 1.0f));
    k.accSoft = WithAlpha(k.acc, 0.16f);
    k.accLine = WithAlpha(k.acc, 0.45f);
    k.onAcc = Hex(0x1A1206);
    k.ok = Hex(0x5BD69A);
    k.bad = Hex(0xF58E99);
    k.cpu = Hex(0x62CEDB);
    k.exp = Hex(0xBBA2FF);
    if (k.rack) {
        k.bg = U32(t.background);
        k.side = U32(t.sidebar);
        k.card = U32(t.panel);
        k.card2 = U32(t.control);
        k.card3 = MixU32(U32(t.control), U32(t.text), 0.08f);
        k.line = U32(GetPanelEdge());
        k.line2 = MixU32(U32(t.panel), U32(t.text), 0.18f);
        k.text = U32(t.text);
        k.muted = U32(t.mutedText);
        k.rCard = 4.0f; k.rCtl = 3.0f; k.rThumb = 2.0f;
    } else {
        k.bg = Hex(0x0E0E11);
        k.side = Hex(0x131317);
        k.card = Hex(0x19191E);
        k.card2 = Hex(0x222229);
        k.card3 = Hex(0x2A2A33);
        k.line = Hex(0x2A2A32);
        k.line2 = Hex(0x3A3A44);
        k.text = Hex(0xECECF1);
        k.muted = Hex(0xA3A3B0);
        k.rCard = 14.0f; k.rCtl = 10.0f; k.rThumb = 8.0f;
    }
    g_tok = k;
    g_scrollHost = nullptr;
    g_entryIndex = 0;   // stagger order restarts every frame
    g_entryDepth = 0;
    g_offsets.clear();
}

void PushText(int weight, float px) { ImGui::PushFont(FontFor(weight, false), S(px)); }
void PushNum(float px, bool medium) { ImGui::PushFont(FontFor(medium ? 1 : 0, true), S(px)); }
void PopFont() { ImGui::PopFont(); }

const char* Caps(const char* text) {
    if (!g_tok.rack || !text) return text;
    static char buf[4][256];
    static int slot = 0;
    char* out = buf[slot = (slot + 1) % 4];
    size_t i = 0;
    for (; text[i] && i < sizeof(buf[0]) - 1; ++i)
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    out[i] = '\0';
    return out;
}

ImU32 WithAlpha(ImU32 col, float alpha) {
    const float a = ((col >> IM_COL32_A_SHIFT) & 0xFF) * (std::max)(0.0f, (std::min)(1.0f, alpha));
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 MixU32(ImU32 a, ImU32 b, float t) {
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return U32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                      x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

void SoftShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                ImU32 color, float spread, int layers) {
    const float baseA = ((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    if (baseA <= 0.0f || layers <= 0) return;
    for (int i = layers - 1; i >= 0; --i) {
        const float f = static_cast<float>(i + 1) / static_cast<float>(layers);
        const float e = spread * f;
        const float a = baseA * (1.0f - f) * (1.0f - f) * 2.5f / static_cast<float>(layers);
        dl->AddRectFilled(ImVec2(min.x - e, min.y - e), ImVec2(max.x + e, max.y + e),
                          WithAlpha(color | IM_COL32_A_MASK, a), rounding + e);
    }
}

void Glow(ImDrawList* dl, ImVec2 c, float radius, ImU32 color, float spread, int layers) {
    const float baseA = ((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    if (baseA <= 0.0f || layers <= 0) return;
    for (int i = layers - 1; i >= 0; --i) {
        const float f = static_cast<float>(i + 1) / static_cast<float>(layers);
        const float a = baseA * (1.0f - f) * (1.0f - f) * 2.5f / static_cast<float>(layers);
        dl->AddCircleFilled(c, radius + spread * f, WithAlpha(color | IM_COL32_A_MASK, a), 32);
    }
}

void DashedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                ImU32 color, float dash, float gap) {
    dl->PathRect(min, max, rounding);
    std::vector<ImVec2> pts(dl->_Path.Data, dl->_Path.Data + dl->_Path.Size);
    dl->PathClear();
    if (pts.size() < 2) return;
    pts.push_back(pts.front());
    float carry = 0.0f;   // distance into the current dash+gap period
    const float period = dash + gap;
    for (size_t i = 1; i < pts.size(); ++i) {
        const ImVec2 a = pts[i - 1], b = pts[i];
        const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        float pos = 0.0f;
        while (pos < len) {
            const float inPeriod = std::fmod(carry + pos, period);
            const bool on = inPeriod < dash;
            const float step = (std::min)(len - pos, on ? dash - inPeriod : period - inPeriod);
            if (on && step > 0.0f) {
                const float t0 = pos / len, t1 = (pos + step) / len;
                dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0),
                            ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1), color, 1.0f);
            }
            pos += (std::max)(step, 0.01f);
        }
        carry = std::fmod(carry + len, period);
    }
}

void StrokeIcon(ImDrawList* dl, const char* path, ImVec2 pos, float size, ImU32 color,
                float thickness, float rotateRad, float scale) {
    const float k = size / 24.0f * scale;
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float cr = std::cos(rotateRad), sr = std::sin(rotateRad);
    auto map = [&](ImVec2 p) {
        const float x = (p.x - 12.0f) * k, y = (p.y - 12.0f) * k;
        return ImVec2(c.x + x * cr - y * sr, c.y + x * sr + y * cr);
    };
    std::vector<ImVec2> pts;
    ImVec2 cur(0, 0), start(0, 0);
    auto flush = [&](bool closed) {
        if (pts.size() >= 2) {
            for (const ImVec2& q : pts) dl->PathLineTo(map(q));
            dl->PathStroke(color, closed ? ImDrawFlags_Closed : 0, thickness);
        }
        pts.clear();
    };
    PathParser ps{path};
    char cmd = 0;
    while (*ps.p) {
        while (*ps.p == ' ' || *ps.p == ',') ++ps.p;
        if (!*ps.p) break;
        if (std::isalpha(static_cast<unsigned char>(*ps.p))) cmd = *ps.p++;
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        float v[7];
        switch (std::toupper(static_cast<unsigned char>(cmd))) {
            case 'M':
                if (!ps.Number(v[0]) || !ps.Number(v[1])) { ++ps.p; break; }
                flush(false);
                cur = rel ? ImVec2(cur.x + v[0], cur.y + v[1]) : ImVec2(v[0], v[1]);
                start = cur;
                pts.push_back(cur);
                cmd = rel ? 'l' : 'L';   // implicit lineto after moveto
                break;
            case 'L':
                if (!ps.Number(v[0]) || !ps.Number(v[1])) { ++ps.p; break; }
                cur = rel ? ImVec2(cur.x + v[0], cur.y + v[1]) : ImVec2(v[0], v[1]);
                pts.push_back(cur);
                break;
            case 'H':
                if (!ps.Number(v[0])) { ++ps.p; break; }
                cur.x = rel ? cur.x + v[0] : v[0];
                pts.push_back(cur);
                break;
            case 'V':
                if (!ps.Number(v[0])) { ++ps.p; break; }
                cur.y = rel ? cur.y + v[0] : v[0];
                pts.push_back(cur);
                break;
            case 'A': {
                bool ok = true;
                for (int i = 0; i < 7 && ok; ++i) ok = ps.Number(v[i]);
                if (!ok) { ++ps.p; break; }
                const ImVec2 to = rel ? ImVec2(cur.x + v[5], cur.y + v[6]) : ImVec2(v[5], v[6]);
                if (pts.empty()) pts.push_back(cur);
                ArcPoints(pts, cur, v[0], v[1], v[2], v[3] != 0.0f, v[4] != 0.0f, to);
                cur = to;
                break;
            }
            case 'Z':
                flush(true);
                cur = start;
                break;
            default:
                ++ps.p;   // unsupported command: skip
                break;
        }
    }
    flush(false);
}

// Entry ----------------------------------------------------------------------------

void PageEntered() { g_pageStart = Now(); }

void BeginEntry() {
    ++g_entryDepth;
    float offset = 0.0f, alpha = 1.0f;
    if (g_entryDepth == 1 && GetMotion() == Motion::Full) {
        const int index = g_entryIndex++;
        const float t = static_cast<float>((Now() - g_pageStart - index * 0.04) / 0.5);
        if (t < 1.0f) {
            const float e = EaseOut(t);
            offset = S(12.0f) * (1.0f - e);
            alpha = e;
        }
    }
    PushOffset(offset, alpha);
}

void EndEntry() {
    PopOffset();
    if (g_entryDepth > 0) --g_entryDepth;
}

// Flash ------------------------------------------------------------------------------

void FlashAnchor(const char* anchor, const char* legacyCaption) {
    g_flashAnchor = anchor ? anchor : "";
    g_flashLegacy = legacyCaption ? legacyCaption : "";
    g_flashPending = true;
    g_flashStart = Now();
}

const char* FlashTarget() { return FlashLive() ? g_flashAnchor.c_str() : ""; }

void SetScrollHost() { g_scrollHost = ImGui::GetCurrentWindow(); }

void Anchor(const char* anchor, ImVec2 min, ImVec2 max) {
    if (!anchor || !FlashLive() || g_flashAnchor != anchor) return;
    MatchFlash(min, max);
}

void LegacyAnchor(const char* caption, ImVec2 min, ImVec2 max) {
    if (!caption || g_flashLegacy.empty() || !FlashLive() || g_flashLegacy != caption) return;
    MatchFlash(min, max);
}

// Cards ------------------------------------------------------------------------------

void BeginCard(const char* id, const char* title, const char* subtitle,
               float rightSlotW, const char* anchor) {
    const Tokens& k = g_tok;
    g_cards.push_back({anchor, rightSlotW > 0.0f, 0});
    if (k.rack) {
        BeginRackPanel(title ? Caps(title) : id);
    } else {
        BeginEntry();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, k.card);
        ImGui::PushStyleColor(ImGuiCol_Border, k.line);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(k.rCard));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24.0f), S(22.0f)));
        ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }
    if (!title && !subtitle && rightSlotW <= 0.0f) return;

    const float x0 = ImGui::GetCursorPosX();
    const float right = ImGui::GetContentRegionMax().x;
    const float y0 = ImGui::GetCursorPosY();
    float x = x0;
    // With a right slot (38 px buttons) the title centres on it.
    PushText(2, 15.0f);
    const float titleDy = rightSlotW > 0.0f ? (std::max)(0.0f, (S(38.0f) - ImGui::GetTextLineHeight()) * 0.5f) : 0.0f;
    PopFont();
    if (title && !k.rack) {
        PushText(2, 15.0f);
        ImGui::SetCursorPos(ImVec2(x, y0 + titleDy));
        ImGui::TextUnformatted(title);
        x += ImGui::CalcTextSize(title).x + S(10.0f);
        PopFont();
    }
    if (subtitle) {
        PushText(0, 13.0f);
        const float wrapX = right - (rightSlotW > 0.0f ? rightSlotW + S(10.0f) : 0.0f);
        ImGui::SetCursorPos(ImVec2(k.rack ? x0 : x, y0 + (k.rack ? 0.0f : titleDy + S(1.5f))));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(wrapX);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        PopFont();
    }
    const float headerBottom = ImGui::GetCursorPosY();
    if (rightSlotW > 0.0f) {
        ImGui::SetCursorPos(ImVec2(right - rightSlotW, y0));
        g_cards.back().headerPending = true;
        // CardHeaderEnd continues below the taller of header text and slot.
        ImGui::GetStateStorage()->SetFloat(ImGui::GetID("##hdrBottom"), headerBottom);
        return;
    }
    ImGui::SetCursorPosY(headerBottom + S(14.0f) - ImGui::GetStyle().ItemSpacing.y);
}

void CardHeaderEnd() {
    if (g_cards.empty() || !g_cards.back().headerPending) return;
    g_cards.back().headerPending = false;
    const float stored = ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##hdrBottom"), 0.0f);
    const float bottom = (std::max)(stored, ImGui::GetItemRectMax().y - ImGui::GetWindowPos().y +
                                                ImGui::GetScrollY());
    ImGui::SetCursorPos(ImVec2(ImGui::GetCurrentWindow()->WindowPadding.x,
                               bottom + S(14.0f) - ImGui::GetStyle().ItemSpacing.y));
}

void EndCard() {
    if (g_cards.empty()) return;
    const CardFrame card = g_cards.back();
    g_cards.pop_back();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    if (g_tok.rack) {
        EndRackPanel();
    } else {
        ImGui::EndChild();
        EndEntry();
        ImGui::Dummy(ImVec2(0.0f, S(16.0f) - 2.0f * ImGui::GetStyle().ItemSpacing.y));
    }
    Anchor(card.anchor, wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
}

// Chips / restart mark ----------------------------------------------------------------

static ImU32 ChipColor(ChipKind kind) {
    switch (kind) {
        case ChipKind::Cpu: return g_tok.cpu;
        case ChipKind::Bad: return g_tok.bad;
        case ChipKind::Ok:  return g_tok.ok;
        case ChipKind::Exp: return g_tok.exp;
        default:            return g_tok.acc;
    }
}

static ImVec2 ChipSize(const char* text) {
    PushText(1, 11.0f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    PopFont();
    return ImVec2(ts.x + S(20.0f), ts.y + S(6.0f));
}

void Chip(const char* text, ChipKind kind) {
    const ImVec2 size = ChipSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ChipColor(kind);
    const float r = g_tok.rack ? S(3.0f) : size.y * 0.5f;
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y),
                kind == ChipKind::Acc ? g_tok.accLine : WithAlpha(col, 0.4f), r);
    PushText(1, 11.0f);
    Text(dl, ImVec2(p.x + S(10.0f), p.y + S(3.0f)), col, text);
    PopFont();
}

static const char* kRestartIcon = "M3 12a9 9 0 1 0 3-6.7L3 8M3 3v5h5";

void RestartMark(bool withText) {
    const char* label = "Applies after restart";
    PushText(0, 11.0f);
    const float textW = withText ? ImGui::CalcTextSize(label).x + S(4.0f) : 0.0f;
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(S(12.0f) + textW, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    StrokeIcon(dl, kRestartIcon, ImVec2(p.x, p.y + (h - S(12.0f)) * 0.5f), S(12.0f),
               g_tok.muted, S(1.4f));
    if (withText) Text(dl, ImVec2(p.x + S(16.0f), p.y), g_tok.muted, label);
    PopFont();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Applies the next time an app opens SVMS.");
        ImGui::EndTooltip();
    }
}

// Rows ------------------------------------------------------------------------------------

void BeginRow(const char* anchor, const char* title, const char* desc, ImVec2 controlSize,
              const ChipDef* chips, int chipCount) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    const bool first = !g_cards.empty() && g_cards.back().rows++ == 0;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float x1 = origin.x + ImGui::GetContentRegionAvail().x;
    const float padTop = first ? S(2.0f) : S(15.0f);
    if (!first) dl->AddLine(ImVec2(origin.x, origin.y), ImVec2(x1, origin.y), g_tok.line, 1.0f);

    const float leftW = (std::max)(S(120.0f), x1 - origin.x - controlSize.x - S(24.0f));
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + padTop));
    ImGui::BeginGroup();
    PushText(1, 14.0f);
    const float titleH = ImGui::GetTextLineHeight();
    ImGui::TextUnformatted(Caps(title));
    PopFont();
    const SettingDef* def = FindSetting(anchor);
    for (int i = 0; i < chipCount; ++i) {
        ImGui::SameLine(0.0f, S(8.0f));
        const float cy = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(cy + (titleH - ChipSize(chips[i].text).y) * 0.5f);
        Chip(chips[i].text, chips[i].kind);
    }
    if (def && def->restart) {
        ImGui::SameLine(0.0f, S(8.0f));
        PushText(0, 11.0f);
        const float markH = ImGui::GetTextLineHeight();
        PopFont();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (titleH - markH) * 0.5f);
        RestartMark();
    }
    if (desc && *desc) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(2.0f) - st.ItemSpacing.y);
        PushText(0, 13.0f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (std::min)(leftW, S(600.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, g_tok.muted);
        ImGui::TextUnformatted(desc);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        PopFont();
    }
    ImGui::EndGroup();
    const float leftBottom = ImGui::GetItemRectMax().y;
    const float leftH = leftBottom - (origin.y + padTop);

    g_row = {anchor, origin.x, x1, origin.y, leftBottom,
             origin.y + padTop + (std::max)(0.0f, (leftH - controlSize.y) * 0.5f), controlSize};
    ImGui::SetCursorScreenPos(ImVec2(x1 - controlSize.x, g_row.ctlTop));
}

void EndRow() {
    const float bottom = (std::max)(g_row.leftBottom, g_row.ctlTop + g_row.ctlSize.y) + S(15.0f);
    // A zero-height item ends exactly at `bottom` (Dummy adds the spacing).
    ImGui::SetCursorScreenPos(ImVec2(g_row.x0, bottom - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    Anchor(g_row.anchor, ImVec2(g_row.x0, g_row.top), ImVec2(g_row.x1, bottom));
}

// Disclosure ------------------------------------------------------------------------------

bool& DisclosureState(const char* key) { return g_disclosures[key ? key : ""]; }

bool Disclosure(const char* key, const char* title, const char* hint) {
    bool& open = DisclosureState(key);
    ImGui::PushID(key);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    PushText(1, 14.0f);
    const float lineH = ImGui::GetTextLineHeight();
    PopFont();
    const float h = lineH + S(30.0f);
    if (ImGui::InvisibleButton("##disc", ImVec2(w, h))) {
        open = !open;
        if (open) AnimTrigger(ImGui::GetID("##reveal"));
    }
    HandCursorIfHovered();
    const bool hov = ImGui::IsItemHovered();
    const float hv = AnimEase(ImGui::GetID("##hov"), hov ? 1.0f : 0.0f, 0.2f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = S(g_tok.rack ? g_tok.rCtl : g_tok.rCard);
    const ImVec2 q(p.x + w, p.y + h);
    if (hv > 0.0f) dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, static_cast<int>(5 * hv)), r);
    DashedRect(dl, p, q, r, MixU32(g_tok.line2, g_tok.accLine, hv));
    const float rot = AnimSpring(ImGui::GetID("##chev"), open ? 1.0f : 0.0f) * kPi * 0.5f;
    const ImU32 ink = MixU32(g_tok.muted, g_tok.text, hv);
    StrokeIcon(dl, "M9 6l6 6-6 6", ImVec2(p.x + S(20.0f), p.y + (h - S(16.0f)) * 0.5f),
               S(16.0f), ink, S(2.0f), rot);
    PushText(1, 14.0f);
    Text(dl, ImVec2(p.x + S(46.0f), p.y + (h - lineH) * 0.5f), g_tok.text, Caps(title));
    PopFont();
    if (hint && *hint) {
        PushText(0, 12.0f);
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        Text(dl, ImVec2(q.x - S(20.0f) - ts.x, p.y + (h - ts.y) * 0.5f), ink, hint);
        PopFont();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, S(16.0f) - 2.0f * ImGui::GetStyle().ItemSpacing.y));
    return open;
}

void BeginReveal(const char* key) {
    ImGui::PushID(key);
    const float t = AnimPulse(ImGui::GetID("##reveal"), 0.4f);
    ImGui::PopID();
    const float e = EaseOut(t);
    PushOffset(-S(8.0f) * (1.0f - e), t >= 1.0f ? 1.0f : e);
}

void EndReveal() { PopOffset(); }

// Toggle --------------------------------------------------------------------------------

struct ToggleState {
    bool last = false;
    bool visual = false;
    bool init = false;
    double changed = -10.0;
    float delay = 0.0f;
};
static std::unordered_map<ImGuiID, ToggleState> g_toggles;

bool Toggle(const char* id, bool* value, float delay) {
    ImGui::PushID(id);
    const ImVec2 size(S(46.0f), S(28.0f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool changed = false;
    if (ImGui::InvisibleButton("##tog", size)) {
        *value = !*value;
        changed = true;
    }
    HandCursorIfHovered();
    const bool hov = ImGui::IsItemHovered();
    const bool down = ImGui::IsItemActive();
    const ImGuiID gid = ImGui::GetID("##tog");
    ToggleState& s = g_toggles[gid];
    const double now = Now();
    if (!s.init) {
        s = ToggleState{*value, *value, true, -10.0, 0.0f};
    } else if (s.last != *value) {
        s.last = *value;
        s.changed = now;
        s.delay = changed ? 0.0f : delay;   // external changes (profiles) stagger
    }
    const bool visual = now - s.changed < s.delay ? !*value : *value;
    if (visual != s.visual) {
        s.visual = visual;
        AnimTrigger(ImGui::GetID(visual ? "##pop" : "##squish"));
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_tok.rack) {
        DrawRackLever(dl, p, size, visual, hov);
        ImGui::PopID();
        return changed;
    }

    // Off squish: scale 1 -> .94 -> 1 over 0.4 s.
    const float sq = AnimPulse(ImGui::GetID("##squish"), 0.4f);
    const float scale = sq < 1.0f ? 1.0f - 0.06f * std::sin(kPi * sq) : 1.0f;
    const ImVec2 c(p.x + size.x * 0.5f, p.y + size.y * 0.5f);
    const ImVec2 a(c.x - size.x * 0.5f * scale, c.y - size.y * 0.5f * scale);
    const ImVec2 b(c.x + size.x * 0.5f * scale, c.y + size.y * 0.5f * scale);
    const float r = size.y * 0.5f * scale;

    const float on = AnimEase(ImGui::GetID("##fill"), visual ? 1.0f : 0.0f, 0.3f);
    // On pop: a ring that expands 10 px and fades.
    const float pop = AnimPulse(ImGui::GetID("##pop"), 0.5f);
    if (pop < 1.0f) {
        const float e = EaseOut(pop) * S(10.0f);
        dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e),
                    WithAlpha(g_tok.accLine, 1.0f - pop), r + e, 0, S(2.0f));
    }
    dl->AddRectFilled(a, b, MixU32(g_tok.card3, g_tok.acc, on), r);
    dl->AddRect(a, b, MixU32(g_tok.line2, g_tok.acc, on), r);

    const float pos = AnimSpring(ImGui::GetID("##thumb"), visual ? 1.0f : 0.0f);
    const float thumbW = AnimEase(ImGui::GetID("##thumbW"), down ? 26.0f : 20.0f, 0.25f);
    const float leftOff = 4.0f + pos * ((down ? 16.0f : 22.0f) - 4.0f);
    const ImVec2 t0(a.x + S(leftOff) * scale, a.y + S(4.0f) * scale);
    const ImVec2 t1(t0.x + S(thumbW) * scale, t0.y + S(20.0f) * scale);
    SoftShadow(dl, ImVec2(t0.x, t0.y + S(2.0f)), ImVec2(t1.x, t1.y + S(2.0f)), S(10.0f),
               IM_COL32(0, 0, 0, 110), S(4.0f), 4);
    dl->AddRectFilled(t0, t1, MixU32(Hex(0xC9C9D2), IM_COL32_WHITE, on), S(10.0f) * scale);
    (void)hov;
    ImGui::PopID();
    return changed;
}

// Segmented ------------------------------------------------------------------------------

static void SegLayout(const char* const* labels, int count, float width,
                      float& cellW, float& totalW, float& fontScale) {
    PushText(0, 13.0f);
    float maxW = 0.0f;
    for (int i = 0; i < count; ++i)
        maxW = (std::max)(maxW, ImGui::CalcTextSize(Caps(labels[i])).x);
    PopFont();
    // Semibold selected labels are a little wider than regular ones.
    maxW *= 1.04f;
    const float pad = S(3.0f);
    if (width < 0.0f) width = ImGui::GetContentRegionAvail().x;
    if (width > 0.0f) {
        totalW = width;
        cellW = (width - 2.0f * pad) / static_cast<float>((std::max)(count, 1));
    } else {
        cellW = maxW + S(30.0f);
        totalW = cellW * count + 2.0f * pad;
    }
    fontScale = maxW > 0.0f ? (std::min)(1.0f, (cellW - S(8.0f)) / maxW) : 1.0f;
}

ImVec2 SegmentedSize(const char* const* labels, int count, float width) {
    float cellW, totalW, fs;
    SegLayout(labels, count, width, cellW, totalW, fs);
    return ImVec2(totalW, S(34.0f) + 2.0f * S(3.0f) + 2.0f);
}

bool Segmented(const char* id, int* current, const char* const* labels, int count, float width) {
    if (g_tok.rack && width == 0.0f) {
        const char* caps[32];
        const int n = (std::min)(count, 32);
        std::string store[32];
        for (int i = 0; i < n; ++i) { store[i] = Caps(labels[i]); caps[i] = store[i].c_str(); }
        return KeyGroup(id, current, caps, n);
    }
    ImGui::PushID(id);
    float cellW, totalW, fontScale;
    SegLayout(labels, count, width, cellW, totalW, fontScale);
    const float pad = S(3.0f) + 1.0f;
    const float cellH = S(34.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + totalW, p.y + cellH + 2.0f * pad);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, g_tok.card2, S(g_tok.rack ? g_tok.rCtl : 11.0f));
    dl->AddRect(p, q, g_tok.line, S(g_tok.rack ? g_tok.rCtl : 11.0f));

    bool changed = false;
    int pressed = -1;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad + cellW * i, p.y + pad));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##cell", ImVec2(cellW, cellH)) && *current != i) {
            *current = i;
            changed = true;
        }
        HandCursorIfHovered();
        if (ImGui::IsItemActive()) pressed = i;
        ImGui::PopID();
    }
    const int cur = (std::max)(0, (std::min)(count - 1, *current));
    const bool valid = *current >= 0 && *current < count;
    const float x = AnimSpring(ImGui::GetID("##thumb"), static_cast<float>(cur));
    if (valid) {
        const float press = AnimSpring(ImGui::GetID("##thpress"), pressed == cur ? 0.93f : 1.0f);
        const float tx = p.x + pad + x * cellW;
        const ImVec2 c(tx + cellW * 0.5f, p.y + pad + cellH * 0.5f);
        const ImVec2 a(c.x - cellW * 0.5f * press, c.y - cellH * 0.5f * press);
        const ImVec2 b(c.x + cellW * 0.5f * press, c.y + cellH * 0.5f * press);
        if (g_tok.rack) {
            dl->AddRectFilled(a, b, g_tok.card3, S(g_tok.rThumb));
            dl->AddRect(a, b, g_tok.accLine, S(g_tok.rThumb));
            DrawLed(dl, ImVec2(c.x, a.y + S(5.0f)), S(2.0f), true);
        } else {
            SoftShadow(dl, a, ImVec2(b.x, b.y + S(2.0f)), S(8.0f), g_tok.accSoft, S(10.0f), 6);
            dl->AddRectFilled(a, b, g_tok.acc, S(8.0f));
        }
    }
    for (int i = 0; i < count; ++i) {
        const bool sel = valid && i == cur;
        const float press = AnimSpring(ImGui::GetID(i + 1000), pressed == i ? 0.93f : 1.0f);
        ImGui::PushFont(FontFor(sel && !g_tok.rack ? 2 : 0, false), S(13.0f) * fontScale * press);
        const char* label = Caps(labels[i]);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const float cx = p.x + pad + cellW * (i + 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad + cellW * i, p.y + pad));
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(p.x + pad + cellW * i, p.y + pad),
                                                    ImVec2(p.x + pad + cellW * (i + 1), p.y + pad + cellH));
        ImU32 col = hov ? g_tok.text : g_tok.muted;
        if (sel) col = g_tok.rack ? g_tok.acc : g_tok.onAcc;
        dl->PushClipRect(ImVec2(p.x + pad + cellW * i, p.y), ImVec2(p.x + pad + cellW * (i + 1), q.y), true);
        Text(dl, ImVec2(cx - ts.x * 0.5f, p.y + pad + (cellH - ts.y) * 0.5f), col, label);
        dl->PopClipRect();
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(totalW, q.y - p.y));
    ImGui::PopID();
    return changed;
}

// Knob ---------------------------------------------------------------------------------------

static float KnobNorm(const KnobSpec& s, float v) {
    if (s.log && s.min > 0.0f) return std::log(v / s.min) / std::log(s.max / s.min);
    return s.max > s.min ? (v - s.min) / (s.max - s.min) : 0.0f;
}

static float KnobDenorm(const KnobSpec& s, float n) {
    n = (std::max)(0.0f, (std::min)(1.0f, n));
    float v = s.log && s.min > 0.0f ? s.min * std::pow(s.max / s.min, n) : s.min + n * (s.max - s.min);
    if (s.step > 0.0f) v = std::round(v / s.step) * s.step;
    return (std::max)(s.min, (std::min)(s.max, v));
}

static std::unordered_map<ImGuiID, float> g_knobDragN;
static std::unordered_map<ImGuiID, bool> g_knobDirty;
static ImGuiID g_knobKeys = 0;

bool Knob(const char* id, float* value, const KnobSpec& spec, bool* committed) {
    if (committed) *committed = false;
    if (g_tok.rack) {
        ImGui::BeginGroup();
        const bool zero = spec.zeroText && *value <= spec.min;
        bool done = false;
        ImGui::PushID(id);
        const bool changed = PanelKnob(Caps(spec.label), value, spec.min, spec.max, spec.def,
                                       zero ? spec.zeroText : spec.format, S(54.0f), nullptr,
                                       &done, spec.display, nullptr);
        ImGui::PopID();
        ImGui::EndGroup();
        if (committed) *committed = done;
        return changed;
    }
    ImGui::PushID(id);
    const float dial = S(64.0f);
    const float w = S(96.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x + (w - dial) * 0.5f, p.y + S(6.0f)));
    ImGui::InvisibleButton("##dial", ImVec2(dial, dial));
    const ImGuiID gid = ImGui::GetID("##dial");
    const bool hov = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hov || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    bool changed = false;
    float n = (std::max)(0.0f, (std::min)(1.0f, KnobNorm(spec, *value)));
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) {
        g_knobDragN[gid] = n;
        g_knobKeys = gid;
    }
    if (active && io.MouseDelta.y != 0.0f) {
        float& dn = g_knobDragN[gid];
        dn = (std::max)(0.0f, (std::min)(1.0f, dn - io.MouseDelta.y / (io.KeyShift ? S(700.0f) : S(180.0f))));
        const float v = KnobDenorm(spec, dn);
        if (v != *value) { *value = v; changed = true; g_knobDirty[gid] = true; }
    }
    if (ImGui::IsItemDeactivated() && g_knobDirty[gid]) {
        g_knobDirty[gid] = false;
        if (committed) *committed = true;
    }
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (*value != spec.def) { *value = spec.def; changed = true; }
        g_knobDirty[gid] = false;
        if (committed) *committed = true;
    }
    if ((hov || g_knobKeys == gid) && !active && !io.WantTextInput) {
        float target = *value;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) target = KnobDenorm(spec, n + 0.02f);
        else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) target = KnobDenorm(spec, n - 0.02f);
        else if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) target = spec.min;
        else if (ImGui::IsKeyPressed(ImGuiKey_End, false)) target = spec.max;
        if (target != *value) {
            *value = target;
            changed = true;
            if (committed) *committed = true;
        }
    }
    if (!hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g_knobKeys == gid) g_knobKeys = 0;

    // Drawn straight from the value: arc and pointer move together, no lag.
    n = (std::max)(0.0f, (std::min)(1.0f, KnobNorm(spec, *value)));
    const float scale = AnimSpring(ImGui::GetID("##scale"), active ? 0.95f : (hov ? 1.06f : 1.0f));
    const float k = dial / 56.0f * scale;
    const ImVec2 c(p.x + w * 0.5f, p.y + S(6.0f) + dial * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float a0 = 0.75f * kPi;
    const float a1 = a0 + 1.5f * kPi * n;
    dl->PathArcTo(c, 22.0f * k, a0, a0 + 1.5f * kPi, 48);
    dl->PathStroke(g_tok.card3, 0, 4.5f * k);
    if (n > 0.002f) {
        dl->PathArcTo(c, 22.0f * k, a0, a1, 48);
        dl->PathStroke(g_tok.accSoft, 0, 9.0f * k);
        dl->PathArcTo(c, 22.0f * k, a0, a1, 48);
        dl->PathStroke(g_tok.acc, 0, 4.5f * k);
        for (float a : {a0, a1}) dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * 22.0f * k, c.y + std::sin(a) * 22.0f * k), 2.25f * k, g_tok.acc, 12);
    } else {
        dl->AddCircleFilled(ImVec2(c.x + std::cos(a0) * 22.0f * k, c.y + std::sin(a0) * 22.0f * k), 2.25f * k, g_tok.card3, 12);
    }
    dl->AddCircleFilled(c, 15.0f * k, g_tok.card2, 40);
    dl->AddCircle(c, 15.0f * k, g_tok.line2, 40, 1.0f);
    const float pa = a1;
    dl->AddLine(ImVec2(c.x + std::cos(pa) * 6.0f * k, c.y + std::sin(pa) * 6.0f * k),
                ImVec2(c.x + std::cos(pa) * 12.0f * k, c.y + std::sin(pa) * 12.0f * k),
                g_tok.text, 2.6f * k);

    char buf[64];
    if (spec.zeroText && *value <= spec.min) std::snprintf(buf, sizeof(buf), "%s", spec.zeroText);
    else std::snprintf(buf, sizeof(buf), spec.format, spec.display ? spec.display(*value) : *value);
    float y = p.y + S(6.0f) + dial + S(6.0f);
    PushNum(13.0f);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    Text(dl, ImVec2(p.x + (w - ts.x) * 0.5f, y), g_tok.acc, buf);
    y += ts.y + S(1.0f);
    PopFont();
    PushText(0, 12.0f);
    ts = ImGui::CalcTextSize(spec.label);
    Text(dl, ImVec2(p.x + (w - ts.x) * 0.5f, y), g_tok.muted, spec.label);
    y += ts.y;
    PopFont();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, y - p.y + S(6.0f)));
    ImGui::PopID();
    return changed;
}

// Buttons ------------------------------------------------------------------------------------

ImVec2 ButtonSize(const char* label) {
    PushText(1, 13.0f);
    const float tw = ImGui::CalcTextSize(Caps(label), nullptr, true).x;
    PopFont();
    return ImVec2(tw + S(40.0f), S(38.0f));
}

static bool DrawButton(const char* label, ButtonKind kind, ImVec2 size, bool enabled,
                       bool armed, float shakeX) {
    if (size.x <= 0.0f || size.y <= 0.0f) {
        const ImVec2 auto_ = ButtonSize(label);
        if (size.x <= 0.0f) size.x = auto_.x;
        if (size.y <= 0.0f) size.y = auto_.y;
    }
    if (g_tok.rack) {
        const char* caps = Caps(label);
        const std::string shown(caps, ImGui::FindRenderedTextEnd(caps));
        return KeyButton(shown.c_str(), size, kind == ButtonKind::Primary || armed, enabled);
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, size) && enabled;
    if (enabled) HandCursorIfHovered();
    const bool hov = ImGui::IsItemHovered() && enabled;
    const bool down = ImGui::IsItemActive() && enabled;
    const float sc = AnimSpring(ImGui::GetItemID(), down ? 0.95f : 1.0f);
    const ImVec2 c(p.x + size.x * 0.5f + shakeX, p.y + size.y * 0.5f);
    const ImVec2 a(c.x - size.x * 0.5f * sc, c.y - size.y * 0.5f * sc);
    const ImVec2 b(c.x + size.x * 0.5f * sc, c.y + size.y * 0.5f * sc);
    ImU32 bg = hov ? g_tok.card3 : g_tok.card2, border = g_tok.line2, ink = g_tok.text;
    if (kind == ButtonKind::Primary) {
        bg = hov ? MixU32(g_tok.acc, IM_COL32_WHITE, 0.08f) : g_tok.acc;
        border = g_tok.acc;
        ink = g_tok.onAcc;
    } else if (kind == ButtonKind::Danger) {
        ink = g_tok.bad;
        border = WithAlpha(g_tok.bad, 0.4f);
        if (armed) { bg = g_tok.bad; border = g_tok.bad; ink = Hex(0x1F0A0D); }
    }
    if (!enabled) { bg = MixU32(bg, g_tok.card, 0.5f); ink = WithAlpha(ink, 0.45f); }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, bg, S(10.0f) * sc);
    dl->AddRect(a, b, border, S(10.0f) * sc);
    ImGui::PushFont(FontFor(1, false), S(13.0f) * sc);
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), ink, label,
                ImGui::FindRenderedTextEnd(label));
    ImGui::PopFont();
    return clicked;
}

bool Button(const char* label, ButtonKind kind, ImVec2 size, bool enabled) {
    return DrawButton(label, kind, size, enabled, false, 0.0f);
}

static std::unordered_map<ImGuiID, double> g_armed;

bool DangerButton(const char* id, const char* label, const char* armedLabel) {
    ImGui::PushID(id);
    const ImGuiID key = ImGui::GetID("##armed");
    const double now = Now();
    auto it = g_armed.find(key);
    const bool armed = it != g_armed.end() && now - it->second < 3.0;
    const ImVec2 a = ButtonSize(label), b = ButtonSize(armedLabel);
    const ImVec2 size((std::max)(a.x, b.x), a.y);
    float shake = 0.0f;
    const float t = AnimPulse(ImGui::GetID("##shake"), 0.4f);
    if (t < 1.0f) shake = -S(3.0f) * std::sin(2.0f * kPi * t);
    char shown[256];
    std::snprintf(shown, sizeof(shown), "%s##btn", armed ? armedLabel : label);
    bool fired = false;
    if (DrawButton(shown, ButtonKind::Danger, size, true, armed, shake)) {
        if (armed) {
            g_armed.erase(key);
            fired = true;
        } else {
            g_armed[key] = now;
            AnimTrigger(ImGui::GetID("##shake"));
        }
    }
    ImGui::PopID();
    return fired;
}

// Fields / slider -----------------------------------------------------------------------------

static void PushFieldStyle(float height) {
    const float fontH = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14.0f), (std::max)(0.0f, (height - fontH) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(g_tok.rCtl));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, g_tok.card2);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, g_tok.card2);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, g_tok.card2);
    ImGui::PushStyleColor(ImGuiCol_Border, g_tok.line2);
}

static void PopFieldStyle() {
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
}

static void FocusRing() {
    if (!ImGui::IsItemActive() || g_tok.rack) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRect(ImVec2(a.x - S(2.0f), a.y - S(2.0f)), ImVec2(b.x + S(2.0f), b.y + S(2.0f)),
                g_tok.accSoft, S(12.0f), 0, S(4.0f));
    dl->AddRect(a, b, g_tok.acc, S(10.0f));
}

bool Field(const char* id, char* buf, size_t bufSize, float width, const char* hint) {
    PushText(0, 13.0f);
    PushFieldStyle(S(38.0f));
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::InputTextWithHint(id, hint ? hint : "", buf, bufSize);
    FocusRing();
    PopFieldStyle();
    PopFont();
    return changed;
}

bool NumberField(const char* id, int* value, int minValue, int maxValue, float width) {
    PushNum(13.0f);
    PushFieldStyle(S(38.0f));
    ImGui::SetNextItemWidth(S(width));
    const bool changed = ImGui::InputInt(id, value, 0, 0);
    if (changed) *value = (std::max)(minValue, (std::min)(maxValue, *value));
    FocusRing();
    PopFieldStyle();
    PopFont();
    return changed;
}

bool RangeSlider(const char* id, float* value, float minValue, float maxValue, float width,
                 bool* committed) {
    if (committed) *committed = false;
    if (g_tok.rack) {
        ImGui::SetNextItemWidth(width);
        const bool changed = ImGui::SliderFloat(id, value, minValue, maxValue, "");
        if (committed) *committed = ImGui::IsItemDeactivatedAfterEdit();
        return changed;
    }
    ImGui::PushID(id);
    const float h = S(22.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##range", ImVec2(width, h));
    HandCursorIfHovered();
    const bool hov = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const float thumbR = S(9.0f);
    const float x0 = p.x + thumbR, x1 = p.x + width - thumbR;
    bool changed = false;
    if (active && maxValue > minValue) {
        const float t = (std::max)(0.0f, (std::min)(1.0f, (ImGui::GetIO().MousePos.x - x0) / (x1 - x0)));
        const float v = minValue + t * (maxValue - minValue);
        if (v != *value) { *value = v; changed = true; }
    }
    if (ImGui::IsItemDeactivated() && committed) *committed = true;
    const float t = maxValue > minValue
        ? (std::max)(0.0f, (std::min)(1.0f, (*value - minValue) / (maxValue - minValue))) : 0.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + h * 0.5f;
    const float tx = x0 + (x1 - x0) * t;
    dl->AddRectFilled(ImVec2(p.x, cy - S(3.0f)), ImVec2(p.x + width, cy + S(3.0f)), g_tok.card3, S(3.0f));
    dl->AddRectFilled(ImVec2(p.x, cy - S(3.0f)), ImVec2(tx, cy + S(3.0f)), g_tok.acc, S(3.0f));
    const float sc = AnimSpring(ImGui::GetID("##thumb"), active ? 0.9f : (hov ? 1.15f : 1.0f));
    const float ring = AnimEase(ImGui::GetID("##ring"), hov ? 7.0f : 4.0f, 0.2f);
    dl->AddCircleFilled(ImVec2(tx, cy), (thumbR + S(ring)) * sc, g_tok.accSoft, 32);
    Glow(dl, ImVec2(tx, cy + S(2.0f)), thumbR * sc, IM_COL32(0, 0, 0, 120), S(4.0f), 4);
    dl->AddCircleFilled(ImVec2(tx, cy), thumbR * sc, IM_COL32_WHITE, 32);
    ImGui::PopID();
    return changed;
}

// Tiles ------------------------------------------------------------------------------------------

struct GridState {
    ImGuiID id = 0;
    int index = 0;
    float colW = 0.0f;
    std::vector<float> rowMax;      // last frame
    std::vector<float> rowMaxNext;  // this frame
};
static std::unordered_map<ImGuiID, GridState> g_grids;
static GridState* g_grid = nullptr;

struct TileFrame {
    ImGuiID id;
    const char* anchor;
    float extra;
    ImVec2 min, max;
};
static TileFrame g_tile{};
static std::unordered_map<ImGuiID, float> g_tileNatural;

void BeginTileGrid(const char* id) {
    const ImGuiID gid = ImGui::GetID(id);
    GridState& g = g_grids[gid];
    g.id = gid;
    g.index = 0;
    g.colW = (ImGui::GetContentRegionAvail().x - S(12.0f)) * 0.5f;
    g.rowMax = g.rowMaxNext;
    g.rowMaxNext.clear();
    g_grid = &g;
}

void EndTileGrid() {
    if (g_grid && (g_grid->index % 2) == 1) ImGui::NewLine();
    g_grid = nullptr;
}

bool BeginTile(const char* anchor, const char* title, const char* desc, bool* on, bool* changed,
               const ChipDef* tags, int tagCount, float toggleDelay) {
    const int index = g_grid ? g_grid->index : 0;
    const int row = index / 2;
    const float colW = g_grid ? g_grid->colW : ImGui::GetContentRegionAvail().x;
    if (g_grid && (index % 2) == 1) ImGui::SameLine(0.0f, S(12.0f));
    ImGui::PushID(anchor);
    const ImGuiID tid = ImGui::GetID("##tile");
    const float natural = g_tileNatural[tid];
    float height = natural;
    if (g_grid && row < static_cast<int>(g_grid->rowMax.size()))
        height = (std::max)(height, g_grid->rowMax[row]);
    const float lit = AnimEase(ImGui::GetID("##lit"), *on ? 1.0f : 0.0f, 0.3f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, MixU32(g_tok.card2, g_tok.acc, 0.06f * lit));
    ImGui::PushStyleColor(ImGuiCol_Border, MixU32(g_tok.line2, g_tok.accLine, lit));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(g_tok.rack ? g_tok.rCtl : 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20.0f), S(18.0f)));
    // Auto-sized to its content; equal row heights come from a spacer above
    // the tags sized from last frame's row maximum, so nothing ever clips.
    ImGui::BeginChild("##tile", ImVec2(colW, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    const ImVec2 wp = ImGui::GetWindowPos();
    // Header: title (flex) + toggle.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float y0 = ImGui::GetCursorPosY();
    PushText(2, 14.0f);
    ImGui::SetCursorPosY(y0 + S(4.0f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail - S(46.0f) - S(14.0f));
    ImGui::TextUnformatted(Caps(title));
    ImGui::PopTextWrapPos();
    PopFont();
    const float titleBottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - ImGui::GetCurrentWindow()->WindowPadding.x - S(46.0f), y0));
    const bool flipped = Toggle("##on", on, toggleDelay);
    if (changed) *changed = flipped;
    ImGui::SetCursorPosY((std::max)(titleBottom, y0 + S(28.0f)) + S(8.0f) - ImGui::GetStyle().ItemSpacing.y);
    if (desc && *desc) {
        PushText(0, 13.0f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, g_tok.muted);
        ImGui::TextUnformatted(desc);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        PopFont();
    }
    // Tags sit at the bottom of an equal-height row.
    const float extra = height > 0.0f && natural > 0.0f ? (std::max)(0.0f, height - natural) : 0.0f;
    if (extra > 0.0f) ImGui::Dummy(ImVec2(0.0f, extra - ImGui::GetStyle().ItemSpacing.y));
    if (tagCount > 0) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6.0f));
        for (int i = 0; i < tagCount; ++i) {
            if (i) ImGui::SameLine(0.0f, S(6.0f));
            Chip(tags[i].text, tags[i].kind);
        }
    }
    g_tile = {tid, anchor, extra, wp, ImVec2(wp.x + colW, wp.y)};
    return *on;
}

void TileNested() {
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0.0f, S(4.0f) - ImGui::GetStyle().ItemSpacing.y));
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (float x = lp.x; x < lp.x + avail; x += S(8.0f))
        dl->AddLine(ImVec2(x, lp.y), ImVec2((std::min)(x + S(4.0f), lp.x + avail), lp.y), g_tok.line2, 1.0f);
    ImGui::Dummy(ImVec2(0.0f, S(14.0f) - ImGui::GetStyle().ItemSpacing.y));
}

void EndTile() {
    const float contentBottom = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y +
                                ImGui::GetCurrentWindow()->WindowPadding.y;
    const float natural = contentBottom - g_tile.extra;
    g_tileNatural[g_tile.id] = natural;
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::EndChild();
    (void)contentBottom;
    ImGui::PopID();
    if (g_grid) {
        const int row = g_grid->index / 2;
        if (static_cast<int>(g_grid->rowMaxNext.size()) <= row) g_grid->rowMaxNext.resize(row + 1, 0.0f);
        g_grid->rowMaxNext[row] = (std::max)(g_grid->rowMaxNext[row], natural);
        ++g_grid->index;
        if ((g_grid->index % 2) == 0) ImGui::Dummy(ImVec2(0.0f, S(12.0f) - 2.0f * ImGui::GetStyle().ItemSpacing.y));
    }
    Anchor(g_tile.anchor, wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
}

} // namespace svms::cfg::ui
