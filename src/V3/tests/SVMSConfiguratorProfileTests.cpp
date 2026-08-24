#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../Configurator/ConfigDocument.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

bool WriteText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    return static_cast<bool>(output);
}

bool ReadJson(const fs::path& path, json& value) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        input >> value;
        return true;
    } catch (...) {
        return false;
    }
}

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

} // namespace

int main() {
    wchar_t tempDirectory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempDirectory)) {
        std::cerr << "FAIL: GetTempPathW\n";
        return 1;
    }
    const fs::path root = fs::path(tempDirectory) /
        (L"SVMS_ProfileTests_" + std::to_wstring(GetCurrentProcessId()));
    if (!CreateDirectoryW(root.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        std::cerr << "FAIL: CreateDirectoryW\n";
        return 1;
    }

    const fs::path active = root / L"config.json";
    const fs::path exported = root / L"profile_\x0442\x0435\x0441\x0442.json";
    const fs::path importedExport = root / L"imported.json";
    const fs::path revertedExport = root / L"reverted.json";
    const fs::path incoming = root / L"incoming.json";
    const fs::path malformed = root / L"malformed.json";
    const fs::path newer = root / L"newer.json";

    bool ok = true;
    ok &= Check(WriteText(active,
        R"json({"schema_version":1,"synth":{"max_voices":111},"active_unknown":{"keep":"active"}})json"),
        "write active config");

    svms::cfg::ConfigDocument document;
    ok &= Check(document.Load(active.wstring()), "load active config");
    ok &= Check(document.Working().maxVoices == 111u,
                "active known value loaded");
    const std::wstring activePath = document.GetActivePath();

    document.Working().maxVoices = 222u;
    document.Working().midiInputEnabled = true;
    document.Working().midiInputDevice = L"Unicode MIDI Ω";
    document.MarkDirty();
    std::string error;
    ok &= Check(document.ExportProfile(exported.wstring(), &error),
                "export working profile");
    ok &= Check(document.GetActivePath() == activePath,
                "export preserves active path");
    ok &= Check(document.IsDirty(), "export does not mark working copy saved");
    json exportedJson;
    ok &= Check(ReadJson(exported, exportedJson), "read exported profile");
    ok &= Check(exportedJson["synth"]["max_voices"] == 222u,
                "export contains working value");
    ok &= Check(exportedJson["midi"]["input_enabled"] == true &&
                    exportedJson["midi"]["input_device"] == "Unicode MIDI Ω",
                "export preserves physical MIDI input routing");
    ok &= Check(exportedJson["active_unknown"]["keep"] == "active",
                "export preserves unknown active fields");

    ok &= Check(WriteText(incoming,
        R"json({"schema_version":1,"synth":{"max_voices":333,"master_volume":0.4},"profile_unknown":{"keep":"profile"}})json"),
        "write incoming profile");
    ok &= Check(document.ImportProfile(incoming.wstring(), &error),
                "import valid profile");
    ok &= Check(document.Working().maxVoices == 333u,
                "import applies known value");
    ok &= Check(document.GetActivePath() == activePath,
                "import preserves active config path");
    ok &= Check(document.IsDirty(), "import marks working copy dirty");
    ok &= Check(document.ExportProfile(importedExport.wstring(), &error),
                "export imported working copy");
    json importedJson;
    ok &= Check(ReadJson(importedExport, importedJson),
                "read imported export");
    ok &= Check(importedJson["profile_unknown"]["keep"] == "profile",
                "profile unknown fields survive import/export");

    document.Revert();
    ok &= Check(document.Working().maxVoices == 111u,
                "revert restores loaded values");
    ok &= Check(!document.Working().midiInputEnabled &&
                    document.Working().midiInputDevice.empty(),
                "revert restores MIDI input routing");
    ok &= Check(!document.IsDirty(), "revert clears dirty state");
    ok &= Check(document.ExportProfile(revertedExport.wstring(), &error),
                "export reverted document");
    json revertedJson;
    ok &= Check(ReadJson(revertedExport, revertedJson), "read reverted export");
    ok &= Check(revertedJson.contains("active_unknown") &&
                !revertedJson.contains("profile_unknown"),
                "revert restores loaded unknown fields");

    const svms::cfg::ConfigValues beforeRejected = document.Working();
    ok &= Check(WriteText(malformed, "{ not json"), "write malformed profile");
    ok &= Check(!document.ImportProfile(malformed.wstring(), &error),
                "reject malformed profile");
    ok &= Check(svms::cfg::ConfigValuesEqual(
                    beforeRejected, document.Working()),
                "malformed rejection is non-destructive");
    ok &= Check(WriteText(newer,
        R"json({"schema_version":99,"synth":{"max_voices":999}})json"),
        "write newer profile");
    ok &= Check(!document.ImportProfile(newer.wstring(), &error),
                "reject newer profile schema");
    ok &= Check(svms::cfg::ConfigValuesEqual(
                    beforeRejected, document.Working()),
                "newer-schema rejection is non-destructive");

    ok &= Check(document.ImportProfile(incoming.wstring(), &error),
                "re-import profile before save");
    ok &= Check(document.Save(active.wstring()),
                "save imported profile to active config");
    json savedJson;
    ok &= Check(ReadJson(active, savedJson), "read saved active config");
    ok &= Check(savedJson["synth"]["max_voices"] == 333u &&
                savedJson["profile_unknown"]["keep"] == "profile",
                "save applies profile and preserves its unknown fields");

    DeleteFileW((active.wstring() + L".bak").c_str());
    DeleteFileW(active.c_str());
    DeleteFileW(exported.c_str());
    DeleteFileW(importedExport.c_str());
    DeleteFileW(revertedExport.c_str());
    DeleteFileW(incoming.c_str());
    DeleteFileW(malformed.c_str());
    DeleteFileW(newer.c_str());
    RemoveDirectoryW(root.c_str());

    if (!ok) return 1;
    std::cout << "Configurator profile tests passed\n";
    return 0;
}
