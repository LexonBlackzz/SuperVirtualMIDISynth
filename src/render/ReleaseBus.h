#pragma once

// The release bus: every release tail of one kernel (region and key) on one channel rendered by one convolution.
//
// A note on channel c, on at frame t0, released d frames later, with velocity gain v, sounds
//   v * A(tau)                    for tau < d   (held)
//   v * A(tau) * r^(tau - d + 1)  for tau >= d  (released: the voice's first released frame is decayed once)
// With B(tau) = A(tau) * r^tau and w = v * r^(1 - d) that is w * B(tau) for every tau, plus the correction
// v * A(tau) - w * B(tau) over tau < d. The bus adds w at t0 to the history of (kernel, c), and the correction to the
// channel's own buffer. Each channel's sum is then multiplied by the channel's mix gains frame by frame, as a voice's
// output is, so tails follow volume, expression and pan.
//
// The bus runs on a grid of fixed blocks (frames [k * block, (k + 1) * block)). A block is convolved once its output
// is first needed, with the input it has by then; a note can join only a block not yet convolved, and its note-off
// must fall in the same block (later releases are a later step). The synth checks all of this before AddNote.

#include "render/ReleaseKernels.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace svms {
namespace bus {

class ReleaseBus {
public:
    static constexpr uint32_t kChannels = 16u;

    void Configure(uint32_t block) {
        block_ = block;
        for (Channel& c : ch_) {
            c.pre[0].assign(block, 0.0f); c.pre[1].assign(block, 0.0f);
            c.corr[0].assign(block, 0.0f); c.corr[1].assign(block, 0.0f);
        }
        Reset(0);
    }
    uint32_t Block() const noexcept { return block_; }

    // Whether a note at `frame` can still join the convolution.
    bool Open(uint64_t frame) const noexcept { return block_ && frame >= processedEnd_; }
    uint64_t BlockEnd(uint64_t frame) const noexcept { return (frame / block_ + 1u) * block_; }

    // One region of a note: on at `on` (an open frame), held `held` frames (on + held within on's block).
    void AddNote(uint64_t kernelKey, const ReleaseKernel* k, uint8_t channel, uint64_t on, uint32_t held, float v) {
        Channel& c = ch_[channel & 15u];
        if (on < processedEnd_) return;   // the synth checked Open; belt and braces
        History& h = HistoryFor(kernelKey, k, channel);
        const uint32_t at = static_cast<uint32_t>(on - processedEnd_);
        const double r = k->releaseDecay;
        const double w = v * std::pow(r, 1.0 - static_cast<double>(held));
        h.in[at] += static_cast<float>(w);
        h.lastInput = on;
        // Correction v * A(tau) * (1 - r^(tau - d + 1)) over tau < d.
        double p = std::pow(r, 1.0 - static_cast<double>(held));   // r^(tau - d + 1) at tau = 0
        for (uint32_t t = 0; t < held && at + t < block_; ++t, p *= r) {
            const float g = static_cast<float>(v * (1.0 - p));
            c.corr[0][at + t] += k->heldL[t] * g;
            c.corr[1][at + t] += k->heldR[t] * g;
        }
        c.active = true;
        ++notes_;
    }

    // The channel's mix gains from `frame` on (a voice's mixScaleLeft / mixScaleRight).
    void SetMix(uint8_t channel, uint64_t frame, float left, float right) {
        Channel& c = ch_[channel & 15u];
        if (!c.changes.empty() && c.changes.back().frame > frame) c.changes.clear();   // the clock went back: reset
        c.changes.push_back({frame, left, right});
    }

    // All sound off on a channel at `frame`: its tails stop there, as voices do.
    void Cut(uint8_t channel, uint64_t frame) {
        Channel& c = ch_[channel & 15u];
        if (frame < processedEnd_) {
            // The block is already convolved: silence what is not yet output, drop the histories.
            const uint64_t from = frame > processedEnd_ - block_ ? frame - (processedEnd_ - block_) : 0u;
            for (int s = 0; s < 2; ++s) std::fill(c.pre[s].begin() + static_cast<std::ptrdiff_t>(from), c.pre[s].end(), 0.0f);
            DropChannel(channel);
        } else {
            for (auto& e : hist_) if (e.second->channel == (channel & 15u)) e.second->cutAt = (std::min)(e.second->cutAt, frame);
        }
    }

    // Drops every tail from `frame` on (reset, SoundFont change).
    void Reset(uint64_t frame) {
        hist_.clear();
        for (Channel& c : ch_) {
            for (int s = 0; s < 2; ++s) {
                std::fill(c.pre[s].begin(), c.pre[s].end(), 0.0f);
                std::fill(c.corr[s].begin(), c.corr[s].end(), 0.0f);
            }
            c.changes.clear();
            c.active = c.preLive = false;
        }
        processedEnd_ = block_ ? frame / block_ * block_ : 0u;
        if (block_ && frame % block_) processedEnd_ += block_;   // a block cut mid-way is not reopened
    }

    // Adds the bus output for [frame, frame + n) to left/right. Calls must be contiguous.
    void Render(float* left, float* right, uint32_t n, uint64_t frame) {
        if (!block_) return;
        uint32_t done = 0;
        while (done < n) {
            const uint64_t f = frame + done;
            if (f >= processedEnd_) {
                if (f >= processedEnd_ + block_) Reset(f);   // a gap: nothing carries over
                ProcessBlock();
            }
            const uint64_t blockStart = processedEnd_ - block_;
            const uint32_t off = static_cast<uint32_t>(f - blockStart);
            const uint32_t take = (std::min)(n - done, block_ - off);
            for (uint32_t ci = 0; ci < kChannels; ++ci) {
                Channel& c = ch_[ci];
                if (!c.preLive) { ApplyChanges(c, f + take); continue; }
                for (uint32_t i = 0; i < take; ++i) {
                    const uint64_t at = f + i;
                    while (c.next < c.changes.size() && c.changes[c.next].frame <= at) {
                        c.mixL = c.changes[c.next].left; c.mixR = c.changes[c.next].right; ++c.next;
                    }
                    left[done + i] += c.pre[0][off + i] * c.mixL;
                    right[done + i] += c.pre[1][off + i] * c.mixR;
                }
                TrimChanges(c);
            }
            done += take;
        }
    }

    size_t Histories() const noexcept { return hist_.size(); }
    uint64_t Notes() const noexcept { return notes_; }

private:
    struct History {
        const ReleaseKernel* kernel = nullptr;
        ConvHistory conv;
        std::vector<float> in;
        uint8_t channel = 0;
        uint64_t lastInput = 0;
        uint64_t cutAt = UINT64_MAX;
    };
    struct Change { uint64_t frame; float left, right; };
    struct Channel {
        std::vector<float> pre[2], corr[2];   // this block's sum before the mix gains; open block's corrections
        std::vector<Change> changes;
        size_t next = 0;
        float mixL = 0.0f, mixR = 0.0f;
        bool active = false, preLive = false;
    };

    History& HistoryFor(uint64_t kernelKey, const ReleaseKernel* k, uint8_t channel) {
        const uint64_t id = (kernelKey << 4) | (channel & 15u);
        auto it = hist_.find(id);
        if (it != hist_.end() && it->second->cutAt == UINT64_MAX) return *it->second;
        std::unique_ptr<History> h(new History());
        h->kernel = k;
        h->conv.Prepare(k->tail);
        h->in.assign(block_, 0.0f);
        h->channel = channel & 15u;
        if (it != hist_.end()) {
            // A cut history still owes its output up to the cut: keep it under a fresh id until then.
            dying_.push_back(std::move(it->second));
            it->second = std::move(h);
            return *it->second;
        }
        return *hist_.emplace(id, std::move(h)).first->second;
    }

    void DropChannel(uint8_t channel) {
        for (auto it = hist_.begin(); it != hist_.end();)
            it = it->second->channel == (channel & 15u) ? hist_.erase(it) : std::next(it);
        dying_.erase(std::remove_if(dying_.begin(), dying_.end(),
                                    [&](const std::unique_ptr<History>& h) { return h->channel == (channel & 15u); }),
                     dying_.end());
    }

    void ApplyChanges(Channel& c, uint64_t upTo) {
        while (c.next < c.changes.size() && c.changes[c.next].frame < upTo) {
            c.mixL = c.changes[c.next].left; c.mixR = c.changes[c.next].right; ++c.next;
        }
        TrimChanges(c);
    }
    static void TrimChanges(Channel& c) {
        if (c.next == c.changes.size()) { c.changes.clear(); c.next = 0; }
    }

    void Convolve(History& h, const uint64_t blockStart) {
        Channel& c = ch_[h.channel];
        scratchL_.assign(block_, 0.0f);
        scratchR_.assign(block_, 0.0f);
        h.conv.Process(h.kernel->tail, h.in.data(), scratchL_.data(), scratchR_.data());
        std::fill(h.in.begin(), h.in.end(), 0.0f);
        const uint32_t stop = h.cutAt <= blockStart ? 0u
            : static_cast<uint32_t>((std::min)(static_cast<uint64_t>(block_), h.cutAt - blockStart));
        for (uint32_t i = 0; i < stop; ++i) { c.pre[0][i] += scratchL_[i]; c.pre[1][i] += scratchR_[i]; }
        c.preLive = true;
    }

    void ProcessBlock() {
        const uint64_t blockStart = processedEnd_;
        for (Channel& c : ch_) {
            for (int s = 0; s < 2; ++s) {
                std::copy(c.corr[s].begin(), c.corr[s].end(), c.pre[s].begin());
                std::fill(c.corr[s].begin(), c.corr[s].end(), 0.0f);
            }
            c.preLive = c.active;
            c.active = false;
        }
        for (auto it = hist_.begin(); it != hist_.end();) {
            History& h = *it->second;
            Convolve(h, blockStart);
            const bool cut = h.cutAt < blockStart + block_;
            const bool silent = blockStart + block_ >= h.lastInput + static_cast<uint64_t>(h.kernel->tail.length) + block_;
            if (cut || silent) it = hist_.erase(it);
            else { ch_[h.channel].active = true; ++it; }
        }
        for (auto& h : dying_) Convolve(*h, blockStart);
        dying_.clear();
        for (Channel& c : ch_) {
            // Exactly-zero channels skip the mix loop.
            if (!c.preLive) continue;
            bool any = false;
            for (int s = 0; s < 2 && !any; ++s)
                for (float x : c.pre[s]) if (x != 0.0f) { any = true; break; }
            c.preLive = any;
        }
        processedEnd_ = blockStart + block_;
    }

    uint32_t block_ = 0;
    uint64_t processedEnd_ = 0;    // the grid block before this is convolved; this one is open
    Channel ch_[kChannels];
    std::unordered_map<uint64_t, std::unique_ptr<History>> hist_;
    std::vector<std::unique_ptr<History>> dying_;
    std::vector<float> scratchL_, scratchR_;
    uint64_t notes_ = 0;
};

}  // namespace bus
}  // namespace svms
