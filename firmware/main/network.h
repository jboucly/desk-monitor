// Wi-Fi (station, or setup access point), SNTP time, mDNS name, and the "sys/wifi" bus status.
#pragma once

#include <string>

#include "dm/bus.h"
#include "dm/settings.h"

namespace dm::esp32 {

// Access point opened when no Wi-Fi is configured (or the saved one stays out of reach)
constexpr const char *SETUP_AP_SSID = "DeskMonitor-setup";

/**
 * @description Starts Wi-Fi with the saved credentials, or the setup access point when there are none.
 */
void network_start(Bus &bus, const SettingsData &settings);

/**
 * @description Applies new Wi-Fi credentials or a new time zone.
 */
void network_reconfigure(const SettingsData &settings, unsigned changes);

/**
 * @description True once the station has an IP address.
 */
bool network_online();

/**
 * @description Blocks until the station is online or the timeout expires. Returns network_online().
 */
bool network_wait_online(uint32_t timeout_ms);

/**
 * @description Current IPv4 address as text ("" when offline).
 */
std::string network_ip();

/**
 * @description Current station RSSI in dBm, 0 when offline.
 */
int network_rssi();

}  // namespace dm::esp32
