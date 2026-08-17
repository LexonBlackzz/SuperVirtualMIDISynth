#ifndef SVMS_RUNTIME_LINK_H
#define SVMS_RUNTIME_LINK_H
//
// RuntimeLink V2 — cross-process IPC between the SVMS V3 driver
// (winmm.dll) and the V3 Configurator.  See SVMSRuntimeLinkProtocol.h
// for the full protocol specification.
//
// Thread model (driver side):
//   - Audio thread: updates the process-local RuntimeAudioSnapshot
//     (odd/even sequence).  Never touches shared memory, mutexes,
//     events, mappings, or the hosts registry.
//   - Control thread: waits ~33 ms per cycle, drains the command
//     mailbox (if any), publishes telemetry into shared memory with
//     the odd/even sequence guard, refreshes the hosts-registry
//     heartbeat, and signals the auto-reset command event.
//
// Thread model (configurator side):
//   - UI thread: reads telemetry (skip-if-busy) and sends commands.
//     Command-payload copies are serialized with the per-PID named
//     mutex (cross-process copy only).  The ACK wait polls
//     processedId/processedToken and can also be woken by the
//     command event.

#if !defined(SVMS_XP_COMPAT)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <stdlib.h>
#include <cstring>
#include <functional>
#include <atomic>
#include <thread>

#include "SVMSRuntimeLinkProtocol.h"

namespace svms {

// ─── Driver side ------------------------------------------------------------

class RuntimeLinkDriverV2 {
public:
    RuntimeLinkDriverV2() = default;
    ~RuntimeLinkDriverV2() { Shutdown(); }

    RuntimeLinkDriverV2(const RuntimeLinkDriverV2&) = delete;
    RuntimeLinkDriverV2& operator=(const RuntimeLinkDriverV2&) = delete;

    using TelemetryProvider = std::function<RuntimeLinkTelemetryV2()>;
    using CommandHandler = std::function<RLResult(
        const RuntimeLinkCommandV2&, char* /*outResultText[256]*/)>;

    // Creates the per-PID mapping/mutex/event and registers this
    // process in the hosts registry.  Non-fatal: returns false without
    // touching anything when the objects already exist (PID reuse) or
    // a system call fails.
    bool Initialize() {
        Shutdown();

        pid_ = GetCurrentProcessId();
        QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&qpcFreq_));

        // sessionId: QPC/time/pid mix (rand_s is not reliably exposed by
        // MSVC's stdlib.h across toolset versions; non-cryptographic use).
        {
            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            const uint32_t r0 = static_cast<uint32_t>(GetTickCount() ^ qpc.LowPart);
            const uint32_t r1 = static_cast<uint32_t>(pid_ ^ qpc.HighPart);
            sessionId_ = (static_cast<uint64_t>(r0) << 32) | r1;
            if (sessionId_ == 0u) sessionId_ = 1u;
        }

        wchar_t memName[128], mutexName[128], evtName[128];
        RLV2_SharedMemName(pid_, memName, 128);
        RLV2_MutexName(pid_, mutexName, 128);
        RLV2_CmdEventName(pid_, evtName, 128);

        const uint32_t mapSize = RuntimeLinkMappingSizeV2();

        hMap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, mapSize, memName);
        if (!hMap_ || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
            return false;
        }
        view_ = static_cast<RuntimeLinkSharedMemoryV2*>(
            MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, mapSize));
        if (!view_) { CloseHandle(hMap_); hMap_ = nullptr; return false; }

        hMutex_ = CreateMutexW(nullptr, FALSE, mutexName);
        if (!hMutex_ || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hMutex_) { CloseHandle(hMutex_); hMutex_ = nullptr; }
            Shutdown();
            return false;
        }
        // Auto-reset command event (wakes the control thread early).
        hCmdEvent_ = CreateEventW(nullptr, FALSE, FALSE, evtName);
        if (!hCmdEvent_ || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hCmdEvent_) { CloseHandle(hCmdEvent_); hCmdEvent_ = nullptr; }
            Shutdown();
            return false;
        }

        std::memset(view_, 0, mapSize);

        RuntimeLinkHeaderV2& h = view_->header;
        h.magic = kRuntimeLinkMagic;
        h.version = kRuntimeLinkVersion;
        h.size = mapSize;
        h.publisherPid = pid_;
        h.structSize = sizeof(RuntimeLinkTelemetryV2);
#ifdef _WIN64
        h.archClass = kRuntimeLinkArchX64;
#else
        h.archClass = kRuntimeLinkArchX86;
#endif
        h.headerCrc = RLV2_HeaderCrc(h);
        // telemetrySequence starts at 2 (settled, empty slot).

        if (!EnsureHostsRegistry()) { Shutdown(); return false; }
        RegisterHostSlot();

        initialized_ = true;
        return true;
    }

    void Shutdown() {
        StopControlThread();
        if (initialized_ && hostsView_) UnregisterHostSlot();

        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
        if (hMutex_) { CloseHandle(hMutex_); hMutex_ = nullptr; }
        if (hCmdEvent_) { CloseHandle(hCmdEvent_); hCmdEvent_ = nullptr; }
        if (hostsView_) { UnmapViewOfFile(hostsView_); hostsView_ = nullptr; }
        if (hostsMap_) { CloseHandle(hostsMap_); hostsMap_ = nullptr; }
        if (hostsMutex_) { CloseHandle(hostsMutex_); hostsMutex_ = nullptr; }
        initialized_ = false;
    }

    bool IsInitialized() const { return initialized_; }
    uint32_t GetPID() const { return pid_; }
    uint64_t GetSessionId() const { return sessionId_; }

    bool StartControlThread(TelemetryProvider provider,
                            CommandHandler handler) {
        if (!initialized_) return false;
        telemetryProvider_ = std::move(provider);
        commandHandler_ = std::move(handler);
        controlThreadRunning_.store(true, std::memory_order_release);
        try {
            controlThread_ = std::thread(&RuntimeLinkDriverV2::ControlThreadProc, this);
        } catch (...) {
            controlThreadRunning_.store(false, std::memory_order_release);
            telemetryProvider_ = nullptr;
            commandHandler_ = nullptr;
            return false;
        }
        return true;
    }

    void StopControlThread() {
        controlThreadRunning_.store(false, std::memory_order_release);
        if (hCmdEvent_) SetEvent(hCmdEvent_);  // wake the wait
        if (controlThread_.joinable()) controlThread_.join();
        telemetryProvider_ = nullptr;
        commandHandler_ = nullptr;
    }

private:
    void ControlThreadProc() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        while (controlThreadRunning_.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForSingleObject(hCmdEvent_,
                kRuntimeLinkPublishIntervalMs);
            if (!controlThreadRunning_.load(std::memory_order_acquire)) break;
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
                for (int i = 0; i < 4; ++i) {
                    if (!ProcessPendingCommand()) break;
                }
                PublishTelemetry();
            } else {
                Sleep(5);
            }
        }
    }

    // Returns true when a new command was captured.  The payload copy
    // happens only after the (requestId, token) commit markers settle,
    // so a racing client write can never produce a torn payload.
    bool ProcessPendingCommand() {
        if (!view_ || busyWithReload_) return false;
        volatile RuntimeLinkHeaderV2* h = &view_->header;

        uint32_t req = h->commandRequestId;
        uint32_t tok = h->commandRequestToken;
        if (req == 0u || (req == processedId_ && tok == processedToken_)) {
            return false;
        }

        RuntimeLinkCommandV2 local{};
        for (int attempt = 0; attempt < 4; ++attempt) {
            local = view_->command;
            RLV2_MemBarrier();
            const uint32_t req2 = h->commandRequestId;
            const uint32_t tok2 = h->commandRequestToken;
            if (req2 != req || tok2 != tok) {
                req = req2;
                tok = tok2;
                continue;
            }
            local = view_->command;  // re-read post-commit
            RLResult result = RLResult::InternalError;
            char resultText[kRuntimeLinkResultTextCapacity] = {};
            if (commandHandler_) {
                if (local.type == static_cast<uint32_t>(RLCommandType::ReloadSoundFont)) {
                    busyWithReload_ = true;
                }
                result = commandHandler_(local, resultText);
                busyWithReload_ = false;
            }
            resultText[kRuntimeLinkResultTextCapacity - 1] = '\0';
            std::memcpy(view_->command.resultText, resultText,
                        kRuntimeLinkResultTextCapacity);
            RLV2_MemBarrier();
            h->commandResult = static_cast<uint32_t>(result);
            h->commandProcessedId = req;
            h->commandProcessedToken = tok;
            RLV2_MemBarrier();
            processedId_ = req;
            processedToken_ = tok;
            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            h->heartbeatQpc = static_cast<uint64_t>(qpc.QuadPart);
            if (hCmdEvent_) SetEvent(hCmdEvent_);  // wake waiting clients
            return true;
        }
        // Unstable capture (two clients racing): leave for the next tick.
        return false;
    }

    void PublishTelemetry() {
        if (!view_) return;

        RuntimeLinkTelemetryV2 snap = telemetryProvider_
            ? telemetryProvider_() : RuntimeLinkTelemetryV2{};
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        snap.timestampQpc = static_cast<uint64_t>(qpc.QuadPart);

        volatile RuntimeLinkHeaderV2* h = &view_->header;
        // Monotonic seqlock publish: odd = writer inside the slot, even =
        // stable.  telemetrySeq_ only ever advances by 2 per publish, so a
        // reader can never mistake two consecutive publishes for one (the
        // old 1 -> 2 toggle repeated its values and could hide a torn
        // mid-copy capture under ABA).
        h->telemetrySequence = telemetrySeq_ | 1u;
        RLV2_MemBarrier();
        view_->telemetry = snap;
        RLV2_MemBarrier();
        telemetrySeq_ += 2u;
        h->telemetrySequence = telemetrySeq_;
        h->heartbeatQpc = snap.timestampQpc;
        RLV2_MemBarrier();

        UpdateHostsHeartbeat(snap.timestampQpc);
    }

    // ── Hosts registry ─────────────────────────────────────────────────

    bool EnsureHostsRegistry() {
        wchar_t regName[128], mtxName[128];
        RLV2_HostsRegName(regName, 128);
        RLV2_HostsMutexName(mtxName, 128);

        const uint32_t regSize = sizeof(RuntimeHostsRegistryV2);
        hostsMap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                       PAGE_READWRITE, 0, regSize, regName);
        if (!hostsMap_) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        hostsView_ = static_cast<RuntimeHostsRegistryV2*>(
            MapViewOfFile(hostsMap_, FILE_MAP_ALL_ACCESS, 0, 0, regSize));
        if (!hostsView_) return false;
        if (created) {
            std::memset(hostsView_, 0, regSize);
            hostsView_->magic = kRuntimeLinkMagic;
            hostsView_->version = kRuntimeLinkVersion;
            hostsView_->slotCapacity = kRuntimeHostMaxCount;
            hostRegistryInitialized_ = true;
        } else {
            // Verify an already-created registry looks plausible.
            volatile RuntimeHostsRegistryV2* r = hostsView_;
            hostRegistryInitialized_ =
                r->magic == kRuntimeLinkMagic &&
                r->version == kRuntimeLinkVersion &&
                r->slotCapacity == kRuntimeHostMaxCount;
            if (!hostRegistryInitialized_ && r->magic == 0u && r->version == 0u) {
                // Weird half-initialized state from a crashed creator:
                // take ownership.
                std::memset(hostsView_, 0, regSize);
                hostRegistryInitialized_ = true;
            }
        }

        hostsMutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mtxName);
        if (!hostsMutex_) {
            HANDLE createdMutex = CreateMutexW(nullptr, FALSE, mtxName);
            if (createdMutex) {
                if (GetLastError() == ERROR_ALREADY_EXISTS) {
                    CloseHandle(createdMutex);
                    hostsMutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE,
                                             FALSE, mtxName);
                } else {
                    hostsMutex_ = createdMutex;
                }
            }
        }
        return hostsMutex_ != nullptr && hostRegistryInitialized_;
    }

    // Claims (or refreshes) this driver's registry slot under the hosts
    // mutex.  Slot policy: same sessionId wins; otherwise an empty slot;
    // otherwise the stalest slot.
    void RegisterHostSlot() {
        if (!hostsView_ || !hostsMutex_ || sessionId_ == 0u) return;

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        const uint64_t now = static_cast<uint64_t>(qpc.QuadPart);

        if (WaitForSingleObject(hostsMutex_, kRuntimeLinkMutexTimeoutMs)
                != WAIT_OBJECT_0) {
            return;
        }
        int own = -1, empty = -1, stale = -1;
        uint64_t staleAge = 0;
        for (int i = 0; i < kRuntimeHostMaxCount; ++i) {
            const RuntimeHostSlotV2& s = hostsView_->slots[i];
            if (s.pid == pid_ && s.sessionId == sessionId_) { own = i; break; }
            if (RLV2_HostsSlotIsEmpty(s) && empty < 0) empty = i;
            if (!RLV2_HostsSlotIsFresh(s, now, qpcFreq_, kRuntimeHostTimeoutMs)
                && stale < 0) {
                stale = i;
                staleAge = 0;
            }
        }
        int target = own;
        if (target < 0) target = empty;
        if (target < 0) target = stale;
        if (target >= 0) {
            RuntimeHostSlotV2& s = hostsView_->slots[target];
            s.magic = kRuntimeLinkMagic;
            s.pid = pid_;
#ifdef _WIN64
            s.archClass = kRuntimeLinkArchX64;
#else
            s.archClass = kRuntimeLinkArchX86;
#endif
            s.sessionId = sessionId_;
            s.lastHeartbeatQpc = now;
            hostRegistered_ = true;
        } else {
            // Table completely full of fresh entries — heartbeats no-op.
            hostRegistered_ = false;
        }
        ReleaseMutex(hostsMutex_);
    }

    void UpdateHostsHeartbeat(uint64_t nowQpc) {
        if (!hostsView_ || !hostsMutex_ || !hostRegistered_) return;
        if (WaitForSingleObject(hostsMutex_, kRuntimeLinkMutexTimeoutMs)
                != WAIT_OBJECT_0) {
            return;
        }
        for (int i = 0; i < kRuntimeHostMaxCount; ++i) {
            RuntimeHostSlotV2& s = hostsView_->slots[i];
            if (s.pid == pid_ && s.sessionId == sessionId_) {
                s.lastHeartbeatQpc = nowQpc;
                break;
            }
        }
        ReleaseMutex(hostsMutex_);
    }

    void UnregisterHostSlot() {
        if (!hostsView_ || !hostsMutex_) return;
        if (WaitForSingleObject(hostsMutex_, kRuntimeLinkMutexTimeoutMs)
                != WAIT_OBJECT_0) {
            return;
        }
        for (int i = 0; i < kRuntimeHostMaxCount; ++i) {
            RuntimeHostSlotV2& s = hostsView_->slots[i];
            if (s.pid == pid_ && s.sessionId == sessionId_) {
                std::memset(&s, 0, sizeof(s));
                break;
            }
        }
        ReleaseMutex(hostsMutex_);
    }

    HANDLE hMap_       = nullptr;
    HANDLE hMutex_     = nullptr;
    HANDLE hCmdEvent_  = nullptr;
    RuntimeLinkSharedMemoryV2* view_ = nullptr;
    uint32_t pid_      = 0;
    uint64_t sessionId_ = 0;
    uint64_t qpcFreq_  = 0;
    bool initialized_  = false;

    HANDLE hostsMap_   = nullptr;
    HANDLE hostsMutex_ = nullptr;
    RuntimeHostsRegistryV2* hostsView_ = nullptr;
    bool hostRegistryInitialized_ = false;
    bool hostRegistered_ = false;

    std::thread controlThread_;
    std::atomic<bool> controlThreadRunning_{false};
    TelemetryProvider telemetryProvider_;
    CommandHandler commandHandler_;
    uint32_t processedId_      = 0;
    uint32_t processedToken_   = 0;
    // Monotonic telemetry seqlock word; starts settled (even) at 2 and
    // advances by 2 per publish (see PublishTelemetry).
    uint32_t telemetrySeq_     = 2u;
    bool busyWithReload_       = false;
};

// ─── Configurator side ------------------------------------------------------

class RuntimeLinkClientV2 {
public:
    RuntimeLinkClientV2() = default;
    ~RuntimeLinkClientV2() { Close(); }

    RuntimeLinkClientV2(const RuntimeLinkClientV2&) = delete;
    RuntimeLinkClientV2& operator=(const RuntimeLinkClientV2&) = delete;

    struct HostInfo {
        uint32_t pid       = 0;
        uint32_t archClass = 0;
        uint64_t sessionId = 0;
        bool fresh         = false;
    };

    bool Open(uint32_t pid) {
        Close();

        wchar_t memName[128], mutexName[128], evtName[128];
        RLV2_SharedMemName(pid, memName, 128);
        RLV2_MutexName(pid, mutexName, 128);
        RLV2_CmdEventName(pid, evtName, 128);

        // Commands are written into the mapping, so the view needs
        // read+write access (V1 wrongly opened FILE_MAP_READ and wrote
        // through a const_cast).
        hMap_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, memName);
        if (!hMap_) return false;
        // Map the whole file (dwBytesToMap == 0); size is validated via
        // header.size after the header is visible.
        view_ = static_cast<RuntimeLinkSharedMemoryV2*>(
            MapViewOfFile(hMap_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
        if (!view_) { CloseHandle(hMap_); hMap_ = nullptr; return false; }

        if (!ValidateHeader(pid)) { Close(); return false; }

        // The client releases the mutex after writing a command, so it
        // needs MUTEX_MODIFY_STATE in addition to SYNCHRONIZE (a mutex
        // opened with SYNCHRONIZE alone makes ReleaseMutex fail).
        hMutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName);
        if (!hMutex_) { Close(); return false; }
        hCmdEvent_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, evtName);  // optional

        pid_ = pid;
        QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&qpcFreq_));

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        uint32_t r0 = static_cast<uint32_t>(GetTickCount() ^ qpc.LowPart) ^ pid_
                    ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
        token_ = r0 ? r0 : 1u;
        requestCounter_ = 1;
        hasTelemetry_ = false;
        return true;
    }

    void Close() {
        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
        if (hMutex_) { CloseHandle(hMutex_); hMutex_ = nullptr; }
        if (hCmdEvent_) { CloseHandle(hCmdEvent_); hCmdEvent_ = nullptr; }
        pid_ = 0;
        hasTelemetry_ = false;
    }

    bool IsOpen() const { return view_ != nullptr; }
    uint32_t GetPID() const { return pid_; }

    // ── Telemetry (skip-if-busy) ──────────────────────────────────────
    //
    // Returns false when the driver is mid-publish (odd sequence or the
    // sequence changed during the copy); out is then left as the last
    // successfully read snapshot.
    bool ReadTelemetry(RuntimeLinkTelemetryV2& out) {
        if (!view_) return false;
        volatile RuntimeLinkHeaderV2* h = &view_->header;

        const uint32_t seq = h->telemetrySequence;
        if ((seq & 1u) != 0u) {            // writer inside the slot
            if (hasTelemetry_) out = lastTelemetry_;
            return false;
        }
        RuntimeLinkTelemetryV2 t = view_->telemetry;
        RLV2_MemBarrier();
        if (h->telemetrySequence != seq) { // changed mid-copy
            if (hasTelemetry_) out = lastTelemetry_;
            return false;
        }
        lastTelemetry_ = t;
        hasTelemetry_ = true;
        out = t;
        return true;
    }

    // Heartbeat-based death detection (QPC deltas, system-wide clock).
    bool IsHostAlive(uint32_t timeoutMs) const {
        if (!view_ || qpcFreq_ == 0u) return false;
        volatile RuntimeLinkHeaderV2* h = &view_->header;
        const uint64_t hb = h->heartbeatQpc;
        if (hb == 0u) return false;
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        const uint64_t now = static_cast<uint64_t>(qpc.QuadPart);
        const uint64_t ageQpc = now > hb ? now - hb : 0u;
        return (ageQpc * 1000u) / qpcFreq_ < timeoutMs;
    }

    // ── Commands ──────────────────────────────────────────────────────
    //
    // Blocks up to timeoutMs waiting for the driver's ACK.  Returns
    // the driver's result (RLResult) or Busy on timeout.  resultText
    // is best-effort diagnostic text for the last ACKed command.
    RLResult SendCommand(RLCommandType type, uint32_t groupMask,
                         uint32_t param, const RuntimeLiveStateV2& live,
                         uint32_t timeoutMs, char resultText[
                             kRuntimeLinkResultTextCapacity] = nullptr) {
        if (!view_ || !hMutex_) {
            if (resultText) {
                strncpy_s(resultText, kRuntimeLinkResultTextCapacity,
                          "not connected", _TRUNCATE);
            }
            return RLResult::InternalError;
        }

        const uint32_t req = requestCounter_++;
        if (WaitForSingleObject(hMutex_, kRuntimeLinkMutexTimeoutMs)
                != WAIT_OBJECT_0) {
            if (resultText) {
                strncpy_s(resultText, kRuntimeLinkResultTextCapacity,
                          "command mutex timeout", _TRUNCATE);
            }
            return RLResult::Busy;
        }

        // Payload first, commit markers last.
        RuntimeLinkCommandV2& slot = view_->command;
        slot.type = static_cast<uint32_t>(type);
        slot.groupMask = groupMask;
        slot.param = param;
        slot.live = live;
        RLV2_MemBarrier();
        volatile RuntimeLinkHeaderV2* h = &view_->header;
        h->commandRequestToken = token_;
        RLV2_MemBarrier();
        h->commandRequestId = req;
        RLV2_MemBarrier();
        ReleaseMutex(hMutex_);
        if (hCmdEvent_) SetEvent(hCmdEvent_);   // early wake for the driver

        // Wait for the ACK (poll; the event keeps the driver waking early
        // even if a second client pollutes the slot).
        const uint32_t startTick = GetTickCount();
        const int32_t timeoutDelta = static_cast<int32_t>(timeoutMs);
        for (;;) {
            const uint32_t proc = h->commandProcessedId;
            const uint32_t procTok = h->commandProcessedToken;
            if (proc == req && procTok == token_) {
                const RLResult result = static_cast<RLResult>(h->commandResult);
                if (resultText) {
                    std::memcpy(resultText, view_->command.resultText,
                                kRuntimeLinkResultTextCapacity);
                }
                return result;
            }
            const int32_t elapsed =
                static_cast<int32_t>(GetTickCount() - startTick);
            if (elapsed >= timeoutDelta) break;
            Sleep(2);
        }
        if (resultText) {
            strncpy_s(resultText, kRuntimeLinkResultTextCapacity,
                      "no ACK within timeout", _TRUNCATE);
        }
        return RLResult::Busy;
    }

    RLResult SendPing(uint32_t timeoutMs,
                      char resultText[kRuntimeLinkResultTextCapacity] = nullptr) {
        return SendCommand(RLCommandType::Ping, 0u, 0u,
                           RuntimeLiveStateV2{}, timeoutMs, resultText);
    }

    // ── Discovery ─────────────────────────────────────────────────────
    //
    // Enumerates live driver instances from the hosts registry.
    // Returns the number of hosts written; entries with fresh == false
    // are stale slots (crashed publishers) included for diagnostics.
    static uint32_t EnumerateHosts(HostInfo* out, uint32_t maxCount) {
        if (!out || maxCount == 0u) return 0u;

        wchar_t regName[128], mtxName[128];
        RLV2_HostsRegName(regName, 128);
        RLV2_HostsMutexName(mtxName, 128);

        HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, regName);
        if (!hMap) return 0u;
        RuntimeHostsRegistryV2* reg = static_cast<RuntimeHostsRegistryV2*>(
            MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
        HANDLE hMtx = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mtxName);
        uint32_t count = 0u;
        if (reg && hMtx &&
            WaitForSingleObject(hMtx, kRuntimeLinkMutexTimeoutMs)
                == WAIT_OBJECT_0) {
            volatile RuntimeHostsRegistryV2* r = reg;
            if (r->magic == kRuntimeLinkMagic &&
                r->version == kRuntimeLinkVersion &&
                r->slotCapacity == kRuntimeHostMaxCount) {
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                uint64_t freq = 0;
                QueryPerformanceFrequency(
                    reinterpret_cast<LARGE_INTEGER*>(&freq));
                const uint64_t now = static_cast<uint64_t>(qpc.QuadPart);
                for (uint32_t i = 0;
                     i < kRuntimeHostMaxCount && count < maxCount; ++i) {
                    const RuntimeHostSlotV2& s = reg->slots[i];
                    if (RLV2_HostsSlotIsEmpty(s)) continue;
                    HostInfo& info = out[count++];
                    info.pid = s.pid;
                    info.archClass = s.archClass;
                    info.sessionId = s.sessionId;
                    info.fresh = RLV2_HostsSlotIsFresh(
                        s, now, freq, kRuntimeHostTimeoutMs);
                }
            }
            ReleaseMutex(hMtx);
        }
        if (hMtx) CloseHandle(hMtx);
        if (reg) UnmapViewOfFile(reg);
        CloseHandle(hMap);
        return count;
    }

private:
    bool ValidateHeader(uint32_t pid) const {
        // Field-by-field copy: volatile members cannot be passed to
        // non-volatile helpers (RLV2_HeaderCrc takes a const&).
        volatile RuntimeLinkHeaderV2* h = &view_->header;
        RuntimeLinkHeaderV2 local{};
        local.heartbeatQpc = h->heartbeatQpc;
        local.magic = h->magic;
        local.version = h->version;
        local.size = h->size;
        local.publisherPid = h->publisherPid;
        local.structSize = h->structSize;
        local.archClass = h->archClass;
        local.headerCrc = h->headerCrc;
        local.telemetrySequence = h->telemetrySequence;
        local.commandRequestId = h->commandRequestId;
        local.commandRequestToken = h->commandRequestToken;
        local.commandProcessedId = h->commandProcessedId;
        local.commandProcessedToken = h->commandProcessedToken;
        local.commandResult = h->commandResult;

        if (local.magic != kRuntimeLinkMagic) return false;
        if (local.version != kRuntimeLinkVersion) return false;
        if (local.publisherPid == 0u || local.publisherPid != pid) return false;
#ifdef _WIN64
        if (local.archClass != kRuntimeLinkArchX64) return false;
#endif
        if (local.structSize != sizeof(RuntimeLinkTelemetryV2)) return false;
        const uint32_t required = RLV2_HeaderSize()
                                + RLV2_PadTo64(local.structSize)
                                + sizeof(RuntimeLinkCommandV2);
        if (local.size < required) return false;
        const uint32_t crc = RLV2_HeaderCrc(local);
        return local.headerCrc == crc;
    }

    HANDLE hMap_      = nullptr;
    HANDLE hMutex_    = nullptr;
    HANDLE hCmdEvent_ = nullptr;
    RuntimeLinkSharedMemoryV2* view_ = nullptr;
    uint32_t pid_     = 0;
    uint64_t qpcFreq_ = 0;
    uint32_t token_   = 0;
    uint32_t requestCounter_ = 0;
    RuntimeLinkTelemetryV2 lastTelemetry_{};
    bool hasTelemetry_ = false;
};

} // namespace svms

#endif // !defined(SVMS_XP_COMPAT)

#endif // SVMS_RUNTIME_LINK_H