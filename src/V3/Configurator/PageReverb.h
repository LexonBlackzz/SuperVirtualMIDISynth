#ifndef SVMS_CONFIGURATOR_PAGEREVERB_H
#define SVMS_CONFIGURATOR_PAGEREVERB_H

// PageReverb.cpp uses ImGui's internal ImClamp helper for its visual smoothing
// math. Pull the helper declaration in here so the page compiles with the
// vendored Dear ImGui version used by V3.
#include "imgui_internal.h"

struct ConfigDocument;

namespace svms::cfg {

void DrawReverbPage(ConfigDocument& doc);

struct ConfigValues;
// The LCD decay trace, sized by the caller (used by the Home overview).
void DrawDecayTraceMini(const ImVec2& size, const ConfigValues& values);

} // namespace svms::cfg

#endif
