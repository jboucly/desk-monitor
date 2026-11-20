#include "printer_client.h"

#include <string>

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "ha_client.h"

namespace dm::esp32 {

namespace {

const char *TAG = "printer";

PrinterSession *g_session = nullptr;
esp_websocket_client_handle_t g_client = nullptr;
std::string g_url;  // kept alive: the client config points to it
std::string g_rx;   // message being reassembled

void on_event(void *, esp_event_base_t, int32_t id, void *event_data) {
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "connected to %s", g_url.c_str());
      g_rx.clear();
      g_session->on_open();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      ESP_LOGW(TAG, "disconnected");
      g_session->on_close();
      break;
    case WEBSOCKET_EVENT_DATA:
      // Text frames (0x1) and continuations (0x0). The history (~20 KB) arrives in several events.
      if (data->op_code != 0x1 && data->op_code != 0x0) break;
      if (data->op_code == 0x1 && data->payload_offset == 0) g_rx.clear();
      g_rx.append(data->data_ptr, data->data_len);
      if (data->fin && data->payload_offset + data->data_len >= data->payload_len) {
        g_session->on_message(g_rx);
        g_rx.clear();
      }
      break;
    default:
      break;
  }
}

void stop_client() {
  if (!g_client) return;
  esp_websocket_client_stop(g_client);
  esp_websocket_client_destroy(g_client);
  g_client = nullptr;
}

}  // namespace

void printer_client_start(PrinterSession &session, const SettingsData &settings) {
  g_session = &session;
  session.set_sender(nullptr);
  stop_client();
  session.set_fetcher(http_fetch);

  g_url = session.configure(settings.printer_model, settings.printer_host, settings.printer_port);
  if (g_url.empty()) return;

  esp_websocket_client_config_t config = {};
  config.uri = g_url.c_str();
  // Internal RAM: the frames are reassembled in g_rx (PSRAM once past 4 KB), small buffers are enough
  config.buffer_size = 2048;
  // Frames are parsed on this task (cJSON in PSRAM): 6 KB of stack is enough
  config.task_stack = 6144;
  config.task_prio = 4;
  config.reconnect_timeout_ms = 10000;
  config.network_timeout_ms = 10000;
  // The printer pushes its temperatures every second: no ping needed to notice a dead link
  config.ping_interval_sec = 30;
  g_client = esp_websocket_client_init(&config);
  esp_websocket_register_events(g_client, WEBSOCKET_EVENT_ANY, on_event, nullptr);
  session.set_sender([](const std::string &text) {
    if (g_client && esp_websocket_client_is_connected(g_client))
      esp_websocket_client_send_text(g_client, text.data(), static_cast<int>(text.size()), pdMS_TO_TICKS(2000));
  });
  esp_websocket_client_start(g_client);
}

}  // namespace dm::esp32
