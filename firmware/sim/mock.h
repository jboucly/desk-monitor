// Data sources of the simulator: a fake Home Assistant and the real Open-Meteo forecast.
#pragma once

#include <map>
#include <set>
#include <string>

#include "dm/bus.h"

namespace sim {

/**
 * @description In-memory stand-in for Home Assistant: every entity used by the layout gets a
 * plausible state, and service calls change it like HA would. LVGL thread only.
 */
class MockHome {
 public:
  explicit MockHome(dm::Bus &bus) : bus_(bus) {}

  /**
   * @description Creates states for new entities and publishes them.
   */
  void set_entities(const std::set<std::string> &entities);

  /**
   * @description Applies a service call (toggle, turn_on with brightness/color, turn_off…).
   * @param data Service data, owned by this function.
   */
  void call(const std::string &domain, const std::string &service, const std::string &entity_id, cJSON *data);

  /**
   * @description Entity list in the format of GET /api/ha/entities.
   */
  std::string entity_list_json() const;

  /**
   * @description Makes the sensors move a little (called every few seconds).
   */
  void tick();

 private:
  void publish(const std::string &entity_id);

  dm::Bus &bus_;
  std::map<std::string, dm::json::Ptr> states_;
  unsigned seed_ = 1;
};

/**
 * @description Fetches the Open-Meteo forecast in a background thread and posts it as "weather".
 * Falls back to canned data when offline.
 */
void fetch_weather(dm::Bus &bus, double lat, double lon, const std::string &name);

}  // namespace sim
