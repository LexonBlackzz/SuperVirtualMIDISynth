#ifndef SVMS_CHANNEL_CACHE_H
#define SVMS_CHANNEL_CACHE_H

#include "SVMSTypes.h"
#include "SVMSConfig.h"
#include <cmath>
#include <cstring>

namespace svms {

// MIDI pitch-wheel range used by V3.  This is intentionally wider than the
// General MIDI default so Black MIDI bends can cover a full octave in either
// direction without changing the event format or renderer path.
static constexpr float kDefaultPitchBendRangeSemitones = 12.0f;

class ChannelCache {
public:
    ChannelCache();

    void Reset();
    void SetMasterVolume(float vol);

    void NoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t channel, uint8_t note);
    void ControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    void ProgramChange(uint8_t channel, uint8_t program);
    void SetSelectedPreset(uint8_t channel, uint16_t presetIndex);
    void ResetControllers(uint8_t channel);
    void PitchBend(uint8_t channel, int16_t value);
    void AllNotesOff(uint8_t channel);
    void AllSoundOff(uint8_t channel);

    void RebuildCache(const struct RuntimeConfigSnapshot& cfg, float sampleRate);
    void RebuildChannel(uint8_t channel,
                        const struct RuntimeConfigSnapshot& cfg,
                        float sampleRate);
    const ChannelParamsSnapshot* GetParams() const;
    uint8_t GetProgram(uint8_t channel) const;
    uint8_t GetBankMSB(uint8_t channel) const;
    uint8_t GetBankLSB(uint8_t channel) const;
    uint16_t GetBank(uint8_t channel) const;
    uint16_t GetSelectedPreset(uint8_t channel) const;
    bool IsSustainActive(uint8_t channel) const;
    float GetPitchBendSemitones(uint8_t channel) const;

    float ComputeVelocity(uint8_t velocity, const struct RuntimeConfigSnapshot& cfg) const;
    void ComputeSoundFontPan(int16_t pan, float& outLeft, float& outRight) const;
    float ComputePanGain(uint8_t pan, float& outLeft, float& outRight,
                         const struct RuntimeConfigSnapshot& cfg) const;
    float NoteToFrequency(uint8_t note) const;

private:
    ChannelParamsSnapshot channels_[kChannelCount];
    uint8_t channelVolume_[kChannelCount];
    uint8_t channelExpression_[kChannelCount];
    uint8_t channelPan_[kChannelCount];
    int16_t channelPitchBend_[kChannelCount];
    uint8_t channelSustain_[kChannelCount];
    uint8_t channelProgram_[kChannelCount];
    uint8_t channelBankMSB_[kChannelCount];
    uint8_t channelBankLSB_[kChannelCount];
    uint16_t channelSelectedPreset_[kChannelCount];
    bool noteActive_[kChannelCount][kNoteCount];
    float masterVolume_;
};

inline ChannelCache::ChannelCache() : masterVolume_(1.0f) {
    Reset();
}

inline void ChannelCache::Reset() {
    for (uint32_t ch = 0; ch < kChannelCount; ++ch) {
        channelVolume_[ch] = 100;
        channelExpression_[ch] = 127;
        channelPan_[ch] = 64;
        channelPitchBend_[ch] = 8192;
        channelSustain_[ch] = 0;
        channelProgram_[ch] = 0;
        channelBankMSB_[ch] = 0;
        channelBankLSB_[ch] = 0;
        channelSelectedPreset_[ch] = 0;
        for (uint32_t n = 0; n < kNoteCount; ++n) {
            noteActive_[ch][n] = false;
        }
    }
}

inline void ChannelCache::SetMasterVolume(float vol) {
    masterVolume_ = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
}

inline void ChannelCache::NoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    (void)velocity;
    if (channel < kChannelCount && note < kNoteCount) {
        noteActive_[channel][note] = true;
    }
}

inline void ChannelCache::NoteOff(uint8_t channel, uint8_t note) {
    if (channel < kChannelCount && note < kNoteCount) {
        if (channelSustain_[channel] >= 64) {
            return;
        }
        noteActive_[channel][note] = false;
    }
}

inline void ChannelCache::ControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (channel >= kChannelCount) return;
    switch (controller) {
        case 7:  channelVolume_[channel] = value; break;
        case 11: channelExpression_[channel] = value; break;
        case 10: channelPan_[channel] = value; break;
        case 0:  channelBankMSB_[channel] = value; break;
        case 32: channelBankLSB_[channel] = value; break;
        case 64: channelSustain_[channel] = value; break;
        case 120: AllSoundOff(channel); break;
        case 121: ResetControllers(channel); break;
        case 123: AllNotesOff(channel); break;
        default: break;
    }
}

inline void ChannelCache::ProgramChange(uint8_t channel, uint8_t program) {
    if (channel < kChannelCount) {
        channelProgram_[channel] = program;
    }
}

inline void ChannelCache::SetSelectedPreset(uint8_t channel, uint16_t presetIndex) {
    if (channel < kChannelCount)
        channelSelectedPreset_[channel] = presetIndex;
}

inline void ChannelCache::ResetControllers(uint8_t channel) {
    if (channel >= kChannelCount) return;
    channelVolume_[channel] = 100;
    channelExpression_[channel] = 127;
    channelPan_[channel] = 64;
    channelPitchBend_[channel] = 8192;
    channelSustain_[channel] = 0;
}

inline void ChannelCache::PitchBend(uint8_t channel, int16_t value) {
    if (channel < kChannelCount) {
        channelPitchBend_[channel] = value;
    }
}

inline void ChannelCache::AllNotesOff(uint8_t channel) {
    if (channel < kChannelCount) {
        for (uint32_t n = 0; n < kNoteCount; ++n) {
            noteActive_[channel][n] = false;
        }
    }
}

inline void ChannelCache::AllSoundOff(uint8_t channel) {
    AllNotesOff(channel);
}

inline void ChannelCache::RebuildCache(const RuntimeConfigSnapshot& cfg, float sampleRate) {
    for (uint32_t channel = 0; channel < kChannelCount; ++channel)
        RebuildChannel(static_cast<uint8_t>(channel), cfg, sampleRate);
}

inline void ChannelCache::RebuildChannel(uint8_t channel,
                                         const RuntimeConfigSnapshot& cfg,
                                         float sampleRate) {
    (void)sampleRate;
    if (channel >= kChannelCount) return;
    const uint32_t ch = channel;
    const float vol = masterVolume_ * (channelVolume_[ch] / 127.0f);
    channels_[ch].volume = vol;
    channels_[ch].expression = channelExpression_[ch] / 127.0f;

    ComputePanGain(static_cast<uint8_t>(channelPan_[ch]),
                   channels_[ch].panLeft, channels_[ch].panRight, cfg);

    const float bend = (channelPitchBend_[ch] - 8192.0f) / 8192.0f;
    channels_[ch].pitchBendCents =
        bend * (kDefaultPitchBendRangeSemitones * 100.0f);

    channels_[ch].sustainActive = channelSustain_[ch] >= 64 ? 1u : 0u;
    channels_[ch].filterCutoff = 20000.0f;
    channels_[ch].filterResonance = 0.0f;
    channels_[ch].modDepth = 0.0f;
}

inline const ChannelParamsSnapshot* ChannelCache::GetParams() const {
    return channels_;
}

inline uint8_t ChannelCache::GetBankMSB(uint8_t channel) const {
    return (channel < kChannelCount) ? channelBankMSB_[channel] : 0;
}

inline uint8_t ChannelCache::GetBankLSB(uint8_t channel) const {
    return (channel < kChannelCount) ? channelBankLSB_[channel] : 0;
}

inline uint16_t ChannelCache::GetBank(uint8_t channel) const {
    return (channel < kChannelCount)
        ? static_cast<uint16_t>((channelBankMSB_[channel] << 7) | channelBankLSB_[channel])
        : 0;
}

inline uint16_t ChannelCache::GetSelectedPreset(uint8_t channel) const {
    return (channel < kChannelCount) ? channelSelectedPreset_[channel] : 0;
}

inline uint8_t ChannelCache::GetProgram(uint8_t channel) const {
    return (channel < kChannelCount) ? channelProgram_[channel] : 0;
}

inline bool ChannelCache::IsSustainActive(uint8_t channel) const {
    return (channel < kChannelCount) && channelSustain_[channel] >= 64;
}

inline float ChannelCache::GetPitchBendSemitones(uint8_t channel) const {
    if (channel >= kChannelCount) return 0.0f;
    return (float)(channelPitchBend_[channel] - 8192) / 8192.0f
        * kDefaultPitchBendRangeSemitones;
}

inline float ChannelCache::ComputeVelocity(
    uint8_t velocity, const RuntimeConfigSnapshot& cfg) const {
    if (velocity == 0 || velocity < cfg.velocityIgnoreBelow) return 0.0f;
    if (cfg.ignoreVelocity) return 1.0f;

    // velocityIgnoreBelow is only an admission gate. It must not remap the
    // surviving range: velocity N has identical loudness whether the gate is
    // zero or N, matching the setting's "ignore below" wording.
    float normalized = static_cast<float>(velocity) / 127.0f;
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    float mapped = cfg.velocityFloor +
        (1.0f - cfg.velocityFloor) * ::powf(normalized, cfg.velocityCurve);
    if (mapped < 0.0f) mapped = 0.0f;
    if (mapped > 1.0f) mapped = 1.0f;
    return mapped;
}

inline void ChannelCache::ComputeSoundFontPan(int16_t pan, float& outLeft,
                                               float& outRight) const {
    float normalized = static_cast<float>(pan) / 500.0f;
    if (normalized < -1.0f) normalized = -1.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    // Relative constant-power gains: center is 1/1 so it leaves the channel
    // pan unchanged; hard left/right becomes sqrt(2)/0. Combined with the
    // channel's centered constant-power gain this yields unity on the
    // selected side without attenuating center-panned SF2 regions twice.
    outLeft = ::sqrtf(1.0f - normalized);
    outRight = ::sqrtf(1.0f + normalized);
}

inline float ChannelCache::ComputePanGain(uint8_t pan, float& outLeft, float& outRight,
                                           const RuntimeConfigSnapshot& cfg) const {
    if (cfg.monoOutput) {
        outLeft = 1.0f;
        outRight = 1.0f;
        return 1.0f;
    }
    float panNorm = (pan - 64.0f) / 64.0f;
    if (panNorm < -1.0f) panNorm = -1.0f;
    if (panNorm > 1.0f) panNorm = 1.0f;

    int panLaw = static_cast<int>(cfg.panLaw);
    switch (panLaw) {
        case 1: {
            float angle = (panNorm + 1.0f) * 0.25f * 3.14159265358979323846f;
            outLeft = cosf(angle);
            outRight = sinf(angle);
            break;
        }
        case 2: {
            outLeft = 1.0f - (panNorm > 0 ? panNorm : 0);
            outRight = 1.0f - (panNorm < 0 ? -panNorm : 0);
            break;
        }
        default: {
            outLeft = (1.0f - panNorm) * 0.5f;
            outRight = (panNorm + 1.0f) * 0.5f;
            break;
        }
    }
    return 1.0f;
}

inline float ChannelCache::NoteToFrequency(uint8_t note) const {
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}

} // namespace svms

#endif
