// SVMSPlayerScan.h — local-synth discovery for the SVMS player.
//
// Classifies DLLs found in the player's own directory WITHOUT loading them:
// the PE export table is parsed statically, so "is this a synth?" never runs
// a foreign DllMain (a patching winmm shim executes its payload on attach —
// classification must stay side-effect free). Loading remains an explicit,
// user-confirmed action per module.
//
// Classification priority (first match wins):
//   SVMS_GetInterface             -> svms   (SVMS family answers this first)
//   KDMAPI export set             -> kdapi  (OmniMIDI-conventional facade)
//   midiOutOpen + midiOutShortMsg -> winmm  (any winmm-shaped module)
//   denylist stem match           -> runtime (never loaded, listed only)
//   anything else                 -> other
//
// Display names: version resource FileDescription, then ProductName, then the
// filename with underscores turned to spaces. Duplicate names get " #1",
// " #2", ... deterministically (entries sorted by filename).
//
// Probes (explicit only, one module at a time):
//   svms  -> SVMS_GetInterface(ABI 1) + capability negotiation (no session)
//   kdapi -> ReturnKDMAPIVer + IsKDMAPIAvailable + init/term (real start,
//            reversed immediately)
//   winmm -> midiOutGetNumDevs + open MIDI_MAPPER + devcaps + close

#pragma once

#include <windows.h>
#include "include/svmsapi.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace svmscan {

struct Entry {
    std::wstring path;
    std::wstring filename;
    std::wstring displayName; // resolved + dedup-suffixed
    std::string kind;         // "svms" | "kdapi" | "winmm" | "runtime" | "other"
    std::string arch;         // "x64" | "x86" | "arm64" | "?"
    uint64_t fileSize = 0;
    bool loadable = false;    // synth the player may load (user-confirmed)
};

// ── denylist ────────────────────────────────────────────────────────────
// Stems are matched lowercase: exact match OR prefix match (catches versioned
// runtime names like msvcp140.dll). Deliberately NOT containing "winmm" —
// a dropped-in winmm.dll is exactly what the user wants to test.

inline bool StemMatches(const std::wstring& stem, const wchar_t* pattern) {
    const size_t len = wcslen(pattern);
    if (stem.size() < len) return false;
    for (size_t i = 0; i < len; ++i) {
        wchar_t c = stem[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (c != pattern[i]) return false;
    }
    return true;
}

inline bool IsDenylisted(const std::wstring& filename) {
    std::wstring stem = filename;
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.resize(dot);
    static const wchar_t* const kPatterns[] = {
        // CRT / compiler runtimes
        L"msvcp", L"msvcr", L"vcruntime", L"ucrtbase", L"concrt", L"vcomp",
        L"msvcrt", L"atl", L"mfc", L"unicows",
        // UCRT / API sets
        L"api-ms-", L"ext-ms-",
        // BASS family (OmniMIDI's engine deps — not directly usable)
        L"bass",
        // system libraries
        L"ntdll", L"kernel32", L"kernelbase", L"user32", L"gdi32", L"shell32",
        L"advapi32", L"ole32", L"oleaut32", L"ws2_32", L"wsock32", L"comctl32",
        L"comdlg32", L"shlwapi", L"wintrust", L"crypt32",
        L"dbghelp", L"d3d9", L"d3d10", L"d3d11", L"d3d12", L"dxgi", L"dsound",
        L"dinput8", L"dinput", L"xinput", L"opengl32", L"glu32", L"ddraw",
        L"dwmapi", L"setupapi", L"cfgmgr32", L"powrprof", L"msimg32",
        L"imm32", L"rpcrt4", L"sechost", L"devobj", L"msdmo",
        L"devenum", L"quartz", L"avrt", L"avifil32", L"msvfw32",
        // common media codecs / helpers
        L"zlib", L"libpng", L"freetype", L"libmpg", L"libsndfile", L"ogg",
        L"vorbis", L"opus", L"flac", L"lame",
    };
    for (const wchar_t* p : kPatterns)
        if (StemMatches(stem, p)) return true;
    return false;
}

// ── static PE export sniffing ───────────────────────────────────────────

struct PeExports {
    bool ok = false;              // MZ + PE + export directory parsed
    uint16_t machine = 0;
    std::vector<std::string> names;
};

inline const char* ArchName(uint16_t machine) {
    switch (machine) {
        case 0x8664: return "x64";
        case 0x014c: return "x86";
        case 0xAA64: return "arm64";
        default:     return "?";
    }
}

inline PeExports ReadExports(const std::wstring& path) {
    PeExports out;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 256 * 1024 * 1024) {
        CloseHandle(file);
        return out;
    }
    std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read,
                  nullptr) || read != data.size()) {
        CloseHandle(file);
        return out;
    }
    CloseHandle(file);

    auto u16 = [&](size_t off) -> uint16_t {
        if (off + 2 > data.size()) return 0;
        return static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
    };
    auto u32 = [&](size_t off) -> uint32_t {
        if (off + 4 > data.size()) return 0;
        return static_cast<uint32_t>(data[off]) |
               (static_cast<uint32_t>(data[off + 1]) << 8) |
               (static_cast<uint32_t>(data[off + 2]) << 16) |
               (static_cast<uint32_t>(data[off + 3]) << 24);
    };

    if (data.size() < 64 || data[0] != 'M' || data[1] != 'Z') return out;
    const size_t peOffset = u32(0x3c);
    if (peOffset == 0 || peOffset + 24 > data.size() ||
        data[peOffset] != 'P' || data[peOffset + 1] != 'E') return out;

    out.machine = u16(peOffset + 4);
    const uint16_t sectionCount = u16(peOffset + 6);
    const uint16_t optSize = u16(peOffset + 20);
    const size_t optOff = peOffset + 24;
    if (optOff + optSize > data.size() || optSize < 96) return out;

    const uint16_t magic = u16(optOff);
    size_t dataDirOff;
    if (magic == 0x20b) dataDirOff = optOff + 112;       // PE32+
    else if (magic == 0x10b) dataDirOff = optOff + 96;   // PE32
    else return out;

    const uint32_t exportRva = u32(dataDirOff);
    const uint32_t exportSize = u32(dataDirOff + 4);
    if (exportRva == 0 || exportSize == 0) return out;   // no exports

    // section table for RVA -> file offset
    struct Section { uint32_t va, rawSize, rawPtr; };
    std::vector<Section> sections;
    const size_t sectOff = optOff + optSize;
    if (sectOff + static_cast<size_t>(sectionCount) * 40 > data.size())
        return out;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t s = sectOff + static_cast<size_t>(i) * 40;
        sections.push_back({u32(s + 12), u32(s + 16), u32(s + 20)});
    }
    auto rvaToOffset = [&](uint32_t rva) -> size_t {
        if (sections.empty() || rva < sections.front().va)
            return rva;  // PE headers region maps 1:1
        for (const Section& s : sections) {
            if (rva >= s.va && rva - s.va < s.rawSize)
                return s.rawPtr + (rva - s.va);
        }
        return static_cast<size_t>(-1);
    };

    const size_t dirOff = rvaToOffset(exportRva);
    if (dirOff == static_cast<size_t>(-1) || dirOff + 40 > data.size())
        return out;
    const uint32_t numberOfNames = u32(dirOff + 24);
    const uint32_t namesRva = u32(dirOff + 32);
    if (numberOfNames == 0 || numberOfNames > 100000) return out;
    const size_t namePtrs = rvaToOffset(namesRva);
    if (namePtrs == static_cast<size_t>(-1) ||
        namePtrs + static_cast<size_t>(numberOfNames) * 4 > data.size())
        return out;

    out.names.reserve(numberOfNames);
    for (uint32_t i = 0; i < numberOfNames; ++i) {
        const uint32_t nameRva = u32(namePtrs + static_cast<size_t>(i) * 4);
        if (nameRva == 0) continue;
        const size_t nameOff = rvaToOffset(nameRva);
        if (nameOff == static_cast<size_t>(-1) || nameOff >= data.size())
            continue;
        size_t end = nameOff;
        while (end < data.size() && data[end] != 0) ++end;
        out.names.emplace_back(
            reinterpret_cast<const char*>(data.data()) + nameOff,
            end - nameOff);
    }
    out.ok = true;
    return out;
}

inline bool HasExport(const std::vector<std::string>& names,
                      const char* symbol) {
    for (const std::string& n : names)
        if (n == symbol) return true;
    return false;
}

inline std::string ClassifyKind(const PeExports& pe) {
    if (!pe.ok) return "other";
    if (HasExport(pe.names, "SVMS_GetInterface")) return "svms";
    if (HasExport(pe.names, "IsKDMAPIAvailable") &&
        (HasExport(pe.names, "InitializeKDMAPIStream") ||
         HasExport(pe.names, "SendDirectData"))) return "kdapi";
    if (HasExport(pe.names, "midiOutOpen") &&
        HasExport(pe.names, "midiOutShortMsg")) return "winmm";
    return "other";
}

// ── display names ───────────────────────────────────────────────────────

inline std::wstring VersionResourceString(const std::wstring& path,
                                          const wchar_t* key) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct LangCp { WORD lang, cp; };
    LangCp* trans = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&trans), &len) ||
        len < sizeof(LangCp)) return {};
    wchar_t sub[96];
    swprintf(sub, 96, L"\\StringFileInfo\\%04x%04x\\%ls",
             trans[0].lang, trans[0].cp, key);
    wchar_t* value = nullptr;
    UINT vlen = 0;
    if (!VerQueryValueW(data.data(), sub,
                        reinterpret_cast<void**>(&value), &vlen) ||
        !value || vlen < 2) return {};
    return value;
}

inline std::wstring FallbackNameFromFilename(const std::wstring& filename) {
    std::wstring stem = filename;
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.resize(dot);
    std::replace(stem.begin(), stem.end(), L'_', L' ');
    return stem;
}

inline std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

inline std::wstring ResolveDisplayName(const std::wstring& path,
                                       const std::wstring& filename) {
    std::wstring name = Trim(VersionResourceString(path, L"FileDescription"));
    if (name.empty())
        name = Trim(VersionResourceString(path, L"ProductName"));
    if (name.empty()) name = FallbackNameFromFilename(filename);
    return name;
}

// Deterministic dedup: entries must already be sorted by filename. When a
// display name occurs more than once, every occurrence gets " #k".
inline void AssignDisplayNames(std::vector<Entry>& entries) {
    std::map<std::wstring, int> counts;
    for (const Entry& e : entries) ++counts[e.displayName];
    std::map<std::wstring, int> seen;
    for (Entry& e : entries) {
        if (counts[e.displayName] > 1) {
            wchar_t suffix[16];
            swprintf(suffix, 16, L" #%d", ++seen[e.displayName]);
            e.displayName += suffix;
        }
    }
}

// ── directory scan (no loading) ─────────────────────────────────────────

inline std::vector<Entry> ScanDirectory(const std::wstring& dir) {
    std::vector<Entry> entries;
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
        pattern += L'\\';
    pattern += L"*.dll";

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return entries;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Entry e;
        e.filename = fd.cFileName;
        e.path = dir;
        if (!e.path.empty() && e.path.back() != L'\\' && e.path.back() != L'/')
            e.path += L'\\';
        e.path += e.filename;
        e.fileSize = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                     fd.nFileSizeLow;
        e.displayName = ResolveDisplayName(e.path, e.filename);

        if (IsDenylisted(e.filename)) {
            e.kind = "runtime";
        } else {
            PeExports pe = ReadExports(e.path);
            e.kind = ClassifyKind(pe);
            e.arch = ArchName(pe.machine);
            e.loadable = (e.kind == "svms" || e.kind == "kdapi" ||
                          e.kind == "winmm");
        }
        entries.push_back(e);
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return _wcsicmp(a.filename.c_str(), b.filename.c_str()) < 0;
              });
    AssignDisplayNames(entries);
    return entries;
}

// ── probes (explicit, one module at a time) ─────────────────────────────

struct ProbeResult {
    bool ok = false;
    std::string detail;
};

inline std::string LastErrorText() {
    char buffer[64];
    sprintf(buffer, "GetLastError=%lu",
            static_cast<unsigned long>(GetLastError()));
    return buffer;
}

// svms: bootstrap + capability negotiation only — no session is created, so
// no audio engine, no mixer threads, fully side-effect free.
inline ProbeResult ProbeSvms(const std::wstring& path) {
    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod) return {false, "LoadLibrary failed (" + LastErrorText() + ")"};
    ProbeResult result;
    using GetInterfaceFn = SVMS_Result (SVMS_CALL*)(uint32_t, uint32_t,
                                                    SVMS_Interface*);
    auto get = reinterpret_cast<GetInterfaceFn>(
        GetProcAddress(mod, "SVMS_GetInterface"));
    if (!get) {
        FreeLibrary(mod);
        return {false, "no SVMS_GetInterface export after load"};
    }
    SVMS_Interface ifc{};
    ifc.struct_size = sizeof(ifc);
    const SVMS_Result r = get(SVMS_ABI_VERSION_1, sizeof(ifc), &ifc);
    if (r != SVMS_RESULT_OK) {
        char buffer[96];
        sprintf(buffer, "SVMS_GetInterface refused (result=%d)",
                static_cast<int>(r));
        FreeLibrary(mod);
        return {false, buffer};
    }
    char buffer[192];
    sprintf(buffer,
            "ABI %u struct v%u (runtime %u bytes) product %u.%u.%u b%u "
            "caps=0x%016llx",
            static_cast<unsigned>(ifc.abi_version),
            static_cast<unsigned>(ifc.struct_version),
            static_cast<unsigned>(ifc.struct_size),
            static_cast<unsigned>(ifc.product_major),
            static_cast<unsigned>(ifc.product_minor),
            static_cast<unsigned>(ifc.product_patch),
            static_cast<unsigned>(ifc.build_number),
            static_cast<unsigned long long>(ifc.capabilities));
    result.detail = buffer;
    static const struct { uint64_t bit; const char* name; } kCaps[] = {
        {SVMS_CAP_EXACT_QPC_TIMESTAMPS, "qpc"},
        {SVMS_CAP_SHORT_EVENT_BATCH, "batch"},
        {SVMS_CAP_SYSTEM_EXCLUSIVE, "sysex"},
        {SVMS_CAP_TELEMETRY_V1, "telemetry"},
        {SVMS_CAP_KDMAPI_FACADE, "kdapi-facade"},
        {SVMS_CAP_EXACT_MONOTONIC_NS, "mono-ns"},
        {SVMS_CAP_EXACT_OUTPUT_FRAMES, "out-frames"},
        {SVMS_CAP_QUEUE_CONTROL, "queue"},
        {SVMS_CAP_SOUNDFONT_RELOAD, "sf2-reload"},
        {SVMS_CAP_MIXED_TIMESTAMP_BATCH, "mixed-ts"},
        {SVMS_CAP_ISOLATED_OFFLINE_SESSIONS, "offline"},
        {SVMS_CAP_CONFIG_JSON, "config-json"},
        {SVMS_CAP_CANCELLABLE_SUBMISSION, "cancel"},
        {SVMS_CAP_ISOLATED_REALTIME_SESSIONS, "rt-sessions"},
    };
    for (const auto& c : kCaps)
        if (ifc.capabilities & c.bit) {
            result.detail += " ";
            result.detail += c.name;
        }
    result.ok = true;
    FreeLibrary(mod);
    return result;
}

// kdapi: a real synth start — InitializeKDMAPIStream spins up the audio
// engine. Reversed immediately with TerminateKDMAPIStream.
inline ProbeResult ProbeKdapi(const std::wstring& path) {
    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod) return {false, "LoadLibrary failed (" + LastErrorText() + ")"};
    ProbeResult result;
    auto available = reinterpret_cast<BOOL (WINAPI*)(void)>(
        GetProcAddress(mod, "IsKDMAPIAvailable"));
    auto version = reinterpret_cast<UINT (WINAPI*)(DWORD*, DWORD*, DWORD*,
                                                   DWORD*)>(
        GetProcAddress(mod, "ReturnKDMAPIVer"));
    auto init = reinterpret_cast<LPVOID (WINAPI*)(void)>(
        GetProcAddress(mod, "InitializeKDMAPIStream"));
    auto term = reinterpret_cast<void (WINAPI*)(void)>(
        GetProcAddress(mod, "TerminateKDMAPIStream"));
    if (!available || !init || !term) {
        FreeLibrary(mod);
        return {false, "missing KDMAPI exports after load"};
    }
    char buffer[128];
    if (version) {
        DWORD major = 0, minor = 0, build = 0, rev = 0;
        version(&major, &minor, &build, &rev);
        sprintf(buffer, "ver %lu.%lu.%lu.%lu available=%d",
                static_cast<unsigned long>(major),
                static_cast<unsigned long>(minor),
                static_cast<unsigned long>(build),
                static_cast<unsigned long>(rev),
                static_cast<int>(available()));
        result.detail = buffer;
    } else {
        result.detail = "no ReturnKDMAPIVer; available=" +
                        std::to_string(available() ? 1 : 0);
    }
    if (!available()) {
        FreeLibrary(mod);
        return {false, result.detail + " (reports unavailable)"};
    }
    LPVOID stream = init();
    result.ok = (stream != nullptr);
    if (stream) term();
    result.detail += stream ? " init/term ok"
                            : " InitializeKDMAPIStream failed";
    FreeLibrary(mod);
    return result;
}

// winmm: open the mapper, read device caps, close. Silent — no notes sent.
inline ProbeResult ProbeWinmm(const std::wstring& path) {
    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod) return {false, "LoadLibrary failed (" + LastErrorText() + ")"};
    ProbeResult result;
    auto numDevs = reinterpret_cast<UINT (WINAPI*)(void)>(
        GetProcAddress(mod, "midiOutGetNumDevs"));
    auto open = reinterpret_cast<MMRESULT (WINAPI*)(HMIDIOUT*, UINT, DWORD_PTR,
                                                    DWORD_PTR, DWORD)>(
        GetProcAddress(mod, "midiOutOpen"));
    auto caps = reinterpret_cast<MMRESULT (WINAPI*)(HMIDIOUT, LPMIDIOUTCAPSA,
                                                    UINT)>(
        GetProcAddress(mod, "midiOutGetDevCapsA"));
    auto close = reinterpret_cast<MMRESULT (WINAPI*)(HMIDIOUT)>(
        GetProcAddress(mod, "midiOutClose"));
    if (!numDevs || !open || !caps || !close) {
        FreeLibrary(mod);
        return {false, "missing midiOut exports after load"};
    }
    result.detail = "devices=" + std::to_string(numDevs());
    HMIDIOUT handle = nullptr;
    MMRESULT r = open(&handle, MIDI_MAPPER, 0, 0, 0);
    if (r != MMSYSERR_NOERROR && numDevs() > 0)
        r = open(&handle, 0, 0, 0, 0);  // fall back to device 0
    if (r == MMSYSERR_NOERROR) {
        MIDIOUTCAPSA mc{};
        if (caps(handle, &mc, sizeof(mc)) == MMSYSERR_NOERROR)
            result.detail += "  \"" + std::string(mc.szPname) + "\"";
        close(handle);
        result.ok = true;
    } else {
        result.detail += "  open failed (mmr=" + std::to_string(r) + ")";
    }
    FreeLibrary(mod);
    return result;
}

// Probe by static classification. Refuses to load anything the scanner did
// not classify as a synth — the bass.dll/msvcp.dll case never reaches here.
inline ProbeResult Probe(const Entry& entry) {
    if (entry.kind == "svms") return ProbeSvms(entry.path);
    if (entry.kind == "kdapi") return ProbeKdapi(entry.path);
    if (entry.kind == "winmm") return ProbeWinmm(entry.path);
    return {false, "refusing to load: classified as \"" + entry.kind + "\""};
}

} // namespace svmscan
