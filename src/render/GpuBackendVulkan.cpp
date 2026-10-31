// Vulkan backend of the GPU render stage (any Vulkan 1.1 compute device).
//
// Same contract and buffer layout as the CUDA and OpenGL backends: the shaders
// are the GLSL of VoiceStageGLSL.h compiled to SPIR-V at build time (glslc;
// the GL uniforms become one push-constant block), so the three backends run
// the same kernels.  Nothing is imported at load time: vulkan-1.dll /
// libvulkan.so.1 is opened when the stage is enabled and the entry points are
// fetched by hand.  A build without the Vulkan SDK compiles this file without
// the shaders and Init reports it.
//
// Each part of a block is its own queue submission, so the GPU starts part 0
// while the CPU packs part 1.  Row uploads only record their scatter dispatch
// into the command buffer of the next part, so they cost no submission of
// their own.  The queue executes in order and every command buffer opens with
// a full memory barrier.
// Host-visible staging (lists, upload records, read-back) comes in two slots
// that alternate per block, so a late block the GPU is still working on never
// shares a buffer with the next one.

#include "render/GpuBackend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(SVMS_HAVE_VULKAN)

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "VoiceStageVulkan_spv.h"

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

#define SVMS_VK_INSTANCE_FNS(X)                                                \
    X(DestroyInstance) X(EnumeratePhysicalDevices)                             \
    X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceProperties2)             \
    X(GetPhysicalDeviceQueueFamilyProperties)                                  \
    X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(GetDeviceProcAddr)

#define SVMS_VK_DEVICE_FNS(X)                                                  \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer)        \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory)             \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory)                            \
    X(InvalidateMappedMemoryRanges) X(CreateShaderModule)                      \
    X(DestroyShaderModule) X(CreateDescriptorSetLayout)                        \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout)                      \
    X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline)      \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) \
    X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool)         \
    X(ResetCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer)        \
    X(EndCommandBuffer) X(CmdBindPipeline) X(CmdBindDescriptorSets)            \
    X(CmdPushConstants) X(CmdDispatch) X(CmdCopyBuffer)       \
    X(CmdPipelineBarrier) X(QueueSubmit) X(QueueWaitIdle) X(CreateFence)       \
    X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(WaitForFences)          \
    X(DeviceWaitIdle)

struct VkFns {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
#define X(n) PFN_vk##n n = nullptr;
    SVMS_VK_INSTANCE_FNS(X)
    SVMS_VK_DEVICE_FNS(X)
#undef X
};

// Push constants shared by every kernel (a superset; each reads its members).
struct Push {
    uint32_t count, frames, listBase, groupBase, retireBits;
    uint32_t groups, groupFirst, mixBase, base;
};
static_assert(sizeof(Push) == 36u, "push block of the shaders");

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override { Shutdown(); }

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
    const char* Name() const override { return name_.c_str(); }
    bool SupportsTimeline() const override { return true; }

private:
    static constexpr uint32_t kGroup = 128u;   // shader GROUP
    static constexpr uint32_t kTile = 4u;      // shader TILE
    static constexpr uint32_t kSlots = 2u;
    static constexpr uint32_t kBindings = 13u;
    static constexpr VkDeviceSize kStagingChunk = 32u << 20;

    enum Kernel { kVoice, kEnv, kReduce, kScatter, kEnvScatter, kKernelCount };
    enum class Mem { Device, Upload, Readback };

    struct Buf {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* map = nullptr;
        bool coherent = true;
    };

    struct Slot {
        Buf list, envList, upload, envUpload, mixRead, dynRead, envDynRead;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> cmds;
        std::vector<VkFence> fences;
        std::vector<uint8_t> submitted;
        uint32_t used = 0u;
    };

    struct Part {
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
        uint32_t slot = 0u;
        uint32_t count = 0u, envCount = 0u, frames = 0u;
        uint32_t listBase = 0u, envBase = 0u;
    };

    bool Ok(VkResult r, const char* what);
    bool LoadLibraryAndInstance(std::string& error);
    bool PickDevice(uint32_t adapterIndex, std::string& error);
    bool CreateDeviceObjects(std::string& error);
    bool AllocBuf(Buf& b, VkDeviceSize bytes, VkBufferUsageFlags usage, Mem kind);
    void FreeBuf(Buf& b);
    void UpdateDescriptors();
    bool OpenBlock();
    bool Recording(VkCommandBuffer& cmd, VkFence& fence);
    bool BeginOp(VkCommandBuffer& cmd, VkFence& fence);
    bool EndOp(VkCommandBuffer cmd, VkFence fence);
    void Barrier(VkCommandBuffer cmd, bool toHost);
    void Dispatch(VkCommandBuffer cmd, Kernel k, const Push& push, uint32_t groups);
    void WaitIdle();
    void Shutdown();

    VkFns vk_;
#if defined(_WIN32)
    HMODULE lib_ = nullptr;
#else
    void* lib_ = nullptr;
#endif
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0u;
    VkPhysicalDeviceMemoryProperties memProps_ = {};
    VkDeviceSize maxStorageRange_ = 0u;
    bool subgroup32_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline pipes_[kKernelCount] = {};
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    Buf store_, rows_, envRows_, state_, timeline_, partial_, mix_, dyn_, envDyn_, staging_;
    Slot slots_[kSlots];
    Part parts_[kMaxParts];
    uint32_t curSlot_ = kSlots - 1u;
    bool blockOpen_ = false;
    // Command buffer collecting row uploads until the next part is submitted.
    bool pending_ = false;
    VkCommandBuffer pendingCmd_ = VK_NULL_HANDLE;
    VkFence pendingFence_ = VK_NULL_HANDLE;
    bool lost_ = false;
    uint32_t partsSubmitted_ = 0u, partsCollected_ = 0u;
    uint32_t listBase_ = 0u, envBase_ = 0u, groupBase_ = 0u;
    uint32_t uploadCursor_ = 0u, envUploadCursor_ = 0u;
    uint32_t handleCapacity_ = 0u, maxFrames_ = 0u;
    std::string name_ = "vulkan";
    bool created_ = false;
};

template <typename F>
bool Fetch(void* lib, F& slot, const char* name) {
#if defined(_WIN32)
    slot = reinterpret_cast<F>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    slot = reinterpret_cast<F>(dlsym(lib, name));
#endif
    return slot != nullptr;
}

bool VulkanBackend::Ok(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    if (r == VK_ERROR_DEVICE_LOST) lost_ = true;
    name_ += std::string(" [") + what + ": " + std::to_string(static_cast<int>(r)) + "]";
    return false;
}

bool VulkanBackend::LoadLibraryAndInstance(std::string& error) {
#if defined(_WIN32)
    lib_ = LoadLibraryW(L"vulkan-1.dll");
#else
    lib_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib_) {
        error = "the Vulkan loader (vulkan-1) is not available";
        return false;
    }
    if (!Fetch(lib_, vk_.GetInstanceProcAddr, "vkGetInstanceProcAddr")) {
        error = "the Vulkan loader is missing vkGetInstanceProcAddr";
        return false;
    }
    vk_.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        vk_.GetInstanceProcAddr(nullptr, "vkCreateInstance"));
    vk_.EnumerateInstanceLayerProperties =
        reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            vk_.GetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"));
    if (!vk_.CreateInstance) {
        error = "the Vulkan loader is missing vkCreateInstance";
        return false;
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SuperVirtualMIDISynth";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    // SVMS_VK_VALIDATE=1 turns the Khronos validation layer on when installed.
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (std::getenv("SVMS_VK_VALIDATE") != nullptr && vk_.EnumerateInstanceLayerProperties) {
        uint32_t n = 0u;
        vk_.EnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> layers(n);
        if (n) vk_.EnumerateInstanceLayerProperties(&n, layers.data());
        for (const VkLayerProperties& l : layers) {
            if (std::strcmp(l.layerName, layer) == 0) {
                ci.enabledLayerCount = 1u;
                ci.ppEnabledLayerNames = &layer;
            }
        }
    }
    const VkResult r = vk_.CreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        error = r == VK_ERROR_INCOMPATIBLE_DRIVER
                    ? "no Vulkan 1.1 driver"
                    : "vkCreateInstance failed (" + std::to_string(static_cast<int>(r)) + ")";
        return false;
    }
    bool ok = true;
#define X(n) ok = (vk_.n = reinterpret_cast<PFN_vk##n>( \
                       vk_.GetInstanceProcAddr(instance_, "vk" #n))) != nullptr && ok;
    SVMS_VK_INSTANCE_FNS(X)
#undef X
    if (!ok) {
        error = "the Vulkan loader is missing instance entry points";
        return false;
    }
    return true;
}

bool VulkanBackend::PickDevice(uint32_t adapterIndex, std::string& error) {
    uint32_t n = 0u;
    vk_.EnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    if (n) vk_.EnumeratePhysicalDevices(instance_, &n, devices.data());
    struct Candidate {
        VkPhysicalDevice device;
        uint32_t family;
        int rank;
    };
    std::vector<Candidate> found;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vk_.GetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t qn = 0u;
        vk_.GetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> fams(qn);
        if (qn) vk_.GetPhysicalDeviceQueueFamilyProperties(d, &qn, fams.data());
        // A compute family without graphics keeps the work off the 3D engine.
        int best = -1;
        uint32_t family = 0u;
        for (uint32_t i = 0u; i < qn; ++i) {
            if ((fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0u) continue;
            const int score = (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? 1 : 2;
            if (score > best) {
                best = score;
                family = i;
            }
        }
        if (best < 0) continue;
        // Discrete GPUs first, then integrated, then the rest.
        const int rank = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0
                         : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 2;
        found.push_back({d, family, rank});
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });
    if (adapterIndex >= found.size()) {
        error = "no Vulkan compute device at index " + std::to_string(adapterIndex);
        return false;
    }
    phys_ = found[adapterIndex].device;
    family_ = found[adapterIndex].family;

    VkPhysicalDeviceSubgroupProperties sub = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &sub;
    vk_.GetPhysicalDeviceProperties2(phys_, &props2);
    const VkPhysicalDeviceProperties& props = props2.properties;
    if (props.limits.maxComputeWorkGroupInvocations < kGroup ||
        props.limits.maxComputeWorkGroupSize[0] < kGroup) {
        error = std::string("the Vulkan device ") + props.deviceName +
                " cannot run 128-wide workgroups";
        return false;
    }
    maxStorageRange_ = props.limits.maxStorageBufferRange;
    const VkSubgroupFeatureFlags need =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    subgroup32_ = sub.subgroupSize == 32u &&
                  (sub.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0u &&
                  (sub.supportedOperations & need) == need &&
                  std::getenv("SVMS_GPU_NO_SUBGROUP") == nullptr;
    name_ = std::string("vulkan: ") + props.deviceName + 
            (subgroup32_ ? " [subgroup]" : "");
    vk_.GetPhysicalDeviceMemoryProperties(phys_, &memProps_);
    return true;
}

bool VulkanBackend::CreateDeviceObjects(std::string& error) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1u;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1u;
    dci.pQueueCreateInfos = &qci;
    if (!Ok(vk_.CreateDevice(phys_, &dci, nullptr, &device_), "device")) {
        error = name_;
        return false;
    }
    bool ok = true;
#define X(n) ok = (vk_.n = reinterpret_cast<PFN_vk##n>( \
                       vk_.GetDeviceProcAddr(device_, "vk" #n))) != nullptr && ok;
    SVMS_VK_DEVICE_FNS(X)
#undef X
    if (!ok) {
        error = "the Vulkan driver is missing device entry points";
        return false;
    }
    vk_.GetDeviceQueue(device_, family_, 0u, &queue_);

    VkDescriptorSetLayoutBinding binds[kBindings];
    for (uint32_t i = 0u; i < kBindings; ++i) {
        binds[i] = {};
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1u;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = kBindings;
    sli.pBindings = binds;
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1u;
    pli.pushConstantRangeCount = 1u;
    pli.pPushConstantRanges = &range;
    ok = Ok(vk_.CreateDescriptorSetLayout(device_, &sli, nullptr, &setLayout_), "set layout");
    pli.pSetLayouts = &setLayout_;
    ok = ok && Ok(vk_.CreatePipelineLayout(device_, &pli, nullptr, &pipeLayout_), "pipeline layout");

    struct Code {
        const unsigned int* words;
        size_t bytes;
    };
#define SVMS_CODE(n) Code{svms_spv_##n, sizeof(svms_spv_##n)}
    const Code code[kKernelCount] = {
        subgroup32_ ? SVMS_CODE(voice_sg) : SVMS_CODE(voice),
        subgroup32_ ? SVMS_CODE(env_sg) : SVMS_CODE(env),
        SVMS_CODE(reduce), SVMS_CODE(scatter), SVMS_CODE(env_scatter)};
#undef SVMS_CODE
    for (uint32_t k = 0u; ok && k < kKernelCount; ++k) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code[k].bytes;
        mi.pCode = code[k].words;
        VkShaderModule module = VK_NULL_HANDLE;
        ok = Ok(vk_.CreateShaderModule(device_, &mi, nullptr, &module), "shader module");
        if (!ok) break;
        VkComputePipelineCreateInfo pi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = module;
        pi.stage.pName = "main";
        pi.layout = pipeLayout_;
        ok = Ok(vk_.CreateComputePipelines(device_, VK_NULL_HANDLE, 1u, &pi, nullptr,
                                           &pipes_[k]), "pipeline");
        vk_.DestroyShaderModule(device_, module, nullptr);
    }
    if (!ok) {
        error = name_;
        return false;
    }

    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindings * kSlots};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kSlots;
    dpi.poolSizeCount = 1u;
    dpi.pPoolSizes = &size;
    ok = Ok(vk_.CreateDescriptorPool(device_, &dpi, nullptr, &descPool_), "descriptor pool");
    for (Slot& s : slots_) {
        if (!ok) break;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descPool_;
        ai.descriptorSetCount = 1u;
        ai.pSetLayouts = &setLayout_;
        ok = Ok(vk_.AllocateDescriptorSets(device_, &ai, &s.set), "descriptor set");
        VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = family_;
        ok = ok && Ok(vk_.CreateCommandPool(device_, &cpi, nullptr, &s.pool), "command pool");
    }
    if (!ok) error = name_;
    return ok;
}

bool VulkanBackend::Init(uint32_t adapterIndex, std::string& error) {
    created_ = true;
    if (!LoadLibraryAndInstance(error) || !PickDevice(adapterIndex, error) ||
        !CreateDeviceObjects(error)) {
        if (error.empty()) error = name_;
        return false;
    }
    return true;
}

bool VulkanBackend::AllocBuf(Buf& b, VkDeviceSize bytes, VkBufferUsageFlags usage, Mem kind) {
    if (bytes == 0u) bytes = 16u;
    bytes = (bytes + 15u) & ~static_cast<VkDeviceSize>(15u);
    if (b.buf && b.size == bytes) return true;
    FreeBuf(b);
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Ok(vk_.CreateBuffer(device_, &bi, nullptr, &b.buf), "buffer")) return false;
    VkMemoryRequirements req;
    vk_.GetBufferMemoryRequirements(device_, b.buf, &req);

    constexpr VkMemoryPropertyFlags kLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr VkMemoryPropertyFlags kVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr VkMemoryPropertyFlags kCoherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    constexpr VkMemoryPropertyFlags kCached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkMemoryPropertyFlags tries[4] = {};
    uint32_t tryCount = 0u;
    switch (kind) {
        case Mem::Device:
            tries[tryCount++] = kLocal;
            tries[tryCount++] = 0u;
            break;
        case Mem::Upload:
            tries[tryCount++] = kVisible | kCoherent | kLocal;
            tries[tryCount++] = kVisible | kCoherent;
            break;
        case Mem::Readback:
            tries[tryCount++] = kVisible | kCached | kCoherent;
            tries[tryCount++] = kVisible | kCached;
            tries[tryCount++] = kVisible | kCoherent;
            tries[tryCount++] = kVisible;
            break;
    }
    VkMemoryPropertyFlags chosen = 0u;
    for (uint32_t t = 0u; t < tryCount && b.mem == VK_NULL_HANDLE; ++t) {
        for (uint32_t i = 0u; i < memProps_.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags have = memProps_.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) == 0u || (have & tries[t]) != tries[t]) continue;
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = i;
            if (vk_.AllocateMemory(device_, &ai, nullptr, &b.mem) == VK_SUCCESS) {
                chosen = have;
                break;
            }
            b.mem = VK_NULL_HANDLE;
        }
    }
    if (b.mem == VK_NULL_HANDLE) {
        name_ += " [alloc: out of memory]";
        vk_.DestroyBuffer(device_, b.buf, nullptr);
        b.buf = VK_NULL_HANDLE;
        return false;
    }
    b.size = bytes;
    if (!Ok(vk_.BindBufferMemory(device_, b.buf, b.mem, 0u), "bind")) return false;
    if (kind != Mem::Device) {
        if (!Ok(vk_.MapMemory(device_, b.mem, 0u, VK_WHOLE_SIZE, 0u, &b.map), "map"))
            return false;
        b.coherent = (chosen & kCoherent) != 0u;
    }
    return true;
}

void VulkanBackend::FreeBuf(Buf& b) {
    if (b.map) vk_.UnmapMemory(device_, b.mem);
    if (b.buf) vk_.DestroyBuffer(device_, b.buf, nullptr);
    if (b.mem) vk_.FreeMemory(device_, b.mem, nullptr);
    b = Buf{};
}

void VulkanBackend::UpdateDescriptors() {
    if (!store_.buf || !rows_.buf) return;
    for (Slot& s : slots_) {
        const Buf* order[kBindings] = {&store_, &rows_, &s.list, &partial_, &mix_, &dyn_,
                                       &s.upload, &envRows_, &s.envList, &envDyn_,
                                       &s.envUpload, &state_, &timeline_};
        VkDescriptorBufferInfo infos[kBindings];
        VkWriteDescriptorSet writes[kBindings];
        for (uint32_t i = 0u; i < kBindings; ++i) {
            infos[i] = {order[i]->buf, 0u, VK_WHOLE_SIZE};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = s.set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1u;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vk_.UpdateDescriptorSets(device_, kBindings, writes, 0u, nullptr);
    }
}

void VulkanBackend::WaitIdle() {
    if (device_) vk_.DeviceWaitIdle(device_);
}

bool VulkanBackend::UploadStore(const int16_t* data, uint32_t frames) {
    Abandon();
    WaitIdle();
    // frames + 8 elements are readable (the sample-store padding contract);
    // whole 32-bit words only, plus one spare zero word the kernels may read.
    const VkDeviceSize bytes = ((static_cast<VkDeviceSize>(frames) + 8u) / 2u) * 4u;
    const VkDeviceSize total = bytes + 16u;
    if (total > maxStorageRange_) {
        name_ += " [store: larger than the device's storage buffer range]";
        return false;
    }
    if (!AllocBuf(store_, total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, Mem::Device) ||
        !AllocBuf(staging_, (std::min)(total, kStagingChunk),
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Mem::Upload)) {
        return false;
    }
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = family_;
    if (!Ok(vk_.CreateCommandPool(device_, &cpi, nullptr, &pool), "store pool")) return false;
    bool ok = true;
    for (VkDeviceSize done = 0u; ok && done < total;) {
        const VkDeviceSize n = (std::min)(total - done, staging_.size);
        const VkDeviceSize real = done < bytes ? (std::min)(n, bytes - done) : 0u;
        if (real) std::memcpy(staging_.map, reinterpret_cast<const char*>(data) + done, real);
        if (real < n) std::memset(static_cast<char*>(staging_.map) + real, 0, n - real);
        vk_.ResetCommandPool(device_, pool, 0u);
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1u;
        ok = Ok(vk_.AllocateCommandBuffers(device_, &ai, &cmd), "store cmd");
        if (!ok) break;
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_.BeginCommandBuffer(cmd, &bi);
        const VkBufferCopy region = {0u, done, n};
        vk_.CmdCopyBuffer(cmd, staging_.buf, store_.buf, 1u, &region);
        vk_.EndCommandBuffer(cmd);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1u;
        si.pCommandBuffers = &cmd;
        ok = Ok(vk_.QueueSubmit(queue_, 1u, &si, VK_NULL_HANDLE), "store submit") &&
             Ok(vk_.QueueWaitIdle(queue_), "store wait");
        done += n;
    }
    vk_.DestroyCommandPool(device_, pool, nullptr);
    if (!ok) return false;
    FreeBuf(staging_);
    UpdateDescriptors();
    return true;
}

bool VulkanBackend::Reserve(uint32_t handleCapacity, uint32_t maxFrames) {
    if (handleCapacity == handleCapacity_ && maxFrames == maxFrames_) return true;
    Abandon();
    WaitIdle();
    const VkDeviceSize cap = handleCapacity;
    // Sustained and envelope rows each round their last group up (each part
    // rounds two kernels' last groups up).
    const VkDeviceSize groups = (cap + kGroup - 1u) / kGroup + 2u * kMaxParts;
    const VkDeviceSize mixBytes = static_cast<VkDeviceSize>(maxFrames) * 2u * kMaxParts * 4u;
    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags kSrc = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    constexpr VkBufferUsageFlags kDst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bool ok =
        AllocBuf(rows_, cap * kRowWords * 4u, kStorage, Mem::Device) &&
        AllocBuf(envRows_, cap * kEnvRowWords * 4u, kStorage, Mem::Device) &&
        AllocBuf(state_, cap * kDynWords * 4u, kStorage, Mem::Device) &&
        AllocBuf(timeline_, cap * 16u, kStorage, Mem::Device) &&
        AllocBuf(partial_, groups * maxFrames * 2u * 4u, kStorage, Mem::Device) &&
        AllocBuf(mix_, mixBytes, kSrc, Mem::Device) &&
        AllocBuf(dyn_, cap * sizeof(DynOut), kSrc, Mem::Device) &&
        AllocBuf(envDyn_, cap * sizeof(EnvDynOut), kSrc, Mem::Device);
    for (Slot& s : slots_) {
        ok = ok &&
             AllocBuf(s.list, cap * 4u, kStorage, Mem::Upload) &&
             AllocBuf(s.envList, cap * 4u, kStorage, Mem::Upload) &&
             AllocBuf(s.upload, cap * sizeof(UploadRecord), kStorage, Mem::Upload) &&
             AllocBuf(s.envUpload, cap * sizeof(EnvUploadRecord), kStorage, Mem::Upload) &&
             AllocBuf(s.mixRead, mixBytes, kDst, Mem::Readback) &&
             AllocBuf(s.dynRead, cap * sizeof(DynOut), kDst, Mem::Readback) &&
             AllocBuf(s.envDynRead, cap * sizeof(EnvDynOut), kDst, Mem::Readback);
    }
    if (!ok) return false;
    handleCapacity_ = handleCapacity;
    maxFrames_ = maxFrames;
    uploadCursor_ = envUploadCursor_ = 0u;
    UpdateDescriptors();
    return true;
}

bool VulkanBackend::OpenBlock() {
    curSlot_ = (curSlot_ + 1u) % kSlots;
    Slot& s = slots_[curSlot_];
    // The block this slot served two blocks ago is normally long finished. A
    // GPU that is busy with other work (a window being drawn) can still hold
    // it; this waits a moment, then declines the block so the CPU renders it
    // instead of the audio thread sitting on the fence.
    std::vector<VkFence> pending;
    for (size_t i = 0u; i < s.submitted.size(); ++i)
        if (s.submitted[i]) pending.push_back(s.fences[i]);
    if (!pending.empty()) {
        const VkResult w = vk_.WaitForFences(device_, static_cast<uint32_t>(pending.size()),
                                             pending.data(), VK_TRUE, 1500000ull);
        if (w != VK_SUCCESS) {
            if (w == VK_ERROR_DEVICE_LOST) lost_ = true;
            return false;
        }
        vk_.ResetFences(device_, static_cast<uint32_t>(pending.size()), pending.data());
    }
    std::fill(s.submitted.begin(), s.submitted.end(), uint8_t(0));
    vk_.ResetCommandPool(device_, s.pool, 0u);
    s.used = 0u;
    blockOpen_ = true;
    return true;
}

// The command buffer of the part being built: opened (with its leading barrier)
// by the first upload or by the part itself.
bool VulkanBackend::Recording(VkCommandBuffer& cmd, VkFence& fence) {
    if (!pending_) {
        if (!BeginOp(pendingCmd_, pendingFence_)) return false;
        Barrier(pendingCmd_, false);
        pending_ = true;
    }
    cmd = pendingCmd_;
    fence = pendingFence_;
    return true;
}

bool VulkanBackend::BeginOp(VkCommandBuffer& cmd, VkFence& fence) {
    Slot& s = slots_[curSlot_];
    if (s.used == s.cmds.size()) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1u;
        VkCommandBuffer fresh = VK_NULL_HANDLE;
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence newFence = VK_NULL_HANDLE;
        if (!Ok(vk_.AllocateCommandBuffers(device_, &ai, &fresh), "cmd alloc") ||
            !Ok(vk_.CreateFence(device_, &fi, nullptr, &newFence), "fence")) {
            return false;
        }
        s.cmds.push_back(fresh);
        s.fences.push_back(newFence);
        s.submitted.push_back(0u);
    }
    cmd = s.cmds[s.used];
    fence = s.fences[s.used];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Ok(vk_.BeginCommandBuffer(cmd, &bi), "begin")) return false;
    ++s.used;
    return true;
}

bool VulkanBackend::EndOp(VkCommandBuffer cmd, VkFence fence) {
    Slot& s = slots_[curSlot_];
    if (!Ok(vk_.EndCommandBuffer(cmd), "end")) return false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &cmd;
    if (!Ok(vk_.QueueSubmit(queue_, 1u, &si, fence), "submit")) return false;
    s.submitted[s.used - 1u] = 1u;
    return true;
}

void VulkanBackend::Barrier(VkCommandBuffer cmd, bool toHost) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    VkPipelineStageFlags dst;
    if (toHost) {
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        dst = VK_PIPELINE_STAGE_HOST_BIT;
    } else {
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    vk_.CmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           dst, 0u, 1u, &mb, 0u, nullptr, 0u, nullptr);
}

void VulkanBackend::Dispatch(VkCommandBuffer cmd, Kernel k, const Push& push, uint32_t groups) {
    vk_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes_[k]);
    vk_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0u, 1u,
                              &slots_[curSlot_].set, 0u, nullptr);
    vk_.CmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(Push), &push);
    vk_.CmdDispatch(cmd, groups, 1u, 1u);
}

bool VulkanBackend::UploadRows(const UploadRecord* records, uint32_t count) {
    if (count == 0u) return true;
    if (lost_ || uploadCursor_ + count > handleCapacity_) return false;
    if (!blockOpen_ && !OpenBlock()) return false;
    Slot& s = slots_[curSlot_];
    std::memcpy(static_cast<char*>(s.upload.map) +
                    static_cast<size_t>(uploadCursor_) * sizeof(UploadRecord),
                records, static_cast<size_t>(count) * sizeof(UploadRecord));
    VkCommandBuffer cmd;
    VkFence fence;
    if (!Recording(cmd, fence)) return false;
    Push push = {};
    push.count = count;
    push.base = uploadCursor_;
    Dispatch(cmd, kScatter, push, (count + 63u) / 64u);
    Barrier(cmd, false);
    uploadCursor_ += count;
    return true;
}

bool VulkanBackend::UploadEnvRows(const EnvUploadRecord* records, uint32_t count) {
    if (count == 0u) return true;
    if (lost_ || envUploadCursor_ + count > handleCapacity_) return false;
    if (!blockOpen_ && !OpenBlock()) return false;
    Slot& s = slots_[curSlot_];
    std::memcpy(static_cast<char*>(s.envUpload.map) +
                    static_cast<size_t>(envUploadCursor_) * sizeof(EnvUploadRecord),
                records, static_cast<size_t>(count) * sizeof(EnvUploadRecord));
    VkCommandBuffer cmd;
    VkFence fence;
    if (!Recording(cmd, fence)) return false;
    Push push = {};
    push.count = count;
    push.base = envUploadCursor_;
    Dispatch(cmd, kEnvScatter, push, (count + 63u) / 64u);
    Barrier(cmd, false);
    envUploadCursor_ += count;
    return true;
}

bool VulkanBackend::Submit(const uint32_t* list, uint32_t count, const uint32_t* envList,
                           uint32_t envCount, uint32_t frames, float retireThreshold,
                           bool last) {
    if (partsSubmitted_ != 0u && partsCollected_ == partsSubmitted_) {
        // The previous block is fully collected: this starts a new one.
        partsSubmitted_ = partsCollected_ = 0u;
        listBase_ = envBase_ = groupBase_ = 0u;
    }
    if (lost_ || listBase_ + count > handleCapacity_ ||
        envBase_ + envCount > handleCapacity_ || frames == 0u ||
        frames > maxFrames_ || partsSubmitted_ >= kMaxParts) {
        return false;
    }
    if (!blockOpen_ && !OpenBlock()) return false;
    const uint32_t part = partsSubmitted_;
    Slot& s = slots_[curSlot_];
    const uint32_t groups = (count + kGroup - 1u) / kGroup;
    const uint32_t envGroups = (envCount + kGroup - 1u) / kGroup;
    if (count != 0u)
        std::memcpy(static_cast<char*>(s.list.map) + static_cast<size_t>(listBase_) * 4u,
                    list, static_cast<size_t>(count) * 4u);
    if (envCount != 0u)
        std::memcpy(static_cast<char*>(s.envList.map) + static_cast<size_t>(envBase_) * 4u,
                    envList, static_cast<size_t>(envCount) * 4u);

    VkCommandBuffer cmd;
    VkFence fence;
    if (!Recording(cmd, fence)) return false;
    if (count != 0u) {
        Push push = {};
        push.count = count;
        push.frames = frames;
        push.listBase = listBase_;
        push.groupBase = groupBase_;
        Dispatch(cmd, kVoice, push, groups);
        Barrier(cmd, false);
    }
    if (envCount != 0u) {
        Push push = {};
        std::memcpy(&push.retireBits, &retireThreshold, sizeof(push.retireBits));
        push.count = envCount;
        push.frames = frames;
        push.listBase = envBase_;
        push.groupBase = groupBase_ + groups;
        Dispatch(cmd, kEnv, push, envGroups);
        Barrier(cmd, false);
    }
    {
        Push push = {};
        push.frames = frames;
        push.groups = groups + envGroups;
        push.groupFirst = groupBase_;
        push.mixBase = part * frames * 2u;
        Dispatch(cmd, kReduce, push, (frames * 2u + 63u) / 64u);
        Barrier(cmd, false);
    }
    const VkDeviceSize mixOffset = static_cast<VkDeviceSize>(part) * frames * 8u;
    const VkBufferCopy mixCopy = {mixOffset, mixOffset, static_cast<VkDeviceSize>(frames) * 8u};
    vk_.CmdCopyBuffer(cmd, mix_.buf, s.mixRead.buf, 1u, &mixCopy);
    if (count != 0u) {
        const VkDeviceSize o = static_cast<VkDeviceSize>(listBase_) * sizeof(DynOut);
        const VkBufferCopy c = {o, o, static_cast<VkDeviceSize>(count) * sizeof(DynOut)};
        vk_.CmdCopyBuffer(cmd, dyn_.buf, s.dynRead.buf, 1u, &c);
    }
    if (envCount != 0u) {
        const VkDeviceSize o = static_cast<VkDeviceSize>(envBase_) * sizeof(EnvDynOut);
        const VkBufferCopy c = {o, o, static_cast<VkDeviceSize>(envCount) * sizeof(EnvDynOut)};
        vk_.CmdCopyBuffer(cmd, envDyn_.buf, s.envDynRead.buf, 1u, &c);
    }
    Barrier(cmd, true);
    pending_ = false;
    if (!EndOp(cmd, fence)) return false;

    Part& p = parts_[part];
    p.fence = fence;
    p.submitted = true;
    p.slot = curSlot_;
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
        blockOpen_ = false;
    }
    return true;
}

void VulkanBackend::Abandon() {
    // The GPU may still finish the parts in flight; nothing reads the results,
    // and the next block uses the other slot and queues behind them.
    for (Part& p : parts_) {
        p.submitted = false;
        p.count = p.envCount = p.frames = 0u;
    }
    partsSubmitted_ = partsCollected_ = 0u;
    listBase_ = envBase_ = groupBase_ = 0u;
    uploadCursor_ = envUploadCursor_ = 0u;
    blockOpen_ = false;
    // Uploads recorded but not yet submitted are dropped with the block (the
    // slot's pool is reset when it is next opened).
    pending_ = false;
}

Result VulkanBackend::Collect(uint64_t timeoutMicros, const float** mixLR,
                              const DynOut** dyn, const EnvDynOut** envDyn) {
    if (lost_) return Result::Lost;
    if (partsCollected_ >= partsSubmitted_) return Result::Idle;
    const uint32_t part = partsCollected_;
    Part& p = parts_[part];
    if (!p.submitted) return Result::Idle;
    const bool forever = timeoutMicros >= (~0ull / 1000ull);
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t spins = 0u;; ++spins) {
        const VkResult r = vk_.GetFenceStatus(device_, p.fence);
        if (r == VK_SUCCESS) break;
        if (r != VK_NOT_READY) {
            lost_ = true;
            return Result::Lost;
        }
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
    Slot& s = slots_[p.slot];
    for (Buf* b : {&s.mixRead, &s.dynRead, &s.envDynRead}) {
        if (b->coherent) continue;
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = b->mem;
        range.size = VK_WHOLE_SIZE;
        vk_.InvalidateMappedMemoryRanges(device_, 1u, &range);
    }
    ++partsCollected_;
    *mixLR = reinterpret_cast<const float*>(s.mixRead.map) +
             static_cast<size_t>(part) * p.frames * 2u;
    *dyn = static_cast<const DynOut*>(s.dynRead.map);
    *envDyn = static_cast<const EnvDynOut*>(s.envDynRead.map);
    return Result::Ok;
}

void VulkanBackend::Shutdown() {
    if (!created_) return;
    if (device_) {
        WaitIdle();
        for (Slot& s : slots_) {
            for (VkFence f : s.fences) vk_.DestroyFence(device_, f, nullptr);
            if (s.pool) vk_.DestroyCommandPool(device_, s.pool, nullptr);
            for (Buf* b : {&s.list, &s.envList, &s.upload, &s.envUpload, &s.mixRead,
                           &s.dynRead, &s.envDynRead}) {
                FreeBuf(*b);
            }
        }
        for (Buf* b : {&store_, &rows_, &envRows_, &state_, &timeline_, &partial_, &mix_, &dyn_,
                       &envDyn_, &staging_}) {
            FreeBuf(*b);
        }
        for (VkPipeline p : pipes_)
            if (p) vk_.DestroyPipeline(device_, p, nullptr);
        if (descPool_) vk_.DestroyDescriptorPool(device_, descPool_, nullptr);
        if (pipeLayout_) vk_.DestroyPipelineLayout(device_, pipeLayout_, nullptr);
        if (setLayout_) vk_.DestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        vk_.DestroyDevice(device_, nullptr);
    }
    if (instance_ && vk_.DestroyInstance) vk_.DestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    created_ = false;
    // The loader stays loaded for the life of the process.
}

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() {
    return std::unique_ptr<Backend>(new VulkanBackend());
}

}  // namespace gpu
}  // namespace svms

#else  // !SVMS_HAVE_VULKAN

namespace svms {
namespace gpu {
namespace {

class NoVulkanBackend final : public Backend {
public:
    bool Init(uint32_t, std::string& error) override {
        error = "this build has no Vulkan shaders (built without the Vulkan SDK)";
        return false;
    }
    bool UploadStore(const int16_t*, uint32_t) override { return false; }
    bool Reserve(uint32_t, uint32_t) override { return false; }
    bool UploadRows(const UploadRecord*, uint32_t) override { return false; }
    bool UploadEnvRows(const EnvUploadRecord*, uint32_t) override { return false; }
    bool Submit(const uint32_t*, uint32_t, const uint32_t*, uint32_t, uint32_t, float,
                bool) override { return false; }
    Result Collect(uint64_t, const float**, const DynOut**, const EnvDynOut**) override {
        return Result::Lost;
    }
    void Abandon() override {}
    const char* Name() const override { return "vulkan"; }
};

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() {
    return std::unique_ptr<Backend>(new NoVulkanBackend());
}

}  // namespace gpu
}  // namespace svms

#endif
