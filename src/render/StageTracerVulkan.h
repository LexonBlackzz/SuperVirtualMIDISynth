#pragma once

// GPU backend of the stage tracer: the same algorithm as CpuTracer, with the
// scene walked by Vulkan ray queries. Opens the Vulkan loader at run time, so
// nothing is imported at load time; returns null when the loader, a ray-query
// capable device or the embedded shader is missing (the caller keeps the CPU
// tracer).

#include "render/StageTracer.h"

#include <memory>
#include <string>

namespace svms {
namespace stage {

// `error` receives the reason when the result is null. `deviceName`, when not
// null, receives the adapter that was picked.
std::unique_ptr<TraceBackend> CreateVulkanTraceBackend(std::string& error,
                                                       std::string* deviceName = nullptr);

} // namespace stage
} // namespace svms
