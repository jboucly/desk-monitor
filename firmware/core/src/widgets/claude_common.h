// Helpers shared by the Claude widgets (claude.cpp) and their detail views (claude_detail.cpp).
#pragma once

#include <string>

#include "common.h"

namespace dm::widgets::claude {

/**
 * @description Token count in short French form: "850", "12 k", "1,2 M".
 */
std::string format_tokens(double tokens);

/**
 * @description Estimated cost in US dollars, French style: "3,42 $", "128 $". Empty when unknown.
 */
std::string format_cost(double dollars);

/**
 * @description Seconds elapsed since a Unix time, NaN when the time is missing or the clock is not set.
 */
double seconds_since(double unix_time);

/**
 * @description Clock time of a reset: "14:30" today, "lun. 09:00" another day. Empty when unknown.
 */
std::string format_reset(double unix_time);

/**
 * @description Transparent flex row, full width, sized to its content.
 */
lv_obj_t *make_row(lv_obj_t *parent, int gap);

/**
 * @description Transparent flex column, full width, sized to its content.
 */
lv_obj_t *make_column(lv_obj_t *parent, int gap);

/**
 * @description Card header: icon and title, the rest of the row left to the caller.
 */
lv_obj_t *make_header(lv_obj_t *parent, const char *icon, const char *title, lv_obj_t **icon_out);

/**
 * @description Lets a tap anywhere on a card reach the card: its descendants stop catching clicks.
 */
void pass_clicks(lv_obj_t *card);

/**
 * @description Color of a session state: waiting, working or idle.
 */
lv_color_t state_color(const char *state);

/**
 * @description French text of a session state: "Attend ton accord", "Au travail", "Inactive".
 */
const char *state_text(const char *state);

}  // namespace dm::widgets::claude
