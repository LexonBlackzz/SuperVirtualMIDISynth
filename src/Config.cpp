#include "Config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <stdio.h>
#include <windows.h>
#include <sys/stat.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

Config& Config::Instance() {
    static Config instance;
    return instance;
}

static void EmitAttributionLog() {
    static bool emitted = false;
    if (!emitted) {
        puts("SuperVirtualMIDISynth - Copyright LexonBlackzz");
        emitted = true;
    }
}

// Helper to trim whitespace
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) {
        return str;
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

struct DefaultSetting {
    const char* key;
    const char* value;
};

static const DefaultSetting kDefaultSettings[] = {
    { "max_voices", "500" },
    { "sample_rate", "44100" },
    { "master_volume", "1.0" },
    { "velocity_curve", "2.4" },
    { "velocity_floor", "0.0" },
    { "velocity_ignore_below", "0" },
    { "async_note_starts", "true" },
    { "event_timing_mode", "accurate" },
    { "wasapi_async_feed", "true" },
    { "reverb_enable", "false" },
    { "reverb_mix", "0.18" },
    { "reverb_feedback", "0.72" },
    { "reverb_tone", "0.28" },
    { "reverb_width", "0.35" },
    { "reverb_blur", "0.45" },
    { "limiter_enable", "true" },
    { "limiter_threshold", "0.98" },
    { "limiter_release_ms", "80" },
    { "polling_rate", "0" },
    { "audio_backend", "auto" },
    { "sampler_engine", "auto" },
    { "sound_source", "gm.sf2" },
    { "soundfont", "gm.sf2" }
};

static void WriteDefaultConfig(std::ofstream& out) {
    for (const DefaultSetting& setting : kDefaultSettings) {
        out << setting.key << "=" << setting.value << "\n";
    }
    out << "# Add sample mappings later\n";
    out << "# 60=C:\\Path\\To\\Sample.wav\n";
}

static bool AppendMissingDefaults(const std::string& filename,
                                  std::map<std::string, std::string>& settings) {
    std::ofstream out(filename, std::ios::app);
    if (!out.is_open()) {
        return false;
    }

    bool wroteHeader = false;
    bool appendedAny = false;
    for (const DefaultSetting& setting : kDefaultSettings) {
        if (settings.find(setting.key) != settings.end()) {
            continue;
        }
        if (!wroteHeader) {
            out << "\n# Added missing defaults\n";
            wroteHeader = true;
        }
        out << setting.key << "=" << setting.value << "\n";
        settings[setting.key] = setting.value;
        appendedAny = true;
    }
    return appendedAny;
}

void Config::Load(const std::string& filename) {
    compat::LockGuard<compat::Mutex> lock(configMutex);
    EmitAttributionLog();
    
    std::string pathToTry = filename;
    struct stat result;
    if (stat(pathToTry.c_str(), &result) != 0) {
        // Not in CWD, try DLL directory
        char dllPath[MAX_PATH];
        HMODULE hModule = reinterpret_cast<HMODULE>(&__ImageBase);
        
        if (hModule && GetModuleFileNameA(hModule, dllPath, MAX_PATH)) {
            char* lastSlash = strrchr(dllPath, '\\');
            if (lastSlash) {
                *(lastSlash + 1) = '\0';
                pathToTry = std::string(dllPath) + filename;
            }
        }
    }
    currentFilename = pathToTry;

    if (stat(currentFilename.c_str(), &result) == 0) {
        lastWriteTime = result.st_mtime;
    } else {
        // File does not exist, create default
        printf("Config file not found, creating default at: %s\n", currentFilename.c_str());
        std::ofstream out(currentFilename);
        if (out.is_open()) {
            WriteDefaultConfig(out);
            out.close();
            
            // Update stat to get the new mtime
            if (stat(currentFilename.c_str(), &result) == 0) {
                lastWriteTime = result.st_mtime;
            }
        } else {
            fputs("Failed to create default config file!\n", stderr);
            return;
        }
    }

    settings.clear();
    std::ifstream file(currentFilename);
    if (!file.is_open()) {
        fprintf(stderr, "Failed to open config file: %s\n", currentFilename.c_str());
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Remove comments
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }

        line = trim(line);
        if (line.empty()) continue;

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string key = trim(line.substr(0, eqPos));
            std::string value = trim(line.substr(eqPos + 1));
            settings[key] = value;
        }
    }

    if (AppendMissingDefaults(currentFilename, settings) &&
        stat(currentFilename.c_str(), &result) == 0) {
        lastWriteTime = result.st_mtime;
    }
    printf("Config loaded from %s\n", currentFilename.c_str());
}

void Config::Reload() {
    if (currentFilename.empty()) return;

    struct stat result;
    if (stat(currentFilename.c_str(), &result) == 0) {
        if (result.st_mtime != lastWriteTime) {
             puts("Config changed, reloading...");
             Load(currentFilename);
        }
    }
}

void Config::ForceReload() {
    std::string filename;
    {
        compat::LockGuard<compat::Mutex> lock(configMutex);
        if (currentFilename.empty()) {
            return;
        }
        filename = currentFilename;
    }
    Load(filename);
}

std::string Config::GetString(const std::string& key, const std::string& defaultValue) {
    compat::LockGuard<compat::Mutex> lock(configMutex);
    auto it = settings.find(key);
    if (it != settings.end()) {
        return it->second;
    }
    return defaultValue;
}

int Config::GetInt(const std::string& key, int defaultValue) {
    std::string val = GetString(key, "");
    if (val.empty()) return defaultValue;
    try {
        return std::stoi(val);
    } catch (...) {
        return defaultValue;
    }
}

float Config::GetFloat(const std::string& key, float defaultValue) {
    std::string val = GetString(key, "");
    if (val.empty()) return defaultValue;
    try {
        return std::stof(val);
    } catch (...) {
        return defaultValue;
    }
}

bool Config::GetBool(const std::string& key, bool defaultValue) {
    std::string val = GetString(key, "");
    if (val.empty()) return defaultValue;
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return (val == "true" || val == "1" || val == "yes");
}

bool Config::SetString(const std::string& key, const std::string& value) {
    compat::LockGuard<compat::Mutex> lock(configMutex);
    settings[key] = value;
    return true;
}

bool Config::SetInt(const std::string& key, int value) {
    std::ostringstream out;
    out << value;
    return SetString(key, out.str());
}

bool Config::SetFloat(const std::string& key, float value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << value;
    std::string text = out.str();
    while (text.size() > 2 && text.find('.') != std::string::npos &&
           text[text.size() - 1] == '0') {
        text.erase(text.size() - 1);
    }
    if (!text.empty() && text[text.size() - 1] == '.') {
        text.push_back('0');
    }
    return SetString(key, text);
}

bool Config::SetBool(const std::string& key, bool value) {
    return SetString(key, value ? "true" : "false");
}

bool Config::Save() {
    compat::LockGuard<compat::Mutex> lock(configMutex);
    return SaveLocked();
}

bool Config::SaveLocked() {
    if (currentFilename.empty()) {
        return false;
    }

    std::ofstream out(currentFilename, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    for (std::map<std::string, std::string>::const_iterator it = settings.begin();
         it != settings.end(); ++it) {
        out << it->first << "=" << it->second << "\n";
    }
    out.flush();
    out.close();

    struct stat result;
    if (stat(currentFilename.c_str(), &result) == 0) {
        lastWriteTime = result.st_mtime;
    }
    return true;
}
