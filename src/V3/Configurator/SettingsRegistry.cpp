#include "SettingsRegistry.h"
#include "ConfigDocument.h"

#include <cctype>
#include <cstring>
#include <string>

namespace svms::cfg {
namespace {

using V = ConfigValues;

template <class T> bool Same(const T& a, const T& b) { return a == b; }
bool Same(float a, float b) { return AlmostEquals(a, b); }

template <class... M>
bool Diff(const V& a, const V& b, M... members) {
    return (!Same(a.*members, b.*members) || ...);
}

#define D(...) [](const V& a, const V& b) { return Diff(a, b, __VA_ARGS__); }

constexpr NavPage H = NavPage::Home, S = NavPage::Sound, E = NavPage::Effects,
                  O = NavPage::Output, P = NavPage::Performance, M = NavPage::Midi,
                  R = NavPage::Render, RC = NavPage::Record, G = NavPage::Diagnostics,
                  T = NavPage::Settings;

// Searchable rows are the prototype's IDX table (names, places and keywords
// verbatim). Rows without a name cover the remaining settings so every
// ConfigValues field counts toward the save bar and the badges.
// restart: verified against the RLCommandType::Set* sends in the pages.
const SettingDef kSettings[] = {
    // id, name, page, where, keywords, fxPanel, disclosure, restart, differs
    {"sf", "SoundFont", S, "Sound", "sf2 sfz bank instrument soundfont load choose priority stack overview", nullptr, nullptr, false, D(&V::soundFontPath, &V::soundFontPaths)},
    {"velCurve", "Velocity curve", S, "Sound \xE2\x80\xBA Velocity", "velocity loud soft dynamics", nullptr, nullptr, true, D(&V::velocityCurve)},
    {"velFloor", "Velocity floor", S, "Sound \xE2\x80\xBA Velocity", "velocity minimum quiet", nullptr, nullptr, true, D(&V::velocityFloor)},
    {"velIgnore", "Ignore notes softer than", S, "Sound \xE2\x80\xBA Velocity", "ignore velocity below threshold drop ghost notes", nullptr, nullptr, true, D(&V::velocityIgnoreBelow)},
    {"phase", "Voice character (phase rotation)", S, "Sound", "phase rotation coherent hum hilbert analytic sweep diffuse random", nullptr, nullptr, false, D(&V::phaseRotationMode)},
    {"tuning", "Tuning (12 / 31 EDO)", S, "Sound", "edo microtonal 31", nullptr, nullptr, true, D(&V::tuningEdo)},
    {"routes", "Bank and preset routes", S, "Sound \xE2\x80\xBA Routes", "bank preset route mapping", nullptr, "routes", true, D(&V::soundFontRoutes)},
    {"revOn", "Reverb", E, "Effects \xE2\x80\xBA Reverb", "reverb room hall space wet", "reverb", nullptr, false, D(&V::enableReverb)},
    {"revMix", "Reverb mix", E, "Effects \xE2\x80\xBA Reverb", "wet dry amount", "reverb", nullptr, false, D(&V::reverbMix)},
    {"revDecay", "Reverb decay", E, "Effects \xE2\x80\xBA Reverb", "rt60 length tail", "reverb", nullptr, false, D(&V::reverbDecay)},
    {"revDamp", "Reverb damping / tone", E, "Effects \xE2\x80\xBA Reverb \xE2\x80\xBA Fine-tune", "damping low cut high cut tone eq", "reverb", "rev", false, D(&V::reverbDamping)},
    {"limOn", "Master limiter", E, "Effects \xE2\x80\xBA Master limiter", "limiter clip clipping loudness compressor threshold", "limiter", nullptr, false, D(&V::limiterEnabled)},
    {"limMode", "Limiter mode (Classic / Adaptive)", E, "Effects \xE2\x80\xBA Master limiter", "adaptive classic lookahead", "limiter", nullptr, false, D(&V::limiterAlgorithm)},
    {"chOn", "Channel limiter", E, "Effects \xE2\x80\xBA Channel limiter", "per channel limiter 16 channels", "chlim", nullptr, false, D(&V::channelLimiterEnabled)},
    {"device", "Audio device", O, "Output", "output device speakers headphones interface refresh devices", nullptr, nullptr, true, D(&V::audioDevice, &V::linuxDevice)},
    {"audioBe", "WASAPI / ASIO", O, "Output", "audio backend asio wasapi shared output driver", nullptr, nullptr, true, D(&V::audioBackend)},
    {"buf", "Buffer size / latency", O, "Output \xE2\x80\xBA Latency", "buffer frames latency delay crackle stutter", nullptr, nullptr, true, D(&V::bufferFrames)},
    {"sr", "Sample rate", O, "Output \xE2\x80\xBA Latency", "sample rate 44100 48000 khz", nullptr, nullptr, true, D(&V::sampleRate)},
    {"synthBe", "Synth backend (OmniMIDI, KDMAPI, WinMM)", O, "Output \xE2\x80\xBA Different synth", "kdmapi omnimidi winmm dll backend route", nullptr, "out", true, D(&V::apiBackend)},
    {"profile", "Performance profile", P, "Performance", "accurate balanced extreme preset", nullptr, nullptr, false, nullptr},
    {"voices", "Voice limit (max voices)", P, "Performance", "maximum voices voice presets polyphony limit pool ceiling", nullptr, nullptr, false, D(&V::maxVoices)},
    {"correct", "Correctness mode (removed: it no longer changed anything)", P, "Performance", "correctness mode full correctness", nullptr, nullptr, false, D(&V::correctnessMode)},
    {"decim", "Voice decimation", P, "Performance \xE2\x80\xBA Shortcuts", "decimation mute quiet cpu", nullptr, nullptr, false, D(&V::decimation)},
    {"fastTail", "Fast tail math", P, "Performance \xE2\x80\xBA Shortcuts", "avx2 chunk accuracy", nullptr, nullptr, false, D(&V::fastChunkMath)},
    {"stealReject", "Skip quiet notes (steal reject)", P, "Performance \xE2\x80\xBA Shortcuts", "steal reject bassmidi drop", nullptr, nullptr, false, D(&V::stealReject)},
    {"coalesce", "Combine rapid repeats (note-on coalescing)", P, "Performance \xE2\x80\xBA Shortcuts", "coalescing retrigger repeats re-excite", nullptr, nullptr, false, D(&V::noteOnCollapseThreshold)},
    {"noteMerge", "Merge identical notes", P, "Performance \xE2\x80\xBA Shortcuts", "note merge duplicate exact", nullptr, nullptr, false, D(&V::noteMerge)},
    {"ccCol", "Skip overwritten controller changes (CC collapse)", P, "Performance \xE2\x80\xBA Shortcuts", "cc collapse controller", nullptr, nullptr, false, D(&V::ccCollapse)},
    {"bendCol", "Thin out pitch bends (bend collapse)", P, "Performance \xE2\x80\xBA Shortcuts", "pitch bend collapse", nullptr, nullptr, false, D(&V::bendCollapse)},
    {"minNote", "Minimum note length", P, "Performance \xE2\x80\xBA Shortcuts", "short notes clicks length", nullptr, nullptr, false, D(&V::minNoteMs)},
    {"omni", "Block timing (OmniMIDI mode)", P, "Performance \xE2\x80\xBA Shortcuts", "omnimidi block timing quantize", nullptr, nullptr, false, D(&V::blockTiming)},
    {"unbounded", "Never skip ahead (unbounded render)", P, "Performance \xE2\x80\xBA Shortcuts", "unbounded recovery slow down", nullptr, nullptr, false, D(&V::unboundedRender)},
    {"threads", "Render threads", P, "Performance \xE2\x80\xBA CPU", "threads cores cpu workers", nullptr, nullptr, true, D(&V::renderThreads)},
    {"shards", "Engine shards", P, "Performance \xE2\x80\xBA CPU", "shards split engine", nullptr, nullptr, true, D(&V::shards)},
    {"affinity", "Core placement (thread affinity)", P, "Performance \xE2\x80\xBA CPU", "hybrid thread affinity p-cores e-cores intel", nullptr, nullptr, false, D(&V::threadAffinityMode)},
    {"largePages", "Large memory pages", P, "Performance \xE2\x80\xBA CPU", "large pages tlb memory", nullptr, nullptr, true, D(&V::largePages)},
    {"steal", "Steal policy", P, "Performance \xE2\x80\xBA Engine internals", "steal quality fast scan", nullptr, "perf", false, D(&V::stealPolicy)},
    {"retire", "Retire floor", P, "Performance \xE2\x80\xBA Engine internals", "voice retire floor release tail -76 db", nullptr, "perf", false, D(&V::voiceRetireThreshold)},
    {"perKey", "Per-key voice cap", P, "Performance \xE2\x80\xBA Engine internals", "per key cap", nullptr, "perf", false, D(&V::perKeyVoiceCap)},
    {"ghost", "Ghost budget", P, "Performance \xE2\x80\xBA Engine internals", "ghost steal storm", nullptr, "perf", false, D(&V::ghostBudget)},
    {"voiceMem", "Voice memory cap", P, "Performance \xE2\x80\xBA Engine internals", "voice memory budget mib ram", nullptr, "perf", true, D(&V::voiceMemoryBudgetMB)},
    {"midiIn", "MIDI keyboard input", M, "MIDI", "keyboard input hardware device physical", nullptr, nullptr, true, D(&V::midiInputEnabled)},
    {"overflow", "Overload behavior (Priority / Lossless)", M, "MIDI", "overflow shed lossless priority queue", nullptr, nullptr, true, D(&V::overflowMode)},
    {"protVel", "Protected velocity", M, "MIDI", "high-priority velocity protected shed", nullptr, nullptr, true, D(&V::highPriorityVelocity)},
    {"shedPct", "Start dropping at queue %", M, "MIDI", "shed start percent queue full", nullptr, nullptr, true, D(&V::shedStartPercent)},
    {"queueCap", "Queue capacity", M, "MIDI \xE2\x80\xBA Queue sizes", "ring capacity events", nullptr, "midi", true, D(&V::eventRingCapacity)},
    {"perCb", "Max events per audio block", M, "MIDI \xE2\x80\xBA Queue sizes", "events per callback", nullptr, "midi", true, D(&V::maxEventsPerBlock)},
    // Sent live with SetFastProducer (the prototype marked it ↻).
    {"fastProd", "Fast player lane (fast producer)", M, "MIDI \xE2\x80\xBA Queue sizes", "fast producer", nullptr, "midi", false, D(&V::fastProducer)},
    {"render", "Render a MIDI to WAV", R, "Tools", "offline renderer render export wav file", nullptr, nullptr, false, nullptr},
    {"record", "Record what you hear", RC, "Tools", "live recording record wav capture", nullptr, nullptr, false, nullptr},
    {"diag", "Diagnostics and system info", G, "Tools", "diagnostics bug report system", nullptr, nullptr, false, nullptr},
    {"theme", "Theme (Modern / Rack), accent colour", T, "Settings", "theme colour color custom rack dark", nullptr, nullptr, false, nullptr},
    {"motion", "Motion (animations)", T, "Settings", "animation animations motion calm bouncy springs reduce", nullptr, nullptr, false, nullptr},
    {"uiscale", "UI size (scaling)", T, "Settings", "size scale scaling zoom bigger larger text dpi small", nullptr, nullptr, false, nullptr},
    {"fire", "Load meter fire", T, "Settings", "fire flames load meter fun 500", nullptr, nullptr, false, nullptr},
    {"tour", "Welcome tour", T, "Settings", "tour intro introduction replay welcome tutorial", nullptr, nullptr, false, nullptr},
    {"config", "Config file, import / export profile, reset", T, "Settings", "advanced config json open config folder profile import export reset defaults", nullptr, nullptr, false, nullptr},
    {"diagOn", "Driver logging", T, "Settings", "debug output logging monitor window", nullptr, nullptr, true, D(&V::diagnosticsEnabled)},

    // Not searchable on their own; found through the rows above.
    {"master", nullptr, H, nullptr, nullptr, nullptr, nullptr, false, D(&V::masterVolume)},
    {"revSize", nullptr, E, nullptr, nullptr, "reverb", nullptr, false, D(&V::reverbRoomSize)},
    {"revPre", nullptr, E, nullptr, nullptr, "reverb", nullptr, false, D(&V::reverbPreDelayMs)},
    {"revWidth", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbWidth)},
    {"revEarly", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbEarlyLevel)},
    {"revLate", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbLateLevel)},
    {"revDiff", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbDiffusion)},
    {"revModD", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbModDepth)},
    {"revModR", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbModRate)},
    {"revLow", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbLowCutHz)},
    {"revHigh", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbHighCutHz)},
    {"limThresh", nullptr, E, nullptr, nullptr, "limiter", nullptr, false, D(&V::limiterThreshold)},
    {"limLook", nullptr, E, nullptr, nullptr, "limiter", nullptr, false, D(&V::limiterLookaheadMs)},
    {"limAtk", nullptr, E, nullptr, nullptr, "limiter", nullptr, false, D(&V::limiterAttackMs)},
    {"limRel", nullptr, E, nullptr, nullptr, "limiter", nullptr, false, D(&V::limiterReleaseMs)},
    {"chThresh", nullptr, E, nullptr, nullptr, "chlim", nullptr, false, D(&V::channelLimiterThreshold)},
    {"chRel", nullptr, E, nullptr, nullptr, "chlim", nullptr, false, D(&V::channelLimiterReleaseMs)},
    {"decByLevel", nullptr, P, nullptr, nullptr, nullptr, nullptr, false, D(&V::decimationByLevel)},
    {"cull", nullptr, P, nullptr, nullptr, nullptr, nullptr, false, D(&V::decimationCullBlocks)},
    {"reexcite", nullptr, P, nullptr, nullptr, nullptr, nullptr, false, D(&V::noteOnReexcite)},
    {"midiDev", nullptr, M, nullptr, nullptr, nullptr, nullptr, true, D(&V::midiInputDevice)},
    {"dll", nullptr, O, nullptr, nullptr, nullptr, "out", true, D(&V::apiBackendDll)},
    {"allowNested", nullptr, O, nullptr, nullptr, nullptr, "out", true, D(&V::apiAllowNested)},
    {"winmmDev", nullptr, O, nullptr, nullptr, nullptr, "out", true, D(&V::apiWinMmDevice)},
    {"diagWin", nullptr, T, nullptr, nullptr, nullptr, nullptr, true, D(&V::diagnosticsWindow)},
    {"debugOut", nullptr, T, nullptr, nullptr, nullptr, nullptr, true, D(&V::diagnosticsDebugOutput)},
};

#undef D

std::string Lower(const char* s) {
    std::string out = s ? s : "";
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

const SettingDef* AllSettings(size_t* count) {
    if (count) *count = sizeof(kSettings) / sizeof(kSettings[0]);
    return kSettings;
}

const SettingDef* FindSetting(const char* id) {
    if (!id) return nullptr;
    for (const SettingDef& s : kSettings)
        if (std::strcmp(s.id, id) == 0) return &s;
    return nullptr;
}

ChangeSummary SummarizeChanges(const ConfigValues& working, const ConfigValues& saved) {
    ChangeSummary out;
    for (const SettingDef& s : kSettings) {
        if (!s.differs || !s.differs(working, saved)) continue;
        ++out.total;
        if (s.restart) ++out.restart;
        ++out.perPage[static_cast<int>(s.page)];
    }
    // Safety net: a field missing from the table must still show the bar.
    if (out.total == 0 && !ConfigValuesEqual(working, saved)) out.total = 1;
    return out;
}

int SearchSettings(const char* query, const SettingDef** out, int max) {
    const std::string q = Lower(query);
    std::string words[8];
    int wordCount = 0;
    for (size_t i = 0; i < q.size() && wordCount < 8;) {
        while (i < q.size() && std::isspace(static_cast<unsigned char>(q[i]))) ++i;
        size_t j = i;
        while (j < q.size() && !std::isspace(static_cast<unsigned char>(q[j]))) ++j;
        if (j > i) words[wordCount++] = q.substr(i, j - i);
        i = j;
    }
    if (wordCount == 0) return 0;
    int n = 0;
    for (const SettingDef& s : kSettings) {
        if (!s.name || n >= max) continue;
        const std::string hay = Lower(s.name) + " " + Lower(s.where) + " " + Lower(s.keywords);
        bool all = true;
        for (int w = 0; w < wordCount && all; ++w)
            all = hay.find(words[w]) != std::string::npos;
        if (all) out[n++] = &s;
    }
    return n;
}

} // namespace svms::cfg
