// Actions of the tiles and shortcuts (tap_action, hold_action, shortcut action) and their confirmation sheet.
#include <cstring>

#include "common.h"

namespace dm::widgets {

namespace {

constexpr int CONFIRM_W = 380;
constexpr int CONFIRM_H = 180;

/**
 * @description Opens the "Confirmer ?" sheet and runs `run` on "Confirmer" (defined below).
 */
void confirm_then(WidgetContext &ctx, const std::string &question, std::function<void()> run);

/**
 * @description Sends an action. `confirmed` is true when a confirmation was already asked, so that an
 * unlock does not ask twice.
 */
void execute(WidgetContext &ctx, const cJSON *action, const std::string &entity, const std::string &name, const char *icon,
             bool confirmed) {
  const char *kind = json::str(action, "kind");
  if (!strcmp(kind, "none")) return;
  if (!strcmp(kind, "toggle") || !strcmp(kind, "more_info")) {
    // Acts on the action's own entity_id when it names one, else on the tile's entity
    const std::string target = *json::str(action, "entity_id") ? json::str(action, "entity_id") : entity;
    const cJSON *state = ctx.bus.get("ha/" + target);
    if (target.empty() || is_unavailable(state)) return;
    const std::string label = target == entity ? name : entity_name(nullptr, state, target);
    if (!strcmp(kind, "toggle")) {
      // Alarm panels and locks with a code are driven from the keypad
      if (needs_keypad(target, state)) return open_keypad_sheet(ctx, target, label);
      // Unlocking a door asks first, unless the action already did
      if (entity_domain(target) == "lock" && !strcmp(json::str(state, "s"), "locked") && !confirmed) {
        return confirm_then(ctx, "Déverrouiller " + label + " ?", [&ctx, target] {
          ctx.services.ha_call("lock", "unlock", target, nullptr);
        });
      }
      return toggle_entity(ctx.services, target, state);
    }
    if (!open_more_info(ctx, target, label, icon))
      ctx.app.toast("Pas de réglages pour cette entité");
    return;
  }
  if (!strcmp(kind, "ha")) {
    if (!ctx.services.ha_call) return ctx.app.toast("Home Assistant non configuré");
    const cJSON *data = json::get(action, "data");
    ctx.services.ha_call(json::str(action, "domain"), json::str(action, "service"), json::str(action, "entity_id"),
                         data ? cJSON_Duplicate(data, true) : nullptr);
    return;
  }
  if (!ctx.services.pc_action || !ctx.services.pc_action(action)) ctx.app.toast("PC non connecté");
}

/**
 * @description "Confirmer ?" sheet; runs the action only on "Confirmer". Deleted when the sheet closes.
 */
class ConfirmSheet {
 public:
  ConfirmSheet(WidgetContext &ctx, const std::string &question, std::function<void()> run) : ctx_(ctx), run_(std::move(run)) {
    lv_obj_t *panel = ctx_.app.open_sheet(CONFIRM_W, CONFIRM_H, [this] { delete this; });
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *text = theme::make_label(panel, &dm_font_20, theme::text(), question.c_str());
    lv_obj_set_width(text, lv_pct(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *row = make_box(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_style_pad_column(row, 12, 0);
    make_button(row, "Annuler", false);
    make_button(row, "Confirmer", true);
  }

 private:
  void make_button(lv_obj_t *parent, const char *label, bool confirm) {
    lv_obj_t *button = make_box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(button, 52);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_bg_color(button, confirm ? theme::accent() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, confirm ? LV_OPA_COVER : 36, 0);
    lv_obj_set_style_bg_opa(button, confirm ? 200 : 80, LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    theme::make_label(button, &dm_font_16, confirm ? lv_color_hex(0x0B0F1A) : theme::text(), label);
    lv_obj_add_event_cb(button, confirm ? on_confirm : on_cancel, LV_EVENT_CLICKED, this);
  }

  static void on_cancel(lv_event_t *e) { static_cast<ConfirmSheet *>(lv_event_get_user_data(e))->ctx_.app.close_sheet(); }

  static void on_confirm(lv_event_t *e) {
    auto *self = static_cast<ConfirmSheet *>(lv_event_get_user_data(e));
    // close_sheet() deletes this object: keep what is needed first
    std::function<void()> run = std::move(self->run_);
    self->ctx_.app.close_sheet();
    run();
  }

  WidgetContext &ctx_;
  std::function<void()> run_;
};

void confirm_then(WidgetContext &ctx, const std::string &question, std::function<void()> run) {
  if (!ctx.services.ha_call) return ctx.app.toast("Home Assistant non configuré");
  new ConfirmSheet(ctx, question, std::move(run));  // owned by its sheet
}

}  // namespace

void run_action(WidgetContext &ctx, const cJSON *action, const std::string &entity, const std::string &name, const char *icon) {
  const cJSON *confirm = json::get(action, "confirm");
  const bool ask = cJSON_IsTrue(confirm) || (cJSON_IsString(confirm) && *confirm->valuestring);
  if (!ask) return execute(ctx, action, entity, name, icon, false);

  // The layout (and the action in it) may be replaced while the sheet is open: keep copies
  json::Ptr copy = json::own(cJSON_Duplicate(action, true));
  const std::string question = cJSON_IsString(confirm) ? confirm->valuestring : name.empty() ? "Confirmer ?" : name + " : confirmer ?";
  const std::string icon_copy = icon ? icon : "";
  new ConfirmSheet(ctx, question, [&ctx, copy, entity, name, icon_copy] {  // owned by its sheet
    execute(ctx, copy.get(), entity, name, icon_copy.empty() ? nullptr : icon_copy.c_str(), true);
  });
}

void run_tile_action(WidgetContext &ctx, const cJSON *props, const char *prop, const char *fallback,
                     const std::string &entity, const std::string &name, const char *icon) {
  if (const cJSON *action = json::get(props, prop); cJSON_IsObject(action)) return run_action(ctx, action, entity, name, icon);
  cJSON *action = cJSON_CreateObject();
  cJSON_AddStringToObject(action, "kind", fallback);
  execute(ctx, action, entity, name, icon, false);
  cJSON_Delete(action);
}

bool has_tile_action(const cJSON *props, const char *prop) { return cJSON_IsObject(json::get(props, prop)); }

}  // namespace dm::widgets
