#ifndef SVMS_CONFIGURATOR_SOCKETRUNTIMELINK_H
#define SVMS_CONFIGURATOR_SOCKETRUNTIMELINK_H

// Linux stand-in for svms::RuntimeLinkClient (SVMSRuntimeLink.h is Win32
// shared memory). Same public surface, so the configurator pages compile
// unchanged, but it speaks the Control API (SVMSControlRegistry.h) over the
// driver's Unix socket.
//
// The driver describes itself in its hello message. Anything the configurator
// has a fixed widget for is mapped onto the RuntimeLiveStateV2 / telemetry
// structs the pages already use; everything else is exposed through Params()
// / SetParam() so the "unknown to this version" page can show it.

#include "driver/SVMSRuntimeLinkProtocol.h"
#include "driver/SVMSControlServer.h"
#include "SVMSBuildInfo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace svms {

class RuntimeLinkClient {
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

    // One driver-advertised parameter (Control API schema entry).
    struct ParamInfo {
        std::string id, label, group, unit, type;  // type: bool|int|float|enum
        double min = 0.0, max = 1.0, def = 0.0;
        std::vector<std::string> options;
    };
    struct MetricInfo { std::string id, label, unit; };

    ~RuntimeLinkClient() { Close(); }

    bool Open(uint32_t pid) {
        Close();
        const int fd = Connect(pid);
        if (fd < 0) return false;
        fd_ = fd;
        // The hello message is queued by the server on accept.
        if (!WaitForHello(750)) { Close(); return false; }
        SendLine(nlohmann::json{{"t", "subscribe"}, {"hz", 30}}.dump());
        protocol_ = Protocol::V3;
        peer_.pid = pid;
        peer_.protocolMin = peer_.protocolMax = 3u;
        peer_.capabilityFlags = build::CapabilityRuntimeLinkV3 |
            build::CapabilityLiveConfiguration | build::CapabilityTelemetry;
        peer_.fresh = true;
        return true;
    }

    void Close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        protocol_ = Protocol::None;
        peer_ = HostInfo{};
        inbox_.clear();
        params_.clear();
        metricSchema_.clear();
        commands_.clear();
        values_.clear();
        metrics_.clear();
        info_ = nlohmann::json::object();
        acks_.clear();
        haveMetrics_ = false;
    }

    bool IsOpen() const { return protocol_ != Protocol::None; }
    uint32_t GetPID() const { return peer_.pid; }
    Protocol GetProtocol() const { return protocol_; }
    const HostInfo& GetPeerInfo() const { return peer_; }
    bool HasCapability(uint64_t capability) const {
        return (peer_.capabilityFlags & capability) == capability;
    }

    bool ReadTelemetry(RuntimeLinkTelemetryV2& out) {
        if (!IsOpen()) return false;
        Pump();
        if (!haveMetrics_) return false;
        out = RuntimeLinkTelemetryV2{};
        const auto metric = [&](const char* id) {
            const auto it = metrics_.find(id);
            return it == metrics_.end() ? 0.0 : it->second;
        };
        out.timestampQpc = NowTicks();
        out.eventsSubmitted = uint64_t(metric("events_submitted"));
        out.eventsAccepted = uint64_t(metric("events_accepted"));
        out.eventsDispatched = uint64_t(metric("events_dispatched"));
        out.activeVoices = uint32_t(metric("active_voices"));
        out.maxVoices = uint32_t(metric("max_voices"));
        out.freeTop = uint32_t(metric("free_voices"));
        out.voiceSteals = uint32_t(metric("voice_steals"));
        out.sampleRate = uint32_t(metric("sample_rate"));
        out.bufferFrames = uint32_t(metric("buffer_frames"));
        out.audioRunning = 1u;
        out.soundFontLoaded = 1u;
        const double budgetMs = out.sampleRate
            ? 1000.0 * double(out.bufferFrames) / double(out.sampleRate) : 0.0;
        const float load = budgetMs > 0.0 ? float(100.0 * metric("render_ms") / budgetMs) : 0.0f;
        out.cpuLoadPercent = load;
        out.callbackP95Percent = load;
        out.rawIngressCount = uint32_t(metric("queue_size"));
        out.scheduledBacklogCount = uint32_t(metric("pending_events"));
        out.effectiveVoices = out.activeVoices;
        const auto soundfont = info_.value("soundfont", std::string());
        const size_t slash = soundfont.find_last_of('/');
        std::snprintf(out.soundFontName, sizeof(out.soundFontName), "%s",
                      soundfont.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
        out.live = LiveFromValues();
        return true;
    }

    bool IsHostAlive(uint32_t timeoutMs) const {
        if (!IsOpen()) return false;
        const_cast<RuntimeLinkClient*>(this)->Pump();
        if (fd_ < 0) return false;
        return NowMs() - lastRxMs_ < double(timeoutMs);
    }

    RLResult SendCommand(RLCommandType type, uint32_t groupMask, uint32_t param,
                         const RuntimeLiveStateV2& live, uint32_t timeoutMs,
                         char resultText[kRuntimeLinkResultTextCapacity] = nullptr,
                         const char* requestText = nullptr) {
        (void)param; (void)requestText;
        auto text = [&](const char* message) {
            if (resultText) std::snprintf(resultText, kRuntimeLinkResultTextCapacity, "%s", message);
        };
        if (!IsOpen()) { text("not connected"); return RLResult::InternalError; }
        switch (type) {
        case RLCommandType::Ping:
            return RLResult::Ok;
        case RLCommandType::ApplyLiveConfig: {
            bool failed = false;
            uint32_t missing = 0u;
            for (const LiveField& f : LiveFields(live)) {
                if (!(groupMask & f.group)) continue;
                if (!HasParam(f.id)) { missing |= f.group; continue; }
                if (SetParamBlocking(f.id, f.value, timeoutMs) != RLResult::Ok) failed = true;
            }
            if (failed) return RLResult::InternalError;
            if (missing) {
                // Supported groups were applied above; name what this driver cannot change live.
                std::string names;
                const auto add = [&](uint32_t g, const char* n) {
                    if (missing & g) names += (names.empty() ? "" : ", ") + std::string(n);
                };
                add(RLGroupMaster, "master volume"); add(RLGroupCorrectness, "correctness");
                add(RLGroupReverb, "reverb"); add(RLGroupLimiter, "limiter");
                add(RLGroupVoices, "voice count");
                text((names + " can't be changed live on this driver; save the configuration and restart it").c_str());
                return RLResult::Unsupported;
            }
            return RLResult::Ok;
        }
        case RLCommandType::ResetVoices: {
            const bool ok = CommandBlocking("reset_voices", timeoutMs) == RLResult::Ok;
            return ok ? RLResult::Ok : RLResult::InternalError;
        }
        default:
            text("not available on this driver");
            return RLResult::Unsupported;
        }
    }

    static uint32_t EnumerateHosts(HostInfo* out, uint32_t maxCount) {
        if (!out || maxCount == 0u) return 0u;
        const std::string dir = ctl::SocketDirectory();
        DIR* directory = opendir(dir.c_str());
        if (!directory) return 0u;
        uint32_t count = 0u;
        while (dirent* entry = readdir(directory)) {
            unsigned pid = 0u;
            if (std::sscanf(entry->d_name, "svms-%u.sock", &pid) != 1 || count >= maxCount) continue;
            RuntimeLinkClient probe;
            if (!probe.Open(pid)) {
                // Connection refused = the driver is gone; clean up its socket.
                if (errno == ECONNREFUSED) unlink((dir + "/" + entry->d_name).c_str());
                continue;
            }
            out[count++] = probe.peer_;
        }
        closedir(directory);
        return count;
    }

    // ── Control API extras (not part of the Windows client) ─────────────
    const std::vector<ParamInfo>& Params() const { return params_; }
    const std::vector<MetricInfo>& MetricSchema() const { return metricSchema_; }
    double ParamValue(const std::string& id) const {
        const auto it = values_.find(id);
        return it == values_.end() ? 0.0 : it->second;
    }
    double MetricValue(const std::string& id) const {
        const auto it = metrics_.find(id);
        return it == metrics_.end() ? 0.0 : it->second;
    }
    const nlohmann::json& Info() const { return info_; }
    // Fire and forget; the driver echoes the applied value back to everyone.
    void SetParam(const std::string& id, double value) {
        SendLine(nlohmann::json{{"t", "set"}, {"id", id}, {"v", value}}.dump());
    }
    void Poll() { if (IsOpen()) Pump(); }
    // True for ids the configurator has dedicated widgets for (RuntimeLiveStateV2 /
    // telemetry mapping above); everything else is shown generically.
    static bool IsKnownParam(const std::string& id) {
        for (const LiveField& f : LiveFields(RuntimeLiveStateV2{}))
            if (id == f.id) return true;
        return false;
    }
    static bool IsKnownMetric(const std::string& id) {
        static const char* const kKnown[] = {
            "events_submitted", "events_accepted", "events_dispatched", "active_voices",
            "max_voices", "free_voices", "voice_steals", "sample_rate", "buffer_frames",
            "render_ms", "queue_size", "pending_events"};
        for (const char* known : kKnown)
            if (id == known) return true;
        return false;
    }

private:
    struct LiveField { const char* id; uint32_t group; double value; };

    static std::vector<LiveField> LiveFields(const RuntimeLiveStateV2& s) {
        return {
            {"master_volume", RLGroupMaster, s.masterVolume},
            {"correctness_mode", RLGroupCorrectness, double(s.correctnessMode)},
            {"reverb_enabled", RLGroupReverb, double(s.reverbEnabled)},
            {"reverb_mix", RLGroupReverb, s.reverbMix},
            {"reverb_room_size", RLGroupReverb, s.reverbRoomSize},
            {"reverb_decay", RLGroupReverb, s.reverbDecay},
            {"reverb_damping", RLGroupReverb, s.reverbDamping},
            {"reverb_width", RLGroupReverb, s.reverbWidth},
            {"reverb_diffusion", RLGroupReverb, s.reverbDiffusion},
            {"reverb_predelay_ms", RLGroupReverb, s.reverbPreDelayMs},
            {"reverb_early_level", RLGroupReverb, s.reverbEarlyLevel},
            {"reverb_late_level", RLGroupReverb, s.reverbLateLevel},
            {"reverb_mod_depth", RLGroupReverb, s.reverbModDepth},
            {"reverb_mod_rate", RLGroupReverb, s.reverbModRate},
            {"reverb_lowcut_hz", RLGroupReverb, s.reverbLowCutHz},
            {"reverb_highcut_hz", RLGroupReverb, s.reverbHighCutHz},
            {"limiter_enabled", RLGroupLimiter, double(s.limiterEnabled)},
            {"limiter_algorithm", RLGroupLimiter, double(s.limiterAlgorithm)},
            {"limiter_threshold", RLGroupLimiter, s.limiterThreshold},
            {"limiter_lookahead_ms", RLGroupLimiter, s.limiterLookaheadMs},
            {"limiter_attack_ms", RLGroupLimiter, s.limiterAttackMs},
            {"limiter_release_ms", RLGroupLimiter, s.limiterReleaseMs},
            {"max_voices", RLGroupVoices, double(s.maxVoices)},
        };
    }

    RuntimeLiveStateV2 LiveFromValues() const {
        RuntimeLiveStateV2 s;
        const auto v = [&](const char* id, double fallback) {
            const auto it = values_.find(id);
            return it == values_.end() ? fallback : it->second;
        };
        s.masterVolume = float(v("master_volume", s.masterVolume));
        s.correctnessMode = uint32_t(v("correctness_mode", s.correctnessMode));
        s.reverbEnabled = uint32_t(v("reverb_enabled", s.reverbEnabled));
        s.reverbMix = float(v("reverb_mix", s.reverbMix));
        s.reverbRoomSize = float(v("reverb_room_size", s.reverbRoomSize));
        s.reverbDecay = float(v("reverb_decay", s.reverbDecay));
        s.reverbDamping = float(v("reverb_damping", s.reverbDamping));
        s.reverbWidth = float(v("reverb_width", s.reverbWidth));
        s.reverbDiffusion = float(v("reverb_diffusion", s.reverbDiffusion));
        s.reverbPreDelayMs = float(v("reverb_predelay_ms", s.reverbPreDelayMs));
        s.reverbEarlyLevel = float(v("reverb_early_level", s.reverbEarlyLevel));
        s.reverbLateLevel = float(v("reverb_late_level", s.reverbLateLevel));
        s.reverbModDepth = float(v("reverb_mod_depth", s.reverbModDepth));
        s.reverbModRate = float(v("reverb_mod_rate", s.reverbModRate));
        s.reverbLowCutHz = float(v("reverb_lowcut_hz", s.reverbLowCutHz));
        s.reverbHighCutHz = float(v("reverb_highcut_hz", s.reverbHighCutHz));
        s.limiterEnabled = uint32_t(v("limiter_enabled", s.limiterEnabled));
        s.limiterAlgorithm = uint32_t(v("limiter_algorithm", s.limiterAlgorithm));
        s.limiterThreshold = float(v("limiter_threshold", s.limiterThreshold));
        s.limiterLookaheadMs = float(v("limiter_lookahead_ms", s.limiterLookaheadMs));
        s.limiterAttackMs = float(v("limiter_attack_ms", s.limiterAttackMs));
        s.limiterReleaseMs = float(v("limiter_release_ms", s.limiterReleaseMs));
        s.maxVoices = uint32_t(v("max_voices", s.maxVoices));
        return s;
    }

    bool HasParam(const char* id) const {
        for (const ParamInfo& p : params_) if (p.id == id) return true;
        return false;
    }

    static double NowMs() {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static uint64_t NowTicks() { return uint64_t(NowMs() * 1000.0); }

    static int Connect(uint32_t pid) {
        const std::string path = ctl::SocketDirectory() + "/svms-" + std::to_string(pid) + ".sock";
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) return -1;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1u);
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            const int saved = errno;
            ::close(fd);
            errno = saved;
            return -1;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        return fd;
    }

    bool WaitForHello(int timeoutMs) {
        const double deadline = NowMs() + timeoutMs;
        while (NowMs() < deadline) {
            pollfd p{fd_, POLLIN, 0};
            poll(&p, 1, 50);
            Pump();
            if (fd_ < 0) return false;
            if (helloSeen_) { helloSeen_ = false; return true; }
        }
        return false;
    }

    void SendLine(const std::string& line) {
        if (fd_ < 0) return;
        const std::string data = line + "\n";
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n > 0) { sent += size_t(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) {  // tiny commands: wait briefly for buffer space
                pollfd p{fd_, POLLOUT, 0};
                poll(&p, 1, 20);
                continue;
            }
            Close();
            return;
        }
    }

    // Reads everything pending and applies each complete line.
    void Pump() {
        while (fd_ >= 0) {
            char buffer[4096];
            const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (n > 0) { inbox_.append(buffer, size_t(n)); continue; }
            if (n == 0) { Close(); return; }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) { Close(); return; }
            break;
        }
        size_t nl;
        while ((nl = inbox_.find('\n')) != std::string::npos) {
            const std::string line = inbox_.substr(0, nl);
            inbox_.erase(0, nl + 1u);
            Apply(nlohmann::json::parse(line, nullptr, false));
        }
    }

    void Apply(const nlohmann::json& msg) {
        if (!msg.is_object()) return;
        lastRxMs_ = NowMs();
        const std::string t = msg.value("t", std::string());
        if (t == "hello") {
            params_.clear();
            for (const auto& p : msg.value("params", nlohmann::json::array())) {
                ParamInfo info;
                info.id = p.value("id", std::string());
                info.label = p.value("label", info.id);
                info.group = p.value("group", std::string());
                info.unit = p.value("unit", std::string());
                info.type = p.value("type", std::string("float"));
                info.min = p.value("min", 0.0);
                info.max = p.value("max", 1.0);
                info.def = p.value("default", 0.0);
                if (p.contains("options")) info.options = p["options"].get<std::vector<std::string>>();
                params_.push_back(std::move(info));
            }
            metricSchema_.clear();
            for (const auto& m : msg.value("metrics", nlohmann::json::array()))
                metricSchema_.push_back({m.value("id", std::string()),
                                         m.value("label", std::string()),
                                         m.value("unit", std::string())});
            info_ = msg.value("info", nlohmann::json::object());
            ApplyValues(msg.value("values", nlohmann::json::object()));
            unsigned a = 0, b = 0, c = 0;
            if (std::sscanf(msg.value("version", std::string()).c_str(), "%u.%u.%u", &a, &b, &c) >= 2) {
                peer_.productMajor = a; peer_.productMinor = b; peer_.productPatch = c;
                peer_.hasVersionIdentity = true;
            }
            helloSeen_ = true;
        } else if (t == "values") {
            ApplyValues(msg.value("values", nlohmann::json::object()));
        } else if (t == "metrics") {
            for (auto it = msg["m"].begin(); it != msg["m"].end(); ++it)
                if (it.value().is_number()) metrics_[it.key()] = it.value().get<double>();
            haveMetrics_ = true;
        } else if (t == "ack" && msg.contains("rid") && msg["rid"].is_number_integer()) {
            acks_[msg["rid"].get<int>()] = msg.value("result", std::string());
        }
    }

    void ApplyValues(const nlohmann::json& values) {
        for (auto it = values.begin(); it != values.end(); ++it)
            if (it.value().is_number()) values_[it.key()] = it.value().get<double>();
    }

    RLResult WaitAck(int rid, uint32_t timeoutMs) {
        const double deadline = NowMs() + double(timeoutMs);
        while (fd_ >= 0 && NowMs() < deadline) {
            Pump();
            const auto it = acks_.find(rid);
            if (it != acks_.end()) {
                const std::string result = it->second;
                acks_.erase(it);
                if (result == "ok") return RLResult::Ok;
                if (result == "invalid") return RLResult::InvalidArgument;
                if (result == "unknown" || result == "unsupported") return RLResult::Unsupported;
                if (result == "restart_required") return RLResult::RestartRequired;
                if (result == "busy") return RLResult::Busy;
                return RLResult::InternalError;
            }
            pollfd p{fd_, POLLIN, 0};
            poll(&p, 1, 5);
        }
        return RLResult::InternalError;
    }

    RLResult SetParamBlocking(const char* id, double value, uint32_t timeoutMs) {
        const int rid = ++nextRid_;
        SendLine(nlohmann::json{{"t", "set"}, {"id", id}, {"v", value}, {"rid", rid}}.dump());
        return WaitAck(rid, timeoutMs);
    }

    RLResult CommandBlocking(const char* name, uint32_t timeoutMs) {
        const int rid = ++nextRid_;
        SendLine(nlohmann::json{{"t", "cmd"}, {"name", name}, {"rid", rid}}.dump());
        return WaitAck(rid, timeoutMs);
    }

    int fd_ = -1;
    Protocol protocol_ = Protocol::None;
    HostInfo peer_{};
    std::string inbox_;
    std::vector<ParamInfo> params_;
    std::vector<MetricInfo> metricSchema_;
    std::vector<std::string> commands_;
    std::map<std::string, double> values_;
    std::map<std::string, double> metrics_;
    nlohmann::json info_ = nlohmann::json::object();
    std::map<int, std::string> acks_;
    bool haveMetrics_ = false;
    bool helloSeen_ = false;
    double lastRxMs_ = 0.0;
    int nextRid_ = 0;
};

} // namespace svms

#endif
