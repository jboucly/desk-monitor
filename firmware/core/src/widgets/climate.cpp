// ha_climate: thermostat tile for climate.* and water_heater.* entities, and its popup (target temperature,
// HVAC or operation mode, preset, fan mode). Commands go out through climate.* / water_heater.* services.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

constexpr uint32_t DEBOUNCE_MS = 800;  // −/+ taps are grouped into one set_temperature
constexpr uint32_t PENDING_MS = 5000;  // a sent target is shown over older states for this long at most

constexpr uint32_t HEAT_COLOR = 0xF97316;
constexpr uint32_t COOL_COLOR = 0x38BDF8;
constexpr uint32_t DRY_COLOR = 0x2DD4BF;

/**
 * @description The fields of a climate or water heater state the tile and the popup use.
 */
struct Climate {
  bool water = false;   // water_heater (operation modes) instead of climate (HVAC modes)
  std::string mode;     // HVAC mode or operation mode: the entity state
  std::string action;   // hvac_action ("heating", "idle"…), empty when the device does not report it
  double current = NAN, target = NAN, low = NAN, high = NAN;
  double min = 7, max = 35, step = 0.5;

  /**
   * @description True when the device keeps a range (heat_cool) instead of a single target.
   */
  bool range() const { return std::isnan(target) && !std::isnan(low) && !std::isnan(high); }

  /**
   * @description Decimals of the temperatures, from the target step.
   */
  int decimals() const { return step < 1 ? 1 : 0; }
};

Climate read_climate(const std::string &entity, const cJSON *state) {
  Climate c;
  c.water = entity_domain(entity) == "water_heater";
  const cJSON *a = json::get(state, "a");
  c.mode = json::str(state, "s");
  c.action = json::str(a, "hvac_action");
  // HA sends null for the fields a mode does not use: json::num() then gives the NaN fallback
  c.current = json::num(a, "current_temperature", NAN);
  c.target = json::num(a, "temperature", NAN);
  c.low = json::num(a, "target_temp_low", NAN);
  c.high = json::num(a, "target_temp_high", NAN);
  c.min = json::num(a, "min_temp", c.water ? 30 : 7);
  c.max = json::num(a, "max_temp", c.water ? 70 : 35);
  if (c.max <= c.min) c.max = c.min + 1;
  c.step = json::num(a, "target_temp_step", c.water ? 1 : 0.5);
  if (c.step <= 0) c.step = 0.5;
  return c;
}

/**
 * @description "21,5°" (or "—" for NaN).
 */
std::string degrees(double value, int decimals) {
  if (std::isnan(value)) return "—";
  return util::format_number(value, decimals) + "°";
}

/**
 * @description Kind of option a chip of the popup sets.
 */
enum class Option { Mode, Preset, Fan };

/**
 * @description French label of an HVAC mode, operation mode, preset or fan mode. Unknown values are
 * shown as sent by HA, with spaces and a capital letter ("high_demand" → "High demand").
 */
std::string option_label(Option kind, const std::string &value) {
  static const struct {
    Option kind;
    const char *value, *label;
  } LABELS[] = {
      {Option::Mode, "off", "Arrêt"},           {Option::Mode, "heat", "Chauffage"},
      {Option::Mode, "cool", "Clim"},           {Option::Mode, "heat_cool", "Chaud/froid"},
      {Option::Mode, "auto", "Auto"},           {Option::Mode, "dry", "Séchage"},
      {Option::Mode, "fan_only", "Ventilation"}, {Option::Mode, "eco", "Éco"},
      {Option::Mode, "electric", "Électrique"}, {Option::Mode, "performance", "Performance"},
      {Option::Mode, "heat_pump", "Pompe à chaleur"}, {Option::Mode, "gas", "Gaz"},
      {Option::Mode, "high_demand", "Forte demande"}, {Option::Preset, "none", "Aucun"},
      {Option::Preset, "eco", "Éco"},           {Option::Preset, "comfort", "Confort"},
      {Option::Preset, "away", "Absent"},       {Option::Preset, "home", "Maison"},
      {Option::Preset, "sleep", "Nuit"},        {Option::Preset, "boost", "Boost"},
      {Option::Preset, "activity", "Activité"}, {Option::Fan, "auto", "Auto"},
      {Option::Fan, "low", "Faible"},           {Option::Fan, "medium", "Moyen"},
      {Option::Fan, "middle", "Moyen"},         {Option::Fan, "high", "Fort"},
      {Option::Fan, "on", "Marche"},            {Option::Fan, "off", "Arrêt"},
      {Option::Fan, "quiet", "Silence"},
  };
  for (const auto &entry : LABELS)
    if (entry.kind == kind && value == entry.value) return entry.label;
  std::string out = value;
  for (char &c : out)
    if (c == '_') c = ' ';
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');
  return out;
}

/**
 * @description Icon of an HVAC mode chip, or null for a text-only chip.
 */
const char *mode_icon(const std::string &mode) {
  if (mode == "off") return "power";
  if (mode == "heat") return "fire";
  if (mode == "cool") return "snowflake";
  if (mode == "heat_cool") return "sun-snowflake-variant";
  if (mode == "auto") return "autorenew";
  if (mode == "dry") return "water-percent";
  if (mode == "fan_only") return "fan";
  if (mode == "eco") return "leaf";
  return nullptr;
}

/**
 * @description How the entity looks right now: icon, color, status text and whether the card is "active".
 */
struct Look {
  const char *icon;
  lv_color_t color;
  std::string text;
  bool active;
};

Look look_of(const Climate &c, const char *base_icon) {
  if (c.mode == "off") return {base_icon, theme::text3(), "Arrêt", false};
  const std::string &a = c.action;
  if (a == "heating" || a == "preheating") return {"fire", lv_color_hex(HEAT_COLOR), "Chauffe", true};
  if (a == "cooling") return {"snowflake", lv_color_hex(COOL_COLOR), "Refroidit", true};
  if (a == "drying") return {"water-percent", lv_color_hex(DRY_COLOR), "Déshumidifie", true};
  if (a == "fan") return {"fan", lv_color_hex(DRY_COLOR), "Ventile", true};
  if (a == "idle") return {base_icon, theme::accent(), "En attente", false};
  if (a == "off") return {base_icon, theme::text3(), "Arrêt", false};
  // No hvac_action (many thermostats, every water heater): the mode tells it
  const lv_color_t color = c.mode == "cool" ? lv_color_hex(COOL_COLOR) : lv_color_hex(HEAT_COLOR);
  return {base_icon, color, option_label(Option::Mode, c.mode), c.water};
}

const char *base_icon_of(const std::string &entity) {
  return entity_domain(entity) == "water_heater" ? "water-boiler" : "thermostat";
}

/**
 * @description Sends target temperatures, grouping quick changes, and remembers what was sent so that the
 * display does not jump back to the old value while HA has not confirmed it yet.
 */
class TargetSender {
 public:
  enum Field { Target, Low, High, FIELDS };

  TargetSender(WidgetContext &ctx, std::string entity) : ctx_(ctx), entity_(std::move(entity)) {}
  ~TargetSender() {
    // A change still waiting (popup closed, layout rebuilt) is sent rather than lost
    if (timer_) send();
  }
  TargetSender(const TargetSender &) = delete;
  TargetSender &operator=(const TargetSender &) = delete;

  /**
   * @description Value to show for a field: the pending one, else the state's. Drops a pending value once
   * the state agrees with it, or once it is too old.
   */
  double shown(Field field, double state_value) {
    double &pending = pending_[field];
    if (std::isnan(pending)) return state_value;
    const bool confirmed = !std::isnan(state_value) && std::fabs(state_value - pending) < 0.01;
    if (!timer_ && (confirmed || lv_tick_elaps(changed_at_) > PENDING_MS)) {
      pending = NAN;
      return state_value;
    }
    return pending;
  }

  /**
   * @description Records a new value. It is sent after DEBOUNCE_MS without another change, or at once.
   */
  void set(Field field, double value, bool now) {
    pending_[field] = value;
    changed_at_ = lv_tick_get();
    if (now) return send();
    if (timer_) return lv_timer_reset(timer_);
    timer_ = lv_timer_create(
        [](lv_timer_t *t) {
          auto *self = static_cast<TargetSender *>(lv_timer_get_user_data(t));
          self->timer_ = nullptr;  // a one-shot timer deletes itself after this call
          self->send();
        },
        DEBOUNCE_MS, this);
    lv_timer_set_repeat_count(timer_, 1);
  }

 private:
  void send() {
    if (timer_) {
      lv_timer_delete(timer_);
      timer_ = nullptr;
    }
    changed_at_ = lv_tick_get();
    if (!ctx_.services.ha_call) return;
    const Climate c = read_climate(entity_, ctx_.bus.get("ha/" + entity_));
    cJSON *data = cJSON_CreateObject();
    if (!std::isnan(pending_[Target])) {
      cJSON_AddNumberToObject(data, "temperature", pending_[Target]);
    } else {
      // A range is always sent whole
      cJSON_AddNumberToObject(data, "target_temp_low", std::isnan(pending_[Low]) ? c.low : pending_[Low]);
      cJSON_AddNumberToObject(data, "target_temp_high", std::isnan(pending_[High]) ? c.high : pending_[High]);
    }
    ctx_.services.ha_call(entity_domain(entity_), "set_temperature", entity_, data);
  }

  WidgetContext &ctx_;
  std::string entity_;
  double pending_[FIELDS] = {NAN, NAN, NAN};
  uint32_t changed_at_ = 0;
  lv_timer_t *timer_ = nullptr;
};

/**
 * @description Snaps a temperature to the step grid and the device limits.
 */
double snap(const Climate &c, double value) {
  return std::clamp(c.min + std::round((value - c.min) / c.step) * c.step, c.min, c.max);
}

// ── Popup ────────────────────────────────────────────────────────────────────

constexpr int SHEET_W = 440;
constexpr int SHEET_MAX_H = 452;
constexpr int CHIP_H = 36;

/**
 * @description One open climate popup. Deleted when its sheet closes.
 */
class ClimatePopup {
 public:
  ClimatePopup(WidgetContext &ctx, std::string entity_id, const std::string &name, const char *icon)
      : ctx_(ctx), entity_(std::move(entity_id)), sender_(ctx, entity_), icon_name_(icon) {
    const cJSON *state = ctx_.bus.get("ha/" + entity_);
    const cJSON *attrs = json::get(state, "a");
    const Climate c = read_climate(entity_, state);
    range_ = c.range();

    const cJSON *modes = json::get(attrs, c.water ? "operation_list" : "hvac_modes");
    const cJSON *presets = c.water ? nullptr : json::get(attrs, "preset_modes");
    const cJSON *fans = c.water ? nullptr : json::get(attrs, "fan_modes");
    const int rows = (cJSON_GetArraySize(modes) > 0) + (cJSON_GetArraySize(presets) > 0) + (cJSON_GetArraySize(fans) > 0);

    // The dial takes what the option rows leave
    constexpr int PAD = 12;
    const int fixed = 36 + 48 + PAD + rows * (CHIP_H + PAD) + (range_ ? 34 + PAD : 0);
    dial_ = std::clamp(SHEET_MAX_H - fixed - PAD, 140, 210);
    panel_ = ctx_.app.open_sheet(SHEET_W, fixed + PAD + dial_, [this] { delete this; });
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel_, PAD, 0);

    build_header(name);
    build_dial(c);
    if (range_) build_range_tabs();
    add_options(Option::Mode, modes, nullptr);
    add_options(Option::Preset, presets, "leaf");
    add_options(Option::Fan, fans, "fan");

    token_ = ctx_.bus.subscribe("ha/" + entity_, [this](const cJSON *s) { render(s); });
  }

  ~ClimatePopup() { ctx_.bus.unsubscribe(token_); }

 private:
  struct Chip {
    lv_obj_t *obj;
    Option kind;
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

    switch_ = lv_switch_create(head);
    lv_obj_set_size(switch_, 60, 32);
    lv_obj_set_style_bg_color(switch_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(switch_, 50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(switch_, theme::accent(), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
    lv_obj_add_event_cb(
        switch_,
        [](lv_event_t *e) {
          auto *self = static_cast<ClimatePopup *>(lv_event_get_user_data(e));
          const bool on = lv_obj_has_state(self->switch_, LV_STATE_CHECKED);
          self->ctx_.services.ha_call(entity_domain(self->entity_), on ? "turn_on" : "turn_off", self->entity_, nullptr);
        },
        LV_EVENT_VALUE_CHANGED, this);
  }

  void build_dial(const Climate &c) {
    lv_obj_t *r = full_row(panel_);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    minus_ = make_round_button(r, "minus", 52);
    lv_obj_add_event_cb(minus_, on_step, LV_EVENT_CLICKED, this);

    arc_ = lv_arc_create(r);
    lv_obj_set_size(arc_, dial_, dial_);
    lv_arc_set_rotation(arc_, 135);
    lv_arc_set_bg_angles(arc_, 0, 270);
    steps_ = std::max(1, static_cast<int>(std::lround((c.max - c.min) / c.step)));
    lv_arc_set_range(arc_, 0, steps_);
    const int width = dial_ >= 180 ? 16 : 12;
    lv_obj_set_style_arc_width(arc_, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_, 40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(arc_, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc_, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(arc_, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(arc_, LV_OPA_40, LV_PART_KNOB);
    lv_obj_add_event_cb(arc_, on_arc, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(arc_, on_arc, LV_EVENT_RELEASED, this);

    // Target and current temperature stacked in the middle of the dial
    lv_obj_t *center = make_box(arc_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(center);
    target_ = theme::make_label(center, dial_ >= 170 ? &dm_font_40 : &dm_font_28, theme::text(), "—");
    current_ = theme::make_label(center, &dm_font_14, theme::text2());

    plus_ = make_round_button(r, "plus", 52);
    lv_obj_add_event_cb(plus_, on_step, LV_EVENT_CLICKED, this);
  }

  void build_range_tabs() {
    lv_obj_t *r = full_row(panel_);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    for (int i = 0; i < 2; i++) {
      range_tabs_[i] = make_chip(r, i == 0 ? "Minimum" : "Maximum", i == 0 ? "fire" : "snowflake", 34);
      lv_obj_add_event_cb(
          range_tabs_[i],
          [](lv_event_t *e) {
            auto *self = static_cast<ClimatePopup *>(lv_event_get_user_data(e));
            self->edit_high_ = lv_event_get_current_target(e) == self->range_tabs_[1];
            self->render(self->ctx_.bus.get("ha/" + self->entity_));
          },
          LV_EVENT_CLICKED, this);
    }
  }

  void add_options(Option kind, const cJSON *values, const char *row_icon) {
    if (cJSON_GetArraySize(values) == 0) return;
    lv_obj_t *r = full_row(panel_);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_height(r, CHIP_H);
    // More chips than the width: the row scrolls sideways
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(r, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    if (row_icon) theme::make_icon(r, &dm_icons_24, row_icon, theme::text2());
    const cJSON *v = nullptr;
    cJSON_ArrayForEach(v, values) {
      if (!cJSON_IsString(v)) continue;
      const std::string value = v->valuestring;
      lv_obj_t *chip = make_chip(r, option_label(kind, value), kind == Option::Mode ? mode_icon(value) : nullptr, CHIP_H);
      lv_obj_add_event_cb(chip, on_chip, LV_EVENT_CLICKED, this);
      chips_.push_back({chip, kind, value});
    }
  }

  TargetSender::Field edited_field() const {
    return !range_ ? TargetSender::Target : edit_high_ ? TargetSender::High : TargetSender::Low;
  }

  /**
   * @description Temperature the dial edits, pending value included.
   */
  double edited_value(const Climate &c) {
    const TargetSender::Field f = edited_field();
    return sender_.shown(f, f == TargetSender::Target ? c.target : f == TargetSender::High ? c.high : c.low);
  }

  /**
   * @description Syncs the controls with the entity state, except the dial while it is dragged.
   */
  void render(const cJSON *state) {
    const Climate c = read_climate(entity_, state);
    const Look look = look_of(c, icon_name_.c_str());
    const bool on = c.mode != "off" && !is_unavailable(state);

    if (on) lv_obj_add_state(switch_, LV_STATE_CHECKED);
    else lv_obj_remove_state(switch_, LV_STATE_CHECKED);
    lv_label_set_text(icon_, theme::icon(look.icon));
    lv_obj_set_style_bg_opa(bubble_, look.active ? 235 : 40, 0);
    lv_obj_set_style_bg_color(bubble_, look.active ? look.color : lv_color_white(), 0);
    lv_obj_set_style_text_color(icon_, look.active ? lv_color_mix(look.color, lv_color_black(), 140) : look.color, 0);
    lv_label_set_text(status_, is_unavailable(state) ? "Indisponible" : look.text.c_str());

    const int dec = c.decimals();
    lv_label_set_text(current_, std::isnan(c.current) ? "" : ("Actuel " + degrees(c.current, dec)).c_str());
    lv_obj_set_style_arc_color(arc_, on ? look.color : theme::text3(), LV_PART_INDICATOR);
    if (!dragging_) {
      const double value = edited_value(c);
      lv_label_set_text(target_, degrees(value, dec).c_str());
      if (!std::isnan(value)) lv_arc_set_value(arc_, static_cast<int>(std::lround((value - c.min) / c.step)));
    }
    if (range_) {
      for (int i = 0; i < 2; i++) {
        if ((i == 1) == edit_high_) lv_obj_add_state(range_tabs_[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(range_tabs_[i], LV_STATE_CHECKED);
      }
    }

    const char *preset = json::str(json::get(state, "a"), "preset_mode");
    const char *fan = json::str(json::get(state, "a"), "fan_mode");
    for (const Chip &chip : chips_) {
      const std::string current = chip.kind == Option::Mode ? c.mode : chip.kind == Option::Preset ? preset : fan;
      if (chip.value == current) lv_obj_add_state(chip.obj, LV_STATE_CHECKED);
      else lv_obj_remove_state(chip.obj, LV_STATE_CHECKED);
    }
  }

  static void on_arc(lv_event_t *e) {
    auto *self = static_cast<ClimatePopup *>(lv_event_get_user_data(e));
    const Climate c = read_climate(self->entity_, self->ctx_.bus.get("ha/" + self->entity_));
    const double value = snap(c, c.min + lv_arc_get_value(self->arc_) * c.step);
    lv_label_set_text(self->target_, degrees(value, c.decimals()).c_str());
    self->dragging_ = lv_event_get_code(e) != LV_EVENT_RELEASED;
    if (!self->dragging_) self->sender_.set(self->edited_field(), value, true);
  }

  static void on_step(lv_event_t *e) {
    auto *self = static_cast<ClimatePopup *>(lv_event_get_user_data(e));
    const cJSON *state = self->ctx_.bus.get("ha/" + self->entity_);
    if (is_unavailable(state)) return;
    const Climate c = read_climate(self->entity_, state);
    const double base = self->edited_value(c);
    if (std::isnan(base)) return;
    const double dir = lv_event_get_current_target(e) == self->plus_ ? 1 : -1;
    self->sender_.set(self->edited_field(), snap(c, base + dir * c.step), false);
    self->render(state);
  }

  static void on_chip(lv_event_t *e) {
    auto *self = static_cast<ClimatePopup *>(lv_event_get_user_data(e));
    const lv_obj_t *target = lv_event_get_current_target_obj(e);
    for (const Chip &chip : self->chips_) {
      if (chip.obj != target) continue;
      const std::string domain = entity_domain(self->entity_);
      cJSON *data = cJSON_CreateObject();
      const char *service = "set_hvac_mode";
      if (chip.kind == Option::Mode && domain == "water_heater") {
        service = "set_operation_mode";
        cJSON_AddStringToObject(data, "operation_mode", chip.value.c_str());
      } else if (chip.kind == Option::Mode) {
        cJSON_AddStringToObject(data, "hvac_mode", chip.value.c_str());
      } else if (chip.kind == Option::Preset) {
        service = "set_preset_mode";
        cJSON_AddStringToObject(data, "preset_mode", chip.value.c_str());
      } else {
        service = "set_fan_mode";
        cJSON_AddStringToObject(data, "fan_mode", chip.value.c_str());
      }
      self->ctx_.services.ha_call(domain, service, self->entity_, data);
      return;
    }
  }

  WidgetContext &ctx_;
  std::string entity_;
  TargetSender sender_;
  std::string icon_name_;
  bool range_ = false, edit_high_ = false, dragging_ = false;
  int dial_ = 180, steps_ = 1;
  int token_ = 0;
  std::vector<Chip> chips_;
  lv_obj_t *panel_ = nullptr, *bubble_ = nullptr, *icon_ = nullptr, *status_ = nullptr, *switch_ = nullptr;
  lv_obj_t *arc_ = nullptr, *target_ = nullptr, *current_ = nullptr, *minus_ = nullptr, *plus_ = nullptr;
  lv_obj_t *range_tabs_[2] = {nullptr, nullptr};
};

// ── Tile ─────────────────────────────────────────────────────────────────────

/**
 * @description Thermostat tile. A tap (or a long press) opens the popup unless the layout sets other actions; on tiles with room, −/+ change
 * the target directly. A 1-column tile shows the current temperature only.
 *
 *   2×1: [bubble] name / status          [−] 21,0° [+]   (buttons from 3 columns)
 *   2×2: [bubble] name / status
 *        20,5°                            [−] 21,0° [+]
 */
class ClimateTile : public Widget {
 public:
  ClimateTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx)
      : ctx_(ctx), spec_(spec), entity_(json::str(spec.props, "entity")), sender_(ctx, entity_) {
    custom_icon_ = json::str(spec.props, "icon");
    narrow_ = spec.w == 1;
    const bool tall = spec.h >= 2;

    root = theme::make_card(parent);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    bind_event<ClimateTile, &ClimateTile::on_gesture>(root, LV_EVENT_SHORT_CLICKED, this);
    bind_event<ClimateTile, &ClimateTile::on_gesture>(root, LV_EVENT_LONG_PRESSED, this);

    if (narrow_) {
      // Same arrangement as the toggle tiles: bubble, value top right, name at the bottom
      bubble_ = make_bubble(root, tall ? 56 : 40, tall);
      lv_obj_align(bubble_, LV_ALIGN_TOP_LEFT, 0, 0);
      temp_ = theme::make_label(root, tall ? &dm_font_28 : &dm_font_16, theme::text());
      lv_obj_align(temp_, tall ? LV_ALIGN_LEFT_MID : LV_ALIGN_TOP_RIGHT, 0, tall ? 10 : 2);
      name_ = theme::make_label(root, &dm_font_16, theme::text());
      lv_obj_set_width(name_, lv_pct(100));
      theme::single_line(name_);
      lv_obj_align(name_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else {
      lv_obj_t *head = make_box(root, LV_FLEX_FLOW_ROW);
      lv_obj_set_width(head, lv_pct(100));
      lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(head, 10, 0);
      lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, 0);
      bubble_ = make_bubble(head, tall ? 48 : 40, tall);
      lv_obj_t *col = make_box(head, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_grow(col, 1);
      name_ = theme::make_label(col, &dm_font_16, theme::text());
      lv_obj_set_width(name_, lv_pct(100));
      theme::single_line(name_);
      status_ = theme::make_label(col, &dm_font_14, theme::text2());
      lv_obj_set_width(status_, lv_pct(100));
      theme::single_line(status_);

      if (!tall) {
        build_controls(head, spec.w >= 3);
      } else if (spec.w >= 3) {
        // Wide enough: current temperature and controls share the bottom row
        lv_obj_t *bottom = make_box(root, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(bottom, lv_pct(100));
        lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_align(bottom, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        temp_ = theme::make_label(bottom, &dm_font_40, theme::text());
        build_controls(bottom, true);
      } else {
        // Two columns: current temperature in the middle, controls across the bottom
        temp_ = theme::make_label(root, &dm_font_40, theme::text());
        lv_obj_align(temp_, LV_ALIGN_LEFT_MID, 0, 8);
        lv_obj_t *controls = build_controls(root, true);
        lv_obj_set_width(controls, lv_pct(100));
        lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, 0);
      }
    }
  }

  /**
   * @description Subscribes to the entity (called by the factory once the tile is built).
   */
  void start() {
    render(nullptr);
    // Rules first: the bus calls the entity listener right away when it already has a state
    rules_.bind(spec_.props, ctx_.bus, [this] { render(ctx_.bus.get("ha/" + entity_)); });
    ctx_.bus.subscribe("ha/" + entity_, [this](const cJSON *state) { render(state); });
  }

 private:
  lv_obj_t *make_bubble(lv_obj_t *parent, int size, bool big) {
    lv_obj_t *bubble = lv_obj_create(parent);
    lv_obj_remove_style_all(bubble);
    lv_obj_add_style(bubble, theme::bubble(), 0);
    lv_obj_set_size(bubble, size, size);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    icon_ = theme::make_icon(bubble, big ? &dm_icons_32 : &dm_icons_24, base_icon(), theme::text());
    lv_obj_center(icon_);
    return bubble;
  }

  /**
   * @description Builds the target label, with −/+ around it when `buttons`. Returns their row.
   */
  lv_obj_t *build_controls(lv_obj_t *parent, bool buttons) {
    lv_obj_t *box = make_box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 6, 0);
    if (buttons) {
      minus_ = make_round_button(box, "minus", 40);
      lv_obj_add_event_cb(minus_, on_step, LV_EVENT_CLICKED, this);
    }
    target_ = theme::make_label(box, &dm_font_20, theme::text(), "—");
    lv_obj_set_style_min_width(target_, buttons ? 62 : 0, 0);
    lv_obj_set_style_text_align(target_, LV_TEXT_ALIGN_CENTER, 0);
    if (buttons) {
      plus_ = make_round_button(box, "plus", 40);
      lv_obj_add_event_cb(plus_, on_step, LV_EVENT_CLICKED, this);
    }
    return box;
  }

  const char *base_icon() const { return custom_icon_.empty() ? base_icon_of(entity_) : custom_icon_.c_str(); }

  void render(const cJSON *state) {
    const Climate c = read_climate(entity_, state);
    const Rule *rule = rules_.match(state);
    const std::string name = entity_name(spec_.props, state, entity_);
    const bool unavailable = is_unavailable(state) && !rule;
    Look look = look_of(c, base_icon());
    if (rule && !rule->icon.empty()) look.icon = rule->icon.c_str();
    if (rule && rule->has_color) look.color = lv_color_hex(rule->color);
    if (rule && rule->active >= 0) look.active = rule->active == 1;
    if (rule && !rule->text.empty()) look.text = Rules::format(rule->text, state, name);
    if (unavailable) look = {base_icon(), theme::text3(), state ? "Indisponible" : "…", false};

    lv_label_set_text(name_, name.c_str());
    lv_label_set_text(icon_, theme::icon(look.icon));
    if (status_) lv_label_set_text(status_, look.text.c_str());
    const int dec = c.decimals();
    if (temp_) lv_label_set_text(temp_, degrees(c.current, dec).c_str());
    if (target_) {
      if (c.range()) {
        lv_label_set_text(target_, (util::format_number(c.low, dec) + "–" + degrees(c.high, dec)).c_str());
      } else if (std::isnan(c.target) || c.mode == "off") {
        // Off: the status line already says "Arrêt"
        lv_label_set_text(target_, "");
      } else {
        lv_label_set_text(target_, degrees(sender_.shown(TargetSender::Target, c.target), dec).c_str());
      }
    }
    // −/+ only make sense for a single target of a running device
    const bool steppable = !unavailable && !c.range() && !std::isnan(c.target) && c.mode != "off";
    for (lv_obj_t *button : {minus_, plus_}) {
      if (!button) continue;
      if (steppable) lv_obj_remove_flag(button, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_style_text_color(name_, unavailable ? theme::text3() : theme::text(), 0);
    if (look.active) {
      theme::paint_active(root, look.color);
      lv_obj_add_state(root, LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(bubble_, 235, 0);
      lv_obj_set_style_text_color(icon_, lv_color_mix(look.color, lv_color_black(), 170), 0);
      if (status_) lv_obj_set_style_text_color(status_, lv_color_white(), 0);
    } else {
      lv_obj_remove_state(root, LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(bubble_, 40, 0);
      lv_obj_set_style_text_color(icon_, look.color, 0);
      if (status_) lv_obj_set_style_text_color(status_, rule && rule->has_color ? look.color : theme::text2(), 0);
    }
  }

  /**
   * @description Tap or long press: the layout's tap_action/hold_action, by default the thermostat popup.
   */
  void on_gesture(lv_event_t *e) {
    const bool hold = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    const cJSON *state = ctx_.bus.get("ha/" + entity_);
    run_tile_action(ctx_, spec_.props, hold ? "hold_action" : "tap_action", "more_info", entity_,
                    entity_name(spec_.props, state, entity_), base_icon());
  }

  static void on_step(lv_event_t *e) {
    auto *self = static_cast<ClimateTile *>(lv_event_get_user_data(e));
    const cJSON *state = self->ctx_.bus.get("ha/" + self->entity_);
    const Climate c = read_climate(self->entity_, state);
    const double base = self->sender_.shown(TargetSender::Target, c.target);
    if (is_unavailable(state) || std::isnan(base)) return;
    const double dir = lv_event_get_current_target(e) == self->plus_ ? 1 : -1;
    self->sender_.set(TargetSender::Target, snap(c, base + dir * c.step), false);
    self->render(state);
  }

  WidgetContext &ctx_;
  WidgetSpec spec_;
  std::string entity_;
  std::string custom_icon_;
  TargetSender sender_;
  RuleBinding rules_;
  bool narrow_ = false;
  lv_obj_t *bubble_ = nullptr, *icon_ = nullptr, *name_ = nullptr, *status_ = nullptr;
  lv_obj_t *temp_ = nullptr, *target_ = nullptr, *minus_ = nullptr, *plus_ = nullptr;
};

}  // namespace

void open_climate_popup(WidgetContext &ctx, const std::string &entity_id, const std::string &name, const char *icon) {
  if (!ctx.services.ha_call) return ctx.app.toast("Home Assistant non configuré");
  new ClimatePopup(ctx, entity_id, name, icon);  // owned by its sheet, deleted on close
}

std::unique_ptr<Widget> create_ha_climate(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  auto tile = std::make_unique<ClimateTile>(parent, spec, ctx);
  tile->start();
  return tile;
}

}  // namespace dm::widgets
