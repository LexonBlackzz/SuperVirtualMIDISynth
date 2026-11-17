// AsioHost: see AsioHost.h for why one thread owns every driver.
#include "driver/AsioHost.h"

#include <avrt.h>
#include <xmmintrin.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace svms {
namespace {

constexpr int kSlots = 8;
constexpr uint32_t kProbeRates[] = {44100, 48000, 88200, 96000};

struct Context;

// One open stream. Published in a callback slot (ASIO callbacks carry no
// user pointer); a slot is reused only after its stream was unpublished and
// every callback that might have seen it has left.
struct Stream {
    uint64_t id = 0;
    int slot = -1;
    Context* ctx = nullptr;
    IASIO* driver = nullptr;
    ASIOBufferInfo infos[2] = {};
    long types[2] = {ASIOSTFloat32LSB, ASIOSTFloat32LSB};
    uint32_t frames = 0;
    bool outputReady = false;
    std::vector<float> scratch;
    std::atomic<bool> buffersReady{false};
    std::atomic<int> depth{0};
};

std::atomic<Stream*> g_slot[kSlots];
std::atomic<int> g_slotInflight[kSlots];
bool g_slotPoisoned[kSlots];   // guarded by Impl::mutex; never reused

struct Command {
    std::function<void(Context&, Command&)> fn;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    bool quit = false;
    std::string error;
    AsioStreamInfo info;
    AsioCaps caps;
    ~Command() { CloseHandle(done); }
};

// Everything the owner thread owns. A context whose thread hung inside a
// driver is abandoned (poisoned) and leaked with its driver; the next
// command gets a fresh context and thread.
struct Context {
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::deque<std::shared_ptr<Command>> queue;   // Impl::mutex
    std::atomic<bool> poisoned{false};
    // Owner thread only.
    HWND window = nullptr;
    IASIO* driver = nullptr;
    AsioDriverEntry entry;
    ULONGLONG lingerUntil = 0;
    Stream* stream = nullptr;
    bool started = false;
};

HRESULT DefaultFactory(const AsioDriverEntry& entry, IASIO** out, void*) {
    return CoCreateInstance(entry.clsid, nullptr, CLSCTX_INPROC_SERVER, entry.clsid,
                            reinterpret_cast<void**>(out));
}

struct Impl {
    std::mutex mutex;
    Context* ctx = nullptr;
    std::thread thread;
    std::vector<std::thread> exited;
    AsioFactory factory = &DefaultFactory;
    void* factoryUser = nullptr;
    bool testDrivers = false;
    std::vector<AsioDriverEntry> drivers;
    uint32_t commandTimeoutMs = 5000;
    uint32_t lingerMs = 3000;
    uint32_t debounceMs = 500;

    // The client side of the stream. attachMutex orders attach (host
    // thread) against detach (any thread); callbacks only read atomics.
    std::mutex attachMutex;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> attachedId{0};
    std::atomic<int> renderInflight{0};
    std::atomic<AsioHost::RenderFn> render{nullptr};
    std::atomic<void*> renderUser{nullptr};
    std::atomic<AsioHost::EventFn> onEvent{nullptr};
    std::atomic<uint64_t> xruns{0};
    std::atomic<uint64_t> nextStreamId{1};
};

// Leaked on purpose: the host thread may outlive static destruction.
Impl& G() {
    static Impl* impl = new Impl();
    return *impl;
}

std::string Narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

bool SameName(const std::string& a, const std::string& b) {
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

std::vector<AsioDriverEntry> RegistryDrivers() {
    std::vector<AsioDriverEntry> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                      KEY_READ | KEY_ENUMERATE_SUB_KEYS, &root) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t key[256];
        DWORD keyLen = 256;
        if (RegEnumKeyExW(root, i, key, &keyLen, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        wchar_t clsidText[64] = {};
        DWORD bytes = sizeof(clsidText);
        if (RegGetValueW(root, key, L"CLSID", RRF_RT_REG_SZ, nullptr, clsidText, &bytes) !=
            ERROR_SUCCESS)
            continue;
        AsioDriverEntry e;
        if (FAILED(CLSIDFromString(clsidText, &e.clsid))) continue;
        e.key = Narrow(key);
        wchar_t desc[256] = {};
        bytes = sizeof(desc);
        e.name = RegGetValueW(root, key, L"Description", RRF_RT_REG_SZ, nullptr, desc,
                              &bytes) == ERROR_SUCCESS && desc[0]
                     ? Narrow(desc)
                     : e.key;
        out.push_back(e);
    }
    RegCloseKey(root);
    return out;
}

std::vector<AsioDriverEntry> CurrentDrivers() {
    Impl& g = G();
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.testDrivers) return g.drivers;
    }
    return RegistryDrivers();
}

// ---- SEH islands: no C++ objects with destructors in these ----------------

bool SafeCreate(AsioFactory factory, const AsioDriverEntry& entry, void* user,
                IASIO** out, HRESULT* hr) {
    __try {
        *hr = factory(entry, out, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

bool SafeInit(IASIO* d, HWND window, ASIOBool* ok) {
    __try {
        *ok = d->init(window);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SafeRelease(IASIO* d) {
    __try {
        d->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- callback path (driver threads) ----------------------------------------

void CallbackThreadSetup() {
    thread_local bool done = false;
    if (done) return;
    done = true;
    _mm_setcsr(_mm_getcsr() | 0x8040);   // flush-to-zero + denormals-are-zero
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
}

uint32_t BytesPerSample(long type) {
    switch (type) {
        case ASIOSTFloat64LSB: return 8;
        case ASIOSTInt24LSB: return 3;
        case ASIOSTInt16LSB: return 2;
        default: return 4;
    }
}

void ConvertChannel(void* dst, const float* src, uint32_t frames, long type, int ch) {
    // src is interleaved stereo: frame i -> src[2i + ch]. Integers are clamped.
    const float* in = src + ch;
    switch (type) {
        case ASIOSTFloat32LSB: {
            float* out = static_cast<float*>(dst);
            for (uint32_t i = 0; i < frames; ++i) out[i] = in[i * 2u];
            break;
        }
        case ASIOSTFloat64LSB: {
            double* out = static_cast<double*>(dst);
            for (uint32_t i = 0; i < frames; ++i) out[i] = static_cast<double>(in[i * 2u]);
            break;
        }
        case ASIOSTInt32LSB: {
            int32_t* out = static_cast<int32_t*>(dst);
            for (uint32_t i = 0; i < frames; ++i) {
                const float s = (std::min)(1.0f, (std::max)(-1.0f, in[i * 2u]));
                out[i] = static_cast<int32_t>(static_cast<double>(s) * 2147483647.0);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            uint8_t* out = static_cast<uint8_t*>(dst);
            for (uint32_t i = 0; i < frames; ++i) {
                const float s = (std::min)(1.0f, (std::max)(-1.0f, in[i * 2u]));
                const int32_t v = static_cast<int32_t>(s * 8388607.0f);
                out[i * 3u + 0u] = static_cast<uint8_t>(v & 0xFF);
                out[i * 3u + 1u] = static_cast<uint8_t>((v >> 8) & 0xFF);
                out[i * 3u + 2u] = static_cast<uint8_t>((v >> 16) & 0xFF);
            }
            break;
        }
        case ASIOSTInt16LSB: {
            int16_t* out = static_cast<int16_t*>(dst);
            for (uint32_t i = 0; i < frames; ++i) {
                const float s = (std::min)(1.0f, (std::max)(-1.0f, in[i * 2u]));
                out[i] = static_cast<int16_t>(s * 32767.0f);
            }
            break;
        }
        default:
            std::memset(dst, 0, static_cast<size_t>(frames) * 4u);
            break;
    }
}

void OnBufferSwitch(int slot, long index) {
    CallbackThreadSetup();
    Impl& g = G();
    g_slotInflight[slot].fetch_add(1);
    Stream* s = g_slot[slot].load();
    if (s && s->buffersReady.load(std::memory_order_acquire) && (index == 0 || index == 1)) {
        // A second call while one is still running (re-entry, or a second
        // driver thread) writes silence instead of rendering.
        if (s->depth.fetch_add(1) == 0) {
            float* lr = s->scratch.data();
            bool rendered = false;
            g.renderInflight.fetch_add(1);
            if (g.attachedId.load() == s->id) {
                if (AsioHost::RenderFn fn = g.render.load()) {
                    fn(lr, s->frames, g.renderUser.load());
                    rendered = true;
                }
            }
            g.renderInflight.fetch_sub(1);
            if (!rendered) std::memset(lr, 0, s->scratch.size() * sizeof(float));
            for (int ch = 0; ch < 2; ++ch)
                ConvertChannel(s->infos[ch].buffers[index], lr, s->frames, s->types[ch], ch);
        } else {
            for (int ch = 0; ch < 2; ++ch)
                std::memset(s->infos[ch].buffers[index], 0,
                            static_cast<size_t>(s->frames) * BytesPerSample(s->types[ch]));
        }
        if (s->outputReady) s->driver->outputReady();
        s->depth.fetch_sub(1);
    }
    g_slotInflight[slot].fetch_sub(1);
}

void OnRateChanged(int slot, ASIOSampleRate rate) {
    (void)slot;
    (void)rate;
}

long OnMessage(int slot, long selector, long value) {
    (void)slot;
    switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion ? 1 : 0;
        case kAsioEngineVersion:
            return 2;
        default:
            return 0;
    }
}

template <int S> void BufferSwitchT(long index, ASIOBool) { OnBufferSwitch(S, index); }
template <int S> ASIOTime* BufferSwitchTimeInfoT(ASIOTime* t, long index, ASIOBool) {
    OnBufferSwitch(S, index);
    return t;
}
template <int S> void RateT(ASIOSampleRate r) { OnRateChanged(S, r); }
template <int S> long MessageT(long selector, long value, void*, double*) {
    return OnMessage(S, selector, value);
}
#define SVMS_ASIO_SLOT(S) {&BufferSwitchT<S>, &RateT<S>, &MessageT<S>, &BufferSwitchTimeInfoT<S>}
// Drivers keep the pointer handed to createBuffers, so these live forever.
ASIOCallbacks g_callbacks[kSlots] = {SVMS_ASIO_SLOT(0), SVMS_ASIO_SLOT(1), SVMS_ASIO_SLOT(2),
                                     SVMS_ASIO_SLOT(3), SVMS_ASIO_SLOT(4), SVMS_ASIO_SLOT(5),
                                     SVMS_ASIO_SLOT(6), SVMS_ASIO_SLOT(7)};
#undef SVMS_ASIO_SLOT

// ---- client attach/detach (any thread) -------------------------------------

void WaitZero(const std::atomic<int>& counter, uint32_t timeoutMs) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    while (counter.load() != 0 && GetTickCount64() < end) Sleep(0);
}

// After this returns the render function is not running and will not run
// again until the next Start.
void DetachClient() {
    Impl& g = G();
    {
        std::lock_guard<std::mutex> lock(g.attachMutex);
        g.running = false;
        g.attachedId = 0;
    }
    WaitZero(g.renderInflight, g.commandTimeoutMs);
}

void AttachIfRunning(Context& ctx) {
    Impl& g = G();
    std::lock_guard<std::mutex> lock(g.attachMutex);
    if (g.running && ctx.stream && !ctx.poisoned) g.attachedId = ctx.stream->id;
}

// ---- host thread -----------------------------------------------------------

HWND CreateHostWindow() {
    static const wchar_t* kClass = L"SVMSAsioHostWindow";
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CreateHostWindow), &module);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = module;
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);   // fails harmlessly when already registered
    return CreateWindowExW(0, kClass, L"SVMS ASIO", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                           module, nullptr);
}

const AsioDriverEntry* FindDriver(const std::vector<AsioDriverEntry>& list,
                                  const std::string& name) {
    for (const AsioDriverEntry& e : list)
        if (SameName(e.name, name) || (!e.key.empty() && SameName(e.key, name))) return &e;
    return nullptr;
}

// Creates and initialises a driver instance on the host thread, retrying
// the way a driver that is still tearing down a previous instance needs.
IASIO* CreateDriver(Context& ctx, const AsioDriverEntry& entry, std::string& error) {
    Impl& g = G();
    AsioFactory factory;
    void* user;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        factory = g.factory;
        user = g.factoryUser;
    }
    std::string last = "the driver refused to load";
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (attempt) {
            Sleep(50);
            CoFreeUnusedLibraries();
        }
        IASIO* d = nullptr;
        HRESULT hr = E_FAIL;
        if (!SafeCreate(factory, entry, user, &d, &hr)) {
            error = "ASIO driver \"" + entry.name + "\" crashed while loading";
            return nullptr;
        }
        if (FAILED(hr) || !d) continue;
        ASIOBool ok = ASIOFalse;
        if (!SafeInit(d, ctx.window, &ok)) {
            error = "ASIO driver \"" + entry.name + "\" crashed while starting";
            return nullptr;   // leaked: a crashed driver is not called again
        }
        if (ctx.poisoned) return nullptr;
        if (ok) return d;
        char msg[128] = {};
        d->getErrorMessage(msg);
        if (msg[0]) last = msg;
        SafeRelease(d);
    }
    error = "could not load ASIO driver \"" + entry.name + "\" (" + last + ")";
    return nullptr;
}

void ReleaseDriver(Context& ctx) {
    if (!ctx.driver) return;
    SafeRelease(ctx.driver);
    ctx.driver = nullptr;
    ctx.entry = AsioDriverEntry();
    CoFreeUnusedLibraries();
}

bool LoadDriver(Context& ctx, const std::string& name, std::string& error) {
    if (ctx.driver && (SameName(ctx.entry.name, name) || SameName(ctx.entry.key, name)))
        return true;
    const std::vector<AsioDriverEntry> list = CurrentDrivers();
    const AsioDriverEntry* entry = FindDriver(list, name);
    if (!entry) {
        error = "no ASIO driver named \"" + name + "\"";
        return false;
    }
    ReleaseDriver(ctx);
    IASIO* d = CreateDriver(ctx, *entry, error);
    if (!d) return false;
    ctx.driver = d;
    ctx.entry = *entry;
    return true;
}

void QueryCaps(IASIO* d, AsioCaps& caps) {
    d->getBufferSize(&caps.minSize, &caps.maxSize, &caps.preferredSize, &caps.granularity);
    caps.legalSizes =
        AsioLegalSizes(caps.minSize, caps.maxSize, caps.preferredSize, caps.granularity);
    long in = 0, out = 0;
    d->getChannels(&in, &out);
    caps.outputChannels = out > 0 ? static_cast<uint32_t>(out) : 0u;
    caps.outputNames.clear();
    for (long c = 0; c < out && c < 64; ++c) {
        ASIOChannelInfo ci = {};
        ci.channel = c;
        ci.isInput = ASIOFalse;
        char fallback[16];
        std::snprintf(fallback, sizeof(fallback), "Out %ld", c + 1);
        caps.outputNames.push_back(d->getChannelInfo(&ci) == ASE_OK && ci.name[0]
                                       ? std::string(ci.name)
                                       : std::string(fallback));
    }
    caps.rates.clear();
    for (uint32_t r : kProbeRates)
        if (d->canSampleRate(static_cast<ASIOSampleRate>(r)) == ASE_OK) caps.rates.push_back(r);
    ASIOSampleRate rate = 0;
    caps.currentRate = d->getSampleRate(&rate) == ASE_OK ? static_cast<uint32_t>(rate) : 0u;
    long inLat = 0, outLat = 0;
    caps.outputLatencyFrames = d->getLatencies(&inLat, &outLat) == ASE_OK ? outLat : 0;
}

void FreeSlot(int slot) {
    Impl& g = G();
    g_slot[slot] = nullptr;
    WaitZero(g_slotInflight[slot], g.commandTimeoutMs);
}

int AllocSlot(Stream* s) {
    Impl& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    for (int i = 0; i < kSlots; ++i) {
        if (g_slotPoisoned[i] || g_slot[i].load()) continue;
        g_slot[i] = s;
        return i;
    }
    return -1;
}

void CloseStream(Context& ctx) {
    Stream* s = ctx.stream;
    if (!s) return;
    DetachClient();
    if (ctx.started) {
        ctx.driver->stop();
        ctx.started = false;
    }
    if (ctx.poisoned) return;
    FreeSlot(s->slot);
    ctx.driver->disposeBuffers();
    delete s;
    ctx.stream = nullptr;
    ctx.lingerUntil = GetTickCount64() + G().lingerMs;
}

void DoOpen(Context& ctx, Command& cmd, const std::string& name, uint32_t rate,
            uint32_t frames, uint32_t firstChannel) {
    Impl& g = G();
    CloseStream(ctx);
    if (!LoadDriver(ctx, name, cmd.error)) return;
    IASIO* d = ctx.driver;
    AsioStreamInfo& info = cmd.info;
    info.driver = ctx.entry.name;
    std::string note;

    long in = 0, out = 0;
    if (d->getChannels(&in, &out) != ASE_OK || out < 2) {
        cmd.error = "ASIO driver \"" + name + "\" has fewer than two outputs";
        return;
    }
    if (static_cast<long>(firstChannel) + 1 >= out) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "outputs %u/%u do not exist, using 1/2; ",
                      firstChannel + 1, firstChannel + 2);
        note += buf;
        firstChannel = 0;
    }

    long mn = 0, mx = 0, pref = 0, gran = 0;
    if (d->getBufferSize(&mn, &mx, &pref, &gran) != ASE_OK) {
        cmd.error = "ASIO driver \"" + name + "\" reports no buffer sizes";
        return;
    }
    const uint32_t legal = AsioNearestLegalSize(AsioLegalSizes(mn, mx, pref, gran), frames);
    if (legal != frames) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "size %u is not legal, using %u; ", frames, legal);
        note += buf;
    }

    ASIOSampleRate current = 0;
    if (d->canSampleRate(static_cast<ASIOSampleRate>(rate)) == ASE_OK) {
        if (d->getSampleRate(&current) != ASE_OK ||
            current != static_cast<ASIOSampleRate>(rate))
            d->setSampleRate(static_cast<ASIOSampleRate>(rate));
    }
    current = 0;
    d->getSampleRate(&current);
    const uint32_t actualRate = current > 0 ? static_cast<uint32_t>(current) : rate;
    if (actualRate != rate) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "rate %u is not supported, using %u; ", rate,
                      actualRate);
        note += buf;
    }

    Stream* s = new Stream();
    s->id = g.nextStreamId.fetch_add(1);
    s->ctx = &ctx;
    s->driver = d;
    s->frames = legal;
    for (int c = 0; c < 2; ++c) {
        s->infos[c].isInput = ASIOFalse;
        s->infos[c].channelNum = static_cast<long>(firstChannel) + c;
    }
    s->slot = AllocSlot(s);
    if (s->slot < 0) {
        delete s;
        cmd.error = "no free ASIO callback slot";
        return;
    }
    if (d->createBuffers(s->infos, 2, static_cast<long>(legal), &g_callbacks[s->slot]) !=
        ASE_OK) {
        FreeSlot(s->slot);
        delete s;
        cmd.error = "ASIO driver \"" + name + "\" refused the buffers";
        return;
    }
    for (int c = 0; c < 2; ++c) {
        ASIOChannelInfo ci = {};
        ci.channel = s->infos[c].channelNum;
        ci.isInput = ASIOFalse;
        s->types[c] = d->getChannelInfo(&ci) == ASE_OK ? ci.type : ASIOSTFloat32LSB;
    }
    s->outputReady = d->outputReady() == ASE_OK;
    s->scratch.assign(static_cast<size_t>(legal) * 2u, 0.0f);
    long inLat = 0, outLat = 0;
    info.outputLatencyFrames = d->getLatencies(&inLat, &outLat) == ASE_OK ? outLat : 0;
    s->buffersReady.store(true, std::memory_order_release);
    ctx.stream = s;

    if (!note.empty()) note.resize(note.size() - 2);
    info.frames = legal;
    info.rate = actualRate;
    info.firstChannel = firstChannel;
    info.note = note;
    cmd.ok = true;
}

void DoStart(Context& ctx, Command& cmd) {
    if (!ctx.stream) {
        cmd.error = "no ASIO stream is open";
        return;
    }
    if (!ctx.started) {
        if (ctx.driver->start() != ASE_OK) {
            cmd.error = "ASIO driver \"" + ctx.entry.name + "\" did not start";
            return;
        }
        ctx.started = true;
    }
    AttachIfRunning(ctx);
    cmd.ok = true;
}

void DoProbe(Context& ctx, Command& cmd, const std::string& name) {
    if (ctx.driver && (SameName(ctx.entry.name, name) || SameName(ctx.entry.key, name))) {
        QueryCaps(ctx.driver, cmd.caps);
        cmd.ok = true;
        return;
    }
    if (!ctx.driver) {
        // Nothing loaded: keep the probed driver loaded for the linger window,
        // a probe is usually followed by an open.
        if (!LoadDriver(ctx, name, cmd.error)) return;
        ctx.lingerUntil = GetTickCount64() + G().lingerMs;
        QueryCaps(ctx.driver, cmd.caps);
        cmd.ok = true;
        return;
    }
    const std::vector<AsioDriverEntry> list = CurrentDrivers();
    const AsioDriverEntry* entry = FindDriver(list, name);
    if (!entry) {
        cmd.error = "no ASIO driver named \"" + name + "\"";
        return;
    }
    IASIO* d = CreateDriver(ctx, *entry, cmd.error);
    if (!d) return;
    QueryCaps(d, cmd.caps);
    SafeRelease(d);
    CoFreeUnusedLibraries();
    cmd.ok = true;
}

void HostThread(Context* ctx) {
    Impl& g = G();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ctx->window = CreateHostWindow();
    bool quit = false;
    while (!quit) {
        DWORD timeout = INFINITE;
        const ULONGLONG now = GetTickCount64();
        if (!ctx->stream && ctx->driver)
            timeout = ctx->lingerUntil > now ? static_cast<DWORD>(ctx->lingerUntil - now) : 0;
        MsgWaitForMultipleObjectsEx(1, &ctx->wake, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        for (;;) {
            std::shared_ptr<Command> cmd;
            {
                std::lock_guard<std::mutex> lock(g.mutex);
                if (ctx->queue.empty()) break;
                cmd = ctx->queue.front();
                ctx->queue.pop_front();
            }
            cmd->fn(*ctx, *cmd);
            if (ctx->poisoned) return;   // abandoned while inside the driver: leak
            SetEvent(cmd->done);
            if (cmd->quit) quit = true;
        }
        if (!ctx->stream && ctx->driver && GetTickCount64() >= ctx->lingerUntil)
            ReleaseDriver(*ctx);
        if (!ctx->stream && !ctx->driver) {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (ctx->queue.empty()) quit = true;
        }
        if (quit) {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (!ctx->queue.empty()) {
                quit = false;   // a command arrived while deciding to leave
                continue;
            }
            if (g.ctx == ctx) g.ctx = nullptr;
        }
    }
    if (ctx->window) DestroyWindow(ctx->window);
    CloseHandle(ctx->wake);
    delete ctx;
    CoUninitialize();
}

// Caller holds g.mutex. Returns the live context, starting a thread if needed.
Context* EnsureContextLocked(Impl& g) {
    if (g.ctx) return g.ctx;
    if (g.thread.joinable()) g.exited.push_back(std::move(g.thread));
    g.ctx = new Context();
    g.thread = std::thread(&HostThread, g.ctx);
    return g.ctx;
}

void JoinExited() {
    Impl& g = G();
    std::vector<std::thread> done;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        done.swap(g.exited);
    }
    for (std::thread& t : done) t.join();
}

// The host thread hung inside a driver: leave it and its driver behind.
void AbandonLocked(Impl& g, Context* ctx) {
    if (g.ctx != ctx) return;
    ctx->poisoned = true;
    for (int i = 0; i < kSlots; ++i) {
        Stream* s = g_slot[i].load();
        if (s && s->ctx == ctx) g_slotPoisoned[i] = true;
    }
    if (g.thread.joinable()) g.thread.detach();
    g.ctx = nullptr;
}

bool Post(std::shared_ptr<Command> cmd) {
    Impl& g = G();
    Context* ctx;
    uint32_t timeout;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        ctx = EnsureContextLocked(g);
        ctx->queue.push_back(cmd);
        timeout = g.commandTimeoutMs;
        SetEvent(ctx->wake);
    }
    JoinExited();
    if (WaitForSingleObject(cmd->done, timeout) == WAIT_OBJECT_0) return true;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        AbandonLocked(g, ctx);
    }
    {
        std::lock_guard<std::mutex> lock(g.attachMutex);
        g.running = false;
        g.attachedId = 0;
    }
    cmd->error = "ASIO driver not responding";
    return false;
}

} // namespace

AsioHost& AsioHost::Instance() {
    static AsioHost host;
    return host;
}

void AsioHost::SetFactoryForTest(AsioFactory factory, void* user,
                                 std::vector<AsioDriverEntry> drivers) {
    Impl& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.factory = factory ? factory : &DefaultFactory;
    g.factoryUser = user;
    g.testDrivers = factory != nullptr;
    g.drivers = std::move(drivers);
}

void AsioHost::SetTimingForTest(uint32_t commandTimeoutMs, uint32_t lingerMs,
                                uint32_t debounceMs) {
    Impl& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.commandTimeoutMs = commandTimeoutMs;
    g.lingerMs = lingerMs;
    g.debounceMs = debounceMs;
}

bool AsioHost::ThreadRunningForTest() {
    Impl& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.ctx != nullptr;
}

std::vector<AsioDriverEntry> AsioHost::ListDrivers() { return CurrentDrivers(); }

bool AsioHost::Probe(const std::string& name, AsioCaps& caps, std::string& error) {
    auto cmd = std::make_shared<Command>();
    cmd->fn = [name](Context& ctx, Command& c) { DoProbe(ctx, c, name); };
    const bool posted = Post(cmd);
    if (posted && cmd->ok) {
        caps = cmd->caps;
        return true;
    }
    error = cmd->error;
    return false;
}

bool AsioHost::Open(const std::string& name, uint32_t rate, uint32_t frames,
                    uint32_t firstChannel, RenderFn render, void* user, EventFn onEvent,
                    AsioStreamInfo& info, std::string& error) {
    Impl& g = G();
    DetachClient();
    g.render = render;
    g.renderUser = user;
    g.onEvent = onEvent;
    auto cmd = std::make_shared<Command>();
    cmd->fn = [name, rate, frames, firstChannel](Context& ctx, Command& c) {
        DoOpen(ctx, c, name, rate, frames, firstChannel);
    };
    const bool posted = Post(cmd);
    if (posted && cmd->ok) {
        info = cmd->info;
        return true;
    }
    error = cmd->error;
    return false;
}

bool AsioHost::Start(std::string& error) {
    Impl& g = G();
    {
        std::lock_guard<std::mutex> lock(g.attachMutex);
        g.running = true;
    }
    auto cmd = std::make_shared<Command>();
    cmd->fn = [](Context& ctx, Command& c) { DoStart(ctx, c); };
    if (Post(cmd) && cmd->ok) return true;
    DetachClient();
    error = cmd->error;
    return false;
}

void AsioHost::Stop() {
    DetachClient();
    Impl& g = G();
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.ctx) return;
    }
    auto cmd = std::make_shared<Command>();
    cmd->fn = [](Context& ctx, Command& c) {
        if (ctx.started) {
            ctx.driver->stop();
            ctx.started = false;
        }
        c.ok = true;
    };
    Post(cmd);
}

void AsioHost::Close() {
    DetachClient();
    Impl& g = G();
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.ctx) return;
    }
    auto cmd = std::make_shared<Command>();
    cmd->fn = [](Context& ctx, Command& c) {
        CloseStream(ctx);
        c.ok = true;
    };
    Post(cmd);
}

void AsioHost::ShutdownForProcessEnd() {
    DetachClient();
    Impl& g = G();
    Context* quitting = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        quitting = g.ctx;
    }
    if (quitting) {
        auto cmd = std::make_shared<Command>();
        cmd->quit = true;
        cmd->fn = [](Context& ctx, Command& c) {
            CloseStream(ctx);
            ReleaseDriver(ctx);
            c.ok = true;
        };
        Post(cmd);
    }
    std::thread last;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.ctx || g.ctx == quitting) {
            last = std::move(g.thread);
            g.ctx = nullptr;
        }
    }
    if (last.joinable()) last.join();
    JoinExited();
}

uint64_t AsioHost::XrunCount() const { return G().xruns.load(); }

} // namespace svms
