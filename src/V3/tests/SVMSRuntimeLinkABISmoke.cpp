#include "../SVMSRuntimeLinkProtocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstddef>

namespace {

int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        ++g_failures; \
    } \
} while(0)

#define CHECK_EQ(a, b, msg) do { \
    if (static_cast<size_t>(a) != static_cast<size_t>(b)) { \
        std::printf("FAIL: %s — got %zu, expected %zu (line %d)\n", \
                    msg, static_cast<size_t>(a), static_cast<size_t>(b), \
                    __LINE__); \
        ++g_failures; \
    } \
} while(0)

void TestConstants() {
    CHECK(svms::kRuntimeLinkMagic == 0x53524C32u, "magic must be 'SRL2'");
    CHECK_EQ(svms::kRuntimeLinkVersion, 2u, "version must be 2");
    CHECK_EQ(svms::kRuntimeLinkArchX86, 0u, "arch X86 must be 0");
    CHECK_EQ(svms::kRuntimeLinkArchX64, 1u, "arch X64 must be 1");
    CHECK_EQ(svms::kRuntimeHostMaxCount, 16u, "hosts max count must be 16");
    CHECK_EQ(svms::kRuntimeHostTimeoutMs, 5000u, "host timeout must be 5000 ms");
    CHECK_EQ(svms::kRuntimeLinkMutexTimeoutMs, 1000u, "mutex timeout must be 1000 ms");
    CHECK_EQ(svms::kRuntimeLinkResultTextCapacity, 256u,
             "result text capacity must be 256");
    CHECK_EQ(svms::kRuntimeLinkDefaultCommandTimeoutMs, 400u,
             "command timeout must be 400 ms");
    CHECK_EQ(svms::kRuntimeLinkPublishIntervalMs, 33u,
             "publish interval must be 33 ms");
    CHECK_EQ(svms::RLV2_PadTo64(1), 64u, "PadTo64(1) must be 64");
    CHECK_EQ(svms::RLV2_PadTo64(64), 64u, "PadTo64(64) must be 64");
    CHECK_EQ(svms::RLV2_PadTo64(65), 128u, "PadTo64(65) must be 128");
    CHECK_EQ(svms::RLV2_SnapshotSettledParity, 0,
             "snapshot settled parity must be 0 (even sequence)");
}

void TestPODLayout() {
    // Each mapped struct must be trivially copyable & the right size.
    CHECK(svms::RLV2_FloatBits(1.0f) != 0, "FloatBits store must not be 0");

    // Hand-computed sizes must hold (guards against padding drift).
    CHECK_EQ(sizeof(svms::RuntimeLiveStateV2), 92u,
             "RuntimeLiveStateV2 must be 92 bytes (12 + 18*4 + 8)");
    CHECK_EQ(sizeof(svms::RuntimeLinkHeaderV2), 64u,
             "RuntimeLinkHeaderV2 must be exactly one cache line");
    CHECK_EQ(sizeof(svms::RuntimeHostSlotV2), 64u,
             "RuntimeHostSlotV2 must be exactly one cache line");
    CHECK_EQ(sizeof(svms::RuntimeHostsRegistryV2), 64u + 64u * svms::kRuntimeHostMaxCount,
             "RuntimeHostsRegistryV2 must be 64 + 16 slots * 64");
    CHECK_EQ(sizeof(svms::RuntimeLinkSharedMemoryV2), 1088u,
             "mapping must be 1088 bytes");
    CHECK_EQ(sizeof(svms::RuntimeAudioSnapshot) % 64, 0u,
             "RuntimeAudioSnapshot must be a cache-line multiple");

    CHECK_EQ(alignof(svms::RuntimeLinkHeaderV2) >= 64 ? size_t(1) : size_t(0), 1u,
             "RuntimeLinkHeaderV2 must be 64-aligned");
    CHECK_EQ(alignof(svms::RuntimeLinkTelemetryV2) >= 64 ? size_t(1) : size_t(0), 1u,
             "RuntimeLinkTelemetryV2 must be 64-aligned");
    CHECK_EQ(alignof(svms::RuntimeLinkCommandV2) >= 64 ? size_t(1) : size_t(0), 1u,
             "RuntimeLinkCommandV2 must be 64-aligned");
}

void TestLiveDefaults() {
    svms::RuntimeLiveStateV2 l{};
    CHECK_EQ(l.correctnessMode, 0u, "default correctnessMode must be 0");
    CHECK_EQ(l.reverbEnabled, 0u, "default reverbEnabled must be 0");
    CHECK_EQ(l.limiterEnabled, 1u, "default limiterEnabled must be 1");
    CHECK(l.masterVolume == 1.0f, "default masterVolume must be 1.0f");
    CHECK(l.reverbMix == 0.25f, "default reverbMix must be 0.25f");
    CHECK(l.reverbRoomSize == 0.60f, "default reverbRoomSize must be 0.60f");
    CHECK(l.reverbDecay == 0.50f, "default reverbDecay must be 0.50f");
    CHECK(l.reverbDamping == 0.35f, "default reverbDamping must be 0.35f");
    CHECK(l.reverbWidth == 1.0f, "default reverbWidth must be 1.0f");
    CHECK(l.reverbDiffusion == 0.70f, "default reverbDiffusion must be 0.70f");
    CHECK(l.reverbPreDelayMs == 12.0f, "default reverbPreDelayMs must be 12.0f");
    CHECK(l.reverbEarlyLevel == 0.35f, "default reverbEarlyLevel must be 0.35f");
    CHECK(l.reverbLateLevel == 0.85f, "default reverbLateLevel must be 0.85f");
    CHECK(l.reverbModDepth == 0.30f, "default reverbModDepth must be 0.30f");
    CHECK(l.reverbModRate == 0.35f, "default reverbModRate must be 0.35f");
    CHECK(l.reverbLowCutHz == 70.0f, "default reverbLowCutHz must be 70.0f");
    CHECK(l.reverbHighCutHz == 16000.0f, "default reverbHighCutHz must be 16000.0f");
    CHECK(l.limiterThreshold == 0.95f, "default limiterThreshold must be 0.95f");
    CHECK(l.limiterLookaheadMs == 3.0f, "default limiterLookaheadMs must be 3.0f");
    CHECK(l.limiterAttackMs == 0.5f, "default limiterAttackMs must be 0.5f");
    CHECK(l.limiterReleaseMs == 100.0f, "default limiterReleaseMs must be 100.0f");
}

void TestTelemetryLayout() {
    CHECK_EQ(sizeof(svms::RuntimeLinkTelemetryV2) % 64, 0u,
             "RuntimeLinkTelemetryV2 must be a cache-line multiple");
    CHECK_EQ(sizeof(svms::RuntimeLinkTelemetryV2), 512u,
             "RuntimeLinkTelemetryV2 must be 512 bytes");
    // Field offsets must be deterministic (no padding drift).
    CHECK_EQ(offsetof(svms::RuntimeLinkTelemetryV2, live), 144u,
             "telemetry.live must sit at byte 144");
    CHECK_EQ(offsetof(svms::RuntimeLinkTelemetryV2, soundFontName), 236u,
             "telemetry.soundFontName must sit at byte 236");

    svms::RuntimeLinkTelemetryV2 t{};
    CHECK_EQ(t.activeVoices, 0u, "default activeVoices must be 0");
    CHECK_EQ(t.sampleRate, 44100u, "default sampleRate must be 44100");
    CHECK_EQ(t.bufferFrames, 2048u, "default bufferFrames must be 2048");
    CHECK_EQ(t.decimationStep, 1u, "default decimationStep must be 1");
    CHECK_EQ(t.maxVoices, 0u, "default maxVoices must be 0");
}

void TestCommandLayout() {
    CHECK_EQ(sizeof(svms::RuntimeLinkCommandV2) % 64, 0u,
             "RuntimeLinkCommandV2 must be a cache-line multiple");
    CHECK_EQ(offsetof(svms::RuntimeLinkCommandV2, live), 16u,
             "command.live must sit at byte 16");
    CHECK_EQ(offsetof(svms::RuntimeLinkCommandV2, resultText), 108u,
             "command.resultText must sit at byte 108");

    svms::RuntimeLinkCommandV2 c{};
    CHECK_EQ(c.type, 0u, "default command type must be NoCommand (0)");
    CHECK_EQ(c.param, 0u, "default param must be 0");
    CHECK_EQ(sizeof(c.resultText), svms::kRuntimeLinkResultTextCapacity,
             "resultText capacity must match protocol constant");
}

void TestMappingOffsets() {
    svms::RuntimeLinkSharedMemoryV2 mem{};
    // Committed offsets: header@0, telemetry@64, command@576.
    size_t offTele = reinterpret_cast<const char*>(&mem.telemetry)
                   - reinterpret_cast<const char*>(&mem);
    size_t offCmd  = reinterpret_cast<const char*>(&mem.command)
                   - reinterpret_cast<const char*>(&mem);
    CHECK_EQ(offTele, 64u, "telemetry must sit at offset 64");
    CHECK_EQ(offCmd, 576u, "command must sit at offset 576");
    CHECK(offTele % 64 == 0 && offCmd % 64 == 0,
          "telemetry and command must be cache-line aligned in mapping");

    // The same offsets must be derivable from the header protocol.
    svms::RuntimeLinkHeaderV2 h{};
    h.structSize = static_cast<uint32_t>(sizeof(svms::RuntimeLinkTelemetryV2));
    CHECK_EQ(svms::RLV2_TelemetryOffset(h), 64u, "RLV2_TelemetryOffset must be 64");
    CHECK_EQ(svms::RLV2_CommandOffset(h), 576u, "RLV2_CommandOffset must be 576");
    CHECK_EQ(svms::RuntimeLinkMappingSizeV2(), 1088u,
             "mapping size must be 1088");
}

void TestHeaderCrc() {
    svms::RuntimeLinkHeaderV2 h{};
    h.size = svms::RuntimeLinkMappingSizeV2();
    h.publisherPid = 12345;
    h.structSize = static_cast<uint32_t>(sizeof(svms::RuntimeLinkTelemetryV2));
    h.archClass = svms::kRuntimeLinkArchX64;

    const uint32_t crc = svms::RLV2_HeaderCrc(h);
    CHECK(crc != 0, "header Crc must be non-zero");

    // Mutating any stable identity field must change the crc.
    h.publisherPid = 12346;
    CHECK(svms::RLV2_HeaderCrc(h) != crc, "crc must change with publisherPid");
    h.publisherPid = 12345;
    CHECK(svms::RLV2_HeaderCrc(h) == crc, "crc must be stable for identical header");
    h.version = 3;
    CHECK(svms::RLV2_HeaderCrc(h) != crc, "crc must change with version");
}

void TestFloatBits() {
    // Exact IEEE bit-pattern transports: identity and negative values.
    CHECK(svms::RLV2_BitsToFloat(svms::RLV2_FloatBits(1.25f)) == 1.25f,
          "float→bits→float round trip must match");
    CHECK(svms::RLV2_BitsToFloat(svms::RLV2_FloatBits(-7.5f)) == -7.5f,
          "negative float round trip must match");
    CHECK(svms::RLV2_BitsToFloat(svms::RLV2_FloatBits(0.0f)) == 0.0f,
          "zero round trip must match");
    const uint32_t bits = svms::RLV2_FloatBits(0.5f);
    CHECK(bits != 0u, "float bits must be nonzero");
    CHECK(svms::RLV2_BitsToFloat(bits) == 0.5f, "bits→float must match 0.5f");
}

void TestEnumCompleteness() {
    svms::RLCommandType types[] = {
        svms::RLCommandType::NoCommand,
        svms::RLCommandType::SetMasterVolume,
        svms::RLCommandType::SetReverbEnabled,
        svms::RLCommandType::SetReverbMix,
        svms::RLCommandType::SetReverbRoomSize,
        svms::RLCommandType::SetReverbDecay,
        svms::RLCommandType::SetReverbDamping,
        svms::RLCommandType::SetReverbWidth,
        svms::RLCommandType::SetReverbDiffusion,
        svms::RLCommandType::SetReverbPreDelayMs,
        svms::RLCommandType::SetReverbEarlyLevel,
        svms::RLCommandType::SetReverbLateLevel,
        svms::RLCommandType::SetReverbModDepth,
        svms::RLCommandType::SetReverbModRate,
        svms::RLCommandType::SetReverbLowCutHz,
        svms::RLCommandType::SetReverbHighCutHz,
        svms::RLCommandType::SetCorrectnessMode,
        svms::RLCommandType::SetLimiterEnabled,
        svms::RLCommandType::SetLimiterThreshold,
        svms::RLCommandType::SetLimiterLookahead,
        svms::RLCommandType::SetLimiterAttack,
        svms::RLCommandType::SetLimiterRelease,
        svms::RLCommandType::Ping,
        svms::RLCommandType::ApplyLiveConfig,
        svms::RLCommandType::ReloadSoundFont,
        svms::RLCommandType::ResetVoices,
        svms::RLCommandType::RequestRestart,
        svms::RLCommandType::Invalid,
    };
    constexpr int count = sizeof(types) / sizeof(types[0]);
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (types[i] == types[j]) {
                std::printf("FAIL: duplicate command type at indices %d and %d (line %d)\n",
                            i, j, __LINE__);
                ++g_failures;
            }
        }
    }

    CHECK_EQ(svms::RLCommandType::NoCommand, 0u, "NoCommand must be 0");
    CHECK_EQ(svms::RLCommandType::Ping, 0x20u, "Ping must be 0x20");
    CHECK_EQ(svms::RLCommandType::ApplyLiveConfig, 0x100u,
             "ApplyLiveConfig must be 0x100");
    CHECK_EQ(svms::RLCommandType::ReloadSoundFont, 0x101u,
             "ReloadSoundFont must be 0x101");
    CHECK_EQ(svms::RLCommandType::ResetVoices, 0x102u, "ResetVoices must be 0x102");
    CHECK_EQ(svms::RLCommandType::RequestRestart, 0x110u,
             "RequestRestart must be 0x110");
    CHECK_EQ(svms::RLCommandType::Invalid, 0xFFFFFFFFu, "Invalid must be 0xFFFFFFFF");
}

void TestGroups() {
    CHECK_EQ(svms::RLV2_GroupForType(svms::RLCommandType::SetMasterVolume),
             svms::RLGroupMaster, "master volume must map to master group");
    CHECK_EQ(svms::RLV2_GroupForType(svms::RLCommandType::SetReverbMix),
             svms::RLGroupReverb, "reverb mix must map to reverb group");
    CHECK_EQ(svms::RLV2_GroupForType(svms::RLCommandType::SetReverbHighCutHz),
             svms::RLGroupReverb, "high cut must map to reverb group");
    CHECK_EQ(svms::RLV2_GroupForType(svms::RLCommandType::SetCorrectnessMode),
             svms::RLGroupCorrectness, "correctness must map to correctness group");
    CHECK_EQ(svms::RLV2_GroupForType(svms::RLCommandType::SetLimiterRelease),
             svms::RLGroupLimiter, "limiter release must map to limiter group");
    CHECK_EQ(svms::RLV2_GroupForType(svms::RLCommandType::SetLimiterThreshold),
             svms::RLGroupLimiter, "limiter threshold must map to limiter group");
    CHECK_EQ(svms::RLV2_GroupForType(svms::RLCommandType::Ping), 0u,
             "wire commands must not map to a group");
    CHECK_EQ(svms::RLV2_GroupForType(svms::RLCommandType::ApplyLiveConfig), 0u,
             "ApplyLiveConfig must not map to a group");
    CHECK_EQ(svms::RLGroupAll, 0xFu, "RLGroupAll must be the union of all groups");
}

void TestNamingConventions() {
    wchar_t buf[128];
    svms::RLV2_SharedMemName(12345, buf, 128);
    CHECK(wcscmp(buf, L"Local\\SVMS_V3_RuntimeLink_v2_12345") == 0,
          "shared mem name format must match");
    svms::RLV2_MutexName(12345, buf, 128);
    CHECK(wcscmp(buf, L"Local\\SVMS_V3_RuntimeMutex_v2_12345") == 0,
          "mutex name format must match");
    svms::RLV2_CmdEventName(12345, buf, 128);
    CHECK(wcscmp(buf, L"Local\\SVMS_V3_RuntimeCommand_v2_12345") == 0,
          "command event name format must match");
    svms::RLV2_HostsRegName(buf, 128);
    CHECK(wcscmp(buf, L"Local\\SVMS_V3_RuntimeHosts_v2") == 0,
          "hosts registry name format must match");
    svms::RLV2_HostsMutexName(buf, 128);
    CHECK(wcscmp(buf, L"Local\\SVMS_V3_RuntimeHostsMutex_v2") == 0,
          "hosts mutex name format must match");
}

void TestHostsRegistry() {
    svms::RuntimeHostsRegistryV2 reg{};
    CHECK(reg.magic == svms::kRuntimeLinkMagic, "hosts registry magic must be set");
    CHECK(reg.version == svms::kRuntimeLinkVersion, "hosts registry version must be 2");
    CHECK(reg.slotCapacity == svms::kRuntimeHostMaxCount,
          "hosts registry capacity must be 16");

    const svms::RuntimeHostSlotV2& empty = reg.slots[0];
    CHECK(svms::RLV2_HostsSlotIsEmpty(empty), "default slot must be empty");
    CHECK(!svms::RLV2_HostsSlotIsFresh(empty, 1000u, 10000u,
                                       svms::kRuntimeHostTimeoutMs),
          "empty slot must never be fresh");

    svms::RuntimeHostSlotV2 slot{};
    slot.magic = svms::kRuntimeLinkMagic;
    slot.pid = 7777;
    slot.sessionId = 42;
    slot.lastHeartbeatQpc = 5000u;
    CHECK(!svms::RLV2_HostsSlotIsEmpty(slot), "populated slot must not be empty");
    CHECK(svms::RLV2_HostsSlotIsFresh(slot, 6000u, 10000u, 1000u),
          "recent heartbeat must be fresh");
    CHECK(!svms::RLV2_HostsSlotIsFresh(slot, 60000u, 10000u, 1000u),
          "old heartbeat must be stale");
}

void TestSnapshotProtocol() {
    svms::RuntimeAudioSnapshot snap{};
    CHECK_EQ(snap.sequence.load(std::memory_order_relaxed) & 1u,
             svms::RLV2_SnapshotSettledParity,
             "fresh snapshot must start settled (even sequence)");

    // Simulate an audio-thread publish: odd sequence, payload stores,
    // then the settle word (even, monotonic +2 from the apex).
    const uint32_t startSeq = snap.sequence.load(std::memory_order_relaxed);
    snap.sequence.store(startSeq | 1u, std::memory_order_relaxed);
    snap.activeVoices.store(1234u, std::memory_order_relaxed);
    snap.releasingVoices.store(99u, std::memory_order_relaxed);
    snap.limiterInputPeakLBits.store(svms::RLV2_FloatBits(0.75f),
                                     std::memory_order_relaxed);
    snap.eventsSubmitted.store(9999u, std::memory_order_relaxed);
    snap.sequence.store(startSeq + 2u, std::memory_order_release);

    // Settled copy must read every field.
    CHECK_EQ(snap.sequence.load(std::memory_order_acquire), startSeq + 2u,
             "settled sequence must advance by exactly 2");
    CHECK_EQ(snap.activeVoices.load(std::memory_order_relaxed), 1234u,
             "settled activeVoices must read back");
    CHECK_EQ(snap.releasingVoices.load(std::memory_order_relaxed), 99u,
             "settled releasingVoices must read back");
    CHECK(svms::RLV2_BitsToFloat(
              snap.limiterInputPeakLBits.load(std::memory_order_relaxed)) == 0.75f,
          "settled limiter meter must read back");
    CHECK_EQ(snap.eventsSubmitted.load(std::memory_order_relaxed), 9999u,
             "settled u64 counter must read back");

    // The sequence is strictly monotonic: a writer-in-progress (odd)
    // sequence must be detectable and the word must never go backwards.
    const uint32_t even2 = snap.sequence.load(std::memory_order_relaxed);
    snap.sequence.store(even2 | 1u, std::memory_order_relaxed);
    CHECK_EQ(snap.sequence.load(std::memory_order_relaxed) & 1u, 1u,
             "writer-in-progress sequence must be detectable (odd)");
    CHECK(snap.sequence.load(std::memory_order_relaxed) > even2,
          "sequenced word must never go backwards");
}

void TestResultStrings() {
    CHECK(strcmp(svms::RLV2_ResultToString(svms::RLResult::Ok), "OK") == 0,
          "Ok must stringify to OK");
    CHECK(strcmp(svms::RLV2_ResultToString(svms::RLResult::Busy), "Busy") == 0,
          "Busy must stringify to Busy");
    CHECK(strcmp(svms::RLV2_ResultToString(svms::RLResult::LoadFailed),
                 "Load failed") == 0,
          "LoadFailed must stringify to Load failed");
    CHECK(strcmp(svms::RLV2_ResultToString(svms::RLResult::RestartRequired),
                 "Restart required") == 0,
          "RestartRequired must stringify to Restart required");
    CHECK(strcmp(svms::RLV2_ResultToString(svms::RLResult::InvalidArgument),
                 "Invalid argument") == 0,
          "InvalidArgument must stringify correctly");
    CHECK(strcmp(svms::RLV2_ResultToString(svms::RLResult::Unsupported),
                 "Unsupported") == 0,
          "Unsupported must stringify correctly");
    CHECK(strcmp(svms::RLV2_ResultToString(svms::RLResult::InternalError),
                 "Internal error") == 0,
          "InternalError must stringify correctly");
}

} // anonymous namespace

int main() {
    std::puts("=== SVMS RuntimeLink V2 Protocol ABI Tests ===");

    TestConstants();
    TestPODLayout();
    TestLiveDefaults();
    TestTelemetryLayout();
    TestCommandLayout();
    TestMappingOffsets();
    TestHeaderCrc();
    TestFloatBits();
    TestEnumCompleteness();
    TestGroups();
    TestNamingConventions();
    TestHostsRegistry();
    TestSnapshotProtocol();
    TestResultStrings();

    if (g_failures == 0) {
        std::puts("PASS: all RuntimeLink V2 protocol ABI tests passed");
        return 0;
    } else {
        std::printf("FAIL: %d test(s) failed\n", g_failures);
        return 1;
    }
}