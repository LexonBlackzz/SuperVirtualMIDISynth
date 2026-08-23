#include <windows.h>

#include "../include/svmsapi.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

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

    // Simulate an ABI-1 caller compiled before the optional function-table
    // tail existed. The runtime must report its newer size without writing a
    // byte beyond the older caller's table.
    constexpr uint32_t oldTableSize = static_cast<uint32_t>(
        offsetof(SVMS_Interface, send_timed_short_batch));
    alignas(SVMS_Interface) unsigned char oldStorage[oldTableSize + 16u];
    std::memset(oldStorage, 0, oldTableSize);
    std::memset(oldStorage + oldTableSize, 0xa5, 16u);
    auto* oldTable = reinterpret_cast<SVMS_Interface*>(oldStorage);
    if (getInterface(SVMS_ABI_VERSION_1, oldTableSize, oldTable) !=
            SVMS_RESULT_OK || oldTable->struct_size != sizeof(SVMS_Interface)) {
        std::puts("FAIL: older ABI-1 table negotiation failed");
        FreeLibrary(runtime);
        return 1;
    }
    for (uint32_t i = 0u; i < 16u; ++i) {
        if (oldStorage[oldTableSize + i] != 0xa5u) {
            std::puts("FAIL: ABI table write exceeded caller size");
            FreeLibrary(runtime);
            return 1;
        }
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
                             SVMS_CAP_TELEMETRY_V1 |
                             SVMS_CAP_EXACT_MONOTONIC_NS |
                             SVMS_CAP_EXACT_OUTPUT_FRAMES |
                             SVMS_CAP_QUEUE_CONTROL |
                             SVMS_CAP_SOUNDFONT_RELOAD |
                             SVMS_CAP_MIXED_TIMESTAMP_BATCH)) !=
            (SVMS_CAP_EXACT_QPC_TIMESTAMPS |
             SVMS_CAP_SHORT_EVENT_BATCH | SVMS_CAP_SYSTEM_EXCLUSIVE |
             SVMS_CAP_TELEMETRY_V1 | SVMS_CAP_EXACT_MONOTONIC_NS |
             SVMS_CAP_EXACT_OUTPUT_FRAMES | SVMS_CAP_QUEUE_CONTROL |
             SVMS_CAP_SOUNDFONT_RELOAD | SVMS_CAP_MIXED_TIMESTAMP_BATCH) ||
        !api.create_session || !api.destroy_session || !api.send_short ||
        !api.send_short_at_qpc || !api.send_short_batch ||
        !api.send_system_exclusive || !api.reset || !api.get_telemetry ||
        !api.get_runtime_clock || !api.send_timed_short_batch ||
        !api.get_output_clock || !api.get_monotonic_clock ||
        !api.set_ingress_mode || !api.get_queue_info ||
        !api.load_soundfont_utf8 || !api.panic) {
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

    uint64_t monotonicNs = 0u, outputFrame = 0u;
    uint32_t outputRate = 0u;
    if (api.get_monotonic_clock(&monotonicNs) != SVMS_RESULT_OK ||
        monotonicNs == 0u ||
        api.get_output_clock(session, &outputFrame, &outputRate) !=
            SVMS_RESULT_OK || outputRate == 0u) {
        std::puts("FAIL: portable/output clocks are unavailable");
        api.destroy_session(session);
        FreeLibrary(runtime);
        return 1;
    }

    SVMS_QueueInfo queue{};
    queue.struct_size = sizeof(queue);
    queue.struct_version = SVMS_STRUCT_VERSION_1;
    if (api.set_ingress_mode(session, SVMS_INGRESS_LOSSLESS) !=
            SVMS_RESULT_OK ||
        api.get_queue_info(session, &queue) != SVMS_RESULT_OK ||
        queue.struct_size != sizeof(queue) ||
        queue.ingress_mode != SVMS_INGRESS_LOSSLESS ||
        queue.queue_capacity == 0u ||
        api.set_ingress_mode(session, SVMS_INGRESS_PRIORITY) !=
            SVMS_RESULT_OK) {
        std::puts("FAIL: native queue negotiation failed");
        api.destroy_session(session);
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
    SVMS_TimedShortEvent timed[2]{};
    timed[0].timestamp = outputFrame;
    timed[0].timestamp_domain = SVMS_TIMESTAMP_OUTPUT_FRAME;
    timed[0].packed_message = 0x00643c90u;
    timed[1].timestamp = monotonicNs;
    timed[1].timestamp_domain = SVMS_TIMESTAMP_MONOTONIC_NS;
    timed[1].packed_message = 0x00003c80u;
    if (api.send_short_batch(session, events, 2u) != SVMS_RESULT_OK ||
        api.send_timed_short_batch(session, timed, 2u) != SVMS_RESULT_OK ||
        api.send_system_exclusive(session, gmReset, sizeof(gmReset)) !=
            SVMS_RESULT_OK || api.reset(session) != SVMS_RESULT_OK ||
        api.panic(session) != SVMS_RESULT_OK) {
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
