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
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
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

using Feed = std::shared_ptr<const std::vector<float>>;

Feed ArpeggioFeed(int ch) {
    auto v = std::make_shared<std::vector<float>>(static_cast<size_t>(8.0f * kRate), 0.0f);
    uint32_t rng = 12345u;
    for (int n = 0; n < 32; ++n) Note(*v, ch, n * kStep, 0.24f, kBase[ch] + kScale[(n + ch) % 10], rng);
    return v;
}

Feed TourFeed(int ch) {
    auto v = std::make_shared<std::vector<float>>(static_cast<size_t>(10.0f * kRate), 0.0f);
    uint32_t rng = 777u + static_cast<uint32_t>(ch);
    Note(*v, ch, 0.6f * ch, 0.5f, kBase[ch], rng);
    return v;
}

Feed LoopFeed() {
    auto v = std::make_shared<std::vector<float>>(static_cast<size_t>(kLoopFrames));
    for (int j = 0; j < kLoopFrames; ++j) {
        const int16_t s = static_cast<int16_t>(kStageLoopBytes[2 * j] | (kStageLoopBytes[2 * j + 1] << 8));
        (*v)[static_cast<size_t>(j)] = 0.5f * static_cast<float>(s) / 32768.0f;
    }
    return v;
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

// Everything the page edits that the sound depends on.
struct Snap {
    std::array<float, 3> room{}, listener{};
    float yaw = 0.0f, trim = 1.0f;
    std::array<uint8_t, 6> surface{};
    std::vector<StageFurnitureValue> furniture;
    std::array<std::array<float, 3>, 16> channel{};
    std::array<bool, 16> on{};
    uint32_t rays = 40000u;
    bool air = true, ear = true;
    int sel = 0;
};

Snap MakeSnap(const ConfigValues& w, int sel) {
    Snap s;
    s.room = w.stageRoom;
    s.listener = w.stageListener;
    s.yaw = w.stageYaw;
    s.trim = w.stageTailTrim;
    s.surface = w.stageSurface;
    s.furniture = w.stageFurniture;
    s.channel = w.stageChannel;
    s.on = w.stageChannelOn;
    s.rays = w.stageRays;
    s.air = w.stageAir;
    s.ear = w.stageEar;
    s.sel = sel;
    return s;
}

bool SameRoom(const Snap& a, const Snap& b) {
    return a.room == b.room && a.surface == b.surface && a.furniture == b.furniture && a.rays == b.rays &&
           a.air == b.air && a.ear == b.ear && a.trim == b.trim;
}

bool SamePositions(const Snap& a, const Snap& b) {
    return a.listener == b.listener && a.yaw == b.yaw && a.channel == b.channel && a.on == b.on && a.sel == b.sel;
}

st::StageScene MakeScene(const Snap& w) {
    st::StageScene s;
    s.room.width = w.room[0];
    s.room.depth = w.room[1];
    s.room.height = w.room[2];
    for (int i = 0; i < 6; ++i) s.room.surface[i] = static_cast<st::Material>(w.surface[i]);
    for (const auto& f : w.furniture) {
        st::Furniture item;
        item.kind = static_cast<st::FurnitureKind>(f.kind);
        item.x = f.x;
        item.y = f.y;
        item.rotated = f.rotated;
        s.room.furniture.push_back(item);
    }
    s.listener = {w.listener[0], w.listener[1], w.listener[2]};
    s.yawDegrees = w.yaw;
    for (int c = 0; c < st::kMaxSources; ++c) s.source[c] = {w.channel[c][0], w.channel[c][1], w.channel[c][2]};
    return s;
}

} // namespace

struct StageAudition::Session {
    Mode mode;
    StageAudition* owner;
    std::atomic<bool> cancel{false};
    std::atomic<bool> dry{false};
    std::atomic<bool> ready{false};
    std::atomic<uint32_t> mask{0};
    std::atomic<uint64_t> frames{0};
    std::mutex m;
    Snap pending;
    Feed feed[16];                       // read and written with std::atomic_load/store
    st::StageRenderer renderer;
    std::thread control, audio;
    std::string error;

    Session(Mode md, StageAudition* o) : mode(md), owner(o) {}
    ~Session() {
        cancel.store(true);
        if (control.joinable()) control.join();
        if (audio.joinable()) audio.join();
    }

    bool Orbit() const { return mode == Mode::Around || mode == Mode::Walk; }
    float Period() const { return mode == Mode::Around ? kLoopSec : 8.0f; }

    // Control thread: rebuilds the response whenever the page's edits change it.
    void Control(Snap first) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (mode == Mode::Surround) {
            owner->SetStatus("Decoding the tracks");
            MFStartup(MF_VERSION);
            std::vector<float> stems[6];
            size_t n = SIZE_MAX;
            for (int i = 0; i < 6; ++i) {
                stems[i] = DecodeTrack(201 + i);
                n = (std::min)(n, stems[i].size());
            }
            MFShutdown();
            if (n == 0 || n == SIZE_MAX) {
                owner->SetStatus("Could not decode the 5.1 tracks");
                cancel.store(true);
                CoUninitialize();
                return;
            }
            // Six full-scale stems sum well past full scale, so each sits at 0.3.
            for (int i = 0; i < 6; ++i) {
                auto v = std::make_shared<std::vector<float>>(stems[i].begin(), stems[i].begin() + static_cast<std::ptrdiff_t>(n));
                for (float& x : *v) x *= 0.3f;
                std::atomic_store(&feed[i], Feed(v));
            }
        } else if (mode == Mode::Tour) {
            for (int c = 0; c < 16; ++c) std::atomic_store(&feed[c], TourFeed(c));
        }
        const Feed loop = (mode == Mode::Music || mode == Mode::Around) ? LoopFeed() : Feed();

        std::string why;
        std::unique_ptr<st::TraceBackend> gpu = st::CreateVulkanTraceBackend(why);
        st::CpuTracer cpu;
        st::TraceBackend& tracer = gpu ? *gpu : static_cast<st::TraceBackend&>(cpu);
        st::EarModelSpatializer ear;
        st::BuildOptions opt;
        opt.sampleRate = static_cast<float>(kRate);
        opt.partition = kBlock;
        opt.cancel = &cancel;
        const int earlyParts = static_cast<int>(std::ceil(opt.earlySeconds * kRate / kBlock)) + 2;
        const int tailParts = static_cast<int>(std::ceil(12.5f * kRate / kBlock)) + 2;
        renderer.Prepare(static_cast<float>(kRate), kBlock, kBlock, earlyParts, tailParts);

        const bool soloMode = mode == Mode::Channel || mode == Mode::Music || mode == Mode::Around || mode == Mode::Walk;
        Snap last = first;
        bool haveLast = false, lastDry = false;
        int lastSel = -1;
        std::shared_ptr<const st::IrSpectra> tail;
        st::RoomMeta meta;
        uint64_t serial = 1;
        uint32_t lastMask = 0xFFFFFFFFu;
        auto lastBuild = std::chrono::steady_clock::now();
        float lastPhase = -1.0f;

        while (!cancel.load()) {
            Snap cur;
            {
                std::lock_guard<std::mutex> g(m);
                cur = pending;
            }
            const bool isDry = dry.load();
            const bool roomChanged = !haveLast || !SameRoom(cur, last);
            if (soloMode && cur.sel != lastSel) {
                std::atomic_store(&feed[cur.sel], (mode == Mode::Channel || mode == Mode::Walk) ? ArpeggioFeed(cur.sel) : loop);
                lastSel = cur.sel;
            }
            uint32_t m2 = 0;
            for (int c = 0; c < 16; ++c) {
                if (!cur.on[c]) continue;
                if (soloMode ? c == cur.sel : (mode == Mode::Surround ? c < 6 : true)) m2 |= 1u << c;
            }
            const uint64_t period = static_cast<uint64_t>(Period() * kRate);
            const float phase = Orbit() ? static_cast<float>(frames.load() % period) / static_cast<float>(period) : 0.0f;
            const auto now = std::chrono::steady_clock::now();
            const bool orbitDue = Orbit() && now - lastBuild > std::chrono::milliseconds(60) && phase != lastPhase;
            const bool moved = haveLast && (!SamePositions(cur, last) || isDry != lastDry || m2 != lastMask);
            if (roomChanged || moved || orbitDue) {
                opt.rays = static_cast<int>(cur.rays);
                if (!gpu) opt.rays = (std::min)(opt.rays, 20000);
                opt.air = cur.air;
                opt.earModel = cur.ear;
                opt.tailTrim = cur.trim;
                ear.enabled = cur.ear;
                st::StageScene scene = MakeScene(cur);
                for (int c = 0; c < 16; ++c) scene.sourceOn[c] = ((m2 >> c) & 1u) != 0;
                if (Orbit()) {
                    const float a = 6.2831853f * phase;
                    const float ca = std::cos(a), sa = std::sin(a);
                    const st::Vec3 L = scene.listener;
                    for (int c = 0; c < 16; ++c) {
                        if (!scene.sourceOn[c]) continue;
                        float dx = scene.source[c].x - L.x, dy = scene.source[c].y - L.y;
                        if (std::hypot(dx, dy) < 1.5f) { dx = 0.0f; dy = 4.0f; }
                        scene.source[c].x = (std::max)(0.3f, (std::min)(scene.room.width - 0.3f, L.x + dx * ca - dy * sa));
                        scene.source[c].y = (std::max)(0.3f, (std::min)(scene.room.depth - 0.3f, L.y + dx * sa + dy * ca));
                    }
                }
                if (roomChanged) {
                    owner->SetStatus(gpu ? "Tracing the room on the GPU" : "Tracing the room");
                    tail = st::BuildTailSpectra(renderer.Plan(), scene, tracer, opt, &meta);
                    if (cancel.load()) break;
                }
                st::BuildOptions pathOpt = opt;
                if (Orbit()) pathOpt.order = 1;   // a moving source keeps only the gliding first-order taps
                auto r = st::BuildPaths(scene, ear, pathOpt, renderer.Plan(), tail, meta, serial++, roomChanged || !haveLast);
                if (isDry) {
                    for (auto& s : r->source) { s.tapCount = 0; s.early.reset(); }
                    r->tail.reset();
                }
                renderer.Submit(std::move(r));
                mask.store(m2);
                last = cur;
                lastDry = isDry;
                lastMask = m2;
                lastBuild = now;
                lastPhase = phase;
                haveLast = true;
                ready.store(true);
                owner->SetStatus("Playing");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CoUninitialize();
    }

    // Audio thread: keeps the device about 150 ms ahead of the speaker.
    void Audio() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* ac = nullptr;
        IAudioRenderClient* rc = nullptr;
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
            while (!ready.load() && !cancel.load()) Sleep(5);
            if (cancel.load()) break;
            const UINT32 target = kRate * 15 / 100;
            std::vector<float> in[16], l(kBlock), r(kBlock);
            for (auto& v : in) v.assign(kBlock, 0.0f);
            float gain = 1.0f;
            ac->Start();
            while (!cancel.load()) {
                UINT32 pad = 0;
                ac->GetCurrentPadding(&pad);
                while (pad < target && bufFrames - pad >= static_cast<UINT32>(kBlock) && !cancel.load()) {
                    const uint32_t bits = mask.load();
                    const float* ptr[16] = {};
                    const uint64_t f0 = frames.load();
                    for (int c = 0; c < 16; ++c) {
                        if (!((bits >> c) & 1u)) continue;
                        const Feed fd = std::atomic_load(&feed[c]);
                        if (!fd || fd->empty()) continue;
                        const size_t n = fd->size();
                        for (int i = 0; i < kBlock; ++i) in[c][static_cast<size_t>(i)] = (*fd)[static_cast<size_t>((f0 + static_cast<uint64_t>(i)) % n)];
                        ptr[c] = in[c].data();
                    }
                    std::fill(l.begin(), l.end(), 0.0f);
                    std::fill(r.begin(), r.end(), 0.0f);
                    renderer.Process(ptr, 16, kBlock, l.data(), r.data());
                    // A limiter that cannot clip: gain drops at once to fit the block, recovers slowly.
                    float peak = 0.0f;
                    for (int i = 0; i < kBlock; ++i)
                        peak = (std::max)(peak, (std::max)(std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)])));
                    const float need = peak > 0.85f ? 0.85f / peak : 1.0f;
                    gain = (std::min)(need, (std::min)(1.0f, gain + 0.02f));
                    BYTE* dst = nullptr;
                    if (FAILED(rc->GetBuffer(kBlock, &dst))) break;
                    float* o = reinterpret_cast<float*>(dst);
                    for (int i = 0; i < kBlock; ++i) {
                        o[2 * i] = l[static_cast<size_t>(i)] * gain;
                        o[2 * i + 1] = r[static_cast<size_t>(i)] * gain;
                    }
                    rc->ReleaseBuffer(kBlock, 0);
                    frames.fetch_add(kBlock);
                    pad += kBlock;
                }
                Sleep(4);
            }
            ac->Stop();
        } while (false);
        if (rc) rc->Release();
        if (ac) ac->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        if (SUCCEEDED(co)) CoUninitialize();
        if (!error.empty()) {
            owner->SetStatus(error);
            cancel.store(true);
        }
    }
};

StageAudition::StageAudition() = default;
StageAudition::~StageAudition() { Stop(); }
bool StageAudition::Supported() { return true; }

void StageAudition::SetStatus(const std::string& text) {
    std::lock_guard<std::mutex> g(lock_);
    status_ = text;
}

std::string StageAudition::Status() const {
    std::lock_guard<std::mutex> g(lock_);
    return status_;
}

bool StageAudition::Busy() const { return session_ && !session_->cancel.load(); }

void StageAudition::Stop() {
    session_.reset();
    SetStatus("Stopped");
}

void StageAudition::Start(Mode mode, const ConfigValues& values, int selected) {
    session_.reset();
    SetStatus("Starting");
    auto s = std::make_unique<Session>(mode, this);
    s->pending = MakeSnap(values, selected);
    const Snap first = s->pending;
    Session* raw = s.get();
    s->control = std::thread([raw, first] { raw->Control(first); });
    s->audio = std::thread([raw] { raw->Audio(); });
    session_ = std::move(s);
}

void StageAudition::Update(const ConfigValues& values, int selected) {
    if (!session_) return;
    if (session_->cancel.load()) {
        session_.reset();
        return;
    }
    std::lock_guard<std::mutex> g(session_->m);
    session_->pending = MakeSnap(values, selected);
}

void StageAudition::SetDry(bool dry) {
    if (session_) session_->dry.store(dry);
}

#else  // !_WIN32

struct StageAudition::Session {};
StageAudition::StageAudition() = default;
StageAudition::~StageAudition() = default;
bool StageAudition::Supported() { return false; }
void StageAudition::SetStatus(const std::string& text) { std::lock_guard<std::mutex> g(lock_); status_ = text; }
std::string StageAudition::Status() const { std::lock_guard<std::mutex> g(lock_); return status_; }
bool StageAudition::Busy() const { return false; }
void StageAudition::Stop() {}
void StageAudition::SetDry(bool) {}
void StageAudition::Update(const ConfigValues&, int) {}
void StageAudition::Start(Mode, const ConfigValues&, int) { SetStatus("Listening tests need Windows"); }

#endif

} // namespace svms::cfg
