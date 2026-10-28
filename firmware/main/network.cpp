#include "network.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"

namespace dm::esp32 {

namespace {

const char *TAG = "network";
constexpr EventBits_t ONLINE_BIT = BIT0;
// Failed connection attempts before the setup access point opens next to the station
constexpr int FAILURES_BEFORE_AP = 6;

Bus *g_bus = nullptr;
EventGroupHandle_t g_events = nullptr;
esp_netif_t *g_sta = nullptr;
esp_netif_t *g_ap = nullptr;
esp_timer_handle_t g_retry_timer = nullptr;
bool g_has_credentials = false;
bool g_ap_active = false;
int g_failures = 0;
bool g_services_started = false;

void post_status() {
  cJSON *status = cJSON_CreateObject();
  if (network_online()) {
    cJSON_AddNumberToObject(status, "rssi", network_rssi());
    cJSON_AddStringToObject(status, "ip", network_ip().c_str());
  } else {
    cJSON_AddNullToObject(status, "rssi");
  }
  if (g_ap_active) {
    cJSON_AddStringToObject(status, "ap", SETUP_AP_SSID);
    cJSON_AddStringToObject(status, "ap_ip", "192.168.4.1");
  }
  g_bus->post("sys/wifi", status);
}

/**
 * @description Opens the open setup network, next to the station when credentials exist.
 */
void start_ap() {
  if (g_ap_active) return;
  if (!g_ap) g_ap = esp_netif_create_default_wifi_ap();
  wifi_config_t ap = {};
  memcpy(ap.ap.ssid, SETUP_AP_SSID, strlen(SETUP_AP_SSID));
  ap.ap.ssid_len = strlen(SETUP_AP_SSID);
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_OPEN;
  esp_wifi_set_mode(g_has_credentials ? WIFI_MODE_APSTA : WIFI_MODE_AP);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  g_ap_active = true;
  ESP_LOGW(TAG, "setup access point \"%s\" open on 192.168.4.1", SETUP_AP_SSID);
  post_status();
}

void stop_ap() {
  if (!g_ap_active) return;
  esp_wifi_set_mode(WIFI_MODE_STA);
  g_ap_active = false;
}

/**
 * @description SNTP and mDNS start once, at the first IP address.
 */
void start_services() {
  if (g_services_started) return;
  g_services_started = true;
  esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  esp_netif_sntp_init(&sntp);
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set("desk-monitor");
    mdns_instance_name_set("Desk Monitor");
    mdns_service_add("Desk Monitor", "_http", "_tcp", 80, nullptr, 0);
    // Lets the PC companion find the screen without typing an address
    mdns_txt_item_t txt[] = {{"path", "/api/pc"}};
    mdns_service_add("Desk Monitor", "_desk-monitor", "_tcp", 80, txt, 1);
  }
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && g_has_credentials) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(g_events, ONLINE_BIT);
    g_failures++;
    if (g_failures >= FAILURES_BEFORE_AP) start_ap();
    // Back off up to 30 s between attempts
    const int delay_s = g_failures < 5 ? 2 * g_failures : 30;
    esp_timer_stop(g_retry_timer);
    esp_timer_start_once(g_retry_timer, static_cast<uint64_t>(delay_s) * 1000000ULL);
    post_status();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    auto *event = static_cast<wifi_event_ap_staconnected_t *>(data);
    ESP_LOGI(TAG, "setup AP: client " MACSTR " joined", MAC2STR(event->mac));
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
    auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(data);
    ESP_LOGI(TAG, "setup AP: client " MACSTR " left (reason %d)", MAC2STR(event->mac), event->reason);
  } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
    auto *event = static_cast<ip_event_ap_staipassigned_t *>(data);
    ESP_LOGI(TAG, "setup AP: client got " IPSTR, IP2STR(&event->ip));
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "online, ip " IPSTR, IP2STR(&event->ip_info.ip));
    g_failures = 0;
    xEventGroupSetBits(g_events, ONLINE_BIT);
    stop_ap();
    start_services();
    post_status();
  }
}

void apply_station_config(const SettingsData &settings) {
  g_has_credentials = !settings.wifi_ssid.empty();
  wifi_config_t sta = {};
  // The SSID and password fields are fixed-size and do not need a terminating NUL when full
  memcpy(sta.sta.ssid, settings.wifi_ssid.data(), std::min(settings.wifi_ssid.size(), sizeof sta.sta.ssid));
  memcpy(sta.sta.password, settings.wifi_password.data(),
         std::min(settings.wifi_password.size(), sizeof sta.sta.password));
  sta.sta.threshold.authmode = settings.wifi_password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  sta.sta.pmf_cfg.capable = true;
  esp_wifi_set_config(WIFI_IF_STA, &sta);
}

void apply_timezone(const std::string &tz) {
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

}  // namespace

void network_start(Bus &bus, const SettingsData &settings) {
  g_bus = &bus;
  g_events = xEventGroupCreate();
  apply_timezone(settings.timezone);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  g_sta = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(g_sta, "desk-monitor");

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_wifi_event, nullptr));

  esp_timer_create_args_t retry = {};
  retry.callback = [](void *) {
    if (g_has_credentials) esp_wifi_connect();
  };
  retry.name = "wifi_retry";
  esp_timer_create(&retry, &g_retry_timer);

  // Signal strength refresh for the header icon
  esp_timer_create_args_t status = {};
  status.callback = [](void *) { post_status(); };
  status.name = "wifi_status";
  esp_timer_handle_t status_timer = nullptr;
  esp_timer_create(&status, &status_timer);
  esp_timer_start_periodic(status_timer, 10ULL * 1000000ULL);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  apply_station_config(settings);
  ESP_ERROR_CHECK(esp_wifi_start());
  if (!g_has_credentials) start_ap();
  post_status();
}

void network_reconfigure(const SettingsData &settings, unsigned changes) {
  if (changes & Settings::TIME) apply_timezone(settings.timezone);
  if (!(changes & Settings::WIFI)) return;
  ESP_LOGI(TAG, "new Wi-Fi settings, reconnecting to \"%s\"", settings.wifi_ssid.c_str());
  g_failures = 0;
  esp_wifi_disconnect();
  apply_station_config(settings);
  if (g_ap_active) esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (g_has_credentials) esp_wifi_connect();
}

bool network_online() { return g_events && (xEventGroupGetBits(g_events) & ONLINE_BIT); }

bool network_wait_online(uint32_t timeout_ms) {
  if (!g_events) return false;
  return xEventGroupWaitBits(g_events, ONLINE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & ONLINE_BIT;
}

std::string network_ip() {
  esp_netif_ip_info_t info = {};
  if (!g_sta || esp_netif_get_ip_info(g_sta, &info) != ESP_OK || info.ip.addr == 0) return "";
  char buf[16];
  snprintf(buf, sizeof buf, IPSTR, IP2STR(&info.ip));
  return buf;
}

int network_rssi() {
  wifi_ap_record_t ap = {};
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

}  // namespace dm::esp32
