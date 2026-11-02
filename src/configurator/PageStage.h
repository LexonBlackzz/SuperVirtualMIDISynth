#ifndef SVMS_CONFIGURATOR_PAGESTAGE_H
#define SVMS_CONFIGURATOR_PAGESTAGE_H

namespace svms::cfg {

class ConfigDocument;

// 3D Stage: put the 16 MIDI channels in a room around the listener. Scenes,
// a projected 3D / top / side stage with draggable channels, listener and
// furniture, quick group knobs, room and furniture, and the quality switches.
// Everything lands in the config's "stage" section and applies at engine start.
void DrawStagePage(ConfigDocument& doc);

} // namespace svms::cfg

#endif
