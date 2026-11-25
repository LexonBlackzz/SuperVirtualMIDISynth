#pragma once

#include <atomic>
#include <cstdint>

namespace svms {

// Render-time cap (voices.render_cap_percent): keeps a block's render under
// `percent` of the block's real duration. While renders stay under the cap
// the budget is unlimited and nothing changes. A block that runs over sets a
// budget of new note-on units for the next blocks, in proportion to the
// overrun; RenderBlock then starts only the loudest note-ons that fit (the
// bulk admission filter) and never touches controllers or note-offs. Under
// the cap the budget grows back by up to a quarter per block, and after a
// run of comfortable blocks it is lifted again.
//
// One governor per renderer, so every shard core caps its own blocks. The
// percent may be set from any thread; Budget/Update run on the render thread.
class RenderCapGovernor {
public:
    static constexpr uint32_t kUnlimited = UINT32_MAX;
    // A block always admits this many units: a cap the CPU cannot meet even
    // without new notes still lets the loudest ones through.
    static constexpr uint32_t kFloorUnits = 16u;
    // Comfortable blocks (under half the cap, budget not binding) before the
    // budget is lifted. About 0.7 s at 1024 frames and 48 kHz.
    static constexpr uint32_t kReleaseBlocks = 32u;

    void SetPercent(uint32_t percent) {
        percent_.store(percent, std::memory_order_relaxed);
    }
    uint32_t Percent() const { return percent_.load(std::memory_order_relaxed); }

    // Note-on units the coming block may start (kUnlimited = all of them).
    uint32_t Budget() const { return Percent() != 0u ? budget_ : kUnlimited; }

    // After a block: its render time and real duration in seconds, the note-on
    // units it was offered and the units it started.
    void Update(double renderSeconds, double blockSeconds,
                uint64_t offeredUnits, uint64_t admittedUnits) {
        const uint32_t percent = Percent();
        if (percent == 0u || blockSeconds <= 0.0) {
            budget_ = kUnlimited;
            calmBlocks_ = 0u;
            return;
        }
        const double target = blockSeconds * percent / 100.0;
        if (renderSeconds > target) {
            calmBlocks_ = 0u;
            // No new notes to cut: the time went to voices already sounding.
            if (admittedUnits == 0u) return;
            const double next = static_cast<double>(admittedUnits) *
                                (target / renderSeconds);
            budget_ = next < kFloorUnits ? kFloorUnits
                                         : static_cast<uint32_t>(next);
            return;
        }
        if (budget_ == kUnlimited) return;
        if (offeredUnits <= budget_ && renderSeconds < 0.5 * target) {
            if (++calmBlocks_ >= kReleaseBlocks) {
                budget_ = kUnlimited;
                calmBlocks_ = 0u;
            }
            return;
        }
        calmBlocks_ = 0u;
        const double room = renderSeconds > 0.0 ? target / renderSeconds : 1.25;
        const double grown = static_cast<double>(budget_) *
                             (room < 1.25 ? room : 1.25) + 1.0;
        budget_ = grown >= static_cast<double>(kUnlimited - 1u)
            ? kUnlimited - 1u : static_cast<uint32_t>(grown);
    }

private:
    std::atomic<uint32_t> percent_{0u};
    uint32_t budget_ = kUnlimited;
    uint32_t calmBlocks_ = 0u;
};

} // namespace svms
