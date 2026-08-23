#ifndef SVMS_NATIVE_REALTIME_H
#define SVMS_NATIVE_REALTIME_H

#if defined(SVMS_XP_COMPAT)
#include "SVMSAudioOutputDirectSound.h"
#else
#include "SVMSAudioOutput.h"
#endif
#include "SVMSEventCompile.h"
#include "SVMSFrameClock.h"
#include "SVMSMPSCQueue.h"
#include "SVMSStandaloneSynth.h"
#include "include/svmsapi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace svms {

class NativeRealtimeSessions {
public:
    static constexpr uint32_t kCapacity = 8u;

    bool IsToken(SVMS_Session session) const noexcept {
        const uint32_t encoded = static_cast<uint32_t>(session);
        return (encoded & kTypeMask) == kTypeBit &&
               (encoded & kIndexMask) >= 1u &&
               (encoded & kIndexMask) <= kCapacity;
    }

    SVMS_Result Create(const SVMS_RealtimeSessionConfig* config,
                       const std::wstring& soundfont,
                       const std::wstring& device,
                       SVMS_Session* outSession) {
        if (!config || !outSession || soundfont.empty())
            return SVMS_RESULT_INVALID_ARGUMENT;
        *outSession = 0u;
        const SVMS_Result validation = ValidateConfig(*config, device);
        if (validation != SVMS_RESULT_OK) return validation;

        for (uint32_t index = 0u; index < kCapacity; ++index) {
            Slot& slot = slots_[index];
            std::lock_guard<std::mutex> guard(slot.control);
            if (slot.token.load(std::memory_order_acquire) != 0u ||
                slot.calls.load(std::memory_order_acquire) != 0u)
                continue;
            try {
                std::unique_ptr<State> state(new (std::nothrow) State{});
                if (!state) return SVMS_RESULT_NO_RESOURCES;
                const SVMS_Result initialized = state->Initialize(
                    *config, soundfont, device);
                if (initialized != SVMS_RESULT_OK) return initialized;

                uint32_t generation = generation_.fetch_add(
                    1u, std::memory_order_relaxed) + 1u;
                if (generation == 0u) {
                    generation = generation_.fetch_add(
                        1u, std::memory_order_relaxed) + 1u;
                }
                const uint64_t token =
                    (static_cast<uint64_t>(generation) << 32u) |
                    static_cast<uint64_t>(kTypeBit | (index + 1u));
                slot.state = std::move(state);
                slot.token.store(token, std::memory_order_release);
                *outSession = token;
                if (config->start_immediately != 0u) {
                    const SVMS_Result started = slot.state->Start();
                    if (started != SVMS_RESULT_OK) {
                        slot.token.store(0u, std::memory_order_release);
                        slot.state.reset();
                        *outSession = 0u;
                        return started;
                    }
                }
                return SVMS_RESULT_OK;
            } catch (const std::bad_alloc&) {
                return SVMS_RESULT_NO_RESOURCES;
            } catch (...) {
                return SVMS_RESULT_INTERNAL_ERROR;
            }
        }
        return SVMS_RESULT_NO_RESOURCES;
    }

    SVMS_Result Destroy(SVMS_Session session) {
        Slot* slot = FindSlot(session);
        if (!slot) return SVMS_RESULT_INVALID_ARGUMENT;
        uint64_t expected = session;
        if (!slot->token.compare_exchange_strong(
                expected, 0u, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return SVMS_RESULT_INVALID_ARGUMENT;
        slot->cancellation.store(session, std::memory_order_release);
        while (slot->calls.load(std::memory_order_acquire) != 0u)
            std::this_thread::yield();
        std::lock_guard<std::mutex> guard(slot->control);
        slot->state.reset();
        return SVMS_RESULT_OK;
    }

    SVMS_Result Start(SVMS_Session session) {
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        return lease.state->Start();
    }

    SVMS_Result Stop(SVMS_Session session) {
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        lease.state->Stop();
        return SVMS_RESULT_OK;
    }

    SVMS_Result SubmitQpc(SVMS_Session session, uint32_t message,
                          uint64_t timestampQpc) {
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        return lease.state->Submit(message, timestampQpc,
                                   QueuedKind::Qpc, lease.cancellation,
                                   session);
    }

    SVMS_Result SubmitFrame(SVMS_Session session, uint32_t message,
                            uint64_t outputFrame) {
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        return lease.state->Submit(message, outputFrame,
                                   QueuedKind::Frame, lease.cancellation,
                                   session);
    }

    SVMS_Result Reset(SVMS_Session session) {
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        return lease.state->Submit(0u, 0u, QueuedKind::Reset,
                                   lease.cancellation, session);
    }

    SVMS_Result Cancel(SVMS_Session session) {
        Slot* slot = FindSlot(session);
        if (!slot || slot->token.load(std::memory_order_acquire) != session)
            return SVMS_RESULT_NOT_INITIALIZED;
        slot->cancellation.store(session, std::memory_order_release);
        return SVMS_RESULT_OK;
    }

    SVMS_Result GetOutputClock(SVMS_Session session, uint64_t* outputFrame,
                               uint32_t* sampleRate) {
        if (!outputFrame || !sampleRate) return SVMS_RESULT_INVALID_ARGUMENT;
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        *outputFrame = lease.state->outputFramePublished.load(
            std::memory_order_acquire);
        *sampleRate = lease.state->sampleRate;
        return SVMS_RESULT_OK;
    }

    SVMS_Result SetIngressMode(SVMS_Session session, uint32_t mode) {
        if (mode > SVMS_INGRESS_LOSSLESS)
            return SVMS_RESULT_INVALID_ARGUMENT;
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        lease.state->ingressMode.store(mode, std::memory_order_release);
        return SVMS_RESULT_OK;
    }

    SVMS_Result GetQueueInfo(SVMS_Session session, SVMS_QueueInfo* info) {
        if (!info || info->struct_size < 16u ||
            info->struct_version != SVMS_STRUCT_VERSION_1)
            return SVMS_RESULT_INVALID_ARGUMENT;
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        const uint32_t callerSize = info->struct_size;
        SVMS_QueueInfo result{};
        result.struct_size = sizeof(result);
        result.struct_version = SVMS_STRUCT_VERSION_1;
        result.ingress_mode = lease.state->ingressMode.load(
            std::memory_order_acquire);
        result.current_velocity_cutoff = 1u;
        result.queue_capacity = lease.state->eventCapacity;
        result.raw_ingress_count = lease.state->queue.Size();
        result.scheduled_count = lease.state->pendingPublished.load(
            std::memory_order_acquire);
        result.max_events_per_callback = lease.state->eventCapacity;
        result.submitted_events = lease.state->submitted.load(
            std::memory_order_relaxed);
        result.accepted_events = lease.state->accepted.load(
            std::memory_order_relaxed);
        result.intentionally_shed_events = lease.state->shed.load(
            std::memory_order_relaxed);
        result.cancelled_submissions = lease.state->cancelled.load(
            std::memory_order_relaxed);
        std::memcpy(info, &result,
                    (std::min)(callerSize,
                               static_cast<uint32_t>(sizeof(result))));
        return SVMS_RESULT_OK;
    }

    SVMS_Result GetTelemetry(SVMS_Session session,
                             SVMS_TelemetryV1* telemetry) {
        if (!telemetry || telemetry->struct_size < sizeof(*telemetry) ||
            telemetry->struct_version != SVMS_STRUCT_VERSION_1)
            return SVMS_RESULT_INVALID_ARGUMENT;
        Lease lease = Acquire(session);
        if (!lease.state) return SVMS_RESULT_NOT_INITIALIZED;
        State& state = *lease.state;
        SVMS_TelemetryV1 result{};
        result.struct_size = sizeof(result);
        result.struct_version = SVMS_STRUCT_VERSION_1;
        result.callback_count = state.callbackCount.load(
            std::memory_order_relaxed);
        result.submitted_events = state.submitted.load(
            std::memory_order_relaxed);
        result.accepted_events = state.accepted.load(
            std::memory_order_relaxed);
        result.dispatched_events = state.dispatched.load(
            std::memory_order_relaxed);
        result.note_ons = state.noteCalls.load(std::memory_order_relaxed);
        result.matched_regions = state.matchedNotes.load(
            std::memory_order_relaxed);
        result.configured_voices = result.matched_regions;
        result.voice_steals = state.voiceSteals.load(
            std::memory_order_relaxed);
        result.active_voices = state.activeVoices.load(
            std::memory_order_relaxed);
        result.free_voices = state.freeVoices.load(
            std::memory_order_relaxed);
        result.sample_rate = state.sampleRate;
        result.buffer_frames = state.bufferFrames;
        result.soundfont_loaded = 1u;
        result.audio_running = state.audio.IsRunning() ? 1u : 0u;
        result.render_time_ms = state.renderMilliseconds.load(
            std::memory_order_relaxed);
        result.render_peak = state.renderPeak.load(std::memory_order_relaxed);
        *telemetry = result;
        return SVMS_RESULT_OK;
    }

private:
    static constexpr uint32_t kTypeMask = 0xc0000000u;
    static constexpr uint32_t kTypeBit = 0x40000000u;
    static constexpr uint32_t kIndexMask = 0x3fffffffu;

    enum class QueuedKind : uint8_t { Qpc, Frame, Reset };

    struct QueuedEvent {
        uint64_t timestamp = 0u;
        uint64_t sequence = 0u;
        uint64_t targetFrame = 0u;
        uint32_t message = 0u;
        QueuedKind kind = QueuedKind::Qpc;
    };

    struct State {
        AudioOutput audio;
        StandaloneSynth synth;
        DynamicMPSCQueue<QueuedEvent> queue;
        std::vector<QueuedEvent> pending;
        std::vector<float> left;
        std::vector<float> right;
        std::mutex lifecycle;
        uint64_t clockEpochQpc = 0u;
        uint64_t clockEpochFrame = 0u;
        uint64_t qpcFrequency = 1u;
        uint64_t outputFrame = 0u;
        uint32_t sampleRate = 0u;
        uint32_t bufferFrames = 0u;
        uint32_t maxVoices = 0u;
        uint32_t eventCapacity = 0u;
        std::atomic<uint64_t> nextSequence{0u};
        std::atomic<uint64_t> outputFramePublished{0u};
        std::atomic<uint32_t> pendingPublished{0u};
        std::atomic<uint32_t> ingressMode{SVMS_INGRESS_LOSSLESS};
        std::atomic<uint64_t> callbackCount{0u};
        std::atomic<uint64_t> submitted{0u};
        std::atomic<uint64_t> accepted{0u};
        std::atomic<uint64_t> dispatched{0u};
        std::atomic<uint64_t> shed{0u};
        std::atomic<uint64_t> cancelled{0u};
        std::atomic<uint64_t> noteCalls{0u};
        std::atomic<uint64_t> matchedNotes{0u};
        std::atomic<uint32_t> voiceSteals{0u};
        std::atomic<uint32_t> activeVoices{0u};
        std::atomic<uint32_t> freeVoices{0u};
        std::atomic<float> renderMilliseconds{0.0f};
        std::atomic<float> renderPeak{0.0f};

        SVMS_Result Initialize(const SVMS_RealtimeSessionConfig& config,
                               const std::wstring& soundfont,
                               const std::wstring& device) {
#if defined(SVMS_XP_COMPAT)
            if (!audio.Initialize(config.sample_rate, config.buffer_frames))
                return SVMS_RESULT_INTERNAL_ERROR;
#else
            if (!audio.Initialize(config.sample_rate, config.buffer_frames,
                                  device))
                return SVMS_RESULT_INTERNAL_ERROR;
#endif
            sampleRate = audio.GetSampleRate();
            bufferFrames = audio.GetBufferFrames();
            maxVoices = config.max_voices;
            eventCapacity = config.event_capacity;
            if (!queue.ConfigureCapacity(eventCapacity))
                return SVMS_RESULT_NO_RESOURCES;
            pending.reserve(eventCapacity);
            left.resize(bufferFrames);
            right.resize(bufferFrames);

            StandaloneSynthConfig synthConfig{};
            synthConfig.soundfont = soundfont;
            synthConfig.sampleRate = sampleRate;
            synthConfig.maxVoices = config.max_voices;
            synthConfig.renderThreads = config.render_threads;
            synthConfig.maxBlockFrames = bufferFrames;
            synthConfig.masterVolume = config.master_volume;
            synthConfig.limiterEnabled = config.limiter_enabled != 0u;
            synthConfig.limiterAlgorithm = config.limiter_algorithm ==
                    SVMS_LIMITER_ADAPTIVE
                ? LimiterAlgorithm::Adaptive : LimiterAlgorithm::Classic;
            synthConfig.limiterThreshold = config.limiter_threshold;
            synthConfig.limiterLookaheadMs = config.limiter_lookahead_ms;
            synthConfig.limiterAttackMs = config.limiter_attack_ms;
            synthConfig.limiterReleaseMs = config.limiter_release_ms;
            switch (config.render_backend) {
            case SVMS_RENDER_BACKEND_AUTO:
                synthConfig.backend = RenderBackend::AVX512;
                break;
            case SVMS_RENDER_BACKEND_SCALAR:
                synthConfig.backend = RenderBackend::Scalar;
                break;
            case SVMS_RENDER_BACKEND_SSE2:
                synthConfig.backend = RenderBackend::SSE2;
                break;
            case SVMS_RENDER_BACKEND_AVX2:
                synthConfig.backend = RenderBackend::AVX2;
                break;
            default:
                return SVMS_RESULT_INVALID_ARGUMENT;
            }
            std::string error;
            if (!synth.Initialize(synthConfig, error))
                return SVMS_RESULT_INTERNAL_ERROR;
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency) ||
                frequency.QuadPart <= 0)
                return SVMS_RESULT_INTERNAL_ERROR;
            qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
            freeVoices.store(maxVoices, std::memory_order_relaxed);
            audio.SetRenderCallback(&RenderCallback, this);
            return SVMS_RESULT_OK;
        }

        SVMS_Result Start() {
            std::lock_guard<std::mutex> guard(lifecycle);
            if (audio.IsRunning()) return SVMS_RESULT_OK;
            LARGE_INTEGER now{};
            if (!QueryPerformanceCounter(&now))
                return SVMS_RESULT_INTERNAL_ERROR;
            clockEpochQpc = static_cast<uint64_t>(now.QuadPart);
            clockEpochFrame = outputFrame;
            return audio.Start() ? SVMS_RESULT_OK
                                 : SVMS_RESULT_INTERNAL_ERROR;
        }

        void Stop() {
            std::lock_guard<std::mutex> guard(lifecycle);
            audio.Stop();
        }

        SVMS_Result Submit(uint32_t message, uint64_t timestamp,
                           QueuedKind kind,
                           const std::atomic<uint64_t>* cancellation,
                           uint64_t cancellationToken) {
            submitted.fetch_add(1u, std::memory_order_relaxed);
            QueuedEvent event{};
            event.timestamp = timestamp;
            event.sequence = nextSequence.fetch_add(
                1u, std::memory_order_relaxed);
            event.message = message;
            event.kind = kind;
            const uint8_t status = static_cast<uint8_t>(message);
            const uint8_t velocity = static_cast<uint8_t>(message >> 16u);
            const bool droppable = kind != QueuedKind::Reset &&
                (status & 0xf0u) == 0x90u && velocity != 0u;
            for (;;) {
                if (cancellation && cancellation->load(
                        std::memory_order_acquire) == cancellationToken) {
                    cancelled.fetch_add(1u, std::memory_order_relaxed);
                    return SVMS_RESULT_CANCELLED;
                }
                if (queue.TryPush(event)) {
                    accepted.fetch_add(1u, std::memory_order_relaxed);
                    return SVMS_RESULT_OK;
                }
                if (droppable && ingressMode.load(std::memory_order_relaxed) ==
                        SVMS_INGRESS_PRIORITY) {
                    shed.fetch_add(1u, std::memory_order_relaxed);
                    return SVMS_RESULT_OK;
                }
                std::this_thread::yield();
            }
        }

        static void RenderCallback(float* output, uint32_t frameCount,
                                   void* userData) {
            static_cast<State*>(userData)->Render(output, frameCount);
        }

        void Render(float* output, uint32_t frameCount) {
            LARGE_INTEGER begin{}, end{};
            QueryPerformanceCounter(&begin);
            if (!output || frameCount > bufferFrames) return;
            std::fill(output, output + static_cast<size_t>(frameCount) * 2u,
                      0.0f);

            QueuedEvent incoming{};
            while (pending.size() < eventCapacity && queue.TryPop(incoming)) {
                if (incoming.kind == QueuedKind::Frame) {
                    incoming.targetFrame = incoming.timestamp;
                } else if (incoming.kind == QueuedKind::Reset) {
                    incoming.targetFrame = outputFrame;
                } else {
                    const int64_t delta = static_cast<int64_t>(incoming.timestamp) -
                        static_cast<int64_t>(clockEpochQpc);
                    const int64_t converted = QpcDeltaToFrames(
                        delta, static_cast<int64_t>(qpcFrequency), sampleRate);
                    incoming.targetFrame = converted <= 0
                        ? clockEpochFrame
                        : clockEpochFrame + static_cast<uint64_t>(converted) +
                              bufferFrames;
                }
                pending.push_back(incoming);
            }
            std::sort(pending.begin(), pending.end(),
                [](const QueuedEvent& leftEvent,
                   const QueuedEvent& rightEvent) {
                    return leftEvent.targetFrame < rightEvent.targetFrame ||
                        (leftEvent.targetFrame == rightEvent.targetFrame &&
                         leftEvent.sequence < rightEvent.sequence);
                });

            const uint64_t blockStart = outputFrame;
            const uint64_t blockEnd = blockStart + frameCount;
            uint32_t cursor = 0u;
            size_t consumed = 0u;
            for (; consumed < pending.size(); ++consumed) {
                const QueuedEvent& event = pending[consumed];
                if (event.targetFrame >= blockEnd) break;
                const uint32_t offset = event.targetFrame <= blockStart
                    ? 0u : static_cast<uint32_t>(event.targetFrame - blockStart);
                RenderSpan(output, cursor, offset - cursor, blockStart);
                cursor = offset;
                if (event.kind == QueuedKind::Reset)
                    synth.ResetAll(blockStart + cursor);
                else
                    synth.Dispatch(event.message, blockStart + cursor);
                dispatched.fetch_add(1u, std::memory_order_relaxed);
            }
            RenderSpan(output, cursor, frameCount - cursor, blockStart);
            if (consumed != 0u)
                pending.erase(pending.begin(), pending.begin() + consumed);
            outputFrame = blockEnd;
            outputFramePublished.store(outputFrame, std::memory_order_release);
            pendingPublished.store(static_cast<uint32_t>(pending.size()),
                                   std::memory_order_release);
            callbackCount.fetch_add(1u, std::memory_order_relaxed);
            noteCalls.store(synth.NoteCalls(), std::memory_order_relaxed);
            matchedNotes.store(synth.MatchedNotes(), std::memory_order_relaxed);
            voiceSteals.store(synth.Steals(), std::memory_order_relaxed);
            activeVoices.store(synth.Active(), std::memory_order_relaxed);
            freeVoices.store(synth.Free(), std::memory_order_relaxed);
            QueryPerformanceCounter(&end);
            renderMilliseconds.store(static_cast<float>(
                static_cast<double>(end.QuadPart - begin.QuadPart) * 1000.0 /
                static_cast<double>(qpcFrequency)), std::memory_order_relaxed);
            float peak = 0.0f;
            for (uint32_t i = 0u; i < frameCount * 2u; ++i)
                peak = (std::max)(peak, std::fabs(output[i]));
            renderPeak.store(peak, std::memory_order_relaxed);
        }

        void RenderSpan(float* output, uint32_t offset, uint32_t frameCount,
                        uint64_t blockStart) {
            if (frameCount == 0u) return;
            synth.Render(left.data(), right.data(), frameCount,
                         blockStart + offset);
            for (uint32_t frame = 0u; frame < frameCount; ++frame) {
                output[(offset + frame) * 2u] = left[frame];
                output[(offset + frame) * 2u + 1u] = right[frame];
            }
        }
    };

    struct Slot {
        std::atomic<uint64_t> token{0u};
        std::atomic<uint64_t> cancellation{0u};
        std::atomic<uint32_t> calls{0u};
        std::mutex control;
        std::unique_ptr<State> state;
    };

    struct Lease {
        Slot* slot = nullptr;
        State* state = nullptr;
        std::atomic<uint64_t>* cancellation = nullptr;
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : slot(other.slot), state(other.state),
              cancellation(other.cancellation) {
            other.slot = nullptr;
            other.state = nullptr;
            other.cancellation = nullptr;
        }
        ~Lease() {
            if (slot) slot->calls.fetch_sub(1u, std::memory_order_release);
        }
    };

    static SVMS_Result ValidateConfig(
        const SVMS_RealtimeSessionConfig& config,
        const std::wstring& device) {
        if (config.struct_size < sizeof(config) ||
            config.struct_version != SVMS_STRUCT_VERSION_1 ||
            config.flags != 0u || config.sample_rate < 8000u ||
            config.sample_rate > 384000u || config.buffer_frames < 16u ||
            config.buffer_frames > 8192u || config.max_voices == 0u ||
            config.max_voices > kMaxPolyphony || config.render_threads > 64u ||
            config.render_backend > SVMS_RENDER_BACKEND_AVX2 ||
            config.event_capacity < 1024u ||
            config.start_immediately > 1u || config.limiter_enabled > 1u ||
            config.limiter_algorithm > SVMS_LIMITER_ADAPTIVE ||
            !std::isfinite(config.master_volume) ||
            config.master_volume < 0.0f || config.master_volume > 4.0f ||
            !std::isfinite(config.limiter_threshold) ||
            config.limiter_threshold < 0.1f ||
            config.limiter_threshold > 1.0f ||
            !std::isfinite(config.limiter_lookahead_ms) ||
            config.limiter_lookahead_ms < 0.0f ||
            config.limiter_lookahead_ms > 20.0f ||
            !std::isfinite(config.limiter_attack_ms) ||
            config.limiter_attack_ms < 0.01f ||
            config.limiter_attack_ms > 100.0f ||
            !std::isfinite(config.limiter_release_ms) ||
            config.limiter_release_ms < 1.0f ||
            config.limiter_release_ms > 5000.0f)
            return SVMS_RESULT_INVALID_ARGUMENT;
        for (uint32_t value : config.reserved)
            if (value != 0u) return SVMS_RESULT_INVALID_ARGUMENT;
#if defined(SVMS_XP_COMPAT)
        if (config.audio_backend != SVMS_AUDIO_BACKEND_AUTO &&
            config.audio_backend != SVMS_AUDIO_BACKEND_DIRECTSOUND)
            return SVMS_RESULT_UNSUPPORTED;
        if (!device.empty() && _wcsicmp(device.c_str(), L"default") != 0)
            return SVMS_RESULT_UNSUPPORTED;
#else
        if (config.audio_backend != SVMS_AUDIO_BACKEND_AUTO &&
            config.audio_backend != SVMS_AUDIO_BACKEND_WASAPI_SHARED)
            return SVMS_RESULT_UNSUPPORTED;
#endif
        return SVMS_RESULT_OK;
    }

    Slot* FindSlot(SVMS_Session session) {
        if (!IsToken(session)) return nullptr;
        const uint32_t index =
            (static_cast<uint32_t>(session) & kIndexMask) - 1u;
        return &slots_[index];
    }

    Lease Acquire(SVMS_Session session) {
        Lease lease;
        Slot* slot = FindSlot(session);
        if (!slot || slot->token.load(std::memory_order_acquire) != session)
            return lease;
        slot->calls.fetch_add(1u, std::memory_order_acq_rel);
        if (slot->token.load(std::memory_order_acquire) != session) {
            slot->calls.fetch_sub(1u, std::memory_order_release);
            return lease;
        }
        lease.slot = slot;
        lease.state = slot->state.get();
        lease.cancellation = &slot->cancellation;
        return lease;
    }

    std::array<Slot, kCapacity> slots_{};
    std::atomic<uint32_t> generation_{1u};
};

} // namespace svms

#endif
