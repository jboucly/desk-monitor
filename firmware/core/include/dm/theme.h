// Glass theme: palette, shared LVGL styles, fonts, icons and the generated wallpaper.
//
// LVGL cannot blur what is behind an object, so the "frosted glass" look is faked:
// a soft, already-blurred wallpaper (generated at boot from the accent colors) and
// translucent cards with a light top-to-bottom sheen and a thin bright border.
#pragma once

#include <string>

#include "dm/layout.h"
#include "lvgl.h"

LV_FONT_DECLARE(dm_font_14);
LV_FONT_DECLARE(dm_font_16);
LV_FONT_DECLARE(dm_font_20);
LV_FONT_DECLARE(dm_font_28);
LV_FONT_DECLARE(dm_font_40);
LV_FONT_DECLARE(dm_font_72);
LV_FONT_DECLARE(dm_icons_18);
LV_FONT_DECLARE(dm_icons_24);
LV_FONT_DECLARE(dm_icons_32);
LV_FONT_DECLARE(dm_icons_48);
LV_FONT_DECLARE(dm_icons_72);

namespace dm::theme {

// Screen geometry shared by the app shell and the widgets
constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 480;
constexpr int HEADER_H = 44;
constexpr int FOOTER_H = 18;
constexpr int GRID_PAD = 12;
constexpr int GRID_GAP = 10;
constexpr int RADIUS = 22;
constexpr int CARD_PAD = 12;

/**
 * @description Pixel width of a widget spanning `cols` grid columns (padding included).
 */
constexpr int span_width(int cols) {
  return ((SCREEN_W - 2 * GRID_PAD - 3 * GRID_GAP) * cols) / 4 + (cols - 1) * GRID_GAP;
}

/**
 * @description Pixel height of a widget spanning `rows` grid rows (padding included).
 */
constexpr int span_height(int rows) {
  return ((SCREEN_H - HEADER_H - FOOTER_H - 2 * GRID_PAD - 3 * GRID_GAP) * rows) / 4 + (rows - 1) * GRID_GAP;
}

/**
 * @description Builds the styles and the wallpaper for a theme. Call again when the theme changes.
 */
void apply(const ThemeSpec &spec);

/**
 * @description Wallpaper image (RGB565, full screen).
 */
const void *wallpaper();

/**
 * @description Visual style of the cards, chosen by the layout (theme.style).
 * - Glass: translucent cards blended with the wallpaper. The most expensive to draw.
 * - Frozen: opaque cards painted with the color the glass would have at their resting place.
 * - Flat: no wallpaper, opaque dark cards and a plain dark background.
 */
enum class Style { Glass, Frozen, Flat };
Style style();

/**
 * @description Screen background color (under the wallpaper, or alone in the flat style).
 */
lv_color_t background();
lv_color_t background2();

/**
 * @description Paints the frozen cards found under `root` once the layout is computed.
 * Does nothing in the other styles.
 */
void finish_cards(lv_obj_t *root);

/**
 * @description Sets the LV_STATE_CHECKED look of a card for an active entity of color `c`.
 */
void paint_active(lv_obj_t *card, lv_color_t c);

// ── Palette ──────────────────────────────────────────────────────────────────
lv_color_t accent();
lv_color_t accent2();
inline lv_color_t text() { return lv_color_hex(0xF4F6FF); }    // primary text
inline lv_color_t text2() { return lv_color_hex(0xB4BCD6); }   // secondary text
inline lv_color_t text3() { return lv_color_hex(0x7D86A3); }   // hints, disabled
inline lv_color_t ok() { return lv_color_hex(0x4ADE80); }
inline lv_color_t warn() { return lv_color_hex(0xFBBF24); }
inline lv_color_t crit() { return lv_color_hex(0xF87171); }
inline lv_color_t warm() { return lv_color_hex(0xFFC078); }    // light "on" without color info
inline lv_color_t panel() { return lv_color_hex(0x121829); }   // popup panel
inline lv_color_t track() { return lv_color_hex(0x2C3244); }   // empty part of bars

/**
 * @description Opacity of popup panels and toasts: slightly see-through in the glass style, opaque otherwise.
 */
lv_opa_t panel_opa();

/**
 * @description Opacity of the black backdrop behind a popup. Only the glass style dims the screen:
 * blending a full-screen layer is the most expensive thing to draw.
 */
lv_opa_t backdrop_opa();

/**
 * @description Color of a usage value: normal below `warn_at`, orange below `crit_at`, red above.
 */
lv_color_t level(float value, float warn_at, float crit_at, lv_color_t normal);

// ── Styles ───────────────────────────────────────────────────────────────────
lv_style_t *card();          // translucent glass card
lv_style_t *card_pressed();  // pressed state of a card
lv_style_t *bubble();        // round icon background
lv_style_t *bare();          // no background, no border, no padding

/**
 * @description Creates an empty glass card.
 */
lv_obj_t *make_card(lv_obj_t *parent);

/**
 * @description Creates a label with a font and a color.
 */
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text = "");

/**
 * @description Limits a label to one line, ending with "…" when the text is too long.
 * The label must have a width (e.g. lv_pct(100) or a flex grow).
 */
void single_line(lv_obj_t *label);

/**
 * @description Creates an icon label from an MDI name (see protocol/icons.json).
 */
lv_obj_t *make_icon(lv_obj_t *parent, const lv_font_t *font, const char *name, lv_color_t color);

/**
 * @description UTF-8 glyph of an MDI icon. Unknown names give the "alert-circle-outline" glyph.
 */
const char *icon(const char *name);

/**
 * @description Same as icon(), but returns null for unknown names.
 */
const char *find_icon(const char *name);

}  // namespace dm::theme
