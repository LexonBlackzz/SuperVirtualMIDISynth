#pragma once

// The release bus: every release tail of one kernel (a note's regions on one key) under one mix rendered by one
// convolution.
//
// A note on channel c, on at frame t0, released d frames later, with velocity gain v, sounds
//   v * A(tau)                    for tau < d   (held)
//   v * A(tau) * r^(tau - d + 1)  for tau >= d  (released: the voice's first released frame is decayed once)
// With B(tau) = A(tau) * r^tau and w = v * r^(1 - d) the release is w * B(tau). The bus splits a note at
// D = 8 blocks: tau < D is rendered directly from A (held, then released), and tau >= D by convolving w at t0 with
// B, whose kernel has no level 0 (the first 8 blocks).
//
// Channels whose mix gains (volume, expression, pan) are the same from now on share a group: one ConvBank, one
// input per kernel whatever the channel, one mix loop. A group's sum is multiplied by its mix gains frame by
// frame, as a voice's output is, so tails follow volume, expression and pan. A mix change on a channel alone in
// its group moves the group; so do changes all its channels make at the same frame to the same gains. Otherwise
// the channel moves out with its tails, to a group of its new mix: the bus keeps each channel's impulses and
// direct parts while they sound, and ConvBank::Move takes them out of one bank and into another whatever their
// age. A cut (all sound off) on a shared group's channel takes its tails out the same way, at the cut's frame.
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
#include <utility>
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
        groups_.clear();
        for (uint32_t i = 0; i < kChannels; ++i) NewGroup();
        for (Chan& h : chan_) h = Chan{};
        Reset(0);
    }
    // Threads for the convolutions (ConvBank::SetParallel).
    void SetParallel(ConvBank::ParallelFn fn, void* user, uint32_t lanes) {
        par_ = fn; parUser_ = user; lanes_ = lanes;
        for (auto& g : groups_) g->bank.SetParallel(fn, user, lanes);
    }
    uint32_t Block() const noexcept { return block_; }
    uint32_t Direct() const noexcept { return kDirectBlocks * block_; }
    // Off: every channel keeps a group of its own (the bus before groups; tests compare the two).
    void SetSharing(bool on) noexcept { share_ = on; }
    // On: the kernels keep level 0 (KernelStore keepLevel0), so a note's direct part is a correction of its held
    // frames only. Every note must then come at its note-on (AddNote); late impulses cannot join level 0.
    void SetLevel0(bool on) noexcept { level0_ = on; }
    bool Level0() const noexcept { return level0_; }

    // Whether a note on `channel` at `frame` can still join as a same-block note.
    bool Open(uint8_t channel, uint64_t frame) const noexcept {
        return block_ && frame >= processedEnd_ && !CutPending(chan_[channel & 15u]);
    }
    // Whether a late note's impulse at `on` can still join the convolution (every bank has the same levels).
    bool Insertable(uint8_t channel, uint64_t on) const noexcept {
        return block_ && !CutPending(chan_[channel & 15u]) &&
               groups_[0]->bank.Insertable(on, processedEnd_ / block_, false);
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
        Chan& h = chan_[channel & 15u];
        Group& g = Route(h, on + static_cast<uint64_t>(k->tail.length) + block_);
        In& in = InputOf(g, kernelKey, k);
        const float w = static_cast<float>(v * std::pow(static_cast<double>(k->releaseDecay), 1.0 - static_cast<double>(held)));
        g.bank.AddImpulse(in.id, on, w, processedEnd_ / block_);
        in.lastInput = (std::max)(in.lastInput, on);
        g.active = true;
        if (!share_) return;
        Hist& hs = h.hist[kernelKey];
        hs.k = k;
        hs.imps.emplace_back(on, w);
        if (hs.imps.size() >= 2u * hs.keep) { Prune(hs); hs.keep = (std::max)(size_t(64), hs.imps.size()); }
    }

    // The direct part of a note, frames [from, on + D): v * A(tau) while held, v * A(tau) * r^(tau - held + 1)
    // after. `from` must not be output yet.
    void AddDirect(const ReleaseKernel* k, uint8_t channel, uint64_t on, uint32_t held, float v, uint64_t from) {
        const uint64_t end = DirectEnd(k, on, held);
        const uint64_t open = processedEnd_ - (processedEnd_ ? block_ : 0u);   // start of the processed block
        if (from < open) from = open;   // already output: cannot change
        if (from >= end) return;
        Chan& h = chan_[channel & 15u];
        Group& g = Route(h, end + block_);
        DirectInto(g, k, on, held, v, from, 1.0f);
        if (!share_) return;
        h.dirs.push_back({k, on, from, held, v});
        if (h.dirs.size() >= 2u * h.dirsKeep) {
            h.dirs.erase(std::remove_if(h.dirs.begin(), h.dirs.end(),
                                        [&](const Dir& d) { return DirectEnd(d.k, d.on, d.held) <= outFrom_; }),
                         h.dirs.end());
            h.dirsKeep = (std::max)(size_t(64), h.dirs.size());
        }
    }

    // The channel's mix gains from `frame` on (a voice's mixScaleLeft / mixScaleRight).
    void SetMix(uint8_t channel, uint64_t frame, float left, float right) {
        Chan& h = chan_[channel & 15u];
        if (left == h.L && right == h.R) return;
        if (h.pending && frame != h.changed && Live(h)) MoveOut(h, true, outFrom_);   // the group missed its last change
        if (frame != h.changed) { h.prevL = h.L; h.prevR = h.R; h.changed = frame; }
        h.L = left; h.R = right;
        if (!Live(h)) return;
        if (!share_ || Alone(h)) { Push(*groups_[size_t(h.g)], frame, left, right); h.pending = false; return; }
        h.pending = true;
        Follow(h.g);
    }

    // All sound off on a channel at `frame`: its tails stop there, as voices do. Until the cut's block is
    // processed the channel takes no new notes.
    void Cut(uint8_t channel, uint64_t frame) {
        Chan& h = chan_[channel & 15u];
        if (!Live(h)) return;
        if (share_ && !Alone(h)) {
            if (frame < processedEnd_) MoveOut(h, false, frame);
            else h.cutAt = (std::min)(h.cutAt, frame);
            return;
        }
        Group& g = *groups_[size_t(h.g)];
        if (frame < processedEnd_) {
            // The block is already processed: silence what is not yet output, drop the rest.
            const uint64_t from = frame > processedEnd_ - block_ ? frame - (processedEnd_ - block_) : 0u;
            for (int s = 0; s < 2; ++s) std::fill(g.pre[s].begin() + static_cast<std::ptrdiff_t>(from), g.pre[s].end(), 0.0f);
            Drop(h.g, frame);
        } else {
            g.cutAt = (std::min)(g.cutAt, frame);
        }
    }

    // Drops every tail from `frame` on (reset, SoundFont change).
    void Reset(uint64_t frame) {
        for (size_t i = 0; i < groups_.size(); ++i) {
            Group& g = *groups_[i];
            Drop(int(i), 0u);
            for (int s = 0; s < 2; ++s) std::fill(g.pre[s].begin(), g.pre[s].end(), 0.0f);
            g.changes.clear();
            g.next = 0;
            g.active = g.preLive = false;
        }
        for (Chan& h : chan_) { Forget(h); h.g = -1; h.until = 0; h.changed = 0; }
        processedEnd_ = block_ ? frame / block_ * block_ : 0u;
        if (block_ && frame % block_) processedEnd_ += block_;   // a block cut mid-way is not reopened
        outFrom_ = frame;
    }

    // Adds the bus output for [frame, frame + n) to left/right. Calls must be contiguous; every event before
    // frame + n is in.
    void Render(float* left, float* right, uint32_t n, uint64_t frame) {
        if (!block_) return;
        Settle();
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
            for (auto& gp : groups_) {
                Group& g = *gp;
                if (!g.preLive) { ApplyChanges(g, f + take); continue; }
                for (uint32_t i = 0; i < take; ++i) {
                    const uint64_t at = f + i;
                    while (g.next < g.changes.size() && g.changes[g.next].frame <= at) {
                        g.mixL = g.changes[g.next].left; g.mixR = g.changes[g.next].right; ++g.next;
                    }
                    left[done + i] += g.pre[0][off + i] * g.mixL;
                    right[done + i] += g.pre[1][off + i] * g.mixR;
                }
                TrimChanges(g);
            }
            done += take;
        }
        outFrom_ = frame + n;
    }

    size_t Histories() const noexcept {
        size_t n = 0;
        for (const auto& g : groups_) n += g->inputs.size();
        return n;
    }
    uint64_t Notes() const noexcept { return notes_; }   // same-block notes
    uint64_t Blocks() const noexcept { return blocks_; }
    uint64_t Convolved() const noexcept { return convolved_; }   // input-blocks
    uint64_t BusNs() const noexcept { return busNs_; }
    uint64_t Moves() const noexcept { return moves_; }   // channels moved out of a shared group with their tails

private:
    struct In { const ReleaseKernel* kernel; int id; uint64_t lastInput; };
    struct Change { uint64_t frame; float left, right; };
    struct Group {
        std::vector<float> pre[2];   // the processed block's sum before the mix gains
        std::vector<float> dir[2];   // direct sound of later frames, frame f at f % ring
        ConvBank bank;
        std::unordered_map<uint64_t, In> inputs;   // kernel key -> bank input
        std::vector<Change> changes;
        size_t next = 0;
        float mixL = 0.0f, mixR = 0.0f;   // the gains being applied
        float L = 0.0f, R = 0.0f, prevL = 0.0f, prevR = 0.0f;   // the last gains set, and the ones before
        uint64_t changed = 0;   // the frame of the last change
        uint64_t cutAt = UINT64_MAX;
        uint64_t until = 0;     // no sound from here on
        bool active = false, preLive = false;
    };
    // A channel's sound in its group, kept while sharing so it can move: impulses per kernel, direct parts.
    struct Hist { const ReleaseKernel* k = nullptr; std::vector<std::pair<uint64_t, float>> imps; size_t keep = 64; };
    struct Dir { const ReleaseKernel* k; uint64_t on, from; uint32_t held; float v; };
    struct Chan {
        float L = 0.0f, R = 0.0f, prevL = 0.0f, prevR = 0.0f;   // as Group's
        uint64_t changed = 0;
        int g = -1;             // the group its sound is in
        uint64_t until = 0;     // its sound there ends before this
        bool pending = false;   // its last change (at `changed`) is not yet the group's
        uint64_t cutAt = UINT64_MAX;   // a cut in a shared group, done when its block is processed
        std::unordered_map<uint64_t, Hist> hist;
        std::vector<Dir> dirs;
        size_t dirsKeep = 64;
    };

    void NewGroup() {
        auto g = std::make_unique<Group>();
        g->pre[0].assign(block_, 0.0f); g->pre[1].assign(block_, 0.0f);
        g->dir[0].assign(ring_, 0.0f); g->dir[1].assign(ring_, 0.0f);
        g->bank.SetParallel(par_, parUser_, lanes_);
        g->bank.Prepare(static_cast<int>(block_));
        groups_.push_back(std::move(g));
    }
    In& InputOf(Group& g, uint64_t kernelKey, const ReleaseKernel* k) {
        auto it = g.inputs.find(kernelKey);
        if (it == g.inputs.end()) it = g.inputs.emplace(kernelKey, In{k, g.bank.Add(k->tail), 0u}).first;
        return it->second;
    }
    // With level 0 the convolution covers every frame and the direct part only corrects the held ones.
    uint64_t DirectEnd(const ReleaseKernel* k, uint64_t on, uint32_t held) const noexcept {
        if (level0_) return on + held;
        return on + (std::min)(static_cast<uint64_t>(Direct()), static_cast<uint64_t>(k->heldFrames));
    }
    void Prune(Hist& hs) const {
        const uint64_t len = static_cast<uint64_t>(hs.k->tail.length) + block_;
        hs.imps.erase(std::remove_if(hs.imps.begin(), hs.imps.end(),
                                     [&](const std::pair<uint64_t, float>& x) { return x.first + len <= outFrom_; }),
                      hs.imps.end());
    }
    static void Forget(Chan& h) {
        h.hist.clear();
        h.dirs.clear();
        h.pending = false;
        h.cutAt = UINT64_MAX;
    }

    bool Live(const Chan& h) const noexcept { return h.g >= 0 && h.until > outFrom_; }
    bool CutPending(const Chan& h) const noexcept {
        return Live(h) && (h.cutAt != UINT64_MAX || groups_[size_t(h.g)]->cutAt != UINT64_MAX);
    }
    bool Alone(const Chan& h) const noexcept {
        for (const Chan& d : chan_) if (&d != &h && Live(d) && d.g == h.g) return false;
        return true;
    }
    // Gains that stay put from the next frame to output on.
    template <class T> bool Steady(const T& x) const noexcept {
        return x.changed <= outFrom_ || (x.prevL == x.L && x.prevR == x.R);
    }
    // Whether the group's gains and the channel's agree on every frame from the next frame to output on.
    bool Matches(const Group& g, const Chan& h) const noexcept {
        if (g.L != h.L || g.R != h.R) return false;
        const bool gs = Steady(g), hs = Steady(h);
        if (gs || hs) return gs && hs;
        return g.changed == h.changed && g.prevL == h.prevL && g.prevR == h.prevR;
    }
    bool HasPending(int gi) const noexcept {
        for (const Chan& d : chan_) if (Live(d) && d.g == gi && d.pending) return true;
        return false;
    }

    // The group of the channel's next sound, which lasts until `end`.
    Group& Route(Chan& h, uint64_t end) {
        if (!Live(h)) { Forget(h); h.g = Pick(h, -1); h.until = 0; }
        h.until = (std::max)(h.until, end);
        Group& g = *groups_[size_t(h.g)];
        g.until = (std::max)(g.until, end);
        return g;
    }
    // A group for the channel's gains: one that has them, else a silent one set to them. Never `except`.
    int Pick(const Chan& h, int except) {
        int free = -1;
        for (size_t i = 0; i < groups_.size(); ++i) {
            if (int(i) == except) continue;
            const Group& g = *groups_[i];
            if (g.until <= outFrom_) { if (free < 0) free = int(i); continue; }
            if (share_ && g.cutAt == UINT64_MAX && Matches(g, h) && !HasPending(int(i))) return int(i);
        }
        if (free < 0) { free = int(groups_.size()); NewGroup(); }
        Group& g = *groups_[size_t(free)];
        g.changes.clear();
        g.next = 0;
        g.cutAt = UINT64_MAX;
        g.L = h.L; g.R = h.R; g.prevL = h.prevL; g.prevR = h.prevR; g.changed = h.changed;
        if (Steady(h)) { g.mixL = h.L; g.mixR = h.R; }
        else { g.mixL = h.prevL; g.mixR = h.prevR; g.changes.push_back({h.changed, h.L, h.R}); }
        return free;
    }
    void Push(Group& g, uint64_t frame, float left, float right) {
        if (!g.changes.empty() && g.changes.back().frame > frame) { g.changes.clear(); g.next = 0; }
        g.changes.push_back({frame, left, right});
        if (frame != g.changed) { g.prevL = g.L; g.prevR = g.R; g.changed = frame; }
        g.L = left; g.R = right;
    }
    // The group follows its channels' change once all of them made it.
    void Follow(int gi) {
        const Chan* first = nullptr;
        for (const Chan& d : chan_) {
            if (!Live(d) || d.g != gi) continue;
            if (!d.pending) return;
            if (!first) first = &d;
            else if (d.changed != first->changed || d.L != first->L || d.R != first->R) return;
        }
        if (!first) return;
        Push(*groups_[size_t(gi)], first->changed, first->L, first->R);
        for (Chan& d : chan_) if (Live(d) && d.g == gi) d.pending = false;
    }
    // Every event up to the frames about to be output is in: a change its group did not follow moves the channel.
    void Settle() {
        for (Chan& h : chan_) {
            if (!Live(h) || !h.pending) continue;
            Follow(h.g);
            if (h.pending) MoveOut(h, true, outFrom_);
        }
    }

    // Takes the channel's sound from `from` on out of its group: into a group of its gains (`keep`), or nowhere (a
    // cut). Frames before `from` stay; `from` is not before the processed block.
    void MoveOut(Chan& h, bool keep, uint64_t from) {
        const int gi = h.g;
        Group& g = *groups_[size_t(gi)];
        const int hi = keep ? Pick(h, gi) : -1;
        Group* to = keep ? groups_[size_t(hi)].get() : nullptr;
        const uint64_t open = processedEnd_ - (processedEnd_ ? block_ : 0u);
        if (from < open) from = open;
        const uint32_t offset = static_cast<uint32_t>((std::min)(from - open, static_cast<uint64_t>(block_)));
        float* const gPre[2] = {g.pre[0].data(), g.pre[1].data()};
        float* const tPre[2] = {to ? to->pre[0].data() : nullptr, to ? to->pre[1].data() : nullptr};
        for (auto& [key, hs] : h.hist) {
            Prune(hs);
            if (hs.imps.empty()) continue;
            auto it = g.inputs.find(key);
            if (it == g.inputs.end()) continue;   // expired with its impulses
            int toId = -1;
            if (to) {
                In& in = InputOf(*to, key, hs.k);
                for (const auto& x : hs.imps) in.lastInput = (std::max)(in.lastInput, x.first);
                toId = in.id;
                to->active = true;
            }
            ConvBank::Move(g.bank, it->second.id, to ? &to->bank : nullptr, toId, hs.imps.data(), hs.imps.size(),
                           processedEnd_ / block_, offset, gPre, tPre);
        }
        for (const Dir& d : h.dirs) {
            const uint64_t f = (std::max)(d.from, from);
            DirectInto(g, d.k, d.on, d.held, d.v, f, -1.0f);
            if (to) DirectInto(*to, d.k, d.on, d.held, d.v, f, 1.0f);
        }
        g.preLive = true;
        if (to) {
            to->preLive = true;
            to->until = (std::max)(to->until, h.until);
            h.g = hi;
            h.pending = false;
        } else {
            Forget(h);
            h.g = -1;
        }
        ++moves_;
    }

    // Frames [from, end of the note's direct span) of a note's direct part, times `sign`.
    void DirectInto(Group& g, const ReleaseKernel* k, uint64_t on, uint32_t held, float v, uint64_t from, float sign) {
        const uint64_t end = DirectEnd(k, on, held);
        const uint64_t open = processedEnd_ - (processedEnd_ ? block_ : 0u);
        if (from < open) from = open;
        if (from >= end) return;
        const double r = k->releaseDecay;
        uint32_t t = static_cast<uint32_t>(from - on);
        // r^(tau - held + 1) at the first released frame on or after `from`; with level 0, at `from` itself.
        double p = std::pow(r, static_cast<double>(level0_ ? t : (std::max)(t, held)) - static_cast<double>(held) + 1.0);
        uint32_t slot = static_cast<uint32_t>(from % ring_);
        for (uint64_t f = from; f < end; ++f, ++t, slot = slot + 1u == ring_ ? 0u : slot + 1u) {
            // Level 0: while held the convolution gives w B = v A r^(tau - held + 1) where the note is v A.
            const float gain = static_cast<float>(level0_ ? v * (1.0 - p) : t < held ? v : v * p) * sign;
            if (level0_ || t >= held) p *= r;
            const float l = k->heldL[t] * gain, rr = k->heldR[t] * gain;
            if (f < processedEnd_) {
                g.pre[0][static_cast<size_t>(f - open)] += l;
                g.pre[1][static_cast<size_t>(f - open)] += rr;
                g.preLive = true;
            } else {
                g.dir[0][slot] += l;
                g.dir[1][slot] += rr;
            }
        }
        g.active = true;
    }

    void Drop(int gi, uint64_t soundUntil) {
        Group& g = *groups_[size_t(gi)];
        g.bank.RemoveAll();
        g.inputs.clear();
        for (int s = 0; s < 2; ++s) std::fill(g.dir[s].begin(), g.dir[s].end(), 0.0f);
        g.cutAt = UINT64_MAX;
        g.until = soundUntil;   // the processed block's frames before a cut still play
        for (Chan& h : chan_) if (h.g == gi) { Forget(h); h.g = -1; }
    }

    void ApplyChanges(Group& g, uint64_t upTo) {
        while (g.next < g.changes.size() && g.changes[g.next].frame < upTo) {
            g.mixL = g.changes[g.next].left; g.mixR = g.changes[g.next].right; ++g.next;
        }
        TrimChanges(g);
    }
    static void TrimChanges(Group& g) {
        if (g.next == g.changes.size()) { g.changes.clear(); g.next = 0; }
    }

    void ProcessBlock() {
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t blockStart = processedEnd_;
        const uint64_t b = blockStart / block_;
        ++blocks_;
        for (size_t gi = 0; gi < groups_.size(); ++gi) {
            Group& g = *groups_[gi];
            bool live = g.active;
            g.active = false;
            const size_t at = static_cast<size_t>(blockStart % ring_);   // ring_ is whole blocks
            for (int s = 0; s < 2; ++s) {
                std::copy(g.dir[s].begin() + static_cast<std::ptrdiff_t>(at),
                          g.dir[s].begin() + static_cast<std::ptrdiff_t>(at + block_), g.pre[s].begin());
                std::fill(g.dir[s].begin() + static_cast<std::ptrdiff_t>(at),
                          g.dir[s].begin() + static_cast<std::ptrdiff_t>(at + block_), 0.0f);
            }
            convolved_ += g.inputs.size();
            // Every block, inputs or not: outputs made earlier may still be due, and the bank's chunks follow the grid.
            live = g.bank.Process(g.pre[0].data(), g.pre[1].data(), b) || live;
            if (g.cutAt < blockStart + block_) {
                const uint64_t from = g.cutAt > blockStart ? g.cutAt - blockStart : 0u;
                for (int s = 0; s < 2; ++s) std::fill(g.pre[s].begin() + static_cast<std::ptrdiff_t>(from), g.pre[s].end(), 0.0f);
                Drop(int(gi), g.cutAt);
            } else {
                for (auto it = g.inputs.begin(); it != g.inputs.end();) {
                    const In& in = it->second;
                    if (blockStart + block_ >= in.lastInput + static_cast<uint64_t>(in.kernel->tail.length) + block_) {
                        g.bank.Remove(in.id);
                        it = g.inputs.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (!g.inputs.empty()) g.active = true;
            // Exactly-zero groups skip the mix loop.
            g.preLive = false;
            if (live)
                for (int s = 0; s < 2 && !g.preLive; ++s)
                    for (float x : g.pre[s]) if (x != 0.0f) { g.preLive = true; break; }
        }
        processedEnd_ = blockStart + block_;
        // Cuts in shared groups: the channel's tails go from the cut's frame on.
        for (Chan& h : chan_)
            if (h.cutAt < processedEnd_) {
                if (Live(h)) MoveOut(h, false, h.cutAt);
                h.cutAt = UINT64_MAX;
            }
        busNs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }

    uint32_t block_ = 0, ring_ = 1;
    uint64_t processedEnd_ = 0;    // the grid block before this is processed; this one is open
    uint64_t outFrom_ = 0;         // the next frame Render outputs
    bool share_ = true;
    bool level0_ = false;
    ConvBank::ParallelFn par_ = nullptr;
    void* parUser_ = nullptr;
    uint32_t lanes_ = 1;
    std::vector<std::unique_ptr<Group>> groups_;
    Chan chan_[kChannels];
    uint64_t notes_ = 0, blocks_ = 0, convolved_ = 0, busNs_ = 0, moves_ = 0;
};

}  // namespace bus
}  // namespace svms
