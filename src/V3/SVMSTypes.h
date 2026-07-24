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

// Event buffer sizing.
// `kEventBufferCapacity` is the per-block render-event queue depth.
// Each RenderEvent is 8 bytes (type:u8, ch:u8, d1:u8, d2:u8, offset:f32).
// At 65536 slots the buffer is 512 KB — fits comfortably in any modern L3 cache.
// This supports up to 65536 events per audio block, which is more than enough
// for high-density Black MIDI playback.
constexpr uint32_t kEventBufferCapacity = 65536;

// ── Event throttling budget ────────────────────────────────────────────
// Maximum number of RenderEvents dispatched per audio block.
// With in-place voice recycling, per-block CPU is bounded by active
// polyphony (~128-256 voices), so the engine can process all incoming
// events cleanly in real-time without batch-chunking artifacts.
// Set to UINT32_MAX to disable the budget entirely — the only practical
// limit is kEventBufferCapacity (65536), which bounds the event array.
constexpr uint32_t kMaxEventsPerBlock = UINT32_MAX;

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

struct VoiceSoA {
    uint8_t  channel[kMaxPolyphony];
    uint8_t  note[kMaxPolyphony];
    uint8_t  velocity[kMaxPolyphony];
    uint8_t  state[kMaxPolyphony];
    uint8_t  envelopeStage[kMaxPolyphony];
    uint8_t  sampleBacked[kMaxPolyphony];
    uint16_t pad16;

    float phases[kMaxPolyphony];
    float phaseIncs[kMaxPolyphony];
    float currentGain[kMaxPolyphony];
    float targetGain[kMaxPolyphony];
    float sustainLevel[kMaxPolyphony];
    float attackGainStep[kMaxPolyphony];
    float decayGainStep[kMaxPolyphony];
    float releaseDecay[kMaxPolyphony];
    float gainLeft[kMaxPolyphony];
    float gainRight[kMaxPolyphony];

    uint32_t sampleStart[kMaxPolyphony];
    uint32_t sampleEnd[kMaxPolyphony];
    uint32_t loopStart[kMaxPolyphony];
    uint32_t loopEnd[kMaxPolyphony];
    uint8_t  loopMode[kMaxPolyphony];

    uint32_t holdSamplesRemaining[kMaxPolyphony];
    uint32_t attackSamplesRemaining[kMaxPolyphony];
    uint32_t decaySamplesRemaining[kMaxPolyphony];
    uint32_t delaySamplesRemaining[kMaxPolyphony];
    float decaySlope[kMaxPolyphony];

    uint32_t samplePageId[kMaxPolyphony];

    int32_t nextStealVoice[kMaxPolyphony];
    int32_t prevStealVoice[kMaxPolyphony];
    uint8_t stealClass[kMaxPolyphony];
    int32_t freeListNext[kMaxPolyphony];
    uint8_t heldBySustain[kMaxPolyphony];
    uint32_t releaseStartInBlock[kMaxPolyphony];
    int32_t nextChannelKeyVoice[kMaxPolyphony];
    float phaseOffset[kMaxPolyphony];
};

} // namespace svms

#endif
