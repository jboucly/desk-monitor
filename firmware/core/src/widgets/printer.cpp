// 3D printer widget and its full-screen view, fed by dm::PrinterSession (bus keys printer, printer/files and
// printer/thumb/<path>, see protocol/README.md "3D printer"):
// - the card shows the job (thumbnail, name, progress, time left) and the temperatures; a tap opens the view;
// - the view adds the controls (pause, resume, stop, light) and the last printed files, which can be printed
//   again. Stopping and starting ask for a second tap.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

#include "claude_common.h"
#include "dm/printer.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

using claude::format_reset;
using claude::make_column;
using claude::make_row;
using claude::pass_clicks;

// How long a stop or start button waits for its confirming tap
constexpr uint32_t CONFIRM_MS = 4000;

bool running(const char *state) { return !strcmp(state, "printing") || !strcmp(state, "paused"); }

/**
 * @description True while a print starts: heating, homing and bed calibration report "printing" at 0 %, with
 * a meaningless time left.
 */
bool preparing(const cJSON *p) {
  return !strcmp(json::str(p, "state"), "printing") && json::num(p, "progress", 0) <= 0;
}

/**
 * @description True when the printer can start a new print.
 */
bool ready(const char *state) { return !strcmp(state, "idle") || !strcmp(state, "finished") || !strcmp(state, "stopped"); }

lv_color_t state_color(const char *state) {
  if (!strcmp(state, "printing")) return theme::accent();
  if (!strcmp(state, "paused")) return theme::warn();
  if (!strcmp(state, "finished")) return theme::ok();
  if (!strcmp(state, "error")) return theme::crit();
  if (!strcmp(state, "idle") || !strcmp(state, "stopped")) return theme::text2();
  return theme::text3();
}

const char *state_text(const char *state) {
  if (!strcmp(state, "printing")) return "Impression";
  if (!strcmp(state, "paused")) return "En pause";
  if (!strcmp(state, "finished")) return "Terminée";
  if (!strcmp(state, "stopped")) return "Arrêtée";
  if (!strcmp(state, "error")) return "Erreur";
  if (!strcmp(state, "idle")) return "Prête";
  if (!strcmp(state, "off")) return "Non configurée";
  if (!strcmp(state, "offline")) return "Hors ligne";
  return "Connexion…";
}

/**
 * @description "205°", or "58 / 60°" while heating toward a target.
 */
std::string temperature(const cJSON *p, const char *key, const char *target_key) {
  const double value = json::num(p, key, NAN), target = json::num(p, target_key, NAN);
  if (std::isnan(value)) return "—";
  if (std::isnan(target) || target <= 0) return util::format_number(value, 0) + "°";
  return util::format_number(value, 0) + " / " + util::format_number(target, 0) + "°";
}

/**
 * @description Clock time at which the job should end, from the time left. Empty when unknown.
 */
std::string end_time(const cJSON *p) {
  const double left = json::num(p, "remaining", NAN);
  const time_t now = time(nullptr);
  if (std::isnan(left) || left <= 0 || now < 1700000000) return "";
  return format_reset(static_cast<double>(now) + left);
}

/**
 * @description Secondary line of a file: material and slicer estimate, "PLA · 4 h 51 min".
 */
std::string file_details(const cJSON *item) {
  std::string out = json::str(item, "material");
  const double estimate = json::num(item, "estimate", NAN);
  if (!std::isnan(estimate)) out += (out.empty() ? "" : " · ") + util::format_elapsed(estimate);
  return out;
}

/**
 * @description Thumbnail of a printer file in a rounded box, with a printer icon until it arrives.
 */
class Thumbnail {
 public:
  Thumbnail(lv_obj_t *parent, WidgetContext &ctx) : ctx_(ctx) {
    box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, PrinterSession::THUMB_SIZE, PrinterSession::THUMB_SIZE);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(PrinterSession::THUMB_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    icon_ = theme::make_icon(box, &dm_icons_32, "printer-3d", theme::text3());
    lv_obj_center(icon_);
    image_ = lv_image_create(box);
    lv_obj_center(image_);
    lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
  }

  // The LVGL objects still exist here (deleted after the widgets, or asynchronously with a sheet)
  ~Thumbnail() {
    ctx_.bus.unsubscribe(token_);
    lv_image_set_src(image_, nullptr);
    if (buf_) lv_draw_buf_destroy(buf_);
  }

  /**
   * @description Shows the thumbnail of a file (asked to the printer the first time).
   */
  void set_path(const std::string &path) {
    if (path == path_) return;
    path_ = path;
    ctx_.bus.unsubscribe(token_);
    token_ = 0;
    set_hidden(image_, true);
    set_hidden(icon_, false);
    if (path.empty()) return;
    if (ctx_.services.printer_thumbnail) ctx_.services.printer_thumbnail(path);
    token_ = ctx_.bus.subscribe("printer/thumb/" + path, [this](const cJSON *v) { load(v); });
  }

  lv_obj_t *box = nullptr;

 private:
  void load(const cJSON *art) {
    const int w = static_cast<int>(json::num(art, "w")), h = static_cast<int>(json::num(art, "h"));
    std::vector<uint8_t> pixels;
    if (w <= 0 || h <= 0 || w > 128 || h > 128 || !util::base64_decode(json::str(art, "data"), pixels) ||
        pixels.size() != static_cast<size_t>(w * h * 2)) {
      return;
    }
    lv_image_set_src(image_, nullptr);
    if (!buf_ || buf_->header.w != w || buf_->header.h != h) {
      if (buf_) lv_draw_buf_destroy(buf_);
      buf_ = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
      if (!buf_) return;
    }
    for (int y = 0; y < h; y++)
      memcpy(lv_draw_buf_goto_xy(buf_, 0, y), &pixels[static_cast<size_t>(y * w * 2)], static_cast<size_t>(w * 2));
    lv_image_set_src(image_, buf_);
    lv_obj_center(image_);
    set_hidden(image_, false);
    set_hidden(icon_, true);
  }

  WidgetContext &ctx_;
  std::string path_;
  int token_ = 0;
  lv_obj_t *icon_ = nullptr, *image_ = nullptr;
  lv_draw_buf_t *buf_ = nullptr;
};

/**
 * @description Progress bar: a track with a filled part.
 */
struct Bar {
  lv_obj_t *track = nullptr, *fill = nullptr;

  void create(lv_obj_t *parent, int height) {
    track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, lv_pct(100), height);
    lv_obj_set_style_radius(track, height / 2, 0);
    lv_obj_set_style_bg_color(track, theme::track(), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_height(fill, height);
    lv_obj_set_style_radius(fill, height / 2, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
  }

  void set(double percent, lv_color_t color) {
    lv_obj_set_width(fill, lv_pct(static_cast<int32_t>(std::clamp(std::isnan(percent) ? 0.0 : percent, 2.0, 100.0))));
    lv_obj_set_style_bg_color(fill, color, 0);
  }
};

/**
 * @description Small icon followed by a value, for temperatures.
 */
lv_obj_t *make_reading(lv_obj_t *parent, const char *icon, const lv_font_t *font) {
  lv_obj_t *row = make_row(parent, 4);
  lv_obj_set_width(row, LV_SIZE_CONTENT);
  theme::make_icon(row, &dm_icons_18, icon, theme::text3());
  return theme::make_label(row, font, theme::text());
}

// ── Card ─────────────────────────────────────────────────────────────────────

class PrinterCard : public Widget {
 public:
  PrinterCard(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx)
      : ctx_(ctx), compact_(spec.h < 2), wide_(spec.w >= 3) {
    title_ = json::str(spec.props, "name");
    root = theme::make_card(parent);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(
        root, [](lv_event_t *e) { open_printer_detail(static_cast<PrinterCard *>(lv_event_get_user_data(e))->ctx_); },
        LV_EVENT_CLICKED, this);
    if (compact_) build_compact();
    else build_full();
    pass_clicks(root);
    ctx.bus.subscribe("printer", [this](const cJSON *p) { render(p); });
    if (!ctx.bus.get("printer")) render(nullptr);
  }

  // The end time moves with the clock
  void tick() override { render(ctx_.bus.get("printer")); }

 private:
  void build_full() {
    lv_obj_t *top = make_row(root, 10);
    thumb_ = std::make_unique<Thumbnail>(top, ctx_);
    lv_obj_t *col = make_column(top, 0);
    lv_obj_set_width(col, 0);
    lv_obj_set_flex_grow(col, 1);
    state_ = theme::make_label(col, &dm_font_14, theme::text2());
    lv_obj_set_width(state_, lv_pct(100));
    theme::single_line(state_);
    name_ = theme::make_label(col, &dm_font_16, theme::text());
    lv_obj_set_width(name_, lv_pct(100));
    theme::single_line(name_);
    details_ = theme::make_label(col, &dm_font_14, theme::text3());
    lv_obj_set_width(details_, lv_pct(100));
    theme::single_line(details_);

    lv_obj_t *mid = make_row(root, 8);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    big_ = theme::make_label(mid, &dm_font_28, theme::text());
    end_ = theme::make_label(mid, &dm_font_14, theme::text2());
    bar_.create(root, 8);

    lv_obj_t *temps = make_row(root, 14);
    nozzle_ = make_reading(temps, "printer-3d-nozzle", &dm_font_14);
    bed_ = make_reading(temps, "radiator", &dm_font_14);
  }

  void build_compact() {
    lv_obj_t *top = make_row(root, 8);
    theme::make_icon(top, &dm_icons_24, "printer-3d", theme::text2());
    name_ = theme::make_label(top, &dm_font_16, theme::text());
    lv_obj_set_flex_grow(name_, 1);
    theme::single_line(name_);
    big_ = theme::make_label(top, &dm_font_16, theme::text());
    details_ = theme::make_label(root, &dm_font_14, theme::text2());
    lv_obj_set_width(details_, lv_pct(100));
    theme::single_line(details_);
    bar_.create(root, 6);
  }

  void render(const cJSON *p) {
    const char *state = json::str(p, "state", "connecting");
    const bool job = running(state) || !strcmp(state, "finished");
    const double progress = json::num(p, "progress", NAN);
    const lv_color_t color = state_color(state);
    const std::string title = !title_.empty() ? title_ : json::str(p, "model_name", "Imprimante 3D");
    const char *file = json::str(p, "name");

    std::string big;
    if (job && !std::isnan(progress)) big = util::format_number(progress, 0) + " %";
    else big = state_text(state);

    // Time left and end, or what to do when nothing prints
    std::string left;
    if (running(state) && !preparing(p)) {
      const double remaining = json::num(p, "remaining", NAN);
      if (!std::isnan(remaining) && remaining > 0) left = "reste " + util::format_elapsed(remaining);
    }

    if (compact_) {
      lv_label_set_text(name_, *file && job ? file : title.c_str());
      lv_label_set_text(big_, job && !std::isnan(progress) ? big.c_str() : "");
      std::string line = preparing(p) ? "Préparation…" : job ? state_text(state) : big;
      if (!left.empty()) line += " · " + left;
      if (!strcmp(state, "off")) line = "À configurer dans l'éditeur";
      else if (!job && json::get(p, "nozzle")) line += " · " + temperature(p, "nozzle", "nozzle_target");
      lv_label_set_text(details_, line.c_str());
      lv_obj_set_style_text_color(details_, color, 0);
      set_hidden(bar_.track, !job);
      bar_.set(progress, color);
      return;
    }

    std::string state_line = preparing(p) ? "Préparation…" : job ? state_text(state) : title;
    const int layer = static_cast<int>(json::num(p, "layer", -1)), layers = static_cast<int>(json::num(p, "layers", -1));
    if (wide_ && running(state) && layers > 0) state_line += " · couche " + std::to_string(layer) + "/" + std::to_string(layers);
    lv_label_set_text(state_, state_line.c_str());
    lv_obj_set_style_text_color(state_, job ? color : theme::text2(), 0);
    lv_label_set_text(name_, *file ? file : !strcmp(state, "off") ? "Aucune imprimante" : "Aucun fichier");
    std::string details = !strcmp(state, "off") ? "À configurer dans l'éditeur" : left.empty() ? file_details(p) : left;
    lv_label_set_text(details_, details.c_str());
    thumb_->set_path(json::str(p, "file"));

    lv_label_set_text(big_, big.c_str());
    lv_obj_set_style_text_color(big_, job ? theme::text() : color, 0);
    const std::string end = running(state) && !preparing(p) ? end_time(p) : "";
    lv_label_set_text(end_, end.empty() ? "" : ("fin " + end).c_str());
    set_hidden(bar_.track, !job);
    bar_.set(progress, color);
    lv_label_set_text(nozzle_, temperature(p, "nozzle", "nozzle_target").c_str());
    lv_label_set_text(bed_, temperature(p, "bed", "bed_target").c_str());
  }

  WidgetContext &ctx_;
  bool compact_, wide_;
  std::string title_;
  std::unique_ptr<Thumbnail> thumb_;
  lv_obj_t *state_ = nullptr, *name_ = nullptr, *details_ = nullptr, *big_ = nullptr, *end_ = nullptr;
  lv_obj_t *nozzle_ = nullptr, *bed_ = nullptr;
  Bar bar_;
};

// ── Full-screen view ─────────────────────────────────────────────────────────

/**
 * @description Pill button with an icon and a text that can change.
 */
struct Button {
  lv_obj_t *obj = nullptr, *icon = nullptr, *label = nullptr;

  void create(lv_obj_t *parent, const char *glyph, const char *text, int height) {
    obj = make_box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(obj, height);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(obj, 16, 0);
    lv_obj_set_style_pad_column(obj, 6, 0);
    lv_obj_set_style_radius(obj, height / 2, 0);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, 30, 0);
    lv_obj_set_style_bg_opa(obj, 80, LV_STATE_PRESSED);
    lv_obj_set_style_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, 4);
    icon = theme::make_icon(obj, &dm_icons_18, glyph, theme::text());
    label = theme::make_label(obj, &dm_font_14, theme::text(), text);
  }

  void set(const char *glyph, const char *text, lv_color_t background, lv_opa_t opa) {
    lv_label_set_text(icon, theme::icon(glyph));
    lv_label_set_text(label, text);
    lv_obj_set_style_bg_color(obj, background, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
  }

  void enable(bool on) {
    if (on) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
  }
};

class PrinterDetail {
 public:
  explicit PrinterDetail(WidgetContext &ctx) : ctx_(ctx) {
    panel_ = ctx_.app.open_sheet(theme::SCREEN_W, theme::SCREEN_H, [this] { delete this; });
    // Full screen: no rounded panel, no border
    lv_obj_set_style_radius(panel_, 0, 0);
    lv_obj_set_style_border_width(panel_, 0, 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel_, 16, 0);
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel_, 10, 0);
    // A swipe down closes the view, like the close button
    lv_obj_add_event_cb(
        panel_,
        [](lv_event_t *e) {
          if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_BOTTOM) return;
          static_cast<PrinterDetail *>(lv_event_get_user_data(e))->ctx_.app.close_sheet();
        },
        LV_EVENT_GESTURE, this);

    build_header();
    body_ = make_column(panel_, 10);
    lv_obj_set_height(body_, 0);
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_add_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_ACTIVE);
    build_job();
    build_controls();
    build_temperatures();
    files_title_ = theme::make_label(body_, &dm_font_14, theme::text3(), "Derniers fichiers");
    lv_obj_set_style_pad_top(files_title_, 6, 0);
    files_ = make_column(body_, 6);

    tokens_.push_back(ctx_.bus.subscribe("printer", [this](const cJSON *p) { render(p); }));
    tokens_.push_back(ctx_.bus.subscribe("printer/files", [this](const cJSON *f) { render_files(f); }));
    if (!ctx_.bus.get("printer")) render(nullptr);
    confirm_timer_ = lv_timer_create(
        [](lv_timer_t *t) {
          auto *self = static_cast<PrinterDetail *>(lv_timer_get_user_data(t));
          self->end_confirm();
          self->render(self->ctx_.bus.get("printer"));
          self->render_files(self->ctx_.bus.get("printer/files"));
        },
        CONFIRM_MS, this);
    lv_timer_pause(confirm_timer_);
  }

  ~PrinterDetail() {
    lv_async_call_cancel(refresh_files_async, this);
    lv_timer_delete(confirm_timer_);
    for (int token : tokens_) ctx_.bus.unsubscribe(token);
  }

 private:
  void build_header() {
    lv_obj_t *head = make_row(panel_, 10);
    theme::make_icon(head, &dm_icons_24, "printer-3d", theme::accent());
    title_ = theme::make_label(head, &dm_font_20, theme::text(), "Imprimante 3D");
    lv_obj_set_flex_grow(title_, 1);
    theme::single_line(title_);
    lv_obj_t *close = lv_obj_create(head);
    lv_obj_remove_style_all(close);
    lv_obj_add_style(close, theme::bubble(), 0);
    lv_obj_set_size(close, 44, 44);
    lv_obj_set_style_bg_color(close, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, 60, LV_STATE_PRESSED);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close, 8);
    lv_obj_center(theme::make_icon(close, &dm_icons_24, "close", theme::text()));
    lv_obj_add_event_cb(
        close, [](lv_event_t *e) { static_cast<PrinterDetail *>(lv_event_get_user_data(e))->ctx_.app.close_sheet(); },
        LV_EVENT_CLICKED, this);
  }

  void build_job() {
    lv_obj_t *top = make_row(body_, 12);
    thumb_ = std::make_unique<Thumbnail>(top, ctx_);
    lv_obj_t *col = make_column(top, 0);
    lv_obj_set_width(col, 0);
    lv_obj_set_flex_grow(col, 1);
    state_ = theme::make_label(col, &dm_font_14, theme::text2());
    name_ = theme::make_label(col, &dm_font_20, theme::text());
    lv_obj_set_width(name_, lv_pct(100));
    theme::single_line(name_);
    details_ = theme::make_label(col, &dm_font_14, theme::text3());
    lv_obj_set_width(details_, lv_pct(100));
    theme::single_line(details_);

    progress_row_ = make_row(body_, 8);
    lv_obj_set_flex_align(progress_row_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    percent_ = theme::make_label(progress_row_, &dm_font_28, theme::text());
    layer_ = theme::make_label(progress_row_, &dm_font_14, theme::text2());
    bar_.create(body_, 10);

    times_ = make_row(body_, 8);
    for (int i = 0; i < 3; i++) {
      lv_obj_t *cell = make_column(times_, 0);
      lv_obj_set_width(cell, 0);
      lv_obj_set_flex_grow(cell, 1);
      times_value_[i] = theme::make_label(cell, &dm_font_20, theme::text());
      theme::make_label(cell, &dm_font_14, theme::text3(), i == 0 ? "Écoulé" : i == 1 ? "Restant" : "Fin prévue");
    }
  }

  void build_controls() {
    lv_obj_t *row = make_row(body_, 8);
    lv_obj_set_style_pad_top(row, 4, 0);
    pause_.create(row, "pause", "Pause", 44);
    stop_.create(row, "stop", "Arrêter", 44);
    light_.create(row, "lightbulb-outline", "Lumière", 44);
    lv_obj_add_event_cb(
        pause_.obj, [](lv_event_t *e) { static_cast<PrinterDetail *>(lv_event_get_user_data(e))->on_pause(); },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        stop_.obj, [](lv_event_t *e) { static_cast<PrinterDetail *>(lv_event_get_user_data(e))->on_stop(); },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        light_.obj, [](lv_event_t *e) { static_cast<PrinterDetail *>(lv_event_get_user_data(e))->on_light(); },
        LV_EVENT_CLICKED, this);
  }

  void build_temperatures() {
    lv_obj_t *row = make_row(body_, 8);
    lv_obj_set_style_pad_top(row, 4, 0);
    for (int i = 0; i < 2; i++) {
      lv_obj_t *cell = make_row(row, 8);
      lv_obj_set_width(cell, 0);
      lv_obj_set_flex_grow(cell, 1);
      theme::make_icon(cell, &dm_icons_24, i == 0 ? "printer-3d-nozzle" : "radiator", theme::text3());
      lv_obj_t *col = make_column(cell, 0);
      lv_obj_set_width(col, LV_SIZE_CONTENT);
      temps_[i] = theme::make_label(col, &dm_font_20, theme::text());
      theme::make_label(col, &dm_font_14, theme::text3(), i == 0 ? "Buse" : "Plateau");
    }
  }

  // ── State ──────────────────────────────────────────────────────────────────

  void render(const cJSON *p) {
    const char *state = json::str(p, "state", "connecting");
    state_name_ = state;
    const bool job = running(state) || !strcmp(state, "finished");
    const lv_color_t color = state_color(state);
    lv_label_set_text(title_, json::str(p, "model_name", "Imprimante 3D"));

    std::string state_line = preparing(p) ? "Préparation (chauffe, calibration)" : state_text(state);
    const char *error = json::str(p, "error");
    if (*error) state_line += std::string(" · ") + error;
    lv_label_set_text(state_, state_line.c_str());
    lv_obj_set_style_text_color(state_, color, 0);
    const char *file = json::str(p, "name");
    lv_label_set_text(name_, *file ? file : !strcmp(state, "off") ? "Aucune imprimante" : "Aucun fichier");
    const std::string details = !strcmp(state, "off") ? "Choisis le modèle et l'adresse dans l'éditeur, onglet Réglages" : file_details(p);
    lv_label_set_text(details_, details.c_str());
    thumb_->set_path(json::str(p, "file"));

    // Progress and times, only for a job
    const double progress = json::num(p, "progress", NAN);
    set_hidden(progress_row_, !job);
    set_hidden(bar_.track, !job);
    set_hidden(times_, !job);
    lv_label_set_text(percent_, std::isnan(progress) ? "—" : (util::format_number(progress, 0) + " %").c_str());
    const int layer = static_cast<int>(json::num(p, "layer", -1)), layers = static_cast<int>(json::num(p, "layers", -1));
    lv_label_set_text(layer_, layers > 0 ? ("couche " + std::to_string(layer) + " / " + std::to_string(layers)).c_str() : "");
    bar_.set(progress, color);
    const double elapsed = json::num(p, "elapsed", NAN), remaining = json::num(p, "remaining", NAN);
    lv_label_set_text(times_value_[0], util::format_elapsed(elapsed).c_str());
    const bool timed = running(state) && !preparing(p);
    lv_label_set_text(times_value_[1], timed ? util::format_elapsed(remaining).c_str() : "—");
    const std::string end = timed ? end_time(p) : "";
    lv_label_set_text(times_value_[2], end.empty() ? "—" : end.c_str());

    // Controls follow the state and what the driver supports
    const cJSON *caps = json::get(p, "caps");
    const auto can = [caps](const char *cap) {
      const cJSON *item = nullptr;
      cJSON_ArrayForEach(item, caps) if (cJSON_IsString(item) && !strcmp(item->valuestring, cap)) return true;
      return false;
    };
    const bool paused = !strcmp(state, "paused");
    pause_.set(paused ? "play" : "pause", paused ? "Reprendre" : "Pause", lv_color_white(), 30);
    pause_.enable(running(state) && can(paused ? "resume" : "pause"));
    const bool confirm_stop = confirm_stop_;
    stop_.set("stop", confirm_stop ? "Confirmer l'arrêt" : "Arrêter", confirm_stop ? theme::crit() : lv_color_white(),
              confirm_stop ? LV_OPA_COVER : 30);
    stop_.enable(running(state) && can("stop"));
    const bool light_on = json::boolean(p, "light");
    light_.set(light_on ? "lightbulb" : "lightbulb-outline", "Lumière", light_on ? theme::warm() : lv_color_white(),
               light_on ? 90 : 30);
    light_.enable(json::get(p, "light") && can("light") && strcmp(state, "offline") != 0);
    can_start_ = ready(state) && can("start");

    lv_label_set_text(temps_[0], temperature(p, "nozzle", "nozzle_target").c_str());
    lv_label_set_text(temps_[1], temperature(p, "bed", "bed_target").c_str());

    // The start buttons depend on the state too
    if (can_start_ != rows_can_start_) render_files(ctx_.bus.get("printer/files"));
  }

  // ── Files ──────────────────────────────────────────────────────────────────

  void render_files(const cJSON *f) {
    rows_can_start_ = can_start_;
    // The thumbnails detach their images first: their objects go with the rows
    row_thumbs_.clear();
    lv_obj_clean(files_);
    paths_.clear();
    const cJSON *recent = json::get(f, "recent");
    const int count = cJSON_IsArray(recent) ? cJSON_GetArraySize(recent) : 0;
    set_hidden(files_title_, count == 0);
    for (int i = 0; i < count; i++) {
      const cJSON *item = cJSON_GetArrayItem(recent, i);
      const std::string path = json::str(item, "path");
      lv_obj_t *row = make_row(files_, 10);

      auto thumb = std::make_unique<Thumbnail>(row, ctx_);
      thumb->set_path(path);
      row_thumbs_.push_back(std::move(thumb));

      lv_obj_t *col = make_column(row, 0);
      lv_obj_set_width(col, 0);
      lv_obj_set_flex_grow(col, 1);
      lv_obj_t *name = theme::make_label(col, &dm_font_16, theme::text(), json::str(item, "name"));
      lv_obj_set_width(name, lv_pct(100));
      theme::single_line(name);
      // When it ran and how it ended
      std::string when = format_reset(json::num(item, "when", NAN));
      const double duration = json::num(item, "duration", NAN);
      if (!std::isnan(duration) && duration > 0) when += (when.empty() ? "" : " · ") + util::format_elapsed(duration);
      const std::string material = json::str(item, "material");
      if (!material.empty()) when = material + (when.empty() ? "" : " · " + when);
      lv_obj_t *meta = make_row(col, 4);
      const char *result = json::str(item, "result");
      if (*result) {
        const bool done = !strcmp(result, "done");
        theme::make_icon(meta, &dm_icons_18, done ? "check-circle" : "close-circle", done ? theme::ok() : theme::crit());
      }
      lv_obj_t *meta_text = theme::make_label(meta, &dm_font_14, theme::text3(), when.c_str());
      lv_obj_set_flex_grow(meta_text, 1);
      theme::single_line(meta_text);

      const bool exists = json::boolean(item, "exists", true);
      const bool confirm = confirming_ == path;
      Button start;
      start.create(row, "play", !exists ? "Supprimé" : confirm ? "Plateau libre ?" : "Lancer", 40);
      if (confirm) start.set("play", "Plateau libre ?", theme::accent(), LV_OPA_COVER);
      start.enable(exists && can_start_);
      paths_.push_back(path);
      lv_obj_set_user_data(start.obj, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
      lv_obj_add_event_cb(
          start.obj,
          [](lv_event_t *e) {
            auto *self = static_cast<PrinterDetail *>(lv_event_get_user_data(e));
            const auto index = reinterpret_cast<intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_current_target(e))));
            self->on_start(static_cast<size_t>(index));
          },
          LV_EVENT_CLICKED, this);
    }
  }

  // ── Actions ────────────────────────────────────────────────────────────────

  bool send(const char *command, const std::string &arg = "") {
    if (ctx_.services.printer_command && ctx_.services.printer_command(command, arg)) return true;
    ctx_.app.toast("Imprimante non connectée");
    return false;
  }

  /**
   * @description First tap of a stop or a start: the button asks to tap again for CONFIRM_MS.
   */
  void ask_confirm(const std::string &path, bool stop) {
    confirming_ = path;
    confirm_stop_ = stop;
    lv_timer_reset(confirm_timer_);
    lv_timer_resume(confirm_timer_);
  }

  void end_confirm() {
    confirming_.clear();
    confirm_stop_ = false;
    lv_timer_pause(confirm_timer_);
  }

  void on_pause() { send(state_name_ == "paused" ? "resume" : "pause"); }

  void on_stop() {
    if (!confirm_stop_) {
      ask_confirm("", true);
      render(ctx_.bus.get("printer"));
      return;
    }
    end_confirm();
    // The printer finishes its bed calibration before it stops
    const bool calibrating = preparing(ctx_.bus.get("printer"));
    if (send("stop")) ctx_.app.toast(calibrating ? "Arrêt après la calibration en cours" : "Arrêt demandé");
    render(ctx_.bus.get("printer"));
  }

  void on_light() { send("light", json::boolean(ctx_.bus.get("printer"), "light") ? "off" : "on"); }

  void on_start(size_t index) {
    if (index >= paths_.size()) return;
    const std::string path = paths_[index];
    if (confirming_ != path) {
      ask_confirm(path, false);
    } else {
      end_confirm();
      if (send("start", path)) ctx_.app.toast("Impression lancée");
    }
    // Rebuilt after the event: the row holding the button is deleted
    lv_async_call(refresh_files_async, this);
  }

  static void refresh_files_async(void *self) {
    auto *view = static_cast<PrinterDetail *>(self);
    view->render_files(view->ctx_.bus.get("printer/files"));
  }

  WidgetContext &ctx_;
  lv_obj_t *panel_ = nullptr, *body_ = nullptr, *title_ = nullptr;
  std::unique_ptr<Thumbnail> thumb_;
  lv_obj_t *state_ = nullptr, *name_ = nullptr, *details_ = nullptr;
  lv_obj_t *progress_row_ = nullptr, *percent_ = nullptr, *layer_ = nullptr, *times_ = nullptr;
  lv_obj_t *times_value_[3] = {};
  lv_obj_t *temps_[2] = {};
  Bar bar_;
  Button pause_, stop_, light_;
  lv_obj_t *files_title_ = nullptr, *files_ = nullptr;
  std::vector<std::unique_ptr<Thumbnail>> row_thumbs_;
  std::vector<std::string> paths_;
  std::vector<int> tokens_;
  std::string state_name_;
  // What waits for a second tap: the stop button, or the start button of a file
  bool confirm_stop_ = false;
  std::string confirming_;
  lv_timer_t *confirm_timer_ = nullptr;
  bool can_start_ = false, rows_can_start_ = false;
};

}  // namespace

void open_printer_detail(WidgetContext &ctx) { new PrinterDetail(ctx); }

std::unique_ptr<Widget> create_printer(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<PrinterCard>(parent, spec, ctx);
}

}  // namespace dm::widgets
