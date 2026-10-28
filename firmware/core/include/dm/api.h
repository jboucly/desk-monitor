// REST API of the web editor (transport-free). See protocol/README.md for the endpoints.
#pragma once

#include <functional>
#include <string>

#include "dm/app.h"
#include "dm/settings.h"

namespace dm {

/**
 * @description An HTTP answer produced by the API.
 */
struct HttpResponse {
  int status = 200;
  std::string body;
  const char *content_type = "application/json";
};

/**
 * @description Routes the /api/... requests to the app and the settings. Thread-safe.
 */
class Api {
 public:
  Api(App &app, Settings &settings) : app_(app), settings_(settings) {}

  /**
   * @description Handles one request.
   * @param method "GET", "PUT", "POST"…
   * @param path Path without the query string, e.g. "/api/layout".
   * @param body Request body (JSON for PUT).
   * @param pin Value of the X-DM-Pin header, checked for every write when a PIN is set.
   */
  HttpResponse handle(const std::string &method, const std::string &path, const std::string &body,
                      const std::string &pin);

  /**
   * @description Platform status as a JSON object (firmware version, IP, memory…).
   */
  std::function<std::string()> status;

  /**
   * @description Blocking fetch of the HA entity list as a JSON array; empty when HA is not reachable.
   */
  std::function<std::string()> ha_entities;

  /**
   * @description Builds a {"error": message} response.
   */
  static HttpResponse error(int status, const std::string &message);

 private:
  App &app_;
  Settings &settings_;
};

}  // namespace dm
