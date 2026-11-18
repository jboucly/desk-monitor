// Analog and choice controls of Home Assistant entities:
// - levels (input_number/number value, light brightness, fan speed, media volume, cover position/tilt),
// - the generic control popup (cover buttons, sliders, fan presets, select options) and open_more_info(),
// - the ha_cover, ha_slider and ha_select tiles.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

// HA CoverEntityFeature bits
constexpr int COVER_OPEN = 1, COVER_CLOSE = 2, COVER_SET_POSITION = 4, COVER_STOP = 8, COVER_SET_TILT = 128;

constexpr uint32_t HOLD_MS = 3000;  // a released slider keeps its value over older states for this long at most
constexpr int SHEET_W = 440;
constexpr int SHEET_MAX_H = 440;

/**
 * @description Supported features of a cover. Without the attribute (some integrations), open/close/stop
 * are assumed, and a position when the cover reports one.
 */
int cover_features(const cJSON *state) {
  const cJSON *attrs = json::get(state, "a");
  const cJSON *features = json::get(attrs, "supported_features");
  if (cJSON_IsNumber(features)) return features->valueint;
  return COVER_OPEN | COVER_CLOSE | COVER_STOP | (cJSON_IsNumber(json::get(attrs, "current_position")) ? COVER_SET_POSITION : 0);
}

/**
 * @description Status text of a cover: "Fermé", "Ouvert à 40 %", "Ouverture…".
 * @param brief Short form for a narrow tile ("40 %").
 */
std::string cover_text(const cJSON *state, bool brief) {
  const char *s = json::str(state, "s");
  const double position = json::num(json::get(state, "a"), "current_position", -1);
  if (!strcmp(s, "opening")) return brief ? "Ouvre…" : "Ouverture…";
  if (!strcmp(s, "closing")) return brief ? "Ferme…" : "Fermeture…";
  if (!strcmp(s, "closed")) return "Fermé";
  if (!strcmp(s, "open")) {
    if (position <= 0 || position >= 100) return "Ouvert";
    const std::string pct = std::to_string(static_cast<int>(position)) + " %";
    return brief ? pct : "Ouvert à " + pct;
  }
  return s;
}

/**
 * @description Icon of a cover from its device class and whether it is open.
 */
const char *cover_icon(const cJSON *state) {
  const std::string device_class = json::str(json::get(state, "a"), "device_class");
  const bool open = strcmp(json::str(state, "s"), "closed") != 0;
  if (device_class == "shutter") return open ? "window-shutter-open" : "window-shutter";
  if (device_class == "garage" || device_class == "gate") return open ? "garage-open" : "garage";
  return open ? "blinds-open" : "blinds";
}

/**
 * @description Sends a cover command: "open_cover", "close_cover" or "stop_cover".
 */
void cover_command(Services &services, const std::string &entity, const char *service) {
  if (services.ha_call) services.ha_call("cover", service, entity, nullptr);
}

// ── Levels ───────────────────────────────────────────────────────────────────

/**
 * @description A value an entity exposes as a slider. `ok` is false when the entity has none.
 */
struct Level {
  bool ok = false;
  double value = NAN, min = 0, max = 100, step = 1;
  std::string unit = "%";
  int decimals = 0;
  const char *icon = "tune-variant";
};

/**
 * @description Reads the level of an entity. `tilt` reads a cover's tilt instead of its position.
 */
Level level_of(const std::string &entity, const cJSON *state, bool tilt = false) {
  Level l;
  if (!state || is_unavailable(state)) return l;
  const std::string domain = entity_domain(entity);
  const cJSON *a = json::get(state, "a");
  const bool on = is_active(state);
  if (domain == "input_number" || domain == "number") {
    l.ok = true;
    l.value = strtod(json::str(state, "s"), nullptr);
    l.min = json::num(a, "min", 0);
    l.max = json::num(a, "max", 100);
    l.step = json::num(a, "step", 1);
    l.unit = json::str(a, "unit_of_measurement");
    l.decimals = l.step >= 1 ? 0 : l.step >= 0.1 ? 1 : 2;
  } else if (domain == "light") {
    l.ok = true;
    l.value = on ? std::lround(json::num(a, "brightness", 255) * 100.0 / 255.0) : 0;
    l.icon = "brightness-6";
  } else if (domain == "fan") {
    l.ok = cJSON_IsNumber(json::get(a, "percentage")) || cJSON_IsNumber(json::get(a, "percentage_step"));
    l.value = on ? json::num(a, "percentage", 100) : 0;
    l.step = json::num(a, "percentage_step", 1);
    l.icon = "fan";
  } else if (domain == "media_player") {
    l.ok = cJSON_IsNumber(json::get(a, "volume_level"));
    l.value = std::lround(json::num(a, "volume_level") * 100);
    l.icon = "volume-high";
  } else if (domain == "cover") {
    const int features = cover_features(state);
    l.ok = tilt ? (features & COVER_SET_TILT) != 0 : (features & COVER_SET_POSITION) != 0;
    l.value = json::num(a, tilt ? "current_tilt_position" : "current_position", 0);
    l.icon = tilt ? "angle-acute" : cover_icon(state);
  }
  if (l.step <= 0) l.step = 1;
  if (l.max <= l.min) l.max = l.min + l.step;
  return l;
}

/**
 * @description Sends a new level with the service of the entity's domain.
 */
void send_level(Services &services, const std::string &entity, double value, bool tilt = false) {
  if (!services.ha_call) return;
  const std::string domain = entity_domain(entity);
  cJSON *data = cJSON_CreateObject();
  if (domain == "input_number" || domain == "number") {
    cJSON_AddNumberToObject(data, "value", value);
    return services.ha_call(domain, "set_value", entity, data);
  }
  if (domain == "light") {
    if (value <= 0) {
      cJSON_Delete(data);
      return services.ha_call(domain, "turn_off", entity, nullptr);
    }
    cJSON_AddNumberToObject(data, "brightness_pct", value);
    return services.ha_call(domain, "turn_on", entity, data);
  }
  if (domain == "fan") {
    cJSON_AddNumberToObject(data, "percentage", value);
    return services.ha_call(domain, "set_percentage", entity, data);
  }
  if (domain == "media_player") {
    cJSON_AddNumberToObject(data, "volume_level", value / 100.0);
    return services.ha_call(domain, "volume_set", entity, data);
  }
  if (domain == "cover") {
    cJSON_AddNumberToObject(data, tilt ? "tilt_position" : "position", value);
    return services.ha_call(domain, tilt ? "set_cover_tilt_position" : "set_cover_position", entity, data);
  }
  cJSON_Delete(data);
}

std::string level_text(const Level &l, double value) {
  if (std::isnan(value)) return "—";
  std::string text = util::format_number(value, l.decimals);
  if (!l.unit.empty()) text += " " + l.unit;
  return text;
}

/**
 * @description A slider bound to a level of an entity: it follows the state, shows the value in a label
 * while dragged, and sends it on release. After a release it keeps its position until HA confirms it.
 */
class LevelSlider {
 public:
  LevelSlider(lv_obj_t *parent, WidgetContext &ctx, std::string entity, lv_obj_t *label, bool tilt)
      : ctx_(ctx), entity_(std::move(entity)), label_(label), tilt_(tilt) {
    slider = lv_slider_create(parent);
    style_slider(slider);
    lv_obj_set_style_bg_color(slider, theme::accent(), LV_PART_INDICATOR);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_add_event_cb(slider, on_event, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(slider, on_event, LV_EVENT_RELEASED, this);
  }
  LevelSlider(const LevelSlider &) = delete;
  LevelSlider &operator=(const LevelSlider &) = delete;

  /**
   * @description Syncs range, position and label with the state (the position stays while held).
   */
  void render(const cJSON *state) {
    level_ = level_of(entity_, state, tilt_);
    if (!level_.ok) return;
    steps_ = std::max(1, static_cast<int>(std::lround((level_.max - level_.min) / level_.step)));
    lv_slider_set_range(slider, 0, steps_);
    if (held_ && !dragging_ && (std::fabs(level_.value - sent_) < level_.step / 2 || lv_tick_elaps(released_at_) > HOLD_MS))
      held_ = false;
    if (!held_ && !dragging_) {
      lv_slider_set_value(slider, static_cast<int>(std::lround((level_.value - level_.min) / level_.step)), LV_ANIM_OFF);
      if (label_) lv_label_set_text(label_, level_text(level_, level_.value).c_str());
    }
    // A light's slider takes the light's color
    if (entity_domain(entity_) == "light")
      lv_obj_set_style_bg_color(slider, is_active(state) ? light_color(state) : theme::text3(), LV_PART_INDICATOR);
  }

  lv_obj_t *slider = nullptr;

 private:
  static void on_event(lv_event_t *e) {
    auto *self = static_cast<LevelSlider *>(lv_event_get_user_data(e));
    if (!self->level_.ok) return;
    const double value = self->level_.min + lv_slider_get_value(self->slider) * self->level_.step;
    if (self->label_) lv_label_set_text(self->label_, level_text(self->level_, value).c_str());
    self->dragging_ = lv_event_get_code(e) != LV_EVENT_RELEASED;
    if (self->dragging_) return;
    self->held_ = true;
    self->sent_ = value;
    self->released_at_ = lv_tick_get();
    send_level(self->ctx_.services, self->entity_, value, self->tilt_);
  }

  WidgetContext &ctx_;
  std::string entity_;
  lv_obj_t *label_ = nullptr;
  bool tilt_ = false;
  Level level_;
  int steps_ = 100;
  bool dragging_ = false, held_ = false;
  double sent_ = NAN;
  uint32_t released_at_ = 0;
};

// ── Options ──────────────────────────────────────────────────────────────────

/**
 * @description Choices an entity offers: select/input_select options, or a fan's presets.
 */
struct Options {
  std::vector<std::string> values;
  std::string current;
};

Options options_of(const std::string &entity, const cJSON *state) {
  Options o;
  const std::string domain = entity_domain(entity);
  const cJSON *a = json::get(state, "a");
  const bool fan = domain == "fan";
  const cJSON *v = nullptr;
  cJSON_ArrayForEach(v, json::get(a, fan ? "preset_modes" : "options")) if (cJSON_IsString(v)) o.values.emplace_back(v->valuestring);
  o.current = fan ? json::str(a, "preset_mode") : json::str(state, "s");
  return o;
}

void select_option(Services &services, const std::string &entity, const std::string &option) {
  if (!services.ha_call) return;
  const std::string domain = entity_domain(entity);
  cJSON *data = cJSON_CreateObject();
  if (domain == "fan") {
    cJSON_AddStringToObject(data, "preset_mode", option.c_str());
    return services.ha_call(domain, "set_preset_mode", entity, data);
  }
  cJSON_AddStringToObject(data, "option", option.c_str());
  services.ha_call(domain, "select_option", entity, data);
}

/**
 * @description French label of a fan preset; select options are the user's own texts and stay as they are.
 */
std::string option_text(const std::string &entity, const std::string &value) {
  if (entity_domain(entity) != "fan") return value;
  static const char *const LABELS[][2] = {{"auto", "Auto"},   {"smart", "Auto"},     {"sleep", "Nuit"},
                                          {"nature", "Nature"}, {"breeze", "Brise"}, {"eco", "Éco"},
                                          {"boost", "Boost"}, {"normal", "Normal"}};
  for (const auto &entry : LABELS)
    if (value == entry[0]) return entry[1];
  return value;
}

// ── Generic control popup ────────────────────────────────────────────────────

/**
 * @description Status line of an entity in the popup and the tiles.
 */
std::string status_text(const std::string &entity, const cJSON *state) {
  if (is_unavailable(state)) return state ? "Indisponible" : "…";
  const std::string domain = entity_domain(entity);
  if (domain == "cover") return cover_text(state, false);
  if (domain == "input_select" || domain == "select") return json::str(state, "s");
  if (domain == "media_player") {
    const char *s = json::str(state, "s");
    if (!strcmp(s, "playing")) return "Lecture";
    if (!strcmp(s, "paused")) return "Pause";
    if (!strcmp(s, "idle") || !strcmp(s, "standby")) return "Inactif";
    if (!strcmp(s, "off")) return "Éteint";
    return s;
  }
  const Level level = level_of(entity, state);
  if (domain == "input_number" || domain == "number") return level_text(level, level.value);
  if (!is_active(state)) return "Éteint";
  if (level.ok) return level_text(level, level.value);
  return "Allumé";
}

/**
 * @description Popup with what fits the entity: on/off switch (fan), level slider, tilt slider and
 * open/stop/close buttons (cover), preset chips (fan) or an option list (select). Deleted on close.
 */
class ControlSheet {
 public:
  ControlSheet(WidgetContext &ctx, std::string entity_id, const std::string &name, const char *icon)
      : ctx_(ctx), entity_(std::move(entity_id)), domain_(entity_domain(entity_)), icon_name_(icon) {
    const cJSON *state = ctx_.bus.get("ha/" + entity_);

    // The panel grows with its content (open_sheet() centers it)
    panel_ = ctx_.app.open_sheet(SHEET_W, 200, [this] { delete this; });
    lv_obj_set_height(panel_, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel_, SHEET_MAX_H, 0);
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel_, 16, 0);

    build_header(name);
    if (domain_ == "cover") build_cover_buttons(state);
    if (level_of(entity_, state).ok) build_slider(false);
    if (domain_ == "cover" && level_of(entity_, state, true).ok) build_slider(true);
    if (domain_ == "fan") build_chips(state);
    if (domain_ == "input_select" || domain_ == "select") build_list(state);

    token_ = ctx_.bus.subscribe("ha/" + entity_, [this](const cJSON *s) { render(s); });
  }

  ~ControlSheet() { ctx_.bus.unsubscribe(token_); }

 private:
  struct Choice {
    lv_obj_t *obj;
    std::string value;
  };

  lv_obj_t *full_row(lv_obj_t *parent) {
    lv_obj_t *r = make_box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    return r;
  }

  void build_header(const std::string &name) {
    lv_obj_t *head = full_row(panel_);
    bubble_ = lv_obj_create(head);
    lv_obj_remove_style_all(bubble_);
    lv_obj_add_style(bubble_, theme::bubble(), 0);
    lv_obj_set_size(bubble_, 48, 48);
    icon_ = theme::make_icon(bubble_, &dm_icons_24, icon_name_.c_str(), theme::text());
    lv_obj_center(icon_);

    lv_obj_t *col = make_box(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_t *title = theme::make_label(col, &dm_font_20, theme::text(), name.c_str());
    lv_obj_set_width(title, lv_pct(100));
    theme::single_line(title);
    status_ = theme::make_label(col, &dm_font_14, theme::text2());

    if (domain_ != "fan") return;
    switch_ = lv_switch_create(head);
    lv_obj_set_size(switch_, 60, 32);
    lv_obj_set_style_bg_color(switch_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(switch_, 50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(switch_, theme::accent(), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
    lv_obj_add_event_cb(
        switch_,
        [](lv_event_t *e) {
          auto *self = static_cast<ControlSheet *>(lv_event_get_user_data(e));
          const bool on = lv_obj_has_state(self->switch_, LV_STATE_CHECKED);
          self->ctx_.services.ha_call(self->domain_, on ? "turn_on" : "turn_off", self->entity_, nullptr);
        },
        LV_EVENT_VALUE_CHANGED, this);
  }

  void build_slider(bool tilt) {
    lv_obj_t *r = full_row(panel_);
    const Level level = level_of(entity_, ctx_.bus.get("ha/" + entity_), tilt);
    theme::make_icon(r, &dm_icons_24, level.icon, theme::text2());
    lv_obj_t *label = theme::make_label(r, &dm_font_16, theme::text(), "—");
    auto slider = std::make_unique<LevelSlider>(r, ctx_, entity_, label, tilt);
    lv_obj_set_flex_grow(slider->slider, 1);
    // The value label goes after the slider
    lv_obj_move_to_index(label, -1);
    lv_obj_set_style_min_width(label, 64, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    sliders_.push_back(std::move(slider));
  }

  void build_cover_buttons(const cJSON *state) {
    const int features = cover_features(state);
    lv_obj_t *r = full_row(panel_);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const struct {
      int feature;
      const char *icon, *service;
    } BUTTONS[] = {{COVER_OPEN, "chevron-up", "open_cover"}, {COVER_STOP, "stop", "stop_cover"}, {COVER_CLOSE, "chevron-down", "close_cover"}};
    for (const auto &b : BUTTONS) {
      if (!(features & b.feature)) continue;
      lv_obj_t *button = make_round_button(r, b.icon, 60);
      lv_obj_set_user_data(button, const_cast<char *>(b.service));
      lv_obj_add_event_cb(
          button,
          [](lv_event_t *e) {
            auto *self = static_cast<ControlSheet *>(lv_event_get_user_data(e));
            auto *service = static_cast<const char *>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
            cover_command(self->ctx_.services, self->entity_, service);
          },
          LV_EVENT_CLICKED, this);
    }
  }

  void build_chips(const cJSON *state) {
    const Options options = options_of(entity_, state);
    if (options.values.empty()) return;
    lv_obj_t *r = full_row(panel_);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_height(r, 36);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(r, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    for (const std::string &value : options.values) {
      lv_obj_t *chip = make_chip(r, option_text(entity_, value), nullptr, 36);
      lv_obj_add_event_cb(chip, on_choice, LV_EVENT_CLICKED, this);
      choices_.push_back({chip, value});
    }
  }

  void build_list(const cJSON *state) {
    const Options options = options_of(entity_, state);
    // Scrolls when the options do not fit the sheet
    lv_obj_t *list = make_box(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_style_max_height(list, SHEET_MAX_H - 36 - 48 - 16, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (const std::string &value : options.values) {
      lv_obj_t *row = make_box(list, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(row, lv_pct(100), 48);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_hor(row, 16, 0);
      lv_obj_set_style_radius(row, 16, 0);
      lv_obj_set_style_bg_color(row, lv_color_white(), 0);
      lv_obj_set_style_bg_opa(row, 24, 0);
      lv_obj_set_style_bg_color(row, theme::accent(), LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
      lv_obj_t *label = theme::make_label(row, &dm_font_16, theme::text(), value.c_str());
      lv_obj_set_flex_grow(label, 1);
      theme::single_line(label);
      lv_obj_add_event_cb(row, on_choice, LV_EVENT_CLICKED, this);
      choices_.push_back({row, value});
    }
    list_ = true;
  }

  void render(const cJSON *state) {
    const bool on = !is_unavailable(state) && is_active(state);
    const lv_color_t color = domain_ == "light" ? light_color(state) : theme::accent();
    if (domain_ == "cover") lv_label_set_text(icon_, theme::icon(cover_icon(state)));
    lv_obj_set_style_bg_opa(bubble_, on ? 235 : 40, 0);
    lv_obj_set_style_bg_color(bubble_, on ? color : lv_color_white(), 0);
    lv_obj_set_style_text_color(icon_, on ? lv_color_mix(color, lv_color_black(), 140) : theme::text(), 0);
    lv_label_set_text(status_, status_text(entity_, state).c_str());
    if (switch_) set_checked(switch_, on);
    for (auto &slider : sliders_) slider->render(state);
    const Options options = options_of(entity_, state);
    for (const Choice &choice : choices_) set_checked(choice.obj, choice.value == options.current);
  }

  static void on_choice(lv_event_t *e) {
    auto *self = static_cast<ControlSheet *>(lv_event_get_user_data(e));
    const lv_obj_t *target = lv_event_get_current_target_obj(e);
    for (const Choice &choice : self->choices_) {
      if (choice.obj != target) continue;
      select_option(self->ctx_.services, self->entity_, choice.value);
      // A list is a one-shot choice: close it; chips stay for more changes
      if (self->list_) self->ctx_.app.close_sheet();
      return;
    }
  }

  WidgetContext &ctx_;
  std::string entity_, domain_;
  std::string icon_name_;
  int token_ = 0;
  bool list_ = false;
  std::vector<std::unique_ptr<LevelSlider>> sliders_;
  std::vector<Choice> choices_;
  lv_obj_t *panel_ = nullptr, *bubble_ = nullptr, *icon_ = nullptr, *status_ = nullptr, *switch_ = nullptr;
};

// ── Tiles ────────────────────────────────────────────────────────────────────

/**
 * @description Base of the tiles below: card, round icon bubble, rules, the entity subscription, and the
 * control popup on a tap or a long press.
 */
class ControlTile : public Widget {
 public:
  ControlTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx)
      : ctx_(ctx), spec_(spec), entity_(json::str(spec.props, "entity")), custom_icon_(json::str(spec.props, "icon")) {
    root = theme::make_card(parent);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    bind_event<ControlTile, &ControlTile::on_open>(root, LV_EVENT_SHORT_CLICKED, this);
    bind_event<ControlTile, &ControlTile::on_open>(root, LV_EVENT_LONG_PRESSED, this);
  }

  /**
   * @description Subscribes to the entity (called by the factory once the tile is built).
   */
  void start() {
    refresh(nullptr);
    // Rules first: the bus calls the entity listener right away when it already has a state
    rules_.bind(spec_.props, ctx_.bus, [this] { refresh(ctx_.bus.get("ha/" + entity_)); });
    ctx_.bus.subscribe("ha/" + entity_, [this](const cJSON *state) { refresh(state); });
  }

 protected:
  /**
   * @description Draws the tile. `rule` (may be null) is the matching display rule.
   */
  virtual void render(const cJSON *state, const Rule *rule) = 0;

  /**
   * @description Icon of the entity when neither the layout nor a rule sets one.
   */
  virtual const char *default_icon(const cJSON *state) { return default_entity_icon(entity_); }

  lv_obj_t *make_bubble(lv_obj_t *parent, int size) {
    bubble_ = lv_obj_create(parent);
    lv_obj_remove_style_all(bubble_);
    lv_obj_add_style(bubble_, theme::bubble(), 0);
    lv_obj_set_size(bubble_, size, size);
    lv_obj_remove_flag(bubble_, LV_OBJ_FLAG_CLICKABLE);
    icon_ = theme::make_icon(bubble_, size >= 48 ? &dm_icons_32 : &dm_icons_24, "home-assistant", theme::text());
    lv_obj_center(icon_);
    return bubble_;
  }

  /**
   * @description Icon chosen by priority: rule, layout, entity.
   */
  const char *icon_for(const cJSON *state, const Rule *rule) {
    if (rule && !rule->icon.empty()) return rule->icon.c_str();
    if (!custom_icon_.empty()) return custom_icon_.c_str();
    return default_icon(state);
  }

  /**
   * @description Colors the card and the bubble: active (filled with `color`) or resting.
   */
  void paint(bool active, lv_color_t color, bool unavailable) {
    if (active) {
      theme::paint_active(root, color);
      lv_obj_add_state(root, LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(bubble_, 235, 0);
      lv_obj_set_style_text_color(icon_, lv_color_mix(color, lv_color_black(), 170), 0);
    } else {
      lv_obj_remove_state(root, LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(bubble_, 40, 0);
      lv_obj_set_style_text_color(icon_, unavailable ? theme::text3() : theme::text(), 0);
    }
  }

  std::string name(const cJSON *state) const { return entity_name(spec_.props, state, entity_); }

  WidgetContext &ctx_;
  WidgetSpec spec_;
  std::string entity_;
  std::string custom_icon_;
  lv_obj_t *bubble_ = nullptr, *icon_ = nullptr;

 private:
  void refresh(const cJSON *state) { render(state, rules_.match(state)); }

  void on_open(lv_event_t *) {
    const cJSON *state = ctx_.bus.get("ha/" + entity_);
    if (is_unavailable(state)) return;
    open_more_info(ctx_, entity_, name(state), icon_for(state, nullptr));
  }

  RuleBinding rules_;
};

/**
 * @description Cover tile: icon, status ("Ouvert à 40 %") and, from two columns, open/stop/close buttons.
 * A tap opens the popup (position and tilt sliders).
 */
class CoverTile : public ControlTile {
 public:
  CoverTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ControlTile(parent, spec, ctx) {
    const bool tall = spec.h >= 2;
    make_bubble(root, tall ? 56 : 40);
    lv_obj_align(bubble_, LV_ALIGN_TOP_LEFT, 0, 0);
    state_ = theme::make_label(root, &dm_font_14, theme::text2());
    lv_obj_align(state_, LV_ALIGN_TOP_RIGHT, 0, 2);

    lv_obj_t *bottom = make_box(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(bottom, lv_pct(100));
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(bottom, 6, 0);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    name_ = theme::make_label(bottom, spec.w >= 2 ? &dm_font_20 : &dm_font_16, theme::text());
    lv_obj_set_flex_grow(name_, 1);
    theme::single_line(name_);
    if (spec.w < 2) return;

    // Stop only where there is room for three buttons
    const bool with_stop = spec.w >= 3 || tall;
    const struct {
      const char *icon, *service;
    } BUTTONS[] = {{"chevron-up", "open_cover"}, {"stop", "stop_cover"}, {"chevron-down", "close_cover"}};
    for (const auto &b : BUTTONS) {
      if (!with_stop && !strcmp(b.service, "stop_cover")) continue;
      lv_obj_t *button = make_round_button(bottom, b.icon, 40);
      lv_obj_set_user_data(button, const_cast<char *>(b.service));
      lv_obj_add_event_cb(
          button,
          [](lv_event_t *e) {
            auto *self = static_cast<CoverTile *>(lv_event_get_user_data(e));
            auto *service = static_cast<const char *>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
            cover_command(self->ctx_.services, self->entity_, service);
          },
          LV_EVENT_CLICKED, this);
      buttons_.push_back(button);
    }
  }

 protected:
  const char *default_icon(const cJSON *state) override { return cover_icon(state); }

  void render(const cJSON *state, const Rule *rule) override {
    const std::string n = name(state);
    const bool unavailable = is_unavailable(state) && !rule;
    lv_label_set_text(name_, n.c_str());
    lv_label_set_text(icon_, theme::icon(icon_for(state, rule)));
    std::string text = unavailable ? (state ? "Indisponible" : "…") : cover_text(state, spec_.w == 1);
    if (rule && !rule->text.empty()) text = Rules::format(rule->text, state, n);
    lv_label_set_text(state_, text.c_str());
    for (lv_obj_t *button : buttons_) set_hidden(button, unavailable);

    const bool open = !unavailable && strcmp(json::str(state, "s"), "closed") != 0;
    const bool active = rule && rule->active >= 0 ? rule->active == 1 : open;
    const bool ruled_color = rule && rule->has_color;
    paint(active, ruled_color ? lv_color_hex(rule->color) : theme::accent(), unavailable);
    lv_obj_set_style_text_color(name_, unavailable ? theme::text3() : theme::text(), 0);
    lv_obj_set_style_text_color(state_, active ? lv_color_white() : ruled_color ? lv_color_hex(rule->color) : theme::text2(), 0);
  }

 private:
  lv_obj_t *state_ = nullptr, *name_ = nullptr;
  std::vector<lv_obj_t *> buttons_;
};

/**
 * @description Slider tile: icon, name and value on top, a slider across the bottom. Works with
 * input_number/number, lights (brightness), fans (speed), media players (volume) and covers (position).
 */
class SliderTile : public ControlTile {
 public:
  SliderTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ControlTile(parent, spec, ctx) {
    lv_obj_t *head = make_box(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, 0);
    // One row is ~60 px inside the card: a smaller bubble and a thinner slider share it
    const bool row = spec.h == 1;
    make_bubble(head, row ? 28 : 36);
    name_ = theme::make_label(head, &dm_font_16, theme::text());
    lv_obj_set_flex_grow(name_, 1);
    theme::single_line(name_);
    // A one-column tile only has room for the value
    if (spec.w == 1) set_hidden(name_, true);
    value_ = theme::make_label(head, spec.h >= 2 ? &dm_font_20 : &dm_font_16, theme::text());

    slider_ = std::make_unique<LevelSlider>(root, ctx, entity_, value_, false);
    lv_obj_set_width(slider_->slider, lv_pct(100));
    if (row) lv_obj_set_height(slider_->slider, 26);
    lv_obj_align(slider_->slider, LV_ALIGN_BOTTOM_MID, 0, row ? -2 : 0);
  }

 protected:
  void render(const cJSON *state, const Rule *rule) override {
    const std::string n = name(state);
    const Level level = level_of(entity_, state);
    const bool unavailable = !level.ok && !rule;
    lv_label_set_text(name_, n.c_str());
    lv_label_set_text(icon_, theme::icon(icon_for(state, rule)));
    set_hidden(slider_->slider, !level.ok);
    if (level.ok) slider_->render(state);
    else lv_label_set_text(value_, state ? "Indisponible" : "…");
    if (rule && !rule->text.empty()) lv_label_set_text(value_, Rules::format(rule->text, state, n).c_str());
    const bool ruled_color = rule && rule->has_color;
    lv_obj_set_style_text_color(value_, ruled_color ? lv_color_hex(rule->color) : theme::text(), 0);
    lv_obj_set_style_text_color(name_, unavailable ? theme::text3() : theme::text(), 0);
    paint(false, theme::accent(), unavailable);
    if (ruled_color) lv_obj_set_style_text_color(icon_, lv_color_hex(rule->color), 0);
  }

 private:
  std::unique_ptr<LevelSlider> slider_;
  lv_obj_t *name_ = nullptr, *value_ = nullptr;
};

/**
 * @description Choice tile (input_select/select options, fan presets): icon, name and current choice.
 * A tap opens the list.
 */
class SelectTile : public ControlTile {
 public:
  SelectTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ControlTile(parent, spec, ctx) {
    const bool tall = spec.h >= 2;
    if (!tall) {
      // One row: [bubble] name / choice [v]; a one-column tile keeps the choice only
      lv_obj_t *r = make_box(root, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(r, lv_pct(100), lv_pct(100));
      lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(r, 10, 0);
      if (spec.w >= 2) make_bubble(r, 40);
      lv_obj_t *col = make_box(r, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_grow(col, 1);
      build_labels(col, spec.w >= 2);
      theme::make_icon(r, &dm_icons_24, "chevron-down", theme::text3());
      if (spec.w < 2) {
        // The bubble is still needed for the colors: keep a hidden one
        make_bubble(root, 0);
        set_hidden(bubble_, true);
      }
      return;
    }
    make_bubble(root, 48);
    lv_obj_align(bubble_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *chevron = theme::make_icon(root, &dm_icons_24, "chevron-down", theme::text3());
    lv_obj_align(chevron, LV_ALIGN_TOP_RIGHT, 0, 4);
    lv_obj_t *bottom = make_box(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(bottom, lv_pct(100));
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    build_labels(bottom, true);
  }

 protected:
  const char *default_icon(const cJSON *state) override {
    return entity_domain(entity_) == "fan" ? "fan" : "format-list-bulleted";
  }

  void build_labels(lv_obj_t *parent, bool wide) {
    name_ = theme::make_label(parent, &dm_font_14, theme::text2());
    lv_obj_set_width(name_, lv_pct(100));
    theme::single_line(name_);
    value_ = theme::make_label(parent, wide ? &dm_font_20 : &dm_font_16, theme::text());
    lv_obj_set_width(value_, lv_pct(100));
    theme::single_line(value_);
  }

  void render(const cJSON *state, const Rule *rule) override {
    const std::string n = name(state);
    const bool unavailable = is_unavailable(state) && !rule;
    lv_label_set_text(name_, n.c_str());
    lv_label_set_text(icon_, theme::icon(icon_for(state, rule)));
    const Options options = options_of(entity_, state);
    std::string text = unavailable ? (state ? "Indisponible" : "…") : options.current.empty() ? "—" : option_text(entity_, options.current);
    if (rule && !rule->text.empty()) text = Rules::format(rule->text, state, n);
    lv_label_set_text(value_, text.c_str());
    const bool active = rule && rule->active == 1;
    const bool ruled_color = rule && rule->has_color;
    paint(active, ruled_color ? lv_color_hex(rule->color) : theme::accent(), unavailable);
    lv_obj_set_style_text_color(value_, unavailable ? theme::text3() : ruled_color && !active ? lv_color_hex(rule->color) : theme::text(), 0);
  }

 private:
  lv_obj_t *name_ = nullptr, *value_ = nullptr;
};

template <typename T>
std::unique_ptr<Widget> create_tile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  auto tile = std::make_unique<T>(parent, spec, ctx);
  tile->start();
  return tile;
}

}  // namespace

bool open_more_info(WidgetContext &ctx, const std::string &entity_id, const std::string &name, const char *icon) {
  const std::string domain = entity_domain(entity_id);
  const bool supported = domain == "light" || domain == "climate" || domain == "water_heater" || domain == "cover" ||
                         domain == "fan" || domain == "input_number" || domain == "number" ||
                         domain == "media_player" || domain == "input_select" || domain == "select";
  if (!supported) return false;
  if (!ctx.services.ha_call) {
    ctx.app.toast("Home Assistant non configuré");
    return true;
  }
  if (domain == "light") open_light_popup(ctx, entity_id, name, icon);
  else if (domain == "climate" || domain == "water_heater") open_climate_popup(ctx, entity_id, name, icon);
  else if (domain == "media_player" && !level_of(entity_id, ctx.bus.get("ha/" + entity_id)).ok) return false;
  else new ControlSheet(ctx, entity_id, name, icon);  // owned by its sheet, deleted on close
  return true;
}

std::unique_ptr<Widget> create_ha_cover(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return create_tile<CoverTile>(parent, spec, ctx);
}

std::unique_ptr<Widget> create_ha_slider(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return create_tile<SliderTile>(parent, spec, ctx);
}

std::unique_ptr<Widget> create_ha_select(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return create_tile<SelectTile>(parent, spec, ctx);
}

}  // namespace dm::widgets
