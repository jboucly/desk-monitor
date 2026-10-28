#include "common.h"

#include <cstring>

#include "dm/util.h"

namespace dm::widgets {

std::string entity_domain(const std::string &entity_id) { return entity_id.substr(0, entity_id.find('.')); }

const char *default_entity_icon(const std::string &entity_id) {
  const std::string domain = entity_domain(entity_id);
  if (domain == "light") return "lightbulb";
  if (domain == "switch") return "power-socket-eu";
  if (domain == "fan") return "fan";
  if (domain == "climate") return "thermometer";
  if (domain == "cover") return "blinds";
  if (domain == "lock") return "lock";
  if (domain == "media_player") return "speaker";
  if (domain == "scene") return "palette";
  if (domain == "script" || domain == "automation") return "rocket-launch";
  if (domain == "input_boolean") return "toggle-switch";
  if (domain == "button" || domain == "input_button") return "power";
  if (domain == "camera") return "camera";
  if (domain == "sensor") return "gauge";
  return "home-assistant";
}

std::string entity_name(const cJSON *props, const cJSON *state, const std::string &entity_id) {
  if (const char *name = json::str(props, "name"); *name) return name;
  if (const char *name = json::str(json::get(state, "a"), "friendly_name"); *name) return name;
  return entity_id;
}

bool is_unavailable(const cJSON *state) {
  const char *s = json::str(state, "s", "unavailable");
  return !strcmp(s, "unavailable") || !strcmp(s, "unknown");
}

bool is_active(const cJSON *state) {
  const char *s = json::str(state, "s");
  for (const char *on : {"on", "open", "opening", "playing", "unlocked", "home", "heat", "cool", "heat_cool", "auto"})
    if (!strcmp(s, on)) return true;
  return false;
}

void toggle_entity(Services &services, const std::string &entity_id, const cJSON *state) {
  if (!services.ha_call) return;
  const std::string domain = entity_domain(entity_id);
  if (domain == "scene" || domain == "script") return services.ha_call(domain, "turn_on", entity_id, nullptr);
  if (domain == "button" || domain == "input_button") return services.ha_call(domain, "press", entity_id, nullptr);
  if (domain == "lock")
    return services.ha_call(domain, !strcmp(json::str(state, "s"), "locked") ? "unlock" : "lock", entity_id, nullptr);
  if (domain == "media_player") return services.ha_call(domain, "media_play_pause", entity_id, nullptr);
  if (domain == "light" || domain == "switch" || domain == "fan" || domain == "cover" || domain == "input_boolean" ||
      domain == "automation" || domain == "siren" || domain == "humidifier")
    return services.ha_call(domain, "toggle", entity_id, nullptr);
  services.ha_call("homeassistant", "toggle", entity_id, nullptr);
}

lv_color_t light_color(const cJSON *state) {
  const cJSON *attrs = json::get(state, "a");
  const cJSON *rgb = json::get(attrs, "rgb_color");
  const char *mode = json::str(attrs, "color_mode");
  if (strcmp(mode, "color_temp") != 0 && cJSON_GetArraySize(rgb) == 3) {
    return lv_color_make(cJSON_GetArrayItem(rgb, 0)->valueint, cJSON_GetArrayItem(rgb, 1)->valueint,
                         cJSON_GetArrayItem(rgb, 2)->valueint);
  }
  const double kelvin = json::num(attrs, "color_temp_kelvin", 0);
  if (kelvin > 0) return util::kelvin_to_rgb(static_cast<float>(kelvin));
  return theme::warm();
}

}  // namespace dm::widgets
