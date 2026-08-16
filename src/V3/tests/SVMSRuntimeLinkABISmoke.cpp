#include "../SVMSRuntimeLinkProtocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        ++g_failures; \
    } \
} while(0)

#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        std::printf("FAIL: %s — got %zu, expected %zu (line %d)\n", \
                    msg, static_cast<size_t>(a), static_cast<size_t>(b), \
                    __LINE__); \
        ++g_failures; \
    } \
} while(0)

void TestConstants() {
    CHECK(svms::kRuntimeLinkMagic == 0x53524C56u, "magic must be 'SRLV'");
    CHECK(svms::kRuntimeLinkVersion == 1, "version must be 1");
    CHECK(svms::kRuntimeLinkCmdRingCapacity == 256, "cmd ring capacity must be 256");
}

void TestRLCommandLayout() {
    CHECK_EQ(sizeof(svms::RLCommand), 16u, "RLCommand must be 16 bytes");
    CHECK_EQ(alignof(svms::RLCommand), 16u, "RLCommand must be 16-byte aligned");

    svms::RLCommand cmd{};
    CHECK(cmd.type == svms::RLCommandType::Invalid, "default type must be Invalid");
    CHECK(cmd.param0 == 0, "default param0 must be 0");
    CHECK(cmd.param1 == 0, "default param1 must be 0");
    CHECK(cmd.value0 == 0.0f, "default value0 must be 0.0f");

    // Verify specific sizes of enum
    CHECK_EQ(sizeof(svms::RLCommandType), 4u, "RLCommandType must be uint32_t sized");
}

void TestRLTelemetryLayout() {
    CHECK(sizeof(svms::RLTelemetry) % 64 == 0,
          "RLTelemetry must be cache-line aligned");
    CHECK(alignof(svms::RLTelemetry) >= 64,
          "RLTelemetry alignof must be >= 64");

    svms::RLTelemetry t{};
    CHECK(t.activeVoices == 0, "default activeVoices must be 0");
    CHECK(t.sampleRate == 44100, "default sampleRate must be 44100");
    CHECK(t.bufferFrames == 2048, "default bufferFrames must be 2048");
    CHECK(t.masterVolume == 1.0f, "default masterVolume must be 1.0f");
    CHECK(t.audioRunning == 0, "default audioRunning must be 0");
    CHECK(t.soundFontLoaded == 0, "default soundFontLoaded must be 0");
    CHECK(t.limiterEnabled == 1, "default limiterEnabled must be 1");
    CHECK(t.limiterThreshold == 0.95f, "default limiterThreshold must be 0.95f");
    CHECK(t.correctnessMode == 1, "default correctnessMode must be 1");
    CHECK(t.decimationStep == 1, "default decimationStep must be 1");

    // Verify reverb defaults match ConfigDocument::Defaults()
    CHECK(t.reverbMix == 0.25f, "default reverbMix must be 0.25f");
    CHECK(t.reverbRoomSize == 0.60f, "default reverbRoomSize must be 0.60f");
    CHECK(t.reverbDecay == 0.50f, "default reverbDecay must be 0.50f");
    CHECK(t.reverbDamping == 0.35f, "default reverbDamping must be 0.35f");
}

void TestRuntimeLinkHeaderLayout() {
    CHECK_EQ(sizeof(svms::RuntimeLinkHeader), 64u,
             "RuntimeLinkHeader must be exactly 64 bytes (one cache line)");
    CHECK(alignof(svms::RuntimeLinkHeader) >= 64,
          "RuntimeLinkHeader must be cache-line aligned");

    svms::RuntimeLinkHeader h{};
    CHECK(h.magic == svms::kRuntimeLinkMagic, "header magic must match");
    CHECK(h.version == svms::kRuntimeLinkVersion, "header version must match");
    CHECK(h.cmdCapacity == svms::kRuntimeLinkCmdRingCapacity,
          "header cmdCapacity must match");
}

void TestSharedMemoryLayout() {
    size_t size = svms::RuntimeLinkMappingSize();
    CHECK(size > 0, "RuntimeLinkMappingSize must be > 0");

    // Verify layout is deterministic: header + 2*telemetry + cmdRing
    size_t expected = sizeof(svms::RuntimeLinkHeader)
                    + 2 * sizeof(svms::RLTelemetry)
                    + svms::kRuntimeLinkCmdRingCapacity * sizeof(svms::RLCommand);
    CHECK_EQ(size, expected, "mapping size must equal header + 2*telemetry + cmdRing");

    svms::RuntimeLinkSharedMemory mem{};
    CHECK_EQ(mem.header.size, 0u, "header.size must be 0 by default (set at init)");
    CHECK_EQ(sizeof(mem.telemetry) / sizeof(mem.telemetry[0]), 2u,
             "must have exactly 2 telemetry slots");
    CHECK_EQ(sizeof(mem.cmdRing) / sizeof(mem.cmdRing[0]),
             svms::kRuntimeLinkCmdRingCapacity,
             "cmdRing must have correct capacity");

    // Verify telemetry slots are properly aligned
    size_t off0 = reinterpret_cast<const char*>(&mem.telemetry[0])
                - reinterpret_cast<const char*>(&mem);
    size_t off1 = reinterpret_cast<const char*>(&mem.telemetry[1])
                - reinterpret_cast<const char*>(&mem);
    CHECK(off0 % 64 == 0, "telemetry[0] must be cache-line aligned in mapping");
    CHECK(off1 % 64 == 0, "telemetry[1] must be cache-line aligned in mapping");
}

void TestCommandRingSPSC() {
    svms::RuntimeLinkSharedMemory mem{};
    mem.header.cmdCapacity = svms::kRuntimeLinkCmdRingCapacity;
    mem.header.cmdHead = 0;
    mem.header.cmdTail.store(0);

    svms::RLCommand cmd;
    cmd.type = svms::RLCommandType::Ping;
    cmd.param0 = 42;
    cmd.value0 = 3.14f;

    // Ring should be empty
    svms::RLCommand outCmd{svms::RLCommandType::Ping, 99, 0, 1.0f};
    CHECK(!svms::RL_PopCommand(&mem, outCmd), "pop from empty ring must fail");
    CHECK(outCmd.type == svms::RLCommandType::Ping, "pop must not modify cmd on failure");
    CHECK(outCmd.param0 == 99u, "pop must not modify param0 on failure");

    // Push one command
    CHECK(svms::RL_PushCommand(&mem, cmd), "push must succeed");

    // Pop it back
    svms::RLCommand out{};
    CHECK(svms::RL_PopCommand(&mem, out), "pop must succeed after push");
    CHECK(out.type == svms::RLCommandType::Ping, "popped type must match");
    CHECK(out.param0 == 42u, "popped param0 must match");
    CHECK(out.value0 == 3.14f, "popped value0 must match");

    // Ring should be empty again
    CHECK(!svms::RL_PopCommand(&mem, out), "pop from drained ring must fail");
}

void TestCommandRingFull() {
    svms::RuntimeLinkSharedMemory mem{};
    mem.header.cmdCapacity = svms::kRuntimeLinkCmdRingCapacity;

    // Fill the ring to capacity
    for (uint32_t i = 0; i < svms::kRuntimeLinkCmdRingCapacity; ++i) {
        svms::RLCommand cmd{};
        cmd.type = svms::RLCommandType::SetMasterVolume;
        cmd.param0 = i;
        cmd.value0 = static_cast<float>(i) / 100.0f;
        bool pushed = svms::RL_PushCommand(&mem, cmd);
        if (!pushed) {
            std::printf("FAIL: push failed at index %u (line %d)\n", i, __LINE__);
            ++g_failures;
            return;
        }
    }

    // Next push must fail (ring full)
    svms::RLCommand extra{};
    extra.type = svms::RLCommandType::Ping;
    CHECK(!svms::RL_PushCommand(&mem, extra), "push to full ring must fail");

    // Drain all and verify ordering
    for (uint32_t i = 0; i < svms::kRuntimeLinkCmdRingCapacity; ++i) {
        svms::RLCommand out{};
        bool popped = svms::RL_PopCommand(&mem, out);
        if (!popped) {
            std::printf("FAIL: pop failed at index %u (line %d)\n", i, __LINE__);
            ++g_failures;
            return;
        }
        if (out.param0 != i) {
            std::printf("FAIL: pop order mismatch at index %u: got param0=%u (line %d)\n",
                        i, out.param0, __LINE__);
            ++g_failures;
        }
        if (out.value0 != static_cast<float>(i) / 100.0f) {
            std::printf("FAIL: pop value mismatch at index %u (line %d)\n", i, __LINE__);
            ++g_failures;
        }
    }

    // Should be empty now
    svms::RLCommand drain{};
    CHECK(!svms::RL_PopCommand(&mem, drain), "pop from drained ring must fail");
}

void TestTelemetryReadWrite() {
    svms::RuntimeLinkSharedMemory mem{};

    // Write to slot 1 (initially writeIndex is 0, so we write to slot 1)
    svms::RLTelemetry snap{};
    snap.activeVoices = 1234;
    snap.sampleRate = 48000;
    snap.masterVolume = 0.75f;
    snap.cpuLoadPercent = 42.5f;
    snap.eventsSubmitted = 9999;

    mem.telemetry[1] = snap;
    mem.header.telemetryWriteIndex.store(1, std::memory_order_release);

    // Read should return slot 1
    const svms::RLTelemetry& read = svms::RL_ReadTelemetry(&mem);
    CHECK(read.activeVoices == 1234, "read telemetry activeVoices must match");
    CHECK(read.sampleRate == 48000u, "read telemetry sampleRate must match");
    CHECK(read.masterVolume == 0.75f, "read telemetry masterVolume must match");
    CHECK(read.cpuLoadPercent == 42.5f, "read telemetry cpuLoadPercent must match");
    CHECK(read.eventsSubmitted == 9999u, "read telemetry eventsSubmitted must match");

    // Write to slot 0 (flip)
    snap.activeVoices = 5678;
    mem.telemetry[0] = snap;
    mem.header.telemetryWriteIndex.store(0, std::memory_order_release);

    const svms::RLTelemetry& read2 = svms::RL_ReadTelemetry(&mem);
    CHECK(read2.activeVoices == 5678, "flip read must return new slot");
}

void TestNamingConventions() {
    wchar_t buf[128];

    svms::RL_SharedMemName(12345, buf, 128);
    CHECK(wcscmp(buf, L"Local\\SVMS_V3_RuntimeLink_v1_12345") == 0,
          "shared mem name format must match");

    svms::RL_MutexName(12345, buf, 128);
    CHECK(wcscmp(buf, L"Local\\SVMS_V3_RL_Mutex_12345") == 0,
          "mutex name format must match");

    svms::RL_CmdEventName(12345, buf, 128);
    CHECK(wcscmp(buf, L"Local\\SVMS_V3_RL_CmdEvent_12345") == 0,
          "cmd event name format must match");
}

void TestEnumCompleteness() {
    // Verify all command types are distinct
    svms::RLCommandType types[] = {
        svms::RLCommandType::Ping,
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
        svms::RLCommandType::SetLimiterEnabled,
        svms::RLCommandType::SetLimiterThreshold,
        svms::RLCommandType::SetLimiterLookahead,
        svms::RLCommandType::SetLimiterAttack,
        svms::RLCommandType::SetLimiterRelease,
        svms::RLCommandType::SetCorrectnessMode,
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

    CHECK(static_cast<uint32_t>(svms::RLCommandType::Ping) == 0,
          "Ping must be 0");
    CHECK(static_cast<uint32_t>(svms::RLCommandType::RequestRestart) == 0x100,
          "RequestRestart must be 0x100");
    CHECK(static_cast<uint32_t>(svms::RLCommandType::Invalid) == 0xFFFFFFFF,
          "Invalid must be 0xFFFFFFFF");
}

} // anonymous namespace

int main() {
    std::puts("=== SVMS RuntimeLink Protocol ABI Tests ===");

    TestConstants();
    TestRLCommandLayout();
    TestRLTelemetryLayout();
    TestRuntimeLinkHeaderLayout();
    TestSharedMemoryLayout();
    TestCommandRingSPSC();
    TestCommandRingFull();
    TestTelemetryReadWrite();
    TestNamingConventions();
    TestEnumCompleteness();

    if (g_failures == 0) {
        std::puts("PASS: all RuntimeLink protocol ABI tests passed");
        return 0;
    } else {
        std::printf("FAIL: %d test(s) failed\n", g_failures);
        return 1;
    }
}
