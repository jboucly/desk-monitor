// Widget type → factory. A type missing here renders as a placeholder card.
#include "common.h"

namespace dm {

WidgetFactory find_widget_factory(const std::string &type) {
  static const struct {
    const char *type;
    WidgetFactory factory;
  } FACTORIES[] = {
      {"clock", widgets::create_clock},         {"weather", widgets::create_weather},
      {"ha_toggle", widgets::create_ha_toggle}, {"ha_light", widgets::create_ha_light},
      {"ha_sensor", widgets::create_ha_sensor}, {"ha_gauge", widgets::create_ha_gauge},
      {"shortcut", widgets::create_shortcut},   {"pc_metrics", widgets::create_pc_metrics},
      {"pc_gauge", widgets::create_pc_gauge},   {"media", widgets::create_media},
  };
  for (const auto &entry : FACTORIES)
    if (type == entry.type) return entry.factory;
  return nullptr;
}

}  // namespace dm
