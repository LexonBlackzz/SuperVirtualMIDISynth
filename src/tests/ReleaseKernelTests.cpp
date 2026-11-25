// Release-bus kernels built from the SoundFont against notes rendered by the engine.
//
// A synthetic SoundFont (one-shot sample at 44.1 kHz, region pan, attenuation and a 0.5 s release) is rendered at
// 48 kHz, so the kernel goes through the same resampling, envelope and pan as a voice. Held: the engine's note
// must equal the kernel times the channel mix. Released: it must equal the kernel, then the kernel times the
// release decay from the note-off on, times the velocity gain. The prototype held the bus to 60 dB.

#include "synth/StandaloneSynth.h"

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
    store.Start([&](uint64_t k, bus::KernelSource& out) {
        return synth.ReleaseKernelSource(uint32_t(k >> 40), uint32_t(k >> 8) & 0xFFFFFFFFu, uint8_t(k), out);
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
    tight.Start([&](uint64_t k, bus::KernelSource& out) {
        return synth.ReleaseKernelSource(uint32_t(k >> 40), uint32_t(k >> 8) & 0xFFFFFFFFu, uint8_t(k), out);
    }, held, 1024, size_t(64) << 10);   // this kernel needs about 0.5 MB
    tight.Get(k67);
    tight.WaitIdle();
    Check(tight.Get(k67) == nullptr && tight.Refused() == 1 && tight.Bytes() == 0, "past the budget a kernel is refused");
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
    std::remove(path.c_str());
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
