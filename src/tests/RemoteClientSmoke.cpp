// RemoteClientSmoke.cpp - REAL two-process test of the SVMSAPI remote client.
//
// Process 1 (this test) loads SVMSAPI.dll (the remote client), spawns Process 2
// (itself, --host; its engine DLL is argv[2] when given, e.g. winmm.dll),
// which loads the same DLL and creates a session (= a running SVMS engine).
// Process 1 then uses ONLY the SVMS_Remote_* exports: enumerate hosts, attach,
// read telemetry V3, apply live params, send a runtime command, detach.
//
// Skips (exit 77) when the host cannot start (no audio device / no session).
#include <windows.h>

#include "include/svmsapi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using GetInterfaceProc = SVMS_Result (SVMS_CALL*)(uint32_t, uint32_t,
                                                  SVMS_Interface*);
using EnumerateProc = uint32_t (SVMS_CALL*)(SVMS_HostInfo*, uint32_t);
using AttachProc = SVMS_Result (SVMS_CALL*)(uint32_t, uint32_t,
                                            SVMS_Interface*, SVMS_Session*);
using IsAliveProc = SVMS_Result (SVMS_CALL*)(SVMS_Session, uint32_t,
                                             uint32_t*);

int Fail(const char* message) {
    std::printf("FAIL: %s\n", message);
    return 1;
}

int RunHost(const char* dllPath) {
    HMODULE module = LoadLibraryA(dllPath);
    if (!module) return 77;
    const auto getInterface = reinterpret_cast<GetInterfaceProc>(
        GetProcAddress(module, "SVMS_GetInterface"));
    SVMS_Interface api{};
    if (!getInterface || getInterface(SVMS_ABI_VERSION_1, sizeof(api), &api) !=
                             SVMS_RESULT_OK)
        return 77;
    SVMS_SessionConfig config{};
    config.struct_size = sizeof(config);
    config.struct_version = SVMS_STRUCT_VERSION_1;
    SVMS_Session session = 0;
    if (api.create_session(&config, &session) != SVMS_RESULT_OK) return 77;
    Sleep(60000);  // the parent terminates this process
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#if defined(SVMS_XP_COMPAT)
    (void)argc; (void)argv;
    std::puts("SKIP: the XP build has no runtime link");
    return 77;
#endif
    if (argc >= 3 && std::strcmp(argv[2], "--host") == 0)
        return RunHost(argv[1]);
    if (argc < 2) return Fail("expected <SVMSAPI.dll path>");

    wchar_t tempDirectory[MAX_PATH] = {};
    wchar_t configPath[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempDirectory) ||
        !GetTempFileNameW(tempDirectory, L"svr", 0, configPath))
        return Fail("could not reserve an isolated config path");
    DeleteFileW(configPath);
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", configPath);
    SetEnvironmentVariableW(L"SVMS_TEST_LOCAL_CONFIG_PATH", configPath);
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS", L"0");
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS_WINDOW", L"0");
    SetEnvironmentVariableW(L"SVMS_DEBUG_OUTPUT", L"0");

    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) return Fail("LoadLibrary failed");
    const auto enumerate = reinterpret_cast<EnumerateProc>(
        GetProcAddress(module, "SVMS_Remote_EnumerateHosts"));
    const auto attach = reinterpret_cast<AttachProc>(
        GetProcAddress(module, "SVMS_Remote_Attach"));
    const auto isAlive = reinterpret_cast<IsAliveProc>(
        GetProcAddress(module, "SVMS_Remote_IsAlive"));
    if (!enumerate || !attach || !isAlive)
        return Fail("remote exports are missing");

    char self[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    // argv[2] (optional) names a different host engine DLL, e.g. winmm.dll.
    const char* hostDll = argc >= 3 ? argv[2] : argv[1];
    std::string commandLine = std::string("\"") + self + "\" \"" + hostDll +
                              "\" --host";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
        return Fail("could not start the host process");

    auto finish = [&](int code) {
        TerminateProcess(child.hProcess, 0);
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        DeleteFileW(configPath);
        return code;
    };

    // 1. Enumeration finds the host by pid.
    SVMS_HostInfo host{};
    bool found = false;
    for (DWORD start = GetTickCount(); !found && GetTickCount() - start < 15000;
         Sleep(100)) {
        DWORD exitCode = STILL_ACTIVE;
        GetExitCodeProcess(child.hProcess, &exitCode);
        if (exitCode != STILL_ACTIVE) {
            std::puts("SKIP: host process could not start an engine");
            return finish(77);
        }
        SVMS_HostInfo hosts[16]{};
        const uint32_t count = enumerate(hosts, 16);
        for (uint32_t i = 0; i < count; ++i) {
            if (hosts[i].pid == child.dwProcessId) {
                host = hosts[i];
                found = true;
            }
        }
    }
    if (!found) return finish(Fail("host was not enumerated"));
    if (host.struct_size != sizeof(host)) return finish(Fail("bad host info"));

    // 2. Attach yields a remote table with the right capabilities.
    SVMS_Interface remote{};
    SVMS_Session session = 0;
    if (attach(0u, sizeof(remote), &remote, &session) !=
            SVMS_RESULT_INVALID_ARGUMENT ||
        attach(child.dwProcessId, sizeof(remote), &remote, &session) !=
            SVMS_RESULT_OK)
        return finish(Fail("attach contract violated"));
    const uint64_t wanted = SVMS_CAP_REMOTE | SVMS_CAP_LIVE_PARAMS |
                            SVMS_CAP_TELEMETRY_V3 | SVMS_CAP_RUNTIME_COMMANDS;
    if ((remote.capabilities & wanted) != wanted || remote.send_short ||
        !remote.get_telemetry_v3 || !remote.get_live_params ||
        !remote.set_live_params || !remote.send_runtime_command ||
        !remote.destroy_session)
        return finish(Fail("remote table is wrong"));

    // 3. Telemetry V3 arrives.
    SVMS_TelemetryV3 t{};
    bool telemetry = false;
    for (DWORD start = GetTickCount(); !telemetry &&
                                       GetTickCount() - start < 10000;
         Sleep(50)) {
        t = SVMS_TelemetryV3{};
        t.struct_size = sizeof(t);
        t.struct_version = SVMS_STRUCT_VERSION_1;
        telemetry = remote.get_telemetry_v3(session, &t) == SVMS_RESULT_OK;
    }
    if (!telemetry || t.census.sample_rate == 0u || t.max_voices == 0u)
        return finish(Fail("no telemetry from the host"));
    SVMS_TelemetryV3 small{};
    small.struct_size = 8u;
    small.struct_version = SVMS_STRUCT_VERSION_1;
    if (remote.get_telemetry_v3(session, &small) !=
        SVMS_RESULT_INVALID_ARGUMENT)
        return finish(Fail("short telemetry struct was accepted"));

    // 4. Live params cross the process boundary and are echoed back applied.
    SVMS_LiveParams live{};
    live.struct_size = sizeof(live);
    live.struct_version = SVMS_STRUCT_VERSION_1;
    if (remote.get_live_params(session, &live) != SVMS_RESULT_OK)
        return finish(Fail("get_live_params failed"));
    live.master_volume = 0.5f;
    live.reverb_enabled = 1u;
    live.reverb_mix = 0.4f;
    if (remote.set_live_params(session, SVMS_LIVE_GROUP_MASTER |
                                        SVMS_LIVE_GROUP_REVERB, &live) !=
        SVMS_RESULT_OK)
        return finish(Fail("set_live_params failed"));
    SVMS_LiveParams applied{};
    applied.struct_size = sizeof(applied);
    applied.struct_version = SVMS_STRUCT_VERSION_1;
    bool echoed = false;
    for (DWORD start = GetTickCount(); !echoed && GetTickCount() - start < 5000;
         Sleep(50)) {
        echoed = remote.get_live_params(session, &applied) == SVMS_RESULT_OK &&
                 std::fabs(applied.master_volume - 0.5f) < 1e-4f &&
                 std::fabs(applied.reverb_mix - 0.4f) < 1e-4f &&
                 applied.reverb_enabled == 1u;
    }
    if (!echoed) return finish(Fail("host never echoed the applied live state"));
    live.master_volume = std::nanf("");
    if (remote.set_live_params(session, SVMS_LIVE_GROUP_MASTER, &live) ==
            SVMS_RESULT_OK ||
        remote.set_live_params(session, 0u, &live) == SVMS_RESULT_OK)
        return finish(Fail("invalid live params were accepted"));

    // 5. Runtime commands: known ok, unknown not ok, errors propagate.
    char text[256] = {};
    if (remote.send_runtime_command(session, SVMS_COMMAND_PING, 0u, text,
                                    sizeof(text)) != SVMS_RESULT_OK ||
        remote.send_runtime_command(session, 0x0000DEADu, 0u, text,
                                    sizeof(text)) == SVMS_RESULT_OK ||
        remote.send_runtime_command(session, SVMS_COMMAND_SET_STEAL_POLICY,
                                    99u, text, sizeof(text)) !=
            SVMS_RESULT_INVALID_ARGUMENT)
        return finish(Fail("runtime command contract violated"));

    // 6. Liveness and detach.
    uint32_t alive = 0u;
    if (isAlive(session, 2000u, &alive) != SVMS_RESULT_OK || alive != 1u)
        return finish(Fail("host reported dead"));
    if (remote.destroy_session(session) != SVMS_RESULT_OK ||
        remote.destroy_session(session) != SVMS_RESULT_NOT_INITIALIZED ||
        remote.get_telemetry_v3(session, &t) != SVMS_RESULT_NOT_INITIALIZED)
        return finish(Fail("detach contract violated"));

    std::puts("SVMS remote client smoke passed");
    return finish(0);
}
