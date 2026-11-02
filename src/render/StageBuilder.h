#pragma once

// Control-thread side of the 3D stage: from a room, sources and a listener to
// an immutable RoomResponse. Slow work (tracing, FFTs of the responses) happens
// here; the audio thread only ever reads the finished snapshot.

#include "render/StageRenderer.h"

namespace svms {
namespace stage {

struct StageScene {
    Room room;
    Vec3 listener{12.0f, 24.0f, 1.2f};
    float yawDegrees = 0.0f;
    Vec3 source[kMaxSources];
    bool sourceOn[kMaxSources] = {};
};

struct BuildOptions {
    float sampleRate = 48000.0f;
    int partition = 512;
    int rays = 40000;
    bool air = true;
    bool earModel = true;
    int order = 3;                // wall reflection order for early paths (1..3)
    int liveTaps = kMaxLiveTaps;  // strongest wall paths that glide when a source moves
    float earlySeconds = 0.42f;
    float tailTrim = 1.0f;        // tail level against the critical-distance balance
    uint32_t seed = 4242;
    const std::atomic<bool>* cancel = nullptr;   // set to stop a long trace early
};

// Tail impulse response: noise whose band envelopes are the traced
// energy-time curves, so decay, band balance and onset come from the room.
// Sparse impulses at the room's echo density, split into the four bands.
inline void SynthesizeTail(const TailResult& tail, float volume, float sr,
                           std::vector<float> out[2]) {
    float tmax = 0.0f;
    for (float t : tail.t60) tmax = (std::max)(tmax, t);
    const float Tm = (std::max)(0.12f, (std::min)(12.0f, tmax));
    const int len = static_cast<int>((Tm * 1.35f + 0.15f) * sr);
    const int tailFrom = len - static_cast<int>(0.22f * static_cast<float>(len));
    const float splits[3] = {250.0f, 1000.0f, 4000.0f};
    float al[3];
    for (int i = 0; i < 3; ++i) al[i] = 1.0f - std::exp(-6.283185307179586f * splits[i] / sr);
    const float c3 = 4.0f * 3.14159265f * kSpeedOfSound * kSpeedOfSound * kSpeedOfSound;
    const float V = (std::max)(30.0f, volume);
    for (int ch = 0; ch < 2; ++ch) {
        out[ch].assign(static_cast<size_t>(len), 0.0f);
        detail::Rng rng(9100u + static_cast<uint32_t>(ch));
        float l0 = 0.0f, l1 = 0.0f, l2 = 0.0f;
        double energy = 0.0;
        for (int i = 0; i < len; ++i) {
            const float t = static_cast<float>(i) / sr, tt = t + 0.005f;
            const float p = (std::max)(0.14f, (std::min)(0.95f, c3 * tt * tt / V / sr));
            float x = 0.0f;
            if (rng.Next() < p) x = (rng.Next() < 0.5f ? -1.0f : 1.0f) / std::sqrt(p);
            l0 += al[0] * (x - l0);
            l1 += al[1] * (x - l1);
            l2 += al[2] * (x - l2);
            const float f = t / tail.dt;
            const int k = (std::max)(0, (std::min)(tail.bins - 2, static_cast<int>(f)));
            const float fr = (std::min)(1.0f, f - static_cast<float>(k));
            const float bands[kBands] = {l0, l1 - l0, l2 - l1, x - l2};
            float v = 0.0f;
            for (int b = 0; b < kBands; ++b) {
                const std::vector<float>& e = tail.envelope[b];
                v += bands[b] * (e[static_cast<size_t>(k)] + (e[static_cast<size_t>(k) + 1] - e[static_cast<size_t>(k)]) * fr);
            }
            v *= 1.0f - std::exp(-t / 0.004f);
            if (i > tailFrom)
                v *= 0.5f * (1.0f + std::cos(3.14159265f * static_cast<float>(i - tailFrom) / static_cast<float>(len - tailFrom)));
            out[ch][static_cast<size_t>(i)] = v;
            energy += static_cast<double>(v) * v;
        }
        const float k = 1.0f / static_cast<float>(std::sqrt(energy > 0.0 ? energy : 1.0));
        for (float& s : out[ch]) s *= k;
    }
}

// Traces the scene and builds the shared tail. `meta` receives decay times and
// flags. The tail is scaled so that, at the critical distance, the reverberant
// field is as loud as the direct sound.
inline std::shared_ptr<const IrSpectra> BuildTailSpectra(const FftPlan& plan, const StageScene& scene,
                                                         TraceBackend& backend, const BuildOptions& opt,
                                                         RoomMeta* meta) {
    std::vector<Vec3> sources;
    for (int i = 0; i < kMaxSources; ++i) if (scene.sourceOn[i]) sources.push_back(scene.source[i]);
    if (sources.empty()) return nullptr;
    const TraceScene geometry = BuildBoxScene(scene.room);
    const float volume = RoomVolume(scene.room);
    TraceOptions t;
    t.rays = opt.rays;
    t.radius = (std::max)(0.4f, (std::min)(3.0f, 0.12f * std::cbrt((std::max)(30.0f, volume))));
    t.skipOrder = opt.order;
    t.seed = opt.seed;
    const EnergyHistogram hist = backend.Run(geometry, sources, scene.listener, t, opt.cancel);
    if (hist.cancelled) return nullptr;
    float absorption[kBands];
    TotalAbsorption(scene.room, absorption);
    const TailResult tail = BuildTail(hist, opt.air, volume, absorption, SabineLowBand{});
    std::vector<float> ir[2];
    SynthesizeTail(tail, volume, opt.sampleRate, ir);
    float meanT60 = 0.0f;
    for (float v : tail.t60) meanT60 += 0.25f * v;
    const float gain = opt.tailTrim / CriticalDistance(scene.room, meanT60);
    // The convolver delays its output by one partition; start the response that much later.
    const int skip = (std::min)(plan.n / 2, static_cast<int>(ir[0].size()));
    for (auto& c : ir) {
        c.erase(c.begin(), c.begin() + skip);
        for (float& s : c) s *= gain;
    }
    if (meta) {
        for (int b = 0; b < kBands; ++b) { meta->t60[b] = tail.t60[b]; meta->waveLimited[b] = tail.waveLimited[b]; }
        meta->schroederHz = tail.schroederHz;
        meta->rays = hist.rays;
        meta->tailSeconds = static_cast<float>(ir[0].size()) / opt.sampleRate;
    }
    return MakeSpectra(plan, ir[0].data(), ir[1].data(), static_cast<int>(ir[0].size()));
}

inline void BandGainWithAir(const EarlyPath& e, bool air, float out[kBands]) {
    const float amp = kEarlyLevel / (std::max)(1.0f, e.path);
    for (int b = 0; b < kBands; ++b)
        out[b] = e.gain[b] * amp * (air ? DbToAmplitude(AirDbPerMetre()[b] * e.path) : 1.0f);
}

// Direct paths, live taps and static early responses. Cheap enough to redo
// whenever a source or the listener moves; the tail is passed in.
inline std::shared_ptr<RoomResponse> BuildPaths(const StageScene& scene, const Spatializer& spatial,
                                                const BuildOptions& opt, const FftPlan& plan,
                                                std::shared_ptr<const IrSpectra> tail,
                                                const RoomMeta& meta, uint64_t serial, bool cut) {
    auto r = std::make_shared<RoomResponse>();
    r->serial = serial;
    r->cut = cut;
    r->tail = std::move(tail);
    r->meta = meta;
    const int B = plan.n / 2;
    for (int i = 0; i < kMaxSources; ++i) {
        if (!scene.sourceOn[i]) continue;
        SourceResponse& s = r->source[i];
        s.active = true;
        const Vec3& pos = scene.source[i];
        const DirectPath d = SolveDirect(scene.room, pos, scene.listener, scene.yawDegrees, opt.air, false);
        s.direct.delaySeconds = d.delaySeconds;
        spatial.Filters(RelativeTo(pos, scene.listener, scene.yawDegrees), d.gain, s.direct.ears);

        std::vector<EarlyPath> all = FindReflections(scene.room, pos, scene.listener, opt.order);
        std::vector<EarlyPath> live, rest;
        for (const EarlyPath& e : all)
            (!e.furniture && e.order <= 2 ? live : rest).push_back(e);
        std::stable_sort(live.begin(), live.end(),
                         [](const EarlyPath& a, const EarlyPath& b) { return a.path < b.path; });
        const size_t keep = static_cast<size_t>((std::max)(0, (std::min)(opt.liveTaps, kMaxLiveTaps)));
        for (size_t k = 0; k < live.size(); ++k) {
            if (k < keep) {
                float g[kBands];
                BandGainWithAir(live[k], opt.air, g);
                LiveTap& t = s.taps[s.tapCount++];
                t.delaySeconds = live[k].path / kSpeedOfSound;
                spatial.Filters(RelativeTo(live[k].image, scene.listener, scene.yawDegrees), g, t.ears);
            } else {
                rest.push_back(live[k]);
            }
        }
        const EarlyResponse er = BuildEarlyResponse(rest, scene.listener, scene.yawDegrees, opt.earModel,
                                                    opt.air, opt.sampleRate, opt.earlySeconds);
        const int skip = (std::min)(B, static_cast<int>(er.left.size()));
        s.early = MakeSpectra(plan, er.left.data() + skip, er.right.data() + skip,
                              static_cast<int>(er.left.size()) - skip);
    }
    return r;
}

// Everything at once; a GPU backend only has to implement TraceBackend.
inline std::shared_ptr<RoomResponse> BuildRoomResponse(const StageScene& scene,
                                                       const Spatializer& spatial,
                                                       TraceBackend& backend, const BuildOptions& opt,
                                                       const FftPlan& plan, uint64_t serial) {
    RoomMeta meta;
    auto tail = BuildTailSpectra(plan, scene, backend, opt, &meta);
    return BuildPaths(scene, spatial, opt, plan, std::move(tail), meta, serial, true);
}

} // namespace stage
} // namespace svms
