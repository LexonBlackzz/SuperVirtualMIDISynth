// Part of Player.cpp; included inside its anonymous namespace. Not standalone.

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
                const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
                    MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory);
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                  file, dumpType, &mei, nullptr, nullptr);
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
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        fn = SVMS_Interface{};  // a refused load must never leave a stale table
        fn.struct_size = sizeof(fn);
        if (getInterface(SVMS_ABI_VERSION_1, sizeof(fn), &fn) !=
                SVMS_RESULT_OK ||
            fn.abi_version != SVMS_ABI_VERSION_1) {
            error = "SVMSAPI.dll does not support ABI 1";
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        // ABI contract: the runtime reports its real table size in
        // struct_size and writes only that prefix. Zero everything beyond it
        // (a foreign or partial runtime must never leave a hole the player
        // would call — this was a real null-call crash with a wrapper that
        // returned a sparse table), then require every function the player
        // uses: a runtime that cannot fill the table cannot drive the
        // player and is refused cleanly here instead of crashing later.
        if (fn.struct_size < sizeof(fn)) {
            memset(reinterpret_cast<uint8_t*>(&fn) + fn.struct_size, 0,
                   sizeof(fn) - fn.struct_size);
        } else if (fn.struct_size > sizeof(fn)) {
            fn.struct_size = sizeof(fn);
        }
        static const struct { size_t offset; const char* name; } kRequired[] = {
#define SVMS_REQUIRED_SLOT(member) \
            { offsetof(SVMS_Interface, member), #member }
            SVMS_REQUIRED_SLOT(create_session),
            SVMS_REQUIRED_SLOT(destroy_session),
            SVMS_REQUIRED_SLOT(send_short),
            SVMS_REQUIRED_SLOT(reset),
            SVMS_REQUIRED_SLOT(get_telemetry),
            SVMS_REQUIRED_SLOT(send_timed_short_batch),
            SVMS_REQUIRED_SLOT(get_output_clock),
            SVMS_REQUIRED_SLOT(set_ingress_mode),
            SVMS_REQUIRED_SLOT(load_soundfont_utf8),
            SVMS_REQUIRED_SLOT(get_config_json),
            SVMS_REQUIRED_SLOT(patch_config_json),
            SVMS_REQUIRED_SLOT(cancel_session_submissions),
#undef SVMS_REQUIRED_SLOT
        };
        const uint8_t* table = reinterpret_cast<const uint8_t*>(&fn);
        for (const auto& slot : kRequired) {
            const void* ptr = nullptr;
            if (slot.offset + sizeof(ptr) <= fn.struct_size)
                memcpy(&ptr, table + slot.offset, sizeof(ptr));
            if (!ptr) {
                error = std::string("runtime table is missing ") +
                        slot.name + " — this module cannot drive the player";
                FreeLibrary(module);
                module = nullptr;
                return false;
            }
        }
        return true;
    }

    void Unload() {
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
        fn = SVMS_Interface{};  // no stale pointers after unload
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
