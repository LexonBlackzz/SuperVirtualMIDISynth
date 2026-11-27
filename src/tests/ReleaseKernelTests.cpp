// Release-bus kernels built from the SoundFont against notes rendered by the engine.
//
// A synthetic SoundFont (one-shot sample at 44.1 kHz, region pan, attenuation and a 0.5 s release) is rendered at
// 48 kHz, so the kernel goes through the same resampling, envelope and pan as a voice. Held: the engine's note
// must equal the kernel times the channel mix. Released: it must equal the kernel, then the kernel times the
// release decay from the note-off on, times the velocity gain. The prototype held the bus to 60 dB.

#include "synth/StandaloneSynth.h"
#include "render/RealtimeBlock.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace svms;

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

void Put16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void Put32(std::vector<uint8_t>& b, uint32_t v) { Put16(b, v & 0xFFFFu); Put16(b, v >> 16); }
void PutName(std::vector<uint8_t>& b, const char* name) {
    char n[20] = {};
    std::strncpy(n, name, 19);
    b.insert(b.end(), n, n + 20);
}
std::vector<uint8_t> Chunk(const char* id, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> c(id, id + 4);
    Put32(c, uint32_t(body.size()));
    c.insert(c.end(), body.begin(), body.end());
    if (body.size() & 1u) c.push_back(0);
    return c;
}
std::vector<uint8_t> List(const char* type, const std::vector<std::vector<uint8_t>>& chunks) {
    std::vector<uint8_t> body(type, type + 4);
    for (const auto& c : chunks) body.insert(body.end(), c.begin(), c.end());
    return Chunk("LIST", body);
}

// One preset, one zone: a 1.5 s one-shot of decaying partials and noise, root key 60, pan 25% left, 6 dB
// attenuation, 0.5 s release.
bool WriteSoundFont(const std::string& path) {
    const uint32_t frames = 66150;
    std::vector<uint8_t> smpl;
    uint32_t lcg = 12345u;
    for (uint32_t i = 0; i < frames + 46; ++i) {
        double s = 0.0;
        if (i < frames) {
            const double t = i / 44100.0;
            s = std::exp(-1.5 * t) * (0.5 * std::sin(2 * 3.14159265358979 * 261.6 * t) +
                                      0.25 * std::sin(2 * 3.14159265358979 * 1570.0 * t) +
                                      0.1 * std::sin(2 * 3.14159265358979 * 7300.0 * t));
            lcg = lcg * 1664525u + 1013904223u;
            s += 0.05 * (double(lcg >> 8) / double(1u << 24) - 0.5);
        }
        Put16(smpl, uint16_t(int16_t(s * 20000.0)));
    }
    std::vector<uint8_t> ifil, inam, phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    Put16(ifil, 2); Put16(ifil, 1);
    inam = {'k', 0};
    PutName(phdr, "p"); Put16(phdr, 0); Put16(phdr, 0); Put16(phdr, 0); Put32(phdr, 0); Put32(phdr, 0); Put32(phdr, 0);
    PutName(phdr, "EOP"); Put16(phdr, 0); Put16(phdr, 0); Put16(phdr, 1); Put32(phdr, 0); Put32(phdr, 0); Put32(phdr, 0);
    Put16(pbag, 0); Put16(pbag, 0); Put16(pbag, 1); Put16(pbag, 0);
    pmod.assign(10, 0);
    Put16(pgen, 41); Put16(pgen, 0);   // instrument 0
    Put16(pgen, 0); Put16(pgen, 0);
    PutName(inst, "i"); Put16(inst, 0);
    PutName(inst, "EOI"); Put16(inst, 1);
    Put16(ibag, 0); Put16(ibag, 0); Put16(ibag, 5); Put16(ibag, 0);
    imod.assign(10, 0);
    Put16(igen, 17); Put16(igen, uint16_t(int16_t(-250)));    // pan 25% left
    Put16(igen, 48); Put16(igen, 60);                          // initial attenuation 6 dB
    Put16(igen, 38); Put16(igen, uint16_t(int16_t(-1200)));   // release 0.5 s
    Put16(igen, 58); Put16(igen, 60);                          // root key
    Put16(igen, 53); Put16(igen, 0);                           // sample 0
    Put16(igen, 0); Put16(igen, 0);
    PutName(shdr, "s"); Put32(shdr, 0); Put32(shdr, frames); Put32(shdr, 0); Put32(shdr, 0);
    Put32(shdr, 44100); shdr.push_back(60); shdr.push_back(0); Put16(shdr, 0); Put16(shdr, 1);
    PutName(shdr, "EOS"); Put32(shdr, 0); Put32(shdr, 0); Put32(shdr, 0); Put32(shdr, 0);
    Put32(shdr, 0); shdr.push_back(0); shdr.push_back(0); Put16(shdr, 0); Put16(shdr, 0);
    std::vector<uint8_t> body{'s', 'f', 'b', 'k'};
    for (const auto& part :
         {List("INFO", {Chunk("ifil", ifil), Chunk("INAM", inam)}),
          List("sdta", {Chunk("smpl", smpl)}),
          List("pdta", {Chunk("phdr", phdr), Chunk("pbag", pbag), Chunk("pmod", pmod), Chunk("pgen", pgen),
                        Chunk("inst", inst), Chunk("ibag", ibag), Chunk("imod", imod), Chunk("igen", igen),
                        Chunk("shdr", shdr)})})
        body.insert(body.end(), part.begin(), part.end());
    const std::vector<uint8_t> riff = Chunk("RIFF", body);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(riff.data(), 1, riff.size(), f) == riff.size();
    std::fclose(f);
    return ok;
}

double SnrDb(const std::vector<float>& ref, const std::vector<float>& got) {
    double s = 0.0, e = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        s += double(ref[i]) * ref[i];
        const double d = double(got[i]) - ref[i];
        e += d * d;
    }
    return 10.0 * std::log10(s / (e > 0.0 ? e : 1e-300));
}

// One note on channel 0 at frame 0, its note-off at `offAt` (none if >= frames).
void RenderNote(StandaloneSynth& synth, uint8_t key, uint8_t vel, uint32_t offAt, uint32_t frames,
                std::vector<float>& l, std::vector<float>& r) {
    l.assign(frames, 0.0f);
    r.assign(frames, 0.0f);
    synth.Dispatch(0x90u | (uint32_t(key) << 8) | (uint32_t(vel) << 16), 0);
    for (uint32_t f = 0; f < frames;) {
        uint32_t n = (std::min)(1024u, frames - f);
        if (f < offAt && offAt < f + n) n = offAt - f;
        if (f == offAt) synth.Dispatch(0x80u | (uint32_t(key) << 8), f);
        synth.Render(l.data() + f, r.data() + f, n, f);
        f += n;
    }
}

void RunBackend(const std::wstring& sf2, RenderBackend backend, const char* name) {
    StandaloneSynthConfig cfg;
    cfg.soundfont = sf2;
    cfg.sampleRate = 48000;
    cfg.maxVoices = 64;
    cfg.renderThreads = 1;
    cfg.maxBlockFrames = 1024;
    cfg.limiterEnabled = false;
    cfg.backend = backend;
    StandaloneSynth synth;
    std::string error;
    if (!synth.Initialize(cfg, error)) {
        if (backend != RenderBackend::Scalar) { std::printf("skip  %s: %s\n", name, error.c_str()); return; }
        std::printf("FAIL  %s: init: %s\n", name, error.c_str());
        ++g_failures;
        return;
    }
    synth.SetRawMix(true);   // no DC blocker or limiter: the voices' sum only
    float mixL = 0.0f, mixR = 0.0f;
    synth.ChannelMixScale(0, mixL, mixR);
    char what[160];

    bus::KernelSource src;
    const uint8_t key = 67;
    Check(synth.ReleaseKernelSource(0, 0, key, src), "the region has a kernel source");
    Check(bus::KernelRefusal(src) == nullptr, "a one-shot with a flat envelope is a bus region");
    const uint32_t frames = 48000;

    // Held through the whole window.
    std::vector<float> aL(frames), aR(frames), l, r;
    bus::RenderHeld(src, frames, aL.data(), aR.data());
    RenderNote(synth, key, 127, frames, frames, l, r);
    std::vector<float> eL(frames), eR(frames);
    for (uint32_t t = 0; t < frames; ++t) { eL[t] = aL[t] * mixL; eR[t] = aR[t] * mixR; }
    const double heldL = SnrDb(eL, l), heldR = SnrDb(eR, r);
    std::snprintf(what, sizeof what, "%s: held note equals the kernel times the channel mix (%.1f / %.1f dB)",
                  name, heldL, heldR);
    Check(heldL >= 60.0 && heldR >= 60.0, what);
    synth.ResetAll(frames);

    // Velocity 64, released after 3,000 frames: the voice's first released frame is already decayed once,
    // so the release is A(tau) * r^(tau - d + 1).
    const uint32_t d = 3000;
    RenderNote(synth, key, 64, d, frames, l, r);
    const float v = synth.VelocityGain(64);
    double p = 1.0;
    for (uint32_t t = 0; t < frames; ++t) {
        if (t >= d) p *= src.releaseDecay;
        const double g = t >= d + src.releaseFrames ? 0.0 : v * p;
        eL[t] = float(aL[t] * g * mixL);
        eR[t] = float(aR[t] * g * mixR);
    }
    const double relL = SnrDb(eL, l), relR = SnrDb(eR, r);
    std::snprintf(what, sizeof what, "%s: released note equals the kernel, velocity gain and decay (%.1f / %.1f dB)",
                  name, relL, relR);
    Check(relL >= 60.0 && relR >= 60.0, what);
    synth.ResetAll(2 * frames);

    // The store: lazy, deduplicated, within budget.
    const uint32_t held = 4 * 1024;
    bus::ReleaseKernelStore store;
    store.Start([&](uint64_t k, std::vector<bus::KernelSource>& out) {
        out.resize(1);
        return synth.ReleaseKernelSource(uint32_t(k >> 40), uint32_t(k >> 8) & 0xFFFFFFFFu, uint8_t(k), out[0]);
    }, held, 1024, size_t(64) << 20);
    const uint64_t k67 = bus::KernelKey(0, 0, key);
    Check(store.Get(k67) == nullptr, "a kernel is not there on first use");
    store.WaitIdle();
    const bus::ReleaseKernel* kernel = store.Get(k67);
    Check(kernel != nullptr, "it is built in the background");
    if (kernel) {
        Check(kernel->tail.length == int(held + src.releaseFrames), "the tail covers the window and the release");
        bool same = kernel->heldL.size() == held;
        for (uint32_t t = 0; same && t < held; ++t) same = kernel->heldL[t] == aL[t] && kernel->heldR[t] == aR[t];
        Check(same, "its held part is the held render");
        Check(store.Bytes() == kernel->bytes && kernel->bytes > 0, "its memory is counted");
        Check(store.Get(k67) == kernel, "a second request returns the same kernel");
    }
    Check(store.Get(bus::KernelKey(0, 9, key)) == nullptr, "a missing region is not built");
    store.WaitIdle();
    Check(store.Get(bus::KernelKey(0, 9, key)) == nullptr && store.Refused() == 1, "and is refused once");

    bus::ReleaseKernelStore tight;
    tight.Start([&](uint64_t k, std::vector<bus::KernelSource>& out) {
        out.resize(1);
        return synth.ReleaseKernelSource(uint32_t(k >> 40), uint32_t(k >> 8) & 0xFFFFFFFFu, uint8_t(k), out[0]);
    }, held, 1024, size_t(64) << 10);   // this kernel needs about 0.5 MB
    tight.Get(k67);
    tight.WaitIdle();
    Check(tight.Get(k67) == nullptr && tight.Refused() == 1 && tight.Bytes() == 0, "past the budget a kernel is refused");
}

// The bus on against the bus off, with a pool large enough that nothing is stolen: the same song must come out.
// Dense short notes (most go to the bus), long notes and same-key overlaps (voices), volume / expression / pan
// automation, a bent channel, a sustain pedal and an all-sound-off.
// features: 1 mix automation, 2 bend, 4 sustain pedal, 8 all sound off.
void RunBus(const std::wstring& sf2, int features, const char* name) {
    const uint32_t block = 1024, seconds = 6, frames = seconds * 48000;
    std::vector<RealtimeEvent> song;
    uint64_t seq = 0;
    auto add = [&](uint64_t f, uint32_t msg) { song.push_back({f, seq++, msg, false}); };
    uint32_t lcg = 777u;
    auto rnd = [&](uint32_t n) { lcg = lcg * 1664525u + 1013904223u; return (lcg >> 8) % n; };
    for (uint32_t c = 0; c < 8; ++c) {
        // 16: one mix for every channel, so they share one bus group
        add(0, 0xB0u | c | (7u << 8) | ((features & 16 ? 100u : 60u + rnd(60)) << 16));
        add(0, 0xB0u | c | (10u << 8) | ((features & 16 ? 64u : rnd(128)) << 16));
    }
    for (uint32_t i = 0; i < 12000; ++i) {
        const uint32_t c = rnd(8), key = 40 + rnd(50), vel = 1 + rnd(127), u = rnd(100);
        const uint64_t on = 2000 + rnd(frames - 30000);
        const uint32_t len = u < 15 ? 0u : u < 60 ? rnd(240) : u < 85 ? 240 + rnd(800) : 2400 + rnd(19000);
        add(on, 0x90u | c | (key << 8) | (vel << 16));
        add(on + len, 0x80u | c | (key << 8));
    }
    if (features & 1)
    for (uint64_t f = 4000; f < frames - 2000; f += 400 + rnd(2000)) {   // mix automation on channels 0-3
        const uint32_t c = rnd(4), cc = rnd(3) == 0 ? 10u : rnd(2) ? 7u : 11u;
        add(f, 0xB0u | c | (cc << 8) | (rnd(128) << 16));
    }
    if (features & 2) {
    // Channel 5 is bent from the start, then centred: its notes are voices until then. (A channel that bends
    // while bus tails ring leaves them at their pitch, owner decision A, so that is not tested for equality.)
    add(0, 0xE0u | 5u | (0u << 8) | (80u << 16));
    add(96000, 0xE0u | 5u | (0u << 8) | (64u << 16));
    }
    if (features & 4) {
    add(120000, 0xB0u | 6u | (64u << 8) | (127u << 16));                  // channel 6 pedal down, then up
    add(170000, 0xB0u | 6u | (64u << 8) | (0u << 16));
    }
    if (features & 8) {
    add(200000, 0xB0u | 7u | (120u << 8));                                // channel 7 all sound off
    }
    if (features & 32)
    for (uint64_t f = 30000; f < frames - 2000; f += 20000 + rnd(9000)) {   // every channel fades together
        const uint32_t v = 40u + rnd(88);
        for (uint32_t c = 0; c < 8; ++c) add(f, 0xB0u | c | (7u << 8) | (v << 16));
    }
    std::stable_sort(song.begin(), song.end(), [](const RealtimeEvent& a, const RealtimeEvent& b) {
        return a.targetFrame != b.targetFrame ? a.targetFrame < b.targetFrame : a.sequence < b.sequence;
    });

    auto render = [&](bool busOn, bool share, std::vector<float>& l, std::vector<float>& r, StandaloneSynth*& keep) {
        StandaloneSynthConfig cfg;
        cfg.soundfont = sf2;
        cfg.sampleRate = 48000;
        cfg.maxVoices = 65536;
        cfg.renderThreads = 1;
        cfg.maxBlockFrames = block;
        cfg.limiterEnabled = false;
        cfg.backend = RenderBackend::Scalar;
        cfg.releaseBus = busOn;
        keep = new StandaloneSynth();
        std::string error;
        if (!keep->Initialize(cfg, error)) { std::printf("FAIL  bus init: %s\n", error.c_str()); ++g_failures; return; }
        keep->SetRawMix(true);
        keep->SetReleaseBusSharing(share);
        RealtimeBlockRenderer blocks;
        l.assign(frames, 0.0f);
        r.assign(frames, 0.0f);
        size_t next = 0;
        for (uint32_t f = 0; f < frames; f += block) {
            const uint32_t n = (std::min)(block, frames - f);
            size_t end = next;
            while (end < song.size() && song[end].targetFrame < f + n) ++end;
            blocks.Render(*keep, song.data() + next, end - next, l.data() + f, r.data() + f, n, f);
            next = end;
        }
    };
    std::vector<float> offL, offR, onL, onR;
    StandaloneSynth* off = nullptr;
    StandaloneSynth* on = nullptr;
    render(false, true, offL, offR, off);
    render(true, true, onL, onR, on);
    if (off && on) {
        char what[200];
        std::snprintf(what, sizeof what, "the bus is on and takes notes (%llu same-block and %llu handed off of 12000, "
                      "%zu histories at the end, %.1f MB of kernels)",
                      (unsigned long long)on->ReleaseBusNotes(), (unsigned long long)on->ReleaseBusHanded(),
                      on->ReleaseBusHistories(), on->ReleaseBusKernelBytes() / 1e6);
        Check(on->ReleaseBusOn() && !off->ReleaseBusOn() && on->ReleaseBusNotes() > 3000 &&
              on->ReleaseBusHanded() > 2000, what);
        Check(off->Steals() == 0 && on->Steals() == 0, "nothing is stolen in either render");
        const double sL = SnrDb(offL, onL), sR = SnrDb(offR, onR);
        size_t first = 0;
        while (first < offL.size() && std::fabs(offL[first] - onL[first]) < 1e-5f) ++first;
        std::snprintf(what, sizeof what, "%s: bus on equals bus off (%.1f / %.1f dB, first difference at frame %zu)", name, sL, sR, first);
        Check(sL >= 60.0 && sR >= 60.0, what);
        if (features & 16) {
            std::vector<float> soloL, soloR;
            StandaloneSynth* solo = nullptr;
            render(true, false, soloL, soloR, solo);
            if (solo) {
                const double shared = on->ReleaseBusConvolutionsPerBlock(), alone = solo->ReleaseBusConvolutionsPerBlock();
                std::snprintf(what, sizeof what, "%s: channels of one mix share convolutions (%.0f per block, %.0f "
                              "with a group per channel; %llu mix moves)", name, shared, alone,
                              (unsigned long long)on->ReleaseBusMoves());
                Check(shared < ((features & 1) ? 0.8 : 0.5) * alone && ((features & 1) || on->ReleaseBusMoves() == 0), what);
                const double gL = SnrDb(soloL, onL), gR = SnrDb(soloR, onR);
                std::snprintf(what, sizeof what, "%s: shared groups equal a group per channel (%.1f / %.1f dB)", name, gL, gR);
                Check(gL >= 100.0 && gR >= 100.0, what);
            }
            delete solo;
        }
    }
    delete off;
    delete on;
}

}  // namespace

int main() {
    const std::string path = "release_kernel_test.sf2";
    if (!WriteSoundFont(path)) { std::printf("FAIL  cannot write %s\n", path.c_str()); return 1; }
    const std::wstring sf2(path.begin(), path.end());

    bus::KernelSource ramp;
    ramp.samples = reinterpret_cast<const int16_t*>(&g_failures);
    ramp.sampleStart = 0; ramp.sampleEnd = 2; ramp.sampleFrames = 2;
    ramp.releaseDecay = 0.999f; ramp.releaseFrames = 100;
    Check(bus::KernelRefusal(ramp) == nullptr, "a flat one-shot passes");
    ramp.attack = 10;
    Check(bus::KernelRefusal(ramp) != nullptr, "an attack ramp is refused");
    ramp.attack = 0; ramp.decay = 10; ramp.sustain = 0.5f;
    Check(bus::KernelRefusal(ramp) != nullptr, "a decay to a lower sustain is refused");
    ramp.sustain = 1.0f; ramp.loopMode = 3;
    Check(bus::KernelRefusal(ramp) != nullptr, "a loop that ends at release is refused");

    RunBackend(sf2, RenderBackend::Scalar, "scalar");
    RunBackend(sf2, RenderBackend::AVX2, "avx2");
    RunBus(sf2, 0, "plain");
    RunBus(sf2, 1, "mix automation");
    RunBus(sf2, 2, "pitch bend");
    RunBus(sf2, 4, "sustain pedal");
    RunBus(sf2, 8, "all sound off");
    RunBus(sf2, 15, "everything");
    RunBus(sf2, 16, "one mix");
    RunBus(sf2, 48, "one mix, faded together");
    RunBus(sf2, 17, "one mix, then automation");
    RunBus(sf2, 63, "one mix, everything");
    std::remove(path.c_str());
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
