#ifndef SVMS_TYPES_H
#define SVMS_TYPES_H

#include <cstdint>
#include <cstddef>

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
constexpr uint32_t kMaxEventsPerBlock = UINT32_MAX;

constexpr uint32_t kNewbornProtectSamples = 64;
// A stolen voice is not allowed to disappear at an arbitrary waveform
// phase.  Keep a compact copy of its render state and ramp that copy to zero
// over 10 ms while the replacement starts in the reclaimed primary slot.
constexpr uint32_t kStealFadeMilliseconds = 10;

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
    uint64_t sequenceGaps = 0;
    uint64_t zeroMatchedRegions = 0;
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
struct alignas(64) VoiceSoA {
    uint8_t  channel[kMaxPolyphony];
    uint8_t  note[kMaxPolyphony];
    uint8_t  velocity[kMaxPolyphony];
    uint8_t  state[kMaxPolyphony];
    uint8_t  envelopeStage[kMaxPolyphony];
    uint8_t  sampleBacked[kMaxPolyphony];
    uint8_t  renderClass[kMaxPolyphony];
    uint16_t pad16;

    // SoundFont identity captured at note-on.  These fields must remain
    // attached to the voice so later program changes and pitch bends cannot
    // reinterpret an already sounding voice through another preset/region.
    uint16_t presetIndex[kMaxPolyphony];
    uint16_t regionIndex[kMaxPolyphony];

    // One generation is shared by every layered region created by a single
    // MIDI note-on.  Note-off releases only the oldest active generation,
    // matching TSF's playIndex behavior for overlapping retriggers.
    uint32_t playIndex[kMaxPolyphony];

    alignas(64) float phases[kMaxPolyphony];
    alignas(64) float phaseIncs[kMaxPolyphony];
    // Pitch increment before the channel pitch-bend contribution.  Most SF2
    // regions use 100% scale tuning and can therefore share one channel bend
    // ratio; unusual scale-tuned regions retain their per-voice multiplier.
    alignas(64) float basePhaseIncs[kMaxPolyphony];
    alignas(64) float pitchBendScales[kMaxPolyphony];
    alignas(64) float currentGain[kMaxPolyphony];
    alignas(64) float targetGain[kMaxPolyphony];
    alignas(64) float sustainLevel[kMaxPolyphony];
    alignas(64) float attackGainStep[kMaxPolyphony];
    alignas(64) float decayGainStep[kMaxPolyphony];
    alignas(64) float releaseDecay[kMaxPolyphony];
    alignas(64) float gainLeft[kMaxPolyphony];
    alignas(64) float gainRight[kMaxPolyphony];
    alignas(64) float mixGainL[kMaxPolyphony];
    alignas(64) float mixGainR[kMaxPolyphony];
    alignas(64) float renderGainL[kMaxPolyphony];
    alignas(64) float renderGainR[kMaxPolyphony];

    alignas(64) uint32_t sampleStart[kMaxPolyphony];
    alignas(64) uint32_t sampleEnd[kMaxPolyphony];
    alignas(64) uint32_t loopStart[kMaxPolyphony];
    alignas(64) uint32_t loopEnd[kMaxPolyphony];
    uint8_t  loopMode[kMaxPolyphony];
    uint8_t  loopEnabled[kMaxPolyphony];

    alignas(64) uint32_t relEnd[kMaxPolyphony];
    alignas(64) uint32_t relLoopS[kMaxPolyphony];
    alignas(64) uint32_t relLoopE[kMaxPolyphony];
    alignas(64) float    relLoopSF[kMaxPolyphony];
    alignas(64) float    relLoopEF[kMaxPolyphony];

    uint32_t holdSamplesRemaining[kMaxPolyphony];
    uint32_t attackSamplesRemaining[kMaxPolyphony];
    uint32_t decaySamplesRemaining[kMaxPolyphony];
    uint32_t delaySamplesRemaining[kMaxPolyphony];
    uint32_t releaseSamplesRemaining[kMaxPolyphony];
    float decaySlope[kMaxPolyphony];

    uint32_t samplePageId[kMaxPolyphony];

    uint8_t heldBySustain[kMaxPolyphony];
    uint32_t releaseStartInBlock[kMaxPolyphony];
    int32_t nextChannelKeyVoice[kMaxPolyphony];

    uint64_t birthFrame[kMaxPolyphony];

    // Click-free voice replacement.  These arrays hold the previous contents
    // of a slot after that slot has been stolen.  They are intentionally much
    // smaller than a second complete VoiceSoA: a steal tail needs only sample
    // traversal and its instantaneous output gain, not MIDI/envelope state.
    float stealTailPhase[kMaxPolyphony];
    float stealTailPhaseInc[kMaxPolyphony];
    float stealTailGain[kMaxPolyphony];
    float stealTailMixGainL[kMaxPolyphony];
    float stealTailMixGainR[kMaxPolyphony];
    uint32_t stealTailSampleStart[kMaxPolyphony];
    uint32_t stealTailRelEnd[kMaxPolyphony];
    uint32_t stealTailRelLoopS[kMaxPolyphony];
    uint32_t stealTailRelLoopE[kMaxPolyphony];
    float stealTailRelLoopSF[kMaxPolyphony];
    float stealTailRelLoopEF[kMaxPolyphony];
    uint32_t stealTailFramesRemaining[kMaxPolyphony];
    uint32_t stealTailFramesTotal[kMaxPolyphony];
    uint32_t stealFadeInFramesRemaining[kMaxPolyphony];
    uint32_t stealFadeInFramesTotal[kMaxPolyphony];
    uint8_t stealTailSampleBacked[kMaxPolyphony];
    uint8_t stealTailLoopEnabled[kMaxPolyphony];
    uint8_t stealTailChannel[kMaxPolyphony];
};

} // namespace svms

#endif
