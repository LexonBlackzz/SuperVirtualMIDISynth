#ifndef SVMS_LARGE_PAGES_H
#define SVMS_LARGE_PAGES_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>

namespace svms {

// Opt-in large-page backing (memory.large_pages, applied at engine init —
// live toggles take effect on the next restart). Backs the biggest
// allocations (voice SoA pool, dense-render shadow) with 2 MB pages to cut
// TLB pressure at 100k+ voice pool sizes. Every failure mode is a silent
// fallback to the regular aligned allocator; the privilege probe below can
// never prompt for elevation — AdjustTokenPrivileges only succeeds when the
// account already holds SeLockMemoryPrivilege.
inline std::atomic<bool> g_largePagesEnabled{false};

// One-shot probe: true only when the privilege is held and was enabled.
inline bool EnsureLockMemoryPrivilege() noexcept {
#if defined(SVMS_XP_COMPAT)
    return false;
#else
    static const bool cached = []() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
            return false;
        TOKEN_PRIVILEGES privileges{};
        if (!LookupPrivilegeValueW(nullptr, SE_LOCK_MEMORY_NAME,
                                   &privileges.Privileges[0].Luid)) {
            CloseHandle(token);
            return false;
        }
        privileges.PrivilegeCount = 1u;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        const BOOL adjusted =
            AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr,
                                  nullptr);
        const DWORD error = GetLastError();
        CloseHandle(token);
        // ERROR_NOT_ALL_ASSIGNED means the privilege is not held: fail
        // quietly instead of asking the user for anything.
        return adjusted != FALSE && error == ERROR_SUCCESS;
    }();
    return cached;
#endif
}

// Returns nullptr on any failure; callers fall back to _aligned_malloc.
inline void* TryAllocateLargePages(size_t bytes) noexcept {
#if defined(SVMS_XP_COMPAT)
    (void)bytes;
    return nullptr;
#else
    if (!g_largePagesEnabled.load(std::memory_order_relaxed)) return nullptr;
    if (!EnsureLockMemoryPrivilege()) return nullptr;
    const SIZE_T minimum = GetLargePageMinimum();
    if (minimum == 0u) return nullptr;
    size_t rounded = bytes + (static_cast<size_t>(minimum) - 1u);
    rounded &= ~static_cast<size_t>(minimum - 1u);
    if (rounded < bytes) return nullptr;  // overflow
    return VirtualAlloc(nullptr, rounded,
                        MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                        PAGE_READWRITE);
#endif
}

inline void FreeLargePages(void* block) noexcept {
#if !defined(SVMS_XP_COMPAT)
    if (block) VirtualFree(block, 0, MEM_RELEASE);
#else
    (void)block;
#endif
}

} // namespace svms

#endif // SVMS_LARGE_PAGES_H
