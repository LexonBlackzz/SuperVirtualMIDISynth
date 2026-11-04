#pragma once

// Measured head for the 3D stage: one generic human head built from the 96
// subjects of the HUTUBS database (see render/data/README.md), applied to the
// direct sound. Reflections keep the plain ear model.
//
// The table (SVHR) holds, for 440 directions and both ears, a minimum-phase
// filter at 44.1 kHz and the interaural delay in samples. This file reads it,
// resamples every filter to the device rate once, and for a direction blends
// the four nearest measurements. Blending is valid because the filters are
// minimum phase and the delay is kept apart: neighbours differ smoothly and
// nothing combs.

#include "render/StageRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace svms {
namespace stage {

// Linked in from StageHrtfData.cpp (the table embedded as bytes).
const unsigned char* EmbeddedHeadTable(size_t& size) noexcept;

struct HeadTable {
    float sampleRate = 0.0f;
    int directions = 0, taps = 0;
    std::vector<float> azimuth, elevation;   // degrees; azimuth counts towards the left
    std::vector<float> delay;                // [direction][ear] samples, one delay per ear, ears 0 = left
    std::vector<float> ir;                   // [direction][ear][tap]

    // Little-endian: 'SVHR', u32 version (1), f32 rate, u32 directions, u32 taps,
    // then azimuth[D], elevation[D], delay[D][2], ir[D][2][taps] as f32.
    bool Parse(const unsigned char* data, size_t size) {
        if (!data || size < 20u || std::memcmp(data, "SVHR", 4) != 0) return false;
        uint32_t version, d, t;
        float rate;
        std::memcpy(&version, data + 4, 4);
        std::memcpy(&rate, data + 8, 4);
        std::memcpy(&d, data + 12, 4);
        std::memcpy(&t, data + 16, 4);
        if (version != 1u || !(rate >= 8000.0f && rate <= 400000.0f) || d == 0u || d > 100000u || t == 0u || t > 4096u)
            return false;
        const size_t floats = static_cast<size_t>(d) * (4u + static_cast<size_t>(t) * 2u);
        if (size != 20u + floats * 4u) return false;
        sampleRate = rate;
        directions = static_cast<int>(d);
        taps = static_cast<int>(t);
        const unsigned char* p = data + 20;
        auto take = [&](std::vector<float>& v, size_t n) {
            v.resize(n);
            std::memcpy(v.data(), p, n * 4u);
            p += n * 4u;
        };
        take(azimuth, d);
        take(elevation, d);
        take(delay, static_cast<size_t>(d) * 2u);
        take(ir, static_cast<size_t>(d) * 2u * t);
        return true;
    }
};

// A direct-sound filter pair for any direction, at the device rate.
class HrtfSpatializer final : public Spatializer {
public:
    // Loudness matches the plain ear model for a source straight ahead
    // (its pan law puts 0.707 on each ear).
    static constexpr float kGain = 0.7071067811865476f;

    bool Build(const HeadTable& t, float sampleRate) {
        ready_ = false;
        if (t.directions <= 0 || t.taps <= 0 || !(sampleRate >= 8000.0f)) return false;
        const int D = t.directions;
        const double ratio = static_cast<double>(t.sampleRate) / sampleRate;   // table samples per output sample
        const double rc = (std::min)(1.0, static_cast<double>(sampleRate) / t.sampleRate);   // low-pass when the device rate is lower
        const double half = 16.0 / rc;
        int outLen = static_cast<int>(std::ceil((t.taps - 1) / ratio)) + 1;
        outLen = (std::min)(outLen, kMaxFir);
        std::vector<float> full(static_cast<size_t>(D) * 2u * static_cast<size_t>(outLen), 0.0f);
        for (int de = 0; de < D * 2; ++de) {
            const float* h = &t.ir[static_cast<size_t>(de) * static_cast<size_t>(t.taps)];
            float* o = &full[static_cast<size_t>(de) * static_cast<size_t>(outLen)];
            for (int m = 0; m < outLen; ++m) {
                const double pos = m * ratio;
                const int lo = (std::max)(0, static_cast<int>(std::ceil(pos - half)));
                const int hi = (std::min)(t.taps - 1, static_cast<int>(std::floor(pos + half)));
                double acc = 0.0;
                for (int n = lo; n <= hi; ++n) {
                    const double x = pos - n;
                    const double a = 3.141592653589793 * rc * x;
                    const double sinc = std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a;
                    const double u = x / half;
                    const double win = 0.42 + 0.5 * std::cos(3.141592653589793 * u) + 0.08 * std::cos(6.283185307179586 * u);
                    acc += h[n] * rc * sinc * win;
                }
                o[m] = static_cast<float>(acc * kGain);
            }
        }
        // Shortest length that keeps every filter within -60 dB of its energy.
        len_ = 1;
        for (int de = 0; de < D * 2; ++de) {
            const float* o = &full[static_cast<size_t>(de) * static_cast<size_t>(outLen)];
            double total = 0.0;
            for (int m = 0; m < outLen; ++m) total += static_cast<double>(o[m]) * o[m];
            double tail = 0.0;
            int need = outLen;
            for (int m = outLen - 1; m > 0; --m) {
                tail += static_cast<double>(o[m]) * o[m];
                if (tail > total * 1e-6) break;
                need = m;
            }
            len_ = (std::max)(len_, need);
        }
        fir_.assign(static_cast<size_t>(D) * 2u * static_cast<size_t>(len_), 0.0f);
        for (int de = 0; de < D * 2; ++de)
            std::memcpy(&fir_[static_cast<size_t>(de) * static_cast<size_t>(len_)],
                        &full[static_cast<size_t>(de) * static_cast<size_t>(outLen)],
                        static_cast<size_t>(len_) * sizeof(float));
        // Delay about the head centre: only the difference between the ears is meaningful.
        delay_.assign(static_cast<size_t>(D) * 2u, 0.0f);
        dir_.assign(static_cast<size_t>(D) * 3u, 0.0f);
        for (int d = 0; d < D; ++d) {
            const float l = t.delay[static_cast<size_t>(d) * 2u], r = t.delay[static_cast<size_t>(d) * 2u + 1u];
            const float mid = 0.5f * (l + r);
            delay_[static_cast<size_t>(d) * 2u] = (l - mid) / t.sampleRate;
            delay_[static_cast<size_t>(d) * 2u + 1u] = (r - mid) / t.sampleRate;
            const float az = t.azimuth[static_cast<size_t>(d)] * 0.017453292519943295f;
            const float el = t.elevation[static_cast<size_t>(d)] * 0.017453292519943295f;
            dir_[static_cast<size_t>(d) * 3u] = std::cos(el) * std::cos(az);        // front
            dir_[static_cast<size_t>(d) * 3u + 1u] = std::cos(el) * std::sin(az);   // left
            dir_[static_cast<size_t>(d) * 3u + 2u] = std::sin(el);                  // up
        }
        dirs_ = D;
        ready_ = true;
        return true;
    }

    bool Ready() const noexcept { return ready_; }
    int FilterLength() const noexcept { return len_; }

    // Reflections and anything without a filter use the plain ear model.
    void Filters(const Relative& rel, const float bandGain[kBands], EarFilters& out) const noexcept override {
        ear_.Filters(rel, bandGain, out);
    }

    void DirectFilters(const Relative& rel, const float bandGain[kBands], EarFilters& out,
                       DirectFir& fir) const noexcept override {
        if (!ready_) {
            Filters(rel, bandGain, out);
            fir.length = 0;
            return;
        }
        const float r = (std::max)(0.1f, rel.distance);
        const float fx = rel.forward / r, fy = -rel.right / r, fz = rel.up / r;
        int best[4] = {-1, -1, -1, -1};
        float bestDot[4] = {-2.0f, -2.0f, -2.0f, -2.0f};
        for (int d = 0; d < dirs_; ++d) {
            const float dot = fx * dir_[static_cast<size_t>(d) * 3u] + fy * dir_[static_cast<size_t>(d) * 3u + 1u] +
                              fz * dir_[static_cast<size_t>(d) * 3u + 2u];
            int k = 4;
            while (k > 0 && dot > bestDot[k - 1]) --k;
            if (k >= 4) continue;
            for (int j = 3; j > k; --j) { bestDot[j] = bestDot[j - 1]; best[j] = best[j - 1]; }
            bestDot[k] = dot;
            best[k] = d;
        }
        float w[4], sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float ang = std::acos((std::max)(-1.0f, (std::min)(1.0f, bestDot[k])));
            w[k] = best[k] < 0 ? 0.0f : 1.0f / (ang * ang + 1e-5f);
            sum += w[k];
        }
        for (int k = 0; k < 4; ++k) w[k] /= sum;
        fir.length = len_;
        for (int e = 0; e < 2; ++e) {
            float* h = fir.h[e];
            std::fill(h, h + len_, 0.0f);
            float dl = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (best[k] < 0) continue;
                const float* src = &fir_[(static_cast<size_t>(best[k]) * 2u + static_cast<size_t>(e)) * static_cast<size_t>(len_)];
                for (int m = 0; m < len_; ++m) h[m] += w[k] * src[m];
                dl += w[k] * delay_[static_cast<size_t>(best[k]) * 2u + static_cast<size_t>(e)];
            }
            out.delay[e] = dl;
            for (int b = 0; b < kBands; ++b) out.band[e][b] = bandGain[b];   // the filter carries the direction
        }
    }

private:
    EarModelSpatializer ear_;
    bool ready_ = false;
    int dirs_ = 0, len_ = 1;
    std::vector<float> fir_, delay_, dir_;
};

// The embedded head at a device rate, or null when it cannot be read or SVMS_NO_HRTF is set
// (for comparing against the plain ear model).
inline std::unique_ptr<HrtfSpatializer> MakeHrtfSpatializer(float sampleRate) {
    if (const char* off = std::getenv("SVMS_NO_HRTF")) {
        if (*off && *off != '0') return nullptr;
    }
    size_t size = 0;
    const unsigned char* bytes = EmbeddedHeadTable(size);
    HeadTable table;
    if (!table.Parse(bytes, size)) return nullptr;
    auto head = std::make_unique<HrtfSpatializer>();
    if (!head->Build(table, sampleRate)) return nullptr;
    return head;
}

} // namespace stage
} // namespace svms
