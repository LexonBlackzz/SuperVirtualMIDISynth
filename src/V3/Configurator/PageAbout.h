#ifndef SVMS_CONFIGURATOR_PAGEABOUT_H
#define SVMS_CONFIGURATOR_PAGEABOUT_H

struct ConfigDocument;

namespace svms::cfg {

class UpdateService;

void DrawAboutPage(ConfigDocument& doc, UpdateService& updates);

} // namespace svms::cfg

#endif
