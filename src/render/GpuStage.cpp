#include "render/GpuStage.h"

#include <cstdlib>

// Everything the renderer calls on GpuStage is inline in the header, so only
// the code that creates a stage (driver, bench, tests) links this file and the
// backends.

namespace svms {
namespace gpu {
std::unique_ptr<Backend> CreateOpenGLBackend();
std::unique_ptr<Backend> CreateCudaBackend();

std::unique_ptr<Backend> CreateBackend(Api api) {
    switch (api) {
        case Api::OpenGL:
            return CreateOpenGLBackend();
        case Api::CUDA:
            return CreateCudaBackend();
        default:
            return nullptr;
    }
}
} // namespace gpu

std::unique_ptr<GpuStage> GpuStage::Create(gpu::Api api, uint32_t deviceIndex,
                                           std::string& error) {
    // Auto: CUDA when an NVIDIA GPU and driver are there, else OpenGL.
    const gpu::Api order[2] = {
        api == gpu::Api::Auto ? gpu::Api::CUDA : api,
        api == gpu::Api::Auto ? gpu::Api::OpenGL : gpu::Api::Off};
    std::unique_ptr<gpu::Backend> backend;
    for (const gpu::Api candidate : order) {
        if (candidate == gpu::Api::Off) break;
        std::unique_ptr<gpu::Backend> attempt = gpu::CreateBackend(candidate);
        if (!attempt) {
            error = "the requested GPU API is not available in this build";
            continue;
        }
        std::string attemptError;
        if (attempt->Init(deviceIndex, attemptError)) {
            backend = std::move(attempt);
            break;
        }
        error = attemptError;
    }
    if (!backend) return nullptr;
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
