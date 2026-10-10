// External synth backend router (api.backend 1-4).

#include "SVMSDriverInternal.h"

namespace svms {

// ── External synth backend (SVMS-API / KDMAPI / WinMM) ───────────────
// api.backend selects where admitted MIDI events go: the in-process SVMS
// engine (0) or a loaded sink (1 SVMS-API DLL, 2 KDMAPI DLL, 3 WinMM
// device). A sink owns its own audio output; the render callback forwards
// events and outputs silence. The load happens once at driver init — a
// failure logs and falls back to the engine, never to silence.

bool Driver::InitializeExternalBackend() {
    // Nested-chain guard: an SVMS plugin loaded inside another synth host (e.g.
    // OmniMIDIv2) sets SVMS_NESTED=1 before starting its inner engine. The inner
    // instance shares our config file, so an external backend here would route
    // events straight back out and loop. Fall back to the in-process engine
    // unless api.allow_nested is set.
    if (engineConfig_.apiBackend != 0u && !engineConfig_.apiAllowNested) {
        wchar_t nested[2] = {};
        if (GetEnvironmentVariableW(L"SVMS_NESTED", nested, 2) && nested[0] == L'1') {
            LOG("api.backend ignored: nested SVMS instance (set api.allow_nested to override)");
            engineConfig_.apiBackend = 0u;
        }
    }
    const uint32_t kind = engineConfig_.apiBackend;
    if (kind == 0u) return true;
    // Kind 4 auto-detects whatever the configured DLL answers with: the
    // SVMS-API table getter, the KDMAPI export set, or WinMM midiOut
    // exports — in that priority order. Kinds 1..3 demand a specific
    // surface; auto is how "throw any synth DLL at it" works.
    if (kind == 4u) {
        if (engineConfig_.apiBackendDll.empty()) {
            LOG("Backend 4 (auto) requested but api.backend_dll is empty");
            return false;
        }
        const HMODULE module =
            LoadLibraryW(engineConfig_.apiBackendDll.c_str());
        if (!module) {
            LOG("FAILED: LoadLibrary(backend dll) error=%lu", GetLastError());
            return false;
        }
        if (OpenSvmsApiBackend(module)) {
            externalBackendKind_.store(1u, std::memory_order_release);
            LOG("External backend auto-detect: SVMS-API dll");
            return true;
        }
        if (OpenKdmapiBackend(module)) {
            externalBackendKind_.store(2u, std::memory_order_release);
            LOG("External backend auto-detect: KDMAPI dll");
            return true;
        }
        if (OpenWinmmBackend(module)) {
            externalBackendKind_.store(3u, std::memory_order_release);
            LOG("External backend auto-detect: WinMM dll");
            return true;
        }
        FreeLibrary(module);
        LOG("FAILED: backend dll answers with no known synth surface");
        return false;
    }
    if (kind == 1u) {
        if (engineConfig_.apiBackendDll.empty()) {
            LOG("Backend 1 (SVMS-API) requested but api.backend_dll is empty");
            return false;
        }
        const HMODULE module =
            LoadLibraryW(engineConfig_.apiBackendDll.c_str());
        if (!module) {
            LOG("FAILED: LoadLibrary(backend dll) error=%lu", GetLastError());
            return false;
        }
        if (OpenSvmsApiBackend(module)) {
            externalBackendKind_.store(1u, std::memory_order_release);
            LOG("External backend: SVMS-API dll active");
            return true;
        }
        FreeLibrary(module);
        return false;
    }
    if (kind == 2u) {
        if (engineConfig_.apiBackendDll.empty()) {
            LOG("Backend 2 (KDMAPI) requested but api.backend_dll is empty");
            return false;
        }
        const HMODULE module =
            LoadLibraryW(engineConfig_.apiBackendDll.c_str());
        if (!module) {
            LOG("FAILED: LoadLibrary(kdmapi dll) error=%lu", GetLastError());
            return false;
        }
        if (OpenKdmapiBackend(module)) {
            externalBackendKind_.store(2u, std::memory_order_release);
            LOG("External backend: KDMAPI dll active");
            return true;
        }
        FreeLibrary(module);
        return false;
    }
    if (kind == 3u) {
        // WinMM MIDI-out device: resolve through the genuine system winmm
        // (absolute path) — our own shim exports these names when built as
        // winmm.dll, so the loader must not hand this proxy back to us.
        if (OpenWinmmBackend(nullptr)) {
            externalBackendKind_.store(3u, std::memory_order_release);
            LOG("External backend: WinMM device %u active",
                engineConfig_.apiWinMmDevice);
            return true;
        }
        return false;
    }
    return false;
}

// Shared openers used by the explicit kinds and by auto-detect. Each takes
// ownership of the module only on success — on failure the caller frees it.

bool Driver::OpenSvmsApiBackend(HMODULE module) {
    const auto getInterface =
        reinterpret_cast<SVMSBackendGetInterfaceFn>(GetProcAddress(
            module, SVMSBACKEND_GETINTERFACE_NAME));
    if (!getInterface) return false;
    SVMSBackendInterface table{};
    table.struct_size = sizeof(table);
    if (getInterface(&table) != 0u ||
        table.struct_size < sizeof(table) ||
        table.api_version != SVMSBACKEND_API_VERSION ||
        !table.initialize || !table.shutdown || !table.reset ||
        !table.send_short) {
        LOG("FAILED: backend interface invalid");
        return false;
    }
    SVMSBackendOpenParams params{};
    params.struct_size = sizeof(params);
    params.sample_rate = sampleRate;
    params.buffer_frames = bufferFrames;
    params.voices_hint = engineConfig_.maxVoices;
    if (table.initialize(table.user, &params) != 0) {
        LOG("FAILED: backend initialize refused");
        return false;
    }
    externalBackend_ = table;
    externalBackendModule_ = module;
    externalBackendOpen_ = true;
    return true;
}

bool Driver::OpenKdmapiBackend(HMODULE module) {
    using KdInitFn = int (WINAPI*)(void);
    using KdTermFn = int (WINAPI*)(void);
    using KdResetFn = void (WINAPI*)(void);
    using KdSendFn = unsigned int (WINAPI*)(unsigned int);
    kdmapiBackend_.initialize = reinterpret_cast<KdInitFn>(
        GetProcAddress(module, "InitializeKDMAPIStream"));
    kdmapiBackend_.terminate = reinterpret_cast<KdTermFn>(
        GetProcAddress(module, "TerminateKDMAPIStream"));
    kdmapiBackend_.reset = reinterpret_cast<KdResetFn>(
        GetProcAddress(module, "ResetKDMAPIStream"));
    kdmapiBackend_.sendNoBuf = reinterpret_cast<KdSendFn>(
        GetProcAddress(module, "SendDirectDataNoBuf"));
    kdmapiBackend_.send = reinterpret_cast<KdSendFn>(
        GetProcAddress(module, "SendDirectData"));
    if (!kdmapiBackend_.initialize || !kdmapiBackend_.terminate ||
        !kdmapiBackend_.reset ||
        (!kdmapiBackend_.sendNoBuf && !kdmapiBackend_.send)) {
        return false;
    }
    if (kdmapiBackend_.initialize() == 0) {
        LOG("FAILED: kdmapi InitializeKDMAPIStream refused");
        return false;
    }
    kdmapiBackendModule_ = module;
    kdmapiBackendOpen_ = true;
    return true;
}

bool Driver::OpenWinmmBackend(HMODULE module) {
    using WmNumProc = UINT(WINAPI*)(void);
    using WmOpenProc = MMRESULT(WINAPI*)(LPHMIDIOUT, UINT, DWORD_PTR,
                                         DWORD_PTR, DWORD);
    using WmShortProc = MMRESULT(WINAPI*)(HMIDIOUT, DWORD);
    using WmResetProc = MMRESULT(WINAPI*)(HMIDIOUT);
    WmNumProc getNum = nullptr;
    WmOpenProc open = nullptr;
    WmShortProc shortMsg = nullptr;
    if (module) {
        // A WinMM-replacement DLL the caller pointed us at: use its exports.
        getNum = reinterpret_cast<WmNumProc>(
            GetProcAddress(module, "midiOutGetNumDevs"));
        open = reinterpret_cast<WmOpenProc>(
            GetProcAddress(module, "midiOutOpen"));
        shortMsg = reinterpret_cast<WmShortProc>(
            GetProcAddress(module, "midiOutShortMsg"));
    } else {
        // The genuine system winmm by absolute path — never our own shim.
        getNum = reinterpret_cast<WmNumProc>(
            GetSystemWinmmProc("midiOutGetNumDevs"));
        open = reinterpret_cast<WmOpenProc>(
            GetSystemWinmmProc("midiOutOpen"));
        shortMsg = reinterpret_cast<WmShortProc>(
            GetSystemWinmmProc("midiOutShortMsg"));
    }
    if (!getNum || !open || !shortMsg) {
        LOG("FAILED: winmm surface lacks midiOut exports");
        return false;
    }
    const UINT devices = getNum();
    if (engineConfig_.apiWinMmDevice >= devices) {
        LOG("FAILED: api.winmm_device %u out of range (%u devices)",
            engineConfig_.apiWinMmDevice, devices);
        return false;
    }
    HMIDIOUT out = nullptr;
    if (open(&out, engineConfig_.apiWinMmDevice, 0, 0,
             CALLBACK_NULL) != MMSYSERR_NOERROR) {
        LOG("FAILED: midiOutOpen device %u", engineConfig_.apiWinMmDevice);
        return false;
    }
    winmmBackendOut_ = out;
    winmmBackendModule_ = module;
    systemWinmmShortMsg_ = shortMsg;
    systemWinmmResetProc_ = reinterpret_cast<WmResetProc>(
        module ? static_cast<void*>(GetProcAddress(module, "midiOutReset"))
               : static_cast<void*>(GetSystemWinmmProc("midiOutReset")));
    return true;
}

void Driver::ShutdownExternalBackend() {
    const uint32_t kind =
        externalBackendKind_.exchange(0u, std::memory_order_acq_rel);
    if (kind == 1u && externalBackendOpen_) {
        externalBackend_.shutdown(externalBackend_.user);
        externalBackendOpen_ = false;
    } else if (kind == 2u && kdmapiBackendOpen_) {
        kdmapiBackend_.terminate();
        kdmapiBackendOpen_ = false;
    } else if (kind == 3u && winmmBackendOut_) {
        using WmResetProc = MMRESULT(WINAPI*)(HMIDIOUT);
        using WmCloseProc = MMRESULT(WINAPI*)(HMIDIOUT);
        if (const auto reset = reinterpret_cast<WmResetProc>(
                GetSystemWinmmProc("midiOutReset")))
            reset(winmmBackendOut_);
        if (const auto close = reinterpret_cast<WmCloseProc>(
                GetSystemWinmmProc("midiOutClose")))
            close(winmmBackendOut_);
        winmmBackendOut_ = nullptr;
        systemWinmmShortMsg_ = nullptr;
        if (winmmBackendModule_) {
            FreeLibrary(winmmBackendModule_);
            winmmBackendModule_ = nullptr;
        }
    }
    if (kind == 1u && externalBackendModule_) {
        FreeLibrary(externalBackendModule_);
        externalBackendModule_ = nullptr;
    }
    if (kind == 2u && kdmapiBackendModule_) {
        FreeLibrary(kdmapiBackendModule_);
        kdmapiBackendModule_ = nullptr;
    }
}

void Driver::ExternalBackendReset() {
    const uint32_t kind = externalBackendKind_.load(std::memory_order_relaxed);
    if (kind == 1u && externalBackendOpen_) {
        externalBackend_.reset(externalBackend_.user);
    } else if (kind == 2u && kdmapiBackendOpen_) {
        kdmapiBackend_.reset();
    } else if (kind == 3u && winmmBackendOut_ && systemWinmmResetProc_) {
        systemWinmmResetProc_(winmmBackendOut_);
    }
}

// Translate one compiled render event back to its packed short MIDI message.
// Internal engine-level events (master volume/tune, rhythm part) have no
// per-message mapping in backend ABI v1 and are dropped; the Reset event
// becomes the backend's hard reset.
static bool TranslateRenderEventToShortMsg(const RenderEvent& ev,
                                           uint32_t& out) {
    const uint32_t ch = ev.channel & 0x0fu;
    switch (ev.type) {
        case RenderEventType::NoteOn:
            out = 0x90u | ch | (static_cast<uint32_t>(ev.data1) << 8u) |
                  (static_cast<uint32_t>(ev.data2) << 16u);
            return ev.data2 != 0u;
        case RenderEventType::NoteOff:
        case RenderEventType::StaleNoteOffBatch:
            out = 0x80u | ch | (static_cast<uint32_t>(ev.data1) << 8u);
            return true;
        case RenderEventType::ControlChange:
            out = 0xB0u | ch | (static_cast<uint32_t>(ev.data1) << 8u) |
                  (static_cast<uint32_t>(ev.data2) << 16u);
            return true;
        case RenderEventType::ProgramChange:
            out = 0xC0u | ch | (static_cast<uint32_t>(ev.data1) << 8u);
            return true;
        case RenderEventType::PitchBend:
            out = 0xE0u | ch | (static_cast<uint32_t>(ev.data1) << 8u) |
                  (static_cast<uint32_t>(ev.data2) << 16u);
            return true;
        case RenderEventType::ChannelPressure:
            out = 0xD0u | ch | (static_cast<uint32_t>(ev.data1) << 8u);
            return true;
        case RenderEventType::AllNotesOff:
            out = 0xB0u | ch | (123u << 8u);
            return true;
        case RenderEventType::AllSoundOff:
            out = 0xB0u | ch | (120u << 8u);
            return true;
        case RenderEventType::Reset:
            out = 0u;
            return false;  // handled by the caller as a backend reset
        default:
            return false;  // MasterVolume/RhythmPart/FineTune/Transpose: v2
    }
}

void Driver::ForwardBlockToBackend(const RenderEvent* events,
                                   uint32_t count) {
    const uint32_t kind = externalBackendKind_.load(std::memory_order_relaxed);
    if (kind == 0u) return;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t msg = 0u;
        if (!TranslateRenderEventToShortMsg(events[i], msg)) {
            if (events[i].type == RenderEventType::Reset)
                ExternalBackendReset();
            continue;
        }
        if (kind == 1u) {
            externalBackend_.send_short(externalBackend_.user, msg);
        } else if (kind == 2u) {
            if (kdmapiBackend_.sendNoBuf)
                kdmapiBackend_.sendNoBuf(msg);
            else if (kdmapiBackend_.send)
                kdmapiBackend_.send(msg);
        } else if (kind == 3u && winmmBackendOut_ && systemWinmmShortMsg_) {
            systemWinmmShortMsg_(winmmBackendOut_, msg);
        }
    }
}

} // namespace svms
