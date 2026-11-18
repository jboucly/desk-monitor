// Shared pieces of the widget implementations (private to core).
#pragma once

#include <memory>
#include <string>

#include "dm/app.h"
#include "dm/bus.h"
#include "dm/json.h"
#include "dm/rules.h"
#include "dm/theme.h"
#include "dm/widget.h"

namespace dm::widgets {

// ── Factories (registered in registry.cpp) ───────────────────────────────────
std::unique_ptr<Widget> create_clock(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_weather(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_toggle(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_light(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_sensor(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_climate(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_gauge(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_shortcut(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_pc_metrics(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_pc_gauge(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_media(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_claude_sessions(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_claude_usage(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_claude_limits(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);

// ── Home Assistant helpers ───────────────────────────────────────────────────

/**
 * @description Domain part of an entity id ("light" for "light.bureau").
 */
std::string entity_domain(const std::string &entity_id);

/**
 * @description Icon used when the layout gives none, chosen from the entity domain.
 */
const char *default_entity_icon(const std::string &entity_id);

/**
 * @description Display name: the layout's "name", else HA's friendly_name, else the entity id.
 */
std::string entity_name(const cJSON *props, const cJSON *state, const std::string &entity_id);

/**
 * @description True when the entity has no usable state (missing, "unavailable" or "unknown").
 */
bool is_unavailable(const cJSON *state);

/**
 * @description True when the entity counts as "on" (on, open, playing, unlocked…).
 */
bool is_active(const cJSON *state);

/**
 * @description Calls the service that toggles an entity of any common domain.
 */
void toggle_entity(Services &services, const std::string &entity_id, const cJSON *state);

/**
 * @description Color of a light from its attributes (rgb_color, color_temp_kelvin), warm white otherwise.
 */
lv_color_t light_color(const cJSON *state);

/**
 * @description The `rules` prop of a tile, bound to the bus: it evaluates the rules against the tile's
 * state and the current states of the other entities they name, and reports changes of those.
 */
class RuleBinding {
 public:
  /**
   * @description Parses the rules (already validated by the layout) and subscribes to the other entities.
   * @param changed Called when one of the other entities changes (also right away when it has a value).
   */
  void bind(const cJSON *props, Bus &bus, std::function<void()> changed);

  /**
   * @description First matching rule for the tile's state, or null (also when there is no state yet).
   */
  const Rule *match(const cJSON *state) const;

 private:
  Rules rules_;
  Bus *bus_ = nullptr;
};

// ── Light popup (light_popup.cpp) ────────────────────────────────────────────

/**
 * @description Opens the light control sheet (on/off, brightness, color wheel, white temperature).
 */
void open_light_popup(WidgetContext &ctx, const std::string &entity_id, const std::string &name, const char *icon);

// ── Climate popup (climate.cpp) ──────────────────────────────────────────────

/**
 * @description Opens the thermostat sheet of a climate or water_heater entity (target, mode, preset, fan).
 */
void open_climate_popup(WidgetContext &ctx, const std::string &entity_id, const std::string &name, const char *icon);

// ── Claude detail views (claude_detail.cpp) ──────────────────────────────────

/**
 * @description Opens a full-screen Claude detail view ("session", "usage" or "limits") and asks the
 * PC companion for its data. The view deletes itself when its sheet closes.
 */
void open_claude_detail(WidgetContext &ctx, const std::string &view, const std::string &id);

// ── LVGL helpers ─────────────────────────────────────────────────────────────

/**
 * @description Registers a member function as an LVGL event handler, passing `self` as user data.
 */
template <typename T, void (T::*Method)(lv_event_t *)>
void bind_event(lv_obj_t *obj, lv_event_code_t code, T *self) {
  lv_obj_add_event_cb(
      obj, [](lv_event_t *e) { (static_cast<T *>(lv_event_get_user_data(e))->*Method)(e); }, code, self);
}

}  // namespace dm::widgets
