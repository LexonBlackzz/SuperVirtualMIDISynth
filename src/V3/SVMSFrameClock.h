#ifndef SVMS_FRAME_CLOCK_H
#define SVMS_FRAME_CLOCK_H

#include <cstdint>

namespace svms {

// Converts against one fixed QPC/output-frame epoch. Splitting whole seconds
// from the remainder avoids overflow and prevents callback-by-callback
// rounding from accumulating into clock drift.
inline int64_t QpcDeltaToFrames(int64_t deltaQpc, int64_t qpcFrequency,
                                uint32_t sampleRate) noexcept {
    if (qpcFrequency <= 0 || sampleRate == 0) return 0;
    const int64_t wholeSeconds = deltaQpc / qpcFrequency;
    const int64_t remainder = deltaQpc % qpcFrequency;
    const int64_t roundedRemainder = remainder >= 0
        ? (remainder * sampleRate + qpcFrequency / 2) / qpcFrequency
        : (remainder * sampleRate - qpcFrequency / 2) / qpcFrequency;
    return wholeSeconds * static_cast<int64_t>(sampleRate) + roundedRemainder;
}

} // namespace svms

#endif
