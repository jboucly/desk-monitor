// Album covers: JPEG decoding into the screen's pixel format (shared by the device and the simulator).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cJSON.h"

namespace dm {

/**
 * @description Decodes a JPEG, crops it to its centered square and scales it down to size x size
 * (box average), in RGB565 little-endian (LVGL's LV_COLOR_FORMAT_RGB565). Any thread.
 * @returns False when the data is not a baseline JPEG or is smaller than `size`.
 */
bool decode_jpeg_cover(const std::string &jpeg, int size, std::vector<uint8_t> &rgb565);

/**
 * @description Builds the bus value of a cover, {"id", "w", "h", "data": base64 RGB565},
 * the same shape as the PC companion's "media_art".
 */
cJSON *cover_json(const std::string &id, int size, const std::vector<uint8_t> &rgb565);

}  // namespace dm
