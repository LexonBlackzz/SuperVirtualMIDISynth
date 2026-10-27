// Voice-allocation policy lab. Replays the note stream of a MIDI file through
// candidate allocation policies with a fake pool and no audio, and reports how
// much audible level each policy gives up compared with the exact policy
// (steal the quietest voice at the note's own frame).
//
// The voice model is deliberately small: a note starts at a level set by its
// velocity, decays exponentially while held, and decays faster once released.
// Within one class (held, released) every voice decays at the same rate, so the
// ordering inside a class never changes and each class is a plain heap keyed on
// a constant. That keeps the exact policy exact without scanning the pool.
//
//   svms_policy_lab --midi file.mid [--cap 4096] [--block 2048]
//       [--policies exact,epoch,epoch-eph] [--max-seconds 60] [--out prefix]
#include "events/MidiStream.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace {

constexpr double kSampleRate = 44100.0;
constexpr double kNever = 1e30;

struct Params {
    uint32_t cap = 4096;
    uint32_t block = 2048;
    double tauHold = 8.0;      // seconds, held decay time constant
    double tauRelease = 0.25;  // seconds, release decay time constant
    double floorLevel = 0.00015;
    bool reject = true;
    double maxSeconds = kNever;
    uint64_t maxNotes = 60000000;
    uint32_t top = 300;
    double dumpStart = -1, dumpLength = 0;
    std::string out = "policy_lab";
} g;

double LevelOfVelocity(uint8_t v) {
    const double x = v / 127.0;
    return x * x;
}

struct Note {
    uint64_t frame;
    uint8_t channel, key, velocity;
};
std::vector<Note> g_notes;

struct Ev {
    uint64_t frame;
    uint32_t note;
    uint8_t off, channel, key;
    bool ephemeral;
};

// Audible level-seconds of a note that starts at t0 with level L0, is released
// at tr and stops at tc (or when it falls under the floor, whichever is first).
double Audibility(double L0, double t0, double tr, double tc) {
    if (L0 <= g.floorLevel || tc <= t0) return 0;
    const double td = t0 + g.tauHold * std::log(L0 / g.floorLevel);
    const double he = std::min(std::min(tr, td), tc);
    double a = g.tauHold * (L0 - L0 * std::exp(-(he - t0) / g.tauHold));
    if (tr < td && tr < tc) {
        const double lr = L0 * std::exp(-(tr - t0) / g.tauHold);
        const double end = std::min(tc, tr + g.tauRelease * std::log(lr / g.floorLevel));
        a += g.tauRelease * (lr - lr * std::exp(-(end - tr) / g.tauRelease));
    }
    return a;
}

enum : uint8_t { kHeld, kReleased, kCutPending, kDead, kFree };

struct Rec {
    double L0, t0, tr, tcut;
    uint32_t note, gen;
    uint8_t state;
    bool inHeap;
};

struct HeapEntry {
    double key;
    uint32_t id, gen;
};
bool HeapGreater(const HeapEntry& a, const HeapEntry& b) { return a.key > b.key; }

class Policy {
public:
    virtual ~Policy() {}
    virtual const char* Name() const = 0;
    virtual void Block(uint64_t start, std::vector<Ev>& evs) = 0;

    void Init(size_t noteCapacity) {
        loss.assign(noteCapacity, 0.0f);
        fate.assign(noteCapacity, 0);
        cutHeldAt.assign(noteCapacity, 0.0f);
        keyq.resize(2048);
    }
    void Finish(double tEnd) {
        for (uint32_t id = 0; id < recs.size(); ++id) {
            Rec& r = recs[id];
            if (r.state == kCutPending) ResolveCut(id, tEnd);
        }
    }
    void RunBlock(uint64_t start, std::vector<Ev>& evs, uint64_t noteOns) {
        const uint64_t t0 = __rdtsc();
        Block(start, evs);
        cycles += __rdtsc() - t0;
        noteOnCount += noteOns;
        series.push_back(active);
    }

    std::vector<float> loss;
    std::vector<float> cutHeldAt;  // seconds, for notes cut while still held
    std::vector<uint8_t> fate;  // 0 played to its natural end, 1 cut, 2 dropped
    std::vector<uint32_t> series;
    double totalLoss = 0, loudLoss = 0, grossAud = 0;
    uint64_t tailCutCount = 0, cycles = 0, noteOnCount = 0, cutCount = 0, dropCount = 0, ephCount = 0;
    uint32_t active = 0, maxActive = 0;

protected:
    // Levels are exp(key - t / tau), so a class heap stays ordered as time passes.
    double HeldKey(const Rec& r) const { return std::log(r.L0) + r.t0 / g.tauHold; }
    double RelKey(const Rec& r) const {
        const double lr = r.L0 * std::exp(-(r.tr - r.t0) / g.tauHold);
        return std::log(lr) + r.tr / g.tauRelease;
    }
    double LevelAt(const Rec& r, double t) const {
        if (r.state == kHeld) return r.L0 * std::exp(-(t - r.t0) / g.tauHold);
        const double lr = r.L0 * std::exp(-(r.tr - r.t0) / g.tauHold);
        return lr * std::exp(-(t - r.tr) / g.tauRelease);
    }
    void PushHeap(uint32_t id) {
        Rec& r = recs[id];
        r.inHeap = true;
        if (r.state == kHeld) {
            held.push_back({HeldKey(r), id, r.gen});
            std::push_heap(held.begin(), held.end(), HeapGreater);
        } else {
            rel.push_back({RelKey(r), id, r.gen});
            std::push_heap(rel.begin(), rel.end(), HeapGreater);
        }
    }
    uint32_t Alloc() {
        if (!freeIds.empty()) {
            const uint32_t id = freeIds.back();
            freeIds.pop_back();
            return id;
        }
        recs.push_back({});
        return uint32_t(recs.size() - 1);
    }
    void FreeRec(uint32_t id) {
        recs[id].state = kFree;
        ++recs[id].gen;
        freeIds.push_back(id);
    }
    void Account(uint32_t note, double l) {
        loss[note] = float(l);
        totalLoss += l;
        if (g_notes[note].velocity >= 64) loudLoss += l;
    }

    uint32_t Spawn(uint32_t note, const Ev& e, bool intoHeap) {
        const uint32_t id = Alloc();
        Rec& r = recs[id];
        r.L0 = LevelOfVelocity(g_notes[note].velocity);
        r.t0 = e.frame / kSampleRate;
        r.tr = kNever;
        r.tcut = 0;
        r.note = note;
        r.gen = r.gen + 1;
        r.state = kHeld;
        r.inHeap = false;
        keyq[e.channel * 128 + e.key].push_back(int32_t(id));
        ++active;
        maxActive = std::max(maxActive, active);
        if (intoHeap) PushHeap(id);
        return id;
    }
    void NoteOff(double t, const Ev& e) {
        auto& q = keyq[e.channel * 128 + e.key];
        if (q.empty()) return;
        const uint32_t id = uint32_t(q.front());
        q.pop_front();
        Rec& r = recs[id];
        grossAud += Audibility(r.L0, r.t0, t, kNever);
        switch (r.state) {
        case kHeld:
            r.state = kReleased;
            r.tr = t;
            ++r.gen;
            if (r.inHeap) PushHeap(id);
            break;
        case kCutPending: ResolveCut(id, t); break;
        case kDead: FreeRec(id); break;
        default: break;
        }
    }
    void ResolveCut(uint32_t id, double tr) {
        Rec& r = recs[id];
        const double full = Audibility(r.L0, r.t0, tr, kNever);
        const double got = Audibility(r.L0, r.t0, tr, r.tcut);
        Account(r.note, std::max(0.0, full - got));
        FreeRec(id);
    }
    // Returns false when the voice had already died on its own at t.
    bool Cut(uint32_t id, double t, uint8_t how = 1) {
        Rec& r = recs[id];
        if (LevelAt(r, t) < g.floorLevel) {
            --active;
            if (r.state == kHeld) {
                r.state = kDead;
                ++r.gen;
            } else {
                FreeRec(id);
            }
            return false;
        }
        fate[r.note] = how;
        (how == 1 ? cutCount : dropCount) += 1;
        --active;
        if (r.state == kHeld) {
            r.state = kCutPending;
            r.tcut = t;
            cutHeldAt[r.note] = float(t);
            ++r.gen;
        } else {
            ++tailCutCount;
            const double l = LevelAt(r, t);
            Account(r.note, g.tauRelease * (l - g.floorLevel));
            FreeRec(id);
        }
        return true;
    }
    void Drop(const Ev& e) {
        const uint32_t id = Spawn(e.note, e, false);
        Cut(id, e.frame / kSampleRate, 2);
    }
    bool CleanTop(std::vector<HeapEntry>& h) {
        while (!h.empty()) {
            const HeapEntry& top = h.front();
            const Rec& r = recs[top.id];
            if (r.gen == top.gen && (r.state == kHeld || r.state == kReleased)) return true;
            std::pop_heap(h.begin(), h.end(), HeapGreater);
            h.pop_back();
        }
        return false;
    }
    // Voices under the retire floor free their slot without being counted as cuts.
    void Retire(double t) {
        for (int c = 0; c < 2; ++c) {
            std::vector<HeapEntry>& h = c == 0 ? held : rel;
            const double tau = c == 0 ? g.tauHold : g.tauRelease;
            while (CleanTop(h)) {
                const HeapEntry top = h.front();
                if (std::exp(top.key - t / tau) >= g.floorLevel) break;
                std::pop_heap(h.begin(), h.end(), HeapGreater);
                h.pop_back();
                Rec& r = recs[top.id];
                --active;
                if (r.state == kHeld) {
                    r.state = kDead;
                    ++r.gen;
                } else {
                    FreeRec(top.id);
                }
            }
        }
    }
    bool Best(double t, double& level, bool& fromHeld) {
        const bool h = CleanTop(held), r = CleanTop(rel);
        if (!h && !r) return false;
        const double lh = h ? std::exp(held.front().key - t / g.tauHold) : 1e300;
        const double lr = r ? std::exp(rel.front().key - t / g.tauRelease) : 1e300;
        fromHeld = lh <= lr;
        level = fromHeld ? lh : lr;
        return true;
    }
    uint32_t PopBest(bool fromHeld) {
        std::vector<HeapEntry>& h = fromHeld ? held : rel;
        const uint32_t id = h.front().id;
        std::pop_heap(h.begin(), h.end(), HeapGreater);
        h.pop_back();
        recs[id].inHeap = false;
        return id;
    }

    std::vector<Rec> recs;
    std::vector<uint32_t> freeIds;
    std::vector<HeapEntry> held, rel;
    std::vector<std::deque<int32_t>> keyq;
};

// The reference: the quietest voice at the moment of the note-on goes.
class ExactPolicy final : public Policy {
public:
    const char* Name() const override { return "exact"; }
    void Block(uint64_t, std::vector<Ev>& evs) override {
        for (const Ev& e : evs) {
            const double t = e.frame / kSampleRate;
            if (e.off) {
                NoteOff(t, e);
                continue;
            }
            Retire(t);
            if (active < g.cap) {
                Spawn(e.note, e, true);
                continue;
            }
            double level;
            bool fromHeld;
            if (!Best(t, level, fromHeld) ||
                (g.reject && level >= LevelOfVelocity(g_notes[e.note].velocity))) {
                Drop(e);
                continue;
            }
            Cut(PopBest(fromHeld), t);
            Spawn(e.note, e, true);
        }
    }
};

// Victims for the whole block are picked once at the block start, ascending by
// level, and handed out in note order. Notes born in the block are protected
// from stealing until the block ends, except when the block has more note-ons
// than the pool has voices.
class EpochPolicy final : public Policy {
public:
    EpochPolicy(bool ephemeral, bool byRank, bool chain = false)
        : ephemeral_(ephemeral), byRank_(byRank), chain_(chain) {}
    const char* Name() const override {
        if (chain_) return "rank-chain";
        if (byRank_) return ephemeral_ ? "rank-eph" : "rank";
        return ephemeral_ ? "epoch-eph" : "epoch";
    }

    void Block(uint64_t start, std::vector<Ev>& evs) override {
        const double tb = start / kSampleRate;
        const double te = (start + g.block) / kSampleRate;
        Retire(tb);
        if (ephemeral_) MarkEphemeral(evs);
        else for (Ev& e : evs) e.ephemeral = false;
        uint32_t wanted = 0;
        for (const Ev& e : evs) wanted += (!e.off && !e.ephemeral);
        uint32_t freeSlots = g.cap > active ? g.cap - active : 0;
        const uint32_t need = wanted > freeSlots ? wanted - freeSlots : 0;
        ClaimVictims(tb, need);
        size_t next = 0;
        born_.clear();
        newborn_.clear();
        chainPrev_ = ~0u;
        // Rank pairing: the loudest note takes the emptiest slot or quietest
        // victim, whatever its position in the block, so survival does not
        // depend on where in the block a note lands.
        const uint32_t freeCount = freeSlots;
        if (byRank_) {
            order_.clear();
            for (size_t i = 0; i < evs.size(); ++i)
                if (!evs[i].off && !evs[i].ephemeral) order_.push_back(uint32_t(i));
            std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
                const uint8_t va = g_notes[evs[a].note].velocity, vb = g_notes[evs[b].note].velocity;
                if (va != vb) return va > vb;
                return Mix(evs[a].note) < Mix(evs[b].note);  // equal levels: no positional preference
            });
            slot_.assign(evs.size(), -1);
            for (size_t r = 0; r < order_.size(); ++r)
                if (r < size_t(freeCount) + victims_.size()) slot_[order_[r]] = int32_t(r);
            used_.assign(victims_.size(), 0);
        }
        for (size_t evIndex = 0; evIndex < evs.size(); ++evIndex) {
            const Ev& e = evs[evIndex];
            const double t = e.frame / kSampleRate;
            if (e.off) {
                NoteOff(t, e);
                continue;
            }
            if (e.ephemeral) {
                ++ephCount;
                born_.push_back(Spawn(e.note, e, false));
                continue;
            }
            const double ln = LevelOfVelocity(g_notes[e.note].velocity);
            if (byRank_) {
                const int32_t c = slot_[evIndex];
                if (c < 0) {
                    LoseSlot(e);
                } else if (uint32_t(c) < freeCount) {
                    Born(Spawn(e.note, e, false));
                } else {
                    const size_t vi = size_t(c) - freeCount;
                    const double lv = LevelAt(recs[victims_[vi]], t);
                    if (lv >= g.floorLevel && g.reject && lv >= ln) {
                        LoseSlot(e);
                    } else {
                        used_[vi] = 1;
                        Cut(victims_[vi], t);
                        Born(Spawn(e.note, e, false));
                    }
                }
                continue;
            }
            if (freeSlots > 0) {
                --freeSlots;
                Born(Spawn(e.note, e, false));
                continue;
            }
            bool placed = false;
            while (next < victims_.size()) {
                const uint32_t id = victims_[next];
                const double lv = LevelAt(recs[id], t);
                if (lv >= g.floorLevel && g.reject && lv >= ln) {
                    Drop(e);
                    placed = true;
                    break;
                }
                ++next;
                Cut(id, t);
                Born(Spawn(e.note, e, false));
                placed = true;
                break;
            }
            if (placed) continue;
            // More note-ons than pool voices: the quietest newborn makes room.
            while (!newborn_.empty()) {
                const auto top = newborn_.front();
                const Rec& r = recs[top.second];
                if (r.gen == top.first && r.state == kHeld) break;
                std::pop_heap(newborn_.begin(), newborn_.end(), NewbornLess(this));
                newborn_.pop_back();
            }
            if (newborn_.empty() ||
                (g.reject && LevelOfVelocity(g_notes[recs[newborn_.front().second].note].velocity) >= ln)) {
                Drop(e);
                continue;
            }
            const uint32_t id = newborn_.front().second;
            std::pop_heap(newborn_.begin(), newborn_.end(), NewbornLess(this));
            newborn_.pop_back();
            Cut(id, t);
            Born(Spawn(e.note, e, false));
        }
        for (size_t i = byRank_ ? 0 : next; i < victims_.size(); ++i)
            if (!byRank_ || !used_[i]) Reinsert(victims_[i]);
        for (uint32_t id : born_) Reinsert(id);
        if (ephemeral_) {
            Retire(te);
            double level;
            bool fromHeld;
            while (active > g.cap && Best(te, level, fromHeld)) Cut(PopBest(fromHeld), te);
        }
    }

private:
    // A note that wins no slot is dropped, or in the ephemeral variants plays to
    // the end of its block and is cut there if it is still the quietest.
    static uint32_t Mix(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        return x ^ (x >> 16);
    }
    void LoseSlot(const Ev& e) {
        if (!ephemeral_) {
            Drop(e);
            return;
        }
        ++ephCount;
        const uint32_t id = Spawn(e.note, e, false);
        born_.push_back(id);
        if (chain_) {
            // Each slotless note ends where the next one starts, as stealing the
            // previous newborn would; the last one joins the pool at block end.
            if (chainPrev_ != ~0u && recs[chainPrev_].state == kHeld && recs[chainPrev_].note != e.note)
                Cut(chainPrev_, e.frame / kSampleRate);
            chainPrev_ = id;
        }
    }
    using Newborn = std::pair<uint32_t, uint32_t>;  // gen, id (keyed by gen only for staleness)
    void Born(uint32_t id) {
        born_.push_back(id);
        newborn_.push_back({recs[id].gen, id});
        std::push_heap(newborn_.begin(), newborn_.end(), NewbornLess(this));
    }
    struct NewbornLess {
        explicit NewbornLess(EpochPolicy* p) : self(p) {}
        bool operator()(const Newborn& a, const Newborn& b) const {
            return self->recs[a.second].L0 > self->recs[b.second].L0;
        }
        EpochPolicy* self;
    };
    void Reinsert(uint32_t id) {
        Rec& r = recs[id];
        if (r.inHeap || (r.state != kHeld && r.state != kReleased)) return;
        PushHeap(id);
    }
    void MarkEphemeral(std::vector<Ev>& evs) {
        for (Ev& e : evs) e.ephemeral = false;
        // A note-on whose own note-off lands in this block needs no slot while
        // the block renders; it joins the pool at the block end as a release.
        std::vector<int32_t> open(2048, -1);
        for (size_t i = 0; i < evs.size(); ++i) {
            const size_t k = evs[i].channel * 128 + evs[i].key;
            if (!evs[i].off) {
                open[k] = int32_t(i);
            } else if (open[k] >= 0) {
                evs[size_t(open[k])].ephemeral = true;
                open[k] = -1;
            }
        }
    }
    void ClaimVictims(double tb, uint32_t need) {
        victims_.clear();
        if (!need) return;
        if (uint64_t(need) * 8 < held.size() + rel.size()) {
            double level;
            bool fromHeld;
            for (uint32_t i = 0; i < need && Best(tb, level, fromHeld); ++i) victims_.push_back(PopBest(fromHeld));
            return;
        }
        std::vector<Cand>& c = cand_;
        c.clear();
        for (int k = 0; k < 2; ++k) {
            const std::vector<HeapEntry>& h = k == 0 ? held : rel;
            const double tau = k == 0 ? g.tauHold : g.tauRelease;
            for (const HeapEntry& he : h) {
                const Rec& r = recs[he.id];
                if (r.gen == he.gen && (r.state == kHeld || r.state == kReleased))
                    c.push_back({std::exp(he.key - tb / tau), he.id});
            }
        }
        const size_t take = std::min<size_t>(need, c.size());
        auto less = [](const Cand& a, const Cand& b) { return a.level < b.level; };
        if (take < c.size()) std::nth_element(c.begin(), c.begin() + take, c.end(), less);
        std::sort(c.begin(), c.begin() + take, less);
        for (size_t i = 0; i < take; ++i) {
            victims_.push_back(c[i].id);
            recs[c[i].id].inHeap = false;
        }
        for (std::vector<HeapEntry>* h : {&held, &rel}) {
            h->erase(std::remove_if(h->begin(), h->end(), [&](const HeapEntry& he) {
                         const Rec& r = recs[he.id];
                         return r.gen != he.gen || !r.inHeap || (r.state != kHeld && r.state != kReleased);
                     }),
                     h->end());
            std::make_heap(h->begin(), h->end(), HeapGreater);
        }
    }

    bool ephemeral_, byRank_, chain_;
    uint32_t chainPrev_ = ~0u;
    std::vector<uint32_t> order_;
    std::vector<int32_t> slot_;
    std::vector<char> used_;
    std::vector<uint32_t> victims_, born_;
    std::vector<Newborn> newborn_;
    struct Cand {
        double level;
        uint32_t id;
    };
    std::vector<Cand> cand_;
};

std::vector<std::unique_ptr<Policy>> g_policies;
std::vector<Ev> g_block;
uint64_t g_blockIdx = ~0ull, g_blockNoteOns = 0, g_blocks = 0;
uint64_t g_lastFrame = 0;
bool g_stop = false;

void FlushBlock() {
    if (g_block.empty()) return;
    for (auto& p : g_policies) p->RunBlock(g_blockIdx * g.block, g_block, g_blockNoteOns);
    ++g_blocks;
    g_block.clear();
    g_blockNoteOns = 0;
}

bool OnEvent(const svms::PackedMidiEvent& pe, void*) {
    const uint8_t status = uint8_t(pe.message & 0xFF);
    if (status < 0x80 || status >= 0xA0) return true;
    if (pe.outputFrame / kSampleRate > g.maxSeconds || g_notes.size() >= g.maxNotes) {
        g_stop = true;
        return false;
    }
    const uint8_t channel = status & 0x0F, key = (pe.message >> 8) & 0x7F, vel = (pe.message >> 16) & 0x7F;
    const bool on = (status & 0xF0) == 0x90 && vel > 0;
    const uint64_t idx = pe.outputFrame / g.block;
    if (idx != g_blockIdx) {
        FlushBlock();
        g_blockIdx = idx;
    }
    g_lastFrame = pe.outputFrame;
    Ev e{pe.outputFrame, 0, uint8_t(on ? 0 : 1), channel, key, false};
    if (on) {
        e.note = uint32_t(g_notes.size());
        g_notes.push_back({pe.outputFrame, channel, key, vel});
        ++g_blockNoteOns;
    }
    g_block.push_back(e);
    return true;
}

std::unique_ptr<Policy> Make(const std::string& name) {
    if (name == "exact") return std::make_unique<ExactPolicy>();
    if (name == "epoch") return std::make_unique<EpochPolicy>(false, false);
    if (name == "epoch-eph") return std::make_unique<EpochPolicy>(true, false);
    if (name == "rank") return std::make_unique<EpochPolicy>(false, true);
    if (name == "rank-eph") return std::make_unique<EpochPolicy>(true, true);
    if (name == "rank-chain") return std::make_unique<EpochPolicy>(true, true, true);
    return nullptr;
}

// Listening dump: the events of one window as a MIDI file containing only what
// a policy actually plays (dropped notes removed, notes cut while held ended at
// their cut time). Everything else passes through, so the sound font programs
// and controllers are unchanged.
struct WindowPass {
    const Policy* policy;
    std::vector<std::pair<uint64_t, uint32_t>> out;
    std::vector<std::deque<uint32_t>> pending = std::vector<std::deque<uint32_t>>(2048);
    uint32_t note = 0;
    uint64_t begin, end;
};

bool OnWindowEvent(const svms::PackedMidiEvent& pe, void* user) {
    WindowPass& w = *static_cast<WindowPass*>(user);
    const uint8_t status = uint8_t(pe.message & 0xFF);
    if (pe.outputFrame > w.end + 44100u * 4) return false;
    const bool inside = pe.outputFrame >= w.begin && pe.outputFrame <= w.end;
    if (status >= 0x80 && status < 0xA0) {
        const uint8_t ch = status & 0x0F, key = (pe.message >> 8) & 0x7F, vel = (pe.message >> 16) & 0x7F;
        auto& q = w.pending[ch * 128 + key];
        if ((status & 0xF0) == 0x90 && vel > 0) {
            const uint32_t id = w.note++;
            q.push_back(id);
            if (inside && w.policy->fate[id] != 2) w.out.push_back({pe.outputFrame, pe.message});
        } else if (!q.empty()) {
            const uint32_t id = q.front();
            q.pop_front();
            const bool cutHeld = w.policy->fate[id] == 1 && w.policy->cutHeldAt[id] > 0;
            if (cutHeld) {
                const uint64_t cutFrame = uint64_t(double(w.policy->cutHeldAt[id]) * kSampleRate);
                if (cutFrame >= w.begin && cutFrame <= w.end)
                    w.out.push_back({cutFrame, uint32_t(0x80 | ch) | (uint32_t(key) << 8)});
            } else if (inside && w.policy->fate[id] != 2) {
                w.out.push_back({pe.outputFrame, pe.message});
            }
        }
    } else if (inside && (status & 0xF0) >= 0xB0 && (status & 0xF0) <= 0xE0) {
        w.out.push_back({pe.outputFrame, pe.message});
    }
    return true;
}

void WriteVlq(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t t[5];
    int n = 0;
    t[n++] = v & 0x7F;
    while (v >>= 7) t[n++] = uint8_t((v & 0x7F) | 0x80);
    while (n) b.push_back(t[--n]);
}

bool WriteWindow(const Policy& p, const svms::MappedMidiFile& file, const std::string& path) {
    WindowPass w;
    w.policy = &p;
    w.begin = uint64_t(g.dumpStart * kSampleRate);
    w.end = w.begin + uint64_t(g.dumpLength * kSampleRate);
    std::string error;
    svms::ParallelMidiDecoder dec;
    svms::MidiStreamDecoder small;
    if (file.Size() < (200u << 20)) small.Decode(file, uint32_t(kSampleRate), OnWindowEvent, &w, nullptr, nullptr, error);
    else dec.Decode(file, uint32_t(kSampleRate), OnWindowEvent, &w, nullptr, nullptr, error);
    for (int ch = 0; ch < 16; ++ch) w.out.push_back({w.end + 44100u * 3, uint32_t(0xB0 | ch) | (123u << 8)});
    std::stable_sort(w.out.begin(), w.out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint8_t> trk = {0, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20};  // 0.5 s per quarter, 22050 ticks per quarter: one tick per frame at 44100
    uint64_t prev = 0;
    for (const auto& e : w.out) {
        const uint64_t tick = e.first - w.begin;
        WriteVlq(trk, uint32_t(tick - prev));
        prev = tick;
        const uint8_t st = uint8_t(e.second & 0xFF);
        trk.push_back(st);
        trk.push_back(uint8_t((e.second >> 8) & 0x7F));
        if ((st & 0xF0) != 0xC0 && (st & 0xF0) != 0xD0) trk.push_back(uint8_t((e.second >> 16) & 0x7F));
    }
    trk.insert(trk.end(), {0, 0xFF, 0x2F, 0});
    std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x56, 0x22, 'M', 'T', 'r', 'k'};
    for (int s = 24; s >= 0; s -= 8) f.push_back(uint8_t(trk.size() >> s));
    f.insert(f.end(), trk.begin(), trk.end());
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fwrite(f.data(), 1, f.size(), fp);
    std::fclose(fp);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring midi;
    std::string policies = "exact,epoch,epoch-eph";
    auto narrow = [](const wchar_t* w) {
        std::string s;
        for (; *w; ++w) s.push_back(char(*w));
        return s;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = narrow(argv[i]);
        auto next = [&]() { return i + 1 < argc ? narrow(argv[++i]) : std::string(); };
        if (a == "--midi" && i + 1 < argc) midi = argv[++i];
        else if (a == "--cap") g.cap = uint32_t(std::atoi(next().c_str()));
        else if (a == "--block") g.block = uint32_t(std::atoi(next().c_str()));
        else if (a == "--policies") policies = next();
        else if (a == "--tau-hold") g.tauHold = std::atof(next().c_str());
        else if (a == "--tau-release") g.tauRelease = std::atof(next().c_str());
        else if (a == "--max-seconds") g.maxSeconds = std::atof(next().c_str());
        else if (a == "--max-notes") g.maxNotes = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--no-reject") g.reject = false;
        else if (a == "--dump") {
            const std::string v = next();
            std::sscanf(v.c_str(), "%lf,%lf", &g.dumpStart, &g.dumpLength);
        }
        else if (a == "--top") g.top = uint32_t(std::atoi(next().c_str()));
        else if (a == "--out") g.out = next();
    }
    if (midi.empty()) {
        std::fprintf(stderr, "usage: svms_policy_lab --midi file.mid [--cap N] [--block N] [--policies a,b] "
                             "[--max-seconds S] [--out prefix]\n");
        return 2;
    }
    {
        size_t pos = 0;
        while (pos <= policies.size()) {
            size_t comma = policies.find(',', pos);
            if (comma == std::string::npos) comma = policies.size();
            auto p = Make(policies.substr(pos, comma - pos));
            if (p) g_policies.push_back(std::move(p));
            pos = comma + 1;
        }
        if (g_policies.empty() || std::strcmp(g_policies[0]->Name(), "exact") != 0)
            g_policies.insert(g_policies.begin(), Make("exact"));
    }

    svms::MappedMidiFile file;
    std::string error;
    if (!file.Open(midi.c_str(), error)) {
        std::fprintf(stderr, "open failed: %s\n", error.c_str());
        return 3;
    }
    uint64_t noteOnsGuess = 0;
    {
        svms::MidiStreamInfo info;
        if (file.Size() < (200u << 20)) {
            svms::MidiStreamDecoder dec;
            if (dec.Scan(file, uint32_t(kSampleRate), info, error)) noteOnsGuess = info.noteOnCount;
        } else {
            noteOnsGuess = g.maxNotes;
        }
    }
    noteOnsGuess = std::min<uint64_t>(std::max<uint64_t>(noteOnsGuess, 1024), g.maxNotes) + 16;
    g_notes.reserve(size_t(noteOnsGuess));
    for (auto& p : g_policies) p->Init(size_t(noteOnsGuess) + 1);
    for (auto& p : g_policies) p->loss.resize(size_t(noteOnsGuess) + 1);

    bool ok;
    if (file.Size() < (200u << 20)) {
        svms::MidiStreamDecoder dec;
        ok = dec.Decode(file, uint32_t(kSampleRate), OnEvent, nullptr, nullptr, nullptr, error);
    } else {
        svms::ParallelMidiDecoder dec;
        ok = dec.Decode(file, uint32_t(kSampleRate), OnEvent, nullptr, nullptr, nullptr, error);
    }
    if (!ok && !g_stop) {
        std::fprintf(stderr, "decode failed: %s\n", error.c_str());
        return 4;
    }
    FlushBlock();
    for (auto& p : g_policies) p->Finish(g_lastFrame / kSampleRate + 5.0);

    std::printf("file notes=%zu blocks=%llu cap=%u block=%u tauHold=%.2f tauRelease=%.3f reject=%d\n",
                g_notes.size(), (unsigned long long)g_blocks, g.cap, g.block, g.tauHold, g.tauRelease, g.reject);
    const Policy& ref = *g_policies[0];
    std::printf("%-10s %9s %8s %8s %9s %9s %9s %9s %8s\n", "policy", "cyc/on", "loss%", "loud%", "cut", "dropped",
                "worse%", "better%", "dActive");
    for (auto& up : g_policies) {
        Policy& p = *up;
        double worse = 0, better = 0, dsum = 0;
        uint32_t dmax = 0;
        for (size_t i = 0; i < g_notes.size(); ++i) {
            const double d = double(p.loss[i]) - double(ref.loss[i]);
            (d > 0 ? worse : better) += std::fabs(d);
        }
        for (size_t b = 0; b < p.series.size() && b < ref.series.size(); ++b) {
            const uint32_t d = p.series[b] > ref.series[b] ? p.series[b] - ref.series[b] : ref.series[b] - p.series[b];
            dsum += d;
            dmax = std::max(dmax, d);
        }
        const double gross = ref.grossAud > 0 ? ref.grossAud : 1;
        std::printf("%-10s %9.1f %8.3f %8.3f %9llu %9llu %9.4f %9.4f %5.1f/%u\n", p.Name(),
                    p.noteOnCount ? double(p.cycles) / double(p.noteOnCount) : 0.0, 100 * p.totalLoss / gross,
                    100 * p.loudLoss / gross, (unsigned long long)p.cutCount, (unsigned long long)p.dropCount,
                    100 * worse / gross, 100 * better / gross, p.series.empty() ? 0.0 : dsum / double(p.series.size()),
                    dmax);
        std::printf("  cuts of released tails (not expressible in a rendered MIDI): %llu of %llu\n",
                    (unsigned long long)p.tailCutCount, (unsigned long long)p.cutCount);
        if (&p == &ref) continue;
        struct Row {
            double delta;
            uint32_t note;
        };
        std::vector<Row> rows;
        uint64_t differing = 0;
        uint64_t perDecile[10] = {};
        double decileDelta[10] = {};
        for (size_t i = 0; i < g_notes.size(); ++i) {
            const double d = double(p.loss[i]) - double(ref.loss[i]);
            if (p.fate[i] != ref.fate[i]) {
                ++differing;
                perDecile[std::min(9, g_notes[i].velocity / 13)]++;
            }
            decileDelta[std::min(9, g_notes[i].velocity / 13)] += d;
            if (d > 1e-9) rows.push_back({d, uint32_t(i)});
        }
        std::printf("  differing fate: %llu notes; by velocity decile:", (unsigned long long)differing);
        for (int k = 0; k < 10; ++k) std::printf(" %llu", (unsigned long long)perDecile[k]);
        std::printf("\n  net loss delta by decile (%% of gross):");
        for (int k = 0; k < 10; ++k) std::printf(" %.3f", 100 * decileDelta[k] / gross);
        std::printf("\n");
        const size_t keep = std::min<size_t>(g.top, rows.size());
        std::partial_sort(rows.begin(), rows.begin() + keep, rows.end(),
                          [](const Row& a, const Row& b) { return a.delta > b.delta; });
        const std::string path = g.out + "_" + p.Name() + "_diff.csv";
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            std::fprintf(f, "note,time_s,channel,key,velocity,fate_exact,fate_policy,loss_exact,loss_policy\n");
            for (size_t i = 0; i < keep; ++i) {
                const Note& n = g_notes[rows[i].note];
                std::fprintf(f, "%u,%.4f,%u,%u,%u,%u,%u,%.6f,%.6f\n", rows[i].note, n.frame / kSampleRate, n.channel,
                             n.key, n.velocity, ref.fate[rows[i].note], p.fate[rows[i].note], ref.loss[rows[i].note],
                             p.loss[rows[i].note]);
            }
            std::fclose(f);
            std::printf("  worst notes -> %s\n", path.c_str());
        }
    }
    if (g.dumpStart >= 0 && g.dumpLength > 0) {
        for (auto& p : g_policies) {
            const std::string path = g.out + "_" + p->Name() + "_win.mid";
            if (WriteWindow(*p, file, path)) std::printf("listening window -> %s\n", path.c_str());
        }
    }
    return 0;
}
