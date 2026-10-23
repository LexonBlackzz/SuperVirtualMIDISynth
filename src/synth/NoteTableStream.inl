// Part of OfflineRenderer.cpp; included inside its anonymous namespace after
// NoteTableRender.inl. Not standalone.
//
// Streaming version of the BASSMIDI-mode note-table renderer (RunNoteTable
// with gNtPolicy == 3). Same pieces (online voice pool, one waveform per
// distinct note, onset curve, kill fade) but produced one window at a time:
//   - events are decoded only up to the end of the window being mixed (a note
//     that is still held, or a kill that has not happened yet, cannot change
//     samples before that point), so every sample matches the whole-file run;
//   - a note whose note-off has not been decoded yet mixes from the "held"
//     waveform (identical to the released one until the release frame);
//   - notes and pool bookkeeping older than the longest waveform are dropped,
//     and waveforms are evicted when the cache grows past kBaseBytesMax.
// Pulling frames is what drives the decode, so a host sees live progress/CPU.

#include "synth/BassKmcApi.h"

template <class T>
struct NtWin {
    std::deque<T> d;
    uint32_t base = 0;
    uint32_t size() const { return base + static_cast<uint32_t>(d.size()); }
    bool live(uint32_t i) const { return i >= base && i < size(); }
    T& operator[](uint32_t i) { return d[i - base]; }
    void push_back(const T& v) { d.push_back(v); }
    void Trim(uint32_t newBase) {
        while (base < newBase && !d.empty()) { d.pop_front(); ++base; }
    }
};

// NtBassPool's policy-3 path over windowed storage. A trimmed note is always a
// gone one, so every lookup of a trimmed index answers "gone".
struct NtStreamPool {
    struct Held { float lv; uint32_t idx; bool rel = false; bool operator<(const Held& o) const { return lv > o.lv || (lv == o.lv && idx > o.idx); } };
    struct Exp { uint32_t t, idx; bool operator<(const Exp& o) const { return t > o.t; } };
    uint32_t cap = 0xffffffffu, minFrames = 0;
    std::priority_queue<Held> held;
    std::priority_queue<Exp> expiry, released;
    std::deque<uint32_t> releasing;
    NtWin<NtNote>& notes;
    NtWin<uint8_t> gone, costs;
    NtWin<float> lvs;
    NtWin<uint8_t> dem;  // priority already dropped by a note-off (gNtFaith & 2)
    std::priority_queue<Exp> ghosts;  // spare-slot fade-outs (gNtFaith & 8): idx = ring slot of ghostCost
    uint8_t ghostCost[65536] = {};
    uint32_t ghostSeq = 0, ghostUsed = 0;
    uint32_t chUsed[16] = {};  // pool voices per MIDI channel
    uint32_t tick = 0;   // frames per 4 ms envelope tick
    uint64_t used = 0, dropped = 0;
    static constexpr uint32_t kDropped = 0xfffffffeu;

    explicit NtStreamPool(NtWin<NtNote>& n) : notes(n) {}
    bool Gone(uint32_t i) { return !gone.live(i) || gone[i] != 0; }
    void Kill(uint32_t i) { gone[i] = 1; used -= costs[i]; chUsed[notes[i].ch & 15u] -= costs[i]; }

    uint32_t NoteOn(const NtNote& n, float lv, uint32_t cost) {
        const uint32_t t = n.start;
        while (!expiry.empty() && expiry.top().t <= t) {
            const uint32_t i = expiry.top().idx;
            if (!Gone(i)) Kill(i);
            expiry.pop();
        }
        while (!released.empty() && released.top().t <= t) {
            const uint32_t i = released.top().idx; released.pop();
            if (dem.live(i)) dem[i] = 1;
            if (gNtRelF > 0.0f) held.push({lvs[i] * gNtRelF, i, true}); else releasing.push_back(i);
        }
        while (used + cost > cap) {
            uint32_t v = kNtNone;
            // A demoted voice that has not rendered a sample yet has envelope level 0, so its priority is 0 and it
            // is the first victim; it dies silently (no spare-slot copy).
            if ((gNtFaith & 16u) && !releasing.empty()) {
                const uint32_t c = releasing.back();
                if (!Gone(c) && notes[c].start >= t) { v = c; releasing.pop_back(); }
            }
            while (!releasing.empty() && v == kNtNone) {
                const uint32_t c = releasing.front(); releasing.pop_front();
                if (!Gone(c)) v = c;
            }
            if (v == kNtNone) {
                while (!held.empty() && (Gone(held.top().idx) || dem[held.top().idx] ||
                                         (!held.top().rel && notes[held.top().idx].end <= t))) held.pop();
                if (held.empty() || lv <= held.top().lv) { ++dropped; return kDropped; }
                v = held.top().idx; held.pop();
            }
            notes[v].kill = t;
            if ((gNtFaith & 8u) && tick && t > notes[v].start) {
                // Decompiled FUN_180006418/FUN_18000639c: a killed voice that has played is copied to a spare
                // slot (a second region the size of the pool) and keeps sounding until its next 4 ms tick.
                while (!ghosts.empty() && ghosts.top().t <= t) { ghostUsed -= ghostCost[ghosts.top().idx & 65535u]; ghosts.pop(); }
                const uint32_t k = notes[v].start + (std::max)(1u, (t - notes[v].start + tick - 1u) / tick) * tick;
                if (ghostUsed + costs[v] <= cap) {
                    notes[v].kill = k;
                    ghostCost[ghostSeq & 65535u] = costs[v]; ghostUsed += costs[v];
                    ghosts.push({k + tick, (ghostSeq++) & 65535u});
                }
            }
            Kill(v);
        }
        const uint32_t idx = notes.size();
        notes.push_back(n);
        gone.push_back(0); costs.push_back(static_cast<uint8_t>(cost)); lvs.push_back(lv); dem.push_back(0);
        held.push({lv, idx});
        used += cost; chUsed[n.ch & 15u] += cost;
        return idx;
    }
    void NoteOff(uint32_t idx, uint32_t frame) {
        if (!notes.live(idx)) return;  // trimmed => already gone
        NtNote& n = notes[idx];
        n.end = (std::max)(frame, n.start + minFrames);
        if ((gNtFaith & 4u) && tick) {
            const uint32_t ticks = (std::max)(1u, (frame - n.start + tick - 1u) / tick);
            n.end = n.start + ticks * tick;
        }
        if (Gone(idx)) return;
        released.push({(gNtFaith & 2u) ? frame : n.end, idx});
        expiry.push({static_cast<uint32_t>((std::min<uint64_t>)(uint64_t(n.end) + gNtRelease, 0xfffffffdull)), idx});
    }
    void Trim(uint32_t newBase) { notes.Trim(newBase); gone.Trim(newBase); costs.Trim(newBase); lvs.Trim(newBase); dem.Trim(newBase); }
};

class NtStreamer {
public:
    static constexpr uint32_t kWindow = 16384;
    static constexpr uint64_t kBaseBytesMax = 768ull << 20;

    ~NtStreamer() {
        cancel_.store(true);
        if (producer_.joinable()) producer_.join();
    }

    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
    void Trace(const char* what) const {
        if (std::getenv("SVMS_NT_TRACE"))
            std::fprintf(stderr, "[nt %7.0f ms] %s\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count(), what);
    }
    bool Init(const Options& options, const EngineConfig& cfg, std::string& error) {
        Trace("init begin");
        o_ = options;
        cfg_ = cfg;
        o_.tuning = &cfg_;
        // BASSMIDI mode (see RunNoteTable): abrupt kill, quietest-voice admission,
        // onset curve, raw float mix with the DC kept.
        g_keepDcOffset = true;
        gNtPolicy = 3u; gNtFade = 20u; gNtOnset = 12u; gNtFaith = 10u;  // faith: demote at note-off + spare-slot ghosts (oracle: rms/DC/250 Hz match)
        if (const char* v = std::getenv("SVMS_NT_ONSET")) gNtOnset = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if (const char* v = std::getenv("SVMS_NT_RELF")) gNtRelF = static_cast<float>(std::atof(v));
        if (const char* v = std::getenv("SVMS_NT_RELEASE")) gNtRelease = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if (const char* v = std::getenv("SVMS_NT_FADE")) gNtFade = (std::min)(kNtFadeMax, static_cast<uint32_t>(std::strtoul(v, nullptr, 10)));
        rate_ = o_.sampleRate;
        if (const char* v = std::getenv("SVMS_NT_FAITH")) gNtFaith = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        pool_.tick = rate_ * 4u / 1000u;
        threads_ = o_.renderThreads ? o_.renderThreads : (std::max)(1u, std::thread::hardware_concurrency());
        const uint32_t minNoteMs = o_.minNoteMs >= 0 ? static_cast<uint32_t>(o_.minNoteMs) : cfg_.minNoteMs;
        pool_.cap = o_.noteTableCap ? o_.noteTableCap : o_.maxVoices;
        pool_.minFrames = static_cast<uint32_t>(uint64_t(minNoteMs) * rate_ / 1000u);
        if (const char* r = std::getenv("SVMS_NT_REGIONS")) regionsOverride_ = static_cast<uint32_t>(std::strtoul(r, nullptr, 10));

        sessions_.resize(threads_);
        cursors_.assign(threads_, uint64_t(1) << 20);
        {
            Options wo = o_;
            wo.maxVoices = 64u; wo.renderThreads = 1u; wo.shards = 1u;
            std::vector<std::thread> init;
            std::atomic<int> failed{0};
            for (uint32_t w = 0; w < threads_; ++w)
                init.emplace_back([&, w] {
                    std::string e;
                    sessions_[w] = std::make_unique<LegacyOfflineSession>();
                    if (!sessions_[w]->Initialize(wo, e)) ++failed;
                    else sessions_[w]->SetRawMix(true);
                });
            for (auto& t : init) t.join();
            Trace("sessions loaded");
            if (failed) { error = "cannot load the SoundFont"; return false; }
        }
        if (!midi_.Open(o_.midi.c_str(), error)) return false;
        Trace("midi mapped");
        ring_ = std::make_unique<ParsedEventRing>(o_.eventBufferMB);
        Trace("ring allocated");
        if (!ring_->IsValid()) { error = "cannot allocate parsed-event ring"; return false; }
        for (auto& st : current_) st.cc.fill(-1);
        states_.push_back(current_[0]);
        producer_ = std::thread([this] {
            ParallelMidiDecoder decoder;
            MidiStreamInfo info{};
            std::string e;
            decoder.Decode(midi_, rate_, [](const PackedMidiEvent& ev, void* p) {
                auto* self = static_cast<NtStreamer*>(p);
                return self->ring_->Push(ev, self->cancel_);
            }, this, &cancel_, &info, e);
            done_.store(true, std::memory_order_release);
        });
        return true;
    }

    // Next `frames` frames of the mix (silence once the notes have ended).
    void Read(float* left, float* right, uint32_t frames) {
        uint32_t written = 0;
        while (written < frames) {
            if (outPos_ == kWindow || outL_.empty()) { Advance(); outPos_ = 0; }
            const uint32_t take = (std::min)(frames - written, kWindow - outPos_);
            std::memcpy(left + written, outL_.data() + outPos_, take * sizeof(float));
            std::memcpy(right + written, outR_.data() + outPos_, take * sizeof(float));
            written += take; outPos_ += take;
        }
    }
    uint32_t ActiveVoices() const { return static_cast<uint32_t>((std::min<uint64_t>)(pool_.used, 0xffffffffu)); }
    void ChannelVoices(uint32_t* o) const { for (int c = 0; c < 16; ++c) o[c] = pool_.chUsed[c]; }

private:
    bool firstAdvance_ = true;
    void Advance() {
        if (firstAdvance_) { Trace("first window"); }
        const uint64_t c0 = cursor_, c1 = c0 + kWindow;
        DecodeUntil(c1);

        // Notes that sound in [c0, c1) and the waveform each one plays.
        struct Use { uint32_t idx; NtBase* base; };
        std::vector<Use> uses;
        std::vector<std::pair<NtKey, NtBase*>> missing;
        for (uint32_t i = notes_.base; i < notes_.size(); ++i) {
            const NtNote& n = notes_[i];
            if (n.kill == n.start || uint64_t(n.start) + kNtMaxLife <= c0) continue;
            const NtKey key = KeyOf(n);
            auto it = bases_.find(key);
            if (it == bases_.end()) {
                it = bases_.emplace(key, NtBase{}).first;
                missing.push_back({key, &it->second});
            }
            it->second.lastUse = window_;
            uses.push_back({i, &it->second});
        }
        RenderBases(missing);
        if (firstAdvance_) { firstAdvance_ = false; Trace("first window done"); }
        for (const auto& m : missing) baseBytes_ += uint64_t(m.second->len) * 8u;

        outL_.assign(kWindow, 0.0f);
        outR_.assign(kWindow, 0.0f);
        const uint32_t parts = (std::min)(threads_, 16u);
        std::vector<std::thread> mixers;
        for (uint32_t p = 0; p < parts; ++p)
            mixers.emplace_back([&, p] {
                const uint64_t a = c0 + uint64_t(kWindow) * p / parts;
                const uint64_t b = c0 + uint64_t(kWindow) * (p + 1) / parts;
                for (const Use& u : uses) AddNote(notes_[u.idx], *u.base, c0, a, b);
            });
        for (auto& t : mixers) t.join();

        // Drop notes whose sound has ended and that the pool no longer tracks.
        uint32_t newBase = notes_.base;
        while (newBase < notes_.size() && uint64_t(notes_[newBase].start) + kNtMaxLife + gNtFade < c1 && pool_.Gone(newBase))
            ++newBase;
        pool_.Trim(newBase);
        if (baseBytes_ > kBaseBytesMax) EvictBases();
        cursor_ = c1;
        ++window_;
    }

    void DecodeUntil(uint64_t limit) {
        for (;;) {
            if (!hasHead_) {
                if (ring_->Pop(head_)) hasHead_ = true;
                else if (done_.load(std::memory_order_acquire)) {
                    if (!ring_->Pop(head_)) return;
                    hasHead_ = true;
                } else { std::this_thread::sleep_for(std::chrono::microseconds(50)); continue; }
            }
            if (head_.outputFrame >= limit) return;
            Process(head_);
            hasHead_ = false;
        }
    }

    // NtSink with the online (BASSMIDI) pool.
    void Process(const PackedMidiEvent& e) {
        if (IsInternalEngineMessage(e.message)) return;
        const uint8_t status = static_cast<uint8_t>(e.message);
        const uint8_t type = status & 0xf0u, ch = status & 0x0fu;
        const uint8_t d1 = (e.message >> 8) & 0x7fu, d2 = (e.message >> 16) & 0x7fu;
        const uint32_t frame = static_cast<uint32_t>(e.outputFrame);
        if (type == 0x90u && d2 != 0u) {
            const uint32_t st = StateId(ch);
            const uint32_t id = ProbeId(st, prog_[ch], ch, d1, d2);
            if (id >= probeLevel_.size()) Probe(id);
            const uint32_t cost = regionsOverride_ ? regionsOverride_ : probeCost_[id];
            const uint32_t idx = pool_.NoteOn({frame, kNtNone, kNtNone, id, st, ch, d1, d2, prog_[ch]}, probeLevel_[id], cost);
            open_[ch * 128u + d1].push_back(idx);
        } else if (type == 0x80u || type == 0x90u) {
            auto& q = open_[ch * 128u + d1];
            while (!q.empty()) {
                const uint32_t idx = q.front(); q.pop_front();
                if (idx != NtStreamPool::kDropped) pool_.NoteOff(idx, frame);
                if (!(gNtFaith & 1u)) break;
            }
        } else if (type == 0xc0u) {
            prog_[ch] = d1;
        } else if (type == 0xb0u) {
            if (d1 == 64 || d1 == 66 || d1 == 67 || d1 == 69 || d1 == 6 || d1 == 38 ||
                (d1 >= 96 && d1 <= 101) || d1 >= 120) {
            } else if (current_[ch].cc[d1] != d2) {
                current_[ch].cc[d1] = d2; dirty_[ch] = true;
            }
        } else if (type == 0xe0u) {
            const int32_t bend = d1 | (int32_t(d2) << 7);
            if (current_[ch].bend != bend) { current_[ch].bend = bend; dirty_[ch] = true; }
        }
    }

    uint32_t StateId(uint8_t ch) {
        if (dirty_[ch]) {
            dirty_[ch] = false;
            for (uint32_t i = 0; i < states_.size(); ++i)
                if (states_[i] == current_[ch]) { currentId_[ch] = i; return i; }
            states_.push_back(current_[ch]);
            currentId_[ch] = static_cast<uint32_t>(states_.size() - 1u);
        }
        return currentId_[ch];
    }
    uint32_t ProbeId(uint32_t state, uint8_t prog, uint8_t ch, uint8_t key, uint8_t vel) {
        const uint64_t k = (uint64_t(state) << 32) | (uint64_t(prog) << 24) | (uint64_t(ch) << 16) |
                           (uint64_t(key) << 8) | vel;
        const uint32_t slot = ch * 128u + key;
        if (lastProbeKey_[slot] == k + 1u) return lastProbeId_[slot];
        auto it = probeIndex_.find(k);
        if (it == probeIndex_.end()) {
            it = probeIndex_.emplace(k, static_cast<uint32_t>(probeKeys_.size())).first;
            probeKeys_.push_back(k);
        }
        lastProbeKey_[slot] = k + 1u; lastProbeId_[slot] = it->second;
        return it->second;
    }

    // One probe render reads the engine's own steal level and region count for a new note identity.
    void Probe(uint32_t id) {
        probeLevel_.resize(id + 1u, 0.0f);
        probeCost_.resize(id + 1u, 1u);
        LegacyOfflineSession& S = *sessions_[0];
        const uint64_t key = probeKeys_[id];
        const NtState& st = states_[key >> 32];
        std::vector<float> bl(256), br(256);
        const uint8_t prog = (key >> 24) & 0xff, ch = (key >> 16) & 0xff,
                      nk = (key >> 8) & 0xff, vel = key & 0xff;
        uint64_t& cursor = probeCursor_;
        S.Dispatch(0xc0u | ch | (uint32_t(prog) << 8), cursor);
        for (uint32_t cc = 0; cc < 128; ++cc)
            if (st.cc[cc] >= 0)
                S.Dispatch(0xb0u | ch | (cc << 8) | (uint32_t(st.cc[cc]) << 16), cursor);
        S.Dispatch(0xe0u | ch | ((uint32_t(st.bend) & 0x7fu) << 8) |
                   (((uint32_t(st.bend) >> 7) & 0x7fu) << 16), cursor);
        RealtimeEvent ev{cursor, 0u, 0x90u | ch | (uint32_t(nk) << 8) | (uint32_t(vel) << 16), false};
        S.RenderEvents(&ev, 1, bl.data(), br.data(), 64, cursor);
        probeLevel_[id] = S.StealLevel(ch, nk);
        probeCost_[id] = static_cast<uint8_t>((std::max)(1u, (std::min)(255u, S.VoiceCount(ch, nk))));
        cursor += 64;
        S.Dispatch(0xb0u | ch | (120u << 8), cursor);
        S.Dispatch(0xb0u | ch | (121u << 8), cursor);
        S.RenderEvents(nullptr, 0, bl.data(), br.data(), 256, cursor);
        cursor += 256;
    }

    // d = the held waveform (no note-off) unless the note's length is known and it was not killed first.
    NtKey KeyOf(const NtNote& n) const {
        const bool endKnown = n.end != kNtNone;
        const bool killedEarly = n.kill != kNtNone && (!endKnown || n.kill <= n.end);
        const uint32_t d = (killedEarly || !endKnown) ? kNtNone
            : static_cast<uint32_t>((std::min<uint64_t>)(n.end - n.start, kNtMaxLife));
        return NtBaseKey(n, d);
    }

    // The waveform worker of RunNoteTable, always to the full kNtMaxLife.
    void RenderBases(std::vector<std::pair<NtKey, NtBase*>>& todo) {
        if (todo.empty()) return;
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (uint32_t w = 0; w < (std::min<size_t>)(threads_, todo.size()); ++w)
            pool.emplace_back([&, w] {
                LegacyOfflineSession& S = *sessions_[w];
                uint64_t& cursor = cursors_[w];
                constexpr uint32_t kBlock = 1024;
                std::vector<float> bl(kBlock), br(kBlock);
                for (size_t b; (b = next.fetch_add(1)) < todo.size();) {
                    const uint64_t key = todo[b].first.a;
                    const NtState& st = states_[todo[b].first.state];
                    const uint32_t d = static_cast<uint32_t>(key >> 32);
                    const uint8_t prog = (key >> 24) & 0xff, ch = (key >> 16) & 0xff,
                                  nk = (key >> 8) & 0xff, vel = key & 0xff;
                    const uint32_t need = kNtMaxLife;
                    S.Dispatch(0xc0u | ch | (uint32_t(prog) << 8), cursor);
                    for (uint32_t cc = 0; cc < 128; ++cc)
                        if (st.cc[cc] >= 0)
                            S.Dispatch(0xb0u | ch | (cc << 8) | (uint32_t(st.cc[cc]) << 16), cursor);
                    S.Dispatch(0xe0u | ch | ((uint32_t(st.bend) & 0x7fu) << 8) |
                               (((uint32_t(st.bend) >> 7) & 0x7fu) << 16), cursor);
                    NtBase& out = *todo[b].second;
                    out.l.reserve(need); out.r.reserve(need);
                    bool released = false;
                    RealtimeEvent ev[2];
                    for (uint32_t done = 0; done < need; done += kBlock) {
                        const uint32_t n = (std::min)(kBlock, need - done);
                        size_t count = 0;
                        if (done == 0u) ev[count++] = {cursor, 0u, 0x90u | ch | (uint32_t(nk) << 8) | (uint32_t(vel) << 16), false};
                        if (!released && d != kNtNone && d >= done && d < done + n) {
                            ev[count++] = {cursor + (d - done), 1u, 0x80u | ch | (uint32_t(nk) << 8), false};
                            released = true;
                        }
                        S.RenderEvents(ev, count, bl.data(), br.data(), n, cursor);
                        out.l.insert(out.l.end(), bl.begin(), bl.begin() + n);
                        out.r.insert(out.r.end(), br.begin(), br.begin() + n);
                        cursor += n;
                        if ((released || d == kNtNone) && done + n > 1024u &&
                            S.Active() == 0u && S.Tails() == 0u) break;
                    }
                    S.Dispatch(0xb0u | ch | (120u << 8), cursor);
                    S.Dispatch(0xb0u | ch | (121u << 8), cursor);
                    S.RenderEvents(nullptr, 0, bl.data(), br.data(), kBlock, cursor);
                    cursor += kBlock;
                    uint32_t len = static_cast<uint32_t>(out.l.size());
                    while (len > 0u && out.l[len - 1u] == 0.0f && out.r[len - 1u] == 0.0f) --len;
                    out.len = len;
                    out.l.resize(len); out.r.resize(len);
                }
            });
        for (auto& t : pool) t.join();
    }

    void EvictBases() {
        for (auto it = bases_.begin(); it != bases_.end();) {
            if (it->second.lastUse + 8u < window_) {
                baseBytes_ -= uint64_t(it->second.len) * 8u;
                it = bases_.erase(it);
            } else ++it;
        }
    }

    // addNote of RunNoteTable, into the window buffers, for frames [a, b) only.
    void AddNote(const NtNote& n, const NtBase& B, uint64_t c0, uint64_t a, uint64_t b) {
        const uint32_t life = n.kill != kNtNone ? n.kill - n.start : B.len;
        const uint32_t lim = (std::min)(B.len, n.kill != kNtNone ? life + gNtFade : B.len);
        const int64_t s = n.start;
        const int64_t lo = (std::max<int64_t>)(0, int64_t(a) - s);
        const int64_t hi = (std::min<int64_t>)(lim, int64_t(b) - s);
        if (lo >= hi) return;
        float* dl = outL_.data() + (s - int64_t(c0));
        float* dr = outR_.data() + (s - int64_t(c0));
        const int64_t onset = gNtOnset;
        const int64_t curveEnd = onset ? 1440 : 0;
        auto onsetGain = [&](int64_t t) {
            const float g = t < 768 ? 0.68f : 0.68f + 0.32f * static_cast<float>(t - 768) / 672.0f;
            return t < onset ? g * static_cast<float>(t) / static_cast<float>(onset) : g;
        };
        const int64_t plainEnd = (std::min<int64_t>)(hi, life);
        int64_t t = lo;
        for (; t < plainEnd && t < curveEnd; ++t) { const float g = onsetGain(t); dl[t] += B.l[t] * g; dr[t] += B.r[t] * g; }
        for (; t < plainEnd; ++t) { dl[t] += B.l[t]; dr[t] += B.r[t]; }
        for (t = (std::max<int64_t>)(lo, life); t < hi; ++t) {
            const float w = (1.0f - static_cast<float>(t - life + 1) / static_cast<float>(gNtFade));
            const float g = t < curveEnd ? w * onsetGain(t) : w;
            dl[t] += B.l[t] * g; dr[t] += B.r[t] * g;
        }
    }

    Options o_;
    EngineConfig cfg_;
    uint32_t rate_ = 44100, threads_ = 1, regionsOverride_ = 0;
    MappedMidiFile midi_;
    std::unique_ptr<ParsedEventRing> ring_;
    std::atomic<bool> cancel_{false}, done_{false};
    std::thread producer_;
    PackedMidiEvent head_{};
    bool hasHead_ = false;

    NtWin<NtNote> notes_;
    NtStreamPool pool_{notes_};
    std::vector<std::deque<uint32_t>> open_ = std::vector<std::deque<uint32_t>>(16 * 128);
    uint8_t prog_[16] = {};
    NtState current_[16];
    uint32_t currentId_[16] = {};
    bool dirty_[16] = {};
    std::vector<NtState> states_;
    std::unordered_map<uint64_t, uint32_t> probeIndex_;
    std::vector<uint64_t> probeKeys_;
    std::vector<float> probeLevel_;
    std::vector<uint8_t> probeCost_;
    uint64_t lastProbeKey_[16 * 128] = {};
    uint32_t lastProbeId_[16 * 128] = {};
    uint64_t probeCursor_ = uint64_t(1) << 20;

    std::vector<std::unique_ptr<LegacyOfflineSession>> sessions_;
    std::vector<uint64_t> cursors_;
    std::unordered_map<NtKey, NtBase, NtKeyHash> bases_;
    uint64_t baseBytes_ = 0;

    uint64_t cursor_ = 0, window_ = 0;
    uint32_t outPos_ = 0;
    std::vector<float> outL_, outR_;
};

// --bassmidi-stream: the streaming renderer written to a WAV, so it can be diffed against RunNoteTable.
int RunNoteTableStream(const svms::Options& o, const svms::EngineConfig& cfg) {
    NtStreamer streamer;
    std::string error;
    if (!streamer.Init(o, cfg, error)) { std::fprintf(stderr, "error: %s\n", error.c_str()); return 1; }
    uint64_t total = 0;
    {
        MappedMidiFile m;
        if (!m.Open(o.midi.c_str(), error) || !ParallelMidiDecoder::MeasureDuration(m, o.sampleRate, total, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str()); return 1;
        }
    }
    const uint64_t frames = o.maxSeconds ? uint64_t(o.maxSeconds) * o.sampleRate  // test slice, like RunNoteTable
                                         : total + uint64_t(5) * o.sampleRate;  // RunNoteTable writes MIDI length + 3 s + 2 s
    WaveWriter wave;
    if (!wave.Open(o.output.c_str(), o.sampleRate, 0)) { std::fprintf(stderr, "error: cannot open output\n"); return 1; }
    std::vector<float> l(8192), r(8192);
    for (uint64_t f = 0; f < frames; f += 8192) {
        const uint32_t n = static_cast<uint32_t>((std::min<uint64_t>)(8192, frames - f));
        streamer.Read(l.data(), r.data(), n);
        if (!wave.Write(l.data(), r.data(), n)) return 1;
    }
    wave.Close();
    return 0;
}

#if defined(SVMS_OFFLINE_AS_LIBRARY)
// The KMC-mode API the bass.dll shim drives (frontends/FrontBass.cpp).
static void* KmcOpen(const wchar_t* midi, const wchar_t* soundfont, uint32_t rate, uint32_t voices) {
    EngineConfig cfg = EngineConfig::Load();
    Options o;
    ApplyConfigDefaults(cfg, o);
    o.midi = midi; o.soundfont = soundfont;
    o.sampleRate = rate; o.maxVoices = voices;
    o.renderThreads = 0u; o.shards = 1u;
    o.bassMode = true; o.noteTable = true; o.minNoteMs = 4;
    auto* s = new NtStreamer();
    std::string error;
    if (!s->Init(o, cfg, error)) { std::fprintf(stderr, "bassmidi mode: %s\n", error.c_str()); delete s; return nullptr; }
    return s;
}
static uint32_t KmcRead(void* h, float* l, float* r, uint32_t n) { static_cast<NtStreamer*>(h)->Read(l, r, n); return n; }
static uint32_t KmcActive(void* h) { return static_cast<NtStreamer*>(h)->ActiveVoices(); }
static void KmcClose(void* h) { delete static_cast<NtStreamer*>(h); }
static void KmcChannels(void* h, uint32_t* o) { static_cast<NtStreamer*>(h)->ChannelVoices(o); }
#endif
