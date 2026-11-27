#include "render/ReleaseConvolver.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

using svms::bus::ConvHistory;
using svms::bus::ConvKernel;
using svms::bus::MakeKernel;

struct Kernel {
    std::vector<float> l, r;
};

Kernel RandomKernel(int length, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    Kernel k;
    k.l.resize(static_cast<size_t>(length));
    k.r.resize(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i) {
        const float fade = std::exp(-3.0f * i / length);
        k.l[static_cast<size_t>(i)] = u(rng) * fade;
        k.r[static_cast<size_t>(i)] = u(rng) * fade;
    }
    return k;
}

// Sparse impulses (most of the input of a bus) with a stretch of dense noise in the middle.
std::vector<float> RandomInput(int frames, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::uniform_int_distribution<int> gap(0, 40);
    std::vector<float> x(static_cast<size_t>(frames), 0.0f);
    for (int i = 0; i < frames; i += 1 + gap(rng)) x[static_cast<size_t>(i)] = u(rng);
    for (int i = frames / 3; i < frames / 3 + 500 && i < frames; ++i) x[static_cast<size_t>(i)] = u(rng);
    return x;
}

// Runs the convolver block by block and returns the stereo output, interleaved by side: [0, frames) left,
// [frames, 2 frames) right.
std::vector<float> Convolve(const ConvKernel& k, const std::vector<float>& x, int block) {
    const int frames = static_cast<int>(x.size());
    std::vector<float> out(static_cast<size_t>(2 * frames), 0.0f);
    ConvHistory h;
    h.Prepare(k);
    for (int b = 0; b + block <= frames; b += block)
        h.Process(k, &x[static_cast<size_t>(b)], &out[static_cast<size_t>(b)], &out[static_cast<size_t>(frames + b)]);
    return out;
}

// Largest error against direct convolution in double, relative to the reference's peak.
double DirectError(const Kernel& kern, const std::vector<float>& x, const std::vector<float>& got) {
    const int frames = static_cast<int>(x.size()), len = static_cast<int>(kern.l.size());
    double peak = 0.0, err = 0.0;
    for (int side = 0; side < 2; ++side) {
        const std::vector<float>& h = side ? kern.r : kern.l;
        for (int t = 0; t < frames; ++t) {
            double y = 0.0;
            for (int tau = 0; tau < len && tau <= t; ++tau) y += double(h[static_cast<size_t>(tau)]) * x[static_cast<size_t>(t - tau)];
            peak = std::fmax(peak, std::fabs(y));
            err = std::fmax(err, std::fabs(y - got[static_cast<size_t>(side * frames + t)]));
        }
    }
    return err / (std::max)(peak, 1e-30);
}

bool MatchesDirect(int block, int length, int levels, int blocks, unsigned seed, const char* name) {
    const Kernel kern = RandomKernel(length, seed);
    const ConvKernel k = MakeKernel(kern.l.data(), kern.r.data(), length, block, levels);
    const std::vector<float> x = RandomInput(block * blocks, seed + 1);
    const double e = DirectError(kern, x, Convolve(k, x, block));
    const bool ok = e < 2e-6;
    std::printf("%s %s: block %d, kernel %d frames, %zu levels, error %.2e of peak\n", ok ? "PASS" : "FAIL", name,
                block, length, k.levels.size(), e);
    return ok;
}

// An impulse in a block's first frame reaches that block's own output: no latency, left and right apart.
bool ZeroLatency() {
    const int block = 64, length = 64 * 40;
    const Kernel kern = RandomKernel(length, 7);
    const ConvKernel k = MakeKernel(kern.l.data(), kern.r.data(), length, block, 3);
    std::vector<float> x(static_cast<size_t>(block * 50), 0.0f);
    x[static_cast<size_t>(3 * block)] = 1.0f;
    const std::vector<float> y = Convolve(k, x, block);
    double err = 0.0;
    for (int t = 0; t < length; ++t) {
        err = std::fmax(err, std::fabs(y[static_cast<size_t>(3 * block + t)] - kern.l[static_cast<size_t>(t)]));
        err = std::fmax(err, std::fabs(y[x.size() + static_cast<size_t>(3 * block + t)] - kern.r[static_cast<size_t>(t)]));
    }
    double before = 0.0;
    for (int t = 0; t < 3 * block; ++t) before = std::fmax(before, std::fabs(y[static_cast<size_t>(t)]));
    const bool ok = err < 1e-5 && before == 0.0;
    std::printf("%s zero latency: impulse response error %.2e, output before the impulse %.2e\n", ok ? "PASS" : "FAIL", err, before);
    return ok;
}

// A bank of three inputs against three kernels (one, two and three levels; the last input joins late) is the sum
// of the three direct convolutions.
bool BankMatchesDirect() {
    const int block = 64, blocks = 260, frames = block * blocks;
    const int lengths[3] = {64 * 6, 64 * 30, 64 * 45 + 17};
    Kernel kern[3];
    std::vector<ConvKernel> k;
    std::vector<float> x[3];
    for (int i = 0; i < 3; ++i) {
        kern[i] = RandomKernel(lengths[i], 20u + i);
        k.push_back(MakeKernel(kern[i].l.data(), kern[i].r.data(), lengths[i], block, 3));
        x[i] = RandomInput(frames, 30u + i);
    }
    const int join = 37;   // the third input is silent before block 37 and added there
    std::fill(x[2].begin(), x[2].begin() + join * block, 0.0f);
    svms::bus::ConvBank bank;
    bank.Prepare(block, 3);
    int id[3] = {bank.Add(k[0]), bank.Add(k[1]), -1};
    std::vector<float> got(static_cast<size_t>(2 * frames), 0.0f);
    for (int b = 0; b < blocks; ++b) {
        if (b == join) id[2] = bank.Add(k[2]);
        for (int i = 0; i < 3; ++i)
            if (id[i] >= 0) std::memcpy(bank.InputBlock(id[i]), &x[i][static_cast<size_t>(b * block)], sizeof(float) * block);
        bank.Process(&got[static_cast<size_t>(b * block)], &got[static_cast<size_t>(frames + b * block)], static_cast<uint64_t>(b));
    }
    double peak = 0.0, err = 0.0;
    for (int side = 0; side < 2; ++side)
        for (int t = 0; t < frames; ++t) {
            double y = 0.0;
            for (int i = 0; i < 3; ++i) {
                const std::vector<float>& h = side ? kern[i].r : kern[i].l;
                for (int tau = 0; tau < lengths[i] && tau <= t; ++tau) y += double(h[static_cast<size_t>(tau)]) * x[i][static_cast<size_t>(t - tau)];
            }
            peak = std::fmax(peak, std::fabs(y));
            err = std::fmax(err, std::fabs(y - got[static_cast<size_t>(side * frames + t)]));
        }
    const double e = err / (std::max)(peak, 1e-30);
    const bool ok = e < 2e-6;
    std::printf("%s bank of three: error %.2e of peak\n", ok ? "PASS" : "FAIL", e);
    return ok;
}

// Late impulses: a kernel without level 0 (the bus's), impulses added blocks after their frame, each at a random
// block up to its deadline (Insertable), equal direct convolution with the kernel's first 8 blocks zeroed.
bool LateImpulses() {
    const int block = 64, blocks = 300, frames = block * blocks, length = 64 * 45 + 17;
    Kernel kern = RandomKernel(length, 60);
    ConvKernel k = MakeKernel(kern.l.data(), kern.r.data(), length, block, 3);
    k.levels[0].parts = 0;
    k.levels[0].h.clear();
    for (int t = 0; t < 8 * block; ++t) kern.l[static_cast<size_t>(t)] = kern.r[static_cast<size_t>(t)] = 0.0f;
    svms::bus::ConvBank bank;
    bank.Prepare(block, 3);
    const int id = bank.Add(k);
    std::mt19937 rng(61);
    std::uniform_int_distribution<int> when(0, frames - 1);
    std::uniform_real_distribution<float> weight(-1.0f, 1.0f);
    struct Imp { int frame, at; float w; };
    std::vector<Imp> imps;
    std::vector<float> x(static_cast<size_t>(frames), 0.0f);
    for (int i = 0; i < 400; ++i) {
        const int f = when(rng);
        int last = f / block;   // the last block before which it can still go in
        while (last + 1 < blocks && bank.Insertable(static_cast<uint64_t>(f), static_cast<uint64_t>(last + 1), false)) ++last;
        std::uniform_int_distribution<int> at(f / block, last);
        imps.push_back({f, at(rng), weight(rng)});
        x[static_cast<size_t>(f)] += imps.back().w;
    }
    std::vector<float> got(static_cast<size_t>(2 * frames), 0.0f);
    int late = 0;
    for (int b = 0; b < blocks; ++b) {
        for (const Imp& m : imps)
            if (m.at == b) {
                bank.AddImpulse(id, static_cast<uint64_t>(m.frame), m.w, static_cast<uint64_t>(b));
                late += m.frame / block < b;
            }
        bank.Process(&got[static_cast<size_t>(b * block)], &got[static_cast<size_t>(frames + b * block)], static_cast<uint64_t>(b));
    }
    const double e = DirectError(kern, x, got);
    const bool ok = e < 2e-6 && late > 100;
    std::printf("%s late impulses: %d of 400 added after their block, error %.2e of peak\n", ok ? "PASS" : "FAIL", late, e);
    return ok;
}

// Moved impulses: a bus kernel (no level 0), impulses on time into bank A; after block `at` - 1 the impulses
// marked moved go from A to B (null: dropped) from that block's frame `offset` on, and the later ones straight to
// B. A then sounds the kept impulses, and the moved ones only before the move; B the moved ones only after it.
bool MovedImpulses(int at, int offset, bool drop, unsigned seed) {
    const int block = 64, blocks = 300, frames = block * blocks, length = 64 * 45 + 17;
    Kernel kern = RandomKernel(length, seed);
    ConvKernel k = MakeKernel(kern.l.data(), kern.r.data(), length, block, 3);
    k.levels[0].parts = 0;
    k.levels[0].h.clear();
    for (int t = 0; t < 8 * block; ++t) kern.l[static_cast<size_t>(t)] = kern.r[static_cast<size_t>(t)] = 0.0f;
    std::mt19937 rng(seed + 1);
    std::uniform_int_distribution<int> when(0, frames - 1), coin(0, 1);
    std::uniform_real_distribution<float> weight(-1.0f, 1.0f);
    struct Imp { int frame; float w; bool moved; };
    std::vector<Imp> imps;
    std::vector<float> kept(static_cast<size_t>(frames), 0.0f), moved(kept);
    for (int i = 0; i < 300; ++i) {
        imps.push_back({when(rng), weight(rng), coin(rng) == 1});
        (imps.back().moved ? moved : kept)[static_cast<size_t>(imps.back().frame)] += imps.back().w;
    }
    svms::bus::ConvBank a, b;
    a.Prepare(block, 3);
    b.Prepare(block, 3);
    const int ia = a.Add(k), ib = b.Add(k);
    std::vector<float> outA(static_cast<size_t>(2 * frames), 0.0f), outB(outA);
    const int moveFrame = (at - 1) * block + offset;
    for (int bl = 0; bl < blocks; ++bl) {
        if (bl == at) {
            std::vector<std::pair<uint64_t, float>> list;
            for (const Imp& m : imps)
                if (m.moved && m.frame < at * block) list.emplace_back(static_cast<uint64_t>(m.frame), m.w);
            float* const preA[2] = {&outA[static_cast<size_t>((at - 1) * block)], &outA[static_cast<size_t>(frames + (at - 1) * block)]};
            float* const preB[2] = {&outB[static_cast<size_t>((at - 1) * block)], &outB[static_cast<size_t>(frames + (at - 1) * block)]};
            svms::bus::ConvBank::Move(a, ia, drop ? nullptr : &b, ib, list.data(), list.size(), static_cast<uint64_t>(at),
                                      static_cast<uint32_t>(offset), preA, preB);
        }
        for (const Imp& m : imps)
            if (m.frame / block == bl) {
                const bool toB = m.moved && bl >= at;
                if (toB && drop) continue;
                (toB ? b : a).AddImpulse(toB ? ib : ia, static_cast<uint64_t>(m.frame), m.w, static_cast<uint64_t>(bl));
            }
        a.Process(&outA[static_cast<size_t>(bl * block)], &outA[static_cast<size_t>(frames + bl * block)], static_cast<uint64_t>(bl));
        b.Process(&outB[static_cast<size_t>(bl * block)], &outB[static_cast<size_t>(frames + bl * block)], static_cast<uint64_t>(bl));
    }
    // The references in double: A = kept + moved before the move, B = moved after it (or nothing, dropped).
    double peak = 0.0, errA = 0.0, errB = 0.0;
    for (int side = 0; side < 2; ++side)
        for (int t = 0; t < frames; ++t) {
            const std::vector<float>& h = side ? kern.r : kern.l;
            double yk = 0.0, ym = 0.0;
            for (int tau = 0; tau < length && tau <= t; ++tau) {
                yk += double(h[static_cast<size_t>(tau)]) * kept[static_cast<size_t>(t - tau)];
                ym += double(h[static_cast<size_t>(tau)]) * moved[static_cast<size_t>(t - tau)];
            }
            const double wantA = yk + (t < moveFrame ? ym : 0.0), wantB = t < moveFrame || drop ? 0.0 : ym;
            peak = std::fmax(peak, std::fabs(yk + ym));
            errA = std::fmax(errA, std::fabs(wantA - outA[static_cast<size_t>(side * frames + t)]));
            errB = std::fmax(errB, std::fabs(wantB - outB[static_cast<size_t>(side * frames + t)]));
        }
    errA /= (std::max)(peak, 1e-30);
    errB /= (std::max)(peak, 1e-30);
    const bool ok = errA < 2e-6 && errB < 2e-6;
    std::printf("%s moved impulses (after block %d, frame %d of it%s): error %.2e / %.2e of peak\n", ok ? "PASS" : "FAIL",
                at - 1, offset, drop ? ", dropped" : "", errA, errB);
    return ok;
}

// The AVX products and butterflies give the scalar loops' bits (renders do not depend on the CPU).
bool AvxMatchesScalar() {
    if (!svms::bus::BusAvx()) { std::printf("SKIP avx equals scalar: no AVX\n"); return true; }
    const int block = 64, blocks = 120, frames = block * blocks, length = 64 * 45 + 17;
    const Kernel kern = RandomKernel(length, 50);
    const std::vector<float> x = RandomInput(frames, 51);
    std::vector<float> out[2];
    for (int pass = 0; pass < 2; ++pass) {
        svms::bus::busScalarOnly = pass == 1;
        const ConvKernel k = MakeKernel(kern.l.data(), kern.r.data(), length, block, 3);
        svms::bus::ConvBank bank;
        bank.Prepare(block, 3);
        const int id = bank.Add(k);
        out[pass].assign(static_cast<size_t>(2 * frames), 0.0f);
        for (int b = 0; b < blocks; ++b) {
            std::memcpy(bank.InputBlock(id), &x[static_cast<size_t>(b * block)], sizeof(float) * block);
            bank.Process(&out[pass][static_cast<size_t>(b * block)], &out[pass][static_cast<size_t>(frames + b * block)], static_cast<uint64_t>(b));
        }
    }
    svms::bus::busScalarOnly = false;
    const bool ok = std::memcmp(out[0].data(), out[1].data(), sizeof(float) * out[0].size()) == 0;
    std::printf("%s avx equals scalar: kernel and bank output bit for bit\n", ok ? "PASS" : "FAIL");
    return ok;
}

// The work of the long partitions is spread: no block does much more than the average.
bool EvenWork() {
    const int block = 1024, length = 240000;
    const Kernel kern = RandomKernel(length, 11);
    const ConvKernel k = MakeKernel(kern.l.data(), kern.r.data(), length, block, 3);
    ConvHistory h;
    h.Prepare(k);
    std::vector<float> in(static_cast<size_t>(block), 0.0f), l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    double sum = 0.0, most = 0.0;
    const int blocks = 512;
    for (int b = 0; b < blocks; ++b) {
        h.Process(k, in.data(), l.data(), r.data());
        sum += h.LastBlockFlops();
        most = std::fmax(most, h.LastBlockFlops());
    }
    const double mean = sum / blocks, perFrame = svms::bus::FlopsPerFrame(k);
    const bool ok = most < 1.35 * mean && std::fabs(mean / block - perFrame) < 0.01 * perFrame;
    std::printf("%s even work: %zu levels, %.0f flops per frame, busiest block %.2fx the mean\n", ok ? "PASS" : "FAIL",
                k.levels.size(), perFrame, most / mean);
    return ok;
}

// Time per second of 48 kHz output for one history on a release-bus-sized kernel. Informational.
void Bench() {
    const int block = 1024, length = 240000, blocks = 4000;
    const Kernel kern = RandomKernel(length, 13);
    const ConvKernel k = MakeKernel(kern.l.data(), kern.r.data(), length, block, 3);
    ConvHistory h;
    h.Prepare(k);
    std::vector<float> in(static_cast<size_t>(block), 0.0f), l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    in[5] = 1.0f;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) h.Process(k, in.data(), l.data(), r.data());
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double audio = double(blocks) * block / 48000.0;
    std::printf("bench: %.0f flops per frame, %.2f ms per second of audio (%.2f GFLOP/s), %.0f histories per core in realtime\n",
                svms::bus::FlopsPerFrame(k), 1000.0 * s / audio, svms::bus::FlopsPerFrame(k) * blocks * block / s / 1e9,
                audio / s);
}

// A bank at the load of one DYHTM shard: 210 inputs on 5 s kernels, each taking a note in about one block in four.
// Informational: time per second of 48 kHz output.
void BenchBank(int kernels, int levels) {
    const int block = 1024, length = 240000, inputs = 210, blocks = 1000;
    std::vector<ConvKernel> k;
    for (int i = 0; i < kernels; ++i) {
        const Kernel kern = RandomKernel(length, 40u + i);
        k.push_back(MakeKernel(kern.l.data(), kern.r.data(), length, block, levels));
    }
    svms::bus::ConvBank bank;
    bank.Prepare(block, levels);
    std::vector<int> id;
    for (int i = 0; i < inputs; ++i) id.push_back(bank.Add(k[static_cast<size_t>(i % kernels)]));
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> when(0, 4 * block - 1);
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < inputs; ++i) {
            const int at = when(rng);
            if (at < block) bank.InputBlock(id[static_cast<size_t>(i)])[at] = 0.5f;
        }
        bank.Process(l.data(), r.data(), static_cast<uint64_t>(b));
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bank bench: %d inputs, %.0f ms per second of audio\n", inputs, 1000.0 * s / (double(blocks) * block / 48000.0));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--bench")) { Bench(); return 0; }
    if (argc > 1 && !std::strcmp(argv[1], "--bench-bank")) { BenchBank(argc > 2 ? std::atoi(argv[2]) : 8, argc > 3 ? std::atoi(argv[3]) : 3); return 0; }
    bool ok = true;
    ok &= MatchesDirect(64, 10, 3, 20, 1, "shorter than a block");
    ok &= MatchesDirect(64, 64 * 8, 3, 40, 2, "level 0 only");
    ok &= MatchesDirect(64, 64 * 32, 3, 120, 3, "two levels, exact");
    ok &= MatchesDirect(64, 64 * 45 + 17, 3, 200, 4, "three levels");
    ok &= MatchesDirect(32, 32 * 150 + 5, 4, 400, 5, "four levels");
    ok &= MatchesDirect(64, 64 * 45 + 17, 1, 120, 6, "one uniform level");
    ok &= ZeroLatency();
    ok &= BankMatchesDirect();
    ok &= LateImpulses();
    ok &= MovedImpulses(9, 0, false, 70);
    ok &= MovedImpulses(40, 17, false, 71);
    ok &= MovedImpulses(77, 63, false, 72);
    ok &= MovedImpulses(129, 5, false, 73);
    ok &= MovedImpulses(211, 30, false, 74);
    ok &= MovedImpulses(100, 12, true, 75);
    ok &= MovedImpulses(144, 0, true, 76);
    ok &= AvxMatchesScalar();
    ok &= EvenWork();
    std::printf(ok ? "all release convolver tests passed\n" : "release convolver tests FAILED\n");
    return ok ? 0 : 1;
}
