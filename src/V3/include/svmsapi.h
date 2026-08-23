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
    SVMS_RESULT_INTERNAL_ERROR = 6,
    SVMS_RESULT_BUFFER_TOO_SMALL = 7,
    SVMS_RESULT_CANCELLED = 8
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
    SVMS_CAP_MIXED_TIMESTAMP_BATCH = UINT64_C(1) << 9,
    SVMS_CAP_ISOLATED_OFFLINE_SESSIONS = UINT64_C(1) << 10,
    SVMS_CAP_CONFIG_JSON = UINT64_C(1) << 11,
    SVMS_CAP_CANCELLABLE_SUBMISSION = UINT64_C(1) << 12
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

enum {
    SVMS_SESSION_OFFLINE_RENDER = 1u,
    SVMS_SESSION_SILENT_ANALYSIS = 2u
};

enum {
    SVMS_RENDER_BACKEND_AUTO = 0u,
    SVMS_RENDER_BACKEND_SCALAR = 1u,
    SVMS_RENDER_BACKEND_SSE2 = 2u,
    SVMS_RENDER_BACKEND_AVX2 = 3u
};

enum {
    SVMS_LIMITER_CLASSIC = 0u,
    SVMS_LIMITER_ADAPTIVE = 1u
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

// Configuration for an isolated caller-driven session. The SoundFont path is
// supplied separately as UTF-8 so this structure has the same layout on x86
// and x64. max_block_frames is also the allocation ceiling for one render call.
typedef struct SVMS_OfflineSessionConfig {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t session_kind;
    uint32_t flags;
    uint32_t sample_rate;
    uint32_t max_voices;
    uint32_t render_threads;
    uint32_t max_block_frames;
    uint32_t render_backend;
    uint32_t limiter_enabled;
    uint32_t limiter_algorithm;
    float master_volume;
    float limiter_threshold;
    float limiter_lookahead_ms;
    float limiter_attack_ms;
    float limiter_release_ms;
    uint32_t reserved[4];
} SVMS_OfflineSessionConfig;

// frame_offset is exact within the current render call. Records must be
// nondecreasing by frame_offset; equal-frame records retain array order.
typedef struct SVMS_OfflineEvent {
    uint32_t frame_offset;
    uint32_t packed_message;
    uint32_t reserved[2];
} SVMS_OfflineEvent;

typedef struct SVMS_OfflineTelemetry {
    uint32_t struct_size;
    uint32_t struct_version;
    uint64_t output_frame;
    uint64_t rendered_frames;
    uint64_t submitted_events;
    uint32_t active_voices;
    uint32_t free_voices;
    uint32_t voice_steals;
    uint32_t sample_rate;
    uint32_t max_block_frames;
    uint32_t session_kind;
    uint32_t reserved[2];
} SVMS_OfflineTelemetry;

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
// The runtime consumes/copies all required bytes before returning and never
// retains data. The caller may immediately reuse or release the buffer.
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
typedef SVMS_Result (SVMS_CALL *SVMS_CreateOfflineSessionFn)(
    const SVMS_OfflineSessionConfig* config, const char* soundfont_path_utf8,
    SVMS_Session* out_session);
typedef SVMS_Result (SVMS_CALL *SVMS_RenderOfflineFn)(
    SVMS_Session session, const SVMS_OfflineEvent* events,
    uint32_t event_count, float* output_left, float* output_right,
    uint32_t frame_count);
typedef SVMS_Result (SVMS_CALL *SVMS_GetOfflineTelemetryFn)(
    SVMS_Session session, SVMS_OfflineTelemetry* telemetry);
typedef SVMS_Result (SVMS_CALL *SVMS_GetConfigJsonFn)(
    SVMS_Session session, char* buffer_utf8, uint32_t* inout_buffer_bytes);
typedef SVMS_Result (SVMS_CALL *SVMS_PatchConfigJsonFn)(
    SVMS_Session session, const char* merge_patch_utf8,
    uint32_t merge_patch_bytes);
typedef SVMS_Result (SVMS_CALL *SVMS_GetConfigPathUtf8Fn)(
    SVMS_Session session, char* buffer_utf8, uint32_t* inout_buffer_bytes);
typedef SVMS_Result (SVMS_CALL *SVMS_CancelSessionSubmissionsFn)(
    SVMS_Session session);

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
    SVMS_CreateOfflineSessionFn create_offline_session;
    SVMS_RenderOfflineFn render_offline;
    SVMS_GetOfflineTelemetryFn get_offline_telemetry;
    SVMS_GetConfigJsonFn get_config_json;
    SVMS_PatchConfigJsonFn patch_config_json;
    SVMS_GetConfigPathUtf8Fn get_config_path_utf8;
    // Permanently fences event submission for this real-time session token and
    // wakes blocked lossless calls. Reset/telemetry/destroy remain valid.
    SVMS_CancelSessionSubmissionsFn cancel_session_submissions;
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
static_assert(sizeof(SVMS_OfflineSessionConfig) == 80,
              "SVMS_OfflineSessionConfig ABI changed");
static_assert(sizeof(SVMS_OfflineEvent) == 16,
              "SVMS_OfflineEvent ABI changed");
static_assert(sizeof(SVMS_OfflineTelemetry) == 64,
              "SVMS_OfflineTelemetry ABI changed");
static_assert(sizeof(SVMS_TelemetryV1) == 128,
              "SVMS_TelemetryV1 ABI changed");
#endif

#endif
