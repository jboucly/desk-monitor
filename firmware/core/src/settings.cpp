#include "dm/settings.h"

#include <cmath>

#include "dm/json.h"

namespace dm {

namespace {

constexpr const char *FILE_NAME = "settings.json";

/**
 * @description Copies a string member into `out` when present. Returns true when the value changed.
 */
bool merge_str(const cJSON *obj, const char *key, std::string &out) {
  const cJSON *item = json::get(obj, key);
  if (!cJSON_IsString(item) || out == item->valuestring) return false;
  out = item->valuestring;
  return true;
}

/**
 * @description Same as merge_str, but an empty string means "keep the stored secret".
 */
bool merge_secret(const cJSON *obj, const char *key, std::string &out) {
  const cJSON *item = json::get(obj, key);
  if (!cJSON_IsString(item) || !*item->valuestring) return false;
  return merge_str(obj, key, out);
}

template <typename T>
bool merge_num(const cJSON *obj, const char *key, T &out) {
  const cJSON *item = json::get(obj, key);
  if (!cJSON_IsNumber(item) || out == static_cast<T>(item->valuedouble)) return false;
  out = static_cast<T>(item->valuedouble);
  return true;
}

/**
 * @description Builds the settings tree. Secrets are included only when `with_secrets` is set.
 */
cJSON *to_json(const SettingsData &d, bool with_secrets) {
  cJSON *root = cJSON_CreateObject();
  cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
  cJSON_AddStringToObject(wifi, "ssid", d.wifi_ssid.c_str());
  cJSON *ha = cJSON_AddObjectToObject(root, "ha");
  cJSON_AddStringToObject(ha, "url", d.ha_url.c_str());
  cJSON *weather = cJSON_AddObjectToObject(root, "weather");
  cJSON_AddNumberToObject(weather, "lat", d.weather_lat);
  cJSON_AddNumberToObject(weather, "lon", d.weather_lon);
  cJSON_AddStringToObject(weather, "name", d.weather_name.c_str());
  cJSON *time = cJSON_AddObjectToObject(root, "time");
  cJSON_AddStringToObject(time, "timezone", d.timezone.c_str());
  cJSON *display = cJSON_AddObjectToObject(root, "display");
  cJSON_AddNumberToObject(display, "brightness", d.brightness);
  cJSON_AddNumberToObject(display, "sleep_minutes", d.sleep_minutes);
  cJSON *security = cJSON_AddObjectToObject(root, "security");
  if (with_secrets) {
    cJSON_AddStringToObject(wifi, "password", d.wifi_password.c_str());
    cJSON_AddStringToObject(ha, "token", d.ha_token.c_str());
    cJSON_AddStringToObject(security, "pin", d.pin.c_str());
  } else {
    cJSON_AddBoolToObject(wifi, "has_password", !d.wifi_password.empty());
    cJSON_AddBoolToObject(ha, "has_token", !d.ha_token.empty());
    cJSON_AddBoolToObject(security, "has_pin", !d.pin.empty());
  }
  return root;
}

/**
 * @description Merges a settings tree into `d`, returning the mask of changed groups.
 * @param from_file True when loading settings.json: empty secrets are kept as empty instead of ignored.
 */
unsigned merge(const cJSON *root, SettingsData &d, bool from_file) {
  unsigned changes = 0;
  const cJSON *wifi = json::get(root, "wifi");
  if (merge_str(wifi, "ssid", d.wifi_ssid)) changes |= Settings::WIFI;
  if ((from_file ? merge_str : merge_secret)(wifi, "password", d.wifi_password)) changes |= Settings::WIFI;
  const cJSON *ha = json::get(root, "ha");
  if (merge_str(ha, "url", d.ha_url)) changes |= Settings::HA;
  if ((from_file ? merge_str : merge_secret)(ha, "token", d.ha_token)) changes |= Settings::HA;
  const cJSON *weather = json::get(root, "weather");
  if (merge_num(weather, "lat", d.weather_lat)) changes |= Settings::WEATHER;
  if (merge_num(weather, "lon", d.weather_lon)) changes |= Settings::WEATHER;
  if (merge_str(weather, "name", d.weather_name)) changes |= Settings::WEATHER;
  if (merge_str(json::get(root, "time"), "timezone", d.timezone)) changes |= Settings::TIME;
  const cJSON *display = json::get(root, "display");
  if (merge_num(display, "brightness", d.brightness)) changes |= Settings::DISPLAY;
  if (merge_num(display, "sleep_minutes", d.sleep_minutes)) changes |= Settings::DISPLAY;
  // The PIN can be cleared from the API with "clear_pin": true, never with an empty string
  const cJSON *security = json::get(root, "security");
  if (from_file) {
    merge_str(security, "pin", d.pin);
  } else if (json::boolean(security, "clear_pin")) {
    d.pin.clear();
  } else {
    merge_secret(security, "pin", d.pin);
  }
  return changes;
}

}  // namespace

void Settings::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto text = storage_.read(FILE_NAME);
  if (!text) return;
  if (auto root = json::parse(*text)) merge(root.get(), data_, true);
}

SettingsData Settings::get() {
  std::lock_guard<std::mutex> lock(mutex_);
  return data_;
}

std::string Settings::to_public_json() {
  std::lock_guard<std::mutex> lock(mutex_);
  return json::print(json::own(to_json(data_, false)).get());
}

bool Settings::update(const std::string &body, std::string &error) {
  auto root = json::parse(body);
  if (!cJSON_IsObject(root.get())) {
    error = "JSON invalide";
    return false;
  }
  unsigned changes;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    SettingsData next = data_;
    changes = merge(root.get(), next, false);
    if (next.brightness < 5 || next.brightness > 100) {
      error = "luminosité hors limites (5 à 100 %)";
      return false;
    }
    if (std::fabs(next.weather_lat) > 90 || std::fabs(next.weather_lon) > 180) {
      error = "coordonnées météo invalides";
      return false;
    }
    data_ = next;
    save_locked();
  }
  if (changes && on_change) on_change(changes);
  return true;
}

void Settings::save_locked() { storage_.write(FILE_NAME, json::print(json::own(to_json(data_, true)).get())); }

}  // namespace dm
