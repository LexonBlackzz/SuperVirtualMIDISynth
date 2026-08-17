#ifndef SVMS_CONFIGURATOR_PAGELIMITER_H
#define SVMS_CONFIGURATOR_PAGELIMITER_H

// PageLimiter.cpp uses a few ImGui math helpers (ImClamp) in its custom
// meter/history drawing. Keep the dependency explicit instead of relying on
// accidental include order from another configurator header.
#include "imgui_internal.h"

namespace svms::cfg {

class ConfigDocument;
void DrawLimiterPage(ConfigDocument& doc);

} // namespace svms::cfg

#endif
