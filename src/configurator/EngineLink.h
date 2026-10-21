#ifndef SVMS_CONFIGURATOR_ENGINELINK_H
#define SVMS_CONFIGURATOR_ENGINELINK_H

// How the configurator talks to a running engine on Windows: the SVMSAPI
// remote client (SVMS_Remote_*), i.e. an SVMS_Interface table attached to the
// engine's process. The class keeps the surface the pages were written
// against (Open, ReadTelemetry, SendCommand, ...), but nothing here knows the
// transport; the shared-memory protocol is private to frontends/RemoteClient.cpp.
//
// Linux still uses the Control API socket client (see DriverLink.h) until the
// native API can describe a driver's parameters.

#include "BuildInfo.h"
#include "driver/LiveParamsConvert.h"
#include "driver/RuntimeLinkProtocol.h"
#include "include/svmsapi.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace svms {

class EngineLink {
public:
    enum class Protocol : uint32_t { None = 0u, V2 = 2u, V3 = 3u };

    struct HostInfo {
        uint32_t pid = 0u;
        uint32_t archClass = 0u;
        uint32_t productMajor = 0u;
        uint32_t productMinor = 0u;
        uint32_t productPatch = 0u;
        uint32_t buildNumber = 0u;
        uint32_t releaseChannel = 0u;
        uint32_t protocolMin = 0u;
        uint32_t protocolMax = 0u;
        uint64_t capabilityFlags = 0u;
        uint64_t sessionId = 0u;
        bool fresh = false;
        bool hasVersionIdentity = false;
    };

    EngineLink() = default;
    ~EngineLink() { Close(); }
    EngineLink(const EngineLink&) = delete;
    EngineLink& operator=(const EngineLink&) = delete;

    bool Open(uint32_t pid) {
        Close();
        HostInfo found{};
        found.pid = pid;
        found.protocolMin = found.protocolMax = 3u;
        found.capabilityFlags = build::CapabilityRuntimeLinkV3 |
            build::CapabilityLiveConfiguration | build::CapabilityTelemetry;
        found.fresh = true;
        HostInfo hosts[kRuntimeHostMaxCount];
        const uint32_t count = EnumerateHosts(hosts, kRuntimeHostMaxCount);
        for (uint32_t i = 0u; i < count; ++i)
            if (hosts[i].pid == pid) found = hosts[i];

        SVMS_Interface table{};
        SVMS_Session session = 0u;
        if (SVMS_Remote_Attach(pid, sizeof(table), &table, &session) !=
            SVMS_RESULT_OK)
            return false;
        table_ = table;
        session_ = session;
        peer_ = found;
        protocol_ = found.protocolMax == 2u ? Protocol::V2 : Protocol::V3;
        return true;
    }

    void Close() {
        if (session_ != 0u && table_.destroy_session)
            table_.destroy_session(session_);
        session_ = 0u;
        table_ = SVMS_Interface{};
        protocol_ = Protocol::None;
        peer_ = HostInfo{};
    }

    bool IsOpen() const { return session_ != 0u; }
    uint32_t GetPID() const { return peer_.pid; }
    Protocol GetProtocol() const { return protocol_; }
    const HostInfo& GetPeerInfo() const { return peer_; }
    bool HasCapability(uint64_t capability) const {
        return (peer_.capabilityFlags & capability) == capability;
    }

    bool ReadTelemetry(RuntimeLinkTelemetryV2& out) {
        if (!IsOpen() || !table_.get_telemetry_v3) return false;
        SVMS_TelemetryV3 t{};
        t.struct_size = sizeof(t);
        t.struct_version = SVMS_STRUCT_VERSION_1;
        if (table_.get_telemetry_v3(session_, &t) != SVMS_RESULT_OK)
            return false;
        out = FromApi(t);
        return true;
    }

    bool IsHostAlive(uint32_t timeoutMs) const {
        if (!IsOpen()) return false;
        uint32_t alive = 0u;
        return SVMS_Remote_IsAlive(session_, timeoutMs, &alive) ==
                   SVMS_RESULT_OK && alive != 0u;
    }

    RLResult SendCommand(RLCommandType type, uint32_t groupMask,
                         uint32_t param, const RuntimeLiveStateV2& live,
                         uint32_t timeoutMs,
                         char resultText[kRuntimeLinkResultTextCapacity] =
                             nullptr,
                         const char* requestText = nullptr) {
        auto text = [&](const char* message) {
            if (!resultText) return;
            const size_t n = (std::min)(std::strlen(message),
                static_cast<size_t>(kRuntimeLinkResultTextCapacity) - 1u);
            std::memcpy(resultText, message, n);
            resultText[n] = '\0';
        };
        if (resultText) resultText[0] = '\0';
        if (!IsOpen()) {
            text("not connected");
            return RLResult::InternalError;
        }
        SVMS_Remote_SetTimeout(session_, timeoutMs != 0u ? timeoutMs : 1500u);

        SVMS_Result result;
        if (type == RLCommandType::ApplyLiveConfig) {
            SVMS_LiveParams params{};
            ApiFromLive(live, params);
            result = table_.set_live_params(session_, groupMask, &params);
        } else {
            char buffer[kRuntimeLinkCommandTextCapacity]{};
            if (requestText) {
                const size_t n = (std::min)(std::strlen(requestText),
                                            sizeof(buffer) - 1u);
                std::memcpy(buffer, requestText, n);
            }
            result = table_.send_runtime_command(
                session_, static_cast<uint32_t>(type), param, buffer,
                sizeof(buffer));
            text(buffer);
        }
        const RLResult rl = RLFromApiResult(result);
        if (resultText && resultText[0] == '\0' && rl != RLResult::Ok)
            text(RLV2_ResultToString(rl));
        return rl;
    }

    static uint32_t EnumerateHosts(HostInfo* out, uint32_t maxCount) {
        if (!out || maxCount == 0u) return 0u;
        SVMS_HostInfo hosts[kRuntimeHostMaxCount]{};
        const uint32_t count = SVMS_Remote_EnumerateHosts(
            hosts, (std::min)(maxCount, kRuntimeHostMaxCount));
        for (uint32_t i = 0u; i < count; ++i) {
            const SVMS_HostInfo& h = hosts[i];
            HostInfo info{};
            info.pid = h.pid;
            info.archClass = h.arch_class;
            info.productMajor = h.product_major;
            info.productMinor = h.product_minor;
            info.productPatch = h.product_patch;
            info.buildNumber = h.build_number;
            info.releaseChannel = h.release_channel;
            info.protocolMin = h.protocol_min;
            info.protocolMax = h.protocol_max;
            info.capabilityFlags = h.capability_flags;
            info.sessionId = h.session_id;
            info.fresh = h.fresh != 0u;
            info.hasVersionIdentity = h.has_version_identity != 0u;
            out[i] = info;
        }
        return count;
    }

private:
    // SVMS_TelemetryV3 -> the struct the pages draw from. The link has no
    // publish timestamp of its own; "now" is when this frame was read.
    static RuntimeLinkTelemetryV2 FromApi(const SVMS_TelemetryV3& v) {
        RuntimeLinkTelemetryV2 t{};
        const SVMS_TelemetryV2& c = v.census;
        t.timestampQpc = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        t.overBudgetCallbacks = c.over_budget_callbacks;
        t.eventsSubmitted = c.submitted_events;
        t.eventsAccepted = c.accepted_events;
        t.eventsDropped = c.dropped_events;
        t.eventsDispatched = c.dispatched_events;
        t.activeVoices = c.active_voices;
        t.maxVoices = v.max_voices;
        t.releasingVoices = v.releasing_voices;
        t.freeTop = v.free_top;
        t.voiceSteals = static_cast<uint32_t>(c.voice_steals);
        t.retiredCount = v.retired_count;
        t.retiredImmediateCount = v.retired_immediate_count;
        t.decimationStep = v.decimation_step;
        t.sampleRate = c.sample_rate;
        t.bufferFrames = c.buffer_frames;
        t.audioRunning = c.audio_running;
        t.soundFontLoaded = c.soundfont_loaded;
        t.audioHResult = v.audio_hresult;
        t.renderPeak = c.render_peak;
        t.cpuLoadPercent = v.cpu_load_percent;
        t.callbackP95Percent = c.callback_p95_percent;
        t.callbackP99Percent = c.callback_p99_percent;
        t.callbackP999Percent = c.callback_p999_percent;
        t.maxConsecutiveOverBudget = c.max_consecutive_over_budget;
        t.limiterInputPeakL = v.limiter_input_peak_l;
        t.limiterInputPeakR = v.limiter_input_peak_r;
        t.limiterOutputPeakL = v.limiter_output_peak_l;
        t.limiterOutputPeakR = v.limiter_output_peak_r;
        t.limiterGainReductionDb = v.limiter_gain_reduction_db;
        LiveFromApi(v.live, t.live);
        t.live.maxVoices = v.max_voices;
        std::memcpy(t.soundFontName, v.soundfont_name,
                    sizeof(t.soundFontName));
        t.schedulerPercent = v.scheduler_percent;
        t.eventDispatchPercent = v.event_dispatch_percent;
        t.rawIngressCount = v.raw_ingress_count;
        t.compiledPagedCount = v.compiled_paged_count;
        t.scheduledBacklogCount = v.scheduled_backlog_count;
        t.channelLimiterEnabled = v.channel_limiter_enabled;
        std::memcpy(t.channelLimiterGainReductionDb,
                    v.channel_limiter_gain_reduction_db,
                    sizeof(t.channelLimiterGainReductionDb));
        std::memcpy(t.channelLimiterInputPeak, v.channel_limiter_input_peak,
                    sizeof(t.channelLimiterInputPeak));
        t.effectiveVoices = c.effective_voices;
        t.effectiveVoicesPeak = c.effective_voices_peak;
        t.noteMergedLow = c.note_merged_events_low;
        t.noteMergedHigh = c.note_merged_events_high;
        return t;
    }

    SVMS_Interface table_{};
    SVMS_Session session_ = 0u;
    Protocol protocol_ = Protocol::None;
    HostInfo peer_{};
};

} // namespace svms

#endif
