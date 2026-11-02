#include "render/StageSolver.h"

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

double Energy(const std::vector<float>& v) {
    double e = 0.0;
    for (float s : v) e += static_cast<double>(s) * s;
    return e;
}

bool Finite(const std::vector<float>& v) {
    for (float s : v) if (!std::isfinite(s)) return false;
    return true;
}

Room Hall() { return Room{}; }

Room Uniform(Material m, float w, float d, float h) {
    Room r;
    r.width = w; r.depth = d; r.height = h;
    for (Material& s : r.surface) s = m;
    return r;
}

void TestMaterials() {
    for (int m = 0; m < static_cast<int>(Material::Count); ++m) {
        const float* a = Absorption(static_cast<Material>(m));
        for (int b = 0; b < kBands; ++b) CHECK(a[b] >= 0.0f && a[b] <= 1.0f);
    }
    const float* open = Absorption(Material::Open);
    for (int b = 0; b < kBands; ++b) CHECK(open[b] == 1.0f);
    const float* carpet = Absorption(Material::Carpet);
    for (int b = 1; b < kBands; ++b) CHECK(carpet[b] > carpet[b - 1]);
    // Hard surfaces keep the highs, carpet does not.
    CHECK(Absorption(Material::Concrete)[3] < 0.1f);
    CHECK(carpet[3] > 0.5f);
}

void TestDecay() {
    float t[kBands];
    ReverbTimes(Hall(), t);
    CHECK(t[1] > 1.5f && t[1] < 3.6f);
    CHECK(t[0] <= 1.5f * t[1] + 1e-4f);
    CHECK(t[3] < t[1]);   // air and absorption eat the highs

    ReverbTimes(Uniform(Material::Foam, 5.0f, 4.0f, 2.8f), t);
    CHECK(t[1] < 0.4f);

    // A bare concrete cathedral hits the cap, never beyond.
    ReverbTimes(Uniform(Material::Concrete, 60.0f, 90.0f, 40.0f), t);
    for (int b = 0; b < kBands; ++b) CHECK(t[b] <= 9.0f + 1e-4f && t[b] > 0.0f);
    CHECK(t[0] <= 1.5f * t[1] + 1e-4f);

    // More absorbing furniture shortens the room.
    Room r = Hall(), f = Hall();
    float a[kBands], b[kBands];
    for (int i = 0; i < 6; ++i) f.furniture.push_back({FurnitureKind::Audience, 12.0f, 6.0f + 4.0f * i, false});
    ReverbTimes(r, a);
    ReverbTimes(f, b);
    CHECK(b[1] < a[1]);

    // Furniture takes volume but never more than half.
    Room crowd = Uniform(Material::Plaster, 2.0f, 2.0f, 2.0f);
    for (int i = 0; i < 20; ++i) crowd.furniture.push_back({FurnitureKind::Piano, 1.0f, 1.0f, false});
    CHECK(RoomVolume(crowd) >= 4.0f - 1e-4f);
    CHECK(RoomVolume(f) < RoomVolume(r));

    CHECK(CriticalDistance(r, 2.0f) > CriticalDistance(Uniform(Material::Plaster, 3, 3, 2.5f), 2.0f));
}

void TestReflectionCounts() {
    const Room r = Uniform(Material::Plaster, 8.0f, 10.0f, 3.0f);
    const Vec3 s{2.0f, 3.0f, 1.5f}, l{5.0f, 7.0f, 1.2f};
    CHECK(FindReflections(r, s, l, 1).size() == 6);
    CHECK(FindReflections(r, s, l, 2).size() == 6 + 18);
    CHECK(FindReflections(r, s, l, 3).size() == 6 + 18 + 38);

    Room f = r;
    f.furniture.push_back({FurnitureKind::Bookshelf, 4.5f, 8.8f, false});
    const auto withFurn = FindReflections(f, s, l, 3);
    CHECK(withFurn.size() > 62 && withFurn.size() <= 62 + 12);
}

void TestFirstOrderGeometry() {
    const Room r = Uniform(Material::Plaster, 8.0f, 10.0f, 3.0f);
    const Vec3 s{2.0f, 3.0f, 1.5f}, l{5.0f, 7.0f, 1.2f};
    const auto list = FindReflections(r, s, l, 1);
    // Floor image is the source mirrored below z = 0.
    CHECK(list[0].wall == 0 && std::fabs(list[0].image.z + 1.5f) < 1e-5f);
    CHECK(std::fabs(list[0].path - Distance(Vec3{2.0f, 3.0f, -1.5f}, l)) < 1e-5f);
    // Left wall image sits behind x = 0.
    CHECK(std::fabs(list[2].image.x + 2.0f) < 1e-5f);
    // Every reflection is later than the direct sound.
    for (const EarlyPath& rf : list) CHECK(rf.path > Distance(s, l));
    // Plaster loses little at 500 Hz, so gains stay near 1 and below it.
    for (const EarlyPath& rf : list)
        for (int b = 0; b < kBands; ++b) CHECK(rf.gain[b] > 0.9f && rf.gain[b] < 1.0f);
}

// Left and right walls differ; the mirrored room swaps them, moves the source
// to the other side and keeps the listener on the centre line.
void TestMirroredRoom() {
    Room a = Uniform(Material::Plaster, 9.0f, 12.0f, 3.2f);
    a.surface[2] = Material::Foam;
    a.surface[3] = Material::Concrete;
    Room b = a;
    std::swap(b.surface[2], b.surface[3]);
    const Vec3 sa{2.0f, 4.0f, 1.4f};
    const Vec3 sb{a.width - sa.x, sa.y, sa.z};
    const Vec3 l{a.width * 0.5f, 8.0f, 1.2f};
    const float sr = 48000.0f;
    const EarlyResponse ra = BuildEarlyResponse(FindReflections(a, sa, l, 3), l, 0.0f, true, true, sr);
    const EarlyResponse rb = BuildEarlyResponse(FindReflections(b, sb, l, 3), l, 0.0f, true, true, sr);
    const double aL = Energy(ra.left), aR = Energy(ra.right);
    const double bL = Energy(rb.left), bR = Energy(rb.right);
    CHECK(std::fabs(aL - bR) < 0.15 * (std::max)(aL, bR));
    CHECK(std::fabs(aR - bL) < 0.15 * (std::max)(aR, bL));
    // The room is not symmetric by itself, so the two ears differ.
    CHECK(std::fabs(aL - aR) > 0.1 * (std::max)(aL, aR));
}

void TestDeterminism() {
    Room r = Hall();
    r.furniture.push_back({FurnitureKind::Piano, 10.0f, 15.0f, false});
    const Vec3 s{6.0f, 8.0f, 1.5f}, l{12.0f, 24.0f, 1.2f};
    const auto refl = FindReflections(r, s, l, 3);
    const EarlyResponse x = BuildEarlyResponse(refl, l, 20.0f, true, true, 48000.0f);
    const EarlyResponse y = BuildEarlyResponse(refl, l, 20.0f, true, true, 48000.0f);
    CHECK(x.left.size() == y.left.size());
    CHECK(std::memcmp(x.left.data(), y.left.data(), x.left.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(x.right.data(), y.right.data(), x.right.size() * sizeof(float)) == 0);
    CHECK(Finite(x.left) && Finite(x.right));
    CHECK(Energy(x.left) > 0.0 && Energy(x.right) > 0.0);
}

// Reflections for one source must not depend on any other source solved
// before it, and furniture hits must follow their own source.
void TestPerSourceIndependence() {
    Room r = Hall();
    r.furniture.push_back({FurnitureKind::Bookshelf, 12.0f, 18.0f, false});
    r.furniture.push_back({FurnitureKind::Pillar, 6.0f, 10.0f, false});
    const Vec3 a{11.0f, 5.0f, 1.5f}, b{13.0f, 10.0f, 1.5f}, l{12.0f, 12.0f, 1.2f};
    const auto a1 = FindReflections(r, a, l, 3);
    const auto b1 = FindReflections(r, b, l, 3);
    const auto a2 = FindReflections(r, a, l, 3);
    CHECK(a1.size() == a2.size());
    bool same = a1.size() == a2.size();
    for (size_t i = 0; same && i < a1.size(); ++i)
        same = a1[i].path == a2[i].path && a1[i].image.x == a2[i].image.x;
    CHECK(same);
    // The furniture taps of the two sources differ.
    float pa = 0.0f, pb = 0.0f;
    for (const EarlyPath& rf : a1) if (rf.furniture) { pa = rf.path; break; }
    for (const EarlyPath& rf : b1) if (rf.furniture) { pb = rf.path; break; }
    CHECK(pa > 0.0f && pb > 0.0f && std::fabs(pa - pb) > 1.0f);
}

void TestOcclusion() {
    Room r = Uniform(Material::Plaster, 10.0f, 10.0f, 3.0f);
    r.furniture.push_back({FurnitureKind::Pillar, 5.0f, 5.0f, false});
    CHECK(Occluded(r, Vec3{5.0f, 1.0f, 1.5f}, Vec3{5.0f, 9.0f, 1.5f}));
    CHECK(!Occluded(r, Vec3{2.0f, 1.0f, 1.5f}, Vec3{2.0f, 9.0f, 1.5f}));
    // Over the top of a sofa is clear, through it is not.
    Room s = Uniform(Material::Plaster, 10.0f, 10.0f, 3.0f);
    s.furniture.push_back({FurnitureKind::Sofa, 5.0f, 5.0f, true});
    CHECK(Occluded(s, Vec3{5.0f, 1.0f, 0.4f}, Vec3{5.0f, 9.0f, 0.4f}));
    CHECK(!Occluded(s, Vec3{5.0f, 1.0f, 2.0f}, Vec3{5.0f, 9.0f, 2.0f}));
    // A table never blocks.
    Room t = Uniform(Material::Plaster, 10.0f, 10.0f, 3.0f);
    t.furniture.push_back({FurnitureKind::Table, 5.0f, 5.0f, false});
    CHECK(!Occluded(t, Vec3{5.0f, 1.0f, 0.4f}, Vec3{5.0f, 9.0f, 0.4f}));

    const Vec3 l{5.0f, 9.0f, 1.5f};
    const DirectPath hidden = SolveDirect(r, Vec3{5.0f, 1.0f, 1.5f}, l, 180.0f, true, true);
    const DirectPath open = SolveDirect(r, Vec3{2.0f, 1.0f, 1.5f}, l, 180.0f, true, true);
    CHECK(hidden.occluded && !open.occluded);
    CHECK(hidden.occlusionDb[3] > 10.0f && open.occlusionDb[3] == 0.0f);
}

void TestDirectPath() {
    const Room r = Uniform(Material::Plaster, 20.0f, 20.0f, 4.0f);
    const Vec3 l{10.0f, 10.0f, 1.5f};
    const DirectPath right = SolveDirect(r, Vec3{14.0f, 10.0f, 1.5f}, l, 0.0f, false, true);
    CHECK(std::fabs(right.azimuthDegrees - 90.0f) < 0.5f);
    CHECK(std::fabs(right.delaySeconds - 4.0f / kSpeedOfSound) < 1e-6f);
    for (int b = 0; b < kBands; ++b) CHECK(std::fabs(right.gain[b] - 0.25f) < 1e-5f);
    const DirectPath ahead = SolveDirect(r, Vec3{10.0f, 14.0f, 1.5f}, l, 0.0f, true, true);
    const DirectPath behind = SolveDirect(r, Vec3{10.0f, 6.0f, 1.5f}, l, 0.0f, true, true);
    CHECK(std::fabs(ahead.azimuthDegrees) < 0.5f);
    CHECK(std::fabs(std::fabs(behind.azimuthDegrees) - 180.0f) < 0.5f);
    CHECK(behind.lowpassHz < ahead.lowpassHz);
    // Turning the listener around swaps ahead and behind.
    const DirectPath turned = SolveDirect(r, Vec3{10.0f, 14.0f, 1.5f}, l, 180.0f, true, true);
    CHECK(std::fabs(std::fabs(turned.azimuthDegrees) - 180.0f) < 0.5f);
    // Air darkens distant sound: far loses more of the 8 kHz band than the 125 Hz band.
    const DirectPath far = SolveDirect(r, Vec3{10.0f, 19.5f, 1.5f}, l, 0.0f, true, false);
    const DirectPath dry = SolveDirect(r, Vec3{10.0f, 19.5f, 1.5f}, l, 0.0f, false, false);
    CHECK(far.gain[3] / dry.gain[3] < 0.95f);
    CHECK(far.gain[0] / dry.gain[0] > 0.99f);
    CHECK(far.lowpassHz == 20000.0f);
}

// ISO 9613-1 values at 20 C, 50 % humidity (dB per km).
void TestAirAbsorption() {
    CHECK(std::fabs(AirAlphaDbPerM(125.0) * 1000.0 - 0.44) < 0.02);
    CHECK(std::fabs(AirAlphaDbPerM(500.0) * 1000.0 - 2.73) < 0.08);
    CHECK(std::fabs(AirAlphaDbPerM(2000.0) * 1000.0 - 9.89) < 0.3);
    CHECK(std::fabs(AirAlphaDbPerM(8000.0) * 1000.0 - 105.29) < 3.2);
    CHECK(AirAlphaDbPerM(4000.0) > AirAlphaDbPerM(1000.0));
}

// Eyring never exceeds Sabine, matches it for hard walls and is much shorter
// for very absorbent ones.
void TestEyringVersusSabine() {
    float sab[kBands], eyr[kBands];
    Room hard = Uniform(Material::Plaster, 8.0f, 10.0f, 3.0f);
    ReverbTimes(hard, sab);
    EyringTimes(hard, eyr);
    CHECK(eyr[1] <= sab[1] + 1e-4f && eyr[1] > 0.97f * sab[1]);
    Room soft = Uniform(Material::Foam, 8.0f, 10.0f, 3.0f);
    ReverbTimes(soft, sab);
    EyringTimes(soft, eyr);
    CHECK(sab[2] > 1.4f * eyr[2]);
}

// Maekawa: a blocker costs highs far more than lows, a clear path costs
// nothing, several blockers add up to the 30 dB cap.
void TestBlocking() {
    Room r = Uniform(Material::Plaster, 10.0f, 10.0f, 3.0f);
    r.furniture.push_back({FurnitureKind::Pillar, 5.0f, 5.0f, false});
    const Vec3 a{5.0f, 1.0f, 1.5f}, b{5.0f, 9.0f, 1.5f};
    float loss[kBands];
    OcclusionLoss(r, a, b, loss);
    CHECK(loss[0] > 1.0f && loss[0] < 5.0f);
    CHECK(loss[0] < loss[1] && loss[1] < loss[2] && loss[2] < loss[3]);
    CHECK(loss[1] > 7.0f && loss[1] < 9.5f);
    CHECK(loss[3] > 15.0f && loss[3] <= 24.0f);
    OcclusionLoss(r, Vec3{2.0f, 1.0f, 1.5f}, Vec3{2.0f, 9.0f, 1.5f}, loss);
    for (int k = 0; k < kBands; ++k) CHECK(loss[k] == 0.0f);

    Room many = r;
    for (int i = 0; i < 4; ++i)
        many.furniture.push_back({FurnitureKind::Pillar, 5.0f, 3.0f + static_cast<float>(i), false});
    OcclusionLoss(many, a, b, loss);
    for (int k = 0; k < kBands; ++k) CHECK(loss[k] <= 30.0f);
    CHECK(loss[3] == 30.0f);
}

// A face of a piece of furniture returns the exact mirror path, and only when
// the bounce point lies on the face.
void TestFurnitureMirror() {
    Room r = Uniform(Material::Plaster, 20.0f, 20.0f, 4.0f);
    r.furniture.push_back({FurnitureKind::Bookshelf, 10.0f, 10.0f, false});
    const float plane = 10.0f - 0.5f * Info(FurnitureKind::Bookshelf).depth;
    const Vec3 s{9.0f, 5.0f, 1.5f}, l{11.0f, 5.0f, 1.5f};
    const float expected = Distance(Vec3{s.x, 2.0f * plane - s.y, s.z}, l);
    int count = 0;
    bool exact = false;
    for (const EarlyPath& e : FindReflections(r, s, l, 1))
        if (e.furniture) { ++count; exact = std::fabs(e.path - expected) < 1e-4f; }
    CHECK(count == 1 && exact);

    // Move the listener so the bounce point falls beside the shelf: no path.
    count = 0;
    for (const EarlyPath& e : FindReflections(r, s, Vec3{15.0f, 5.0f, 1.5f}, 1))
        if (e.furniture) ++count;
    CHECK(count == 0);
}

// The legs of a wall path through its bounce points add up to the image distance.
void TestWallBounceGeometry() {
    const Room r = Uniform(Material::Plaster, 8.0f, 10.0f, 3.0f);
    const Vec3 s{2.0f, 3.0f, 1.5f}, l{5.0f, 7.0f, 1.2f};
    for (const EarlyPath& e : FindReflections(r, s, l, 3)) {
        Vec3 pts[3];
        BouncePoints(r, s, l, e.seq, e.order, pts);
        float sum = Distance(s, pts[0]);
        for (int k = 1; k < e.order; ++k) sum += Distance(pts[k - 1], pts[k]);
        sum += Distance(pts[e.order - 1], l);
        CHECK(std::fabs(sum - e.path) < 1e-3f);
    }
}

// Two orderings of perpendicular walls share one image; only one is kept.
void TestNoDuplicateImages() {
    const Room r = Uniform(Material::Plaster, 8.0f, 10.0f, 3.0f);
    const auto list = FindReflections(r, Vec3{2.0f, 3.0f, 1.5f}, Vec3{5.0f, 7.0f, 1.2f}, 3);
    for (size_t i = 0; i < list.size(); ++i)
        for (size_t j = i + 1; j < list.size(); ++j)
            CHECK(Distance(list[i].image, list[j].image) > 1e-3f);
}

struct OneSided final : SourceModel {
    float Gain(const Vec3& dir, int) const noexcept override { return dir.x > 0.0f ? 0.25f : 1.0f; }
};

void TestSourceModel() {
    const Room r = Uniform(Material::Plaster, 20.0f, 20.0f, 4.0f);
    const Vec3 s{10.0f, 10.0f, 1.5f}, l{14.0f, 10.0f, 1.5f};
    const OneSided model;
    const OmniSource omni;
    const DirectPath a = SolveDirect(r, s, l, 0.0f, false, false, &model);
    const DirectPath b = SolveDirect(r, s, l, 0.0f, false, false, &omni);
    const DirectPath c = SolveDirect(r, s, l, 0.0f, false, false, nullptr);
    for (int k = 0; k < kBands; ++k) {
        CHECK(std::fabs(a.gain[k] - 0.25f * b.gain[k]) < 1e-6f);
        CHECK(b.gain[k] == c.gain[k]);
    }
}

void TestResponseShape() {
    const Room r = Hall();
    const Vec3 s{4.0f, 6.0f, 1.5f}, l{12.0f, 24.0f, 1.2f};
    const auto refl = FindReflections(r, s, l, 3);
    const float sr = 48000.0f;
    // Without the ear model both ears share one timeline.
    const EarlyResponse e = BuildEarlyResponse(refl, l, 0.0f, false, true, sr);
    float earliest = 1e9f;
    for (const EarlyPath& rf : refl) earliest = (std::min)(earliest, rf.path);
    const int first = static_cast<int>(earliest / kSpeedOfSound * sr) - 2;
    bool silent = true;
    for (int i = 0; i < first && i < static_cast<int>(e.left.size()); ++i)
        silent = silent && e.left[static_cast<size_t>(i)] == 0.0f && e.right[static_cast<size_t>(i)] == 0.0f;
    CHECK(silent);
    CHECK(Finite(e.left) && Finite(e.right));

    // Air takes energy away, and more of it from distant taps.
    const EarlyResponse dry = BuildEarlyResponse(refl, l, 0.0f, false, false, sr);
    CHECK(Energy(e.left) < Energy(dry.left));

    // A more absorbing room returns less early energy.
    Room soft = Hall();
    for (Material& m : soft.surface) m = Material::Foam;
    const auto softRefl = FindReflections(soft, s, l, 3);
    const EarlyResponse sf = BuildEarlyResponse(softRefl, l, 0.0f, false, true, sr);
    CHECK(Energy(sf.left) < 0.5 * Energy(e.left));

    // An open field (every surface open) returns nothing.
    Room open = Hall();
    for (Material& m : open.surface) m = Material::Open;
    const EarlyResponse none = BuildEarlyResponse(FindReflections(open, s, l, 3), l, 0.0f, true, true, sr);
    CHECK(Energy(none.left) == 0.0 && Energy(none.right) == 0.0);
}

} // namespace

int main() {
    TestMaterials();
    TestDecay();
    TestReflectionCounts();
    TestFirstOrderGeometry();
    TestMirroredRoom();
    TestDeterminism();
    TestPerSourceIndependence();
    TestOcclusion();
    TestDirectPath();
    TestResponseShape();
    TestAirAbsorption();
    TestEyringVersusSabine();
    TestBlocking();
    TestFurnitureMirror();
    TestWallBounceGeometry();
    TestSourceModel();
    TestNoDuplicateImages();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("stage solver tests passed\n");
    return 0;
}
