// Small LVGL building blocks shared by the tiles and popups (see common.h, "UI helpers").
#include "common.h"

namespace dm::widgets {

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

void place_tile_name(lv_obj_t *bubble, lv_obj_t *name, const WidgetSpec &spec) {
  if (spec.h >= 2 || spec.w < 2) {
    lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return;
  }
  constexpr int GAP = 10;
  const int size = lv_obj_get_style_width(bubble, LV_PART_MAIN);
  const int line = lv_font_get_line_height(lv_obj_get_style_text_font(name, LV_PART_MAIN));
  lv_obj_align(bubble, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_width(name, theme::span_width(spec.w) - 2 * theme::CARD_PAD - size - GAP);
  lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, size + GAP, -(size - line) / 2);
}

void scroll_vertically(lv_obj_t *list) {
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_right(list, 6, 0);
  lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(list, lv_color_white(), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, 90, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(list, 0, LV_PART_SCROLLBAR);
}

lv_obj_t *make_box(lv_obj_t *parent, lv_flex_flow_t flow) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, flow);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  return box;
}

lv_obj_t *make_round_button(lv_obj_t *parent, const char *icon, int size) {
  lv_obj_t *button = lv_obj_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_size(button, size, size);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(button, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(button, 36, 0);
  lv_obj_set_style_bg_opa(button, 90, LV_STATE_PRESSED);
  lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_ext_click_area(button, 6);
  lv_obj_t *glyph = theme::make_icon(button, size >= 44 ? &dm_icons_32 : &dm_icons_24, icon, theme::text());
  lv_obj_center(glyph);
  return button;
}

lv_obj_t *make_chip(lv_obj_t *parent, const std::string &text, const char *icon, int height) {
  lv_obj_t *chip = make_box(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_height(chip, height);
  lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(chip, 14, 0);
  lv_obj_set_style_pad_column(chip, 6, 0);
  lv_obj_set_style_radius(chip, height / 2, 0);
  lv_obj_set_style_bg_color(chip, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(chip, 30, 0);
  lv_obj_set_style_bg_color(chip, theme::accent(), LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_STATE_CHECKED);
  lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  // Let a horizontal drag on a chip scroll its row
  lv_obj_add_flag(chip, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  if (icon) theme::make_icon(chip, &dm_icons_18, icon, theme::text());
  theme::make_label(chip, &dm_font_14, theme::text(), text.c_str());
  return chip;
}

void set_checked(lv_obj_t *obj, bool checked) {
  if (checked) lv_obj_add_state(obj, LV_STATE_CHECKED);
  else lv_obj_remove_state(obj, LV_STATE_CHECKED);
}

void set_hidden(lv_obj_t *obj, bool hidden) {
  if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace dm::widgets
