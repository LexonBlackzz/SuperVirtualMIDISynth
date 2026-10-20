#ifndef SVMS_CONFIGURATOR_SETTINGSREGISTRY_H
#define SVMS_CONFIGURATOR_SETTINGSREGISTRY_H

// One row per setting: the single source of truth for search, the ↻ restart
// marks, the save bar's unsaved counts and the sidebar badges.

#include <cstddef>

namespace svms::cfg {

struct ConfigValues;

enum class NavPage {
    Home,
    Sound,
    Effects,
    Output,
    Performance,
    Midi,
    Render,
    Record,
    Diagnostics,
    Settings,
    DriverFeatures,  // Linux: schema-driven page, listed under Tools
    Count
};

struct SettingDef {
    const char* id;          // anchor id (prototype IDX / state key)
    const char* name;        // search result title; nullptr = not searchable
    NavPage page;
    const char* where;       // "Page › Section" under the result
    const char* keywords;    // extra search words, incl. 0.8.1 names
    const char* fxPanel;     // Effects chain panel to select, or nullptr
    const char* disclosure;  // disclosure key to open, or nullptr
    // True when the configurator never sends this setting over RuntimeLink
    // (checked against the RLCommandType::Set* sends): it applies at the
    // next driver start.
    bool restart;
    // Working vs saved differ for this setting; nullptr = search-only entry.
    bool (*differs)(const ConfigValues& a, const ConfigValues& b);
};

const SettingDef* AllSettings(size_t* count);
const SettingDef* FindSetting(const char* id);

struct ChangeSummary {
    int total = 0;
    int restart = 0;
    int perPage[static_cast<int>(NavPage::Count)] = {};
};
ChangeSummary SummarizeChanges(const ConfigValues& working,
                               const ConfigValues& saved);

// Every word must appear in name + where + keywords (case-insensitive).
// Returns the number of hits written (at most `max`).
int SearchSettings(const char* query, const SettingDef** out, int max);

} // namespace svms::cfg

#endif
