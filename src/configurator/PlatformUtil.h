#ifndef SVMS_CONFIGURATOR_PLATFORMUTIL_H
#define SVMS_CONFIGURATOR_PLATFORMUTIL_H

// UTF-8 <-> wide string conversion and the std::filesystem spelling shared by
// the configurator on Windows and Linux. The config model keeps std::wstring
// paths (UTF-16 on Windows, UTF-32 on Linux); these are the only conversions.

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <codecvt>
#include <cwctype>
#include <locale>
// The page classes keep Windows handle types in their signatures; on Linux
// they are opaque and unused.
using HWND = void*;
using HANDLE = void*;

inline int _wcsicmp(const wchar_t* a, const wchar_t* b) {
    for (;; ++a, ++b) {
        const wint_t x = std::towlower(static_cast<wint_t>(*a));
        const wint_t y = std::towlower(static_cast<wint_t>(*b));
        if (x != y) return x < y ? -1 : 1;
        if (*a == L'\0') return 0;
    }
}
#endif

// GCC < 9 (the Ubuntu 18.04 build floor) only ships the Filesystem TS.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 9
#include <experimental/filesystem>
namespace svms::cfg { namespace fs = std::experimental::filesystem; }
#else
#include <filesystem>
namespace svms::cfg { namespace fs = std::filesystem; }
#endif

namespace svms::cfg::util {

inline std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
#ifdef _WIN32
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
#else
    try {
        return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(value);
    } catch (...) {
        return {};
    }
#endif
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
#ifdef _WIN32
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          value.data(), static_cast<int>(value.size()),
                                          nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
#else
    try {
        return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(value);
    } catch (...) {
        return {};
    }
#endif
}

// path::lexically_normal() is missing from the Filesystem TS (GCC 7).
inline fs::path LexicallyNormal(const fs::path& path) {
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 9
    fs::path normal;
    for (const fs::path& part : path) {
        if (part == ".") continue;
        if (part == ".." && !normal.empty() && normal.filename() != "..") {
            normal = normal.parent_path();
            continue;
        }
        normal /= part;
    }
    return normal;
#else
    return path.lexically_normal();
#endif
}

// Atomically replaces `to` with `from` (MoveFileEx write-through on Windows).
inline bool ReplaceFile(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
#endif
}

} // namespace svms::cfg::util

namespace svms::cfg::util {

// Opens a URL or file/folder with the desktop's default handler
// (ShellExecute on Windows, xdg-open elsewhere). Fire and forget.
bool OpenExternal(const std::string& urlOrPath);

#ifndef _WIN32
// File chooser through whichever of kdialog/zenity is installed. `patterns`
// is a space separated glob list ("*.sf2 *.sfz"). Returns false when the
// user cancelled or no chooser exists (callers then keep the typed path).
bool PickFile(bool save, const std::string& title, const std::string& startDir,
              const std::string& patterns, std::string& path);
bool HaveFilePicker();
#endif

} // namespace svms::cfg::util

#endif
