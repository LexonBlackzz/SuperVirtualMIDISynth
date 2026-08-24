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
#include <cstdio>
#include <functional>
#include <atomic>
#include <thread>

#include "SVMSRuntimeLinkProtocol.h"
#include "SVMSLiveControl.h"

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

    // Creates or reclaims the per-PID mapping/mutex/event and registers
    // this process in the hosts registry. Other processes may keep the named
    // objects alive across a transient MIDI-driver shutdown, so reopening an
    // existing object is expected and the new session republishes its state.
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
        if (!hMap_) return false;
        view_ = static_cast<RuntimeLinkSharedMemoryV2*>(
            MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, mapSize));
        if (!view_) { CloseHandle(hMap_); hMap_ = nullptr; return false; }

        hMutex_ = CreateMutexW(nullptr, FALSE, mutexName);
        if (!hMutex_) {
            Shutdown();
            return false;
        }
        // Auto-reset command event (wakes the control thread early).
        hCmdEvent_ = CreateEventW(nullptr, FALSE, FALSE, evtName);
        if (!hCmdEvent_) {
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

        // V2 remains authoritative for already-shipped configurators. V3 is
        // published in parallel and is deliberately non-fatal so telemetry
        // can never prevent MIDI/audio initialization.
        InitializeV3();

        initialized_ = true;
        return true;
    }

    void Shutdown() {
        StopControlThread();
        if (initialized_ && hostsView_) UnregisterHostSlot();
        ShutdownV3();

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
    bool IsV3Initialized() const { return v3Initialized_; }
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
        if (v3CmdEvent_) SetEvent(v3CmdEvent_);
        if (controlThread_.joinable()) controlThread_.join();
        telemetryProvider_ = nullptr;
        commandHandler_ = nullptr;
    }

private:
    void ControlThreadProc() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        while (controlThreadRunning_.load(std::memory_order_acquire)) {
            HANDLE waits[2] = { hCmdEvent_, v3CmdEvent_ };
            const DWORD waitCount = v3CmdEvent_ ? 2u : 1u;
            const DWORD wait = WaitForMultipleObjects(
                waitCount, waits, FALSE, kRuntimeLinkPublishIntervalMs);
            if (!controlThreadRunning_.load(std::memory_order_acquire)) break;
            if ((wait >= WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + waitCount) ||
                wait == WAIT_TIMEOUT) {
                for (int i = 0; i < 4; ++i) {
                    const bool v2 = ProcessPendingCommand();
                    const bool v3 = ProcessPendingCommandV3();
                    if (!v2 && !v3) break;
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
        if (!view_) return false;
        return ProcessPendingCommandSlot(&view_->header, view_->command,
                                         processedId_, processedToken_,
                                         hCmdEvent_);
    }

    bool ProcessPendingCommandV3() {
        if (!v3View_) return false;
        return ProcessPendingCommandSlot(&v3View_->header, v3View_->command,
                                         v3ProcessedId_, v3ProcessedToken_,
                                         v3CmdEvent_);
    }

    template <typename Header>
    bool ProcessPendingCommandSlot(volatile Header* h,
                                   RuntimeLinkCommandV2& sharedCommand,
                                   uint32_t& processedId,
                                   uint32_t& processedToken,
                                   HANDLE commandEvent) {
        if (!h || busyWithReload_) return false;

        uint32_t req = h->commandRequestId;
        uint32_t tok = h->commandRequestToken;
        if (req == 0u || (req == processedId && tok == processedToken)) {
            return false;
        }

        RuntimeLinkCommandV2 local{};
        for (int attempt = 0; attempt < 4; ++attempt) {
            local = sharedCommand;
            RLV2_MemBarrier();
            const uint32_t req2 = h->commandRequestId;
            const uint32_t tok2 = h->commandRequestToken;
            if (req2 != req || tok2 != tok) {
                req = req2;
                tok = tok2;
                continue;
            }
            local = sharedCommand;  // re-read post-commit
            RLResult result = RLResult::InternalError;
            char resultText[kRuntimeLinkResultTextCapacity] = {};
            bool skipHandler = false;

            // Voice-cap changes are process-local control state, not shared
            // memory consumed by the audio thread. Validate against the pool
            // allocated at startup, then publish one atomic request that the
            // VoiceManager observes at a render boundary.
            if (local.type == static_cast<uint32_t>(RLCommandType::ApplyLiveConfig) &&
                (local.groupMask & RLGroupVoices) != 0u) {
                const uint32_t requested = local.live.maxVoices;
                const uint32_t capacity = RuntimeVoicePoolCapacity();
                if (requested == 0u) {
                    result = RLResult::InvalidArgument;
                    strncpy_s(resultText, kRuntimeLinkResultTextCapacity,
                              "voice cap must be at least 1", _TRUNCATE);
                    skipHandler = true;
                } else if (capacity != 0u && requested > capacity) {
                    result = RLResult::RestartRequired;
                    std::snprintf(resultText, kRuntimeLinkResultTextCapacity,
                                  "voice cap %u exceeds startup pool %u",
                                  requested, capacity);
                    skipHandler = true;
                } else {
                    RequestRuntimeVoiceLimit(requested);
                }
            }

            if (!skipHandler && commandHandler_) {
                if (local.type == static_cast<uint32_t>(RLCommandType::ReloadSoundFont)) {
                    busyWithReload_ = true;
                }
                result = commandHandler_(local, resultText);
                busyWithReload_ = false;
            }
            resultText[kRuntimeLinkResultTextCapacity - 1] = '\0';
            std::memcpy(sharedCommand.resultText, resultText,
                        kRuntimeLinkResultTextCapacity);
            RLV2_MemBarrier();
            h->commandResult = static_cast<uint32_t>(result);
            h->commandProcessedId = req;
            h->commandProcessedToken = tok;
            RLV2_MemBarrier();
            processedId = req;
            processedToken = tok;
            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            h->heartbeatQpc = static_cast<uint64_t>(qpc.QuadPart);
            if (commandEvent) SetEvent(commandEvent);  // wake waiting clients
            return true;
        }
        // Unstable capture (two clients racing): leave for the next tick.
        return false;
    }

    void PublishTelemetry() {
        if (!view_) return;

        RuntimeLinkTelemetryV2 snap = telemetryProvider_
            ? telemetryProvider_() : RuntimeLinkTelemetryV2{};
        // The driver mailbox echo predates the live voice-cap field. Overlay
        // the audio-thread-applied logical cap here; snap.maxVoices remains
        // the physical startup pool so the Configurator can distinguish
        // "live now" from "restart required to grow".
        const uint32_t appliedVoiceLimit = AppliedRuntimeVoiceLimit();
        if (appliedVoiceLimit != 0u) snap.live.maxVoices = appliedVoiceLimit;

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
        PublishTelemetryV3(snap);
    }

    void PublishTelemetryV3(const RuntimeLinkTelemetryV2& snap) {
        if (!v3View_) return;
        volatile RuntimeDiscoveryHeaderV3* h = &v3View_->header;
        h->telemetrySequence = v3TelemetrySeq_ | 1u;
        RLV2_MemBarrier();
        v3View_->telemetry = snap;
        RLV2_MemBarrier();
        v3TelemetrySeq_ += 2u;
        h->telemetrySequence = v3TelemetrySeq_;
        h->heartbeatQpc = snap.timestampQpc;
        RLV2_MemBarrier();
        UpdateV3HostsHeartbeat(snap.timestampQpc);
    }

    // ── Hosts registry ─────────────────────────────────────────────────

    bool InitializeV3() {
        ShutdownV3();

        wchar_t memName[128], mutexName[128], evtName[128];
        RLV3_SharedMemName(pid_, memName, 128);
        RLV3_MutexName(pid_, mutexName, 128);
        RLV3_CmdEventName(pid_, evtName, 128);

        const uint32_t mapSize = RuntimeLinkMappingSizeV3();
        v3Map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                    PAGE_READWRITE, 0, mapSize, memName);
        if (!v3Map_) {
            ShutdownV3();
            return false;
        }
        v3View_ = static_cast<RuntimeLinkSharedMemoryV3*>(
            MapViewOfFile(v3Map_, FILE_MAP_ALL_ACCESS, 0, 0, mapSize));
        if (!v3View_) {
            ShutdownV3();
            return false;
        }

        v3Mutex_ = CreateMutexW(nullptr, FALSE, mutexName);
        if (!v3Mutex_) {
            ShutdownV3();
            return false;
        }
        v3CmdEvent_ = CreateEventW(nullptr, FALSE, FALSE, evtName);
        if (!v3CmdEvent_) {
            ShutdownV3();
            return false;
        }

        std::memset(v3View_, 0, mapSize);
        RuntimeDiscoveryHeaderV3& h = v3View_->header;
        h.magic = kRuntimeDiscoveryMagic;
        h.headerVersion = kRuntimeDiscoveryHeaderVersion;
        h.headerSize = sizeof(RuntimeDiscoveryHeaderV3);
        h.totalSize = mapSize;
        h.publisherPid = pid_;
#ifdef _WIN64
        h.archClass = kRuntimeLinkArchX64;
#else
        h.archClass = kRuntimeLinkArchX86;
#endif
        h.productMajor = build::kProductMajor;
        h.productMinor = build::kProductMinor;
        h.productPatch = build::kProductPatch;
        h.buildNumber = build::kBuildNumber;
        h.releaseChannel = build::kReleaseChannelId;
        h.protocolMin = build::kRuntimeProtocolMin;
        h.protocolMax = build::kRuntimeProtocolMax;
        h.nativeAbiMin = build::kNativeAbiMin;
        h.nativeAbiMax = build::kNativeAbiMax;
        h.telemetryOffset = offsetof(RuntimeLinkSharedMemoryV3, telemetry);
        h.telemetrySize = sizeof(RuntimeLinkTelemetryV2);
        h.commandOffset = offsetof(RuntimeLinkSharedMemoryV3, command);
        h.commandSize = sizeof(RuntimeLinkCommandV2);
        h.accessFlags = kRuntimeAccessTelemetryRead |
                        kRuntimeAccessCommandWrite;
        h.capabilityFlags = build::kDriverCapabilities;
        h.headerCrc = RLV3_HeaderCrc(h);
        h.sessionId = sessionId_;
        h.telemetrySequence = 2u;
        v3TelemetrySeq_ = 2u;

        if (!EnsureV3HostsRegistry()) {
            ShutdownV3();
            return false;
        }
        RegisterV3HostSlot();
        v3Initialized_ = true;
        return true;
    }

    void ShutdownV3() {
        if (v3HostRegistered_) UnregisterV3HostSlot();
        if (v3View_) { UnmapViewOfFile(v3View_); v3View_ = nullptr; }
        if (v3Map_) { CloseHandle(v3Map_); v3Map_ = nullptr; }
        if (v3Mutex_) { CloseHandle(v3Mutex_); v3Mutex_ = nullptr; }
        if (v3CmdEvent_) { CloseHandle(v3CmdEvent_); v3CmdEvent_ = nullptr; }
        if (v3HostsView_) {
            UnmapViewOfFile(v3HostsView_);
            v3HostsView_ = nullptr;
        }
        if (v3HostsMap_) { CloseHandle(v3HostsMap_); v3HostsMap_ = nullptr; }
        if (v3HostsMutex_) {
            CloseHandle(v3HostsMutex_);
            v3HostsMutex_ = nullptr;
        }
        v3Initialized_ = false;
        v3HostRegistered_ = false;
        v3ProcessedId_ = 0u;
        v3ProcessedToken_ = 0u;
        v3TelemetrySeq_ = 2u;
    }

    bool EnsureV3HostsRegistry() {
        wchar_t regName[128], mutexName[128];
        RLV3_HostsRegName(regName, 128);
        RLV3_HostsMutexName(mutexName, 128);
        const uint32_t bytes = sizeof(RuntimeDiscoveryHostsRegistryV1);

        v3HostsMap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0, bytes, regName);
        if (!v3HostsMap_) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        v3HostsView_ = static_cast<RuntimeDiscoveryHostsRegistryV1*>(
            MapViewOfFile(v3HostsMap_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
        if (!v3HostsView_) return false;

        if (created) {
            std::memset(v3HostsView_, 0, bytes);
            v3HostsView_->magic = kRuntimeDiscoveryRegistryMagic;
            v3HostsView_->version = kRuntimeDiscoveryRegistryVersion;
            v3HostsView_->totalSize = bytes;
            v3HostsView_->slotCapacity = kRuntimeHostMaxCount;
            v3HostsView_->slotSize = sizeof(RuntimeDiscoveryHostSlotV1);
        } else if (v3HostsView_->magic != kRuntimeDiscoveryRegistryMagic ||
                   v3HostsView_->version != kRuntimeDiscoveryRegistryVersion ||
                   v3HostsView_->totalSize < bytes ||
                   v3HostsView_->slotCapacity != kRuntimeHostMaxCount ||
                   v3HostsView_->slotSize != sizeof(RuntimeDiscoveryHostSlotV1)) {
            return false;
        }

        v3HostsMutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE,
                                   FALSE, mutexName);
        if (!v3HostsMutex_) {
            HANDLE createdMutex = CreateMutexW(nullptr, FALSE, mutexName);
            if (createdMutex) {
                if (GetLastError() == ERROR_ALREADY_EXISTS) {
                    CloseHandle(createdMutex);
                    v3HostsMutex_ = OpenMutexW(
                        SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName);
                } else {
                    v3HostsMutex_ = createdMutex;
                }
            }
        }
        return v3HostsMutex_ != nullptr;
    }

    void RegisterV3HostSlot() {
        if (!v3HostsView_ || !v3HostsMutex_ || sessionId_ == 0u) return;
        if (WaitForSingleObject(v3HostsMutex_, kRuntimeLinkMutexTimeoutMs) !=
            WAIT_OBJECT_0) return;

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        const uint64_t now = static_cast<uint64_t>(qpc.QuadPart);
        int target = -1;
        int stale = -1;
        for (uint32_t i = 0u; i < kRuntimeHostMaxCount; ++i) {
            RuntimeDiscoveryHostSlotV1& slot = v3HostsView_->slots[i];
            if (slot.pid == pid_ && slot.sessionId == sessionId_) {
                target = static_cast<int>(i);
                break;
            }
            if (target < 0 && RLV3_HostsSlotIsEmpty(slot))
                target = static_cast<int>(i);
            if (stale < 0 && !RLV3_HostsSlotIsFresh(
                    slot, now, qpcFreq_, kRuntimeHostTimeoutMs))
                stale = static_cast<int>(i);
        }
        if (target < 0) target = stale;
        if (target >= 0) {
            RuntimeDiscoveryHostSlotV1& slot = v3HostsView_->slots[target];
            std::memset(&slot, 0, sizeof(slot));
            slot.magic = kRuntimeDiscoveryMagic;
            slot.pid = pid_;
#ifdef _WIN64
            slot.archClass = kRuntimeLinkArchX64;
#else
            slot.archClass = kRuntimeLinkArchX86;
#endif
            slot.headerVersion = kRuntimeDiscoveryHeaderVersion;
            slot.sessionId = sessionId_;
            slot.lastHeartbeatQpc = now;
            slot.capabilityFlags = build::kDriverCapabilities;
            slot.productMajor = build::kProductMajor;
            slot.productMinor = build::kProductMinor;
            slot.productPatch = build::kProductPatch;
            slot.buildNumber = build::kBuildNumber;
            slot.protocolMax = build::kRuntimeProtocolMax;
            slot.nativeAbiMax = build::kNativeAbiMax;
            v3HostRegistered_ = true;
        }
        ReleaseMutex(v3HostsMutex_);
    }

    void UpdateV3HostsHeartbeat(uint64_t nowQpc) {
        if (!v3HostsView_ || !v3HostsMutex_ || !v3HostRegistered_) return;
        if (WaitForSingleObject(v3HostsMutex_, kRuntimeLinkMutexTimeoutMs) !=
            WAIT_OBJECT_0) return;
        for (uint32_t i = 0u; i < kRuntimeHostMaxCount; ++i) {
            RuntimeDiscoveryHostSlotV1& slot = v3HostsView_->slots[i];
            if (slot.pid == pid_ && slot.sessionId == sessionId_) {
                slot.lastHeartbeatQpc = nowQpc;
                break;
            }
        }
        ReleaseMutex(v3HostsMutex_);
    }

    void UnregisterV3HostSlot() {
        if (!v3HostsView_ || !v3HostsMutex_) return;
        if (WaitForSingleObject(v3HostsMutex_, kRuntimeLinkMutexTimeoutMs) !=
            WAIT_OBJECT_0) return;
        for (uint32_t i = 0u; i < kRuntimeHostMaxCount; ++i) {
            RuntimeDiscoveryHostSlotV1& slot = v3HostsView_->slots[i];
            if (slot.pid == pid_ && slot.sessionId == sessionId_) {
                std::memset(&slot, 0, sizeof(slot));
                break;
            }
        }
        ReleaseMutex(v3HostsMutex_);
        v3HostRegistered_ = false;
    }

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

    HANDLE v3Map_ = nullptr;
    HANDLE v3Mutex_ = nullptr;
    HANDLE v3CmdEvent_ = nullptr;
    RuntimeLinkSharedMemoryV3* v3View_ = nullptr;
    HANDLE v3HostsMap_ = nullptr;
    HANDLE v3HostsMutex_ = nullptr;
    RuntimeDiscoveryHostsRegistryV1* v3HostsView_ = nullptr;
    bool v3Initialized_ = false;
    bool v3HostRegistered_ = false;
    uint32_t v3ProcessedId_ = 0u;
    uint32_t v3ProcessedToken_ = 0u;
    uint32_t v3TelemetrySeq_ = 2u;

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
                             kRuntimeLinkResultTextCapacity] = nullptr,
                         const char* requestText = nullptr) {
        if (!view_ || !hMutex_) {
            if (resultText) {
                strncpy_s(resultText, kRuntimeLinkResultTextCapacity,
                          "not connected", _TRUNCATE);
            }
            return RLResult::InternalError;
        }

        const size_t requestLength = requestText
            ? strnlen_s(requestText, kRuntimeLinkCommandTextCapacity) : 0u;
        if (requestText && requestLength >= kRuntimeLinkCommandTextCapacity) {
            if (resultText) {
                strncpy_s(resultText, kRuntimeLinkResultTextCapacity,
                          "command text is too long", _TRUNCATE);
            }
            return RLResult::InvalidArgument;
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
        std::memset(slot.resultText, 0, sizeof(slot.resultText));
        if (requestLength != 0u)
            std::memcpy(slot.resultText, requestText, requestLength);
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

class RuntimeLinkClientV3 {
public:
    struct PeerInfo {
        uint32_t pid = 0u;
        uint32_t archClass = 0u;
        uint32_t productMajor = 0u;
        uint32_t productMinor = 0u;
        uint32_t productPatch = 0u;
        uint32_t buildNumber = 0u;
        uint32_t releaseChannel = 0u;
        uint32_t protocolMin = 0u;
        uint32_t protocolMax = 0u;
        uint32_t nativeAbiMin = 0u;
        uint32_t nativeAbiMax = 0u;
        uint64_t capabilityFlags = 0u;
        uint64_t sessionId = 0u;
        bool fresh = false;
    };

    RuntimeLinkClientV3() = default;
    ~RuntimeLinkClientV3() { Close(); }
    RuntimeLinkClientV3(const RuntimeLinkClientV3&) = delete;
    RuntimeLinkClientV3& operator=(const RuntimeLinkClientV3&) = delete;

    bool Open(uint32_t pid) {
        Close();
        wchar_t memName[128], mutexName[128], eventName[128];
        RLV3_SharedMemName(pid, memName, 128);
        RLV3_MutexName(pid, mutexName, 128);
        RLV3_CmdEventName(pid, eventName, 128);

        hMap_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                 memName);
        if (!hMap_) return false;
        mapping_ = static_cast<uint8_t*>(MapViewOfFile(
            hMap_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
        if (!mapping_) {
            Close();
            return false;
        }
        header_ = reinterpret_cast<RuntimeDiscoveryHeaderV3*>(mapping_);
        if (!ValidateHeader(pid)) {
            Close();
            return false;
        }

        telemetry_ = reinterpret_cast<RuntimeLinkTelemetryV2*>(
            mapping_ + peer_.telemetryOffset);
        command_ = reinterpret_cast<RuntimeLinkCommandV2*>(
            mapping_ + peer_.commandOffset);
        hMutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                             mutexName);
        if (!hMutex_) {
            Close();
            return false;
        }
        hCmdEvent_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
        pid_ = pid;
        QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&qpcFreq_));
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        token_ = static_cast<uint32_t>(GetTickCount() ^ qpc.LowPart ^ pid ^
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this)));
        if (token_ == 0u) token_ = 1u;
        requestCounter_ = 1u;
        hasTelemetry_ = false;
        return true;
    }

    void Close() {
        telemetry_ = nullptr;
        command_ = nullptr;
        header_ = nullptr;
        if (mapping_) { UnmapViewOfFile(mapping_); mapping_ = nullptr; }
        if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
        if (hMutex_) { CloseHandle(hMutex_); hMutex_ = nullptr; }
        if (hCmdEvent_) { CloseHandle(hCmdEvent_); hCmdEvent_ = nullptr; }
        pid_ = 0u;
        qpcFreq_ = 0u;
        hasTelemetry_ = false;
        peer_ = CapturedHeader{};
    }

    bool IsOpen() const { return header_ != nullptr; }
    uint32_t GetPID() const { return pid_; }

    PeerInfo GetPeerInfo() const {
        PeerInfo info{};
        info.pid = peer_.publisherPid;
        info.archClass = peer_.archClass;
        info.productMajor = peer_.productMajor;
        info.productMinor = peer_.productMinor;
        info.productPatch = peer_.productPatch;
        info.buildNumber = peer_.buildNumber;
        info.releaseChannel = peer_.releaseChannel;
        info.protocolMin = peer_.protocolMin;
        info.protocolMax = peer_.protocolMax;
        info.nativeAbiMin = peer_.nativeAbiMin;
        info.nativeAbiMax = peer_.nativeAbiMax;
        info.capabilityFlags = peer_.capabilityFlags;
        info.sessionId = header_ ? header_->sessionId : 0u;
        info.fresh = IsHostAlive(kRuntimeHostTimeoutMs);
        return info;
    }

    bool ReadTelemetry(RuntimeLinkTelemetryV2& out) {
        if (!header_ || !telemetry_) return false;
        volatile RuntimeDiscoveryHeaderV3* h = header_;
        const uint32_t seq = h->telemetrySequence;
        if ((seq & 1u) != 0u) {
            if (hasTelemetry_) out = lastTelemetry_;
            return false;
        }
        RuntimeLinkTelemetryV2 snapshot = *telemetry_;
        RLV2_MemBarrier();
        if (h->telemetrySequence != seq) {
            if (hasTelemetry_) out = lastTelemetry_;
            return false;
        }
        lastTelemetry_ = snapshot;
        hasTelemetry_ = true;
        out = snapshot;
        return true;
    }

    bool IsHostAlive(uint32_t timeoutMs) const {
        if (!header_ || qpcFreq_ == 0u) return false;
        const uint64_t heartbeat = header_->heartbeatQpc;
        if (heartbeat == 0u) return false;
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        const uint64_t now = static_cast<uint64_t>(qpc.QuadPart);
        const uint64_t age = now > heartbeat ? now - heartbeat : 0u;
        return (age * 1000u) / qpcFreq_ < timeoutMs;
    }

    RLResult SendCommand(RLCommandType type, uint32_t groupMask,
                         uint32_t param, const RuntimeLiveStateV2& live,
                         uint32_t timeoutMs,
                         char resultText[kRuntimeLinkResultTextCapacity] =
                             nullptr,
                         const char* requestText = nullptr) {
        if (!header_ || !command_ || !hMutex_) {
            if (resultText) strncpy_s(resultText,
                kRuntimeLinkResultTextCapacity, "not connected", _TRUNCATE);
            return RLResult::InternalError;
        }
        const size_t requestLength = requestText
            ? strnlen_s(requestText, kRuntimeLinkCommandTextCapacity) : 0u;
        if (requestText && requestLength >= kRuntimeLinkCommandTextCapacity) {
            if (resultText) strncpy_s(resultText,
                kRuntimeLinkResultTextCapacity, "command text is too long",
                _TRUNCATE);
            return RLResult::InvalidArgument;
        }
        const uint32_t request = requestCounter_++;
        if (WaitForSingleObject(hMutex_, kRuntimeLinkMutexTimeoutMs) !=
            WAIT_OBJECT_0) {
            if (resultText) strncpy_s(resultText,
                kRuntimeLinkResultTextCapacity, "command mutex timeout",
                _TRUNCATE);
            return RLResult::Busy;
        }

        command_->type = static_cast<uint32_t>(type);
        command_->groupMask = groupMask;
        command_->param = param;
        command_->live = live;
        std::memset(command_->resultText, 0, sizeof(command_->resultText));
        if (requestLength != 0u)
            std::memcpy(command_->resultText, requestText, requestLength);
        RLV2_MemBarrier();
        volatile RuntimeDiscoveryHeaderV3* h = header_;
        h->commandRequestToken = token_;
        RLV2_MemBarrier();
        h->commandRequestId = request;
        RLV2_MemBarrier();
        ReleaseMutex(hMutex_);
        if (hCmdEvent_) SetEvent(hCmdEvent_);

        const uint32_t start = GetTickCount();
        for (;;) {
            if (h->commandProcessedId == request &&
                h->commandProcessedToken == token_) {
                const RLResult result =
                    static_cast<RLResult>(h->commandResult);
                if (resultText) std::memcpy(resultText,
                    command_->resultText, kRuntimeLinkResultTextCapacity);
                return result;
            }
            if (static_cast<int32_t>(GetTickCount() - start) >=
                static_cast<int32_t>(timeoutMs)) break;
            Sleep(2);
        }
        if (resultText) strncpy_s(resultText,
            kRuntimeLinkResultTextCapacity, "no ACK within timeout",
            _TRUNCATE);
        return RLResult::Busy;
    }

    RLResult SendPing(uint32_t timeoutMs,
                      char resultText[kRuntimeLinkResultTextCapacity] =
                          nullptr) {
        return SendCommand(RLCommandType::Ping, 0u, 0u,
                           RuntimeLiveStateV2{}, timeoutMs, resultText);
    }

    static uint32_t EnumerateHosts(PeerInfo* out, uint32_t maxCount) {
        if (!out || maxCount == 0u) return 0u;
        wchar_t registryName[128], mutexName[128];
        RLV3_HostsRegName(registryName, 128);
        RLV3_HostsMutexName(mutexName, 128);
        HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, registryName);
        if (!map) return 0u;
        RuntimeDiscoveryHostsRegistryV1* registry =
            static_cast<RuntimeDiscoveryHostsRegistryV1*>(MapViewOfFile(
                map, FILE_MAP_READ, 0, 0, 0));
        HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                  mutexName);
        uint32_t count = 0u;
        if (registry && mutex &&
            WaitForSingleObject(mutex, kRuntimeLinkMutexTimeoutMs) ==
                WAIT_OBJECT_0) {
            if (registry->magic == kRuntimeDiscoveryRegistryMagic &&
                registry->version == kRuntimeDiscoveryRegistryVersion &&
                registry->totalSize >= sizeof(*registry) &&
                registry->slotCapacity == kRuntimeHostMaxCount &&
                registry->slotSize == sizeof(RuntimeDiscoveryHostSlotV1)) {
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                uint64_t frequency = 0u;
                QueryPerformanceFrequency(
                    reinterpret_cast<LARGE_INTEGER*>(&frequency));
                const uint64_t now = static_cast<uint64_t>(qpc.QuadPart);
                for (uint32_t i = 0u;
                     i < kRuntimeHostMaxCount && count < maxCount; ++i) {
                    const RuntimeDiscoveryHostSlotV1& slot =
                        registry->slots[i];
                    if (RLV3_HostsSlotIsEmpty(slot)) continue;
                    PeerInfo& info = out[count++];
                    info.pid = slot.pid;
                    info.archClass = slot.archClass;
                    info.productMajor = slot.productMajor;
                    info.productMinor = slot.productMinor;
                    info.productPatch = slot.productPatch;
                    info.buildNumber = slot.buildNumber;
                    info.protocolMax = slot.protocolMax;
                    info.nativeAbiMax = slot.nativeAbiMax;
                    info.capabilityFlags = slot.capabilityFlags;
                    info.sessionId = slot.sessionId;
                    info.fresh = RLV3_HostsSlotIsFresh(
                        slot, now, frequency, kRuntimeHostTimeoutMs);
                }
            }
            ReleaseMutex(mutex);
        }
        if (mutex) CloseHandle(mutex);
        if (registry) UnmapViewOfFile(registry);
        CloseHandle(map);
        return count;
    }

private:
    struct CapturedHeader {
        uint32_t magic = 0u;
        uint32_t headerVersion = 0u;
        uint32_t headerSize = 0u;
        uint32_t totalSize = 0u;
        uint32_t publisherPid = 0u;
        uint32_t archClass = 0u;
        uint32_t productMajor = 0u;
        uint32_t productMinor = 0u;
        uint32_t productPatch = 0u;
        uint32_t buildNumber = 0u;
        uint32_t releaseChannel = 0u;
        uint32_t protocolMin = 0u;
        uint32_t protocolMax = 0u;
        uint32_t nativeAbiMin = 0u;
        uint32_t nativeAbiMax = 0u;
        uint32_t telemetryOffset = 0u;
        uint32_t telemetrySize = 0u;
        uint32_t commandOffset = 0u;
        uint32_t commandSize = 0u;
        uint32_t accessFlags = 0u;
        uint64_t capabilityFlags = 0u;
        uint32_t headerCrc = 0u;
    } peer_{};

    bool ValidateHeader(uint32_t pid) {
        volatile RuntimeDiscoveryHeaderV3* h = header_;
        peer_.magic = h->magic;
        peer_.headerVersion = h->headerVersion;
        peer_.headerSize = h->headerSize;
        peer_.totalSize = h->totalSize;
        peer_.publisherPid = h->publisherPid;
        peer_.archClass = h->archClass;
        peer_.productMajor = h->productMajor;
        peer_.productMinor = h->productMinor;
        peer_.productPatch = h->productPatch;
        peer_.buildNumber = h->buildNumber;
        peer_.releaseChannel = h->releaseChannel;
        peer_.protocolMin = h->protocolMin;
        peer_.protocolMax = h->protocolMax;
        peer_.nativeAbiMin = h->nativeAbiMin;
        peer_.nativeAbiMax = h->nativeAbiMax;
        peer_.telemetryOffset = h->telemetryOffset;
        peer_.telemetrySize = h->telemetrySize;
        peer_.commandOffset = h->commandOffset;
        peer_.commandSize = h->commandSize;
        peer_.accessFlags = h->accessFlags;
        peer_.capabilityFlags = h->capabilityFlags;
        peer_.headerCrc = h->headerCrc;

        RuntimeDiscoveryHeaderV3 stable{};
        stable.magic = peer_.magic;
        stable.headerVersion = peer_.headerVersion;
        stable.headerSize = peer_.headerSize;
        stable.totalSize = peer_.totalSize;
        stable.publisherPid = peer_.publisherPid;
        stable.archClass = peer_.archClass;
        stable.productMajor = peer_.productMajor;
        stable.productMinor = peer_.productMinor;
        stable.productPatch = peer_.productPatch;
        stable.buildNumber = peer_.buildNumber;
        stable.releaseChannel = peer_.releaseChannel;
        stable.protocolMin = peer_.protocolMin;
        stable.protocolMax = peer_.protocolMax;
        stable.nativeAbiMin = peer_.nativeAbiMin;
        stable.nativeAbiMax = peer_.nativeAbiMax;
        stable.telemetryOffset = peer_.telemetryOffset;
        stable.telemetrySize = peer_.telemetrySize;
        stable.commandOffset = peer_.commandOffset;
        stable.commandSize = peer_.commandSize;
        stable.accessFlags = peer_.accessFlags;
        stable.capabilityFlags = peer_.capabilityFlags;

        if (peer_.magic != kRuntimeDiscoveryMagic ||
            peer_.headerVersion != kRuntimeDiscoveryHeaderVersion ||
            peer_.headerSize < sizeof(RuntimeDiscoveryHeaderV3) ||
            peer_.totalSize < peer_.headerSize ||
            peer_.publisherPid != pid || pid == 0u ||
            peer_.protocolMin > kRuntimeLinkVersionV3 ||
            peer_.protocolMax < kRuntimeLinkVersionV3 ||
            peer_.telemetrySize < sizeof(RuntimeLinkTelemetryV2) ||
            peer_.commandSize < sizeof(RuntimeLinkCommandV2) ||
            (peer_.accessFlags & (kRuntimeAccessTelemetryRead |
                                  kRuntimeAccessCommandWrite)) !=
                (kRuntimeAccessTelemetryRead | kRuntimeAccessCommandWrite) ||
            (peer_.capabilityFlags & build::CapabilityRuntimeLinkV3) == 0u)
            return false;
        if (peer_.telemetryOffset < peer_.headerSize ||
            peer_.telemetryOffset + peer_.telemetrySize > peer_.totalSize ||
            peer_.commandOffset < peer_.headerSize ||
            peer_.commandOffset + peer_.commandSize > peer_.totalSize)
            return false;
        return RLV3_HeaderCrc(stable) == peer_.headerCrc;
    }

    HANDLE hMap_ = nullptr;
    HANDLE hMutex_ = nullptr;
    HANDLE hCmdEvent_ = nullptr;
    uint8_t* mapping_ = nullptr;
    RuntimeDiscoveryHeaderV3* header_ = nullptr;
    RuntimeLinkTelemetryV2* telemetry_ = nullptr;
    RuntimeLinkCommandV2* command_ = nullptr;
    uint32_t pid_ = 0u;
    uint64_t qpcFreq_ = 0u;
    uint32_t token_ = 0u;
    uint32_t requestCounter_ = 0u;
    RuntimeLinkTelemetryV2 lastTelemetry_{};
    bool hasTelemetry_ = false;
};

// Negotiating client used by current tools.  V3 discovery is attempted first;
// V2 remains a complete fallback so an updated configurator can still manage
// older drivers.  Callers never need to select a wire layout themselves.
class RuntimeLinkClient {
public:
    enum class Protocol : uint32_t { None = 0u, V2 = 2u, V3 = 3u };

    struct HostInfo {
        uint32_t pid = 0u;
        uint32_t archClass = 0u;
        uint32_t productMajor = 0u;
        uint32_t productMinor = 0u;
        uint32_t productPatch = 0u;
        uint32_t buildNumber = 0u;
        uint32_t releaseChannel = 0u;
        uint32_t protocolMin = 0u;
        uint32_t protocolMax = 0u;
        uint64_t capabilityFlags = 0u;
        uint64_t sessionId = 0u;
        bool fresh = false;
        bool hasVersionIdentity = false;
    };

    bool Open(uint32_t pid) {
        Close();
        if (v3_.Open(pid)) {
            protocol_ = Protocol::V3;
            peer_ = FromV3(v3_.GetPeerInfo());
            return true;
        }
        if (v2_.Open(pid)) {
            protocol_ = Protocol::V2;
            peer_.pid = pid;
            peer_.protocolMin = kRuntimeLinkVersion;
            peer_.protocolMax = kRuntimeLinkVersion;
            peer_.capabilityFlags = build::CapabilityRuntimeLinkV2 |
                build::CapabilityLiveConfiguration |
                build::CapabilityTelemetry;
            peer_.fresh = true;
            return true;
        }
        return false;
    }

    void Close() {
        v3_.Close();
        v2_.Close();
        protocol_ = Protocol::None;
        peer_ = HostInfo{};
    }

    bool IsOpen() const { return protocol_ != Protocol::None; }
    uint32_t GetPID() const { return peer_.pid; }
    Protocol GetProtocol() const { return protocol_; }
    const HostInfo& GetPeerInfo() const { return peer_; }
    bool HasCapability(uint64_t capability) const {
        return (peer_.capabilityFlags & capability) == capability;
    }

    bool ReadTelemetry(RuntimeLinkTelemetryV2& out) {
        return protocol_ == Protocol::V3 ? v3_.ReadTelemetry(out) :
               protocol_ == Protocol::V2 ? v2_.ReadTelemetry(out) : false;
    }

    bool IsHostAlive(uint32_t timeoutMs) const {
        return protocol_ == Protocol::V3 ? v3_.IsHostAlive(timeoutMs) :
               protocol_ == Protocol::V2 ? v2_.IsHostAlive(timeoutMs) : false;
    }

    RLResult SendCommand(RLCommandType type, uint32_t groupMask,
                         uint32_t param, const RuntimeLiveStateV2& live,
                         uint32_t timeoutMs,
                         char resultText[kRuntimeLinkResultTextCapacity] =
                             nullptr,
                         const char* requestText = nullptr) {
        if (protocol_ == Protocol::V3)
            return v3_.SendCommand(type, groupMask, param, live, timeoutMs,
                                   resultText, requestText);
        if (protocol_ == Protocol::V2)
            return v2_.SendCommand(type, groupMask, param, live, timeoutMs,
                                   resultText, requestText);
        if (resultText) strncpy_s(resultText,
            kRuntimeLinkResultTextCapacity, "not connected", _TRUNCATE);
        return RLResult::InternalError;
    }

    static uint32_t EnumerateHosts(HostInfo* out, uint32_t maxCount) {
        if (!out || maxCount == 0u) return 0u;
        RuntimeLinkClientV3::PeerInfo modern[kRuntimeHostMaxCount]{};
        const uint32_t modernCount = RuntimeLinkClientV3::EnumerateHosts(
            modern, kRuntimeHostMaxCount);
        uint32_t count = 0u;
        for (uint32_t i = 0u; i < modernCount && count < maxCount; ++i)
            out[count++] = FromV3(modern[i]);

        RuntimeLinkClientV2::HostInfo legacy[kRuntimeHostMaxCount]{};
        const uint32_t legacyCount = RuntimeLinkClientV2::EnumerateHosts(
            legacy, kRuntimeHostMaxCount);
        for (uint32_t i = 0u; i < legacyCount && count < maxCount; ++i) {
            bool duplicate = false;
            for (uint32_t j = 0u; j < count; ++j)
                duplicate |= out[j].pid == legacy[i].pid;
            if (duplicate) continue;
            HostInfo info{};
            info.pid = legacy[i].pid;
            info.archClass = legacy[i].archClass;
            info.protocolMin = kRuntimeLinkVersion;
            info.protocolMax = kRuntimeLinkVersion;
            info.capabilityFlags = build::CapabilityRuntimeLinkV2 |
                build::CapabilityLiveConfiguration |
                build::CapabilityTelemetry;
            info.sessionId = legacy[i].sessionId;
            info.fresh = legacy[i].fresh;
            out[count++] = info;
        }
        return count;
    }

private:
    static HostInfo FromV3(const RuntimeLinkClientV3::PeerInfo& source) {
        HostInfo info{};
        info.pid = source.pid;
        info.archClass = source.archClass;
        info.productMajor = source.productMajor;
        info.productMinor = source.productMinor;
        info.productPatch = source.productPatch;
        info.buildNumber = source.buildNumber;
        info.releaseChannel = source.releaseChannel;
        info.protocolMin = source.protocolMin;
        info.protocolMax = source.protocolMax;
        info.capabilityFlags = source.capabilityFlags;
        info.sessionId = source.sessionId;
        info.fresh = source.fresh;
        info.hasVersionIdentity = true;
        return info;
    }

    RuntimeLinkClientV3 v3_{};
    RuntimeLinkClientV2 v2_{};
    Protocol protocol_ = Protocol::None;
    HostInfo peer_{};
};

} // namespace svms

#endif // !defined(SVMS_XP_COMPAT)

#endif // SVMS_RUNTIME_LINK_H
