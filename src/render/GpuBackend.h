#ifndef SVMS_GPU_BACKEND_H
#define SVMS_GPU_BACKEND_H

// Realtime GPU render stage: the per-API half (see NEXT_SESSION_GPU_RENDER_STAGE.md).
// A backend owns one device/context and renders one block of sustained-loop
// rows per Submit/Collect pair.  Everything API-neutral lives in GpuStage.

#include <cstdint>
#include <memory>
#include <string>

namespace svms {
namespace gpu {

// Words per GPU row record.  Layout (all 32-bit):
//   0 sampleStart | filterEnabled << 31     6 phaseInc    (float)
//   1 relLoopS                              7 renderGainL (float)
//   2 relLoopE                              8 renderGainR (float)
//   3 relLoopSF   (float)                   9 filterA0    (float)
//   4 relLoopEF   (float)                  10 filterB1    (float)
//   5 (reserved)                           11 filterB2    (float)
constexpr uint32_t kRowWords = 12u;
constexpr uint32_t kFilterFlag = 0x80000000u;

// One row-static upload record (64 B): the handle then its row words.
struct UploadRecord {
    uint32_t handle;
    uint32_t words[kRowWords];
    uint32_t pad[3];
};
static_assert(sizeof(UploadRecord) == 64u, "UploadRecord is 16 words");

// One entry of the per-block voice list (16 B): the row handle plus the
// dynamic state the CPU owns (phase, filter delay line).
struct ListEntry {
    uint32_t handle;
    float phase;
    float z1;
    float z2;
};
static_assert(sizeof(ListEntry) == 16u, "ListEntry is one vec4");

// Dynamic state read back for list position i.
struct DynOut {
    float phase;
    float z1;
    float z2;
    float pad;
};
static_assert(sizeof(DynOut) == 16u, "DynOut is one vec4");

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

    // Upload the int16 sample store.  `frames` excludes the trailing padding;
    // the source holds at least frames + 8 readable elements.
    virtual bool UploadStore(const int16_t* data, uint32_t frames) = 0;

    // Size the handle-indexed row buffers and the per-block scratch.
    virtual bool Reserve(uint32_t handleCapacity, uint32_t maxFrames) = 0;

    // Write row-static records into the handle-indexed buffer.  May be called
    // several times before a Submit.
    virtual bool UploadRows(const UploadRecord* records, uint32_t count) = 0;

    // Start rendering `count` list entries over `frames` frames.
    virtual bool Submit(const ListEntry* list, uint32_t count,
                        uint32_t frames) = 0;

    // Poll until the block finishes or `timeoutMicros` elapse (0 = a single
    // poll, ~0ull = wait forever).  On Ok, mixLR holds 2*frames floats
    // (left, right per frame) and dyn holds `count` entries.
    virtual Result Collect(uint64_t timeoutMicros, float* mixLR,
                           DynOut* dyn) = 0;

    // Accumulated GPU time of the voice and reduction passes (nanoseconds),
    // when the backend profiles (SVMS_GPU_PROFILE=1).
    virtual void ProfileNs(uint64_t& voice, uint64_t& reduce) const {
        voice = reduce = 0u;
    }

    virtual const char* Name() const = 0;
    virtual uint32_t MaxVoices() const { return 0u; }
};

// Selector values for synth.gpu_api (0 = off).
enum class Api : uint32_t { Off = 0, Auto = 1, OpenGL = 2, Vulkan = 3, D3D11 = 4 };

// Creates (but does not Init) the backend for `api`; null when the API is not
// compiled into this build.
std::unique_ptr<Backend> CreateBackend(Api api);

} // namespace gpu
} // namespace svms

#endif
