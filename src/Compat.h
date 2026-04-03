#ifndef COMPAT_H
#define COMPAT_H

#include <windows.h>

// Windows 7 compatibility: GetSystemTimePreciseAsFileTime is only available on Windows 8+
// We provide a fallback that uses GetSystemTimeAsFileTime on older systems
#ifndef GetSystemTimePreciseAsFileTime
#define SVMS_NEED_GETSYSTEMTIMEPRECISE 1
#else
#define SVMS_NEED_GETSYSTEMTIMEPRECISE 0
#endif

#if SVMS_NEED_GETSYSTEMTIMEPRECISE
// Function pointer type for GetSystemTimePreciseAsFileTime
typedef void (WINAPI *PFN_GetSystemTimePreciseAsFileTime)(LPFILETIME);

// Runtime-initialized function pointer (will be NULL on Windows 7)
extern PFN_GetSystemTimePreciseAsFileTime g_pfnGetSystemTimePreciseAsFileTime;

// Initialize the function pointer (call once at startup)
inline void CompatInitializeTimeFunctions() {
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        g_pfnGetSystemTimePreciseAsFileTime = 
            (PFN_GetSystemTimePreciseAsFileTime)GetProcAddress(hKernel32, "GetSystemTimePreciseAsFileTime");
    }
}

// Wrapper that uses precise time on Windows 8+, falls back to regular time on Windows 7
inline void CompatGetSystemTimePreciseAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
    if (g_pfnGetSystemTimePreciseAsFileTime != NULL) {
        g_pfnGetSystemTimePreciseAsFileTime(lpSystemTimeAsFileTime);
    } else {
        GetSystemTimeAsFileTime(lpSystemTimeAsFileTime);
    }
}
#else
inline void CompatInitializeTimeFunctions() {}
inline void CompatGetSystemTimePreciseAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
    GetSystemTimePreciseAsFileTime(lpSystemTimeAsFileTime);
}
#endif

#ifdef SVMS_LEGACY_XP
namespace compat {

class Mutex {
public:
  Mutex() { InitializeCriticalSection(&cs_); }
  ~Mutex() { DeleteCriticalSection(&cs_); }

  void lock() { EnterCriticalSection(&cs_); }
  void unlock() { LeaveCriticalSection(&cs_); }
  bool try_lock() { return TryEnterCriticalSection(&cs_) != 0; }

private:
  CRITICAL_SECTION cs_;
};

template <typename T> class LockGuard {
public:
  explicit LockGuard(T &mutex) : mutex_(mutex) { mutex_.lock(); }
  ~LockGuard() { mutex_.unlock(); }

private:
  T &mutex_;
};

inline DWORD64 GetTickCount64Compat() {
  static DWORD lastLow = 0;
  static DWORD high = 0;
  static Mutex mutex;
  LockGuard<Mutex> lock(mutex);

  DWORD current = GetTickCount();
  if (current < lastLow) {
    ++high;
  }
  lastLow = current;
  return (static_cast<DWORD64>(high) << 32) | current;
}

} // namespace compat

#else
#include <mutex>

namespace compat {

using Mutex = std::mutex;

template <typename T> using LockGuard = std::lock_guard<T>;

inline ULONGLONG GetTickCount64Compat() { return GetTickCount64(); }

} // namespace compat

#endif

#endif
