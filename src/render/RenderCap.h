#pragma once

#include <atomic>
#include <cstdint>

namespace svms {

// Render-time cap (voices.render_cap_percent): keeps a block's render under
// `percent` of the block's real duration. While renders stay under the cap
// nothing is limited and nothing changes.
//
// A block that runs over is split into its two costs: planning (starting the
// new notes, serial) and rendering the voices that sound. Planning may use at
// most half the cap; past that the next blocks start only the loudest new
// note-ons that fit (the bulk admission filter; controllers and note-offs are
// never dropped). The voices get the rest of the cap; past that the voice
// limit comes down in proportion, and the voice manager releases the quietest
// voices over a short fade. So a CPU that is slow at rendering many voices
// loses quiet voices and keeps starting new notes, and one that is slow at
// starting notes skips the quietest new ones.
//
// Under the cap each limit grows back by up to a quarter per block, and after
// a run of comfortable blocks both are lifted.
//
// One governor per renderer, so every shard core caps its own blocks. The
// percent may be set from any thread; the rest runs on the render thread.
class RenderCapGovernor {
public:
    static constexpr uint32_t kUnlimited = UINT32_MAX;
    // A block always admits this many note-on units and keeps this many
    // voices: a cap the CPU cannot meet still lets the loudest through.
    static constexpr uint32_t kFloorUnits = 16u;
    static constexpr uint32_t kFloorVoices = 256u;
    // Comfortable blocks (under half the cap, nothing binding) before the
    // limits are lifted. About 0.7 s at 1024 frames and 48 kHz.
    static constexpr uint32_t kReleaseBlocks = 32u;

    void SetPercent(uint32_t percent) {
        percent_.store(percent, std::memory_order_relaxed);
    }
    uint32_t Percent() const { return percent_.load(std::memory_order_relaxed); }

    // Note-on units the coming block may start (kUnlimited = all of them).
    uint32_t Budget() const { return Percent() != 0u ? budget_ : kUnlimited; }
    // Voices the pool may hold (kUnlimited = the configured limit).
    uint32_t VoiceBudget() const {
        return Percent() != 0u ? voiceBudget_ : kUnlimited;
    }

    // After a block: its render time and real duration in seconds, the note-on
    // units it was offered and started, the share of the render spent
    // planning (0..1) and the voices sounding at its end.
    void Update(double renderSeconds, double blockSeconds,
                uint64_t offeredUnits, uint64_t admittedUnits,
                double planShare = 1.0, uint64_t activeVoices = 0u) {
        const uint32_t percent = Percent();
        if (percent == 0u || blockSeconds <= 0.0) {
            budget_ = voiceBudget_ = kUnlimited;
            calmBlocks_ = 0u;
            return;
        }
        if (planShare < 0.0) planShare = 0.0;
        if (planShare > 1.0) planShare = 1.0;
        const double target = blockSeconds * percent / 100.0;
        if (renderSeconds > target) {
            calmBlocks_ = 0u;
            const double plan = renderSeconds * planShare;
            const double voices = renderSeconds - plan;
            const double planAllowed = 0.5 * target;
            if (plan > planAllowed && admittedUnits != 0u) {
                const double next = static_cast<double>(admittedUnits) *
                                    (planAllowed / plan);
                budget_ = Clamp(next, kFloorUnits);
            }
            const double voicesAllowed =
                target - (plan < planAllowed ? plan : planAllowed);
            if (voices > voicesAllowed && activeVoices != 0u) {
                const double next = static_cast<double>(activeVoices) *
                                    (voicesAllowed / voices);
                const uint32_t shed = Clamp(next, kFloorVoices);
                voiceBudget_ = shed < voiceBudget_ ? shed : voiceBudget_;
            }
            return;
        }
        if (budget_ == kUnlimited && voiceBudget_ == kUnlimited) return;
        const bool notesBind = budget_ != kUnlimited && offeredUnits > budget_;
        const bool voicesBind = voiceBudget_ != kUnlimited &&
                                activeVoices >= voiceBudget_;
        if (!notesBind && !voicesBind && renderSeconds < 0.5 * target) {
            if (++calmBlocks_ >= kReleaseBlocks) {
                budget_ = voiceBudget_ = kUnlimited;
                calmBlocks_ = 0u;
            }
            return;
        }
        calmBlocks_ = 0u;
        const double room = renderSeconds > 0.0 ? target / renderSeconds : 1.25;
        const double grow = room < 1.25 ? room : 1.25;
        if (budget_ != kUnlimited) budget_ = Grow(budget_, grow);
        if (voiceBudget_ != kUnlimited) voiceBudget_ = Grow(voiceBudget_, grow);
    }

private:
    static uint32_t Clamp(double value, uint32_t floor) {
        if (value < floor) return floor;
        if (value >= static_cast<double>(kUnlimited - 1u)) return kUnlimited - 1u;
        return static_cast<uint32_t>(value);
    }
    static uint32_t Grow(uint32_t value, double factor) {
        return Clamp(static_cast<double>(value) * factor + 1.0, value);
    }

    std::atomic<uint32_t> percent_{0u};
    uint32_t budget_ = kUnlimited;
    uint32_t voiceBudget_ = kUnlimited;
    uint32_t calmBlocks_ = 0u;
};

} // namespace svms
