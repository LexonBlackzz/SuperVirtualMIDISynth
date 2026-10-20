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

// A missed callback represents output frames the audio device has already
// lost. Leaving the render cursor behind wall time creates a MIDI backlog that
// can never catch up because the renderer cannot run faster than real time.
inline int64_t RecoverRealtimeRenderFrame(int64_t renderFrame,
                                          int64_t wallFrame,
                                          uint32_t callbackFrames) noexcept {
    uint32_t tolerance = callbackFrames / 2u;
    if (tolerance < 64u) tolerance = 64u;
    return wallFrame - renderFrame > static_cast<int64_t>(tolerance)
        ? wallFrame : renderFrame;
}

// Small amounts of lateness are clamped to the next writable frame. A note-on
// older than one device buffer belongs to audio time that was skipped and must
// not be replayed as a post-pause catch-up storm. State events and note-offs
// are deliberately handled separately and remain lossless.
inline bool IsObsoleteNoteOn(int64_t targetFrame, int64_t renderFrame,
                             uint32_t graceFrames) noexcept {
    return targetFrame < renderFrame - static_cast<int64_t>(graceFrames);
}

} // namespace svms

#endif
