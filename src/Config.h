#ifndef CONFIG_H
#define CONFIG_H

#include "Compat.h"
#include <string>
#include <map>

class Config {
public:
    static Config& Instance();

    void Load(const std::string& filename);
    void Reload();
    void ForceReload();
    
    std::string GetString(const std::string& key, const std::string& defaultValue = "");
    int GetInt(const std::string& key, int defaultValue = 0);
    float GetFloat(const std::string& key, float defaultValue = 0.0f);
    bool GetBool(const std::string& key, bool defaultValue = false);
    bool SetString(const std::string& key, const std::string& value);
    bool SetInt(const std::string& key, int value);
    bool SetFloat(const std::string& key, float value);
    bool SetBool(const std::string& key, bool value);
    bool Save();

private:
    Config() {}
    bool SaveLocked();
    std::map<std::string, std::string> settings;
    std::string currentFilename;
    compat::Mutex configMutex;
    long long lastWriteTime = 0;
};

#endif // CONFIG_H
