#ifndef SVMS_NATIVE_OFFLINE_H
#define SVMS_NATIVE_OFFLINE_H

#include "SVMSStandaloneSynth.h"
#include "include/svmsapi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace svms {

// Independent caller-driven sessions for native API users. These sessions do
// not own an OS audio device and never share MIDI, SoundFont, voice, limiter,
// or render state with the process-global real-time driver.
class NativeOfflineSessions {
public:
    static constexpr uint32_t kCapacity = 16u;

    bool IsToken(SVMS_Session session) const noexcept {
        const uint32_t encoded = static_cast<uint32_t>(session);
        return (encoded & kTypeBit) != 0u &&
               (encoded & ~kTypeBit) >= 1u &&
               (encoded & ~kTypeBit) <= kCapacity;
    }

    SVMS_Result Create(const SVMS_OfflineSessionConfig* config,
                       const std::wstring& soundfont,
                       SVMS_Session* outSession) {
        if (!config || !outSession || soundfont.empty())
            return SVMS_RESULT_INVALID_ARGUMENT;
        *outSession = 0u;
        if (!ValidateConfig(*config)) return SVMS_RESULT_INVALID_ARGUMENT;

        try {
            for (uint32_t i = 0u; i < kCapacity; ++i) {
                Slot& slot = slots_[i];
                std::lock_guard<std::mutex> guard(slot.mutex);
                if (slot.token != 0u) continue;

                std::unique_ptr<State> state(new (std::nothrow) State{});
                if (!state) return SVMS_RESULT_NO_RESOURCES;
                StandaloneSynthConfig synthConfig{};
                synthConfig.soundfont = soundfont;
                synthConfig.sampleRate = config->sample_rate;
                synthConfig.maxVoices = config->max_voices;
                synthConfig.renderThreads = config->render_threads;
                synthConfig.maxBlockFrames = config->max_block_frames;
                synthConfig.masterVolume = config->master_volume;
                synthConfig.limiterEnabled = config->limiter_enabled != 0u;
                synthConfig.limiterAlgorithm = config->limiter_algorithm ==
                        SVMS_LIMITER_ADAPTIVE
                    ? LimiterAlgorithm::Adaptive : LimiterAlgorithm::Classic;
                synthConfig.limiterThreshold = config->limiter_threshold;
                synthConfig.limiterLookaheadMs = config->limiter_lookahead_ms;
                synthConfig.limiterAttackMs = config->limiter_attack_ms;
                synthConfig.limiterReleaseMs = config->limiter_release_ms;
                switch (config->render_backend) {
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
                if (!state->synth.Initialize(synthConfig, error))
                    return SVMS_RESULT_INTERNAL_ERROR;
                state->scratchLeft.resize(config->max_block_frames);
                state->scratchRight.resize(config->max_block_frames);
                state->sampleRate = config->sample_rate;
                state->maxVoices = config->max_voices;
                state->maxBlockFrames = config->max_block_frames;
                state->sessionKind = config->session_kind;

                uint32_t generation = generation_.fetch_add(
                    1u, std::memory_order_relaxed) + 1u;
                if (generation == 0u) {
                    generation = generation_.fetch_add(
                        1u, std::memory_order_relaxed) + 1u;
                }
                const uint64_t token =
                    (static_cast<uint64_t>(generation) << 32u) |
                    static_cast<uint64_t>(kTypeBit | (i + 1u));
                slot.state = std::move(state);
                slot.token = token;
                *outSession = token;
                return SVMS_RESULT_OK;
            }
        } catch (const std::bad_alloc&) {
            return SVMS_RESULT_NO_RESOURCES;
        } catch (...) {
            return SVMS_RESULT_INTERNAL_ERROR;
        }
        return SVMS_RESULT_NO_RESOURCES;
    }

    SVMS_Result Destroy(SVMS_Session session) {
        Slot* slot = FindSlot(session);
        if (!slot) return SVMS_RESULT_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> guard(slot->mutex);
        if (slot->token != session || !slot->state)
            return SVMS_RESULT_INVALID_ARGUMENT;
        slot->state.reset();
        slot->token = 0u;
        return SVMS_RESULT_OK;
    }

    SVMS_Result Reset(SVMS_Session session) {
        Slot* slot = FindSlot(session);
        if (!slot) return SVMS_RESULT_NOT_INITIALIZED;
        std::lock_guard<std::mutex> guard(slot->mutex);
        if (slot->token != session || !slot->state)
            return SVMS_RESULT_NOT_INITIALIZED;
        slot->state->synth.ResetAll(slot->state->outputFrame);
        return SVMS_RESULT_OK;
    }

    SVMS_Result Render(SVMS_Session session,
                       const SVMS_OfflineEvent* events,
                       uint32_t eventCount, float* outputLeft,
                       float* outputRight, uint32_t frameCount) {
        Slot* slot = FindSlot(session);
        if (!slot) return SVMS_RESULT_NOT_INITIALIZED;
        std::lock_guard<std::mutex> guard(slot->mutex);
        if (slot->token != session || !slot->state)
            return SVMS_RESULT_NOT_INITIALIZED;
        State& state = *slot->state;
        if ((!events && eventCount != 0u) ||
            frameCount > state.maxBlockFrames ||
            (state.sessionKind == SVMS_SESSION_OFFLINE_RENDER && frameCount &&
             (!outputLeft || !outputRight)))
            return SVMS_RESULT_INVALID_ARGUMENT;

        uint32_t priorOffset = 0u;
        for (uint32_t i = 0u; i < eventCount; ++i) {
            if (events[i].reserved[0] != 0u || events[i].reserved[1] != 0u ||
                events[i].frame_offset > frameCount ||
                (i != 0u && events[i].frame_offset < priorOffset))
                return SVMS_RESULT_INVALID_ARGUMENT;
            priorOffset = events[i].frame_offset;
        }

        if (outputLeft) std::fill(outputLeft, outputLeft + frameCount, 0.0f);
        if (outputRight) std::fill(outputRight, outputRight + frameCount, 0.0f);
        uint32_t cursor = 0u;
        for (uint32_t i = 0u; i < eventCount; ++i) {
            const uint32_t offset = events[i].frame_offset;
            RenderSpan(state, outputLeft, outputRight, cursor, offset - cursor);
            cursor = offset;
            state.synth.Dispatch(events[i].packed_message,
                                 state.outputFrame + cursor);
        }
        RenderSpan(state, outputLeft, outputRight, cursor,
                   frameCount - cursor);
        state.outputFrame += frameCount;
        state.renderedFrames += frameCount;
        state.submittedEvents += eventCount;
        return SVMS_RESULT_OK;
    }

    SVMS_Result GetTelemetry(SVMS_Session session,
                             SVMS_OfflineTelemetry* telemetry) {
        Slot* slot = FindSlot(session);
        if (!slot) return SVMS_RESULT_NOT_INITIALIZED;
        if (!telemetry || telemetry->struct_size < 16u ||
            telemetry->struct_version != SVMS_STRUCT_VERSION_1)
            return SVMS_RESULT_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> guard(slot->mutex);
        if (slot->token != session || !slot->state)
            return SVMS_RESULT_NOT_INITIALIZED;
        const uint32_t callerSize = telemetry->struct_size;
        const State& state = *slot->state;
        SVMS_OfflineTelemetry result{};
        result.struct_size = sizeof(result);
        result.struct_version = SVMS_STRUCT_VERSION_1;
        result.output_frame = state.outputFrame;
        result.rendered_frames = state.renderedFrames;
        result.submitted_events = state.submittedEvents;
        result.active_voices = state.synth.Active();
        result.free_voices = state.synth.Free();
        result.voice_steals = state.synth.Steals();
        result.sample_rate = state.sampleRate;
        result.max_block_frames = state.maxBlockFrames;
        result.session_kind = state.sessionKind;
        std::memcpy(telemetry, &result,
                    (std::min)(callerSize,
                               static_cast<uint32_t>(sizeof(result))));
        return SVMS_RESULT_OK;
    }

private:
    static constexpr uint32_t kTypeBit = 0x80000000u;

    struct State {
        StandaloneSynth synth;
        std::vector<float> scratchLeft;
        std::vector<float> scratchRight;
        uint64_t outputFrame = 0u;
        uint64_t renderedFrames = 0u;
        uint64_t submittedEvents = 0u;
        uint32_t sampleRate = 0u;
        uint32_t maxVoices = 0u;
        uint32_t maxBlockFrames = 0u;
        uint32_t sessionKind = 0u;
    };

    struct Slot {
        std::mutex mutex;
        uint64_t token = 0u;
        std::unique_ptr<State> state;
    };

    static bool ValidateConfig(const SVMS_OfflineSessionConfig& config) {
        if (config.struct_size < sizeof(SVMS_OfflineSessionConfig) ||
            config.struct_version != SVMS_STRUCT_VERSION_1 ||
            (config.session_kind != SVMS_SESSION_OFFLINE_RENDER &&
             config.session_kind != SVMS_SESSION_SILENT_ANALYSIS) ||
            config.flags != 0u || config.sample_rate < 8000u ||
            config.sample_rate > 384000u || config.max_voices == 0u ||
            config.max_voices > kMaxPolyphony || config.render_threads > 64u ||
            config.max_block_frames < 16u ||
            config.max_block_frames > 1048576u ||
            config.render_backend > SVMS_RENDER_BACKEND_AVX2 ||
            config.limiter_enabled > 1u ||
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
            return false;
        for (uint32_t value : config.reserved)
            if (value != 0u) return false;
        return true;
    }

    Slot* FindSlot(SVMS_Session session) {
        if (!IsToken(session)) return nullptr;
        const uint32_t index =
            (static_cast<uint32_t>(session) & ~kTypeBit) - 1u;
        return &slots_[index];
    }

    static void RenderSpan(State& state, float* outputLeft,
                           float* outputRight, uint32_t offset,
                           uint32_t frameCount) {
        if (frameCount == 0u) return;
        state.synth.Render(state.scratchLeft.data(), state.scratchRight.data(),
                           frameCount, state.outputFrame + offset);
        if (outputLeft) {
            std::copy_n(state.scratchLeft.data(), frameCount,
                        outputLeft + offset);
        }
        if (outputRight) {
            std::copy_n(state.scratchRight.data(), frameCount,
                        outputRight + offset);
        }
    }

    std::array<Slot, kCapacity> slots_{};
    std::atomic<uint32_t> generation_{1u};
};

} // namespace svms

#endif
