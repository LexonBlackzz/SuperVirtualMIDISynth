#ifndef SVMS_CONFIGURATOR_WIDGETS_H
#define SVMS_CONFIGURATOR_WIDGETS_H

#include "imgui.h"
#include <cstdint>
#include <string>
#include <functional>

namespace svms {
class RuntimeLinkClient;
struct RuntimeLinkTelemetryV2;
enum class RLCommandType : uint32_t;
}

namespace svms::cfg {

class ConfiguratorApp;
struct ConfigValues;

struct LiveLinkContext {
    ConfiguratorApp* app = nullptr;              // routing target for live changes
    svms::RuntimeLinkClient* client = nullptr; // direct client access (reserved)
    const svms::RuntimeLinkTelemetryV2* telemetry = nullptr;
    bool connected = false;
};

void SetLiveLinkContext(const LiveLinkContext& ctx);
const LiveLinkContext& GetLiveLinkContext();

void PushLiveFloat(svms::RLCommandType type, float value);
void PushLiveBool(svms::RLCommandType type, bool value);
void PushLiveMaxVoices(uint32_t value);
void PushLiveLimiterAlgorithm(uint32_t value);
// Coalesced per-MIDI-channel limiter update (dedicated wire command).
void PushLiveChannelLimiter(bool enabled, float threshold, float releaseMs);

// --- Rack-style primitives -------------------------------------------------
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
// Small lamp; `on` lights it with the accent colour.
void DrawLed(ImDrawList* dl, ImVec2 center, float radius, bool on,
             const ImVec4* color = nullptr);
// Bordered RESTART tag, drawn at the cursor.
void RestartPill();

// --- Panel controls: captioned controls for hardware-style panels ----------
// Small mono caption; hover shows `help`; optional RESTART tag after it.
void PanelCaption(const char* caption, const char* help, bool restart = false);
// Caption above a key group. Returns true when the selection changed.
bool PanelKeys(const char* caption, int* current, const char* const* labels,
               int count, const char* help, bool restart = false);
// Lever with its caption beside it.
bool PanelLever(const char* caption, bool* value, const char* help,
                bool restart = false);
// Knob centred in the available width. `committed` is set when the drag ends
// (for settings that should be sent once, not on every frame).
bool PanelKnob(const char* label, float* value, float minValue, float maxValue,
               float defaultValue, const char* format, float size,
               const char* help, bool* committed = nullptr,
               float (*displayFn)(float) = nullptr);
// Number on an LCD-styled field. `committed` is set when editing finishes.
bool PanelLcdInt(const char* caption, int* value, int minValue, int maxValue,
                 const char* help, bool restart = false,
                 bool* committed = nullptr, const char* zeroText = nullptr);
// Inset display: dark background, rim, optional grid. Draws only; the caller
// reserves layout space (e.g. with Dummy).
void DrawLcdFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, int gridCols = 0,
                  int gridRows = 0);
// LED ladder meter (shared by the limiter page and DrawVerticalMeter).
void DrawLedLadder(ImDrawList* dl, ImVec2 pos, ImVec2 size, float value,
                   float peak, bool fromTop, const ImVec4* single);

// Settings label with the first sentence of `help` as a dim description line
// (clipped to the column); the full text shows on hover.
void SettingLabel(const char* label, const char* help);
// SectionHeader opens a rack panel (closing the previous one) while the page
// is drawn between these two calls. Pages that never call SectionHeader, or
// call it from a nested window, are unaffected.
void BeginAutoPanels();
void EndAutoPanels();

void SectionHeader(const char* label);
void HelpMarker(const char* desc);
bool ToggleSwitch(const char* label, bool* value, const char* tooltip = nullptr);
bool LabeledFloat(const char* label, float* value, float min, float max,
                  const char* format = "%.2f", const char* tooltip = nullptr);
bool LabeledInt(const char* label, int* value, int min, int max,
                const char* tooltip = nullptr);
bool LabeledUInt(const char* label, unsigned int* value, unsigned int min,
                 unsigned int max, const char* tooltip = nullptr);
bool LabeledCombo(const char* label, int* current, const char* const* items,
                  int itemCount, const char* tooltip = nullptr);
bool SliderFloat(const char* label, float* value, float min, float max,
                 const char* format = "%.2f", const char* tooltip = nullptr);
bool SliderInt(const char* label, int* value, int min, int max,
               const char* tooltip = nullptr);
void StatusBar(const char* text, bool modified);
void ToastNotification(const char* message, float durationSeconds = 3.0f);
void PushToastStyle();
void PopToastStyle();
bool BeginToast(const char* id);
void EndToast();

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
};

bool RotaryKnob(KnobState& state, const char* format = "%.2f");

void DrawVerticalMeter(const char* id, float value, float peak,
                       const ImVec2& size, bool showScale = true);
void DrawGainReductionMeter(const char* id, float gr,
                            const ImVec2& size);

// Applied-echo badge: compares the WORKING copy against the live state
// the engine echoes back in telemetry ("applied"), so the user sees the
// RuntimeLink flush converge (or stall).  Green APPLIED when they match,
// amber PENDING while a flush is in flight, grey OFFLINE when no host.
void AppliedStateBadge(bool connected,
                       const svms::RuntimeLinkTelemetryV2* telemetry,
                       const ConfigValues& working,
                       const char* scopeTooltip = nullptr);
bool LiveAppliedMatches(const svms::RuntimeLinkTelemetryV2& telemetry,
                        const ConfigValues& working);

void DrawReverbVisualizer(ImDrawList* dl, ImVec2 center, float radius,
                           float roomSize, float decay, float diffusion,
                           float width, float modDepth, float time);

bool BeginToastOverlay();
void EndToastOverlay();

void LiveBadge(const char* tooltip = nullptr);
void RestartRequiredBadge();

} // namespace svms::cfg

#endif
