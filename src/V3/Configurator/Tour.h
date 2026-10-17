#ifndef SVMS_CONFIGURATOR_TOUR_H
#define SVMS_CONFIGURATOR_TOUR_H

// Welcome tour (CONFIGURATOR_REDESIGN/VISION.md §10): a welcome card, then seven spotlight steps.
// Targets register their rect every frame; the hole and the tip card glide on springs.

#include "imgui.h"

namespace svms::cfg {

class ConfiguratorApp;

namespace tour {

// Starts at the welcome card (Settings > Replay now, and first launch).
void Start();
bool Active();
// A target reports its screen rect each frame. Only the keys the tour uses are kept.
void Register(const char* key, ImVec2 min, ImVec2 max);
// Draws the overlay; call last in the frame, after every other window.
void Draw(ConfiguratorApp& app);

} // namespace tour
} // namespace svms::cfg

#endif
