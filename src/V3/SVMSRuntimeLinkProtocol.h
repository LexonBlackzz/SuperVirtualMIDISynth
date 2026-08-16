#ifndef SVMS_RUNTIME_LINK_PROTOCOL_H
#define SVMS_RUNTIME_LINK_PROTOCOL_H

#include <cstdint>
#include <cstring>
#include <atomic>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#endif

namespace svms {

// ─── Naming conventions ─────────────────────────────────────────────────────
//
//   Shared memory:  Local\SVMS_V3_RuntimeLink_v1_<PID>
//   Mutex:          Local\SVMS_V3_RL_Mutex_<PID>
//   Command event:  Local\SVMS_V3_RL_CmdEvent_<PID>
//
// The driver creates these objects when RuntimeLink is initialized.
// The configurator discovers them by PID (passed via --runtime-link <PID>
// or enumerated from the driver process).

#ifdef _WIN32
inline const wchar_t* RL_SharedMemName(uint32_t pid, wchar_t* buf, size_t bufLen) {
    _snwprintf_s(buf, bufLen, _TRUNCATE, L"Local\\SVMS_V3_RuntimeLink_v1_%u", pid);
    return buf;
}

inline const wchar_t* RL_MutexName(uint32_t pid, wchar_t* buf, size_t bufLen) {
    _snwprintf_s(buf, bufLen, _TRUNCATE, L"Local\\SVMS_V3_RL_Mutex_%u", pid);
    return buf;
}

inline const wchar_t* RL_CmdEventName(uint32_t pid, wchar_t* buf, size_t bufLen) {
    _snwprintf_s(buf, bufLen, _TRUNCATE, L"Local\\SVMS_V3_RL_CmdEvent_%u", pid);
    return buf;
}
#endif

// ─── Protocol constants ─────────────────────────────────────────────────────

inline constexpr uint32_t kRuntimeLinkMagic    = 0x53524C56u;  // "SRLV"
inline constexpr uint32_t kRuntimeLinkVersion  = 1;
inline constexpr uint32_t kRuntimeLinkCmdRingCapacity = 256;

// ─── Command types ──────────────────────────────────────────────────────────
//
// LIVE parameters can be applied by the control thread without restart.
// RESTART parameters are acknowledged but require a full re-init.

enum class RLCommandType : uint32_t {
    // LIVE (applied immediately by control thread)
    Ping                = 0x00000000,
    SetMasterVolume     = 0x00000001,
    SetReverbEnabled    = 0x00000002,
    SetReverbMix        = 0x00000003,
    SetReverbRoomSize   = 0x00000004,
    SetReverbDecay      = 0x00000005,
    SetReverbDamping    = 0x00000006,
    SetReverbWidth      = 0x00000007,
    SetReverbDiffusion  = 0x00000008,
    SetReverbPreDelayMs = 0x00000009,
    SetReverbEarlyLevel = 0x0000000A,
    SetReverbLateLevel  = 0x0000000B,
    SetReverbModDepth   = 0x0000000C,
    SetReverbModRate    = 0x0000000D,
    SetReverbLowCutHz   = 0x0000000E,
    SetReverbHighCutHz  = 0x0000000F,
    SetLimiterEnabled   = 0x00000010,
    SetLimiterThreshold = 0x00000011,
    SetLimiterLookahead = 0x00000012,
    SetLimiterAttack    = 0x00000013,
    SetLimiterRelease   = 0x00000014,
    SetCorrectnessMode  = 0x00000015,

    // RESTART (acknowledged, but requires driver restart)
    RequestRestart      = 0x00000100,

    // Sentinel
    Invalid             = 0xFFFFFFFF,
};

// ─── Command entry (16 bytes, cache-line friendly) ─────────────────────────

struct alignas(16) RLCommand {
    RLCommandType type   = RLCommandType::Invalid;
    uint32_t      param0 = 0;
    uint32_t      param1 = 0;
    float         value0 = 0.0f;
};

static_assert(sizeof(RLCommand) == 16, "RLCommand must be 16 bytes");

// ─── Telemetry snapshot (written by control thread, read by configurator) ───
//
// All fields are plain-old-data. The control thread copies from the
// process-local double-buffer into the shared-memory slot, then
// atomically publishes the write index.

struct alignas(64) RLTelemetry {
    // Voice pool
    uint32_t activeVoices        = 0;
    uint32_t maxVoices           = 0;
    uint32_t releasingVoices     = 0;
    uint32_t freeTop             = 0;
    uint32_t voiceSteals         = 0;
    uint32_t retiredCount        = 0;
    uint32_t retiredImmediateCount = 0;
    uint32_t decimationStep      = 1;

    // Audio state
    uint32_t sampleRate          = 44100;
    uint32_t bufferFrames        = 2048;
    float    renderPeak          = 0.0f;
    float    masterVolume        = 1.0f;
    uint32_t audioRunning        = 0;
    uint32_t soundFontLoaded     = 0;
    int32_t  audioHResult        = 0;

    // CPU / performance
    float    cpuLoadPercent      = 0.0f;
    float    callbackP95         = 0.0f;
    float    callbackP99         = 0.0f;
    float    callbackP999        = 0.0f;
    uint64_t overBudgetCallbacks = 0;
    uint32_t maxConsecutiveOverBudget = 0;

    // Event pipeline (subset — full EventTelemetry is too large for IPC)
    uint64_t eventsSubmitted     = 0;
    uint64_t eventsAccepted      = 0;
    uint64_t eventsDropped       = 0;
    uint64_t eventsDispatched    = 0;

    // Limiter
    float    limiterGainReduction = 0.0f;

    // Reverb live params (for configurator to read back)
    float    reverbMix           = 0.25f;
    float    reverbRoomSize      = 0.60f;
    float    reverbDecay         = 0.50f;
    float    reverbDamping       = 0.35f;
    float    reverbWidth         = 1.0f;
    float    reverbDiffusion     = 0.70f;
    float    reverbPreDelayMs    = 12.0f;
    float    reverbEarlyLevel    = 0.35f;
    float    reverbLateLevel     = 0.85f;
    float    reverbModDepth      = 0.30f;
    float    reverbModRate       = 0.35f;
    float    reverbLowCutHz      = 70.0f;
    float    reverbHighCutHz     = 16000.0f;

    // Limiter live params (for configurator to read back)
    uint32_t limiterEnabled      = 1;
    float    limiterThreshold    = 0.95f;
    float    limiterLookaheadMs  = 3.0f;
    float    limiterAttackMs     = 0.5f;
    float    limiterReleaseMs    = 100.0f;

    // Correctness mode
    uint32_t correctnessMode     = 1;

    // Timestamp (QPC of last publish)
    uint64_t timestampQPC        = 0;

    // Reserved for future use
    uint32_t reserved[3]         = {};
};

static_assert(sizeof(RLTelemetry) % 64 == 0,
              "RLTelemetry must be cache-line aligned");

// ─── Shared memory layout ───────────────────────────────────────────────────
//
// The shared memory region is laid out as:
//
//   [RuntimeLinkHeader]              (64 bytes, cache-line aligned)
//   [RLTelemetry slot 0]             (cache-line aligned)
//   [RLTelemetry slot 1]             (cache-line aligned)
//   [RLCommand ring buffer]          (kCmdRingCapacity entries × 16 bytes)
//
// Total: 64 + 2*sizeof(RLTelemetry) + 256*16 = 64 + 2*N + 4096 bytes
// With RLTelemetry padded to 64-byte alignment, this is deterministic.

struct alignas(64) RuntimeLinkHeader {
    uint32_t magic               = kRuntimeLinkMagic;
    uint32_t version             = kRuntimeLinkVersion;
    uint32_t size                = 0;  // filled at creation: total mapping size
    uint32_t flags               = 0;

    // Telemetry double-buffer index (0 or 1).
    // Audio/control thread writes to slot[index^1], then stores index^1.
    // Reader loads with acquire semantics.
    std::atomic<uint32_t> telemetryWriteIndex{0};
    uint32_t telemetryPadding = 0;

    // Command ring (SPSC: configurator writes tail, driver reads head).
    // Both are monotonically increasing indices; slot = index % capacity.
    std::atomic<uint32_t> cmdTail{0};  // written by configurator
    uint32_t cmdHead            = 0;   // written by driver control thread
    uint32_t cmdCapacity        = kRuntimeLinkCmdRingCapacity;
    uint32_t cmdPadding         = 0;

    uint8_t  reserved[16]       = {};
};

static_assert(sizeof(RuntimeLinkHeader) == 64,
              "RuntimeLinkHeader must be exactly one cache line");

// Full shared memory mapping
struct RuntimeLinkSharedMemory {
    RuntimeLinkHeader header;
    RLTelemetry       telemetry[2];
    RLCommand         cmdRing[kRuntimeLinkCmdRingCapacity];
};

// Helper to compute the required mapping size
inline constexpr size_t RuntimeLinkMappingSize() {
    return sizeof(RuntimeLinkSharedMemory);
}

// ─── Command ring helpers (lock-free SPSC) ──────────────────────────────────
//
// The configurator calls PushCommand under the mutex. The driver's control
// thread calls PopCommand from its own thread.

inline bool RL_PushCommand(RuntimeLinkSharedMemory* mem,
                           const RLCommand& cmd) {
    auto& h = mem->header;
    uint32_t tail = h.cmdTail.load(std::memory_order_relaxed);
    uint32_t next = tail + 1;
    // Full when next - head > capacity (unsigned subtraction handles wrap).
    // One slot is reserved to distinguish full from empty.
    if (next - h.cmdHead > h.cmdCapacity) return false;
    mem->cmdRing[tail % h.cmdCapacity] = cmd;
    h.cmdTail.store(next, std::memory_order_release);
    return true;
}

inline bool RL_PopCommand(const RuntimeLinkSharedMemory* mem,
                          RLCommand& cmd) {
    auto& h = const_cast<RuntimeLinkHeader&>(mem->header);
    uint32_t head = h.cmdHead;
    if (head == h.cmdTail.load(std::memory_order_acquire)) return false;
    cmd = mem->cmdRing[head % h.cmdCapacity];
    h.cmdHead = head + 1;
    return true;
}

// ─── Telemetry read helpers ─────────────────────────────────────────────────

inline const RLTelemetry& RL_ReadTelemetry(const RuntimeLinkSharedMemory* mem) {
    uint32_t idx = mem->header.telemetryWriteIndex.load(std::memory_order_acquire);
    return mem->telemetry[idx & 1];
}

} // namespace svms

#endif // SVMS_RUNTIME_LINK_PROTOCOL_H
