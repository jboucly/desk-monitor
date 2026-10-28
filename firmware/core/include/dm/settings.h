// Device settings (Wi-Fi, Home Assistant, weather, display), stored in settings.json.
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "dm/services.h"

namespace dm {

/**
 * @description Plain copy of every setting.
 */
struct SettingsData {
  std::string wifi_ssid;
  std::string wifi_password;
  std::string ha_url;  // e.g. "ws://homeassistant.local:8123/api/websocket"
  std::string ha_token;
  double weather_lat = 48.857;
  double weather_lon = 2.352;
  std::string weather_name = "Paris";
  std::string timezone = "CET-1CEST,M3.5.0,M10.5.0/3";
  int brightness = 100;   // backlight, percent
  int sleep_minutes = 5;  // backlight off after inactivity, 0 = never
  std::string pin;        // protects write API calls when not empty
};

/**
 * @description Thread-safe settings store. Secrets never leave through to_public_json().
 */
class Settings {
 public:
  // Which group of settings changed in an update (bit mask)
  enum Change : unsigned { WIFI = 1, HA = 2, WEATHER = 4, DISPLAY = 8, TIME = 16 };

  explicit Settings(Storage &storage) : storage_(storage) {}

  /**
   * @description Loads settings.json, keeping the defaults for missing fields.
   */
  void load();

  /**
   * @description Returns a copy of the current settings.
   */
  SettingsData get();

  /**
   * @description JSON for the web editor. Passwords and tokens are replaced by "has_*" flags.
   */
  std::string to_public_json();

  /**
   * @description Merges a partial update from the web editor and saves it.
   * Empty secret fields keep the stored value.
   * @param body JSON object with the same shape as to_public_json().
   * @param error Receives a French message on failure.
   * @returns True when the update was applied.
   */
  bool update(const std::string &body, std::string &error);

  /**
   * @description Called after a successful update with the mask of changed groups.
   */
  std::function<void(unsigned changes)> on_change;

 private:
  void save_locked();

  Storage &storage_;
  std::mutex mutex_;
  SettingsData data_;
};

}  // namespace dm
