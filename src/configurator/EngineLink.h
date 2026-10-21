#ifndef SVMS_CONFIGURATOR_ENGINELINK_H
#define SVMS_CONFIGURATOR_ENGINELINK_H

// How the configurator talks to a running engine on Windows: the SVMSAPI
// remote client (SVMS_Remote_*), i.e. an SVMS_Interface table attached to the
// engine's process. The class keeps the surface the pages were written
// against (Open, ReadTelemetry, SendCommand, ...), but nothing here knows the
// transport; the shared-memory protocol is private to frontends/RemoteClient.cpp.
//
// Drivers that describe their own controls (the Linux daemon) also serve a
// schema (SVMS_CAP_SCHEMA); Params()/MetricSchema()/Info() expose it for the
// "unknown to this version" page and are empty on drivers without one.

#include "BuildInfo.h"
#include "driver/LiveParamsConvert.h"
#include "driver/RuntimeLinkProtocol.h"
#include "include/svmsapi.h"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
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
        LoadSchema();
        return true;
    }

    void Close() {
        if (session_ != 0u && table_.destroy_session)
            table_.destroy_session(session_);
        session_ = 0u;
        table_ = SVMS_Interface{};
        params_.clear();
        metricSchema_.clear();
        info_ = nlohmann::json::object();
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

    // ── Driver schema (empty unless the host serves SVMS_CAP_SCHEMA) ──────
    struct ParamInfo {
        std::string id, label, group, unit, type;  // type: bool|int|float|enum
        double min = 0.0, max = 1.0, def = 0.0;
        std::vector<std::string> options;
    };
    struct MetricInfo { std::string id, label, unit; };

    const std::vector<ParamInfo>& Params() const { return params_; }
    const std::vector<MetricInfo>& MetricSchema() const { return metricSchema_; }
    const nlohmann::json& Info() const { return info_; }
    double ParamValue(const std::string& id) const {
        double v = 0.0;
        if (IsOpen() && table_.get_param_value)
            table_.get_param_value(session_, id.c_str(), &v);
        return v;
    }
    double MetricValue(const std::string& id) const {
        double v = 0.0;
        if (IsOpen() && table_.get_metric_value)
            table_.get_metric_value(session_, id.c_str(), &v);
        return v;
    }
    // Fire and forget, like the pages expect; the driver echoes the value.
    void SetParam(const std::string& id, double value) {
        if (IsOpen() && table_.set_param_value)
            table_.set_param_value(session_, id.c_str(), value);
    }
    void Poll() {}  // the remote client pumps inside every call
    // Ids the configurator has dedicated widgets for (the live state and the
    // metrics the pages draw); the rest go to the generic page.
    static bool IsKnownParam(const std::string& id) {
        static const char* const kKnown[] = {
            "master_volume", "correctness_mode", "reverb_enabled", "reverb_mix",
            "reverb_room_size", "reverb_decay", "reverb_damping", "reverb_width",
            "reverb_diffusion", "reverb_predelay_ms", "reverb_early_level",
            "reverb_late_level", "reverb_mod_depth", "reverb_mod_rate",
            "reverb_lowcut_hz", "reverb_highcut_hz", "limiter_enabled",
            "limiter_algorithm", "limiter_threshold", "limiter_lookahead_ms",
            "limiter_attack_ms", "limiter_release_ms", "max_voices"};
        for (const char* known : kKnown)
            if (id == known) return true;
        return false;
    }
    static bool IsKnownMetric(const std::string& id) {
        static const char* const kKnown[] = {
            "events_submitted", "events_accepted", "events_dispatched",
            "active_voices", "max_voices", "free_voices", "voice_steals",
            "sample_rate", "buffer_frames", "render_ms", "queue_size",
            "pending_events"};
        for (const char* known : kKnown)
            if (id == known) return true;
        return false;
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

    void LoadSchema() {
        params_.clear();
        metricSchema_.clear();
        info_ = nlohmann::json::object();
        if (!table_.get_schema_json) return;
        uint32_t bytes = 0u;
        if (table_.get_schema_json(session_, nullptr, &bytes) !=
                SVMS_RESULT_BUFFER_TOO_SMALL || bytes == 0u)
            return;
        std::string text(bytes, '\0');
        if (table_.get_schema_json(session_, text.data(), &bytes) !=
            SVMS_RESULT_OK)
            return;
        const nlohmann::json doc = nlohmann::json::parse(text.c_str(), nullptr, false);
        if (!doc.is_object()) return;
        for (const auto& p : doc.value("params", nlohmann::json::array())) {
            ParamInfo info;
            info.id = p.value("id", std::string());
            info.label = p.value("label", info.id);
            info.group = p.value("group", std::string());
            info.unit = p.value("unit", std::string());
            info.type = p.value("type", std::string("float"));
            info.min = p.value("min", 0.0);
            info.max = p.value("max", 1.0);
            info.def = p.value("default", 0.0);
            if (p.contains("options"))
                info.options = p["options"].get<std::vector<std::string>>();
            params_.push_back(std::move(info));
        }
        for (const auto& m : doc.value("metrics", nlohmann::json::array()))
            metricSchema_.push_back({m.value("id", std::string()),
                                     m.value("label", std::string()),
                                     m.value("unit", std::string())});
        info_ = doc.value("info", nlohmann::json::object());
    }

    SVMS_Interface table_{};
    std::vector<ParamInfo> params_;
    std::vector<MetricInfo> metricSchema_;
    nlohmann::json info_ = nlohmann::json::object();
    SVMS_Session session_ = 0u;
    Protocol protocol_ = Protocol::None;
    HostInfo peer_{};
};

} // namespace svms

#endif
