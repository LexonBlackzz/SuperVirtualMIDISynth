// EngineLinkSmoke.cpp - the configurator's engine link (EngineLink, on the
// SVMSAPI remote client) against a REAL engine in another process.
//
// Process 1 (this test) spawns Process 2 (itself, --host), which loads
// SVMSAPI.dll and creates a session. Process 1 then drives it exactly as the
// configurator does: enumerate, Open, ReadTelemetry, ApplyLiveConfig,
// commands with and without request text, IsHostAlive, Close.
//
// Skips (exit 77) when the host cannot start (no audio device / no session).
#include <windows.h>

#include "configurator/EngineLink.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using GetInterfaceProc = SVMS_Result (SVMS_CALL*)(uint32_t, uint32_t,
                                                  SVMS_Interface*);

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
        !GetTempFileNameW(tempDirectory, L"sve", 0, configPath))
        return Fail("could not reserve an isolated config path");
    DeleteFileW(configPath);
    SetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH", configPath);
    SetEnvironmentVariableW(L"SVMS_TEST_LOCAL_CONFIG_PATH", configPath);
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS", L"0");
    SetEnvironmentVariableW(L"SVMS_DIAGNOSTICS_WINDOW", L"0");
    SetEnvironmentVariableW(L"SVMS_DEBUG_OUTPUT", L"0");

    char self[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string commandLine = std::string("\"") + self + "\" \"" + argv[1] +
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

    // Discovery, the way ConfiguratorApp::TryAutoDiscoverDriver does it.
    // The remote client lives in this exe: no DLL is loaded here at all.
    svms::EngineLink::HostInfo host{};
    bool found = false;
    for (DWORD start = GetTickCount(); !found && GetTickCount() - start < 15000;
         Sleep(100)) {
        DWORD exitCode = STILL_ACTIVE;
        GetExitCodeProcess(child.hProcess, &exitCode);
        if (exitCode != STILL_ACTIVE) {
            std::puts("SKIP: host process could not start an engine");
            return finish(77);
        }
        svms::EngineLink::HostInfo hosts[svms::EngineLink::kMaxHosts];
        const uint32_t count = svms::EngineLink::EnumerateHosts(
            hosts, svms::EngineLink::kMaxHosts);
        for (uint32_t i = 0; i < count; ++i)
            if (hosts[i].pid == child.dwProcessId && hosts[i].fresh) {
                host = hosts[i];
                found = true;
            }
    }
    if (!found) return finish(Fail("host was not enumerated"));

    svms::EngineLink link;
    if (link.IsOpen() || link.IsHostAlive(100u))
        return finish(Fail("a closed link claims to be open"));
    SVMS_TelemetryV3 t{};
    char text[svms::EngineLink::kResultTextBytes] = {};
    if (link.ReadTelemetry(t) ||
        link.SendCommand(SVMS_COMMAND_PING, 0u, 100u, text) == SVMS_RESULT_OK)
        return finish(Fail("a closed link accepted traffic"));
    if (!link.Open(child.dwProcessId) || !link.IsOpen() ||
        link.GetPID() != child.dwProcessId)
        return finish(Fail("Open failed"));
    if (!link.HasCapability(svms::build::CapabilityLiveConfiguration) ||
        !link.HasCapability(svms::build::CapabilityTelemetry))
        return finish(Fail("host lacks live configuration or telemetry"));

    // Telemetry frames carry what the pages draw.
    bool telemetry = false;
    for (DWORD start = GetTickCount(); !telemetry &&
                                       GetTickCount() - start < 10000;
         Sleep(50))
        telemetry = link.ReadTelemetry(t) && t.census.sample_rate != 0u &&
                    t.max_voices != 0u;
    if (!telemetry) return finish(Fail("no telemetry from the host"));
    if (t.live.max_voices != t.max_voices || t.census.buffer_frames == 0u)
        return finish(Fail("telemetry frame is inconsistent"));

    // Live params round trip, like ConfiguratorApp::FlushLiveChanges.
    SVMS_LiveParams live = t.live;
    live.master_volume = 0.5f;
    live.reverb_enabled = 1u;
    live.reverb_mix = 0.4f;
    if (link.SetLiveParams(SVMS_LIVE_GROUP_MASTER | SVMS_LIVE_GROUP_REVERB,
                           live, 500u, text) != SVMS_RESULT_OK)
        return finish(Fail("set live params failed"));
    bool echoed = false;
    for (DWORD start = GetTickCount(); !echoed && GetTickCount() - start < 5000;
         Sleep(50))
        echoed = link.ReadTelemetry(t) &&
                 std::fabs(t.live.master_volume - 0.5f) < 1e-4f &&
                 std::fabs(t.live.reverb_mix - 0.4f) < 1e-4f &&
                 t.live.reverb_enabled == 1u;
    if (!echoed) return finish(Fail("host never echoed the applied state"));
    live.master_volume = std::nanf("");
    if (link.SetLiveParams(SVMS_LIVE_GROUP_MASTER, live, 500u, text) ==
            SVMS_RESULT_OK ||
        text[0] == '\0')
        return finish(Fail("invalid live state was accepted or gave no text"));

    // Commands: plain parameter, rejected parameter, request text in and
    // result text out (the SoundFont path travels as request text).
    if (link.SendCommand(SVMS_COMMAND_PING, 0u, 500u, text) != SVMS_RESULT_OK ||
        link.SendCommand(SVMS_COMMAND_SET_STEAL_POLICY, 99u, 500u, text) ==
            SVMS_RESULT_OK)
        return finish(Fail("plain command contract violated"));
    text[0] = '\0';
    link.SendCommand(SVMS_COMMAND_QUERY_SOUNDFONT_LOAD, 0u, 100u, text);
    if (text[0] == '\0') return finish(Fail("no result text from a query"));
    if (link.SendCommand(SVMS_COMMAND_RELOAD_SOUNDFONT, 0u, 500u, text,
                         "C:\definitely\not\a\soundfont.sf2") ==
        SVMS_RESULT_OK) {
        // Accepted for queueing is fine; the load itself then fails.
        for (DWORD start = GetTickCount(); GetTickCount() - start < 3000;
             Sleep(50)) {
            link.SendCommand(SVMS_COMMAND_QUERY_SOUNDFONT_LOAD, 0u, 100u, text);
            if (std::strstr(text, "fail") || std::strstr(text, "Fail") ||
                std::strstr(text, "error") || std::strstr(text, "4\t"))
                break;
        }
    }

    // Liveness, then Close.
    if (!link.IsHostAlive(2000u)) return finish(Fail("host reported dead"));
    link.Close();
    if (link.IsOpen() || link.ReadTelemetry(t))
        return finish(Fail("Close left the link open"));
    // Reconnect after close works (the configurator's auto-reconnect).
    if (!link.Open(child.dwProcessId) || !link.IsHostAlive(2000u))
        return finish(Fail("reopen after close failed"));
    link.Close();

    std::puts("SVMS engine link smoke passed");
    return finish(0);
}
