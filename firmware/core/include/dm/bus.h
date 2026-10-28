// Data bus: the only channel between data sources (HA, weather, BLE…) and widgets.
//
// Keys in use (see protocol/README.md):
//   ha/<entity_id>  {"s": state, "a": {attributes}}
//   weather         parsed Open-Meteo forecast (core/src/weather.cpp)
//   sys/wifi        {"rssi": -60, "ip": "192.168.1.50"} or {"rssi": null}
//   sys/ha          {"state": "ok" | "connecting" | "auth_error" | "off"}
//   sys/pc          {"state": "connected" | "off", "name": "..."}
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "dm/json.h"

namespace dm {

/**
 * @description Keyed store of JSON values with change listeners.
 * Producers call post() from any thread; dispatch() runs in the LVGL thread,
 * stores the values and notifies listeners there, so widgets never race with LVGL.
 */
class Bus {
 public:
  using Listener = std::function<void(const cJSON *value)>;

  /**
   * @description Queues a new value for a key. Thread-safe.
   * @param key Bus key, e.g. "ha/light.bureau".
   * @param value Tree to publish; the bus takes ownership.
   */
  void post(const std::string &key, cJSON *value);

  /**
   * @description Applies queued values and calls the listeners. LVGL thread only.
   */
  void dispatch();

  /**
   * @description Returns the last value of a key, or null. LVGL thread only.
   */
  const cJSON *get(const std::string &key) const;

  /**
   * @description Registers a listener. It is called right away when the key already has a value. LVGL thread only.
   * @returns A token for unsubscribe().
   */
  int subscribe(const std::string &key, Listener listener);

  /**
   * @description Removes one listener. Safe to call during a dispatch, but not from the listener being removed. LVGL thread only.
   */
  void unsubscribe(int token);

  /**
   * @description Drops every listener (before the widgets are rebuilt). LVGL thread only.
   */
  void clear_subscriptions();

 private:
  std::mutex mutex_;
  std::vector<std::pair<std::string, json::Ptr>> pending_;
  std::map<std::string, json::Ptr> values_;
  // Unsubscribed listeners are emptied in place and erased later, so that iteration stays valid
  std::multimap<std::string, std::pair<int, Listener>> listeners_;
  int next_token_ = 1;
};

}  // namespace dm
