#ifndef SVMS_RUNTIME_LINK_PROTOCOL_H
#define SVMS_RUNTIME_LINK_PROTOCOL_H
//
// SVMS RuntimeLink Protocol — V2
// =============================
//
// Cross-process IPC between the SVMS V3 driver (winmm.dll) and the
// V3 Configurator.  V2 replaces the V1 double-buffered telemetry +
// 256-entry command-ring protocol.  The V1 protocol was unsafe for IPC:
// it placed std::atomic members inside the mapped structure, opened the
// mapping FILE_MAP_READ and const_cast'd it to write commands, kept two
// disjoint atomics (pointers/index) that could tear on 32-bit builds,
// wrote telemetry from the audio thread, and made no distinction between
// live and restart-only parameters.
//
// V2 design rules (enforced by static_asserts below):
//   1. Everything mapped into shared memory is plain-old-data:
//      uint32_t / int32_t / float / fixed char arrays only.
//      No std::atomic, no size_t, no bool, no pointers, no HANDLEs,
//      no strings, no containers inside the mapping.
//   2. Synchronization fields are single 32-bit words.  Aligned 32-bit
//      loads/stores are atomic on x86/x64, and the processor provides
//      TSO ordering, so a plain volatile store followed by the commit
//      word (requestId / telemetrySequence) is a valid release pattern
//      for these targets.  RLV2_MemBarrier() emits a compiler fence;
//      the hardware guarantees visibility order.
//   3. The named mutex EXISTS ONLY to serialize cross-process COPIES
//      (configurator → command slot).  No code on the audio thread and
//      no code on the driver control thread ever waits on the mutex.
//   4. Telemetry is a single stable slot guarded by an odd/even
//      telemetrySequence: the writer stores odd → writes the slot →
//      stores even.  Readers skip (or keep their last good copy) while
//      the sequence is odd or changed mid-copy.  No double buffer, no
//      atomic index swaps.
//   5. Commands use a single mailbox with a requestId/processedId
//      commit marker plus a per-client request token.  The client
//      writes the payload, then the requestId (+ token); the driver
//      detects new work when (requestId, token) differs from the last
//      processed pair, and only then reads the payload.  A client's
//      ACK condition is processedId == requestId AND processedToken
//      == its token, so two clients can never falsely observe each
//      other's results.
//   6. The audio thread never touches shared memory.  It only updates
//      a process-local RuntimeAudioSnapshot (odd/even sequence, floats
//      via AtomicFloatBits).  The driver control thread reads that
//      snapshot and publishes the shared-memory telemetry at ~30 Hz.
//   7. Discovery uses a fixed, well-known hosts registry mapping
//      (Local\SVMS_V3_RuntimeHosts_v2) with per-slot heartbeats, so
//      the configurator can enumerate running drivers without scanning
//      the process table.

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <cmath>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#endif

namespace svms {

// ─── Naming conventions ─────────────────────────────────────────────────────
//
//   Shared memory:  Local\SVMS_V3_RuntimeLink_v2_<PID>
//   Mutex:          Local\SVMS_V3_RuntimeMutex_v2_<PID>
//   Command event:  Local\SVMS_V3_RuntimeCommand_v2_<PID>   (auto-reset)
//   Host registry:  Local\SVMS_V3_RuntimeHosts_v2
//   Hosts mutex:    Local\SVMS_V3_RuntimeHostsMutex_v2
//
// The driver creates the per-PID objects on Initialize().  The
// configurator discovers drivers via the hosts registry, then opens the
// per-PID objects by process id (also passable via --runtime-link <PID>).

#ifdef _WIN32
inline const wchar_t* RLV2_SharedMemName(uint32_t pid, wchar_t* buf, size_t bufLen) {
    _snwprintf_s(buf, bufLen, _TRUNCATE, L"Local\\SVMS_V3_RuntimeLink_v2_%u", pid);
    return buf;
}

inline const wchar_t* RLV2_MutexName(uint32_t pid, wchar_t* buf, size_t bufLen) {
    _snwprintf_s(buf, bufLen, _TRUNCATE, L"Local\\SVMS_V3_RuntimeMutex_v2_%u", pid);
    return buf;
}

inline const wchar_t* RLV2_CmdEventName(uint32_t pid, wchar_t* buf, size_t bufLen) {
    _snwprintf_s(buf, bufLen, _TRUNCATE, L"Local\\SVMS_V3_RuntimeCommand_v2_%u", pid);
    return buf;
}

inline const wchar_t* RLV2_HostsRegName(wchar_t* buf, size_t bufLen) {
    _snwprintf_s(buf, bufLen, _TRUNCATE, L"Local\\SVMS_V3_RuntimeHosts_v2");
    return buf;
}

inline const wchar_t* RLV2_HostsMutexName(wchar_t* buf, size_t bufLen) {
    _snwprintf_s(buf, bufLen, _TRUNCATE, L"Local\\SVMS_V3_RuntimeHostsMutex_v2");
    return buf;
}
#endif

// ─── Protocol constants ─────────────────────────────────────────────────────

inline constexpr uint32_t kRuntimeLinkMagic    = 0x53524C32u;  // "SRL2"
inline constexpr uint32_t kRuntimeLinkVersion  = 2;
inline constexpr uint32_t kRuntimeLinkArchX86  = 0;
inline constexpr uint32_t kRuntimeLinkArchX64  = 1;
inline constexpr uint32_t kRuntimeHostMaxCount = 16;
inline constexpr uint32_t kRuntimeHostTimeoutMs = 5000u;   // stale after 5 s
inline constexpr uint32_t kRuntimeLinkMutexTimeoutMs = 1000u;
inline constexpr uint32_t kRuntimeLinkResultTextCapacity = 256;
inline constexpr uint32_t kRuntimeLinkDefaultCommandTimeoutMs = 400u;
inline constexpr uint32_t kRuntimeLinkPublishIntervalMs = 33u;  // ~30 Hz

inline constexpr uint32_t RLV2_PadTo64(uint32_t n) { return (n + 63u) & ~63u; }

// Compiler fence.  Ordering on x86/x64 is provided by the TSO memory
// model; this prevents the compiler from reordering the adjacent
// volatile accesses.
inline void RLV2_MemBarrier() {
#ifdef _MSC_VER
    _ReadWriteBarrier();
#else
    __asm__ volatile("" ::: "memory");
#endif
}

// ─── Result codes ───────────────────────────────────────────────────────────

enum class RLResult : uint32_t {
    Ok              = 0,
    InvalidArgument = 1,
    Unsupported     = 2,
    Busy            = 3,
    RestartRequired = 4,
    LoadFailed      = 5,
    InternalError   = 6,
};

inline const char* RLV2_ResultToString(RLResult r) {
    switch (r) {
        case RLResult::Ok:              return "OK";
        case RLResult::InvalidArgument: return "Invalid argument";
        case RLResult::Unsupported:     return "Unsupported";
        case RLResult::Busy:            return "Busy";
        case RLResult::RestartRequired: return "Restart required";
        case RLResult::LoadFailed:      return "Load failed";
        case RLResult::InternalError:   return "Internal error";
    }
    return "Unknown";
}

// ─── Command types ──────────────────────────────────────────────────────────
//
// LIVE parameter changes are routed through the single ApplyLiveConfig
// command, which carries the complete RuntimeLiveStateV2 payload and a
// groupMask describing which groups the driver must apply.  This is one
// command per UI change (not one command per knob).  The Set* values in
// the 0x01..0x1F range are kept for configurator-internal routing (which
// widget mutated the working state) and map to groups via
// RLV2_GroupForType(); they never cross the process boundary.

enum class RLCommandType : uint32_t {
    NoCommand            = 0x00000000,

    // Live-field ids (configurator-internal routing; grouped on the wire)
    SetMasterVolume      = 0x00000001,
    SetReverbEnabled     = 0x00000002,
    SetReverbMix         = 0x00000003,
    SetReverbRoomSize    = 0x00000004,
    SetReverbDecay       = 0x00000005,
    SetReverbDamping     = 0x00000006,
    SetReverbWidth       = 0x00000007,
    SetReverbDiffusion   = 0x00000008,
    SetReverbPreDelayMs  = 0x00000009,
    SetReverbEarlyLevel  = 0x0000000A,
    SetReverbLateLevel   = 0x0000000B,
    SetReverbModDepth    = 0x0000000C,
    SetReverbModRate     = 0x0000000D,
    SetReverbLowCutHz    = 0x0000000E,
    SetReverbHighCutHz   = 0x0000000F,
    SetCorrectnessMode   = 0x00000010,
    SetLimiterEnabled    = 0x00000011,
    SetLimiterThreshold  = 0x00000012,
    SetLimiterLookahead  = 0x00000013,
    SetLimiterAttack     = 0x00000014,
    SetLimiterRelease    = 0x00000015,

    // Wire commands (cross the process boundary)
    Ping                 = 0x00000020,
    ApplyLiveConfig      = 0x00000100,
    ReloadSoundFont      = 0x00000101,
    ResetVoices          = 0x00000102,
    RequestRestart       = 0x00000110,

    Invalid              = 0xFFFFFFFF,
};

// Live-parameter groups.  The driver applies whole groups per command.
inline constexpr uint32_t RLGroupMaster      = 0x00000001u;
inline constexpr uint32_t RLGroupCorrectness = 0x00000002u;
inline constexpr uint32_t RLGroupReverb      = 0x00000004u;
inline constexpr uint32_t RLGroupLimiter     = 0x00000008u;
inline constexpr uint32_t RLGroupAll =
    RLGroupMaster | RLGroupCorrectness | RLGroupReverb | RLGroupLimiter;

inline uint32_t RLV2_GroupForType(RLCommandType type) {
    switch (type) {
        case RLCommandType::SetMasterVolume:     return RLGroupMaster;
        case RLCommandType::SetReverbEnabled:
        case RLCommandType::SetReverbMix:
        case RLCommandType::SetReverbRoomSize:
        case RLCommandType::SetReverbDecay:
        case RLCommandType::SetReverbDamping:
        case RLCommandType::SetReverbWidth:
        case RLCommandType::SetReverbDiffusion:
        case RLCommandType::SetReverbPreDelayMs:
        case RLCommandType::SetReverbEarlyLevel:
        case RLCommandType::SetReverbLateLevel:
        case RLCommandType::SetReverbModDepth:
        case RLCommandType::SetReverbModRate:
        case RLCommandType::SetReverbLowCutHz:
        case RLCommandType::SetReverbHighCutHz:  return RLGroupReverb;
        case RLCommandType::SetCorrectnessMode:  return RLGroupCorrectness;
        case RLCommandType::SetLimiterEnabled:
        case RLCommandType::SetLimiterThreshold:
        case RLCommandType::SetLimiterLookahead:
        case RLCommandType::SetLimiterAttack:
        case RLCommandType::SetLimiterRelease:   return RLGroupLimiter;
        default:                                 return 0u;
    }
}

// ─── AtomicFloatBits ────────────────────────────────────────────────────────
//
// A float transported in a single 32-bit word where bit 0 is reserved as
// a write stamp.  Storing alternates the float payload in bits[31..1]
// and pins the stamp; a reader that observes a stamp different from the
// stamp it expects knows the value was written by a *different* publish
// epoch (or is mid-tearing on a hypothetical non-atomic platform).  On
// x86/x64 the aligned 32-bit access is atomic, so the stamp is a
// defense-in-depth marker used together with the enclosing odd/even
// sequence guards.

inline uint32_t RLV2_FloatBits(float v) {
    static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32-bit");
    uint32_t b;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

inline float RLV2_BitsToFloat(uint32_t b) {
    float v;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

struct AtomicFloatBits {
    uint32_t bits = 0;

    void Store(float v, uint32_t stamp) {
        const uint32_t b = RLV2_FloatBits(v);
        bits = (b & ~1u) | (stamp & 1u);
    }

    // Returns the value if (and only if) the nested stamp matches the
    // expected stamp; otherwise leaves out untouched and returns false.
    bool TryLoad(float& out, uint32_t expectedStamp) const {
        const uint32_t b = bits;
        if ((b & 1u) != (expectedStamp & 1u)) return false;
        out = RLV2_BitsToFloat(b & ~1u);
        return true;
    }

    // Unsafe fast read — only correct when the enclosing sequence guard
    // (odd/even) already proves the whole snapshot is settled.
    float LoadUnchecked() const {
        return RLV2_BitsToFloat(bits & ~1u);
    }
};

static_assert(std::is_trivially_copyable<AtomicFloatBits>::value,
              "AtomicFloatBits must be trivially copyable");
static_assert(sizeof(AtomicFloatBits) == 4, "AtomicFloatBits must be 4 bytes");

// ─── Live-parameter payload ─────────────────────────────────────────────────
//
// The complete set of live-tweakable parameters.  Full group payloads
// travel in ApplyLiveConfig commands and are echoed back in the
// telemetry's `live` field so the configurator can display the RUNTIME
// state (as opposed to the SAVED or WORKING state it owns locally).

struct RuntimeLiveStateV2 {
    uint32_t correctnessMode   = 0;
    uint32_t reverbEnabled     = 0;
    uint32_t limiterEnabled    = 1;

    float masterVolume         = 1.0f;   // 0..4 (contract honored end-to-end)

    float reverbMix            = 0.25f;
    float reverbRoomSize       = 0.60f;
    float reverbDecay          = 0.50f;
    float reverbDamping        = 0.35f;
    float reverbWidth          = 1.0f;
    float reverbDiffusion      = 0.70f;
    float reverbPreDelayMs     = 12.0f;
    float reverbEarlyLevel     = 0.35f;
    float reverbLateLevel      = 0.85f;
    float reverbModDepth       = 0.30f;
    float reverbModRate        = 0.35f;
    float reverbLowCutHz       = 70.0f;
    float reverbHighCutHz      = 16000.0f;

    float limiterThreshold     = 0.95f;
    float limiterLookaheadMs   = 3.0f;
    float limiterAttackMs      = 0.5f;
    float limiterReleaseMs     = 100.0f;

    uint32_t reserved[2]       = {};
};

static_assert(std::is_trivially_copyable<RuntimeLiveStateV2>::value,
              "RuntimeLiveStateV2 must be trivially copyable");
static_assert(alignof(RuntimeLiveStateV2) == 4,
              "RuntimeLiveStateV2 must be 4-byte aligned");

inline bool RLV2_IsFinite(float v) { return std::isfinite(v); }

// ─── Telemetry snapshot ─────────────────────────────────────────────────────
//
// Single stable slot.  Written by the driver's control thread under the
// odd/even telemetrySequence guard in the header; read by any number of
// configurators with a skip-if-busy pattern.  Percentile fields carry
// PERCENT OF CALLBACK BUDGET (P95 = 48 means the P95 callback consumed
// 48% of its budget), not raw milliseconds.

struct alignas(64) RuntimeLinkTelemetryV2 {
    uint64_t timestampQpc            = 0;   // QPC of publish (same clock as heartbeatQpc)
    uint64_t overBudgetCallbacks     = 0;
    uint64_t eventsSubmitted         = 0;
    uint64_t eventsAccepted          = 0;
    uint64_t eventsDropped           = 0;
    uint64_t eventsDispatched        = 0;

    uint32_t activeVoices            = 0;
    uint32_t maxVoices               = 0;
    uint32_t releasingVoices         = 0;
    uint32_t freeTop                 = 0;
    uint32_t voiceSteals             = 0;
    uint32_t retiredCount            = 0;
    uint32_t retiredImmediateCount   = 0;
    uint32_t decimationStep          = 1;

    uint32_t sampleRate              = 44100;
    uint32_t bufferFrames            = 2048;
    uint32_t audioRunning            = 0;
    uint32_t soundFontLoaded         = 0;
    int32_t  audioHResult            = 0;

    float    renderPeak              = 0.0f;
    float    cpuLoadPercent          = 0.0f;   // smoothed
    float    callbackP95Percent      = 0.0f;   // percent of budget
    float    callbackP99Percent      = 0.0f;
    float    callbackP999Percent     = 0.0f;
    uint32_t maxConsecutiveOverBudget = 0;

    // Limiter meters — measured in the audio loop, published from the
    // snapshot.  Gain reduction is POSITIVE dB (0 = no reduction).
    float    limiterInputPeakL       = 0.0f;
    float    limiterInputPeakR       = 0.0f;
    float    limiterOutputPeakL      = 0.0f;
    float    limiterOutputPeakR      = 0.0f;
    float    limiterGainReductionDb  = 0.0f;

    // Live-state echo (what the driver actually applied)
    RuntimeLiveStateV2 live;

    char     soundFontName[256]      = {};

    uint32_t reserved[5]             = {};
};

static_assert(std::is_trivially_copyable<RuntimeLinkTelemetryV2>::value,
              "RuntimeLinkTelemetryV2 must be trivially copyable");
static_assert(sizeof(RuntimeLinkTelemetryV2) % 64 == 0,
              "RuntimeLinkTelemetryV2 must be a multiple of one cache line");

// ─── Command mailbox ────────────────────────────────────────────────────────
//
// Single command slot.  Client-writable fields: type, groupMask, param,
// live.  Driver-writable field: resultText (best-effort detail for the
// last ACKed command).  The commit markers (commandRequestId +
// commandRequestToken) live in the header and are written LAST by the
// client; the driver only reads the payload after those markers change.

struct alignas(64) RuntimeLinkCommandV2 {
    uint32_t type           = 0;    // RLCommandType
    uint32_t groupMask      = 0;
    uint32_t param          = 0;    // command-specific int argument
    uint32_t reserved0      = 0;

    RuntimeLiveStateV2 live;        // ApplyLiveConfig payload

    char resultText[kRuntimeLinkResultTextCapacity] = {};

    uint32_t reserved1[23]  = {};
};

static_assert(std::is_trivially_copyable<RuntimeLinkCommandV2>::value,
              "RuntimeLinkCommandV2 must be trivially copyable");
static_assert(sizeof(RuntimeLinkCommandV2) % 64 == 0,
              "RuntimeLinkCommandV2 must be a multiple of one cache line");

// ─── Header (one cache line) ────────────────────────────────────────────────
//
// All synchronization words are single 32-bit fields accessed through
// volatile qualified pointers.  Stable identity fields (magic, version,
// size, publisherPid, structSize, archClass) are covered by headerCrc.

struct alignas(64) RuntimeLinkHeaderV2 {
    // Liveness (must stay first: 8-byte field, keeps the struct to one
    // 64-byte cache line)
    uint64_t heartbeatQpc  = 0;    // updated every publish / command

    // Stable identity (covered by headerCrc)
    uint32_t magic          = kRuntimeLinkMagic;
    uint32_t version        = kRuntimeLinkVersion;
    uint32_t size           = 0;    // total mapping size, set at creation
    uint32_t publisherPid   = 0;    // PID of the driver process
    uint32_t structSize     = 0;    // sizeof(RuntimeLinkTelemetryV2)
    uint32_t archClass      = 0;    // kRuntimeLinkArchX86 / X64
    uint32_t headerCrc      = 0;    // FNV-1a over magic..archClass

    // Telemetry publish guard: odd = writer inside the slot, even = stable
    uint32_t telemetrySequence = 0;

    // Command commit markers
    uint32_t commandRequestId    = 0;  // client → driver (commit marker)
    uint32_t commandRequestToken = 0;  // client → driver (per-client id)
    uint32_t commandProcessedId  = 0;  // driver → client (ACK marker)
    uint32_t commandProcessedToken = 0; // driver → client (token echo)
    uint32_t commandResult       = 0;  // driver → client (RLResult)
};

static_assert(std::is_trivially_copyable<RuntimeLinkHeaderV2>::value,
              "RuntimeLinkHeaderV2 must be trivially copyable");
static_assert(sizeof(RuntimeLinkHeaderV2) == 64,
              "RuntimeLinkHeaderV2 must be exactly one cache line");

// ─── Mapping layout ─────────────────────────────────────────────────────────
//
//   offset 0       RuntimeLinkHeaderV2     (64 B)
//   offset 64      RuntimeLinkTelemetryV2  (512 B, 64-aligned)
//   offset 576     RuntimeLinkCommandV2    (512 B, 64-aligned)
//   total          1088 B
//
// The command offset is derived from header.structSize so a future
// telemetry growth does not silently break readers.

struct RuntimeLinkSharedMemoryV2 {
    RuntimeLinkHeaderV2    header;
    RuntimeLinkTelemetryV2 telemetry;
    RuntimeLinkCommandV2   command;
};

static_assert(std::is_trivially_copyable<RuntimeLinkSharedMemoryV2>::value,
              "RuntimeLinkSharedMemoryV2 must be trivially copyable");

inline constexpr uint32_t RLV2_HeaderSize() {
    return sizeof(RuntimeLinkHeaderV2);
}

inline uint32_t RLV2_TelemetryOffset(const RuntimeLinkHeaderV2& h) {
    (void)h;
    return RLV2_HeaderSize();
}

inline uint32_t RLV2_CommandOffset(const RuntimeLinkHeaderV2& h) {
    return RLV2_HeaderSize() + RLV2_PadTo64(h.structSize);
}

inline constexpr uint32_t RuntimeLinkMappingSizeV2() {
    return sizeof(RuntimeLinkSharedMemoryV2);
}

inline uint32_t RLV2_Fnv1a32(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

// CRC over the stable identity prefix (magic..archClass).
inline uint32_t RLV2_HeaderCrc(const RuntimeLinkHeaderV2& h) {
    uint8_t raw[sizeof(h.magic) * 6];
    size_t p = 0;
    const uint32_t stable[6] = {
        h.magic, h.version, h.size, h.publisherPid, h.structSize, h.archClass
    };
    std::memcpy(raw + p, stable, sizeof(stable));
    p += sizeof(stable);
    return RLV2_Fnv1a32(raw, p);
}

// ─── Hosts registry ─────────────────────────────────────────────────────────
//
// A fixed, well-known mapping listing every live driver instance.  Each
// slot carries pid/sessionId (identity) and lastHeartbeatQpc (liveness).
// All writes happen under the hosts mutex and come from the driver's
// control thread at ~30 Hz (or on shutdown, to unregister).  Stale slots
// from crashed processes are ignored by freshness checking and
// eventually reused.

struct alignas(64) RuntimeHostSlotV2 {
    uint32_t magic           = 0;   // kRuntimeLinkMagic or 0 (empty)
    uint32_t pid             = 0;
    uint32_t archClass       = 0;
    uint32_t reserved0       = 0;
    uint64_t sessionId       = 0;
    uint64_t lastHeartbeatQpc = 0;
    uint32_t reserved1[8]    = {};
};

static_assert(std::is_trivially_copyable<RuntimeHostSlotV2>::value,
              "RuntimeHostSlotV2 must be trivially copyable");
static_assert(sizeof(RuntimeHostSlotV2) == 64,
              "RuntimeHostSlotV2 must be exactly one cache line");

struct alignas(64) RuntimeHostsRegistryV2 {
    uint32_t magic           = kRuntimeLinkMagic;
    uint32_t version         = kRuntimeLinkVersion;
    uint32_t slotCapacity    = kRuntimeHostMaxCount;
    uint32_t reserved0[13]   = {};
    RuntimeHostSlotV2 slots[kRuntimeHostMaxCount];
};

static_assert(std::is_trivially_copyable<RuntimeHostsRegistryV2>::value,
              "RuntimeHostsRegistryV2 must be trivially copyable");
static_assert(sizeof(RuntimeHostsRegistryV2) == 64 + 64 * kRuntimeHostMaxCount,
              "RuntimeHostsRegistryV2 layout must be deterministic");

inline bool RLV2_HostsSlotIsEmpty(const RuntimeHostSlotV2& s) {
    return s.magic != kRuntimeLinkMagic || s.pid == 0 || s.sessionId == 0;
}

// Fresh when the heartbeat is younger than timeoutMs (QPC deltas are
// comparable across processes — the frequency is system-wide).
inline bool RLV2_HostsSlotIsFresh(const RuntimeHostSlotV2& s,
                                  uint64_t nowQpc, uint64_t qpcFreq,
                                  uint32_t timeoutMs) {
    if (RLV2_HostsSlotIsEmpty(s)) return false;
    const uint64_t ageQpc = nowQpc > s.lastHeartbeatQpc
        ? nowQpc - s.lastHeartbeatQpc : 0u;
    if (qpcFreq == 0u) return ageQpc == 0u;
    const uint64_t ageMs = (ageQpc * 1000u) / qpcFreq;
    return ageMs < timeoutMs;
}

// ─── Process-local audio snapshot ───────────────────────────────────────────
//
// NEVER MAPPED.  The audio thread stores into this structure once per
// render block (seq 1 → payload → seq 2).  The driver control thread
// reads it at publish time; a non-2 sequence means a torn/in-progress
// snapshot and the reader keeps the previous snapshot.  Float fields use
// AtomicFloatBits with a fixed stamp of 1 (every publish re-stores all
// floats, so a constant stamp identifies the current epoch).  The 64-bit
// counters are protected by the same odd/even sequence.  The snapshot
// also carries decoder-visible audio-running flags and the limiter
// meter maxima measured in the render loop.

struct alignas(64) RuntimeAudioSnapshot {
    uint32_t sequence = 2;    // 1 = writer inside, 2 = settled (initial state)
    uint32_t tickMs   = 0;    // GetTickCount64() truncated, for diagnostics

    AtomicFloatBits activeVoices;
    AtomicFloatBits releasingVoices;
    AtomicFloatBits freeTop;
    AtomicFloatBits voiceSteals;
    AtomicFloatBits retiredCount;
    AtomicFloatBits retiredImmediateCount;
    AtomicFloatBits decimationStep;

    AtomicFloatBits renderPeak;
    AtomicFloatBits audioRunning;
    AtomicFloatBits soundFontLoaded;
    AtomicFloatBits audioHResult;
    AtomicFloatBits cpuLoadPercent;

    AtomicFloatBits callbackP95Percent;
    AtomicFloatBits callbackP99Percent;
    AtomicFloatBits callbackP999Percent;
    AtomicFloatBits maxConsecutiveOverBudget;

    // Limiter meters (linear peaks + positive dB gain reduction)
    AtomicFloatBits limiterInputPeakL;
    AtomicFloatBits limiterInputPeakR;
    AtomicFloatBits limiterOutputPeakL;
    AtomicFloatBits limiterOutputPeakR;
    AtomicFloatBits limiterGainReductionDb;

    uint64_t overBudgetCallbacks   = 0;
    uint64_t eventsSubmitted       = 0;
    uint64_t eventsAccepted        = 0;
    uint64_t eventsDropped         = 0;
    uint64_t eventsDispatched      = 0;

    uint32_t reserved[14]          = {};
};

static_assert(std::is_trivially_copyable<RuntimeAudioSnapshot>::value,
              "RuntimeAudioSnapshot must be trivially copyable");
static_assert(sizeof(RuntimeAudioSnapshot) % 64 == 0,
              "RuntimeAudioSnapshot must be a multiple of one cache line");

inline constexpr uint32_t RLV2_SnapshotSettledSequence = 2u;

} // namespace svms

#endif // SVMS_RUNTIME_LINK_PROTOCOL_H