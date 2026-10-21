// RemoteSchemaTest.cpp - the SVMSAPI driver-schema calls and the configurator's
// EngineLink schema accessors, against an in-memory fake of the Linux socket
// client (tests/FakeSocketClient.h). This is the code Linux uses; compiling it
// here with the fake gives it real coverage on every platform.
#include "include/svmsapi.h"
#include "configurator/EngineLink.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}
}  // namespace

int main() {
    SVMS_HostInfo hosts[4]{};
    const uint32_t count = SVMS_Remote_EnumerateHosts(hosts, 4);
    Check(count == 1u && hosts[0].pid == 4242u, "fake host is enumerated");

    SVMS_Interface api{};
    SVMS_Session session = 0;
    Check(SVMS_Remote_Attach(4242u, sizeof(api), &api, &session) ==
              SVMS_RESULT_OK, "attach");
    Check((api.capabilities & SVMS_CAP_SCHEMA) != 0u && api.get_schema_json &&
              api.get_param_value && api.set_param_value &&
              api.get_metric_value && api.run_command,
          "attached table advertises the schema calls");

    // Schema JSON: size query, too-small buffer, then the document.
    uint32_t bytes = 0u;
    Check(api.get_schema_json(session, nullptr, &bytes) ==
                  SVMS_RESULT_BUFFER_TOO_SMALL && bytes > 1u,
          "size query reports the needed size");
    std::string text(bytes, '\0');
    uint32_t small = 4u;
    Check(api.get_schema_json(session, text.data(), &small) ==
                  SVMS_RESULT_BUFFER_TOO_SMALL && small == bytes,
          "a short buffer is refused with the needed size");
    Check(api.get_schema_json(session, text.data(), &bytes) == SVMS_RESULT_OK,
          "schema fetched");
    const nlohmann::json doc = nlohmann::json::parse(text.c_str(), nullptr, false);
    Check(doc.is_object() && doc["params"].size() == 3u &&
              doc["metrics"].size() == 2u && doc["commands"].size() == 1u &&
              doc["info"].value("backend", std::string()) == "fake",
          "schema document has params, metrics, commands and info");
    Check(doc["params"][1].value("type", std::string()) == "enum" &&
              doc["params"][1]["options"].size() == 2u,
          "enum params carry their options");
    Check(api.get_schema_json(session, text.data(), nullptr) ==
              SVMS_RESULT_INVALID_ARGUMENT, "null size is rejected");

    // Values, setters and their verdicts.
    double v = -1.0;
    Check(api.get_param_value(session, "master_volume", &v) == SVMS_RESULT_OK &&
              v == 1.0, "get_param_value");
    Check(api.get_param_value(session, "nope", &v) ==
              SVMS_RESULT_INVALID_ARGUMENT, "unknown param get is invalid");
    Check(api.set_param_value(session, "master_volume", 2.5) == SVMS_RESULT_OK,
          "set_param_value");
    Check(api.get_param_value(session, "master_volume", &v) == SVMS_RESULT_OK &&
              v == 2.5, "set value reads back");
    Check(api.set_param_value(session, "master_volume", 99.0) ==
              SVMS_RESULT_INVALID_ARGUMENT, "out-of-range set is invalid");
    Check(api.set_param_value(session, "nope", 1.0) == SVMS_RESULT_UNSUPPORTED,
          "unknown param set is unsupported");
    Check(api.set_param_value(session, "fancy_mode", 1.0) ==
              SVMS_RESULT_RESTART_REQUIRED, "restart-required is reported");
    Check(api.set_param_value(session, "master_volume", std::nan("")) ==
              SVMS_RESULT_INVALID_ARGUMENT, "NaN set is invalid");
    Check(api.get_metric_value(session, "active_voices", &v) == SVMS_RESULT_OK &&
              v == 7.0, "get_metric_value");
    Check(api.get_metric_value(session, "nope", &v) ==
              SVMS_RESULT_INVALID_ARGUMENT, "unknown metric is invalid");
    Check(api.run_command(session, "reset_voices") == SVMS_RESULT_OK &&
              api.run_command(session, "nope") == SVMS_RESULT_UNSUPPORTED &&
              api.run_command(session, "") == SVMS_RESULT_INVALID_ARGUMENT,
          "run_command verdicts");
    Check(api.destroy_session(session) == SVMS_RESULT_OK &&
              api.get_schema_json(session, nullptr, &bytes) ==
                  SVMS_RESULT_NOT_INITIALIZED, "detach");

    // The configurator's view of the same host.
    svms::EngineLink link;
    Check(link.Open(4242u), "EngineLink opens the fake host");
    Check(link.Params().size() == 3u && link.MetricSchema().size() == 2u &&
              link.Info().value("soundfont", std::string()) == "/tmp/fake.sf2",
          "EngineLink loads the schema on open");
    Check(link.Params()[1].type == "enum" && link.Params()[1].options.size() == 2u &&
              link.Params()[0].max == 4.0, "EngineLink keeps param details");
    Check(svms::EngineLink::IsKnownParam("master_volume") &&
              !svms::EngineLink::IsKnownParam("fancy_mode") &&
              svms::EngineLink::IsKnownMetric("active_voices") &&
              !svms::EngineLink::IsKnownMetric("cache_hits"),
          "known/unknown classification");
    link.SetParam("master_volume", 3.0);
    Check(link.ParamValue("master_volume") == 3.0 &&
              link.MetricValue("cache_hits") == 99.0, "EngineLink values");
    svms::RuntimeLinkTelemetryV2 t{};
    Check(link.ReadTelemetry(t) && t.sampleRate == 48000u && t.activeVoices == 7u,
          "telemetry maps through the fake client");
    link.Close();
    Check(link.Params().empty() && !link.IsOpen(), "Close clears the schema");

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("SVMS remote schema test passed");
    return 0;
}
