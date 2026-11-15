#include "render/StageBuilder.h"
#include "render/StageHrtf.h"

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

constexpr float kSr = 48000.0f;

// Runs `frames` frames of `feed` (one mono signal on channel `ch`) through the
// renderer in blocks of `block` frames; returns both ears.
void Run(StageRenderer& r, int ch, const std::vector<float>& feed, int block,
         std::vector<float>& outL, std::vector<float>& outR) {
    outL.assign(feed.size(), 0.0f);
    outR.assign(feed.size(), 0.0f);
    for (size_t pos = 0; pos < feed.size(); pos += static_cast<size_t>(block)) {
        const int n = static_cast<int>((std::min)(static_cast<size_t>(block), feed.size() - pos));
        const float* in[kMaxSources] = {};
        in[ch] = feed.data() + pos;
        r.Process(in, kMaxSources, n, outL.data() + pos, outR.data() + pos);
    }
}

std::vector<float> Noise(int n, uint32_t seed) {
    detail::Rng rng(seed);
    std::vector<float> v(static_cast<size_t>(n));
    for (float& x : v) x = (rng.Next() * 2.0f - 1.0f) * 0.3f;
    return v;
}

StageScene OneSource(const Room& room, const Vec3& source, const Vec3& listener) {
    StageScene s;
    s.room = room;
    s.listener = listener;
    s.source[0] = source;
    s.sourceOn[0] = true;
    return s;
}

Room OpenField() {
    Room r;
    for (Material& m : r.surface) m = Material::Open;
    return r;
}

BuildOptions Quick() {
    BuildOptions o;
    o.partition = 256;
    o.rays = 12000;
    o.air = false;
    return o;
}

// An impulse arrives after exactly distance / c, scaled by 1/d.
void TestDirectDelay() {
    const BuildOptions opt = Quick();
    StageRenderer r;
    r.Prepare(kSr, opt.partition, 2048, 80, 80);
    CpuTracer tracer;
    EarModelSpatializer ear;
    const StageScene scene = OneSource(OpenField(), Vec3{12.0f, 16.86f, 1.2f}, Vec3{12.0f, 10.0f, 1.2f});
    r.Submit(BuildRoomResponse(scene, ear, tracer, opt, r.Plan(), 1));
    std::vector<float> in(4096, 0.0f), l, rr;
    in[100] = 1.0f;
    Run(r, 0, in, 2048, l, rr);
    const float d = 6.86f;
    const int expected = 100 + static_cast<int>(std::lround(d / kSpeedOfSound * kSr));
    size_t peak = 0;
    for (size_t i = 0; i < l.size(); ++i) if (std::fabs(l[i]) > std::fabs(l[peak])) peak = i;
    CHECK(std::abs(static_cast<int>(peak) - expected) <= 1);
    CHECK(std::fabs(std::fabs(l[peak]) - 0.7071f / d) < 0.12f / d);   // centre pan, 1/d
    CHECK(std::fabs(l[peak] - rr[peak]) < 1e-5f);                     // straight ahead: both ears equal
}

// Output does not depend on how the stream is cut into blocks.
void TestBlockInvariance() {
    const BuildOptions opt = Quick();
    Room hall;
    CpuTracer tracer;
    EarModelSpatializer ear;
    StageScene scene = OneSource(hall, Vec3{6.0f, 8.0f, 1.5f}, Vec3{12.0f, 24.0f, 1.2f});
    scene.source[1] = Vec3{18.0f, 10.0f, 1.5f};
    scene.sourceOn[1] = true;
    StageRenderer probe;
    probe.Prepare(kSr, opt.partition, 4096, 80, 80);
    const auto response = BuildRoomResponse(scene, ear, tracer, opt, probe.Plan(), 1);
    const std::vector<float> feed = Noise(30000, 5);
    std::vector<float> a[2], b[2];
    for (int variant = 0; variant < 2; ++variant) {
        StageRenderer r;
        r.Prepare(kSr, opt.partition, 4096, 80, 80);
        r.Submit(response);
        Run(r, 0, feed, variant == 0 ? 4096 : 137, variant == 0 ? a[0] : b[0], variant == 0 ? a[1] : b[1]);
    }
    CHECK(std::memcmp(a[0].data(), b[0].data(), a[0].size() * sizeof(float)) == 0);
    CHECK(std::memcmp(a[1].data(), b[1].data(), a[1].size() * sizeof(float)) == 0);
    double e = 0.0;
    for (float x : a[0]) e += static_cast<double>(x) * x;
    CHECK(e > 0.0 && std::isfinite(e));
}

double FrequencyHz(const std::vector<float>& x, size_t from, size_t to) {
    int n = 0;
    double first = -1.0, last = -1.0;
    for (size_t i = from + 1; i < to; ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) {
            const double t = static_cast<double>(i - 1) + (-x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0.0) first = t;
            last = t;
            ++n;
        }
    return n > 1 ? (n - 1) * static_cast<double>(kSr) / (last - first) : 0.0;
}

// A source receding at v m/s shifts a tone by (1 - v / c): the per-block delay
// ramp is exact Doppler.
void TestDoppler() {
    const int block = 256;
    for (float v : {20.0f, -20.0f, 0.0f}) {
        StageRenderer r;
        r.Prepare(kSr, 256, block, 4, 4);
        const int blocks = 400;
        std::vector<float> out;
        std::vector<float> tone(static_cast<size_t>(blocks * block));
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.5f * std::sin(6.283185307f * 1000.0f * static_cast<float>(i) / kSr);
        std::vector<float> l(tone.size(), 0.0f), rr(tone.size(), 0.0f);
        for (int b = 0; b < blocks; ++b) {
            auto resp = std::make_shared<RoomResponse>();
            SourceResponse& s = resp->source[0];
            s.active = true;
            const float dist = 100.0f + v * static_cast<float>(b + 1) * block / kSr;
            s.direct.delaySeconds = dist / kSpeedOfSound;
            for (int e = 0; e < 2; ++e) for (float& g : s.direct.ears.band[e]) g = 1.0f;
            r.Submit(resp);
            const float* in[kMaxSources] = {};
            in[0] = tone.data() + static_cast<size_t>(b) * block;
            r.Process(in, kMaxSources, block, l.data() + static_cast<size_t>(b) * block,
                      rr.data() + static_cast<size_t>(b) * block);
        }
        const double expected = 1000.0 * (1.0 - v / kSpeedOfSound);
        const double f = FrequencyHz(l, 48000, l.size() - 1000);
        CHECK(std::fabs(f - expected) < 0.003 * expected);
    }
}

// A closed room gives a tail that decays; an open field gives nothing after the direct sound.
void TestTail() {
    BuildOptions opt = Quick();
    opt.partition = 512;
    CpuTracer tracer;
    EarModelSpatializer ear;
    StageRenderer r;
    r.Prepare(kSr, opt.partition, 4096, 60, 1200);
    const StageScene hall = OneSource(Room{}, Vec3{12.0f, 10.0f, 1.5f}, Vec3{12.0f, 24.0f, 1.2f});
    const auto response = BuildRoomResponse(hall, ear, tracer, opt, r.Plan(), 1);
    CHECK(response->tail != nullptr);
    CHECK(response->meta.t60[1] > 1.0f && response->meta.t60[1] < 6.0f);
    r.Submit(response);
    std::vector<float> in(static_cast<size_t>(4 * 48000), 0.0f), l, rr;
    in[0] = 1.0f;
    Run(r, 0, in, 2048, l, rr);
    auto energy = [&](double t0, double t1) {
        double e = 0.0;
        for (size_t i = static_cast<size_t>(t0 * kSr); i < static_cast<size_t>(t1 * kSr); ++i)
            e += static_cast<double>(l[i]) * l[i] + static_cast<double>(rr[i]) * rr[i];
        return e;
    };
    const double early = energy(0.1, 0.4), late = energy(1.6, 1.9);
    CHECK(early > 0.0 && late > 0.0);
    CHECK(late < 0.5 * early);
    for (float x : l) CHECK(std::isfinite(x));

    StageRenderer open;
    open.Prepare(kSr, opt.partition, 4096, 60, 1200);
    open.Submit(BuildRoomResponse(OneSource(OpenField(), Vec3{12.0f, 10.0f, 1.5f}, Vec3{12.0f, 24.0f, 1.2f}),
                                  ear, tracer, opt, open.Plan(), 1));
    Run(open, 0, in, 2048, l, rr);
    double e = 0.0;
    for (size_t i = static_cast<size_t>(0.2 * kSr); i < l.size(); ++i) e += static_cast<double>(l[i]) * l[i];
    CHECK(e == 0.0);
}

// The tail is cut into small partitions for its first samples and large ones after; the sum must be the
// plain convolution: an impulse in comes out as the response itself, one partition later.
void TestSplitTail() {
    for (int partition : {256, 512}) {
        StageRenderer r;
        r.Prepare(kSr, partition, 1024, 4, 200);
        const int len = 37000;
        const std::vector<float> hl = Noise(len, 11u), hr = Noise(len, 12u);
        auto response = std::make_shared<RoomResponse>();
        response->source[0].active = true;
        response->tail = MakeTailSpectra(r.Plan(), hl.data(), hr.data(), len);
        CHECK(response->tail && response->tail->late);
        r.Submit(response);
        std::vector<float> feed(static_cast<size_t>(len + 2 * partition + 9000), 0.0f);
        feed[100] = 1.0f;
        std::vector<float> l, rr;
        Run(r, 0, feed, 480, l, rr);
        double worst = 0.0;
        for (int j = 0; j < len; ++j) {
            const size_t at = static_cast<size_t>(100 + partition + j);
            worst = (std::max)(worst, static_cast<double>(std::fabs(l[at] - hl[static_cast<size_t>(j)])));
            worst = (std::max)(worst, static_cast<double>(std::fabs(rr[at] - hr[static_cast<size_t>(j)])));
        }
        std::printf("split tail partition %d: worst %.3g\n", partition, worst);
        CHECK(worst < 2e-4);
    }
}

// A loudspeaker keeps its level on axis and low frequencies everywhere; highs fall away off axis, more so
// behind it. A source without a pattern is untouched.
void TestSpeakerPattern() {
    SpeakerSource sp;
    sp.axis = {0.0f, 1.0f, 0.0f};
    const Vec3 onAxis{0.0f, 1.0f, 0.0f}, side{1.0f, 0.0f, 0.0f}, back{0.0f, -1.0f, 0.0f};
    for (int b = 0; b < kBands; ++b) CHECK(std::fabs(sp.Gain(onAxis, b) - 1.0f) < 1e-6f);
    CHECK(std::fabs(sp.Gain(back, 0) - 1.0f) < 1e-6f);                    // lows go everywhere
    for (int b = 1; b < kBands; ++b) CHECK(sp.Gain(side, b) < sp.Gain(onAxis, b));
    for (int b = 1; b < kBands; ++b) CHECK(sp.Gain(back, b) < sp.Gain(side, b));
    CHECK(sp.Gain(side, 3) < sp.Gain(side, 1));                          // the narrower the higher

    // The scene builder applies it: the same speaker turned away is quieter in the highs.
    const BuildOptions opt = Quick();
    CpuTracer tracer;
    EarModelSpatializer ear;
    StageRenderer probe;
    probe.Prepare(kSr, opt.partition, 1024, 20, 20);
    float high[2] = {};
    for (int turned = 0; turned < 2; ++turned) {
        StageScene scene = OneSource(OpenField(), Vec3{12.0f, 16.0f, 1.2f}, Vec3{12.0f, 10.0f, 1.2f});
        scene.sourceDirectional[0] = true;
        scene.sourceAim[0] = turned ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, -1.0f, 0.0f};   // at the listener / away
        const auto r = BuildRoomResponse(scene, ear, tracer, opt, probe.Plan(), 1);
        high[turned] = r->source[0].direct.ears.band[0][3];
    }
    CHECK(high[1] < 0.5f * high[0]);
}

// Same scene, same bytes.
void TestDeterministicBuild() {
    const BuildOptions opt = Quick();
    CpuTracer tracer;
    EarModelSpatializer ear;
    const StageScene s = OneSource(Room{}, Vec3{6.0f, 8.0f, 1.5f}, Vec3{12.0f, 24.0f, 1.2f});
    StageRenderer r;
    r.Prepare(kSr, opt.partition, 2048, 80, 1200);
    const auto a = BuildRoomResponse(s, ear, tracer, opt, r.Plan(), 1);
    const auto b = BuildRoomResponse(s, ear, tracer, opt, r.Plan(), 1);
    CHECK(a->tail && b->tail && a->tail->re[0].size() == b->tail->re[0].size());
    CHECK(std::memcmp(a->tail->re[0].data(), b->tail->re[0].data(), a->tail->re[0].size() * sizeof(float)) == 0);
    CHECK(a->source[0].tapCount == b->source[0].tapCount && a->source[0].tapCount > 0);
    CHECK(a->source[0].direct.delaySeconds == b->source[0].direct.delaySeconds);
}

// A mirrored scene mirrors the ears.
void TestMirroredEars() {
    const BuildOptions opt = Quick();
    CpuTracer tracer;
    EarModelSpatializer ear;
    Room room;
    room.width = 20.0f; room.depth = 20.0f; room.height = 4.0f;
    const StageScene a = OneSource(room, Vec3{4.0f, 14.0f, 1.4f}, Vec3{10.0f, 10.0f, 1.2f});
    const StageScene b = OneSource(room, Vec3{16.0f, 14.0f, 1.4f}, Vec3{10.0f, 10.0f, 1.2f});
    FftPlan plan(2 * opt.partition);
    const auto ra = BuildPaths(a, ear, opt, plan, nullptr, RoomMeta{}, 1, true);
    const auto rb = BuildPaths(b, ear, opt, plan, nullptr, RoomMeta{}, 1, true);
    for (int k = 0; k < kBands; ++k) {
        CHECK(std::fabs(ra->source[0].direct.ears.band[0][k] - rb->source[0].direct.ears.band[1][k]) < 1e-5f);
        CHECK(std::fabs(ra->source[0].direct.ears.band[1][k] - rb->source[0].direct.ears.band[0][k]) < 1e-5f);
    }
    CHECK(std::fabs(ra->source[0].direct.ears.delay[0] + rb->source[0].direct.ears.delay[0]) < 1e-9f);
}

} // namespace

// The measured head: the table loads, a source on the left reaches the left ear first and louder,
// the mirror image does the opposite, and the output stays independent of the block size.
double Energy(const std::vector<float>& v) {
    double e = 0.0;
    for (float x : v) e += static_cast<double>(x) * x;
    return e;
}

size_t Onset(const std::vector<float>& v) {
    float peak = 0.0f;
    for (float x : v) peak = (std::max)(peak, std::fabs(x));
    for (size_t i = 0; i < v.size(); ++i) if (std::fabs(v[i]) > 0.2f * peak) return i;
    return 0;
}

void TestHeadTable() {
    size_t size = 0;
    const unsigned char* bytes = EmbeddedHeadTable(size);
    HeadTable table;
    CHECK(table.Parse(bytes, size));
    CHECK(table.directions == 440 && table.taps == 256 && std::fabs(table.sampleRate - 44100.0f) < 1.0f);
    CHECK(!table.Parse(bytes, size - 1));   // a truncated file is refused
    HrtfSpatializer head;
    CHECK(head.Build(table, kSr));
    CHECK(head.FilterLength() >= 32 && head.FilterLength() <= kMaxFir);
    const BuildOptions opt = Quick();
    CpuTracer tracer;
    const Vec3 listener{12.0f, 10.0f, 1.2f};
    auto render = [&](const Vec3& src, int block) {
        StageRenderer r;
        r.Prepare(kSr, opt.partition, 4096, 80, 80);
        r.Submit(BuildRoomResponse(OneSource(OpenField(), src, listener), head, tracer, opt, r.Plan(), 1));
        std::vector<float> in(4096, 0.0f), l, rr;
        in[100] = 1.0f;
        Run(r, 0, in, block, l, rr);
        return std::make_pair(l, rr);
    };
    const auto left = render(Vec3{6.0f, 10.0f, 1.2f}, 2048);    // 6 m to the listener's left
    const auto right = render(Vec3{18.0f, 10.0f, 1.2f}, 2048);
    const auto front = render(Vec3{12.0f, 16.0f, 1.2f}, 2048);
    const int itd = static_cast<int>(Onset(left.second)) - static_cast<int>(Onset(left.first));
    std::printf("head: filter %d taps, left source ITD %d samples, energy L/R %.2f\n", head.FilterLength(), itd,
                Energy(left.first) / Energy(left.second));
    CHECK(itd >= 20 && itd <= 40);                                         // about 0.45 to 0.8 ms at 48 kHz
    CHECK(Energy(left.first) > 1.5 * Energy(left.second));                 // near ear louder
    CHECK(Onset(right.first) > Onset(right.second));                       // and the mirror image
    CHECK(Energy(right.second) > 1.5 * Energy(right.first));
    const double ratio = Energy(front.first) / Energy(front.second);
    CHECK(ratio > 0.6 && ratio < 1.7);                                     // straight ahead is near symmetric
    // Block size changes nothing.
    const auto a = render(Vec3{7.0f, 14.0f, 2.0f}, 4096);
    const auto b = render(Vec3{7.0f, 14.0f, 2.0f}, 137);
    CHECK(std::memcmp(a.first.data(), b.first.data(), a.first.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(a.second.data(), b.second.data(), a.second.size() * sizeof(float)) == 0);
}

// A source that moves from the left to the right while a tone plays: the filter and the delay glide
// across the block, so no sample steps more than the tone itself does.
void TestHeadMotion() {
    HeadTable table;
    size_t size = 0;
    const unsigned char* bytes = EmbeddedHeadTable(size);
    CHECK(table.Parse(bytes, size));
    HrtfSpatializer head;
    CHECK(head.Build(table, kSr));
    BuildOptions opt = Quick();
    opt.buildEarly = false;
    CpuTracer tracer;
    const Vec3 listener{12.0f, 10.0f, 1.2f};
    StageRenderer r;
    r.Prepare(kSr, opt.partition, 256, 80, 80);
    const auto first = BuildRoomResponse(OneSource(OpenField(), Vec3{6.0f, 10.0f, 1.2f}, listener), head, tracer, opt, r.Plan(), 1);
    r.Submit(first);
    std::vector<float> tone(256 * 24);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.3f * std::sin(6.283185307179586f * 300.0f * static_cast<float>(i) / kSr);
    std::vector<float> l(tone.size(), 0.0f), rr(tone.size(), 0.0f);
    for (size_t pos = 0, blk = 0; pos < tone.size(); pos += 256, ++blk) {
        if (blk == 12)
            r.Submit(BuildPaths(OneSource(OpenField(), Vec3{18.0f, 10.0f, 1.2f}, listener), head, opt, r.Plan(), first->tail,
                                first->meta, 2, false));
        const float* in[kMaxSources] = {};
        in[0] = tone.data() + pos;
        r.Process(in, kMaxSources, 256, l.data() + pos, rr.data() + pos);
    }
    float steady = 0.0f, across = 0.0f;
    for (size_t i = 256 * 6 + 1; i < 256 * 10; ++i) steady = (std::max)(steady, std::fabs(l[i] - l[i - 1]));
    for (size_t i = 256 * 11; i < 256 * 14; ++i) across = (std::max)(across, std::fabs(l[i] - l[i - 1]));
    std::printf("head motion: steady step %.5f, across the move %.5f\n", steady, across);
    CHECK(steady > 0.0f && across < 3.0f * steady);
}

int main() {
    TestDirectDelay();
    TestBlockInvariance();
    TestDoppler();
    TestSpeakerPattern();
    TestSplitTail();
    TestTail();
    TestDeterministicBuild();
    TestMirroredEars();
    TestHeadTable();
    TestHeadMotion();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("stage renderer tests passed\n");
    return 0;
}
