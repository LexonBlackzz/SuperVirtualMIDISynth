#pragma once
// Half-rate voice bus.
//
// A sustained looping voice whose output holds nothing above ~0.38 of the
// output Nyquist is rendered at half the output rate and reconstructed by a
// 2x interpolator.  The bus is stateless across blocks: a block renders the
// half-rate samples of times [-2*kLead, n + 2*kLead) output frames (the voice's
// cursor is backed up and run forward from there), so a voice can join or
// leave the bus at any block start with no crossfade and no filter history.
// Buffer index j holds the output time 2*(j - kLead).

#include <cmath>
#include <cstdint>

namespace svms {
namespace halfrate {

constexpr uint32_t kLead = 16;          // half-frames before and after the block
constexpr uint32_t kMinFrames = 256;    // shorter stretches stay at full rate
// Ceiling for (sample bandwidth / sample Nyquist) * step: the content reaching
// the output must sit this far below the output Nyquist.  The interpolator
// below holds ~1e-5 error up to 0.19 cycles per frame.
constexpr float kBandLimit = 0.38f;

inline uint32_t BusFrames(uint32_t n) { return (n + 1u) / 2u + 2u * kLead; }

// 2*kLead-tap windowed-sinc half-sample interpolator (Kaiser, beta 9), unity DC.
inline const float* Taps() {
    static float taps[2 * kLead];
    static bool ready = false;
    if (!ready) {
        auto bessel0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        const double beta = 9.0;
        const double pi = 3.14159265358979323846;
        double total = 0.0;
        double raw[2 * kLead];
        for (uint32_t k = 0; k < 2 * kLead; ++k) {
            const double x = static_cast<double>(k) -
                             (static_cast<double>(kLead) - 1.0) - 0.5;
            const double s = std::sin(pi * x) / (pi * x);
            const double r = 2.0 * (k + 1.0) / (2.0 * kLead + 1.0) - 1.0;
            raw[k] = s * bessel0(beta * std::sqrt(1.0 - r * r)) / bessel0(beta);
            total += raw[k];
        }
        for (uint32_t k = 0; k < 2 * kLead; ++k)
            taps[k] = static_cast<float>(raw[k] / total);
        ready = true;
    }
    return taps;
}

// out[0..n) += reconstruction of the half-rate bus (see the header comment).
// `accL`/`accR` are scratch of at least n/2 floats.  The tap loop runs outside
// and the sample loop inside, so the compiler vectorizes it without reordering
// any single output's sum.
inline void UpsampleAdd(const float* __restrict halfL,
                        const float* __restrict halfR, float* outL,
                        float* outR, uint32_t n, float* __restrict accL,
                        float* __restrict accR) {
    const float* h = Taps();
    const uint32_t pairs = n / 2u;
    for (uint32_t m = 0; m < pairs; ++m) {
        accL[m] = 0.0f;
        accR[m] = 0.0f;
    }
    for (uint32_t k = 0; k < 2 * kLead; ++k) {
        const float tap = h[k];
        const float* xl = halfL + 1u + k;
        const float* xr = halfR + 1u + k;
        for (uint32_t m = 0; m < pairs; ++m) {
            accL[m] += tap * xl[m];
            accR[m] += tap * xr[m];
        }
    }
    for (uint32_t m = 0; m < pairs; ++m) {
        outL[2 * m] += halfL[m + kLead];
        outR[2 * m] += halfR[m + kLead];
        outL[2 * m + 1] += accL[m];
        outR[2 * m + 1] += accR[m];
    }
    if (n & 1u) {
        outL[n - 1] += halfL[pairs + kLead];
        outR[n - 1] += halfR[pairs + kLead];
    }
}

}  // namespace halfrate
}  // namespace svms
