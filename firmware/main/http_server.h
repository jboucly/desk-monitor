// HTTP server: web editor (embedded, gzipped), REST API, firmware upload (OTA).
#pragma once

#include "dm/api.h"
#include "dm/settings.h"

namespace dm::esp32 {

/**
 * @description Starts the server on port 80.
 */
void http_start(Api &api, Settings &settings);

}  // namespace dm::esp32
