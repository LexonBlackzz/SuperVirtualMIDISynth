// Conversions between the runtime-link wire structs and the native-API
// structs (svmsapi.h). Shared by the in-process driver and the remote client
// so both fill SVMS_LiveParams / SVMS_TelemetryV3 identically.
#ifndef SVMS_LIVE_PARAMS_CONVERT_H
#define SVMS_LIVE_PARAMS_CONVERT_H

#include "driver/RuntimeLinkProtocol.h"
#include "include/svmsapi.h"

#include <cstring>

namespace svms {

inline void ApiFromLive(const RuntimeLiveStateV2& l, SVMS_LiveParams& p) {
    p = SVMS_LiveParams{};
    p.struct_size = sizeof(p);
    p.struct_version = SVMS_STRUCT_VERSION_1;
    p.correctness_mode = l.correctnessMode;
    p.reverb_enabled = l.reverbEnabled;
    p.limiter_enabled = l.limiterEnabled;
    p.max_voices = l.maxVoices;
    p.limiter_algorithm = l.limiterAlgorithm;
    p.master_volume = l.masterVolume;
    p.reverb_mix = l.reverbMix;
    p.reverb_room_size = l.reverbRoomSize;
    p.reverb_decay = l.reverbDecay;
    p.reverb_damping = l.reverbDamping;
    p.reverb_width = l.reverbWidth;
    p.reverb_diffusion = l.reverbDiffusion;
    p.reverb_pre_delay_ms = l.reverbPreDelayMs;
    p.reverb_early_level = l.reverbEarlyLevel;
    p.reverb_late_level = l.reverbLateLevel;
    p.reverb_mod_depth = l.reverbModDepth;
    p.reverb_mod_rate = l.reverbModRate;
    p.reverb_low_cut_hz = l.reverbLowCutHz;
    p.reverb_high_cut_hz = l.reverbHighCutHz;
    p.limiter_threshold = l.limiterThreshold;
    p.limiter_lookahead_ms = l.limiterLookaheadMs;
    p.limiter_attack_ms = l.limiterAttackMs;
    p.limiter_release_ms = l.limiterReleaseMs;
}

inline void LiveFromApi(const SVMS_LiveParams& p, RuntimeLiveStateV2& l) {
    l.correctnessMode = p.correctness_mode;
    l.reverbEnabled = p.reverb_enabled;
    l.limiterEnabled = p.limiter_enabled;
    l.maxVoices = p.max_voices;
    l.limiterAlgorithm = p.limiter_algorithm;
    l.masterVolume = p.master_volume;
    l.reverbMix = p.reverb_mix;
    l.reverbRoomSize = p.reverb_room_size;
    l.reverbDecay = p.reverb_decay;
    l.reverbDamping = p.reverb_damping;
    l.reverbWidth = p.reverb_width;
    l.reverbDiffusion = p.reverb_diffusion;
    l.reverbPreDelayMs = p.reverb_pre_delay_ms;
    l.reverbEarlyLevel = p.reverb_early_level;
    l.reverbLateLevel = p.reverb_late_level;
    l.reverbModDepth = p.reverb_mod_depth;
    l.reverbModRate = p.reverb_mod_rate;
    l.reverbLowCutHz = p.reverb_low_cut_hz;
    l.reverbHighCutHz = p.reverb_high_cut_hz;
    l.limiterThreshold = p.limiter_threshold;
    l.limiterLookaheadMs = p.limiter_lookahead_ms;
    l.limiterAttackMs = p.limiter_attack_ms;
    l.limiterReleaseMs = p.limiter_release_ms;
}

// The runtime-link-only part of SVMS_TelemetryV3 (everything except the V2
// census, which the in-process driver fills from its own counters).
inline void FillTelemetryV3Extension(const RuntimeLinkTelemetryV2& t,
                                     SVMS_TelemetryV3& r) {
    r.publish_timestamp = t.timestampQpc;
    r.max_voices = t.maxVoices;
    r.releasing_voices = t.releasingVoices;
    r.free_top = t.freeTop;
    r.retired_count = t.retiredCount;
    r.retired_immediate_count = t.retiredImmediateCount;
    r.decimation_step = t.decimationStep;
    r.audio_hresult = t.audioHResult;
    r.raw_ingress_count = t.rawIngressCount;
    r.compiled_paged_count = t.compiledPagedCount;
    r.scheduled_backlog_count = t.scheduledBacklogCount;
    r.cpu_load_percent = t.cpuLoadPercent;
    r.scheduler_percent = t.schedulerPercent;
    r.event_dispatch_percent = t.eventDispatchPercent;
    r.limiter_input_peak_l = t.limiterInputPeakL;
    r.limiter_input_peak_r = t.limiterInputPeakR;
    r.limiter_output_peak_l = t.limiterOutputPeakL;
    r.limiter_output_peak_r = t.limiterOutputPeakR;
    r.limiter_gain_reduction_db = t.limiterGainReductionDb;
    r.channel_limiter_enabled = t.channelLimiterEnabled;
    r.gpu_mode = t.gpuMode;
    r.gpu_share_percent = t.gpuSharePercent;
    r.gpu_rows = t.gpuRows;
    r.gpu_late_blocks = t.gpuLateBlocks;
    static_assert(sizeof(r.channel_limiter_input_peak) ==
                      sizeof(t.channelLimiterInputPeak),
                  "per-channel meter count must match the runtime link");
    std::memcpy(r.channel_limiter_gain_reduction_db,
                t.channelLimiterGainReductionDb,
                sizeof(r.channel_limiter_gain_reduction_db));
    std::memcpy(r.channel_limiter_input_peak, t.channelLimiterInputPeak,
                sizeof(r.channel_limiter_input_peak));
    ApiFromLive(t.live, r.live);
    r.live.max_voices = t.maxVoices;
    std::memcpy(r.soundfont_name, t.soundFontName, sizeof(r.soundfont_name));
}

// RLResult <-> SVMS_Result: the native API now carries the live-control
// outcomes (restart required, load failed, busy) the configurator relies on.
inline SVMS_Result ApiResultFromRL(RLResult r) {
    switch (r) {
        case RLResult::Ok: return SVMS_RESULT_OK;
        case RLResult::InvalidArgument: return SVMS_RESULT_INVALID_ARGUMENT;
        case RLResult::Unsupported: return SVMS_RESULT_UNSUPPORTED;
        case RLResult::Busy: return SVMS_RESULT_BUSY;
        case RLResult::RestartRequired: return SVMS_RESULT_RESTART_REQUIRED;
        case RLResult::LoadFailed: return SVMS_RESULT_LOAD_FAILED;
        default: return SVMS_RESULT_INTERNAL_ERROR;
    }
}

inline RLResult RLFromApiResult(SVMS_Result r) {
    switch (r) {
        case SVMS_RESULT_OK: return RLResult::Ok;
        case SVMS_RESULT_INVALID_ARGUMENT: return RLResult::InvalidArgument;
        case SVMS_RESULT_UNSUPPORTED:
        case SVMS_RESULT_UNSUPPORTED_ABI: return RLResult::Unsupported;
        case SVMS_RESULT_BUSY:
        case SVMS_RESULT_NO_RESOURCES: return RLResult::Busy;
        case SVMS_RESULT_RESTART_REQUIRED: return RLResult::RestartRequired;
        case SVMS_RESULT_LOAD_FAILED: return RLResult::LoadFailed;
        default: return RLResult::InternalError;
    }
}

} // namespace svms

#endif
