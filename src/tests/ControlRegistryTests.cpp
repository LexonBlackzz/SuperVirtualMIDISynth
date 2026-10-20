// Control API v1: registry semantics (all platforms) + socket round trip (Linux).
#include "driver/ControlRegistry.h"

#include <cstdio>
#include <cstdlib>

#ifdef __linux__
#include "driver/ControlServer.h"
#include <chrono>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#endif

using nlohmann::json;
using namespace svms::ctl;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static double gain = 1.0;
static int resets = 0;

static void Build(Registry& r) {
    Param p;
    p.id = "master_volume"; p.label = "Master"; p.group = "master";
    p.type = ParamType::Float; p.min = 0; p.max = 4; p.def = 1;
    p.get = [] { return gain; };
    p.set = [](double v) { gain = v; return SetResult::Ok; };
    r.AddParam(p);
    Param e;
    e.id = "mode"; e.label = "Mode"; e.group = "x"; e.type = ParamType::Enum;
    e.min = 0; e.max = 1; e.options = {"A", "B"};
    e.get = [] { return 0.0; };
    e.set = [](double) { return SetResult::Ok; };
    r.AddParam(e);
    r.AddMetric({"voices", "Voices", "", [] { return 42.0; }});
    r.AddCommand({"reset_voices", "Reset", [] { ++resets; return SetResult::Ok; }});
}

static json One(Registry& r, const char* line, Reply* keep = nullptr) {
    Reply reply;
    r.Handle(line, reply);
    CHECK(!reply.toSender.empty());
    json j = reply.toSender.empty() ? json() : json::parse(reply.toSender.back());
    if (keep) *keep = reply;
    return j;
}

static void TestRegistry() {
    Registry r;
    Build(r);
    const json hello = json::parse(r.Hello("SVMS", "1.2.3", 77, "test"));
    CHECK(hello["t"] == "hello" && hello["proto"] == kProtocolVersion);
    CHECK(hello["params"].size() == 2 && hello["params"][1]["options"].size() == 2);
    CHECK(hello["metrics"][0]["id"] == "voices" && hello["commands"][0]["name"] == "reset_voices");
    CHECK(hello["values"]["master_volume"] == 1.0);

    Reply reply;
    json ack = One(r, R"({"t":"set","id":"master_volume","v":2.5,"rid":7})", &reply);
    CHECK(ack["result"] == "ok" && ack["rid"] == 7 && ack["v"] == 2.5 && gain == 2.5);
    CHECK(reply.broadcast.size() == 1);  // everyone learns the new values

    CHECK(One(r, R"({"t":"set","id":"master_volume","v":9})")["result"] == "invalid");
    CHECK(One(r, R"({"t":"set","id":"master_volume","v":"x"})")["result"] == "invalid");
    CHECK(One(r, R"({"t":"set","id":"mode","v":0.5})")["result"] == "invalid");  // enum must be integral
    CHECK(gain == 2.5);
    CHECK(One(r, R"({"t":"set","id":"from_the_future","v":1})")["result"] == "unknown");
    CHECK(One(r, R"({"t":"cmd","name":"reset_voices"})")["result"] == "ok" && resets == 1);
    CHECK(One(r, R"({"t":"cmd","name":"nope"})")["result"] == "unknown");
    CHECK(One(r, R"({"t":"teleport"})")["result"] == "unsupported");
    CHECK(One(r, "not json")["result"] == "invalid");
    CHECK(One(r, R"([1,2])")["result"] == "invalid");

    Reply sub;
    One(r, R"({"t":"subscribe","hz":30})", &sub);
    CHECK(sub.subscribeHz == 30);
    CHECK(json::parse(r.MetricsLine())["m"]["voices"] == 42.0);
}

#ifdef __linux__
static std::string ReadLine(int fd) {
    std::string s;
    char c;
    while (read(fd, &c, 1) == 1 && c != '\n') s += c;
    return s;
}

static void TestSocket() {
    Registry r;
    Build(r);
    ControlServer server(r, "SVMS", "test");
    std::string error;
    CHECK(server.Start(error));
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", server.Path().c_str());
    CHECK(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(json::parse(ReadLine(fd))["t"] == "hello");
    const char req[] = "{\"t\":\"set\",\"id\":\"master_volume\",\"v\":3,\"rid\":1}\n";
    CHECK(write(fd, req, sizeof(req) - 1) == long(sizeof(req) - 1));
    json a = json::parse(ReadLine(fd));
    if (a["t"] == "values") a = json::parse(ReadLine(fd));  // broadcast may precede/follow the ack
    CHECK(a["t"] == "ack" && a["result"] == "ok");
    close(fd);
    server.Stop();
    CHECK(access(server.Path().c_str(), F_OK) != 0);  // socket file removed
}
#endif

int main() {
    TestRegistry();
#ifdef __linux__
    TestSocket();
#endif
    std::puts(failures ? "FAILED" : "control registry tests passed");
    return failures ? 1 : 0;
}
