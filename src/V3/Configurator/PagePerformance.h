#ifndef SVMS_CONFIGURATOR_PAGEPERFORMANCE_H
#define SVMS_CONFIGURATOR_PAGEPERFORMANCE_H

namespace svms::cfg {

class ConfigDocument;
struct ConfigValues;
void DrawPerformancePage(ConfigDocument& doc);

// Profiles (VISION section 3): 0 Accurate, 1 Balanced, 2 Extreme. The match
// returns -1 ("Custom") when the shortcut switches fit none of them. Applying
// one pushes every changed switch live exactly like its own toggle does.
int MatchPerformanceProfile(const ConfigValues& w);
void ApplyPerformanceProfile(ConfigDocument& doc, int profile);

} // namespace svms::cfg

#endif
