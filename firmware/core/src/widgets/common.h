// Shared pieces of the widget implementations (private to core).
#pragma once

#include <memory>
#include <string>

#include "dm/app.h"
#include "dm/json.h"
#include "dm/theme.h"
#include "dm/widget.h"

namespace dm::widgets {

// ── Factories (registered in registry.cpp) ───────────────────────────────────
std::unique_ptr<Widget> create_clock(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_weather(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_toggle(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_light(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_sensor(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_ha_gauge(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_shortcut(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_pc_metrics(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);
std::unique_ptr<Widget> create_pc_gauge(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);

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

// ── Light popup (light_popup.cpp) ────────────────────────────────────────────

/**
 * @description Opens the light control sheet (on/off, brightness, color wheel, white temperature).
 */
void open_light_popup(WidgetContext &ctx, const std::string &entity_id, const std::string &name, const char *icon);

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
