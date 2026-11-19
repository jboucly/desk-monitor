// ha_list: several Home Assistant entities (or templates) in one tile, as rows ("pièce") or as colored
// badges with a problem count ("statut des services").
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "common.h"
#include "dm/ha.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

constexpr int ROW_H = 36;
constexpr int BADGE_H = 30;

/**
 * @description binary_sensor classes where "on" is the bad news (a leak, smoke, a problem…).
 */
bool alarm_class(const std::string &device_class) {
  for (const char *c : {"problem", "safety", "smoke", "gas", "moisture", "tamper", "carbon_monoxide", "heat", "battery"})
    if (device_class == c) return true;
  return false;
}

/**
 * @description Whether an item looks fine without a rule: unavailable is a problem; a binary_sensor is fine
 * when on (connectivity, running…) or off for alarm classes; a switch-like entity when on; anything else is fine.
 */
bool default_ok(const std::string &entity, const cJSON *state) {
  if (is_unavailable(state)) return false;
  const std::string domain = entity_domain(entity);
  const char *s = json::str(state, "s");
  if (domain == "binary_sensor") {
    const bool on = !strcmp(s, "on");
    return alarm_class(json::str(json::get(state, "a"), "device_class")) ? !on : on;
  }
  for (const char *d : {"switch", "light", "fan", "input_boolean", "automation", "media_player"})
    if (domain == d) return is_active(state);
  return true;
}

/**
 * @description Short French text of a state: "21,5 °C", "Allumé", "Connecté", "Indisponible"…
 */
std::string state_label(const std::string &entity, const cJSON *state, int decimals) {
  if (!state) return "…";
  if (is_unavailable(state)) return "Indisponible";
  const char *s = json::str(state, "s");
  const cJSON *attrs = json::get(state, "a");
  char *end = nullptr;
  const double number = strtod(s, &end);
  if (*s && *end == '\0' && std::isfinite(number)) {
    const char *unit = json::str(attrs, "unit_of_measurement");
    return util::format_number(number, decimals) + (*unit ? std::string(" ") + unit : "");
  }
  const std::string device_class = json::str(attrs, "device_class");
  const bool on = !strcmp(s, "on");
  if (entity_domain(entity) == "binary_sensor") {
    if (device_class == "connectivity") return on ? "Connecté" : "Déconnecté";
    if (device_class == "door" || device_class == "window" || device_class == "opening") return on ? "Ouvert" : "Fermé";
    if (device_class == "motion" || device_class == "occupancy" || device_class == "presence")
      return on ? "Détecté" : "Rien";
    if (device_class == "running") return on ? "En marche" : "Arrêté";
    if (alarm_class(device_class)) return on ? "Alerte" : "OK";
  }
  if (on) return "Allumé";
  if (!strcmp(s, "off")) return "Éteint";
  if (!strcmp(s, "open")) return "Ouvert";
  if (!strcmp(s, "closed")) return "Fermé";
  if (!strcmp(s, "locked")) return "Verrouillé";
  if (!strcmp(s, "unlocked")) return "Déverrouillé";
  if (!strcmp(s, "home")) return "Présent";
  if (!strcmp(s, "not_home")) return "Absent";
  return s;
}

/**
 * @description What a tap on an item does by default: toggle what can be toggled, open the popup of what
 * has one, nothing for read-only entities and templates.
 */
const char *default_tap(const std::string &entity) {
  const std::string domain = entity_domain(entity);
  for (const char *d : {"light", "switch", "fan", "input_boolean", "cover", "scene", "script", "automation", "button"})
    if (domain == d) return "toggle";
  for (const char *d : {"climate", "water_heater", "input_number", "number", "input_select", "select", "media_player"})
    if (domain == d) return "more_info";
  return "none";
}

class ListTile : public Widget {
 public:
  ListTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ctx_(ctx), spec_(spec) {
    badges_ = !strcmp(json::str(spec.props, "mode", "rows"), "badges");
    decimals_ = static_cast<int>(json::num(spec.props, "decimals", 1));

    root = theme::make_card(parent);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 8, 0);

    // Header: title, and the problem count of a badge list
    const char *title = json::str(spec.props, "name");
    if (*title || badges_) {
      lv_obj_t *head = make_box(root, LV_FLEX_FLOW_ROW);
      lv_obj_set_width(head, lv_pct(100));
      lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_t *label = theme::make_label(head, &dm_font_14, theme::text2(), *title ? title : "Statut");
      lv_obj_set_flex_grow(label, 1);
      theme::single_line(label);
      if (badges_) summary_ = theme::make_label(head, &dm_font_14, theme::text2());
    }

    list_ = make_box(root, badges_ ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(list_, lv_pct(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_style_pad_row(list_, badges_ ? 8 : 2, 0);
    lv_obj_set_style_pad_column(list_, 8, 0);
    // More items than the tile shows: they scroll vertically (pages still swipe sideways)
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, json::get(spec.props, "items")) items_.push_back(build_item(item));
  }

  /**
   * @description Subscribes every item (called by the factory once the tile is built).
   */
  void start() {
    for (auto &item : items_) {
      Item *it = item.get();
      // Rules first: the bus calls the listener right away when the key already has a value
      it->rules.bind(it->props, ctx_.bus, [this, it] { render(*it, ctx_.bus.get(it->key)); });
      ctx_.bus.subscribe(it->key, [this, it](const cJSON *state) { render(*it, state); });
      render(*it, ctx_.bus.get(it->key));
    }
  }

 private:
  struct Item {
    const cJSON *props = nullptr;  // the item object in the layout
    std::string entity, key;
    RuleBinding rules;
    bool ok = true;
    lv_obj_t *obj = nullptr, *mark = nullptr, *name = nullptr, *state = nullptr;
  };

  std::unique_ptr<Item> build_item(const cJSON *props) {
    auto item = std::make_unique<Item>();
    item->props = props;
    item->entity = json::str(props, "entity");
    const std::string tpl = json::str(props, "template");
    item->key = tpl.empty() ? "ha/" + item->entity : "ha_tpl/" + HaSession::template_key(tpl);
    if (!tpl.empty()) item->entity.clear();

    lv_obj_t *obj = make_box(list_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 8, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, 50, LV_STATE_PRESSED);
    if (badges_) {
      // Pill: colored dot + name
      lv_obj_set_height(obj, BADGE_H);
      lv_obj_set_style_pad_hor(obj, 10, 0);
      lv_obj_set_style_radius(obj, BADGE_H / 2, 0);
      lv_obj_set_style_bg_opa(obj, 22, 0);
      item->mark = lv_obj_create(obj);
      lv_obj_remove_style_all(item->mark);
      lv_obj_set_size(item->mark, 10, 10);
      lv_obj_set_style_radius(item->mark, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(item->mark, LV_OPA_COVER, 0);
      lv_obj_remove_flag(item->mark, LV_OBJ_FLAG_CLICKABLE);
      item->name = theme::make_label(obj, &dm_font_14, theme::text());
    } else {
      // Row: icon, name, state on the right
      lv_obj_set_size(obj, lv_pct(100), ROW_H);
      lv_obj_set_style_pad_hor(obj, 4, 0);
      lv_obj_set_style_radius(obj, 10, 0);
      item->mark = theme::make_icon(obj, &dm_icons_24, "home-assistant", theme::text2());
      item->name = theme::make_label(obj, &dm_font_16, theme::text());
      lv_obj_set_flex_grow(item->name, 1);
      theme::single_line(item->name);
      item->state = theme::make_label(obj, &dm_font_14, theme::text2());
    }
    item->obj = obj;
    lv_obj_add_event_cb(obj, on_gesture, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_add_event_cb(obj, on_gesture, LV_EVENT_LONG_PRESSED, this);
    return item;
  }

  std::string item_name(const Item &item, const cJSON *state) const {
    if (item.entity.empty()) return json::str(item.props, "name", "Template");
    return entity_name(item.props, state, item.entity);
  }

  const char *item_icon(const Item &item, const Rule *rule) const {
    if (rule && !rule->icon.empty()) return rule->icon.c_str();
    if (const char *icon = json::str(item.props, "icon"); *icon) return icon;
    return item.entity.empty() ? "home-assistant" : default_entity_icon(item.entity);
  }

  void render(Item &item, const cJSON *state) {
    const Rule *rule = item.rules.match(state);
    const std::string name = item_name(item, state);
    lv_label_set_text(item.name, name.c_str());
    // In a list, a rule's "active" says whether the item is fine (true) or a problem (false)
    item.ok = rule && rule->active >= 0 ? rule->active == 1 : state ? default_ok(item.entity, state) : true;
    const bool unavailable = is_unavailable(state) && !rule;

    if (badges_) {
      const lv_color_t color = rule && rule->has_color ? lv_color_hex(rule->color)
                               : !state                 ? theme::text3()
                               : item.ok                ? theme::ok()
                                                        : theme::crit();
      lv_obj_set_style_bg_color(item.mark, color, 0);
      // A rule text is shown after the name ("NAS · 3 To libres")
      if (rule && !rule->text.empty())
        lv_label_set_text(item.name, (name + " · " + Rules::format(rule->text, state, name)).c_str());
      update_summary();
      return;
    }

    std::string text = rule && !rule->text.empty() ? Rules::format(rule->text, state, name)
                                                   : state_label(item.entity, state, decimals_);
    lv_label_set_text(item.state, text.c_str());
    lv_label_set_text(item.mark, theme::icon(item_icon(item, rule)));
    const bool on = !unavailable && is_active(state);
    const lv_color_t color = rule && rule->has_color ? lv_color_hex(rule->color)
                             : unavailable            ? theme::text3()
                             : on                     ? theme::accent()
                                                      : theme::text2();
    lv_obj_set_style_text_color(item.mark, color, 0);
    lv_obj_set_style_text_color(item.state, rule && rule->has_color ? color : unavailable ? theme::text3() : theme::text2(), 0);
    lv_obj_set_style_text_color(item.name, unavailable ? theme::text3() : theme::text(), 0);
  }

  void update_summary() {
    if (!summary_) return;
    int problems = 0;
    for (const auto &item : items_) problems += !item->ok;
    if (problems == 0) {
      lv_label_set_text(summary_, "Tout va bien");
      lv_obj_set_style_text_color(summary_, theme::ok(), 0);
    } else {
      lv_label_set_text_fmt(summary_, "%d en panne", problems);
      lv_obj_set_style_text_color(summary_, theme::crit(), 0);
    }
  }

  static void on_gesture(lv_event_t *e) {
    auto *self = static_cast<ListTile *>(lv_event_get_user_data(e));
    const lv_obj_t *target = lv_event_get_current_target_obj(e);
    const bool hold = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    for (const auto &item : self->items_) {
      if (item->obj != target) continue;
      const cJSON *state = self->ctx_.bus.get(item->key);
      const char *fallback = item->entity.empty() ? "none" : hold ? "more_info" : default_tap(item->entity);
      run_tile_action(self->ctx_, item->props, hold ? "hold_action" : "tap_action", fallback, item->entity,
                      self->item_name(*item, state), self->item_icon(*item, nullptr));
      return;
    }
  }

  WidgetContext &ctx_;
  WidgetSpec spec_;
  bool badges_ = false;
  int decimals_ = 1;
  std::vector<std::unique_ptr<Item>> items_;
  lv_obj_t *list_ = nullptr, *summary_ = nullptr;
};

}  // namespace

std::unique_ptr<Widget> create_ha_list(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  auto tile = std::make_unique<ListTile>(parent, spec, ctx);
  tile->start();
  return tile;
}

}  // namespace dm::widgets
