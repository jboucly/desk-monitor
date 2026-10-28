// WebSocket transport of the Home Assistant session (esp_websocket_client).
#pragma once

#include <functional>
#include <string>

#include "dm/ha.h"
#include "dm/settings.h"

namespace dm::esp32 {

/**
 * @description (Re)connects to Home Assistant with the URL and token of the settings.
 * Without a URL or a token the client stays off and "sys/ha" reports "off".
 */
void ha_client_start(HaSession &session, const SettingsData &settings);

/**
 * @description Fetches the entity list for the editor. Blocks up to 8 s; empty string on failure.
 */
std::string ha_client_entities();

/**
 * @description Downloads a Home Assistant resource (e.g. "/api/media_player_proxy/…?token=…") from a
 * background task, then calls `done` from that task with the body (empty on failure, 1 MB at most).
 */
void ha_fetch(const std::string &path, std::function<void(std::string body)> done);

}  // namespace dm::esp32
