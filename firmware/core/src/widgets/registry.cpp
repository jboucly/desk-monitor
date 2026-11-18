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
      {"ha_climate", widgets::create_ha_climate}, {"ha_cover", widgets::create_ha_cover},
      {"ha_slider", widgets::create_ha_slider}, {"ha_select", widgets::create_ha_select},
      {"ha_list", widgets::create_ha_list},     {"ha_history", widgets::create_ha_history},
      {"ha_alarm", widgets::create_ha_alarm},
      {"shortcut", widgets::create_shortcut},   {"pc_metrics", widgets::create_pc_metrics},
      {"pc_gauge", widgets::create_pc_gauge},   {"media", widgets::create_media},
      {"claude_sessions", widgets::create_claude_sessions},
      {"claude_usage", widgets::create_claude_usage},
      {"claude_limits", widgets::create_claude_limits},
  };
  for (const auto &entry : FACTORIES)
    if (type == entry.type) return entry.factory;
  return nullptr;
}

}  // namespace dm
