// Open-Meteo client logic (transport-free): request URL and response parsing.
#pragma once

#include <string>

#include "cJSON.h"

namespace dm::weather {

// Refresh period of the forecast
constexpr int REFRESH_SECONDS = 15 * 60;

/**
 * @description Builds the Open-Meteo forecast URL for a location (no API key needed).
 */
std::string url(double lat, double lon);

/**
 * @description Parses an Open-Meteo response into the "weather" bus value:
 * {"name", "current": {"t", "feel", "code", "day", "rh", "wind"}, "daily": [{"wday", "code", "tmin", "tmax", "pp"}]}
 * @returns The new tree (caller owns it), or null when the response is unusable.
 */
cJSON *parse(const std::string &body, const std::string &place_name);

}  // namespace dm::weather
