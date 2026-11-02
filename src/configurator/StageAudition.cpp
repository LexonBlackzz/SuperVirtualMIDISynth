#include "configurator/StageAudition.h"
#include "configurator/ConfigDocument.h"

#ifdef _WIN32
#include "render/StageBuilder.h"
#include "render/StageTracerVulkan.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "StageLoop.inc"   // kStageLoopBytes: mono 48 kHz int16
#endif

namespace svms::cfg {

#ifdef _WIN32

namespace {

namespace st = svms::stage;

constexpr int kRate = 48000;
constexpr int kBlock = 512;
constexpr float kStep = 0.25f;       // seconds per note
constexpr int kLoopFrames = static_cast<int>(sizeof(kStageLoopBytes) / 2);
constexpr float kLoopSec = static_cast<float>(kLoopFrames) / kRate;
constexpr int kLoopPasses = 3;
constexpr int kOrbitBlocks = 16;     // room response refresh while a source moves
const int kScale[10] = {0, 2, 4, 7, 9, 12, 9, 7, 4, 2};
const int kBase[16] = {60, 67, 72, 48, 76, 69, 55, 72, 64, 40, 36, 60, 65, 67, 72, 84};

float Hz(int midi) { return 440.0f * std::pow(2.0f, (static_cast<float>(midi) - 69.0f) / 12.0f); }

// One plucked note (or a drum hit on channel 10) added into `out`.
void Note(std::vector<float>& out, int ch, float startSec, float lenSec, int midi, uint32_t& rng) {
    const int n0 = static_cast<int>(startSec * kRate);
    const int n = static_cast<int>(lenSec * kRate);
    const float f = Hz(midi);
    for (int i = 0; i < n && n0 + i < static_cast<int>(out.size()); ++i) {
        const float t = static_cast<float>(i) / kRate;
        float v;
        if (ch == 9) {
            rng = rng * 1664525u + 1013904223u;
            const float noise = static_cast<float>(rng >> 8) / 8388608.0f - 1.0f;
            v = 0.5f * noise * std::exp(-t * 40.0f) + 0.6f * std::sin(6.2832f * (55.0f + 80.0f * std::exp(-t * 30.0f)) * t) * std::exp(-t * 14.0f);
        } else {
            const float a = (std::min)(1.0f, t / 0.005f) * std::exp(-t * 5.0f);
            const float p = 6.2832f * f * t;
            v = 0.2f * a * (std::sin(p) + 0.5f * std::sin(2.0f * p) + 0.25f * std::sin(3.0f * p));
        }
        out[static_cast<size_t>(n0 + i)] += v;
    }
}

// One embedded AAC track (resource id) decoded to mono float at kRate.
std::vector<float> DecodeTrack(int id) {
    std::vector<float> out;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return out;
    HGLOBAL mem = LoadResource(nullptr, res);
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return out;
    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(data), SizeofResource(nullptr, res));
    IMFByteStream* bytes = nullptr;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* type = nullptr;
    if (stream && SUCCEEDED(MFCreateMFByteStreamOnStream(stream, &bytes)) &&
        SUCCEEDED(MFCreateSourceReaderFromByteStream(bytes, nullptr, &reader)) && SUCCEEDED(MFCreateMediaType(&type))) {
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type))) {
            for (;;) {
                DWORD flags = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample))) break;
                if (sample) {
                    IMFMediaBuffer* buf = nullptr;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                        BYTE* p = nullptr;
                        DWORD len = 0;
                        if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                            const float* f = reinterpret_cast<const float*>(p);
                            out.insert(out.end(), f, f + len / 4);
                            buf->Unlock();
                        }
                        buf->Release();
                    }
                    sample->Release();
                }
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }
        }
    }
    if (type) type->Release();
    if (reader) reader->Release();
    if (bytes) bytes->Release();
    if (stream) stream->Release();
    return out;
}

st::StageScene MakeScene(const ConfigValues& w) {
    st::StageScene s;
    s.room.width = w.stageRoom[0];
    s.room.depth = w.stageRoom[1];
    s.room.height = w.stageRoom[2];
    for (int i = 0; i < 6; ++i) s.room.surface[i] = static_cast<st::Material>(w.stageSurface[i]);
    for (const auto& f : w.stageFurniture) {
        st::Furniture item;
        item.kind = static_cast<st::FurnitureKind>(f.kind);
        item.x = f.x;
        item.y = f.y;
        item.rotated = f.rotated;
        s.room.furniture.push_back(item);
    }
    s.listener = {w.stageListener[0], w.stageListener[1], w.stageListener[2]};
    s.yawDegrees = w.stageYaw;
    for (int c = 0; c < st::kMaxSources; ++c) {
        s.source[c] = {w.stageChannel[c][0], w.stageChannel[c][1], w.stageChannel[c][2]};
        s.sourceOn[c] = w.stageChannelOn[c];
    }
    return s;
}

// Plays interleaved stereo floats on the default render device (shared mode, the
// device converts rate and format). Returns when done or cancelled.
// `ready` counts the frames rendered so far; playback starts once 0.4 s exist and
// never runs past what has been rendered.
bool PlayStereo(const std::vector<float>& pcm, const std::atomic<size_t>& ready, const std::atomic<bool>& cancel,
                std::string& error) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&en)))) { error = "No audio device"; break; }
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) { error = "No output device"; break; }
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac)))) { error = "Cannot open the device"; break; }
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  10000000, 0, &wf, nullptr))) { error = "The device refused the format"; break; }
        UINT32 bufFrames = 0;
        ac->GetBufferSize(&bufFrames);
        if (FAILED(ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc)))) { error = "No render service"; break; }
        const size_t total = pcm.size() / 2;
        size_t pos = 0;
        const size_t lead = (std::min<size_t>)(total, kRate * 2 / 5);
        while (ready.load() < lead && !cancel.load()) Sleep(5);
        ac->Start();
        while (!cancel.load(std::memory_order_relaxed)) {
            UINT32 pad = 0;
            ac->GetCurrentPadding(&pad);
            const size_t have = ready.load();
            if (pos >= total) {
                if (pad == 0) break;
            } else if (have > pos) {
                const UINT32 n = static_cast<UINT32>((std::min<size_t>)(bufFrames - pad, have - pos));
                if (n > 0) {
                    BYTE* dst = nullptr;
                    if (SUCCEEDED(rc->GetBuffer(n, &dst))) {
                        float* o = reinterpret_cast<float*>(dst);
                        for (UINT32 i = 0; i < n * 2; ++i) o[i] = std::tanh(pcm[pos * 2 + i]);   // soft ceiling
                        rc->ReleaseBuffer(n, 0);
                        pos += n;
                    }
                }
            }
            Sleep(5);
        }
        ac->Stop();
        ok = true;
    } while (false);
    if (rc) rc->Release();
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

} // namespace

bool StageAudition::Supported() { return true; }

void StageAudition::SetStatus(const std::string& text) {
    std::lock_guard<std::mutex> g(lock_);
    status_ = text;
}

std::string StageAudition::Status() const {
    std::lock_guard<std::mutex> g(lock_);
    return status_;
}

bool StageAudition::Busy() const { return busy_.load(); }

void StageAudition::Stop() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
    busy_.store(false);
    SetStatus("Stopped");
}

void StageAudition::Start(Mode mode, const ConfigValues& values, int selected) {
    Stop();
    busy_.store(true);
    SetStatus("Rendering 0%");
    worker_ = std::thread([this, mode, values, selected] { Work(mode, values, selected); });
}

void StageAudition::Work(Mode mode, const ConfigValues& w, int selected) {
    const bool orbit = mode == Mode::Around || mode == Mode::Walk;
    const bool music = mode == Mode::Music || mode == Mode::Around;
    const bool surround = mode == Mode::Surround;
    const bool solo = mode != Mode::Tour && !surround;
    const bool dry = mode == Mode::Dry;
    std::vector<float> stems[6];
    float stemSec = 0.0f;
    if (surround) {
        SetStatus("Decoding the tracks");
        MFStartup(MF_VERSION);
        for (int i = 0; i < 6; ++i) stems[i] = DecodeTrack(201 + i);
        MFShutdown();
        size_t n = stems[0].size();
        for (auto& v : stems) n = (std::min)(n, v.size());
        if (n == 0) { SetStatus("Could not decode the 5.1 tracks"); busy_.store(false); return; }
        stemSec = static_cast<float>(n) / kRate;
    }
    const float seconds = surround ? stemSec : mode == Mode::Tour ? 16 * 0.6f + 0.4f : music ? kLoopSec * kLoopPasses : 8.0f;
    const float period = music ? kLoopSec : 8.0f;   // one trip around the listener

    // The test piece: one mono feed per channel.
    uint32_t rng = 12345u;
    const int body = static_cast<int>(seconds * kRate);
    std::vector<std::vector<float>> feed(16);
    bool active[16] = {};
    for (int c = 0; c < 16; ++c) {
        if (!w.stageChannelOn[c]) continue;
        if (solo && c != selected) continue;
        if (surround && c >= 6) continue;
        active[c] = true;
        feed[c].assign(static_cast<size_t>(body), 0.0f);
        if (surround) {
            for (int i = 0; i < body; ++i) feed[c][static_cast<size_t>(i)] = stems[c][static_cast<size_t>(i)];
            continue;
        }
        if (music) {
            // The supplied loop, through the selected channel only.
            for (int i = 0; i < body; ++i) {
                const int j = i % kLoopFrames;
                const int16_t v = static_cast<int16_t>(kStageLoopBytes[2 * j] | (kStageLoopBytes[2 * j + 1] << 8));
                feed[c][static_cast<size_t>(i)] = 0.5f * static_cast<float>(v) / 32768.0f;
            }
            continue;
        }
        if (mode == Mode::Tour) {
            Note(feed[c], c, 0.6f * c, 0.5f, kBase[c], rng);
            continue;
        }
        const int steps = static_cast<int>(seconds / kStep);
        for (int n = 0; n < steps; ++n) {
            if (!solo && (n + c) % (2 + c % 3) != 0) continue;
            Note(feed[c], c, n * kStep + (c % 4) * 0.0625f, 0.24f, kBase[c] + kScale[(n + c) % 10], rng);
        }
    }

    // The room.
    st::StageScene scene = MakeScene(w);
    st::StageRenderer renderer;
    st::BuildOptions opt;
    opt.sampleRate = static_cast<float>(kRate);
    opt.partition = kBlock;
    opt.rays = static_cast<int>(w.stageRays);
    opt.air = w.stageAir;
    opt.earModel = w.stageEar;
    opt.tailTrim = w.stageTailTrim;
    opt.cancel = &cancel_;
    std::string why;
    std::unique_ptr<st::TraceBackend> gpu = st::CreateVulkanTraceBackend(why);
    st::CpuTracer cpu;
    if (!gpu) opt.rays = (std::min)(opt.rays, 20000);
    st::TraceBackend& tracer = gpu ? *gpu : static_cast<st::TraceBackend&>(cpu);
    st::EarModelSpatializer ear;
    ear.enabled = w.stageEar;
    const int earlyParts = static_cast<int>(std::ceil(opt.earlySeconds * kRate / kBlock)) + 2;
    const int tailParts = static_cast<int>(std::ceil(12.5f * kRate / kBlock)) + 2;
    renderer.Prepare(static_cast<float>(kRate), kBlock, kBlock, earlyParts, tailParts);

    st::RoomMeta meta;
    std::shared_ptr<const st::IrSpectra> tail;
    if (!dry) {
        SetStatus(gpu ? "Tracing the room on the GPU" : "Tracing the room");
        tail = st::BuildTailSpectra(renderer.Plan(), scene, tracer, opt, &meta);
    }
    if (cancel_.load()) { busy_.store(false); return; }
    uint64_t serial = 1;
    auto build = [&](const st::StageScene& sc, bool cut) {
        auto r = st::BuildPaths(sc, ear, opt, renderer.Plan(), tail, meta, serial++, cut);
        if (dry) {
            for (auto& s : r->source) { s.tapCount = 0; s.early.reset(); }
            r->tail.reset();
        }
        renderer.Submit(std::move(r));
    };
    st::BuildOptions fast = opt;
    if (orbit) { fast.order = 1; }   // a moving source: only the gliding first-order taps, no rebuilt echo set
    auto buildFast = [&](const st::StageScene& sc, bool cut) {
        auto r = st::BuildPaths(sc, ear, orbit ? fast : opt, renderer.Plan(), tail, meta, serial++, cut);
        if (dry) {
            for (auto& s2 : r->source) { s2.tapCount = 0; s2.early.reset(); }
            r->tail.reset();
        }
        renderer.Submit(std::move(r));
    };
    buildFast(scene, true);

    const int tailFrames = dry ? kRate / 2 : static_cast<int>((std::min)(6.0f, meta.tailSeconds + 0.6f) * kRate);
    const int totalFrames = (body + tailFrames + kBlock - 1) / kBlock * kBlock;
    std::vector<float> pcm(static_cast<size_t>(totalFrames) * 2, 0.0f);
    std::atomic<size_t> ready{0};
    std::string error;
    bool played = true;
    std::thread player([&] { played = PlayStereo(pcm, ready, cancel_, error); });
    auto join = [&] { if (player.joinable()) player.join(); };
    std::vector<float> in[16], l(kBlock), r(kBlock);
    for (auto& v : in) v.assign(kBlock, 0.0f);
    const st::Vec3 L = scene.listener;
    st::StageScene moving = scene;

    for (int pos = 0, block = 0; pos < totalFrames; pos += kBlock, ++block) {
        if (cancel_.load()) { join(); busy_.store(false); return; }
        if (orbit && block % kOrbitBlocks == 0 && block > 0) {
            const float a = 6.2831853f * static_cast<float>(pos) / (period * kRate);
            for (int c = 0; c < 16; ++c) {
                if (!active[c]) continue;
                float dx = scene.source[c].x - L.x, dy = scene.source[c].y - L.y;
                if (mode == Mode::Walk && std::hypot(dx, dy) < 1.5f) { dx = 0.0f; dy = 4.0f; }
                const float ca = std::cos(a), sa = std::sin(a);
                moving.source[c].x = (std::max)(0.3f, (std::min)(scene.room.width - 0.3f, L.x + dx * ca - dy * sa));
                moving.source[c].y = (std::max)(0.3f, (std::min)(scene.room.depth - 0.3f, L.y + dx * sa + dy * ca));
            }
            buildFast(moving, false);
        }
        const float* ptr[16] = {};
        for (int c = 0; c < 16; ++c) {
            if (!active[c]) continue;
            std::fill(in[c].begin(), in[c].end(), 0.0f);
            for (int i = 0; i < kBlock && pos + i < body; ++i) in[c][static_cast<size_t>(i)] = feed[c][static_cast<size_t>(pos + i)];
            ptr[c] = in[c].data();
        }
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        renderer.Process(ptr, 16, kBlock, l.data(), r.data());
        for (int i = 0; i < kBlock; ++i) {
            pcm[static_cast<size_t>(pos + i) * 2] = l[static_cast<size_t>(i)];
            pcm[static_cast<size_t>(pos + i) * 2 + 1] = r[static_cast<size_t>(i)];
        }
        ready.store(static_cast<size_t>(pos + kBlock));
        if (pos == 0 || pos + kBlock == kRate / 2) SetStatus("Playing");
    }
    SetStatus("Playing");
    join();
    SetStatus(played ? "Stopped" : error);
    busy_.store(false);
}

#else  // !_WIN32

bool StageAudition::Supported() { return false; }
void StageAudition::SetStatus(const std::string& text) { std::lock_guard<std::mutex> g(lock_); status_ = text; }
std::string StageAudition::Status() const { std::lock_guard<std::mutex> g(lock_); return status_; }
bool StageAudition::Busy() const { return false; }
void StageAudition::Stop() {}
void StageAudition::Start(Mode, const ConfigValues&, int) { SetStatus("Listening tests need Windows"); }
void StageAudition::Work(Mode, const ConfigValues&, int) {}

#endif

} // namespace svms::cfg
