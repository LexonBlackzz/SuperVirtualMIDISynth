#include "ConfigDocument.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace svms::cfg {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                    static_cast<int>(value.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    value.data(), static_cast<int>(value.size()),
                                    nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

constexpr uint32_t kConfigSchemaVersion = 1;
constexpr uint64_t kMaxProfileBytes = 16ull * 1024ull * 1024ull;

bool WriteJsonAtomic(const fs::path& path, const json& root,
                     bool preserveBackup, std::string* error) {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    std::error_code ec;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) return fail("could not create the destination directory");
    }

    if (preserveBackup && fs::exists(path, ec)) {
        fs::path backup = path.wstring() + L".bak";
        fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec);
        // A backup is best effort. Failure must not prevent the same atomic
        // replacement behavior that existing config saves already provide.
        ec.clear();
    }

    fs::path temp = path;
    temp += L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) return fail("could not create the temporary file");
        output << root.dump(2) << '\n';
        output.flush();
        if (!output) {
            output.close();
            DeleteFileW(temp.c_str());
            return fail("could not write the complete profile");
        }
    }

    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return fail("could not atomically replace the destination file");
    }
    if (error) error->clear();
    return true;
}

template <typename T>
void ReadNum(const json& obj, const char* key, T& dest, T min, T max) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return;
    try {
        T v = it->get<T>();
        if (v >= min && v <= max) dest = v;
    } catch (...) {}
}

void ReadBool(const json& obj, const char* key, bool& dest) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return;
    dest = it->get<bool>();
}

void ReadString(const json& obj, const char* key, std::wstring& dest) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return;
    dest = Utf8ToWide(it->get<std::string>());
}

void ReadOverflowMode(const json& obj, const char* key, int& dest) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return;
    std::string v = it->get<std::string>();
    if (v == "priority") dest = 0;
    else if (v == "lossless") dest = 1;
}

void ReadLimiterAlgorithm(const json& obj, const char* key, uint32_t& dest) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return;
    const std::string v = it->get<std::string>();
    if (v == "classic") dest = 0u;
    else if (v == "adaptive") dest = 1u;
}

} // namespace

ConfigValues ConfigDocument::Defaults() {
    ConfigValues d{};
    d.sampleRate = 44100;
    d.bufferFrames = 2048;
    d.maxVoices = 1000;
    d.voiceMemoryBudgetMB = 0;
    d.renderThreads = 0;
    d.masterVolume = 1.0f;
    d.velocityCurve = 1.0f;
    d.velocityFloor = 0.0f;
    d.velocityIgnoreBelow = 0;
    d.limiterEnabled = true;
    d.limiterAlgorithm = 0u;
    d.limiterThreshold = 0.95f;
    d.limiterLookaheadMs = 3.0f;
    d.limiterAttackMs = 0.5f;
    d.limiterReleaseMs = 100.0f;
    d.enableReverb = false;
    d.reverbMix = 0.25f;
    d.reverbRoomSize = 0.60f;
    d.reverbDecay = 0.50f;
    d.reverbDamping = 0.35f;
    d.reverbWidth = 1.0f;
    d.reverbDiffusion = 0.70f;
    d.reverbPreDelayMs = 12.0f;
    d.reverbEarlyLevel = 0.35f;
    d.reverbLateLevel = 0.85f;
    d.reverbModDepth = 0.30f;
    d.reverbModRate = 0.35f;
    d.reverbLowCutHz = 70.0f;
    d.reverbHighCutHz = 16000.0f;
    d.phaseRotationMode = 0u;
    d.eventRingCapacity = 393216;
    d.highPriorityVelocity = 96;
    d.shedStartPercent = 70;
    d.maxEventsPerBlock = 65536;
    d.overflowMode = 0;
    d.correctnessMode = true;
    d.diagnosticsEnabled = false;
    d.diagnosticsWindow = false;
    d.diagnosticsDebugOutput = false;
    d.midiInputEnabled = false;
    d.midiInputDevice.clear();
    d.audioDevice = L"default";
    d.soundFontPath.clear();
    d.soundFontPaths.clear();
    d.soundFontRoutes.clear();
    return d;
}

void ConfigDocument::FromJson(const json& root) {
    defaults_ = Defaults();

    if (auto it = root.find("audio"); it != root.end() && it->is_object()) {
        ReadString(*it, "device", working_.audioDevice);
        ReadNum(*it, "sample_rate", working_.sampleRate, 8000u, 384000u);
        ReadNum(*it, "buffer_frames", working_.bufferFrames, 16u, 8192u);
    }
    if (auto it = root.find("synth"); it != root.end() && it->is_object()) {
        ReadString(*it, "soundfont", working_.soundFontPath);
        if (auto stack = it->find("soundfonts");
            stack != it->end() && stack->is_array()) {
            working_.soundFontPaths.clear();
            for (const auto& item : *stack) {
                if (working_.soundFontPaths.size() >= 16u) break;
                if (item.is_string()) {
                    const std::wstring path = Utf8ToWide(item.get<std::string>());
                    if (!path.empty()) working_.soundFontPaths.push_back(path);
                }
            }
        }
        if (working_.soundFontPaths.empty() && !working_.soundFontPath.empty())
            working_.soundFontPaths.push_back(working_.soundFontPath);
        if (!working_.soundFontPaths.empty())
            working_.soundFontPath = working_.soundFontPaths.front();

        if (auto routes = it->find("soundfont_routes");
            routes != it->end() && routes->is_array()) {
            working_.soundFontRoutes.clear();
            for (const auto& item : *routes) {
                if (working_.soundFontRoutes.size() >= 256u ||
                    !item.is_object()) break;
                SoundFontRouteValue route{};
                route.soundFontIndex = item.value("soundfont", 0u);
                route.targetBank = item.value("bank", 0u);
                route.targetPreset = item.value("preset", -1);
                route.sourceBank = item.value("source_bank", 0u);
                route.sourcePreset = item.value("source_preset", -1);
                route.percussion = item.value("percussion", false);
                if (route.soundFontIndex < 16u && route.targetBank <= 127u &&
                    route.targetPreset >= -1 && route.targetPreset <= 127 &&
                    route.sourceBank <= 65535u && route.sourcePreset >= -1 &&
                    route.sourcePreset <= 127) {
                    working_.soundFontRoutes.push_back(route);
                }
            }
        }
        ReadNum(*it, "max_voices", working_.maxVoices, 1u, 524288u);
        ReadNum(*it, "voice_memory_budget_mb", working_.voiceMemoryBudgetMB,
                0u, 65536u);
        ReadNum(*it, "render_threads", working_.renderThreads, 0u, 64u);
        ReadNum(*it, "master_volume", working_.masterVolume, 0.0f, 4.0f);
        ReadNum(*it, "velocity_curve", working_.velocityCurve, 0.1f, 10.0f);
        ReadNum(*it, "velocity_floor", working_.velocityFloor, 0.0f, 0.99f);
        uint32_t vib = working_.velocityIgnoreBelow;
        ReadNum(*it, "velocity_ignore_below", vib, 0u, 127u);
        working_.velocityIgnoreBelow = vib;
    }
    if (auto it = root.find("events"); it != root.end() && it->is_object()) {
        ReadNum(*it, "ring_capacity", working_.eventRingCapacity, 4096u, UINT32_MAX);
        ReadNum(*it, "high_priority_velocity", working_.highPriorityVelocity, 1u, 127u);
        ReadNum(*it, "shed_start_percent", working_.shedStartPercent, 1u, 99u);
        ReadNum(*it, "max_events_per_block", working_.maxEventsPerBlock, 1u, UINT32_MAX);
        ReadOverflowMode(*it, "overflow_mode", working_.overflowMode);
    }
    if (auto it = root.find("quality"); it != root.end() && it->is_object()) {
        ReadBool(*it, "correctness_mode", working_.correctnessMode);
    }
    if (auto it = root.find("limiter"); it != root.end() && it->is_object()) {
        ReadBool(*it, "enabled", working_.limiterEnabled);
        ReadLimiterAlgorithm(*it, "algorithm", working_.limiterAlgorithm);
        ReadNum(*it, "threshold", working_.limiterThreshold, 0.1f, 1.0f);
        ReadNum(*it, "lookahead_ms", working_.limiterLookaheadMs, 0.0f, 20.0f);
        ReadNum(*it, "attack_ms", working_.limiterAttackMs, 0.01f, 100.0f);
        ReadNum(*it, "release_ms", working_.limiterReleaseMs, 1.0f, 5000.0f);
    }
    if (auto it = root.find("reverb"); it != root.end() && it->is_object()) {
        ReadBool(*it, "enabled", working_.enableReverb);
        ReadNum(*it, "mix", working_.reverbMix, 0.0f, 1.0f);
        ReadNum(*it, "room_size", working_.reverbRoomSize, 0.0f, 1.0f);
        ReadNum(*it, "decay", working_.reverbDecay, 0.0f, 1.0f);
        ReadNum(*it, "damping", working_.reverbDamping, 0.0f, 1.0f);
        ReadNum(*it, "width", working_.reverbWidth, 0.0f, 1.0f);
        ReadNum(*it, "diffusion", working_.reverbDiffusion, 0.0f, 1.0f);
        ReadNum(*it, "pre_delay_ms", working_.reverbPreDelayMs, 0.0f, 200.0f);
        ReadNum(*it, "early_level", working_.reverbEarlyLevel, 0.0f, 1.5f);
        ReadNum(*it, "late_level", working_.reverbLateLevel, 0.0f, 1.5f);
        ReadNum(*it, "mod_depth", working_.reverbModDepth, 0.0f, 1.0f);
        ReadNum(*it, "mod_rate", working_.reverbModRate, 0.0f, 1.0f);
        ReadNum(*it, "low_cut_hz", working_.reverbLowCutHz, 0.0f, 2000.0f);
        ReadNum(*it, "high_cut_hz", working_.reverbHighCutHz, 1000.0f, 20000.0f);
    }
    if (auto it = root.find("phase_rotation"); it != root.end() && it->is_object()) {
        ReadNum(*it, "mode", working_.phaseRotationMode, 0u, 3u);
    }
    if (auto it = root.find("diagnostics"); it != root.end() && it->is_object()) {
        ReadBool(*it, "enabled", working_.diagnosticsEnabled);
        ReadBool(*it, "window", working_.diagnosticsWindow);
        ReadBool(*it, "debug_output", working_.diagnosticsDebugOutput);
    }
    if (auto it = root.find("midi"); it != root.end() && it->is_object()) {
        ReadBool(*it, "input_enabled", working_.midiInputEnabled);
        ReadString(*it, "input_device", working_.midiInputDevice);
    }
}

nlohmann::json ConfigDocument::ToJson() const {
    json root = rawJson_;

    root["schema_version"] = kConfigSchemaVersion;

    root["audio"]["backend"] = "wasapi-shared";
    root["audio"]["device"] = WideToUtf8(working_.audioDevice);
    root["audio"]["sample_rate"] = working_.sampleRate;
    root["audio"]["buffer_frames"] = working_.bufferFrames;

    root["synth"]["soundfont"] = WideToUtf8(working_.soundFontPath);
    json soundFonts = json::array();
    if (!working_.soundFontPaths.empty()) {
        for (const std::wstring& path : working_.soundFontPaths)
            soundFonts.push_back(WideToUtf8(path));
    } else if (!working_.soundFontPath.empty()) {
        soundFonts.push_back(WideToUtf8(working_.soundFontPath));
    }
    root["synth"]["soundfonts"] = std::move(soundFonts);
    json routes = json::array();
    for (const SoundFontRouteValue& route : working_.soundFontRoutes) {
        routes.push_back({
            {"soundfont", route.soundFontIndex},
            {"bank", route.targetBank},
            {"preset", route.targetPreset},
            {"source_bank", route.sourceBank},
            {"source_preset", route.sourcePreset},
            {"percussion", route.percussion}
        });
    }
    root["synth"]["soundfont_routes"] = std::move(routes);
    root["synth"]["max_voices"] = working_.maxVoices;
    root["synth"]["voice_memory_budget_mb"] =
        working_.voiceMemoryBudgetMB;
    root["synth"]["render_threads"] = working_.renderThreads;
    root["synth"]["master_volume"] = working_.masterVolume;
    root["synth"]["velocity_curve"] = working_.velocityCurve;
    root["synth"]["velocity_floor"] = working_.velocityFloor;
    root["synth"]["velocity_ignore_below"] = working_.velocityIgnoreBelow;

    root["events"]["overflow_mode"] = working_.overflowMode == 0 ? "priority" : "lossless";
    root["events"]["ring_capacity"] = working_.eventRingCapacity;
    root["events"]["high_priority_velocity"] = working_.highPriorityVelocity;
    root["events"]["shed_start_percent"] = working_.shedStartPercent;
    root["events"]["max_events_per_block"] = working_.maxEventsPerBlock;

    root["quality"]["correctness_mode"] = working_.correctnessMode;
    root["quality"]["interpolation"] = "linear";
    root["quality"]["pan_law"] = "constant-power";

    root["limiter"]["enabled"] = working_.limiterEnabled;
    root["limiter"]["algorithm"] = working_.limiterAlgorithm == 1u ? "adaptive" : "classic";
    root["limiter"]["threshold"] = working_.limiterThreshold;
    root["limiter"]["lookahead_ms"] = working_.limiterLookaheadMs;
    root["limiter"]["attack_ms"] = working_.limiterAttackMs;
    root["limiter"]["release_ms"] = working_.limiterReleaseMs;

    root["reverb"]["enabled"] = working_.enableReverb;
    root["reverb"]["mix"] = working_.reverbMix;
    root["reverb"]["room_size"] = working_.reverbRoomSize;
    root["reverb"]["decay"] = working_.reverbDecay;
    root["reverb"]["damping"] = working_.reverbDamping;
    root["reverb"]["width"] = working_.reverbWidth;
    root["reverb"]["diffusion"] = working_.reverbDiffusion;
    root["reverb"]["pre_delay_ms"] = working_.reverbPreDelayMs;
    root["reverb"]["early_level"] = working_.reverbEarlyLevel;
    root["reverb"]["late_level"] = working_.reverbLateLevel;
    root["reverb"]["mod_depth"] = working_.reverbModDepth;
    root["reverb"]["mod_rate"] = working_.reverbModRate;
    root["reverb"]["low_cut_hz"] = working_.reverbLowCutHz;
    root["reverb"]["high_cut_hz"] = working_.reverbHighCutHz;


    root["phase_rotation"]["mode"] = working_.phaseRotationMode;

    root["midi"]["input_enabled"] = working_.midiInputEnabled;
    root["midi"]["input_device"] = WideToUtf8(working_.midiInputDevice);

    root["diagnostics"]["enabled"] = working_.diagnosticsEnabled;
    root["diagnostics"]["window"] = working_.diagnosticsWindow;
    root["diagnostics"]["debug_output"] = working_.diagnosticsDebugOutput;

    return root;
}

bool ConfigDocument::Load(const std::wstring& path) {
    working_ = Defaults();
    defaults_ = Defaults();
    rawJson_ = json::object();
    loadedRawJson_ = rawJson_;
    parseError_.clear();
    configWarning_.clear();
    dirty_ = false;
    readOnly_ = false;
    loadedSchemaVersion_ = 0u;
    activePath_ = path;

    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            configWarning_ = "unable to open config file";
            loaded_ = working_;
            return true;
        }
        input >> rawJson_;
        uint32_t version = rawJson_.value("schema_version", 0u);
        loadedSchemaVersion_ = version;
        if (version == kConfigSchemaVersion) {
            FromJson(rawJson_);
        } else if (version > kConfigSchemaVersion) {
            configWarning_ = "config schema is newer than this build";
            readOnly_ = true;
            FromJson(rawJson_);
        } else {
            configWarning_ = "unsupported or missing config schema version";
            FromJson(rawJson_);
        }
    } catch (const std::exception& e) {
        parseError_ = std::string("malformed config.json: ") + e.what();
        readOnly_ = true;
        working_ = Defaults();
    }

    loaded_ = working_;
    loadedRawJson_ = rawJson_;
    return true;
}

bool ConfigDocument::LoadDefaults() {
    const bool preserveReadOnly = readOnly_ && !activePath_.empty();
    const uint32_t previousSchemaVersion = loadedSchemaVersion_;
    working_ = Defaults();
    loaded_ = working_;
    rawJson_ = json::object();
    loadedRawJson_ = rawJson_;
    parseError_.clear();
    configWarning_.clear();
    dirty_ = false;
    readOnly_ = preserveReadOnly;
    loadedSchemaVersion_ = preserveReadOnly ? previousSchemaVersion : 0u;
    return true;
}

bool ConfigDocument::Save(const std::wstring& path) {
    if (readOnly_) return false;
    ConfigValidation v = Validate();
    if (!v.valid) return false;

    return SaveAtomic(path);
}

bool ConfigDocument::SaveAtomic(const std::wstring& path) {
    if (readOnly_) return false;
    json root = ToJson();
    if (!WriteJsonAtomic(fs::path(path), root, true, nullptr)) return false;

    loaded_ = working_;
    rawJson_ = root;
    loadedRawJson_ = root;
    dirty_ = false;
    return true;
}

bool ConfigDocument::ImportProfile(const std::wstring& path,
                                   std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (readOnly_)
        return fail("the active configuration is read-only");

    try {
        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        if (ec) return fail("could not open the profile");
        if (size == 0u) return fail("the profile is empty");
        if (size > kMaxProfileBytes)
            return fail("the profile exceeds the 16 MiB safety limit");

        std::ifstream input(path, std::ios::binary);
        if (!input) return fail("could not open the profile");
        json profile;
        input >> profile;
        if (!profile.is_object())
            return fail("the profile root must be a JSON object");
        const auto schema = profile.find("schema_version");
        if (schema == profile.end() || !schema->is_number_unsigned())
            return fail("the profile has no valid schema_version");
        const uint32_t version = schema->get<uint32_t>();
        if (version > kConfigSchemaVersion)
            return fail("the profile uses a newer unsupported schema");
        if (version != kConfigSchemaVersion)
            return fail("the profile schema is unsupported");

        ConfigDocument candidate;
        candidate.working_ = Defaults();
        candidate.defaults_ = candidate.working_;
        candidate.rawJson_ = profile;
        candidate.FromJson(profile);
        const ConfigValidation validation = candidate.Validate();
        if (!validation.valid)
            return fail("the profile contains invalid settings: " +
                        validation.warnings);

        working_ = candidate.working_;
        rawJson_ = std::move(profile);
        dirty_ = true;
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        return fail(std::string("malformed profile: ") + exception.what());
    }
}

bool ConfigDocument::ExportProfile(const std::wstring& path,
                                   std::string* error) const {
    if (readOnly_) {
        if (error) *error = "the active configuration is read-only";
        return false;
    }
    const ConfigValidation validation = Validate();
    if (!validation.valid) {
        if (error)
            *error = "current settings are invalid: " + validation.warnings;
        return false;
    }
    return WriteJsonAtomic(fs::path(path), ToJson(), false, error);
}

bool ConfigValuesEqual(const ConfigValues& a, const ConfigValues& b) {
    return a.sampleRate == b.sampleRate
        && a.bufferFrames == b.bufferFrames
        && a.maxVoices == b.maxVoices
        && a.voiceMemoryBudgetMB == b.voiceMemoryBudgetMB
        && a.renderThreads == b.renderThreads
        && AlmostEquals(a.masterVolume, b.masterVolume)
        && AlmostEquals(a.velocityCurve, b.velocityCurve)
        && AlmostEquals(a.velocityFloor, b.velocityFloor)
        && a.velocityIgnoreBelow == b.velocityIgnoreBelow
        && a.limiterEnabled == b.limiterEnabled
        && a.limiterAlgorithm == b.limiterAlgorithm
        && AlmostEquals(a.limiterThreshold, b.limiterThreshold)
        && AlmostEquals(a.limiterLookaheadMs, b.limiterLookaheadMs)
        && AlmostEquals(a.limiterAttackMs, b.limiterAttackMs)
        && AlmostEquals(a.limiterReleaseMs, b.limiterReleaseMs)
        && a.enableReverb == b.enableReverb
        && AlmostEquals(a.reverbMix, b.reverbMix)
        && AlmostEquals(a.reverbRoomSize, b.reverbRoomSize)
        && AlmostEquals(a.reverbDecay, b.reverbDecay)
        && AlmostEquals(a.reverbDamping, b.reverbDamping)
        && AlmostEquals(a.reverbWidth, b.reverbWidth)
        && AlmostEquals(a.reverbDiffusion, b.reverbDiffusion)
        && AlmostEquals(a.reverbPreDelayMs, b.reverbPreDelayMs)
        && AlmostEquals(a.reverbEarlyLevel, b.reverbEarlyLevel)
        && AlmostEquals(a.reverbLateLevel, b.reverbLateLevel)
        && AlmostEquals(a.reverbModDepth, b.reverbModDepth)
        && AlmostEquals(a.reverbModRate, b.reverbModRate)
        && AlmostEquals(a.reverbLowCutHz, b.reverbLowCutHz)
        && AlmostEquals(a.reverbHighCutHz, b.reverbHighCutHz)
        && a.phaseRotationMode == b.phaseRotationMode
        && a.eventRingCapacity == b.eventRingCapacity
        && a.highPriorityVelocity == b.highPriorityVelocity
        && a.shedStartPercent == b.shedStartPercent
        && a.maxEventsPerBlock == b.maxEventsPerBlock
        && a.overflowMode == b.overflowMode
        && a.correctnessMode == b.correctnessMode
        && a.diagnosticsEnabled == b.diagnosticsEnabled
        && a.diagnosticsWindow == b.diagnosticsWindow
        && a.diagnosticsDebugOutput == b.diagnosticsDebugOutput
        && a.midiInputEnabled == b.midiInputEnabled
        && a.midiInputDevice == b.midiInputDevice
        && a.audioDevice == b.audioDevice
        && a.soundFontPath == b.soundFontPath
        && a.soundFontPaths == b.soundFontPaths
        && a.soundFontRoutes == b.soundFontRoutes;
}

bool ConfigDocument::IsDirty() const {
    return dirty_ || !ConfigValuesEqual(working_, loaded_);
}

void ConfigDocument::MarkDirty() {
    dirty_ = true;
}

void ConfigDocument::ClearDirty() {
    dirty_ = false;
}

void ConfigDocument::Revert() {
    working_ = loaded_;
    rawJson_ = loadedRawJson_;
    dirty_ = false;
}

ConfigValidation ConfigDocument::Validate() const {
    ConfigValidation v;
    v.valid = true;

    auto warn = [&](const char* field, const char* msg) {
        v.valid = false;
        if (!v.warnings.empty()) v.warnings += "; ";
        v.warnings += std::string(field) + ": " + msg;
    };

    if (working_.sampleRate < 8000 || working_.sampleRate > 384000)
        warn("audio.sample_rate", "must be 8000..384000");
    if (working_.bufferFrames < 16 || working_.bufferFrames > 8192)
        warn("audio.buffer_frames", "must be 16..8192");
    if (working_.maxVoices < 1 || working_.maxVoices > 524288)
        warn("synth.max_voices", "must be 1..524288");
    if (working_.voiceMemoryBudgetMB > 65536)
        warn("synth.voice_memory_budget_mb", "must be 0..65536");
    if (working_.renderThreads > 64)
        warn("synth.render_threads", "must be 0..64");
    if (working_.masterVolume < 0.0f || working_.masterVolume > 4.0f)
        warn("synth.master_volume", "must be 0..4");
    if (working_.velocityCurve < 0.1f || working_.velocityCurve > 10.0f)
        warn("synth.velocity_curve", "must be 0.1..10");
    if (working_.velocityFloor < 0.0f || working_.velocityFloor >= 1.0f)
        warn("synth.velocity_floor", "must be 0..0.99");
    if (working_.velocityIgnoreBelow > 127)
        warn("synth.velocity_ignore_below", "must be 0..127");
    if (working_.limiterAlgorithm > 1u)
        warn("limiter.algorithm", "must be classic or adaptive");
    if (working_.limiterThreshold < 0.1f || working_.limiterThreshold > 1.0f)
        warn("limiter.threshold", "must be 0.1..1");
    if (working_.limiterLookaheadMs < 0.0f || working_.limiterLookaheadMs > 20.0f)
        warn("limiter.lookahead_ms", "must be 0..20");
    if (working_.limiterAttackMs < 0.01f || working_.limiterAttackMs > 100.0f)
        warn("limiter.attack_ms", "must be 0.01..100");
    if (working_.limiterReleaseMs < 1.0f || working_.limiterReleaseMs > 5000.0f)
        warn("limiter.release_ms", "must be 1..5000");
    if (working_.reverbMix < 0.0f || working_.reverbMix > 1.0f)
        warn("reverb.mix", "must be 0..1");
    if (working_.reverbRoomSize < 0.0f || working_.reverbRoomSize > 1.0f)
        warn("reverb.room_size", "must be 0..1");
    if (working_.reverbDecay < 0.0f || working_.reverbDecay > 1.0f)
        warn("reverb.decay", "must be 0..1");
    if (working_.reverbDamping < 0.0f || working_.reverbDamping > 1.0f)
        warn("reverb.damping", "must be 0..1");
    if (working_.reverbWidth < 0.0f || working_.reverbWidth > 1.0f)
        warn("reverb.width", "must be 0..1");
    if (working_.reverbDiffusion < 0.0f || working_.reverbDiffusion > 1.0f)
        warn("reverb.diffusion", "must be 0..1");
    if (working_.reverbPreDelayMs < 0.0f || working_.reverbPreDelayMs > 200.0f)
        warn("reverb.pre_delay_ms", "must be 0..200");
    if (working_.reverbEarlyLevel < 0.0f || working_.reverbEarlyLevel > 1.5f)
        warn("reverb.early_level", "must be 0..1.5");
    if (working_.reverbLateLevel < 0.0f || working_.reverbLateLevel > 1.5f)
        warn("reverb.late_level", "must be 0..1.5");
    if (working_.reverbModDepth < 0.0f || working_.reverbModDepth > 1.0f)
        warn("reverb.mod_depth", "must be 0..1");
    if (working_.reverbModRate < 0.0f || working_.reverbModRate > 1.0f)
        warn("reverb.mod_rate", "must be 0..1");
    if (working_.reverbLowCutHz < 0.0f || working_.reverbLowCutHz > 2000.0f)
        warn("reverb.low_cut_hz", "must be 0..2000");
    if (working_.reverbHighCutHz < 1000.0f || working_.reverbHighCutHz > 20000.0f)
        warn("reverb.high_cut_hz", "must be 1000..20000");
    if (working_.phaseRotationMode > 4u)
        warn("phase_rotation.mode", "must be 0..4");
    if (working_.eventRingCapacity < 4096)
        warn("events.ring_capacity", "must be >= 4096");
    if (working_.highPriorityVelocity < 1 || working_.highPriorityVelocity > 127)
        warn("events.high_priority_velocity", "must be 1..127");
    if (working_.shedStartPercent < 1 || working_.shedStartPercent >= 100)
        warn("events.shed_start_percent", "must be 1..99");
    if (working_.maxEventsPerBlock == 0)
        warn("events.max_events_per_block", "must be > 0");

    return v;
}

void ConfigValidation::AddWarning(const char* field) {
    valid = false;
    if (!warnings.empty()) warnings += "; ";
    warnings += std::string("invalid: ") + field;
}

} // namespace svms::cfg
