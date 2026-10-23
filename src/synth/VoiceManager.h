#ifndef SVMS_VOICE_MANAGER_H
#define SVMS_VOICE_MANAGER_H

#include "synth/Types.h"
#include "render/Envelope.h"
#include "render/PhaseRotation.h"
#include "synth/LiveControl.h"
#include "render/RenderKernels.h"
#include "render/VoiceFilter.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#if defined(SVMS_ENABLE_REFERENCE_RENDERER) && defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#define SVMS_VM_HAS_PREFETCH 1
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif

#if defined(_MSC_VER)
#define SVMS_VM_FORCEINLINE __forceinline
#elif defined(__GNUC__)
#define SVMS_VM_FORCEINLINE inline __attribute__((always_inline))
#else
#define SVMS_VM_FORCEINLINE inline
#endif

namespace svms {

// voices.launch_staging, read when a VoiceManager initializes (restart-applied
// like synth.large_pages).  Off = the production SoA launch path, unchanged.
inline std::atomic<bool> g_launchStagingEnabled{false};
// Experimental (voices.drop_zero_life_victims, env SVMS_DROP_ZERO_LIFE for
// now): a voice stolen on the very frame it was launched has played zero
// samples; skip its ghost, which only contributes the 64-frame steal-fade
// tail of its first sample.  Default off = unchanged audio.
inline std::atomic<bool> g_dropZeroLifeVictims{false};
inline bool DropZeroLifeVictimsEnabled() noexcept {
    static const bool env = std::getenv("SVMS_DROP_ZERO_LIFE") != nullptr;
    return env || g_dropZeroLifeVictims.load(std::memory_order_relaxed);
}
#if defined(SVMS_ENABLE_REFERENCE_RENDERER)
// Test builds: staged launches issued, and blocks that ended with a row still
// staged (must stay 0).
inline std::atomic<uint64_t> g_launchStagingStaged{0};
inline std::atomic<uint64_t> g_launchStagingLeaks{0};
// Lazy same-frame launch: keys written lazily vs keys the commit recomputes
// (must stay 0), and how many lazy steps ran.
inline std::atomic<uint64_t> g_lazyKeyMismatches{0};
inline std::atomic<uint64_t> g_lazyChainSteps{0};
#endif

// Fully prepared hot-path state for one SF2 layer.  Applying this as one
// transaction avoids repeatedly reclassifying the newborn and repairing the
// exact steal index while its fields are still only partially initialized.
// Restart one row's envelope for a re-excite hit (see VoiceManager::ReExciteKey).
// Row-local: the whole-voice workers run it on rows and ghosts at the hit's
// exact frame, the oracle runs it at dispatch. Delay, hold and attack
// are all "not yet at peak" and are left alone; a decaying or sustaining
// voice ramps from its current level back to its peak over `attackFrames`
// (no step) and the decay restarts. Sample phase is untouched.
inline void ExciteVoiceRow(VoiceSoA& v, uint32_t h,
                           uint32_t attackFrames) noexcept {
    const uint8_t stage = v.envelopeStage[h];
    if (stage != 2u && stage != 3u) return;
    const float target = v.targetGain[h];
    const float current = v.currentGain[h];
    const float sustain = v.sustainLevel[h];
    const float slope = v.decaySlope[h];
    // Frames the decay needs to fall from peak back to the sustain level,
    // from the same recurrence the kernels run (gain *= slope).
    uint32_t decayFrames = 0u;
    if (slope > 0.0f && slope < 1.0f && sustain > 0.0f && sustain < target) {
        const float frames = std::log(sustain / target) / std::log(slope);
        decayFrames = frames >= 1.0e9f ? 1000000000u
            : static_cast<uint32_t>(frames) + 1u;
    }
    if (current < target * 0.999f && attackFrames > 0u) {
        v.envelopeStage[h] = 1u;
        v.attackSamplesRemaining[h] = attackFrames;
        v.attackGainStep[h] = (target - current) /
                              static_cast<float>(attackFrames);
        v.decaySamplesRemaining[h] = decayFrames;
    } else {
        // At (or within 0.1% of) the peak: nothing to ramp, the decay just
        // restarts. currentGain stays put, so no write can race a mix op.
        v.decaySamplesRemaining[h] = decayFrames;
        v.envelopeStage[h] = decayFrames > 0u ? 2u : 3u;
    }
}

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
    float filterA0 = 0.0f;
    float filterB1 = 0.0f;
    float filterB2 = 0.0f;
    uint32_t filterEnabled = 0u;
    uint16_t presetIndex = UINT16_MAX;
    uint16_t regionIndex = UINT16_MAX;
    // Membership class for SF2 exclusiveClass / SFZ group.
    uint16_t exclusiveClass = 0;
    // Class stopped by this note. Zero falls back to exclusiveClass so
    // existing SF2/direct callers retain self-choke behavior; UINT16_MAX
    // explicitly disables choking for SFZ group-only regions.
    uint16_t offByClass = 0;
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
// see PhaseRotation.h.  The former post-mix allpass cascade was removed:
// a fixed LTI filter on the mixed signal preserves the magnitude of every
// Fourier component of a periodic signal, so the steady-state dispatch-rate
// hum survived it untouched.  The replacement rotates each voice by an
// independent random constant angle (Hilbert/quadrature form) at note-on,
// which decorrelates the voices (hum sums as √N instead of N) while keeping
// the magnitude spectrum and the sample-exact onset timing intact.
// Coherent (mode 0) remains a bit-exact bypass.
// ════════════════════════════════════════════════════════════════════════

// Per-handle index links the manager rewrites on every steal (play group,
// exclusive class, channel/render index slots, volatile-list position),
// stored AoS: 32 bytes, two voices per cache line, instead of eleven
// separate SoA arrays = eleven victim misses.  Fields scanned or gathered
// across many voices (activePosition_, stealCandidate*, stealStableKey_,
// stealVolatileHeapPosition_) stay SoA.
#define SVMS_VOICE_LINK_FIELDS(X) \
    X(int32_t, playGroupNext_) \
    X(int32_t, playGroupPrev_) \
    X(int32_t, exclusiveClassNext_) \
    X(int32_t, exclusiveClassPrev_) \
    X(uint32_t, channelActiveBlock_) \
    X(uint32_t, stealVolatilePosition_) \
    X(uint8_t, renderClassLinked_) \
    X(uint8_t, channelActiveOffset_) \
    X(uint8_t, exclusiveClass_)

struct alignas(32) VoiceLinkRow {
#define SVMS_DECLARE_LINK_FIELD(type, name) type name;
    SVMS_VOICE_LINK_FIELDS(SVMS_DECLARE_LINK_FIELD)
#undef SVMS_DECLARE_LINK_FIELD
};
static_assert(sizeof(VoiceLinkRow) == 32, "VoiceLinkRow must stay 32 bytes");

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
    void SetStealPolicy(uint32_t policy);
    void SetPerKeyVoiceCap(uint32_t cap) { perKeyVoiceCap_ = cap; }
    // Opt-in (default off) BASSMIDI-style victim-score reject: with a full
    // pool, a single-layer note-on whose steal score is strictly worse than
    // the voice it would replace never launches (no ghost, no configure, the
    // victim keeps playing). Quality steal policy only.
    void SetStealReject(bool on) {
        stealReject_.store(on, std::memory_order_relaxed);
    }
    uint64_t StealRejectCount() const { return stealRejectCount_; }
    // Opt-in (default off) BASSMIDI 2018 victim ranking: the steal score has no age
    // term (KMC 18.2.8's bassmidi.dll ranks voices by level alone, and an equal-level
    // newcomer loses). Pair with SetStealReject. Call before the pool is used.
    void SetStealAgeless(bool on) {
        stealAgeScale_ = on ? 0.0f : (1.0f / 256.0f);
        stealHeapValid_ = false;
        stealVolatileHeapValid_ = false;
    }
    // True when the most recent LaunchVoiceGroup returned false because the
    // reject fired (not an allocation failure).
    bool LastLaunchRejected() const { return lastLaunchRejected_; }
    // Live still-playing voice count for one (channel,note) — test and
    // telemetry readout backing the per-key voice cap.
    uint32_t GetChannelKeyVoiceCount(uint8_t channel, uint8_t note) const {
        return channel < kChannelCount && note < kNoteCount
            ? channelKeyVoiceCount_[channel][note] : 0u;
    }
    using PreTailCaptureHook = void(*)(VoiceHandle handle, void* userData);
    using VoiceConfiguredHook = void(*)(VoiceHandle handle, void* userData);
    VoiceManager();
    VoiceManager(const VoiceManager& other);
    VoiceManager& operator=(const VoiceManager&) = delete;
    ~VoiceManager();
    bool Initialize(uint32_t maxVoices, uint32_t sampleRate = 44100);
    void Reset();
    // Live sample-rate change (ASIO driver rate switch). Affects only
    // voices configured from now on; voices in flight keep the envelope
    // units they were configured with and retire naturally.
    void SetSampleRate(uint32_t sampleRate) {
        if (sampleRate) sampleRate_ = sampleRate;
    }

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
                                     uint32_t preselectedVictimPosition = 0u,
                                     VoiceHandle forcedVictim = kInvalidVoice);
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
                          VoiceHandle* outHandles,
                          uint32_t blockOffset = 0u);
    // `multiplicity` > 1 launches ONE voice group that stands for that many
    // exact-duplicate note-ons (same channel, key, velocity and frame): the
    // per-voice gains carry the factor, which is exact because identical
    // voices sum linearly. Each later note-off releases one unit (see
    // NoteOffOldestPlayIndices); a partial release forks a twin.
    bool LaunchVoiceGroup(uint8_t channel, uint8_t note, uint8_t velocity,
                          const VoiceConfiguration* setups, uint32_t count,
                          uint32_t playIndex,
                          const ChannelParamsSnapshot& channelParams,
                          VoiceHandle* outHandles,
                          uint32_t blockOffset = 0u,
                          uint32_t multiplicity = 1u);

    // ── Merged-voice (multiplicity) support ────────────────────────────
    // A partial release splits a merged voice into a releasing twin (the
    // released units) and the remainder. The twin starts as an exact copy of
    // the source row, so a renderer that only sees rows (the whole-voice
    // pre-pass) can give it the source's start frame and op ingress.
    using VoiceForkedHook = void (*)(VoiceHandle twin, VoiceHandle source,
                                     void* userData);
    void SetVoiceForkedHook(VoiceForkedHook hook, void* userData) noexcept {
        voiceForkedHook_ = hook;
        voiceForkedUserData_ = userData;
    }
    // Length of the re-excite attack ramp (kReExciteAttackMs at the pool's
    // sample rate). Shorter ramps back up from a deep decay sound clicky.
    uint32_t ReExciteAttackFrames() const noexcept {
        const uint32_t frames = static_cast<uint32_t>(
            static_cast<uint64_t>(sampleRate_) * kReExciteAttackMs / 1000u);
        return frames != 0u ? frames : 1u;
    }
    // Note-on re-excite (the lossless-timing answer to note-on coalescing):
    // a repeat hit on a key that still has live voices does not launch a new
    // voice, it restarts those voices' envelopes at the caller's exact frame.
    // The gain ramps from wherever it is back to the voice's peak over
    // `attackFrames` (0 = ReExciteAttackFrames(); no step, so no click) and the
    // decay restarts from there;
    // the sample phase is untouched, so no new attack transient is stacked.
    // Releasing voices are left alone. Returns the number of live voices
    // found (0 = nothing to excite: the caller should launch normally).
    uint32_t ReExciteKey(uint8_t channel, uint8_t note,
                         uint32_t attackFrames = 0u) noexcept;
    uint32_t GetVoiceMultiplicity(VoiceHandle handle) const noexcept {
        return handle < maxVoices_ && v.mult[handle] > 1u ? v.mult[handle] : 1u;
    }
    // Releasing twins forked off merged voices since construction (wraps
    // after 2^31; diagnostics and tests only).
    uint32_t GetForkCount() const noexcept {
        return nextForkPlayIndex_ - 0x80000000u;
    }
    // Partial releases that could not fork (the generation was released in
    // full instead). Zero unless the pool is exhausted.
    uint32_t GetForkFailureCount() const noexcept { return forkFailureCount_; }
    // True once a merged voice has been launched and until a scan proves
    // none remain.
    bool MergedVoicesMayExist() const noexcept { return mergedVoicesMayExist_; }
    // Sum of multiplicities over live voices: how many voices the pool would
    // hold had nothing been merged. Also refreshes MergedVoicesMayExist().
    uint64_t CountEffectiveVoices() noexcept;
    // Rolling estimate of the same sum for big pools: each call visits at
    // most `budget` voices of the active list and `published` holds the total
    // of the last completed pass. Voices that launch or retire mid-pass can
    // be missed or counted twice, so this is a statistic, never a gate.
    struct EffectiveScan {
        uint32_t cursor = 0u;
        uint64_t total = 0u;
        uint64_t published = 0u;
    };
    // `weight(channel, note)` scales each voice (note-on coalescing: how many
    // note-ons the surviving voice stands for); the default weighs 1.
    template <class Weight>
    void AdvanceEffectiveScan(EffectiveScan& scan, uint32_t budget,
                              Weight weight) const noexcept {
        uint32_t end = scan.cursor + budget;
        if (end > activeCount_) end = activeCount_;
        for (uint32_t position = scan.cursor; position < end; ++position) {
            const VoiceHandle h = activeList_[position];
            scan.total += static_cast<uint64_t>(GetVoiceMultiplicity(h)) *
                          weight(v.channel[h], v.note[h]);
        }
        scan.cursor = end;
        if (scan.cursor >= activeCount_) {
            scan.published = scan.total;
            scan.cursor = 0u;
            scan.total = 0u;
        }
    }
    void AdvanceEffectiveScan(EffectiveScan& scan, uint32_t budget) const noexcept {
        AdvanceEffectiveScan(scan, budget,
                             [](uint8_t, uint8_t) noexcept { return 1u; });
    }

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

    // ── Batched (option-B) mix-gain fold ────────────────────────────────
    // O(1) CC-side mark: record the channel's mix scales and set its dirty
    // bits. The per-voice fold runs once per exact-frame batch (vintage-exact
    // boundary folds) instead of once per CC, plus a steal-only fold at
    // launch entry so victim scores stay fresh.
    void MarkChannelMixStale(uint8_t channel, const ChannelParamsSnapshot& cp);
    // Reapply the channel's pitch-bend ratio to every sounding voice (and,
    // under the whole-voice pre-pass, report it as a channel op instead —
    // see SetRowOpHooks).  bendSemitones is the sysex-aware total; the
    // per-voice ratio applies pitchBendScales for scaled regions.
    void ApplyChannelBendRatio(uint8_t channel, float bendSemitones);
    // Full fold at vintage-exact boundaries (block top, exact-frame dispatch
    // boundaries): mixGain, stealOutputGain, renderGain, steal candidates.
    void FoldStaleChannelMixGains();
    // Steal-only variant for launch entry: updates mixGain/stealOutputGain
    // and the steal candidates but NOT renderGain — renderGainL/R embed a
    // currentGain vintage that must only be rewritten at batch boundaries.
    void FoldStaleChannelMixGainsForSteal();
    // Whole-voice renderer post-pass: refresh the steal candidates of the
    // channels touched by row ops, using the scales the op hook recorded.
    // Values are idempotent (the workers already wrote these rows at the op
    // frames with identical scales); only the cached candidate keys move.
    void FoldStealGainsForChannels(uint64_t channelMask);

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
    // Bounds per-launch steal group sizes.
    // Physical allocation ceiling. It can grow at a render boundary; it is
    // never shrunk live so existing voice handles remain stable.
    uint32_t GetMaxVoices() const { return maxVoices_; }
    uint32_t GetLaunchLayersHighWater() const { return launchLayersHighWater_; }
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
    // boundary, before worker jobs can observe voice state.
    void ApplyRuntimeVoiceLimit(uint64_t frame);
    void SetCurrentFrame(uint64_t frame);
    void SetStealKeyBackend(RenderBackend backend) {
        stealKeyBackend_ = backend;
    }
    uint32_t GetVoiceAge(VoiceHandle handle) const;
    // Sum of the BASS-style steal level of every active voice on (channel, note).
    float StealLevelOfKey(uint8_t channel, uint8_t note) const {
        float level = 0.0f;
        ForEachChannelKeyVoice(channel, note, [&](VoiceHandle h) {
            level += ComputeEffectiveStealLevel(h);
        });
        return level;
    }
    uint32_t VoiceCountOfKey(uint8_t channel, uint8_t note) const {
        uint32_t n = 0;
        ForEachChannelKeyVoice(channel, note, [&](VoiceHandle) { ++n; });
        return n;
    }
    uint32_t GetChannelActiveCount(uint8_t channel) const;
    template <typename Consumer>
    void ForEachChannelActive(uint8_t channel, Consumer&& consume) const noexcept;
    // Visits every voice currently linked to (channel, note) through the
    // per-key chain (active voices only — StartRelease unlinks).
    template <typename Consumer>
    void ForEachChannelKeyVoice(uint8_t channel, uint8_t note,
                                Consumer&& consume) const noexcept;
    void InvalidateStealCandidates();
    void MarkStealScoresDirty();
    void RefreshRenderClass(VoiceHandle handle);
    // Launch staging (voices.launch_staging, see VoiceLaunchRecord).  The
    // whole-voice plan switches staging on around its dispatch loop; only the
    // in-place single-voice steal launch stages.  Every plan-time reader or
    // writer of a staged field calls EnsureMaterialized first; the render item
    // that owns the row materializes it before its first kernel read, and
    // RenderWholeVoiceBlock clears the pending flag once the jobs are done.
    bool LaunchStagingStorage() const noexcept { return v.launch != nullptr; }
    void SetLaunchStagingActive(bool active) noexcept {
        launchStagingActive_ = active && v.launch != nullptr;
    }
    // Lazy same-frame launch (needs DropZeroLifeVictimsEnabled): inside a
    // NoteOn run of the whole-voice plan, a launch whose victim group was
    // itself born on this frame replaces nothing physical.  It is parked as a
    // pending note (new steal keys written to the tree) and a following launch
    // that again picks the same group simply overwrites it.  The pending note
    // is committed through the normal launch code the moment anything else
    // happens (see CommitPendingLaunch).
    void SetPlanWindow(bool active) noexcept { planWindow_ = active; }
    bool BeginLazyLaunchRun() noexcept {
        lazyRunActive_ = planWindow_ && DropZeroLifeVictimsEnabled();
        return lazyRunActive_;
    }
    void EndLazyLaunchRun() noexcept {
        CommitPendingLaunch();
        lazyRunActive_ = false;
    }
    void CommitPendingLaunch() {
        if (pendingLaunch_.valid) CommitPendingLaunchImpl();
    }
    uint64_t GetLazyChainSteps() const noexcept { return lazyChainSteps_; }
    uint64_t GetLazyStarts() const noexcept { return lazyStarts_; }
    // Test: the steal index (winner tree + stable keys) matches another pool's.
    bool SameStealIndexAs(const VoiceManager& other) const noexcept {
        return stealTreeLeafBase_ == other.stealTreeLeafBase_ &&
            std::memcmp(stealWinnerTree_, other.stealWinnerTree_,
                        stealTreeLeafBase_ * 2u * sizeof(uint64_t)) == 0 &&
            std::memcmp(stealStableKey_, other.stealStableKey_,
                        maxVoices_ * sizeof(uint64_t)) == 0;
    }
    bool StagedPending() const noexcept { return stagedPending_; }
    void ClearStagedPending() noexcept { stagedPending_ = false; }
    void EnsureMaterialized(VoiceHandle handle) noexcept {
        if (stagedPending_ && handle < maxVoices_ && v.staged[handle] != 0u)
            MaterializeLaunch(handle);
    }
    void MaterializeLaunch(VoiceHandle handle) noexcept;
    // Test/diagnostic: rows still carrying a staged record.
    uint32_t CountStagedRows() const noexcept;
    uint32_t GetRenderClassCount(VoiceRenderClass renderClass) const;
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
    // Whole-voice renderer pre-pass: StartRelease keeps its lifecycle
    // bookkeeping (chain unlink, held-flag clears) but defers the audible
    // state flip to the exact event frame, reported through this hook.  The
    // renderer applies the flip inside the owning worker between spans.
    using DeferredReleaseHook = void (*)(VoiceHandle handle, void* userData);
    // Whole-voice pre-pass: ReExciteKey reports each live voice it finds
    // instead of restarting it, and the renderer restarts the row at the
    // event's exact frame inside the owning worker.
    using ReExciteHook = void (*)(VoiceHandle handle, void* userData);
    void SetReExciteHook(ReExciteHook hook, void* userData) noexcept {
        reExciteHook_ = hook;
        reExciteUserData_ = userData;
    }
    void SetDeferredReleaseHook(DeferredReleaseHook hook,
                                void* userData) noexcept {
        deferredReleaseHook_ = hook;
        deferredReleaseUserData_ = userData;
    }
    // Whole-voice renderer pre-pass: CC120 (All Sound Off) kills every
    // channel voice and channel-owned tail immediately.  While the pre-pass
    // dispatch runs, the renderer captures each killed voice as a ghost
    // (exact pre-kill state, death at the kill frame) BEFORE the bookkeeping
    // below retires it, and records each channel tail's kill frame so the
    // block render can emit the tail's audio up to that exact frame.  Null
    // outside the pre-pass: the oracle kills inline mid-block.
    using SilenceVoiceHook = void (*)(VoiceHandle handle, void* userData);
    using SilenceTailHook = void (*)(uint32_t slot, void* userData);
    void SetSilenceChannelHooks(SilenceVoiceHook voiceHook,
                                SilenceTailHook tailHook,
                                void* userData) noexcept {
        silenceVoiceHook_ = voiceHook;
        silenceTailHook_ = tailHook;
        silenceHookUserData_ = userData;
    }
    // Whole-voice renderer pre-pass: channel-wide row mutations must land at
    // the event's exact frame DURING the render, not during the pre-pass
    // dispatch (every voice would otherwise observe the new state for the
    // whole block).  When these hooks are installed, the mutators report the
    // channel and payload once per event instead of rewriting rows inline;
    // the renderer records a channel op and the owning worker applies it
    // between spans with row-local writes (thread-safe).  Null outside the
    // pre-pass: the oracle mutates inline at its exact-frame
    // dispatch boundaries, exactly as before.
    using ChannelMixOpHook = void (*)(uint8_t channel, float mixScaleLeft,
                                      float mixScaleRight, void* userData);
    using ChannelBendOpHook = void (*)(uint8_t channel, float bendSemitones,
                                       float commonRatio, void* userData);
    // Whole-voice pre-pass: reports the channel's post-rebuild modulation
    // depth (CC1 mod wheel, CC121 reset, channel pressure) so the vibrato
    // LFO can start/stop at the event's exact frame inside the owning
    // worker.  Null outside the pre-pass — the sparse vibrato pass reads
    // the live channel snapshot per span and needs no op.
    using ChannelVibratoOpHook = void (*)(uint8_t channel, float modDepth,
                                          void* userData);
    void SetRowOpHooks(ChannelMixOpHook mixHook, ChannelBendOpHook bendHook,
                       ChannelVibratoOpHook vibratoHook,
                       void* userData) noexcept {
        mixOpHook_ = mixHook;
        bendOpHook_ = bendHook;
        vibratoOpHook_ = vibratoHook;
        rowOpUserData_ = userData;
    }
    // Report a channel modulation-depth change (CC1 / CC121 / channel
    // pressure, after the channel cache rebuilt).  Whole-voice pre-pass:
    // records one vibrato op.  Otherwise a no-op — AdvanceVibratoSpan
    // picks the depth up from the channel snapshot at the next span.
    void MarkChannelVibratoDepth(uint8_t channel, float modDepth) noexcept {
        if (vibratoOpHook_ != nullptr && channel < kChannelCount)
            vibratoOpHook_(channel, modDepth, rowOpUserData_);
    }
    // Whole-voice renderer: stolen victims continue as renderer-owned ghost
    // snapshots (pre-pass state + exact-frame retirement), so the in-band
    // steal-tail capture — which would otherwise record block-start state —
    // is suppressed while the pre-pass runs.
    void SetStealTailCaptureSuppressed(bool suppressed) noexcept {
        stealTailCaptureSuppressed_ = suppressed;
    }
    uint32_t GetStealFadeFrames() const noexcept { return stealFadeFrames_; }
    uint64_t GetStealHeapBuildCount() const { return stealHeapBuildCount_; }
    // Post-render bookkeeping for a release whose state flip was applied by a
    // worker at its exact frame (see SetDeferredReleaseHook).
    void FinalizeDeferredRelease(VoiceHandle handle);
    // Whole-voice renderer: install a fully-formed tail record (fields
    // already written into v.stealTail*[slot]) into the live tail list.
    void AdoptStealTailSlot(uint32_t slot);
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
    // Largest layer count any launch group has used.  The block splitter
    // bounds ghosts per stretch with it (a launch steals at most one victim
    // per layer).
    uint32_t launchLayersHighWater_ = 1u;

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

    // ── Per-voice phase rotation (PhaseRotation.h) ─────────────────
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
    // Per-voice phase rotation (PhaseRotation.h).  Audio-thread-only;
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

    alignas(64) uint32_t renderClassCount_[kVoiceRenderClassCount];

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
    // Times the winner-tree root failed its linkage validation and the index
    // was rebuilt instead of faulting (steal-index invariant repairs).
    uint64_t stealIndexRepairCount_;
    uint32_t* stealVolatileList_;
    uint32_t stealVolatileCount_;
    uint64_t* stealVolatileHeapKey_;
    uint32_t* stealVolatileHeapHandle_;
    uint32_t* stealVolatileHeapPosition_;
    uint32_t stealVolatileHeapCount_;
    uint64_t stealVolatileHeapFrame_;
    bool stealVolatileHeapValid_;
    RenderBackend stealKeyBackend_;
    // Steal policy: 0 = Quality (incremental winner tree + volatile heap,
    // quietest-victim semantics), 1 = FastCursor (O(1) round-robin slot
    // takeover, no index structures; victim quality is "oldest slot"). The
    // index is unmaintained under FastCursor and fully rebuilt from live
    // pool state on a switch back to Quality.
    uint32_t stealPolicy_ = 0u;  // 0 quality, 1 cursor, 2 scan
    uint32_t perKeyVoiceCap_ = 0u;  // 0 = off; else max playing voices per (channel,key)
    std::atomic<bool> stealReject_{false};  // live-togglable from the link thread
    float stealAgeScale_ = 1.0f / 256.0f;   // score units per sample of age; 0 = ageless
    bool lastLaunchRejected_ = false;
    uint64_t stealRejectCount_ = 0u;
    bool ShouldRejectNoteOn(const VoiceConfiguration* setups, uint32_t count,
                            const ChannelParamsSnapshot& cp,
                            uint32_t multiplicity);
    uint32_t stealScanCursor_ = 0u;  // rotating scan window (policy 2)
    uint32_t stealCursor_ = 0u;
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

    // Mirrored mix scales + pending-fold masks for the batched mix fold.
    // Written by MarkChannelMixStale on the dispatch thread; folded on the
    // same thread at the consumption points listed above. Scales are only
    // ever read for channels whose bit is set, and marking always supplies
    // fresh scales, so the mirror can never feed a stale value.
    float channelMixScaleL_[kChannelCount];
    float channelMixScaleR_[kChannelCount];
    // Two pending-fold masks: the steal mask is consumed at launch entry
    // (victim scores must see fresh levels), the render mask at vintage-exact
    // batch boundaries (renderGain rewrites). Marking sets both.
    uint64_t channelMixStealMask_;
    uint64_t channelMixDirtyMask_;
    // Per-key tracking for EndVoicesForChannelKey
    int32_t channelKeyVoiceHead_[kChannelCount][kNoteCount];
    // Tail of the newest-to-oldest intrusive chain. Note-off can therefore
    // identify the oldest outstanding generation in O(1), then touch only
    // that generation's adjacent SF2 layers.
    int32_t channelKeyVoiceOldest_[kChannelCount][kNoteCount];
    // Live chain length per (channel,note) — only still-playing voices are
    // linked (StartRelease unlinks), so this is the per-key polyphony the
    // optional per-key voice cap is measured against. Maintained by
    // Link/UnlinkChannelKey; O(1) reads keep the cap off the launch hot path.
    uint32_t channelKeyVoiceCount_[kChannelCount][kNoteCount];
    // Physical SF2 regions created by one MIDI note-on form one atomic steal
    // group. Dedicated links remain valid after note-off unlinks channel/key
    // tracking, so a releasing stereo pair cannot be split either.
    // SF2 exclusiveClass is defined for values 1..127.  This intrusive
    // active-voice index makes choking proportional to the affected groups,
    // independent of total polyphony.
    static constexpr uint32_t kExclusiveClassCount = 128u;
    int32_t exclusiveClassHead_[kChannelCount][kExclusiveClassCount];
    uint32_t lastLinkedPlayIndex_;
    VoiceHandle lastLinkedPlayVoice_;
    // Cap on the per-launch batched steal-victim selection (launch groups
    // larger than this fall back to the per-layer pop path).
    static constexpr uint32_t kStealBatchMaxLayers = 16u;
    void* metadataStorage_;
    size_t metadataBytes_;
    VoiceLinkRow* linkRows_ = nullptr;
#define SVMS_DECLARE_LINK_COLUMN(type, name) \
    RowColumn<VoiceLinkRow, type, &VoiceLinkRow::name> name;
    SVMS_VOICE_LINK_FIELDS(SVMS_DECLARE_LINK_COLUMN)
#undef SVMS_DECLARE_LINK_COLUMN
    void BindLinkRows(VoiceLinkRow* rows) noexcept {
        linkRows_ = rows;
#define SVMS_BIND_LINK_COLUMN(type, name) name.rows = rows;
        SVMS_VOICE_LINK_FIELDS(SVMS_BIND_LINK_COLUMN)
#undef SVMS_BIND_LINK_COLUMN
    }

    bool ReserveMetadata(uint32_t capacity);
    void CopyFrom(const VoiceManager& other);

    void InitializeVoice(VoiceHandle handle, uint8_t channel, uint8_t note, uint8_t velocity);
    void PrefetchVoiceLines(VoiceHandle handle);
    void InitializePreparedVoice(VoiceHandle handle, uint8_t channel,
                                 uint8_t note, uint8_t velocity);
    void ApplyVoiceConfigurationFields(
        VoiceHandle handle, const VoiceConfiguration& setup,
        const ChannelParamsSnapshot& channelParams,
        VoiceRenderClass knownClass = VoiceRenderClass::Generic);
    // One body, two sinks: kStage = false writes the SoA arrays (the
    // production path); kStage = true writes the VoiceLaunchRecord and sets
    // VoiceRow::staged.  Same arithmetic either way.
    template <bool kStage>
    void ApplyVoiceConfigurationFieldsImpl(
        VoiceHandle handle, const VoiceConfiguration& setup,
        const ChannelParamsSnapshot& channelParams,
        VoiceRenderClass knownClass);
    static VoiceRenderClass ClassifyConfiguration(
        const VoiceConfiguration& setup);
    static bool IsStableConfiguration(const VoiceConfiguration& setup);
    void LinkChannelKey(VoiceHandle handle);
    void UnlinkChannelKey(VoiceHandle handle);
    void UnlinkPlayGroup(VoiceHandle handle);
    void LinkExclusiveClass(VoiceHandle handle);
    void UnlinkExclusiveClass(VoiceHandle handle);
    void ChokeExclusiveClasses(uint8_t channel,
                               const VoiceConfiguration* setups,
                               uint32_t count, uint32_t blockOffset);
    void LinkChannelActive(VoiceHandle handle);
    void UnlinkChannelActive(VoiceHandle handle);
    void MoveChannelActiveInPlace(VoiceHandle handle, uint8_t newChannel);
    uint32_t AllocateChannelIndexBlock();
    void FreeChannelIndexBlock(uint32_t block);
    VoiceHandle LastChannelActive(uint8_t channel) const;
    VoiceRenderClass ClassifyVoice(VoiceHandle handle) const;
    void LinkRenderClass(VoiceHandle handle);
    void UnlinkRenderClass(VoiceHandle handle);
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
                               const bool* consumed,
                               VoiceHandle consumedByForcedSteal = kInvalidVoice);
    // Releasing-ring fast path: consumes (or peeks, with consume=false) the
    // oldest plausible Releasing entry, validating it against live pool
    // state; returns kInvalidVoice when the ring runs dry or every remaining
    // entry is stale.
    VoiceHandle NextValidReleasingRingVictim(uint32_t& victimPosition,
                                             bool consume);
    bool ReleasingRingEligible() const;
    bool FastSteal() const { return stealPolicy_ != 0u; }
    VoiceHandle SelectCursorVictim();
    VoiceHandle SelectScanVictim();
    bool ScanSteal() const { return stealPolicy_ == 2u; }
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
    SVMS_VM_FORCEINLINE bool IsLinkedActiveVoice(VoiceHandle handle) const;
    float ComputeEffectiveStealLevel(VoiceHandle handle) const;
    // Shared per-voice body of the mix-gain fold (eager refresh and batched
    // folds — must stay bit-identical between them).
    void FoldVoiceMixGains(VoiceHandle handle, float mixScaleLeft,
                           float mixScaleRight, bool& stableTreeDirty,
                           bool writeRenderGain);
    void FoldMask(uint64_t mask, bool writeRenderGain);
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
    // Transient: the multiplicity of the launch in flight (1 outside a
    // LaunchVoiceGroup call). ApplyVoiceConfigurationFields folds it into the
    // voice's gains, so every launch path (in-place, reuse, general) agrees.
    uint32_t launchMult_ = 1u;
    bool mergedVoicesMayExist_ = false;
    // Fork twins need a playIndex that cannot collide with the driver's
    // counter (which counts up from 1). The twin is released at once and
    // leaves the key chain, so this only has to be unique among live groups.
    uint32_t nextForkPlayIndex_ = 0x80000000u;
    uint32_t forkFailureCount_ = 0u;
    VoiceForkedHook voiceForkedHook_ = nullptr;
    void* voiceForkedUserData_ = nullptr;
    bool ForkReleasedUnits(uint8_t channel, uint8_t note, uint32_t playIndex,
                           uint32_t units, bool sustain, uint32_t blockOffset);
    PreTailCaptureHook preTailCaptureHook_ = nullptr;
    void* preTailCaptureUserData_ = nullptr;
    VoiceConfiguredHook voiceConfiguredHook_ = nullptr;
    void* voiceConfiguredUserData_ = nullptr;
    DeferredReleaseHook deferredReleaseHook_ = nullptr;
    ReExciteHook reExciteHook_ = nullptr;
    void* reExciteUserData_ = nullptr;
    void* deferredReleaseUserData_ = nullptr;
    SilenceVoiceHook silenceVoiceHook_ = nullptr;
    SilenceTailHook silenceTailHook_ = nullptr;
    void* silenceHookUserData_ = nullptr;
    ChannelMixOpHook mixOpHook_ = nullptr;
    ChannelBendOpHook bendOpHook_ = nullptr;
    ChannelVibratoOpHook vibratoOpHook_ = nullptr;
    void* rowOpUserData_ = nullptr;
    bool stealTailCaptureSuppressed_ = false;
    bool launchStagingActive_ = false;   // plan window: launches may stage
    bool planWindow_ = false;            // inside PlanWholeVoiceBlock dispatch
    bool lazyRunActive_ = false;         // inside a NoteOn run (lazy launches)
    uint64_t lazyChainSteps_ = 0u;
    uint64_t lazyStarts_ = 0u;
    struct PendingLaunch {
        bool valid = false;
        uint32_t count = 0u;
        VoiceHandle handles[8]{};
        VoiceConfiguration setups[8];
        uint8_t channel = 0u, note = 0u, velocity = 0u;
        uint32_t playIndex = 0u;
        uint32_t multiplicity = 1u;
        const ChannelParamsSnapshot* params = nullptr;
        // Steal score of each pending layer as a victim (age 0), for
        // ShouldRejectNoteOn.
        float scores[8]{};
        // Chain mode: while the pending group stays the tournament root the
        // winner tree is not refreshed per step. Its leaves and paths are
        // stale until FlushPendingTree(); the root is maxKey (> runnerUp, the
        // best key of every voice outside the group).
        bool treeDirty = false;
        uint32_t winnerIdx = 0u;   // layer of handles[] holding maxKey
        uint64_t maxKey = 0u;
        uint64_t runnerUp = 0u;
    } pendingLaunch_;
    void FlushPendingTree();
    uint64_t StealRootKey() const noexcept {
        return pendingLaunch_.valid && pendingLaunch_.treeDirty
            ? pendingLaunch_.maxKey : stealWinnerTree_[1];
    }
    bool TryLazyGroupLaunch(uint8_t channel, uint8_t note, uint8_t velocity,
                            const VoiceConfiguration* setups, uint32_t count,
                            uint32_t playIndex,
                            const ChannelParamsSnapshot& channelParams,
                            VoiceHandle* outHandles);
    void CommitPendingLaunchImpl();
    void ReplaceGroupInPlace(uint8_t channel, uint8_t note, uint8_t velocity,
                             const VoiceConfiguration* setups, uint32_t count,
                             const VoiceHandle* handles,
                             bool candidatesReservedInPlace);
    bool stagedPending_ = false;         // some row may still be staged
    int8_t launchStagingMode_ = -1;      // -1: follow g_launchStagingEnabled
};

// ════════════════════════════════════════════════════════════════════════
// Implementation
// ════════════════════════════════════════════════════════════════════════

#include "synth/voicemanager/Storage.inl"
#include "synth/voicemanager/Indices.inl"
#include "synth/voicemanager/Stealing.inl"
#include "synth/voicemanager/Launch.inl"
#include "synth/voicemanager/Control.inl"

} // namespace svms

#undef SVMS_VM_FORCEINLINE
#endif
