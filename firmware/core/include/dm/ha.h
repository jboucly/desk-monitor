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
   * @description Calls a service. Dropped when not connected.
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
};

}  // namespace dm
