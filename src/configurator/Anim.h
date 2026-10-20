#ifndef SVMS_CONFIGURATOR_ANIM_H
#define SVMS_CONFIGURATOR_ANIM_H

#include "imgui.h"

namespace svms::cfg {

// Motion level from Settings: Full = springs, Calm = springs become eases and
// the page-entry stagger is dropped, Off = every value snaps to its target.
enum class Motion { Full = 0, Calm = 1, Off = 2 };

void SetMotion(Motion motion);
Motion GetMotion();

// Per-ID animated values. The first call for an ID snaps to the target.
// Springs overshoot like cubic-bezier(.34,1.56,.64,1) over ~0.45 s (thumbs,
// indicators, presses); eases follow cubic-bezier(.2,.8,.2,1) (fades,
// reveals). With Motion::Off both return exactly `target`.
float AnimSpring(ImGuiID id, float target);
float AnimEase(ImGuiID id, float target, float seconds = 0.3f);

// One-shot pulses (search flash, toggle pop). Trigger starts it; Pulse
// returns progress 0..1 over `seconds`, and 1 when idle or with motion Off.
void AnimTrigger(ImGuiID id);
float AnimPulse(ImGuiID id, float seconds);
bool AnimPulseActive(ImGuiID id, float seconds);

// The ease curve itself, for callers that drive their own time base.
float EaseOut(float t);

} // namespace svms::cfg

#endif
