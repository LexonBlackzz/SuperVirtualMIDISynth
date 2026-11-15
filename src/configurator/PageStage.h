#ifndef SVMS_CONFIGURATOR_PAGESTAGE_H
#define SVMS_CONFIGURATOR_PAGESTAGE_H

#include "imgui.h"

namespace svms::cfg {

class ConfigDocument;

// 3D Stage: put the 16 MIDI channels in a room around the listener. Scenes,
// a projected 3D / top / side stage with draggable channels, listener and
// furniture, quick group knobs, room and furniture, and the quality switches.
// Everything lands in the config's "stage" section; with a driver running the
// changes play live, otherwise the driver reads them when it starts.
void DrawStagePage(ConfigDocument& doc);

// The listening transport for the page: a strip that floats over the top of
// the page area (areaMin..areaMax), so a test can be started, stopped and
// compared without scrolling back to the canvas. Call it after the page
// window every frame while the page is shown.
void DrawStageTransport(ConfigDocument& doc, ImVec2 areaMin, ImVec2 areaMax);

// Ends a running listening test. Call it every frame the page is not shown.
void StopStageListening();

} // namespace svms::cfg

#endif
