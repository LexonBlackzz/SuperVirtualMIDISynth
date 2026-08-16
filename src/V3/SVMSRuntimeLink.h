#ifndef SVMS_RUNTIME_LINK_H
#define SVMS_RUNTIME_LINK_H
//
// RuntimeLink — cross-process IPC between the SVMS V3 driver (winmm.dll) and
// the V3 Configurator.  Uses per-PID shared memory with a double-buffered
// telemetry snapshot and a lock-free SPSC command ring.
//
// Thread model (driver side):
//   - Audio thread writes process-local telemetry (never touches IPC)
//   - Control thread copies telemetry to shared memory + drains command ring
//
// Thread model (configurator side):
//   - UI thread reads telemetry (lock-free acquire)
//   - UI thread writes commands under named mutex

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>

#include "SVMSRuntimeLinkProtocol.h"

namespace svms {

// ─── Driver-side: create and manage RuntimeLink ─────────────────────────────

class RuntimeLinkDriver {
public:
    RuntimeLinkDriver() = default;
    ~RuntimeLinkDriver() { Shutdown(); }

    // Non-copyable
    RuntimeLinkDriver(const RuntimeLinkDriver&) = delete;
    RuntimeLinkDriver& operator=(const RuntimeLinkDriver&) = delete;

    bool Initialize() {
        Shutdown();

        DWORD pid = GetCurrentProcessId();
        wchar_t memName[128], mutexName[128], evtName[128];
        RL_SharedMemName(pid, memName, 128);
        RL_MutexName(pid, mutexName, 128);
        RL_CmdEventName(pid, evtName, 128);

        size_t mapSize = RuntimeLinkMappingSize();

        // Create shared memory mapping
        hMap_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, static_cast<DWORD>(mapSize), memName);
        if (!hMap_) return false;

        view_ = static_cast<RuntimeLinkSharedMemory*>(
            MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, mapSize));
        if (!view_) { CloseHandle(hMap_); hMap_ = nullptr; return false; }

        // Initialize header (can't copy-assign due to std::atomic members)
        view_->header.magic = kRuntimeLinkMagic;
        view_->header.version = kRuntimeLinkVersion;
        view_->header.size = static_cast<uint32_t>(mapSize);
        view_->header.flags = 0;
        view_->header.telemetryWriteIndex.store(0, std::memory_order_relaxed);
        view_->header.telemetryPadding = 0;
        view_->header.cmdTail.store(0, std::memory_order_relaxed);
        view_->header.cmdHead = 0;
        view_->header.cmdCapacity = kRuntimeLinkCmdRingCapacity;
        view_->header.cmdPadding = 0;
        std::memset(view_->header.reserved, 0, sizeof(view_->header.reserved));

        // Initialize telemetry slots
        view_->telemetry[0] = RLTelemetry{};
        view_->telemetry[1] = RLTelemetry{};

        // Create mutex (initially owned by nobody)
        hMutex_ = CreateMutexW(nullptr, FALSE, mutexName);
        if (!hMutex_) { Shutdown(); return false; }

        // Create command event (manual reset, initially non-signaled)
        hCmdEvent_ = CreateEventW(nullptr, TRUE, FALSE, evtName);
        if (!hCmdEvent_) { Shutdown(); return false; }

        pid_ = pid;
        initialized_ = true;
        return true;
    }

    void Shutdown() {
        StopControlThread();

        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
        if (hMutex_) { CloseHandle(hMutex_); hMutex_ = nullptr; }
        if (hCmdEvent_) { CloseHandle(hCmdEvent_); hCmdEvent_ = nullptr; }
        initialized_ = false;
    }

    bool IsInitialized() const { return initialized_; }
    uint32_t GetPID() const { return pid_; }

    // ── Telemetry publishing (called from control thread) ────────────────

    void PublishTelemetry(const RLTelemetry& snap) {
        if (!view_) return;
        uint32_t writeIdx = view_->header.telemetryWriteIndex.load(
            std::memory_order_relaxed);
        view_->telemetry[writeIdx ^ 1] = snap;
        // Publish with release semantics so reader sees complete data
        view_->header.telemetryWriteIndex.store(
            writeIdx ^ 1, std::memory_order_release);
    }

    // ── Command ring (called from control thread) ───────────────────────

    bool PopCommand(RLCommand& cmd) {
        if (!view_) return false;
        return RL_PopCommand(view_, cmd);
    }

    void SignalCommandAvailable() {
        if (hCmdEvent_) SetEvent(hCmdEvent_);
    }

    // ── Control thread ──────────────────────────────────────────────────

    using CommandHandler = std::function<void(const RLCommand&)>;

    bool StartControlThread(CommandHandler handler) {
        if (!initialized_) return false;
        commandHandler_ = std::move(handler);
        controlThreadRunning_ = true;
        controlThread_ = std::thread(&RuntimeLinkDriver::ControlThreadProc, this);
        return true;
    }

    void StopControlThread() {
        controlThreadRunning_ = false;
        if (hCmdEvent_) SetEvent(hCmdEvent_); // wake thread to exit
        if (controlThread_.joinable()) controlThread_.join();
        commandHandler_ = nullptr;
    }

private:
    void ControlThreadProc() {
        // Set normal priority (not time-critical)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

        while (controlThreadRunning_) {
            // Wait for command event or timeout (100ms poll for telemetry update)
            DWORD wait = WaitForSingleObject(hCmdEvent_, 100);

            if (wait == WAIT_OBJECT_0) {
                // Process all pending commands
                RLCommand cmd;
                while (PopCommand(cmd)) {
                    if (commandHandler_) commandHandler_(cmd);
                }
                ResetEvent(hCmdEvent_);
            }
        }
    }

    HANDLE hMap_       = nullptr;
    HANDLE hMutex_     = nullptr;
    HANDLE hCmdEvent_  = nullptr;
    RuntimeLinkSharedMemory* view_ = nullptr;
    uint32_t pid_      = 0;
    bool initialized_  = false;

    std::thread controlThread_;
    std::atomic<bool> controlThreadRunning_{false};
    CommandHandler commandHandler_;
};

// ─── Configurator-side: open and read/write RuntimeLink ─────────────────────

class RuntimeLinkClient {
public:
    RuntimeLinkClient() = default;
    ~RuntimeLinkClient() { Close(); }

    RuntimeLinkClient(const RuntimeLinkClient&) = delete;
    RuntimeLinkClient& operator=(const RuntimeLinkClient&) = delete;

    bool Open(uint32_t pid) {
        Close();

        wchar_t memName[128], mutexName[128], evtName[128];
        RL_SharedMemName(pid, memName, 128);
        RL_MutexName(pid, mutexName, 128);
        RL_CmdEventName(pid, evtName, 128);

        hMap_ = OpenFileMappingW(FILE_MAP_READ, FALSE, memName);
        if (!hMap_) return false;

        view_ = static_cast<const RuntimeLinkSharedMemory*>(
            MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0,
                          RuntimeLinkMappingSize()));
        if (!view_) { CloseHandle(hMap_); hMap_ = nullptr; return false; }

        hMutex_ = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, mutexName);
        // Mutex is optional — commands may be unavailable
        hCmdEvent_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, evtName);

        pid_ = pid;
        return true;
    }

    void Close() {
        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
        if (hMutex_) { CloseHandle(hMutex_); hMutex_ = nullptr; }
        if (hCmdEvent_) { CloseHandle(hCmdEvent_); hCmdEvent_ = nullptr; }
        pid_ = 0;
    }

    bool IsOpen() const { return view_ != nullptr; }
    uint32_t GetPID() const { return pid_; }

    // ── Read telemetry (lock-free) ──────────────────────────────────────

    RLTelemetry ReadTelemetry() const {
        if (!view_) return {};
        return RL_ReadTelemetry(view_);
    }

    // ── Write command (under mutex) ─────────────────────────────────────

    bool PushCommand(const RLCommand& cmd) {
        if (!view_ || !hMutex_) return false;

        DWORD wait = WaitForSingleObject(hMutex_, 1000);
        if (wait == WAIT_TIMEOUT) return false;

        bool ok = RL_PushCommand(const_cast<RuntimeLinkSharedMemory*>(view_), cmd);
        ReleaseMutex(hMutex_);

        if (ok && hCmdEvent_) SetEvent(hCmdEvent_);
        return ok;
    }

    // Convenience: push a single-float command
    bool PushFloatCommand(RLCommandType type, float value) {
        RLCommand cmd;
        cmd.type = type;
        cmd.value0 = value;
        return PushCommand(cmd);
    }

    // Convenience: push a bool command
    bool PushBoolCommand(RLCommandType type, bool value) {
        RLCommand cmd;
        cmd.type = type;
        cmd.param0 = value ? 1u : 0u;
        return PushCommand(cmd);
    }

private:
    HANDLE hMap_      = nullptr;
    HANDLE hMutex_    = nullptr;
    HANDLE hCmdEvent_ = nullptr;
    const RuntimeLinkSharedMemory* view_ = nullptr;
    uint32_t pid_     = 0;
};

} // namespace svms

#endif // SVMS_RUNTIME_LINK_H
