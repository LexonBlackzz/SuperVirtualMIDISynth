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
    d.renderThreads = 1;
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
    d.eventRingCapacity = 393216;
    d.highPriorityVelocity = 96;
    d.shedStartPercent = 70;
    d.maxEventsPerBlock = 65536;
    d.overflowMode = 0;
    d.correctnessMode = true;
    d.diagnosticsEnabled = false;
    d.diagnosticsWindow = false;
    d.diagnosticsDebugOutput = false;
    d.audioDevice = L"default";
    d.soundFontPath.clear();
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
        ReadNum(*it, "max_voices", working_.maxVoices, 1u, 524288u);
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
    if (auto it = root.find("diagnostics"); it != root.end() && it->is_object()) {
        ReadBool(*it, "enabled", working_.diagnosticsEnabled);
        ReadBool(*it, "window", working_.diagnosticsWindow);
        ReadBool(*it, "debug_output", working_.diagnosticsDebugOutput);
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
    root["synth"]["max_voices"] = working_.maxVoices;
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

    root["diagnostics"]["enabled"] = working_.diagnosticsEnabled;
    root["diagnostics"]["window"] = working_.diagnosticsWindow;
    root["diagnostics"]["debug_output"] = working_.diagnosticsDebugOutput;

    return root;
}

bool ConfigDocument::Load(const std::wstring& path) {
    working_ = Defaults();
    defaults_ = Defaults();
    rawJson_ = json::object();
    parseError_.clear();
    configWarning_.clear();
    dirty_ = false;
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
        if (version == kConfigSchemaVersion) {
            FromJson(rawJson_);
        } else if (version > kConfigSchemaVersion) {
            configWarning_ = "config schema is newer than this build";
            FromJson(rawJson_);
        } else {
            configWarning_ = "unsupported or missing config schema version";
            FromJson(rawJson_);
        }
    } catch (const std::exception& e) {
        parseError_ = std::string("malformed config.json: ") + e.what();
        working_ = Defaults();
    }

    loaded_ = working_;
    return true;
}

bool ConfigDocument::LoadDefaults() {
    working_ = Defaults();
    loaded_ = working_;
    rawJson_ = json::object();
    parseError_.clear();
    configWarning_.clear();
    dirty_ = false;
    return true;
}

bool ConfigDocument::Save(const std::wstring& path) {
    ConfigValidation v = Validate();
    if (!v.valid) return false;

    return SaveAtomic(path);
}

bool ConfigDocument::SaveAtomic(const std::wstring& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    fs::path backup = fs::path(path).wstring() + L".bak";
    if (fs::exists(path, ec)) {
        fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec);
    }

    fs::path temp = path;
    temp += L".tmp." + std::to_wstring(GetCurrentProcessId());

    json root = ToJson();
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << root.dump(2) << '\n';
        output.flush();
        if (!output) return false;
    }

    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }

    loaded_ = working_;
    rawJson_ = root;
    dirty_ = false;
    return true;
}

bool ConfigValuesEqual(const ConfigValues& a, const ConfigValues& b) {
    return a.sampleRate == b.sampleRate
        && a.bufferFrames == b.bufferFrames
        && a.maxVoices == b.maxVoices
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
        && a.eventRingCapacity == b.eventRingCapacity
        && a.highPriorityVelocity == b.highPriorityVelocity
        && a.shedStartPercent == b.shedStartPercent
        && a.maxEventsPerBlock == b.maxEventsPerBlock
        && a.overflowMode == b.overflowMode
        && a.correctnessMode == b.correctnessMode
        && a.diagnosticsEnabled == b.diagnosticsEnabled
        && a.diagnosticsWindow == b.diagnosticsWindow
        && a.diagnosticsDebugOutput == b.diagnosticsDebugOutput
        && a.audioDevice == b.audioDevice
        && a.soundFontPath == b.soundFontPath;
}

bool ConfigDocument::IsDirty() const {
    return !ConfigValuesEqual(working_, loaded_);
}

void ConfigDocument::MarkDirty() {
    dirty_ = true;
}

void ConfigDocument::ClearDirty() {
    dirty_ = false;
}

void ConfigDocument::Revert() {
    working_ = loaded_;
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
    if (working_.eventRingCapacity < 4096)
        warn("events.ring_capacity", "must be >= 4096");
    if (working_.highPriorityVelocity < 1 || working_.highPriorityVelocity > 127)
        warn("events.high_priority_velocity", "must be 1..127");
    if (working_.shedStartPercent < 1 || working_.shedStartPercent >= 100)
        warn("events.shed_start_percent", "must be 1..99");
    if (working_.maxEventsPerBlock == 0)
        warn("events.max_events_per_block", "must be > 0");
    if (working_.maxEventsPerBlock > working_.eventRingCapacity)
        warn("events.max_events_per_block", "exceeds ring_capacity");

    return v;
}

void ConfigValidation::AddWarning(const char* field) {
    valid = false;
    if (!warnings.empty()) warnings += "; ";
    warnings += std::string("invalid: ") + field;
}

} // namespace svms::cfg
