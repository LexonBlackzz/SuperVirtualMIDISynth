#ifndef SVMS_CONFIGURATOR_WIDGETS_H
#define SVMS_CONFIGURATOR_WIDGETS_H

#include "imgui.h"
#include <cstdint>
#include <string>
#include <functional>

namespace svms {
class RuntimeLinkClient;
struct RLTelemetry;
enum class RLCommandType : uint32_t;
}

namespace svms::cfg {

struct LiveLinkContext {
    svms::RuntimeLinkClient* client = nullptr;
    const svms::RLTelemetry* telemetry = nullptr;
    bool connected = false;
};

void SetLiveLinkContext(const LiveLinkContext& ctx);
const LiveLinkContext& GetLiveLinkContext();

void PushLiveFloat(svms::RLCommandType type, float value);
void PushLiveBool(svms::RLCommandType type, bool value);

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

void DrawReverbVisualizer(ImDrawList* dl, ImVec2 center, float radius,
                           float roomSize, float decay, float diffusion,
                           float width, float modDepth, float time);

bool BeginToastOverlay();
void EndToastOverlay();

void LiveBadge(const char* tooltip = nullptr);
void RestartRequiredBadge();

} // namespace svms::cfg

#endif
