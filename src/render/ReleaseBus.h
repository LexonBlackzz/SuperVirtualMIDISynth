#pragma once

// The release bus: every release tail of one kernel (a note's regions on one key) on one channel rendered by one
// convolution.
//
// A note on channel c, on at frame t0, released d frames later, with velocity gain v, sounds
//   v * A(tau)                    for tau < d   (held)
//   v * A(tau) * r^(tau - d + 1)  for tau >= d  (released: the voice's first released frame is decayed once)
// With B(tau) = A(tau) * r^tau and w = v * r^(1 - d) the release is w * B(tau). The bus splits a note at
// D = 8 blocks: tau < D is rendered directly from A (held, then released), and tau >= D by convolving w at t0 with
// B, whose kernel has no level 0 (the first 8 blocks). Each channel's inputs share one ConvBank; the channel's sum
// is then multiplied by its mix gains frame by frame, as a voice's output is, so tails follow volume, expression
// and pan.
//
// Two kinds of note:
// - a same-block note (note-off in the block of its note-on, no voice at all): AddNote, while the block is open;
// - a late note, which played as a voice and is handed over after its note-off: AddImpulse at the note-off (its
//   note-on is in the past, which the convolution allows until its first chunk is transformed: Insertable), then
//   AddDirect from the frame its voices stop up to t0 + D.
//
// The bus runs on a grid of fixed blocks (frames [k * block, (k + 1) * block)). A block is processed once its
// output is first needed; the synth checks Open and Insertable before adding.

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
    static constexpr uint32_t kDirectBlocks = 8u;   // the span of the kernels' missing level 0

    void Configure(uint32_t block) {
        block_ = block;
        ring_ = 2u * kDirectBlocks * block;   // whole blocks, more than a note's direct span past the open block
        for (Channel& c : ch_) {
            c.pre[0].assign(block, 0.0f); c.pre[1].assign(block, 0.0f);
            c.dir[0].assign(ring_, 0.0f); c.dir[1].assign(ring_, 0.0f);
            c.bank.Prepare(static_cast<int>(block));
        }
        Reset(0);
    }
    uint32_t Block() const noexcept { return block_; }
    uint32_t Direct() const noexcept { return kDirectBlocks * block_; }

    // Whether a note on `channel` at `frame` can still join as a same-block note.
    bool Open(uint8_t channel, uint64_t frame) const noexcept {
        return block_ && frame >= processedEnd_ && ch_[channel & 15u].cutAt == UINT64_MAX;
    }
    // Whether a late note's impulse at `on` can still join the convolution.
    bool Insertable(uint8_t channel, uint64_t on) const noexcept {
        const Channel& c = ch_[channel & 15u];
        return block_ && c.cutAt == UINT64_MAX && c.bank.Insertable(on, processedEnd_ / block_, false);
    }
    uint64_t BlockEnd(uint64_t frame) const noexcept { return (frame / block_ + 1u) * block_; }
    // The first frame not yet output-ready: frames from here on can still take direct sound.
    uint64_t Processed() const noexcept { return processedEnd_; }

    // A same-block note: on at `on` (an open frame), held `held` frames (within on's block).
    void AddNote(uint64_t kernelKey, const ReleaseKernel* k, uint8_t channel, uint64_t on, uint32_t held, float v) {
        if (on < processedEnd_) return;   // the synth checked Open; belt and braces
        AddImpulse(kernelKey, k, channel, on, held, v);
        AddDirect(k, channel, on, held, v, on);
        ++notes_;
    }

    // The convolved part of a note (tau >= D): w at `on`. The synth checked Open or Insertable.
    void AddImpulse(uint64_t kernelKey, const ReleaseKernel* k, uint8_t channel, uint64_t on, uint32_t held, float v) {
        Channel& c = ch_[channel & 15u];
        auto it = c.inputs.find(kernelKey);
        if (it == c.inputs.end()) it = c.inputs.emplace(kernelKey, In{k, c.bank.Add(k->tail), on}).first;
        const double w = v * std::pow(static_cast<double>(k->releaseDecay), 1.0 - static_cast<double>(held));
        c.bank.AddImpulse(it->second.id, on, static_cast<float>(w), processedEnd_ / block_);
        it->second.lastInput = (std::max)(it->second.lastInput, on);
        c.active = true;
    }

    // The direct part of a note, frames [from, on + D): v * A(tau) while held, v * A(tau) * r^(tau - held + 1)
    // after. `from` must not be output yet.
    void AddDirect(const ReleaseKernel* k, uint8_t channel, uint64_t on, uint32_t held, float v, uint64_t from) {
        Channel& c = ch_[channel & 15u];
        const uint64_t end = on + (std::min)(static_cast<uint64_t>(Direct()), static_cast<uint64_t>(k->heldFrames));
        const uint64_t open = processedEnd_ - (processedEnd_ ? block_ : 0u);   // start of the processed block
        if (from < open) from = open;   // already output: cannot change
        if (from >= end) return;
        const double r = k->releaseDecay;
        uint32_t t = static_cast<uint32_t>(from - on);
        // r^(tau - held + 1) at the first released frame on or after `from`.
        double p = std::pow(r, static_cast<double>((std::max)(t, held)) - static_cast<double>(held) + 1.0);
        uint32_t slot = static_cast<uint32_t>(from % ring_);
        for (uint64_t f = from; f < end; ++f, ++t, slot = slot + 1u == ring_ ? 0u : slot + 1u) {
            const float g = static_cast<float>(t < held ? v : v * p);
            if (t >= held) p *= r;
            const float l = k->heldL[t] * g, rr = k->heldR[t] * g;
            if (f < processedEnd_) {
                c.pre[0][static_cast<size_t>(f - open)] += l;
                c.pre[1][static_cast<size_t>(f - open)] += rr;
                c.preLive = true;
            } else {
                c.dir[0][slot] += l;
                c.dir[1][slot] += rr;
            }
        }
        c.active = true;
    }

    // The channel's mix gains from `frame` on (a voice's mixScaleLeft / mixScaleRight).
    void SetMix(uint8_t channel, uint64_t frame, float left, float right) {
        Channel& c = ch_[channel & 15u];
        if (!c.changes.empty() && c.changes.back().frame > frame) { c.changes.clear(); c.next = 0; }
        c.changes.push_back({frame, left, right});
    }

    // All sound off on a channel at `frame`: its tails stop there, as voices do. Until the cut's block is
    // processed the channel takes no new notes.
    void Cut(uint8_t channel, uint64_t frame) {
        Channel& c = ch_[channel & 15u];
        if (frame < processedEnd_) {
            // The block is already processed: silence what is not yet output, drop the rest.
            const uint64_t from = frame > processedEnd_ - block_ ? frame - (processedEnd_ - block_) : 0u;
            for (int s = 0; s < 2; ++s) std::fill(c.pre[s].begin() + static_cast<std::ptrdiff_t>(from), c.pre[s].end(), 0.0f);
            Drop(c);
        } else {
            c.cutAt = (std::min)(c.cutAt, frame);
        }
    }

    // Drops every tail from `frame` on (reset, SoundFont change).
    void Reset(uint64_t frame) {
        for (Channel& c : ch_) {
            Drop(c);
            for (int s = 0; s < 2; ++s) std::fill(c.pre[s].begin(), c.pre[s].end(), 0.0f);
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
    uint64_t Notes() const noexcept { return notes_; }   // same-block notes
    uint64_t Blocks() const noexcept { return blocks_; }
    uint64_t Convolved() const noexcept { return convolved_; }   // input-blocks
    uint64_t BusNs() const noexcept { return busNs_; }

private:
    struct In { const ReleaseKernel* kernel; int id; uint64_t lastInput; };
    struct Change { uint64_t frame; float left, right; };
    struct Channel {
        std::vector<float> pre[2];   // the processed block's sum before the mix gains
        std::vector<float> dir[2];   // direct sound of later frames, frame f at f & (ring - 1)
        ConvBank bank;
        std::unordered_map<uint64_t, In> inputs;   // kernel key -> bank input
        std::vector<Change> changes;
        size_t next = 0;
        float mixL = 0.0f, mixR = 0.0f;
        uint64_t cutAt = UINT64_MAX;
        bool active = false, preLive = false;
    };

    void Drop(Channel& c) {
        c.bank.RemoveAll();
        c.inputs.clear();
        for (int s = 0; s < 2; ++s) std::fill(c.dir[s].begin(), c.dir[s].end(), 0.0f);
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
        const uint64_t b = blockStart / block_;
        ++blocks_;
        for (Channel& c : ch_) {
            bool live = c.active;
            c.active = false;
            const size_t at = static_cast<size_t>(blockStart % ring_);   // ring_ is whole blocks
            for (int s = 0; s < 2; ++s) {
                std::copy(c.dir[s].begin() + static_cast<std::ptrdiff_t>(at),
                          c.dir[s].begin() + static_cast<std::ptrdiff_t>(at + block_), c.pre[s].begin());
                std::fill(c.dir[s].begin() + static_cast<std::ptrdiff_t>(at),
                          c.dir[s].begin() + static_cast<std::ptrdiff_t>(at + block_), 0.0f);
            }
            convolved_ += c.inputs.size();
            // Every block, inputs or not: outputs made earlier may still be due, and the bank's chunks follow the grid.
            live = c.bank.Process(c.pre[0].data(), c.pre[1].data(), b) || live;
            if (c.cutAt < blockStart + block_) {
                const uint64_t from = c.cutAt > blockStart ? c.cutAt - blockStart : 0u;
                for (int s = 0; s < 2; ++s) std::fill(c.pre[s].begin() + static_cast<std::ptrdiff_t>(from), c.pre[s].end(), 0.0f);
                Drop(c);
                c.cutAt = UINT64_MAX;
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

    uint32_t block_ = 0, ring_ = 1;
    uint64_t processedEnd_ = 0;    // the grid block before this is processed; this one is open
    Channel ch_[kChannels];
    uint64_t notes_ = 0, blocks_ = 0, convolved_ = 0, busNs_ = 0;
};

}  // namespace bus
}  // namespace svms
