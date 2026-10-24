#include "render/GpuStage.h"

#include <cstdlib>

// Everything the renderer calls on GpuStage is inline in the header, so only
// the code that creates a stage (driver, bench, tests) links this file and the
// backends.

namespace svms {
namespace gpu {
std::unique_ptr<Backend> CreateOpenGLBackend();

std::unique_ptr<Backend> CreateBackend(Api api) {
    switch (api) {
        case Api::Auto:     // M1: OpenGL is the only backend
        case Api::OpenGL:
            return CreateOpenGLBackend();
        default:
            return nullptr;
    }
}
} // namespace gpu

std::unique_ptr<GpuStage> GpuStage::Create(gpu::Api api, uint32_t deviceIndex,
                                           std::string& error) {
    std::unique_ptr<gpu::Backend> backend = gpu::CreateBackend(api);
    if (!backend) {
        error = "the requested GPU API is not available in this build";
        return nullptr;
    }
    if (!backend->Init(deviceIndex, error)) return nullptr;
    std::unique_ptr<GpuStage> stage(new GpuStage());
    stage->backend_ = std::move(backend);
    // Debug/bench override: SVMS_GPU_SPLIT_MIN=<rows> (huge = never split).
    if (const char* parts = std::getenv("SVMS_GPU_PARTS"))
        stage->SetParts(static_cast<uint32_t>(std::strtoul(parts, nullptr, 10)));
    if (const char* split = std::getenv("SVMS_GPU_SPLIT_MIN"))
        stage->SetSplitMinRows(static_cast<uint32_t>(std::strtoul(split, nullptr, 10)));
    return stage;
}

} // namespace svms
