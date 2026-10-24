#include "render/GpuStage.h"

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
    return stage;
}

} // namespace svms
