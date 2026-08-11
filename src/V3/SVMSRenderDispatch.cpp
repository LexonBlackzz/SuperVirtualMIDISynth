#include "SVMSRenderKernels.h"

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace svms {
namespace {

void CpuId(int leaf, int subleaf, int regs[4]) {
#if defined(_MSC_VER)
    __cpuidex(regs, leaf, subleaf);
#else
    unsigned int a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(static_cast<unsigned int>(leaf),
                  static_cast<unsigned int>(subleaf), a, b, c, d);
    regs[0] = static_cast<int>(a);
    regs[1] = static_cast<int>(b);
    regs[2] = static_cast<int>(c);
    regs[3] = static_cast<int>(d);
#endif
}

bool HasSSE2() {
#if defined(_M_X64) || defined(__x86_64__)
    return true;
#else
    int regs[4]{};
    CpuId(1, 0, regs);
    return (regs[3] & (1 << 26)) != 0;
#endif
}

#if !defined(SVMS_XP_COMPAT)
bool HasAVX2() {
    int regs[4]{};
    CpuId(0, 0, regs);
    if (regs[0] < 7) return false;
    CpuId(1, 0, regs);
    constexpr int kOsXsave = 1 << 27;
    constexpr int kAvx = 1 << 28;
    if ((regs[2] & (kOsXsave | kAvx)) != (kOsXsave | kAvx)) return false;
#if defined(_MSC_VER)
    const unsigned __int64 xcr0 = _xgetbv(0);
#else
    unsigned int eax = 0, edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    const unsigned long long xcr0 =
        (static_cast<unsigned long long>(edx) << 32) | eax;
#endif
    if ((xcr0 & 0x6u) != 0x6u) return false;
    CpuId(7, 0, regs);
    return (regs[1] & (1 << 5)) != 0;
}
#endif

} // namespace

bool IsRenderBackendSupported(RenderBackend backend) {
    switch (backend) {
        case RenderBackend::Scalar: return true;
        case RenderBackend::SSE2: return HasSSE2();
#if !defined(SVMS_XP_COMPAT)
        case RenderBackend::AVX2: return HasAVX2();
#endif
        default: return false;
    }
}

const RenderKernelSet* SelectRenderKernelSet(RenderBackend backend) {
    if (!IsRenderBackendSupported(backend)) return nullptr;
    switch (backend) {
        case RenderBackend::Scalar: return &GetScalarRenderKernelSet();
        case RenderBackend::SSE2: return &GetSSE2RenderKernelSet();
#if !defined(SVMS_XP_COMPAT)
        case RenderBackend::AVX2: return &GetAVX2RenderKernelSet();
#endif
        default: return nullptr;
    }
}

const RenderKernelSet& SelectBestRenderKernelSet() {
#if !defined(SVMS_XP_COMPAT)
    if (HasAVX2()) return GetAVX2RenderKernelSet();
#endif
    if (HasSSE2()) return GetSSE2RenderKernelSet();
    return GetScalarRenderKernelSet();
}

} // namespace svms
