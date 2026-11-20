#include "dm/api.h"

#include "dm/json.h"

namespace dm {

HttpResponse Api::error(int status, const std::string &message) {
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "error", message.c_str());
  HttpResponse response{status, json::print(body)};
  cJSON_Delete(body);
  return response;
}

HttpResponse Api::handle(const std::string &method, const std::string &path, const std::string &body,
                         const std::string &pin) {
  const bool write = method != "GET";
  if (write) {
    const std::string expected = settings_.get().pin;
    if (!expected.empty() && pin != expected) return error(401, "PIN incorrect");
  }

  if (path == "/api/layout") {
    if (method == "GET") return {200, app_.layout_json()};
    if (method == "PUT") {
      std::string problem;
      if (!app_.set_layout(body, problem)) return error(400, problem);
      return {200, R"({"ok":true})"};
    }
  }

  if (path == "/api/settings") {
    if (method == "GET") return {200, settings_.to_public_json()};
    if (method == "PUT") {
      std::string problem;
      if (!settings_.update(body, problem)) return error(400, problem);
      return {200, settings_.to_public_json()};
    }
  }

  if (path == "/api/status" && method == "GET") return {200, status ? status() : "{}"};

  if (path == "/api/printer" && method == "GET") return {200, printer_status ? printer_status() : R"({"state":"off"})"};

  if (path == "/api/ha/entities" && method == "GET") {
    std::string list = ha_entities ? ha_entities() : "";
    if (list.empty()) return error(503, "Home Assistant non connecté");
    return {200, list};
  }

  if (path == "/api/ha/template" && method == "POST") {
    auto request = json::parse(body);
    const char *tpl = json::str(request.get(), "template");
    if (!*tpl) return error(400, "template manquant");
    std::string out;
    if (!ha_render) return error(503, "Home Assistant non connecté");
    const bool ok = ha_render(tpl, out);
    cJSON *answer = cJSON_CreateObject();
    cJSON_AddStringToObject(answer, ok ? "result" : "error", out.c_str());
    HttpResponse response{ok ? 200 : 422, json::print(answer)};
    cJSON_Delete(answer);
    return response;
  }

  return error(404, "route inconnue");
}

}  // namespace dm
