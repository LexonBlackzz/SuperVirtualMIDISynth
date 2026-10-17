#ifndef SVMS_CONFIGURATOR_PAGESETTINGS_H
#define SVMS_CONFIGURATOR_PAGESETTINGS_H

namespace svms::cfg {

class ConfigDocument;
class UpdateService;

// Settings: look (theme, accent, motion, fire, welcome tour), the config file,
// driver logging and About.
void DrawSettingsPage(ConfigDocument& doc, UpdateService& updates);

} // namespace svms::cfg

#endif
