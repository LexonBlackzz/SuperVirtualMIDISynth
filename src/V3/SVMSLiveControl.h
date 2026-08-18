#ifndef SVMS_LIVE_CONTROL_H
#define SVMS_LIVE_CONTROL_H

#include <atomic>
#include <cstdint>

namespace svms {

// Process-local bridge between RuntimeLink's control thread and the audio
// thread's VoiceManager.  This is deliberately NOT part of shared memory:
// RuntimeLink validates and publishes the requested cap here, then the audio
// thread observes it at render boundaries without locks or IPC traffic.
inline std::atomic<uint32_t> g_runtimeVoicePoolCapacity{0u};
inline std::atomic<uint32_t> g_runtimeRequestedVoiceLimit{0u};
inline std::atomic<uint32_t> g_runtimeAppliedVoiceLimit{0u};

inline void PublishRuntimeVoicePoolCapacity(uint32_t capacity) noexcept {
    g_runtimeVoicePoolCapacity.store(capacity, std::memory_order_release);
    if (capacity != 0u &&
        g_runtimeRequestedVoiceLimit.load(std::memory_order_relaxed) == 0u) {
        g_runtimeRequestedVoiceLimit.store(capacity, std::memory_order_release);
        g_runtimeAppliedVoiceLimit.store(capacity, std::memory_order_release);
    }
}

inline uint32_t RuntimeVoicePoolCapacity() noexcept {
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
