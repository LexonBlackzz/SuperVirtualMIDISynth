#ifndef SVMS_VOICE_MANAGER_H
#define SVMS_VOICE_MANAGER_H

#include "SVMSTypes.h"
#include "SVMSEnvelope.h"
#include <cassert>
#include <cstring>

namespace svms {

// ════════════════════════════════════════════════════════════════════════
// VoiceManager — flat-array voice pool with score-based stealing.
//
// Replaces the old doubly-linked steal lists and freeList with flat arrays:
//   activeList[]  — indices of currently active/releasing voices
//   freeStack[]   — LIFO stack of free slot indices for O(1) allocation
//
// Stealing: when the pool is full, the least audible/important voice is
// replaced.  Released, quiet and low-velocity voices are preferred while
// loud (velocity >= 96) and newborn/attack voices are protected.  The old
// sample cursor is retained as a short fade tail so replacement never cuts a
// waveform at full amplitude.
// ════════════════════════════════════════════════════════════════════════
class VoiceManager {
public:
    VoiceManager();
    void Initialize(uint32_t maxVoices, uint32_t sampleRate = 44100);
    void Reset();

    // Allocate a fresh voice slot.  Returns kInvalidVoice when pool is full
    // (caller should then call AllocateVoiceOrSteal).
    VoiceHandle AllocateVoice(uint8_t channel, uint8_t note, uint8_t velocity);

    // Allocate a slot, stealing the lowest-priority voice if the pool is
    // full.  The manager captures the outgoing tail and arms the replacement
    // fade itself; `outStolen` is retained for diagnostics/tests.
    VoiceHandle AllocateVoiceOrSteal(uint8_t channel, uint8_t note, uint8_t velocity,
                                      bool* outStolen = nullptr);

    // Release a voice (transitions Active → Releasing).  Does NOT free the
    // slot — the voice continues rendering its release tail.
    void StartRelease(VoiceHandle handle);

    // Retire a voice (frees the slot immediately).  Called when the voice
    // has finished (end of sample, or gain below kVoiceRetireThreshold).
    void RetireVoice(VoiceHandle handle);

    bool IsActive(VoiceHandle handle) const;

    // ── Voice configuration — called once per note-on ──────────────────
    void SetVoiceSample(VoiceHandle handle, uint32_t start, uint32_t end,
                        uint32_t loopStart, uint32_t loopEnd, uint8_t loopMode,
                        float phaseStep, uint8_t sampleBacked);

    void SetVoiceSoundFontIdentity(VoiceHandle handle, uint16_t presetIndex,
                                   uint16_t regionIndex);
    void SetVoicePlayIndex(VoiceHandle handle, uint32_t playIndex);
    uint32_t FindOldestPlayIndex(uint8_t channel, uint8_t note) const;
    void StartReleaseForPlayIndex(uint8_t channel, uint8_t note,
                                  uint32_t playIndex);

    void SetVoiceEnvelope(VoiceHandle handle, float initialGain, float sustainLevel,
                          uint32_t delaySamples, uint32_t holdSamples, uint32_t attackSamples,
                          uint32_t decaySamples, float attackGainStep,
                          float decaySlope, float releaseDecay,
                          uint32_t releaseSamples = 0);

    void SetVoiceGain(VoiceHandle handle, float left, float right);

    // Premultiplied output gains (mixGainL/R = gainLeft/Right × pan × volume).
    // Called once per note-on (single) and once per block from RenderBlock
    // (all) so the per-sample mix path never touches ChannelParamsSnapshot.
    void RefreshMixGain(VoiceHandle handle, const ChannelParamsSnapshot& cp);
    void RefreshMixGains(const ChannelParamsSnapshot* chParams);

    // ── Channel-key utilities ──────────────────────────────────────────

    // Release all voices on a (channel, note) with a fixed fast release.
    // Used for panic / all-notes-off scenarios.
    void EndVoicesForChannelKey(uint8_t channel, uint8_t note, uint32_t blockOffset);
    void SilenceChannelImmediate(uint8_t channel);
    void ReleaseChannel(uint8_t channel, uint32_t blockOffset);

    uint32_t GetActiveCount() const { return activeCount_; }
    uint32_t GetMaxVoices() const { return maxVoices_; }
    void SetCurrentFrame(uint64_t frame) { currentFrame_ = frame; }
    uint32_t GetVoiceAge(VoiceHandle handle) const;

    // ── Public read-only access ────────────────────────────────────────
    VoiceSoA v;

    // Per-block voice iteration: activeList[0 .. activeCount_-1] holds
    // the indices of all non-Free voices.  The renderer and driver iterate
    // this list instead of scanning 0..maxVoices_.
    uint32_t activeCount_;
    uint32_t activeList_[kMaxPolyphony];
    uint32_t activePosition_[kMaxPolyphony];
    uint32_t freeTop_;

    void RebuildActivePositions();

    // Diagnostic counters (read from driver)
    uint32_t retireCount_;
    uint32_t retireImmediateCount_;
    uint32_t stealCount_;

private:
    uint32_t maxVoices_;
    uint32_t sampleRate_;
    uint32_t stealFadeFrames_;
    uint64_t currentFrame_;

    // LIFO free slot stack
    int32_t freeStack_[kMaxPolyphony];

    // Per-key tracking for EndVoicesForChannelKey
    int32_t channelKeyVoiceHead_[kChannelCount][kNoteCount];

    void InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity);
    void LinkChannelKey(VoiceHandle handle);
    void UnlinkChannelKey(VoiceHandle handle);

    // ── Score-based steal priority ─────────────────────────────────────
    // Computes a priority score for a voice.  HIGHER score = stolen FIRST.
    //   Released, low-output and low-velocity voices score higher.
    //   Velocity >= 96 and attack/newborn voices receive protection.
    float ComputeStealScore(uint32_t idx) const;
};

// ════════════════════════════════════════════════════════════════════════
// Implementation
// ════════════════════════════════════════════════════════════════════════

inline VoiceManager::VoiceManager()
    : maxVoices_(0), activeCount_(0), sampleRate_(44100), stealFadeFrames_(441),
      currentFrame_(0), freeTop_(0),
      retireCount_(0), retireImmediateCount_(0), stealCount_(0) {
    std::memset(&v, 0, sizeof(v));
    std::memset(activeList_, 0, sizeof(activeList_));
    std::memset(activePosition_, 0xff, sizeof(activePosition_));
    std::memset(freeStack_, 0, sizeof(freeStack_));
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n)
            channelKeyVoiceHead_[ch][n] = -1;
}

inline void VoiceManager::Initialize(uint32_t maxVoices, uint32_t sampleRate) {
    maxVoices_ = maxVoices < kMaxPolyphony ? maxVoices : kMaxPolyphony;
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100;
    stealFadeFrames_ = (sampleRate_ * kStealFadeMilliseconds + 999u) / 1000u;
    if (stealFadeFrames_ < kNewbornProtectSamples)
        stealFadeFrames_ = kNewbornProtectSamples;
    Reset();
}

inline void VoiceManager::Reset() {
    std::memset(&v, 0, sizeof(v));
    std::memset(activeList_, 0, sizeof(activeList_));
    std::memset(activePosition_, 0xff, sizeof(activePosition_));
    activeCount_ = 0;
    currentFrame_ = 0;
    retireCount_ = 0;
    retireImmediateCount_ = 0;
    stealCount_ = 0;
    freeTop_ = maxVoices_;
    for (uint32_t i = 0; i < maxVoices_; ++i) {
        v.state[i] = static_cast<uint8_t>(VoiceState::Free);
        v.nextChannelKeyVoice[i] = -1;
        freeStack_[i] = static_cast<int32_t>(i);
    }
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n)
            channelKeyVoiceHead_[ch][n] = -1;
}

inline uint32_t VoiceManager::GetVoiceAge(VoiceHandle handle) const {
    if (handle >= maxVoices_ || currentFrame_ <= v.birthFrame[handle]) return 0;
    const uint64_t age = currentFrame_ - v.birthFrame[handle];
    return age > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age);
}

inline float VoiceManager::ComputeStealScore(uint32_t idx) const {
    const uint32_t ageFrames = GetVoiceAge(static_cast<VoiceHandle>(idx));
    const float ageSeconds = static_cast<float>(ageFrames) /
                             static_cast<float>(sampleRate_);
    const float cappedAge = ageSeconds < 10.0f ? ageSeconds : 10.0f;

    float audibleGain = v.currentGain[idx];
    if (audibleGain < 0.0f) audibleGain = -audibleGain;
    float outputGain = v.mixGainL[idx];
    if (outputGain < 0.0f) outputGain = -outputGain;
    float rightGain = v.mixGainR[idx];
    if (rightGain < 0.0f) rightGain = -rightGain;
    if (rightGain > outputGain) outputGain = rightGain;
    audibleGain *= outputGain;
    if (audibleGain > 1.0f) audibleGain = 1.0f;

    // Higher score means less important and therefore stolen first.
    float score = (1.0f - audibleGain) * 10000.0f;
    score += static_cast<float>(127u - v.velocity[idx]) * 100.0f;
    score += cappedAge * 100.0f;

    if (v.state[idx] == static_cast<uint8_t>(VoiceState::Releasing))
        score += 20000.0f;
    if (v.velocity[idx] >= 96)
        score -= 5000.0f;

    const uint8_t stage = v.envelopeStage[idx];
    const bool attackPhase = (stage == 0 || stage == 1 || stage == 4);
    if (v.state[idx] == static_cast<uint8_t>(VoiceState::Active) &&
        (attackPhase || ageFrames < kNewbornProtectSamples)) {
        score -= 30000.0f;
    }
    return score;
}

inline void VoiceManager::InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity) {
    v.state[handle]        = static_cast<uint8_t>(VoiceState::Active);
    v.channel[handle]      = channel;
    v.note[handle]         = note;
    v.velocity[handle]     = velocity;
    v.phases[handle]       = 0.0f;
    v.phaseIncs[handle]    = 0.0f;
    v.currentGain[handle]  = 0.0f;
    v.targetGain[handle]   = 1.0f;
    v.sustainLevel[handle] = 0.7f;
    v.attackGainStep[handle] = 0.0f;
    v.decayGainStep[handle]  = 0.0f;
    v.releaseDecay[handle]   = kDefaultReleaseDecay;
    v.gainLeft[handle]     = 1.0f;
    v.gainRight[handle]    = 1.0f;
    v.sampleStart[handle]  = 0;
    v.sampleEnd[handle]    = 0;
    v.loopStart[handle]    = 0;
    v.loopEnd[handle]      = 0;
    v.loopMode[handle]     = 0;
    v.sampleBacked[handle] = 0;
    v.presetIndex[handle] = UINT16_MAX;
    v.regionIndex[handle] = UINT16_MAX;
    v.playIndex[handle] = UINT32_MAX;
    v.holdSamplesRemaining[handle]   = 0;
    v.attackSamplesRemaining[handle] = 0;
    v.decaySamplesRemaining[handle]  = 0;
    v.delaySamplesRemaining[handle]  = 0;
    v.releaseSamplesRemaining[handle] = UINT32_MAX;
    v.decaySlope[handle]        = 1.0f;
    v.samplePageId[handle]      = 0;
    v.envelopeStage[handle]     = 0;
    v.heldBySustain[handle]     = 0;
    v.releaseStartInBlock[handle] = 0;
    v.nextChannelKeyVoice[handle] = -1;
    v.mixGainL[handle]          = 0.0f;
    v.mixGainR[handle]          = 0.0f;
    v.relEnd[handle]            = 0;
    v.relLoopS[handle]          = 0;
    v.relLoopE[handle]          = 0;
    v.relLoopSF[handle]         = 0.0f;
    v.relLoopEF[handle]         = 0.0f;
    v.loopEnabled[handle]       = 0;
    v.birthFrame[handle]        = currentFrame_;
    v.stealTailFramesRemaining[handle] = 0;
    v.stealTailFramesTotal[handle] = 0;
    v.stealFadeInFramesRemaining[handle] = 0;
    v.stealFadeInFramesTotal[handle] = 0;
    v.stealTailChannel[handle] = UINT8_MAX;
}

inline void VoiceManager::LinkChannelKey(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    uint8_t ch = v.channel[handle];
    uint8_t nt = v.note[handle];
    v.nextChannelKeyVoice[handle] = channelKeyVoiceHead_[ch][nt];
    channelKeyVoiceHead_[ch][nt] = static_cast<int32_t>(handle);
}

inline void VoiceManager::UnlinkChannelKey(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    uint8_t ch = v.channel[handle];
    uint8_t nt = v.note[handle];
    int32_t prev = -1;
    int32_t cur = channelKeyVoiceHead_[ch][nt];
    while (cur >= 0) {
        if (cur == static_cast<int32_t>(handle)) {
            if (prev >= 0)
                v.nextChannelKeyVoice[prev] = v.nextChannelKeyVoice[cur];
            else
                channelKeyVoiceHead_[ch][nt] = v.nextChannelKeyVoice[cur];
            v.nextChannelKeyVoice[handle] = -1;
            return;
        }
        prev = cur;
        cur = v.nextChannelKeyVoice[cur];
    }
}

inline VoiceHandle VoiceManager::AllocateVoice(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (freeTop_ == 0) return kInvalidVoice;
    uint32_t idx = freeStack_[--freeTop_];

    InitializeVoice(static_cast<VoiceHandle>(idx), channel, note, velocity);
    LinkChannelKey(static_cast<VoiceHandle>(idx));

    activeList_[activeCount_] = idx;
    activePosition_[idx] = activeCount_;
    activeCount_++;

    return static_cast<VoiceHandle>(idx);
}

inline VoiceHandle VoiceManager::AllocateVoiceOrSteal(uint8_t channel, uint8_t note,
                                                        uint8_t velocity,
                                                        bool* outStolen) {
    VoiceHandle vh = AllocateVoice(channel, note, velocity);
    if (vh != kInvalidVoice) {
        if (outStolen) *outStolen = false;
        return vh;
    }

    // Pool is full — find the lowest-priority voice to steal.
    float bestScore = -1.0e30f;
    uint32_t bestIdx = 0;
    uint32_t bestPos = 0;
    bool foundVictim = false;

    for (uint32_t i = 0; i < activeCount_; ++i) {
        uint32_t idx = activeList_[i];
        float score = ComputeStealScore(idx);
        if (!foundVictim || score > bestScore) {
            bestScore = score;
            bestIdx  = idx;
            bestPos  = i;
            foundVictim = true;
        }
    }

    if (!foundVictim) return kInvalidVoice;

    // Capture exactly the sample state that would have rendered on this
    // frame.  InitializeVoice clears the primary slot, so keep the snapshot
    // in locals until the replacement has been installed.
    const float tailPhase = v.phases[bestIdx];
    const float tailPhaseInc = v.phaseIncs[bestIdx];
    const float tailGain = v.currentGain[bestIdx];
    const float tailMixGainL = v.mixGainL[bestIdx];
    const float tailMixGainR = v.mixGainR[bestIdx];
    const uint32_t tailSampleStart = v.sampleStart[bestIdx];
    const uint32_t tailRelEnd = v.relEnd[bestIdx];
    const uint32_t tailRelLoopS = v.relLoopS[bestIdx];
    const uint32_t tailRelLoopE = v.relLoopE[bestIdx];
    const float tailRelLoopSF = v.relLoopSF[bestIdx];
    const float tailRelLoopEF = v.relLoopEF[bestIdx];
    const uint8_t tailSampleBacked = v.sampleBacked[bestIdx];
    const uint8_t tailLoopEnabled = v.loopEnabled[bestIdx];
    const uint8_t tailChannel = v.channel[bestIdx];
    const bool tailAudible = tailSampleBacked != 0 && tailRelEnd > 1u &&
        tailGain > kVoiceRetireThreshold &&
        (tailMixGainL != 0.0f || tailMixGainR != 0.0f);

    // Retire the victim.
    ++stealCount_;
    UnlinkChannelKey(static_cast<VoiceHandle>(bestIdx));
    v.state[bestIdx] = static_cast<uint8_t>(VoiceState::Free);

    // Reinitialize in-place
    InitializeVoice(static_cast<VoiceHandle>(bestIdx), channel, note, velocity);
    if (tailAudible) {
        v.stealTailPhase[bestIdx] = tailPhase;
        v.stealTailPhaseInc[bestIdx] = tailPhaseInc;
        v.stealTailGain[bestIdx] = tailGain;
        v.stealTailMixGainL[bestIdx] = tailMixGainL;
        v.stealTailMixGainR[bestIdx] = tailMixGainR;
        v.stealTailSampleStart[bestIdx] = tailSampleStart;
        v.stealTailRelEnd[bestIdx] = tailRelEnd;
        v.stealTailRelLoopS[bestIdx] = tailRelLoopS;
        v.stealTailRelLoopE[bestIdx] = tailRelLoopE;
        v.stealTailRelLoopSF[bestIdx] = tailRelLoopSF;
        v.stealTailRelLoopEF[bestIdx] = tailRelLoopEF;
        v.stealTailSampleBacked[bestIdx] = tailSampleBacked;
        v.stealTailLoopEnabled[bestIdx] = tailLoopEnabled;
        v.stealTailChannel[bestIdx] = tailChannel;
        v.stealTailFramesRemaining[bestIdx] = stealFadeFrames_;
        v.stealTailFramesTotal[bestIdx] = stealFadeFrames_;
    }
    v.stealFadeInFramesRemaining[bestIdx] = stealFadeFrames_;
    v.stealFadeInFramesTotal[bestIdx] = stealFadeFrames_;
    LinkChannelKey(static_cast<VoiceHandle>(bestIdx));
    activeList_[bestPos] = bestIdx;  // reuse the victim's position
    activePosition_[bestIdx] = bestPos;

    if (outStolen) *outStolen = true;
    return static_cast<VoiceHandle>(bestIdx);
}

inline void VoiceManager::StartRelease(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Active)) {
        UnlinkChannelKey(handle);
        v.state[handle] = static_cast<uint8_t>(VoiceState::Releasing);
        // SF2 sampleModes 3 = loop during key depression: stop looping so the
        // sample plays out to its end through the release tail.
        if (v.loopMode[handle] == 3) v.loopEnabled[handle] = 0;
    }
}

inline void VoiceManager::RetireVoice(VoiceHandle handle) {
    if (handle >= maxVoices_ || v.state[handle] == static_cast<uint8_t>(VoiceState::Free))
        return;

    ++retireCount_;
    if (GetVoiceAge(handle) < 2) ++retireImmediateCount_;

    UnlinkChannelKey(handle);
    v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
    v.currentGain[handle] = 0.0f;
    freeStack_[freeTop_++] = static_cast<int32_t>(handle);
    assert(freeTop_ <= maxVoices_ && "freeStack_ overflow in RetireVoice");

    const uint32_t position = activePosition_[handle];
    assert(position < activeCount_ && activeList_[position] == handle);
    const uint32_t lastPosition = activeCount_ - 1;
    if (position != lastPosition) {
        const uint32_t moved = activeList_[lastPosition];
        activeList_[position] = moved;
        activePosition_[moved] = position;
    }
    --activeCount_;
    activePosition_[handle] = UINT32_MAX;
}

inline void VoiceManager::RebuildActivePositions() {
    for (uint32_t position = 0; position < activeCount_; ++position)
        activePosition_[activeList_[position]] = position;
}

inline bool VoiceManager::IsActive(VoiceHandle handle) const {
    return handle < maxVoices_ && v.state[handle] != static_cast<uint8_t>(VoiceState::Free);
}

inline void VoiceManager::SetVoiceSample(VoiceHandle handle, uint32_t start, uint32_t end,
                                          uint32_t loopStart, uint32_t loopEnd, uint8_t loopMode,
                                          float phaseStep, uint8_t sb) {
    if (handle >= maxVoices_) return;
    v.sampleStart[handle] = start;
    v.sampleEnd[handle]   = end;
    v.loopStart[handle]   = loopStart;
    v.loopEnd[handle]     = loopEnd;
    v.loopMode[handle]    = loopMode;
    v.phaseIncs[handle]   = phaseStep;
    v.sampleBacked[handle] = sb;

    // Precomputed render constants (region bounds relative to sampleStart).
    // The per-sample loop reads these directly instead of recomputing them
    // from the absolute SF2 offsets every sample.
    uint32_t relE = (end > start) ? (end - start) : 0u;
    uint32_t relS = (loopStart > start) ? (loopStart - start) : 0u;
    uint32_t relL = (loopEnd > start) ? (loopEnd - start) : 0u;
    v.relEnd[handle]    = relE;
    v.relLoopS[handle]  = relS;
    v.relLoopE[handle]  = relL;
    v.relLoopSF[handle] = static_cast<float>(relS);
    v.relLoopEF[handle] = static_cast<float>(relL);
    // Mirrors ShouldLoopSVMS() for a freshly-allocated (Active) voice.
    v.loopEnabled[handle] = ((loopMode == 1 || loopMode == 3) &&
                             loopEnd > loopStart + 1u) ? 1 : 0;

    // Start every voice at the exact beginning of its SoundFont region.
    // Render-correct engines retrigger chopped notes from the same sample
    // position; randomizing this phase smears the repeated waveform and
    // destroys the characteristic coherent buzz of dense retriggers.
    v.phases[handle] = 0.0f;
}

inline void VoiceManager::SetVoiceSoundFontIdentity(VoiceHandle handle,
                                                      uint16_t presetIndex,
                                                      uint16_t regionIndex) {
    if (handle >= maxVoices_) return;
    v.presetIndex[handle] = presetIndex;
    v.regionIndex[handle] = regionIndex;
}

inline void VoiceManager::SetVoicePlayIndex(VoiceHandle handle,
                                             uint32_t playIndex) {
    if (handle >= maxVoices_) return;
    v.playIndex[handle] = playIndex;
}

inline uint32_t VoiceManager::FindOldestPlayIndex(uint8_t channel,
                                                   uint8_t note) const {
    uint32_t oldest = UINT32_MAX;
    for (uint32_t ai = 0; ai < activeCount_; ++ai) {
        uint32_t i = activeList_[ai];
        // Match TSF: note-off searches only voices that are still active.
        // Releasing/held generations must not mask a newer retrigger.
        if (v.state[i] != static_cast<uint8_t>(VoiceState::Active) ||
            v.heldBySustain[i] || v.channel[i] != channel ||
            v.note[i] != note) continue;
        if (v.playIndex[i] < oldest) oldest = v.playIndex[i];
    }
    return oldest;
}

inline void VoiceManager::StartReleaseForPlayIndex(uint8_t channel,
                                                    uint8_t note,
                                                    uint32_t playIndex) {
    if (playIndex == UINT32_MAX) return;
    for (uint32_t ai = 0; ai < activeCount_; ++ai) {
        uint32_t i = activeList_[ai];
        if (v.state[i] == static_cast<uint8_t>(VoiceState::Free) ||
            v.channel[i] != channel || v.note[i] != note ||
            v.playIndex[i] != playIndex) continue;
        StartRelease(i);
    }
}

inline void VoiceManager::SetVoiceEnvelope(VoiceHandle handle, float initialGain,
                                            float sustainLevel, uint32_t delaySamples,
                                            uint32_t holdSamples, uint32_t attackSamples,
                                            uint32_t decaySamples, float attackGainStep,
                                            float decaySlope, float releaseDecay,
                                            uint32_t releaseSamples) {
    if (handle >= maxVoices_) return;
    v.targetGain[handle] = initialGain;
    v.sustainLevel[handle] = sustainLevel * initialGain;
    v.delaySamplesRemaining[handle]  = delaySamples;
    v.holdSamplesRemaining[handle]   = holdSamples;
    v.attackSamplesRemaining[handle] = attackSamples;
    v.decaySamplesRemaining[handle]  = decaySamples;
    v.attackGainStep[handle] = attackGainStep;
    v.decaySlope[handle]     = decaySlope;
    v.releaseDecay[handle]   = releaseDecay;
    // UINT32_MAX preserves threshold-based behavior for synthetic/offline
    // callers that provide only a multiplier.  Live SF2 voices always pass
    // the exact parsed segment length.
    v.releaseSamplesRemaining[handle] = releaseSamples > 0u
        ? releaseSamples : UINT32_MAX;
    v.currentGain[handle]    = 0.0f;

    if (delaySamples > 0)
        v.envelopeStage[handle] = 4;
    else if (holdSamples > 0)
        v.envelopeStage[handle] = 0;
    else if (attackSamples > 0)
        v.envelopeStage[handle] = 1;
    else if (decaySamples > 0) {
        // SF2 regions commonly have an instantaneous attack followed by a
        // non-zero decay.  Decay must begin at the note's initial level;
        // leaving currentGain at its allocation value of zero makes the
        // entire region silent (it immediately clamps to its sustain level).
        v.envelopeStage[handle] = 2;
        v.currentGain[handle] = initialGain;
    } else {
        v.envelopeStage[handle] = 3;
        v.currentGain[handle] = initialGain;
    }
}

inline void VoiceManager::SetVoiceGain(VoiceHandle handle, float left, float right) {
    if (handle >= maxVoices_) return;
    v.gainLeft[handle]  = left;
    v.gainRight[handle] = right;
}

inline void VoiceManager::RefreshMixGain(VoiceHandle handle, const ChannelParamsSnapshot& cp) {
    if (handle >= maxVoices_) return;
    v.mixGainL[handle] = v.gainLeft[handle]  * cp.panLeft  * cp.volume * cp.expression;
    v.mixGainR[handle] = v.gainRight[handle] * cp.panRight * cp.volume * cp.expression;
}

inline void VoiceManager::RefreshMixGains(const ChannelParamsSnapshot* chParams) {
    for (uint32_t ai = 0; ai < activeCount_; ++ai) {
        uint32_t i = activeList_[ai];
        const ChannelParamsSnapshot& cp = chParams[v.channel[i]];
        v.mixGainL[i] = v.gainLeft[i]  * cp.panLeft  * cp.volume * cp.expression;
        v.mixGainR[i] = v.gainRight[i] * cp.panRight * cp.volume * cp.expression;
    }
}

inline void VoiceManager::EndVoicesForChannelKey(uint8_t channel, uint8_t note,
                                                   uint32_t blockOffset) {
    // Fixed fast release (100ms) regardless of pool pressure.
    // BASSMIDI uses SF2-defined release — we use a consistent fast release
    // so panic/sound-off always sounds the same.
    float releaseDecay = MakeReleaseDecay(0.100f, sampleRate_);
    const uint32_t releaseSamples = MakeReleaseSamples(0.100f, sampleRate_);

    int32_t idx = channelKeyVoiceHead_[channel][note];
    while (idx >= 0) {
        int32_t next = v.nextChannelKeyVoice[idx];
        if (v.state[idx] != static_cast<uint8_t>(VoiceState::Free)) {
            if (v.state[idx] == static_cast<uint8_t>(VoiceState::Active)) {
                v.releaseDecay[idx] = releaseDecay;
                v.releaseSamplesRemaining[idx] = releaseSamples;
                v.releaseStartInBlock[idx] = blockOffset;
                StartRelease(static_cast<VoiceHandle>(idx));
            }
        }
        idx = next;
    }
}

inline void VoiceManager::SilenceChannelImmediate(uint8_t channel) {
    if (channel >= kChannelCount) return;
    for (uint32_t position = 0; position < activeCount_;) {
        const uint32_t idx = activeList_[position];
        if (v.stealTailFramesRemaining[idx] != 0 &&
            v.stealTailChannel[idx] == channel) {
            v.stealTailFramesRemaining[idx] = 0;
        }
        if (v.channel[idx] != channel) {
            ++position;
            continue;
        }

        // CC120 is the hard-stop controller.  It also cancels any transient
        // steal crossfade attached to the slot; no audio from this channel is
        // permitted after the controller's target frame.
        v.stealTailFramesRemaining[idx] = 0;
        v.stealFadeInFramesRemaining[idx] = 0;
        RetireVoice(static_cast<VoiceHandle>(idx));
        // RetireVoice swap-removes, so inspect the replacement at this same
        // active-list position before advancing.
    }
}

inline void VoiceManager::ReleaseChannel(uint8_t channel, uint32_t blockOffset) {
    if (channel >= kChannelCount) return;
    for (uint32_t position = 0; position < activeCount_; ++position) {
        const uint32_t idx = activeList_[position];
        if (v.channel[idx] != channel ||
            v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) {
            continue;
        }
        v.heldBySustain[idx] = 0;
        v.releaseStartInBlock[idx] = blockOffset;
        StartRelease(static_cast<VoiceHandle>(idx));
    }
}

} // namespace svms

#endif
