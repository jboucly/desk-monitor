// Data sources of the simulator: a fake Home Assistant and the real Open-Meteo forecast.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

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
   * @description Renders these templates live on "ha_tpl/<key>" (re-rendered when states change).
   */
  void set_templates(const std::set<std::string> &templates);

  /**
   * @description Renders a template with the mock states. Understands {{ states('id') }} and
   * {{ state_attr('id', 'attr') }}; other expressions are copied as typed, {% … %} blocks dropped.
   * @returns False with an error message when the braces do not balance.
   */
  bool render(const std::string &tpl, std::string &out);

  /**
   * @description Publishes a made-up history of a numeric entity on "ha_hist/<entity>/<hours>"
   * (a daily wave plus noise around its current value).
   */
  void request_history(const std::string &entity_id, int hours);

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
  void publish_templates();
  const cJSON *state_of(const std::string &entity_id);

  dm::Bus &bus_;
  std::map<std::string, dm::json::Ptr> states_;
  std::set<std::string> templates_;
  unsigned seed_ = 1;
};

/**
 * @description Fake Creality printer (stock firmware protocol, see protocol/README.md "3D printer"): it
 * answers the session's requests and pushes state frames like a CR-10 SE. LVGL thread only.
 */
class MockPrinter {
 public:
  /**
   * @param push Delivers a frame to the session, as the WebSocket would.
   */
  explicit MockPrinter(std::function<void(const std::string &)> push) : push_(std::move(push)) {}

  /**
   * @description The session sent a frame (request or command).
   */
  void receive(const std::string &text);

  /**
   * @description Pushes the full state, as the printer does when a client connects.
   */
  void connect();

  /**
   * @description Moves the job and the temperatures by one second.
   */
  void tick();

  /**
   * @description Puts the printer in a state for screenshots: "printing", "paused", "finished", "idle".
   */
  void set_state(const std::string &state);

  /**
   * @description A made-up 96x96 PNG thumbnail for a printer file, different for each name.
   */
  static std::string thumbnail_png(const std::string &url);

 private:
  void push_state();
  void push_history();
  void finish(int result);

  std::function<void(const std::string &)> push_;
  int state_ = 1;  // Creality code: 0 idle, 1 printing, 2 finished, 4 stopped, 5 paused
  std::string file_ = "/usr/data/printer_data/gcodes/support_casque.stl_PLA_2h10m5s.gcode";
  double progress_ = 42, elapsed_ = 3280, total_ = 7805, nozzle_ = 209.6, bed_ = 60.1;
  int light_ = 1;
  std::vector<std::pair<std::string, int>> history_;  // file, printfinish
};

/**
 * @description Blocking HTTP GET with libcurl. Returns the body, or an empty string on any failure.
 */
std::string http_get(const std::string &url);

/**
 * @description Fetches the Open-Meteo forecast in a background thread and posts it as "weather".
 * Falls back to canned data when offline.
 */
void fetch_weather(dm::Bus &bus, double lat, double lon, const std::string &name);

}  // namespace sim
