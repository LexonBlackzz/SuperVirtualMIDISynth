#ifndef SVMS_CONFIGURATOR_PAGEEFFECTS_H
#define SVMS_CONFIGURATOR_PAGEEFFECTS_H

#include <string>

namespace svms::cfg {

class ConfigDocument;

// Effects: the signal chain (Synth, Channel limiter, Reverb, Master limiter,
// Output) and the panel of the selected block. `panel` is "reverb", "limiter"
// or "chlim"; clicking a chain block changes it.
void DrawEffectsPage(ConfigDocument& doc, std::string& panel);

} // namespace svms::cfg

#endif
