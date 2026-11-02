#include "render/StageTracerVulkan.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
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

struct Result {
    float t60[kBands];
    double total[kBands];
    double ms;
};

Result Measure(TraceBackend& backend, const TraceScene& scene, float V, int rays, uint32_t seed = 4242) {
    TraceOptions opt;
    opt.seed = seed;
    opt.rays = rays;
    opt.radius = (std::max)(0.4f, (std::min)(3.0f, 0.12f * std::cbrt(V)));
    opt.skipOrder = 3;
    const std::vector<Vec3> sources = {{6.0f, 8.0f, 1.5f}, {18.0f, 10.0f, 1.5f}};
    const Vec3 listener{12.0f, 24.0f, 1.2f};
    const auto t0 = std::chrono::steady_clock::now();
    const EnergyHistogram h = backend.Run(scene, sources, listener, opt, nullptr);
    Result r{};
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (int b = 0; b < kBands; ++b) {
        const std::vector<double> e = EnergyCurve(h, b, true);
        r.t60[b] = DecayTime(e, h.dt);
        double s = 0.0;
        for (double x : e) s += x;
        r.total[b] = s;
    }
    return r;
}

// The GPU histogram agrees with the CPU tracer: decay times and total energy
// per band within a few percent (different random streams and float atomics,
// so not bit-equal).
void TestAgainstCpu() {
    std::string error, device;
    auto gpu = CreateVulkanTraceBackend(error, &device);
    if (!gpu) {
        std::printf("SKIP: no GPU tracer (%s)\n", error.c_str());
        return;
    }
    std::printf("GPU tracer on %s\n", device.c_str());
    Room hall;
    hall.furniture.push_back({FurnitureKind::Piano, 12.0f, 14.0f, false});
    hall.furniture.push_back({FurnitureKind::Pillar, 6.0f, 20.0f, false});
    const TraceScene scene = BuildBoxScene(hall);
    const float V = RoomVolume(hall);
    CpuTracer cpu;
    // Decay times of a flat band wander a few percent with the ray count
    // (band 2 reads 2.52 s at 60k rays and 2.40 s at 400k on the CPU), so
    // compare at a count where both have settled; the GPU makes that cheap.
    const Result c = Measure(cpu, scene, V, 400000);
    const Result g = Measure(*gpu, scene, V, 400000);
    std::printf("cpu %.0f ms, gpu %.0f ms\n", c.ms, g.ms);
    for (int b = 0; b < kBands; ++b) {
        std::printf("band %d: t60 cpu %.3f gpu %.3f, energy cpu %.4g gpu %.4g\n", b, c.t60[b], g.t60[b],
                    c.total[b], g.total[b]);
        CHECK(c.t60[b] > 0.0f && g.t60[b] > 0.0f);
        CHECK(std::fabs(g.t60[b] - c.t60[b]) < 0.06f * c.t60[b]);
        CHECK(std::fabs(g.total[b] - c.total[b]) < 0.06 * c.total[b]);
    }
    // Cancelling stops early and says so.
    std::atomic<bool> stop{true};
    TraceOptions opt;
    opt.rays = 20000;
    CHECK(gpu->Run(scene, {{6.0f, 8.0f, 1.5f}}, {12.0f, 24.0f, 1.2f}, opt, &stop).cancelled);
}

} // namespace

int main() {
    TestAgainstCpu();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("stage gpu tests passed\n");
    return 0;
}
