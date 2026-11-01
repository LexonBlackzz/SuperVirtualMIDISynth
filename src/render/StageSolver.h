#pragma once

// Room acoustics for the 3D stage. Everything in here is pure: a room, a
// source and a listener go in, reflection lists, decay times and impulse
// responses come out. Nothing touches the audio thread and no function keeps
// hidden state between calls, so two sources never share results.
//
// Coordinates: x runs left to right, y front to back, z up, in metres. The
// room is a box with its corner at the origin. Wall order everywhere is
// floor, ceiling, left (x = 0), right (x = width), front (y = 0), back
// (y = depth).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace svms {
namespace stage {

constexpr int kBands = 4;                    // 125 Hz, 500 Hz, 2 kHz, 8 kHz
constexpr float kSpeedOfSound = 343.0f;
constexpr float kHeadRadius = 0.0875f;
constexpr int kWalls = 6;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline float Distance(const Vec3& a, const Vec3& b) noexcept {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

// ── Materials ─────────────────────────────────────────────────────────────
enum class Material : uint8_t {
    Plaster, Stone, Concrete, Wood, Glass, Carpet, Seating, Curtain, Foam,
    Books, Foliage, Open, Count
};

// Absorption coefficient per band. Open is a perfect absorber (nothing comes
// back), which is how an open field is modelled.
inline const float* Absorption(Material m) noexcept {
    static const float table[static_cast<int>(Material::Count)][kBands] = {
        {0.02f, 0.03f, 0.04f, 0.05f},   // Plaster
        {0.02f, 0.03f, 0.04f, 0.06f},   // Stone
        {0.01f, 0.02f, 0.03f, 0.04f},   // Concrete
        {0.15f, 0.10f, 0.06f, 0.07f},   // Wood
        {0.18f, 0.06f, 0.03f, 0.02f},   // Glass
        {0.08f, 0.24f, 0.57f, 0.69f},   // Carpet
        {0.25f, 0.45f, 0.60f, 0.60f},   // Seating
        {0.10f, 0.45f, 0.75f, 0.72f},   // Curtain
        {0.20f, 0.70f, 0.95f, 0.95f},   // Foam
        {0.15f, 0.25f, 0.35f, 0.35f},   // Books
        {0.10f, 0.20f, 0.30f, 0.35f},   // Foliage
        {1.00f, 1.00f, 1.00f, 1.00f},   // Open
    };
    return table[static_cast<int>(m)];
}

// Air takes energy out of the room in proportion to its volume (Sabine's 4mV
// term) and out of a single path in proportion to its length.
constexpr float kAirAbsorption[kBands] = {0.0003f, 0.0024f, 0.012f, 0.12f};
constexpr float kAirLossDbPerMetre[kBands] = {0.0f, 0.0025f, 0.015f, 0.1f};

// ── Furniture ─────────────────────────────────────────────────────────────
enum class FurnitureKind : uint8_t {
    Sofa, Armchair, Bookshelf, Table, Piano, Rug, Curtains, Plant, Audience,
    Pillar, Partition, Speaker, Subwoofer, Count
};

struct FurnitureInfo {
    float width, depth, height;
    Material material;
    bool blocks;     // stands between a source and the listener
    bool flat;       // lies on the floor (a rug): changes the floor only
    bool fullHeight; // floor to ceiling
};

inline const FurnitureInfo& Info(FurnitureKind k) noexcept {
    static const FurnitureInfo table[static_cast<int>(FurnitureKind::Count)] = {
        {2.0f, 0.9f, 0.8f, Material::Seating, true, false, false},   // Sofa
        {0.9f, 0.9f, 0.9f, Material::Seating, true, false, false},   // Armchair
        {1.4f, 0.35f, 2.0f, Material::Books, true, false, false},    // Bookshelf
        {1.6f, 0.9f, 0.75f, Material::Wood, false, false, false},    // Table
        {1.5f, 1.6f, 1.0f, Material::Wood, true, false, false},      // Piano
        {3.0f, 2.0f, 0.02f, Material::Carpet, false, true, false},   // Rug
        {2.4f, 0.15f, 2.4f, Material::Curtain, false, false, false}, // Curtains
        {0.5f, 0.5f, 1.4f, Material::Foliage, false, false, false},  // Plant
        {6.0f, 2.5f, 1.1f, Material::Seating, false, false, false},  // Audience
        {1.0f, 1.0f, 99.0f, Material::Stone, true, false, true},     // Pillar
        {3.0f, 0.1f, 2.0f, Material::Plaster, true, false, false},   // Partition
        {0.26f, 0.3f, 0.9f, Material::Wood, true, false, false},     // Speaker
        {0.42f, 0.42f, 0.4f, Material::Wood, true, false, false},    // Subwoofer
    };
    return table[static_cast<int>(k)];
}

struct Furniture {
    FurnitureKind kind = FurnitureKind::Sofa;
    float x = 0.0f, y = 0.0f;   // floor position of the centre
    bool rotated = false;       // turned a quarter turn
};

struct Box {
    Vec3 lo, hi;
};

struct Room {
    float width = 24.0f, depth = 36.0f, height = 14.0f;
    Material surface[kWalls] = {Material::Seating, Material::Plaster,
                                Material::Wood,    Material::Wood,
                                Material::Wood,    Material::Curtain};
    std::vector<Furniture> furniture;
};

inline Box FurnitureBox(const Room& room, const Furniture& f) noexcept {
    const FurnitureInfo& info = Info(f.kind);
    const float w = f.rotated ? info.depth : info.width;
    const float d = f.rotated ? info.width : info.depth;
    const float h = info.fullHeight ? room.height : info.height;
    return Box{{f.x - 0.5f * w, f.y - 0.5f * d, 0.0f},
               {f.x + 0.5f * w, f.y + 0.5f * d, h}};
}

// ── Room totals ───────────────────────────────────────────────────────────
inline void WallAreas(const Room& r, float out[kWalls]) noexcept {
    out[0] = out[1] = r.width * r.depth;
    out[2] = out[3] = r.depth * r.height;
    out[4] = out[5] = r.width * r.height;
}

// Volume the sound actually has, with the furniture taken out (never less
// than half the empty box).
inline float RoomVolume(const Room& r) noexcept {
    const float full = r.width * r.depth * r.height;
    float solid = 0.0f;
    for (const Furniture& f : r.furniture) {
        const FurnitureInfo& info = Info(f.kind);
        if (info.flat) continue;
        const Box b = FurnitureBox(r, f);
        solid += (b.hi.x - b.lo.x) * (b.hi.y - b.lo.y) * (b.hi.z - b.lo.z);
    }
    return (std::max)(full * 0.5f, full - solid);
}

// Total absorption (square metres of open window) per band: walls, furniture
// and the air itself.
inline void TotalAbsorption(const Room& r, float out[kBands]) noexcept {
    float area[kWalls];
    WallAreas(r, area);
    const float fullVolume = r.width * r.depth * r.height;
    for (int b = 0; b < kBands; ++b) {
        float a = kAirAbsorption[b] * fullVolume;
        for (int w = 0; w < kWalls; ++w) a += area[w] * Absorption(r.surface[w])[b];
        for (const Furniture& f : r.furniture) {
            const FurnitureInfo& info = Info(f.kind);
            const Box box = FurnitureBox(r, f);
            const float w = box.hi.x - box.lo.x, d = box.hi.y - box.lo.y,
                        h = box.hi.z - box.lo.z;
            const float mat = Absorption(info.material)[b];
            if (info.flat) {
                // A rug only changes what the floor under it absorbs.
                a += w * d * (std::max)(0.0f, mat - Absorption(r.surface[0])[b]);
            } else {
                a += mat * (w * d + 2.0f * (w + d) * h);
            }
        }
        out[b] = a;
    }
}

// Decay time (60 dB) per band from Sabine's formula. The lowest band is kept
// within 1.5 times the 500 Hz value and nothing rings longer than 9 s, so a
// room cannot boom beyond what is believable.
inline void ReverbTimes(const Room& r, float t60[kBands]) noexcept {
    float absorption[kBands];
    TotalAbsorption(r, absorption);
    const float volume = RoomVolume(r);
    for (int b = 0; b < kBands; ++b) {
        t60[b] = absorption[b] <= 0.001f
                     ? 0.0f
                     : (std::min)(9.0f, 0.161f * volume / absorption[b]);
    }
    t60[0] = (std::min)(t60[0], 1.5f * (std::max)(t60[1], 0.05f));
}

// Distance at which the direct sound and the room are equally loud.
inline float CriticalDistance(const Room& r, float meanT60) noexcept {
    const float t = (std::max)(0.15f, meanT60);
    return (std::max)(0.4f, 0.057f * std::sqrt(RoomVolume(r) / t));
}

// ── Blocking ──────────────────────────────────────────────────────────────
// Segment-versus-box test in slab form.
inline bool SegmentHitsBox(const Vec3& a, const Vec3& b, const Box& box) noexcept {
    const float o[3] = {a.x, a.y, a.z};
    const float dv[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
    const float lo[3] = {box.lo.x, box.lo.y, box.lo.z};
    const float hi[3] = {box.hi.x, box.hi.y, box.hi.z};
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(dv[k]) < 1e-9f) {
            if (o[k] < lo[k] || o[k] > hi[k]) return false;
        } else {
            float ta = (lo[k] - o[k]) / dv[k], tb = (hi[k] - o[k]) / dv[k];
            if (ta > tb) std::swap(ta, tb);
            t0 = (std::max)(t0, ta);
            t1 = (std::min)(t1, tb);
            if (t0 > t1) return false;
        }
    }
    return true;
}

inline bool Occluded(const Room& r, const Vec3& source, const Vec3& listener) noexcept {
    for (const Furniture& f : r.furniture) {
        if (!Info(f.kind).blocks) continue;
        if (SegmentHitsBox(source, listener, FurnitureBox(r, f))) return true;
    }
    return false;
}

// ── Reflections ───────────────────────────────────────────────────────────
struct Reflection {
    Vec3 image;                 // where the sound seems to come from
    float path = 0.0f;          // total path length in metres
    float gain[kBands] = {};    // surface loss per band, amplitude, no distance term
    uint8_t order = 1;
    bool furniture = false;
    int8_t wall = -1;           // last wall hit, -1 for furniture
};

inline Vec3 MirrorAcrossWall(const Room& r, const Vec3& p, int wall) noexcept {
    Vec3 q = p;
    switch (wall) {
        case 0: q.z = -p.z; break;
        case 1: q.z = 2.0f * r.height - p.z; break;
        case 2: q.x = -p.x; break;
        case 3: q.x = 2.0f * r.width - p.x; break;
        case 4: q.y = -p.y; break;
        default: q.y = 2.0f * r.depth - p.y; break;
    }
    return q;
}

inline void SurfaceGain(Material m, float out[kBands]) noexcept {
    const float* a = Absorption(m);
    for (int b = 0; b < kBands; ++b) out[b] = std::sqrt((std::max)(0.0f, 1.0f - a[b]));
}

// Single reflections off the furniture: rays leave the source in a fixed fan
// of directions, the first box each one meets becomes a reflection, and the
// shortest few paths to the listener are kept.
inline void FurnitureReflections(const Room& r, const Vec3& source,
                                 const Vec3& listener, int keep,
                                 std::vector<Reflection>& out) {
    if (r.furniture.empty()) return;
    constexpr int kRays = 96;
    std::vector<Reflection> hits;
    for (int i = 0; i < kRays; ++i) {
        const float z = 1.0f - 2.0f * (i + 0.5f) / kRays;
        const float rad = std::sqrt((std::max)(0.0f, 1.0f - z * z));
        const float ph = static_cast<float>(i) * 2.399963f;
        const float dir[3] = {rad * std::cos(ph), rad * std::sin(ph), z};
        const float o[3] = {source.x, source.y, source.z};
        float bestT = 1e30f;
        const Furniture* bestF = nullptr;
        for (const Furniture& f : r.furniture) {
            const Box bx = FurnitureBox(r, f);
            const float lo[3] = {bx.lo.x, bx.lo.y, bx.lo.z};
            const float hi[3] = {bx.hi.x, bx.hi.y, bx.hi.z};
            float t0 = 0.0f, t1 = 1e30f;
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                if (std::fabs(dir[k]) < 1e-9f) {
                    if (o[k] < lo[k] || o[k] > hi[k]) ok = false;
                } else {
                    float ta = (lo[k] - o[k]) / dir[k], tb = (hi[k] - o[k]) / dir[k];
                    if (ta > tb) std::swap(ta, tb);
                    t0 = (std::max)(t0, ta);
                    t1 = (std::min)(t1, tb);
                    if (t0 > t1) ok = false;
                }
            }
            if (ok && t0 > 1e-4f && t0 < bestT) { bestT = t0; bestF = &f; }
        }
        if (!bestF) continue;
        const Vec3 b{source.x + dir[0] * bestT, source.y + dir[1] * bestT,
                     source.z + dir[2] * bestT};
        const float dl = Distance(b, listener);
        if (dl < 0.3f) continue;
        Reflection rf;
        rf.path = bestT + dl;
        rf.image = Vec3{listener.x + (b.x - listener.x) / dl * rf.path,
                        listener.y + (b.y - listener.y) / dl * rf.path,
                        listener.z + (b.z - listener.z) / dl * rf.path};
        SurfaceGain(Info(bestF->kind).material, rf.gain);
        rf.furniture = true;
        hits.push_back(rf);
    }
    std::sort(hits.begin(), hits.end(),
              [](const Reflection& a, const Reflection& b) { return a.path < b.path; });
    if (static_cast<int>(hits.size()) > keep) hits.resize(static_cast<size_t>(keep));
    out.insert(out.end(), hits.begin(), hits.end());
}

// Mirror-image reflections off the walls up to the given order (1 to 3),
// followed by the furniture reflections. Second and third order keep only the
// shortest 18 and 38 paths.
inline std::vector<Reflection> FindReflections(const Room& r, const Vec3& source,
                                               const Vec3& listener, int order) {
    std::vector<Reflection> all;
    for (int w = 0; w < kWalls; ++w) {
        Reflection rf;
        rf.image = MirrorAcrossWall(r, source, w);
        rf.path = Distance(rf.image, listener);
        SurfaceGain(r.surface[w], rf.gain);
        rf.order = 1;
        rf.wall = static_cast<int8_t>(w);
        all.push_back(rf);
    }
    FurnitureReflections(r, source, listener, 8, all);
    std::vector<Reflection> level;
    for (const Reflection& rf : all) if (rf.wall >= 0) level.push_back(rf);
    for (int o = 2; o <= (std::min)(order, 3); ++o) {
        std::vector<Reflection> next;
        for (const Reflection& rf : level) {
            for (int w = 0; w < kWalls; ++w) {
                if (w == rf.wall) continue;
                Reflection n;
                n.image = MirrorAcrossWall(r, rf.image, w);
                n.path = Distance(n.image, listener);
                float g[kBands];
                SurfaceGain(r.surface[w], g);
                for (int b = 0; b < kBands; ++b) n.gain[b] = rf.gain[b] * g[b];
                n.order = static_cast<uint8_t>(o);
                n.wall = static_cast<int8_t>(w);
                next.push_back(n);
            }
        }
        std::stable_sort(next.begin(), next.end(),
                         [](const Reflection& a, const Reflection& b) { return a.path < b.path; });
        const size_t cap = o == 2 ? 18u : 38u;
        if (next.size() > cap) next.resize(cap);
        all.insert(all.end(), next.begin(), next.end());
        level = next;
    }
    return all;
}

// ── Direct path ───────────────────────────────────────────────────────────
// Where a point is relative to a listener who faces `yawDegrees` clockwise
// from the +y direction.
struct Relative {
    float right = 0.0f, forward = 0.0f, up = 0.0f, distance = 0.0f;
};

inline Relative RelativeTo(const Vec3& p, const Vec3& listener, float yawDegrees) noexcept {
    const float a = yawDegrees * 0.017453292519943295f;
    const float dx = p.x - listener.x, dy = p.y - listener.y, dz = p.z - listener.z;
    Relative out;
    out.right = dx * std::cos(a) - dy * std::sin(a);
    out.forward = dx * std::sin(a) + dy * std::cos(a);
    out.up = dz;
    out.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return out;
}

struct DirectPath {
    float delaySeconds = 0.0f;
    float gain = 1.0f;
    float lowpassHz = 20000.0f;
    float azimuthDegrees = 0.0f;   // 0 straight ahead, positive to the right
    float elevationDegrees = 0.0f;
    float distance = 0.0f;
    bool occluded = false;
};

inline float BehindFactor(const Relative& rel) noexcept {
    const float r = (std::max)(0.1f, rel.distance);
    return (std::max)(0.0f, -rel.forward / r);
}

// Highs a listener loses to a source behind them.
inline float BehindLowpassHz(float behind) noexcept {
    return (std::max)(2500.0f, 20000.0f * std::pow(0.2f, behind * 2.2f));
}

inline DirectPath SolveDirect(const Room& r, const Vec3& source, const Vec3& listener,
                              float yawDegrees, bool airAbsorption, bool earModel) noexcept {
    DirectPath d;
    const Relative rel = RelativeTo(source, listener, yawDegrees);
    d.distance = (std::max)(0.5f, rel.distance);
    d.delaySeconds = (std::min)(0.5f, d.distance / kSpeedOfSound);
    d.occluded = Occluded(r, source, listener);
    d.gain = (d.occluded ? 0.4f : 1.0f) / d.distance;
    float cut = airAbsorption ? (std::max)(2500.0f, 20000.0f * std::exp(-d.distance / 55.0f)) : 20000.0f;
    if (d.occluded) cut = (std::min)(cut, 2200.0f);
    if (earModel) cut = (std::min)(cut, BehindLowpassHz(BehindFactor(rel)));
    d.lowpassHz = cut;
    d.azimuthDegrees = std::atan2(rel.right, rel.forward) * 57.29577951308232f;
    d.elevationDegrees = std::atan2(rel.up, std::hypot(rel.right, rel.forward)) * 57.29577951308232f;
    return d;
}

// ── Early response ────────────────────────────────────────────────────────
struct EarlyResponse {
    std::vector<float> left, right;
};

// Level of the early reflections against the direct sound's 1/d, chosen by
// ear for a natural balance at the listening position.
constexpr float kEarlyLevel = 0.17f;

namespace detail {
inline uint32_t Hash32(uint32_t a) noexcept {
    a += 0x6D2B79F5u;
    uint32_t t = (a ^ (a >> 15)) * (1u | a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return t ^ (t >> 14);
}
// Small deterministic generator: the same tap always gets the same noise, so
// rebuilding a response never changes its texture.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) noexcept : s(seed) {}
    float Next() noexcept {   // [0, 1)
        s += 0x6D2B79F5u;
        uint32_t t = (s ^ (s >> 15)) * (1u | s);
        t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
        return static_cast<float>((t ^ (t >> 14)) >> 8) * (1.0f / 16777216.0f);
    }
};
} // namespace detail

// Turns a reflection list into a two-ear impulse response. Each reflection is
// a short burst of noise, not a click: walls and furniture scatter sound, and
// later reflections smear more. Per band the burst carries the surface loss
// and the air absorption of its path. With `earModel` on, each ear also gets
// its own arrival time and a head-shadow and behind-you rolloff on the highs.
inline EarlyResponse BuildEarlyResponse(const std::vector<Reflection>& reflections,
                                        const Vec3& listener, float yawDegrees,
                                        bool earModel, bool airAbsorption,
                                        float sampleRate, float seconds = 0.42f) {
    EarlyResponse out;
    const int length = (std::max)(1, static_cast<int>(sampleRate * seconds));
    out.left.assign(static_cast<size_t>(length), 0.0f);
    out.right.assign(static_cast<size_t>(length), 0.0f);
    const float splits[3] = {250.0f, 1000.0f, 4000.0f};
    float alpha[3];
    for (int i = 0; i < 3; ++i)
        alpha[i] = 1.0f - std::exp(-2.0f * 3.14159265358979f * splits[i] / sampleRate);
    const int maxBurst = static_cast<int>(0.026f * sampleRate);
    std::vector<float> burst(static_cast<size_t>((std::max)(maxBurst, 8)));

    for (size_t idx = 0; idx < reflections.size(); ++idx) {
        const Reflection& rf = reflections[idx];
        const float tp = rf.path / kSpeedOfSound;
        if (tp > seconds - 0.02f) continue;
        const Relative rel = RelativeTo(rf.image, listener, yawDegrees);
        const float rr = (std::max)(0.1f, rel.distance);
        const float lat = (std::max)(-1.0f, (std::min)(1.0f, rel.right / rr));
        const float behind = BehindFactor(rel);
        const float amp = kEarlyLevel / (std::max)(1.0f, rf.path);
        float gb[kBands];
        for (int b = 0; b < kBands; ++b) {
            gb[b] = rf.gain[b] * (airAbsorption ? std::pow(10.0f, -kAirLossDbPerMetre[b] * rf.path / 20.0f) : 1.0f);
        }
        const float theta = std::asin(lat);
        const float itd = earModel ? kHeadRadius / kSpeedOfSound * (theta + std::sin(theta)) : 0.0f;
        const float pan = (lat + 1.0f) * 0.7853981633974483f;
        const float behindFactor = earModel ? (std::max)(0.12f, std::pow(0.2f, behind * 2.2f)) : 1.0f;
        int width = static_cast<int>(tp * sampleRate * 0.2f);
        width = (std::max)(6, (std::min)(static_cast<int>(burst.size()), width));

        for (int ear = 0; ear < 2; ++ear) {
            std::vector<float>& dst = ear == 0 ? out.left : out.right;
            const float levelPan = ear == 0 ? std::cos(pan) : std::sin(pan);
            const float arrival = ear == 0 ? tp + itd * 0.5f : tp - itd * 0.5f;
            const float shadowSide = ear == 0 ? (std::max)(0.0f, lat) : (std::max)(0.0f, -lat);
            const float f = earModel ? std::pow(0.18f, shadowSide) * behindFactor : 1.0f;
            const float g = levelPan * amp;
            const float q0 = gb[0] * g, q1 = gb[1] * g,
                        q2 = gb[2] * g * std::sqrt(f), q3 = gb[3] * g * f;
            const int start = static_cast<int>(std::lround((std::max)(0.0f, arrival) * sampleRate));
            detail::Rng rng(detail::Hash32(static_cast<uint32_t>(idx * 2u + static_cast<uint32_t>(ear) + 1u)));
            float energy = 0.0f;
            for (int j = 0; j < width; ++j) {
                const float x = (rng.Next() * 2.0f - 1.0f) * std::exp(-3.0f * static_cast<float>(j) / static_cast<float>(width));
                burst[static_cast<size_t>(j)] = x;
                energy += x * x;
            }
            const float scale = 1.0f / std::sqrt(energy > 0.0f ? energy : 1.0f);
            float l0 = 0.0f, l1 = 0.0f, l2 = 0.0f;
            const int run = width + 200;
            for (int j = 0; j < run && start + j < length; ++j) {
                const float x = j < width ? burst[static_cast<size_t>(j)] * scale : 0.0f;
                l0 += alpha[0] * (x - l0);
                l1 += alpha[1] * (x - l1);
                l2 += alpha[2] * (x - l2);
                dst[static_cast<size_t>(start + j)] += q0 * l0 + q1 * (l1 - l0) + q2 * (l2 - l1) + q3 * (x - l2);
            }
        }
    }
    return out;
}

} // namespace stage
} // namespace svms
