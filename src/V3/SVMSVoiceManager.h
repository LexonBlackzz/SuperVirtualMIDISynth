#ifndef SVMS_VOICE_MANAGER_H
#define SVMS_VOICE_MANAGER_H

#include "SVMSTypes.h"
#include "SVMSEnvelope.h"
#include "SVMSPhaseRotation.h"
#include "SVMSLiveControl.h"
#include "SVMSRenderKernels.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(_MSC_VER)
#define SVMS_VM_FORCEINLINE __forceinline
#elif defined(__GNUC__)
#define SVMS_VM_FORCEINLINE inline __attribute__((always_inline))
#else
#define SVMS_VM_FORCEINLINE inline
#endif

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
    float vibLfoToPitchCents = 0.0f;
    float vibLfoPhaseStep = 0.0f;
    uint32_t vibLfoDelaySamples = 0u;
    uint16_t presetIndex = UINT16_MAX;
    uint16_t regionIndex = UINT16_MAX;
    uint8_t loopMode = 0;
    uint8_t sampleBacked = 1;
};

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
enum class LaunchProfileStage : uint32_t {
    VictimSelection = 0u,
    TailCapture,
    Lifecycle,
    Configuration,
    TreeMaintenance,
    Count
};

struct LaunchChurnBucketStats {
    uint64_t samples = 0u;
    uint64_t totalCycles = 0u;
    uint64_t stageCycles[static_cast<uint32_t>(LaunchProfileStage::Count)]{};
};

struct LaunchChurnStats {
    // Logical MIDI/SF2 launch transactions versus physical region voices.
    uint64_t logicalLaunches = 0u;
    uint64_t successfulLaunches = 0u;
    uint64_t failedLaunches = 0u;
    uint64_t physicalVoicesRequested = 0u;
    uint64_t physicalVoicesConfigured = 0u;
    uint64_t freeSlotAllocations = 0u;

    // Exact steal composition. One launch may retire multiple play groups.
    uint64_t stealTransactions = 0u;
    uint64_t victimGroups = 0u;
    uint64_t physicalVictims = 0u;
    uint64_t sameFrameVictimGroups = 0u;
    uint64_t sameFramePhysicalVictims = 0u;
    uint64_t monoVictimGroups = 0u;
    uint64_t layeredVictimGroups = 0u;
    uint64_t matchingSizeVictimGroups = 0u;
    uint64_t mismatchedSizeVictimGroups = 0u;
    uint64_t stableVictimGroups = 0u;
    uint64_t volatileVictimGroups = 0u;
    uint64_t sameChannelKeyVictimGroups = 0u;
    uint64_t matchingPlanVictimGroups = 0u;
    uint64_t singleInPlaceVictimGroups = 0u;
    uint64_t matchingReuseVictimGroups = 0u;
    uint64_t reservedReuseVictimGroups = 0u;
    uint64_t generalVictimGroups = 0u;

    // The voices/groups left after all exact-frame replacements are the
    // materialization lower bound for a future shadow-launch transaction.
    uint64_t nextFrameSurvivingGroups = 0u;
    uint64_t nextFrameSurvivingPhysicalVoices = 0u;

    uint64_t tailCaptureAttempts = 0u;
    uint64_t tailCaptureAccepted = 0u;
    uint64_t tailCaptureReplaced = 0u;
    uint64_t tailCaptureRejected = 0u;
    uint64_t tailCaptureIneligible = 0u;

    // Bits: 0=same-frame, 1=layered, 2=volatile,
    // 3=general fallback or a non-reserved matching-group transaction.
    // Bucket 16 contains launches which did not steal.
    static constexpr uint32_t kClassificationBuckets = 17u;
    LaunchChurnBucketStats buckets[kClassificationBuckets]{};
};
#endif

// ════════════════════════════════════════════════════════════════════════
// Phase rotation (black-MIDI hum removal) moved to per-voice processing —
// see SVMSPhaseRotation.h.  The former post-mix allpass cascade was removed:
// a fixed LTI filter on the mixed signal preserves the magnitude of every
// Fourier component of a periodic signal, so the steady-state dispatch-rate
// hum survived it untouched.  The replacement rotates each voice by an
// independent random constant angle (Hilbert/quadrature form) at note-on,
// which decorrelates the voices (hum sums as √N instead of N) while keeping
// the magnitude spectrum and the sample-exact onset timing intact.
// Coherent (mode 0) remains a bit-exact bypass.
// ════════════════════════════════════════════════════════════════════════

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
    using PreTailCaptureHook = void(*)(VoiceHandle handle, void* userData);
    using VoiceConfiguredHook = void(*)(VoiceHandle handle, void* userData);
    VoiceManager();
    VoiceManager(const VoiceManager& other);
    VoiceManager& operator=(const VoiceManager&) = delete;
    ~VoiceManager();
    bool Initialize(uint32_t maxVoices, uint32_t sampleRate = 44100);
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
                                     bool reserveCandidateInPlace = true,
                                     VoiceHandle preselectedVictim = kInvalidVoice,
                                     uint32_t preselectedVictimPosition = 0u);
    // Complete a deferred note-on setup with one exact steal-index update.
    // This avoids repeatedly removing/reinserting the same newborn while its
    // sample, envelope and gains are filled in sequentially.
    void CommitVoiceConfiguration(VoiceHandle handle);
    void ConfigureVoice(VoiceHandle handle, const VoiceConfiguration& setup,
                        const ChannelParamsSnapshot& channelParams,
                        bool commitDeferred);
    void ConfigureVoice(VoiceHandle handle, const VoiceConfiguration& setup,
                        uint32_t playIndex,
                        const ChannelParamsSnapshot& channelParams,
                        bool commitDeferred);
    bool LaunchVoiceGroup(uint8_t channel, uint8_t note, uint8_t velocity,
                          const VoiceConfiguration* setups, uint32_t count,
                          const ChannelParamsSnapshot& channelParams,
                          VoiceHandle* outHandles);
    bool LaunchVoiceGroup(uint8_t channel, uint8_t note, uint8_t velocity,
                          const VoiceConfiguration* setups, uint32_t count,
                          uint32_t playIndex,
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
    uint32_t NoteOffOldestPlayIndices(uint8_t channel, uint8_t note,
                                      uint32_t count, bool sustain,
                                      uint32_t blockOffset);

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
    void CaptureSostenuto(uint8_t channel);
    void ReleaseSostenuto(uint8_t channel, uint32_t blockOffset);
    void ReleaseSustain(uint8_t channel, uint32_t blockOffset);

    uint32_t GetActiveCount() const { return activeCount_; }
    // Largest physical voice group ever created by one MIDI note-on. Every
    // launch updates this in O(1); groups never outgrow their launch count,
    // so it is a monotone upper bound for future victim/newborn group sizes.
    // The dense planner uses it to bound per-launch steal mutations.
    uint32_t GetMaxLaunchGroupSize() const { return maxLaunchGroupSize_; }
    // Physical allocation ceiling. It can grow at a render boundary; it is
    // never shrunk live so existing voice handles remain stable.
    uint32_t GetMaxVoices() const { return maxVoices_; }
    // Logical live ceiling. It may move freely inside (or grow) the pool.
    uint32_t GetVoiceLimit() const { return voiceLimit_; }
    bool GrowCapacity(uint32_t capacity);
    // NOTE: phase rotation is no longer applied per-voice at note-on (see
    // PhaseRotator below). Delaying individual voice onsets only rotates
    // phase relative to a note's own attack, which barely touches sub-100Hz
    // hum and staggers dense onsets into audible smear/clicks. Real phase
    // rotation is a signal-domain filter applied to the continuous mixed
    // output; see PhaseRotator, owned by the render callback/offline synth.

    bool SetVoiceLimit(uint32_t limit);
    uint32_t EnforceVoiceLimit(uint32_t maxReleases = 8192u,
                               float releaseSeconds = 0.050f);
    size_t GetAllocatedBytes() const {
        return sizeof(*this) + v.GetAllocatedBytes() - sizeof(v) +
               metadataBytes_;
    }
    static size_t EstimateAllocatedBytes(uint32_t capacity) noexcept;
    // Apply a process-local live limit request only at a render-block
    // boundary, before dense plans or worker jobs can observe voice state.
    void ApplyRuntimeVoiceLimit(uint64_t frame);
    void SetCurrentFrame(uint64_t frame);
    void SetStealKeyBackend(RenderBackend backend) {
        stealKeyBackend_ = backend;
    }
    uint32_t GetVoiceAge(VoiceHandle handle) const;
    uint32_t GetChannelActiveCount(uint8_t channel) const;
    template <typename Consumer>
    void ForEachChannelActive(uint8_t channel, Consumer&& consume) const noexcept;
    void InvalidateStealCandidates();
    void RefreshRenderClass(VoiceHandle handle);
    uint32_t GetRenderClassCount(VoiceRenderClass renderClass) const;
    template <typename Consumer>
    void ForEachRenderClassBlock(VoiceRenderClass renderClass,
                                 Consumer&& consume) const noexcept;
    uint32_t GetNonemptyRenderClassMask() const { return renderClassMask_; }
    uint32_t GetStealTailCount() const { return stealTailCount_; }
    const uint32_t* GetStealTailList() const { return stealTailList_; }
    void RefreshStealTail(VoiceHandle handle);
    void SetPreTailCaptureHook(PreTailCaptureHook hook,
                               void* userData) noexcept {
        preTailCaptureHook_ = hook;
        preTailCaptureUserData_ = userData;
    }
    void SetVoiceConfiguredHook(VoiceConfiguredHook hook,
                                void* userData) noexcept {
        voiceConfiguredHook_ = hook;
        voiceConfiguredUserData_ = userData;
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    VoiceHandle FindStealVictimExhaustiveForTest() const;
    uint64_t GetStealHeapBuildCountForTest() const {
        return stealHeapBuildCount_;
    }
    uint32_t GetStealTreeLeafBaseForTest() const {
        return stealTreeLeafBase_;
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
    uint64_t GetLaunchProfileSamplesForTest() const {
        return launchProfileSamples_;
    }
    uint64_t GetLaunchProfilePopCyclesForTest() const {
        return launchProfilePopCycles_;
    }
    uint64_t GetLaunchProfileTailCyclesForTest() const {
        return launchProfileTailCycles_;
    }
    uint64_t GetLaunchProfileLifecycleCyclesForTest() const {
        return launchProfileLifecycleCycles_;
    }
    uint64_t GetLaunchProfileConfigureCyclesForTest() const {
        return launchProfileConfigureCycles_;
    }
    uint64_t GetLaunchProfileTreeCyclesForTest() const {
        return launchProfileTreeCycles_;
    }
    uint64_t GetVolatileHeapProfileBuildsForTest() const {
        return volatileHeapProfileBuilds_;
    }
    uint64_t GetVolatileHeapProfileSamplesForTest() const {
        return volatileHeapProfileSamples_;
    }
    uint64_t GetVolatileHeapProfileCyclesForTest() const {
        return volatileHeapProfileCycles_;
    }
    uint64_t GetVolatileHeapProfileCandidatesForTest() const {
        return volatileHeapProfileCandidates_;
    }
    const LaunchChurnStats& GetLaunchChurnStatsForTest() const {
        return launchChurnStats_;
    }
    void SetLaunchChurnProfilingEnabledForTest(bool enabled) {
        launchChurnProfilingEnabled_ = enabled;
        ResetGroupReuseCountersForTest();
    }
    void SetVolatileFallbackScanForTest(bool enabled) {
        volatileFallbackScanForTest_ = enabled;
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
        launchProfileCounter_ = 0u;
        launchProfileSamples_ = 0u;
        launchProfilePopCycles_ = 0u;
        launchProfileTailCycles_ = 0u;
        launchProfileLifecycleCycles_ = 0u;
        launchProfileConfigureCycles_ = 0u;
        launchProfileTreeCycles_ = 0u;
        launchChurnStats_ = LaunchChurnStats{};
        launchTestContext_ = LaunchTestContext{};
        launchTestTrackedFrame_ = currentFrame_;
        launchTestProvisionalGroups_ = 0;
        launchTestProvisionalPhysicalVoices_ = 0;
        volatileHeapProfileCounter_ = 0u;
        volatileHeapProfileBuilds_ = 0u;
        volatileHeapProfileSamples_ = 0u;
        volatileHeapProfileCycles_ = 0u;
        volatileHeapProfileCandidates_ = 0u;
    }
#endif

    // Unit-test oracle: returns exactly the victim AllocateVoiceOrSteal's
    // selection stage yields now — the oldest valid Releasing entry when the
    // fast-path eligibility holds, otherwise (reference builds) the
    // exhaustive winner.
    VoiceHandle PredictStealVictimForTest(uint32_t& activePosition);

    // ── Public read-only access ────────────────────────────────────────
    VoiceSoA v;

    // Per-block voice iteration: activeList[0 .. activeCount_-1] holds
    // the indices of all non-Free voices.  The renderer and driver iterate
    // this list instead of scanning 0..maxVoices_.
    uint32_t activeCount_;
    uint32_t* activeList_;
    uint32_t* activePosition_;
    uint32_t freeTop_;

    void RebuildActivePositions();

    // Diagnostic counters (read from driver)
    uint32_t retireCount_;
    uint32_t retireImmediateCount_;
    uint32_t stealCount_;

    // Exact count of voices currently in the Releasing state, maintained
    // O(1) at every state transition.  Audio-thread-only writes; read by
    // the audio thread for telemetry.  Atomic so a diagnostic reader can
    // never observe a torn value, at zero cost for the single-threaded
    // transitions (uncontended atomic load/store on x86/x64 is a plain
    // mov).
    std::atomic<uint32_t> releasingCount_{0u};

    uint32_t GetReleasingCount() const {
        return releasingCount_.load(std::memory_order_relaxed);
    }

    // Diagnostic counter: steals served by the Releasing-ring fast path
    // instead of the stable-tree/volatile-heap tiers.
    uint64_t releasingRingHits_{0u};
    uint32_t GetReleasingRingCountForTest() const {
        return releasingRingCount_;
    }

    // Master switch for the Releasing-ring steal fast path. On by default;
    // victim-exactness oracle tests opt out where the probed selection route
    // (reserved in-place/group-reuse) intentionally keeps exact tier
    // semantics.
    void SetReleasingRingEnabled(const bool enabled) noexcept {
        enableReleasingRing_ = enabled;
    }

    // Batch steal-candidate pop: retrieves up to `count` victims in one call,
    // selecting each victim with the exact PopStealCandidate(pos, false)
    // tier logic (releasing-ring fast path → stable winner-tree root →
    // general stable-vs-volatile comparison), fully repairing the
    // ring/tree/heap after every individual pop. The resulting victim
    // sequence is identical to `count` sequential single pops: pop 1, then
    // pop 1 again from the updated structures, and so on. Returns the number
    // of victims actually found (fewer than `count` when the candidate
    // structures run dry; 0 when there is nothing to steal).
    // This exists purely to amortize call overhead in same-frame batched
    // note-on runs — it performs exactly the same tree operations as the
    // equivalent sequential pops. (A genuine algorithmic batching that
    // extracts N victims in fewer than N repair operations is out of scope.)
    uint32_t PopStealCandidates(uint32_t count, VoiceHandle* outHandles,
                                uint32_t* outActivePositions);

    // Hot toggle for batched per-launch steal selection (driver; mirrored
    // from the correctness-mode config bool every dispatch run). When false,
    // every LaunchVoiceGroup layer takes the unchanged per-layer
    // PopStealCandidate path.
    void SetStealBatchingEnabled(const bool enabled) noexcept {
        stealBatchingEnabled_ = enabled;
    }

    // Test-only access to the single-victim pop, so the batch wrapper can be
    // proven victim-identical to sequential pops from the regression suite.
    friend struct VoiceStealBatchTestAccess;

    // ── Per-voice phase rotation (SVMSPhaseRotation.h) ─────────────────
    // Mode 0 (Coherent) frees the per-voice state and leaves the render
    // path bit-identical.  Modes 1-4 allocate the state array and seed
    // every currently live voice deterministically; new voices are seeded
    // at allocation.  Returns false only when the state allocation fails
    // (mode stays 0 in that case).
    bool SetPhaseRotationMode(uint32_t mode);
    uint32_t GetPhaseRotationMode() const noexcept {
        return phaseRotationMode_;
    }
    // Rotation activity flag for the render paths (v.rot != nullptr).
    bool PhaseRotationActive() const noexcept { return v.rot != nullptr; }
    // Seed (or re-seed) one voice's rotation state deterministically from
    // its MIDI identity, birth frame and a monotonic counter.
    void SeedVoiceRotationForVoice(VoiceHandle handle);

private:
    struct StealCandidate {
        float score;
        uint32_t handle;
        uint32_t activePosition;
    };
    uint32_t maxVoices_;
    uint32_t voiceLimit_;
    uint32_t sampleRate_;
    // Per-voice phase rotation (SVMSPhaseRotation.h).  Audio-thread-only;
    // 0 = Coherent (v.rot == nullptr, bit-exact render path).
    uint32_t phaseRotationMode_ = 0u;
    uint64_t rotationSeedCounter_ = 0u;
    uint32_t stealFadeFrames_;
    uint64_t currentFrame_;
    uint64_t lastVoiceLimitEnforceFrame_;

    // LIFO free slot stack
    int32_t* freeStack_;

    // Circular ring of voice handles believed to be Releasing. Pushed in
    // StartRelease; consumed (and lazily validated) by the steal fast path.
    // Stale entries are simply skipped at consumption time — never repaired
    // mid-search — which keeps the audio-thread push/pop O(1) with no
    // bookkeeping on retirement. Audio-thread-only.
    uint32_t* releasingRing_;
    uint32_t releasingRingCapacity_;  // power of two, >= 2 * capacity
    uint32_t releasingRingMask_;
    uint32_t releasingRingHead_;      // index of the oldest entry
    uint32_t releasingRingCount_;
    bool enableReleasingRing_ = true;
    // Hot toggle: batch the per-layer steal-victim selection of one
    // LaunchVoiceGroup transaction into a single PopStealCandidates call.
    // Mirrored from the driver's correctness-mode bool every dispatch run;
    // false restores the unchanged per-layer PopStealCandidate path.
    bool stealBatchingEnabled_ = false;

    // Dense per-channel indices make controller, sustain, pitch-bend and
    // channel termination work proportional to that channel's polyphony.
    static constexpr uint32_t kChannelIndexBlockSize = 64u;
    struct ChannelIndexBlock {
        uint32_t handles[kChannelIndexBlockSize];
        uint32_t count;
        uint32_t previous;
        uint32_t next;
    };
    uint32_t channelActiveCount_[kChannelCount];
    uint32_t channelActiveHead_[kChannelCount];
    uint32_t channelActiveTail_[kChannelCount];
    ChannelIndexBlock* channelIndexBlocks_;
    uint32_t* channelIndexFreeStack_;
    uint32_t channelIndexBlockCount_;
    uint32_t channelIndexFreeTop_;
    uint32_t* channelActiveBlock_;
    uint8_t* channelActiveOffset_;

    alignas(64) uint32_t renderClassCount_[kVoiceRenderClassCount];
    uint32_t renderClassMask_;
    static constexpr uint32_t kRenderClassBlockSize = 1024u;
    struct RenderClassBlock {
        uint32_t handles[kRenderClassBlockSize];
        uint32_t count;
        uint32_t previous;
        uint32_t next;
    };
    uint32_t renderClassHead_[kVoiceRenderClassCount];
    uint32_t renderClassTail_[kVoiceRenderClassCount];
    RenderClassBlock* renderClassBlocks_;
    uint32_t* renderClassFreeStack_;
    uint32_t renderClassBlockCount_;
    uint32_t renderClassFreeTop_;
    uint32_t* renderClassBlock_;
    uint16_t* renderClassOffset_;

    // Steal tails are rendered independently from primary render classes.
    // Keeping a dense list avoids probing all active voices in every short
    // event span when the overwhelmingly common tail count is zero.
    alignas(64) uint32_t stealTailList_[kStealTailReserve];
    alignas(64) uint32_t stealTailPosition_[kStealTailReserve];
    uint32_t stealTailCount_;
    // Tail levels are unchanged between render boundaries. Build the exact
    // quietest-tail heap once per frame, then update only its root when a
    // denser same-frame burst replaces that tail.
    uint64_t stealTailMinHeapKey_[kStealTailReserve];
    uint32_t stealTailMinHeapCount_;
    uint64_t stealTailMinHeapFrame_;
    bool stealTailMinHeapValid_;

    // Fixed-leaf tournament tree for candidates whose relative steal score is
    // time-invariant. This includes delay/hold/attack because stealing protects
    // them using their fixed target gain. Decay/release remain volatile.
    uint64_t* stealStableKey_;
    // Each node stores the complete ordered winner key. The low word encodes
    // the unique active-list position, so the root can recover its handle
    // without carrying handles through the tree and reloading both leaf keys
    // at every level.
    uint64_t* stealWinnerTree_;
    uint32_t stealTreeLeafBase_;
    uint32_t stealHeapCount_;
    bool stealHeapValid_;
    uint64_t stealHeapBuildCount_;
    uint32_t* stealVolatileList_;
    uint32_t* stealVolatilePosition_;
    uint32_t stealVolatileCount_;
    uint64_t* stealVolatileHeapKey_;
    uint32_t* stealVolatileHeapHandle_;
    uint32_t* stealVolatileHeapPosition_;
    uint32_t stealVolatileHeapCount_;
    uint64_t stealVolatileHeapFrame_;
    bool stealVolatileHeapValid_;
    RenderBackend stealKeyBackend_;
    uint8_t* stealCandidateDeferred_;
    // A deferred same-frame replacement may keep ownership of the volatile
    // heap root while its sample/envelope fields are configured.  The slot
    // and active position do not change, so CommitVoiceConfiguration can
    // update the key in place instead of remove + insert heap traversals.
    uint8_t* stealCandidateReserved_;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    enum class LaunchVictimPath : uint8_t {
        SingleInPlace,
        MatchingReuse,
        General
    };
    struct LaunchTestContext {
        bool active = false;
        bool sampled = false;
        bool allVictimsSameFrame = true;
        bool anyLayeredVictim = false;
        bool anyVolatileVictim = false;
        bool anyGeneralVictim = false;
        uint8_t incomingChannel = 0u;
        uint8_t incomingNote = 0u;
        uint32_t incomingCount = 0u;
        const VoiceConfiguration* incomingSetups = nullptr;
        uint32_t victimGroups = 0u;
        uint32_t sameFramePhysicalVictims = 0u;
        uint64_t beginCycles = 0u;
        uint64_t stageCycles[
            static_cast<uint32_t>(LaunchProfileStage::Count)]{};
    };
    uint64_t groupReuseAttemptCount_ = 0u;
    uint64_t groupReuseMatchCount_ = 0u;
    uint64_t groupReuseReservedCount_ = 0u;
    uint64_t groupReuseSmallerCount_ = 0u;
    uint64_t groupReuseLargerCount_ = 0u;
    uint64_t launchProfileCounter_ = 0u;
    uint64_t launchProfileSamples_ = 0u;
    uint64_t launchProfilePopCycles_ = 0u;
    uint64_t launchProfileTailCycles_ = 0u;
    uint64_t launchProfileLifecycleCycles_ = 0u;
    uint64_t launchProfileConfigureCycles_ = 0u;
    uint64_t launchProfileTreeCycles_ = 0u;
    bool launchChurnProfilingEnabled_ = false;
    bool volatileFallbackScanForTest_ = false;
    LaunchChurnStats launchChurnStats_{};
    LaunchTestContext launchTestContext_{};
    uint64_t launchTestTrackedFrame_ = 0u;
    int64_t launchTestProvisionalGroups_ = 0;
    int64_t launchTestProvisionalPhysicalVoices_ = 0;
    // Sampled cost of exact per-frame volatile heap key reconstruction.
    uint64_t volatileHeapProfileCounter_ = 0u;
    uint64_t volatileHeapProfileBuilds_ = 0u;
    uint64_t volatileHeapProfileSamples_ = 0u;
    uint64_t volatileHeapProfileCycles_ = 0u;
    uint64_t volatileHeapProfileCandidates_ = 0u;
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
    int32_t* playGroupNext_;
    int32_t* playGroupPrev_;
    uint32_t lastLinkedPlayIndex_;
    VoiceHandle lastLinkedPlayVoice_;
    uint32_t maxLaunchGroupSize_ = 1u;
    // Cap on the per-launch batched steal-victim selection (launch groups
    // larger than this fall back to the per-layer pop path).
    static constexpr uint32_t kStealBatchMaxLayers = 16u;
    void* metadataStorage_;
    size_t metadataBytes_;

    bool ReserveMetadata(uint32_t capacity);
    void CopyFrom(const VoiceManager& other);

    void InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity);
    void InitializePreparedVoice(VoiceHandle handle, uint8_t channel,
                                 uint8_t note, uint8_t velocity);
    void ApplyVoiceConfigurationFields(
        VoiceHandle handle, const VoiceConfiguration& setup,
        const ChannelParamsSnapshot& channelParams,
        VoiceRenderClass knownClass = VoiceRenderClass::Generic);
    static VoiceRenderClass ClassifyConfiguration(
        const VoiceConfiguration& setup);
    static bool IsStableConfiguration(const VoiceConfiguration& setup);
    void LinkChannelKey(VoiceHandle handle);
    void UnlinkChannelKey(VoiceHandle handle);
    void UnlinkPlayGroup(VoiceHandle handle);
    void LinkChannelActive(VoiceHandle handle);
    void UnlinkChannelActive(VoiceHandle handle);
    void MoveChannelActiveInPlace(VoiceHandle handle, uint8_t newChannel);
    uint32_t AllocateChannelIndexBlock();
    void FreeChannelIndexBlock(uint32_t block);
    VoiceHandle LastChannelActive(uint8_t channel) const;
    VoiceRenderClass ClassifyVoice(VoiceHandle handle) const;
    void LinkRenderClass(VoiceHandle handle);
    void UnlinkRenderClass(VoiceHandle handle);
    uint32_t AllocateRenderClassBlock();
    void FreeRenderClassBlock(uint32_t block);
    void LinkStealTail(VoiceHandle handle);
    void UnlinkStealTail(VoiceHandle handle);
    void BuildStealTailMinHeap();
    void StealTailHeapSiftDown(uint32_t position);
    void BuildStealHeap();
    SVMS_VM_FORCEINLINE void RefreshStealWinnerPath(VoiceHandle handle);
    void RefreshStealWinnerPaths(const VoiceHandle* handles, uint32_t count);
    void RebuildStableWinnerTree();
    VoiceHandle PopStealCandidate(uint32_t& activePosition,
                                  bool reserveVolatileRoot);
    // Batched per-launch steal selection support. A batch-popped victim is
    // fully removed from its tier; InsertPreselectedVictim restores exactly
    // the entry the pop erased (used for victims a launch never consumed, so
    // the post-launch index matches the per-layer path byte for byte).
    void InsertPreselectedVictim(VoiceHandle victim);
    void RearmLiveBatchVictims(const VoiceHandle* victims, uint32_t popped,
                               const bool* consumed);
    // Releasing-ring fast path: consumes (or peeks, with consume=false) the
    // oldest plausible Releasing entry, validating it against live pool
    // state; returns kInvalidVoice when the ring runs dry or every remaining
    // entry is stale.
    VoiceHandle NextValidReleasingRingVictim(uint32_t& victimPosition,
                                             bool consume);
    bool ReleasingRingEligible() const;
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
    void RemoveReservedVolatileRoot(VoiceHandle handle);
    bool IsStableStealCandidate(VoiceHandle handle) const;
    float ComputeEffectiveStealLevel(VoiceHandle handle) const;
    float ComputeTailLevel(uint32_t tailSlot) const;
    uint32_t SelectStealTailSlot(float outgoingLevel,
                                 bool& replacingHeapRoot);
    void CaptureStealTail(VoiceHandle handle);
    bool ReuseMatchingStealGroup(uint8_t channel, uint8_t note,
                                 uint8_t velocity,
                                 const VoiceConfiguration* setups,
                                 uint32_t count, VoiceHandle* outHandles,
                                 bool& candidatesReservedInPlace);
    SVMS_VM_FORCEINLINE bool TryLaunchSingleVoiceInPlace(
        uint8_t channel, uint8_t note, uint8_t velocity,
        const VoiceConfiguration& setup, uint32_t playIndex,
        const ChannelParamsSnapshot& channelParams,
        VoiceHandle& outHandle);
    void CommitVoiceGroupConfigurations(const VoiceHandle* handles,
                                        uint32_t count,
                                        bool candidatesReservedInPlace);
    void RetireStolenSibling(VoiceHandle handle,
                             VoiceHandle selectedVictim);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    void BeginLaunchTestProfile(uint8_t channel, uint8_t note,
                                const VoiceConfiguration* setups,
                                uint32_t count);
    void FinishLaunchTestProfile(bool success);
    void RecordVictimGroupForTest(VoiceHandle selected,
                                  LaunchVictimPath path,
                                  bool reservedInPlace);
    uint64_t BeginLaunchStageForTest() const;
    void EndLaunchStageForTest(LaunchProfileStage stage, uint64_t begin);
    void TrackLaunchFrameForTest(uint64_t frame);
#endif
    float ComputeStableStealKey(VoiceHandle handle) const;
    float ComputeNewbornStableStealKey(VoiceHandle handle) const;
    static SVMS_VM_FORCEINLINE uint64_t EncodeStableWinnerKey(
        float score, uint32_t activePosition);
    static bool HigherPriorityCandidate(const StealCandidate& a,
                                        const StealCandidate& b);

    // ── Score-based steal priority ─────────────────────────────────────
    // Computes BASSMIDI-like priority. HIGHER score = stolen FIRST.
    // Effective control/envelope level protects audible voices; rendered age
    // is the only independent bias. Velocity is not a separate priority.
    float ComputeStealScore(uint32_t idx) const;
    PreTailCaptureHook preTailCaptureHook_ = nullptr;
    void* preTailCaptureUserData_ = nullptr;
    VoiceConfiguredHook voiceConfiguredHook_ = nullptr;
    void* voiceConfiguredUserData_ = nullptr;
};

// ════════════════════════════════════════════════════════════════════════
// Implementation
// ════════════════════════════════════════════════════════════════════════

inline VoiceManager::VoiceManager()
    : activeCount_(0), activeList_(nullptr), activePosition_(nullptr),
      freeTop_(0), maxVoices_(0), voiceLimit_(0), sampleRate_(44100),
      stealFadeFrames_(kStealFadeFrames),
      currentFrame_(0), lastVoiceLimitEnforceFrame_(UINT64_MAX),
      freeStack_(nullptr),
      channelIndexBlocks_(nullptr), channelIndexFreeStack_(nullptr),
      channelIndexBlockCount_(0u), channelIndexFreeTop_(0u),
      channelActiveBlock_(nullptr), channelActiveOffset_(nullptr),
      renderClassMask_(0u), renderClassBlocks_(nullptr),
      renderClassFreeStack_(nullptr), renderClassBlockCount_(0u),
      renderClassFreeTop_(0u), renderClassBlock_(nullptr),
      renderClassOffset_(nullptr),
      retireCount_(0), retireImmediateCount_(0), stealCount_(0),
      stealTailCount_(0), stealTailMinHeapCount_(0),
      stealTailMinHeapFrame_(UINT64_MAX), stealTailMinHeapValid_(false),
      stealStableKey_(nullptr), stealWinnerTree_(nullptr),
      stealTreeLeafBase_(1u), stealHeapCount_(0), stealHeapValid_(false),
      stealHeapBuildCount_(0),
      stealVolatileList_(nullptr), stealVolatilePosition_(nullptr),
      stealVolatileCount_(0), stealVolatileHeapKey_(nullptr),
      stealVolatileHeapHandle_(nullptr),
      stealVolatileHeapPosition_(nullptr), stealVolatileHeapCount_(0),
      stealVolatileHeapFrame_(UINT64_MAX), stealVolatileHeapValid_(false),
      stealKeyBackend_(RenderBackend::Scalar) {
    stealCandidateDeferred_ = nullptr;
    stealCandidateReserved_ = nullptr;
    playGroupNext_ = nullptr;
    playGroupPrev_ = nullptr;
    metadataStorage_ = nullptr;
    metadataBytes_ = 0u;
    v.Reset();
    std::memset(channelActiveCount_, 0, sizeof(channelActiveCount_));
    std::memset(channelActiveHead_, 0xff, sizeof(channelActiveHead_));
    std::memset(channelActiveTail_, 0xff, sizeof(channelActiveTail_));
    std::memset(renderClassCount_, 0, sizeof(renderClassCount_));
    std::memset(renderClassHead_, 0xff, sizeof(renderClassHead_));
    std::memset(renderClassTail_, 0xff, sizeof(renderClassTail_));
    std::memset(stealTailPosition_, 0xff, sizeof(stealTailPosition_));
    lastLinkedPlayIndex_ = UINT32_MAX;
    lastLinkedPlayVoice_ = kInvalidVoice;
    for (uint32_t ch = 0; ch < kChannelCount; ++ch)
        for (uint32_t n = 0; n < kNoteCount; ++n)
            channelKeyVoiceHead_[ch][n] = channelKeyVoiceOldest_[ch][n] = -1;
}

inline VoiceManager::VoiceManager(const VoiceManager& other)
    : VoiceManager() {
    if (!Initialize(other.maxVoices_, other.sampleRate_))
        throw std::bad_alloc();
    CopyFrom(other);
}

inline VoiceManager::~VoiceManager() {
    _aligned_free(metadataStorage_);
}

inline size_t VoiceManager::EstimateAllocatedBytes(
    uint32_t capacity) noexcept {
    if (capacity == 0u || capacity > kMaxPolyphony) return 0u;
    uint32_t treeLeaves = 1u;
    while (treeLeaves < capacity) treeLeaves <<= 1u;
    const uint32_t channelBlocks =
        (capacity + kChannelIndexBlockSize - 1u) /
            kChannelIndexBlockSize + kChannelCount;
    const uint32_t renderBlocks =
        (capacity + kRenderClassBlockSize - 1u) /
            kRenderClassBlockSize + kVoiceRenderClassCount;
    size_t metadataBytes = 0u;
    auto add = [&](size_t elementSize, size_t count) {
        metadataBytes = (metadataBytes + kMixBufferAlign - 1u) &
                ~(static_cast<size_t>(kMixBufferAlign) - 1u);
        if (count > ((std::numeric_limits<size_t>::max)() - metadataBytes) /
                        elementSize) {
            metadataBytes = (std::numeric_limits<size_t>::max)();
            return;
        }
        metadataBytes += elementSize * count;
    };
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(int32_t), capacity);
    {
        uint32_t ringCapacity = 2u;
        while (ringCapacity < static_cast<uint32_t>(capacity) * 2u)
            ringCapacity <<= 1u;
        add(sizeof(uint32_t), ringCapacity); // releasing ring
    }
    add(sizeof(ChannelIndexBlock), channelBlocks);
    add(sizeof(uint32_t), channelBlocks);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint8_t), capacity);
    add(sizeof(RenderClassBlock), renderBlocks);
    add(sizeof(uint32_t), renderBlocks);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint16_t), capacity);
    add(sizeof(uint64_t), capacity);
    add(sizeof(uint64_t), static_cast<size_t>(treeLeaves) * 2u);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint64_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint32_t), capacity);
    add(sizeof(uint8_t), capacity);
    add(sizeof(uint8_t), capacity);
    add(sizeof(int32_t), capacity);
    add(sizeof(int32_t), capacity);
    if (metadataBytes == (std::numeric_limits<size_t>::max)())
        return metadataBytes;
    const size_t voiceBytes = VoiceSoA::EstimateStorageBytes(capacity);
    if (voiceBytes > (std::numeric_limits<size_t>::max)() -
                         sizeof(VoiceManager) - metadataBytes)
        return (std::numeric_limits<size_t>::max)();
    return sizeof(VoiceManager) + voiceBytes + metadataBytes;
}

inline bool VoiceManager::ReserveMetadata(uint32_t capacity) {
    if (capacity == 0u || capacity > kMaxPolyphony) return false;
    uint32_t treeLeaves = 1u;
    while (treeLeaves < capacity) treeLeaves <<= 1u;
    const uint32_t channelBlocks =
        (capacity + kChannelIndexBlockSize - 1u) /
            kChannelIndexBlockSize + kChannelCount;
    const uint32_t renderBlocks =
        (capacity + kRenderClassBlockSize - 1u) /
            kRenderClassBlockSize + kVoiceRenderClassCount;
    uint32_t ringCapacity = 2u;
    while (ringCapacity < static_cast<uint32_t>(capacity) * 2u)
        ringCapacity <<= 1u;

    size_t bytes = 0u;
    auto add = [&](size_t elementSize, size_t count) {
        bytes = (bytes + kMixBufferAlign - 1u) &
                ~(static_cast<size_t>(kMixBufferAlign) - 1u);
        if (count > ((std::numeric_limits<size_t>::max)() - bytes) /
                        elementSize) {
            bytes = (std::numeric_limits<size_t>::max)();
            return;
        }
        bytes += elementSize * count;
    };
    add(sizeof(uint32_t), capacity); // activeList
    add(sizeof(uint32_t), capacity); // activePosition
    add(sizeof(int32_t), capacity);  // freeStack
    add(sizeof(uint32_t), ringCapacity); // releasing ring
    add(sizeof(ChannelIndexBlock), channelBlocks);
    add(sizeof(uint32_t), channelBlocks);
    add(sizeof(uint32_t), capacity); // channelActiveBlock
    add(sizeof(uint8_t), capacity);  // channelActiveOffset
    add(sizeof(RenderClassBlock), renderBlocks);
    add(sizeof(uint32_t), renderBlocks);
    add(sizeof(uint32_t), capacity); // renderClassBlock
    add(sizeof(uint16_t), capacity); // renderClassOffset
    add(sizeof(uint64_t), capacity); // stealStableKey
    add(sizeof(uint64_t), static_cast<size_t>(treeLeaves) * 2u);
    add(sizeof(uint32_t), capacity); // volatile list
    add(sizeof(uint32_t), capacity); // volatile position
    add(sizeof(uint64_t), capacity); // volatile heap ordered key
    add(sizeof(uint32_t), capacity); // volatile heap handle
    add(sizeof(uint32_t), capacity); // volatile heap position
    add(sizeof(uint8_t), capacity);  // deferred
    add(sizeof(uint8_t), capacity);  // reserved
    add(sizeof(int32_t), capacity);  // play next
    add(sizeof(int32_t), capacity);  // play previous
    if (bytes == (std::numeric_limits<size_t>::max)()) return false;

    void* allocation = _aligned_malloc(bytes, kMixBufferAlign);
    if (!allocation) return false;
    _aligned_free(metadataStorage_);
    metadataStorage_ = allocation;
    metadataBytes_ = bytes;
    channelIndexBlockCount_ = channelBlocks;
    renderClassBlockCount_ = renderBlocks;
    stealTreeLeafBase_ = treeLeaves;
    releasingRingCapacity_ = ringCapacity;
    releasingRingMask_ = ringCapacity - 1u;
    releasingRingHead_ = 0u;
    releasingRingCount_ = 0u;

    size_t offset = 0u;
    uint8_t* base = static_cast<uint8_t*>(metadataStorage_);
    auto take = [&](size_t elementSize, size_t count) -> void* {
        offset = (offset + kMixBufferAlign - 1u) &
                 ~(static_cast<size_t>(kMixBufferAlign) - 1u);
        void* result = base + offset;
        offset += elementSize * count;
        return result;
    };
    activeList_ = static_cast<uint32_t*>(take(sizeof(uint32_t), capacity));
    activePosition_ = static_cast<uint32_t*>(take(sizeof(uint32_t), capacity));
    freeStack_ = static_cast<int32_t*>(take(sizeof(int32_t), capacity));
    releasingRing_ = static_cast<uint32_t*>(take(sizeof(uint32_t),
                                                 releasingRingCapacity_));
    channelIndexBlocks_ = static_cast<ChannelIndexBlock*>(
        take(sizeof(ChannelIndexBlock), channelBlocks));
    channelIndexFreeStack_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), channelBlocks));
    channelActiveBlock_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    channelActiveOffset_ = static_cast<uint8_t*>(
        take(sizeof(uint8_t), capacity));
    renderClassBlocks_ = static_cast<RenderClassBlock*>(
        take(sizeof(RenderClassBlock), renderBlocks));
    renderClassFreeStack_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), renderBlocks));
    renderClassBlock_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    renderClassOffset_ = static_cast<uint16_t*>(
        take(sizeof(uint16_t), capacity));
    stealStableKey_ = static_cast<uint64_t*>(
        take(sizeof(uint64_t), capacity));
    stealWinnerTree_ = static_cast<uint64_t*>(take(
        sizeof(uint64_t), static_cast<size_t>(treeLeaves) * 2u));
    stealVolatileList_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    stealVolatilePosition_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    stealVolatileHeapKey_ = static_cast<uint64_t*>(
        take(sizeof(uint64_t), capacity));
    stealVolatileHeapHandle_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    stealVolatileHeapPosition_ = static_cast<uint32_t*>(
        take(sizeof(uint32_t), capacity));
    stealCandidateDeferred_ = static_cast<uint8_t*>(
        take(sizeof(uint8_t), capacity));
    stealCandidateReserved_ = static_cast<uint8_t*>(
        take(sizeof(uint8_t), capacity));
    playGroupNext_ = static_cast<int32_t*>(
        take(sizeof(int32_t), capacity));
    playGroupPrev_ = static_cast<int32_t*>(
        take(sizeof(int32_t), capacity));
    return offset <= metadataBytes_;
}

inline void VoiceManager::CopyFrom(const VoiceManager& other) {
    v = other.v;
    std::memcpy(metadataStorage_, other.metadataStorage_, metadataBytes_);
    activeCount_ = other.activeCount_;
    freeTop_ = other.freeTop_;
    retireCount_ = other.retireCount_;
    retireImmediateCount_ = other.retireImmediateCount_;
    stealCount_ = other.stealCount_;
    voiceLimit_ = other.voiceLimit_;
    stealFadeFrames_ = other.stealFadeFrames_;
    currentFrame_ = other.currentFrame_;
    lastVoiceLimitEnforceFrame_ = other.lastVoiceLimitEnforceFrame_;
    std::memcpy(channelActiveCount_, other.channelActiveCount_,
                sizeof(channelActiveCount_));
    std::memcpy(channelActiveHead_, other.channelActiveHead_,
                sizeof(channelActiveHead_));
    std::memcpy(channelActiveTail_, other.channelActiveTail_,
                sizeof(channelActiveTail_));
    channelIndexFreeTop_ = other.channelIndexFreeTop_;
    std::memcpy(renderClassCount_, other.renderClassCount_,
                sizeof(renderClassCount_));
    renderClassMask_ = other.renderClassMask_;
    std::memcpy(renderClassHead_, other.renderClassHead_,
                sizeof(renderClassHead_));
    std::memcpy(renderClassTail_, other.renderClassTail_,
                sizeof(renderClassTail_));
    renderClassFreeTop_ = other.renderClassFreeTop_;
    std::memcpy(stealTailList_, other.stealTailList_, sizeof(stealTailList_));
    std::memcpy(stealTailPosition_, other.stealTailPosition_,
                sizeof(stealTailPosition_));
    stealTailCount_ = other.stealTailCount_;
    std::memcpy(stealTailMinHeapKey_, other.stealTailMinHeapKey_,
                sizeof(stealTailMinHeapKey_));
    stealTailMinHeapCount_ = other.stealTailMinHeapCount_;
    stealTailMinHeapFrame_ = other.stealTailMinHeapFrame_;
    stealTailMinHeapValid_ = other.stealTailMinHeapValid_;
    stealHeapCount_ = other.stealHeapCount_;
    stealHeapValid_ = other.stealHeapValid_;
    stealHeapBuildCount_ = other.stealHeapBuildCount_;
    stealVolatileCount_ = other.stealVolatileCount_;
    stealVolatileHeapCount_ = other.stealVolatileHeapCount_;
    stealVolatileHeapFrame_ = other.stealVolatileHeapFrame_;
    stealVolatileHeapValid_ = other.stealVolatileHeapValid_;
    stealKeyBackend_ = other.stealKeyBackend_;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    groupReuseAttemptCount_ = other.groupReuseAttemptCount_;
    groupReuseMatchCount_ = other.groupReuseMatchCount_;
    groupReuseReservedCount_ = other.groupReuseReservedCount_;
    groupReuseSmallerCount_ = other.groupReuseSmallerCount_;
    groupReuseLargerCount_ = other.groupReuseLargerCount_;
    launchProfileCounter_ = other.launchProfileCounter_;
    launchProfileSamples_ = other.launchProfileSamples_;
    launchProfilePopCycles_ = other.launchProfilePopCycles_;
    launchProfileTailCycles_ = other.launchProfileTailCycles_;
    launchProfileLifecycleCycles_ = other.launchProfileLifecycleCycles_;
    launchProfileConfigureCycles_ = other.launchProfileConfigureCycles_;
    launchProfileTreeCycles_ = other.launchProfileTreeCycles_;
    launchChurnProfilingEnabled_ = other.launchChurnProfilingEnabled_;
    volatileFallbackScanForTest_ = other.volatileFallbackScanForTest_;
    launchChurnStats_ = other.launchChurnStats_;
    launchTestContext_ = LaunchTestContext{};
    launchTestTrackedFrame_ = other.launchTestTrackedFrame_;
    launchTestProvisionalGroups_ = other.launchTestProvisionalGroups_;
    launchTestProvisionalPhysicalVoices_ =
        other.launchTestProvisionalPhysicalVoices_;
#endif
    std::memcpy(channelKeyVoiceHead_, other.channelKeyVoiceHead_,
                sizeof(channelKeyVoiceHead_));
    std::memcpy(channelKeyVoiceOldest_, other.channelKeyVoiceOldest_,
                sizeof(channelKeyVoiceOldest_));
    lastLinkedPlayIndex_ = other.lastLinkedPlayIndex_;
    lastLinkedPlayVoice_ = other.lastLinkedPlayVoice_;
    maxLaunchGroupSize_ = other.maxLaunchGroupSize_;
}

inline bool VoiceManager::Initialize(uint32_t maxVoices, uint32_t sampleRate) {
    const uint32_t requested = maxVoices < kMaxPolyphony
        ? maxVoices : kMaxPolyphony;
    if (requested == 0u || !v.Reserve(requested) ||
        !ReserveMetadata(requested)) {
        maxVoices_ = 0u;
        voiceLimit_ = 0u;
        return false;
    }
    maxVoices_ = requested;
    voiceLimit_ = requested;
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100;
    stealFadeFrames_ = kStealFadeFrames;
    Reset();
    return true;
}

inline void VoiceManager::Reset() {
    v.Reset();
    std::memset(activeList_, 0, sizeof(*activeList_) * maxVoices_);
    std::memset(activePosition_, 0xff,
                sizeof(*activePosition_) * maxVoices_);
    std::memset(channelActiveCount_, 0, sizeof(channelActiveCount_));
    std::memset(channelActiveHead_, 0xff, sizeof(channelActiveHead_));
    std::memset(channelActiveTail_, 0xff, sizeof(channelActiveTail_));
    std::memset(channelActiveBlock_, 0xff,
                sizeof(*channelActiveBlock_) * maxVoices_);
    std::memset(channelActiveOffset_, 0xff,
                sizeof(*channelActiveOffset_) * maxVoices_);
    channelIndexFreeTop_ = channelIndexBlockCount_;
    for (uint32_t block = 0u; block < channelIndexBlockCount_; ++block) {
        channelIndexFreeStack_[block] = channelIndexBlockCount_ - 1u - block;
        channelIndexBlocks_[block].count = 0u;
        channelIndexBlocks_[block].previous = UINT32_MAX;
        channelIndexBlocks_[block].next = UINT32_MAX;
    }
    std::memset(renderClassCount_, 0, sizeof(renderClassCount_));
    renderClassMask_ = 0u;
    std::memset(renderClassHead_, 0xff, sizeof(renderClassHead_));
    std::memset(renderClassTail_, 0xff, sizeof(renderClassTail_));
    std::memset(renderClassBlock_, 0xff,
                sizeof(*renderClassBlock_) * maxVoices_);
    std::memset(renderClassOffset_, 0xff,
                sizeof(*renderClassOffset_) * maxVoices_);
    renderClassFreeTop_ = renderClassBlockCount_;
    for (uint32_t block = 0u; block < renderClassBlockCount_; ++block) {
        renderClassFreeStack_[block] = renderClassBlockCount_ - 1u - block;
        renderClassBlocks_[block].count = 0u;
        renderClassBlocks_[block].previous = UINT32_MAX;
        renderClassBlocks_[block].next = UINT32_MAX;
    }
    std::memset(stealTailPosition_, 0xff, sizeof(stealTailPosition_));
    std::memset(stealWinnerTree_, 0,
                sizeof(*stealWinnerTree_) * stealTreeLeafBase_ * 2u);
    std::memset(stealStableKey_, 0,
                sizeof(*stealStableKey_) * maxVoices_);
    std::memset(stealVolatilePosition_, 0xff,
                sizeof(*stealVolatilePosition_) * maxVoices_);
    std::memset(stealVolatileHeapPosition_, 0xff,
                sizeof(*stealVolatileHeapPosition_) * maxVoices_);
    std::memset(stealCandidateDeferred_, 0,
                sizeof(*stealCandidateDeferred_) * maxVoices_);
    std::memset(stealCandidateReserved_, 0,
                sizeof(*stealCandidateReserved_) * maxVoices_);
    std::memset(playGroupNext_, 0xff,
                sizeof(*playGroupNext_) * maxVoices_);
    std::memset(playGroupPrev_, 0xff,
                sizeof(*playGroupPrev_) * maxVoices_);
    activeCount_ = 0;
    currentFrame_ = 0;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    launchChurnStats_ = LaunchChurnStats{};
    launchTestContext_ = LaunchTestContext{};
    launchTestTrackedFrame_ = 0u;
    launchTestProvisionalGroups_ = 0;
    launchTestProvisionalPhysicalVoices_ = 0;
#endif
    lastVoiceLimitEnforceFrame_ = UINT64_MAX;
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
    maxLaunchGroupSize_ = 1u;
    freeTop_ = maxVoices_;
    releasingCount_.store(0u, std::memory_order_relaxed);
    releasingRingHead_ = 0u;
    releasingRingCount_ = 0u;
    releasingRingHits_ = 0u;
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

inline bool VoiceManager::GrowCapacity(uint32_t capacity) {
    if (capacity <= maxVoices_) return capacity != 0u;
    if (capacity == 0u || capacity > kMaxPolyphony) return false;

    const uint32_t oldCapacity = maxVoices_;
    VoiceManager grown;
    if (!grown.Initialize(capacity, sampleRate_)) return false;

    // Preserve every render-visible per-voice field. Handles are stable: old
    // voices keep the same array index, while the newly added range remains
    // Free from grown.Initialize().
#define SVMS_COPY_GROWN_VOICE_FIELD(name) \
    std::memcpy(grown.v.name, v.name, \
                static_cast<size_t>(oldCapacity) * sizeof(*v.name))
    SVMS_COPY_GROWN_VOICE_FIELD(channel);
    SVMS_COPY_GROWN_VOICE_FIELD(note);
    SVMS_COPY_GROWN_VOICE_FIELD(velocity);
    SVMS_COPY_GROWN_VOICE_FIELD(state);
    SVMS_COPY_GROWN_VOICE_FIELD(envelopeStage);
    SVMS_COPY_GROWN_VOICE_FIELD(sampleBacked);
    SVMS_COPY_GROWN_VOICE_FIELD(renderClass);
    SVMS_COPY_GROWN_VOICE_FIELD(presetIndex);
    SVMS_COPY_GROWN_VOICE_FIELD(regionIndex);
    SVMS_COPY_GROWN_VOICE_FIELD(playIndex);
    SVMS_COPY_GROWN_VOICE_FIELD(phases);
    SVMS_COPY_GROWN_VOICE_FIELD(phaseIncs);
    SVMS_COPY_GROWN_VOICE_FIELD(basePhaseIncs);
    SVMS_COPY_GROWN_VOICE_FIELD(pitchBendScales);
    SVMS_COPY_GROWN_VOICE_FIELD(currentGain);
    SVMS_COPY_GROWN_VOICE_FIELD(targetGain);
    SVMS_COPY_GROWN_VOICE_FIELD(sustainLevel);
    SVMS_COPY_GROWN_VOICE_FIELD(attackGainStep);
    SVMS_COPY_GROWN_VOICE_FIELD(releaseDecay);
    SVMS_COPY_GROWN_VOICE_FIELD(gainLeft);
    SVMS_COPY_GROWN_VOICE_FIELD(gainRight);
    SVMS_COPY_GROWN_VOICE_FIELD(mixGainL);
    SVMS_COPY_GROWN_VOICE_FIELD(mixGainR);
    SVMS_COPY_GROWN_VOICE_FIELD(renderGainL);
    SVMS_COPY_GROWN_VOICE_FIELD(renderGainR);
    SVMS_COPY_GROWN_VOICE_FIELD(stealOutputGain);
    SVMS_COPY_GROWN_VOICE_FIELD(vibLfoToPitchCents);
    SVMS_COPY_GROWN_VOICE_FIELD(vibLfoSteps);
    SVMS_COPY_GROWN_VOICE_FIELD(vibLfoPhases);
    SVMS_COPY_GROWN_VOICE_FIELD(vibLfoDelays);
    SVMS_COPY_GROWN_VOICE_FIELD(vibLfoModulated);
    SVMS_COPY_GROWN_VOICE_FIELD(sampleStart);
    SVMS_COPY_GROWN_VOICE_FIELD(loopMode);
    SVMS_COPY_GROWN_VOICE_FIELD(loopEnabled);
    SVMS_COPY_GROWN_VOICE_FIELD(relEnd);
    SVMS_COPY_GROWN_VOICE_FIELD(relLoopS);
    SVMS_COPY_GROWN_VOICE_FIELD(relLoopE);
    SVMS_COPY_GROWN_VOICE_FIELD(relLoopSF);
    SVMS_COPY_GROWN_VOICE_FIELD(relLoopEF);
    SVMS_COPY_GROWN_VOICE_FIELD(holdSamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(attackSamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(decaySamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(delaySamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(releaseSamplesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(decaySlope);
    SVMS_COPY_GROWN_VOICE_FIELD(heldBySustain);
    SVMS_COPY_GROWN_VOICE_FIELD(heldBySostenuto);
    SVMS_COPY_GROWN_VOICE_FIELD(releaseStartInBlock);
    SVMS_COPY_GROWN_VOICE_FIELD(nextChannelKeyVoice);
    SVMS_COPY_GROWN_VOICE_FIELD(prevChannelKeyVoice);
    SVMS_COPY_GROWN_VOICE_FIELD(birthFrame);
    SVMS_COPY_GROWN_VOICE_FIELD(stealFadeInFramesRemaining);
    SVMS_COPY_GROWN_VOICE_FIELD(stealFadeInFramesTotal);
#undef SVMS_COPY_GROWN_VOICE_FIELD

#define SVMS_COPY_GROWN_TAIL_FIELD(name) \
    std::memcpy(grown.v.name, v.name, sizeof(v.name))
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailPhase);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailPhaseInc);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailGain);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailMixGainL);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailMixGainR);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailSampleStart);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelEnd);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelLoopS);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelLoopE);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelLoopSF);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRelLoopEF);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailFramesRemaining);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailFramesTotal);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailSampleBacked);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailLoopEnabled);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailChannel);
    SVMS_COPY_GROWN_TAIL_FIELD(stealTailRot);
#undef SVMS_COPY_GROWN_TAIL_FIELD
    grown.v.pad16 = v.pad16;

    // Preserve per-voice rotation state across the growth (handles are
    // stable, so a straight prefix copy is exact).
    grown.phaseRotationMode_ = phaseRotationMode_;
    grown.rotationSeedCounter_ = rotationSeedCounter_;
    if (phaseRotationMode_ != 0u && v.rot &&
        grown.v.ReserveRotation(oldCapacity)) {
        std::memcpy(grown.v.rot, v.rot,
                    static_cast<size_t>(oldCapacity) *
                        sizeof(VoiceRotationState));
    }

    grown.activeCount_ = activeCount_;
    if (activeCount_ != 0u)
        std::memcpy(grown.activeList_, activeList_,
                    static_cast<size_t>(activeCount_) * sizeof(*activeList_));
    std::memset(grown.activePosition_, 0xff,
                static_cast<size_t>(capacity) * sizeof(*grown.activePosition_));
    for (uint32_t position = 0u; position < grown.activeCount_; ++position)
        grown.activePosition_[grown.activeList_[position]] = position;

    // Rebuild the free stack from copied voice state. The order of free slots
    // is not semantic; rebuilding also naturally includes the newly grown
    // handle range without touching any active handle.
    grown.freeTop_ = 0u;
    for (uint32_t handle = 0u; handle < capacity; ++handle) {
        if (grown.v.state[handle] == static_cast<uint8_t>(VoiceState::Free))
            grown.freeStack_[grown.freeTop_++] = static_cast<int32_t>(handle);
    }

    std::memcpy(grown.channelKeyVoiceHead_, channelKeyVoiceHead_,
                sizeof(channelKeyVoiceHead_));
    std::memcpy(grown.channelKeyVoiceOldest_, channelKeyVoiceOldest_,
                sizeof(channelKeyVoiceOldest_));
    std::memcpy(grown.playGroupNext_, playGroupNext_,
                static_cast<size_t>(oldCapacity) * sizeof(*playGroupNext_));
    std::memcpy(grown.playGroupPrev_, playGroupPrev_,
                static_cast<size_t>(oldCapacity) * sizeof(*playGroupPrev_));
    grown.lastLinkedPlayIndex_ = lastLinkedPlayIndex_;
    grown.lastLinkedPlayVoice_ = lastLinkedPlayVoice_;
    grown.maxLaunchGroupSize_ = maxLaunchGroupSize_;

    // The dense channel/render indices depend on allocation capacity, so
    // rebuild them from the stable active list instead of copying pages whose
    // geometry changed. Steal candidates are rebuilt lazily on the next steal.
    for (uint32_t position = 0u; position < grown.activeCount_; ++position) {
        const VoiceHandle handle = static_cast<VoiceHandle>(grown.activeList_[position]);
        grown.LinkChannelActive(handle);
        grown.LinkRenderClass(handle);
    }
    grown.stealHeapCount_ = 0u;
    grown.stealHeapValid_ = false;
    grown.stealVolatileCount_ = 0u;
    grown.stealVolatileHeapCount_ = 0u;
    grown.stealVolatileHeapFrame_ = UINT64_MAX;
    grown.stealVolatileHeapValid_ = false;
    grown.stealKeyBackend_ = stealKeyBackend_;
    std::memset(grown.stealWinnerTree_, 0,
                sizeof(*grown.stealWinnerTree_) * grown.stealTreeLeafBase_ * 2u);
    std::memset(grown.stealStableKey_, 0,
                static_cast<size_t>(capacity) * sizeof(*grown.stealStableKey_));
    std::memset(grown.stealVolatilePosition_, 0xff,
                static_cast<size_t>(capacity) * sizeof(*grown.stealVolatilePosition_));
    std::memset(grown.stealVolatileHeapPosition_, 0xff,
                static_cast<size_t>(capacity) * sizeof(*grown.stealVolatileHeapPosition_));
    std::memset(grown.stealCandidateDeferred_, 0,
                static_cast<size_t>(capacity) * sizeof(*grown.stealCandidateDeferred_));
    std::memset(grown.stealCandidateReserved_, 0,
                static_cast<size_t>(capacity) * sizeof(*grown.stealCandidateReserved_));

    std::memcpy(grown.stealTailList_, stealTailList_, sizeof(stealTailList_));
    std::memcpy(grown.stealTailPosition_, stealTailPosition_,
                sizeof(stealTailPosition_));
    grown.stealTailCount_ = stealTailCount_;
    grown.stealTailMinHeapCount_ = 0u;
    grown.stealTailMinHeapFrame_ = UINT64_MAX;
    grown.stealTailMinHeapValid_ = false;

    grown.retireCount_ = retireCount_;
    grown.retireImmediateCount_ = retireImmediateCount_;
    grown.stealCount_ = stealCount_;
    grown.releasingCount_.store(GetReleasingCount(), std::memory_order_relaxed);
    grown.releasingRingHits_ = releasingRingHits_;
    grown.releasingRingHead_ = 0u;
    grown.releasingRingCount_ = releasingRingCount_;
    for (uint32_t i = 0u; i < releasingRingCount_; ++i)
        grown.releasingRing_[i & grown.releasingRingMask_] =
            releasingRing_[(releasingRingHead_ + i) & releasingRingMask_];
    grown.voiceLimit_ = voiceLimit_;
    grown.currentFrame_ = currentFrame_;
    grown.lastVoiceLimitEnforceFrame_ = lastVoiceLimitEnforceFrame_;
    grown.stealFadeFrames_ = stealFadeFrames_;
    grown.stealHeapBuildCount_ = stealHeapBuildCount_;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    grown.groupReuseAttemptCount_ = groupReuseAttemptCount_;
    grown.groupReuseMatchCount_ = groupReuseMatchCount_;
    grown.groupReuseReservedCount_ = groupReuseReservedCount_;
    grown.groupReuseSmallerCount_ = groupReuseSmallerCount_;
    grown.groupReuseLargerCount_ = groupReuseLargerCount_;
    grown.launchProfileCounter_ = launchProfileCounter_;
    grown.launchProfileSamples_ = launchProfileSamples_;
    grown.launchProfilePopCycles_ = launchProfilePopCycles_;
    grown.launchProfileTailCycles_ = launchProfileTailCycles_;
    grown.launchProfileLifecycleCycles_ = launchProfileLifecycleCycles_;
    grown.launchProfileConfigureCycles_ = launchProfileConfigureCycles_;
    grown.launchProfileTreeCycles_ = launchProfileTreeCycles_;
    grown.launchChurnProfilingEnabled_ = launchChurnProfilingEnabled_;
    grown.volatileFallbackScanForTest_ = volatileFallbackScanForTest_;
    grown.launchChurnStats_ = launchChurnStats_;
    grown.launchTestContext_ = LaunchTestContext{};
    grown.launchTestTrackedFrame_ = launchTestTrackedFrame_;
    grown.launchTestProvisionalGroups_ = launchTestProvisionalGroups_;
    grown.launchTestProvisionalPhysicalVoices_ =
        launchTestProvisionalPhysicalVoices_;
#endif

    // The staging manager now owns a complete larger representation. Move
    // only its owning allocations/pointers into this instance; the old
    // metadata and VoiceSoA storage are freed after the swap, while all fixed
    // counters and indices are copied below. No live voice is re-launched.
    void* oldMetadataStorage = metadataStorage_;
    VoiceSoA oldVoices = std::move(v);
    v = std::move(grown.v);

    activeList_ = grown.activeList_;
    activePosition_ = grown.activePosition_;
    freeStack_ = grown.freeStack_;
    releasingRing_ = grown.releasingRing_;
    channelIndexBlocks_ = grown.channelIndexBlocks_;
    channelIndexFreeStack_ = grown.channelIndexFreeStack_;
    channelActiveBlock_ = grown.channelActiveBlock_;
    channelActiveOffset_ = grown.channelActiveOffset_;
    renderClassBlocks_ = grown.renderClassBlocks_;
    renderClassFreeStack_ = grown.renderClassFreeStack_;
    renderClassBlock_ = grown.renderClassBlock_;
    renderClassOffset_ = grown.renderClassOffset_;
    stealStableKey_ = grown.stealStableKey_;
    stealWinnerTree_ = grown.stealWinnerTree_;
    stealVolatileList_ = grown.stealVolatileList_;
    stealVolatilePosition_ = grown.stealVolatilePosition_;
    stealVolatileHeapKey_ = grown.stealVolatileHeapKey_;
    stealVolatileHeapHandle_ = grown.stealVolatileHeapHandle_;
    stealVolatileHeapPosition_ = grown.stealVolatileHeapPosition_;
    stealCandidateDeferred_ = grown.stealCandidateDeferred_;
    stealCandidateReserved_ = grown.stealCandidateReserved_;
    playGroupNext_ = grown.playGroupNext_;
    playGroupPrev_ = grown.playGroupPrev_;
    metadataStorage_ = grown.metadataStorage_;
    metadataBytes_ = grown.metadataBytes_;
    grown.metadataStorage_ = nullptr;
    grown.metadataBytes_ = 0u;

    maxVoices_ = capacity;
    voiceLimit_ = grown.voiceLimit_;
    activeCount_ = grown.activeCount_;
    freeTop_ = grown.freeTop_;
    channelIndexBlockCount_ = grown.channelIndexBlockCount_;
    channelIndexFreeTop_ = grown.channelIndexFreeTop_;
    std::memcpy(channelActiveCount_, grown.channelActiveCount_,
                sizeof(channelActiveCount_));
    std::memcpy(channelActiveHead_, grown.channelActiveHead_,
                sizeof(channelActiveHead_));
    std::memcpy(channelActiveTail_, grown.channelActiveTail_,
                sizeof(channelActiveTail_));
    renderClassBlockCount_ = grown.renderClassBlockCount_;
    renderClassFreeTop_ = grown.renderClassFreeTop_;
    renderClassMask_ = grown.renderClassMask_;
    std::memcpy(renderClassCount_, grown.renderClassCount_,
                sizeof(renderClassCount_));
    std::memcpy(renderClassHead_, grown.renderClassHead_,
                sizeof(renderClassHead_));
    std::memcpy(renderClassTail_, grown.renderClassTail_,
                sizeof(renderClassTail_));
    stealTreeLeafBase_ = grown.stealTreeLeafBase_;
    stealHeapCount_ = grown.stealHeapCount_;
    stealHeapValid_ = grown.stealHeapValid_;
    stealVolatileCount_ = grown.stealVolatileCount_;
    stealVolatileHeapCount_ = grown.stealVolatileHeapCount_;
    stealVolatileHeapFrame_ = grown.stealVolatileHeapFrame_;
    stealVolatileHeapValid_ = grown.stealVolatileHeapValid_;
    stealKeyBackend_ = grown.stealKeyBackend_;
    std::memcpy(stealTailList_, grown.stealTailList_, sizeof(stealTailList_));
    std::memcpy(stealTailPosition_, grown.stealTailPosition_,
                sizeof(stealTailPosition_));
    stealTailCount_ = grown.stealTailCount_;
    stealTailMinHeapCount_ = grown.stealTailMinHeapCount_;
    stealTailMinHeapFrame_ = grown.stealTailMinHeapFrame_;
    stealTailMinHeapValid_ = grown.stealTailMinHeapValid_;
    lastLinkedPlayIndex_ = grown.lastLinkedPlayIndex_;
    lastLinkedPlayVoice_ = grown.lastLinkedPlayVoice_;
    currentFrame_ = grown.currentFrame_;
    lastVoiceLimitEnforceFrame_ = grown.lastVoiceLimitEnforceFrame_;
    retireCount_ = grown.retireCount_;
    retireImmediateCount_ = grown.retireImmediateCount_;
    stealCount_ = grown.stealCount_;
    releasingCount_.store(grown.GetReleasingCount(), std::memory_order_relaxed);
    releasingRingHits_ = grown.releasingRingHits_;
    releasingRingHead_ = 0u;
    releasingRingCount_ = grown.releasingRingCount_;
    releasingRingCapacity_ = grown.releasingRingCapacity_;
    releasingRingMask_ = grown.releasingRingMask_;
    for (uint32_t i = 0u; i < grown.releasingRingCount_; ++i)
        releasingRing_[i & releasingRingMask_] =
            grown.releasingRing_[i & grown.releasingRingMask_];

    _aligned_free(oldMetadataStorage);
    PublishRuntimeVoicePoolCapacity(capacity);
    return true;
}

inline bool VoiceManager::SetVoiceLimit(uint32_t limit) {
    if (limit == 0u || limit > maxVoices_) return false;
    voiceLimit_ = limit;
    PublishAppliedRuntimeVoiceLimit(limit);
    return true;
}

inline uint32_t VoiceManager::EnforceVoiceLimit(uint32_t maxReleases,
                                                 float releaseSeconds) {
    if (voiceLimit_ == 0u || maxReleases == 0u) return 0u;
    const uint32_t releasing = GetReleasingCount();
    const uint32_t primaryVoices = activeCount_ > releasing
        ? activeCount_ - releasing : 0u;
    if (primaryVoices <= voiceLimit_) return 0u;

    uint32_t remaining = primaryVoices - voiceLimit_;
    remaining = (std::min)(remaining, maxReleases);
    const float seconds = (std::max)(0.005f, (std::min)(0.250f, releaseSeconds));
    const float releaseDecay = MakeReleaseDecay(seconds, sampleRate_);
    const uint32_t releaseSamples = MakeReleaseSamples(seconds, sampleRate_);

    uint32_t released = 0u;
    while (released < remaining) {
        uint32_t position = 0u;
        const VoiceHandle victim = PopStealCandidate(position, false);
        if (victim == kInvalidVoice) break;
        if (v.state[victim] != static_cast<uint8_t>(VoiceState::Active))
            continue;

        v.heldBySustain[victim] = 0u;
        v.heldBySostenuto[victim] = 0u;
        v.releaseStartInBlock[victim] = 0u;
        v.releaseDecay[victim] = releaseDecay;
        v.releaseSamplesRemaining[victim] = releaseSamples;
        // PopStealCandidate already removed this victim.  Suppress the
        // release reclassification's candidate update so a large live cap
        // reduction does not remove, reinsert, then remove every victim.
        stealCandidateDeferred_[victim] = 1u;
        StartRelease(victim);
        stealCandidateDeferred_[victim] = 0u;
        ++released;
    }

    stealHeapValid_ = false;
    stealVolatileHeapValid_ = false;
    return released;
}

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
inline void VoiceManager::TrackLaunchFrameForTest(uint64_t frame) {
    if (!launchChurnProfilingEnabled_) return;
    if (frame == launchTestTrackedFrame_) return;
    if (launchTestProvisionalGroups_ > 0) {
        launchChurnStats_.nextFrameSurvivingGroups +=
            static_cast<uint64_t>(launchTestProvisionalGroups_);
    }
    if (launchTestProvisionalPhysicalVoices_ > 0) {
        launchChurnStats_.nextFrameSurvivingPhysicalVoices +=
            static_cast<uint64_t>(launchTestProvisionalPhysicalVoices_);
    }
    launchTestTrackedFrame_ = frame;
    launchTestProvisionalGroups_ = 0;
    launchTestProvisionalPhysicalVoices_ = 0;
}

inline uint64_t VoiceManager::BeginLaunchStageForTest() const {
#if defined(_MSC_VER)
    return launchTestContext_.active && launchTestContext_.sampled
        ? __rdtsc() : 0u;
#else
    return 0u;
#endif
}

inline void VoiceManager::EndLaunchStageForTest(LaunchProfileStage stage,
                                                 uint64_t begin) {
#if defined(_MSC_VER)
    if (begin == 0u || !launchTestContext_.active ||
        !launchTestContext_.sampled) return;
    const uint32_t index = static_cast<uint32_t>(stage);
    if (index < static_cast<uint32_t>(LaunchProfileStage::Count))
        launchTestContext_.stageCycles[index] += __rdtsc() - begin;
#else
    (void)stage;
    (void)begin;
#endif
}

inline void VoiceManager::BeginLaunchTestProfile(
    uint8_t channel, uint8_t note, const VoiceConfiguration* setups,
    uint32_t count) {
    if (!launchChurnProfilingEnabled_) {
        launchTestContext_.active = false;
        return;
    }
    TrackLaunchFrameForTest(currentFrame_);
    ++launchChurnStats_.logicalLaunches;
    launchChurnStats_.physicalVoicesRequested += count;
    launchTestContext_ = LaunchTestContext{};
    launchTestContext_.active = true;
    launchTestContext_.sampled =
        ((launchChurnStats_.logicalLaunches - 1u) & 4095u) == 0u;
    launchTestContext_.incomingChannel = channel;
    launchTestContext_.incomingNote = note;
    launchTestContext_.incomingCount = count;
    launchTestContext_.incomingSetups = setups;
#if defined(_MSC_VER)
    if (launchTestContext_.sampled)
        launchTestContext_.beginCycles = __rdtsc();
#endif
}

inline void VoiceManager::FinishLaunchTestProfile(bool success) {
    if (!launchTestContext_.active) return;
    if (success) {
        ++launchChurnStats_.successfulLaunches;
        launchChurnStats_.physicalVoicesConfigured +=
            launchTestContext_.incomingCount;
        ++launchTestProvisionalGroups_;
        launchTestProvisionalPhysicalVoices_ +=
            launchTestContext_.incomingCount;
    } else {
        ++launchChurnStats_.failedLaunches;
    }
    if (launchTestContext_.victimGroups != 0u)
        ++launchChurnStats_.stealTransactions;

    uint32_t bucket = LaunchChurnStats::kClassificationBuckets - 1u;
    if (launchTestContext_.victimGroups != 0u) {
        bucket = 0u;
        if (launchTestContext_.allVictimsSameFrame) bucket |= 1u;
        if (launchTestContext_.anyLayeredVictim) bucket |= 2u;
        if (launchTestContext_.anyVolatileVictim) bucket |= 4u;
        if (launchTestContext_.anyGeneralVictim) bucket |= 8u;
    }
    if (launchTestContext_.sampled &&
        bucket < LaunchChurnStats::kClassificationBuckets) {
        LaunchChurnBucketStats& result = launchChurnStats_.buckets[bucket];
        ++result.samples;
#if defined(_MSC_VER)
        result.totalCycles += __rdtsc() - launchTestContext_.beginCycles;
#endif
        for (uint32_t stage = 0u;
             stage < static_cast<uint32_t>(LaunchProfileStage::Count);
             ++stage) {
            result.stageCycles[stage] +=
                launchTestContext_.stageCycles[stage];
        }
    }
    launchTestContext_.active = false;
}

inline void VoiceManager::RecordVictimGroupForTest(
    VoiceHandle selected, LaunchVictimPath path, bool reservedInPlace) {
    if (!launchTestContext_.active || selected >= maxVoices_ ||
        v.state[selected] == static_cast<uint8_t>(VoiceState::Free)) return;

    VoiceHandle smallGroup[8]{};
    uint32_t smallCount = 0u;
    uint32_t groupCount = 0u;
    uint32_t sameFrameCount = 0u;
    bool allStable = true;
    auto observe = [&](VoiceHandle handle) {
        if (handle >= maxVoices_ || groupCount >= maxVoices_) return;
        if (smallCount < 8u) smallGroup[smallCount++] = handle;
        ++groupCount;
        if (v.birthFrame[handle] == currentFrame_) ++sameFrameCount;
        if (!IsStableStealCandidate(handle)) allStable = false;
    };

    int32_t linked = playGroupPrev_[selected];
    while (linked >= 0 && groupCount < maxVoices_) {
        const VoiceHandle handle = static_cast<VoiceHandle>(linked);
        linked = playGroupPrev_[handle];
        observe(handle);
    }
    observe(selected);
    linked = playGroupNext_[selected];
    while (linked >= 0 && groupCount < maxVoices_) {
        const VoiceHandle handle = static_cast<VoiceHandle>(linked);
        linked = playGroupNext_[handle];
        observe(handle);
    }
    if (groupCount == 0u) return;

    const bool allSameFrame = sameFrameCount == groupCount;
    ++launchChurnStats_.victimGroups;
    launchChurnStats_.physicalVictims += groupCount;
    launchChurnStats_.sameFramePhysicalVictims += sameFrameCount;
    if (allSameFrame) ++launchChurnStats_.sameFrameVictimGroups;
    if (groupCount == 1u) ++launchChurnStats_.monoVictimGroups;
    else ++launchChurnStats_.layeredVictimGroups;
    if (groupCount == launchTestContext_.incomingCount)
        ++launchChurnStats_.matchingSizeVictimGroups;
    else
        ++launchChurnStats_.mismatchedSizeVictimGroups;
    if (allStable) ++launchChurnStats_.stableVictimGroups;
    else ++launchChurnStats_.volatileVictimGroups;
    if (v.channel[selected] == launchTestContext_.incomingChannel &&
        v.note[selected] == launchTestContext_.incomingNote) {
        ++launchChurnStats_.sameChannelKeyVictimGroups;
    }

    bool matchingPlan = groupCount == launchTestContext_.incomingCount &&
        groupCount <= 8u && smallCount == groupCount &&
        launchTestContext_.incomingSetups != nullptr;
    if (matchingPlan) {
        uint32_t oldIdentities[8]{};
        uint32_t newIdentities[8]{};
        for (uint32_t i = 0u; i < groupCount; ++i) {
            oldIdentities[i] =
                (static_cast<uint32_t>(v.presetIndex[smallGroup[i]]) << 16u) |
                v.regionIndex[smallGroup[i]];
            newIdentities[i] =
                (static_cast<uint32_t>(
                    launchTestContext_.incomingSetups[i].presetIndex) << 16u) |
                launchTestContext_.incomingSetups[i].regionIndex;
        }
        std::sort(oldIdentities, oldIdentities + groupCount);
        std::sort(newIdentities, newIdentities + groupCount);
        matchingPlan = std::equal(oldIdentities,
                                  oldIdentities + groupCount,
                                  newIdentities);
    }
    if (matchingPlan) ++launchChurnStats_.matchingPlanVictimGroups;

    switch (path) {
        case LaunchVictimPath::SingleInPlace:
            ++launchChurnStats_.singleInPlaceVictimGroups;
            break;
        case LaunchVictimPath::MatchingReuse:
            ++launchChurnStats_.matchingReuseVictimGroups;
            if (reservedInPlace)
                ++launchChurnStats_.reservedReuseVictimGroups;
            break;
        case LaunchVictimPath::General:
            ++launchChurnStats_.generalVictimGroups;
            break;
    }

    ++launchTestContext_.victimGroups;
    launchTestContext_.sameFramePhysicalVictims += sameFrameCount;
    launchTestContext_.allVictimsSameFrame &= allSameFrame;
    launchTestContext_.anyLayeredVictim |= groupCount > 1u;
    launchTestContext_.anyVolatileVictim |= !allStable;
    launchTestContext_.anyGeneralVictim |=
        path == LaunchVictimPath::General || !reservedInPlace;

    launchTestProvisionalPhysicalVoices_ -= sameFrameCount;
    if (allSameFrame) --launchTestProvisionalGroups_;
}
#endif

inline void VoiceManager::ApplyRuntimeVoiceLimit(uint64_t frame) {
    SetCurrentFrame(frame);
    // RuntimeLink publishes only a process-local atomic request. Applying it
    // here keeps all VoiceManager mutation on the audio/render thread. This
    // method is called once before a render block is planned, never while
    // dense workers are consuming an immutable chunk plan.
    uint32_t requestedLimit = RequestedRuntimeVoiceLimit();
    if (requestedLimit > RuntimeVoiceGrowthCeiling()) {
        RequestRuntimeVoiceLimit(voiceLimit_);
        requestedLimit = voiceLimit_;
    }
    if (requestedLimit > maxVoices_ && requestedLimit <= kMaxPolyphony) {
        if (!GrowCapacity(requestedLimit)) {
            // Do not retry a failed large allocation every render boundary.
            // Keep the last applied cap and let a later user action request
            // growth again explicitly.
            RequestRuntimeVoiceLimit(voiceLimit_);
            requestedLimit = voiceLimit_;
        }
    }
    if (requestedLimit != 0u && requestedLimit <= maxVoices_ &&
        requestedLimit != voiceLimit_) {
        SetVoiceLimit(requestedLimit);
        lastVoiceLimitEnforceFrame_ = UINT64_MAX; // enforce this boundary
    }

    if (voiceLimit_ != 0u && activeCount_ > voiceLimit_) {
        const uint64_t enforceInterval =
            (std::max)(uint64_t{1}, static_cast<uint64_t>(sampleRate_) / 100u);
        if (lastVoiceLimitEnforceFrame_ == UINT64_MAX ||
            frame >= lastVoiceLimitEnforceFrame_ + enforceInterval) {
            // Bound each reduction pass so a giant 100k -> 1k change cannot
            // spend an unbounded amount of one callback in victim selection.
            // Repeated boundaries continue shedding until the logical cap is
            // reached; each victim receives the requested ~50 ms release.
            EnforceVoiceLimit(256u, 0.050f);
            lastVoiceLimitEnforceFrame_ = frame;
        }
    }
}

inline void VoiceManager::SetCurrentFrame(uint64_t frame) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    TrackLaunchFrameForTest(frame);
#endif
    if (frame != currentFrame_) stealTailMinHeapValid_ = false;
    currentFrame_ = frame;
}

inline uint32_t VoiceManager::GetChannelActiveCount(uint8_t channel) const {
    return channel < kChannelCount ? channelActiveCount_[channel] : 0u;
}

template <typename Consumer>
inline void VoiceManager::ForEachChannelActive(
    uint8_t channel, Consumer&& consume) const noexcept {
    if (channel >= kChannelCount) return;
    uint32_t block = channelActiveHead_[channel];
    while (block != UINT32_MAX) {
        const ChannelIndexBlock& page = channelIndexBlocks_[block];
        for (uint32_t offset = 0u; offset < page.count; ++offset)
            consume(static_cast<VoiceHandle>(page.handles[offset]));
        block = page.next;
    }
}

inline void VoiceManager::InvalidateStealCandidates() {
    stealHeapValid_ = false;
}

inline uint32_t VoiceManager::GetRenderClassCount(
    VoiceRenderClass renderClass) const {
    const uint32_t index = static_cast<uint32_t>(renderClass);
    return index < kVoiceRenderClassCount ? renderClassCount_[index] : 0u;
}

template <typename Consumer>
inline void VoiceManager::ForEachRenderClassBlock(
    VoiceRenderClass renderClass, Consumer&& consume) const noexcept {
    const uint32_t index = static_cast<uint32_t>(renderClass);
    if (index >= kVoiceRenderClassCount) return;
    uint32_t block = renderClassHead_[index];
    while (block != UINT32_MAX) {
        const RenderClassBlock& page = renderClassBlocks_[block];
        consume(page.handles, page.count);
        block = page.next;
    }
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
    // BASS ranks a control-derived level before waveform sampling. The
    // cached stereo energy norm removes constant-power pan from that estimate.
    // It changes only with region/channel gain, while volatile decay/release
    // voices are rescored every output frame.
    const float outputGain = v.stealOutputGain[handle];
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
    v.releaseDecay[handle]   = kDefaultReleaseDecay;
    v.gainLeft[handle]     = 1.0f;
    v.gainRight[handle]    = 1.0f;
    v.sampleStart[handle]  = 0;
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
    v.envelopeStage[handle]     = 0;
    v.renderClass[handle] = static_cast<uint8_t>(VoiceRenderClass::Generic);
    v.heldBySustain[handle]     = 0;
    v.heldBySostenuto[handle]   = 0;
    v.releaseStartInBlock[handle] = 0;
    v.nextChannelKeyVoice[handle] = -1;
    v.prevChannelKeyVoice[handle] = -1;
    playGroupNext_[handle] = -1;
    playGroupPrev_[handle] = -1;
    v.mixGainL[handle]          = 0.0f;
    v.mixGainR[handle]          = 0.0f;
    v.renderGainL[handle]       = 0.0f;
    v.renderGainR[handle]       = 0.0f;
    v.stealOutputGain[handle]   = 0.0f;
    v.vibLfoToPitchCents[handle] = 0.0f;
    v.vibLfoSteps[handle]       = 0.0f;
    v.vibLfoPhases[handle]      = 0.0f;
    v.vibLfoDelays[handle]      = 0u;
    v.vibLfoModulated[handle]   = 0u;
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
    v.heldBySostenuto[handle] = 0u;
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

inline uint32_t VoiceManager::AllocateChannelIndexBlock() {
    assert(channelIndexFreeTop_ != 0u);
    if (channelIndexFreeTop_ == 0u) return UINT32_MAX;
    const uint32_t block = channelIndexFreeStack_[--channelIndexFreeTop_];
    channelIndexBlocks_[block].count = 0u;
    channelIndexBlocks_[block].previous = UINT32_MAX;
    channelIndexBlocks_[block].next = UINT32_MAX;
    return block;
}

inline void VoiceManager::FreeChannelIndexBlock(uint32_t block) {
    if (block >= channelIndexBlockCount_) return;
    channelIndexBlocks_[block].count = 0u;
    channelIndexBlocks_[block].previous = UINT32_MAX;
    channelIndexBlocks_[block].next = UINT32_MAX;
    assert(channelIndexFreeTop_ < channelIndexBlockCount_);
    channelIndexFreeStack_[channelIndexFreeTop_++] = block;
}

inline VoiceHandle VoiceManager::LastChannelActive(uint8_t channel) const {
    if (channel >= kChannelCount) return kInvalidVoice;
    const uint32_t tail = channelActiveTail_[channel];
    if (tail == UINT32_MAX || channelIndexBlocks_[tail].count == 0u)
        return kInvalidVoice;
    return static_cast<VoiceHandle>(channelIndexBlocks_[tail].handles[
        channelIndexBlocks_[tail].count - 1u]);
}

inline void VoiceManager::LinkChannelActive(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t channel = v.channel[handle];
    uint32_t tail = channelActiveTail_[channel];
    if (tail == UINT32_MAX ||
        channelIndexBlocks_[tail].count == kChannelIndexBlockSize) {
        const uint32_t block = AllocateChannelIndexBlock();
        assert(block != UINT32_MAX);
        if (block == UINT32_MAX) return;
        channelIndexBlocks_[block].previous = tail;
        if (tail != UINT32_MAX)
            channelIndexBlocks_[tail].next = block;
        else
            channelActiveHead_[channel] = block;
        channelActiveTail_[channel] = block;
        tail = block;
    }
    ChannelIndexBlock& page = channelIndexBlocks_[tail];
    const uint32_t offset = page.count++;
    page.handles[offset] = handle;
    channelActiveBlock_[handle] = tail;
    channelActiveOffset_[handle] = static_cast<uint8_t>(offset);
    ++channelActiveCount_[channel];
}

inline void VoiceManager::UnlinkChannelActive(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const uint8_t channel = v.channel[handle];
    const uint32_t block = channelActiveBlock_[handle];
    const uint32_t offset = channelActiveOffset_[handle];
    const uint32_t tail = channelActiveTail_[channel];
    if (block >= channelIndexBlockCount_ || tail >= channelIndexBlockCount_ ||
        offset >= channelIndexBlocks_[block].count) return;

    ChannelIndexBlock& tailPage = channelIndexBlocks_[tail];
    const uint32_t lastOffset = tailPage.count - 1u;
    const uint32_t moved = tailPage.handles[lastOffset];
    if (block != tail || offset != lastOffset) {
        channelIndexBlocks_[block].handles[offset] = moved;
        channelActiveBlock_[moved] = block;
        channelActiveOffset_[moved] = static_cast<uint8_t>(offset);
    }
    --tailPage.count;
    --channelActiveCount_[channel];
    channelActiveBlock_[handle] = UINT32_MAX;
    channelActiveOffset_[handle] = UINT8_MAX;

    if (tailPage.count == 0u) {
        const uint32_t previous = tailPage.previous;
        if (previous != UINT32_MAX)
            channelIndexBlocks_[previous].next = UINT32_MAX;
        else
            channelActiveHead_[channel] = UINT32_MAX;
        channelActiveTail_[channel] = previous;
        FreeChannelIndexBlock(tail);
    }
}

inline void VoiceManager::MoveChannelActiveInPlace(VoiceHandle handle,
                                                    uint8_t newChannel) {
    assert(handle < maxVoices_ && newChannel < kChannelCount);
    const uint8_t oldChannel = v.channel[handle];
    assert(oldChannel != newChannel);

    // Remove from the old dense channel pages, but keep this handle live and
    // transfer it directly into the new channel instead of publishing an
    // unindexed intermediate state.
    const uint32_t oldBlock = channelActiveBlock_[handle];
    const uint32_t oldOffset = channelActiveOffset_[handle];
    const uint32_t oldTail = channelActiveTail_[oldChannel];
    assert(oldBlock < channelIndexBlockCount_ &&
           oldTail < channelIndexBlockCount_ &&
           oldOffset < channelIndexBlocks_[oldBlock].count);
    ChannelIndexBlock& oldTailPage = channelIndexBlocks_[oldTail];
    const uint32_t oldLastOffset = oldTailPage.count - 1u;
    const uint32_t moved = oldTailPage.handles[oldLastOffset];
    if (oldBlock != oldTail || oldOffset != oldLastOffset) {
        channelIndexBlocks_[oldBlock].handles[oldOffset] = moved;
        channelActiveBlock_[moved] = oldBlock;
        channelActiveOffset_[moved] = static_cast<uint8_t>(oldOffset);
    }
    --oldTailPage.count;
    --channelActiveCount_[oldChannel];
    if (oldTailPage.count == 0u) {
        const uint32_t previous = oldTailPage.previous;
        if (previous != UINT32_MAX)
            channelIndexBlocks_[previous].next = UINT32_MAX;
        else
            channelActiveHead_[oldChannel] = UINT32_MAX;
        channelActiveTail_[oldChannel] = previous;
        FreeChannelIndexBlock(oldTail);
    }

    v.channel[handle] = newChannel;
    uint32_t newTail = channelActiveTail_[newChannel];
    if (newTail == UINT32_MAX ||
        channelIndexBlocks_[newTail].count == kChannelIndexBlockSize) {
        const uint32_t block = AllocateChannelIndexBlock();
        assert(block != UINT32_MAX);
        channelIndexBlocks_[block].previous = newTail;
        if (newTail != UINT32_MAX)
            channelIndexBlocks_[newTail].next = block;
        else
            channelActiveHead_[newChannel] = block;
        channelActiveTail_[newChannel] = block;
        newTail = block;
    }
    ChannelIndexBlock& newPage = channelIndexBlocks_[newTail];
    const uint32_t newOffset = newPage.count++;
    newPage.handles[newOffset] = handle;
    channelActiveBlock_[handle] = newTail;
    channelActiveOffset_[handle] = static_cast<uint8_t>(newOffset);
    ++channelActiveCount_[newChannel];
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

inline uint32_t VoiceManager::AllocateRenderClassBlock() {
    assert(renderClassFreeTop_ != 0u);
    if (renderClassFreeTop_ == 0u) return UINT32_MAX;
    const uint32_t block = renderClassFreeStack_[--renderClassFreeTop_];
    renderClassBlocks_[block].count = 0u;
    renderClassBlocks_[block].previous = UINT32_MAX;
    renderClassBlocks_[block].next = UINT32_MAX;
    return block;
}

inline void VoiceManager::FreeRenderClassBlock(uint32_t block) {
    if (block >= renderClassBlockCount_) return;
    renderClassBlocks_[block].count = 0u;
    renderClassBlocks_[block].previous = UINT32_MAX;
    renderClassBlocks_[block].next = UINT32_MAX;
    assert(renderClassFreeTop_ < renderClassBlockCount_);
    renderClassFreeStack_[renderClassFreeTop_++] = block;
}

inline void VoiceManager::LinkRenderClass(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    const VoiceRenderClass renderClass = ClassifyVoice(handle);
    const uint32_t classIndex = static_cast<uint32_t>(renderClass);
    uint32_t tail = renderClassTail_[classIndex];
    if (tail == UINT32_MAX ||
        renderClassBlocks_[tail].count == kRenderClassBlockSize) {
        const uint32_t block = AllocateRenderClassBlock();
        assert(block != UINT32_MAX);
        if (block == UINT32_MAX) return;
        renderClassBlocks_[block].previous = tail;
        if (tail != UINT32_MAX)
            renderClassBlocks_[tail].next = block;
        else
            renderClassHead_[classIndex] = block;
        renderClassTail_[classIndex] = block;
        tail = block;
    }
    RenderClassBlock& page = renderClassBlocks_[tail];
    const uint32_t offset = page.count++;
    page.handles[offset] = handle;
    renderClassBlock_[handle] = tail;
    renderClassOffset_[handle] = static_cast<uint16_t>(offset);
    ++renderClassCount_[classIndex];
    renderClassMask_ |= 1u << classIndex;
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
    const uint32_t block = renderClassBlock_[handle];
    const uint32_t offset = renderClassOffset_[handle];
    const uint32_t tail = renderClassTail_[classIndex];
    if (block >= renderClassBlockCount_ || tail >= renderClassBlockCount_ ||
        offset >= renderClassBlocks_[block].count) return;

    RenderClassBlock& tailPage = renderClassBlocks_[tail];
    const uint32_t lastOffset = tailPage.count - 1u;
    const uint32_t moved = tailPage.handles[lastOffset];
    if (block != tail || offset != lastOffset) {
        renderClassBlocks_[block].handles[offset] = moved;
        renderClassBlock_[moved] = block;
        renderClassOffset_[moved] = static_cast<uint16_t>(offset);
    }
    --tailPage.count;
    const uint32_t remaining = --renderClassCount_[classIndex];
    renderClassBlock_[handle] = UINT32_MAX;
    renderClassOffset_[handle] = UINT16_MAX;

    if (tailPage.count == 0u) {
        const uint32_t previous = tailPage.previous;
        if (previous != UINT32_MAX)
            renderClassBlocks_[previous].next = UINT32_MAX;
        else
            renderClassHead_[classIndex] = UINT32_MAX;
        renderClassTail_[classIndex] = previous;
        FreeRenderClassBlock(tail);
    }
    if (remaining == 0u) renderClassMask_ &= ~(1u << classIndex);
}

inline void VoiceManager::RefreshRenderClass(VoiceHandle handle) {
    if (handle >= maxVoices_ ||
        v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return;
    const VoiceRenderClass desired = ClassifyVoice(handle);
    if (v.renderClass[handle] == static_cast<uint8_t>(desired) &&
        renderClassBlock_[handle] < renderClassBlockCount_ &&
        renderClassOffset_[handle] <
            renderClassBlocks_[renderClassBlock_[handle]].count) {
        // Attack and decay intentionally share the transient render class,
        // but only attack has a time-invariant protected steal level. Move
        // the candidate between the persistent tree and volatile heap even
        // when no render-list migration is required.
        if (stealHeapValid_ && stealCandidateDeferred_[handle] == 0u) {
            const bool indexedStable =
                stealWinnerTree_[stealTreeLeafBase_ + handle] != 0u;
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
    if (handle >= maxVoices_ || handle >= kStealTailReserve ||
        v.stealTailFramesRemaining[handle] == 0u ||
        stealTailPosition_[handle] < stealTailCount_) return;
    const uint32_t position = stealTailCount_++;
    stealTailList_[position] = handle;
    stealTailPosition_[handle] = position;
    stealTailMinHeapValid_ = false;
}

inline void VoiceManager::UnlinkStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_ || handle >= kStealTailReserve) return;
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
    if (handle >= maxVoices_ || handle >= kStealTailReserve) return;
    if (v.stealTailFramesRemaining[handle] != 0u)
        LinkStealTail(handle);
    else
        UnlinkStealTail(handle);
}

inline float VoiceManager::ComputeTailLevel(uint32_t tailSlot) const {
    if (tailSlot >= maxVoices_ || tailSlot >= kStealTailReserve ||
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
        if (right < stealTailMinHeapCount_ &&
            stealTailMinHeapKey_[right] < stealTailMinHeapKey_[left])
            quietest = right;
        if (stealTailMinHeapKey_[quietest] >=
            stealTailMinHeapKey_[position]) break;
        const uint64_t temporaryKey = stealTailMinHeapKey_[position];
        stealTailMinHeapKey_[position] = stealTailMinHeapKey_[quietest];
        stealTailMinHeapKey_[quietest] = temporaryKey;
        position = quietest;
    }
}

inline void VoiceManager::BuildStealTailMinHeap() {
    stealTailMinHeapCount_ = (std::min)(stealTailCount_, kStealTailReserve);
    for (uint32_t i = 0; i < stealTailMinHeapCount_; ++i) {
        const uint32_t tailSlot = stealTailList_[i];
        const float level = ComputeTailLevel(tailSlot);
        uint32_t levelBits = 0u;
        std::memcpy(&levelBits, &level, sizeof(levelBits));
        // Tail levels are finite and non-negative, so IEEE-754 bit order is
        // numeric order. The low word preserves the existing list-position
        // tie rule without a second load or floating-point branch.
        stealTailMinHeapKey_[i] =
            (static_cast<uint64_t>(levelBits) << 32u) |
            stealTailPosition_[tailSlot];
    }
    if (stealTailMinHeapCount_ > 1u) {
        for (uint32_t position = stealTailMinHeapCount_ / 2u;
             position-- > 0u;) StealTailHeapSiftDown(position);
    }
    stealTailMinHeapFrame_ = currentFrame_;
    stealTailMinHeapValid_ = true;
}

inline uint32_t VoiceManager::SelectStealTailSlot(
    float outgoingLevel, bool& replacingHeapRoot) {
    replacingHeapRoot = false;
    const uint32_t reserveLimit = (std::min)(maxVoices_, kStealTailReserve);
    if (reserveLimit == 0u || outgoingLevel <= 0.0f) return UINT32_MAX;
    if (stealTailCount_ < reserveLimit) {
        for (uint32_t slot = 0; slot < reserveLimit; ++slot) {
            if (v.stealTailFramesRemaining[slot] == 0u) return slot;
        }
        return UINT32_MAX;
    }

    if (!stealTailMinHeapValid_ || stealTailMinHeapFrame_ != currentFrame_)
        BuildStealTailMinHeap();
    const uint64_t quietestKey = stealTailMinHeapKey_[0];
    const uint32_t quietestBits = static_cast<uint32_t>(quietestKey >> 32u);
    float quietestLevel = 0.0f;
    std::memcpy(&quietestLevel, &quietestBits, sizeof(quietestLevel));
    if (outgoingLevel <= quietestLevel) return UINT32_MAX;
    replacingHeapRoot = true;
    return stealTailList_[static_cast<uint32_t>(quietestKey)];
}

inline void VoiceManager::CaptureStealTail(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t tailBegin = BeginLaunchStageForTest();
    if (launchTestContext_.active)
        ++launchChurnStats_.tailCaptureAttempts;
#endif
    if (preTailCaptureHook_)
        preTailCaptureHook_(handle, preTailCaptureUserData_);
    const float gain = v.currentGain[handle];
    const float mixL = v.mixGainL[handle];
    const float mixR = v.mixGainR[handle];
    const float outgoingLevel = std::fabs(gain) *
        (std::fabs(mixL) + std::fabs(mixR));
    if (v.sampleBacked[handle] == 0u || v.relEnd[handle] <= 1u ||
        outgoingLevel <= kVoiceRetireThreshold) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (launchTestContext_.active)
            ++launchChurnStats_.tailCaptureIneligible;
        EndLaunchStageForTest(LaunchProfileStage::TailCapture, tailBegin);
#endif
        return;
    }
    bool replacingHeapRoot = false;
    const uint32_t tailSlot = SelectStealTailSlot(
        outgoingLevel, replacingHeapRoot);
    if (tailSlot == UINT32_MAX) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        if (launchTestContext_.active)
            ++launchChurnStats_.tailCaptureRejected;
        EndLaunchStageForTest(LaunchProfileStage::TailCapture, tailBegin);
#endif
        return;
    }

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const bool replacingTail =
        v.stealTailFramesRemaining[tailSlot] != 0u;
    if (launchTestContext_.active) {
        ++launchChurnStats_.tailCaptureAccepted;
        if (replacingTail) ++launchChurnStats_.tailCaptureReplaced;
    }
#endif

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
    // Carry the victim's rotation state over so the tail keeps the same
    // phase profile — no spectral jump at the steal boundary.
    if (v.rot) v.stealTailRot[tailSlot] = v.rot[handle];
    if (replacingHeapRoot) {
        // SelectStealTailSlot accepts a replacement only when it is louder
        // than the current root, so the updated root can move only downward.
        uint32_t levelBits = 0u;
        std::memcpy(&levelBits, &outgoingLevel, sizeof(levelBits));
        stealTailMinHeapKey_[0] =
            (static_cast<uint64_t>(levelBits) << 32u) |
            stealTailPosition_[tailSlot];
        StealTailHeapSiftDown(0u);
    } else {
        LinkStealTail(static_cast<VoiceHandle>(tailSlot));
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::TailCapture, tailBegin);
#endif
}

inline void VoiceManager::RetireStolenSibling(VoiceHandle handle,
                                               VoiceHandle selectedVictim) {
    if (handle >= maxVoices_ ||
        v.state[handle] == static_cast<uint8_t>(VoiceState::Free)) return;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t treeBegin = BeginLaunchStageForTest();
#endif
    if (stealHeapValid_) RemoveStealCandidate(handle);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance, treeBegin);
#endif
    CaptureStealTail(handle);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t lifecycleBegin = BeginLaunchStageForTest();
#endif
    ++stealCount_;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
        releasingCount_.fetch_sub(1u, std::memory_order_relaxed);
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
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Lifecycle, lifecycleBegin);
#endif
}

inline bool VoiceManager::HigherPriorityCandidate(const StealCandidate& a,
                                                   const StealCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.activePosition < b.activePosition;
}

SVMS_VM_FORCEINLINE uint64_t VoiceManager::EncodeStableWinnerKey(
    float score, uint32_t activePosition) {
    if (score == 0.0f) score = 0.0f;
    uint32_t bits = 0u;
    std::memcpy(&bits, &score, sizeof(bits));
    const uint32_t orderedScore = (bits & 0x80000000u) != 0u
        ? ~bits : bits ^ 0x80000000u;
    return (static_cast<uint64_t>(orderedScore) << 32u) |
        (UINT32_MAX - activePosition);
}

SVMS_VM_FORCEINLINE void VoiceManager::RefreshStealWinnerPath(
    VoiceHandle handle) {
    uint32_t node = stealTreeLeafBase_ + handle;
    uint64_t winner = stealWinnerTree_[node];
    while (node > 1u) {
        const uint64_t sibling = stealWinnerTree_[node ^ 1u];
        winner = winner > sibling ? winner : sibling;
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
    if (count == 1u) {
        RefreshStealWinnerPath(handles[0]);
        return;
    }
    if (count > kBatchPaths) {
        for (uint32_t i = 0u; i < count; ++i)
            RefreshStealWinnerPath(handles[i]);
        return;
    }

    uint32_t nodes[kBatchPaths]{};
    uint32_t nodeCount = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t node = (stealTreeLeafBase_ + handles[i]) >> 1u;
        bool duplicate = false;
        for (uint32_t existing = 0u; existing < nodeCount; ++existing)
            duplicate |= nodes[existing] == node;
        if (!duplicate) nodes[nodeCount++] = node;
    }

    while (nodeCount != 0u) {
        uint32_t parentCount = 0u;
        for (uint32_t i = 0u; i < nodeCount; ++i) {
            const uint32_t node = nodes[i];
            const uint64_t left = stealWinnerTree_[node << 1u];
            const uint64_t right = stealWinnerTree_[(node << 1u) + 1u];
            stealWinnerTree_[node] = left > right ? left : right;
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
    for (uint32_t node = stealTreeLeafBase_; node-- > 1u;) {
        const uint64_t left = stealWinnerTree_[node << 1u];
        const uint64_t right = stealWinnerTree_[(node << 1u) + 1u];
        stealWinnerTree_[node] = left > right ? left : right;
    }
}

inline void VoiceManager::BuildStealHeap() {
    ++stealHeapBuildCount_;
    stealHeapCount_ = 0u;
    stealVolatileCount_ = 0u;
    stealVolatileHeapCount_ = 0u;
    stealVolatileHeapValid_ = false;
    std::memset(stealWinnerTree_, 0,
                sizeof(*stealWinnerTree_) * stealTreeLeafBase_ * 2u);
    std::memset(stealVolatilePosition_, 0xff,
                sizeof(*stealVolatilePosition_) * maxVoices_);
    for (uint32_t position = 0; position < activeCount_; ++position) {
        const uint32_t handle = activeList_[position];
        if (IsStableStealCandidate(handle)) {
            ++stealHeapCount_;
            const float score = ComputeStableStealKey(handle);
            stealStableKey_[handle] = EncodeStableWinnerKey(
                score, position);
            stealWinnerTree_[stealTreeLeafBase_ + handle] =
                stealStableKey_[handle];
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

    // Releasing-ring fast path. Under saturated chopped-note churn the pool
    // is dominated by end-of-life Releasing voices, so the exhaustive
    // winner search adds nothing there: any Releasing voice sits at the
    // bottom of BASSMIDI's effective-level ranking anyway (its envelope has
    // left sustain). Engages only on a full free stack with the pool ≥85%
    // occupied; every other case falls through to the exact existing tier
    // selection unchanged, keeping sparse/mixed behavior byte-identical.
    // Root-reserving probes keep their exact tier semantics.
    if (!reserveVolatileRoot && ReleasingRingEligible()) {
        uint32_t ringPosition = 0u;
        const VoiceHandle ringVictim =
            NextValidReleasingRingVictim(ringPosition, true);
        if (ringVictim != kInvalidVoice) {
            // Repair the incremental index: the victim is unlinked from
            // whichever tier held it, mirroring tier-pop side effects.
            RemoveStealCandidate(ringVictim);
            ++releasingRingHits_;
            activePosition = ringPosition;
            return ringVictim;
        }
    }

    // Saturated sustained playback has no decay/release candidates. The
    // tournament root is already the exact exhaustive winner, including the
    // active-position tie, so avoid constructing/comparing generic candidate
    // records and touching the empty volatile heap on every note launch.
    if (stealHeapCount_ > 0u && stealVolatileCount_ == 0u) {
        const uint64_t rootKey = stealWinnerTree_[1];
        const uint32_t winnerPosition =
            UINT32_MAX - static_cast<uint32_t>(rootKey);
        assert(rootKey != 0u && winnerPosition < activeCount_);
        const VoiceHandle winner = static_cast<VoiceHandle>(
            activeList_[winnerPosition]);
        assert(stealStableKey_[winner] == rootKey);
        activePosition = winnerPosition;
        if (reserveVolatileRoot) {
            stealCandidateReserved_[winner] = 2u;
        } else {
            --stealHeapCount_;
            stealWinnerTree_[stealTreeLeafBase_ + winner] = 0u;
            RefreshStealWinnerPath(winner);
        }
        return winner;
    }

    // Transient gains change while samples render, not between equal-frame
    // MIDI events. Rebuild once when the output frame advances and keep exact
    // O(log N) replacement updates for the rest of that frame.
    if (!stealVolatileHeapValid_ || stealVolatileHeapFrame_ != currentFrame_)
        BuildVolatileStealHeap();

    uint64_t bestKey = 0u;
    uint32_t bestHandle = UINT32_MAX;
    uint32_t bestPosition = 0u;
    bool haveBest = false;
    bool bestIsVolatile = false;
    if (stealHeapCount_ > 0u) {
        const uint64_t rootKey = stealWinnerTree_[1];
        const uint32_t winnerPosition =
            UINT32_MAX - static_cast<uint32_t>(rootKey);
        assert(rootKey != 0u && winnerPosition < activeCount_);
        const VoiceHandle stableWinner = static_cast<VoiceHandle>(
            activeList_[winnerPosition]);
        assert(stealStableKey_[stableWinner] == rootKey);
        bestKey = rootKey;
        bestHandle = stableWinner;
        bestPosition = winnerPosition;
        haveBest = true;
    }
    if (stealVolatileHeapCount_ > 0u) {
        const uint64_t candidateKey = stealVolatileHeapKey_[0];
        if (!haveBest || candidateKey > bestKey) {
            const uint32_t candidatePosition =
                UINT32_MAX - static_cast<uint32_t>(candidateKey);
            bestKey = candidateKey;
            bestHandle = stealVolatileHeapHandle_[0];
            bestPosition = candidatePosition;
            haveBest = true;
            bestIsVolatile = true;
        }
    }
    // A valid current-frame heap contains every linked volatile candidate.
    // LinkVolatileCandidate inserts into it, UnlinkVolatileCandidate removes
    // from both structures, and a reserved root deliberately remains in both
    // until its launch transaction commits. The former defensive list walk
    // therefore inspected the entire volatile population for every steal and
    // never contributed a candidate.
    assert(stealVolatileHeapCount_ == stealVolatileCount_);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    // Retain the redundant predecessor scan as an explicit benchmark oracle,
    // disabled by default and absent from production builds.
    if (volatileFallbackScanForTest_) {
        for (uint32_t i = 0u; i < stealVolatileCount_; ++i) {
            const uint32_t handle = stealVolatileList_[i];
            if (stealVolatileHeapPosition_[handle] < stealVolatileHeapCount_)
                continue;
            const uint32_t candidatePosition = activePosition_[handle];
            const uint64_t candidateKey = EncodeStableWinnerKey(
                ComputeStableStealKey(static_cast<VoiceHandle>(handle)),
                candidatePosition);
            if (!haveBest || candidateKey > bestKey) {
                bestKey = candidateKey;
                bestHandle = handle;
                bestPosition = candidatePosition;
                haveBest = true;
                bestIsVolatile = true;
            }
        }
    }
#endif
    if (!haveBest) return kInvalidVoice;
    activePosition = bestPosition;
    if (bestIsVolatile) {
        const bool canReserve = reserveVolatileRoot &&
            stealVolatileHeapPosition_[bestHandle] == 0u;
        if (canReserve) {
            stealCandidateReserved_[bestHandle] = 1u;
        } else {
            UnlinkVolatileCandidate(static_cast<VoiceHandle>(bestHandle));
        }
    } else {
        if (reserveVolatileRoot) {
            // A single transactional note launch overwrites this handle and
            // commits before another steal. Keep its leaf in place so commit
            // changes one winner-tree path instead of remove + insert paths.
            stealCandidateReserved_[bestHandle] = 2u;
        } else {
            --stealHeapCount_;
            stealWinnerTree_[stealTreeLeafBase_ + bestHandle] = 0u;
            RefreshStealWinnerPath(static_cast<VoiceHandle>(bestHandle));
        }
    }
    return static_cast<VoiceHandle>(bestHandle);
}

// Thin batch wrapper over PopStealCandidate(pos, false). Deliberately NOT a
// heap-algorithm optimization: every victim is produced by one complete
// single-victim pop, including full tree/ring/heap repair, so victim k sees
// exactly the structure state that the k-th of K sequential pops would see.
// This guarantees the batch picks the same N voices, in the same priority
// order, as K independent pops. The only saving is call-overhead amortization
// for the caller (a same-frame run of K note-ons currently re-enters
// LaunchVoiceGroup -> AllocateVoiceOrSteal -> PopStealCandidate per note).
// Note: a true algorithmic batching (bulk-extracting the N smallest keys with
// fewer than N repair operations) could exist in principle, but it would risk
// changing selection order and is explicitly out of scope for this task.
inline uint32_t VoiceManager::PopStealCandidates(uint32_t count,
                                                 VoiceHandle* outHandles,
                                                 uint32_t* outActivePositions) {
    uint32_t found = 0u;
    while (found < count) {
        const VoiceHandle victim =
            PopStealCandidate(outActivePositions[found], false);
        if (victim == kInvalidVoice) break;
        outHandles[found] = victim;
        ++found;
    }
    return found;
}

// Restores exactly the tier-index entry a reserve=false steal pop erased.
// Stable victims: rewrite the winner-tree leaf with the key the pop cleared,
// restore the heap count and repair the winner path — the precise inverse of
// the pop's `--stealHeapCount_; leaf = 0; RefreshStealWinnerPath` sequence.
// Volatile victims: LinkVolatileCandidate re-inserts into the candidate list
// and (same-frame valid) heap; the victim was the heap maximum when popped
// and no candidate with a larger key can appear within one exact-frame launch
// transaction (keys are frozen per frame and commits land only after the
// allocation loop), so it sifts back to the root the pop assumed. Heap-array
// order may differ from the pre-pop arrangement, but the heap is fully
// ordered by unique keys, so every subsequent selection is identical.
inline void VoiceManager::InsertPreselectedVictim(VoiceHandle victim) {
    // The sequential path builds the volatile heap before any selection that
    // can observe volatile candidates (tier c inside PopStealCandidate), so
    // the re-arm must guarantee the same precondition before re-linking a
    // volatile victim; otherwise a later probe's eligibility check would see
    // a stale heap and take a different launch path than the sequential one.
    if (!stealVolatileHeapValid_ || stealVolatileHeapFrame_ != currentFrame_)
        BuildVolatileStealHeap();
    if (IsStableStealCandidate(victim)) {
        stealWinnerTree_[stealTreeLeafBase_ + victim] =
            stealStableKey_[victim];
        ++stealHeapCount_;
        RefreshStealWinnerPath(victim);
    } else {
        LinkVolatileCandidate(victim);
    }
}

// Re-inserts every batch-popped victim of one launch transaction that was
// never consumed (fed to a layer) and is still live. Victims retired as
// play-group siblings of an earlier layer's victim are Free and are skipped;
// consumed victims were replaced in place and are skipped via `consumed`.
inline void VoiceManager::RearmLiveBatchVictims(const VoiceHandle* victims,
                                                uint32_t popped,
                                                const bool* consumed) {
    if (!victims || !consumed) return;
    for (uint32_t i = 0u; i < popped; ++i) {
        const VoiceHandle victim = victims[i];
        if (victim == kInvalidVoice || consumed[i] ||
            victim >= maxVoices_ ||
            v.state[victim] != static_cast<uint8_t>(VoiceState::Active))
            continue;
        InsertPreselectedVictim(victim);
    }
}

inline void VoiceManager::PushStealCandidate(VoiceHandle handle,
                                              uint32_t activePosition) {
    if (!stealHeapValid_ || handle >= maxVoices_) return;
    if (!IsStableStealCandidate(handle)) {
        LinkVolatileCandidate(handle);
        return;
    }
    ++stealHeapCount_;
    const float score = ComputeStableStealKey(handle);
    stealStableKey_[handle] = EncodeStableWinnerKey(
        score, activePosition);
    stealWinnerTree_[stealTreeLeafBase_ + handle] = stealStableKey_[handle];
    RefreshStealWinnerPath(handle);
}

inline void VoiceManager::UpdateStealCandidate(VoiceHandle handle) {
    if (!stealHeapValid_ || handle >= maxVoices_ ||
        stealCandidateDeferred_[handle] != 0u) return;

    const uint32_t leaf = stealTreeLeafBase_ + handle;
    const bool indexedStable = stealWinnerTree_[leaf] != 0u;
    const bool indexedVolatile =
        stealVolatilePosition_[handle] < stealVolatileCount_;
    const bool alive =
        v.state[handle] != static_cast<uint8_t>(VoiceState::Free);
    const bool wantsStable = alive && IsStableStealCandidate(handle);

    if (wantsStable) {
        if (indexedVolatile) UnlinkVolatileCandidate(handle);
        if (!indexedStable) ++stealHeapCount_;
        const float score = ComputeStableStealKey(handle);
        stealStableKey_[handle] = EncodeStableWinnerKey(
            score, activePosition_[handle]);
        stealWinnerTree_[leaf] = stealStableKey_[handle];
        // Updating an existing leaf used to clear and repair this path, then
        // insert and repair it a second time. One repair is exact because no
        // victim selection can observe the intermediate key on the audio
        // thread.
        RefreshStealWinnerPath(handle);
        return;
    }

    if (indexedStable) {
        stealWinnerTree_[leaf] = 0u;
        assert(stealHeapCount_ > 0u);
        --stealHeapCount_;
        RefreshStealWinnerPath(handle);
    }
    if (!alive) {
        if (indexedVolatile) UnlinkVolatileCandidate(handle);
        return;
    }
    if (!indexedVolatile) {
        LinkVolatileCandidate(handle);
        return;
    }

    // A volatile candidate can change gain or active-position without
    // changing class. Refresh its current-frame heap entry in place instead
    // of unlinking it from both the heap and compact candidate list.
    if (stealVolatileHeapValid_ &&
        stealVolatileHeapFrame_ == currentFrame_) {
        const uint32_t heapPosition = stealVolatileHeapPosition_[handle];
        if (heapPosition < stealVolatileHeapCount_) {
            stealVolatileHeapKey_[heapPosition] = EncodeStableWinnerKey(
                ComputeStableStealKey(handle), activePosition_[handle]);
            VolatileHeapSiftUp(heapPosition);
            VolatileHeapSiftDown(stealVolatileHeapPosition_[handle]);
        }
    }
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
        stealVolatileHeapKey_[heapPosition] = EncodeStableWinnerKey(
            ComputeStableStealKey(handle), activePosition_[handle]);
        stealVolatileHeapHandle_[heapPosition] = handle;
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
    const uint64_t temporaryKey = stealVolatileHeapKey_[a];
    stealVolatileHeapKey_[a] = stealVolatileHeapKey_[b];
    stealVolatileHeapKey_[b] = temporaryKey;
    const uint32_t temporaryHandle = stealVolatileHeapHandle_[a];
    stealVolatileHeapHandle_[a] = stealVolatileHeapHandle_[b];
    stealVolatileHeapHandle_[b] = temporaryHandle;
    stealVolatileHeapPosition_[stealVolatileHeapHandle_[a]] = a;
    stealVolatileHeapPosition_[stealVolatileHeapHandle_[b]] = b;
}

inline void VoiceManager::VolatileHeapSiftUp(uint32_t position) {
    while (position > 0u) {
        const uint32_t parent = (position - 1u) >> 1u;
        if (stealVolatileHeapKey_[position] <=
            stealVolatileHeapKey_[parent]) break;
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
            stealVolatileHeapKey_[right] > stealVolatileHeapKey_[left])
            best = right;
        if (stealVolatileHeapKey_[best] <=
            stealVolatileHeapKey_[position]) break;
        VolatileHeapSwap(position, best);
        position = best;
    }
}

inline void VoiceManager::BuildVolatileStealHeap() {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    // Sampled rdtsc pair: measures this rebuild without distorting every call.
    const bool profileThisBuild =
        (++volatileHeapProfileCounter_ & 15u) == 0u;
    const uint64_t profileBegin = profileThisBuild ? __rdtsc() : 0u;
#endif
    // The linked volatile set and the previous heap contain the same handles;
    // removals update both structures immediately. Every surviving handle's
    // inverse position is overwritten below, so clearing the old heap first
    // was a redundant second pass over the hottest dense-stealing population.
    stealVolatileHeapCount_ = 0u;
    bool vectorized = false;
#if !defined(SVMS_XP_COMPAT)
    if (stealKeyBackend_ == RenderBackend::AVX2 &&
        stealVolatileCount_ >= 8u) {
        vectorized = BuildVolatileStealKeysAVX2(
            stealVolatileList_, stealVolatileCount_, v.birthFrame,
            v.currentGain, v.stealOutputGain, activePosition_, currentFrame_,
            kBassMidiStealGainScale, stealVolatileHeapKey_,
            stealVolatileHeapHandle_, stealVolatileHeapPosition_);
    }
#endif
    if (!vectorized) {
        const float commonAgeScore =
            static_cast<float>(currentFrame_) * (1.0f / 256.0f);
        for (uint32_t position = 0; position < stealVolatileCount_; ++position) {
            const uint32_t handle = stealVolatileList_[position];
            // Volatile means active decay or releasing, so its effective
            // level is always currentGain * outputGain. Keep the original
            // arithmetic order so ties and floating-point rounding remain
            // unchanged.
            const uint64_t rawAge = currentFrame_ > v.birthFrame[handle]
                ? currentFrame_ - v.birthFrame[handle] : 0u;
            const uint32_t age = rawAge > UINT32_MAX
                ? UINT32_MAX : static_cast<uint32_t>(rawAge);
            const float ageUnits =
                static_cast<float>(age) * (1.0f / 256.0f);
            const float effectiveLevel = std::fabs(v.currentGain[handle]) *
                v.stealOutputGain[handle];
            const float score = ageUnits -
                effectiveLevel * kBassMidiStealGainScale;
            stealVolatileHeapKey_[position] = EncodeStableWinnerKey(
                score - commonAgeScore, activePosition_[handle]);
            stealVolatileHeapHandle_[position] = handle;
            stealVolatileHeapPosition_[handle] = position;
        }
    }
    stealVolatileHeapCount_ = stealVolatileCount_;
    if (stealVolatileHeapCount_ > 1u) {
        for (uint32_t position = stealVolatileHeapCount_ / 2u;
             position-- > 0u;) VolatileHeapSiftDown(position);
    }
    stealVolatileHeapFrame_ = currentFrame_;
    stealVolatileHeapValid_ = true;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    if (profileThisBuild) {
        ++volatileHeapProfileBuilds_;
        ++volatileHeapProfileSamples_;
        volatileHeapProfileCycles_ += __rdtsc() - profileBegin;
        volatileHeapProfileCandidates_ += stealVolatileHeapCount_;
    }
#endif
}

inline void VoiceManager::RemoveVolatileHeapCandidate(VoiceHandle handle) {
    if (!stealVolatileHeapValid_ || handle >= maxVoices_) return;
    const uint32_t position = stealVolatileHeapPosition_[handle];
    if (position >= stealVolatileHeapCount_) return;
    const uint32_t last = --stealVolatileHeapCount_;
    stealVolatileHeapPosition_[handle] = UINT32_MAX;
    if (position != last) {
        stealVolatileHeapKey_[position] = stealVolatileHeapKey_[last];
        const uint32_t movedHandle = stealVolatileHeapHandle_[last];
        stealVolatileHeapHandle_[position] = movedHandle;
        stealVolatileHeapPosition_[movedHandle] = position;
        VolatileHeapSiftUp(position);
        const uint32_t adjusted = stealVolatileHeapPosition_[movedHandle];
        VolatileHeapSiftDown(adjusted);
    }
}

inline void VoiceManager::RemoveReservedVolatileRoot(VoiceHandle handle) {
    assert(handle < maxVoices_ && stealVolatileHeapValid_ &&
           stealVolatileHeapCount_ != 0u &&
           stealVolatileHeapHandle_[0] == handle &&
           stealVolatileHeapPosition_[handle] == 0u);

    const uint32_t lastHeap = --stealVolatileHeapCount_;
    stealVolatileHeapPosition_[handle] = UINT32_MAX;
    if (lastHeap != 0u) {
        stealVolatileHeapKey_[0] = stealVolatileHeapKey_[lastHeap];
        const uint32_t moved = stealVolatileHeapHandle_[lastHeap];
        stealVolatileHeapHandle_[0] = moved;
        stealVolatileHeapPosition_[moved] = 0u;
        VolatileHeapSiftDown(0u);
    }

    const uint32_t listPosition = stealVolatilePosition_[handle];
    assert(listPosition < stealVolatileCount_);
    const uint32_t lastList = --stealVolatileCount_;
    if (listPosition != lastList) {
        const uint32_t moved = stealVolatileList_[lastList];
        stealVolatileList_[listPosition] = moved;
        stealVolatilePosition_[moved] = listPosition;
    }
    stealVolatilePosition_[handle] = UINT32_MAX;
}

inline void VoiceManager::RemoveStealCandidate(VoiceHandle handle) {
    if (handle >= maxVoices_) return;
    UnlinkVolatileCandidate(handle);
    const uint32_t leaf = stealTreeLeafBase_ + handle;
    if (stealWinnerTree_[leaf] == 0u) return;
    stealWinnerTree_[leaf] = 0u;
    assert(stealHeapCount_ > 0u);
    --stealHeapCount_;
    RefreshStealWinnerPath(handle);
}

// Consume (or peek) the best Releasing entry from the ring fast path.
// Ring contents are hints: a voice may have been retired, re-stolen
// through the normal tiers, or become a deferred replacement after its
// entry was pushed. Stale entries are dropped (oldest-first) without
// repairing the rest of the ring mid-search; live pool state is the only
// source of truth. Instead of popping the head unconditionally (pure FIFO),
// a bounded 12-entry window starting at the head is scanned and the single
// best victim is selected with ComputeStableStealKey, compared exactly like
// the winner-tree root (largest encoded key wins), so quality ranking
// survives the O(1)-ish fast path. With consume=false nothing is mutated on
// success, so callers can predict the fast-path outcome before committing
// to it. If no valid entry exists in the entire ring, kInvalidVoice is
// returned and callers fall through to the exact winner-tree path.
inline VoiceHandle VoiceManager::NextValidReleasingRingVictim(
    uint32_t& victimPosition, const bool consume) {
    if (releasingRingCapacity_ == 0u) return kInvalidVoice;
    const auto releasingState = static_cast<uint8_t>(VoiceState::Releasing);

    // Bounded quality window: up to 12 entries from the head, non-mutating.
    VoiceHandle bestHandle = kInvalidVoice;
    uint32_t bestPosition = 0u;
    uint64_t bestKey = 0u;
    uint32_t bestSlot = 0u;
    {
        const uint32_t window =
            releasingRingCount_ < 12u ? releasingRingCount_ : 12u;
        for (uint32_t i = 0u; i < window; ++i) {
            const uint32_t slot =
                (releasingRingHead_ + i) & releasingRingMask_;
            const uint32_t handle = releasingRing_[slot];
            if (handle >= maxVoices_) continue; // stale hint
            const uint32_t position = activePosition_[handle];
            const bool valid = handle < maxVoices_ &&
                v.state[handle] == releasingState &&
                position < activeCount_ && activeList_[position] == handle &&
                stealCandidateDeferred_[handle] == 0u &&
                stealCandidateReserved_[handle] == 0u;
            if (!valid) continue; // stale: skipped, dropped below if at head
            // Same comparison direction as the winner-tree combine and the
            // volatile fallback scan: largest encoded key is the victim.
            const uint64_t key = EncodeStableWinnerKey(
                ComputeStableStealKey(static_cast<VoiceHandle>(handle)),
                position);
            if (bestHandle == kInvalidVoice || key > bestKey) {
                bestHandle = static_cast<VoiceHandle>(handle);
                bestPosition = position;
                bestKey = key;
                bestSlot = slot;
            }
        }
    }
    if (bestHandle != kInvalidVoice) {
        if (!consume) {
            victimPosition = bestPosition;
            return bestHandle;
        }
        // Remove the winner: if it is not at the head, swap it with the
        // head entry (avoids shifting the ring), then pop the head.
        if (bestSlot != releasingRingHead_) {
            releasingRing_[bestSlot] = releasingRing_[releasingRingHead_];
        }
        releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
        --releasingRingCount_;
        victimPosition = bestPosition;
        // Stale-drop: drain any stale entries the swap/win left at the head
        // (mirrors the old search-time stale-drop; never drops a live voice).
        while (releasingRingCount_ != 0u) {
            const uint32_t head = releasingRing_[releasingRingHead_];
            if (head < maxVoices_) {
                const uint32_t position = activePosition_[head];
                const bool valid = v.state[head] == releasingState &&
                    position < activeCount_ &&
                    activeList_[position] == head &&
                    stealCandidateDeferred_[head] == 0u &&
                    stealCandidateReserved_[head] == 0u;
                if (valid) break;
            }
            releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
            --releasingRingCount_;
        }
        return bestHandle;
    }

    // Window (and possibly ring beyond it) holds no quality candidate:
    // fall back to the original oldest-first drain so the caller falls
    // through to the winner-tree path when the ring is fully stale.
    while (releasingRingCount_ != 0u) {
        const uint32_t handle = releasingRing_[releasingRingHead_];
        if (handle >= maxVoices_) {
            // Stale entry: drop it and keep scanning.
            releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
            --releasingRingCount_;
            continue;
        }
        const uint32_t position = activePosition_[handle];
        const bool valid = handle < maxVoices_ &&
            v.state[handle] == releasingState &&
            position < activeCount_ && activeList_[position] == handle &&
            stealCandidateDeferred_[handle] == 0u &&
            stealCandidateReserved_[handle] == 0u;
        if (valid) {
            if (!consume) return static_cast<VoiceHandle>(handle);
            releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
            --releasingRingCount_;
            victimPosition = position;
            return static_cast<VoiceHandle>(handle);
        }
        // Stale entry: drop it and keep scanning.
        releasingRingHead_ = (releasingRingHead_ + 1u) & releasingRingMask_;
        --releasingRingCount_;
    }
    return kInvalidVoice;
}

inline bool VoiceManager::ReleasingRingEligible() const {
    return enableReleasingRing_ && freeTop_ == 0u &&
           releasingRingCapacity_ != 0u && releasingRingCount_ != 0u &&
           activeCount_ * 20u >= voiceLimit_ * 17u;
}

// Unit-test oracle mirroring AllocateVoiceOrSteal's selection stage.
inline VoiceHandle VoiceManager::PredictStealVictimForTest(
    uint32_t& bestPos) {
    if (ReleasingRingEligible()) {
        const VoiceHandle ringVictim =
            NextValidReleasingRingVictim(bestPos, false);
        if (ringVictim != kInvalidVoice) return ringVictim;
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    return FindStealVictimExhaustiveForTest();
#else
    return kInvalidVoice;
#endif
}

inline bool VoiceManager::SetPhaseRotationMode(uint32_t mode) {
    if (mode > 4u) mode = 0u;
    if (mode == phaseRotationMode_) return true;
    phaseRotationMode_ = mode;
    if (mode == 0u) {
        // Coherent: free the state entirely — every render path then takes
        // its original bit-exact code path (v.rot == nullptr).
        v.ReleaseRotation();
        return true;
    }
    if (!v.ReserveRotation(maxVoices_)) {
        phaseRotationMode_ = 0u;
        return false;
    }
    // Seed every currently live voice so a mid-playback mode switch never
    // produces unrotated (or zero-state) filter output.
    for (uint32_t position = 0u; position < activeCount_; ++position)
        SeedVoiceRotationForVoice(activeList_[position]);
    return true;
}

inline void VoiceManager::SeedVoiceRotationForVoice(VoiceHandle handle) {
    if (phaseRotationMode_ == 0u || v.rot == nullptr ||
        handle >= maxVoices_) {
        return;
    }
    const uint64_t seed = MakeVoiceRotationSeed(
        v.channel[handle], v.note[handle], handle, v.birthFrame[handle],
        ++rotationSeedCounter_);
    SeedVoiceRotation(v.rot[handle], phaseRotationMode_, seed,
                      static_cast<float>(sampleRate_));
}

inline VoiceHandle VoiceManager::AllocateVoice(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (voiceLimit_ == 0u || activeCount_ >= voiceLimit_ || freeTop_ == 0u)
        return kInvalidVoice;
    uint32_t idx = freeStack_[--freeTop_];

    InitializeVoice(static_cast<VoiceHandle>(idx), channel, note, velocity);
    SeedVoiceRotationForVoice(static_cast<VoiceHandle>(idx));
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
                                                        bool reserveCandidateInPlace,
                                                        VoiceHandle preselectedVictim,
                                                        uint32_t preselectedVictimPosition) {
    // While a lowered cap is still draining its forced-release tails, do not
    // admit replacement notes that would turn those tails back into primaries.
    // The transition is bounded by the forced release (normally 50 ms).
    if (voiceLimit_ == 0u || activeCount_ > voiceLimit_) {
        if (outStolen) *outStolen = false;
        return kInvalidVoice;
    }

    VoiceHandle vh = kInvalidVoice;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t freeLifecycleBegin = BeginLaunchStageForTest();
#endif
    if (deferCandidate && freeTop_ != 0u && activeCount_ < voiceLimit_) {
        const uint32_t idx = static_cast<uint32_t>(freeStack_[--freeTop_]);
        vh = static_cast<VoiceHandle>(idx);
        InitializePreparedVoice(vh, channel, note, velocity);
        SeedVoiceRotationForVoice(vh);
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
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::Lifecycle,
                              freeLifecycleBegin);
        if (launchTestContext_.active)
            ++launchChurnStats_.freeSlotAllocations;
#endif
        stealCandidateDeferred_[vh] = deferCandidate ? 1u : 0u;
        if (outStolen) *outStolen = false;
        return vh;
    }

    // Physical pool or logical live cap is full — find the lowest-priority
    // voice to steal. Replacement happens in-place, so the live cap cannot
    // grow past its configured value.
    uint32_t bestPos = preselectedVictimPosition;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t selectionBegin = BeginLaunchStageForTest();
#endif
    // A preselected victim (batched per-launch layer allocation) was produced
    // by the caller's PopStealCandidates call — the exact reserve=false pop
    // sequence this site would perform — with full ring/tree/heap repair, so
    // only the redundant re-selection call is skipped here; the victim is
    // identical and the post-selection logic below is untouched.
    const VoiceHandle bestHandle = preselectedVictim != kInvalidVoice
        ? preselectedVictim
        : PopStealCandidate(bestPos,
                            deferCandidate && reserveCandidateInPlace);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::VictimSelection,
                          selectionBegin);
#endif
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
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    RecordVictimGroupForTest(bestHandle, LaunchVictimPath::General, false);
#endif
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
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t lifecycleBegin = BeginLaunchStageForTest();
#endif
    ++stealCount_;
    if (v.state[bestIdx] == static_cast<uint8_t>(VoiceState::Releasing))
        releasingCount_.fetch_sub(1u, std::memory_order_relaxed);
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
    if (!deferCandidate) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::Lifecycle,
                              lifecycleBegin);
        const uint64_t treeBegin = BeginLaunchStageForTest();
#endif
        PushStealCandidate(static_cast<VoiceHandle>(bestIdx), bestPos);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance,
                              treeBegin);
#endif
    } else {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::Lifecycle,
                              lifecycleBegin);
#endif
    }

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
                const float score = ComputeStableStealKey(handle);
                stealStableKey_[handle] = EncodeStableWinnerKey(
                    score, activePosition_[handle]);
                stealWinnerTree_[stealTreeLeafBase_ + handle] =
                    stealStableKey_[handle];
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
            stealVolatileHeapKey_[heapPosition] = EncodeStableWinnerKey(
                ComputeStableStealKey(handle), activePosition_[handle]);
            stealVolatileHeapHandle_[heapPosition] = handle;
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
        v.heldBySustain[handle] = 0u;
        v.heldBySostenuto[handle] = 0u;
        v.state[handle] = static_cast<uint8_t>(VoiceState::Releasing);
        releasingCount_.fetch_add(1u, std::memory_order_relaxed);
        // Track the voice in the Releasing-ring fast-path index. Entries are
        // validated lazily when consumed; if a voice leaves Releasing by any
        // other route its entry simply goes stale and is skipped later.
        if (releasingRingCapacity_ != 0u) {
            if (releasingRingCount_ == releasingRingCapacity_) {
                // Overwrite the oldest entry on overflow; bounded memory,
                // correctness never depends on ring contents.
                releasingRingHead_ =
                    (releasingRingHead_ + 1u) & releasingRingMask_;
                --releasingRingCount_;
            }
            releasingRing_[(releasingRingHead_ + releasingRingCount_) &
                           releasingRingMask_] =
                static_cast<uint32_t>(handle);
            ++releasingRingCount_;
        }
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

    const bool wasReleasing =
        v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing);
    if (wasReleasing)
        releasingCount_.fetch_sub(1u, std::memory_order_relaxed);

    // Dense chopped streams interleave volatile retirement with immediate
    // full-pool launches. Invalidating here made the next steal rebuild the
    // complete candidate index, turning ordinary churn into repeated O(V)
    // spikes. Remove the retiring candidate and repair only the swapped
    // handle's path; both stable and volatile indices support this exactly.
    const bool maintainStealIndex = stealHeapValid_;
    if (maintainStealIndex)
        RemoveStealCandidate(handle);
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
    ConfigureVoice(handle, setup, setup.playIndex, cp, commitDeferred);
}

inline void VoiceManager::ConfigureVoice(
    VoiceHandle handle, const VoiceConfiguration& setup, uint32_t playIndex,
    const ChannelParamsSnapshot& cp, bool commitDeferred) {
    if (handle >= maxVoices_) return;

    SetVoicePlayIndex(handle, playIndex);
    ApplyVoiceConfigurationFields(handle, setup, cp);
    RefreshRenderClass(handle);
    if (commitDeferred)
        CommitVoiceConfiguration(handle);
    else
        UpdateStealCandidate(handle);
    if (voiceConfiguredHook_)
        voiceConfiguredHook_(handle, voiceConfiguredUserData_);
}

inline void VoiceManager::ApplyVoiceConfigurationFields(
    VoiceHandle handle, const VoiceConfiguration& setup,
    const ChannelParamsSnapshot& cp, VoiceRenderClass knownClass) {

    v.sampleStart[handle] = setup.sampleStart;
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
    const bool knownSustained =
        knownClass == VoiceRenderClass::SustainedLoop ||
        knownClass == VoiceRenderClass::SustainedOneShot;
    v.loopEnabled[handle] = knownSustained
        ? static_cast<uint8_t>(
            knownClass == VoiceRenderClass::SustainedLoop)
        : static_cast<uint8_t>(
            (setup.loopMode == 1u || setup.loopMode == 3u) &&
            setup.loopEnd > setup.loopStart + 1u);

    v.presetIndex[handle] = setup.presetIndex;
    v.regionIndex[handle] = setup.regionIndex;
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
    if (knownSustained) {
        v.envelopeStage[handle] = 3u;
        v.currentGain[handle] = setup.initialGain;
    } else if (setup.delaySamples > 0u) {
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
    v.vibLfoToPitchCents[handle] = setup.vibLfoToPitchCents;
    v.vibLfoSteps[handle] = setup.vibLfoPhaseStep;
    v.vibLfoPhases[handle] = 0.0f;
    v.vibLfoDelays[handle] = setup.vibLfoDelaySamples;
    v.vibLfoModulated[handle] = 0u;
    v.mixGainL[handle] = setup.gainLeft * cp.mixScaleLeft;
    v.mixGainR[handle] = setup.gainRight * cp.mixScaleRight;
    v.stealOutputGain[handle] = std::sqrt(
        v.mixGainL[handle] * v.mixGainL[handle] +
        v.mixGainR[handle] * v.mixGainR[handle]);
    v.renderGainL[handle] = v.currentGain[handle] * v.mixGainL[handle];
    v.renderGainR[handle] = v.currentGain[handle] * v.mixGainR[handle];
}

inline VoiceRenderClass VoiceManager::ClassifyConfiguration(
    const VoiceConfiguration& setup) {
    if (setup.sampleBacked == 0u ||
        setup.sampleEnd <= setup.sampleStart + 1u)
        return VoiceRenderClass::Generic;
    const bool loop =
        (setup.loopMode == 1u || setup.loopMode == 3u) &&
        setup.loopEnd > setup.loopStart + 1u;
    uint8_t stage = 3u;
    if (setup.delaySamples != 0u) stage = 4u;
    else if (setup.holdSamples != 0u) stage = 0u;
    else if (setup.attackSamples != 0u) stage = 1u;
    else if (setup.decaySamples != 0u) stage = 2u;
    if (stage == 3u)
        return loop ? VoiceRenderClass::SustainedLoop
                    : VoiceRenderClass::SustainedOneShot;
    if (loop && (stage == 1u || stage == 2u))
        return VoiceRenderClass::TransientLoop;
    return VoiceRenderClass::Generic;
}

inline bool VoiceManager::IsStableConfiguration(
    const VoiceConfiguration& setup) {
    const bool preDecay = setup.delaySamples != 0u ||
        setup.holdSamples != 0u || setup.attackSamples != 0u;
    return setup.sampleBacked != 0u &&
        setup.sampleEnd > setup.sampleStart + 1u &&
        (preDecay || setup.decaySamples == 0u);
}

inline float VoiceManager::ComputeNewbornStableStealKey(
    VoiceHandle handle) const {
    const float outputGain = v.stealOutputGain[handle];
    const float effectiveLevel =
        std::fabs(v.targetGain[handle]) * outputGain;
    const float score = 0.0f - effectiveLevel * kBassMidiStealGainScale;
    const float commonAgeScore =
        static_cast<float>(currentFrame_) * (1.0f / 256.0f);
    return score - commonAgeScore;
}

SVMS_VM_FORCEINLINE bool VoiceManager::TryLaunchSingleVoiceInPlace(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration& setup, uint32_t playIndex,
    const ChannelParamsSnapshot& cp, VoiceHandle& outHandle) {
    if (freeTop_ != 0u || !IsStableConfiguration(setup)) return false;

    uint32_t selectedPosition = 0u;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const bool profileLaunch = (++launchProfileCounter_ & 4095u) == 0u;
    const uint64_t profileBegin = profileLaunch ? __rdtsc() : 0u;
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t testSelectionBegin = BeginLaunchStageForTest();
#endif
    // Releasing-ring fast path: under release-storm churn the oldest
    // releasing voice is an acceptable BASSMIDI-consistent victim, exactly
    // like PopStealCandidate's fast path. Peek first so grouped voices are
    // left untouched (they need the atomic group retirement path below).
    uint32_t ringPeekPosition = 0u;
    VoiceHandle ringPeek = kInvalidVoice;
    if (ReleasingRingEligible())
        ringPeek = NextValidReleasingRingVictim(ringPeekPosition, false);
    const bool fromRing = ringPeek != kInvalidVoice &&
        playGroupPrev_[ringPeek] < 0 && playGroupNext_[ringPeek] < 0;
    VoiceHandle handle = kInvalidVoice;
    if (fromRing) {
        handle = NextValidReleasingRingVictim(selectedPosition, true);
        ++releasingRingHits_;
    } else {
        handle = PopStealCandidate(selectedPosition, true);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::VictimSelection,
                          testSelectionBegin);
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const uint64_t profileAfterPop = profileLaunch ? __rdtsc() : 0u;
#endif
    if (handle == kInvalidVoice) return false;

    const bool volatileVictim = !fromRing &&
        stealCandidateReserved_[handle] == 1u &&
        !IsStableStealCandidate(handle) &&
        stealVolatileHeapPosition_[handle] < stealVolatileHeapCount_;
    const bool eligible = fromRing ||
        ((stealCandidateReserved_[handle] == 2u &&
          IsStableStealCandidate(handle) &&
          stealWinnerTree_[stealTreeLeafBase_ + handle] ==
              stealStableKey_[handle]) ||
         volatileVictim);
    if (!eligible) {
        stealCandidateReserved_[handle] = 0u;
        return false;
    }

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    RecordVictimGroupForTest(handle, LaunchVictimPath::SingleInPlace, true);
#endif

    const VoiceRenderClass desiredClass = ClassifyConfiguration(setup);
    const uint8_t desiredClassValue = static_cast<uint8_t>(desiredClass);
    const bool preserveChannelIndex = v.channel[handle] == channel;
    const bool preserveRenderIndex =
        v.renderClass[handle] == desiredClassValue;

    CaptureStealTail(handle);
    if (fromRing) {
        // Mirror tier-pop bookkeeping: unlink the victim from the volatile
        // candidate index it still occupies.
        RemoveStealCandidate(handle);
    } else if (volatileVictim) {
        stealCandidateReserved_[handle] = 0u;
        RemoveReservedVolatileRoot(handle);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const uint64_t profileAfterTail = profileLaunch ? __rdtsc() : 0u;
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t testLifecycleBegin = BeginLaunchStageForTest();
#endif
    ++stealCount_;
    if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
        releasingCount_.fetch_sub(1u, std::memory_order_relaxed);
    UnlinkChannelKey(handle);
    if (!preserveChannelIndex) MoveChannelActiveInPlace(handle, channel);
    if (!preserveRenderIndex) UnlinkRenderClass(handle);

    // This atomic path has no observer between victim removal and complete
    // configuration. Initialize only lifecycle fields that survive the
    // transaction; the generic prepared initializer's placeholder sample,
    // envelope, class, links, and gains would all be overwritten below.
    v.state[handle] = static_cast<uint8_t>(VoiceState::Active);
    if (preserveChannelIndex) v.channel[handle] = channel;
    v.note[handle] = note;
    v.velocity[handle] = velocity;
    v.heldBySustain[handle] = 0u;
    v.heldBySostenuto[handle] = 0u;
    v.releaseStartInBlock[handle] = 0u;
    v.birthFrame[handle] = currentFrame_;
    v.stealFadeInFramesRemaining[handle] = 0u;
    v.stealFadeInFramesTotal[handle] = 0u;
    LinkChannelKey(handle);

    // A playIndex is unique to this MIDI note-on. The old group has already
    // been unlinked, so SetVoicePlayIndex's second unlink is unnecessary.
    v.playIndex[handle] = playIndex;
    lastLinkedPlayIndex_ = playIndex;
    lastLinkedPlayVoice_ = handle;

#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const uint64_t profileAfterLifecycle = profileLaunch ? __rdtsc() : 0u;
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Lifecycle,
                          testLifecycleBegin);
    const uint64_t testConfigureBegin = BeginLaunchStageForTest();
#endif

    ApplyVoiceConfigurationFields(handle, setup, cp, desiredClass);
    if (preserveRenderIndex) {
        v.renderClass[handle] = desiredClassValue;
    } else {
        LinkRenderClass(handle);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    const uint64_t profileAfterConfigure = profileLaunch ? __rdtsc() : 0u;
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Configuration,
                          testConfigureBegin);
    const uint64_t testTreeBegin = BeginLaunchStageForTest();
#endif

    // The stable replacement retains its leaf; a volatile victim vacated the
    // heap and inserts that same physical handle as a new stable leaf.
    // Ring-served victims likewise occupied a volatile-tier slot before.
    stealCandidateReserved_[handle] = 0u;
    const float score = ComputeNewbornStableStealKey(handle);
    stealStableKey_[handle] = EncodeStableWinnerKey(
        score, activePosition_[handle]);
    stealWinnerTree_[stealTreeLeafBase_ + handle] = stealStableKey_[handle];
    if (volatileVictim || fromRing) ++stealHeapCount_;
    RefreshStealWinnerPath(handle);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance,
                          testTreeBegin);
#endif
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
    if (profileLaunch) {
        const uint64_t profileEnd = __rdtsc();
        ++launchProfileSamples_;
        launchProfilePopCycles_ += profileAfterPop - profileBegin;
        launchProfileTailCycles_ += profileAfterTail - profileAfterPop;
        launchProfileLifecycleCycles_ +=
            profileAfterLifecycle - profileAfterTail;
        launchProfileConfigureCycles_ +=
            profileAfterConfigure - profileAfterLifecycle;
        launchProfileTreeCycles_ += profileEnd - profileAfterConfigure;
    }
#endif
    outHandle = handle;
    if (voiceConfiguredHook_)
        voiceConfiguredHook_(handle, voiceConfiguredUserData_);
    return true;
}

inline bool VoiceManager::ReuseMatchingStealGroup(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count,
    VoiceHandle* outHandles, bool& candidatesReservedInPlace) {
    candidatesReservedInPlace = false;
    if (freeTop_ != 0u || count == 0u || count > maxVoices_ || !setups ||
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
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t selectionBegin = BeginLaunchStageForTest();
#endif
    VoiceHandle selected = PopStealCandidate(selectedPosition, true);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::VictimSelection,
                          selectionBegin);
#endif
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
        canReserveInPlace = IsStableConfiguration(setup) &&
            IsStableStealCandidate(handle) &&
            stealWinnerTree_[stealTreeLeafBase_ + handle] ==
                stealStableKey_[handle];
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
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        const uint64_t treeBegin = BeginLaunchStageForTest();
#endif
        const bool selectedVolatileRoot =
            stealCandidateReserved_[selected] == 1u &&
            stealVolatileHeapValid_ && stealVolatileHeapCount_ != 0u &&
            stealVolatileHeapHandle_[0] == selected &&
            stealVolatileHeapPosition_[selected] == 0u;
        stealCandidateReserved_[selected] = 0u;
        if (selectedVolatileRoot)
            RemoveReservedVolatileRoot(selected);
        else
            RemoveStealCandidate(selected);
        for (uint32_t i = 1u; i < count; ++i)
            RemoveStealCandidate(outHandles[i]);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance,
                              treeBegin);
#endif
    }

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    RecordVictimGroupForTest(selected, LaunchVictimPath::MatchingReuse,
                             candidatesReservedInPlace);
#endif

    // Capture every old layer, then replace the complete physical note in
    // place. The fast path keeps its winner-tree leaves reserved; the fallback
    // removed them above. Active positions and the free stack never move.
    for (uint32_t i = 0u; i < count; ++i)
        CaptureStealTail(outHandles[i]);

#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t lifecycleBegin = BeginLaunchStageForTest();
#endif
    for (uint32_t i = 0u; i < count; ++i) {
        const VoiceHandle handle = outHandles[i];
        // Steal-index reservation and channel-list membership are independent.
        // Replacing a grouped voice on the same channel can retain its exact
        // dense channel slot even when the victim came from the volatile heap.
        const bool preserveChannelIndex = v.channel[handle] == channel;
        const VoiceRenderClass desiredClass =
            ClassifyConfiguration(setups[i]);
        const uint8_t configuredClass = static_cast<uint8_t>(desiredClass);
        const bool preserveRenderIndex = candidatesReservedInPlace &&
            v.renderClass[handle] == configuredClass;
        ++stealCount_;
        if (v.state[handle] == static_cast<uint8_t>(VoiceState::Releasing))
            releasingCount_.fetch_sub(1u, std::memory_order_relaxed);
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
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Lifecycle, lifecycleBegin);
#endif
    return true;
}

inline void VoiceManager::CommitVoiceGroupConfigurations(
    const VoiceHandle* handles, uint32_t count,
    bool candidatesReservedInPlace) {
    if (!candidatesReservedInPlace) {
        // A prepared SF2 note becomes visible atomically. When every layer is
        // a stable candidate, publish all leaves first and repair the union of
        // their tree paths once. No event can observe an intermediate layer,
        // and the resulting root is identical to individual commits.
        bool canBatchStable = stealHeapValid_ && count <= 8u;
        for (uint32_t layer = 0u; layer < count && canBatchStable; ++layer) {
            const VoiceHandle handle = handles[layer];
            canBatchStable = handle < maxVoices_ &&
                stealCandidateDeferred_[handle] != 0u &&
                stealCandidateReserved_[handle] == 0u &&
                IsStableStealCandidate(handle);
        }
        if (canBatchStable) {
            for (uint32_t layer = 0u; layer < count; ++layer) {
                const VoiceHandle handle = handles[layer];
                stealCandidateDeferred_[handle] = 0u;
                ++stealHeapCount_;
                const float score = ComputeStableStealKey(handle);
                stealStableKey_[handle] = EncodeStableWinnerKey(
                    score, activePosition_[handle]);
                stealWinnerTree_[stealTreeLeafBase_ + handle] =
                    stealStableKey_[handle];
            }
            RefreshStealWinnerPaths(handles, count);
            return;
        }
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
            stealWinnerTree_[stealTreeLeafBase_ + handle] ==
                stealStableKey_[handle];
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
        const float score = ComputeStableStealKey(handle);
        stealStableKey_[handle] = EncodeStableWinnerKey(
            score, activePosition_[handle]);
        stealWinnerTree_[stealTreeLeafBase_ + handle] =
            stealStableKey_[handle];
    }
    RefreshStealWinnerPaths(handles, count);
}

inline bool VoiceManager::LaunchVoiceGroup(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count,
    const ChannelParamsSnapshot& channelParams, VoiceHandle* outHandles) {
    const uint32_t playIndex = setups && count != 0u
        ? setups[0].playIndex : UINT32_MAX;
    return LaunchVoiceGroup(channel, note, velocity, setups, count, playIndex,
                            channelParams, outHandles);
}

inline bool VoiceManager::LaunchVoiceGroup(
    uint8_t channel, uint8_t note, uint8_t velocity,
    const VoiceConfiguration* setups, uint32_t count, uint32_t playIndex,
    const ChannelParamsSnapshot& channelParams, VoiceHandle* outHandles) {
    if (!setups || !outHandles || count == 0u || count > maxVoices_ ||
        count > voiceLimit_)
        return false;
    if (count > maxLaunchGroupSize_) maxLaunchGroupSize_ = count;
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    BeginLaunchTestProfile(channel, note, setups, count);
#endif
    if (count == 1u && TryLaunchSingleVoiceInPlace(
            channel, note, velocity, setups[0], playIndex, channelParams,
            outHandles[0])) {
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
        ++groupReuseAttemptCount_;
        ++groupReuseMatchCount_;
        ++groupReuseReservedCount_;
        FinishLaunchTestProfile(true);
#endif
        return true;
    }
    bool candidatesReservedInPlace = false;
    if (!ReuseMatchingStealGroup(channel, note, velocity, setups, count,
                                 outHandles, candidatesReservedInPlace)) {
        // ── FLAG (steal-batch scope) ─────────────────────────────────────
        // The victim batching is deliberately scoped to ONE launch
        // transaction: between the first and last layer selection no
        // candidate insertion can occur (commits land only after this loop),
        // so the descending victim order of PopStealCandidates is identical
        // to the sequential per-layer PopStealCandidate pops — only the
        // per-layer call overhead is saved. Prefetching victims ACROSS notes
        // of a same-frame dispatch run is NOT exact: newborn commits of
        // earlier notes re-key their slots and can outrank remaining
        // prefetched victims, which changes WHICH voice is stolen (confirmed
        // empirically during bring-up). Any future run-level scheme must
        // solve that insertion race first.
        VoiceHandle batchVictims[kStealBatchMaxLayers];
        uint32_t batchPositions[kStealBatchMaxLayers];
        bool consumed[kStealBatchMaxLayers] = {};
        uint32_t popped = 0u;
        const bool batchSteal = stealBatchingEnabled_ &&
            count >= 2u && count <= kStealBatchMaxLayers &&
            // Releasing-ring-ordered victims cannot be re-armed (the ring
            // entry is consumed and there is no front-repush), so the batch
            // only engages while the ring fast path is inactive; once
            // inactive it stays inactive for the rest of the launch (sibling
            // retirements only move freeTop_/activeCount_ further from
            // eligibility and dispatch never pushes ring entries).
            !ReleasingRingEligible();
        if (batchSteal)
            popped = PopStealCandidates(count, batchVictims, batchPositions);
        uint32_t allocated = 0u;
        uint32_t cursor = 0u;
        for (; allocated < count; ++allocated) {
            // Mirror AllocateVoiceOrSteal's free-stack-vs-steal decision:
            // only hand a prefetched victim to a layer that is guaranteed to
            // take the steal path (the free-slot branch cannot fire), and
            // skip victims already retired as siblings of an earlier layer's
            // victim (state Free).
            while (cursor < popped &&
                   v.state[batchVictims[cursor]] !=
                       static_cast<uint8_t>(VoiceState::Active))
                ++cursor;
            VoiceHandle fed = kInvalidVoice;
            uint32_t fedPosition = 0u;
            uint32_t fedCursor = UINT32_MAX;
            if (freeTop_ == 0u && activeCount_ >= voiceLimit_ &&
                cursor < popped) {
                fed = batchVictims[cursor];
                fedPosition = batchPositions[cursor];
                fedCursor = cursor;
                ++cursor;
            }
            outHandles[allocated] = AllocateVoiceOrSteal(
                channel, note, velocity, nullptr, true, false, fed,
                fedPosition);
            if (outHandles[allocated] == kInvalidVoice) {
                // Restore the unconsumed, still-live prefetched victims so
                // the candidate index matches the per-layer path exactly,
                // then roll the layers launched so far back.
                RearmLiveBatchVictims(batchVictims, popped, consumed);
                for (uint32_t rollback = 0u; rollback < allocated; ++rollback)
                    RetireVoice(outHandles[rollback]);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
                FinishLaunchTestProfile(false);
#endif
                return false;
            }
            if (fedCursor != UINT32_MAX) consumed[fedCursor] = true;
        }
        // The launch consumed its fed victims; every other popped victim
        // must rejoin the candidate index (the per-layer path would never
        // have popped it).
        RearmLiveBatchVictims(batchVictims, popped, consumed);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    const uint64_t configureBegin = BeginLaunchStageForTest();
#endif
    for (uint32_t layer = 0u; layer < count; ++layer) {
        // Keep every layer invisible to the steal index until the complete
        // physical note has been prepared.  The former layered path linked a
        // Generic candidate, configured it, and reclassified it one layer at
        // a time, multiplying tree/list work for stereo SoundFonts.
        ConfigureVoice(outHandles[layer], setups[layer], playIndex,
                       channelParams, false);
    }
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::Configuration,
                          configureBegin);
    const uint64_t treeBegin = BeginLaunchStageForTest();
#endif
    CommitVoiceGroupConfigurations(outHandles, count,
                                   candidatesReservedInPlace);
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
    EndLaunchStageForTest(LaunchProfileStage::TreeMaintenance, treeBegin);
    FinishLaunchTestProfile(true);
#endif
    return true;
}

inline void VoiceManager::SetVoiceSample(VoiceHandle handle, uint32_t start, uint32_t end,
                                          uint32_t loopStart, uint32_t loopEnd, uint8_t loopMode,
                                          float phaseStep, uint8_t sb) {
    if (handle >= maxVoices_) return;
    v.sampleStart[handle] = start;
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
            if (sustain || v.heldBySostenuto[handle]) {
                if (sustain) v.heldBySustain[handle] = 1u;
                // A pedal-held generation has received its note-off and must
                // not mask the next retrigger. Pedal release uses the channel
                // active index, so key-chain membership is unnecessary.
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
        if (sustain || v.heldBySostenuto[handle]) {
            if (sustain) v.heldBySustain[handle] = 1u;
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
    v.mixGainL[handle] = v.gainLeft[handle] * cp.mixScaleLeft;
    v.mixGainR[handle] = v.gainRight[handle] * cp.mixScaleRight;
    v.stealOutputGain[handle] = std::sqrt(
        v.mixGainL[handle] * v.mixGainL[handle] +
        v.mixGainR[handle] * v.mixGainR[handle]);
    v.renderGainL[handle] = v.currentGain[handle] * v.mixGainL[handle];
    v.renderGainR[handle] = v.currentGain[handle] * v.mixGainR[handle];
    UpdateStealCandidate(handle);
}

inline void VoiceManager::RefreshMixGains(const ChannelParamsSnapshot* chParams) {
    bool stableTreeDirty = false;
    for (uint32_t ai = 0; ai < activeCount_; ++ai) {
        uint32_t i = activeList_[ai];
        const ChannelParamsSnapshot& cp = chParams[v.channel[i]];
        v.mixGainL[i] = v.gainLeft[i] * cp.mixScaleLeft;
        v.mixGainR[i] = v.gainRight[i] * cp.mixScaleRight;
        v.stealOutputGain[i] = std::sqrt(
            v.mixGainL[i] * v.mixGainL[i] +
            v.mixGainR[i] * v.mixGainR[i]);
        v.renderGainL[i] = v.currentGain[i] * v.mixGainL[i];
        v.renderGainR[i] = v.currentGain[i] * v.mixGainR[i];
        if (stealHeapValid_ && stealCandidateDeferred_[i] == 0u &&
            IsStableStealCandidate(static_cast<VoiceHandle>(i)) &&
            stealWinnerTree_[stealTreeLeafBase_ + i] ==
                stealStableKey_[i]) {
            const float score = ComputeStableStealKey(
                static_cast<VoiceHandle>(i));
            stealStableKey_[i] = EncodeStableWinnerKey(
                score, activePosition_[i]);
            stealWinnerTree_[stealTreeLeafBase_ + i] = stealStableKey_[i];
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
    bool stableTreeDirty = false;
    ForEachChannelActive(channel, [&](VoiceHandle voice) {
        const uint32_t i = voice;
        v.mixGainL[i] = v.gainLeft[i] * cp.mixScaleLeft;
        v.mixGainR[i] = v.gainRight[i] * cp.mixScaleRight;
        v.stealOutputGain[i] = std::sqrt(
            v.mixGainL[i] * v.mixGainL[i] +
            v.mixGainR[i] * v.mixGainR[i]);
        v.renderGainL[i] = v.currentGain[i] * v.mixGainL[i];
        v.renderGainR[i] = v.currentGain[i] * v.mixGainR[i];
        if (stealHeapValid_ && stealCandidateDeferred_[i] == 0u &&
            IsStableStealCandidate(static_cast<VoiceHandle>(i)) &&
            stealWinnerTree_[stealTreeLeafBase_ + i] ==
                stealStableKey_[i]) {
            const float score = ComputeStableStealKey(
                static_cast<VoiceHandle>(i));
            stealStableKey_[i] = EncodeStableWinnerKey(
                score, activePosition_[i]);
            stealWinnerTree_[stealTreeLeafBase_ + i] = stealStableKey_[i];
            stableTreeDirty = true;
        } else {
            UpdateStealCandidate(static_cast<VoiceHandle>(i));
        }
    });
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
        const uint32_t idx = LastChannelActive(channel);
        assert(idx != kInvalidVoice);
        // Tail ownership is independent from the active slot and was handled
        // by channel identity above.
        v.stealFadeInFramesRemaining[idx] = 0;
        RetireVoice(static_cast<VoiceHandle>(idx));
    }
}

inline void VoiceManager::ReleaseChannel(uint8_t channel, uint32_t blockOffset) {
    if (channel >= kChannelCount) return;
    ForEachChannelActive(channel, [&](VoiceHandle voice) {
        const uint32_t idx = voice;
        if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) return;
        v.heldBySustain[idx] = 0;
        v.heldBySostenuto[idx] = 0;
        v.releaseStartInBlock[idx] = blockOffset;
        StartRelease(static_cast<VoiceHandle>(idx));
    });
}

inline void VoiceManager::CaptureSostenuto(uint8_t channel) {
    if (channel >= kChannelCount) return;
    ForEachChannelActive(channel, [&](VoiceHandle handle) {
        if (v.state[handle] != static_cast<uint8_t>(VoiceState::Active)) return;
        const uint8_t note = v.note[handle];
        const bool awaitingNoteOff =
            channelKeyVoiceHead_[channel][note] == static_cast<int32_t>(handle) ||
            v.prevChannelKeyVoice[handle] >= 0 ||
            v.nextChannelKeyVoice[handle] >= 0;
        if (awaitingNoteOff) v.heldBySostenuto[handle] = 1u;
    });
}

inline void VoiceManager::ReleaseSostenuto(uint8_t channel,
                                            uint32_t blockOffset) {
    if (channel >= kChannelCount) return;
    ForEachChannelActive(channel, [&](VoiceHandle handle) {
        if (!v.heldBySostenuto[handle]) return;
        const uint8_t note = v.note[handle];
        const bool awaitingNoteOff =
            channelKeyVoiceHead_[channel][note] == static_cast<int32_t>(handle) ||
            v.prevChannelKeyVoice[handle] >= 0 ||
            v.nextChannelKeyVoice[handle] >= 0;
        v.heldBySostenuto[handle] = 0u;
        if (!awaitingNoteOff && !v.heldBySustain[handle]) {
            v.releaseStartInBlock[handle] = blockOffset;
            StartRelease(handle);
        }
    });
}

inline void VoiceManager::ReleaseSustain(uint8_t channel,
                                         uint32_t blockOffset) {
    if (channel >= kChannelCount) return;
    ForEachChannelActive(channel, [&](VoiceHandle handle) {
        if (!v.heldBySustain[handle]) return;
        v.heldBySustain[handle] = 0u;
        if (!v.heldBySostenuto[handle]) {
            v.releaseStartInBlock[handle] = blockOffset;
            StartRelease(handle);
        }
    });
}

inline uint32_t VoiceManager::NoteOffOldestPlayIndices(
        uint8_t channel, uint8_t note, uint32_t count, bool sustain,
        uint32_t blockOffset) {
    uint32_t released = 0u;
    while (released < count) {
        const uint32_t playIndex = FindOldestPlayIndex(channel, note);
        if (playIndex == UINT32_MAX) break;
        NoteOffPlayIndex(channel, note, playIndex, sustain, blockOffset);
        ++released;
    }
    return released;
}

} // namespace svms

#undef SVMS_VM_FORCEINLINE
#endif
