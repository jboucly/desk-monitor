// Periodic Open-Meteo fetch publishing the "weather" bus key.
#pragma once

#include "dm/bus.h"
#include "dm/settings.h"

namespace dm::esp32 {

/**
 * @description Starts the task: fetch when online, then every 15 minutes.
 */
void weather_start(Bus &bus, Settings &settings);

/**
 * @description Fetches again right away (after the location changed).
 */
void weather_refresh();

}  // namespace dm::esp32
