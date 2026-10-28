// Home Assistant entity tiles: ha_toggle, ha_light and ha_sensor.
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

/**
 * @description Tile with a round icon, a state line and a name; tapping toggles the entity.
 * The card switches to a colored "active" look while the entity is on (LV_STATE_CHECKED).
 */
class EntityTile : public Widget {
 public:
  EntityTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ctx_(ctx), spec_(spec) {
    entity_ = json::str(spec.props, "entity");
    custom_icon_ = json::str(spec.props, "icon");
    const bool tall = spec.h >= 2;

    root = theme::make_card(parent);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);

    bubble_ = lv_obj_create(root);
    lv_obj_remove_style_all(bubble_);
    lv_obj_add_style(bubble_, theme::bubble(), 0);
    lv_obj_set_size(bubble_, tall ? 56 : 40, tall ? 56 : 40);
    lv_obj_align(bubble_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(bubble_, LV_OBJ_FLAG_CLICKABLE);
    icon_ = theme::make_icon(bubble_, tall ? &dm_icons_32 : &dm_icons_24, icon_name(), theme::text());
    lv_obj_center(icon_);

    state_ = theme::make_label(root, &dm_font_14, theme::text2());
    lv_obj_align(state_, LV_ALIGN_TOP_RIGHT, 0, 2);

    name_ = theme::make_label(root, spec.w >= 2 || tall ? &dm_font_20 : &dm_font_16, theme::text());
    lv_obj_set_width(name_, lv_pct(100));
    if (spec.h >= 3) lv_label_set_long_mode(name_, LV_LABEL_LONG_WRAP);
    else theme::single_line(name_);
    lv_obj_align(name_, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    bind_event<EntityTile, &EntityTile::on_click>(root, LV_EVENT_SHORT_CLICKED, this);
  }

  /**
   * @description Subscribes to the entity. Called by the factory once the object is fully built,
   * so that render() reaches the overrides of derived tiles.
   */
  void start() {
    render(nullptr);
    ctx_.bus.subscribe("ha/" + entity_, [this](const cJSON *state) { render(state); });
  }

 protected:
  /**
   * @description Text of the top-right state line.
   */
  virtual std::string state_text(const cJSON *state) {
    if (is_unavailable(state)) return state ? "Indisponible" : "…";
    return is_active(state) ? "Allumé" : "Éteint";
  }

  /**
   * @description Color of the active card.
   */
  virtual lv_color_t active_color(const cJSON *state) { return theme::accent(); }

  /**
   * @description Refreshes every label and color from an entity state (null = not received yet).
   */
  void render(const cJSON *state) {
    state_json_ = state;
    lv_label_set_text(name_, entity_name(spec_.props, state, entity_).c_str());
    // A one-column tile has no room for "Allumé" next to the icon: only short values (e.g. "58 %") stay
    std::string text = state_text(state);
    if (spec_.w == 1 && text.size() > 5) text.clear();
    lv_label_set_text(state_, text.c_str());
    if (custom_icon_.empty()) lv_label_set_text(icon_, theme::icon(icon_name()));

    const bool unavailable = is_unavailable(state);
    const bool on = !unavailable && is_active(state);
    lv_obj_set_style_text_color(name_, unavailable ? theme::text3() : theme::text(), 0);
    if (on) {
      const lv_color_t c = active_color(state);
      theme::paint_active(root, c);
      lv_obj_set_style_text_color(state_, lv_color_white(), 0);
      lv_obj_set_style_bg_opa(bubble_, 235, 0);
      lv_obj_set_style_text_color(icon_, lv_color_mix(c, lv_color_black(), 170), 0);
      lv_obj_add_state(root, LV_STATE_CHECKED);
    } else {
      lv_obj_remove_state(root, LV_STATE_CHECKED);
      lv_obj_set_style_text_color(state_, unavailable ? theme::text3() : theme::text2(), 0);
      lv_obj_set_style_bg_opa(bubble_, 40, 0);
      lv_obj_set_style_text_color(icon_, unavailable ? theme::text3() : theme::text(), 0);
    }
  }

  virtual void on_click(lv_event_t *) {
    if (!is_unavailable(state_json_)) toggle_entity(ctx_.services, entity_, state_json_);
  }

  const char *icon_name() const {
    if (!custom_icon_.empty()) return custom_icon_.c_str();
    // Lights get an outline bulb while off
    if (entity_domain(entity_) == "light" && !is_active(state_json_)) return "lightbulb-outline";
    return default_entity_icon(entity_);
  }

  WidgetContext &ctx_;
  WidgetSpec spec_;
  std::string entity_;
  std::string custom_icon_;
  const cJSON *state_json_ = nullptr;  // owned by the bus, valid until the next update
  lv_obj_t *bubble_ = nullptr, *icon_ = nullptr, *state_ = nullptr, *name_ = nullptr;
};

/**
 * @description Light tile: shows the brightness, takes the light's color when on,
 * and opens the light popup on a long press.
 */
class LightTile : public EntityTile {
 public:
  LightTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : EntityTile(parent, spec, ctx) {
    lv_obj_add_event_cb(
        root,
        [](lv_event_t *e) {
          auto *self = static_cast<LightTile *>(lv_event_get_user_data(e));
          if (is_unavailable(self->state_json_)) return;
          open_light_popup(self->ctx_, self->entity_, entity_name(self->spec_.props, self->state_json_, self->entity_),
                           self->custom_icon_.empty() ? "lightbulb" : self->custom_icon_.c_str());
        },
        LV_EVENT_LONG_PRESSED, this);
  }

 protected:
  std::string state_text(const cJSON *state) override {
    if (is_unavailable(state) || !is_active(state)) return EntityTile::state_text(state);
    const double brightness = json::num(json::get(state, "a"), "brightness", -1);
    if (brightness < 0) return "Allumé";
    return std::to_string(static_cast<int>(std::lround(brightness * 100.0 / 255.0))) + " %";
  }

  lv_color_t active_color(const cJSON *state) override { return light_color(state); }
};

/**
 * @description Read-only sensor tile: name, big value and unit.
 */
class SensorTile : public Widget {
 public:
  SensorTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : spec_(spec) {
    entity_ = json::str(spec.props, "entity");
    decimals_ = static_cast<int>(json::num(spec.props, "decimals", 1));
    const bool big = spec.h >= 2 && spec.w >= 2;

    root = theme::make_card(parent);
    lv_obj_t *head = lv_obj_create(root);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 6, 0);
    const char *icon = json::str(spec.props, "icon", default_entity_icon(entity_));
    theme::make_icon(head, &dm_icons_18, icon, theme::accent());
    name_ = theme::make_label(head, &dm_font_14, theme::text2());
    lv_obj_set_flex_grow(name_, 1);
    theme::single_line(name_);

    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    value_ = theme::make_label(row, big ? &dm_font_40 : &dm_font_28, theme::text(), "…");
    unit_ = theme::make_label(row, &dm_font_16, theme::text2());
    lv_obj_set_style_pad_bottom(unit_, big ? 6 : 3, 0);

    ctx.bus.subscribe("ha/" + entity_, [this](const cJSON *state) { render(state); });
    lv_label_set_text(name_, entity_name(spec.props, nullptr, entity_).c_str());
  }

 private:
  void render(const cJSON *state) {
    lv_label_set_text(name_, entity_name(spec_.props, state, entity_).c_str());
    const char *raw = json::str(state, "s");
    const char *unit = json::str(spec_.props, "unit", json::str(json::get(state, "a"), "unit_of_measurement"));
    if (is_unavailable(state)) {
      lv_label_set_text(value_, "—");
      lv_label_set_text(unit_, "");
      return;
    }
    char *end = nullptr;
    const double number = strtod(raw, &end);
    if (end != raw && *end == '\0') lv_label_set_text(value_, util::format_number(number, decimals_).c_str());
    else lv_label_set_text(value_, raw);
    lv_label_set_text(unit_, unit);
  }

  WidgetSpec spec_;
  std::string entity_;
  int decimals_ = 1;
  lv_obj_t *name_ = nullptr, *value_ = nullptr, *unit_ = nullptr;
};

}  // namespace

std::unique_ptr<Widget> create_ha_toggle(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  auto tile = std::make_unique<EntityTile>(parent, spec, ctx);
  tile->start();
  return tile;
}

std::unique_ptr<Widget> create_ha_light(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  auto tile = std::make_unique<LightTile>(parent, spec, ctx);
  tile->start();
  return tile;
}

std::unique_ptr<Widget> create_ha_sensor(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<SensorTile>(parent, spec, ctx);
}

}  // namespace dm::widgets
