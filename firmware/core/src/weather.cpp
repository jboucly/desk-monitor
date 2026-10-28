#include "dm/weather.h"

#include <cmath>
#include <cstdio>

#include "dm/json.h"

namespace dm::weather {

namespace {

/**
 * @description Day of week (0 = Sunday) of an ISO date "YYYY-MM-DD" (Sakamoto's method). -1 when malformed.
 */
int iso_weekday(const char *date) {
  int y, m, d;
  if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) return -1;
  static const int OFFSETS[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + OFFSETS[(m - 1) % 12] + d) % 7;
}

/**
 * @description Number at index `i` of an array member, NAN when missing.
 */
double at(const cJSON *obj, const char *key, int i) {
  const cJSON *item = cJSON_GetArrayItem(json::get(obj, key), i);
  return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

}  // namespace

std::string url(double lat, double lon) {
  char buf[512];
  snprintf(buf, sizeof buf,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=auto&forecast_days=7",
           lat, lon);
  return buf;
}

cJSON *parse(const std::string &body, const std::string &place_name) {
  auto root = json::parse(body);
  const cJSON *current = json::get(root.get(), "current");
  const cJSON *daily = json::get(root.get(), "daily");
  if (!current || !daily) return nullptr;

  cJSON *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "name", place_name.c_str());

  cJSON *now = cJSON_AddObjectToObject(out, "current");
  cJSON_AddNumberToObject(now, "t", json::num(current, "temperature_2m", NAN));
  cJSON_AddNumberToObject(now, "feel", json::num(current, "apparent_temperature", NAN));
  cJSON_AddNumberToObject(now, "code", json::num(current, "weather_code", -1));
  cJSON_AddBoolToObject(now, "day", json::num(current, "is_day", 1) != 0);
  cJSON_AddNumberToObject(now, "rh", json::num(current, "relative_humidity_2m", NAN));
  cJSON_AddNumberToObject(now, "wind", json::num(current, "wind_speed_10m", NAN));

  cJSON *days = cJSON_AddArrayToObject(out, "daily");
  const cJSON *dates = json::get(daily, "time");
  for (int i = 0; i < cJSON_GetArraySize(dates); i++) {
    const cJSON *date = cJSON_GetArrayItem(dates, i);
    cJSON *day = cJSON_CreateObject();
    cJSON_AddNumberToObject(day, "wday", iso_weekday(cJSON_IsString(date) ? date->valuestring : ""));
    cJSON_AddNumberToObject(day, "code", at(daily, "weather_code", i));
    cJSON_AddNumberToObject(day, "tmin", at(daily, "temperature_2m_min", i));
    cJSON_AddNumberToObject(day, "tmax", at(daily, "temperature_2m_max", i));
    cJSON_AddNumberToObject(day, "pp", at(daily, "precipitation_probability_max", i));
    cJSON_AddItemToArray(days, day);
  }
  return out;
}

}  // namespace dm::weather
