#ifndef SVMS_CONFIGURATOR_WIDGETS_H
#define SVMS_CONFIGURATOR_WIDGETS_H

#include "imgui.h"
#include <cstdint>
#include <string>
#include <functional>

namespace svms {
class RuntimeLinkClientV2;
struct RuntimeLinkTelemetryV2;
enum class RLCommandType : uint32_t;
}

namespace svms::cfg {

class ConfiguratorApp;
struct ConfigValues;

struct LiveLinkContext {
    ConfiguratorApp* app = nullptr;              // routing target for live changes
    svms::RuntimeLinkClientV2* client = nullptr; // direct client access (reserved)
    const svms::RuntimeLinkTelemetryV2* telemetry = nullptr;
    bool connected = false;
};

void SetLiveLinkContext(const LiveLinkContext& ctx);
const LiveLinkContext& GetLiveLinkContext();

void PushLiveFloat(svms::RLCommandType type, float value);
void PushLiveBool(svms::RLCommandType type, bool value);
void PushLiveMaxVoices(uint32_t value);

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
