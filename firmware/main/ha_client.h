// WebSocket transport of the Home Assistant session (esp_websocket_client).
#pragma once

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

}  // namespace dm::esp32
