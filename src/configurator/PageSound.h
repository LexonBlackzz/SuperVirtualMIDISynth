#ifndef SVMS_CONFIGURATOR_PAGESOUND_H
#define SVMS_CONFIGURATOR_PAGESOUND_H

namespace svms::cfg {

class ConfigDocument;

// Sound: SoundFont list, velocity, voice character, tuning, routes.
void DrawSoundPage(ConfigDocument& doc);

// Adds any .sf2 / .sfz files dropped on the window to the SoundFont list
// (works from every page). Returns how many were added.
int HandleDroppedSoundFonts(ConfigDocument& doc);

} // namespace svms::cfg

#endif
