#ifndef SVMS_NOTE_TRACE_H
#define SVMS_NOTE_TRACE_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace svms {

// Records when every voice starts and when a steal cuts it, so an offline run
// can say which note-ons were heard and for how long.  Compiled in only with
// SVMS_NOTE_TRACE (the svms_v3_midi_song target); the shipped DLLs never see it.
struct NoteTrace {
    struct Launch {
        uint32_t frame;      // start frame
        uint32_t cut;        // frame a steal displaced it, UINT32_MAX if never
        uint16_t key;        // channel << 7 | note
        uint8_t velocity;
        uint8_t reserved;
        uint32_t cycles;     // launch cost, split evenly over the voices of one note-on
    };

    std::vector<Launch> launches;   // in dispatch order, so non-decreasing frame
    uint32_t how = 0u;              // 0 general, 1 ring, 2 volatile, 3 stable victim
    uint64_t cutBy[4] = {};
    uint64_t zeroCutBy[4] = {};

    void OnLaunch(uint32_t handle, uint32_t playIndex, uint8_t channel,
                  uint8_t note, uint8_t velocity, uint64_t frame) {
        if (handle >= recordOf_.size()) {
            recordOf_.resize(handle + 1u, 0u);
            playOf_.resize(handle + 1u, UINT32_MAX);
        }
        recordOf_[handle] = static_cast<uint32_t>(launches.size());
        playOf_[handle] = playIndex;
        launches.push_back({static_cast<uint32_t>(frame), UINT32_MAX,
                            static_cast<uint16_t>((channel << 7) | note),
                            velocity, 0u, 0u});
    }

    // Spreads the cycles one launch call took over the voices it started.
    void StampCycles(size_t first, uint64_t cycles) {
        const size_t n = launches.size() - first;
        if (n == 0u) return;
        const uint32_t each = static_cast<uint32_t>(
            (std::min<uint64_t>)(cycles / n, UINT32_MAX));
        for (size_t i = first; i < launches.size(); ++i) launches[i].cycles = each;
    }

    // A twin forked at release carries its own play index, so it never matches.
    void OnSteal(uint32_t handle, uint32_t playIndex, uint64_t frame) {
        if (handle >= recordOf_.size() || playOf_[handle] != playIndex) return;
        Launch& launch = launches[recordOf_[handle]];
        if (launch.cut == UINT32_MAX) {
            launch.cut = static_cast<uint32_t>(frame);
            ++cutBy[how & 3u];
            if (launch.cut == launch.frame) ++zeroCutBy[how & 3u];
        }
    }

private:
    std::vector<uint32_t> recordOf_;   // handle -> index into launches
    std::vector<uint32_t> playOf_;     // handle -> play index of that launch
};

inline NoteTrace* g_noteTrace = nullptr;

}  // namespace svms

#endif
