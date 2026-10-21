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
    // Sizes the pages need for command text, host tables and liveness.
    static constexpr uint32_t kResultTextBytes = 256u;
    static constexpr uint32_t kRequestTextBytes = 348u;
    static constexpr uint32_t kMaxHosts = 16u;
    static constexpr uint32_t kHostTimeoutMs = 5000u;

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
        HostInfo hosts[kMaxHosts];
        const uint32_t count = EnumerateHosts(hosts, kMaxHosts);
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
        peer_ = HostInfo{};
    }

    bool IsOpen() const { return session_ != 0u; }
    uint32_t GetPID() const { return peer_.pid; }
    const HostInfo& GetPeerInfo() const { return peer_; }
    bool HasCapability(uint64_t capability) const {
        return (peer_.capabilityFlags & capability) == capability;
    }

    bool ReadTelemetry(SVMS_TelemetryV3& out) {
        if (!IsOpen() || !table_.get_telemetry_v3) return false;
        SVMS_TelemetryV3 t{};
        t.struct_size = sizeof(t);
        t.struct_version = SVMS_STRUCT_VERSION_1;
        if (table_.get_telemetry_v3(session_, &t) != SVMS_RESULT_OK)
            return false;
        out = t;
        return true;
    }

    bool IsHostAlive(uint32_t timeoutMs) const {
        if (!IsOpen()) return false;
        uint32_t alive = 0u;
        return SVMS_Remote_IsAlive(session_, timeoutMs, &alive) ==
                   SVMS_RESULT_OK && alive != 0u;
    }

    // One live-control command (an SVMS_COMMAND_* id). `requestText` is the
    // payload for commands that take one (a path, "a;b;c" settings);
    // `resultText` (kResultTextBytes, optional) receives the driver's note.
    SVMS_Result SendCommand(uint32_t command, uint32_t param,
                            uint32_t timeoutMs, char* resultText = nullptr,
                            const char* requestText = nullptr) {
        if (resultText) resultText[0] = '\0';
        if (!IsOpen()) {
            CopyText(resultText, kResultTextBytes, "not connected");
            return SVMS_RESULT_NOT_INITIALIZED;
        }
        SVMS_Remote_SetTimeout(session_, timeoutMs != 0u ? timeoutMs : 1500u);
        char buffer[kRequestTextBytes]{};
        if (requestText) CopyText(buffer, sizeof(buffer), requestText);
        const SVMS_Result result = table_.send_runtime_command(
            session_, command, param, buffer, sizeof(buffer));
        CopyText(resultText, kResultTextBytes, buffer);
        if (resultText && resultText[0] == '\0' && result != SVMS_RESULT_OK)
            CopyText(resultText, kResultTextBytes, ResultString(result));
        return result;
    }

    // Apply the live-state groups in `groupMask` (SVMS_LIVE_GROUP_*).
    SVMS_Result SetLiveParams(uint32_t groupMask, const SVMS_LiveParams& live,
                              uint32_t timeoutMs, char* resultText = nullptr) {
        if (resultText) resultText[0] = '\0';
        if (!IsOpen()) {
            CopyText(resultText, kResultTextBytes, "not connected");
            return SVMS_RESULT_NOT_INITIALIZED;
        }
        SVMS_Remote_SetTimeout(session_, timeoutMs != 0u ? timeoutMs : 1500u);
        const SVMS_Result result =
            table_.set_live_params(session_, groupMask, &live);
        if (result != SVMS_RESULT_OK)
            CopyText(resultText, kResultTextBytes, ResultString(result));
        return result;
    }

    static const char* ResultString(SVMS_Result r) {
        switch (r) {
            case SVMS_RESULT_OK: return "OK";
            case SVMS_RESULT_INVALID_ARGUMENT: return "Invalid argument";
            case SVMS_RESULT_UNSUPPORTED:
            case SVMS_RESULT_UNSUPPORTED_ABI: return "Unsupported";
            case SVMS_RESULT_NOT_INITIALIZED: return "Not connected";
            case SVMS_RESULT_NO_RESOURCES: return "No resources";
            case SVMS_RESULT_BUSY: return "Busy";
            case SVMS_RESULT_RESTART_REQUIRED: return "Restart required";
            case SVMS_RESULT_LOAD_FAILED: return "Load failed";
            default: return "Internal error";
        }
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
    SVMS_Result SendPing(uint32_t timeoutMs, char* resultText = nullptr) {
        return SendCommand(SVMS_COMMAND_PING, 0u, timeoutMs, resultText);
    }

    static uint32_t EnumerateHosts(HostInfo* out, uint32_t maxCount) {
        if (!out || maxCount == 0u) return 0u;
        SVMS_HostInfo hosts[kMaxHosts]{};
        const uint32_t count = SVMS_Remote_EnumerateHosts(
            hosts, (std::min)(maxCount, kMaxHosts));
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
    static void CopyText(char* dst, size_t capacity, const char* src) {
        if (!dst || capacity == 0u) return;
        const size_t n = (std::min)(std::strlen(src), capacity - 1u);
        std::memcpy(dst, src, n);
        dst[n] = 0;
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
    HostInfo peer_{};
};

} // namespace svms

#endif
