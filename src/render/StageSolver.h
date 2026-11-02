#pragma once

// Room acoustics for the 3D stage. Everything in here is pure: a room, a
// source and a listener go in, reflection lists, decay times, blocking losses
// and impulse responses come out. Nothing touches the audio thread and no
// function keeps hidden state between calls, so two sources never share
// results.
//
// Coordinates: x runs left to right, y front to back, z up, in metres. The
// room is a box with its corner at the origin. Wall order everywhere is
// floor, ceiling, left (x = 0), right (x = width), front (y = 0), back
// (y = depth).
//
// The parts that are expected to grow (materials, bands, source patterns,
// blocking) sit behind small, separate entry points so a better version can
// replace one without touching the rest.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace svms {
namespace stage {

// ── Bands ─────────────────────────────────────────────────────────────────
// Octave bands the room is described in. Blocking is evaluated one octave
// lower at the bottom (63 Hz) so a subwoofer is judged at its own frequency.
constexpr int kBands = 4;
constexpr float kBandHz[kBands] = {125.0f, 500.0f, 2000.0f, 8000.0f};
constexpr float kBlockHz[kBands] = {63.0f, 500.0f, 2000.0f, 8000.0f};
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

// Absorption coefficient per band, and the scattering coefficient (the share
// of reflected energy that leaves in all directions instead of as a mirror
// bounce). Open is a perfect absorber, which is how an open field is modelled.
// Scattering values are typical orders of magnitude: smooth surfaces about
// 0.05-0.15, seating, shelves and plants 0.5 and up.
struct MaterialData {
    float absorption[kBands];
    float scatter;
};

inline const MaterialData& Data(Material m) noexcept {
    static const MaterialData table[static_cast<int>(Material::Count)] = {
        {{0.02f, 0.03f, 0.04f, 0.05f}, 0.10f},   // Plaster
        {{0.02f, 0.03f, 0.04f, 0.06f}, 0.10f},   // Stone
        {{0.01f, 0.02f, 0.03f, 0.04f}, 0.10f},   // Concrete
        {{0.15f, 0.10f, 0.06f, 0.07f}, 0.10f},   // Wood
        {{0.18f, 0.06f, 0.03f, 0.02f}, 0.05f},   // Glass
        {{0.08f, 0.24f, 0.57f, 0.69f}, 0.10f},   // Carpet
        {{0.25f, 0.45f, 0.60f, 0.60f}, 0.60f},   // Seating
        {{0.10f, 0.45f, 0.75f, 0.72f}, 0.30f},   // Curtain
        {{0.20f, 0.70f, 0.95f, 0.95f}, 0.20f},   // Foam
        {{0.15f, 0.25f, 0.35f, 0.35f}, 0.60f},   // Books
        {{0.10f, 0.20f, 0.30f, 0.35f}, 0.80f},   // Foliage
        {{1.00f, 1.00f, 1.00f, 1.00f}, 0.00f},   // Open
    };
    return table[static_cast<int>(m)];
}

inline const float* Absorption(Material m) noexcept { return Data(m).absorption; }
inline float Scatter(Material m) noexcept { return Data(m).scatter; }

// ── Air ───────────────────────────────────────────────────────────────────
// Pure-tone atmospheric absorption in dB per metre (ISO 9613-1).
inline double AirAlphaDbPerM(double f, double tempC = 20.0, double humidityPct = 50.0,
                             double pressureKpa = 101.325) noexcept {
    const double T = tempC + 273.15, T0 = 293.15, T01 = 273.16, pr = 101.325;
    const double pp = pressureKpa / pr;
    const double c = -6.8346 * std::pow(T01 / T, 1.261) + 4.6151;
    const double h = humidityPct * std::pow(10.0, c) / pp;
    const double frO = pp * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
    const double frN = pp * std::pow(T / T0, -0.5) *
                       (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(T / T0, -1.0 / 3.0) - 1.0)));
    const double f2 = f * f;
    return 8.686 * f2 * (1.84e-11 / pp * std::sqrt(T / T0) +
                         std::pow(T / T0, -2.5) *
                             (0.01275 * std::exp(-2239.1 / T) / (frO + f2 / frO) +
                              0.1068 * std::exp(-3352.0 / T) / (frN + f2 / frN)));
}

// dB per metre in each band at 20 C, 50 % humidity.
inline const float* AirDbPerMetre() noexcept {
    static const std::array<float, kBands> a = [] {
        std::array<float, kBands> r{};
        for (int b = 0; b < kBands; ++b) r[b] = static_cast<float>(AirAlphaDbPerM(kBandHz[b]));
        return r;
    }();
    return a.data();
}

// The air's share of a room's total absorption is 4 m V, with m the energy
// attenuation per metre (dB per metre divided by 4.343).
inline const float* AirEnergyPerMetre() noexcept {
    static const std::array<float, kBands> a = [] {
        std::array<float, kBands> r{};
        for (int b = 0; b < kBands; ++b) r[b] = AirDbPerMetre()[b] / 4.343f;
        return r;
    }();
    return a.data();
}

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
    const float h = info.fullHeight ? room.height : (std::min)(info.height, room.height);
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

// Absorption of the surfaces and furniture only (square metres of open
// window per band), without the air.
inline void SurfaceAbsorption(const Room& r, float out[kBands]) noexcept {
    float area[kWalls];
    WallAreas(r, area);
    for (int b = 0; b < kBands; ++b) {
        float a = 0.0f;
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

// Total absorption per band: surfaces, furniture and the air itself.
inline void TotalAbsorption(const Room& r, float out[kBands]) noexcept {
    SurfaceAbsorption(r, out);
    const float fullVolume = r.width * r.depth * r.height;
    for (int b = 0; b < kBands; ++b) out[b] += 4.0f * AirEnergyPerMetre()[b] * fullVolume;
}

// Decay time (60 dB) per band from Sabine's formula. The lowest band is kept
// within 1.5 times the 500 Hz value and nothing rings longer than 9 s: real
// walls also leak and flex at low frequencies, which the absorption table does
// not list. This is the statistical fallback; the traced decay replaces it
// wherever the ray method is valid.
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

// Eyring's formula, better than Sabine's when the walls absorb a lot:
// T = 0.161 V / (-S ln(1 - mean absorption) + 4 m V).
inline void EyringTimes(const Room& r, float t60[kBands]) noexcept {
    float surf[kBands], area[kWalls];
    SurfaceAbsorption(r, surf);
    WallAreas(r, area);
    float total = 0.0f;
    for (float a : area) total += a;
    const float volume = RoomVolume(r);
    for (int b = 0; b < kBands; ++b) {
        const float mean = (std::min)(0.999f, surf[b] / total);
        const float denom = -total * std::log(1.0f - mean) + 4.0f * AirEnergyPerMetre()[b] * volume;
        t60[b] = denom <= 1e-6f ? 0.0f : (std::min)(9.0f, 0.161f * volume / denom);
    }
}

// Above this frequency a room's response is dense enough for rays to describe;
// below it the room rings in separate modes.
inline float SchroederHz(float t60, float volume) noexcept {
    return 2000.0f * std::sqrt((std::max)(0.01f, t60) / (std::max)(1.0f, volume));
}

// Distance at which the direct sound and the room are equally loud.
inline float CriticalDistance(const Room& r, float meanT60) noexcept {
    const float t = (std::max)(0.15f, meanT60);
    return (std::max)(0.4f, 0.057f * std::sqrt(RoomVolume(r) / t));
}

// ── Sources ───────────────────────────────────────────────────────────────
// How much a source radiates toward a direction, per band. `dir` is a unit
// vector from the source into the room. The default is omnidirectional; a
// speaker or instrument pattern replaces it without touching the solver.
struct SourceModel {
    virtual ~SourceModel() = default;
    virtual float Gain(const Vec3& dir, int band) const noexcept = 0;
};

struct OmniSource final : SourceModel {
    float Gain(const Vec3&, int) const noexcept override { return 1.0f; }
};

inline float EmissionGain(const SourceModel* model, const Vec3& from, const Vec3& toward,
                          int band) noexcept {
    if (!model) return 1.0f;
    const float d = Distance(from, toward);
    if (d < 1e-6f) return 1.0f;
    return model->Gain(Vec3{(toward.x - from.x) / d, (toward.y - from.y) / d,
                            (toward.z - from.z) / d},
                       band);
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
            const float inv = 1.0f / dv[k];
            float ta = (lo[k] - o[k]) * inv, tb = (hi[k] - o[k]) * inv;
            if (ta > tb) std::swap(ta, tb);
            t0 = (std::max)(t0, ta);
            t1 = (std::min)(t1, tb);
            if (t0 > t1) return false;
        }
    }
    return true;
}

// True when a blocking piece of furniture (other than `skip`) stands on the
// segment.
inline bool Occluded(const Room& r, const Vec3& a, const Vec3& b, int skip = -1) noexcept {
    for (size_t i = 0; i < r.furniture.size(); ++i) {
        if (static_cast<int>(i) == skip) continue;
        const Furniture& f = r.furniture[i];
        if (!Info(f.kind).blocks) continue;
        Box box = FurnitureBox(r, f);
        const float e = 0.002f;   // a path that ends on the surface is not blocked by it
        box.lo = Vec3{box.lo.x + e, box.lo.y + e, box.lo.z + e};
        box.hi = Vec3{box.hi.x - e, box.hi.y - e, box.hi.z - e};
        if (SegmentHitsBox(a, b, box)) return true;
    }
    return false;
}

// Loss in dB per band when furniture blocks the path a-b. The sound gets
// round by diffraction, so low frequencies pass an object almost untouched
// while highs are shadowed. For each blocker: Maekawa's insertion loss
// 10 log10(3 + 20 N), N = 2 delta f / c, delta being how much longer the
// shortest way over or around the object is than the straight line, capped at
// 24 dB. Maekawa assumes a screen much larger than the wavelength; below that
// the object scatters weakly (Rayleigh regime), so the loss is faded in with
// x / (1 + x), x = (2 pi f D / c)^2, D the object's size. Several blockers add,
// capped at 30 dB.
inline void OcclusionLoss(const Room& r, const Vec3& a, const Vec3& b, float out[kBands],
                          int skip = -1) noexcept {
    for (int k = 0; k < kBands; ++k) out[k] = 0.0f;
    const float d0 = Distance(a, b);
    for (size_t i = 0; i < r.furniture.size(); ++i) {
        if (static_cast<int>(i) == skip) continue;
        const Furniture& f = r.furniture[i];
        if (!Info(f.kind).blocks) continue;
        const Box box = FurnitureBox(r, f);
        Box inner = box;
        const float e = 0.002f;
        inner.lo = Vec3{box.lo.x + e, box.lo.y + e, box.lo.z + e};
        inner.hi = Vec3{box.hi.x - e, box.hi.y - e, box.hi.z - e};
        if (!SegmentHitsBox(a, b, inner)) continue;

        const float cx[4] = {box.lo.x, box.hi.x, box.hi.x, box.lo.x};
        const float cy[4] = {box.lo.y, box.lo.y, box.hi.y, box.hi.y};
        const float zTop = box.hi.z;
        float best = 1e9f;
        for (int c = 0; c < 4; ++c) {
            const int n = (c + 1) % 4;
            for (int s = 0; s <= 12; ++s) {   // along each top edge
                const float u = static_cast<float>(s) / 12.0f;
                const Vec3 p{cx[c] + (cx[n] - cx[c]) * u, cy[c] + (cy[n] - cy[c]) * u, zTop};
                best = (std::min)(best, Distance(a, p) + Distance(p, b) - d0);
            }
            for (int s = 0; s <= 12; ++s) {   // up each vertical edge
                const Vec3 p{cx[c], cy[c], zTop * static_cast<float>(s) / 12.0f};
                best = (std::min)(best, Distance(a, p) + Distance(p, b) - d0);
            }
        }
        const float size = std::cbrt((box.hi.x - box.lo.x) * (box.hi.y - box.lo.y) * zTop);
        for (int k = 0; k < kBands; ++k) {
            const float N = 2.0f * (std::max)(0.0f, best) * kBlockHz[k] / kSpeedOfSound;
            const float kd = 6.2831853f * kBlockHz[k] * size / kSpeedOfSound;
            const float x = kd * kd;
            out[k] += (std::min)(24.0f, 10.0f * std::log10(3.0f + 20.0f * N)) * x / (1.0f + x);
        }
    }
    for (int k = 0; k < kBands; ++k) out[k] = (std::min)(30.0f, out[k]);
}

inline float DbToAmplitude(float db) noexcept { return std::pow(10.0f, -db / 20.0f); }

// ── Early paths ───────────────────────────────────────────────────────────
struct EarlyPath {
    Vec3 image;                 // where the sound seems to come from
    float path = 0.0f;          // total path length in metres
    float gain[kBands] = {};    // surface loss, blocking and source pattern per band; no distance or air
    uint8_t order = 1;
    uint8_t seq[3] = {};        // walls hit, in order from the source (wall paths only)
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

// Where the path source -> walls seq[0..n-1] -> listener touches each wall:
// mirror the source across the walls in turn, then walk back from the
// listener toward each image. pts[0] is the first bounce.
inline void BouncePoints(const Room& r, const Vec3& source, const Vec3& listener,
                         const uint8_t* seq, int n, Vec3* pts) noexcept {
    Vec3 images[4];
    images[0] = source;
    for (int k = 0; k < n; ++k) images[k + 1] = MirrorAcrossWall(r, images[k], seq[k]);
    Vec3 cur = listener;
    for (int k = n; k >= 1; --k) {
        const int w = seq[k - 1];
        const Vec3& m = images[k];
        float plane, a, d;
        switch (w) {
            case 0: plane = 0.0f; a = cur.z; d = m.z - cur.z; break;
            case 1: plane = r.height; a = cur.z; d = m.z - cur.z; break;
            case 2: plane = 0.0f; a = cur.x; d = m.x - cur.x; break;
            case 3: plane = r.width; a = cur.x; d = m.x - cur.x; break;
            case 4: plane = 0.0f; a = cur.y; d = m.y - cur.y; break;
            default: plane = r.depth; a = cur.y; d = m.y - cur.y; break;
        }
        const float t = std::fabs(d) < 1e-9f ? 0.0f : (plane - a) / d;
        Vec3 p{cur.x + (m.x - cur.x) * t, cur.y + (m.y - cur.y) * t, cur.z + (m.z - cur.z) * t};
        pts[k - 1] = p;
        cur = p;
    }
}

inline bool InsideRoom(const Room& r, const Vec3& p) noexcept {
    const float e = 1e-3f;
    return p.x >= -e && p.x <= r.width + e && p.y >= -e && p.y <= r.depth + e &&
           p.z >= -e && p.z <= r.height + e;
}

// Mirror reflections off the faces of furniture boxes by the image-source
// method: mirror the source across a face plane; the reflection exists when
// the line from that image to the listener crosses the face itself. Blocking
// by other furniture weakens a leg like it weakens the direct sound.
inline void FurnitureReflections(const Room& r, const Vec3& source, const Vec3& listener,
                                 const SourceModel* model, int keep,
                                 std::vector<EarlyPath>& out) {
    std::vector<EarlyPath> hits;
    for (size_t fi = 0; fi < r.furniture.size(); ++fi) {
        const Furniture& f = r.furniture[fi];
        const FurnitureInfo& info = Info(f.kind);
        if (info.flat) continue;
        const Box box = FurnitureBox(r, f);
        const float lo[3] = {box.lo.x, box.lo.y, box.lo.z}, hi[3] = {box.hi.x, box.hi.y, box.hi.z};
        const float s3[3] = {source.x, source.y, source.z}, l3[3] = {listener.x, listener.y, listener.z};
        // Faces: -x, +x, -y, +y and the top. The bottom is on the floor.
        const int axis[5] = {0, 0, 1, 1, 2};
        const float side[5] = {-1.0f, 1.0f, -1.0f, 1.0f, 1.0f};
        for (int fc = 0; fc < 5; ++fc) {
            const int ax = axis[fc];
            const float sg = side[fc], plane = sg < 0.0f ? lo[ax] : hi[ax];
            if (sg * (s3[ax] - plane) <= 0.0f || sg * (l3[ax] - plane) <= 0.0f) continue;
            float m3[3] = {s3[0], s3[1], s3[2]};
            m3[ax] = 2.0f * plane - s3[ax];
            const float t = (plane - l3[ax]) / (m3[ax] - l3[ax]);
            if (!(t > 0.0f && t < 1.0f)) continue;
            const float p3[3] = {l3[0] + (m3[0] - l3[0]) * t, l3[1] + (m3[1] - l3[1]) * t,
                                 l3[2] + (m3[2] - l3[2]) * t};
            bool inside = true;
            for (int k = 0; k < 3; ++k)
                if (k != ax && (p3[k] < lo[k] || p3[k] > hi[k])) inside = false;
            if (!inside) continue;
            const Vec3 p{p3[0], p3[1], p3[2]}, image{m3[0], m3[1], m3[2]};
            EarlyPath e;
            e.image = image;
            e.path = Distance(image, listener);
            e.furniture = true;
            SurfaceGain(info.material, e.gain);
            float l1[kBands], l2[kBands];
            OcclusionLoss(r, source, p, l1, static_cast<int>(fi));
            OcclusionLoss(r, p, listener, l2, static_cast<int>(fi));
            for (int b = 0; b < kBands; ++b)
                e.gain[b] *= DbToAmplitude(l1[b] + l2[b]) * EmissionGain(model, source, p, b);
            hits.push_back(e);
        }
    }
    std::sort(hits.begin(), hits.end(),
              [](const EarlyPath& a, const EarlyPath& b) { return a.path < b.path; });
    if (static_cast<int>(hits.size()) > keep) hits.resize(static_cast<size_t>(keep));
    out.insert(out.end(), hits.begin(), hits.end());
}

// Mirror-image reflections off the walls up to the given order (1 to 3),
// followed by the furniture reflections. Second and third order keep only the
// shortest 18 and 38 paths. Blocking by furniture and the source pattern are
// applied to every kept path.
inline std::vector<EarlyPath> FindReflections(const Room& r, const Vec3& source,
                                              const Vec3& listener, int order,
                                              const SourceModel* model = nullptr) {
    std::vector<EarlyPath> walls;
    std::vector<EarlyPath> level;
    for (int w = 0; w < kWalls; ++w) {
        EarlyPath e;
        e.image = MirrorAcrossWall(r, source, w);
        e.path = Distance(e.image, listener);
        SurfaceGain(r.surface[w], e.gain);
        e.order = 1;
        e.seq[0] = static_cast<uint8_t>(w);
        e.wall = static_cast<int8_t>(w);
        walls.push_back(e);
        level.push_back(e);
    }
    for (int o = 2; o <= (std::min)(order, 3); ++o) {
        std::vector<EarlyPath> next;
        std::vector<EarlyPath> valid;
        for (const EarlyPath& rf : level) {
            for (int w = 0; w < kWalls; ++w) {
                if (w == rf.wall) continue;
                EarlyPath n;
                n.image = MirrorAcrossWall(r, rf.image, w);
                n.path = Distance(n.image, listener);
                float g[kBands];
                SurfaceGain(r.surface[w], g);
                for (int b = 0; b < kBands; ++b) n.gain[b] = rf.gain[b] * g[b];
                n.order = static_cast<uint8_t>(o);
                for (int k = 0; k < o - 1; ++k) n.seq[k] = rf.seq[k];
                n.seq[o - 1] = static_cast<uint8_t>(w);
                n.wall = static_cast<int8_t>(w);
                next.push_back(n);
                // Mirroring across perpendicular walls commutes, so two
                // orderings share one image but only one is a real path: in the
                // other the bounce point falls outside the room.
                Vec3 pts[3];
                BouncePoints(r, source, listener, n.seq, o, pts);
                bool inside = true;
                for (int k = 0; k < o; ++k) inside = inside && InsideRoom(r, pts[k]);
                if (inside) valid.push_back(n);
            }
        }
        std::stable_sort(valid.begin(), valid.end(),
                         [](const EarlyPath& a, const EarlyPath& b) { return a.path < b.path; });
        const size_t cap = o == 2 ? 18u : 38u;
        if (valid.size() > cap) valid.resize(cap);
        walls.insert(walls.end(), valid.begin(), valid.end());
        level = next;   // later orders grow from every image, valid or not
    }
    for (EarlyPath& e : walls) {
        Vec3 pts[3];
        BouncePoints(r, source, listener, e.seq, e.order, pts);
        float loss[kBands] = {};
        Vec3 from = source;
        for (int k = 0; k <= e.order; ++k) {
            const Vec3 to = k < e.order ? pts[k] : listener;
            float leg[kBands];
            OcclusionLoss(r, from, to, leg);
            for (int b = 0; b < kBands; ++b) loss[b] += leg[b];
            from = to;
        }
        for (int b = 0; b < kBands; ++b)
            e.gain[b] *= DbToAmplitude(loss[b]) * EmissionGain(model, source, pts[0], b);
    }
    FurnitureReflections(r, source, listener, model, 12, walls);
    return walls;
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
    float gain[kBands] = {};       // 1/d, air, blocking and source pattern per band (amplitude)
    float occlusionDb[kBands] = {};
    float lowpassHz = 20000.0f;    // behind-you rolloff of the ear model only
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
                              float yawDegrees, bool airAbsorption, bool earModel,
                              const SourceModel* model = nullptr) noexcept {
    DirectPath d;
    const Relative rel = RelativeTo(source, listener, yawDegrees);
    d.distance = (std::max)(0.5f, rel.distance);
    d.delaySeconds = (std::min)(0.5f, d.distance / kSpeedOfSound);
    OcclusionLoss(r, source, listener, d.occlusionDb);
    for (int b = 0; b < kBands; ++b) {
        if (d.occlusionDb[b] > 0.0f) d.occluded = true;
        const float airDb = airAbsorption ? AirDbPerMetre()[b] * d.distance : 0.0f;
        d.gain[b] = DbToAmplitude(airDb + d.occlusionDb[b]) / d.distance *
                    EmissionGain(model, source, listener, b);
    }
    d.lowpassHz = earModel ? BehindLowpassHz(BehindFactor(rel)) : 20000.0f;
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

// Turns a path list into a two-ear impulse response. Each reflection is a
// short burst of noise, not a click: walls and furniture scatter sound, and
// later reflections smear more. Per band the burst carries the surface loss
// and the air absorption of its path. With `earModel` on, each ear also gets
// its own arrival time and a head-shadow and behind-you rolloff on the highs.
inline EarlyResponse BuildEarlyResponse(const std::vector<EarlyPath>& reflections,
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
        const EarlyPath& rf = reflections[idx];
        const float tp = rf.path / kSpeedOfSound;
        if (tp > seconds - 0.02f) continue;
        const Relative rel = RelativeTo(rf.image, listener, yawDegrees);
        const float rr = (std::max)(0.1f, rel.distance);
        const float lat = (std::max)(-1.0f, (std::min)(1.0f, rel.right / rr));
        const float behind = BehindFactor(rel);
        const float amp = kEarlyLevel / (std::max)(1.0f, rf.path);
        float gb[kBands];
        for (int b = 0; b < kBands; ++b)
            gb[b] = rf.gain[b] * (airAbsorption ? DbToAmplitude(AirDbPerMetre()[b] * rf.path) : 1.0f);
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
