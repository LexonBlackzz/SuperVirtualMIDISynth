#include "SVMSConfig.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace svms {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr uint32_t kConfigSchemaVersion = 1;
constexpr wchar_t kConfigMutexName[] = L"Local\\SuperVirtualMIDISynth_Config_v1";

bool PathExists(const fs::path& path) noexcept {
    if (path.empty()) return false;
    std::error_code error;
    const bool exists = fs::exists(path, error);
    return !error && exists;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::wstring GetExecutableDirectory() {
    std::wstring path(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                     static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return fs::path(path).parent_path().wstring();
}

fs::path GetSoundFontSearchDirectory() {
    // Test-only override. Production always discovers beside winmm.dll.
    std::wstring testPath(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"SVMS_TEST_SOUNDFONT_DIRECTORY", testPath.data(),
        static_cast<DWORD>(testPath.size()));
    if (length > 0u && length < testPath.size()) {
        testPath.resize(length);
        return fs::path(testPath);
    }
    return fs::path(GetV3ModuleDirectory());
}

bool IsSoundFontFile(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) return false;
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == L".sf2";
}

std::vector<fs::path> DiscoverLocalSoundFonts() {
    std::vector<fs::path> paths;
    std::error_code error;
    const fs::path directory = GetSoundFontSearchDirectory();
    fs::directory_iterator iterator(directory, error);
    const fs::directory_iterator end;
    while (!error && iterator != end) {
        if (IsSoundFontFile(iterator->path())) paths.push_back(iterator->path());
        iterator.increment(error);
    }
    std::sort(paths.begin(), paths.end(), [](const fs::path& left,
                                             const fs::path& right) {
        std::wstring a = left.filename().wstring();
        std::wstring b = right.filename().wstring();
        std::transform(a.begin(), a.end(), a.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        std::transform(b.begin(), b.end(), b.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (a != b) return a < b;
        return left.filename().wstring() < right.filename().wstring();
    });
    return paths;
}

std::string Trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::map<std::string, std::string> ReadLegacyIni(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = Trim(line.substr(0, equals));
        std::string value = Trim(line.substr(equals + 1));
        if (!key.empty()) values[key] = value;
    }
    return values;
}

bool ParseBool(const std::string& text, bool fallback) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "1" || lower == "true" || lower == "yes") return true;
    if (lower == "0" || lower == "false" || lower == "no") return false;
    return fallback;
}

template <typename T>
void ImportNumber(const std::map<std::string, std::string>& ini,
                  const char* key, json& destination) {
    auto it = ini.find(key);
    if (it == ini.end()) return;
    std::istringstream input(it->second);
    T value{};
    if (input >> value) destination = value;
}

json MakeDefaultJson(const EngineConfig& cfg) {
    const char* backend = cfg.audioBackend == AudioBackend::DirectSound
                            ? "directsound" : "wasapi-shared";
    return json{
        {"schema_version", kConfigSchemaVersion},
        {"audio", {
            {"backend", backend},
            {"device", WideToUtf8(cfg.audioDevice)},
            {"sample_rate", cfg.sampleRate},
            {"buffer_frames", cfg.bufferFrames}
        }},
        {"synth", {
            {"soundfont", WideToUtf8(cfg.soundFontPath)},
            {"max_voices", cfg.maxVoices},
            {"master_volume", cfg.masterVolume},
            {"velocity_curve", cfg.velocityCurve},
            {"velocity_floor", cfg.velocityFloor},
            {"velocity_ignore_below", cfg.velocityIgnoreBelow}
        }},
        {"events", {
            {"overflow_mode", "priority"},
            {"ring_capacity", cfg.eventRingCapacity},
            {"high_priority_velocity", cfg.highPriorityVelocity},
            {"shed_start_percent", cfg.shedStartPercent},
            {"max_events_per_block", cfg.maxEventsPerBlock}
        }},
        {"quality", {
            {"correctness_mode", cfg.correctnessMode},
            {"interpolation", "linear"},
            {"pan_law", "constant-power"}
        }},
        {"diagnostics", {
            {"enabled", cfg.diagnosticsEnabled},
            {"window", cfg.diagnosticsWindow},
            {"debug_output", cfg.diagnosticsDebugOutput}
        }}
    };
}

void ImportLegacyIni(json& root, const fs::path& path) {
    const auto ini = ReadLegacyIni(path);
    if (ini.empty()) return;

    ImportNumber<uint32_t>(ini, "sample_rate", root["audio"]["sample_rate"]);
    ImportNumber<uint32_t>(ini, "buffer_frames", root["audio"]["buffer_frames"]);
    ImportNumber<uint32_t>(ini, "max_voices", root["synth"]["max_voices"]);
    ImportNumber<float>(ini, "master_volume", root["synth"]["master_volume"]);
    ImportNumber<float>(ini, "velocity_curve", root["synth"]["velocity_curve"]);
    ImportNumber<float>(ini, "velocity_floor", root["synth"]["velocity_floor"]);
    ImportNumber<uint32_t>(ini, "velocity_ignore_below",
                           root["synth"]["velocity_ignore_below"]);

    auto importString = [&](const char* oldKey, json& target) {
        auto it = ini.find(oldKey);
        if (it != ini.end() && !it->second.empty()) target = it->second;
    };
    importString("audio_backend", root["audio"]["backend"]);
    auto source = ini.find("soundfont");
    if (source == ini.end() || source->second.empty()) source = ini.find("sound_source");
    if (source != ini.end() && !source->second.empty())
        root["synth"]["soundfont"] = source->second;

    auto correctness = ini.find("correctness_mode");
    if (correctness != ini.end())
        root["quality"]["correctness_mode"] = ParseBool(correctness->second, true);

    // Effects remain outside this stabilization tranche, but preserve every
    // recognized legacy effects/limiter setting in the migration document
    // so the one-time import is lossless and a later schema can promote it.
    static constexpr const char* preservedLegacyKeys[] = {
        "reverb_enable", "reverb_blur", "reverb_feedback", "reverb_mix",
        "reverb_tone", "reverb_width", "chorus_enable", "filter_enable",
        "limiter_enable", "limiter_release_ms", "limiter_threshold"
    };
    for (const char* key : preservedLegacyKeys) {
        auto value = ini.find(key);
        if (value != ini.end()) root["legacy_import"][key] = value->second;
    }
}

bool AtomicWriteJson(const fs::path& target, const json& root) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) return false;

    fs::path temporary = target;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << root.dump(2) << '\n';
        output.flush();
        if (!output) return false;
    }

    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

fs::path FindLegacyIni() {
    std::wstring testPath(32768, L'\0');
    DWORD testLength = GetEnvironmentVariableW(L"SVMS_TEST_LEGACY_INI",
        testPath.data(), static_cast<DWORD>(testPath.size()));
    if (testLength > 0 && testLength < testPath.size()) {
        testPath.resize(testLength);
        return fs::path(testPath);
    }
    const fs::path moduleIni = fs::path(GetV3ModuleDirectory()) / L"config.ini";
    if (fs::exists(moduleIni)) return moduleIni;
    const fs::path executableIni = fs::path(GetExecutableDirectory()) / L"config.ini";
    if (fs::exists(executableIni)) return executableIni;
    return {};
}

template <typename T>
bool ReadValue(const json& object, const char* key, T& destination,
               const T& minimum, const T& maximum) {
    auto it = object.find(key);
    if (it == object.end()) return true;
    try {
        T value = it->get<T>();
        if (value < minimum || value > maximum) return false;
        destination = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadBool(const json& object, const char* key, bool& destination) {
    auto it = object.find(key);
    if (it == object.end()) return true;
    if (!it->is_boolean()) return false;
    destination = it->get<bool>();
    return true;
}

void AppendWarning(std::string& warning, const char* field) {
    if (!warning.empty()) warning += "; ";
    warning += "invalid config field: ";
    warning += field;
}

void ApplyJson(const json& root, EngineConfig& cfg) {
    if (auto it = root.find("audio"); it != root.end() && it->is_object()) {
        auto device = it->find("device");
        if (device != it->end()) {
            if (device->is_string()) cfg.audioDevice = Utf8ToWide(device->get<std::string>());
            else AppendWarning(cfg.configWarning, "audio.device");
        }
        auto backend = it->find("backend");
        if (backend != it->end()) {
            if (!backend->is_string()) {
                AppendWarning(cfg.configWarning, "audio.backend");
            } else {
                const std::string value = backend->get<std::string>();
#if defined(SVMS_XP_COMPAT)
                if (value == "directsound")
                    cfg.audioBackend = AudioBackend::DirectSound;
                else
                    AppendWarning(cfg.configWarning, "audio.backend");
#else
                if (value == "wasapi-shared")
                    cfg.audioBackend = AudioBackend::WASAPIShared;
                else
                    AppendWarning(cfg.configWarning, "audio.backend");
#endif
            }
        }
        if (!ReadValue(*it, "sample_rate", cfg.sampleRate, 8000u, 384000u))
            AppendWarning(cfg.configWarning, "audio.sample_rate");
        if (!ReadValue(*it, "buffer_frames", cfg.bufferFrames, 16u, 8192u))
            AppendWarning(cfg.configWarning, "audio.buffer_frames");
    }
    if (auto it = root.find("synth"); it != root.end() && it->is_object()) {
        if (!ReadValue(*it, "max_voices", cfg.maxVoices, 1u, kMaxPolyphony))
            AppendWarning(cfg.configWarning, "synth.max_voices");
        if (!ReadValue(*it, "master_volume", cfg.masterVolume, 0.0f, 4.0f))
            AppendWarning(cfg.configWarning, "synth.master_volume");
        if (!ReadValue(*it, "velocity_curve", cfg.velocityCurve, 0.1f, 10.0f))
            AppendWarning(cfg.configWarning, "synth.velocity_curve");
        if (!ReadValue(*it, "velocity_floor", cfg.velocityFloor, 0.0f, 0.99f))
            AppendWarning(cfg.configWarning, "synth.velocity_floor");
        uint32_t threshold = cfg.velocityIgnoreBelow;
        if (!ReadValue(*it, "velocity_ignore_below", threshold, 0u, 127u))
            AppendWarning(cfg.configWarning, "synth.velocity_ignore_below");
        else cfg.velocityIgnoreBelow = static_cast<uint8_t>(threshold);
        auto sf = it->find("soundfont");
        if (sf != it->end()) {
            if (sf->is_string()) cfg.soundFontPath = Utf8ToWide(sf->get<std::string>());
            else AppendWarning(cfg.configWarning, "synth.soundfont");
        }
    }
    if (auto it = root.find("events"); it != root.end() && it->is_object()) {
        if (!ReadValue(*it, "ring_capacity", cfg.eventRingCapacity,
                       4096u, kDefaultEventRingCapacity))
            AppendWarning(cfg.configWarning, "events.ring_capacity");
        if (!ReadValue(*it, "high_priority_velocity", cfg.highPriorityVelocity, 1u, 127u))
            AppendWarning(cfg.configWarning, "events.high_priority_velocity");
        if (!ReadValue(*it, "shed_start_percent", cfg.shedStartPercent, 1u, 99u))
            AppendWarning(cfg.configWarning, "events.shed_start_percent");
        if (!ReadValue(*it, "max_events_per_block", cfg.maxEventsPerBlock, 1u,
                       kEventBufferCapacity))
            AppendWarning(cfg.configWarning, "events.max_events_per_block");
        if (cfg.maxEventsPerBlock > cfg.eventRingCapacity) {
            cfg.maxEventsPerBlock = cfg.eventRingCapacity;
            AppendWarning(cfg.configWarning,
                          "events.max_events_per_block exceeds ring_capacity");
        }
        auto mode = it->find("overflow_mode");
        if (mode != it->end()) {
            if (mode->is_string() && mode->get<std::string>() == "lossless")
                cfg.eventOverflowMode = EventOverflowMode::LosslessBackpressure;
            else if (mode->is_string() && mode->get<std::string>() == "priority")
                cfg.eventOverflowMode = EventOverflowMode::PriorityVelocity;
            else AppendWarning(cfg.configWarning, "events.overflow_mode");
        }
    }
    if (auto it = root.find("quality"); it != root.end() && it->is_object()) {
        if (!ReadBool(*it, "correctness_mode", cfg.correctnessMode))
            AppendWarning(cfg.configWarning, "quality.correctness_mode");
        auto interpolation = it->find("interpolation");
        if (interpolation != it->end()) {
            if (interpolation->is_string() && interpolation->get<std::string>() == "nearest")
                cfg.interpolation = InterpolationMode::Nearest;
            else if (interpolation->is_string() && interpolation->get<std::string>() == "linear")
                cfg.interpolation = InterpolationMode::Linear;
            else if (interpolation->is_string() && interpolation->get<std::string>() == "cubic")
                cfg.interpolation = InterpolationMode::Cubic;
            else AppendWarning(cfg.configWarning, "quality.interpolation");
        }
        auto pan = it->find("pan_law");
        if (pan != it->end()) {
            if (pan->is_string() && pan->get<std::string>() == "linear") cfg.panLaw = PanLaw::Linear;
            else if (pan->is_string() && pan->get<std::string>() == "constant-power") cfg.panLaw = PanLaw::ConstantPower;
            else if (pan->is_string() && pan->get<std::string>() == "balance") cfg.panLaw = PanLaw::Balance;
            else AppendWarning(cfg.configWarning, "quality.pan_law");
        }
    }
    if (auto it = root.find("diagnostics"); it != root.end() && it->is_object()) {
        if (!ReadBool(*it, "enabled", cfg.diagnosticsEnabled))
            AppendWarning(cfg.configWarning, "diagnostics.enabled");
        if (!ReadBool(*it, "window", cfg.diagnosticsWindow))
            AppendWarning(cfg.configWarning, "diagnostics.window");
        if (!ReadBool(*it, "debug_output", cfg.diagnosticsDebugOutput))
            AppendWarning(cfg.configWarning, "diagnostics.debug_output");
    }
}

bool EnvironmentFlag(const wchar_t* name, bool current) {
    wchar_t value[16]{};
    if (GetEnvironmentVariableW(name, value, 16) == 0) return current;
    std::wstring lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    if (lower == L"1" || lower == L"true" || lower == L"yes") return true;
    if (lower == L"0" || lower == L"false" || lower == L"no") return false;
    return current;
}

void ApplyEnvironment(EngineConfig& cfg) {
    wchar_t audioDevice[1024]{};
    const DWORD audioDeviceLength = GetEnvironmentVariableW(
        L"SVMS_AUDIO_DEVICE", audioDevice,
        static_cast<DWORD>(_countof(audioDevice)));
    if (audioDeviceLength > 0u && audioDeviceLength < _countof(audioDevice))
        cfg.audioDevice.assign(audioDevice, audioDeviceLength);
    if (EnvironmentFlag(L"SVMS_NO_DROP_EVENTS", false))
        cfg.eventOverflowMode = EventOverflowMode::LosslessBackpressure;
    cfg.correctnessMode = EnvironmentFlag(L"SVMS_CORRECTNESS_MODE", cfg.correctnessMode);
    cfg.diagnosticsEnabled = EnvironmentFlag(L"SVMS_DIAGNOSTICS", cfg.diagnosticsEnabled);
    cfg.diagnosticsWindow = EnvironmentFlag(L"SVMS_DIAGNOSTICS_WINDOW", cfg.diagnosticsWindow);
    cfg.diagnosticsDebugOutput = EnvironmentFlag(L"SVMS_DEBUG_OUTPUT", cfg.diagnosticsDebugOutput);
}

} // namespace

EngineConfig EngineConfig::Default() {
    EngineConfig cfg{};
    cfg.sampleRate = kDefaultSampleRate;
    cfg.bufferFrames = kDefaultBufferFrames;
    cfg.maxVoices = kMaxVoicesDefault;
    cfg.maxSampleCacheMB = 256;
    cfg.interpolation = InterpolationMode::Linear;
    cfg.filterType = FilterType::None;
#if defined(SVMS_XP_COMPAT)
    cfg.audioBackend = AudioBackend::DirectSound;
#else
    cfg.audioBackend = AudioBackend::WASAPIShared;
#endif
    cfg.renderBackend = RenderBackend::Scalar;
    cfg.panLaw = PanLaw::ConstantPower;
    cfg.masterVolume = 0.1f;
    cfg.velocityCurve = 1.0f;
    cfg.velocityFloor = 0.0f;
    cfg.velocityIgnoreBelow = 0;
    cfg.ignoreVelocity = false;
    cfg.monoOutput = false;
    cfg.enableReverb = false;
    cfg.enableChorus = false;
    cfg.enableFilter = false;
    cfg.enableModulators = false;
    cfg.gpuDeviceIndex = 0;
    cfg.enableGPU = false;
    cfg.eventRingCapacity = kDefaultEventRingCapacity;
    cfg.eventOverflowMode = EventOverflowMode::PriorityVelocity;
    cfg.highPriorityVelocity = 96;
    cfg.shedStartPercent = 70;
    cfg.maxEventsPerBlock = 65536;
    cfg.correctnessMode = true;
#if defined(SVMS_XP_COMPAT)
    // XP has no WASAPI status tooling and audio failures otherwise look like
    // a completely silent synth. Keep this configurable, but make the first
    // XP run visible by default.
    cfg.diagnosticsEnabled = true;
    cfg.diagnosticsWindow = true;
#else
    cfg.diagnosticsEnabled = false;
    cfg.diagnosticsWindow = false;
#endif
    cfg.diagnosticsDebugOutput = false;
    // Keep the first-run choice explicit in config.json while following the
    // user's current Windows default if that default changes later.
    cfg.audioDevice = L"default";
    cfg.soundFontPath.clear();
    return cfg;
}

EngineConfig EngineConfig::Load() {
    EngineConfig cfg = Default();

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kConfigMutexName);
    if (mutex) WaitForSingleObject(mutex, INFINITE);

    const fs::path localPath(GetV3LocalConfigPath());
    const fs::path appDataPath(GetV3AppDataConfigPath());
    fs::path path;
    if (PathExists(localPath)) path = localPath;
    else if (PathExists(appDataPath)) path = appDataPath;
    else if (!appDataPath.empty()) path = appDataPath;
    else path = localPath;
    cfg.configPath = path.wstring();

    if (!PathExists(path)) {
        // A new configuration records an actually discovered DLL-local SF2
        // instead of baking a particular filename into every installation.
        const auto localSoundFonts = DiscoverLocalSoundFonts();
        if (!localSoundFonts.empty())
            cfg.soundFontPath = localSoundFonts.front().filename().wstring();
        json root = MakeDefaultJson(cfg);
        const fs::path legacy = FindLegacyIni();
        if (!legacy.empty()) ImportLegacyIni(root, legacy);
        if (!AtomicWriteJson(path, root)) {
            // Portable/demo systems may not expose a writable Roaming
            // AppData folder. In that case use the DLL directory as the
            // first-run store if it is writable.
            if (path != localPath && !localPath.empty() &&
                AtomicWriteJson(localPath, root)) {
                path = localPath;
                cfg.configPath = path.wstring();
            } else {
                cfg.configWarning =
                    "unable to create config.json in AppData or DLL directory";
            }
        }
    }

    try {
        std::ifstream input(path, std::ios::binary);
        if (input) {
            json root;
            input >> root;
            const uint32_t version = root.value("schema_version", 0u);
            if (version == kConfigSchemaVersion) ApplyJson(root, cfg);
            else if (version > kConfigSchemaVersion)
                cfg.configWarning = "config schema is newer than this V3 build";
            else
                cfg.configWarning = "unsupported or missing config schema version";
        }
    } catch (const std::exception& error) {
        cfg = Default();
        cfg.configPath = path.wstring();
        cfg.configWarning = std::string("malformed config.json: ") + error.what();
    }

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

    ApplyEnvironment(cfg);
    return cfg;
}

bool EngineConfig::Validate() const {
    return sampleRate >= 8000 && sampleRate <= 384000 &&
           bufferFrames >= 16 && bufferFrames <= 8192 &&
           maxVoices >= 1 && maxVoices <= kMaxPolyphony &&
           masterVolume >= 0.0f && masterVolume <= 4.0f &&
           velocityCurve >= 0.1f && velocityCurve <= 10.0f &&
           velocityFloor >= 0.0f && velocityFloor < 1.0f &&
           eventRingCapacity >= 4096u &&
           eventRingCapacity <= kDefaultEventRingCapacity &&
           highPriorityVelocity >= 1 && highPriorityVelocity <= 127 &&
           shedStartPercent >= 1 && shedStartPercent < 100 &&
           maxEventsPerBlock > 0 &&
           maxEventsPerBlock <= eventRingCapacity;
}

std::wstring GetV3LocalConfigPath() {
    // Test harness isolation only. Normal hosts use the directory containing
    // this winmm.dll module.
    std::wstring testPath(32768, L'\0');
    DWORD testLength = GetEnvironmentVariableW(L"SVMS_TEST_LOCAL_CONFIG_PATH",
        testPath.data(), static_cast<DWORD>(testPath.size()));
    if (testLength > 0 && testLength < testPath.size()) {
        testPath.resize(testLength);
        return testPath;
    }
    return (fs::path(GetV3ModuleDirectory()) / L"config.json").wstring();
}

std::wstring GetV3AppDataConfigPath() {
    // Test harness isolation only. Normal hosts use Roaming AppData.
    std::wstring testPath(32768, L'\0');
    DWORD testLength = GetEnvironmentVariableW(L"SVMS_TEST_CONFIG_PATH",
        testPath.data(), static_cast<DWORD>(testPath.size()));
    if (testLength > 0 && testLength < testPath.size()) {
        testPath.resize(testLength);
        return testPath;
    }

    std::wstring result;
#if defined(SVMS_XP_COMPAT)
    wchar_t roaming[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE,
                                   nullptr, SHGFP_TYPE_CURRENT, roaming))) {
        result = (fs::path(roaming) / L"SuperVirtualMIDISynth" / L"config.json").wstring();
    }
#else
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,
                                       KF_FLAG_DEFAULT, nullptr, &roaming))) {
        result = (fs::path(roaming) / L"SuperVirtualMIDISynth" / L"config.json").wstring();
        CoTaskMemFree(roaming);
    }
#endif
    return result;
}

std::wstring GetV3ConfigPath() {
    const fs::path local(GetV3LocalConfigPath());
    if (PathExists(local)) return local.wstring();
    const fs::path appData(GetV3AppDataConfigPath());
    if (!appData.empty()) return appData.wstring();
    return local.wstring();
}

std::wstring GetV3ModuleDirectory() {
    std::wstring path(32768, L'\0');
    HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
    DWORD length = GetModuleFileNameW(module, path.data(),
                                     static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return GetExecutableDirectory();
    path.resize(length);
    return fs::path(path).parent_path().wstring();
}

std::wstring ResolveV3SoundFontPath(const EngineConfig& cfg,
                                    std::string* warning) {
    const fs::path searchDirectory = GetSoundFontSearchDirectory();
    if (!cfg.soundFontPath.empty()) {
        fs::path requested(cfg.soundFontPath);
        if (requested.is_relative()) requested = searchDirectory / requested;
        if (IsSoundFontFile(requested)) return requested.wstring();
        if (warning) {
            *warning = "configured SoundFont was not found: " +
                       WideToUtf8(cfg.soundFontPath);
        }
    }

    const auto localSoundFonts = DiscoverLocalSoundFonts();
    if (localSoundFonts.empty()) {
        if (warning) {
            if (!warning->empty()) *warning += "; ";
            *warning += "no .sf2 file found beside winmm.dll";
        }
        return {};
    }

    if (warning && (cfg.soundFontPath.empty() || localSoundFonts.size() > 1u)) {
        if (!warning->empty()) *warning += "; ";
        *warning += "using DLL-local SoundFont " +
                    WideToUtf8(localSoundFonts.front().filename().wstring());
        if (localSoundFonts.size() > 1u)
            *warning += " (multiple .sf2 files found; set synth.soundfont explicitly)";
    }
    return localSoundFonts.front().wstring();
}

} // namespace svms
