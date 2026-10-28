// ha_gauge: 270° arc showing a numeric Home Assistant sensor between min and max.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

class Gauge : public Widget {
 public:
  Gauge(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : spec_(spec) {
    entity_ = json::str(spec.props, "entity");
    min_ = json::num(spec.props, "min", 0);
    max_ = json::num(spec.props, "max", 100);
    if (max_ <= min_) max_ = min_ + 1;
    warn_ = json::num(spec.props, "warn", NAN);
    crit_ = json::num(spec.props, "crit", NAN);
    decimals_ = static_cast<int>(json::num(spec.props, "decimals", 0));

    root = theme::make_card(parent);
    lv_obj_set_style_pad_all(root, 8, 0);

    // The arc fills the card's smallest side, keeping room for the name at the bottom
    const int inner_w = theme::span_width(spec.w) - 16;
    const int inner_h = theme::span_height(spec.h) - 16 - 18;
    const int size = std::max(48, std::min(inner_w, inner_h));
    const bool small = size < 110;

    arc_ = lv_arc_create(root);
    lv_obj_set_size(arc_, size, size);
    lv_obj_align(arc_, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(arc_, 135);
    lv_arc_set_bg_angles(arc_, 0, 270);
    lv_arc_set_range(arc_, 0, 1000);
    lv_arc_set_value(arc_, 0);
    lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
    const int width = small ? 7 : 10;
    lv_obj_set_style_arc_width(arc_, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_, 40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_, theme::accent(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_MAIN);

    value_ = theme::make_label(arc_, small ? &dm_font_20 : &dm_font_28, theme::text(), "…");
    lv_obj_align(value_, LV_ALIGN_CENTER, 0, small ? 0 : -4);
    unit_ = theme::make_label(arc_, &dm_font_14, theme::text2());
    lv_obj_align_to(unit_, value_, LV_ALIGN_OUT_BOTTOM_MID, 0, -2);

    name_ = theme::make_label(root, &dm_font_14, theme::text2());
    lv_obj_set_width(name_, lv_pct(100));
    lv_obj_set_style_text_align(name_, LV_TEXT_ALIGN_CENTER, 0);
    theme::single_line(name_);
    lv_obj_align(name_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_label_set_text(name_, entity_name(spec.props, nullptr, entity_).c_str());

    ctx.bus.subscribe("ha/" + entity_, [this](const cJSON *state) { render(state); });
  }

 private:
  void render(const cJSON *state) {
    lv_label_set_text(name_, entity_name(spec_.props, state, entity_).c_str());
    const char *raw = json::str(state, "s");
    char *end = nullptr;
    const double value = strtod(raw, &end);
    const bool valid = !is_unavailable(state) && end != raw && *end == '\0';
    lv_label_set_text(value_, valid ? util::format_number(value, decimals_).c_str() : "—");
    lv_label_set_text(unit_, json::str(spec_.props, "unit", json::str(json::get(state, "a"), "unit_of_measurement")));
    lv_obj_align_to(unit_, value_, LV_ALIGN_OUT_BOTTOM_MID, 0, -2);

    const double ratio = valid ? std::clamp((value - min_) / (max_ - min_), 0.0, 1.0) : 0.0;
    lv_arc_set_value(arc_, static_cast<int32_t>(ratio * 1000));
    // Without explicit thresholds, the arc stays in the accent color
    const float warn_at = std::isnan(warn_) ? INFINITY : static_cast<float>(warn_);
    const float crit_at = std::isnan(crit_) ? INFINITY : static_cast<float>(crit_);
    lv_obj_set_style_arc_color(arc_, theme::level(valid ? value : NAN, warn_at, crit_at, theme::accent()),
                               LV_PART_INDICATOR);
  }

  WidgetSpec spec_;
  std::string entity_;
  double min_, max_, warn_, crit_;
  int decimals_;
  lv_obj_t *arc_ = nullptr, *value_ = nullptr, *unit_ = nullptr, *name_ = nullptr;
};

}  // namespace

std::unique_ptr<Widget> create_ha_gauge(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<Gauge>(parent, spec, ctx);
}

}  // namespace dm::widgets
