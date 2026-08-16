#ifndef SVMS_CONFIGURATOR_EASTEREGGS_H
#define SVMS_CONFIGURATOR_EASTEREGGS_H

#include <string>

namespace svms::cfg {

struct EasterEggState {
    bool megaFuckerDac = false;
    std::string realDeviceName;
};

EasterEggState RollEasterEggs(int argc, char** argv);
void ShowMegaFuckerNotification(const EasterEggState& state, bool* open);

} // namespace svms::cfg

#endif
