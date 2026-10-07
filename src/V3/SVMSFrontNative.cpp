// Native SVMS API: SVMS_GetInterface and its session functions.

#include "SVMSDriverInternal.h"
#include "SVMSNativeOffline.h"


extern "C" {

// â”€â”€ Native SVMS API â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static constexpr uint32_t kNativeSessionCapacity = 64u;
static std::atomic<uint64_t> g_nativeSessions[kNativeSessionCapacity]{};
static std::atomic<uint64_t>
    g_nativeSessionCancellation[kNativeSessionCapacity]{};
static std::atomic<uint32_t> g_nativeSessionGeneration{1u};
static svms::NativeOfflineSessions g_nativeOfflineSessions;

static bool NativeSessionIsValid(SVMS_Session session) {
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    if (encodedIndex == 0u || encodedIndex > kNativeSessionCapacity)
        return false;
    return g_nativeSessions[encodedIndex - 1u].load(
        std::memory_order_acquire) == session;
}

static std::atomic<uint64_t>* NativeSessionCancellation(
    SVMS_Session session) {
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    if (encodedIndex == 0u || encodedIndex > kNativeSessionCapacity ||
        g_nativeSessions[encodedIndex - 1u].load(std::memory_order_acquire) !=
            session)
        return nullptr;
    return &g_nativeSessionCancellation[encodedIndex - 1u];
}

static SVMS_Result SVMS_CALL NativeCreateSession(
    const SVMS_SessionConfig* config, SVMS_Session* outSession) {
    if (!outSession) return SVMS_RESULT_INVALID_ARGUMENT;
    *outSession = 0u;
    if (config) {
        if (config->struct_size < 16u ||
            config->struct_version != SVMS_STRUCT_VERSION_1 ||
            config->flags != 0u)
            return SVMS_RESULT_INVALID_ARGUMENT;
    }

    // Reserve engine ownership before initialization so another frontend
    // cannot shut the shared runtime down between StartAudio and slot publish.
    g_nativeOwners.fetch_add(1u, std::memory_order_acq_rel);
    if (!EnsureDriverInitialized()) {
        g_nativeOwners.fetch_sub(1u, std::memory_order_acq_rel);
        return SVMS_RESULT_INTERNAL_ERROR;
    }

    uint32_t generation = g_nativeSessionGeneration.fetch_add(
        1u, std::memory_order_relaxed) + 1u;
    if (generation == 0u)
        generation = g_nativeSessionGeneration.fetch_add(
            1u, std::memory_order_relaxed) + 1u;
    for (uint32_t i = 0u; i < kNativeSessionCapacity; ++i) {
        const uint64_t token = (static_cast<uint64_t>(generation) << 32u) |
                               static_cast<uint64_t>(i + 1u);
        uint64_t empty = 0u;
        if (g_nativeSessions[i].compare_exchange_strong(
                empty, token, std::memory_order_release,
                std::memory_order_relaxed)) {
            *outSession = token;
            return SVMS_RESULT_OK;
        }
    }
    g_nativeOwners.fetch_sub(1u, std::memory_order_acq_rel);
    MaybeShutdownDriver();
    return SVMS_RESULT_NO_RESOURCES;
}

SVMS_Result SVMS_CALL NativeDestroySession(SVMS_Session session) {
    if (g_nativeOfflineSessions.IsToken(session))
        return g_nativeOfflineSessions.Destroy(session);
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    if (encodedIndex == 0u || encodedIndex > kNativeSessionCapacity)
        return SVMS_RESULT_INVALID_ARGUMENT;
    if (g_nativeSessions[encodedIndex - 1u].load(std::memory_order_acquire) !=
        session)
        return SVMS_RESULT_INVALID_ARGUMENT;
    g_nativeSessionCancellation[encodedIndex - 1u].store(
        session, std::memory_order_release);
    if (g_driver) g_driver->WakeBlockedProducers();
    uint64_t expected = session;
    if (!g_nativeSessions[encodedIndex - 1u].compare_exchange_strong(
            expected, 0u, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return SVMS_RESULT_INVALID_ARGUMENT;
    g_nativeOwners.fetch_sub(1u, std::memory_order_acq_rel);
    MaybeShutdownDriver();
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeSendShort(SVMS_Session session,
                                              uint32_t message) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    LARGE_INTEGER timestamp{};
    QueryPerformanceCounter(&timestamp);
    return g_driver->SubmitShortMsgAtQpcCancellable(
               message, static_cast<uint64_t>(timestamp.QuadPart),
               cancellation, session)
        ? SVMS_RESULT_OK : SVMS_RESULT_CANCELLED;
}

static SVMS_Result SVMS_CALL NativeSendShortAtQpc(
    SVMS_Session session, uint32_t message, uint64_t timestampQpc) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (timestampQpc == 0u) {
        LARGE_INTEGER timestamp{};
        QueryPerformanceCounter(&timestamp);
        timestampQpc = static_cast<uint64_t>(timestamp.QuadPart);
    }
    return g_driver->SubmitShortMsgAtQpcCancellable(
               message, timestampQpc, cancellation, session)
        ? SVMS_RESULT_OK : SVMS_RESULT_CANCELLED;
}

static SVMS_Result SVMS_CALL NativeSendShortBatch(
    SVMS_Session session, const SVMS_ShortEvent* events,
    uint32_t eventCount) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!events && eventCount != 0u) return SVMS_RESULT_INVALID_ARGUMENT;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        if (events[i].reserved != 0u) return SVMS_RESULT_INVALID_ARGUMENT;
    }
    // A batch is one submission boundary. Timestamp its immediate records
    // once, just like NativeSendTimedShortBatch, rather than crossing into
    // QueryPerformanceCounter for every packed MIDI message. Explicit QPC
    // timestamps remain untouched and equal-time records retain array order.
    LARGE_INTEGER immediate{};
    if (eventCount != 0u && !QueryPerformanceCounter(&immediate))
        return SVMS_RESULT_INTERNAL_ERROR;
    const uint64_t immediateQpc = static_cast<uint64_t>(immediate.QuadPart);
    return g_driver->SubmitShortBatchAtQpcCancellable(
               events, eventCount, immediateQpc, cancellation, session)
        ? SVMS_RESULT_OK : SVMS_RESULT_CANCELLED;
}

static SVMS_Result SVMS_CALL NativeSendSystemExclusive(
    SVMS_Session session, const uint8_t* data, uint32_t size) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!data || size < 2u || data[0] != 0xf0u || data[size - 1u] != 0xf7u)
        return SVMS_RESULT_INVALID_ARGUMENT;
    return g_driver->SubmitSystemExclusiveCancellable(
               data, size, cancellation, session)
        ? SVMS_RESULT_OK : SVMS_RESULT_CANCELLED;
}

static SVMS_Result SVMS_CALL NativeReset(SVMS_Session session) {
    if (g_nativeOfflineSessions.IsToken(session))
        return g_nativeOfflineSessions.Reset(session);
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    g_driver->ResetAllVoices();
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetTelemetry(
    SVMS_Session session, SVMS_TelemetryV1* telemetry) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!telemetry || telemetry->struct_size < sizeof(SVMS_TelemetryV1) ||
        telemetry->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    svms::DriverDebugInfo debug{};
    svms::SnappyVoiceStatistics voices{};
    g_driver->CopyDebugInfo(debug);
    g_driver->CopyVoiceStatistics(voices);
    SVMS_TelemetryV1 result{};
    result.struct_size = sizeof(result);
    result.struct_version = SVMS_STRUCT_VERSION_1;
    result.callback_count = debug.callbackCount;
    result.submitted_events = debug.submitted;
    result.accepted_events = debug.accepted;
    result.dispatched_events = debug.dispatched;
    result.note_ons = debug.noteOns;
    result.matched_regions = debug.matchedRegions;
    result.configured_voices = debug.configuredVoices;
    result.voice_steals = voices.voiceSteals;
    result.active_voices = voices.activeVoices;
    result.free_voices = voices.freeVoices;
    result.sample_rate = g_driver->sampleRate;
    result.buffer_frames = g_driver->bufferFrames;
    result.soundfont_loaded = debug.soundFontLoaded;
    result.audio_running = debug.audioRunning;
    result.render_time_ms = g_driver->GetRenderingTimeMilliseconds();
    result.render_peak = debug.renderPeak;
    *telemetry = result;
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeSendRuntimeCommand(
    SVMS_Session session, uint32_t command, uint32_t param,
    char* result_text_utf8, uint32_t inout_text_bytes) {
#if defined(SVMS_XP_COMPAT)
    (void)session; (void)command; (void)param;
    (void)result_text_utf8; (void)inout_text_bytes;
    return SVMS_RESULT_NOT_INITIALIZED;
#else
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (inout_text_bytes != 0u && !result_text_utf8)
        return SVMS_RESULT_INVALID_ARGUMENT;
    svms::RuntimeLinkCommandV2 cmd{};
    cmd.type = command;
    cmd.param = param;
    char text[svms::kRuntimeLinkCommandTextCapacity]{};
    const svms::RLResult rl = g_driver->ExecuteRuntimeCommand(cmd, text);
    if (inout_text_bytes != 0u) {
        strncpy_s(result_text_utf8, inout_text_bytes, text, _TRUNCATE);
    }
    switch (rl) {
        case svms::RLResult::Ok: return SVMS_RESULT_OK;
        case svms::RLResult::InvalidArgument:
            return SVMS_RESULT_INVALID_ARGUMENT;
        default: return SVMS_RESULT_INTERNAL_ERROR;
    }
#endif
}

static SVMS_Result SVMS_CALL NativeGetTelemetryV2(
    SVMS_Session session, SVMS_TelemetryV2* telemetry) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!telemetry || telemetry->struct_size < sizeof(SVMS_TelemetryV2) ||
        telemetry->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    g_driver->CopyTelemetryCensus(telemetry);
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetRuntimeClock(
    uint64_t* qpcNow, uint64_t* qpcFrequency) {
    if (!qpcNow || !qpcFrequency) return SVMS_RESULT_INVALID_ARGUMENT;
    LARGE_INTEGER now{}, frequency{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency))
        return SVMS_RESULT_INTERNAL_ERROR;
    *qpcNow = static_cast<uint64_t>(now.QuadPart);
    *qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
    return SVMS_RESULT_OK;
}

static uint64_t QpcTicksToMonotonicNanoseconds(uint64_t ticks,
                                               uint64_t frequency) {
    if (!frequency) return 0u;
    const uint64_t seconds = ticks / frequency;
    const uint64_t remainder = ticks % frequency;
    return seconds * 1000000000ull +
        (remainder * 1000000000ull) / frequency;
}

static uint64_t MonotonicNanosecondsToQpcTicks(uint64_t nanoseconds,
                                               uint64_t frequency) {
    if (!frequency) return 0u;
    const uint64_t seconds = nanoseconds / 1000000000ull;
    const uint64_t remainder = nanoseconds % 1000000000ull;
    return seconds * frequency + (remainder * frequency) / 1000000000ull;
}

static SVMS_Result SVMS_CALL NativeGetMonotonicClock(uint64_t* nanoseconds) {
    if (!nanoseconds) return SVMS_RESULT_INVALID_ARGUMENT;
    LARGE_INTEGER now{}, frequency{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency))
        return SVMS_RESULT_INTERNAL_ERROR;
    *nanoseconds = QpcTicksToMonotonicNanoseconds(
        static_cast<uint64_t>(now.QuadPart),
        static_cast<uint64_t>(frequency.QuadPart));
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeSendTimedShortBatch(
    SVMS_Session session, const SVMS_TimedShortEvent* events,
    uint32_t eventCount) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!events && eventCount != 0u) return SVMS_RESULT_INVALID_ARGUMENT;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        if (events[i].reserved != 0u ||
            events[i].timestamp_domain > SVMS_TIMESTAMP_MONOTONIC_NS ||
            (events[i].timestamp_domain == SVMS_TIMESTAMP_OUTPUT_FRAME &&
             events[i].timestamp > svms::kAbsoluteFrameTimestampMask) ||
            (events[i].timestamp_domain == SVMS_TIMESTAMP_QPC &&
             (events[i].timestamp & svms::kAbsoluteFrameTimestampTag) != 0u))
            return SVMS_RESULT_INVALID_ARGUMENT;
    }
    LARGE_INTEGER immediate{}, frequency{};
    if (!QueryPerformanceCounter(&immediate) ||
        !QueryPerformanceFrequency(&frequency))
        return SVMS_RESULT_INTERNAL_ERROR;
    const uint64_t immediateQpc = static_cast<uint64_t>(immediate.QuadPart);
    const uint64_t qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
    for (uint32_t i = 0u; i < eventCount; ++i) {
        const SVMS_TimedShortEvent& event = events[i];
        switch (event.timestamp_domain) {
        case SVMS_TIMESTAMP_IMMEDIATE:
            if (!g_driver->SubmitShortMsgAtQpcCancellable(
                    event.packed_message, immediateQpc, cancellation, session))
                return SVMS_RESULT_CANCELLED;
            break;
        case SVMS_TIMESTAMP_OUTPUT_FRAME:
            if (!g_driver->SubmitShortMsgAtFrameCancellable(
                    event.packed_message, event.timestamp, cancellation,
                    session))
                return SVMS_RESULT_CANCELLED;
            break;
        case SVMS_TIMESTAMP_QPC:
            if (!g_driver->SubmitShortMsgAtQpcCancellable(
                    event.packed_message, event.timestamp, cancellation,
                    session))
                return SVMS_RESULT_CANCELLED;
            break;
        case SVMS_TIMESTAMP_MONOTONIC_NS:
            if (!g_driver->SubmitShortMsgAtQpcCancellable(
                    event.packed_message,
                    MonotonicNanosecondsToQpcTicks(event.timestamp,
                                                   qpcFrequency),
                    cancellation, session))
                return SVMS_RESULT_CANCELLED;
            break;
        }
    }
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetOutputClock(
    SVMS_Session session, uint64_t* nextOutputFrame, uint32_t* sampleRate) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!nextOutputFrame || !sampleRate) return SVMS_RESULT_INVALID_ARGUMENT;
    *nextOutputFrame = g_driver->GetNextOutputFrame();
    *sampleRate = g_driver->sampleRate;
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeSetIngressMode(
    SVMS_Session session, uint32_t ingressMode) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (ingressMode > SVMS_INGRESS_LOSSLESS)
        return SVMS_RESULT_INVALID_ARGUMENT;
    g_driver->SetIngressMode(ingressMode == SVMS_INGRESS_LOSSLESS
        ? svms::EventOverflowMode::LosslessBackpressure
        : svms::EventOverflowMode::PriorityVelocity);
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetQueueInfo(
    SVMS_Session session, SVMS_QueueInfo* queueInfo) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!queueInfo || queueInfo->struct_size < 16u ||
        queueInfo->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    const uint32_t callerSize = queueInfo->struct_size;
    SVMS_QueueInfo result{};
    g_driver->CopyNativeQueueInfo(result);
    std::memcpy(queueInfo, &result,
                (std::min)(callerSize,
                           static_cast<uint32_t>(sizeof(result))));
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeLoadSoundFontUtf8(
    SVMS_Session session, const char* pathUtf8) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!g_driver) return SVMS_RESULT_NOT_INITIALIZED;
    if (!pathUtf8 || !*pathUtf8) return SVMS_RESULT_INVALID_ARGUMENT;
    const int required = MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1,
                                              nullptr, 0);
    if (required <= 1) return SVMS_RESULT_INVALID_ARGUMENT;
    std::wstring path(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1, path.data(), required) ==
        0)
        return SVMS_RESULT_INVALID_ARGUMENT;
    path.resize(static_cast<size_t>(required - 1));
    return g_driver->LoadSoundFont(path.c_str()) ? SVMS_RESULT_OK
                                                 : SVMS_RESULT_INTERNAL_ERROR;
}

static SVMS_Result SVMS_CALL NativePanic(SVMS_Session session) {
    return NativeReset(session);
}

static bool NativeUtf8ToWide(const char* pathUtf8, std::wstring& path) {
    if (!pathUtf8 || !*pathUtf8) return false;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              pathUtf8, -1, nullptr, 0);
    if (required <= 1) return false;
    path.assign(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pathUtf8, -1,
                            path.data(), required) == 0) {
        path.clear();
        return false;
    }
    path.resize(static_cast<size_t>(required - 1));
    return true;
}

SVMS_Result SVMS_CALL NativeCreateOfflineSession(
    const SVMS_OfflineSessionConfig* config, const char* soundfontPathUtf8,
    SVMS_Session* outSession) {
    std::wstring soundfont;
    if (!NativeUtf8ToWide(soundfontPathUtf8, soundfont))
        return SVMS_RESULT_INVALID_ARGUMENT;
    return g_nativeOfflineSessions.Create(config, soundfont, outSession);
}

SVMS_Result SVMS_CALL NativeRenderOffline(
    SVMS_Session session, const SVMS_OfflineEvent* events,
    uint32_t eventCount, float* outputLeft, float* outputRight,
    uint32_t frameCount) {
    return g_nativeOfflineSessions.Render(session, events, eventCount,
                                           outputLeft, outputRight,
                                           frameCount);
}

SVMS_Result SVMS_CALL NativeGetOfflineTelemetry(
    SVMS_Session session, SVMS_OfflineTelemetry* telemetry) {
    return g_nativeOfflineSessions.GetTelemetry(session, telemetry);
}

static SVMS_Result NativeCopyUtf8(const std::string& value, char* buffer,
                                  uint32_t* inoutBytes) {
    if (!inoutBytes || value.size() >= UINT32_MAX)
        return SVMS_RESULT_INVALID_ARGUMENT;
    const uint32_t required = static_cast<uint32_t>(value.size() + 1u);
    const uint32_t supplied = *inoutBytes;
    *inoutBytes = required;
    if (!buffer || supplied < required) return SVMS_RESULT_BUFFER_TOO_SMALL;
    std::memcpy(buffer, value.c_str(), required);
    return SVMS_RESULT_OK;
}

static SVMS_Result SVMS_CALL NativeGetConfigJson(
    SVMS_Session session, char* bufferUtf8, uint32_t* inoutBufferBytes) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    std::string document;
    if (!svms::ReadV3ConfigJson(document)) return SVMS_RESULT_INTERNAL_ERROR;
    return NativeCopyUtf8(document, bufferUtf8, inoutBufferBytes);
}

static SVMS_Result SVMS_CALL NativePatchConfigJson(
    SVMS_Session session, const char* mergePatchUtf8,
    uint32_t mergePatchBytes) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    std::string warning;
    if (svms::PatchV3ConfigJson(mergePatchUtf8, mergePatchBytes, &warning))
        return SVMS_RESULT_OK;
    return warning.find("invalid") != std::string::npos ||
           warning.find("schema") != std::string::npos ||
           warning.find("must be") != std::string::npos
        ? SVMS_RESULT_INVALID_ARGUMENT : SVMS_RESULT_INTERNAL_ERROR;
}

static SVMS_Result SVMS_CALL NativeGetConfigPathUtf8(
    SVMS_Session session, char* bufferUtf8, uint32_t* inoutBufferBytes) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    const std::wstring path = svms::GetV3ConfigPath();
    if (path.empty()) return SVMS_RESULT_INTERNAL_ERROR;
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return SVMS_RESULT_INTERNAL_ERROR;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, path.data(),
                            static_cast<int>(path.size()), utf8.data(), bytes,
                            nullptr, nullptr) == 0)
        return SVMS_RESULT_INTERNAL_ERROR;
    return NativeCopyUtf8(utf8, bufferUtf8, inoutBufferBytes);
}

static SVMS_Result SVMS_CALL NativeCancelSessionSubmissions(
    SVMS_Session session) {
    std::atomic<uint64_t>* cancellation =
        NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    cancellation->store(session, std::memory_order_release);
    if (g_driver) g_driver->WakeBlockedProducers();
    return SVMS_RESULT_OK;
}

SVMS_Result SVMS_CALL SVMS_GetInterface(
    uint32_t requestedAbi, uint32_t callerTableSize,
    SVMS_Interface* outInterface) {
    constexpr uint32_t minimumSize = static_cast<uint32_t>(
        offsetof(SVMS_Interface, get_runtime_clock) +
        sizeof(SVMS_GetRuntimeClockFn));
    if (!outInterface || callerTableSize < minimumSize)
        return SVMS_RESULT_INVALID_ARGUMENT;
    if (requestedAbi != SVMS_ABI_VERSION_1)
        return SVMS_RESULT_UNSUPPORTED_ABI;

    SVMS_Interface table{};
    table.struct_size = sizeof(table);
    table.struct_version = SVMS_STRUCT_VERSION_1;
    table.abi_version = SVMS_ABI_VERSION_1;
    table.capabilities = SVMS_CAP_EXACT_QPC_TIMESTAMPS |
        SVMS_CAP_SHORT_EVENT_BATCH | SVMS_CAP_SYSTEM_EXCLUSIVE |
        SVMS_CAP_TELEMETRY_V1 | SVMS_CAP_KDMAPI_FACADE |
        SVMS_CAP_EXACT_MONOTONIC_NS | SVMS_CAP_EXACT_OUTPUT_FRAMES |
        SVMS_CAP_QUEUE_CONTROL | SVMS_CAP_SOUNDFONT_RELOAD |
        SVMS_CAP_MIXED_TIMESTAMP_BATCH |
        SVMS_CAP_ISOLATED_OFFLINE_SESSIONS | SVMS_CAP_CONFIG_JSON |
        SVMS_CAP_CANCELLABLE_SUBMISSION | SVMS_CAP_TELEMETRY_V2;
#if !defined(SVMS_XP_COMPAT)
    table.capabilities |= SVMS_CAP_RUNTIME_COMMANDS;
#endif
    table.product_major = svms::build::kProductMajor;
    table.product_minor = svms::build::kProductMinor;
    table.product_patch = svms::build::kProductPatch;
    table.build_number = svms::build::kBuildNumber;
    table.create_session = NativeCreateSession;
    table.destroy_session = NativeDestroySession;
    table.send_short = NativeSendShort;
    table.send_short_at_qpc = NativeSendShortAtQpc;
    table.send_short_batch = NativeSendShortBatch;
    table.send_system_exclusive = NativeSendSystemExclusive;
    table.reset = NativeReset;
    table.get_telemetry = NativeGetTelemetry;
    table.get_runtime_clock = NativeGetRuntimeClock;
    table.send_timed_short_batch = NativeSendTimedShortBatch;
    table.get_output_clock = NativeGetOutputClock;
    table.get_monotonic_clock = NativeGetMonotonicClock;
    table.set_ingress_mode = NativeSetIngressMode;
    table.get_queue_info = NativeGetQueueInfo;
    table.load_soundfont_utf8 = NativeLoadSoundFontUtf8;
    table.panic = NativePanic;
    table.create_offline_session = NativeCreateOfflineSession;
    table.render_offline = NativeRenderOffline;
    table.get_offline_telemetry = NativeGetOfflineTelemetry;
    table.get_config_json = NativeGetConfigJson;
    table.patch_config_json = NativePatchConfigJson;
    table.get_config_path_utf8 = NativeGetConfigPathUtf8;
    table.cancel_session_submissions = NativeCancelSessionSubmissions;
    table.send_runtime_command = NativeSendRuntimeCommand;
    table.get_telemetry_v2 = NativeGetTelemetryV2;
    std::memcpy(outInterface, &table,
                (std::min)(callerTableSize,
                           static_cast<uint32_t>(sizeof(table))));
    return SVMS_RESULT_OK;
}

} // extern "C"
