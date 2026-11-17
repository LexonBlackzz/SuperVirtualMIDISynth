// ASIO host tests: legal buffer sizes and the owner-thread host driven by a
// fake IASIO driver. Runs everywhere ASIO is built; needs no audio hardware.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "driver/AsioSizes.h"
#include "tests/FakeAsioDriver.h"

using namespace svms;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using Sizes = std::vector<uint32_t>;

static void TestLegalSizes() {
    CHECK(AsioLegalSizes(8, 2048, 32, -1) == (Sizes{8, 16, 32, 64, 128, 256, 512, 1024, 2048}));
    CHECK(AsioLegalSizes(256, 256, 256, 0) == (Sizes{256}));
    CHECK(AsioLegalSizes(64, 512, 128, 0) == (Sizes{64, 512}));
    CHECK(AsioLegalSizes(96, 480, 96, 96) == (Sizes{96, 192, 288, 384, 480}));
    CHECK(AsioLegalSizes(48, 2048, 48, -1) == (Sizes{48, 64, 128, 256, 512, 1024, 2048}));
    CHECK(AsioLegalSizes(64, 1000, 64, -1) == (Sizes{64, 128, 256, 512, 1000}));
    CHECK(AsioLegalSizes(0, 0, 0, 0).empty());
    CHECK(AsioLegalSizes(512, 256, 256, -1).empty());
}

static void TestNearestLegalSize() {
    const Sizes p2{8, 16, 32, 64, 128, 256, 512, 1024, 2048};
    CHECK(AsioNearestLegalSize(p2, 32) == 32);
    CHECK(AsioNearestLegalSize(p2, 24) == 32);   // tie 16/32 -> larger
    CHECK(AsioNearestLegalSize(p2, 20) == 16);
    CHECK(AsioNearestLegalSize(p2, 1) == 8);
    CHECK(AsioNearestLegalSize(p2, 100000) == 2048);
    CHECK(AsioNearestLegalSize({}, 64) == 64);   // no list: request unchanged
}

// ---- host with the fake driver -------------------------------------------

static bool WaitFor(const std::function<bool()>& cond, int ms) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
    while (GetTickCount64() < end) {
        if (cond()) return true;
        Sleep(1);
    }
    return cond();
}

static int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

struct Counter {
    std::atomic<int> calls{0};
    std::atomic<int> depth{0};
    std::atomic<int> maxDepth{0};
    std::atomic<int> sleepMs{0};
    std::atomic<int64_t> lastExitQpc{0};
    std::atomic<uint32_t> frames{0};
};

static void CountRender(float* lr, uint32_t frames, void* user) {
    Counter* c = static_cast<Counter*>(user);
    const int d = ++c->depth;
    int seen = c->maxDepth.load();
    while (d > seen && !c->maxDepth.compare_exchange_weak(seen, d)) {}
    c->frames = frames;
    std::memset(lr, 0, sizeof(float) * 2u * frames);
    if (const int ms = c->sleepMs.load()) Sleep(static_cast<DWORD>(ms));
    ++c->calls;
    c->lastExitQpc = Qpc();
    --c->depth;
}

static AsioHost& FreshHost() {
    AsioHost& h = AsioHost::Instance();
    h.ShutdownForProcessEnd();
    fake::g_fake.Reset();
    h.SetFactoryForTest(&fake::Factory, nullptr, fake::Drivers());
    h.SetTimingForTest(5000, 300, 100);
    return h;
}

static bool OpenStart(AsioHost& h, const char* name, uint32_t frames, Counter& c,
                      AsioStreamInfo* infoOut = nullptr, uint32_t first = 0) {
    AsioStreamInfo info;
    std::string err;
    if (!h.Open(name, 44100, frames, first, &CountRender, &c, nullptr, info, err)) {
        std::fprintf(stderr, "Open failed: %s\n", err.c_str());
        return false;
    }
    if (infoOut) *infoOut = info;
    if (!h.Start(err)) {
        std::fprintf(stderr, "Start failed: %s\n", err.c_str());
        return false;
    }
    return true;
}

static void HostOpenStartStop() {
    AsioHost& h = FreshHost();
    Counter c;
    AsioStreamInfo info;
    CHECK(OpenStart(h, "Fake A", 32, c, &info));
    CHECK(info.frames == 32);
    CHECK(info.rate == 44100);
    CHECK(WaitFor([&] { return c.calls >= 10; }, 1000));
    CHECK(c.frames == 32);
    h.Stop();
    h.Close();
    const int after = c.calls;
    Sleep(50);
    CHECK(c.calls == after);
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

static void HostOpenCloseAcrossThreads() {
    AsioHost& h = FreshHost();
    for (int round = 0; round < 20; ++round) {
        Counter c;
        bool ok = false;
        std::thread([&] { ok = OpenStart(h, "Fake A", 32, c); }).join();
        CHECK(ok);
        CHECK(WaitFor([&] { return c.calls >= 3; }, 1000));
        std::thread([&] { h.Close(); }).join();
    }
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

static void HostReselect50() {
    AsioHost& h = FreshHost();
    for (int round = 0; round < 50; ++round) {
        Counter c;
        h.Close();
        CHECK(OpenStart(h, "Fake A", 32, c));
        CHECK(WaitFor([&] { return c.calls >= 3; }, 1000));
    }
    h.Close();
    CHECK(fake::g_fake.loadCount == 1);
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

static void HostCloseDuringCallback() {
    AsioHost& h = FreshHost();
    Counter c;
    c.sleepMs = 20;
    CHECK(OpenStart(h, "Fake A", 32, c));
    CHECK(WaitFor([&] { return c.calls >= 2; }, 1000));
    CHECK(WaitFor([&] { return c.depth.load() == 1; }, 1000));
    h.Close();
    const int64_t closed = Qpc();
    CHECK(c.depth == 0);
    CHECK(c.lastExitQpc <= closed);
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

static void HostReentry() {
    AsioHost& h = FreshHost();
    fake::g_fake.reenterEveryN = 3;
    Counter c;
    c.sleepMs = 2;
    CHECK(OpenStart(h, "Fake A", 32, c));
    Sleep(500);
    const int mid = c.calls;
    CHECK(mid > 10);
    CHECK(WaitFor([&] { return c.calls > mid + 5; }, 1000));
    CHECK(c.maxDepth == 1);
    h.Close();
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

static void HostIllegalSizeAndPair() {
    AsioHost& h = FreshHost();
    Counter c;
    AsioStreamInfo info;
    CHECK(OpenStart(h, "Fake A", 24, c, &info, 7));
    CHECK(info.frames == 32);
    CHECK(info.firstChannel == 0);
    CHECK(!info.note.empty());
    AsioStreamInfo info2;
    Counter c2;
    h.Close();
    CHECK(OpenStart(h, "Fake A", 64, c2, &info2, 2));
    CHECK(info2.frames == 64);
    CHECK(info2.firstChannel == 2);
    CHECK(info2.note.empty());
    h.Close();
    h.ShutdownForProcessEnd();
}

static void HostProbe() {
    AsioHost& h = FreshHost();
    AsioCaps caps;
    std::string err;
    CHECK(h.Probe("Fake A", caps, err));
    CHECK(caps.legalSizes == (Sizes{8, 16, 32, 64, 128, 256, 512, 1024, 2048}));
    CHECK(caps.outputChannels == 8);
    CHECK(caps.outputNames.size() == 8 && caps.outputNames[2] == "Out 3");
    CHECK(caps.rates == (Sizes{44100, 48000}));
    CHECK(!h.Probe("No Such Driver", caps, err));
    CHECK(!err.empty());
    Counter c;
    CHECK(OpenStart(h, "Fake A", 32, c));
    fake::g_fake.maxLiveInstances = fake::g_fake.liveInstances.load();
    AsioCaps caps2;
    CHECK(h.Probe("Fake A", caps2, err));
    CHECK(caps2.legalSizes == caps.legalSizes);
    CHECK(fake::g_fake.maxLiveInstances == 1);
    CHECK(WaitFor([&] { return c.calls > 0; }, 1000));
    h.Close();
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

// ---- driver messages, failures and lifetime --------------------------------

struct Events {
    std::atomic<int> reopened{0};
    std::atomic<int> dead{0};
    std::atomic<uint32_t> frames{0};
    std::atomic<uint32_t> rate{0};
};

struct EventCounter : Counter {
    Events ev;
};

static void OnEvent(const AsioStreamInfo& info, bool dead, void* user) {
    Events& e = static_cast<EventCounter*>(user)->ev;
    if (dead) {
        ++e.dead;
        return;
    }
    e.frames = info.frames;
    e.rate = info.rate;
    ++e.reopened;
}

static AsioHost& FreshHostFast() {
    AsioHost& h = FreshHost();
    h.SetTimingForTest(500, 300, 100);
    return h;
}

static bool OpenStartEv(AsioHost& h, const char* name, uint32_t frames, EventCounter& c) {
    AsioStreamInfo info;
    std::string err;
    if (!h.Open(name, 44100, frames, 0, &CountRender, &c, &OnEvent, info, err)) {
        std::fprintf(stderr, "Open failed: %s\n", err.c_str());
        return false;
    }
    return h.Start(err);
}

static fake::Driver* Current() { return fake::g_current.load(); }

static void HostResetDebounce() {
    AsioHost& h = FreshHostFast();
    EventCounter c;
    CHECK(OpenStartEv(h, "Fake A", 32, c));
    CHECK(WaitFor([&] { return c.calls >= 3; }, 1000));
    const int base = fake::g_fake.createBuffersCount;
    for (int i = 0; i < 10; ++i) {
        CHECK(Current()->RaiseMessage(kAsioResetRequest) == 1);
        Sleep(5);
    }
    CHECK(WaitFor([&] { return c.ev.reopened >= 1; }, 1000));
    Sleep(300);
    CHECK(c.ev.reopened == 1);
    CHECK(fake::g_fake.createBuffersCount == base + 1);
    const int after = c.calls;
    CHECK(WaitFor([&] { return c.calls > after + 3; }, 1000));
    h.Close();
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

static void HostResetKeepsOurSize() {
    AsioHost& h = FreshHostFast();
    EventCounter c;
    CHECK(OpenStartEv(h, "Fake A", 32, c));
    CHECK(WaitFor([&] { return c.calls >= 3; }, 1000));
    fake::g_fake.preferred = 256;
    Current()->RaiseMessage(kAsioResetRequest);
    CHECK(WaitFor([&] { return c.ev.reopened >= 1; }, 1000));
    CHECK(c.ev.frames == 32);
    const int after = c.calls;
    CHECK(WaitFor([&] { return c.calls > after + 3; }, 1000));
    CHECK(c.frames == 32);
    h.Close();
    h.ShutdownForProcessEnd();
}

static void HostResyncAndRateChange() {
    AsioHost& h = FreshHostFast();
    EventCounter c;
    CHECK(OpenStartEv(h, "Fake A", 32, c));
    CHECK(WaitFor([&] { return c.calls >= 3; }, 1000));
    CHECK(Current()->RaiseMessage(kAsioResyncRequest) == 1);
    CHECK(WaitFor([&] { return c.ev.reopened == 1; }, 1000));
    Sleep(200);
    Current()->RaiseRateChange(48000.0);
    CHECK(WaitFor([&] { return c.ev.reopened == 2; }, 1000));
    CHECK(c.ev.rate == 48000);
    CHECK(c.ev.frames == 32);
    h.Close();
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

static void HostLatenciesAndOverload() {
    AsioHost& h = FreshHostFast();
    EventCounter c;
    CHECK(OpenStartEv(h, "Fake A", 32, c));
    const int base = fake::g_fake.createBuffersCount;
    const uint64_t xruns = h.XrunCount();
    CHECK(Current()->RaiseMessage(kAsioLatenciesChanged) == 1);
    for (int i = 0; i < 3; ++i) Current()->RaiseMessage(kAsioOverload);
    Sleep(300);
    CHECK(fake::g_fake.createBuffersCount == base);
    CHECK(c.ev.reopened == 0);
    CHECK(h.XrunCount() == xruns + 3);
    h.Close();
    h.ShutdownForProcessEnd();
}

static void HostLoadRetries() {
    AsioHost& h = FreshHostFast();
    fake::g_fake.failLoadTimes = 5;
    EventCounter c;
    CHECK(OpenStartEv(h, "Fake A", 32, c));
    h.Close();
    h.ShutdownForProcessEnd();

    fake::g_fake.failLoadTimes = 25;
    h.SetTimingForTest(5000, 300, 100);
    AsioStreamInfo info;
    std::string err;
    CHECK(!h.Open("Fake A", 44100, 32, 0, &CountRender, &c, nullptr, info, err));
    CHECK(err.find("Fake A") != std::string::npos);
    h.ShutdownForProcessEnd();
}

static void HostResetReloadFailsReportsDead() {
    AsioHost& h = FreshHost();
    h.SetTimingForTest(5000, 300, 100);
    EventCounter c;
    CHECK(OpenStartEv(h, "Fake A", 32, c));
    fake::g_fake.failLoadTimes = 25;
    Current()->RaiseMessage(kAsioResetRequest);
    CHECK(WaitFor([&] { return c.ev.dead == 1; }, 3000));
    const int after = c.calls;
    Sleep(100);
    CHECK(c.calls == after);
    h.Close();
    h.ShutdownForProcessEnd();
}

static void HostDriverHangTimesOut() {
    AsioHost& h = FreshHostFast();
    EventCounter c;
    CHECK(OpenStartEv(h, "Fake A", 32, c));
    fake::g_fake.hangInStopMs = 3000;
    const ULONGLONG t0 = GetTickCount64();
    h.Close();
    CHECK(GetTickCount64() - t0 < 1000);
    fake::g_fake.hangInStopMs = 0;
    EventCounter c2;
    CHECK(OpenStartEv(h, "Fake B", 32, c2));
    CHECK(WaitFor([&] { return c2.calls >= 3; }, 1000));
    h.Close();

    fake::g_fake.hangInInitMs = 3000;
    h.ShutdownForProcessEnd();   // unload B so the next open creates (and hangs)
    AsioStreamInfo info;
    std::string err;
    const ULONGLONG t1 = GetTickCount64();
    CHECK(!h.Open("Fake A", 44100, 32, 0, &CountRender, &c2, nullptr, info, err));
    CHECK(GetTickCount64() - t1 < 1000);
    CHECK(err.find("not responding") != std::string::npos);
    fake::g_fake.hangInInitMs = 0;
    EventCounter c3;
    CHECK(OpenStartEv(h, "Fake B", 32, c3));
    CHECK(WaitFor([&] { return c3.calls >= 3; }, 1000));
    h.Close();
    h.ShutdownForProcessEnd();
    Sleep(3200);   // let the abandoned threads wake up and leave
}

static void HostCrashInCreate() {
    AsioHost& h = FreshHostFast();
    fake::g_fake.crashInCreate = true;
    AsioStreamInfo info;
    std::string err;
    Counter c;
    CHECK(!h.Open("Fake A", 44100, 32, 0, &CountRender, &c, nullptr, info, err));
    CHECK(err.find("crashed") != std::string::npos);
    fake::g_fake.crashInCreate = false;
    CHECK(OpenStart(h, "Fake A", 32, c));
    h.Close();
    h.ShutdownForProcessEnd();
}

static void HostLingerUnload() {
    AsioHost& h = FreshHostFast();
    Counter c;
    CHECK(OpenStart(h, "Fake A", 32, c));
    h.Close();
    CHECK(fake::g_fake.liveInstances == 1);   // lingering
    CHECK(WaitFor([&] { return fake::g_fake.liveInstances == 0 && !h.ThreadRunningForTest(); },
                  1500));
    Counter c2;
    CHECK(OpenStart(h, "Fake A", 32, c2));
    CHECK(WaitFor([&] { return c2.calls >= 3; }, 1000));
    h.Close();
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

static void HostSwitchDrivers() {
    AsioHost& h = FreshHostFast();
    const char* order[] = {"Fake A", "Fake B", "Fake A"};
    for (const char* name : order) {
        Counter c;
        CHECK(OpenStart(h, name, 32, c));
        CHECK(WaitFor([&] { return c.calls >= 3; }, 1000));
    }
    h.Close();
    CHECK(fake::g_fake.maxLiveInstances == 1);
    CHECK(fake::g_fake.loadCount == 3);
    CHECK(!fake::g_fake.violation);
    h.ShutdownForProcessEnd();
}

int main() {
    TestLegalSizes();
    TestNearestLegalSize();
    HostOpenStartStop();
    HostOpenCloseAcrossThreads();
    HostReselect50();
    HostCloseDuringCallback();
    HostReentry();
    HostIllegalSizeAndPair();
    HostProbe();
    HostResetDebounce();
    HostResetKeepsOurSize();
    HostResyncAndRateChange();
    HostLatenciesAndOverload();
    HostLoadRetries();
    HostResetReloadFailsReportsDead();
    HostCrashInCreate();
    HostLingerUnload();
    HostSwitchDrivers();
    HostDriverHangTimesOut();   // last: leaves abandoned threads behind
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("asio host tests passed\n");
    return 0;
}
