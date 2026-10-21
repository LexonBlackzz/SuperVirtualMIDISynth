#ifndef SVMS_CONFIGURATOR_LIVEPARAM_H
#define SVMS_CONFIGURATOR_LIVEPARAM_H

// Which live-state knob a widget changed. The ConfiguratorApp writes the value
// into its working SVMS_LiveParams and marks the knob's group dirty; the group
// travels in one set_live_params call. (Not a wire id: nothing here crosses
// the process boundary.)

#include "include/svmsapi.h"

#include <cstdint>

namespace svms::cfg {

enum class LiveParam : uint32_t {
    SetMasterVolume,
    SetReverbEnabled,
    SetReverbMix,
    SetReverbRoomSize,
    SetReverbDecay,
    SetReverbDamping,
    SetReverbWidth,
    SetReverbDiffusion,
    SetReverbPreDelayMs,
    SetReverbEarlyLevel,
    SetReverbLateLevel,
    SetReverbModDepth,
    SetReverbModRate,
    SetReverbLowCutHz,
    SetReverbHighCutHz,
    SetCorrectnessMode,
    SetLimiterEnabled,
    SetLimiterThreshold,
    SetLimiterLookahead,
    SetLimiterAttack,
    SetLimiterRelease,
};

inline uint32_t LiveParamGroup(LiveParam p) {
    switch (p) {
        case LiveParam::SetMasterVolume: return SVMS_LIVE_GROUP_MASTER;
        case LiveParam::SetCorrectnessMode: return SVMS_LIVE_GROUP_CORRECTNESS;
        case LiveParam::SetLimiterEnabled:
        case LiveParam::SetLimiterThreshold:
        case LiveParam::SetLimiterLookahead:
        case LiveParam::SetLimiterAttack:
        case LiveParam::SetLimiterRelease: return SVMS_LIVE_GROUP_LIMITER;
        default: return SVMS_LIVE_GROUP_REVERB;  // every Reverb* knob
    }
}

} // namespace svms::cfg

#endif
