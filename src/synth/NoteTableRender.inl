// Part of OfflineRenderer.cpp; included inside its anonymous namespace. Not standalone.
//
// EXPERIMENT: "note table" offline renderer (--note-table CAP).
//
// Voices are linear and independent, and an offline renderer can see the whole
// file. So instead of the realtime launch/steal loop it:
//   1. decodes the file into a table of notes (start, end, key, velocity, program);
//   2. runs the voice limit as a cheap schedule (a kill frame per note) over
//      key shards in parallel;
//   3. renders one waveform per distinct (program, channel, key, velocity,
//      length) with the real engine, once, on worker mini-synths;
//   4. adds the notes into the output in time chunks, in parallel, with a
//      64-frame fade at the kill frame;
//   5. runs reverb + limiter once over the result.
// Channel state other than the program (CC, bend, pedal) is not modelled yet.

#include <array>
#include <unordered_map>

using namespace svms;

struct NtNote {
    uint32_t start, end, kill, base;  // frames; kill/base UINT32_MAX = none
    uint32_t state;                   // channel-state snapshot id (NtCollect::states)
    uint8_t ch, key, vel, prog;
};
static_assert(sizeof(NtNote) == 24);

// Latest controller values and pitch bend of a channel at a note's start.
// Pedals (64/66/67/69) and the RPN/data-entry controllers are not replayed.
struct NtState {
    std::array<int16_t, 128> cc;  // -1 = never set
    int32_t bend = 8192;
    bool operator==(const NtState& o) const { return cc == o.cc && bend == o.bend; }
};
struct NtKey {
    uint64_t a;       // d(32) | prog(8) | ch(8) | key(8) | vel(8)
    uint32_t state;
    bool operator==(const NtKey& o) const { return a == o.a && state == o.state; }
};
struct NtKeyHash {
    size_t operator()(const NtKey& k) const {
        return static_cast<size_t>((k.a * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.state) * 0xC2B2AE3D27D4EB4Full));
    }
};

struct NtBase {
    uint32_t len = 0;
    uint64_t lastUse = 0;  // streaming renderer: window of the last mix
    std::vector<float> l, r;
};

constexpr uint32_t kNtNone = 0xffffffffu;
static uint32_t gNtFade = 64;           // kill fade in frames (engine kStealFadeFrames = 64; 0 = cut)
constexpr uint32_t kNtFadeMax = 1024;
constexpr uint32_t kNtMaxLife = 96000;  // longest base waveform (2 s at 48 kHz)
constexpr uint32_t kNtShort = 4096;     // notes up to this long take the chunk fast path
static float gNtRelT = 0.0f;           // >0: BASSMIDI released-voice priority with this release time (s): lv * 10^(-0.5/T), then x10^(-0.02/T) per 4 ms tick
static bool gNtVelLevel = false;       // experiment: admission level scales with (velocity/127)^2 like BASSMIDI's v[0x13]
static uint32_t gNtOnset = 0;          // BASSMIDI-style onset: a note's first frames ramp up linearly from silence over this many frames
static float gNtRelF = 0.0f;           // >0: a released voice keeps this fraction of its level instead of being the first victim
static uint32_t gNtRelease = 4800;      // release time assumed for the voice-limit schedule
// BASSMIDI 2018 voice model (decompiled FUN_180006ab8), bit mask: 1 = a note-off frees every voice on the key,
// 2 = pool priority drops at the true note-off, 4 = the audible release waits for the voice's next 4 ms tick.
static uint32_t gNtFaith = 0;
static uint32_t gNtPolicy = 2;          // 0 kill oldest, 1 releasing first, 2 releasing first then lowest steal level

// KMC-era BASSMIDI admission (decompiled FUN_180006ab8), run online while the MIDI is decoded so only the
// notes that are admitted are ever stored. One global pool: a full pool admits a newcomer only if its level
// is strictly above the quietest live voice (released voices count as quieter than any held one); the
// quietest dies and there is no age term. A note costs `cost` voice slots (regions).
struct NtBassPool {
    struct Held { float lv; uint32_t idx; bool rel = false; bool operator<(const Held& o) const { return lv > o.lv || (lv == o.lv && idx > o.idx); } };
    struct Exp { uint32_t t, idx; bool operator<(const Exp& o) const { return t > o.t; } };
    uint32_t cap = 0xffffffffu, regionsOverride = 0, minFrames = 0;
    std::priority_queue<Held> held;
    std::priority_queue<Exp> expiry, released;
    std::deque<uint32_t> releasing;
    std::vector<uint8_t> gone, costs;  // per stored note
    std::vector<float> lvs;
    std::priority_queue<std::pair<double, uint32_t>, std::vector<std::pair<double, uint32_t>>, std::greater<>> relh;  // (log10 priority extrapolated to frame 0, idx)
    uint64_t used = 0, dropped = 0;
    static constexpr uint32_t kDropped = 0xfffffffeu;

    // Returns the stored index, or kDropped when the pool refuses the note.
    uint32_t NoteOn(std::vector<NtNote>& notes, const NtNote& n, float lv, uint32_t cost) {
        const uint32_t t = n.start;
        while (!expiry.empty() && expiry.top().t <= t) {
            const uint32_t i = expiry.top().idx;
            if (!gone[i]) { gone[i] = 1; used -= costs[i]; }
            expiry.pop();
        }
        while (!released.empty() && released.top().t <= t) {
            const uint32_t i = released.top().idx, tr = released.top().t; released.pop();
            if (gNtRelT > 0.0f) relh.push({std::log10(double(lvs[i])) - 0.5 / gNtRelT + (0.02 / gNtRelT) * (double(tr) / 192.0), i});
            else if (gNtRelF > 0.0f) held.push({lvs[i] * gNtRelF, i, true}); else releasing.push_back(i);
        }
        while (gNtRelT > 0.0f && used + cost > cap) {
            while (!held.empty() && (gone[held.top().idx] || notes[held.top().idx].end <= t)) held.pop();
            while (!relh.empty() && gone[relh.top().second]) relh.pop();
            const double lg = std::log10(double(lv));
            const double pr = relh.empty() ? 1e300 : relh.top().first - (0.02 / gNtRelT) * (double(t) / 192.0);
            const double ph = held.empty() ? 1e300 : std::log10(double(held.top().lv));
            if (pr >= 1e299 && ph >= 1e299) { ++dropped; return kDropped; }
            const bool useRel = pr <= ph;
            if (lg <= (useRel ? pr : ph)) { ++dropped; return kDropped; }
            uint32_t v;
            if (useRel) { v = relh.top().second; relh.pop(); } else { v = held.top().idx; held.pop(); }
            notes[v].kill = t; gone[v] = 1; used -= costs[v];
        }
        while (used + cost > cap) {
            uint32_t v = kNtNone;
            while (!releasing.empty() && v == kNtNone) {
                const uint32_t c = releasing.front(); releasing.pop_front();
                if (!gone[c]) v = c;
            }
            if (v == kNtNone) {
                while (!held.empty() && (gone[held.top().idx] || (!held.top().rel && notes[held.top().idx].end <= t))) held.pop();
                if (held.empty() || lv <= held.top().lv) { ++dropped; return kDropped; }
                v = held.top().idx; held.pop();
            }
            notes[v].kill = t; gone[v] = 1; used -= costs[v];
        }
        const uint32_t idx = static_cast<uint32_t>(notes.size());
        notes.push_back(n);
        gone.push_back(0); costs.push_back(static_cast<uint8_t>(cost)); lvs.push_back(lv);
        held.push({lv, idx});
        used += cost;
        return idx;
    }
    void NoteOff(std::vector<NtNote>& notes, uint32_t idx, uint32_t frame) {
        NtNote& n = notes[idx];
        n.end = (std::max)(frame, n.start + minFrames);
        if (gone[idx]) return;
        released.push({n.end, idx});
        expiry.push({static_cast<uint32_t>((std::min<uint64_t>)(uint64_t(n.end) + gNtRelease, 0xfffffffdull)), idx});
    }
};

struct NtCollect {
    std::vector<NtNote> notes;
    std::vector<std::deque<uint32_t>> open = std::vector<std::deque<uint32_t>>(16 * 128);
    uint8_t prog[16] = {};
    NtState current[16];
    uint32_t currentId[16] = {};
    bool dirty[16] = {};
    std::vector<NtState> states;
    // Distinct (state, program, channel, key, velocity): one probe render each
    // reads the engine's own steal level for it.
    std::unordered_map<uint64_t, uint32_t> probeIndex;
    std::vector<uint64_t> probeKeys;
    std::vector<float> probeLevel;
    uint64_t maxNotes = ~uint64_t(0);  // what fits in memory
    bool online = false;               // voice limit applied while decoding (BASSMIDI policy)
    NtBassPool pool;
    std::function<void(uint32_t)> probe;  // fills probeLevel/probeCost for a new identity
    bool overflow = false;
    std::atomic<uint64_t> progressFrame{0};  // decode position, read by the progress thread
    std::vector<uint8_t> probeCost;  // voices (regions) one note of this identity takes in the pool
    uint64_t lastProbeKey[16 * 128] = {};
    uint32_t lastProbeId[16 * 128] = {};
    uint64_t maxFrames = 0;
    uint64_t ignoredControl = 0;
    NtCollect() {
        for (auto& st : current) st.cc.fill(-1);
        states.push_back(current[0]);
        for (auto& id : currentId) id = 0;
    }
    uint32_t ProbeId(uint32_t state, uint8_t prog, uint8_t ch, uint8_t key, uint8_t vel) {
        const uint64_t k = (uint64_t(state) << 32) | (uint64_t(prog) << 24) | (uint64_t(ch) << 16) |
                           (uint64_t(key) << 8) | vel;
        const uint32_t slot = ch * 128u + key;
        if (lastProbeKey[slot] == k + 1u) return lastProbeId[slot];
        auto it = probeIndex.find(k);
        if (it == probeIndex.end()) {
            it = probeIndex.emplace(k, static_cast<uint32_t>(probeKeys.size())).first;
            probeKeys.push_back(k);
        }
        lastProbeKey[slot] = k + 1u; lastProbeId[slot] = it->second;
        return it->second;
    }
    uint32_t StateId(uint8_t ch) {
        if (dirty[ch]) {
            dirty[ch] = false;
            for (uint32_t i = 0; i < states.size(); ++i)
                if (states[i] == current[ch]) { currentId[ch] = i; return i; }
            states.push_back(current[ch]);
            currentId[ch] = static_cast<uint32_t>(states.size() - 1u);
        }
        return currentId[ch];
    }
};

// How many 24-byte table entries (plus per-note bookkeeping, about 64 bytes in all) fit in free memory.
uint64_t NtNoteBudget() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) return static_cast<uint64_t>(ms.ullAvailPhys * 0.7) / 64u;
#endif
    return ~uint64_t(0);
}

bool NtSink(const PackedMidiEvent& e, void* p) {
    auto& c = *static_cast<NtCollect*>(p);
    if (e.outputFrame >= c.maxFrames) return false;
    c.progressFrame.store(e.outputFrame, std::memory_order_relaxed);
    if (IsInternalEngineMessage(e.message)) return true;
    const uint8_t status = static_cast<uint8_t>(e.message);
    const uint8_t type = status & 0xf0u, ch = status & 0x0fu;
    const uint8_t d1 = (e.message >> 8) & 0x7fu, d2 = (e.message >> 16) & 0x7fu;
    const uint32_t frame = static_cast<uint32_t>(e.outputFrame);
    if (type == 0x90u && d2 != 0u) {
        if (c.notes.size() >= c.maxNotes) { c.overflow = true; return false; }
        const uint32_t st = c.StateId(ch);
        const uint32_t id = c.ProbeId(st, c.prog[ch], ch, d1, d2);
        uint32_t idx;
        if (c.online) {
            if (id >= c.probeLevel.size()) c.probe(id);
            const uint32_t cost = c.pool.regionsOverride ? c.pool.regionsOverride : c.probeCost[id];
            idx = c.pool.NoteOn(c.notes, {frame, kNtNone, kNtNone, id, st, ch, d1, d2, c.prog[ch]}, gNtVelLevel ? c.probeLevel[id] * (float(d2) * float(d2) / (127.0f * 127.0f)) : c.probeLevel[id], cost);
        } else {
            idx = static_cast<uint32_t>(c.notes.size());
            c.notes.push_back({frame, kNtNone, kNtNone, id, st, ch, d1, d2, c.prog[ch]});
        }
        c.open[ch * 128u + d1].push_back(idx);
    } else if (type == 0x80u || type == 0x90u) {
        auto& q = c.open[ch * 128u + d1];
        if (!q.empty()) {
            const uint32_t idx = q.front(); q.pop_front();
            if (idx != NtBassPool::kDropped) {
                if (c.online) c.pool.NoteOff(c.notes, idx, frame);
                else c.notes[idx].end = frame;
            }
        }
    } else if (type == 0xc0u) {
        c.prog[ch] = d1;
    } else if (type == 0xb0u) {
        if (d1 == 64 || d1 == 66 || d1 == 67 || d1 == 69 || d1 == 6 || d1 == 38 ||
            (d1 >= 96 && d1 <= 101) || d1 >= 120) {
            ++c.ignoredControl;
        } else if (c.current[ch].cc[d1] != d2) {
            c.current[ch].cc[d1] = d2; c.dirty[ch] = true;
        }
    } else if (type == 0xe0u) {
        const int32_t bend = d1 | (int32_t(d2) << 7);
        if (c.current[ch].bend != bend) { c.current[ch].bend = bend; c.dirty[ch] = true; }
    }
    return true;
}

constexpr uint32_t kNtLevelBuckets = 4096;
inline uint32_t NtLevelBucket(float level) {
    const float b = level * 2048.0f;  // BASS level * 42000 ~ 20 units per bucket
    return b <= 0.0f ? 0u : (b >= float(kNtLevelBuckets - 1u) ? kNtLevelBuckets - 1u : static_cast<uint32_t>(b));
}

inline uint32_t NtShardOf(uint32_t ch, uint32_t key, uint32_t shards) {
    return (((ch * 128u + key) * 2654435761u) >> 16u) % shards;
}

// Kill frame per note: within a shard, a new note that finds the pool full
// kills the oldest still-sounding voice.
void NtSchedule(std::vector<NtNote>& notes, const std::vector<float>& level,
                const std::vector<uint8_t>& costOf, uint32_t cap, uint32_t shards, uint32_t threads) {
    if (cap == 0u) return;
    if (gNtPolicy == 3u) return;  // the BASSMIDI policy already ran online (NtBassPool)
    std::vector<std::vector<uint32_t>> bucket(shards);
    for (uint32_t i = 0; i < notes.size(); ++i)
        bucket[NtShardOf(notes[i].ch, notes[i].key, shards)].push_back(i);
    const uint32_t shardCap = (std::max)(1u, (cap + shards - 1u) / shards);
    std::vector<uint8_t> gone(notes.size(), 0);  // shards own disjoint indices
    std::atomic<uint32_t> next{0};
    auto work = [&] {
        struct Exp { uint32_t t, idx; bool operator<(const Exp& o) const { return t > o.t; } };
        for (uint32_t s; (s = next.fetch_add(1)) < shards;) {
            std::priority_queue<Exp> expiry, released;
            std::deque<uint32_t> held, releasing;
            // policy 2: held voices bucketed by steal level (BASSMIDI kills the lowest first)
            std::vector<std::deque<uint32_t>> byLevel(gNtPolicy == 2u ? kNtLevelBuckets : 0u);
            uint32_t alive = 0;
            for (uint32_t idx : bucket[s]) {
                const uint32_t t = notes[idx].start;
                while (!expiry.empty() && expiry.top().t <= t) {
                    if (!gone[expiry.top().idx]) { gone[expiry.top().idx] = 1; --alive; }
                    expiry.pop();
                }
                if (gNtPolicy >= 1u)
                    while (!released.empty() && released.top().t <= t) {
                        releasing.push_back(released.top().idx);
                        released.pop();
                    }
                while (alive >= shardCap) {
                    uint32_t v = kNtNone;
                    while (!releasing.empty() && v == kNtNone) {
                        const uint32_t c = releasing.front(); releasing.pop_front();
                        if (!gone[c]) v = c;
                    }
                    if (gNtPolicy == 2u) {
                        // BASSMIDI-style minimum priority: the quietest held voice, oldest first.
                        for (uint32_t b = 0; b < kNtLevelBuckets && v == kNtNone; ++b)
                            while (!byLevel[b].empty() && v == kNtNone) {
                                const uint32_t c = byLevel[b].front(); byLevel[b].pop_front();
                                if (!gone[c] && notes[c].end > t) v = c;
                            }
                    } else
                    while (!held.empty() && v == kNtNone) {
                        const uint32_t c = held.front(); held.pop_front();
                        if (!gone[c]) v = c;
                    }
                    if (v == kNtNone) break;
                    notes[v].kill = t;
                    gone[v] = 1;
                    --alive;
                }
                if (gNtPolicy == 2u) byLevel[NtLevelBucket(level[notes[idx].base])].push_back(idx);
                else held.push_back(idx);
                const uint64_t endT = notes[idx].end == kNtNone
                    ? 0xfffffffeull : static_cast<uint64_t>(notes[idx].end);
                if (gNtPolicy >= 1u) released.push({static_cast<uint32_t>(endT), idx});
                expiry.push({static_cast<uint32_t>((std::min<uint64_t>)(endT + gNtRelease, 0xfffffffeull)), idx});
                ++alive;
            }
        }
    };
    std::vector<std::thread> pool;
    for (uint32_t i = 0; i < (std::min)(threads, shards); ++i) pool.emplace_back(work);
    for (auto& t : pool) t.join();
}

inline NtKey NtBaseKey(const NtNote& n, uint32_t d) {
    return {(uint64_t(d) << 32) | (uint64_t(n.prog) << 24) | (uint64_t(n.ch) << 16) |
            (uint64_t(n.key) << 8) | n.vel, n.state};
}

int RunNoteTable(const svms::Options& o, const svms::EngineConfig& cfg) {
    using namespace svms;
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point t) {
        return std::chrono::duration<double>(Clock::now() - t).count();
    };
    if (o.bassMode) {
        gNtPolicy = 3u; gNtFade = 8u; gNtOnset = 12u;  // BASSMIDI: abrupt kill, quietest-voice admission, onset curve
    }
    const uint32_t capVoices = (o.bassMode && o.noteTableCap == 0u) ? o.maxVoices : o.noteTableCap;
    const uint32_t threads = o.renderThreads
        ? o.renderThreads : (std::max)(1u, std::thread::hardware_concurrency());
    if (std::getenv("SVMS_NT_VELLV")) gNtVelLevel = true;
    if (const char* v = std::getenv("SVMS_NT_ONSET")) gNtOnset = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    if (const char* v = std::getenv("SVMS_NT_RELT")) gNtRelT = static_cast<float>(std::atof(v));
    if (const char* v = std::getenv("SVMS_NT_RELF")) gNtRelF = static_cast<float>(std::atof(v));
    if (const char* v = std::getenv("SVMS_NT_RELEASE")) gNtRelease = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    if (const char* v = std::getenv("SVMS_NT_FADE")) gNtFade = (std::min)(kNtFadeMax, static_cast<uint32_t>(std::strtoul(v, nullptr, 10)));
    if (const char* v = std::getenv("SVMS_NT_POLICY")) gNtPolicy = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    std::string error;
    ExternalCancellation externalCancel;
    if (!externalCancel.Open(o.cancelEvent, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        MachineStatus(o, "ERROR", error);
        return 1;
    }
    // Cancel button: stop at the next check; nothing is written yet, so there is no partial WAV.
    auto bail = [&]() { MachineStatus(o, "CANCELLED", "Render cancelled"); return 3; };
    // Length: --max-seconds, or the MIDI's own duration plus a 3 s release tail.
    uint64_t maxFrames = uint64_t(o.maxSeconds) * o.sampleRate;
    if (o.maxSeconds == 0u) {
        MappedMidiFile m;
        uint64_t total = 0;
        if (!m.Open(o.midi.c_str(), error) ||
            !ParallelMidiDecoder::MeasureDuration(m, o.sampleRate, total, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            MachineStatus(o, "ERROR", error);
            return 1;
        }
        maxFrames = total + uint64_t(3) * o.sampleRate;
    }
    const double totalSeconds = double(maxFrames) / o.sampleRate;
    const auto tStart = Clock::now();
    // Machine progress: six steps, weighted by where the time actually goes (decoding dominates).
    auto report = [&](double frac, int step, const char* name) {
        if (!o.machineProgress) return;
        frac = (std::max)(0.0, (std::min)(1.0, frac));
        MachinePhase(o, step, 6, name);
        const double el = since(tStart), total = totalSeconds, done = frac * total;
        MachineRenderProgress(o, frac, el, done, total, el > 0.0 ? done / el : 0.0,
                              frac > 0.01 ? el * (1.0 - frac) / frac : 0.0, 0u, 0u, 0u, 0u, false);
    };

    NtCollect collect;
    collect.maxFrames = maxFrames;
    collect.maxNotes = NtNoteBudget();
    report(0.0, 1, "Loading the SoundFont");
    // workers: one mini-synth per thread, reused for probes and waveforms
    std::vector<std::unique_ptr<LegacyOfflineSession>> sessions(threads);
    {
        Options wo = o;
        wo.maxVoices = 64u; wo.renderThreads = 1u; wo.shards = 1u;
        std::vector<std::thread> init;
        std::atomic<int> initFailed{0};
        for (uint32_t w = 0; w < threads; ++w)
            init.emplace_back([&, w] {
                std::string e;
                sessions[w] = std::make_unique<LegacyOfflineSession>();
                if (!sessions[w]->Initialize(wo, e)) ++initFailed;
                else sessions[w]->SetRawMix(true);
            });
        for (auto& t : init) t.join();
        if (initFailed) {
            std::fprintf(stderr, "error: cannot load the SoundFont\n");
            MachineStatus(o, "ERROR", "Cannot load the SoundFont.");
            return 1;
        }
    }


    // BASSMIDI policy: the voice limit runs online while decoding, so the whole note table never has to fit in memory.
    const bool online = gNtPolicy == 3u;
    const uint32_t minNoteMs = o.minNoteMs >= 0 ? static_cast<uint32_t>(o.minNoteMs) : cfg.minNoteMs;
    const uint32_t minFrames = static_cast<uint32_t>(uint64_t(minNoteMs) * o.sampleRate / 1000u);
    uint64_t onlineCursor = 1u << 20;
    auto probeOne = [&](LegacyOfflineSession& S, uint64_t key, const NtState& st, float& level, uint8_t& cost, uint64_t& cursor) {
        std::vector<float> bl(256), br(256);
        const uint8_t prog = (key >> 24) & 0xff, ch = (key >> 16) & 0xff,
                      nk = (key >> 8) & 0xff, vel = key & 0xff;
        S.Dispatch(0xc0u | ch | (uint32_t(prog) << 8), cursor);
        for (uint32_t cc = 0; cc < 128; ++cc)
            if (st.cc[cc] >= 0)
                S.Dispatch(0xb0u | ch | (cc << 8) | (uint32_t(st.cc[cc]) << 16), cursor);
        S.Dispatch(0xe0u | ch | ((uint32_t(st.bend) & 0x7fu) << 8) |
                   (((uint32_t(st.bend) >> 7) & 0x7fu) << 16), cursor);
        RealtimeEvent ev{cursor, 0u, 0x90u | ch | (uint32_t(nk) << 8) | (uint32_t(vel) << 16), false};
        S.RenderEvents(&ev, 1, bl.data(), br.data(), 64, cursor);
        level = S.StealLevel(ch, nk);
        cost = static_cast<uint8_t>((std::max)(1u, (std::min)(255u, S.VoiceCount(ch, nk))));
        cursor += 64;
        S.Dispatch(0xb0u | ch | (120u << 8), cursor);
        S.Dispatch(0xb0u | ch | (121u << 8), cursor);
        S.RenderEvents(nullptr, 0, bl.data(), br.data(), 256, cursor);
        cursor += 256;
    };
    collect.online = online;
    if (online) {
        collect.pool.cap = capVoices ? capVoices : 0xffffffffu;
        collect.pool.minFrames = minFrames;
        if (const char* r = std::getenv("SVMS_NT_REGIONS")) collect.pool.regionsOverride = static_cast<uint32_t>(std::strtoul(r, nullptr, 10));
        collect.probe = [&](uint32_t id) {
            collect.probeLevel.resize(id + 1u, 0.0f);
            collect.probeCost.resize(id + 1u, 1u);
            const uint64_t key = collect.probeKeys[id];
            probeOne(*sessions[0], key, collect.states[key >> 32], collect.probeLevel[id], collect.probeCost[id], onlineCursor);
        };
    }

    // 1. decode -> note table
    auto t0 = Clock::now();
    {
        MappedMidiFile midi;
        if (!midi.Open(o.midi.c_str(), error)) { std::fprintf(stderr, "error: %s\n", error.c_str()); return 1; }
        ParallelMidiDecoder decoder;
        std::atomic<bool> cancel{false};
        MidiStreamInfo info{};
        std::atomic<bool> decoding{true};
        std::thread ticker([&] {
            while (decoding.load()) {
                if (externalCancel.Requested()) cancel.store(true);
                report(0.34 * double(collect.progressFrame.load(std::memory_order_relaxed)) / double((std::max<uint64_t>)(1u, maxFrames)), 1, "Reading the MIDI notes");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
        decoder.Decode(midi, o.sampleRate, NtSink, &collect, &cancel, &info, error);
        decoding.store(false);
        ticker.join();
        if (externalCancel.Requested()) return bail();
        if (collect.overflow) {
            const std::string msg = "The notes that sound in this MIDI do not fit in memory (" +
                std::to_string(collect.notes.size()) + " so far).";
            std::fprintf(stderr, "error: %s\n", msg.c_str());
            MachineStatus(o, "ERROR", msg);
            return 1;
        }
    }
    auto& notes = collect.notes;
    for (auto& n : notes) if (n.end == kNtNone) n.end = static_cast<uint32_t>(maxFrames);
    if (std::getenv("SVMS_NT_DEBUG") && !notes.empty()) {
        std::vector<uint32_t> len; len.reserve(notes.size());
        uint32_t chCount[16] = {}, keyMin = 255, keyMax = 0, velMin = 255, velMax = 0;
        for (const auto& n : notes) {
            len.push_back(n.end - n.start); ++chCount[n.ch];
            keyMin = (std::min<uint32_t>)(keyMin, n.key); keyMax = (std::max<uint32_t>)(keyMax, n.key);
            velMin = (std::min<uint32_t>)(velMin, n.vel); velMax = (std::max<uint32_t>)(velMax, n.vel);
        }
        std::sort(len.begin(), len.end());
        auto pct = [&](double q) { return len[static_cast<size_t>(q * (len.size() - 1))] * 1000.0 / o.sampleRate; };
        std::fprintf(stderr, "[nt-debug] gate ms p0 %.4f p10 %.4f p50 %.4f p90 %.3f p100 %.3f; keys %u..%u vel %u..%u; channels:",
                     pct(0), pct(0.1), pct(0.5), pct(0.9), pct(1.0), keyMin, keyMax, velMin, velMax);
        for (int c = 0; c < 16; ++c) if (chCount[c]) std::fprintf(stderr, " %d:%u", c, chCount[c]);
        std::fprintf(stderr, "\n[nt-debug] first note frames:");
        for (size_t i = 0; i < 12 && i < notes.size(); ++i)
            std::fprintf(stderr, " (%u ch%u k%u v%u end%u)", notes[i].start, notes[i].ch, notes[i].key, notes[i].vel, notes[i].end);
        std::fprintf(stderr, "\n");
    }
    // Minimum note length: the note-off is held back until the note has sounded that long.
    if (minNoteMs != 0u) {
        for (auto& n : notes) if (n.end - n.start < minFrames) n.end = n.start + minFrames;
    }
    std::fprintf(stderr, "[nt] decode: %zu notes in %.2fs (%llu control events ignored)\n",
                 notes.size(), since(t0), (unsigned long long)collect.ignoredControl);

    // 1b. probe the engine's steal level for each distinct note identity
    t0 = Clock::now();
    collect.probeLevel.resize(collect.probeKeys.size(), 0.0f);
    collect.probeCost.resize(collect.probeKeys.size(), 1u);
    if (!online) {
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (uint32_t w = 0; w < threads; ++w)
            pool.emplace_back([&, w] {
                LegacyOfflineSession& S = *sessions[w];
                uint64_t cursor = 1u << 20;
                for (size_t b; (b = next.fetch_add(1)) < collect.probeKeys.size();) {
                    const uint64_t key = collect.probeKeys[b];
                    probeOne(S, key, collect.states[key >> 32], collect.probeLevel[b], collect.probeCost[b], cursor);
                }
            });
        for (auto& t : pool) t.join();
    }
    std::fprintf(stderr, "[nt] %zu note identities probed, %.2fs\n", collect.probeKeys.size(), since(t0));
    if (std::getenv("SVMS_NT_DEBUG"))
        for (size_t i = 0; i < collect.probeKeys.size(); ++i)
            std::fprintf(stderr, "  probe key %llx level %.5f\n", (unsigned long long)collect.probeKeys[i], collect.probeLevel[i]);

    if (externalCancel.Requested()) return bail();
    report(0.36, 3, "Applying the voice limit");
    // 2. voice-limit schedule
    t0 = Clock::now();
    const uint32_t shards = (std::max)(1u, o.shards);  // 1 = one global pool, like BASSMIDI
    NtSchedule(notes, collect.probeLevel, collect.probeCost, capVoices, shards, threads);
    if (std::getenv("SVMS_NT_DEBUG")) {
        uint32_t shown = 0; uint64_t longN = 0, longAlive = 0;
        for (const auto& n : notes) {
            if (n.end == kNtNone || n.end - n.start < 960u) continue;
            ++longN; if (n.kill == kNtNone) ++longAlive;
            if (shown < 25 && n.start > 48000u) { ++shown;
                std::fprintf(stderr, "[nt-long] t=%.3fs ch%u prog%u key%u vel%u len %.0fms level %.5f kill %s\n", n.start / double(o.sampleRate),
                    n.ch, n.prog, n.key, n.vel, (n.end - n.start) * 1000.0 / o.sampleRate, collect.probeLevel[n.base],
                    n.kill == kNtNone ? "none" : (n.kill == n.start ? "dropped" : "killed")); }
        }
        std::fprintf(stderr, "[nt-long] %llu long notes, %llu never killed/dropped\n", (unsigned long long)longN, (unsigned long long)longAlive);
    }
    if (const char* dump = std::getenv("SVMS_NT_DUMPSURV")) {
        // Diagnostic: the notes that survive our voice-limit schedule (start end ch key vel prog, in frames).
        if (FILE* f = std::fopen(dump, "w")) {
            uint64_t kept = 0;
            for (const auto& n : notes) {
                if (n.kill == n.start) continue;
                const uint32_t stop = n.kill != kNtNone ? n.kill : n.end;
                std::fprintf(f, "%u %u %u %u %u %u\n", n.start, stop, n.ch, n.key, n.vel, n.prog);
                ++kept;
            }
            std::fclose(f);
            std::fprintf(stderr, "[nt] dumped %llu surviving notes\n", (unsigned long long)kept);
        }
    }
    uint64_t killed = 0;
    for (const auto& n : notes) killed += n.kill != kNtNone;
    std::fprintf(stderr, "[nt] schedule: cap %u over %u shards, %llu killed, %.2fs\n",
                 capVoices, shards, (unsigned long long)killed, since(t0));

    if (externalCancel.Requested()) return bail();
    report(0.41, 4, "Rendering the distinct note sounds");
    // 3. distinct waveforms
    t0 = Clock::now();
    std::unordered_map<NtKey, uint32_t, NtKeyHash> baseIndex;
    std::vector<NtKey> baseKeys;
    std::vector<uint32_t> baseNeed;
    std::vector<uint32_t> longNotes;
    for (uint32_t i = 0; i < notes.size(); ++i) {
        if ((i & 0xfffffu) == 0u) {
            if (externalCancel.Requested()) return bail();
            report(0.41 + 0.56 * double(i) / double(notes.size()), 4, "Rendering the distinct note sounds");
        }
        NtNote& n = notes[i];
        const bool killedEarly = n.kill != kNtNone && n.kill <= n.end;
        const uint32_t d = killedEarly ? kNtNone
            : static_cast<uint32_t>((std::min<uint64_t>)(n.end - n.start, kNtMaxLife));
        const uint32_t need = n.kill != kNtNone
            ? (std::min)(n.kill - n.start + gNtFade, kNtMaxLife) : kNtMaxLife;
        const NtKey key = NtBaseKey(n, d);
        auto it = baseIndex.find(key);
        if (it == baseIndex.end()) {
            it = baseIndex.emplace(key, static_cast<uint32_t>(baseKeys.size())).first;
            baseKeys.push_back(key); baseNeed.push_back(need);
        } else if (baseNeed[it->second] < need) {
            baseNeed[it->second] = need;
        }
        n.base = it->second;
        if (need > kNtShort) longNotes.push_back(i);
    }
    std::fprintf(stderr, "[nt] %zu distinct waveforms, %zu long notes, %.2fs\n",
                 baseKeys.size(), longNotes.size(), since(t0));

    // 4. render the waveforms on worker mini-synths
    t0 = Clock::now();
    std::vector<NtBase> bases(baseKeys.size());
    {
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (uint32_t w = 0; w < threads; ++w)
            pool.emplace_back([&, w] {
                LegacyOfflineSession& S = *sessions[w];
                uint64_t cursor = 1u << 20;
                constexpr uint32_t kBlock = 1024;
                std::vector<float> bl(kBlock), br(kBlock);
                for (size_t b; (b = next.fetch_add(1)) < baseKeys.size();) {
                    const uint64_t key = baseKeys[b].a;
                    const NtState& st = collect.states[baseKeys[b].state];
                    const uint32_t d = static_cast<uint32_t>(key >> 32);
                    const uint8_t prog = (key >> 24) & 0xff, ch = (key >> 16) & 0xff,
                                  nk = (key >> 8) & 0xff, vel = key & 0xff;
                    const uint32_t need = baseNeed[b];
                    S.Dispatch(0xc0u | ch | (uint32_t(prog) << 8), cursor);
                    for (uint32_t cc = 0; cc < 128; ++cc)
                        if (st.cc[cc] >= 0)
                            S.Dispatch(0xb0u | ch | (cc << 8) | (uint32_t(st.cc[cc]) << 16), cursor);
                    S.Dispatch(0xe0u | ch | ((uint32_t(st.bend) & 0x7fu) << 8) |
                               (((uint32_t(st.bend) >> 7) & 0x7fu) << 16), cursor);
                    NtBase& out = bases[b];
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
                    // Drop whatever is still sounding before the next waveform.
                    S.Dispatch(0xb0u | ch | (120u << 8), cursor);
                    S.Dispatch(0xb0u | ch | (121u << 8), cursor);  // reset controllers
                    S.RenderEvents(nullptr, 0, bl.data(), br.data(), kBlock, cursor);
                    cursor += kBlock;
                    uint32_t len = static_cast<uint32_t>(out.l.size());
                    while (len > 0u && out.l[len - 1u] == 0.0f && out.r[len - 1u] == 0.0f) --len;
                    out.len = len;
                    out.l.resize(len); out.r.resize(len);
                }
            });
        for (auto& t : pool) t.join();
        uint64_t samples = 0;
        for (const auto& b : bases) samples += b.len;
        std::fprintf(stderr, "[nt] waveforms rendered: %.1f M samples, %.2fs\n",
                     samples * 1e-6, since(t0));
    }

    if (externalCancel.Requested()) return bail();
    report(0.97, 5, "Mixing the notes");
    // 5. add the notes into the output, in parallel time chunks
    t0 = Clock::now();
    const uint64_t outFrames = maxFrames + kNtMaxLife + gNtFade;
    std::vector<float> L(outFrames, 0.0f), R(outFrames, 0.0f);
    std::vector<float> ramp(gNtFade + 1u, 0.0f);
    for (uint32_t j = 0; j < gNtFade; ++j)
        ramp[j] = 1.0f - static_cast<float>(j + 1u) / static_cast<float>(gNtFade);
    constexpr uint32_t kChunk = 16384;
    auto addNote = [&](const NtNote& n, uint64_t c0, uint64_t c1) {
        if (n.kill == n.start) return;  // rejected by the voice limit: never sounded
        const NtBase& B = bases[n.base];
        const uint32_t life = n.kill != kNtNone ? n.kill - n.start : B.len;
        const uint32_t lim = (std::min)(B.len, n.kill != kNtNone ? life + gNtFade : B.len);
        const int64_t s = n.start;
        const int64_t lo = (std::max<int64_t>)(0, int64_t(c0) - s);
        const int64_t hi = (std::min<int64_t>)(lim, int64_t(c1) - s);
        float* dl = L.data() + s; float* dr = R.data() + s;
        const int64_t plainEnd = (std::min<int64_t>)(hi, life);
        const int64_t onset = gNtOnset;  // 0 = no BASSMIDI onset curve
        const int64_t curveEnd = onset ? 1440 : 0;
        auto onsetGain = [&](int64_t t) {
            const float g = t < 768 ? 0.68f : 0.68f + 0.32f * static_cast<float>(t - 768) / 672.0f;
            return t < onset ? g * static_cast<float>(t) / static_cast<float>(onset) : g;
        };
        int64_t t = lo;
        for (; t < plainEnd && t < curveEnd; ++t) { const float g = onsetGain(t); dl[t] += B.l[t] * g; dr[t] += B.r[t] * g; }
        for (; t < plainEnd; ++t) { dl[t] += B.l[t]; dr[t] += B.r[t]; }
        for (t = (std::max<int64_t>)(lo, life); t < hi; ++t) {
            const float w = t < curveEnd ? ramp[t - life] * onsetGain(t) : ramp[t - life];
            dl[t] += B.l[t] * w; dr[t] += B.r[t] * w;
        }
    };
    std::atomic<uint64_t> nextChunk{0};
    const uint64_t chunks = (outFrames + kChunk - 1u) / kChunk;
    std::vector<std::thread> mixers;
    for (uint32_t w = 0; w < threads; ++w)
        mixers.emplace_back([&] {
            for (uint64_t c; (c = nextChunk.fetch_add(1)) < chunks;) {
                const uint64_t c0 = c * kChunk, c1 = (std::min)(outFrames, c0 + kChunk);
                const uint32_t from = c0 > kNtShort ? static_cast<uint32_t>(c0 - kNtShort) : 0u;
                auto first = std::lower_bound(notes.begin(), notes.end(), from,
                    [](const NtNote& n, uint32_t v) { return n.start < v; });
                for (auto it = first; it != notes.end() && it->start < c1; ++it) {
                    const uint32_t need = it->kill != kNtNone
                        ? (std::min)(it->kill - it->start + gNtFade, kNtMaxLife) : kNtMaxLife;
                    if (need > kNtShort) continue;  // handled via the long list
                    addNote(*it, c0, c1);
                }
                for (uint32_t i : longNotes) {
                    const NtNote& n = notes[i];
                    if (n.start < c1 && uint64_t(n.start) + kNtMaxLife + gNtFade > c0) addNote(n, c0, c1);
                }
            }
        });
    for (auto& t : mixers) t.join();
    std::fprintf(stderr, "[nt] mix: %.2fs\n", since(t0));

    if (externalCancel.Requested()) return bail();
    report(0.99, 6, "Writing the WAV");
    // 6. reverb + limiter, then the WAV
    t0 = Clock::now();
    LegacyOfflineSession post;
    {
        Options po = o; po.maxVoices = 8u; po.renderThreads = 1u; po.shards = 1u;
        if (!post.Initialize(po, error)) { std::fprintf(stderr, "error: %s\n", error.c_str()); MachineStatus(o, "ERROR", error); return 1; }
    }
    WaveWriter wave;
    if (!wave.Open(o.output.c_str(), o.sampleRate, 0)) {
        std::fprintf(stderr, "error: cannot open output\n");
        MachineStatus(o, "ERROR", "Cannot write the output WAV. Is it open in a player or another program?");
        return 1;
    }
    const uint64_t writeFrames = maxFrames + uint64_t(2) * o.sampleRate;
    for (uint64_t f = 0; f < (std::min)(writeFrames, outFrames); f += 8192) {
        const uint32_t n = static_cast<uint32_t>((std::min<uint64_t>)(8192, (std::min)(writeFrames, outFrames) - f));
        if (gNtPolicy != 3u) post.PostProcess(L.data() + f, R.data() + f, n);  // BASSMIDI mode writes the raw float mix, like KMC (no limiter, master volume or reverb)
        if (!wave.Write(L.data() + f, R.data() + f, n)) {
            MachineStatus(o, "ERROR", "Writing the WAV failed (disk full?).");
            return 1;
        }
    }
    wave.Close();
    std::fprintf(stderr, "[nt] post+write: %.2fs\n", since(t0));
    (void)cfg;
    return 0;
}
