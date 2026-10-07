// Engine lifetime for the front ends, crash reporter, DllMain.

#include "SVMSDriverInternal.h"


bool EnsureDriverInitialized() {
    EnterCriticalSection(&g_frontendLock);
    if (g_driver && g_driver->initialized) {
        LeaveCriticalSection(&g_frontendLock);
        return true;
    }
    if (g_driver) { g_driver->Shutdown(); g_driver = nullptr; }
    g_driver = &svms::Driver::Instance();
    if (!g_driver->Initialize()) {
        LOG("KDMAPI: Initialize FAILED");
        XPBootstrapTrace("[SVMS XP] KDMAPI engine initialization FAILED\r\n");
#if !defined(SVMS_XP_COMPAT)
        g_driver->Shutdown();
        g_driver = nullptr;
#endif
        LeaveCriticalSection(&g_frontendLock);
        return false;
    }
    g_driver->LoadConfiguredSoundFont();
    const bool started = g_driver->StartAudio();
    LeaveCriticalSection(&g_frontendLock);
    return started;
}

void MaybeShutdownDriver() {
    if (g_winmmOwners.load(std::memory_order_acquire) != 0u ||
        g_nativeOwners.load(std::memory_order_acquire) != 0u ||
        g_kdmapiInitialized.load(std::memory_order_acquire))
        return;
    EnterCriticalSection(&g_frontendLock);
    if (g_winmmOwners.load(std::memory_order_acquire) == 0u &&
        g_nativeOwners.load(std::memory_order_acquire) == 0u &&
        !g_kdmapiInitialized.load(std::memory_order_acquire) && g_driver) {
        g_driver->Shutdown();
        g_driver = nullptr;
    }
    LeaveCriticalSection(&g_frontendLock);
}

extern "C" {

// ── Crash reporter ─────────────────────────────────────────────────────
// VEH that appends a symbolized stack trace for fatal faults to
// %TEMP%\SVMSV3Crash.log, then continues searching so the host
// application's own handler still observes (and reports) the crash.
// The handler path uses Win32 file APIs and preallocated static buffers
// only; dbghelp is initialized once at install time.
#if !defined(SVMS_XP_COMPAT)
#pragma comment(lib, "dbghelp.lib")
namespace {

bool g_crashSymReady = false;

void SvmsWriteCrashStack(HANDLE f, CONTEXT* faultCtx);

LONG WINAPI SvmsCrashFilter(EXCEPTION_POINTERS* ep) {
    static LONG armed = 0;
    if (InterlockedExchange(&armed, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;   // re-entry (fault inside the logger)
    __try {
        if (!ep || !ep->ExceptionRecord || ep->ExceptionRecord->ExceptionFlags != 0)
            __leave;
        // Report only faults inside our own module. Drop-in aliases share
        // processes with hosts and other synth engines (OmniMIDI's KDMAPI
        // driver can crash inside wvsprintfA while probing our shim);
        // walking foreign stacks from a VEH destabilizes the very tools
        // (DebugView) used to diagnose them.
        HMODULE selfModule = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(
                    static_cast<uintptr_t>(
                        reinterpret_cast<uintptr_t>(&SvmsCrashFilter))),
                &selfModule) || !selfModule)
            __leave;
        const uintptr_t selfBase =
            reinterpret_cast<uintptr_t>(selfModule);
        const uintptr_t faultAddress = reinterpret_cast<uintptr_t>(
            ep->ExceptionRecord->ExceptionAddress);
        HMODULE faultModule = nullptr;
        if (faultAddress < selfBase ||
            !GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(faultAddress), &faultModule) ||
            faultModule != selfModule)
            __leave;
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        // Fatal hardware-style faults only; leave C++ EH / breakpoints alone.
        const bool fatal =
            code == EXCEPTION_ACCESS_VIOLATION ||
            code == EXCEPTION_ILLEGAL_INSTRUCTION ||
            code == EXCEPTION_STACK_OVERFLOW ||
            code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
            code == EXCEPTION_PRIV_INSTRUCTION ||
            code == 0xC0000409u /* fastfail */;
        if (!fatal || !g_crashSymReady)
            __leave;
        char path[MAX_PATH];
        UINT n = GetTempPathA(MAX_PATH, path);
        if (n == 0 || n + 16 >= MAX_PATH) __leave;
        lstrcatA(path, "SVMSV3Crash.log");
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) __leave;
        char line[512];
        DWORD written = 0;
        SYSTEMTIME st; GetLocalTime(&st);
        wsprintfA(line,
                  "\r\n==== SVMS V3 crash %04u-%02u-%02u %02u:%02u:%02u pid=%lu tid=%lu\r\n"
                  "code=0x%08lX addr=0x%p\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  GetCurrentProcessId(), GetCurrentThreadId(),
                  code, ep->ExceptionRecord->ExceptionAddress);
        WriteFile(f, line, lstrlenA(line), &written, nullptr);
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            wsprintfA(line, "%s at 0x%p\r\n",
                      ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                      reinterpret_cast<void*>(
                          ep->ExceptionRecord->ExceptionInformation[1]));
            WriteFile(f, line, lstrlenA(line), &written, nullptr);
        }
        SvmsWriteCrashStack(f, ep->ContextRecord);
        CloseHandle(f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Logger faulted — swallow, never mask the original crash.
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void SvmsWriteCrashStack(HANDLE f, CONTEXT* faultCtx) {
    CONTEXT ctx = *faultCtx;
    STACKFRAME64 frame = {};
#if defined(_M_X64)
    const DWORD imageType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86)
    const DWORD imageType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp; frame.AddrStack.Mode = AddrModeFlat;
#else
    (void)f; (void)ctx; return;
#endif
    char line[512];
    for (unsigned depth = 0; depth < 32; ++depth) {
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc) {
            HMODULE mod = nullptr;
            uintptr_t rva = 0;
            const char* modName = "?";
            char modBuf[MAX_PATH];
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(pc)),
                    &mod) && mod) {
                rva = static_cast<uintptr_t>(pc) -
                      reinterpret_cast<uintptr_t>(mod);
                if (GetModuleFileNameA(mod, modBuf, MAX_PATH)) {
                    modName = modBuf;
                    for (char* s = modBuf; *s; ++s)
                        if (*s == '\\' || *s == '/') modName = s + 1;
                }
            }
            wsprintfA(line, "  %s+0x%llX", modName,
                      static_cast<unsigned long long>(rva));
            char symBuf[sizeof(SYMBOL_INFO) + 192] = {};
            SYMBOL_INFO* s = reinterpret_cast<SYMBOL_INFO*>(symBuf);
            s->SizeOfStruct = sizeof(SYMBOL_INFO);
            s->MaxNameLen = 191;
            DWORD64 disp = 0;
            if (g_crashSymReady &&
                SymFromAddr(GetCurrentProcess(), pc, &disp, s)) {
                lstrcatA(line, "  ");
                lstrcatA(line, s->Name);
                wsprintfA(line + lstrlenA(line), " + %llu",
                          static_cast<unsigned long long>(disp));
            }
            lstrcatA(line, "\r\n");
            DWORD written = 0;
            WriteFile(f, line, lstrlenA(line), &written, nullptr);
        }
        if (!StackWalk64(imageType, GetCurrentProcess(), GetCurrentThread(),
                         &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr))
            break;
    }
}

void SvmsInstallCrashReporter() {
    if (!SymInitialize(GetCurrentProcess(), nullptr, FALSE))
        return;
    g_crashSymReady = true;
    AddVectoredExceptionHandler(0, SvmsCrashFilter);
}

} // namespace
#else
namespace { void SvmsInstallCrashReporter() {} } // XP: no dbghelp reporter
#endif

// ── DLL Entry ──────────────────────────────────────────────────────────

// ── DLL Entry ──────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        SvmsInstallCrashReporter();
        InitializeCriticalSection(&g_frontendLock);
        // Pin ourselves: a drop-in winmm replacement cannot survive being
        // unloaded while render/worker threads execute inside it (the host
        // calling FreeLibrary mid-render deadlocks on the loader lock in
        // DLL_PROCESS_DETACH). The extra self-reference keeps the module
        // resident for the life of the process; process exit reclaims it.
        wchar_t selfPath[MAX_PATH];
        if (GetModuleFileNameW(hinstDLL, selfPath, MAX_PATH) > 0)
            LoadLibraryW(selfPath);
        XPBootstrapTrace("[SVMS XP] V3 winmm.dll loaded\r\n");
        LogInit();
        LOG("DLL_PROCESS_ATTACH");
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        LOG("DLL_PROCESS_DETACH");
    }
    return TRUE;
}

} // extern "C"
