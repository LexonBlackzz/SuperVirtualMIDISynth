#ifndef SVMS_PUBLIC_API_H
#define SVMS_PUBLIC_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define SVMS_CALL __cdecl
#if defined(SVMS_API_EXPORTS)
#define SVMS_PUBLIC __declspec(dllexport)
#else
#define SVMS_PUBLIC
#endif
#else
#define SVMS_CALL
#define SVMS_PUBLIC __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SVMS_ABI_VERSION_1 1u
#define SVMS_STRUCT_VERSION_1 1u

typedef uint64_t SVMS_Session;

typedef enum SVMS_Result {
    SVMS_RESULT_OK = 0,
    SVMS_RESULT_INVALID_ARGUMENT = 1,
    SVMS_RESULT_UNSUPPORTED_ABI = 2,
    SVMS_RESULT_UNSUPPORTED = 3,
    SVMS_RESULT_NOT_INITIALIZED = 4,
    SVMS_RESULT_NO_RESOURCES = 5,
    SVMS_RESULT_INTERNAL_ERROR = 6
} SVMS_Result;

enum {
    SVMS_CAP_EXACT_QPC_TIMESTAMPS = UINT64_C(1) << 0,
    SVMS_CAP_SHORT_EVENT_BATCH    = UINT64_C(1) << 1,
    SVMS_CAP_SYSTEM_EXCLUSIVE     = UINT64_C(1) << 2,
    SVMS_CAP_TELEMETRY_V1         = UINT64_C(1) << 3,
    SVMS_CAP_KDMAPI_FACADE        = UINT64_C(1) << 4,
    SVMS_CAP_EXACT_MONOTONIC_NS   = UINT64_C(1) << 5,
    SVMS_CAP_EXACT_OUTPUT_FRAMES  = UINT64_C(1) << 6,
    SVMS_CAP_QUEUE_CONTROL        = UINT64_C(1) << 7,
    SVMS_CAP_SOUNDFONT_RELOAD     = UINT64_C(1) << 8,
    SVMS_CAP_MIXED_TIMESTAMP_BATCH = UINT64_C(1) << 9
};

enum {
    SVMS_TIMESTAMP_IMMEDIATE = 0u,
    SVMS_TIMESTAMP_OUTPUT_FRAME = 1u,
    SVMS_TIMESTAMP_QPC = 2u,
    SVMS_TIMESTAMP_MONOTONIC_NS = 3u
};

enum {
    SVMS_INGRESS_PRIORITY = 0u,
    SVMS_INGRESS_LOSSLESS = 1u
};

typedef struct SVMS_SessionConfig {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t reserved[6];
} SVMS_SessionConfig;

// timestamp_qpc == 0 timestamps the event at submission. Otherwise it is an
// absolute tick in the runtime clock domain returned by get_runtime_clock().
// Windows uses QueryPerformanceCounter ticks. Portable runtimes may advertise
// SVMS_CAP_EXACT_MONOTONIC_NS and use monotonic nanoseconds. Equal timestamps
// retain array/ingress order. The field name is retained for ABI stability.
typedef struct SVMS_ShortEvent {
    uint64_t timestamp_qpc;
    uint32_t packed_message;
    uint32_t reserved;
} SVMS_ShortEvent;

// Mixed-domain batch record. Each event is converted independently to an
// absolute output frame; batching never gives the array one shared timestamp.
typedef struct SVMS_TimedShortEvent {
    uint64_t timestamp;
    uint32_t packed_message;
    uint16_t timestamp_domain;
    uint16_t reserved;
} SVMS_TimedShortEvent;

typedef struct SVMS_QueueInfo {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t ingress_mode;
    uint32_t current_velocity_cutoff;
    uint64_t queue_capacity;
    uint64_t raw_ingress_count;
    uint64_t compiled_count;
    uint64_t scheduled_count;
    uint64_t max_events_per_callback;
    uint64_t submitted_events;
    uint64_t accepted_events;
    uint64_t intentionally_shed_events;
    uint64_t cancelled_submissions;
} SVMS_QueueInfo;

typedef struct SVMS_TelemetryV1 {
    uint32_t struct_size;
    uint32_t struct_version;
    uint64_t callback_count;
    uint64_t submitted_events;
    uint64_t accepted_events;
    uint64_t dispatched_events;
    uint64_t note_ons;
    uint64_t matched_regions;
    uint64_t configured_voices;
    uint64_t voice_steals;
    uint32_t active_voices;
    uint32_t free_voices;
    uint32_t sample_rate;
    uint32_t buffer_frames;
    uint32_t soundfont_loaded;
    uint32_t audio_running;
    float render_time_ms;
    float render_peak;
    uint32_t reserved[6];
} SVMS_TelemetryV1;

typedef SVMS_Result (SVMS_CALL *SVMS_CreateSessionFn)(
    const SVMS_SessionConfig* config, SVMS_Session* out_session);
typedef SVMS_Result (SVMS_CALL *SVMS_DestroySessionFn)(SVMS_Session session);
typedef SVMS_Result (SVMS_CALL *SVMS_SendShortFn)(
    SVMS_Session session, uint32_t packed_message);
typedef SVMS_Result (SVMS_CALL *SVMS_SendShortAtQpcFn)(
    SVMS_Session session, uint32_t packed_message, uint64_t timestamp_qpc);
typedef SVMS_Result (SVMS_CALL *SVMS_SendShortBatchFn)(
    SVMS_Session session, const SVMS_ShortEvent* events,
    uint32_t event_count);
typedef SVMS_Result (SVMS_CALL *SVMS_SendSystemExclusiveFn)(
    SVMS_Session session, const uint8_t* data, uint32_t size);
typedef SVMS_Result (SVMS_CALL *SVMS_ResetFn)(SVMS_Session session);
typedef SVMS_Result (SVMS_CALL *SVMS_GetTelemetryFn)(
    SVMS_Session session, SVMS_TelemetryV1* telemetry);
typedef SVMS_Result (SVMS_CALL *SVMS_GetRuntimeClockFn)(
    uint64_t* qpc_now, uint64_t* qpc_frequency);
typedef SVMS_Result (SVMS_CALL *SVMS_SendTimedShortBatchFn)(
    SVMS_Session session, const SVMS_TimedShortEvent* events,
    uint32_t event_count);
typedef SVMS_Result (SVMS_CALL *SVMS_GetOutputClockFn)(
    SVMS_Session session, uint64_t* next_output_frame,
    uint32_t* sample_rate);
typedef SVMS_Result (SVMS_CALL *SVMS_GetMonotonicClockFn)(
    uint64_t* monotonic_nanoseconds);
typedef SVMS_Result (SVMS_CALL *SVMS_SetIngressModeFn)(
    SVMS_Session session, uint32_t ingress_mode);
typedef SVMS_Result (SVMS_CALL *SVMS_GetQueueInfoFn)(
    SVMS_Session session, SVMS_QueueInfo* queue_info);
typedef SVMS_Result (SVMS_CALL *SVMS_LoadSoundFontUtf8Fn)(
    SVMS_Session session, const char* path_utf8);
typedef SVMS_Result (SVMS_CALL *SVMS_PanicFn)(SVMS_Session session);

typedef struct SVMS_Interface {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t abi_version;
    uint32_t reserved0;
    uint64_t capabilities;
    uint32_t product_major;
    uint32_t product_minor;
    uint32_t product_patch;
    uint32_t build_number;
    SVMS_CreateSessionFn create_session;
    SVMS_DestroySessionFn destroy_session;
    SVMS_SendShortFn send_short;
    SVMS_SendShortAtQpcFn send_short_at_qpc;
    SVMS_SendShortBatchFn send_short_batch;
    SVMS_SendSystemExclusiveFn send_system_exclusive;
    SVMS_ResetFn reset;
    SVMS_GetTelemetryFn get_telemetry;
    SVMS_GetRuntimeClockFn get_runtime_clock;
    // Optional ABI-1 tail. Check both struct_size and the matching capability
    // before calling when a newer application loads an older runtime.
    SVMS_SendTimedShortBatchFn send_timed_short_batch;
    SVMS_GetOutputClockFn get_output_clock;
    SVMS_GetMonotonicClockFn get_monotonic_clock;
    SVMS_SetIngressModeFn set_ingress_mode;
    SVMS_GetQueueInfoFn get_queue_info;
    SVMS_LoadSoundFontUtf8Fn load_soundfont_utf8;
    SVMS_PanicFn panic;
} SVMS_Interface;

// Permanent bootstrap symbol. Function-table fields are append-only within an
// ABI version. The runtime writes at most caller_table_size bytes and reports
// its complete table size in struct_size.
SVMS_PUBLIC SVMS_Result SVMS_CALL SVMS_GetInterface(
    uint32_t requested_abi, uint32_t caller_table_size,
    SVMS_Interface* out_interface);

#ifdef __cplusplus
} // extern "C"

static_assert(sizeof(SVMS_SessionConfig) == 64,
              "SVMS_SessionConfig ABI changed");
static_assert(sizeof(SVMS_ShortEvent) == 16,
              "SVMS_ShortEvent ABI changed");
static_assert(sizeof(SVMS_TimedShortEvent) == 16,
              "SVMS_TimedShortEvent ABI changed");
static_assert(sizeof(SVMS_QueueInfo) == 88,
              "SVMS_QueueInfo ABI changed");
static_assert(sizeof(SVMS_TelemetryV1) == 128,
              "SVMS_TelemetryV1 ABI changed");
#endif

#endif
