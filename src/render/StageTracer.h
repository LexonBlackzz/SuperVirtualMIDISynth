#pragma once

// Late-field tracing for the 3D stage: sound is followed through the real
// geometry as energy rays, and the energy that passes the listener, binned by
// arrival time, is the room's energy-time curve. The tail therefore depends on
// the room's shape, not just its volume and absorption.
//
// Method (energy ray tracing, as used in room-acoustics software): every ray
// leaves a source with equal energy; at each surface the energy drops by
// (1 - absorption) per band and the ray leaves either as a mirror bounce or,
// with the surface's scattering coefficient as probability, in a Lambert-
// distributed random direction. A detector sphere around the listener adds up
// energy times path length inside it per time bin (track-length estimator).
// Air absorption (ISO 9613-1) is applied per bin afterwards.
//
// The backend is a seam: `CpuTracer` runs here; a GPU backend implements the
// same `TraceBackend::Run` and returns the same histogram layout.

#include "render/StageSolver.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace svms {
namespace stage {

struct Triangle {
    Vec3 a, b, c;
    float absorption[kBands] = {};
    float scatter = 0.0f;
};

struct TraceScene {
    std::vector<Triangle> tris;
};

namespace detail {
inline void AddQuad(TraceScene& s, const Vec3& p, const Vec3& q, const Vec3& r, const Vec3& t,
                    Material m) {
    Triangle x;
    for (int b = 0; b < kBands; ++b) x.absorption[b] = Absorption(m)[b];
    x.scatter = Scatter(m);
    x.a = p; x.b = q; x.c = r;
    s.tris.push_back(x);
    x.a = p; x.b = r; x.c = t;
    s.tris.push_back(x);
}
inline void AddBox(TraceScene& s, const Box& bx, Material m) {
    const Vec3 v[8] = {{bx.lo.x, bx.lo.y, bx.lo.z}, {bx.hi.x, bx.lo.y, bx.lo.z},
                       {bx.hi.x, bx.hi.y, bx.lo.z}, {bx.lo.x, bx.hi.y, bx.lo.z},
                       {bx.lo.x, bx.lo.y, bx.hi.z}, {bx.hi.x, bx.lo.y, bx.hi.z},
                       {bx.hi.x, bx.hi.y, bx.hi.z}, {bx.lo.x, bx.hi.y, bx.hi.z}};
    static const int f[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                {3, 2, 6, 7}, {0, 3, 7, 4}, {1, 2, 6, 5}};
    for (const auto& q : f) AddQuad(s, v[q[0]], v[q[1]], v[q[2]], v[q[3]], m);
}
} // namespace detail

// The room's six walls and every piece of furniture as triangles.
inline TraceScene BuildBoxScene(const Room& r) {
    TraceScene s;
    const float w = r.width, d = r.depth, h = r.height;
    using detail::AddQuad;
    AddQuad(s, {0, 0, 0}, {w, 0, 0}, {w, d, 0}, {0, d, 0}, r.surface[0]);
    AddQuad(s, {0, 0, h}, {w, 0, h}, {w, d, h}, {0, d, h}, r.surface[1]);
    AddQuad(s, {0, 0, 0}, {0, d, 0}, {0, d, h}, {0, 0, h}, r.surface[2]);
    AddQuad(s, {w, 0, 0}, {w, d, 0}, {w, d, h}, {w, 0, h}, r.surface[3]);
    AddQuad(s, {0, 0, 0}, {w, 0, 0}, {w, 0, h}, {0, 0, h}, r.surface[4]);
    AddQuad(s, {0, d, 0}, {w, d, 0}, {w, d, h}, {0, d, h}, r.surface[5]);
    for (const Furniture& f : r.furniture) {
        Box b = FurnitureBox(r, f);
        b.hi.z = (std::min)(b.hi.z, h - 0.01f);
        detail::AddBox(s, b, Info(f.kind).material);
    }
    return s;
}

// Mean absorption of the scene's triangles per band, area weighted, and the
// total area. The surface part of Sabine's formula for scenes without a Room.
inline void SceneAbsorption(const TraceScene& s, float absorptionArea[kBands], float* area) {
    double a[kBands] = {}, total = 0.0;
    for (const Triangle& t : s.tris) {
        const double ux = t.b.x - t.a.x, uy = t.b.y - t.a.y, uz = t.b.z - t.a.z;
        const double vx = t.c.x - t.a.x, vy = t.c.y - t.a.y, vz = t.c.z - t.a.z;
        const double ar = 0.5 * std::sqrt((uy * vz - uz * vy) * (uy * vz - uz * vy) +
                                          (uz * vx - ux * vz) * (uz * vx - ux * vz) +
                                          (ux * vy - uy * vx) * (ux * vy - uy * vx));
        total += ar;
        for (int b = 0; b < kBands; ++b) a[b] += ar * t.absorption[b];
    }
    for (int b = 0; b < kBands; ++b) absorptionArea[b] = static_cast<float>(a[b]);
    if (area) *area = static_cast<float>(total);
}

namespace detail {
struct PreTri {
    double p[3], e1[3], e2[3], n[3];
    double keep[kBands];     // 1 - absorption
    double scatter;
};

inline std::vector<PreTri> Prepare(const TraceScene& s) {
    std::vector<PreTri> out(s.tris.size());
    for (size_t i = 0; i < s.tris.size(); ++i) {
        const Triangle& t = s.tris[i];
        PreTri& o = out[i];
        o.p[0] = t.a.x; o.p[1] = t.a.y; o.p[2] = t.a.z;
        o.e1[0] = t.b.x - t.a.x; o.e1[1] = t.b.y - t.a.y; o.e1[2] = t.b.z - t.a.z;
        o.e2[0] = t.c.x - t.a.x; o.e2[1] = t.c.y - t.a.y; o.e2[2] = t.c.z - t.a.z;
        double nx = o.e1[1] * o.e2[2] - o.e1[2] * o.e2[1];
        double ny = o.e1[2] * o.e2[0] - o.e1[0] * o.e2[2];
        double nz = o.e1[0] * o.e2[1] - o.e1[1] * o.e2[0];
        double l = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (l <= 0.0) l = 1.0;
        o.n[0] = nx / l; o.n[1] = ny / l; o.n[2] = nz / l;
        for (int b = 0; b < kBands; ++b) o.keep[b] = 1.0 - t.absorption[b];
        o.scatter = t.scatter;
    }
    return out;
}

// Nearest hit along a ray (Moeller-Trumbore); -1 when nothing is hit.
inline int Hit(const std::vector<PreTri>& tris, const double* o, const double* d, int skip,
               double* tOut) noexcept {
    double best = 1e30;
    int bi = -1;
    for (size_t i = 0; i < tris.size(); ++i) {
        if (static_cast<int>(i) == skip) continue;
        const PreTri& t = tris[i];
        const double px = d[1] * t.e2[2] - d[2] * t.e2[1];
        const double py = d[2] * t.e2[0] - d[0] * t.e2[2];
        const double pz = d[0] * t.e2[1] - d[1] * t.e2[0];
        const double det = t.e1[0] * px + t.e1[1] * py + t.e1[2] * pz;
        if (det > -1e-12 && det < 1e-12) continue;
        const double inv = 1.0 / det;
        const double tx = o[0] - t.p[0], ty = o[1] - t.p[1], tz = o[2] - t.p[2];
        const double u = (tx * px + ty * py + tz * pz) * inv;
        if (u < 0.0 || u > 1.0) continue;
        const double qx = ty * t.e1[2] - tz * t.e1[1];
        const double qy = tz * t.e1[0] - tx * t.e1[2];
        const double qz = tx * t.e1[1] - ty * t.e1[0];
        const double w = (d[0] * qx + d[1] * qy + d[2] * qz) * inv;
        if (w < 0.0 || u + w > 1.0) continue;
        const double tt = (t.e2[0] * qx + t.e2[1] * qy + t.e2[2] * qz) * inv;
        if (tt > 1e-6 && tt < best) { best = tt; bi = static_cast<int>(i); }
    }
    *tOut = best;
    return bi;
}
} // namespace detail

// Volume enclosed by a closed scene, from the parity of surface crossings
// along vertical rays on a regular grid. Independent of triangle winding.
// Meant for a closed room shell (an imported mesh); furniture that touches the
// floor makes coincident faces, so a room's volume comes from RoomVolume.
inline float SceneVolume(const TraceScene& s, int grid = 96) {
    if (s.tris.empty()) return 0.0f;
    double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
    for (const Triangle& t : s.tris)
        for (const Vec3* v : {&t.a, &t.b, &t.c}) {
            lo[0] = (std::min)(lo[0], static_cast<double>(v->x));
            hi[0] = (std::max)(hi[0], static_cast<double>(v->x));
            lo[1] = (std::min)(lo[1], static_cast<double>(v->y));
            hi[1] = (std::max)(hi[1], static_cast<double>(v->y));
        }
    const std::vector<detail::PreTri> tris = detail::Prepare(s);
    const double cell = ((hi[0] - lo[0]) / grid) * ((hi[1] - lo[1]) / grid);
    double volume = 0.0;
    std::vector<double> ts;
    for (int i = 0; i < grid; ++i)
        for (int j = 0; j < grid; ++j) {
            // A small irrational offset keeps rays off shared edges.
            const double x = lo[0] + (hi[0] - lo[0]) * (i + 0.5003) / grid;
            const double y = lo[1] + (hi[1] - lo[1]) * (j + 0.4997) / grid;
            const double dir[3] = {0.0, 0.0, 1.0};
            double o[3] = {x, y, -1e4};
            ts.clear();
            int last = -1;
            for (int guard = 0; guard < 256; ++guard) {
                double t;
                const int h = detail::Hit(tris, o, dir, last, &t);
                if (h < 0) break;
                ts.push_back(o[2] + t);
                o[2] += t;
                last = h;
            }
            for (size_t k = 0; k + 1 < ts.size(); k += 2) volume += (ts[k + 1] - ts[k]) * cell;
        }
    return static_cast<float>(volume);
}

// ── Histogram and backend ────────────────────────────────────────────────
struct TraceOptions {
    int rays = 40000;
    float radius = 0.6f;       // detector sphere around the listener
    float tmax = 12.0f;
    float dt = 0.002f;         // bin width in seconds
    int skipOrder = 0;         // paths that stay specular up to this many bounces are left out
    bool includeDirect = false;
    int maxBounces = 600;
    uint32_t seed = 4242;
};

// Raw energy sums per band and bin. Bit-identical for the same scene, sources,
// listener and options on one backend; another backend agrees within tolerance.
struct EnergyHistogram {
    float dt = 0.002f;
    int bins = 0;
    int rays = 0;
    float radius = 0.6f;
    std::vector<double> sum[kBands];
    bool cancelled = false;
};

struct TraceBackend {
    virtual ~TraceBackend() = default;
    virtual EnergyHistogram Run(const TraceScene& scene, const std::vector<Vec3>& sources,
                                const Vec3& listener, const TraceOptions& opt,
                                const std::atomic<bool>* cancel) = 0;
};

constexpr double kTwoPi = 6.283185307179586;

struct CpuTracer final : TraceBackend {
    EnergyHistogram Run(const TraceScene& scene, const std::vector<Vec3>& sources,
                        const Vec3& listener, const TraceOptions& opt,
                        const std::atomic<bool>* cancel) override {
        EnergyHistogram h;
        h.dt = opt.dt;
        h.bins = static_cast<int>(std::ceil(opt.tmax / opt.dt));
        h.radius = opt.radius;
        for (auto& v : h.sum) v.assign(static_cast<size_t>(h.bins), 0.0);
        if (sources.empty() || scene.tris.empty()) return h;
        const std::vector<detail::PreTri> tris = detail::Prepare(scene);
        const int per = (std::max)(1, (opt.rays + static_cast<int>(sources.size()) - 1) /
                                          static_cast<int>(sources.size()));
        h.rays = per * static_cast<int>(sources.size());
        detail::Rng rng(opt.seed);
        const double R2 = static_cast<double>(opt.radius) * opt.radius, c = kSpeedOfSound;
        for (int i = 0; i < h.rays; ++i) {
            if (cancel && (i & 255) == 0 && cancel->load(std::memory_order_relaxed)) {
                h.cancelled = true;
                break;
            }
            const Vec3& s = sources[static_cast<size_t>(i / per)];
            const double z = 1.0 - 2.0 * rng.Next(), rr = std::sqrt((std::max)(0.0, 1.0 - z * z)),
                         ph = kTwoPi * rng.Next();
            double d[3] = {rr * std::cos(ph), rr * std::sin(ph), z};
            double o[3] = {s.x, s.y, s.z};
            double e[kBands] = {1, 1, 1, 1}, dist = 0.0;
            int nb = 0, last = -1;
            bool scat = false;
            while (nb < opt.maxBounces) {
                double t;
                const int hit = detail::Hit(tris, o, d, last, &t);
                const double tseg = hit >= 0 ? t : 50.0;
                if ((nb > 0 || opt.includeDirect) &&
                    (scat || nb > opt.skipOrder || (nb == 0 && opt.includeDirect))) {
                    const double cx = o[0] - listener.x, cy = o[1] - listener.y,
                                 cz = o[2] - listener.z;
                    const double b = cx * d[0] + cy * d[1] + cz * d[2];
                    const double disc = b * b - (cx * cx + cy * cy + cz * cz - R2);
                    if (disc > 0.0) {
                        const double sq = std::sqrt(disc);
                        const double t0 = (std::max)(0.0, -b - sq), t1 = (std::min)(tseg, -b + sq);
                        if (t1 > t0) {
                            const int bin = static_cast<int>(((dist + (t0 + t1) * 0.5) / c) / opt.dt);
                            if (bin < h.bins) {
                                const double ch = t1 - t0;
                                for (int k = 0; k < kBands; ++k) h.sum[k][static_cast<size_t>(bin)] += e[k] * ch;
                            }
                        }
                    }
                }
                if (hit < 0) break;
                dist += t;
                if (dist / c > opt.tmax) break;
                const detail::PreTri& tr = tris[static_cast<size_t>(hit)];
                o[0] += d[0] * t; o[1] += d[1] * t; o[2] += d[2] * t;
                double alive = 0.0;
                for (int k = 0; k < kBands; ++k) { e[k] *= tr.keep[k]; alive += e[k]; }
                if (alive < 1e-9) break;
                double nx = tr.n[0], ny = tr.n[1], nz = tr.n[2];
                if (nx * d[0] + ny * d[1] + nz * d[2] > 0.0) { nx = -nx; ny = -ny; nz = -nz; }
                if (rng.Next() < tr.scatter) {
                    scat = true;
                    double ux, uy, uz;
                    if (std::fabs(nx) < 0.9) { ux = 0.0; uy = nz; uz = -ny; }
                    else { ux = -nz; uy = 0.0; uz = nx; }
                    const double ul = std::sqrt(ux * ux + uy * uy + uz * uz);
                    ux /= ul; uy /= ul; uz /= ul;
                    const double vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;
                    const double r1 = kTwoPi * rng.Next(), r2 = rng.Next();
                    const double sr = std::sqrt(r2), cs = std::sqrt(1.0 - r2);
                    d[0] = (ux * std::cos(r1) + vx * std::sin(r1)) * sr + nx * cs;
                    d[1] = (uy * std::cos(r1) + vy * std::sin(r1)) * sr + ny * cs;
                    d[2] = (uz * std::cos(r1) + vz * std::sin(r1)) * sr + nz * cs;
                } else {
                    const double dn = d[0] * nx + d[1] * ny + d[2] * nz;
                    d[0] -= 2.0 * dn * nx; d[1] -= 2.0 * dn * ny; d[2] -= 2.0 * dn * nz;
                }
                last = hit;
                ++nb;
            }
        }
        return h;
    }
};

// ── Energy-time curve, decay times, tail envelope ────────────────────────
// Energy per unit detector volume, per band and bin, with air absorption.
inline std::vector<double> EnergyCurve(const EnergyHistogram& h, int band, bool air) {
    std::vector<double> e(static_cast<size_t>(h.bins));
    const double vol = 4.0 / 3.0 * 3.14159265358979 * h.radius * h.radius * h.radius;
    const double scale = h.rays > 0 ? 1.0 / (vol * h.rays) : 0.0;
    const double m = air ? AirEnergyPerMetre()[band] : 0.0;
    for (int i = 0; i < h.bins; ++i)
        e[static_cast<size_t>(i)] = h.sum[band][static_cast<size_t>(i)] * scale *
                                    std::exp(-m * kSpeedOfSound * (i + 0.5) * h.dt);
    return e;
}

// Schroeder backward integration; the time the decay takes to fall from `lo`
// to `hi` dB, extrapolated to 60 dB. Zero when the curve never gets there.
inline float DecayTime(const std::vector<double>& e, float dt, float lo = -5.0f, float hi = -35.0f) {
    const size_t n = e.size();
    std::vector<double> s(n);
    double acc = 0.0;
    for (size_t i = n; i-- > 0;) { acc += e[i]; s[i] = acc; }
    if (n == 0 || s[0] <= 0.0) return 0.0f;
    long t0 = -1, t1 = -1;
    for (size_t j = 0; j < n; ++j) {
        const double db = 10.0 * std::log10(s[j] / s[0]);
        if (t0 < 0 && db <= lo) t0 = static_cast<long>(j);
        if (t1 < 0 && db <= hi) { t1 = static_cast<long>(j); break; }
    }
    if (t0 < 0 || t1 <= t0) return 0.0f;
    return static_cast<float>((t1 - t0) * dt * 60.0f / (lo - hi));
}

struct BandEnvelope {
    std::vector<float> amplitude;   // sqrt of the smoothed energy, per bin
    float t60 = 0.0f;
};

// Moving average whose window grows with time, then an exponential extension
// from the fitted slope once the curve is only noise.
inline BandEnvelope MakeEnvelope(const std::vector<double>& e, float dt) {
    const int n = static_cast<int>(e.size());
    BandEnvelope out;
    out.amplitude.assign(e.size(), 0.0f);
    if (n == 0) return out;
    std::vector<double> pre(static_cast<size_t>(n) + 1, 0.0), es(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) pre[static_cast<size_t>(i) + 1] = pre[static_cast<size_t>(i)] + e[static_cast<size_t>(i)];
    double pk = 0.0;
    int ip = 0;
    for (int j = 0; j < n; ++j) {
        const int w = (std::max)(3, static_cast<int>(std::lround(0.08 * j)));
        const int a = (std::max)(0, j - w), b = (std::min)(n - 1, j + w);
        es[static_cast<size_t>(j)] = (pre[static_cast<size_t>(b) + 1] - pre[static_cast<size_t>(a)]) / (b - a + 1);
        if (es[static_cast<size_t>(j)] > pk) { pk = es[static_cast<size_t>(j)]; ip = j; }
    }
    if (pk <= 0.0) return out;
    int ia = -1, ib = -1;
    for (int k = ip; k < n; ++k) {
        if (ia < 0 && es[static_cast<size_t>(k)] < pk * 0.03) ia = k;
        if (ib < 0 && es[static_cast<size_t>(k)] < pk * 3e-4) { ib = k; break; }
    }
    if (ia > 0 && ib > ia) {
        const double kk = std::log(es[static_cast<size_t>(ia)] / es[static_cast<size_t>(ib)]) / (ib - ia);
        if (kk > 0.0) {
            out.t60 = static_cast<float>(13.8155 / kk * dt);
            for (int m = ib; m < n; ++m)
                es[static_cast<size_t>(m)] = es[static_cast<size_t>(ib)] * std::exp(-kk * (m - ib));
        }
    }
    for (int q = 0; q < n; ++q) out.amplitude[static_cast<size_t>(q)] = static_cast<float>(std::sqrt(es[static_cast<size_t>(q)]));
    return out;
}

// Where the band decay below the Schroeder frequency comes from. Rays describe
// sound only above that frequency; below it a room rings in separate modes.
// The default is the statistical (Sabine) decay; a wave solver can replace it.
struct LowBandSource {
    virtual ~LowBandSource() = default;
    // `absorption` is the room's total absorption (square metres) in the band.
    virtual float T60(int band, float volume, float absorption, float midT60) const noexcept = 0;
};

struct SabineLowBand final : LowBandSource {
    float T60(int, float volume, float absorption, float midT60) const noexcept override {
        const float t = absorption <= 0.001f ? 9.0f : (std::min)(9.0f, 0.161f * volume / absorption);
        // Real walls leak and flex at low frequencies, which the absorption
        // table does not list, so the band stays within 1.5 times the mid band.
        return (std::min)(t, 1.5f * (std::max)(midT60, 0.05f));
    }
};

struct TailResult {
    float dt = 0.002f;
    int bins = 0;
    std::vector<float> envelope[kBands];
    float t60[kBands] = {};
    float schroederHz = 0.0f;
    bool waveLimited[kBands] = {};
};

// `absorption` is the room's total absorption per band (surfaces plus air).
inline TailResult BuildTail(const EnergyHistogram& h, bool air, float volume,
                            const float absorption[kBands], const LowBandSource& low) {
    TailResult r;
    r.dt = h.dt;
    r.bins = h.bins;
    for (int b = 0; b < kBands; ++b) {
        BandEnvelope be = MakeEnvelope(EnergyCurve(h, b, air), h.dt);
        r.envelope[b] = std::move(be.amplitude);
        r.t60[b] = be.t60;
    }
    const float v = (std::max)(30.0f, volume);
    r.schroederHz = SchroederHz((std::max)(0.1f, r.t60[1]), v);
    for (int b = 0; b < kBands; ++b) {
        r.waveLimited[b] = kBandHz[b] < r.schroederHz;
        if (!r.waveLimited[b]) continue;
        const float ts = low.T60(b, v, absorption[b], r.t60[1]);
        std::vector<float>& e = r.envelope[b];
        size_t ip = 0;
        for (size_t i = 1; i < e.size(); ++i) if (e[i] > e[ip]) ip = i;
        const float peak = e.empty() ? 0.0f : e[ip];
        for (size_t i = ip; i < e.size(); ++i)
            e[i] = peak * std::pow(10.0f, -3.0f * static_cast<float>(i - ip) * h.dt / ts);
        r.t60[b] = ts;
    }
    return r;
}

} // namespace stage
} // namespace svms
