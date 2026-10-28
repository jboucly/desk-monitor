// weather: current conditions or a daily forecast from the "weather" bus key (Open-Meteo).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

/**
 * @description "12°" with a rounded temperature, "—" when unknown.
 */
std::string degrees(double value) {
  if (std::isnan(value)) return "—";
  return std::to_string(static_cast<int>(std::lround(value))) + "°";
}

lv_obj_t *make_column(lv_obj_t *parent, lv_flex_align_t cross = LV_FLEX_ALIGN_START) {
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, cross, cross);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
  return col;
}

/**
 * @description Current conditions. Three layouts: compact (1 cell), wide (one row) and big (2x2 and more).
 */
class CurrentWeather : public Widget {
 public:
  CurrentWeather(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
    root = theme::make_card(parent);
    big_ = spec.w >= 2 && spec.h >= 2;
    const bool wide = !big_ && spec.w >= 2;

    if (big_) {
      icon_ = theme::make_icon(root, &dm_icons_72, "weather-cloudy", theme::text());
      lv_obj_align(icon_, LV_ALIGN_TOP_LEFT, -4, -6);
      place_ = theme::make_label(root, &dm_font_14, theme::text3());
      lv_obj_align(place_, LV_ALIGN_TOP_RIGHT, 0, 0);
      temp_ = theme::make_label(root, &dm_font_40, theme::text(), "—");
      lv_obj_align(temp_, LV_ALIGN_TOP_RIGHT, 0, 20);
      text_ = theme::make_label(root, &dm_font_16, theme::text());
      lv_obj_set_width(text_, lv_pct(100));
      theme::single_line(text_);
      lv_obj_align(text_, LV_ALIGN_BOTTOM_LEFT, 0, -42);
      details_ = theme::make_label(root, &dm_font_14, theme::text2());
      lv_obj_align(details_, LV_ALIGN_BOTTOM_LEFT, 0, -20);
      range_ = theme::make_label(root, &dm_font_14, theme::text2());
      lv_obj_align(range_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else if (wide) {
      lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(root, 10, 0);
      icon_ = theme::make_icon(root, &dm_icons_48, "weather-cloudy", theme::text());
      lv_obj_t *col = make_column(root);
      lv_obj_set_flex_grow(col, 1);
      temp_ = theme::make_label(col, &dm_font_28, theme::text(), "—");
      text_ = theme::make_label(col, &dm_font_14, theme::text2());
      lv_obj_set_width(text_, lv_pct(100));
      theme::single_line(text_);
    } else {
      lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      icon_ = theme::make_icon(root, &dm_icons_32, "weather-cloudy", theme::text());
      temp_ = theme::make_label(root, &dm_font_20, theme::text(), "—");
    }
    ctx.bus.subscribe("weather", [this](const cJSON *w) { render(w); });
  }

 private:
  void render(const cJSON *w) {
    const cJSON *now = json::get(w, "current");
    const int code = static_cast<int>(json::num(now, "code", -1));
    const bool day = json::boolean(now, "day", true);
    lv_label_set_text(icon_, theme::icon(util::weather_icon(code, day)));
    lv_obj_set_style_text_color(icon_, util::weather_color(code, day), 0);
    lv_label_set_text(temp_, degrees(json::num(now, "t", NAN)).c_str());
    if (text_) lv_label_set_text(text_, util::weather_text(code));
    if (!big_) return;

    lv_label_set_text(place_, json::str(w, "name"));
    const double feel = json::num(now, "feel", NAN), rh = json::num(now, "rh", NAN), wind = json::num(now, "wind", NAN);
    lv_label_set_text_fmt(details_, "Ressenti %s  ·  %d %%  ·  %d km/h", degrees(feel).c_str(),
                          std::isnan(rh) ? 0 : static_cast<int>(rh), std::isnan(wind) ? 0 : static_cast<int>(std::lround(wind)));
    const cJSON *today = cJSON_GetArrayItem(json::get(w, "daily"), 0);
    lv_label_set_text_fmt(range_, "Min %s  ·  Max %s", degrees(json::num(today, "tmin", NAN)).c_str(),
                          degrees(json::num(today, "tmax", NAN)).c_str());
  }

  bool big_ = false;
  lv_obj_t *icon_ = nullptr, *temp_ = nullptr, *text_ = nullptr, *place_ = nullptr, *details_ = nullptr,
           *range_ = nullptr;
};

/**
 * @description Daily forecast: one column per day (day name, icon, max, min).
 */
class Forecast : public Widget {
 public:
  Forecast(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
    root = theme::make_card(parent);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // As many days as asked, but never more than fit (about one per 60 px)
    const int fit = std::max(1, theme::span_width(spec.w) / 60);
    const int days = std::clamp(static_cast<int>(json::num(spec.props, "days", 4)), 1, std::min(7, fit));
    const bool tall = spec.h >= 2;
    for (int i = 0; i < days; i++) {
      Day d;
      lv_obj_t *col = make_column(root, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(col, tall ? 4 : 0, 0);
      d.name = theme::make_label(col, &dm_font_14, theme::text2(), "—");
      d.icon = theme::make_icon(col, tall ? &dm_icons_32 : &dm_icons_24, "weather-cloudy", theme::text());
      d.max = theme::make_label(col, &dm_font_16, theme::text(), "—");
      d.min = theme::make_label(col, &dm_font_14, theme::text3(), "—");
      if (!tall) lv_obj_add_flag(d.min, LV_OBJ_FLAG_HIDDEN);
      days_.push_back(d);
    }
    ctx.bus.subscribe("weather", [this](const cJSON *w) { render(w); });
  }

 private:
  struct Day {
    lv_obj_t *name, *icon, *max, *min;
  };

  void render(const cJSON *w) {
    const cJSON *daily = json::get(w, "daily");
    for (size_t i = 0; i < days_.size(); i++) {
      const cJSON *day = cJSON_GetArrayItem(daily, static_cast<int>(i));
      const int code = static_cast<int>(json::num(day, "code", -1));
      lv_label_set_text(days_[i].name, i == 0 ? "auj." : util::day_name(static_cast<int>(json::num(day, "wday")), true));
      lv_label_set_text(days_[i].icon, theme::icon(util::weather_icon(code, true)));
      lv_obj_set_style_text_color(days_[i].icon, util::weather_color(code, true), 0);
      lv_label_set_text(days_[i].max, degrees(json::num(day, "tmax", NAN)).c_str());
      lv_label_set_text(days_[i].min, degrees(json::num(day, "tmin", NAN)).c_str());
    }
  }

  std::vector<Day> days_;
};

}  // namespace

std::unique_ptr<Widget> create_weather(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  if (!strcmp(json::str(spec.props, "mode", "current"), "forecast")) return std::make_unique<Forecast>(parent, spec, ctx);
  return std::make_unique<CurrentWeather>(parent, spec, ctx);
}

}  // namespace dm::widgets
