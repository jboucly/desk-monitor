#include "http_server.h"

#include <string>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

// Web editor bundle embedded by main/CMakeLists.txt: the gzipped build of web/, or a plain fallback page
extern const uint8_t WEB_START[] asm("_binary_web_bin_start");
extern const uint8_t WEB_END[] asm("_binary_web_bin_end");

namespace dm::esp32 {

namespace {

const char *TAG = "http";
constexpr size_t MAX_BODY = 64 * 1024;

struct Context {
  Api *api;
  Settings *settings;
};
Context g_ctx;

const char *status_line(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 413: return "413 Payload Too Large";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

std::string header(httpd_req_t *req, const char *name) {
  const size_t len = httpd_req_get_hdr_value_len(req, name);
  if (!len) return {};
  std::string value(len + 1, '\0');
  httpd_req_get_hdr_value_str(req, name, value.data(), value.size());
  value.resize(len);
  return value;
}

esp_err_t send(httpd_req_t *req, const HttpResponse &r) {
  httpd_resp_set_status(req, status_line(r.status));
  httpd_resp_set_type(req, r.content_type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, r.body.data(), static_cast<ssize_t>(r.body.size()));
}

/**
 * @description Every /api/... route except the OTA upload goes through dm::Api.
 */
esp_err_t api_handler(httpd_req_t *req) {
  if (req->content_len > MAX_BODY) return send(req, Api::error(413, "requête trop volumineuse"));
  std::string body(req->content_len, '\0');
  for (size_t received = 0; received < body.size();) {
    const int n = httpd_req_recv(req, body.data() + received, body.size() - received);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return ESP_FAIL;
    received += n;
  }
  std::string path = req->uri;
  path = path.substr(0, path.find('?'));
  const char *method = req->method == HTTP_GET ? "GET" : req->method == HTTP_PUT ? "PUT" : "POST";
  return send(req, g_ctx.api->handle(method, path, body, header(req, "X-DM-Pin")));
}

/**
 * @description Receives a firmware image as the raw request body and boots it.
 */
esp_err_t ota_handler(httpd_req_t *req) {
  const std::string pin = g_ctx.settings->get().pin;
  if (!pin.empty() && header(req, "X-DM-Pin") != pin) return send(req, Api::error(401, "PIN incorrect"));

  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  esp_ota_handle_t ota = 0;
  if (!target || esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK)
    return send(req, Api::error(500, "partition OTA indisponible"));

  ESP_LOGI(TAG, "OTA: %d bytes to %s", req->content_len, target->label);
  static char buf[4096];
  for (size_t remaining = req->content_len; remaining > 0;) {
    const int n = httpd_req_recv(req, buf, remaining < sizeof buf ? remaining : sizeof buf);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0 || esp_ota_write(ota, buf, n) != ESP_OK) {
      esp_ota_abort(ota);
      return send(req, Api::error(500, "écriture du firmware interrompue"));
    }
    remaining -= n;
  }
  if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK)
    return send(req, Api::error(400, "image firmware invalide"));

  send(req, HttpResponse{200, R"({"ok":true,"rebooting":true})"});
  // Restart once the answer is out
  esp_timer_create_args_t args = {};
  args.callback = [](void *) { esp_restart(); };
  args.name = "ota_reboot";
  esp_timer_handle_t timer = nullptr;
  esp_timer_create(&args, &timer);
  esp_timer_start_once(timer, 1500 * 1000);
  return ESP_OK;
}

esp_err_t reboot_handler(httpd_req_t *req) {
  const std::string pin = g_ctx.settings->get().pin;
  if (!pin.empty() && header(req, "X-DM-Pin") != pin) return send(req, Api::error(401, "PIN incorrect"));
  send(req, HttpResponse{200, R"({"ok":true})"});
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

/**
 * @description Single-page editor: every non-API path returns the same page.
 */
esp_err_t web_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  // gzip magic bytes: the real editor; otherwise the plain fallback page
  if (WEB_END - WEB_START > 2 && WEB_START[0] == 0x1F && WEB_START[1] == 0x8B)
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, reinterpret_cast<const char *>(WEB_START), WEB_END - WEB_START);
}

void route(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *)) {
  httpd_uri_t def = {};
  def.uri = uri;
  def.method = method;
  def.handler = handler;
  httpd_register_uri_handler(server, &def);
}

}  // namespace

void http_start(Api &api, Settings &settings) {
  g_ctx = {&api, &settings};
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 10 * 1024;
  config.max_uri_handlers = 12;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 15;
  httpd_handle_t server = nullptr;
  if (const esp_err_t err = httpd_start(&server, &config); err != ESP_OK) {
    ESP_LOGE(TAG, "cannot start the HTTP server: %s (internal RAM %u free)", esp_err_to_name(err),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return;
  }
  // Specific routes first: the wildcard matcher keeps the registration order
  route(server, "/api/ota", HTTP_POST, ota_handler);
  route(server, "/api/reboot", HTTP_POST, reboot_handler);
  route(server, "/api/*", HTTP_GET, api_handler);
  route(server, "/api/*", HTTP_PUT, api_handler);
  route(server, "/api/*", HTTP_POST, api_handler);
  route(server, "/*", HTTP_GET, web_handler);
  ESP_LOGI(TAG, "editor on http://desk-monitor.local");
}

}  // namespace dm::esp32
