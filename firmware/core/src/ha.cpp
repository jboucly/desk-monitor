#include "dm/ha.h"

#include <cstring>

namespace dm {

std::string HaSession::websocket_url(std::string base) {
  while (!base.empty() && (base.back() == '/' || base.back() == ' ')) base.pop_back();
  if (base.empty()) return base;
  if (base.rfind("https://", 0) == 0) base = "wss://" + base.substr(8);
  else if (base.rfind("http://", 0) == 0) base = "ws://" + base.substr(7);
  else if (base.rfind("ws://", 0) != 0 && base.rfind("wss://", 0) != 0) base = "ws://" + base;
  const std::string suffix = "/api/websocket";
  if (base.size() < suffix.size() || base.compare(base.size() - suffix.size(), suffix.size(), suffix) != 0) base += suffix;
  return base;
}

void HaSession::set_sender(Sender sender) {
  std::lock_guard<std::mutex> lock(mutex_);
  sender_ = std::move(sender);
}

void HaSession::set_token(const std::string &token) {
  std::lock_guard<std::mutex> lock(mutex_);
  token_ = token;
}

void HaSession::on_open() {
  std::lock_guard<std::mutex> lock(mutex_);
  authenticated_ = false;
  subscription_id_ = 0;
  next_id_ = 1;
  outbox_.clear();
}

void HaSession::on_close() {
  std::map<int, ResultCallback> failed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    authenticated_ = false;
    subscription_id_ = 0;
    outbox_.clear();
    failed.swap(pending_);
  }
  for (auto &[id, callback] : failed)
    if (callback) callback(nullptr);
  post_status("connecting");
}

void HaSession::on_message(const std::string &text) {
  auto root = json::parse(text);
  const char *type = json::str(root.get(), "type");
  ResultCallback callback;
  cJSON *result = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!strcmp(type, "auth_required")) {
      // The token is sent without an id, as the handshake requires
      cJSON *auth = cJSON_CreateObject();
      cJSON_AddStringToObject(auth, "type", "auth");
      cJSON_AddStringToObject(auth, "access_token", token_.c_str());
      outbox_.push_back(json::print(auth));
      cJSON_Delete(auth);
    } else if (!strcmp(type, "auth_ok")) {
      authenticated_ = true;
      subscribe_locked();
    } else if (!strcmp(type, "auth_invalid")) {
      authenticated_ = false;
    } else if (!strcmp(type, "event")) {
      if (static_cast<int>(json::num(root.get(), "id")) == subscription_id_) apply_event_locked(json::get(root.get(), "event"));
    } else if (!strcmp(type, "result")) {
      auto it = pending_.find(static_cast<int>(json::num(root.get(), "id")));
      if (it != pending_.end()) {
        callback = std::move(it->second);
        pending_.erase(it);
        if (json::boolean(root.get(), "success")) result = cJSON_DetachItemFromObject(root.get(), "result");
      }
    }
  }
  if (!strcmp(type, "auth_ok")) post_status("ok");
  if (!strcmp(type, "auth_invalid")) post_status("auth_error");
  if (callback) callback(result);
  else cJSON_Delete(result);
  flush();
}

void HaSession::set_entities(const std::set<std::string> &entities) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entities == entities_) return;
    entities_ = entities;
    // Forget the states of entities that are no longer used
    for (auto it = states_.begin(); it != states_.end();) it = entities_.count(it->first) ? std::next(it) : states_.erase(it);
    if (authenticated_) subscribe_locked();
  }
  flush();
}

void HaSession::call_service(const std::string &domain, const std::string &service, const std::string &entity_id,
                             cJSON *data) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!authenticated_) {
      cJSON_Delete(data);
      return;
    }
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "call_service");
    cJSON_AddStringToObject(msg, "domain", domain.c_str());
    cJSON_AddStringToObject(msg, "service", service.c_str());
    cJSON_AddItemToObject(msg, "service_data", data ? data : cJSON_CreateObject());
    if (!entity_id.empty()) {
      cJSON *target = cJSON_AddObjectToObject(msg, "target");
      cJSON_AddStringToObject(target, "entity_id", entity_id.c_str());
    }
    send_locked(msg);
  }
  flush();
}

void HaSession::list_entities(ResultCallback callback) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!authenticated_) {
      callback(nullptr);
      return;
    }
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "config/entity_registry/list_for_display");
    // Reduce the compact registry format ({"ei", "en"}) to {"id", "name"}
    send_locked(msg, [callback](cJSON *result) {
      if (!result) return callback(nullptr);
      cJSON *out = cJSON_CreateArray();
      const cJSON *entity = nullptr;
      cJSON_ArrayForEach(entity, json::get(result, "entities")) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", json::str(entity, "ei"));
        cJSON_AddStringToObject(item, "name", json::str(entity, "en"));
        cJSON_AddItemToArray(out, item);
      }
      cJSON_Delete(result);
      callback(out);
    });
  }
  flush();
}

void HaSession::post_status(const char *state) {
  cJSON *status = cJSON_CreateObject();
  cJSON_AddStringToObject(status, "state", state);
  bus_.post("sys/ha", status);
}

void HaSession::send_locked(cJSON *message, ResultCallback callback) {
  const int id = next_id_++;
  cJSON_AddNumberToObject(message, "id", id);
  if (callback) pending_[id] = std::move(callback);
  outbox_.push_back(json::print(message));
  cJSON_Delete(message);
}

void HaSession::subscribe_locked() {
  if (subscription_id_) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "unsubscribe_events");
    cJSON_AddNumberToObject(msg, "subscription", subscription_id_);
    send_locked(msg);
    subscription_id_ = 0;
  }
  if (entities_.empty()) return;
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "subscribe_entities");
  cJSON *ids = cJSON_AddArrayToObject(msg, "entity_ids");
  for (const auto &id : entities_) cJSON_AddItemToArray(ids, cJSON_CreateString(id.c_str()));
  subscription_id_ = next_id_;
  send_locked(msg);
}

void HaSession::apply_event_locked(const cJSON *event) {
  // "a": entities added with their full state
  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, json::get(event, "a")) {
    cJSON *state = cJSON_CreateObject();
    cJSON_AddStringToObject(state, "s", json::str(item, "s"));
    const cJSON *attrs = json::get(item, "a");
    cJSON_AddItemToObject(state, "a", attrs ? cJSON_Duplicate(attrs, true) : cJSON_CreateObject());
    states_[item->string] = json::own(state);
    publish_locked(item->string);
  }
  // "c": changes, "+" = new values (attributes are merged), "-" = removed attributes
  cJSON_ArrayForEach(item, json::get(event, "c")) {
    auto it = states_.find(item->string);
    if (it == states_.end()) continue;
    cJSON *state = it->second.get();
    const cJSON *plus = json::get(item, "+");
    if (const cJSON *s = json::get(plus, "s"); cJSON_IsString(s))
      cJSON_ReplaceItemInObject(state, "s", cJSON_CreateString(s->valuestring));
    cJSON *attrs = cJSON_GetObjectItem(state, "a");
    const cJSON *attr = nullptr;
    cJSON_ArrayForEach(attr, json::get(plus, "a")) {
      cJSON_DeleteItemFromObject(attrs, attr->string);
      cJSON_AddItemToObject(attrs, attr->string, cJSON_Duplicate(attr, true));
    }
    cJSON_ArrayForEach(attr, json::get(json::get(item, "-"), "a")) {
      if (cJSON_IsString(attr)) cJSON_DeleteItemFromObject(attrs, attr->valuestring);
    }
    publish_locked(item->string);
  }
  // "r": removed entities
  cJSON_ArrayForEach(item, json::get(event, "r")) {
    if (!cJSON_IsString(item)) continue;
    states_.erase(item->valuestring);
    cJSON *state = cJSON_CreateObject();
    cJSON_AddStringToObject(state, "s", "unavailable");
    cJSON_AddObjectToObject(state, "a");
    bus_.post(std::string("ha/") + item->valuestring, state);
  }
}

void HaSession::publish_locked(const std::string &entity_id) {
  auto it = states_.find(entity_id);
  if (it != states_.end()) bus_.post("ha/" + entity_id, cJSON_Duplicate(it->second.get(), true));
}

void HaSession::flush() {
  std::vector<std::string> batch;
  Sender sender;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batch.swap(outbox_);
    sender = sender_;
  }
  if (!sender) return;
  for (const auto &text : batch) sender(text);
}

}  // namespace dm
