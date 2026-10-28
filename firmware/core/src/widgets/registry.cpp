// Widget type → factory. Types missing here (pc_metrics, pc_gauge, media) render as a placeholder.
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
      {"shortcut", widgets::create_shortcut},
  };
  for (const auto &entry : FACTORIES)
    if (type == entry.type) return entry.factory;
  return nullptr;
}

}  // namespace dm
