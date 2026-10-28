// Light popup: on/off switch, color wheel (hue = angle, saturation = radius), white temperature and brightness.
// Opened by a long press on an ha_light tile; commands go out through light.turn_on / light.turn_off.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

constexpr int SHEET_W = 440;
constexpr int SHEET_H = 430;
constexpr int WHEEL = 196;  // color wheel diameter
constexpr int KNOB = 26;

/**
 * @description Color wheel image (ARGB8888, anti-aliased edge). Generated once and kept for later popups.
 */
lv_draw_buf_t *wheel_image() {
  static lv_draw_buf_t *buf = nullptr;
  if (buf) return buf;
  buf = lv_draw_buf_create(WHEEL, WHEEL, LV_COLOR_FORMAT_ARGB8888, 0);
  if (!buf) return nullptr;
  const float radius = WHEEL / 2.0f, center = (WHEEL - 1) / 2.0f;
  for (int y = 0; y < WHEEL; y++) {
    auto *row = reinterpret_cast<lv_color32_t *>(lv_draw_buf_goto_xy(buf, 0, y));
    for (int x = 0; x < WHEEL; x++) {
      const float dx = x - center, dy = y - center;
      const float r = sqrtf(dx * dx + dy * dy);
      const float alpha = std::clamp(radius - r, 0.0f, 1.0f);
      // Hue 0° at the top, clockwise; saturation grows towards the edge
      const float hue = fmodf(atan2f(dx, -dy) * 180.0f / static_cast<float>(M_PI) + 360.0f, 360.0f);
      const lv_color_t c = util::hs_to_rgb(hue, std::min(r / radius, 1.0f) * 100.0f);
      row[x] = {c.blue, c.green, c.red, static_cast<uint8_t>(alpha * 255)};
    }
  }
  return buf;
}

/**
 * @description Supported features of a light, read from supported_color_modes.
 */
struct Features {
  bool brightness = false, color = false, temp = false;
};

Features features_of(const cJSON *state) {
  Features f;
  const cJSON *mode = nullptr;
  cJSON_ArrayForEach(mode, json::get(json::get(state, "a"), "supported_color_modes")) {
    const char *m = cJSON_IsString(mode) ? mode->valuestring : "";
    for (const char *c : {"hs", "rgb", "rgbw", "rgbww", "xy"}) f.color |= !strcmp(m, c);
    f.temp |= !strcmp(m, "color_temp");
    f.brightness |= strcmp(m, "onoff") != 0;
  }
  return f;
}

/**
 * @description Styles a slider as a thick glass track with a round white knob.
 */
void style_slider(lv_obj_t *slider) {
  lv_obj_set_height(slider, 40);
  lv_obj_set_style_radius(slider, 20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, 36, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, 20, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, -4, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(slider, 12, LV_PART_KNOB);
  lv_obj_set_style_shadow_opa(slider, LV_OPA_40, LV_PART_KNOB);
}

/**
 * @description One open popup. Deleted when its sheet closes.
 */
class LightPopup {
 public:
  LightPopup(WidgetContext &ctx, std::string entity_id, const std::string &name, const char *icon)
      : ctx_(ctx), entity_(std::move(entity_id)) {
    const cJSON *state = ctx_.bus.get("ha/" + entity_);
    features_ = features_of(state);
    mode_color_ = features_.color && strcmp(json::str(json::get(state, "a"), "color_mode"), "color_temp") != 0;
    if (!features_.color) mode_color_ = false;

    panel_ = ctx_.app.open_sheet(SHEET_W, SHEET_H, [this] { delete this; });
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel_, 14, 0);

    build_header(name, icon);
    if (features_.color && features_.temp) build_tabs();
    build_body();
    if (features_.brightness) build_brightness();

    token_ = ctx_.bus.subscribe("ha/" + entity_, [this](const cJSON *s) { render(s); });
  }

  ~LightPopup() { ctx_.bus.unsubscribe(token_); }

 private:
  lv_obj_t *row(lv_obj_t *parent) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
  }

  void build_header(const std::string &name, const char *icon) {
    lv_obj_t *head = row(panel_);
    bubble_ = lv_obj_create(head);
    lv_obj_remove_style_all(bubble_);
    lv_obj_add_style(bubble_, theme::bubble(), 0);
    lv_obj_set_size(bubble_, 48, 48);
    icon_ = theme::make_icon(bubble_, &dm_icons_24, icon, theme::text());
    lv_obj_center(icon_);

    lv_obj_t *col = lv_obj_create(head);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *title = theme::make_label(col, &dm_font_20, theme::text(), name.c_str());
    lv_obj_set_width(title, lv_pct(100));
    theme::single_line(title);
    status_ = theme::make_label(col, &dm_font_14, theme::text2());

    switch_ = lv_switch_create(head);
    lv_obj_set_size(switch_, 60, 32);
    lv_obj_set_style_bg_color(switch_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(switch_, 50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(switch_, theme::accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(
        switch_,
        [](lv_event_t *e) {
          auto *self = static_cast<LightPopup *>(lv_event_get_user_data(e));
          const bool on = lv_obj_has_state(self->switch_, LV_STATE_CHECKED);
          self->ctx_.services.ha_call("light", on ? "turn_on" : "turn_off", self->entity_, nullptr);
        },
        LV_EVENT_VALUE_CHANGED, this);
  }

  void build_tabs() {
    lv_obj_t *tabs = row(panel_);
    lv_obj_set_style_pad_column(tabs, 8, 0);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 2; i++) {
      lv_obj_t *tab = lv_obj_create(tabs);
      lv_obj_remove_style_all(tab);
      lv_obj_set_size(tab, 120, 34);
      lv_obj_set_style_radius(tab, 17, 0);
      lv_obj_set_style_bg_color(tab, lv_color_white(), 0);
      lv_obj_set_style_bg_opa(tab, 30, 0);
      lv_obj_set_style_bg_color(tab, theme::accent(), LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_CHECKED);
      lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_t *label = theme::make_label(tab, &dm_font_16, theme::text(), i == 0 ? "Couleur" : "Blanc");
      lv_obj_center(label);
      lv_obj_add_event_cb(
          tab,
          [](lv_event_t *e) {
            auto *self = static_cast<LightPopup *>(lv_event_get_user_data(e));
            self->mode_color_ = lv_event_get_current_target(e) == self->tabs_[0];
            self->show_mode();
          },
          LV_EVENT_CLICKED, this);
      tabs_[i] = tab;
    }
  }

  void build_body() {
    body_ = lv_obj_create(panel_);
    lv_obj_remove_style_all(body_);
    lv_obj_set_width(body_, lv_pct(100));
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    if (features_.color) {
      wheel_ = lv_image_create(body_);
      lv_image_set_src(wheel_, wheel_image());
      lv_obj_center(wheel_);
      lv_obj_add_flag(wheel_, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(wheel_, on_wheel, LV_EVENT_PRESSING, this);
      lv_obj_add_event_cb(wheel_, on_wheel, LV_EVENT_PRESSED, this);
      lv_obj_add_event_cb(wheel_, on_wheel, LV_EVENT_RELEASED, this);

      knob_ = lv_obj_create(wheel_);
      lv_obj_remove_style_all(knob_);
      lv_obj_set_size(knob_, KNOB, KNOB);
      lv_obj_set_style_radius(knob_, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_color(knob_, lv_color_white(), 0);
      lv_obj_set_style_border_width(knob_, 3, 0);
      lv_obj_set_style_bg_opa(knob_, LV_OPA_COVER, 0);
      lv_obj_set_style_shadow_width(knob_, 10, 0);
      lv_obj_set_style_shadow_opa(knob_, LV_OPA_50, 0);
      lv_obj_remove_flag(knob_, LV_OBJ_FLAG_CLICKABLE);
    }

    if (features_.temp) {
      const cJSON *attrs = json::get(ctx_.bus.get("ha/" + entity_), "a");
      kelvin_min_ = static_cast<int>(json::num(attrs, "min_color_temp_kelvin", 2000));
      kelvin_max_ = static_cast<int>(json::num(attrs, "max_color_temp_kelvin", 6500));
      if (kelvin_max_ <= kelvin_min_) kelvin_max_ = kelvin_min_ + 1;

      temp_box_ = lv_obj_create(body_);
      lv_obj_remove_style_all(temp_box_);
      lv_obj_set_size(temp_box_, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_center(temp_box_);
      lv_obj_set_flex_flow(temp_box_, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(temp_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(temp_box_, 10, 0);
      kelvin_label_ = theme::make_label(temp_box_, &dm_font_28, theme::text(), "—");

      kelvin_ = lv_slider_create(temp_box_);
      lv_obj_set_width(kelvin_, lv_pct(100));
      style_slider(kelvin_);
      lv_obj_set_height(kelvin_, 48);
      lv_obj_set_style_radius(kelvin_, 24, LV_PART_MAIN);
      lv_slider_set_range(kelvin_, kelvin_min_, kelvin_max_);
      // Warm to cool gradient on the track; the indicator stays invisible
      lv_obj_set_style_bg_opa(kelvin_, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_bg_color(kelvin_, util::kelvin_to_rgb(static_cast<float>(kelvin_min_)), LV_PART_MAIN);
      lv_obj_set_style_bg_grad_color(kelvin_, util::kelvin_to_rgb(static_cast<float>(kelvin_max_)), LV_PART_MAIN);
      lv_obj_set_style_bg_grad_dir(kelvin_, LV_GRAD_DIR_HOR, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(kelvin_, LV_OPA_TRANSP, LV_PART_INDICATOR);
      lv_obj_set_style_border_color(kelvin_, lv_color_black(), LV_PART_KNOB);
      lv_obj_set_style_border_opa(kelvin_, LV_OPA_20, LV_PART_KNOB);
      lv_obj_set_style_border_width(kelvin_, 2, LV_PART_KNOB);
      lv_obj_add_event_cb(kelvin_, on_kelvin, LV_EVENT_VALUE_CHANGED, this);
      lv_obj_add_event_cb(kelvin_, on_kelvin, LV_EVENT_RELEASED, this);

      lv_obj_t *legend = row(temp_box_);
      lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      theme::make_label(legend, &dm_font_14, theme::text2(), "Chaud");
      theme::make_label(legend, &dm_font_14, theme::text2(), "Froid");
    }

    if (!features_.color && !features_.temp) {
      lv_obj_t *hint = theme::make_label(body_, &dm_font_16, theme::text2(), "Cette lumière ne gère que l'allumage.");
      lv_obj_center(hint);
    }
    show_mode();
  }

  void build_brightness() {
    lv_obj_t *r = row(panel_);
    theme::make_icon(r, &dm_icons_24, "brightness-6", theme::text2());
    brightness_ = lv_slider_create(r);
    lv_obj_set_flex_grow(brightness_, 1);
    style_slider(brightness_);
    lv_slider_set_range(brightness_, 1, 100);
    lv_obj_set_style_bg_grad_dir(brightness_, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_add_event_cb(brightness_, on_brightness, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(brightness_, on_brightness, LV_EVENT_RELEASED, this);
    brightness_label_ = theme::make_label(r, &dm_font_16, theme::text(), "—");
    lv_obj_set_width(brightness_label_, 52);
    lv_obj_set_style_text_align(brightness_label_, LV_TEXT_ALIGN_RIGHT, 0);
  }

  void show_mode() {
    if (wheel_) set_hidden(wheel_, !mode_color_);
    if (temp_box_) set_hidden(temp_box_, mode_color_);
    for (int i = 0; i < 2; i++) {
      if (!tabs_[i]) continue;
      if ((i == 0) == mode_color_) lv_obj_add_state(tabs_[i], LV_STATE_CHECKED);
      else lv_obj_remove_state(tabs_[i], LV_STATE_CHECKED);
    }
  }

  static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }

  /**
   * @description Syncs the controls with the entity state, except the one being dragged.
   */
  void render(const cJSON *state) {
    const cJSON *attrs = json::get(state, "a");
    const bool on = is_active(state);
    const lv_color_t color = light_color(state);

    if (on) lv_obj_add_state(switch_, LV_STATE_CHECKED);
    else lv_obj_remove_state(switch_, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bubble_, on ? 235 : 40, 0);
    lv_obj_set_style_bg_color(bubble_, on ? color : lv_color_white(), 0);
    lv_obj_set_style_text_color(icon_, on ? lv_color_mix(color, lv_color_black(), 140) : theme::text(), 0);

    const int pct = static_cast<int>(std::lround(json::num(attrs, "brightness", 0) * 100.0 / 255.0));
    lv_label_set_text(status_, on ? (features_.brightness ? (std::to_string(pct) + " %").c_str() : "Allumée") : "Éteinte");

    if (brightness_ && !dragging_) {
      lv_slider_set_value(brightness_, on ? std::max(pct, 1) : 1, LV_ANIM_OFF);
      lv_label_set_text_fmt(brightness_label_, "%d %%", on ? pct : 0);
    }
    if (brightness_) {
      lv_obj_set_style_bg_color(brightness_, lv_color_mix(color, lv_color_black(), 110), LV_PART_INDICATOR);
      lv_obj_set_style_bg_grad_color(brightness_, color, LV_PART_INDICATOR);
    }

    if (wheel_ && !dragging_) {
      const cJSON *hs = json::get(attrs, "hs_color");
      if (cJSON_GetArraySize(hs) == 2) {
        hue_ = static_cast<float>(cJSON_GetArrayItem(hs, 0)->valuedouble);
        sat_ = static_cast<float>(cJSON_GetArrayItem(hs, 1)->valuedouble);
      }
      place_knob();
    }
    if (kelvin_ && !dragging_) {
      const int k = static_cast<int>(json::num(attrs, "color_temp_kelvin", (kelvin_min_ + kelvin_max_) / 2));
      lv_slider_set_value(kelvin_, k, LV_ANIM_OFF);
      lv_label_set_text_fmt(kelvin_label_, "%d K", k);
    }
  }

  void place_knob() {
    const float radius = WHEEL / 2.0f, angle = hue_ * static_cast<float>(M_PI) / 180.0f, dist = sat_ / 100.0f * radius;
    lv_obj_set_pos(knob_, static_cast<int32_t>(radius + sinf(angle) * dist - KNOB / 2.0f),
                   static_cast<int32_t>(radius - cosf(angle) * dist - KNOB / 2.0f));
    lv_obj_set_style_bg_color(knob_, util::hs_to_rgb(hue_, sat_), 0);
  }

  static void on_wheel(lv_event_t *e) {
    auto *self = static_cast<LightPopup *>(lv_event_get_user_data(e));
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t area;
    lv_obj_get_coords(self->wheel_, &area);
    const float radius = WHEEL / 2.0f;
    const float dx = p.x - area.x1 - radius, dy = p.y - area.y1 - radius;
    self->hue_ = fmodf(atan2f(dx, -dy) * 180.0f / static_cast<float>(M_PI) + 360.0f, 360.0f);
    self->sat_ = std::min(sqrtf(dx * dx + dy * dy) / radius, 1.0f) * 100.0f;
    self->place_knob();
    self->dragging_ = lv_event_get_code(e) != LV_EVENT_RELEASED;
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    cJSON *data = cJSON_CreateObject();
    cJSON *hs = cJSON_AddArrayToObject(data, "hs_color");
    cJSON_AddItemToArray(hs, cJSON_CreateNumber(std::round(self->hue_)));
    cJSON_AddItemToArray(hs, cJSON_CreateNumber(std::round(self->sat_)));
    self->ctx_.services.ha_call("light", "turn_on", self->entity_, data);
  }

  static void on_kelvin(lv_event_t *e) {
    auto *self = static_cast<LightPopup *>(lv_event_get_user_data(e));
    const int k = lv_slider_get_value(self->kelvin_);
    lv_label_set_text_fmt(self->kelvin_label_, "%d K", k);
    self->dragging_ = lv_event_get_code(e) != LV_EVENT_RELEASED;
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "color_temp_kelvin", k);
    self->ctx_.services.ha_call("light", "turn_on", self->entity_, data);
  }

  static void on_brightness(lv_event_t *e) {
    auto *self = static_cast<LightPopup *>(lv_event_get_user_data(e));
    const int pct = lv_slider_get_value(self->brightness_);
    lv_label_set_text_fmt(self->brightness_label_, "%d %%", pct);
    self->dragging_ = lv_event_get_code(e) != LV_EVENT_RELEASED;
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "brightness_pct", pct);
    self->ctx_.services.ha_call("light", "turn_on", self->entity_, data);
  }

  WidgetContext &ctx_;
  std::string entity_;
  Features features_;
  bool mode_color_ = false;
  bool dragging_ = false;
  float hue_ = 0, sat_ = 0;
  int kelvin_min_ = 2000, kelvin_max_ = 6500;
  int token_ = 0;
  lv_obj_t *panel_ = nullptr, *bubble_ = nullptr, *icon_ = nullptr, *status_ = nullptr, *switch_ = nullptr;
  lv_obj_t *tabs_[2] = {nullptr, nullptr};
  lv_obj_t *body_ = nullptr, *wheel_ = nullptr, *knob_ = nullptr;
  lv_obj_t *temp_box_ = nullptr, *kelvin_ = nullptr, *kelvin_label_ = nullptr;
  lv_obj_t *brightness_ = nullptr, *brightness_label_ = nullptr;
};

}  // namespace

void open_light_popup(WidgetContext &ctx, const std::string &entity_id, const std::string &name, const char *icon) {
  if (!ctx.services.ha_call) return ctx.app.toast("Home Assistant non configuré");
  new LightPopup(ctx, entity_id, name, icon);  // owned by its sheet, deleted on close
}

}  // namespace dm::widgets
