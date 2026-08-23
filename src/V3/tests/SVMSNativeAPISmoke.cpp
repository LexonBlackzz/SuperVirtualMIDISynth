#include <windows.h>

#include "../include/svmsapi.h"

#include <cstdio>

using GetInterfaceProc = SVMS_Result (SVMS_CALL *)(
    uint32_t, uint32_t, SVMS_Interface*);
using InitializeKDMAPIProc = void* (WINAPI*)();
using TerminateKDMAPIProc = void (WINAPI*)();
using SendDirectDataProc = void (WINAPI*)(DWORD);

int main(int argc, char** argv) {
    if (argc != 2) {
        std::puts("FAIL: expected the path to SVMS.dll");
        return 1;
    }
    HMODULE runtime = LoadLibraryA(argv[1]);
    if (!runtime) {
        std::puts("FAIL: could not load the native runtime");
        return 1;
    }
    const auto getInterface = reinterpret_cast<GetInterfaceProc>(
        GetProcAddress(runtime, "SVMS_GetInterface"));
    if (!getInterface) {
        std::puts("FAIL: permanent bootstrap symbol is missing");
        FreeLibrary(runtime);
        return 1;
    }

    SVMS_Interface api{};
    if (getInterface(999u, sizeof(api), &api) !=
            SVMS_RESULT_UNSUPPORTED_ABI ||
        getInterface(SVMS_ABI_VERSION_1, 8u, &api) !=
            SVMS_RESULT_INVALID_ARGUMENT ||
        getInterface(SVMS_ABI_VERSION_1, sizeof(api), &api) !=
            SVMS_RESULT_OK ||
        api.abi_version != SVMS_ABI_VERSION_1 ||
        api.struct_size != sizeof(api) ||
        (api.capabilities & (SVMS_CAP_EXACT_QPC_TIMESTAMPS |
                             SVMS_CAP_SHORT_EVENT_BATCH |
                             SVMS_CAP_SYSTEM_EXCLUSIVE |
                             SVMS_CAP_TELEMETRY_V1)) == 0u ||
        !api.create_session || !api.destroy_session || !api.send_short ||
        !api.send_short_at_qpc || !api.send_short_batch ||
        !api.send_system_exclusive || !api.reset || !api.get_telemetry ||
        !api.get_runtime_clock) {
        std::puts("FAIL: ABI V1 table is invalid");
        FreeLibrary(runtime);
        return 1;
    }

    uint64_t now = 0u, frequency = 0u;
    if (api.get_runtime_clock(&now, &frequency) != SVMS_RESULT_OK ||
        now == 0u || frequency == 0u) {
        std::puts("FAIL: runtime clock is unavailable");
        FreeLibrary(runtime);
        return 1;
    }

    SVMS_SessionConfig config{};
    config.struct_size = sizeof(config);
    config.struct_version = SVMS_STRUCT_VERSION_1;
    SVMS_Session session = 0u;
    const SVMS_Result create = api.create_session(&config, &session);
    if (create != SVMS_RESULT_OK) {
        std::printf("SKIP: native session could not start audio (%u)\n",
                    static_cast<unsigned>(create));
        FreeLibrary(runtime);
        return 77;
    }
    if (session == 0u) {
        std::puts("FAIL: session token is zero");
        FreeLibrary(runtime);
        return 1;
    }

    // The legacy facade must coexist with a native session. Releasing KDMAPI
    // ownership may not stop the engine while the native owner is still live.
    const auto initializeKDMAPI = reinterpret_cast<InitializeKDMAPIProc>(
        GetProcAddress(runtime, "InitializeKDMAPIStream"));
    const auto terminateKDMAPI = reinterpret_cast<TerminateKDMAPIProc>(
        GetProcAddress(runtime, "TerminateKDMAPIStream"));
    const auto sendDirectData = reinterpret_cast<SendDirectDataProc>(
        GetProcAddress(runtime, "SendDirectData"));
    if (!initializeKDMAPI || !terminateKDMAPI || !sendDirectData ||
        !initializeKDMAPI()) {
        std::puts("FAIL: KDMAPI compatibility facade is unavailable");
        api.destroy_session(session);
        FreeLibrary(runtime);
        return 1;
    }
    std::puts("INFO: KDMAPI facade initialized beside native session");
    std::fflush(stdout);
    sendDirectData(0x00643c90u);
    terminateKDMAPI();
    std::puts("INFO: KDMAPI facade released; native session retained");
    std::fflush(stdout);
    if (api.send_short(session, 0x00003c80u) != SVMS_RESULT_OK) {
        std::puts("FAIL: KDMAPI termination stopped an owned native session");
        api.destroy_session(session);
        FreeLibrary(runtime);
        return 1;
    }

    SVMS_ShortEvent events[2]{};
    events[0].timestamp_qpc = now;
    events[0].packed_message = 0x00643c90u;
    events[1].timestamp_qpc = now;
    events[1].packed_message = 0x00003c80u;
    const uint8_t gmReset[] = {0xf0u, 0x7eu, 0x7fu, 0x09u, 0x01u, 0xf7u};
    if (api.send_short_batch(session, events, 2u) != SVMS_RESULT_OK ||
        api.send_system_exclusive(session, gmReset, sizeof(gmReset)) !=
            SVMS_RESULT_OK || api.reset(session) != SVMS_RESULT_OK) {
        std::puts("FAIL: native event submission failed");
        api.destroy_session(session);
        FreeLibrary(runtime);
        return 1;
    }

    SVMS_TelemetryV1 telemetry{};
    telemetry.struct_size = sizeof(telemetry);
    telemetry.struct_version = SVMS_STRUCT_VERSION_1;
    if (api.get_telemetry(session, &telemetry) != SVMS_RESULT_OK ||
        telemetry.struct_size != sizeof(telemetry) ||
        telemetry.sample_rate == 0u) {
        std::puts("FAIL: native telemetry is invalid");
        api.destroy_session(session);
        FreeLibrary(runtime);
        return 1;
    }
    std::puts("INFO: destroying final native session");
    std::fflush(stdout);
    if (api.destroy_session(session) != SVMS_RESULT_OK ||
        api.send_short(session, 0x00643c90u) !=
            SVMS_RESULT_NOT_INITIALIZED ||
        api.destroy_session(session) != SVMS_RESULT_INVALID_ARGUMENT) {
        std::puts("FAIL: stale session was accepted");
        FreeLibrary(runtime);
        return 1;
    }

    FreeLibrary(runtime);
    std::puts("PASS: native SVMS ABI V1 negotiation/session/event smoke");
    return 0;
}
