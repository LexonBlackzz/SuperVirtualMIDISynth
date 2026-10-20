#ifndef SVMS_CONTROL_REGISTRY_H
#define SVMS_CONTROL_REGISTRY_H

// Control API v1: the driver describes itself, the configurator renders it.
//
// Wire format is newline-delimited JSON (one object per line, UTF-8) over a
// local stream socket. The driver registers parameters, metrics and commands;
// the "hello" message carries the full schema, so a configurator can show
// features it has never heard of (grouped under "unknown to this version")
// and a driver can grow without a protocol bump. Transport-independent:
// ControlServer.cpp supplies the POSIX socket, tests drive Handle()
// directly.
//
//   S->C {"t":"hello","proto":1,"product":..,"version":..,"pid":..,
//         "info":{..},"params":[..],"metrics":[..],"commands":[..],"values":{..}}
//   C->S {"t":"set","id":"master_volume","v":1.5,"rid":7}
//   S->C {"t":"ack","rid":7,"id":..,"result":"ok","v":1.5}
//   S->C {"t":"values","values":{id:v}}        (to every client after a change)
//   C->S {"t":"cmd","name":"reset_voices","rid":8}
//   C->S {"t":"get"} / {"t":"subscribe","hz":30} / {"t":"ping","rid":9}
//   S->C {"t":"metrics","m":{id:v}}            (to subscribers)
//
// All values are JSON numbers (bool = 0/1, enum = option index).

#include <nlohmann/json.hpp>

#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace svms::ctl {

inline constexpr int kProtocolVersion = 1;

enum class ParamType { Bool, Int, Float, Enum };
enum class SetResult { Ok, Invalid, Unknown, RestartRequired, Busy };

inline const char* ResultName(SetResult r) {
    switch (r) {
    case SetResult::Ok:              return "ok";
    case SetResult::Invalid:         return "invalid";
    case SetResult::Unknown:         return "unknown";
    case SetResult::RestartRequired: return "restart_required";
    case SetResult::Busy:            return "busy";
    }
    return "unknown";
}

struct Param {
    std::string id, label, group, unit;
    ParamType type = ParamType::Float;
    double min = 0.0, max = 1.0, def = 0.0;
    std::vector<std::string> options;      // Enum labels
    std::function<double()> get;
    std::function<SetResult(double)> set;  // validated (range/type) before the call
};

struct Metric {
    std::string id, label, unit;
    std::function<double()> get;
};

struct Command {
    std::string name, label;
    std::function<SetResult()> run;
};

// What Handle() wants sent: replies to the requester, lines for every client.
struct Reply {
    std::vector<std::string> toSender;
    std::vector<std::string> broadcast;
    int subscribeHz = -1;                  // >=0: requester's telemetry rate (0 = off)
};

class Registry {
public:
    void AddParam(Param p) { params_.push_back(std::move(p)); }
    void AddMetric(Metric m) { metrics_.push_back(std::move(m)); }
    void AddCommand(Command c) { commands_.push_back(std::move(c)); }
    void SetInfo(const std::string& key, nlohmann::json value) { info_[key] = std::move(value); }

    nlohmann::json Values() const {
        nlohmann::json v = nlohmann::json::object();
        for (const Param& p : params_) if (p.get) v[p.id] = p.get();
        return v;
    }

    nlohmann::json MetricsSnapshot() const {
        nlohmann::json m = nlohmann::json::object();
        for (const Metric& x : metrics_) if (x.get) m[x.id] = x.get();
        return m;
    }

    std::string MetricsLine() const {
        return nlohmann::json{{"t", "metrics"}, {"m", MetricsSnapshot()}}.dump();
    }

    std::string Hello(const std::string& product, const std::string& version,
                      long pid, const std::string& platform) const {
        using nlohmann::json;
        json params = json::array(), metrics = json::array(), commands = json::array();
        for (const Param& p : params_) {
            static const char* kType[] = {"bool", "int", "float", "enum"};
            json d = {{"id", p.id}, {"label", p.label}, {"group", p.group},
                      {"unit", p.unit}, {"type", kType[int(p.type)]},
                      {"min", p.min}, {"max", p.max}, {"default", p.def}};
            if (p.type == ParamType::Enum) d["options"] = p.options;
            params.push_back(std::move(d));
        }
        for (const Metric& m : metrics_)
            metrics.push_back({{"id", m.id}, {"label", m.label}, {"unit", m.unit}});
        for (const Command& c : commands_)
            commands.push_back({{"name", c.name}, {"label", c.label}});
        return json{{"t", "hello"}, {"proto", kProtocolVersion}, {"product", product},
                    {"version", version}, {"pid", pid}, {"platform", platform},
                    {"info", info_}, {"params", params}, {"metrics", metrics},
                    {"commands", commands}, {"values", Values()}}.dump();
    }

    // One request line in; malformed or unknown input answers, never throws.
    void Handle(const std::string& line, Reply& out) {
        using nlohmann::json;
        json req = json::parse(line, nullptr, false);
        if (!req.is_object() || !req.contains("t") || !req["t"].is_string()) {
            out.toSender.push_back(json{{"t", "ack"}, {"result", "invalid"}}.dump());
            return;
        }
        const std::string t = req["t"].get<std::string>();
        json ack = {{"t", "ack"}, {"result", "ok"}};
        if (req.contains("rid")) ack["rid"] = req["rid"];

        if (t == "set") {
            const SetResult r = Set(req, ack);
            ack["result"] = ResultName(r);
            if (r == SetResult::Ok)
                out.broadcast.push_back(json{{"t", "values"}, {"values", Values()}}.dump());
        } else if (t == "cmd") {
            SetResult r = SetResult::Unknown;
            const std::string name = req.value("name", "");
            ack["name"] = name;
            for (const Command& c : commands_)
                if (c.name == name && c.run) { r = c.run(); break; }
            ack["result"] = ResultName(r);
        } else if (t == "get") {
            out.toSender.push_back(json{{"t", "values"}, {"values", Values()}}.dump());
            return;
        } else if (t == "subscribe") {
            const double hz = req.value("hz", 30.0);
            out.subscribeHz = hz <= 0.0 ? 0 : int(hz > 120.0 ? 120.0 : hz);
        } else if (t != "ping") {
            ack["result"] = "unsupported";
        }
        out.toSender.push_back(ack.dump());
    }

private:
    SetResult Set(const nlohmann::json& req, nlohmann::json& ack) {
        const std::string id = req.value("id", "");
        ack["id"] = id;
        for (Param& p : params_) {
            if (p.id != id) continue;
            if (!req.contains("v") || !req["v"].is_number() || !p.set) return SetResult::Invalid;
            const double v = req["v"].get<double>();
            if (!std::isfinite(v) || v < p.min || v > p.max) return SetResult::Invalid;
            if (p.type != ParamType::Float && v != std::floor(v)) return SetResult::Invalid;
            const SetResult r = p.set(v);
            if (r == SetResult::Ok && p.get) ack["v"] = p.get();
            return r;
        }
        return SetResult::Unknown;
    }

    std::vector<Param> params_;
    std::vector<Metric> metrics_;
    std::vector<Command> commands_;
    nlohmann::json info_ = nlohmann::json::object();
};

} // namespace svms::ctl

#endif
