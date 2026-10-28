// HTTP server: web editor (embedded, gzipped), REST API, firmware upload (OTA), PC link (WebSocket).
#pragma once

#include "dm/api.h"
#include "dm/pc.h"
#include "dm/settings.h"

namespace dm::esp32 {

/**
 * @description Starts the server on port 80. The PC companion connects to ws://<screen>/api/pc.
 */
void http_start(Api &api, Settings &settings, PcSession &pc);

}  // namespace dm::esp32
