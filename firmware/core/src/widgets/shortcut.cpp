// shortcut: Stream Deck-like key. "ha" actions go to Home Assistant, the others to the PC companion.
#include <cstdlib>
#include <cstring>

#include "common.h"

namespace dm::widgets {

namespace {

/**
 * @description Default icon of an action kind.
 */
const char *default_icon(const char *kind) {
  if (!strcmp(kind, "ha")) return "home-assistant";
  if (!strcmp(kind, "keys")) return "keyboard";
  if (!strcmp(kind, "url")) return "web";
  return "application";
}

class Shortcut : public Widget {
 public:
  Shortcut(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ctx_(ctx), spec_(spec) {
    const cJSON *action = json::get(spec.props, "action");
    const char *kind = json::str(action, "kind");
    const lv_color_t color = cJSON_IsString(json::get(spec.props, "color"))
                                 ? lv_color_hex(strtoul(json::str(spec.props, "color") + 1, nullptr, 16))
                                 : theme::accent();
    // A one-row wide key lays out horizontally, bigger keys get a bigger badge
    const bool row = spec.w >= 2 && spec.h == 1;
    const bool big = !row && (spec.w >= 2 || spec.h >= 2);

    root = theme::make_card(parent);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(root, row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, row ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_set_style_pad_column(root, 14, 0);

    // Tinted rounded square with the icon, like an app icon
    lv_obj_t *badge = lv_obj_create(root);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, big ? 64 : 46, big ? 64 : 46);
    lv_obj_set_style_radius(badge, big ? 20 : 15, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badge, color, 0);
    lv_obj_set_style_bg_grad_color(badge, lv_color_mix(color, lv_color_black(), 160), 0);
    lv_obj_set_style_bg_grad_dir(badge, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *icon = theme::make_icon(badge, big ? &dm_icons_32 : &dm_icons_24,
                                      json::str(spec.props, "icon", default_icon(kind)), lv_color_white());
    lv_obj_center(icon);

    lv_obj_t *name = theme::make_label(root, row ? &dm_font_20 : &dm_font_14, theme::text(), json::str(spec.props, "name"));
    if (row) {
      lv_obj_set_flex_grow(name, 1);
    } else {
      lv_obj_set_width(name, lv_pct(100));
      lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    }
    theme::single_line(name);

    bind_event<Shortcut, &Shortcut::on_click>(root, LV_EVENT_CLICKED, this);
  }

 private:
  void on_click(lv_event_t *) {
    const cJSON *action = json::get(spec_.props, "action");
    if (!strcmp(json::str(action, "kind"), "ha")) {
      if (!ctx_.services.ha_call) return ctx_.app.toast("Home Assistant non configuré");
      const cJSON *data = json::get(action, "data");
      ctx_.services.ha_call(json::str(action, "domain"), json::str(action, "service"), json::str(action, "entity_id"),
                            data ? cJSON_Duplicate(data, true) : nullptr);
      return;
    }
    if (!ctx_.services.pc_action || !ctx_.services.pc_action(action)) ctx_.app.toast("PC non connecté");
  }

  WidgetContext &ctx_;
  WidgetSpec spec_;
};

}  // namespace

std::unique_ptr<Widget> create_shortcut(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<Shortcut>(parent, spec, ctx);
}

}  // namespace dm::widgets
