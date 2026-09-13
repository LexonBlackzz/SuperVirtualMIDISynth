#ifndef SVMS_CONFIGURATOR_PAGECHANNELLIMITER_H
#define SVMS_CONFIGURATOR_PAGECHANNELLIMITER_H

// DrawChannelGrBar uses ImClamp from ImGui's internal helpers.
#include "imgui_internal.h"

namespace svms::cfg {

class ConfigDocument;
void DrawChannelLimiterPage(ConfigDocument& doc);

} // namespace svms::cfg

#endif
