#pragma once

// C++17 portability for the canonical engine. The Linux build floor
// (Ubuntu 18.04, GCC 7) has no <span> and only the Filesystem TS, so the
// engine uses Span and Path instead of std::span / std::filesystem::path.

#include <cstddef>
#include <type_traits>
#include <vector>

#if __has_include(<filesystem>)
#include <filesystem>
#define SVMS_CANONICAL_FILESYSTEM_TS 0
#else
#include <experimental/filesystem>
#define SVMS_CANONICAL_FILESYSTEM_TS 1
#endif

namespace svms::canonical {

#if SVMS_CANONICAL_FILESYSTEM_TS
using Path = std::experimental::filesystem::path;
#else
using Path = std::filesystem::path;
#endif

// Non-owning contiguous view: the subset of std::span the engine uses.
template <class T>
class Span {
public:
    constexpr Span() noexcept = default;
    constexpr Span(T* data, std::size_t size) noexcept : data_(data), size_(size) {}
    template <class U, class A,
              class = std::enable_if_t<std::is_convertible<U (*)[], T (*)[]>::value>>
    Span(std::vector<U, A>& values) noexcept
        : data_(values.data()), size_(values.size()) {}
    template <class U, class A,
              class = std::enable_if_t<std::is_convertible<const U (*)[], T (*)[]>::value>>
    Span(const std::vector<U, A>& values) noexcept
        : data_(values.data()), size_(values.size()) {}
    template <class U,
              class = std::enable_if_t<std::is_convertible<U (*)[], T (*)[]>::value>>
    constexpr Span(const Span<U>& other) noexcept
        : data_(other.data()), size_(other.size()) {}

    constexpr T* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr T* begin() const noexcept { return data_; }
    constexpr T* end() const noexcept { return data_ + size_; }
    constexpr T& operator[](std::size_t index) const noexcept { return data_[index]; }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace svms::canonical
