// Live settings on a key-sharded engine reach every core.
// Drives SVMSAPI.dll with the device-free test output (SVMS_TEST_FAKE_AUDIO=1)
// and an isolated configuration that asks for two shards.
//   svms_v3_shard_commands_tests <path to SVMSAPI.dll>
#include <windows.h>

#include "include/svmsapi.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using GetInterfaceProc = SVMS_Result(SVMS_CALL*)(uint32_t, uint32_t, SVMS_Interface*);

SVMS_Interface g_api{};
SVMS_Session g_session = 0;

SVMS_TelemetryV2 Telemetry() {
    SVMS_TelemetryV2 t{};
    t.struct_size = sizeof(t);
    t.struct_version = SVMS_STRUCT_VERSION_1;
    g_api.get_telemetry_v2(g_session, &t);
    return t;
}

SVMS_Result Command(uint32_t id, uint32_t param) {
    char text[512] = {};
    return g_api.send_runtime_command(g_session, id, param, text, sizeof(text));
}

// Every key gets 64 note-ons well inside the 20 ms coalescing window, then a
// note-off; returns how many of the note-ons were submitted but not accepted
// once all of them have been counted.
uint64_t FloodDropped() {
    const SVMS_TelemetryV2 before = Telemetry();
    const uint32_t keys = 32u, hits = 64u;
    for (uint32_t key = 40u; key < 40u + keys; ++key) {
        for (uint32_t i = 0; i < hits; ++i)
            g_api.send_short(g_session, 0x90u | (key << 8) | (100u << 16));
        g_api.send_short(g_session, 0x80u | (key << 8));
    }
    const uint64_t sent = uint64_t(keys) * (hits + 1u);
    uint64_t submitted = 0, accepted = 0;
    for (int tries = 0; tries < 400 && submitted < sent; ++tries) {
        Sleep(5);
        const SVMS_TelemetryV2 now = Telemetry();
        submitted = now.submitted_events - before.submitted_events;
        accepted = now.accepted_events - before.accepted_events;
    }
    if (submitted < sent)
        std::printf("note: only %llu of %llu events were counted\n",
                    (unsigned long long)submitted, (unsigned long long)sent);
    return submitted > accepted ? submitted - accepted : 0u;
}

// With coalescing at 32 and re-excite off, a key keeps 2 of 64 hits; the rest
// are dropped on whichever core owns the key. The 32 keys split 15/17 over
// the two cores, so a master-only setting drops under half of them; asking
// for three quarters leaves room for a send loop preempted past a window.
// Switching it back off must reach every core too.
bool CoalescingReachesEveryShard() {
    const uint64_t off = FloodDropped();
    if (off != 0u) {
        std::printf("FAIL: %llu note-ons dropped with coalescing off\n",
                    (unsigned long long)off);
        return false;
    }
    if (Command(SVMS_COMMAND_SET_NOTE_ON_REEXCITE, 0u) != SVMS_RESULT_OK ||
        Command(SVMS_COMMAND_SET_NOTE_ON_COLLAPSE, 32u) != SVMS_RESULT_OK) {
        std::puts("FAIL: the coalescing commands were refused");
        return false;
    }
    const uint64_t wanted = 32u * 64u * 3u / 4u;
    const uint64_t on = FloodDropped();
    if (on < wanted) {
        std::printf("FAIL: %llu note-ons coalesced, expected at least %llu\n",
                    (unsigned long long)on, (unsigned long long)wanted);
        return false;
    }
    Sleep(30);  // let the 20 ms windows of the last flood close
    if (Command(SVMS_COMMAND_SET_NOTE_ON_COLLAPSE, 1u) != SVMS_RESULT_OK) {
        std::puts("FAIL: switching coalescing off was refused");
        return false;
    }
    const uint64_t offAgain = FloodDropped();
    if (offAgain != 0u) {
        std::printf("FAIL: %llu note-ons dropped after switching coalescing off\n",
                    (unsigned long long)offAgain);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::puts("usage: svms_v3_shard_commands_tests <SVMSAPI.dll>");
        return 1;
    }
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string dir = std::string(temp) + "svms_shard_commands_test\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    const std::string config = dir + "config.json";
    FILE* f = std::fopen(config.c_str(), "wb");
    if (!f) {
        std::puts("FAIL: could not write the test config");
        return 1;
    }
    std::fputs("{\"schema_version\": 1, \"synth\": {\"shards\": 2}}\n", f);
    std::fclose(f);
    SetEnvironmentVariableA("SVMS_TEST_LOCAL_CONFIG_PATH", config.c_str());
    SetEnvironmentVariableA("SVMS_TEST_CONFIG_PATH", config.c_str());
    SetEnvironmentVariableA("SVMS_TEST_FAKE_AUDIO", "1");

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        std::puts("FAIL: could not load SVMSAPI.dll");
        return 1;
    }
    auto getInterface =
        reinterpret_cast<GetInterfaceProc>(GetProcAddress(dll, "SVMS_GetInterface"));
    if (!getInterface ||
        getInterface(SVMS_ABI_VERSION_1, sizeof(g_api), &g_api) != SVMS_RESULT_OK) {
        std::puts("FAIL: no SVMS interface");
        return 1;
    }
    SVMS_SessionConfig cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.struct_version = SVMS_STRUCT_VERSION_1;
    if (g_api.create_session(&cfg, &g_session) != SVMS_RESULT_OK) {
        std::puts("FAIL: the session did not start on the test output");
        return 1;
    }
    const bool ok = CoalescingReachesEveryShard();
    g_api.destroy_session(g_session);
    FreeLibrary(dll);
    if (!ok) return 1;
    std::puts("shard command tests passed");
    return 0;
}
