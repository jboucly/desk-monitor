// Desk Monitor firmware entry point: wires the portable core (dm::) to the ESP32 platform (dm::esp32::).
#include <string>

#include "display.h"
#include "dm/api.h"
#include "dm/app.h"
#include "dm/bus.h"
#include "dm/ha.h"
#include "dm/settings.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "ha_client.h"
#include "http_server.h"
#include "network.h"
#include "nvs_flash.h"
#include "psram_alloc.h"
#include "storage.h"
#include "weather_task.h"

// Default layout (firmware/data/layout.json), used until the editor saves one
extern const char DEFAULT_LAYOUT_START[] asm("_binary_layout_json_start");
extern const char DEFAULT_LAYOUT_END[] asm("_binary_layout_json_end");

namespace {

const char *TAG = "main";

/**
 * @description JSON returned by GET /api/status.
 */
std::string status_json() {
  cJSON *status = cJSON_CreateObject();
  cJSON_AddStringToObject(status, "version", esp_app_get_description()->version);
  cJSON_AddStringToObject(status, "platform", "esp32s3");
  cJSON_AddStringToObject(status, "ip", dm::esp32::network_ip().c_str());
  cJSON_AddNumberToObject(status, "rssi", dm::esp32::network_rssi());
  cJSON_AddNumberToObject(status, "uptime", static_cast<double>(esp_timer_get_time() / 1000000));
  cJSON_AddNumberToObject(status, "heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(status, "heap_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  std::string out = dm::json::print(status);
  cJSON_Delete(status);
  return out;
}

}  // namespace

extern "C" void app_main(void) {
  dm::esp32::psram_alloc_init();

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  // Long-lived objects: app_main returns, they must not live on its stack
  static dm::esp32::LittleFsStorage storage;
  static dm::Settings settings(storage);
  settings.load();
  const dm::SettingsData initial = settings.get();

  static dm::Bus bus;
  static dm::HaSession ha(bus);
  static dm::Services services;
  services.ha_call = [](const std::string &domain, const std::string &service, const std::string &entity_id,
                        cJSON *data) { ha.call_service(domain, service, entity_id, data); };
  // services.pc_action arrives with the BLE companion (phase 2)

  dm::esp32::display_init();
  dm::esp32::display_configure(initial.brightness, initial.sleep_minutes);

  static dm::App app(bus, services, storage, std::string(DEFAULT_LAYOUT_START, DEFAULT_LAYOUT_END));
  app.on_entities_changed = [](const std::set<std::string> &entities) { ha.set_entities(entities); };
  if (lvgl_port_lock(0)) {
    app.start();
    lvgl_port_unlock();
  }

  ESP_LOGI(TAG, "internal RAM after UI: %u free, largest block %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  dm::esp32::network_start(bus, initial);
  dm::esp32::ha_client_start(ha, initial);
  dm::esp32::weather_start(bus, settings);

  static dm::Api api(app, settings);
  api.status = status_json;
  api.ha_entities = dm::esp32::ha_client_entities;
  dm::esp32::http_start(api, settings);

  // Settings saved from the editor take effect without a reboot
  settings.on_change = [](unsigned changes) {
    const dm::SettingsData s = settings.get();
    dm::esp32::network_reconfigure(s, changes);
    if (changes & dm::Settings::HA) dm::esp32::ha_client_start(ha, s);
    if (changes & dm::Settings::WEATHER) dm::esp32::weather_refresh();
    if (changes & dm::Settings::DISPLAY) dm::esp32::display_configure(s.brightness, s.sleep_minutes);
  };

  ESP_LOGI(TAG, "Desk Monitor %s started", esp_app_get_description()->version);
}
