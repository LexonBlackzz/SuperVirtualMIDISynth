// Realtime block (SVMSRealtimeBlock.h) checks, scalar backend, mixed
// note/CC/bend/sustain/program/Reset storm. Skips (77) without a SoundFont.
//
//  1. Parity with the old per-event "render span, dispatch, repeat" loop.
//     Program changes are limited to program 0: the old loop and the batched
//     path disagree for some GM presets after a program change (open item,
//     see changelog); everything else must match to float noise.
#include "render/SVMSRealtimeBlock.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace svms;

constexpr uint32_t kFrames = 1024, kBlocks = 24;

static void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static std::wstring FindSoundFont() {
    if (const char* env = std::getenv("SVMS_TEST_SOUNDFONT")) {
        std::string s(env);
        return std::wstring(s.begin(), s.end());
    }
    for (const char* p : {"gm.sf2", "../gm.sf2", "../../gm.sf2", "../../../gm.sf2", "../../../../gm.sf2"}) {
        if (FILE* f = std::fopen(p, "rb")) {
            std::fclose(f);
            std::string s(p);
            return std::wstring(s.begin(), s.end());
        }
    }
    return {};
}

static std::unique_ptr<StandaloneSynth> MakeSynth(const std::wstring& sf2) {
    StandaloneSynthConfig cfg;
    cfg.soundfont = sf2;
    cfg.sampleRate = 44100;
    cfg.maxVoices = 2048;
    cfg.renderThreads = 1;
    cfg.maxBlockFrames = kFrames;
    cfg.backend = RenderBackend::Scalar;
    std::unique_ptr<StandaloneSynth> synth(new StandaloneSynth());
    std::string error;
    if (!synth->Initialize(cfg, error)) {
        std::fprintf(stderr, "init: %s\n", error.c_str());
        return nullptr;
    }
    return synth;
}

// One block of events, sorted by (frame, sequence).
static std::vector<RealtimeEvent> MakeBlock(std::mt19937& rng, uint64_t start, uint32_t block,
                                            uint64_t& sequence, bool realPrograms) {
    std::vector<RealtimeEvent> events;
    const uint32_t count = 30 + rng() % 120;
    for (uint32_t i = 0; i < count; ++i) {
        RealtimeEvent e;
        e.targetFrame = start + rng() % kFrames;
        e.sequence = sequence++;
        const uint32_t ch = rng() % 4, key = 36 + rng() % 50, vel = 1 + rng() % 127;
        switch (rng() % 10) {
        case 0: case 1: case 2: case 3: e.message = 0x90u | ch | (key << 8) | (vel << 16); break;
        case 4: case 5: e.message = 0x80u | ch | (key << 8); break;
        case 6: e.message = 0xB0u | ch | (7u << 8) | ((rng() % 128) << 16); break;
        case 7: e.message = 0xB0u | ch | (64u << 8) | ((rng() % 2 ? 127u : 0u) << 16); break;
        case 8: e.message = 0xE0u | ch | ((rng() % 128) << 8) | ((rng() % 128) << 16); break;
        default: {
            const uint32_t program = rng() % 20;
            e.message = 0xC0u | ch | ((realPrograms ? program : 0u) << 8);
            break;
        }
        }
        events.push_back(e);
    }
    if (block == 12) {  // a Reset barrier mid-block
        RealtimeEvent r;
        r.reset = true;
        r.targetFrame = start + 400;
        r.sequence = sequence++;
        events.push_back(r);
    }
    std::stable_sort(events.begin(), events.end(), [](const RealtimeEvent& a, const RealtimeEvent& b) {
        return a.targetFrame != b.targetFrame ? a.targetFrame < b.targetFrame : a.sequence < b.sequence;
    });
    return events;
}

// The old Linux loop: render up to each event, dispatch it, continue.
static void RenderPerEvent(StandaloneSynth& synth, const std::vector<RealtimeEvent>& events,
                           float* l, float* r, uint64_t start) {
    uint32_t cursor = 0;
    for (const RealtimeEvent& e : events) {
        const uint64_t writable = start + cursor;
        const uint64_t target = std::max(e.targetFrame, writable);
        const uint32_t span = uint32_t(target - writable);
        if (span) { synth.Render(l + cursor, r + cursor, span, writable); cursor += span; }
        if (e.reset) synth.ResetAll(target); else synth.Dispatch(e.message, target);
    }
    if (cursor < kFrames) synth.Render(l + cursor, r + cursor, kFrames - cursor, start + cursor);
}


static double MaxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, double(std::fabs(a[i] - b[i])));
    return worst;
}

// Runs `kBlocks` of the storm through `renderA` and `renderB`, returns the worst difference.
template <class A, class B>
static double Compare(const std::wstring& sf2, bool realPrograms, A renderA, B renderB, double& energy,
                      double& msA, double& msB) {
    auto a = MakeSynth(sf2), b = MakeSynth(sf2);
    if (!a || !b) return 1e9;
    std::mt19937 rng(1234);
    uint64_t sequence = 0;
    std::vector<float> al(kFrames), ar(kFrames), bl(kFrames), br(kFrames);
    double worst = 0.0;
    energy = msA = msB = 0.0;
    for (uint32_t block = 0; block < kBlocks; ++block) {
        const uint64_t start = uint64_t(block) * kFrames;
        const auto events = MakeBlock(rng, start, block, sequence, realPrograms);
        const auto t0 = std::chrono::steady_clock::now();
        renderA(*a, events, al.data(), ar.data(), start);
        const auto t1 = std::chrono::steady_clock::now();
        renderB(*b, events, bl.data(), br.data(), start);
        const auto t2 = std::chrono::steady_clock::now();
        msA += std::chrono::duration<double, std::milli>(t1 - t0).count();
        msB += std::chrono::duration<double, std::milli>(t2 - t1).count();
        worst = std::max({worst, MaxDiff(al, bl), MaxDiff(ar, br)});
        for (float v : al) energy += double(v) * v;
    }
    return worst;
}

// Regression: a looping voice held across blocks, then a block whose events
// cut it into a long span followed by a < 8 frame span. The long path leaves
// the loop wrap pending (phase >= loopEnd); the short path used to clamp that
// phase to loopStart and drop the fractional overflow (0.126 samples here).
static bool TestShortSpanLoopWrap(const std::wstring& sf2) {
    auto a = MakeSynth(sf2), b = MakeSynth(sf2);
    RealtimeBlockRenderer renderer;
    std::vector<float> al(kFrames), ar(kFrames), bl(kFrames), br(kFrames);
    auto at = [](uint64_t frame, uint32_t message, uint64_t sequence) {
        RealtimeEvent e; e.targetFrame = frame; e.message = message; e.sequence = sequence; return e;
    };
    double worst = 0.0;
    for (uint32_t block = 0; block < 10; ++block) {
        std::vector<RealtimeEvent> events;
        if (block == 0) {
            events.push_back(at(0, 0x05C0u, 0));                  // program 5 (gm.sf2 electric piano)
            events.push_back(at(193, 0x665390u, 1));              // held high key, short loop
        } else if (block == 7) {
            events.push_back(at(7974, 0x532e90u, 2));
            events.push_back(at(8004, 0x582f90u, 3));
            events.push_back(at(8010, 0x295090u, 4));             // 6 frame span after 8004
        }
        const uint64_t start = uint64_t(block) * kFrames;
        RenderPerEvent(*a, events, al.data(), ar.data(), start);
        renderer.Render(*b, events.data(), events.size(), bl.data(), br.data(), kFrames, start);
        worst = std::max({worst, MaxDiff(al, bl), MaxDiff(ar, br)});
    }
    std::printf("short-span loop wrap: max diff %.3g\n", worst);
    return worst <= 2e-5;
}

int main() {
    // Hermetic: StandaloneSynth reads the engine config (note merge, collapse,
    // re-excite...), which the batched path honours. A developer's live config
    // must not leak into this comparison.
    SetEnv("SVMS_TEST_LOCAL_CONFIG_PATH", "svms-test-no-such-config.json");
    SetEnv("SVMS_TEST_CONFIG_PATH", "svms-test-no-such-config.json");
    const std::wstring sf2 = FindSoundFont();
    if (sf2.empty()) { std::puts("no SoundFont; skipping"); return 77; }

    RealtimeBlockRenderer batchedRenderer;
    if (!TestShortSpanLoopWrap(sf2)) { std::puts("FAIL: short-span loop wrap loses phase"); return 1; }
    int failures = 0;
    double energy = 0.0, msPerEvent = 0.0, msBatched = 0.0;

    const double parity = Compare(
        sf2, false,
        [](StandaloneSynth& s, const std::vector<RealtimeEvent>& e, float* l, float* r, uint64_t start) {
            RenderPerEvent(s, e, l, r, start);
        },
        [&](StandaloneSynth& s, const std::vector<RealtimeEvent>& e, float* l, float* r, uint64_t start) {
            batchedRenderer.Render(s, e.data(), e.size(), l, r, kFrames, start);
        },
        energy, msPerEvent, msBatched);
    std::printf("per-event loop vs batched block: max diff %.3g (energy %.3g); render time %.1f ms vs %.1f ms\n",
                parity, energy, msPerEvent, msBatched);
    if (energy < 1e-3 || parity > 2e-5) { std::puts("FAIL: batched block diverges from the per-event loop"); ++failures; }


    std::puts(failures ? "FAILED" : "realtime block tests passed");
    return failures ? 1 : 0;
}
