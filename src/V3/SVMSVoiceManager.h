#ifndef SVMS_VOICE_MANAGER_H
#define SVMS_VOICE_MANAGER_H

#include "SVMSTypes.h"
#include "SVMSEnvelope.h"
#include <cstring>

namespace svms {

enum StealClass : uint8_t {
    StealClassNone = 0,
    StealClassActiveLoud = 1,
    StealClassActiveQuiet = 2,
    StealClassReleaseLoud = 3,
    StealClassReleaseQuiet = 4,
};

static constexpr uint8_t kStealQuietVelocity = 60;

// A voice released faster than this cannot complete a single oscillation
// cycle of a mid-range note (~169 samples for Middle C at 44.1kHz), so it
// reads as a click/mute instead of a note. Every adaptive release tier
// below is floored here regardless of pool pressure.
static constexpr float kMinReleaseSeconds = 0.010f; // 10ms

// Pool-pressure-scaled release time: shorter releases free SoA slots
// sooner under heavy polyphony, but never below kMinReleaseSeconds.
inline float ComputeAdaptiveReleaseSeconds(float poolPressure,
                                            float highPressureSeconds,
                                            float medPressureSeconds,
                                            float lowPressureSeconds,
                                            float idleSeconds) {
    float seconds;
    if (poolPressure > 0.90f)      seconds = highPressureSeconds;
    else if (poolPressure > 0.70f) seconds = medPressureSeconds;
    else if (poolPressure > 0.50f) seconds = lowPressureSeconds;
    else                            seconds = idleSeconds;
    return seconds < kMinReleaseSeconds ? kMinReleaseSeconds : seconds;
}

class VoiceManager {
public:
    VoiceManager();
    void Initialize(uint32_t maxVoices, uint32_t sampleRate = 44100);
    void Reset();

    VoiceHandle AllocateVoice(uint8_t channel, uint8_t note, uint8_t velocity);

    // Allocate a fresh voice or steal the lowest-priority voice if the
    // pool is full.  When `outStolen` is non-null and a steal occurred,
    // `*outStolen` is set to `true` so the caller can apply a micro-fade
    // attack ramp (64-sample forced attack) to prevent a click on the
    // stolen slot.
    VoiceHandle AllocateVoiceOrSteal(uint8_t channel, uint8_t note, uint8_t velocity,
                                      bool* outStolen = nullptr);
    VoiceHandle StealVoice();
    void ReleaseVoice(VoiceHandle handle);
    void RetireVoice(VoiceHandle handle);
    bool IsActive(VoiceHandle handle) const;

    // ── In-place voice recycling (BassMIDI architecture) ──────────────

    // Return the first Active (non-releasing) voice for the given
    // (channel, note) pair, or kInvalidVoice if none exists.
    VoiceHandle FindActiveVoiceForChannelKey(uint8_t channel, uint8_t note) const;

    // Recycle an existing voice in-place for a retrigger on the same
    // (channel, key) pair.  Resets phase accumulator to 0, snaps the
    // envelope back to Attack, and updates velocity.  The channelKey
    // linkage is preserved (the voice stays at the same linked-list
    // position).  The caller must still re-configure sample ranges,
    // envelope parameters, and gains via SetVoiceSample/SetVoiceEnvelope/
    // SetVoiceGain.
    void RecycleVoiceForChannelKey(VoiceHandle handle, uint8_t channel,
                                   uint8_t note, uint8_t velocity);

    // Release *all* active voices for (channel, note) EXCEPT the one
    // specified by `keepHandle`.  Used after in-place recycling to clean
    // up surplus duplicate voices from multi-layer instruments.  Release
    // time is forced to kMinReleaseSeconds (10ms) since these are
    // redundant duplicates.
    void ReleaseOtherVoicesForChannelKey(uint8_t channel, uint8_t note,
                                          VoiceHandle keepHandle,
                                          uint32_t blockOffset);

    void SetVoiceSample(VoiceHandle handle, uint32_t start, uint32_t end,
                        uint32_t loopStart, uint32_t loopEnd, uint8_t loopMode,
                        float phaseStep, uint8_t sampleBacked);

    void SetVoiceEnvelope(VoiceHandle handle, float initialGain, float sustainLevel,
                          uint32_t delaySamples, uint32_t holdSamples, uint32_t attackSamples,
                          uint32_t decaySamples, float attackGainStep,
                          float decaySlope, float releaseDecay);

    void SetVoiceGain(VoiceHandle handle, float left, float right);

    void StartRelease(VoiceHandle handle);

    // Force-end ALL voices on (channel, note) with adaptive release time.
    // No longer called from HandleNoteOn (replaced by in-place recycling).
    // May still be used for panic / all-notes-off scenarios.
    void EndVoicesForChannelKey(uint8_t channel, uint8_t note, uint32_t blockOffset);

    uint32_t GetActiveCount() const;
    uint32_t GetMaxVoices() const;

    VoiceSoA v;

private:
    uint32_t maxVoices_;
    uint32_t activeCount_;
    uint32_t sampleRate_;

    int32_t stealHeads_[5];
    int32_t stealTails_[5];
    int32_t freeListHead_;
    int32_t channelKeyVoiceHead[kChannelCount][kNoteCount];

    void AddToStealList(VoiceHandle handle);
    void RemoveFromStealList(VoiceHandle handle);
    void InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity);
    void LinkChannelKey(VoiceHandle handle);
    void UnlinkChannelKey(VoiceHandle handle);
    uint8_t ComputeStealClass(uint8_t velocity, uint8_t state) const;
};

inline VoiceManager::VoiceManager()
    : maxVoices_(0), activeCount_(0), sampleRate_(44100), freeListHead_(-1) {
    std::memset(&v, 0, sizeof(v));
    for (int i = 0; i < 5; ++i) {
        stealHeads_[i] = -1;
        stealTails_[i] = -1;
    }
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n)
            channelKeyVoiceHead[ch][n] = -1;
}

inline void VoiceManager::Initialize(uint32_t maxVoices, uint32_t sampleRate) {
    maxVoices_ = maxVoices < kMaxPolyphony ? maxVoices : kMaxPolyphony;
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100;
    Reset();
}

inline void VoiceManager::Reset() {
    std::memset(&v, 0, sizeof(v));
    activeCount_ = 0;
    freeListHead_ = 0;
    for (int i = 0; i < 5; ++i) {
        stealHeads_[i] = -1;
        stealTails_[i] = -1;
    }
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n)
            channelKeyVoiceHead[ch][n] = -1;
    for (uint32_t i = 0; i < maxVoices_; ++i) {
        v.state[i] = static_cast<uint8_t>(VoiceState::Free);
        v.nextStealVoice[i] = -1;
        v.prevStealVoice[i] = -1;
        v.stealClass[i] = StealClassNone;
        v.freeListNext[i] = (int32_t)(i + 1);
    }
    if (maxVoices_ > 0)
        v.freeListNext[maxVoices_ - 1] = -1;
}

inline uint8_t VoiceManager::ComputeStealClass(uint8_t velocity, uint8_t state) const {
    if (state == static_cast<uint8_t>(VoiceState::Free)) return StealClassNone;
    bool quiet = (velocity <= kStealQuietVelocity);
    if (state == static_cast<uint8_t>(VoiceState::Releasing))
        return quiet ? StealClassReleaseQuiet : StealClassReleaseLoud;
    return quiet ? StealClassActiveQuiet : StealClassActiveLoud;
}

inline void VoiceManager::AddToStealList(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    uint8_t sc = ComputeStealClass(v.velocity[handle], v.state[handle]);
    if (sc == StealClassNone) return;
    v.stealClass[handle] = sc;
    v.nextStealVoice[handle] = -1;
    v.prevStealVoice[handle] = stealTails_[sc];
    if (stealTails_[sc] >= 0)
        v.nextStealVoice[stealTails_[sc]] = (int32_t)handle;
    else
        stealHeads_[sc] = (int32_t)handle;
    stealTails_[sc] = (int32_t)handle;
}

inline void VoiceManager::RemoveFromStealList(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    uint8_t sc = v.stealClass[handle];
    if (sc == StealClassNone) return;
    int32_t prev = v.prevStealVoice[handle];
    int32_t next = v.nextStealVoice[handle];
    if (prev >= 0) v.nextStealVoice[prev] = next;
    else stealHeads_[sc] = next;
    if (next >= 0) v.prevStealVoice[next] = prev;
    else stealTails_[sc] = prev;
    v.nextStealVoice[handle] = -1;
    v.prevStealVoice[handle] = -1;
    v.stealClass[handle] = StealClassNone;
}

inline void VoiceManager::InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity) {
    v.state[handle] = static_cast<uint8_t>(VoiceState::Active);
    v.channel[handle] = channel;
    v.note[handle] = note;
    v.velocity[handle] = velocity;
    v.phases[handle] = 0.0f;
    v.phaseIncs[handle] = 0.0f;
    v.currentGain[handle] = 0.0f;
    v.targetGain[handle] = 1.0f;
    v.sustainLevel[handle] = 0.7f;
    v.attackGainStep[handle] = 0.0f;
    v.decayGainStep[handle] = 0.0f;
    v.releaseDecay[handle] = kDefaultReleaseDecay;
    v.gainLeft[handle] = 1.0f;
    v.gainRight[handle] = 1.0f;
    v.sampleStart[handle] = 0;
    v.sampleEnd[handle] = 0;
    v.loopStart[handle] = 0;
    v.loopEnd[handle] = 0;
    v.loopMode[handle] = 0;
    v.sampleBacked[handle] = 0;
    v.holdSamplesRemaining[handle] = 0;
    v.attackSamplesRemaining[handle] = 0;
    v.decaySamplesRemaining[handle] = 0;
    v.delaySamplesRemaining[handle] = 0;
    v.decaySlope[handle] = 1.0f;
    v.samplePageId[handle] = 0;
    v.envelopeStage[handle] = 0;
    v.heldBySustain[handle] = 0;
    v.releaseStartInBlock[handle] = 0;
    v.nextChannelKeyVoice[handle] = -1;
    v.phaseOffset[handle] = 0.0f;
}

inline void VoiceManager::LinkChannelKey(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    uint8_t ch = v.channel[handle];
    uint8_t nt = v.note[handle];
    v.nextChannelKeyVoice[handle] = channelKeyVoiceHead[ch][nt];
    channelKeyVoiceHead[ch][nt] = (int32_t)handle;
}

inline void VoiceManager::UnlinkChannelKey(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    uint8_t ch = v.channel[handle];
    uint8_t nt = v.note[handle];
    int32_t prev = -1;
    int32_t cur = channelKeyVoiceHead[ch][nt];
    while (cur >= 0) {
        if (cur == (int32_t)handle) {
            if (prev >= 0)
                v.nextChannelKeyVoice[prev] = v.nextChannelKeyVoice[cur];
            else
                channelKeyVoiceHead[ch][nt] = v.nextChannelKeyVoice[cur];
            v.nextChannelKeyVoice[handle] = -1;
            return;
        }
        prev = cur;
        cur = v.nextChannelKeyVoice[cur];
    }
}

// ── Impl: In-place voice recycling (BassMIDI architecture) ─────────────

inline VoiceHandle VoiceManager::FindActiveVoiceForChannelKey(uint8_t channel,
                                                                uint8_t note) const {
    int32_t idx = channelKeyVoiceHead[channel][note];
    while (idx >= 0) {
        if (v.state[idx] == static_cast<uint8_t>(VoiceState::Active))
            return static_cast<VoiceHandle>(idx);
        idx = v.nextChannelKeyVoice[idx];
    }
    return kInvalidVoice;
}

inline void VoiceManager::RecycleVoiceForChannelKey(VoiceHandle handle,
                                                      uint8_t channel,
                                                      uint8_t note,
                                                      uint8_t velocity) {
    if (handle >= maxVoices_) return;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return;

    // Leave the channelKey link in place — the voice stays at its current
    // linked-list position for the (channel, note) pair.
    // Remove from old steal class (old velocity may map differently).
    RemoveFromStealList(handle);

    // Hard-sync: reset phase accumulator to 0, snap envelope back to
    // the Attack stage with currentGain=0.  The caller will reconfigure
    // sample ranges and envelope/gain via SetVoiceSample/SetVoiceEnvelope.
    v.channel[handle] = channel;
    v.note[handle] = note;
    v.velocity[handle] = velocity;
    v.state[handle] = static_cast<uint8_t>(VoiceState::Active);
    v.phases[handle] = 0.0f;
    v.phaseIncs[handle] = 0.0f;
    v.currentGain[handle] = 0.0f;
    v.targetGain[handle] = 1.0f;
    v.sustainLevel[handle] = 0.7f;
    v.attackGainStep[handle] = 0.0f;
    v.decayGainStep[handle] = 0.0f;
    v.releaseDecay[handle] = kDefaultReleaseDecay;
    v.gainLeft[handle] = 1.0f;
    v.gainRight[handle] = 1.0f;
    v.sampleStart[handle] = 0;
    v.sampleEnd[handle] = 0;
    v.loopStart[handle] = 0;
    v.loopEnd[handle] = 0;
    v.loopMode[handle] = 0;
    v.sampleBacked[handle] = 0;
    v.holdSamplesRemaining[handle] = 0;
    v.attackSamplesRemaining[handle] = 0;
    v.decaySamplesRemaining[handle] = 0;
    v.delaySamplesRemaining[handle] = 0;
    v.decaySlope[handle] = 1.0f;
    v.samplePageId[handle] = 0;
    v.envelopeStage[handle] = 0;
    v.heldBySustain[handle] = 0;
    v.releaseStartInBlock[handle] = 0;
    v.phaseOffset[handle] = 0.0f;

    // Re-add to steal list with the new velocity classification.
    AddToStealList(handle);
}

inline void VoiceManager::ReleaseOtherVoicesForChannelKey(uint8_t channel,
                                                            uint8_t note,
                                                            VoiceHandle keepHandle,
                                                            uint32_t blockOffset) {
    float releaseDecay = MakeReleaseDecay(kMinReleaseSeconds, sampleRate_);

    int32_t idx = channelKeyVoiceHead[channel][note];
    while (idx >= 0) {
        int32_t next = v.nextChannelKeyVoice[idx];
        if ((uint32_t)idx != keepHandle) {
            if (v.state[idx] == static_cast<uint8_t>(VoiceState::Active)) {
                v.releaseDecay[idx] = releaseDecay;
                v.releaseStartInBlock[idx] = blockOffset;
                StartRelease(static_cast<VoiceHandle>(idx));
            }
        }
        idx = next;
    }
}
inline void VoiceManager::EndVoicesForChannelKey(uint8_t channel, uint8_t note,
                                                   uint32_t blockOffset) {
    float pressure = maxVoices_ > 0 ? (float)activeCount_ / (float)maxVoices_ : 0.0f;
    float releaseSeconds = ComputeAdaptiveReleaseSeconds(pressure,
                                                          0.015f,  // >90% full -> 15ms
                                                          0.020f,  // >70% full -> 20ms
                                                          0.100f,  // >50% full -> 100ms
                                                          0.300f); // otherwise  -> 300ms
    float releaseDecay = MakeReleaseDecay(releaseSeconds, sampleRate_);

    int32_t idx = channelKeyVoiceHead[channel][note];
    while (idx >= 0) {
        int32_t next = v.nextChannelKeyVoice[idx];
        if (v.state[idx] != static_cast<uint8_t>(VoiceState::Free)) {
            if (v.state[idx] == static_cast<uint8_t>(VoiceState::Active)) {
                // The voice will be retired by ProcessVoice once gain drops
                // below kVoiceRetireThreshold.
                v.releaseDecay[idx] = releaseDecay;
                v.releaseStartInBlock[idx] = blockOffset;
                StartRelease(static_cast<VoiceHandle>(idx));
            }
        }
        idx = next;
    }
}

inline VoiceHandle VoiceManager::AllocateVoice(uint8_t channel, uint8_t note, uint8_t velocity) {
    int32_t i = freeListHead_;
    if (i < 0) return kInvalidVoice;
    freeListHead_ = v.freeListNext[i];

    InitializeVoice(static_cast<VoiceHandle>(i), channel, note, velocity);
    ++activeCount_;
    AddToStealList(static_cast<VoiceHandle>(i));
    LinkChannelKey(static_cast<VoiceHandle>(i));
    return static_cast<VoiceHandle>(i);
}

inline VoiceHandle VoiceManager::AllocateVoiceOrSteal(uint8_t channel, uint8_t note,
                                                        uint8_t velocity,
                                                        bool* outStolen) {
    VoiceHandle vh = AllocateVoice(channel, note, velocity);
    if (vh != kInvalidVoice) {
        if (outStolen) *outStolen = false;
        return vh;
    }

    // Pool is full — steal the lowest-priority voice.  This path is
    // extremely rare when in-place recycling is used (active polyphony
    // stays bounded to ~128-256 voices).  The caller receives `*outStolen =
    // true` so it can force a 64-sample attack ramp on the new voice,
    // providing a click-free micro-fade onset on the stolen slot.
    static const uint8_t stealOrder[] = {
        StealClassReleaseQuiet, StealClassReleaseLoud,
        StealClassActiveQuiet, StealClassActiveLoud
    };
    for (int si = 0; si < 4; ++si) {
        int32_t idx = stealHeads_[stealOrder[si]];
        if (idx >= 0) {
            VoiceHandle stolen = static_cast<VoiceHandle>(idx);
            RemoveFromStealList(stolen);
            UnlinkChannelKey(stolen);
            --activeCount_;
            InitializeVoice(stolen, channel, note, velocity);
            ++activeCount_;
            AddToStealList(stolen);
            LinkChannelKey(stolen);
            if (outStolen) *outStolen = true;
            return stolen;
        }
    }
    return kInvalidVoice;
}

inline VoiceHandle VoiceManager::StealVoice() {
    static const uint8_t stealOrder[] = {
        StealClassReleaseQuiet, StealClassReleaseLoud,
        StealClassActiveQuiet, StealClassActiveLoud
    };
    for (int si = 0; si < 4; ++si) {
        int32_t idx = stealHeads_[stealOrder[si]];
        if (idx >= 0) {
            VoiceHandle stolen = static_cast<VoiceHandle>(idx);
            UnlinkChannelKey(stolen);
            RemoveFromStealList(stolen);
            --activeCount_;
            return stolen;
        }
    }
    return kInvalidVoice;
}

inline void VoiceManager::ReleaseVoice(VoiceHandle handle) {
    if (handle < maxVoices_ && v.state[handle] != static_cast<uint8_t>(VoiceState::Free)) {
        v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
        --activeCount_;
    }
}

inline void VoiceManager::RetireVoice(VoiceHandle handle) {
    if (handle >= maxVoices_ || v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return;
    UnlinkChannelKey(handle);
    RemoveFromStealList(handle);
    v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
    v.currentGain[handle] = 0.0f;
    v.freeListNext[handle] = freeListHead_;
    freeListHead_ = (int32_t)handle;
    --activeCount_;
}

inline bool VoiceManager::IsActive(VoiceHandle handle) const {
    return handle < maxVoices_ && v.state[handle] != static_cast<uint8_t>(VoiceState::Free);
}

inline void VoiceManager::SetVoiceSample(VoiceHandle handle, uint32_t start, uint32_t end,
                                          uint32_t loopStart, uint32_t loopEnd, uint8_t loopMode,
                                          float phaseStep, uint8_t sb) {
    if (handle >= maxVoices_) return;
    v.sampleStart[handle] = start;
    v.sampleEnd[handle] = end;
    v.loopStart[handle] = loopStart;
    v.loopEnd[handle] = loopEnd;
    v.loopMode[handle] = loopMode;
    v.phaseIncs[handle] = phaseStep;
    v.sampleBacked[handle] = sb;

    // Phase randomization: prevent coherent phase peaks when many same-note
    // voices trigger simultaneously (all starting at identical sample
    // positions).  A deterministic hash of the voice handle scatters
    // initial phase across ~0-31 input samples (~0.7ms @ 44.1kHz),
    // which is below common temporal resolution thresholds while
    // providing enough decorrelation to reduce peak stacking.
    v.phases[handle] = static_cast<float>((handle * 1103515245u) & 31);
}

inline void VoiceManager::SetVoiceEnvelope(VoiceHandle handle, float initialGain,
                                            float sustainLevel, uint32_t delaySamples,
                                            uint32_t holdSamples, uint32_t attackSamples,
                                            uint32_t decaySamples, float attackGainStep,
                                            float decaySlope, float releaseDecay) {
    if (handle >= maxVoices_) return;
    v.targetGain[handle] = initialGain;
    v.sustainLevel[handle] = sustainLevel * initialGain;
    v.delaySamplesRemaining[handle] = delaySamples;
    v.holdSamplesRemaining[handle] = holdSamples;
    v.attackSamplesRemaining[handle] = attackSamples;
    v.decaySamplesRemaining[handle] = decaySamples;
    v.attackGainStep[handle] = attackGainStep;
    v.decaySlope[handle] = decaySlope;
    v.releaseDecay[handle] = releaseDecay;
    v.currentGain[handle] = 0.0f;

    if (delaySamples > 0)
        v.envelopeStage[handle] = 4;
    else if (holdSamples > 0)
        v.envelopeStage[handle] = 0;
    else if (attackSamples > 0)
        v.envelopeStage[handle] = 1;
    else if (decaySamples > 0)
        v.envelopeStage[handle] = 2;
    else {
        v.envelopeStage[handle] = 3;
        v.currentGain[handle] = initialGain;
    }
}

inline void VoiceManager::SetVoiceGain(VoiceHandle handle, float left, float right) {
    if (handle >= maxVoices_) return;
    v.gainLeft[handle] = left;
    v.gainRight[handle] = right;
}

inline void VoiceManager::StartRelease(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Active)) {
        UnlinkChannelKey(handle);
        RemoveFromStealList(handle);
        v.state[handle] = static_cast<uint8_t>(VoiceState::Releasing);
        AddToStealList(handle);
    }
}

inline uint32_t VoiceManager::GetActiveCount() const {
    return activeCount_;
}

inline uint32_t VoiceManager::GetMaxVoices() const {
    return maxVoices_;
}

} // namespace svms

#endif
