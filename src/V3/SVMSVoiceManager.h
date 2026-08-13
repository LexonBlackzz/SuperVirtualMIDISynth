#ifndef SVMS_VOICE_MANAGER_H
#define SVMS_VOICE_MANAGER_H

#include "SVMSTypes.h"
#include "SVMSEnvelope.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace svms {

// Fully prepared hot-path state for one SF2 layer.  Applying this as one
// transaction avoids repeatedly reclassifying the newborn and repairing the
// exact steal index while its fields are still only partially initialized.
struct VoiceConfiguration {
    uint32_t sampleStart = 0;
    uint32_t sampleEnd = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    uint32_t playIndex = UINT32_MAX;
    uint32_t delaySamples = 0;
    uint32_t holdSamples = 0;
    uint32_t attackSamples = 0;
    uint32_t decaySamples = 0;
    uint32_t releaseSamples = 0;
    float phaseStep = 1.0f;
    float basePhaseStep = 1.0f;
    float pitchBendScale = 1.0f;
    float initialGain = 0.0f;
    float sustainLevel = 1.0f;
    float attackGainStep = 0.0f;
    float decaySlope = 1.0f;
    float releaseDecay = kDefaultReleaseDecay;
    float gainLeft = 1.0f;
    float gainRight = 1.0f;
    uint16_t presetIndex = UINT16_MAX;
    uint16_t regionIndex = UINT16_MAX;
    uint8_t loopMode = 0;
    uint8_t sampleBacked = 1;
};

// ════════════════════════════════════════════════════════════════════════
// VoiceManager — flat-array voice pool with score-based stealing.
//
// Replaces the old doubly-linked steal lists and freeList with flat arrays:
//   activeList[]  — indices of currently active/releasing voices
//   freeStack[]   — LIFO stack of free slot indices for O(1) allocation
//
// Stealing follows BASSMIDI's pool-limit policy: effective control/envelope
// level protects audible voices while rendered age makes older voices easier
// to replace. MIDI velocity is not a separate priority. Audible victims enter
// a bounded reserve of short fade tails so replacement never hard-cuts them.
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
                                     bool* outStolen = nullptr,
                                     bool deferCandidate = false,
                                     bool reserveCandidateInPlace = true);
    // Complete a deferred note-on setup with one exact steal-index update.
    // This avoids repeatedly removing/reinserting the same newborn while its
    // sample, envelope and gains are filled in sequentially.
    void CommitVoiceConfiguration(VoiceHandle handle);
    void ConfigureVoice(VoiceHandle handle, const VoiceConfiguration& setup,
                        const ChannelParamsSnapshot& channelParams,
                        bool commitDeferred);
    bool LaunchVoiceGroup(uint8_t channel, uint8_t note, uint8_t velocity,
                          const VoiceConfiguration* setups, uint32_t count,
                          const ChannelParamsSnapshot& channelParams,
                          VoiceHandle* outHandles);

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
    void SetVoicePitchBase(VoiceHandle handle, float basePhaseStep,
                           float pitchBendScale);

    void SetVoiceSoundFontIdentity(VoiceHandle handle, uint16_t presetIndex,
                                   uint16_t regionIndex);
    void SetVoicePlayIndex(VoiceHandle handle, uint32_t playIndex);
    uint32_t FindOldestPlayIndex(uint8_t channel, uint8_t note) const;
    void StartReleaseForPlayIndex(uint8_t channel, uint8_t note,
                                  uint32_t playIndex);
    void NoteOffPlayIndex(uint8_t channel, uint8_t note, uint32_t playIndex,
                          bool sustain, uint32_t blockOffset);

    void SetVoiceEnvelope(VoiceHandle handle, float initialGain, float sustainLevel,
                          uint32_t delaySamples, uint32_t holdSamples, uint32_t attackSamples,
                          uint32_t decaySamples, float attackGainStep,
                          float decaySlope, float releaseDecay,
                          uint32_t releaseSamples = 0);

    void SetVoiceGain(VoiceHandle handle, float left, float right);

    // Premultiplied output gains (mixGainL/R = gainLeft/Right × pan × volume).
    // Called once per note-on and only for channels affected by gain-state
    // controllers, so render kernels never touch ChannelParamsSnapshot.
    void RefreshMixGain(VoiceHandle handle, const ChannelParamsSnapshot& cp);
    void RefreshMixGains(const ChannelParamsSnapshot* chParams);
    void RefreshMixGainsForChannel(uint8_t channel,
                                   const ChannelParamsSnapshot& cp);

    // ── Channel-key utilities ──────────────────────────────────────────

    // Release all voices on a (channel, note) with a fixed fast release.
    // Used for panic / all-notes-off scenarios.
    void EndVoicesForChannelKey(uint8_t channel, uint8_t note, uint32_t blockOffset);
    void SilenceChannelImmediate(uint8_t channel);
    void ReleaseChannel(uint8_t channel, uint32_t blockOffset);

    uint32_t GetActiveCount() const { return activeCount_; }
    uint32_t GetMaxVoices() const { return maxVoices_; }
    void SetCurrentFrame(uint64_t frame);
    uint32_t GetVoiceAge(VoiceHandle handle) const;
    uint32_t GetChannelActiveCount(uint8_t channel) const;
    const uint32_t* GetChannelActiveList(uint8_t channel) const;
    void InvalidateStealCandidates();
    void RefreshRenderClass(VoiceHandle handle);
    uint32_t GetRenderClassCount(VoiceRenderClass renderClass) const;
    const uint32_t* GetRenderClassList(VoiceRenderClass renderClass) const;
    uint32_t GetNonemptyRenderClassMask() const { return renderClassMask_; }
    uint32_t GetStealTailCount() const { return stealTailCount_; }
    const uint32_t* GetStealTailList() const { return stealTailList_; }
    void RefreshStealTail(VoiceHandle handle);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    VoiceHandle FindStealVictimExhaustiveForTest() const;
    uint64_t GetStealHeapBuildCountForTest() const {
        return stealHeapBuildCount_;
    }
    uint64_t GetGroupReuseAttemptCountForTest() const {
        return groupReuseAttemptCount_;
    }
    uint64_t GetGroupReuseMatchCountForTest() const {
        return groupReuseMatchCount_;
    }
    uint64_t GetGroupReuseReservedCountForTest() const {
        return groupReuseReservedCount_;
    }
    uint64_t GetGroupReuseSmallerCountForTest() const {
        return groupReuseSmallerCount_;
    }
    uint64_t GetGroupReuseLargerCountForTest() const {
        return groupReuseLargerCount_;
    }
    uint32_t GetPlayGroupSizeForTest(VoiceHandle handle) const {
        if (handle >= maxVoices_ ||
            v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return 0u;
        uint32_t count = 1u;
        int32_t linked = playGroupPrev_[handle];
        while (linked >= 0) {
            ++count;
            linked = playGroupPrev_[static_cast<uint32_t>(linked)];
        }
        linked = playGroupNext_[handle];
        while (linked >= 0) {
            ++count;
            linked = playGroupNext_[static_cast<uint32_t>(linked)];
        }
        return count;
    }
    void ResetGroupReuseCountersForTest() {
        groupReuseAttemptCount_ = 0u;
        groupReuseMatchCount_ = 0u;
        groupReuseReservedCount_ = 0u;
        groupReuseSmallerCount_ = 0u;
        groupReuseLargerCount_ = 0u;
    }
#endif

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
    struct StealCandidate {
        float score;
        uint32_t handle;
        uint32_t activePosition;
    };

    uint32_t maxVoices_;
    uint32_t sampleRate_;
    uint32_t stealFadeFrames_;
    uint64_t currentFrame_;

    // LIFO free slot stack
    int32_t freeStack_[kMaxPolyphony];

    // Dense per-channel indices make controller, sustain, pitch-bend and
    // channel termination work proportional to that channel's polyphony.
    uint32_t channelActiveCount_[kChannelCount];
    uint32_t channelActiveList_[kChannelCount][kMaxPolyphony];
    uint32_t channelActivePosition_[kMaxPolyphony];

    alignas(64) uint32_t renderClassCount_[kVoiceRenderClassCount];
    uint32_t renderClassMask_;
    alignas(64) uint32_t
        renderClassList_[kVoiceRenderClassCount][kMaxPolyphony];
    alignas(64) uint32_t renderClassPosition_[kMaxPolyphony];

    // Steal tails are rendered independently from primary render classes.
    // Keeping a dense list avoids probing all active voices in every short
    // event span when the overwhelmingly common tail count is zero.
    alignas(64) uint32_t stealTailList_[kMaxPolyphony];
    alignas(64) uint32_t stealTailPosition_[kMaxPolyphony];
    uint32_t stealTailCount_;
    // Tail levels are unchanged between render boundaries. Build the exact
    // quietest-tail heap once per frame, then update only its root when a
    // denser same-frame burst replaces that tail.
    uint32_t stealTailMinHeap_[kStealTailReserve];
    float stealTailMinHeapLevel_[kStealTailReserve];
    uint32_t stealTailMinHeapCount_;
    uint64_t stealTailMinHeapFrame_;
    bool stealTailMinHeapValid_;

    // Fixed-leaf tournament tree for candidates whose relative steal score is
    // time-invariant. This includes delay/hold/attack because stealing protects
    // them using their fixed target gain. Decay/release remain volatile.
    static constexpr uint32_t kStealTreeLeafBase = kMaxPolyphony;
    StealCandidate stealStableCandidate_[kMaxPolyphony];
    uint32_t stealWinnerTree_[kStealTreeLeafBase * 2u];
    uint32_t stealHeapCount_;
    bool stealHeapValid_;
    uint64_t stealHeapBuildCount_;
    uint32_t stealVolatileList_[kMaxPolyphony];
    uint32_t stealVolatilePosition_[kMaxPolyphony];
    uint32_t stealVolatileCount_;
    alignas(64) StealCandidate stealVolatileHeap_[kMaxPolyphony];
    uint32_t stealVolatileHeapPosition_[kMaxPolyphony];
    uint32_t stealVolatileHeapCount_;
    uint64_t stealVolatileHeapFrame_;
    bool stealVolatileHeapValid_;
    uint8_t stealCandidateDeferred_[kMaxPolyphony];
    // A deferred same-frame replacement may keep ownership of the volatile
    // heap root while its sample/envelope fields are configured.  The slot
    // and active position do not change, so CommitVoiceConfiguration can
    // update the key in place instead of remove + insert heap traversals.
    uint8_t stealCandidateReserved_[kMaxPolyphony];
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    uint64_t groupReuseAttemptCount_ = 0u;
    uint64_t groupReuseMatchCount_ = 0u;
    uint64_t groupReuseReservedCount_ = 0u;
    uint64_t groupReuseSmallerCount_ = 0u;
    uint64_t groupReuseLargerCount_ = 0u;
#endif

    // Per-key tracking for EndVoicesForChannelKey
    int32_t channelKeyVoiceHead_[kChannelCount][kNoteCount];
    // Tail of the newest-to-oldest intrusive chain. Note-off can therefore
    // identify the oldest outstanding generation in O(1), then touch only
    // that generation's adjacent SF2 layers.
    int32_t channelKeyVoiceOldest_[kChannelCount][kNoteCount];
    // Physical SF2 regions created by one MIDI note-on form one atomic steal
    // group. Dedicated links remain valid after note-off unlinks channel/key
    // tracking, so a releasing stereo pair cannot be split either.
    int32_t playGroupNext_[kMaxPolyphony];
    int32_t playGroupPrev_[kMaxPolyphony];
    uint32_t lastLinkedPlayIndex_;
    VoiceHandle lastLinkedPlayVoice_;

    void InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity);
    void InitializePreparedVoice(VoiceHandle handle, uint8_t channel,
                                 uint8_t note, uint8_t velocity);
    void LinkChannelKey(VoiceHandle handle);
    void UnlinkChannelKey(VoiceHandle handle);
    void UnlinkPlayGroup(VoiceHandle handle);
    void LinkChannelActive(VoiceHandle handle);
    void UnlinkChannelActive(VoiceHandle handle);
    VoiceRenderClass ClassifyVoice(VoiceHandle handle) const;
    void LinkRenderClass(VoiceHandle handle);
    void UnlinkRenderClass(VoiceHandle handle);
    void LinkStealTail(VoiceHandle handle);
    void UnlinkStealTail(VoiceHandle handle);
    void BuildStealTailMinHeap();
    void StealTailHeapSiftDown(uint32_t position);
    void BuildStealHeap();
    VoiceHandle SelectStableWinner(VoiceHandle left,
                                   VoiceHandle right) const;
    void RefreshStealWinnerPath(VoiceHandle handle);
    void RefreshStealWinnerPaths(const VoiceHandle* handles, uint32_t count);
    void RebuildStableWinnerTree();
    VoiceHandle PopStealCandidate(uint32_t& activePosition,
                                  bool reserveVolatileRoot);
    void PushStealCandidate(VoiceHandle handle, uint32_t activePosition);
    void UpdateStealCandidate(VoiceHandle handle);
    void RemoveStealCandidate(VoiceHandle handle);
    void LinkVolatileCandidate(VoiceHandle handle);
    void UnlinkVolatileCandidate(VoiceHandle handle);
    void BuildVolatileStealHeap();
    void VolatileHeapSwap(uint32_t a, uint32_t b);
    void VolatileHeapSiftUp(uint32_t position);
    void VolatileHeapSiftDown(uint32_t position);
    void RemoveVolatileHeapCandidate(VoiceHandle handle);
    bool IsStableStealCandidate(VoiceHandle handle) const;
    float ComputeEffectiveStealLevel(VoiceHandle handle) const;
    float ComputeTailLevel(uint32_t tailSlot) const;
    uint32_t SelectStealTailSlot(float outgoingLevel);
    void CaptureStealTail(VoiceHandle handle);
    bool ReuseMatchingStealGroup(uint8_t channel, uint8_t note,
                                 uint8_t velocity,
                                 const VoiceConfiguration* setups,
                                 uint32_t count, VoiceHandle* outHandles,
                                 bool& candidatesReservedInPlace);
    void CommitVoiceGroupConfigurations(const VoiceHandle* handles,
                                        uint32_t count,
                                        bool candidatesReservedInPlace);
    void RetireStolenSibling(VoiceHandle handle,
                             VoiceHandle selectedVictim);
    float ComputeStableStealKey(VoiceHandle handle) const;
    static bool HigherPriorityCandidate(const StealCandidate& a,
                                        const StealCandidate& b);

    // ── Score-based steal priority ─────────────────────────────────────
    // Computes BASSMIDI-like priority. HIGHER score = stolen FIRST.
    // Effective control/envelope level protects audible voices; rendered age
    // is the only independent bias. Velocity is not a separate priority.
    float ComputeStealScore(uint32_t idx) const;
};

// ════════════════════════════════════════════════════════════════════════
// Implementation
// ════════════════════════════════════════════════════════════════════════

inline VoiceManager::VoiceManager()
    : maxVoices_(0), activeCount_(0), sampleRate_(44100),
      stealFadeFrames_(kStealFadeFrames),
      currentFrame_(0), freeTop_(0),
      retireCount_(0), retireImmediateCount_(0), stealCount_(0),
      stealTailCount_(0), stealTailMinHeapCount_(0),
      stealTailMinHeapFrame_(UINT64_MAX), stealTailMinHeapValid_(false),
      stealHeapCount_(0), stealHeapValid_(false),
      stealHeapBuildCount_(0),
      stealVolatileCount_(0), stealVolatileHeapCount_(0),
      stealVolatileHeapFrame_(UINT64_MAX), stealVolatileHeapValid_(false) {
    std::memset(&v, 0, sizeof(v));
    std::memset(activeList_, 0, sizeof(activeList_));
    std::memset(activePosition_, 0xff, sizeof(activePosition_));
    std::memset(freeStack_, 0, sizeof(freeStack_));
    std::memset(channelActiveCount_, 0, sizeof(channelActiveCount_));
    std::memset(channelActivePosition_, 0xff, sizeof(channelActivePosition_));
    std::memset(renderClassCount_, 0, sizeof(renderClassCount_));
    renderClassMask_ = 0u;
    std::memset(renderClassPosition_, 0xff, sizeof(renderClassPosition_));
    std::memset(stealTailPosition_, 0xff, sizeof(stealTailPosition_));
    std::memset(stealWinnerTree_, 0xff, sizeof(stealWinnerTree_));
    std::memset(stealVolatilePosition_, 0xff, sizeof(stealVolatilePosition_));
    std::memset(stealVolatileHeapPosition_, 0xff,
                sizeof(stealVolatileHeapPosition_));
    std::memset(stealCandidateDeferred_, 0, sizeof(stealCandidateDeferred_));
    std::memset(stealCandidateReserved_, 0, sizeof(stealCandidateReserved_));
    std::memset(playGroupNext_, 0xff, sizeof(playGroupNext_));
    std::memset(playGroupPrev_, 0xff, sizeof(playGroupPrev_));
    lastLinkedPlayIndex_ = UINT32_MAX;
    lastLinkedPlayVoice_ = kInvalidVoice;
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n)
            channelKeyVoiceHead_[ch][n] = channelKeyVoiceOldest_[ch][n] = -1;
}

inline void VoiceManager::Initialize(uint32_t maxVoices, uint32_t sampleRate) {
    maxVoices_ = maxVoices < kMaxPolyphony ? maxVoices : kMaxPolyphony;
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100;
    stealFadeFrames_ = kStealFadeFrames;
    Reset();
}

inline void VoiceManager::Reset() {
    std::memset(&v, 0, sizeof(v));
    std::memset(activeList_, 0, sizeof(activeList_));
    std::memset(activePosition_, 0xff, sizeof(activePosition_));
    std::memset(channelActiveCount_, 0, sizeof(channelActiveCount_));
    std::memset(channelActivePosition_, 0xff, sizeof(channelActivePosition_));
    std::memset(renderClassCount_, 0, sizeof(renderClassCount_));
    renderClassMask_ = 0u;
    std::memset(renderClassPosition_, 0xff, sizeof(renderClassPosition_));
    std::memset(stealTailPosition_, 0xff, sizeof(stealTailPosition_));
    std::memset(stealWinnerTree_, 0xff, sizeof(stealWinnerTree_));
    std::memset(stealVolatilePosition_, 0xff, sizeof(stealVolatilePosition_));
    std::memset(stealVolatileHeapPosition_, 0xff,
                sizeof(stealVolatileHeapPosition_));
    std::memset(stealCandidateDeferred_, 0, sizeof(stealCandidateDeferred_));
    std::memset(stealCandidateReserved_, 0, sizeof(stealCandidateReserved_));
    std::memset(playGroupNext_, 0xff, sizeof(playGroupNext_));
    std::memset(playGroupPrev_, 0xff, sizeof(playGroupPrev_));
    activeCount_ = 0;
    currentFrame_ = 0;
    retireCount_ = 0;
    retireImmediateCount_ = 0;
    stealCount_ = 0;
    stealTailCount_ = 0;
    stealTailMinHeapCount_ = 0;
    stealTailMinHeapFrame_ = UINT64_MAX;
    stealTailMinHeapValid_ = false;
    stealHeapCount_ = 0;
    stealHeapValid_ = false;
    stealHeapBuildCount_ = 0;
    stealVolatileCount_ = 0;
    stealVolatileHeapCount_ = 0;
    stealVolatileHeapFrame_ = UINT64_MAX;
    stealVolatileHeapValid_ = false;
    lastLinkedPlayIndex_ = UINT32_MAX;
    lastLinkedPlayVoice_ = kInvalidVoice;
    freeTop_ = maxVoices_;
    for (uint32_t i = 0; i < maxVoices_; ++i) {
        v.state[i] = static_cast<uint8_t>(VoiceState::Free);
        v.nextChannelKeyVoice[i] = -1;
        v.prevChannelKeyVoice[i] = -1;
        freeStack_[i] = static_cast<int32_t>(i);
    }
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n)
            channelKeyVoiceHead_[ch][n] = channelKeyVoiceOldest_[ch][n] = -1;
}

inline void VoiceManager::SetCurrentFrame(uint64_t frame) {
    if (frame != currentFrame_) stealTailMinHeapValid_ = false;
    currentFrame_ = frame;
}

inline uint32_t VoiceManager::GetChannelActiveCount(uint8_t channel) const {
    return channel < kChannelCount ? channelActiveCount_[channel] : 0u;
}

inline const uint32_t* VoiceManager::GetChannelActiveList(uint8_t channel) const {
    return channel < kChannelCount ? channelActiveList_[channel] : nullptr;
}

inline void VoiceManager::InvalidateStealCandidates() {
    stealHeapValid_ = false;
}

inline uint32_t VoiceManager::GetRenderClassCount(
    VoiceRenderClass renderClass) const {
    const uint32_t index = static_cast<uint32_t>(renderClass);
    return index < kVoiceRenderClassCount ? renderClassCount_[index] : 0u;
}

inline const uint32_t* VoiceManager::GetRenderClassList(
    VoiceRenderClass renderClass) const {
    const uint32_t index = static_cast<uint32_t>(renderClass);
    return index < kVoiceRenderClassCount ? renderClassList_[index] : nullptr;
}

inline uint32_t VoiceManager::GetVoiceAge(VoiceHandle handle) const {
    if (handle >= maxVoices_ || currentFrame_ <= v.birthFrame[handle]) return 0;
    const uint64_t age = currentFrame_ - v.birthFrame[handle];
    return age > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age);
}

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
inline VoiceHandle VoiceManager::FindStealVictimExhaustiveForTest() const {
    StealCandidate best{};
    bool found = false;
    for (uint32_t position = 0; position < activeCount_; ++position) {
        const uint32_t handle = activeList_[position];
        const StealCandidate candidate{
            ComputeStealScore(handle), handle, position};
        if (!found || HigherPriorityCandidate(candidate, best)) {
            best = candidate;
            found = true;
        }
    }
    return found ? static_cast<VoiceHandle>(best.handle) : kInvalidVoice;
}
#endif

inline float VoiceManager::ComputeEffectiveStealLevel(
    VoiceHandle handle) const {
    const float left = v.mixGainL[handle];
    const float right = v.mixGainR[handle];
    // BASS ranks a control-derived level before waveform sampling. The
    // stereo energy norm removes constant-power pan from that estimate.
    const float outputGain = std::sqrt(left * left + right * right);
    const uint8_t stage = v.envelopeStage[handle];
    const bool preDecay =
        v.state[handle] == static_cast<uint8_t>(VoiceState::Active) &&
        (stage == 4u || stage == 0u || stage == 1u);
    const float envelopeGain = preDecay
        ? v.targetGain[handle] : v.currentGain[handle];
    return std::fabs(envelopeGain) * outputGain;
}

inline float VoiceManager::ComputeStealScore(uint32_t idx) const {
    // Reversed form of BASSMIDI's minimum-priority scan:
    //   int(effectiveLevel * 42000) - (ageSamples >> 8)
    // A continuous /256 age term keeps the persistent heap invariant between
    // voice-state changes while preserving the same ordering and scale.
    const float ageUnits = static_cast<float>(GetVoiceAge(
        static_cast<VoiceHandle>(idx))) * (1.0f / 256.0f);
    return ageUnits - ComputeEffectiveStealLevel(
        static_cast<VoiceHandle>(idx)) * kBassMidiStealGainScale;
}

inline void VoiceManager::InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity) {
    v.state[handle]        = static_cast<uint8_t>(VoiceState::Active);
    v.channel[handle]      = channel;
    v.note[handle]         = note;
    v.velocity[handle]     = velocity;
    v.phases[handle]       = 0.0f;
    v.phaseIncs[handle]    = 0.0f;
    v.basePhaseIncs[handle] = 0.0f;
    v.pitchBendScales[handle] = 1.0f;
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
    v.renderClass[handle] = static_cast<uint8_t>(VoiceRenderClass::Generic);
    v.heldBySustain[handle]     = 0;
    v.releaseStartInBlock[handle] = 0;
    v.nextChannelKeyVoice[handle] = -1;
    v.prevChannelKeyVoice[handle] = -1;
    playGroupNext_[handle] = -1;
    playGroupPrev_[handle] = -1;
    v.mixGainL[handle]          = 0.0f;
    v.mixGainR[handle]          = 0.0f;
    v.renderGainL[handle]       = 0.0f;
    v.renderGainR[handle]       = 0.0f;
    v.relEnd[handle]            = 0;
    v.relLoopS[handle]          = 0;
    v.relLoopE[handle]          = 0;
    v.relLoopSF[handle]         = 0.0f;
    v.relLoopEF[handle]         = 0.0f;
    v.loopEnabled[handle]       = 0;
    v.birthFrame[handle]        = currentFrame_;
    v.stealFadeInFramesRemaining[handle] = 0;
    v.stealFadeInFramesTotal[handle] = 0;
    stealCandidateDeferred_[handle] = 0u;
}

inline void VoiceManager::InitializePreparedVoice(
    VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity) {
    // ConfigureVoice immediately supplies every sample/envelope/gain field.
    // Initialize only state observed by lifecycle indices before that
    // transaction commits, avoiding dozens of default stores per SF2 note.
    v.state[handle] = static_cast<uint8_t>(VoiceState::Active);
    v.channel[handle] = channel;
    v.note[handle] = note;
    v.velocity[handle] = velocity;
    v.sampleBacked[handle] = 0u;
    v.relEnd[handle] = 0u;
    v.envelopeStage[handle] = 0u;
    v.renderClass[handle] = static_cast<uint8_t>(VoiceRenderClass::Generic);
    v.heldBySustain[handle] = 0u;
    v.releaseStartInBlock[handle] = 0u;
    v.nextChannelKeyVoice[handle] = -1;
    v.prevChannelKeyVoice[handle] = -1;
    v.playIndex[handle] = UINT32_MAX;
    playGroupNext_[handle] = -1;
    playGroupPrev_[handle] = -1;
    v.birthFrame[handle] = currentFrame_;
    v.stealFadeInFramesRemaining[handle] = 0u;
    v.stealFadeInFramesTotal[handle] = 0u;
    stealCandidateDeferred_[handle] = 0u;
}

inline void VoiceManager::LinkChannelKey(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    uint8_t ch = v.channel[handle];
    uint8_t nt = v.note[handle];
    const int32_t previousHead = channelKeyVoiceHead_[ch][nt];
    v.prevChannelKeyVoice[handle] = -1;
    v.nextChannelKeyVoice[handle] = previousHead;
    if (previousHead >= 0)
        v.prevChannelKeyVoice[static_cast<uint32_t>(previousHead)] =
            static_cast<int32_t>(handle);
    else
        channelKeyVoiceOldest_[ch][nt] = static_cast<int32_t>(handle);
    channelKeyVoiceHead_[ch][nt] = static_cast<int32_t>(handle);
}

inline void VoiceManager::UnlinkChannelKey(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t ch = v.channel[handle];
    const uint8_t nt = v.note[handle];
    const int32_t previous = v.prevChannelKeyVoice[handle];
    const int32_t next = v.nextChannelKeyVoice[handle];
    if (previous >= 0)
        v.nextChannelKeyVoice[static_cast<uint32_t>(previous)] = next;
    else if (channelKeyVoiceHead_[ch][nt] == static_cast<int32_t>(handle))
        channelKeyVoiceHead_[ch][nt] = next;
    else
        return;
    if (next >= 0)
        v.prevChannelKeyVoice[static_cast<uint32_t>(next)] = previous;
    else if (channelKeyVoiceOldest_[ch][nt] == static_cast<int32_t>(handle))
        channelKeyVoiceOldest_[ch][nt] = previous;
    v.nextChannelKeyVoice[handle] = -1;
    v.prevChannelKeyVoice[handle] = -1;
}

inline void VoiceManager::UnlinkPlayGroup(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const int32_t previous = playGroupPrev_[handle];
    const int32_t next = playGroupNext_[handle];
    if (previous >= 0)
        playGroupNext_[static_cast<uint32_t>(previous)] = next;
    if (next >= 0)
        playGroupPrev_[static_cast<uint32_t>(next)] = previous;
    if (lastLinkedPlayVoice_ == handle) {
        if (previous >= 0) {
            lastLinkedPlayVoice_ = static_cast<VoiceHandle>(previous);
        } else {
            lastLinkedPlayVoice_ = kInvalidVoice;
            lastLinkedPlayIndex_ = UINT32_MAX;
        }
    }
    playGroupPrev_[handle] = -1;
    playGroupNext_[handle] = -1;
}

inline void VoiceManager::LinkChannelActive(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t channel = v.channel[handle];
    const uint32_t position = channelActiveCount_[channel]++;
    channelActiveList_[channel][position] = handle;
    channelActivePosition_[handle] = position;
}

inline void VoiceManager::UnlinkChannelActive(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t channel = v.channel[handle];
    const uint32_t position = channelActivePosition_[handle];
    const uint32_t count = channelActiveCount_[channel];
    if (position >= count) return;
    const uint32_t lastPosition = count - 1u;
    if (position != lastPosition) {
        const uint32_t moved = channelActiveList_[channel][lastPosition];
        channelActiveList_[channel][position] = moved;
        channelActivePosition_[moved] = position;
    }
    channelActiveCount_[channel] = lastPosition;
    channelActivePosition_[handle] = UINT32_MAX;
}

inline VoiceRenderClass VoiceManager::ClassifyVoice(VoiceHandle handle) const {
    if (handle >= maxVoices_ ||
        v.state[handle] == static_cast<uint8_t>(VoiceState::Free) ||
        v.sampleBacked[handle] == 0u || v.relEnd[handle] < 2u) {
        return VoiceRenderClass::Generic;
    }
    if (v.stealFadeInFramesRemaining[handle] != 0u)
        return VoiceRenderClass::Generic;
    const bool loop = v.loopEnabled[handle] != 0u;
    if (loop && (v.relLoopS[handle] >= v.relLoopE[handle] ||
                 v.relLoopE[handle] > v.relEnd[handle])) {
        return VoiceRenderClass::Generic;
    }
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
        return loop ? VoiceRenderClass::ReleaseLoop
                    : VoiceRenderClass::ReleaseOneShot;
    if (v.envelopeStage[handle] == 3u)
        return loop ? VoiceRenderClass::SustainedLoop
                    : VoiceRenderClass::SustainedOneShot;
    if (loop && (v.envelopeStage[handle] == 1u ||
                 v.envelopeStage[handle] == 2u)) {
        return VoiceRenderClass::TransientLoop;
    }
    return VoiceRenderClass::Generic;
}

inline void VoiceManager::LinkRenderClass(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const VoiceRenderClass renderClass = ClassifyVoice(handle);
    const uint32_t classIndex = static_cast<uint32_t>(renderClass);
    const uint32_t position = renderClassCount_[classIndex]++;
    renderClassList_[classIndex][position] = handle;
    renderClassMask_ |= 1u << classIndex;
    renderClassPosition_[handle] = position;
    v.renderClass[handle] = static_cast<uint8_t>(renderClass);
    if (renderClass == VoiceRenderClass::SustainedLoop ||
        renderClass == VoiceRenderClass::SustainedOneShot) {
        v.renderGainL[handle] = v.currentGain[handle] * v.mixGainL[handle];
        v.renderGainR[handle] = v.currentGain[handle] * v.mixGainR[handle];
    }
}

inline void VoiceManager::UnlinkRenderClass(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint32_t classIndex = v.renderClass[handle];
    if (classIndex >= kVoiceRenderClassCount) return;
    const uint32_t position = renderClassPosition_[handle];
    const uint32_t count = renderClassCount_[classIndex];
    if (position >= count) return;
    const uint32_t lastPosition = count - 1u;
    if (position != lastPosition) {
        const uint32_t moved = renderClassList_[classIndex][lastPosition];
        renderClassList_[classIndex][position] = moved;
        renderClassPosition_[moved] = position;
    }
    renderClassCount_[classIndex] = lastPosition;
    if (lastPosition == 0u) renderClassMask_ &= ~(1u << classIndex);
    renderClassPosition_[handle] = UINT32_MAX;
}

inline void VoiceManager::RefreshRenderClass(VoiceHandle handle) {
    if (handle >= maxVoices_ ||
        v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return;
    const VoiceRenderClass desired = ClassifyVoice(handle);
    if (v.renderClass[handle] == static_cast<uint8_t>(desired) &&
        renderClassPosition_[handle] <
            renderClassCount_[static_cast<uint32_t>(desired)]) {
        // Attack and decay intentionally share the transient render class,
        // but only attack has a time-invariant protected steal level. Move
        // the candidate between the persistent tree and volatile heap even
        // when no render-list migration is required.
        if (stealHeapValid_ && stealCandidateDeferred_[handle] == 0u) {
            const bool indexedStable =
                stealWinnerTree_[kStealTreeLeafBase + handle] == handle;
            if (indexedStable != IsStableStealCandidate(handle))
                UpdateStealCandidate(handle);
        }
        return;
    }
    UnlinkRenderClass(handle);
    LinkRenderClass(handle);
    // Envelope transitions can move a voice from the exact volatile set to
    // the persistent sustained heap without any MIDI event touching it.
    UpdateStealCandidate(handle);
}

inline void VoiceManager::LinkStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_ || v.stealTailFramesRemaining[handle] == 0u ||
        stealTailPosition_[handle] < stealTailCount_) return;
    const uint32_t position = stealTailCount_++;
    stealTailList_[position] = handle;
    stealTailPosition_[handle] = position;
    stealTailMinHeapValid_ = false;
}

inline void VoiceManager::UnlinkStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint32_t position = stealTailPosition_[handle];
    if (position >= stealTailCount_) return;
    const uint32_t lastPosition = --stealTailCount_;
    if (position != lastPosition) {
        const uint32_t moved = stealTailList_[lastPosition];
        stealTailList_[position] = moved;
        stealTailPosition_[moved] = position;
    }
    stealTailPosition_[handle] = UINT32_MAX;
    stealTailMinHeapValid_ = false;
}

inline void VoiceManager::RefreshStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    if (v.stealTailFramesRemaining[handle] != 0u)
        LinkStealTail(handle);
    else
        UnlinkStealTail(handle);
}

inline float VoiceManager::ComputeTailLevel(uint32_t tailSlot) const {
    if (tailSlot >= maxVoices_ ||
        v.stealTailFramesRemaining[tailSlot] == 0u) return 0.0f;
    const uint32_t total = v.stealTailFramesTotal[tailSlot];
    const float fade = total > 1u
        ? static_cast<float>(v.stealTailFramesRemaining[tailSlot] - 1u) /
          static_cast<float>(total - 1u)
        : 0.0f;
    const float gain = std::fabs(v.stealTailGain[tailSlot]) * fade;
    return gain * (std::fabs(v.stealTailMixGainL[tailSlot]) +
                   std::fabs(v.stealTailMixGainR[tailSlot]));
}

inline void VoiceManager::StealTailHeapSiftDown(uint32_t position) {
    for (;;) {
        const uint32_t left = position * 2u + 1u;
        if (left >= stealTailMinHeapCount_) break;
        const uint32_t right = left + 1u;
        uint32_t quietest = left;
        auto lessQuiet = [this](uint32_t a, uint32_t b) {
            const float levelA = stealTailMinHeapLevel_[a];
            const float levelB = stealTailMinHeapLevel_[b];
            if (levelA != levelB) return levelA < levelB;
            return stealTailPosition_[stealTailMinHeap_[a]] <
                   stealTailPosition_[stealTailMinHeap_[b]];
        };
        if (right < stealTailMinHeapCount_ &&
            lessQuiet(right, left))
            quietest = right;
        if (!lessQuiet(quietest, position)) break;
        const uint32_t temporary = stealTailMinHeap_[position];
        stealTailMinHeap_[position] = stealTailMinHeap_[quietest];
        stealTailMinHeap_[quietest] = temporary;
        const float temporaryLevel = stealTailMinHeapLevel_[position];
        stealTailMinHeapLevel_[position] = stealTailMinHeapLevel_[quietest];
        stealTailMinHeapLevel_[quietest] = temporaryLevel;
        position = quietest;
    }
}

inline void VoiceManager::BuildStealTailMinHeap() {
    stealTailMinHeapCount_ = (std::min)(stealTailCount_, kStealTailReserve);
    for (uint32_t i = 0; i < stealTailMinHeapCount_; ++i) {
        stealTailMinHeap_[i] = stealTailList_[i];
        stealTailMinHeapLevel_[i] = ComputeTailLevel(stealTailList_[i]);
    }
    if (stealTailMinHeapCount_ > 1u) {
        for (uint32_t position = stealTailMinHeapCount_ / 2u;
             position-- > 0u;) StealTailHeapSiftDown(position);
    }
    stealTailMinHeapFrame_ = currentFrame_;
    stealTailMinHeapValid_ = true;
}

inline uint32_t VoiceManager::SelectStealTailSlot(float outgoingLevel) {
    const uint32_t reserveLimit = (std::min)(maxVoices_, kStealTailReserve);
    if (reserveLimit == 0u || outgoingLevel <= 0.0f) return UINT32_MAX;
    if (stealTailCount_ < reserveLimit) {
        for (uint32_t slot = 0; slot < maxVoices_; ++slot) {
            if (v.stealTailFramesRemaining[slot] == 0u) return slot;
        }
        return UINT32_MAX;
    }

    if (!stealTailMinHeapValid_ || stealTailMinHeapFrame_ != currentFrame_)
        BuildStealTailMinHeap();
    const uint32_t quietest = stealTailMinHeap_[0];
    const float quietestLevel = stealTailMinHeapLevel_[0];
    return outgoingLevel > quietestLevel ? quietest : UINT32_MAX;
}

inline void VoiceManager::CaptureStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const float gain = v.currentGain[handle];
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const float outgoingLevel = std::fabs(gain) *
        (std::fabs(mixL) + std::fabs(mixR));
    if (v.sampleBacked[handle] == 0u || v.relEnd[handle] <= 1u ||
        outgoingLevel <= kVoiceRetireThreshold) return;
    const uint32_t tailSlot = SelectStealTailSlot(outgoingLevel);
    if (tailSlot == UINT32_MAX) return;

    v.stealTailPhase[tailSlot] = v.phases[handle];
    v.stealTailPhaseInc[tailSlot] = v.phaseIncs[handle];
    v.stealTailGain[tailSlot] = gain;
    v.stealTailMixGainL[tailSlot] = mixL;
    v.stealTailMixGainR[tailSlot] = mixR;
    v.stealTailSampleStart[tailSlot] = v.sampleStart[handle];
    v.stealTailRelEnd[tailSlot] = v.relEnd[handle];
    v.stealTailRelLoopS[tailSlot] = v.relLoopS[handle];
    v.stealTailRelLoopE[tailSlot] = v.relLoopE[handle];
    v.stealTailRelLoopSF[tailSlot] = v.relLoopSF[handle];
    v.stealTailRelLoopEF[tailSlot] = v.relLoopEF[handle];
    v.stealTailSampleBacked[tailSlot] = v.sampleBacked[handle];
    v.stealTailLoopEnabled[tailSlot] = v.loopEnabled[handle];
    v.stealTailChannel[tailSlot] = v.channel[handle];
    v.stealTailFramesRemaining[tailSlot] = stealFadeFrames_;
    v.stealTailFramesTotal[tailSlot] = stealFadeFrames_;
    const bool alreadyLinked = stealTailPosition_[tailSlot] < stealTailCount_;
    LinkStealTail(static_cast<VoiceHandle>(tailSlot));
    if (alreadyLinked && stealTailMinHeapValid_ &&
        stealTailMinHeapFrame_ == currentFrame_ &&
        stealTailMinHeapCount_ != 0u && stealTailMinHeap_[0] == tailSlot) {
        // SelectStealTailSlot accepts a replacement only when it is louder
        // than the current root, so the updated root can move only downward.
        stealTailMinHeapLevel_[0] = outgoingLevel;
        StealTailHeapSiftDown(0u);
    }
}

inline void VoiceManager::RetireStolenSibling(VoiceHandle handle,
                                               VoiceHandle selectedVictim) {
    if (handle >= maxVoices_ ||
        v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return;
    if (stealHeapValid_) RemoveStealCandidate(handle);
    CaptureStealTail(handle);
    ++stealCount_;
    UnlinkChannelKey(handle);
    UnlinkPlayGroup(handle);
    UnlinkChannelActive(handle);
    UnlinkRenderClass(handle);
    stealCandidateDeferred_[handle] = 0u;
    stealCandidateReserved_[handle] = 0u;
    v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
    v.currentGain[handle] = 0.0f;
    freeStack_[freeTop_++] = static_cast<int32_t>(handle);
    assert(freeTop_ <= maxVoices_ && "freeStack_ overflow in grouped stealing");

    const uint32_t position = activePosition_[handle];
    assert(position < activeCount_ && activeList_[position] == handle);
    const uint32_t lastPosition = --activeCount_;
    if (position != lastPosition) {
        const uint32_t moved = activeList_[lastPosition];
        activeList_[position] = moved;
        activePosition_[moved] = position;
        // Heap ties use active-list position. The already-popped selected
        // victim deliberately remains absent until its replacement is ready.
        if (stealHeapValid_ && moved != selectedVictim)
            UpdateStealCandidate(static_cast<VoiceHandle>(moved));
    }
    activePosition_[handle] = UINT32_MAX;
}

inline bool VoiceManager::HigherPriorityCandidate(const StealCandidate& a,
                                                   const StealCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.activePosition < b.activePosition;
}

inline VoiceHandle VoiceManager::SelectStableWinner(VoiceHandle left,
                                                     VoiceHandle right) const {
    if (left == kInvalidVoice) return right;
    if (right == kInvalidVoice) return left;
    return HigherPriorityCandidate(stealStableCandidate_[left],
                                   stealStableCandidate_[right])
        ? left : right;
}

inline void VoiceManager::RefreshStealWinnerPath(VoiceHandle handle) {
    uint32_t node = kStealTreeLeafBase + handle;
    VoiceHandle winner = static_cast<VoiceHandle>(stealWinnerTree_[node]);
    while (node > 1u) {
        const VoiceHandle sibling = static_cast<VoiceHandle>(
            stealWinnerTree_[node ^ 1u]);
        winner = (node & 1u) != 0u
            ? SelectStableWinner(sibling, winner)
            : SelectStableWinner(winner, sibling);
        node >>= 1u;
        stealWinnerTree_[node] = winner;
    }
}

inline void VoiceManager::RefreshStealWinnerPaths(
    const VoiceHandle* handles, uint32_t count) {
    // Cached launch plans cover at most eight layers. Recompute the union of
    // their tournament-tree paths bottom-up so shared ancestors are touched
    // once instead of once per physical voice.
    static constexpr uint32_t kBatchPaths = 8u;
    if (!handles || count == 0u) return;
    if (count > kBatchPaths) {
        for (uint32_t i = 0u; i < count; ++i)
            RefreshStealWinnerPath(handles[i]);
        return;
    }

    uint32_t nodes[kBatchPaths]{};
    uint32_t nodeCount = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t node = (kStealTreeLeafBase + handles[i]) >> 1u;
        bool duplicate = false;
        for (uint32_t existing = 0u; existing < nodeCount; ++existing)
            duplicate |= nodes[existing] == node;
        if (!duplicate) nodes[nodeCount++] = node;
    }

    while (nodeCount != 0u) {
        uint32_t parentCount = 0u;
        for (uint32_t i = 0u; i < nodeCount; ++i) {
            const uint32_t node = nodes[i];
            stealWinnerTree_[node] = SelectStableWinner(
                static_cast<VoiceHandle>(stealWinnerTree_[node << 1u]),
                static_cast<VoiceHandle>(stealWinnerTree_[(node << 1u) + 1u]));
            if (node == 1u) continue;
            const uint32_t parent = node >> 1u;
            bool duplicate = false;
            for (uint32_t existing = 0u; existing < parentCount; ++existing)
                duplicate |= nodes[existing] == parent;
            if (!duplicate) nodes[parentCount++] = parent;
        }
        nodeCount = parentCount;
    }
}

inline void VoiceManager::RebuildStableWinnerTree() {
    for (uint32_t node = kStealTreeLeafBase; node-- > 1u;)
        stealWinnerTree_[node] = SelectStableWinner(
            static_cast<VoiceHandle>(stealWinnerTree_[node << 1u]),
            static_cast<VoiceHandle>(stealWinnerTree_[(node << 1u) + 1u]));
}

inline void VoiceManager::BuildStealHeap() {
    ++stealHeapBuildCount_;
    stealHeapCount_ = 0u;
    stealVolatileCount_ = 0u;
    stealVolatileHeapCount_ = 0u;
    stealVolatileHeapValid_ = false;
    std::memset(stealWinnerTree_, 0xff, sizeof(stealWinnerTree_));
    std::memset(stealVolatilePosition_, 0xff, sizeof(stealVolatilePosition_));
    for (uint32_t position = 0; position < activeCount_; ++position) {
        const uint32_t handle = activeList_[position];
        if (IsStableStealCandidate(handle)) {
            ++stealHeapCount_;
            stealStableCandidate_[handle] = {
                ComputeStableStealKey(handle), handle, position};
            stealWinnerTree_[kStealTreeLeafBase + handle] = handle;
        } else {
            LinkVolatileCandidate(static_cast<VoiceHandle>(handle));
        }
    }
    RebuildStableWinnerTree();
    stealHeapValid_ = true;
}

inline VoiceHandle VoiceManager::PopStealCandidate(uint32_t& activePosition,
                                                    bool reserveVolatileRoot) {
    if (!stealHeapValid_) BuildStealHeap();
    // Transient gains change while samples render, not between equal-frame
    // MIDI events. Rebuild once when the output frame advances and keep exact
    // O(log N) replacement updates for the rest of that frame.
    if (!stealVolatileHeapValid_ || stealVolatileHeapFrame_ != currentFrame_)
        BuildVolatileStealHeap();

    StealCandidate best{};
    bool haveBest = false;
    bool bestIsVolatile = false;
    if (stealHeapCount_ > 0u) {
        const VoiceHandle stableWinner = static_cast<VoiceHandle>(
            stealWinnerTree_[1]);
        assert(stableWinner != kInvalidVoice);
        best = stealStableCandidate_[stableWinner];
        haveBest = true;
    }
    if (stealVolatileHeapCount_ > 0u) {
        const StealCandidate& candidate = stealVolatileHeap_[0];
        if (!haveBest || HigherPriorityCandidate(candidate, best)) {
            best = candidate;
            haveBest = true;
            bestIsVolatile = true;
        }
    }
    for (uint32_t i = 0; i < stealVolatileCount_; ++i) {
        const uint32_t handle = stealVolatileList_[i];
        if (stealVolatileHeapPosition_[handle] < stealVolatileHeapCount_)
            continue;
        const StealCandidate candidate{
            ComputeStableStealKey(static_cast<VoiceHandle>(handle)),
            handle, activePosition_[handle]};
        if (!haveBest || HigherPriorityCandidate(candidate, best)) {
            best = candidate;
            haveBest = true;
            bestIsVolatile = true;
        }
    }
    if (!haveBest) return kInvalidVoice;
    activePosition = best.activePosition;
    if (bestIsVolatile) {
        const bool canReserve = reserveVolatileRoot &&
            stealVolatileHeapPosition_[best.handle] == 0u;
        if (canReserve) {
            stealCandidateReserved_[best.handle] = 1u;
        } else {
            UnlinkVolatileCandidate(static_cast<VoiceHandle>(best.handle));
        }
    } else {
        if (reserveVolatileRoot) {
            // A single transactional note launch overwrites this handle and
            // commits before another steal. Keep its leaf in place so commit
            // changes one winner-tree path instead of remove + insert paths.
            stealCandidateReserved_[best.handle] = 2u;
        } else {
            --stealHeapCount_;
            stealWinnerTree_[kStealTreeLeafBase + best.handle] = kInvalidVoice;
            RefreshStealWinnerPath(static_cast<VoiceHandle>(best.handle));
        }
    }
    return static_cast<VoiceHandle>(best.handle);
}

inline void VoiceManager::PushStealCandidate(VoiceHandle handle,
                                              uint32_t activePosition) {
    if (!stealHeapValid_ || handle >= maxVoices_) return;
    if (!IsStableStealCandidate(handle)) {
        LinkVolatileCandidate(handle);
        return;
    }
    ++stealHeapCount_;
    stealStableCandidate_[handle] = {
        ComputeStableStealKey(handle), handle, activePosition};
    stealWinnerTree_[kStealTreeLeafBase + handle] = handle;
    RefreshStealWinnerPath(handle);
}

inline void VoiceManager::UpdateStealCandidate(VoiceHandle handle) {
    if (!stealHeapValid_ || handle >= maxVoices_ ||
        stealCandidateDeferred_[handle] != 0u) return;
    RemoveStealCandidate(handle);
    if (v.state[handle] != static_cast<uint8_t>(VoiceState::Free))
        PushStealCandidate(handle, activePosition_[handle]);
}

inline bool VoiceManager::IsStableStealCandidate(VoiceHandle handle) const {
    if (handle >= maxVoices_ ||
        v.state[handle] != static_cast<uint8_t>(VoiceState::Active)) {
        return false;
    }
    // ComputeEffectiveStealLevel uses targetGain throughout delay, hold and
    // attack, so after removing the shared current-frame age term their key is
    // constant just like sustain. Decay is the only active stage whose
    // effective level changes every rendered frame.
    return v.envelopeStage[handle] != 2u;
}

inline float VoiceManager::ComputeStableStealKey(VoiceHandle handle) const {
    const float commonAgeScore =
        static_cast<float>(currentFrame_) * (1.0f / 256.0f);
    return ComputeStealScore(handle) - commonAgeScore;
}

inline void VoiceManager::LinkVolatileCandidate(VoiceHandle handle) {
    if (handle >= maxVoices_ ||
        stealVolatilePosition_[handle] < stealVolatileCount_) return;
    const uint32_t position = stealVolatileCount_++;
    stealVolatileList_[position] = handle;
    stealVolatilePosition_[handle] = position;
    if (stealVolatileHeapValid_ && stealVolatileHeapFrame_ == currentFrame_) {
        const uint32_t heapPosition = stealVolatileHeapCount_++;
        stealVolatileHeap_[heapPosition] = {
            ComputeStableStealKey(handle), handle, activePosition_[handle]};
        stealVolatileHeapPosition_[handle] = heapPosition;
        VolatileHeapSiftUp(heapPosition);
    }
}

inline void VoiceManager::UnlinkVolatileCandidate(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    RemoveVolatileHeapCandidate(handle);
    const uint32_t position = stealVolatilePosition_[handle];
    if (position >= stealVolatileCount_) return;
    const uint32_t last = --stealVolatileCount_;
    if (position != last) {
        const uint32_t moved = stealVolatileList_[last];
        stealVolatileList_[position] = moved;
        stealVolatilePosition_[moved] = position;
    }
    stealVolatilePosition_[handle] = UINT32_MAX;
}

inline void VoiceManager::VolatileHeapSwap(uint32_t a, uint32_t b) {
    const StealCandidate temporary = stealVolatileHeap_[a];
    stealVolatileHeap_[a] = stealVolatileHeap_[b];
    stealVolatileHeap_[b] = temporary;
    stealVolatileHeapPosition_[stealVolatileHeap_[a].handle] = a;
    stealVolatileHeapPosition_[stealVolatileHeap_[b].handle] = b;
}

inline void VoiceManager::VolatileHeapSiftUp(uint32_t position) {
    while (position > 0u) {
        const uint32_t parent = (position - 1u) >> 1u;
        if (!HigherPriorityCandidate(stealVolatileHeap_[position],
                                     stealVolatileHeap_[parent])) break;
        VolatileHeapSwap(position, parent);
        position = parent;
    }
}

inline void VoiceManager::VolatileHeapSiftDown(uint32_t position) {
    for (;;) {
        const uint32_t left = position * 2u + 1u;
        if (left >= stealVolatileHeapCount_) break;
        const uint32_t right = left + 1u;
        uint32_t best = left;
        if (right < stealVolatileHeapCount_ &&
            HigherPriorityCandidate(stealVolatileHeap_[right],
                                    stealVolatileHeap_[left])) best = right;
        if (!HigherPriorityCandidate(stealVolatileHeap_[best],
                                     stealVolatileHeap_[position])) break;
        VolatileHeapSwap(position, best);
        position = best;
    }
}

inline void VoiceManager::BuildVolatileStealHeap() {
    for (uint32_t position = 0; position < stealVolatileHeapCount_; ++position)
        stealVolatileHeapPosition_[stealVolatileHeap_[position].handle] =
            UINT32_MAX;
    stealVolatileHeapCount_ = 0u;
    for (uint32_t position = 0; position < stealVolatileCount_; ++position) {
        const uint32_t handle = stealVolatileList_[position];
        const uint32_t heapPosition = stealVolatileHeapCount_++;
        stealVolatileHeap_[heapPosition] = {
            ComputeStableStealKey(static_cast<VoiceHandle>(handle)),
            handle, activePosition_[handle]};
        stealVolatileHeapPosition_[handle] = heapPosition;
    }
    if (stealVolatileHeapCount_ > 1u) {
        for (uint32_t position = stealVolatileHeapCount_ / 2u;
             position-- > 0u;) VolatileHeapSiftDown(position);
    }
    stealVolatileHeapFrame_ = currentFrame_;
    stealVolatileHeapValid_ = true;
}

inline void VoiceManager::RemoveVolatileHeapCandidate(VoiceHandle handle) {
    if (!stealVolatileHeapValid_ || handle >= maxVoices_) return;
    const uint32_t position = stealVolatileHeapPosition_[handle];
    if (position >= stealVolatileHeapCount_) return;
    const uint32_t last = --stealVolatileHeapCount_;
    stealVolatileHeapPosition_[handle] = UINT32_MAX;
    if (position != last) {
        stealVolatileHeap_[position] = stealVolatileHeap_[last];
        stealVolatileHeapPosition_[stealVolatileHeap_[position].handle] = position;
        VolatileHeapSiftUp(position);
        const uint32_t adjusted =
            stealVolatileHeapPosition_[stealVolatileHeap_[position].handle];
        VolatileHeapSiftDown(adjusted);
    }
}

inline void VoiceManager::RemoveStealCandidate(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    UnlinkVolatileCandidate(handle);
    const uint32_t leaf = kStealTreeLeafBase + handle;
    if (stealWinnerTree_[leaf] == kInvalidVoice) return;
    stealWinnerTree_[leaf] = kInvalidVoice;
    assert(stealHeapCount_ > 0u);
    --stealHeapCount_;
    RefreshStealWinnerPath(handle);
}

inline VoiceHandle VoiceManager::AllocateVoice(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (freeTop_ == 0) return kInvalidVoice;
    uint32_t idx = freeStack_[--freeTop_];

    InitializeVoice(static_cast<VoiceHandle>(idx), channel, note, velocity);
    LinkChannelKey(static_cast<VoiceHandle>(idx));
    LinkChannelActive(static_cast<VoiceHandle>(idx));
    LinkRenderClass(static_cast<VoiceHandle>(idx));

    activeList_[activeCount_] = idx;
    activePosition_[idx] = activeCount_;
    activeCount_++;
    if (stealHeapValid_)
        PushStealCandidate(static_cast<VoiceHandle>(idx),
                           activePosition_[idx]);

    return static_cast<VoiceHandle>(idx);
}

inline VoiceHandle VoiceManager::AllocateVoiceOrSteal(uint8_t channel, uint8_t note,
                                                        uint8_t velocity,
                                                        bool* outStolen,
                                                        bool deferCandidate,
                                                        bool reserveCandidateInPlace) {
    VoiceHandle vh = kInvalidVoice;
    if (deferCandidate && freeTop_ != 0u) {
        const uint32_t idx = static_cast<uint32_t>(freeStack_[--freeTop_]);
        vh = static_cast<VoiceHandle>(idx);
        InitializePreparedVoice(vh, channel, note, velocity);
        LinkChannelKey(vh);
        LinkChannelActive(vh);
        activeList_[activeCount_] = idx;
        activePosition_[idx] = activeCount_++;
        // A deferred voice is invisible to the steal index until its
        // configuration transaction commits. Keep the existing heap valid.
    } else {
        vh = AllocateVoice(channel, note, velocity);
    }
    if (vh != kInvalidVoice) {
        stealCandidateDeferred_[vh] = deferCandidate ? 1u : 0u;
        if (outStolen) *outStolen = false;
        return vh;
    }

    // Pool is full — find the lowest-priority voice to steal.
    uint32_t bestPos = 0;
    const VoiceHandle bestHandle = PopStealCandidate(
        bestPos, deferCandidate && reserveCandidateInPlace);
    if (bestHandle == kInvalidVoice) return kInvalidVoice;
    const uint32_t bestIdx = bestHandle;

    // Every SF2 region produced by one MIDI note-on shares playIndex. Stereo
    // SoundFonts commonly use one left and one right region; stealing just one
    // side collapses the note into the opposite ear. Retire the whole physical
    // voice group atomically, leaving the selected slot for this replacement
    // and pushing its siblings onto the normal free stack for following layers.
    const uint32_t victimPlayIndex = v.playIndex[bestIdx];
    const bool hasSiblings = victimPlayIndex != UINT32_MAX &&
        (playGroupPrev_[bestIdx] >= 0 || playGroupNext_[bestIdx] >= 0);
    if (hasSiblings) {
        // A reserved volatile root was intentionally left in the index for a
        // single deferred replacement. Group retirement changes its active
        // position, so remove it now and insert the configured replacement at
        // commit instead of invalidating and rebuilding the complete heap.
        if (stealCandidateReserved_[bestIdx] != 0u) {
            stealCandidateReserved_[bestIdx] = 0u;
            RemoveStealCandidate(static_cast<VoiceHandle>(bestIdx));
        }
        int32_t linked = playGroupPrev_[bestIdx];
        while (linked >= 0) {
            const VoiceHandle sibling = static_cast<VoiceHandle>(linked);
            linked = playGroupPrev_[sibling];
            RetireStolenSibling(sibling, bestHandle);
        }
        linked = playGroupNext_[bestIdx];
        while (linked >= 0) {
            const VoiceHandle sibling = static_cast<VoiceHandle>(linked);
            linked = playGroupNext_[sibling];
            RetireStolenSibling(sibling, bestHandle);
        }
        bestPos = activePosition_[bestIdx];
    }
    CaptureStealTail(bestHandle);

    // Retire the victim.
    ++stealCount_;
    UnlinkChannelKey(static_cast<VoiceHandle>(bestIdx));
    UnlinkPlayGroup(static_cast<VoiceHandle>(bestIdx));
    UnlinkChannelActive(static_cast<VoiceHandle>(bestIdx));
    UnlinkRenderClass(static_cast<VoiceHandle>(bestIdx));
    v.state[bestIdx] = static_cast<uint8_t>(VoiceState::Free);

    // Reinitialize in-place
    if (deferCandidate)
        InitializePreparedVoice(static_cast<VoiceHandle>(bestIdx), channel,
                                note, velocity);
    else
        InitializeVoice(static_cast<VoiceHandle>(bestIdx), channel, note, velocity);
    stealCandidateDeferred_[bestIdx] = deferCandidate ? 1u : 0u;
    // The outgoing victim needs an anti-click tail. The replacement is a
    // legitimate new attack and must start at its SF2 envelope level; fading
    // every replacement in smears dense streams once stealing becomes steady.
    v.stealFadeInFramesRemaining[bestIdx] = 0;
    v.stealFadeInFramesTotal[bestIdx] = 0;
    LinkChannelKey(static_cast<VoiceHandle>(bestIdx));
    LinkChannelActive(static_cast<VoiceHandle>(bestIdx));
    if (!deferCandidate)
        LinkRenderClass(static_cast<VoiceHandle>(bestIdx));
    activeList_[bestPos] = bestIdx;  // reuse the victim's position
    activePosition_[bestIdx] = bestPos;
    if (!deferCandidate)
        PushStealCandidate(static_cast<VoiceHandle>(bestIdx), bestPos);

    if (outStolen) *outStolen = true;
    return static_cast<VoiceHandle>(bestIdx);
}

inline void VoiceManager::CommitVoiceConfiguration(VoiceHandle handle) {
    if (handle >= maxVoices_ || stealCandidateDeferred_[handle] == 0u) return;
    stealCandidateDeferred_[handle] = 0u;
    if (stealCandidateReserved_[handle] != 0u) {
        const uint8_t reservation = stealCandidateReserved_[handle];
        stealCandidateReserved_[handle] = 0u;
        if (reservation == 2u) {
            if (IsStableStealCandidate(handle)) {
                stealStableCandidate_[handle] = {
                    ComputeStableStealKey(handle), handle,
                    activePosition_[handle]};
                RefreshStealWinnerPath(handle);
            } else {
                RemoveStealCandidate(handle);
                if (stealHeapValid_)
                    PushStealCandidate(handle, activePosition_[handle]);
            }
            return;
        }
        const uint32_t heapPosition = stealVolatileHeapPosition_[handle];
        if (!IsStableStealCandidate(handle) &&
            heapPosition < stealVolatileHeapCount_ &&
            stealVolatileHeapFrame_ == currentFrame_) {
            stealVolatileHeap_[heapPosition] = {
                ComputeStableStealKey(handle), handle,
                activePosition_[handle]};
            VolatileHeapSiftUp(heapPosition);
            VolatileHeapSiftDown(stealVolatileHeapPosition_[handle]);
            return;
        }
        // Attack/release replacements belong to the exact dynamic list,
        // while a rare immediately-stable replacement belongs to the stable
        // heap.  Reclassify those uncommon cases through the normal path.
        RemoveStealCandidate(handle);
        if (stealHeapValid_ &&
            v.state[handle] != static_cast<uint8_t>(VoiceState::Free)) {
            PushStealCandidate(handle, activePosition_[handle]);
        }
        return;
    }
    if (stealHeapValid_ &&
        v.state[handle] != static_cast<uint8_t>(VoiceState::Free)) {
        PushStealCandidate(handle, activePosition_[handle]);
    }
}

inline void VoiceManager::StartRelease(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Active)) {
        UnlinkChannelKey(handle);
        v.state[handle] = static_cast<uint8_t>(VoiceState::Releasing);
        // SF2 sampleModes 3 = loop during key depression: stop looping so the
        // sample plays out to its end through the release tail.
        if (v.loopMode[handle] == 3) v.loopEnabled[handle] = 0;
        RefreshRenderClass(handle);
    }
}

inline void VoiceManager::RetireVoice(VoiceHandle handle) {
    if (handle >= maxVoices_ || v.state[handle] == static_cast<uint8_t>(VoiceState::Free))
        return;

    ++retireCount_;
    if (GetVoiceAge(handle) < 2) ++retireImmediateCount_;

    // Stable one-shot retirement is cheap to maintain incrementally. Release
    // storms can retire hundreds of volatile voices in one span; updating a
    // winner path for every swap is slower than one lazy rebuild before the
    // next steal, so invalidate once for that workload.
    const bool maintainStealIndex = stealHeapValid_ &&
        IsStableStealCandidate(handle);
    if (maintainStealIndex)
        RemoveStealCandidate(handle);
    else
        stealHeapValid_ = false;
    UnlinkChannelKey(handle);
    UnlinkPlayGroup(handle);
    UnlinkChannelActive(handle);
    UnlinkRenderClass(handle);
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
        if (maintainStealIndex)
            UpdateStealCandidate(static_cast<VoiceHandle>(moved));
    }
    --activeCount_;
    activePosition_[handle] = UINT32_MAX;
}

inline void VoiceManager::RebuildActivePositions() {
    for (uint32_t position = 0; position < activeCount_; ++position)
        activePosition_[activeList_[position]] = position;
    stealHeapValid_ = false;
}

inline bool VoiceManager::IsActive(VoiceHandle handle) const {
    return handle < maxVoices_ && v.state[handle] != static_cast<uint8_t>(VoiceState::Free);
}

inline void VoiceManager::ConfigureVoice(
    VoiceHandle handle, const VoiceConfiguration& setup,
    const ChannelParamsSnapshot& cp, bool commitDeferred) {
    if (handle >= maxVoices_) return;

    v.sampleStart[handle] = setup.sampleStart;
    v.sampleEnd[handle] = setup.sampleEnd;
    v.loopStart[handle] = setup.loopStart;
    v.loopEnd[handle] = setup.loopEnd;
    v.loopMode[handle] = setup.loopMode;
    v.phaseIncs[handle] = setup.phaseStep;
    v.basePhaseIncs[handle] = setup.basePhaseStep;
    v.pitchBendScales[handle] = setup.pitchBendScale;
    v.sampleBacked[handle] = setup.sampleBacked;
    v.phases[handle] = 0.0f;

    const uint32_t relEnd = setup.sampleEnd > setup.sampleStart
        ? setup.sampleEnd - setup.sampleStart : 0u;
    const uint32_t relLoopStart = setup.loopStart > setup.sampleStart
        ? setup.loopStart - setup.sampleStart : 0u;
    const uint32_t relLoopEnd = setup.loopEnd > setup.sampleStart
        ? setup.loopEnd - setup.sampleStart : 0u;
    v.relEnd[handle] = relEnd;
    v.relLoopS[handle] = relLoopStart;
    v.relLoopE[handle] = relLoopEnd;
    v.relLoopSF[handle] = static_cast<float>(relLoopStart);
    v.relLoopEF[handle] = static_cast<float>(relLoopEnd);
    v.loopEnabled[handle] =
        ((setup.loopMode == 1u || setup.loopMode == 3u) &&
         setup.loopEnd > setup.loopStart + 1u) ? 1u : 0u;

    v.presetIndex[handle] = setup.presetIndex;
    v.regionIndex[handle] = setup.regionIndex;
    SetVoicePlayIndex(handle, setup.playIndex);
    v.targetGain[handle] = setup.initialGain;
    v.sustainLevel[handle] = setup.sustainLevel * setup.initialGain;
    v.delaySamplesRemaining[handle] = setup.delaySamples;
    v.holdSamplesRemaining[handle] = setup.holdSamples;
    v.attackSamplesRemaining[handle] = setup.attackSamples;
    v.decaySamplesRemaining[handle] = setup.decaySamples;
    v.attackGainStep[handle] = setup.attackGainStep;
    v.decaySlope[handle] = setup.decaySlope;
    v.releaseDecay[handle] = setup.releaseDecay;
    v.releaseSamplesRemaining[handle] = setup.releaseSamples > 0u
        ? setup.releaseSamples : UINT32_MAX;
    v.currentGain[handle] = 0.0f;
    if (setup.delaySamples > 0u) {
        v.envelopeStage[handle] = 4u;
    } else if (setup.holdSamples > 0u) {
        v.envelopeStage[handle] = 0u;
    } else if (setup.attackSamples > 0u) {
        v.envelopeStage[handle] = 1u;
    } else if (setup.decaySamples > 0u) {
        v.envelopeStage[handle] = 2u;
        v.currentGain[handle] = setup.initialGain;
    } else {
        v.envelopeStage[handle] = 3u;
        v.currentGain[handle] = setup.initialGain;
    }

    v.gainLeft[handle] = setup.gainLeft;
    v.gainRight[handle] = setup.gainRight;
    v.mixGainL[handle] = setup.gainLeft * cp.panLeft * cp.volume * cp.expression;
    v.mixGainR[handle] = setup.gainRight * cp.panRight * cp.volume * cp.expression;
    v.renderGainL[handle] = v.currentGain[handle] * v.mixGainL[handle];
    v.renderGainR[handle] = v.currentGain[handle] * v.mixGainR[handle];

    RefreshRenderClass(handle);
    if (commitDeferred)
        CommitVoiceConfiguration(handle);
    else
        UpdateStealCandidate(handle);
}

inline bool VoiceManager::ReuseMatchingStealGroup(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count,
    VoiceHandle* outHandles, bool& candidatesReservedInPlace) {
    candidatesReservedInPlace = false;
    if (freeTop_ != 0u || count < 2u || count > maxVoices_ || !setups ||
        !outHandles)
        return false;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    ++groupReuseAttemptCount_;
#endif

    uint32_t selectedPosition = 0u;
    // Keep the selected leaf present until we know whether the complete
    // physical group can be replaced in-place. No other selection occurs
    // during this transaction, so the reservation is exact and private to
    // the audio thread.
    const VoiceHandle selected = PopStealCandidate(selectedPosition, true);
    if (selected == kInvalidVoice) return false;

    uint32_t groupCount = 1u;
    outHandles[0] = selected;
    bool tooMany = false;
    int32_t linked = playGroupPrev_[selected];
    while (linked >= 0) {
        if (groupCount >= count) { tooMany = true; break; }
        outHandles[groupCount++] = static_cast<VoiceHandle>(linked);
        linked = playGroupPrev_[static_cast<uint32_t>(linked)];
    }
    if (!tooMany) {
        linked = playGroupNext_[selected];
        while (linked >= 0) {
            if (groupCount >= count) { tooMany = true; break; }
            outHandles[groupCount++] = static_cast<VoiceHandle>(linked);
            linked = playGroupNext_[static_cast<uint32_t>(linked)];
        }
    }

    if (groupCount != count || tooMany) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (tooMany || groupCount > count) ++groupReuseLargerCount_;
        else ++groupReuseSmallerCount_;
#endif
        // The caller needs a different number of slots. Restore the exact
        // candidate and use the general allocation path, which may retire a
        // larger/smaller physical group according to the existing policy.
        stealCandidateReserved_[selected] = 0u;
        return false;
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    ++groupReuseMatchCount_;
#endif

    bool canReserveInPlace = count <= 8u;
    for (uint32_t i = 0u; i < count && canReserveInPlace; ++i) {
        const VoiceHandle handle = outHandles[i];
        const VoiceConfiguration& setup = setups[i];
        const bool preDecay = setup.delaySamples != 0u ||
            setup.holdSamples != 0u || setup.attackSamples != 0u;
        const bool configuredStable = setup.sampleBacked != 0u &&
            setup.sampleEnd > setup.sampleStart + 1u &&
            (preDecay || setup.decaySamples == 0u);
        canReserveInPlace = configuredStable &&
            IsStableStealCandidate(handle) &&
            stealWinnerTree_[kStealTreeLeafBase + handle] == handle;
    }

    if (canReserveInPlace) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        ++groupReuseReservedCount_;
#endif
        // The old and new voices occupy the same leaves. Preserve the leaves
        // while lifecycle/sample state is rewritten, then refresh their
        // cached scores and the union of both paths once at commit.
        candidatesReservedInPlace = true;
        for (uint32_t i = 0u; i < count; ++i) {
            stealCandidateReserved_[outHandles[i]] = 2u;
            stealCandidateDeferred_[outHandles[i]] = 1u;
        }
    } else {
        stealCandidateReserved_[selected] = 0u;
        RemoveStealCandidate(selected);
        for (uint32_t i = 1u; i < count; ++i)
            RemoveStealCandidate(outHandles[i]);
    }

    // Capture every old layer, then replace the complete physical note in
    // place. The fast path keeps its winner-tree leaves reserved; the fallback
    // removed them above. Active positions and the free stack never move.
    for (uint32_t i = 0u; i < count; ++i)
        CaptureStealTail(outHandles[i]);

    for (uint32_t i = 0u; i < count; ++i) {
        const VoiceHandle handle = outHandles[i];
        const bool preserveChannelIndex = candidatesReservedInPlace &&
            v.channel[handle] == channel;
        const bool configuredLoop =
            (setups[i].loopMode == 1u || setups[i].loopMode == 3u) &&
            setups[i].loopEnd > setups[i].loopStart + 1u;
        uint8_t configuredStage = 3u;
        if (setups[i].delaySamples != 0u) configuredStage = 4u;
        else if (setups[i].holdSamples != 0u) configuredStage = 0u;
        else if (setups[i].attackSamples != 0u) configuredStage = 1u;
        else if (setups[i].decaySamples != 0u) configuredStage = 2u;
        VoiceRenderClass desiredClass = VoiceRenderClass::Generic;
        if (configuredStage == 3u) {
            desiredClass = configuredLoop
                ? VoiceRenderClass::SustainedLoop
                : VoiceRenderClass::SustainedOneShot;
        } else if (configuredLoop &&
                   (configuredStage == 1u || configuredStage == 2u)) {
            desiredClass = VoiceRenderClass::TransientLoop;
        }
        const uint8_t configuredClass = static_cast<uint8_t>(desiredClass);
        const bool preserveRenderIndex = candidatesReservedInPlace &&
            v.renderClass[handle] == configuredClass;
        ++stealCount_;
        UnlinkChannelKey(handle);
        UnlinkPlayGroup(handle);
        if (!preserveChannelIndex) UnlinkChannelActive(handle);
        if (!preserveRenderIndex) UnlinkRenderClass(handle);
        stealCandidateDeferred_[handle] = 1u;
        if (!candidatesReservedInPlace)
            stealCandidateReserved_[handle] = 0u;
        v.state[handle] = static_cast<uint8_t>(VoiceState::Free);
        v.currentGain[handle] = 0.0f;

        InitializePreparedVoice(handle, channel, note, velocity);
        if (preserveRenderIndex)
            v.renderClass[handle] = configuredClass;
        stealCandidateDeferred_[handle] = 1u;
        LinkChannelKey(handle);
        if (!preserveChannelIndex) LinkChannelActive(handle);
    }
    return true;
}

inline void VoiceManager::CommitVoiceGroupConfigurations(
    const VoiceHandle* handles, uint32_t count,
    bool candidatesReservedInPlace) {
    if (!candidatesReservedInPlace) {
        for (uint32_t layer = 0u; layer < count; ++layer)
            CommitVoiceConfiguration(handles[layer]);
        return;
    }

    bool stillStable = stealHeapValid_ && count <= 8u;
    for (uint32_t layer = 0u; layer < count && stillStable; ++layer) {
        const VoiceHandle handle = handles[layer];
        stillStable = handle < maxVoices_ &&
            stealCandidateDeferred_[handle] != 0u &&
            stealCandidateReserved_[handle] == 2u &&
            IsStableStealCandidate(handle) &&
            stealWinnerTree_[kStealTreeLeafBase + handle] == handle;
    }
    if (!stillStable) {
        for (uint32_t layer = 0u; layer < count; ++layer)
            CommitVoiceConfiguration(handles[layer]);
        return;
    }

    for (uint32_t layer = 0u; layer < count; ++layer) {
        const VoiceHandle handle = handles[layer];
        stealCandidateDeferred_[handle] = 0u;
        stealCandidateReserved_[handle] = 0u;
        stealStableCandidate_[handle] = {
            ComputeStableStealKey(handle), handle, activePosition_[handle]};
    }
    RefreshStealWinnerPaths(handles, count);
}

inline bool VoiceManager::LaunchVoiceGroup(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count,
    const ChannelParamsSnapshot& channelParams, VoiceHandle* outHandles) {
    if (!setups || !outHandles || count == 0u || count > maxVoices_)
        return false;
    bool candidatesReservedInPlace = false;
    if (!ReuseMatchingStealGroup(channel, note, velocity, setups, count,
                                 outHandles, candidatesReservedInPlace)) {
        uint32_t allocated = 0u;
        for (; allocated < count; ++allocated) {
            outHandles[allocated] = AllocateVoiceOrSteal(
                channel, note, velocity, nullptr, true, false);
            if (outHandles[allocated] == kInvalidVoice) {
                for (uint32_t rollback = 0u; rollback < allocated; ++rollback)
                    RetireVoice(outHandles[rollback]);
                return false;
            }
        }
    }
    for (uint32_t layer = 0u; layer < count; ++layer) {
        // Keep every layer invisible to the steal index until the complete
        // physical note has been prepared.  The former layered path linked a
        // Generic candidate, configured it, and reclassified it one layer at
        // a time, multiplying tree/list work for stereo SoundFonts.
        ConfigureVoice(outHandles[layer], setups[layer], channelParams, false);
    }
    CommitVoiceGroupConfigurations(outHandles, count,
                                   candidatesReservedInPlace);
    return true;
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
    v.basePhaseIncs[handle] = phaseStep;
    v.pitchBendScales[handle] = 1.0f;
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
    RefreshRenderClass(handle);
}

inline void VoiceManager::SetVoicePitchBase(VoiceHandle handle,
                                             float basePhaseStep,
                                             float pitchBendScale) {
    if (handle >= maxVoices_) return;
    v.basePhaseIncs[handle] = basePhaseStep;
    v.pitchBendScales[handle] = pitchBendScale;
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
    UnlinkPlayGroup(handle);
    v.playIndex[handle] = playIndex;
    if (playIndex == UINT32_MAX) return;
    if (lastLinkedPlayIndex_ == playIndex &&
        lastLinkedPlayVoice_ < maxVoices_ &&
        v.state[lastLinkedPlayVoice_] != static_cast<uint8_t>(VoiceState::Free) &&
        v.playIndex[lastLinkedPlayVoice_] == playIndex) {
        playGroupPrev_[handle] = static_cast<int32_t>(lastLinkedPlayVoice_);
        playGroupNext_[lastLinkedPlayVoice_] = static_cast<int32_t>(handle);
    }
    lastLinkedPlayIndex_ = playIndex;
    lastLinkedPlayVoice_ = handle;
}

inline uint32_t VoiceManager::FindOldestPlayIndex(uint8_t channel,
                                                   uint8_t note) const {
    if (channel >= kChannelCount || note >= kNoteCount) return UINT32_MAX;
    const int32_t oldest = channelKeyVoiceOldest_[channel][note];
    if (oldest < 0) return UINT32_MAX;
    const uint32_t handle = static_cast<uint32_t>(oldest);
    return v.state[handle] == static_cast<uint8_t>(VoiceState::Active) &&
        !v.heldBySustain[handle] ? v.playIndex[handle] : UINT32_MAX;
}

inline void VoiceManager::StartReleaseForPlayIndex(uint8_t channel,
                                                    uint8_t note,
                                                    uint32_t playIndex) {
    if (playIndex == UINT32_MAX) return;
    int32_t current = channelKeyVoiceHead_[channel][note];
    while (current >= 0) {
        const VoiceHandle handle = static_cast<VoiceHandle>(current);
        current = v.nextChannelKeyVoice[handle];
        if (v.state[handle] != static_cast<uint8_t>(VoiceState::Free) &&
            v.playIndex[handle] == playIndex) {
            StartRelease(handle);
        }
    }
}

inline void VoiceManager::NoteOffPlayIndex(uint8_t channel, uint8_t note,
                                            uint32_t playIndex, bool sustain,
                                            uint32_t blockOffset) {
    if (channel >= kChannelCount || note >= kNoteCount || playIndex == UINT32_MAX)
        return;
    // Generations are inserted as adjacent layers at the head. The tail is
    // the oldest outstanding generation, so ordinary note-off is O(layers)
    // instead of walking every same-key retrigger in the pool.
    int32_t current = channelKeyVoiceOldest_[channel][note];
    if (current >= 0 && v.playIndex[static_cast<uint32_t>(current)] == playIndex) {
        while (current >= 0) {
            const VoiceHandle handle = static_cast<VoiceHandle>(current);
            if (v.playIndex[handle] != playIndex) break;
            current = v.prevChannelKeyVoice[handle];
            if (sustain) {
                v.heldBySustain[handle] = 1u;
                // A sustained generation has received its note-off and must
                // not mask the next retrigger. Sustain release uses the
                // channel-active index, so key-chain membership is unnecessary.
                UnlinkChannelKey(handle);
            } else {
                v.releaseStartInBlock[handle] = blockOffset;
                StartRelease(handle);
            }
        }
        return;
    }

    // Preserve the public helper's behavior for uncommon callers that name a
    // non-oldest generation explicitly.
    current = channelKeyVoiceHead_[channel][note];
    while (current >= 0) {
        const VoiceHandle handle = static_cast<VoiceHandle>(current);
        current = v.nextChannelKeyVoice[handle];
        if (v.state[handle] == static_cast<uint8_t>(VoiceState::Free) ||
            v.playIndex[handle] != playIndex) {
            continue;
        }
        if (sustain) {
            v.heldBySustain[handle] = 1u;
            UnlinkChannelKey(handle);
        } else {
            v.releaseStartInBlock[handle] = blockOffset;
            StartRelease(handle);
        }
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
    RefreshRenderClass(handle);
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
    v.renderGainL[handle] = v.currentGain[handle] * v.mixGainL[handle];
    v.renderGainR[handle] = v.currentGain[handle] * v.mixGainR[handle];
    UpdateStealCandidate(handle);
}

inline void VoiceManager::RefreshMixGains(const ChannelParamsSnapshot* chParams) {
    bool stableTreeDirty = false;
    for (uint32_t ai = 0; ai < activeCount_; ++ai) {
        uint32_t i = activeList_[ai];
        const ChannelParamsSnapshot& cp = chParams[v.channel[i]];
        v.mixGainL[i] = v.gainLeft[i]  * cp.panLeft  * cp.volume * cp.expression;
        v.mixGainR[i] = v.gainRight[i] * cp.panRight * cp.volume * cp.expression;
        v.renderGainL[i] = v.currentGain[i] * v.mixGainL[i];
        v.renderGainR[i] = v.currentGain[i] * v.mixGainR[i];
        if (stealHeapValid_ && stealCandidateDeferred_[i] == 0u &&
            IsStableStealCandidate(static_cast<VoiceHandle>(i)) &&
            stealWinnerTree_[kStealTreeLeafBase + i] == i) {
            stealStableCandidate_[i] = {
                ComputeStableStealKey(static_cast<VoiceHandle>(i)), i,
                activePosition_[i]};
            stableTreeDirty = true;
        } else {
            UpdateStealCandidate(static_cast<VoiceHandle>(i));
        }
    }
    if (stableTreeDirty) RebuildStableWinnerTree();
}


inline void VoiceManager::RefreshMixGainsForChannel(
    uint8_t channel, const ChannelParamsSnapshot& cp) {
    if (channel >= kChannelCount) return;
    const uint32_t count = channelActiveCount_[channel];
    bool stableTreeDirty = false;
    for (uint32_t position = 0; position < count; ++position) {
        const uint32_t i = channelActiveList_[channel][position];
        v.mixGainL[i] = v.gainLeft[i] * cp.panLeft * cp.volume * cp.expression;
        v.mixGainR[i] = v.gainRight[i] * cp.panRight * cp.volume * cp.expression;
        v.renderGainL[i] = v.currentGain[i] * v.mixGainL[i];
        v.renderGainR[i] = v.currentGain[i] * v.mixGainR[i];
        if (stealHeapValid_ && stealCandidateDeferred_[i] == 0u &&
            IsStableStealCandidate(static_cast<VoiceHandle>(i)) &&
            stealWinnerTree_[kStealTreeLeafBase + i] == i) {
            stealStableCandidate_[i] = {
                ComputeStableStealKey(static_cast<VoiceHandle>(i)), i,
                activePosition_[i]};
            stableTreeDirty = true;
        } else {
            UpdateStealCandidate(static_cast<VoiceHandle>(i));
        }
    }
    if (stableTreeDirty) RebuildStableWinnerTree();
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
    // A stolen tail can belong to a different channel than the replacement
    // occupying its slot, so inspect the independent dense tail list.
    uint32_t tailPosition = 0u;
    while (tailPosition < stealTailCount_) {
        const uint32_t idx = stealTailList_[tailPosition];
        if (v.stealTailFramesRemaining[idx] != 0 &&
            v.stealTailChannel[idx] == channel) {
            v.stealTailFramesRemaining[idx] = 0;
            UnlinkStealTail(static_cast<VoiceHandle>(idx));
            continue;
        }
        ++tailPosition;
    }
    while (channelActiveCount_[channel] > 0u) {
        const uint32_t idx =
            channelActiveList_[channel][channelActiveCount_[channel] - 1u];
        // Tail ownership is independent from the active slot and was handled
        // by channel identity above.
        v.stealFadeInFramesRemaining[idx] = 0;
        RetireVoice(static_cast<VoiceHandle>(idx));
    }
}

inline void VoiceManager::ReleaseChannel(uint8_t channel, uint32_t blockOffset) {
    if (channel >= kChannelCount) return;
    const uint32_t count = channelActiveCount_[channel];
    for (uint32_t position = 0; position < count; ++position) {
        const uint32_t idx = channelActiveList_[channel][position];
        if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) continue;
        v.heldBySustain[idx] = 0;
        v.releaseStartInBlock[idx] = blockOffset;
        StartRelease(static_cast<VoiceHandle>(idx));
    }
}

} // namespace svms

#endif
