// Live audio output restarts, driven through SVMSAPI.dll with the device-free
// test output (SVMS_TEST_FAKE_AUDIO=1) and an isolated configuration.
//   svms_v3_audio_restart_tests <path to SVMSAPI.dll>
#include <windows.h>

#include "include/svmsapi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                         #cond);                                                     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

// ---- a one-sample looping SoundFont --------------------------------------

void Put16(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>& b, uint32_t v) {
    Put16(b, v & 0xFFFFu);
    Put16(b, v >> 16);
}
void PutName(std::vector<uint8_t>& b, const char* name) {
    char n[20] = {};
    std::strncpy(n, name, 19);
    b.insert(b.end(), n, n + 20);
}
std::vector<uint8_t> Chunk(const char* id, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> c(id, id + 4);
    Put32(c, static_cast<uint32_t>(body.size()));
    c.insert(c.end(), body.begin(), body.end());
    if (body.size() & 1u) c.push_back(0);
    return c;
}
std::vector<uint8_t> List(const char* type, const std::vector<std::vector<uint8_t>>& chunks) {
    std::vector<uint8_t> body(type, type + 4);
    for (const auto& c : chunks) body.insert(body.end(), c.begin(), c.end());
    return Chunk("LIST", body);
}

bool WriteTestSoundFont(const std::string& path) {
    const uint32_t frames = 2000;
    std::vector<uint8_t> smpl;
    for (uint32_t i = 0; i < frames + 46; ++i) {
        const double s = i < frames ? std::sin(i * 2.0 * 3.14159265358979 / 100.0) : 0.0;
        Put16(smpl, static_cast<uint16_t>(static_cast<int16_t>(s * 12000.0)));
    }
    std::vector<uint8_t> ifil, inam, phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    Put16(ifil, 2); Put16(ifil, 1);
    inam = {'t', 0};
    PutName(phdr, "p"); Put16(phdr, 0); Put16(phdr, 0); Put16(phdr, 0);
    Put32(phdr, 0); Put32(phdr, 0); Put32(phdr, 0);
    PutName(phdr, "EOP"); Put16(phdr, 0); Put16(phdr, 0); Put16(phdr, 1);
    Put32(phdr, 0); Put32(phdr, 0); Put32(phdr, 0);
    Put16(pbag, 0); Put16(pbag, 0); Put16(pbag, 1); Put16(pbag, 0);
    pmod.assign(10, 0);
    Put16(pgen, 41); Put16(pgen, 0);   // instrument 0
    Put16(pgen, 0); Put16(pgen, 0);
    PutName(inst, "i"); Put16(inst, 0);
    PutName(inst, "EOI"); Put16(inst, 1);
    Put16(ibag, 0); Put16(ibag, 0); Put16(ibag, 2); Put16(ibag, 0);
    imod.assign(10, 0);
    Put16(igen, 54); Put16(igen, 1);   // loop continuously
    Put16(igen, 53); Put16(igen, 0);   // sample 0
    Put16(igen, 0); Put16(igen, 0);
    PutName(shdr, "s"); Put32(shdr, 0); Put32(shdr, frames); Put32(shdr, 100);
    Put32(shdr, 1900); Put32(shdr, 44100); shdr.push_back(60); shdr.push_back(0);
    Put16(shdr, 0); Put16(shdr, 1);
    PutName(shdr, "EOS"); Put32(shdr, 0); Put32(shdr, 0); Put32(shdr, 0); Put32(shdr, 0);
    Put32(shdr, 0); shdr.push_back(0); shdr.push_back(0); Put16(shdr, 0); Put16(shdr, 0);

    std::vector<uint8_t> body{'s', 'f', 'b', 'k'};
    for (const auto& part :
         {List("INFO", {Chunk("ifil", ifil), Chunk("INAM", inam)}),
          List("sdta", {Chunk("smpl", smpl)}),
          List("pdta", {Chunk("phdr", phdr), Chunk("pbag", pbag), Chunk("pmod", pmod),
                        Chunk("pgen", pgen), Chunk("inst", inst), Chunk("ibag", ibag),
                        Chunk("imod", imod), Chunk("igen", igen), Chunk("shdr", shdr)})})
        body.insert(body.end(), part.begin(), part.end());
    const std::vector<uint8_t> riff = Chunk("RIFF", body);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(riff.data(), 1, riff.size(), f) == riff.size();
    std::fclose(f);
    return ok;
}

// ---- session helpers -----------------------------------------------------

using GetInterfaceProc = SVMS_Result(SVMS_CALL*)(uint32_t, uint32_t, SVMS_Interface*);

SVMS_Interface g_api{};
SVMS_Session g_session = 0;

bool WaitFor(bool (*cond)(), int ms) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
    while (GetTickCount64() < end) {
        if (cond()) return true;
        Sleep(5);
    }
    return cond();
}

SVMS_TelemetryV1 Telemetry() {
    SVMS_TelemetryV1 t{};
    t.struct_size = sizeof(t);
    t.struct_version = SVMS_STRUCT_VERSION_1;
    g_api.get_telemetry(g_session, &t);
    return t;
}

bool CallbacksAdvance() {
    const uint64_t before = Telemetry().callback_count;
    const ULONGLONG end = GetTickCount64() + 2000;
    while (GetTickCount64() < end) {
        if (Telemetry().callback_count > before + 3) return true;
        Sleep(5);
    }
    return false;
}

void PlayNotes(int pairs) {
    for (int i = 0; i < pairs; ++i) {
        const uint32_t key = 36u + static_cast<uint32_t>(i % 48);
        g_api.send_short(g_session, 0x90u | (key << 8) | (100u << 16));
        g_api.send_short(g_session, 0x80u | (key << 8));
    }
}

bool VoicesQuiet() { return Telemetry().active_voices == 0u; }
bool VoicesSounding() { return Telemetry().active_voices > 0u; }
bool SoundFontLoaded() { return Telemetry().soundfont_loaded != 0u; }

SVMS_Result Command(uint32_t id, const std::string& in, std::string* out = nullptr) {
    char text[512] = {};
    strncpy_s(text, in.c_str(), _TRUNCATE);
    const SVMS_Result r = g_api.send_runtime_command(g_session, id, 0u, text, sizeof(text));
    if (out) *out = text;
    return r;
}

std::string QueryValue(const std::string& key) {
    std::string text;
    Command(SVMS_COMMAND_QUERY_AUDIO_OUTPUT, "", &text);
    const std::string all = "\n" + text;
    const std::string k = "\n" + key + "=";
    size_t p = all.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    const size_t end = all.find('\n', p);
    return all.substr(p, end == std::string::npos ? std::string::npos : end - p);
}

uint32_t QueryNumber(const std::string& key) {
    return static_cast<uint32_t>(std::strtoul(QueryValue(key).c_str(), nullptr, 10));
}

bool Settled() { return QueryValue("restarting") == "0"; }

bool SetOutput(const std::string& lines) {
    return Command(SVMS_COMMAND_SET_AUDIO_OUTPUT, lines) == SVMS_RESULT_OK &&
           WaitFor(&Settled, 10000);
}

// ---- tests ---------------------------------------------------------------

void FakeOutputPlays() {
    CHECK(Telemetry().audio_running == 1u);
    CHECK(CallbacksAdvance());
    for (uint32_t key = 40; key < 48; ++key)
        g_api.send_short(g_session, 0x90u | (key << 8) | (100u << 16));
    CHECK(WaitFor(&VoicesSounding, 2000));
    for (uint32_t key = 40; key < 48; ++key) g_api.send_short(g_session, 0x80u | (key << 8));
    PlayNotes(64);
    CHECK(WaitFor(&VoicesQuiet, 3000));
}

void RestartGrowsCapacity() {
    const uint32_t sizes[] = {32u, 2048u, 8192u, 64u};
    for (uint32_t frames : sizes) {
        CHECK(SetOutput("frames=" + std::to_string(frames)));
        CHECK(QueryNumber("frames") == frames);
        CHECK(Telemetry().buffer_frames == frames);
        CHECK(QueryValue("running") == "1");
        CHECK(CallbacksAdvance());
        if (frames == 8192u) CHECK(QueryNumber("capacity") >= 8192u);
    }
    PlayNotes(64);
    CHECK(WaitFor(&VoicesQuiet, 3000));
}

void RestartStorm() {
    const uint32_t sizes[] = {8u, 32u, 256u, 2048u, 441u};
    for (int i = 0; i < 50; ++i) {
        const std::string lines = "frames=" + std::to_string(sizes[i % 5]) +
                                  "\nrate=" + (i % 2 ? "48000" : "44100");
        CHECK(Command(SVMS_COMMAND_SET_AUDIO_OUTPUT, lines) == SVMS_RESULT_OK);
        PlayNotes(40);
        Sleep(10);
    }
    CHECK(WaitFor(&Settled, 15000));
    CHECK(QueryNumber("frames") == 441u);
    CHECK(QueryNumber("rate") == 48000u);
    CHECK(QueryNumber("capacity") >= 2048u);
    CHECK(CallbacksAdvance());
    CHECK(WaitFor(&VoicesQuiet, 3000));
    CHECK(SetOutput("frames=8192"));
    CHECK(QueryNumber("capacity") >= 8192u);
    CHECK(SetOutput("frames=441\nrate=44100"));
    CHECK(CallbacksAdvance());
}

void SetAudioOutputRejectsUnknown() {
    std::string out;
    CHECK(Command(SVMS_COMMAND_SET_AUDIO_OUTPUT, "bogus=1", &out) != SVMS_RESULT_OK);
    CHECK(out.find("bogus") != std::string::npos);
    CHECK(Command(SVMS_COMMAND_SET_AUDIO_OUTPUT, "frames=0", &out) != SVMS_RESULT_OK);
    CHECK(Command(SVMS_COMMAND_SET_AUDIO_OUTPUT, "backend=dsound", &out) != SVMS_RESULT_OK);
    CHECK(QueryValue("restarting") == "0");
}

void QueryAfterFallback() {
    CHECK(SetOutput("backend=asio\ndevice=NoSuchDriver"));
    CHECK(QueryValue("requested") == "asio");
    CHECK(QueryValue("active") == "wasapi-shared");
    CHECK(QueryValue("fallback") == "1");
    CHECK(!QueryValue("error").empty());
    CHECK(CallbacksAdvance());
    CHECK(SetOutput("backend=wasapi-shared\ndevice="));
    CHECK(QueryValue("fallback") == "0");
    CHECK(CallbacksAdvance());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::puts("usage: svms_v3_audio_restart_tests <SVMSAPI.dll>");
        return 1;
    }
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string dir = std::string(temp) + "svms_audio_restart_test\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    const std::string config = dir + "config.json";
    const std::string sf2 = dir + "restart_test.sf2";
    DeleteFileA(config.c_str());
    SetEnvironmentVariableA("SVMS_TEST_LOCAL_CONFIG_PATH", config.c_str());
    SetEnvironmentVariableA("SVMS_TEST_CONFIG_PATH", config.c_str());
    SetEnvironmentVariableA("SVMS_TEST_FAKE_AUDIO", "1");
    if (!WriteTestSoundFont(sf2)) {
        std::puts("FAIL: could not write the test SoundFont");
        return 1;
    }

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        std::puts("FAIL: could not load SVMSAPI.dll");
        return 1;
    }
    auto getInterface =
        reinterpret_cast<GetInterfaceProc>(GetProcAddress(dll, "SVMS_GetInterface"));
    if (!getInterface ||
        getInterface(SVMS_ABI_VERSION_1, sizeof(g_api), &g_api) != SVMS_RESULT_OK) {
        std::puts("FAIL: no SVMS interface");
        return 1;
    }
    SVMS_SessionConfig cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.struct_version = SVMS_STRUCT_VERSION_1;
    if (g_api.create_session(&cfg, &g_session) != SVMS_RESULT_OK) {
        std::puts("FAIL: the session did not start on the test output");
        return 1;
    }
    if (g_api.load_soundfont_utf8(g_session, sf2.c_str()) != SVMS_RESULT_OK ||
        !WaitFor(&SoundFontLoaded, 5000)) {
        std::puts("FAIL: the test SoundFont did not load");
        return 1;
    }

    FakeOutputPlays();
    RestartGrowsCapacity();
    RestartStorm();
    SetAudioOutputRejectsUnknown();
    QueryAfterFallback();

    g_api.destroy_session(g_session);
    FreeLibrary(dll);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("audio restart tests passed");
    return 0;
}
