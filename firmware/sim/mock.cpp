#include "mock.h"

#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <thread>

#include "dm/ha.h"
#include "dm/weather.h"
#include "lvgl.h"

namespace sim {

namespace {

/**
 * @description Stable pseudo-random number in [0, 1) derived from a string (same entity, same look).
 */
float hash01(const std::string &text) {
  uint32_t h = 2166136261u;
  for (char c : text) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  return (h % 10000) / 10000.0f;
}

void set_number(cJSON *obj, const char *key, double value) {
  cJSON_DeleteItemFromObject(obj, key);
  cJSON_AddNumberToObject(obj, key, value);
}

void set_string(cJSON *obj, const char *key, const char *value) {
  cJSON_DeleteItemFromObject(obj, key);
  cJSON_AddStringToObject(obj, key, value);
}

/**
 * @description Recomputes rgb_color from hs_color or color_temp_kelvin, like HA does.
 */
void update_rgb(cJSON *attrs) {
  lv_color_t c;
  const cJSON *hs = cJSON_GetObjectItem(attrs, "hs_color");
  if (!strcmp(dm::json::str(attrs, "color_mode"), "hs") && cJSON_GetArraySize(hs) == 2) {
    c = lv_color_hsv_to_rgb(static_cast<uint16_t>(cJSON_GetArrayItem(hs, 0)->valuedouble),
                            static_cast<uint8_t>(cJSON_GetArrayItem(hs, 1)->valuedouble), 100);
  } else {
    // Rough warm-to-cool white
    const float t = (dm::json::num(attrs, "color_temp_kelvin", 3000) - 2000) / 4500.0f;
    c = lv_color_make(255, static_cast<uint8_t>(180 + 70 * t), static_cast<uint8_t>(110 + 140 * t));
  }
  cJSON_DeleteItemFromObject(attrs, "rgb_color");
  cJSON *rgb = cJSON_AddArrayToObject(attrs, "rgb_color");
  cJSON_AddItemToArray(rgb, cJSON_CreateNumber(c.red));
  cJSON_AddItemToArray(rgb, cJSON_CreateNumber(c.green));
  cJSON_AddItemToArray(rgb, cJSON_CreateNumber(c.blue));
}

/**
 * @description Adds a list of strings to an object.
 */
void add_strings(cJSON *obj, const char *key, std::initializer_list<const char *> values) {
  cJSON *list = cJSON_AddArrayToObject(obj, key);
  for (const char *v : values) cJSON_AddItemToArray(list, cJSON_CreateString(v));
}

/**
 * @description Recomputes hvac_action from the mode and the temperatures, like a real thermostat reports it.
 */
void update_hvac_action(cJSON *state) {
  cJSON *attrs = cJSON_GetObjectItem(state, "a");
  const std::string mode = dm::json::str(state, "s");
  const double current = dm::json::num(attrs, "current_temperature");
  const double target = dm::json::num(attrs, "temperature", current);
  const char *action = "idle";
  if (mode == "off") action = "off";
  else if ((mode == "heat" || mode == "auto") && current < target - 0.2) action = "heating";
  else if ((mode == "cool" || mode == "auto") && current > target + 0.2) action = "cooling";
  else if (mode == "dry") action = "drying";
  else if (mode == "fan_only") action = "fan";
  set_string(attrs, "hvac_action", action);
}

/**
 * @description Initial mock state of an entity, chosen from its domain and name.
 */
cJSON *initial_state(const std::string &id) {
  const float r = hash01(id);
  const std::string domain = id.substr(0, id.find('.'));
  cJSON *state = cJSON_CreateObject();
  cJSON *attrs = cJSON_AddObjectToObject(state, "a");
  std::string friendly = id.substr(id.find('.') + 1);
  for (char &c : friendly)
    if (c == '_') c = ' ';
  cJSON_AddStringToObject(attrs, "friendly_name", friendly.c_str());

  if (domain == "light") {
    const bool on = r < 0.65f;
    cJSON_AddStringToObject(state, "s", on ? "on" : "off");
    cJSON *modes = cJSON_AddArrayToObject(attrs, "supported_color_modes");
    cJSON_AddItemToArray(modes, cJSON_CreateString("color_temp"));
    if (r < 0.5f) cJSON_AddItemToArray(modes, cJSON_CreateString("hs"));
    cJSON_AddNumberToObject(attrs, "min_color_temp_kelvin", 2200);
    cJSON_AddNumberToObject(attrs, "max_color_temp_kelvin", 6500);
    if (on) {
      const bool colored = r < 0.25f;
      cJSON_AddNumberToObject(attrs, "brightness", 80 + static_cast<int>(r * 175));
      cJSON_AddStringToObject(attrs, "color_mode", colored ? "hs" : "color_temp");
      cJSON_AddNumberToObject(attrs, "color_temp_kelvin", 2700 + static_cast<int>(r * 1500));
      cJSON *hs = cJSON_AddArrayToObject(attrs, "hs_color");
      cJSON_AddItemToArray(hs, cJSON_CreateNumber(static_cast<int>(r * 1440) % 360));
      cJSON_AddItemToArray(hs, cJSON_CreateNumber(70));
      update_rgb(attrs);
    }
  } else if (domain == "media_player") {
    // A speaker playing, with a cover served by the simulator (sim/assets/cover.jpg)
    cJSON_AddStringToObject(state, "s", "playing");
    cJSON_AddStringToObject(attrs, "media_title", "Clair de lune");
    cJSON_AddStringToObject(attrs, "media_artist", "Claude Debussy");
    cJSON_AddStringToObject(attrs, "app_name", "Sonos");
    cJSON_AddNumberToObject(attrs, "media_duration", 302);
    cJSON_AddNumberToObject(attrs, "media_position", 42);
    char now[32];
    const time_t t = time(nullptr);
    strftime(now, sizeof now, "%Y-%m-%dT%H:%M:%S+00:00", gmtime(&t));
    cJSON_AddStringToObject(attrs, "media_position_updated_at", now);
    cJSON_AddStringToObject(attrs, "entity_picture", "/mock/cover.jpg");
    cJSON_AddNumberToObject(attrs, "volume_level", 0.35);
  } else if (domain == "sensor" && (id.find("fonctionnement") != std::string::npos || id.find("uptime") != std::string::npos)) {
    // Like Proxmox VE's uptime: the boot time, as a timestamp
    char boot[32];
    const time_t at = time(nullptr) - 12 * 86400 - 4 * 3600 - 1500;
    strftime(boot, sizeof boot, "%Y-%m-%dT%H:%M:%S+00:00", gmtime(&at));
    cJSON_AddStringToObject(state, "s", boot);
    cJSON_AddStringToObject(attrs, "device_class", "timestamp");
  } else if (domain == "sensor") {
    char value[16];
    snprintf(value, sizeof value, "%.1f", 15 + r * 75);
    cJSON_AddStringToObject(state, "s", value);
    cJSON_AddStringToObject(attrs, "unit_of_measurement", "%");
  } else if (domain == "climate") {
    // A heating thermostat; names with "clim" get an air conditioner with cooling and fan modes
    const bool ac = id.find("clim", id.find('.')) != std::string::npos;
    cJSON_AddStringToObject(state, "s", ac ? "cool" : "heat");
    add_strings(attrs, "hvac_modes", ac ? std::initializer_list<const char *>{"off", "heat", "cool", "auto", "dry", "fan_only"}
                                        : std::initializer_list<const char *>{"off", "heat", "auto"});
    cJSON_AddNumberToObject(attrs, "min_temp", ac ? 16 : 7);
    cJSON_AddNumberToObject(attrs, "max_temp", ac ? 30 : 28);
    cJSON_AddNumberToObject(attrs, "target_temp_step", 0.5);
    cJSON_AddNumberToObject(attrs, "current_temperature", ac ? 26.5 : 19.5);
    cJSON_AddNumberToObject(attrs, "temperature", ac ? 24 : 21);
    add_strings(attrs, "preset_modes", {"none", "eco", "comfort", "away", "boost"});
    cJSON_AddStringToObject(attrs, "preset_mode", "comfort");
    if (ac) {
      add_strings(attrs, "fan_modes", {"auto", "low", "medium", "high"});
      cJSON_AddStringToObject(attrs, "fan_mode", "auto");
    }
    update_hvac_action(state);
  } else if (domain == "cover") {
    // Shutters ("volet") have a position; names with "store" also tilt
    const bool tilt = id.find("store", id.find('.')) != std::string::npos;
    const int position = static_cast<int>(r * 10) * 10;
    cJSON_AddStringToObject(state, "s", position > 0 ? "open" : "closed");
    cJSON_AddNumberToObject(attrs, "current_position", position);
    cJSON_AddNumberToObject(attrs, "supported_features", 15 | (tilt ? 16 | 32 | 64 | 128 : 0));
    if (tilt) cJSON_AddNumberToObject(attrs, "current_tilt_position", 50);
    if (id.find("volet", id.find('.')) != std::string::npos) cJSON_AddStringToObject(attrs, "device_class", "shutter");
  } else if (domain == "input_number" || domain == "number") {
    const bool temp = id.find("temp", id.find('.')) != std::string::npos;
    cJSON_AddStringToObject(state, "s", temp ? "19.5" : "40.0");
    cJSON_AddNumberToObject(attrs, "min", temp ? 15 : 0);
    cJSON_AddNumberToObject(attrs, "max", temp ? 25 : 100);
    cJSON_AddNumberToObject(attrs, "step", temp ? 0.5 : 1);
    cJSON_AddStringToObject(attrs, "unit_of_measurement", temp ? "°C" : "%");
  } else if (domain == "input_select" || domain == "select") {
    cJSON_AddStringToObject(state, "s", "Maison");
    add_strings(attrs, "options", {"Maison", "Absent", "Nuit", "Vacances", "Télétravail"});
  } else if (domain == "fan") {
    cJSON_AddStringToObject(state, "s", "on");
    cJSON_AddNumberToObject(attrs, "percentage", 50);
    cJSON_AddNumberToObject(attrs, "percentage_step", 25);
    add_strings(attrs, "preset_modes", {"auto", "sleep", "nature"});
    cJSON_AddNullToObject(attrs, "preset_mode");
  } else if (domain == "water_heater") {
    cJSON_AddStringToObject(state, "s", "eco");
    add_strings(attrs, "operation_list", {"off", "eco", "electric", "performance"});
    cJSON_AddStringToObject(attrs, "operation_mode", "eco");
    cJSON_AddNumberToObject(attrs, "min_temp", 40);
    cJSON_AddNumberToObject(attrs, "max_temp", 65);
    cJSON_AddNumberToObject(attrs, "current_temperature", 52);
    cJSON_AddNumberToObject(attrs, "temperature", 55);
  } else {
    cJSON_AddStringToObject(state, "s", r < 0.5f ? "on" : "off");
  }
  return state;
}

size_t append_body(char *data, size_t size, size_t count, void *user) {
  static_cast<std::string *>(user)->append(data, size * count);
  return size * count;
}

const char *CANNED_WEATHER = R"({"current":{"temperature_2m":13.4,"apparent_temperature":11.2,"relative_humidity_2m":72,
"weather_code":2,"is_day":1,"wind_speed_10m":14.3},"daily":{"time":["2026-10-28","2026-10-29","2026-10-30","2026-10-31",
"2026-11-01","2026-11-02","2026-11-03"],"weather_code":[2,61,3,0,80,95,71],"temperature_2m_max":[15.1,13.2,12.8,14.9,11.7,10.2,6.5],
"temperature_2m_min":[8.2,9.1,7.4,5.9,6.3,4.8,1.2],"precipitation_probability_max":[10,80,30,0,60,90,70]}})";

}  // namespace

void MockHome::set_entities(const std::set<std::string> &entities) {
  for (const auto &id : entities) {
    // An entity may already exist without having been published (created by a template or a history request)
    if (!states_.count(id)) states_[id] = dm::json::own(initial_state(id));
    publish(id);
  }
}

void MockHome::call(const std::string &domain, const std::string &service, const std::string &entity_id, cJSON *data) {
  auto owned = dm::json::own(data);
  auto it = states_.find(entity_id);
  if (it == states_.end()) return;
  cJSON *state = it->second.get();
  cJSON *attrs = cJSON_GetObjectItem(state, "a");
  const bool on = !strcmp(dm::json::str(state, "s"), "on");

  if (domain == "media_player") {
    const bool playing = !strcmp(dm::json::str(state, "s"), "playing");
    if (service == "media_play_pause") set_string(state, "s", playing ? "paused" : "playing");
    if (service == "volume_set") set_number(attrs, "volume_level", dm::json::num(data, "volume_level"));
    printf("[mock HA] media_player.%s %s\n", service.c_str(), entity_id.c_str());
    publish(entity_id);
    return;
  }

  auto log_and_publish = [&] {
    printf("[mock HA] %s.%s %s %s\n", domain.c_str(), service.c_str(), entity_id.c_str(),
           data ? dm::json::print(data).c_str() : "");
    publish(entity_id);
  };
  if (domain == "cover") {
    double position = dm::json::num(attrs, "current_position");
    if (service == "open_cover") position = 100;
    if (service == "close_cover") position = 0;
    if (service == "toggle") position = !strcmp(dm::json::str(state, "s"), "closed") ? 100 : 0;
    if (service == "set_cover_position") position = dm::json::num(data, "position");
    if (service == "set_cover_tilt_position") set_number(attrs, "current_tilt_position", dm::json::num(data, "tilt_position"));
    set_number(attrs, "current_position", position);
    set_string(state, "s", position > 0 ? "open" : "closed");
    return log_and_publish();
  }
  if (domain == "input_number" || domain == "number") {
    char value[16];
    snprintf(value, sizeof value, "%.1f", dm::json::num(data, "value"));
    set_string(state, "s", value);
    return log_and_publish();
  }
  if (domain == "input_select" || domain == "select") {
    set_string(state, "s", dm::json::str(data, "option"));
    return log_and_publish();
  }
  if (domain == "fan" && (service == "set_percentage" || service == "set_preset_mode")) {
    if (service == "set_percentage") {
      const double pct = dm::json::num(data, "percentage");
      set_number(attrs, "percentage", pct);
      set_string(state, "s", pct > 0 ? "on" : "off");
    } else {
      set_string(attrs, "preset_mode", dm::json::str(data, "preset_mode"));
      set_string(state, "s", "on");
    }
    return log_and_publish();
  }
  if (domain == "climate" || domain == "water_heater") {
    const bool water = domain == "water_heater";
    if (service == "turn_off") set_string(state, "s", "off");
    if (service == "turn_on") set_string(state, "s", water ? "eco" : "heat");
    if (service == "set_hvac_mode") set_string(state, "s", dm::json::str(data, "hvac_mode", "heat"));
    if (service == "set_operation_mode") {
      set_string(state, "s", dm::json::str(data, "operation_mode", "eco"));
      set_string(attrs, "operation_mode", dm::json::str(state, "s"));
    }
    if (service == "set_preset_mode") set_string(attrs, "preset_mode", dm::json::str(data, "preset_mode", "none"));
    if (service == "set_fan_mode") set_string(attrs, "fan_mode", dm::json::str(data, "fan_mode", "auto"));
    if (service == "set_temperature") {
      for (const char *key : {"temperature", "target_temp_low", "target_temp_high"})
        if (const cJSON *v = cJSON_GetObjectItem(data, key); cJSON_IsNumber(v)) set_number(attrs, key, v->valuedouble);
    }
    if (!water) update_hvac_action(state);
    printf("[mock HA] %s.%s %s %s\n", domain.c_str(), service.c_str(), entity_id.c_str(),
           data ? dm::json::print(data).c_str() : "");
    publish(entity_id);
    return;
  }

  bool turn_on = service == "turn_on" || (service == "toggle" && !on);
  if (service == "turn_off" || (service == "toggle" && on)) turn_on = false;
  if (dm::json::num(data, "brightness_pct", 1) == 0) turn_on = false;
  set_string(state, "s", turn_on ? "on" : "off");

  if (domain == "light" && turn_on) {
    if (!cJSON_HasObjectItem(attrs, "brightness")) set_number(attrs, "brightness", 200);
    if (!cJSON_HasObjectItem(attrs, "color_mode")) set_string(attrs, "color_mode", "color_temp");
    if (!cJSON_HasObjectItem(attrs, "color_temp_kelvin")) set_number(attrs, "color_temp_kelvin", 3000);
    if (const cJSON *pct = cJSON_GetObjectItem(data, "brightness_pct"))
      set_number(attrs, "brightness", std::round(pct->valuedouble * 2.55));
    if (const cJSON *k = cJSON_GetObjectItem(data, "color_temp_kelvin")) {
      set_number(attrs, "color_temp_kelvin", k->valuedouble);
      set_string(attrs, "color_mode", "color_temp");
    }
    if (const cJSON *hs = cJSON_GetObjectItem(data, "hs_color")) {
      cJSON_DeleteItemFromObject(attrs, "hs_color");
      cJSON_AddItemToObject(attrs, "hs_color", cJSON_Duplicate(hs, true));
      set_string(attrs, "color_mode", "hs");
    }
    update_rgb(attrs);
  }
  printf("[mock HA] %s.%s %s %s\n", domain.c_str(), service.c_str(), entity_id.c_str(),
         data ? dm::json::print(data).c_str() : "");
  publish(entity_id);
}

std::string MockHome::entity_list_json() const {
  cJSON *list = cJSON_CreateArray();
  const char *extra[] = {"light.cuisine", "switch.imprimante_3d", "sensor.temperature_salon", "scene.soiree",
                         "fan.ventilateur_bureau", "climate.chauffage_bureau"};
  for (const auto &[id, state] : states_) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", id.c_str());
    cJSON_AddStringToObject(item, "name", dm::json::str(dm::json::get(state.get(), "a"), "friendly_name"));
    cJSON_AddItemToArray(list, item);
  }
  for (const char *id : extra) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", id);
    cJSON_AddStringToObject(item, "name", "");
    cJSON_AddItemToArray(list, item);
  }
  std::string out = dm::json::print(list);
  cJSON_Delete(list);
  return out;
}

void MockHome::tick() {
  for (auto &[id, state] : states_) {
    if (id.rfind("climate.", 0) == 0) {
      // The room slowly moves towards the target while the thermostat runs
      cJSON *attrs = cJSON_GetObjectItem(state.get(), "a");
      const char *action = dm::json::str(attrs, "hvac_action");
      const double current = dm::json::num(attrs, "current_temperature");
      const double delta = !strcmp(action, "heating") ? 0.1 : !strcmp(action, "cooling") ? -0.1 : 0;
      if (delta == 0) continue;
      set_number(attrs, "current_temperature", std::round((current + delta) * 10) / 10);
      update_hvac_action(state.get());
      publish(id);
      continue;
    }
    if (id.rfind("sensor.", 0) != 0) continue;
    seed_ = seed_ * 1103515245u + 12345u;
    const double delta = ((seed_ >> 16) % 600) / 100.0 - 3.0;
    const double value = std::clamp(atof(dm::json::str(state.get(), "s")) + delta, 1.0, 99.0);
    char buf[16];
    snprintf(buf, sizeof buf, "%.1f", value);
    set_string(state.get(), "s", buf);
    publish(id);
  }
}

void MockHome::publish(const std::string &entity_id) {
  bus_.post("ha/" + entity_id, cJSON_Duplicate(states_[entity_id].get(), true));
  publish_templates();
}

void MockHome::set_templates(const std::set<std::string> &templates) {
  templates_ = templates;
  publish_templates();
}

void MockHome::publish_templates() {
  for (const auto &tpl : templates_) {
    std::string out;
    cJSON *state = cJSON_CreateObject();
    if (render(tpl, out)) {
      cJSON_AddStringToObject(state, "s", out.c_str());
    } else {
      cJSON_AddStringToObject(state, "s", "unavailable");
      cJSON_AddStringToObject(state, "error", out.c_str());
    }
    cJSON_AddObjectToObject(state, "a");
    bus_.post("ha_tpl/" + dm::HaSession::template_key(tpl), state);
  }
}

void MockHome::request_history(const std::string &entity_id, int hours) {
  const double now = static_cast<double>(time(nullptr)), t0 = now - hours * 3600.0;
  const double base = atof(dm::json::str(state_of(entity_id), "s"));
  // One change every few minutes, like a real sensor: 200 changes over the period
  cJSON *changes = cJSON_CreateArray();
  for (int i = 0; i < 200; i++) {
    const double at = t0 + (now - t0) * i / 200.0;
    seed_ = seed_ * 1103515245u + 12345u;
    const double noise = ((seed_ >> 16) % 100) / 100.0 - 0.5;
    const double value = base + 2.5 * std::sin((at / 86400.0) * 2 * M_PI) + noise;
    cJSON *change = cJSON_CreateObject();
    char text[16];
    snprintf(text, sizeof text, "%.1f", value);
    cJSON_AddStringToObject(change, "s", text);
    cJSON_AddNumberToObject(change, "lu", at);
    cJSON_AddItemToArray(changes, change);
  }
  bus_.post("ha_hist/" + entity_id + "/" + std::to_string(hours),
            dm::HaSession::history_series(changes, t0, now, dm::HaSession::HISTORY_POINTS));
  cJSON_Delete(changes);
}

const cJSON *MockHome::state_of(const std::string &entity_id) {
  auto it = states_.find(entity_id);
  if (it == states_.end()) it = states_.emplace(entity_id, dm::json::own(initial_state(entity_id))).first;
  return it->second.get();
}

bool MockHome::render(const std::string &tpl, std::string &out) {
  out.clear();
  // Text of the first quoted argument after `from`
  auto quoted = [](const std::string &expr, size_t from, size_t &end) -> std::string {
    const size_t open = expr.find_first_of("'\"", from);
    if (open == std::string::npos) return {};
    const size_t close = expr.find(expr[open], open + 1);
    if (close == std::string::npos) return {};
    end = close + 1;
    return expr.substr(open + 1, close - open - 1);
  };
  size_t pos = 0;
  while (pos < tpl.size()) {
    const size_t expr_open = tpl.find("{{", pos), block_open = tpl.find("{%", pos);
    const size_t open = std::min(expr_open, block_open);
    if (open == std::string::npos) {
      out += tpl.substr(pos);
      break;
    }
    out += tpl.substr(pos, open - pos);
    const bool block = open == block_open;
    const size_t close = tpl.find(block ? "%}" : "}}", open + 2);
    if (close == std::string::npos) {
      out = "TemplateSyntaxError: unexpected end of template (simulateur)";
      return false;
    }
    if (!block) {
      const std::string expr = tpl.substr(open + 2, close - open - 2);
      size_t end = 0;
      if (expr.find("state_attr(") != std::string::npos) {
        const std::string id = quoted(expr, expr.find("state_attr("), end);
        const std::string attr = quoted(expr, end, end);
        const cJSON *value = cJSON_GetObjectItem(cJSON_GetObjectItem(state_of(id), "a"), attr.c_str());
        out += cJSON_IsString(value) ? value->valuestring : value ? dm::json::print(value) : "None";
      } else if (expr.find("states(") != std::string::npos) {
        out += dm::json::str(state_of(quoted(expr, expr.find("states("), end)), "s");
      } else {
        // Anything else is shown as typed, trimmed
        const size_t a = expr.find_first_not_of(' '), b = expr.find_last_not_of(' ');
        if (a != std::string::npos) out += expr.substr(a, b - a + 1);
      }
    }
    pos = close + 2;
  }
  return true;
}

void fetch_weather(dm::Bus &bus, double lat, double lon, const std::string &name) {
  std::thread([&bus, lat, lon, name] {
    std::string body;
    CURL *curl = curl_easy_init();
    if (curl) {
      curl_easy_setopt(curl, CURLOPT_URL, dm::weather::url(lat, lon).c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
      if (curl_easy_perform(curl) != CURLE_OK) body.clear();
      curl_easy_cleanup(curl);
    }
    cJSON *weather = dm::weather::parse(body, name);
    if (!weather) {
      printf("[weather] Open-Meteo unreachable, using canned data\n");
      weather = dm::weather::parse(CANNED_WEATHER, name + " (hors ligne)");
    }
    bus.post("weather", weather);
  }).detach();
}

}  // namespace sim
