// SVMSPlayer.cpp — console MIDI player over the native SVMS API.
//
// A dogfood client for the standalone SVMSAPI.dll: it integrates exactly the
// way an external player would (LoadLibrary + GetProcAddress of the single
// SVMS_GetInterface bootstrap) and exercises the API's differentiating paths:
//   - output-frame scheduling (send_timed_short_batch, OUTPUT_FRAME domain)
//   - lossless ingress        (bounded backpressure, never drops events)
//   - telemetry polling       (voices / render time ASCII graphs)
//   - live config patching    (+/- master volume via patch_config_json)
//
// Architecture (three threads, one SPSC ring, bounded memory):
//   decoder thread  MappedMidiFile + MidiStreamDecoder -> PackedMidiEvent
//                   stream pushed into the ring; events before the start
//                   frame are folded into per-channel replay state instead
//                   (program/bank/CC/pitch), so seeks replay correctly.
//   submit thread   ring -> send_timed_short_batch with OUTPUT_FRAME stamps
//                   anchored to get_output_clock; bounded lookahead.
//   main thread     console UI, keyboard, telemetry sampling.
//
// Memory: the file is mapped (zero copy), the ring is a fixed budget
// (default 64 MB) and the decoder blocks on Push when full — any file size
// plays with constant memory. Pause drains the lookahead before silencing
// (zero music loss); seek/stop fence the engine via reset() (instant).

#include <windows.h>
#include <mmsystem.h>

#include <dbghelp.h>

#include "SVMSMidiStream.h"
#include "include/svmsapi.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kDefaultApiName[] = L"SVMSAPI.dll";
constexpr uint32_t kSubmitBatchMax = 2048u;

enum PlayerState : uint32_t {
    kStateIdle = 0,     // stopped at 0
    kStatePlaying = 1,
    kStatePausing = 2,  // draining the lookahead before silencing
    kStatePaused = 3,
    kStateSeeking = 4,
};

// UTF-8 block glyphs for the sparklines (U+2581..U+2588, lowest to highest).
const char* const kSparkBands[8] = {
    "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
};

// ── Crash diagnostics: player-side VEH — minidump + symbolized stack ───
// The engine DLL has a reporter, but heap-corruption fast-fails bypass it
// and some AV paths were not logged; the player owns its own evidence.

// Proper x64 unwind via RtlVirtualUnwind — dbghelp's StackWalk64 stops
// early when DIA can't load the newest toolset's PDB format. Frames are
// printed as module+RVA and resolved offline via the map file.
struct UnwindFrame {
    uintptr_t pc;
    uintptr_t rsp;
};

unsigned WalkX64(uintptr_t rip, uintptr_t rsp, uintptr_t rbp,
                 UnwindFrame* out, unsigned maxFrames) {
    unsigned count = 0;
    uintptr_t currentRip = rip;
    uintptr_t currentRsp = rsp;
    uintptr_t currentRbp = rbp;
    out[count].pc = currentRip;
    out[count].rsp = currentRsp;
    ++count;
    for (unsigned depth = 0; depth + 1 < maxFrames; ++depth) {
        uint64_t imageBase = 0;
        RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(
            currentRip, &imageBase, nullptr);
        fprintf(stderr, "    [walk %u] rip=%p fn=%p base=%llx\n", depth,
                reinterpret_cast<void*>(currentRip),
                reinterpret_cast<void*>(fn),
                static_cast<unsigned long long>(imageBase));
        if (!fn) {
            // Leaf frame: return address sits at [rsp].
            void* ret = *reinterpret_cast<void**>(currentRsp);
            if (!ret) break;
            currentRip = reinterpret_cast<uintptr_t>(ret);
            currentRsp += sizeof(void*);
        } else {
            CONTEXT ctx{};
            ctx.Rip = currentRip;
            ctx.Rsp = currentRsp;
            ctx.Rbp = currentRbp;
            void* handlerData = nullptr;
            uint64_t establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, currentRip, fn,
                             &ctx, &handlerData, &establisherFrame, nullptr);
            currentRip = ctx.Rip;
            currentRsp = ctx.Rsp;
            currentRbp = ctx.Rbp;
        }
        if (!currentRip || currentRip < 0x10000u) break;
        out[count].pc = currentRip;
        out[count].rsp = currentRsp;
        ++count;
    }
    return count;
}

LONG WINAPI PlayerCrashFilter(EXCEPTION_POINTERS* ep) {
    // Arm on REAL fatal faults only — OutputDebugString raises benign
    // first-chance exceptions (0x40010006) that must not consume the arm.
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatal =
        code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_PRIV_INSTRUCTION ||
        code == 0xC0000374u /* heap corruption */ ||
        code == 0xC0000409u /* fastfail */;
    if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
    static LONG armed = 0;
    if (InterlockedExchange(&armed, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    __try {
        // 1) Minidump next to the exe.
        wchar_t dumpPath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, dumpPath, MAX_PATH) > 0) {
            wcscat_s(dumpPath, L".crash.dmp");
            HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{
                    GetCurrentThreadId(), ep, 0};
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                  file, MiniDumpNormal, &mei, nullptr,
                                  nullptr);
                CloseHandle(file);
            }
        }
        // 2) Unwound stack to stderr (module+RVA; resolve via map file).
        CONTEXT ctx = *ep->ContextRecord;
        UnwindFrame frames[40] = {};
        const unsigned frameCount = WalkX64(
            static_cast<uintptr_t>(ctx.Rip),
            static_cast<uintptr_t>(ctx.Rsp),
            static_cast<uintptr_t>(ctx.Rbp), frames, 40u);
        fprintf(stderr, "\n==== player crash 0x%08lX at %p tid=%lu ====\n",
                ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress,
                GetCurrentThreadId());
        for (unsigned i = 0; i < frameCount; ++i) {
            const uintptr_t pc = frames[i].pc;
            HMODULE mod = nullptr;
            char modName[MAX_PATH] = "?";
            uintptr_t rva = 0;
            if (pc > 0x10000u &&
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(pc), &mod) && mod) {
                rva = pc - reinterpret_cast<uintptr_t>(mod);
                GetModuleFileNameA(mod, modName, MAX_PATH);
                for (char* s = modName; *s; ++s)
                    if (*s == '\\' || *s == '/') {
                        memmove(modName, s + 1, strlen(s + 1) + 1);
                        break;
                    }
            }
            fprintf(stderr, "  %2u  %s+0x%llX\n", i, modName,
                    static_cast<unsigned long long>(rva));
        }
        // Stack scan backup: raw stack slots that point into loaded
        // modules (catches frames the unwind metadata misses).
        fprintf(stderr, "  ---- stack scan ----\n");
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t sp = static_cast<uintptr_t>(
            ep->ContextRecord->Rsp);
        uint32_t printed = 0;
        for (uintptr_t p = sp; p < sp + 0x10000u && printed < 48u;
             p += sizeof(void*)) {
            if (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi,
                             sizeof(mbi)) != sizeof(mbi))
                break;
            if (mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                break;
            const uintptr_t value = *reinterpret_cast<const uintptr_t*>(p);
            HMODULE mod = nullptr;
            if (value > 0x10000u &&
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(value), &mod) && mod) {
                char modName[MAX_PATH] = "?";
                GetModuleFileNameA(mod, modName, MAX_PATH);
                for (char* s = modName; *s; ++s)
                    if (*s == '\\' || *s == '/') {
                        memmove(modName, s + 1, strlen(s + 1) + 1);
                        break;
                    }
                const uintptr_t rva = value - reinterpret_cast<uintptr_t>(mod);
                char symBuf[sizeof(SYMBOL_INFO) + 192] = {};
                SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 191;
                DWORD64 disp = 0;
                if (!SymFromAddr(GetCurrentProcess(), value, &disp, sym))
                    sym->Name[0] = '\0';
                fprintf(stderr, "  [rsp+%#llx] %s+0x%llX  %s+%llu\n",
                        static_cast<unsigned long long>(p - sp), modName,
                        static_cast<unsigned long long>(rva), sym->Name,
                        static_cast<unsigned long long>(disp));
                ++printed;
            }
        }
        fflush(stderr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── API binding ────────────────────────────────────────────────────────

using GetInterfaceProc = SVMS_Result (SVMS_CALL*)(
    uint32_t requested_abi, uint32_t caller_table_size,
    SVMS_Interface* out_interface);

struct Api {
    HMODULE module = nullptr;
    SVMS_Interface fn{};

    bool Load(const std::wstring& explicitPath, std::string& error) {
        // Prefer an explicitly requested DLL, then the SVMSAPI.dll next to
        // the player, then the normal search path. Never link-time import:
        // this is the player contract.
        if (!explicitPath.empty()) {
            module = LoadLibraryW(explicitPath.c_str());
            if (!module) {
                error = "could not load the requested SVMSAPI.dll path";
                return false;
            }
        } else {
            wchar_t self[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring local(self);
            const size_t slash = local.find_last_of(L"\\/");
            if (slash != std::wstring::npos) local.resize(slash + 1);
            local += kDefaultApiName;
            module = LoadLibraryW(local.c_str());
            if (!module) module = LoadLibraryW(kDefaultApiName);
            if (!module) {
                error = "could not load SVMSAPI.dll (place it next to "
                        "svms_player.exe or in PATH)";
                return false;
            }
        }
        const auto getInterface = reinterpret_cast<GetInterfaceProc>(
            GetProcAddress(module, "SVMS_GetInterface"));
        if (!getInterface) {
            error = "SVMSAPI.dll has no SVMS_GetInterface export";
            return false;
        }
        fn.struct_size = sizeof(fn);
        if (getInterface(SVMS_ABI_VERSION_1, sizeof(fn), &fn) !=
                SVMS_RESULT_OK ||
            fn.abi_version != SVMS_ABI_VERSION_1) {
            error = "SVMSAPI.dll does not support ABI 1";
            return false;
        }
        return true;
    }

    void Unload() {
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
    }
};

// ── Legacy backends: KDMAPI and WinMM synths ───────────────────────────
// The player is API-agnostic at the process level: these sinks are resolved
// with GetProcAddress exactly the way an external client would, so the same
// player binary can drive SVMSAPI.dll, an SVMS-built OmniMIDI.dll/winmm.dll
// facade, or a third-party synth exposing those surfaces. KDMAPI and WinMM
// are immediate-submission APIs: the player owns the clock (QPC-paced
// scheduling) instead of the engine's output-frame clock.

enum Backend { kBackendSvms = 0, kBackendKdapi = 1, kBackendWinmm = 2 };

struct KdapiProcs {
    BOOL(__cdecl* available)() = nullptr;
    BOOL(__cdecl* initStream)() = nullptr;
    BOOL(__cdecl* termStream)() = nullptr;
    VOID(__cdecl* resetStream)() = nullptr;
    MMRESULT(__cdecl* sendDirect)(UINT) = nullptr;
    DWORD(__cdecl* voiceCount)() = nullptr;
};

struct WinmmProcs {
    UINT(WINAPI* numDevs)() = nullptr;
    MMRESULT(WINAPI* outOpen)(HMIDIOUT*, UINT, DWORD_PTR, DWORD_PTR,
                              DWORD) = nullptr;
    MMRESULT(WINAPI* outClose)(HMIDIOUT) = nullptr;
    MMRESULT(WINAPI* outShortMsg)(HMIDIOUT, DWORD) = nullptr;
    MMRESULT(WINAPI* outReset)(HMIDIOUT) = nullptr;
};

struct LegacySink {
    enum Kind { kKindKdapi, kKindWinmm };
    Kind kind = kKindKdapi;
    HMODULE module = nullptr;
    HMODULE timeModule = nullptr;   // winmm (system or shim) for timers
    KdapiProcs kd{};
    WinmmProcs mm{};
    HMIDIOUT out = nullptr;
    bool started = false;
    wchar_t moduleName[MAX_PATH] = {};

    bool Load(Kind kind_, const std::wstring& explicitPath,
              std::string& error) {
        kind = kind_;
        const wchar_t* defaultName =
            kind == kKindKdapi ? L"OmniMIDI.dll" : L"winmm.dll";
        if (!explicitPath.empty()) {
            module = LoadLibraryW(explicitPath.c_str());
        } else {
            // Application directory first: a synth winmm.dll/OmniMIDI.dll
            // placed next to the player shadows the system one — the exact
            // drop-in scenario being tested.
            wchar_t self[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring local(self);
            const size_t slash = local.find_last_of(L"\\/");
            if (slash != std::wstring::npos) local.resize(slash + 1);
            local += defaultName;
            module = LoadLibraryW(local.c_str());
            if (!module) module = LoadLibraryW(defaultName);
        }
        if (!module) {
            error = "could not load the synth module";
            return false;
        }
        GetModuleFileNameW(module, moduleName, MAX_PATH);
        if (kind == kKindKdapi) {
            kd.available = reinterpret_cast<BOOL(__cdecl*)()>(
                GetProcAddress(module, "IsKDMAPIAvailable"));
            kd.initStream = reinterpret_cast<BOOL(__cdecl*)()>(
                GetProcAddress(module, "InitializeKDMAPIStream"));
            kd.termStream = reinterpret_cast<BOOL(__cdecl*)()>(
                GetProcAddress(module, "TerminateKDMAPIStream"));
            kd.resetStream = reinterpret_cast<VOID(__cdecl*)()>(
                GetProcAddress(module, "ResetKDMAPIStream"));
            kd.sendDirect = reinterpret_cast<MMRESULT(__cdecl*)(UINT)>(
                GetProcAddress(module, "SendDirectData"));
            kd.voiceCount = reinterpret_cast<DWORD(__cdecl*)()>(
                GetProcAddress(module, "GetVoiceCount"));
            if (!kd.available || !kd.initStream || !kd.termStream ||
                !kd.sendDirect) {
                error = "module has no usable KDMAPI exports";
                return false;
            }
        } else {
            mm.numDevs = reinterpret_cast<UINT(WINAPI*)()>(
                GetProcAddress(module, "midiOutGetNumDevs"));
            mm.outOpen = reinterpret_cast<MMRESULT(WINAPI*)(
                HMIDIOUT*, UINT, DWORD_PTR, DWORD_PTR, DWORD)>(
                GetProcAddress(module, "midiOutOpen"));
            mm.outClose = reinterpret_cast<MMRESULT(WINAPI*)(HMIDIOUT)>(
                GetProcAddress(module, "midiOutClose"));
            mm.outShortMsg =
                reinterpret_cast<MMRESULT(WINAPI*)(HMIDIOUT, DWORD)>(
                    GetProcAddress(module, "midiOutShortMsg"));
            mm.outReset = reinterpret_cast<MMRESULT(WINAPI*)(HMIDIOUT)>(
                GetProcAddress(module, "midiOutReset"));
            if (!mm.numDevs || !mm.outOpen || !mm.outClose ||
                !mm.outShortMsg || !mm.outReset) {
                error = "module has no midiOut* exports";
                return false;
            }
        }
        // Coarse timers ruin the pacing; ask winmm (system or the loaded
        // shim — both export it) for 1 ms scheduling granularity.
        timeModule = LoadLibraryW(L"winmm.dll");
        if (timeModule) {
            const auto begin = reinterpret_cast<MMRESULT(WINAPI*)(UINT)>(
                GetProcAddress(timeModule, "timeBeginPeriod"));
            if (begin) begin(1u);
        }
        return true;
    }

    bool Start(std::string& error) {
        if (kind == kKindKdapi) {
            if (!kd.available()) {
                error = "the KDMAPI synth reports itself unavailable";
                return false;
            }
            if (!kd.initStream()) {
                error = "InitializeKDMAPIStream failed";
                return false;
            }
        } else {
            if (mm.numDevs() == 0u) {
                error = "the synth exposes no MIDI out devices";
                return false;
            }
            // MIDI_MAPPER: route through the synth's own device mapping.
            if (mm.outOpen(&out, MIDI_MAPPER, 0, 0, CALLBACK_NULL) !=
                MMSYSERR_NOERROR) {
                error = "midiOutOpen failed";
                return false;
            }
        }
        started = true;
        return true;
    }

    void Send(uint32_t message) {
        if (!started) return;
        if (kind == kKindKdapi) kd.sendDirect(message);
        else mm.outShortMsg(out, message);
    }

    void SendVolume(float volume) {
        int value = static_cast<int>(volume * 100.0f + 0.5f);
        if (value < 0) value = 0;
        if (value > 127) value = 127;
        for (uint32_t ch = 0; ch < 16u; ++ch)
            Send(0xB0u | ch | (7u << 8) | (uint32_t(value) << 16));
    }

    void Silence() {
        // All notes off + sustain off on every channel: the gentle pause.
        for (uint32_t ch = 0; ch < 16u; ++ch) {
            Send(0xB0u | ch | (123u << 8));
            Send(0xB0u | ch | (64u << 8));
        }
    }

    void FullReset() {
        if (kind == kKindKdapi) {
            if (kd.resetStream) kd.resetStream();
            else Silence();
        } else {
            mm.outReset(out);
        }
    }

    uint32_t VoiceCount() const {
        return (kind == kKindKdapi && kd.voiceCount) ? kd.voiceCount() : 0u;
    }

    void Stop() {
        if (!started) return;
        if (kind == kKindKdapi) kd.termStream();
        else {
            mm.outReset(out);
            mm.outClose(out);
            out = nullptr;
        }
        started = false;
        if (timeModule) {
            const auto end = reinterpret_cast<MMRESULT(WINAPI*)(UINT)>(
                GetProcAddress(timeModule, "timeEndPeriod"));
            if (end) end(1u);
        }
    }

    void Unload() {
        Stop();
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
        if (timeModule) {
            FreeLibrary(timeModule);
            timeModule = nullptr;
        }
    }
};

// ── Shared player core ─────────────────────────────────────────────────

struct ChannelReplay {
    bool programSeen = false;
    uint8_t program = 0;
    uint8_t cc[128] = {};
    bool ccSeen[128] = {};
    bool pitchSeen = false;
    uint8_t pitchLsb = 0;
    uint8_t pitchMsb = 0;
    bool pressureSeen = false;
    uint8_t pressure = 0;
};

struct PlayerCore {
    Api api;
    Backend backend = kBackendSvms;
    LegacySink legacy;
    // Legacy (kdapi/winmm) wall-clock scheduling anchor: file frame
    // `legacyAnchorFrame` maps to QPC tick `legacyAnchorQpc`.
    std::atomic<int64_t> legacyAnchorQpc{0};
    std::atomic<int64_t> legacyAnchorFrame{0};
    SVMS_Session session = 0;
    svms::ParsedEventRing* ring = nullptr;  // owned by main

    std::wstring filePath;
    uint32_t sampleRate = 48000;
    uint64_t totalFrames = 0;
    uint64_t lookaheadFrames = 0;
    uint64_t delayFrames = 0;

    std::atomic<uint32_t> state{kStateIdle};
    std::atomic<bool> quit{false};
    std::atomic<bool> reanchor{false};
    std::atomic<bool> endOfSong{false};
    std::atomic<uint64_t> cursorFrame{0};   // resume point (file frames)
    std::atomic<int64_t> playhead{0};       // file-space playhead (submit)
    std::atomic<uint64_t> anchorFrame{0};
    std::atomic<uint64_t> submittedEvents{0};
    std::atomic<bool> decoderFailed{false};
    std::string decoderError;
    std::string soundfontUtf8;   // set when --sf2 was given (stress reloads)
    bool stressConfig = false;   // --stress-config: hammer live settings

    // Replay state accumulates across seeks (documented approximation).
    ChannelReplay channels[16];

    // Highest file frame handed to the engine (submit thread; main during
    // seeks while the submit thread is parked in a non-playing state).
    std::atomic<uint64_t> lastSubmittedFrame{0};

    void ObserveChannelState(const svms::PackedMidiEvent& event) {
        const uint32_t msg = event.message;
        ChannelReplay& channel = channels[msg & 0x0fu];
        switch (msg & 0xf0u) {
        case 0xC0u:
            channel.programSeen = true;
            channel.program = static_cast<uint8_t>((msg >> 8) & 0x7fu);
            break;
        case 0xB0u: {
            const uint32_t controller = (msg >> 8) & 0x7fu;
            channel.cc[controller] =
                static_cast<uint8_t>((msg >> 16) & 0x7fu);
            channel.ccSeen[controller] = true;
            break;
        }
        case 0xE0u:
            channel.pitchSeen = true;
            channel.pitchLsb = static_cast<uint8_t>((msg >> 8) & 0x7fu);
            channel.pitchMsb = static_cast<uint8_t>((msg >> 16) & 0x7fu);
            break;
        case 0xD0u:
            channel.pressureSeen = true;
            channel.pressure = static_cast<uint8_t>((msg >> 8) & 0x7fu);
            break;
        default:
            break;
        }
    }

    // Push the collected channel state as messages stamped at startFrame so
    // a seek lands with the right instruments, volumes and controllers.
    void EmitReplay(uint64_t startFrame, const std::atomic<bool>& cancel) {
        svms::PackedMidiEvent event{};
        event.outputFrame = startFrame;
        for (uint32_t ch = 0; ch < 16; ++ch) {
            ChannelReplay& channel = channels[ch];
            const uint32_t base = ch;
            if (channel.ccSeen[0]) {
                event.message = 0xB0u | base | (0u << 8) |
                                (uint32_t(channel.cc[0]) << 16);
                if (!ring->Push(event, cancel)) return;
            }
            if (channel.ccSeen[32]) {
                event.message = 0xB0u | base | (32u << 8) |
                                (uint32_t(channel.cc[32]) << 16);
                if (!ring->Push(event, cancel)) return;
            }
            if (channel.programSeen) {
                event.message =
                    0xC0u | base | (uint32_t(channel.program) << 8);
                if (!ring->Push(event, cancel)) return;
            }
            for (uint32_t controller = 1; controller < 128; ++controller) {
                if (controller == 32u || !channel.ccSeen[controller]) continue;
                event.message = 0xB0u | base | (controller << 8) |
                                (uint32_t(channel.cc[controller]) << 16);
                if (!ring->Push(event, cancel)) return;
            }
            if (channel.pitchSeen) {
                event.message = 0xE0u | base |
                                (uint32_t(channel.pitchLsb) << 8) |
                                (uint32_t(channel.pitchMsb) << 16);
                if (!ring->Push(event, cancel)) return;
            }
            if (channel.pressureSeen) {
                event.message = 0xD0u | base |
                                (uint32_t(channel.pressure) << 8);
                if (!ring->Push(event, cancel)) return;
            }
        }
    }
};

// ── Decoder thread ─────────────────────────────────────────────────────

struct DecodeArgs {
    PlayerCore* core = nullptr;
    std::wstring path;
    uint64_t startFrame = 0;
    bool replayEmitted = false;
    std::atomic<bool> cancel{false};
};

bool DecodeSink(const svms::PackedMidiEvent& event, void* user) {
    auto* args = static_cast<DecodeArgs*>(user);
    if (args->cancel.load(std::memory_order_relaxed)) return false;
    if (event.outputFrame < args->startFrame) {
        // Fold pre-seek events into the replay state instead of the ring.
        args->core->ObserveChannelState(event);
        return true;
    }
    if (!args->replayEmitted) {
        args->replayEmitted = true;
        args->core->EmitReplay(args->startFrame, args->cancel);
    }
    return args->core->ring->Push(event, args->cancel);
}

void DecoderThread(DecodeArgs* args) {
    PlayerCore& core = *args->core;
    svms::MappedMidiFile file;
    std::string error;
    if (!file.Open(args->path.c_str(), error)) {
        core.decoderError = error;
        core.decoderFailed.store(true, std::memory_order_release);
        delete args;
        return;
    }
    svms::MidiStreamDecoder decoder;
    if (!decoder.Decode(file, core.sampleRate, DecodeSink, args,
                        &args->cancel, nullptr, error)) {
        if (!args->cancel.load(std::memory_order_relaxed)) {
            core.decoderError = error;
            core.decoderFailed.store(true, std::memory_order_release);
        }
    }
    delete args;
}

// ── Submit thread: ring -> engine, output-frame stamped ───────────────

// Legacy (KDMAPI / WinMM) submission: immediate sends paced by a QPC
// wall-clock scheduler. The file-frame domain maps linearly onto QPC:
// due(event) = anchorQpc + (frame - anchorFrame) * freq / sampleRate.
// Pausing silences the synth immediately (no engine-side lookahead to
// drain) and re-anchors on resume; held notes are lost — the SVMS path
// is the exact-timing one, these backends measure the synth, not us.
void LegacySubmitThread(PlayerCore* core) {
    LegacySink& sink = core->legacy;
    svms::PackedMidiEvent event;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double freq = static_cast<double>(frequency.QuadPart);
    const double framesToQpc = freq / static_cast<double>(core->sampleRate);
    LARGE_INTEGER now{};

    while (!core->quit.load(std::memory_order_relaxed)) {
        const uint32_t state = core->state.load(std::memory_order_relaxed);
        if (state == kStatePausing) {
            sink.Silence();
            const int64_t playhead =
                core->playhead.load(std::memory_order_relaxed);
            core->cursorFrame.store(
                playhead > 0 ? static_cast<uint64_t>(playhead) : 0u,
                std::memory_order_relaxed);
            QueryPerformanceCounter(&now);
            core->legacyAnchorQpc.store(now.QuadPart,
                                        std::memory_order_relaxed);
            core->legacyAnchorFrame.store(
                playhead - static_cast<int64_t>(core->delayFrames),
                std::memory_order_relaxed);
            core->state.store(kStatePaused, std::memory_order_release);
            continue;
        }
        if (state != kStatePlaying) {
            Sleep(2u);
            continue;
        }
        if (core->reanchor.exchange(false, std::memory_order_acq_rel)) {
            QueryPerformanceCounter(&now);
            core->legacyAnchorQpc.store(now.QuadPart,
                                        std::memory_order_relaxed);
            core->legacyAnchorFrame.store(
                static_cast<int64_t>(core->cursorFrame.load(
                    std::memory_order_relaxed)) -
                    static_cast<int64_t>(core->delayFrames),
                std::memory_order_relaxed);
        }

        const int64_t anchorQpc = core->legacyAnchorQpc.load(
            std::memory_order_relaxed);
        const int64_t anchorFrame = core->legacyAnchorFrame.load(
            std::memory_order_relaxed);
        QueryPerformanceCounter(&now);
        const int64_t playhead = anchorFrame + static_cast<int64_t>(
            static_cast<double>(now.QuadPart - anchorQpc) / framesToQpc);
        core->playhead.store(playhead, std::memory_order_relaxed);

        if (core->ring->Size() == 0u && core->totalFrames != 0u &&
            playhead >= static_cast<int64_t>(core->totalFrames)) {
            core->endOfSong.store(true, std::memory_order_release);
            Sleep(2u);
            continue;
        }
        if (!core->ring->Peek(event)) {
            Sleep(1u);
            continue;
        }

        const int64_t dueQpc = anchorQpc + static_cast<int64_t>(
            static_cast<double>(static_cast<int64_t>(event.outputFrame) -
                                anchorFrame) * framesToQpc);
        int64_t delta = dueQpc - now.QuadPart;
        if (delta > static_cast<int64_t>(frequency.QuadPart / 500u)) {
            Sleep(1u);   // more than 2 ms away
            continue;
        }
        while (delta > 0) {
            YieldProcessor();
            QueryPerformanceCounter(&now);
            delta = dueQpc - now.QuadPart;
        }
        core->ring->Pop(event);
        sink.Send(event.message);
        core->submittedEvents.fetch_add(1u, std::memory_order_relaxed);
    }
}

void SubmitThread(PlayerCore* core) {
    if (core->backend != kBackendSvms) {
        LegacySubmitThread(core);
        return;
    }
    std::vector<SVMS_TimedShortEvent> batch;
    batch.reserve(kSubmitBatchMax);
    svms::PackedMidiEvent event;

    while (!core->quit.load(std::memory_order_relaxed)) {
        const uint32_t state = core->state.load(std::memory_order_relaxed);

        if (state == kStatePausing) {
            // Let the device play out everything already submitted so no
            // music is lost, then silence the tails (CC120 also fences the
            // channel against anything still in flight).
            uint64_t deviceNext = 0;
            uint32_t rate32 = 0;
            const int64_t anchor =
                static_cast<int64_t>(
                    core->anchorFrame.load(std::memory_order_relaxed));
            if (core->api.fn.get_output_clock(core->session, &deviceNext,
                                              &rate32) == SVMS_RESULT_OK &&
                static_cast<int64_t>(deviceNext) - anchor >=
                    static_cast<int64_t>(core->lastSubmittedFrame)) {
                for (uint32_t ch = 0; ch < 16; ++ch) {
                    core->api.fn.send_short(core->session,
                                            0xB0u | ch | (120u << 8));
                }
                core->cursorFrame.store(core->lastSubmittedFrame + 1u,
                                        std::memory_order_relaxed);
                core->state.store(kStatePaused, std::memory_order_release);
            }
            Sleep(1u);
            continue;
        }
        if (state != kStatePlaying) {
            Sleep(2u);
            continue;
        }

        uint64_t deviceNext = 0;
        uint32_t rate32 = 0;
        if (core->api.fn.get_output_clock(core->session, &deviceNext,
                                          &rate32) != SVMS_RESULT_OK) {
            Sleep(5u);
            continue;
        }

        if (core->reanchor.exchange(false, std::memory_order_acq_rel)) {
            const uint64_t cursor =
                core->cursorFrame.load(std::memory_order_relaxed);
            core->anchorFrame.store(deviceNext + core->delayFrames - cursor,
                                    std::memory_order_relaxed);
        }
        const int64_t anchor = static_cast<int64_t>(
            core->anchorFrame.load(std::memory_order_relaxed));
        const int64_t playhead =
            static_cast<int64_t>(deviceNext) - anchor;
        core->playhead.store(playhead, std::memory_order_relaxed);
        const int64_t horizon = playhead +
            static_cast<int64_t>(core->lookaheadFrames);

        uint32_t count = 0;
        while (count < kSubmitBatchMax && core->ring->Peek(event) &&
               static_cast<int64_t>(event.outputFrame) <= horizon) {
            core->ring->Pop(event);
            SVMS_TimedShortEvent out{};
            out.timestamp = static_cast<uint64_t>(
                anchor + static_cast<int64_t>(event.outputFrame));
            out.timestamp_domain = SVMS_TIMESTAMP_OUTPUT_FRAME;
            out.packed_message = event.message;
            batch.push_back(out);
            ++count;
        }

        if (count == 0u) {
            if (core->ring->Size() == 0u &&
                core->totalFrames != 0u &&
                playhead >= static_cast<int64_t>(core->totalFrames)) {
                core->endOfSong.store(true, std::memory_order_release);
            }
            Sleep(2u);
            continue;
        }

        const SVMS_Result result = core->api.fn.send_timed_short_batch(
            core->session, batch.data(), count);
        core->submittedEvents.fetch_add(count, std::memory_order_relaxed);
        core->lastSubmittedFrame = static_cast<uint64_t>(
            static_cast<int64_t>(batch[count - 1u].timestamp) - anchor);
        batch.clear();
        if (result != SVMS_RESULT_OK && result != SVMS_RESULT_CANCELLED)
            Sleep(2u);
    }
}

// ── Console / UI helpers ───────────────────────────────────────────────

bool EnableConsoleFancy() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) return false;
    return SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void FormatTime(char* buffer, size_t size, int64_t frames, uint32_t rate) {
    if (frames < 0) frames = 0;
    const double seconds = static_cast<double>(frames) /
                           static_cast<double>(rate ? rate : 1u);
    const int whole = static_cast<int>(seconds);
    snprintf(buffer, size, "%02d:%05.2f", whole / 60,
             seconds - 60.0 * (whole / 60));
}

// Draws the last `width` samples of `history` (ring buffer) as block glyphs.
void DrawSpark(char* out, size_t size, const float* history,
               uint32_t historyCount, uint32_t width, float scaleMax) {
    size_t used = 0;
    const uint32_t samples = historyCount < width ? historyCount : width;
    for (uint32_t i = 0; i < width; ++i) {
        float value = 0.0f;
        if (i >= width - samples) {
            const uint32_t index =
                (historyCount - (width - i)) % historyCount;
            value = history[index];
        }
        uint32_t band = 0;
        if (scaleMax > 0.0f) {
            band = static_cast<uint32_t>(value / scaleMax * 7.0f + 0.5f);
            if (band > 7u) band = 7u;
        }
        const int written = snprintf(out + used, size - used, "%s",
                                     kSparkBands[band]);
        if (written < 0 || used + static_cast<size_t>(written) >= size) break;
        used += static_cast<size_t>(written);
    }
}

// ── Live master volume via the config-JSON patch path ──────────────────

float ReadMasterVolume(PlayerCore& core) {
    std::vector<char> buffer(4096);
    uint32_t size = static_cast<uint32_t>(buffer.size());
    for (;;) {
        const SVMS_Result result = core.api.fn.get_config_json(
            core.session, buffer.data(), &size);
        if (result == SVMS_RESULT_OK) break;
        if (result != SVMS_RESULT_BUFFER_TOO_SMALL) return 1.0f;
        buffer.resize(size);
    }
    std::string document(buffer.data(), size);
    const size_t key = document.find("\"master_volume\"");
    if (key == std::string::npos) return 1.0f;
    const size_t colon = document.find(':', key);
    if (colon == std::string::npos) return 1.0f;
    return static_cast<float>(atof(document.c_str() + colon + 1));
}

bool PatchMasterVolume(PlayerCore& core, float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 2.0f) value = 2.0f;
    char patch[64];
    snprintf(patch, sizeof(patch), "{\"master_volume\":%.3f}", value);
    return core.api.fn.patch_config_json(
               core.session, patch,
               static_cast<uint32_t>(strlen(patch))) == SVMS_RESULT_OK;
}

// ── --stress-config: inciter for live-adoption races ───────────────────
// Hammers the same paths the configurator uses while audio renders:
// voice-pool resizes, worker-thread resizes, buffer resizes, gain sweeps
// and soundfont reloads. The VEH crash logger in the DLL captures stacks.

uint32_t StressRandom(uint64_t* state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return static_cast<uint32_t>(*state >> 32);
}

void StressConfigTick(PlayerCore& core, uint64_t* rng,
                      LARGE_INTEGER now, LARGE_INTEGER frequency,
                      LARGE_INTEGER* lastPatch,
                      LARGE_INTEGER* lastFontReload) {
    const double patchInterval =
        0.08 + 0.0001 * static_cast<double>(StressRandom(rng) % 500);
    if (lastPatch->QuadPart != 0) {
        const double elapsed =
            static_cast<double>(now.QuadPart - lastPatch->QuadPart) /
            static_cast<double>(frequency.QuadPart);
        if (elapsed < patchInterval) {
            // Soundfont reloads on their own ~1.2 s cadence.
            if (core.soundfontUtf8.empty()) return;
            const double sinceReload =
                static_cast<double>(now.QuadPart - lastFontReload->QuadPart) /
                static_cast<double>(frequency.QuadPart);
            if (sinceReload >= 1.2) {
                *lastFontReload = now;
                core.api.fn.load_soundfont_utf8(
                    core.session, core.soundfontUtf8.c_str());
            }
            return;
        }
    }
    *lastPatch = now;

    static const uint32_t kVoicePools[] = {64u, 256u, 1024u, 2048u, 4096u};
    static const uint32_t kRenderThreads[] = {0u, 1u, 2u, 4u, 6u, 8u};
    static const uint32_t kBufferFrames[] = {256u, 512u, 1024u, 2048u};
    switch (StressRandom(rng) % 6u) {
    case 0: {
        char patch[96];
        snprintf(patch, sizeof(patch),
                 "{\"synth\":{\"max_voices\":%u}}",
                 kVoicePools[StressRandom(rng) % 5u]);
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    case 1: {
        char patch[96];
        snprintf(patch, sizeof(patch),
                 "{\"synth\":{\"render_threads\":%u}}",
                 kRenderThreads[StressRandom(rng) % 6u]);
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    case 2: {
        char patch[96];
        snprintf(patch, sizeof(patch),
                 "{\"audio\":{\"buffer_frames\":%u}}",
                 kBufferFrames[StressRandom(rng) % 4u]);
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    case 3: {
        char patch[96];
        snprintf(patch, sizeof(patch),
                 "{\"synth\":{\"master_volume\":%.2f,"
                 "\"velocity_curve\":%.2f}}",
                 0.1f + 0.001f *
                     static_cast<float>(StressRandom(rng) % 1200u),
                 0.5f + 0.01f *
                     static_cast<float>(StressRandom(rng) % 300u));
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    case 4:
        if (!core.soundfontUtf8.empty()) {
            core.api.fn.load_soundfont_utf8(core.session,
                                            core.soundfontUtf8.c_str());
            *lastFontReload = now;
        }
        break;
    default: {
        char patch[128];
        snprintf(patch, sizeof(patch),
                 "{\"synth\":{\"max_voices\":%u,"
                 "\"render_threads\":%u}}",
                 kVoicePools[StressRandom(rng) % 5u],
                 kRenderThreads[StressRandom(rng) % 6u]);
        core.api.fn.patch_config_json(core.session, patch,
                                      static_cast<uint32_t>(strlen(patch)));
        break;
    }
    }
}

// ── Transport control (main thread) ────────────────────────────────────

DecodeArgs* g_decoderArgs = nullptr;
std::thread g_decoderThread;

void StopDecoder() {
    if (g_decoderArgs) g_decoderArgs->cancel.store(true);
    if (g_decoderThread.joinable()) g_decoderThread.join();
    g_decoderArgs = nullptr;  // DecoderThread owns and deletes its args
}

void StartDecoder(PlayerCore* core, uint64_t startFrame) {
    StopDecoder();
    g_decoderArgs = new DecodeArgs{core, core->filePath, startFrame};
    g_decoderThread = std::thread(DecoderThread, g_decoderArgs);
}

void StartPlayback(PlayerCore* core) {
    core->reanchor.store(true, std::memory_order_release);
    core->state.store(kStatePlaying, std::memory_order_release);
}

// Seeks are instant: reset() publishes a global termination fence in the
// engine (pending and scheduled events are rejected at dispatch), the ring
// is drained, and the decoder restarts from the target with channel replay.
void SeekTo(PlayerCore* core, uint64_t target) {
    const uint64_t limit =
        core->totalFrames ? core->totalFrames - 1u : 0u;
    if (target > limit) target = limit;
    core->state.store(kStateSeeking, std::memory_order_release);
    StopDecoder();
    svms::PackedMidiEvent drain{};
    while (core->ring->Pop(drain)) {
    }
    if (core->backend == kBackendSvms) core->api.fn.reset(core->session);
    else core->legacy.FullReset();
    core->cursorFrame.store(target, std::memory_order_relaxed);
    core->lastSubmittedFrame.store(target, std::memory_order_relaxed);
    core->endOfSong.store(false, std::memory_order_release);
    StartDecoder(core, target);
    StartPlayback(core);
}

// ── Entry ──────────────────────────────────────────────────────────────

void PrintUsage() {
    std::puts(
        "svms_player — console MIDI player for SuperVirtualMIDISynth\n"
        "\n"
        "usage: svms_player [options] <file.mid>\n"
        "  --backend <name>    svms (default) | kdapi | winmm\n"
        "  --dll <path>        synth module for the kdapi/winmm backends\n"
        "                      (default: OmniMIDI.dll / winmm.dll, app dir\n"
        "                      first — drop a synth DLL next to the player)\n"
        "  --sf2 <path>        load this soundfont into the engine (svms)\n"
        "  --ring-mb <n>       event ring budget in MB (default 64)\n"
        "  --lookahead-ms <n>  submit-ahead horizon (default 150, svms)\n"
        "  --loop              restart when the song ends\n"
        "  --auto <seconds>    quit automatically after N seconds of play\n"
        "  --quit-at-end       exit when the song finishes\n"
        "\n"
        "keys: SPACE pause/resume  S stop  LEFT/RIGHT seek 5s  "
        "UP/DOWN volume  L loop  Q quit");
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                            static_cast<int>(wide.size()), result.data(),
                            bytes, nullptr, nullptr);
    }
    return result;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    AddVectoredExceptionHandler(0, PlayerCrashFilter);

    std::wstring file;
    std::wstring soundfont;
    std::wstring apiPath;
    std::wstring backendName;
    std::wstring legacyPath;
    uint32_t ringMegabytes = 64u;
    uint32_t lookaheadMs = 150u;
    double autoSeconds = 0.0;
    bool loop = false;
    bool stressConfig = false;
    bool idleMode = false;
    bool quitAtEnd = false;

    std::vector<std::wstring> positional;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring {
            return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring();
        };
        if (arg == L"--sf2" && i + 1 < argc) {
            soundfont = next();
        } else if (arg == L"--api" && i + 1 < argc) {
            apiPath = next();
        } else if (arg == L"--backend" && i + 1 < argc) {
            backendName = next();
        } else if (arg == L"--dll" && i + 1 < argc) {
            legacyPath = next();
        } else if (arg == L"--ring-mb" && i + 1 < argc) {
            ringMegabytes = static_cast<uint32_t>(wcstoul(next().c_str(),
                                                          nullptr, 10));
        } else if (arg == L"--lookahead-ms" && i + 1 < argc) {
            lookaheadMs = static_cast<uint32_t>(wcstoul(next().c_str(),
                                                        nullptr, 10));
        } else if (arg == L"--auto" && i + 1 < argc) {
            autoSeconds = wcstod(next().c_str(), nullptr);
        } else if (arg == L"--loop") {
            loop = true;
        } else if (arg == L"--stress-config") {
            stressConfig = true;
        } else if (arg == L"--idle") {
            idleMode = true;
        } else if (arg == L"--quit-at-end") {
            quitAtEnd = true;
        } else if (arg == L"--help" || arg == L"-h") {
            PrintUsage();
            return 0;
        } else if (!arg.empty() && arg[0] == L'-') {
            std::fwprintf(stderr, L"unknown option: %ls\n", arg.c_str());
            PrintUsage();
            return 1;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.empty()) {
        PrintUsage();
        return 1;
    }
    file = positional.front();
    if (ringMegabytes < 1u) ringMegabytes = 1u;

    PlayerCore core;
    core.filePath = file;
    svms::ParsedEventRing ring(ringMegabytes);
    core.ring = &ring;

    if (backendName.empty() || backendName == L"svms") {
        core.backend = kBackendSvms;
    } else if (backendName == L"kdapi") {
        core.backend = kBackendKdapi;
    } else if (backendName == L"winmm") {
        core.backend = kBackendWinmm;
    } else {
        std::fwprintf(stderr, L"unknown backend: %ls\n", backendName.c_str());
        return 1;
    }
    const bool svmsBackend = core.backend == kBackendSvms;
    // Early-exit cleanup shared by every startup failure below.
    auto shutdownSink = [&core, svmsBackend]() {
        if (svmsBackend) {
            core.api.fn.destroy_session(core.session);
            core.session = 0;
            core.api.Unload();
        } else {
            core.legacy.Stop();
            core.legacy.Unload();
        }
    };

    std::string error;
    if (svmsBackend) {
        if (!core.api.Load(apiPath, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }

        if (core.api.fn.create_session(nullptr, &core.session) !=
                SVMS_RESULT_OK ||
            core.api.fn.set_ingress_mode(core.session,
                                         SVMS_INGRESS_LOSSLESS) !=
                SVMS_RESULT_OK) {
            std::fprintf(stderr, "error: could not create an SVMS session\n");
            shutdownSink();
            return 1;
        }

        if (!soundfont.empty()) {
            const std::string utf8 = WideToUtf8(soundfont);
            core.soundfontUtf8 = utf8;
            if (core.api.fn.load_soundfont_utf8(core.session, utf8.c_str()) !=
                SVMS_RESULT_OK) {
                std::fprintf(stderr, "warning: soundfont load failed (%ls)\n",
                             soundfont.c_str());
            }
        }
        core.stressConfig = stressConfig;

        // The decoder's frame domain is the engine's sample rate.
        uint64_t deviceNext = 0;
        if (core.api.fn.get_output_clock(core.session, &deviceNext,
                                         &core.sampleRate) != SVMS_RESULT_OK) {
            std::fprintf(stderr, "error: engine clock unavailable\n");
            shutdownSink();
            return 1;
        }
    } else {
        const LegacySink::Kind kind = core.backend == kBackendKdapi
                                          ? LegacySink::kKindKdapi
                                          : LegacySink::kKindWinmm;
        if (!core.legacy.Load(kind, legacyPath, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        if (!core.legacy.Start(error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            core.legacy.Unload();
            return 1;
        }
        core.sampleRate = 48000;  // decoder timebase (wall-clock paced)
        if (!soundfont.empty()) {
            std::fwprintf(stderr,
                          L"note: --sf2 ignored with the %ls backend — the "
                          L"synth loads its own soundfonts\n",
                          backendName.c_str());
        }
    }
    core.lookaheadFrames =
        static_cast<uint64_t>(lookaheadMs) * core.sampleRate / 1000u;
    core.delayFrames = core.sampleRate / 25u;  // 40 ms start offset

    // Scan first: validates the file and yields duration/peak statistics.
    svms::MappedMidiFile mapFile;
    if (!mapFile.Open(file.c_str(), error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        shutdownSink();
        return 1;
    }
    svms::MidiStreamInfo info;
    svms::MidiStreamDecoder decoder;
    std::printf("scanning...\r");
    std::fflush(stdout);
    if (!decoder.Scan(mapFile, core.sampleRate, info, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        shutdownSink();
        return 1;
    }
    core.totalFrames = info.totalFrames;

    // ── Launch threads and enter the console UI loop ────────────────────
    const bool fancy = EnableConsoleFancy();
    if (fancy) std::printf("\x1b[2J\x1b[?25l");

    char versionLabel[96];
    if (svmsBackend) {
        snprintf(versionLabel, sizeof(versionLabel), "%u.%u.%u (API %u)",
                 core.api.fn.product_major, core.api.fn.product_minor,
                 core.api.fn.product_patch, core.api.fn.abi_version);
    } else {
        char moduleAscii[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, core.legacy.moduleName, -1,
                            moduleAscii, sizeof(moduleAscii), nullptr,
                            nullptr);
        snprintf(versionLabel, sizeof(versionLabel), "%s backend — %.230s",
                 core.backend == kBackendKdapi ? "KDMAPI" : "WinMM",
                 moduleAscii);
    }
    std::printf("SVMS Player %s — %ls\n", versionLabel, file.c_str());
    std::printf("events %llu  notes %llu  duration %.1f s  peak %llu evt/s  "
                "dup note-ons %llu\n\n",
                static_cast<unsigned long long>(info.eventCount),
                static_cast<unsigned long long>(info.noteOnCount),
                static_cast<double>(info.totalFrames) / core.sampleRate,
                static_cast<unsigned long long>(info.peakEventsPerSecond),
                static_cast<unsigned long long>(
                    info.exactDuplicateNoteOnCount));

    StartDecoder(&core, 0);
    std::thread submit(SubmitThread, &core);
    if (!idleMode) StartPlayback(&core);

    float voiceHistory[128] = {};
    float renderHistory[128] = {};
    uint32_t historyCount = 0;
    uint32_t historyIndex = 0;
    uint64_t lastFeedSnapshot = 0;
    uint64_t feedRatePerSecond = 0;
    LARGE_INTEGER lastFeedQpc{};
    LARGE_INTEGER playingStart{}, uiFrequency{};
    QueryPerformanceFrequency(&uiFrequency);
    bool playingTimerStarted = false;
    float volume = 1.0f;
    if (svmsBackend) volume = ReadMasterVolume(core);
    bool running = true;
    bool songEnded = false;
    SVMS_TelemetryV1 telemetry{};
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    uint64_t stressRng = GetTickCount64() | 1u;
    LARGE_INTEGER lastStressPatch{}, lastStressFont{};

    while (running && !core.quit.load(std::memory_order_relaxed)) {
        // ── Input (30 ms cadence drives the UI framerate too) ──────────
        DWORD wait = WaitForSingleObject(stdinHandle, 30u);
        while (wait == WAIT_OBJECT_0) {
            INPUT_RECORD record{};
            DWORD read = 0;
            if (!ReadConsoleInputW(stdinHandle, &record, 1u, &read) ||
                read == 0u)
                break;
            if (record.EventType != KEY_EVENT ||
                !record.Event.KeyEvent.bKeyDown)
                break;
            const WORD key = record.Event.KeyEvent.wVirtualKeyCode;
            const int64_t playhead =
                core.playhead.load(std::memory_order_relaxed);
            switch (key) {
            case VK_SPACE:
                if (core.state.load(std::memory_order_relaxed) ==
                    kStatePlaying) {
                    core.state.store(kStatePausing,
                                     std::memory_order_release);
                } else if (core.state.load(std::memory_order_relaxed) ==
                           kStatePaused) {
                    StartPlayback(&core);
                } else if (core.state.load(std::memory_order_relaxed) ==
                           kStateIdle) {
                    SeekTo(&core, 0);
                }
                break;
            case 'S':
                SeekTo(&core, 0);
                core.state.store(kStateIdle, std::memory_order_release);
                break;
            case VK_LEFT:
                SeekTo(&core,
                       static_cast<uint64_t>(
                           playhead - 5i64 * core.sampleRate));
                break;
            case VK_RIGHT:
                SeekTo(&core,
                       static_cast<uint64_t>(
                           playhead + 5i64 * core.sampleRate));
                break;
            case VK_UP:
                volume += 0.1f;
                if (svmsBackend) PatchMasterVolume(core, volume);
                else core.legacy.SendVolume(volume);
                break;
            case VK_DOWN:
                volume -= 0.1f;
                if (svmsBackend) PatchMasterVolume(core, volume);
                else core.legacy.SendVolume(volume);
                break;
            case 'L':
                loop = !loop;
                break;
            case 'Q':
            case VK_ESCAPE:
                running = false;
                break;
            case VK_HOME:
                SeekTo(&core, 0);
                break;
            default:
                break;
            }
            wait = WaitForSingleObject(stdinHandle, 0u);
        }

        // ── Telemetry sampling ─────────────────────────────────────────
        if (core.stressConfig && svmsBackend) {
            LARGE_INTEGER stressNow{};
            QueryPerformanceCounter(&stressNow);
            StressConfigTick(core, &stressRng, stressNow, uiFrequency,
                             &lastStressPatch, &lastStressFont);
        }
        if (svmsBackend) {
            telemetry.struct_size = sizeof(telemetry);
            telemetry.struct_version = SVMS_STRUCT_VERSION_1;
            if (core.api.fn.get_telemetry(core.session, &telemetry) ==
                SVMS_RESULT_OK) {
                voiceHistory[historyIndex] =
                    static_cast<float>(telemetry.active_voices);
                renderHistory[historyIndex] = telemetry.render_time_ms;
                historyIndex = (historyIndex + 1u) % 128u;
                if (historyCount < 128u) ++historyCount;
            }
        } else {
            // Legacy synths expose (at most) a voice count; the render-time
            // graph is SVMS telemetry and stays flat here.
            telemetry.active_voices = core.legacy.VoiceCount();
            telemetry.audio_running = 1u;
            telemetry.sample_rate = core.sampleRate;
            voiceHistory[historyIndex] =
                static_cast<float>(telemetry.active_voices);
            renderHistory[historyIndex] = 0.0f;
            historyIndex = (historyIndex + 1u) % 128u;
            if (historyCount < 128u) ++historyCount;
        }

        // ── Auto-quit / end-of-song handling ───────────────────────────
        if (!playingTimerStarted && autoSeconds > 0.0) {
            playingTimerStarted = true;
            QueryPerformanceCounter(&playingStart);
        }
        if (playingTimerStarted && autoSeconds > 0.0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (static_cast<double>(now.QuadPart - playingStart.QuadPart) /
                    static_cast<double>(uiFrequency.QuadPart) >=
                autoSeconds)
                running = false;
        }
        if (core.endOfSong.exchange(false, std::memory_order_acq_rel)) {
            if (loop) {
                SeekTo(&core, 0);
            } else {
                if (svmsBackend) core.api.fn.reset(core.session);
                else core.legacy.FullReset();
                core.state.store(kStateIdle, std::memory_order_release);
                songEnded = true;
                if (quitAtEnd) running = false;
            }
        }
        if (core.decoderFailed.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "\ndecoder error: %s\n",
                         core.decoderError.c_str());
            running = false;
        }

        // ── Render ─────────────────────────────────────────────────────
        const uint64_t submittedNow =
            core.submittedEvents.load(std::memory_order_relaxed);
        {
            LARGE_INTEGER feedNow{};
            QueryPerformanceCounter(&feedNow);
            if (lastFeedQpc.QuadPart != 0) {
                const double elapsed =
                    static_cast<double>(feedNow.QuadPart -
                                        lastFeedQpc.QuadPart) /
                    static_cast<double>(uiFrequency.QuadPart);
                if (elapsed >= 0.5) {
                    feedRatePerSecond = static_cast<uint64_t>(
                        static_cast<double>(submittedNow -
                                            lastFeedSnapshot) / elapsed);
                    lastFeedSnapshot = submittedNow;
                    lastFeedQpc = feedNow;
                }
            } else {
                lastFeedSnapshot = submittedNow;
                lastFeedQpc = feedNow;
            }
        }
        const int64_t playhead =
            core.playhead.load(std::memory_order_relaxed);

        char played[32], total[32];
        FormatTime(played, sizeof(played), playhead, core.sampleRate);
        FormatTime(total, sizeof(total),
                   static_cast<int64_t>(core.totalFrames),
                   core.sampleRate);

        const char* stateName = "stopped";
        switch (core.state.load(std::memory_order_relaxed)) {
        case kStatePlaying: stateName = "PLAYING"; break;
        case kStatePausing: stateName = "pausing"; break;
        case kStatePaused:  stateName = "PAUSED"; break;
        case kStateSeeking: stateName = "seeking"; break;
        default: break;
        }
        if (songEnded &&
            core.state.load(std::memory_order_relaxed) == kStateIdle)
            stateName = "ENDED";
        if (core.state.load(std::memory_order_relaxed) == kStatePlaying)
            songEnded = false;

        char voiceSpark[512], renderSpark[512];
        float voiceMax = 1.0f, renderMax = 1.0f;
        for (uint32_t i = 0; i < historyCount; ++i) {
            if (voiceHistory[i] > voiceMax) voiceMax = voiceHistory[i];
            if (renderHistory[i] > renderMax) renderMax = renderHistory[i];
        }
        DrawSpark(voiceSpark, sizeof(voiceSpark), voiceHistory,
                  historyCount, 72u, voiceMax);
        DrawSpark(renderSpark, sizeof(renderSpark), renderHistory,
                  historyCount, 72u, renderMax);

        double progress =
            core.totalFrames
                ? static_cast<double>(playhead) /
                      static_cast<double>(core.totalFrames) * 100.0
                : 0.0;
        if (progress < 0.0) progress = 0.0;
        if (progress > 100.0) progress = 100.0;

        const uint64_t ringSize = core.ring->Size();
        const uint64_t ringCapacity = core.ring->Capacity();
        const uint32_t ringPercent = static_cast<uint32_t>(
            ringCapacity ? ringSize * 100u / ringCapacity : 0u);

        std::printf(
            "\x1b[H"
            "SVMS Player %s  [%s]  volume %.0f%%  %s\n"
            "--------------------------------------------------------------------------\n"
            "  %s / %s   (%.1f%%)   events %llu   feed %llu evt/s\n"
            "  voices %s   now %u  peak %.0f\n"
            "  render %s   now %.2f ms  peak %.2f ms\n"
            "  ring %u%%  steals %llu  audio %s  sf2 %s  rate %u\n"
            "  [space] pause  [s] stop  [left/right] seek  [up/down] volume  [l]oop  [q]uit\n",
            versionLabel, stateName, volume * 100.0f,
            loop ? "loop" : "",
            played, total, progress,
            static_cast<unsigned long long>(submittedNow),
            static_cast<unsigned long long>(feedRatePerSecond),
            voiceSpark,
            telemetry.active_voices, static_cast<double>(voiceMax),
            renderSpark, telemetry.render_time_ms, telemetry.render_peak,
            ringPercent,
            static_cast<unsigned long long>(telemetry.voice_steals),
            telemetry.audio_running ? "running" : "NOT RUNNING",
            telemetry.soundfont_loaded ? "loaded" : "none",
            telemetry.sample_rate);
    }

    // ── Shutdown: every player thread must be out of the DLL before the
    // session is destroyed — a call in flight during destroy races the
    // session-slot teardown (observed as a 0xC0000005 at exit). A submit
    // parked in a lossless wait can only be woken by
    // cancel_session_submissions (the API's designed shutdown path);
    // set_ingress_mode alone never reaches an already-blocked producer.
    core.quit.store(true, std::memory_order_release);
    core.state.store(kStateIdle, std::memory_order_release);
    if (svmsBackend) core.api.fn.cancel_session_submissions(core.session);
    submit.join();
    StopDecoder();
    if (svmsBackend) {
        core.api.fn.destroy_session(core.session);
        core.session = 0;
    } else {
        core.legacy.Stop();
    }
    if (fancy) std::printf("\x1b[?25h\n");
    // core.ring points at the stack-local `ring` — it dies with the scope.
    core.ring = nullptr;
    if (svmsBackend) core.api.Unload();
    else core.legacy.Unload();
    return 0;
}

