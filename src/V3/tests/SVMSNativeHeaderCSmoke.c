#include "../include/svmsapi.h"

#include <stddef.h>

typedef char assert_session_config_size[
    sizeof(SVMS_SessionConfig) == 64 ? 1 : -1];
typedef char assert_short_event_size[
    sizeof(SVMS_ShortEvent) == 16 ? 1 : -1];
typedef char assert_timed_short_event_size[
    sizeof(SVMS_TimedShortEvent) == 16 ? 1 : -1];
typedef char assert_queue_info_size[
    sizeof(SVMS_QueueInfo) == 88 ? 1 : -1];
typedef char assert_offline_config_size[
    sizeof(SVMS_OfflineSessionConfig) == 80 ? 1 : -1];
typedef char assert_offline_event_size[
    sizeof(SVMS_OfflineEvent) == 16 ? 1 : -1];
typedef char assert_offline_telemetry_size[
    sizeof(SVMS_OfflineTelemetry) == 64 ? 1 : -1];
typedef char assert_telemetry_size[
    sizeof(SVMS_TelemetryV1) == 128 ? 1 : -1];
typedef char assert_interface_order[
    offsetof(SVMS_Interface, create_session) >
        offsetof(SVMS_Interface, capabilities) ? 1 : -1];
typedef char assert_append_only_tail[
    offsetof(SVMS_Interface, send_timed_short_batch) >
        offsetof(SVMS_Interface, get_runtime_clock) ? 1 : -1];
typedef char assert_offline_append_only_tail[
    offsetof(SVMS_Interface, create_offline_session) >
        offsetof(SVMS_Interface, panic) ? 1 : -1];

int main(void) {
    SVMS_Interface api = {0};
    SVMS_SessionConfig config = {0};
    config.struct_size = (uint32_t)sizeof(config);
    config.struct_version = SVMS_STRUCT_VERSION_1;
    return api.struct_size == 0u && config.reserved[0] == 0u ? 0 : 1;
}
