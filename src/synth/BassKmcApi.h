// The KMC/BASSMIDI-mode renderer as the bass.dll shim sees it. Only the bass.dll
// build links the renderer (synth/OfflineRenderer.cpp); SvmsBassKmcApi() is null
// everywhere else.
#pragma once
#include <cstdint>

struct SvmsKmcApi {
    void* (*Open)(const wchar_t* midi, const wchar_t* soundfont, uint32_t rate, uint32_t voices);
    uint32_t (*Read)(void* handle, float* left, float* right, uint32_t frames);
    uint32_t (*Active)(void* handle);
    void (*Close)(void* handle);
    void (*Channels)(void* handle, uint32_t* out16);  // live voices per MIDI channel
};
extern "C" const SvmsKmcApi* SvmsBassKmcApi();
