// Vulkan ray-query backend of the stage tracer. See StageTracerVulkan.h.
//
// One acceleration structure is built per Run (the scene is a few hundred
// triangles, so it takes well under a millisecond). Rays are traced in batches
// of kBatch per submission, each batch short enough to stay far below the
// driver's watchdog, and a cancel flag is checked between batches.

#include "render/StageTracerVulkan.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(SVMS_HAVE_VULKAN_RT)

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "StageTrace_spv.h"

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
namespace stage {
namespace {

#define SVMS_RT_INSTANCE_FNS(X)                                                \
    X(DestroyInstance) X(EnumeratePhysicalDevices)                             \
    X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceProperties2)             \
    X(GetPhysicalDeviceFeatures2) X(EnumerateDeviceExtensionProperties)        \
    X(GetPhysicalDeviceQueueFamilyProperties)                                  \
    X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(GetDeviceProcAddr)

#define SVMS_RT_DEVICE_FNS(X)                                                  \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer)        \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory)             \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateShaderModule)      \
    X(DestroyShaderModule) X(CreateDescriptorSetLayout)                        \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout)                      \
    X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline)      \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) \
    X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool)         \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)        \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants)            \
    X(CmdDispatch) X(CmdPipelineBarrier) X(QueueSubmit) X(CreateFence)         \
    X(DestroyFence) X(ResetFences) X(WaitForFences) X(ResetCommandBuffer)      \
    X(GetBufferDeviceAddress)

#define SVMS_RT_AS_FNS(X)                                                      \
    X(CreateAccelerationStructureKHR) X(DestroyAccelerationStructureKHR)       \
    X(GetAccelerationStructureBuildSizesKHR)                                   \
    X(CmdBuildAccelerationStructuresKHR)                                       \
    X(GetAccelerationStructureDeviceAddressKHR)

struct Fns {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
#define X(n) PFN_vk##n n = nullptr;
    SVMS_RT_INSTANCE_FNS(X)
    SVMS_RT_DEVICE_FNS(X)
    SVMS_RT_AS_FNS(X)
#undef X
};

struct PushBlock {
    float listener[3];
    float radiusSq;
    uint32_t perSource, rayBase, rayCount, bins;
    float dt;
    uint32_t skipOrder, includeDirect, maxBounces, seed;
    float tmax;
    uint32_t pad0, pad1;
};
static_assert(sizeof(PushBlock) == 64u, "push block of the shader");

template <typename F>
bool Fetch(void* lib, F& slot, const char* name) {
#if defined(_WIN32)
    slot = reinterpret_cast<F>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    slot = reinterpret_cast<F>(dlsym(lib, name));
#endif
    return slot != nullptr;
}

class VulkanTracer final : public TraceBackend {
public:
    ~VulkanTracer() override { Shutdown(); }

    bool Init(std::string& error, std::string* deviceName) {
        if (!OpenLoader(error)) return false;
        if (!PickDevice(error)) return false;
        if (!CreateObjects(error)) return false;
        if (deviceName) *deviceName = name_;
        return true;
    }

    EnergyHistogram Run(const TraceScene& scene, const std::vector<Vec3>& sources,
                        const Vec3& listener, const TraceOptions& opt,
                        const std::atomic<bool>* cancel) override;

private:
    struct Buf {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* map = nullptr;
        VkDeviceAddress address = 0;
    };

    bool OpenLoader(std::string& error);
    bool PickDevice(std::string& error);
    bool CreateObjects(std::string& error);
    bool AllocBuf(Buf& b, VkDeviceSize bytes, VkBufferUsageFlags usage, bool address);
    void FreeBuf(Buf& b);
    bool SubmitAndWait(VkCommandBuffer cmd);
    bool BuildScene(const TraceScene& scene, const std::vector<Vec3>& sources);
    void FreeScene();
    void Shutdown();

    Fns vk_;
#if defined(_WIN32)
    HMODULE lib_ = nullptr;
#else
    void* lib_ = nullptr;
#endif
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkPhysicalDeviceMemoryProperties mem_ = {};
    VkDeviceSize scratchAlign_ = 256;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::string name_;

    // Per-run scene objects.
    Buf vertices_, instances_, blasBuf_, tlasBuf_, scratch_, tris_, srcs_, hist_;
    VkAccelerationStructureKHR blas_ = VK_NULL_HANDLE, tlas_ = VK_NULL_HANDLE;
};

bool VulkanTracer::OpenLoader(std::string& error) {
#if defined(_WIN32)
    lib_ = LoadLibraryW(L"vulkan-1.dll");
#else
    lib_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib_) { error = "the Vulkan loader (vulkan-1) is not available"; return false; }
    if (!Fetch(lib_, vk_.GetInstanceProcAddr, "vkGetInstanceProcAddr")) {
        error = "the Vulkan loader is missing vkGetInstanceProcAddr";
        return false;
    }
    vk_.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        vk_.GetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!vk_.CreateInstance) { error = "the Vulkan loader is missing vkCreateInstance"; return false; }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SuperVirtualMIDISynth";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    const VkResult r = vk_.CreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) { error = "vkCreateInstance failed (Vulkan 1.2 needed)"; return false; }
    bool ok = true;
#define X(n) ok = (vk_.n = reinterpret_cast<PFN_vk##n>( \
                       vk_.GetInstanceProcAddr(instance_, "vk" #n))) != nullptr && ok;
    SVMS_RT_INSTANCE_FNS(X)
#undef X
    if (!ok) { error = "the Vulkan loader is missing instance entry points"; return false; }
    return true;
}

bool VulkanTracer::PickDevice(std::string& error) {
    uint32_t n = 0;
    vk_.EnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    if (n) vk_.EnumeratePhysicalDevices(instance_, &n, devices.data());
    int bestRank = 99;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vk_.GetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_2) continue;
        uint32_t en = 0;
        vk_.EnumerateDeviceExtensionProperties(d, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        if (en) vk_.EnumerateDeviceExtensionProperties(d, nullptr, &en, exts.data());
        auto has = [&](const char* name) {
            for (const auto& e : exts) if (std::strcmp(e.extensionName, name) == 0) return true;
            return false;
        };
        if (!has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) || !has(VK_KHR_RAY_QUERY_EXTENSION_NAME) ||
            !has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) ||
            !has(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME))
            continue;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayQueryFeaturesKHR rq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f12; f12.pNext = &as; as.pNext = &rq; rq.pNext = &af;
        vk_.GetPhysicalDeviceFeatures2(d, &f2);
        if (!as.accelerationStructure || !rq.rayQuery || !af.shaderBufferFloat32AtomicAdd ||
            !f12.bufferDeviceAddress)
            continue;
        uint32_t qn = 0;
        vk_.GetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> fams(qn);
        if (qn) vk_.GetPhysicalDeviceQueueFamilyProperties(d, &qn, fams.data());
        int family = -1;
        for (uint32_t i = 0; i < qn; ++i)
            if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = static_cast<int>(i); break; }
        if (family < 0) continue;
        const int rank = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0 : 1;
        if (rank < bestRank) {
            bestRank = rank;
            phys_ = d;
            family_ = static_cast<uint32_t>(family);
            name_ = props.deviceName;
        }
    }
    if (phys_ == VK_NULL_HANDLE) {
        error = "no Vulkan device with ray queries, acceleration structures and float atomics";
        return false;
    }
    return true;
}

bool VulkanTracer::CreateObjects(std::string& error) {
    vk_.GetPhysicalDeviceMemoryProperties(phys_, &mem_);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps =
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &asProps;
    vk_.GetPhysicalDeviceProperties2(phys_, &p2);
    scratchAlign_ = std::max<VkDeviceSize>(256, asProps.minAccelerationStructureScratchOffsetAlignment);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    af.shaderBufferFloat32AtomicAdd = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR rq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE;
    rq.pNext = &af;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    as.accelerationStructure = VK_TRUE;
    as.pNext = &rq;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    f12.pNext = &as;
    const char* exts[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                          VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                          VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 4;
    dci.ppEnabledExtensionNames = exts;
    if (vk_.CreateDevice(phys_, &dci, nullptr, &device_) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        return false;
    }
    bool ok = true;
    auto dev = [&](const char* n) { return vk_.GetDeviceProcAddr(device_, n); };
#define X(n) ok = (vk_.n = reinterpret_cast<PFN_vk##n>(dev("vk" #n))) != nullptr && ok;
    SVMS_RT_DEVICE_FNS(X)
    SVMS_RT_AS_FNS(X)
#undef X
    if (!ok) { error = "missing device entry points"; return false; }
    vk_.GetDeviceQueue(device_, family_, 0, &queue_);

    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(svms_spv_stage_trace);
    smi.pCode = svms_spv_stage_trace;
    VkShaderModule module;
    if (vk_.CreateShaderModule(device_, &smi, nullptr, &module) != VK_SUCCESS) {
        error = "the trace shader was rejected";
        return false;
    }
    VkDescriptorSetLayoutBinding b[4] = {};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (uint32_t i = 1; i < 4; ++i) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
    for (auto& x : b) { x.descriptorCount = 1; x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 4;
    dl.pBindings = b;
    vk_.CreateDescriptorSetLayout(device_, &dl, nullptr, &setLayout_);
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock)};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    vk_.CreatePipelineLayout(device_, &pl, nullptr, &pipeLayout_);
    VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = module;
    cp.stage.pName = "main";
    cp.layout = pipeLayout_;
    const VkResult pr = vk_.CreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe_);
    vk_.DestroyShaderModule(device_, module, nullptr);
    if (pr != VK_SUCCESS) { error = "could not create the trace pipeline"; return false; }

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    vk_.CreateDescriptorPool(device_, &dp, nullptr, &pool_);
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = pool_;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &setLayout_;
    vk_.AllocateDescriptorSets(device_, &da, &set_);

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = family_;
    vk_.CreateCommandPool(device_, &cpi, nullptr, &cmdPool_);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmdPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vk_.AllocateCommandBuffers(device_, &cai, &cmd_);
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk_.CreateFence(device_, &fi, nullptr, &fence_);
    return true;
}

bool VulkanTracer::AllocBuf(Buf& b, VkDeviceSize bytes, VkBufferUsageFlags usage, bool address) {
    b.size = std::max<VkDeviceSize>(bytes, 16);
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = b.size;
    bi.usage = usage | (address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk_.CreateBuffer(device_, &bi, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk_.GetBufferMemoryRequirements(device_, b.buf, &req);
    // Host-visible and coherent for everything: the scene is small.
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mem_.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mem_.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = address ? &flags : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vk_.AllocateMemory(device_, &ai, nullptr, &b.mem) != VK_SUCCESS) return false;
    if (vk_.BindBufferMemory(device_, b.buf, b.mem, 0) != VK_SUCCESS) return false;
    if (vk_.MapMemory(device_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map) != VK_SUCCESS) return false;
    if (address) {
        VkBufferDeviceAddressInfo info = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = b.buf;
        b.address = vk_.GetBufferDeviceAddress(device_, &info);
    }
    return true;
}

void VulkanTracer::FreeBuf(Buf& b) {
    if (b.map) vk_.UnmapMemory(device_, b.mem);
    if (b.buf) vk_.DestroyBuffer(device_, b.buf, nullptr);
    if (b.mem) vk_.FreeMemory(device_, b.mem, nullptr);
    b = Buf{};
}

bool VulkanTracer::SubmitAndWait(VkCommandBuffer cmd) {
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vk_.ResetFences(device_, 1, &fence_);
    if (vk_.QueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return false;
    return vk_.WaitForFences(device_, 1, &fence_, VK_TRUE, 30ull * 1000000000ull) == VK_SUCCESS;
}

void VulkanTracer::FreeScene() {
    if (device_ == VK_NULL_HANDLE) return;
    if (tlas_) vk_.DestroyAccelerationStructureKHR(device_, tlas_, nullptr);
    if (blas_) vk_.DestroyAccelerationStructureKHR(device_, blas_, nullptr);
    tlas_ = blas_ = VK_NULL_HANDLE;
    for (Buf* b : {&vertices_, &instances_, &blasBuf_, &tlasBuf_, &scratch_, &tris_, &srcs_, &hist_}) FreeBuf(*b);
}

bool VulkanTracer::BuildScene(const TraceScene& scene, const std::vector<Vec3>& sources) {
    const uint32_t n = static_cast<uint32_t>(scene.tris.size());
    const VkBufferUsageFlags asInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (!AllocBuf(vertices_, sizeof(float) * 9ull * n, asInput | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    float* v = static_cast<float*>(vertices_.map);
    for (uint32_t i = 0; i < n; ++i) {
        const Triangle& t = scene.tris[i];
        const Vec3* p[3] = {&t.a, &t.b, &t.c};
        for (int k = 0; k < 3; ++k) { v[9 * i + 3 * k] = p[k]->x; v[9 * i + 3 * k + 1] = p[k]->y; v[9 * i + 3 * k + 2] = p[k]->z; }
    }
    // Per triangle: normal + scatter, then the energy kept per band.
    if (!AllocBuf(tris_, sizeof(float) * 8ull * n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) return false;
    float* td = static_cast<float*>(tris_.map);
    for (uint32_t i = 0; i < n; ++i) {
        const Triangle& t = scene.tris[i];
        const double ux = t.b.x - t.a.x, uy = t.b.y - t.a.y, uz = t.b.z - t.a.z;
        const double vx = t.c.x - t.a.x, vy = t.c.y - t.a.y, vz = t.c.z - t.a.z;
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        double l = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (l <= 0.0) l = 1.0;
        td[8 * i + 0] = static_cast<float>(nx / l);
        td[8 * i + 1] = static_cast<float>(ny / l);
        td[8 * i + 2] = static_cast<float>(nz / l);
        td[8 * i + 3] = t.scatter;
        for (int b = 0; b < kBands; ++b) td[8 * i + 4 + b] = 1.0f - t.absorption[b];
    }
    if (!AllocBuf(srcs_, sizeof(float) * 4ull * (std::max<size_t>)(1, sources.size()), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) return false;
    float* sd = static_cast<float*>(srcs_.map);
    for (size_t i = 0; i < sources.size(); ++i) { sd[4 * i] = sources[i].x; sd[4 * i + 1] = sources[i].y; sd[4 * i + 2] = sources[i].z; sd[4 * i + 3] = 0.0f; }

    // Bottom level: one triangle geometry, no index buffer.
    VkAccelerationStructureGeometryKHR geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo.geometry.triangles.vertexData.deviceAddress = vertices_.address;
    geo.geometry.triangles.vertexStride = 12;
    geo.geometry.triangles.maxVertex = 3 * n - 1;
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bi = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vk_.GetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &n, &sizes);
    if (!AllocBuf(blasBuf_, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, true)) return false;
    VkAccelerationStructureCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = blasBuf_.buf;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vk_.CreateAccelerationStructureKHR(device_, &ci, nullptr, &blas_) != VK_SUCCESS) return false;
    VkAccelerationStructureDeviceAddressInfoKHR ai = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = blas_;
    const VkDeviceAddress blasAddress = vk_.GetAccelerationStructureDeviceAddressKHR(device_, &ai);

    // Top level: one identity instance of the bottom level.
    if (!AllocBuf(instances_, sizeof(VkAccelerationStructureInstanceKHR), asInput, true)) return false;
    VkAccelerationStructureInstanceKHR inst = {};
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xFF;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = blasAddress;
    std::memcpy(instances_.map, &inst, sizeof(inst));
    VkAccelerationStructureGeometryKHR tgeo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tgeo.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    tgeo.geometry.instances.data.deviceAddress = instances_.address;
    VkAccelerationStructureBuildGeometryInfoKHR tbi = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tbi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tbi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tbi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tbi.geometryCount = 1;
    tbi.pGeometries = &tgeo;
    const uint32_t one = 1;
    VkAccelerationStructureBuildSizesInfoKHR tsizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vk_.GetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tbi, &one, &tsizes);
    if (!AllocBuf(tlasBuf_, tsizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, true)) return false;
    VkAccelerationStructureCreateInfoKHR tci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    tci.buffer = tlasBuf_.buf;
    tci.size = tsizes.accelerationStructureSize;
    tci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (vk_.CreateAccelerationStructureKHR(device_, &tci, nullptr, &tlas_) != VK_SUCCESS) return false;

    // One scratch buffer serves both builds (they run one after the other).
    const VkDeviceSize scratchBytes = (std::max)(sizes.buildScratchSize, tsizes.buildScratchSize) + scratchAlign_;
    if (!AllocBuf(scratch_, scratchBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    const VkDeviceAddress scratchAddr = (scratch_.address + scratchAlign_ - 1) & ~(scratchAlign_ - 1);
    bi.dstAccelerationStructure = blas_;
    bi.scratchData.deviceAddress = scratchAddr;
    tbi.dstAccelerationStructure = tlas_;
    tbi.scratchData.deviceAddress = scratchAddr;

    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.ResetCommandBuffer(cmd_, 0);
    vk_.BeginCommandBuffer(cmd_, &begin);
    VkAccelerationStructureBuildRangeInfoKHR range = {n, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = {&range};
    vk_.CmdBuildAccelerationStructuresKHR(cmd_, 1, &bi, ranges);
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                           VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkAccelerationStructureBuildRangeInfoKHR trange = {1, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* tranges[1] = {&trange};
    vk_.CmdBuildAccelerationStructuresKHR(cmd_, 1, &tbi, tranges);
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    vk_.EndCommandBuffer(cmd_);
    return SubmitAndWait(cmd_);
}

EnergyHistogram VulkanTracer::Run(const TraceScene& scene, const std::vector<Vec3>& sources,
                                  const Vec3& listener, const TraceOptions& opt,
                                  const std::atomic<bool>* cancel) {
    EnergyHistogram h;
    h.dt = opt.dt;
    h.bins = static_cast<int>(std::ceil(opt.tmax / opt.dt));
    h.radius = opt.radius;
    for (auto& v : h.sum) v.assign(static_cast<size_t>(h.bins), 0.0);
    if (sources.empty() || scene.tris.empty()) return h;
    const uint32_t per = static_cast<uint32_t>((std::max)(1, (opt.rays + static_cast<int>(sources.size()) - 1) /
                                                                 static_cast<int>(sources.size())));
    h.rays = static_cast<int>(per * sources.size());

    FreeScene();
    const bool built = BuildScene(scene, sources) &&
                       AllocBuf(hist_, sizeof(float) * kBands * static_cast<size_t>(h.bins),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    if (!built) { FreeScene(); h.cancelled = true; return h; }
    std::memset(hist_.map, 0, static_cast<size_t>(hist_.size));

    VkWriteDescriptorSetAccelerationStructureKHR wa = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    wa.accelerationStructureCount = 1;
    wa.pAccelerationStructures = &tlas_;
    VkDescriptorBufferInfo bufs[3] = {{tris_.buf, 0, VK_WHOLE_SIZE}, {srcs_.buf, 0, VK_WHOLE_SIZE}, {hist_.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[4] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].pNext = &wa;
    w[0].dstSet = set_;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (uint32_t i = 0; i < 3; ++i) {
        w[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i + 1].dstSet = set_;
        w[i + 1].dstBinding = i + 1;
        w[i + 1].descriptorCount = 1;
        w[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i + 1].pBufferInfo = &bufs[i];
    }
    vk_.UpdateDescriptorSets(device_, 4, w, 0, nullptr);

    // Short submissions: a long one can trip the driver's watchdog.
    constexpr uint32_t kBatch = 8192;
    const uint32_t total = static_cast<uint32_t>(h.rays);
    for (uint32_t base = 0; base < total; base += kBatch) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { h.cancelled = true; break; }
        PushBlock pb = {};
        pb.listener[0] = listener.x; pb.listener[1] = listener.y; pb.listener[2] = listener.z;
        pb.radiusSq = opt.radius * opt.radius;
        pb.perSource = per;
        pb.rayBase = base;
        pb.rayCount = (std::min)(kBatch, total - base);
        pb.bins = static_cast<uint32_t>(h.bins);
        pb.dt = opt.dt;
        pb.skipOrder = static_cast<uint32_t>((std::max)(0, opt.skipOrder));
        pb.includeDirect = opt.includeDirect ? 1u : 0u;
        pb.maxBounces = static_cast<uint32_t>((std::max)(1, opt.maxBounces));
        pb.seed = opt.seed;
        pb.tmax = opt.tmax;
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_.ResetCommandBuffer(cmd_, 0);
        vk_.BeginCommandBuffer(cmd_, &begin);
        vk_.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
        vk_.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
        vk_.CmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pb), &pb);
        vk_.CmdDispatch(cmd_, (pb.rayCount + 63u) / 64u, 1, 1);
        vk_.EndCommandBuffer(cmd_);
        if (!SubmitAndWait(cmd_)) { h.cancelled = true; break; }
    }
    if (!h.cancelled) {
        const float* hist = static_cast<const float*>(hist_.map);
        for (int b = 0; b < kBands; ++b)
            for (int i = 0; i < h.bins; ++i)
                h.sum[b][static_cast<size_t>(i)] = hist[static_cast<size_t>(b) * static_cast<size_t>(h.bins) + static_cast<size_t>(i)];
    }
    FreeScene();
    return h;
}

void VulkanTracer::Shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        FreeScene();
        if (fence_) vk_.DestroyFence(device_, fence_, nullptr);
        if (cmdPool_) vk_.DestroyCommandPool(device_, cmdPool_, nullptr);
        if (pool_) vk_.DestroyDescriptorPool(device_, pool_, nullptr);
        if (pipe_) vk_.DestroyPipeline(device_, pipe_, nullptr);
        if (pipeLayout_) vk_.DestroyPipelineLayout(device_, pipeLayout_, nullptr);
        if (setLayout_) vk_.DestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        vk_.DestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE && vk_.DestroyInstance) vk_.DestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
#if defined(_WIN32)
    if (lib_) FreeLibrary(lib_);
#else
    if (lib_) dlclose(lib_);
#endif
    lib_ = nullptr;
}

} // namespace

std::unique_ptr<TraceBackend> CreateVulkanTraceBackend(std::string& error, std::string* deviceName) {
    auto tracer = std::make_unique<VulkanTracer>();
    if (!tracer->Init(error, deviceName)) return nullptr;
    return tracer;
}

} // namespace stage
} // namespace svms

#else   // no Vulkan SDK at build time

namespace svms {
namespace stage {

std::unique_ptr<TraceBackend> CreateVulkanTraceBackend(std::string& error, std::string*) {
    error = "built without the Vulkan SDK";
    return nullptr;
}

} // namespace stage
} // namespace svms

#endif
