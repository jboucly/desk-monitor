#include "dm/theme.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dm::theme {

namespace {

struct IconEntry {
  const char *name;
  const char *glyph;
};

const IconEntry ICONS[] = {
#include "generated/icons.inc"
};

/**
 * @description A soft colored light spot of the wallpaper. Positions and radius are fractions of the screen.
 */
struct Blob {
  float x, y, radius, strength;
  uint32_t color;  // 0 = accent, 1 = accent2, otherwise 0xRRGGBB
};

struct Wallpaper {
  const char *name;
  uint32_t base;
  Blob blobs[3];
};

const Wallpaper WALLPAPERS[] = {
    {"aurora", 0x070A14, {{0.10f, 0.05f, 0.75f, 0.55f, 0}, {1.00f, 0.50f, 0.70f, 0.45f, 1}, {0.25f, 1.05f, 0.65f, 0.40f, 0x2DD4BF}}},
    {"nebula", 0x0A0614, {{0.85f, 0.05f, 0.70f, 0.55f, 0xE879F9}, {0.05f, 0.55f, 0.70f, 0.45f, 1}, {0.70f, 1.05f, 0.65f, 0.40f, 0}}},
    {"ocean", 0x030E18, {{0.15f, 0.00f, 0.75f, 0.50f, 0x22D3EE}, {0.95f, 0.60f, 0.70f, 0.45f, 0x3B82F6}, {0.20f, 1.05f, 0.60f, 0.35f, 0x14B8A6}}},
    {"sunset", 0x120709, {{0.00f, 0.10f, 0.75f, 0.50f, 0xFB923C}, {1.00f, 0.35f, 0.70f, 0.45f, 0xF472B6}, {0.50f, 1.10f, 0.70f, 0.40f, 1}}},
    {"midnight", 0x05070D, {{0.20f, 0.00f, 0.80f, 0.28f, 0}, {0.95f, 1.00f, 0.80f, 0.22f, 1}, {0.50f, 0.50f, 0.10f, 0.00f, 0}}},
};

// 4x4 ordered dithering matrix, hides the banding of RGB565 gradients
const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

lv_color_t g_accent = lv_color_hex(0x7C9CFF);
lv_color_t g_accent2 = lv_color_hex(0xC084FC);
lv_draw_buf_t *g_wallpaper = nullptr;
std::string g_wallpaper_key;

lv_style_t s_card, s_card_pressed, s_bubble, s_bare;
bool g_styles_ready = false;
// Marks the objects made by make_card(), found again by finish_cards()
constexpr lv_obj_flag_t CARD_FLAG = LV_OBJ_FLAG_USER_1;
Style g_style = Style::Glass;

// Flat style palette
constexpr uint32_t FLAT_BG = 0x0B0D13;
constexpr uint32_t FLAT_BG2 = 0x151926;
constexpr uint32_t FLAT_CARD = 0x1A1E2B;
constexpr uint32_t FLAT_CARD_PRESSED = 0x272C3C;
constexpr uint32_t FLAT_BORDER = 0x262B3A;
// Glass sheen: white over the wallpaper, top and bottom of a card (pressed: brighter)
constexpr lv_opa_t SHEEN_TOP = 34, SHEEN_BOTTOM = 14, SHEEN_TOP_PRESSED = 70, SHEEN_BOTTOM_PRESSED = 40;
// Frozen style: one plain sheen, the average of the glass gradient
constexpr lv_opa_t SHEEN_FROZEN = 26, SHEEN_FROZEN_PRESSED = 58;

/**
 * @description Renders the wallpaper into an RGB565 buffer: dark base + additive soft blobs + vignette, dithered.
 */
void render_wallpaper(const Wallpaper &wp) {
  if (!g_wallpaper) g_wallpaper = lv_draw_buf_create(SCREEN_W, SCREEN_H, LV_COLOR_FORMAT_RGB565, 0);
  if (!g_wallpaper) return;

  struct ResolvedBlob {
    float cx, cy, inv_r2, r, g, b;
  } blobs[3];
  for (int i = 0; i < 3; i++) {
    const Blob &b = wp.blobs[i];
    lv_color_t c = b.color == 0 ? g_accent : b.color == 1 ? g_accent2 : lv_color_hex(b.color);
    const float radius = b.radius * SCREEN_W;
    blobs[i] = {b.x * SCREEN_W, b.y * SCREEN_H, 1.0f / (radius * radius), c.red * b.strength, c.green * b.strength,
                c.blue * b.strength};
  }
  const float base_r = (wp.base >> 16) & 0xFF, base_g = (wp.base >> 8) & 0xFF, base_b = wp.base & 0xFF;

  for (int y = 0; y < SCREEN_H; y++) {
    auto *row = reinterpret_cast<uint16_t *>(lv_draw_buf_goto_xy(g_wallpaper, 0, y));
    for (int x = 0; x < SCREEN_W; x++) {
      float r = base_r, g = base_g, b = base_b;
      for (const auto &blob : blobs) {
        const float dx = x - blob.cx, dy = y - blob.cy;
        const float t = (dx * dx + dy * dy) * blob.inv_r2;
        if (t >= 1.0f) continue;
        // Smooth (1 - t)^2 falloff, t being the squared normalized distance
        const float k = (1.0f - t) * (1.0f - t);
        r += blob.r * k;
        g += blob.g * k;
        b += blob.b * k;
      }
      // Vignette: darken the corners a little
      const float vx = (x - SCREEN_W / 2.0f) / SCREEN_W, vy = (y - SCREEN_H / 2.0f) / SCREEN_H;
      const float v = 1.0f - 0.9f * (vx * vx + vy * vy);
      const float d = BAYER[y & 3][x & 3] / 16.0f;
      const int r5 = std::clamp(static_cast<int>(r * v / 8.0f + d), 0, 31);
      const int g6 = std::clamp(static_cast<int>(g * v / 4.0f + d), 0, 63);
      const int b5 = std::clamp(static_cast<int>(b * v / 8.0f + d), 0, 31);
      row[x] = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
    }
  }
}

void init_styles() {
  if (g_styles_ready) {
    lv_style_reset(&s_card);
    lv_style_reset(&s_card_pressed);
    lv_style_reset(&s_bubble);
    lv_style_reset(&s_bare);
  }
  g_styles_ready = true;

  lv_style_init(&s_card);
  lv_style_init(&s_card_pressed);
  lv_style_set_radius(&s_card, RADIUS);
  lv_style_set_bg_opa(&s_card, LV_OPA_COVER);
  lv_style_set_border_width(&s_card, 1);
  lv_style_set_pad_all(&s_card, 12);
  lv_style_set_text_color(&s_card, text());
  lv_style_set_text_font(&s_card, &dm_font_16);
  switch (g_style) {
    case Style::Glass:
      // White sheen fading from top to bottom, blended with the wallpaper, thin bright border
      lv_style_set_bg_color(&s_card, lv_color_white());
      lv_style_set_bg_grad_color(&s_card, lv_color_white());
      lv_style_set_bg_grad_dir(&s_card, LV_GRAD_DIR_VER);
      lv_style_set_bg_main_opa(&s_card, SHEEN_TOP);
      lv_style_set_bg_grad_opa(&s_card, SHEEN_BOTTOM);
      lv_style_set_border_color(&s_card, lv_color_white());
      lv_style_set_border_opa(&s_card, 46);
      lv_style_set_bg_main_opa(&s_card_pressed, SHEEN_TOP_PRESSED);
      lv_style_set_bg_grad_opa(&s_card_pressed, SHEEN_BOTTOM_PRESSED);
      lv_style_set_border_opa(&s_card_pressed, 90);
      break;
    case Style::Frozen:
      // Opaque plain color, set per card by finish_cards()
      lv_style_set_bg_color(&s_card, lv_color_hex(FLAT_CARD));
      lv_style_set_border_color(&s_card, lv_color_white());
      lv_style_set_border_opa(&s_card, 46);
      lv_style_set_border_opa(&s_card_pressed, 90);
      break;
    case Style::Flat:
      lv_style_set_bg_color(&s_card, lv_color_hex(FLAT_CARD));
      lv_style_set_border_color(&s_card, lv_color_hex(FLAT_BORDER));
      lv_style_set_bg_color(&s_card_pressed, lv_color_hex(FLAT_CARD_PRESSED));
      break;
  }

  lv_style_init(&s_bubble);
  lv_style_set_radius(&s_bubble, LV_RADIUS_CIRCLE);
  lv_style_set_bg_opa(&s_bubble, 40);
  lv_style_set_bg_color(&s_bubble, lv_color_white());
  lv_style_set_border_width(&s_bubble, 0);
  lv_style_set_pad_all(&s_bubble, 0);

  lv_style_init(&s_bare);
  lv_style_set_bg_opa(&s_bare, LV_OPA_TRANSP);
  lv_style_set_border_width(&s_bare, 0);
  lv_style_set_pad_all(&s_bare, 0);
  lv_style_set_radius(&s_bare, 0);
  lv_style_set_outline_width(&s_bare, 0);
  lv_style_set_shadow_width(&s_bare, 0);
}

}  // namespace

void apply(const ThemeSpec &spec) {
  g_style = spec.style == "flat" ? Style::Flat : spec.style == "frozen" ? Style::Frozen : Style::Glass;
  g_accent = lv_color_hex(spec.accent);
  g_accent2 = lv_color_hex(spec.accent2);
  init_styles();

  // The wallpaper is only regenerated when its inputs change (it takes a few hundred ms on the ESP32)
  char key[64];
  snprintf(key, sizeof key, "%s/%06lX/%06lX", spec.wallpaper.c_str(), static_cast<unsigned long>(spec.accent),
           static_cast<unsigned long>(spec.accent2));
  if (g_wallpaper && g_wallpaper_key == key) return;
  g_wallpaper_key = key;
  const Wallpaper *wp = &WALLPAPERS[0];
  for (const auto &candidate : WALLPAPERS)
    if (spec.wallpaper == candidate.name) wp = &candidate;
  render_wallpaper(*wp);
}

const void *wallpaper() { return g_wallpaper; }

Style style() { return g_style; }

lv_opa_t panel_opa() { return g_style == Style::Glass ? 240 : LV_OPA_COVER; }
lv_opa_t backdrop_opa() { return g_style == Style::Glass ? LV_OPA_60 : LV_OPA_TRANSP; }

lv_color_t background() { return g_style == Style::Flat ? lv_color_hex(FLAT_BG) : lv_color_black(); }
lv_color_t background2() { return g_style == Style::Flat ? lv_color_hex(FLAT_BG2) : lv_color_black(); }

namespace {

/**
 * @description Average wallpaper color over an area (screen coordinates, clamped), sampled every 4 pixels.
 */
lv_color_t wallpaper_area_color(const lv_area_t &area) {
  if (!g_wallpaper) return lv_color_black();
  const int x1 = std::clamp<int>(area.x1, 0, SCREEN_W - 1), x2 = std::clamp<int>(area.x2, x1, SCREEN_W - 1);
  const int y1 = std::clamp<int>(area.y1, 0, SCREEN_H - 1), y2 = std::clamp<int>(area.y2, y1, SCREEN_H - 1);
  uint32_t r = 0, g = 0, b = 0, n = 0;
  for (int y = y1; y <= y2; y += 4) {
    const auto *row = reinterpret_cast<const uint16_t *>(lv_draw_buf_goto_xy(g_wallpaper, 0, y));
    for (int x = x1; x <= x2; x += 4, n++) {
      r += (row[x] >> 11) & 0x1F;
      g += (row[x] >> 5) & 0x3F;
      b += row[x] & 0x1F;
    }
  }
  return lv_color_make(static_cast<uint8_t>(r * 255 / (31 * n)), static_cast<uint8_t>(g * 255 / (63 * n)),
                       static_cast<uint8_t>(b * 255 / (31 * n)));
}

/**
 * @description Paints a frozen card with the glass color of the wallpaper at its resting place.
 * Cards inside a tileview page move with it, so their resting place is relative to the page.
 * A plain color, not a gradient: RGB565 gradients this subtle show bands without dithering.
 */
void freeze_card(lv_obj_t *card) {
  lv_area_t area;
  lv_obj_get_coords(card, &area);
  for (lv_obj_t *obj = card; obj; obj = lv_obj_get_parent(obj)) {
    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (parent && lv_obj_check_type(parent, &lv_tileview_class)) {
      lv_area_t tile, view;
      lv_obj_get_coords(obj, &tile);
      lv_obj_get_coords(parent, &view);
      lv_area_move(&area, view.x1 - tile.x1, view.y1 - tile.y1);
      break;
    }
  }
  const lv_color_t under = wallpaper_area_color(area);
  lv_obj_set_style_bg_color(card, lv_color_mix(lv_color_white(), under, SHEEN_FROZEN), 0);
  lv_obj_set_style_bg_color(card, lv_color_mix(lv_color_white(), under, SHEEN_FROZEN_PRESSED), LV_STATE_PRESSED);
}

void freeze_tree(lv_obj_t *obj) {
  if (lv_obj_has_flag(obj, CARD_FLAG)) freeze_card(obj);
  const uint32_t count = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < count; i++) freeze_tree(lv_obj_get_child(obj, static_cast<int32_t>(i)));
}

}  // namespace

void finish_cards(lv_obj_t *root) {
  if (g_style != Style::Frozen || !root) return;
  lv_obj_update_layout(root);
  freeze_tree(root);
}

void paint_active(lv_obj_t *card, lv_color_t c) {
  lv_obj_set_style_bg_color(card, c, LV_STATE_CHECKED);
  lv_obj_set_style_bg_grad_color(card, lv_color_mix(c, lv_color_black(), 150), LV_STATE_CHECKED);
  lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, LV_STATE_CHECKED);
  // Glass: tinted glass over the wallpaper; other styles: opaque, cheap to draw
  const bool glass = g_style == Style::Glass;
  lv_obj_set_style_bg_main_opa(card, glass ? 200 : LV_OPA_COVER, LV_STATE_CHECKED);
  lv_obj_set_style_bg_grad_opa(card, glass ? 150 : LV_OPA_COVER, LV_STATE_CHECKED);
  lv_obj_set_style_border_opa(card, 110, LV_STATE_CHECKED);
}

lv_color_t accent() { return g_accent; }
lv_color_t accent2() { return g_accent2; }

lv_color_t level(float value, float warn_at, float crit_at, lv_color_t normal) {
  if (std::isnan(value)) return text3();
  if (value >= crit_at) return crit();
  if (value >= warn_at) return warn();
  return normal;
}

lv_style_t *card() { return &s_card; }
lv_style_t *card_pressed() { return &s_card_pressed; }
lv_style_t *bubble() { return &s_bubble; }
lv_style_t *bare() { return &s_bare; }

lv_obj_t *make_card(lv_obj_t *parent) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_add_style(obj, card(), 0);
  lv_obj_add_style(obj, card_pressed(), LV_STATE_PRESSED);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(obj, CARD_FLAG);
  return obj;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_label_set_text(label, text);
  return label;
}

void single_line(lv_obj_t *label) {
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
}

lv_obj_t *make_icon(lv_obj_t *parent, const lv_font_t *font, const char *name, lv_color_t color) {
  return make_label(parent, font, color, icon(name));
}

const char *find_icon(const char *name) {
  if (!name) return nullptr;
  if (!strncmp(name, "mdi:", 4)) name += 4;
  for (const auto &entry : ICONS)
    if (!strcmp(entry.name, name)) return entry.glyph;
  return nullptr;
}

const char *icon(const char *name) {
  const char *glyph = find_icon(name);
  return glyph ? glyph : find_icon("alert-circle-outline");
}

}  // namespace dm::theme
