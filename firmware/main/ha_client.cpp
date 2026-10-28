#include "ha_client.h"

#include <memory>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace dm::esp32 {

namespace {

const char *TAG = "ha";

HaSession *g_session = nullptr;
esp_websocket_client_handle_t g_client = nullptr;
std::string g_url;   // kept alive: the client config points to it
std::string g_rx;    // message being reassembled

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
      // Text frames (0x1) and continuations (0x0). Big frames arrive in several events (payload_offset).
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

void ha_client_start(HaSession &session, const SettingsData &settings) {
  g_session = &session;
  session.set_sender(nullptr);
  stop_client();

  g_url = HaSession::websocket_url(settings.ha_url);
  if (g_url.empty() || settings.ha_token.empty()) {
    session.post_status("off");
    return;
  }
  session.set_token(settings.ha_token);
  session.post_status("connecting");

  esp_websocket_client_config_t config = {};
  config.uri = g_url.c_str();
  config.buffer_size = 4096;
  config.task_stack = 8192;
  config.task_prio = 5;
  config.reconnect_timeout_ms = 5000;
  config.network_timeout_ms = 10000;
  config.ping_interval_sec = 20;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  g_client = esp_websocket_client_init(&config);
  esp_websocket_register_events(g_client, WEBSOCKET_EVENT_ANY, on_event, nullptr);
  session.set_sender([](const std::string &text) {
    if (g_client && esp_websocket_client_is_connected(g_client))
      esp_websocket_client_send_text(g_client, text.data(), static_cast<int>(text.size()), pdMS_TO_TICKS(2000));
  });
  esp_websocket_client_start(g_client);
}

std::string ha_client_entities() {
  if (!g_session) return "";
  // The callback may run after a timeout: it owns its copy of the shared state
  struct Pending {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    std::string result;
    ~Pending() { vSemaphoreDelete(done); }
  };
  auto pending = std::make_shared<Pending>();
  g_session->list_entities([pending](cJSON *list) {
    if (list) pending->result = json::print(list);
    cJSON_Delete(list);
    xSemaphoreGive(pending->done);
  });
  std::string out;
  if (xSemaphoreTake(pending->done, pdMS_TO_TICKS(8000)) == pdTRUE) out = pending->result;
  return out;
}

namespace {

struct FetchJob {
  std::string url;
  std::function<void(std::string)> done;
};

constexpr size_t MAX_FETCH = 1024 * 1024;

void fetch_task(void *arg) {
  std::unique_ptr<FetchJob> job(static_cast<FetchJob *>(arg));
  std::string body;
  esp_http_client_config_t config = {};
  config.url = job->url.c_str();
  config.timeout_ms = 8000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client && esp_http_client_open(client, 0) == ESP_OK) {
    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) == 200) {
      char chunk[1024];
      int n;
      while ((n = esp_http_client_read(client, chunk, sizeof chunk)) > 0 && body.size() < MAX_FETCH) body.append(chunk, n);
    }
  }
  if (client) esp_http_client_cleanup(client);
  if (body.size() >= MAX_FETCH) body.clear();
  ESP_LOGI(TAG, "fetched %u bytes from %s", static_cast<unsigned>(body.size()), job->url.substr(0, job->url.find('?')).c_str());
  job->done(std::move(body));
  vTaskDelete(nullptr);
}

}  // namespace

void ha_fetch(const std::string &path, std::function<void(std::string body)> done) {
  // http(s)://host:port from the WebSocket URL of the session
  std::string base = g_url;
  if (base.rfind("wss://", 0) == 0) base = "https://" + base.substr(6);
  else if (base.rfind("ws://", 0) == 0) base = "http://" + base.substr(5);
  const size_t api = base.find("/api/websocket");
  if (api != std::string::npos) base.resize(api);
  if (base.empty() || path.empty()) {
    done({});
    return;
  }
  auto *job = new FetchJob{path.rfind("http", 0) == 0 ? path : base + path, std::move(done)};
  // The JPEG decoding of the cover runs in this task too (TJpgDec, small recursion, PSRAM buffers)
  if (xTaskCreate(fetch_task, "ha_fetch", 8192, job, 3, nullptr) != pdPASS) {
    job->done({});
    delete job;
  }
}

}  // namespace dm::esp32
