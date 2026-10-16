#ifndef SVMS_MIDI_BYTE_STREAM_H
#define SVMS_MIDI_BYTE_STREAM_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace svms {

// Live raw-MIDI byte-stream parser for long-data buffers (midiOutLongMsg,
// SendDirectLongData, native send_midi_stream). A buffer may carry any mix of
// channel messages (with running status), complete or fragmented SysEx, and
// interleaved realtime bytes. State persists across Feed() calls so a SysEx
// or running status split over several buffers stays intact.
//
// Pure: no driver, no allocation on the channel-message path.
class MidiByteStreamParser {
public:
    // Largest SysEx kept; a longer one is dropped whole (bulk dumps).
    static constexpr size_t kMaxSysEx = 1u << 20;

    void Reset() noexcept {
        running_ = 0u; have_ = 0u; need_ = 0u; skip_ = 0u;
        inSysex_ = false; overflow_ = false; sysex_.clear();
    }

    // onShort(uint32_t packed 0x00ddsscc), onSysex(const uint8_t*, uint32_t)
    // with the full F0..F7 frame, in stream order.
    template <typename Short, typename Sysex>
    void Feed(const uint8_t* p, size_t n, Short&& onShort, Sysex&& onSysex) {
        for (size_t i = 0u; i < n; ++i) {
            const uint8_t b = p[i];
            if (b >= 0xf8u) continue;               // realtime: no state change
            if (b < 0x80u) {                        // data byte
                if (inSysex_) {
                    if (sysex_.size() < kMaxSysEx) sysex_.push_back(b);
                    else overflow_ = true;
                } else if (skip_ != 0u) {
                    --skip_;
                } else if (running_ != 0u) {
                    data_[have_++] = b;
                    if (have_ == need_) {
                        onShort(static_cast<uint32_t>(running_) |
                                (static_cast<uint32_t>(data_[0]) << 8u) |
                                (need_ == 2u
                                     ? static_cast<uint32_t>(data_[1]) << 16u
                                     : 0u));
                        have_ = 0u;
                    }
                }
                continue;
            }
            // Status byte. Any status ends an open SysEx; only F7 completes it.
            if (inSysex_) {
                inSysex_ = false;
                if (b == 0xf7u && !overflow_) {
                    sysex_.push_back(0xf7u);
                    onSysex(sysex_.data(), static_cast<uint32_t>(sysex_.size()));
                }
                sysex_.clear();
                overflow_ = false;
                if (b == 0xf7u) continue;
            }
            have_ = 0u;
            if (b < 0xf0u) {
                running_ = b;
                need_ = (b >= 0xc0u && b < 0xe0u) ? 1u : 2u;
                skip_ = 0u;
            } else {
                running_ = 0u;                      // system common clears it
                skip_ = b == 0xf2u ? 2u : (b == 0xf1u || b == 0xf3u) ? 1u : 0u;
                if (b == 0xf0u) {
                    inSysex_ = true;
                    sysex_.push_back(0xf0u);
                }
            }
        }
    }

private:
    uint8_t running_ = 0u, have_ = 0u, need_ = 0u, skip_ = 0u;
    uint8_t data_[2] = {};
    bool inSysex_ = false, overflow_ = false;
    std::vector<uint8_t> sysex_;
};

} // namespace svms

#endif
