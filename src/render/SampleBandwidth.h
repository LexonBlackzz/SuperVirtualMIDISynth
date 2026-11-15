#pragma once
// Load-time bandwidth measurement for the half-rate voice bus: how much of a
// sample's own Nyquist range holds its energy, as a 1..255 code (0 = unknown).

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace svms {

// Smallest frequency (fraction of the sample's Nyquist, rounded up to 1/255)
// above which less than `tailLimit` of the averaged power spectrum remains.
inline uint8_t MeasureSampleBandCode(const int16_t* x, uint32_t len,
                                     double tailLimit = 1.0e-5) {
    constexpr int N = 2048;
    constexpr int B = N / 2;
    if (x == nullptr || len == 0u) return 0u;
    typedef std::complex<double> cd;
    const double pi = 3.14159265358979323846;
    std::vector<cd> buf(N);
    std::vector<double> acc(B, 0.0);
    const uint32_t span = len > static_cast<uint32_t>(N) ? len - N : 0u;
    const int want = len < static_cast<uint32_t>(N)
        ? 1 : static_cast<int>((std::min)(32u, span / (N / 2) + 1u));
    for (int w = 0; w < want; ++w) {
        const uint32_t off = want > 1
            ? static_cast<uint32_t>(static_cast<uint64_t>(span) * w / (want - 1))
            : 0u;
        for (int i = 0; i < N; ++i) {
            const double v = off + i < len ? x[off + i] / 32768.0 : 0.0;
            buf[i] = cd(v * (0.5 - 0.5 * std::cos(2.0 * pi * i / (N - 1))), 0.0);
        }
        for (size_t i = 1, j = 0; i < N; ++i) {
            size_t bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(buf[i], buf[j]);
        }
        for (size_t n = 2; n <= N; n <<= 1) {
            const double ang = -2.0 * pi / static_cast<double>(n);
            const cd wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < N; i += n) {
                cd t(1.0);
                for (size_t k = 0; k < n / 2; ++k) {
                    const cd u = buf[i + k], q = buf[i + k + n / 2] * t;
                    buf[i + k] = u + q;
                    buf[i + k + n / 2] = u - q;
                    t *= wl;
                }
            }
        }
        for (int k = 0; k < B; ++k) acc[k] += std::norm(buf[k]);
    }
    double total = 0.0;
    for (double e : acc) total += e;
    if (total <= 0.0) return 0u;
    double tail = 0.0;
    int bin = B;
    while (bin > 0 && tail + acc[bin - 1] / total < tailLimit) {
        --bin;
        tail += acc[bin] / total;
    }
    const int code = static_cast<int>(std::ceil(255.0 * bin / B));
    return static_cast<uint8_t>(code < 1 ? 1 : (code > 255 ? 255 : code));
}

}  // namespace svms
