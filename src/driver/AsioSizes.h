// Legal ASIO buffer sizes from a driver's getBufferSize() answer.
#ifndef SVMS_ASIO_SIZES_H
#define SVMS_ASIO_SIZES_H

#include <cstdint>
#include <vector>

namespace svms {

// granularity -1: min, the powers of two above it, then max.
// granularity 0: min, plus max when it differs (fixed or two-size drivers).
// granularity > 0: min, min + g, ... up to max.
inline std::vector<uint32_t> AsioLegalSizes(long minSize, long maxSize,
                                            long preferred, long granularity) {
    (void)preferred;
    std::vector<uint32_t> out;
    if (minSize <= 0 || maxSize < minSize) return out;
    out.push_back(static_cast<uint32_t>(minSize));
    if (granularity < 0) {
        for (long s = 1; s <= maxSize && s > 0; s <<= 1)
            if (s > minSize) out.push_back(static_cast<uint32_t>(s));
    } else if (granularity > 0) {
        for (long s = minSize + granularity; s <= maxSize; s += granularity)
            out.push_back(static_cast<uint32_t>(s));
    }
    if (out.back() != static_cast<uint32_t>(maxSize))
        out.push_back(static_cast<uint32_t>(maxSize));
    return out;
}

// Nearest legal size; a tie goes to the larger one. Empty list: unchanged.
inline uint32_t AsioNearestLegalSize(const std::vector<uint32_t>& legal,
                                     uint32_t requested) {
    uint32_t best = requested;
    uint64_t bestDist = UINT64_MAX;
    for (uint32_t s : legal) {
        const uint64_t d = s > requested ? s - requested : requested - s;
        if (d < bestDist || (d == bestDist && s > best)) {
            best = s;
            bestDist = d;
        }
    }
    return best;
}

} // namespace svms

#endif // SVMS_ASIO_SIZES_H
