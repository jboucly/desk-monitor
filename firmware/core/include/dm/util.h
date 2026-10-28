// Domain helpers: weather codes, light colors, French dates.
#pragma once

#include <ctime>
#include <cstdint>
#include <string>
#include <vector>

#include "lvgl.h"

namespace dm::util {

/**
 * @description French label of a WMO weather code (Open-Meteo "weather_code").
 */
const char *weather_text(int code);

/**
 * @description MDI icon name of a WMO weather code, with a night variant for clear and partly cloudy skies.
 */
const char *weather_icon(int code, bool is_day);

/**
 * @description Accent color of a weather code, used to tint the weather icon.
 */
lv_color_t weather_color(int code, bool is_day);

/**
 * @description Approximate RGB color of a white light at a temperature in Kelvin (Tanner Helland's algorithm).
 */
lv_color_t kelvin_to_rgb(float kelvin);

/**
 * @description Converts hue (0-360) and saturation (0-100) at full value to RGB.
 */
lv_color_t hs_to_rgb(float hue, float saturation);

/**
 * @description French day name ("lundi"…), or its 3-letter short form ("lun.").
 */
const char *day_name(int wday, bool short_form);

/**
 * @description French month name ("janvier"…).
 */
const char *month_name(int month);

/**
 * @description Formats a number with a fixed count of decimals, French decimal comma.
 */
std::string format_number(double value, int decimals);

/**
 * @description Formats a duration in seconds as "m:ss" or "h:mm:ss". NaN or negative gives "–:––".
 */
std::string format_duration(double seconds);

/**
 * @description Decodes standard base64 (padding optional, whitespace ignored).
 * @returns False on an invalid character.
 */
bool base64_decode(const char *text, std::vector<uint8_t> &out);

/**
 * @description Encodes bytes as standard base64 with padding.
 */
std::string base64_encode(const uint8_t *data, size_t len);

}  // namespace dm::util
