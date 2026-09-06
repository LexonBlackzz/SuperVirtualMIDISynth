#ifndef SVMS_THREAD_AFFINITY_H
#define SVMS_THREAD_AFFINITY_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <vector>

#include "SVMSTypes.h"

namespace svms {

// Thread-affinity policy (opt-in; synth.thread_affinity_mode, live command
// SetThreadAffinityMode). Everything is a no-op on single-efficiency-class
// CPUs and on XP:
//   0 = off (default): threads keep default scheduler placement.
//   1 = every render thread (audio output, event compiler, workers) pinned
//       to performance cores; unpinned threads can land on E-cores, which
//       cost ~2x on the AVX2 kernels and jitter the worker join path.
//   2 = audio output + event compiler pinned to performance cores, render
//       workers pinned to efficiency cores so the scheduler keeps the
//       P-cores free for the RT threads. The worker count is never
//       reduced — pinning only narrows where existing threads run, and a
//       missing E-core set leaves workers unpinned.
inline std::atomic<uint32_t> g_threadAffinityMode{0u};

enum class AffinityRole : uint32_t { Realtime, Compiler, Worker };

struct CoreClassMasks {
    ULONG_PTR performance = 0;
    ULONG_PTR efficiency = 0;  // 0 unless the CPU reports mixed classes
};

// Single-processor-group systems only: SetThreadAffinityMask is group-blind.
// Multi-group systems return empty masks (feature disabled) rather than a
// wrong pin.
inline CoreClassMasks DetectCoreClassMasks() {
#if defined(SVMS_XP_COMPAT)
    return {};
#else
    using InfoExProc = BOOL(WINAPI*)(LOGICAL_PROCESSOR_RELATIONSHIP,
                                     PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                     PDWORD);
    const InfoExProc infoEx = reinterpret_cast<InfoExProc>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetLogicalProcessorInformationEx"));
    if (!infoEx) return {};

    DWORD size = 0;
    if (infoEx(RelationProcessorCore, nullptr, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }
    std::vector<uint8_t> buffer(size);
    auto* const info = reinterpret_cast<
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!infoEx(RelationProcessorCore, info, &size)) return {};

    CoreClassMasks masks;
    bool hybrid = false;
    ULONG_PTR seenGroupsMask = 0;
    for (DWORD offset = 0; offset < size;) {
        auto* const entry = reinterpret_cast<
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) {
            const GROUP_AFFINITY& group = entry->Processor.GroupMask[0];
            if (entry->Processor.GroupCount != 1 || group.Group != 0)
                return {};  // multi-group topology: opt out entirely
            seenGroupsMask |= group.Mask;
            if (entry->Processor.EfficiencyClass > 0) {
                masks.performance |= group.Mask;
                hybrid = true;
            } else {
                masks.efficiency |= group.Mask;
            }
        }
        offset += entry->Size;
    }
    if (!hybrid) masks.efficiency = 0;
    return masks;
#endif
}

// Mask of logical processors on efficiency-class cores (P-cores on hybrid
// CPUs such as Raptor Lake).  Returns 0 when the system reports a single
// efficiency class (pinning would be a no-op) or on pre-Win7 systems.
inline ULONG_PTR PerformanceCoreMask() {
    return DetectCoreClassMasks().performance;
}

inline ULONG_PTR EfficiencyCoreMask() {
    return DetectCoreClassMasks().efficiency;
}

// Pins a render thread to performance cores regardless of the configured
// mode.  Prefer ApplyThreadAffinity except for the bench, which pins
// explicitly via --pinned-core-style controls.
inline void PinThreadToPerformanceCores(HANDLE thread) {
    if (!thread) return;
    const ULONG_PTR mask = PerformanceCoreMask();
    if (mask != 0) SetThreadAffinityMask(thread, mask);
}

// Returns a thread to the process-default mask (un-pin).
inline void ResetThreadAffinity(HANDLE thread) {
    if (!thread) return;
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask,
                               &systemMask))
        SetThreadAffinityMask(thread, systemMask);
}

// Applies the current affinity policy to a thread. Used both at thread
// creation and for live re-apply; the reset branches leave a fresh thread
// at its inherited (process-default) mask, so creation-time calls are safe
// under every mode.
inline void ApplyThreadAffinity(HANDLE thread, AffinityRole role) {
    if (!thread) return;
    const uint32_t mode = g_threadAffinityMode.load(std::memory_order_relaxed);
    if (role == AffinityRole::Worker) {
        if (mode >= 2u) {
            const ULONG_PTR mask = EfficiencyCoreMask();
            if (mask != 0u) SetThreadAffinityMask(thread, mask);
        } else if (mode == 1u) {
            PinThreadToPerformanceCores(thread);
        } else {
            ResetThreadAffinity(thread);
        }
        return;
    }
    if (mode >= 1u) {
        PinThreadToPerformanceCores(thread);
    } else {
        ResetThreadAffinity(thread);
    }
}

} // namespace svms

#endif // SVMS_THREAD_AFFINITY_H
