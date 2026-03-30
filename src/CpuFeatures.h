#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

// CPU Feature Detection for SIMD dispatch
// Uses CPUID on x86/x64 to detect SSE2, AVX, AVX2 support

#ifdef _WIN32
#include <intrin.h>
#endif

// Singleton pattern to ensure one-time initialization
class CpuFeatures {
public:
  static bool HasSSE2() { return GetInstance().hasSSE2; }
  static bool HasAVX() { return GetInstance().hasAVX; }
  static bool HasAVX2() { return GetInstance().hasAVX2; }

private:
  bool hasSSE2;
  bool hasAVX;
  bool hasAVX2;

  CpuFeatures() : hasSSE2(false), hasAVX(false), hasAVX2(false) { Detect(); }

  static CpuFeatures &GetInstance() {
    static CpuFeatures instance;
    return instance;
  }

  void Detect() {
#ifdef _WIN32
    int cpuInfo[4] = {0};

    __cpuid(cpuInfo, 0);
    int nIds = cpuInfo[0];

    if (nIds >= 1) {
      __cpuid(cpuInfo, 1);
      hasSSE2 = (cpuInfo[3] & (1 << 26)) != 0;

      // Check for AVX: need OSXSAVE and AVX bits
      bool osUsesXSAVE = (cpuInfo[2] & (1 << 27)) != 0;
      bool cpuAVXSupport = (cpuInfo[2] & (1 << 28)) != 0;

      if (osUsesXSAVE && cpuAVXSupport) {
        // Check if OS has enabled AVX registers (XCR0)
        unsigned long long xcrFeatureMask = _xgetbv(0);
        hasAVX = (xcrFeatureMask & 0x6) == 0x6; // XMM and YMM state
      }
    }

    if (nIds >= 7 && hasAVX) {
      __cpuidex(cpuInfo, 7, 0);
      hasAVX2 = (cpuInfo[1] & (1 << 5)) != 0;
    }
#else
    // Non-Windows: assume SSE2 available on x64
    hasSSE2 = true;
#endif
  }
};

#endif // CPU_FEATURES_H
