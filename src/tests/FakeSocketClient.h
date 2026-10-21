// In-memory stand-in for the Linux control-socket client
// (configurator/SocketRuntimeLink.h), with the same public surface that
// frontends/RemoteClient.cpp uses. Compiled in with
// SVMS_REMOTE_USE_FAKE_CLIENT so the schema code - which otherwise only builds
// on Linux - is compiled and exercised on every platform.
#ifndef SVMS_TESTS_FAKE_SOCKET_CLIENT_H
#define SVMS_TESTS_FAKE_SOCKET_CLIENT_H

#include "BuildInfo.h"
#include "driver/RuntimeLinkProtocol.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace svms {

class RuntimeLinkClient {
public:
    static constexpr uint32_t kFakePid = 4242u;

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
    struct ParamInfo {
        std::string id, label, group, unit, type;
        double min = 0.0, max = 1.0, def = 0.0;
        std::vector<std::string> options;
    };
    struct MetricInfo { std::string id, label, unit; };
    struct CommandInfo { std::string name, label; };

    bool Open(uint32_t pid) {
        Close();
        if (pid != kFakePid) return false;
        open_ = true;
        peer_.pid = pid;
        peer_.protocolMin = peer_.protocolMax = 3u;
        peer_.capabilityFlags = build::CapabilityRuntimeLinkV3 |
            build::CapabilityLiveConfiguration | build::CapabilityTelemetry;
        peer_.fresh = true;
        params_ = {
            {"master_volume", "Master volume", "master", "x", "float", 0.0, 4.0, 1.0, {}},
            {"limiter_algorithm", "Limiter algorithm", "limiter", "", "enum", 0.0, 1.0, 0.0,
             {"Classic", "Adaptive"}},
            {"fancy_mode", "Fancy mode", "extras", "", "bool", 0.0, 1.0, 0.0, {}},
        };
        metrics_schema_ = {{"active_voices", "Active voices", ""},
                           {"cache_hits", "Cache hits", "n"}};
        commands_ = {{"reset_voices", "Reset all voices"}};
        values_ = {{"master_volume", 1.0}, {"limiter_algorithm", 0.0},
                   {"fancy_mode", 0.0}};
        metrics_ = {{"active_voices", 7.0}, {"cache_hits", 99.0}};
        info_ = {{"soundfont", "/tmp/fake.sf2"}, {"backend", "fake"}};
        return true;
    }
    void Close() {
        open_ = false;
        peer_ = HostInfo{};
    }
    bool IsOpen() const { return open_; }
    uint32_t GetPID() const { return peer_.pid; }
    Protocol GetProtocol() const { return Protocol::V3; }
    const HostInfo& GetPeerInfo() const { return peer_; }
    bool HasCapability(uint64_t c) const {
        return (peer_.capabilityFlags & c) == c;
    }
    bool ReadTelemetry(RuntimeLinkTelemetryV2& out) {
        if (!open_) return false;
        out = RuntimeLinkTelemetryV2{};
        out.timestampQpc = 1u;
        out.sampleRate = 48000u;
        out.bufferFrames = 512u;
        out.maxVoices = 1024u;
        out.activeVoices = static_cast<uint32_t>(metrics_["active_voices"]);
        out.live.maxVoices = out.maxVoices;
        return true;
    }
    bool IsHostAlive(uint32_t) const { return open_; }
    RLResult SendCommand(RLCommandType type, uint32_t, uint32_t,
                         const RuntimeLiveStateV2&, uint32_t,
                         char* resultText = nullptr, const char* = nullptr) {
        if (resultText) resultText[0] = '\0';
        return open_ && type == RLCommandType::Ping ? RLResult::Ok
                                                    : RLResult::Unsupported;
    }
    static uint32_t EnumerateHosts(HostInfo* out, uint32_t maxCount) {
        if (!out || maxCount == 0u) return 0u;
        HostInfo h;
        h.pid = kFakePid;
        h.protocolMin = h.protocolMax = 3u;
        h.capabilityFlags = build::CapabilityRuntimeLinkV3 |
            build::CapabilityLiveConfiguration | build::CapabilityTelemetry;
        h.fresh = true;
        out[0] = h;
        return 1u;
    }

    // Control API extras.
    const std::vector<ParamInfo>& Params() const { return params_; }
    const std::vector<MetricInfo>& MetricSchema() const { return metrics_schema_; }
    const std::vector<CommandInfo>& Commands() const { return commands_; }
    const nlohmann::json& Info() const { return info_; }
    double ParamValue(const std::string& id) const {
        const auto it = values_.find(id);
        return it == values_.end() ? 0.0 : it->second;
    }
    double MetricValue(const std::string& id) const {
        const auto it = metrics_.find(id);
        return it == metrics_.end() ? 0.0 : it->second;
    }
    bool HasParamId(const std::string& id) const { return values_.count(id) != 0u; }
    bool HasMetricId(const std::string& id) const { return metrics_.count(id) != 0u; }
    RLResult SetParamAck(const std::string& id, double value, uint32_t) {
        for (const ParamInfo& p : params_) {
            if (p.id != id) continue;
            if (value < p.min || value > p.max) return RLResult::InvalidArgument;
            if (id == "fancy_mode") return RLResult::RestartRequired;
            values_[id] = value;
            return RLResult::Ok;
        }
        return RLResult::Unsupported;  // the server answers "unknown"
    }
    RLResult RunCommand(const std::string& name, uint32_t) {
        if (name == "reset_voices") { ++resets; return RLResult::Ok; }
        return RLResult::Unsupported;
    }
    void SetParam(const std::string& id, double value) { values_[id] = value; }
    void Poll() {}

    int resets = 0;

private:
    bool open_ = false;
    HostInfo peer_{};
    std::vector<ParamInfo> params_;
    std::vector<MetricInfo> metrics_schema_;
    std::vector<CommandInfo> commands_;
    std::map<std::string, double> values_;
    mutable std::map<std::string, double> metrics_;
    nlohmann::json info_ = nlohmann::json::object();
};

} // namespace svms

#endif
