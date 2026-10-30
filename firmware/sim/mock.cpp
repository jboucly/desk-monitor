#include "mock.h"

#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <thread>

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
    if (states_.count(id)) continue;
    states_[id] = dm::json::own(initial_state(id));
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
    printf("[mock HA] media_player.%s %s\n", service.c_str(), entity_id.c_str());
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
                         "fan.ventilateur_bureau"};
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
