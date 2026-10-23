#ifndef SVMS_OVERSAMPLE_H
#define SVMS_OVERSAMPLE_H

#include "synth/SoundFont.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

namespace svms {

// Experimental sample-store oversampling: every sample is upsampled by `factor`
// with a 16-point windowed sinc at load time, and every position and sample rate
// in the (already built) SF2Data is scaled to match. Playback keeps its plain
// linear interpolator, but over data that is `factor` times denser, so the
// interpolation error drops by roughly factor^2. Phase 0 of every output group
// is the original sample, untouched. Call after sf2_build_regions.
inline bool OversampleSF2(SF2Data& d, uint32_t factor, uint32_t threads) {
    if (factor < 2u || !d.sampleData || d.sampleDataFrames == 0u) return true;
    constexpr int kHalf = 8;  // taps on each side of the interpolation point
    const uint64_t frames = d.sampleDataFrames;
    const uint64_t outFrames = frames * factor;
    if (outFrames > 0x7fffff00ull) return false;
    int16_t* out = static_cast<int16_t*>(std::malloc((outFrames + 8u) * sizeof(int16_t)));
    if (!out) return false;
    std::fill(out + outFrames, out + outFrames + 8u, int16_t{0});

    // kernel[p][t]: weight of x[n + t - (kHalf - 1)] for phase p/factor.
    const double pi = 3.14159265358979323846;
    std::vector<float> kernel(size_t(factor) * 2u * kHalf);
    for (uint32_t p = 1; p < factor; ++p) {
        const double frac = double(p) / factor;
        double sum = 0.0;
        for (int t = 0; t < 2 * kHalf; ++t) {
            const double x = (t - (kHalf - 1)) - frac;  // distance to the point
            const double sinc = std::sin(pi * x) / (pi * x);
            const double w = 0.5 + 0.5 * std::cos(pi * x / kHalf);  // Hann
            kernel[size_t(p) * 2u * kHalf + t] = static_cast<float>(sinc * w);
            sum += sinc * w;
        }
        for (int t = 0; t < 2 * kHalf; ++t)
            kernel[size_t(p) * 2u * kHalf + t] /= static_cast<float>(sum);  // unity DC
    }

    const int16_t* in = d.sampleData;
    auto work = [&](uint64_t from, uint64_t to) {
        for (uint64_t n = from; n < to; ++n) {
            out[n * factor] = in[n];
            for (uint32_t p = 1; p < factor; ++p) {
                const float* k = &kernel[size_t(p) * 2u * kHalf];
                float acc = 0.0f;
                for (int t = 0; t < 2 * kHalf; ++t) {
                    const int64_t i = int64_t(n) + t - (kHalf - 1);
                    if (i >= 0 && i < int64_t(frames)) acc += k[t] * float(in[i]);
                }
                acc = std::nearbyint(acc);
                out[n * factor + p] = static_cast<int16_t>(
                    acc > 32767.0f ? 32767.0f : (acc < -32768.0f ? -32768.0f : acc));
            }
        }
    };
    const uint32_t n = (std::max)(1u, threads);
    std::vector<std::thread> pool;
    const uint64_t chunk = (frames + n - 1u) / n;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t a = i * chunk, b = (std::min)(frames, a + chunk);
        if (a < b) pool.emplace_back(work, a, b);
    }
    for (auto& t : pool) t.join();

    std::free(d.sampleData);
    d.sampleData = out;
    d.sampleDataFrames = static_cast<uint32_t>(outFrames);
    d.sampleDataSize = d.sampleDataSize ? d.sampleDataSize * factor : 0u;
    for (uint32_t i = 0; i < d.sampleCount; ++i) {
        SF2Sample& s = d.samples[i];
        s.start *= factor; s.end *= factor;
        s.loopStart *= factor; s.loopEnd *= factor;
        s.sampleDataOffset *= factor;
        if (s.sampleRate) s.sampleRate *= factor;
    }
    for (uint32_t i = 0; i < d.regionCount; ++i) {
        SFSampleRegion& r = d.regions[i];
        r.startOffset *= factor; r.endOffset *= factor;
        r.loopStartOffset *= factor; r.loopEndOffset *= factor;
    }
    return true;
}

} // namespace svms

#endif
