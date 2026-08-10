#ifndef SVMS_DRIVER_H
#define SVMS_DRIVER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmeapi.h>
#include <cstdint>
#include <atomic>

#include "SVMSPSCQueue.h"

namespace svms {

class Driver {
public:
    static Driver& Instance();

    bool Initialize();
    void Shutdown();
    bool LoadSoundFont(const char* path);
    bool IsInitialized() const;

    void SubmitShortMsg(uint32_t msg);

    static Driver* instance;
    bool initialized;
    uint32_t sampleRate;
    uint32_t bufferFrames;

private:
    Driver();
    ~Driver();

    static void RenderCallback(float* output, uint32_t numFrames, void* userData);
    static void DispatchRenderEvent(const RenderEvent& event, uint32_t blockCursor,
                                     void* userData);

    void HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void HandleNoteOff(uint8_t channel, uint8_t note, uint32_t blockOffset);
    void HandleControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    void HandleProgramChange(uint8_t channel, uint8_t program);
    void HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb);

    // Lock-free SPSC queue: MIDI thread pushes, audio thread pops.
    // Capacity 16384 = 128 KB (8 bytes per event).  Power of two.
    SPSCQueue<TimestampedMidiEvent, kDefaultEventRingCapacity> midiEventQueue_;

    void* audioOutputPtr;
    void* voiceManagerPtr;
    void* channelCachePtr;
    void* renderScalarPtr;
    void* soundFontDataPtr;
    void* configSnapshotPtr;
    void* sampleDataPtr;
    uint32_t sampleCount;
    void* samplesPtr;

    CRITICAL_SECTION cs;
};

} // namespace svms

#endif
