#ifndef SVMS_PLATFORM_H
#define SVMS_PLATFORM_H

#include <cstddef>
#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>
#else
inline void* _aligned_malloc(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0u) bytes = 1u;
    void* memory = nullptr;
    return posix_memalign(&memory, alignment, bytes) == 0 ? memory : nullptr;
}

inline void _aligned_free(void* memory) {
    std::free(memory);
}

inline void OutputDebugStringA(const char*) {}
#endif

#endif
