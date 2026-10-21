// Stack-wide preset resolution: which SoundFont of a priority-ordered stack
// answers a channel's bank/program. Pure (no driver types) so tests can drive it.
#pragma once

#include "synth/Config.h"
#include "synth/SoundFont.h"

namespace svms {

// An SFZ is one instrument with no bank/program map: alone it answers every
// program, but in a stack it only claims Acoustic Grand Piano by default so
// it cannot swallow the other SoundFonts' programs. It is still the last
// resort when nothing else matches, and routes / per-channel assignment
// override both rules.
inline bool SfzClaimsProgram(uint8_t program, bool percussion) noexcept {
    return !percussion && program == 0u;
}

// `channelFont` < 0 means automatic. Order: explicit routes, the channel's
// assigned SoundFont, stack priority (SFZ gated by SfzClaimsProgram), then
// any SFZ as last resort.
inline bool ResolveStackPreset(const SF2Data* const* banks, uint32_t bankCount,
                               const SoundFontRoute* routes, uint32_t routeCount,
                               int channelFont, uint16_t bank, uint8_t program,
                               bool percussion, uint8_t* outFont,
                               uint32_t* outPreset) {
    for (uint32_t i = 0u; i < routeCount; ++i) {
        const SoundFontRoute& route = routes[i];
        if (route.targetBank != bank || route.percussion != percussion ||
            (route.targetPreset >= 0 && route.targetPreset != program))
            continue;
        if (route.soundFontIndex >= bankCount || !banks[route.soundFontIndex])
            continue;
        const uint16_t sourcePreset = route.sourcePreset >= 0
            ? static_cast<uint16_t>(route.sourcePreset) : program;
        if (sf2_find_preset(banks[route.soundFontIndex], route.sourceBank,
                            sourcePreset, outPreset)) {
            *outFont = static_cast<uint8_t>(route.soundFontIndex);
            return true;
        }
    }
    if (channelFont >= 0 && static_cast<uint32_t>(channelFont) < bankCount &&
        banks[channelFont] &&
        sf2_resolve_preset(banks[channelFont], bank, program, percussion,
                           outPreset)) {
        *outFont = static_cast<uint8_t>(channelFont);
        return true;
    }
    for (uint32_t i = 0u; i < bankCount; ++i) {
        const SF2Data* data = banks[i];
        if (!data) continue;
        if (data->isSfz && bankCount > 1u &&
            !SfzClaimsProgram(program, percussion)) continue;
        if (sf2_resolve_preset(data, bank, program, percussion, outPreset)) {
            *outFont = static_cast<uint8_t>(i);
            return true;
        }
    }
    for (uint32_t i = 0u; i < bankCount; ++i) {
        const SF2Data* data = banks[i];
        if (data && data->isSfz &&
            sf2_resolve_preset(data, bank, program, percussion, outPreset)) {
            *outFont = static_cast<uint8_t>(i);
            return true;
        }
    }
    return false;
}

} // namespace svms
