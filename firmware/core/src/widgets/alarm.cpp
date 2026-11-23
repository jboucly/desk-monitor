// Alarm panels and locks: the ha_alarm tile and the keypad popup (arm/disarm, lock/unlock/open, with the
// code typed on the screen when the entity has a code_format).
#include <algorithm>
#include <cstring>
#include <vector>

#include "common.h"

namespace dm::widgets {

namespace {

// HA AlarmControlPanelEntityFeature and LockEntityFeature bits
constexpr int ALARM_ARM_HOME = 1, ALARM_ARM_AWAY = 2, ALARM_ARM_NIGHT = 4, ALARM_ARM_VACATION = 32;
constexpr int LOCK_OPEN = 1;

constexpr int MAX_CODE = 8;
constexpr int KEY_W = 60, KEY_H = 50, KEY_GAP = 8;

/**
 * @description How an alarm or lock state looks: text, icon, color, whether the card is "active".
 */
struct SecurityLook {
  const char *text, *icon;
  lv_color_t color;
  bool active;
};

SecurityLook security_look(const std::string &entity, const cJSON *state) {
  const std::string s = json::str(state, "s");
  if (entity_domain(entity) == "lock") {
    if (s == "locked") return {"Verrouillé", "lock", theme::ok(), false};
    if (s == "unlocked") return {"Déverrouillé", "lock-open-variant", theme::warn(), true};
    if (s == "open") return {"Ouvert", "door-open", theme::warn(), true};
    if (s == "locking") return {"Verrouillage…", "lock", theme::warn(), false};
    if (s == "unlocking" || s == "opening") return {"Déverrouillage…", "lock-open-variant", theme::warn(), false};
    if (s == "jammed") return {"Bloqué !", "lock", theme::crit(), true};
    return {"Indisponible", "lock", theme::text3(), false};
  }
  if (s == "disarmed") return {"Désarmée", "shield-off", theme::text2(), false};
  if (s == "armed_home") return {"Armée (maison)", "shield-home", theme::accent(), true};
  if (s == "armed_away") return {"Armée (absent)", "shield-lock", theme::accent(), true};
  if (s == "armed_night") return {"Armée (nuit)", "shield-moon", theme::accent(), true};
  if (s == "armed_vacation") return {"Armée (vacances)", "shield-airplane", theme::accent(), true};
  if (s == "armed_custom_bypass") return {"Armée (perso)", "shield-lock", theme::accent(), true};
  if (s == "pending") return {"En attente…", "shield-alert-outline", theme::warn(), true};
  if (s == "arming") return {"Armement…", "shield-lock", theme::warn(), false};
  if (s == "disarming") return {"Désarmement…", "shield-off", theme::warn(), false};
  if (s == "triggered") return {"Déclenchée !", "bell-ring", theme::crit(), true};
  return {"Indisponible", "shield-off", theme::text3(), false};
}

/**
 * @description True when the entity asks for a code (its code_format is set).
 */
bool has_code(const cJSON *state) { return cJSON_IsString(json::get(json::get(state, "a"), "code_format")); }

/**
 * @description A button of the popup: label, service, and whether it needs the code.
 */
struct Command {
  const char *label, *icon, *service;
  bool needs_code;
};

std::vector<Command> commands_of(const std::string &entity, const cJSON *state) {
  const cJSON *attrs = json::get(state, "a");
  const int features = static_cast<int>(json::num(attrs, "supported_features", 0));
  const bool code = has_code(state);
  std::vector<Command> out;
  if (entity_domain(entity) == "lock") {
    out.push_back({"Verrouiller", "lock", "lock", code});
    out.push_back({"Déverrouiller", "lock-open-variant", "unlock", code});
    if (features & LOCK_OPEN) out.push_back({"Ouvrir", "door-open", "open", code});
    return out;
  }
  // Arming needs the code only when the panel says so (code_arm_required, true by default)
  const bool arm_code = code && json::boolean(attrs, "code_arm_required", true);
  out.push_back({"Désarmer", "shield-off", "alarm_disarm", code});
  if (features & ALARM_ARM_HOME) out.push_back({"Maison", "shield-home", "alarm_arm_home", arm_code});
  if (features & ALARM_ARM_AWAY) out.push_back({"Absent", "shield-lock", "alarm_arm_away", arm_code});
  if (features & ALARM_ARM_NIGHT) out.push_back({"Nuit", "shield-moon", "alarm_arm_night", arm_code});
  if (features & ALARM_ARM_VACATION) out.push_back({"Vacances", "shield-airplane", "alarm_arm_vacation", arm_code});
  return out;
}

/**
 * @description Popup of an alarm panel or a lock: its commands, and a keypad when a code is needed.
 * The code only lives in this object and is sent with the command. Deleted when the sheet closes.
 */
class KeypadSheet {
 public:
  KeypadSheet(WidgetContext &ctx, std::string entity_id, const std::string &name)
      : ctx_(ctx), entity_(std::move(entity_id)) {
    const cJSON *state = ctx_.bus.get("ha/" + entity_);
    commands_ = commands_of(entity_, state);
    bool keypad = false;
    for (const Command &c : commands_) keypad |= c.needs_code;

    const int count = static_cast<int>(commands_.size());
    const int commands_h = count * 52 + (count - 1) * 8;
    const int keypad_h = keypad ? 36 + 12 + 4 * KEY_H + 3 * KEY_GAP : 0;
    const int body_h = std::max(commands_h, keypad_h);
    panel_ = ctx_.app.open_sheet(440, 36 + 48 + 16 + body_h, [this] { delete this; });
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel_, 16, 0);

    build_header(name);
    lv_obj_t *body = make_box(panel_, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    if (keypad) build_keypad(body);
    lv_obj_t *column = make_box(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 8, 0);
    lv_obj_set_width(column, keypad ? 180 : lv_pct(100));
    for (const Command &c : commands_) build_command(column, c);

    token_ = ctx_.bus.subscribe("ha/" + entity_, [this](const cJSON *s) { render(s); });
  }

  ~KeypadSheet() {
    ctx_.bus.unsubscribe(token_);
    code_.assign(code_.size(), '\0');  // do not leave the code in memory
  }

 private:
  void build_header(const std::string &name) {
    lv_obj_t *head = make_box(panel_, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 12, 0);
    bubble_ = lv_obj_create(head);
    lv_obj_remove_style_all(bubble_);
    lv_obj_add_style(bubble_, theme::bubble(), 0);
    lv_obj_set_size(bubble_, 48, 48);
    lv_obj_remove_flag(bubble_, LV_OBJ_FLAG_CLICKABLE);
    icon_ = theme::make_icon(bubble_, &dm_icons_24, "shield-off", theme::text());
    lv_obj_center(icon_);
    lv_obj_t *col = make_box(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_t *title = theme::make_label(col, &dm_font_20, theme::text(), name.c_str());
    lv_obj_set_width(title, lv_pct(100));
    theme::single_line(title);
    status_ = theme::make_label(col, &dm_font_14, theme::text2());
  }

  void build_keypad(lv_obj_t *parent) {
    lv_obj_t *pad = make_box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pad, 12, 0);

    // Code display: one dot per typed digit
    dots_ = make_box(pad, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(dots_, 3 * KEY_W + 2 * KEY_GAP, 36);
    lv_obj_set_flex_align(dots_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots_, 10, 0);
    lv_obj_set_style_radius(dots_, 12, 0);
    lv_obj_set_style_bg_color(dots_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dots_, 20, 0);
    hint_ = theme::make_label(dots_, &dm_font_14, theme::text3(), "Code");
    for (lv_obj_t *&dot : dot_objs_) {
      dot = lv_obj_create(dots_);
      lv_obj_remove_style_all(dot);
      lv_obj_set_size(dot, 12, 12);
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(dot, theme::text(), 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
      set_hidden(dot, true);
    }

    lv_obj_t *grid = make_box(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(grid, 3 * KEY_W + 2 * KEY_GAP);
    lv_obj_set_style_pad_row(grid, KEY_GAP, 0);
    lv_obj_set_style_pad_column(grid, KEY_GAP, 0);
    static const char *const KEYS[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0", "<"};
    for (const char *key : KEYS) {
      lv_obj_t *button = make_box(grid, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(button, KEY_W, KEY_H);
      lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_radius(button, 16, 0);
      lv_obj_set_style_bg_color(button, lv_color_white(), 0);
      lv_obj_set_style_bg_opa(button, 30, 0);
      lv_obj_set_style_bg_opa(button, 90, LV_STATE_PRESSED);
      lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
      if (!strcmp(key, "<")) theme::make_icon(button, &dm_icons_24, "backspace-outline", theme::text());
      else theme::make_label(button, &dm_font_20, theme::text(), key);
      lv_obj_set_user_data(button, const_cast<char *>(key));
      lv_obj_add_event_cb(button, on_key, LV_EVENT_CLICKED, this);
    }
  }

  void build_command(lv_obj_t *parent, const Command &command) {
    lv_obj_t *button = make_box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(button, lv_pct(100), 52);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(button, 16, 0);
    lv_obj_set_style_pad_column(button, 10, 0);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, 30, 0);
    lv_obj_set_style_bg_color(button, theme::accent(), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(button, 90, LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    theme::make_icon(button, &dm_icons_24, command.icon, theme::text());
    theme::make_label(button, &dm_font_16, theme::text(), command.label);
    lv_obj_add_event_cb(button, on_command, LV_EVENT_CLICKED, this);
    buttons_.push_back(button);
  }

  void render(const cJSON *state) {
    const SecurityLook look = security_look(entity_, state);
    lv_label_set_text(status_, look.text);
    lv_label_set_text(icon_, theme::icon(look.icon));
    lv_obj_set_style_bg_opa(bubble_, look.active ? 235 : 40, 0);
    lv_obj_set_style_bg_color(bubble_, look.active ? look.color : lv_color_white(), 0);
    lv_obj_set_style_text_color(icon_, look.active ? lv_color_mix(look.color, lv_color_black(), 140) : look.color, 0);
    // The command matching the current state is highlighted
    const std::string s = json::str(state, "s");
    for (size_t i = 0; i < buttons_.size() && i < commands_.size(); i++) {
      const std::string service = commands_[i].service;
      const bool current = (service == "alarm_disarm" && s == "disarmed") || (service == "lock" && s == "locked") ||
                           (service == "unlock" && s == "unlocked") || (service == "open" && s == "open") ||
                           (service.rfind("alarm_arm_", 0) == 0 && s == "armed_" + service.substr(10));
      set_checked(buttons_[i], current);
    }
  }

  void show_code() {
    set_hidden(hint_, !code_.empty());
    for (int i = 0; i < MAX_CODE; i++) set_hidden(dot_objs_[i], i >= static_cast<int>(code_.size()));
  }

  static void on_key(lv_event_t *e) {
    auto *self = static_cast<KeypadSheet *>(lv_event_get_user_data(e));
    const char *key = static_cast<const char *>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
    if (!strcmp(key, "C")) self->code_.clear();
    else if (!strcmp(key, "<")) {
      if (!self->code_.empty()) self->code_.pop_back();
    } else if (static_cast<int>(self->code_.size()) < MAX_CODE) {
      self->code_ += key;
    }
    lv_label_set_text(self->hint_, "Code");
    lv_obj_set_style_text_color(self->hint_, theme::text3(), 0);
    self->show_code();
  }

  static void on_command(lv_event_t *e) {
    auto *self = static_cast<KeypadSheet *>(lv_event_get_user_data(e));
    const lv_obj_t *target = lv_event_get_current_target_obj(e);
    for (size_t i = 0; i < self->buttons_.size(); i++) {
      if (self->buttons_[i] != target) continue;
      const Command &command = self->commands_[i];
      if (command.needs_code && self->code_.empty()) {
        lv_label_set_text(self->hint_, "Tape le code");
        lv_obj_set_style_text_color(self->hint_, theme::crit(), 0);
        return;
      }
      cJSON *data = nullptr;
      if (command.needs_code) {
        data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "code", self->code_.c_str());
      }
      if (self->ctx_.services.ha_call) self->ctx_.services.ha_call(entity_domain(self->entity_), command.service, self->entity_, data);
      else cJSON_Delete(data);
      // A wrong code comes back as a toast from Home Assistant
      self->ctx_.app.close_sheet();
      return;
    }
  }

  WidgetContext &ctx_;
  std::string entity_;
  std::vector<Command> commands_;
  std::string code_;
  int token_ = 0;
  std::vector<lv_obj_t *> buttons_;
  lv_obj_t *panel_ = nullptr, *bubble_ = nullptr, *icon_ = nullptr, *status_ = nullptr;
  lv_obj_t *dots_ = nullptr, *hint_ = nullptr;
  lv_obj_t *dot_objs_[MAX_CODE] = {};
};

/**
 * @description Alarm panel tile: icon, state ("Armée (absent)", "Déclenchée !") and name; the card is
 * colored while armed (orange while arming, red when triggered). A tap opens the keypad popup.
 */
class AlarmTile : public Widget {
 public:
  AlarmTile(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx)
      : ctx_(ctx), spec_(spec), entity_(json::str(spec.props, "entity")) {
    const bool tall = spec.h >= 2;
    root = theme::make_card(parent);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    bubble_ = lv_obj_create(root);
    lv_obj_remove_style_all(bubble_);
    lv_obj_add_style(bubble_, theme::bubble(), 0);
    // 1x1: a smaller bubble, so that it and the state fit the ~57 px inside the card
    const int bubble = tall ? 56 : spec.w >= 2 ? 40 : 34;
    lv_obj_set_size(bubble_, bubble, bubble);
    lv_obj_remove_flag(bubble_, LV_OBJ_FLAG_CLICKABLE);
    icon_ = theme::make_icon(bubble_, tall ? &dm_icons_32 : &dm_icons_24, "shield-off", theme::text());
    lv_obj_center(icon_);
    state_ = theme::make_label(root, spec.w >= 2 ? &dm_font_16 : &dm_font_14, theme::text2());
    name_ = theme::make_label(root, spec.w >= 2 ? &dm_font_20 : &dm_font_16, theme::text());
    theme::single_line(name_);
    place_tile_name(bubble_, name_, spec);
    if (spec.w >= 2) {
      lv_obj_align(state_, LV_ALIGN_TOP_RIGHT, 0, 2);
    } else if (tall) {
      lv_obj_align(state_, LV_ALIGN_BOTTOM_LEFT, 0, -22);
    } else {
      // A 1x1 tile only has room for the bubble and one line: the state, more useful than the name
      // (the popup shows the name)
      lv_obj_set_width(state_, lv_pct(100));
      theme::single_line(state_);
      lv_obj_align(state_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
      set_hidden(name_, true);
    }
    bind_event<AlarmTile, &AlarmTile::on_gesture>(root, LV_EVENT_SHORT_CLICKED, this);
    bind_event<AlarmTile, &AlarmTile::on_gesture>(root, LV_EVENT_LONG_PRESSED, this);
  }

  void start() {
    render(nullptr);
    // Rules first: the bus calls the entity listener right away when it already has a state
    rules_.bind(spec_.props, ctx_.bus, [this] { render(ctx_.bus.get("ha/" + entity_)); });
    ctx_.bus.subscribe("ha/" + entity_, [this](const cJSON *state) { render(state); });
  }

 private:
  void render(const cJSON *state) {
    const Rule *rule = rules_.match(state);
    const std::string name = entity_name(spec_.props, state, entity_);
    SecurityLook look = security_look(entity_, state);
    if (!state) look.text = "…";
    std::string text = rule && !rule->text.empty() ? Rules::format(rule->text, state, name) : look.text;
    const char *icon = rule && !rule->icon.empty() ? rule->icon.c_str() : *json::str(spec_.props, "icon") ? json::str(spec_.props, "icon") : look.icon;
    if (rule && rule->has_color) look.color = lv_color_hex(rule->color);
    if (rule && rule->active >= 0) look.active = rule->active == 1;

    lv_label_set_text(name_, name.c_str());
    lv_label_set_text(state_, text.c_str());
    lv_label_set_text(icon_, theme::icon(icon));
    if (look.active) {
      theme::paint_active(root, look.color);
      lv_obj_add_state(root, LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(bubble_, 235, 0);
      lv_obj_set_style_text_color(icon_, lv_color_mix(look.color, lv_color_black(), 170), 0);
      lv_obj_set_style_text_color(state_, lv_color_white(), 0);
    } else {
      lv_obj_remove_state(root, LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(bubble_, 40, 0);
      lv_obj_set_style_text_color(icon_, look.color, 0);
      lv_obj_set_style_text_color(state_, theme::text2(), 0);
    }
  }

  void on_gesture(lv_event_t *e) {
    const bool hold = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    const cJSON *state = ctx_.bus.get("ha/" + entity_);
    run_tile_action(ctx_, spec_.props, hold ? "hold_action" : "tap_action", "more_info", entity_,
                    entity_name(spec_.props, state, entity_), security_look(entity_, state).icon);
  }

  WidgetContext &ctx_;
  WidgetSpec spec_;
  std::string entity_;
  RuleBinding rules_;
  lv_obj_t *bubble_ = nullptr, *icon_ = nullptr, *state_ = nullptr, *name_ = nullptr;
};

}  // namespace

bool needs_keypad(const std::string &entity_id, const cJSON *state) {
  const std::string domain = entity_domain(entity_id);
  return domain == "alarm_control_panel" || (domain == "lock" && has_code(state));
}

void open_keypad_sheet(WidgetContext &ctx, const std::string &entity_id, const std::string &name) {
  if (!ctx.services.ha_call) return ctx.app.toast("Home Assistant non configuré");
  new KeypadSheet(ctx, entity_id, name);  // owned by its sheet, deleted on close
}

std::unique_ptr<Widget> create_ha_alarm(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  auto tile = std::make_unique<AlarmTile>(parent, spec, ctx);
  tile->start();
  return tile;
}

}  // namespace dm::widgets
