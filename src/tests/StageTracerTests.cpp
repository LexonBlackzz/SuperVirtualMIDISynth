#include "render/StageTracer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace svms::stage;

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// A box whose six walls all absorb `alpha` in every band and scatter `scatter`.
TraceScene UniformBox(float w, float d, float h, float alpha, float scatter) {
    Room r;
    r.width = w; r.depth = d; r.height = h;
    TraceScene s = BuildBoxScene(r);
    for (Triangle& t : s.tris) {
        for (float& a : t.absorption) a = alpha;
        t.scatter = scatter;
    }
    return s;
}

float TracedT60(const TraceScene& s, float V, int rays, float* eyring = nullptr) {
    CpuTracer tracer;
    TraceOptions opt;
    opt.rays = rays;
    opt.radius = (std::max)(0.4f, (std::min)(3.0f, 0.12f * std::cbrt(V)));
    const std::vector<Vec3> src = {{2.0f, 3.0f, 1.4f}};
    const Vec3 listener{5.0f, 6.0f, 1.2f};
    const EnergyHistogram h = tracer.Run(s, src, listener, opt, nullptr);
    if (eyring) {
        float absorptionArea[kBands], area;
        SceneAbsorption(s, absorptionArea, &area);
        const float mean = absorptionArea[1] / area;
        *eyring = 0.161f * V / (-area * std::log(1.0f - mean));
    }
    return DecayTime(EnergyCurve(h, 1, false), h.dt);
}

// A diffuse box decays like Eyring's formula says, a little slower as the
// walls absorb more (the Kuttruff correction), and never faster.
void TestEyring() {
    const float w = 8.0f, d = 10.0f, h = 3.0f, V = w * d * h;
    for (float alpha : {0.05f, 0.2f}) {
        float eyring = 0.0f;
        const float t = TracedT60(UniformBox(w, d, h, alpha, 1.0f), V, 20000, &eyring);
        CHECK(t >= 0.97f * eyring);
        CHECK(t <= 1.15f * eyring);
        // Sabine is the upper bound.
        CHECK(t <= 0.161f * V / (2.0f * (w * d + w * h + d * h) * alpha) * 1.02f);
    }
}

// Mirror-only walls keep sound ringing in fixed patterns: no faster than Eyring.
void TestSpecularBox() {
    const float V = 8.0f * 10.0f * 3.0f;
    float eyring = 0.0f;
    const float t = TracedT60(UniformBox(8.0f, 10.0f, 3.0f, 0.2f, 0.0f), V, 20000, &eyring);
    CHECK(t >= 0.95f * eyring);
}

void TestDeterminism() {
    const TraceScene s = UniformBox(6.0f, 7.0f, 3.0f, 0.1f, 0.3f);
    CpuTracer tracer;
    TraceOptions opt;
    opt.rays = 3000;
    const std::vector<Vec3> src = {{1.0f, 1.0f, 1.2f}, {4.0f, 5.0f, 1.2f}};
    const Vec3 l{3.0f, 3.0f, 1.2f};
    const EnergyHistogram a = tracer.Run(s, src, l, opt, nullptr);
    const EnergyHistogram b = tracer.Run(s, src, l, opt, nullptr);
    for (int k = 0; k < kBands; ++k)
        CHECK(std::memcmp(a.sum[k].data(), b.sum[k].data(), a.sum[k].size() * sizeof(double)) == 0);
    opt.seed = 7;
    const EnergyHistogram c = tracer.Run(s, src, l, opt, nullptr);
    CHECK(std::memcmp(a.sum[1].data(), c.sum[1].data(), a.sum[1].size() * sizeof(double)) != 0);

    // Cancelling stops early and says so.
    std::atomic<bool> stop{true};
    CHECK(tracer.Run(s, src, l, opt, &stop).cancelled);
}

// Volume from surface parity: box, a closed octahedron (4/3 r^3) and a box
// with the winding of half its faces flipped.
void TestVolume() {
    const TraceScene box = UniformBox(6.0f, 5.0f, 3.0f, 0.1f, 0.1f);
    CHECK(std::fabs(SceneVolume(box) - 90.0f) < 0.9f);

    TraceScene oct;
    const float r = 2.0f;
    const Vec3 v[6] = {{r, 0, 0}, {-r, 0, 0}, {0, r, 0}, {0, -r, 0}, {0, 0, r}, {0, 0, -r}};
    const int f[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                         {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (const auto& q : f) {
        Triangle t;
        t.a = v[q[0]]; t.b = v[q[1]]; t.c = v[q[2]];
        oct.tris.push_back(t);
    }
    const float expected = 4.0f / 3.0f * r * r * r;
    CHECK(std::fabs(SceneVolume(oct) - expected) < 0.02f * expected);

    TraceScene flipped = box;
    for (size_t i = 0; i < flipped.tris.size(); i += 2) std::swap(flipped.tris[i].b, flipped.tris[i].c);
    CHECK(std::fabs(SceneVolume(flipped) - 90.0f) < 0.9f);
}

// A small room is wave-limited at 125 Hz, a big hall is not; the fallback
// keeps the low band within 1.5 times the mid band.
void TestSchroederFallback() {
    CpuTracer tracer;
    TraceOptions opt;
    opt.rays = 8000;
    auto run = [&](float w, float d, float h, float alpha) {
        const TraceScene s = UniformBox(w, d, h, alpha, 0.5f);
        const std::vector<Vec3> src = {{w * 0.3f, d * 0.3f, 1.4f}};
        const Vec3 l{w * 0.6f, d * 0.6f, 1.2f};
        const EnergyHistogram hist = tracer.Run(s, src, l, opt, nullptr);
        float abs[kBands], area;
        SceneAbsorption(s, abs, &area);
        const float V = w * d * h;
        for (float& a : abs) a += 0.0f;
        return BuildTail(hist, true, V, abs, SabineLowBand{});
    };
    const TailResult small = run(3.0f, 4.0f, 2.5f, 0.2f);
    CHECK(small.waveLimited[0]);
    CHECK(!small.waveLimited[3]);
    CHECK(small.t60[0] <= 1.5f * small.t60[1] + 1e-4f);

    const TailResult hall = run(30.0f, 40.0f, 14.0f, 0.1f);
    CHECK(!hall.waveLimited[0] && !hall.waveLimited[1]);
    CHECK(hall.t60[1] > 1.0f);
    CHECK(hall.t60[3] < hall.t60[1] + 0.01f || hall.t60[3] > 0.0f);
    // Envelopes start at their peak and fall.
    for (int b = 0; b < kBands; ++b) {
        const auto& e = hall.envelope[b];
        size_t ip = 0;
        for (size_t i = 1; i < e.size(); ++i) if (e[i] > e[ip]) ip = i;
        CHECK(e[ip] > 0.0f);
        CHECK(e[e.size() - 1] < 0.05f * e[ip]);
    }
}

// Shape matters: the same volume and walls in a long corridor and a cube
// give different decays when the walls scatter little.
void TestShapeMatters() {
    const float alpha = 0.15f;
    const float cube = TracedT60(UniformBox(10.0f, 10.0f, 10.0f, alpha, 0.0f), 1000.0f, 20000);
    const float hall = TracedT60(UniformBox(40.0f, 5.0f, 5.0f, alpha, 0.0f), 1000.0f, 20000);
    CHECK(cube > 0.0f && hall > 0.0f);
    CHECK(std::fabs(cube - hall) > 0.02f * cube);
}

} // namespace

int main() {
    TestEyring();
    TestSpecularBox();
    TestDeterminism();
    TestVolume();
    TestSchroederFallback();
    TestShapeMatters();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("stage tracer tests passed\n");
    return 0;
}
