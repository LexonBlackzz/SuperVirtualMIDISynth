// BassEncShim.cpp — the bassenc.dll build.
//
// Real bassenc.dll imports an undocumented data table ("_") from the real
// bass.dll, so it cannot sit on top of the SVMS bass.dll shim. This module
// replaces it with the part players use for conversions: BASS_Encode_Start
// on a decode stream, driven by the channel DSP chain (the SVMS bass.dll
// calls DSPs from BASS_ChannelGetData). Supported:
//   - BASS_ENCODE_PCM: the stream's samples are written as a WAV file
//     (float32 for float streams, 16-bit for BASS_ENCODE_FP_16BIT or 16-bit
//     streams), the way KMC exports WAV;
//   - a command line: the encoder process is started and fed a streaming WAV
//     on its standard input (KMC's OGG/MP3 route through kmcogg/kmcmp3);
//   - an ENCODEPROC callback with PCM (no file, no command line).
// BASS_ENCODE_AUTOFREE, PAUSE, and Stop/StopEx are honoured. Everything else
// in the bassenc surface (casting, ACM, servers) is not implemented.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr DWORD kEncNoHead = 0x1u;
constexpr DWORD kEncFp8 = 0x2u, kEncFp16 = 0x4u, kEncFp24 = 0x6u, kEncFp32 = 0x8u;
constexpr DWORD kEncPcm = 0x40u;
constexpr DWORD kEncAutoFree = 0x40000u;
constexpr DWORD kEncUnicode = 0x80000000u;

typedef void(WINAPI* DSPPROC)(DWORD handle, DWORD channel, void* buffer, DWORD length, void* user);
typedef void(WINAPI* ENCODEPROC)(DWORD handle, DWORD channel, const void* buffer, DWORD length, void* user);
typedef DWORD(WINAPI* SetDSP_t)(DWORD, DSPPROC, void*, int);
typedef BOOL(WINAPI* RemoveDSP_t)(DWORD, DWORD);
typedef BOOL(WINAPI* GetInfo_t)(DWORD, void*);

struct Encoder {
    DWORD handle = 0u;       // HENCODE
    DWORD channel = 0u;
    DWORD dsp = 0u;
    HANDLE file = INVALID_HANDLE_VALUE;      // WAV file (PCM mode)
    HANDLE process = nullptr;                // encoder process (command mode)
    HANDLE pipe = INVALID_HANDLE_VALUE;      // encoder stdin
    ENCODEPROC proc = nullptr;
    void* user = nullptr;
    DWORD flags = 0u;
    DWORD rate = 44100u, chans = 2u, bits = 32u;
    bool floatIn = true, floatOut = true;
    bool paused = false;
    bool active = true;
    bool wavHeader = false;                  // header written, patch sizes on close
    uint64_t dataBytes = 0u, patchedBytes = 0u;
    std::vector<uint8_t> scratch;
};

std::mutex g_mutex;
std::vector<std::unique_ptr<Encoder>> g_encoders;
int g_error = 0;

HMODULE BassModule() {
    static HMODULE module = GetModuleHandleW(L"bass.dll");
    return module;
}

void PutLe32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void PutLe16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }

std::vector<uint8_t> WavHeader(const Encoder& e, uint32_t dataBytes, uint32_t riffBytes) {
    std::vector<uint8_t> h(44, 0);
    std::memcpy(h.data(), "RIFF", 4); PutLe32(&h[4], riffBytes);
    std::memcpy(&h[8], "WAVEfmt ", 8); PutLe32(&h[16], 16);
    PutLe16(&h[20], e.floatOut ? 3 : 1);
    PutLe16(&h[22], static_cast<uint16_t>(e.chans));
    PutLe32(&h[24], e.rate);
    const uint32_t blockAlign = e.chans * (e.bits / 8u);
    PutLe32(&h[28], e.rate * blockAlign);
    PutLe16(&h[32], static_cast<uint16_t>(blockAlign));
    PutLe16(&h[34], static_cast<uint16_t>(e.bits));
    std::memcpy(&h[36], "data", 4); PutLe32(&h[40], dataBytes);
    return h;
}

bool WriteAll(HANDLE h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n != 0u) {
        DWORD chunk = static_cast<DWORD>(n > (1u << 20) ? (1u << 20) : n), done = 0u;
        if (!WriteFile(h, p, chunk, &done, nullptr) || done == 0u) return false;
        p += done; n -= done;
    }
    return true;
}

// Keeps the WAV sizes current while writing, so a file left open when the host dies or exits without
// BASS_Encode_Stop still imports (Audacity refuses a header that says 0 data bytes).
void PatchWavSizes(Encoder& e) {
    const uint32_t data = static_cast<uint32_t>(e.dataBytes > 0xFFFFFFF0ull ? 0xFFFFFFF0ull : e.dataBytes);
    LARGE_INTEGER zero{}, at{}, pos{};
    if (!SetFilePointerEx(e.file, zero, &at, FILE_CURRENT)) return;
    uint8_t riff[4], len[4];
    PutLe32(riff, 36u + data); PutLe32(len, data);
    pos.QuadPart = 4;  SetFilePointerEx(e.file, pos, nullptr, FILE_BEGIN); WriteAll(e.file, riff, 4);
    pos.QuadPart = 40; SetFilePointerEx(e.file, pos, nullptr, FILE_BEGIN); WriteAll(e.file, len, 4);
    SetFilePointerEx(e.file, at, nullptr, FILE_BEGIN);
    e.patchedBytes = e.dataBytes;
}

// Converts one block to the encoder's output format and delivers it.
void Deliver(Encoder& e, const void* buffer, DWORD length) {
    if (!e.active || e.paused || length == 0u) return;
    const void* out = buffer;
    DWORD outLen = length;
    if (e.floatIn && !e.floatOut) {
        const float* in = static_cast<const float*>(buffer);
        const size_t n = length / 4u;
        e.scratch.resize(n * (e.bits / 8u));
        for (size_t i = 0; i < n; ++i) {
            float v = in[i] * (e.bits == 16u ? 32767.0f : 127.0f);
            if (e.bits == 16u) {
                v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
                const int16_t s = static_cast<int16_t>(v);
                std::memcpy(&e.scratch[i * 2u], &s, 2u);
            } else {
                v = v > 127.0f ? 127.0f : (v < -128.0f ? -128.0f : v);
                e.scratch[i] = static_cast<uint8_t>(static_cast<int>(v) + 128);
            }
        }
        out = e.scratch.data();
        outLen = static_cast<DWORD>(e.scratch.size());
    }
    if (e.proc) e.proc(e.handle, e.channel, out, outLen, e.user);
    HANDLE sink = e.file != INVALID_HANDLE_VALUE ? e.file : e.pipe;
    if (sink != INVALID_HANDLE_VALUE) {
        if (!WriteAll(sink, out, outLen)) e.active = false;
        e.dataBytes += outLen;
        if (e.wavHeader && e.file != INVALID_HANDLE_VALUE && e.dataBytes - e.patchedBytes >= (1u << 20)) PatchWavSizes(e);
    }
}

void WINAPI EncodeDsp(DWORD handle, DWORD channel, void* buffer, DWORD length, void* user) {
    (void)handle; (void)channel;
    Encoder* e = static_cast<Encoder*>(user);
    if (e) Deliver(*e, buffer, length);
}

void Finish(Encoder& e) {
    if (!e.active && e.dsp == 0u && e.file == INVALID_HANDLE_VALUE && e.pipe == INVALID_HANDLE_VALUE) return;
    if (e.dsp != 0u && BassModule()) {
        if (auto remove = reinterpret_cast<RemoveDSP_t>(GetProcAddress(BassModule(), "BASS_ChannelRemoveDSP")))
            remove(e.channel, e.dsp);
        e.dsp = 0u;
    }
    if (e.file != INVALID_HANDLE_VALUE) {
        if (e.wavHeader) {
            const uint32_t data = static_cast<uint32_t>(e.dataBytes > 0xFFFFFFF0ull ? 0xFFFFFFF0ull : e.dataBytes);
            const std::vector<uint8_t> h = WavHeader(e, data, 36u + data);
            LARGE_INTEGER zero{};
            SetFilePointerEx(e.file, zero, nullptr, FILE_BEGIN);
            WriteAll(e.file, h.data(), h.size());
        }
        CloseHandle(e.file);
        e.file = INVALID_HANDLE_VALUE;
    }
    if (e.pipe != INVALID_HANDLE_VALUE) { CloseHandle(e.pipe); e.pipe = INVALID_HANDLE_VALUE; }
    if (e.process) {
        WaitForSingleObject(e.process, 60000);  // let the encoder flush its output
        CloseHandle(e.process);
        e.process = nullptr;
    }
    e.active = false;
}

Encoder* Resolve(DWORD handle) {
    if (handle == 0u || handle > g_encoders.size()) return nullptr;
    return g_encoders[handle - 1u].get();
}

}  // namespace

extern "C" {

DWORD WINAPI BASS_Encode_GetVersion(void) { return 0x02040a00u; }

DWORD WINAPI BASS_Encode_Start(DWORD handle, const void* cmdline, DWORD flags, ENCODEPROC proc, void* user) {
    std::lock_guard<std::mutex> lock(g_mutex);
    HMODULE bass = BassModule();
    if (!bass) { g_error = 8; return 0u; }  // BASS_ERROR_INIT
    auto getInfo = reinterpret_cast<GetInfo_t>(GetProcAddress(bass, "BASS_ChannelGetInfo"));
    auto setDsp = reinterpret_cast<SetDSP_t>(GetProcAddress(bass, "BASS_ChannelSetDSP"));
    struct { DWORD freq, chans, flags, ctype; } info{};
    if (!getInfo || !setDsp || !getInfo(handle, &info)) { g_error = 5; return 0u; }  // BASS_ERROR_HANDLE
    auto e = std::make_unique<Encoder>();
    e->channel = handle; e->flags = flags; e->proc = proc; e->user = user;
    e->rate = info.freq; e->chans = info.chans;
    e->floatIn = (info.flags & 0x100u) != 0u;
    e->floatOut = e->floatIn;
    e->bits = e->floatIn ? 32u : 16u;
    const DWORD fp = flags & 0xEu;
    if (fp == kEncFp16) { e->floatOut = false; e->bits = 16u; }
    else if (fp == kEncFp8) { e->floatOut = false; e->bits = 8u; }
    else if (fp == kEncFp32) { e->floatOut = true; e->bits = 32u; }
    else if (fp == kEncFp24) { e->floatOut = false; e->bits = 16u; }  // 24-bit PCM not produced: 16-bit
    if (!e->floatIn && e->bits != 16u) { e->floatOut = false; e->bits = 16u; }

    auto widen = [&](const void* text) {
        std::wstring w;
        const char* a = static_cast<const char*>(text);
        if ((flags & kEncUnicode) != 0u || (a[0] != 0 && a[1] == 0)) return std::wstring(static_cast<const wchar_t*>(text));
        const int n = MultiByteToWideChar(CP_ACP, 0, a, -1, nullptr, 0);
        w.assign(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_ACP, 0, a, -1, w.data(), n);
        return w;
    };

    if (cmdline && (flags & kEncPcm) != 0u) {
        const std::wstring path = widen(cmdline);
        e->file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (e->file == INVALID_HANDLE_VALUE) { g_error = 2; return 0u; }  // BASS_ERROR_FILEOPEN
        if ((flags & kEncNoHead) == 0u) {
            const std::vector<uint8_t> h = WavHeader(*e, 0u, 36u);
            if (!WriteAll(e->file, h.data(), h.size())) { CloseHandle(e->file); g_error = 2; return 0u; }
            e->wavHeader = true;
        }
    } else if (cmdline) {
        // Encoder process: it reads a streaming WAV on stdin.
        std::wstring command = widen(cmdline);
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 1u << 20)) { g_error = 2; return 0u; }
        SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE; si.hStdInput = readEnd;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        const BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                       nullptr, nullptr, &si, &pi);
        CloseHandle(readEnd);
        if (!ok) { CloseHandle(writeEnd); g_error = 2; return 0u; }
        CloseHandle(pi.hThread);
        e->process = pi.hProcess; e->pipe = writeEnd;
        if ((flags & kEncNoHead) == 0u) {
            const std::vector<uint8_t> h = WavHeader(*e, 0xFFFFFFFFu, 0xFFFFFFFFu);
            WriteAll(e->pipe, h.data(), h.size());
        }
    } else if (!proc) {
        g_error = 20; return 0u;  // BASS_ERROR_ILLPARAM: nowhere to send the audio
    }
    Encoder* raw = e.get();
    g_encoders.push_back(std::move(e));
    raw->handle = static_cast<DWORD>(g_encoders.size());
    raw->dsp = setDsp(handle, EncodeDsp, raw, -1000);  // after other DSP, like bassenc
    if (raw->dsp == 0u) { raw->active = false; g_error = 5; return 0u; }
    g_error = 0;
    return raw->handle;
}

BOOL WINAPI BASS_Encode_Stop(DWORD handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Like bassenc, a channel handle stops every encoder on it.
    bool any = false;
    for (auto& e : g_encoders) {
        if (e && (e->handle == handle || e->channel == handle) && e->active) { Finish(*e); any = true; }
    }
    g_error = any ? 0 : 5;
    return any ? TRUE : FALSE;
}

BOOL WINAPI BASS_Encode_StopEx(DWORD handle, BOOL queue) {
    (void)queue;
    return BASS_Encode_Stop(handle);
}

DWORD WINAPI BASS_Encode_IsActive(DWORD handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Encoder* e = Resolve(handle);
    if (e) return e->active ? (e->paused ? 2u : 1u) : 0u;
    for (auto& x : g_encoders) if (x && x->channel == handle && x->active) return x->paused ? 2u : 1u;
    return 0u;
}

BOOL WINAPI BASS_Encode_SetPaused(DWORD handle, BOOL paused) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Encoder* e = Resolve(handle);
    if (!e) { g_error = 5; return FALSE; }
    e->paused = paused != 0;
    return TRUE;
}

BOOL WINAPI BASS_Encode_Write(DWORD handle, const void* buffer, DWORD length) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Encoder* e = Resolve(handle);
    if (!e) { g_error = 5; return FALSE; }
    Deliver(*e, buffer, length);
    return TRUE;
}

DWORD WINAPI BASS_Encode_GetChannel(DWORD handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Encoder* e = Resolve(handle);
    return e ? e->channel : 0u;
}

DWORD WINAPI BASS_Encode_GetCount(DWORD handle, DWORD count) {
    (void)handle; (void)count;
    return 0u;
}

BOOL WINAPI BASS_Encode_SetNotify(DWORD handle, void* proc, void* user) {
    (void)handle; (void)proc; (void)user;
    return TRUE;
}

}  // extern "C"
