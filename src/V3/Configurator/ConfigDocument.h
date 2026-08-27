#ifndef SVMS_CONFIGURATOR_CONFIGDOCUMENT_H
#define SVMS_CONFIGURATOR_CONFIGDOCUMENT_H

#include <string>
#include <vector>
#include <cmath>
#include <nlohmann/json.hpp>

namespace svms::cfg {

struct SoundFontRouteValue {
    uint32_t soundFontIndex = 0u;
    uint32_t targetBank = 0u;
    int32_t targetPreset = -1;
    uint32_t sourceBank = 0u;
    int32_t sourcePreset = -1;
    bool percussion = false;

    bool operator==(const SoundFontRouteValue& other) const {
        return soundFontIndex == other.soundFontIndex &&
               targetBank == other.targetBank &&
               targetPreset == other.targetPreset &&
               sourceBank == other.sourceBank &&
               sourcePreset == other.sourcePreset &&
               percussion == other.percussion;
    }
};

struct ConfigValues {
    uint32_t sampleRate = 44100;
    uint32_t bufferFrames = 2048;
    uint32_t maxVoices = 1024;
    uint32_t voiceMemoryBudgetMB = 0;
    uint32_t renderThreads = 0;
    float masterVolume = 1.0f;
    float velocityCurve = 1.0f;
    float velocityFloor = 0.0f;
    uint32_t velocityIgnoreBelow = 0;

    bool limiterEnabled = true;
    uint32_t limiterAlgorithm = 0; // 0 = Classic, 1 = Adaptive
    float limiterThreshold = 0.95f;
    float limiterLookaheadMs = 3.0f;
    float limiterAttackMs = 0.5f;
    float limiterReleaseMs = 100.0f;

    bool enableReverb = false;
    float reverbMix = 0.25f;
    float reverbRoomSize = 0.60f;
    float reverbDecay = 0.50f;
    float reverbDamping = 0.35f;
    float reverbWidth = 1.0f;
    float reverbDiffusion = 0.70f;
    float reverbPreDelayMs = 12.0f;
    float reverbEarlyLevel = 0.35f;
    float reverbLateLevel = 0.85f;
    float reverbModDepth = 0.30f;
    float reverbModRate = 0.35f;
    float reverbLowCutHz = 70.0f;
    float reverbHighCutHz = 16000.0f;
    // 0 = Coherent (off), 1 = Analytic, 2 = Sweep, 3 = Diffuse
    uint32_t phaseRotationMode = 0u;

    uint32_t eventRingCapacity = 393216;
    uint32_t highPriorityVelocity = 96;
    uint32_t shedStartPercent = 70;
    uint32_t maxEventsPerBlock = 65536;
    int overflowMode = 0;
    bool correctnessMode = true;

    bool diagnosticsEnabled = false;
    bool diagnosticsWindow = false;
    bool diagnosticsDebugOutput = false;

    bool midiInputEnabled = false;
    std::wstring midiInputDevice;

    std::wstring audioDevice = L"default";
    std::wstring soundFontPath;
    std::vector<std::wstring> soundFontPaths;
    std::vector<SoundFontRouteValue> soundFontRoutes;
};

struct ConfigValidation {
    bool valid = true;
    std::string warnings;

    void AddWarning(const char* field);
};

inline bool AlmostEquals(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

bool ConfigValuesEqual(const ConfigValues& a, const ConfigValues& b);

class ConfigDocument {
public:
    ConfigDocument() = default;

    bool Load(const std::wstring& path);
    bool LoadDefaults();
    bool Save(const std::wstring& path);
    bool SaveAtomic(const std::wstring& path);
    bool ImportProfile(const std::wstring& path, std::string* error = nullptr);
    bool ExportProfile(const std::wstring& path, std::string* error = nullptr) const;

    bool IsDirty() const;
    void MarkDirty();
    void ClearDirty();
    void Revert();

    bool HasParseError() const { return !parseError_.empty(); }
    const std::string& ParseError() const { return parseError_; }
    const std::string& ConfigWarning() const { return configWarning_; }
    bool IsReadOnly() const { return readOnly_; }
    uint32_t LoadedSchemaVersion() const { return loadedSchemaVersion_; }

    std::wstring GetActivePath() const { return activePath_; }
    void SetActivePath(const std::wstring& path) { activePath_ = path; }

    ConfigValues& Working() { return working_; }
    const ConfigValues& Working() const { return working_; }
    const ConfigValues& Loaded() const { return loaded_; }

    static ConfigValues Defaults();

    ConfigValidation Validate() const;

private:
    void FromJson(const nlohmann::json& root);
    nlohmann::json ToJson() const;

    ConfigValues defaults_;
    ConfigValues loaded_;
    ConfigValues working_;
    nlohmann::json rawJson_;
    nlohmann::json loadedRawJson_;
    std::wstring activePath_;
    std::string parseError_;
    std::string configWarning_;
    bool dirty_ = false;
    bool readOnly_ = false;
    uint32_t loadedSchemaVersion_ = 0u;
};

} // namespace svms::cfg

#endif
