// A fake IASIO driver for the host tests. It behaves like a strict hardware
// driver: every method except outputReady must run on the thread that
// created the instance (otherwise g_fake.violation is set), and it streams
// from its own callback thread like a real driver.
#ifndef SVMS_FAKE_ASIO_DRIVER_H
#define SVMS_FAKE_ASIO_DRIVER_H

#include "driver/AsioHost.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace fake {

struct State {
    std::atomic<int> loadCount{0};          // successful creations
    std::atomic<int> liveInstances{0};
    std::atomic<int> maxLiveInstances{0};
    std::atomic<int> createBuffersCount{0};
    std::atomic<bool> violation{false};
    std::atomic<int> failLoadTimes{0};
    std::atomic<int> hangInStopMs{0};
    std::atomic<int> hangInInitMs{0};
    std::atomic<bool> crashInCreate{false};
    std::atomic<int> reenterEveryN{0};
    std::atomic<long> preferred{32};
    std::atomic<double> rate{44100.0};
    void Reset() {
        loadCount = 0; liveInstances = 0; maxLiveInstances = 0;
        createBuffersCount = 0; violation = false; failLoadTimes = 0;
        hangInStopMs = 0; hangInInitMs = 0; crashInCreate = false;
        reenterEveryN = 0; preferred = 32; rate = 44100.0;
    }
};
inline State g_fake;

class Driver;
inline std::atomic<Driver*> g_current{nullptr};   // last created instance

class Driver : public IASIO {
public:
    Driver() : owner_(GetCurrentThreadId()) {
        const int live = ++g_fake.liveInstances;
        int seen = g_fake.maxLiveInstances.load();
        while (live > seen && !g_fake.maxLiveInstances.compare_exchange_weak(seen, live)) {}
        ++g_fake.loadCount;
        g_current = this;
    }
    ~Driver() {
        StopThread();
        --g_fake.liveInstances;
        Driver* self = this;
        g_current.compare_exchange_strong(self, nullptr);
    }

    // Test-side triggers (any thread).
    void RaiseMessage(long selector) {
        if (ASIOCallbacks* cb = cb_) cb->asioMessage(selector, 0, nullptr, nullptr);
    }
    void RaiseRateChange(double rate) {
        g_fake.rate = rate;
        if (ASIOCallbacks* cb = cb_) cb->sampleRateDidChange(rate);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        Check();
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // IASIO
    ASIOBool init(void* sysHandle) override {
        Check();
        if (!sysHandle || !IsWindow(static_cast<HWND>(sysHandle))) g_fake.violation = true;
        if (const int ms = g_fake.hangInInitMs.load()) Sleep(ms);
        return ASIOTrue;
    }
    void getDriverName(char* name) override { Check(); strcpy(name, "Fake"); }
    long getDriverVersion() override { Check(); return 1; }
    void getErrorMessage(char* s) override { Check(); strcpy(s, "fake error"); }
    ASIOError start() override {
        Check();
        if (!cb_) return ASE_NotPresent;
        StopThread();
        quit_ = false;
        thread_ = std::thread([this] { Run(); });
        reenter_ = std::thread([this] { RunReenter(); });
        return ASE_OK;
    }
    ASIOError stop() override {
        Check();
        if (const int ms = g_fake.hangInStopMs.load()) Sleep(ms);
        StopThread();
        return ASE_OK;
    }
    ASIOError getChannels(long* in, long* out) override {
        Check(); *in = 0; *out = 8; return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override {
        Check(); *in = 0; *out = frames_ + 16; return ASE_OK;
    }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        Check(); *mn = 8; *mx = 2048; *pref = g_fake.preferred; *gran = -1; return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        Check();
        return (r == 44100.0 || r == 48000.0) ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override { Check(); *r = g_fake.rate; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        Check();
        if (r != 44100.0 && r != 48000.0) return ASE_NoClock;
        g_fake.rate = r; return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource*, long* n) override { Check(); *n = 0; return ASE_OK; }
    ASIOError setClockSource(long) override { Check(); return ASE_OK; }
    ASIOError getSamplePosition(ASIOSamples*, ASIOTimeStamp*) override { return ASE_NotPresent; }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        Check();
        if (info->isInput || info->channel < 0 || info->channel >= 8) return ASE_InvalidParameter;
        info->isActive = ASIOTrue;
        info->channelGroup = 0;
        info->type = ASIOSTFloat32LSB;
        std::snprintf(info->name, sizeof(info->name), "Out %ld", info->channel + 1);
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long n, long size, ASIOCallbacks* cb) override {
        Check();
        if (size < 8 || size > 2048 || n != 2) return ASE_InvalidMode;
        frames_ = size;
        for (long c = 0; c < n; ++c) {
            if (infos[c].isInput || infos[c].channelNum < 0 || infos[c].channelNum >= 8)
                return ASE_InvalidParameter;
            mem_[c].assign(static_cast<size_t>(size) * 2u, 0.0f);
            infos[c].buffers[0] = mem_[c].data();
            infos[c].buffers[1] = mem_[c].data() + size;
        }
        cb_ = cb;
        ++g_fake.createBuffersCount;
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        Check();
        StopThread();
        cb_ = nullptr;
        for (auto& m : mem_) std::vector<float>().swap(m);
        return ASE_OK;
    }
    ASIOError controlPanel() override { g_fake.violation = true; return ASE_OK; }
    ASIOError future(long, void*) override { Check(); return ASE_InvalidParameter; }
    ASIOError outputReady() override { return ASE_OK; }   // callback thread, by design

private:
    void Check() {
        if (GetCurrentThreadId() != owner_) {
            std::fprintf(stderr, "fake ASIO: called from thread %lu, created on %lu\n",
                         GetCurrentThreadId(), owner_);
            g_fake.violation = true;
        }
    }
    void StopThread() {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
        if (reenter_.joinable()) reenter_.join();
    }
    void Run() {
        long index = 0;
        int tick = 0;
        const DWORD periodMs = (std::max)(1L, static_cast<long>(frames_ * 1000 / 44100));
        while (!quit_) {
            ASIOCallbacks* cb = cb_;
            if (!cb) break;
            const int n = g_fake.reenterEveryN.load();
            if (n > 0 && (++tick % n) == 0) fireReenter_ = 1;
            cb->bufferSwitch(index, ASIOTrue);
            index ^= 1;
            Sleep(periodMs);
        }
    }
    // Calls bufferSwitch concurrently with the main callback thread.
    void RunReenter() {
        while (!quit_) {
            if (fireReenter_.exchange(0)) {
                if (ASIOCallbacks* cb = cb_) cb->bufferSwitch(1, ASIOTrue);
            }
            Sleep(0);
        }
    }

    DWORD owner_;
    std::atomic<ULONG> refs_{1};
    std::atomic<ASIOCallbacks*> cb_{nullptr};
    std::vector<float> mem_[2];
    long frames_ = 32;
    std::atomic<bool> quit_{false};
    std::atomic<int> fireReenter_{0};
    std::thread thread_;
    std::thread reenter_;
};

// Fake drivers "Fake A" and "Fake B" share the knobs above.
inline HRESULT Factory(const svms::AsioDriverEntry&, IASIO** out, void*) {
    if (g_fake.crashInCreate) {
        volatile int* p = nullptr;
        *p = 1;
    }
    if (g_fake.failLoadTimes.load() > 0) {
        --g_fake.failLoadTimes;
        return E_FAIL;
    }
    *out = new Driver();
    return S_OK;
}

inline std::vector<svms::AsioDriverEntry> Drivers() {
    return {{"Fake A", CLSID{}}, {"Fake B", CLSID{}}};
}

} // namespace fake

#endif // SVMS_FAKE_ASIO_DRIVER_H
