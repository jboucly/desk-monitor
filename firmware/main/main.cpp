// Desk Monitor firmware entry point: wires the portable core (dm::) to the ESP32 platform (dm::esp32::).
#include <string>

#include "display.h"
#include "dm/api.h"
#include "dm/app.h"
#include "dm/bus.h"
#include "dm/ha.h"
#include "dm/pc.h"
#include "dm/settings.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "esp_system.h"
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
 * @description Why the chip last restarted, as a short name ("panic", "brownout", "task_wdt"…). Shown in
 * GET /api/status so that a crash can be diagnosed without a serial console.
 */
const char *reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
  }
}

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
  cJSON_AddStringToObject(status, "reset_reason", reset_reason());
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
  static dm::PcSession pc(bus, settings, [] { return esp_random(); });
  services.pc_action = [](const cJSON *action) { return pc.send_action(action); };
  services.pc_media = [](const std::string &command) { return pc.send_media(command); };
  services.pc_claude_answer = [](const std::string &id, const char *decision) {
    return pc.send_claude_answer(id, decision);
  };
  services.pc_claude_view = [](const std::string &view, const std::string &id) { return pc.send_claude_view(view, id); };
  services.wake = dm::esp32::display_wake;
  services.ha_fetch = dm::esp32::ha_fetch;

  dm::esp32::display_init();
  dm::esp32::display_configure(initial.brightness, initial.sleep_minutes);

  static dm::App app(bus, services, storage, std::string(DEFAULT_LAYOUT_START, DEFAULT_LAYOUT_END));
  app.on_entities_changed = [](const std::set<std::string> &entities) { ha.set_entities(entities); };
  app.on_templates_changed = [](const std::set<std::string> &templates) { ha.set_templates(templates); };
  if (lvgl_port_lock(0)) {
    app.start();
    lvgl_port_unlock();
  }
  dm::esp32::display_show();

  ESP_LOGI(TAG, "internal RAM after UI: %u free, largest block %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  dm::esp32::network_start(bus, initial);
  dm::esp32::ha_client_start(ha, initial);
  dm::esp32::weather_start(bus, settings);

  static dm::Api api(app, settings);
  api.status = status_json;
  api.ha_entities = dm::esp32::ha_client_entities;
  api.ha_render = dm::esp32::ha_client_render;
  dm::esp32::http_start(api, settings, pc);

  // Drops a silent PC and expires pairing codes
  esp_timer_create_args_t pc_poll = {};
  pc_poll.callback = [](void *) { pc.poll(); };
  pc_poll.name = "pc_poll";
  esp_timer_handle_t pc_timer = nullptr;
  esp_timer_create(&pc_poll, &pc_timer);
  esp_timer_start_periodic(pc_timer, 1000 * 1000);

  // Settings saved from the editor take effect without a reboot
  settings.on_change = [](unsigned changes) {
    const dm::SettingsData s = settings.get();
    dm::esp32::network_reconfigure(s, changes);
    if (changes & dm::Settings::HA) dm::esp32::ha_client_start(ha, s);
    if (changes & dm::Settings::WEATHER) dm::esp32::weather_refresh();
    if (changes & dm::Settings::PC) pc.forget();
    if (changes & dm::Settings::DISPLAY) dm::esp32::display_configure(s.brightness, s.sleep_minutes);
  };

  ESP_LOGI(TAG, "Desk Monitor %s started (last reset: %s)", esp_app_get_description()->version, reset_reason());
}
