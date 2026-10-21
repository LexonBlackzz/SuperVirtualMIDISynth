// OfflineStackSmoke.cpp - SVMS_CAP_OFFLINE_SOUNDFONT_STACK through the real
// SVMSAPI.dll. Builds two one-sample SFZ instruments that differ only in
// loudness (so a wrong sample offset or a wrong bank reads as the wrong level)
// and checks stack order, SFZ gating, per-channel pins, live replacement and
// that a failed replacement keeps the old stack.
#include <windows.h>

#include "include/svmsapi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

using GetInterfaceProc = SVMS_Result (SVMS_CALL*)(uint32_t, uint32_t,
                                                  SVMS_Interface*);

int g_failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

void WriteU32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); }
void WriteU16(std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); }

// 1 s of a 441 Hz sine, mono 16-bit 44.1 kHz.
void WriteWav(const std::string& path, double amplitude) {
    const uint32_t frames = 44100u;
    std::ofstream f(path, std::ios::binary);
    f.write("RIFF", 4); WriteU32(f, 36u + frames * 2u); f.write("WAVE", 4);
    f.write("fmt ", 4); WriteU32(f, 16u); WriteU16(f, 1u); WriteU16(f, 1u);
    WriteU32(f, 44100u); WriteU32(f, 88200u); WriteU16(f, 2u); WriteU16(f, 16u);
    f.write("data", 4); WriteU32(f, frames * 2u);
    for (uint32_t i = 0; i < frames; ++i) {
        const double s = amplitude * std::sin(2.0 * 3.14159265358979 * 441.0 * i / 44100.0);
        WriteU16(f, static_cast<uint16_t>(static_cast<int16_t>(s * 32767.0)));
    }
}

void WriteSfz(const std::string& path, const std::string& wav) {
    std::ofstream f(path);
    f << "<region> sample=" << wav << " pitch_keycenter=69 lokey=0 hikey=127\n";
}

// Plays A4 on `channel` for a few blocks, returns the left channel's peak.
float PeakOf(const SVMS_Interface& api, SVMS_Session session, uint32_t channel,
             bool program5 = false) {
    api.reset(session);
    float peak = 0.0f;
    float left[512], right[512];
    for (uint32_t block = 0; block < 6u; ++block) {
        SVMS_OfflineEvent events[2]{};
        uint32_t count = 0u;
        if (block == 0u) {
            if (program5)
                events[count++] = {0u, 0xC0u | channel | (5u << 8u), {0u, 0u}};
            events[count++] = {0u, 0x90u | channel | (69u << 8u) | (100u << 16u), {0u, 0u}};
        }
        if (api.render_offline(session, events, count, left, right, 512u) !=
            SVMS_RESULT_OK)
            return -1.0f;
        if (block >= 2u)
            for (float v : left) peak = (std::max)(peak, std::fabs(v));
    }
    return peak;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("FAIL: expected <SVMSAPI.dll path>"); return 1; }
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) { std::puts("FAIL: could not load the runtime"); return 1; }
    const auto getInterface = reinterpret_cast<GetInterfaceProc>(
        GetProcAddress(module, "SVMS_GetInterface"));
    SVMS_Interface api{};
    if (!getInterface || getInterface(SVMS_ABI_VERSION_1, sizeof(api), &api) !=
                             SVMS_RESULT_OK) {
        std::puts("FAIL: no interface");
        return 1;
    }
    if (!(api.capabilities & SVMS_CAP_OFFLINE_SOUNDFONT_STACK) ||
        !api.set_offline_soundfonts || !api.set_offline_channel_soundfont) {
        std::puts("FAIL: stack capability or functions missing");
        return 1;
    }

    char tempDir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tempDir);
    const std::string dir = std::string(tempDir) + "svms_stack_smoke_" +
                            std::to_string(GetCurrentProcessId()) + "\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    WriteWav(dir + "quiet.wav", 0.2);
    WriteWav(dir + "loud.wav", 0.6);
    WriteSfz(dir + "quiet.sfz", "quiet.wav");
    WriteSfz(dir + "loud.sfz", "loud.wav");
    const std::string quiet = dir + "quiet.sfz", loud = dir + "loud.sfz";

    SVMS_OfflineSessionConfig config{};
    config.struct_size = sizeof(config);
    config.struct_version = SVMS_STRUCT_VERSION_1;
    config.session_kind = SVMS_SESSION_OFFLINE_RENDER;
    config.sample_rate = 44100u;
    config.max_voices = 64u;
    config.render_threads = 1u;
    config.max_block_frames = 512u;
    config.render_backend = SVMS_RENDER_BACKEND_SCALAR;
    config.limiter_enabled = 0u;
    config.limiter_algorithm = SVMS_LIMITER_CLASSIC;
    config.master_volume = 1.0f;
    config.limiter_threshold = 0.95f;
    config.limiter_lookahead_ms = 3.0f;
    config.limiter_attack_ms = 0.5f;
    config.limiter_release_ms = 100.0f;
    SVMS_Session session = 0;
    const SVMS_Result created =
        api.create_offline_session(&config, quiet.c_str(), &session);
    if (created != SVMS_RESULT_OK) {
        std::printf("FAIL: could not create the session (%u)\n",
                    static_cast<unsigned>(created));
        return 1;
    }

    const float quietPeak = PeakOf(api, session, 0u);
    Check(quietPeak > 0.01f, "single SFZ renders");

    const char* stack[2] = {quiet.c_str(), loud.c_str()};
    Check(api.set_offline_soundfonts(session, stack, 2u) == SVMS_RESULT_OK,
          "install a two-entry stack");
    Check(std::fabs(PeakOf(api, session, 0u) - quietPeak) < quietPeak * 0.01f,
          "first entry answers program 0");

    Check(api.set_offline_channel_soundfont(session, 0u, 2u) == SVMS_RESULT_OK,
          "pin channel 0 to entry 2");
    const float loudPeak = PeakOf(api, session, 0u);
    Check(loudPeak > quietPeak * 2.0f && loudPeak < quietPeak * 4.0f,
          "pinned channel plays the second entry (right sample offset)");
    Check(std::fabs(PeakOf(api, session, 1u) - quietPeak) < quietPeak * 0.01f,
          "unpinned channel is unchanged");

    Check(api.set_offline_channel_soundfont(session, 0u, 3u) ==
              SVMS_RESULT_INVALID_ARGUMENT &&
          api.set_offline_channel_soundfont(session, 16u, 1u) ==
              SVMS_RESULT_INVALID_ARGUMENT,
          "out-of-range pins are rejected");

    Check(std::fabs(PeakOf(api, session, 1u, true) - quietPeak) < quietPeak * 0.01f,
          "program 5: SFZs do not claim it, first SFZ is the last resort");

    const char* bad[2] = {quiet.c_str(), (dir + "missing.sfz").c_str()};
    Check(api.set_offline_soundfonts(session, bad, 2u) != SVMS_RESULT_OK,
          "a stack with a missing file is refused");
    Check(std::fabs(PeakOf(api, session, 0u) - loudPeak) < loudPeak * 0.01f,
          "a refused stack leaves the old one (and the pin) in place");

    const char* only[1] = {loud.c_str()};
    Check(api.set_offline_soundfonts(session, only, 1u) == SVMS_RESULT_OK,
          "replace with a single entry");
    Check(std::fabs(PeakOf(api, session, 0u) - loudPeak) < loudPeak * 0.01f,
          "replaced stack plays");
    Check(api.set_offline_soundfonts(session, nullptr, 0u) ==
              SVMS_RESULT_INVALID_ARGUMENT,
          "an empty stack is rejected");

    api.destroy_session(session);
    FreeLibrary(module);
    for (const char* n : {"quiet.wav", "loud.wav", "quiet.sfz", "loud.sfz"})
        DeleteFileA((dir + n).c_str());
    RemoveDirectoryA(dir.c_str());
    if (g_failures == 0) std::puts("SVMS offline SoundFont stack smoke passed");
    return g_failures == 0 ? 0 : 1;
}
