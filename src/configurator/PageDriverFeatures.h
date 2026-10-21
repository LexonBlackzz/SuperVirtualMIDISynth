#ifndef SVMS_CONFIGURATOR_PAGEDRIVERFEATURES_H
#define SVMS_CONFIGURATOR_PAGEDRIVERFEATURES_H

namespace svms { class EngineLink; }

namespace svms::cfg {

// Linux (Control API) only: the driver advertises its own parameters, so a
// driver newer than this configurator can still be driven. Everything the
// other pages already have widgets for is left to them; the rest shows here
// under "unknown by this configurator version".
void DrawDriverFeaturesPage(svms::EngineLink* client);

} // namespace svms::cfg

#endif
