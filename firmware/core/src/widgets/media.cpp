// media: now playing with cover, progress and controls. Source: the PC companion (any app playing on
// the computer), or, when the PC plays nothing, an optional Home Assistant media_player.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>

#include "common.h"
#include "dm/cover.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

constexpr int COVER_MAX = 160;  // the companion sends 160x160 covers

/**
 * @description What the widget shows, whatever the source.
 */
struct NowPlaying {
  std::string title, artist, app;
  bool playing = false;
  double duration = NAN;
  double elapsed = NAN;     // at `since`
  uint32_t since = 0;       // lv_tick when `elapsed` was read
  lv_color_t color;
  bool has_color = false;
};

class Media : public Widget {
 public:
  Media(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ctx_(ctx) {
    entity_ = json::str(spec.props, "entity");
    wide_ = spec.w >= 3;
    tall_ = spec.h >= 2;

    root = theme::make_card(parent);
    lv_obj_set_flex_flow(root, wide_ || !tall_ ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(root, 14, 0);
    if (wide_ || !tall_) lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(root, 8, 0);

    // Cover: a square that fills the card height (wide or one-row cards) or a small one on top
    const int inner_h = theme::span_height(spec.h) - 2 * theme::CARD_PAD;
    const int inner_w = theme::span_width(spec.w) - 2 * theme::CARD_PAD;
    cover_size_ = wide_ || !tall_ ? std::min({inner_h, COVER_MAX, inner_w / 2}) : std::min(72, inner_h / 2);
    lv_obj_t *top = root;
    if (!wide_ && tall_) {
      // Narrow card: cover + titles on one row, progress and controls below
      top = lv_obj_create(root);
      lv_obj_add_style(top, theme::bare(), 0);
      lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(top, 10, 0);
    }
    cover_ = lv_obj_create(top);
    lv_obj_add_style(cover_, theme::bare(), 0);
    lv_obj_set_size(cover_, cover_size_, cover_size_);
    lv_obj_set_style_radius(cover_, cover_size_ > 100 ? 14 : 10, 0);
    lv_obj_set_style_clip_corner(cover_, true, 0);
    lv_obj_set_style_bg_color(cover_, theme::track(), 0);
    lv_obj_set_style_bg_opa(cover_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cover_, LV_OBJ_FLAG_SCROLLABLE);
    placeholder_ = theme::make_icon(cover_, cover_size_ > 100 ? &dm_icons_48 : &dm_icons_24, "music-note",
                                    theme::text3());
    lv_obj_center(placeholder_);
    image_ = lv_image_create(cover_);
    lv_obj_center(image_);
    lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);

    // Texts
    lv_obj_t *info = lv_obj_create(wide_ || !tall_ ? root : top);
    lv_obj_add_style(info, theme::bare(), 0);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, wide_ ? lv_pct(100) : LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 2, 0);
    app_ = text(info, &dm_font_14, theme::text3());
    title_ = text(info, wide_ ? &dm_font_20 : &dm_font_16, theme::text());
    artist_ = text(info, &dm_font_14, theme::text2());
    if (!tall_) lv_obj_add_flag(app_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bottom = wide_ ? info : root;
    if (wide_) {
      // Pushes the progress and the controls to the bottom of the column
      lv_obj_t *spacer = lv_obj_create(info);
      lv_obj_add_style(spacer, theme::bare(), 0);
      lv_obj_set_width(spacer, 1);
      lv_obj_set_flex_grow(spacer, 1);
    }

    if (tall_) {
      bar_ = lv_bar_create(bottom);
      lv_obj_set_size(bar_, lv_pct(100), 6);
      lv_bar_set_range(bar_, 0, 1000);
      lv_obj_set_style_radius(bar_, 3, LV_PART_MAIN);
      lv_obj_set_style_radius(bar_, 3, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(bar_, theme::track(), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, LV_PART_INDICATOR);

      lv_obj_t *times = row(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN);
      elapsed_ = theme::make_label(times, &dm_font_14, theme::text3(), "");
      duration_ = theme::make_label(times, &dm_font_14, theme::text3(), "");
    }

    // Controls
    lv_obj_t *controls = tall_ ? row(bottom, LV_FLEX_ALIGN_CENTER) : root;
    if (tall_) lv_obj_set_style_pad_column(controls, wide_ ? 28 : 22, 0);
    if (tall_) prev_ = button(controls, "skip-previous", false);
    play_ = button(controls, "play", true);
    if (tall_) next_ = button(controls, "skip-next", false);

    ctx.bus.subscribe("pc/media", [this](const cJSON *v) {
      pc_ = json::own(cJSON_Duplicate(v, true));
      refresh();
    });
    ctx.bus.subscribe("pc/media_art", [this](const cJSON *v) { load_cover(v, pc_cover_); });
    if (!entity_.empty()) {
      ctx.bus.subscribe("ha/" + entity_, [this](const cJSON *v) {
        ha_ = json::own(cJSON_Duplicate(v, true));
        fetch_ha_cover();
        refresh();
      });
      ctx.bus.subscribe("ha_art/" + entity_, [this](const cJSON *v) { load_cover(v, ha_cover_); });
    }
    refresh();
  }

  ~Media() override {
    // The image is deleted after the widgets: detach the buffers first
    lv_image_set_src(image_, nullptr);
    if (pc_cover_) lv_draw_buf_destroy(pc_cover_);
    if (ha_cover_) lv_draw_buf_destroy(ha_cover_);
  }

  void tick() override {
    if (now_.playing) show_progress();
  }

 private:
  enum class Source { None, Pc, Ha };

  lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *label = theme::make_label(parent, font, color);
    lv_obj_set_width(label, lv_pct(100));
    theme::single_line(label);
    return label;
  }

  lv_obj_t *row(lv_obj_t *parent, lv_flex_align_t align) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_add_style(r, theme::bare(), 0);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
  }

  /**
   * @description Round control button; `main` is the bigger play/pause one, filled with the accent.
   */
  lv_obj_t *button(lv_obj_t *parent, const char *icon, bool main) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_add_style(b, theme::bare(), 0);
    const int size = main ? (tall_ ? 48 : 40) : 40;
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, main ? theme::accent() : theme::track(), 0);
    lv_obj_set_style_bg_opa(b, main ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *glyph = theme::make_icon(b, main ? &dm_icons_32 : &dm_icons_24, icon,
                                       main ? lv_color_hex(0x0B0D13) : theme::text());
    lv_obj_center(glyph);
    lv_obj_add_event_cb(
        b,
        [](lv_event_t *e) {
          auto *self = static_cast<Media *>(lv_event_get_user_data(e));
          lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
          self->command(target == self->play_ ? "toggle" : target == self->next_ ? "next" : "prev");
        },
        LV_EVENT_CLICKED, this);
    return b;
  }

  /**
   * @description PC first when it plays something, else the Home Assistant player.
   */
  Source pick(NowPlaying &out) {
    out = NowPlaying{};
    const cJSON *pc = pc_.get();
    if (*json::str(pc, "title")) {
      out.title = json::str(pc, "title");
      out.artist = json::str(pc, "artist");
      out.app = json::str(pc, "app");
      out.playing = json::boolean(pc, "playing");
      out.duration = json::num(pc, "duration", NAN);
      out.elapsed = json::num(pc, "elapsed", NAN);
      out.since = lv_tick_get();
      unsigned long rgb = 0;
      const char *color = json::str(pc, "color");
      if (*color == '#' && sscanf(color + 1, "%06lx", &rgb) == 1) {
        out.color = lv_color_hex(static_cast<uint32_t>(rgb));
        out.has_color = true;
      }
      return Source::Pc;
    }
    const cJSON *ha = ha_.get();
    const std::string state = json::str(ha, "s");
    const cJSON *a = json::get(ha, "a");
    if ((state == "playing" || state == "paused") && *json::str(a, "media_title")) {
      out.title = json::str(a, "media_title");
      out.artist = json::str(a, "media_artist", json::str(a, "media_album_name"));
      out.app = json::str(a, "app_name", json::str(a, "friendly_name"));
      out.playing = state == "playing";
      out.duration = json::num(a, "media_duration", NAN);
      out.elapsed = json::num(a, "media_position", NAN);
      out.since = lv_tick_get();
      // HA reports the position at media_position_updated_at: catch up to now
      const double updated = util::parse_iso8601(json::str(a, "media_position_updated_at"));
      const double now = static_cast<double>(time(nullptr));
      if (out.playing && updated > 0 && now > 1.6e9 && now > updated) out.elapsed += now - updated;
      return Source::Ha;
    }
    return Source::None;
  }

  void refresh() {
    source_ = pick(now_);
    const bool idle = source_ == Source::None;
    lv_label_set_text(title_, idle ? "Rien en lecture" : now_.title.c_str());
    lv_label_set_text(artist_, idle ? (entity_.empty() ? "Lance de la musique sur l'ordinateur" : "") : now_.artist.c_str());
    lv_label_set_text(app_, now_.app.c_str());
    lv_obj_set_style_text_color(title_, idle ? theme::text2() : theme::text(), 0);

    // Each source has its own cover
    lv_draw_buf_t *cover = source_ == Source::Pc ? pc_cover_ : source_ == Source::Ha ? ha_cover_ : nullptr;
    const bool show_cover = cover != nullptr;
    if (lv_image_get_src(image_) != cover) {
      lv_image_set_src(image_, nullptr);
      if (cover) {
        lv_image_set_src(image_, cover);
        // Covers are 160 px; smaller cards scale them down (only redrawn when the cover changes)
        lv_image_set_scale(image_, static_cast<uint32_t>(LV_SCALE_NONE * cover_size_ / std::max<int>(cover->header.w, cover->header.h)));
        lv_obj_center(image_);
      }
    }
    if (show_cover) lv_obj_remove_flag(image_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    if (show_cover) lv_obj_add_flag(placeholder_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(placeholder_, LV_OBJ_FLAG_HIDDEN);

    const lv_color_t tint = now_.has_color ? now_.color : theme::accent();
    lv_obj_set_style_bg_color(play_, tint, 0);
    lv_obj_t *glyph = lv_obj_get_child(play_, 0);
    lv_label_set_text(glyph, theme::icon(now_.playing ? "pause" : "play"));
    // Dark glyph on light tints, white on dark ones
    lv_obj_set_style_text_color(glyph, lv_color_brightness(tint) > 150 ? lv_color_hex(0x0B0D13) : lv_color_white(), 0);
    if (bar_) lv_obj_set_style_bg_color(bar_, tint, LV_PART_INDICATOR);
    for (lv_obj_t *b : {prev_, play_, next_}) {
      if (!b) continue;
      if (idle) lv_obj_add_state(b, LV_STATE_DISABLED);
      else lv_obj_remove_state(b, LV_STATE_DISABLED);
      lv_obj_set_style_opa(lv_obj_get_child(b, 0), idle ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
    show_progress();
  }

  void show_progress() {
    if (!bar_) return;
    double elapsed = now_.elapsed;
    if (now_.playing && !std::isnan(elapsed)) elapsed += lv_tick_elaps(now_.since) / 1000.0;
    const bool known = !std::isnan(now_.duration) && now_.duration > 0;
    if (known) elapsed = std::min(elapsed, now_.duration);
    lv_bar_set_value(bar_, known && !std::isnan(elapsed) ? static_cast<int32_t>(elapsed / now_.duration * 1000) : 0,
                     LV_ANIM_OFF);
    lv_label_set_text(elapsed_, source_ == Source::None ? "" : util::format_duration(elapsed).c_str());
    lv_label_set_text(duration_, known ? util::format_duration(now_.duration).c_str() : "");
  }

  /**
   * @description Replaces a cover with an RGB565 image ({"w", "h", "data": base64}), from the PC
   * companion or decoded from Home Assistant's entity_picture.
   */
  void load_cover(const cJSON *art, lv_draw_buf_t *&target) {
    const int w = static_cast<int>(json::num(art, "w")), h = static_cast<int>(json::num(art, "h"));
    std::vector<uint8_t> pixels;
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || !util::base64_decode(json::str(art, "data"), pixels) ||
        pixels.size() != static_cast<size_t>(w * h * 2)) {
      return;
    }
    if (target && lv_image_get_src(image_) == target) lv_image_set_src(image_, nullptr);
    if (!target || target->header.w != w || target->header.h != h) {
      if (target) lv_draw_buf_destroy(target);
      target = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
      if (!target) return;
    }
    for (int y = 0; y < h; y++)
      memcpy(lv_draw_buf_goto_xy(target, 0, y), &pixels[static_cast<size_t>(y * w * 2)], static_cast<size_t>(w * 2));
    refresh();
  }

  /**
   * @description Downloads and decodes the Home Assistant player's picture when it changes. The work
   * runs on a worker thread and ends with a "ha_art/<entity>" bus post: the widget may be gone by then.
   */
  void fetch_ha_cover() {
    const char *picture = json::str(json::get(ha_.get(), "a"), "entity_picture");
    if (!*picture || ha_picture_ == picture || !ctx_.services.ha_fetch) return;
    ha_picture_ = picture;
    Bus &bus = ctx_.bus;
    const std::string key = "ha_art/" + entity_, id = picture;
    const int size = cover_size_ > 100 ? COVER_MAX : 96;
    ctx_.services.ha_fetch(picture, [&bus, key, id, size](std::string body) {
      std::vector<uint8_t> pixels;
      if (!body.empty() && decode_jpeg_cover(body, size, pixels)) bus.post(key, cover_json(id, size, pixels));
    });
  }

  void command(const char *cmd) {
    if (source_ == Source::Pc) {
      if (!ctx_.services.pc_media || !ctx_.services.pc_media(cmd)) ctx_.app.toast("PC non connecté");
    } else if (source_ == Source::Ha && ctx_.services.ha_call) {
      const char *service = !strcmp(cmd, "toggle") ? "media_play_pause"
                            : !strcmp(cmd, "next") ? "media_next_track"
                                                   : "media_previous_track";
      ctx_.services.ha_call("media_player", service, entity_, nullptr);
    }
  }

  WidgetContext &ctx_;
  std::string entity_;
  bool wide_ = false, tall_ = false;
  int cover_size_ = 0;
  json::Ptr pc_, ha_;
  NowPlaying now_;
  Source source_ = Source::None;
  lv_draw_buf_t *pc_cover_ = nullptr, *ha_cover_ = nullptr;
  std::string ha_picture_;
  lv_obj_t *cover_ = nullptr, *placeholder_ = nullptr, *image_ = nullptr;
  lv_obj_t *app_ = nullptr, *title_ = nullptr, *artist_ = nullptr;
  lv_obj_t *bar_ = nullptr, *elapsed_ = nullptr, *duration_ = nullptr;
  lv_obj_t *prev_ = nullptr, *play_ = nullptr, *next_ = nullptr;
};

}  // namespace

std::unique_ptr<Widget> create_media(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<Media>(parent, spec, ctx);
}

}  // namespace dm::widgets
