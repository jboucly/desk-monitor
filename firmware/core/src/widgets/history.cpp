// ha_history: line chart of a numeric Home Assistant entity over the last hours, with its current value
// and the minimum and maximum of the period.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common.h"
#include "dm/ha.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

constexpr unsigned REFRESH_TICKS = 300;  // the series is fetched again every 5 minutes (ticks are seconds)
constexpr unsigned RETRY_TICKS = 15;     // until a first series arrives (HA not connected, clock not set…)

/**
 * @description "24 h", or "7 j" for whole days from two days on.
 */
std::string period_text(int hours) {
  if (hours >= 48 && hours % 24 == 0) return std::to_string(hours / 24) + " j";
  return std::to_string(hours) + " h";
}

class HistoryTile : public Widget {
 public:
  HistoryTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ctx_(ctx), spec_(spec) {
    entity_ = json::str(spec.props, "entity");
    hours_ = std::clamp(static_cast<int>(json::num(spec.props, "hours", 24)), 1, 168);
    decimals_ = std::clamp(static_cast<int>(json::num(spec.props, "decimals", 1)), 0, 3);
    scale_ = std::pow(10.0, decimals_);
    const char *hex = json::str(spec.props, "color");
    color_ = strlen(hex) == 7 && hex[0] == '#' ? lv_color_hex(strtoul(hex + 1, nullptr, 16)) : theme::accent();

    root = theme::make_card(parent);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 6, 0);

    // Header: icon, name, current value and unit
    lv_obj_t *head = make_box(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(head, 6, 0);
    theme::make_icon(head, &dm_icons_18, json::str(spec.props, "icon", default_entity_icon(entity_)), color_);
    name_ = theme::make_label(head, &dm_font_14, theme::text2());
    lv_obj_set_flex_grow(name_, 1);
    theme::single_line(name_);
    value_ = theme::make_label(head, spec.h >= 2 && spec.w >= 2 ? &dm_font_28 : &dm_font_20, theme::text(), "…");
    unit_ = theme::make_label(head, &dm_font_14, theme::text2());

    chart_ = lv_chart_create(root);
    lv_obj_set_width(chart_, lv_pct(100));
    lv_obj_set_flex_grow(chart_, 1);
    lv_obj_remove_flag(chart_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(chart_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart_, 0, 0);
    lv_obj_set_style_pad_all(chart_, 4, 0);
    lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart_, 3, 0);
    lv_obj_set_style_line_color(chart_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart_, 24, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart_, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(chart_, true, LV_PART_ITEMS);
    // No dots on the points: a plain line
    lv_obj_set_style_width(chart_, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart_, 0, LV_PART_INDICATOR);
    lv_chart_set_point_count(chart_, HaSession::HISTORY_POINTS);
    series_ = lv_chart_add_series(chart_, color_, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(chart_, series_, LV_CHART_POINT_NONE);

    // Footer: minimum and maximum of the period, and the period
    lv_obj_t *foot = make_box(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    range_ = theme::make_label(foot, &dm_font_14, theme::text3());
    theme::make_label(foot, &dm_font_14, theme::text3(), period_text(hours_).c_str());
    // A one-row tile keeps the chart only
    if (spec.h == 1) set_hidden(foot, true);

    lv_label_set_text(name_, entity_name(spec.props, nullptr, entity_).c_str());
    ctx.bus.subscribe("ha/" + entity_, [this](const cJSON *state) { render_state(state); });
    ctx.bus.subscribe("ha_hist/" + entity_ + "/" + std::to_string(hours_), [this](const cJSON *series) { render_series(series); });
    request();
  }

 private:
  void request() {
    if (ctx_.services.ha_history) ctx_.services.ha_history(entity_, hours_);
  }

  void tick() override {
    ticks_++;
    if (ticks_ % (has_series_ ? REFRESH_TICKS : RETRY_TICKS) == 0) request();
  }

  void render_state(const cJSON *state) {
    lv_label_set_text(name_, entity_name(spec_.props, state, entity_).c_str());
    const char *raw = json::str(state, "s");
    char *end = nullptr;
    const double value = strtod(raw, &end);
    const bool numeric = *raw && *end == '\0' && std::isfinite(value);
    lv_label_set_text(value_, is_unavailable(state) ? "—" : numeric ? util::format_number(value, decimals_).c_str() : raw);
    lv_label_set_text(unit_, json::str(spec_.props, "unit", json::str(json::get(state, "a"), "unit_of_measurement")));
    // The last point follows the live value between two fetches
    if (numeric && has_series_) {
      values_.back() = value;
      draw();
    }
  }

  void render_series(const cJSON *series) {
    values_.assign(HaSession::HISTORY_POINTS, NAN);
    int i = 0;
    const cJSON *v = nullptr;
    cJSON_ArrayForEach(v, json::get(series, "v")) {
      if (i >= HaSession::HISTORY_POINTS) break;
      values_[i++] = cJSON_IsNumber(v) ? v->valuedouble : NAN;
    }
    has_series_ = true;
    draw();
  }

  void draw() {
    double lo = INFINITY, hi = -INFINITY;
    for (double v : values_)
      if (!std::isnan(v)) lo = std::min(lo, v), hi = std::max(hi, v);
    if (!std::isfinite(lo)) {
      lv_chart_set_all_values(chart_, series_, LV_CHART_POINT_NONE);
      lv_label_set_text(range_, "Pas de valeurs numériques");
      return;
    }
    lv_label_set_text(range_, ("min " + util::format_number(lo, decimals_) + " · max " + util::format_number(hi, decimals_)).c_str());

    // Y axis: the layout's min/max when set, else the data with a margin so that the line does not touch the edges
    const double margin = std::max((hi - lo) * 0.12, 1.0 / scale_);
    const double axis_lo = json::num(spec_.props, "min", lo - margin);
    const double axis_hi = std::max(json::num(spec_.props, "max", hi + margin), axis_lo + 1.0 / scale_);
    lv_chart_set_axis_range(chart_, LV_CHART_AXIS_PRIMARY_Y, static_cast<int32_t>(std::floor(axis_lo * scale_)),
                            static_cast<int32_t>(std::ceil(axis_hi * scale_)));
    std::vector<int32_t> points(values_.size());
    for (size_t i = 0; i < values_.size(); i++)
      points[i] = std::isnan(values_[i]) ? LV_CHART_POINT_NONE : static_cast<int32_t>(std::lround(values_[i] * scale_));
    lv_chart_set_series_values(chart_, series_, points.data(), points.size());
    lv_chart_refresh(chart_);
  }

  WidgetContext &ctx_;
  WidgetSpec spec_;
  std::string entity_;
  int hours_ = 24, decimals_ = 1;
  double scale_ = 10;
  lv_color_t color_;
  bool has_series_ = false;
  unsigned ticks_ = 0;
  std::vector<double> values_;
  lv_obj_t *name_ = nullptr, *value_ = nullptr, *unit_ = nullptr, *chart_ = nullptr, *range_ = nullptr;
  lv_chart_series_t *series_ = nullptr;
};

}  // namespace

std::unique_ptr<Widget> create_ha_history(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<HistoryTile>(parent, spec, ctx);
}

}  // namespace dm::widgets
