// CUDA backend of the GPU render stage (NVIDIA only).
//
// Same contract and buffer layout as the OpenGL backend: the kernels are a
// line-for-line port of VoiceStageGLSL.h (render/gpu/VoiceStageCUDA.cu).  No
// CUDA toolkit is needed at run time and nothing is imported at load time:
// nvcuda.dll / libcuda.so.1 is opened when the stage is enabled and about 30
// driver-API entry points are fetched by hand.  The kernels ship as a fat
// binary (SASS for the common architectures plus PTX) embedded in the DLL; a
// build without the toolkit compiles this file without them and Init reports it.

#include "render/GpuBackend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(SVMS_HAVE_CUDA_KERNELS)
#include "VoiceStageCUDA_fatbin.h"
#endif

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace svms {
namespace gpu {
namespace {

// ── Minimal driver-API declarations (no CUDA headers needed) ────────────
using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
using CUcontext = struct CUctx_st*;
using CUmodule = struct CUmod_st*;
using CUfunction = struct CUfunc_st*;
using CUstream = struct CUstream_st*;
using CUevent = struct CUevent_st*;

constexpr CUresult CUDA_SUCCESS = 0;
constexpr CUresult CUDA_ERROR_NOT_READY = 600;
constexpr unsigned CU_STREAM_NON_BLOCKING = 0x1;
constexpr unsigned CU_EVENT_DEFAULT = 0x0;
constexpr unsigned CU_EVENT_DISABLE_TIMING = 0x2;
constexpr unsigned CU_MEMHOSTALLOC_PORTABLE = 0x1;
constexpr int CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75;
constexpr int CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 76;

#if defined(_WIN32) && !defined(_WIN64)
#  define SVMS_CUAPI __stdcall
#else
#  define SVMS_CUAPI
#endif

struct CuFns {
    CUresult (SVMS_CUAPI* Init)(unsigned) = nullptr;
    CUresult (SVMS_CUAPI* DriverGetVersion)(int*) = nullptr;
    CUresult (SVMS_CUAPI* DeviceGetCount)(int*) = nullptr;
    CUresult (SVMS_CUAPI* DeviceGet)(CUdevice*, int) = nullptr;
    CUresult (SVMS_CUAPI* DeviceGetName)(char*, int, CUdevice) = nullptr;
    CUresult (SVMS_CUAPI* DeviceGetAttribute)(int*, int, CUdevice) = nullptr;
    CUresult (SVMS_CUAPI* PrimaryCtxRetain)(CUcontext*, CUdevice) = nullptr;
    CUresult (SVMS_CUAPI* PrimaryCtxRelease)(CUdevice) = nullptr;
    CUresult (SVMS_CUAPI* CtxSetCurrent)(CUcontext) = nullptr;
    CUresult (SVMS_CUAPI* ModuleLoadData)(CUmodule*, const void*) = nullptr;
    CUresult (SVMS_CUAPI* ModuleUnload)(CUmodule) = nullptr;
    CUresult (SVMS_CUAPI* ModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;
    CUresult (SVMS_CUAPI* MemAlloc)(CUdeviceptr*, size_t) = nullptr;
    CUresult (SVMS_CUAPI* MemFree)(CUdeviceptr) = nullptr;
    CUresult (SVMS_CUAPI* MemHostAlloc)(void**, size_t, unsigned) = nullptr;
    CUresult (SVMS_CUAPI* MemFreeHost)(void*) = nullptr;
    CUresult (SVMS_CUAPI* MemcpyHtoD)(CUdeviceptr, const void*, size_t) = nullptr;
    CUresult (SVMS_CUAPI* MemcpyHtoDAsync)(CUdeviceptr, const void*, size_t, CUstream) = nullptr;
    CUresult (SVMS_CUAPI* MemcpyDtoHAsync)(void*, CUdeviceptr, size_t, CUstream) = nullptr;
    CUresult (SVMS_CUAPI* LaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                        unsigned, unsigned, unsigned, unsigned,
                                        CUstream, void**, void**) = nullptr;
    CUresult (SVMS_CUAPI* StreamCreate)(CUstream*, unsigned) = nullptr;
    CUresult (SVMS_CUAPI* StreamDestroy)(CUstream) = nullptr;
    CUresult (SVMS_CUAPI* StreamSynchronize)(CUstream) = nullptr;
    CUresult (SVMS_CUAPI* EventCreate)(CUevent*, unsigned) = nullptr;
    CUresult (SVMS_CUAPI* EventDestroy)(CUevent) = nullptr;
    CUresult (SVMS_CUAPI* EventRecord)(CUevent, CUstream) = nullptr;
    CUresult (SVMS_CUAPI* EventQuery)(CUevent) = nullptr;
    CUresult (SVMS_CUAPI* EventElapsedTime)(float*, CUevent, CUevent) = nullptr;
    CUresult (SVMS_CUAPI* GetErrorName)(CUresult, const char**) = nullptr;
};

class CudaBackend final : public Backend {
public:
    ~CudaBackend() override { Shutdown(); }

    bool Init(uint32_t adapterIndex, std::string& error) override;
    bool UploadStore(const int16_t* data, uint32_t frames) override;
    bool Reserve(uint32_t handleCapacity, uint32_t maxFrames) override;
    bool UploadRows(const UploadRecord* records, uint32_t count) override;
    bool UploadEnvRows(const EnvUploadRecord* records, uint32_t count) override;
    bool Submit(const uint32_t* list, uint32_t count, const uint32_t* envList,
                uint32_t envCount, uint32_t frames, float retireThreshold,
                bool last) override;
    Result Collect(uint64_t timeoutMicros, const float** mixLR,
                   const DynOut** dyn, const EnvDynOut** envDyn) override;
    void Abandon() override;
    void ProfileNs(uint64_t& voice, uint64_t& reduce, uint64_t& env) const override {
        voice = profileNs_[0];
        reduce = profileNs_[1];
        env = profileNs_[2];
    }
    const char* Name() const override { return name_.c_str(); }
    bool SupportsTimeline() const override { return true; }

private:
    enum Buf {
        kStore, kRows, kList, kPartial, kMix, kDyn, kUpload, kEnvRows,
        kEnvList, kEnvDyn, kEnvUpload, kState, kTimeline, kDevBufCount
    };
    enum Host { kMixRead, kDynRead, kEnvDynRead, kHostBufCount };
    // Kernel group size / tile (constants of the .cu file).
    static constexpr uint32_t kGroup = 128u;
    static constexpr uint32_t kTile = 4u;

    struct Part {
        CUevent done = nullptr;
        CUevent timed[6] = {};   // voice, reduce, env start/stop pairs
        bool voiceTimed = false;
        bool envTimed = false;
        bool submitted = false;
        uint32_t count = 0u, envCount = 0u, frames = 0u;
        uint32_t listBase = 0u, envBase = 0u;
    };

    bool Ok(CUresult r, const char* what);
    bool Bind();
    bool Alloc(Buf which, size_t bytes);
    bool AllocHost(Host which, size_t bytes);
    void FreeAll();
    void Shutdown();
    bool Launch(CUfunction f, uint32_t blocks, uint32_t threads, void** args);

    CuFns cu_;
#if defined(_WIN32)
    HMODULE lib_ = nullptr;
#else
    void* lib_ = nullptr;
#endif
    CUdevice device_ = 0;
    CUcontext ctx_ = nullptr;
    CUmodule module_ = nullptr;
    CUstream stream_ = nullptr;
    CUfunction fnVoice_ = nullptr, fnEnv_ = nullptr, fnReduce_ = nullptr,
               fnScatter_ = nullptr, fnEnvScatter_ = nullptr;
    CUdeviceptr buf_[kDevBufCount] = {};
    size_t bufBytes_[kDevBufCount] = {};
    void* host_[kHostBufCount] = {};
    size_t hostBytes_[kHostBufCount] = {};
    Part parts_[kMaxParts];
    uint32_t partsSubmitted_ = 0u, partsCollected_ = 0u;
    uint32_t listBase_ = 0u, envBase_ = 0u, groupBase_ = 0u;
    uint32_t uploadCursor_ = 0u, envUploadCursor_ = 0u;
    uint32_t handleCapacity_ = 0u, maxFrames_ = 0u;
    bool bound_ = false;
    std::thread::id boundThread_;
    bool profile_ = false;
    uint64_t profileNs_[3] = {};
    std::string name_ = "cuda";
    bool created_ = false;
};

template <typename F>
bool Fetch(void* lib, F& slot, const char* name, bool required = true) {
#if defined(_WIN32)
    slot = reinterpret_cast<F>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    slot = reinterpret_cast<F>(dlsym(lib, name));
#endif
    return slot != nullptr || !required;
}

bool CudaBackend::Ok(CUresult r, const char* what) {
    if (r == CUDA_SUCCESS) return true;
    const char* text = nullptr;
    if (cu_.GetErrorName) cu_.GetErrorName(r, &text);
    name_ += std::string(" [") + what + ": " + (text ? text : std::to_string(r).c_str()) + "]";
    return false;
}

bool CudaBackend::Init(uint32_t adapterIndex, std::string& error) {
#if !defined(SVMS_HAVE_CUDA_KERNELS)
    (void)adapterIndex;
    error = "this build has no CUDA kernels (built without the CUDA toolkit)";
    return false;
#else
#if defined(_WIN32)
    lib_ = LoadLibraryW(L"nvcuda.dll");
#else
    lib_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib_) {
        error = "the NVIDIA driver (nvcuda) is not available";
        return false;
    }
    bool ok = true;
#define SVMS_FETCH(field, name) ok = Fetch(lib_, cu_.field, name) && ok
    SVMS_FETCH(Init, "cuInit");
    SVMS_FETCH(DriverGetVersion, "cuDriverGetVersion");
    SVMS_FETCH(DeviceGetCount, "cuDeviceGetCount");
    SVMS_FETCH(DeviceGet, "cuDeviceGet");
    SVMS_FETCH(DeviceGetName, "cuDeviceGetName");
    SVMS_FETCH(DeviceGetAttribute, "cuDeviceGetAttribute");
    SVMS_FETCH(PrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
    SVMS_FETCH(PrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2");
    SVMS_FETCH(CtxSetCurrent, "cuCtxSetCurrent");
    SVMS_FETCH(ModuleLoadData, "cuModuleLoadData");
    SVMS_FETCH(ModuleUnload, "cuModuleUnload");
    SVMS_FETCH(ModuleGetFunction, "cuModuleGetFunction");
    SVMS_FETCH(MemAlloc, "cuMemAlloc_v2");
    SVMS_FETCH(MemFree, "cuMemFree_v2");
    SVMS_FETCH(MemHostAlloc, "cuMemHostAlloc");
    SVMS_FETCH(MemFreeHost, "cuMemFreeHost");
    SVMS_FETCH(MemcpyHtoD, "cuMemcpyHtoD_v2");
    SVMS_FETCH(MemcpyHtoDAsync, "cuMemcpyHtoDAsync_v2");
    SVMS_FETCH(MemcpyDtoHAsync, "cuMemcpyDtoHAsync_v2");
    SVMS_FETCH(LaunchKernel, "cuLaunchKernel");
    SVMS_FETCH(StreamCreate, "cuStreamCreate");
    SVMS_FETCH(StreamDestroy, "cuStreamDestroy_v2");
    SVMS_FETCH(StreamSynchronize, "cuStreamSynchronize");
    SVMS_FETCH(EventCreate, "cuEventCreate");
    SVMS_FETCH(EventDestroy, "cuEventDestroy_v2");
    SVMS_FETCH(EventRecord, "cuEventRecord");
    SVMS_FETCH(EventQuery, "cuEventQuery");
    SVMS_FETCH(EventElapsedTime, "cuEventElapsedTime");
#undef SVMS_FETCH
    Fetch(lib_, cu_.GetErrorName, "cuGetErrorName", false);
    if (!ok) {
        error = "the CUDA driver is missing entry points";
        return false;
    }
    created_ = true;
    CUresult r = cu_.Init(0u);
    if (r != CUDA_SUCCESS) {
        error = "cuInit failed (" + std::to_string(r) + "): no NVIDIA GPU or driver";
        return false;
    }
    int count = 0;
    cu_.DeviceGetCount(&count);
    if (count <= 0 || adapterIndex >= static_cast<uint32_t>(count)) {
        error = "no CUDA device at index " + std::to_string(adapterIndex);
        return false;
    }
    cu_.DeviceGet(&device_, static_cast<int>(adapterIndex));
    char deviceName[128] = {};
    cu_.DeviceGetName(deviceName, static_cast<int>(sizeof(deviceName)), device_);
    int major = 0, minor = 0, driver = 0;
    cu_.DeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device_);
    cu_.DeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device_);
    cu_.DriverGetVersion(&driver);
    name_ = std::string("cuda: ") + deviceName + " (sm_" + std::to_string(major) +
            std::to_string(minor) + ", driver " + std::to_string(driver / 1000) +
            "." + std::to_string((driver % 1000) / 10) + ")";
    if (!Ok(cu_.PrimaryCtxRetain(&ctx_, device_), "context")) {
        error = name_;
        return false;
    }
    if (!Ok(cu_.CtxSetCurrent(ctx_), "set context")) {
        error = name_;
        return false;
    }
    bound_ = true;
    boundThread_ = std::this_thread::get_id();
    r = cu_.ModuleLoadData(&module_, svms_cuda_fatbin);
    if (r != CUDA_SUCCESS) {
        // 209: no kernel image for this architecture; 222: PTX newer than the
        // driver.  Either way the stage falls back to another backend.
        error = std::string("the CUDA kernels do not load on ") + deviceName +
                " (error " + std::to_string(r) + ")";
        return false;
    }
    ok = Ok(cu_.ModuleGetFunction(&fnVoice_, module_, "svms_voice"), "svms_voice") &&
         Ok(cu_.ModuleGetFunction(&fnEnv_, module_, "svms_env"), "svms_env") &&
         Ok(cu_.ModuleGetFunction(&fnReduce_, module_, "svms_reduce"), "svms_reduce") &&
         Ok(cu_.ModuleGetFunction(&fnScatter_, module_, "svms_scatter"), "svms_scatter") &&
         Ok(cu_.ModuleGetFunction(&fnEnvScatter_, module_, "svms_env_scatter"),
            "svms_env_scatter");
    if (!ok) {
        error = name_;
        return false;
    }
    if (!Ok(cu_.StreamCreate(&stream_, CU_STREAM_NON_BLOCKING), "stream")) {
        error = name_;
        return false;
    }
    profile_ = std::getenv("SVMS_GPU_PROFILE") != nullptr;
    for (Part& p : parts_) {
        if (!Ok(cu_.EventCreate(&p.done, CU_EVENT_DISABLE_TIMING), "event")) {
            error = name_;
            return false;
        }
        if (profile_)
            for (CUevent& e : p.timed) cu_.EventCreate(&e, CU_EVENT_DEFAULT);
    }
    return true;
#endif
}

bool CudaBackend::Bind() {
    // A context is current on one thread at a time.  The audio thread binds it
    // on its first block; a rebuilt audio thread (device change) rebinds it.
    const std::thread::id self = std::this_thread::get_id();
    if (bound_ && boundThread_ == self) return true;
    if (!Ok(cu_.CtxSetCurrent(ctx_), "set context")) return false;
    boundThread_ = self;
    bound_ = true;
    return true;
}

bool CudaBackend::Alloc(Buf which, size_t bytes) {
    if (bytes == 0u) bytes = 16u;
    if (bufBytes_[which] == bytes) return true;
    if (buf_[which]) cu_.MemFree(buf_[which]);
    buf_[which] = 0u;
    bufBytes_[which] = 0u;
    if (!Ok(cu_.MemAlloc(&buf_[which], bytes), "alloc")) return false;
    bufBytes_[which] = bytes;
    return true;
}

bool CudaBackend::AllocHost(Host which, size_t bytes) {
    if (bytes == 0u) bytes = 16u;
    if (hostBytes_[which] == bytes) return true;
    if (host_[which]) cu_.MemFreeHost(host_[which]);
    host_[which] = nullptr;
    hostBytes_[which] = 0u;
    if (!Ok(cu_.MemHostAlloc(&host_[which], bytes, CU_MEMHOSTALLOC_PORTABLE),
            "pinned alloc")) {
        return false;
    }
    hostBytes_[which] = bytes;
    return true;
}

bool CudaBackend::UploadStore(const int16_t* data, uint32_t frames) {
    if (!Bind()) return false;
    cu_.StreamSynchronize(stream_);
    // frames + 8 elements are readable (the sample-store padding contract);
    // whole 32-bit words only.
    const size_t words = (static_cast<size_t>(frames) + 8u) / 2u;
    const size_t bytes = words * 4u;
    // One spare word: the kernel reads the word after the last sample.
    if (!Alloc(kStore, bytes + 16u)) return false;
    return Ok(cu_.MemcpyHtoD(buf_[kStore], data, bytes), "store upload");
}

bool CudaBackend::Reserve(uint32_t handleCapacity, uint32_t maxFrames) {
    if (!Bind()) return false;
    if (handleCapacity == handleCapacity_ && maxFrames == maxFrames_) return true;
    Abandon();
    cu_.StreamSynchronize(stream_);
    const size_t cap = handleCapacity;
    // Sustained and envelope rows each round their last group up (each part
    // rounds two kernels' last groups up).
    const size_t groups = (cap + kGroup - 1u) / kGroup + 2u * kMaxParts;
    const size_t mixBytes = static_cast<size_t>(maxFrames) * 2u * kMaxParts * 4u;
    const bool ok =
        Alloc(kEnvRows, cap * kEnvRowWords * 4u) &&
        Alloc(kState, cap * kDynWords * 4u) &&
        Alloc(kTimeline, cap * 16u) &&
        Alloc(kEnvList, cap * sizeof(uint32_t)) &&
        Alloc(kEnvDyn, cap * sizeof(EnvDynOut)) &&
        Alloc(kEnvUpload, cap * sizeof(EnvUploadRecord)) &&
        Alloc(kRows, cap * kRowWords * 4u) &&
        Alloc(kList, cap * sizeof(uint32_t)) &&
        Alloc(kDyn, cap * sizeof(DynOut)) &&
        Alloc(kPartial, groups * maxFrames * 2u * 4u) &&
        Alloc(kMix, mixBytes) &&
        Alloc(kUpload, cap * sizeof(UploadRecord)) &&
        AllocHost(kMixRead, mixBytes) &&
        AllocHost(kDynRead, cap * sizeof(DynOut)) &&
        AllocHost(kEnvDynRead, cap * sizeof(EnvDynOut));
    if (!ok) return false;
    handleCapacity_ = handleCapacity;
    maxFrames_ = maxFrames;
    uploadCursor_ = envUploadCursor_ = 0u;
    return true;
}

bool CudaBackend::Launch(CUfunction f, uint32_t blocks, uint32_t threads,
                         void** args) {
    return Ok(cu_.LaunchKernel(f, blocks, 1u, 1u, threads, 1u, 1u, 0u, stream_,
                               args, nullptr), "launch");
}

bool CudaBackend::UploadRows(const UploadRecord* records, uint32_t count) {
    if (count == 0u) return true;
    if (!Bind() || uploadCursor_ + count > handleCapacity_) return false;
    // Pageable source: the driver has copied the records before this returns,
    // so the caller's scratch can be reused at once.
    const size_t offset = static_cast<size_t>(uploadCursor_) * sizeof(UploadRecord);
    if (!Ok(cu_.MemcpyHtoDAsync(buf_[kUpload] + offset, records,
                                static_cast<size_t>(count) * sizeof(UploadRecord),
                                stream_), "row upload")) {
        return false;
    }
    CUdeviceptr rows = buf_[kRows], up = buf_[kUpload], state = buf_[kState];
    uint32_t n = count, base = uploadCursor_;
    void* args[] = {&rows, &up, &state, &n, &base};
    if (!Launch(fnScatter_, (count + 63u) / 64u, 64u, args)) return false;
    uploadCursor_ += count;
    return true;
}

bool CudaBackend::UploadEnvRows(const EnvUploadRecord* records, uint32_t count) {
    if (count == 0u) return true;
    if (!Bind() || envUploadCursor_ + count > handleCapacity_) return false;
    const size_t offset = static_cast<size_t>(envUploadCursor_) * sizeof(EnvUploadRecord);
    if (!Ok(cu_.MemcpyHtoDAsync(buf_[kEnvUpload] + offset, records,
                                static_cast<size_t>(count) * sizeof(EnvUploadRecord),
                                stream_), "env row upload")) {
        return false;
    }
    CUdeviceptr rows = buf_[kEnvRows], up = buf_[kEnvUpload], state = buf_[kState],
                timeline = buf_[kTimeline];
    uint32_t n = count, base = envUploadCursor_;
    void* args[] = {&rows, &up, &state, &timeline, &n, &base};
    if (!Launch(fnEnvScatter_, (count + 63u) / 64u, 64u, args)) return false;
    envUploadCursor_ += count;
    return true;
}

bool CudaBackend::Submit(const uint32_t* list, uint32_t count,
                         const uint32_t* envList, uint32_t envCount,
                         uint32_t frames, float retireThreshold, bool last) {
    if (partsSubmitted_ != 0u && partsCollected_ == partsSubmitted_) {
        // The previous block is fully collected: this starts a new one.
        partsSubmitted_ = partsCollected_ = 0u;
        listBase_ = envBase_ = groupBase_ = 0u;
    }
    if (!Bind() || listBase_ + count > handleCapacity_ ||
        envBase_ + envCount > handleCapacity_ || frames == 0u ||
        frames > maxFrames_ || partsSubmitted_ >= kMaxParts) {
        return false;
    }
    const uint32_t part = partsSubmitted_;
    Part& p = parts_[part];
    const uint32_t groups = (count + kGroup - 1u) / kGroup;
    const uint32_t envGroups = (envCount + kGroup - 1u) / kGroup;
    if (count != 0u &&
        !Ok(cu_.MemcpyHtoDAsync(buf_[kList] + static_cast<size_t>(listBase_) * 4u,
                                list, static_cast<size_t>(count) * 4u, stream_),
            "list upload")) {
        return false;
    }
    if (envCount != 0u &&
        !Ok(cu_.MemcpyHtoDAsync(buf_[kEnvList] + static_cast<size_t>(envBase_) * 4u,
                                envList, static_cast<size_t>(envCount) * 4u, stream_),
            "env list upload")) {
        return false;
    }
    p.voiceTimed = profile_ && count != 0u;
    p.envTimed = profile_ && envCount != 0u;
    if (count != 0u) {
        CUdeviceptr store = buf_[kStore], rows = buf_[kRows], listv = buf_[kList],
                    dyn = buf_[kDyn], state = buf_[kState], partial = buf_[kPartial];
        uint32_t n = count, f = frames, lb = listBase_, gb = groupBase_;
        void* args[] = {&store, &rows, &listv, &dyn, &state, &partial, &n, &f, &lb, &gb};
        if (p.voiceTimed) cu_.EventRecord(p.timed[0], stream_);
        if (!Launch(fnVoice_, groups, kGroup, args)) return false;
        if (p.voiceTimed) cu_.EventRecord(p.timed[1], stream_);
    }
    if (envCount != 0u) {
        CUdeviceptr store = buf_[kStore], erows = buf_[kEnvRows], elist = buf_[kEnvList],
                    edyn = buf_[kEnvDyn], state = buf_[kState], partial = buf_[kPartial],
                    timeline = buf_[kTimeline];
        uint32_t retireBits = 0u;
        std::memcpy(&retireBits, &retireThreshold, sizeof(retireBits));
        uint32_t n = envCount, f = frames, eb = envBase_, gb = groupBase_ + groups;
        void* args[] = {&store, &erows, &elist, &edyn, &state, &partial, &n, &f,
                        &eb, &gb, &retireBits, &timeline};
        if (p.envTimed) cu_.EventRecord(p.timed[4], stream_);
        if (!Launch(fnEnv_, envGroups, kGroup, args)) return false;
        if (p.envTimed) cu_.EventRecord(p.timed[5], stream_);
    }
    {
        CUdeviceptr partial = buf_[kPartial], mix = buf_[kMix];
        uint32_t f = frames, g = groups + envGroups, gf = groupBase_,
                 mb = part * frames * 2u;
        void* args[] = {&partial, &mix, &f, &g, &gf, &mb};
        if (profile_) cu_.EventRecord(p.timed[2], stream_);
        if (!Launch(fnReduce_, (frames * 2u + 63u) / 64u, 64u, args)) return false;
        if (profile_) cu_.EventRecord(p.timed[3], stream_);
    }
    // The GPU writes the results straight into pinned host memory.
    const size_t mixOffset = static_cast<size_t>(part) * frames * 8u;
    if (!Ok(cu_.MemcpyDtoHAsync(static_cast<char*>(host_[kMixRead]) + mixOffset,
                                buf_[kMix] + mixOffset,
                                static_cast<size_t>(frames) * 8u, stream_),
            "mix readback")) {
        return false;
    }
    if (count != 0u) {
        const size_t o = static_cast<size_t>(listBase_) * sizeof(DynOut);
        if (!Ok(cu_.MemcpyDtoHAsync(static_cast<char*>(host_[kDynRead]) + o,
                                    buf_[kDyn] + o,
                                    static_cast<size_t>(count) * sizeof(DynOut),
                                    stream_), "dyn readback")) {
            return false;
        }
    }
    if (envCount != 0u) {
        const size_t o = static_cast<size_t>(envBase_) * sizeof(EnvDynOut);
        if (!Ok(cu_.MemcpyDtoHAsync(static_cast<char*>(host_[kEnvDynRead]) + o,
                                    buf_[kEnvDyn] + o,
                                    static_cast<size_t>(envCount) * sizeof(EnvDynOut),
                                    stream_), "env dyn readback")) {
            return false;
        }
    }
    if (!Ok(cu_.EventRecord(p.done, stream_), "event record")) return false;
    p.submitted = true;
    p.count = count;
    p.envCount = envCount;
    p.frames = frames;
    p.listBase = listBase_;
    p.envBase = envBase_;
    listBase_ += count;
    envBase_ += envCount;
    groupBase_ += groups + envGroups;
    ++partsSubmitted_;
    if (last) {
        uploadCursor_ = 0u;
        envUploadCursor_ = 0u;
    }
    return true;
}

void CudaBackend::Abandon() {
    // The GPU may still finish the parts in flight; nothing reads the results,
    // and the next block's work queues behind them on the same stream.
    for (Part& p : parts_) {
        p.submitted = false;
        p.count = p.envCount = p.frames = 0u;
    }
    partsSubmitted_ = partsCollected_ = 0u;
    listBase_ = envBase_ = groupBase_ = 0u;
    uploadCursor_ = envUploadCursor_ = 0u;
}

Result CudaBackend::Collect(uint64_t timeoutMicros, const float** mixLR,
                            const DynOut** dyn, const EnvDynOut** envDyn) {
    if (partsCollected_ >= partsSubmitted_) return Result::Idle;
    const uint32_t part = partsCollected_;
    Part& p = parts_[part];
    if (!p.submitted) return Result::Idle;
    const bool forever = timeoutMicros >= (~0ull / 1000ull);
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t spins = 0u;; ++spins) {
        const CUresult r = cu_.EventQuery(p.done);
        if (r == CUDA_SUCCESS) break;
        if (r != CUDA_ERROR_NOT_READY) return Result::Lost;
        if (!forever) {
            const uint64_t waited = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count());
            if (waited >= timeoutMicros) return Result::Late;   // caller abandons
        }
        // A short spin keeps the wake-up latency low; after that give the core
        // back.
        if (spins > 2000u) std::this_thread::yield();
    }
    if (p.voiceTimed || p.envTimed || profile_) {
        for (uint32_t q = 0; q < 3u; ++q) {
            if ((q == 0u && !p.voiceTimed) || (q == 2u && !p.envTimed)) continue;
            const uint32_t slot = q == 0u ? 0u : (q == 1u ? 2u : 4u);
            float ms = 0.0f;
            if (cu_.EventElapsedTime(&ms, p.timed[slot], p.timed[slot + 1u]) == CUDA_SUCCESS)
                profileNs_[q] += static_cast<uint64_t>(ms * 1.0e6f);
        }
    }
    ++partsCollected_;
    *mixLR = static_cast<const float*>(host_[kMixRead]) +
             static_cast<size_t>(part) * p.frames * 2u;
    *dyn = static_cast<const DynOut*>(host_[kDynRead]);
    *envDyn = static_cast<const EnvDynOut*>(host_[kEnvDynRead]);
    return Result::Ok;
}

void CudaBackend::FreeAll() {
    for (CUdeviceptr& b : buf_) {
        if (b) cu_.MemFree(b);
        b = 0u;
    }
    for (void*& h : host_) {
        if (h) cu_.MemFreeHost(h);
        h = nullptr;
    }
}

void CudaBackend::Shutdown() {
    if (!created_) return;
    if (ctx_ && cu_.CtxSetCurrent(ctx_) == CUDA_SUCCESS) {
        if (stream_) cu_.StreamSynchronize(stream_);
        for (Part& p : parts_) {
            if (p.done) cu_.EventDestroy(p.done);
            for (CUevent e : p.timed)
                if (e) cu_.EventDestroy(e);
            p = Part{};
        }
        FreeAll();
        if (stream_) cu_.StreamDestroy(stream_);
        if (module_) cu_.ModuleUnload(module_);
        cu_.PrimaryCtxRelease(device_);
        cu_.CtxSetCurrent(nullptr);
    }
    stream_ = nullptr;
    module_ = nullptr;
    ctx_ = nullptr;
    created_ = false;
    // The driver library stays loaded for the life of the process.
}

}  // namespace

std::unique_ptr<Backend> CreateCudaBackend() {
    return std::unique_ptr<Backend>(new CudaBackend());
}

}  // namespace gpu
}  // namespace svms
