#include "dm/util.h"

#include <cmath>
#include <cstdio>

namespace dm::util {

const char *weather_text(int code) {
  switch (code) {
    case 0: return "Ensoleillé";
    case 1: return "Plutôt dégagé";
    case 2: return "Partiellement nuageux";
    case 3: return "Couvert";
    case 45: case 48: return "Brouillard";
    case 51: case 53: case 55: return "Bruine";
    case 56: case 57: return "Bruine verglaçante";
    case 61: return "Pluie faible";
    case 63: return "Pluie";
    case 65: return "Forte pluie";
    case 66: case 67: return "Pluie verglaçante";
    case 71: return "Neige faible";
    case 73: return "Neige";
    case 75: return "Forte neige";
    case 77: return "Grains de neige";
    case 80: case 81: return "Averses";
    case 82: return "Fortes averses";
    case 85: case 86: return "Averses de neige";
    case 95: return "Orage";
    case 96: case 99: return "Orage et grêle";
    default: return "—";
  }
}

const char *weather_icon(int code, bool is_day) {
  switch (code) {
    case 0: return is_day ? "weather-sunny" : "weather-night";
    case 1: case 2: return is_day ? "weather-partly-cloudy" : "weather-night-partly-cloudy";
    case 3: return "weather-cloudy";
    case 45: case 48: return "weather-fog";
    case 51: case 53: case 55: case 61: case 63: case 80: case 81: return "weather-rainy";
    case 65: case 82: return "weather-pouring";
    case 56: case 57: case 66: case 67: return "weather-snowy-rainy";
    case 71: case 73: case 77: case 85: return "weather-snowy";
    case 75: case 86: return "weather-snowy-heavy";
    case 95: return "weather-lightning";
    case 96: case 99: return "weather-lightning-rainy";
    default: return "weather-cloudy";
  }
}

lv_color_t weather_color(int code, bool is_day) {
  if (code <= 1) return lv_color_hex(is_day ? 0xFFD166 : 0xC7D2FE);
  if (code == 2) return lv_color_hex(is_day ? 0xFFE3A3 : 0xC7D2FE);
  if (code >= 95) return lv_color_hex(0xFDE047);
  if (code >= 71 && code <= 86 && code != 80 && code != 81 && code != 82) return lv_color_hex(0xE0F2FE);
  if (code >= 51) return lv_color_hex(0x7DD3FC);
  return lv_color_hex(0xD6DCEB);
}

lv_color_t kelvin_to_rgb(float kelvin) {
  const float t = kelvin / 100.0f;
  float r, g, b;
  if (t <= 66.0f) {
    r = 255.0f;
    g = 99.4708025861f * logf(t) - 161.1195681661f;
    b = (t <= 19.0f) ? 0.0f : 138.5177312231f * logf(t - 10.0f) - 305.0447927307f;
  } else {
    r = 329.698727446f * powf(t - 60.0f, -0.1332047592f);
    g = 288.1221695283f * powf(t - 60.0f, -0.0755148492f);
    b = 255.0f;
  }
  auto clamp = [](float v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
  return lv_color_make(clamp(r), clamp(g), clamp(b));
}

lv_color_t hs_to_rgb(float hue, float saturation) {
  return lv_color_hsv_to_rgb(static_cast<uint16_t>(fmodf(hue + 360.0f, 360.0f)), static_cast<uint8_t>(saturation), 100);
}

const char *day_name(int wday, bool short_form) {
  static const char *const LONG[] = {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"};
  static const char *const SHORT[] = {"dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam."};
  wday = ((wday % 7) + 7) % 7;
  return short_form ? SHORT[wday] : LONG[wday];
}

const char *month_name(int month) {
  static const char *const NAMES[] = {"janvier", "février", "mars",      "avril",   "mai",      "juin",
                                      "juillet", "août",    "septembre", "octobre", "novembre", "décembre"};
  return NAMES[((month % 12) + 12) % 12];
}

std::string format_number(double value, int decimals) {
  if (std::isnan(value)) return "—";
  char buf[32];
  snprintf(buf, sizeof buf, "%.*f", decimals, value);
  for (char *p = buf; *p; p++)
    if (*p == '.') *p = ',';
  return buf;
}

}  // namespace dm::util
