#include "configurator/PageStage.h"
#include "configurator/Anim.h"
#include "configurator/ConfigDocument.h"
#include "configurator/ModernWidgets.h"
#include "configurator/StageAudition.h"
#include "configurator/Widgets.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace svms::cfg {

namespace {

constexpr float kD2R = 0.017453292f, kR2D = 57.29578f;

float Clamp(float v, float lo, float hi) { return (std::max)(lo, (std::min)(hi, v)); }
float Wrap180(float a) { return std::fmod(std::fmod(a + 180.0f, 360.0f) + 360.0f, 360.0f) - 180.0f; }
float S() { return ui::Scale(); }

// --- Data copied from the design prototype -------------------------------------------------

const char* const kNames[16] = {"Piano", "Strings", "Violin", "Cello", "Flute", "Oboe", "Horn", "Harp",
                                "Choir", "Drums", "Bass", "Brass", "Pads", "Guitar", "Marimba", "Effects"};

// Material order matches render/StageSolver.h.
const char* const kMatName[12] = {"Plaster", "Stone", "Concrete", "Wood", "Glass", "Carpet",
                                  "Seating", "Curtain", "Foam", "Books", "Foliage", "Open"};
// Mid-band (500 Hz / 2 kHz mean) absorption.
const float kMatAbs[12] = {0.035f, 0.035f, 0.025f, 0.08f, 0.045f, 0.405f,
                           0.525f, 0.60f, 0.825f, 0.30f, 0.25f, 1.0f};
const uint8_t kFloorRgb[12][3] = {{180, 180, 180}, {150, 150, 164}, {140, 140, 148}, {206, 150, 92},
                                  {120, 150, 170}, {168, 86, 96},   {118, 106, 150}, {128, 108, 176},
                                  {160, 160, 120}, {150, 105, 60},  {70, 132, 84},   {120, 120, 120}};

struct FurnType {
    const char* name;
    float w, d, h;
    uint8_t col[3];
    bool flat;
    bool pillar;
    const char* why;
};
// Indexed by StageSolver's FurnitureKind.
const FurnType kFurn[13] = {
    {"Sofa", 2.0f, 0.9f, 0.8f, {84, 128, 152}, false, false, "Upholstery soaks up mids and highs."},
    {"Armchair", 0.9f, 0.9f, 0.9f, {160, 112, 84}, false, false, "Upholstery soaks up mids and highs."},
    {"Bookshelf", 1.4f, 0.35f, 2.0f, {150, 105, 60}, false, false, "Books scatter the sound, so reflections get softer."},
    {"Table", 1.6f, 0.9f, 0.75f, {132, 92, 58}, false, false, "A hard surface that bounces sound."},
    {"Grand piano", 1.5f, 1.6f, 1.0f, {30, 30, 38}, false, false, "A big hard shape: it reflects and shadows sound behind it."},
    {"Rug", 3.0f, 2.0f, 0.02f, {168, 76, 76}, true, false, "Takes the shine off the floor reflection."},
    {"Curtains", 2.4f, 0.15f, 2.4f, {128, 108, 176}, false, false, "Heavy fabric soaks up highs, great against a bare wall."},
    {"Plant", 0.5f, 0.5f, 1.4f, {70, 132, 84}, false, false, "Leaves scatter high frequencies."},
    {"Audience", 6.0f, 2.5f, 1.1f, {112, 102, 128}, false, false, "People are very absorbent. A full hall is much drier than an empty one."},
    {"Pillar", 1.0f, 1.0f, 99.0f, {150, 150, 158}, false, true, "Reflects sound and casts a sound shadow behind it."},
    {"Partition", 3.0f, 0.1f, 2.0f, {214, 208, 196}, false, false, "A thin wall: blocks the direct sound and muffles what is behind it."},
    {"Speaker", 0.26f, 0.3f, 0.9f, {36, 36, 44}, false, false, "A loudspeaker cabinet. It is also a small obstacle in the room."},
    {"Subwoofer", 0.42f, 0.42f, 0.4f, {28, 28, 34}, false, false, "The bass speaker of a 5.1 set."},
};
enum Fk { Sofa, Armchair, Bookshelf, Table, Piano, Rug, Curtains, Plant, Audience, Pillar, Partition };

struct RoomDef {
    const char* name;
    float w, d, h;
    uint8_t mats[6];  // floor, ceiling, left, right, front, back
};
const RoomDef kRooms[5] = {
    {"Studio", 4, 5, 2.7f, {5, 8, 8, 8, 8, 0}},
    {"Living room", 6, 8, 2.8f, {5, 0, 7, 0, 3, 0}},
    {"Concert hall", 24, 36, 14, {6, 0, 3, 3, 3, 7}},
    {"Cathedral", 30, 62, 28, {6, 1, 1, 1, 1, 1}},
    {"Open air", 80, 80, 40, {11, 11, 11, 11, 11, 11}},
};
const char* const kSeatNames[4] = {"Orchestra", "Surround", "Close", "Wall"};
enum Seat { Orch, Wide, Close, Wall };

struct SceneDef {
    const char* name;
    const char* text;
    int room, seat, level;  // level 0 empty, 1 cosy, 2 packed
    float amount;
};
const SceneDef kScenes[5] = {
    {"Studio headphones", "Dry and close, like a booth", 0, Close, 0, 50},
    {"Living room", "Warm and a little roomy", 1, Close, 1, 60},
    {"Concert hall", "Front row of a real hall", 2, Orch, 1, 70},
    {"Cathedral", "Long, slow, huge tail", 3, Orch, 0, 80},
    {"Open field", "No walls, distant echo", 4, Wide, 0, 70},
};

// --- UI state (one stage page per process) --------------------------------------------------

struct State {
    int view = 2;            // 0 top, 1 side, 2 3D
    int mode = 0;            // 0 simple, 1 advanced
    float yaw = -0.62f, pitch = 0.52f;
    int sel = 0, fsel = -1;
    bool you = false;
    bool refl = true, labels = true;
    int roomIdx = 2, seat = Orch, level = 1;
    float amount = 100.0f;
    // Quick-knob state; the base is the band as it stood before the first nudge.
    float spread = 1.0f, closeness = 1.0f, lift = 0.0f, size = 1.0f;
    bool baseValid = false;
    float baseYaw = 0.0f;
    float basePolar[16][3] = {};
    // Drag.
    int dragMode = 0;        // 1 move, 2 orbit
    ImVec2 last{};
};
State g;

// --- Geometry -------------------------------------------------------------------------------

struct V3 { float x, y, z; };

struct Polar { float az, el, di; };

Polar ToPolar(const ConfigValues& w, const std::array<float, 3>& p) {
    const auto& L = w.stageListener;
    const float dx = p[0] - L[0], dy = p[1] - L[1], dz = p[2] - L[2], h = std::hypot(dx, dy);
    return {Wrap180(std::atan2(dx, dy) * kR2D - w.stageYaw), std::atan2(dz, h) * kR2D, std::hypot(h, dz)};
}

void SetPolar(ConfigValues& w, std::array<float, 3>& p, float az, float el, float di) {
    const auto& L = w.stageListener;
    const float a = (az + w.stageYaw) * kD2R, e = el * kD2R, h = di * std::cos(e);
    p[0] = Clamp(L[0] + h * std::sin(a), 0.3f, w.stageRoom[0] - 0.3f);
    p[1] = Clamp(L[1] + h * std::cos(a), 0.3f, w.stageRoom[1] - 0.3f);
    p[2] = Clamp(L[2] + di * std::sin(e), 0.2f, w.stageRoom[2] - 0.2f);
}

void FitAll(ConfigValues& w) {
    w.stageListener[0] = Clamp(w.stageListener[0], 0.5f, w.stageRoom[0] - 0.5f);
    w.stageListener[1] = Clamp(w.stageListener[1], 0.5f, w.stageRoom[1] - 0.5f);
    w.stageListener[2] = Clamp(w.stageListener[2], 0.2f, w.stageRoom[2] - 0.2f);
    for (auto& c : w.stageChannel) {
        c[0] = Clamp(c[0], 0.3f, w.stageRoom[0] - 0.3f);
        c[1] = Clamp(c[1], 0.3f, w.stageRoom[1] - 0.3f);
        c[2] = Clamp(c[2], 0.2f, w.stageRoom[2] - 0.2f);
    }
    for (auto& f : w.stageFurniture) {
        f.x = Clamp(f.x, 0.0f, w.stageRoom[0]);
        f.y = Clamp(f.y, 0.0f, w.stageRoom[1]);
    }
}

// Moving the listener carries the band along, so everyone keeps their direction.
void MoveListener(ConfigValues& w, float x, float y) {
    Polar pol[16];
    for (int i = 0; i < 16; ++i) pol[i] = ToPolar(w, w.stageChannel[i]);
    w.stageListener[0] = x;
    w.stageListener[1] = y;
    for (int i = 0; i < 16; ++i) SetPolar(w, w.stageChannel[i], pol[i].az, pol[i].el, pol[i].di);
}

void ResetQuick() {
    g.spread = g.closeness = g.size = 1.0f;
    g.lift = 0.0f;
    g.baseValid = false;
}

void SeatBand(ConfigValues& w, int kind) {
    g.seat = kind;
    for (int i = 0; i < 16; ++i) {
        const int row = i < 8 ? 0 : 1, k = i % 8;
        float az, el, di;
        switch (kind) {
        case Orch: az = -70.0f + k * 20.0f; el = row ? 14.0f : 0.0f; di = row ? 10.0f : 6.5f; break;
        case Wide: az = -180.0f + i * 22.5f; el = static_cast<float>((i % 2) * 10); di = 5.0f + (i % 3) * 1.2f; break;
        case Close: az = -40.0f + (i % 8) * 11.4f; el = row ? 6.0f : 0.0f; di = 2.6f + row * 0.8f; break;
        default: az = -80.0f + i * 10.7f; el = static_cast<float>((i % 4) * 8); di = 6.0f + (i % 3) * 2.6f; break;
        }
        SetPolar(w, w.stageChannel[i], az, el, di);
    }
    if (kind == Orch) SetPolar(w, w.stageChannel[9], 0, 0, 4);
    FitAll(w);
    ResetQuick();
}

void AddFurn(ConfigValues& w, int kind, float x, float y, bool rotated = false) {
    if (w.stageFurniture.size() >= 32) return;
    StageFurnitureValue f;
    f.kind = static_cast<uint8_t>(kind);
    f.x = x;
    f.y = y;
    f.rotated = rotated;
    w.stageFurniture.push_back(f);
}

void AutoFurnish(ConfigValues& w, int level) {
    w.stageFurniture.clear();
    g.fsel = -1;
    g.level = level;
    if (level == 0) return;
    const float W = w.stageRoom[0], D = w.stageRoom[1];
    const bool full = level == 2;
    if ((std::min)(W, D) < 12.0f) {
        AddFurn(w, Rug, W / 2, D * 0.42f);
        AddFurn(w, Sofa, W / 2, D - 0.6f);
        AddFurn(w, Bookshelf, 0.3f, D * 0.55f, true);
        AddFurn(w, Curtains, W - 0.12f, D * 0.4f, true);
        AddFurn(w, Plant, 0.45f, D - 0.5f);
        if (full) {
            AddFurn(w, Armchair, W * 0.8f, D * 0.72f);
            AddFurn(w, Table, W * 0.3f, D * 0.8f);
            AddFurn(w, Bookshelf, W - 0.25f, D * 0.72f, true);
            AddFurn(w, Piano, W * 0.72f, D * 0.14f);
            AddFurn(w, Curtains, W - 0.12f, D * 0.7f, true);
        }
    } else {
        AddFurn(w, Audience, W * 0.22f, D * 0.3f);
        AddFurn(w, Audience, W * 0.78f, D * 0.3f);
        AddFurn(w, Pillar, W * 0.1f, D * 0.55f);
        AddFurn(w, Pillar, W * 0.9f, D * 0.55f);
        if (full) {
            AddFurn(w, Audience, W * 0.22f, D * 0.46f);
            AddFurn(w, Audience, W * 0.78f, D * 0.46f);
            AddFurn(w, Pillar, W * 0.1f, D * 0.8f);
            AddFurn(w, Pillar, W * 0.9f, D * 0.8f);
            AddFurn(w, Curtains, W * 0.3f, D - 0.1f);
            AddFurn(w, Curtains, W * 0.5f, D - 0.1f);
            AddFurn(w, Curtains, W * 0.7f, D - 0.1f);
            AddFurn(w, Plant, W * 0.35f, D * 0.1f);
            AddFurn(w, Plant, W * 0.65f, D * 0.1f);
        }
    }
    FitAll(w);
}

void ApplyRoom(ConfigValues& w, int idx, bool resetListener) {
    const RoomDef& r = kRooms[idx];
    g.roomIdx = idx;
    w.stageRoom = {r.w, r.d, r.h};
    for (int i = 0; i < 6; ++i) w.stageSurface[i] = r.mats[i];
    if (resetListener) {
        w.stageListener = {r.w * 0.5f, r.d * 0.42f, 1.2f};
        w.stageYaw = 0.0f;
    }
    SeatBand(w, g.seat);
    AutoFurnish(w, g.level);
}

void ApplyScene(ConfigValues& w, const SceneDef& s) {
    g.seat = s.seat;
    g.level = s.level;
    ApplyRoom(w, s.room, true);
    w.stageTailTrim = s.amount / 100.0f;
    g.amount = s.amount;
    g.you = false;
    g.fsel = -1;
    g.sel = 0;
    ResetQuick();
}

void FurnDims(const ConfigValues& w, const StageFurnitureValue& f, float& hw, float& hd, float& h) {
    const FurnType& t = kFurn[f.kind];
    hw = (f.rotated ? t.d : t.w) * 0.5f;
    hd = (f.rotated ? t.w : t.d) * 0.5f;
    h = t.pillar ? w.stageRoom[2] : t.h;
}

// Sabine estimate for the chips and notes (the engine traces the real value).
float Rt60(const ConfigValues& w) {
    const float W = w.stageRoom[0], D = w.stageRoom[1], H = w.stageRoom[2];
    const float area[6] = {W * D, W * D, D * H, D * H, W * H, W * H};
    float A = 0.0f;
    for (int i = 0; i < 6; ++i) A += area[i] * kMatAbs[(std::min)(11, static_cast<int>(w.stageSurface[i]))];
    static const float furnAbs[13] = {0.5f, 0.5f, 0.3f, 0.08f, 0.08f, 0.4f, 0.6f, 0.25f, 0.55f, 0.035f, 0.035f, 0.08f, 0.08f};
    for (const auto& f : w.stageFurniture) {
        float hw, hd, h;
        FurnDims(w, f, hw, hd, h);
        if (!kFurn[f.kind].flat) A += 2.0f * (hw + hd) * 2.0f * h * furnAbs[f.kind] * 0.5f;
        A += 4.0f * hw * hd * furnAbs[f.kind] * (kFurn[f.kind].flat ? 0.5f : 0.4f);
    }
    return A <= 0.001f ? 0.0f : (std::min)(12.0f, 0.161f * W * D * H / A);
}

const char* Character(float t) {
    return t < 0.25f ? "Dry" : t < 0.8f ? "Intimate" : t < 1.8f ? "Natural" : t < 3.2f ? "Roomy" : "Cavernous";
}

// --- Colours --------------------------------------------------------------------------------

ImU32 Hsl(float hDeg, float s, float l, float a = 1.0f) {
    const float h = std::fmod(hDeg, 360.0f) / 60.0f, c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f)), m = l - c * 0.5f;
    float r = 0, gg = 0, b = 0;
    if (h < 1) { r = c; gg = x; }
    else if (h < 2) { r = x; gg = c; }
    else if (h < 3) { gg = c; b = x; }
    else if (h < 4) { gg = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else { r = c; b = x; }
    return IM_COL32(static_cast<int>((r + m) * 255), static_cast<int>((gg + m) * 255), static_cast<int>((b + m) * 255),
                    static_cast<int>(a * 255));
}
ImU32 Hue(int i, float a = 1.0f) { return Hsl(i * 360.0f / 16.0f + 20.0f, 0.72f, 0.62f, a); }
ImU32 WallCol(float absorb, float a) { return Hsl(205.0f - 175.0f * (std::min)(1.0f, absorb * 1.6f), 0.55f, 0.62f, a); }
ImU32 Rgb(const uint8_t* c, float k, float a) {
    return IM_COL32((std::min)(255, static_cast<int>(c[0] * k)), (std::min)(255, static_cast<int>(c[1] * k)),
                    (std::min)(255, static_cast<int>(c[2] * k)), static_cast<int>(a * 255));
}

// --- Projection (port of the prototype's Pj) ------------------------------------------------

struct Proj {
    int view = 2;
    ImVec2 org;
    float W = 0, H = 0;
    float sc = 1, ox = 0, oy = 0;
    float cxm = 0, cym = 0, czm = 0, R = 1, cy = 1, sy = 0, cp = 1, sp = 0, fit = 1;
    float roomW = 1, roomD = 1, roomH = 1;

    void Init(const ConfigValues& w, ImVec2 origin, float width, float height, int v) {
        view = v;
        org = origin;
        W = width;
        H = height;
        roomW = w.stageRoom[0];
        roomD = w.stageRoom[1];
        roomH = w.stageRoom[2];
        if (v != 2) {
            const float pad = 44.0f * S();
            const float ax = v == 0 ? roomW : roomD, ay = v == 0 ? roomD : roomH;
            sc = (std::min)((W - 2 * pad) / ax, (H - 2 * pad) / ay);
            ox = (W - ax * sc) * 0.5f;
            oy = (H + ay * sc) * 0.5f;
            return;
        }
        cxm = roomW * 0.5f;
        cym = roomD * 0.5f;
        czm = roomH * 0.35f;
        R = (std::max)((std::max)(roomW, roomD), roomH * 1.4f) * 1.55f;
        cy = std::cos(g.yaw);
        sy = std::sin(g.yaw);
        cp = std::cos(g.pitch);
        sp = std::sin(g.pitch);
        fit = (std::min)(W, H);
    }
    // Screen point; depth (distance along the view axis) and px-per-metre go to d / k.
    ImVec2 F(V3 p, float* d = nullptr, float* k = nullptr) const {
        if (view != 2) {
            const float a = view == 0 ? p.x : p.y, b = view == 0 ? p.y : p.z;
            if (d) *d = 0;
            if (k) *k = sc;
            return ImVec2(org.x + ox + a * sc, org.y + oy - b * sc);
        }
        const float x = p.x - cxm, y = p.y - cym, z = p.z - czm;
        const float x1 = x * cy - y * sy, y1 = x * sy + y * cy;
        const float y2 = y1 * cp - z * sp, z2 = y1 * sp + z * cp;
        const float dp = R + y2, kk = fit / (dp * 0.62f);
        if (d) *d = dp;
        if (k) *k = kk;
        return ImVec2(org.x + W * 0.5f + x1 * kk, org.y + H * 0.5f - z2 * kk);
    }
    // Back to the horizontal plane z (3D) or the view plane (2D); a/b are the two world axes.
    bool Inv(ImVec2 s, float z, float& a, float& b) const {
        const float qx = s.x - org.x, qy = s.y - org.y;
        if (view != 2) {
            const float u = (qx - ox) / sc, v = (oy - qy) / sc;
            a = u;
            b = v;
            return true;
        }
        const float zc = z - czm, B = (H * 0.5f - qy) * 0.62f / fit, den = B * cp - sp;
        if (std::fabs(den) < 1e-4f) return false;
        const float y1 = (zc * cp - B * (R - zc * sp)) / den, y2 = y1 * cp - zc * sp;
        const float k = fit / ((R + y2) * 0.62f), x1 = (qx - W * 0.5f) / k;
        a = x1 * cy + y1 * sy + cxm;
        b = -x1 * sy + y1 * cy + cym;
        return true;
    }
};

// --- Drawing --------------------------------------------------------------------------------

void Quad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 fill, ImU32 line = 0, float lw = 1.0f) {
    const ImVec2 pts[4] = {a, b, c, d};
    dl->AddConvexPolyFilled(pts, 4, fill);
    if (line) dl->AddPolyline(pts, 4, line, ImDrawFlags_Closed, lw);
}

void Glow(ImDrawList* dl, ImVec2 c, float r, ImU32 col) { ui::Glow(dl, c, r, col, r * 1.6f, 6); }

// Furniture box with the prototype's per-face shading; faces turned away are skipped.
void DrawBox3(ImDrawList* dl, const Proj& P, float cx, float cy, float hw, float hd, float h, const uint8_t* col,
              bool flat, bool selected) {
    const V3 c[8] = {{cx - hw, cy - hd, 0}, {cx + hw, cy - hd, 0}, {cx + hw, cy + hd, 0}, {cx - hw, cy + hd, 0},
                     {cx - hw, cy - hd, h}, {cx + hw, cy - hd, h}, {cx + hw, cy + hd, h}, {cx - hw, cy + hd, h}};
    ImVec2 s[8];
    for (int i = 0; i < 8; ++i) s[i] = P.F(c[i]);
    if (flat) {
        Quad(dl, s[0], s[1], s[2], s[3], Rgb(col, 1.0f, 0.85f), selected ? ui::Tok().acc : Rgb(col, 1.3f, 0.6f), selected ? 2.0f * S() : 1.0f);
        return;
    }
    const V3 toCam = {-P.cp * P.sy, -P.cp * P.cy, P.sp};
    struct Face { int a, b, c, d; V3 n; float shade; };
    const Face faces[5] = {{0, 1, 5, 4, {0, -1, 0}, 0.80f}, {2, 3, 7, 6, {0, 1, 0}, 0.52f},
                           {3, 0, 4, 7, {-1, 0, 0}, 0.60f}, {1, 2, 6, 5, {1, 0, 0}, 0.70f},
                           {4, 5, 6, 7, {0, 0, 1}, 1.08f}};
    for (const Face& f : faces) {
        if (f.n.x * toCam.x + f.n.y * toCam.y + f.n.z * toCam.z <= 0.0f) continue;
        Quad(dl, s[f.a], s[f.b], s[f.c], s[f.d], Rgb(col, f.shade, 1.0f), IM_COL32(255, 255, 255, 40));
    }
    if (selected) {
        const ImVec2 q[4] = {s[0], s[1], s[2], s[3]};
        dl->AddPolyline(q, 4, ui::Tok().acc, ImDrawFlags_Closed, 2.0f * S());
    }
}

void DrawFurn(ImDrawList* dl, const Proj& P, const ConfigValues& w, int idx) {
    const StageFurnitureValue& f = w.stageFurniture[idx];
    float hw, hd, h;
    FurnDims(w, f, hw, hd, h);
    const FurnType& t = kFurn[f.kind];
    const bool sel = idx == g.fsel;
    if (P.view == 2) {
        DrawBox3(dl, P, f.x, f.y, hw, hd, h, t.col, t.flat, sel);
        return;
    }
    const bool top = P.view == 0;
    const float a0 = top ? f.x - hw : f.y - hd, a1 = top ? f.x + hw : f.y + hd;
    const float b0 = top ? f.y - hd : 0.0f, b1 = top ? f.y + hd : h;
    auto pt = [&](float a, float b) { return top ? P.F({a, b, 0}) : P.F({0, a, b}); };
    Quad(dl, pt(a0, b0), pt(a1, b0), pt(a1, b1), pt(a0, b1), Rgb(t.col, 1.0f, t.flat ? 0.6f : 0.9f),
         sel ? ui::Tok().acc : Rgb(t.col, 1.4f, 0.8f), sel ? 2.0f * S() : 1.0f);
}

V3 ChPos(const ConfigValues& w, int i) { return {w.stageChannel[i][0], w.stageChannel[i][1], w.stageChannel[i][2]}; }
V3 ListenerPos(const ConfigValues& w) { return {w.stageListener[0], w.stageListener[1], w.stageListener[2]}; }

// First-order wall reflections of the selected channel, as the audio uses them.
void DrawReflections(ImDrawList* dl, const Proj& P, const ConfigValues& w, int ch) {
    const V3 s = ChPos(w, ch), l = ListenerPos(w);
    const float dims[3] = {w.stageRoom[0], w.stageRoom[1], w.stageRoom[2]};
    for (int wall = 0; wall < 6; ++wall) {
        const int ax = wall < 2 ? 2 : wall < 4 ? 0 : 1;      // floor/ceiling z, left/right x, front/back y
        const float plane = (wall % 2) ? dims[ax] : 0.0f;
        V3 m = s;
        float* mv = ax == 0 ? &m.x : ax == 1 ? &m.y : &m.z;
        *mv = 2.0f * plane - *mv;
        const float lv = ax == 0 ? l.x : ax == 1 ? l.y : l.z, sv = ax == 0 ? m.x : ax == 1 ? m.y : m.z;
        if (std::fabs(sv - lv) < 1e-4f) continue;
        const float t = (plane - lv) / (sv - lv);
        const V3 b = {l.x + (m.x - l.x) * t, l.y + (m.y - l.y) * t, l.z + (m.z - l.z) * t};
        if (b.x < -0.01f || b.x > dims[0] + 0.01f || b.y < -0.01f || b.y > dims[1] + 0.01f || b.z < -0.01f ||
            b.z > dims[2] + 0.01f)
            continue;
        const ImU32 col = Hue(ch, 0.55f);
        const ImVec2 ps = P.F(s), pb = P.F(b), pl = P.F(l);
        dl->AddLine(ps, pb, col, 1.4f * S());
        dl->AddLine(pb, pl, col, 1.4f * S());
        dl->AddCircleFilled(pb, 2.5f * S(), col);
    }
    dl->AddLine(P.F(s), P.F(l), Hue(ch, 0.9f), 1.8f * S());
}

void DrawRoom(ImDrawList* dl, const Proj& P, const ConfigValues& w) {
    const ui::Tokens& k = ui::Tok();
    const float W = w.stageRoom[0], D = w.stageRoom[1], H = w.stageRoom[2];
    const float area[6] = {W * D, W * D, D * H, D * H, W * H, W * H};
    (void)area;
    if (P.view != 2) {
        const bool top = P.view == 0;
        const ImVec2 a = top ? P.F({0, 0, 0}) : P.F({0, 0, 0}), b = top ? P.F({W, D, 0}) : P.F({0, D, H});
        const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
        const uint8_t* fc = kFloorRgb[(std::min)(11, static_cast<int>(w.stageSurface[0]))];
        dl->AddRectFilled(lo, hi, top ? Rgb(fc, 1.0f, 0.10f) : IM_COL32(255, 255, 255, 8));
        // Walls: the edges coloured by their material (side walls in the top view, floor/ceiling in side view).
        const int m0 = top ? 2 : 0, m1 = top ? 3 : 1, m2 = top ? 4 : 4, m3 = top ? 5 : 5;
        auto mat = [&](int i) { return kMatAbs[(std::min)(11, static_cast<int>(w.stageSurface[i]))]; };
        const float lw = 3.0f * S();
        if (top) {
            dl->AddLine(ImVec2(lo.x, lo.y), ImVec2(lo.x, hi.y), WallCol(mat(m0), 0.9f), lw);
            dl->AddLine(ImVec2(hi.x, lo.y), ImVec2(hi.x, hi.y), WallCol(mat(m1), 0.9f), lw);
            dl->AddLine(ImVec2(lo.x, hi.y), ImVec2(hi.x, hi.y), WallCol(mat(m2), 0.9f), lw);   // front, y = 0
            dl->AddLine(ImVec2(lo.x, lo.y), ImVec2(hi.x, lo.y), WallCol(mat(m3), 0.9f), lw);   // back
        } else {
            dl->AddLine(ImVec2(lo.x, hi.y), ImVec2(hi.x, hi.y), WallCol(mat(m0), 0.9f), lw);   // floor
            dl->AddLine(ImVec2(lo.x, lo.y), ImVec2(hi.x, lo.y), WallCol(mat(m1), 0.9f), lw);   // ceiling
            dl->AddLine(ImVec2(lo.x, lo.y), ImVec2(lo.x, hi.y), WallCol(mat(m2), 0.9f), lw);
            dl->AddLine(ImVec2(hi.x, lo.y), ImVec2(hi.x, hi.y), WallCol(mat(m3), 0.9f), lw);
        }
        return;
    }
    // Floor tint and grid.
    const uint8_t* fc = kFloorRgb[(std::min)(11, static_cast<int>(w.stageSurface[0]))];
    Quad(dl, P.F({0, 0, 0}), P.F({W, 0, 0}), P.F({W, D, 0}), P.F({0, D, 0}), Rgb(fc, 1.0f, 0.13f));
    const float step = W > 30.0f || D > 30.0f ? 4.0f : W > 10.0f || D > 10.0f ? 2.0f : 1.0f;
    for (float x = step; x < W - 0.01f; x += step) dl->AddLine(P.F({x, 0, 0}), P.F({x, D, 0}), IM_COL32(255, 255, 255, 14));
    for (float y = step; y < D - 0.01f; y += step) dl->AddLine(P.F({0, y, 0}), P.F({W, y, 0}), IM_COL32(255, 255, 255, 14));
    // Far faces (centre farther than the room centre) get a wash by absorption.
    const V3 cs[6] = {{W / 2, D / 2, 0}, {W / 2, D / 2, H}, {0, D / 2, H / 2}, {W, D / 2, H / 2}, {W / 2, 0, H / 2}, {W / 2, D, H / 2}};
    float roomDepth;
    P.F({W / 2, D / 2, H / 2}, &roomDepth);
    for (int i = 1; i < 6; ++i) {
        float d;
        P.F(cs[i], &d);
        if (d < roomDepth) continue;
        V3 q[4];
        if (i == 1) { q[0] = {0, 0, H}; q[1] = {W, 0, H}; q[2] = {W, D, H}; q[3] = {0, D, H}; }
        else if (i == 2) { q[0] = {0, 0, 0}; q[1] = {0, D, 0}; q[2] = {0, D, H}; q[3] = {0, 0, H}; }
        else if (i == 3) { q[0] = {W, 0, 0}; q[1] = {W, D, 0}; q[2] = {W, D, H}; q[3] = {W, 0, H}; }
        else if (i == 4) { q[0] = {0, 0, 0}; q[1] = {W, 0, 0}; q[2] = {W, 0, H}; q[3] = {0, 0, H}; }
        else { q[0] = {0, D, 0}; q[1] = {W, D, 0}; q[2] = {W, D, H}; q[3] = {0, D, H}; }
        const float ab = kMatAbs[(std::min)(11, static_cast<int>(w.stageSurface[i]))];
        Quad(dl, P.F(q[0]), P.F(q[1]), P.F(q[2]), P.F(q[3]), WallCol(ab, 0.11f), WallCol(ab, 0.55f), 1.5f * S());
    }
    // Remaining edges.
    const V3 e[8] = {{0, 0, 0}, {W, 0, 0}, {W, D, 0}, {0, D, 0}, {0, 0, H}, {W, 0, H}, {W, D, H}, {0, D, H}};
    const int ed[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& i : ed) dl->AddLine(P.F(e[i[0]]), P.F(e[i[1]]), IM_COL32(255, 255, 255, 38), 1.0f);
    (void)k;
}

void Label(ImDrawList* dl, ImVec2 p, ImU32 col, const char* text, bool centered = true) {
    ui::PushText(1, 12.0f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    if (centered) p.x -= ts.x * 0.5f;
    dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 160), text);
    dl->AddText(p, col, text);
    ui::PopFont();
}

float* TargetXYZ(ConfigValues& w, bool& isFurn) {
    isFurn = false;
    if (g.fsel >= 0 && g.fsel < static_cast<int>(w.stageFurniture.size())) {
        isFurn = true;
        return &w.stageFurniture[g.fsel].x;  // x, y adjacent
    }
    return g.you ? w.stageListener.data() : w.stageChannel[g.sel].data();
}

void MoveTarget(ConfigValues& w, float x, float y, float z, bool setX, bool setY, bool setZ) {
    bool furn;
    float* t = TargetXYZ(w, furn);
    if (furn) {
        if (setX) w.stageFurniture[g.fsel].x = Clamp(x, 0.0f, w.stageRoom[0]);
        if (setY) w.stageFurniture[g.fsel].y = Clamp(y, 0.0f, w.stageRoom[1]);
        return;
    }
    if (g.you) {
        const float nx = setX ? Clamp(x, 0.5f, w.stageRoom[0] - 0.5f) : t[0];
        const float ny = setY ? Clamp(y, 0.5f, w.stageRoom[1] - 0.5f) : t[1];
        MoveListener(w, nx, ny);
        if (setZ) w.stageListener[2] = Clamp(z, 0.2f, w.stageRoom[2] - 0.2f);
        return;
    }
    if (setX) t[0] = Clamp(x, 0.3f, w.stageRoom[0] - 0.3f);
    if (setY) t[1] = Clamp(y, 0.3f, w.stageRoom[1] - 0.3f);
    if (setZ) t[2] = Clamp(z, 0.2f, w.stageRoom[2] - 0.2f);
}

const char* const kViews[3] = {"Top", "Side", "3D"};

void StageCanvas(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float height = Clamp(avail * 0.56f, 340.0f * s, 560.0f * s);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + avail, p0.y + height);
    ImGui::InvisibleButton("##stagecv", ImVec2(avail, height));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Proj P;
    P.Init(w, p0, avail, height, g.view);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool shift = ImGui::GetIO().KeyShift;

    // --- Input ---
    if (ImGui::IsItemActivated()) {
        const float grab = 22.0f * s;
        float best = grab;
        int hitCh = -2;  // -1 listener
        for (int i = 0; i < 17; ++i) {
            const V3 p = i < 16 ? ChPos(w, i) : ListenerPos(w);
            const ImVec2 q = P.F(p);
            const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
            if (d < best) { best = d; hitCh = i < 16 ? i : -1; }
        }
        int hitF = -1;
        if (hitCh == -2) {
            float bf = 34.0f * s;
            for (int i = 0; i < static_cast<int>(w.stageFurniture.size()); ++i) {
                float hw, hd, h;
                FurnDims(w, w.stageFurniture[i], hw, hd, h);
                const ImVec2 q = P.F({w.stageFurniture[i].x, w.stageFurniture[i].y, (std::min)(h, 3.0f) * 0.5f});
                const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
                if (d < bf) { bf = d; hitF = i; }
            }
        }
        if (hitCh >= 0) { g.sel = hitCh; g.you = false; g.fsel = -1; g.dragMode = 1; }
        else if (hitCh == -1) { g.you = true; g.fsel = -1; g.dragMode = 1; }
        else if (hitF >= 0) { g.fsel = hitF; g.you = false; g.dragMode = 1; }
        else g.dragMode = g.view == 2 ? 2 : 0;
        g.last = mouse;
    }
    if (g.dragMode != 0 && !ImGui::IsMouseDown(0)) {
        if (g.dragMode == 1) g.baseValid = false;
        g.dragMode = 0;
    }
    if (g.dragMode == 2 && ImGui::IsMouseDown(0)) {
        g.yaw += (mouse.x - g.last.x) * 0.008f;
        g.pitch = Clamp(g.pitch + (mouse.y - g.last.y) * 0.006f, 0.08f, 1.45f);
        g.last = mouse;
    } else if (g.dragMode == 1 && ImGui::IsMouseDown(0)) {
        bool furn;
        float* t = TargetXYZ(w, furn);
        const float curZ = furn ? 0.0f : t[2];
        if (g.view == 2 && shift && !furn) {
            float kk;
            P.F(V3{t[0], t[1], curZ}, nullptr, &kk);
            MoveTarget(w, 0, 0, curZ - (mouse.y - g.last.y) / (kk * P.cp), false, false, true);
            doc.MarkDirty();
        } else if (g.view == 1) {
            float a, b;
            if (P.Inv(mouse, 0, a, b)) {
                MoveTarget(w, 0, a, b, false, true, !furn);
                doc.MarkDirty();
            }
        } else {
            float a, b;
            if (P.Inv(mouse, curZ, a, b)) {
                MoveTarget(w, a, b, 0, true, true, false);
                doc.MarkDirty();
            }
        }
        g.last = mouse;
    }
    if (hovered || ImGui::IsItemFocused()) {
        const float step = shift ? 1.0f : ImGui::GetIO().KeyAlt ? 0.05f : 0.25f;
        bool furn;
        float* t = TargetXYZ(w, furn);
        float dx = 0, dy = 0, dz = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) (g.view == 1 ? dy : dx) -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) (g.view == 1 ? dy : dx) += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) (g.view == 1 ? dz : dy) += step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) (g.view == 1 ? dz : dy) -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) dz += step;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) dz -= step;
        if (dx != 0 || dy != 0 || dz != 0) {
            MoveTarget(w, t[0] + dx, t[1] + dy, furn ? 0.0f : t[2] + dz, dx != 0, dy != 0, dz != 0 && !furn);
            g.baseValid = false;
            doc.MarkDirty();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Q)) { w.stageYaw = Wrap180(w.stageYaw - 5.0f); doc.MarkDirty(); }
        if (ImGui::IsKeyPressed(ImGuiKey_E)) { w.stageYaw = Wrap180(w.stageYaw + 5.0f); doc.MarkDirty(); }
        if (g.fsel >= 0 && g.fsel < static_cast<int>(w.stageFurniture.size())) {
            if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                w.stageFurniture[g.fsel].rotated = !w.stageFurniture[g.fsel].rotated;
                g.level = 3;
                doc.MarkDirty();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                w.stageFurniture.erase(w.stageFurniture.begin() + g.fsel);
                g.fsel = -1;
                g.level = 3;
                doc.MarkDirty();
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { g.fsel = -1; g.you = false; }
    }

    // --- Draw ---
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 12, 255), 12.0f * s);
    DrawRoom(dl, P, w);

    // Distance rings around the listener.
    {
        const V3 L = ListenerPos(w);
        for (float r : {3.0f, 6.0f, 10.0f}) {
            ImVec2 pts[73];
            int n = 0;
            for (int i = 0; i <= 72; ++i) {
                const float a = i / 72.0f * 6.2832f;
                pts[n++] = P.F({L.x + std::sin(a) * r, L.y + std::cos(a) * r, 0});
            }
            if (g.view == 1) break;
            dl->AddPolyline(pts, n, IM_COL32(255, 255, 255, 18), 0, 1.0f);
        }
    }

    // Furniture, far to near.
    {
        std::vector<std::pair<float, int>> order;
        for (int i = 0; i < static_cast<int>(w.stageFurniture.size()); ++i) {
            float d;
            P.F({w.stageFurniture[i].x, w.stageFurniture[i].y, 0}, &d);
            order.push_back({-d, i});
        }
        std::sort(order.begin(), order.end());
        for (const auto& o : order) DrawFurn(dl, P, w, o.second);
    }

    if (g.refl && !g.you && g.fsel < 0 && g.view != 1) DrawReflections(dl, P, w, g.sel);

    // Channels, far to near; the listener last.
    {
        std::vector<std::pair<float, int>> order;
        for (int i = 0; i < 16; ++i) {
            float d;
            P.F(ChPos(w, i), &d);
            order.push_back({-d, i});
        }
        std::sort(order.begin(), order.end());
        for (const auto& o : order) {
            const int i = o.second;
            const V3 pp = ChPos(w, i);
            float kk;
            const ImVec2 q = P.F(pp, nullptr, &kk);
            const ImVec2 fl = P.F({pp.x, pp.y, 0});
            const bool on = w.stageChannelOn[i], sel = i == g.sel && !g.you && g.fsel < 0;
            const float a = on ? 1.0f : 0.35f;
            if (g.view == 2 && pp.z > 0.05f) {
                dl->AddLine(q, fl, Hue(i, 0.28f * a), 1.0f);
                dl->AddEllipseFilled(fl, ImVec2(5.0f * s, 2.2f * s), Hue(i, 0.35f * a), 0.0f, 12);
            }
            const float r = g.view == 2 ? Clamp(0.85f * kk, 9.0f * s, 17.0f * s) : 9.5f * s;
            if (on) Glow(dl, q, r * 1.7f, Hue(i, sel ? 0.55f : 0.28f));
            dl->AddCircleFilled(q, r, Hue(i, a));
            dl->AddCircle(q, r, IM_COL32(255, 255, 255, on ? 90 : 40), 0, 1.0f);
            if (sel) dl->AddCircle(q, r + 4.0f * s, k.acc, 0, 2.2f * s);
            char n[8];
            std::snprintf(n, sizeof(n), "%d", i + 1);
            ui::PushNum(r > 9.0f * s ? 11.0f : 10.0f, true);
            const ImVec2 ts = ImGui::CalcTextSize(n);
            dl->AddText(ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), IM_COL32(12, 12, 16, 255), n);
            ui::PopFont();
            if (g.labels && (sel || g.view != 2 || r > 8.0f * s)) Label(dl, ImVec2(q.x, q.y - r - 16.0f * s), IM_COL32(235, 235, 240, on ? 230 : 110), kNames[i]);
        }
        // Listener.
        const V3 L = ListenerPos(w);
        float kk;
        const ImVec2 q = P.F(L, nullptr, &kk);
        const bool sel = g.you;
        const float r = g.view == 2 ? Clamp(0.8f * kk, 9.0f * s, 16.0f * s) : 10.0f * s;
        Glow(dl, q, r * 1.8f, IM_COL32(220, 220, 235, sel ? 120 : 60));
        dl->AddCircleFilled(q, r, IM_COL32(230, 230, 240, 255));
        dl->AddCircle(q, r + 3.0f * s, sel ? k.acc : IM_COL32(255, 255, 255, 120), 0, 2.0f * s);
        const float yawR = w.stageYaw * kD2R;
        const ImVec2 tip = P.F({L.x + std::sin(yawR) * 1.4f, L.y + std::cos(yawR) * 1.4f, L.z});
        dl->AddLine(q, tip, k.acc, 2.5f * s);
        dl->AddCircleFilled(tip, 3.5f * s, k.acc);
        Label(dl, ImVec2(q.x, q.y + r + 4.0f * s), IM_COL32(235, 235, 240, 235), "You");
    }

    // Overlay: chips and size.
    {
        float x = p0.x + 14.0f * s;
        const float y = p0.y + 12.0f * s;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (w.stageEar) { ui::Chip("Binaural", ui::ChipKind::Cpu); ImGui::SameLine(0, 8.0f * s); }
        char buf[64];
        const float t60 = Rt60(w);
        if (g.roomIdx >= 0 && g.roomIdx < 5 && w.stageSurface[0] != 11) {
            std::snprintf(buf, sizeof(buf), "%s, %.1f s", kRooms[g.roomIdx].name, t60);
            ui::Chip(buf, ui::ChipKind::Exp);
            ImGui::SameLine(0, 8.0f * s);
        }
        ui::Chip(t60 <= 0.001f ? "Open" : Character(t60), ui::ChipKind::Ok);
        std::snprintf(buf, sizeof(buf), "%.0f x %.0f x %.0f m", w.stageRoom[0], w.stageRoom[1], w.stageRoom[2]);
        ui::PushNum(11.0f);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(p1.x - ts.x - 14.0f * s, p0.y + 14.0f * s), k.muted, buf);
        ui::PopFont();
        const char* hint = g.view == 2 ? "drag a dot: move on floor | Shift+drag: lift | empty space: orbit"
                          : g.view == 1 ? "drag a dot: along the room, up and down" : "drag a dot, furniture or yourself";
        ui::PushText(0, 12.0f);
        const ImVec2 hs = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(p1.x - hs.x - 14.0f * s, p1.y - hs.y - 10.0f * s), IM_COL32(150, 150, 160, 200), hint);
        ui::PopFont();
    }
    dl->PopClipRect();
    dl->AddRect(p0, p1, k.line2, 12.0f * s);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(avail, 0));
}

// --- Cards ----------------------------------------------------------------------------------

void HeaderBar(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = S();
    const float avail = ImGui::GetContentRegionAvail().x;
    static const char* const modes[2] = {"Simple", "Advanced"};
    const float bw = ui::ButtonSize("Reset to a good start").x, sw = ui::SegmentedSize(modes, 2).x, tw = 46.0f * s;
    const float gap = 10.0f * s;
    const float x0 = ImGui::GetCursorPosX() + avail - (bw + sw + tw + 2 * gap);
    const float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0, y));
    if (ui::Button("Reset to a good start")) {
        g.seat = Orch;
        g.level = 1;
        w.stageTailTrim = 0.7f;
        g.amount = 70.0f;
        w.stageEar = true;
        w.stageAir = true;
        w.stageEnabled = true;
        ApplyRoom(w, 2, true);
        doc.MarkDirty();
    }
    ImGui::SameLine(0, gap);
    ui::Segmented("##stmode", &g.mode, modes, 2);
    ImGui::SameLine(0, gap);
    ui::Anchor("stageOn", ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + tw, ImGui::GetCursorScreenPos().y + 28.0f * s));
    if (ui::Toggle("##stmaster", &w.stageEnabled)) doc.MarkDirty();
    ImGui::Dummy(ImVec2(0, 4.0f * s));
    ui::RestartMark(true);
    ImGui::SameLine(0, 8.0f * s);
    ui::PushText(0, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
    ImGui::TextUnformatted(w.stageEnabled ? "Everything here is read when the engine starts." : "The stage is off: nothing below changes the sound yet.");
    ImGui::PopStyleColor();
    ui::PopFont();
}

void ScenesCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ui::BeginCard("##stscenes", "Start with a scene", "One click sets the room, seating, furniture and room amount. Then adjust below.");
    const float avail = ImGui::GetContentRegionAvail().x;
    const int n = 5;
    const float gap = 10.0f * s, cw = (avail - gap * (n - 1)) / n, ch = 74.0f * s;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < n; ++i) {
        const ImVec2 a(o.x + i * (cw + gap), o.y), b(a.x + cw, a.y + ch);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        ImGui::InvisibleButton("##scene", ImVec2(cw, ch));
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) {
            ApplyScene(w, kScenes[i]);
            doc.MarkDirty();
        }
        const float t = AnimEase(ImGui::GetID("##sh"), hov ? 1.0f : 0.0f, 0.2f);
        dl->AddRectFilled(a, b, k.card2, 12.0f * s);
        dl->AddRect(a, b, ui::MixU32(k.line2, k.acc, t), 12.0f * s);
        ui::PushText(2, 14.0f);
        dl->AddText(ImVec2(a.x + 14.0f * s, a.y + 12.0f * s), k.text, kScenes[i].name);
        ui::PopFont();
        ui::PushText(0, 12.0f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 14.0f * s, a.y + 34.0f * s), k.muted, kScenes[i].text, nullptr, cw - 24.0f * s);
        ui::PopFont();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(avail, ch));
    ui::EndCard();
}

void StageCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = S();
    float rw = ui::SegmentedSize(kViews, 3).x;
    ui::BeginCard("##ststage", "Stage",
                  g.view == 2 ? "Drag a dot to move it, hold Shift to lift it, drag empty space to orbit"
                  : g.view == 1 ? "Drag up or down for height, left and right along the room"
                                : "Drag any dot, furniture or yourself to move it",
                  rw, "stageStage");
    ui::Segmented("##stview", &g.view, kViews, 3);
    ui::CardHeaderEnd();
    StageCanvas(doc);
    ImGui::Dummy(ImVec2(0, 8.0f * s));
    ui::Toggle("##strefl", &g.refl);
    ImGui::SameLine(0, 8.0f * s);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Reflections");
    ImGui::SameLine(0, 22.0f * s);
    ui::Toggle("##stlbl", &g.labels);
    ImGui::SameLine(0, 8.0f * s);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Names");
    ImGui::SameLine(0, 22.0f * s);
    ImGui::AlignTextToFramePadding();
    ui::PushText(0, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
    ImGui::TextUnformatted("Arrows nudge the selection (Shift big, Alt tiny), PgUp/PgDn lift, Q/E turn you, R turns furniture, Del removes it");
    ImGui::PopStyleColor();
    ui::PopFont();
    (void)w;
    ui::EndCard();
}

void ChannelsCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    ui::BeginCard("##stchan", "Channels", "Click one, then drag it on the stage. M leaves it out of the stage.");
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail > 760.0f * s ? 4 : avail > 420.0f * s ? 2 : 1;
    const float gap = 10.0f * s, cw = (avail - gap * (cols - 1)) / cols, rh = 40.0f * s;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 16; ++i) {
        const int r = i / cols, c = i % cols;
        const ImVec2 a(o.x + c * (cw + gap), o.y + r * (rh + 6.0f * s)), b(a.x + cw, a.y + rh);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##row", ImVec2(cw - 40.0f * s, rh));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) { g.sel = i; g.you = false; g.fsel = -1; }
        const bool sel = g.sel == i && !g.you && g.fsel < 0;
        dl->AddRectFilled(a, b, sel ? ui::WithAlpha(k.acc, 0.10f) : k.card2, 10.0f * s);
        dl->AddRect(a, b, sel ? k.acc : k.line2, 10.0f * s);
        const bool on = w.stageChannelOn[i];
        dl->AddCircleFilled(ImVec2(a.x + 16.0f * s, a.y + rh * 0.5f), 5.5f * s, Hue(i, on ? 1.0f : 0.35f));
        ui::PushText(1, 14.0f);
        dl->AddText(ImVec2(a.x + 32.0f * s, a.y + (rh - ImGui::GetTextLineHeight()) * 0.5f), on ? k.text : k.muted, kNames[i]);
        const float nw = ImGui::CalcTextSize(kNames[i]).x;
        ui::PopFont();
        char n[8];
        std::snprintf(n, sizeof(n), "%d", i + 1);
        ui::PushNum(11.0f);
        dl->AddText(ImVec2(a.x + 32.0f * s + nw + 6.0f * s, a.y + (rh - ImGui::GetTextLineHeight()) * 0.5f + 1.0f), k.muted, n);
        ui::PopFont();
        // Mute.
        const ImVec2 ma(b.x - 34.0f * s, a.y + (rh - 26.0f * s) * 0.5f), mb(ma.x + 26.0f * s, ma.y + 26.0f * s);
        ImGui::SetCursorScreenPos(ma);
        ImGui::InvisibleButton("##m", ImVec2(26.0f * s, 26.0f * s));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) { w.stageChannelOn[i] = !w.stageChannelOn[i]; doc.MarkDirty(); }
        dl->AddRectFilled(ma, mb, !on ? ui::WithAlpha(k.bad, 0.25f) : k.card, 7.0f * s);
        dl->AddRect(ma, mb, !on ? k.bad : k.line2, 7.0f * s);
        ui::PushNum(11.0f, true);
        const ImVec2 ts = ImGui::CalcTextSize("M");
        dl->AddText(ImVec2(ma.x + (26.0f * s - ts.x) * 0.5f, ma.y + (26.0f * s - ts.y) * 0.5f), !on ? k.bad : k.muted, "M");
        ui::PopFont();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(avail, ((16 + cols - 1) / cols) * (rh + 6.0f * s)));
    ui::EndCard();
}

float PctDisp(float v) { return v * 100.0f; }

void QuickCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = S();
    static const char* const detail[3] = {"Fast", "Balanced", "Cinematic"};
    int tier = w.stageRays <= 25000u ? 0 : w.stageRays <= 90000u ? 1 : 2;
    const float rw = ui::SegmentedSize(detail, 3).x + ui::ButtonSize("Reset knobs").x + 70.0f * s + 20.0f * s;
    ui::BeginCard("##stquick", "Quick controls", "Each knob moves a whole group at once. Turn one and listen.", rw);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Detail");
    ImGui::SameLine(0, 8.0f * s);
    if (ui::Segmented("##sttier", &tier, detail, 3)) {
        static const uint32_t rays[3] = {15000u, 40000u, 150000u};
        w.stageRays = rays[tier];
        doc.MarkDirty();
    }
    ImGui::SameLine(0, 10.0f * s);
    if (ui::Button("Reset knobs")) {
        // Put the band back where the seating puts it.
        SeatBand(w, g.seat);
        w.stageYaw = 0.0f;
        w.stageTailTrim = 1.0f;
        g.amount = 100.0f;
        doc.MarkDirty();
    }
    ui::CardHeaderEnd();

    auto applyMac = [&]() {
        if (!g.baseValid) {
            g.baseYaw = w.stageYaw;
            for (int i = 0; i < 16; ++i) {
                const Polar p = ToPolar(w, w.stageChannel[i]);
                g.basePolar[i][0] = p.az; g.basePolar[i][1] = p.el; g.basePolar[i][2] = p.di;
            }
            g.baseValid = true;
        }
        const float yaw = w.stageYaw;
        w.stageYaw = g.baseYaw;
        for (int i = 0; i < 16; ++i)
            SetPolar(w, w.stageChannel[i], Clamp(g.basePolar[i][0] * g.spread, -180, 180),
                     Clamp(g.basePolar[i][1] + g.lift, -60, 85), Clamp(g.basePolar[i][2] * g.closeness, 0.5f, 60));
        w.stageYaw = yaw;
        FitAll(w);
    };
    auto roomScale = [&](float r) {
        const float nw = Clamp(w.stageRoom[0] * r, 3, 100), nd = Clamp(w.stageRoom[1] * r, 3, 120), nh = Clamp(w.stageRoom[2] * r, 2.2f, 60);
        const float sx = (nw - w.stageRoom[0]) * 0.5f, sy = (nd - w.stageRoom[1]) * 0.5f;
        for (auto& c : w.stageChannel) { c[0] += sx; c[1] += sy; }
        w.stageListener[0] += sx; w.stageListener[1] += sy;
        for (auto& f : w.stageFurniture) { f.x += sx; f.y += sy; }
        w.stageRoom = {nw, nd, nh};
        if (g.level != 3) AutoFurnish(w, g.level);
        FitAll(w);
    };

    struct Q {
        const char* id; ui::KnobSpec spec; const char* sub;
    };
    static const Q qs[6] = {
        {"stqSpread", {"Spread", 0.2f, 2.0f, 1.0f, 0.01f, false, "%.0f%%", PctDisp}, "How wide the band fans out around you"},
        {"stqDist", {"Distance", 0.4f, 2.5f, 1.0f, 0.01f, false, "%.0f%%", PctDisp}, "Moves everyone closer or farther away"},
        {"stqHeight", {"Height", -30.0f, 45.0f, 0.0f, 1.0f, false, "%.0f\xC2\xB0"}, "Lifts everyone above ear level"},
        {"stqSize", {"Room size", 0.5f, 2.5f, 1.0f, 0.01f, false, "%.0f%%", PctDisp}, "Grows the room around the band"},
        {"stqFace", {"Facing", -180.0f, 180.0f, 0.0f, 1.0f, false, "%.0f\xC2\xB0"}, "Turn your head, the band stays put"},
        {"stqAmount", {"Room amount", 0.0f, 150.0f, 100.0f, 1.0f, false, "%.0f%%"}, "How much of the room you hear"},
    };
    const ImVec2 ks = ui::KnobSize();
    const float avail = ImGui::GetContentRegionAvail().x;
    const int perRow = (std::max)(1, (std::min)(6, static_cast<int>(avail / (150.0f * s))));
    const float slot = avail / perRow;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float cellH = ks.y + 42.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 6; ++i) {
        const int r = i / perRow, c = i % perRow;
        const ImVec2 pos(o.x + c * slot + (slot - ks.x) * 0.5f, o.y + r * cellH);
        ImGui::SetCursorScreenPos(pos);
        ui::Anchor(qs[i].id, pos, ImVec2(pos.x + ks.x, pos.y + ks.y));
        float v = i == 0 ? g.spread : i == 1 ? g.closeness : i == 2 ? g.lift : i == 3 ? g.size : i == 4 ? w.stageYaw : g.amount;
        if (ui::Knob(qs[i].id, &v, qs[i].spec)) {
            if (i == 0) { g.spread = v; applyMac(); }
            else if (i == 1) { g.closeness = v; applyMac(); }
            else if (i == 2) { g.lift = v; applyMac(); }
            else if (i == 3) { const float q = v / g.size; g.size = v; roomScale(q); g.baseValid = false; }
            else if (i == 4) w.stageYaw = std::round(v);
            else { g.amount = std::round(v); w.stageTailTrim = g.amount / 100.0f; }
            doc.MarkDirty();
        }
        ui::PushText(0, 11.0f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(o.x + c * slot + 8.0f * s, pos.y + ks.y + 2.0f * s), ui::Tok().muted,
                    qs[i].sub, nullptr, slot - 16.0f * s);
        ui::PopFont();
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(avail, ((6 + perRow - 1) / perRow) * cellH));
    ui::EndCard();
}

void RoomAndFurniture(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = S();
    char sub[96];
    std::snprintf(sub, sizeof(sub), "%s, %s. Click to change.", g.roomIdx >= 0 && g.roomIdx < 5 ? kRooms[g.roomIdx].name : "Custom room",
                  w.stageFurniture.empty() ? "empty" : (std::to_string(w.stageFurniture.size()) + " furniture items").c_str());
    if (!ui::Disclosure("stageroom", "Room and furniture", sub)) return;
    ui::BeginReveal("stageroom");
    ui::BeginCard("##stroom", "Room", "The shape sets how long the room rings and how bright it sounds.");
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const int n = 5;
        const float gap = 10.0f * s, cw = (avail - gap * (n - 1)) / n, ch = 64.0f * s;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < n; ++i) {
            const ImVec2 a(o.x + i * (cw + gap), o.y), b(a.x + cw, a.y + ch);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            ImGui::InvisibleButton("##room", ImVec2(cw, ch));
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemClicked()) {
                g.level = g.level == 3 ? 1 : g.level;
                ApplyRoom(w, i, true);
                doc.MarkDirty();
            }
            const bool sel = g.roomIdx == i;
            dl->AddRectFilled(a, b, sel ? ui::WithAlpha(k.acc, 0.10f) : k.card2, 12.0f * s);
            dl->AddRect(a, b, sel ? k.acc : k.line2, 12.0f * s);
            // Footprint.
            const RoomDef& r = kRooms[i];
            const float m = (std::max)(r.w, r.d), fw = r.w / m * 34.0f * s, fd = r.d / m * 28.0f * s;
            const ImVec2 fa(a.x + 14.0f * s, a.y + 10.0f * s);
            const bool open = r.mats[0] == 11;
            dl->AddRectFilled(fa, ImVec2(fa.x + fw, fa.y + fd), open ? IM_COL32(255, 255, 255, 10) : WallCol(kMatAbs[r.mats[2]], 0.25f), 3.0f * s);
            dl->AddRect(fa, ImVec2(fa.x + fw, fa.y + fd), open ? IM_COL32(255, 255, 255, 50) : WallCol(kMatAbs[r.mats[2]], 0.9f), 3.0f * s);
            ui::PushText(2, 13.0f);
            dl->AddText(ImVec2(a.x + 14.0f * s, a.y + 42.0f * s), k.text, r.name);
            ui::PopFont();
            char dim[40];
            std::snprintf(dim, sizeof(dim), "%.0f x %.0f x %.0f m", r.w, r.d, r.h);
            ui::PushNum(10.0f);
            dl->AddText(ImVec2(a.x + 14.0f * s + 40.0f * s, a.y + 12.0f * s), k.muted, dim);
            ui::PopFont();
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(o);
        ImGui::Dummy(ImVec2(avail, ch));
    }
    ImGui::Dummy(ImVec2(0, 8.0f * s));
    {
        ui::BeginRow("stageSeat", "Where the band sits", "Seats all 16 channels around you in one of four layouts.", ui::SegmentedSize(kSeatNames, 4));
        int seat = g.seat;
        if (ui::Segmented("##stseat", &seat, kSeatNames, 4)) {
            SeatBand(w, seat);
            doc.MarkDirty();
        }
        ui::EndRow();
    }
    ui::EndCard();

    ui::BeginCard("##stfurn", "Furniture", "Soft things absorb, hard things reflect, tall things block the direct sound.");
    {
        static const char* const lv[3] = {"Empty", "Cosy", "Packed"};
        ui::BeginRow("stageFurnLevel", "How full is the room?", "An audience, a sofa and a rug all change how long the room rings.", ui::SegmentedSize(lv, 3));
        int level = g.level > 2 ? -1 : g.level;
        if (ui::Segmented("##stlevel", &level, lv, 3) && level >= 0) {
            AutoFurnish(w, level);
            doc.MarkDirty();
        }
        ui::EndRow();
    }
    ImGui::Dummy(ImVec2(0, 6.0f * s));
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        float x = 0;
        for (int i = 0; i < 13; ++i) {
            char label[48];
            std::snprintf(label, sizeof(label), "+ %s", kFurn[i].name);
            const float bw = ui::ButtonSize(label).x;
            if (x > 0 && x + bw > avail) x = 0;
            else if (x > 0) ImGui::SameLine(0, 8.0f * s);
            ImGui::PushID(i);
            if (ui::Button(label)) {
                const FurnType& t = kFurn[i];
                float fx = w.stageRoom[0] * 0.7f, fy = w.stageRoom[1] * 0.62f;
                bool rot = false;
                if (i == Curtains) { fx = w.stageRoom[0] - 0.12f; fy = w.stageRoom[1] * 0.5f; rot = true; }
                if (i == Audience) fy = w.stageRoom[1] * 0.4f;
                (void)t;
                AddFurn(w, i, fx, fy, rot);
                g.fsel = static_cast<int>(w.stageFurniture.size()) - 1;
                g.you = false;
                g.level = 3;
                doc.MarkDirty();
            }
            ImGui::PopID();
            x += bw + 8.0f * s;
        }
    }
    ImGui::Dummy(ImVec2(0, 8.0f * s));
    {
        const bool has = g.fsel >= 0 && g.fsel < static_cast<int>(w.stageFurniture.size());
        if (ui::Button("Turn 90\xC2\xB0", ui::ButtonKind::Normal, ImVec2(0, 0), has) && has) {
            w.stageFurniture[g.fsel].rotated = !w.stageFurniture[g.fsel].rotated;
            g.level = 3;
            doc.MarkDirty();
        }
        ImGui::SameLine(0, 8.0f * s);
        if (ui::Button("Remove", ui::ButtonKind::Normal, ImVec2(0, 0), has) && has) {
            w.stageFurniture.erase(w.stageFurniture.begin() + g.fsel);
            g.fsel = -1;
            g.level = 3;
            doc.MarkDirty();
        }
        ImGui::SameLine(0, 8.0f * s);
        if (ui::Button("Remove all", ui::ButtonKind::Normal, ImVec2(0, 0), !w.stageFurniture.empty())) {
            w.stageFurniture.clear();
            g.fsel = -1;
            g.level = 0;
            doc.MarkDirty();
        }
        ImGui::Dummy(ImVec2(0, 6.0f * s));
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::PushTextWrapPos(0.0f);
        if (has) {
            const FurnType& t = kFurn[w.stageFurniture[g.fsel].kind];
            ImGui::Text("%s: %s", t.name, t.why);
        } else {
            ImGui::TextUnformatted("Click a piece of furniture on the stage to select it, then drag, turn or remove it.");
        }
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    ui::EndCard();
    ui::EndReveal();
}

void ListenCard(ConfigDocument& doc) {
    static StageAudition audition;
    ConfigValues& w = doc.Working();
    const float s = S();
    ui::BeginCard("##stlisten", "Listen", "Plays through the room as it is set up now, saved or not, until you press Stop. Edit the room or move a channel while it plays and the sound follows.");
    struct B { const char* label; StageAudition::Mode mode; ui::ButtonKind kind; const char* tip; };
    static const B buttons[] = {
        {"Play selected channel", StageAudition::Mode::Channel, ui::ButtonKind::Primary, "The channel you picked plays an arpeggio from where it stands."},
        {"Play each channel in turn", StageAudition::Mode::Tour, ui::ButtonKind::Normal, "One note per channel, 1 to 16, so you hear where each one is."},
        {"Play 5.1 test (6 tracks)", StageAudition::Mode::Surround, ui::ButtonKind::Normal, "Seats channels 1 to 6 as front left, front right, centre, bass, rear left, rear right and plays a surround test on them."},
        {"Play music loop", StageAudition::Mode::Music, ui::ButtonKind::Normal, "The loop plays through the selected channel until you press Stop. Move it or change the room meanwhile."},
        {"Loop around me", StageAudition::Mode::Around, ui::ButtonKind::Normal, "The loop circles your head once per pass, with Doppler."},
        {"Walk around me", StageAudition::Mode::Walk, ui::ButtonKind::Normal, "The selected channel circles you, with Doppler."},
    };
    const float avail = ImGui::GetContentRegionAvail().x;
    float x = 0;
    bool first = true;
    for (const B& b : buttons) {
        const float bw = ui::ButtonSize(b.label).x;
        if (!first && x + bw > avail) x = 0;
        else if (!first) ImGui::SameLine(0, 8.0f * s);
        first = false;
        if (ui::Button(b.label, b.kind)) {
            if (b.mode == StageAudition::Mode::Surround) {
                static const float seat51[6][3] = {{-30, 0, 4}, {30, 0, 4}, {0, 0, 4}, {0, -12, 3}, {-140, 0, 4}, {140, 0, 4}};
                for (int i = 0; i < 6; ++i) SetPolar(w, w.stageChannel[i], seat51[i][0], seat51[i][1], seat51[i][2]);
                g.sel = 0;
                g.you = false;
                g.fsel = -1;
                g.baseValid = false;
                doc.MarkDirty();
            }
            audition.Start(b.mode, w, g.sel);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", b.tip);
        x += bw + 8.0f * s;
    }
    auto place = [&](const char* label) {
        const float bw = ui::ButtonSize(label).x;
        if (x + bw > avail) x = 0;
        else ImGui::SameLine(0, 8.0f * s);
        x += bw + 8.0f * s;
    };
    place("Stop");
    if (ui::Button("Stop")) audition.Stop();
    place("Hold: hear it without the room");
    ui::Button("Hold: hear it without the room");
    audition.SetDry(ImGui::IsItemActive());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hold the button to hear the same sound with no room and no reflections.");
    audition.Update(w, g.sel);
    ImGui::Dummy(ImVec2(0, 6.0f * s));
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
    ImGui::TextUnformatted(StageAudition::Supported() ? audition.Status().c_str() : "Listening tests need Windows");
    ImGui::PopStyleColor();
    ui::PopFont();
    ui::EndCard();
}

void AdvancedCards(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = S();

    // Selected channel.
    {
        const bool chan = !g.you && g.fsel < 0;
        char title[48];
        std::snprintf(title, sizeof(title), chan ? "%s (channel %d)" : g.you ? "You" : "Furniture", kNames[g.sel], g.sel + 1);
        ui::BeginCard("##stsel", title, chan ? "Place this channel by direction, height and distance from you." : "Select a channel to place it by numbers.");
        if (chan) {
            Polar p = ToPolar(w, w.stageChannel[g.sel]);
            auto slider = [&](const char* anchor, const char* name, const char* desc, float* v, float lo, float hi, const char* fmt) {
                const ImVec2 sz(360.0f * s, 28.0f * s);
                ui::BeginRow(anchor, name, desc, sz);
                bool changed = false;
                const float vw = 64.0f * s;
                changed = ui::RangeSlider(anchor, v, lo, hi, sz.x - vw - 10.0f * s);
                ImGui::SameLine(0, 10.0f * s);
                char b[24];
                std::snprintf(b, sizeof(b), fmt, *v);
                ui::PushNum(12.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(b);
                ui::PopFont();
                ui::EndRow();
                return changed;
            };
            bool ch = false;
            ch |= slider("##stAz", "Direction", "0\xC2\xB0 is straight ahead, positive is to your right.", &p.az, -180, 180, "%.0f\xC2\xB0");
            ch |= slider("##stEl", "Height", "Above or below ear level.", &p.el, -60, 90, "%.0f\xC2\xB0");
            ch |= slider("##stDi", "Distance", "Farther is quieter, duller and roomier.", &p.di, 0.5f, 30, "%.1f m");
            if (ch) {
                SetPolar(w, w.stageChannel[g.sel], p.az, p.el, p.di);
                g.baseValid = false;
                doc.MarkDirty();
            }
        } else {
            ImGui::Dummy(ImVec2(0, 2.0f * s));
        }
        ui::EndCard();
    }

    // Room.
    {
        char sub[64];
        std::snprintf(sub, sizeof(sub), "Rings for about %.1f s", Rt60(w));
        ui::BeginCard("##stroomadv", "Room", sub);
        static const ui::KnobSpec kw = {"Width", 3.0f, 100.0f, 24.0f, 0.5f, false, "%.1f m"};
        static const ui::KnobSpec kd = {"Depth", 3.0f, 120.0f, 36.0f, 0.5f, false, "%.1f m"};
        static const ui::KnobSpec kh = {"Height", 2.2f, 60.0f, 14.0f, 0.5f, false, "%.1f m"};
        const ImVec2 ks = ui::KnobSize();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float slot = (std::max)(ks.x + 30.0f * s, 120.0f * s);
        float* dims[3] = {&w.stageRoom[0], &w.stageRoom[1], &w.stageRoom[2]};
        const ui::KnobSpec* specs[3] = {&kw, &kd, &kh};
        static const char* ids[3] = {"stRoomW", "stRoomD", "stRoomH"};
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(o.x + i * slot, o.y));
            if (ui::Knob(ids[i], dims[i], *specs[i])) {
                g.roomIdx = -1;
                g.level = g.level == 3 ? 3 : g.level;
                FitAll(w);
                doc.MarkDirty();
            }
        }
        ImGui::SetCursorScreenPos(o);
        ImGui::Dummy(ImVec2(slot * 3, ks.y));
        ImGui::Dummy(ImVec2(0, 6.0f * s));
        static const char* const surf[6] = {"Floor", "Ceiling", "Left wall", "Right wall", "Front wall", "Back wall"};
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = avail > 700.0f * s ? 3 : 2;
        const float cw = (avail - 12.0f * s * (cols - 1)) / cols;
        const ImVec2 so = ImGui::GetCursorScreenPos();
        for (int i = 0; i < 6; ++i) {
            const int r = i / cols, c = i % cols;
            ImGui::SetCursorScreenPos(ImVec2(so.x + c * (cw + 12.0f * s), so.y + r * 62.0f * s));
            ui::PushText(0, 12.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
            ImGui::TextUnformatted(surf[i]);
            ImGui::PopStyleColor();
            ui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(so.x + c * (cw + 12.0f * s), so.y + r * 62.0f * s + 20.0f * s));
            int m = w.stageSurface[i];
            char id[16];
            std::snprintf(id, sizeof(id), "##stsurf%d", i);
            if (ui::Select(id, &m, kMatName, 12, cw)) {
                w.stageSurface[i] = static_cast<uint8_t>(m);
                g.roomIdx = -1;
                doc.MarkDirty();
            }
        }
        ImGui::SetCursorScreenPos(so);
        ImGui::Dummy(ImVec2(avail, ((6 + cols - 1) / cols) * 62.0f * s));
        ui::EndCard();
    }

    // Quality.
    {
        ui::BeginCard("##stqual", "Quality", "What the stage spends at start-up and in the audio callback.");
        {
            ui::BeginRow("stageRays", "Tail quality", "Rays traced through the room to measure its tail. The GPU does it in a blink when it can.", ImVec2(360.0f * s, 28.0f * s));
            float r = std::log10(static_cast<float>(w.stageRays));
            if (ui::RangeSlider("##strays", &r, std::log10(8000.0f), std::log10(400000.0f), 290.0f * s)) {
                w.stageRays = static_cast<uint32_t>(std::pow(10.0f, r) / 1000.0f + 0.5f) * 1000u;
                doc.MarkDirty();
            }
            ImGui::SameLine(0, 10.0f * s);
            char b[24];
            std::snprintf(b, sizeof(b), "%uk", w.stageRays / 1000u);
            ui::PushNum(12.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(b);
            ui::PopFont();
            ui::EndRow();
        }
        auto toggleRow = [&](const char* anchor, const char* name, const char* desc, bool* v) {
            ui::BeginRow(anchor, name, desc, ImVec2(46.0f * s, 28.0f * s));
            if (ui::Toggle(anchor, v)) doc.MarkDirty();
            ui::EndRow();
        };
        toggleRow("stageEar", "3D headphone sound", "Makes direction and height audible on headphones. Turn off for speakers.", &w.stageEar);
        toggleRow("stageAir", "Air absorption", "Distant sound loses highs, like outdoors on a humid day.", &w.stageAir);
        toggleRow("stageKeep", "Keep the driver reverb", "Run the normal reverb after the stage as well.", &w.stageKeepReverb);
        ui::EndCard();
    }
}

} // namespace

void DrawStagePage(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    // A fresh config has every channel at one point: seat the band like the default scene.
    {
        bool stacked = true;
        for (int i = 1; i < 16; ++i)
            if (w.stageChannel[i] != w.stageChannel[0]) stacked = false;
        if (stacked) {
            g.seat = Orch;
            g.level = 1;
            ApplyRoom(w, 2, true);
        }
    }
    g.sel = (std::max)(0, (std::min)(15, g.sel));
    if (g.fsel >= static_cast<int>(w.stageFurniture.size())) g.fsel = -1;
    HeaderBar(doc);
    ImGui::Dummy(ImVec2(0, 6.0f * S()));
    ScenesCard(doc);
    StageCard(doc);
    ChannelsCard(doc);
    QuickCard(doc);
    RoomAndFurniture(doc);
    ListenCard(doc);
    if (g.mode == 1) AdvancedCards(doc);
}

} // namespace svms::cfg
