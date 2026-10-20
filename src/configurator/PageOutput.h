#ifndef SVMS_CONFIGURATOR_PAGEOUTPUT_H
#define SVMS_CONFIGURATOR_PAGEOUTPUT_H

namespace svms::cfg {

class ConfigDocument;
struct EasterEggState;

// Output: audio device and driver type, latency, and routing MIDI to a
// different synth.
void DrawOutputPage(ConfigDocument& doc, const EasterEggState& eggs);

} // namespace svms::cfg

#endif
