// CompileDirectRun must give, event for event, what the ring path's
// CompileTimestampedEvent gives, and report the same termination fences.
// Also times it against the reference (the compile step only).
#include "../SVMSDirectIngest.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace svms;

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)

int main() {
    const DirectCompileContext ctx{1000000u, 10000000, 48000u, 1024u};
    std::mt19937_64 rng(12345);
    const uint64_t immediate = 1000000u + 777u;

    for (int round = 0; round < 200; ++round) {
        const uint32_t n = 1u + static_cast<uint32_t>(rng() % 3000u);
        std::vector<SVMS_ShortEvent> events(n);
        uint64_t t = ctx.epochQpc + rng() % 5000u;
        for (auto& e : events) {
            if (rng() % 3u == 0u) t += rng() % 4000u;           // repeats share a timestamp
            e.timestamp_qpc = rng() % 40u == 0u ? 0u : t;        // some "at submission"
            uint32_t pick = static_cast<uint32_t>(rng() % 100u);
            uint32_t status;
            if (pick < 70u) status = 0x90u | (rng() % 16u);
            else if (pick < 85u) status = 0x80u | (rng() % 16u);
            else if (pick < 92u) status = 0xB0u | (rng() % 16u);
            else if (pick < 96u) status = 0xC0u | (rng() % 16u);
            else if (pick < 98u) status = 0xE0u | (rng() % 16u);
            else status = static_cast<uint32_t>(rng() % 256u);   // anything, incl. 0xF0+
            uint32_t message = status | static_cast<uint32_t>(rng() % 256u) << 8u |
                               static_cast<uint32_t>(rng() % 256u) << 16u;
            const uint32_t odd = static_cast<uint32_t>(rng() % 50u);
            if (odd == 0u) message |= 0x01000000u;                       // re-excite tag
            else if (odd == 1u) message = kInternalResetMessage;
            else if (odd == 2u) message = MakeInternalMasterVolumeMessage(static_cast<uint16_t>(rng()));
            else if (odd == 3u) message = (0xB0u | (rng() % 16u)) | (120u + (rng() % 4u == 0u ? 3u : 0u)) << 8u;
            e.packed_message = message;
            e.reserved = 0u;
        }
        const uint32_t seqBase = static_cast<uint32_t>(rng());

        std::vector<ScheduledRenderEvent> got(n), want;
        struct Fence { bool global; uint8_t channel; uint32_t sequence; };
        std::vector<Fence> gotFences, wantFences;
        const uint32_t shedBelow = rng() % 3u == 0u ? 0u : 1u + static_cast<uint32_t>(rng() % 100u);
        std::vector<uint8_t> gotShed, wantShed;
        const uint32_t written = CompileDirectRun(
            events.data(), n, immediate, seqBase, ctx, got.data(),
            [&](bool g, uint8_t c, uint32_t s) { gotFences.push_back({g, c, s}); },
            shedBelow, [&](uint8_t v) { gotShed.push_back(v); });

        for (uint32_t i = 0; i < n; ++i) {
            TimestampedMidiEvent timed{};
            timed.message = events[i].packed_message;
            timed.qpcTimestamp = events[i].timestamp_qpc ? events[i].timestamp_qpc : immediate;
            timed.sequence = seqBase + i;
            ScheduledRenderEvent s{};
            if (!CompileTimestampedEvent(timed, ctx.epochQpc, ctx.qpcFrequency,
                                         ctx.sampleRate, ctx.leadFrames, s)) continue;
            const uint8_t status = static_cast<uint8_t>(timed.message & 0xffu);
            if (s.type == RenderEventType::NoteOn && (timed.message & 0xff000000u) == 0u &&
                (status & 0xe0u) == 0x80u && s.data2 < shedBelow) {
                wantShed.push_back(s.data2);
                continue;
            }
            want.push_back(s);
            if (timed.message == kInternalResetMessage)
                wantFences.push_back({true, 0u, timed.sequence});
            else if ((status & 0xf0u) == 0xb0u) {
                const uint8_t c = static_cast<uint8_t>((timed.message >> 8u) & 0x7fu);
                if (c == 120u || c == 123u)
                    wantFences.push_back({false, static_cast<uint8_t>(status & 0x0f), timed.sequence});
            }
        }
        CHECK(written == want.size());
        for (uint32_t i = 0; i < written && i < want.size(); ++i) {
            CHECK(got[i].targetFrame == want[i].targetFrame);
            CHECK(got[i].sequence == want[i].sequence);
            CHECK(got[i].type == want[i].type);
            CHECK(got[i].channel == want[i].channel);
            CHECK(got[i].data1 == want[i].data1);
            CHECK(got[i].data2 == want[i].data2);
            if (g_failures > 20) return 1;
        }
        CHECK(gotShed == wantShed);
        CHECK(gotFences.size() == wantFences.size());
        for (size_t i = 0; i < gotFences.size() && i < wantFences.size(); ++i) {
            CHECK(gotFences[i].global == wantFences[i].global);
            CHECK(gotFences[i].channel == wantFences[i].channel);
            CHECK(gotFences[i].sequence == wantFences[i].sequence);
        }
    }

    // Timing: a dense plain-note batch, 16 events per timestamp.
    {
        const uint32_t n = 8192u;
        std::vector<SVMS_ShortEvent> events(n);
        for (uint32_t i = 0; i < n; ++i) {
            events[i].timestamp_qpc = ctx.epochQpc + 5000u + (i / 16u) * 7u;
            events[i].packed_message = 0x90u | (i & 15u) | (static_cast<uint32_t>(40u + i % 60u) << 8u) | (100u << 16u);
            events[i].reserved = 0u;
        }
        std::vector<ScheduledRenderEvent> out(n);
        auto bench = [&](auto&& fn) {
            double best = 1e9;
            for (int r = 0; r < 200; ++r) {
                const auto a = std::chrono::steady_clock::now();
                fn();
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
                if (s < best) best = s;
            }
            return best / n * 1e9;
        };
        volatile uint32_t sink = 0;
        const double direct = bench([&] {
            sink = CompileDirectRun(events.data(), n, 0u, 0u, ctx, out.data(), [](bool, uint8_t, uint32_t) {}, 0u, [](uint8_t) {});
        });
        const double reference = bench([&] {
            uint32_t w = 0;
            for (uint32_t i = 0; i < n; ++i) {
                TimestampedMidiEvent timed{events[i].timestamp_qpc, events[i].packed_message, i};
                ScheduledRenderEvent s{};
                if (CompileTimestampedEvent(timed, ctx.epochQpc, ctx.qpcFrequency, ctx.sampleRate, ctx.leadFrames, s))
                    out[w++] = s;
            }
            sink = w;
        });
        std::printf("compile ns/event: direct %.2f, reference %.2f\n", direct, reference);
    }

    std::printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
    return g_failures ? 1 : 0;
}
