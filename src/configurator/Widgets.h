#ifndef SVMS_CONFIGURATOR_WIDGETS_H
#define SVMS_CONFIGURATOR_WIDGETS_H

#include "imgui.h"
#include "configurator/LiveParam.h"
#include <cstdint>
#include <string>
#include <functional>

struct SVMS_TelemetryV3;

namespace svms {
class EngineLink;


}

namespace svms::cfg {

class ConfiguratorApp;
struct ConfigValues;

struct LiveLinkContext {
    ConfiguratorApp* app = nullptr;              // routing target for live changes
    svms::EngineLink* client = nullptr; // direct client access (reserved)
    const SVMS_TelemetryV3* telemetry = nullptr;
    bool connected = false;
};

void SetLiveLinkContext(const LiveLinkContext& ctx);
const LiveLinkContext& GetLiveLinkContext();

void PushLiveFloat(LiveParam type, float value);
void PushLiveBool(LiveParam type, bool value);
void PushLiveMaxVoices(uint32_t value);
void PushLiveLimiterAlgorithm(uint32_t value);
// Coalesced per-MIDI-channel limiter update (dedicated wire command).
void PushLiveChannelLimiter(bool enabled, float threshold, float releaseMs);
// Pages that send a RuntimeLink command directly call this for the top bar's
// "Applied live" chip.
void NoteLiveChange();
// Sends one RuntimeLink command with a plain integer parameter (the live pushes
// that are not part of the coalesced working state). No-op when offline.
void SendLiveCommand(uint32_t command, uint32_t param);

// --- Rack-style primitives -------------------------------------------------
// Panel labels: mono caps as authored.
void PushLabel(float scale = 0.9f);
void PopLabel();
// Monospace font for readouts; falls back to the UI font when unavailable.
void PushMono(float scale = 0.9f);
void PopMono();
// Panel with an engraved title and corner screws. Always call EndRackPanel().
bool BeginRackPanel(const char* title);
void EndRackPanel();
// Row of raised keys with a lamp; exactly one is lit. Returns true on change.
bool KeyGroup(const char* id, int* current, const char* const* labels,
              int count);
// Raised push key. `primary` fills with the accent colour.
// A disabled key is drawn flat and dim and never reports a click.
bool KeyButton(const char* label, const ImVec2& size, bool primary = false,
               bool enabled = true);
// Rack toggle: a slot with a sliding lever that lights when on (draw only).
void DrawRackLever(ImDrawList* dl, ImVec2 pos, ImVec2 size, bool on, bool hovered);
// Small lamp; `on` lights it with the accent colour.
void DrawLed(ImDrawList* dl, ImVec2 center, float radius, bool on,
             const ImVec4* color = nullptr);

// Knob centred in the available width. `committed` is set when the drag ends
// (for settings that should be sent once, not on every frame).
bool PanelKnob(const char* label, float* value, float minValue, float maxValue,
               float defaultValue, const char* format, float size,
               const char* help, bool* committed = nullptr,
               float (*displayFn)(float) = nullptr,
               float (*inverseFn)(float) = nullptr);

bool SliderFloat(const char* label, float* value, float min, float max,
                 const char* format = "%.2f", const char* tooltip = nullptr);
bool SliderInt(const char* label, int* value, int min, int max,
               const char* tooltip = nullptr);

struct KnobState {
    float value;
    float minValue;
    float maxValue;
    float defaultValue;
    const char* label;
    const char* unit;
    float size;
    float displayScale = 1.0f;
    float (*displayFn)(float) = nullptr;
    // Maps a typed display value back to the stored value (needed for
    // Ctrl+click entry whenever displayFn is not a plain scale).
    float (*inverseFn)(float) = nullptr;
};

bool RotaryKnob(KnobState& state, const char* format = "%.2f");





} // namespace svms::cfg

#endif
