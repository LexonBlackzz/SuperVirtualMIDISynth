// One thread owns every ASIO driver in the process.
//
// ASIO drivers are apartment-threaded COM objects, and hardware drivers
// (TOPPING, RME, ...) misbehave or crash when they are created on one thread
// and called or released on another. MIDI players open and close the synth
// from whatever thread they like, so nothing outside this host touches a
// driver: every call is posted to the host's own STA thread, which pumps
// window messages, owns the driver for as long as it is loaded, and keeps it
// loaded for a short linger window after a close so a quick reopen does not
// unload and reload it (the reload is where hardware drivers race their own
// teardown).
//
// Drivers are talked to through IASIO directly (registry CLSID +
// CoCreateInstance), not through the SDK's process-global C facade.
#ifndef SVMS_ASIO_HOST_H
#define SVMS_ASIO_HOST_H

#include <windows.h>
#include <objbase.h>
#include <cstdint>
#include <string>
#include <vector>
#include "asiosys.h"   // defines IEEE754_64FLOAT on Windows; must precede asio.h
#include "asio.h"
#include "iasiodrv.h"
#include "driver/AsioSizes.h"

namespace svms {

struct AsioCaps {
    long minSize = 0, maxSize = 0, preferredSize = 0, granularity = 0;
    std::vector<uint32_t> legalSizes;
    uint32_t outputChannels = 0;
    std::vector<std::string> outputNames;   // as the driver names them
    std::vector<uint32_t> rates;            // of 44100/48000/88200/96000
    uint32_t currentRate = 0;
    long outputLatencyFrames = 0;
};

struct AsioStreamInfo {
    std::string driver;
    uint32_t frames = 0;
    uint32_t rate = 0;
    uint32_t firstChannel = 0;
    long outputLatencyFrames = 0;
    std::string note;   // why the stream differs from the request, if it does
};

struct AsioDriverEntry {
    std::string name;   // registry "description", else the key name
    CLSID clsid{};
    std::string key;    // registry key name (also accepted when opening)
};

// Creates a driver instance. The default is CoCreateInstance(clsid).
using AsioFactory = HRESULT (*)(const AsioDriverEntry& entry, IASIO** out, void* user);

class AsioHost {
public:
    using RenderFn = void (*)(float* interleavedLR, uint32_t frames, void* user);
    // Called on the host thread after the host reopened the stream by itself
    // (driver reset, rate change), or with streamDead when it gave up.
    using EventFn = void (*)(const AsioStreamInfo& info, bool streamDead, void* user);

    static AsioHost& Instance();

    void SetFactoryForTest(AsioFactory factory, void* user,
                           std::vector<AsioDriverEntry> drivers);
    void SetTimingForTest(uint32_t commandTimeoutMs, uint32_t lingerMs,
                          uint32_t debounceMs);
    bool ThreadRunningForTest();

    std::vector<AsioDriverEntry> ListDrivers();
    bool Probe(const std::string& name, AsioCaps& caps, std::string& error);

    // One stream at a time; opening closes the previous one. render and
    // onEvent receive `user`.
    bool Open(const std::string& name, uint32_t rate, uint32_t frames,
              uint32_t firstChannel, RenderFn render, void* user, EventFn onEvent,
              AsioStreamInfo& info, std::string& error);
    bool Start(std::string& error);
    void Stop();    // returns once the render function is no longer running
    void Close();   // Stop + free the buffers; the driver lingers
    // Engine shutdown (never from DllMain): unload the driver, end the thread.
    void ShutdownForProcessEnd();
    uint64_t XrunCount() const;

private:
    AsioHost() = default;
};

} // namespace svms

#endif // SVMS_ASIO_HOST_H
