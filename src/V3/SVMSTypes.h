#ifndef SVMS_TYPES_H
#define SVMS_TYPES_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <malloc.h>
#include <new>

namespace svms {

constexpr uint32_t kChannelCount = 16;
constexpr uint32_t kNoteCount = 128;
constexpr uint32_t kMaxPolyphony = 4096;
constexpr uint32_t kSamplesPerPage = 4096;
constexpr uint32_t kDefaultSampleRate = 44100;
constexpr uint32_t kDefaultBufferFrames = 2048;
constexpr uint32_t kMaxVoicesDefault = 1000;

constexpr uint32_t kMixBufferAlign = 64;

// Matches OmniMIDI's default logical EV-buffer size.  This is a logical
// event count, not a byte count; TimestampedMidiEvent remains compact.
constexpr uint32_t kDefaultEventRingCapacity = 393216;
constexpr uint32_t kEventBufferCapacity = kDefaultEventRingCapacity;
// Configuration is intentionally not capped to a cache-size-derived value.
// Runtime allocation and the process address space define the practical
// ceiling (especially for 32-bit/XP builds).
constexpr uint32_t kMaxConfigurableEventCapacity = UINT32_MAX;
constexpr uint32_t kMaxEventsPerBlock = UINT32_MAX;

constexpr uint32_t kNewbornProtectSamples = 64;
// A stolen voice is not allowed to disappear at an arbitrary waveform
// phase. Keep a compact copy for a short fixed ramp. Sixty-four output frames
// suppress the discontinuity without retaining a near-second voice layer
// throughout sustained full-pool stealing.
constexpr uint32_t kStealFadeFrames = 64;
constexpr uint32_t kStealTailReserve = 50;
// BASSMIDI converts its internal voice gain back to mixer amplitude with a
// 1/42000 factor. Keep the inverse scale so loudness and age retain the same
// relative weight in the replicated victim score.
constexpr float kBassMidiStealGainScale = 42000.0f;

// Raw MIDI ingress record. Producers capture both timestamp and global
// sequence before any backpressure wait.
struct TimestampedMidiEvent {
    uint64_t qpcTimestamp;
    uint32_t message;
    uint32_t sequence;
};

// Sequence comparisons remain valid across uint32 wrap as long as the live
// window is smaller than 2^31 events (the complete ingress capacity is well
// below that).  Equality is included because the terminating controller owns
// the fence sequence itself.
inline bool SequenceAtOrBefore(uint32_t sequence, uint32_t fence) noexcept {
    return static_cast<int32_t>(sequence - fence) <= 0;
}

// ── Adaptive decimation ─────────────────────────────────────────────────
// SnappySynth-style step table.  Step is the stride through the active
// voice list when rendering under polyphony pressure.  Step=1 means full
// quality (all voices rendered every sample).
// At step=8, every 8th voice is rendered — 87.5% voice cull.
// Unrendered voices still hold their state but do not produce output
// samples, effectively pausing their temporal resolution.
// The step changes per-block based on active voice count.
//
// The stabilized 4096-voice engine is always full quality. Decimation tiers
// remain reserved for the future storage expansion beyond the current pool:
//   <= 4,096 voices: step 1
//   < 50,000 voices: step 2
//   < 150,000 voices: step 4
//   < 500,000 voices: step 8
//   >= 500,000 voices: step 16
constexpr uint32_t kDecimationTier1 = kMaxPolyphony + 1; // step 1 through 4096
constexpr uint32_t kDecimationTier2 = 50000;   // step 2
constexpr uint32_t kDecimationTier3 = 150000;  // step 4
constexpr uint32_t kDecimationTier4 = 500000;  // step 8
                                                // beyond: step 16

inline uint32_t ComputeDecimationStep(uint32_t activeVoiceCount) {
    if (activeVoiceCount < kDecimationTier1) return 1;
    if (activeVoiceCount < kDecimationTier2) return 2;
    if (activeVoiceCount < kDecimationTier3) return 4;
    if (activeVoiceCount < kDecimationTier4) return 8;
    return 16;
}

enum class EventType : uint8_t {
    NoteOn = 0,
    NoteOff = 1,
    ControlChange = 2,
    ProgramChange = 3,
    PitchBend = 4,
    ChannelPressure = 5,
    KeyPressure = 6,
    AllNotesOff = 7,
    AllSoundOff = 8,
    ResetAllControllers = 9,
    SystemExclusive = 10,
    Invalid = 0xFF,
};

enum class EventOverflowMode : uint8_t {
    PriorityVelocity = 0,
    LosslessBackpressure = 1,
    // Source compatibility with the pre-stabilization names.
    DropOnOverflow = PriorityVelocity,
    NoDropBackpressure = LosslessBackpressure,
};

// Audio-thread-owned counters.  Producers only update submitted/accepted/
// dropped and blocked QPC counters through the driver's admission path.
struct EventTelemetry {
    uint64_t submitted = 0;
    uint64_t accepted = 0;
    uint64_t dropped = 0;
    uint64_t producerBlockedQPC = 0;
    uint64_t backpressureBlocks = 0;
    uint64_t ingressHighWater = 0;
    uint64_t scheduledHighWater = 0;
    uint64_t dispatched = 0;
    uint64_t late = 0;
    uint64_t skippedOutputFrames = 0;
    uint64_t staleNoteOnsSkipped = 0;
    uint64_t staleNoteOffsCompacted = 0;
    uint64_t sequenceGaps = 0;
    uint64_t zeroMatchedRegions = 0;
    uint64_t noteRegionCacheHits = 0;
    uint64_t noteRegionCacheMisses = 0;
    uint64_t allocationFailures = 0;
    uint64_t voiceSteals = 0;
    uint64_t immediateRetirements = 0;
    uint64_t maxCallbackQPC = 0;
    float callbackP95Percent = 0.0f;
    float callbackP99Percent = 0.0f;
    float callbackP999Percent = 0.0f;
    uint64_t overBudgetCallbacks = 0;
    uint32_t maxConsecutiveOverBudget = 0;
    uint64_t shedNoteOns = 0;
    uint64_t shedByVelocity[128]{};
    uint64_t cancelledSubmissions = 0;
    uint32_t currentVelocityCutoff = 1;
};

// Audio-thread-owned proof that live note-ons reached a valid SF2 sample.
// The diagnostic window reads a snapshot once per callback.
struct LiveSF2Telemetry {
    uint64_t noteOns = 0;
    uint64_t exactRegionMatches = 0;
    uint64_t zeroMatchedRegions = 0;
    uint64_t invalidPresets = 0;
    uint64_t invalidRegions = 0;
    uint64_t invalidSampleRanges = 0;
    uint64_t configuredVoices = 0;
    float lastInitialPeak = 0.0f;
    float lastVoiceGain = 0.0f;
    float lastMixGainL = 0.0f;
    float lastMixGainR = 0.0f;
    float renderPeak = 0.0f;
    float lastFloatSample = 0.0f;
    float lastPhaseStep = 0.0f;
    float lastPhase = 0.0f;
    uint32_t lastDelaySamples = 0;
    uint32_t lastAttackSamples = 0;
    uint32_t lastRelativeEnd = 0;
    uint32_t lastVoiceHandle = UINT32_MAX;
    uint8_t lastSampleBacked = 0;
    uint8_t lastChannel = 0;
    uint8_t lastNote = 0;
    uint8_t lastVelocity = 0;
    uint16_t lastPreset = UINT16_MAX;
    uint16_t lastRegion = UINT16_MAX;
    uint16_t lastSample = UINT16_MAX;
    uint32_t lastSampleStart = 0;
    uint32_t lastSampleEnd = 0;
};

// Stable, read-only snapshot returned by the versioned
// SVMSGetDriverDebugInfoV1 export.
// The audio thread publishes this as a whole through a double buffer so a
// diagnostic client never has to inspect live engine structures.
struct DriverDebugInfo {
    static constexpr uint32_t kMagic = 0x534D5653u; // "SVMS"
    uint32_t magic = kMagic;
    uint32_t version = 1;
    uint32_t size = sizeof(DriverDebugInfo);
    uint32_t reserved = 0;
    uint64_t callbackCount = 0;
    uint64_t submitted = 0;
    uint64_t accepted = 0;
    uint64_t dispatched = 0;
    uint64_t noteOns = 0;
    uint64_t matchedRegions = 0;
    uint64_t configuredVoices = 0;
    uint32_t activeVoices = 0;
    uint32_t sampleDataFrames = 0;
    uint32_t sampleCount = 0;
    uint32_t soundFontLoaded = 0;
    uint32_t audioRunning = 0;
    int32_t audioHResult = 0;
    float renderPeak = 0.0f;
};

// Compatibility payload used by SnappySynth V2's GetVoiceStatistics export.
// Ziggy reads these three DWORDs in this exact order.
struct SnappyVoiceStatistics {
    uint32_t activeVoices = 0;
    uint32_t freeVoices = 0;
    uint32_t voiceSteals = 0;
};

// Legacy OmniMIDI/KDMAPI debug layout returned by GetDriverDebugInfo().
// Keep this separate from DriverDebugInfo: the latter is V3's versioned ABI.
struct LegacyDriverDebugInfo {
    float renderingTime = 0.0f;
    uint32_t activeVoices[kChannelCount]{};
    double asioInputLatency = 0.0;
    double asioOutputLatency = 0.0;
    double healthThreadTime = 0.0;
    double activeThreadTime = 0.0;
    double eventProcessingThreadTime = 0.0;
    double cookedThreadTime = 0.0;
    uint32_t currentSoundFontList = 0;
    double audioLatency = 0.0;
    uint32_t audioBufferSize = 0;
};

static_assert(sizeof(SnappyVoiceStatistics) == 12,
              "SSV2 voice statistics ABI changed");

struct MidiEvent {
    EventType type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
    int64_t sampleOffset;
};

enum class VoiceState : uint8_t {
    Free = 0,
    Active = 1,
    Releasing = 2,
};

// Stable kernel eligibility shared by scalar and future SIMD backends.  The
// lifecycle list remains independent so render ordering never changes MIDI or
// stealing semantics.
enum class VoiceRenderClass : uint8_t {
    SustainedLoop = 0,
    SustainedOneShot,
    TransientLoop,
    ReleaseLoop,
    ReleaseOneShot,
    Generic,
    Count
};

inline constexpr uint32_t kVoiceRenderClassCount =
    static_cast<uint32_t>(VoiceRenderClass::Count);

enum class SampleInterpolation : uint8_t {
    Nearest = 0,
    Linear = 1,
    Cubic = 2,
    Sinc = 3,
};

enum class FilterType : uint8_t {
    None = 0,
    LowPass2Pole = 1,
    LowPass4Pole = 2,
};

enum class InterpolationMode : uint8_t {
    Nearest = 0,
    Linear = 1,
    Cubic = 2,
};

enum class AudioBackend : uint8_t {
    WASAPIShared = 0,
    WASAPIExclusive = 1,
    ASIO = 2,
    DirectSound = 3,
};

enum class RenderBackend : uint8_t {
    Scalar = 0,
    SSE2 = 1,
    AVX2 = 2,
    AVX512 = 3,
    CUDA = 4,
    Vulkan = 5,
};

enum class PanLaw : uint8_t {
    Linear = 0,
    ConstantPower = 1,
    Balance = 2,
};

enum class OverloadState : uint8_t {
    Normal = 0,
    Soft = 1,
    Hard = 2,
    Panic = 3,
};

using VoiceHandle = uint32_t;
constexpr VoiceHandle kInvalidVoice = UINT32_MAX;

struct EnvelopeParams {
    float delay;
    float attack;
    float hold;
    float decay;
    float sustain;
    float release;
};

struct ChannelParamsSnapshot {
    float volume;
    float expression;
    float panLeft;
    float panRight;
    // Rebuilt only when this channel changes. Note launch and controller gain
    // refresh then need one region-gain multiply per side instead of three.
    float mixScaleLeft;
    float mixScaleRight;
    float pitchBendCents;
    float filterCutoff;
    float filterResonance;
    float modDepth;
    uint32_t sustainActive;
    float dummy;
};

struct RenderStats {
    uint32_t activeVoices;
    uint32_t maxVoices;
    uint32_t voicesRendered;
    uint32_t underruns;
    float cpuLoadPercent;
    OverloadState overload;
};

struct SoundFontHeader {
    uint32_t sampleRate;
    uint32_t sampleCount;
    uint32_t presetCount;
    uint32_t instrumentCount;
    float dummy;
};

struct SamplePage {
    float samples[kSamplesPerPage];
    uint32_t sampleRate;
    uint32_t frameCount;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint8_t loopMode;
    uint8_t mipLevel;
    uint16_t padding;
};

// ════════════════════════════════════════════════════════════════════════
// VoiceSoA — Structure-of-Arrays voice pool.
//
// Dropped from V3 initial design:
//   - Doubly-linked steal lists (nextStealVoice, prevStealVoice, stealClass)
//   - Singly-linked free list (freeListNext)
//   Replaced by flat arrays activeList[] / freeStack[] in VoiceManager.
//
// Added:
//   - birthFrame: absolute output frame captured at allocation. Voice age is
//     derived from the render clock, so block size and decimation cannot
//     skew stealing or newborn protection.
//
// Render-hot precomputed fields (filled by SetVoiceSample / RefreshMixGain,
// read every sample by RenderBlock — keep these arrays dense):
//   - relEnd / relLoopS / relLoopE: sample-region bounds relative to
//     sampleStart, so the per-sample loop never recomputes them.
//   - relLoopSF / relLoopEF: float copies for the phase-wrap compare
//     (avoids per-sample cvtsi2ss).
//   - loopEnabled: ShouldLoop result cached per voice.  Cleared by
//     StartRelease for loopMode==3 (loop-during-key-depression) voices.
//   - mixGainL / mixGainR: gainLeft/Right × channel pan × channel volume,
//     premultiplied so the per-sample mix is a single FMA per channel.
//     Refreshed at note-on and only for a channel whose gain state changes.
//   - renderGainL / renderGainR: sustained-envelope gain folded into mixGain,
//     avoiding redundant multiplies in dense short-span kernels.
//
// Removed: fractional phase offsets; event starts are integer output frames.
// ════════════════════════════════════════════════════════════════════════
#define SVMS_VOICE_SOA_DYNAMIC_FIELDS(X) \
    X(uint8_t, channel) \
    X(uint8_t, note) \
    X(uint8_t, velocity) \
    X(uint8_t, state) \
    X(uint8_t, envelopeStage) \
    X(uint8_t, sampleBacked) \
    X(uint8_t, renderClass) \
    X(uint16_t, presetIndex) \
    X(uint16_t, regionIndex) \
    X(uint32_t, playIndex) \
    X(float, phases) \
    X(float, phaseIncs) \
    X(float, basePhaseIncs) \
    X(float, pitchBendScales) \
    X(float, currentGain) \
    X(float, targetGain) \
    X(float, sustainLevel) \
    X(float, attackGainStep) \
    X(float, decayGainStep) \
    X(float, releaseDecay) \
    X(float, gainLeft) \
    X(float, gainRight) \
    X(float, mixGainL) \
    X(float, mixGainR) \
    X(float, renderGainL) \
    X(float, renderGainR) \
    X(uint32_t, sampleStart) \
    X(uint32_t, sampleEnd) \
    X(uint32_t, loopStart) \
    X(uint32_t, loopEnd) \
    X(uint8_t, loopMode) \
    X(uint8_t, loopEnabled) \
    X(uint32_t, relEnd) \
    X(uint32_t, relLoopS) \
    X(uint32_t, relLoopE) \
    X(float, relLoopSF) \
    X(float, relLoopEF) \
    X(uint32_t, holdSamplesRemaining) \
    X(uint32_t, attackSamplesRemaining) \
    X(uint32_t, decaySamplesRemaining) \
    X(uint32_t, delaySamplesRemaining) \
    X(uint32_t, releaseSamplesRemaining) \
    X(float, decaySlope) \
    X(uint32_t, samplePageId) \
    X(uint8_t, heldBySustain) \
    X(uint32_t, releaseStartInBlock) \
    X(int32_t, nextChannelKeyVoice) \
    X(int32_t, prevChannelKeyVoice) \
    X(uint64_t, birthFrame) \
    X(uint32_t, stealFadeInFramesRemaining) \
    X(uint32_t, stealFadeInFramesTotal)

#define SVMS_VOICE_SOA_FIXED_TAIL_FIELDS(X) \
    X(float, stealTailPhase, kStealTailReserve) \
    X(float, stealTailPhaseInc, kStealTailReserve) \
    X(float, stealTailGain, kStealTailReserve) \
    X(float, stealTailMixGainL, kStealTailReserve) \
    X(float, stealTailMixGainR, kStealTailReserve) \
    X(uint32_t, stealTailSampleStart, kStealTailReserve) \
    X(uint32_t, stealTailRelEnd, kStealTailReserve) \
    X(uint32_t, stealTailRelLoopS, kStealTailReserve) \
    X(uint32_t, stealTailRelLoopE, kStealTailReserve) \
    X(float, stealTailRelLoopSF, kStealTailReserve) \
    X(float, stealTailRelLoopEF, kStealTailReserve) \
    X(uint32_t, stealTailFramesRemaining, kStealTailReserve) \
    X(uint32_t, stealTailFramesTotal, kStealTailReserve) \
    X(uint8_t, stealTailSampleBacked, kStealTailReserve) \
    X(uint8_t, stealTailLoopEnabled, kStealTailReserve) \
    X(uint8_t, stealTailChannel, kStealTailReserve)

struct alignas(64) VoiceSoA {
#define SVMS_DECLARE_DYNAMIC_FIELD(type, name) type* name = nullptr;
    SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_DECLARE_DYNAMIC_FIELD)
#undef SVMS_DECLARE_DYNAMIC_FIELD

    uint16_t pad16 = 0;

    // SoundFont identity captured at note-on.  These fields must remain
    // attached to the voice so later program changes and pitch bends cannot
    // reinterpret an already sounding voice through another preset/region.
    // Click-free voice replacement.  These arrays hold the previous contents
    // of a slot after that slot has been stolen.  They are intentionally much
    // smaller than a second complete VoiceSoA: a steal tail needs only sample
    // traversal and its instantaneous output gain, not MIDI/envelope state.
    // Tail slots are independent from primary voice handles and are hard-
    // capped by kStealTailReserve. Keeping one tail record per potential voice
    // wasted roughly 55 bytes/voice despite only 50 ever being addressable.
#define SVMS_DECLARE_FIXED_TAIL_FIELD(type, name, count) type name[count]{};
    SVMS_VOICE_SOA_FIXED_TAIL_FIELDS(SVMS_DECLARE_FIXED_TAIL_FIELD)
#undef SVMS_DECLARE_FIXED_TAIL_FIELD

    VoiceSoA() noexcept { ResetFixedTails(); }

    ~VoiceSoA() { _aligned_free(storage_); }

    VoiceSoA(const VoiceSoA& other) {
        ResetFixedTails();
        if (!Reserve(other.capacity_)) throw std::bad_alloc();
        CopyFrom(other);
    }

    VoiceSoA& operator=(const VoiceSoA& other) {
        if (this == &other) return *this;
        if (!Reserve(other.capacity_)) throw std::bad_alloc();
        CopyFrom(other);
        return *this;
    }

    VoiceSoA(VoiceSoA&& other) noexcept { MoveFrom(other); }

    VoiceSoA& operator=(VoiceSoA&& other) noexcept {
        if (this == &other) return *this;
        _aligned_free(storage_);
        MoveFrom(other);
        return *this;
    }

    bool Reserve(uint32_t capacity) noexcept {
        if (capacity == capacity_) return true;
        if (capacity == 0u) {
            ReleaseStorage();
            return true;
        }

        size_t bytes = 0u;
#define SVMS_ACCUMULATE_FIELD_SIZE(type, name) \
        bytes = AlignUp(bytes); \
        bytes += static_cast<size_t>(capacity) * sizeof(type);
        SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_ACCUMULATE_FIELD_SIZE)
#undef SVMS_ACCUMULATE_FIELD_SIZE

        void* allocation = _aligned_malloc(bytes, kMixBufferAlign);
        if (!allocation) return false;
        _aligned_free(storage_);
        storage_ = allocation;
        storageBytes_ = bytes;
        capacity_ = capacity;

        size_t offset = 0u;
        uint8_t* base = static_cast<uint8_t*>(storage_);
#define SVMS_BIND_DYNAMIC_FIELD(type, name) \
        offset = AlignUp(offset); \
        name = reinterpret_cast<type*>(base + offset); \
        offset += static_cast<size_t>(capacity_) * sizeof(type);
        SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_BIND_DYNAMIC_FIELD)
#undef SVMS_BIND_DYNAMIC_FIELD
        return true;
    }

    void Reset() noexcept {
#define SVMS_CLEAR_DYNAMIC_FIELD(type, name) \
        if (name) std::memset(name, 0, static_cast<size_t>(capacity_) * sizeof(type));
        SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_CLEAR_DYNAMIC_FIELD)
#undef SVMS_CLEAR_DYNAMIC_FIELD
        pad16 = 0u;
        ResetFixedTails();
    }

    uint32_t GetCapacity() const noexcept { return capacity_; }
    size_t GetAllocatedBytes() const noexcept {
        return sizeof(*this) + storageBytes_;
    }

private:
    static size_t AlignUp(size_t value) noexcept {
        return (value + (kMixBufferAlign - 1u)) &
               ~(static_cast<size_t>(kMixBufferAlign) - 1u);
    }

    void ResetFixedTails() noexcept {
#define SVMS_CLEAR_FIXED_TAIL_FIELD(type, name, count) \
        std::memset(name, 0, sizeof(name));
        SVMS_VOICE_SOA_FIXED_TAIL_FIELDS(SVMS_CLEAR_FIXED_TAIL_FIELD)
#undef SVMS_CLEAR_FIXED_TAIL_FIELD
    }

    void CopyFrom(const VoiceSoA& other) noexcept {
#define SVMS_COPY_DYNAMIC_FIELD(type, name) \
        if (name) std::memcpy(name, other.name, \
            static_cast<size_t>(capacity_) * sizeof(type));
        SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_COPY_DYNAMIC_FIELD)
#undef SVMS_COPY_DYNAMIC_FIELD
#define SVMS_COPY_FIXED_TAIL_FIELD(type, name, count) \
        std::memcpy(name, other.name, sizeof(name));
        SVMS_VOICE_SOA_FIXED_TAIL_FIELDS(SVMS_COPY_FIXED_TAIL_FIELD)
#undef SVMS_COPY_FIXED_TAIL_FIELD
        pad16 = other.pad16;
    }

    void MoveFrom(VoiceSoA& other) noexcept {
        storage_ = other.storage_;
        storageBytes_ = other.storageBytes_;
        capacity_ = other.capacity_;
#define SVMS_MOVE_DYNAMIC_FIELD(type, name) \
        name = other.name; \
        other.name = nullptr;
        SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_MOVE_DYNAMIC_FIELD)
#undef SVMS_MOVE_DYNAMIC_FIELD
#define SVMS_MOVE_FIXED_TAIL_FIELD(type, name, count) \
        std::memcpy(name, other.name, sizeof(name));
        SVMS_VOICE_SOA_FIXED_TAIL_FIELDS(SVMS_MOVE_FIXED_TAIL_FIELD)
#undef SVMS_MOVE_FIXED_TAIL_FIELD
        pad16 = other.pad16;
        other.storage_ = nullptr;
        other.storageBytes_ = 0u;
        other.capacity_ = 0u;
        other.pad16 = 0u;
        other.ResetFixedTails();
    }

    void ReleaseStorage() noexcept {
        _aligned_free(storage_);
        storage_ = nullptr;
        storageBytes_ = 0u;
        capacity_ = 0u;
#define SVMS_NULL_DYNAMIC_FIELD(type, name) name = nullptr;
        SVMS_VOICE_SOA_DYNAMIC_FIELDS(SVMS_NULL_DYNAMIC_FIELD)
#undef SVMS_NULL_DYNAMIC_FIELD
    }

    void* storage_ = nullptr;
    size_t storageBytes_ = 0u;
    uint32_t capacity_ = 0u;
};

#undef SVMS_VOICE_SOA_FIXED_TAIL_FIELDS
#undef SVMS_VOICE_SOA_DYNAMIC_FIELDS

} // namespace svms

#endif
