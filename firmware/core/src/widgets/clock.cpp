// clock: big time with the French date, drawn straight on the wallpaper unless props.card is true.
#include <ctime>

#include "common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

class Clock : public Widget {
 public:
  Clock(lv_obj_t *parent, const WidgetSpec &spec) {
    show_date_ = json::boolean(spec.props, "show_date", true);
    show_seconds_ = json::boolean(spec.props, "show_seconds", false);

    if (json::boolean(spec.props, "card", false)) {
      root = theme::make_card(parent);
    } else {
      root = lv_obj_create(parent);
      lv_obj_remove_style_all(root);
      lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(root, 0, 0);

    // Font sizes follow the space the widget gets on the grid
    const lv_font_t *time_font = &dm_font_28;
    if (spec.w >= 3 && spec.h >= 2 && !show_seconds_) time_font = &dm_font_72;
    else if (spec.w >= 2) time_font = &dm_font_40;
    time_ = theme::make_label(root, time_font, theme::text(), "--:--");
    lv_obj_set_style_text_letter_space(time_, time_font == &dm_font_72 ? -2 : 0, 0);
    date_ = theme::make_label(root, spec.w >= 3 ? &dm_font_20 : &dm_font_14, theme::text2());
    if (!show_date_ || spec.h * spec.w < 2) lv_obj_add_flag(date_, LV_OBJ_FLAG_HIDDEN);
    tick();
  }

  void tick() override {
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) return;  // not synchronized yet
    if (show_seconds_) lv_label_set_text_fmt(time_, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    else lv_label_set_text_fmt(time_, "%02d:%02d", t.tm_hour, t.tm_min);
    if (show_date_) {
      lv_label_set_text_fmt(date_, "%s %d %s", util::day_name(t.tm_wday, false), t.tm_mday, util::month_name(t.tm_mon));
    }
  }

 private:
  bool show_date_ = true, show_seconds_ = false;
  lv_obj_t *time_ = nullptr, *date_ = nullptr;
};

}  // namespace

std::unique_ptr<Widget> create_clock(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &) {
  return std::make_unique<Clock>(parent, spec);
}

}  // namespace dm::widgets
