// WebSocket transport of the 3D printer session (esp_websocket_client).
#pragma once

#include "dm/printer.h"
#include "dm/settings.h"

namespace dm::esp32 {

/**
 * @description (Re)connects to the printer of the settings. Without a model or an address the client stays
 * off and the "printer" bus key reports "off".
 */
void printer_client_start(PrinterSession &session, const SettingsData &settings);

}  // namespace dm::esp32
