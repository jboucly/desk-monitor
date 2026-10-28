#include "weather_task.h"

#include <string>

#include "dm/weather.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"

namespace dm::esp32 {

namespace {

const char *TAG = "weather";
TaskHandle_t g_task = nullptr;

esp_err_t collect(esp_http_client_event_t *event) {
  if (event->event_id == HTTP_EVENT_ON_DATA)
    static_cast<std::string *>(event->user_data)->append(static_cast<const char *>(event->data), event->data_len);
  return ESP_OK;
}

/**
 * @description Downloads and parses the forecast. Returns false on any network or parse error.
 */
bool fetch(Bus &bus, const SettingsData &s) {
  const std::string url = weather::url(s.weather_lat, s.weather_lon);
  std::string body;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = collect;
  config.user_data = &body;
  config.timeout_ms = 10000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK || status != 200) {
    ESP_LOGW(TAG, "fetch failed: %s, HTTP %d", esp_err_to_name(err), status);
    return false;
  }
  cJSON *weather = weather::parse(body, s.weather_name);
  if (!weather) {
    ESP_LOGW(TAG, "unexpected response");
    return false;
  }
  bus.post("weather", weather);
  return true;
}

struct TaskArgs {
  Bus *bus;
  Settings *settings;
};

void task(void *arg) {
  auto *args = static_cast<TaskArgs *>(arg);
  while (true) {
    network_wait_online(portMAX_DELAY);
    const bool ok = fetch(*args->bus, args->settings->get());
    // Retry after a minute on failure; a notification (new location) wakes the task early
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? weather::REFRESH_SECONDS * 1000 : 60 * 1000));
  }
}

}  // namespace

void weather_start(Bus &bus, Settings &settings) {
  static TaskArgs args{&bus, &settings};
  xTaskCreate(task, "dm_weather", 8192, &args, 3, &g_task);
}

void weather_refresh() {
  if (g_task) xTaskNotifyGive(g_task);
}

}  // namespace dm::esp32
