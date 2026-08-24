#ifndef SVMS_LIVE_CONTROL_H
#define SVMS_LIVE_CONTROL_H

#include <atomic>
#include <cstdint>

namespace svms {

// Process-local bridge between RuntimeLink's control thread and the audio
// thread's VoiceManager. This is deliberately NOT part of shared memory:
// RuntimeLink publishes a requested logical cap here, then the audio thread
// observes it at a render boundary. A request above the current allocation is
// handled by VoiceManager::GrowCapacity before the logical cap is applied.
inline constexpr uint32_t kRuntimeVoiceGrowthCeiling = 524288u;
inline std::atomic<uint32_t> g_runtimeVoicePoolCapacity{0u};
inline std::atomic<uint32_t> g_runtimeRequestedVoiceLimit{0u};
inline std::atomic<uint32_t> g_runtimeAppliedVoiceLimit{0u};
inline std::atomic<uint32_t> g_runtimeVoiceGrowthCeiling{
    kRuntimeVoiceGrowthCeiling};

inline void ConfigureRuntimeVoiceGrowthCeiling(uint32_t ceiling) noexcept {
    if (ceiling == 0u || ceiling > kRuntimeVoiceGrowthCeiling)
        ceiling = kRuntimeVoiceGrowthCeiling;
    g_runtimeVoiceGrowthCeiling.store(ceiling, std::memory_order_release);
}

inline uint32_t RuntimeVoiceGrowthCeiling() noexcept {
    return g_runtimeVoiceGrowthCeiling.load(std::memory_order_acquire);
}

inline void PublishRuntimeVoicePoolCapacity(uint32_t capacity) noexcept {
    g_runtimeVoicePoolCapacity.store(capacity, std::memory_order_release);
    if (capacity != 0u &&
        g_runtimeRequestedVoiceLimit.load(std::memory_order_relaxed) == 0u) {
        g_runtimeRequestedVoiceLimit.store(capacity, std::memory_order_release);
        g_runtimeAppliedVoiceLimit.store(capacity, std::memory_order_release);
    }
}

// RuntimeLink uses this as its command-validation ceiling. Once a VoiceManager
// exists, the pool is growable up to V3's hard polyphony ceiling; the current
// physical allocation is intentionally NOT the limit for a live request.
inline uint32_t RuntimeVoicePoolCapacity() noexcept {
    return g_runtimeVoicePoolCapacity.load(std::memory_order_acquire) == 0u
        ? 0u : RuntimeVoiceGrowthCeiling();
}

// Actual allocation currently owned by the running VoiceManager. Telemetry
// normally obtains this directly from VoiceManager::GetMaxVoices(); expose it
// here as well for process-local diagnostics that need the distinction.
inline uint32_t RuntimeAllocatedVoicePoolCapacity() noexcept {
    return g_runtimeVoicePoolCapacity.load(std::memory_order_acquire);
}

inline void RequestRuntimeVoiceLimit(uint32_t limit) noexcept {
    g_runtimeRequestedVoiceLimit.store(limit, std::memory_order_release);
}

inline uint32_t RequestedRuntimeVoiceLimit() noexcept {
    return g_runtimeRequestedVoiceLimit.load(std::memory_order_acquire);
}

inline void PublishAppliedRuntimeVoiceLimit(uint32_t limit) noexcept {
    g_runtimeAppliedVoiceLimit.store(limit, std::memory_order_release);
}

inline uint32_t AppliedRuntimeVoiceLimit() noexcept {
    return g_runtimeAppliedVoiceLimit.load(std::memory_order_acquire);
}

} // namespace svms

#endif // SVMS_LIVE_CONTROL_H
