#ifndef SVMS_CONFIGURATOR_PAGEREVERB_H
#define SVMS_CONFIGURATOR_PAGEREVERB_H

// PageReverb.cpp uses ImGui's internal ImClamp helper for its visual smoothing
// math. Pull the helper declaration in here so the page compiles with the
// vendored Dear ImGui version used by V3.
#include "imgui_internal.h"

struct ConfigDocument;

namespace svms::cfg {

void DrawReverbPage(ConfigDocument& doc);

} // namespace svms::cfg

#endif
