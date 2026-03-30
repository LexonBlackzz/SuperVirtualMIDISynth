#ifndef COMPAT_H
#define COMPAT_H

#include <windows.h>

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
