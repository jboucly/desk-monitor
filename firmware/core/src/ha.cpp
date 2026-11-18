#include "dm/ha.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

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
  template_subs_.clear();
  next_id_ = 1;
  outbox_.clear();
}

void HaSession::on_close() {
  std::map<int, ResultCallback> failed;
  std::map<int, std::function<void(bool, std::string)>> renders;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    authenticated_ = false;
    subscription_id_ = 0;
    template_subs_.clear();
    outbox_.clear();
    failed.swap(pending_);
    renders.swap(renders_);
  }
  for (auto &[id, callback] : failed)
    if (callback) callback(nullptr);
  for (auto &[id, callback] : renders) callback(false, "Home Assistant déconnecté");
  post_status("connecting");
}

void HaSession::on_message(const std::string &text) {
  auto root = json::parse(text);
  const char *type = json::str(root.get(), "type");
  ResultCallback callback;
  cJSON *result = nullptr;
  std::function<void(bool, std::string)> render;
  bool render_ok = false;
  std::string render_text;
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
      const int id = static_cast<int>(json::num(root.get(), "id"));
      const cJSON *event = json::get(root.get(), "event");
      // report_errors also sends warnings (e.g. an undefined variable): a result follows them
      const bool warning = !strcmp(json::str(event, "level"), "WARNING");
      if (warning) {
      } else if (id == subscription_id_) {
        apply_event_locked(event);
      } else if (auto sub = template_subs_.find(id); sub != template_subs_.end()) {
        publish_template_locked(sub->second, event);
      } else if (auto once = renders_.find(id); once != renders_.end()) {
        // First render of a one-shot template: answer and stop listening
        render = std::move(once->second);
        renders_.erase(once);
        const cJSON *error = json::get(event, "error");
        render_ok = !error;
        render_text = error ? json::str(event, "error") : json::print(json::get(event, "result"));
        if (render_ok && cJSON_IsString(json::get(event, "result"))) render_text = json::str(event, "result");
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "unsubscribe_events");
        cJSON_AddNumberToObject(msg, "subscription", id);
        send_locked(msg);
      }
    } else if (!strcmp(type, "result")) {
      const int id = static_cast<int>(json::num(root.get(), "id"));
      // A template HA cannot parse fails at once, with the reason in error.message
      if (!json::boolean(root.get(), "success")) {
        const char *message = json::str(json::get(root.get(), "error"), "message", "erreur inconnue");
        if (auto sub = template_subs_.find(id); sub != template_subs_.end()) {
          cJSON *event = cJSON_CreateObject();
          cJSON_AddStringToObject(event, "error", message);
          publish_template_locked(sub->second, event);
          cJSON_Delete(event);
        } else if (auto once = renders_.find(id); once != renders_.end()) {
          render = std::move(once->second);
          renders_.erase(once);
          render_text = message;
        }
      }
      auto it = pending_.find(id);
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
  if (render) render(render_ok, render_text);
  flush();
}

void HaSession::set_templates(const std::set<std::string> &templates) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (templates == templates_) return;
    templates_ = templates;
    if (authenticated_) {
      // Stop the subscriptions of templates no longer used, start the new ones
      std::set<std::string> subscribed;
      for (auto it = template_subs_.begin(); it != template_subs_.end();) {
        if (templates_.count(it->second)) {
          subscribed.insert(it->second);
          ++it;
          continue;
        }
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "unsubscribe_events");
        cJSON_AddNumberToObject(msg, "subscription", it->first);
        send_locked(msg);
        it = template_subs_.erase(it);
      }
      for (const auto &tpl : templates_)
        if (!subscribed.count(tpl)) subscribe_template_locked(tpl);
    }
  }
  flush();
}

void HaSession::render_template(const std::string &tpl, std::function<void(bool ok, std::string text)> callback) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!authenticated_) {
      callback(false, "Home Assistant non connecté");
      return;
    }
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "render_template");
    cJSON_AddStringToObject(msg, "template", tpl.c_str());
    cJSON_AddBoolToObject(msg, "report_errors", true);
    renders_[next_id_] = std::move(callback);
    send_locked(msg);
  }
  flush();
}

void HaSession::request_history(const std::string &entity, int hours) {
  const time_t now = time(nullptr);
  if (now < 1600000000) return;  // no SNTP answer yet: the start time would be wrong
  const double t1 = static_cast<double>(now), t0 = t1 - hours * 3600.0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!authenticated_) return;
    char start[32];
    const time_t from = static_cast<time_t>(t0);
    struct tm utc;
    gmtime_r(&from, &utc);
    strftime(start, sizeof start, "%Y-%m-%dT%H:%M:%SZ", &utc);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "history/history_during_period");
    cJSON_AddStringToObject(msg, "start_time", start);
    cJSON *ids = cJSON_AddArrayToObject(msg, "entity_ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(entity.c_str()));
    // Compact answer: [{"s", "lu"}…] without attributes
    cJSON_AddBoolToObject(msg, "minimal_response", true);
    cJSON_AddBoolToObject(msg, "no_attributes", true);
    cJSON_AddBoolToObject(msg, "significant_changes_only", false);
    const std::string key = "ha_hist/" + entity + "/" + std::to_string(hours);
    send_locked(msg, [this, entity, key, t0, t1](cJSON *result) {
      if (!result) return;
      bus_.post(key, history_series(json::get(result, entity.c_str()), t0, t1, HISTORY_POINTS));
      cJSON_Delete(result);
    });
  }
  flush();
}

cJSON *HaSession::history_series(const cJSON *changes, double t0, double t1, int buckets) {
  std::vector<double> sum(buckets, 0);
  std::vector<int> count(buckets, 0);
  double before = NAN;  // last value before t0 (HA starts with the state at t0)
  const double width = (t1 - t0) / buckets;
  const cJSON *change = nullptr;
  cJSON_ArrayForEach(change, changes) {
    const char *s = json::str(change, "s");
    char *end = nullptr;
    const double value = strtod(s, &end);
    if (end == s || *end != '\0' || !std::isfinite(value)) continue;  // "unavailable", "on"…
    const double at = json::num(change, "lu", t0);
    if (at < t0) {
      before = value;
      continue;
    }
    const int bucket = std::min(buckets - 1, static_cast<int>((at - t0) / width));
    sum[bucket] += value;
    count[bucket]++;
  }
  cJSON *series = cJSON_CreateObject();
  cJSON_AddNumberToObject(series, "t0", t0);
  cJSON_AddNumberToObject(series, "t1", t1);
  cJSON *values = cJSON_AddArrayToObject(series, "v");
  double last = before;
  for (int i = 0; i < buckets; i++) {
    if (count[i]) last = sum[i] / count[i];
    cJSON_AddItemToArray(values, std::isnan(last) ? cJSON_CreateNull() : cJSON_CreateNumber(last));
  }
  return series;
}

std::string HaSession::template_key(const std::string &tpl) {
  // FNV-1a: stable across boots and platforms
  uint32_t hash = 2166136261u;
  for (char c : tpl) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  char key[9];
  snprintf(key, sizeof key, "%08x", static_cast<unsigned>(hash));
  return key;
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
  // After a (re)connection the template subscriptions are gone too: start them again
  if (template_subs_.empty())
    for (const auto &tpl : templates_) subscribe_template_locked(tpl);
  if (entities_.empty()) return;
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "subscribe_entities");
  cJSON *ids = cJSON_AddArrayToObject(msg, "entity_ids");
  for (const auto &id : entities_) cJSON_AddItemToArray(ids, cJSON_CreateString(id.c_str()));
  subscription_id_ = next_id_;
  send_locked(msg);
}

void HaSession::subscribe_template_locked(const std::string &tpl) {
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "render_template");
  cJSON_AddStringToObject(msg, "template", tpl.c_str());
  cJSON_AddBoolToObject(msg, "report_errors", true);
  template_subs_[next_id_] = tpl;
  send_locked(msg);
}

void HaSession::publish_template_locked(const std::string &tpl, const cJSON *event) {
  cJSON *state = cJSON_CreateObject();
  if (const cJSON *error = json::get(event, "error")) {
    cJSON_AddStringToObject(state, "s", "unavailable");
    cJSON_AddStringToObject(state, "error", cJSON_IsString(error) ? error->valuestring : "erreur");
  } else {
    // HA parses results that look like numbers or lists: show them as text again
    const cJSON *result = json::get(event, "result");
    cJSON_AddStringToObject(state, "s", cJSON_IsString(result) ? result->valuestring : json::print(result).c_str());
  }
  cJSON_AddObjectToObject(state, "a");
  bus_.post("ha_tpl/" + template_key(tpl), state);
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
