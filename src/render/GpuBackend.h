#ifndef SVMS_GPU_BACKEND_H
#define SVMS_GPU_BACKEND_H

// Realtime GPU render stage: the per-API half.
// A backend owns one device/context and renders one block of sustained-loop
// rows per Submit/Collect pair.  Everything API-neutral lives in GpuStage.

#include <cstdint>
#include <memory>
#include <string>

namespace svms {
namespace gpu {

// Words per GPU row record.  Layout (all 32-bit):
//   0 sampleStart | filterEnabled << 31     6 (reserved)
//   1 relLoopS                              7 renderGainL (float)
//   2 relLoopE                              8 renderGainR (float)
//   3 stepIdx  (32.32 step, whole frames)   9 filterA0    (float)
//   4 stepFrac (32.32 step, fraction)      10 filterB1    (float)
//   5 phaseFrac (initial cursor fraction;  11 filterB2    (float)
//     only read when the row is uploaded)
constexpr uint32_t kRowWords = 12u;
constexpr uint32_t kFilterFlag = 0x80000000u;

// The dynamic state of a row lives on the GPU between blocks (8 words per
// handle).  The per-block list is just handles; a row's state is uploaded
// only when the CPU changed it behind the GPU's back (new row, CPU-rendered
// block, kind change).  Layout: 0 cursor index, 1 z1, 2 z2, 3 gain, 4 stage (0 =
// releasing, else the envelope stage), 5 attackRem, 6 decayRem, 7 releaseRem,
// 8 cursor fraction.  The cursor is 32.32 fixed point (synth/PhaseFx.h).
constexpr uint32_t kDynWords = 9u;

// A block is submitted in up to this many parts.
constexpr uint32_t kMaxParts = 4u;

// One row upload record (64 B): the handle, its static row words, and the
// dynamic state it starts from (cursor index, filter delay line; the cursor
// fraction rides in words[5]).
struct UploadRecord {
    uint32_t handle;
    uint32_t words[kRowWords];
    uint32_t phaseIdx;
    float z1;
    float z2;
};
static_assert(sizeof(UploadRecord) == 64u, "UploadRecord is 16 words");

// Dynamic state read back for list position i.
struct DynOut {
    uint32_t phaseIdx;
    float z1;
    float z2;
    uint32_t phaseFrac;
};
static_assert(sizeof(DynOut) == 16u, "DynOut is one vec4");

// Envelope rows (release tails, attack/decay, one-shots) run the scalar
// generic span machine instead of the sustained fast path, so they carry the
// envelope too.  Statics, 20 words (all 32-bit):
//   0 sampleStart            7 (reserved)            14 releaseDecay (float)
//   1 flags: bit0 filter,    8 mixGainL (float)      15 filterA0 (float)
//     bit1 loop              9 mixGainR (float)      16 filterB1 (float)
//   2 relEnd                10 targetGain (float)    17 filterB2 (float)
//   3 relLoopS              11 sustainLevel (float)  18, 19 see below
//   4 relLoopE              12 attackStep (float)
//   5 stepIdx               13 decaySlope (float)
//   6 stepFrac
//   18, 19 renderGainL/R (float): the gains of a row that renders as a held
//     sustained loop until its release frame (kTlPreSustained).
constexpr uint32_t kEnvRowWords = 20u;
constexpr uint32_t kEnvFlagFilter = 1u;
constexpr uint32_t kEnvFlagLoop = 2u;

// Timeline of an envelope row inside the block (EnvUploadRecord::timeline):
//   [0] startFrame: nothing renders (and no state advances) before it
//   [1] releaseFrame: the row flips to its release at this frame
//       (kNoReleaseFrame = none)
//   [2] flags: kTlActive when the row has a start or release frame, plus
//       kTlPreSustained (frames before the release run the held sustained-loop
//       machine, the one the CPU's sustained span uses) and kTlClearLoop (the
//       release also ends looping: SF2 sampleModes 3)
// A row without a timeline carries {0, kNoReleaseFrame, 0}.  Rows whose
// timeline changes every block are re-uploaded every block.
constexpr uint32_t kNoReleaseFrame = 0xFFFFFFFFu;
constexpr uint32_t kTlActive = 0x80000000u;
constexpr uint32_t kTlPreSustained = 1u;
constexpr uint32_t kTlClearLoop = 2u;

// Envelope upload record (144 B): handle, static words, initial dynamic
// state (kDynWords words, layout above), the block timeline, padding to a
// whole number of vec4s.
struct EnvUploadRecord {
    uint32_t handle;
    uint32_t words[kEnvRowWords];
    uint32_t dyn[kDynWords];
    uint32_t timeline[3];
    uint32_t pad[3];
};
static_assert(sizeof(EnvUploadRecord) == 144u, "EnvUploadRecord is 36 words");

// Read back per envelope row.  stageRetire: bits 24..27 the stage code after
// the block (for a row that flipped to its release inside the block: the stage
// it had at the flip), bit 28 set when the row flipped to its release at its
// timeline frame, bits 0..23 the frame the voice retired on (kEnvNoRetire =
// none).  A row that flipped out of the sustain stage reports its gain at the
// flip in attackRem (both attackRem and decayRem are zero there).
constexpr uint32_t kEnvFlipped = 1u << 28;
constexpr uint32_t kEnvNoRetire = 0xFFFFFFu;
struct EnvDynOut {
    uint32_t phaseIdx;
    float z1;
    float z2;
    float gain;
    uint32_t stageRetire;
    uint32_t attackRem;
    uint32_t decayRem;
    uint32_t releaseRem;
    uint32_t phaseFrac;
    uint32_t pad[3];
};
static_assert(sizeof(EnvDynOut) == 48u, "EnvDynOut is three vec4");

enum class Result : uint32_t {
    Ok = 0,     // mix and dynamic state are filled
    Late,       // still running at the deadline; results are discarded
    Lost,       // device gone; the stage disables itself
    Idle,       // nothing in flight
};

struct Limits {
    uint32_t groupVoices = 256u;     // voices per workgroup (shader constant)
    uint32_t tileFrames = 8u;        // frames per reduction tile (shader constant)
};

class Backend {
public:
    virtual ~Backend() = default;

    // Create the device/context and compile the shaders.  The calling thread
    // owns the context until the first Submit rebinds it to the audio thread.
    virtual bool Init(uint32_t adapterIndex, std::string& error) = 0;

    // Gives up the calling thread's claim on the context (a backend whose
    // context is current on one thread at a time), so the audio thread can bind
    // it on its first block after the stage was prepared elsewhere.
    virtual void Unbind() {}

    // Upload the int16 sample store.  `frames` excludes the trailing padding;
    // the source holds at least frames + 8 readable elements.
    virtual bool UploadStore(const int16_t* data, uint32_t frames) = 0;

    // Size the handle-indexed row buffers and the per-block scratch.
    virtual bool Reserve(uint32_t handleCapacity, uint32_t maxFrames) = 0;

    // Write row-static records into the handle-indexed buffer.  May be called
    // several times before a Submit.
    virtual bool UploadRows(const UploadRecord* records, uint32_t count) = 0;

    // Same for envelope rows (the second row buffer).
    virtual bool UploadEnvRows(const EnvUploadRecord* records,
                               uint32_t count) = 0;

    // Start rendering a part of a block: `count` sustained-loop entries and
    // `envCount` envelope entries (this part's slice of the block's lists) over
    // `frames` frames.  A block is submitted as one or two parts so the CPU can
    // pack the second while the GPU runs the first; `last` ends the block.
    // `retireThreshold` is the release gain floor (VoiceRetireThreshold()).
    virtual bool Submit(const uint32_t* list, uint32_t count,
                        const uint32_t* envList, uint32_t envCount,
                        uint32_t frames, float retireThreshold, bool last) = 0;

    // Waits for the oldest submitted part until it finishes or `timeoutMicros`
    // elapse (0 = a single poll, ~0ull = wait forever).  On Ok, mixLR holds
    // that part's 2*frames floats (left, right per frame) and dyn / envDyn the
    // block's read-back arrays, indexed by position in the block's lists (this
    // part's entries are valid).  The pointers stay valid until the next
    // Submit that starts a block.
    virtual Result Collect(uint64_t timeoutMicros, const float** mixLR,
                           const DynOut** dyn, const EnvDynOut** envDyn) = 0;

    // Drops every part still in flight (late or failed block); the GPU may
    // finish them but nothing reads the results.
    virtual void Abandon() = 0;

    // Accumulated GPU time of the voice and reduction passes (nanoseconds),
    // when the backend profiles (SVMS_GPU_PROFILE=1).
    virtual void ProfileNs(uint64_t& voice, uint64_t& reduce,
                           uint64_t& env) const {
        voice = reduce = env = 0u;
    }

    virtual const char* Name() const = 0;
    virtual uint32_t MaxVoices() const { return 0u; }

    // True when the envelope kernel honors EnvUploadRecord::timeline (rows with
    // a start and/or release frame inside the block).  A backend without it only
    // takes rows that span the whole block.
    virtual bool SupportsTimeline() const { return false; }
};

// Selector values for synth.gpu_api (0 = off).
enum class Api : uint32_t { Off = 0, Auto = 1, OpenGL = 2, Vulkan = 3, D3D11 = 4, CUDA = 5 };

// Creates (but does not Init) the backend for `api`; null when the API is not
// compiled into this build.
std::unique_ptr<Backend> CreateBackend(Api api);

} // namespace gpu
} // namespace svms

#endif
