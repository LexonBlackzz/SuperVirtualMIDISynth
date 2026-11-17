#include "configurator/PageOutput.h"
#include "configurator/ConfigDocument.h"
#include "configurator/EasterEggs.h"
#include "configurator/DriverLink.h"
#include "configurator/ModernWidgets.h"
#include "configurator/PlatformUtil.h"
#include "configurator/Widgets.h"
#include "configurator/Anim.h"
#include "imgui.h"
#ifdef _WIN32
#include "configurator/WasapiDevices.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include "driver/AsioSizes.h"
#if defined(SVMS_WITH_ASIO)
#include "driver/AsioHost.h"
#endif
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace svms::cfg {
namespace {

std::string W2U(const std::wstring& ws) { return util::WideToUtf8(ws); }
std::wstring U2W(const std::string& s) { return util::Utf8ToWide(s); }

bool EqualCI(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

void NoteText(const char* text, float wrapW = 0.0f) {
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
    ImGui::PushTextWrapPos(wrapW > 0.0f ? ImGui::GetCursorPosX() + wrapW : 0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::PopFont();
}

// Small muted caption above a segment (the prototype's .lbl).
void Label(const char* text, float topGap) {
    if (topGap > 0.0f) ImGui::Dummy(ImVec2(0.0f, topGap * ui::Scale() - ImGui::GetStyle().ItemSpacing.y));
    ui::PushText(0, 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 8.0f * ui::Scale() - ImGui::GetStyle().ItemSpacing.y));
}

#ifdef _WIN32
// Installed ASIO drivers register under HKLM\SOFTWARE\ASIO (and the WOW6432
// view); each subkey's name is the driver's user-visible name.
std::vector<std::string> EnumerateAsioDrivers() {
    std::vector<std::string> names;
    static const wchar_t* kRoots[] = {L"SOFTWARE\\ASIO", L"SOFTWARE\\WOW6432Node\\ASIO"};
    for (const wchar_t* root : kRoots) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        DWORD index = 0;
        wchar_t name[256];
        DWORD nameLen = 256;
        while (RegEnumKeyExW(key, index++, name, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            DWORD type = 0;
            wchar_t clsid[128] = {};
            DWORD cb = sizeof(clsid);
            if (RegGetValueW(key, name, L"CLSID", RRF_RT_REG_SZ, &type, clsid, &cb) == ERROR_SUCCESS && clsid[0]) {
                std::string n = W2U(name);
                if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(std::move(n));
            }
            nameLen = 256;
        }
        RegCloseKey(key);
    }
    return names;
}
#endif

bool BrowseDll(std::wstring& path) {
#ifdef _WIN32
    wchar_t fileBuf[1024] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Synth DLL (*.dll)\0*.dll\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = static_cast<DWORD>(_countof(fileBuf));
    ofn.lpstrTitle = L"Choose a synth DLL";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    path = fileBuf;
    return true;
#else
    std::string chosen;
    if (!util::PickFile(false, "Choose a synth DLL", "", "*.dll *.so", chosen)) return false;
    path = U2W(chosen);
    return true;
#endif
}

const uint32_t kRates[6] = {44100, 48000, 88200, 96000, 176400, 192000};
const char* const kRateLabels[6] = {"44.1k", "48k", "88.2k", "96k", "176.4k", "192k"};
const uint32_t kBuffers[8] = {64, 128, 256, 512, 1024, 2048, 4096, 8192};
const char* const kBufferLabels[8] = {"64", "128", "256", "512", "1k", "2k", "4k", "8k"};

template <size_t N>
int IndexOf(const uint32_t (&values)[N], uint32_t v) {
    for (size_t i = 0; i < N; ++i)
        if (values[i] == v) return static_cast<int>(i);
    return -1;
}

// --- Live audio output ---------------------------------------------------------------------

#ifndef _WIN32
// The Linux driver reopens ALSA only at start-up.
void ApplyAudioOutputLive(const ConfigValues&) {}
#else
// The value of `key=` in a driver answer made of key=value lines.
std::string KeyValue(const std::string& text, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (text.compare(pos, k.size(), k) == 0) return text.substr(pos + k.size(), end - pos - k.size());
        pos = end + 1;
    }
    return {};
}

std::vector<std::string> SplitText(const std::string& text, char sep) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (!text.empty() && pos <= text.size()) {
        size_t end = text.find(sep, pos);
        if (end == std::string::npos) end = text.size();
        parts.push_back(text.substr(pos, end - pos));
        pos = end + 1;
    }
    return parts;
}

bool IsAsio(const ConfigValues& w) { return EqualCI(W2U(w.audioBackend), "asio"); }

// Hands the page's whole output setting to the running driver, which restarts
// its audio output with it. Offline the change waits in the config file.
void ApplyAudioOutputLive(const ConfigValues& w) {
    if (!GetLiveLinkContext().connected) return;
    const std::string request = std::string("backend=") + (IsAsio(w) ? "asio" : "wasapi-shared") +
        "\ndevice=" + W2U(w.audioDevice) + "\nframes=" + std::to_string(w.bufferFrames) +
        "\nrate=" + std::to_string(w.sampleRate) + "\nfirst_channel=" + std::to_string(w.asioFirstChannel);
    std::string result;
    if (SendLiveText(SVMS_COMMAND_SET_AUDIO_OUTPUT, request, result)) NoteLiveChange();
}

// What an ASIO driver accepts, for the buffer slider and the pair picker.
struct AsioCapsView {
    std::string driver;   // the driver these caps describe
    bool ok = false;
    std::vector<uint32_t> sizes;
    uint32_t outputs = 0;
    std::vector<std::string> names;
    std::string error;
};

AsioCapsView ProbeAsioCaps(const std::string& driver) {
    AsioCapsView v;
    v.driver = driver;
    if (driver.empty()) {
        v.error = "no ASIO driver is installed";
        return v;
    }
    std::string text;
    if (GetLiveLinkContext().connected) {
        // The running synth asks its own ASIO thread (it may hold the driver).
        if (!SendLiveText(SVMS_COMMAND_PROBE_ASIO_DRIVER, driver, text, 6000u)) {
            v.error = text;
            return v;
        }
        for (const std::string& s : SplitText(KeyValue(text, "sizes"), ','))
            if (const unsigned long n = std::strtoul(s.c_str(), nullptr, 10)) v.sizes.push_back(static_cast<uint32_t>(n));
        v.outputs = static_cast<uint32_t>(std::strtoul(KeyValue(text, "outputs").c_str(), nullptr, 10));
        v.names = SplitText(KeyValue(text, "names"), '|');
    } else {
#if defined(SVMS_WITH_ASIO)
        // No synth running: load the driver here for a moment.
        AsioCaps caps;
        const bool ok = AsioHost::Instance().Probe(driver, caps, v.error);
        AsioHost::Instance().ShutdownForProcessEnd();
        if (!ok) return v;
        v.sizes = caps.legalSizes;
        v.outputs = caps.outputChannels;
        v.names = caps.outputNames;
#else
        v.error = "ASIO is not built into this Configurator";
        return v;
#endif
    }
    v.ok = !v.sizes.empty();
    if (!v.ok && v.error.empty()) v.error = "the driver reported no buffer sizes";
    return v;
}

// The driver the synth opens for `w.audioDevice` ("default" = the first one installed).
std::string AsioDriverFor(const ConfigValues& w, const std::vector<std::string>& installed) {
    if (!w.audioDevice.empty() && w.audioDevice != L"default") return W2U(w.audioDevice);
    return installed.empty() ? std::string() : installed.front();
}

AsioCapsView& AsioCapsFor(const ConfigValues& w, const std::vector<std::string>& installed, bool refresh) {
    static AsioCapsView caps;
    static bool probed = false;
    const std::string driver = AsioDriverFor(w, installed);
    if (refresh || !probed || caps.driver != driver) {
        caps = ProbeAsioCaps(driver);
        probed = true;
    }
    return caps;
}

// The installed ASIO drivers, read from the registry once and again on Refresh.
std::vector<std::string>& InstalledAsioDrivers(bool refresh) {
    static std::vector<std::string> drivers;
    static bool read = false;
    if (refresh || !read) {
        drivers = EnumerateAsioDrivers();
        read = true;
    }
    return drivers;
}

std::string OutputName(const AsioCapsView& caps, uint32_t channel) {
    if (channel < caps.names.size() && !caps.names[channel].empty()) return caps.names[channel];
    return "Out " + std::to_string(channel + 1u);
}

// One line on what the driver is playing through right now, polled once a second.
void StatusLine(float wrapW) {
    static double lastPoll = -10.0;
    static std::string status;
    if (!GetLiveLinkContext().connected) return;
    const double now = ImGui::GetTime();
    if (now - lastPoll >= 1.0) {
        lastPoll = now;
        std::string text;
        if (SendLiveText(SVMS_COMMAND_QUERY_AUDIO_OUTPUT, std::string(), text, 200u)) {
            const std::string active = KeyValue(text, "active");
            const unsigned frames = static_cast<unsigned>(std::strtoul(KeyValue(text, "frames").c_str(), nullptr, 10));
            const unsigned rate = static_cast<unsigned>(std::strtoul(KeyValue(text, "rate").c_str(), nullptr, 10));
            const unsigned first = static_cast<unsigned>(std::strtoul(KeyValue(text, "first_channel").c_str(), nullptr, 10));
            char line[256];
            if (KeyValue(text, "restarting") == "1") {
                status = "Restarting the audio output...";
            } else if (KeyValue(text, "running") != "1") {
                status = "Audio output stopped.";
            } else {
                std::snprintf(line, sizeof(line), "Playing: %s, %s, %u frames at %u Hz (%.1f ms)",
                              active.c_str(), KeyValue(text, "device").c_str(), frames, rate,
                              rate ? frames * 1000.0 / rate : 0.0);
                status = line;
                if (active == "asio") {
                    std::snprintf(line, sizeof(line), ", outputs %u/%u", first + 1u, first + 2u);
                    status += line;
                }
                status += ".";
            }
            if (KeyValue(text, "fallback") == "1") status += " ASIO did not open, so WASAPI took over.";
            const std::string note = KeyValue(text, "note");
            const std::string error = KeyValue(text, "error");
            if (!note.empty()) status += " " + note;
            if (!error.empty()) status += " " + error;
        } else {
            status.clear();
        }
    }
    if (status.empty()) return;
    ImGui::Dummy(ImVec2(0.0f, 10.0f * ui::Scale() - ImGui::GetStyle().ItemSpacing.y));
    NoteText(status.c_str(), wrapW);
}
#endif

// --- Audio device card --------------------------------------------------------------------

void DeviceCard(ConfigDocument& doc, const EasterEggState& eggs) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
#ifdef _WIN32
    static WasapiDeviceList deviceList;
    static bool enumerated = false;
    if (!enumerated) {
        deviceList.Enumerate();
        enumerated = true;
    }
    const bool asio = IsAsio(w);
    const std::vector<std::string>& asioDrivers = InstalledAsioDrivers(false);

    std::vector<std::string> names;
    int current = -1;
    const std::string configured = W2U(w.audioDevice);
    const bool isDefault = w.audioDevice.empty() || w.audioDevice == L"default";
    if (asio) {
        names.push_back("Default (first installed ASIO driver)");
        names.insert(names.end(), asioDrivers.begin(), asioDrivers.end());
        if (isDefault) current = 0;
        else
            for (size_t i = 0; i < asioDrivers.size(); ++i)
                if (asioDrivers[i] == configured) current = static_cast<int>(i + 1);
    } else {
        names = deviceList.FriendlyNames();
        if (isDefault) current = 0;
        else
            for (size_t i = 0; i < deviceList.Devices().size(); ++i) {
                const auto& dev = deviceList.Devices()[i];
                if (EqualCI(W2U(dev.friendlyName), configured) || W2U(dev.id) == configured) current = static_cast<int>(i + 1);
            }
    }
    const int known = static_cast<int>(names.size());
    if (current < 0) {
        names.push_back((asio ? "Missing ASIO driver: " : "Missing: ") + configured);
        current = known;
    }
    if (eggs.megaFuckerDac && !asio && current < known) names[static_cast<size_t>(current)] = "MegaFucker DAC Pro 9000";
    std::vector<const char*> items;
    for (const std::string& n : names) items.push_back(n.c_str());

    ui::BeginCard("##device", "Audio device", nullptr, 0.0f, "device");
    const float refreshW = ui::ButtonSize("Refresh").x;
    int sel = current;
    if (ui::Select("##device", &sel, items.data(), static_cast<int>(items.size()),
                   (std::max)(180.0f * s, ImGui::GetContentRegionAvail().x - refreshW - 10.0f * s)) && sel != current && sel < known) {
        if (sel == 0) w.audioDevice = L"default";
        else if (asio) w.audioDevice = U2W(asioDrivers[static_cast<size_t>(sel - 1)]);
        else w.audioDevice = deviceList.Devices()[static_cast<size_t>(sel - 1)].friendlyName;
        doc.MarkDirty();
        ApplyAudioOutputLive(w);
    }
    ImGui::SameLine(0.0f, 10.0f * s);
    if (ui::Button("Refresh")) {
        deviceList.Enumerate();
        InstalledAsioDrivers(true);
        if (asio) AsioCapsFor(w, InstalledAsioDrivers(false), true);
    }
    if (eggs.megaFuckerDac) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().exp);
        ui::PushText(0, 13.0f);
        ImGui::TextUnformatted("Display override active - your actual audio device has not changed.");
        ui::PopFont();
        ImGui::PopStyleColor();
    }

    static const char* backends[2] = {"WASAPI", "ASIO"};
    int be = asio ? 1 : 0;
    ImGui::Dummy(ImVec2(0.0f, 10.0f * s - ImGui::GetStyle().ItemSpacing.y));
    ui::BeginRow("audioBe", "Driver type",
                 "WASAPI works everywhere. ASIO is lower latency but needs an ASIO driver installed; falls back to WASAPI if none opens.",
                 ui::SegmentedSize(backends, 2));
    if (ui::Segmented("##backend", &be, backends, 2) && be != (asio ? 1 : 0)) {
        w.audioBackend = be == 1 ? L"asio" : L"wasapi-shared";
        // A WASAPI device name means nothing to ASIO and the other way round.
        w.audioDevice = L"default";
        doc.MarkDirty();
        ApplyAudioOutputLive(w);
    }
    ui::EndRow();

    if (asio) {
        const AsioCapsView& caps = AsioCapsFor(w, asioDrivers, false);
        std::vector<std::string> pairs;
        std::vector<uint32_t> firsts;
        for (uint32_t c = 0; c + 1u < caps.outputs; c += 2u) {
            pairs.push_back(OutputName(caps, c) + " / " + OutputName(caps, c + 1u));
            firsts.push_back(c);
        }
        int pair = -1;
        for (size_t i = 0; i < firsts.size(); ++i)
            if (firsts[i] == w.asioFirstChannel) pair = static_cast<int>(i);
        if (pair < 0) {
            // Unprobed driver or a pair it no longer has: keep showing what is set.
            pairs.push_back(OutputName(caps, w.asioFirstChannel) + " / " + OutputName(caps, w.asioFirstChannel + 1u));
            firsts.push_back(w.asioFirstChannel);
            pair = static_cast<int>(firsts.size() - 1u);
        }
        std::vector<const char*> pairItems;
        for (const std::string& p : pairs) pairItems.push_back(p.c_str());
        ImGui::Dummy(ImVec2(0.0f, 10.0f * s - ImGui::GetStyle().ItemSpacing.y));
        const float pickW = 220.0f * s;
        ui::BeginRow("asioPair", "Output pair", "The two driver outputs SVMS plays left and right through.",
                     ImVec2(pickW, 38.0f * s));
        int picked = pair;
        if (ui::Select("##asiopair", &picked, pairItems.data(), static_cast<int>(pairItems.size()), pickW) &&
            picked != pair) {
            w.asioFirstChannel = firsts[static_cast<size_t>(picked)];
            doc.MarkDirty();
            ApplyAudioOutputLive(w);
        }
        ui::EndRow();
        if (!caps.ok && !caps.error.empty()) {
            const std::string why = "Could not ask the driver what it supports: " + caps.error;
            NoteText(why.c_str(), ImGui::GetContentRegionAvail().x);
        }
    }
    StatusLine(ImGui::GetContentRegionAvail().x);
    ui::EndCard();
#else
    (void)eggs;
    ui::BeginCard("##device", "Audio device", "The driver renders to one ALSA PCM. 'default' follows PipeWire/PulseAudio; or name one (hw:0,0, plughw:1,0).", 0.0f, "device");
    char device[128]{};
    W2U(w.linuxDevice).copy(device, sizeof(device) - 1u);
    if (ui::Field("##alsa", device, sizeof(device), ImGui::GetContentRegionAvail().x, "default")) {
        w.linuxDevice = U2W(device);
        doc.MarkDirty();
    }
    ui::EndCard();
    (void)s;
#endif
}

// --- Latency card ---------------------------------------------------------------------------

void LatencyBar(float pos, float width) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0.0f, 16.0f * s - ImGui::GetStyle().ItemSpacing.y));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 8.0f * s;
    // ok -> accent (60 %) -> bad, at 85 % opacity.
    const float split = 0.6f;
    const ImU32 a = ui::WithAlpha(k.ok, 0.85f), b = ui::WithAlpha(k.acc, 0.85f), c = ui::WithAlpha(k.bad, 0.85f);
    dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), a, h * 0.5f);
    dl->AddRectFilled(ImVec2(p.x + width - h, p.y), ImVec2(p.x + width, p.y + h), c, h * 0.5f);
    dl->AddRectFilledMultiColor(ImVec2(p.x + h * 0.5f, p.y), ImVec2(p.x + width * split, p.y + h), a, b, b, a);
    dl->AddRectFilledMultiColor(ImVec2(p.x + width * split, p.y), ImVec2(p.x + width - h * 0.5f, p.y + h), b, c, c, b);
    const float mx = p.x + width * pos / 100.0f;
    const float x = AnimSpring(ImGui::GetID("##latmark"), mx - p.x) + p.x;
    const ImVec2 ma(x - 2.0f * s, p.y - 5.0f * s), mb(x + 2.0f * s, p.y - 5.0f * s + 18.0f * s);
    dl->AddRectFilled(ImVec2(ma.x - 3.0f * s, ma.y - 3.0f * s), ImVec2(mb.x + 3.0f * s, mb.y + 3.0f * s), k.bg, 5.0f * s);
    dl->AddRectFilled(ma, mb, IM_COL32(255, 255, 255, 255), 2.0f * s);
    ImGui::Dummy(ImVec2(width, h));
    ImGui::Dummy(ImVec2(0.0f, 6.0f * s - ImGui::GetStyle().ItemSpacing.y));
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    ui::PushText(0, 11.0f);
    const char* l = "Snappy, can crackle";
    const char* r = "Safe for heavy Black MIDI";
    dl->AddText(lp, k.muted, l);
    dl->AddText(ImVec2(lp.x + width - ImGui::CalcTextSize(r).x, lp.y), k.muted, r);
    ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
    ui::PopFont();
}

void LatencyCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
#ifdef _WIN32
    // The running driver restarts its output in place, so nothing here waits for a restart.
    ui::BeginCard("##latency", "Latency", nullptr, 0.0f, "buf");
#else
    ui::BeginCard("##latency", "Latency", nullptr, ui::RestartMarkWidth(), "buf");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 11.0f * s);
    ui::RestartMark();
    ui::CardHeaderEnd();
#endif

    const float lat = static_cast<float>(w.bufferFrames) / static_cast<float>((std::max)(1u, w.sampleRate)) * 1000.0f;
    const float pos = (std::max)(0.0f, (std::min)(100.0f, (std::log2(static_cast<float>((std::max)(1u, w.bufferFrames))) - 6.0f) / 7.0f * 100.0f));
    const char* advice = lat < 6.0f ? "Very low. Expect crackles on dense files."
        : lat < 25.0f ? "Good for playing a keyboard live. Dense Black MIDI may stutter."
        : lat < 100.0f ? "A safe middle ground for Black MIDI playback."
        : "Very stable, but you will feel the delay when playing live.";

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float gap = 16.0f * s;
    const float colW = (width - gap) * 0.5f;

    // Left: readout, gradient marker, advice.
    ImGui::BeginGroup();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        char big[24];
        std::snprintf(big, sizeof(big), "%.1f", lat);
        ui::PushNum(44.0f);
        const ImVec2 bs = ImGui::CalcTextSize(big);
        dl->AddText(p, k.text, big);
        ui::PopFont();
        ui::PushText(0, 13.0f);
        const ImVec2 ms = ImGui::CalcTextSize("ms delay");
        dl->AddText(ImVec2(p.x + bs.x + 8.0f * s, p.y + bs.y - ms.y - 6.0f * s), k.muted, "ms delay");
        ui::PopFont();
        ImGui::Dummy(ImVec2(colW, bs.y));
    }
    LatencyBar(pos, colW);
    ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
    NoteText(advice, colW);
    ImGui::EndGroup();
    const float leftBottom = ImGui::GetItemRectMax().y;

    // Right: buffer and sample-rate segments.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + colW + gap, origin.y));
    ImGui::BeginGroup();
    Label("Buffer size (frames)", 0.0f);
    bool slid = false;
#ifdef _WIN32
    if (IsAsio(w)) {
        // Every size the driver accepts, not just the powers of two.
        const AsioCapsView& caps = AsioCapsFor(w, InstalledAsioDrivers(false), false);
        if (caps.ok) {
            slid = true;
            const uint32_t shown = AsioNearestLegalSize(caps.sizes, w.bufferFrames);
            int at = 0;
            for (size_t i = 0; i < caps.sizes.size(); ++i)
                if (caps.sizes[i] == shown) at = static_cast<int>(i);
            float slot = static_cast<float>(at);
            bool committed = false;
            static bool moved = false;
            const float last = static_cast<float>(caps.sizes.size() - 1u);
            if (ui::RangeSlider("##asiobuf", &slot, 0.0f, last, colW, &committed)) {
                const int next = static_cast<int>(std::lround(slot));
                if (caps.sizes[static_cast<size_t>(next)] != w.bufferFrames) {
                    w.bufferFrames = caps.sizes[static_cast<size_t>(next)];
                    doc.MarkDirty();
                    moved = true;
                }
            }
            // Restart once when the drag ends, not on every step of it.
            if (committed && moved) {
                ApplyAudioOutputLive(w);
                moved = false;
            }
            char readout[64];
            std::snprintf(readout, sizeof(readout), "%u frames (%.2f ms)", w.bufferFrames,
                          w.bufferFrames * 1000.0 / (std::max)(1u, w.sampleRate));
            NoteText(readout);
        }
    }
#endif
    if (!slid) {
        int buf = IndexOf(kBuffers, w.bufferFrames);
        if (ui::Segmented("##buf", &buf, kBufferLabels, 8, colW) && buf >= 0) {
            w.bufferFrames = kBuffers[buf];
            doc.MarkDirty();
            ApplyAudioOutputLive(w);
        }
    }
    const ImVec2 srAt = ImGui::GetCursorScreenPos();
    ui::Anchor("sr", srAt, ImVec2(srAt.x + colW, srAt.y + 80.0f * s));
    Label("Sample rate (Hz)", 16.0f);
    int sr = IndexOf(kRates, w.sampleRate);
    if (ui::Segmented("##sr", &sr, kRateLabels, 6, colW) && sr >= 0) {
        w.sampleRate = kRates[sr];
        doc.MarkDirty();
        ApplyAudioOutputLive(w);
    }
    ImGui::EndGroup();
    const float rightBottom = ImGui::GetItemRectMax().y;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, (std::max)(leftBottom, rightBottom)));
    ImGui::Dummy(ImVec2(width, 0.0f));
    ui::EndCard();
}

// --- Different synth -------------------------------------------------------------------------

void SynthCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    static const char* kinds[5] = {"SVMS", "SVMS-API DLL", "KDMAPI DLL", "WinMM device", "Auto"};
    static const char* descs[5] = {
        "Play everything with the SVMS engine. This is what you want unless you know otherwise.",
        "Forward MIDI to a synth DLL that implements the SVMS-API backend interface. That synth makes the sound.",
        "Forward MIDI to a KDMAPI synth such as OmniMIDI. That synth makes the sound.",
        "Forward MIDI to a Windows MIDI-out device, such as the Microsoft GS Wavetable Synth.",
        "Try the DLL below as each kind of backend, and fall back to the SVMS engine if none fits."};
    if (!ui::Disclosure("out", "Send MIDI to a different synth", "For routing through OmniMIDI, a WinMM device, or another DLL")) return;
    ui::BeginReveal("out");
    ui::BeginCard("##synth", "Synth backend", nullptr, ui::RestartMarkWidth(), "synthBe");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 11.0f * s);
    ui::RestartMark();
    ui::CardHeaderEnd();
    int kind = static_cast<int>((std::min)(4u, w.apiBackend));
    if (ui::Segmented("##kind", &kind, kinds, 5, ImGui::GetContentRegionAvail().x)) {
        w.apiBackend = static_cast<uint32_t>(kind);
        doc.MarkDirty();
    }
    // The description panel (.pdesc).
    {
        const ui::Tokens& k = ui::Tok();
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ui::PushText(0, 13.0f);
        const float wrap = width - 36.0f * s;
        const ImVec2 ts = ImGui::CalcTextSize(descs[kind], nullptr, false, wrap);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + ts.y + 28.0f * s), k.card2, k.rack ? 3.0f * s : 10.0f * s);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 18.0f * s, p.y + 14.0f * s));
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextUnformatted(descs[kind]);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ts.y + 28.0f * s));
        ImGui::Dummy(ImVec2(width, 0.0f));
    }
    const bool needsDll = kind == 1 || kind == 2 || kind == 4;
    if (needsDll || kind == 3) {
        ui::BeginNest();
        if (needsDll) {
            const float fieldW = 260.0f * s, browseW = ui::ButtonSize("Browse").x;
            ui::BeginRow("dll", "Synth DLL", "For example OmniMIDI\\OmniMIDI.dll",
                         ImVec2(fieldW + 8.0f * s + browseW, 38.0f * s));
            char dll[512]{};
            W2U(w.apiBackendDll).copy(dll, sizeof(dll) - 1u);
            if (ui::Field("##dll", dll, sizeof(dll), fieldW, "Choose a .dll...")) {
                w.apiBackendDll = U2W(dll);
                doc.MarkDirty();
            }
            ImGui::SameLine(0.0f, 8.0f * s);
            if (ui::Button("Browse")) {
                std::wstring chosen;
                if (BrowseDll(chosen)) {
                    w.apiBackendDll = chosen;
                    doc.MarkDirty();
                }
            }
            ui::EndRow();
        } else {
            int dev = static_cast<int>(w.apiWinMmDevice);
            ui::BeginRow("winmmDev", "MIDI-out device", "Index of the Windows MIDI-out device (0 is the first).", ImVec2(150.0f * s, 38.0f * s));
            if (ui::NumberField("##winmm", &dev, 0, 255, 150.0f)) {
                w.apiWinMmDevice = static_cast<uint32_t>(dev);
                doc.MarkDirty();
            }
            ui::EndRow();
        }
        const ui::ChipDef chip[1] = {{"Advanced", ui::ChipKind::Exp}};
        ui::BeginRow("allowNested", "Allow nested",
                     "Only if an SVMS running inside another synth host uses its own config. Otherwise MIDI would loop back.",
                     ImVec2(46.0f * s, 28.0f * s), chip, 1);
        if (ui::Toggle("##nested", &w.apiAllowNested)) doc.MarkDirty();
        ui::EndRow();
        ui::EndNest();
    }
    ui::EndCard();
    ui::EndReveal();
}

} // namespace

static void ChannelsCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    ui::BeginCard("##channels", "Output channels", nullptr, 0.0f, "monoDown");
    ui::BeginRow("monoDownmix", "Mono output",
                 "Both speakers get the average of left and right. Useful for MIDIs that pan hard to one side. Applies immediately.",
                 ImVec2(46.0f * s, 28.0f * s));
    if (ui::Toggle("##monodownmix", &w.monoDownmix)) {
        doc.MarkDirty();
        SendLiveCommand(SVMS_COMMAND_SET_MONO_DOWNMIX, w.monoDownmix ? 1u : 0u);
    }
    ui::EndRow();
    ui::EndCard();
}

void DrawOutputPage(ConfigDocument& doc, const EasterEggState& eggs) {
    DeviceCard(doc, eggs);
    ChannelsCard(doc);
    LatencyCard(doc);
    SynthCard(doc);
}

} // namespace svms::cfg
