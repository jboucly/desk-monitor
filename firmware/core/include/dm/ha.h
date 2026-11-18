// Home Assistant WebSocket API session (transport-free).
//
// The platform owns the WebSocket and forwards open/close/message events here;
// the session answers the auth handshake, subscribes to the entities used by the
// layout (subscribe_entities), merges the compressed state diffs and publishes
// every entity on the bus as "ha/<entity_id>".
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dm/bus.h"

namespace dm {

/**
 * @description Protocol state machine of one Home Assistant WebSocket connection. Thread-safe.
 */
class HaSession {
 public:
  using Sender = std::function<void(const std::string &text)>;
  using ResultCallback = std::function<void(cJSON *result)>;

  explicit HaSession(Bus &bus) : bus_(bus) {}

  /**
   * @description Turns what the user typed ("homeassistant.local:8123", "https://ha.example.com"…)
   * into the WebSocket endpoint ("ws://…/api/websocket"). Empty input gives an empty string.
   */
  static std::string websocket_url(std::string base);

  /**
   * @description Sets the function that writes a text frame on the socket. Called without any lock held.
   */
  void set_sender(Sender sender);

  /**
   * @description Long-lived access token used by the next handshake.
   */
  void set_token(const std::string &token);

  /**
   * @description The socket is open: Home Assistant will send "auth_required".
   */
  void on_open();

  /**
   * @description The socket closed: pending requests fail, status goes back to "connecting".
   */
  void on_close();

  /**
   * @description Handles one complete text message.
   */
  void on_message(const std::string &text);

  /**
   * @description Changes the set of subscribed entities (re-subscribes when connected).
   */
  void set_entities(const std::set<std::string> &entities);

  /**
   * @description Changes the set of templates rendered live (one render_template subscription each).
   * Every result is published on "ha_tpl/<template_key()>" as {"s": text}, or {"s": "unavailable",
   * "error": message} when Home Assistant reports an error.
   */
  void set_templates(const std::set<std::string> &templates);

  /**
   * @description Renders a template once, for the editor's preview. The callback gets true and the text,
   * or false and an error message. It runs on the socket thread (or at once when not connected).
   */
  void render_template(const std::string &tpl, std::function<void(bool ok, std::string text)> callback);

  /**
   * @description Fetches the history of an entity over the last `hours` and publishes it on
   * "ha_hist/<entity>/<hours>" as {"t0", "t1", "v": [HISTORY_POINTS values, null before the first one]}.
   * Dropped when not connected or when the clock is not set yet.
   */
  void request_history(const std::string &entity, int hours);

  /**
   * @description Reduces state changes ([{"s", "lu"}…], "lu" in Unix seconds) to `buckets` values between
   * t0 and t1: the average of the numeric states inside each bucket, else the last value before it
   * (HA only records changes). Returns the {"t0", "t1", "v"} object published by request_history().
   */
  static cJSON *history_series(const cJSON *changes, double t0, double t1, int buckets);

  /**
   * @description Number of values of a history series.
   */
  static constexpr int HISTORY_POINTS = 96;

  /**
   * @description Short stable key of a template, used in its bus key ("ha_tpl/<key>").
   */
  static std::string template_key(const std::string &tpl);

  /**
   * @description Calls a service. Dropped when not connected. When Home Assistant refuses it (wrong code,
   * unknown service…), its message is shown as a toast ("sys/toast").
   * @param data Optional service data, owned by the session.
   */
  void call_service(const std::string &domain, const std::string &service, const std::string &entity_id, cJSON *data);

  /**
   * @description Requests the entity registry for the editor's pickers.
   * The callback receives an owned array of {"id", "name"} objects, or null on failure. It runs on the socket thread.
   */
  void list_entities(ResultCallback callback);

  /**
   * @description Publishes a status on "sys/ha" ("off", "connecting", "ok", "auth_error").
   */
  void post_status(const char *state);

 private:
  void send_locked(cJSON *message, ResultCallback callback = nullptr);
  void subscribe_locked();
  void subscribe_template_locked(const std::string &tpl);
  void publish_template_locked(const std::string &tpl, const cJSON *event);
  void apply_event_locked(const cJSON *event);
  void publish_locked(const std::string &entity_id);
  void flush();

  Bus &bus_;
  std::mutex mutex_;
  Sender sender_;
  std::string token_;
  std::vector<std::string> outbox_;
  bool authenticated_ = false;
  int next_id_ = 1;
  int subscription_id_ = 0;
  std::set<std::string> entities_;
  std::map<std::string, json::Ptr> states_;
  std::map<int, ResultCallback> pending_;
  std::map<int, std::string> calls_;  // call_service ids → "domain.service", for the error toast
  std::set<std::string> templates_;
  std::map<int, std::string> template_subs_;  // render_template subscription id → template
  std::map<int, std::function<void(bool, std::string)>> renders_;  // one-shot renders by id
};

}  // namespace dm
