#ifndef SVMS_THREAD_AFFINITY_H
#define SVMS_THREAD_AFFINITY_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

#include "SVMSTypes.h"

namespace svms {

// Mask of logical processors sitting on efficiency-class cores (P-cores on
// hybrid CPUs such as Raptor Lake).  Returns 0 when the system reports a
// single efficiency class (pinning would be a no-op) or on pre-Win7 systems.
// Single-processor-group systems only: SetThreadAffinityMask is group-blind.
inline ULONG_PTR PerformanceCoreMask() {
#if defined(SVMS_XP_COMPAT)
    return 0;
#else
    using InfoExProc = BOOL(WINAPI*)(LOGICAL_PROCESSOR_RELATIONSHIP,
                                     PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                     PDWORD);
    const InfoExProc infoEx = reinterpret_cast<InfoExProc>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetLogicalProcessorInformationEx"));
    if (!infoEx) return 0;

    DWORD size = 0;
    if (infoEx(RelationProcessorCore, nullptr, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return 0;
    }
    std::vector<uint8_t> buffer(size);
    auto* const info = reinterpret_cast<
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!infoEx(RelationProcessorCore, info, &size)) return 0;

    ULONG_PTR mask = 0;
    for (DWORD offset = 0; offset < size;) {
        auto* const entry = reinterpret_cast<
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore &&
            entry->Processor.EfficiencyClass > 0) {
            mask |= entry->Processor.GroupMask[0].Mask;
        }
        offset += entry->Size;
    }
    return mask;
#endif
}

// Pins a render thread to performance cores.  Threads left unpinned may be
// scheduled onto E-cores, which cost ~2x on the AVX2 kernels and jitter the
// worker join path.  No-op on single-class CPUs and XP.
inline void PinThreadToPerformanceCores(HANDLE thread) {
    if (!thread) return;
    const ULONG_PTR mask = PerformanceCoreMask();
    if (mask != 0) SetThreadAffinityMask(thread, mask);
}

} // namespace svms

#endif // SVMS_THREAD_AFFINITY_H
