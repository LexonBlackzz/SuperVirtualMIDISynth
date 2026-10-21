// Cross-process SVMSAPI client (SVMS_CAP_REMOTE): enumerate running SVMS
// hosts and attach to one, as an SVMS_Interface table. The transport is the
// runtime link that already exists per OS (Win32 shared memory / Unix control
// socket); both clients share one C++ surface (configurator/DriverLink.h), so
// this file is OS-neutral and the wire formats stay private to the module.

#include "include/svmsapi.h"

#if defined(SVMS_XP_COMPAT)

// No runtime link in the XP build: the exports exist, but report nothing.
extern "C" {
SVMS_PUBLIC uint32_t SVMS_CALL SVMS_Remote_EnumerateHosts(SVMS_HostInfo*,
                                                          uint32_t) {
    return 0u;
}
SVMS_PUBLIC SVMS_Result SVMS_CALL SVMS_Remote_Attach(uint32_t, uint32_t,
                                                     SVMS_Interface*,
                                                     SVMS_Session*) {
    return SVMS_RESULT_UNSUPPORTED;
}
SVMS_PUBLIC SVMS_Result SVMS_CALL SVMS_Remote_SetTimeout(SVMS_Session,
                                                         uint32_t) {
    return SVMS_RESULT_UNSUPPORTED;
}
SVMS_PUBLIC SVMS_Result SVMS_CALL SVMS_Remote_IsAlive(SVMS_Session, uint32_t,
                                                      uint32_t*) {
    return SVMS_RESULT_UNSUPPORTED;
}
}

#else

#ifdef _WIN32
#include "driver/RuntimeLink.h"
#else
#include "configurator/SocketRuntimeLink.h"
#endif
#include "driver/LiveParamsConvert.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

constexpr uint32_t kCommandTimeoutMs = 1500u;

// strncpy_s is MSVC-only; this file also builds for Linux.
void CopyText(char* dst, size_t capacity, const char* src) {
    if (capacity == 0u) return;
    const size_t n = (std::min)(std::strlen(src), capacity - 1u);
    std::memcpy(dst, src, n);
    dst[n] = 0;
}

struct Remote {
    std::mutex mutex;  // the link clients are single-threaded
    svms::RuntimeLinkClient client;
    std::atomic<uint32_t> timeoutMs{kCommandTimeoutMs};
};

std::mutex g_remotesMutex;
std::map<SVMS_Session, std::shared_ptr<Remote>> g_remotes;
SVMS_Session g_nextSession = 1u;

std::shared_ptr<Remote> FindRemote(SVMS_Session session) {
    std::lock_guard<std::mutex> lock(g_remotesMutex);
    const auto it = g_remotes.find(session);
    return it == g_remotes.end() ? nullptr : it->second;
}


// What the link carries of the V2 census; the rest stays zero (documented in
// svmsapi.h).
void FillCensus(const svms::RuntimeLinkTelemetryV2& t, SVMS_TelemetryV2& c) {
    c.struct_size = sizeof(c);
    c.struct_version = SVMS_STRUCT_VERSION_1;
    c.submitted_events = t.eventsSubmitted;
    c.accepted_events = t.eventsAccepted;
    c.dispatched_events = t.eventsDispatched;
    c.voice_steals = t.voiceSteals;
    c.active_voices = t.activeVoices;
    c.free_voices = t.freeTop;
    c.sample_rate = t.sampleRate;
    c.buffer_frames = t.bufferFrames;
    c.soundfont_loaded = t.soundFontLoaded;
    c.audio_running = t.audioRunning;
    c.render_peak = t.renderPeak;
    c.dropped_events = t.eventsDropped;
    c.scheduled_events = t.scheduledBacklogCount;
    c.over_budget_callbacks = t.overBudgetCallbacks;
    c.max_consecutive_over_budget = t.maxConsecutiveOverBudget;
    c.callback_p95_percent = t.callbackP95Percent;
    c.callback_p99_percent = t.callbackP99Percent;
    c.callback_p999_percent = t.callbackP999Percent;
    c.effective_voices = t.effectiveVoices;
    c.effective_voices_peak = t.effectiveVoicesPeak;
    c.note_merged_events_low = t.noteMergedLow;
    c.note_merged_events_high = t.noteMergedHigh;
}

bool ReadTelemetry(Remote& remote, svms::RuntimeLinkTelemetryV2& t) {
    return remote.client.ReadTelemetry(t) && t.timestampQpc != 0u;
}

SVMS_Result SVMS_CALL RemoteDestroy(SVMS_Session session) {
    std::shared_ptr<Remote> remote;
    {
        std::lock_guard<std::mutex> lock(g_remotesMutex);
        const auto it = g_remotes.find(session);
        if (it == g_remotes.end()) return SVMS_RESULT_NOT_INITIALIZED;
        remote = it->second;
        g_remotes.erase(it);
    }
    std::lock_guard<std::mutex> lock(remote->mutex);
    remote->client.Close();
    return SVMS_RESULT_OK;
}

SVMS_Result SVMS_CALL RemoteGetTelemetryV3(SVMS_Session session,
                                           SVMS_TelemetryV3* out) {
    const std::shared_ptr<Remote> remote = FindRemote(session);
    if (!remote) return SVMS_RESULT_NOT_INITIALIZED;
    if (!out || out->struct_size < sizeof(SVMS_TelemetryV3) ||
        out->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    svms::RuntimeLinkTelemetryV2 t{};
    {
        std::lock_guard<std::mutex> lock(remote->mutex);
        // NOT_INITIALIZED: the host has not published a frame yet (retry).
        if (!ReadTelemetry(*remote, t)) return SVMS_RESULT_NOT_INITIALIZED;
    }
    SVMS_TelemetryV3 r{};
    r.struct_size = sizeof(r);
    r.struct_version = SVMS_STRUCT_VERSION_1;
    FillCensus(t, r.census);
    svms::FillTelemetryV3Extension(t, r);
    *out = r;
    return SVMS_RESULT_OK;
}

SVMS_Result SVMS_CALL RemoteGetLiveParams(SVMS_Session session,
                                          SVMS_LiveParams* out) {
    const std::shared_ptr<Remote> remote = FindRemote(session);
    if (!remote) return SVMS_RESULT_NOT_INITIALIZED;
    if (!out || out->struct_size < sizeof(SVMS_LiveParams) ||
        out->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    svms::RuntimeLinkTelemetryV2 t{};
    {
        std::lock_guard<std::mutex> lock(remote->mutex);
        if (!ReadTelemetry(*remote, t)) return SVMS_RESULT_NOT_INITIALIZED;
    }
    svms::ApiFromLive(t.live, *out);
    out->max_voices = t.maxVoices;
    return SVMS_RESULT_OK;
}

SVMS_Result SVMS_CALL RemoteSetLiveParams(SVMS_Session session,
                                          uint32_t group_mask,
                                          const SVMS_LiveParams* params) {
    const std::shared_ptr<Remote> remote = FindRemote(session);
    if (!remote) return SVMS_RESULT_NOT_INITIALIZED;
    if (!params || params->struct_size < sizeof(SVMS_LiveParams) ||
        params->struct_version != SVMS_STRUCT_VERSION_1 ||
        (group_mask & SVMS_LIVE_GROUP_ALL) == 0u)
        return SVMS_RESULT_INVALID_ARGUMENT;
    svms::RuntimeLiveStateV2 live{};
    svms::LiveFromApi(*params, live);
    char text[svms::kRuntimeLinkResultTextCapacity]{};
    std::lock_guard<std::mutex> lock(remote->mutex);
    return svms::ApiResultFromRL(remote->client.SendCommand(
        svms::RLCommandType::ApplyLiveConfig,
        group_mask & SVMS_LIVE_GROUP_ALL, 0u, live, remote->timeoutMs.load(), text));
}

// The text buffer is in/out, exactly as for an in-process session.
SVMS_Result SVMS_CALL RemoteSendRuntimeCommand(SVMS_Session session,
                                               uint32_t command, uint32_t param,
                                               char* text, uint32_t text_bytes) {
    const std::shared_ptr<Remote> remote = FindRemote(session);
    if (!remote) return SVMS_RESULT_NOT_INITIALIZED;
    if (text_bytes != 0u && !text) return SVMS_RESULT_INVALID_ARGUMENT;
    char request[svms::kRuntimeLinkCommandTextCapacity]{};
    if (text_bytes != 0u) {
        const std::string in(text, strnlen(text, text_bytes));
        CopyText(request, sizeof(request), in.c_str());
    }
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    svms::RLResult rl;
    {
        std::lock_guard<std::mutex> lock(remote->mutex);
        rl = remote->client.SendCommand(
            static_cast<svms::RLCommandType>(command), 0u, param,
            svms::RuntimeLiveStateV2{}, remote->timeoutMs.load(), result,
            request[0] ? request : nullptr);
    }
    if (text_bytes != 0u) CopyText(text, text_bytes, result);
    return svms::ApiResultFromRL(rl);
}

SVMS_Result SVMS_CALL RemoteReset(SVMS_Session session) {
    char text[svms::kRuntimeLinkResultTextCapacity]{};
    return RemoteSendRuntimeCommand(session, SVMS_COMMAND_RESET_VOICES, 0u,
                                    text, sizeof(text));
}

void FillHostInfo(const svms::RuntimeLinkClient::HostInfo& h,
                  SVMS_HostInfo& out) {
    out = SVMS_HostInfo{};
    out.struct_size = sizeof(out);
    out.struct_version = SVMS_STRUCT_VERSION_1;
    out.pid = h.pid;
    out.arch_class = h.archClass;
    out.product_major = h.productMajor;
    out.product_minor = h.productMinor;
    out.product_patch = h.productPatch;
    out.build_number = h.buildNumber;
    out.release_channel = h.releaseChannel;
    out.protocol_min = h.protocolMin;
    out.protocol_max = h.protocolMax;
    out.fresh = h.fresh ? 1u : 0u;
    out.has_version_identity = h.hasVersionIdentity ? 1u : 0u;
    out.capability_flags = h.capabilityFlags;
    out.session_id = h.sessionId;
}

} // namespace

extern "C" {

SVMS_PUBLIC uint32_t SVMS_CALL SVMS_Remote_EnumerateHosts(
    SVMS_HostInfo* out_hosts, uint32_t capacity) {
    if (!out_hosts || capacity == 0u) return 0u;
    svms::RuntimeLinkClient::HostInfo hosts[svms::kRuntimeHostMaxCount]{};
    const uint32_t count = svms::RuntimeLinkClient::EnumerateHosts(
        hosts, (std::min)(capacity, svms::kRuntimeHostMaxCount));
    for (uint32_t i = 0u; i < count; ++i) FillHostInfo(hosts[i], out_hosts[i]);
    return count;
}

SVMS_PUBLIC SVMS_Result SVMS_CALL SVMS_Remote_Attach(
    uint32_t pid, uint32_t out_table_size, SVMS_Interface* out_table,
    SVMS_Session* out_session) {
    if (!out_table || !out_session || out_table_size < 8u || pid == 0u)
        return SVMS_RESULT_INVALID_ARGUMENT;
    auto remote = std::make_shared<Remote>();
    if (!remote->client.Open(pid)) return SVMS_RESULT_NOT_INITIALIZED;

    SVMS_Interface table{};
    table.struct_size = sizeof(table);
    table.struct_version = SVMS_STRUCT_VERSION_1;
    table.abi_version = SVMS_ABI_VERSION_1;
    table.capabilities = SVMS_CAP_REMOTE | SVMS_CAP_RUNTIME_COMMANDS |
                         SVMS_CAP_LIVE_PARAMS | SVMS_CAP_TELEMETRY_V3;
    const auto& peer = remote->client.GetPeerInfo();
    table.product_major = peer.productMajor;
    table.product_minor = peer.productMinor;
    table.product_patch = peer.productPatch;
    table.build_number = peer.buildNumber;
    table.destroy_session = RemoteDestroy;
    table.reset = RemoteReset;
    table.send_runtime_command = RemoteSendRuntimeCommand;
    table.get_telemetry_v3 = RemoteGetTelemetryV3;
    table.get_live_params = RemoteGetLiveParams;
    table.set_live_params = RemoteSetLiveParams;

    SVMS_Session session;
    {
        std::lock_guard<std::mutex> lock(g_remotesMutex);
        session = g_nextSession++;
        g_remotes.emplace(session, std::move(remote));
    }
    std::memcpy(out_table, &table, (std::min)(out_table_size,
                static_cast<uint32_t>(sizeof(table))));
    *out_session = session;
    return SVMS_RESULT_OK;
}

SVMS_PUBLIC SVMS_Result SVMS_CALL SVMS_Remote_SetTimeout(
    SVMS_Session session, uint32_t timeout_ms) {
    const std::shared_ptr<Remote> remote = FindRemote(session);
    if (!remote) return SVMS_RESULT_NOT_INITIALIZED;
    if (timeout_ms == 0u || timeout_ms > 60000u)
        return SVMS_RESULT_INVALID_ARGUMENT;
    remote->timeoutMs.store(timeout_ms);
    return SVMS_RESULT_OK;
}

SVMS_PUBLIC SVMS_Result SVMS_CALL SVMS_Remote_IsAlive(
    SVMS_Session session, uint32_t timeout_ms, uint32_t* out_alive) {
    const std::shared_ptr<Remote> remote = FindRemote(session);
    if (!remote) return SVMS_RESULT_NOT_INITIALIZED;
    if (!out_alive) return SVMS_RESULT_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(remote->mutex);
    *out_alive = remote->client.IsHostAlive(timeout_ms) ? 1u : 0u;
    return SVMS_RESULT_OK;
}

} // extern "C"

#endif // SVMS_XP_COMPAT
