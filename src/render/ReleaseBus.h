#pragma once

// The release bus: every release tail of one kernel (a note's regions on one key) on one channel rendered by one
// convolution.
//
// A note on channel c, on at frame t0, released d frames later, with velocity gain v, sounds
//   v * A(tau)                    for tau < d   (held)
//   v * A(tau) * r^(tau - d + 1)  for tau >= d  (released: the voice's first released frame is decayed once)
// With B(tau) = A(tau) * r^tau and w = v * r^(1 - d) that is w * B(tau) for every tau, plus the correction
// v * A(tau) - w * B(tau) over tau < d. The bus adds w at t0 to the input of (kernel, c), and the correction to the
// channel's own buffer. Each channel's inputs share one ConvBank (one inverse FFT per level for all of them); the
// channel's sum is then multiplied by its mix gains frame by frame, as a voice's output is, so tails follow volume,
// expression and pan.
//
// The bus runs on a grid of fixed blocks (frames [k * block, (k + 1) * block)). A block is convolved once its output
// is first needed, with the input it has by then; a note can join only a block not yet convolved, and its note-off
// must fall in the same block (later releases are a later step). The synth checks all of this before AddNote.

#include "render/ReleaseKernels.h"

#include <algorithm>
#include <chrono>
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
            c.bank.Prepare(static_cast<int>(block));
        }
        Reset(0);
    }
    uint32_t Block() const noexcept { return block_; }

    // Whether a note on `channel` at `frame` can still join the convolution.
    bool Open(uint8_t channel, uint64_t frame) const noexcept {
        return block_ && frame >= processedEnd_ && ch_[channel & 15u].cutAt == UINT64_MAX;
    }
    uint64_t BlockEnd(uint64_t frame) const noexcept { return (frame / block_ + 1u) * block_; }

    // One kernel of a note: on at `on` (an open frame), held `held` frames (on + held within on's block).
    void AddNote(uint64_t kernelKey, const ReleaseKernel* k, uint8_t channel, uint64_t on, uint32_t held, float v) {
        Channel& c = ch_[channel & 15u];
        if (on < processedEnd_) return;   // the synth checked Open; belt and braces
        auto it = c.inputs.find(kernelKey);
        if (it == c.inputs.end()) it = c.inputs.emplace(kernelKey, In{k, c.bank.Add(k->tail), on}).first;
        const uint32_t at = static_cast<uint32_t>(on - processedEnd_);
        const double r = k->releaseDecay;
        const double w = v * std::pow(r, 1.0 - static_cast<double>(held));
        c.bank.InputBlock(it->second.id)[at] += static_cast<float>(w);
        it->second.lastInput = on;
        // Correction v * A(tau) * (1 - r^(tau - d + 1)) over tau < d.
        double p = w / v;   // r^(tau - d + 1) at tau = 0
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
        if (!c.changes.empty() && c.changes.back().frame > frame) { c.changes.clear(); c.next = 0; }
        c.changes.push_back({frame, left, right});
    }

    // All sound off on a channel at `frame`: its tails stop there, as voices do. Until the cut's block is
    // convolved the channel takes no new notes.
    void Cut(uint8_t channel, uint64_t frame) {
        Channel& c = ch_[channel & 15u];
        if (frame < processedEnd_) {
            // The block is already convolved: silence what is not yet output, drop the inputs.
            const uint64_t from = frame > processedEnd_ - block_ ? frame - (processedEnd_ - block_) : 0u;
            for (int s = 0; s < 2; ++s) std::fill(c.pre[s].begin() + static_cast<std::ptrdiff_t>(from), c.pre[s].end(), 0.0f);
            DropInputs(c);
        } else {
            c.cutAt = (std::min)(c.cutAt, frame);
        }
    }

    // Drops every tail from `frame` on (reset, SoundFont change).
    void Reset(uint64_t frame) {
        for (Channel& c : ch_) {
            DropInputs(c);
            for (int s = 0; s < 2; ++s) {
                std::fill(c.pre[s].begin(), c.pre[s].end(), 0.0f);
                std::fill(c.corr[s].begin(), c.corr[s].end(), 0.0f);
            }
            c.changes.clear();
            c.next = 0;
            c.active = c.preLive = false;
            c.cutAt = UINT64_MAX;
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

    size_t Histories() const noexcept {
        size_t n = 0;
        for (const Channel& c : ch_) n += c.inputs.size();
        return n;
    }
    uint64_t Notes() const noexcept { return notes_; }
    uint64_t Blocks() const noexcept { return blocks_; }
    uint64_t Convolved() const noexcept { return convolved_; }   // input-blocks
    uint64_t BusNs() const noexcept { return busNs_; }

private:
    struct In { const ReleaseKernel* kernel; int id; uint64_t lastInput; };
    struct Change { uint64_t frame; float left, right; };
    struct Channel {
        std::vector<float> pre[2], corr[2];   // this block's sum before the mix gains; open block's corrections
        ConvBank bank;
        std::unordered_map<uint64_t, In> inputs;   // kernel key -> bank input
        std::vector<Change> changes;
        size_t next = 0;
        float mixL = 0.0f, mixR = 0.0f;
        uint64_t cutAt = UINT64_MAX;
        bool active = false, preLive = false;
    };

    static void DropInputs(Channel& c) {
        c.bank.RemoveAll();
        c.inputs.clear();
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

    void ProcessBlock() {
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t blockStart = processedEnd_;
        ++blocks_;
        for (Channel& c : ch_) {
            for (int s = 0; s < 2; ++s) {
                std::copy(c.corr[s].begin(), c.corr[s].end(), c.pre[s].begin());
                std::fill(c.corr[s].begin(), c.corr[s].end(), 0.0f);
            }
            bool live = c.active;
            c.active = false;
            if (!c.inputs.empty()) {
                convolved_ += c.inputs.size();
                live = c.bank.Process(c.pre[0].data(), c.pre[1].data()) || live;
                if (c.cutAt < blockStart + block_) {
                    const uint64_t from = c.cutAt > blockStart ? c.cutAt - blockStart : 0u;
                    for (int s = 0; s < 2; ++s) std::fill(c.pre[s].begin() + static_cast<std::ptrdiff_t>(from), c.pre[s].end(), 0.0f);
                    DropInputs(c);
                } else {
                    for (auto it = c.inputs.begin(); it != c.inputs.end();) {
                        const In& in = it->second;
                        if (blockStart + block_ >= in.lastInput + static_cast<uint64_t>(in.kernel->tail.length) + block_) {
                            c.bank.Remove(in.id);
                            it = c.inputs.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }
            if (c.cutAt < blockStart + block_) c.cutAt = UINT64_MAX;
            if (!c.inputs.empty()) c.active = true;
            // Exactly-zero channels skip the mix loop.
            c.preLive = false;
            if (live)
                for (int s = 0; s < 2 && !c.preLive; ++s)
                    for (float x : c.pre[s]) if (x != 0.0f) { c.preLive = true; break; }
        }
        processedEnd_ = blockStart + block_;
        busNs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }

    uint32_t block_ = 0;
    uint64_t processedEnd_ = 0;    // the grid block before this is convolved; this one is open
    Channel ch_[kChannels];
    uint64_t notes_ = 0, blocks_ = 0, convolved_ = 0, busNs_ = 0;
};

}  // namespace bus
}  // namespace svms
