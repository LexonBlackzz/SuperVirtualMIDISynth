#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmeapi.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "SVMSTypes.h"

#if defined(__MINGW32__)
// MinGW's ksmedia.h only declares (never defines) the KS subtype GUIDs; provide
// the constant so the test links without ksuser.lib.
static const GUID kKsSubTypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kKsSubTypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
#else
static const GUID& kKsSubTypeIeeeFloat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
static const GUID& kKsSubTypePcm = KSDATAFORMAT_SUBTYPE_PCM;
#endif

template <typename T>
static void ReleaseCom(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

static HRESULT SelectRenderDevice(IMMDeviceEnumerator* enumerator,
                                  const wchar_t* requested,
                                  IMMDevice** selected) {
    if (!requested || !*requested)
        return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, selected);
    IMMDeviceCollection* collection = nullptr;
    HRESULT hr = enumerator->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) return hr;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count && !*selected; ++index) {
        IMMDevice* candidate = nullptr;
        IPropertyStore* properties = nullptr;
        PROPVARIANT friendlyName;
        PropVariantInit(&friendlyName);
        if (SUCCEEDED(collection->Item(index, &candidate)) && candidate &&
            SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &properties)) &&
            properties &&
            SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,
                                           &friendlyName)) &&
            friendlyName.vt == VT_LPWSTR && friendlyName.pwszVal &&
            _wcsicmp(friendlyName.pwszVal, requested) == 0) {
            *selected = candidate;
            candidate = nullptr;
        }
        PropVariantClear(&friendlyName);
        ReleaseCom(properties);
        ReleaseCom(candidate);
    }
    collection->Release();
    return *selected ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 5) {
        fwprintf(stderr,
            L"usage: svms_v3_live_smoke <winmm.dll> [repeat-count] [midi-note] [output-device]\n");
        return 2;
    }
    const uint32_t repeatCount = argc >= 3
        ? static_cast<uint32_t>(wcstoul(argv[2], nullptr, 10)) : 1u;
    const uint32_t midiNote = argc >= 4
        ? static_cast<uint32_t>(wcstoul(argv[3], nullptr, 10)) : 60u;
    const wchar_t* outputDevice = argc >= 5 ? argv[4] : nullptr;
    if (repeatCount > 4096 || midiNote > 127) return 2;

    std::printf("sizeof_waveformatex=%zu sizeof_extensible=%zu offsets=%zu,%zu,%zu\n",
                sizeof(WAVEFORMATEX), sizeof(WAVEFORMATEXTENSIBLE),
                offsetof(WAVEFORMATEXTENSIBLE, Samples),
                offsetof(WAVEFORMATEXTENSIBLE, dwChannelMask),
                offsetof(WAVEFORMATEXTENSIBLE, SubFormat));
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 3;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* captureClient = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;
    HMODULE synth = nullptr;
    HMIDIOUT midi = nullptr;

    auto cleanup = [&] {
        if (captureClient) captureClient->Stop();
        ReleaseCom(capture);
        ReleaseCom(captureClient);
        ReleaseCom(device);
        ReleaseCom(enumerator);
        if (format) CoTaskMemFree(format);
        if (synth) FreeLibrary(synth);
        CoUninitialize();
    };

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) { cleanup(); return 4; }
    hr = SelectRenderDevice(enumerator, outputDevice, &device);
    if (FAILED(hr) || !device) { cleanup(); return 5; }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&captureClient));
    if (FAILED(hr) || !captureClient) { cleanup(); return 6; }
    hr = captureClient->GetMixFormat(&format);
    if (FAILED(hr) || !format) { cleanup(); return 7; }
    std::printf("mix_format_tag=0x%04X channels=%u rate=%lu bits=%u valid_bits=%u\n",
                format->wFormatTag, format->nChannels,
                static_cast<unsigned long>(format->nSamplesPerSec),
                format->wBitsPerSample,
                format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                    ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->Samples.wValidBitsPerSample
                    : format->wBitsPerSample);
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
        std::printf("mix_subformat=%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X "
                    "float=%u pcm=%u mask=0x%08lX\n",
                    static_cast<unsigned long>(ext->SubFormat.Data1), ext->SubFormat.Data2,
                    ext->SubFormat.Data3, ext->SubFormat.Data4[0], ext->SubFormat.Data4[1],
                    ext->SubFormat.Data4[2], ext->SubFormat.Data4[3], ext->SubFormat.Data4[4],
                    ext->SubFormat.Data4[5], ext->SubFormat.Data4[6], ext->SubFormat.Data4[7],
                    IsEqualGUID(ext->SubFormat, kKsSubTypeIeeeFloat) ? 1u : 0u,
                    IsEqualGUID(ext->SubFormat, kKsSubTypePcm) ? 1u : 0u,
                    static_cast<unsigned long>(ext->dwChannelMask));
    }
    hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, format, nullptr);
    if (FAILED(hr)) { cleanup(); return 8; }
    hr = captureClient->GetService(__uuidof(IAudioCaptureClient),
                                   reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) { cleanup(); return 9; }
    hr = captureClient->Start();
    if (FAILED(hr)) { cleanup(); return 10; }

    if (outputDevice) SetEnvironmentVariableW(L"SVMS_AUDIO_DEVICE", outputDevice);
    synth = LoadLibraryW(argv[1]);
    if (!synth) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        cleanup();
        return 11;
    }
    using OpenProc = MMRESULT (WINAPI*)(LPHMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD);
    using ShortProc = MMRESULT (WINAPI*)(HMIDIOUT, DWORD);
    using CloseProc = MMRESULT (WINAPI*)(HMIDIOUT);
    using DebugProc = DWORD (WINAPI*)(LPVOID, DWORD);
    using VoiceStatsProc = svms::SnappyVoiceStatistics* (WINAPI*)(
        svms::SnappyVoiceStatistics*);
    using VoiceCountProc = DWORD (WINAPI*)();
    using RenderingTimeProc = FLOAT (WINAPI*)();
    using LegacyDebugProc = svms::LegacyDriverDebugInfo* (WINAPI*)();
    auto open = reinterpret_cast<OpenProc>(GetProcAddress(synth, "midiOutOpen"));
    auto shortMessage = reinterpret_cast<ShortProc>(GetProcAddress(synth, "midiOutShortMsg"));
    auto close = reinterpret_cast<CloseProc>(GetProcAddress(synth, "midiOutClose"));
    auto debug = reinterpret_cast<DebugProc>(GetProcAddress(synth, "SVMSGetDriverDebugInfoV1"));
    auto voiceStats = reinterpret_cast<VoiceStatsProc>(
        GetProcAddress(synth, "GetVoiceStatistics"));
    auto voiceCount = reinterpret_cast<VoiceCountProc>(
        GetProcAddress(synth, "GetVoiceCount"));
    auto renderingTime = reinterpret_cast<RenderingTimeProc>(
        GetProcAddress(synth, "GetRenderingTime"));
    auto legacyDebug = reinterpret_cast<LegacyDebugProc>(
        GetProcAddress(synth, "GetDriverDebugInfo"));
    if (!open || !shortMessage || !close || !debug || !voiceStats ||
        !voiceCount || !renderingTime || !legacyDebug) {
        cleanup();
        return 12;
    }

    const MMRESULT openResult = open(&midi, 0, 0, 0, CALLBACK_NULL);
    if (openResult != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "midiOutOpen failed: %u\n", openResult);
        cleanup();
        return 13;
    }
    const DWORD noteOn = 0x007F0090u | (midiNote << 8u);
    const DWORD noteOff = 0x00000080u | (midiNote << 8u);
    for (uint32_t i = 0; i < repeatCount; ++i) shortMessage(midi, noteOn);

    const bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat,
                     kKsSubTypeIeeeFloat));
    float peak = 0.0f;
    uint64_t capturedFrames = 0;
    const ULONGLONG deadline = GetTickCount64() + 2500;
    while (GetTickCount64() < deadline) {
        UINT32 packetFrames = 0;
        if (FAILED(capture->GetNextPacketSize(&packetFrames))) break;
        if (packetFrames == 0) { Sleep(5); continue; }

        BYTE* bytes = nullptr;
        DWORD flags = 0;
        UINT64 devicePosition = 0;
        UINT64 qpcPosition = 0;
        if (FAILED(capture->GetBuffer(&bytes, &packetFrames, &flags,
                                      &devicePosition, &qpcPosition))) break;
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && bytes) {
            const uint32_t channels = format->nChannels;
            for (UINT32 frame = 0; frame < packetFrames; ++frame) {
                float sample = 0.0f;
                const size_t index = static_cast<size_t>(frame) * channels;
                if (isFloat && format->wBitsPerSample == 32) {
                    std::memcpy(&sample, bytes + index * sizeof(float), sizeof(float));
                } else if (format->wBitsPerSample == 16) {
                    int16_t value = 0;
                    std::memcpy(&value, bytes + index * sizeof(int16_t), sizeof(value));
                    sample = static_cast<float>(value) / 32768.0f;
                } else if (format->wBitsPerSample == 32) {
                    int32_t value = 0;
                    std::memcpy(&value, bytes + index * sizeof(int32_t), sizeof(value));
                    sample = static_cast<float>(value / 2147483648.0);
                }
                peak = (std::max)(peak, std::fabs(sample));
            }
        }
        capturedFrames += packetFrames;
        capture->ReleaseBuffer(packetFrames);
    }

    svms::DriverDebugInfo debugInfo;
    const DWORD debugBytes = debug(&debugInfo, sizeof(debugInfo));
    svms::SnappyVoiceStatistics compatibilityStats;
    const float compatibilityRenderMilliseconds = renderingTime();
    const bool compatibilityValid =
        voiceStats(&compatibilityStats) == &compatibilityStats &&
        voiceCount() == compatibilityStats.activeVoices &&
        compatibilityStats.activeVoices + compatibilityStats.freeVoices > 0u &&
        compatibilityRenderMilliseconds > 0.0f && legacyDebug() != nullptr;
    for (uint32_t i = 0; i < repeatCount; ++i) shortMessage(midi, noteOff);
    close(midi);
    midi = nullptr;
    std::printf("repeat=%u note=%u captured_frames=%llu peak=%.9f\n",
                repeatCount, midiNote,
                static_cast<unsigned long long>(capturedFrames), peak);
    if (debugBytes == sizeof(debugInfo) &&
        debugInfo.magic == svms::DriverDebugInfo::kMagic) {
        std::printf("callbacks=%llu submitted=%llu accepted=%llu dispatched=%llu "
                    "note_ons=%llu matches=%llu configured=%llu active=%u "
                    "sf_loaded=%u sample_frames=%u samples=%u running=%u "
                    "hr=0x%08X render_peak=%.9f\n",
                    static_cast<unsigned long long>(debugInfo.callbackCount),
                    static_cast<unsigned long long>(debugInfo.submitted),
                    static_cast<unsigned long long>(debugInfo.accepted),
                    static_cast<unsigned long long>(debugInfo.dispatched),
                    static_cast<unsigned long long>(debugInfo.noteOns),
                    static_cast<unsigned long long>(debugInfo.matchedRegions),
                    static_cast<unsigned long long>(debugInfo.configuredVoices),
                    debugInfo.activeVoices, debugInfo.soundFontLoaded,
                    debugInfo.sampleDataFrames, debugInfo.sampleCount,
                    debugInfo.audioRunning, static_cast<unsigned>(debugInfo.audioHResult),
                    debugInfo.renderPeak);
    }
    std::printf("ziggy_active=%u free=%u steals=%u render_ms=%.4f valid=%u\n",
                compatibilityStats.activeVoices, compatibilityStats.freeVoices,
                compatibilityStats.voiceSteals, compatibilityRenderMilliseconds,
                compatibilityValid ? 1u : 0u);
    cleanup();
    // repeat-count 0 is a silent endpoint/initialization probe.
    return (repeatCount == 0u || peak > 1.0e-5f) && compatibilityValid ? 0 : 1;
}
