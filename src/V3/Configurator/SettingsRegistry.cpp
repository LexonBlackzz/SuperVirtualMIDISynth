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
    // id, name, page, where, keywords, fxPanel, disclosure, restart, differs, legacy caption, legacy page
    {"sf", "SoundFont", S, "Sound", "sf2 sfz bank instrument soundfont load choose priority stack overview", nullptr, nullptr, false, D(&V::soundFontPath, &V::soundFontPaths), "SOUNDFONT", S},
    {"velCurve", "Velocity curve", S, "Sound \xE2\x80\xBA Velocity", "velocity loud soft dynamics", nullptr, nullptr, true, D(&V::velocityCurve), "CURVE", M},
    {"velFloor", "Velocity floor", S, "Sound \xE2\x80\xBA Velocity", "velocity minimum quiet", nullptr, nullptr, true, D(&V::velocityFloor), "FLOOR", M},
    {"velIgnore", "Ignore notes softer than", S, "Sound \xE2\x80\xBA Velocity", "ignore velocity below threshold drop ghost notes", nullptr, nullptr, true, D(&V::velocityIgnoreBelow), "IGNORE BELOW", M},
    {"phase", "Voice character (phase rotation)", S, "Sound", "phase rotation coherent hum hilbert analytic sweep diffuse random", nullptr, nullptr, false, D(&V::phaseRotationMode), "PHASE ROTATION  (LIVE)", S},
    {"tuning", "Tuning (12 / 31 EDO)", S, "Sound", "edo microtonal 31", nullptr, nullptr, true, D(&V::tuningEdo), "TUNING", M},
    {"routes", "Bank and preset routes", S, "Sound \xE2\x80\xBA Routes", "bank preset route mapping", nullptr, "routes", true, D(&V::soundFontRoutes), "BANK / PRESET ROUTES", S},
    {"revOn", "Reverb", E, "Effects \xE2\x80\xBA Reverb", "reverb room hall space wet", "reverb", nullptr, false, D(&V::enableReverb), "REVERB", E},
    {"revMix", "Reverb mix", E, "Effects \xE2\x80\xBA Reverb", "wet dry amount", "reverb", nullptr, false, D(&V::reverbMix), "REVERB", E},
    {"revDecay", "Reverb decay", E, "Effects \xE2\x80\xBA Reverb", "rt60 length tail", "reverb", nullptr, false, D(&V::reverbDecay), "REVERB", E},
    {"revDamp", "Reverb damping / tone", E, "Effects \xE2\x80\xBA Reverb \xE2\x80\xBA Fine-tune", "damping low cut high cut tone eq", "reverb", "rev", false, D(&V::reverbDamping), "TEXTURE / MODULATION / TONE", E},
    {"limOn", "Master limiter", E, "Effects \xE2\x80\xBA Master limiter", "limiter clip clipping loudness compressor threshold", "limiter", nullptr, false, D(&V::limiterEnabled), "LIMITER", E},
    {"limMode", "Limiter mode (Classic / Adaptive)", E, "Effects \xE2\x80\xBA Master limiter", "adaptive classic lookahead", "limiter", nullptr, false, D(&V::limiterAlgorithm), "LIMITER", E},
    {"chOn", "Channel limiter", E, "Effects \xE2\x80\xBA Channel limiter", "per channel limiter 16 channels", "chlim", nullptr, false, D(&V::channelLimiterEnabled), "PER-CHANNEL LIMITER", E},
    {"device", "Audio device", O, "Output", "output device speakers headphones interface refresh devices", nullptr, nullptr, true, D(&V::audioDevice, &V::linuxDevice), "DEVICE", O},
    {"audioBe", "WASAPI / ASIO", O, "Output", "audio backend asio wasapi shared output driver", nullptr, nullptr, true, D(&V::audioBackend), "BACKEND", O},
    {"buf", "Buffer size / latency", O, "Output \xE2\x80\xBA Latency", "buffer frames latency delay crackle stutter", nullptr, nullptr, true, D(&V::bufferFrames), "BUFFER FRAMES", O},
    {"sr", "Sample rate", O, "Output \xE2\x80\xBA Latency", "sample rate 44100 48000 khz", nullptr, nullptr, true, D(&V::sampleRate), "SAMPLE RATE", O},
    {"synthBe", "Synth backend (OmniMIDI, KDMAPI, WinMM)", O, "Output \xE2\x80\xBA Different synth", "kdmapi omnimidi winmm dll backend route", nullptr, "out", true, D(&V::apiBackend), "SYNTH BACKEND", O},
    {"profile", "Performance profile", P, "Performance", "accurate balanced extreme preset", nullptr, nullptr, false, nullptr, nullptr, P},
    {"voices", "Voice limit (max voices)", P, "Performance", "maximum voices voice presets polyphony limit pool ceiling", nullptr, nullptr, false, D(&V::maxVoices), "VOICE CEILING  (LIVE)", P},
    {"correct", "Correctness mode (removed: it no longer changed anything)", P, "Performance", "correctness mode full correctness", nullptr, nullptr, false, D(&V::correctnessMode), "FULL CORRECTNESS", P},
    {"decim", "Voice decimation", P, "Performance \xE2\x80\xBA Shortcuts", "decimation mute quiet cpu", nullptr, nullptr, false, D(&V::decimation), "VOICE DECIMATION", P},
    {"fastTail", "Fast tail math", P, "Performance \xE2\x80\xBA Shortcuts", "avx2 chunk accuracy", nullptr, nullptr, false, D(&V::fastChunkMath), "FAST TAIL MATH  (LOWERS ACCURACY)", P},
    {"stealReject", "Skip quiet notes (steal reject)", P, "Performance \xE2\x80\xBA Shortcuts", "steal reject bassmidi drop", nullptr, nullptr, false, D(&V::stealReject), "STEAL REJECT  (LISTEN)", P},
    {"coalesce", "Combine rapid repeats (note-on coalescing)", P, "Performance \xE2\x80\xBA Shortcuts", "coalescing retrigger repeats re-excite", nullptr, nullptr, false, D(&V::noteOnCollapseThreshold), "NOTE-ON COALESCING", M},
    {"noteMerge", "Merge identical notes", P, "Performance \xE2\x80\xBA Shortcuts", "note merge duplicate exact", nullptr, nullptr, false, D(&V::noteMerge), "EXACT NOTE MERGE", M},
    {"ccCol", "Skip overwritten controller changes (CC collapse)", P, "Performance \xE2\x80\xBA Shortcuts", "cc collapse controller", nullptr, nullptr, false, D(&V::ccCollapse), "CC COLLAPSE", M},
    {"bendCol", "Thin out pitch bends (bend collapse)", P, "Performance \xE2\x80\xBA Shortcuts", "pitch bend collapse", nullptr, nullptr, false, D(&V::bendCollapse), "BEND COLLAPSE  (LOWERS ACCURACY)", M},
    {"minNote", "Minimum note length", P, "Performance \xE2\x80\xBA Shortcuts", "short notes clicks length", nullptr, nullptr, false, D(&V::minNoteMs), "MIN NOTE LENGTH  (LOWERS ACCURACY)", M},
    {"omni", "Block timing (OmniMIDI mode)", P, "Performance \xE2\x80\xBA Shortcuts", "omnimidi block timing quantize", nullptr, nullptr, false, D(&V::blockTiming), "OMNIMIDI MODE", M},
    {"unbounded", "Never skip ahead (unbounded render)", P, "Performance \xE2\x80\xBA Shortcuts", "unbounded recovery slow down", nullptr, nullptr, false, D(&V::unboundedRender), "UNBOUNDED RENDER", M},
    {"threads", "Render threads", P, "Performance \xE2\x80\xBA CPU", "threads cores cpu workers", nullptr, nullptr, true, D(&V::renderThreads), "RENDER THREADS", P},
    {"shards", "Engine shards", P, "Performance \xE2\x80\xBA CPU", "shards split engine", nullptr, nullptr, true, D(&V::shards), "ENGINE SHARDS", P},
    {"affinity", "Core placement (thread affinity)", P, "Performance \xE2\x80\xBA CPU", "hybrid thread affinity p-cores e-cores intel", nullptr, nullptr, false, D(&V::threadAffinityMode), "THREAD AFFINITY  (LIVE)", P},
    {"largePages", "Large memory pages", P, "Performance \xE2\x80\xBA CPU", "large pages tlb memory", nullptr, nullptr, true, D(&V::largePages), "LARGE PAGES", P},
    {"steal", "Steal policy", P, "Performance \xE2\x80\xBA Engine internals", "steal quality fast scan", nullptr, "perf", false, D(&V::stealPolicy), "STEAL POLICY", P},
    {"retire", "Retire floor", P, "Performance \xE2\x80\xBA Engine internals", "voice retire floor release tail -76 db", nullptr, "perf", false, D(&V::voiceRetireThreshold), "RETIRE FLOOR", P},
    {"perKey", "Per-key voice cap", P, "Performance \xE2\x80\xBA Engine internals", "per key cap", nullptr, "perf", false, D(&V::perKeyVoiceCap), "PER-KEY CAP", P},
    {"ghost", "Ghost budget", P, "Performance \xE2\x80\xBA Engine internals", "ghost steal storm", nullptr, "perf", false, D(&V::ghostBudget), "GHOST BUDGET", P},
    {"voiceMem", "Voice memory cap", P, "Performance \xE2\x80\xBA Engine internals", "voice memory budget mib ram", nullptr, "perf", true, D(&V::voiceMemoryBudgetMB), "VOICE MEMORY  (MIB)", P},
    {"midiIn", "MIDI keyboard input", M, "MIDI", "keyboard input hardware device physical", nullptr, nullptr, true, D(&V::midiInputEnabled), "ROUTE PHYSICAL INPUT", M},
    {"overflow", "Overload behavior (Priority / Lossless)", M, "MIDI", "overflow shed lossless priority queue", nullptr, nullptr, true, D(&V::overflowMode), "OVERFLOW MODE", M},
    {"protVel", "Protected velocity", M, "MIDI", "high-priority velocity protected shed", nullptr, nullptr, true, D(&V::highPriorityVelocity), "PROTECTED VELOCITY", M},
    {"shedPct", "Start dropping at queue %", M, "MIDI", "shed start percent queue full", nullptr, nullptr, true, D(&V::shedStartPercent), "SHED STARTS AT %", M},
    {"queueCap", "Queue capacity", M, "MIDI \xE2\x80\xBA Queue sizes", "ring capacity events", nullptr, "midi", true, D(&V::eventRingCapacity), "QUEUE CAPACITY", M},
    {"perCb", "Max events per audio block", M, "MIDI \xE2\x80\xBA Queue sizes", "events per callback", nullptr, "midi", true, D(&V::maxEventsPerBlock), "EVENTS / CALLBACK", M},
    // Sent live with SetFastProducer (the prototype marked it ↻).
    {"fastProd", "Fast player lane (fast producer)", M, "MIDI \xE2\x80\xBA Queue sizes", "fast producer", nullptr, "midi", false, D(&V::fastProducer), "FAST PRODUCER  (LISTEN)", M},
    {"render", "Render a MIDI to WAV", R, "Tools", "offline renderer render export wav file", nullptr, nullptr, false, nullptr, "OFFLINE RENDERER", R},
    {"record", "Record what you hear", RC, "Tools", "live recording record wav capture", nullptr, nullptr, false, nullptr, "LIVE WAV RECORDING", RC},
    {"diag", "Diagnostics and system info", G, "Tools", "diagnostics bug report system", nullptr, nullptr, false, nullptr, nullptr, G},
    {"theme", "Theme (Modern / Rack), accent colour, motion", T, "Settings", "theme colour color custom rack dark animation motion fire", nullptr, nullptr, false, nullptr, "APPEARANCE", T},
    {"config", "Config file, import / export profile, reset", T, "Settings", "advanced config json open config folder profile import export reset defaults", nullptr, nullptr, false, nullptr, "CONFIGURATION", T},
    {"diagOn", "Driver logging", T, "Settings", "debug output logging monitor window", nullptr, nullptr, true, D(&V::diagnosticsEnabled), "Diagnostics Enabled", T},

    // Not searchable on their own; found through the rows above.
    {"master", nullptr, H, nullptr, nullptr, nullptr, nullptr, false, D(&V::masterVolume), nullptr, H},
    {"revSize", nullptr, E, nullptr, nullptr, "reverb", nullptr, false, D(&V::reverbRoomSize), nullptr, E},
    {"revPre", nullptr, E, nullptr, nullptr, "reverb", nullptr, false, D(&V::reverbPreDelayMs), nullptr, E},
    {"revWidth", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbWidth), nullptr, E},
    {"revEarly", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbEarlyLevel), nullptr, E},
    {"revLate", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbLateLevel), nullptr, E},
    {"revDiff", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbDiffusion), nullptr, E},
    {"revModD", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbModDepth), nullptr, E},
    {"revModR", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbModRate), nullptr, E},
    {"revLow", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbLowCutHz), nullptr, E},
    {"revHigh", nullptr, E, nullptr, nullptr, "reverb", "rev", false, D(&V::reverbHighCutHz), nullptr, E},
    {"limThresh", nullptr, E, nullptr, nullptr, "limiter", nullptr, false, D(&V::limiterThreshold), nullptr, E},
    {"limLook", nullptr, E, nullptr, nullptr, "limiter", nullptr, false, D(&V::limiterLookaheadMs), nullptr, E},
    {"limAtk", nullptr, E, nullptr, nullptr, "limiter", nullptr, false, D(&V::limiterAttackMs), nullptr, E},
    {"limRel", nullptr, E, nullptr, nullptr, "limiter", nullptr, false, D(&V::limiterReleaseMs), nullptr, E},
    {"chThresh", nullptr, E, nullptr, nullptr, "chlim", nullptr, false, D(&V::channelLimiterThreshold), nullptr, E},
    {"chRel", nullptr, E, nullptr, nullptr, "chlim", nullptr, false, D(&V::channelLimiterReleaseMs), nullptr, E},
    {"decByLevel", nullptr, P, nullptr, nullptr, nullptr, nullptr, false, D(&V::decimationByLevel), nullptr, P},
    {"cull", nullptr, P, nullptr, nullptr, nullptr, nullptr, false, D(&V::decimationCullBlocks), nullptr, P},
    {"reexcite", nullptr, P, nullptr, nullptr, nullptr, nullptr, false, D(&V::noteOnReexcite), nullptr, M},
    {"midiDev", nullptr, M, nullptr, nullptr, nullptr, nullptr, true, D(&V::midiInputDevice), nullptr, M},
    {"dll", nullptr, O, nullptr, nullptr, nullptr, "out", true, D(&V::apiBackendDll), nullptr, O},
    {"allowNested", nullptr, O, nullptr, nullptr, nullptr, "out", true, D(&V::apiAllowNested), nullptr, O},
    {"winmmDev", nullptr, O, nullptr, nullptr, nullptr, "out", true, D(&V::apiWinMmDevice), nullptr, O},
    {"diagWin", nullptr, T, nullptr, nullptr, nullptr, nullptr, true, D(&V::diagnosticsWindow), nullptr, T},
    {"debugOut", nullptr, T, nullptr, nullptr, nullptr, nullptr, true, D(&V::diagnosticsDebugOutput), nullptr, T},
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
